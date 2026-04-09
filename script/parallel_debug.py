"""
Debug runner to *find* unsat_with_best cases quickly.

This script is intentionally separate from script/parallel.py (do not edit the original).

Strategy:
- Either provide explicit benches (recommended for debugging), or
  read script/exp/all_0-9_stat.csv to pick candidates with relatively large best_k_avg
  but small runtime_sec_avg (fast-ish).
- For each (bench, dc_pct), run many seeds and look for opt_status=unsat_with_best.

Example:
  python3 script/parallel_debug.py --seeds 0:199 --max-cases 6 --max-workers 6 --prefix debug
"""

import os
import re
import csv
import sys
import subprocess
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime


ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCHMARK_DIR = os.path.join(ROOT_DIR, "benchmarks")
SCRIPT_DIR = os.path.join(ROOT_DIR, "script")
LOG_DIR = os.path.join(SCRIPT_DIR, "log")
EXP_DIR = os.path.join(SCRIPT_DIR, "exp")
ABC_BINARY = os.path.join(ROOT_DIR, "abc")

STAT_CSV = os.path.join(EXP_DIR, "all_0-9_stat.csv")


def bench_stem(path: str) -> str:
    # e.g. "iscas89/s13207.aig" -> "s13207"
    base = os.path.basename(path)
    return base[:-4] if base.endswith(".aig") else base


def parse_seed_range(s: str):
    # "0:199" inclusive; or "0,1,2"
    s = s.strip()
    if ":" in s:
        a, b = s.split(":", 1)
        lo = int(a)
        hi = int(b)
        if hi < lo:
            lo, hi = hi, lo
        return list(range(lo, hi + 1))
    return [int(x) for x in s.split(",") if x.strip()]


