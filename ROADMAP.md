# PEMF Roadmap

This is a living document. PEMF is an ambitious, long-haul project, and its
scope is deliberately open: the JSON event engine and the officer/crew systems
are the first pillars, not the whole plan. Expect this list to grow and shift
as the framework matures and as we learn what the engine can actually be pushed
to do.

Status is honest here on purpose — including the things that don't work yet.

**Legend**

| Mark | Meaning |
|------|---------|
| ✅ | Done and verified in-game |
| 🟡 | Partially working / needs polish |
| 🚧 | In progress |
| ⛔ | Blocked — needs a prerequisite solved first |
| 📐 | Designed, not yet built |
| 💡 | Idea / under consideration, not committed |

---

## Foundations (the plumbing)

These are the load-bearing pieces everything else stands on. They work.

- ✅ **Injection chain** — a `version.dll` proxy loads the core. The proxy
  forwards every export of the real system DLL, so nothing else in the process
  breaks. No modified game files.
- ✅ **In-process hooks, no inline patching** — the core reaches the game thread
  by hooking imported functions in the IAT (`timeGetTime`, `PeekMessageA`,
  `CreateFileA/W`). Nothing in the game's code is rewritten.
- ✅ **The "safe point"** — a precise spot at the top of the game's main loop
  where the queue is drained, nothing is mid-render, and it's safe to act. All
  game logic runs from here.
- ✅ **Deferred dispatch** — triggers never present anything directly; they post,
  and events are shown one per frame from the safe point. This is what keeps the
  mod from destabilizing the game.
- ✅ **Validated state layer** — every change to game state goes through one
  place that checks, clamps, and logs it. Bad writes are refused, not crashed on.
- ✅ **Save/load awareness** — state is stored in a sidecar file next to each
  save, keyed to that save, so it travels correctly and save-scumming behaves.

## Narrative engine

- ✅ **Native-looking event cards** — events are drawn through the game's *own*
  text and dialog routines, so they match the game's style instead of being an
  obvious overlay.
- ✅ **Real choices and branching** — an event can offer options and branch on
  what the player picks, with outcomes that read as the next page of the card.
- ✅ **JSON-authored events** — events are data, not code. Drop a `.json` file in
  and it loads; a broken file is skipped without taking the rest down.
- ✅ **Load-time validation** — malformed events are rejected with a precise
  reason in the log, so authors get real feedback.
- ✅ **Triggers** — events can fire after a stretch of sailing or on approaching
  a port, with repeat-guards so they don't spam.
