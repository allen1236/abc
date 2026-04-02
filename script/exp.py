import os
import subprocess
import csv
import re
from datetime import datetime
import sys

# ==========================================
# 實驗參數設定區
# ==========================================

# 專案根目錄（script/exp.py → repo root）
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

BENCHMARKS = [
    # "itc99/b01.aig",
    # "itc99/b02.aig",
    # "itc99/b03.aig",
    # "itc99/b04.aig",
    # "itc99/b05.aig",
    # "itc99/b06.aig",
    # "itc99/b07.aig",
    # "itc99/b08.aig",
    # "itc99/b09.aig",
    # "itc99/b10.aig",
    # "itc99/b11.aig",
    # "itc99/b12.aig",
    # "itc99/b13.aig",
    # "itc99/b14.aig",
    "itc99/b15.aig",
    "itc99/b17.aig",
    "itc99/b18.aig",
    "itc99/b19.aig",
    "itc99/b20.aig",
    "itc99/b21.aig",
    "itc99/b22.aig",
#     "iscas89/s1196.aig",
#     "iscas89/s1238.aig",
#     "iscas89/s13207.aig",
#     "iscas89/s1423.aig",
#     "iscas89/s1488.aig",
#     "iscas89/s15850.aig",
#     "iscas89/s27.aig",
#     "iscas89/s298.aig",
#     "iscas89/s344.aig",
#     "iscas89/s349.aig",
#     "iscas89/s35932.aig",
#     "iscas89/s382.aig",
#     "iscas89/s38417.aig",
    "iscas89/s38584.aig",
#     "iscas89/s400.aig",
#     "iscas89/s420.aig",
#     "iscas89/s444.aig",
#     "iscas89/s510.aig",
#     "iscas89/s526.aig",
#     "iscas89/s5378.aig",
#     "iscas89/s641.aig",
#     "iscas89/s713.aig",
#     "iscas89/s820.aig",
#     "iscas89/s832.aig",
#     "iscas89/s838.aig",
#     "iscas89/s9234.aig",
#     "iscas89/s953.aig",
]

BENCHMARK_DIR = os.path.join(ROOT_DIR, "benchmarks")
SCRIPT_DIR = os.path.join(ROOT_DIR, "script")
LOG_DIR = os.path.join(SCRIPT_DIR, "log")
EXP_DIR = os.path.join(SCRIPT_DIR, "exp")

ABC_BINARY = os.path.join(ROOT_DIR, "abc")
TIMEOUT_SEC = 1200  # Python subprocess 層的 Timeout（秒）

# parameters
K = 1
SEEDS = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
RANDOM_SIM_CYCLE = 500
REFINE_MODE = 1  # -x <mode>: 0=off, 1=CEC, 2=cut, 3=eq cut
REFINE_BIND_DC = True  # -X: bind don't-care target ROs (only with -x)
REFINE_CONF_LIMIT = 10000  # -c <nConf>: SAT refine conflict limit (0=unlimited)
REFINE_CORE_ONLY = False  # -C: core-only refine (skip trial release)
OTHER_ARGS = ""  # 預留給其他字串參數
OPTIMIZE_MODE = 1  # 0=fix -k, 1=-O 1 (sweep k), 2=-O 2 (outer-loop)
DC_RATIO = [50]  # -D: 設定 target state 中 don't care 比例 (1-99), 0=不使用
TOTAL_TIMEOUT = 600  # -t: 傳給 &minr 的總時間預算 (秒), 0=不限


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

    prefix_base = (sys.argv[1].strip() if len(sys.argv) > 1 and sys.argv[1].strip() else "exp")
    prefix_base = re.sub(r"[^\w\-]", "_", prefix_base)

    detail_csv = build_csv_path(f"{prefix_base}_detail_")
    stat_csv = build_csv_path(f"{prefix_base}_stat_")

    def bench_stem(path_or_name: str) -> str:
        base = os.path.basename(path_or_name)
        stem, _ext = os.path.splitext(base)
        return stem

    # detail csv column order (as requested)
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

    def _to_float_percent(s: str):
        if not s:
            return None
        s = s.strip()
        if s == "N/A" or s == "NA":
            return None
        if s.endswith("%"):
            s = s[:-1].strip()
        try:
            return float(s)
        except ValueError:
            return None

    def _to_int(s: str):
        try:
            return int(s)
        except Exception:
            return None

    def _to_float(s: str):
        try:
            return float(s)
        except Exception:
            return None

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
                        print(f"Running benchmark: {stem} (r={seed}, D={dc_pct}) ...", end=" ", flush=True)

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

                        # parse container (use minr report keys)
                        parsed = {}
                        is_timeout = False

                        try:
                            subprocess.run(abc_cmd, shell=True, timeout=TIMEOUT_SEC, check=True, capture_output=True)
                        except subprocess.TimeoutExpired:
                            print(f"[Timeout after {TIMEOUT_SEC}s]")
                            is_timeout = True
                        except subprocess.CalledProcessError as e:
                            print(f"[Error: ABC process failed with return code {e.returncode}]")

                        if not is_timeout and os.path.exists(log_path):
                            with open(log_path, "r", encoding="utf-8") as f:
                                content = f.read()
                                for key, pattern in patterns.items():
                                    match = re.search(pattern, content)
                                    if match:
                                        parsed[key] = match.group(1)
                            print("[Done]")
                        elif not is_timeout:
                            print(f"[Warning: Log file not found at {log_path}]")

                        # build detail row in requested column order
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

                        detail_writer.writerow(out)
                        detail_f.flush()
                        os.fsync(detail_f.fileno())
        except KeyboardInterrupt:
            print(f"\n[Interrupt] Stopped by user. Partial detail saved to {detail_csv}")

    print(f"\nAll tasks finished. Detail saved to {detail_csv}")
    print("All tasks finished. (No stat generated; use script/stat.py)")


if __name__ == "__main__":
    main()

