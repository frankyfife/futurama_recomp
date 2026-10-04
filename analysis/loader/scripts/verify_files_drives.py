"""Scan the XBE for drive-letter paths: ASCII/UTF-16 strings 'X:\\' or 'X:/' in data sections, and
immediates encoding 'X:\\' (e.g. 0x005c3a55 = 'U:\\') in code. Reports owner function of each code reference."""
from verify_files_common import *
import re
BS = b"\x5c"
rx = re.compile(rb"(?<![A-Za-z0-9])([A-Za-z]):[\x5c/]")
for n, va, vsz, raw, rsz in X.sections:
    blob = X.b[raw:raw + rsz]
    if n in (".rdata", ".data"):
        for m in rx.finditer(blob):
            a = va + m.start()
            s = blob[m.start():blob.find(b"\0", m.start())][:70]
            refs = [h for _, h in xrefs(a)]
            print(f"ASCII {n} {a:#x} {s!r} refs={[hex(r) for r in refs]} owners={[owner(r)[2] for r in refs if owner(r)]}")
    for m in re.finditer(rb"([A-Za-z])\x00:\x00[\x5c/]\x00", blob):
        a = va + m.start()
        e = m.start()
        while e + 1 < len(blob) and blob[e:e + 2] != b"\0\0": e += 2
        print(f"UTF16 {n} {a:#x} {blob[m.start():e].decode('utf-16le', 'replace')!r} refs={[hex(h) for _, h in xrefs(a)]}")
for n, va, vsz, raw, rsz in X.sections:
    if n not in (".text", "D3D", "DSOUND", "XMV", "XPP"): continue
    blob = X.b[raw:raw + rsz]
    for m in re.finditer(rb"([A-Za-z]):\x5c", blob):
        a = va + m.start()
        for back in range(1, 8):
            ins = list(md.disasm(X.read(a - back, back + 4), a - back))
            if ins and ins[0].address == a - back and ins[0].size == back + 4 and ins[0].mnemonic in ("mov", "push", "cmp"):
                o = owner(a)
                print(f"IMM {n} {a - back:#x} {ins[0].mnemonic} {ins[0].op_str}  ({chr(m.group(1)[0])}:) in {o[2] if o else None}")
                break
