"""Review 2: further claims - XapiThreadStartup, ExQueryNonVolatileSetting indices, doexit/_exit reachability,
critical-section users, NtCreateEvent/NtWaitForSingleObject, LocalAlloc callers, File::Read clamp, imports list."""
import sys
from verify_startup_r2_common import *
from verify_startup_r2_common import _FUNCS
sec = sys.argv[1:] or ["thread", "nv", "exit", "sync", "local", "fileread", "imports"]

if "thread" in sec:
    print("== XapiThreadStartup 0xF99DA")
    pdis(0xF99DA, 0x98)
if "nv" in sec:
    print("\n== ExQueryNonVolatileSetting stub 0xF9CC6 callers with ValueIndex (last push before call)")
    for a, k in rawrefs(0xF9CC6):
        b = before(a, 6)
        pushes = [i for i in b if i.mnemonic == "push"]
        print("   0x%X %s  first-arg push: %s" % (a, fdesc(a), (pushes[-1].op_str if pushes else "?")))
    s = slot_of_ord(24)
    print("   direct thunk calls to ord 24:", [hex(x) for x in kcalls(s)[0]], kcalls(s)[1])
if "exit" in sec:
    print("\n== doexit 0x169F1B / _exit 0x169FC5 / _amsg_exit 0x1692B4 / _cexit / exit")
    for t in (0x169F1B, 0x169FC5, 0x1692B4):
        print("   0x%X %s raw callers %s abs %s" % (t, FNAME.get(t), [(hex(a), fdesc(a)) for a, k in rawrefs(t)], [(n, hex(v)) for n, v in absrefs(t)]))
    print("   [0x2D86AC] =", hex(X.u32(0x2D86AC)))
    for a, s_, n in _FUNCS:
        if n in ("_exit", "exit", "_cexit", "__cexit", "_c_exit", "__exit", "_doexit", "doexit"):
            print("   named", hex(a), n, [(hex(c), fdesc(c)) for c, k in rawrefs(a)])
    pdis(0x169FB0, 0x20)
if "sync" in sec:
    print("\n== sync imports")
    for o, nm in ((189, "NtCreateEvent"), (233, "NtWaitForSingleObject"), (234, "NtWaitForSingleObjectEx"), (159, "KeWaitForSingleObject"),
                  (277, "RtlEnterCriticalSection"), (294, "RtlLeaveCriticalSection"), (291, "RtlInitializeCriticalSection"), (99, "KeDelayExecutionThread"),
                  (192, "NtCreateSemaphore"), (113, "KeInitializeTimerEx"), (149, "KeSetTimer"), (107, "KeInitializeDpc"), (145, "KeSetEvent")):
        s = slot_of_ord(o)
        if s is None:
            print("   ord %d (%s, Vermutung Name) not imported" % (o, nm)); continue
        c, oth = kcalls(s)
        where = sorted({"%s[%s]" % (fdesc(a), X.section(a)) for a in c})
        print("   ord %d slot %d ghidra=%s: %d sites; %s; other %s" % (o, s, KNAME.get(s), len(c), where, [hex(x) for x in oth]))
if "local" in sec:
    print("\n== LocalAlloc 0xF63E0 callers")
    for a, k in rawrefs(0xF63E0):
        print("   0x%X %s [%s]" % (a, fdesc(a), X.section(a)))
    print("== HeapAlloc-like FUN_000f52cc raw callers")
    for a, k in rawrefs(0xF52CC):
        print("   0x%X %s [%s]" % (a, fdesc(a), X.section(a)))
if "fileread" in sec:
    print("\n== FUN_0013f7b0 (read size clamp)")
    pdis(0x13F7B0, 0x80)
if "imports" in sec:
    print("\n== full thunk table (slot ord ghidra-name)")
    for i, o in enumerate(TH):
        print("   %3d %3d %s" % (i, o, KNAME.get(i, "-")))
