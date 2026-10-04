"""Print printable ASCII runs (offset<TAB>string) like `strings -t x`."""
import re, sys
data = open(sys.argv[1], "rb").read()
n = int(sys.argv[2]) if len(sys.argv) > 2 else 5
for m in re.finditer(rb"[\x20-\x7e\t]{%d,}" % n, data):
    print(f"{m.start():08x}\t{m.group().decode()}")
