# sdbcmp.py <stage> <log A> <log B>: BIFROST_MESA_DEBUG=shaderdb statistics of the same shaders (by
# source_blake3) in two logs, e.g. without and with MALISX2_V7_FP16=1: totals of instructions,
# tuples, clauses and cycles over the shaders both logs compiled, how many got better or worse,
# and the ten biggest changes in cycles. <stage>: FRAGMENT or VERTEX.
import re, sys, collections
stage, fa, fb = sys.argv[1:4]
pat = re.compile(r"MESA_SHADER_%s shader: (\d+) instrs, (\d+) tuples, (\d+) clauses, ([\d.]+) cycles" % stage)
def load(path):
    out = collections.OrderedDict(); blake = None
    for ln in open(path, encoding="utf-8", errors="replace"):
        if ln.startswith("source_blake3"):
            blake = ln.split(":", 1)[-1].strip()[:40]
        m = pat.search(ln)
        if m and blake:
            out.setdefault(blake, (int(m.group(1)), int(m.group(2)), int(m.group(3)), float(m.group(4))))
            blake = None
    return out
A, B = load(fa), load(fb)
common = [k for k in A if k in B]
print(f"{stage}: {len(A)} shaders in A, {len(B)} in B, {len(common)} in both")
if not common:
    sys.exit()
names = ["instrs", "tuples", "clauses", "cycles"]
for i, n in enumerate(names):
    sa = sum(A[k][i] for k in common); sb = sum(B[k][i] for k in common)
    print(f"  {n:8} {sa:10.1f} -> {sb:10.1f}  {100 * (sb - sa) / sa:+.1f} %")
better = sum(B[k][3] < A[k][3] for k in common); worse = sum(B[k][3] > A[k][3] for k in common)
print(f"  cycles: {better} better, {worse} worse, {len(common) - better - worse} same")
d = sorted(common, key=lambda k: B[k][3] - A[k][3])
for k in d[:5] + d[-5:]:
    print(f"    {k[:12]} cycles {A[k][3]:6.1f} -> {B[k][3]:6.1f}  tuples {A[k][1]:4d} -> {B[k][1]:4d}")
