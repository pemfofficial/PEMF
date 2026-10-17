"""Static verification of PEMF against the unpacked Steam executable.

The Steam Pirates!.exe is PECompact-packed on disk, so its code cannot be read from the file.
A separate reference workspace (D:\\pirates-reference) rebuilt the unpacked image by static
decoding (checksum-verified against the loader). This script uses that image to check, without
launching anything:

  1. parity   - every byte of the GOG exe against the Steam image at the same address
  2. relocs   - which .rdata items moved between the two builds (from differing pointer operands)
  3. source   - every absolute address in PEMF source: GOG==Steam over 32 bytes, moved .rdata items
  4. bounds   - every code address is an instruction start or an operand inside one (Steam disasm)
  5. sigs     - every byte signature PEMF checks is present on Steam where PEMF checks it
  6. docs     - every address cited in docs/ and re/out/offsets.json, same test as 3

Read-only. Writes results/*.json next to this file and prints a summary.
Usage: python verify_steam.py [--gog PATH] [--steam PATH] [--disasm PATH]
"""
import argparse, bisect, collections, json, pathlib, re, struct, sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[2]
ap = argparse.ArgumentParser()
ap.add_argument('--gog', default=r"C:\GOG Games\Sid Meier's Pirates\Pirates!.exe")
ap.add_argument('--steam', default=r'D:\pirates-reference\raw\binaries\Pirates.analysis.exe')
ap.add_argument('--disasm', default=r'D:\pirates-reference\evidence\ghidra\Pirates.analysis.exe\instructions.jsonl')
a = ap.parse_args()
OUT = HERE / 'results'; OUT.mkdir(exist_ok=True)

g = pathlib.Path(a.gog).read_bytes()
s = pathlib.Path(a.steam).read_bytes()


def sections(b):
    pe = struct.unpack_from('<I', b, 0x3c)[0]
    n = struct.unpack_from('<H', b, pe + 6)[0]; opt = struct.unpack_from('<H', b, pe + 20)[0]
    out = []
    for i in range(n):
        o = pe + 24 + opt + 40 * i
        vs, va, rs, ro = struct.unpack_from('<IIII', b, o + 8)
        out.append((b[o:o + 8].rstrip(b'\0').decode('latin1'), va, vs, ro, rs))
    return out


GSEC, SSEC = sections(g), sections(s)


def read(b, secs, va, n):
    r = va - 0x400000
    for _, sva, _, ro, rs in secs:
        if sva <= r < sva + rs:
            return b[ro + r - sva: ro + min(r - sva + n, rs)]
    return None


def region(va):
    return 'text' if va < 0x6C0000 else 'rdata' if va < 0x717000 else 'data' if va < 0x730000 else 'bss'


IAT = (0x6C0000, 0x6C0600)   # import address table: GOG holds pre-load thunks, the unpacked image zeros

# 1. parity ------------------------------------------------------------------------------------
parity = []
for name, va, vs, ro, rs in GSEC:
    same = diff = 0
    for i in range(min(vs, rs)):
        sb = read(s, SSEC, 0x400000 + va + i, 1)
        if sb is None:
            continue
        if sb[0] == g[ro + i]:
            same += 1
        else:
            diff += 1
    parity.append({'section': name, 'va': hex(0x400000 + va), 'identical': same, 'different': diff})

# 2. relocated .rdata items, from differing 4-byte pointer operands in .text --------------------
gt = read(g, GSEC, 0x401000, 0x2C0000 - 0x1000); st = read(s, SSEC, 0x401000, len(gt))
pairs = collections.Counter(); covered = bytearray(len(gt)); unexplained = []
for i in range(len(gt)):
    if gt[i] == st[i] or covered[i]:
        continue
    for k in range(4):
        j = i - k
        if 0 <= j and j + 4 <= len(gt):
            gv, sv = struct.unpack_from('<I', gt, j)[0], struct.unpack_from('<I', st, j)[0]
            if gv != sv and 0x400000 <= gv < 0x900000 and 0x400000 <= sv < 0x900000:
                pairs[(gv, sv)] += 1
                covered[j:j + 4] = b'\x01\x01\x01\x01'
                break
    else:
        unexplained.append(0x401000 + i)
moved = {gv: sv for (gv, sv) in pairs}
reloc = {'operand_pairs': len(pairs),
         'regions': dict(collections.Counter(f'{region(x)}->{region(y)}' for x, y in pairs)),
         'unexplained_code_bytes': len(unexplained),
         'unexplained_ranges': sorted({hex(x & ~0xF) for x in unexplained}),
         'pairs': [[hex(x), hex(y), c] for (x, y), c in sorted(pairs.items())]}

# addresses named in PEMF source and docs ------------------------------------------------------
RX = re.compile(r'0x(00[4-8][0-9A-Fa-f]{5})\b')


def collect(files):
    hits = collections.defaultdict(list)
    for f in files:
        if not f.exists():
            continue
        for ln, line in enumerate(f.read_text(encoding='utf-8', errors='replace').splitlines(), 1):
            for m in RX.finditer(line):
                v = int(m.group(1), 16)
                if 0x401000 <= v < 0x900000:
                    hits[v].append(f'{f.relative_to(REPO).as_posix()}:{ln}')
    return hits


src_files = sorted((REPO / 'src').rglob('*.h')) + sorted((REPO / 'src').rglob('*.cpp')) + [REPO / 'sdk' / 'pemf_sdk.h']
src_files = [f for f in src_files if 'vendor' not in f.parts]
doc_files = sorted((REPO / 'docs').glob('*.md')) + [REPO / 're' / 'out' / 'offsets.json', REPO / 'ROADMAP.md', REPO / 'README.md']
src = collect(src_files); docs = collect(doc_files)


