import argparse
import csv
import os
import statistics


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


def _mean_std(vals):
    vals = [v for v in vals if v is not None]
    if not vals:
        return None, None
    if len(vals) == 1:
        return float(vals[0]), None
    return float(statistics.mean(vals)), float(statistics.stdev(vals))


def _fmt_pct(x):
    return "NA" if x is None else f"{x:.2f}%"


def _fmt_num(x, nd=3):
    return "NA" if x is None else f"{x:.{nd}f}"


def _required_resets_from_ratio_of_ff(ratio_pct: float, ff: int):
    """
    從 reset_ratio / k0_reset_ratio（佔全部 ff 的百分比，log 常為 %.2f）還原「需要 reset 的個數」整數，
    避免 (pct/100)*ff 的浮點誤差造成 k0_reduction 出現 ±0.01% 一類雜訊。
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


def main():
    ap = argparse.ArgumentParser(description="Aggregate minr exp detail CSV(s) into stat CSV.")
    ap.add_argument("inputs", nargs="+", help="One or more exp detail CSV files")
    ap.add_argument("-o", "--output", default="", help="Output stat CSV path (default: <first>_stat.csv)")
    args = ap.parse_args()

    # group[(circuit, dc_ratio)] = { info, total, entries[], opt_counts{} }
    groups = {}

    for path in args.inputs:
        for row in _read_rows(path):
            circuit = (row.get("circuit") or "").strip()
            dc_ratio = _to_int(row.get("dc_ratio"))
            if circuit == "" or dc_ratio is None:
                continue

            key = (circuit, dc_ratio)
            g = groups.setdefault(
                key,
                {
                    "ff": row.get("ff", ""),
                    "nodes": row.get("nodes", ""),
                    "total": 0,
                    "entries": [],
                    "opt_counts": {"found_best": 0, "timeout_with_best": 0, "timeout_no_solution": 0, "other": 0},
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

            specified = _to_int(row.get("specified"))
            required_reset = _to_int(row.get("required_reset"))
            runtime_sec = _to_float(row.get("runtime_sec"))
            refine_sec = _to_float(row.get("refine_sec"))
            k0_reset_ratio = _to_float_percent(row.get("k0_reset_ratio"))
            reset_ratio_before_pct = _to_float_percent(row.get("reset_ratio_before_refine"))
            best_k = _to_int(row.get("best_k"))

            # reduction (%) = 1 - required_reset / specified
            reduction = None
            if specified is not None and specified > 0 and required_reset is not None:
                reduction = 100.0 * (1.0 - (float(required_reset) / float(specified)))

            # valid run: must have runtime + required_reset + specified (so reduction defined)
            if runtime_sec is None or reduction is None:
                continue

            # k=0 / refine 前：從佔 ff 的百分比還原整數 required reset 數
            required_reset_k0 = None
            if k0_reset_ratio is not None and ff is not None:
                required_reset_k0 = _required_resets_from_ratio_of_ff(k0_reset_ratio, ff)

            required_reset_before_refine = None
            if reset_ratio_before_pct is not None and ff is not None:
                required_reset_before_refine = _required_resets_from_ratio_of_ff(
                    reset_ratio_before_pct, ff
                )

            k0_reduction = None
            if required_reset_k0 is not None and specified is not None and specified > 0:
                k0_reduction = 100.0 * (1.0 - (float(required_reset_k0) / float(specified)))

            reduction_before_refine = None
            if required_reset_before_refine is not None and specified is not None and specified > 0:
                reduction_before_refine = 100.0 * (
                    1.0 - (float(required_reset_before_refine) / float(specified))
                )

            g["entries"].append(
                {
                    "specified": specified,
                    "required_reset": required_reset,
                    "reduction": reduction,
                    "k0_reduction": k0_reduction,
                    "required_reset_k0": required_reset_k0,
                    "reduction_before_refine": reduction_before_refine,
                    "required_reset_before_refine": required_reset_before_refine,
                    "best_k": best_k,
                    "runtime_sec": runtime_sec,
                    "refine_sec": refine_sec,
                }
            )

    out_path = args.output
    if not out_path:
        base = args.inputs[0]
        root, ext = os.path.splitext(base)
        out_path = f"{root}_stat.csv"

    # Put all *_avg on the left, then all *_std on the right (same order).
    headers = [
        "circuit",
        "dc_ratio",
        "ff",
        "nodes",
        # avgs
        "specified_avg",
        "k0_reduction_avg",
        "required_reset_k0_avg",
        "reduction_before_refine_avg",
        "required_reset_before_refine_avg",
        "best_k_avg",
        "reduction_avg",
        "required_reset_avg",
        "runtime_sec_avg",
        "refine_sec_avg",
        # counts
        "valid_runs",
        "total_runs",
        "opt_found_best",
        "opt_timeout_with_best",
        "opt_timeout_no_solution",
        "opt_other",
        # stds (same order as avgs)
        "specified_std",
        "k0_reduction_std",
        "required_reset_k0_std",
        "reduction_before_refine_std",
        "required_reset_before_refine_std",
        "best_k_std",
        "reduction_std",
        "required_reset_std",
        "runtime_sec_std",
        "refine_sec_std",
    ]

    rows_out = []
    for (circuit, dc_ratio) in sorted(groups.keys(), key=lambda x: (x[0], x[1])):
        g = groups[(circuit, dc_ratio)]
        entries = g["entries"]

        specifieds = [e.get("specified") for e in entries]
        k0s = [e.get("k0_reduction") for e in entries]
        k0_rs = [e.get("required_reset_k0") for e in entries]
        rbfs = [e.get("reduction_before_refine") for e in entries]
        br_rs = [e.get("required_reset_before_refine") for e in entries]
        bks = [e.get("best_k") for e in entries]
        reds = [e.get("reduction") for e in entries]
        resets = [e.get("required_reset") for e in entries]
        runtimes = [e.get("runtime_sec") for e in entries]
        refsecs = [e.get("refine_sec") for e in entries]

        m, s = _mean_std(specifieds)
        m_k0, s_k0 = _mean_std(k0s)
        m_k0r, s_k0r = _mean_std(k0_rs)
        m_rbf, s_rbf = _mean_std(rbfs)
        m_brr, s_brr = _mean_std(br_rs)
        m_bk, s_bk = _mean_std(bks)
        m_red, s_red = _mean_std(reds)
        m_rst, s_rst = _mean_std(resets)
        m_rt, s_rt = _mean_std(runtimes)
        m_rf, s_rf = _mean_std(refsecs)

        rows_out.append(
            {
                "circuit": circuit,
                "dc_ratio": str(dc_ratio),
                "ff": g.get("ff", ""),
                "nodes": g.get("nodes", ""),
                # avgs
                "specified_avg": _fmt_num(m, nd=3),
                "k0_reduction_avg": _fmt_pct(m_k0),
                "required_reset_k0_avg": _fmt_num(m_k0r, nd=3),
                "reduction_before_refine_avg": _fmt_pct(m_rbf),
                "required_reset_before_refine_avg": _fmt_num(m_brr, nd=3),
                "best_k_avg": _fmt_num(m_bk, nd=3),
                "reduction_avg": _fmt_pct(m_red),
                "required_reset_avg": _fmt_num(m_rst, nd=3),
                "runtime_sec_avg": _fmt_num(m_rt, nd=3),
                "refine_sec_avg": _fmt_num(m_rf, nd=3),
                # counts
                "valid_runs": str(len(entries)),
                "total_runs": str(g["total"]),
                "opt_found_best": str(g["opt_counts"]["found_best"]),
                "opt_timeout_with_best": str(g["opt_counts"]["timeout_with_best"]),
                "opt_timeout_no_solution": str(g["opt_counts"]["timeout_no_solution"]),
                "opt_other": str(g["opt_counts"]["other"]),
                # stds
                "specified_std": _fmt_num(s, nd=3),
                "k0_reduction_std": _fmt_pct(s_k0),
                "required_reset_k0_std": _fmt_num(s_k0r, nd=3),
                "reduction_before_refine_std": _fmt_pct(s_rbf),
                "required_reset_before_refine_std": _fmt_num(s_brr, nd=3),
                "best_k_std": _fmt_num(s_bk, nd=3),
                "reduction_std": _fmt_pct(s_red),
                "required_reset_std": _fmt_num(s_rst, nd=3),
                "runtime_sec_std": _fmt_num(s_rt, nd=3),
                "refine_sec_std": _fmt_num(s_rf, nd=3),
            }
        )

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=headers)
        w.writeheader()
        w.writerows(rows_out)

    print(f"Stat saved to {out_path}")


if __name__ == "__main__":
    main()

