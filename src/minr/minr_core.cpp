/**CFile****************************************************************
  FileName    [minr_core.cpp]
  SystemName  [ABC: Logic synthesis and verification system.]
  PackageName [Minr: MaxSAT-based partial reset minimization.]
  Synopsis    [Core logic: Propagation, Cut, Unrolling, CNF, Solver, Decode.]
  Author      [Allen]
  Affiliation [NTU]
  Date        [Ver. 2.3. Updated - Feb 03, 2026.]
***********************************************************************/

#include "minr.h"
#include "minr_ipamir_dyn.h"
#include <chrono>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

ABC_NAMESPACE_IMPL_START

////////////////////////////////////////////////////////////////////////
///                        CONSTANTS & MACROS                        ///
////////////////////////////////////////////////////////////////////////

// 3-Value Logic Constants
#define MINR_VAL_0 0
#define MINR_VAL_1 1
#define MINR_VAL_X 2

// After Abc_Random(1), discard this many Abc_Random(0) per unit of user -r seed (seed 0 = no skip).
#define MINR_RANDOM_SKIP_MULT 10000u

// Helper macros for Dual-Rail
static inline int Minr_GetVar(Minr_Man_t * p, int ObjId, int Frame) {
    return Vec_IntEntry(p->vVarMap, ObjId * (p->nFrames + 1) + Frame);
}
static inline int Lit_T(Minr_Man_t * p, int ObjId, int Frame) { return Abc_Var2Lit(Minr_GetVar(p, ObjId, Frame), 0); }
static inline int Lit_F(Minr_Man_t * p, int ObjId, int Frame) { return Abc_Var2Lit(Minr_GetVar(p, ObjId, Frame) + 1, 0); }

// Kleene Logic Helpers
static inline int Minr_KleeneAnd(int v0, int v1) {
    if (v0 == MINR_VAL_0 || v1 == MINR_VAL_0) return MINR_VAL_0;
    if (v0 == MINR_VAL_1 && v1 == MINR_VAL_1) return MINR_VAL_1;
    return MINR_VAL_X;
}
static inline int Minr_Not(int v) {
    if (v == MINR_VAL_0) return MINR_VAL_1;
    if (v == MINR_VAL_1) return MINR_VAL_0;
    return MINR_VAL_X;
}

// Reset ABC PRNG and skip (user_seed * MINR_RANDOM_SKIP_MULT) draws; user_seed==0 skips none.
// Abc_Random's first arg is a reset flag, not a numeric seed — see utilSort.c.
static void Minr_RandomSeedStreamFromUserSeed( int userSeed )
{
    Abc_Random( 1 );
    if ( userSeed > 0 )
    {
        unsigned long long nSkip = (unsigned long long)(unsigned)userSeed * (unsigned long long)MINR_RANDOM_SKIP_MULT;
        for ( unsigned long long i = 0; i < nSkip; i++ )
            Abc_Random( 0 );
    }
}

// Random bit from current ABC stream (caller must have seeded via Minr_RandomSeedStreamFromUserSeed if needed).
static inline int Minr_RandomBinary( void )
{
    return (Abc_Random( 0 ) & 1) ? MINR_VAL_1 : MINR_VAL_0;
}

// Forward declaration (used by helpers below)
void Minr_SimulateTimeframe(Gia_Man_t * pGia, Vec_Int_t * vObjVals);

// Derive target reset string by multi-frame random simulation.
// If nFramesToSim == 0: directly generate random state (no simulation)
// Otherwise: simulate nFramesToSim timeframes with random PI and state transitions
//   - Initial state: random binary (or from pRoInitStr if given)
//   - Each timeframe: random PI -> simulate -> use RI as next state
// The final state (after nFramesToSim) becomes the target reset value.
// Caller must free the returned string.
static char * Minr_DeriveTargetResetByRandomSim( Gia_Man_t * pGia, char * pRoInitStr, int nFramesToSim, int vLevel, int seed )
{
    int nRegs = Gia_ManRegNum(pGia);
    Vec_Int_t * vObjVals = Vec_IntStart( Gia_ManObjNum(pGia) );
    Vec_Int_t * vCurrentState = Vec_IntAlloc(nRegs);
    Gia_Obj_t * pObj;
    int iObj, k, t;

    Minr_RandomSeedStreamFromUserSeed( seed );

    if ( nFramesToSim == 0 )
    {
        // No simulation: directly generate random state
        char * pTarget = (char *)malloc( (size_t)(nRegs + 1) );
        if ( pTarget == NULL )
        {
            Vec_IntFree( vObjVals );
            Vec_IntFree( vCurrentState );
            return NULL;
        }
        k = 0;
        Gia_ManForEachRo( pGia, pObj, iObj )
        {
            if ( pRoInitStr && k < nRegs )
            {
                pTarget[k] = pRoInitStr[k];
                k++;
            }
            else
            {
                pTarget[k++] = Minr_RandomBinary() ? '1' : '0';
            }
        }
        pTarget[nRegs] = '\0';
        if ( vLevel > 0 )
            printf( "[Rand] Generated random target reset (no sim): %s\n", pTarget );
        Vec_IntFree( vObjVals );
        Vec_IntFree( vCurrentState );
        return pTarget;
    }

    // Initialize state: random binary or from pRoInitStr
    k = 0;
    Gia_ManForEachRo( pGia, pObj, iObj )
    {
        int Val;
        if ( pRoInitStr && k < nRegs )
        {
            char c = pRoInitStr[k];
            Val = (c == '1') ? MINR_VAL_1 : (c == '0') ? MINR_VAL_0 : MINR_VAL_X;
        }
        else
        {
            Val = Minr_RandomBinary() ? MINR_VAL_1 : MINR_VAL_0;
        }
        Vec_IntPush( vCurrentState, Val );
        Vec_IntWriteEntry( vObjVals, Gia_ObjId(pGia, pObj), Val );
        k++;
    }

    // Simulate nFramesToSim timeframes
    for ( t = 0; t < nFramesToSim; t++ )
    {
        // Set random PIs for this timeframe
        Gia_ManForEachPi( pGia, pObj, iObj )
            Vec_IntWriteEntry( vObjVals, Gia_ObjId(pGia, pObj), Minr_RandomBinary() );

        // Set ROs from current state
        k = 0;
        Gia_ManForEachRo( pGia, pObj, iObj )
            Vec_IntWriteEntry( vObjVals, Gia_ObjId(pGia, pObj), Vec_IntEntry(vCurrentState, k++) );

        // Simulate one timeframe
        Minr_SimulateTimeframe( pGia, vObjVals );

        // Update state: collect RIs as next state
        Vec_IntClear( vCurrentState );
        Gia_ManForEachRi( pGia, pObj, iObj )
            Vec_IntPush( vCurrentState, Vec_IntEntry(vObjVals, Gia_ObjId(pGia, pObj)) );
    }

    // Final state becomes target reset value
    char * pTarget = (char *)malloc( (size_t)(nRegs + 1) );
    if ( pTarget == NULL )
    {
        Vec_IntFree( vObjVals );
        Vec_IntFree( vCurrentState );
        return NULL;
    }
    k = 0;
    Vec_IntForEachEntry( vCurrentState, iObj, k )
    {
        int Val = iObj;
        pTarget[k] = (Val == MINR_VAL_1) ? '1' : (Val == MINR_VAL_0) ? '0' : 'x';
    }
    pTarget[nRegs] = '\0';

    if ( vLevel > 0 )
        printf( "[Rand] Derived target reset by %d-frame random sim: %s\n", nFramesToSim, pTarget );

    Vec_IntFree( vObjVals );
    Vec_IntFree( vCurrentState );
    return pTarget;
}

////////////////////////////////////////////////////////////////////////
///                     CORE SIMULATION KERNEL                       ///
////////////////////////////////////////////////////////////////////////

/**
 * Minr_SimulateTimeframe
 * * The unified simulation function.
 * Pre-condition: Caller must set values for Constant0, PIs, and ROs in vObjVals.
 * This function computes values for all Internal Nodes (AND) and COs (PO/RI).
 */
void Minr_SimulateTimeframe(Gia_Man_t * pGia, Vec_Int_t * vObjVals) {
    int iObj;
    Gia_Obj_t * pObj;

    // Ensure Constant 0 is set correctly (just in case)
    Vec_IntWriteEntry(vObjVals, 0, MINR_VAL_0);

    // Propagate: ANDs and COs in topological order
    // Note: GIA guarantees topological order for internal nodes.
    Gia_ManForEachObj(pGia, pObj, iObj) {
        if (Gia_ObjIsCi(pObj) || iObj == 0) continue; // Skip CIs (set by caller) and Const0

        int Val = MINR_VAL_X;
        
        if (Gia_ObjIsAnd(pObj)) {
            int iFan0 = Gia_ObjFaninId0(pObj, iObj);
            int iFan1 = Gia_ObjFaninId1(pObj, iObj);
            int v0 = Vec_IntEntry(vObjVals, iFan0);
            int v1 = Vec_IntEntry(vObjVals, iFan1);
            if (Gia_ObjFaninC0(pObj)) v0 = Minr_Not(v0);
            if (Gia_ObjFaninC1(pObj)) v1 = Minr_Not(v1);
            Val = Minr_KleeneAnd(v0, v1);
        }
        else if (Gia_ObjIsCo(pObj)) { // POs and RIs
            int iFan0 = Gia_ObjFaninId0(pObj, iObj);
            int v0 = Vec_IntEntry(vObjVals, iFan0);
            if (Gia_ObjFaninC0(pObj)) v0 = Minr_Not(v0);
            Val = v0;
        }
        
        Vec_IntWriteEntry(vObjVals, iObj, Val);
    }
}

////////////////////////////////////////////////////////////////////////
///                  PROPAGATION & CUT SELECTION                     ///
////////////////////////////////////////////////////////////////////////

/**
 * Minr_ExtractCut - Extract constraint cut via reverse DFS from COs.
 *
 * For each CO (PO/RI):
 *   - If its value is known (0/1): add the CO itself to the cut.
 *   - If its value is X: DFS backwards through fanins; when a known-valued
 *     node is reached, add it to the cut and stop (don't go deeper).
 *
 * This produces a tighter cut than the forward-pass approach: only nodes
 * whose values are actually observable from some CO are constrained.
 *
 * Requires: p->vPropVals already populated (by propagation).
 * Modifies: p->vCutNodes (cleared and repopulated).
 */
void Minr_ExtractCut(Minr_Man_t * p) {
    Gia_Man_t * pGia = p->pGia;
    int nObjs = Gia_ManObjNum(pGia);
    Gia_Obj_t * pObj;
    int iObj;

    if (!p->vCutNodes)
        p->vCutNodes = Vec_IntAlloc(100);
    else
        Vec_IntClear(p->vCutNodes);

    Vec_Int_t * vCutFlags = Vec_IntStart(nObjs);
    Vec_Int_t * vVisited  = Vec_IntStart(nObjs);
    Vec_Int_t * vStack    = Vec_IntAlloc(256);

    Gia_ManForEachCo(pGia, pObj, iObj) {
        int coId = Gia_ObjId(pGia, pObj);
        int coVal = Vec_IntEntry(p->vPropVals, coId);

        if (coVal != MINR_VAL_X) {
            Vec_IntWriteEntry(vCutFlags, coId, 1);
            continue;
        }

        // X-valued CO: reverse DFS from its driver to find known frontier
        Vec_IntClear(vStack);
        Vec_IntPush(vStack, Gia_ObjFaninId0(pObj, coId));

        while (Vec_IntSize(vStack) > 0) {
            int nodeId = Vec_IntPop(vStack);
            if (nodeId == 0) continue;
            if (Vec_IntEntry(vVisited, nodeId)) continue;
            Vec_IntWriteEntry(vVisited, nodeId, 1);

            int val = Vec_IntEntry(p->vPropVals, nodeId);
            if (val != MINR_VAL_X) {
                Vec_IntWriteEntry(vCutFlags, nodeId, 1);
                continue;
            }

            Gia_Obj_t * pNode = Gia_ManObj(pGia, nodeId);
            if (Gia_ObjIsAnd(pNode)) {
                Vec_IntPush(vStack, Gia_ObjFaninId0(pNode, nodeId));
                Vec_IntPush(vStack, Gia_ObjFaninId1(pNode, nodeId));
            }
        }
    }

    Gia_ManForEachObj(pGia, pObj, iObj) {
        if (Vec_IntEntry(vCutFlags, iObj))
            Vec_IntPush(p->vCutNodes, iObj);
    }

    Vec_IntFree(vCutFlags);
    Vec_IntFree(vVisited);
    Vec_IntFree(vStack);
}

/**
 * Minr_ExtractEqCut - Extract eq cut for mode 3 refinement.
 *
 * Eq cut = nodes closest to outputs that depend only on constraint cut,
 * specified registers, and PIs (i.e. not on free register cone).
 *
 * Algorithm:
 * 1. From each free RO, forward traverse toward outputs. Mark all visited
 *    nodes. STOP when we hit the constraint cut (do not mark, do not cross).
 * 2. From each CO, reverse DFS. When we hit an unmarked node, add it to
 *    eq cut and stop (don't go deeper).
 *
 * Requires: p->vCutNodes (constraint cut), p->vPropVals, p->pInitStr.
 * Modifies: p->vEqCutNodes (cleared and repopulated).
 */
