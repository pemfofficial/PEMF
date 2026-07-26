"""Dump the import table (per-DLL function names) of the target PE."""
import struct
import sys
from xref_scan import PE

DEFAULT = r"C:\Users\Shadow\Projects\PiratesMod\re\bin\Pirates_gog.exe"


def cstr(data, off):
    end = off
    while data[end] != 0:
        end += 1
    return data[off:end].decode("ascii", "replace")


def main():
    pe = PE(sys.argv[1] if len(sys.argv) > 1 else DEFAULT)
    dd = pe.pe + 24 + 96
    imp_rva = struct.unpack_from("<I", pe.data, dd + 8)[0]
    o = pe.va_to_off(pe.imagebase + imp_rva)

    while True:
        oft, _, _, name_rva, ft = struct.unpack_from("<IIIII", pe.data, o)
        if name_rva == 0:
            break
        dll = cstr(pe.data, pe.va_to_off(pe.imagebase + name_rva))
        thunk_rva = oft if oft else ft
        t = pe.va_to_off(pe.imagebase + thunk_rva)
        funcs = []
        while True:
            entry = struct.unpack_from("<I", pe.data, t)[0]
            if entry == 0:
                break
            if entry & 0x80000000:
                funcs.append(f"#{entry & 0xFFFF}  (ordinal)")
            else:
                hn = pe.va_to_off(pe.imagebase + entry)
                funcs.append(cstr(pe.data, hn + 2))
            t += 4
        print(f"\n=== {dll}  ({len(funcs)} imports) ===")
        for f in funcs:
            print(f"    {f}")
        o += 20


if __name__ == "__main__":
    main()
