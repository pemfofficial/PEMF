"""
callers_of.py <hex_va> - find every `call rel32` to a target and show the
instructions that set up registers immediately before it.

Used to work out non-standard calling conventions: what does the game actually
put in ecx/edx/eax before this call?
"""
import struct
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from xref_scan import PE

DEFAULT = r"C:\Users\Shadow\Projects\PiratesMod\re\bin\Pirates_gog.exe"


def main():
    target = int(sys.argv[1], 16)
    back = int(sys.argv[2]) if len(sys.argv) > 2 else 7
    pe = PE(DEFAULT)
    text = pe.sec_by_name(".text")
    blob = pe.data[text["rp"]:text["rp"] + text["rs"]]
    base_va = pe.imagebase + text["va"]

    sites = []
    for i in range(len(blob) - 5):
        if blob[i] != 0xE8:
            continue
        rel = struct.unpack_from("<i", blob, i + 1)[0]
        if base_va + i + 5 + rel == target:
            sites.append(base_va + i)

    print(f"target 0x{target:08X}: {len(sites)} call sites\n")
    md = Cs(CS_ARCH_X86, CS_MODE_32)

    for n, site in enumerate(sites, 1):
        # Disassemble a window before the call and keep the last `back` insns.
        start = site - 48
        off = pe.va_to_off(start)
        insns = [i for i in md.disasm(pe.data[off:off + 48 + 8], start)
                 if i.address <= site]
        print(f"--- site {n}: 0x{site:08X} ---")
        for i in insns[-back:]:
            mark = "  <== CALL" if i.address == site else ""
            print(f"    0x{i.address:08X}  {i.mnemonic:<7}{i.op_str}{mark}")
        print()


if __name__ == "__main__":
    main()
