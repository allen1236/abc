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

ABC_NAMESPACE_HEADER_START

////////////////////////////////////////////////////////////////////////
///                         STRUCTURES                               ///
////////////////////////////////////////////////////////////////////////

typedef struct Minr_Man_t_ Minr_Man_t;
struct Minr_Man_t_
{
    // User Configuration
    Gia_Man_t * pGia;       // Source AIG
    int         nFrames;    // Unroll depth (k)
    char * pInitStr;   // Polarity constraints / Initial state (-I)
    int         fRandTarget; // Use random sim to derive target reset (-r)
    int         nRandomSim;  // Number of random simulation frames (-R, default=k if -r given)
    char * pSolver;    // Path to MaxSAT solver binary (-S)
    char * pOutDir;    // Output directory
    char * pPrefix;    // Output filename prefix
    int         vLevel;     // Verbose level (0/1/2)
    int         seed;       // Random seed

    // Internal State
    Vec_Int_t * vVarMap;    // Mapping: (ObjId, Frame) -> SatVar (Base)
    Vec_Wec_t * vClauses;   // CNF Store
    int         nSatVars;   // Current SAT variable count (1-based)
    
    // Phase 2: Propagation & Cut
    Vec_Int_t * vPropVals;  // 3-value simulation result for each Obj
                            // 0=Logic0, 1=Logic1, 2=Unknown(X)
    Vec_Int_t * vCutNodes;  // List of ObjIds in the selected Cut

    // Decoded solution (for verify / user-friendly access)
    Vec_Int_t * vPiVals;    // size = nFrames * nPI, entries {0,1} (t=0..k-1)
    Vec_Int_t * vRoVals0;   // size = nReg, entries {0,1,2(X)} at t=0
    
    // Statistics
    abctime     timeStart;
};

////////////////////////////////////////////////////////////////////////
///                     FUNCTION DEFINITIONS                         ///
////////////////////////////////////////////////////////////////////////

extern void Minr_Solve( Gia_Man_t * pGia, int nFrames, char * pInitStr, int fRandTarget, int nRandomSim, char * pSolver, char * pOutDir, char * pPrefix, int vLevel, int seed );

ABC_NAMESPACE_HEADER_END

#endif