/**CFile****************************************************************
  FileName    [minr_sat.cpp]
  SystemName  [ABC: Logic synthesis and verification system.]
  PackageName [Minr: MaxSAT-based partial reset minimization.]
  Synopsis    [SAT-based post-refine and verification.]
  Author      [Allen]
  Affiliation [NTU]
  Date        [Ver. 1.0. Created - Feb 22, 2026.]
***********************************************************************/

#include "minr.h"
#include "sat/bsat/satSolver.h"
#include "proof/cec/cec.h"
#include "aig/aig/aig.h"
#include "aig/gia/giaAig.h"
#include "sat/cnf/cnf.h"

ABC_NAMESPACE_IMPL_START

#define MINR_VAL_0 0
#define MINR_VAL_1 1
#define MINR_VAL_X 2

extern void Minr_SimulateTimeframe(Gia_Man_t * pGia, Vec_Int_t * vObjVals);

////////////////////////////////////////////////////////////////////////
///                        SAT CONTEXT                                ///
////////////////////////////////////////////////////////////////////////

typedef struct Minr_SatCtx_t_ {
    sat_solver * pSat;
    int          nFrames;
    int          nObjs;
    Vec_Int_t *  vObj2Var;  // size = nObjs * nFrames
    int          nVars;
    int          iMiterVar;
} Minr_SatCtx_t;

static inline int Minr_SatVar(Minr_SatCtx_t * ctx, int iObj, int frame) {
    return Vec_IntEntry(ctx->vObj2Var, iObj * ctx->nFrames + frame);
}

////////////////////////////////////////////////////////////////////////
///                   MODEL BUILDING                                  ///
////////////////////////////////////////////////////////////////////////

static Minr_SatCtx_t * Minr_BuildBinarySatModel(Gia_Man_t * pGia, int nFrames)
{
    Minr_SatCtx_t * ctx = ABC_ALLOC(Minr_SatCtx_t, 1);
    memset(ctx, 0, sizeof(Minr_SatCtx_t));

    int nObjs = Gia_ManObjNum(pGia);
    ctx->nFrames = nFrames;
    ctx->nObjs   = nObjs;
    ctx->vObj2Var = Vec_IntStartFull(nObjs * nFrames);
    ctx->nVars   = 0;
    ctx->iMiterVar = -1;

    for (int t = 0; t < nFrames; t++)
        for (int i = 0; i < nObjs; i++) {
            Vec_IntWriteEntry(ctx->vObj2Var, i * nFrames + t, ctx->nVars);
            ctx->nVars++;
        }

    ctx->pSat = sat_solver_new();
    sat_solver_setnvars(ctx->pSat, ctx->nVars);

    Gia_Obj_t * pObj;
    int iObj;

    for (int t = 0; t < nFrames; t++) {
        // Const0: var(0,t) = 0
        {
            lit Lits[1];
            Lits[0] = toLitCond(Minr_SatVar(ctx, 0, t), 1);
            sat_solver_addclause(ctx->pSat, Lits, Lits + 1);
        }

        Gia_ManForEachObj(pGia, pObj, iObj) {
            if (iObj == 0) continue;
            if (Gia_ObjIsAnd(pObj)) {
                sat_solver_add_and(ctx->pSat,
                    Minr_SatVar(ctx, iObj, t),
                    Minr_SatVar(ctx, Gia_ObjFaninId0(pObj, iObj), t),
                    Minr_SatVar(ctx, Gia_ObjFaninId1(pObj, iObj), t),
                    Gia_ObjFaninC0(pObj), Gia_ObjFaninC1(pObj), 0);
            }
            else if (Gia_ObjIsCo(pObj)) {
                sat_solver_add_buffer(ctx->pSat,
                    Minr_SatVar(ctx, iObj, t),
                    Minr_SatVar(ctx, Gia_ObjFaninId0(pObj, iObj), t),
                    Gia_ObjFaninC0(pObj));
            }
        }

        // Latch transition: RO_i(t+1) = RI_i(t)
        if (t < nFrames - 1) {
            int i;
            Gia_Obj_t * pRi;
            Gia_ManForEachRi(pGia, pRi, i) {
                Gia_Obj_t * pRo = Gia_ManRo(pGia, i);
                sat_solver_add_buffer(ctx->pSat,
                    Minr_SatVar(ctx, Gia_ObjId(pGia, pRo), t + 1),
                    Minr_SatVar(ctx, Gia_ObjId(pGia, pRi), t),
                    0);
            }
        }
    }
    return ctx;
}

static void Minr_SatCtxFree(Minr_SatCtx_t * ctx)
{
    if (ctx->pSat)    sat_solver_delete(ctx->pSat);
    if (ctx->vObj2Var) Vec_IntFree(ctx->vObj2Var);
    ABC_FREE(ctx);
}

////////////////////////////////////////////////////////////////////////
///              POST-SOLVE REFINE: Minr_SatRefine                    ///
////////////////////////////////////////////////////////////////////////

static void Minr_SatAddXor(sat_solver * pSat, int iVarC, int iVarA, int iVarB)
{
    lit Lits[3];
    Lits[0] = toLitCond(iVarC, 1); Lits[1] = toLitCond(iVarA, 1); Lits[2] = toLitCond(iVarB, 1);
    sat_solver_addclause(pSat, Lits, Lits+3);
    Lits[0] = toLitCond(iVarC, 1); Lits[1] = toLitCond(iVarA, 0); Lits[2] = toLitCond(iVarB, 0);
    sat_solver_addclause(pSat, Lits, Lits+3);
    Lits[0] = toLitCond(iVarC, 0); Lits[1] = toLitCond(iVarA, 1); Lits[2] = toLitCond(iVarB, 0);
    sat_solver_addclause(pSat, Lits, Lits+3);
    Lits[0] = toLitCond(iVarC, 0); Lits[1] = toLitCond(iVarA, 0); Lits[2] = toLitCond(iVarB, 1);
    sat_solver_addclause(pSat, Lits, Lits+3);
}

