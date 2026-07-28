# PEMF — Pirates! Expanded Modding Framework

A modding framework for **Sid Meier's Pirates! (2004)**, built as a DLL that runs
inside the game. Narrative events authored in JSON, with real choices and real
consequences — and, in time, named officers and a crew with opinions.

The game executable is never modified.

---

## Start here

| Document | For | Contents |
|---|---|---|
| **[PLAYER_MANUAL.md](PLAYER_MANUAL.md)** | Players | Install, uninstall, what works today, what is planned, troubleshooting |
| **[EVENT_AUTHORING.md](EVENT_AUTHORING.md)** | Players & modders | Writing events for `PEMF\events\` — schema, tokens, effects, error messages |
| **[DEVELOPER.md](DEVELOPER.md)** | Developers | Architecture, build, RE workflow, conventions, roadmap, lessons |
| **[GAME_API.md](GAME_API.md)** | Developers | Reverse-engineered engine reference: functions, addresses, conventions |
| **[ARCHITECTURE_REVIEW.md](ARCHITECTURE_REVIEW.md)** | Developers | Eight passes: hazards found, fixed, and the invariants they establish |
| **[`re/experiments/`](../re/experiments/)** | Developers | Write-ups of specific investigations — the question, the method, what the numbers said, and what was concluded (including the answers that were "no") |

---

## What a player installs

```
<game folder>/
├── Pirates!.exe           stock, untouched
├── version.dll            PEMF loader
├── pemf_core.dll          PEMF
└── PEMF/
    ├── events/
    │   └── core_events.json
    └── docs/
```

**Every `.json` in `PEMF\events\` is loaded.** Add-ons ship their own file and sit
side by side; a broken one is skipped with a reason rather than taking the others
down.

Build a release archive with `.\build.ps1 -Package`.

---

## Repository layout

```
PiratesMod/
├── build.ps1              build, install, package
├── INSTALL.txt            ships in the release archive
├── content/PEMF/events/   authored narrative content
├── docs/                  you are here
├── dist/                  release archives
├── src/
│   ├── proxy/             version.dll loader (+ gen_proxy.py)
│   ├── core/              pemf_core.dll
│   │   ├── game.h         raw addresses + calling shims
│   │   ├── state.h        validated state mutation
│   │   ├── session.h      career lifecycle, save/load, persistence
│   │   ├── events.h       deferred dispatch (the safe point)
│   │   ├── content.h      JSON event loader + validation
│   │   ├── triggers.h     when events fire (world sampling)
│   │   ├── render.h       the render phase (where things are shown)
│   │   └── core.cpp       hooks and wiring
│   └── vendor/            nlohmann/json
└── re/
    ├── bin/               staged copies of the target binaries
    ├── scripts/           Python RE tooling + Ghidra Java scripts
    ├── ghidra_proj/       Ghidra project (analysed)
    └── out/               decompilation output, offsets.json
