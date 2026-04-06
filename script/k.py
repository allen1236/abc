"""
依 k 掃描結果匯出 CSV（搭配 &minr -O 1 -k K_MIN -K K_MAX）。

輸出兩個檔案：
1) *_detail_*.csv：每個 (電路, seed, dc_ratio) 一列，含各 k 的 reset / r_s / runtime_sec（與先前相同）。
2) *_pivot_*.csv：每列一個 k；欄位先 k，再 **依電路** 輪流：每個電路一段為
   該電路各 dc 的 runtime → 該電路各 dc 的 R/F → 該電路各 dc 的 R/S，
   然後下一個電路重複同樣順序。同一 metric 內 dc 欄依 specified 由大到小（S100 在前）。
   欄名例：s1423_rt_S100、s1423_rf_S100、s1423_rs_S100（Sxx = xx% specified）。

r_s（R/S）與 minr report [result] 一致：100 * (required_reset / specified_regs)。

檔名前綴預設 k_。需已建置支援 -K 的 abc。

平行執行（與 parallel.py 類似）:
  python script/k.py [prefix] [max_workers]
  環境變數 MINR_EXP_WORKERS 可設預設並行數（預設 8）。
  Ctrl+C 會終止所有 abc 子程序並 exit 130；結束時寫入已收集的列。
"""

import os
import re
import csv
import sys
import subprocess
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed, CancelledError
from datetime import datetime

_PY39 = sys.version_info >= (3, 9)

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

BENCHMARKS = [
    # "iscas89/s5378.aig",
    "iscas89/s35932.aig",
    "iscas89/s9234.aig",
    "iscas89/s15850.aig",
    "iscas89/s13207.aig",
    "itc99/b12.aig",
    "itc99/b14.aig",
    "itc99/b20.aig",
]

BENCHMARK_DIR = os.path.join(ROOT_DIR, "benchmarks")
SCRIPT_DIR = os.path.join(ROOT_DIR, "script")
LOG_DIR = os.path.join(SCRIPT_DIR, "log")
EXP_DIR = os.path.join(SCRIPT_DIR, "exp")

ABC_BINARY = os.path.join(ROOT_DIR, "abc")
TIMEOUT_SEC = 4000

K_MIN = 0  # 傳給 &minr 的 -k：dense 掃描起始 k（未給 -k 時 minr 預設 0）
K_MAX = 40  # 傳給 &minr 的 -K：掃描到此 k（含）；需 K_MIN <= K_MAX
SEEDS = [0]
RANDOM_SIM_CYCLE = 100
REFINE_MODE = 0
REFINE_BIND_DC = True
REFINE_CONF_LIMIT = 10000
REFINE_CORE_ONLY = False
OTHER_ARGS = ""
OPTIMIZE_MODE = 1
DC_RATIO = [0, 50]
TOTAL_TIMEOUT = 3600


def _default_max_workers() -> int:
    w = os.environ.get("MINR_EXP_WORKERS", "").strip()
    if w.isdigit():
        return max(1, int(w))
    return 7

MAX_WORKERS = _default_max_workers()

def _to_int(s):
    if s is None:
        return None
    s = str(s).strip()
    if not s or s.upper() in ("NA", "N/A"):
        return None
    try:
        return int(float(s))
    except ValueError:
        return None


def r_s_from_reset_and_specified(reset_str: str, specified_str: str) -> str:
    """與 minr [result] r_s 相同：100 * (required_reset / specified_regs)。"""
    r = _to_int(reset_str)
    s = _to_int(specified_str)
    if r is None or s is None or s <= 0:
        return "NA"
    pct = 100.0 * (float(r) / float(s))
    return f"{pct:.2f}%"


def r_f_from_reset_and_ff(reset_str: str, ff_str: str) -> str:
    """R/F：100 * (required_reset / ff)，ff 為電路暫存器總數。"""
    r = _to_int(reset_str)
    f = _to_int(ff_str)
    if r is None or f is None or f <= 0:
        return "NA"
    pct = 100.0 * (float(r) / float(f))
    return f"{pct:.2f}%"


