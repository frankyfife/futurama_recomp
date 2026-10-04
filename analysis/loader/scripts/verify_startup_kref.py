"""List every reference to a kernel thunk slot (call [slot], jmp [slot], mov reg,[slot], push [slot]) with the
preceding instructions. usage: verify_startup_kref.py <ordinal> [...]   (ORDINAL, not slot)"""
from verify_startup_common import *
for o in map(lambda s: int(s, 0), sys.argv[1:]):
    s = TH.index(o)
    slotaddr = THUNK + 4 * s
    print("=== ord %d slot %d (%#x)" % (o, s, slotaddr))
    for sec, va in abs_refs(slotaddr, CODE):
        ins_addr = None
        # find instruction containing the reference
        pre, post = [], []
        for back in range(1, 4):
            p, q = back_dis(va - back, 0x40, 12)
            if q and q[0].address == va - back and ("0x%x" % slotaddr) in q[0].op_str:
                pre, post = p, q; break
        f = func_of(va)
        print("  ref %08x in %s" % (va, ("%x %s" % (f[0], f[2])) if f else "<no func>"))
        for i in pre[-7:] + post[:2]:
            print("     %08x %s %s" % (i.address, i.mnemonic, i.op_str))
