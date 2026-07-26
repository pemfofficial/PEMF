"""Count code references to an imported function's IAT slot."""
import struct
import sys
from xref_scan import PE, find_dword_refs

DEFAULT = r"C:\Users\Shadow\Projects\PiratesMod\re\bin\Pirates_gog.exe"


def cstr(data, off):
    e = off
    while data[e]:
        e += 1
    return data[off:e].decode("ascii", "replace")


def iat_slots(pe):
    """-> {(dll, func): iat_va}"""
    dd = pe.pe + 24 + 96
    imp_rva = struct.unpack_from("<I", pe.data, dd + 8)[0]
    o = pe.va_to_off(pe.imagebase + imp_rva)
    out = {}
    while True:
        oft, _, _, name_rva, ft = struct.unpack_from("<IIIII", pe.data, o)
        if name_rva == 0:
            break
        dll = cstr(pe.data, pe.va_to_off(pe.imagebase + name_rva))
        thunk = oft if oft else ft
        t = pe.va_to_off(pe.imagebase + thunk)
        slot_va = pe.imagebase + ft
        while True:
            entry = struct.unpack_from("<I", pe.data, t)[0]
            if entry == 0:
                break
            if not (entry & 0x80000000):
                hn = pe.va_to_off(pe.imagebase + entry)
                out[(dll.lower(), cstr(pe.data, hn + 2))] = slot_va
            t += 4
            slot_va += 4
        o += 20
    return out


def main():
    pe = PE(DEFAULT)
    slots = iat_slots(pe)
    wanted = sys.argv[1:] or ["timeGetTime", "joyGetPosEx", "mmioRead",
                              "GetTickCount", "QueryPerformanceCounter",
                              "PeekMessageA", "GetCursorPos", "Sleep"]
    print(f"{'function':<26} {'IAT slot':<12} {'call sites'}")
    print("-" * 52)
    for name in wanted:
        hit = [(d, f, va) for (d, f), va in slots.items() if f == name]
        if not hit:
            print(f"{name:<26} {'not imported':<12} -")
            continue
        for d, f, va in hit:
            refs = find_dword_refs(pe, va)
            print(f"{f:<26} 0x{va:08X}   {len(refs)}   ({d})")


if __name__ == "__main__":
    main()
