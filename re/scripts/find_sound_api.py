"""
find_sound_api.py - locate the game's "play sound by name" function.

Approach mirrors xref_scan.py: the dialog API is already known to take a sound
NAME by string ("snap"). Sound names appear as ASCII in .rdata. Find every
push of such a string in .text, take the CALL that follows, and cluster the
targets. The most-hit target across many distinct sound names is the shared
play-by-name entry point.

Also dumps Mss32.dll (Miles Sound System) imports and their IAT slots, so the
native low-level audio calls can be cross-referenced.

Dependency-free (no capstone) -- uses the PE helper from xref_scan.
"""
import struct
import sys
from collections import Counter, defaultdict

from xref_scan import PE, find_dword_refs, next_call_target


# base names of loose files seen under Assets/Sounds, plus the known "snap"
KNOWN_SOUND_TOKENS = (
    "snap", "cannon", "DivPlunder", "Boing", "HavenTalk", "HavenKnife",
    "HavenMustache", "BagCatch", "BagFlip", "Pencil", "BagOMoney",
)


def find_strings_all(pe, predicate, min_len=3):
    """Like xref_scan.find_strings but a shorter min_len (sound names are short)."""
    out = []
    for s in pe.sections:
        if s["name"] not in (".rdata", ".data"):
            continue
        blob = pe.data[s["rp"]:s["rp"] + s["rs"]]
        start = None
        for i, b in enumerate(blob):
            if 0x20 <= b < 0x7F:
                if start is None:
                    start = i
            else:
                if start is not None and i - start >= min_len:
                    txt = blob[start:i].decode("ascii")
                    if predicate(txt):
                        out.append((pe.imagebase + s["va"] + start, txt))
                start = None
    return out


def import_table(pe):
    """Yield (dll_name, func_name, iat_va) for every imported symbol."""
    rva, size = struct.unpack_from("<II", pe.data, pe.datadir + 8)  # import dir = entry 1
    base = pe.va_to_off(pe.imagebase + rva)
    out = []
    i = 0
    while True:
        o = base + i * 20
        ilt, _t, _f, name_rva, iat_rva = struct.unpack_from("<IIIII", pe.data, o)
        if name_rva == 0 and iat_rva == 0:
            break
        dll_off = pe.va_to_off(pe.imagebase + name_rva)
        e = dll_off
        while pe.data[e]:
            e += 1
        dll = pe.data[dll_off:e].decode("ascii", "replace")

        thunk_rva = ilt or iat_rva
        t = pe.va_to_off(pe.imagebase + thunk_rva)
        slot = pe.imagebase + iat_rva
        while True:
            val = struct.unpack_from("<I", pe.data, t)[0]
            if val == 0:
                break
            if val & 0x80000000:
                fname = f"#ordinal_{val & 0xFFFF}"
            else:
                no = pe.va_to_off(pe.imagebase + val) + 2  # skip hint
                e = no
                while pe.data[e]:
                    e += 1
                fname = pe.data[no:e].decode("ascii", "replace")
            out.append((dll, fname, slot))
            t += 4
            slot += 4
        i += 1
    return out


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else \
        r"C:\Users\Shadow\Projects\PiratesMod\re\bin\Pirates_gog.exe"
    pe = PE(path)
    print(f"# {path}")
    print(f"# imagebase=0x{pe.imagebase:X}\n")

    # --- 1. Miles / audio imports ------------------------------------------
    imports = import_table(pe)
    print("=== Mss32.dll (Miles) imports ===")
    miles = [x for x in imports if x[0].lower().startswith("mss32")]
    for dll, fn, slot in miles:
        print(f"  {fn:<28} IAT 0x{slot:08X}")
    if not miles:
        print("  (none found by name -- may be delay/dynamically loaded)")
    print(f"  total audio imports: {len(miles)}\n")

    # --- 2. sound-name strings ---------------------------------------------
    def looks_like_sound(t):
        tl = t.lower()
        if tl.endswith(".wav") or "/sounds/" in tl or tl.startswith("carib"):
            return True
        return any(tok.lower() == tl for tok in KNOWN_SOUND_TOKENS)

    sounds = find_strings_all(pe, looks_like_sound)
    # de-dup by va
    seen = set()
    sounds = [(va, t) for va, t in sounds if not (va in seen or seen.add(va))]
    print(f"=== sound-name strings found: {len(sounds)} (showing up to 30) ===")
    for va, t in sounds[:30]:
        print(f"  0x{va:08X}  {t}")
    print()

    # --- 3. cluster call targets after a push of a sound name --------------
    targets = Counter()
    per_target = defaultdict(list)
    referenced = 0
    for va, t in sounds:
        refs = find_dword_refs(pe, va)
        if refs:
            referenced += 1
        for r in refs:
            tgt = next_call_target(pe, r, max_scan=48)
            if tgt:
                targets[tgt] += 1
                per_target[tgt].append((va, t))

    print(f"referenced sound strings: {referenced}/{len(sounds)}")
    print("\n=== top CALL targets following a sound-name push ===")
    print("    (the play-sound-by-name function should dominate)")
    for tgt, n in targets.most_common(12):
        print(f"  0x{tgt:08X}  hits={n}")
        for va, t in per_target[tgt][:4]:
            print(f"        e.g. 0x{va:08X} {t}")


if __name__ == "__main__":
    main()
