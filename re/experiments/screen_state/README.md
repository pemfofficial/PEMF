# How does the mod know the overworld is on screen?

## The short version

PEMF draws its own text into the game's frame. World-anchored text is given a
map position and the engine projects it against **whatever camera the current
screen is using** — so a notice drawn while a menu is up lands *on that menu*.
A player reported exactly that: a lookout's call reading *"Land ho! Martinique
off the bow!"* printed across the Load/Save screen.

The gate at the time asked **"has the ship moved in the last 350 ms?"**. That is
not a screen test. Opening a menu freezes the ship, so a notice went on painting
over the menu until the window lapsed — which is why it appeared briefly and
then vanished, as though it were *almost* getting it right.

**Tightening the window cannot fix this.** A becalmed or paused ship at sea is
indistinguishable from a menu under any motion test, so a shorter window only
trades a menu leak for notices disappearing while you sit still. The two
failures are the same bug seen from either side.

What was needed was a **positive** signal that the overworld is being rendered.
Three candidates were measured in one session. Two failed. The third works, and
is now what the gate uses — but *not* by hardcoding the values it found.

**Conclusion: the screen-state globals are a usable per-screen signature, and
the gate learns the overworld's signature at runtime rather than comparing
against constants.**

## Three things worth carrying away

1. **"Not an enum" is not the same as "not usable."** These globals had been
   written off as a dead end, with a note saying *do not re-investigate*. What
   the original work actually established was that the values are not an
   enumeration — which is true, they read as a bitfield. That is a much weaker
   result than "they cannot tell screens apart", and the stronger reading is
   what kept this unsolved. **Re-read old dead-end notes for what they proved,
   not for their headline.**
2. **Measured values are evidence, not constants.** The signatures below are
   recorded so the reasoning can be checked, and nothing in the framework
   compares against them. Hardcoding `0x0FFFEFDF` would mean that the first HUD
   state nobody happened to visit stops all drawing, silently.
3. **A heuristic can be used to *learn* a fact instead of *being* the fact.**
   Ship movement is unreliable as "is the overworld on screen" but completely
   reliable as "the overworld is on screen *right now*". Using it only at the
   moment it is trustworthy converts a permanent approximation into a one-time
   calibration.

---

## What was measured

A temporary probe (`src/core/screenprobe.h`, deleted once it had answered)
sampled three candidate signals every frame and logged a line twice a second.
It was armed by dropping a marker file — `PEMF\screenprobe.on` — next to the
executable, the same opt-in mechanism the data-file probe uses, because it wrote
a canary into a game global and that is not something to leave running.

The session sailed, stopped at sea, entered a town, opened Load/Save, and fought
a battle. 142 samples.

### Candidate 1 — the projection matrix ❌

Read off the device inside the frame with `GetTransform(D3DTS_PROJECTION)`. The
widescreen work had established that world cameras are locked to 4:3 while the
UI camera is a distinct perspective, so the two should be separable:

| Camera | `_11` | `_22` | `_33` | `_43` | |
|---|---|---|---|---|---|
| UI | 2.0000 | 2.6667 | 2.5000 | −1440.0 | far = 1.667 × near |
| World | 3.0178 | 4.0237 | 1.0001 | −10.001 | far ≫ near |

**The rule is right and the sample point is wrong.** At `BeginScene` — which is
where world-anchored text *must* be issued, because it builds scene-graph nodes
the render walk then draws — the transform is still whatever the previous pass
left behind, which is the UI camera. 140 of 142 samples read `_33 = 2.5`
regardless of what was on screen. Only two caught a world camera.

Worth knowing if this is ever revisited: `_33` is the discriminator, not the
aspect ratio. Both cameras have a horizontal-to-vertical ratio of 1.3333, so
aspect separates nothing.

### Candidate 2 — a canary on the world-text size global ❌

`0x0085A11C` is recomputed by the sailing render every world frame — that is why
`game.h` has to set it immediately before each world-text call. The idea was to
write a sentinel at the frame boundary and see whether anything overwrote it.

It reads **100 on every screen**, including the depth-1 main menu. Something
ubiquitous writes it, not the sailing render. Dead.

### Candidate 3 — the screen-state pair ✅

`ScreenId` (`0x007263BC`) and `ScreenDepth` (`0x00726A84`), taken **together**:

| Screen | `ScreenId` | `ScreenDepth` |
|---|---|---|
| Sailing / overworld | `0x0FFFEFDF`, `0x0FFFFFDF` | 3 |
| Town | `0x0FFFEFFA`, `0x0FFFFFFA` | 3 |
| Load / Save | `0x0FFBE770`, `0x0FFBE750` | 4 |
| Battle | `0x8FFFEFFF`, `0x8FFFFFFF` | 4–5 |
| Main menu | `0x0FFFEFF0`, `0x0FFFFFF0` | 1 |

Clean separation. Two of these were confirmed independently rather than by
eyeballing the timeline: Load/Save by the `session: SAVE detected` line landing
exactly on `0x0FFBE750`, and battle by the render-pass count switching 5 → 1
while the overworld position stayed frozen.

The overworld uses two variants because one bit of the id flickers. Depth alone
is not enough — town and sailing share depth 3.

## What was built

The values above are **not** in the code. `triggers::WorldOnScreen()`
calibrates itself:

> A ship whose position changed on **this very tick** is unambiguously out on
> the overworld, whatever the numbers happen to be — so that is when a signature
> is learned. Afterwards the overworld is on screen whenever the live signature
> matches a learned one.

Motion is used to *learn* the answer and never to *be* it. This fixes both
directions of the original bug at once: a menu never matches, so nothing leaks
and there is no tail to wait out; and a becalmed ship still matches, so notices
no longer drop out when you come to a stop.

It fails in the safe direction. An unrecognised screen draws nothing until the
ship moves and teaches us its signature, so the worst case is a missing notice
rather than one painted across the save screen. If the overworld ever produces
more than four signatures the log says so loudly, because a gate that goes quiet
is the failure this project likes least.

**Battle is deliberately not learned.** It has its own ship array and the
overworld position is frozen throughout, so a notice anchored to a map position
was hanging at a stale place. Excluding it is a fix, not a regression — but it
is a behaviour change from before.

## One more thing the same session fixed

A notice's life was measured in wall-clock time, so it kept running down while
nobody could see it: open a menu with a notice up, stay a moment, and it had
expired by the time you came back. Glancing at the map cost you a lookout's
call.

The clock is now **held** whenever the overworld is off screen. Both timestamps
move forward by exactly the time that passed, which leaves the *remaining* time
untouched, so a notice resumes with what it had left rather than restarting or
vanishing.
