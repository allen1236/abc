"""
平行版 minr 實驗腳本（批次 benchmark × seed × dc_ratio）。
各 job 回傳一列；主程式依完成順序寫入 detail CSV。

用法:
  python3 script/parallel.py --prefix <prefix> [--max-workers N]
  python3 script/parallel.py --csv <existing_detail.csv> --prefix <new_prefix> [--max-workers N]

環境變數:
  MINR_EXP_WORKERS  預設並行數（預設 8）；若命令列有給第二個數字則覆寫。

終端機狀態標籤（每個 job 結束時）:
  [ok]      abc 正常結束，且 log 裡至少解析到一個欄位（patterns 有命中）
  [fail]    abc 非零結束、沒有 log、或 log 完全對不上 patterns（parsed 為空）
  [timeout] Python subprocess 超過 TIMEOUT_SEC

進度列: [start active=N] = 目前有 N 個 job 在跑；[finish D/T active=A] = 已完成 D/T，剩餘並行中 A 個。

Ctrl+C: 終止所有已啟動的 abc 子程序、取消尚未執行的 job，並結束程式（exit 130）；已寫入 CSV 的列會保留。
"""

import os
import re
import csv
import sys
import subprocess
import threading
import argparse
import signal
from concurrent.futures import ThreadPoolExecutor, as_completed, CancelledError
from datetime import datetime

# subprocess: ThreadPoolExecutor.shutdown(cancel_futures=...) needs 3.9+
_PY39 = sys.version_info >= (3, 9)

# ==========================================
# 實驗參數設定區（benchmark 僅留較小電路方便試跑）
# ==========================================

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

BENCHMARKS = [
    "iscas89/s9234.aig",
    "iscas89/s13207.aig",
    "iscas89/s38417.aig",
    "itc99/b12.aig",
    "itc99/b20.aig",
    "itc99/b21.aig",
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
    "itc99/b13.aig",
    "itc99/b14.aig",
    "iscas89/s1196.aig",
    "iscas89/s1238.aig",
    "iscas89/s1423.aig",
    "iscas89/s1488.aig",
    "iscas89/s15850.aig",
    "iscas89/s27.aig",
    "iscas89/s298.aig",
    "iscas89/s344.aig",
    "iscas89/s349.aig",
    "iscas89/s35932.aig",
    "iscas89/s382.aig",
    "iscas89/s386.aig",
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
    "iscas89/s953.aig",
    "itc99/b15.aig",
    "itc99/b17.aig",
    "itc99/b18.aig",
    "itc99/b19.aig",
    "itc99/b22.aig",
    # "iscas89/s38584.aig",
]

BENCHMARK_DIR = os.path.join(ROOT_DIR, "benchmarks")
SCRIPT_DIR = os.path.join(ROOT_DIR, "script")
LOG_DIR = os.path.join(SCRIPT_DIR, "log")
EXP_DIR = os.path.join(SCRIPT_DIR, "exp")

ABC_BINARY = os.path.join(ROOT_DIR, "abc")
TIMEOUT_SEC = 1200

