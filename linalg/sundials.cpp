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

#include "sundials.hpp"

#ifdef MFEM_USE_SUNDIALS

#include "solvers.hpp"
#ifdef MFEM_USE_MPI
#include "hypre.hpp"
#endif

#include <sunlinsol/sunlinsol_dense.h> /* access to dense SUNLinearSolver      */
#include <sundials/sundials_config.h>
#include <sundials/sundials_matrix.h>
#include <sundials/sundials_linearsolver.h>
#include <nvector/nvector_serial.h>

#include <cvodes/cvodes.h>
#include <sunlinsol/sunlinsol_spgmr.h>

#define GET_CONTENT(X) ( X->content )

using namespace std;

namespace mfem
{
  // ---------------------------------------------------------------------------
  // SUNMatrix interface functions
  // ---------------------------------------------------------------------------

  // Access the wrapped object in the SUNMatrix
  static inline SundialsLinearSolver *GetObj(SUNMatrix A)
  {
    return (SundialsLinearSolver *)(A->content);
  }

  // Return the matrix ID
  static SUNMatrix_ID MatGetID(SUNMatrix A)
  {
    return(SUNMATRIX_CUSTOM);
  }

  static void MatDestroy(SUNMatrix A)
  {
    if (A->content) { A->content = NULL; }
    if (A->ops) { free(A->ops); A->ops = NULL; }
    free(A); A = NULL;
    return;
  }

  // ---------------------------------------------------------------------------
  // SUNLinearSolver interface functions
  // ---------------------------------------------------------------------------

  // Access wrapped object in the SUNLinearSolver
  static inline SundialsLinearSolver *GetObj(SUNLinearSolver LS)
  {
    return (SundialsLinearSolver *)(LS->content);
  }

  // Return the linear solver type
  static SUNLinearSolver_Type LSGetType(SUNLinearSolver LS)
  {
    return(SUNLINEARSOLVER_MATRIX_ITERATIVE);
  }

  // Initialize the linear solver
  static int LSInit(SUNLinearSolver LS)
  {
    return(GetObj(LS)->Init());
  }

  // Setup the linear solver
  static int LSSetup(SUNLinearSolver LS, SUNMatrix A)
  {
    return(GetObj(LS)->Setup());
  }

  // Solve the linear system A x = b
  static int LSSolve(SUNLinearSolver LS, SUNMatrix A, N_Vector x, N_Vector b,
                     realtype tol)
  {
    Vector mfem_x(x);
    const Vector mfem_b(b);
    return(GetObj(LS)->Solve(mfem_x, mfem_b));
  }

  static int LSFree(SUNLinearSolver LS)
  {
    if (LS->content) { LS->content = NULL; }
    if (LS->ops) { free(LS->ops); LS->ops = NULL; }
    free(LS); LS = NULL;
    return(0);
  }

  // ---------------------------------------------------------------------------
  // Wrappers for evaluating ODE linear systems
  // ---------------------------------------------------------------------------

  static int cvLinSysSetup(realtype t, N_Vector y, N_Vector fy, SUNMatrix A,
                           booleantype jok, booleantype *jcur, realtype gamma,
                           void *user_data, N_Vector tmp1, N_Vector tmp2,
                           N_Vector tmp3)
  {
    // Get data from N_Vectors
    const Vector mfem_y(y);
    const Vector mfem_fy(fy);

    // Compute the linear system
    return(GetObj(A)->ODELinSys(t, mfem_y, mfem_fy, jok, jcur, gamma));
  }

  static int arkLinSysSetup(realtype t, N_Vector y, N_Vector fy, SUNMatrix A,
                            SUNMatrix M, booleantype jok, booleantype *jcur,
                            realtype gamma, void *user_data, N_Vector tmp1,
                            N_Vector tmp2, N_Vector tmp3)
  {
    // Get data from N_Vectors
    const Vector mfem_y(y);
    Vector mfem_fy(fy);

    // Compute the linear system
    return(GetObj(A)->ODELinSys(t, mfem_y, mfem_fy, jok, jcur, gamma));
  }

  static int arkMassSysSetup(realtype t, SUNMatrix M, void *user_data,
                             N_Vector tmp1, N_Vector tmp2, N_Vector tmp3)
  {
    // Compute the mass matrix linear system
    return(GetObj(M)->ODEMassSys(t));
  }

  // ---------------------------------------------------------------------------
  // CVODE interface
  // ---------------------------------------------------------------------------

  int CVODESolver::RHS(realtype t, const N_Vector y, N_Vector ydot,
                       void *user_data)
  {
    // Get data from N_Vectors
    const Vector mfem_y(y);
    Vector mfem_ydot(ydot);
    CVODESolver *self = static_cast<CVODESolver*>(user_data);

    // Compute y' = f(t, y)
    self->f->SetTime(t);
    self->f->Mult(mfem_y, mfem_ydot);

    // Return success
    return(0);
  }

  int CVODESolver::root(realtype t, N_Vector y, realtype *gout, void *user_data)
  {
    CVODESolver *self = static_cast<CVODESolver*>(user_data);

    if (!self->root_func) return CV_RTFUNC_FAIL;    

    Vector mfem_y(y);
    Vector mfem_gout(gout, self->root_components);
        
    return self->root_func(t, mfem_y, mfem_gout, self);
  }

  void CVODESolver::SetRootFinder(int components, RootFunction func) {
    root_func = func;

    flag = CVodeRootInit(sundials_mem, components, root);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in SetRootFinder()");
  }

  int CVODESolver::LinSysSetup(realtype t, N_Vector y, N_Vector fy, SUNMatrix A,
                               booleantype jok, booleantype *jcur,
                               realtype gamma, void *user_data, N_Vector tmp1,
                               N_Vector tmp2, N_Vector tmp3)
  {
    // Get data from N_Vectors
    const Vector mfem_y(y);
    const Vector mfem_fy(fy);
    CVODESolver *self = static_cast<CVODESolver*>(GET_CONTENT(A));

    // Compute the linear system
    return(self->f->ImplicitSetup(t, mfem_y, mfem_fy, jok, jcur, gamma));
  }

  int CVODESolver::LinSysSolve(SUNLinearSolver LS, SUNMatrix A, N_Vector x,
                               N_Vector b, realtype tol)
  {
    Vector mfem_x(x);
    const Vector mfem_b(b);
    CVODESolver *self = static_cast<CVODESolver*>(GET_CONTENT(LS));

    // Solve the linear system
    return(self->f->ImplicitSolve(mfem_x, mfem_b, tol));
  }

  CVODESolver::CVODESolver(int lmm)
    : lmm_type(lmm), step_mode(CV_NORMAL)
  {
    // Allocate an empty serial N_Vector
    AllocateEmptyN_Vector(y);
  }

#ifdef MFEM_USE_MPI
  CVODESolver::CVODESolver(MPI_Comm comm, int lmm)
    : lmm_type(lmm), step_mode(CV_NORMAL)
  {
    AllocateEmptyN_Vector(y, comm);
  }
#endif

  void CVODESolver::Init(TimeDependentOperator &f_)
  {
    mfem_error("CVODE Initialization error: use the initialization method\n"
               "CVODESolver::Init(TimeDependentOperator &f_, double &t, Vector &x)\n");
  }

  void CVODESolver::Init(TimeDependentOperator &f_, double &t, Vector &x)
  {
    // Check intputs for consistency
    MFEM_VERIFY(f_.Height() == x.Size(),
                "error inconsistent operator and vector size");

    MFEM_VERIFY(f_.GetTime() == t,
                "error inconsistent initial times");

    // Initialize the base class
    ODESolver::Init(f_);

    // Create or ReInit CVODE
    if (!sundials_mem) {

      // Create CVODE memory
      CVODESolver::Create(t, x);

    } else {

      // Check that data is the same size and update array data
      VerifyN_Vector(y, x);
      
      // Reinitialize CVODE memory
      flag = CVodeReInit(sundials_mem, t, y);
      MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeReInit()");

    }
  }

