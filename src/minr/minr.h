/**CFile****************************************************************
  FileName    [minr.h]
  SystemName  [ABC: Logic synthesis and verification system.]
  PackageName [Minr: MaxSAT-based partial reset minimization.]
  Synopsis    [External declarations.]
  Author      [Allen]
  Affiliation [NTU]
  Date        [Ver. 2.0. Updated - Feb 01, 2026.]
***********************************************************************/

#ifndef ABC__src__minr__minr_h
#define ABC__src__minr__minr_h

#include "base/abc/abc.h"
#include "aig/gia/gia.h"
#include "minr_ipamir_dyn.h"

ABC_NAMESPACE_HEADER_START

////////////////////////////////////////////////////////////////////////
///                         STRUCTURES                               ///
////////////////////////////////////////////////////////////////////////

typedef struct Minr_Man_t_ Minr_Man_t;

// -O 2: fraction of time budget per segment (1/P). In .h for tuning.
#define MINR_OPT_BUDGET_PARTS 10
// Early stop in optimize: if improvement (vs previous k) below this ratio, stop. 0.001 = 0.1%.
#define MINR_EARLY_STOP_IMPROVEMENT_RATIO 0.001
// In-process MaxSAT via IPAMIR (EvalMaxSAT2022); path relative to ABC cwd.
#define MINR_IPAMIR_SO_DEFAULT "third_party/EvalMaxSAT2022/libipamirEvalMaxSAT2022.so"
// -p (cut diagnosis): external EvalMaxSAT binary (WCNF → stdout file). Normal &minr uses IPAMIR only.
// MINR_DEBUG_EVALMAXSAT_BIN — path to EvalMaxSAT_bin (default: third_party/EvalMaxSAT/build/EvalMaxSAT_bin).
// MINR_DEBUG_EVALMAXSAT_TIMEOUT_MULT — CPU-time multiplier on -t for external EvalMaxSAT (RLIMIT_CPU seconds; default 1).
// MINR_DEBUG_EVALMAXSAT_NO_TIMEOUT=1 — do not wrap with timeout(1).

struct Minr_Man_t_
{
    // User Configuration
    Gia_Man_t * pGia;       // Source AIG
    int         nFrames;    // Unroll depth (k)
    char * pInitStr;   // Polarity constraints / Initial state (-I)
    int         fExplicitInit; // 1 if user explicitly provided -I
    int         fRandTarget; // Use random sim to derive target reset (-r)
    int         nRandomSim;  // Number of random simulation frames (-R, default=k if -r given)
    int         vLevel;     // Verbose level (0/1/2)
    int         seed;       // Random seed
    int         nRefineMode;    // -x <mode>: 0=off, 1=CEC, 2=cut, 3=eq cut
    int         fRefineBindDc;  // -X: bind don't-care target ROs to unrolled t=k (only with -x)
    int         nRefineConfLimit; // -c <int>: SAT refine conflict limit (0=unlimited)
    int         fRefineCoreOnly;  // -C: core-only refine (skip trial release)
    char *      pReportFile;    // Report output filename (-o)
    int         nOptimizeMode;  // -O <mode>: 0=off, 1=sweep k, 2=outer-loop heuristic
    double      totalTimeout;   // Total time budget in seconds (-t)
    int         nDontCarePercent; // -D <1..99>: % of target registers set to don't care
    int         nOptimizeDenseKMax; // -K N with -O 1: dense sweep inclusive end; -1 = geometric schedule
    int         nOptimizeDenseKMin; // with -K: dense sweep start from -k (CLI default 0 if -k omitted)
    int         fDebugNoPropCut;   // -p: use external EvalMaxSAT binary instead of IPAMIR (.so)
    int         fSpecRegConstraintAtK; // -S: at t=k constrain specified ROs (target 0/1) instead of constant cut

    // Internal State
    Vec_Int_t * vVarMap;    // Mapping: (ObjId, Frame) -> SatVar (Base)
    Vec_Wec_t * vClauses;   // CNF Store
    int         nSatVars;   // Current SAT variable count (1-based)
    
    // Phase 2: Propagation & Cut
    Vec_Int_t * vPropVals;  // 3-value simulation result for each Obj
                            // 0=Logic0, 1=Logic1, 2=Unknown(X)
    Vec_Int_t * vCutNodes;   // List of ObjIds in the constraint cut
    Vec_Int_t * vEqCutNodes; // List of ObjIds in eq cut (mode 3 only)

    // Decoded solution (for verify / user-friendly access)
    Vec_Int_t * vPiVals;    // size = nFrames * nPI, entries {0,1} (t=0..k-1)
    Vec_Int_t * vRoVals0;   // size = nReg, entries {0,1,2(X)} at t=0
    
    // Refine statistics
    int         nRefineReleased;    // total #FFs released by refine
    int         nRefineByTrial;     // #FFs released by one-by-one trial
    int         nRefineByCore;      // #FFs released by UNSAT core analysis
    abctime     timeRefine;         // refine wall-clock time

    // Diagnostic statistics
    double      specRoCutRatio;     // % of specified (0/1) ROs appearing in constraint cut
    double      simRegMismatchWeakPct;   // % X vs 0/1 (using don't-care)
    double      simRegMismatchStrongPct; // % 0 vs 1 (actual conflict)

