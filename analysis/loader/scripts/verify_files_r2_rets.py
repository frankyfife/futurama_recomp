"""Table 2.2: stdcall cleanup ('ret N') of every XAPI function quoted, from the raw bytes,
plus the push count before every E8 call to it (argument count cross-check)."""
from verify_files_r2_common import *

TABLE = [  # addr, name, claimed ret
    (0xF76E0, "CreateFileA", 0x1C), (0xF7195, "ReadFile", 0x14), (0xF7603, "ReadFileEx", 0x14),
    (0xF7654, "WriteFileEx", 0x14), (0xF7282, "WriteFile", 0x14), (0xF76A5, "GetFileSize", 8),
    (0xF75C5, "GetFileSizeEx", 8), (0xF73DC, "SetFilePointer", 0x10), (0xF7358, "SetEndOfFile", 4),
    (0xF7B4D, "CopyFileA", 0xC), (0xF786B, "CopyFileExA", 0x18), (0xF7B6C, "SetFileAttributesA", 8),
    (0xF7C12, "GetFileAttributesA", 4), (0xF7C5D, "GetDiskFreeSpaceExA", 0x10),
    (0xF8081, "XGetDiskClusterSizeA", 4), (0xF8120, "XGetDiskSectorSizeA", 4),
    (0xF85AA, "CreateDirectoryA", 8), (0xF8616, "RemoveDirectoryA", 4), (0xF8961, "FindFirstFileA", 8),
    (0xF8A6E, "FindNextFileA", 8), (0xF8886, "DelTree", 8), (0xF6987, "XCreateSaveGame", 0x18),
    (0xF6BEA, "XDeleteSaveGame", 8), (0xF6CBA, "XFindFirstSaveGame", 8), (0xF6DC1, "XFindNextSaveGame", 8),
    (0xF6E08, "XFindClose", 4), (0xF7163, "XCalculateSignatureBegin", 4), (0xF70EC, "XCalculateSignatureUpdate", 0xC),
    (0xF7106, "XCalculateSignatureEnd", 8), (0xF7F7C, "XMountUtilityDrive", 4), (0xF946D, "FATX format", 8),
    (0x13F1D0, "NiUDSStream completion", 0xC), (0xE01A0, "Save completion", 0xC),
]
for va, name, claim in TABLE:
    f = owner(va)
    size = f[1] if f and f[0] == va else 0x400
    rets = sorted({i.op_str for i in dis(va, va + size) if i.mnemonic == "ret"})
    retv = sorted({int(r, 16) if r else 0 for r in rets})
    # pushes before call sites
    pc = collections.Counter()
    for c in raw_calls_to(va):
        o = owner(c)
        ins = dis(o[0], c + 5) if o and not o[2].endswith("+out") and o[0] < c else list(md.disasm(X.read(c - 0x30, 0x35), c - 0x30))
        idx = [k for k, i in enumerate(ins) if i.address == c]
        if idx:
            pc[pushes_before(ins, idx[0])] += 1
    ok = claim in retv and len(retv) == 1
    print("%08x %-26s ret=%s claimed=0x%X %s  pushes@callers=%s" % (va, name, [hex(r) for r in retv], claim, "OK" if ok else "CHECK", dict(pc)))