  void CVODESolver::Create(double &t, Vector &x)
  {
    // Fill N_Vector wrapper with initial condition data
    FillN_Vector(y, x);

    // Create CVODE
    sundials_mem = CVodeCreate(lmm_type);
    MFEM_VERIFY(sundials_mem, "error in CVodeCreate()");

    // Initialize CVODE
    flag = CVodeInit(sundials_mem, CVODESolver::RHS, t, y);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeInit()");

    // Attach the CVODESolver as user-defined data
    flag = CVodeSetUserData(sundials_mem, this);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeSetUserData()");

    // Set default tolerances
    flag = CVodeSStolerances(sundials_mem, default_rel_tol, default_abs_tol);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeSetSStolerances()");

    // Set default linear solver (Newton is the default Nonlinear Solver)
    LSA = SUNLinSol_SPGMR(y, PREC_NONE, 0);
    MFEM_VERIFY(LSA, "error in SUNLinSol_SPGMR()");

    flag = CVodeSetLinearSolver(sundials_mem, LSA, NULL);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeSetLinearSolver()");
  }

  void CVODESolver::Step(Vector &x, double &t, double &dt)
  {

    VerifyN_Vector(y, x);
    
    // Integrate the system
    double tout = t + dt;
    flag = CVode(sundials_mem, tout, y, &t, step_mode);
    MFEM_VERIFY(flag >= 0, "error in CVode()");

    // Return the last incremental step size
    flag = CVodeGetLastStep(sundials_mem, &dt);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeGetLastStep()");
  }

  void CVODESolver::SetLinearSolver(SundialsLinearSolver &ls_spec)
  {
    // Free any existing linear solver
    if (LSA != NULL) { SUNLinSolFree(LSA); LSA = NULL; }

    // Wrap linear solver as SUNLinearSolver and SUNMatrix
    LSA = SUNLinSolNewEmpty();
    MFEM_VERIFY(sundials_mem, "error in SUNLinSolNewEmpty()");

    LSA->content         = &ls_spec;
    LSA->ops->gettype    = LSGetType;
    LSA->ops->initialize = LSInit;
    LSA->ops->setup      = LSSetup;
    LSA->ops->solve      = LSSolve;
    LSA->ops->free       = LSFree;

    A = SUNMatNewEmpty();
    MFEM_VERIFY(sundials_mem, "error in SUNMatNewEmpty()");

    A->content      = &ls_spec;
    A->ops->getid   = MatGetID;
    A->ops->destroy = MatDestroy;

    // Attach the linear solver and matrix
    flag = CVodeSetLinearSolver(sundials_mem, LSA, A);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeSetLinearSolver()");

    // Set the linear system evaluation function
    flag = CVodeSetLinSysFn(sundials_mem, cvLinSysSetup);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeSetLinSysFn()");
  }


  void CVODESolver::SetLinearSolver()
  {
    // Free any existing linear solver
    if (LSA != NULL) { SUNLinSolFree(LSA); LSA = NULL; }

    // Wrap linear solver as SUNLinearSolver and SUNMatrix
    LSA = SUNLinSolNewEmpty();
    MFEM_VERIFY(sundials_mem, "error in SUNLinSolNewEmpty()");

    LSA->content         = this;
    LSA->ops->gettype    = LSGetType;
    LSA->ops->solve      = CVODESolver::LinSysSolve;
    LSA->ops->free       = LSFree;

    A = SUNMatNewEmpty();
    MFEM_VERIFY(sundials_mem, "error in SUNMatNewEmpty()");

    A->content      = this;
    A->ops->getid   = MatGetID;
    A->ops->destroy = MatDestroy;

    // Attach the linear solver and matrix
    flag = CVodeSetLinearSolver(sundials_mem, LSA, A);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeSetLinearSolver()");

    // Set the linear system evaluation function
    flag = CVodeSetLinSysFn(sundials_mem, CVODESolver::LinSysSetup);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeSetLinSysFn()");
  }

  void CVODESolver::SetStepMode(int itask)
  {
    step_mode = itask;
  }

  void CVODESolver::SetSStolerances(double reltol, double abstol)
  {
    flag = CVodeSStolerances(sundials_mem, reltol, abstol);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeSStolerances()");
  }

  void CVODESolver::SetSVtolerances(double reltol, Vector abstol)
  {
    MFEM_VERIFY(abstol.Size() == f->Height(), "abstolerance is not the same size.");
   
    N_Vector nv_abstol = N_VNewEmpty_Serial(0);    
    abstol.ToNVector(nv_abstol);
    
    flag = CVodeSVtolerances(sundials_mem, reltol, nv_abstol);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeSVtolerances()");
  }
  
  void CVODESolver::SetMaxStep(double dt_max)
  {
    flag = CVodeSetMaxStep(sundials_mem, dt_max);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeSetMaxStep()");
  }

  void CVODESolver::SetMaxOrder(int max_order)
  {
    flag = CVodeSetMaxOrd(sundials_mem, max_order);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeSetMaxOrd()");
  }

  void CVODESolver::PrintInfo() const
  {
    long int nsteps, nfevals, nlinsetups, netfails;
    int      qlast, qcur;
    double   hinused, hlast, hcur, tcur;
    long int nniters, nncfails;

    // Get integrator stats
    flag = CVodeGetIntegratorStats(sundials_mem,
                                   &nsteps,
                                   &nfevals,
                                   &nlinsetups,
                                   &netfails,
                                   &qlast,
                                   &qcur,
                                   &hinused,
                                   &hlast,
                                   &hcur,
                                   &tcur);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeGetIntegratorStats()");

    // Get nonlinear solver stats
    flag = CVodeGetNonlinSolvStats(sundials_mem,
                                   &nniters,
                                   &nncfails);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeGetNonlinSolvStats()");

    mfem::out <<
      "CVODE:\n"
      "num steps:            " << nsteps << "\n"
      "num rhs evals:        " << nfevals << "\n"
      "num lin setups:       " << nlinsetups << "\n"
      "num nonlin sol iters: " << nniters << "\n"
      "num nonlin conv fail: " << nncfails << "\n"
      "num error test fails: " << netfails << "\n"
      "last order:           " << qlast << "\n"
      "current order:        " << qcur << "\n"
      "initial dt:           " << hinused << "\n"
      "last dt:              " << hlast << "\n"
      "current dt:           " << hcur << "\n"
      "current t:            " << tcur << "\n" << endl;

    return;
  }

  CVODESolver::~CVODESolver()
  {
    N_VDestroy(y);
    SUNMatDestroy(A);
    SUNLinSolFree(LSA);
    SUNNonlinSolFree(NLS);
    CVodeFree(&sundials_mem);
  }


  // ---------------------------------------------------------------------------
  // CVODES interface
  // ---------------------------------------------------------------------------

  // Setup Newton problem M = (I-gamma J) is the linearized tangent of the rate equation
  int CVODESSolver::LinSysSetupB(realtype t, N_Vector y, N_Vector yB, N_Vector fyB, SUNMatrix AB,
				 booleantype jokB, booleantype *jcurB,
				 realtype gammaB, void *user_data, N_Vector tmp1,
				 N_Vector tmp2, N_Vector tmp3)
  {
    // Get data from N_Vectors
    const Vector mfem_y(y);
    const Vector mfem_yB(yB);
    Vector mfem_fyB(fyB);
    CVODESSolver *self = static_cast<CVODESSolver*>(GET_CONTENT(AB));
    TimeDependentAdjointOperator * f = static_cast<TimeDependentAdjointOperator *>(self->f);
    
    // Compute the linear system
    return(f->ImplicitSetupB(t, mfem_y, mfem_yB, mfem_fyB, jokB, jcurB, gammaB));
  }

