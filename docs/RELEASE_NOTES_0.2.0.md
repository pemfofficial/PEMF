@everyone **False Flag is here.**

This is PEMF's first release with a **complete, brand-new gameplay mechanic** in
it — not a tweak to something the game already did, but a system that never
existed in Pirates!

Fly another nation's colours. Find out what it costs when they stop believing
you.

---

## Install

1. Find your game folder — the one with `Pirates!.exe` in it.
   *(Steam: right-click the game → Manage → Browse local files)*
2. Drop everything from the archive into it.
3. Launch.

**GOG and Steam both work.** The game executable is never modified. To uninstall,
delete `version.dll`, `pemf_core.dll` and the `PEMF` folder.

---

## What you'll notice straight away

**WASD steering**, everywhere — sailing, ship battles, land battles, duels,
sneaking. Two keys had to move to make room:

| | Was | Now |
|---|---|---|
| Attack ship | `a` | **Space** |
| Quick save | `S` | **F5** (quick load is **F9**) |

Your original bindings are backed up, and PEMF won't touch the file again — so
anything you rebind afterwards stays put.

---

## Playing False Flag

| Key | Does |
|---|---|
| **Ctrl+Shift+8** | Run up a different flag |
| **Ctrl+Shift+9** | Run your own colours back up |
| **Ctrl+Shift+0** | Write a report to `pemf.log` |

Raise a nation's flag that isn't yours and a meter appears in the top right:

```
                                     Spanish colours
                                     ======......  <
```

It **fills while their ships and ports can see you**, and **empties in open
water**. The `<` means someone's watching right now.

That's the whole tension: **a disguise is worth wearing because it lets you get
close — and getting close is exactly what gives you away.**

They'll signal for your colours. Then study your rigging. Then come about.

If the meter fills, you're rumbled:

- your **reputation with that nation drops** — a real number the game already uses
- their **ports close** to you, and past a point there's a **price on your head**
- your **colours are struck**
- **pirate-hunters sail** from their nearest port

Sail away and the trail goes cold — the further you get, the faster it cools.
Hunters break off eventually, or if you get clear enough. **How many come, and
how strong they are, depends on how much that nation already hates you.** A first
offence is one ship. A long history is a squadron.

### Tuning it

Everything's in **`PEMF\suspicion.ini`** — rise and decay rates, distances,
thresholds, what being caught costs, how doggedly hunters chase. It's commented,
and reinstalling won't overwrite your edits.

**It's deliberately tuned fast right now** so you can see the whole arc in a
minute. If you want it subtler, lower the three `rise*` values.

### Two known rough edges

- You start under **English colours** regardless of the nation you chose. That's
  an unfinished feature, not a bug — the flag you begin with isn't wired to your
  faction yet.
- Suspicion is quick. See above.

---

## For the technically curious

**Engine & hooks.** A `version.dll` proxy loads PEMF into the game. Hooks go into
the import table, never into the game's code — nothing is patched, nothing is
rewritten. All game logic runs from one identified safe point at the top of the
main loop, so nothing is ever done mid-render.

**Both store builds.** GOG's executable is byte-identical to our reference. Steam's
is DRM-packed — its code unpacks from 1.1 MB on disk to 5 MB in memory — so PEMF
waits for the unpack, hooks by absolute address, and **signature-checks every
engine function it calls at runtime**. A build that has drifted loses one feature
with an explanation in the log rather than crashing.

**Narrative engine.** Events are JSON, not code. Drop a file in `PEMF\events\`
and it loads; a broken one is skipped with a reason. Events render through the
game's *own* text and dialog routines, so they look native because they are.
Triggers fire on elapsed sailing, port proximity, or a live value crossing a line.

**What False Flag is actually doing.** It reads the game's real nation-relations
matrix and your standing with each crown, and when you're unmasked it writes the
game's own reputation number — then lets the engine's existing machinery close
ports and post bounties. The pirate-hunters are built through the game's own ship
factory, classified so they hover as *"Spanish pirate-hunter"*, and the engine
treats them as genuinely hostile — they hail you with its own dialogue, not ours.

**Research suite.** Offset maps, Ghidra scripts, and a Python toolkit for
dissecting the shipyard, trade goods, nation relations and audio routines. Every
investigation is written up — including the ones that concluded "no."

---

## Bugs

Grab **`pemf.log`** from your game folder and post it with a sentence about what
you were doing. The log is where everything useful is.

Developer probes are off by default. If you want them for a repro, drop an empty
file called **`dev.on`** into `PEMF\`.

---

## What's next

No promises on timing, but these are confirmed possible and being worked on:

- Polishing False Flag toward a fully native-feeling system
- A deeper crew system
- Custom trade and economy
- UI systems to carry future mechanics
- Multi-faction naval interactions and shifting territory
- An expanded overworld with new regions
- More authoring power in the JSON layer

---

*PEMF is free to use and redistribute. Not affiliated with Firaxis, 2K or Atari.*
