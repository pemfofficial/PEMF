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

## Step 4 — put the modified file where step 3 said

`text.ini` here is the stock file with **one line added**: `Value = Rum` as the
eighth `[ITEM]` entry.

Copy it to the path step 3 identified, restart, and press **Ctrl+Shift+6**.

| Result | Verdict |
|---|---|
| `itemprobe: [7] Rum` | **The list length is the file's.** New goods can be named through the game's own text system. |
| same as the step 2 control | the loose file is not being read — try another path from step 3 |
| anything else | the list length is baked in somewhere; new goods need their own text path |

---

## Restoring

Delete the file you copied. Nothing else was touched: the probes only read, and
the game's own `text.ini` inside `Pak1.FPK` is never modified.

## What a positive result does and does not buy

It does **not** create an eighth cargo slot — that array is boxed in, and no
amount of text changes that. What it buys is that a good living in PEMF's own
memory can carry a name the engine will render, so our text about it looks like
the game's own rather than like an overlay.
