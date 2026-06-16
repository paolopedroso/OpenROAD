#!/usr/bin/env python3
"""rta_cut_analysis.py -- Task 4 (category-error) analysis.

Reads:
  --diag <csv>      The RTA_DIAG_DUMP_NETS CSV (per-hyperedge static vs
                    seq slack, driver, loads).
  --static <part>   .part file from a static-slack (mode C) run.
  --rta    <part>   .part file from an RTA Pan (mode D) run.

Produces a table:
  - Classifies each hyperedge by slack quadrant:
      consistent_safe      (static high, seq high)
      consistent_critical  (static low,  seq low)
      retiming_relaxable   (static low,  seq high)  <- the H4 suspects
      retiming_tightened   (static high, seq low)   <- usually empty
  - For each quadrant: count of hyperedges, and for each partition mode
    the count that are CUT (driver and at least one load in different
    partitions).
  - Prints whether RTA cuts the "retiming_relaxable" nets more often
    than static does (the H4 prediction). Also prints the largest |diff|
    nets for spot-checking.

Slack quadrant thresholds: medians of each axis (so each axis is split
50/50 over the population). Robust to scale.

Usage:
  python3 rta_cut_analysis.py \
      --diag results/gcd_diag.csv \
      --static results/gcd_static_s1.part \
      --rta    results/gcd_rta_s1.part
"""

import argparse
import csv
import statistics
import sys


def parse_part_file(path):
    """Read a .part file: <name>  <block_id>. Returns dict name -> int."""
    out = {}
    with open(path) as f:
        for line in f:
            toks = line.split()
            if len(toks) < 2:
                continue
            name = " ".join(toks[:-1])
            try:
                bid = int(toks[-1])
            except ValueError:
                continue
            out[name] = bid
    return out


def is_net_cut(driver, loads, part):
    """A net is CUT if any (driver, load) pair lies in different partitions."""
    if driver not in part:
        return False  # driver is e.g. a port -> treat as not cut
    d = part[driver]
    for load in loads:
        if load in part and part[load] != d:
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--diag", required=True)
    ap.add_argument("--static", required=True, dest="static_part")
    ap.add_argument("--rta", required=True)
    args = ap.parse_args()

    static_part = parse_part_file(args.static_part)
    rta_part = parse_part_file(args.rta)
    print(f"static .part: {len(static_part)} entries")
    print(f"rta    .part: {len(rta_part)} entries")

    rows = []
    with open(args.diag) as f:
        for r in csv.DictReader(f):
            try:
                row = {
                    "hyperedge_id": int(r["hyperedge_id"]),
                    "static": float(r["static_norm_slack"]),
                    "raw_seq": float(r["raw_seq_slack_s"]),
                    "rta": float(r["rta_norm_slack"]),
                    "cost": float(r["timing_cost_factor"]),
                    "driver": r["driver"],
                    "n_loads": int(r["n_loads"]),
                    "loads": [
                        x for x in r["loads_pipe_sep"].split("|") if x
                    ],
                }
                rows.append(row)
            except (KeyError, ValueError):
                continue

    if not rows:
        print("ERROR: no parseable rows in diag CSV", file=sys.stderr)
        sys.exit(1)

    # Compute medians to split the population 50/50 per axis.
    statics = [r["static"] for r in rows]
    rtas = [r["rta"] for r in rows]
    static_med = statistics.median(statics)
    rta_med = statistics.median(rtas)
    print(
        f"\nslack medians: static={static_med:.4f}  rta={rta_med:.4f}"
    )

    quadrants = {
        "consistent_safe": [],       # static high, rta high
        "consistent_critical": [],   # static low,  rta low
        "retiming_relaxable": [],    # static low,  rta high  <- H4 suspects
        "retiming_tightened": [],    # static high, rta low
    }
    for r in rows:
        s_high = r["static"] >= static_med
        r_high = r["rta"] >= rta_med
        if s_high and r_high:
            q = "consistent_safe"
        elif (not s_high) and (not r_high):
            q = "consistent_critical"
        elif (not s_high) and r_high:
            q = "retiming_relaxable"
        else:
            q = "retiming_tightened"
        r["quadrant"] = q
        quadrants[q].append(r)

    print(
        f"\n{'quadrant':<22}  {'n':>5}  "
        f"{'cut_in_static':>14}  {'cut_in_rta':>11}  {'delta':>7}"
    )
    print("-" * 70)
    for qname, rs in quadrants.items():
        n = len(rs)
        cs = sum(
            1
            for r in rs
            if is_net_cut(r["driver"], r["loads"], static_part)
        )
        cr = sum(
            1
            for r in rs
            if is_net_cut(r["driver"], r["loads"], rta_part)
        )
        print(
            f"{qname:<22}  {n:>5}  {cs:>14}  {cr:>11}  {cr-cs:>+7}"
        )

    print(
        f"\nH4 prediction: in 'retiming_relaxable', RTA cuts MORE than "
        "static -- because RTA's high seq slack convinces the partitioner "
        "these nets are safe to cut, even though they remain statically "
        "critical (no retiming actually runs)."
    )
    rr = quadrants["retiming_relaxable"]
    cs_rr = sum(
        1 for r in rr if is_net_cut(r["driver"], r["loads"], static_part)
    )
    cr_rr = sum(
        1 for r in rr if is_net_cut(r["driver"], r["loads"], rta_part)
    )
    print(
        f"\nretiming_relaxable cut counts:  static={cs_rr}  rta={cr_rr}"
        f"  delta={cr_rr - cs_rr:+d}"
    )
    if cr_rr > cs_rr:
        print(
            "=> Consistent with H4: RTA cuts these nets MORE than static."
        )
    elif cr_rr < cs_rr:
        print(
            "=> Inverts H4: RTA actually cuts these LESS than static."
        )
    else:
        print(
            "=> Neutral: RTA and static cut equally many of these nets."
        )

    # Spot check: 10 nets with largest |seq - static| where seq > static
    diffs = [(r["rta"] - r["static"], r) for r in rows]
    diffs.sort(reverse=True)
    print("\nTop 10 'most retiming-relaxed' nets (rta_norm − static_norm):")
    print(
        f"{'#':>3}  {'delta':>8}  {'static':>8}  {'rta':>8}  "
        f"{'cost':>8}  cut(stat/rta)  driver"
    )
    for i, (d, r) in enumerate(diffs[:10]):
        cs = "Y" if is_net_cut(r["driver"], r["loads"], static_part) else "N"
        cr = "Y" if is_net_cut(r["driver"], r["loads"], rta_part) else "N"
        print(
            f"{i:>3}  {d:>+8.4f}  {r['static']:>8.4f}  {r['rta']:>8.4f}  "
            f"{r['cost']:>8.4f}  {cs:>5}/{cr:<5}    {r['driver']}"
        )


if __name__ == "__main__":
    main()
