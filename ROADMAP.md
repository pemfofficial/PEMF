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
- ✅ **Two presentation styles** — the interrupting modal card, and the
  non-interrupting on-screen "notice" (a lookout's call while sailing). Both
  are drawn in-game.
- ✅ **Notices anchored to your ship** — a notice can hang over the player's
  vessel and **follow it as you sail**, turning and moving with the ship, then
  easing out at the end. It is drawn by the same routine the game uses for its
  own ship labels, so it looks native because it is. `"anchor": "ship"` works on
  **any** notice, whatever fired it — placement and trigger are independent.
- ✅ **Authoring without the sharp edges** — a `{placeholder}` layer over the
  engine's own text tokens. An author writes `"Land ho! {port} off the bow!"`
  and supplies no arguments at all; PEMF carries the token and its value
  together so they cannot fall out of step. This matters more than it looks:
  the engine's `@CITYNAME` consumes *three* arguments rather than one, and
  getting that wrong reads stack garbage. `{port}` hides it entirely.
- ✅ **Live world values in authored text** — `{port}`, `{portNation}`,
  `{portType}` name the nearest settlement, so an event can say
  *"Land ho! Nevis off the bow!"* Verified in-game.
- ✅ **Triggers beyond time and place** — `stateCrosses` fires when a live value
  crosses a threshold: crew, gold, morale or months at sea. Edge-triggered, so
  it fires on crossing and re-arms on crossing back, rather than repeating while
  a value sits past the line. One shape covers every readable value, so adding
  the next one is a line of code rather than a new concept for authors.

## Audio

Custom sound is a first-class pillar, not a nice-to-have. Existing mods for this
game can only *replace* the built-in sounds by swapping files; because PEMF runs
live code inside the game, it can **add** new sounds and play them on cue — tied
to events, triggers, and game state. As far as we can tell, nobody has done
event-driven audio here before.

- ✅ **Sound engine mapped** — the game uses Miles Sound System; its whole audio
  import surface and the in-game load/play functions are located.
- ✅ **Playing a named game sound** — events name a sound and it plays with the
  card, through the game's own presentation path. Confirmed in-game.
- ✅ **Positional audio located** — `0x00488A80` plays a sound *at a world
  position*, and it is what the sailing render uses for ship hails. This is the
  entry point for spoken callouts that come from where the thing actually is.
  (It was misread as a text function for a while; see the note in
  [`docs/GAME_API.md`](docs/GAME_API.md).)
- 📐 **Custom clips added, not just swapped** — drop a new `.wav` in and play it;
  the game already loads loose `.wav` files by name, which is the groundwork.
  This is the remaining piece: today we can only play sounds the game ships.
- 📐 **Sound on notices** — pairing a callout with the floating text, so "Land
  ho!" is heard and seen over the ship at once. Both halves now exist; wiring
  them together is next.

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

### ✅ Drawing our own visuals — solved

The long-standing blocker is gone. PEMF sits **inside the frame**, hooked onto
the game's own Direct3D device, and draws its own text — at the top of the
screen, or anchored in the world so it tracks the player's ship. It survives
the engine rebuilding its device state mid-session (it re-verifies and
re-installs itself), and a heartbeat in the log means a dead hook can never
pass unnoticed. A draw that faults disables drawing for the session rather than
repeating: a missing notice, never a crashing game.

Two phases, because the game treats the two kinds of text completely
differently and they are not interchangeable:

- **Screen text** is an immediate 2D blit, drawn at `EndScene` so it lands on
  top of the finished frame.
- **World-anchored text** builds scene-graph nodes that the render walk then
  draws, so it is issued at `BeginScene`, before the world is built. Hand it a
  map position and the game re-projects it every frame — which is what makes a
  notice follow the ship for free, with no projection maths of our own.

Text is done. Still open, and now ordinary work rather than research: drawing
**shapes and art** of our own (panels, indicators, a roster), and presenting
event cards from inside the frame. Details in
[`docs/GAME_API.md`](docs/GAME_API.md#drawing-our-own-text--solved-and-how).

### ✅ GOG and Steam both supported

Both storefront builds run from one codebase, with no separate offset map.

- **GOG** — plain executable, analysed, every address mapped.
- **Steam** — **solved.** The executable is DRM-packed: encrypted on disk,
  unpacked in memory at launch. The packer wrecks the import *name* tables, so
  hooking by name fails outright — but the import *slots* are populated at their
  known addresses, and the underlying build is the same one GOG ships, so the
  whole offset map transfers unchanged. PEMF waits for the game to unpack and
  then hooks the slots **by absolute address**. The mod and the DRM coexist
  cleanly, provided every image read is fault-guarded, which is now an
  invariant rather than a precaution. Details in
  [`docs/GAME_API.md`](docs/GAME_API.md#build-support-gog-vs-steam).

Still open: **Challenge Pack / retail disc** are different builds, and PEMF
refuses any build it does not recognise rather than risk your game. Broadening
that set means a version-detection step — fingerprint the game, load the right
map — which is now ordinary work rather than research.

### 🟡 Windows Smart App Control

On some Windows 11 machines the mod's files are blocked at startup with a
"Bad Image" / `0xC0E90002` error. That's Windows refusing to load unsigned code,
not a bug in the mod. The fix is code signing, which is in progress. See
[`docs/SIGNING.md`](docs/SIGNING.md) for the full story.

---

## Next up

Rough order, subject to change:

1. **Custom clips** — play a `.wav` we ship, not just a sound the game already
   has, and name it from an event.
2. **Callout plus floating text together** — a voice line at the ship's world
   position paired with the notice above it. Both halves exist now.
3. **Event cards from inside the frame** (stage 3) — fixes the half-drawn
   background behind dialogs, which is the one visible rough edge left. The
   scaffolding exists; the work is that the game's dialog is **modal and
   blocking**, so presenting it from inside the frame hook re-enters
   `BeginScene`/`EndScene` from the dialog's own message loop. That needs a
   re-entrancy guard, not just raising the stage constant.
4. **Labels on world objects** — the world-text facility takes any map
   position, not just the player's ship, so ports, rivals and waypoints can all
   carry our own text.
5. **Code signing** so the mod loads cleanly on locked-down Windows installs
   (application submitted, awaiting approval).
6. **Officer roster** — data model first, then the panel, now that drawing is
   within reach.
7. **Version detection** — broaden support beyond GOG and Steam.

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
