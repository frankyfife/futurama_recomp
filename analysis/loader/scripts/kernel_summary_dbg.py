import json, collections, sys
d = json.load(open('kernel_raw.json'))
names = {t['slot']: t['name'] for t in d['thunks']}
ords = {t['slot']: t['ordinal'] for t in d['thunks']}
sites = collections.defaultdict(list)
for s in d['sites']: sites[s['slot']].append(s)
refs = collections.defaultdict(list)
for r in d['refs']: refs[r['slot']].append(r)
# check every ref is covered
site_addrs = {s['site'] for s in d['sites']}
dr_addrs = {r['ins'] for r in d['datarefs']}
stub_addrs = {int(a) for a in d['stubs']}
unc = [r for r in d['refs'] if r['ins'] not in site_addrs and r['ins'] not in dr_addrs and r['func'] not in stub_addrs and r['ins'] not in stub_addrs]
print('uncovered refs', [(hex(r['ins']), names[r['slot']], r['mnem'], r['ops']) for r in unc])
for slot in range(len(names)):
    ss = sites[slot]
    pi = d['purge_init'].get(str(slot)); pf = d['purge'].get(str(slot))
    prof = d['profiles'].get(str(slot))
    pc = collections.Counter('%d/%s' % (s['pushes'], s['pend']) for s in ss)
    cl = collections.Counter(s['cleanup'] for s in ss)
    ecx = sum(s['ecx_set'] for s in ss); edx = sum(s['edx_set'] for s in ss)
    eax = collections.Counter(str(s['eax_used']) for s in ss)
    profs = ''
    if prof:
        best = min(prof.values()); profs = ' '.join('%s:%d' % (int(k)//4, v) for k, v in prof.items() if v <= best + 3)
    print('%3d %3x %-30s refs=%3d sites=%3d init=%s fin=%s pushes=%s cleanup=%s ecx=%d edx=%d eax=%s | %s' % (
        slot, ords[slot], names[slot], len(refs[slot]), len(ss), pi, pf, dict(pc), dict(cl), ecx, edx, dict(eax), profs))
