"""For every call [slot] to NtQueryInformationFile / NtSetInformationFile / NtQueryVolumeInformationFile /
NtQueryDirectoryFile: print the pushed argument list (push operands in order, last-arg first) so the
information class and length can be read off (claims 2.2 / 5.1 item 4)."""
from verify_files_r2_common import *

SLOTS = {0x2935F0: "NtQueryInformationFile", 0x2935B8: "NtSetInformationFile",
         0x2935F4: "NtQueryVolumeInformationFile", 0x293618: "NtQueryDirectoryFile",
         0x2935C4: "NtCreateFile", 0x29360C: "NtOpenFile"}
for slot, name in SLOTS.items():
    print("==", name)
    for sec, va in raw_dword_refs(slot, CODE_SECTIONS):
        f = owner(va)
        if not f or f[2].endswith("+out"):
            start = va - 0x60
        else:
            start = f[0]
        ins = dis(start, va + 6)
        idx = [k for k, i in enumerate(ins) if i.address <= va - 2 < i.address + i.size]
        if not idx:
            continue
        k = idx[0]
        call = ins[k]
        args = []
        j = k - 1
        while j >= 0 and len(args) < 11:
            i = ins[j]
            if i.mnemonic == "push":
                args.append(i.op_str)
            elif i.mnemonic in ("call", "ret") or i.mnemonic.startswith("j"):
                break
            j -= 1
        # args[0] is the last push = first argument
        print("  %08x %-40s %s(%s)" % (call.address, (f[2] if f else "?")[:40], call.mnemonic, ", ".join(args)))
