"""
彙總 minr experiment 的 detail CSV，產生 *_stat.csv；並另存 *_barplot.csv（長條圖用：

detail 可含 runtime_cpu_sec / runtime_wall_sec；彙總時 runtime 仍以 **CPU** 為準（優先 runtime_cpu_sec，否則 runtime_sec），最後多一欄 **runtime_wall_sec** 平均（舊 CSV 無牆鐘欄則 NA）。
每列一電路、一 dc 條件；欄位為 benchmark、specified_ratio（目標，100−dc_ratio）、r_f、
specified_minus_r_f、actual_specified_pct（實際 specified register 數／ff 總數，百分比，與 valid runs 之 specified 平均一致）。
"""
import argparse
import csv
import os


def _to_int(s: str):
    if s is None:
        return None
    s = str(s).strip()
    if not s or s.upper() == "NA" or s.upper() == "N/A":
        return None
    try:
        return int(float(s))
    except Exception:
        return None


def _to_float(s: str):
    if s is None:
        return None
    s = str(s).strip()
    if not s or s.upper() == "NA" or s.upper() == "N/A":
        return None
    try:
        return float(s)
    except Exception:
        return None


def _to_float_percent(s: str):
    if s is None:
        return None
    s = str(s).strip()
    if not s or s.upper() == "NA" or s.upper() == "N/A":
        return None
    if s.endswith("%"):
        s = s[:-1].strip()
    try:
        return float(s)
    except Exception:
        return None


def _pct_col(row: dict, *keys: str):
    """讀 detail CSV 百分比欄位：優先新欄名，再嘗試舊欄名。"""
    for k in keys:
        v = _to_float_percent(row.get(k))
        if v is not None:
            return v
    return None


def _mean(vals):
    vals = [v for v in vals if v is not None]
    if not vals:
        return None
    return float(sum(vals)) / float(len(vals))


def _fmt_pct(x):
    return "NA" if x is None else f"{x:.2f}%"


def _fmt_num(x, nd=3):
    return "NA" if x is None else f"{x:.{nd}f}"


def _required_resets_from_ratio_of_ff(ratio_pct: float, ff: int):
    """
    從 r_f / k0_r_f（佔全部 ff 的百分比，log 常為 %.2f）還原「需要 reset 的個數」整數。
    """
    if ratio_pct is None or ff is None or ff <= 0:
        return None
    x = (ratio_pct / 100.0) * float(ff)
    r = int(round(x))
    return max(0, min(r, ff))


def _read_rows(csv_path: str):
    with open(csv_path, newline="", encoding="utf-8") as f:
        r = csv.DictReader(f)
        for row in r:
            yield row


def _ratio_pass(entries, key: str) -> str:
    """統計 pass / (pass+fail)，僅計有 pass 或 fail 的列。"""
    ok = 0
    tot = 0
    for e in entries:
        v = (e.get(key) or "").strip().lower()
        if v == "pass":
            ok += 1
            tot += 1
        elif v == "fail":
            tot += 1
    if tot == 0:
        return "NA"
    return f"{ok}/{tot}"


