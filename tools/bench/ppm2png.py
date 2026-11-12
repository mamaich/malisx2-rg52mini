import struct, zlib, sys
def conv(src, dst):
    d = open(src, 'rb').read()
    tok = []; i = 2
    while len(tok) < 3:
        while d[i:i+1].isspace(): i += 1
        j = i
        while not d[j:j+1].isspace(): j += 1
        tok.append(int(d[i:j])); i = j
    i += 1
    w, h, _ = tok
    px = d[i:i + w * h * 3]
    raw = b''.join(b'\x00' + px[y*w*3:(y+1)*w*3] for y in range(h))
    def chunk(t, data):
        c = t + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
    open(dst, 'wb').write(png)
for a in sys.argv[1:]:
    conv(a, a.rsplit('.', 1)[0] + '.png'); print(a)
