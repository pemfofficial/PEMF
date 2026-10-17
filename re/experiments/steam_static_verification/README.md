# Checking PEMF against the Steam executable without launching it

## The short version

The Steam `Pirates!.exe` is packed on disk, so until now the only way to know
that PEMF's addresses were right on Steam was to run the game and read the
log. A separate reference workspace (`D:\pirates-reference`) has since rebuilt
the **unpacked Steam image** by decoding the packer statically. Its loader
checksum matches, so the image is what the game runs. With that image every
address PEMF uses can be checked from files alone.

**Result: PEMF's addresses hold on Steam.** All 417 absolute addresses in the
source, all 32 byte signatures it checks, and every code patch site agree with
the Steam image. The one exception is a constant that the source declares and
never reads (`WorldTextTilt`).

**But Steam is not byte-for-byte the GOG build, which the docs had assumed.**
The code is the same, at the same addresses, and so is every piece of game
state (crew, plunder, ships, nations and so on). Some read-only data (vtables,
float constants, a few strings) sits at different addresses in the two
builds. Any address in `0x006C0000-0x00716FFF` taken from the GOG file has to
be checked against Steam before it is used.

## What was compared

| Input | Identity |
|---|---|
| GOG `Pirates!.exe` | SHA-256 `6E88B90E…`, the build PEMF was mapped on |
| Steam `Pirates!.exe` (packed, on disk) | SHA-256 `5342209C…`, the build players run |
| Unpacked Steam image | `D:\pirates-reference\raw\binaries\Pirates.analysis.exe`, SHA-256 `E09ADBB9…` |
| Steam disassembly | `D:\pirates-reference\evidence\ghidra\Pirates.analysis.exe\instructions.jsonl` (Ghidra 12.1.2) |

Both executables report version 1.0.2.0 (30927) and the build string
`Pirates! 21031`.

## Findings

1. **Code is the same.** Of 2,878,602 code bytes, 2,873,268 are identical.
   Almost every difference is a 4-byte pointer to read-only data: 599
   distinct pointers, all from `.rdata` to `.rdata`. None point at `.data` or
   uninitialised memory, so all game state is at the same address in both
   builds.
2. **Read-only data moved.** Two vtables at `0x6F5814` and `0x6F583C` swap
   places between the builds. Runs of float constants are shifted, mostly by
   8 or 24 bytes. Full map: `results/relocated_rdata.json`, as GOG → Steam
   pairs.
3. **One small code difference.** Steam has an extra 6-byte function at
   `0x004EDAC0` (`mov eax, 0x727898; ret`), which pushes the next three small
   functions 16 bytes later than in GOG, up to `0x004EDB60`. PEMF uses nothing
   in that window.
4. **The packer is PECompact 2, not SteamStub.** The file has a `PEC2` marker,
   no `.bind` section and no Steam API. The rule PEMF already follows still
   stands: write no code before the first safe point, because the loader
   checksums code while it unpacks.

## PEMF checks

| Check | Result |
|---|---|
| Absolute addresses in `src/` and `sdk/` | 417 (292 code, 7 read-only data, 27 data, 91 uninitialised). The bytes PEMF reads, writes or checks are the same on Steam for all of them except `WorldTextTilt` |
| Code addresses: instruction start or operand | 281 starts, 9 operands (immediate/address rewrites), 2 jump tables (data). None land mid-instruction |
| Byte signatures in the source | 32 of 32 found on Steam at the address where PEMF checks them. One more array is patch data PEMF writes (`storms.h`, rain), not a check |
| Set-frustum call sites (`widescreen.h`) | Exactly 24 `E8` calls to `0x004CCE40` on Steam, all 24 listed |
| Pirate start, combat effects | Patch sites, stack offsets, registers and argument counts checked against Steam disassembly: correct |

Seven source addresses are flagged by the 32-byte window. Six are not
problems: the difference falls beyond what PEMF touches (a 1-byte write, a
5-byte call patch, a call target), or the address only appears in a comment.
The seventh is `WorldTextTilt` (`0x00713600`, a double). On Steam that
constant is at `0x007135F8`, and `0x00713600` holds two unrelated floats. PEMF
never reads it (it uses `kDegToBam`), so nothing breaks, but the address is
wrong for Steam.

## Docs that cite GOG-only data addresses

From `results/doc_addresses.json`; none of these are used by the source:

- `WIDESCREEN.md`: the UI's 1024 and 768 scale constants are at `0x007135C4`
  / `0x007135C8` in GOG but `0x007135BC` / `0x007135C0` on Steam. The ten
  instructions cited there that read them (`0x00446BD1`, `0x00446D5F`, `0x005038ED`,
  `0x00503985`, `0x0050436D`, `0x005043C8`, `0x0050C7EF`, `0x0050C841`,
  `0x0050CAFC`, `0x0050CB40`) carry the Steam addresses on Steam. A patch that
  rewrote the constants by GOG address would hit the wrong data on Steam.
  Patching the instructions' operands is safe if the old operand is compared
  against the build's own value.
- `GAME_API.md`: `0x00712E74` and `0x00712F68` (strings) are at `0x00712E6C`
  and `0x00712F60` on Steam. `WorldTextTilt`, as above.

## Re-running

```
python re\experiments\steam_static_verification\verify_steam.py
```

It needs the GOG exe and the reference workspace's unpacked image and
disassembly (paths are arguments). It reads only and writes `results\`. Run
it after adding any address, and before a release.