def dc_ratio_to_s_label(dc_pct: int) -> int:
    """dc_ratio 為 don't-care 百分比；回傳欄名用的 specified 百分比（S100 = 100% specified）。"""
    return 100 - int(dc_pct)


def sort_dc_ratios_for_columns(dc_list: list) -> list:
    """同一 metric 內欄位順序：specified 比例大者在前（即 dc_ratio 小者在前）。"""
    return sorted(dc_list, key=lambda d: (100 - int(d)), reverse=True)


def parse_iterations_block(content: str):
    """Parse [iterations] lines into { k: {reset, runtime_sec} }（reset = 該 k 的 required_reset）。"""
    out = {}
    sec = re.search(r"\[iterations\]\s*\n(.*?)(?:\n\[|\Z)", content, re.S)
    if not sec:
        return out
    block = sec.group(1)
    # k=1, resets=3, r_s=40.00%, 2ms
    for m in re.finditer(
        r"^k=(\d+),\s*resets=(\d+),\s*r_s=(N/A|[\d.]+%),\s*(\d+)ms\s*$", block, re.M
    ):
        k = int(m.group(1))
        out[k] = {
            "reset": m.group(2),
            "runtime_sec": f"{int(m.group(4)) / 1000.0:.6f}",
        }
    # Legacy: k=1, resets=3, reduction=40.00%, 2ms
    for m in re.finditer(
        r"^k=(\d+),\s*resets=(\d+),\s*reduction=(N/A|[\d.]+%),\s*(\d+)ms\s*$", block, re.M
    ):
        k = int(m.group(1))
        if k in out:
            continue
        out[k] = {
            "reset": m.group(2),
            "runtime_sec": f"{int(m.group(4)) / 1000.0:.6f}",
        }
    # Legacy: k=1, resets=3, 2ms
    for m in re.finditer(r"^k=(\d+),\s*resets=(\d+),\s*(\d+)ms\s*$", block, re.M):
        k = int(m.group(1))
        if k in out:
            continue
        out[k] = {
            "reset": m.group(2),
            "runtime_sec": f"{int(m.group(3)) / 1000.0:.6f}",
        }
    # Failure: k=1, unsat, r_s=N/A, 2ms
    for m in re.finditer(
        r"^k=(\d+),\s*(?:unsat|timeout|error),\s*r_s=N/A,\s*(\d+)ms\s*$", block, re.M
    ):
        k = int(m.group(1))
        out[k] = {"reset": "NA", "runtime_sec": f"{int(m.group(2)) / 1000.0:.6f}"}
    for m in re.finditer(
        r"^k=(\d+),\s*(?:unsat|timeout|error),\s*reduction=N/A,\s*(\d+)ms\s*$", block, re.M
    ):
        k = int(m.group(1))
        if k in out:
            continue
        out[k] = {"reset": "NA", "runtime_sec": f"{int(m.group(2)) / 1000.0:.6f}"}
    # Legacy fail: k=1, unsat, 2ms
    for m in re.finditer(r"^k=(\d+),\s*(unsat|timeout|error),\s*(\d+)ms\s*$", block, re.M):
        k = int(m.group(1))
        if k in out:
            continue
        out[k] = {"reset": "NA", "runtime_sec": f"{int(m.group(3)) / 1000.0:.6f}"}
    return out


