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
| ⚙️ | Built and wired, but not yet watched working in a live game |
| 🟡 | Partially working / needs polish |
| 🚧 | In progress |
| ⛔ | Blocked — needs a prerequisite solved first |
| 📐 | Designed, not yet built |
| 💡 | Idea / under consideration, not committed |

---

## Built 2026-10-16/17, awaiting playtest

Each is written up in [`docs/JOURNAL.md`](docs/JOURNAL.md); all of them add to
the game and replace nothing of its own.

- ⚙️ **Flags are earned** — a letter of marque gives that crown's colours; the
  tavern trader sometimes sells one (`flagtrader.h`).
- ⚙️ **Three kinds of suspicion** — deceit, enemy colours, the black flag;
  distance matters; the panel in the true top-right corner.
- ⚙️ **Hostile crowns** — hunters that truly chase (the stock leash lifted for
  PEMF's hunters only); their governor, merchant and shipwright refuse you.
- ⚙️ **The waterfront** — bribe the harbourmaster, dockside gossip, dockhands,
  the chandler (`waterfront.json`).
- ⚙️ **A moving market** — shortages and gluts announced by port, a war premium
  (`market.h`, `market.ini`).
- ⚙️ **A louder sea battle** — more flying wreckage, a camera kick on your
  broadside, bigger gun smoke (`combatfx.h`, `combat.ini`).
- ⚙️ **The HUD in the screen corners** at any aspect (`hudcorners.h`).
- 📐 **Clothing at character creation** — not started; needs its own research.

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
- ⚙️ **Triggers beyond time and place** — `stateCrosses` fires when a live value
  crosses a threshold: crew, gold, morale or months at sea. Edge-triggered, so
  it fires on crossing and re-arms on crossing back, rather than repeating while
  a value sits past the line. One shape covers every readable value, so adding
  the next one is a line of code rather than a new concept for authors. Built and
  loading; **not yet watched firing in play**, which is the only thing keeping it
  off the list above.

## Audio

Custom sound is a first-class pillar, not a nice-to-have. Existing mods for this
game can only *replace* the built-in sounds by swapping files; because PEMF runs
live code inside the game, it can **add** new sounds and play them on cue — tied
to events, triggers, and game state. As far as we can tell, nobody has done
event-driven audio here before.

- ✅ **PEMF plays its own audio, and can fade it** — a small mixer of our own
  (`src/core/audiomix.h`): Media Foundation decodes a file to PCM once, XAudio2
  loops it with a real gain control. **Shipped and playing** — it is what the
  storm bed runs on. It mixes *alongside* the game rather than through the
  game's mixer, so the game's volume slider does not reach it and anything
  played through it has to be gated on game state by the caller. MCI was tried
  first and rejected: it reports success for a volume change, reads the value
  back, and plays at full volume regardless.
- ✅ **Sound engine mapped** — the game uses Miles Sound System; its whole audio
  import surface and the in-game load/play functions are located.
- 🟡 **The game's own sound path is mapped but never called.** `0x0052F700` is
  the core play-by-numeric-id routine (four thin wrappers reach it:
  `0x004A06C0`, `0x004A07C0`, `0x00528E70`, `0x00528EE0`). This was read from
  decompilation and **has not been run** — PEMF does not call it, and no event
  names a sound. There is no `sound` field in the event schema yet.
- ✅ **Positional audio located** — `0x00488A80` plays a sound *at a world
  position*, and it is what the sailing render uses for ship hails. This is the
  entry point for spoken callouts that come from where the thing actually is.
  Located and documented; **not yet called** from PEMF. (It was misread as a
  text function for a while; see the note in
  [`docs/GAME_API.md`](docs/GAME_API.md).)
- 📐 **Custom clips on events** — the decode-and-play half exists and ships; what
  is missing is a one-shot path (the mixer today drives one looping bed) and a
  `sound` field in the event/notice JSON to name a clip from content.
- 📐 **Sound on notices** — pairing a callout with the floating text, so "Land
  ho!" is heard and seen over the ship at once. Both halves now exist; wiring
  them together is next.

## Weather

Shipped in 0.2.1 and refined in 0.2.2. Tunable throughout from
`PEMF\storms.ini`; the reverse engineering is in `src/core/storms.h`.

- ✅ **Bigger, heavier storms** — how a storm *looks* comes down to three
  immediates in one stretch of `FUN_004612B0`, so this is operand patching
  rather than anything clever. Verified at 8.75× scale in game. ⚠️ The storm and
  the ordinary fair-weather cloud are **different prefabs** (`0x008CCB58` vs
  `0x008CC498`); the first attempt scaled the wrong one and players correctly
  reported that the normal clouds had grown while the storms had not.
- ✅ **Storms that drift** — a storm's *position* is the engine's to own, but the
  shim owns the **arguments** of the draw, so PEMF draws its own drifting
  system. Measured, not assumed: they travel **west at ~350 units/sec** with
  latitude fixed. `param_8` turned out to be a timer rather than an opacity,
  which is why **storms cannot fade** — that is an engine fact, not a to-do.
- ✅ **A storm that costs you something** — optional cargo attrition while you
  are inside real weather (off by default; `cargoLoss` in the ini). The engine's
  own intensity value decides when you are "in it". Note this is **PEMF's**
  effect: the engine's own storms are, as far as we have found, purely visual —
  whether a stock storm does anything to your ship has never been located.
- ✅ **A storm you can hear** — a music bed with real fades, on PEMF's own mixer,
  ducking the game's music. See Audio above for why it is our mixer and not the
  game's.
- 🟡 **A battle keeps the weather it began in** — the sailing view's storm does not
  follow you into a fight, so PEMF swaps the storm prefab into the battle's own
  cloud draw and freezes the system while you are off the overworld. You come back
  out into the same weather you left. On by default (`battleWeather`). 🟡 for the
  half that does not work: the battle's own **rain** draws heavy, static, and
  across only part of the screen, so `battleIntensity` is **off by default** and
  left in the ini to experiment with. It is the cloud that reads as weather here,
  not the rain.
- ⛔ **Three weather slots is a hard ceiling.** The loop at `0x0046354A` indexes
  two parallel position arrays that are **twelve bytes apart** — exactly three
  dwords. A count of four writes the x array into the y array. This knob looks
  like "how much weather" and is really "how far past the end of an array shall
  we go", so it is clamped.
- 💡 **Weather that changes how the sea behaves** — waves, handling, visibility.
  Not located, and possibly not there to find.

- 🚧 **One storm, seen and felt** — the game's own storm rules (found
  2026-10-16: storms push the ship along, damage the fleet in their core, and
  drive the HUD's knots) now measure from the storm PEMF draws, instead of an
  invisible one elsewhere. Built; awaiting a playtest.
- 🚧 **Dynamic wind** — the trade wind still prevails, but it backs and veers,
  and fronts swing it right round. Storms make up to windward and drift
  downwind from any quarter, and every ship's sailing speed follows the same
  wind. Sea battles keep the game's own. Built; awaiting a playtest.
- 🚧 **Storm events** — `stateCrosses` on `storm` (0–3) and `wind` (knots);
  four starter events in `storm_events.json`, including a man overboard.

## Display & resolution

Full write-up in [`docs/WIDESCREEN.md`](docs/WIDESCREEN.md); session record in
[`docs/JOURNAL.md`](docs/JOURNAL.md).

- ✅ **Every resolution in Options** — the stock list kept only 4:3 modes. PEMF
  builds it instead: each size once, smallest to largest, at the desktop's
  refresh rate.
- ✅ **Widescreen without stretching** — the 3D world shows more at the sides
  (vertical view kept); the 4:3 interface stays whole and centred; painted
  screens are scaled evenly to fill the width; cutscene letterbox, fades and
  frame-copy effects run full width; town scenes fill the screen. Corrected at
  the camera choke point, so shader-drawn ships no longer split as they did in
  the first attempt. Verified in game on Steam at 1920×1080.
- ✅ **Mouse follows the centred interface** — one correction at the window,
  before the game reads the cursor.
- ✅ **In-game screenshots** — Ctrl+Shift+X (or `autoShots`) saves the finished
  frame to `PEMF\shots`, for bug reports from a game that minimises on every
  outside capture tool.
- 📐 **HUD at the screen corners** — the Fame dial and key pad sit at the 4:3
  edges; moving them means moving their click areas with them.
- 💡 **Upscaling** — MSAA and anisotropic filtering at the device; higher-
  resolution textures are an asset question.

## Controls

- ✅ **A WASD layout, installed for you** — written into the player's real
  `KeyMap.ini` under `My Games` once, with the original kept beside it as
  `.pemf-backup`, and never touched again, so anything rebound afterwards stays
  put. If the file cannot be backed up it is not replaced. The folder name
  differs between the GOG and Steam installs, which is why the one PEMF chose is
  named in the log rather than assumed.
- ✅ **The keys the sailing view reads past the keymap** — `a` and `s` were
  steering *and* loading and saving, because the sailing view reads those two
  straight from the keyboard rather than from `KeyMap.ini`, so rebinding them in
  the file never had any effect. Steering left would load the last save. They now
  steer and nothing else. Attack moved off Space for the same class of reason:
  Space is the game's own "enter the nearest port".

## Standing

- ✅ **A standing ledger** (`src/core/standing.h`) — reputation treated as an
  *output* rather than something PEMF stores, so it cannot drift out of step
  with what the game itself believes. Used by Suspicion to decide which crowns
  care about a blown disguise.

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
- ✅ **Suspicion** — what gives a false flag stakes, and it is playing. It rises
  while that crown's ships and ports can **see** you and falls in open water,
  per-nation and with a memory of *where* it was earned, so running for the far
  side of the map cools it faster than waiting does. Thresholds drive the signal
  for your colours, the study of your rigging, and finally being unmasked — which
  drops your reputation with that crown, strikes your colours, and dispatches
  hunters whose strength and number scale with how badly they already want you.
  Tunable throughout from `PEMF\suspicion.ini`. See
  [`docs/SUSPICION.md`](docs/SUSPICION.md).
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
- ⚙️ **A hunter that truly chases** — built 2026-10-16, awaiting playtest. The
  decompiled sailing update shows pirate-hunters (purpose 1) already steer for
  the player every tick; what made them give up was a leash to their home port.
  PEMF lifts it for its own hunters only (one redirected call) and ends hunts on
  its own terms. The earlier "aim beyond the player" re-aim is gone. See
  [`docs/SUSPICION.md`](docs/SUSPICION.md#how-a-hunt-actually-works).
- ⚙️ **The challenge card** at suspicion 90 — hold the ruse, strike your
  colours, or run. Built 2026-10-16 as a content event, awaiting playtest.
- 📐 **Starting on your faction's colours.** A custom flag in `Config.ini`
  overrides the faction flag on every career — that is the game's own behaviour,
  not a PEMF effect. Now unblocked: the chosen nation can be read directly.
- 💡 **Lowering and raising the flag** when colours change. Flags attach via
  `bone_flag_*_pivot` and are ordinary named scene nodes, so driving the node
  transform from the frame hook is the plausible route. Unproven.

## The modding surface itself

PEMF is a framework as much as a mod, and this is the part that makes that
true rather than aspirational.

- ✅ **JSON content engine** — events, notices, triggers, effects, placeholders,
  all validated at load with the offending field named. Documented in
  [`docs/EVENT_AUTHORING.md`](docs/EVENT_AUTHORING.md).
- ✅ **Rows in the game's own town menu** — a mod adds its own option to the
  port menu, placed above the row that leaves, so every one of the game's own
  options keeps its position. Limitable to one port or one crown.
- ✅ **Menus of our own, authored as a tree** — a title, options, and each
  option opens another menu, fires an event, or ends on a card. Drawn with the
  engine's own renderer against the port you are standing in.
- ⚙️ **A plugin ABI for code mods** — a 32-bit DLL in `PEMF\plugins\` exporting
  `PemfPluginInit`, handed a table of function pointers. A **C ABI**, so a
  plugin built with another compiler keeps working and a PEMF rebuild does not
  break every plugin in the wild. Faults are contained: a plugin that crashes
  is disabled for the session with its name in the log and the game carries on.
  One header, `sdk/pemf_sdk.h`, which ships inside the release.
  See [`docs/PLUGINS.md`](docs/PLUGINS.md). Short of ✅ for one reason: the host
  and the SDK both ship, but **no plugin exists yet**, so nothing has been
  observed loading through it in a live game.
- 📐 **Plugin hooks beyond menu rows** — per-frame, on entering a port, on a
  battle ending. Menu rows are the only entry point today; the ABI is built to
  grow.
- 📐 **Plugin persistence** — PEMF's own state travels in a sidecar beside each
  save; a plugin has nowhere to put anything yet.

## Crew & officers

Shipped in 0.2.4. Tunable from `PEMF\crew.ini`, and all the content — names,
histories, traits, posts, standings, costs — is authored in
`PEMF\officers\roster.json`. The design and the measurements behind it are in
[`docs/CREW_AND_OFFICERS.md`](docs/CREW_AND_OFFICERS.md).

- ✅ **Morale model understood** — the game's own crew-happiness math is mapped
  and readable, so events can react to a genuinely unhappy crew. There is no
  morale *variable*: `GetMoraleLevel` derives a 0–4 level on every call from the
  crew's share against what they expect.
- ✅ **Named officers** — six posts, three standings, and a card for the man who
  turns up: his name, his history, one to three talents, and possibly a flaw.
  **Paying buys the search, not the officer** — the gold sends the crew ashore to
  ask after somebody, and whether anyone is found is a roll. An interactive
  roster where each man can be spoken to or examined, and every officer travels
  in the save sidecar, written as authored ids so an edited `roster.json` does
  not silently turn him into a different man.
- ✅ **Officer talents that change the real game** — a trait names a `target`,
  and PEMF only offers targets it can actually deliver: what is plundered, the
  crew's temper, cargo saved in a blow, how long false colours hold, and how
  many of the wounded live. A trait naming anything else is **refused at load
  with the supported list**, because a framework that silently accepts a target
  it cannot honour teaches authors to distrust the whole schema. Flaws are the
  same mechanism with a negative value.
- ✅ **PEMF's own crew morale** — a signed −100…100 scale with seven tiers of its
  own, because the engine's five have no negative, no resolution, and names that
  live in `text.ini` where they cannot be extended. It moves on the plunder being
  divided, men going over the side, months at sea, and the officers kept, and it
  moves **slowly**, so it reads as a voyage rather than a bar that twitches.
- ✅ **Our morale drives the engine's** — one byte at `0x00869B27` has exactly one
  cross reference and no engine writer, so PEMF **solves the engine's own formula
  backwards** for the value that produces the level it wants. The HUD icon and
  the engine's own desertion behaviour then follow our number. Measured, and the
  measurement shaped the code: the byte reaches every level but in only about
  four usable notches, and some levels are unreachable at some wealths — so the
  solver searches, keeps the byte it has when an alternative is only equally
  good, and logs a mismatch as expected rather than letting it read as a bug.
- ✅ **PEMF's share of the plunder** — `0x00861FF8` is a running total that rises
  when plunder is *earned* and never when it is *spent*, so a share is applied by
  watching it move rather than by hooking any award site. That covers combat,
  sacking a town and digging up treasure at once, with no `.text` write.
  Measured: 660 plundered became 990 at +50%. Off by default.
- ⚙️ **Men a surgeon puts back** — crew losses are sampled the same way, and a
  fall in the crew count is read as men lost. Built and wired, but the saving has
  not been watched happen in a live game. The known cost of sampling is that PEMF
  cannot tell *why* men were lost, so a surgeon saves them from disease and from
  a cutlass alike.
- 📐 **Officers with opinions** — an officer today has a name, a history, a
  tenure and his talents. He has no view of the other five and no loyalty of his
  own that can be lost. Relationships were always the second half of "named
  officers" and none of it is built.
- 📐 **Paying an officer off** — one post holds one man, and the refusal to hire a
  second tells the player to pay the first off. There is no row that does it yet.
- 📐 **Crew as a living resource** — the rank-and-file have a temper now, but it
  rarely says anything of its own. `stateCrosses` can fire an event on morale
  crossing a line, which is the hook; the authored content that grows events out
  of crew state is missing.
- 📐 **Mutiny & splintering** — unhappy crews break off; outcomes like being
  marooned at the next port with your ship stolen, or worse. PEMF's scale already
  reaches MUTINOUS and the engine's own desertion follows our byte, so an
  unhappy crew thins out — but nothing takes the ship off the player. Designed,
  not built.

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

### 🔴 Windows blocks unsigned code — and PEMF will stay unsigned

Two separate symptoms, one root cause. Defender may refuse the **download** as
`Trojan:Win32/Wacatac.B!ml` — a cloud machine-learning guess, not a signature
match, and a false positive. On some Windows 11 machines Smart App Control
blocks the game at **startup** with "Bad Image" / `0xC0E90002`.

Code signing would answer both. We applied to the SignPath Foundation's free
OSS programme and **were rejected**; paid certificates are out of scope for this
project. So the workarounds are the answer: allow the file by hash, and turn
Smart App Control off if it's on. Reputation should erode the antivirus half
over time, and a false-positive report is filed with Microsoft.

See [`docs/WINDOWS_SECURITY.md`](docs/WINDOWS_SECURITY.md) for the full story,
the published hashes, and the approaches already ruled out.

---

## Next up

Rough order, subject to change:

1. **Event cards from inside the frame** (stage 3). The game's dialog is
   **modal and blocking**, so presenting it from the frame hook re-enters
   `BeginScene`/`EndScene` from the dialog's own message loop — that needs a
   re-entrancy guard, not just raising the stage constant.
   *Note the reason has changed:* this used to be listed as the fix for the
   half-drawn background behind cards. **That bug is already fixed** — the draw
   paths lacked an `events::Busy()` guard, and the 250ms overworld gate went
   stale during a card's nested loop, which was the leak. Stage 3 is now about
   presenting cards natively, not about repairing a visible artifact.
2. **What the crew system is missing, in the order that makes each one worth
   having.** Officers and morale shipped in 0.2.4, so this is no longer "build
   the data model" — it is finishing what the data model made possible. Mutiny
   first: PEMF's scale already reaches MUTINOUS and nothing happens when it gets
   there, which is the largest gap between what the number says and what the
   player feels. Then paying an officer off, so a post is not filled for the
   career. Then officers who have opinions of each other, which is the half of
   "named officers" that was never built. See *Crew & officers* above.
3. **Authored content that uses what now exists.** The engine grew faster than
   the content did: `stateCrosses` can fire on a souring crew, town-menu rows and
   nested menus take authored JSON, and almost nothing ships that uses either.
   This is writing rather than research, and it is what turns the systems into
   something a player meets.
4. **Labels on world objects** — the world-text facility takes any map
   position, not just the player's ship, so ports, rivals and waypoints can all
   carry our own text.
5. **Audio: custom clips and callouts** — a one-shot path on our existing mixer,
   a `sound` field in the event schema, and a voice line at the ship's position
   paired with the notice above it. Deliberately parked; the mixer already
   works, so this is plumbing rather than research whenever we want it.
6. **Version detection** — broaden support beyond GOG and Steam.

**Code signing is not on this list.** The SignPath Foundation rejected our
application and paid certificates are out of scope, so PEMF ships unsigned and
the workarounds in [`docs/WINDOWS_SECURITY.md`](docs/WINDOWS_SECURITY.md) are
the answer rather than a stopgap. (`build.ps1 -Sign` is kept in case that ever
changes.)

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

Items move up the list (💡 → 📐 → 🚧 → 🟡 → ⚙️ → ✅) as they get real. When something
ships, it moves into the "done" sections above with a note on what actually
works. When something turns out to be a dead end, it's cut rather than left to
rot. Treat the roadmap as the current best understanding, not a contract.
