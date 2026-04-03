"""
平行版實驗腳本（邏輯對齊 exp.py，不修改 exp.py）。
各 job 回傳一列；主程式依完成順序寫入 detail CSV。

用法:
  python script/parallel.py [prefix] [max_workers]

環境變數:
  MINR_EXP_WORKERS  預設並行數（預設 8）；若命令列有給第二個數字則覆寫。
"""

import os
import re
import csv
import sys
import subprocess
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime

# ==========================================
# 實驗參數設定區（與 exp.py 對齊；benchmark 僅留較小電路方便試跑）
# ==========================================

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

BENCHMARKS = [
    "itc99/b01.aig",
    "itc99/b02.aig",
    "itc99/b03.aig",
    "itc99/b04.aig",
    "itc99/b05.aig",
    "itc99/b06.aig",
    "itc99/b07.aig",
    "itc99/b08.aig",
    "itc99/b09.aig",
    "itc99/b10.aig",
    "itc99/b11.aig",
    "itc99/b12.aig",
    "itc99/b13.aig",
    "itc99/b14.aig",
    "iscas89/s1196.aig",
    "iscas89/s1238.aig",
    "iscas89/s13207.aig",
    "iscas89/s1423.aig",
    "iscas89/s1488.aig",
    "iscas89/s15850.aig",
    "iscas89/s27.aig",
    "iscas89/s298.aig",
    "iscas89/s344.aig",
    "iscas89/s349.aig",
    "iscas89/s35932.aig",
    "iscas89/s382.aig",
    "iscas89/s38417.aig",
    "iscas89/s400.aig",
    "iscas89/s420.aig",
    "iscas89/s444.aig",
    "iscas89/s510.aig",
    "iscas89/s526.aig",
    "iscas89/s5378.aig",
    "iscas89/s641.aig",
    "iscas89/s713.aig",
    "iscas89/s820.aig",
    "iscas89/s832.aig",
    "iscas89/s838.aig",
    "iscas89/s9234.aig",
    "iscas89/s953.aig",
    "iscas89/s38584.aig",
    "itc99/b15.aig",
    "itc99/b17.aig",
    "itc99/b18.aig",
    "itc99/b19.aig",
    "itc99/b20.aig",
    "itc99/b21.aig",
    "itc99/b22.aig",
]

BENCHMARK_DIR = os.path.join(ROOT_DIR, "benchmarks")
SCRIPT_DIR = os.path.join(ROOT_DIR, "script")
LOG_DIR = os.path.join(SCRIPT_DIR, "log")
EXP_DIR = os.path.join(SCRIPT_DIR, "exp")

ABC_BINARY = os.path.join(ROOT_DIR, "abc")
TIMEOUT_SEC = 2000

K = 1
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

# 預設 8；可用 MINR_EXP_WORKERS 或命令列第二參數覆寫
def _default_max_workers() -> int:
    w = os.environ.get("MINR_EXP_WORKERS", "").strip()
    if w.isdigit():
        return max(1, int(w))
    return 8


MAX_WORKERS = _default_max_workers()


