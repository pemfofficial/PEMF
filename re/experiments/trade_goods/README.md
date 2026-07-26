# Can a trade good be *added*?

The engine's cargo array is fixed at seven slots and cannot grow — measured in
[`docs/GAME_API.md`](../../../docs/GAME_API.md#trade-goods--the-cargo-model-and-what-adding-one-would-cost).
That leaves one question worth answering before designing anything:

> **Is the item list the *file's*, or the *engine's*?**

`@ITEM` is a `__VAR` lookup into the `[ITEM]` group parsed from `text.ini` at
runtime. If an eighth entry loads and renders, then goods PEMF invents can carry
names through the game's own text system, and authored text about them reads
natively. If it does not, they need a text path of their own.

Nothing here modifies the game. The probes read state back and log it.

---

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
