#!/usr/bin/env python3
"""
掃描 benchmarks/iscas89 與 benchmarks/itc99 內的 AIGER（.aig / .aag），
讀首列標頭取得 inputs / outputs，輸出 CSV。

列順序：先 iscas89（檔名排序），再 itc99（檔名排序）。

用法:
  python3 script/benchmark_io_csv.py
  python3 script/benchmark_io_csv.py -o script/exp/benchmark_io.csv
"""

from __future__ import annotations

import argparse
import csv
import os
import sys

# 專案根目錄（script/ 上一層）
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BENCH_ROOT = os.path.join(ROOT, "benchmarks")
SUITES = ("iscas89", "itc99")
EXTS = (".aig", ".aag")


def _read_aiger_header_io(path: str) -> tuple[int | None, int | None]:
    """回傳 (inputs, outputs)；無法解析則 (None, None)。標頭列：aig|aag M I L O A"""
    try:
        with open(path, "rb") as f:
            line = f.readline()
    except OSError:
        return None, None
    if not line:
        return None, None
    try:
        s = line.decode("ascii").strip()
    except UnicodeDecodeError:
        return None, None
    parts = s.split()
    if len(parts) < 6:
        return None, None
    kind = parts[0].lower()
    if kind not in ("aig", "aag"):
        return None, None
    try:
        _m = int(parts[1])
        n_inputs = int(parts[2])
        _l = int(parts[3])
        n_outputs = int(parts[4])
        _a = int(parts[5])
    except ValueError:
        return None, None
    return n_inputs, n_outputs


def _collect_rows(bench_root: str) -> list[dict[str, str | int]]:
    rows: list[dict[str, str | int]] = []
    for suite in SUITES:
        d = os.path.join(bench_root, suite)
        if not os.path.isdir(d):
            continue
        names: list[str] = []
        for ent in os.listdir(d):
            low = ent.lower()
            if not low.endswith(EXTS):
                continue
            p = os.path.join(d, ent)
            if not os.path.isfile(p):
                continue
            names.append(ent)
        for ent in sorted(names, key=str.lower):
            rel = f"{suite}/{ent}"
            full = os.path.join(d, ent)
            stem, _ext = os.path.splitext(ent)
            ni, no = _read_aiger_header_io(full)
            rows.append(
                {
                    "suite": suite,
                    "circuit": stem,
                    "path": rel,
                    "inputs": "NA" if ni is None else ni,
                    "outputs": "NA" if no is None else no,
                }
            )
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description="Export iscas89/itc99 AIGER I/O counts to CSV.")
    ap.add_argument(
        "-b",
        "--bench-root",
        default=DEFAULT_BENCH_ROOT,
        help=f"benchmarks 根目錄（預設: {DEFAULT_BENCH_ROOT}）",
    )
    ap.add_argument(
        "-o",
        "--output",
        default="",
        help="輸出 CSV 路徑；未給則寫入 stdout",
    )
    args = ap.parse_args()

    rows = _collect_rows(os.path.abspath(args.bench_root))
    fieldnames = ["suite", "circuit", "path", "inputs", "outputs"]

    if args.output:
        os.makedirs(os.path.dirname(os.path.abspath(args.output)) or ".", exist_ok=True)
        with open(args.output, "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=fieldnames)
            w.writeheader()
            w.writerows(rows)
        print(f"Wrote {len(rows)} rows to {args.output}", file=sys.stderr)
    else:
        w = csv.DictWriter(sys.stdout, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(rows)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