void Minr_ExtractEqCut(Minr_Man_t * p) {
    Gia_Man_t * pGia = p->pGia;
    int nObjs = Gia_ManObjNum(pGia);
    Gia_Obj_t * pObj;
    int iObj;

    if (!p->vEqCutNodes)
        p->vEqCutNodes = Vec_IntAlloc(100);
    else
        Vec_IntClear(p->vEqCutNodes);

    Vec_Int_t * vCutSet = Vec_IntStart(nObjs);
    int NodeId, ci;
    Vec_IntForEachEntry(p->vCutNodes, NodeId, ci)
        Vec_IntWriteEntry(vCutSet, NodeId, 1);

    Vec_Int_t * vMarked = Vec_IntStart(nObjs);

    Gia_ManStaticFanoutStart(pGia);

    Vec_Int_t * vQueue = Vec_IntAlloc(256);
    int k = 0;
    Gia_ManForEachRo(pGia, pObj, iObj) {
        char c = p->pInitStr[k++];
        if (c == '0' || c == '1') continue;
        int roId = Gia_ObjId(pGia, pObj);
        Vec_IntPush(vQueue, roId);
        Vec_IntWriteEntry(vMarked, roId, 1);
    }

    while (Vec_IntSize(vQueue) > 0) {
        int nodeId = Vec_IntPop(vQueue);
        if (Vec_IntEntry(vCutSet, nodeId)) continue;
        Gia_Obj_t * pNode = Gia_ManObj(pGia, nodeId);
        int i;
        Gia_Obj_t * pFanout;
        Gia_ObjForEachFanoutStatic(pGia, pNode, pFanout, i) {
            int foId = Gia_ObjId(pGia, pFanout);
            if (Vec_IntEntry(vCutSet, foId)) continue;
            if (Vec_IntEntry(vMarked, foId)) continue;
            Vec_IntWriteEntry(vMarked, foId, 1);
            Vec_IntPush(vQueue, foId);
        }
    }

    Gia_ManStaticFanoutStop(pGia);
    Vec_IntFree(vQueue);

    Vec_Int_t * vEqCutFlags = Vec_IntStart(nObjs);
    Vec_Int_t * vVisited = Vec_IntStart(nObjs);
    Vec_Int_t * vStack = Vec_IntAlloc(256);

    Gia_ManForEachCo(pGia, pObj, iObj) {
        int coId = Gia_ObjId(pGia, pObj);
        Vec_IntClear(vStack);
        Vec_IntPush(vStack, Gia_ObjFaninId0(pObj, coId));

        while (Vec_IntSize(vStack) > 0) {
            int nodeId = Vec_IntPop(vStack);
            if (nodeId == 0) continue;
            if (Vec_IntEntry(vVisited, nodeId)) continue;
            Vec_IntWriteEntry(vVisited, nodeId, 1);

            if (!Vec_IntEntry(vMarked, nodeId)) {
                Vec_IntWriteEntry(vEqCutFlags, nodeId, 1);
                continue;
            }

            Gia_Obj_t * pNode = Gia_ManObj(pGia, nodeId);
            if (Gia_ObjIsAnd(pNode)) {
                Vec_IntPush(vStack, Gia_ObjFaninId0(pNode, nodeId));
                Vec_IntPush(vStack, Gia_ObjFaninId1(pNode, nodeId));
            }
        }
    }

    Gia_ManForEachObj(pGia, pObj, iObj) {
        if (Vec_IntEntry(vEqCutFlags, iObj))
            Vec_IntPush(p->vEqCutNodes, iObj);
    }

    Vec_IntFree(vCutSet);
    Vec_IntFree(vMarked);
    Vec_IntFree(vEqCutFlags);
    Vec_IntFree(vVisited);
    Vec_IntFree(vStack);
}

void Minr_PropagateAndCut(Minr_Man_t * p) {
    Gia_Man_t * pGia = p->pGia;
    int iObj;
    Gia_Obj_t * pObj;

    // 1. Initialize Values vector (free old if re-entering, e.g. -O 2 outer loop)
    if (p->vPropVals) Vec_IntFree(p->vPropVals);
    p->vPropVals = Vec_IntStart(Gia_ManObjNum(pGia));
    
    // 2. Setup Inputs for Propagation (Scenario 2: RO=-I, PI=X)
    
    // Set PIs to X
    Gia_ManForEachPi(pGia, pObj, iObj)
        Vec_IntWriteEntry(p->vPropVals, Gia_ObjId(pGia, pObj), MINR_VAL_X);

    // Set ROs from -I
    int k = 0;
    Gia_ManForEachRo(pGia, pObj, iObj) {
        char c = p->pInitStr[k++];
        int Val = MINR_VAL_X;
        if (c == '0') Val = MINR_VAL_0;
        else if (c == '1') Val = MINR_VAL_1;
        Vec_IntWriteEntry(p->vPropVals, Gia_ObjId(pGia, pObj), Val);
    }

    // 3. Run Core Simulation
    Minr_SimulateTimeframe(pGia, p->vPropVals);

    // 4. Cut Selection (Reverse DFS from COs)
    Minr_ExtractCut(p);

    // Compute specRoCutRatio: % of specified (0/1) ROs in the cut
    {
        Vec_Int_t * vCutSet = Vec_IntStart(Gia_ManObjNum(pGia));
        int NodeId, ci;
        Vec_IntForEachEntry(p->vCutNodes, NodeId, ci)
            Vec_IntWriteEntry(vCutSet, NodeId, 1);
        int nSpec = 0, nSpecInCut = 0;
        k = 0;
        Gia_ManForEachRo(pGia, pObj, iObj) {
            char c = p->pInitStr[k++];
            if (c == '0' || c == '1') {
                nSpec++;
                if (Vec_IntEntry(vCutSet, Gia_ObjId(pGia, pObj)))
                    nSpecInCut++;
            }
        }
        p->specRoCutRatio = (nSpec > 0) ? 100.0 * nSpecInCut / nSpec : 0.0;
        Vec_IntFree(vCutSet);
        if (p->vLevel > 0)
            printf("[Prop] Specified ROs in cut: %d/%d (%.2f%%)\n",
                   nSpecInCut, nSpec, p->specRoCutRatio);
    }

    if (p->vLevel > 0) {
        printf("[Prop] Cut size: %d nodes.\n", Vec_IntSize(p->vCutNodes));
    }
    if (p->vLevel > 1) {
        int i, NodeId;
        Gia_Obj_t * pCutObj;
        Vec_IntForEachEntry(p->vCutNodes, NodeId, i) {
            pCutObj = Gia_ManObj(pGia, NodeId);
            const char * pType = Gia_ObjIsRo(pGia, pCutObj) ? "RO" :
                                Gia_ObjIsPo(pGia, pCutObj) ? "PO" :
                                Gia_ObjIsRi(pGia, pCutObj) ? "RI" : "AND";
            printf("  Cut[%d]: ObjId=%d type=%s Val=%d\n", i, NodeId, pType, Vec_IntEntry(p->vPropVals, NodeId));
        }
    }
}

////////////////////////////////////////////////////////////////////////
///                     CNF GENERATION UTILS                         ///
////////////////////////////////////////////////////////////////////////

void Minr_AddClause(Minr_Man_t * p, Vec_Int_t * vLits) {
    Vec_Int_t * v = Vec_WecPushLevel(p->vClauses);
    Vec_IntAppend(v, vLits);
}
void Minr_AddClause1(Minr_Man_t * p, int L1) {
    Vec_Int_t * v = Vec_WecPushLevel(p->vClauses);
    Vec_IntPush(v, L1);
}
void Minr_AddClause2(Minr_Man_t * p, int L1, int L2) {
    Vec_Int_t * v = Vec_WecPushLevel(p->vClauses);
    Vec_IntPush(v, L1);
    Vec_IntPush(v, L2);
}
void Minr_AddClause3(Minr_Man_t * p, int L1, int L2, int L3) {
    Vec_Int_t * v = Vec_WecPushLevel(p->vClauses);
    Vec_IntPush(v, L1);
    Vec_IntPush(v, L2);
    Vec_IntPush(v, L3);
}
void Minr_AddIllegalStateCheck(Minr_Man_t * p, int ObjId, int Frame) {
    Minr_AddClause2(p, Abc_LitNot(Lit_T(p, ObjId, Frame)), Abc_LitNot(Lit_F(p, ObjId, Frame)));
}
void Minr_AddBinaryConstraint(Minr_Man_t * p, int ObjId, int Frame) {
    Minr_AddClause2(p, Lit_T(p, ObjId, Frame), Lit_F(p, ObjId, Frame));
}
void Minr_AddUnknownConstraint(Minr_Man_t * p, int ObjId, int Frame) {
    Minr_AddClause1(p, Abc_LitNot(Lit_T(p, ObjId, Frame)));
    Minr_AddClause1(p, Abc_LitNot(Lit_F(p, ObjId, Frame)));
}
void Minr_AddAnd(Minr_Man_t * p, int iObj, int iFan0, int iFan1, int fCompl0, int fCompl1, int Frame) {
    int a_T = fCompl0 ? Lit_F(p, iFan0, Frame) : Lit_T(p, iFan0, Frame);
    int a_F = fCompl0 ? Lit_T(p, iFan0, Frame) : Lit_F(p, iFan0, Frame);
    int b_T = fCompl1 ? Lit_F(p, iFan1, Frame) : Lit_T(p, iFan1, Frame);
    int b_F = fCompl1 ? Lit_T(p, iFan1, Frame) : Lit_F(p, iFan1, Frame);
    int z_T = Lit_T(p, iObj, Frame);
    int z_F = Lit_F(p, iObj, Frame);
    Minr_AddClause2(p, Abc_LitNot(z_T), a_T);
    Minr_AddClause2(p, Abc_LitNot(z_T), b_T);
    Minr_AddClause3(p, Abc_LitNot(a_T), Abc_LitNot(b_T), z_T);
    Minr_AddClause2(p, Abc_LitNot(a_F), z_F);
    Minr_AddClause2(p, Abc_LitNot(b_F), z_F);
    Minr_AddClause3(p, Abc_LitNot(z_F), a_F, b_F);
}
void Minr_AddCoBuffer(Minr_Man_t * p, int iCo, int iDriver, int fCompl, int Frame) {
    int d_T = fCompl ? Lit_F(p, iDriver, Frame) : Lit_T(p, iDriver, Frame);
    int d_F = fCompl ? Lit_T(p, iDriver, Frame) : Lit_F(p, iDriver, Frame);
    int z_T = Lit_T(p, iCo, Frame);
    int z_F = Lit_F(p, iCo, Frame);
    Minr_AddClause2(p, Abc_LitNot(z_T), d_T);
    Minr_AddClause2(p, Abc_LitNot(d_T), z_T);
    Minr_AddClause2(p, Abc_LitNot(z_F), d_F);
    Minr_AddClause2(p, Abc_LitNot(d_F), z_F);
}
void Minr_AddEquiv(Minr_Man_t * p, int iObjTo, int iObjFrom, int FrameTo, int FrameFrom) {
    int to_T = Lit_T(p, iObjTo, FrameTo);
    int to_F = Lit_F(p, iObjTo, FrameTo);
    int from_T = Lit_T(p, iObjFrom, FrameFrom);
    int from_F = Lit_F(p, iObjFrom, FrameFrom);
    Minr_AddClause2(p, Abc_LitNot(to_T), from_T);
    Minr_AddClause2(p, Abc_LitNot(from_T), to_T);
    Minr_AddClause2(p, Abc_LitNot(to_F), from_F);
    Minr_AddClause2(p, Abc_LitNot(from_F), to_F);
}

////////////////////////////////////////////////////////////////////////
///                    SOLVER IO & DECODING                          ///
////////////////////////////////////////////////////////////////////////

static Vec_Int_t * Minr_CallSolverIpamir( Minr_Man_t * p, Vec_Wec_t * vHardClauses, Vec_Int_t * vSoftLits, const char * pSoPath, double timeoutSec )
{
    Minr_IpamirApi_t Api;
    if ( !Minr_IpamirApiLoad( &Api, pSoPath ) )
    {
        p->solverStatus = 3;
        return NULL;
    }

    void * s = Api.ipamir_init();
    if ( !s )
    {
        printf("[Minr] ipamir_init() failed.\n");
        Minr_IpamirApiUnload( &Api );
        p->solverStatus = 3;
        return NULL;
    }

    struct Minr_TermState_t {
        abctime startCpu;
        abctime limitCpu; // CPU ticks (CLOCKS_PER_SEC based)
    };
    auto TermCb = [](void * pState) -> int {
        auto * st = (Minr_TermState_t *)pState;
        return (Abc_Clock() - st->startCpu) >= st->limitCpu;
    };
    Minr_TermState_t TermState;

    // Hard clauses (CNF store uses ABC literals; convert to DIMACS signed literals).
    Vec_Int_t * vC; int k, Lit, i;
    Vec_WecForEachLevel( vHardClauses, vC, k )
    {
        Vec_IntForEachEntry( vC, Lit, i )
        {
            int Var = Abc_Lit2Var( Lit );
            int DimacsLit = Abc_LitIsCompl( Lit ) ? -Var : Var;
            Api.ipamir_add_hard( s, (int32_t)DimacsLit );
        }
        Api.ipamir_add_hard( s, 0 );
    }

    // Soft literals: Minr objective uses soft unit clause (u) weight=1, which prefers u=1 (unknown).
    // EvalMaxSAT2022's glue maps the sign to preferred assignment; passing (-Var) encourages Var=true there.
    int Soft;
    Vec_IntForEachEntry( vSoftLits, Soft, k )
    {
        int Var = Abc_Lit2Var( Soft );
        Api.ipamir_add_soft_lit( s, (int32_t)(-Var), (uint64_t)1 );
    }

    // Attach time limit if requested (timeoutSec > 0).
    if ( Api.ipamir_set_terminate && timeoutSec > 0 ) {
        TermState.startCpu = Abc_Clock();
        TermState.limitCpu = (abctime)(timeoutSec * (double)CLOCKS_PER_SEC);
        Api.ipamir_set_terminate( s, &TermState, TermCb );
    }

    abctime clk = Abc_Clock();
    int32_t st = Api.ipamir_solve( s );
    p->timeSolver = Abc_Clock() - clk;

    if ( st == 0 )
    {
        printf("[Minr] Solver timed out (%.1f sec CPU limit).\n", timeoutSec);
        p->solverStatus = 4;
        Api.ipamir_release( s );
        Minr_IpamirApiUnload( &Api );
        return NULL;
    }

    if ( st == 20 )
    {
        p->solverStatus = 2;
        Api.ipamir_release( s );
        Minr_IpamirApiUnload( &Api );
        return NULL;
    }
    if ( st != 30 )
    {
        p->solverStatus = 3;
        printf("[Minr] Unexpected IPAMIR solve status: %d\n", st);
        Api.ipamir_release( s );
        Minr_IpamirApiUnload( &Api );
        return NULL;
    }

    p->solverStatus = 1;

    Vec_Int_t * vModel = Vec_IntStart( p->nSatVars + 1 );
    for ( int v = 1; v <= p->nSatVars; v++ )
    {
        int32_t val = Api.ipamir_val_lit( s, v );
        if ( val == v ) Vec_IntWriteEntry( vModel, v, 1 );
        else if ( val == -v ) Vec_IntWriteEntry( vModel, v, 0 );
    }

    Api.ipamir_release( s );
    Minr_IpamirApiUnload( &Api );
    return vModel;
}

