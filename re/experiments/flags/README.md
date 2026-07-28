# Flags, emblems, and flying false colours

## The short version

The question was how to let a player **add** flags and sail emblems rather than
replace the ones the game ships. The answer turned out to be that **the game
already does this**, without a mod, and has since 2004.

`custom\flag_*.dds` and `custom\ship_sail_emblem_lrg_*.dds` are enumerated with
`FindFirstFileA` into a growable array; the picker in **Options → Change Sails
and Flags** is a wrapping carousel over however many were found, and the
selection is stored **by name** in `Config.ini`. Drop eleven flags in and you
get eleven. Verified in game: the folder was given four new flags and two test
copies, and the game counted all of them.

So the honest answer to "can PEMF let us add flags" is: **you already can, and
PEMF is not needed for it.** What PEMF is needed for is making the flag *mean*
something, which is the rest of this document.

## Four things worth carrying away

1. **"Historically you had to replace these" was wrong, and checking cost one
   `FindFirstFile` call site.** The belief is real and widespread, but it
   applies to the five *nation* flags, not to the player's own. Before building
   a feature to work around a limit, confirm the limit exists.
2. **Engine objects are REFCOUNTED, and holding one means taking a reference.**
   Getting this wrong crashed the game — see below. This is now a layer rule.
3. **The flag the player flies and the nationality the game reasons about are
   unrelated.** Two separate systems that never touch. Which means the game has
   no concept of false colours at all, so PEMF owns the whole mechanic rather
   than having to stay in step with engine behaviour.
4. **A measurement that moves when *you* move is not a measurement.** The first
   pursuit metric reported eight vessels "closing" in one tick, each by an
   identical 802 units. That was the player sailing.

---

## What the game already has

### Enumeration is unbounded

`FUN_004B00E0(filter, container)`:

- splits the filter on `;` — `flag_*.dds;flag_*.tga`
- scans **two** directories: the game's `custom\`, and
  `My Documents\My Games\Sid Meier's Pirates!\Custom\`
- `FindFirstFileA` / `FindNextFileA`, appending each match, calling
  `resize(n)` (`alloc(n*4 + 4)`, arbitrary size) whenever the array is full
- de-duplicates, and returns the count

| Global | Holds |
|---|---|
| `0x008C9560` | number of custom **flags** found |
| `0x008C9564` | number of custom **sails** found |

Both read `0` until **Change Sails and Flags** is first opened — the scan is
lazy, not a startup cost.

### The picker is a carousel, not a fixed list

`FUN_004B7C70(kind, sailCount, sailList, flagCount, flagList)`, reached from the
Options menu (case `0x212` in `FUN_004B4760`). It draws **three** thumbnails
(`while (i < 0xc)` stepping 4) and indexes them `1 % count`, so it wraps around
the whole list however long it is. Three visible slots, unlimited entries.

Selection persists as `CustomFlag =` / `CustomSail =` in `Config.ini` — **by
name**, so adding files never invalidates a saved choice.

### Nation flags are a different, fixed thing

`FUN_0046BAA0` loads exactly five flag meshes and clones them into live nodes:

| Prototype | Live node | Nation |
|---|---|---|
| `0x00860B40` | `0x00860B54` | Spanish (0) |
| `0x00860B44` | `0x00860B58` | English (1) |
| `0x00860B48` | `0x00860B5C` | French (2) |
| `0x00860B4C` | `0x00860B60` | Dutch (3) |
| `0x00860B50` | `0x00860B64` | Pirate (4) |

Five slots, hardcoded — the same shape as the cargo array. Their textures are
`custom\flag_spa.dds` and friends, which is why retexturing a nation means
replacing a file, and why a `custom\` folder missing the defaults produces the
[well-known pink flags](https://steamcommunity.com/app/3920/discussions/0/3276942370888450991/).

**Custom nation art: yes** (retexture the live node at runtime, additive and
reversible). **A sixth nation: no.**

---

## The player's flag

`FUN_004AF760` re-applies four texture pointers to matching scene nodes, every
time round, comparing before it applies:

| Global | Applied to |
|---|---|
| `0x008E8FB0` | `ship_playercolor*` |
| `0x008E8FB4` | `flag*` — **the player's flag** |
| `0x008E8FB8` | `ship_sail_emblem_lrg*` |
| `0x008E8FBC` | `ship_sail_emblem_sml*` |

Written by the picker (`0x4B888E`, `0x4B88B5`, `0x4B89CF`) and by config load
(`0x4293FB`).

**Verified in game:** writing `0x008E8FB4` changes the flag on the mast, and the
engine keeps it there. Five swaps and a restore in one session, no instability,
game ran on to a clean save. The player's flag is a single pointer.

### The dead end that had to be measured

The overworld ship array is at `0x008142F8`, stride `0x45C`, and the player is
entry 0 — `PlayerX` (`0x00814304`) and `PlayerY` (`0x00814308`) land exactly on
`+0x0C` and `+0x10`, which is how the array was found. `+0x04` is a nationality
field, read by `"She's flying @NATIONALITY colors."` (`0x0046BA80`).

That looked like the false-colours lever. **It is not**, and two measurements say
so rather than one:

- cycling it through all five nations changed nothing on the mast;
- it reads `0` (Spanish) on a career started under the **English** flag, so it
  does not even track the nation the player sails for.

Kept in `game.h` because it is a real finding about AI vessels. It is simply not
the player's flag.

---

## The crash, and the rule it produced

The first working probe captured texture pointers as the player browsed the
picker, then wrote them back later. It **crashed the game instantly.**

The cause was in the function we had already read and quoted. The picker
releases a texture as soon as it scrolls away:

```c
refcount = *(dword*)((char*)obj + 4);
if (--refcount == 0)
    (*(void(**)(int))*(void**)obj)(1);   // vtable[0] -> destructor
