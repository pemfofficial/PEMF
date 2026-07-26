"""
trace_sound2.py - map the game's sound module and its call graph.

The Miles wrappers cluster around 0x0052Dxxx. Identify function starts by the
CC/CC padding MSVC inserts between functions, attribute each Miles FF15 call and
each ".wav"/"-%03d.wav" string reference to its enclosing function, then walk the
E8 call graph upward to find the high-level "play sound by name" entry point.
"""
import struct
from collections import defaultdict

from xref_scan import PE, find_dword_refs
from find_sound_api import import_table

PE_PATH = r"C:\Users\Shadow\Projects\PiratesMod\re\bin\Pirates_gog.exe"


def function_starts(pe):
    """Every VA that begins a function: preceded by CC padding or ret+CC."""
    text = pe.sec_by_name(".text")
    rp, rs = text["rp"], text["rs"]
    data = pe.data
    starts = []
    i = rp + 1
    end = rp + rs
    while i < end:
        prev = data[i - 1]
        cur = data[i]
        # function bodies begin right after 0xCC padding or a ret, and start
        # with a real instruction (not more padding)
        if prev == 0xCC and cur != 0xCC:
            starts.append(pe.off_to_va(i))
        i += 1
    return sorted(set(starts))


def enclosing(starts, va):
    """Largest function start <= va."""
    lo, hi, best = 0, len(starts) - 1, None
    while lo <= hi:
        mid = (lo + hi) // 2
        if starts[mid] <= va:
            best = starts[mid]
            lo = mid + 1
        else:
            hi = mid - 1
    return best


def e8_callers(pe, target_va, limit=60):
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
    pe = PE(PE_PATH)
    starts = function_starts(pe)
    print(f"# {len(starts)} function starts detected\n")

    imports = import_table(pe)
    miles = {slot: fn for (d, fn, slot) in imports if d.lower().startswith("mss32")}

    # attribute every Miles FF15 call to its enclosing function
    func_miles = defaultdict(set)
    for slot, fn in miles.items():
        for off in find_dword_refs(pe, slot):
            if pe.data[off - 2] == 0xFF and pe.data[off - 1] == 0x15:
                fv = enclosing(starts, pe.off_to_va(off))
                func_miles[fv].add(fn.replace("_AIL_", "").replace("@", " @"))

    # attribute ".wav" / "-%03d.wav" refs
    wav_va = {va for va, t in
              [(0x0070AB88, ".wav"), (0x00712F68, "-%03d.wav")]}
    func_wav = defaultdict(list)
    for va in (0x0070AB88, 0x00712F68):
        for off in find_dword_refs(pe, va):
            fv = enclosing(starts, pe.off_to_va(off))
            if fv:
                func_wav[fv].append(va)

    print("=== sound-module functions (Miles callers) ===")
    for fv in sorted(func_miles):
        tag = "  <-- references .wav" if fv in func_wav else ""
        print(f"\n  FUNC 0x{fv:08X}{tag}")
        print(f"    miles: {', '.join(sorted(func_miles[fv]))}")
        callers = e8_callers(pe, fv)
        print(f"    callers ({len(callers)}): "
              + ", ".join(f"0x{c:08X}" for c in callers[:12]))

    # the loader = function that references .wav AND calls set_named_sample_file
    print("\n\n=== KEY: file-name -> sample loader (references .wav) ===")
    for fv in sorted(func_wav):
        m = func_miles.get(fv, set())
        print(f"  0x{fv:08X}  miles={sorted(m)}  wavrefs={len(func_wav[fv])}")

    # walk up from the loader/player to find the by-name entry the game logic uses
    print("\n=== one level up from each sound-module function ===")
    for fv in sorted(func_miles):
        for c in e8_callers(pe, fv)[:20]:
            up = enclosing(starts, c)
            if up and up not in func_miles:
                print(f"  0x{up:08X} -> calls sound func 0x{fv:08X}")


if __name__ == "__main__":
    main()