- 🟡 **Two presentation styles** — the interrupting modal card works; the
  non-interrupting on-screen "notice" (a lookout's call while sailing) is
  designed and wired but **can't be drawn yet** (see the render blocker below).

## Audio

Custom sound is a first-class pillar, not a nice-to-have. Existing mods for this
game can only *replace* the built-in sounds by swapping files; because PEMF runs
live code inside the game, it can **add** new sounds and play them on cue — tied
to events, triggers, and game state. As far as we can tell, nobody has done
event-driven audio here before.

- ✅ **Sound engine mapped** — the game uses Miles Sound System; its whole audio
  import surface and the in-game load/play functions are located.
- 🟡 **Play-by-name details** — the exact way to trigger a named sound is mapped
  structurally but needs final confirmation (see the tooling note under Blockers).
- 📐 **Custom clips added, not just swapped** — drop a new `.wav` in and play it;
  the game already loads loose `.wav` files by name, which is the groundwork.
- 📐 **Sound on events** — an event or notice names a clip to play when it fires
  (a voice line, a callout, a sting). Slots straight into the JSON schema.
- 💡 **Spoken callouts** — "land ho", lookout calls, crew reactions. Notably these
  are **not** blocked by the render work, so audio callouts can arrive before the
  on-screen versions.

## World & map

Early exploration, scope deliberately open — but the reverse engineering here is
real and documented in [`docs/GAME_API.md`](docs/GAME_API.md). The overworld turns
out to be far more open to modding than expected.

- ✅ **Map + town system mapped** — the overworld is a fixed ~1024 grid, 8-bit
  paletted bitmap at three zoom levels; the engine's town lookup walks **128
  settlement slots** (the base game uses ~44 named cities), and each town is an
  editable record (nation, economy, population, goods).
- ✅ **Towns are runtime-creatable** — a town is table data the game itself
  rewrites (captures, foundings). A mod can create, re-own, or reshape towns
  through the state layer (a complete town is a bundle of linked records, not one
  value).
- ✅ **The map edge is a clean hook** — the "you've strayed too far" boundary
  handler detects exactly which edge you crossed, an ideal place to trigger a
  transition instead of a bounce-back.
- 💡 **A combined, multi-region world** (e.g. Caribbean + Europe + Asia). Three
  shapes, increasing difficulty:
  - 📐 *Compress* all regions into the fixed 1024 grid — asset/data only, safe.
  - 📐 *Zone transitions* — sail to the edge, load the next region. **The promising
    path:** each region keeps its own full map *and* its own ~128 towns, sidestepping
    both the size and town limits. Reuses the save/state system for per-region
    snapshots. A loading transition, not seamless.
  - 💡 *Enlarge the grid* — patch the hardcoded map dimensions. Highest risk
    (many coupled constants, 16× memory, doesn't lift the town cap). Last resort.
- 💡 **Authored towns on any map** — layer real names, nations, and lore over the
  procedurally-placed towns that map mods generate. Natural collaboration point
  with existing map-mod projects.

## Crew & morale

- ✅ **Morale model understood** — the game's own crew-happiness math is mapped
  and readable, so events can react to a genuinely unhappy crew.
- 📐 **Named officers** — limited roster slots, each a persistent character with
  identity, history, needs, wants, and opinions. The headline feature after the
  engine is solid. Not built yet.
- 📐 **Crew as a living resource** — the rank-and-file as a body with morale,
  needs, and opinions that generate events rather than just a number.
- 📐 **Mutiny & splintering** — unhappy crews break off; outcomes like being
  marooned at the next port with your ship stolen, or worse. Designed, not built.

---

## Known blockers & things that need work

### ⛔ Drawing our own visuals (the big one)

Right now PEMF can only show things through the game's existing dialog routines.
Anything genuinely new on screen — the sailing "notice" text, a crew/officer
count indicator, "LAND HO!" callouts, a proper roster panel — needs a hook into
the game's rendering that we don't have yet. Two approaches were tried and ruled
out; the current plan is to locate the renderer's device and hook it directly.
Until this is solved, everything visual is on hold. Everything non-visual works.

### 🟡 Running on every version of the game

The framework currently targets one specific build. Different releases are
different binaries, so internal addresses don't line up; a version-detection step
(fingerprint the game, load the right map) is the plan, and until then PEMF refuses
any build it doesn't recognise rather than risk your game.

The build landscape, now measured:

- **GOG** — fully supported. Plain executable, analysed, every address mapped.
- **Steam** — **DRM-packed.** The executable is compressed/encrypted on disk and
  only unpacks in memory at launch, so its addresses can't be read from the file
  the way GOG's were. Supporting it is a separate, larger effort: run it, dump the
  unpacked image from memory, check whether the underlying build matches GOG (if so,
  the map rebases cheaply), and adapt the hooks to install after the unpacker runs.
  Details in [`docs/GAME_API.md`](docs/GAME_API.md#build-support-gog-vs-steam).
- **Challenge Pack / retail disc** — different builds again; out of scope for now.

### 🟡 Windows Smart App Control

On some Windows 11 machines the mod's files are blocked at startup with a
"Bad Image" / `0xC0E90002` error. That's Windows refusing to load unsigned code,
not a bug in the mod. The fix is code signing, which is in progress. See
[`docs/SIGNING.md`](docs/SIGNING.md) for the full story.

---

## Next up

Rough order, subject to change:

1. **Audio callouts** — an early, satisfying win that *isn't* gated by the render
   work. Confirm the play-by-name path and fire a custom sound from an event.
2. **Solve the render hook.** It's the gate in front of most of the interesting
   *visual* work.
3. **Code signing** so the mod loads cleanly on locked-down Windows installs.
4. **On-screen notices** ("LAND HO!", lookout calls) — the first visual payoff
   once drawing works. Pairs naturally with the audio callouts.
5. **Officer roster** — data model first, then the panel once we can draw it.
6. **Version detection** — broaden the set of game builds supported (GOG + Steam
   first).

---

## Bigger picture — scope is intentionally open

PEMF is **not** just the event engine and officers. Those are the beachhead. The
long-term goal is to expand the game substantially — putting our own simulation
in our own memory and using the original game as the world and renderer beneath
it. A lot of what comes after the foundations is still being figured out, and
that's fine. Some of the directions on the table:

- 💡 Deeper faction / nation politics and standing
- 💡 Economy and trade systems with more moving parts
- 💡 Persistent world events and consequences that outlast a single voyage
- 💡 Richer ship, fleet, and port management
- 💡 Player-authored content beyond events — the modding surface itself as a
  feature, so others can build systems, not just fill in text
- 💡 Custom UI, cards, and eventually new art once drawing is fully in hand

Nothing in this section is a promise. It's the horizon we're steering toward, and
it will get sharper as the groundwork lands. If you have ideas, that's exactly
what this stage is for.

---

## How this document works

Items move up the list (💡 → 📐 → 🚧 → 🟡 → ✅) as they get real. When something
ships, it moves into the "done" sections above with a note on what actually
works. When something turns out to be a dead end, it's cut rather than left to
rot. Treat the roadmap as the current best understanding, not a contract.
