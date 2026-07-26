"""
trace_sound.py - identify the game's audio wrapper functions by finding which
of them call the Miles (Mss32) IAT slots, and by walking the call graph up from
those wrappers.

No disassembler needed: on x86 an import call is `FF 15 <abs IAT slot>`, and a
direct call is `E8 <rel32>`. We scan for both by raw bytes.
"""
import struct
import sys
from collections import defaultdict

from xref_scan import PE, find_dword_refs
from find_sound_api import import_table


def func_start(pe, off):
    """Walk back to the nearest standard prologue 55 8B EC (push ebp; mov ebp,esp)."""
    lo = max(0, off - 0x800)
    for i in range(off, lo, -1):
        if pe.data[i] == 0x55 and pe.data[i + 1] == 0x8B and pe.data[i + 2] == 0xEC:
            return pe.off_to_va(i)
    return None


def scan_func_for_imports(pe, start_va, miles_slots, max_len=0x1200):
    """From a function start, collect FF15 import calls until a lone RET(C3)/RETN(C2)."""
    off = pe.va_to_off(start_va)
    if off is None:
        return []
    hits = []
    i = off
    end = off + max_len
    while i < end:
        b = pe.data[i]
        if b == 0xFF and pe.data[i + 1] == 0x15:
            slot = struct.unpack_from("<I", pe.data, i + 2)[0]
            if slot in miles_slots:
                hits.append((pe.off_to_va(i), miles_slots[slot]))
            i += 6
            continue
        # crude end-of-function: C3 ret at a plausible boundary
        if b == 0xC3 and i > off + 8:
            break
        i += 1
    return hits


def callers_of(pe, target_va, limit=40):
    """Direct E8 callers of target_va."""
    text = pe.sec_by_name(".text")
    blob = pe.data[text["rp"]:text["rp"] + text["rs"]]
    out = []
    i = 0
    while i < len(blob) - 5 and len(out) < limit:
        if blob[i] == 0xE8:
            rel = struct.unpack_from("<i", blob, i + 1)[0]
            src = pe.off_to_va(text["rp"] + i)
            if src and src + 5 + rel == target_va:
                out.append(src)
        i += 1
    return out


def main():
    path = r"C:\Users\Shadow\Projects\PiratesMod\re\bin\Pirates_gog.exe"
    pe = PE(path)

    imports = import_table(pe)
    miles_slots = {slot: fn for (dll, fn, slot) in imports
                   if dll.lower().startswith("mss32")}
    print(f"# Miles slots: {len(miles_slots)}  range "
          f"0x{min(miles_slots):X}..0x{max(miles_slots):X}\n")

    key = {
        "_AIL_allocate_sample_handle@4",
        "_AIL_set_named_sample_file@20",
        "_AIL_start_sample@4",
        "_AIL_init_sample@4",
        "_AIL_set_sample_volume_pan@12",
        "_AIL_set_sample_loop_count@8",
    }
    key_slots = {s for s, fn in miles_slots.items() if fn in key}

    # 1. call sites of each key Miles function -> enclosing game function
    print("=== enclosing game functions that call key Miles APIs ===")
    wrappers = defaultdict(set)
    for slot in sorted(key_slots):
        fn = miles_slots[slot]
        sites = find_dword_refs(pe, slot)
        for s_off in sites:
            # confirm it's an FF15 (import call), not incidental data
            if pe.data[s_off - 2] == 0xFF and pe.data[s_off - 1] == 0x15:
                fs = func_start(pe, s_off)
                if fs:
                    wrappers[fs].add(fn)
    for fv in sorted(wrappers):
        fns = ", ".join(sorted(x.replace("_AIL_", "").replace("@", " @")
                               for x in wrappers[fv]))
        print(f"  func 0x{fv:08X}  calls: {fns}")

    # 2. the play chain: whichever wrapper calls start_sample is the player
    print("\n=== candidate play-sound wrapper(s) (call start_sample) ===")
    players = [fv for fv, fns in wrappers.items()
               if "_AIL_start_sample@4" in fns]
    for pv in sorted(players):
        cs = callers_of(pe, pv)
        print(f"  0x{pv:08X}  <- {len(cs)} caller(s): "
              + ", ".join(f"0x{c:08X}" for c in cs[:10]))

    # 3. inspect the earlier string-cluster candidates
    print("\n=== Miles calls inside the string-cluster candidates ===")
    for cand in (0x004CDDF0, 0x0052C980, 0x0052CF90, 0x004F4ED0, 0x00527D30):
        hits = scan_func_for_imports(pe, cand, miles_slots)
        names = ", ".join(sorted({h[1].replace("_AIL_", "") for h in hits}))
        print(f"  0x{cand:08X}: {names or '(no direct Miles calls)'}")


if __name__ == "__main__":
    main()
