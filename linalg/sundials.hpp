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

/*
  Approach 1:
    - Updated Init function to take initial contion as input
    - Setting options must occur after initialization
    - Addition of LinSysSetup functions to setup linear systems
    - Addition of SUNLinSolEmpty() and SUNMatEmpty() functions to make
      creating wrappers to linear solver and matrix easier. Also protects
      against the addition of new optional operations to the APIs.
    - Simplified user-supplied methods for custom linear solvers.
    - Need to add ReInit and ReSize methods.
*/

#ifndef MFEM_SUNDIALS
#define MFEM_SUNDIALS

#include "../config/config.hpp"

#ifdef MFEM_USE_SUNDIALS

#ifdef MFEM_USE_MPI
#include <mpi.h>
#endif

#include "ode.hpp"
#include "solvers.hpp"

#include <cvodes/cvodes.h>
#include <arkode/arkode_arkstep.h>
#include <kinsol/kinsol.h>

#include <functional>

#ifdef MFEM_USE_MPI
#include <nvector/nvector_parallel.h>
#endif


namespace mfem
{
  // ---------------------------------------------------------------------------
  // Base class for interfacing with SUNMatrix and SUNLinearSolver API
  // ---------------------------------------------------------------------------

  /** Abstract base class for providing custom linear solvers to SUNDIALS ODE
      packages, CVODE and ARKODE. For a given ODE system

      dy/dt = f(y,t) or M dy/dt = f(y,t)

      the purpose of this class is to facilitate the (approximate) solution of
      linear systems of the form

      (I - gamma J) y = b or (M - gamma J) y = b,   J = J(y,t) = df/dy

      and mass matrix systems of the form

      M y = b,   M = M(t)

      for given b, y, t and gamma, where gamma is a scaled time step. */
  class SundialsLinearSolver
  {
  protected:
    SundialsLinearSolver() { }
    virtual ~SundialsLinearSolver() { }

  public:
    /** Setup the ODE linear system A(y,t) = (I - gamma J) or A = (M - gamma J).
        @param[in]  t     The time at which A(y,t) should be evaluated
        @param[in]  y     The state at which A(y,t) should be evaluated
        @param[in]  fy    The current value of the ODE Rhs function, f(y,t)
        @param[in]  jok   Flag indicating if the Jacobian should be updated
        @param[out] jcur  Flag to signal if the Jacobian was updated
        @param[in]  gamma The scaled time step value */
    virtual int ODELinSys(double t, Vector y, Vector fy, int jok, int *jcur,
                          double gamma)
    {
      mfem_error("SundialsLinearSolver::ODELinSys() is not overridden!");
      return(-1);
    }
    
    /** Setup the ODE linear system A(y,t) = (I - gamma J) or A = (M - gamma J).
        @param[in]  t     The time at which A(y,t) should be evaluated
        @param[in]  y     The state at which A(y,t) should be evaluated
        @param[in]  yB     The state at which A(yB,t) should be evaluated
        @param[in]  fyB    The current value of the ODE Rhs function, f(y,t)
        @param[in]  jok   Flag indicating if the Jacobian should be updated
        @param[out] jcur  Flag to signal if the Jacobian was updated
        @param[in]  gamma The scaled time step value */   
    virtual int ODELinSysB(double t, Vector y, Vector yB, Vector fyB, int jok, int *jcur,
                          double gamma)
    {
      mfem_error("SundialsLinearSolver::ODELinSysB() is not overridden!");
      return(-1);
    }

    
    /** Setup the ODE Mass matrix system M.
        @param[in] t The time at which M(t) should be evaluated */
    virtual int ODEMassSys(double t)
    {
      mfem_error("SundialsLinearSolver::ODEMassSys() is not overridden!");
      return(-1);
    }

    /** Initialize the linear solver (optional). */
    virtual int Init() { return(0); };

    /** Setup the linear solver (optional). */
    virtual int Setup() { return(0); };

    /** Solve the linear system A x = b.
        @param[in/out]  x  On input, the initial guess. On output, the solution
        @param[in]      b  The linear system right-hand side */
    virtual int Solve(Vector &x, Vector b) = 0;
  };