def main():
    ap = argparse.ArgumentParser(description="Aggregate minr exp detail CSV(s) into stat CSV.")
    ap.add_argument("inputs", nargs="+", help="One or more exp detail CSV files")
    ap.add_argument("-o", "--output", default="", help="Output stat CSV path (default: <first>_stat.csv)")
    args = ap.parse_args()

    groups = {}
    mismatch_by_dc: dict[int, dict[str, int]] = {}
    # (circuit, seed, dc_ratio) -> {"runtime": [...], "refine": [...]}
    pair_dc_runs: dict[tuple[str, int, int], dict[str, list]] = {}
    dc_seen: set[int] = set()

    for path in args.inputs:
        for row in _read_rows(path):
            circuit = (row.get("circuit") or "").strip()
            dc_ratio = _to_int(row.get("dc_ratio"))
            if circuit == "" or dc_ratio is None:
                continue
            dc_seen.add(dc_ratio)

            key = (circuit, dc_ratio)
            g = groups.setdefault(
                key,
                {
                    "ff": row.get("ff", ""),
                    "nodes": row.get("nodes", ""),
                    "inputs": "",
                    "outputs": "",
                    "total": 0,
                    "entries": [],
                    "opt_counts": {
                        "found_best": 0,
                        "timeout_with_best": 0,
                        "unsat_with_best": 0,
                        "stopped_with_best": 0,
                        "timeout_no_solution": 0,
                        "other": 0,
                    },
                },
            )
            g["total"] += 1

            opt_status = (row.get("opt_status") or "").strip()
            if opt_status in g["opt_counts"]:
                g["opt_counts"][opt_status] += 1
            elif opt_status:
                g["opt_counts"]["other"] += 1

            ff = _to_int(row.get("ff"))
            nodes = _to_int(row.get("nodes"))
            if ff is not None:
                g["ff"] = str(ff)
            if nodes is not None:
                g["nodes"] = str(nodes)
            ni = _to_int(row.get("inputs"))
            if ni is not None:
                g["inputs"] = str(ni)
            no = _to_int(row.get("outputs"))
            if no is not None:
                g["outputs"] = str(no)

            specified = _to_int(row.get("specified"))
            required_reset = _to_int(row.get("required_reset"))
            # 統計用 runtime 一律 CPU：新報表優先 runtime_cpu_sec，舊 CSV 僅有 runtime_sec
            runtime_sec = _to_float(row.get("runtime_cpu_sec"))
            if runtime_sec is None:
                runtime_sec = _to_float(row.get("runtime_sec"))
            runtime_wall_sec = _to_float(row.get("runtime_wall_sec"))
            refine_sec = _to_float(row.get("refine_sec"))
            seed_i = _to_int(row.get("seed"))
            k0_r_f_pct = _pct_col(row, "k0_r_f", "k0_reset_ratio")
            r_f_before_pct = _pct_col(row, "r_f_before_refine", "reset_ratio_before_refine")
            best_k = _to_int(row.get("best_k"))

            r_s = None
            if specified is not None and specified > 0 and required_reset is not None:
                r_s = 100.0 * (float(required_reset) / float(specified))

            if runtime_sec is None or r_s is None:
                continue

            # collect per (benchmark, seed, dc) for "all dc runtime<600" filtering
            if seed_i is not None:
                pd_key = (circuit, seed_i, dc_ratio)
                agg_pd = pair_dc_runs.setdefault(pd_key, {"runtime": [], "refine": []})
                agg_pd["runtime"].append(runtime_sec)
                if refine_sec is not None:
                    agg_pd["refine"].append(refine_sec)

            required_reset_k0 = None
            if k0_r_f_pct is not None and ff is not None:
                required_reset_k0 = _required_resets_from_ratio_of_ff(k0_r_f_pct, ff)

            required_reset_before_refine = None
            if r_f_before_pct is not None and ff is not None:
                required_reset_before_refine = _required_resets_from_ratio_of_ff(
                    r_f_before_pct, ff
                )

            k0_r_s = None
            if required_reset_k0 is not None and specified is not None and specified > 0:
                k0_r_s = 100.0 * (float(required_reset_k0) / float(specified))

            r_s_before_refine = None
            if required_reset_before_refine is not None and specified is not None and specified > 0:
                r_s_before_refine = 100.0 * (
                    float(required_reset_before_refine) / float(specified)
                )

            weak_pct = _to_float_percent(row.get("sim_reg_mismatch_weak"))
            strong_pct = _to_float_percent(row.get("sim_reg_mismatch_strong"))
            if weak_pct is not None and strong_pct is not None:
                agg = mismatch_by_dc.setdefault(dc_ratio, {"n_spec": 0, "n_weak": 0, "n_strong": 0})
                agg["n_spec"] += specified
                agg["n_weak"] += int(round(weak_pct / 100.0 * float(specified)))
                agg["n_strong"] += int(round(strong_pct / 100.0 * float(specified)))

            g["entries"].append(
                {
                    "specified": specified,
                    "required_reset": required_reset,
                    "r_s": r_s,
                    "k0_r_s": k0_r_s,
                    "required_reset_k0": required_reset_k0,
                    "r_s_before_refine": r_s_before_refine,
                    "required_reset_before_refine": required_reset_before_refine,
                    "best_k": best_k,
                    "runtime_sec": runtime_sec,
                    "runtime_wall_sec": runtime_wall_sec,
                    "refine_sec": refine_sec,
                    "r_f": _pct_col(row, "r_f", "reset_ratio"),
                    "k0_r_f": _pct_col(row, "k0_r_f", "k0_reset_ratio"),
                    "r_f_before_refine": _pct_col(row, "r_f_before_refine", "reset_ratio_before_refine"),
                    "spec_ro_in_cut": _to_float_percent(row.get("spec_ro_in_cut")),
                    "sim_reg_mismatch_weak": weak_pct,
                    "sim_reg_mismatch_strong": strong_pct,
                    "cut_verified": (row.get("cut_verified") or "").strip(),
                    "cec_verified": (row.get("cec_verified") or "").strip(),
                    "refine_mode": (row.get("refine_mode") or "").strip(),
                }
            )

    out_path = args.output
    if not out_path:
        base = args.inputs[0]
        root, ext = os.path.splitext(base)
        out_path = f"{root}_stat.csv"

    # 主欄位依使用者順序；其餘在後。僅均值，不輸出 std。
    headers = [
        "circuit",
        "dc_ratio",
        "inputs",
        "outputs",
        "nodes",
        "ff",
        "specified_ff",
        "r_s_k0",
        "r_s_before_refine",
        "r_s_after_refine",
        "best_k",
        "runtime_sec",
        "refine_sec",
        "required_reset_k0",
        "required_reset_before_refine",
        "required_reset_after_refine",
        "valid_runs",
        "total_runs",
        "opt_found_best",
        "opt_timeout_with_best",
        "opt_unsat_with_best",
        "opt_stopped_with_best",
        "opt_timeout_no_solution",
        "opt_other",
        "r_f",
        "k0_r_f",
        "r_f_before_refine",
        "spec_ro_in_cut",
        "sim_reg_mismatch_weak",
        "sim_reg_mismatch_strong",
        "refine_mode",
        "cut_verified",
        "cec_verified",
        "runtime_wall_sec",
    ]

    rows_out = []
    for (circuit, dc_ratio) in sorted(groups.keys(), key=lambda x: (x[0], x[1])):
        g = groups[(circuit, dc_ratio)]
        entries = g["entries"]

        specifieds = [e.get("specified") for e in entries]
        k0s = [e.get("k0_r_s") for e in entries]
        k0_rs = [e.get("required_reset_k0") for e in entries]
        rbfs = [e.get("r_s_before_refine") for e in entries]
        br_rs = [e.get("required_reset_before_refine") for e in entries]
        bks = [e.get("best_k") for e in entries]
        rafter = [e.get("r_s") for e in entries]
        resets = [e.get("required_reset") for e in entries]
        runtimes = [e.get("runtime_sec") for e in entries]
        walltimes = [e.get("runtime_wall_sec") for e in entries]
        refsecs = [e.get("refine_sec") for e in entries]
        rfs = [e.get("r_f") for e in entries]
        k0_rfs = [e.get("k0_r_f") for e in entries]
        rfbfs = [e.get("r_f_before_refine") for e in entries]
        spec_ros = [e.get("spec_ro_in_cut") for e in entries]
        sim_w = [e.get("sim_reg_mismatch_weak") for e in entries]
        sim_s = [e.get("sim_reg_mismatch_strong") for e in entries]

        refine_mode = "NA"
        for e in entries:
            v = (e.get("refine_mode") or "").strip()
            if v:
                refine_mode = v
                break

        rows_out.append(
            {
                "circuit": circuit,
                "dc_ratio": str(dc_ratio),
                "inputs": g.get("inputs") or "NA",
                "outputs": g.get("outputs") or "NA",
                "nodes": g.get("nodes") or "NA",
                "ff": g.get("ff") or "NA",
                "specified_ff": _fmt_num(_mean(specifieds), nd=3),
                "r_s_k0": _fmt_pct(_mean(k0s)),
                "r_s_before_refine": _fmt_pct(_mean(rbfs)),
                "r_s_after_refine": _fmt_pct(_mean(rafter)),
                "best_k": _fmt_num(_mean(bks), nd=3),
                "runtime_sec": _fmt_num(_mean(runtimes), nd=3),
                "refine_sec": _fmt_num(_mean(refsecs), nd=3),
                "required_reset_k0": _fmt_num(_mean(k0_rs), nd=3),
                "required_reset_before_refine": _fmt_num(_mean(br_rs), nd=3),
                "required_reset_after_refine": _fmt_num(_mean(resets), nd=3),
                "valid_runs": str(len(entries)),
                "total_runs": str(g["total"]),
                "opt_found_best": str(g["opt_counts"]["found_best"]),
                "opt_timeout_with_best": str(g["opt_counts"]["timeout_with_best"]),
                "opt_unsat_with_best": str(g["opt_counts"]["unsat_with_best"]),
                "opt_stopped_with_best": str(g["opt_counts"]["stopped_with_best"]),
                "opt_timeout_no_solution": str(g["opt_counts"]["timeout_no_solution"]),
                "opt_other": str(g["opt_counts"]["other"]),
                "r_f": _fmt_pct(_mean(rfs)),
                "k0_r_f": _fmt_pct(_mean(k0_rfs)),
                "r_f_before_refine": _fmt_pct(_mean(rfbfs)),
                "spec_ro_in_cut": _fmt_pct(_mean(spec_ros)),
                "sim_reg_mismatch_weak": _fmt_pct(_mean(sim_w)),
                "sim_reg_mismatch_strong": _fmt_pct(_mean(sim_s)),
                "refine_mode": refine_mode,
                "cut_verified": _ratio_pass(entries, "cut_verified"),
                "cec_verified": _ratio_pass(entries, "cec_verified"),
                "runtime_wall_sec": _fmt_num(_mean(walltimes), nd=3),
            }
        )

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=headers)
        w.writeheader()
        w.writerows(rows_out)

    print(f"Stat saved to {out_path}")

    # Filter benchmark-seed pairs that are fast (<600s) under every dc_ratio, then average by dc_ratio.
    # For each (circuit, seed) we require it appears in ALL dc_seen and all its runs have runtime_sec < 600.
    if dc_seen and pair_dc_runs:
        dc_list = sorted(dc_seen)
        pair_keys = sorted(set((c, s) for (c, s, _d) in pair_dc_runs.keys()))
        eligible_pairs = []
        for circuit, seed_i in pair_keys:
            ok = True
            for dc in dc_list:
                pd_key = (circuit, seed_i, dc)
                if pd_key not in pair_dc_runs:
                    ok = False
                    break
                rts = pair_dc_runs[pd_key]["runtime"]
                if not rts:
                    ok = False
                    break
                # "每個 dc 下 runtime 都小於 600": interpret as all valid runs under that dc must be <600
                if any(rt >= 600.0 for rt in rts):
                    ok = False
                    break
            if ok:
                eligible_pairs.append((circuit, seed_i))

        by_dc_rows = []
        for dc in dc_list:
            per_pair_runtime = []
            per_pair_refine = []
            for circuit, seed_i in eligible_pairs:
                pd_key = (circuit, seed_i, dc)
                rts = pair_dc_runs[pd_key]["runtime"]
                ref = pair_dc_runs[pd_key]["refine"]
                per_pair_runtime.append(_mean(rts))
                per_pair_refine.append(_mean(ref))
            avg_runtime = _mean(per_pair_runtime)
            avg_refine = _mean(per_pair_refine)
            avg_maxsat = None
            if avg_runtime is not None:
                avg_maxsat = avg_runtime - (avg_refine or 0.0)
            by_dc_rows.append(
                {
                    "dc_ratio": str(dc),
                    "specified_ratio": str(100 - int(dc)),
                    "n_benchmark_seed": str(len(eligible_pairs)),
                    "avg_runtime_sec": _fmt_num(avg_runtime, nd=3),
                    "avg_refine_sec": _fmt_num(avg_refine, nd=3),
                    "avg_maxsat_sec": _fmt_num(avg_maxsat, nd=3),
                }
            )

        root_noext, _ext = os.path.splitext(out_path)
        if root_noext.endswith("_stat"):
            fast_path = root_noext[:-5] + "_fast_pairs_runtime.csv"
        else:
            fast_path = root_noext + "_fast_pairs_runtime.csv"
        with open(fast_path, "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(
                f,
                fieldnames=[
                    "specified_ratio",
                    "dc_ratio",
                    "n_benchmark_seed",
                    "avg_runtime_sec",
                    "avg_refine_sec",
                    "avg_maxsat_sec",
                ],
            )
            w.writeheader()
            # specified_ratio 大到小
            w.writerows(sorted(by_dc_rows, key=lambda r: _to_int(r["specified_ratio"]) or -1, reverse=True))
        print(f"Fast-pairs runtime saved to {fast_path}")

    # barplot.csv：依電路分組，組內 dc_ratio 由小到大（目標 specified 由大到小）
    root_noext, _ext = os.path.splitext(out_path)
    if root_noext.endswith("_stat"):
        barplot_path = root_noext[:-5] + "_barplot.csv"
    else:
        barplot_path = root_noext + "_barplot.csv"
    barplot_header_row = [
        "benchmark",
        "specified_ratio",
        "r_f",
        "specified_minus_r_f",
        "actual_specified_pct",
    ]
    barplot_rows = []
    circuits_sorted = sorted(set(c for c, _ in groups.keys()))
    for circuit in circuits_sorted:
        dcs = sorted(d for c, d in groups.keys() if c == circuit)
        for dc_ratio in dcs:
            g = groups[(circuit, dc_ratio)]
            entries = g["entries"]
            rfs = [e.get("r_f") for e in entries]
            specs = [e.get("specified") for e in entries]
            rf_mean = _mean(rfs)
            spec_mean = _mean(specs)
            ff_int = _to_int(g.get("ff"))
            spec_ratio = 100 - int(dc_ratio)
            sr = str(spec_ratio)
            if (
                ff_int is not None
                and ff_int > 0
                and spec_mean is not None
            ):
                actual_pct = 100.0 * float(spec_mean) / float(ff_int)
                actual_str = f"{actual_pct:.2f}"
            else:
                actual_str = "NA"
            if rf_mean is None:
                barplot_rows.append([circuit, sr, "NA", "NA", actual_str])
            else:
                barplot_rows.append(
                    [
                        circuit,
                        sr,
                        f"{rf_mean:.2f}",
                        f"{float(spec_ratio) - rf_mean:.2f}",
                        actual_str,
                    ]
                )
    with open(barplot_path, "w", newline="", encoding="utf-8") as f:
        bw = csv.writer(f)
        bw.writerow(barplot_header_row)
        bw.writerows(barplot_rows)
    print(f"Barplot saved to {barplot_path}")

    if mismatch_by_dc:
        print()
        print(
            "[sim_reg_mismatch 依 dc_ratio 彙總] "
            "（每筆：weak/strong% × nSpec 四捨五入成整數後，同 dc_ratio 加總；"
            "分母為該組 nSpec 總和；僅含兩欄皆可解析的 valid run）"
        )
        for dc in sorted(mismatch_by_dc.keys()):
            a = mismatch_by_dc[dc]
            ns = a["n_spec"]
            nw = a["n_weak"]
            nst = a["n_strong"]
            n_rem = ns - nw - nst
            inv = 1.0 / ns if ns > 0 else 0.0
            p_rem = 100.0 * n_rem * inv
            pw = 100.0 * nw * inv
            ps = 100.0 * nst * inv
            print(f"  dc_ratio={dc}  (nSpec_total={ns})")
            print(
                f"    nSpec−weak−strong = {n_rem} ({p_rem:.2f}% of nSpec)"
            )
            print(f"    weak_violation    = {nw} ({pw:.2f}% of nSpec)")
            print(f"    strong_violation  = {nst} ({ps:.2f}% of nSpec)")


if __name__ == "__main__":
    main()
