"""
map_recon.py - find the functions that drive the overworld map and town tables,
so we know what to decompile. Dependency-free.

Interesting data:
  city positions  0x0085B170 (stride 16: x@+0, y@+4)
  city records    0x00860B70 (stride 0x20: flags@+0, type@+4)
  player X/Y      0x00814304 / 0x00814308
  map bitmaps     "CaribbeanMap6/4/2.bmp"
  FindNearestCity 0x0045FD40
"""
import struct
from collections import defaultdict

from xref_scan import PE, find_dword_refs
from trace_sound2 import function_starts, enclosing, e8_callers

PE_PATH = r"C:\Users\Shadow\Projects\PiratesMod\re\bin\Pirates_gog.exe"

WATCH = {
    0x0085B170: "city_positions_table",
    0x00860B70: "city_records_table",
    0x00814304: "player_X",
    0x00814308: "player_Y",
    0x00701B2C: '"CaribbeanMap6.bmp"',
    0x00701B40: '"CaribbeanMap4.bmp"',
    0x00701B54: '"CaribbeanMap2.bmp"',
    0x0045FD40: "FindNearestCity",
}


def main():
    pe = PE(PE_PATH)
    starts = function_starts(pe)
    print(f"# {len(starts)} functions\n")

    for addr, label in WATCH.items():
        refs = find_dword_refs(pe, addr)
        funcs = defaultdict(int)
        for off in refs:
            fv = enclosing(starts, pe.off_to_va(off))
            if fv:
                funcs[fv] += 1
        print(f"=== {label}  (0x{addr:08X})  {len(refs)} refs in {len(funcs)} funcs ===")
        for fv, n in sorted(funcs.items(), key=lambda kv: -kv[1])[:8]:
            print(f"    FUNC 0x{fv:08X}  x{n}")
        print()

    # callers of FindNearestCity -> the map/overworld logic that uses towns
    print("=== callers of FindNearestCity ===")
    for c in e8_callers(pe, 0x0045FD40)[:20]:
        print(f"    call @0x{c:08X}  in FUNC 0x{enclosing(starts, c):08X}")


if __name__ == "__main__":
    main()