Vec_Int_t * Minr_CallSolver(Minr_Man_t * p, char * pFileName, char * pSolverPath, double timeoutSec) {
    char LogFile[1000];
    sprintf(LogFile, "%s.log", pFileName);

    char * pPath = pSolverPath ? pSolverPath : (char *)"third_party/EvalMaxSAT2022/libipamirEvalMaxSAT2022.so";
    if (access(pPath, F_OK) == -1) {
        printf("[Minr] Solver binary not found: %s\n", pPath);
        p->solverStatus = 3;
        return NULL;
    }

    // Run external solver with CPU-time limit (timeoutSec). If timeoutSec == 0, run unlimited.
    // CPU time here refers to the child process CPU usage (utime+stime).
    pid_t pid;
    abctime clk = Abc_Clock();
    {
        pid = fork();
        if (pid == -1) {
            perror("[Minr] fork");
            p->solverStatus = 3;
            return NULL;
        }
        if (pid == 0) {
            // child: redirect stdout to log file, then exec solver
            FILE * f = freopen(LogFile, "w", stdout);
            (void)f;
            // also redirect stderr to stdout for easier debug
            dup2(fileno(stdout), fileno(stderr));
            execl(pPath, pPath, pFileName, (char *)NULL);
            perror("[Minr] exec");
            _exit(127);
        }
    }

    long hz = sysconf(_SC_CLK_TCK);
    long long cpuLimitTicks = (timeoutSec > 0) ? (long long)(timeoutSec * (double)hz) : -1;
    int status = 0;
    int timedOut = 0;
    for (;;) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        if (w == -1) {
            perror("[Minr] waitpid");
            p->solverStatus = 3;
            break;
        }

        if (cpuLimitTicks >= 0) {
            // read /proc/<pid>/stat for utime+stime
            char path[128];
            sprintf(path, "/proc/%d/stat", (int)pid);
            FILE * fp = fopen(path, "r");
            if (fp) {
                // format: pid (comm) state ppid ... utime stime ...
                int rpid = 0;
                char comm[256];
                char state = 0;
                // read first 3 fields; comm may contain spaces but is wrapped in ()
                if (fscanf(fp, "%d %255s %c", &rpid, comm, &state) == 3) {
                    // skip fields 4..13 (10 ints/lls), then read utime/stime (14/15)
                    long long dummy;
                    for (int i = 0; i < 10; i++) {
                        if (fscanf(fp, "%lld", &dummy) != 1) { dummy = 0; break; }
                    }
                    unsigned long long ut = 0, stt = 0;
                    if (fscanf(fp, "%llu %llu", &ut, &stt) == 2) {
                        long long used = (long long)(ut + stt);
                        if (used >= cpuLimitTicks) {
                            timedOut = 1;
                            kill(pid, SIGKILL);
                        }
                    }
                }
                fclose(fp);
            }
        }
        // 10ms polling
        usleep(10000);
    }

    p->timeSolver = Abc_Clock() - clk;
    if (p->vLevel > 0) Abc_PrintTime(1, "Solver runtime", p->timeSolver);

    if (timedOut) {
        printf("[Minr] Solver timed out (%.1f sec CPU limit).\n", timeoutSec);
        p->solverStatus = 4;
        return NULL;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) != 30) {
        // old code expected 7680 from system(); now we see raw exit code.
        if (p->vLevel > 0) printf("[Minr] Solver exit code %d\n", WEXITSTATUS(status));
    }

    FILE * pFile = fopen(LogFile, "r");
    if (pFile == NULL) {
        printf("[Minr] Cannot open solver log file: %s\n", LogFile);
        return NULL;
    }

    Vec_Int_t * vModel = NULL;
    char LineBuf[1024];
    int fStatusFound = 0;

    // First pass: find "s " status line and determine assignment format
    int fStandardDimacs = -1;  // -1 = unknown, 0 = bitstring, 1 = standard DIMACS
    while (fgets(LineBuf, sizeof(LineBuf), pFile)) {
        if (strncmp(LineBuf, "s ", 2) == 0 && !fStatusFound) {
            fStatusFound = 1;
            if (strstr(LineBuf, "OPTIMUM FOUND")) {
                p->solverStatus = 1;
            } else if (strstr(LineBuf, "UNSATISFIABLE")) {
                p->solverStatus = 2;
                printf("[Minr] Problem is UNSATISFIABLE.\n");
                fclose(pFile);
                return NULL;
            } else {
                p->solverStatus = 3;
                LineBuf[strcspn(LineBuf, "\r\n")] = '\0';
                printf("[Minr] Unexpected solver status: \"%s\"\n", LineBuf);
                fclose(pFile);
                return NULL;
            }
        }
        else if (strncmp(LineBuf, "v ", 2) == 0 && fStandardDimacs == -1) {
            char * pStr = LineBuf + 2;
            for (int k = 0; pStr[k] && pStr[k] != '\n' && pStr[k] != '\r'; k++) {
                if (pStr[k] == ' ') { fStandardDimacs = 1; break; }
            }
            if (fStandardDimacs == -1) fStandardDimacs = 0;
        }
    }

    if (!fStatusFound) {
        p->solverStatus = 3;
        printf("[Minr] No status line (\"s ...\") found in solver output.\n");
        fclose(pFile);
        return NULL;
    }

    // Second pass: decode assignment
    rewind(pFile);
    vModel = Vec_IntStart(p->nSatVars + 1);

    if (p->vLevel > 1) printf("[Minr] fStandardDimacs: %d\n", fStandardDimacs);

    while (fgets(LineBuf, sizeof(LineBuf), pFile)) {
        if (strncmp(LineBuf, "v ", 2) == 0) {
            if (fStandardDimacs == 1) {
                char * pStr = LineBuf + 2;
                char * pToken = strtok(pStr, " \t\r\n");
                while (pToken) {
                    int Lit = atoi(pToken);
                    if (Lit != 0) {
                        int Var = abs(Lit);
                        if (Var <= p->nSatVars)
                            Vec_IntWriteEntry(vModel, Var, (Lit > 0) ? 1 : 0);
                    }
                    pToken = strtok(NULL, " \t\r\n");
                }
            } else {
                int offset = 0;
                int isFirstLine = 1;
                while (offset < p->nSatVars) {
                    char * pStr = isFirstLine ? (LineBuf + 2) : LineBuf;
                    int i = 0;
                    while (pStr[i] && pStr[i] != '\n' && pStr[i] != '\r') {
                        if (offset + 1 > p->nSatVars) break;
                        Vec_IntWriteEntry(vModel, offset + 1, (pStr[i] == '1') ? 1 : 0);
                        offset++;
                        i++;
                    }
                    if (offset >= p->nSatVars) break;
                    if (pStr[i] == '\n' || pStr[i] == '\r') break;
                    if (!fgets(LineBuf, sizeof(LineBuf), pFile)) break;
                    if (strncmp(LineBuf, "v ", 2) == 0) break;
                    isFirstLine = 0;
                }
            }
        }
    }
    fclose(pFile);
    return vModel;
}

// Scenario 1: Verify the result by simulation
// Uses decoded PI sequence / reset values when available; otherwise falls back to vModel.
int Minr_VerifyResult(Minr_Man_t * p, Vec_Int_t * vModel) {
    Gia_Man_t * pGia = p->pGia;
    int iObj;
    Gia_Obj_t * pObj;

    // 1. Extract Initial State (Reset) from Model at t=0
    // Note: If U_i=1 (Unknown), we set RO to X. Else check T/F.
    Vec_Int_t * vCurrentState = Vec_IntAlloc(Gia_ManRegNum(pGia));
    
    if (p->vRoVals0) {
        // Prefer decoded reset values at t=0
        int i;
        Vec_IntForEachEntry(p->vRoVals0, iObj, i) {
            Vec_IntPush(vCurrentState, iObj);
        }
    } else {
        Gia_ManForEachRo(pGia, pObj, iObj) {
            int iId = Gia_ObjId(pGia, pObj);
            int varT = Minr_GetVar(p, iId, 0);
            int varF = varT + 1;
            int valT = Vec_IntEntry(vModel, varT);
            int valF = Vec_IntEntry(vModel, varF);
            
            int Val = MINR_VAL_X;
            if (valT == 1 && valF == 0) Val = MINR_VAL_1;
            else if (valT == 0 && valF == 1) Val = MINR_VAL_0;
            // Else (0,0) -> X
            
            Vec_IntPush(vCurrentState, Val);
        }
    }

    // 2. Simulate t=0 to t=k
    Vec_Int_t * vObjVals = Vec_IntStart(Gia_ManObjNum(pGia));
    int t;
    
    for (t = 0; t <= p->nFrames; t++) {
        // Set PIs
        if (t < p->nFrames) {
            // t < k: Use PIs from decoded sequence when available (binary 0/1)
            if (p->vPiVals) {
                int iPi, nPi = Gia_ManPiNum(pGia);
                Gia_ManForEachPi(pGia, pObj, iPi) {
                    int iId = Gia_ObjId(pGia, pObj);
                    int Val = Vec_IntEntry(p->vPiVals, t * nPi + iPi) ? MINR_VAL_1 : MINR_VAL_0;
                    Vec_IntWriteEntry(vObjVals, iId, Val);
                }
            } else {
                // fallback: read from solver model
                Gia_ManForEachPi(pGia, pObj, iObj) {
                    int iId = Gia_ObjId(pGia, pObj);
                    int varT = Minr_GetVar(p, iId, t);
                    int varF = varT + 1;
                    int valT = Vec_IntEntry(vModel, varT);
                    int valF = Vec_IntEntry(vModel, varF);
                    int Val = MINR_VAL_X;
                    if (valT == 1 && valF == 0) Val = MINR_VAL_1;
                    else if (valT == 0 && valF == 1) Val = MINR_VAL_0;
                    Vec_IntWriteEntry(vObjVals, iId, Val);
                }
            }
        } else {
            // t == k: PIs from vPiAtK if set (-O 2 iter>1), else X
            if (p->vPiAtK) {
                int iPi;
                Gia_ManForEachPi(pGia, pObj, iPi) {
                    int Val = Vec_IntEntry(p->vPiAtK, iPi) ? MINR_VAL_1 : MINR_VAL_0;
                    Vec_IntWriteEntry(vObjVals, Gia_ObjId(pGia, pObj), Val);
                }
            } else {
                Gia_ManForEachPi(pGia, pObj, iObj)
                    Vec_IntWriteEntry(vObjVals, Gia_ObjId(pGia, pObj), MINR_VAL_X);
            }
        }

        // Set ROs from vCurrentState
        int k = 0;
        Gia_ManForEachRo(pGia, pObj, iObj) {
            Vec_IntWriteEntry(vObjVals, Gia_ObjId(pGia, pObj), Vec_IntEntry(vCurrentState, k++));
        }

        // Run Simulation
        Minr_SimulateTimeframe(pGia, vObjVals);

        // If not last frame, capture RIs for next state
        if (t < p->nFrames) {
            k = 0;
            Gia_ManForEachRi(pGia, pObj, iObj) {
                int Val = Vec_IntEntry(vObjVals, Gia_ObjId(pGia, pObj));
                Vec_IntWriteEntry(vCurrentState, k++, Val);
            }
        }
    }

    // 2b. Register mismatch stat at t=k (only for specified 0/1 registers)
    // weak = X vs 0/1 (don't-care utilized); strong = 0 vs 1 (actual conflict)
    {
        int nRegs = Gia_ManRegNum(pGia);
        int nSpec = 0, nWeak = 0, nStrong = 0;
        for (int ri = 0; ri < nRegs; ri++) {
            char tc = p->pInitStr[ri];
            if (tc != '0' && tc != '1') continue;
            nSpec++;
            int simVal = Vec_IntEntry(vCurrentState, ri);
            int tgtVal = (tc == '0') ? MINR_VAL_0 : MINR_VAL_1;
            if (simVal == tgtVal) continue;
            if (simVal == MINR_VAL_X)
                nWeak++;
            else
                nStrong++;
        }
        p->simRegMismatchWeakPct   = (nSpec > 0) ? 100.0 * nWeak / nSpec : 0.0;
        p->simRegMismatchStrongPct = (nSpec > 0) ? 100.0 * nStrong / nSpec : 0.0;
        if (p->vLevel > 0)
            printf("[Verify] Reg mismatch at t=k: weak(X vs 0/1)=%d, strong(0 vs 1)=%d, of %d specified\n",
                   nWeak, nStrong, nSpec);
    }

    // 3. Verify Cut Constraints at t=k
    int nFailures = 0;
    int i, NodeId;
    Vec_IntForEachEntry(p->vCutNodes, NodeId, i) {
        int ValTarget = Vec_IntEntry(p->vPropVals, NodeId);
        int ValSim    = Vec_IntEntry(vObjVals, NodeId);

        // If Target is Known (0/1), Sim must match.
        // Note: ValSim could be X if solver failed logic, or solver used X for irrelevant inputs.
        // But since we enforced equality on the cut, Sim SHOULD be 0/1 matching Target.
        if (ValTarget != MINR_VAL_X) {
            if (ValSim != ValTarget) {
                nFailures++;
                if (p->vLevel > 0)
                    printf("[Verify] Fail at Obj %d: Target=%d, Sim=%d\n", NodeId, ValTarget, ValSim);
            }
        }
    }

    Vec_IntFree(vCurrentState);
    Vec_IntFree(vObjVals);

    if (nFailures == 0) {
        if (p->vLevel > 0) printf("[Verify] SUCCESS. Simulation matches cut constraints.\n");
        return 1;
    } else {
        printf("[Verify] FAILED. %d mismatches found.\n", nFailures);
        return 0;
    }
}

