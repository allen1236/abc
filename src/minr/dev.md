# &minr — MaxSAT-Based Partial Reset Minimization

## Overview

`&minr` is a custom ABC command that minimizes the number of flip-flops (FFs) requiring explicit reset in a sequential circuit. Given a target state, it finds the smallest subset of FFs that must be reset (to 0 or 1) such that the remaining FFs can reach the target state within k timeframes from any initial value, driven by an appropriate PI sequence.

The core formulation is a **Partial MaxSAT** problem: hard clauses encode circuit semantics and the target state constraint; soft clauses encode the objective of maximizing the number of "don't-care" (unknown) FFs at t=0.

## Source Files

```
src/minr/
├── minr.h          — Minr_Man_t struct definition + function declarations
├── minr_cmd.cpp    — ABC command registration, CLI parsing, parameter validation
├── minr_core.cpp   — All core logic: propagation, cut, CNF, solver I/O, decode, verify, report, optimize
├── minr_sat.cpp    — SAT-based post-refine + SAT/CEC verification
└── module.make     — Build integration (SRC list)
```

Experiment runner: `_/exp.py` (Python script that batch-runs benchmarks and collects CSV results).

## Command Usage

```
&minr [-k <int>] [-I <string>] [-r [<seed>]] [-R <num>] [-D <pct>]
      [-S <path>] [-d <dir>] [-p <prefix>] [-o <file>] [-v <level>]
      [-t <sec>] [-x <mode>] [-X] [-c <nConf>] [-C] [-O <mode>] [-h]
```

| Flag | Description |
|------|-------------|
| `-k <int>` | Timeframe expansion depth. Required in single-k mode. |
| `-I <string>` | Target state for latches (`0`/`1`/`x`). Default: all `0`. Length must equal nRegs. |
| `-r [<seed>]` | Derive target state via random simulation (optional integer seed). |
| `-R <num>` | Number of random simulation frames (default: k when `-r` given). |
| `-D <pct>` | Set `pct`% (1–99) of target registers to don't care (requires `-r`). |
| `-S <path>` | Path to MaxSAT solver. If it ends with `.so`, `&minr` loads it via IPAMIR and solves in-process; otherwise it is executed as an external solver binary. Default: `third_party/EvalMaxSAT2022/libipamirEvalMaxSAT2022.so`. |
| `-d <dir>` | Output directory for WCNF files. |
| `-p <prefix>` | Output filename prefix (default: `minr_out`). |
| `-o <file>` | Dump structured report (log) to file. |
| `-v <level>` | Verbose level: 0=none, 1=summary, 2=debug. |
| `-t <sec>` | Total time budget (seconds). Used as the solver timeout in single-k, and as a global time budget in optimize modes. |
| `-x <mode>` | SAT-based post-refine mode: `0=off`, `1=CEC output equiv`, `2=constant cut`, `3=eq cut`. |
| `-X` | Bind don't-care target ROs to the unrolled circuit at `t=k` (effective with `-x` modes that build a target circuit). |
| `-c <nConf>` | SAT refine conflict limit. `0=unlimited`. If the initial refine solve returns UNKNOWN due to this limit, no resets are released. |
| `-C` | Core-only refine (skip trial release; do only UNSAT-core bulk release). |
| `-O <mode>` | Optimize mode. `1=sweep k` (primary flow), `2=outer-loop heuristic` (fallback for very large circuits). Requires an argument; ignores `-k`. |

### Solve Modes (practical guidance)

- **Primary flow: `-O 1` (sweep-k)**. This is the default research/experiment flow: automatically tries a fixed `k` schedule (including `k=0`) under a total time budget (optional), tracks best-so-far, and stops early on diminishing returns.
- **Heuristic fallback: `-O 2` (outer-loop)**. Intended for circuits that are too large for a straightforward sweep. It partitions the total budget into segments and re-targets iteratively.
- **Single-k**: run once with `-k` and no `-O`. Conceptually, this corresponds to one iteration of the sweep-k flow with a fixed `k`.

## Execution Flow

