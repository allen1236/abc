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
                    "inputs": "",
                    "outputs": "",
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
            ni = _to_int(row.get("inputs"))
            if ni is not None:
                g["inputs"] = str(ni)
            no = _to_int(row.get("outputs"))
            if no is not None:
                g["outputs"] = str(no)

            specified = _to_int(row.get("specified"))
            required_reset = _to_int(row.get("required_reset"))
            runtime_sec = _to_float(row.get("runtime_sec"))
            refine_sec = _to_float(row.get("refine_sec"))
            k0_r_f_pct = _pct_col(row, "k0_r_f", "k0_reset_ratio")
            r_f_before_pct = _pct_col(row, "r_f_before_refine", "reset_ratio_before_refine")
            best_k = _to_int(row.get("best_k"))

            r_s = None
            if specified is not None and specified > 0 and required_reset is not None:
                r_s = 100.0 * (float(required_reset) / float(specified))

            if runtime_sec is None or r_s is None:
                continue

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
                    "refine_sec": refine_sec,
                    "r_f": _pct_col(row, "r_f"),
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
            }
        )

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=headers)
        w.writeheader()
        w.writerows(rows_out)

    print(f"Stat saved to {out_path}")

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
