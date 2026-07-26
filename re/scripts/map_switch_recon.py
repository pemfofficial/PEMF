"""
map_switch_recon.py - assess whether the overworld map + town table can be
SWAPPED at runtime (a "sail to the edge, load the next region" zone transition).

Looks for:
  - edge / "turn back" detection strings + their referencing functions
  - callers of the map render/load funcs (0x004458D0 / 0x00445D40) and the map
    cache global (0x008DC920) -> is the map loaded once or re-loadable?
  - who bulk-writes the city record table (0x00860B70) -> the scenario/new-game
    town initialiser we'd swap per region
"""
import struct
from collections import defaultdict

from xref_scan import PE, find_dword_refs, find_strings
from trace_sound2 import function_starts, enclosing, e8_callers

PE_PATH = r"C:\Users\Shadow\Projects\PiratesMod\re\bin\Pirates_gog.exe"


def main():
    pe = PE(PE_PATH)
    starts = function_starts(pe)

    # 1. edge / boundary / turn-back strings
    print("=== edge / boundary strings ===")
    hits = find_strings(pe, lambda t: any(k in t.lower() for k in
                 ("turn back", "edge of", "sail back", "cannot sail", "end of",
                  "boundary", "too far", "off the map", "return to")))
    for va, t in hits[:20]:
        refs = find_dword_refs(pe, va)
        fns = {enclosing(starts, pe.off_to_va(r)) for r in refs}
        loc = ", ".join(f"0x{f:08X}" for f in sorted(x for x in fns if x))
        print(f"  0x{va:08X} refs={len(refs)} in [{loc}]  {t[:60]!r}")

    # 2. map render/load callers + cache global
    print("\n=== callers of map render 0x004458D0 ===")
    for c in e8_callers(pe, 0x004458D0)[:15]:
        print(f"  call@0x{c:08X} in FUNC 0x{enclosing(starts, c):08X}")
    print("=== callers of map bitmap loader 0x00445D40 ===")
    for c in e8_callers(pe, 0x00445D40)[:15]:
        print(f"  call@0x{c:08X} in FUNC 0x{enclosing(starts, c):08X}")
    print("=== refs to map cache global 0x008DC920 (0=needs (re)load) ===")
    funcs = defaultdict(int)
    for r in find_dword_refs(pe, 0x008DC920):
        f = enclosing(starts, pe.off_to_va(r))
        if f:
            funcs[f] += 1
    for f, n in sorted(funcs.items(), key=lambda kv: -kv[1])[:10]:
        print(f"  FUNC 0x{f:08X} x{n}")

    # 3. bulk writers of the city table 0x00860B70 (scenario / town init)
    print("\n=== functions referencing city table 0x00860B70 the most ===")
    funcs = defaultdict(int)
    for r in find_dword_refs(pe, 0x00860B70):
        f = enclosing(starts, pe.off_to_va(r))
        if f:
            funcs[f] += 1
    for f, n in sorted(funcs.items(), key=lambda kv: -kv[1])[:12]:
        print(f"  FUNC 0x{f:08X} x{n}")


if __name__ == "__main__":
    main()
