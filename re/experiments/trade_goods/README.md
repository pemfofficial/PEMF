# Can a trade good be *added*?

## The short version

The game has **7 cargo types**, hardwired. Could we add an 8th? Two walls, and
neither one ends up mattering.

**Wall 1 — the hold.** The ship's cargo is a fixed block of 7 numbers in
memory. The number immediately after it is already used for something else, so
there is no room to grow. 82 places in the game's code read that block, many
naming individual goods outright.

**Wall 2 — the names.** Good names (Gold, Food, Sugar…) come from `text.ini`,
which is sealed inside `Pak1.FPK`. **The game never looks on disk for it**
(proven below), so a modified copy cannot be slipped in without repacking a game
file — which this project does not do.

**Why neither matters.** A PEMF trade good does not need the game's 7 slots or
its name list. It lives in *our* memory, its name is a string in *our* JSON, and
it is drawn with the *same* routines the game uses — so it still looks like part
of the game. And the engine's own trade screen loops over items 0-5 and stops,
so it could never have shown an 8th good anyway. Winning either wall would have
changed nothing.

**Conclusion: build trade goods in PEMF, with our own UI. Do not repack
anything.**

## Two things worth carrying away

1. **`@ITEM` is not bounds checked** — asking for an item index past the end of
   the list access-violates. Never emit one; bounds-check against the live list
   length, not a constant.
2. **Loose-file override works for some files and not others**, depending on
   which part of the game reads them. `landscape.ini` and
   `advancedlighting.ini` are read from disk in preference to their packed
   copies — so terrain, foliage and shadow settings can be changed by dropping
   an edited file in, no repacking. `text.ini` is not. **Check, do not assume**;
   the probe below answers it in one restart.

---

## The detail

## Step 1 — does the probe itself work?

Load a career and press **Ctrl+Shift+5**. In `pemf.log`:

```
itemprobe: reading @ITEM for indices 0..6
itemprobe: [0] Gold
itemprobe: [1] Food
...
itemprobe: [6] Cannon
```

**If these are not the seven known names, stop** — the probe is wrong and
nothing it says about index 7 means anything. Everything below assumes this step
passed.

## Step 2 — what does index 7 do *before* any change?

Press **Ctrl+Shift+6**. This asks for an index past the end of the stock list,
which is why it is on its own key: the engine's lookup may not be bounds
checked. Expect one of:

| Result | Means |
|---|---|
| `itemprobe: [7] (empty)` | the lookup is bounded and fails softly |
| `itemprobe: [7] <garbage>` | it reads past the end of the list |
| `itemprobe: [7] EXCEPTION` | not bounds checked at all |

This is the **control**. Without it, a name appearing in step 4 proves nothing —
it could have been reading adjacent memory all along.

### Result, 2026-07-26

```
itemprobe: [0] Gold  [1] Food  [2] Luxuries  [3] Goods
           [4] Spice [5] Sugar [6] Cannon              <- probe is sound
itemprobe: [7] EXCEPTION 0xC0000005 -- the lookup is NOT bounds checked
```

**`@ITEM` past the end of the list access-violates.** Our SEH caught it and the
game carried on, twice, but the constraint is real and general:

> **Never emit `@ITEM` with an index the list does not contain.** Any future
> `{item}` placeholder must be bounds checked against the live list length, not
> against a constant. This is the same class of hazard as the token argument
> counts — the engine does not check, so we must.

It is *consistent with* the list being sized from the file: with seven values,
index 7 reads one past the parsed array and dereferences whatever follows. If
the file supplied eight, index 7 would be inside it. So the question is still
open, and step 4 still decides it — but the control now says clearly that a
crash means "not loaded", not "loaded and empty".

## Step 3 — where does the game look for `text.ini`?

**Arm it before the game starts.** Create `PEMF\fileprobe.on` next to the exe
and the probe is live from the first file the game opens. The hotkey toggle
exists too, but it is nearly useless here: the text and asset systems load
during startup, long before a key can be pressed. The first attempt at this
found nothing but `Config.ini` for exactly that reason.

The log records every `.ini` / `.txt` / `.csv` / `.fpk` — and anything named
"text", whatever its extension — that the game opens **and every one it fails
to open**:

```
fileprobe: MISS  C:\...\Sid Meier's Pirates\text.ini
fileprobe: OPEN  C:\...\Assets\Pak1.FPK
```

**The misses are the point.** A probe for a loose file that is not there is the
evidence that dropping one in would be picked up. The game already overrides
packed art with loose files from `custom\`, and the exe carries a `custom` path
string, so a loose override path exists — this identifies exactly which one.

Delete `PEMF\fileprobe.on` to stop logging.

If nothing appears even armed at startup, the hooks are installing after the
text system has already read its file — on the DRM-packed Steam build the
by-address hooks only take once the unpacker has run. Try the GOG copy, where
the hooks install by name immediately.

### Result, 2026-07-26 — answered

Armed at startup, the log shows the whole picture:

```
fileprobe: OPEN  ASSETS\LANG0.FPK   ... through ...  ASSETS\PAK8.FPK
fileprobe: MISS  Data\AdvancedLighting.ini
fileprobe: MISS  assets\data\Landscape.ini
fileprobe: OPEN  ...\My Games\Sid Meier's Pirates!\Config.ini, KeyMap.ini
```

**There is no loose probe for `text.ini`. Not a missing one — none at all.**

That absence is real evidence rather than a gap in the instrumentation, because
two *other* `.ini` files inside the same archive **are** probed on disk first
and missed. `landscape.ini` and `advancedlighting.ini` both live in
`Pak1.FPK`, and the game still looks for them on disk before falling back. So
the probe catches misses, and the text system simply never asks the disk.

The ten `.FPK` archives are opened once at startup and read from thereafter, so
nothing inside them appears as a separate file open.

**Verdict: `[ITEM]` cannot be extended with a loose file.** Doing it would mean
repacking `Pak1.FPK` — modifying a game file, which this project does not do —
or patching the parsed table in memory after load.

### …and it does not matter

The question was whether a PEMF-invented good could carry a name the engine
renders. It can, just not through `@ITEM`: a PEMF good's name is a string in
its own JSON, drawn through the same text and drawing routines everything else
already uses, so it looks native because it is rendered natively.

`@ITEM` would only have been needed for *engine* code to name our good — and
that code iterates items 0-5 and cannot show an eighth good regardless. So the
archive being closed to us costs nothing the plan actually wanted.

**Do not repack `Pak1.FPK` for this.** It touches a game file, it is not needed,
and `@ITEM` past the end of the list access-violates.

---

## A useful side finding: loose override *does* work — selectively

`Data\AdvancedLighting.ini` and `assets\data\Landscape.ini` are looked for on
disk **before** the archive copy is used. Creating either path loose would
override the packed version with no repacking at all.

So loose-file override is **per subsystem**, not global: some readers try disk
first, the text system does not. Worth remembering for anything that turns out
to live in one of the readers that does.

---

## Restoring

Delete `PEMF\fileprobe.on` to stop the logging. Nothing else was changed —
every probe here only reads.
