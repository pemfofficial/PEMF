# PEMF — Pirates! Expanded Modding Framework

PEMF is a modding framework for **Sid Meier's Pirates! (2004)**. It adds a
data-driven **narrative event engine** — branching events with real choices and
consequences, authored as JSON — plus expanded crew and morale systems, on top
of the original game.

It loads through a `version.dll` proxy and reaches the game through **IAT
hooks**, which is how everything the framework does is driven. Some features —
weather is the current one — additionally rewrite a few immediates in the
game's code **in memory**, once the game is running. **No game file is ever
modified on disk.** Events are presented through the game's **own** text and
dialog routines, so they look native.

> **Not affiliated with Firaxis Games, 2K, or Atari.** PEMF ships **no game
> files** and modifies none on disk. You must own a legitimate copy of the game.

## ⬇️ Download

### **[Get the latest release →](https://github.com/pemfofficial/PEMF/releases/latest)**

Grab **`PEMF-0.2.3.zip`** from that page — it's the file under **Assets**.
Unzip it into your game folder, the one with `Pirates!.exe` in it, and launch the
game. That's the whole install; nothing else is needed and the game executable is
never modified.

**Don't use the green "Code" button** at the top of this page. That downloads the
*source code*, which has no `version.dll` or `pemf_core.dll` in it — those are
built, not stored here. If you ended up with a folder of `.cpp` files and no DLLs,
that's what happened. Use the releases link above.

**GOG and Steam both work.** To uninstall, delete `version.dll`, `pemf_core.dll`
and the `PEMF` folder.

### Check what you downloaded

```powershell
Get-FileHash .\PEMF-0.2.3.zip -Algorithm SHA256
```

| File | SHA256 |
|---|---|
| `PEMF-0.2.3.zip` | `1D7B3880B9489F4FB545F7A9384B6CD56AA6DBE5BBCD53BCC5344B5DA8546EC4` |
| `pemf_core.dll` | `E1A75858F7A37CF728387FC3620D9211E88ECB08697766E788DC2CA7EBFC7E71` |
| `version.dll` | `FE5687E05AB4B90EFE6FC3FCDAC76E1CEC8EF7F3B98D00687DE69286E1291389` |

PEMF is unsigned, so this hash is your integrity check. If it doesn't match,
you didn't get the file from us.

## ⚠️ Windows may block it

PEMF is **unsigned** — we applied to the SignPath Foundation's free
open-source signing programme and were rejected, and paid certificates are out
of scope for this project. Windows reacts to that in two ways:

**Defender may refuse the download** as `Trojan:Win32/Wacatac.B!ml`. This is a
**false positive**. The `!ml` suffix marks it as a machine-learning guess made in
Microsoft's cloud rather than a match against known malware, and the published
bytes scan clean against Defender's full signature set. A brand-new unsigned file
that hooks into a game process is simply the shape a classifier distrusts —
that's the same technique every PC mod loader has used for twenty years. Check
the hash, then allow it in *Windows Security → Protection history*.

**Smart App Control may block the game from starting**, with `Bad Image ...
0xC0E90002`. Most people never see this — it only turns itself on for clean
Windows 11 installs. The only fix is turning Smart App Control off, and that
can't be undone without reinstalling Windows, so it's a fair reason to walk away.

Full detail, including what we've already ruled out:
**[`docs/WINDOWS_SECURITY.md`](docs/WINDOWS_SECURITY.md)**.

PEMF has no network code, no persistence, no autostart, and touches nothing
outside your game folder. It's MIT-licensed and every line is in this repo — read
it, or build it yourself with `.\build.ps1 -Package` and run the result instead
of ours. (A rebuild won't match our hash byte-for-byte: MSVC stamps the build
time into the DLL. Same code, different bytes.)

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

`-Package` writes `dist\PEMF-<version>.zip` — the same archive published on the
releases page. `dist\` is not tracked in git, which is why the source tree
contains no DLLs.

See [`INSTALL.txt`](INSTALL.txt) for player install steps and
[`docs/EVENT_AUTHORING.md`](docs/EVENT_AUTHORING.md) to write your own events.

## License

[MIT](LICENSE).
