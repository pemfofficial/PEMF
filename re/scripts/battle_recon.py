"""
battle_recon.py - locate the ship-battle instance: how the game enters combat,
where in-battle ship state lives, and the win/lose logic. Groundwork for a
race mode that reuses the battle arena.

Dependency-free.
"""
from collections import defaultdict

from xref_scan import PE, find_dword_refs, find_strings
from trace_sound2 import function_starts, enclosing, e8_callers

PE_PATH = r"C:\Users\Shadow\Projects\PiratesMod\re\bin\Pirates_gog.exe"

# strings that only appear in / near ship combat
BATTLE_KEYS = (
    "battle sails", "full sail", "close to board", "prepare to board",
    "surrender", "strike your colors", "strikes her colors", "sink",
    "grape", "chain shot", "round shot", "broadside", "rake",
    "wind gauge", "enemy ship", "man-o-war", "war galleon", "flee",
    "gun deck", "gunners", "sails and rigging", "hull", "flag of truce",
)


def main():
    pe = PE(PE_PATH)
    starts = function_starts(pe)

    print("=== combat strings -> enclosing functions ===")
    fn_hits = defaultdict(list)
    hits = find_strings(pe, lambda t: any(k in t.lower() for k in BATTLE_KEYS),
                        min_len=4)
    for va, t in hits:
        for r in find_dword_refs(pe, va):
            f = enclosing(starts, pe.off_to_va(r))
            if f:
                fn_hits[f].append(t[:40])
    for f, ts in sorted(fn_hits.items(), key=lambda kv: -len(kv[1]))[:20]:
        sample = "; ".join(dict.fromkeys(ts))[:90]
        print(f"  FUNC 0x{f:08X}  x{len(ts):<2}  {sample}")

    print(f"\n  ({len(hits)} combat strings total)")


if __name__ == "__main__":
    main()
