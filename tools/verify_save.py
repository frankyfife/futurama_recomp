"""verify_save.py - check (and optionally re-sign) Futurama save containers (M7, analysis/loader/m7_saves.md 3).

Every save file of the game (Futurama.dat, Options.dat, Unlock.dat; files.md 4.1/4.2) is a container of exactly
4096 bytes:

    0x000  u32     payload length
    0x004  u8[]    payload (Futurama.dat: ASCII "Key=Value" lines; Options.dat 0x214 bytes; Unlock.dat 0x78 bytes)
    ...    00      zeros up to 0xFEC
    0xFEC  u8[20]  XCalculateSignature = HMAC-SHA1(XboxSignatureKey, bytes 0x000..0xFEB)

The key is the 16-byte XboxSignatureKey of the port's kernel HLE ([saves] key_mode, docs/loader_design.md 4.5):
  fixed (default)  SHA-1("FuturamaPC/XboxSignatureKey/fixed/v1")[0..15], or [saves] signature_key (32 hex digits)
  xbox             HMAC-SHA1(XboxCERTKey, certificate SignatureKey)[0..15] (ASSUMPTION after Cxbx/xemu; the
                   certificate key of the EU retail XBE is 98c52648dc61d9aa5aaa4eebf25a4ae4, --xbe reads it)

A file is reported DAMAGED if it is not 4096 bytes long, its payload length runs into the signature, or the signature
does not match - the same tests as the game (FUN_000E0210 size, FUN_000E0340 signature); the game then shows
"Unable to load save game" (MEMCARD_052) and offers to delete it.

Also checked per save directory: SaveMeta.xbx (UTF-16 "Name=<save name>") exists and the directory name is the XAPI
hash of that name (FUN_000F6544: 12 hex digits of the name modulo 0xFFFFFFFFFFC5) - XDeleteSaveGame/XCreateSaveGame
find a save only through that hash.

Usage:
    python tools/verify_save.py [--config futurama.ini] [paths ...]   (default path: <userdata>/hdd/partition1/UDATA)
    python tools/verify_save.py --key 32HEX file.dat ...
    python tools/verify_save.py --config futurama.ini --resign file.dat   (rewrite the signature with that key, e.g.
                                                                           after importing a save signed elsewhere)
    python tools/verify_save.py --xbox-cert-key 32HEX file.dat          (check with the xbox key mode)
Options: --json (machine-readable result on stdout), --quiet (only damaged files and the summary).
Exit code: 0 = every container is valid, 1 = at least one damaged container, 2 = usage error / nothing found.
Without --config the INI is not read; the default userdata folder is never assumed (pass paths or --config).
"""
import argparse
import configparser
import hashlib
import hmac
import json
import os
import struct
import sys
from pathlib import Path

CONTAINER_SIZE = 0x1000
SIGNATURE_OFFSET = 0xFEC
SIGNATURE_SIZE = 20
FIXED_KEY_LABEL = b"FuturamaPC/XboxSignatureKey/fixed/v1"
# XBE certificate SignatureKey of the EU retail default.xbe (cert 0x10180 + 0xC0, files.md 4.3).
EU_CERT_SIGNATURE_KEY = bytes.fromhex("98c52648dc61d9aa5aaa4eebf25a4ae4")
TITLE_ID = "46490002"
SAVE_FILES = ("futurama.dat", "options.dat", "unlock.dat")
HASH_MODULUS = 0xFFFFFFFFFFC5


def fixed_key():
    return hashlib.sha1(FIXED_KEY_LABEL).digest()[:16]


def xbox_key(cert_key, cert_signature_key=EU_CERT_SIGNATURE_KEY):
    return hmac.new(cert_key, cert_signature_key, hashlib.sha1).digest()[:16]


def parse_hex16(text, what):
    t = "".join(c for c in text if c not in " -:")
    if len(t) != 32:
        raise ValueError("%s must be 32 hex digits" % what)
    return bytes.fromhex(t)


def signature(data, key):
    """HMAC-SHA1 of bytes 0..0xFEB of a 4096-byte container."""
    return hmac.new(key, bytes(data[:SIGNATURE_OFFSET]), hashlib.sha1).digest()