/**
 * Minr_SatRefine - UNSAT-core-based post-refine.
 *
 * Mode 0 (CEC): constraint = all POs/RIs equivalent between target and
 *   unrolled circuits. Don't-care target ROs bound to unrolled ROs at t=k.
 * Mode 1 (cut): constraint = each cut node at t=k matches its propagated value.
 *
 * Both modes: resets at t=0 are assumptions. Pass1 bulk-releases by UNSAT core,
 * then one-by-one trial with core-release after each success.
 */
void Minr_SatRefine(Minr_Man_t * p)
{
    Gia_Man_t * pGia = p->pGia;
    int nRegs = Gia_ManRegNum(pGia);
    int nPI   = Gia_ManPiNum(pGia);
    int nObjs = Gia_ManObjNum(pGia);
    Gia_Obj_t * pObj;
    int iObj, k;
    int mode = p->nRefineMode;

    if (!p->vPiVals || !p->vRoVals0) {
        printf("[Refine] No decoded result available, skipping.\n");
        return;
    }

    if (mode == 3)
        Minr_ExtractEqCut(p);

    if (p->vLevel > 0) {
        const char * modeStr = (mode == 1) ? "CEC output equiv" :
                              (mode == 2) ? (Minr_ManUsesSpecRegAtLastTf(p) ? "specified RO @k" : "constant cut") :
                              (mode == 3) ? "eq cut" : "?";
        printf("[Refine] Starting (mode %d: %s%s)...\n", mode, modeStr,
               ((mode == 1 || mode == 3) && p->fRefineBindDc) ? ", bind DC" : "");
    }

    int nSatFrames = p->nFrames + 1;
    int frameK     = p->nFrames;

    Minr_SatCtx_t * ctx = Minr_BuildBinarySatModel(pGia, nSatFrames);
    int nextVar = ctx->nVars;
    int miterVar = -1;

    // ---- Mode-specific constraint building ----
    if (mode == 1) {
        // CEC: add target circuit + miter on all COs
        int nCOs = Gia_ManCoNum(pGia);
        int * pTgtVars = ABC_ALLOC(int, nObjs);

        pTgtVars[0] = Minr_SatVar(ctx, 0, frameK);
        Gia_ManForEachPi(pGia, pObj, iObj)
            pTgtVars[Gia_ObjId(pGia, pObj)] = Minr_SatVar(ctx, Gia_ObjId(pGia, pObj), frameK);

        k = 0;
        Gia_ManForEachRo(pGia, pObj, iObj) {
            int roId = Gia_ObjId(pGia, pObj);
            if (p->pInitStr[k] == '0' || p->pInitStr[k] == '1')
                pTgtVars[roId] = nextVar++;
            else if (p->fRefineBindDc)
                pTgtVars[roId] = Minr_SatVar(ctx, roId, frameK);
            else
                pTgtVars[roId] = nextVar++;
            k++;
        }
        Gia_ManForEachObj(pGia, pObj, iObj) {
            if (iObj == 0 || Gia_ObjIsCi(pObj)) continue;
            pTgtVars[iObj] = nextVar++;
        }

        int * pXorVars = ABC_ALLOC(int, nCOs);
        for (int i = 0; i < nCOs; i++) pXorVars[i] = nextVar++;
        int * pOrVars = NULL;
        if (nCOs > 1) {
            pOrVars = ABC_ALLOC(int, nCOs - 1);
            for (int i = 0; i < nCOs - 1; i++) pOrVars[i] = nextVar++;
        }

        sat_solver_setnvars(ctx->pSat, nextVar);
        ctx->nVars = nextVar;

        k = 0;
        Gia_ManForEachRo(pGia, pObj, iObj) {
            int roId = Gia_ObjId(pGia, pObj);
            if (p->pInitStr[k] == '0') {
                lit L = toLitCond(pTgtVars[roId], 1);
                sat_solver_addclause(ctx->pSat, &L, &L + 1);
            } else if (p->pInitStr[k] == '1') {
                lit L = toLitCond(pTgtVars[roId], 0);
                sat_solver_addclause(ctx->pSat, &L, &L + 1);
            }
            k++;
        }

        Gia_ManForEachObj(pGia, pObj, iObj) {
            if (iObj == 0 || Gia_ObjIsCi(pObj)) continue;
            if (Gia_ObjIsAnd(pObj))
                sat_solver_add_and(ctx->pSat, pTgtVars[iObj],
                    pTgtVars[Gia_ObjFaninId0(pObj, iObj)],
                    pTgtVars[Gia_ObjFaninId1(pObj, iObj)],
                    Gia_ObjFaninC0(pObj), Gia_ObjFaninC1(pObj), 0);
            else if (Gia_ObjIsCo(pObj))
                sat_solver_add_buffer(ctx->pSat, pTgtVars[iObj],
                    pTgtVars[Gia_ObjFaninId0(pObj, iObj)], Gia_ObjFaninC0(pObj));
        }

        int coIdx = 0;
        Gia_ManForEachCo(pGia, pObj, iObj) {
            int coId = Gia_ObjId(pGia, pObj);
            Minr_SatAddXor(ctx->pSat, pXorVars[coIdx],
                           pTgtVars[coId], Minr_SatVar(ctx, coId, frameK));
            coIdx++;
        }

        if (nCOs == 1) {
            miterVar = pXorVars[0];
        } else {
            sat_solver_add_and(ctx->pSat, pOrVars[0], pXorVars[0], pXorVars[1], 1, 1, 1);
            for (int i = 2; i < nCOs; i++)
                sat_solver_add_and(ctx->pSat, pOrVars[i-1], pOrVars[i-2], pXorVars[i], 1, 1, 1);
            miterVar = pOrVars[nCOs - 2];
        }

        ABC_FREE(pTgtVars);
        ABC_FREE(pXorVars);
        if (pOrVars) ABC_FREE(pOrVars);

    } else if (mode == 3) {
        // Mode 3 (eq cut): target circuit, no bind free ROs, equiv on eq cut
        if (!p->vEqCutNodes || Vec_IntSize(p->vEqCutNodes) == 0) {
            printf("[Refine] Eq cut empty, skipping.\n");
            Minr_SatCtxFree(ctx);
            return;
        }
        int * pTgtVars = ABC_ALLOC(int, nObjs);
        pTgtVars[0] = Minr_SatVar(ctx, 0, frameK);
        Gia_ManForEachPi(pGia, pObj, iObj)
            pTgtVars[Gia_ObjId(pGia, pObj)] = Minr_SatVar(ctx, Gia_ObjId(pGia, pObj), frameK);

        k = 0;
        Gia_ManForEachRo(pGia, pObj, iObj) {
            int roId = Gia_ObjId(pGia, pObj);
            if (p->pInitStr[k] == '0' || p->pInitStr[k] == '1')
                pTgtVars[roId] = nextVar++;
            else if (p->fRefineBindDc)
                pTgtVars[roId] = Minr_SatVar(ctx, roId, frameK);
            else
                pTgtVars[roId] = nextVar++;
            k++;
        }
        Gia_ManForEachObj(pGia, pObj, iObj) {
            if (iObj == 0 || Gia_ObjIsCi(pObj)) continue;
            pTgtVars[iObj] = nextVar++;
        }

        int nEqCut = Vec_IntSize(p->vEqCutNodes);
        int * pXorVars = ABC_ALLOC(int, nEqCut);
        for (int i = 0; i < nEqCut; i++) pXorVars[i] = nextVar++;
        int * pOrVars = NULL;
        if (nEqCut > 1) {
            pOrVars = ABC_ALLOC(int, nEqCut - 1);
            for (int i = 0; i < nEqCut - 1; i++) pOrVars[i] = nextVar++;
        }

        sat_solver_setnvars(ctx->pSat, nextVar);
        ctx->nVars = nextVar;

        k = 0;
        Gia_ManForEachRo(pGia, pObj, iObj) {
            int roId = Gia_ObjId(pGia, pObj);
            if (p->pInitStr[k] == '0') {
                lit L = toLitCond(pTgtVars[roId], 1);
                sat_solver_addclause(ctx->pSat, &L, &L + 1);
            } else if (p->pInitStr[k] == '1') {
                lit L = toLitCond(pTgtVars[roId], 0);
                sat_solver_addclause(ctx->pSat, &L, &L + 1);
            }
            k++;
        }

        Gia_ManForEachObj(pGia, pObj, iObj) {
            if (iObj == 0 || Gia_ObjIsCi(pObj)) continue;
            if (Gia_ObjIsAnd(pObj))
                sat_solver_add_and(ctx->pSat, pTgtVars[iObj],
                    pTgtVars[Gia_ObjFaninId0(pObj, iObj)],
                    pTgtVars[Gia_ObjFaninId1(pObj, iObj)],
                    Gia_ObjFaninC0(pObj), Gia_ObjFaninC1(pObj), 0);
            else if (Gia_ObjIsCo(pObj))
                sat_solver_add_buffer(ctx->pSat, pTgtVars[iObj],
                    pTgtVars[Gia_ObjFaninId0(pObj, iObj)], Gia_ObjFaninC0(pObj));
        }

        int eqIdx = 0, NodeId;
        Vec_IntForEachEntry(p->vEqCutNodes, NodeId, k) {
            Minr_SatAddXor(ctx->pSat, pXorVars[eqIdx],
                pTgtVars[NodeId], Minr_SatVar(ctx, NodeId, frameK));
            eqIdx++;
        }

        if (nEqCut == 1) {
            miterVar = pXorVars[0];
        } else {
            sat_solver_add_and(ctx->pSat, pOrVars[0], pXorVars[0], pXorVars[1], 1, 1, 1);
            for (int i = 2; i < nEqCut; i++)
                sat_solver_add_and(ctx->pSat, pOrVars[i-1], pOrVars[i-2], pXorVars[i], 1, 1, 1);
            miterVar = pOrVars[nEqCut - 2];
        }

        if (p->vLevel > 0)
            printf("[Refine] Eq cut size: %d\n", nEqCut);

        ABC_FREE(pTgtVars);
        ABC_FREE(pXorVars);
        if (pOrVars) ABC_FREE(pOrVars);

    } else if (mode == 2) {
        // Mode 2 (cut): each valid cut node at t=k must match propagated value;
        // with -S, instead constrain specified target ROs at t=k to 0/1.
        int nValidCut = 0;
        if (Minr_ManUsesSpecRegAtLastTf(p)) {
            int ri;
            Gia_Obj_t * pRo;
            Gia_ManForEachRo(pGia, pRo, ri)
                if (p->pInitStr[ri] == '0' || p->pInitStr[ri] == '1') nValidCut++;
        } else {
            int NodeId;
            Vec_IntForEachEntry(p->vCutNodes, NodeId, k) {
                if (Vec_IntEntry(p->vPropVals, NodeId) != MINR_VAL_X) nValidCut++;
            }
        }
        if (nValidCut == 0) {
            printf("[Refine] No valid last-timeframe constraint nodes, skipping.\n");
            Minr_SatCtxFree(ctx);
            return;
        }

        int * pXorVars = ABC_ALLOC(int, nValidCut);
        for (int i = 0; i < nValidCut; i++) pXorVars[i] = nextVar++;
        int * pOrVars = NULL;
        if (nValidCut > 1) {
            pOrVars = ABC_ALLOC(int, nValidCut - 1);
            for (int i = 0; i < nValidCut - 1; i++) pOrVars[i] = nextVar++;
        }
        sat_solver_setnvars(ctx->pSat, nextVar);
        ctx->nVars = nextVar;

        {
            int idx = 0;
            if (Minr_ManUsesSpecRegAtLastTf(p)) {
                int ri;
                Gia_Obj_t * pRo;
                Gia_ManForEachRo(pGia, pRo, ri) {
                    char c = p->pInitStr[ri];
                    if (c != '0' && c != '1') continue;
                    int Val = (c == '0') ? MINR_VAL_0 : MINR_VAL_1;
                    int iRo = Gia_ObjId(pGia, pRo);
                    sat_solver_add_buffer(ctx->pSat, pXorVars[idx],
                        Minr_SatVar(ctx, iRo, frameK), Val);
                    idx++;
                }
            } else {
                int NodeId;
                Vec_IntForEachEntry(p->vCutNodes, NodeId, k) {
                    int Val = Vec_IntEntry(p->vPropVals, NodeId);
                    if (Val == MINR_VAL_X) continue;
                    sat_solver_add_buffer(ctx->pSat, pXorVars[idx],
                        Minr_SatVar(ctx, NodeId, frameK), Val);
                    idx++;
                }
            }
        }

        if (nValidCut == 1) {
            miterVar = pXorVars[0];
        } else {
            sat_solver_add_and(ctx->pSat, pOrVars[0], pXorVars[0], pXorVars[1], 1, 1, 1);
            for (int i = 2; i < nValidCut; i++)
                sat_solver_add_and(ctx->pSat, pOrVars[i-1], pOrVars[i-2], pXorVars[i], 1, 1, 1);
            miterVar = pOrVars[nValidCut - 2];
        }

        ABC_FREE(pXorVars);
        if (pOrVars) ABC_FREE(pOrVars);
    }

    // ---- Common: fix PIs at t<k ----
    for (int t = 0; t < p->nFrames; t++) {
        int iPi;
        Gia_ManForEachPi(pGia, pObj, iPi) {
            int piVal = Vec_IntEntry(p->vPiVals, t * nPI + iPi);
            lit L = toLitCond(Minr_SatVar(ctx, Gia_ObjId(pGia, pObj), t), !piVal);
            sat_solver_addclause(ctx->pSat, &L, &L + 1);
        }
    }
    // ---- Fix PI at t=k when vPiAtK set (-O 2, iteration > 1) ----
    if (p->vPiAtK) {
        int iPi;
        Gia_ManForEachPi(pGia, pObj, iPi) {
            int piVal = Vec_IntEntry(p->vPiAtK, iPi);
            lit L = toLitCond(Minr_SatVar(ctx, Gia_ObjId(pGia, pObj), frameK), !piVal);
            sat_solver_addclause(ctx->pSat, &L, &L + 1);
        }
    }

    // ---- Common: collect reset assumptions ----
    Vec_Int_t * vResetLit = Vec_IntAlloc(nRegs);
    Vec_Int_t * vResetReg = Vec_IntAlloc(nRegs);

    int nFixed = 0;
    k = 0;
    Gia_ManForEachRo(pGia, pObj, iObj) {
        int roVal = Vec_IntEntry(p->vRoVals0, k);
        if (roVal == MINR_VAL_0 || roVal == MINR_VAL_1) {
            Vec_IntPush(vResetLit,
                toLitCond(Minr_SatVar(ctx, Gia_ObjId(pGia, pObj), 0),
                          (roVal == MINR_VAL_0) ? 1 : 0));
            Vec_IntPush(vResetReg, k);
            nFixed++;
        }
        k++;
    }

    Vec_Int_t * vActive = Vec_IntStart(nFixed);
    for (int i = 0; i < nFixed; i++)
        Vec_IntWriteEntry(vActive, i, 1);

    if (p->vLevel > 0)
        printf("[Refine] %d reset assumptions, miter var=%d, SAT vars=%d\n",
               nFixed, miterVar, ctx->nVars);

    auto buildAssumps = [&](int skipIdx) -> Vec_Int_t * {
        Vec_Int_t * v = Vec_IntAlloc(nFixed + 1);
        Vec_IntPush(v, toLitCond(miterVar, 0));
        for (int i = 0; i < nFixed; i++) {
            if (!Vec_IntEntry(vActive, i)) continue;
            if (i == skipIdx) continue;
            Vec_IntPush(v, Vec_IntEntry(vResetLit, i));
        }
        return v;
    };

    int nByTrial = 0, nByCore = 0;

    auto releaseByCore = [&]() -> int {
        int * pCore;
        int nCore = sat_solver_final(ctx->pSat, &pCore);
        if (nCore == 0) return 0;  // empty core (e.g. root_level==0) -> release nothing
        Vec_Int_t * vCoreVarSet = Vec_IntStart(ctx->nVars);
        for (int ci = 0; ci < nCore; ci++)
            Vec_IntWriteEntry(vCoreVarSet, lit_var(pCore[ci]), 1);

        int nRel = 0;
        for (int i = 0; i < nFixed; i++) {
            if (!Vec_IntEntry(vActive, i)) continue;
            int aVar = lit_var(Vec_IntEntry(vResetLit, i));
            if (!Vec_IntEntry(vCoreVarSet, aVar)) {
                Vec_IntWriteEntry(vActive, i, 0);
                Vec_IntWriteEntry(p->vRoVals0, Vec_IntEntry(vResetReg, i), MINR_VAL_X);
                nRel++;
                if (p->vLevel > 1)
                    printf("  [Refine] core-release FF[%d]\n", Vec_IntEntry(vResetReg, i));
            }
        }
        Vec_IntFree(vCoreVarSet);
        nByCore += nRel;
        if (p->vLevel > 0 && nRel > 0) {
            int nCur = 0;
            for (int j = 0; j < nFixed; j++)
                if (Vec_IntEntry(vActive, j)) nCur++;
            printf("[Refine] core: released=%d, remaining=%d\n", nRel, nCur);
        }
        return nRel;
    };

    // ---- Pass 1: solve with all resets → UNSAT core bulk release ----
    {
        Vec_Int_t * vA = buildAssumps(-1);
        int nConf = p->nRefineConfLimit;
        int status = sat_solver_solve(ctx->pSat,
            Vec_IntArray(vA), Vec_IntArray(vA) + Vec_IntSize(vA), nConf, 0, 0, 0);
        Vec_IntFree(vA);

        /* NOTE: If conflict limit reached, status can be l_Undef (unknown).
           In refine, unknown must be treated as "cannot release". */
        if (status != l_False) {
            if (status == l_Undef)
                printf("[Refine] WARNING: initial solve is UNKNOWN (conflict limit reached) — no resets released.\n");
            else
                printf("[Refine] WARNING: initial solve is SAT — no resets released.\n");
            goto cleanup;
        }
        releaseByCore();
    }

    // ---- Pass 2: one-by-one trial, no outer loop ----
    if (p->fRefineCoreOnly)
        goto cleanup;
    for (int i = 0; i < nFixed; i++) {
        if (!Vec_IntEntry(vActive, i)) continue;

        Vec_Int_t * vA = buildAssumps(i);
        int nConf = p->nRefineConfLimit;
        int status = sat_solver_solve(ctx->pSat,
            Vec_IntArray(vA), Vec_IntArray(vA) + Vec_IntSize(vA), nConf, 0, 0, 0);
        Vec_IntFree(vA);

        /* Only UNSAT allows releasing this reset. SAT or UNKNOWN => keep it. */
        if (status == l_False) {
            Vec_IntWriteEntry(vActive, i, 0);
            Vec_IntWriteEntry(p->vRoVals0, Vec_IntEntry(vResetReg, i), MINR_VAL_X);
            nByTrial++;
            if (p->vLevel > 1)
                printf("  [Refine] trial-release FF[%d]\n", Vec_IntEntry(vResetReg, i));
            releaseByCore();
        }
    }

    if (p->vLevel > 0) {
        int nFinal = 0;
        for (int i = 0; i < nFixed; i++)
            if (Vec_IntEntry(vActive, i)) nFinal++;
        printf("[Refine] Done. released=%d (trial=%d, core=%d), final=%d/%d\n",
               nByTrial + nByCore, nByTrial, nByCore, nFinal, nFixed);
    }

cleanup:
    p->nRefineReleased = nByTrial + nByCore;
    p->nRefineByTrial  = nByTrial;
    p->nRefineByCore   = nByCore;

    Vec_IntFree(vResetLit);
    Vec_IntFree(vResetReg);
    Vec_IntFree(vActive);
    Minr_SatCtxFree(ctx);
}

