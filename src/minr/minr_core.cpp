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
#include <sys/stat.h> // For mkdir
#include <unistd.h>   // For access()

ABC_NAMESPACE_IMPL_START

////////////////////////////////////////////////////////////////////////
///                        CONSTANTS & MACROS                        ///
////////////////////////////////////////////////////////////////////////

// 3-Value Logic Constants
#define MINR_VAL_0 0
#define MINR_VAL_1 1
#define MINR_VAL_X 2

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

// Random Helper (0 or 1)
static inline int Minr_RandomBinary( int seed = 0 ) {
    return (Abc_Random(seed) & 1) ? MINR_VAL_1 : MINR_VAL_0;
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
                pTarget[k++] = Minr_RandomBinary(seed) ? '1' : '0';
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
            Val = Minr_RandomBinary(seed) ? MINR_VAL_1 : MINR_VAL_0;
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
            Vec_IntWriteEntry( vObjVals, Gia_ObjId(pGia, pObj), Minr_RandomBinary(seed) );

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

void Minr_PropagateAndCut(Minr_Man_t * p) {
    Gia_Man_t * pGia = p->pGia;
    int iObj;
    Gia_Obj_t * pObj;

    // 1. Initialize Values vector
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

    // 4. Cut Selection (Forward Pass)
    // Rule: Obj is Cut if (Val != X) AND (IsCO OR Exists Fanout with Val == X)
    p->vCutNodes = Vec_IntAlloc(100);
    
    // Need Fanouts. Enable static fanout.
    Gia_ManStaticFanoutStart(pGia);

    Gia_ManForEachObj(pGia, pObj, iObj) {
        int Val = Vec_IntEntry(p->vPropVals, iObj);
        
        // Only consider Known nodes for the cut
        if (Val == MINR_VAL_X) continue;
        
        int fIsCut = 0;
        
        // Case A: It's a CO (Combinational Output: PO or RI/Latch Input)
        if (Gia_ObjIsCo(pObj)) {
            fIsCut = 1;
        }
        // Case B: Fanout check
        else {
            int iFanout;
            int k;
            Gia_ObjForEachFanoutStaticId(pGia, iObj, iFanout, k) {
                int ValFan = Vec_IntEntry(p->vPropVals, iFanout);
                if (ValFan == MINR_VAL_X) {
                    fIsCut = 1;
                    break;
                }
            }
        }
        
        if (fIsCut) {
            Vec_IntPush(p->vCutNodes, iObj);
        }
    }
    
    Gia_ManStaticFanoutStop(pGia);

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

Vec_Int_t * Minr_CallSolver(Minr_Man_t * p, char * pFileName, char * pSolverPath) {
    char Command[2000];
    char LogFile[1000];
    sprintf(LogFile, "%s.log", pFileName);

    char * pPath = pSolverPath ? pSolverPath : (char *)"_/EvalMaxSAT";
    if (access(pPath, F_OK) == -1) {
        printf("[Minr] Solver binary not found: %s\n", pPath);
        return NULL;
    }

    sprintf(Command, "%s %s > %s", pPath, pFileName, LogFile);
    if (p->vLevel > 0) printf("Running solver: %s\n", Command);
    
    abctime clk = Abc_Clock();
    int ret = system(Command);
    if (p->vLevel > 0) Abc_PrintTime(1, "Solver runtime", Abc_Clock() - clk);
    if (ret != 7680) printf("[Minr] Solver check: exit code %d\n", ret);

    FILE * pFile = fopen(LogFile, "r");
    if (pFile == NULL) {
        printf("[Minr] Cannot open solver log file: %s\n", LogFile);
        return NULL;
    }

    Vec_Int_t * vModel = NULL;
    char LineBuf[1024];  // Buffer for reading line-by-line (status lines are short)
    int fSat = 0;

    // First pass: find status line and determine format
    int fStandardDimacs = -1;  // -1 = unknown, 0 = bitstring, 1 = standard DIMACS
    while (fgets(LineBuf, sizeof(LineBuf), pFile)) {
        if (strncmp(LineBuf, "s ", 2) == 0) {
            if (strstr(LineBuf, "OPTIMUM FOUND") || strstr(LineBuf, "SATISFIABLE")) fSat = 1;
            else if (strstr(LineBuf, "UNSATISFIABLE")) {
                printf("[Minr] Problem is UNSATISFIABLE.\n");
                fclose(pFile);
                return NULL;
            }
        }
        else if (strncmp(LineBuf, "v ", 2) == 0 && fStandardDimacs == -1) {
            // Detect format from first "v " line
            char * pStr = LineBuf + 2;
            for (int k = 0; pStr[k] && pStr[k] != '\n' && pStr[k] != '\r'; k++) {
                if (pStr[k] == ' ') { fStandardDimacs = 1; break; }
            }
            if (fStandardDimacs == -1) fStandardDimacs = 0;  // bitstring format
        }
    }

    // Second pass: parse model (reopen file or rewind)
    rewind(pFile);
    if (vModel == NULL && fSat) vModel = Vec_IntStart(p->nSatVars + 1);

    if (p->vLevel > 1) printf("[Minr] fStandardDimacs: %d\n", fStandardDimacs);

    while (fgets(LineBuf, sizeof(LineBuf), pFile)) {
        if (strncmp(LineBuf, "v ", 2) == 0) {
            if (vModel == NULL) vModel = Vec_IntStart(p->nSatVars + 1);
            
            if (fStandardDimacs == 1) {
                // Standard DIMACS: parse tokens from this line
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
                // Bitstring format: read character-by-character, may span multiple lines
                int offset = 0;
                int isFirstLine = 1;
                
                // Read bitstring, handling multi-line case when buffer is too small
                while (offset < p->nSatVars) {
                    char * pStr = isFirstLine ? (LineBuf + 2) : LineBuf;  // Skip "v " on first line only
                    int i = 0;
                    
                    // Read characters from current line
                    while (pStr[i] && pStr[i] != '\n' && pStr[i] != '\r') {
                        if (offset + 1 > p->nSatVars) break;
                        Vec_IntWriteEntry(vModel, offset + 1, (pStr[i] == '1') ? 1 : 0);
                        offset++;
                        i++;
                    }
                    
                    // Check if done
                    if (offset >= p->nSatVars) break;
                    if (pStr[i] == '\n' || pStr[i] == '\r') break;  // End of bitstring
                    
                    // Buffer was full (no newline found), read continuation from next line
                    // Next line should be continuation of bitstring (no "v " prefix)
                    if (!fgets(LineBuf, sizeof(LineBuf), pFile)) break;
                    if (strncmp(LineBuf, "v ", 2) == 0) break;  // New model line, stop
                    isFirstLine = 0;  // Subsequent lines don't have "v " prefix
                }
            }
        }
    }
    fclose(pFile);

    if (!fSat) {
        printf("[Minr] Solver failed to find solution. Check log.\n");
        if (vModel) Vec_IntFree(vModel);
        return NULL;
    }
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
            // t == k: Set PIs to X (Scenario 1 Requirement)
            Gia_ManForEachPi(pGia, pObj, iObj)
                Vec_IntWriteEntry(vObjVals, Gia_ObjId(pGia, pObj), MINR_VAL_X);
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
///                     MAIN SOLVER PROCEDURE                        ///
////////////////////////////////////////////////////////////////////////

#if !defined(ABC_NAMESPACE)
extern "C"
#endif
void Minr_Solve(Gia_Man_t * pGia, int nFrames, char * pInitStr, int fRandTarget, int nRandomSim, char * pSolver, char * pOutDir, char * pPrefix, int vLevel, int seed) {
    Minr_Man_t Man;
    Minr_Man_t * p = &Man;
    memset(p, 0, sizeof(Minr_Man_t));
    
    p->pGia = pGia;
    p->nFrames = nFrames;
    p->pInitStr = pInitStr;
    p->fRandTarget = fRandTarget;
    p->nRandomSim = nRandomSim;
    p->pSolver = pSolver;
    p->vLevel = vLevel;
    p->vClauses = Vec_WecAlloc(1000);
    p->timeStart = Abc_Clock();
    p->seed = seed;

    // Optional: derive target reset value by random multi-frame simulation (-r)
    // After deriving, proceed as usual: PI=X propagation -> cut -> CNF -> solve.
    char * pTargetInitStr = NULL;
    if ( p->fRandTarget )
    {
        pTargetInitStr = Minr_DeriveTargetResetByRandomSim( pGia, p->pInitStr, p->nRandomSim, p->vLevel, p->seed );
        if ( pTargetInitStr )
            p->pInitStr = pTargetInitStr;
        else
            printf( "[Rand] Warning: failed to derive target reset value; using given -I\n" );
    }

    // 0. Pre-processing: Propagation & Cut
    Minr_PropagateAndCut(p);

    // 1. Allocate Vars
    p->vVarMap = Vec_IntStart(Gia_ManObjNum(pGia) * (nFrames + 1));
    p->nSatVars = 0;
    int iObj, t;
    Gia_Obj_t * pObj;
    for (t = 0; t <= nFrames; t++) {
        Gia_ManForEachObj(pGia, pObj, iObj) {
            p->nSatVars++; Vec_IntWriteEntry(p->vVarMap, iObj * (nFrames + 1) + t, p->nSatVars);
            p->nSatVars++; 
        }
    }

    if (p->vLevel > 0) printf("Created %d SAT variables for %d frames.\n", p->nSatVars, nFrames);

    // 2. Unrolling & Hard Constraints
    for (t = 0; t <= nFrames; t++) {
        // Const0
        {
            int c0_T = Lit_T(p, 0, t), c0_F = Lit_F(p, 0, t);
            Minr_AddClause1(p, Abc_LitNot(c0_T)); Minr_AddClause1(p, c0_F);
        }
        // Logic
        Gia_ManForEachObj(pGia, pObj, iObj) {
            if (iObj == 0) continue; 
            
            if (Gia_ObjIsCi(pObj)) {
                if (t < nFrames) {
                    // t=0..k-1: PI must be binary (0/1), RO can be 0/1/X
                    if (Gia_ObjIsPi(pGia, pObj)) {
                         Minr_AddBinaryConstraint(p, iObj, t);  // Force (T|F)
                         Minr_AddIllegalStateCheck(p, iObj, t); // Force !(T&F)
                    } else {
                         // RO: only illegal check (can be X at t=0 for objective)
                         Minr_AddIllegalStateCheck(p, iObj, t);
                    }
                } else {
                    // t == k: PI must be unknown (X), RO only illegal check
                    if (Gia_ObjIsPi(pGia, pObj)) {
                        Minr_AddUnknownConstraint(p, iObj, t);  // Force (!T & !F)
                    } else {
                        Minr_AddIllegalStateCheck(p, iObj, t);
                    }
                }
            }
            if (Gia_ObjIsAnd(pObj)) {
                Minr_AddAnd(p, iObj, Gia_ObjFaninId0(pObj, iObj), Gia_ObjFaninId1(pObj, iObj), Gia_ObjFaninC0(pObj), Gia_ObjFaninC1(pObj), t);
                Minr_AddIllegalStateCheck(p, iObj, t);
            }
            if (Gia_ObjIsCo(pObj)) {
                Minr_AddCoBuffer(p, iObj, Gia_ObjFaninId0(pObj, iObj), Gia_ObjFaninC0(pObj), t);
                Minr_AddIllegalStateCheck(p, iObj, t);
            }
        }
        // Latch Transition
        if (t < nFrames) {
            int i;
            Gia_ManForEachRi(pGia, pObj, i) {
                Gia_Obj_t * pObjRoNext = Gia_ManRo(pGia, i);
                Minr_AddEquiv(p, Gia_ObjId(pGia, pObjRoNext), Gia_ObjId(pGia, pObj), t+1, t);
            }
        }
    }

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
    const char * pFinalPrefix = pPrefix ? pPrefix : "minr_out";
    const char * pFinalDir = pOutDir ? pOutDir : ".";
#ifdef WIN32
    mkdir(pFinalDir);
#else
    mkdir(pFinalDir, 0777);
#endif
    sprintf(Buffer, "%s/%s.wcnf", pFinalDir, pFinalPrefix);
    if (p->vLevel > 0) printf("Writing WCNF to %s ...\n", Buffer);
    long long topWeight = Gia_ManRegNum(pGia) + 1;
    FILE * pFile = fopen(Buffer, "w");
    if (!pFile) { printf("Error: Cannot open %s\n", Buffer); goto cleanup; }
    fprintf(pFile, "p wcnf %d %d %lld\n", p->nSatVars, Vec_WecSize(p->vClauses) + Vec_IntSize(vSoftLits), topWeight);
    Vec_Int_t * vC; int k, Lit, i;
    Vec_WecForEachLevel(p->vClauses, vC, k) {
        fprintf(pFile, "%lld ", topWeight);
        Vec_IntForEachEntry(vC, Lit, i) fprintf(pFile, "%s%d ", Abc_LitIsCompl(Lit) ? "-" : "", Abc_Lit2Var(Lit));
        fprintf(pFile, "0\n");
    }
    Vec_IntForEachEntry(vSoftLits, Lit, k) fprintf(pFile, "1 %s%d 0\n", Abc_LitIsCompl(Lit) ? "-" : "", Abc_Lit2Var(Lit));
    fclose(pFile);

    // Call Solver & Decode & Verify
    {
        Vec_Int_t * vModel = Minr_CallSolver(p, Buffer, pSolver);
        if (vModel) {
            Minr_DecodeResult(p, vModel);
            // Run Verification (Scenario 1)
            Minr_VerifyResult(p, vModel);
            Vec_IntFree(vModel);
        }
    }

cleanup:
    Vec_IntFree(p->vVarMap);
    Vec_WecFree(p->vClauses);
    Vec_IntFree(vSoftLits);
    if (p->vPropVals) Vec_IntFree(p->vPropVals);
    if (p->vCutNodes) Vec_IntFree(p->vCutNodes);
    if (p->vPiVals) Vec_IntFree(p->vPiVals);
    if (p->vRoVals0) Vec_IntFree(p->vRoVals0);
    if (pTargetInitStr) free(pTargetInitStr);
}

ABC_NAMESPACE_IMPL_END