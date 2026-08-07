# PEMF — Player Manual

*Pirates! Expanded Modding Framework*

A deeper Caribbean. Today that means **false colours** you can be hunted for,
**weather** worth steering around, and a narrative engine that turns your
situation into events with real consequences. In time it means named officers
with their own histories and grudges, and a crew that has opinions about how you
captain them.

> **Current state: early development.**
> Three mechanics are real and playable: **False Flag**, **Weather**, and the
> **event engine** — custom events render in the game's own style, offer real
> choices, and change your game state. **Very little event content exists yet.**
> Sections marked *Planned* — officers, crew, mutiny — are design intent, not
> features you can use today, and nothing in them is built.
> Everything under [What Works Today](#what-works-today) is real and testable.

---

## Requirements

| | |
|---|---|
| Game | **Sid Meier's Pirates! (2004)** — **GOG and Steam both work** |
| Executable | The **stock** `Pirates!.exe` — see the warning below |
| OS | Windows |

### Both store versions are supported

The **GOG** executable is the one PEMF was mapped against.

The **Steam** executable is DRM-packed — its code is compressed on disk and only
becomes real once the game has started unpacking itself. PEMF handles this: it
waits for the unpack, hooks by absolute address rather than by name, and
signature-checks every engine function it calls at runtime. If anything does not
match, it says so in the log and disables the affected feature instead of
guessing.

### Important: stock executable only

PEMF refuses to run against a modified `Pirates!.exe`. In particular the
**Challenge Pack** base executable is a completely different build, and every
internal address differs.

If you have used other exe-patching mods, restore the original first. The mod
checks on startup — if it does not match, it disables itself rather than risk
corrupting your game.

---

## Installing

Extract the archive into your game folder, next to `Pirates!.exe`:

| Store | Typical path |
|---|---|
| GOG | `C:\GOG Games\Sid Meier's Pirates\` |
| Steam | `C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Pirates!\` |

In Steam, **right-click the game → Manage → Browse local files** gets you there.
You should end up with:

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

## Playing False Flag

The first complete new mechanic in PEMF. Fly another nation's colours, and find
out what it costs when they stop believing you.

### The keys

| Key | Does |
|---|---|
| **Ctrl+Shift+8** | Run up the next flag |
| **Ctrl+Shift+9** | Run your own colours back up |
| **Ctrl+Shift+0** | Write a report to `pemf.log` |

Every `flag_*.dds` in your `custom\` folder is available, including any you have
added yourself. The game has always supported adding flags — PEMF just lets you
change them at sea instead of in a menu.

### How it works

Raise a nation's flag that is not your own and a meter appears in the top right:

```
                                        Spanish colours
                                        ======......  <
```

It fills while their ships and ports can **see** you, and empties in open water.
The `<` means someone is watching right now.

**Getting close is the point, and getting close is the risk.** A disguise is
worth wearing because it lets you be somewhere you should not be — and being
there is exactly what gives you away.

Along the way they will signal for your colours, then study your rigging, then
come about. If the meter fills:

- your **reputation with that nation drops** — real, and the game's own number
- their **ports close** to you, and above a certain point there is a price on
  your head and an amnesty to buy back
- your **colours are struck** — the ruse is over
- **pirate-hunters sail** from their nearest port

### Getting away

Distance and time. The further you get from where it happened, the faster it
cools — the people over there have not heard yet.

Hunters break off if you get clear enough, or after long enough. **How many come
and how strong they are depends on how much that nation already hates you**, so a
first offence is one ship and a long history is a squadron.

### Tuning it

Everything is in **`PEMF\suspicion.ini`** — how fast suspicion rises and falls,
the distances, the thresholds, what being caught costs, and how doggedly hunters
pursue. The file is commented, and reinstalling PEMF will not overwrite one you
have edited.

If it feels too fast, the three `rise*` values are the ones to lower.

---

## Weather

The Caribbean gets its weather back. Storms are drawn far larger and heavier
than the game ships them, they **drift across the map** rather than sitting
still, and they have a sound of their own that comes up as you close on one and
fades as you leave it behind.

It is on by default. Turn it off with `enabled = 0` in `PEMF\storms.ini`.

### It can cost you

Optionally, sailing inside real weather can put a little cargo over the side.
This is **off by default** — set `cargoLoss = 1` in `storms.ini` if you want
storms to be something you avoid rather than something you admire.

Worth being plain about: in the stock game a storm appears to be **purely
decorative**, as far as we have been able to find. Cargo loss is PEMF's
addition, not a hidden vanilla rule we switched on.

### Tuning it

Everything is in **`PEMF\storms.ini`**, which is commented throughout. The two
you will reach for first:

| Setting | What it does |
|---|---|
| `stormScale` | How big a storm is drawn. Vanilla is 80; PEMF ships 300. |
| `stormHeight` | How high it sits. **Raise this when you raise `stormScale`** — a large cloud sitting low swallows the camera and reads as fog. |

Two things the file will stop you doing, both for good reasons: **storms cannot
fade** (the value that looks like opacity is a timer — an engine limitation, not
an oversight), and you cannot have **more than three** weather systems at once
(the game keeps only three slots, and asking for a fourth writes past the end of
its own array). Your edits survive reinstalling PEMF.

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
you in a town, a menu, a battle, or while paused. They can fire after a stretch
of sailing, on approaching a port, or when something about your ship crosses a
line — your crew thinning out, your purse running low, morale souring, a voyage
dragging on.

A notice can also **hang over your ship and follow it** as you sail, the way the
game labels other vessels, and it can name the port you are approaching:
*"Land ho! Nevis off the bow!"*

Notices stay where they belong. They appear only in the sailing view — never
over a town, a menu or the Load/Save screen — and if you open one of those with
a notice up, **it waits for you**: its time is paused while it is out of sight,
so you come back to it rather than finding it gone. Coming to a stop at sea does
not cut one short either.

### Your saves are respected

The mod keeps its own state — which events have fired, and in time your officers
— in a file beside each save (`<name>.pemf`). Load an older save and
you get that save's state back, exactly as you left it. Nothing carries across
into a career it does not belong to.

### WASD steering

**If WASD is not working, settle which build you are on first.** The top line of
`pemf.log` says. A line like

```
=== PEMF loaded === pid=...
```

with **no version number** is older than 0.2.2, and on those builds WASD never
installed on GOG at all — the layout was written to a folder that does not exist
on a GOG install, so no `KeyMap.ini` changed and no `.pemf-backup` appeared.
Update and it installs on the next launch.

A current build says

```
=== PEMF 0.2.3.0 loaded === built ...
```

and then prints exactly which folder it picked and what it did:

```
profile: candidate "Sid Meier's Pirates" (Config.ini present)  <- matches the install folder
profile: using ...\My Games\Sid Meier's Pirates
keymap: WASD installed -> ...\KeyMap.ini (original saved as ...\KeyMap.ini.pemf-backup)
```

If there are no `profile:` or `keymap:` lines at all, PEMF is not the build you
think it is.

**Getting the layout back after you have changed it.** PEMF writes your keymap
once and then leaves it alone, so your own rebinds are safe. To have the layout
put back, delete the `PEMF-WASD-INSTALLED` line from

```
Documents\My Games\Sid Meier's Pirates[!]\KeyMap.ini
```

⚠️ **Not** the copy in your game folder under `PEMF\KeyMap_WASD.ini` — that one
is only the template PEMF copies from, and editing it changes nothing about the
keymap you are playing with.

Installed automatically the first time you run the game. Sailing, ship battles,
land battles, duels, sneaking — all of it.

Two keys had to move to free up the letters:

| | Was | Now |
|---|---|---|
| Attack ship | `a` | **`f`** — the same key as Fire in a sea battle |
| Quick save | `S` | **F5** (quick load is **F9**) |

⚠️ **Attack was on Space in 0.2.4 and 0.2.5, and that was a mistake.** Space is
the game's own "enter the nearest port" key, so attacking a ship within reach of
a harbour docked you instead. It is `f` from 0.2.6 on, and your `KeyMap.ini` is
rewritten once so the change actually reaches you — anything you rebind after
that is yours and stays, and your pre-PEMF file is still beside it as
`KeyMap.ini.pemf-backup`.

**Also fixed in 0.2.6:** while sailing, `a` and `s` loaded and saved the game as
well as steering. The sailing view reads those two keys straight from the
keyboard rather than from `KeyMap.ini`, so rebinding them in the file never had
any effect — steering left would load your last save and drop you into a port,
over and over. They now steer and nothing else. `L` still quick-loads, exactly as
it does in the stock game.

Your original bindings are kept as `KeyMap.ini.pemf-backup`, next to the
`KeyMap.ini` it replaced. That lives in your Documents folder, under
`My Games` — but **the folder name depends on which copy of the game you own**:
the Steam build uses `Sid Meier's Pirates!` and the GOG build
`Sid Meier's Pirates`, without the exclamation mark. PEMF finds it for you and
writes the folder it chose into `pemf.log`, so if the keys did not change, that
line says where it looked.

PEMF writes the layout **once** and never touches the file again, so anything
you rebind afterwards stays put. If PEMF cannot back your file up, it does not
replace it — your bindings are never overwritten with no way back.

### Developer tools

Everything above is live for everyone. The probes PEMF uses to read engine
memory are not — they are off unless you ask for them, so nobody hits one by
accident reaching for the flag keys.

To turn them on, put an **empty file named `dev.on`** in your `PEMF\` folder.
Then:

| Keys | What it does |
|---|---|
| **Ctrl+Shift+1 / 2** | Fire the first or second loaded event |
| **Ctrl+Shift+3 / 4** | A test notice, at the top of the screen or over your ship |
| **Ctrl+Shift+N** | Report nation relations and your standing with each crown |
| **Ctrl+Shift+U** | Cycle your reputation with the nearest port's nation |
| **Ctrl+Shift+P** | Compare a ship PEMF built against one the game made |

A second file, `shipyard.on`, enables keys that **build ships out of nothing**.
Those create state that persists into your save and cannot be undone by pressing
the key again — use a career you do not mind losing.

You must be **in an actual game** (sailing, not at the main menu) — these read
live state.

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

### Flags — something you can do today, with no mod at all

Worth knowing, because the opposite is widely believed: **Pirates! already lets
you add as many flags and sail emblems as you like.** You do not need PEMF for
it, and you should not replace anything.

Put your own `.dds` files in the game's `custom\` folder, named
`flag_<whatever>.dds` (256×256) or `ship_sail_emblem_lrg_<whatever>.dds`
(512×512), then pick them in **Options → Change Sails and Flags**. The game
scans the folder and offers everything it finds — eleven flags, thirty, however
many you put there. Your choice is remembered by name, so adding more later
never disturbs it.

Two cautions. **Keep the original files.** The five nation flags
(`flag_spa`, `flag_eng`, `flag_fre`, `flag_dut`, `flag_pir`) double as the flags
every other ship in the world flies, and a `custom\` folder missing them shows
bright pink sails and flags — the usual symptom of installing a flag pack that
overwrote them. And that is also why *changing* a nation's flag still means
replacing a file, while adding your own never does.

What PEMF will add later is not more flags but a reason to change them: flying
another nation's colours to get close, and the risk of being found out.

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
