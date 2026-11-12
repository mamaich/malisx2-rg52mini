# overlap.py <trace>: per frame, time both slots run together, one alone, or the GPU idles (frames 360-660)
import re, sys
pat = re.compile(r"^\s*(\S+)-(\d+)\s+\[\d+\]\s+\S+\s+([\d.]+): (\w+): (.*)$")
ev = []; frames = []
for line in open(sys.argv[1]):
    m = pat.match(line)
    if not m: continue
    t = float(m.group(3))
    if m.group(4) == "cv_run": frames.append(t); continue
    if m.group(4) != "mali_job_slots_event": continue
    e = int(re.search(r"event=(\d+)", m.group(5)).group(1))
    ev.append((t, e >> 24, (e >> 16) & 0xff))
w0, w1 = frames[360], frames[660]
runs = {0: [], 1: []}
for slot in (0, 1):
    st = [t for t, ty, s in ev if s == slot and ty == 1]; sp = [t for t, ty, s in ev if s == slot and ty in (2, 3)]
    if st: sp = [t for t in sp if t >= st[0]]  # stops of a job begun before the trace
    prev = 0
    for s, t in zip(st, sp):
        b = max(s, prev); prev = t
        if b < w1 and t > w0: runs[slot].append((max(b, w0), min(t, w1)))
# sweep
pts = []
for slot in (0, 1):
    for b, t in runs[slot]: pts += [(b, slot, 1), (t, slot, -1)]
pts.sort(); act = {0: 0, 1: 0}; last = w0; acc = {"both": 0, "frag only": 0, "vtc only": 0, "idle": 0}
for t, slot, d in pts:
    k = "both" if act[0] and act[1] else "frag only" if act[0] else "vtc only" if act[1] else "idle"
    acc[k] += t - last; last = t; act[slot] += d
acc["idle"] += w1 - last
print(sys.argv[1].split("/")[-1], f"{1000*(w1-w0)/300:.1f} ms/frame:", ", ".join(f"{k} {1000*v/300:.1f}" for k, v in acc.items()))