def main():
    if K_MIN > K_MAX:
        print(f"Error: K_MIN ({K_MIN}) must be <= K_MAX ({K_MAX}).", file=sys.stderr)
        sys.exit(1)

    os.makedirs(LOG_DIR, exist_ok=True)
    os.makedirs(EXP_DIR, exist_ok=True)

    patterns = {
        "nodes": r"nodes\s*=\s*(\d+)",
        "ff": r"ff\s*=\s*(\d+)",
        "specified_regs": r"specified_regs\s*=\s*(\d+)",
    }

    def build_log_suffix(seed: int, dc_pct: int):
        parts = [f"O{OPTIMIZE_MODE}", f"k{K_MIN}", f"K{K_MAX}", f"r{seed}", f"R{RANDOM_SIM_CYCLE}"]
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
            f"K{K_MIN}-{K_MAX}",
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

    max_workers = MAX_WORKERS
    if len(sys.argv) > 2 and sys.argv[2].strip().isdigit():
        max_workers = max(1, int(sys.argv[2].strip()))

    detail_csv = build_csv_path(f"{prefix_base}_detail_")
    pivot_csv = build_csv_path(f"{prefix_base}_pivot_")

    def bench_stem(path_or_name: str) -> str:
        base = os.path.basename(path_or_name)
        stem, _ext = os.path.splitext(base)
        return stem

    base_headers = ["circuit", "nodes", "ff", "dc_ratio", "specified", "seed"]
    k_headers = []
    for kk in range(K_MIN, K_MAX + 1):
        k_headers.extend([f"k{kk}_reset", f"k{kk}_r_s", f"k{kk}_runtime_sec"])
    headers = base_headers + k_headers

    def merged_metrics_for_k(matches: list, kk: int) -> tuple:
        """同一 (電路, dc)、可能多 seed：平均 runtime；reset 取平均後算 R/F、R/S。"""
        if not matches:
            return ("NA", "NA", "NA")
        runtimes = []
        resets = []
        specified = None
        ff = None
        for r in matches:
            rt = r.get(f"k{kk}_runtime_sec", "NA")
            try:
                if rt != "NA" and str(rt).strip():
                    runtimes.append(float(rt))
            except ValueError:
                pass
            rs = r.get(f"k{kk}_reset", "NA")
            ri = _to_int(rs)
            if ri is not None:
                resets.append(float(ri))
            if specified is None:
                specified = r.get("specified", "NA")
            if ff is None:
                ff = r.get("ff", "NA")
        avg_rt = f"{sum(runtimes) / len(runtimes):.6f}" if runtimes else "NA"
        avg_reset = (
            str(int(round(sum(resets) / len(resets)))) if resets else "NA"
        )
        rf = r_f_from_reset_and_ff(avg_reset, ff if ff is not None else "NA")
        r_s = r_s_from_reset_and_specified(
            avg_reset, specified if specified is not None else "NA"
        )
        return (avg_rt, rf, r_s)

    def build_pivot_fieldnames() -> list:
        cols = ["k"]
        dc_order = sort_dc_ratios_for_columns(DC_RATIO)
        for bench in BENCHMARKS:
            stem = bench_stem(bench)
            for dc in dc_order:
                s = dc_ratio_to_s_label(dc)
                cols.append(f"{stem}_rt_S{s}")
            for dc in dc_order:
                s = dc_ratio_to_s_label(dc)
                cols.append(f"{stem}_rf_S{s}")
            for dc in dc_order:
                s = dc_ratio_to_s_label(dc)
                cols.append(f"{stem}_rs_S{s}")
        return cols

    def build_pivot_rows(all_rows: list) -> list:
        dc_order = sort_dc_ratios_for_columns(DC_RATIO)
        out = []
        for kk in range(K_MIN, K_MAX + 1):
            row = {"k": kk}
            for bench in BENCHMARKS:
                stem = bench_stem(bench)
                for dc in dc_order:
                    matches = [
                        r
                        for r in all_rows
                        if r["circuit"] == stem and int(r["dc_ratio"]) == dc
                    ]
                    rt, rf, rs = merged_metrics_for_k(matches, kk)
                    s = dc_ratio_to_s_label(dc)
                    row[f"{stem}_rt_S{s}"] = rt
                    row[f"{stem}_rf_S{s}"] = rf
                    row[f"{stem}_rs_S{s}"] = rs
            out.append(row)
        return out

    jobs = [(b, s, d) for b in BENCHMARKS for s in SEEDS for d in DC_RATIO]
    n_jobs = len(jobs)
    progress_lock = threading.Lock()
    run_state = {"active": 0, "done": 0}
    child_procs_lock = threading.Lock()
    child_procs = []

    def terminate_all_abc_children():
        with child_procs_lock:
            snap = list(child_procs)
        for p in snap:
            try:
                if p.poll() is None:
                    p.terminate()
            except Exception:
                pass
        time.sleep(0.2)
        with child_procs_lock:
            snap = list(child_procs)
        for p in snap:
            try:
                if p.poll() is None:
                    p.kill()
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

        opt_arg = f"-O {OPTIMIZE_MODE}"
        k_lo_arg = f"-k {K_MIN} "
        k_hi_arg = f"-K {K_MAX}"
        abc_cmd = (
            f'{ABC_BINARY} -c "read_aiger {src_aig}; &get; &ps;'
            f'&minr {opt_arg} {k_lo_arg}{k_hi_arg} -r {seed} -R {RANDOM_SIM_CYCLE} {dc_arg} {refine_arg}{bind_arg}{conf_arg}{core_only_arg} {timeout_arg} {OTHER_ARGS} -o {log_path}"'
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
                        proc.kill()
                    except Exception:
                        pass
                    try:
                        proc.wait(timeout=5)
                    except Exception:
                        pass
                    is_timeout = True
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

            for kk in range(K_MIN, K_MAX + 1):
                if kk in by_k:
                    row[f"k{kk}_reset"] = by_k[kk]["reset"]
                    row[f"k{kk}_runtime_sec"] = by_k[kk]["runtime_sec"]
                else:
                    row[f"k{kk}_reset"] = "NA"
                    row[f"k{kk}_runtime_sec"] = "NA"
                row[f"k{kk}_r_s"] = r_s_from_reset_and_specified(
                    row[f"k{kk}_reset"], row["specified"]
                )

            status = "timeout" if is_timeout else ("ok" if parsed else "fail")
            return row
        finally:
            with progress_lock:
                run_state["active"] -= 1
                run_state["done"] += 1
                print(
                    f"[finish {run_state['done']}/{n_jobs} active={run_state['active']}] [{status}] {stem} r={seed} D={dc_pct}",
                    flush=True,
                )

    print(f"k.py detail CSV: {detail_csv}")
    print(f"k.py pivot CSV: {pivot_csv}")
    print(f"Jobs: {n_jobs}, max_workers: {max_workers}")

    all_rows = []
    all_rows_lock = threading.Lock()
    interrupted = False

    def write_outputs():
        if not all_rows:
            return
        with open(detail_csv, "w", newline="", encoding="utf-8") as detail_f:
            detail_writer = csv.DictWriter(detail_f, fieldnames=headers)
            detail_writer.writeheader()
            for row in all_rows:
                detail_writer.writerow(row)
        pivot_fieldnames = build_pivot_fieldnames()
        pivot_rows = build_pivot_rows(all_rows)
        with open(pivot_csv, "w", newline="", encoding="utf-8") as pivot_f:
            pivot_writer = csv.DictWriter(pivot_f, fieldnames=pivot_fieldnames)
            pivot_writer.writeheader()
            for prow in pivot_rows:
                pivot_writer.writerow(prow)

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
            with all_rows_lock:
                all_rows.append(row)
    except KeyboardInterrupt:
        interrupted = True
        print("\n[Interrupt] 正在停止所有 abc 子程序與未開始的 job …", flush=True)
        terminate_all_abc_children()
        if _PY39:
            executor.shutdown(wait=False, cancel_futures=True)
        else:
            executor.shutdown(wait=False)
        write_outputs()
        print(
            f"[Interrupt] 已中止。已寫入 detail: {detail_csv}，pivot: {pivot_csv}（若有資料）",
            flush=True,
        )
        sys.exit(130)
    finally:
        if not interrupted:
            executor.shutdown(wait=True)

    if not interrupted:
        write_outputs()
        print(f"\nAll tasks finished. Detail: {detail_csv}")
        print(f"Pivot (k 為列、依電路區塊 rt→rf→rs): {pivot_csv}")


if __name__ == "__main__":
    main()
