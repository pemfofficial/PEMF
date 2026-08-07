# PEMF 0.2.5 — Alt-Tab

A fix-only release. No new features, one long-standing crash closed, and one
thing PEMF was doing to your CPU that it had no business doing.

**0.2.4 claimed to fix the alt-tab crash. It did not, and this is the honest
second attempt** — 0.2.4 stopped PEMF drawing *during* a device reset, which was
necessary and not sufficient. The rest is below.

## The alt-tab crash

Alt-tab back into the game and you could get:

```
Microsoft Visual C++ Runtime Library
Runtime Error!
R6025 - pure virtual function call
```

When you alt-tab away from a fullscreen game, Direct3D takes the graphics device
away and hands it back when you return. PEMF knew about that and stood down
while it was happening.

What it got wrong is what "handed back" means. The device is usable the instant
the reset finishes — but the **game's** own graphics objects are not. The engine
rebuilds those over the next few frames, and everything PEMF draws goes through
the game's own routines, which walk exactly those objects. So PEMF started
drawing again one frame too early, into a scene that was still half rebuilt, and
called a function on an object that had been thrown away and not yet replaced.

That is precisely what "pure virtual function call" means, and it is why the
crash always landed on the way *back* into the game rather than on the way out.

PEMF now waits for the engine to settle after a reset before it draws anything.

## PEMF was burning a core while you were away

Not a crash, but the more interesting find, and you may notice this one.

*Pirates!* does not go quiet when you alt-tab away. It spins — calling the
graphics driver about 8,000 times a second and drawing nothing at all, for as
long as you are gone. PEMF's own once-per-frame work runs off that same loop, so
it was spinning right alongside: scanning the ship array, ageing suspicion,
dispatching pirate-hunters, moving storms, and writing world samples to the log.
Several thousand times a second. For results nobody could see, because nothing
was being drawn.

One tester sat like that in ten-minute stretches while copying a large file, with
PEMF eating a core throughout. It also wrote about four thousand junk lines into
`pemf.log`, which was most of the reason their log was half a megabyte.

PEMF now notices when the game has stopped drawing, stands down completely, and
gives the CPU back — which is presumably what you alt-tabbed away to get. It
picks straight back up when frames return, and the clocks it keeps are rebased
on the way in, so ten minutes away no longer ages every suspicion trail to
nothing the moment you come back.

Your log will be dramatically smaller as a side effect.

## Known, and not fixed

**A hard lockup on alt-tab is still possible.** One tester hit a black screen
that survived alt-tabbing back, with the game unresponsive but Windows fine
underneath. Their log ends inside the graphics driver's own reset call, with PEMF
sitting in the middle doing nothing but passing the call through — we were not
holding any graphics resources, and were not re-entered while it ran.

That looks like a fragility in how a 2004 Direct3D game handles being minimised
for a long time on a modern driver, rather than something PEMF causes. The
changes above remove PEMF from that window almost entirely, which should make it
rarer, but it would be dishonest to call it fixed.

If you hit it: **Ctrl+Shift+Esc** and end `Pirates!.exe` — Task Manager draws
above the stuck window, so you do not need to reboot. Then send us `pemf.log`.
Playing in windowed mode avoids the whole class of problem, because the device is
never lost in the first place.

## Installing

Extract into your game folder, over the top of any earlier version. Saves, key
bindings and edited `.ini` files are left alone.

⚠️ Windows' own "Extract All" creates a subfolder instead of putting the files
where you pointed it — and when upgrading that is worse than nothing, because the
old DLLs stay in place and the game keeps running the old version. `INSTALL.txt`
has the detail. Check the third line of `pemf.log`: it names the version, and
says so in capitals if the two DLLs do not match.
