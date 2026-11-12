#!/usr/bin/env python3
# hwc-ana.py <layout.xml> <out>... - mhwc totals per frame: counter names from Mesa's
# hardwarelayout/Mali-G52.xml; frames from "F <count>" lines the run script appends. Prints the
# counters that matter for where the GPU time goes, per frame, side by side for each file.
import re, sys
import xml.etree.ElementTree as ET

layout = ET.parse(sys.argv[1]).getroot()
names = {}
order = {"GPU Front-end": 0, "Tiler": 1, "Memory System": 2, "Shader Core": 3}
for blk in layout.iter("CounterBlock"):
    t = order.get(blk.get("type"))
    if t is None:
        continue
    for c in blk.iter("Counter"):
        names[(t, int(c.get("index")))] = c.get("name")


def load(path):
    v, ns, cyc, frames = {}, 0, 0, 0
    for line in open(path):
        p = line.split()
        if not p:
            continue
        if p[0] == "T":
            ns, cyc = int(p[1]), int(p[2])
        elif p[0] == "F":
            frames = int(p[1])
        elif p[0] == "C":
            t, b, i, val = int(p[1]), int(p[2]), int(p[3]), int(p[4])
            n = names.get((t, i), f"{t}:{i}")
            v[n] = v.get(n, 0) + val
    return v, ns, cyc, frames


runs = [(p.split("/")[-1], *load(p)) for p in sys.argv[2:]]
keys = ["GPU_ACTIVE", "JS0_ACTIVE", "JS1_ACTIVE", "JS0_JOBS", "JS1_JOBS", "JS0_TASKS", "JS1_TASKS",
        "JS0_WAIT_FLUSH", "JS1_WAIT_FLUSH", "JS0_WAIT_READ", "JS1_WAIT_READ", "JS0_WAIT_ISSUE", "JS1_WAIT_ISSUE",
        "JS0_WAIT_DEPEND", "JS1_WAIT_DEPEND", "JS0_WAIT_FINISH", "JS1_WAIT_FINISH", "CACHE_FLUSH",
        "TILER_ACTIVE", "TRIANGLES", "PRIM_VISIBLE", "PRIM_CULLED", "PRIM_CLIPPED", "IDVS_POS_SHAD_REQ",
        "VCACHE_HIT", "VCACHE_MISS",
        "FRAG_ACTIVE", "FRAG_PRIMITIVES", "FRAG_PRIM_RAST", "FRAG_FPK_ACTIVE", "FRAG_WARPS", "FRAG_PARTIAL_WARPS",
        "FRAG_QUADS_RAST", "FRAG_QUADS_EZS_TEST", "FRAG_QUADS_EZS_UPDATE", "FRAG_QUADS_EZS_KILL",
        "FRAG_LZS_TEST", "FRAG_LZS_KILL", "FRAG_PTILES", "FRAG_TRANS_ELIM",
        "COMPUTE_ACTIVE", "COMPUTE_TASKS", "COMPUTE_WARPS", "COMPUTE_STARVING", "FRAG_STARVING",
        "EXEC_CORE_ACTIVE", "EXEC_INSTR_FMA", "EXEC_INSTR_CVT", "EXEC_INSTR_SFU", "EXEC_INSTR_MSG",
        "EXEC_INSTR_DIVERGED", "EXEC_ICACHE_MISS", "EXEC_STARVE_ARITH", "CALL_BLEND_SHADER",
        "TEX_MSGI_NUM_QUADS", "TEX_DFCH_NUM_PASSES", "TEX_TFCH_NUM_OPERATIONS", "TEX_FILT_NUM_OPERATIONS",
        "LS_MEM_READ_FULL", "LS_MEM_READ_SHORT", "LS_MEM_WRITE_FULL", "LS_MEM_WRITE_SHORT", "LS_MEM_ATOMIC",
        "VARY_INSTR", "VARY_SLOT_32", "VARY_SLOT_16", "ATTR_INSTR",
        "BEATS_RD_FTC", "BEATS_RD_FTC_EXT", "BEATS_RD_LSC", "BEATS_RD_LSC_EXT", "BEATS_RD_TEX", "BEATS_RD_TEX_EXT",
        "BEATS_RD_OTHER", "BEATS_WR_LSC_WB", "BEATS_WR_TIB", "BEATS_WR_LSC_OTHER",
        "L2_RD_MSG_IN", "L2_WR_MSG_IN", "L2_ANY_LOOKUP", "L2_READ_LOOKUP", "L2_EXT_READ", "L2_EXT_WRITE",
        "L2_EXT_READ_BEATS", "L2_EXT_WRITE_BEATS", "L2_EXT_R_BUF_FULL", "L2_EXT_RD_BUF_FULL",
        "L2_EXT_W_STALL", "L2_EXT_AR_STALL", "L2_EXT_READ_STALL"]
allk = keys + sorted({k for r in runs for k in r[1]} - set(keys))
print("%-26s" % "per frame" + "".join("%16s" % r[0][:15] for r in runs))
print("%-26s" % "frames" + "".join("%16d" % r[4] for r in runs))
print("%-26s" % "seconds" + "".join("%16.1f" % (r[2] / 1e9) for r in runs))
print("%-26s" % "gpu cycles/frame" + "".join("%16.0f" % (r[3] / max(r[4], 1)) for r in runs))
for k in allk:
    if not any(k in r[1] for r in runs):
        continue
    print("%-26s" % k + "".join("%16.0f" % (r[1].get(k, 0) / max(r[4], 1)) for r in runs))