  int CVODESSolver::LinSysSolveB(SUNLinearSolver LS, SUNMatrix AB, N_Vector yB,
                               N_Vector Rb, realtype tol)
  {
    Vector mfem_yB(yB);
    const Vector mfem_Rb(Rb);
    CVODESSolver *self = static_cast<CVODESSolver*>(GET_CONTENT(LS));
    TimeDependentAdjointOperator * f = static_cast<TimeDependentAdjointOperator *>(self->f);
    // Solve the linear system
    int ret = f->ImplicitSolveB(mfem_yB, mfem_Rb, tol);
    return(ret);
  }

  
  
  void CVODESSolver::CreateB(double &tB, Vector &xB)
  {
    // Fill N_Vector wrapper with initial condition data
    FillN_Vector(yB, xB);

    // Create the solver memory
    flag = CVodeCreateB(sundials_mem, CV_BDF, &indexB);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeCreateB()");
    
    // Initialize CVODEB
    flag = CVodeInitB(sundials_mem, indexB, fB, tB, yB);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeInit()");

    // Attach the CVODESSolver as user-defined data
    flag = CVodeSetUserDataB(sundials_mem, indexB, this);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeSetUserDataB()");

    // Set default tolerances
    flag = CVodeSStolerancesB(sundials_mem, indexB, default_rel_tolB, default_abs_tolB);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeSetSStolerancesB()");

  }
  
  CVODESSolver::CVODESSolver(int lmm) :
    CVODESolver(lmm),
    ncheck(0),
    indexB(0)
  {
    AllocateEmptyN_Vector(yB);
    AllocateEmptyN_Vector(yy);
  }
  
#ifdef MFEM_USE_MPI
  CVODESSolver::CVODESSolver(MPI_Comm comm, int lmm) :
    CVODESolver(comm, lmm),
    ncheck(0),
    indexB(0)
  {
    AllocateEmptyN_Vector(yB, comm);
    AllocateEmptyN_Vector(yy, comm);
  }
#endif
  
  void CVODESSolver::InitAdjointSolve(int steps)
  {
    flag = CVodeAdjInit(sundials_mem, steps, CV_HERMITE);
    MFEM_VERIFY(flag == CV_SUCCESS, "Error in CVodeAdjInit");
  }
  
  // For now just do some kind of default
  void CVODESSolver::InitQuadIntegration(double reltolQ, double abstolQ)
  {
    q = N_VNew_Serial(1);    

    flag = CVodeQuadInit(sundials_mem, fQ, q);
    MFEM_VERIFY(flag == CV_SUCCESS, "Error in CVodeQuadInit()");

    flag = CVodeSetQuadErrCon(sundials_mem, SUNTRUE);
    MFEM_VERIFY(flag == CV_SUCCESS, "Error in CVodeSetQuadErrCon");

    flag = CVodeQuadSStolerances(sundials_mem, reltolQ, abstolQ);
    MFEM_VERIFY(flag == CV_SUCCESS, "Error in CVodeQuadSStolerances");
    
  }

  // For now just do some kind of default
  void CVODESSolver::InitQuadIntegrationB(double reltolQB, double abstolQB)
  {
    qB = N_VNew_Serial(3);    

    flag = CVodeQuadInitB(sundials_mem, indexB, fQB, qB);
    MFEM_VERIFY(flag == CV_SUCCESS, "Error in CVodeQuadInitB()");

    flag = CVodeSetQuadErrConB(sundials_mem, indexB, SUNTRUE);
    MFEM_VERIFY(flag == CV_SUCCESS, "Error in CVodeSetQuadErrConB");

    flag = CVodeQuadSStolerancesB(sundials_mem, indexB, reltolQB, abstolQB);
    MFEM_VERIFY(flag == CV_SUCCESS, "Error in CVodeQuadSStolerancesB");
    
  }

  
  void CVODESSolver::Init(TimeDependentAdjointOperator &f_, double &t, Vector &x)
  {
    CVODESolver::Init(f_, t, x);
  }

  void CVODESSolver::InitB(TimeDependentAdjointOperator &f_, double &tB, Vector &xB)
  {

    int loc_size = f_.Height();
    // Initiailize forward solver output
    yy = N_VNew_Serial(NV_LENGTH_S(y));    

    CreateB(tB, xB);
    
    // // Set default linear solver (Newton is the default Nonlinear Solver)
    // LSB = SUNLinSol_SPGMR(yB, PREC_NONE, 0);
    // MFEM_VERIFY(LSB, "error in SUNLinSol_SPGMR()");

    // flag = CVodeSetLinearSolverB(sundials_mem, indexB, LSB, NULL);
    // MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeSetLinearSolverB()");

    /* Create dense SUNMatrix for use in linear solves */
    AB = SUNDenseMatrix(loc_size, loc_size);
    MFEM_VERIFY(AB, "error creating AB");
    
    /* Create dense SUNLinearSolver object */
    LSB = SUNLinSol_Dense(yB, AB);
    MFEM_VERIFY(LSB, "error in SUNLinSol_Dense()");
    
    /* Attach the matrix and linear solver */
    flag = CVodeSetLinearSolverB(sundials_mem, indexB, LSB, AB);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeSetLinearSolverB()");

  }

  void CVODESSolver::SetLinearSolverB()
  {
    // Free any existing linear solver
    if (LSB != NULL) { SUNLinSolFree(LSB); LSB = NULL; }

    // Wrap linear solver as SUNLinearSolver and SUNMatrix
    LSB = SUNLinSolNewEmpty();
    MFEM_VERIFY(sundials_mem, "error in SUNLinSolNewEmpty()");

    LSB->content         = this;
    LSB->ops->gettype    = LSGetType;
    LSB->ops->solve      = CVODESSolver::LinSysSolveB; // JW change
    LSB->ops->free       = LSFree;

    AB = SUNMatNewEmpty();
    MFEM_VERIFY(sundials_mem, "error in SUNMatNewEmpty()");

    AB->content      = this;
    AB->ops->getid   = MatGetID;
    AB->ops->destroy = MatDestroy;

    // Attach the linear solver and matrix
    flag = CVodeSetLinearSolverB(sundials_mem, indexB, LSB, AB);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeSetLinearSolverB()");

    // Set the linear system evaluation function
    flag = CVodeSetLinSysFnB(sundials_mem, indexB, CVODESSolver::LinSysSetupB); // JW change
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeSetLinSysFn()");
  }

  // void CVODESSolver::SetLinearSolverB(SundialsLinearSolver &ls_spec)
  // {
  //   // Free any existing linear solver
  //   if (LSB != NULL) { SUNLinSolFree(LSB); LSB = NULL; }

  //   // Wrap linear solver as SUNLinearSolver and SUNMatrix
  //   LSB = SUNLinSolNewEmpty();
  //   MFEM_VERIFY(sundials_mem, "error in SUNLinSolNewEmpty()");

  //   LSB->content         = &ls_spec;
  //   LSB->ops->gettype    = LSGetType;
  //   LSB->ops->initialize = LSInit;
  //   LSB->ops->setup      = LSSetup;
  //   LSB->ops->solve      = LSSolve;
  //   LSB->ops->free       = LSFree;

  //   AB = SUNMatNewEmpty();
  //   MFEM_VERIFY(sundials_mem, "error in SUNMatNewEmpty()");

  //   AB->content      = &ls_spec;
  //   AB->ops->getid   = MatGetID;
  //   AB->ops->destroy = MatDestroy;

  //   // Attach the linear solver and matrix
  //   flag = CVodeSetLinearSolverB(sundials_mem, indexB, LSB, AB);
  //   MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeSetLinearSolver()");

