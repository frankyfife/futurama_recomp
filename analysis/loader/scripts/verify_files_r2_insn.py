"""Check instruction-level claims of files.md: decode the instruction at each quoted address and
compare with the claimed mnemonic/operand substring. Prints OK/MISMATCH."""
from verify_files_r2_common import *

CLAIMS = [
    # key fact 3
    (0x13F3B2, "call", "0x13fe10", "Archivsuche FUN_0013FE10"),
    (0x13F3F0, "push", "0x68000000", "Flags OVERLAPPED|NO_BUFFERING|SEQ_SCAN"),
    (0x13F404, "call", "0xf76e0", "CreateFileA"),
    # key fact 4
    (0x13F85F, "and", "0xfffff800", "Laenge & ~0x7ff"),
    (0x13F8CA, "and", "0xfffff800", "Seek & ~0x7ff"),
    (0x13F847, "lea", "0x7ff", "Aufrunden"),
    (0x13F792, "and", "0xfffff800", "Ringwechsel"),
    # key fact 5
    (0xF7625, "push", "[0x2935fc]", "NtUserIoApcDispatcher"),
    (0xF7630, "call", "[0x2935e8]", "NtReadFile"),
    # key fact 8
    (0xA26BD, "mov", "0xe8", "game+0xE8 (Bericht: cmp)"),
    (0xA26C8, "cmp", "eax, ebx", "cmp mit 1"),
    (0xA2707, "mov", "0x2e7b70", "NTSC-Suffix"),
    (0xA2744, "mov", "0x4c4150", "PAL-Suffix"),
    (0xA26E9, "mov", "0x42700000", "60.0f"),
    (0xA271A, "mov", "0x42480000", "50.0f"),
    (0xA2434, "call", "0x166f54", "sprintf %s-%s"),
    # key fact 14
    (0x20EFC0, None, None, "C++-Init XGetDiskClusterSize"),
    (0x20EFDD, "idiv", "ecx", "Division durch Clustergroesse"),
    # key fact 15
    (0xE0CB1, "mov", "edx, dword ptr [esp + 0x10]", "high"),
    (0xE0CB5, "or", "edx, dword ptr [esp + 0xc]", "high|low"),
    (0xE0CD3, "mov", "[0x2edb88]", "Summe laden (Bericht nannte 0xE0CD9 = cmp edx, ecx)"),
    (0xE0CD9, "cmp", "edx, ecx", "Vergleich frei vs. Summe"),
    # key fact 16
    (0xF934C, None, None, "IoCreateSymbolicLink D:"),
    (0xF9351, "call", "[0x293620]", "IoCreateSymbolicLink"),
    (0xF93CC, "call", "0xf7f7c", "XMountUtilityDrive"),
    (0xF93E8, None, None, "GameRatings-Vergleich"),
    (0xF77DD, None, "0x40", "FILE_NON_DIRECTORY_FILE"),
    (0xF7745, "call", "[0x29356c]", "RtlInitAnsiString in CreateFileA"),
    (0xF7894, None, None, "MmAllocateSystemMemory in CopyFileEx"),
    (0xF64E9, "call", "[0x2935b4]", "KeDelayExecutionThread in SleepEx"),
    (0xF80E5, "push", "esi", "Registersicherung"),
    (0xF75A6, None, None, "FlushFileBuffers push"),
    (0xF7418, None, None, "SetFilePointer FILE_END"),
    (0x147618, "mov", "[0x30edd4]", "NiTCache-Bedingung (Bericht: 0x147619, kein Befehlsanfang)"),
    (0x264706, "mov", "[esi + 0x7c]", "XMV close prueft Handle +0x7C"),
    (0xA28DB, "mov", "[esi + 0xe8]", "Sprachwahl in FUN_000A26A0 nur bei +0xE8==0"),
    (0xA28F3, "call", "0xd9990", "zweiter Aufruf von FUN_000D9990 (XGetLanguage)"),
    (0xE0034, "call", "0xf64be", "SleepEx(0,TRUE) in Save-vf14 0xE0030"),
    (0xE0030, "push", "1", "Alertable=TRUE"),
    (0xF8531, "call", "0xf9a72", "einziger CreateThread-Aufruf (entry)"),
    (0xF931E, "jmp", None, "Ergebnis DVD-Auth verworfen"),
    (0xF9326, None, None, "InitFlags & 8"),
    (0xF933D, None, None, "HardwareInfo Flags & 8"),
    (0xF9375, None, None, "InitFlags & 0x18"),
    (0xF93B3, None, None, "InitFlags & 8 / & 1"),
]


def at(va):
    lst = list(md.disasm(X.read(va, 16), va, count=1))
    return lst[0] if lst else None


bad = 0
for va, mn, sub, what in CLAIMS:
    i = at(va)
    if i is None:
        print("%08x  UNDECODABLE  (%s)" % (va, what)); bad += 1; continue
    ok = (mn is None or i.mnemonic == mn) and (sub is None or sub in i.op_str)
    # also check that this address is an instruction boundary inside its function
    f = owner(va)
    boundary = "?"
    if f and not f[2].endswith("+out"):
        boundary = "yes" if any(j.address == va for j in dis(f[0], f[0] + f[1])) else "NO"
    print("%08x  %-8s %-40s %-6s boundary=%s  [%s]" % (va, i.mnemonic, i.op_str[:40], "OK" if ok else "MISMATCH", boundary, what))
    if not ok:
        bad += 1
print("mismatches:", bad)