// Scenario 3: Helper to get a random reachable state
// Returns a vector of RO values (size nRegs). Caller must free.
Vec_Int_t * Minr_GetRandomReachableState(Gia_Man_t * pGia, int nFramesToSim) {
    Vec_Int_t * vState = Vec_IntAlloc(Gia_ManRegNum(pGia));
    Vec_Int_t * vObjVals = Vec_IntStart(Gia_ManObjNum(pGia));
    
    // Init state: All 0 (or random?) - let's use all 0 as cold start
    Gia_Obj_t * pObj;
    int iObj, k;
    
    // Set initial ROs to 0/random
    Gia_ManForEachRo(pGia, pObj, iObj) 
    {
        // Vec_IntWriteEntry(vObjVals, Gia_ObjId(pGia, pObj), MINR_VAL_0);
        Vec_IntWriteEntry(vObjVals, Gia_ObjId(pGia, pObj), Minr_RandomBinary());
    }

    for (int t = 0; t < nFramesToSim; t++) {
        // Set Random PIs
        Gia_ManForEachPi(pGia, pObj, iObj)
            Vec_IntWriteEntry(vObjVals, Gia_ObjId(pGia, pObj), Minr_RandomBinary());
            
        // Sim
        Minr_SimulateTimeframe(pGia, vObjVals);
        
        // Update ROs from RIs
        Vec_IntClear(vState);
        Gia_ManForEachRi(pGia, pObj, iObj)
            Vec_IntPush(vState, Vec_IntEntry(vObjVals, Gia_ObjId(pGia, pObj)));
            
        k = 0;
        Gia_ManForEachRo(pGia, pObj, iObj)
            Vec_IntWriteEntry(vObjVals, Gia_ObjId(pGia, pObj), Vec_IntEntry(vState, k++));
    }
    
    Vec_IntFree(vObjVals);
    return vState;
}

void Minr_DecodeResult(Minr_Man_t * p, Vec_Int_t * vModel) {
    if (!vModel) return;

    printf("\n[Minr] Result Decoding:\n");
    
    // Decode PI Sequence
    if (p->nFrames == 0) printf("  PI Sequence: (k=0, no steps)\n");
    else printf("  PI Sequence (t=0..%d):\n", p->nFrames - 1);
    
    // Store decoded results for verify / later use
    if (p->vPiVals) Vec_IntFree(p->vPiVals);
    if (p->vRoVals0) Vec_IntFree(p->vRoVals0);
    p->vPiVals = Vec_IntAlloc(p->nFrames * Gia_ManPiNum(p->pGia));
    p->vRoVals0 = Vec_IntAlloc(Gia_ManRegNum(p->pGia));

    int t, i;
    Gia_Obj_t * pObj;
    for (t = 0; t < p->nFrames; t++) {
        printf("    t=%d: ", t);
        Gia_ManForEachPi(p->pGia, pObj, i) {
            int iObj = Gia_ObjId(p->pGia, pObj);
            int varT = Minr_GetVar(p, iObj, t);
            int valT = Vec_IntEntry(vModel, varT);
            int valF = Vec_IntEntry(vModel, varT + 1);
            char c = (valT && !valF) ? '1' : (!valT && valF) ? '0' : (!valT && !valF) ? 'x' : '!';
            printf("%c", c);
            // store as binary (assume constraints make it 0/1 for t<k)
            Vec_IntPush(p->vPiVals, (c == '1') ? 1 : 0);
        }
        printf("\n");
    }

    // Decode Reset
    int nUnknowns = 0, nResets = 0, nTotal = Gia_ManRegNum(p->pGia);
    Vec_Int_t * vResetIndices = Vec_IntAlloc(nTotal);
    Vec_Int_t * vResetValues  = Vec_IntAlloc(nTotal);

    Gia_ManForEachRo(p->pGia, pObj, i) {
        int varT = Minr_GetVar(p, Gia_ObjId(p->pGia, pObj), 0);
        int valT = Vec_IntEntry(vModel, varT);
        int valF = Vec_IntEntry(vModel, varT + 1);
        if (!valT && !valF) {
            nUnknowns++;
            Vec_IntPush(p->vRoVals0, MINR_VAL_X);
        } else {
            nResets++;
            Vec_IntPush(vResetIndices, i);
            Vec_IntPush(vResetValues, (valT == 1) ? 1 : 0);
            Vec_IntPush(p->vRoVals0, (valT == 1) ? MINR_VAL_1 : MINR_VAL_0);
        }
    }

    printf("\n  Reset Requirements (t=0):\n");
    printf("    Total Registers : %d\n", nTotal);
    printf("    Unknown (Free)  : %d\n", nUnknowns);
    printf("    Reset Needed    : %d\n", nResets);
    int nSpecRegs = 0;
    if (p->pInitStr) {
        for (int ri = 0; ri < nTotal && p->pInitStr[ri]; ri++)
            if (p->pInitStr[ri] == '0' || p->pInitStr[ri] == '1') nSpecRegs++;
    }
    printf("    Specified Regs  : %d\n", nSpecRegs);
    if (nTotal > 0)
        printf("    Reset Ratio     : %.2f%%\n", 100.0 * nResets / nTotal);
    if (nSpecRegs > 0)
        printf("    Reduction       : %.2f%%\n", 100.0 * (1.0 - (double)nResets / (double)nSpecRegs));
    else
        printf("    Reduction       : N/A\n");
    if (p->nRefineMode > 0) {
        int nBefore = nResets + p->nRefineReleased;
        if (nTotal > 0)
            printf("    Reset Ratio (before refine): %.2f%%\n", 100.0 * nBefore / nTotal);
        if (nSpecRegs > 0)
            printf("    Reduction   (before refine): %.2f%%\n", 100.0 * (1.0 - (double)nBefore / (double)nSpecRegs));
        else
            printf("    Reduction   (before refine): N/A\n");
    }

    
    if ( p->vLevel > 1) {
        if (nResets > 0) {
            printf("    Detailed Reset List:\n");
            int RegIdx, k;
            Vec_IntForEachEntry(vResetIndices, RegIdx, k) {
                printf("      Latch[%d] -> %d\n", RegIdx, Vec_IntEntry(vResetValues, k));
            }
        } else {
            printf("    (No registers need to be reset!)\n");
        }
    }
    Vec_IntFree(vResetIndices);
    Vec_IntFree(vResetValues);
}

////////////////////////////////////////////////////////////////////////
///                        REPORT DUMP                               ///
////////////////////////////////////////////////////////////////////////