```

Every captured pointer was destroyed before it was ever used. Holding a pointer
to a refcounted object means **taking a reference**, and the engine's idiom for
that is on the next line again — `obj[1] += 1`.

The fix takes a reference on capture, refuses to record anything whose vtable
does not read back sanely, and re-checks before use. PEMF never releases these:
a handful of flag textures held for the life of the process is the right price
for pointers that cannot dangle.

**Rule, now in `DEVELOPER.md`: a pointer to an engine object kept beyond the
call that produced it must be referenced, or it is a crash waiting to happen.**

---

## Measuring whether the AI cares

The point of false colours is being treated as somebody else, so the experiment
needs to measure AI behaviour — and the first attempt did not.

It tracked the **distance** between the player and each vessel and called a
shrinking gap "closing". One session reported eight vessels converging in the
same tick, each by an identical 802 units: that was the player sailing, not a
hunt. It also read "closing" just as often under true colours as under a false
flag, so it could not have answered anything.

The replacement measures each vessel's **own displacement projected onto the
direction to the player** — positive means it chose to come at us, and player
movement cancels out entirely. It also ignores anything beyond 30,000 units,
because the map is ~422,000 units across and the first session was dominated by
vessels 400,000 away whose behaviour meant nothing.

It works:

```
falsecolours: vessel 20 flies French  dist 17849  moved 1494  ** PURSUING **   (we fly Spanish)
```

---

## Where this leaves the feature

**Presentation is solved.** The flag is a pointer, the engine maintains it, and
the swap is verified in game.

**The mechanic is entirely PEMF's to build**, because the flag feeds nothing —
it is a texture and the game reasons about nationality somewhere else that does
not move with it. Nothing to fight, nothing to keep in step.

### Loading a flag BY NAME — solved

Capturing pointers worked but could only ever fly "texture #3" without knowing
which flag that was. The recipe for doing it properly was in the **config-load
path** at `0x004293D7` — the code that turns `CustomFlag = <name>` in
`Config.ini` into the texture on the mast:

```asm
mov  esi, [0x726a8c]     ; the name (engine string: length in the dword at -4)
call 0x4f4ed0            ; AssetExists()  -> al
mov  esi, [0x726a8c]
xor  eax, eax            ; default format
call 0x500850            ; LoadTexture()  -> texture*
...                      ; release outgoing, store, then: inc [esi+4]  = AddRef
```

| Address | Signature |
|---|---|
| `0x004F4ED0` | `char AssetExists()` — `esi` = name |
| `0x00500850` | `void* LoadTexture()` — `esi` = name, `eax` = format struct or 0 |

Both take the name in **ESI** and end in a plain `ret`, so the shims only place
the register. With `eax = 0` the loader requires the name to end `.dds` and
picks the format itself. **A bare filename is enough** — `flag_spa.dds`, exactly
as the enumerator reports it. Verified in game: all eleven flags load by name,
zero refusals.

That same path independently confirmed the refcount rules learned by crashing,
which is why it is copied wholesale rather than paraphrased.

### Per-career colours — solved, and it exposed two other bugs

A disguise belongs to a save, not to `Config.ini`, so the sidecar carries `flag`
and `trueFlag`. Getting that to behave correctly turned out to require fixing
career and save/load detection from the ground up — including two pre-existing
bugs that had nothing to do with flags. That is its own write-up:
[`../career_state/`](../career_state/README.md).

Two design points worth keeping:

- **`trueFlag` is recorded once and never overwritten while disguised.**
  Recording it again mid-disguise would make the disguise permanent — the one
  bug a player could never undo, so it is designed out rather than tested for.
- **The sidecar is player-editable text**, and its value is handed to the
  engine's asset loader, so it is validated as a filename rather than trusted.

### PEMF enumerates flags itself

The game's scan only runs when the picker screen is first opened, so depending
on it would mean telling players to visit a menu before the framework works.
PEMF does its own `FindFirstFileA` over the same two folders with the same
pattern, de-duplicated the same way, at startup. No engine call involved.

---

## Where this leaves the feature

**Presentation is solved.** The flag is a texture pointer, loadable by name, the
engine maintains it, and it persists per career.

**The mechanic is entirely PEMF's to build**, because the flag feeds nothing.

Still open, in order:

1. **Suspicion** — the PEMF-owned system that gives the flag stakes: rising with
   proximity to warships and ports, with time under false colours, and with
   infamy; falling with distance and time. Thresholds drive hails, challenges,
   and being unmasked.
2. **The AI half.** What the game actually uses to decide hostility is not yet
   located; the leads followed so far (`International Relations`, the war and
   Letter-of-Marque strings) all landed in news-ticker and pedia formatters
   rather than live state.
3. **The player's chosen faction.** Not located either. The nation-select screen
   art (`intro_s3_e/d/f/s.dds`) turns out to be intro cinematics, and the
   ship-record nationality is measured not to be it. The starting port proves
   the game tracks it somewhere. Needed before a new career can start on its
   faction's colours, and before Suspicion can know who you are impersonating
   *relative to*.
4. **Lower-and-raise animation** — flags attach via `bone_flag_*_pivot` and are
   ordinary named scene nodes, so driving the node transform from the frame hook
   is the plausible route. Unproven.

**Worth knowing about the "wrong" starting flag:** a custom flag in `Config.ini`
overrides the faction flag on every career — that is the game's own behaviour,
not a PEMF effect. `FUN_004AF760` applies it unconditionally whenever one is
set. A career only shows its faction's flag when no custom flag is selected.
Making new careers start on their faction's colours would be PEMF improving on
the game, and needs (3).
