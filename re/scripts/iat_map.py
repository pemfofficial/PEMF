"""iat_map.py [slot...] - map IAT slot addresses to imported function names."""
import struct
import sys
from iat_refs import iat_slots
from xref_scan import PE

DEFAULT = r"C:\Users\Shadow\Projects\PiratesMod\re\bin\Pirates_gog.exe"


def main():
    pe = PE(DEFAULT)
    slots = iat_slots(pe)                       # {(dll, func): va}
    by_va = {va: (d, f) for (d, f), va in slots.items()}

    if len(sys.argv) > 1:
        for a in sys.argv[1:]:
            va = int(a, 16)
            hit = by_va.get(va)
            print(f"0x{va:08X} -> {hit[1] + '  (' + hit[0] + ')' if hit else 'NOT AN IAT SLOT'}")
        return

    # No args: list everything printf-ish, so we can spot format-string paths.
    print("=== format/string related imports ===")
    for (dll, fn), va in sorted(slots.items(), key=lambda kv: kv[1]):
        if any(k in fn.lower() for k in
               ("printf", "str", "cpy", "cat", "cmp", "len")):
            print(f"  0x{va:08X}  {fn:<24} ({dll})")


if __name__ == "__main__":
    main()