  // ---------------------------------------------------------------------------
  // Base class for interfacing with SUNDIALS packages
  // ---------------------------------------------------------------------------

  class SundialsSolver
  {
  protected:
    void *sundials_mem; /// SUNDIALS mem structure.
    mutable int flag;   /// Last flag returned from a call to SUNDIALS.

    N_Vector           y;   /// State vector.
    SUNMatrix          A;   /// Linear system A = I - gamma J, M - gamma J, or J.
    SUNMatrix          M;   /// Mass matrix M.
    SUNLinearSolver    LSA; /// Linear solver for A.
    SUNLinearSolver    LSM; /// Linear solver for M.
    SUNNonlinearSolver NLS; /// Nonlinear solver.

#ifdef MFEM_USE_MPI
    bool Parallel() const
    { return (N_VGetVectorID(y) != SUNDIALS_NVEC_SERIAL); }
#else
    bool Parallel() const { return false; }
#endif

    /// Default scalar tolerances.
    static constexpr double default_rel_tol = 1e-4;
    static constexpr double default_abs_tol = 1e-9;

    /// Constructors
    SundialsSolver() : sundials_mem(NULL), flag(0), y(NULL), A(NULL), M(NULL),
                       LSA(NULL), LSM(NULL), NLS(NULL) { }

    void FillN_Vector(N_Vector &y, Vector &x)
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
    }

