# fp16log.py <log>...: the MALISX2_V7_FP16=1 choices a run logged (MALISX2_V7_FP16_LOG=1): how many
# fragment shaders the pass changed, which build was kept, and the cycles of the kept builds
# against the 32-bit ones (the compiler's estimate, per shader once).
import re, sys
pat = re.compile(r"malisx2 fp16 ([0-9a-f]{8}): (FP16|FP32); FP16 ([\d.]+) cycles (\d+) threads (\d+):(\d+) spills, FP32 ([\d.]+) cycles (\d+) threads (\d+):(\d+) spills")
for path in sys.argv[1:]:
    seen = {}
    for ln in open(path, encoding="utf-8", errors="replace"):
        m = pat.search(ln)
        if m:
            seen.setdefault(m.group(1), m.groups())
    if not seen:
        print(f"{path}: no choices logged"); continue
    v = list(seen.values())
    k16 = [x for x in v if x[1] == "FP16"]
    c32 = sum(float(x[6]) for x in v)
    ckept = sum(float(x[2]) if x[1] == "FP16" else float(x[6]) for x in v)
    t_up = sum(int(x[3]) > int(x[7]) for x in k16)
    print(f"{path}: {len(v)} changed shaders, FP16 kept {len(k16)}, FP32 kept {len(v) - len(k16)}; "
          f"cycles of the kept builds {ckept:.1f} vs all FP32 {c32:.1f} ({100 * (ckept - c32) / c32:+.1f} %); "
          f"FP16 builds with more threads: {t_up}")
    gain = sorted(k16, key=lambda x: float(x[2]) - float(x[6]))[:5]
    for x in gain:
        print(f"    {x[0]} {x[6]} -> {x[2]} cycles, threads {x[7]} -> {x[3]}")