def main():
    os.makedirs(LOG_DIR, exist_ok=True)
    os.makedirs(EXP_DIR, exist_ok=True)

    patterns = {
        "inputs": r"inputs\s*=\s*(\d+)",
        "outputs": r"outputs\s*=\s*(\d+)",
        "ff": r"ff\s*=\s*(\d+)",
        "nodes": r"nodes\s*=\s*(\d+)",
        "specified_regs": r"specified_regs\s*=\s*(\d+)",
        "required_reset": r"required_reset\s*=\s*(\d+)",
        "reset_ratio": r"reset_ratio\s*=\s*([\d.]+%?)",
        "reduction": r"reduction\s*=\s*([\d.]+%?|N/A)",
        "k0_reset_ratio": r"k0_reset_ratio\s*=\s*([\d.]+%?|N/A)",
        "k0_reduction": r"k0_reduction\s*=\s*([\d.]+%?|N/A)",
        "reset_ratio_before_refine": r"reset_ratio_before_refine\s*=\s*([\d.]+%?|N/A)",
        "reduction_before_refine": r"reduction_before_refine\s*=\s*([\d.]+%?|N/A)",
        "runtime_sec": r"runtime_sec\s*=\s*([\d.]+)",
        "cut_verified": r"cut_verified\s*=\s*(\w+)",
        "cec_verified": r"cec_verified\s*=\s*(\w+)",
        "spec_ro_in_cut": r"spec_ro_in_cut\s*=\s*([\d.]+%?)",
        "sim_reg_mismatch_weak": r"sim_reg_mismatch_weak\s*=\s*([\d.]+%?)",
        "sim_reg_mismatch_strong": r"sim_reg_mismatch_strong\s*=\s*([\d.]+%?)",
        "dontcare_pct": r"dontcare_pct\s*=\s*(\d+)",
        "refine_mode": r"\[refine\][\s\S]*?mode\s*=\s*(\d+)",
        "refine_by_trial": r"by_trial\s*=\s*(\d+)",
        "refine_by_core": r"by_core\s*=\s*(\d+)",
        "refine_sec": r"refine_sec\s*=\s*([\d.]+)",
    }
    if OPTIMIZE_MODE:
        patterns["best_k"] = r"best_k\s*=\s*(\d+)"
        patterns["opt_status"] = r"opt_status\s*=\s*(\w+)"

    def build_log_suffix(seed: int, dc_pct: int):
        parts = []
        if OPTIMIZE_MODE:
            parts.append(f"O{OPTIMIZE_MODE}")
        else:
            parts.append(f"k{K}")
        parts.append(f"r{seed}")
        parts.append(f"R{RANDOM_SIM_CYCLE}")
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
        parts = []
        parts.append(f"S{min(SEEDS)}-{max(SEEDS)}")
        parts.append(f"DC{min(DC_RATIO)}-{max(DC_RATIO)}")
        parts.append(f"R{RANDOM_SIM_CYCLE}")
        if REFINE_MODE >= 0:
            parts.append(f"x{REFINE_MODE}")
            if REFINE_BIND_DC:
                parts.append("X")
        if REFINE_MODE > 0:
            parts.append(f"c{REFINE_CONF_LIMIT}")
            if REFINE_CORE_ONLY:
                parts.append("C")
        if OPTIMIZE_MODE:
            parts.append(f"O{OPTIMIZE_MODE}")
        else:
            parts.append(f"k{K}")
        if TOTAL_TIMEOUT > 0:
            parts.append(f"t{TOTAL_TIMEOUT}")
        if OTHER_ARGS.strip():
            sanitized = re.sub(r"[^\w\-=]", "_", OTHER_ARGS.strip())
            parts.append(sanitized)
        param_str = "_".join(parts)
        return os.path.join(EXP_DIR, f"{prefix}{date_str}_{time_str}_{param_str}.csv")

    prefix_base = sys.argv[1].strip() if len(sys.argv) > 1 and sys.argv[1].strip() else "exp"
    prefix_base = re.sub(r"[^\w\-]", "_", prefix_base)

    max_workers = MAX_WORKERS
    if len(sys.argv) > 2 and sys.argv[2].strip().isdigit():
        max_workers = max(1, int(sys.argv[2].strip()))

    detail_csv = build_csv_path(f"{prefix_base}_parallel_detail_")

    def bench_stem(path_or_name: str) -> str:
        base = os.path.basename(path_or_name)
        stem, _ext = os.path.splitext(base)
        return stem

    headers = [
        "circuit",
        "nodes",
        "ff",
        "dc_ratio",
        "specified",
        "seed",
        "k0_reset_ratio",
        "refine_mode",
        "best_k",
        "required_reset",
        "reset_ratio",
        "runtime_sec",
        "opt_status",
        "cut_verified",
        "cec_verified",
        "reset_ratio_before_refine",
        "refine_by_trial",
        "refine_by_core",
        "refine_sec",
        "spec_ro_in_cut",
        "sim_reg_mismatch_weak",
        "sim_reg_mismatch_strong",
    ]

    def run_one_job(bench: str, seed: int, dc_pct: int) -> dict:
        stem = bench_stem(bench)
        src_aig = os.path.join(BENCHMARK_DIR, bench)

        log_suffix = build_log_suffix(seed, dc_pct)
        log_path = os.path.join(LOG_DIR, f"{stem}{log_suffix}")

        dc_arg = f"-D {dc_pct}" if dc_pct > 0 else ""
        refine_arg = f"-x {REFINE_MODE}" if REFINE_MODE > 0 else ""
        bind_arg = " -X" if (REFINE_MODE > 0 and REFINE_BIND_DC) else ""
        conf_arg = f" -c {REFINE_CONF_LIMIT}" if (REFINE_MODE > 0 and REFINE_CONF_LIMIT is not None) else ""
        core_only_arg = " -C" if (REFINE_MODE > 0 and REFINE_CORE_ONLY) else ""
        timeout_arg = f"-t {TOTAL_TIMEOUT}" if TOTAL_TIMEOUT > 0 else ""

        if OPTIMIZE_MODE:
            opt_arg = f"-O {OPTIMIZE_MODE}"
            abc_cmd = (
                f'{ABC_BINARY} -c "read_aiger {src_aig}; &get; &ps;'
                f'&minr {opt_arg} -r {seed} -R {RANDOM_SIM_CYCLE} {dc_arg} {refine_arg}{bind_arg}{conf_arg}{core_only_arg} {timeout_arg} {OTHER_ARGS} -o {log_path}"'
            )
        else:
            abc_cmd = (
                f'{ABC_BINARY} -c "read_aiger {src_aig}; &get; &ps;'
                f'&minr -k {K} -r {seed} -R {RANDOM_SIM_CYCLE} {dc_arg} {refine_arg}{bind_arg}{conf_arg}{core_only_arg} {timeout_arg} {OTHER_ARGS} -o {log_path}"'
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

        out = {h: "NA" for h in headers}
        out["circuit"] = stem
        out["dc_ratio"] = str(dc_pct)
        out["seed"] = str(seed)
        out["nodes"] = parsed.get("nodes", "NA")
        out["ff"] = parsed.get("ff", "NA")
        out["specified"] = parsed.get("specified_regs", "NA")
        out["k0_reset_ratio"] = parsed.get("k0_reset_ratio", "NA") if OPTIMIZE_MODE else "NA"
        out["refine_mode"] = parsed.get("refine_mode", "NA")
        out["best_k"] = parsed.get("best_k", "NA") if OPTIMIZE_MODE else "NA"
        out["required_reset"] = parsed.get("required_reset", "NA")
        out["reset_ratio"] = parsed.get("reset_ratio", "NA")
        out["runtime_sec"] = parsed.get("runtime_sec", "NA")
        out["opt_status"] = parsed.get("opt_status", "NA") if OPTIMIZE_MODE else "NA"
        out["cut_verified"] = parsed.get("cut_verified", "NA")
        out["cec_verified"] = parsed.get("cec_verified", "NA")
        out["reset_ratio_before_refine"] = parsed.get("reset_ratio_before_refine", "NA")
        out["refine_by_trial"] = parsed.get("refine_by_trial", "NA")
        out["refine_by_core"] = parsed.get("refine_by_core", "NA")
        out["refine_sec"] = parsed.get("refine_sec", "NA")
        out["spec_ro_in_cut"] = parsed.get("spec_ro_in_cut", "NA")
        out["sim_reg_mismatch_weak"] = parsed.get("sim_reg_mismatch_weak", "NA")
        out["sim_reg_mismatch_strong"] = parsed.get("sim_reg_mismatch_strong", "NA")

        status = "timeout" if is_timeout else ("ok" if parsed else "fail")
        print(f"[{status}] {stem} r={seed} D={dc_pct}", flush=True)
        return out

    jobs = [(b, s, d) for b in BENCHMARKS for s in SEEDS for d in DC_RATIO]
    n_jobs = len(jobs)
    print(f"Parallel detail CSV: {detail_csv}")
    print(f"Jobs: {n_jobs}, max_workers: {max_workers}")

    write_lock = threading.Lock()

    with open(detail_csv, "w", newline="", encoding="utf-8") as detail_f:
        detail_writer = csv.DictWriter(detail_f, fieldnames=headers)
        detail_writer.writeheader()
        detail_f.flush()
        os.fsync(detail_f.fileno())

        try:
            with ThreadPoolExecutor(max_workers=max_workers) as executor:
                future_map = {
                    executor.submit(run_one_job, b, s, d): (b, s, d) for b, s, d in jobs
                }
                for fut in as_completed(future_map):
                    row = fut.result()
                    with write_lock:
                        detail_writer.writerow(row)
                        detail_f.flush()
                        os.fsync(detail_f.fileno())
        except KeyboardInterrupt:
            print(f"\n[Interrupt] Partial detail saved to {detail_csv}")
            raise

    print(f"\nAll tasks finished. Detail saved to {detail_csv}")
    print("All tasks finished. (No stat generated; use script/stat.py)")


if __name__ == "__main__":
    main()
