# PEMF 0.3.0 — The Black Flag

The biggest release so far: you can start as a pirate, flags are earned,
suspicion depends on how each crown sees you, ports have a waterfront and a
moving market, sea battles are louder, the weather moves, and the game runs
widescreen. Tested on Steam. `docs/JOURNAL.md` has the dated detail for every
item.

**Install:** extract `PEMF-0.3.0.zip` into the game folder (the one with
`Pirates!.exe`), replacing older PEMF files. Your tuning files are kept if you
edited them; the new defaults are placed beside them as `.ini.new`.

## New for players

- **Start as a pirate.** A new career signs on with no crown and begins in a
  pirate haven, flying the black flag. The captain is dressed in a crown's
  colours (`pirateOutfit`). Turn it off with `startUnaligned = 0` in
  `suspicion.ini` for the shipped character creation.
- **Flags are earned.** A governor's letter of marque brings his crown's
  colours; the tavern's trinket seller sometimes has a flag for sale. Press
  **Q** at sea to choose which colours to fly.
- **Suspicion by relationship (V2).** The black flag makes every crown
  suspicious. A crown's own colours with its commission make you a friend.
  Aggression is what turns suspicion into certainty, and worse deeds cost
  more. At the top of the meter a ship of that crown trails you and backs off
  if you stay quiet. Your own fleet no longer counts as watchers.
- **Pirate-hunters** come only from crowns that hate you: one at a time, more
  the worse they think of you, and they give up if you sit in port. Hostile
  crowns' governors, merchants and shipwrights refuse you.
- **The waterfront** in every port: bribe the harbourmaster (a hostile port
  can open again), hear dockside gossip, hire hands, buy stores.
- **A moving market.** Shortages and gluts come and go around ports, and war
  raises the price of food and cannon. The game's own prices are left as they
  are; PEMF adds movement on top.
- **Crew at sea (C)**, officers who unlock events, and an in-game **guide
  (H)**. Q, C and H work only while sailing, never in battle.
- **New events** at sea, for officers, around storms and suspicion, and in the
  world.
- **Louder sea battles.** More wreckage and men overboard, bigger gun smoke,
  smoke that hangs where shot strikes, and a deck that shakes on your
  broadside.
- **A battle song** that plays only while a ship battle is on screen and hands
  the game's music back everywhere else. Song by Nastelborn.
- **Weather.** A wind that backs and veers, with rare sharp fronts. Storms
  drift with the wind, and the game's storm rules (the push, damage in the
  core, the knots readout) now follow PEMF's storm rather than an invisible
  one.
- **Widescreen and higher resolutions.** Every display mode in the menu, a
  wider 3D view, the 4:3 interface centred with a working mouse, and the HUD
  in the true corners of the screen.
- **The sailing master** speaks only when you really are far out to sea.
- **Tuning files update themselves** unless you've edited them. Edited files
  are kept, with the new defaults beside them as `.ini.new`.

## For modders and contributors

- **Steam checked without launching it.** `re/experiments/steam_static_verification`
  compares every address, byte signature and patch site PEMF uses against the
  unpacked Steam executable. All 417 addresses and 32 signatures pass. Finding:
  Steam is a separate build of the same 1.0.2.0 source. Code and game state are
  at the same addresses as GOG, but some read-only data (vtables, float
  constants, strings) has moved, so any such address taken from the GOG file
  must be checked on Steam. The Steam packer is PECompact 2.
- **The text system, mapped.** Every token the game defines (72), with its
  type and exact argument count; the 4,096-byte message buffer; `AddText`
  replace versus append mode; the layout of name tokens. In `GAME_API.md`.
  This is the groundwork for letting event authors use far more than `@NUM`
  and `@HAPPY`.
- New docs: `JOURNAL.md` (dated session log), `TROUBLESHOOTING.md`. Updated:
  `GAME_API.md`, `WIDESCREEN.md`, `SUSPICION.md`, `CREW_AND_OFFICERS.md`,
  `PLAYER_MANUAL.md`.
- New tuning files: `combat.ini`, `market.ini`, `keys.ini`, `widescreen.ini`.
  `tools/validate_events.cpp` now checks that every menu option leads
  somewhere.

## Known limits

- The suspicion panel can still draw over some cards the sailing view shows
  itself.
- The pirate start skips the "Welcome aboard" walk, and the line before it
  still shows.
- Officer loyalty is not built yet; the crew menu and officer-gated events are.
