# Developer Guide

How the mod is put together, how to build it, and how to keep extending it.

For the reverse-engineered engine functions themselves, see
**[GAME_API.md](GAME_API.md)**.

---

## Architecture

```
Pirates!.exe                     stock binary, never modified on disk.
│                                GOG or Steam -- one codebase, one offset map.
│
└── version.dll                  proxy loader (ours)
    │  forwards all 17 exports to C:\Windows\System32\version.dll
    │
    └── pemf_core.dll            PEMF
        ├── game.h               raw addresses + calling shims
        ├── state.h              VALIDATED access to live game state
        ├── nations.h            relations, standing, the crown you serve
        ├── standing.h           the reputation ledger (an output, not storage)
        ├── session.h            career lifecycle, save/load, our persistence
        ├── events.h             deferred dispatch (the queue)
        ├── content.h            JSON event loading + validation
        ├── triggers.h           when events fire (world sampling)
        ├── suspicion.h          false colours, hunters, the panel
        ├── townmenu.h           our rows in the game's menu, and our own menus
        ├── plugin.h             loading other people's DLLs (the C ABI host)
        ├── storms.h             weather: operand patches, drift, cargo loss
        ├── stormaudio.h         the storm bed and its fades
        ├── audiomix.h           our own mixer (Media Foundation + XAudio2)
        ├── render.h             the render phase (where things are shown)
        ├── d3d9hook.h           the device vtable hook
        ├── log.h                the log
        └── core.cpp             hooks and wiring
```

`plugin.h` is the only place PEMF runs code it did not write. Everything a
plugin can reach goes through the layers above it -- `state.h` for anything that
changes the game, `content.h` for events -- so a plugin gets the same clamping
and logging PEMF holds itself to, and a plugin that faults is disabled for the
session rather than taking the game with it. The public half is `sdk/pemf_sdk.h`.

`storms.h` is the one place PEMF writes to the game's **code** rather than its
data, and it does so only from the first safe point — writing earlier races the
Steam build's DRM checksum and the game refuses to start. See the lessons below.

`nations.h` **only reads**. The relations matrix is the engine's to maintain, and
a framework that edited it would be rewriting the world rather than reacting to
it. The one number PEMF does write is the player's own reputation, from
`suspicion.h`, because that is player state rather than world state.

`suspicion.h` owns a whole mechanic and touches the engine in exactly two places:
it writes reputation when you are unmasked, and it calls the ship factory to
dispatch a hunter. Everything else it does is its own.

Content lives outside the DLL entirely, in `PEMF\events\*.json` beside the game.

### Four IAT hooks

| Hook | Role |
|---|---|
| `WINMM!timeGetTime` | cheap per-frame tick; triggers **post** to the queue |
| `USER32!PeekMessageA` | **the decide point** — triggers and session state |
| `KERNEL32!CreateFileA` | save/load detection for our sidecar state |
| `KERNEL32!CreateFileW` | the same, for the wide path — the exe imports both |

### One call-site redirection

| Site | Role |
|---|---|
| `IDirect3DDevice9::BeginScene` / `EndScene` | **the render phase** — where anything visible happens |

### Decide at the top, show inside the frame

Two different points in the frame, for two different jobs. Conflating them was a
real bug.

**The decide point** — the main loop is `FUN_0042E1D0`, and its `PeekMessageA`
call sits at `0x0042E206`, so the **return address `0x0042E20C` uniquely
identifies the top of the frame**: message queue drained, nothing rendering, no
game locks held. Triggers and session lifecycle run here. A modal's own nested
pump calls `PeekMessageA` from a *different* site, so it can never be mistaken
for it.

**The render phase** — the top of the frame is *before anything is drawn*, which
makes it the wrong place to show things:

- a dialog composites over the back buffer, so presented there it sits on a
  stale, half-drawn frame
- HUD text drawn there is painted over by the world

So showing happens **inside the frame**, on the game's own Direct3D device.

> **Historical note, kept because it cost a lot to learn.** This used to work by
> rewriting the rel32 of the single call to `FUN_004612B0` (the sailing render).
> That **black-screened the game even with a callback that did nothing but
> increment a counter** — the redirection itself was the fault. `render.h`
> survives as the record of that dead end; the live path is `d3d9hook.h`.

