"""G4 (analysis/loader/g4_select.md 2.3/3): dumps the built-in debug level select of the retail EU XBE.
Cheat code table 0x2D6038 (char*[], NULL-terminated, compared by FUN_0002C7D0), level start states
0x2D60BC (stride 8, used by FUN_0002C530 for codes 0..0x15) and the unused labels at 0x2D60B8 (same stride),
plus the button sequence for each code (FUN_0002C460: LT held, Y=1 B=2 A=3 X=4, with RT: 5..8, WHITE=9,
BLACK=0, BACK = confirm). Usage: python g4_tables.py"""
import sys
sys.path.insert(0, r"D:\FuturamaDecomp\tools")
from xbe import Xbe

x = Xbe(r"D:\FuturamaDecomp\orig\retail_eu\default.xbe")
BTN = {"1": "Y", "2": "B", "3": "A", "4": "X", "5": "RT+Y", "6": "RT+B", "7": "RT+A", "8": "RT+X", "9": "WHITE", "0": "BLACK"}
EFFECT = {0x16: "Health = 100", 0x17: "Munition 999 (5 Slots)", 0x18: "Leben +5", 0x19: "Power/Charge +10000 (vf+0x15C)",
          0x1A: "Gott-Modus an/aus (Health 55555, vf+0x174) + Flag 0x2D9880", 0x1B: "Flag 0x2D9880 an/aus",
          0x1C: "Flag 0x2EB7D4 an/aus (alle Zwischensequenzen ueberspringbar)"}
i = 0
while True:
    p = x.u32(0x2D6038 + 4 * i)
    if p == 0:
        break
    code = x.cstr(p)
    seq = " ".join(BTN[c] for c in code) + " BACK"
    if i < 0x16:
        state = x.cstr(x.u32(0x2D60BC + 8 * i)).replace("\n", "; ")
        label = x.cstr(x.u32(0x2D60B8 + 8 * i))
        what = "%-22s %s" % (label, state)
    else:
        what = EFFECT.get(i, "?")
    print("%2d %s  [%s]  %s" % (i, code, seq, what))
    i += 1