  //   // Todo: JW Requires rigging up ODELinSys and SundialsSetup in a SundialsSolver
  //   // Set the linear system evaluation function
  //   // flag = CVodeSetLinSysFnB(sundials_mem, indexB, cvLinSysSetupB);
  //   // MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeSetLinSysFnB()");
  // }

  
  int CVODESSolver::fQ(realtype t, const N_Vector y, N_Vector qdot, void *user_data)
  {
    CVODESSolver *self = static_cast<CVODESSolver*>(user_data);
    Vector mfem_y(y);
    Vector mfem_qdot(qdot);
    TimeDependentAdjointOperator * f = static_cast<TimeDependentAdjointOperator *>(self->f);
    f->SetTime(t);
    f->QuadratureIntegration(mfem_y, mfem_qdot);
    return 0;
  }

  int CVODESSolver::fQB(realtype t, N_Vector y, N_Vector yB, N_Vector qBdot, void *user_dataB)
  {
    CVODESSolver *self = static_cast<CVODESSolver*>(user_dataB);
    Vector mfem_y(y);
    Vector mfem_yB(yB);
    Vector mfem_qBdot(qBdot);
    TimeDependentAdjointOperator * f = static_cast<TimeDependentAdjointOperator *>(self->f);
    f->SetTime(t);
    f->ObjectiveSensitivityMult(mfem_y, mfem_yB, mfem_qBdot);
    return 0;   
  }

  int CVODESSolver::fB(realtype t, N_Vector y, N_Vector yB, N_Vector yBdot, void *user_dataB)
  {
    CVODESSolver *self = static_cast<CVODESSolver*>(user_dataB);
    Vector mfem_y(y);
    Vector mfem_yB(yB);
    Vector mfem_yBdot(yBdot);
    TimeDependentAdjointOperator * f = static_cast<TimeDependentAdjointOperator *>(self->f);
    f->SetTime(t);
    f->AdjointRateMult(mfem_y, mfem_yB, mfem_yBdot);    
    return 0;
  }
  
  int CVODESSolver::ewt(N_Vector y, N_Vector w, void *user_data)
  {
    CVODESSolver *self = static_cast<CVODESSolver*>(user_data);

    Vector mfem_y(y);
    Vector mfem_w(w);
    
    return self->ewt_func(mfem_y, mfem_w, self);
  }
  
  void CVODESSolver::SetWFTolerances(EWTFunction func)
  {
    ewt_func = func;
    CVodeWFtolerances(sundials_mem, ewt);
  }

  void CVODESSolver::Step(Vector &x, double &t, double &dt)
  {
    VerifyN_Vector(y, x);
    
    // Integrate the system
    double tout = t + dt;
    flag = CVodeF(sundials_mem, tout, y, &t, step_mode, &ncheck);
    //    flag = CVode(sundials_mem, tout, y, &t, step_mode);
    MFEM_VERIFY(flag >= 0, "error in CVodeF()");

    // Return the last incremental step size
    flag = CVodeGetLastStep(sundials_mem, &dt);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeGetLastStep()");
  }

  void CVODESSolver::StepB(Vector &xB, double &tB, double &dtB)
  {

    VerifyN_Vector(yB, xB);
    
    // Integrate the system
    double tout = tB - dtB;
    flag = CVodeB(sundials_mem, tout, step_mode);
    MFEM_VERIFY(flag >= 0, "error in CVodeB()");

    /* Call CVodeGetB to get yB of the backward ODE problem. */
    flag = CVodeGetB(sundials_mem, indexB, &tB, yB);
    MFEM_VERIFY(flag >= 0, "error in CVodeGetB()");

    // // JW : Is there a LastStepB?
    // // Return the last incremental step size
    // flag = CVodeGetLastStep(sundials_mem, &dtB);
    // MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeGetLastStep()");
  }

  void CVODESSolver::GetCorrespondingForwardSolution(double tB, mfem::Vector & yyy)
  {
    Vector mfem_yyy(yy);

    flag = CVodeGetAdjY(sundials_mem, tB, yy);
    MFEM_VERIFY(flag >= 0, "error in CVodeGetAdjY()");

    yyy = mfem_yyy;
  }
  
  
  long CVODESSolver::GetNumSteps()
  {
    long nst;
    flag = CVodeGetNumSteps(sundials_mem, &nst);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeGetNumStep()");

    return nst;
  }

  void CVODESSolver::EvalQuadIntegration(double t, Vector & Q)
  {
    MFEM_VERIFY(t <= f->GetTime(), "t > current forward solver time");
    
    flag = CVodeGetQuad(sundials_mem, &t, q);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeGetQuad()");

    Vector mfem_q(q);
    Q = mfem_q;
  }

  void CVODESSolver::EvalObjectiveSensitivity(double t, Vector &dG_dp)
  {
    MFEM_VERIFY(t <= f->GetTime(), "t > current forward solver time");
    
    flag = CVodeGetQuadB(sundials_mem, indexB, &t, qB);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in CVodeGetQuadB()");
    Vector mfem_qB(qB);
    dG_dp = mfem_qB;
    dG_dp *= -1.;
  }

  
  // ---------------------------------------------------------------------------
  // ARKStep interface
  // ---------------------------------------------------------------------------

  int ARKStepSolver::RHS1(realtype t, const N_Vector y, N_Vector ydot,
                          void *user_data)
  {
    // Get data from N_Vectors
    const Vector mfem_y(y);
    Vector mfem_ydot(ydot);
    ARKStepSolver *self = static_cast<ARKStepSolver*>(user_data);

    // Compute f(t, y) in y' = f(t, y) or fe(t, y) in y' = fe(t, y) + fi(t, y)
    self->f->SetTime(t);
    self->f->Mult(mfem_y, mfem_ydot);

    // Return success
    return(0);
  }

  int ARKStepSolver::RHS2(realtype t, const N_Vector y, N_Vector ydot,
                          void *user_data)
  {
    // Get data from N_Vectors
    const Vector mfem_y(y);
    Vector mfem_ydot(ydot);
    ARKStepSolver *self = static_cast<ARKStepSolver*>(user_data);

    // Compute fi(t, y) in y' = fe(t, y) + fi(t, y)
    self->f2->SetTime(t);
    self->f2->Mult(mfem_y, mfem_ydot);

    // Return success
    return(0);
  }

  int ARKStepSolver::LinSysSetup(realtype t, N_Vector y, N_Vector fy, SUNMatrix A,
                                 SUNMatrix M, booleantype jok, booleantype *jcur,
                                 realtype gamma, void *user_data, N_Vector tmp1,
                                 N_Vector tmp2, N_Vector tmp3)
  {
    // Get data from N_Vectors
    const Vector mfem_y(y);
    const Vector mfem_fy(fy);
    ARKStepSolver *self = static_cast<ARKStepSolver*>(GET_CONTENT(A));

    // Compute the linear system
    return(self->f->ImplicitSetup(t, mfem_y, mfem_fy, jok, jcur, gamma));
  }

  int ARKStepSolver::LinSysSolve(SUNLinearSolver LS, SUNMatrix A, N_Vector x,
                                 N_Vector b, realtype tol)
  {
    Vector mfem_x(x);
    const Vector mfem_b(b);
    ARKStepSolver *self = static_cast<ARKStepSolver*>(GET_CONTENT(LS));

    // Solve the linear system
    if (self->rk_type == IMPLICIT)
      return(self->f->ImplicitSolve(mfem_x, mfem_b, tol));
    else
      return(self->f2->ImplicitSolve(mfem_x, mfem_b, tol));
  }

  int ARKStepSolver::MassSysSetup(realtype t, SUNMatrix M, void *user_data,
                                  N_Vector tmp1, N_Vector tmp2, N_Vector tmp3)
  {
    ARKStepSolver *self = static_cast<ARKStepSolver*>(GET_CONTENT(M));

    // Compute the mass matrix system
    return(self->f->MassSetup(t));
  }