static void Minr_DumpReport(Minr_Man_t * p)
{
    if (!p->pReportFile) return;

    FILE * pFile = fopen(p->pReportFile, "w");
    if (!pFile) {
        printf("[Minr] Cannot open report file: %s\n", p->pReportFile);
        return;
    }

    Gia_Man_t * pGia = p->pGia;
    int nRegs = Gia_ManRegNum(pGia);
    int nPI   = Gia_ManPiNum(pGia);

    // Timestamp
    time_t rawtime;
    struct tm * ti;
    char timebuf[64];
    time(&rawtime);
    ti = localtime(&rawtime);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", ti);

    // Runtime (exclude verification if timeSolveEnd set)
    abctime clkEnd = p->timeSolveEnd ? p->timeSolveEnd : Abc_Clock();
    double totalSec = (double)(clkEnd - p->timeSolveStart) / CLOCKS_PER_SEC;
    double solverSec = (double)p->timeSolver / CLOCKS_PER_SEC;
    double refineSec = (double)p->timeRefine / CLOCKS_PER_SEC;

    // Result counts
    int nResetRequired = 0;
    if (p->vRoVals0) {
        int val, idx;
        Vec_IntForEachEntry(p->vRoVals0, val, idx)
            if (val == MINR_VAL_0 || val == MINR_VAL_1) nResetRequired++;
    }
    int nSpecRegs = 0;
    for (int ri = 0; ri < nRegs; ri++)
        if (p->pInitStr[ri] == '0' || p->pInitStr[ri] == '1') nSpecRegs++;
    double resetRatio = nRegs > 0 ? 100.0 * nResetRequired / nRegs : 0.0;
    double reduction = nSpecRegs > 0 ? 100.0 * (1.0 - (double)nResetRequired / (double)nSpecRegs) : -1.0;

    int nResetBefore = nResetRequired + p->nRefineReleased;
    double resetRatioBefore = (p->nRefineMode > 0 && nRegs > 0) ? (100.0 * nResetBefore / nRegs) : -1.0;
    double reductionBefore  = (p->nRefineMode > 0 && nSpecRegs > 0) ? (100.0 * (1.0 - (double)nResetBefore / (double)nSpecRegs)) : -1.0;

    // Solver status string
    const char * pStatus;
    switch (p->solverStatus) {
        case 1:  pStatus = "optimum";  break;
        case 2:  pStatus = "unsat";    break;
        case 3:  pStatus = "error";    break;
        case 4:  pStatus = "timeout";  break;
        default: pStatus = "not_run";  break;
    }

    // --- [circuit] ---
    const char * pName = Gia_ManName(pGia) ? Gia_ManName(pGia) : "unknown";
    fprintf(pFile, "[circuit]\n");
    fprintf(pFile, "name    = %s\n",  pName);
    fprintf(pFile, "inputs  = %d\n",  nPI);
    fprintf(pFile, "outputs = %d\n",  Gia_ManPoNum(pGia));
    fprintf(pFile, "ff      = %d\n",  nRegs);
    fprintf(pFile, "nodes   = %d\n",  Gia_ManAndNum(pGia));
    fprintf(pFile, "\n");

    // --- [settings] ---
    fprintf(pFile, "[settings]\n");
    fprintf(pFile, "k              = %d\n",  p->nFrames);
    fprintf(pFile, "target_state   = %s\n",  p->pInitStr);
    if (p->fRandTarget) {
        fprintf(pFile, "random_seed    = %d\n",  p->seed);
        fprintf(pFile, "random_cycles  = %d\n",  p->nRandomSim);
    } else {
        fprintf(pFile, "random_seed    = N/A\n");
        fprintf(pFile, "random_cycles  = N/A\n");
    }
    if (p->nDontCarePercent > 0)
        fprintf(pFile, "dontcare_pct   = %d\n",  p->nDontCarePercent);
    fprintf(pFile, "refine_mode    = %d\n",  p->nRefineMode);
    fprintf(pFile, "\n");

    // --- [refine] --- (before result)
    if (p->nRefineMode > 0) {
        fprintf(pFile, "[refine]\n");
        fprintf(pFile, "mode           = %d\n", p->nRefineMode);
        fprintf(pFile, "released       = %d\n", p->nRefineReleased);
        fprintf(pFile, "by_trial       = %d\n", p->nRefineByTrial);
        fprintf(pFile, "by_core        = %d\n", p->nRefineByCore);
        fprintf(pFile, "refine_sec     = %.3f\n", refineSec);
        fprintf(pFile, "reset_before   = %d\n", nResetRequired + p->nRefineReleased);
        fprintf(pFile, "\n");
    }

    // --- [result] ---
    fprintf(pFile, "[result]\n");
    fprintf(pFile, "solver_status  = %s\n",    pStatus);
    fprintf(pFile, "specified_regs = %d\n",    nSpecRegs);
    fprintf(pFile, "required_reset = %d\n",    nResetRequired);
    fprintf(pFile, "reset_ratio    = %.2f%%\n", resetRatio);
    if (reduction >= 0.0)
        fprintf(pFile, "reduction      = %.2f%%\n", reduction);
    else
        fprintf(pFile, "reduction      = N/A\n");
    if (p->nRefineMode > 0 && resetRatioBefore >= 0.0)
        fprintf(pFile, "reset_ratio_before_refine = %.2f%%\n", resetRatioBefore);
    else
        fprintf(pFile, "reset_ratio_before_refine = N/A\n");
    if (p->nRefineMode > 0 && reductionBefore >= 0.0)
        fprintf(pFile, "reduction_before_refine   = %.2f%%\n", reductionBefore);
    else
        fprintf(pFile, "reduction_before_refine   = N/A\n");
    fprintf(pFile, "cut_verified   = %s\n",    (p->solverStatus == 1) ? (p->fVerifyPass ? "pass" : "fail") : "N/A");
    fprintf(pFile, "cec_verified   = %s\n",    (p->solverStatus == 1) ? (p->fCecVerifyPass ? "pass" : "fail") : "N/A");
    fprintf(pFile, "runtime_sec    = %.3f\n",  totalSec);
    fprintf(pFile, "solver_sec     = %.3f\n",  solverSec);
    fprintf(pFile, "timestamp      = %s\n",    timebuf);
    if (p->vCutNodes)
        fprintf(pFile, "cut_size       = %d\n", Vec_IntSize(p->vCutNodes));
    if (p->vEqCutNodes)
        fprintf(pFile, "eq_cut_size    = %d\n", Vec_IntSize(p->vEqCutNodes));
    fprintf(pFile, "spec_ro_in_cut = %.2f%%\n", p->specRoCutRatio);
    if (p->solverStatus == 1) {
        fprintf(pFile, "sim_reg_mismatch_weak   = %.2f%%\n", p->simRegMismatchWeakPct);
        fprintf(pFile, "sim_reg_mismatch_strong = %.2f%%\n", p->simRegMismatchStrongPct);
    }
    if (p->nOptimizeMode != 0) {
        const char * pOptStatus;
        switch (p->optStatus) {
            case 0:  pOptStatus = "found_best";            break;
            case 1:  pOptStatus = "timeout_with_best";     break;
            case 2:  pOptStatus = "timeout_no_solution";   break;
            default: pOptStatus = "unknown";               break;
        }
        fprintf(pFile, "opt_status     = %s\n", pOptStatus);
        fprintf(pFile, "best_k         = %d\n", p->bestK);
    }
    /* k=0 metrics (only meaningful for -O 1 which sweeps k and logs iterations) */
    if (p->nOptimizeMode == 1 && p->vOptIterK && p->vOptIterResets) {
        int found = 0;
        for (int itr = 0; itr < Vec_IntSize(p->vOptIterK); itr++) {
            if (Vec_IntEntry(p->vOptIterK, itr) != 0) continue;
            int r0 = Vec_IntEntry(p->vOptIterResets, itr);
            if (r0 < 0) break;
            fprintf(pFile, "k0_reset_ratio = %.2f%%\n", nRegs > 0 ? 100.0 * r0 / nRegs : 0.0);
            if (nSpecRegs > 0)
                fprintf(pFile, "k0_reduction   = %.2f%%\n", 100.0 * (1.0 - (double)r0 / (double)nSpecRegs));
            else
                fprintf(pFile, "k0_reduction   = N/A\n");
            found = 1;
            break;
        }
        if (!found) {
            fprintf(pFile, "k0_reset_ratio = N/A\n");
            fprintf(pFile, "k0_reduction   = N/A\n");
        }
    }
    fprintf(pFile, "\n");

    // --- [details] ---
    fprintf(pFile, "[details]\n");

    if (p->solverStatus != 1) {
        fprintf(pFile, "# No solution available.\n");
        fclose(pFile);
        printf("[Minr] Report written to %s\n", p->pReportFile);
        return;
    }

    // PI sequence
    fprintf(pFile, "# PI sequence (t=0 to t=%d)\n", p->nFrames - 1);
    if (p->vPiVals && p->nFrames > 0) {
        for (int t = 0; t < p->nFrames; t++) {
            fprintf(pFile, "t=%d: ", t);
            for (int i = 0; i < nPI; i++)
                fprintf(pFile, "%d", Vec_IntEntry(p->vPiVals, t * nPI + i));
            fprintf(pFile, "\n");
        }
    } else if (p->nFrames == 0) {
        fprintf(pFile, "# (k=0, no PI steps)\n");
    }
    fprintf(pFile, "\n");

    // FF reset requirements string (index i = i-th character)
    fprintf(pFile, "# FF reset requirements (01x string; index i = i-th character)\n");
    if (p->vRoVals0) {
        for (int i = 0; i < nRegs; i++) {
            int v = Vec_IntEntry(p->vRoVals0, i);
            char c = (v == MINR_VAL_0) ? '0' : (v == MINR_VAL_1) ? '1' : 'x';
            fputc(c, pFile);
        }
        fputc('\n', pFile);
    }

    // --- [iterations] --- (-O 1 only)
    if (p->nOptimizeMode == 1 && p->vOptIterK && Vec_IntSize(p->vOptIterK) > 0) {
        fprintf(pFile, "\n[iterations]\n");
        fprintf(pFile, "# k, resets, reduction, time_ms\n");
        int itr;
        for (itr = 0; itr < Vec_IntSize(p->vOptIterK); itr++) {
            int iterK      = Vec_IntEntry(p->vOptIterK, itr);
            int iterResets  = Vec_IntEntry(p->vOptIterResets, itr);
            int iterStatus  = Vec_IntEntry(p->vOptIterStatus, itr);
            int iterTimeMs  = Vec_IntEntry(p->vOptIterTimeMs, itr);
            if (iterResets >= 0) {
                if (nSpecRegs > 0)
                    fprintf(pFile, "k=%d, resets=%d, reduction=%.2f%%, %dms\n", iterK, iterResets,
                            100.0 * (1.0 - (double)iterResets / (double)nSpecRegs), iterTimeMs);
                else
                    fprintf(pFile, "k=%d, resets=%d, reduction=N/A, %dms\n", iterK, iterResets, iterTimeMs);
            } else {
                const char * pTag;
                switch (iterStatus) {
                    case 2:  pTag = "unsat";   break;
                    case 4:  pTag = "timeout"; break;
                    default: pTag = "error";   break;
                }
                fprintf(pFile, "k=%d, %s, reduction=N/A, %dms\n", iterK, pTag, iterTimeMs);
            }
        }
    }

    // --- [optimize2_outer] --- (-O 2 only)
    if (p->nOptimizeMode == 2 && p->vOpt2OuterSegmentTimeMs && Vec_IntSize(p->vOpt2OuterSegmentTimeMs) > 0) {
        fprintf(pFile, "\n[optimize2_outer]\n");
        int nOuter = Vec_IntSize(p->vOpt2OuterSegmentTimeMs);
        int o;
        for (o = 0; o < nOuter; o++) {
            int segMs = Vec_IntEntry(p->vOpt2OuterSegmentTimeMs, o);
            int bestR = Vec_IntEntry(p->vOpt2OuterBestResets, o);
            int tgtR = (p->vOpt2OuterTargetResets && o < Vec_IntSize(p->vOpt2OuterTargetResets)) ? Vec_IntEntry(p->vOpt2OuterTargetResets, o) : -1;
            fprintf(pFile, "outer=%d, target_resets=%d, segment_ms=%d, best_resets=%d\n", o, tgtR, segMs, bestR);
            if (p->vOpt2OuterInnerK && o < Vec_WecSize(p->vOpt2OuterInnerK)) {
                Vec_Int_t * vK = Vec_WecEntry(p->vOpt2OuterInnerK, o);
                Vec_Int_t * vR = Vec_WecEntry(p->vOpt2OuterInnerResets, o);
                Vec_Int_t * vT = Vec_WecEntry(p->vOpt2OuterInnerTimeMs, o);
                int itr;
                for (itr = 0; itr < Vec_IntSize(vK); itr++) {
                    int ik = Vec_IntEntry(vK, itr);
                    int ir = Vec_IntEntry(vR, itr);
                    int it = Vec_IntEntry(vT, itr);
                    if (ir >= 0)
                        fprintf(pFile, "  k=%d, resets=%d, %dms\n", ik, ir, it);
                    else
                        fprintf(pFile, "  k=%d, fail, %dms\n", ik, it);
                }
            }
        }
    }

    fclose(pFile);
    printf("[Minr] Report written to %s\n", p->pReportFile);
}

////////////////////////////////////////////////////////////////////////
///                  TFI COMPUTATION                                 ///
////////////////////////////////////////////////////////////////////////

/**
 * Minr_ComputeTfi - Compute transitive fanin cone in combinational AIG.
 * Returns a bit-vector of size nObjs where entry[iObj]=1 means iObj is
 * in the TFI of at least one root.
 */
static Vec_Int_t * Minr_ComputeTfi(Gia_Man_t * pGia, Vec_Int_t * vRoots)
{
    int nObjs = Gia_ManObjNum(pGia);
    Vec_Int_t * vMark = Vec_IntStart(nObjs);
    Vec_Int_t * vStack = Vec_IntAlloc(256);
    int i, iObj;
    Vec_IntForEachEntry(vRoots, iObj, i) {
        if (iObj >= 0 && iObj < nObjs && !Vec_IntEntry(vMark, iObj)) {
            Vec_IntWriteEntry(vMark, iObj, 1);
            Vec_IntPush(vStack, iObj);
        }
    }
    while (Vec_IntSize(vStack) > 0) {
        int cur = Vec_IntPop(vStack);
        Gia_Obj_t * pObj = Gia_ManObj(pGia, cur);
        if (Gia_ObjIsAnd(pObj)) {
            int f0 = Gia_ObjFaninId0(pObj, cur);
            int f1 = Gia_ObjFaninId1(pObj, cur);
            if (!Vec_IntEntry(vMark, f0)) { Vec_IntWriteEntry(vMark, f0, 1); Vec_IntPush(vStack, f0); }
            if (!Vec_IntEntry(vMark, f1)) { Vec_IntWriteEntry(vMark, f1, 1); Vec_IntPush(vStack, f1); }
        } else if (Gia_ObjIsCo(pObj)) {
            int f0 = Gia_ObjFaninId0(pObj, cur);
            if (!Vec_IntEntry(vMark, f0)) { Vec_IntWriteEntry(vMark, f0, 1); Vec_IntPush(vStack, f0); }
        }
    }
    Vec_IntFree(vStack);
    return vMark;
}

////////////////////////////////////////////////////////////////////////
///                     MAIN SOLVER PROCEDURE                        ///
////////////////////////////////////////////////////////////////////////

/**
 * Minr_SolveSingleK - Solve for a specific k value.
 * Builds CNF, writes WCNF, calls solver, decodes, verifies.
 * Returns the number of resets required, or -1 on failure/timeout.
 * The caller is responsible for freeing p->vVarMap, p->vClauses etc.
 * p->vPropVals and p->vCutNodes must already be set (from propagation).
 */
