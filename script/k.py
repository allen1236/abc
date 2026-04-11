"""
依 k 掃描結果匯出 CSV：每個 (電路, seed, dc_ratio, k) **獨立**呼叫一次 abc，
`&minr -O 0 -k <k> …`（不開 refine、不做 -O1 dense sweep），讓每個 k 有完整 `-t` 預算。

傳給 minr 的 `-t` 為 TOTAL_TIMEOUT（秒）；外層 `subprocess.wait` 為 TOTAL_TIMEOUT+30，
避免 minr 用滿 CPU 預算前就被 Python 先殺掉。

輸出兩個檔案：
1) *_detail_*.csv：每個 (電路, seed, dc_ratio) 一列，含各 k 的 reset / r_s / runtime_sec（與 runtime_cpu_sec 同義）/ runtime_cpu_sec / runtime_wall_sec。
2) *_pivot_*.csv：每列一個 k；欄位先 k，再 **依電路** 輪流：每個電路一段為
   該電路各 dc 的 runtime → 該電路各 dc 的 R/F → 該電路各 dc 的 R/S，
   然後下一個電路重複同樣順序。同一 metric 內 dc 欄依 specified 由大到小（S100 在前）。

r_s（R/S）與 minr report [result] 一致：100 * (required_reset / specified_regs)。

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
import signal
from concurrent.futures import ThreadPoolExecutor, as_completed, CancelledError
from datetime import datetime

_PY39 = sys.version_info >= (3, 9)

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

BENCHMARKS = [
    "iscas89/s9234.aig",
    # "iscas89/s35932.aig",
    # "iscas89/s15850.aig",
    # "iscas89/s13207.aig",
    # "itc99/b12.aig",
    # "itc99/b14.aig",
    # "itc99/b20.aig",
]

BENCHMARK_DIR = os.path.join(ROOT_DIR, "benchmarks")
SCRIPT_DIR = os.path.join(ROOT_DIR, "script")
LOG_DIR = os.path.join(SCRIPT_DIR, "log")
EXP_DIR = os.path.join(SCRIPT_DIR, "exp")

ABC_BINARY = os.path.join(ROOT_DIR, "abc")
# 傳給 &minr 的 -t（thread-CPU 秒）；外層 subprocess.wait 會多等 PROC_WAIT_EXTRA_SEC
TOTAL_TIMEOUT = 800
PROC_WAIT_EXTRA_SEC = 100

K_MIN = 0  # 掃描 k 下限（含）
K_MAX = 40  # 掃描 k 上限（含）；每個 k 各跑一次 single-k solve
SEEDS = [0]
RANDOM_SIM_CYCLE = 100
REFINE_MODE = 0  # 不 refine（不加 -x）
REFINE_BIND_DC = True
REFINE_CONF_LIMIT = 10000
REFINE_CORE_ONLY = False
OTHER_ARGS = ""
# 0 = 單一 k 求解，命令列**不帶** -O（minr 只接受 -O 1 或 -O 2）
OPTIMIZE_MODE = 0
DC_RATIO = [0, 50]


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


def merge_job_results(job_results: list, headers: list, k_min: int, k_max: int) -> list:
    """將各單一 k 子工作的結果併成每個 (電路, seed, dc) 一列（與原本 detail 寬表相同）。"""
    buckets = {}
    for jr in job_results:
        key = (jr["circuit"], jr["seed"], jr["dc_ratio"])
        if key not in buckets:
            row = {h: "NA" for h in headers}
            row["circuit"] = jr["circuit"]
            row["seed"] = jr["seed"]
            row["dc_ratio"] = jr["dc_ratio"]
            buckets[key] = row
        row = buckets[key]
        for fld in ("nodes", "ff", "specified"):
            v = jr.get(fld)
            if v is not None and str(v).strip() and str(v).strip().upper() not in ("NA", "N/A"):
                row[fld] = v
        kk = jr["kk"]
        row[f"k{kk}_reset"] = jr["k_reset"]
        row[f"k{kk}_runtime_sec"] = jr["k_runtime_sec"]
        row[f"k{kk}_runtime_cpu_sec"] = jr.get("k_runtime_cpu_sec", "NA")
        row[f"k{kk}_runtime_wall_sec"] = jr.get("k_runtime_wall_sec", "NA")
    for row in buckets.values():
        spec = row.get("specified", "NA")
        for kk in range(k_min, k_max + 1):
            row[f"k{kk}_r_s"] = r_s_from_reset_and_specified(row[f"k{kk}_reset"], spec)
    return sorted(buckets.values(), key=lambda x: (x["circuit"], x["dc_ratio"], x["seed"]))


def _parse_resume_arg(argv: list) -> str:
    """
    支援：
      --resume_pivot /path/to/k_pivot.csv
      --resume_pivot=/path/to/k_pivot.csv
    """
    for i, a in enumerate(argv):
        if a == "--resume_pivot" and i + 1 < len(argv):
            return argv[i + 1].strip()
        if a.startswith("--resume_pivot="):
            return a.split("=", 1)[1].strip()
    return ""


def _stem_to_bench_path(stem: str) -> str:
    for b in BENCHMARKS:
        if os.path.splitext(os.path.basename(b))[0] == stem:
            return b
    return ""


def _safe_float_str(x: str) -> str:
    if x is None:
        return "NA"
    s = str(x).strip()
    if not s or s.upper() in ("NA", "N/A"):
        return "NA"
    return s


def _iter_row_ms(cpu_ms: int, wall_ms=None) -> dict:
    wall_ms = wall_ms if wall_ms is not None else cpu_ms
    cpu_s = f"{cpu_ms / 1000.0:.6f}"
    wall_s = f"{wall_ms / 1000.0:.6f}"
    return {
        "runtime_sec": cpu_s,
        "runtime_cpu_sec": cpu_s,
        "runtime_wall_sec": wall_s,
    }


def parse_iterations_block(content: str):
    """Parse [iterations] → { k: { reset, runtime_sec, runtime_cpu_sec, runtime_wall_sec } }。"""
    out = {}
    sec = re.search(r"\[iterations\]\s*\n(.*?)(?:\n\[|\Z)", content, re.S)
    if not sec:
        return out
    block = sec.group(1)
    # New: k=1, resets=3, r_s=40.00%, cpu_ms=2, wall_ms=5
    for m in re.finditer(
        r"^k=(\d+),\s*resets=(\d+),\s*r_s=(N/A|[\d.]+%),\s*cpu_ms=(\d+),\s*wall_ms=(\d+)\s*$",
        block,
        re.M,
    ):
        k = int(m.group(1))
        t = _iter_row_ms(int(m.group(4)), int(m.group(5)))
        out[k] = {"reset": m.group(2), **t}
    # New fail: k=1, unsat, r_s=N/A, cpu_ms=2, wall_ms=5
    for m in re.finditer(
        r"^k=(\d+),\s*(unsat|timeout|error),\s*r_s=N/A,\s*cpu_ms=(\d+),\s*wall_ms=(\d+)\s*$",
        block,
        re.M,
    ):
        k = int(m.group(1))
        if k in out:
            continue
        t = _iter_row_ms(int(m.group(3)), int(m.group(4)))
        out[k] = {"reset": "NA", **t}
    # k=1, resets=3, r_s=40.00%, 2ms
    for m in re.finditer(
        r"^k=(\d+),\s*resets=(\d+),\s*r_s=(N/A|[\d.]+%),\s*(\d+)ms\s*$", block, re.M
    ):
        k = int(m.group(1))
        if k in out:
            continue
        t = _iter_row_ms(int(m.group(4)))
        out[k] = {"reset": m.group(2), **t}
    # Legacy: k=1, resets=3, reduction=40.00%, 2ms
    for m in re.finditer(
        r"^k=(\d+),\s*resets=(\d+),\s*reduction=(N/A|[\d.]+%),\s*(\d+)ms\s*$", block, re.M
    ):
        k = int(m.group(1))
        if k in out:
            continue
        t = _iter_row_ms(int(m.group(4)))
        out[k] = {"reset": m.group(2), **t}
    # Legacy: k=1, resets=3, 2ms
    for m in re.finditer(r"^k=(\d+),\s*resets=(\d+),\s*(\d+)ms\s*$", block, re.M):
        k = int(m.group(1))
        if k in out:
            continue
        t = _iter_row_ms(int(m.group(3)))
        out[k] = {"reset": m.group(2), **t}
    # Failure: k=1, unsat, r_s=N/A, 2ms
    for m in re.finditer(
        r"^k=(\d+),\s*(?:unsat|timeout|error),\s*r_s=N/A,\s*(\d+)ms\s*$", block, re.M
    ):
        k = int(m.group(1))
        if k in out:
            continue
        t = _iter_row_ms(int(m.group(2)))
        out[k] = {"reset": "NA", **t}
    for m in re.finditer(
        r"^k=(\d+),\s*(?:unsat|timeout|error),\s*reduction=N/A,\s*(\d+)ms\s*$", block, re.M
    ):
        k = int(m.group(1))
        if k in out:
            continue
        t = _iter_row_ms(int(m.group(2)))
        out[k] = {"reset": "NA", **t}
    # Legacy fail: k=1, unsat, 2ms
    for m in re.finditer(r"^k=(\d+),\s*(unsat|timeout|error),\s*(\d+)ms\s*$", block, re.M):
        k = int(m.group(1))
        if k in out:
            continue
        t = _iter_row_ms(int(m.group(3)))
        out[k] = {"reset": "NA", **t}
    return out


def parse_flat_report_for_k(content: str, expect_k: int):
    """
    非 -O1 時報告通常沒有 [iterations]，改由 [settings] 的 k 與 [result] 的
    required_reset、runtime_sec 取得該次單一 k 的結果。
    """
    m_set = re.search(r"\[settings\](.*?)(?:\n\[|\Z)", content, re.S)
    if m_set:
        mk = re.search(r"(?m)^\s*k\s*=\s*(\d+)", m_set.group(1))
        if mk and int(mk.group(1)) != expect_k:
            return None
    m_res = re.search(r"\[result\](.*?)(?:\n\[|\Z)", content, re.S)
    if not m_res:
        return None
    block = m_res.group(1)
    st = re.search(r"(?m)^solver_status\s*=\s*(\S+)", block)
    if not st or st.group(1).strip().lower() != "optimum":
        return None
    rr = re.search(r"(?m)^required_reset\s*=\s*(\S+)", block)
    rt = re.search(r"(?m)^runtime_sec\s*=\s*([\d.]+)", block)
    rtc = re.search(r"(?m)^runtime_cpu_sec\s*=\s*([\d.]+)", block)
    rtw = re.search(r"(?m)^runtime_wall_sec\s*=\s*([\d.]+)", block)
    if not rr or not rt:
        return None
    reset_s = rr.group(1).strip()
    if reset_s.upper() in ("N/A", "NA"):
        reset_s = "NA"
    cpu_s = rtc.group(1).strip() if rtc else rt.group(1).strip()
    wall_s = rtw.group(1).strip() if rtw else cpu_s
    return {
        "reset": reset_s,
        "runtime_sec": cpu_s,
        "runtime_cpu_sec": cpu_s,
        "runtime_wall_sec": wall_s,
    }


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

    def build_log_suffix(seed: int, dc_pct: int, kk: int):
        parts = [f"O{OPTIMIZE_MODE}", f"k{kk}", f"r{seed}", f"R{RANDOM_SIM_CYCLE}"]
        if dc_pct > 0:
            parts.append(f"D{dc_pct}")
        if REFINE_MODE > 0:
            parts.append(f"x{REFINE_MODE}")
            if REFINE_BIND_DC:
                parts.append("X")
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
        if REFINE_MODE > 0:
            parts.append(f"x{REFINE_MODE}")
            if REFINE_BIND_DC:
                parts.append("X")
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
        k_headers.extend(
            [
                f"k{kk}_reset",
                f"k{kk}_r_s",
                f"k{kk}_runtime_sec",
                f"k{kk}_runtime_cpu_sec",
                f"k{kk}_runtime_wall_sec",
            ]
        )
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

    resume_pivot = _parse_resume_arg(sys.argv[1:])
    jobs = []
    if not resume_pivot:
        jobs = [
            (b, s, d, kk)
            for b in BENCHMARKS
            for s in SEEDS
            for d in DC_RATIO
            for kk in range(K_MIN, K_MAX + 1)
        ]
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
                    os.killpg(p.pid, signal.SIGTERM)
            except Exception:
                pass
        time.sleep(0.2)
        with child_procs_lock:
            snap = list(child_procs)
        for p in snap:
            try:
                if p.poll() is None:
                    os.killpg(p.pid, signal.SIGKILL)
            except Exception:
                pass

    def run_one_job(bench: str, seed: int, dc_pct: int, kk: int) -> dict:
        stem = bench_stem(bench)
        with progress_lock:
            run_state["active"] += 1
            print(
                f"[start active={run_state['active']}] {stem} r={seed} D={dc_pct} k={kk}",
                flush=True,
            )

        src_aig = os.path.join(BENCHMARK_DIR, bench)
        log_suffix = build_log_suffix(seed, dc_pct, kk)
        log_path = os.path.join(LOG_DIR, f"{stem}{log_suffix}")

        dc_arg = f"-D {dc_pct}" if dc_pct > 0 else ""
        refine_arg = f"-x {REFINE_MODE}" if REFINE_MODE > 0 else ""
        bind_arg = " -X" if (REFINE_MODE > 0 and REFINE_BIND_DC) else ""
        conf_arg = f" -c {REFINE_CONF_LIMIT}" if (REFINE_MODE > 0 and REFINE_CONF_LIMIT is not None) else ""
        core_only_arg = " -C" if (REFINE_MODE > 0 and REFINE_CORE_ONLY) else ""
        timeout_arg = f"-t {TOTAL_TIMEOUT}" if TOTAL_TIMEOUT > 0 else ""

        # minr 只接受 -O 1 或 -O 2；單一 k 時勿傳 -O 0
        opt_arg = f"-O {OPTIMIZE_MODE} " if OPTIMIZE_MODE > 0 else ""
        abc_cmd = (
            f'{ABC_BINARY} -c "read_aiger {src_aig}; &get; &ps;'
            f'&minr {opt_arg}-k {kk} -r {seed} -R {RANDOM_SIM_CYCLE} {dc_arg} {refine_arg}{bind_arg}{conf_arg}{core_only_arg} {timeout_arg} {OTHER_ARGS} -o {log_path}"'
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
                    proc.wait(timeout=TOTAL_TIMEOUT + PROC_WAIT_EXTRA_SEC)
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
                if kk not in by_k:
                    flat = parse_flat_report_for_k(content, kk)
                    if flat:
                        by_k[kk] = flat
            else:
                by_k = {}

            if kk in by_k:
                bk = by_k[kk]
                k_reset = bk["reset"]
                k_runtime_sec = bk["runtime_sec"]
                k_runtime_cpu_sec = bk.get("runtime_cpu_sec", k_runtime_sec)
                k_runtime_wall_sec = bk.get("runtime_wall_sec", k_runtime_sec)
            else:
                k_reset = "NA"
                k_runtime_sec = "NA"
                k_runtime_cpu_sec = "NA"
                k_runtime_wall_sec = "NA"

            status = "timeout" if is_timeout else ("ok" if parsed else "fail")
            return {
                "circuit": stem,
                "seed": str(seed),
                "dc_ratio": str(dc_pct),
                "kk": kk,
                "nodes": parsed.get("nodes", "NA"),
                "ff": parsed.get("ff", "NA"),
                "specified": parsed.get("specified_regs", "NA"),
                "k_reset": k_reset,
                "k_runtime_sec": k_runtime_sec,
                "k_runtime_cpu_sec": k_runtime_cpu_sec,
                "k_runtime_wall_sec": k_runtime_wall_sec,
                "_status": status,
            }
        finally:
            with progress_lock:
                run_state["active"] -= 1
                run_state["done"] += 1
                print(
                    f"[finish {run_state['done']}/{n_jobs} active={run_state['active']}] [{status}] {stem} r={seed} D={dc_pct} k={kk}",
                    flush=True,
                )

    print(f"k.py detail CSV: {detail_csv}")
    print(f"k.py pivot CSV: {pivot_csv}")
    if resume_pivot:
        print(f"Resume pivot: {resume_pivot}")
    print(f"Jobs: {n_jobs}, max_workers: {max_workers}")

    all_rows = []
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

    # --- Resume mode: read a pivot CSV and fill NA cells by rerunning only missing cases ---
    if resume_pivot:
        with open(resume_pivot, newline="", encoding="utf-8") as f:
            r = csv.DictReader(f)
            pivot_in_rows = list(r)
            pivot_in_fields = list(r.fieldnames or [])

        # infer which stems and specified ratios (Sxx) exist in the pivot
        col_re = re.compile(r"^(?P<stem>.+)_(?P<metric>rt|rf|rs)_S(?P<s>\d+)$")
        stems = []
        stem_set = set()
        s_values = set()
        for c in pivot_in_fields:
            m = col_re.match(c)
            if not m:
                continue
            st = m.group("stem")
            if st not in stem_set:
                stem_set.add(st)
                stems.append(st)
            s_values.add(int(m.group("s")))
        # map Sxx -> dc_ratio (dontcare_pct)
        dc_list = sorted({100 - s for s in s_values})

        # discover missing (k, stem, dc)
        missing = []
        for row in pivot_in_rows:
            k_s = row.get("k", "").strip()
            if not k_s.isdigit():
                continue
            kk = int(k_s)
            for st in stems:
                for dc in dc_list:
                    s = dc_ratio_to_s_label(dc)
                    rt = _safe_float_str(row.get(f"{st}_rt_S{s}"))
                    rf = _safe_float_str(row.get(f"{st}_rf_S{s}"))
                    rs = _safe_float_str(row.get(f"{st}_rs_S{s}"))
                    if rt == "NA" or rf == "NA" or rs == "NA":
                        missing.append((st, dc, kk))

        # de-dup
        missing = sorted(set(missing))
        print(f"Resume: missing cells needing rerun = {len(missing)}")
        if not missing:
            patched = resume_pivot + ".patched.csv"
            with open(patched, "w", newline="", encoding="utf-8") as f:
                w = csv.DictWriter(f, fieldnames=pivot_in_fields)
                w.writeheader()
                w.writerows(pivot_in_rows)
            print(f"Resume: nothing to run. Patched pivot saved to {patched}")
            return

        # create rerun jobs using current BENCHMARKS mapping; reuse SEEDS[0] (pivot doesn't encode seed)
        seed = SEEDS[0] if SEEDS else 0
        rerun_jobs = []
        for st, dc, kk in missing:
            b = _stem_to_bench_path(st)
            if not b:
                print(f"[resume][skip] stem not in BENCHMARKS: {st}")
                continue
            rerun_jobs.append((b, seed, dc, kk))
        print(f"Resume: rerun jobs = {len(rerun_jobs)} (seed={seed})")

        job_results = []
        executor = ThreadPoolExecutor(max_workers=max_workers)
        try:
            future_map = {
                executor.submit(run_one_job, b, seed, dc, kk): (b, dc, kk)
                for b, seed, dc, kk in rerun_jobs
            }
            for fut in as_completed(future_map):
                try:
                    job_results.append(fut.result())
                except CancelledError:
                    continue
        except KeyboardInterrupt:
            interrupted = True
            print("\n[Interrupt] 正在停止所有 abc 子程序與未開始的 job …", flush=True)
            terminate_all_abc_children()
            if _PY39:
                executor.shutdown(wait=False, cancel_futures=True)
            else:
                executor.shutdown(wait=False)
        finally:
            if not interrupted:
                executor.shutdown(wait=True)

        # patch pivot rows in-memory using rerun results
        res_map = {(jr["circuit"], int(jr["dc_ratio"]), int(jr["kk"])): jr for jr in job_results}
        for row in pivot_in_rows:
            k_s = row.get("k", "").strip()
            if not k_s.isdigit():
                continue
            kk = int(k_s)
            for st in stems:
                for dc in dc_list:
                    key = (st, int(dc), kk)
                    if key not in res_map:
                        continue
                    jr = res_map[key]
                    s = dc_ratio_to_s_label(dc)
                    reset = jr.get("k_reset", "NA")
                    rt = jr.get("k_runtime_sec", "NA")
                    ff = jr.get("ff", "NA")
                    spec = jr.get("specified", "NA")
                    row[f"{st}_rt_S{s}"] = rt
                    row[f"{st}_rf_S{s}"] = r_f_from_reset_and_ff(reset, ff)
                    row[f"{st}_rs_S{s}"] = r_s_from_reset_and_specified(reset, spec)

        patched = resume_pivot + ".patched.csv"
        with open(patched, "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=pivot_in_fields)
            w.writeheader()
            w.writerows(pivot_in_rows)
        print(f"Resume: patched pivot saved to {patched}")

        # also dump rerun detail (only rerun jobs) for inspection
        rerun_detail = resume_pivot + ".rerun_detail.csv"
        rerun_wide = merge_job_results(job_results, headers, K_MIN, K_MAX)
        with open(rerun_detail, "w", newline="", encoding="utf-8") as f:
            dw = csv.DictWriter(f, fieldnames=headers)
            dw.writeheader()
            dw.writerows(rerun_wide)
        print(f"Resume: rerun detail saved to {rerun_detail}")

        if interrupted:
            sys.exit(130)
        return

    # --- Normal mode: run full grid ---
    job_results = []
    executor = ThreadPoolExecutor(max_workers=max_workers)
    try:
        future_map = {
            executor.submit(run_one_job, b, s, d, kk): (b, s, d, kk)
            for b, s, d, kk in jobs
        }
        for fut in as_completed(future_map):
            try:
                job_results.append(fut.result())
            except CancelledError:
                continue
    except KeyboardInterrupt:
        interrupted = True
        print("\n[Interrupt] 正在停止所有 abc 子程序與未開始的 job …", flush=True)
        terminate_all_abc_children()
        if _PY39:
            executor.shutdown(wait=False, cancel_futures=True)
        else:
            executor.shutdown(wait=False)
    finally:
        if not interrupted:
            executor.shutdown(wait=True)

    all_rows = merge_job_results(job_results, headers, K_MIN, K_MAX)
    write_outputs()
    if interrupted:
        print(
            f"[Interrupt] 已中止。已寫入 detail: {detail_csv}，pivot: {pivot_csv}（若有資料）",
            flush=True,
        )
        sys.exit(130)

    print(f"\nAll tasks finished. Detail: {detail_csv}")
    print(f"Pivot (k 為列、依電路區塊 rt→rf→rs): {pivot_csv}")


if __name__ == "__main__":
    main()
