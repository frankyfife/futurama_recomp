"""Every caller of the ExQueryNonVolatileSetting jmp-stub 0xF9CC6 with the pushed ValueIndex (first argument =
last push before the call)."""
from verify_startup_common import *
for site, kind in raw_calls(0xf9cc6):
    pre, post = back_dis(site, 0x40, 6)
    f = func_of(site)
    pushes = [i for i in pre if i.mnemonic == "push"]
    print("%08x %-4s %-40s last pushes: %s" % (site, kind, ("%x %s" % (f[0], f[2])) if f else "<no func>",
          "; ".join("%x:%s" % (i.address, i.op_str) for i in pushes[-5:])))
