"""Dump the export table of a PE (used to verify the version.dll proxy)."""
import struct
import sys
from xref_scan import PE


def main(path):
    pe = PE(path)
    dd = pe.pe + 24 + 96
    rva, size = struct.unpack_from("<II", pe.data, dd)
    if not rva:
        print(f"{path}: no export directory")
        return
    o = pe.va_to_off(pe.imagebase + rva)
    # IMAGE_EXPORT_DIRECTORY: Characteristics, TimeDateStamp, Major+Minor,
    # Name, Base, NumberOfFunctions, NumberOfNames, AddressOfFunctions,
    # AddressOfNames, AddressOfNameOrdinals
    (_chars, _tds, _ver, name_rva, ordinal_base, n_funcs, n_names,
     addr_rva, names_rva, ords_rva) = struct.unpack_from("<IIIIIIIIII", pe.data, o)

    def cstr(off):
        e = off
        while pe.data[e]:
            e += 1
        return pe.data[off:e].decode()

    dll = cstr(pe.va_to_off(pe.imagebase + name_rva))
    print(f"=== {path}")
    print(f"    internal name: {dll}   exports: {n_names}")
    no = pe.va_to_off(pe.imagebase + names_rva)
    oo = pe.va_to_off(pe.imagebase + ords_rva)
    ao = pe.va_to_off(pe.imagebase + addr_rva)
    for i in range(n_names):
        nrva = struct.unpack_from("<I", pe.data, no + i * 4)[0]
        ordn = struct.unpack_from("<H", pe.data, oo + i * 2)[0]
        frva = struct.unpack_from("<I", pe.data, ao + ordn * 4)[0]
        print(f"    #{ordn + ordinal_base:<4} {cstr(pe.va_to_off(pe.imagebase + nrva)):<28} rva=0x{frva:X}")


if __name__ == "__main__":
    for p in sys.argv[1:]:
        main(p)
