#!/usr/bin/env python3
# Times two cluau builds on the same scripts (the median of several runs
# each) and prints the change: a release's speed against an earlier one's.
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


def median(command, runs):
    times = []
    for _ in range(runs):
        start = time.perf_counter()
        subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        times.append(time.perf_counter() - start)
    return statistics.median(times)


def main():
    if len(sys.argv) != 3:
        print("usage: python3 bench/compare.py <old cluau> <new cluau>", file=sys.stderr)
        return 2
    old, new = sys.argv[1], sys.argv[2]
    print(f"{'case':16} {'old':>10} {'new':>10} {'change':>8}")
    for name, args, runs in CASES:
        a = median([old] + args, runs)
        b = median([new] + args, runs)
        print(f"{name:16} {a * 1000:9.1f}ms {b * 1000:9.1f}ms {100 * (b - a) / a:+7.0f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
