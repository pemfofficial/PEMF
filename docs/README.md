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
| **[ARCHITECTURE_REVIEW.md](ARCHITECTURE_REVIEW.md)** | Developers | Four audit passes: hazards found, fixed, and the invariants they establish |

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
| Render-phase hook (call-site redirection) | **Does not work** — see below |
| `notice` event kind — visible on screen | **Blocked** on a render hook |
| Dialogs presented over a fully drawn frame | **Blocked** on a render hook |

### Known issue: no render hook yet

Redirecting the sailing-render call site black-screens the game on world entry
and then crashes it. A controlled experiment settled the cause:

| Build | Callback does | Result |
|---|---|---|
| no hook | — | reaches the world, sails, stable |
| hook installed | nothing but increment a counter | black screen, crash |

Since the callback does nothing, **the redirection itself is the problem** — not
the work it was going to do. Calling convention and namespaced inline-asm
symbols were both ruled out along the way.

The consequence is cosmetic: `notice` events fire correctly but are not drawn
(the log says so explicitly when it happens), and dialogs present from the top of
the frame, so the world behind them can look half-drawn.

A second approach — hooking `IDirect3DDevice9::EndScene` by creating a throwaway
device and patching its vtable — was also tried. It is **non-destructive** (the
game ran normally throughout) but cannot work: after the throwaway device is
released, its vtable address reports `MEM_RESERVE` and `VirtualProtect` fails
with `ERROR_INVALID_ADDRESS`. The vtable is **per-instance heap memory**, freed
with the device, not the shared table in `d3d9.dll` that the technique assumes.

**Next attempt: find the game's own `IDirect3DDevice9*` and patch the real
vtable.** The engine is Gamebryo/NiDX9, and `NiDX9Renderer` holds that pointer —
locating it is ordinary reverse engineering of the same kind that found the text
API.

Both attempts are kept in the tree at stage 0, failing safely, so the next one
starts from something rather than a blank page.
| Officer simulation | Not started |
| Factions and divisions | Not started |

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
