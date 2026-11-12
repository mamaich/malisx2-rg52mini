# slots.py <trace>: per kbase job slot, how busy the GPU is between frames 360 and 660 of the bench
import re, sys, collections
ev = []; frames = []
pat = re.compile(r"^\s*(\S+)-(\d+)\s+\[\d+\]\s+\S+\s+([\d.]+): (\w+): (.*)$")
for line in open(sys.argv[1]):
    m = pat.match(line)
    if not m: continue
    t = float(m.group(3))
    if m.group(4) == "cv_run": frames.append(t); continue
    if m.group(4) != "mali_job_slots_event": continue
    f = dict(kv.split("=") for kv in m.group(5).split())
    e = int(f["event"]); typ, slot = e >> 24, (e >> 16) & 0xff
    ev.append((t, typ, slot, int(f["tgid"]), int(f["job_id"]), m.group(1)))
w0, w1 = frames[360], frames[660]; nfr = 300; span = w1 - w0
print(f"{sys.argv[1]}: window {span:.2f} s, {nfr/span:.2f} fps, {1000*span/nfr:.1f} ms/frame")
tg = collections.Counter((e[3], e[5]) for e in ev if e[1] == 1 and w0 <= e[0] < w1)
print("  starts by tgid/thread:", dict(tg.most_common(4)))
for slot in (0, 1):
    starts = [e for e in ev if e[2] == slot and e[1] == 1]
    stops = [e for e in ev if e[2] == slot and e[1] in (2, 3)]
    # a trace that begins inside a job has its stop first: drop stops before the first start
    if starts: stops = [e for e in stops if e[0] >= starts[0][0]]
    # atoms leave a slot in the order they entered it: pair the n-th start with the n-th stop
    n = min(len(starts), len(stops)); runs = []
    prev_stop = 0
    for i in range(n):
        s, t = starts[i][0], stops[i][0]
        b = max(s, prev_stop); prev_stop = t
        if b < w1 and t > w0: runs.append((max(b, w0), min(t, w1)))
    busy = sum(t - b for b, t in runs)
    d = sorted((t - b) * 1e3 for b, t in runs)
    q = lambda p: d[int(p * (len(d) - 1))] if d else 0
    name = "fragment" if slot == 0 else "vertex/tiler/compute"
    print(f"  slot {slot} {name}: {len(runs)} jobs = {len(runs)/nfr:.1f}/frame, busy {100*busy/span:.0f}% = {1000*busy/nfr:.1f} ms/frame;"
          f" job ms median {q(.5):.3f}, p90 {q(.9):.3f}, max {q(1):.2f}; unpaired {len(starts)-len(stops)}")
    by = collections.Counter()
    for x in d: by[min(int(x / 0.25), 12)] += x
    print("    ms per frame by job length (0.25 ms bins, last = 3 ms+):",
          " ".join(f"{k*0.25:.2f}:{v/nfr:.1f}" for k, v in sorted(by.items())))
