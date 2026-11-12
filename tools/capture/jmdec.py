#!/usr/bin/env python3
"""Decode a malisx2 job-manager capture (MALISX2_MEASURE=csf on a G52/v7).

usage: jmdec.py jm-NNNNNNNN.bin [--fault 0xVA] [--quiet]

The capture (src/measurement/recorder_jm.c) holds the atoms of one
vkQueueSubmit and the command memory of its command buffers. This walks
every atom's job chain through the headers' Next pointers and decodes the
jobs with the v7 layouts (mesa/src/panfrost/genxml/v7.xml):

  Vertex / Compute job   Draw section at +64
  Tiler job              Primitive at +40, Draw at +128
  Write Value job        payload at +32
  Fragment job           payload at +32

For a Draw it follows the Attribute Buffer and Attribute tables (when they
are in captured memory) and prints, per attribute, the byte range the
attribute unit reads: pointer + offset + index * stride over the job's
vertex range [offset_start, offset_start + count), with the hardware's
32-bit wrap of offset + index * stride shown. With --fault, the jobs whose
reads could produce that address are marked.

Memory outside the capture (shader RSDs in the device's descriptor pool,
application buffers) is not available; such reads print as '?'.
"""
import struct
import sys

JOB_TYPES = {0: 'Not started', 1: 'Null', 2: 'Write value', 3: 'Cache flush',
             4: 'Compute', 5: 'Vertex', 6: 'Geometry', 7: 'Tiler', 8: 'Fused',
             9: 'Fragment', 10: 'Indexed Vertex'}
ATTR_TYPES = {0: 'none', 1: '1D', 2: '1D POT', 3: '1D Modulus', 4: '1D NPOT',
              5: '3D Linear', 6: '3D Interleaved', 7: '1D Prim Index', 32: 'Continuation'}
INDEX_TYPES = {0: 'none', 1: 'u8', 2: 'u16', 3: 'u32'}


class Mem:
    def __init__(self):
        self.ranges = []  # (base, bytes)

    def add(self, base, data):
        self.ranges.append((base, data))

    def read(self, addr, n):
        for base, data in self.ranges:
            if base <= addr and addr + n <= base + len(data):
                o = addr - base
                return data[o:o + n]
        return None

    def u32(self, addr):
        b = self.read(addr, 4)
        return None if b is None else struct.unpack('<I', b)[0]

    def u64(self, addr):
        b = self.read(addr, 8)
        return None if b is None else struct.unpack('<Q', b)[0]


def load(path):
    d = open(path, 'rb').read()
    magic, ver, pid, submit, hz = struct.unpack_from('<8sIIQQ', d, 0)
    if magic != b'MALIJM01':
        sys.exit('%s: not a malisx2 JM capture' % path)
    mem, atoms, cmdbufs = Mem(), [], []
    o = 32
    while o + 32 <= len(d):
        t, arg, a, b, size = struct.unpack_from('<IIQQQ', d, o)
        o += 32
        if t == 0xffffffff:
            break
        payload = d[o:o + size]
        if t == 1:
            for i in range(arg):
                atoms.append(struct.unpack_from('<8Q', payload, 64 * i))
        elif t == 2:
            mem.add(a, payload)
        elif t == 3:
            cmdbufs.append((arg,) + struct.unpack_from('<III', payload, 0))
        o += (size + 7) // 8 * 8
    return pid, submit, mem, atoms, cmdbufs


def bits(v, start, size):
    return (v >> start) & ((1 << size) - 1)


def sext32(v):
    return v - (1 << 32) if v & 0x80000000 else v


def invocation_counts(mem, inv):
    """(local x, y, z, groups x, y, z) from the packed Invocation."""
    w0 = mem.u32(inv)
    w1 = mem.u32(inv + 4)
    if w0 is None:
        return None
    sy, sz = bits(w1, 0, 5), bits(w1, 5, 5)
    gx, gy, gz = bits(w1, 10, 6), bits(w1, 16, 6), bits(w1, 22, 6)
    edges = [0, sy, sz, gx, gy, gz, 32]
    vals = []
    for i in range(6):
        lo, hi = edges[i], edges[i + 1]
        vals.append(bits(w0, lo, max(hi - lo, 0)) + 1 if hi > lo else 1)
    return vals


