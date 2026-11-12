#!/usr/bin/env python3
# sum.py RUN_DIR... - totals of runall.sh runs: shaders, failures, compile time (sum, max), and the
# sums over all shaders of instructions, static cycles, spills and fills (main and IDVS varying
# shader together), plus the same for the vertex shaders above 1000 cycles alone.
import re
import sys

PART = re.compile(r"\| (main|var) (\d+) instrs (\d+) clauses ([\d.]+) cyc (\d+) regs (\d+) thr "
                  r"(\d+):(\d+) sp:fi (\d+) cost")
for d in sys.argv[1:]:
    n = fail = 0
    t_sum = t_max = 0.0
    tot = [0, 0.0, 0, 0]
    big = [0, 0.0, 0, 0, 0]
    for line in open(d + "/summary.txt"):
        n += 1
        if "TIMEOUT" in line:
            fail += 1
            continue
        t = float(line.split(" ms ")[0].split()[-1])
        t_sum += t
        t_max = max(t_max, t)
        cyc = 0.0
        acc = [0, 0.0, 0, 0]
        for m in PART.finditer(line):
            acc[0] += int(m.group(2))
            acc[1] += float(m.group(4))
            acc[2] += int(m.group(7))
            acc[3] += int(m.group(8))
        for i in range(4):
            tot[i] += acc[i]
        if acc[1] > 1000:
            big[4] += 1
            for i in range(4):
                big[i] += acc[i]
    print(f"{d}: {n} shaders, {fail} failed, compile {t_sum / 1000:.1f} s total, max {t_max / 1000:.1f} s | "
          f"all: {tot[0]} instrs {tot[1]:.0f} cyc {tot[2]}:{tot[3]} sp:fi | "
          f"{big[4]} big: {big[0]} instrs {big[1]:.0f} cyc {big[2]}:{big[3]} sp:fi")
