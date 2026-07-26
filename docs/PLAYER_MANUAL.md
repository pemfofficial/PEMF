# PEMF — Player Manual

*Pirates! Expanded Modding Framework*

A deeper Caribbean. Named officers with their own histories and grudges, a crew
that has opinions about how you captain them, and a narrative engine that turns
those opinions into events with real consequences.

> **Current state: early development.**
> The framework is working — custom events render in the game's own style, offer
> real choices, and change your game state. **Very little content exists yet.**
> Sections marked *Planned* are design intent, not features you can use today.
> Everything under [What Works Today](#what-works-today) is real and testable.

---

## Requirements

| | |
|---|---|
| Game | **Sid Meier's Pirates! (2004)**, GOG edition |
| Executable | The **stock** `Pirates!.exe` — see the warning below |
| OS | Windows |

### Important: stock executable only

This mod will refuse to run against a modified `Pirates!.exe`. In particular the
**Challenge Pack** base executable is a completely different build of the game,
and every internal address the mod relies on differs.

If you have used other exe-patching mods, restore the original before installing.
GOG installs keep a copy, and the mod checks on startup — if it does not match,
it disables itself rather than risk corrupting your game.

---

## Installing

Extract the archive into your game folder, next to `Pirates!.exe` — typically
`C:\GOG Games\Sid Meier's Pirates\`. You should end up with:

```
Pirates!.exe          (already there)
version.dll           <- new, the loader
pemf_core.dll         <- new, PEMF itself
PEMF\
    events\
        core_events.json
    docs\
```

Then launch the game normally. That is the whole install. No files are modified,
nothing is patched, and your saves are untouched.

### Uninstalling

Delete `version.dll`, `pemf_core.dll` and the `PEMF` folder. The game returns to
stock immediately. Nothing else is left behind except `pemf.log` and any `.pemf`
files beside your saves, which are also safe to delete.

### Checking it loaded

The mod writes `pemf.log` in the game folder. A healthy start looks like:

```
=== PEMF loaded ===
target verified: offsets match the expected build
content: scanning ...\PEMF\events
content:   core_events.json             3 event(s)
content: 3 event(s) from 1 file(s)
game thread latched: 15180
safe point reached (main loop PeekMessageA) -- deferred dispatch live
```

---

## What Works Today

Events appear as cards in the game's own style, using its fonts, backgrounds and
layout — they are meant to be indistinguishable from vanilla Pirates! events.

Events come in two kinds.

**Choices** stop you and ask something. They:

- **Read your situation.** Crew size, morale, undivided plunder and time at sea
  appear naturally in the prose, in the game's voice.
- **Offer real choices.** Selectable options on the card. Three have been tested;
  the format allows up to six, though more than three is so far untried.
- **Change your game.** Your choice can cost you plunder or change crew numbers.
- **Tell you what came of it.** A follow-up reflects what you chose.

**Notices** never interrupt you. They appear as a line of text while you sail —
the same style the game uses for its own `Press 'r' to return to ship.` — and
fade on their own. A lookout sighting a harbour is a notice, not a question.

### Events fire from play

Events happen while you are **sailing the overworld**. They will not interrupt
you in a town, a menu, a battle, or while paused. So far they can fire after a
stretch of sailing, or on approaching a port; more triggers are coming.

### Your saves are respected

The mod keeps its own state — which events have fired, and in time your officers
— in a file beside each save (`<name>.pemf`). Load an older save and
you get that save's state back, exactly as you left it. Nothing carries across
into a career it does not belong to.

### Testing hotkeys

While the trigger system is being built, events are fired by hand. These are
temporary and will disappear once events fire naturally from play.

| Keys | What it does |
|---|---|
| **Ctrl+Shift+1** | Fires the first loaded event |
| **Ctrl+Shift+2** | Fires the second |

You must be **in an actual game** (sailing, not at the main menu) — events read
your live crew state.

---

## Writing Your Own Events

**You can do this now.** Events live in `PEMF\events\`: the prose, the choices
offered, and what each choice does to your game. Edit any `.json` file there — or
drop in your own — restart the game, and they are live. No code, no compiler.

Every `.json` in that folder is loaded, so add-ons sit side by side without
having to merge files.

Everything is checked when the file loads. A bad event is rejected with a precise
reason in `pemf.log` and the rest of the file still works — this has been
tested against deliberately malformed content covering every validation rule.

The validation exists specifically to stop authored text reaching the 2004 engine
in a state it cannot handle. It is thorough, but it is not a formal guarantee: if
you do find content that misbehaves, please report it with your log.

See **[EVENT_AUTHORING.md](EVENT_AUTHORING.md)** for the full format.

---

## Planned Features

Everything below is design intent. None of it is playable yet.

### Officers

A limited roster of **named officers**, each a persistent individual with their
own history, loyalties, ambitions and opinions — of you, of each other, and of
how you run the ship. Officer slots are scarce, so who you keep matters.

The plan includes a roster screen and a crew/officer count indicator.

### Crew

Beyond your officers, the crew are a body with **morale, needs and opinions**.
They notice how long it has been since the plunder was divided, how well they
eat, and how you treat them. Events grow out of that state rather than firing at
random.

### Mutiny and its consequences

An unhappy crew is a real hazard, with outcomes that matter:

- **Splintering** — dissatisfied crew break away and leave.
- **Marooning** — the crew put you ashore at the next port and sail off with your
  ship. You keep your name and your reputation, and very little else.
- **The plank** — the worst case. It ends the career.

### Factions and divisions

Deeper factional politics, and divisions *within* your own crew. Later.

### Custom content

Longer term: new interface elements, new event artwork, and new models.

---

## Troubleshooting

**Nothing happens, and there is no `pemf.log`.**
`version.dll` is missing or in the wrong folder. It must sit beside
`Pirates!.exe`.

**The log says `TARGET MISMATCH`.**
Your `Pirates!.exe` is not the stock GOG build. Restore the original executable.
The mod disabled itself deliberately to avoid corrupting your game.

**`content: no event files found`.**
The `PEMF\events\` folder did not come across. Check the layout above.

**An event of mine did not appear.**
Look for `content: REJECTED` in the log — it names the event and the exact
problem. See [EVENT_AUTHORING.md](EVENT_AUTHORING.md).

**Hotkeys do nothing.**
Be in an actual game rather than the menu, and hold Ctrl and Shift together with
the number key.

**The game closed with no error message.**
Please report it with your `pemf.log` — it is written continuously and
usually shows exactly what the mod was doing at the time.
