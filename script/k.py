"""
依 k 掃描結果匯出 CSV（搭配 &minr -O 1 -K N）：每個 k 一組 reset / reduction / runtime_sec。

檔名前綴預設 k_（非 exp_）。需已建置支援 -K 的 abc。
"""

import os
import re
import csv
import sys
import subprocess
from datetime import datetime

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

BENCHMARKS = [
    "iscas89/s5378.aig",
    "iscas89/s9234.aig",
    "iscas89/s15850.aig",
]

BENCHMARK_DIR = os.path.join(ROOT_DIR, "benchmarks")
SCRIPT_DIR = os.path.join(ROOT_DIR, "script")
LOG_DIR = os.path.join(SCRIPT_DIR, "log")
EXP_DIR = os.path.join(SCRIPT_DIR, "exp")

ABC_BINARY = os.path.join(ROOT_DIR, "abc")
TIMEOUT_SEC = 1200

K_MAX = 8  # 傳給 &minr 的 -K：掃描 k=0..K_MAX（含）
SEEDS = [5, 6, 7, 8, 9]
RANDOM_SIM_CYCLE = 100
REFINE_MODE = 1
REFINE_BIND_DC = True
REFINE_CONF_LIMIT = 10000
REFINE_CORE_ONLY = False
OTHER_ARGS = ""
OPTIMIZE_MODE = 1
DC_RATIO = [0, 25, 50, 75]
TOTAL_TIMEOUT = 600


def parse_iterations_block(content: str):
    """Parse [iterations] lines into { k: {reset, reduction, runtime_sec} }."""
    out = {}
    sec = re.search(r"\[iterations\]\s*\n(.*?)(?:\n\[|\Z)", content, re.S)
    if not sec:
        return out
    block = sec.group(1)
    # New format: k=1, resets=3, reduction=40.00%, 2ms
    for m in re.finditer(
        r"^k=(\d+),\s*resets=(\d+),\s*reduction=(N/A|[\d.]+%),\s*(\d+)ms\s*$", block, re.M
    ):
        k = int(m.group(1))
        red_raw = m.group(3).strip()
        if red_raw == "N/A":
            red = "NA"
        else:
            red = red_raw.rstrip("%").strip()
        out[k] = {
            "reset": m.group(2),
            "reduction": red,
            "runtime_sec": f"{int(m.group(4)) / 1000.0:.6f}",
        }
    # Legacy: k=1, resets=3, 2ms
    for m in re.finditer(r"^k=(\d+),\s*resets=(\d+),\s*(\d+)ms\s*$", block, re.M):
        k = int(m.group(1))
        if k in out:
            continue
        out[k] = {
            "reset": m.group(2),
            "reduction": "NA",
            "runtime_sec": f"{int(m.group(3)) / 1000.0:.6f}",
        }
    # Failure: k=1, unsat, reduction=N/A, 2ms
    for m in re.finditer(
        r"^k=(\d+),\s*(?:unsat|timeout|error),\s*reduction=N/A,\s*(\d+)ms\s*$", block, re.M
    ):
        k = int(m.group(1))
        out[k] = {"reset": "NA", "reduction": "NA", "runtime_sec": f"{int(m.group(2)) / 1000.0:.6f}"}
    # Legacy fail: k=1, unsat, 2ms
    for m in re.finditer(r"^k=(\d+),\s*(unsat|timeout|error),\s*(\d+)ms\s*$", block, re.M):
        k = int(m.group(1))
        if k in out:
            continue
        out[k] = {"reset": "NA", "reduction": "NA", "runtime_sec": f"{int(m.group(3)) / 1000.0:.6f}"}
    return out


