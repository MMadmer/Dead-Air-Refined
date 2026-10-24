"""Average the vegetation's cost per heading and mode from tools/qa/water/veg_cost.lua runs (docs/dead-air/VEGETATION_GOT.md).

Reads the log of every run given (a run folder of tools/qa/Run-WaterQa.ps1 with its log.txt, or a log file): the
r__gpu_log lines "[gpu] frame=<ms> key=<ms> ..." and the r__fps_log lines "[FPS] avg <n>" between the scenario's
"[qa] veg begin <view> <mode>" and "[qa] veg end <view> <mode>" markers. Prints one row per heading and mode (the
mean of its lines) and a row per mode summed over the headings, the modes in the plan's order.

    python tools/qa/Parse-VegCost.py <run folder or log> [...]
"""

import re
import sys
from collections import defaultdict
from pathlib import Path

KEYS = ["frame", "scene", "sun", "lights", "grass", "grass_gen", "shrubs", "shrubs_bend", "trees", "trees_bend",
        "trees_bb", "gd", "combine", "trees_cpu", "shrubs_cpu", "veg_wait", "fps"]


def parse(path):
    cur = None
    acc = defaultdict(lambda: defaultdict(list))
    order = []
    for line in open(path, encoding="utf-8", errors="replace"):
        if m := re.search(r"\[qa\] veg begin (\S+) (\S+)", line):
            cur = (m.group(1), m.group(2))
            if cur not in order:
                order.append(cur)
        elif re.search(r"\[qa\] veg end ", line):
            cur = None
        elif cur and (m := re.search(r"\[FPS\] avg ([0-9.]+)", line)):
            acc[cur]["fps"].append(float(m.group(1)))
        elif cur and "[gpu] frame=" in line:
            for k, v in re.findall(r"\b([a-z_]+)=([0-9.]+)", line):
                acc[cur][k].append(float(v))
    return order, acc


def main():
    for arg in sys.argv[1:]:
        path = Path(arg) / "log.txt" if Path(arg).is_dir() else Path(arg)
        order, acc = parse(path)
        print("==", path)
        print("%-6s %-10s" % ("view", "mode") + "".join("%11s" % k for k in KEYS))
        total = defaultdict(lambda: defaultdict(float))
        for key in order:
            row = []
            for k in KEYS:
                vals = acc[key].get(k, [])
                v = sum(vals) / len(vals) if vals else float("nan")
                row.append(v)
                total[key[1]][k] += v
            print("%-6s %-10s" % key + "".join("%11.3f" % v for v in row))
        for mode, d in total.items():
            print("%-6s %-10s" % ("sum", mode) + "".join("%11.3f" % d[k] for k in KEYS))


if __name__ == "__main__":
    main()
