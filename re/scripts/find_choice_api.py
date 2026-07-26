"""
find_choice_api.py - locate the game's choice/menu presentation functions.

Two candidate idioms spotted in the strings:
  * yes/no prompts  -- "Do you wish to form a landing party and go ashore?"
  * menu options    -- strings with a LEADING SPACE, e.g. " Divide the Plunder",
                       " Accept her invitation."

For each, find where the string address is pushed and which function is called
next, then cluster.
"""
from collections import Counter, defaultdict
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from xref_scan import PE, find_strings, find_dword_refs, next_call_target

EXE = r"C:\Users\Shadow\Projects\PiratesMod\re\bin\Pirates_gog.exe"

QUESTIONS = ("do you wish", "would you like", "will you be")


def cluster(pe, strs, label):
    targets = Counter()
    examples = defaultdict(list)
    for va, t in strs:
        for r in find_dword_refs(pe, va):
            tgt = next_call_target(pe, r, max_scan=96)
            if tgt:
                targets[tgt] += 1
                examples[tgt].append(t)
    print(f"\n=== {label}: {len(strs)} strings ===")
    for tgt, n in targets.most_common(8):
        print(f"  0x{tgt:08X}  hits={n}")
        for e in examples[tgt][:3]:
            print(f"        {e[:70]!r}")
    return targets


def main():
    pe = PE(EXE)

    qs = find_strings(pe, lambda t: any(q in t.lower() for q in QUESTIONS))
    cluster(pe, qs, "yes/no question prompts")

    # Menu options: leading space, capitalised word, no format tokens.
    opts = find_strings(
        pe,
        lambda t: (len(t) > 6 and t[0] == ' ' and t[1].isupper()
                   and '@' not in t and '%' not in t and '.dds' not in t
                   and '\\' not in t and '/' not in t),
        min_len=8,
    )
    print(f"\n(found {len(opts)} leading-space option-like strings)")
    for va, t in opts[:20]:
        print(f"    0x{va:08X} {t!r}")
    cluster(pe, opts, "leading-space menu options")


if __name__ == "__main__":
    main()