def main():
    os.makedirs(LOG_DIR, exist_ok=True)
    os.makedirs(EXP_DIR, exist_ok=True)

    patterns = {
        "nodes": r"nodes\s*=\s*(\d+)",
        "ff": r"ff\s*=\s*(\d+)",
        "specified_regs": r"specified_regs\s*=\s*(\d+)",
    }

    def build_log_suffix(seed: int, dc_pct: int):
        parts = [f"O{OPTIMIZE_MODE}", f"K{K_MAX}", f"r{seed}", f"R{RANDOM_SIM_CYCLE}"]
        if dc_pct > 0:
            parts.append(f"D{dc_pct}")
        if REFINE_MODE >= 0:
            parts.append(f"x{REFINE_MODE}")
            if REFINE_BIND_DC:
                parts.append("X")
        if REFINE_MODE > 0:
            parts.append(f"c{REFINE_CONF_LIMIT}")
            if REFINE_CORE_ONLY:
                parts.append("C")
        if TOTAL_TIMEOUT > 0:
            parts.append(f"t{TOTAL_TIMEOUT}")
        if OTHER_ARGS.strip():
            sanitized = re.sub(r"[^\w\-=]", "_", OTHER_ARGS.strip())
            parts.append(sanitized)
        return "_" + "_".join(parts)

    def build_csv_path(prefix: str):
        now = datetime.now()
        date_str = now.strftime("%m%d")
        time_str = now.strftime("%H%M%S")
        parts = [
            f"K0-{K_MAX}",
            f"S{min(SEEDS)}-{max(SEEDS)}",
            f"DC{min(DC_RATIO)}-{max(DC_RATIO)}",
            f"R{RANDOM_SIM_CYCLE}",
        ]
        if REFINE_MODE >= 0:
            parts.append(f"x{REFINE_MODE}")
            if REFINE_BIND_DC:
                parts.append("X")
        if REFINE_MODE > 0:
            parts.append(f"c{REFINE_CONF_LIMIT}")
            if REFINE_CORE_ONLY:
                parts.append("C")
        parts.append(f"O{OPTIMIZE_MODE}")
        if TOTAL_TIMEOUT > 0:
            parts.append(f"t{TOTAL_TIMEOUT}")
        if OTHER_ARGS.strip():
            sanitized = re.sub(r"[^\w\-=]", "_", OTHER_ARGS.strip())
            parts.append(sanitized)
        param_str = "_".join(parts)
        return os.path.join(EXP_DIR, f"{prefix}{date_str}_{time_str}_{param_str}.csv")

    prefix_base = sys.argv[1].strip() if len(sys.argv) > 1 and sys.argv[1].strip() else "k"
    prefix_base = re.sub(r"[^\w\-]", "_", prefix_base)

    detail_csv = build_csv_path(f"{prefix_base}_detail_")

    def bench_stem(path_or_name: str) -> str:
        base = os.path.basename(path_or_name)
        stem, _ext = os.path.splitext(base)
        return stem

    base_headers = ["circuit", "nodes", "ff", "dc_ratio", "specified", "seed"]
    k_headers = []
    for kk in range(0, K_MAX + 1):
        k_headers.extend([f"k{kk}_reset", f"k{kk}_reduction", f"k{kk}_runtime_sec"])
    headers = base_headers + k_headers

    with open(detail_csv, "w", newline="", encoding="utf-8") as detail_f:
        detail_writer = csv.DictWriter(detail_f, fieldnames=headers)
        detail_writer.writeheader()
        detail_f.flush()
        os.fsync(detail_f.fileno())

        try:
            for bench in BENCHMARKS:
                stem = bench_stem(bench)
                src_aig = os.path.join(BENCHMARK_DIR, bench)

                for seed in SEEDS:
                    for dc_pct in DC_RATIO:
                        print(f"Running: {stem} r={seed} D={dc_pct} ...", flush=True)

                        log_suffix = build_log_suffix(seed, dc_pct)
                        log_path = os.path.join(LOG_DIR, f"{stem}{log_suffix}")

                        dc_arg = f"-D {dc_pct}" if dc_pct > 0 else ""
                        refine_arg = f"-x {REFINE_MODE}" if REFINE_MODE > 0 else ""
                        bind_arg = " -X" if (REFINE_MODE > 0 and REFINE_BIND_DC) else ""
                        conf_arg = f" -c {REFINE_CONF_LIMIT}" if (REFINE_MODE > 0 and REFINE_CONF_LIMIT is not None) else ""
                        core_only_arg = " -C" if (REFINE_MODE > 0 and REFINE_CORE_ONLY) else ""
                        timeout_arg = f"-t {TOTAL_TIMEOUT}" if TOTAL_TIMEOUT > 0 else ""

                        opt_arg = f"-O {OPTIMIZE_MODE}"
                        k_arg = f"-K {K_MAX}"
                        abc_cmd = (
                            f'{ABC_BINARY} -c "read_aiger {src_aig}; &get; &ps;'
                            f'&minr {opt_arg} {k_arg} -r {seed} -R {RANDOM_SIM_CYCLE} {dc_arg} {refine_arg}{bind_arg}{conf_arg}{core_only_arg} {timeout_arg} {OTHER_ARGS} -o {log_path}"'
                        )

                        parsed = {}
                        is_timeout = False
                        try:
                            subprocess.run(abc_cmd, shell=True, timeout=TIMEOUT_SEC, check=True, capture_output=True)
                        except subprocess.TimeoutExpired:
                            is_timeout = True
                        except subprocess.CalledProcessError:
                            pass

                        if not is_timeout and os.path.exists(log_path):
                            with open(log_path, "r", encoding="utf-8") as f:
                                content = f.read()
                            for key, pattern in patterns.items():
                                match = re.search(pattern, content)
                                if match:
                                    parsed[key] = match.group(1)
                            by_k = parse_iterations_block(content)
                        else:
                            by_k = {}

                        row = {h: "NA" for h in headers}
                        row["circuit"] = stem
                        row["dc_ratio"] = str(dc_pct)
                        row["seed"] = str(seed)
                        row["nodes"] = parsed.get("nodes", "NA")
                        row["ff"] = parsed.get("ff", "NA")
                        row["specified"] = parsed.get("specified_regs", "NA")

                        for kk in range(0, K_MAX + 1):
                            if kk in by_k:
                                row[f"k{kk}_reset"] = by_k[kk]["reset"]
                                row[f"k{kk}_reduction"] = by_k[kk]["reduction"]
                                row[f"k{kk}_runtime_sec"] = by_k[kk]["runtime_sec"]
                            else:
                                row[f"k{kk}_reset"] = "NA"
                                row[f"k{kk}_reduction"] = "NA"
                                row[f"k{kk}_runtime_sec"] = "NA"

                        detail_writer.writerow(row)
                        detail_f.flush()
                        os.fsync(detail_f.fileno())
                        print("  done", flush=True)
        except KeyboardInterrupt:
            print(f"\n[Interrupt] Partial detail saved to {detail_csv}")

    print(f"\nDetail saved to {detail_csv}")


if __name__ == "__main__":
    main()
