#!/usr/bin/env python3
# pdsum.py <pandecode dump>... - what a pandecode dump of a v7 job-manager trace (mtrace) is made
# of, per dump: job types, framebuffer parameters, render target and ZS writeback formats, tiler
# hierarchy masks, draw state bits (FPK, early ZS), texture layouts, and the unique shaders with
# their instruction and clause counts. For comparing Arm's driver with malisx2 on the same scene.
import re
import sys
from collections import Counter, OrderedDict

FB_FIELDS = ['Effective Tile Size', 'Color Buffer Allocation', 'Render Target Count', 'Z Internal Format',
             'Z Write Enable', 'Z Preload Enable', 'Z Unload Enable', 'S Write Enable', 'S Preload Enable',
             'CRC Read Enable', 'CRC Write Enable', 'Has ZS CRC Extension', 'Pre Frame 0', 'Pre Frame 1',
             'Post Frame', 'Empty Tile Read Enable', 'Empty Tile Write Enable', 'Sample Count']
RT_FIELDS = ['Internal Format', 'Writeback Format', 'Writeback Block Format', 'Write Enable',
             'Clean Tile Write Enable', 'sRGB', 'Dithering Enable']
ZS_FIELDS = ['Write Format', 'Block Format', 'Clean Tile Write Enable']
ST_FIELDS = ['Allow forward pixel to kill', 'Allow forward pixel to be killed', 'Pixel kill operation',
             'ZS update operation', 'Shader register allocation', 'Depth function', 'Depth write mask',
             'Overdraw alpha0', 'Overdraw alpha1', 'Alpha-to-coverage', 'Shader modifies coverage',
             'Depth source']
TEX_FIELDS = ['Format (v7)', 'Texel ordering', 'Levels', 'Dimension']
TILER_FIELDS = ['Hierarchy Mask']
JOB_FIELDS = ['Type', 'Barrier', 'Invalidate Cache', 'Suppress Prefetch', 'Enable Texture Mapper']
DRAW_FIELDS = ['Four Components Per Vertex', 'Draw Descriptor Is 64b', 'Allow Forward Pixel To Kill',
               'Allow Forward Pixel To Be Killed', 'Pixel Kill Operation', 'ZS Update Operation',
               'Primitive Barrier', 'Occlusion Query', 'Front Face CCW', 'Cull Front Face', 'Cull Back Face']
PRIM_FIELDS = ['Draw Mode', 'Index Type', 'Primitive Restart', 'First Provoking Vertex', 'Job Task Split']


def summarize(path):
    lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
    blocks = {k: Counter() for k in ['job', 'fb', 'rt', 'zs', 'state', 'tex', 'tiler', 'draw', 'prim']}
    nblocks = Counter()
    shaders = OrderedDict()   # gpu va -> dict
    cur = None                # (kind, fieldset, depth)
    shader = None
    i = 0
    n = len(lines)
    while i < n:
        ln = lines[i]
        s = ln.strip()
        ind = len(ln) - len(ln.lstrip(' '))
        m = re.match(r'^Shader 0x[0-9a-f]+ \(GPU VA ([0-9a-f]+)\) sz (\d+)', ln)
        if m:
            va, sz = m.group(1), int(m.group(2))
            shader = shaders.setdefault(va, {'size': sz, 'instr': 0, 'nop': 0, 'clauses': 0, 'uses': 0})
            shader['uses'] += 1
            first = shader['instr'] == 0 and shader['clauses'] == 0
            i += 1
            while i < n:
                ln = lines[i]
                if ln and not ln[0].isspace() and not ln.startswith(('clause_', '#', 'ds(', '{', '}')):
                    break
                if first:
                    if ln.startswith('clause_'):
                        shader['clauses'] += 1
                    st = ln.lstrip()
                    if st.startswith(('*', '+')):
                        shader['instr'] += 1
                        if st[1:].startswith('NOP'):
                            shader['nop'] += 1
                i += 1
            cur = None
            continue
        if ln and not ln[0].isspace():
            cur = None
            if ln.startswith('Job Header'):
                cur = ('job', JOB_FIELDS, 2)
            elif ln.startswith('Parameters:'):
                cur = ('fb', FB_FIELDS, 2)
            elif ln.startswith('Color Render Targets'):
                cur = ('rt', RT_FIELDS, 4)
            elif ln.startswith('ZS:'):
                cur = ('zs', ZS_FIELDS, 2)
            elif ln.startswith('State:'):
                cur = ('state', ST_FIELDS, 4)
            elif ln.startswith('Textures'):
                cur = ('tex', TEX_FIELDS, 6)
            elif ln.startswith('Primitive:'):
                cur = ('prim', PRIM_FIELDS, 2)
            elif ln.startswith('Fragment:') or ln.startswith('Draw:'):
                cur = ('draw', DRAW_FIELDS, 2)
            if cur:
                nblocks[cur[0]] += 1
        elif ind >= 2 and ln.startswith('  Tiler Context @'):
            cur = ('tiler', TILER_FIELDS, 4)
            nblocks['tiler'] += 1
        elif cur and ':' in s:
            key, _, val = s.partition(':')
            key = key.strip()
            val = val.strip()
            kind, fields, depth = cur
            if key in fields:
                blocks[kind][(key, val)] += 1
            elif kind == 'rt' and key.endswith('Color Render Target 0') or (kind == 'rt' and re.match(r'.*Color Render Target \d+$', key)):
                blocks[kind][('kind', key.split(' Color')[0])] += 1
            elif kind == 'tex' and key == 'Width':
                blocks[kind][('width', val)] += 1
        i += 1
    return nblocks, blocks, shaders


def main():
    for path in sys.argv[1:]:
        nblocks, blocks, shaders = summarize(path)
        print(f'===== {path}')
        print('blocks:', dict(nblocks))
        for kind in ['job', 'fb', 'rt', 'zs', 'tiler', 'state', 'draw', 'prim', 'tex']:
            c = blocks[kind]
            if not c:
                continue
            print(f'-- {kind} ({nblocks[kind]} blocks)')
            byfield = OrderedDict()
            for (k, v), cnt in c.items():
                byfield.setdefault(k, []).append((cnt, v))
            for k, vs in byfield.items():
                vs.sort(reverse=True)
                print(f'   {k}: ' + ', '.join(f'{v} x{cnt}' for cnt, v in vs[:8]))
        tot_i = sum(s['instr'] for s in shaders.values())
        tot_c = sum(s['clauses'] for s in shaders.values())
        tot_n = sum(s['nop'] for s in shaders.values())
        print(f'-- shaders: {len(shaders)} unique, {tot_i} instrs ({tot_n} NOP), {tot_c} clauses; '
              f'uses {sum(s["uses"] for s in shaders.values())}')
        big = sorted(shaders.items(), key=lambda kv: -kv[1]['instr'])[:12]
        for va, s in big:
            print(f'   {va}: {s["instr"]:4d} instr {s["nop"]:3d} nop {s["clauses"]:3d} clauses, {s["size"]:5d} B, used {s["uses"]}')


main()