  int ARKStepSolver::MassSysSolve(SUNLinearSolver LS, SUNMatrix M, N_Vector x,
                                  N_Vector b, realtype tol)
  {
    Vector mfem_x(x);
    const Vector mfem_b(b);
    ARKStepSolver *self = static_cast<ARKStepSolver*>(GET_CONTENT(LS));

    // Solve the mass matrix system
    if (self->rk_type == IMPLICIT)
      return(self->f->MassSolve(mfem_x, mfem_b, tol));
    else
      return(self->f2->MassSolve(mfem_x, mfem_b, tol));
  }

  ARKStepSolver::ARKStepSolver(Type type)
    : rk_type(type), step_mode(ARK_NORMAL),
      use_implicit(type == IMPLICIT || type == IMEX)
  {
    // Allocate an empty serial N_Vector
    y = N_VNewEmpty_Serial(0);
    MFEM_VERIFY(y, "error in N_VNewEmpty_Serial()");
  }

#ifdef MFEM_USE_MPI
  ARKStepSolver::ARKStepSolver(MPI_Comm comm, Type type)
    : rk_type(type), step_mode(ARK_NORMAL),
      use_implicit(type == IMPLICIT || type == IMEX)
  {
    if (comm == MPI_COMM_NULL) {

      // Allocate an empty serial N_Vector
      y = N_VNewEmpty_Serial(0);
      MFEM_VERIFY(y, "error in N_VNewEmpty_Serial()");

    } else {

      // Allocate an empty parallel N_Vector
      y = N_VNewEmpty_Parallel(comm, 0, 0);  // calls MPI_Allreduce()
      MFEM_VERIFY(y, "error in N_VNewEmpty_Parallel()");

    }
  }
#endif

  void ARKStepSolver::Init(TimeDependentOperator &f_)
  {
    mfem_error("ARKStep Initialization error: use the initialization method\n"
               "ARKStepSolver::Init(TimeDependentOperator &f_, double &t, Vector &x)\n");
  }

  void ARKStepSolver::Init(TimeDependentOperator &f_, double &t, Vector &x)
  {
    // Check type
    MFEM_VERIFY(rk_type != IMEX,
                "error incorrect initialization method for IMEX problems\n");

    // Check intputs for consistency
    MFEM_VERIFY(f_.Height() == x.Size(),
                "error inconsistent operator and vector size");

    MFEM_VERIFY(f_.GetTime() == t,
                "error inconsistent initial times");

    // Initialize the base class
    ODESolver::Init(f_);

    // Create or ReInit ARKStep
    if (!sundials_mem) {

      // Create ARKStep memory
      ARKStepSolver::Create(t, x);

    } else {

      // Check that data is the same size and update array data
      if (!Parallel()) {
        MFEM_VERIFY(NV_LENGTH_S(y) == x.Size(),
                    "error to resize ARKStep use ARKStep::ReSize()");
        NV_DATA_S(y) = x.GetData();
      } else {
#ifdef MFEM_USE_MPI
        MFEM_VERIFY(NV_LOCLENGTH_P(y) == x.Size(),
                    "error to resize ARKStep use ARKStep::ReSize()");
        NV_DATA_P(y) = x.GetData();
#endif
      }

      // Reinitialize ARKStep memory
      if (rk_type == IMPLICIT)
        flag = ARKStepReInit(sundials_mem, NULL, ARKStepSolver::RHS1, t, y);
      else
        flag = ARKStepReInit(sundials_mem, ARKStepSolver::RHS1, NULL, t, y);
      MFEM_VERIFY(sundials_mem, "error in ARKStepReInit()");

    }
  }

  void ARKStepSolver::Init(TimeDependentOperator &f_, TimeDependentOperator &f2_,
                           double &t, Vector &x)
  {
    // Check type
    MFEM_VERIFY(rk_type == IMEX,
                "error incorrect initialization method for non-IMEX problems\n");

    // Check intputs for consistency
    MFEM_VERIFY(f_.Height() == x.Size(),
                "error inconsistent operator and vector size");

    MFEM_VERIFY(f_.GetTime() == t,
                "error inconsistent initial times");

    // Initialize the base class
    ODESolver::Init(f_, f2_);

    // Create or ReInit ARKStep
    if (!sundials_mem) {

      // Create ARKStep
      ARKStepSolver::Create(t, x);

    } else {

      // Check that data is the same size and update array data
      if (!Parallel()) {
        MFEM_VERIFY(NV_LENGTH_S(y) == x.Size(),
                    "error to resize ARKStep use ARKStep::ReSize()");
        NV_DATA_S(y) = x.GetData();
      } else {
#ifdef MFEM_USE_MPI
        MFEM_VERIFY(NV_LOCLENGTH_P(y) == x.Size(),
                    "error to resize ARKStep use ARKStep::ReSize()");
        NV_DATA_P(y) = x.GetData();
#endif
      }

      // Reinitialize ARKStep memory
      flag = ARKStepReInit(sundials_mem, ARKStepSolver::RHS1, ARKStepSolver::RHS2, t, y);
      MFEM_VERIFY(sundials_mem, "error in ARKStepCreate()");

    }
  }

  void ARKStepSolver::Create(double &t, Vector &x)
  {
    // Fill N_Vector wrapper with initial condition data
    if (!Parallel()) {
      NV_LENGTH_S(y) = x.Size();
      NV_DATA_S(y)   = x.GetData();
    } else {
#ifdef MFEM_USE_MPI
      long local_size = x.Size();
      long global_size;
      MPI_Allreduce(&local_size, &global_size, 1, MPI_LONG, MPI_SUM,
                    NV_COMM_P(y));
      NV_LOCLENGTH_P(y)  = x.Size();
      NV_GLOBLENGTH_P(y) = global_size;
      NV_DATA_P(y)       = x.GetData();
#endif
    }

    // Initialize ARKStep
    if (rk_type == IMPLICIT)
      sundials_mem = ARKStepCreate(NULL, ARKStepSolver::RHS1, t, y);
    else if (rk_type == EXPLICIT)
      sundials_mem = ARKStepCreate(ARKStepSolver::RHS1, NULL, t, y);
    else
      sundials_mem = ARKStepCreate(ARKStepSolver::RHS1, ARKStepSolver::RHS2, t, y);
    MFEM_VERIFY(sundials_mem, "error in ARKStepCreate()");

    // Attach the ARKStepSolver as user-defined data
    flag = ARKStepSetUserData(sundials_mem, this);
    MFEM_VERIFY(flag == ARK_SUCCESS, "error in ARKStepSetUserData()");

    // Set default tolerances
    flag = ARKStepSStolerances(sundials_mem, default_rel_tol, default_abs_tol);
    MFEM_VERIFY(flag == ARK_SUCCESS, "error in ARKStepSetSStolerances()");

    // If implicit, set default linear solver
    if (use_implicit) {
      LSA = SUNLinSol_SPGMR(y, PREC_NONE, 0);
      MFEM_VERIFY(LSA, "error in SUNLinSol_SPGMR()");

      flag = ARKStepSetLinearSolver(sundials_mem, LSA, NULL);
      MFEM_VERIFY(flag == ARK_SUCCESS, "error in ARKStepSetLinearSolver()");
    }
  }

  void ARKStepSolver::Resize(Vector &x, double hscale, double &t)
  {
    MFEM_VERIFY(f->GetTime() == t,
                "error inconsistent times");

    if (rk_type == IMEX)
      MFEM_VERIFY(f2->GetTime() == t,
                  "error inconsistent times");

    // Fill N_Vector wrapper
    if (!Parallel()) {
      NV_LENGTH_S(y) = x.Size();
      NV_DATA_S(y)   = x.GetData();
    } else {
#ifdef MFEM_USE_MPI
      long local_size = x.Size();
      long global_size;
      MPI_Allreduce(&local_size, &global_size, 1, MPI_LONG, MPI_SUM,
                    NV_COMM_P(y));
      NV_LOCLENGTH_P(y)  = x.Size();
      NV_GLOBLENGTH_P(y) = global_size;
      NV_DATA_P(y)       = x.GetData();
#endif
    }

    // Resize ARKode memory
    ARKStepResize(sundials_mem, y, hscale, t, NULL, NULL);
    MFEM_VERIFY(flag == ARK_SUCCESS, "error in ARKStepResize()");
  }

