# PEMF 0.2.4 — Officers and Crew

The biggest release so far. PEMF now puts **its own options in the game's town
menu**, hires **named officers** with talents and flaws, keeps **its own crew
morale** that drives the game's, and ships an **SDK** so other people can write
mods in code.

---

## Named officers

Walk into any port and there is a new line in the menu: **Manage yer crew!**

Six posts to fill — Quartermaster, Sailing Master, Bosun, Master Gunner,
Carpenter, Surgeon — each at three standings.

**You are paying for the search, not the man.** The gold sends your crew ashore
to ask after somebody of that standing. Whether anyone turns up is a roll, and
you pay either way. A Master is dear and often fruitless, which is rather the
point of him being a Master.

If somebody is found you get a card: his name, his standing, a line of his
history, and what he is good at. Take him on or let him walk.

### They are not interchangeable

Every officer is generated: a name, a history, one to three talents — **and
possibly a flaw**. Two Master Bosuns are not the same man.

A flaw is a real cost, not flavour text. *Light Fingers* loses you part of every
prize. *A Loose Tongue* gets your disguise seen through sooner. *More Butcher
Than Surgeon* means men who would have lived do not. The cheap searches turn up
flawed men far more often than the dear ones.

### What they actually do

Everything an officer's traits promise, they deliver:

| | |
|---|---|
| **Plunder** | a sharp-eyed quartermaster adds to every prize you take |
| **The crew's temper** | a well-liked officer makes a crew that recovers to a better place |
| **Cargo** | a good carpenter keeps cargo aboard when the sea comes over the rail |
| **Your disguise** | a close-mouthed officer buys you time under false colours |
| **The wounded** | a real surgeon puts men back who would otherwise be lost |

### The roster

**Look over the roster** lists your officers, and each one can be spoken to.
Ask how the men find the voyage and he will tell you, in his own words. Ask what
he does for you and you get his standing, how long he has been aboard, his
history, and every trait with what it is actually worth.

---

## Crew morale

The game has always had a crew mood. PEMF now has its own, and it is wider:
seven states from **MUTINOUS** through **STEADY** to **DEVOTED**, including
places worse than the game can express on its own.

It moves for real reasons — the officers you keep, cargo lost in a storm, time
and events — and it moves **slowly**, so it is something you notice over a
voyage rather than a bar that twitches.

And it is not decoration. PEMF drives the game's own morale from it, so the
mood on your crew's faces and their willingness to stay follow the number.

> Your morale and your officers now travel **in your save**. Load an old game
> and that career's crew comes back with it.

---

## For modders: write a mod in code

PEMF has always been a framework for authored content. It now takes **plugins**.

A plugin is a small DLL in `PEMF\plugins\`. PEMF hands it a table of functions
and it can read and change the game — add its own row to the town menu, show
cards, post notices, read and adjust crew, gold and reputation, fire events.

Everything you need is one header, `PEMF\sdk\pemf_sdk.h`, **which ships with the
mod** — no repository to clone. A plugin that crashes is disabled for the
session with its name in the log, and the game carries on.

Full details in [`PLUGINS.md`](PLUGINS.md).

### And more for content authors

- **Your own rows in the town menu**, from JSON, limitable to one port or one
  crown.
- **Menus of your own** — a title, options, and each option opens another menu,
  fires an event, or ends on a card. Nest them as deep as you like.
- Officer names, histories, traits, roles and costs are all in
  `PEMF\officers\roster.json` and all meant to be edited.

See [`EVENT_AUTHORING.md`](EVENT_AUTHORING.md).

---

## Fixes

**The game no longer crashes when you alt-tab.** Returning to the game could
put up `R6025 - pure virtual function call` and take it down. Alt-tab loses the
graphics device, and PEMF was still drawing into it while the game was rebuilding
it. It now stands down until the device comes back.

**Storm drums come back.** They played once and then stayed silent for the rest
of the session, even in new weather. PEMF was trusting its own note of whether
the track was playing rather than asking the sound engine, and once those two
disagreed nothing ever restarted it.

**Pausing no longer kills the storm music.** Opening the menu took the sea off
screen, which read as leaving the storm. A pause now holds the drums where they
are.

**WASD:** the marker line that controls the keymap shipped in the wrong file —
the copy in your game folder, rather than the one in `My Documents` that
actually decides anything. Deleting it did nothing, correctly reported as such.
The marker is now written into the file it talks about and says which one that
is.

---

## Installing

Extract into your Pirates! folder, next to `Pirates!.exe` — **not into a
subfolder**. Works on **GOG and Steam**. Saves, key bindings and edited `.ini`
files are untouched.

To uninstall, remove `version.dll` and `pemf_core.dll`.

PEMF is **unsigned**, so Smart App Control will block it and Defender may flag
the download. See [`WINDOWS_SECURITY.md`](WINDOWS_SECURITY.md) — the short
version is that this is what every PC game mod has looked like for twenty years
and there is no certificate to be had.

## Tuning

| File | What it controls |
|---|---|
| `PEMF\suspicion.ini` | False colours, hunters, standing |
| `PEMF\storms.ini` | Everything about weather |
| `PEMF\crew.ini` | The crew and officer systems |
| `PEMF\officers\roster.json` | Officer names, histories, traits, roles, costs |

All commented, and all survive a reinstall.

## Reporting

Post `pemf.log` from your game folder with a sentence about what you were doing.
The first line says which version you are on — please include it. If the game
will not start at all, `pemf_proxy.log` from the same folder is written even
when nothing else is.
