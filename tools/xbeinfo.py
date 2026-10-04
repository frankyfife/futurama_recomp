"""Dump XBE header, sections, library versions and debug info."""
import struct, sys, time

XOR_EP = {"retail": 0xA8FC57AB, "debug": 0x94859D4B, "chihiro": 0x40B5C16E}
XOR_KT = {"retail": 0x5B6D40B6, "debug": 0xEFB1F152, "chihiro": 0x2290059D}

def cstr(b, off):
    end = b.index(b"\0", off)
    return b[off:end].decode("latin-1")

def main(path):
    b = open(path, "rb").read()
    assert b[:4] == b"XBEH"
    base = struct.unpack_from("<I", b, 0x104)[0]
    va = lambda a: a - base
    f = dict(zip(
        "size_hdrs size_img size_imghdr timedate cert nsect sect initflags ep tls stack_commit heap_res heap_commit pe_base pe_size pe_csum pe_time dbg_path dbg_file dbg_ufile kthunk nonkimp nlib libs klib xapilib logo logosz".split(),
        struct.unpack_from("<27I", b, 0x108)))
    print(f"base         {base:#010x}")
    print(f"timedate     {time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(f['timedate']))} UTC")
    print(f"pe timedate  {time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(f['pe_time']))} UTC")
    for kind in XOR_EP:
        ep = f["ep"] ^ XOR_EP[kind]
        kt = f["kthunk"] ^ XOR_KT[kind]
        if base <= ep < base + f["size_img"] and base <= kt < base + f["size_img"]:
            print(f"xor keys     {kind}  entry={ep:#010x}  kernel_thunk={kt:#010x}")
    print(f"init flags   {f['initflags']:#x}")
    print(f"debug path   {cstr(b, va(f['dbg_path']))}")
    print(f"debug file   {cstr(b, va(f['dbg_file']))}")
    c = va(f["cert"])
    title_id = struct.unpack_from("<I", b, c + 8)[0]
    title = b[c + 0xC:c + 0xC + 80].decode("utf-16le").rstrip("\0")
    regions, rating, discno, ver = struct.unpack_from("<IIII", b, c + 0xA0)
    print(f"title        {title!r}  id={title_id:#010x} ({chr(title_id>>24)}{chr((title_id>>16)&0xff)}-{title_id&0xffff:03d})")
    print(f"regions      {regions:#x}  version={ver:#x}")
    print("libraries:")
    for i in range(f["nlib"]):
        o = va(f["libs"]) + i * 16
        name = b[o:o + 8].rstrip(b"\0").decode()
        maj, mi, bld, fl = struct.unpack_from("<4H", b, o + 8)
        print(f"  {name:8} {maj}.{mi}.{bld}  qfe={fl & 0x1fff} approved={(fl>>13)&3} debug={fl>>15}")
    print("sections:")
    for i in range(f["nsect"]):
        o = va(f["sect"]) + i * 0x38
        fl, sva, vsz, raw, rsz, nameaddr = struct.unpack_from("<6I", b, o)
        flags = "".join(c for c, bit in zip("WPRXHI", range(6)) if fl >> bit & 1)
        print(f"  {cstr(b, va(nameaddr)):10} va={sva:#010x} vsz={vsz:#09x} raw={raw:#09x} rsz={rsz:#09x} [{flags}]")

main(sys.argv[1])