  void ARKStepSolver::Step(Vector &x, double &t, double &dt)
  {
    if (!Parallel()) {
      NV_DATA_S(y) = x.GetData();
      MFEM_VERIFY(NV_LENGTH_S(y) == x.Size(), "");
    } else {
#ifdef MFEM_USE_MPI
      NV_DATA_P(y) = x.GetData();
      MFEM_VERIFY(NV_LOCLENGTH_P(y) == x.Size(), "");
#endif
    }

    // Integrate the system
    double tout = t + dt;
    flag = ARKStepEvolve(sundials_mem, tout, y, &t, step_mode);
    MFEM_VERIFY(flag >= 0, "error in ARKStepEvolve()");

    // Return the last incremental step size
    flag = ARKStepGetLastStep(sundials_mem, &dt);
    MFEM_VERIFY(flag == ARK_SUCCESS, "error in ARKStepGetLastStep()");
  }

  void ARKStepSolver::SetLinearSolver(SundialsLinearSolver &ls_spec)
  {
    // Free any existing matrix and linear solver
    if (A != NULL)   { SUNMatDestroy(A); A = NULL; }
    if (LSA != NULL) { SUNLinSolFree(LSA); LSA = NULL; }

    // Check for implicit method before attaching
    MFEM_VERIFY(use_implicit,
                "The function is applicable only to implicit or imex time integration.");

    // Wrap linear solver as SUNLinearSolver and SUNMatrix
    LSA = SUNLinSolNewEmpty();
    MFEM_VERIFY(sundials_mem, "error in SUNLinSolNewEmpty()");

    LSA->content         = &ls_spec;
    LSA->ops->gettype    = LSGetType;
    LSA->ops->initialize = LSInit;
    LSA->ops->setup      = LSSetup;
    LSA->ops->solve      = LSSolve;
    LSA->ops->free       = LSFree;

    A = SUNMatNewEmpty();
    MFEM_VERIFY(sundials_mem, "error in SUNMatNewEmpty()");

    A->content      = &ls_spec;
    A->ops->getid   = MatGetID;
    A->ops->destroy = MatDestroy;

    // Attach the linear solver and matrix
    flag = ARKStepSetLinearSolver(sundials_mem, LSA, A);
    MFEM_VERIFY(flag == ARK_SUCCESS, "error in ARKStepSetLinearSolver()");

    // Set the linear system evaluation function
    flag = ARKStepSetLinSysFn(sundials_mem, arkLinSysSetup);
    MFEM_VERIFY(flag == ARK_SUCCESS, "error in ARKStepSetLinSysFn()");
  }

  void ARKStepSolver::SetLinearSolver()
  {
    // Free any existing linear solver
    if (LSA != NULL) { SUNLinSolFree(LSA); LSA = NULL; }

    // Wrap linear solver as SUNLinearSolver and SUNMatrix
    LSA = SUNLinSolNewEmpty();
    MFEM_VERIFY(sundials_mem, "error in SUNLinSolNewEmpty()");

    LSA->content         = this;
    LSA->ops->gettype    = LSGetType;
    LSA->ops->solve      = ARKStepSolver::LinSysSolve;
    LSA->ops->free       = LSFree;

    A = SUNMatNewEmpty();
    MFEM_VERIFY(sundials_mem, "error in SUNMatNewEmpty()");

    A->content      = this;
    A->ops->getid   = MatGetID;
    A->ops->destroy = MatDestroy;

    // Attach the linear solver and matrix
    flag = ARKStepSetLinearSolver(sundials_mem, LSA, A);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in ARKStepSetLinearSolver()");

    // Set the linear system evaluation function
    flag = ARKStepSetLinSysFn(sundials_mem, ARKStepSolver::LinSysSetup);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in ARKStepSetLinSysFn()");
  }

  void ARKStepSolver::SetMassLinearSolver(SundialsLinearSolver &ls_spec,
                                          int tdep)
  {
    // Free any existing matrix and linear solver
    if (M != NULL)   { SUNMatDestroy(M); M = NULL; }
    if (LSM != NULL) { SUNLinSolFree(LSM); LSA = NULL; }

    // Wrap linear solver as SUNLinearSolver and SUNMatrix
    LSM = SUNLinSolNewEmpty();
    MFEM_VERIFY(sundials_mem, "error in SUNLinSolNewEmpty()");

    LSM->content         = &ls_spec;
    LSM->ops->gettype    = LSGetType;
    LSM->ops->initialize = LSInit;
    LSM->ops->setup      = LSSetup;
    LSM->ops->solve      = LSSolve;
    LSA->ops->free       = LSFree;

    M = SUNMatNewEmpty();
    MFEM_VERIFY(sundials_mem, "error in SUNMatNewEmpty()");

    M->content      = &ls_spec;
    M->ops->getid   = SUNMatGetID;
    M->ops->destroy = MatDestroy;

    // Attach the linear solver and matrix
    flag = ARKStepSetMassLinearSolver(sundials_mem, LSM, M, tdep);
    MFEM_VERIFY(flag == ARK_SUCCESS, "error in ARKStepSetLinearSolver()");

    // Set the linear system function
    flag = ARKStepSetMassFn(sundials_mem, arkMassSysSetup);
    MFEM_VERIFY(flag == ARK_SUCCESS, "error in ARKStepSetMassFn()");
  }

  void ARKStepSolver::SetMassLinearSolver(int tdep)
  {
    // Free any existing matrix and linear solver
    if (M != NULL)   { SUNMatDestroy(M); M = NULL; }
    if (LSM != NULL) { SUNLinSolFree(LSM); LSA = NULL; }

    // Wrap linear solver as SUNLinearSolver and SUNMatrix
    LSM = SUNLinSolNewEmpty();
    MFEM_VERIFY(sundials_mem, "error in SUNLinSolNewEmpty()");

    LSM->content         = this;
    LSM->ops->gettype    = LSGetType;
    LSM->ops->solve      = ARKStepSolver::MassSysSolve;
    LSA->ops->free       = LSFree;

    M = SUNMatNewEmpty();
    MFEM_VERIFY(sundials_mem, "error in SUNMatNewEmpty()");

    M->content      = this;
    M->ops->getid   = SUNMatGetID;
    M->ops->destroy = MatDestroy;

    // Attach the linear solver and matrix
    flag = ARKStepSetMassLinearSolver(sundials_mem, LSM, M, tdep);
    MFEM_VERIFY(flag == ARK_SUCCESS, "error in ARKStepSetLinearSolver()");

    // Set the linear system function
    flag = ARKStepSetMassFn(sundials_mem, ARKStepSolver::MassSysSetup);
    MFEM_VERIFY(flag == ARK_SUCCESS, "error in ARKStepSetMassFn()");
  }

  void ARKStepSolver::SetStepMode(int itask)
  {
    step_mode = itask;
  }

  void ARKStepSolver::SetSStolerances(double reltol, double abstol)
  {
    flag = ARKStepSStolerances(sundials_mem, reltol, abstol);
    MFEM_VERIFY(flag == ARK_SUCCESS, "error in ARKStepSStolerances()");
  }

  void ARKStepSolver::SetMaxStep(double dt_max)
  {
    flag = ARKStepSetMaxStep(sundials_mem, dt_max);
    MFEM_VERIFY(flag == ARK_SUCCESS, "error in ARKStepSetMaxStep()");
  }

  void ARKStepSolver::SetOrder(int order)
  {
    flag = ARKStepSetOrder(sundials_mem, order);
    MFEM_VERIFY(flag == ARK_SUCCESS, "error in ARKStepSetOrder()");
  }