    void VerifyN_Vector(N_Vector &y, Vector &x)
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
    }

    void AllocateEmptyN_Vector(N_Vector &y, MPI_Comm comm = MPI_COMM_NULL)
    {
      if (comm == MPI_COMM_NULL) {

	// Allocate an empty serial N_Vector
	y = N_VNewEmpty_Serial(0);
	MFEM_VERIFY(y, "error in N_VNewEmpty_Serial()");

      } else {
#ifdef MFEM_USE_MPI	
	// Allocate an empty parallel N_Vector
	y = N_VNewEmpty_Parallel(comm, 0, 0);  // calls MPI_Allreduce()
	MFEM_VERIFY(y, "error in N_VNewEmpty_Parallel()");
#endif
      }      
    }
    
  public:
    /// Access the SUNDIALS memory structure.
    void *GetMem() const { return sundials_mem; }

    /// Returns the last flag retured a call to a SUNDIALS function.
    int GetFlag() const { return flag; }
  };

  // ---------------------------------------------------------------------------
  // Interface to the CVODE library -- linear multi-step methods
  // ---------------------------------------------------------------------------

  class CVODESolver : public ODESolver, public SundialsSolver
  {
  private:
    /// Utility function for creating CVODE.
    void Create(double &t, Vector &x);

  protected:
    int lmm_type;  /// linear multistep method type
    int step_mode; /// CVODE step mode (CV_NORMAL or CV_ONE_STEP).
    int root_components; /// Number of components in gout   

    /// Wrapper to compute the ODE Rhs function.
    static int RHS(realtype t, const N_Vector y, N_Vector ydot, void *user_data);

    /// Setup the linear system A x = b
    static int LinSysSetup(realtype t, N_Vector y, N_Vector fy, SUNMatrix A,
                           booleantype jok, booleantype *jcur,
                           realtype gamma, void *user_data, N_Vector tmp1,
                           N_Vector tmp2, N_Vector tmp3);

    /// Solve the linear system A x = b
    static int LinSysSolve(SUNLinearSolver LS, SUNMatrix A, N_Vector x,
                           N_Vector b, realtype tol);

    static int root(realtype t, N_Vector y, realtype *gout, void *user_data);
    
    typedef std::function<int(realtype t, Vector y, Vector gout, CVODESolver *)> RootFunction;
    RootFunction root_func;
    
  public:
    /** Construct a serial wrapper to SUNDIALS' CVODE integrator.
        @param[in] lmm Specifies the linear multistep method, the options are:
                       CV_ADAMS - implicit methods for non-stiff systems
                       CV_BDF   - implicit methods for stiff systems */
    CVODESolver(int lmm);

#ifdef MFEM_USE_MPI
    /** Construct a parallel wrapper to SUNDIALS' CVODE integrator.
        @param[in] comm The MPI communicator used to partition the ODE system
        @param[in] lmm  Specifies the linear multistep method, the options are:
                        CV_ADAMS - implicit methods for non-stiff systems
                        CV_BDF   - implicit methods for stiff systems */
    CVODESolver(MPI_Comm comm, int lmm);
#endif

    /// Base class Init -- DO NOT CALL, use the below initialization function
    /// that takes the initial t and x as inputs.
    virtual void Init(TimeDependentOperator &f_);

    /** Initialize CVODE: Calls CVodeInit() and sets some defaults.
        @param[in] f_ the TimeDependentOperator that defines the ODE system
        @param[in] t  the initial time
        @param[in] x  the initial condition

        @note All other methods must be called after Init(). */
    void Init(TimeDependentOperator &f_, double &t, Vector &x);

    /** Integrate the ODE with CVODE using the specified step mode.

        @param[out]    x  Solution vector at the requested output timem x=x(t).
        @param[in/out] t  On output, the output time reached.
        @param[in/out] dt On output, the last time step taken.

        @note On input, the values of t and dt are used to compute desired
        output time for the integration, tout = t + dt.
    */
    virtual void Step(Vector &x, double &t, double &dt);

    /** Attach a custom linear solver solver to CVODE.
        @param[in] ls_spec A SundialsLinearSolver object defining the custom
                           linear solver */
    void SetLinearSolver(SundialsLinearSolver &ls_spec);

    /** Attach a custom linear solver solver to CVODE. */
    void SetLinearSolver();

    /** Select the CVODE step mode: CV_NORMAL (default) or CV_ONE_STEP.
        @param[in] itask  The desired step mode */
    void SetStepMode(int itask);

    /** Set the scalar relative and scalar absolute tolerances. */
    void SetSStolerances(double reltol, double abstol);

    /** Set the scalar relative and vector of absolute tolerances. */
    void SetSVtolerances(double reltol, Vector abstol);

    /** Initialize root Finder */
    void SetRootFinder(int components, RootFunction func);
    
    /** Set the maximum time step. */
    void SetMaxStep(double dt_max);

    /** Set the maximum method order.

        CVODE uses adaptive-order integration, based on the local truncation
        error. The default values for max_order are 12 for CV_ADAMS and
        5 for CV_BDF. Use this if you know a priori that your system is such
        that higher order integration formulas are unstable.

        @note max_order can't be higher than the current maximum order. */
    void SetMaxOrder(int max_order);

    /** Print various CVODE statistics. */
    void PrintInfo() const;

    /// Destroy the associated CVODE memory and SUNDIALS objects.
    virtual ~CVODESolver();
  };
  
  class TimeDependentAdjointOperator : public TimeDependentOperator
  {
  public:
    TimeDependentAdjointOperator(int dim, double t = 0., Type type = EXPLICIT) :
      TimeDependentOperator(dim, t, type) {}

    virtual ~TimeDependentAdjointOperator(){};

    virtual void QuadratureIntegration(const Vector &y, Vector &qdot) const = 0;
    virtual void AdjointRateMult(const Vector &y, Vector & yB, Vector &yBdot) const = 0;
    virtual void ObjectiveSensitivityMult(const Vector &y, const Vector &yB, Vector &qBdot) const = 0;
    virtual int ImplicitSetupB(const double t, const Vector &x, const Vector &xB, const Vector &fxB,
			       int jokB, int *jcurB, double gammaB)
    {
      mfem_error("TimeDependentOperator::ImplicitSetupB() is not overridden!");
      return(-1);
    }

    virtual int ImplicitSolveB(Vector &x, const Vector &b, double tol)
    {
      mfem_error("TimeDependentOperator::ImplicitSolveB() is not overridden!");
      return(-1);
    }
    
  protected:
    
  };
  
  // ---------------------------------------------------------------------------
  // Interface to the CVODES library -- linear multi-step methods
  // ---------------------------------------------------------------------------

  class CVODESSolver : public CVODESolver
  {
  protected:
    int ncheck; // number of checkpoints used so far
    int indexB; // backward index?
    
    /// Wrapper to compute the ODE Rhs function.
    static int fQ(realtype t, const N_Vector y, N_Vector qdot, void *user_data);

    static int fB(realtype t, N_Vector y, 
		  N_Vector yB, N_Vector yBdot, void *user_dataB);

    static int fQB(realtype t, N_Vector y, N_Vector yB, 
		   N_Vector qBdot, void *user_dataB);

    static int ewt(N_Vector y, N_Vector w, void *user_data);

    typedef std::function<int(Vector y, Vector w, CVODESSolver*)> EWTFunction;
    EWTFunction ewt_func;

    SUNMatrix          AB;   /// Linear system A = I - gamma J, M - gamma J, or J.
    SUNLinearSolver    LSB;  /// Linear solver for A.
    N_Vector           q;    /// Quadrature vector.
    N_Vector           yB;   /// State vector.
    N_Vector           yy;   /// State vector.
    N_Vector           qB;   /// State vector.

    /// Default scalar tolerances.
    static constexpr double default_rel_tolB = 1e-4;
    static constexpr double default_abs_tolB = 1e-9;
    static constexpr double default_abs_tolQB = 1e-9;
    
    
  public:
    /** Construct a serial wrapper to SUNDIALS' CVODE integrator.
        @param[in] lmm Specifies the linear multistep method, the options are:
                       CV_ADAMS - implicit methods for non-stiff systems
                       CV_BDF   - implicit methods for stiff systems */
    CVODESSolver(int lmm);

#ifdef MFEM_USE_MPI
    /** Construct a parallel wrapper to SUNDIALS' CVODE integrator.
        @param[in] comm The MPI communicator used to partition the ODE system
        @param[in] lmm  Specifies the linear multistep method, the options are:
                        CV_ADAMS - implicit methods for non-stiff systems
                        CV_BDF   - implicit methods for stiff systems */
    CVODESSolver(MPI_Comm comm, int lmm);
#endif

    /** Initialize CVODE: Calls CVodeInit() and sets some defaults.
        @param[in] f_ the TimeDependentOperator that defines the ODE system
        @param[in] t  the initial time
        @param[in] x  the initial condition

        @note All other methods must be called after Init(). */
    void Init(TimeDependentAdjointOperator &f_, double &t, Vector &x);

    void InitB(TimeDependentAdjointOperator &f_, double &tB, Vector &xB);
    
    /** Integrate the ODE with CVODE using the specified step mode.

        @param[out]    x  Solution vector at the requested output timem x=x(t).
        @param[in/out] t  On output, the output time reached.
        @param[in/out] dt On output, the last time step taken.

        @note On input, the values of t and dt are used to compute desired
        output time for the integration, tout = t + dt.
    */
    virtual void Step(Vector &x, double &t, double &dt);

    // Adjoint stuff
    virtual void StepB(Vector &w, double &t, double &dt);

    /// Set multiplicative error weights
    void SetWFTolerances(EWTFunction func);

    // Initialize Quadrature Integration
    void InitQuadIntegration(double reltolQ = 1.e-3, double abstolQ = 1e-8);

    // Initialize Quadrature Integration (Adjoint)
    void InitQuadIntegrationB(double reltolQB = 1.e-3, double abstolQB = 1e-8);
    
    // Initialize Adjoint
    void InitAdjointSolve(int steps);

    // Get Number of Steps for ForwardSolve
    long GetNumSteps();

    // Evalute Quadrature
    void EvalQuadIntegration(double t, Vector &q);

    // Evaluate Quadrature solution
    void EvalObjectiveSensitivity(double t, Vector &dG_dp);
    
    // Get Interpolated Forward solution y at backward integration time tB
    void GetCorrespondingForwardSolution(double tB, mfem::Vector & yy);
    
    // Set Linear Solver for the backward problem
    void SetLinearSolverB(SundialsLinearSolver &ls_spec);

    void SetLinearSolverB();

    
    /// Setup the linear system A x = b
    static int LinSysSetupB(realtype t, N_Vector y, N_Vector yB, N_Vector fyB, SUNMatrix A,
			    booleantype jok, booleantype *jcur,
			    realtype gamma, void *user_data, N_Vector tmp1,
			    N_Vector tmp2, N_Vector tmp3);

    /// Solve the linear system A x = b
    static int LinSysSolveB(SUNLinearSolver LS, SUNMatrix A, N_Vector x,
                           N_Vector b, realtype tol);

    
    /// Destroy the associated CVODE memory and SUNDIALS objects.
    virtual ~CVODESSolver() {};

  private:
    void CreateB(double &t, Vector &x);
    
  };

  
  // ---------------------------------------------------------------------------
  // Interface to ARKode's ARKStep module -- Additive Runge-Kutta methods
  // ---------------------------------------------------------------------------

  class ARKStepSolver : public ODESolver, public SundialsSolver
  {
  private:
    /// Utility function for creating ARKStep.
    void Create(double &t, Vector &x);

  public:
    /// Types of ARKODE solvers.
    enum Type { EXPLICIT, IMPLICIT, IMEX };

  protected:
    Type rk_type;      /// Runge-Kutta type
    int step_mode;     /// ARKStep step mode (ARK_NORMAL or ARK_ONE_STEP).
    bool use_implicit; /// true for implicit or imex integration

    /// Wrappers to compute the ODE Rhs functions. RHS1 is explicit RHS and RHS2
    /// the implicit RHS for IMEX integration. When purely implicit or explicit
    /// only RHS1 is used.
    static int RHS1(realtype t, const N_Vector y, N_Vector ydot, void *user_data);
    static int RHS2(realtype t, const N_Vector y, N_Vector ydot, void *user_data);

    /// Setup the linear system A x = b
    static int LinSysSetup(realtype t, N_Vector y, N_Vector fy, SUNMatrix A,
                           SUNMatrix M, booleantype jok, booleantype *jcur,
                           realtype gamma, void *user_data, N_Vector tmp1,
                           N_Vector tmp2, N_Vector tmp3);

    /// Solve the linear system A x = b
    static int LinSysSolve(SUNLinearSolver LS, SUNMatrix A, N_Vector x,
                           N_Vector b, realtype tol);

    /// Setup the linear system M x = b
    static int MassSysSetup(realtype t, SUNMatrix M, void *user_data,
                            N_Vector tmp1, N_Vector tmp2, N_Vector tmp3);

    /// Solve the linear system M x = b
    static int MassSysSolve(SUNLinearSolver LS, SUNMatrix M, N_Vector x,
                            N_Vector b, realtype tol);

  public:
    /** Construct a serial wrapper to SUNDIALS' ARKode integrator.
        @param[in] type Specifies the RK method type
                        EXPLICIT - explicit RK method
                        IMPLICIT - implicit RK method
                        IMEX     - implicit-explicit ARK method */
    ARKStepSolver(Type type = EXPLICIT);

#ifdef MFEM_USE_MPI
    /** Construct a parallel wrapper to SUNDIALS' ARKode integrator.
        @param[in] comm The MPI communicator used to partition the ODE system
        @param[in] type Specifies the RK method type
                        EXPLICIT - explicit RK method
                        IMPLICIT - implicit RK method
                        IMEX     - implicit-explicit ARK method */
    ARKStepSolver(MPI_Comm comm, Type type = EXPLICIT);
#endif

    /// Base class Init -- DO NOT CALL, use the below initialization function
    /// that takes the initial t and x as inputs.
    virtual void Init(TimeDependentOperator &f_);

    /** Initialize ARKode: Calls ARKStepInit() for explicit or implicit problems
        and sets some defaults.
        @param[in] f_ the TimeDependentOperator that defines the ODE system
        @param[in] t  the initial time
        @param[in] x  the initial condition

        @note All other methods must be called after Init(). */
    void Init(TimeDependentOperator &f_, double &t, Vector &x);

    /** Initialize ARKode: Calls ARKStepInit() for IMEX problems and sets some
        defaults.
        @param[in] f_ the TimeDependentOperator that defines the ODE system
        @param[in] t  the initial time
        @param[in] x  the initial condition

        @note All other methods must be called after Init(). */
    void Init(TimeDependentOperator &f_, TimeDependentOperator &f2_, double &t,
              Vector &x);

    /** Resize ARKode: Resize ARKode internal memory for the current problem.

        @param[in] x      the newly-sized state vector x(t).
        @param[in] hscale sacling factor for the next time step.
        @param[in] t      the current time (must be consistent with x(t).
    */
    void Resize(Vector &x, double hscale, double &t);

    /** Integrate the ODE with ARKode using the specified step mode.

        @param[out]    x  Solution vector at the requested output timem x=x(t).
        @param[in/out] t  On output, the output time reached.
        @param[in/out] dt On output, the last time step taken.

        @note On input, the values of t and dt are used to compute desired
        output time for the integration, tout = t + dt.
    */
    virtual void Step(Vector &x, double &t, double &dt);

    /** Attach a custom linear solver solver to ARKode.
        @param[in] ls_spec A SundialsLinearSolver object defining the custom
                           linear solver */
    void SetLinearSolver(SundialsLinearSolver &ls_spec);

    /** Attach a custom linear solver solver to ARKode.*/
    void SetLinearSolver();

    /** Attach a custom mass matrix linear solver solver to ARKode.
        @param[in] ls_spec A SundialsLinearSolver object defining the custom
                           linear solver
        @param[in] tdep    A integer flag indicating if the mass matrix is time
                           dependent (1) or time independent (0). */
    void SetMassLinearSolver(SundialsLinearSolver &ls_spec, int tdep);

    /** Attach a custom mass matrix linear solver solver to ARKode.
        @param[in] tdep    A integer flag indicating if the mass matrix is time
                           dependent (1) or time independent (0). */
    void SetMassLinearSolver(int tdep);

    /** Select the ARKode step mode: ARK_NORMAL (default) or ARK_ONE_STEP.
        @param[in] itask  The desired step mode */
    void SetStepMode(int itask);

    /** Set the scalar relative and scalar absolute tolerances. */
    void SetSStolerances(double reltol, double abstol);

    /** Set the maximum time step. */
    void SetMaxStep(double dt_max);

    /// Chooses integration order for all explicit / implicit / IMEX methods.
    /** The default is 4, and the allowed ranges are: [2, 8] for explicit;
        [2, 5] for implicit; [3, 5] for IMEX. */
    void SetOrder(int order);

    /// Choose a specific Butcher table for an explicit RK method.
    /** See the documentation for all possible options, stability regions, etc.
        For example, table_num = BOGACKI_SHAMPINE_4_2_3 is 4-stage 3rd order. */
    void SetERKTableNum(int table_num);

    /// Choose a specific Butcher table for a diagonally implicit RK method.
    /** See the documentation for all possible options, stability regions, etc.
        For example, table_num = CASH_5_3_4 is 5-stage 4th order. */
    void SetIRKTableNum(int table_num);

    /// Choose a specific Butcher table for an IMEX RK method.
    /** See the documentation for all possible options, stability regions, etc.
        For example, etable_num = ARK548L2SA_DIRK_8_4_5 and
        itable_num = ARK548L2SA_ERK_8_4_5 is 8-stage 5th order. */
    void SetIMEXTableNum(int etable_num, int itable_num);

    /// Use a fixed time step size (disable temporal adaptivity).
    /** Use of this function is not recommended, since there is no assurance of
        the validity of the computed solutions. It is primarily provided for
        code-to-code verification testing purposes. */
    void SetFixedStep(double dt);

    /** Print various ARKStep statistics. */
    void PrintInfo() const;

    /// Destroy the associated ARKode memory and SUNDIALS objects.
    virtual ~ARKStepSolver();
  };

  // ---------------------------------------------------------------------------
  // Interface to the KINSOL library -- nonlinear solver methods
  // ---------------------------------------------------------------------------

  class KINSolver : public NewtonSolver, public SundialsSolver
  {
  protected:
    int global_strategy;               // KINSOL solution strategy
    bool use_oper_grad;                // use the Jv prod function
    mutable N_Vector y_scale, f_scale; // scaling vectors
    const Operator *jacobian;          // stores oper->GetGradient()

    /// Wrapper to compute the nonlinear residual F(u) = 0
    static int Mult(const N_Vector u, N_Vector fu, void *user_data);

    /// Wrapper to compute the Jacobian-vector product J(u) v = Jv
    static int GradientMult(N_Vector v, N_Vector Jv, N_Vector u,
                            booleantype *new_u, void *user_data);

    /// Setup the linear system J u = b
    static int LinSysSetup(N_Vector u, N_Vector fu, SUNMatrix J,
                           void *user_data, N_Vector tmp1, N_Vector tmp2);

    /// Solve the linear system J u = b
    static int LinSysSolve(SUNLinearSolver LS, SUNMatrix J, N_Vector u,
                           N_Vector b, realtype tol);

  public:

    /// Construct a serial warpper to SUNDIALS' KINSOL nonlinear solver
    /** @param[in] strategy   Specifies the nonlinear solver strategy:
                              KIN_NONE / KIN_LINESEARCH / KIN_PICARD / KIN_FP.
        @param[in] oper_grad  Specifies whether the solver should use its
                              Operator's GetGradient() method to compute the
                              Jacobian of the system. */
    KINSolver(int strategy, bool oper_grad = true);

#ifdef MFEM_USE_MPI
    /// Construct a parallel warpper to SUNDIALS' KINSOL nonlinear solver
    /** @param[in] comm       The MPI communicator used to partition the system.
        @param[in] strategy   Specifies the nonlinear solver strategy:
                              KIN_NONE / KIN_LINESEARCH / KIN_PICARD / KIN_FP.
       @param[in] oper_grad   Specifies whether the solver should use its
                              Operator's GetGradient() method to compute the
                              Jacobian of the system. */
    KINSolver(MPI_Comm comm, int strategy, bool oper_grad = true);
#endif

    /// Destroy the associated KINSOL memory.
    virtual ~KINSolver();

    /// Set the nonlinear Operator of the system and initialize KINSOL.
    virtual void SetOperator(const Operator &op);

    /// Set the linear solver for inverting the Jacobian.
    /** @note This function assumes that Operator::GetGradient(const Vector &)
              is implemented by the Operator specified by
              SetOperator(const Operator &). */
    virtual void SetSolver(Solver &solver);

    /// Equivalent to SetSolver(Solver)
    virtual void SetPreconditioner(Solver &solver) { SetSolver(solver); }

   /// Set KINSOL's scaled step tolerance.
   /** The default tolerance is U^(2/3), where U = machine unit roundoff. */
   void SetScaledStepTol(double sstol);

    /// Set maximum number of nonlinear iterations without a Jacobian update.
    /** The default is 10. */
    void SetMaxSetupCalls(int max_calls);

    /// Solve the nonlinear system F(x) = 0.
    /** This method computes the x_scale and fx_scale vectors and calls the
        other Mult(Vector&, Vector&, Vector&) const method. The x_scale vector
        is a vector of ones and values of fx_scale are determined by comparing
        the chosen relative and functional norm (i.e. absolute) tolerances.
        @param[in]     b  Not used, KINSOL always assumes zero RHS.
        @param[in,out] x  On input, initial guess, if @a #iterative_mode = true,
                          otherwise the initial guess is zero; on output, the
                          solution. */
    virtual void Mult(const Vector &b, Vector &x) const;

   /// Solve the nonlinear system F(x) = 0.
   /** Calls KINSol() to solve the nonlinear system. Before calling KINSol(),
       this functions uses the data members inherited from class IterativeSolver
       to set corresponding KINSOL options.
       @param[in,out] x         On input, initial guess, if @a #iterative_mode =
                                true, otherwise the initial guess is zero; on
                                output, the solution.
       @param[in]     x_scale   Elements of a diagonal scaling matrix D, s.t.
                                D*x has all elements roughly the same when
                                x is close to a solution.
       @param[in]     fx_scale  Elements of a diagonal scaling matrix E, s.t.
                                D*F(x) has all elements roughly the same when
                                x is not too close to a solution. */
    void Mult(Vector &x, const Vector &x_scale, const Vector &fx_scale) const;
  };

}  // namespace mfem

#endif // MFEM_USE_SUNDIALS

#endif // MFEM_SUNDIALS
