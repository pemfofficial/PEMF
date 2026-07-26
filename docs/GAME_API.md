# Game API Reference — Sid Meier's Pirates! (2004)

Everything in this file was recovered by reverse engineering and, unless marked
otherwise, **verified by running it in the game**. It is the authoritative
record of what we can call and what we know about how the engine behaves.

> **Every address here is valid for ONE binary.** See [Target Binary](#target-binary).
> Verify before trusting: `python re/scripts/diff_exe.py`.

---

## Target Binary

| Property | Value |
|---|---|
| File | `Pirates!.exe` (GOG, stock) |
| SHA-256 prefix | `6E88B90E4E2E3024` |
| Size | 3,323,288 bytes |
| Architecture | x86, 32-bit |
| Image base | `0x00400000` |
| ASLR | **Disabled** (`DllCharacteristics = 0`) |
| Linker | MSVC 7.1 (2005-07-06) |
| Engine | Gamebryo 10.1.0.0, NiDX9 renderer |
| D3D | `d3d9.dll`, loaded dynamically (not in the import table) |

Because ASLR is off, **absolute addresses are stable across every launch**. No
pattern scanning is required — a fixed offset map is correct and faster.

### Do not use the Challenge Pack binary

`_modtools\backups\Pirates!.exe.cp-base` is **a different build**, not stock plus
patches: 2,634,387 of 2,879,488 `.text` bytes differ (91%). None of these
addresses apply to it. `game::VerifyTarget()` byte-probes on startup and refuses
to touch memory on a mismatch.

### Build support: GOG vs Steam

| Build | On disk | Analysable statically? | Status |
|---|---|---|---|
| **GOG** (`SHA-256 6E88B90E…`, 3,323,288 B) | plain PE, 4 sections, no ASLR | **Yes** | fully supported — every address here |
| **Steam** (`SHA-256 5342209C…`, 1,189,888 B) | **DRM-packed** | **No** | needs extra work — see below |

The **Steam executable is DRM-wrapped**. Its on-disk form has only two sections; the
`.text` is `~1.1 MB` raw but `~5 MB` virtual with **entropy 8.00** (fully
compressed/encrypted), and its import stub pulls in exactly `LoadLibraryA`,
`GetProcAddress`, `VirtualAlloc`, `VirtualFree` — an unpacker that allocates memory,
decompresses the real code at launch, and rebuilds the import table in memory. The
real game imports (`timeGetTime`, `PeekMessageA`, `CreateFileA`, …) are **not on
disk**.

**What the runtime probe established** (mod deployed to the Steam copy and run):

- **The mod coexists with the DRM.** The `version.dll` proxy loads, the core loads,
  and the game runs normally — *provided the core never faults while inspecting the
  packed image*. Deliberately faulting on encrypted/mid-unpack memory (even with the
  fault caught) disturbs the packer's own exception-based unpacking and corrupts the
  on-demand unpack of later screens (it blanked character creation). The core now
  checks every pointer with `VirtualQuery` before reading — see `PageReadable` — so
  image inspection is fault-free.
- **The underlying build is GOG.** Once the code unpacks, `VerifyTarget`'s byte
  probes at the GOG addresses **match** — so the whole offset map transfers. No dump
  or re-reverse-engineering is needed.
- **The import *directory* is destroyed** — even fully unpacked, walking the PE
  import descriptors yields garbage, so finding an import **by name** (our normal
  `HookIAT`) does not work on Steam.
- **But the IAT *slots* are populated.** The game runs, so `timeGetTime`,
  `PeekMessageA` and friends *are* resolved — the packer fills the actual slots (at
  their GOG addresses, since there is no ASLR) even though it wrecks the name tables.

**So the Steam path is smaller than a dump-and-re-map — and it is now implemented
and verified in-game on both builds:**

1. Detect the packed host and **poll until it has unpacked** (loop until `VerifyTarget`
   passes) — the timing is nondeterministic; the Steam build typically unpacks within
   ~100 ms of the core loading.
2. **Hook the IAT slots by absolute address** (the known GOG slot VAs — see
   `game::addr::Slot*`, e.g. `WINMM!timeGetTime` at `0x006C0430`), retrying until the
   slot holds a real pointer into the expected module. The core tries the name-based
   walk first (GOG, unchanged) and falls back to the by-address patch (Steam).
3. Everything else — offsets, structures, logic — transfers from GOG unchanged.

Verified: on GOG all four hooks install by name instantly; on Steam they install by
address after the short unpack wait, and both reach the safe point and run save/load.
The whole framework runs on both distributions from one codebase. The one caveat to
watch is a packer that re-populates a slot *after* we hook it (not observed in the
retry window); if it ever appears, a periodic re-hook watchdog covers it.

---

## The Message / Event System

The game composes text into a buffer, optionally word-wraps it, then presents it.
All narrative in the game flows through this.

### Format tokens

Text is printf-style with **named tokens** that consume varargs left-to-right,
exactly like `%d`:

| Token | Consumes | Renders as |
|---|---|---|
| `@NUM` | `int` | the number |
| `@HAPPY` | `int` (0–4) | crew mood word — e.g. `DEVOTED` |
| `@NAME` | `char*` | a name |
| `@CITYNAME` | — | current city |
| `@NATIONALITY` | — | nation adjective |
| `@CITYTYPE` | — | town / village / mission |
| `@MONTH` | `int` | month name |
| `@DIFFICULTY` | — | difficulty word |
| `@SHARE` | — | the player's loot percentage |

Example, verified in-game:

```
"Your crew of @NUM stands @HAPPY."  + (40, 4)
  -> "Your crew of 40 stands DEVOTED."
```

### Functions

| Address | Name | Convention | Notes |
|---|---|---|---|
| `0x004F6090` | `AddText` (mode 0) | cdecl, varargs | Narrative prose. 740 call sites. |
| `0x004F60B0` | `AddTextOption` (mode 1) | cdecl, varargs | **Selectable menu option.** 416 call sites. |
| `0x004F60D0` | formatter core | fastcall(`ecx`=fmt) | Both wrappers call this. |
| `0x004879F0` | `WrapText` | **cdecl** | `(width, mode)`. Word-wraps to `width` columns. |
| `0x00410C50` | `ShowMessage` | fastcall + `eax` | Presents; returns result. |
| `0x00430190` | `ShowDialogDirect` | **cdecl, 10 args** | The real renderer. Returns `1` = confirmed. |
| `0x00412F10` | string assign | `eax`=dest, **`ebx`=source** | Used internally by `WrapText`. |

### Critical: the `esi` register

**`esi` must equal `0x008E9F58` (the message object) across the whole
compose → wrap → present sequence.** The game sets it once at the top of the
calling function and holds it there. Setting it per-call and restoring it makes
`WrapText` access-violate.

Every shim in `game.h` sets `esi` itself, which is equivalent and safer.

### Critical: `WrapText` is cdecl, not stdcall

`FUN_004879F0` ends with a plain `ret` at `0x00487CD5`. The game's call sites
show no `add esp, 8` because **MSVC defers the adjustment and folds it into later
`[esp+N]` offsets** — at `0x0046932B` a single `add esp, 0x30` cleans 40 bytes of
renderer args *plus* WrapText's uncleaned 8.

Misreading this as stdcall leaves `esp` 8 bytes low, so `pop esi` takes an
argument and `ret` jumps into it. Symptom: `0xC0000005`.

### Buffers

| Address | What it is |
|---|---|
| `0x008E9F58` | Message object. `esi` must point here. `[obj+0x68]` is a `char*` (length at `ptr-4`). |
| `0x00869B48` | **The message text** — a plain NUL-terminated char buffer. |
| `0x008CACD0` | **NOT an output buffer.** Shared empty-string sentinel; always reads length 0. |

Reset between events with `*(char*)0x00869B48 = 0` — the game's own idiom
(`DAT_00869b48 = 0`) after every card.

---

## Dialog Forms

`ShowMessage`'s `eax` parameter selects the **form**. This is not cosmetic.

| `eax` | Form | Behaviour | Returns |
|---|---|---|---|
| `-1` | Modal message card | **Blocks** until dismissed | `-1` |
| `10` | Polled town menu | **Non-blocking** — renders one frame | `-2` = nothing picked yet |

`eax = 10` expects an outer per-frame loop to keep calling it. Called one-shot it
appears as a **flash** and returns `-2` immediately. It is not a one-shot modal.

Also note: **appending options to an `eax = -1` card does not make them
selectable.** The prose renders, the options are appended to the buffer, and the
card dismisses.

### Modal choice — the working mechanism

**Options are LINES OF THE MESSAGE TEXT, not separate calls.** A choice prompt is
one string: the body, then one line per option, each starting with a single
space and terminated by `\n`. The renderer turns those lines into selectable
rows and **returns the index chosen** (0-based).

Ground truth, decompiled from the landing-party prompt at `0x004692DA`:

```c
FUN_004f6090("Do you wish to form a landing party and go ashore?\n"
             " No, sail away.\n"
             " Yes, we'll anchor here.\n");
FUN_004879f0(0x2c, 0);
iVar23 = FUN_00430190(screenW/4, screenH/8 + 1, &DAT_006fb75c /* "snap" */,
                      0, -1, 0, 0, -1, 0, 0);
if (iVar23 == 1) { ... }        // 1 = the SECOND option ("Yes")
```

`"snap"` is a sound-effect name; an empty string means no sound.

> **`AddText` REPLACES the buffer — it does not append.** The entire prompt, body
> plus every option line, must go in as **one string**, exactly as the game's own
> literal does. Emitting one `AddText` per option leaves only the last one, which
> renders as a bodyless card with a single *Continue* button and returns `-1`.

`game::AskChoice` composes body + option lines into a single buffer, then makes
one `AddText` call — so the body can still carry `@`-tokens while option text
stays literal.

```cpp
int pick = game::AskChoice(body, options, count, a, b);   // -> 0, 1, 2, ...
bool yes = game::AskYesNo(prompt);                        // decline first, accept second
```

> **Do not use `AddTextOption` (mode 1) for this.** That is the persistent
> town-menu system and has no effect on modal cards. A card with no
> leading-space option lines renders a single *Continue* button.

Emit option text literally — never through a `%s` format. The engine formatter
uses `@`-tokens and printf conversions cannot be assumed.

---

## Crew and Morale

### Morale formula

Recovered from `FUN_00404810`, which returns an `int` in **0–4** (this is the
value `@HAPPY` consumes — an index, not a string pointer):

```c
int d = DAT_00869A76 - 4 + DAT_0085A158;      // difficulty inputs
d = (d * d) / 4 - DAT_00869B27 * 4;
d = clamp(d, 1, 999);

int morale = (undividedPlunder + 500)
           / ((stateFlags & 0x80 ? 1 : 20) + crewCount)
           / d;
return clamp(morale, 0, 4);
```

This is the classic Pirates! "gold per man" mechanic.

### State globals

| Address | Type | Meaning |
|---|---|---|
| `0x00869AB0` | `int32` | **Crew count** |
| `0x00869AB4` | `int32` | **Undivided plunder** |
| `0x00869B1A` | `int16` | Months at sea |
| `0x00869B27` | `int8` | Morale calc input |
| `0x00869B34` | `int32` | State flags (bit `0x80` used by morale) |
| `0x00869A76` | `int16` | Morale calc input |
| `0x0085A158` | `int32` | Morale calc input |
| `0x0085A26C` | `int32` | Screen width |
| `0x0085A268` | `int32` | Screen height |

Reading these is safe any time. `crewCount == 0` reliably means "not in a game"
(menu or intro) — the heartbeat uses it that way.

---

## World and Map

### Player position

| Address | Type | Meaning |
|---|---|---|
| `0x00814304` | `int32` | Player X, in milli-units |
| `0x00814308` | `int32` | Player Y, in milli-units |

Divide by 1000 to get city-table coordinates. The game does exactly that at
`0x004691xx` before calling the search below.

### Nearest-city search — `FUN_0045FD40`

Plain `__cdecl`, five arguments. The engine's own port lookup; we call it rather
than reimplementing it, so our distances always match its own.

```c
int FindNearestCity(int x, int y, unsigned typeMask,
                    int maxDist, unsigned excludeFlags);
// -> city index, or -1 if nothing qualifies within maxDist
```

The game calls it as
`FindNearestCity(playerX/1000, playerY/1000, 0xFFFFFFFF, 2000, 0x80000000)`.

It walks two parallel tables:

| Address | Stride | Layout |
|---|---|---|
| `0x0085B170` | 16 bytes | city positions: `x` at +0, `y` at +4 |
| `0x00860B70` | `0x20` | city records: flags at +0, type at +4 |

`typeMask` is tested as `1 << type`; `excludeFlags` is tested against the record
flags. Distance is an **octagonal approximation**, not Euclidean:

```c
d = (min(|dx|,|dy|) + max(|dx|,|dy|) * 2) / 2
```

`game::CityDistance()` reproduces this so reported distances match the engine's.

### City table capacity — how many towns the world can hold

`FindNearestCity`'s own scan loop is the ground truth:

```c
piVar4 = &DAT_0085B170;   // positions, stride 16
puVar3 = &DAT_00860B70;   // records,   stride 0x20
do { /* checks 4 cities per pass */
    puVar3 += 0x20; piVar4 += 0x10; iVar5 += 4;
} while (iVar5 + 2 < 0x80);   // indices 0..127
```

- **The engine walks 128 settlement slots.** That is the ceiling on findable towns.
- **44 (`0x2C`) are the named colonial cities.** The city-capture code gates a
  "home nation" write with `if (index < 0x2C)`; the remaining slots are villages,
  pirate havens, Jesuit missions and native settlements.
- **So there is real headroom** — the base game is well under 128, leaving room to
  add settlements up to that cap.

City record layout (`0x00860B70`, stride `0x20`):

| Offset | Field |
|---|---|
| +0x00 | flags (`dword`) — bits `0x400` relic, `0x10000`/`0x20000`/`0x800000` state |
| +0x04 | nation (`byte`) |
| +0x05 | population / prosperity (`byte`) |
| +0x06 | militia (`byte`) |
| +0x08 | goods (`int`) |
| +0x0C | economy (`int`, 0–200) |
| +0x10 | home nation (first 44 only) |

Positions live in the parallel table at `0x0085B170` (x, y as 32-bit ints).

### Overworld map format

The map loaders (`0x004458D0`, `0x00445D40`) read `CaribbeanMap2/4/6.bmp` — the
`2/4/6` are zoom levels of the same map — and:

- build a **`0x400 × 0x400` (1024×1024)** working surface,
- from an **8-bit paletted** bitmap (256-colour, `FUN_0053A540(0, 0x100)`),
- blitted at `0x400 × 0x286` (1024 × 646).

**`0x400` (1024) is hardcoded** in the surface size and row stride, so the world
is a fixed ~1024 grid. World/city coordinates are 32-bit ints, so the coordinate
range itself is not a constraint.

**What this means for a combined/enlarged world:** replacing the coastline bitmaps
(what existing map mods do) is straightforward and the game re-fits towns to the
new landmass procedurally. Fitting *multiple* regions into one world has three
shapes, in increasing difficulty:

1. **Compress into the 1024 grid** — lay all regions inside the fixed world with
   up to 128 towns spread across them. Asset-and-data only, no code risk.
2. **Zone transitions** — keep separate 1024 maps and swap at the edge (see
   below). Sidesteps *both* the size and town limits; medium effort, low risk.
3. **Enlarge the grid** — patch the hardcoded `0x400` everywhere plus the
   coordinate mapping. Real risk: many coupled sites, 16× the map memory on a
   32-bit game, and it does **not** raise the separate 128-town cap. Code-level,
   version-specific, test-heavy. Last resort.

### Map boundary — the "strayed too far" handler (`0x00460F50`)

`FUN_00460F50` is the world-edge handler. It checks the player against hardcoded
bounds and, when crossed, **clamps the position back inside** and shows the
turn-around message:

- Naval: *"You have strayed too far from the action. You turn around and set sail
  for the heart of the Caribbean!"*
- Land: *"…you turn around and walk further inland."*

```c
// player position, milli-units
DAT_00814304 = X   (bounds ~15,000,000 .. 437,000,000 = 0x1A10AB20)
DAT_00814308 = Y   (bounds ~15,000,000 .. 280,000,000)
DAT_0081430C = heading written after a bounce
// returns 1 if it bounced the player, 0 if in-bounds
```

This is a **clean hook point for a zone transition**: the function already detects
exactly which edge was crossed, so a mod can replace the bounce-back with "load the
adjacent region" instead.

### Swapping the map at runtime — zone transitions

The overworld map is **loaded once and cached**. The renderer `FUN_004458D0` (one
caller, `FUN_00446B49`) calls the bitmap loader `FUN_00445D40` three times (zoom
2/4/6) **only when the cache `0x008DC920` is null**, then just re-blits every frame.

So a runtime map swap is: invalidate the cache (`0x008DC920` and the per-zoom
surfaces `0x008DC924[]`) and feed the loader a different region's bitmap — the map
filenames are hardcoded literals, so this means hooking the loader (or replacing
files). Combined with the edge handler above, a "sail to the edge → load the next
region" transition looks like:

1. **Hook `0x00460F50`** — on the crossed edge, begin a transition instead of a
   bounce.
2. **Snapshot the departing region's state** — the city table plus the other
   Caribbean-wide globals (nation relations, prices, etc.). PEMF's save-sidecar
   system is the natural foundation: treat each region like a save.
3. **Swap the map** — invalidate the cache and load the new region's bitmap.
4. **Swap the town table** — repopulate `0x00860B70` with the new region's towns
   (towns are runtime-writable; see below).
5. **Reposition the player** to the opposite edge of the new map.

The payoff: **each region gets its own full 1024 map _and_ its own ~128 towns** —
so this sidesteps both hard limits without patching any engine dimension. The cost
is a loading transition at the edge (not seamless) and the real work of per-region
state save/restore. It works *within* the engine's assumptions rather than against
them, which is why it is far lower-risk than enlarging the grid.

### World-event resolver — `FUN_0044D2E0`

A large switch that resolves pending world events, useful both as a reference and
as a set of hooks. It handles: royal ultimatums / war and peace, immigrant and
troop convoys, plague, new governors, pirate and native raids, **city capture**
(which rewrites a city's nation), **procedural settlement founding** (a village is
promoted to a town with a nation, population and goods assigned), and the named
sword-duel encounters. It composes its messages through the same `AddText`
`@NATION` / `@CITYNAME` pipeline documented above — confirming that towns are
data-driven and can be created and re-owned at runtime.

### Creating and modifying towns at runtime

A town is **not a compiled entity — it is a record** in the city table
(`0x00860B70`), and the game itself rewrites those records while you play:

- **City capture** overwrites a town's nation (+0x04).
- **Founding** (the resolver's case `0x15`) promotes a native village into a
  colony, writing its nation, economy (`= 100`), population, and goods (`rand % 20`)
  into the record.

Because it is just table data that the engine already mutates, a mod can do the
same through the validated state layer: change ownership or prosperity, found a new
town in a free slot (up to the 128 cap), or reassign goods.

**Caveat — a complete town is a *bundle* of linked records, not one write.** A
fully working settlement needs, at minimum:

- a record in the city table (nation, economy, population, goods, flags),
- a position that sits on a valid coastline,
- a name (from the name tables),
- and the linked economy / governor / relations state other systems expect.

The engine supports all of this (it founds towns itself), but authoring a new town
means populating each linked piece correctly — not flipping a single value. This is
also exactly what a **zone transition** does when it swaps in a region's town set.

### Screen state — a dead end

`0x007263BC` and `0x00726A84` looked like a current-screen id and a screen-stack
depth (`DAT_007263bc = (&DAT_007268f4)[DAT_00726a84 * 4]` in the town code).
**They are not usable as a screen enum.** Sampled across a real session the
values were:

| Value | Samples |
|---|---|
| `268431327` = `0x1000EF5F` | 120 |
| `268431354` = `0x1000EF7A` | 4 |
| `268435423` = `0x1000FF5F` | 1 |

Those are pointer-like, not an enumeration. Do not re-investigate on that basis.

### "Am I sailing" — position-based, playtest-validated

Instead: the player is in a career **and** the ship's position has changed within
the last 2.5 seconds. Position freezes in towns and menus, so this separates
them.

Validated over a ~10 minute session — `sailing` read 1 throughout open-sea
travel and dropped to 0 exactly while the ship was stationary. It is still a
heuristic rather than a state read, but it behaves correctly.

One consequence: pausing at sea also reads as not sailing, so events will not
fire while paused. That is desirable anyway.

### HUD / floating text — `FUN_004B06C0`

The game's in-world informational text: ship names and types floating above
vessels, `Wind: @NUM`, `Battle Sails`, and so on. **307 call sites.**

It reads the *same* message buffer we already compose into (`0x00869B48`), so
the whole `AddText` token pipeline works with it unchanged.

```c
// eax = 0 on entry at every observed site
void DrawHudText(char* text, int x, int y, int size,
                 unsigned colour, unsigned flags, int p7, int p8);
```

The wind/sails HUD call, from the sailing render function:

```c
DAT_0085a11c = 0x4b;
FUN_004b06c0(&DAT_00869b48,            // the shared message buffer
             DAT_0085a26c / 2,         // x = screenW / 2 (centred)
             0x18,                     // y = 24
             0x4b,                     // size / wrap width
             ((0x95 < DAT_008b9984) - 1 & 0xffc0c0) - 0x1000000,   // ARGB
             4,                        // flags (alignment?)
             0xffffffff, 0);
```

Call sites consistently push the arguments, then the buffer, then `xor eax, eax`
— so `eax` is a register parameter that is always 0 in practice.

> **This is a per-frame draw, not a timed message.** There is no "show this for
> N seconds" call: the game re-draws HUD text every frame while it should be
> visible. Anything using it must re-issue the draw each frame and time the
> expiry itself — which is what `content::DrawNotices()` does.

## The render phase — `FUN_004612B0`

The sailing render function: sea, ships, their floating name labels, and the HUD
text above. **Exactly one caller, at `0x004726CA`.**

This is the anchor for anything visible, because both of the following need a
**fully drawn frame behind them**:

- **Dialogs.** The dialog renderer composites over the back buffer. Presented
  before the world is drawn, it sits on a stale, half-finished frame — the
  "scene under the modal looks broken" symptom.
- **HUD text.** Drawn at the top of the frame, the world is painted over it.

Our safe point (the main loop's `PeekMessageA`) is the top of the frame, so it is
right for *deciding* and wrong for *showing*.

### Call-site redirection, not a prologue detour

The call site is a plain `call rel32`:

```
0x004726CA:  E8 E1 EB FE FF     ->  0x004612B0
```

Since there is only one, PEMF rewrites **that rel32** to point at its own stub,
which calls the original and then does its work. One 4-byte write, no trampoline,
no relocated instructions, and trivially reversible. `render::Install()` also
verifies the call previously targeted `0x004612B0` and reverts if not.

The stub is naked so the game's register-passed arguments (`eax`, `ecx`) reach
the original untouched, and the return value is preserved across the callback.

### Notice pattern

The game's own, from `0x0046338D` — reusable exactly as-is:

```c
AddText("Press 'r' to return to ship.");   // compose, @-tokens work
DAT_0085a11c = 0x4b;                        // 0x0085A11C: text style
FUN_004b06c0(&DAT_00869b48,                 // the shared message buffer
             screenW / 2, 8,                // centred, near the top
             0x4b, 0xFFFFFFFF, 4, -1, 0);
```

Wrapped as `game::ShowNotice(text, y, colour, args, argCount)`.

### Measured map scale

Useful when choosing trigger distances. From a real session:

| Distance to nearest port | Meaning |
|---|---|
| ~1000 | sailing right into a harbour |
| ~1500–3000 | harbour in sight |
| ~10000+ | open sea |
| `-1` | nothing within the search radius |

**The closest approach varies enormously by route.** Two sessions of ordinary
sailing produced minimums of **988** and **1620** — so a threshold set at 1200
fired in one and never fired in the other. For "arriving at a port", ~3000 is a
far more reliable choice than the literal minimum.

---

## Ship-battle instance (naval combat)

> **Confidence: decompiled, not yet run.** Field offsets are read from the code
> and partially inferred; treat as a strong map, not a tested contract.

Naval combat is the game's separate "instance," and it is the natural arena to
reuse for new modes (a **race**, for example). Two findings make that practical.

### It reuses the overworld sailing system

The naval-combat sailing render is **`FUN_004612B0`** — the *same* sailing render
function used on the overworld. Combat is not a separate engine; it is the sailing
system with combatants and a win condition. So the physics, wind, camera, and ship
rendering are all already there.

### Ships are an array of fixed structs

Each ship in a battle is a **`0x4A8`-byte struct**, in an array based at
**`0x008BC468`**, indexed `[i * 0x12A]` (dword stride = `0x4A8`). Offsets confirmed
from the setup code in `FUN_0047A040`:

| Offset | Field |
|---|---|
| `+0x00` | flags / type (set to `1` on init) |
| `+0x04` | **X position** (stored ×100) |
| `+0x08` | **Y position** (stored ×100) |
| `+0x0C` | heading / facing (drives the wind gauge) |
| `+0x10` | stat (`300` at init — crew/speed) |
| `+0x2C` | hull damage (`100 - value` = %) |
| `+0x30` | name / id |
| `+0x48` | **command / action** (see below) |
| `+0x60` | combat state |

The reference ship lives at array index 4 (`0x008BD708`), so ship-to-ship distance
is `(0x008BD70C − shipX, 0x008BD710 − shipY)` fed through the octagonal formula.
Ship speed is surfaced to the HUD as `"@NUM knots"`, and sail state toggles between
**`Battle Sails`** and **`Full Sails`**.

### The battle instance — `FUN_0047A040`

This is the naval-combat instance: on entry it initialises the ship array (ship 0 =
the player, position `(0,0)` scaled ×100, heading, a stat of 300), then runs the
per-ship combat loop — naming (`"Pirate @SHIPTYPE '@SHIPNAME'"`), firing and range
checks (via `FUN_00476190`), movement, and the camera centre
(`0x0085A124`/`0x0085A128`). A race mode would enter this same instance with chosen
ships and start positions.

**Ship-to-ship distance uses the same octagonal approximation** as
`FindNearestCity` — `(min + max*2)/2` — which `game::CityDistance()` already
reproduces. So measuring "how far is ship A from point B" needs no new math.

### The combat AI and the command field — `FUN_00478730`

Per ship, the AI picks one of **Fire / Board / Flee / Strike / Escort** from the
relative hull, sail, and position of the two ships, and writes the choice to the
ship's **command field at `+0x48`** (`0x008BC4B0` for ship 0). `FUN_00476190` then
reads that field (`0` fire, `1` board/escort, `2` flee) to drive the sail flags and
the ship's behaviour that frame.

So the command field is the single lever for a race: **override it (or write the
heading at `+0x0C` directly) to steer a ship toward the next waypoint instead of
letting the AI choose a combat action.** No rendering involved — it is a plain
field write on the game thread, exactly the kind of state change the framework
already does safely.

### Designing a race mode on top of this

A player-vs-AI race reuses the instance and needs surprisingly little that the
engine does not already provide:

| Piece | How | Needs the render hook? |
|---|---|---|
| Arena, physics, rendering | reuse the battle instance | No — the game draws it |
| Ship positions & speed | read the `0x4A8` structs | No |
| **Finish-line trigger** | octagonal distance from a ship to point B < threshold | No |
| Win / standings | first ship under the threshold; announce via the event/notice system | No |
| AI racers | override `FUN_00478730` so AI ships steer to waypoints instead of fighting | No |
| **Visible course markers** | place an object the game already renders (an anchored ship / marker) at each waypoint, and use it as both marker and trigger | No, if an existing object is reused |

So the race *logic* — start, finish detection, standings — needs **no drawing at
all**. The only thing that wants rendering is a *visible* finish line, and that can
likely be avoided by placing an object the game already knows how to draw (the same
"reuse what the engine renders" approach used elsewhere), with audio cues as a
fallback.

### Still to find (next steps)

- **The battle-entry path** — how the game enters a naval instance and populates
  the ship array, so a mode can be started on demand with chosen ships and start
  positions. Not yet located.
- **Confirming the position offsets** inside the `0x4A8` struct.
- **Steering an AI ship to a waypoint** — mapping the command field and heading to
  directed movement.

None of these need the render hook; they are ordinary reverse engineering of the
combat setup, and player-vs-AI needs no networking.

## Resolution / widescreen

The game has a native resolution switcher (Options → *Change Video Resolution*),
but its list is **filtered to 4:3 modes** — so it offers e.g. 1440×1080 but not
1920×1080. The framework adds widescreen by calling the game's own apply function
directly, bypassing that list.

| Address | What |
|---|---|
| `0x004D3AB0` | `SetResolution(int w, int h)` — cdecl. The game's own "switch to WxH". |
| `0x0072637C` / `0x00726380` | UI width / height — copied into `ScreenW` / `ScreenH` by the apply. |
| `0x008CAC18` / `0x008CAC1C` | current device width / height (the apply's change-guard). |
| `0x008CABF0` | resolution table base (12-byte entries: width at +4, height at +8), indexed by the slider. |

`SetResolution` resets the D3D device, resizes the window, and — importantly —
derives the projection aspect from `height / width`. So 1920×1080 yields a genuine
**16:9 view (wider field of view), not a stretched 4:3**.

**Do not call `SetResolution` cold.** Invoking it outside the menu's own sequence
(e.g. from the safe point at startup) reaches the device reset in a state the menu
would have prepared, and the process crashes on the reset — verified. The
`game::ForceResolution` wrapper exists and is correct as to *what* to write, but
forcing it standalone is not viable.

**The implemented approach adds 1920×1080 to the menu's resolution list**, so the
player selects it and the game runs its own complete, crash-free switch. The list
builder `FUN_004B2DF0(&table)` enumerates display modes and keeps only 4:3 ones via
`cmp (height*100/width), 75` → `jne` at `0x004B2E8A` (bytes `75 21`). The core NOPs
that `jne` (`90 90`) so every mode — widescreen included — is listed. The table is
at `0x008CABF0` (12-byte entries, width +4, height +8), and the builder is a
two-pass count-then-fill, so the count stays consistent.

**Result: 1920×1080 is selectable and the 3D world renders in true 16:9** (the
projection aspect comes from the resolution).

### The 4:3 UI at 16:9 — an open problem

The game renders its 2D UI in a **fixed 640×480 logical space** — `ScreenW`/`ScreenH`
(and their sources `UIWidth`/`UIHeight`) stay `640×480` for *every* resolution;
only the device size changes. At a 4:3 device that logical space scales up
uniformly and everything lines up. At 16:9 (1920×1080) the horizontal scale
diverges from the vertical, so the 2D UI stretches and the **mouse is offset
horizontally** over menu items.

The mouse maps through `FUN_005025D0` (`ScreenToClient`) → `FUN_005045A0` →
`FUN_00504310` (the client→UI transform). A proper fix pillarboxes the 2D UI (keep
the 4:3 interface centred with side bars, keep the 3D at 16:9) and adjusts the
mouse mapping for the offset. That is the pending piece; forcing `ScreenW`/`ScreenH`
to the device size is *not* a safe shortcut, since the 2D pipeline is built around
the 640×480 logical space.

## Audio / Sound System

> **Confidence: decompiled, not yet run.** Recovered by byte analysis
> (`re/scripts/find_sound_api.py`, `trace_sound.py`, `trace_sound2.py`) and then
> **decompiled** (`re/scripts/DecompileTargets.java`). The architecture below is
> read from the actual code, but **nothing here has been exercised in-game yet** —
> treat it as a confirmed map, not a tested contract.

### Engine

The game's audio is **Miles Sound System** (`Mss32.dll`), dynamically referenced
through **71 imported `AIL_*` functions**. Their IAT slots are contiguous:

| Range | Contents |
|---|---|
| `0x006C0470`–`0x006C0588` | all 71 Miles import slots (e.g. `AIL_start_sample` at `0x006C04E4`, `AIL_set_named_sample_file` at `0x006C0504`, `AIL_allocate_sample_handle` at `0x006C04B4`) |

Miles is called with ordinary `FF 15` indirect calls, all from one code module
around `0x0052C000`–`0x0052F800` — the game's sound engine.

### Architecture

The game has a single **audio manager** object (a global at `0x008ECD78`, a C++
object with a vtable) and a **table of registered sounds** (`0x008ED4A0`, one
`0x40`-byte record per sound). Game code plays a sound by its **numeric id**; the
manager looks up the record and handles loading and playback.

If the manager is not up, the game prints
*"The audio manager has not been properly initialized yet"* — a handy landmark.

### Functions

| Address | Conv | Role |
|---|---|---|
| `0x0052F700` | (custom) | **Core play.** `param_1` = **numeric sound id** (`id*0x40 + 0x008ED4A0` indexes the table), `param_2` = volume (float), `param_3` = pan (`0x3F000000` = 0.5 centre). Loads if needed, applies volume/pan via the manager vtable, starts the sample. |
| `0x004A06C0`, `0x00528E70` | fastcall | **Game-logic entries.** Thin wrappers: check the manager is initialised, then call `0x0052F700`. |
| `0x0052CDC0` | cdecl | **Filename resolver.** `int (char* baseName)` — see below. |
| `0x0052D6D0` | fastcall | **Sample loader** → `AIL_set_named_sample_file` with a `".wav"` or `".mp3"` format hint. |
| `0x0052DD30` | thiscall | **Play/stop dispatcher** on the manager. `param_2` = channel (0 = 2D, 1 = 3D, 2/3 = stream), `param_3` = sample index → `AIL_start_sample` / `AIL_start_3D_sample` / `AIL_start_stream`. |
| `0x0052D880` | fastcall | Audio init — sets Miles redist dir `".\Miles\win32"`, `AIL_startup`, `AIL_open_digital_driver`. |
| `0x0052D950` | — | Audio shutdown. |

### How sounds are resolved — by constructed filename

`FUN_0052CDC0(char* baseName)` builds candidate filenames and probes each until
one loads, in this order:

```
<base>-000.wav, <base>-001.wav, ...   (numbered variants)
<base>.wav                            (single)
<base>-000.mp3, <base>-001.mp3, ...
<base>.mp3
```

Two things this proves:

- **The game resolves sounds by building a filename at runtime, not from a fixed
  compiled catalogue.** A file that matches the constructed name is picked up.
- **MP3 is supported as well as WAV** (`".wav"` @ `0x0070AB88`, `".mp3"` @
  `0x00712E74`, `"-%03d.wav"` @ `0x00712F68`).

On disk the sounds are **loose files under `Assets/Sounds/`** (not sealed inside
the `.FPK` archives) — the same situation as the text overrides.

### Known Miles signature (from the Miles SDK)

```c
S32 AIL_set_named_sample_file(HSAMPLE S, const void* file_image,
                              S32 file_size, S32 file_format, S32 flags);
S32 AIL_start_sample(HSAMPLE S);
```

`AIL_set_named_sample_file` takes a **memory image**, not a path — the game reads
the file bytes itself and hands the buffer to Miles.

### What this means for adding custom audio

The important nuance: the game's high-level play path plays **by numeric id from
the pre-registered table** — not by an arbitrary filename at the call site. So:

1. **Triggering an existing game sound** is the easy native case — find its id and
   call `0x0052F700` (with the manager initialised). Good for reusing the game's
   own SFX.
2. **A brand-new custom clip** through the native path needs either a new table
   record (id → base name) or a call into the lower-level filename path
   (`0x0052CDC0` + loader) with our own base name and the manager `this`. Feasible,
   but more moving parts.
3. **Self-contained playback via XAudio2** (built into Windows, no dependencies)
   needs none of the above, works on any build, and is fully under our control —
   it just mixes alongside the game rather than through its mixer.

Recommended: **XAudio2 for our own custom clips first** (simplest, version-proof);
the native id-based path for reusing existing game sounds. Either way, audio does
**not** depend on the render hook, so a spoken callout (e.g. "land ho" on the
`nearPort` trigger) can work before any on-screen drawing does.

### Still to confirm (needs an in-game test)

- The cleanest callable entry for playing a **new** clip by name through the game
  (tracing where `0x0052CDC0`'s loader stores the sample and how to start it).
- Everything above read cleanly from decompilation but has **not** been run yet.

---

## Useful Call Sites

Reference points for how the game itself does things.

| Address | What happens there |
|---|---|
| `0x004125A0` | Mutiny message — the canonical compose→wrap→show sequence |
| `0x00443DC0` | Crew status (`"Your crew of @NUM is @HAPPY."`) |
| `0x00410321` | Divide the plunder |
| `0x00437A5B`, `0x00437CA6` | `Morale +@NUM` — **first trigger-hook candidates** |
| `0x004608E0` | "Captain, the men are starving." |
| `0x004692EF` | Modal yes/no prompt (landing party) |
| `0x0041191E` | Town menu — the polled form in use |

---

## Reentrancy — read this before hooking

`ShowMessage` and `ShowDialogDirect` are **modal**. Their loops call
`timeGetTime`, which re-enters our IAT hook.

Getting this wrong recursed **~610 times in 0.2 seconds**, exhausted the stack,
and killed the process with no error dialog. Three guards are required:

1. Update key edge state **before** doing any work, never after.
2. A hard reentrancy flag — a card must never open inside a card.
3. A cooldown, so one physical press cannot queue several cards.

---

## Verified vs Unverified

Everything below was either observed in a running game (with log evidence) or is
marked otherwise. Nothing here is inferred from disassembly alone unless said so.

| Capability | Status |
|---|---|
| DLL injection, game-thread execution | **Verified in-game** |
| Reading live game state | **Verified in-game** |
| `AddText` with token substitution (`@NUM`, `@HAPPY`) | **Verified in-game** |
| `WrapText` word wrapping at column 44 | **Verified in-game** |
| Modal card presentation | **Verified in-game** |
| Modal choice via option lines, returning the index | **Verified in-game** (3 options) |
| Writing state as an event consequence | **Verified in-game** |
| Deferred dispatch from the safe point | **Verified in-game** |
| Save / load detection via `CreateFileA` | **Verified in-game** |
| Engine formatter never calls printf-family | **Verified statically** — 0 call sites in `0x4F5000-0x4F8000` |
| More than 3 options on one card | **Untested.** The loader permits 6; only 3 have been rendered. |
| `@NAME`, `@CITYNAME`, `@NATIONALITY`, `@MONTH`, `@SHARE`, `@DIFFICULTY` | **Unverified whether they consume a vararg.** Not used; rejected by the content loader. |
| `ShowMessage` `ecx` = background-art index | **Inferred from disassembly**, only `ecx = 0` has been exercised |
| `ShowDialogDirect` args 4-10 | **Copied verbatim from the game's own call site**; individual meanings unknown |
| `AddTextOption` renders selectable options on a modal card | **Disproven** — it is the town-menu system |
| `ShowMessage` with `eax = 10` as a one-shot modal | **Disproven** — non-blocking, returns `-2` |
| In-game event triggers | Not started |
| Audio engine = Miles (`Mss32.dll`), 71-import IAT map | **Verified statically** — byte analysis of the import table |
| Sound module + loader/player function roles | **Decompiled** — read from the actual code |
| High-level play is **by numeric id** from a registered table (`0x008ED4A0`) | **Decompiled** — not yet exercised |
| Filename resolution order (numbered `.wav` → `.wav` → `.mp3`); MP3 supported | **Decompiled** — not yet exercised |
| Playing a *new* clip by name / adding to the table | **Unconfirmed** — needs an in-game test |

### Known unknowns

Worth stating plainly, since this file may be read as authoritative:

- The **morale formula** is decompiled and its behaviour matches observation
  (spending plunder lowered the morale level as predicted), but the meaning of
  `0x00869A76`, `0x0085A158` and `0x00869B27` is inferred, not confirmed.
- `FUN_00430190`'s ten parameters are reproduced from a working call site. Only
  the first three (x, y, sound) are understood.
- The `.pirates_savegame` format itself has not been examined at all. We detect
  saves and load by file access, never by parsing them.
