# Game assets — what they are, and how to get at them

The game ships roughly 840 MB of art, models, animation and text sealed in ten
`.FPK` archives under `Assets\`:

| File | Size | Holds |
|---|---|---|
| `Pak0`–`Pak7.FPK` | ~100 MB each | textures (`.dds`), models (`.nif`), animation (`.kf`, `.kfm`) |
| `Pak8.FPK` | 8.5 MB | the tail of the set |
| `lang0.FPK` | 372 KB | localised text — ship names, city names, the pedia, credits |

Loose assets sit outside the archives in `Assets\Sounds\`, `Assets\Shaders\FX\`,
`Assets\Interface\` and `custom\`.

---

## The short answer

**Extracting and rebuilding these archives is solved and verified.** `tools\fpk.py`
does both directions, and a round-trip is byte-exact.

**But reach for a loose file first.** Several subsystems look on disk before they
look in the archive, and where that is true you can override a packed asset by
dropping a file next to the game — no repacking, no modified game file, nothing to
undo. Repacking is the fallback for the subsystems that do not.

---

## The format

```
u32  version            always 2
u32  asset_count
asset_count x {
    u32   filename_length
    char  filename[round_up(filename_length, 4)]
    u32   checksum
    u32   tag
    u32   length
    u32   offset
}
...file bodies in offset order...
```

Three details matter, and all three are easy to get wrong:

- **Filenames are obfuscated** — every byte is stored incremented by one.
- **The name padding is not zeroes.** Padding of *n* bytes is `bytes([n])` followed
  by *n−1* zeroes, so the table of padding values is indexed by **padding length**,
  never by `len(name) % 4`. The two happen to agree at length 2 and nowhere else.
- **The namespace is flat.** No directories, and no entry can escape the output
  folder — the extractor rejects any name containing a separator.

Everything the index does not describe — alignment gaps between bodies, trailing
bytes — is preserved verbatim in the sidecar as `padding` parts. That is what makes
the round-trip exact rather than merely equivalent.

`tag` is a 32-bit field carried through unexamined. What it selects is not known.

---

## `tools\fpk.py`

```
python tools\fpk.py d <dir> <file.FPK>      extract, writing <dir>_db.json
python tools\fpk.py a <dir> <file.FPK>      rebuild from <dir> and its sidecar
```

The sidecar `<dir>_db.json` is the index — checksums, tags and the padding — and
**rebuilding needs it**. Keep it beside the extracted folder.

Verified on `lang0.FPK`:

- extract → assemble reproduces the original **byte for byte**
- a genuinely modified file rebuilds correctly: 380,720 → 380,794 bytes after adding
  37 UTF-16 characters to `shipnames_enu.txt`, the archive re-extracts cleanly, the
  marker is present, and every neighbouring file is unchanged

### Two bugs fixed from the inherited version

The copy in `_modtools\scripts\fpkextract.py` extracts correctly but **cannot
assemble** — and fails destructively.

1. **Header size counted filename blocks as bytes.** The field is a whole number of
   4-byte blocks, so the header came out roughly three-quarters of a filename short
   per entry. It surfaced only at a closing assert, *after* the archive had been
   written.
2. **Padding indexed by `len % 4`** instead of by padding length, so names of most
   lengths round-tripped to different bytes.

Both are fixed in `tools\fpk.py`, with the reasoning at the site.

### Failure is now non-destructive

The inherited tool opened the target `"wb"` — which truncates on open. A fault in
assembly therefore left a truncated file where a 100 MB game asset had been, with
nothing to restore from. `tools\fpk.py` builds beside the target and moves into
place only on success. Verified: an assemble that fails leaves the archive
byte-identical and no leftover temporary.

---

## Loose-file override — per subsystem, not global

Measured with the startup-armed file probe (`PEMF\fileprobe.on`), which logs every
file the game opens **and every one it fails to open**. The misses are the evidence:
a probe for a file that is not there is proof that putting one there would be read.

| Asset | Looks on disk first? |
|---|---|
| `Data\AdvancedLighting.ini` | **Yes** — missed on disk, then read from the archive |
| `assets\data\Landscape.ini` | **Yes** — same |
| `custom\flag_*.dds`, sail emblems | **Yes** — this is how custom flags have always worked |
| `Assets\Sounds\*` | **Loose already**, never archived |
| `text.ini` | **No** — no disk probe at all |

`text.ini` is the important negative. It lives in `Pak1.FPK` alongside the two
`.ini` files that *are* probed, so the absence of a probe is a real finding rather
than a hole in the instrumentation. Item names cannot be extended with a loose file;
that would need a repack or a patch of the parsed table in memory.

**So: probe before you repack.** Arm `fileprobe.on`, start the game, and look for a
miss on the path you care about. A miss means a loose file wins.

---

## The open question: does the engine check the checksum?

`tools\fpk.py` carries `checksum` through from the index rather than recomputing it,
because the algorithm has not been identified. A file whose contents you changed
goes back in **under its original checksum**.

That has not been tested in a running game. Two outcomes:

- The engine ignores the field, and modified archives simply work.
- The engine validates it, and the algorithm has to be recovered before repacking is
  usable at all.

The cheap experiment settles it: repack `lang0.FPK` with a visibly altered ship name
and start the game. A changed name on screen answers it in one run. `lang0.FPK` is
the right subject — 372 KB, text only, and nothing structural depends on it.

Until that is answered, treat repacking as unproven.

---

## Policy

**PEMF does not ship modified game files, and the framework never writes to one.**
`tools\fpk.py` is for inspection and for authors who choose to build content; it is
not part of the runtime and nothing in `src\` calls it.

If you do repack, back the original up first. These archives come with the game and
a bad write is not recoverable from the install.