def sign(data, key):
    """Returns the container with a fresh signature at 0xFEC (data must be 4096 bytes)."""
    if len(data) != CONTAINER_SIZE:
        raise ValueError("a container has exactly 4096 bytes")
    out = bytearray(data)
    out[SIGNATURE_OFFSET:] = signature(out, key)
    return bytes(out)


def make_container(payload, key):
    """A signed container around `payload` (like FUN_000E1390/0xE0EA0/0xE1080 + FUN_000E03B0)."""
    if len(payload) > SIGNATURE_OFFSET - 4:
        raise ValueError("payload too long")
    data = bytearray(CONTAINER_SIZE)
    struct.pack_into("<I", data, 0, len(payload))
    data[4:4 + len(payload)] = payload
    return sign(data, key)


def save_dir_name(name):
    """XAPI directory name of a save (FUN_000F6544): the UTF-16 name as a base-0x10000 number modulo
    0xFFFFFFFFFFC5, 12 upper-case hex digits."""
    raw = name.encode("utf-16-le")
    v = 0
    for unit in struct.unpack("<%dH" % (len(raw) // 2), raw):
        v = (v * 0x10000 + unit) % HASH_MODULUS
    return "%012X" % v


def save_meta(name):
    """SaveMeta.xbx as XCreateSaveGame writes it: UTF-16LE with BOM, "Name=<name>\\r\\n" (format 0x2A5B5C)."""
    return ("﻿Name=%s\r\n" % name).encode("utf-16-le")


def read_save_meta_name(path):
    try:
        raw = Path(path).read_bytes()
    except OSError:
        return None
    try:
        text = raw.decode("utf-16-le")
    except UnicodeDecodeError:
        return None
    text = text.lstrip("﻿")
    for line in text.splitlines():
        if line.startswith("Name="):
            return line[5:]
    return None


def check_container(data, key):
    """(ok, reason, payload) for the bytes of one container."""
    if len(data) != CONTAINER_SIZE:
        return False, "size %d bytes (must be 4096)" % len(data), b""
    length = struct.unpack_from("<I", data, 0)[0]
    if length > SIGNATURE_OFFSET - 4:
        return False, "payload length 0x%X runs into the signature" % length, b""
    payload = bytes(data[4:4 + length])
    if not hmac.compare_digest(signature(data, key), bytes(data[SIGNATURE_OFFSET:])):
        return False, "signature mismatch (HMAC-SHA1 at 0xFEC)", payload
    return True, "ok", payload


def describe_payload(name, payload):
    lower = name.lower()
    if lower == "futurama.dat":
        text = payload.rstrip(b"\0").decode("latin-1", "replace")
        return text.replace("\r", "").replace("\n", " ").strip()
    if lower == "unlock.dat":
        return "%d bytes (24 x {u8, u32})" % len(payload)
    if lower == "options.dat":
        return "%d bytes" % len(payload)
    return "%d bytes" % len(payload)


def collect(paths):
    """Container files below the given files/directories."""
    files = []
    for p in paths:
        p = Path(p)
        if p.is_file():
            files.append(p)
        elif p.is_dir():
            for f in sorted(p.rglob("*")):
                if f.is_file() and f.name.lower() in SAVE_FILES:
                    files.append(f)
    return files


def userdata_from_config(ini):
    cp = configparser.ConfigParser(strict=False, interpolation=None)
    cp.read(ini, encoding="utf-8-sig")
    ud = cp.get("paths", "userdata", fallback="").strip()
    if not ud:
        # design 4.1 / M3c: without [paths] userdata the user data live next to futurama.ini
        return Path(ini).resolve().parent, cp
    p = Path(ud)
    if not p.is_absolute():
        p = Path(ini).resolve().parent / p
    return p, cp


def key_from_config(cp):
    mode = cp.get("saves", "key_mode", fallback="fixed").strip().lower()
    if mode == "xbox":
        cert = cp.get("saves", "xbox_cert_key", fallback="").strip()
        if not cert:
            raise ValueError("[saves] key_mode = xbox needs [saves] xbox_cert_key")
        return xbox_key(parse_hex16(cert, "[saves] xbox_cert_key")), "xbox"
    custom = cp.get("saves", "signature_key", fallback="").strip()
    if custom:
        return parse_hex16(custom, "[saves] signature_key"), "fixed (custom signature_key)"
    return fixed_key(), "fixed"


def cert_signature_key_from_xbe(path):
    b = Path(path).read_bytes()
    if b[:4] != b"XBEH":
        raise ValueError("%s is not an XBE" % path)
    base, cert = struct.unpack_from("<I", b, 0x104)[0], struct.unpack_from("<I", b, 0x118)[0]
    return b[cert - base + 0xC0:cert - base + 0xD0]


def verify(files, key, quiet=False, resign=False):
    results = []
    for f in files:
        data = f.read_bytes()
        ok, reason, payload = check_container(data, key)
        entry = {"file": str(f), "ok": ok, "reason": reason}
        meta = f.parent / "SaveMeta.xbx"
        name = read_save_meta_name(meta)
        if name is not None:
            entry["name"] = name
            expected = save_dir_name(name)
            entry["dir_matches_name"] = f.parent.name.upper() == expected
        if ok:
            entry["payload"] = describe_payload(f.name, payload)
        if resign and not ok and len(data) == CONTAINER_SIZE:
            f.write_bytes(sign(data, key))
            entry["resigned"] = True
        results.append(entry)
        if not quiet or not ok:
            line = "%-8s %s" % ("OK" if ok else "DAMAGED", f)
            if name is not None:
                line += '  ("%s"%s)' % (name, "" if entry["dir_matches_name"] else
                                        ", directory name is not the hash %s" % save_dir_name(name))
            if not ok:
                line += "  - " + reason + (" - re-signed" if entry.get("resigned") else "")
            elif not quiet:
                line += "  " + entry["payload"][:160]
            print(line)
    return results


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("paths", nargs="*", help="container files or directories (searched recursively)")
    ap.add_argument("--config", help="futurama.ini: [saves] key and [paths] userdata")
    ap.add_argument("--key", help="XboxSignatureKey as 32 hex digits (overrides --config)")
    ap.add_argument("--xbox-cert-key", help="XboxCERTKey (32 hex digits): use the xbox key mode")
    ap.add_argument("--xbe", help="read the certificate SignatureKey from this XBE (xbox key mode)")
    ap.add_argument("--resign", action="store_true", help="rewrite the signature of damaged 4096-byte containers")
    ap.add_argument("--json", action="store_true", help="print the result as JSON")
    ap.add_argument("--quiet", action="store_true", help="print only damaged files and the summary")
    args = ap.parse_args(argv)

    try:
        key, mode = fixed_key(), "fixed"
        paths = list(args.paths)
        if args.config:
            userdata, cp = userdata_from_config(args.config)
            key, mode = key_from_config(cp)
            if not paths:
                if userdata is None:
                    print("verify_save: [paths] userdata is not set in %s" % args.config, file=sys.stderr)
                    return 2
                paths = [userdata / "hdd" / "partition1" / "UDATA" / TITLE_ID]
        if args.xbox_cert_key:
            cert_sig = cert_signature_key_from_xbe(args.xbe) if args.xbe else EU_CERT_SIGNATURE_KEY
            key, mode = xbox_key(parse_hex16(args.xbox_cert_key, "--xbox-cert-key"), cert_sig), "xbox"
        if args.key:
            key, mode = parse_hex16(args.key, "--key"), "explicit key"
    except (ValueError, OSError) as e:
        print("verify_save: %s" % e, file=sys.stderr)
        return 2
    if not paths:
        print("verify_save: no paths (pass files/directories or --config)", file=sys.stderr)
        return 2
    files = collect(paths)
    if not files:
        print("verify_save: no save containers (Futurama.dat, Options.dat, Unlock.dat) in %s" %
              ", ".join(str(p) for p in paths), file=sys.stderr)
        return 2
    if not args.json:
        print("key mode %s, XboxSignatureKey %s" % (mode, key.hex().upper()))
    results = verify(files, key, quiet=args.quiet or args.json, resign=args.resign)
    damaged = [r for r in results if not r["ok"]]
    if args.json:
        print(json.dumps({"key_mode": mode, "key": key.hex().upper(), "files": results}, indent=1))
    else:
        print("%d container(s), %d valid, %d damaged" % (len(results), len(results) - len(damaged), len(damaged)))
    return 1 if damaged and not args.resign else 0


if __name__ == "__main__":
    sys.exit(main())
