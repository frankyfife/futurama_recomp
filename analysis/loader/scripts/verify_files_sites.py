"""Assert the concrete instruction sites cited in files.md (key facts 3-5, 8, 12, 14-16 and section 2.4/4.4):
decode the instruction at each address and compare mnemonic/operands with the claim. Prints OK/MISMATCH per site."""
from verify_files_common import *
SITES = [
    (0x13F2B3, "mov", "cl, byte ptr [eax + 0x2d7ad8]", "prefix 'D:\\' copy"),
    (0x13F3B2, "call", "0x13fe10", "archive lookup before loose open"),
    (0x13F3F0, "push", "0x68000000", "OVERLAPPED|NO_BUFFERING|SEQUENTIAL_SCAN"),
    (0x13F3F5, "push", "3", "OPEN_EXISTING"),
    (0x13F3F8, "push", "1", "FILE_SHARE_READ"),
    (0x13F3FA, "push", "0x80000000", "GENERIC_READ"),
    (0x13F404, "call", "0xf76e0", "CreateFileA"),
    (0x13F68C, "push", "0x13f1d0", "completion routine"),
    (0x13F6A0, "call", "0xf7603", "ReadFileEx"),
    (0x13F792, "and", "eax, 0xfffff800", "ring change aligns position"),
    (0x13F847, "lea", "eax, [ecx + 0x7ff]", "last chunk rounded up"),
    (0x13F85F, "and", "eax, 0xfffff800", "chunk length aligned"),
    (0x13F8CA, "and", "edx, 0xfffff800", "seek aligns position"),
    (0x13F200, "ret", "0xc", "completion stdcall 3 args"),
    (0x13F59E, "call", "0xf64be", "SleepEx (alertable)"),
    (0x13F514, "call", "0xf64be", "SleepEx in destructor"),
    (0xF7625, "push", "dword ptr [0x2935fc]", "ApcRoutine = NtUserIoApcDispatcher slot"),
    (0xF7630, "call", "dword ptr [0x2935e8]", "NtReadFile"),
    (0xF7676, "push", "dword ptr [0x2935fc]", "WriteFileEx ApcRoutine"),
    (0xF7681, "call", "dword ptr [0x2935ec]", "NtWriteFile"),
    (0xF77BB, "mov", "dword ptr [ebp - 0x20], 0xfffffffd", "RootDirectory = -3"),
    (0xF77C2, "mov", "dword ptr [ebp - 0x18], 0x40", "OBJ_CASE_INSENSITIVE"),
    (0xF7868, "ret", "0x1c", "CreateFileA 7 args"),
    (0xF64E7, "push", "1", "SleepEx: KeDelayExecutionThread WaitMode=UserMode"),
    (0xA26BD, "mov", "eax, dword ptr [esi + 0xe8]", "PAL/NTSC flag"),
    (0xA26E9, "mov", "dword ptr [0x2e7b6c], 0x42700000", "60.0f"),
    (0xA271A, "mov", "dword ptr [0x2e7b6c], 0x42480000", "50.0f"),
    (0xA2744, "mov", "dword ptr [0x2e7b70], 0x4c4150", "'PAL'"),
    (0x20EFC0, "push", "0x2a4d80", "'U:\\'"),
    (0x20EFC5, "call", "0xf8081", "XGetDiskClusterSizeA"),
    (0x20EFDD, "idiv", "ecx", "division by cluster size"),
    (0xE0CA8, "call", "0xf7c5d", "GetDiskFreeSpaceExA"),
    (0xE0CB1, "mov", "edx, dword ptr [esp + 0x10]", "free bytes high"),
    (0xE0CB5, "or", "edx, dword ptr [esp + 0xc]", "high|low bug"),
    (0xE0CD9, "cmp", "edx, ecx", "compare with total need 0x2edb88"),
    (0xE0D65, "cmp", "eax, 0x19", "25 saves = full"),
    (0xE0364, "call", "0xf7163", "XCalculateSignatureBegin(0) verify"),
    (0xE03C6, "call", "0xf7163", "XCalculateSignatureBegin(0) sign"),
    (0xF934C, "push", "0x2d70a0", "\\??\\D:"),
    (0xF9351, "call", "dword ptr [0x293620]", "IoCreateSymbolicLink"),
    (0xF93CC, "call", "0xf7f7c", "XMountUtilityDrive"),
    (0xF93E8, "cmp", "dword ptr [ecx + 0xa4], eax", "GameRatings vs parental"),
    (0xF846E, "call", "0xf92c0", "XapiInitProcess before _cinit"),
    (0xF84BA, "call", "0xf9783", "_cinit"),
]
bad = 0
for a, mn, ops, why in SITES:
    ins = next(md.disasm(X.read(a, 16), a), None)
    ok = ins is not None and ins.mnemonic == mn and ins.op_str == ops
    bad += not ok
    got = f"{ins.mnemonic} {ins.op_str}" if ins else "?"
    print(f"{a:#08x} {'OK      ' if ok else 'MISMATCH'} {got:45s} ({why})")
print("mismatches:", bad)