def check(hits):
    rows = []
    for va in sorted(hits):
        reg = region(va); gw = read(g, GSEC, va, 32); sw = read(s, SSEC, va, 32)
        first = None
        if gw and sw:
            first = next((k for k in range(min(len(gw), len(sw))) if gw[k] != sw[k]), None)
        issues = []
        if reg in ('text', 'rdata') and first is not None and not (IAT[0] <= va < IAT[1]):
            issues.append(f'bytes differ at +{first}')
        if va in moved and moved[va] != va:
            issues.append(f'.rdata item is at {moved[va]:#010x} on Steam')
        rows.append({'address': f'{va:#010x}', 'region': reg, 'first_difference': first, 'issues': issues, 'cited_at': hits[va]})
    return rows


src_rows = check(src); doc_rows = check(docs)

# 4. instruction bounds (Steam disassembly) ------------------------------------------------------
ins = {}
with open(a.disasm, encoding='utf-8') as fh:
    for l in fh:
        r = json.loads(l); ins[int(r['address'], 16)] = (r['length'], r['assembly'])
starts = sorted(ins)
bounds = collections.Counter(); odd = []
for r in src_rows:
    if r['region'] != 'text':
        continue
    va = int(r['address'], 16)
    if va in ins:
        bounds['instruction start'] += 1; continue
    i = bisect.bisect_right(starts, va) - 1
    st0 = starts[i]; ln, asm = ins[st0]
    if not st0 < va < st0 + ln:
        bounds['not code (table or data)'] += 1; odd.append([r['address'], 'not inside an instruction', r['cited_at'][0]]); continue
    k = va - st0
    if k in (1, 2, ln - 1, ln - 2, ln - 4) or (ln >= 7 and k == ln - 5):
        bounds['operand inside an instruction'] += 1
    else:
        bounds['unusual offset'] += 1; odd.append([r['address'], f'+{k} into "{asm}" at {st0:#x}', r['cited_at'][0]])

# 5. byte signatures -----------------------------------------------------------------------------
named = set(src)
sigs = []
for f in src_files:
    t = f.read_text(encoding='utf-8', errors='replace')
    found = [(m.start(), bytes.fromhex(m.group(1).replace('\\x', ''))) for m in re.finditer(r'"((?:\\x[0-9A-Fa-f]{2}){3,})"', t)]
    for m in re.finditer(r'(?:uint8_t|unsigned char|BYTE)\s+(\w+)\s*\[\s*\w*\s*\]\s*=\s*\{([^}]*)\}', t):
        vals = re.findall(r"0x([0-9A-Fa-f]{1,2})\b|'(.)'|\b(\d+)\b", m.group(2))
        b = bytes(int(h, 16) if h else ord(c) if c else int(d) for h, c, d in vals)
        if len(b) >= 3:
            found.append((m.start(), b))
    for pos, b in found:
        line = t.count('\n', 0, pos) + 1
        ctx = t[max(0, pos - 300): pos]
        written = bool(re.search(r'blob\s*\[|memcpy\s*\(\s*at', t[pos - 200: pos + 400])) and 'Orig' not in ctx[-80:]
        hits = [i + 0x401000 - 0x400 for i in (mm.start() for mm in re.finditer(re.escape(b), s))]
        at_named = [h for h in hits if any(0 <= h - n <= 32 for n in named)]
        sigs.append({'where': f'{f.relative_to(REPO).as_posix()}:{line}', 'bytes': b.hex(' '), 'found_where_checked': bool(at_named),
                     'note': 'patch bytes PEMF writes, not a check' if (not at_named and written) else ''})

summary = {
    'gog_sha256_prefix': __import__('hashlib').sha256(g).hexdigest()[:16],
    'steam_image_sha256_prefix': __import__('hashlib').sha256(s).hexdigest()[:16],
    'parity': parity,
    'relocated_rdata': {k: v for k, v in reloc.items() if k != 'pairs'},
    'source_addresses': {'total': len(src_rows), 'by_region': dict(collections.Counter(r['region'] for r in src_rows)),
                         'flagged': [r for r in src_rows if r['issues']]},
    'instruction_bounds': {'counts': dict(bounds), 'not_plain': odd},
    'signatures': {'total': len(sigs), 'found_where_checked': sum(1 for x in sigs if x['found_where_checked']),
                   'not_found': [x for x in sigs if not x['found_where_checked']]},
    'doc_addresses': {'total': len(doc_rows), 'flagged': [r for r in doc_rows if r['issues']]},
}
(OUT / 'summary.json').write_text(json.dumps(summary, indent=1))
(OUT / 'relocated_rdata.json').write_text(json.dumps(reloc, indent=1))
(OUT / 'source_addresses.json').write_text(json.dumps(src_rows, indent=1))
(OUT / 'doc_addresses.json').write_text(json.dumps(doc_rows, indent=1))
(OUT / 'signatures.json').write_text(json.dumps(sigs, indent=1))
print(json.dumps({k: v for k, v in summary.items() if k in ('parity', 'relocated_rdata')}, indent=1))
print('source addresses', summary['source_addresses']['total'], 'flagged', len(summary['source_addresses']['flagged']))
for r in summary['source_addresses']['flagged']:
    print('  ', r['address'], r['issues'], r['cited_at'][0])
print('instruction bounds', dict(bounds))
print('signatures', summary['signatures']['total'], 'found where checked', summary['signatures']['found_where_checked'],
      [(x['where'], x['note']) for x in summary['signatures']['not_found']])
print('doc addresses', len(doc_rows), 'flagged', len(summary['doc_addresses']['flagged']))
