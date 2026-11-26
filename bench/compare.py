#!/usr/bin/env python3
# Times two cluau builds on the same scripts (the median of several runs
# each, the two builds taking turns) and prints the change: a release's
# speed against an earlier one's. A run that fails stops it.
#
#   python3 bench/compare.py <old cluau> <new cluau>

import os
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CASES = [
    ("hello (-e)", ["-e", "print(1)"], 60),
    ("startup.cluau", [os.path.join(HERE, "startup.cluau")], 40),
    ("compute.cluau", [os.path.join(HERE, "compute.cluau")], 7),
    ("tables.cluau", [os.path.join(HERE, "tables.cluau")], 5),
]


def run(command):
    """Runs a case once and returns how long it took; a run that fails stops the comparison."""
    start = time.perf_counter()
    result = subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, check=False)
    elapsed = time.perf_counter() - start
    if result.returncode != 0:
        error = result.stderr.decode(errors="replace").strip()
        raise RuntimeError(f"{' '.join(command)} exited with status {result.returncode}" + (f": {error}" if error else ""))
    return elapsed


def medians(old, new, runs):
    """The median time of each command, their runs taking turns (so that a
    machine that slows down or speeds up meanwhile affects both alike)."""
    run(old)  # once each first: a build that fails stops here, and caches are warm
    run(new)
    a, b = [], []
    for _ in range(runs):
        a.append(run(old))
        b.append(run(new))
    return statistics.median(a), statistics.median(b)


def main():
    if len(sys.argv) != 3:
        print("usage: python3 bench/compare.py <old cluau> <new cluau>", file=sys.stderr)
        return 2
    old, new = sys.argv[1], sys.argv[2]
    print(f"{'case':16} {'old':>10} {'new':>10} {'change':>8}", flush=True)
    for name, args, runs in CASES:
        try:
            a, b = medians([old] + args, [new] + args, runs)
        except (OSError, RuntimeError) as error:
            print(f"compare.py: {name}: {error}", file=sys.stderr)
            return 1
        print(f"{name:16} {a * 1000:9.1f}ms {b * 1000:9.1f}ms {100 * (b - a) / a:+7.0f}%", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