static int Minr_SolveSingleK(Minr_Man_t * p, double solverTimeout)
{
    Gia_Man_t * pGia = p->pGia;
    int nFrames = p->nFrames;

    // 0. Compute TFI cones for CNF pruning
    //    vTfiCut: TFI of cut nodes (non-X) — used at t=k
    //    vTfiRi:  TFI of all RIs           — used at t<k
    Vec_Int_t * vCutRoots = Vec_IntAlloc(64);
    {
        int i, NodeId;
        Vec_IntForEachEntry(p->vCutNodes, NodeId, i) {
            if (Vec_IntEntry(p->vPropVals, NodeId) != MINR_VAL_X)
                Vec_IntPush(vCutRoots, NodeId);
        }
    }
    Vec_Int_t * vTfiCut = Minr_ComputeTfi(pGia, vCutRoots);
    Vec_IntFree(vCutRoots);

    Vec_Int_t * vRiRoots = Vec_IntAlloc(Gia_ManRegNum(pGia));
    {
        int i; Gia_Obj_t * pO;
        Gia_ManForEachRi(pGia, pO, i)
            Vec_IntPush(vRiRoots, Gia_ObjId(pGia, pO));
    }
    Vec_Int_t * vTfiRi = Minr_ComputeTfi(pGia, vRiRoots);
    Vec_IntFree(vRiRoots);

    if (p->vLevel >= 2) {
        int nC = 0, nR = 0, nO = Gia_ManObjNum(pGia);
        for (int j = 0; j < nO; j++) { nC += Vec_IntEntry(vTfiCut, j); nR += Vec_IntEntry(vTfiRi, j); }
        printf("[TFI] cut cone: %d/%d nodes, RI cone: %d/%d nodes\n", nC, nO, nR, nO);
    }

    // 1. Allocate Vars (only for nodes in the relevant TFI cone)
    if (p->vVarMap) Vec_IntFree(p->vVarMap);
    p->vVarMap = Vec_IntStart(Gia_ManObjNum(pGia) * (nFrames + 1));
    if (p->vClauses) Vec_WecFree(p->vClauses);
    p->vClauses = Vec_WecAlloc(1000);
    p->nSatVars = 0;
    if (p->vPiVals) { Vec_IntFree(p->vPiVals); p->vPiVals = NULL; }
    if (p->vRoVals0) { Vec_IntFree(p->vRoVals0); p->vRoVals0 = NULL; }
    p->solverStatus = 0;
    p->fVerifyPass = 0;
    p->timeSolver = 0;

    int iObj, t;
    Gia_Obj_t * pObj;
    for (t = 0; t <= nFrames; t++) {
        Vec_Int_t * vTfi = (t == nFrames) ? vTfiCut : vTfiRi;
        Gia_ManForEachObj(pGia, pObj, iObj) {
            int fNeed;
            if (iObj == 0 || Gia_ObjIsCi(pObj)) {
                fNeed = 1;  /* const0, PIs, ROs: always allocate */
            } else {
                fNeed = Vec_IntEntry(vTfi, iObj);  /* AND, CO: only if in TFI */
            }
            if (fNeed) {
                p->nSatVars++; Vec_IntWriteEntry(p->vVarMap, iObj * (nFrames + 1) + t, p->nSatVars);
                p->nSatVars++;
            }
        }
    }

    if (p->vLevel > 0) printf("Created %d SAT variables for %d frames.\n", p->nSatVars, nFrames);

    // 2. Unrolling & Hard Constraints (skip AND/CO not in TFI; CIs always included)
    for (t = 0; t <= nFrames; t++) {
        Vec_Int_t * vTfi = (t == nFrames) ? vTfiCut : vTfiRi;
        {
            int c0_T = Lit_T(p, 0, t), c0_F = Lit_F(p, 0, t);
            Minr_AddClause1(p, Abc_LitNot(c0_T)); Minr_AddClause1(p, c0_F);
        }
        Gia_ManForEachObj(pGia, pObj, iObj) {
            if (iObj == 0) continue;

            if (Gia_ObjIsCi(pObj)) {
                if (t < nFrames) {
                    if (Gia_ObjIsPi(pGia, pObj)) {
                         Minr_AddBinaryConstraint(p, iObj, t);
                         Minr_AddIllegalStateCheck(p, iObj, t);
                    } else {
                         Minr_AddIllegalStateCheck(p, iObj, t);
                    }
                } else {
                    if (Gia_ObjIsPi(pGia, pObj)) {
                        if (p->vPiAtK) {
                            /* fixed in separate pass below */
                        } else {
                            Minr_AddUnknownConstraint(p, iObj, t);
                        }
                    } else {
                        Minr_AddIllegalStateCheck(p, iObj, t);
                    }
                }
            }
            if (Gia_ObjIsAnd(pObj) && Vec_IntEntry(vTfi, iObj)) {
                Minr_AddAnd(p, iObj, Gia_ObjFaninId0(pObj, iObj), Gia_ObjFaninId1(pObj, iObj), Gia_ObjFaninC0(pObj), Gia_ObjFaninC1(pObj), t);
                Minr_AddIllegalStateCheck(p, iObj, t);
            }
            if (Gia_ObjIsCo(pObj) && Vec_IntEntry(vTfi, iObj)) {
                Minr_AddCoBuffer(p, iObj, Gia_ObjFaninId0(pObj, iObj), Gia_ObjFaninC0(pObj), t);
                Minr_AddIllegalStateCheck(p, iObj, t);
            }
        }
        if (t < nFrames) {
            int i;
            Gia_ManForEachRi(pGia, pObj, i) {
                Gia_Obj_t * pObjRoNext = Gia_ManRo(pGia, i);
                Minr_AddEquiv(p, Gia_ObjId(pGia, pObjRoNext), Gia_ObjId(pGia, pObj), t+1, t);
            }
        }
    }

    // 2b. Fix PI at t=k when vPiAtK is set (-O 2, iteration > 1)
    if (p->vPiAtK) {
        int iPi;
        Gia_ManForEachPi(pGia, pObj, iPi) {
            int iObjPi = Gia_ObjId(pGia, pObj);
            int val = Vec_IntEntry(p->vPiAtK, iPi);
            int lit_T = Lit_T(p, iObjPi, nFrames);
            int lit_F = Lit_F(p, iObjPi, nFrames);
            if (val == 0) {
                Minr_AddClause1(p, Abc_LitNot(lit_T));
                Minr_AddClause1(p, lit_F);
            } else {
                Minr_AddClause1(p, lit_T);
                Minr_AddClause1(p, Abc_LitNot(lit_F));
            }
        }
    }

    Vec_IntFree(vTfiCut);
    Vec_IntFree(vTfiRi);

    // 3. Cut Constraints at t=k
    {
        int i, NodeId;
        Vec_IntForEachEntry(p->vCutNodes, NodeId, i) {
            int Val = Vec_IntEntry(p->vPropVals, NodeId);
            if (Val == MINR_VAL_X) continue;
            int lit_T = Lit_T(p, NodeId, nFrames);
            int lit_F = Lit_F(p, NodeId, nFrames);
            if (Val == MINR_VAL_0) { Minr_AddClause1(p, Abc_LitNot(lit_T)); Minr_AddClause1(p, lit_F); }
            else if (Val == MINR_VAL_1) { Minr_AddClause1(p, lit_T); Minr_AddClause1(p, Abc_LitNot(lit_F)); }
        }
    }

    // 4. Objective (Soft Clauses)
    Vec_Int_t * vSoftLits = Vec_IntAlloc(Gia_ManRegNum(pGia));
    {
        int i;
        Gia_Obj_t * pObjRo;
        Gia_ManForEachRo(pGia, pObjRo, i) {
            int iObj = Gia_ObjId(pGia, pObjRo);
            p->nSatVars++; int u_Var = p->nSatVars; int u_Lit = Abc_Var2Lit(u_Var, 0);
            int t_Lit = Lit_T(p, iObj, 0);
            int f_Lit = Lit_F(p, iObj, 0);
            Minr_AddClause2(p, Abc_LitNot(u_Lit), Abc_LitNot(t_Lit));
            Minr_AddClause2(p, Abc_LitNot(u_Lit), Abc_LitNot(f_Lit));
            Minr_AddClause3(p, t_Lit, f_Lit, u_Lit);
            Vec_IntPush(vSoftLits, u_Lit);
        }
    }

    // 5. Write WCNF
    char Buffer[1000];
    const char * pFinalPrefix = p->pPrefix ? p->pPrefix : "minr_out";
    const char * pFinalDir = p->pOutDir ? p->pOutDir : "_/tmp";
#ifdef WIN32
    mkdir(pFinalDir);
#else
    mkdir(pFinalDir, 0777);
#endif
    if (p->nOptimizeMode != 0)
        sprintf(Buffer, "%s/%s_k%d.wcnf", pFinalDir, pFinalPrefix, nFrames);
    else
        sprintf(Buffer, "%s/%s.wcnf", pFinalDir, pFinalPrefix);
    if (p->vLevel > 0) printf("Writing WCNF to %s ...\n", Buffer);
    long long topWeight = Gia_ManRegNum(pGia) + 1;
    FILE * pFile = fopen(Buffer, "w");
    if (!pFile) { printf("Error: Cannot open %s\n", Buffer); Vec_IntFree(vSoftLits); return -1; }
    fprintf(pFile, "p wcnf %d %d %lld\n", p->nSatVars, Vec_WecSize(p->vClauses) + Vec_IntSize(vSoftLits), topWeight);
    Vec_Int_t * vC; int k, Lit, i;
    Vec_WecForEachLevel(p->vClauses, vC, k) {
        fprintf(pFile, "%lld ", topWeight);
        Vec_IntForEachEntry(vC, Lit, i) fprintf(pFile, "%s%d ", Abc_LitIsCompl(Lit) ? "-" : "", Abc_Lit2Var(Lit));
        fprintf(pFile, "0\n");
    }
    Vec_IntForEachEntry(vSoftLits, Lit, k) fprintf(pFile, "1 %s%d 0\n", Abc_LitIsCompl(Lit) ? "-" : "", Abc_Lit2Var(Lit));
    fclose(pFile);

    // Call Solver & Decode (PostRelax + Verify are done once in Minr_Solve)
    {
        Vec_Int_t * vModel = NULL;
        if ( p->pSolver && strstr(p->pSolver, ".so") )
            vModel = Minr_CallSolverIpamir( p, p->vClauses, vSoftLits, p->pSolver, solverTimeout );
        else
            vModel = Minr_CallSolver(p, Buffer, p->pSolver, solverTimeout);
        if (vModel) {
            Minr_DecodeResult(p, vModel);
            Vec_IntFree(vModel);
        }
    }

    Vec_IntFree(vSoftLits);

    // Count resets
    if (p->solverStatus == 1 && p->vRoVals0) {
        int nResets = 0, val, idx;
        Vec_IntForEachEntry(p->vRoVals0, val, idx)
            if (val == MINR_VAL_0 || val == MINR_VAL_1) nResets++;
        return nResets;
    }
    return -1;
}

/**
 * Minr_SolveOptimize - Optimize mode: sweep k (default 0,1,2,4,8,... or dense 0..N with -K N)
 * with total time budget, best-so-far tracking, and early stop.
 */
#if !defined(ABC_NAMESPACE)
extern "C"
#endif
void Minr_SolveOptimize(Minr_Man_t * p)
{
    int nRegs = Gia_ManRegNum(p->pGia);
    int nSpecRegs = 0;
    if (p->pInitStr) {
        for (int ri = 0; ri < nRegs && p->pInitStr[ri]; ri++)
            if (p->pInitStr[ri] == '0' || p->pInitStr[ri] == '1') nSpecRegs++;
    }
    int kScheduleStatic[] = {0, 1, 2, 4, 8, 16, 32, 64, 128, 256};
    int * kSchedule = NULL;
    int nSchedule = 0;
    int fFreeSchedule = 0;
    if ( p->nOptimizeDenseKMax >= 0 ) {
        nSchedule = p->nOptimizeDenseKMax + 1;
        kSchedule = ABC_ALLOC( int, nSchedule );
        if ( !kSchedule ) {
            printf( "[Optimize] Out of memory for k schedule. Using geometric k schedule.\n" );
            kSchedule = kScheduleStatic;
            nSchedule = (int)(sizeof(kScheduleStatic) / sizeof(kScheduleStatic[0]));
        } else {
            fFreeSchedule = 1;
            for ( int i = 0; i < nSchedule; i++ )
                kSchedule[i] = i;
        }
    } else {
        kSchedule = kScheduleStatic;
        nSchedule = (int)(sizeof(kScheduleStatic) / sizeof(kScheduleStatic[0]));
    }

    p->bestK = -1;
    p->bestResetCount = nRegs + 1;
    p->vBestPiVals = NULL;
    p->vBestRoVals0 = NULL;
    p->bestSolverStatus = 0;
    p->optStatus = 2;  // default: timeout_no_solution

    p->vOptIterK      = Vec_IntAlloc(nSchedule);
    p->vOptIterResets  = Vec_IntAlloc(nSchedule);
    p->vOptIterStatus  = Vec_IntAlloc(nSchedule);
    p->vOptIterTimeMs  = Vec_IntAlloc(nSchedule);

    int prevResetCount = nRegs;  // for early stop comparison
    int fDenseSweep = (p->nOptimizeDenseKMax >= 0); /* -K: run all k=0..N (or until global timeout) for per-k stats */

    printf("\n[Optimize] Starting k-sweep with %s time budget.\n",
           p->totalTimeout > 0 ? "limited" : "unlimited");
    if (fDenseSweep)
        printf("[Optimize] Dense sweep (-K): no early exit on 0 resets, UNSAT, or small improvement; stop at k=N or time budget.\n");

    for (int si = 0; si < nSchedule; si++) {
        int curK = kSchedule[si];

        // Check time budget
        double elapsed = (double)(Abc_Clock() - p->timeSolveStart) / CLOCKS_PER_SEC;
        double tRemain = 0;
        if (p->totalTimeout > 0) {
            tRemain = p->totalTimeout - elapsed;
            if (tRemain <= 1.0) {
                printf("[Optimize] Time budget exhausted (%.1fs elapsed). Stopping.\n", elapsed);
                break;
            }
        }

        printf("\n[Optimize] === Iteration k=%d (elapsed=%.1fs", curK, elapsed);
        if (p->totalTimeout > 0)
            printf(", remaining=%.1fs", tRemain);
        printf(") ===\n");

        p->nFrames = curK;

        // Use remaining time as solver timeout (or 0 for unlimited)
        double solverTimeout = (p->totalTimeout > 0) ? tRemain : 0;

        abctime clkIter = Abc_Clock();
        int nResets = Minr_SolveSingleK(p, solverTimeout);
        int iterMs = (int)((double)(Abc_Clock() - clkIter) * 1000.0 / CLOCKS_PER_SEC);

        Vec_IntPush(p->vOptIterK, curK);
        Vec_IntPush(p->vOptIterResets, nResets);
        Vec_IntPush(p->vOptIterStatus, p->solverStatus);
        Vec_IntPush(p->vOptIterTimeMs, iterMs);

        if (nResets >= 0) {
            printf("[Optimize] k=%d: reset_needed=%d (specified=%d, %.2f%%)\n",
                   curK, nResets, nSpecRegs, nSpecRegs > 0 ? 100.0 * nResets / nSpecRegs : 0.0);

            if (nResets < p->bestResetCount) {
                // Update best-so-far
                p->bestK = curK;
                p->bestResetCount = nResets;
                p->bestSolverStatus = p->solverStatus;
                if (p->vBestPiVals) Vec_IntFree(p->vBestPiVals);
                if (p->vBestRoVals0) Vec_IntFree(p->vBestRoVals0);
                p->vBestPiVals = p->vPiVals ? Vec_IntDup(p->vPiVals) : NULL;
                p->vBestRoVals0 = p->vRoVals0 ? Vec_IntDup(p->vRoVals0) : NULL;
                p->optStatus = 0;  // found_best

                printf("[Optimize] >> New best: k=%d, resets=%d\n", curK, nResets);
            }

            // Early stop: check improvement vs previous round (skipped in dense -K sweep)
            if (!fDenseSweep) {
                if (nResets == 0) {
                    printf("[Optimize] Perfect solution (0 resets). Stopping.\n");
                    break;
                }
                if (si > 0 && prevResetCount > 0) {
                    double improvement = (double)(prevResetCount - nResets) / prevResetCount;
                    if (improvement < (double)MINR_EARLY_STOP_IMPROVEMENT_RATIO) {
                        printf("[Optimize] Improvement %.2f%% < threshold %.2f%%. Stopping.\n",
                               improvement * 100.0, (double)MINR_EARLY_STOP_IMPROVEMENT_RATIO * 100.0);
                        break;
                    }
                }
            } else if (nResets == 0) {
                printf("[Optimize] Perfect solution (0 resets); continuing dense sweep.\n");
            }
            prevResetCount = nResets;
        } else {
            printf("[Optimize] k=%d: no solution (status=%d)\n", curK, p->solverStatus);
            if (!fDenseSweep && (p->solverStatus == 4 || p->solverStatus == 2) && p->bestResetCount <= nRegs) {
                p->optStatus = 1;  // timeout_with_best
                printf("[Optimize] Solver %s. Keeping best-so-far.\n",
                       p->solverStatus == 4 ? "timed out" : "returned UNSAT");
                break;
            }
            if (fDenseSweep && (p->solverStatus == 4 || p->solverStatus == 2))
                printf("[Optimize] Continuing dense sweep (next k).\n");
        }
    }

    // Restore best solution into p for report dumping
    if (p->bestK >= 0) {
        p->nFrames = p->bestK;
        p->solverStatus = p->bestSolverStatus;
        if (p->vPiVals) Vec_IntFree(p->vPiVals);
        if (p->vRoVals0) Vec_IntFree(p->vRoVals0);
        p->vPiVals = p->vBestPiVals;   p->vBestPiVals = NULL;
        p->vRoVals0 = p->vBestRoVals0; p->vBestRoVals0 = NULL;

        double totalSec = (double)(Abc_Clock() - p->timeSolveStart) / CLOCKS_PER_SEC;
        printf("\n[Optimize] Final best: k=%d, resets=%d/%d (%.2f%%), total=%.3fs\n",
               p->bestK, p->bestResetCount, nRegs,
               100.0 * p->bestResetCount / nRegs, totalSec);
    } else {
        printf("\n[Optimize] No feasible solution found.\n");
    }

    if ( fFreeSchedule && kSchedule )
        ABC_FREE( kSchedule );
}

