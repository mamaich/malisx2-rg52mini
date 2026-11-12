# avgrgb.py <ppm>...: mean R G B of each grab (a quick check that two runs show the same scene in the same lighting)
import sys
for p in sys.argv[1:]:
    d = open(p, 'rb').read()
    # P6 header: magic, width, height, maxval
    parts = d.split(b'\n', 3)
    px = parts[3] if len(parts) == 4 else d[15:]
    n = len(px) // 3
    r = sum(px[0::3][::16]); g = sum(px[1::3][::16]); b = sum(px[2::3][::16]); k = len(px[0::3][::16])
    print('%-22s R %5.1f G %5.1f B %5.1f' % (p.split('/')[-1], r / k, g / k, b / k))