The device is found at `[[0x00727C30]+0x60]` and its vtable patched, so no game
code is rewritten. **Two phases, not interchangeable, and both fail silently if
confused:**

- **`BeginScene`** — the frame is empty. **World-anchored text** goes here,
  because the engine's world-text call builds scene-graph nodes that the render
  walk then draws. Issued later, they are built after the walk that would have
  drawn them.
- **`EndScene`** — the scene is complete. **Screen-space HUD text** goes here,
  because it is an immediate 2D blit that has to land on top.

A displayed frame contains **several** `BeginScene`/`EndScene` pairs with
different cameras, so world text is drawn in the **first** pass only. The frame
boundary comes from the safe point, **not** `Present` — this game never calls
`Present` on the device. Full account in
[`GAME_API.md`](GAME_API.md#drawing-our-own-text--solved-and-how).

### Layer rules

- **Never** call `game::` mutators directly from content — go through `state.h`.
- **Never** present UI from a trigger — `Post()`, and let the render phase run it.
- **Never** present two dialogs in one frame — use `events::PostFollowUp()`, which
  runs on the next frame ahead of the queue.
- **Never** draw HUD text outside the render phase — it has no timer of its own
  and must be re-issued every frame.
- **Never** draw world-anchored text at `EndScene`, or screen text at
  `BeginScene`. Both are silent failures, not errors.
- **Never** apply a per-frame rate in integer arithmetic without carrying the
  remainder. `rate * 16 / 1000` is zero; suspicion could not rise at all while
  reporting a correct rate.
- **Never** assume a draw left the shared buffer clean. The engine's HUD call
  uses `0x00869B48` as scratch, so *any* draw dirties it — clear after drawing in
  BOTH phases, or the sailing render paints it across the sea.
- **Never** trust a value published at the safe point without checking its age.
  A modal dialog stops the main loop without changing the screen signature.
- **Never** verify a packed build from its file. The Steam exe's `.text` unpacks
  1,176,576 -> 5,177,344 bytes; only runtime signature checks mean anything.
- **Never** hand the engine a token argument count, or an `@ITEM` index, that has
  not been read out of the disassembly. It does not bounds check; `@ITEM` past
  the end of its list access-violates.
- **Never** draw across a **device reset**. Alt-tab loses the D3D9 device, and
  everything PEMF draws goes through the game's own routines, which walk
  refcounted Gamebryo objects that are released and rebuilt around a reset.
  Calling a virtual on a destructed one is an `R6025 pure virtual function
  call`, which took the game down twice on returning from alt-tab, with
  `d3d9: device Reset #1` as the last line in the log both times. Two things
  were missing: nothing knew a reset was in flight, and the pass counter is
  incremented **before** the real `BeginScene`, so a `BeginScene` that FAILED on
  a lost device still left the counter reading 1 and the `EndScene` hook drew
  into it. A device is well only between a `BeginScene` that **succeeded** and
  its `EndScene`, and never while `Reset` is in flight.
- **Never** keep a pointer to an engine object past the call that produced it
  without **taking a reference**. Gamebryo objects are refcounted — the count is
  the dword at `+4`, and dropping to zero calls `vtable[0](1)`, the destructor.
  Capturing flag textures as the player browsed the picker crashed the game
  instantly, because the picker releases each one as it scrolls away. Take the
  reference (`obj[1] += 1`), and check the object still reads back sanely before
  handing it to the engine.
- **Never** infer "the overworld is on screen" from ship movement. Menus freeze
  the ship, so motion cannot tell a menu from a becalmed ship at sea — that
  mistake put a lookout's call across the Load/Save screen. Drawing is gated on
  `triggers::WorldOnScreen()`, which matches a screen signature it *learned*
  from ticks where the ship demonstrably moved. Motion learns the answer; it is
  never the answer.
- **Never** use `state::InGame()` to mean "a career is running". It is
  `crew > 0`, and the crew count does not reset at the main menu — so it is true
  forever after the first career of a session. Use `session::InCareer()`, which
  reads the screen state.
- **Never** treat a save file being read as a load. Starting a **new career**
  reads one too, and the load screen reads every save just to list them. Staged
  state is committed only when its fingerprint matches the live career.
- **Never** hardcode a measured screen signature. They are recorded in
  [`GAME_API.md`](GAME_API.md#drawing-belongs-to-the-sailing-view) as evidence
  only. Comparing against them would mean one unvisited HUD state stops all
  drawing, silently.
- `session::Ready()` gates content on an active, loaded career.

### Why a `version.dll` proxy

`Pirates!.exe` statically imports from `VERSION.dll`, so the OS loads our DLL
before the game's own initialisation — earlier than a `d3d9.dll` proxy would.

**A proxy DLL owns that module name for the entire process.** It must export
the real DLL's *complete* surface, not just what the exe imports. The exe uses
only 3 version.dll functions, but `d3d9.dll` imports `GetFileVersionInfoW` — and
a missing export is a hard load failure with an "Entry Point Not Found" dialog.

`src/proxy/gen_proxy.py` reads the real system DLL's export table and generates
all 17 stubs, so the list tracks whatever Windows ships. Each stub is a naked
tail-call:

```asm
mov eax, [slot]      ; per-export forward pointer
test eax, eax
jne ready
call ProxyResolve    ; lazy — never LoadLibrary under the loader lock
mov eax, [slot]
ready:
jmp eax              ; stack frame untouched → any calling convention
```

Resolution is lazy so we never call `LoadLibrary` while holding the loader lock.
The real DLL is opened by **absolute path**, otherwise the search order finds our
own file and recurses.

### Why hook `timeGetTime`

We need code running on the **game's own thread** — game state is not
thread-safe, and calling engine functions from a worker thread is unsafe.

`WINMM!timeGetTime` is imported by the exe and called every frame (84 call
sites). An **IAT hook** on it is the cheapest possible way in:

- no inline patching, no trampoline, no length disassembler
- nothing to get wrong, trivially reversible
- guaranteed to run on whichever thread drives the game loop

The tick is our scheduler. Everything the mod does happens from there.

---

## Building

### Requirements

| Tool | Version used | Notes |
|---|---|---|
| MSVC x86 toolset | 14.51 (VS 2026) | **32-bit required** |
| Python | 3.14 | RE scripts, proxy generation |
| Ghidra | 12.1.2 | headless decompilation |
| Temurin JDK | 21 | required by Ghidra |

Local MinGW is `x86_64` only and cannot build these — use MSVC.

### Commands

```powershell
.\build.ps1                    # build only
.\build.ps1 -Deploy            # build, then install into the game folder
.\build.ps1 -Package           # build, then produce dist\PEMF-<version>.zip
.\build.ps1 -Deploy -Package   # both
```

The script locates the toolset via `vswhere`, compiles both DLLs with
`/MT` (static CRT — do not depend on a runtime the game may not ship) and
`/GS-` (keeps naked thunks free of stack-check prologues), then **verifies each
output is actually a 32-bit image** before deploying.

### Regenerating the proxy exports

Only needed if a Windows update changes `version.dll`:

```powershell
cd src\proxy; python gen_proxy.py
```

---

## Writing calling shims

Most engine functions use non-standard conventions. Rules that have bitten us:

**Naked functions must contain nothing but `__asm`.** No `(void)param;` casts —
those emit code and corrupt the frame.

**MSVC inline asm cannot resolve namespaced `constexpr` names.** Addresses used
inside `__asm` are also `#define`d as `PGA_*` macros. Keep them in sync with
`game::addr`.

**Confirm the convention from the callee's `ret`, not the caller.** A caller with
no visible `add esp, N` does *not* prove stdcall — MSVC defers cleanup. Look for
`ret` vs `ret N` in the target function.

**Varargs shims pass a fixed 9 slots** (format + `kMaxTextArgs` = 8). Surplus
arguments are harmless: the callee reads only what its format tokens require, and
cdecl means we clean up. This avoids a copy loop. Eight rather than four because
once events are authored in JSON the token count is content-controlled.

**Guard every new engine call with SEH** while you are still unsure of it:

```cpp
__try   { game::SomeCall(); }
__except (EXCEPTION_EXECUTE_HANDLER) {
    Log("EXCEPTION 0x%08X", GetExceptionCode());
}
```

A logged exception code is far more useful than a crashed game, and it keeps the
iteration loop fast.

---

## Reverse engineering workflow

### Ghidra headless

Full analysis of the exe takes ~400 s and is already done; the project lives in
`re/ghidra_proj`. To decompile specific addresses:

```powershell
$env:JAVA_HOME = "C:\Program Files\Eclipse Adoptium\jdk-21.0.11.10-hotspot"
& "C:\Tools\ghidra_12.1.2_PUBLIC\support\analyzeHeadless.bat" `
    re\ghidra_proj Pirates -process "Pirates_gog.exe" -noanalysis `
    -scriptPath re\scripts -postScript DecompileTargets.java `
    re\out\myfile.c 00410c50 00430190
```

Scripts must be **Java**: local Python is 3.14 but the bundled jpype wheels stop
at cp313, so PyGhidra will not install.

A headless exit code of `-1` is a detached-process artifact, **not** a failure —
read the log and look for `Analysis succeeded`.

### Python tooling (`re/scripts/`)

Dependency-free except `capstone`. Often faster than opening Ghidra.

| Script | Purpose |
|---|---|
| `xref_scan.py` | PE parser (PE32 + PE32+) and string/xref scanning. Imported by the rest. |
| `disasm.py` | Disassemble any VA: `python disasm.py 410C50` |
| `callers_of.py` | Every call site of a target, with preceding register setup — **the tool for decoding conventions** |
| `imports.py` / `exports.py` | Import and export tables |
| `iat_refs.py` | Count call sites of an imported function (finding hook points) |
| `find_choice_api.py` | Cluster strings by which function presents them |
| `diff_exe.py` | Compare builds; check whether our offsets are valid |

### The method that keeps working

1. Find a **string** the feature displays.
2. Find code that pushes its address (`xref_scan.py`).
3. Look at what is **called next** — that is usually the presenter.
4. Cluster across many strings; the common target is the shared function.
5. Use `callers_of.py` to see how registers are set up at *every* call site.
   Differences between sites reveal what the parameters mean.

Step 5 is what identified the dialog forms: message sites pass `eax = -1`, menu
sites pass `eax = 10`.

---

## Debugging in-game

The mod logs to `<game>\pemf.log`, rewritten each launch, flushed after
every line.

A healthy startup:

```
=== PEMF loaded === pid=13012
target verified: offsets match the expected build
IAT hook installed on WINMM!timeGetTime
first game-thread tick (thread 9820) -- IAT hook is live
heartbeat: ticks=1993 crew=40 morale=4 plunder=600 -- IN GAME
```

- **No log file** → the core never loaded; check `version.dll` is present.
- **No `first game-thread tick`** → the game has not reached its main loop.
- **`TARGET MISMATCH`** → wrong exe; see [GAME_API.md](GAME_API.md).
- `crew=0` → menu or intro, not in a game yet.

### Debug hotkeys

Behind `kDebugHotkeys` in `core.cpp`. Triggers are live, so these are for
testing content without waiting for the conditions to come round, plus a few
engine probes. F-keys collide with the game's own menus, hence Ctrl+Shift.

| Key | Action |
|---|---|
| Ctrl+Shift+1 | Fire the first authored event |
| Ctrl+Shift+2 | Fire the second |
| Ctrl+Shift+3 | Test notice at the top of the screen |
| Ctrl+Shift+4 | Test notice anchored over the ship |
| Ctrl+Shift+5 | Log the engine's item names, indices 0-6 |
| Ctrl+Shift+6 | Log item index 7 — **past the stock list; the lookup is not bounds checked** |
| Ctrl+Shift+7 | Toggle the data-file probe (see below) |

**The data-file probe** logs every `.ini` / `.txt` / `.csv` / `.fpk` the game
opens *and every one it fails to open* — the misses are what reveal whether a
loose file would be picked up. The hotkey is nearly useless on its own, because
assets load during startup: drop a `PEMF\fileprobe.on` next to the exe and it is
armed from the first open instead.

The event engine must never know these exist. It takes a trigger and fires;
whether that trigger was a keypress or a port entry is the caller's business.

---

## Roadmap

| Milestone | Status |
|---|---|
| 0 — injection, game thread, native card | **Verified in-game** |
| 0.5 — modal choice + state consequences | **Verified in-game** |
| 0.75 — hardening: safe point, threads, save/load, validation | **Verified in-game** |
| 1a — JSON event engine | **Verified in-game** |
| 1b — trigger layer (`elapsedSailing`, `nearPort`, `stateCrosses`) | **Verified in-game**; `stateCrosses` built |
| 1c — render phase + `notice` events | **Verified in-game**, on GOG and Steam |
| 1c+ — notices anchored to the ship, `{placeholder}` authoring | **Verified in-game** |
| 1c++ — notices confined to the sailing view, clock held behind menus | **Verified in-game** |
| 1d — event cards from inside the frame (stage 3) | **Next** — see below |
| 1e — officers | After 1d |
| 2 — crew simulation, mutiny outcomes | Planned |
| 3 — factions and divisions | Later |
| 4 — custom UI, textures, meshes | Much later |

### Next: stage 3 — cards from inside the frame

Event cards are presented from the safe point, before the world has been drawn
that frame. Presenting from inside the frame instead is the more native path.

⚠️ **This used to be listed as the fix for the half-drawn card background. That
bug is already fixed and is no longer a reason to do stage 3.** The cause was
elsewhere: the draw paths had no `events::Busy()` guard, and the overworld gate
goes stale on a 250ms timer refreshed from the safe point — which a card's
nested loop displaces — so for the first quarter-second of every card the gate
still answered "yes". That window was the leak. See `PemfOnEndScene` in
`core.cpp`.

The scaffolding for stage 3 exists
(`PEMF_D3D9_STAGE 3`). **It is not a matter of raising the constant:** the
game's dialog is modal and blocking, so its own message loop would re-enter
`BeginScene`/`EndScene` from inside our hook. That needs a re-entrancy guard
first.

### After that: officers

The framework pieces officers need are in place — persistence keyed to the
save (`session.h`), validated state access, a render phase for any UI, and two
event kinds to talk to the player through.

`session::ModState` is where officer data belongs. It is three ints today; when
it grows, move the sidecar from `key=value` to JSON rather than inventing a
second persistence path.

### Also outstanding

More triggers: entering port, battle outcomes, month rollover, morale crossing a
threshold. `0x00437A5B` and `0x00437CA6` (the `Morale +@NUM` sites) are the
obvious next hooks.

---

## Hard-won lessons

Each of these cost real debugging time. Do not relearn them.

- **A proxy DLL must export the real DLL's entire surface**, not just what the
  exe imports. Other DLLs in the process resolve against it too.
- **`esi` is held across the whole message sequence**, not passed per call.
- **`WrapText` is cdecl.** A caller with no `add esp, N` does not imply stdcall.
- **Modal engine calls re-enter the frame hook.** Guard reentrancy or die.
- **`0x008CACD0` is an empty-string sentinel**, not an output buffer.
- **`eax` on `ShowMessage` selects the dialog form**, and `10` is non-blocking.
- **Verify the binary before writing memory.** `VerifyTarget()` byte-probes on
  startup; the Challenge Pack exe is a completely different build.
- **The engine formatter never calls printf.** Verified statically: zero
  `vsprintf`/`sprintf`/`_snprintf` call sites in `0x4F5000-0x4F8000`. A `%` in
  authored text is literal, so there is no format-string hazard. The real risk is
  `@`-token over-consumption, which the content loader rejects.
- **`AddText` replaces the buffer, it does not append.** The whole prompt must be
  one string in one call.
- **Do not round-trip markdown through PowerShell 5.1.** `Get-Content -Raw` plus
  `Set-Content` mangles UTF-8 (em dashes become `â€”`). Edit files directly.
- **Deciding and showing are different points in the frame.** The top of the
  frame is before anything is drawn; presenting there gives a stale, half-drawn
  background. Show things from the render phase.
- **Prefer redirecting a call site to detouring a function.** If a target has a
  single caller, rewriting that `call rel32` needs no trampoline, relocates no
  instructions, and is a single reversible write. Verify the previous target
  before keeping the redirect.
- **Don't tune a threshold to the smallest value you have observed.** Closest
  approach to a port measured 988 in one session and 1620 in another; a
  threshold of 1200 fired in one and never in the other.

---

## Releasing

```powershell
.\build.ps1 -Package -Version 0.2.6
```

Produces `dist\PEMF-<version>.zip` with no wrapping folder, so a player extracts
it straight into the game directory. Contents: both DLLs, `INSTALL.txt`, the
events, `suspicion.ini`, `storms.ini`, `KeyMap_WASD.ini`, `PEMF\audio\` and the
player-facing docs.

### Release checklist

`-Package` also prints the SHA256 of the zip and both DLLs, and writes the same
table to `dist\PEMF-<version>.sha256.md`. **Paste it into `README.md` — and
nowhere else.**

⛔ **THE HASH TABLE LIVES IN `README.md` ONLY, BECAUSE THE README IS NOT SHIPPED
IN THE ZIP.** `docs/WINDOWS_SECURITY.md` used to carry it too, and that could
never be right: it ships *inside* the archive it was vouching for, so writing the
hash into it changes the archive and invalidates the hash it just wrote. Editing
the README is free; editing anything under `docs/` that gets packaged means
rebuilding, which re-stamps the DLL timestamps and rots every hash again.

⚠️ **DO NOT REBUILD AFTER TAKING THE HASHES.** The zip in `dist\` must be the
exact one you publish. `build.ps1` with no changes still produces different
bytes — MSVC writes the build time into the PE header.

The order that works:

1. finish every change, **including docs that go in the zip**
2. `.\build.ps1 -Package`
3. paste the hashes into `README.md`
4. commit, tag, `gh release create` with **that** zip

Related, same family of mistake: check the README names the **current** zip. It
advertised `0.2.0` for two releases running.

### Then actually publish it

⚠️ **A TAG IS NOT A RELEASE.** `git push --tags` puts the tag on GitHub and
changes nothing a player can see: the releases page still shows the previous
version as Latest, with no zip attached. This was missed on 0.2.4 and the
release sat invisible until someone went looking for it.

```powershell
gh release create v<version> dist\PEMF-<version>.zip `
    --title "PEMF <version> -- <name>" `
    --notes-file docs\RELEASE_NOTES_<version>.md
```

**Check the zip's hash against the one you just published** before uploading. A
release whose bytes disagree with the table in `README.md` tells every player
with a good download that they have a bad one:

```powershell
(Get-FileHash dist\PEMF-<version>.zip -Algorithm SHA256).Hash
```

Then confirm it took:

```powershell
gh release list                     # the new one should say Latest
gh release view v<version> --json assets
```

**Extract it into a clean folder and check it before linking anyone to it.** The
packager has silently skipped a missing `INSTALL.txt` for most of this project's
life; nobody noticed because nobody looked in the archive.

### What a player gets versus what we get

Marker files in `PEMF\`, none of which ship in the archive:

| File | Turns on |
|---|---|
| *(none)* | Flag keys `Ctrl+Shift+8/9/0`. Always live — this is the mechanic. |
| `dev.on` | Every probe: event firing, engine dumps, nations report, the reputation cheat, ship diffs. |
| `shipyard.on` | Keys that **build ships**. Creates state that persists into a save and cannot be undone by pressing the key again. |

Nothing is compiled out of a release build. A player who hits a bug can enable
the whole toolkit with an empty file, which is what makes their report useful.

### Signing

The archive is **unsigned and stays unsigned** — the SignPath Foundation
rejected our application and paid certificates are out of scope. Two
consequences, and both belong in every release's notes and FAQ, because neither
is diagnosable from the log:

- Smart App Control refuses to load the DLLs: `Bad Image ... 0xC0E90002`.
- Defender may block the **download** as `Trojan:Win32/Wacatac.B!ml`, a cloud
  ML false positive.

See [`WINDOWS_SECURITY.md`](WINDOWS_SECURITY.md). **Publish the SHA256 of every
artifact you ship** — with no signature, the hash is the only way a player can
tell our build from someone else's.
