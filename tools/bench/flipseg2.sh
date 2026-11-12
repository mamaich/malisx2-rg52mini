#!/bin/bash
# On the device, as root. flipseg2.sh start: record every DRM flip from now on
# (ES stopped, so only the game flips). flipseg2.sh stop <label> <A> <B> [<A2> <B2>...]:
# for each pair, fps between the game's A-th and B-th shown frame, and the
# frame time between them: mean, p95, p99, max (ms).
T=/sys/kernel/tracing
case $1 in
start)
  [ -d $T/events ] || mount -t tracefs nodev $T
  echo 0 > $T/tracing_on; echo > $T/trace; echo 4096 > $T/buffer_size_kb
  echo 1 > $T/events/drm/drm_vblank_event_delivered/enable
  echo 1 > $T/tracing_on ;;
stop)
  label=$2; shift 2
  echo 0 > $T/tracing_on; echo 0 > $T/events/drm/drm_vblank_event_delivered/enable
  grep drm_vblank_event_delivered $T/trace | grep -o '[0-9]*\.[0-9]*:' | tr -d : > /tmp/flips.txt
  echo > $T/trace
  python3 - "$label" "$@" <<'P'
import sys
label, pairs = sys.argv[1], [int(x) for x in sys.argv[2:]]
t = [float(x) for x in open("/tmp/flips.txt")]
out = "%s frames=%d in %.1f s |" % (label, len(t), t[-1] - t[0] if t else 0)
for a, b in zip(pairs[::2], pairs[1::2]):
    if b > len(t):
        out += " %d-%d: not reached" % (a, b); continue
    seg = t[a - 1:b]
    d = sorted((y - x) * 1000 for x, y in zip(seg, seg[1:]))
    q = lambda p: d[min(len(d) - 1, int(p * len(d)))]
    out += " %d-%d: %.2f fps, mean %.2f p95 %.2f p99 %.2f max %.2f ms" % (
        a, b, (b - a) / (seg[-1] - seg[0]), sum(d) / len(d), q(0.95), q(0.99), d[-1])
print(out)
P
  ;;
esac
