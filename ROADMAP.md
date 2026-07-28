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
- ✅ **Notices stay where they belong** — a notice appears only in the sailing
  view, never over a town, a menu or the Load/Save screen, and its time is
  **paused while it is out of sight**, so opening the map with one up means
  coming back to it rather than finding it gone. This replaced a motion-based
  guess that could not tell a menu from a becalmed ship; the framework now
  learns the overworld's screen signature at runtime instead. Method and
  measurements in
  [`re/experiments/screen_state/`](re/experiments/screen_state/README.md).
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

## Economy & trade goods

Investigated 2026-07-26; findings in [`docs/GAME_API.md`](docs/GAME_API.md).

- ✅ **Cargo model mapped** — seven item slots (`Gold, Food, Luxuries, Goods,
  Spice, Sugar, Cannon`), the hold being one contiguous array at `0x00869AB4`
  indexed by item. Item names come from `text.ini`, not the exe.
- ⛔ **An eighth *engine* slot is not worth pursuing.** The array is boxed in by
  a live global, 82 code sites touch it, individual goods are referenced by
  hardcoded address, and the count is baked into loop bounds. Measured, not
  assumed — the numbers are in `GAME_API.md`.
- 📐 **Adding goods the way that works**: a new good lives in PEMF's own memory
  with its own price model, reads the settlement's real `economy` and `goods`
  fields so it behaves like part of the world, and presents through our own
  drawing. Encouragingly the town side stores **no** per-good table — prices are
  derived — so there is much less fixed structure to fight there than in the
  hold. Its name is a string in its own JSON, drawn through the routines we
  already use.
- ✅ **Tested, and it settled the design**: the engine's item-name list cannot be
  extended with a loose file — the text system never looks on disk, only inside
  the archive. It turns out not to matter, because engine code could not show an
  eighth good anyway. Details in
  [`re/experiments/trade_goods/`](re/experiments/trade_goods/README.md).
- ✅ **Loose-file override works — but per subsystem, not everywhere.** Measured:
  the game reads `assets\data\Landscape.ini` (terrain types, tree canopy, terrain
  streaming) and `Data\AdvancedLighting.ini` (shadow darkness, light levels)
  **from disk in preference to the packed copies**, so those can be changed by
  dropping an edited file in — no repacking, no tools. `text.ini` is not read
  that way. Which files can be overridden depends on which part of the game
  reads them, so it has to be checked rather than assumed; the data-file probe
  answers it in one restart. Nothing to do with trade goods — it fell out of
  proving the text result — but a real lever for anyone changing how the world
  looks.

## Flags & false colours

Findings in [`re/experiments/flags/`](re/experiments/flags/README.md).

- ✅ **Adding flags and sail emblems needs no mod** — and this is worth stating
  plainly, because the opposite is widely believed. The game enumerates
  `custom\flag_*.dds` and `custom\ship_sail_emblem_lrg_*.dds` with a directory
  scan into a growable array; the Options picker is a carousel that wraps around
  however many it found, and the choice is stored **by name** in `Config.ini`.
  Drop eleven flags in and you get eleven. Measured. What *does* require
  replacing files is the five **nation** flags — a different system, and where
  the belief comes from.
- ✅ **The player's flag is a single texture pointer** (`0x008E8FB4`), which the
  engine re-applies on its own. Changing it changes the flag on the mast,
  verified in game. That is the whole presentation half of false colours.
- ✅ **The flag and the game's notion of nationality are unrelated** — measured
  twice, including on a career started under a different nation. The engine has
  no concept of flying false colours, which means the mechanic is **entirely
  PEMF's to define** rather than something to keep in step with engine
  behaviour. Better news than it sounds.
- ✅ **Flags fly by name** — `flag_spa.dds`, through the engine's own texture
  loader, using the recipe the game uses for its `Config.ini` setting. PEMF also
  scans for flags itself at startup, so nothing has to be opened first.
- ✅ **Colours belong to the career, not the settings file** — a disguise is
  stored in that save's sidecar and restored with it. Getting this right meant
  rebuilding career and save/load detection, which turned up two long-standing
  bugs unrelated to flags; see
  [`re/experiments/career_state/`](re/experiments/career_state/README.md).
- 📐 **Your captain's own colours** — a flag chosen per captain at career start,
  rather than inherited from the one global setting every captain shares.
- ✅ **Suspicion** — BUILT and playing. Rises on being observed, falls in open water, per-nation with a memory of where it was earned. Being unmasked drops reputation, strikes your colours and dispatches hunters whose strength and number scale with how badly that crown wants you. Tunable in `PEMF\suspicion.ini`. See [`docs/SUSPICION.md`](docs/SUSPICION.md).
- 📐 *(original design note)* the system that gives a false flag stakes: rising with
  proximity to warships and ports, with time spent under false colours, and with
  infamy; falling with distance and time. Thresholds drive hails, challenges and
  being unmasked. Without it a disguise is a cheat rather than a gamble.
- 💡 **Custom nation art at runtime** — retexture the five nation flag nodes in
  memory instead of replacing files. Additive, reversible, and immune to the
  "pink flags" trap that catches people who install a partial flag pack.
- ✅ **The nation you serve is found** — `0x00869AA8`, an int16. It went unfound
  for a long time because the game never stores the choice made at character
  creation: it stores a consequence, recomputing "your nation" as the crown you
  hold a strictly higher rank with and caching the result. Confirmed against the
  disassembly three ways and measured across four careers under four crowns.
  See [`re/experiments/nations/`](re/experiments/nations/README.md).
- ✅ **Nation relations are readable** — an 8×8 matrix at `0x0085A168`, war and
  treaty, verified changing live in game. Together with the standing arrays this
  is everything Suspicion needs to know who would care about a false flag.
- ⛔ **The AI lever is NOT the ship record's nationality.** That was the plan of
  record and it is dead: the player's field read `0` in four careers begun under
  four different crowns, so it neither records who you are nor plausibly tells
  the AI anything. **There is no hostility flag at all** — nothing in the binary
  reads the player's nationality.
- ✅ **PEMF can build ships.** The engine's own factory, called from PEMF: a
  vessel of any nation, at any port, sailing to any destination. Verified in
  game. This is how the world reacts to a blown disguise, because it is how the
  *game* reacts when a crown decides you are a problem — it dispatches a ship
  rather than flagging one. See
  [`re/experiments/shipyard/`](re/experiments/shipyard/README.md).
- 📐 **A hunter that chases rather than travels.** A spawned ship sails to a
  *place*; pointing it past the player reads as pursuit, but true target-chasing
  has not been found. Re-aiming the destination as the player moves is the
  fallback.
- 📐 **Starting on your faction's colours.** A custom flag in `Config.ini`
  overrides the faction flag on every career — that is the game's own behaviour,
  not a PEMF effect. Now unblocked: the chosen nation can be read directly.
- 💡 **Lowering and raising the flag** when colours change. Flags attach via
  `bone_flag_*_pivot` and are ordinary named scene nodes, so driving the node
  transform from the frame hook is the plausible route. Unproven.

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