/**
 * Minr_SolveOptimize2 - -O 2: outer-loop heuristic.
 * Outer loop: when time for current target exceeds budget/P, run cut verify,
 * SAT refinement, set new target from current reset config, start next iteration.
 * First iteration: t=k PI unknown. Later iterations: t=k PI = prev iteration's t=0 PI.
 * Final solution: total t = sum of k over iterations; reset = last iteration's;
 * input sequence = apply order: last iter, then second-to-last, ..., first.
 * Constant-cut verify only before refinement each segment; CEC verify once at end.
 */
#if !defined(ABC_NAMESPACE)
extern "C"
#endif
void Minr_SolveOptimize2(Minr_Man_t * p)
{
    Gia_Man_t * pGia = p->pGia;
    int nRegs = Gia_ManRegNum(pGia);
    int nPI = Gia_ManPiNum(pGia);
    double segLimit = (p->totalTimeout > 0) ? (p->totalTimeout / (double)MINR_OPT_BUDGET_PARTS) : 1e20;
    int kSchedule[] = {0, 1, 2, 4, 8, 16, 32, 64, 128, 256};
    int nSchedule = (int)(sizeof(kSchedule) / sizeof(kSchedule[0]));
    p->vConcatPiVals = NULL;
    p->vPiAtK = NULL;
    char * pTargetStr = NULL;  /* owned by us after first re-target */
    Vec_Wec_t * vOuterPiVals = Vec_WecAlloc(32);   /* each row = one outer's PI (flattened) */
    Vec_Int_t * vOuterK = Vec_IntAlloc(32);
    Vec_Wec_t * vOuterRoVals0 = Vec_WecAlloc(32); /* each row = one outer's RoVals0 (nRegs) */
    int nOuter = 0;
    int totalK = 0;
    Vec_Int_t * vLastRoVals0 = NULL;
    p->vOpt2OuterSegmentTimeMs = Vec_IntAlloc(32);
    p->vOpt2OuterBestResets = Vec_IntAlloc(32);
    p->vOpt2OuterTargetResets = Vec_IntAlloc(32);
    p->vOpt2OuterInnerK = Vec_WecAlloc(32);
    p->vOpt2OuterInnerResets = Vec_WecAlloc(32);
    p->vOpt2OuterInnerTimeMs = Vec_WecAlloc(32);

    p->bestK = -1;
    p->bestResetCount = nRegs + 1;
    p->optStatus = 2;

    /* Save original pInitStr for final CEC (will be mutated during outer loop) */
    char * pOrigInitStr = p->pInitStr;

    printf("\n[Optimize2] Outer-loop heuristic, segment limit=%.2fs (1/%d of budget).\n",
           segLimit, MINR_OPT_BUDGET_PARTS);

    for (;; nOuter++) {
        if (p->vLevel >= 3) printf("[Optimize2] >>> outer loop iteration nOuter=%d\n", nOuter);
        double tOuterStart = (double)(Abc_Clock() - p->timeSolveStart) / CLOCKS_PER_SEC;
        if (p->totalTimeout > 0 && tOuterStart >= p->totalTimeout - 0.5) {
            printf("[Optimize2] Total time budget exhausted. Stopping.\n");
            break;
        }

        if (nOuter > 0 && Vec_WecSize(vOuterPiVals) >= nOuter) {
            Vec_Int_t * prevPi = Vec_WecEntry(vOuterPiVals, nOuter - 1);
            if (p->vLevel >= 3) printf("[Optimize2] vPiAtK from prev segment: prevPi size=%d, nPI=%d\n", Vec_IntSize(prevPi), nPI);
            p->vPiAtK = Vec_IntAlloc(nPI);
            for (int i = 0; i < nPI && i < Vec_IntSize(prevPi); i++)
                Vec_IntPush(p->vPiAtK, Vec_IntEntry(prevPi, i));
            while (Vec_IntSize(p->vPiAtK) < nPI)
                Vec_IntPush(p->vPiAtK, 0);
            if (p->vLevel >= 3) printf("[Optimize2] vPiAtK size=%d\n", Vec_IntSize(p->vPiAtK));
        } else {
            p->vPiAtK = NULL;
        }

        /* Log target state for this outer iteration (reset count from pInitStr) */
        {
            int targetResets = 0, i;
            for (i = 0; i < nRegs && p->pInitStr && p->pInitStr[i]; i++)
                if (p->pInitStr[i] == '0' || p->pInitStr[i] == '1') targetResets++;
            Vec_IntPush(p->vOpt2OuterTargetResets, targetResets);
            if (p->vLevel > 0)
                printf("[Optimize2] outer=%d, target_state_resets=%d (from previous solution reset)\n", nOuter, targetResets);
        }
        if (p->vLevel >= 3) printf("[Optimize2] calling Minr_PropagateAndCut (pInitStr=%p, nRegs=%d)\n", (void*)p->pInitStr, nRegs);

        Minr_PropagateAndCut(p);
        if (p->vLevel >= 3) printf("[Optimize2] Minr_PropagateAndCut done, cut size=%d\n", p->vCutNodes ? Vec_IntSize(p->vCutNodes) : -1);

        double segStart = Abc_Clock();
        int segBestResets = nRegs + 1;
        Vec_Int_t * vInnerK = Vec_IntAlloc(16);
        Vec_Int_t * vInnerResets = Vec_IntAlloc(16);
        Vec_Int_t * vInnerTimeMs = Vec_IntAlloc(16);
        int prevResetCount = nRegs;
        int segBestK = -1;
        Vec_Int_t * vSegBestPi = NULL;
        Vec_Int_t * vSegBestRo = NULL;
        int fSegmentUnsat = 0;  /* set when any k in this segment returns UNSAT → terminate after this segment */
        int fBrokeSegmentLimit = 0;  /* set when we break inner loop due to segment time limit (already pushed vOpt2* and freed vInner*) */
        int fBrokeEarlyStop = 0;     /* set when we break inner due to 0 resets or improvement threshold → then break outer */

        /* When nOuter > 0, start k from (previous segment's last k) / 2 */
        int startSi = 0;
        if (nOuter > 0 && p->vOpt2OuterInnerK && Vec_WecSize(p->vOpt2OuterInnerK) >= nOuter) {
            Vec_Int_t * prevInnerK = Vec_WecEntry(p->vOpt2OuterInnerK, nOuter - 1);
            if (Vec_IntSize(prevInnerK) > 0) {
                int lastKPrev = Vec_IntEntry(prevInnerK, Vec_IntSize(prevInnerK) - 1);
                int startK = (lastKPrev >= 1) ? lastKPrev : 1;
                for (startSi = 0; startSi < nSchedule && kSchedule[startSi] < startK; startSi++)
                    ;
                if (p->vLevel > 0)
                    printf("[Optimize2] outer=%d: start k from %d (prev segment last k=%d)\n", nOuter, startSi < nSchedule ? kSchedule[startSi] : -1, lastKPrev);
            }
        }

        for (int si = startSi; si < nSchedule; si++) {
            int curK = kSchedule[si];
            double elapsed = (double)(Abc_Clock() - p->timeSolveStart) / CLOCKS_PER_SEC;
            if (p->totalTimeout > 0 && elapsed >= p->totalTimeout - 1.0) break;

            double segElapsed = (double)(Abc_Clock() - segStart) / CLOCKS_PER_SEC;
            if (segElapsed >= segLimit) {
                if (p->vLevel > 0)
                    printf("[Optimize2] Segment time (%.2fs) reached. Running refine, then new target.\n", segElapsed);
                int segMs = (int)((double)(Abc_Clock() - segStart) * 1000.0 / CLOCKS_PER_SEC);
                int segBestR = (segBestResets <= nRegs) ? segBestResets : -1;
                Vec_IntPush(p->vOpt2OuterSegmentTimeMs, segMs);
                Vec_IntPush(p->vOpt2OuterBestResets, segBestR);
                {
                    Vec_Int_t * r = Vec_WecPushLevel(p->vOpt2OuterInnerK);
                    Vec_IntAppend(r, vInnerK);
                    r = Vec_WecPushLevel(p->vOpt2OuterInnerResets);
                    Vec_IntAppend(r, vInnerResets);
                    r = Vec_WecPushLevel(p->vOpt2OuterInnerTimeMs);
                    Vec_IntAppend(r, vInnerTimeMs);
                }
                Vec_IntFree(vInnerK);
                Vec_IntFree(vInnerResets);
                Vec_IntFree(vInnerTimeMs);
                /* Use segment best (not last k) as new target when available */
                if (segBestK >= 0 && vSegBestPi && vSegBestRo) {
                    if (p->vPiVals) Vec_IntFree(p->vPiVals);
                    if (p->vRoVals0) Vec_IntFree(p->vRoVals0);
                    p->vPiVals = Vec_IntDup(vSegBestPi);
                    p->vRoVals0 = Vec_IntDup(vSegBestRo);
                    p->nFrames = segBestK;
                    p->solverStatus = 1;
                }
                if (p->solverStatus == 1 && p->vPiVals && p->vRoVals0) {
                    if (p->nRefineMode > 0) {
                        abctime clkRef = Abc_Clock();
                        Minr_SatRefine(p);
                        p->timeRefine = Abc_Clock() - clkRef;
                    }
                    {
                        int nR = 0, i;
                        for (i = 0; i < nRegs; i++)
                            if (Vec_IntEntry(p->vRoVals0, i) == MINR_VAL_0 || Vec_IntEntry(p->vRoVals0, i) == MINR_VAL_1) nR++;
                        if (p->vLevel > 0)
                            printf("[Optimize2] segment_limit: segment_best_resets=%d, new_target_resets=%d (reset state from this solution)\n", segBestResets <= nRegs ? segBestResets : -1, nR);
                    }
                    Vec_IntPush(vOuterK, p->nFrames);
                    Vec_Int_t * row = Vec_WecPushLevel(vOuterPiVals);
                    for (int i = 0; i < p->nFrames * nPI; i++)
                        Vec_IntPush(row, Vec_IntEntry(p->vPiVals, i));
                    row = Vec_WecPushLevel(vOuterRoVals0);
                    for (int i = 0; i < nRegs; i++)
                        Vec_IntPush(row, Vec_IntEntry(p->vRoVals0, i));
                    char * pNew = (char *)malloc((size_t)(nRegs + 1));
                    for (int i = 0; i < nRegs; i++) {
                        int v = Vec_IntEntry(p->vRoVals0, i);
                        pNew[i] = (v == MINR_VAL_0) ? '0' : (v == MINR_VAL_1) ? '1' : 'x';
                    }
                    pNew[nRegs] = '\0';
                    if (pTargetStr) free(pTargetStr);
                    pTargetStr = pNew;
                    p->pInitStr = pTargetStr;
                }
                if (p->vPiAtK) { Vec_IntFree(p->vPiAtK); p->vPiAtK = NULL; }
                fBrokeSegmentLimit = 1;
                break;
            }

            p->nFrames = curK;
            /* MaxSAT timeout = total remaining only; segment limit is only for deciding next segment */
            double solverTimeout = (p->totalTimeout > 0) ? (p->totalTimeout - elapsed) : 0;
            if (solverTimeout > 0 && solverTimeout < 1.0) break;

            abctime clkIter = Abc_Clock();
            int nResets = Minr_SolveSingleK(p, solverTimeout);
            int iterMs = (int)((double)(Abc_Clock() - clkIter) * 1000.0 / CLOCKS_PER_SEC);

            Vec_IntPush(vInnerK, curK);
            Vec_IntPush(vInnerResets, nResets);
            Vec_IntPush(vInnerTimeMs, iterMs);

            if (nResets >= 0) {
                if (nResets < segBestResets) {
                    segBestResets = nResets;
                    segBestK = curK;
                    if (vSegBestPi) Vec_IntFree(vSegBestPi);
                    if (vSegBestRo) Vec_IntFree(vSegBestRo);
                    vSegBestPi = p->vPiVals ? Vec_IntDup(p->vPiVals) : NULL;
                    vSegBestRo = p->vRoVals0 ? Vec_IntDup(p->vRoVals0) : NULL;
                }
                if (nResets == 0) {
                    if (p->vLevel > 0) printf("[Optimize2] Perfect (0 resets). Stopping segment.\n");
                    fBrokeEarlyStop = 1;
                    break;
                }
                if (si > 0 && prevResetCount > 0) {
                    double imp = (double)(prevResetCount - nResets) / prevResetCount;
                    if (imp < (double)MINR_EARLY_STOP_IMPROVEMENT_RATIO) {
                        fBrokeEarlyStop = 1;
                        break;
                    }
                }
                prevResetCount = nResets;
            } else {
                if (p->solverStatus == 2) fSegmentUnsat = 1;  /* UNSAT → do not run next segment */
                if ((p->solverStatus == 4 || p->solverStatus == 2) && segBestResets <= nRegs) break;
            }
        }

        /* Push segment stats and inner iteration data only when we did not break due to segment limit (that path already pushed and freed vInner*) */
        if (!fBrokeSegmentLimit) {
            int segMs = (int)((double)(Abc_Clock() - segStart) * 1000.0 / CLOCKS_PER_SEC);
            Vec_IntPush(p->vOpt2OuterSegmentTimeMs, segMs);
            Vec_IntPush(p->vOpt2OuterBestResets, segBestResets <= nRegs ? segBestResets : -1);
            {
                Vec_Int_t * r = Vec_WecPushLevel(p->vOpt2OuterInnerK);
                Vec_IntAppend(r, vInnerK);
                r = Vec_WecPushLevel(p->vOpt2OuterInnerResets);
                Vec_IntAppend(r, vInnerResets);
                r = Vec_WecPushLevel(p->vOpt2OuterInnerTimeMs);
                Vec_IntAppend(r, vInnerTimeMs);
            }
            Vec_IntFree(vInnerK);
            Vec_IntFree(vInnerResets);
            Vec_IntFree(vInnerTimeMs);
        }

        /* Every segment: run cut verify + SAT refinement on segment best (when -x > 0); skip if we already did it in segment-limit path */
        if (!fBrokeSegmentLimit && segBestK >= 0 && vSegBestPi && vSegBestRo && p->nRefineMode > 0) {
            if (p->vPiVals) Vec_IntFree(p->vPiVals);
            if (p->vRoVals0) Vec_IntFree(p->vRoVals0);
            p->vPiVals = Vec_IntDup(vSegBestPi);
            p->vRoVals0 = Vec_IntDup(vSegBestRo);
            p->nFrames = segBestK;
            p->solverStatus = 1;
            abctime clkRef = Abc_Clock();
            Minr_SatRefine(p);
            p->timeRefine = Abc_Clock() - clkRef;
            Vec_IntFree(vSegBestPi);
            Vec_IntFree(vSegBestRo);
            vSegBestPi = p->vPiVals;
            vSegBestRo = p->vRoVals0;
            p->vPiVals = p->vRoVals0 = NULL;
        }

        /* Push segment best to vOuter* for concat at end; skip if we already pushed in segment-limit path */
        if (!fBrokeSegmentLimit && segBestK >= 0 && vSegBestPi && vSegBestRo) {
            Vec_IntPush(vOuterK, segBestK);
            Vec_Int_t * row = Vec_WecPushLevel(vOuterPiVals);
            Vec_IntAppend(row, vSegBestPi);
            row = Vec_WecPushLevel(vOuterRoVals0);
            Vec_IntAppend(row, vSegBestRo);
            Vec_IntFree(vSegBestPi);
            Vec_IntFree(vSegBestRo);
            vSegBestPi = vSegBestRo = NULL;
        }
        if (p->vLevel > 0 && (segBestK >= 0 || Vec_IntSize(p->vOpt2OuterSegmentTimeMs) > 0))
            printf("[Optimize2] outer=%d segment done: segment_best_resets=%d\n", nOuter, segBestResets <= nRegs ? segBestResets : -1);

        if (p->vPiAtK) { Vec_IntFree(p->vPiAtK); p->vPiAtK = NULL; }

        /* UNSAT in this segment → terminate entire process (no further segments) */
        if (fSegmentUnsat) {
            if (p->vLevel > 0) printf("[Optimize2] UNSAT in segment; terminating (no next segment).\n");
            break;
        }
        /* No solution in this segment → no new target for next; terminate */
        if (segBestK < 0) {
            if (p->vLevel > 0) printf("[Optimize2] No solution in segment; terminating (no next segment).\n");
            break;
        }
        /* Early stop (0 resets or improvement threshold) → terminate; do not run next segment */
        if (fBrokeEarlyStop) {
            if (p->vLevel > 0) printf("[Optimize2] Early stop in segment; terminating (no next segment).\n");
            break;
        }
        /* If the very first segment's best is k=0, treat as global optimum and stop. */
        if (nOuter == 0 && segBestK == 0) {
            if (p->vLevel > 0) printf("[Optimize2] First segment best_k=0; treating as global optimum. Stopping.\n");
            break;
        }
        double tTotal = (double)(Abc_Clock() - p->timeSolveStart) / CLOCKS_PER_SEC;
        if (p->totalTimeout > 0 && tTotal >= p->totalTimeout - 0.5) break;
        if (segBestResets == 0) break;
        if (Vec_IntSize(vOuterK) == 0) break;  /* no solution at all so far → terminate */
    }

    nOuter = Vec_IntSize(vOuterK);
    if (nOuter == 0) {
        printf("[Optimize2] No feasible solution.\n");
        Vec_WecFree(vOuterPiVals);
        Vec_IntFree(vOuterK);
        Vec_WecFree(vOuterRoVals0);
        if (pTargetStr) free(pTargetStr);
        Vec_IntFree(p->vOpt2OuterSegmentTimeMs);
        Vec_IntFree(p->vOpt2OuterBestResets);
        Vec_IntFree(p->vOpt2OuterTargetResets);
        Vec_WecFree(p->vOpt2OuterInnerK);
        Vec_WecFree(p->vOpt2OuterInnerResets);
        Vec_WecFree(p->vOpt2OuterInnerTimeMs);
        return;
    }

    /* Build vConcatPiVals: apply order = last outer first, ..., first outer last */
    p->vConcatPiVals = Vec_IntAlloc(256);
    for (int o = nOuter - 1; o >= 0; o--) {
        int k = Vec_IntEntry(vOuterK, o);
        Vec_Int_t * row = Vec_WecEntry(vOuterPiVals, o);
        for (int i = 0; i < Vec_IntSize(row); i++)
            Vec_IntPush(p->vConcatPiVals, Vec_IntEntry(row, i));
        totalK += k;
    }
    vLastRoVals0 = Vec_WecEntry(vOuterRoVals0, nOuter - 1);
    p->nFrames = totalK;
    if (p->vPiVals) Vec_IntFree(p->vPiVals);
    p->vPiVals = p->vConcatPiVals;
    p->vConcatPiVals = NULL;
    p->vRoVals0 = Vec_IntDup(vLastRoVals0);
    p->solverStatus = 1;
    int nResets = 0, ii, kk;
    Vec_IntForEachEntry(p->vRoVals0, ii, kk)
        if (ii == MINR_VAL_0 || ii == MINR_VAL_1) nResets++;
    p->bestResetCount = nResets;
    p->bestK = totalK;

    /* Restore original pInitStr so CEC verifies against the true target */
    p->pInitStr = pOrigInitStr;

    // End of "runtime_sec" measurement: after optimize2 solving, before final verification.
    p->timeSolveEnd = Abc_Clock();

    p->fCecVerifyPass = Minr_CecVerify(p);
    p->optStatus = 0;

    printf("\n[Optimize2] Done: %d outer iterations, total k=%d, resets=%d, CEC=%s\n",
           nOuter, totalK, nResets, p->fCecVerifyPass ? "pass" : "fail");

    Vec_WecFree(vOuterPiVals);
    Vec_IntFree(vOuterK);
    Vec_WecFree(vOuterRoVals0);
    if (pTargetStr) free(pTargetStr);
}

