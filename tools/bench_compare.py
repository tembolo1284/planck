#!/usr/bin/env python3
"""Compare two benchmark runs and point out what got slower.

    tools/bench_compare.py OLD.csv NEW.csv
    tools/bench_compare.py OLD_DIRECTORY NEW_DIRECTORY

The inputs are what planck-bench --csv and planck-loadgen --csv print. Given
two directories, every .csv file in each is read. Rows are matched by
benchmark name.

A row counts as slower when its median or its 99th percentile grew by more
than the allowed share (--p50 and --p99, in percent). Rows where both runs
are under --floor nanoseconds are shown and never flagged: at that size the
measurement is mostly noise.

Exit status is 1 if anything got slower, 0 otherwise. Benchmarks on a shared
or unpinned machine move by tens of percent on their own, so treat a flag as
a reason to run again, not as proof.
"""
import argparse
import csv
import os
import sys

COLUMNS = ("p50_ns", "p99_ns", "p999_ns")


def read_rows(path):
    """Read one csv file or every csv file in a directory.

    Returns {benchmark name: {column: value}}. A name that appears twice
    keeps its last row.
    """
    if os.path.isdir(path):
        files = sorted(os.path.join(path, name) for name in os.listdir(path)
                       if name.endswith(".csv"))
        if not files:
            raise ValueError("no .csv files in %s" % path)
    else:
        files = [path]
    rows = {}
    for name in files:
        with open(name, newline="") as handle:
            for row in csv.DictReader(handle):
                if "benchmark" not in row or any(column not in row for column in COLUMNS):
                    raise ValueError("%s is not benchmark output" % name)
                rows[row["benchmark"]] = {column: float(row[column]) for column in COLUMNS}
    return rows


def change(old, new):
    """Growth from old to new in percent. Zero if there is nothing to compare."""
    if old <= 0:
        return 0.0
    return (new - old) * 100.0 / old


def compare(old_rows, new_rows, p50_limit, p99_limit, floor):
    """Line up two runs.

    Returns (lines, slower, missing, added). Each line is
    (name, old, new, {column: percent}, verdict) with verdict one of
    "slower", "faster", "" and "small".
    """
    lines = []
    slower = []
    for name in sorted(set(old_rows) & set(new_rows)):
        old, new = old_rows[name], new_rows[name]
        changes = {column: change(old[column], new[column]) for column in COLUMNS}
        if old["p99_ns"] < floor and new["p99_ns"] < floor:
            verdict = "small"
        elif changes["p50_ns"] > p50_limit or changes["p99_ns"] > p99_limit:
            verdict = "slower"
            slower.append(name)
        elif changes["p50_ns"] < -p50_limit or changes["p99_ns"] < -p99_limit:
            verdict = "faster"
        else:
            verdict = ""
        lines.append((name, old, new, changes, verdict))
    missing = sorted(set(old_rows) - set(new_rows))
    added = sorted(set(new_rows) - set(old_rows))
    return lines, slower, missing, added


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("old")
    parser.add_argument("new")
    parser.add_argument("--p50", type=float, default=10.0, metavar="PERCENT",
                        help="allowed growth of the median (default 10)")
    parser.add_argument("--p99", type=float, default=25.0, metavar="PERCENT",
                        help="allowed growth of the 99th percentile (default 25)")
    parser.add_argument("--floor", type=float, default=10.0, metavar="NS",
                        help="never flag rows where both runs are below this (default 10)")
    arguments = parser.parse_args(argv)

    try:
        old_rows = read_rows(arguments.old)
        new_rows = read_rows(arguments.new)
    except (OSError, ValueError, KeyError) as problem:
        print("bench_compare: %s" % problem, file=sys.stderr)
        return 2

    lines, slower, missing, added = compare(old_rows, new_rows, arguments.p50, arguments.p99,
                                            arguments.floor)
    print("%-26s %10s %10s %7s   %10s %10s %7s   %s" % (
        "benchmark (ns)", "p50 old", "p50 new", "", "p99 old", "p99 new", "", ""))
    for name, old, new, changes, verdict in lines:
        print("%-26s %10.1f %10.1f %+6.0f%%   %10.1f %10.1f %+6.0f%%   %s" % (
            name, old["p50_ns"], new["p50_ns"], changes["p50_ns"],
            old["p99_ns"], new["p99_ns"], changes["p99_ns"], verdict))
    for name in missing:
        print("%-26s only in the old run" % name)
    for name in added:
        print("%-26s only in the new run" % name)

    if slower:
        print("\n%d slower: %s" % (len(slower), ", ".join(slower)))
        return 1
    print("\nnothing got slower by more than %.0f%% (p50) or %.0f%% (p99)" % (
        arguments.p50, arguments.p99))
    return 0


if __name__ == "__main__":
    sys.exit(main())
