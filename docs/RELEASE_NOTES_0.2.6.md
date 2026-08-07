# PEMF 0.2.6 — Steering

A fix-only release, and the important one is embarrassing: **steering left could
load a savegame.**

## Steering left loaded your last save

Reported as "after a while the game teleports you to a port, says a game was
loaded, and dumps you in the town menu — and it keeps doing it until I restart."

The sailing view reads a few keys **straight from the keyboard** rather than
from `KeyMap.ini`, in a jump table inside the executable. Three of them are save
and load:

- `A` — load the **`arrival`** save (the autosave written when you reach a port)
- `L` — quick load
- `S` — quick save

PEMF's WASD layout put steering on `A` and `S`. So steering left also loaded the
arrival save: teleported to that port, "Game loaded.", town menu. Steer left
again and it happened again.

This hid for a long time because `KeyMap.ini` *says* `QuickLoad_L = F9` and
`QuickSave_S = F5`, so the keys looked rebound. They were not — that file has no
say over this table. `W` and `D` do not appear in it, which is exactly why only
two of the four steering keys ever misbehaved.

`A` and `S` now steer and nothing else. `L` still quick-loads, as in the stock
game — it is not a steering key and it is not ours to take away.

## Attack has moved off Space, to `f`

Same class of fault, milder. Space is the game's own **"enter the nearest
port"** key, so attacking a ship within reach of a harbour docked you instead.

Attack is now **`f`**, which the game already uses for Fire during a sea battle,
so it is the same finger for the same job in both places.

⚠️ **Your `KeyMap.ini` is rewritten once** to pick this up, because PEMF
otherwise writes that file a single time and never touches it again — which
would have left the collision in place on every existing install, permanently.
Anything you rebind *after* this update is yours and stays. Your original
pre-PEMF bindings are untouched in `KeyMap.ini.pemf-backup`.

## Ports get their records back

Every time a nation sent a pirate-hunter after you, PEMF set a flag on that
port's record and stored the hunter's slot there — and never cleared either one.
Not when the hunter broke off, not when she sank, not when the career ended, not
when you loaded a save. PEMF was writing a permanent change into the game's own
city table and never taking it back, which is the one thing a mod must not do.

Cleared now, however the hunt ends, and cleared for every port when a career
ends or a save is loaded.

## Under the hood

The ship accessors took an index and used it unchecked. The array holds 256
records, and slot 256 would land squarely on the sailing tick counter and the
view flags. Every caller happened to be correct and nothing enforced it. Now out
of range is a hard stop.

## Known issue: alt-tabbing quickly

**Alt-tab out and straight back in — before Windows has finished putting your
desktop resolution back — and the game can lock up on a black screen** that has
to be force-closed.

Wait a beat before switching back and you will not see it. If it happens:
**Ctrl+Shift+Esc → Pirates!.exe → End task.** Task Manager draws above the stuck
window, so no reboot is needed, and **your saves are not at risk** — the game is
stuck, not writing.

Playing in a window avoids the whole class of problem, because the graphics
device is never taken away.

This one is ours, not the game's: the stock game survives the same test. It is
still being worked on, and it would be dishonest to imply otherwise.

## Not a bug, and now documented

The sailing master's *"We're far out to sea captain..."* card has been reported
three times as PEMF's doing. It is not. Two independent distance calculations —
ours and the engine's own — were logged side by side and agree exactly. The
game's threshold is simply 60 leagues to the nearest **national** port, which is
easily crossed while coasting in sight of land, and native villages and missions
do not count toward it.

The full account is in `docs/GAME_API.md` so nobody investigates it a fourth
time.

## Installing

Extract into your game folder, over the top of any earlier version. Saves and
edited `.ini` files are left alone; `KeyMap.ini` is rewritten once, as above.

⚠️ Windows' "Extract All" creates a subfolder instead of putting the files where
you pointed it — and when upgrading that is worse than nothing, because the old
DLLs stay in place and the game keeps running the old version. Check the third
line of `pemf.log`: it names the version, and says so in capitals if the two DLLs
do not match.
