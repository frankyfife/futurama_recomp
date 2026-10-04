"""Fact 2: thread creation sites, priority imports, CreateThread callers (raw + absolute refs)."""
from verify_startup_common import *
print("imported ordinals:", sorted(TH))
for o in (143, 148, 254, 255, 258, 126, 127, 156, 189, 99, 234, 233):
    print("ord %d imported: %s" % (o, o in TH), ("slot %d" % TH.index(o)) if o in TH else "")
s = TH.index(255)
print("PsCreateSystemThreadEx call sites:", [hex(a) for a in kcall_sites(s)[0]], "other refs:", [hex(a) for a in kcall_sites(s)[1]])
for t in (0xf9a72,):
    print("CreateThread raw E8/E9:", [(hex(a), k) for a, k in raw_calls(t)])
    print("CreateThread abs refs (all sections):", [(n, hex(a)) for n, a in abs_refs(t)])
print("abs refs to 0xf8469:", [(n, hex(a)) for n, a in abs_refs(0xf8469)])
print("abs refs to 0xf99da:", [(n, hex(a)) for n, a in abs_refs(0xf99da)])
# Any function named *Thread* in functions.tsv
for a, sz, nm in funcs():
    if re.search(r"Thread|Fiber", nm): print("  func %08x %5d %s" % (a, sz, nm))
