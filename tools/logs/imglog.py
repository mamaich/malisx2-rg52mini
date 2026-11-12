#!/usr/bin/env python3
# imglog.py <log> - the images of a MALISX2_LOG_IMAGES=1 run grouped by format, levels, layers,
# usage, flags and layout, with the memory each group takes and would take as AFBC with tiled
# headers (mali_image_layout.c afbc_slice) where v7 could compress it (v7_can_use_afbc with levels).
import re, sys
from collections import defaultdict
pat = re.compile(r"malisx2 image (\S+) (\d+)x(\d+)x(\d+), (\d+) levels, (\d+) layers, (\d+) samples, "
                 r"usage 0x([0-9a-f]+), flags 0x([0-9a-f]+), tiling (\d+): (\S+)")
# Formats v7 compresses (v7_can_use_afbc), and bytes per pixel for the sizes.
AFBC_FMT = {"r8_unorm", "r8g8_unorm", "r8g8b8a8_unorm", "r8g8b8a8_srgb", "z32_float"}
BPP = {"r8_unorm": 1, "r8g8_unorm": 2, "r8g8b8a8_unorm": 4, "r8g8b8a8_srgb": 4, "z32_float": 4,
       "b8g8r8a8_unorm": 4, "b8g8r8a8_srgb": 4, "b5g6r5_unorm": 2, "r5g6b5_unorm": 2,
       "a4b4g4r4_unorm": 2, "r4g4b4a4_unorm": 2, "b5g5r5a1_unorm": 2, "a1r5g5b5_unorm": 2,
       "r5g5b5a1_unorm": 2, "z24x8_unorm": 4, "z24_unorm_s8_uint": 4, "z16_unorm": 2,
       "r10g10b10a2_unorm": 4, "r16g16b16a16_float": 8, "r32_float": 4}
def al(x, a): return (x + a - 1) // a * a
def afbc_level(w, h, bpp):
    ts = 8 if bpp <= 4 else 4
    aw, ah = al(w, 16 * ts), al(h, 16 * ts)
    row = al((aw // 16) * ts * 16, 1024 if bpp <= 4 else 256)
    sbs = (row // (16 * ts)) * (ah // 16)
    return al(sbs * 16, 4096) + sbs * 256 * bpp
def tiled_level(w, h, bpp):
    row = al(256 * bpp * ((w + 15) // 16), 64)
    return al(row * ((h + 15) // 16), 64)
def size(w, h, lv, ly, bpp, fn):
    return sum(fn(max(w >> l, 1), max(h >> l, 1), bpp) for l in range(lv)) * ly
SAMPLED, STORAGE, CA, DSA, HOSTX = 0x4, 0x8, 0x10, 0x20, 0x400000
MUTABLE, BLOCKTEXEL, ALIAS, DISJOINT = 0x8, 0x80, 0x400, 0x200
groups = defaultdict(lambda: [0, 0, 0, 0])
n = 0
for ln in open(sys.argv[1], errors="replace"):
    m = pat.search(ln)
    if not m:
        continue
    n += 1
    fmt, w, h, d, lv, ly, smp, usage, flags, tiling, lay = m.groups()
    w, h, lv, ly, usage, flags = int(w), int(h), int(lv), int(ly), int(usage, 16), int(flags, 16)
    bpp = BPP.get(fmt)
    why = []
    if fmt not in AFBC_FMT: why.append("format")
    if usage & (STORAGE | HOSTX): why.append("storage")
    if flags & (MUTABLE | BLOCKTEXEL | ALIAS | DISJOINT): why.append("mutable/alias")
    if not usage & (SAMPLED | CA | DSA): why.append("usage")
    if int(smp) != 1: why.append("msaa")
    if fmt in AFBC_FMT and (w < 64 or h < 64): why.append("small")
    key = (fmt, "lv1" if lv == 1 else "mips", ly, hex(usage), hex(flags), lay, ",".join(why) or "AFBC-able")
    g = groups[key]
    g[0] += 1
    if bpp:
        g[1] += size(w, h, lv, ly, bpp, tiled_level)
        g[2] += size(w, h, lv, ly, bpp, afbc_level)
    g[3] += w * h
print(f"{n} images")
print(f"{'count':>5} {'format':16} {'lv':4} {'ly':>2} {'usage':>8} {'flags':>6} {'layout':14} {'u-int MiB':>9} {'AFBC MiB':>9}  v7 AFBC")
for k, g in sorted(groups.items(), key=lambda kv: -kv[1][0]):
    print(f"{g[0]:5} {k[0]:16} {k[1]:4} {k[2]:2} {k[3]:>8} {k[4]:>6} {k[5]:14} {g[1]/2**20:9.1f} {g[2]/2**20:9.1f}  {k[6]}")
