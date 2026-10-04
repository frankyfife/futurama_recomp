"""Check memory-unit support: MU_Init / XInitDevices callers, device-type table passed by the game."""
from verify_files_common import *
for a in (0x28c3b5, 0x28cfe1, 0x28c09c):
    print("callers of %x %s:" % (a, NAMES.get(a)), callers(a))
print(decomp(0x28c09c)[:3000])
print(decomp(0x28c3b5)[:2000])
