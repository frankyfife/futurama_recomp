"""For NtQueryInformationFile (0x2935F0), NtSetInformationFile (0x2935B8), NtQueryVolumeInformationFile (0x2935F4)
and NtQueryDirectoryFile (0x293618): disassemble each owner function linearly and print the pushes leading to each
'call [slot]' (args are pushed right-to-left, so the first push of the 5-arg info calls is FileInformationClass)."""
from verify_files_common import *
import struct
SLOTS = {0x2935F0: "NtQueryInformationFile", 0x2935B8: "NtSetInformationFile", 0x2935F4: "NtQueryVolumeInformationFile",
         0x293618: "NtQueryDirectoryFile", 0x2935C4: "NtCreateFile", 0x29360C: "NtOpenFile"}
code = [s for s in X.sections if s[0] in (".text", "D3D")]
for slot, nm in SLOTS.items():
    sites = []
    for n, va, vsz, raw, rsz in code:
        blob = X.b[raw:raw + rsz]; nd = b"\xff\x15" + struct.pack("<I", slot); i = blob.find(nd)
        while i >= 0: sites.append(va + i); i = blob.find(nd, i + 1)
    print(f"== {nm}")
    for s in sites:
        f = owner(s)
        ins = [i for i in md.disasm(X.read(f[0], s + 6 - f[0]), f[0])]
        if not ins or ins[-1].address != s:
            # resync from closer start
            for back in range(60, 0, -1):
                ins = list(md.disasm(X.read(s - back, back + 6), s - back))
                if ins and ins[-1].address == s: break
        pushes = []
        for i in reversed(ins[:-1]):
            if i.mnemonic == "call" or i.mnemonic.startswith("j") or i.mnemonic == "ret": break
            if i.mnemonic == "push": pushes.append(i.op_str)
        print(f"  {s:#x} [{f[2]}] pushes(last arg first): {list(reversed(pushes))[:12]}")