////////////////////////////////////////////////////////////////////////
///                  SAT-BASED VERIFICATION                           ///
////////////////////////////////////////////////////////////////////////

int Minr_SatVerify(Minr_Man_t * p)
{
    Gia_Man_t * pGia = p->pGia;
    int nRegs = Gia_ManRegNum(pGia);
    int nPI   = Gia_ManPiNum(pGia);
    Gia_Obj_t * pObj;
    int iObj, k;

    if (!p->vPiVals || !p->vRoVals0) {
        printf("[SatVerify] No decoded result available, cannot verify.\n");
        return 0;
    }

    int nSatFrames = p->nFrames + 1;
    int frameK     = p->nFrames;

    Minr_SatCtx_t * ctx = Minr_BuildBinarySatModel(pGia, nSatFrames);

    // Build miter at t=k (constant cut or specified ROs when -S)
    int nValidCut = 0;
    if (Minr_ManUsesSpecRegAtLastTf(p)) {
        int ri;
        Gia_Obj_t * pRo;
        Gia_ManForEachRo(pGia, pRo, ri)
            if (p->pInitStr[ri] == '0' || p->pInitStr[ri] == '1') nValidCut++;
    } else {
        int NodeId;
        Vec_IntForEachEntry(p->vCutNodes, NodeId, k) {
            if (Vec_IntEntry(p->vPropVals, NodeId) != MINR_VAL_X) nValidCut++;
        }
    }

    if (nValidCut == 0) {
        printf("[SatVerify] No valid last-timeframe constraints to check. Trivially PASS.\n");
        Minr_SatCtxFree(ctx);
        return 1;
    }

    int nExtra = nValidCut + (nValidCut > 1 ? nValidCut - 1 : 0);
    sat_solver_setnvars(ctx->pSat, ctx->nVars + nExtra);

    int nextVar = ctx->nVars;
    int * pXorVars = ABC_ALLOC(int, nValidCut);

    {
        int idx = 0;
        if (Minr_ManUsesSpecRegAtLastTf(p)) {
            int ri;
            Gia_Obj_t * pRo;
            Gia_ManForEachRo(pGia, pRo, ri) {
                char c = p->pInitStr[ri];
                if (c != '0' && c != '1') continue;
                int Val = (c == '0') ? MINR_VAL_0 : MINR_VAL_1;
                int iRo = Gia_ObjId(pGia, pRo);
                pXorVars[idx] = nextVar++;
                sat_solver_add_buffer(ctx->pSat,
                    pXorVars[idx],
                    Minr_SatVar(ctx, iRo, frameK),
                    Val);
                idx++;
            }
        } else {
            int NodeId;
            Vec_IntForEachEntry(p->vCutNodes, NodeId, k) {
                int Val = Vec_IntEntry(p->vPropVals, NodeId);
                if (Val == MINR_VAL_X) continue;
                pXorVars[idx] = nextVar++;
                sat_solver_add_buffer(ctx->pSat,
                    pXorVars[idx],
                    Minr_SatVar(ctx, NodeId, frameK),
                    Val);
                idx++;
            }
        }
    }

    if (nValidCut == 1) {
        ctx->iMiterVar = pXorVars[0];
    } else {
        int prev = pXorVars[0];
        for (int i = 1; i < nValidCut; i++) {
            int orVar = nextVar++;
            sat_solver_add_and(ctx->pSat, orVar, prev, pXorVars[i], 1, 1, 1);
            prev = orVar;
        }
        ctx->iMiterVar = prev;
    }
    ABC_FREE(pXorVars);

    // Assumptions: fix PIs for t=0..k-1, fix non-X ROs at t=0
    Vec_Int_t * vAssumps = Vec_IntAlloc(nSatFrames * nPI + nRegs + 1);

    for (int t = 0; t < p->nFrames; t++) {
        int iPi;
        Gia_ManForEachPi(pGia, pObj, iPi) {
            int piVal = Vec_IntEntry(p->vPiVals, t * nPI + iPi);
            Vec_IntPush(vAssumps,
                toLitCond(Minr_SatVar(ctx, Gia_ObjId(pGia, pObj), t), !piVal));
        }
    }

    k = 0;
    Gia_ManForEachRo(pGia, pObj, iObj) {
        int roVal = Vec_IntEntry(p->vRoVals0, k);
        if (roVal == MINR_VAL_0 || roVal == MINR_VAL_1)
            Vec_IntPush(vAssumps,
                toLitCond(Minr_SatVar(ctx, Gia_ObjId(pGia, pObj), 0),
                          (roVal == MINR_VAL_0) ? 1 : 0));
        k++;
    }

    // assume M=1 (try to find a cut mismatch)
    Vec_IntPush(vAssumps, toLitCond(ctx->iMiterVar, 0));

    int status = sat_solver_solve(ctx->pSat,
        Vec_IntArray(vAssumps),
        Vec_IntArray(vAssumps) + Vec_IntSize(vAssumps),
        0, 0, 0, 0);

    Vec_IntFree(vAssumps);

    int fPass = (status == l_False);
    if (fPass) {
        if (p->vLevel > 0)
            printf("[SatVerify] SUCCESS. No cut mismatch possible (UNSAT).\n");
    } else {
        printf("[SatVerify] FAILED. Cut mismatch exists (status=%s).\n",
               (status == l_True) ? "SAT" : "UNDEF");
    }

    Minr_SatCtxFree(ctx);
    return fPass;
}

