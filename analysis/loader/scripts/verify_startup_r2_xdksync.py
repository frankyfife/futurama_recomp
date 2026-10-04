"""Review 2: per-section counts of DPC/timer/IRQL/wait kernel call sites (table 4.4 of startup.md)."""
from verify_startup_r2_common import *
import collections
ords = {107: "KeInitializeDpc", 113: "KeInitializeTimerEx", 149: "KeSetTimer", 97: "KeCancelTimer", 129: "KeRaiseIrqlToDpcLevel",
        161: "KfLowerIrql", 160: "KfRaiseIrql", 145: "KeSetEvent", 159: "KeWaitForSingleObject", 119: "KeInsertQueueDpc",
        98: "KeConnectInterrupt", 109: "KeInitializeInterrupt", 44: "HalGetInterruptVector", 153: "KeSynchronizeExecution", 225: "NtSetEvent"}
for o, nm in ords.items():
    s = slot_of_ord(o)
    if s is None:
        print(o, nm, "not imported"); continue
    c, oth = kcalls(s)
    cnt = collections.Counter(X.section(a) for a in c)
    cnt2 = collections.Counter(X.section(a) for a in oth)
    print("ord %3d %-24s (ghidra %s) call sites per section %s ; other refs (mov reg,[slot] etc.) %s" % (o, nm, KNAME.get(s), dict(cnt), dict(cnt2)))