  void ARKStepSolver::SetERKTableNum(int table_num)
  {
    flag = ARKStepSetTableNum(sundials_mem, -1, table_num);
    MFEM_VERIFY(flag == ARK_SUCCESS, "error in ARKStepSetTableNum()");
  }

  void ARKStepSolver::SetIRKTableNum(int table_num)
  {
    flag = ARKStepSetTableNum(sundials_mem, table_num, -1);
    MFEM_VERIFY(flag == ARK_SUCCESS, "error in ARKStepSetTableNum()");
  }

  void ARKStepSolver::SetIMEXTableNum(int etable_num, int itable_num)
  {
    flag = ARKStepSetTableNum(sundials_mem, itable_num, itable_num);
    MFEM_VERIFY(flag == ARK_SUCCESS, "error in ARKStepSetTableNum()");
  }

  void ARKStepSolver::SetFixedStep(double dt)
  {
    flag = ARKStepSetFixedStep(sundials_mem, dt);
    MFEM_VERIFY(flag == ARK_SUCCESS, "error in ARKStepSetFixedStep()");
  }

  void ARKStepSolver::PrintInfo() const
  {
    long int nsteps, expsteps, accsteps, step_attempts;
    long int nfe_evals, nfi_evals;
    long int nlinsetups, netfails;
    double   hinused, hlast, hcur, tcur;
    long int nniters, nncfails;

    // Get integrator stats
    flag = ARKStepGetTimestepperStats(sundials_mem,
                                      &expsteps,
                                      &accsteps,
                                      &step_attempts,
                                      &nfe_evals,
                                      &nfi_evals,
                                      &nlinsetups,
                                      &netfails);
    MFEM_VERIFY(flag == ARK_SUCCESS, "error in ARKStepGetTimestepperStats()");

    flag = ARKStepGetStepStats(sundials_mem,
                               &nsteps,
                               &hinused,
                               &hlast,
                               &hcur,
                               &tcur);

    // Get nonlinear solver stats
    flag = ARKStepGetNonlinSolvStats(sundials_mem,
                                     &nniters,
                                     &nncfails);
    MFEM_VERIFY(flag == ARK_SUCCESS, "error in ARKStepGetNonlinSolvStats()");

    mfem::out <<
      "ARKStep:\n"
      "num steps:                 " << nsteps << "\n"
      "num exp rhs evals:         " << nfe_evals << "\n"
      "num imp rhs evals:         " << nfi_evals << "\n"
      "num lin setups:            " << nlinsetups << "\n"
      "num nonlin sol iters:      " << nniters << "\n"
      "num nonlin conv fail:      " << nncfails << "\n"
      "num steps attempted:       " << step_attempts << "\n"
      "num acc limited steps:     " << accsteps << "\n"
      "num exp limited stepfails: " << expsteps << "\n"
      "num error test fails:      " << netfails << "\n"
      "initial dt:                " << hinused << "\n"
      "last dt:                   " << hlast << "\n"
      "current dt:                " << hcur << "\n"
      "current t:                 " << tcur << "\n" << endl;

    return;
  }

  ARKStepSolver::~ARKStepSolver()
  {
    N_VDestroy(y);
    SUNMatDestroy(A);
    SUNLinSolFree(LSA);
    SUNNonlinSolFree(NLS);
    ARKStepFree(&sundials_mem);
  }

  // ---------------------------------------------------------------------------
  // KINSOL interface
  // ---------------------------------------------------------------------------

  // Wrapper for evaluating the nonlinear residual F(u) = 0
  int KINSolver::Mult(const N_Vector u, N_Vector fu, void *user_data)
  {
    const Vector mfem_u(u);
    Vector mfem_fu(fu);
    KINSolver *self = static_cast<KINSolver*>(user_data);

    // Compute the non-linear action F(u).
    self->oper->Mult(mfem_u, mfem_fu);

    // Return success
    return 0;
  }

  // Wrapper for computing Jacobian-vector products
  int KINSolver::GradientMult(N_Vector v, N_Vector Jv, N_Vector u,
                              booleantype *new_u, void *user_data)
  {
    const Vector mfem_v(v);
    Vector mfem_Jv(Jv);
    KINSolver *self = static_cast<KINSolver*>(user_data);

    // Update Jacobian information if needed
    if (*new_u) {
      const Vector mfem_u(u);
      self->jacobian = &self->oper->GetGradient(mfem_u);
      *new_u = SUNFALSE;
    }

    // Compute the Jacobian-vector product
    self->jacobian->Mult(mfem_v, mfem_Jv);

    // Return success
    return 0;
  }

  // Wrapper for evaluating linear systems J u = b
  int KINSolver::LinSysSetup(N_Vector u, N_Vector fu, SUNMatrix J,
                             void *user_data, N_Vector tmp1, N_Vector tmp2)
  {
    const Vector mfem_u(u);
    KINSolver *self = static_cast<KINSolver*>(GET_CONTENT(J));

    // Update the Jacobian
    self->jacobian = &self->oper->GetGradient(mfem_u);

    // Set the Jacobian solve operator
    self->prec->SetOperator(*self->jacobian);

    // Return success
    return(0);
  }

  // Wrapper for solving linear systems J u = b
  int KINSolver::LinSysSolve(SUNLinearSolver LS, SUNMatrix J, N_Vector u,
                             N_Vector b, realtype tol)
  {
    Vector mfem_u(u), mfem_b(b);
    KINSolver *self = static_cast<KINSolver*>(GET_CONTENT(LS));

    // Solve for u = [J(u)]^{-1} b, maybe approximately.
    self->prec->Mult(mfem_b, mfem_u);

    // Return success
    return(0);
  }

  KINSolver::KINSolver(int strategy, bool oper_grad)
    : global_strategy(strategy), use_oper_grad(oper_grad), y_scale(NULL),
      f_scale(NULL), jacobian(NULL)
  {
    // Allocate empty serial N_Vectors
    y = N_VNewEmpty_Serial(0);
    y_scale = N_VNewEmpty_Serial(0);
    f_scale = N_VNewEmpty_Serial(0);
    MFEM_VERIFY(y && y_scale && f_scale, "Error in N_VNewEmpty_Serial().");

    // Create the solver memory
    sundials_mem = KINCreate();
    MFEM_VERIFY(sundials_mem, "Error in KINCreate().");

    // Default abs_tol and print_level
    abs_tol     = pow(UNIT_ROUNDOFF, 1.0/3.0);
    print_level = 0;
  }

#ifdef MFEM_USE_MPI
  KINSolver::KINSolver(MPI_Comm comm, int strategy, bool oper_grad)
    : global_strategy(strategy), use_oper_grad(oper_grad), y_scale(NULL),
      f_scale(NULL), jacobian(NULL)
  {
    if (comm == MPI_COMM_NULL) {

      // Allocate empty serial N_Vectors
      y = N_VNewEmpty_Serial(0);
      y_scale = N_VNewEmpty_Serial(0);
      f_scale = N_VNewEmpty_Serial(0);
      MFEM_VERIFY(y && y_scale && f_scale, "error in N_VNewEmpty_Serial()");

    } else {

      // Allocate empty parallel N_Vectors
      y = N_VNewEmpty_Parallel(comm, 0, 0);
      y_scale = N_VNewEmpty_Parallel(comm, 0, 0);
      f_scale = N_VNewEmpty_Parallel(comm, 0, 0);
      MFEM_VERIFY(y && y_scale && f_scale, "error in N_VNewEmpty_Parallel()");

    }

    // Create the solver memory
    sundials_mem = KINCreate();
    MFEM_VERIFY(sundials_mem, "error in KINCreate().");

    // Default abs_tol and print_level
    abs_tol     = pow(UNIT_ROUNDOFF, 1.0/3.0);
    print_level = 0;
  }
#endif


