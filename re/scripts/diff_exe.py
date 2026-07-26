"""Compare gog-original vs cp-base and report whether our offsets are affected."""
import json
import struct
from xref_scan import PE

A = r"C:\Users\Shadow\Projects\PiratesMod\re\bin\Pirates_gog.exe"
B = r"C:\Users\Shadow\Projects\PiratesMod\re\bin\Pirates_cp.exe"

pa, pb = PE(A), PE(B)

# Addresses the mod depends on, with how many bytes matter at each.
CRITICAL = {
    "AddText0":        (0x004F6090, 16),
    "AddText1":        (0x004F60B0, 16),
    "Formatter_core":  (0x004F60D0, 64),
    "WrapText":        (0x004879F0, 32),
    "ShowMessage":     (0x00410C50, 64),
    "GetMoraleLevel":  (0x00404810, 96),
}

print("=== overall ===")
print(f"gog : {len(pa.data)} bytes")
print(f"cp  : {len(pb.data)} bytes  (+{len(pb.data)-len(pa.data)})")

# Compare only the regions both share, section by section, by VA.
print("\n=== per-section byte differences (common VA ranges) ===")
total_diff = 0
for sa in pa.sections:
    sb = next((s for s in pb.sections if s["name"] == sa["name"]), None)
    if not sb:
        print(f"  {sa['name']:<10} MISSING in cp")
        continue
    n = min(sa["rs"], sb["rs"])
    da = pa.data[sa["rp"]:sa["rp"] + n]
    db = pb.data[sb["rp"]:sb["rp"] + n]
    diff = sum(1 for x, y in zip(da, db) if x != y)
    total_diff += diff
    flag = "" if diff == 0 else "   <-- PATCHED"
    print(f"  {sa['name']:<10} compared {n:>8} bytes, {diff:>6} differ{flag}")
print(f"  total differing bytes: {total_diff}")

print("\n=== critical addresses ===")
all_ok = True
for name, (va, n) in CRITICAL.items():
    oa, ob = pa.va_to_off(va), pb.va_to_off(va)
    ba = pa.data[oa:oa + n]
    bb = pb.data[ob:ob + n]
    ok = ba == bb
    all_ok &= ok
    print(f"  {name:<16} 0x{va:08X} [{n:>3}B]  {'identical' if ok else 'DIFFERS'}")
    if not ok:
        for i, (x, y) in enumerate(zip(ba, bb)):
            if x != y:
                print(f"        +0x{i:02X}: gog={x:02X} cp={y:02X}")

print("\nVERDICT:", "offsets are valid for BOTH builds"
      if all_ok else "offsets are NOT safe on cp-base")
