"""Reproduces the ad-hoc checks of the startup.md review (each section prints the evidence used in '## Verifikation').
usage: python verify_startup_misc.py [section ...]   (no argument = all sections)"""
from verify_startup_common import *

def callers(a):
    return [(hex(c), k, (func_of(c)[2] if func_of(c) else "<nofunc>")) for c, k in raw_calls(a)]

def s_stub():
    print("# main thread stub 0xF8469 and entry 0xF84DD"); pdis(0xf8469, 0x74); pdis(0xf84dd, 0x72, True)
    print("# CreateThread 0xF9A72"); pdis(0xf9a72, 0x6e, True)
    print("# XapiThreadStartup 0xF99DA (ends with ord 258 + int3)"); pdis(0xf99da, 0x98)
def s_xinit():
    print("# XapiInitProcess 0xF92C0..0xF946D (BootToDash argument paths)"); pdis(0xf92c0, 0x1ad)
    print("# XapiBootToDash"); print(decomp(0xf8fca))
def s_loop():
    print("# Game::Frame fixed-step core"); pdis(0xa2ce8, 0xa4)
    print("# FUN_000a46d0 (period: counter > 0x28 / > 200)"); print(decomp(0xa46d0))
    print("# CApp::Init"); print(decomp(0xd26a0))
    print("# FUN_0002a660 (context switch, 8x ClearBuffer+DisplayFrame)"); print(decomp(0x2a660))
def s_timer():
    print("# QPC callers", callers(0xf641b)); print("# QPF callers", callers(0xf642c))
    for a in (0x260bc0, 0x2744e0):
        o = X.va2off(a); print("# raw bytes around suspected rdtsc %#x:" % a, X.b[o:o + 0x50].hex())
    print("# D3D rdtsc helper 0x23B780"); pdis(0x23b780, 3); print("  callers", callers(0x23b780))
def s_sleep():
    for n in ("XAPILIB::SleepEx", "XAPILIB::Sleep", "XAPILIB::WaitForSingleObject", "XAPILIB::WaitForSingleObjectEx", "XAPILIB::GetOverlappedResult"):
        a = [x for x in funcs() if x[2] == n][0][0]; print("#", n, hex(a), callers(a))
    print("# SleepEx wrapper 0xE0030 + vtable entry"); pdis(0xe0030, 0xc, True); print("  abs refs", [(n, hex(a)) for n, a in abs_refs(0xe0030)])
    print("  strings before vtable:", repr(X.cstr(0x2a4d80)), repr(X.cstr(0x2a4d94)))
    print("# File::Poll 0x13F590 callers", callers(0x13f590))
def s_cinit_xdk():
    print("# XDK static initializers inside the C++ init table")
    for f in (0x28d254, 0x235770, 0x24968f, 0x24969a, 0x24f3d4, 0x24f3df):
        print("  slot", [hex(i) for i in range(0x2bdef0, 0x2d5b30, 4) if X.u32(i) == f]); pdis(f, 0x40, True)
def s_dash():
    print("# FUN_000e4b00 (memory dashboard)"); pdis(0xe4b00, 0x48, True)
    print("# FUN_000f8081 = cluster size (product of FILE_FS_SIZE_INFORMATION +0x10 * +0x14)"); print(decomp(0xf8081))
def s_exit():
    print("# _atexit callers:", len(raw_calls(0x166f42)))
    print("# _amsg_exit 0x1692B4 -> [0x2D86AC] =", hex(X.u32(0x2d86ac))); pdis(0x1692b4, 0x1c, True)
    print("# doexit tail with KeBugCheck(0)"); pdis(0x169fb0, 0x26)
    print("# doexit callers", callers(0x169f1b), " _exit 0x169FC5 refs", [(n, hex(a)) for n, a in abs_refs(0x169fc5)])
def s_misc():
    print("# getptd variant 0x169352 (fs:[0x24]/[0x28]/[4])"); pdis(0x169352, 0x3a); print("  callers", callers(0x169352))
    print("# DSOUND fs:[0x58] before KeSaveFloatingPointState"); pdis(0x246d7a, 0x25, True)
    print("# hotfix FUN_000f9804"); print(decomp(0xf9804))
    print("# shutdown routine 0xF9C7F + DPC 0xF9B08 + XAutoPowerDownResetTimer"); pdis(0xf9c7f, 0x3e, True); pdis(0xf9b08, 0x12, True); pdis(0xf9aed, 0x1b, True)
    print("# DisplayFrame 0x146460"); pdis(0x146460, 0x29, True)
    print("# XGetLanguage"); print(decomp(0xf6440))
    print("# XCalculateSignatureEnd -> FUN_000f6ffa: XboxHDKey (ord 323) zero test, XQueryValue(0xFFFF)"); pdis(0xf6ffa, 0x40)
SECTIONS = dict(stub=s_stub, xinit=s_xinit, loop=s_loop, timer=s_timer, sleep=s_sleep, cinit_xdk=s_cinit_xdk,
                dash=s_dash, exit=s_exit, misc=s_misc)
if __name__ == "__main__":
    for k in (sys.argv[1:] or SECTIONS):
        print("\n==================== %s" % k); SECTIONS[k]()
