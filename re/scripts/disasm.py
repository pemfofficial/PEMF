"""disasm.py - disassemble a VA range in the target PE."""
import sys
import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from xref_scan import PE

DEFAULT = r"C:\Users\Shadow\Projects\PiratesMod\re\bin\Pirates_gog.exe"


def dis(pe, va, count=40, label=""):
    off = pe.va_to_off(va)
    if off is None:
        print(f"!! VA 0x{va:X} not mapped")
        return
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = False
    blob = pe.data[off:off + count * 8]
    print(f"\n===== {label} 0x{va:08X} (file off 0x{off:X}) =====")
    n = 0
    for i in md.disasm(blob, va):
        print(f"  0x{i.address:08X}  {i.bytes.hex():<20} {i.mnemonic} {i.op_str}")
        n += 1
        if n >= count:
            break


if __name__ == "__main__":
    pe = PE(DEFAULT)
    args = sys.argv[1:]
    if args:
        for a in args:
            dis(pe, int(a, 16), 40)
    else:
        dis(pe, 0x004F6090, 24, "CANDIDATE A")
        dis(pe, 0x004F60B0, 24, "CANDIDATE B")
        dis(pe, 0x004125A0, 30, "mutiny call site")
        dis(pe, 0x00443DC0, 24, "'crew of @NUM is @HAPPY' site")