  void KINSolver::SetOperator(const Operator &op)
  {
    // Initialize the base class
    NewtonSolver::SetOperator(op);
    jacobian = NULL;

    // Set actual size and data in the N_Vector y.
    if (!Parallel()) {

      NV_LENGTH_S(y)       = height;
      NV_DATA_S(y)         = new double[height](); // value-initialize
      NV_LENGTH_S(y_scale) = height;
      NV_DATA_S(y_scale)   = NULL;
      NV_LENGTH_S(f_scale) = height;
      NV_DATA_S(f_scale)   = NULL;

    } else {
#ifdef MFEM_USE_MPI
      long local_size = height, global_size;
      MPI_Allreduce(&local_size, &global_size, 1, MPI_LONG, MPI_SUM,
                    NV_COMM_P(y));
      NV_LOCLENGTH_P(y)        = local_size;
      NV_GLOBLENGTH_P(y)       = global_size;
      NV_DATA_P(y)             = new double[local_size](); // value-initialize
      NV_LOCLENGTH_P(y_scale)  = local_size;
      NV_GLOBLENGTH_P(y_scale) = global_size;
      NV_DATA_P(y_scale)       = NULL;
      NV_LOCLENGTH_P(f_scale)  = local_size;
      NV_GLOBLENGTH_P(f_scale) = global_size;
      NV_DATA_P(f_scale)       = NULL;
#endif
    }

    // Initialize KINSOL
    flag = KINInit(sundials_mem, KINSolver::Mult, y);
    MFEM_VERIFY(flag == KIN_SUCCESS, "error in KINInit()");

    // Attach the KINSolver as user-defined data
    flag = KINSetUserData(sundials_mem, this);
    MFEM_ASSERT(flag == KIN_SUCCESS, "error in KINSetUserData()");

    // Set default linear solver if necessary
    if (!prec) {
      LSA  = SUNLinSol_SPGMR(y, PREC_NONE, 0);
      MFEM_VERIFY(LSA, "error in SUNLinSol_SPGMR()");

      flag = KINSetLinearSolver(sundials_mem, LSA, NULL);
      MFEM_ASSERT(flag == KIN_SUCCESS, "error in KINSetLinearSolver()");

      // Set Jacobian-vector product function
      if (use_oper_grad) {
        flag = KINSetJacTimesVecFn(sundials_mem, KINSolver::GradientMult);
        MFEM_ASSERT(flag == KIN_SUCCESS, "error in KINSetJacTimesVecFn()");
      }
    }

    // Delete the allocated data in y.
    if (!Parallel()) {
      delete [] NV_DATA_S(y);
      NV_DATA_S(y) = NULL;
    } else {
#ifdef MFEM_USE_MPI
      delete [] NV_DATA_P(y);
      NV_DATA_P(y) = NULL;
#endif
    }
  }

  void KINSolver::SetSolver(Solver &solver)
  {
    // Store the solver
    prec = &solver;

    // Free any existing linear solver
    if (LSA != NULL) { SUNLinSolFree(LSA); LSA = NULL; }

    // Wrap KINSolver as SUNLinearSolver and SUNMatrix
    LSA = SUNLinSolNewEmpty();
    MFEM_VERIFY(sundials_mem, "error in SUNLinSolNewEmpty()");

    LSA->content      = this;
    LSA->ops->gettype = LSGetType;
    LSA->ops->solve   = KINSolver::LinSysSolve;
    LSA->ops->free    = LSFree;

    A = SUNMatNewEmpty();
    MFEM_VERIFY(sundials_mem, "error in SUNMatNewEmpty()");

    A->content      = this;
    A->ops->getid   = MatGetID;
    A->ops->destroy = MatDestroy;

    // Attach the linear solver and matrix
    flag = KINSetLinearSolver(sundials_mem, LSA, A);
    MFEM_VERIFY(flag == KIN_SUCCESS, "error in KINSetLinearSolver()");

    // Set the Jacobian evaluation function
    flag = KINSetJacFn(sundials_mem, KINSolver::LinSysSetup);
    MFEM_VERIFY(flag == CV_SUCCESS, "error in KINSetJacFn()");
  }

  void KINSolver::SetScaledStepTol(double sstol)
  {
    flag = KINSetScaledStepTol(sundials_mem, sstol);
    MFEM_ASSERT(flag == KIN_SUCCESS, "error in KINSetScaledStepTol()");
  }

  void KINSolver::SetMaxSetupCalls(int max_calls)
  {
    flag = KINSetMaxSetupCalls(sundials_mem, max_calls);
    MFEM_ASSERT(flag == KIN_SUCCESS, "error in KINSetMaxSetupCalls()");
  }

  // Compute the scaling vectors and solve nonlinear system
  void KINSolver::Mult(const Vector &b, Vector &x) const
  {
    // Uses c = 1, corresponding to x_scale.
    c = 1.0;

    if (!iterative_mode) { x = 0.0; }

    // For relative tolerance, r = 1 / |residual(x)|, corresponding to fx_scale.
    if (rel_tol > 0.0) {

      oper->Mult(x, r);

      // Note that KINSOL uses infinity norms.
      double norm;
      if (!Parallel()) {
        norm = r.Normlinf();
      } else {
#ifdef MFEM_USE_MPI
        double lnorm = r.Normlinf();
        MPI_Allreduce(&lnorm, &norm, 1, MPI_DOUBLE, MPI_MAX, NV_COMM_P(y));
#endif
      }
      if (abs_tol > rel_tol * norm) {
        r = 1.0;
      } else {
        r =  1.0 / norm;
      }
    } else {
      r = 1.0;
    }

    // Set the residual norm tolerance
    flag = KINSetFuncNormTol(sundials_mem, abs_tol);
    MFEM_ASSERT(flag == KIN_SUCCESS, "error in KINSetFuncNormTol()");

    // Solve the nonlinear system by calling the other Mult method
    KINSolver::Mult(x, c, r);
  }

  // Solve the onlinear system using the provided scaling vectors
  void KINSolver::Mult(Vector &x,
                       const Vector &x_scale, const Vector &fx_scale) const
  {
    flag = KINSetPrintLevel(sundials_mem, print_level);
    MFEM_VERIFY(flag == KIN_SUCCESS, "KINSetPrintLevel() failed!");

    flag = KINSetNumMaxIters(sundials_mem, max_iter);
    MFEM_ASSERT(flag == KIN_SUCCESS, "KINSetNumMaxIters() failed!");

    if (!Parallel()) {

      NV_DATA_S(y) = x.GetData();
      MFEM_VERIFY(NV_LENGTH_S(y) == x.Size(), "");
      NV_DATA_S(y_scale) = x_scale.GetData();
      NV_DATA_S(f_scale) = fx_scale.GetData();

    } else {

#ifdef MFEM_USE_MPI
      NV_DATA_P(y) = x.GetData();
      MFEM_VERIFY(NV_LOCLENGTH_P(y) == x.Size(), "");
      NV_DATA_P(y_scale) = x_scale.GetData();
      NV_DATA_P(f_scale) = fx_scale.GetData();
#endif

    }

    if (!iterative_mode) { x = 0.0; }

    // Solve the nonlinear system
    flag = KINSol(sundials_mem, y, global_strategy, y_scale, f_scale);
    converged = (flag >= 0);

    // Get number of nonlinear iterations
    long int tmp_nni;
    flag = KINGetNumNonlinSolvIters(sundials_mem, &tmp_nni);
    MFEM_ASSERT(flag == KIN_SUCCESS, "error in KINGetNumNonlinSolvIters()");
    final_iter = (int) tmp_nni;

    // Get the residual norm
    flag = KINGetFuncNorm(sundials_mem, &final_norm);
    MFEM_ASSERT(flag == KIN_SUCCESS, "error in KINGetFuncNorm()");
  }

  KINSolver::~KINSolver()
  {
    N_VDestroy(y);
    N_VDestroy(y_scale);
    N_VDestroy(f_scale);
    SUNMatDestroy(A);
    SUNLinSolFree(LSA);
    KINFree(&sundials_mem);
  }

} // namespace mfem

#endif
