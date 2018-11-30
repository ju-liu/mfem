// Copyright (c) 2010, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-443211. All Rights
// reserved. See file COPYRIGHT for details.
//
// This file is part of the MFEM library. For more information and source code
// availability see http://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License (as published by the Free
// Software Foundation) version 2.1 dated February 1999.

#include "../general/error.hpp"
#include "../general/okina.hpp"
#include "kernels/mm.hpp"
#include "custub.hpp"

namespace mfem
{

// *****************************************************************************
static size_t xs_shift = 0;
static bool xs_shifted = false;
#define MFEM_SIGSEGV_FOR_STACK __builtin_trap()

// *****************************************************************************
static inline void *xsShift(const void *adrs)
{
   if (!xs_shifted) { return (void*) adrs; }
   return ((size_t*) adrs) - xs_shift;
}

// *****************************************************************************
void mm::Setup(void)
{
   assert(!mng);
   // Create our mapping h_adrs => (size, h_adrs, d_adrs)
   mng = new mm_t();
   // Initialize the CUDA device to be ready to allocate memory
   config::Get().Setup();
   // Shift address accesses to trig SIGSEGV
   if ((xs_shifted=getenv("XS"))) { xs_shift = 1ull << 48; }
}

// *****************************************************************************
void *mm::Range(const void *adrs)
{
   const auto search = mng->find(adrs);
   const bool known = search != mng->end();
   assert(not known);
   for(mm_t::iterator address = mng->begin(); address != mng->end(); address++) {
      void *h_adrs = address->second.h_adrs;
      if (h_adrs > adrs) continue;
      const size_t bytes = address->second.bytes;
      const size_t *end = (size_t*)h_adrs + bytes;
      if (adrs <= end) return h_adrs;
   }
   return NULL;
}

// *****************************************************************************
bool mm::Known(const void *adrs, const bool insert_if_in_range)
{
   const auto search = mng->find(adrs);
   const bool known = search != mng->end();
   if (known) return true;
   if (not insert_if_in_range) return false;
   // Now inserting this adrs if it is in range
   const void *base = Range(adrs);
   if (not base) return false;
   mm2dev_t &mm2dev = mng->operator[](base);
   const size_t bytes = mm2dev.bytes;
   assert(0 < bytes);
   assert(base < adrs);
   const size_t offset = (size_t*) adrs - (size_t*) base;
   dbg("[Known] Insert %p < %p", base, adrs);
   Insert(adrs,bytes-offset,1,__FILE__,__LINE__);
   return true;
}

// *****************************************************************************
// * Add an address only on the host
// * Warning:
// *   - size can be 0: from mfem::GroupTopology::Create
// *****************************************************************************
void* mm::Insert(const void *adrs,
                 const size_t size, const size_t size_of_T,
                 const char *file, const int line,
                 const bool ranged)
{
   dbg("[Insert] %s:%d",file,line);
   if (!mm::Get().mng) { mm::Get().Setup(); }
   size_t *h_adrs = ((size_t *) adrs) + xs_shift;
   const bool known = Known(h_adrs);
   if (size==0) {
      //return h_adrs;
      //MFEM_SIGSEGV_FOR_STACK;
   }
   if (known) {
      dbg("[Insert] Known %p",h_adrs);
      mm2dev_t &mm2dev = mng->operator[](h_adrs);
      if (not mm2dev.ranged){
         MFEM_SIGSEGV_FOR_STACK;
         MFEM_ASSERT(false, "[ERROR] Trying to add already present address!");
      }else{
         MFEM_ASSERT(false, "[ERROR] Trying to add already RANGED address!");
      }
   }
   mm2dev_t &mm2dev = mng->operator[](h_adrs);
   mm2dev.host = true;
   mm2dev.bytes = size*size_of_T;
   dbg("[Insert] Add %p, bytes: %ld",h_adrs,mm2dev.bytes);
   mm2dev.h_adrs = h_adrs;
   mm2dev.d_adrs = NULL;
   mm2dev.ranged = ranged;
   return mm2dev.h_adrs;
}

// *****************************************************************************
// * Remove the address from the map
// *****************************************************************************
void *mm::Erase(const void *adrs)
{
   const bool known = Known(adrs);
   if (not known) { MFEM_SIGSEGV_FOR_STACK; }
   MFEM_ASSERT(known, "[ERROR] Trying to remove an unknown address!");
   mng->erase(adrs);
   return xsShift(adrs);
}

// *****************************************************************************
// * Get an address from host or device
// *****************************************************************************
void* mm::Adrs(const void *adrs)
{
   const bool cuda = config::Get().Cuda();
   const bool insert_if_in_range = true;
   const bool known = Known(adrs, insert_if_in_range);
   if (not known) { MFEM_SIGSEGV_FOR_STACK; }
   MFEM_ASSERT(known, "[ERROR] Trying to convert unknown address!");
   mm2dev_t &mm2dev = mng->operator[](adrs);
   // Just return asked known host address if not in CUDA mode
   if (mm2dev.host and not cuda) { return xsShift(mm2dev.h_adrs); }
   // If it hasn't been seen, alloc it in the device
   if (not mm2dev.d_adrs)
   {
      const bool nvcc = config::nvcc();
      if (not nvcc)
      {
         mfem_error("[ERROR] Trying to run without CUDA support!");
      }
      const size_t bytes = mm2dev.bytes;
      if (bytes>0) { okMemAlloc(&mm2dev.d_adrs, bytes); }
      void *stream = config::Get().Stream();
      okMemcpyHtoDAsync(mm2dev.d_adrs, mm2dev.h_adrs, bytes, stream);
      mm2dev.host = false; // Now this address is GPU born
   }
   return mm2dev.d_adrs;
}

// *****************************************************************************
void mm::Push(const void *adrs)
{
   MFEM_ASSERT(Known(adrs), "[ERROR] Trying to push an unknown address!");
   const mm2dev_t &mm2dev = mng->operator[](adrs);
   if (mm2dev.host) { return; }
   okMemcpyHtoD(mm2dev.d_adrs, mm2dev.h_adrs, mm2dev.bytes);
}

// *****************************************************************************
void mm::Pull(const void *adrs)
{
   MFEM_ASSERT(Known(adrs), "[ERROR] Trying to pull an unknown address!");
   const mm2dev_t &mm2dev = mng->operator[](adrs);
   if (mm2dev.host) { return; }
   okMemcpyDtoH(mm2dev.h_adrs, mm2dev.d_adrs, mm2dev.bytes);
}

// *****************************************************************************
void* mm::memcpy(void *dest, const void *src, size_t bytes)
{
   return mm::D2D(dest, src, bytes, false);
}

// ******************************************************************************
void* mm::H2D(void *dest, const void *src, size_t bytes, const bool async)
{
   if (bytes==0) { return dest; }
   const bool cuda = config::Get().Cuda();
   if (not cuda) { return std::memcpy(dest, src, bytes); }
   return mfem::kH2D(dest, src, bytes, async);
}

// *****************************************************************************
void* mm::D2H(void *dest, const void *src, size_t bytes, const bool async)
{
   if (bytes==0) { return dest; }
   const bool cuda = config::Get().Cuda();
   if (not cuda) { return std::memcpy(dest, src, bytes); }
   return mfem::kD2H(dest, src, bytes, async);
}

// *****************************************************************************
void* mm::D2D(void *dest, const void *src, size_t bytes, const bool async)
{
   if (bytes==0) { return dest; }
   const bool cuda = config::Get().Cuda();
   if (not cuda) { return std::memcpy(dest, src, bytes); }
   return mfem::kD2D(dest, src, bytes, async);
}

} // namespace mfem