K = 0
SEEDS = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
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
    return 12


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
        "r_f": r"r_f\s*=\s*([\d.]+%?)",
        "r_s": r"r_s\s*=\s*([\d.]+%?|N/A)",
        "k0_r_f": r"k0_r_f\s*=\s*([\d.]+%?|N/A)",
        "k0_r_s": r"k0_r_s\s*=\s*([\d.]+%?|N/A)",
        "r_f_before_refine": r"r_f_before_refine\s*=\s*([\d.]+%?|N/A)",
        "r_s_before_refine": r"r_s_before_refine\s*=\s*([\d.]+%?|N/A)",
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

    ap = argparse.ArgumentParser(
        description="Parallel minr experiment runner. Optionally rerun timeout cases from an existing CSV."
    )
    ap.add_argument(
        "--prefix",
        default="exp",
        help="Output filename prefix (default: exp)",
    )
    ap.add_argument(
        "--csv",
        default="",
        help="If set, do NOT use BENCHMARKS/SEEDS/DC_RATIO grid; instead rerun only timeout rows in this CSV.",
    )
    ap.add_argument(
        "--max-workers",
        type=int,
        default=0,
        help="Override max parallel workers (default: env MINR_EXP_WORKERS or script default).",
    )
    args = ap.parse_args()

    prefix_base = (args.prefix or "exp").strip()
    prefix_base = re.sub(r"[^\w\-]", "_", prefix_base) if prefix_base else "exp"

    max_workers = MAX_WORKERS if not args.max_workers else max(1, int(args.max_workers))

    detail_csv = build_csv_path(f"{prefix_base}_parallel_detail_")

    def bench_stem(path_or_name: str) -> str:
        base = os.path.basename(path_or_name)
        stem, _ext = os.path.splitext(base)
        return stem

    default_headers = [
        "circuit",
        "inputs",
        "outputs",
        "nodes",
        "ff",
        "dc_ratio",
        "specified",
        "seed",
        "k0_r_f",
        "refine_mode",
        "best_k",
        "required_reset",
        "r_f",
        "r_s",
        "runtime_sec",
        "opt_status",
        "cut_verified",
        "cec_verified",
        "r_f_before_refine",
        "r_s_before_refine",
        "refine_by_trial",
        "refine_by_core",
        "refine_sec",
        "spec_ro_in_cut",
        "sim_reg_mismatch_weak",
        "sim_reg_mismatch_strong",
    ]

    def _is_na(v: object) -> bool:
        if v is None:
            return True
        s = str(v).strip()
        if s == "":
            return True
        u = s.upper()
        return u == "NA" or u == "N/A"

    def _is_timeout_row(row: dict) -> bool:
        """
        依使用者約定：timeout 列除了 circuit/dc_ratio/seed 之外，其餘欄位皆為 NA。
        這邊用「所有其他欄位皆 NA」來判定。
        """
        keep = {"circuit", "dc_ratio", "seed"}
        for k, v in row.items():
            if k in keep:
                continue
            if not _is_na(v):
                return False
        return True

    def _resolve_bench_path(circuit_field: str) -> str:
        """
        CSV 的 circuit 欄位通常是 stem（例如 s13207 / b12）。
        這裡把它解析回 BENCHMARK_DIR 底下的相對路徑（例如 iscas89/s13207.aig）。
        """
        c = (circuit_field or "").strip()
        if not c:
            return ""
        # 若已是相對路徑或檔名
        if "/" in c or c.endswith(".aig"):
            return c
        # 優先在 BENCHMARKS 列表用 stem 對應（避免猜錯資料集）
        matches = [b for b in BENCHMARKS if bench_stem(b) == c]
        if len(matches) == 1:
            return matches[0]
        # 再嘗試常見資料夾
        cand = os.path.join("iscas89", f"{c}.aig")
        if os.path.exists(os.path.join(BENCHMARK_DIR, cand)):
            return cand
        cand = os.path.join("itc99", f"{c}.aig")
        if os.path.exists(os.path.join(BENCHMARK_DIR, cand)):
            return cand
        return c

    # 先決定輸出欄位順序（csv 模式用原檔 header；否則用預設）
    input_rows_to_copy = []
    jobs = []
    output_headers = list(default_headers)
    if args.csv:
        with open(args.csv, newline="", encoding="utf-8") as f:
            r = csv.DictReader(f)
            if r.fieldnames:
                output_headers = list(r.fieldnames)
            for row in r:
                if _is_timeout_row(row):
                    circuit = (row.get("circuit") or "").strip()
                    dc_ratio = (row.get("dc_ratio") or "").strip()
                    seed = (row.get("seed") or "").strip()
                    if not circuit or _is_na(dc_ratio) or _is_na(seed):
                        continue
                    try:
                        dci = int(float(dc_ratio))
                        si = int(float(seed))
                    except Exception:
                        continue
                    bench_rel = _resolve_bench_path(circuit)
                    if bench_rel:
                        jobs.append((bench_rel, si, dci))
                else:
                    input_rows_to_copy.append(row)
    else:
        jobs = [(b, s, d) for b in BENCHMARKS for s in SEEDS for d in DC_RATIO]

    n_jobs = len(jobs)
    progress_lock = threading.Lock()
    run_state = {"active": 0, "done": 0}
    child_procs_lock = threading.Lock()
    child_procs = []

    def terminate_all_abc_children():
        """終止所有由本程式啟動、仍在跑的 abc（含 shell 子程序）。"""
        with child_procs_lock:
            snap = list(child_procs)
        for p in snap:
            try:
                if p.poll() is None:
                    # shell=True + start_new_session=True: kill whole process group to avoid orphaned abc
                    os.killpg(p.pid, signal.SIGTERM)
            except Exception:
                pass
        # 給 SIGTERM 一點時間，再補 SIGKILL
        try:
            import time

            time.sleep(0.2)
        except Exception:
            pass
        with child_procs_lock:
            snap = list(child_procs)
        for p in snap:
            try:
                if p.poll() is None:
                    os.killpg(p.pid, signal.SIGKILL)
            except Exception:
                pass

    def run_one_job(bench: str, seed: int, dc_pct: int) -> dict:
        stem = bench_stem(bench)
        with progress_lock:
            run_state["active"] += 1
            print(
                f"[start active={run_state['active']}] {stem} r={seed} D={dc_pct}",
                flush=True,
            )

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

        status = "fail"
        try:
            parsed = {}
            is_timeout = False

            proc = subprocess.Popen(
                abc_cmd,
                shell=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            with child_procs_lock:
                child_procs.append(proc)
            try:
                try:
                    proc.wait(timeout=TIMEOUT_SEC)
                except subprocess.TimeoutExpired:
                    try:
                        os.killpg(proc.pid, signal.SIGKILL)
                    except Exception:
                        pass
                    try:
                        proc.wait(timeout=5)
                    except Exception:
                        pass
                    is_timeout = True
                else:
                    if proc.returncode != 0:
                        pass  # 與原 subprocess.run(..., check=True) 失敗時相同，不擲出
            finally:
                with child_procs_lock:
                    try:
                        child_procs.remove(proc)
                    except ValueError:
                        pass

            if not is_timeout and os.path.exists(log_path):
                with open(log_path, "r", encoding="utf-8") as f:
                    content = f.read()
                    for key, pattern in patterns.items():
                        match = re.search(pattern, content)
                        if match:
                            parsed[key] = match.group(1)

            out = {h: "NA" for h in output_headers}
            out["circuit"] = stem
            out["dc_ratio"] = str(dc_pct)
            out["seed"] = str(seed)
            out["inputs"] = parsed.get("inputs", "NA")
            out["outputs"] = parsed.get("outputs", "NA")
            out["nodes"] = parsed.get("nodes", "NA")
            out["ff"] = parsed.get("ff", "NA")
            out["specified"] = parsed.get("specified_regs", "NA")
            out["k0_r_f"] = parsed.get("k0_r_f", "NA") if OPTIMIZE_MODE else "NA"
            out["refine_mode"] = parsed.get("refine_mode", "NA")
            out["best_k"] = parsed.get("best_k", "NA") if OPTIMIZE_MODE else "NA"
            out["required_reset"] = parsed.get("required_reset", "NA")
            out["r_f"] = parsed.get("r_f", "NA")
            out["r_s"] = parsed.get("r_s", "NA")
            out["runtime_sec"] = parsed.get("runtime_sec", "NA")
            out["opt_status"] = parsed.get("opt_status", "NA") if OPTIMIZE_MODE else "NA"
            out["cut_verified"] = parsed.get("cut_verified", "NA")
            out["cec_verified"] = parsed.get("cec_verified", "NA")
            out["r_f_before_refine"] = parsed.get("r_f_before_refine", "NA")
            out["r_s_before_refine"] = parsed.get("r_s_before_refine", "NA")
            out["refine_by_trial"] = parsed.get("refine_by_trial", "NA")
            out["refine_by_core"] = parsed.get("refine_by_core", "NA")
            out["refine_sec"] = parsed.get("refine_sec", "NA")
            out["spec_ro_in_cut"] = parsed.get("spec_ro_in_cut", "NA")
            out["sim_reg_mismatch_weak"] = parsed.get("sim_reg_mismatch_weak", "NA")
            out["sim_reg_mismatch_strong"] = parsed.get("sim_reg_mismatch_strong", "NA")

            status = "timeout" if is_timeout else ("ok" if parsed else "fail")
            return out
        finally:
            with progress_lock:
                run_state["active"] -= 1
                run_state["done"] += 1
                print(
                    f"[finish {run_state['done']}/{n_jobs} active={run_state['active']}] [{status}] {stem} r={seed} D={dc_pct}",
                    flush=True,
                )

    print(f"Parallel detail CSV: {detail_csv}")
    print(f"Jobs: {n_jobs}, max_workers: {max_workers}")
    if args.csv:
        print(f"CSV rerun mode: {args.csv}")
        print(f"Copy-through rows (non-timeout): {len(input_rows_to_copy)}")

    write_lock = threading.Lock()
    interrupted = False

    with open(detail_csv, "w", newline="", encoding="utf-8") as detail_f:
        detail_writer = csv.DictWriter(detail_f, fieldnames=output_headers)
        detail_writer.writeheader()
        # 先把原本「非 timeout」列原封不動寫入
        if input_rows_to_copy:
            for row in input_rows_to_copy:
                out_row = {h: row.get(h, "NA") for h in output_headers}
                detail_writer.writerow(out_row)
        detail_f.flush()
        os.fsync(detail_f.fileno())

        executor = ThreadPoolExecutor(max_workers=max_workers)
        try:
            future_map = {
                executor.submit(run_one_job, b, s, d): (b, s, d) for b, s, d in jobs
            }
            for fut in as_completed(future_map):
                try:
                    row = fut.result()
                except CancelledError:
                    continue
                with write_lock:
                    detail_writer.writerow(row)
                    detail_f.flush()
                    os.fsync(detail_f.fileno())
        except KeyboardInterrupt:
            interrupted = True
            print("\n[Interrupt] 正在停止所有 abc 子程序與未開始的 job …", flush=True)
            terminate_all_abc_children()
            if _PY39:
                executor.shutdown(wait=False, cancel_futures=True)
            else:
                executor.shutdown(wait=False)
            print(f"[Interrupt] 已中止。已寫入的列仍保留於 {detail_csv}", flush=True)
            sys.exit(130)
        finally:
            if not interrupted:
                executor.shutdown(wait=True)

    if not interrupted:
        print(f"\nAll tasks finished. Detail saved to {detail_csv}")
        print("All tasks finished. (No stat generated; use script/stat.py)")


if __name__ == "__main__":
    main()
