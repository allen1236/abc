#include "base/abc/abc.h"
#include "base/main/main.h"
#include "base/main/mainInt.h"
#include "aig/gia/gia.h"
#include "misc/extra/extra.h"
#include "minr.h" // 假設你會把核心邏輯宣告在這裡

static int Minr_CommandAbc9Minr(Abc_Frame_t* pAbc, int argc, char** argv);

void init(Abc_Frame_t* pAbc) {
    Cmd_CommandAdd(pAbc, "MINR", "&minr", Minr_CommandAbc9Minr , 0);
}

void destroy(Abc_Frame_t *pAbc) {}

Abc_FrameInitializer_t frame_initializer = {init, destroy};

struct PackageRegistrationManager {
    PackageRegistrationManager() { Abc_FrameAddInitializer(&frame_initializer); }
} minrPackageRegistrationManager;

int Minr_CommandAbc9Minr(Abc_Frame_t* pAbc, int argc, char** argv) {

    Gia_Man_t* pGia;
    int nRegs;

    int c;
    int nFrames = -1;           // -k
    char* pInitStr = NULL;      // -I
    char* pInitStrAlloc = NULL; // -I default (all 0), caller frees
    int fRandTarget = 0;        // -r
    int nRandomSim = -1;        // -R <num> (default=k if -r given, -1 means not set)
    char* pSolver = NULL;       // -S
    char* pOutDir = NULL;       // -d
    char* pPrefix = NULL;       // -p
    int vLevel = 0;             // -v <level>
    int seed = 0;               // -r (seed)
    int nRefineMode = 0;        // -x <mode>: 0=off, 1=CEC, 2=cut, 3=eq cut
    int fRefineBindDc = 0;      // -X: bind don't-care ROs (only with -x)
    int nRefineConfLimit = 10000; // -c <int>: SAT refine conflict limit (default 10000)
    int fRefineCoreOnly = 0;      // -C: core-only refine (skip trial release)
    char* pReportFile = NULL;   // -o <filename>
    int nOptimizeMode = 0;      // -O [<mode>]: 0=off, 1=sweep k, 2=outer-loop heuristic
    double totalTimeout = 0.0;  // -t <sec> (total time budget)
    int fExplicitInit = 0;      // track if user gave -I explicitly
    int nDontCarePercent = 0;   // -D <1..99> don't care percentage (requires -r)
    int nOptimizeDenseKMax = -1; // -K N: with -O 1, dense sweep up to N; default -1 = geometric
    int nOptimizeDenseKMin = 0;  // with -K: start k from -k (set below), else 0

    Extra_UtilGetoptReset();
    while ((c = Extra_UtilGetopt(argc, argv, "k:I:S:d:D:p:v:r:R:o:t:x:O:c:CXK:h")) != EOF) {
        switch (c) {
            case 'k':
                if (globalUtilOptarg == NULL || globalUtilOptarg[0] == '\0') {
                    Abc_Print(-1, "Command line switch \"-k\" should be followed by an integer.\n");
                    goto usage;
                }
                nFrames = atoi(globalUtilOptarg);
                if (nFrames < 0) goto usage;
                break;
            case 'I':
                if (globalUtilOptarg != NULL)
                    pInitStr = (char *)globalUtilOptarg;
                break;
            case 'S':
                if (globalUtilOptarg == NULL) {
                    Abc_Print(-1, "Command line switch \"-S\" should be followed by a string.\n");
                    goto usage;
                }
                pSolver = (char *)globalUtilOptarg;
                break;
            case 'd':
                if (globalUtilOptarg == NULL) {
                    Abc_Print(-1, "Command line switch \"-d\" should be followed by a string.\n");
                    goto usage;
                }
                pOutDir = (char *)globalUtilOptarg;
                break;
            case 'D':
                if (globalUtilOptarg == NULL || globalUtilOptarg[0] == '\0') {
                    Abc_Print(-1, "Command line switch \"-D\" should be followed by an integer (1-99).\n");
                    goto usage;
                }
                nDontCarePercent = atoi(globalUtilOptarg);
                if (nDontCarePercent < 1 || nDontCarePercent > 99) {
                    Abc_Print(-1, "Error: -D percentage must be between 1 and 99.\n");
                    goto usage;
                }
                break;
            case 'p':
                if (globalUtilOptarg == NULL) {
                    Abc_Print(-1, "Command line switch \"-p\" should be followed by a string.\n");
                    goto usage;
                }
                pPrefix = (char *)globalUtilOptarg;
                break;
            case 'v':
                if (globalUtilOptarg == NULL || globalUtilOptarg[0] == '\0') {
                    vLevel = 1;
                } else {
                    vLevel = atoi(globalUtilOptarg);
                    if (vLevel < 0) vLevel = 0;
                    if (vLevel > 2) vLevel = 2;
                }
                break;
            case 'r':
                fRandTarget = 1;
                if (globalUtilOptarg != NULL && globalUtilOptarg[0] != '\0') {
                    seed = atoi(globalUtilOptarg);
                    if (seed < 0) goto usage;
                }
                break;
            case 'R':
                if (globalUtilOptarg == NULL || globalUtilOptarg[0] == '\0') {
                    Abc_Print(-1, "Command line switch \"-R\" should be followed by an integer.\n");
                    goto usage;
                }
                nRandomSim = atoi(globalUtilOptarg);
                if (nRandomSim < 0) goto usage;
                break;
            case 'o':
                if (globalUtilOptarg == NULL) {
                    Abc_Print(-1, "Command line switch \"-o\" should be followed by a filename.\n");
                    goto usage;
                }
                pReportFile = (char *)globalUtilOptarg;
                break;
            case 't':
                if (globalUtilOptarg == NULL || globalUtilOptarg[0] == '\0') {
                    Abc_Print(-1, "Command line switch \"-t\" should be followed by a number (seconds).\n");
                    goto usage;
                }
                totalTimeout = atof(globalUtilOptarg);
                if (totalTimeout < 0) goto usage;
                break;
            case 'O':
                if (globalUtilOptarg == NULL || globalUtilOptarg[0] == '\0') {
                    Abc_Print(-1, "Error: -O requires an argument (1 or 2).\n");
                    goto usage;
                }
                nOptimizeMode = atoi(globalUtilOptarg);
                if (nOptimizeMode < 1 || nOptimizeMode > 2) {
                    Abc_Print(-1, "Error: -O mode must be 1 (sweep k) or 2 (outer-loop).\n");
                    goto usage;
                }
                break;
            case 'x':
                if (globalUtilOptarg == NULL || globalUtilOptarg[0] == '\0') {
                    Abc_Print(-1, "Command line switch \"-x\" should be followed by a mode (0=off, 1=CEC, 2=cut, 3=eq cut).\n");
                    goto usage;
                }
                nRefineMode = atoi(globalUtilOptarg);
                if (nRefineMode < 0 || nRefineMode > 3) {
                    Abc_Print(-1, "Error: -x mode must be 0 (off), 1 (CEC), 2 (cut), or 3 (eq cut).\n");
                    goto usage;
                }
                break;
            case 'X':
                fRefineBindDc = 1;
                break;
            case 'c':
                if (globalUtilOptarg == NULL || globalUtilOptarg[0] == '\0') {
                    Abc_Print(-1, "Command line switch \"-c\" should be followed by an integer.\n");
                    goto usage;
                }
                nRefineConfLimit = atoi(globalUtilOptarg);
                if (nRefineConfLimit < 0) goto usage;
                break;
            case 'C':
                fRefineCoreOnly = 1;
                break;
            case 'K':
                if (globalUtilOptarg == NULL || globalUtilOptarg[0] == '\0') {
                    Abc_Print(-1, "Command line switch \"-K\" should be followed by a non-negative integer (max k for dense sweep with -O 1).\n");
                    goto usage;
                }
                nOptimizeDenseKMax = atoi(globalUtilOptarg);
                if (nOptimizeDenseKMax < 0) {
                    Abc_Print(-1, "Error: -K must be non-negative.\n");
                    goto usage;
                }
                break;
            case 'h':
                goto usage;
            default:
                goto usage;
        }
    }

    if (pAbc->pGia == NULL) {
        Abc_Print(-1, "Minr_CommandAbc9Minr(): There is no AIG.\n");
        return 0;
    }

    pGia = pAbc->pGia;

    // 必要參數檢查
    if (nOptimizeMode == 0 && nFrames == -1) {
        Abc_Print(-1, "Error: Number of frames (-k) must be specified (or use -O 1 or -O 2 for optimize).\n");
        return 0;
    }
    if (nOptimizeMode == 0 && nFrames < 0) {
        Abc_Print(-1, "Error: -k must be non-negative.\n");
        return 0;
    }
    if (nOptimizeMode != 0 && nOptimizeDenseKMax >= 0) {
        /* Dense sweep (-K): start k from -k if given, else 0; end at -K */
        nOptimizeDenseKMin = (nFrames >= 0) ? nFrames : 0;
        if (nOptimizeDenseKMin > nOptimizeDenseKMax) {
            Abc_Print(-1, "Error: -k <start> must be <= -K <end>.\n");
            return 0;
        }
        nFrames = nOptimizeDenseKMin; /* for -R default when unset, and verbose */
    } else if (nOptimizeMode != 0) {
        if (nFrames != -1)
            Abc_Print(1, "Warning: -k ignored in optimize mode (-O). k will be swept automatically.\n");
        nFrames = 1; // placeholder for non-dense optimize (overridden inside)
    }

    nRegs = Gia_ManRegNum(pGia);
    fExplicitInit = (pInitStr != NULL);
    if (pInitStr == NULL) {
        // Default: all 0
        pInitStrAlloc = (char *)malloc((size_t)(nRegs + 1));
        if (!pInitStrAlloc) {
            Abc_Print(-1, "Error: Cannot allocate init string.\n");
            return 0;
        }
        for (int i = 0; i < nRegs; i++) pInitStrAlloc[i] = '0';
        pInitStrAlloc[nRegs] = '\0';
        pInitStr = pInitStrAlloc;
    } else if ((int)strlen(pInitStr) != nRegs) {
        Abc_Print(-1, "Error: Length of init string (%d) does not match number of latches (%d).\n",
                  (int)strlen(pInitStr), nRegs);
        return 0;
    }

    // 輸出基本資訊 (M1 Deliverable), level >= 1
    if (vLevel > 0) {
        Abc_Print(1, "Minr Configuration:\n");
        Abc_Print(1, "  Target Frames (k) : %d\n", nFrames);
        Abc_Print(1, "  Polarity Constraint: %s\n", pInitStr);
        Abc_Print(1, "  Network Stats     : nPI=%d, nPO=%d, nReg=%d\n", 
                  Gia_ManPiNum(pGia), Gia_ManPoNum(pGia), nRegs);
        
        Abc_Print(1, "  Latch Mapping:\n");
        int i;
        Gia_Obj_t * pObj;
        // 遍歷所有 Latch (GIA 中 Latch 是 RO/RI pair，Gia_ManForEachRi 遍歷 Latch inputs)
        // 但通常我們用 Gia_ManForEachCi 裡的 latch 部分，或者直接遍歷 Sequential objects
        // 這裡示範用 Gia_ManForEachRo (Latch Outputs) 對應 -I 的 index
        Gia_ManForEachRo(pGia, pObj, i) {
            // pObj 是 Latch Output (Ri in ABC terminology? Wait. In GIA: RO=Latch Output=PPI)
            // GIA: CI = PI + RO (Latch Output)
            // GIA: CO = PO + RI (Latch Input)
            // 修正：Gia_ManForEachRo 遍歷的是 Latch Outputs (State outputs at t)
            
            // 找出對應的 Latch Input (RI / Next State)
            // 通常 GIA 不直接存這連結，但在 construction 時通常是一對一
            // 簡單列印 Ro ID 即可
            Abc_Print(1, "    L[%d]: ObjId=%d, Polarity=%c\n", i, Gia_ObjId(pGia, pObj), pInitStr[i]);
        }
    }

    // Set default nRandomSim: if -r given but -R not given, use nFrames (k)
    if (fRandTarget && nRandomSim == -1) {
        nRandomSim = nFrames;
    }

    if (nDontCarePercent > 0 && !fRandTarget) {
        Abc_Print(-1, "Error: -D requires -r (random target derivation).\n");
        return 0;
    }
    if (nOptimizeDenseKMax >= 0 && nOptimizeMode != 1) {
        Abc_Print(-1, "Error: -K (dense k sweep 0..N) requires -O 1.\n");
        return 0;
    }

    // Default solver: use in-process EvalMaxSAT2022 IPAMIR shared library.
    if (pSolver == NULL)
        pSolver = (char *)"third_party/EvalMaxSAT2022/libipamirEvalMaxSAT2022.so";

    // Call Minr_Solve function
    Minr_Solve(pGia, nFrames, pInitStr, fExplicitInit, fRandTarget, nRandomSim, pSolver, pOutDir, pPrefix, vLevel, seed, nRefineMode, fRefineBindDc, nRefineConfLimit, fRefineCoreOnly, pReportFile, nOptimizeMode, totalTimeout, nDontCarePercent, nOptimizeDenseKMax, nOptimizeDenseKMin);

    if (pInitStrAlloc) free(pInitStrAlloc);
    return 0;

usage:
    Abc_Print(-2, "usage: &minr [-k <int>] [-I <string>] [-r [<seed>]] [-R <num>] [-D <pct>] [-S <path>] [-d <dir>] [-p <prefix>] [-o <file>] [-v <level>] [-t <sec>] [-x <mode>] [-c <nConf>] [-C] [-O <mode>] [-K <N>]\n");
    Abc_Print(-2, "\t-k <int>    : timeframe expansion depth (t=0..k), k=0 means single frame\n");
    Abc_Print(-2, "\t-I <string> : initial value for latches (0,1,x); default all 0; length = nRegs\n");
    Abc_Print(-2, "\t-r [<seed>] : derive target reset by random simulation (optional seed)\n");
    Abc_Print(-2, "\t-R <num>    : number of random simulation frames (default=k if -r given, 0=no sim)\n");
    Abc_Print(-2, "\t-D <pct>    : set <pct>%% (1-99) of target registers to don't care (requires -r)\n");
    Abc_Print(-2, "\t-S <path>   : path to MaxSAT solver binary (default third_party/EvalMaxSAT2022/libipamirEvalMaxSAT2022.so)\n");
    Abc_Print(-2, "\t             or path to IPAMIR shared library (.so) for in-process solving\n");
    Abc_Print(-2, "\t-d <dir>    : output directory\n");
    Abc_Print(-2, "\t-p <prefix> : output filename prefix\n");
    Abc_Print(-2, "\t-o <file>   : dump structured report to file\n");
    Abc_Print(-2, "\t-v <level>  : verbose level (0=none, 1=summary, 2=debug)\n");
    Abc_Print(-2, "\t-t <sec>    : total time budget in seconds (used with -O or single -k mode)\n");
    Abc_Print(-2, "\t-x <mode>   : SAT-based post-refine (0=off, 1=CEC, 2=cut, 3=eq cut)\n");
    Abc_Print(-2, "\t-X          : bind don't-care target ROs to unrolled t=k (only with -x)\n");
    Abc_Print(-2, "\t-c <nConf>  : SAT refine conflict limit (default 10000, 0=unlimited)\n");
    Abc_Print(-2, "\t-C          : core-only refine (skip trial release; do one UNSAT-core release)\n");
    Abc_Print(-2, "\t-O <mode>   : optimize mode (1=sweep k, 2=outer-loop); argument required\n");
    Abc_Print(-2, "\t-K <N>      : with -O 1 only: dense sweep from -k (default 0) through N; omit -k to start at 0\n");
    Abc_Print(-2, "\t-h          : print the command usage\n");
    return 1;
}