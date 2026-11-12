# jcdec.py <capture dir>: count the jobs malisx2 submits on the G52 (v7 job headers), per type,
# from its MALISX2_MEASURE=csf capture (jm-*.bin, recorder_jm.c)
import glob, struct, sys, bisect, collections
TYPES = {1: "null", 2: "write value", 3: "cache flush", 4: "compute", 5: "vertex", 6: "geometry",
         7: "tiler", 8: "fused", 9: "fragment", 10: "indexed vertex"}
tot = collections.Counter(); draws = passes = disp = 0; nsub = 0; barriers = 0
vranges = []; per_atom = []
for fn in sorted(glob.glob(sys.argv[1] + "/jm-*.bin")):
    d = open(fn, "rb").read()
    assert d[:8] == b"MALIJM01"
    p = 32; atoms = []; mem = []
    while p < len(d):
        typ, arg, a, b, size = struct.unpack_from("<IIQQQ", d, p); p += 32
        if typ == 0xffffffff: break
        payload = d[p:p + size]; p += (size + 7) & ~7
        if typ == 1:
            for i in range(arg): atoms.append(payload[64 * i:64 * i + 64])
        elif typ == 2: mem.append((a, payload))
        elif typ == 3:
            dd, dr, ps, _ = struct.unpack_from("<IIII", payload); disp += dd; draws += dr; passes += ps
    mem.sort(); keys = [m[0] for m in mem]
    def read(addr, n):
        i = bisect.bisect_right(keys, addr) - 1
        if i < 0: return None
        base, buf = mem[i]
        off = addr - base
        return buf[off:off + n] if off + n <= len(buf) else None
    nsub += 1
    for at in atoms:
        jc, = struct.unpack_from("<Q", at, 8); core_req, = struct.unpack_from("<I", at, 52)
        c = collections.Counter(); j = jc; guard = 0
        while j and guard < 100000:
            h = read(j, 32)
            if h is None: c["(unreadable)"] += 1; break
            w4, w5 = struct.unpack_from("<II", h, 16); nxt, = struct.unpack_from("<Q", h, 24)
            t = (w4 >> 1) & 0x7f; c[TYPES.get(t, t)] += 1; barriers += (w4 >> 8) & 1
            if t == 5:
                inv = read(j + 32, 8)
                if inv:
                    v, w1 = struct.unpack("<II", inv)
                    ys, zs = (w1 >> 16) & 63, (w1 >> 22) & 63
                    vranges.append(((v >> ys) & ((1 << (zs - ys)) - 1)) + 1 if zs > ys else 1)
            j = nxt; guard += 1
        tot.update(c); per_atom.append((core_req, sum(c.values())))
print(f"{nsub} submits: {draws} draws, {passes} passes, {disp} dispatches recorded")
for k, v in tot.most_common(): print(f"  {k:14} {v:7}  {v/max(draws,1):.2f} per draw")
print(f"  jobs with a barrier: {barriers}")
if vranges:
    vr = sorted(vranges); print(f"  vertex jobs: vertex range median {vr[len(vr)//2]}, mean {sum(vr)/len(vr):.0f}, max {vr[-1]}, total {sum(vr)}")
print(f"  atoms {len(per_atom)}; jobs per atom median {sorted(n for _, n in per_atom)[len(per_atom)//2]}")
