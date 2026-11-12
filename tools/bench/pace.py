# pace.py <trace>: frame pacing between frames 360 and 660 of the bench (azahar-bench.sh GPUTRACE=1):
# emulated frame intervals (retro_run) by refresh periods, DRM flips, and the GPU's idle gaps
# (neither job slot running): how many end within 2 ms after a flip, i.e. the GPU waited for it.
import re, sys, collections
pat = re.compile(r"^\s*(\S+)-(\d+)\s+\[\d+\]\s+\S+\s+([\d.]+): (\w+): (.*)$")
frames, flips, ev = [], [], []
for line in open(sys.argv[1]):
    m = pat.match(line)
    if not m: continue
    t, name = float(m.group(3)), m.group(4)
    if name == "cv_run": frames.append(t)
    elif name == "drm_vblank_event_delivered": flips.append(t)
    elif name == "mali_job_slots_event":
        e = int(re.search(r"event=(\d+)", m.group(5)).group(1))
        ev.append((t, e >> 24, (e >> 16) & 0xff))
w0, w1 = frames[360], frames[660]
P = 1000 / 60
d = [(b - a) * 1e3 for a, b in zip(frames[360:660], frames[361:661])]
h = collections.Counter(min(6, round(x / P * 2) / 2) for x in d)
fl = [t for t in flips if w0 <= t < w1]
print(f"{sys.argv[1]}: {300 / (w1 - w0):.2f} fps, flips {len(fl)} ({len(fl) / (w1 - w0):.1f}/s)")
print("  frame interval in refresh periods (rounded to 0.5):",
      " ".join(f"{k:g}:{v}" for k, v in sorted(h.items())))
srt = sorted(d); q = lambda p: srt[int(p * (len(srt) - 1))]
print(f"  frame ms: p10 {q(.1):.1f} p50 {q(.5):.1f} p90 {q(.9):.1f} max {srt[-1]:.1f}")
# GPU busy intervals: a slot runs from its start to its stop (starts and stops pair in order)
runs = []
for slot in (0, 1):
    st = [e[0] for e in ev if e[2] == slot and e[1] == 1]
    sp = [e[0] for e in ev if e[2] == slot and e[1] in (2, 3)]
    if st: sp = [t for t in sp if t >= st[0]]  # stops of a job begun before the trace
    prev = 0
    for s, t in zip(st, sp):
        b = max(s, prev); prev = t
        if t > w0 and b < w1: runs.append((max(b, w0), min(t, w1)))
runs.sort(); merged = []
for b, t in runs:
    if merged and b <= merged[-1][1]: merged[-1][1] = max(merged[-1][1], t)
    else: merged.append([b, t])
gaps = [(a[1], b[0]) for a, b in zip(merged, merged[1:]) if b[0] - a[1] > 0.002]
idle = sum(b - a for a, b in zip([m[1] for m in merged], [m[0] for m in merged[1:]]))
import bisect
after_flip = 0
for g0, g1 in gaps:
    i = bisect.bisect_right(flips, g1)
    if i > 0 and g1 - flips[i - 1] <= 0.002: after_flip += 1
gl = sorted((b - a) * 1e3 for a, b in gaps)
print(f"  GPU idle {1e3 * idle / 300:.1f} ms/frame; gaps > 2 ms: {len(gaps)}"
      f" (median {gl[len(gl) // 2] if gl else 0:.1f} ms), ending within 2 ms after a flip: {after_flip}")