    // Result tracking
    int         solverStatus;   // 0=not_run, 1=optimum, 2=unsat, 3=error, 4=timeout
    int         fVerifyPass;    // cut-based verification result (1=pass, 0=fail)
    int         fCecVerifyPass; // CEC-based verification result (1=pass, 0=fail)
    abctime     timeSolveStart; // timer start (after target state derived); thread CPU ticks
    abctime     timeSolveStartWall; // Abc_Clock() at same boundary as timeSolveStart (for runtime_wall_sec)
    abctime     timeSolveEnd;   // timer end (before verification); 0 = use current time; CPU ticks
    abctime     timeSolveEndWall;   // Abc_Clock() when timeSolveEnd was taken (for runtime_wall_sec)
    abctime     timeTickAfterOptimize; // after Minr_SolveOptimize (-O 1); 0 if N/A; for runtime_sec floor
    abctime     timeSolver;     // MaxSAT: thread CPU around ipamir_solve, or parent overhead around external wait4
    double      extSolverChildCpuSec; // cumulative EvalMaxSAT child user+sys CPU (wait4); external binary only; drives -t + report

    // -O 2 only: intermediate target state and concatenated output
    Vec_Int_t * vPiAtK;         // PI at t=k for current iteration (prev iter's t=0). NULL = first iter
    Vec_Int_t * vConcatPiVals;  // output: concatenated PI sequence over all outer iterations

    // Optimize mode: best-so-far tracking
    int         bestK;          // k that produced the best solution
    int         bestResetCount; // best (minimum) reset count found so far
    Vec_Int_t * vBestPiVals;    // PI sequence of best solution
    Vec_Int_t * vBestRoVals0;   // RO values of best solution
    int         bestSolverStatus;
    int         optStatus;      // 0=found_best, 1=stopped_with_best (see optLastFailSolverStatus), 2=no_solution
    int         optLastFailSolverStatus; // when optStatus==1: solverStatus that ended sweep (2=UNSAT, 4=timeout)

    // Optimize mode: per-iteration log (-O 1)
    Vec_Int_t * vOptIterK;      // k value per iteration
    Vec_Int_t * vOptIterResets; // reset count (-1 = no solution)
    Vec_Int_t * vOptIterStatus; // solverStatus per iteration
    Vec_Int_t * vOptIterTimeMs; // iteration CPU-ish time in ms (thread + ext child delta when external)
    Vec_Int_t * vOptIterWallMs; // iteration wall elapsed ms (Abc_Clock around Minr_SolveSingleK)

    // -O 2: per-outer-iteration log (each wec row = one outer iter)
    Vec_Int_t * vOpt2OuterSegmentTimeMs;  // segment time per outer
    Vec_Int_t * vOpt2OuterBestResets;     // best resets at end of that segment
    Vec_Int_t * vOpt2OuterTargetResets;   // target state reset count at start of each outer
    Vec_Wec_t * vOpt2OuterInnerK;        // inner k list per outer
    Vec_Wec_t * vOpt2OuterInnerResets;   // inner resets per outer
    Vec_Wec_t * vOpt2OuterInnerTimeMs;   // inner time_ms per outer

    // Incremental MaxSAT state (for -O 1)
    Minr_IpamirApi_t incrApi;       // IPAMIR API function pointers (valid when pIncrSolver != NULL)
    void *          pIncrSolver;    // persistent IPAMIR solver instance (NULL = not initialized)
    int             nIncrFrames;    // highest frame index in solver (-1 = not initialized)
    int             nIncrPiPromoted;// highest frame with PI binary constraint (-1 = none)
    int             fIncrSoftAdded; // 1 if soft clauses have been added to the solver
    Vec_Int_t *     vFrameVarBase;  // base SAT variable for each frame (indexed by frame)
};

////////////////////////////////////////////////////////////////////////
///                     FUNCTION DEFINITIONS                         ///
////////////////////////////////////////////////////////////////////////

extern void Minr_ExtractCut( Minr_Man_t * p );
extern void Minr_ExtractEqCut( Minr_Man_t * p );
extern void Minr_Solve( Gia_Man_t * pGia, int nFrames, char * pInitStr, int fExplicitInit, int fRandTarget, int nRandomSim, int vLevel, int seed, int nRefineMode, int fRefineBindDc, int nRefineConfLimit, int fRefineCoreOnly, char * pReportFile, int nOptimizeMode, double totalTimeout, int nDontCarePercent, int nOptimizeDenseKMax, int nOptimizeDenseKMin, int fDebugNoPropCut, int fSpecRegConstraintAtK );

/** 1 iff -S is on and target has at least one specified (0/1) register; else solver falls back to cut at t=k. */
static inline int Minr_ManUsesSpecRegAtLastTf( Minr_Man_t const * p )
{
    Gia_Man_t * pGia;
    int nRegs, ri;
    if ( !p || !p->fSpecRegConstraintAtK || !p->pInitStr )
        return 0;
    pGia = p->pGia;
    if ( !pGia )
        return 0;
    nRegs = Gia_ManRegNum( pGia );
    for ( ri = 0; ri < nRegs; ri++ )
        if ( p->pInitStr[ri] == '0' || p->pInitStr[ri] == '1' )
            return 1;
    return 0;
}
extern void Minr_SolveOptimize( Minr_Man_t * p );
extern void Minr_SolveOptimize2( Minr_Man_t * p );

extern void Minr_SatRefine( Minr_Man_t * p );
extern int  Minr_SatVerify( Minr_Man_t * p );
extern int  Minr_CecVerify( Minr_Man_t * p );

ABC_NAMESPACE_HEADER_END

#endif