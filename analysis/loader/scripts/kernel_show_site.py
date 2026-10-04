"""Print disassembly before/after given kernel call sites (evidence helper).
usage: python kernel_show_site.py <ImportName|0xaddr> [n_before] [n_after]"""
import json, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kernel_common import *
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
md = Cs(CS_ARCH_X86, CS_MODE_32)
x = load_xbe(); F = Functions()
d = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'kernel_raw.json')))
names = {t['slot']: t['name'] for t in d['thunks']}
fn = d['func_names']
key = sys.argv[1]
nb = int(sys.argv[2]) if len(sys.argv) > 2 else 14
na = int(sys.argv[3]) if len(sys.argv) > 3 else 3
lim = int(sys.argv[4]) if len(sys.argv) > 4 else 3
sel = [s for s in d['sites'] if names[s['slot']] == key or (key.startswith('0x') and s['site'] == int(key, 16))]
for s in sel[:lim]:
    f = s['func']
    print('=== site %06x %s in %s (%06x) via %s pushes=%s pend=%s depth=%s next=%s' % (
        s['site'], names[s['slot']], fn.get(str(f)), f, s['via'], s['pushes'], s['pend'], s['depth'], s['next']))
    # linear decode from function start (or 0x300 before) to site
    start = f if 0 <= s['site'] - f < 0x2000 else s['site'] - 0x100
    lines = []
    a = start
    while a < s['site'] + 0x40:
        ins = next(md.disasm(x.read(a, 16), a), None)
        if ins is None: break
        lines.append(ins); a += ins.size
    idx = [i for i, l in enumerate(lines) if l.address == s['site']]
    if not idx:
        print('  (linear decode out of sync)'); continue
    i = idx[0]
    for l in lines[max(0, i - nb): i + na + 1]:
        print('  %06x %-16s %s %s' % (l.address, l.bytes.hex(), l.mnemonic, l.op_str))
