"""
find_safe_point.py - locate timeGetTime call sites inside the main loop.

Our per-frame hook currently acts on ANY of the ~84 timeGetTime call sites,
which means we can be anywhere in the frame -- possibly mid-render or holding a
lock -- when we present a modal dialog. If one call site lives in the main loop
(FUN_0042E1D0) we can filter on the return address and get a known-safe point
without adding an inline hook.
"""
from collections import Counter
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from xref_scan import PE, find_dword_refs

EXE = r"C:\Users\Shadow\Projects\PiratesMod\re\bin\Pirates_gog.exe"
IAT_TIMEGETTIME = 0x006C0430
MAIN_LOOP = (0x0042C000, 0x0042F000)


def main():
    pe = PE(EXE)
    md = Cs(CS_ARCH_X86, CS_MODE_32)

    refs = find_dword_refs(pe, IAT_TIMEGETTIME)
    # `call dword ptr [imm32]` is FF 15 <imm32>, so the instruction starts 2
    # bytes before the operand we matched.
    sites = sorted(v - 2 for v in (pe.off_to_va(r) for r in refs) if v)
    print(f"timeGetTime call sites: {len(sites)}")

    print(f"\n=== sites inside the main-loop region "
          f"0x{MAIN_LOOP[0]:08X}-0x{MAIN_LOOP[1]:08X} ===")
    found = 0
    for s in sites:
        if MAIN_LOOP[0] <= s <= MAIN_LOOP[1]:
            found += 1
            off = pe.va_to_off(s - 24)
            ins = [i for i in md.disasm(pe.data[off:off + 48], s - 24)
                   if i.address <= s]
            ctx = " | ".join(f"{i.mnemonic} {i.op_str}" for i in ins[-4:])
            # return address is the instruction after the 6-byte call
            print(f"  call 0x{s:08X}  ret=0x{s+6:08X}")
            print(f"      {ctx}")
    if not found:
        print("  (none)")

    print("\n=== all sites by page ===")
    c = Counter((s >> 12) << 12 for s in sites)
    for page, n in sorted(c.items()):
        bar = "#" * n
        print(f"  0x{page:08X}  {n:>3}  {bar}")


if __name__ == "__main__":
    main()