////////////////////////////////////////////////////////////////////////
///               CEC-BASED VERIFICATION (FRAIG)                      ///
////////////////////////////////////////////////////////////////////////

/**
 * Minr_CecVerify - Verify initialization sequence by combinational
 * equivalence checking.
 *
 * Builds two combinational circuits inside a single GIA:
 *   1. Target circuit: single timeframe, ROs replaced by target constants.
 *   2. Unrolled circuit: k+1 timeframes with decoded PI sequence and
 *      reset values.  Unknown (X) ROs at t=0 become free PI variables.
 *
 * A miter (OR of XOR of all PO/RI pairs) is formed.  If the miter
 * reduces to constant 0 after structural hashing + SAT sweeping (fraig),
 * the initialization sequence is correct.
 *
 * Assumes: target initial state has no unknown FFs.
 */
int Minr_CecVerify(Minr_Man_t * p)
{
    Gia_Man_t * pGia = p->pGia;
    int nPI     = Gia_ManPiNum(pGia);
    int nPO     = Gia_ManPoNum(pGia);
    int nRegs   = Gia_ManRegNum(pGia);
    int nObjs   = Gia_ManObjNum(pGia);
    int nFrames = p->nFrames;
    Gia_Obj_t * pObj;
    int i, iObj;

    if (!p->vPiVals || !p->vRoVals0) {
        printf("[CecVerify] No decoded result available, cannot verify.\n");
        return 0;
    }

    int nDcTarget = 0;
    for (i = 0; i < nRegs; i++)
        if (p->pInitStr[i] != '0' && p->pInitStr[i] != '1') nDcTarget++;

    if (p->vLevel > 0)
        printf("[CecVerify] Building miter GIA (k=%d, nPI=%d, nPO=%d, nRegs=%d, dc_target=%d)...\n",
               nFrames, nPI, nPO, nRegs, nDcTarget);

    int nUnkRo = 0;
    for (i = 0; i < nRegs; i++)
        if (Vec_IntEntry(p->vRoVals0, i) == MINR_VAL_X) nUnkRo++;
    if (p->vLevel > 0)
        printf("[CecVerify] Unknown ROs at t=0: %d (free PIs in miter)\n", nUnkRo);

    // --- Allocate miter GIA with structural hashing ---
    Gia_Man_t * pNew = Gia_ManStart(nObjs * (nFrames + 2) + 100);
    Gia_ManHashAlloc(pNew);

    // Shared PIs (circuit PIs at t=k for unrolled / PIs for target)
    int * pSharedPiLits = ABC_ALLOC(int, nPI);
    for (i = 0; i < nPI; i++)
        pSharedPiLits[i] = Gia_ManAppendCi(pNew);

    // Free PIs for unknown ROs at t=0
    int * pUnkRoLits = ABC_ALLOC(int, nRegs);
    Gia_ManForEachRo(pGia, pObj, i) {
        if (Vec_IntEntry(p->vRoVals0, i) == MINR_VAL_X)
            pUnkRoLits[i] = Gia_ManAppendCi(pNew);
        else
            pUnkRoLits[i] = -1;
    }

    // ==== Unrolled Circuit FIRST (need t=k register values for don't care binding) ====
    int nF1 = nFrames + 1;
    Vec_Int_t * vUnr = Vec_IntStartFull(nObjs * nF1);

    for (int t = 0; t <= nFrames; t++) {
        Vec_IntWriteEntry(vUnr, 0 * nF1 + t, 0);

        if (t < nFrames) {
            Gia_ManForEachPi(pGia, pObj, i) {
                int piVal = Vec_IntEntry(p->vPiVals, t * nPI + i);
                Vec_IntWriteEntry(vUnr, Gia_ObjId(pGia, pObj) * nF1 + t,
                                  piVal ? 1 : 0);
            }
        } else {
            Gia_ManForEachPi(pGia, pObj, i)
                Vec_IntWriteEntry(vUnr, Gia_ObjId(pGia, pObj) * nF1 + t,
                                  pSharedPiLits[i]);
        }

        if (t == 0) {
            Gia_ManForEachRo(pGia, pObj, i) {
                int roVal = Vec_IntEntry(p->vRoVals0, i);
                int lit;
                if (roVal == MINR_VAL_0)      lit = 0;
                else if (roVal == MINR_VAL_1) lit = 1;
                else                          lit = pUnkRoLits[i];
                Vec_IntWriteEntry(vUnr, Gia_ObjId(pGia, pObj) * nF1 + t, lit);
            }
        } else {
            Gia_ManForEachRi(pGia, pObj, i) {
                Gia_Obj_t * pRo = Gia_ManRo(pGia, i);
                int riLit = Vec_IntEntry(vUnr, Gia_ObjId(pGia, pObj) * nF1 + (t - 1));
                Vec_IntWriteEntry(vUnr, Gia_ObjId(pGia, pRo) * nF1 + t, riLit);
            }
        }

        Gia_ManForEachObj(pGia, pObj, iObj) {
            if (iObj == 0 || Gia_ObjIsCi(pObj)) continue;
            if (Gia_ObjIsAnd(pObj)) {
                int lit0 = Vec_IntEntry(vUnr, Gia_ObjFaninId0(pObj, iObj) * nF1 + t);
                int lit1 = Vec_IntEntry(vUnr, Gia_ObjFaninId1(pObj, iObj) * nF1 + t);
                if (Gia_ObjFaninC0(pObj)) lit0 = Abc_LitNot(lit0);
                if (Gia_ObjFaninC1(pObj)) lit1 = Abc_LitNot(lit1);
                Vec_IntWriteEntry(vUnr, iObj * nF1 + t,
                                  Gia_ManHashAnd(pNew, lit0, lit1));
            }
            else if (Gia_ObjIsCo(pObj)) {
                int lit0 = Vec_IntEntry(vUnr, Gia_ObjFaninId0(pObj, iObj) * nF1 + t);
                if (Gia_ObjFaninC0(pObj)) lit0 = Abc_LitNot(lit0);
                Vec_IntWriteEntry(vUnr, iObj * nF1 + t, lit0);
            }
        }
    }

    // ==== Target Circuit (single timeframe) ====
    // Specified ROs → constants; don't care ROs → bound to unrolled RO value at t=k
    Vec_Int_t * vTgt = Vec_IntStartFull(nObjs);
    Vec_IntWriteEntry(vTgt, 0, 0);

    Gia_ManForEachPi(pGia, pObj, i)
        Vec_IntWriteEntry(vTgt, Gia_ObjId(pGia, pObj), pSharedPiLits[i]);

    Gia_ManForEachRo(pGia, pObj, i) {
        int lit;
        if (p->pInitStr[i] == '1')      lit = 1;
        else if (p->pInitStr[i] == '0') lit = 0;
        else  // don't care: bind to unrolled register value at t=k
            lit = Vec_IntEntry(vUnr, Gia_ObjId(pGia, pObj) * nF1 + nFrames);
        Vec_IntWriteEntry(vTgt, Gia_ObjId(pGia, pObj), lit);
    }

    Gia_ManForEachObj(pGia, pObj, iObj) {
        if (iObj == 0 || Gia_ObjIsCi(pObj)) continue;
        if (Gia_ObjIsAnd(pObj)) {
            int lit0 = Vec_IntEntry(vTgt, Gia_ObjFaninId0(pObj, iObj));
            int lit1 = Vec_IntEntry(vTgt, Gia_ObjFaninId1(pObj, iObj));
            if (Gia_ObjFaninC0(pObj)) lit0 = Abc_LitNot(lit0);
            if (Gia_ObjFaninC1(pObj)) lit1 = Abc_LitNot(lit1);
            Vec_IntWriteEntry(vTgt, iObj, Gia_ManHashAnd(pNew, lit0, lit1));
        }
        else if (Gia_ObjIsCo(pObj)) {
            int lit0 = Vec_IntEntry(vTgt, Gia_ObjFaninId0(pObj, iObj));
            if (Gia_ObjFaninC0(pObj)) lit0 = Abc_LitNot(lit0);
            Vec_IntWriteEntry(vTgt, iObj, lit0);
        }
    }

    // ==== Miter: XOR all PO/RI pairs, OR together ====
    int miterLit = 0;

    Gia_ManForEachPo(pGia, pObj, i) {
        int iId = Gia_ObjId(pGia, pObj);
        int xorLit = Gia_ManHashXor(pNew,
            Vec_IntEntry(vTgt, iId),
            Vec_IntEntry(vUnr, iId * nF1 + nFrames));
        miterLit = Gia_ManHashOr(pNew, miterLit, xorLit);
    }
    Gia_ManForEachRi(pGia, pObj, i) {
        int iId = Gia_ObjId(pGia, pObj);
        int xorLit = Gia_ManHashXor(pNew,
            Vec_IntEntry(vTgt, iId),
            Vec_IntEntry(vUnr, iId * nF1 + nFrames));
        miterLit = Gia_ManHashOr(pNew, miterLit, xorLit);
    }

    Gia_ManAppendCo(pNew, miterLit);
    Gia_ManSetRegNum(pNew, 0);
    Gia_ManHashStop(pNew);

    // Cleanup dangling nodes
    Gia_Man_t * pMiter = Gia_ManCleanup(pNew);
    Gia_ManStop(pNew);

    if (p->vLevel > 0)
        printf("[CecVerify] Miter built: PI=%d, AND=%d, PO=%d\n",
               Gia_ManPiNum(pMiter), Gia_ManAndNum(pMiter), Gia_ManPoNum(pMiter));

    Vec_IntFree(vTgt);
    Vec_IntFree(vUnr);
    ABC_FREE(pSharedPiLits);
    ABC_FREE(pUnkRoLits);

    // ==== Check trivial constant-0 ====
    int fPass = 0;
    {
        Gia_Obj_t * pPo = Gia_ManPo(pMiter, 0);
        int poId = Gia_ObjId(pMiter, pPo);
        if (Gia_ObjFaninId0(pPo, poId) == 0 && Gia_ObjFaninC0(pPo) == 0) {
            fPass = 1;
            if (p->vLevel > 0)
                printf("[CecVerify] Miter is trivially constant 0 after hashing. PASS.\n");
        }
    }

    // ==== SAT sweeping (fraig) ====
    if (!fPass) {
        if (p->vLevel > 0)
            printf("[CecVerify] Running fraig (SAT sweeping)...\n");

        Cec_ParFra_t ParsFra;
        Cec_ManFraSetDefaultParams(&ParsFra);
        ParsFra.fVerbose = (p->vLevel > 1);
        ParsFra.nBTLimit = 10000000;  // remove effort limit for definitive result
        ParsFra.nItersMax = 1000;

        Gia_Man_t * pFraig = Cec_ManSatSweeping(pMiter, &ParsFra, (p->vLevel <= 1));
        if (pFraig) {
            if (p->vLevel > 0)
                printf("[CecVerify] After fraig: PI=%d, AND=%d\n",
                       Gia_ManPiNum(pFraig), Gia_ManAndNum(pFraig));

            Gia_Obj_t * pPo = Gia_ManPo(pFraig, 0);
            int poId = Gia_ObjId(pFraig, pPo);
            if (Gia_ObjFaninId0(pPo, poId) == 0 && Gia_ObjFaninC0(pPo) == 0)
                fPass = 1;

            Gia_ManStop(pFraig);
        }
    }

    // ==== Direct SAT fallback when fraig did not reduce to constant 0 ====
    if (!fPass) {
        if (p->vLevel > 0)
            printf("[CecVerify] Fraig inconclusive. Running direct SAT (no limit) to decide equivalence...\n");

        Aig_Man_t * pAig = Gia_ManToAigSimple(pMiter);
        if (pAig) {
            Cnf_Dat_t * pCnf = Cnf_DeriveSimple(pAig, 1);
            if (pCnf) {
                sat_solver * pSat = sat_solver_new();
                sat_solver_setnvars(pSat, pCnf->nVars);
                int i;
                for (i = 0; i < pCnf->nClauses; i++) {
                    if (!sat_solver_addclause(pSat, pCnf->pClauses[i], pCnf->pClauses[i+1])) {
                        sat_solver_delete(pSat);
                        Cnf_DataFree(pCnf);
                        Aig_ManStop(pAig);
                        goto done_sat_fallback;
                    }
                }
                Aig_Obj_t * pCo = Aig_ManCo(pAig, 0);
                int poVar = pCnf->pVarNums[pCo->Id];
                if (poVar >= 0) {
                    lit Lits[1];
                    Lits[0] = toLitCond(poVar, 0);  // PO = 1 (miter output high)
                    if (sat_solver_addclause(pSat, Lits, Lits + 1)) {
                        int status = sat_solver_solve(pSat, NULL, NULL, (ABC_INT64_T)0, (ABC_INT64_T)0, (ABC_INT64_T)0, (ABC_INT64_T)0);
                        if (status == l_False) {
                            fPass = 1;
                            if (p->vLevel > 0)
                                printf("[CecVerify] Direct SAT: UNSAT (equivalent).\n");
                        } else if (status == l_True && p->vLevel > 0) {
                            printf("[CecVerify] Direct SAT: SAT (not equivalent).\n");
                        }
                    }
                }
                sat_solver_delete(pSat);
                Cnf_DataFree(pCnf);
            }
            Aig_ManStop(pAig);
        }
done_sat_fallback:;
    }

    if (fPass)
        printf("[CecVerify] SUCCESS. Initialization sequence verified (combinational equivalence).\n");
    else
        printf("[CecVerify] FAILED. Miter did not reduce to constant 0.\n");

    Gia_ManStop(pMiter);
    return fPass;
}

ABC_NAMESPACE_IMPL_END