```

---

## Status

Status is tracked honestly, because this project is easy to overstate. Three
levels are used throughout the docs:

- **Verified** — observed working in a running game, with log evidence.
- **Built** — implemented and compiling, but not yet exercised in a live game.
- **Not started** — design intent only.

| Capability | State |
|---|---|
| DLL injection, code on the game thread | **Verified** |
| Reading live game state | **Verified** |
| Custom narrative text with token substitution | **Verified** |
| Word wrapping | **Verified** |
| Native-style card rendering | **Verified** |
| Modal N-way choice (up to 3 options observed) | **Verified** |
| State changes as event consequences | **Verified** |
| Deferred dispatch from a safe frame point | **Verified** |
| Thread affinity (game thread only) | **Verified** |
| Validated / logged state mutation | **Verified** |
| Save & load detection, per-save sidecar | **Verified** |
| JSON event loading, folder scan, validation | **Verified** |
| Firing a JSON event — dispatch and arg binding | **Verified** |
| Event queue cleared on load | **Built** |
| `CreateFileW` save path | **Built** — hook installs; no save has been observed using it |
| Trigger layer — loads, arms, evaluates at the safe point | **Verified** |
| `elapsedSailing` trigger firing during play | **Verified** — fired at 120 s and 300 s |
| `nearPort` trigger firing during play | **Built** — retuned twice; thresholds vary a lot by route |
| "Am I sailing" detection | **Verified** — heuristic, but correct across a full session |
| Firing a JSON event — card observed on screen | **Verified** |
| `nearPort` trigger firing during play | **Verified** — fired at distance 3000, and re-armed correctly |
| `notice` event kind — schema, validation, triggering | **Verified** |
| Render-phase hook — the game's own D3D9 device | **Verified** — live on GOG and Steam |
| `notice` event kind — visible on screen | **Verified** — drawn from inside the frame |
| Notices anchored to the player's ship | **Verified** — hangs over the vessel and follows it as you sail |
| `stateCrosses` trigger — crew / gold / morale / months | **Built** |
| City tokens — `@CITYNAME` / `@NATIONALITY` / `@LOCTYPE` | **Verified** — "Land ho! Nevis off the bow!" |
| `{placeholder}` authoring layer | **Verified** — every shipped event uses it |
| Notices confined to the sailing view | **Verified** |
| Reading nation relations — war, treaty, neutral | **Verified** — seen changing live as wars ended |
| Reading the nation the player serves | **Verified** — four careers, four crowns, four values |
| Reading the player's rank and reputation per crown | **Built** — addresses confirmed in the disassembly; every career tested so far holds no commission, so the values have not yet moved |
| Playing a named game sound with an event | **Verified** |
| Officer simulation | Not started |
| Factions and divisions | Not started |

### The render hook — solved

The framework runs inside the frame, at `IDirect3DDevice9::EndScene` on the
game's **own** Direct3D device. This is the way onto the screen for everything
we want to draw ourselves: sailing notices, callouts, indicators, panels.

**How the device is found.** The renderer singleton at `0x00727C30` holds its
`IDirect3DDevice9*` at `+0x60`. That came out of the device-creation sequence
(the `CreateDevice` call writes the new device into `renderer+0x60`) and is
confirmed by 19 independent sites that read `[[0x00727C30]+0x60]` and call COM
methods on it. The framework polls that pointer from the safe point and patches
the real vtable the moment the device exists — so nothing in the game's code is
rewritten, and the hooks are on the vtable actually in use.

**The vtable gets rebuilt mid-session.** Observed repeatedly in-game, so the
installer re-verifies its slots every safe point and re-installs when they have
been shed. Without that the hook dies silently; with it, recovery is automatic
and logged. A 15-second heartbeat means a dead hook can never look like a quiet
one.

Two earlier approaches failed, and both are worth remembering:

| Approach | Result |
|---|---|
| Redirect the sailing-render call site | Black screen **even with a do-nothing callback** — the redirection itself was the fault |
| Throwaway device to read "the" vtable | Cannot work: the vtable is **per-instance heap memory**, freed with the throwaway device (`MEM_RESERVE` right after `Release`) |

Currently at **stage 2** — our own text is drawn from inside the frame, on both
builds. Two phases are hooked, because the game draws the two kinds of text
differently and they are not interchangeable: **`EndScene`** for screen-space
HUD lines (an immediate 2D blit, on top of the finished scene) and
**`BeginScene`** for world-anchored text (which builds scene-graph nodes the
render walk then draws, so it must exist before the world does). How that works,
and the exact call the ship labels use, is in
[`GAME_API.md`](GAME_API.md#drawing-our-own-text--solved-and-how).

**A displayed frame contains more than one render pass**, and they do not share
a camera. World text drawn in every pass appears several times over in different
places. So it is drawn in the **first** pass only — the world pass, the one that
walks the scene graph the label was just attached to.

The boundary that separates one frame's passes from the next comes from the
**safe point**, not from `Present`: this game never calls `Present` on the
device, so a counter reset only there never resets, and anchored text stops
drawing after the first frame. The top of the game's own main loop runs once
per iteration and is the reliable boundary.

Stage 3 — presenting event cards from inside the frame, which fixes the
half-drawn background behind dialogs — is the next step.

The single most important verified result: **a custom event, written by us,
rendered natively in-game with live state substituted into it** —

> *"A hush falls over the deck. Your crew of 40 stands DEVOTED, and word spreads
> that something has changed aboard this ship."*

That proved the whole approach.

---

## A note on scope

An earlier plan found in the game folder (`our-mods/runtime/PLAN.md`, inherited
from previous work) concluded that new systems were unrealistic and that this
should remain a tuning mod.

That is **demonstrably wrong** and should not be treated as guidance. We can run
code on the game thread, read and write live state, call engine functions with
recovered calling conventions, and render native UI with arbitrary content. Those
are the primitives for a total conversion.

The design principle that follows: **new systems live in our memory, not the
exe's data structures.** Do not fight for a 7th trade good slot in a fixed-size
table. Officers, factions and narrative state are our objects, unbounded by 2004
table sizes; the exe stays the world, physics and rendering substrate, and we
write back only the handful of values it natively understands.
