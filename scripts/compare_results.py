#!/usr/bin/env python3
# ─── OpenRVBench :: Result Comparison Tool ───────────────────────────────────
# Loads multiple result JSON files and produces a side-by-side comparison
# in either terminal or CSV format.
#
# Usage:
#   python3 compare_results.py results/*.json
#   python3 compare_results.py results/*.json --csv > comparison.csv
# ─────────────────────────────────────────────────────────────────────────────
"""Compare multiple OpenRVBench result files."""

import argparse
import csv
import json
import pathlib
import sys
from typing import List, Dict, Any


def load_result(path: pathlib.Path) -> Dict[str, Any]:
    data = json.loads(path.read_text())
    bench_map = {}
    for b in data.get("benchmarks", []):
        bid = b.get("bench_id")
        if bid:
            bench_map[bid] = b
    return {
        "file":    path.name,
        "board":   data.get("board", {}).get("board", "?"),
        "isa":     data.get("board", {}).get("isa", "?"),
        "cores":   data.get("board", {}).get("cores", "?"),
        "ram_gb":  data.get("board", {}).get("ram_gb", "?"),
        "ts":      data.get("timestamp", "?")[:10],
        "benches": bench_map,
    }


def all_bench_ids(records: List[Dict]) -> List[str]:
    ids = []
    for rec in records:
        for bid in rec["benches"]:
            if bid not in ids:
                ids.append(bid)
    return ids


def fmt(v, precision=1):
    if v is None:
        return "—"
    if isinstance(v, float):
        return f"{v:.{precision}f}"
    return str(v)


def compare_terminal(records: List[Dict]):
    bench_ids = all_bench_ids(records)

    # Header
    col = 26
    print(f"\n  {'Board':<{col}}  {'ISA':<18}  {'Cores':>5}  {'RAM':>5}", end="")
    for bid in bench_ids:
        print(f"  {bid.upper():>10}", end="")
    print(f"  {'TOTAL':>10}")
    print("  " + "─" * (col + 18 + 5 + 5 + len(bench_ids) * 12 + 20))

    for rec in records:
        board = rec["board"][:col-1]
        isa   = rec["isa"][:17]
        print(f"  {board:<{col}}  {isa:<18}  {rec['cores']:>5}  {fmt(rec['ram_gb'], 0):>4}G",
              end="")
        total = 0.0
        for bid in bench_ids:
            b = rec["benches"].get(bid)
            if b and b.get("passed"):
                score = b.get("score", 0.0)
                total += score
                print(f"  {fmt(score):>10}", end="")
            else:
                print(f"  {'—':>10}", end="")
        print(f"  {fmt(total):>10}")

    print()

    # Per-benchmark detail
    print("  ── Per-metric Comparison ────────────────────────────────────────────")
    for bid in bench_ids:
        print(f"\n  [{bid.upper()}]")
        # Collect all metric names for this bench
        metric_names = []
        for rec in records:
            b = rec["benches"].get(bid, {})
            for m in b.get("metrics", []):
                if m["name"] not in metric_names and not isinstance(m.get("value"), bool):
                    metric_names.append(m["name"])

        # Header row
        row_hdr = f"    {'Metric':<32}"
        for rec in records:
            board = rec["board"][:14]
            row_hdr += f"  {board:>14}"
        print(row_hdr)
        print("    " + "─" * (32 + 16 * len(records)))

        for mname in metric_names:
            row = f"    {mname.replace('_', ' '):<32}"
            unit = ""
            for rec in records:
                b = rec["benches"].get(bid, {})
                val = None
                for m in b.get("metrics", []):
                    if m["name"] == mname:
                        val = m.get("value")
                        unit = m.get("unit", "")
                        break
                row += f"  {fmt(val):>14}"
            row += f"  {unit}"
            print(row)


def compare_csv(records: List[Dict]):
    bench_ids = all_bench_ids(records)
    writer = csv.writer(sys.stdout)

    # Build header
    header = ["board", "isa", "cores", "ram_gb", "timestamp"]
    for bid in bench_ids:
        header.append(f"{bid}_score")
        header.append(f"{bid}_unit")
    header.append("total_score")
    writer.writerow(header)

    for rec in records:
        row = [rec["board"], rec["isa"], rec["cores"], rec["ram_gb"], rec["ts"]]
        total = 0.0
        for bid in bench_ids:
            b = rec["benches"].get(bid)
            if b and b.get("passed"):
                score = b.get("score", 0.0)
                total += score
                row.append(fmt(score))
                row.append(b.get("score_unit", "pts"))
            else:
                row.append("")
                row.append("")
        row.append(fmt(total))
        writer.writerow(row)


def main():
    p = argparse.ArgumentParser(
        description="Compare OpenRVBench result files")
    p.add_argument("files", nargs="+", help="Result JSON files to compare")
    p.add_argument("--csv", action="store_true",
                   help="Output CSV instead of terminal table")
    args = p.parse_args()

    records = []
    for fpath in args.files:
        path = pathlib.Path(fpath)
        if not path.exists():
            print(f"Warning: {fpath} not found", file=sys.stderr)
            continue
        try:
            records.append(load_result(path))
        except Exception as e:
            print(f"Warning: Could not load {fpath}: {e}", file=sys.stderr)

    if not records:
        print("No valid result files found.", file=sys.stderr)
        sys.exit(1)

    if args.csv:
        compare_csv(records)
    else:
        compare_terminal(records)


if __name__ == "__main__":
    main()