def pick_candidates(max_cases: int, min_best_k: float, max_runtime: float):
    # Returns list of tuples: (bench_rel_path, dc_pct, best_k_avg, runtime_avg)
    rows = []
    with open(STAT_CSV, "r", encoding="utf-8") as f:
        r = csv.DictReader(f)
        for row in r:
            try:
                name = row["circuit"]
                dc = int(row["dc_ratio"])
                best_k = float(row["best_k_avg"])
                runtime = float(row["runtime_sec_avg"])
            except Exception:
                continue
            if best_k < min_best_k:
                continue
            if runtime > max_runtime:
                continue

            # Prefer iscas89 first (usually interesting), then itc99.
            # We run "read_aiger benchmarks/<group>/<name>.aig"
            # Infer group by name prefix (s* are iscas89, b* are itc99).
            group = "iscas89" if name.startswith("s") else "itc99"
            bench = f"{group}/{name}.aig"
            rows.append((bench, dc, best_k, runtime))

    # Sort: larger best_k, then smaller runtime.
    rows.sort(key=lambda x: (-x[2], x[3], x[1], x[0]))
    # De-dup by (bench, dc)
    seen = set()
    out = []
    for bench, dc, best_k, runtime in rows:
        key = (bench, dc)
        if key in seen:
            continue
        seen.add(key)
        out.append((bench, dc, best_k, runtime))
        if len(out) >= max_cases:
            break
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--prefix", default="debug", help="Output prefix for detail CSV")
    ap.add_argument("--seeds", default="0:199", help="Seed list: '0:199' or '0,1,2'")
    ap.add_argument("--max-workers", type=int, default=6)
    ap.add_argument("--max-cases", type=int, default=6)
    ap.add_argument("--min-best-k", type=float, default=16.0)
    ap.add_argument("--max-runtime", type=float, default=2.0)
    ap.add_argument("--total-timeout", type=float, default=200.0, help="&minr -t budget (seconds)")
    ap.add_argument("--proc-timeout", type=float, default=140.0, help="Subprocess wall timeout per run (seconds)")
    ap.add_argument("--random-cycles", type=int, default=100, help="&minr -R cycles")
    ap.add_argument("--optimize-mode", type=int, default=1, help="&minr -O mode (default 1)")
    ap.add_argument("--refine-mode", type=int, default=0, help="&minr -x mode (default 0 for speed)")
    ap.add_argument(
        "--benches",
        default="",
        help="Comma-separated bench list relative to benchmarks/, e.g. 'iscas89/s13207.aig,itc99/b12.aig'. "
             "If set, stat-csv candidate picking is skipped.",
    )
    ap.add_argument(
        "--dc-pcts",
        default="0,25,50,75",
        help="Comma-separated don't-care percentages to try with --benches (default: 0,25,50,75).",
    )
    ap.add_argument(
        "--stop-on-hit",
        action="store_true",
        help="Stop immediately when the first unsat_with_best is found.",
    )
    args = ap.parse_args()

    seeds = parse_seed_range(args.seeds)

    os.makedirs(LOG_DIR, exist_ok=True)
    os.makedirs(EXP_DIR, exist_ok=True)

    cands = []
    if args.benches.strip():
        benches = [b.strip() for b in args.benches.split(",") if b.strip()]
        dc_pcts = [int(x.strip()) for x in args.dc_pcts.split(",") if x.strip()]
        for b in benches:
            for dc in dc_pcts:
                cands.append((b, dc, 0.0, 0.0))
    else:
        cands = pick_candidates(
            max_cases=args.max_cases,
            min_best_k=args.min_best_k,
            max_runtime=args.max_runtime,
        )
        if not cands:
            print("[debug] No candidates matched filters.", file=sys.stderr)
            return 2

    print("[debug] Candidates (bench, dc, best_k_avg, runtime_avg):")
    for bench, dc, best_k, runtime in cands:
        print(f"  - {bench} D={dc} best_k_avg={best_k:.1f} runtime_avg={runtime:.3f}s")

    patterns = {
        "opt_status": r"opt_status\s*=\s*(\w+)",
        "best_k": r"best_k\s*=\s*(\d+)",
        "runtime_sec": r"runtime_sec\s*=\s*([\d.]+)",
        "solver_status": r"solver_status\s*=\s*(\w+)",
    }

    now = datetime.now().strftime("%m%d_%H%M%S")
    out_csv = os.path.join(EXP_DIR, f"{args.prefix}_{now}_detail.csv")
    headers = [
        "bench",
        "dc_pct",
        "seed",
        "opt_status",
        "best_k",
        "solver_status",
        "runtime_sec",
        "log_path",
    ]

    def run_one(bench: str, dc_pct: int, seed: int):
        stem = bench_stem(bench)
        src_aig = os.path.join(BENCHMARK_DIR, bench)
        log_path = os.path.join(LOG_DIR, f"{stem}_dbg_O{args.optimize_mode}_r{seed}_R{args.random_cycles}_D{dc_pct}_t{int(args.total_timeout)}")

        dc_arg = f"-D {dc_pct}" if dc_pct > 0 else ""
        refine_arg = f"-x {args.refine_mode}" if args.refine_mode > 0 else ""
        timeout_arg = f"-t {args.total_timeout}" if args.total_timeout > 0 else ""
        opt_arg = f"-O {args.optimize_mode}" if args.optimize_mode else ""

        abc_cmd = (
            f'{ABC_BINARY} -c "read_aiger {src_aig}; &get; &ps; '
            f'&minr {opt_arg} -r {seed} -R {args.random_cycles} {dc_arg} {refine_arg} {timeout_arg} -o {log_path}"'
        )

        try:
            subprocess.run(
                abc_cmd,
                shell=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
                timeout=max(1.0, args.proc_timeout),
            )
        except subprocess.TimeoutExpired:
            return {
                "bench": bench,
                "dc_pct": str(dc_pct),
                "seed": str(seed),
                "opt_status": "NA",
                "best_k": "NA",
                "solver_status": "NA",
                "runtime_sec": "NA",
                "log_path": log_path,
            }
        parsed = {k: "NA" for k in patterns.keys()}
        if os.path.exists(log_path):
            with open(log_path, "r", encoding="utf-8") as f:
                content = f.read()
            for k, pat in patterns.items():
                # patterns are raw regex strings; do not double-escape backslashes
                m = re.search(pat, content)
                if m:
                    parsed[k] = m.group(1)
        return {
            "bench": bench,
            "dc_pct": str(dc_pct),
            "seed": str(seed),
            "opt_status": parsed["opt_status"],
            "best_k": parsed["best_k"],
            "solver_status": parsed["solver_status"],
            "runtime_sec": parsed["runtime_sec"],
            "log_path": log_path,
        }

    hits = 0
    total = 0
    with open(out_csv, "w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=headers)
        w.writeheader()

        with ThreadPoolExecutor(max_workers=max(1, args.max_workers)) as ex:
            futs = []
            for bench, dc, _, _ in cands:
                for seed in seeds:
                    futs.append(ex.submit(run_one, bench, dc, seed))
            total = len(futs)
            done = 0

            for fut in as_completed(futs):
                row = fut.result()
                w.writerow(row)
                done += 1
                if row["opt_status"] == "unsat_with_best":
                    hits += 1
                    print(f"[hit] unsat_with_best bench={row['bench']} D={row['dc_pct']} seed={row['seed']} best_k={row['best_k']} log={row['log_path']}")
                    if args.stop_on_hit:
                        print("[debug] stop-on-hit: stopping early.")
                        return 0
                if done % 25 == 0:
                    print(f"[debug] progress {done}/{total} hits={hits}", flush=True)

    print(f"[debug] Wrote {out_csv}")
    print(f"[debug] unsat_with_best hits: {hits}/{total}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