def decode_draw(mem, addr, vcount, fault, out):
    """The Draw section at addr. vcount: vertices the job shades (from its
    Invocation). Returns True when an attribute read can hit `fault`."""
    w = [mem.u32(addr + 4 * i) for i in range(32)]
    if w[0] is None:
        out.append('      Draw: not captured')
        return False
    q = lambda i: w[i] | (w[i + 1] << 32)
    offset_start = sext32(w[1])
    out.append('      Draw: occlusion=%d offset_start=%d instance_size=%d '
               'prim_index_base=%d' % (bits(w[0], 3, 2), offset_start, bits(w[0], 16, 8),
                                       sext32(w[2])))
    out.append('        position=0x%x ubos=0x%x textures=0x%x samplers=0x%x push=0x%x '
               'state=0x%x' % (q(4), q(6), q(8), q(10), q(12), q(14)))
    out.append('        attr_bufs=0x%x attrs=0x%x vary_bufs=0x%x varyings=0x%x '
               'viewport=0x%x occlusion=0x%x tls=0x%x' % (q(16), q(18), q(20), q(22),
                                                         q(24), q(26), q(28)))
    hit = False
    bufs = []
    ab = q(16)
    if ab:
        for i in range(64):
            lo, hi = mem.u64(ab + 16 * i), mem.u64(ab + 16 * i + 8)
            if lo is None:
                bufs.append(None)
                out.append('        attr_buf[%d]: not captured' % i)
                break
            typ = bits(lo, 0, 6)
            ptr = bits(lo, 6, 50) << 6
            stride, size = hi & 0xffffffff, hi >> 32
            bufs.append((typ, ptr, stride, size, lo, hi))
            if typ == 0 and ptr == 0:
                break
            out.append('        attr_buf[%d]: type=%s ptr=0x%x stride=%d size=%d' %
                       (i, ATTR_TYPES.get(typ, typ), ptr, stride, size))
    at = q(18)
    if at:
        for i in range(32):
            v = mem.u64(at + 8 * i)
            if v is None:
                out.append('        attr[%d]: not captured' % i)
                break
            bi, oen, fmt, off = bits(v, 0, 9), bits(v, 9, 1), bits(v, 10, 22), sext32(v >> 32)
            # A table ends where its entries stop making sense.
            if bi >= len(bufs) and fmt == 0:
                break
            line = '        attr[%d]: buf=%d offset_enable=%d format=0x%x offset=%d' % (
                i, bi, oen, fmt, off)
            if bi < len(bufs) and bufs[bi] and bufs[bi][1]:
                typ, ptr, stride, size = bufs[bi][:4]
                if typ == 1:
                    first, last = offset_start, offset_start + vcount - 1
                    # The attribute unit adds offset + index * stride to the
                    # pointer; show it both as a signed 64-bit sum and with a
                    # 32-bit wrap of the byte offset.
                    rng = []
                    for idx in (first, last):
                        boff = off + idx * stride
                        a64 = ptr + boff
                        a32 = ptr + (boff & 0xffffffff)
                        rng.append((boff, a64, a32))
                    line += ' | idx %d..%d -> bytes %d..%d' % (first, last, rng[0][0], rng[1][0])
                    if rng[0][0] < 0 or rng[1][0] >= size:
                        line += '  *** OUTSIDE size %d ***' % size
                    if fault is not None:
                        for boff, a64, a32 in rng:
                            if fault in (a64, a32) or (a32 <= fault < a32 + 16):
                                line += '  <== FAULT'
                                hit = True
                        if ptr + 0xffffffff >= fault >= ptr and (fault - ptr) & 0x80000000:
                            line += '  (fault = ptr + 0x%x: a negative 32-bit byte offset %d)' % (
                                fault - ptr, sext32(fault - ptr))
                            hit = True
            out.append(line)
    return hit


def decode_job(mem, job, fault, out):
    hdr = [mem.u32(job + 4 * i) for i in range(8)]
    if hdr[0] is None:
        out.append('  job 0x%x: not captured' % job)
        return None, False
    w4 = hdr[4]
    typ = bits(w4, 1, 7)
    nxt = hdr[6] | (hdr[7] << 32)
    out.append('  job 0x%x: %s index=%d dep1=%d dep2=%d barrier=%d next=0x%x' % (
        job, JOB_TYPES.get(typ, typ), bits(w4, 16, 16), bits(hdr[5], 0, 16),
        bits(hdr[5], 16, 16), bits(w4, 8, 1), nxt))
    hit = False
    if typ in (4, 5):
        inv = invocation_counts(mem, job + 32)
        if inv:
            out.append('    invocation: local %dx%dx%d groups %dx%dx%d' % tuple(inv))
        vcount = inv[4] if (inv and typ == 5) else (inv[3] if inv else 1)
        hit = decode_draw(mem, job + 64, vcount, fault, out)
    elif typ == 7:
        inv = invocation_counts(mem, job + 32)
        if inv:
            out.append('    invocation: local %dx%dx%d groups %dx%dx%d' % tuple(inv))
        p = [mem.u32(job + 40 + 4 * i) for i in range(6)]
        out.append('    Primitive: mode=%d index_type=%s base_vertex_offset=%d index_count=%d '
                   'indices=0x%x' % (bits(p[0], 0, 8), INDEX_TYPES.get(bits(p[0], 8, 3)),
                                     sext32(p[1]), p[3] + 1, p[4] | (p[5] << 32)))
        vcount = inv[4] if inv else 1
        hit = decode_draw(mem, job + 128, vcount, fault, out)
    elif typ == 2:
        a = mem.u64(job + 32)
        t = mem.u32(job + 40)
        v = mem.u64(job + 48)
        out.append('    Write value: addr=0x%x type=%d value=0x%x' % (a, t, v))
    elif typ == 9:
        w = [mem.u32(job + 32 + 4 * i) for i in range(4)]
        out.append('    Fragment: bound (%d,%d)-(%d,%d) fbd=0x%x' % (
            bits(w[0], 0, 12), bits(w[0], 16, 12), bits(w[1], 0, 12), bits(w[1], 16, 12),
            w[2] | (w[3] << 32)))
    return nxt, hit


def main():
    args = sys.argv[1:]
    fault = None
    quiet = '--quiet' in args
    if '--fault' in args:
        fault = int(args[args.index('--fault') + 1], 0)
    paths = [a for a in args if a.endswith('.bin')]
    for path in paths:
        pid, submit, mem, atoms, cmdbufs = load(path)
        print('%s: GPU 0x%x, submit %d, %d atoms, command buffers %s' % (
            path, pid, submit, len(atoms),
            ', '.join('#%d: %d dispatches %d draws %d passes' % c for c in cmdbufs)))
        for n, a in enumerate(atoms):
            jc = a[1]
            print('atom %d: jc=0x%x words %s' % (n, jc, ' '.join('%x' % x for x in a)))
            job, seen = jc, set()
            while job and job not in seen:
                seen.add(job)
                out = []
                job_next, hit = decode_job(mem, job, fault, out)
                if hit:
                    out[0] += '   <<<<<<<<<< can produce the fault address'
                if not quiet or hit:
                    print('\n'.join(out))
                job = job_next


if __name__ == '__main__':
    main()