```
Minr_CommandAbc9Minr()          [minr_cmd.cpp]
│ Parse CLI → validate → build default pInitStr
│
└─► Minr_Solve()                [minr_core.cpp]
    │
    ├─ (1) Random Target Derivation (if -r)
    │   └─ Minr_DeriveTargetResetByRandomSim()
    │      • If no explicit -I: pRoInitStr=NULL → random initial state
    │      • If explicit -I: uses given string as initial state for sim
    │      • Result overwrites pInitStr as the target state
    │   └─ Don't Care Masking (if -D)
    │      • Randomly set floor(nRegs*pct/100) registers to 'x' in pInitStr
    │
    ├─ (2) Propagation & Cut [runs once, shared by all k values]
    │   └─ Minr_PropagateAndCut()
    │      • Sets PI=X, RO=target_state → 3-value Kleene simulation
    │      • Cut extraction via Minr_ExtractCut(): reverse DFS from COs
    │        - Known CO → directly in cut
    │        - X CO → DFS backwards, add known-valued frontier nodes
    │      • Output: vPropVals (per-obj 3-value), vCutNodes (cut frontier)
    │
    ├─ (3) Solve Phase [BRANCHING POINT]
    │   │
    │   ├─ Single-k: Minr_SolveSingleK(p, timeout)
    │   │
    │   └─ Optimize:
    │       ├─ `-O 1`: Minr_SolveOptimize(p)
    │       │          └─ for each k in {0,1,2,4,8,...,256}:
    │       │               • Check total budget
    │       │               • Minr_SolveSingleK(p, tRemain)
    │       │               • Update best-so-far if improved
    │       │               • Early stop: reset=0, diminishing improvement, timeout/UNSAT with best
    │       │          └─ Restore best solution into p
    │       └─ `-O 2`: Minr_SolveOptimize2(p)  (outer-loop heuristic for large circuits)
    │
    ├─ (4) Post-Processing [runs once, shared by single-k and `-O 1`]
    │   │ (only if solverStatus == optimum)
    │   │
    │   ├─ SAT Post-Refine (if -x > 0)
    │   │   └─ Minr_SatRefine()             [minr_sat.cpp]
    │   │      • Releases additional reset FFs using UNSAT core + optional trial release
    │   │
    │   └─ Verification
    │       ├─ Minr_VerifyResult()  — always runs (x-sim + mismatch stats)
    │       ├─ Minr_SatVerify()     — runs when refine released any FFs (stronger cut-based check)
    │       └─ Minr_CecVerify()     — full PO+RI equivalence (GIA miter + fraig/SAT), runs after above
    │
    ├─ (5) Report Dump
    │   └─ Minr_DumpReport()  → writes structured log to -o file
    │
    └─ (6) Cleanup (free all allocated memory)
```

## Minr_SolveSingleK — The Core Solve for One k Value

This is the function (`minr_core.cpp`) that builds and solves the MaxSAT problem for a given k. It is called once in single-k mode, or repeatedly (with different k) in optimize mode.

Steps inside:

1. **Allocate dual-rail SAT variables**: 2 vars per (ObjId, Frame) pair. `vVarMap[ObjId*(k+1)+t]` → base var. `Lit_T` = true-rail, `Lit_F` = false-rail. Encoding: `(T=1,F=0)`→Logic1, `(T=0,F=1)`→Logic0, `(T=0,F=0)`→Unknown(X). `(T=1,F=1)` is illegal.

2. **Unrolling** (t=0..k): For each frame:
   - Const0: force `(!T, F)`
   - PIs at t<k: binary constraint `(T|F)` + illegal check
   - PIs at t=k: unknown `(!T, !F)` by default; may be fixed when `-O 2` sets `vPiAtK`
   - ROs: illegal check only (value determined by solver or latch transition)
   - AND gates: dual-rail AND encoding (6 clauses per gate)
   - CO buffers: dual-rail buffer (4 clauses)
   - Latch transition (t<k): `RO_i(t+1) ≡ RI_i(t)` via `Minr_AddEquiv`

3. **Cut constraints at t=k**: For each cut node with known value, force that value at frame k.

4. **Soft clauses (objective)**: For each RO at t=0, create auxiliary variable U_i: `U_i=1 ↔ (T_i=0 ∧ F_i=0)` i.e. "FF_i is unknown". Each `U_i` is a soft clause with weight 1. Maximizing satisfied soft clauses = minimizing resets.

5. **Write WCNF**: Standard weighted partial MaxSAT format. Hard clauses get weight = nRegs+1.

6. **Solve + decode**:
   - If `-S` ends with `.so`: in-process IPAMIR calls.
   - Otherwise: external solver binary + parse solver output.

## SAT-Based Operations (minr_sat.cpp)

### Minr_SatRefine (-x <mode>)

Goal: after a MaxSAT solution is decoded, try releasing additional reset FFs while preserving correctness, using a binary SAT model and UNSAT-core/trial release.

Modes:
- `-x 1` (CEC output equiv): build a target circuit at `t=k` and enforce equivalence on **all** COs (PO+RI) vs the unrolled circuit.
- `-x 2` (constant cut): enforce equality only on the (known-valued) constraint cut nodes at `t=k`.
- `-x 3` (eq cut): enforce equivalence on an extracted eq-cut (a smaller set than all COs, meant to be stronger than the constant cut).

Notes:
- `-X` enables binding target don't-cares to the unrolled values at `t=k` in modes that build a target circuit.
- `-c` conflict limit can make the initial refine solve UNKNOWN; in that case refine does not release any FFs.

### Minr_SatVerify

Checks whether the decoded solution (PI sequence + reset assignments) guarantees the constant-cut constraints at `t=k`. Used when refine released any FFs (stronger than simulation-only cut check).

### Minr_CecVerify (GIA miter + fraig)

Goal: verify that the initialization sequence is correct by full combinational equivalence checking. Unlike the cut-based `Minr_SatVerify`, this compares **all** combinational outputs (PO + RI), providing a stronger guarantee.

Target don't-cares (`x` in `pInitStr`) are supported by binding them to the unrolled RO values at `t=k` when building the target circuit.

## Solver Interface

- Default solver: `third_party/EvalMaxSAT2022/libipamirEvalMaxSAT2022.so` via IPAMIR (in-process).
- External solver fallback: if `-S` does not end with `.so`, `&minr` runs it as a binary.

