#!/usr/bin/env python3
# On the PC: profcmp.py <new.proftext> <old.proftext> - how much of an old PGO profile still applies
# to the code a new profile was trained on. Text profiles: llvm-profdata-19 merge --text -o x.proftext
# x.profdata. For each function of the new profile, whether the old one has it with the same hash
# (usable on this code) or not (lost: changed, new or renamed); a function's weight is the sum of
# its counters in the new profile. Then the two profiles' training volume by area (compiler, driver,
# util, other) on the functions they share unchanged, and a few compile entry points.
import sys
from collections import defaultdict


def parse(path):
    funcs = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        lines = [l.rstrip('\n') for l in f]
    i = 0
    n = len(lines)
    while i < n:
        l = lines[i]
        if not l or l.startswith('#') or l.startswith(':'):
            i += 1
            continue
        name = l
        # expect "# Func Hash:" next
        if i + 1 < n and lines[i + 1].startswith('# Func Hash'):
            h = int(lines[i + 2])
            nc = int(lines[i + 4])
            vals = [int(x) for x in lines[i + 6:i + 6 + nc]]
            funcs[name] = (h, vals)
            i = i + 6 + nc
            # skip value profile sections until blank line
            while i < n and lines[i] != '':
                i += 1
        else:
            i += 1
    return funcs


def area(name):
    path = name.split(';')[0] if ';' in name else ''
    fn = name.split(';')[-1]
    if 'compiler' in path or 'panfrost/compiler' in path or fn.startswith(('bi_', 'nir_', 'va_', 'pan_nir', 'glsl', 'spirv', 'vtn_', 'bifrost')):
        return 'compiler'
    if 'mesa/src/util' in path or fn.startswith(('util_', '_mesa_hash', 'ralloc', 'u_', 'hash_table', '_mesa_set')):
        return 'util'
    if 'src/vk' in path or fn.startswith(('mali_', 'vk_')):
        return 'driver'
    return 'other'


new = parse(sys.argv[1])
old = parse(sys.argv[2])
tot_new = sum(sum(v) for _, v in new.values())
lost = []
kept_w = 0
for name, (h, v) in new.items():
    w = sum(v)
    o = old.get(name)
    if o is None or o[0] != h:
        lost.append((w, name, 'new' if o is None else 'changed'))
    else:
        kept_w += w
lost.sort(reverse=True)
lw = sum(w for w, _, _ in lost)
print(f'new profile: {len(new)} functions, total weight {tot_new}')
print(f'old profile: {len(old)} functions')
print(f'old profile on this code: {len([1 for w, _, _ in lost if w > 0])} weighted functions lost, '
      f'{100.0 * lw / tot_new:.1f} % of the weight')
for w, name, why in lost[:15]:
    print(f'  {100.0 * w / tot_new:5.2f} %  {why:7s} {name}')

# training volume per area: total counts in each profile (functions with same hash only)
by_new = defaultdict(int)
by_old = defaultdict(int)
for name, (h, v) in new.items():
    o = old.get(name)
    if o is not None and o[0] == h:
        by_new[area(name)] += sum(v)
        by_old[area(name)] += sum(o[1])
print('training volume on unchanged functions (new / old counts):')
for a in sorted(set(by_new) | set(by_old)):
    r = by_new[a] / by_old[a] if by_old[a] else float('inf')
    print(f'  {a:9s} new {by_new[a]:>14d}  old {by_old[a]:>14d}  ratio {r:.2f}')

# entry counts of a few compile entry points
for key in ['bi_allocate_registers', 'bi_compile_variant', 'nir_opt_algebraic', 'mali_shader_compile',
            'bifrost_compile_shader_nir', 'bi_schedule', 'vk_common_CreateGraphicsPipelines']:
    for name in list(new):
        if name.split(';')[-1] == key:
            hn, vn = new[name]
            o = old.get(name)
            print(f'  {key}: entry new {vn[0] if vn else 0}, old {o[1][0] if o and o[1] else "-"}'
                  f'{"" if o and o[0] == hn else " (hash differs)"}')
