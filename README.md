# PEMF — Pirates! Expanded Modding Framework

PEMF is a modding framework for **Sid Meier's Pirates! (2004)**. It adds a
data-driven **narrative event engine** — branching events with real choices and
consequences, authored as JSON — plus expanded crew and morale systems, on top
of the original game.

It loads through a `version.dll` proxy and reaches the game with **IAT hooks
only** — no inline code patching, no modified game files. Events are presented
through the game's **own** text and dialog routines, so they look native.

> **Not affiliated with Firaxis Games, 2K, or Atari.** PEMF ships **no game
> files** and modifies none on disk. You must own a legitimate copy of the game.

## What's here

| Path | What |
|---|---|
| `src/proxy/` | `version.dll` proxy — forwards every real export, loads the core |
| `src/core/` | the framework: hooks, state, session/save, event + trigger engine |
| `content/` | example JSON events |
| `docs/` | player manual, event-authoring guide, developer + API notes |
| `re/scripts/` | the reverse-engineering tools used to map the game (Python + Ghidra) |
| `re/out/offsets.json` | the derived address map (facts about the binary) |
| `build.ps1` | one-step build / deploy / package (MSVC x86) |

The reverse-engineered game binary, the Ghidra database, and full decompilation
dumps are **deliberately not included** — RE for interoperability is one thing,
redistributing the copyrighted binary is another.

## Building

Requires the MSVC **x86** toolset (Visual Studio with the C++ x86/x64 tools) and
the Windows SDK.

```powershell
.\build.ps1                 # build
.\build.ps1 -Deploy         # build + install into the game folder
.\build.ps1 -Package        # build + produce a ready-to-extract zip
```

See [`INSTALL.txt`](INSTALL.txt) for player install steps and
[`docs/EVENT_AUTHORING.md`](docs/EVENT_AUTHORING.md) to write your own events.

## Known issue: Windows Smart App Control

On some Windows 11 machines, PEMF's DLLs are blocked at startup with
`Bad Image ... 0xC0E90002`. This is Windows' **Smart App Control** refusing
unsigned code — not a bug in the mod. See [`docs/SIGNING.md`](docs/SIGNING.md)
for the full explanation and the fix (code signing).

## License

[MIT](LICENSE).
