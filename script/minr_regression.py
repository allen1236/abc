#!/usr/bin/env python3
"""
minr 迴歸測試：多組 benchmark × 多個 -k × seed × dc_ratio，加上子集 -O 1；與 baseline 比對解品質與耗時。

- 固定 -k：多個 k 值（預設 0,1,2,4；s5378 僅 0,1,2 以控制耗時）。
- -O 1：僅在較小、數秒內可完成的電路上跑（itc99 b01/b02/b06/b08 + iscas89 s27/s298），
  並以較短的 -t 預算限制單次指令總時間。

與 baseline 比對時，會分兩區塊分別彙報「相對 baseline 的時間差」：固定 -k 一區、-O 1 一區（不互相比較兩種模式）。

用法:
  python3 script/minr_regression.py              # 跑實驗並與 baseline 比對（預設）
  python3 script/minr_regression.py --update-baseline   # 重寫 script/minr_regression_baseline.json
  python3 script/minr_regression.py --dry-run      # 只印將執行的 abc 指令

環境變數:
  ABC  覆寫 abc 二進位路徑（預設 repo 根目錄的 abc）

結束碼: 0 通過、1 有失敗、2 baseline 不存在且未指定 --update-baseline

變更 key 格式後須重新 --update-baseline。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from datetime import datetime, timezone

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT_DIR = os.path.join(ROOT_DIR, "script")
BENCHMARK_DIR = os.path.join(ROOT_DIR, "benchmarks")
BASELINE_PATH = os.path.join(SCRIPT_DIR, "minr_regression_baseline.json")
RESULTS_DIR = os.path.join(SCRIPT_DIR, "regression_results")

# 與 parallel 類似：含快/慢 itc99 與多個 iscas89
BENCHMARKS = [
    "itc99/b01.aig",
    "itc99/b02.aig",
    "itc99/b06.aig",
    "itc99/b08.aig",
    "itc99/b03.aig",
    "itc99/b04.aig",
    "iscas89/s27.aig",
    "iscas89/s298.aig",
    "iscas89/s1196.aig",
    "iscas89/s1238.aig",
    "iscas89/s1423.aig",
    "iscas89/s5378.aig",
]

# 預設多組 -k；最大電路略縮小 k 範圍以免單 job 過長
K_VALUES_DEFAULT = [0, 1, 2, 4]
K_VALUES_LARGE = [0, 1, 2]  # s5378.aig

LARGE_BENCH_SUFFIX = "s5378.aig"


def k_values_for_bench(bench_rel: str) -> list[int]:
    if bench_rel.endswith(LARGE_BENCH_SUFFIX):
        return list(K_VALUES_LARGE)
    return list(K_VALUES_DEFAULT)

# 僅用於 -O 1：數秒～十餘秒內可跑完的子集
O1_BENCHMARKS = [
    "itc99/b01.aig",
    "itc99/b02.aig",
    "itc99/b06.aig",
    "itc99/b08.aig",
    "iscas89/s27.aig",
    "iscas89/s298.aig",
]

SEEDS = [0, 1]
DC_RATIO = [0, 25, 50, 75]
RANDOM_SIM_CYCLE = 500
REFINE_MODE = 1
REFINE_BIND_DC = True
REFINE_CONF_LIMIT = 10000
REFINE_CORE_ONLY = False
TOTAL_TIMEOUT = 300  # 單次固定 -k 求解總上限（秒）
O1_TOTAL_TIMEOUT = 25  # -O 1 單次總預算（秒），控制 sweep 在數秒級電路上可結束
SUBPROCESS_TIMEOUT = 1200

PATTERNS = {
    "inputs": r"inputs\s*=\s*(\d+)",
    "outputs": r"outputs\s*=\s*(\d+)",
    "ff": r"ff\s*=\s*(\d+)",
    "nodes": r"nodes\s*=\s*(\d+)",
    "specified_regs": r"specified_regs\s*=\s*(\d+)",
    "required_reset": r"required_reset\s*=\s*(\d+)",
    "r_f": r"r_f\s*=\s*([\d.]+%?)",
    "r_s": r"r_s\s*=\s*([\d.]+%?|N/A)",
    "r_f_before_refine": r"r_f_before_refine\s*=\s*([\d.]+%?|N/A)",
    "r_s_before_refine": r"r_s_before_refine\s*=\s*([\d.]+%?|N/A)",
    "runtime_sec": r"runtime_sec\s*=\s*([\d.]+)",
    "cut_verified": r"cut_verified\s*=\s*(\w+)",
    "cec_verified": r"cec_verified\s*=\s*(\w+)",
    "refine_sec": r"refine_sec\s*=\s*([\d.]+)",
}


def _parse_log(content: str) -> dict[str, str]:
    out = {}
    for key, pat in PATTERNS.items():
        m = re.search(pat, content)
        if m:
            out[key] = m.group(1)
    return out


def _to_float(s: str | None):
    if s is None:
        return None
    s = str(s).strip()
    if not s or s.upper() in ("NA", "N/A"):
        return None
    try:
        return float(s)
    except ValueError:
        return None


def _to_float_pct(s: str | None):
    if s is None:
        return None
    s = str(s).strip()
    if not s or s.upper() in ("NA", "N/A"):
        return None
    if s.endswith("%"):
        s = s[:-1].strip()
    try:
        return float(s)
    except ValueError:
        return None


def _to_int(s: str | None):
    if s is None:
        return None
    s = str(s).strip()
    if not s or s.upper() in ("NA", "N/A"):
        return None
    try:
        return int(float(s))
    except ValueError:
        return None


def _case_key(mode: str, k: int | None, circuit: str, seed: int, dc: int) -> str:
    if mode == "O1":
        return f"O1|{circuit}|{seed}|{dc}"
    assert k is not None
    return f"k{k}|{circuit}|{seed}|{dc}"


def _build_abc_cmd(
    bench_rel: str,
    seed: int,
    dc_pct: int,
    log_path: str,
    *,
    k: int | None = None,
    optimize1: bool = False,
) -> list[str]:
    src = os.path.join(BENCHMARK_DIR, bench_rel)
    dc_arg = f"-D {dc_pct}" if dc_pct > 0 else ""
    refine_arg = f"-x {REFINE_MODE}" if REFINE_MODE > 0 else ""
    bind_arg = " -X" if (REFINE_MODE > 0 and REFINE_BIND_DC) else ""
    conf_arg = f" -c {REFINE_CONF_LIMIT}" if REFINE_MODE > 0 else ""
    core_arg = " -C" if (REFINE_MODE > 0 and REFINE_CORE_ONLY) else ""
    abc_bin = os.environ.get("ABC", os.path.join(ROOT_DIR, "abc"))

    if optimize1:
        to_arg = f"-t {O1_TOTAL_TIMEOUT}" if O1_TOTAL_TIMEOUT > 0 else ""
        inner = (
            f"read_aiger {src}; &get; &minr -O 1 -r {seed} -R {RANDOM_SIM_CYCLE} "
            f"{dc_arg} {refine_arg}{bind_arg}{conf_arg}{core_arg} {to_arg} -o {log_path}"
        )
    else:
        assert k is not None
        to_arg = f"-t {TOTAL_TIMEOUT}" if TOTAL_TIMEOUT > 0 else ""
        inner = (
            f"read_aiger {src}; &get; &minr -k {k} -r {seed} -R {RANDOM_SIM_CYCLE} "
            f"{dc_arg} {refine_arg}{bind_arg}{conf_arg}{core_arg} {to_arg} -o {log_path}"
        )
    return [abc_bin, "-c", inner]


def _run_one(
    bench_rel: str, seed: int, dc_pct: int, *, k: int | None = None, optimize1: bool = False
) -> tuple[int, dict, str]:
    stem = os.path.splitext(os.path.basename(bench_rel))[0]
    if optimize1:
        tag = f"{stem}_O1_r{seed}_D{dc_pct}_"
    else:
        tag = f"{stem}_k{k}_r{seed}_D{dc_pct}_"
    fd, log_path = tempfile.mkstemp(prefix=f"minr_reg_{tag}", suffix=".log")
    os.close(fd)
    try:
        cmd = _build_abc_cmd(bench_rel, seed, dc_pct, log_path, k=k, optimize1=optimize1)
        r = subprocess.run(
            cmd,
            cwd=ROOT_DIR,
            capture_output=True,
            text=True,
            timeout=SUBPROCESS_TIMEOUT,
        )
        parsed = {}
        if os.path.isfile(log_path):
            with open(log_path, encoding="utf-8", errors="replace") as f:
                parsed = _parse_log(f.read())
        return r.returncode, parsed, log_path
    finally:
        try:
            os.remove(log_path)
        except OSError:
            pass


def _metrics_from_parsed(parsed: dict[str, str]) -> dict:
    return {
        "required_reset": _to_int(parsed.get("required_reset")),
        "specified_regs": _to_int(parsed.get("specified_regs")),
        "ff": _to_int(parsed.get("ff")),
        "r_s": _to_float_pct(parsed.get("r_s")),
        "r_f": _to_float_pct(parsed.get("r_f")),
        "r_s_before_refine": _to_float_pct(parsed.get("r_s_before_refine")),
        "r_f_before_refine": _to_float_pct(parsed.get("r_f_before_refine")),
        "runtime_sec": _to_float(parsed.get("runtime_sec")),
        "refine_sec": _to_float(parsed.get("refine_sec")),
        "cut_verified": (parsed.get("cut_verified") or "").strip() or None,
        "cec_verified": (parsed.get("cec_verified") or "").strip() or None,
        "inputs": _to_int(parsed.get("inputs")),
        "outputs": _to_int(parsed.get("outputs")),
        "nodes": _to_int(parsed.get("nodes")),
    }


def _float_close(a: float | None, b: float | None, eps: float) -> bool:
    if a is None and b is None:
        return True
    if a is None or b is None:
        return False
    return abs(a - b) <= eps


def _compare_metrics(cur: dict, base: dict, key: str) -> list[str]:
    errs = []
    strict_int = ("required_reset", "specified_regs", "ff", "inputs", "outputs", "nodes")
    for f in strict_int:
        if cur.get(f) != base.get(f):
            errs.append(f"{key}: {f} cur={cur.get(f)} baseline={base.get(f)}")
    for f in ("cut_verified", "cec_verified"):
        if cur.get(f) != base.get(f):
            errs.append(f"{key}: {f} cur={cur.get(f)} baseline={base.get(f)}")
    if not _float_close(cur.get("r_s"), base.get("r_s"), 0.02):
        errs.append(f"{key}: r_s cur={cur.get('r_s')} baseline={base.get('r_s')}")
    if not _float_close(cur.get("r_f"), base.get("r_f"), 0.02):
        errs.append(f"{key}: r_f cur={cur.get('r_f')} baseline={base.get('r_f')}")
    if not _float_close(cur.get("r_s_before_refine"), base.get("r_s_before_refine"), 0.02):
        errs.append(
            f"{key}: r_s_before_refine cur={cur.get('r_s_before_refine')} baseline={base.get('r_s_before_refine')}"
        )
    if not _float_close(cur.get("r_f_before_refine"), base.get("r_f_before_refine"), 0.02):
        errs.append(
            f"{key}: r_f_before_refine cur={cur.get('r_f_before_refine')} baseline={base.get('r_f_before_refine')}"
        )
    return errs


def _runtime_vs_baseline_line(key: str, cur: dict, base: dict) -> str | None:
    """每一筆都回報 runtime/refine 與 baseline 差異（有數字才輸出）。"""
    parts = []
    for label, fld in (("runtime_sec", "runtime_sec"), ("refine_sec", "refine_sec")):
        c = cur.get(fld)
        b = base.get(fld)
        if c is None and b is None:
            continue
        if b is None:
            parts.append(f"{label} cur={c}")
        elif c is None:
            parts.append(f"{label} cur=None baseline={b}")
        else:
            d = c - b
            parts.append(f"{label} cur={c:.6f} baseline={b:.6f} delta={d:+.6f}")
    if not parts:
        return None
    return f"{key}: " + "; ".join(parts)


def _partition_runtime_stats(
    results: list[dict],
    base_by_key: dict[str, dict],
    mode: str,
) -> tuple[list[float], list[float], float, float, float, float]:
    """回傳 (runtime_deltas, refine_deltas, sum_base_rt, sum_cur_rt, sum_base_rf, sum_cur_rf)。"""
    rt_deltas: list[float] = []
    rf_deltas: list[float] = []
    sum_b_rt = sum_c_rt = 0.0
    sum_b_rf = sum_c_rf = 0.0
    for r in results:
        if r["mode"] != mode:
            continue
        key = r["key"]
        if key not in base_by_key:
            continue
        cur = r["metrics"]
        base = base_by_key[key]
        c_rt, b_rt = cur.get("runtime_sec"), base.get("runtime_sec")
        if c_rt is not None and b_rt is not None:
            rt_deltas.append(c_rt - b_rt)
            sum_b_rt += b_rt
            sum_c_rt += c_rt
        c_rf, b_rf = cur.get("refine_sec"), base.get("refine_sec")
        if c_rf is not None and b_rf is not None:
            rf_deltas.append(c_rf - b_rf)
            sum_b_rf += b_rf
            sum_c_rf += c_rf
    return rt_deltas, rf_deltas, sum_b_rt, sum_c_rt, sum_b_rf, sum_c_rf


def _lines_partition_vs_baseline(
    title: str,
    rt_deltas: list[float],
    rf_deltas: list[float],
    sum_b_rt: float,
    sum_c_rt: float,
    sum_b_rf: float,
    sum_c_rf: float,
) -> list[str]:
    lines = [title]
    n_rt = len(rt_deltas)
    n_rf = len(rf_deltas)
    if n_rt == 0:
        lines.append("  runtime_sec: （無可比對筆數，或 baseline／本次缺值）")
    else:
        sd = sum(rt_deltas)
        lines.append(
            f"  runtime_sec: {n_rt} 筆, baseline 加總={sum_b_rt:.6f}s, 本次加總={sum_c_rt:.6f}s, "
            f"差值(本次−baseline)={sd:+.6f}s, 平均每筆差={sd / n_rt:+.6f}s"
        )
        lines.append(f"               單筆差 min={min(rt_deltas):+.6f}s, max={max(rt_deltas):+.6f}s")
    if n_rf == 0:
        lines.append("  refine_sec:  （無可比對筆數或缺值）")
    else:
        sdf = sum(rf_deltas)
        lines.append(
            f"  refine_sec:  {n_rf} 筆, baseline 加總={sum_b_rf:.6f}s, 本次加總={sum_c_rf:.6f}s, "
            f"差值={sdf:+.6f}s, 平均每筆差={sdf / n_rf:+.6f}s"
        )
        lines.append(f"               單筆差 min={min(rf_deltas):+.6f}s, max={max(rf_deltas):+.6f}s")
    return lines


def _summarize_runtime_by_partition_vs_baseline(
    results: list[dict],
    base_by_key: dict[str, dict],
) -> list[str]:
    """固定 -k 與 -O 1 兩區塊，分別彙總相對 baseline 的 runtime / refine 差（不互相比較）。"""
    out: list[str] = []
    rt_k, rf_k, sbk_rt, sck_rt, sbk_rf, sck_rf = _partition_runtime_stats(results, base_by_key, "k")
    out.extend(
        _lines_partition_vs_baseline(
            "=== 固定 -k 區塊：相對 baseline 的時間差（彙總）===",
            rt_k,
            rf_k,
            sbk_rt,
            sck_rt,
            sbk_rf,
            sck_rf,
        )
    )
    out.append("")
    rt_o, rf_o, sbo_rt, sco_rt, sbo_rf, sco_rf = _partition_runtime_stats(results, base_by_key, "O1")
    out.extend(
        _lines_partition_vs_baseline(
            "=== -O 1 區塊：相對 baseline 的時間差（彙總）===",
            rt_o,
            rf_o,
            sbo_rt,
            sco_rt,
            sbo_rf,
            sco_rf,
        )
    )
    return out


def _summarize_runtime_by_partition_current_only(results: list[dict]) -> list[str]:
    """--update-baseline 時僅印本次各區塊加總／平均（尚無 baseline 差異）。"""
    lines: list[str] = []
    for mode, title in (
        ("k", "=== 固定 -k 區塊：本次 run（尚無 baseline 差異）==="),
        ("O1", "=== -O 1 區塊：本次 run（尚無 baseline 差異）==="),
    ):
        lines.append(title)
        rts: list[float] = []
        rfs: list[float] = []
        for r in results:
            if r["mode"] != mode:
                continue
            m = r["metrics"]
            if m.get("runtime_sec") is not None:
                rts.append(m["runtime_sec"])
            if m.get("refine_sec") is not None:
                rfs.append(m["refine_sec"])
        if rts:
            lines.append(
                f"  runtime_sec: {len(rts)} 筆, 加總={sum(rts):.6f}s, 平均={sum(rts) / len(rts):.6f}s"
            )
        else:
            lines.append("  runtime_sec: （無）")
        if rfs:
            lines.append(
                f"  refine_sec:  {len(rfs)} 筆, 加總={sum(rfs):.6f}s, 平均={sum(rfs) / len(rfs):.6f}s"
            )
        else:
            lines.append("  refine_sec:  （無）")
        lines.append("")
    return lines


def main() -> int:
    ap = argparse.ArgumentParser(description="minr regression vs baseline (multi -k + -O 1 subset)")
    ap.add_argument("--update-baseline", action="store_true", help="寫入 minr_regression_baseline.json")
    ap.add_argument("--dry-run", action="store_true", help="只列出指令不執行")
    args = ap.parse_args()

    abc_bin = os.environ.get("ABC", os.path.join(ROOT_DIR, "abc"))
    if not args.dry_run and not os.path.isfile(abc_bin):
        print(f"ERROR: abc not found: {abc_bin}", file=sys.stderr)
        return 1

    benches = []
    for b in BENCHMARKS:
        p = os.path.join(BENCHMARK_DIR, b)
        if os.path.isfile(p):
            benches.append(b)
        else:
            print(f"WARNING: benchmark file missing, skip: {b}", file=sys.stderr)
    if not benches:
        print("ERROR: no benchmark .aig files found under benchmarks/", file=sys.stderr)
        return 1

    o1_benches = [b for b in O1_BENCHMARKS if b in benches]

    jobs: list[tuple] = []
    for b in benches:
        for k in k_values_for_bench(b):
            for s in SEEDS:
                for d in DC_RATIO:
                    jobs.append(("k", b, k, s, d))
    for b in o1_benches:
        for s in SEEDS:
            for d in DC_RATIO:
                jobs.append(("O1", b, None, s, d))

    n_k = sum(1 for j in jobs if j[0] == "k")
    n_o1 = sum(1 for j in jobs if j[0] == "O1")
    print(
        f"minr regression: {len(jobs)} jobs ({n_k} fixed-k + {n_o1} -O 1), "
        f"k in {K_VALUES_DEFAULT} (s5378: {K_VALUES_LARGE}), R={RANDOM_SIM_CYCLE}, refine={REFINE_MODE}"
    )

    if args.dry_run:
        print("  sample fixed-k:")
        print("   ", subprocess.list2cmdline(_build_abc_cmd(benches[0], 0, 0, "/tmp/x.log", k=1)))
        print("  sample -O 1:")
        if o1_benches:
            print("   ", subprocess.list2cmdline(_build_abc_cmd(o1_benches[0], 0, 0, "/tmp/y.log", optimize1=True)))
        print("  ...")
        return 0

    results = []
    failures = []
    for kind, bench, k, seed, dc in jobs:
        circuit = os.path.splitext(os.path.basename(bench))[0]
        if kind == "O1":
            rc, parsed, _ = _run_one(bench, seed, dc, optimize1=True)
            key = _case_key("O1", None, circuit, seed, dc)
            m = _metrics_from_parsed(parsed)
            results.append(
                {
                    "key": key,
                    "bench": bench,
                    "circuit": circuit,
                    "seed": seed,
                    "dc_ratio": dc,
                    "mode": "O1",
                    "k": None,
                    "abc_returncode": rc,
                    "metrics": m,
                    "parsed_raw": parsed,
                }
            )
        else:
            rc, parsed, _ = _run_one(bench, seed, dc, k=k)
            key = _case_key("k", k, circuit, seed, dc)
            m = _metrics_from_parsed(parsed)
            results.append(
                {
                    "key": key,
                    "bench": bench,
                    "circuit": circuit,
                    "seed": seed,
                    "dc_ratio": dc,
                    "mode": "k",
                    "k": k,
                    "abc_returncode": rc,
                    "metrics": m,
                    "parsed_raw": parsed,
                }
            )
        if rc != 0:
            failures.append(f"{key}: abc exit {rc}")
        if not parsed:
            failures.append(f"{key}: empty parse (no report?)")

    meta = {
        "k_values_default": K_VALUES_DEFAULT,
        "k_values_large_s5378": K_VALUES_LARGE,
        "o1_benchmarks": o1_benches,
        "o1_total_timeout_sec": O1_TOTAL_TIMEOUT,
        "R": RANDOM_SIM_CYCLE,
        "refine_mode": REFINE_MODE,
        "refine_bind_dc": REFINE_BIND_DC,
        "refine_conf_limit": REFINE_CONF_LIMIT,
        "total_timeout_sec": TOTAL_TIMEOUT,
        "benchmarks": benches,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "abc_path": os.path.abspath(abc_bin),
    }

    os.makedirs(RESULTS_DIR, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    run_json = os.path.join(RESULTS_DIR, f"run_{stamp}.json")
    with open(run_json, "w", encoding="utf-8") as f:
        json.dump({"meta": meta, "cases": results}, f, indent=2, ensure_ascii=False)
    print(f"Wrote {run_json}")

    if args.update_baseline:
        slim = {
            "meta": meta,
            "cases": [
                {
                    "key": r["key"],
                    "bench": r["bench"],
                    "circuit": r["circuit"],
                    "seed": r["seed"],
                    "dc_ratio": r["dc_ratio"],
                    "mode": r["mode"],
                    "k": r["k"],
                    "metrics": r["metrics"],
                }
                for r in results
            ],
        }
        with open(BASELINE_PATH, "w", encoding="utf-8") as f:
            json.dump(slim, f, indent=2, ensure_ascii=False)
        print(f"Updated baseline {BASELINE_PATH}")
        if failures:
            print("WARNING: some runs failed but baseline was still written.", file=sys.stderr)
        for ln in _summarize_runtime_by_partition_current_only(results):
            print(ln)
        return 1 if failures else 0

    if not os.path.isfile(BASELINE_PATH):
        print(f"ERROR: no baseline at {BASELINE_PATH}; run with --update-baseline first", file=sys.stderr)
        return 2

    with open(BASELINE_PATH, encoding="utf-8") as f:
        baseline = json.load(f)
    base_by_key = {c["key"]: c["metrics"] for c in baseline.get("cases", [])}

    compare_errs = []
    runtime_lines_k: list[str] = []
    runtime_lines_o1: list[str] = []
    for r in results:
        k = r["key"]
        if k not in base_by_key:
            compare_errs.append(f"{k}: missing in baseline")
            continue
        b = base_by_key[k]
        cur = r["metrics"]
        compare_errs.extend(_compare_metrics(cur, b, k))
        ln = _runtime_vs_baseline_line(k, cur, b)
        if ln:
            if r["mode"] == "O1":
                runtime_lines_o1.append(ln)
            else:
                runtime_lines_k.append(ln)

    partition_summary = _summarize_runtime_by_partition_vs_baseline(results, base_by_key)

    summary_path = os.path.join(RESULTS_DIR, f"summary_{stamp}.txt")
    lines = [
        f"run: {run_json}",
        f"baseline: {BASELINE_PATH}",
        f"compare_errors: {len(compare_errs)}",
        "",
    ]
    if compare_errs:
        lines.extend(compare_errs)
        lines.append("")
    lines.extend(partition_summary)
    lines.append("")
    lines.append("=== 固定 -k 區塊：每筆 runtime / refine vs baseline ===")
    if runtime_lines_k:
        lines.extend(runtime_lines_k)
    else:
        lines.append("(無)")
    lines.append("")
    lines.append("=== -O 1 區塊：每筆 runtime / refine vs baseline ===")
    if runtime_lines_o1:
        lines.extend(runtime_lines_o1)
    else:
        lines.append("(無)")

    with open(summary_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"Wrote {summary_path}")

    if failures:
        for x in failures:
            print(f"FAIL {x}", file=sys.stderr)
    if compare_errs:
        for x in compare_errs:
            print(f"MISMATCH {x}", file=sys.stderr)
        return 1
    if failures:
        return 1
    print("OK: all cases match baseline (solution metrics).")
    for ln in partition_summary:
        print(ln)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
