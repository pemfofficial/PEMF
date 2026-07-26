# Developer Guide

How the mod is put together, how to build it, and how to keep extending it.

For the reverse-engineered engine functions themselves, see
**[GAME_API.md](GAME_API.md)**.

---

## Architecture

```
Pirates!.exe                     stock GOG binary, never modified on disk
│
└── version.dll                  proxy loader (ours)
    │  forwards all 17 exports to C:\Windows\System32\version.dll
    │
    └── pemf_core.dll            PEMF
        ├── game.h               raw addresses + calling shims
        ├── state.h              VALIDATED access to live game state
        ├── session.h            career lifecycle, save/load, our persistence
        ├── events.h             deferred dispatch (the queue)
        ├── content.h            JSON event loading + validation
        ├── render.h             the render phase (where things are shown)
        └── core.cpp             hooks and wiring
```

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
| `0x004726CA` → `FUN_004612B0` | **the render phase** — where anything visible happens |

### Decide at the top, show after the render

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

So presenting happens after `FUN_004612B0` (the sailing render) returns.
That function has exactly one caller, so PEMF rewrites **that call's rel32** to
its own stub rather than detouring the prologue — one 4-byte write, no
trampoline, nothing relocated, and reversible. `render::Install()` verifies the
call previously targeted `0x004612B0` and reverts if it did not.

### Layer rules

- **Never** call `game::` mutators directly from content — go through `state.h`.
- **Never** present UI from a trigger — `Post()`, and let the render phase run it.
- **Never** present two dialogs in one frame — use `events::PostFollowUp()`, which
  runs on the next frame ahead of the queue.
- **Never** draw HUD text outside the render phase — it has no timer of its own
  and must be re-issued every frame.
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

**Scaffolding only** — behind `kDebugHotkeys` in `core.cpp`, to be removed once
real triggers exist. F-keys collide with the game's own menus, hence Ctrl+Shift.

| Key | Action |
|---|---|
| Ctrl+Shift+1 | Consequence event (yes/no + state change + outcome) |
| Ctrl+Shift+2 | Choice event, message form |
| Ctrl+Shift+3 | Plain narrative card |

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
| 1b — trigger layer (`elapsedSailing`, `nearPort`) | `elapsedSailing` **verified**; `nearPort` built |
| 1c — render phase + `notice` events | **Built**, hook verified installing |
| 1d — officers | **Next** |
| 2 — crew simulation, mutiny outcomes | Planned |
| 3 — factions and divisions | Later |
| 4 — custom UI, textures, meshes | Much later |

### Next: officers

The framework pieces officers need are now in place — persistence keyed to the
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
