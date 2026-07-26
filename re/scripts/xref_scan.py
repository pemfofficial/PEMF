"""
xref_scan.py - dependency-free PE string-xref + call-target clustering.

Goal: locate the "show event card" presenter function in Pirates!.exe by
finding every @NUM/@HAPPY template string, locating the code that pushes its
address, and clustering the call targets that follow.

The most frequently called target across many template strings is almost
certainly the shared presenter/formatter.
"""
import struct
import sys
from collections import Counter, defaultdict


class PE:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        self.pe = pe
        assert self.data[pe:pe + 4] == b"PE\0\0", "not a PE"
        self.machine = struct.unpack_from("<H", self.data, pe + 4)[0]
        nsec = struct.unpack_from("<H", self.data, pe + 6)[0]
        optsz = struct.unpack_from("<H", self.data, pe + 20)[0]
        magic = struct.unpack_from("<H", self.data, pe + 24)[0]
        self.is64 = magic == 0x20B
        if self.is64:
            self.imagebase = struct.unpack_from("<Q", self.data, pe + 24 + 24)[0]
            self.datadir = pe + 24 + 112
        else:
            self.imagebase = struct.unpack_from("<I", self.data, pe + 24 + 28)[0]
            self.datadir = pe + 24 + 96
        so = pe + 24 + optsz
        self.sections = []
        for i in range(nsec):
            o = so + i * 40
            name = self.data[o:o + 8].rstrip(b"\0").decode("ascii", "replace")
            vs, va, rs, rp = struct.unpack_from("<IIII", self.data, o + 8)
            chars = struct.unpack_from("<I", self.data, o + 36)[0]
            self.sections.append(
                dict(name=name, vs=vs, va=va, rs=rs, rp=rp, chars=chars)
            )

    def sec_by_name(self, name):
        for s in self.sections:
            if s["name"] == name:
                return s
        return None

    def va_to_off(self, va):
        rva = va - self.imagebase
        for s in self.sections:
            if s["va"] <= rva < s["va"] + max(s["vs"], s["rs"]):
                off = s["rp"] + (rva - s["va"])
                return off if off < len(self.data) else None
        return None

    def off_to_va(self, off):
        for s in self.sections:
            if s["rp"] <= off < s["rp"] + s["rs"]:
                return self.imagebase + s["va"] + (off - s["rp"])
        return None


def find_strings(pe, predicate, min_len=6):
    """Yield (va, text) for ASCII strings in read-only/data sections."""
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


def find_dword_refs(pe, value):
    """Offsets in .text where the literal 4-byte value appears."""
    text = pe.sec_by_name(".text")
    blob = pe.data[text["rp"]:text["rp"] + text["rs"]]
    needle = struct.pack("<I", value)
    hits = []
    i = blob.find(needle)
    while i != -1:
        hits.append(text["rp"] + i)
        i = blob.find(needle, i + 1)
    return hits


def next_call_target(pe, off, max_scan=64):
    """
    From a push-site, scan forward for the first E8 rel32 CALL and
    return its absolute target VA.
    """
    text = pe.sec_by_name(".text")
    end = text["rp"] + text["rs"]
    i = off
    limit = min(off + max_scan, end - 5)
    while i < limit:
        if pe.data[i] == 0xE8:
            rel = struct.unpack_from("<i", pe.data, i + 1)[0]
            src_va = pe.off_to_va(i)
            if src_va is None:
                return None
            target = src_va + 5 + rel
            # sanity: target must land inside .text
            if pe.va_to_off(target) is not None:
                return target
        i += 1
    return None


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else \
        r"C:\Users\Shadow\Projects\PiratesMod\re\bin\Pirates_gog.exe"
    pe = PE(path)
    print(f"# {path}")
    print(f"# imagebase=0x{pe.imagebase:X} machine=0x{pe.machine:X}")

    tmpl = find_strings(pe, lambda t: "@NUM" in t or "@HAPPY" in t or "@NAME" in t)
    print(f"\n=== template strings found: {len(tmpl)} ===")
    for va, t in tmpl[:15]:
        print(f"  0x{va:08X}  {t[:88]}")
    if len(tmpl) > 15:
        print(f"  ... and {len(tmpl)-15} more")

    # cluster call targets that immediately follow a push of a template string
    targets = Counter()
    per_target = defaultdict(list)
    unreferenced = 0
    for va, t in tmpl:
        refs = find_dword_refs(pe, va)
        if not refs:
            unreferenced += 1
            continue
        for r in refs:
            tgt = next_call_target(pe, r)
            if tgt:
                targets[tgt] += 1
                per_target[tgt].append((va, t))

    print(f"\n=== template strings with no code ref: {unreferenced} ===")
    print("\n=== top call targets following a template-string push ===")
    for tgt, n in targets.most_common(12):
        print(f"  0x{tgt:08X}  hits={n}")
        for va, t in per_target[tgt][:3]:
            print(f"        e.g. 0x{va:08X} {t[:70]}")

    # specifically trace the mutiny string
    print("\n=== mutiny/morale anchor strings ===")
    anchors = find_strings(
        pe,
        lambda t: any(k in t.lower() for k in
                      ("crew is mutinous", "your crew of", "morale +",
                       "men are starving", "loot is divided")),
    )
    for va, t in anchors:
        refs = find_dword_refs(pe, va)
        print(f"  0x{va:08X} refs={len(refs)}  {t[:70]}")
        for r in refs:
            rva = pe.off_to_va(r)
            tgt = next_call_target(pe, r)
            ts = f"0x{tgt:08X}" if tgt else "None"
            print(f"        push@0x{rva:08X} -> call {ts}")


if __name__ == "__main__":
    main()