#if !defined(ABC_NAMESPACE)
extern "C"
#endif
void Minr_Solve(Gia_Man_t * pGia, int nFrames, char * pInitStr, int fExplicitInit, int fRandTarget, int nRandomSim, char * pSolver, char * pOutDir, char * pPrefix, int vLevel, int seed, int nRefineMode, int fRefineBindDc, int nRefineConfLimit, int fRefineCoreOnly, char * pReportFile, int nOptimizeMode, double totalTimeout, int nDontCarePercent, int nOptimizeDenseKMax) {
    Minr_Man_t Man;
    Minr_Man_t * p = &Man;
    memset(p, 0, sizeof(Minr_Man_t));
    
    p->pGia = pGia;
    p->nFrames = nFrames;
    p->pInitStr = pInitStr;
    p->fExplicitInit = fExplicitInit;
    p->fRandTarget = fRandTarget;
    p->nRandomSim = nRandomSim;
    p->pSolver = pSolver;
    p->pOutDir = pOutDir;
    p->pPrefix = pPrefix;
    p->vLevel = vLevel;
    p->seed = seed;
    p->nRefineMode = nRefineMode;
    p->fRefineBindDc = fRefineBindDc;
    p->nRefineConfLimit = nRefineConfLimit;
    p->fRefineCoreOnly = fRefineCoreOnly;
    p->pReportFile = pReportFile;
    p->nOptimizeMode = nOptimizeMode;
    p->totalTimeout = totalTimeout;
    p->nDontCarePercent = nDontCarePercent;
    p->nOptimizeDenseKMax = nOptimizeDenseKMax;

    // Optional: derive target reset value by random multi-frame simulation (-r)
    // If user didn't explicitly provide -I, pass NULL so random sim starts from random state.
    // If user gave -I, pass that string so random sim starts from the specified state.
    char * pTargetInitStr = NULL;
    if ( p->fRandTarget )
    {
        int nSimFrames = (p->nRandomSim >= 0) ? p->nRandomSim : nFrames;
        char * pSimInit = p->fExplicitInit ? p->pInitStr : NULL;
        pTargetInitStr = Minr_DeriveTargetResetByRandomSim( pGia, pSimInit, nSimFrames, p->vLevel, p->seed );
        if ( pTargetInitStr )
            p->pInitStr = pTargetInitStr;
        else
            printf( "[Rand] Warning: failed to derive target reset value; using given -I\n" );
    }

    // Apply don't care masking (-D): randomly set a percentage of registers to 'x'
    if ( p->nDontCarePercent > 0 && p->fRandTarget )
    {
        int nRegs = Gia_ManRegNum(pGia);
        int nDC = nRegs * p->nDontCarePercent / 100;
        if ( nDC > 0 )
        {
            // Fisher-Yates shuffle to pick nDC random indices
            int * perm = ABC_ALLOC( int, nRegs );
            for ( int i = 0; i < nRegs; i++ ) perm[i] = i;
            // RNG already advanced by target derivation above; continue same stream.
            for ( int i = nRegs - 1; i > 0; i-- )
            {
                int j = Abc_Random(0) % (i + 1);
                int tmp = perm[i]; perm[i] = perm[j]; perm[j] = tmp;
            }
            for ( int i = 0; i < nDC; i++ )
                p->pInitStr[perm[i]] = 'x';
            ABC_FREE( perm );
            if ( p->vLevel > 0 )
                printf( "[DontCare] Set %d/%d registers (%.0f%%) to don't care.\n",
                        nDC, nRegs, 100.0 * nDC / nRegs );
        }
    }

    p->timeSolveStart = Abc_Clock();
    p->timeSolveEnd = 0;

    // 0. Pre-processing: Propagation & Cut (shared across all k values)
    Minr_PropagateAndCut(p);

    // Solve phase
    if ( nOptimizeMode == 1 )
    {
        Minr_SolveOptimize( p );
    }
    else if ( nOptimizeMode == 2 )
    {
        Minr_SolveOptimize2( p );
    }
    else
    {
        p->vVarMap = NULL;
        p->vClauses = NULL;
        double solverTimeout = (totalTimeout > 0) ? totalTimeout : 0;
        Minr_SolveSingleK(p, solverTimeout);
    }

    // Post-processing: Refine + Verify (runs once; -O 2 does its own inside Optimize2)
    if ( p->solverStatus == 1 && p->nOptimizeMode != 2 )
    {
        if ( p->nRefineMode > 0 )
        {
            abctime clkRef = Abc_Clock();
            Minr_SatRefine( p );
            p->timeRefine = Abc_Clock() - clkRef;
            if ( p->vLevel > 0 )
                Abc_PrintTime(1, "[Refine] Refine time", p->timeRefine);
        }
        // End of "runtime_sec" measurement: after refine, before verification.
        p->timeSolveEnd = Abc_Clock();
        // Always run x-simulation verify (computes simRegMismatchWeak/StrongPct)
        p->fVerifyPass = Minr_VerifyResult(p, NULL);
        // If refine modified result, also run SAT verify (overrides cut_verified)
        if ( p->nRefineMode > 0 && p->nRefineReleased > 0 )
            p->fVerifyPass = Minr_SatVerify( p );

        // CEC-based verification (compares all PO/RI, not just cut)
        p->fCecVerifyPass = Minr_CecVerify( p );
    }
    else if ( p->nOptimizeMode != 2 )
    {
        // No solution (or -x off): still stop timer before any verification.
        p->timeSolveEnd = Abc_Clock();
    }

    Minr_DumpReport( p );

    // Cleanup
    if (p->vVarMap) Vec_IntFree(p->vVarMap);
    if (p->vClauses) Vec_WecFree(p->vClauses);
    if (p->vPropVals) Vec_IntFree(p->vPropVals);
    if (p->vCutNodes) Vec_IntFree(p->vCutNodes);
    if (p->vEqCutNodes) Vec_IntFree(p->vEqCutNodes);
    if (p->vPiVals) Vec_IntFree(p->vPiVals);
    if (p->vRoVals0) Vec_IntFree(p->vRoVals0);
    if (p->vBestPiVals) Vec_IntFree(p->vBestPiVals);
    if (p->vBestRoVals0) Vec_IntFree(p->vBestRoVals0);
    if (p->vOptIterK) Vec_IntFree(p->vOptIterK);
    if (p->vOptIterResets) Vec_IntFree(p->vOptIterResets);
    if (p->vOptIterStatus) Vec_IntFree(p->vOptIterStatus);
    if (p->vOptIterTimeMs) Vec_IntFree(p->vOptIterTimeMs);
    if (p->vOpt2OuterSegmentTimeMs) Vec_IntFree(p->vOpt2OuterSegmentTimeMs);
    if (p->vOpt2OuterBestResets) Vec_IntFree(p->vOpt2OuterBestResets);
    if (p->vOpt2OuterTargetResets) Vec_IntFree(p->vOpt2OuterTargetResets);
    if (p->vOpt2OuterInnerK) Vec_WecFree(p->vOpt2OuterInnerK);
    if (p->vOpt2OuterInnerResets) Vec_WecFree(p->vOpt2OuterInnerResets);
    if (p->vOpt2OuterInnerTimeMs) Vec_WecFree(p->vOpt2OuterInnerTimeMs);
    if (pTargetInitStr) free(pTargetInitStr);
}

ABC_NAMESPACE_IMPL_END