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

### Token argument appetites — read, never guessed

A token consumes a fixed number of varargs, and that number is only knowable by
reading the code. Getting it wrong does not error: it reads whatever is next on
the stack. Verified counts:

| Token | Slots | Where the value comes from |
|---|---|---|
| `@NUM` | 1 | any int |
| `@HAPPY` | 1 | mood 0-4 |
| `@CITYNAME` | **3** | `0x008DBD08 + index*12`, three dwords |
| `@NATIONALITY` | 1 | `0x00860B74 + index*32`, **signed** byte |
| `@LOCTYPE` | 1 | `0x00860B80 + index*32`, int |

`@CITYNAME` taking three is the one that catches people. From the sailing
render at `0x00462548`, formatting
`"'We're bound for @CITYNAME.' (@NATIONALITY @LOCTYPE)"`:

```asm
mov  al,  [edx + 0x860B74]      ; nation, movsx'd before pushing
mov  ecx, ebx
shl  ecx, 5                     ; index * 32
mov  edx, [ecx + 0x860B80]      ; location type
push edx                        ; ... pushed right-to-left
movsx eax, al
push eax
lea  ecx, [ebx + ebx*2]         ; index * 3
lea  edx, [ecx*4 + 0x8DBD08]    ; record base + index*12
mov  ecx, [edx]                 ; three dwords, written as a block
sub  esp, 0xC
...
push 0x707948                   ; the format string, last
```

A city name is a **three-word record**, not a string pointer. Any token not in
the table above is refused by the content loader, because enabling one means
reading its call site first. `docs/EVENT_AUTHORING.md` hides all of this behind
`{port}`.

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

### Trade goods — the cargo model, and what "adding one" would cost

> **Confidence: static analysis.** Addresses and counts read from the binary;
> not yet exercised at runtime.

**Seven item slots, 0-6.** The names come from the `[ITEM]` group in
`text.ini`: `Gold, Food, Luxuries, Goods, Spice, Sugar, Cannon`.

**The player's hold is one contiguous int array at `0x00869AB4`**, indexed by
item — the code reads `[reg*4 + 0x869AB4]` (e.g. `0x00405466`, `0x004DDF23`,
`0x00443E76`). Item 0 is *gold*, which is why `0x00869AB4` is also the
undivided-plunder global; item 6 is cannon at `0x00869ACC`.

| Item | Address | Name |
|---|---|---|
| 0 | `0x00869AB4` | Gold (= undivided plunder) |
| 1 | `0x00869AB8` | Food |
| 2 | `0x00869ABC` | Luxuries |
| 3 | `0x00869AC0` | Goods |
| 4 | `0x00869AC4` | Spice |
| 5 | `0x00869AC8` | Sugar |
| 6 | `0x00869ACC` | Cannon |

**There is no headroom, and the array cannot grow in place:**

- `0x00869AD0` — the dword immediately after item 6 — is a live global with
  ~43 references. The array is boxed in.
- **82 code sites** touch the array.
- Each item is *also* referenced by its own absolute address 11-18 times. The
  code is not a clean indexed abstraction; it names individual goods directly.
- The count appears as immediate loop bounds: `cmp eax, 6` (`0x00405472`),
  `cmp edi, 6` (`0x004DDF07`), `mov ecx, 7` (`0x004DCFEE`) — 15 such sites sit
  within a few instructions of a cargo access alone.

So an 8th engine slot means relocating a 7-entry array that 82 sites reference,
many by hardcoded per-item address, and finding every bound. That is not a
seventh-slot fight worth having.

**What *is* data-driven, and is the opening:**

- **Item names are not in the exe.** `text.ini` is parsed at runtime: `@ITEM`
  (`0x006FAE1C`) and `__VAR` (`0x007107E0`) are strings in the binary, while
  `[ITEM]` is not — group names come from the file.
- ⚠️ **`@ITEM` is NOT bounds checked.** Asking for an index past the end of the
  list access-violates (`0xC0000005`), confirmed in a running game. **Never emit
  `@ITEM` with an index the live list does not contain**, and bounds-check any
  future `{item}` placeholder against the list's real length rather than a
  constant. Same class of hazard as the token argument counts: the engine does
  not check, so we must.
- **Loose override works, but PER SUBSYSTEM — and not for text.** Measured with
  a startup-armed file probe: the ten `.FPK` archives are opened once and read
  from thereafter, and `Data\AdvancedLighting.ini` and `assets\data\Landscape.ini`
  are looked for **on disk first** and missed before the packed copies are used —
  so creating either loose would override it with no repacking. But there is
  **no disk probe at all for `text.ini`**, which lives in the same archive. That
  absence is evidence rather than a gap, precisely because the other two misses
  prove the probe catches them. So `[ITEM]` cannot be extended without repacking
  `Pak1.FPK` or patching the parsed table in memory.
- **The town side stores no per-good table.** A settlement record holds a single
  `goods` int (`+0x08`) and `economy` (`+0x0C`) — prices and availability are
  *derived*, not stored per (city, good). There is far less fixed table on the
  economy side than on the cargo side.

**The shape that works.** A new good lives in PEMF's own memory with its own
price model, reading the town's real `economy` and `goods` fields as inputs so
it behaves like part of the world, and presenting through our own drawing rather
than the engine's trade screen — which iterates 0-5 and cannot be given a row.
Its name is simply a string in its own JSON, drawn through the text and drawing
routines we already use — native-looking because it is natively rendered.
`@ITEM` was never actually needed: it would only let *engine* code name our
good, and that code iterates items 0-5 and could not show an eighth one anyway.

This is the standing principle — new systems live in our memory, the exe stays
the world and the renderer — but it is a much better bet than it was, because
drawing our own UI is now solved rather than blocked.

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

## Drawing our own text — solved, and how

> **Confidence: running in-game**, on both supported builds.

Two facilities together make it work: a place to draw from, and the game's own
text routines to draw with.

### The frame hook — two phases

The framework runs inside every frame on the game's **own** device — the
renderer singleton at `0x00727C30` holds the `IDirect3DDevice9*` at `+0x60`.
Full account in [`README.md`](README.md#the-render-hook--solved).

Both `BeginScene` (vtable 41) and `EndScene` (42) are hooked, and **which phase
a draw belongs in is decided by how the game draws that kind of thing.** They
are not interchangeable:

| Phase | State of the frame | What goes here |
|---|---|---|
| `BeginScene` | open and empty; the world has not been built | **world-anchored text**, which builds scene-graph nodes for the render walk to draw |
| `EndScene` | scene complete, nothing presented | **screen-space HUD text**, an immediate 2D blit that must land on top |

Getting this backwards is silent: a 2D blit at `BeginScene` is painted over by
the world, and a scene-graph node at `EndScene` is built after the walk that
would have drawn it. Neither errors — you simply see nothing. The safe point,
meanwhile, is right for *deciding* and wrong for *showing*.

At `BeginScene` the real method is called **first**, then our callback: the
device has to be inside a scene before anything we do can contribute to it.

### One displayed frame is several render passes

`BeginScene`/`EndScene` pairs are **not** frames. The game runs more than one
pass per displayed frame and they do not share a camera, so world-anchored text
issued in every pass is drawn several times over, in different places. The
symptom is a notice that appears twice: one copy correctly tracking the ship,
one apparently stale copy drifting somewhere else.

World text is therefore drawn in the **first** pass of each frame only. The
first pass is the world pass: the one that walks the scene graph the label was
just attached to. Later passes carry their own camera and their own graph, so a
label built during one either lands in the wrong place or is never walked at
all.

**The frame boundary comes from the safe point, not from `Present`.** This is
worth stating flatly because the obvious answer is wrong and fails silently:
`Present` (vtable 17) *is* hooked, but **this game never calls it on the
device**. A pass counter reset only there never resets, so after the first
frame no pass is ever "the first" again and anchored text stops drawing
entirely. The top of the game's own main loop — the safe point, which already
runs exactly once per iteration — is the reliable boundary. `Present` still
resets the counter when it fires; nothing depends on it.

### Drawing belongs to the sailing view

World coordinates are projected against whatever camera the current screen is
using, so a label drawn from a menu lands *on that menu* — a lookout's call
across the Load/Save map. Both draw phases are gated on the overworld being on
screen.

For a long time that gate asked the wrong question. It tested whether the ship
had moved recently, which is not a screen test at all: opening a menu freezes
the ship, so a notice went on painting over the menu until the window lapsed.
Shortening the window could not fix it, because a becalmed ship at sea is
indistinguishable from a menu under a motion test — it only traded a menu leak
for notices vanishing at sea.

The gate now uses the screen-state globals (`ScreenId` / `ScreenDepth`). These
are **not an enum** — an earlier playtest established that and recorded them as
a dead end, which was too strong a conclusion. They read as a bitfield, but the
pair taken together is a stable per-screen signature, and a session visiting
every screen separates them cleanly:

| Screen | `ScreenId` | `ScreenDepth` |
|---|---|---|
| Sailing / overworld | `0x0FFFEFDF`, `0x0FFFFFDF` | 3 |
| Town | `0x0FFFEFFA`, `0x0FFFFFFA` | 3 |
| Load / Save | `0x0FFBE770`, `0x0FFBE750` | 4 |
| Battle | `0x8FFFEFFF`, `0x8FFFFFFF` | 4–5 |
| Main menu | `0x0FFFEFF0`, `0x0FFFFFF0` | 1 |

Those numbers are **evidence, not constants**: nothing compares against them.
Hardcoding them would be a value nobody could maintain — one HUD state we never
visited and notices stop, silently. Instead `triggers::WorldOnScreen()`
calibrates itself. A ship whose position changed on *this* tick is unambiguously
out on the overworld whatever the numbers are, so that is when a signature is
learned; afterwards the overworld is on screen whenever the live signature
matches a learned one. **Motion is used to learn the answer, never to be the
answer.**

This fixes both directions at once: a menu never matches, so nothing leaks and
there is no tail to wait out, and a becalmed ship still matches, so notices no
longer drop out when you stop. It fails closed — an unrecognised screen draws
nothing until the ship moves and teaches us its signature — and more than four
overworld signatures logs a loud warning rather than going quiet.

Battle is deliberately not learned: it has its own ship array and the overworld
position is frozen throughout, so a notice anchored to a map position would hang
at a stale place.

A notice's clock is also **held while the overworld is off screen**, so one does
not spend its life expiring behind a menu and reappear already gone.

### The shared message buffer is a trap — and a discovery

Composition (`ResetMessage` + `AddText`) writes into the game's **shared**
message buffer at `0x00869B48`. Text left there does not simply sit idle: the
game draws it **over the player's ship**, on its own, in its own style. Every
notice appeared twice until this was understood — once where we drew it, once
hanging on the vessel.

So: **resolve once, when the notice is posted (at the safe point), copy the
result out, and hand the buffer back empty.** Per-frame drawing then takes a
plain string and touches no shared state. `game::ComposeText` does exactly this.

### World-anchored text — `FUN_004AEC30`

The floating labels over ships — `@NATIONALITY @SHIPTYPE '@SHIPNAME'` and
`'We're 2 days out of Antigua.'` — come from one call with **13 call sites**.
It takes register parameters *and* nine `cdecl` stack arguments:

```
DrawWorldText(ecx = const char* text, eax = uint32 colour,
              int wx, int wy, int wz,
              int a4, int a5, int a6, int a7, int a8, int a9)
```

The caller does `add esp, 0x24` — nine arguments, ours to clean up. Recovered
from the two sailing call sites, which are identical but for `wz`, `a5` and
`a7`:

| | ship name (`0x0046231A`) | ship speech (`0x00462E88`) |
|---|---|---|
| `ecx` | `0x00869B48` | `0x00869B48` |
| `eax` | `0xFF000000` | `0xFF000000` |
| `wx`, `wy` | ship map pos / 1000 | ship map pos / 1000, `wy` less 500 |
| `wz` | `625` | `500` |
| `a4` | `0` | `0` |
| `a5` | `round(-70 or -90 × [0x00713600])` | same |
| `a6` | `[0x008B98D8]` | `[0x008B98D8]` |
| `a7` | `500` | `clamp(age × 30 + 200, 0, 500)` |
| `a8` | `12` | `12` |
| `a9` | `0` | `0` |

Notes that matter:

- **The text is a genuine parameter.** The callee saves `ecx` (`mov ebx, ecx`)
  and the string builder at `0x004AEB20` walks it with a plain `strlen` and
  copies it. The game passes the shared message buffer at both sites, but
  nothing requires that — we pass our own, and so avoid the buffer entirely.
- **World coordinates are map units divided by 1000** — plain integers here,
  *not* the `× 0.001` float scaling the positional-audio call uses.
- `a5` is a camera-relative tilt the game recomputes every frame:
  `-90` if view flag `0x40` is set in `[0x0085A164]`, else `-70`, times the
  double at `0x00713600`.
- `a7` runs `0` … `500`. The game ramps it **up** as a line appears; driving it
  **down** is what gives a notice a fade as it expires.
- `a9 = 0` makes the callee use the default label manager at `[0x008C9DD8]`.
- The drawer reads the text size from the global at `0x0085A11C`.

Hand it a map position and the game re-projects the label every frame, so it
**follows whatever it is over** with no projection maths of our own — and it
looks native because it *is* native. Any world position works, not just the
player's: anything with a map coordinate can be labelled this way.

### A correction worth recording

For a while this project had `FUN_00488A80` documented as the world-label
drawer, with a `kind` argument selecting the player's ship. **That was wrong.**
`0x00488A80` is **positional audio**: it opens with the audio manager at
`[0x008ECD78]`, calls its "is initialised" method at `+0x18`, and bails to the
string *"The audio manager has not been properly initialized yet"*. Its first
argument is a sound id, not a label kind, and its trailing `float` is gain.

The mistake was plausible because it *is* called from the sailing render at the
player's ship position, right beside the label code, and calling it appeared to
work — the text that showed up over the ship was the game redrawing the stale
message buffer, the very trap described above. Two effects with one apparent
cause. The lesson: **a call that "works" is not a call that is understood**; the
disassembly settled in a minute what screenshots could not.

The error was not wasted. `0x00488A80` is now the entry point for spoken
callouts at a world position — see the audio section of the roadmap.

### In the framework

`notice` events take `"anchor"`: `"screen"` (default, a line at the top, drawn
at `EndScene`) or `"ship"` (hangs over the player's vessel and follows it,
drawn at `BeginScene`, easing out over its last second). A draw that raises an
exception latches drawing off for the session and says so in the log, rather
than repeating the fault every frame.

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

## Flags, emblems, and the overworld ship array

> **Confidence: verified in game**, except where marked. Full write-up in
> [`re/experiments/flags/`](../re/experiments/flags/README.md).

### Custom flags and sails are already unlimited

The game enumerates them rather than loading fixed names, so **adding flags
needs no mod at all**. `FUN_004B00E0(filter, container)` splits its filter on
`;` (`flag_*.dds;flag_*.tga`), scans the game's `custom\` **and** the player's
profile `Custom\` folder (see [The profile folder](#the-profile-folder-is-not-one-name))
with `FindFirstFileA`, de-duplicates, and appends into an array it resizes as
needed.

| Global | Holds |
|---|---|
| `0x008C9560` | number of custom **flags** found |
| `0x008C9564` | number of custom **sails** found |

Both read `0` until **Options → Change Sails and Flags** is first opened — the
scan is lazy. Measured: 11 flag files gave 11, 8 emblem files gave 8.

The picker (`FUN_004B7C70`, Options case `0x212`) shows three thumbnails and
indexes them `1 % count`, wrapping around the whole list — a carousel, not a
cap. The choice persists **by name** as `CustomFlag` / `CustomSail` in
`Config.ini`, so adding files never invalidates a saved selection.

### The player's flag is one texture pointer

`FUN_004AF760` re-applies four textures to matching scene nodes every time
round, comparing before applying:

| Global | Applied to nodes named |
|---|---|
| `0x008E8FB0` | `ship_playercolor*` |
| `0x008E8FB4` | `flag*` — **the player's flag** |
| `0x008E8FB8` | `ship_sail_emblem_lrg*` |
| `0x008E8FBC` | `ship_sail_emblem_sml*` |

Write `0x008E8FB4` and the flag on the mast changes; the engine keeps it there
with no further calls. Verified in game across repeated swaps and a restore.

### Loading a texture by name

Recovered from the config-load path at `0x004293D7` — the code that turns
`CustomFlag = <name>` into the texture on the mast.

| Address | Signature |
|---|---|
| `0x004F4ED0` | `char AssetExists()` — `esi` = name |
| `0x00500850` | `void* LoadTexture()` — `esi` = name, `eax` = format struct, or 0 for defaults |

Both take the name in **ESI** and end in a plain `ret`, so there is no stack to
clean. With `eax = 0` the loader requires the name to end `.dds` and chooses the
format itself. **A bare filename works** — `flag_spa.dds`, exactly as the
directory scan reports it. Verified in game across all eleven flags.

The name globals are the engine's string type — characters at the pointer,
**length in the dword at −4**:

| Global | Holds |
|---|---|
| `0x00726A88` | `CustomSail` name |
| `0x00726A8C` | `CustomFlag` name |
| `0x00726A90` | sail name list |
| `0x00726AA8` | flag name list |

Each list is the game's growable array: `+0x04` entries (`char**`), `+0x08`
capacity, `+0x0C` count.

### ⚠️ Engine objects are refcounted

**The count is the dword at `+4`, and reaching zero calls `vtable[0](1)`.** Both
halves are visible in the same config-load path, which is why PEMF copies its
ownership sequence wholesale rather than paraphrasing it:

```
load -> store -> AddRef(new) -> Release(old)
```

Capturing a texture without taking a reference **crashes the game**, because the
picker releases each one the moment it scrolls away. That is not hypothetical —
it is how this was learned. See the layer rule in
[`DEVELOPER.md`](DEVELOPER.md#layer-rules).

### Nation flags are five fixed meshes

`FUN_0046BAA0` loads five flag meshes and clones them into live scene nodes:

| Prototype | Live node | Nation |
|---|---|---|
| `0x00860B40` | `0x00860B54` | Spanish (0) |
| `0x00860B44` | `0x00860B58` | English (1) |
| `0x00860B48` | `0x00860B5C` | French (2) |
| `0x00860B4C` | `0x00860B60` | Dutch (3) |
| `0x00860B50` | `0x00860B64` | Pirate (4) |

Custom nation *art* is possible by retexturing a live node at runtime — additive
and reversible, unlike replacing `custom\flag_spa.dds`. A **sixth nation is
not**: five hardcoded slots, the same shape as the cargo array.

### The overworld ship array

Found because `PlayerX`/`PlayerY` are fields of its first record:

```
base 0x008142F8, stride 0x45C, player = entry 0
  +0x04   nationality  (0x008142FC)   read by "She's flying @NATIONALITY colors."
  +0x0C   X            (0x00814304)
  +0x10   Y            (0x00814308)
  +0x58   flags        (0x00814350)
```

⛔ **`+0x04` is NOT the player's flag**, and two measurements say so: cycling it
through all five nations changed nothing on the mast, and it reads `0` (Spanish)
on a career started under the **English** flag, so it does not track the nation
the player sails for either. Recorded because it is a real find about AI
vessels — it is simply not the flag.

**Consequence for false colours:** the flag and the nationality the game reasons
about are unrelated systems that never touch, so the engine has no concept of
flying false colours. That makes the mechanic entirely PEMF's to define rather
than something to keep in step with engine behaviour.

⛔ **The player's own nationality field is vestigial.** Four careers begun under
four different crowns all read `0`. It does not track the nation you serve, and
writing it is not a lever on the AI — a game deciding hostility from a field
that is always `0` would treat every player as Spanish. For the player's real
nation see `PlayerNation` below.

## Nations

### The relations matrix — `0x0085A168`

An **8×8 int32** grid, indexed `[a * 8 + b]`. `1` = at war, `-1` = treaty,
`0` = neutral. Live state, confirmed changing in game as wars ended.

Size is stated twice by the binary: the new-game reset at `0x00404229` clears it
with `rep stosd` over `0x40` dwords, and the save serializer writes it out as
exactly `0x100` bytes.

Slots 4 and 5 are initialised **permanently at war with all four crowns**
(`0x004042E0`–`0x0040431D`). Slot 4 is Pirate. Slot 5 is a sixth power, at war
with every crown and neutral toward pirates.

### The player's standing — `0x00869A78` and `0x00869A88`

Two parallel `word[4]` arrays, both indexed `nation * 2`:

| Address | Meaning |
|---|---|
| `0x00869A78` | reputation with nation N |
| `0x00869A88` | rank with nation N; `0` = no letter of marque |

Ranks run 0..9 and index a `char*[10]` at `0x007272B4` — Grunt, Grunt, Captain,
Major, Colonel, Admiral, Baron, Count, Marquis, Duke. Both arrays read `0` for a
captain who has never taken a commission, which is the correct value rather than
a failed read.

### ✅ Reputation IS the hostility value

**Negative reputation with a nation is that nation being hostile to you.** This
corrects an earlier conclusion in this document that no hostility model had been
found — the search was looking for a per-*ship* flag, and the game keeps a
per-*nation* number instead.

Two independent sites settle it:

```
0x0040B5DC  cmp word [nation*2 + 0x869A78], -1
0x0040B5EA  jge skip
            ...composes "'I see that the @NATIONALITY have put a price on
            your head, yet I sense that you are a basically good person."
            followed by the offer of an amnesty
```

and, in the loop that searches settlements (`0x00406265`), **cities are skipped
entirely when their nation's reputation is below zero** — which is the game
declining to let you make port where you are not welcome.

| Reputation | Meaning |
|---|---|
| `> 0` | welcome; ports usable |
| `< 0` | hostile; that nation's settlements are excluded from port searches |
| `< -1` | a price on your head, and an amnesty becomes purchasable |

This is **player state**, inside the 216-byte player record, and it is saved with
the career. Writing it is a far smaller step than touching the nation relations
matrix, which is world state — see the note in `nations.h`.

### `PlayerNation` — `0x00869AA8`, int16

**The crown the player serves.** The game does not store your choice at
character creation; it stores a consequence and recomputes it. At `0x0040D690`,
on every promotion, the nation you hold a **strictly higher rank** with than any
other is written here.

Confirmed against a city's nation byte (`0x0040DA19`), pushed where
`@NATIONALITY` is expected (`0x0040FF62`), and measured across four careers:
English → `1`, Dutch → `3`, Spanish → `0`, French → `2`. Same enum as the
flag-mesh table.

Read it rather than deriving it — it is the value the engine itself acts on.
Full account in [`re/experiments/nations/`](../re/experiments/nations/README.md).

### The save serializer — `0x00401400`

Pushes `(address, size)` for every block the game persists, so it doubles as a
map of what game state *is*. Sizes worth having:

| Block | Size | Meaning |
|---|---|---|
| `0x00869A70` | `0xD8` | the player record — 216 bytes, ending at `MessageText` |
| `0x0072C6B8` | `0xB8` | the pending-career record, serialised **only in mode 2** (character creation) |
| `0x0085A168` | `0x100` | the relations matrix |
| `0x008142F8` | `0x45C00` | the overworld ship array |
| `0x00860B68` | `0x1000` | 128 settlement records |

⚠️ **The ship array is 256 slots**, not the 24 every loop in this framework
walks (`0x45C00 / 0x45C`). Twenty-four has been a serviceable window on the
water near the player, never a correct bound.

## Weather — storms and clouds on the sailing map

Both are prefab instances drawn by the overworld tick `FUN_004612B0`, positioned
every frame through `FUN_004BBC80`. Registered once at startup by
`FUN_00429610`, which is also where the asset names live.

### ⚠️ There are TWO cloud prefabs and they are not the same thing

| Prefab | Asset | What it is |
|---|---|---|
| `0x008CC498` | `cloud00.nif` (`0x006FDD84`) | the **ordinary white fair-weather cloud** |
| `0x008CCB58` | `stormcloud_sound.kfm` (`0x006FDD90`) | the **dark storm** |

They are registered on adjacent lines, which is exactly why the first attempt at
this scaled the wrong one: the sky filled with enormous white puffs while the
storms stayed the size they always were. The symptom is unmistakable once you
know it — *"the normal clouds look larger for sure"* with no change to the
weather you actually wanted.

### The draw call

```c
// __thiscall, ecx = prefab
FUN_004BBC80(prefab, x, y, z, rotMatrix, scale, param_7, param_8);
```

and inside it the sixth argument lands on the scene node:

```c
*(float*)(*(int*)(node + 0x1c) + 0x68) = ABS((float)scale * 0.01);
```

so **drawn size is `scale * 0.01`**, and the caller's arithmetic is ours to
change. `FUN_004BBC10` is a thin wrapper that converts three binary angles
(x `2*pi/2^32`) into the rotation matrix first.

### The three sites worth patching

| What | Address | Original |
|---|---|---|
| **Storm size** | `0x0046374A` | `lea edx,[ebx+ebx*4]` + `shl edx,4` → `EBX * 80` |
| **Storm height** | `0x0046376E` | `push 0x12C` (z = 300) |
| **Fair cloud size** | `0x0046393A` | `imul ebx,ebx,0xFA` (x250) |
| **Fair cloud height** | `0x0046395D` | `push 0x1F4` (z = 500) |
| **Weather slots** | `0x00463973` | `cmp eax,3` |

The storm pair is **six bytes**, exactly the length of `imul edx, ebx, imm32`
(`69 D3 imm32`), so PEMF replaces both instructions with one that computes the
same value from an operand it controls — same registers, same result, no code
displacement. See [`src/core/storms.h`](../src/core/storms.h).

### ⛔ THREE WEATHER SLOTS IS A HARD CEILING

The loop at `0x0046354A` indexes two parallel arrays:

```
0x008B98E4   x positions
0x008B98F0   y positions
```

**They are 12 bytes apart — exactly three dwords.** A count of 4 reads and
writes the x array into the y array. `cmp eax,3` looks like a "how much weather"
dial and is really "how far past the end of an array shall we go". Storms and
clouds share these slots; slot 0 is special-cased (`test esi,esi` at
`0x00463563`) and is the one the storm uses.

More storms therefore cannot come from the count. The route, unproven, is to
call `FUN_004BBC80` ourselves with the storm prefab at jittered offsets — the
engine plainly supports several instances per frame, since that is how three
slots become three separate clouds.

### ⛔ Do not scale weather through `0x00725678`

It feeds the fair-cloud scale **and** the ship AI's engagement distances
(`FUN_00467F90` reads `DAT_00725678 * 1000` and `* 0x960`). Scaling weather
through it would quietly change how ships fight.

### The weather curve — `FUN_0045FA70`

The game's own weather-proximity function, and the thing to read instead of
inventing a radius:

```c
int __cdecl StormIntensity(int x, int y, int playSound);   // 0x0045FA70
```

It walks the three weather slots, takes the octagonal distance to the nearest
(with a +2000 penalty on slots 1 and 2, so slot 0 — the storm — dominates) and
returns

```
0x1F400 / (distance + 4000)          // 128000 / (d + 4000)
```

≈32 on top of the storm, 14 at 5,000 map units, 10 at 9,000, 4 far out. **There
is no hard edge to a storm; weather is a gradient.** The result is stored in
`0x0085A0F8` and read by six sites.

✅ **Safe to call with `playSound = 0`** — the trailing write to `0x0085A0F8`
stores back the value it already read, so there is no side effect.

⚠️ **It reads the POSITION arrays, never the cloud's drawn scale.** Making a
storm bigger on screen does not make it hit harder or blow you along faster.

### ⚠️ `0x1F400` is a DROP-SIZE dial, not a drop-count dial

The numerator is an immediate at `0x0045FAF6` (`B8 00 F4 01 00`). Raising it
does **not** give heavier rain — it gives *bigger* rain, and more wind with it,
because the same value feeds the sailing update. Measured:

| Value | Result |
|---|---|
| `300000` | fat white bars that visibly bend with perspective; **27-knot winds** |
| `128000` | vanilla — still large and in your face at the heart of a storm |
| `70000` | smaller, calmer drops, gentler push |
| `45000` | noticeably restrained |

The bend *is* the wind. Genuinely **denser** rain needs the particle count,
which lives elsewhere in `FUN_0047A040` and has not been isolated.

### Storms are seeded once, relative to the player, and never move

```
00463550  MOV  AL, [0x008B996C]      ; a "reseed me" flag
00463561  JZ   skip                  ; ...only when set
00463567  MOV  EAX, [0x008B96B4]     ; player X / 1,000,000
0046356C  ADD  EAX, 0x8              ; eight coarse cells east of you
0046356F  IMUL EAX, EAX, 0x3E8
00463575  MOV  [0x008B98F0], EAX     ; storm X
00463582  MOV  [0x008B98E4], EAX     ; storm Y
00463587  MOV  byte [0x008B996C], 0  ; and clear the flag
```

A storm is **dropped at a fixed world position** and stays there — it does not
drift and does not follow. Sail out of one and you have left it behind.
`0x008B996C` is only ever written **to zero** (here and at `0x00471F3A`);
whatever sets it is a bulk write to a surrounding struct and has not been found.

Drifting or persistent weather would hook in here.

### ⚠️ KNOWN LIMITATION — the cull bound is not scaled

Scaling a cloud grows the visual and **not** its culling sphere, so at high
scale the engine drops the whole cloud while most of it is still on screen.
Reported from play as "it disappears once the storm is 20–30% off screen", and
it reads as the storm teleporting or respawning.

From the Gamebryo 2.6 headers, `NiAVObject` orders its members
`m_kWorldBound` → `m_kLocal` → `m_kWorld`, and `NiBound` is `NiPoint3` centre
plus `float` radius. We know `m_kLocal` sits at **node+0x38** (rotation `+0x38`,
translation `+0x5C`, scale `+0x68` — the field the storm patch writes), so:

| Field | Offset |
|---|---|
| world bound centre | `node + 0x28` |
| **world bound radius** | **`node + 0x34`** |

with `node` being `*(int*)(instance + 0x1C)`.

Not yet fixed. Note that Gamebryo recomputes world bounds during its update
traversal, so writing the radius after the draw may simply be overwritten — the
durable fix is likely scaling the **model** bound once, rather than the world
bound per frame.

### Clusters — the prefab keeps an instance pool

`FUN_004BB4B0` walks a list at `+0x10` and allocates another instance whenever
the current one is already marked used this frame (`+0x24`, set by
`FUN_004BBC80`). That is how three weather slots become three separate clouds,
and it is why **calling the draw again yields another cloud rather than moving
the first**. PEMF builds a squall line by redirecting the `call` at `0x0046377A`
through a shim that issues extra draws at fixed ring offsets before the game's
own — same phase, same ordering, no guessing about render stages.

⚠️ Offsets must be **fixed**. Jittering them per frame makes the front strobe.

### ⛔ The storm POSITION is not ours — measured

`0x008B98F0` / `0x008B98E4` are rewritten by the engine **about ten times a
second**, not once when a flag fires. Measured by intervening and counting:
**122 interventions in one short session, one every ~95 ms.**

So there is no quiet moment in which our value survives. Anything that writes
those addresses fights the engine, and the fight is visible: the storm flickers
between wherever we put it and wherever the game wants it. Holding the position
every tick "wins" the fight and is worse still — it pins the storm to one
coordinate forever, so it can never be retired or re-seeded and weather stops
behaving like weather.

**Storm placement belongs to the engine.** It seeds eight coarse cells EAST of
the player, which is *upwind* — the trade wind in this game blows east to west,
so weather correctly arrives from windward every time. That is not a bug; it is
only conspicuous once the clouds are large.

Anything we want to change about *when* and *where* has to come from what we
already own — the draw call — not from the position.

### Global rain is separate from the cloud's rain

Rain falls across the **whole screen**, including clear sky with no cloud above
it, and draws over the storm rather than under it. It is not emitted by the
cloud: the storm prefab carries its own rain underneath, and this is a second,
global effect scaled by the weather value.

```
0047BC3C  imul eax, dword ptr [0x0085A0F8]     0F AF 05 F8 A0 85 00
```

Seven bytes, where `imul eax, eax, imm8` (`6B C0 imm8`) is three. Replacing it
decouples the screen-wide rain from the weather curve **without** touching
`0x0085A0F8`, which also drives the wind and every intensity threshold. A
constant of `0` removes the global rain entirely and leaves only the rain the
cloud itself carries — which falls under the cloud, where it belongs.

The engine's draw order puts that global rain over the clouds with no depth
test, and reordering engine passes is not available to us. Removing the effect
is.

### Ducking the game's music — `0x0072638C` and `FUN_004D4480`

The game keeps four volume settings in globals and copies them into the live
channel array when its options screen applies them:

```c
FUN_004D4480(void):
    channels[0] = 0x00726388      // sound
    channels[1] = 0x0072638C      // MUSIC
    channels[2] = 0x00726390
    channels[3] = 0x00726394
```

The pairing is **matched, not guessed**: the settings writer at `0x004D260F`
reads `0x0072638C` immediately before pushing the key string `"MusicVolume"`
(`0x0070C948`).

So ducking is: save the player's value, write 0, call the apply — and reverse it
after. ⚠️ Never capture your own zero as "the player's setting", or a storm will
leave the game permanently silent.

### ✅ A storm is a DRIFTING WEATHER SYSTEM — measured

The single most useful measurement in this area, and it corrected three
successive designs built on guesses. Watching the position for 24 seconds
without intervening:

```
pos=(422993,110000)  moved=0
pos=(422636,110000)  moved=347
pos=(421880,110000)  moved=384
pos=(415412,110000)  moved=341
```

**Latitude never changes. Longitude falls ~350 map units per second, steadily.**
A storm is seeded to windward and then blows WEST with the trade wind, passes
over, and recedes.

So the position changing every frame — which an earlier round measured as "the
engine rewrites it ten times a second" and read as constant re-seeding — is
simply the storm **moving**. Treating that movement as "the engine retired this
storm" is what made storms vanish the instant the player sailed toward one: a
few seconds of ordinary drift crossed the threshold.

A genuine re-seed is a large *discontinuous* jump, not 350 units of drift.

### `param_8` is a millisecond timer, not opacity

The last argument to `FUN_004BBC80` is stored as `*(float*)(inst + 0x28) =
param_8 * 0.001`, which has the shape of the 0..1000 fade convention used by
the world-text call. It is not. Measured climbing monotonically ~1000 per
second (3125 → 25681 over 22 s) — it is elapsed time, for animation.

**There is no known opacity argument on this draw.** Scale is the only size or
visibility control, and scale is *size*: ramping it does not fade a cloud in or
out, it inflates one and then shrinks it to a blob. Both were tried and reverted.

### ⛔ NEVER SKIP THE ENGINE'S DRAW CALL

Suppressing weather by simply not calling `FUN_004BBC80` corrupts the scene
graph, progressively, and takes prefabs you never touched with it.

`FUN_004BBC80` is where an instance is **marked used for this frame** (`+0x24`),
and `FUN_004BB4B0` allocates a **new** instance whenever the current one is
already marked. Miss the call and those flags are never cleared, so the next
draw allocates instead of reusing — every frame, for as long as the suppression
lasts. The pool grows without bound, exhausts the scene-node budget it shares
with everything else, and clouds stop appearing **anywhere**.

Reported from play as *"the sky is empty and I never see another cloud again"* —
including the fair-weather prefab, which was never patched.

**To hide a storm, draw it at scale 1** (a hundredth of a unit — invisible) and
let all the bookkeeping run. Extra instances of our own are safe to skip,
because the engine never expected them.

### The draw's ARGUMENTS are ours, even though the position is not

The lesson that unlocked everything else. Writing `0x008B98F0/E4` fights the
engine and always will. But the shim owns every argument passed to
`FUN_004BBC80` — the size **and** the x/y. So PEMF can draw a storm anywhere,
at any size, on its own schedule, without touching a single engine global.

That is how PEMF's weather works now: the engine's storm is drawn at scale 1
(invisible, bookkeeping intact) and our own system is drawn instead — born to
windward off screen, drifting west at the measured rate, living as long as we
choose.

⚠️ Consequence: the drawn storm and the engine's invisible one are in different
places, so **wind still comes from the engine's position** while rain comes from
our cloud. Anything that must agree with what the player SEES has to measure
against our system, not `FUN_0045FA70`.

### Measured limits

Vanilla storm scale is 80. Verified in game up to **700 (8.75x)** with height
raised to 1100, still reading as a defined storm with a visible edge. Size and
height fight each other: a large cloud left at a low altitude swallows the
camera and reads as fog rather than weather — at scale 1000 with height 320 the
whole screen washed to a pale haze with no visible cloud at all. **Raise the
height whenever you raise the scale.**

Sound is already loose on disk and named by intensity: `Storm.wav`,
`StormBoom.wav`, `StormYell1/2/Neg-NNN.wav`, and the cues `NAV_THUNDER_LOW` /
`_MEDIUM` / `_HIGH` (`0x0070B814`).

**Purely visual.** Whether a storm does more to your ship — damage, crew, speed
— lives somewhere else and has not been located.

## Making ships — the factory

The engine has no way to make an existing vessel hostile: **nothing anywhere
reads the player's nationality field**, checked exhaustively. When a nation
decides you are a problem the game does not flip a bit on a passing merchant —
it *builds a ship and sends it*. That is what a pirate hunter is, what a
privateer is when two crowns go to war, and what a governor's blockade fleet is.

So the primitive worth having is not a hostility switch. It is the constructor.

### ⛔ `+0x08` is a pursuit target — and it does NOT accept the player

There **is** a per-ship "chase that vessel" field, and it is the only thing in
the record that overrides the war-relations check. It is still useless to us,
and the measurements below are here so nobody spends another day on it.

`0x00814300`, int16, offset `+0x08`. Six references in the whole binary, in two
functions. The engagement test at `0x0046A4FC`:

```
relations[them][me] == 1 && ((purpose == 2 && ...) || purpose == 3)
|| ship[me].target == them          <-- this arm ignores relations entirely
```

The engine writes it in exactly **one** place — `0x0044EF0D`, inside the convoy
builder `FUN_0044EB10(city, homeCity, shipType, escortNation, kind)` — and
always with a slot allocated moments earlier in the same call:

```c
if (param_4 != -1 && param_5 != 2) {          // escortNation given
  do { iVar8 = AllocShip();
       ship[iVar8].target  = iVar7;           // the ship built at the top
       ship[iVar8].purpose = 2;               // privateer
  } while (local_14 <= DAT_0085A158 / 2);
}
```

That is convoy escorts and hunting packs. **It is never pointed at slot 0.**

**Measured, three runs, 2026-08-02.** Slot 0 is both "the player" and the AI's
"no target" sentinel (`if (0 < target)` guards every read; stale ones cleared at
`0x00469F40`). Writing `target = 0` persists perfectly — read back on every one
of ~25 samples, never wiped — and changes **nothing**:

| | `target = 0` | `target = -1` (control) |
|---|---|---|
| spawn distance to player | 2,629 | 2,328 |
| closest approach | 1,359 | 882 |
| distance at end | 6,601 | 6,828 |
| distance from home port | 867 → 8,274 | 1,025 → 8,739 |
| progress to ordered port | steady | steady |

Identical. Both sailed to their ordered port and left the player behind, both
passing within the `dist < 0xBB8` window that the engagement test requires. The
sentinel wins: a target of `0` reads as *no target*.

**And the battle handshake excludes the player anyway.** Two engagement paths
exist. The relations-based one is wrapped in `if (other != 0)` — slot 0 skipped
explicitly. The other (`0x0046A61E`, which writes `+0x5C = 0x96`, `+0x60 =
other` and flags `|= 0x60` reciprocally) has no such guard, but is reachable
only through the target match above.

The player *is* seen — the neighbour scan at `0x00469DF6` starts at slot 0 or 8
depending on a bit in `0x0085A164`, and `0x00469DFB` explicitly permits slot 0
as a candidate. That scan is what produces `"Stay clear you scurvy Pirate!"`
(`0x00707A40`, hailed from `FUN_004612B0`). Being seen and being *engaged* are
different things.

**Conclusion: overworld ships never initiate combat with the player.** Contact
does. A pursuer's job is to make contact unavoidable, not to open fire.

### ⚠️ Ships far from the player are culled within ~2 seconds

Measured during the same runs, and it invalidates any "spawn a ship over there"
plan:

| Spawn distance to player | Outcome |
|---|---|
| 2,328 – 6,315 | lived as long as observed (25 s+) |
| 11,023 – 15,492 | **culled in 1.8 – 2.6 s** |

A ship also dies if it has *arrived*: the factories set destination to the port
of origin, so a spawned ship with no fresh destination docks and is removed in
about two seconds. Both failure modes look identical from the outside — the ship
simply is not there any more — which is why they were confused for each other
twice during this work.

### Slot allocation — `+0x00 == -1` means free

| Slots | Use |
|---|---|
| 0 | the player |
| 1–7 | reserved |
| 8–255 | the AI pool |

The allocator walks from slot 8 looking for a record whose **type word at
`+0x00` is `-1`**, and stops at `0x00859EF8` — which is `0x008142F8 + 256 *
0x45C`, the third independent statement that the array holds 256.

### `FUN_00414FC0(cityIndex, kind)` — allocate and build

**`__cdecl`, two args, returns the new slot index or `-1`.** Caller-cleaned;
seen at `0x0040DA9A` as `push 0xB / push ecx / call`.

Finds a free slot, zeroes the whole `0x45C` record, sets flags `|= 0x800`, and
fills in the ship. This is the clean entry point — prefer it.

### `FUN_00414D00(cityIndex)` — the same job, one arg

**`__cdecl`, one arg, returns index or `-1`** (`0x0040DB62`).

### There is a family, and the builder decides the role

A ship built by `FUN_00414FC0` turns straight round and sails home, because it
comes out with **role 0** — ordinary traffic with no orders. Role is not a field
to patch afterwards; it is chosen by *which builder you call*, and each one sets
its ship up completely.

| Function | Args | Role | Flags | Nationality |
|---|---|---|---|---|
| `FUN_00414FC0` | `(city, kind)`, cdecl | 0 | `0x800` | from the city |
| `FUN_00415290` | `eax = city`, type on stack | 4 | `0x200` | from the city |
| `FUN_004154F0` | `(type)`, cdecl | 3 | `0x1C00` | hardcoded `0`, port from `0x00722A08` |

All three are verified callable in a running game — signature check passes, a
slot is consumed, flags come out distinct per builder. `FUN_00415290`'s mixed
convention needs a naked shim; no compiler emits it.

### ✅ `+0x02` is what a ship IS — and 1 is *pirate-hunter*

The overworld hover label gives this away completely. At `0x00462098`:

```
movsx eax, word [edi + 0x8142FA]      ; +0x02
dec   eax
cmp   eax, 3
ja    no_label
jmp   dword [eax*4 + 0x00463CB4]      ; four-entry jump table
```

and the table resolves, in order, to the four strings at `0x00707A60`–`AA4`:

| `+0x02` | Hover label |
|---|---|
| 0 | *(none — gets the plain `@NATIONALITY @SHIPTYPE '@SHIPNAME'`)* |
| **1** | **`@NATIONALITY pirate-hunter`** |
| 2 | `@NATIONALITY privateer` |
| 3 | `@NATIONALITY raider` |
| 4 | `@NATIONALITY smuggler` |
| 5, 6 | written by the game, no label — `6` comes out of two builders |

**This is the classification the game actually uses**, and it is *not* the field
at `+0x2A` that three test rounds here called "role". A spawned ship showing no
hover text is the symptom: the builders leave `+0x02` at a value the label code
does not name.

✅ **Verified in game.** A ship stamped `+0x02 = 4` hovered as `English
smuggler`; `= 1` hovered as `English pirate-hunter`.

### ✅ The game's own pirate-hunter dispatch — `0x0045F060`

Reproduced exactly (there is a duplicate at `0x00465670`):

```
slot = FUN_00414FC0(city, kind)         ; the same factory PEMF calls
rep  = reputation[CityNation(city)]     ; your standing with that crown
str  = 2 - rep/5                        ; imul 0x66666667 / sar 1  (/5, NOT /10)
ship[slot].purpose = 1                  ; PIRATE-HUNTER
clamp str to [2, 4]
```

**The worse your standing with a nation, the stronger the hunter it sends.** That
one line is the entire relationship between reputation and being pursued, and it
means a system that drives reputation is turning a dial the game already reads
rather than inventing a parallel one.

The `kind` argument is indexed out of a table at `0x007252D0`.

### And the behaviour — `0x0046A345`

```
mov  eax, [ebp + 0x814350]      ; flags
test al, 8                       ; must have bit 0x8 to be in this path at all
je   skip
test ah, 0x41
jne  skip
cmp  word [ebp + 0x8142FA], 1    ; a pirate-hunter?
je   skip                        ; <- EXEMPT
```

Ordinary vessels take a branch that **steers them away from the player**;
purpose `1` is exempted from it. So "hunting" is not a pursuit routine — it is
the *absence* of the avoidance every other ship has, in a ship that is already
near you.

⚠️ Note `test al, 8`. A ship needs flag `0x8` to enter this path, and none of the
three builders sets it. That is the most likely reason a hand-stamped hunter
sits still.

Values are written at: `0x0040DACA` (3, governor's dispatch), `0x00413FD0` (4),
`0x0041412A` (2), `0x00414093` (3 — the raider branch, gated on the game-progress
value at `0x0085A150` exceeding `0xBB8`), `0x0041535D` (2), `0x00414B3C` and
`0x004155B6` (6).

### ✅ The lever is the DESTINATION, not the role

Both builders hand back a ship whose **destination city equals the port it was
built at**. It has arrived. That — not a missing role, not missing flags — is
why the first spawned ships turned straight round or sat at anchor: they were
doing exactly what a ship with nowhere to go does.

Write a different city into `+0x3E` and **the ship sails for it**. Verified in a
running game across all four role values; every one of them travels once it has
somewhere to be, so role is not what gates movement.

```
slot 20 AS BUILT -- ROLE 4, dest city 0, home city 0        <- already there
slot 20 ORDERED -- role 1, dest city 72 (Spanish, 87828 away)  <- sails
```

**What this makes possible:** build a vessel of any nation, at any port, and
send it anywhere. A pirate hunter is a ship built at the offended crown's
nearest port with a destination on the far side of the player. No hostility flag
required — and there is none to find, so this is the mechanism, not a
workaround.

Roles 1 and 2 cannot be produced by any callable builder — they are written only
inside the encounter spawner and the governor's dispatch. They can be **stamped**
after the fact, which is how their behaviour was tested at all. Patching role on
a ship built by the wrong builder would otherwise be a bad idea: the remaining
fields would belong to a different kind of vessel.

⛔ **`FUN_004154F0` reads its home port from `0x00722A08` and does not validate
it.** In a fresh career that global is unset, and the builder cheerfully makes
ships at a null settlement — measured, three of them at map position `(1,2)`,
the corner of the world, and a handful of those crashed the game. Check the
global before calling; PEMF refuses when it is out of range.

### `FUN_004135F0` — the low-level initialiser

Takes its arguments **in registers** (`ebp` = slot index, `edx` = city index,
returns `eax = ebp`), so it needs a naked shim. What it does is worth knowing
even if it is not what you call:

```
memset(ship, 0, 0x45C)                   ; rep stosd, 0x117 dwords
flags |= 0x10
+0x18 = 0x12C
+0x4C = 0xFFFF
if (cityIndex != -1) {
    +0x3E = cityIndex                     ; home city
    +0x04 = CityNation(cityIndex)         ; NATIONALITY COMES FROM THE CITY
    +0x0C, +0x10 = city position * 1000   ; spawned at that city
    +0x5C = 1
    flags |= 0x200
}
```

⚠️ The exact entry point needs pinning before this is called — the bytes just
before `0x004135F8` disassemble ambiguously. Use `FUN_00414FC0` instead unless
there is a reason not to.

### Ship record fields recovered so far

| Offset | Global | Meaning |
|---|---|---|
| `+0x00` | `0x008142F8` | ship type, word. **`-1` = free slot** |
| `+0x04` | `0x008142FC` | nationality, word — the colours she is seen to fly |
| `+0x0C` | `0x00814304` | X, milli-units |
| `+0x10` | `0x00814308` | Y |
| `+0x02` | `0x008142FA` | **purpose** — what the ship *is*. See below. |
| `+0x2A` | `0x00814322` | a second classifier, values 1–4, meaning unknown |
| `+0x3E` | `0x00814336` | a city index (destination) |
| `+0x40` | `0x00814338` | a city index (origin) |
| `+0x58` | `0x00814350` | flags |

⚠️ Those three offsets were first written down as `+0x22`, `+0x36` and `+0x38` —
plain subtraction errors against the `0x008142F8` base. The first live spawn test
consequently reported *every* ship as role 0, including two built by functions
that demonstrably write 4 and 3. The flags read correctly throughout, which is
what showed the calls were fine and the readout was not. **Derive offsets by
subtracting, then check the arithmetic against a field whose value is already
known.**

Flag bits seen written: `0x10`, `0x200`, `0x800`, `0x1400`, `0x8000`, `0x28000`,
`0x400000`, `0x800000`. ⛔ `0x400000` is **written in five places and read in
none** — it is not a hostility bit, whatever it is. One of its writes happens
after a yes/no dialog returns "no", so "player declined, do not re-offer" is the
better guess.

### `FUN_00413710` — the encounter spawner

The function that puts traffic in front of you. It:

1. reads the player's position, divides by 1000, and offsets Y by `+500` —
   a point just ahead of the ship;
2. picks a free slot (two pools: `108 + rand%4` for one case, `8 + rand%0x78`
   otherwise);
3. picks a random city (`rand % 0x2C`), rejects it unless it is within `0x73` of
   that point and its flags pass;
4. builds the ship and **assigns the role field** at `+0x22`.

Every one of the role assignments in the binary lives inside this function or
the governor-mission code, which is why the role values are worth mapping: they
are the vocabulary for "what is this ship doing". Role `2` is set by the
governor's blockade dispatch at `0x0040DAD3`, alongside the message *"I am about
to send the Brig '@SHIPNAME' to blockade @CITYNAME."*

**Not yet established:** what roles 1, 3 and 4 mean, and which role (if any) is
"hunt the player". No consumer of `+0x22` has been found — every access through
the absolute form is a write, so the readers use a register base.

### Screen state also answers "am I in a career"

The same `ScreenId`/`ScreenDepth` pair that gates notice drawing. **Depth alone
is enough for career presence**, and it is the only reliable source:

| Depth | Screen |
|---|---|
| 1 | main menu |
| 2 | character creation |
| 3 | sailing, town |
| 4 | Load/Save |
| 4–5 | battle |

⛔ **Do NOT use `crew > 0` for this.** The crew count does **not** return to zero
when the player leaves a career, so anything built on it works exactly once per
session and then silently stops seeing transitions. That mistake made every
career after the first inherit the previous one's state. Measured; full account
in [`re/experiments/career_state/`](../re/experiments/career_state/README.md).

### ⛔ A save file being read does not mean a save was loaded

**Starting a brand new career makes the game read a save file**, at the same
distance in time as a genuine load, with nothing about the access to tell them
apart. The Load/Save screen also opens every save it can see just to list them.

So file access cannot answer "was this loaded or is it new". PEMF stamps a
fingerprint of the career into each sidecar and compares it against the live
game instead. Same write-up.

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

## The notice strip — the game's own notification line

> **Confidence: verified in-game.** PEMF posts to this strip in the shipping
> build; the screenshots show our text on the game's own parchment.

The torn-paper strip along the bottom of the sailing screen — where the game
says *"Pirate attack on the city of Nevis defeated by local militia."* — is
**two plain globals and no function call at all**. It is the cheapest thing in
this document to use and the safest: no hook, no stack, no re-entrancy.

### The two globals

| Address | Type | What it is |
|---|---|---|
| `0x00876BF8` | `char[256]` | the strip's text buffer |
| `0x008CA9E8` | `uint8` | visibility counter — drawn while non-zero |

`0x008CA9E8` is a **countdown, not a timestamp**. The game's own post sets it to
**100** and `FUN_004612B0` decrements it once per update tick:

```asm
004633B3  MOV  AL, [0x008CA9E8]
004633B8  TEST AL, AL
004633BA  JZ   done
004633BC  DEC  AL
004633BE  MOV  [0x008CA9E8], AL
```

### Capacity is 256 bytes — measured, not assumed

The next address any code references above the buffer is `0x00876CF8`, exactly
`+0x100`, with nine referents of its own. Scan for it with:

```bash
# 4-byte LE operands in .text falling just above the buffer
python re/scripts/xref_scan.py   # same technique as callers_of.py
```

**The game does not respect this bound.** `FUN_004600A0` `strcpy`s in from the
4096-byte compose buffer with no length check — a latent overflow in the
original. PEMF caps at 200 and refuses rather than truncating into a global.

### Who draws it

`FUN_004741B0` is the **overworld sailing HUD** — it also draws Fame, `@NUM
knots`, `@NUM crew` and the date. Per-frame and non-blocking:

```c
if (DAT_008CA9E8 != 0) {
    tex = FUN_004CDDF0("tornPaper.dds", 0, 0, &w, &h);
    FUN_004AF080(tex, 0, 0, w, h, x, y, ...);
    FUN_004B06C0(&DAT_00876BF8, x, y, 0x32, 0xFF1A3E1A, 1, 200, 0);
}
```

`0xFF1A3E1A` is the dark green the strip's text is drawn in.

### The game's own post — `FUN_004600A0`

```c
void FUN_004600A0(void)
{
    FUN_004879F0(0x20, 0);        // WrapText to 32 columns
    strcpy(0x00876BF8, 0x00869B48);
    DAT_008CA9E8 = 100;
}
```

Called at `0x0044E91B`, right after the news composer finishes. **The `0x20` is
the wrap width** — 32 columns is the strip's own break point, so text wrapped to
anything else will not line up with the game's.

### There is no queue — this is the important part

The strip is **one slot, last-write-wins**. Posting overwrites whatever is on it
mid-display, with no way to recover it. Write blind and you will eat the game's
own news: a player would simply never learn their city had been raided.

Anything sharing the strip must therefore **gate on `0x008CA9E8 == 0`** and hold
its own queue. PEMF does this in `content::PumpNoticeStrip()`, pumped from the
safe point so the "is it free" question is asked once per frame.

### Doing it without calling the game

Because the API is two globals, the whole thing is:

```c
// text first, counter LAST -- the HUD reads both every frame, and setting the
// counter first exposes one frame of whatever the buffer still held.
memcpy((void*)0x00876BF8, text, len + 1);
*(unsigned char*)0x008CA9E8 = ticks;
```

`FUN_004600A0` itself is **not** worth calling: it would re-wrap whatever is
currently in `0x00869B48` and clobber our text.

---

## `FUN_004888E0` is AUDIO — a trap that has caught us twice

> **Confidence: proven by the function's own error string.**

`FUN_004888E0` sits immediately after the news composer's text work and looks
exactly like a presenter. It is not. Its own diagnostic string at `0x00709E20`
is:

> `"The audio manager has not been properly initialized yet"`

logged when the vtable predicate `[[0x008ECD78]+0x18]` (audio-manager-ready)
returns 0. On the call shape:

```asm
PUSH -1.0f ; PUSH -1 ; PUSH -1 ; PUSH <n>
OR ESI,-1  ; XOR EAX,EAX ; OR ECX,-1
CALL 0x004888E0            ; then ADD ESP,0x10
```

`<n>` is a **sound id**, not a presentation style — `0x2A` is the news sting,
`0x39` the card's. The single `float` is volume/pan; the `-1`s are channel and
priority defaults. The `CALL 0x00412810; CMP EAX,0x1C; JGE skip` guard around
the news call is *positional* audio: no sting for a city across the map.

`FUN_00488A80` next door is likewise audio (the spoken-callout entry point).

**Rule for the `0x4888xx`–`0x4889xx` range: read the function's own format and
error strings before naming it.** Two byte-identical call sites differing in one
integer prove nothing about what that integer means.

### Related text-layout helpers

| Address | What it actually is |
|---|---|
| `0x00509500` | **Text measurement.** `(font, str, stopAtNewline)` — walks glyph widths off the font object at `+0x44`, handles `\n` and `<` markup, returns max line width as `float10`. Presents nothing. |
| `0x005F0DDC` | CRT `float`→`int64` **round** helper (consumes ST0). Not a timer. |
| `0x00487E90` | **Fit-to-width.** Measures the buffer, then while the result is narrower than the available width *and* runs to more than 3 lines, widens the `WrapText` column by 2 and retries. Finds the narrowest wrap that fits in three lines. |

---

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
| `0x004A06C0`, `0x004A07C0`, `0x00528E70`, `0x00528EE0` | fastcall | **Game-logic entries.** Thin wrappers: check the manager is initialised, then call `0x0052F700`. These are its only callers (confirmed by xref, 2026-08-06). |
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

## The town menu — adding our own options

> **Confidence: read from the disassembly, not yet run.** Mapped 2026-08-06 with
> live Ghidra. Every address below was read off the listing; the two open
> questions at the end are marked as open rather than guessed.

This is the groundwork for PEMF adding its own rows to the game's existing
menus. It is **not** the same problem as an event card, and it is much the
easier of the two:

| | Event card | Town menu |
|---|---|---|
| Renderer | `FUN_00430190`, **blocking** | form `10`, **polled** |
| Shape | spins the game's own pump until a choice is made | renders one frame, returns `-2` for "nothing yet", expects an outer loop |
| Consequence | cannot be presented from inside a frame hook — see [Drawing](#drawing-our-own-text--solved-and-how) | no open-scene problem at all |

The whole town menu lives in **`FUN_00410D30`** (`0x00410D30`–`0x004126BB`).

### How the menu is composed

Prose first, then options, exactly like PEMF's own cards:

| Address | Call | Role |
|---|---|---|
| `0x00411658` | `AddText0` (`0x004F6090`) | the town description |
| `0x00411683` | `WrapText` (`0x004879F0`) | wraps the prose only |
| `0x00411677`, `0x004116AE`, `0x004116C3`, `0x00411729`, `0x004117A0`, `0x004117BD`, `0x004117D4`, `0x004117E8`, `0x00411815`, `0x0041185B`, `0x004118A8` | `AddText1` (`0x004F60B0`) | one selectable row each |
| `0x0041191E` | `ShowMessage` (`0x00410C50`) | presents; returns the picked index |

**The option sites are conditional, and mostly alternatives to each other.**
Which ones run is decided by a jump table at `0x004126E4` (dispatched at
`0x0041169D`). Three of them are the settlement-type variants, and each pushes
**one multi-line string containing every option at once**:

| Site | String | Contents |
|---|---|---|
| `0x004117E8` | `0x006F5B50` | `\nDo you...\n Talk to the Mayor\n Visit the Tavern\n Trade with the Merchant\n Consult with the Shipwright\n Divide the Plunder\n` |
| `0x004117D4` | `0x006F5BCC` | `\nDo you...\n Talk to the Chief\n Trade with the Chief\n` (village) |
| `0x004117BD` | `0x006F5C04` | `\nDo you...\n Talk to the Abbot\n Trade with the Abbot\n` (mission) |
| `0x0041185B` | `0x006F5B40` | `" Check Status\n"` — appended after the block above |
| `0x004118A8` | `0x006F5AEC…` | the leave row, by settlement type |

### The full row → action id table

Combining the strings with the jump table at `0x004126F4`:

| id | Row | Target |
|---|---|---|
| 0 | Talk to the Mayor / Chief / Abbot | `0x004119A9` |
| 1 | Visit the Tavern | `0x00411CA1` |
| 2 | Trade with the Merchant / Chief / Abbot | `0x004122E7` |
| 3 | Consult with the Shipwright | `0x004119E9` |
| 4 | Divide the Plunder | `0x00411BC4` |
| 5 | Check Status | `0x00411BDC` |
| **6+** | **Leave** | `0x00411BF4` (via the `JA`) |

Two independent confirmations that this table is right:

- id 4's target calls `0x004102A0`, and `0x00410321` — the known "divide the
  plunder" site already recorded in this file — is **inside** that function.
- The remap now makes sense arithmetically. A village shows two rows,
  `0 = Talk`, `1 = Trade`. The remap adds 1 when the index is 1, turning the
  village's row 1 into **id 2 = Trade**, which is exactly right. It adds 3
  otherwise, mapping a third compacted row onto id 5 (Check Status). The
  "5 or 6" in `[EBX+0x860B80]` is the settlement type — village and mission.

### ⚠️ The returned index is remapped before it is used

This is the trap, and it is the reason "just append a row and match on its
index" would misfire. Immediately after `ShowMessage`:

```
00411923  MOV  ESI, EAX                    ; ESI = the row the player picked
00411925  MOV  EAX, [EBX + 0x860B80]       ; a town field
0041192B  CMP  EAX, 6
0041192E  JZ   0x00411935
00411930  CMP  EAX, 5
00411933  JNZ  0x00411947                  ; not 5 or 6 -> no remap
00411935  TEST ESI, ESI
00411937  JLE  0x00411947                  ; index 0 is never remapped
00411939  XOR  ECX, ECX
0041193B  CMP  ESI, 1
0041193E  SETNZ CL
00411941  LEA  ECX, [ECX + ECX*1 + 1]      ; ECX = 1 when ESI == 1, else 3
00411945  ADD  ESI, ECX                    ; ESI += 1 or 3
```

So when `[EBX+0x860B80]` is **5 or 6**, some rows are absent from the display and
the game **maps the compacted on-screen position back onto a canonical action
id**. The value `ShowMessage` returns is a *row position*; what the dispatch
consumes is an *action id*, and in those town states they are not the same
number.

**Consequence for PEMF:** an appended row's position is not a stable identifier.
Our own rows have to be identified by where we put them relative to the game's
count *at composition time*, and any interception has to happen with the same
remap applied — or before the remap, on the raw return.

### The dispatch — and why "out of range" is not a spare slot

```
00411999  CMP  ESI, 5
0041199C  JA   0x00411BF4                  ; anything above 5 -> here
004119A2  JMP  dword ptr [ESI*4 + 0x4126F4]
```

A six-entry jump table at **`0x004126F4`** covers ids `0`–`5`. The comparison is
**unsigned**, so anything above 5 — and any negative value, the polled form's
`-2` included — goes to `0x00411BF4`.

⛔ **`0x00411BF4` IS NOT AN ERROR HANDLER, AND LANDING ON IT IS NOT HARMLESS.**
This was assumed on a first reading and it is wrong. Traced:

- It has **exactly one xref** — the `JA` above. Nothing else reaches it, and it
  is not a jump-table target.
- The **last option row the menu composes** is a single `AddText1` call at
  `0x004118A8` reached by **five** different pushes:

  | Push site | String | |
  |---|---|---|
  | `0x0041186C` | `" Sail away\n"` (`0x006F5B34`) | **the ordinary colonial case — seen in game** |
  | `0x00411889` | `" Leave Town\n"` (`0x006F5AEC`) | |
  | `0x00411890` | `" Leave Village\n"` (`0x006F5AFC`) | |
  | `0x00411897` | `" Leave Mission\n"` (`0x006F5B0C`) | |
  | `0x0041189E` | `" Leave Pirate Haven\n"` (`0x006F5B1C`) | |

  ⚠️ An earlier version of this section listed only the four `" Leave …"`
  strings and implied one of them was always the leave row. A playtest showed
  **"Sail away"** in an English settlement, which is why anything keying off the
  leave row's *text* would be wrong. Anchor on the last option **line**, not on
  a known string.
- `0x00411BF4` is therefore **the departure sequence**: leaving is simply the row
  whose id is above 5, and the `JA` is how the game routes it — not a bounds
  check that happens to be safe.
- That path then offers the tavern recruits:
  `'Captain, there's a group of @NUM men back at the tavern eager to join our
  crew.'` (`0x006F5A98`) with `" Let's sign them up.\n No matter, cast off!\n"`
  (`0x006F5A68`), gated on crew room and a few world checks.

**So an appended row would be read as "leave town".** A PEMF option added after
the game's own rows takes an id above 5, falls through the `JA`, and the player
watches themselves walk out of the settlement — a wrong behaviour that is
visible, plausible-looking, and would be maddening to attribute.

The earlier note in this file said an extra row "cannot crash the dispatch on its
own". That is still true and is also beside the point: it will not crash, it
will do something else entirely.

`FUN_00410D30` has two further jump tables: `0x004126BC` (dispatched at
`0x00411480`) and `0x004126E4` (at `0x0041169D`, deciding which options compose).

### What this means for the hook

PEMF cannot let its own rows reach the dispatch. The index has to be intercepted
**between `ShowMessage` at `0x0041191E` and the bounds check at `0x00411999`**,
which is a nine-instruction window that also contains the remap.

The tractable way in is to **redirect the `CALL 0x00410C50` at `0x0041191E`** —
a single 5-byte call site, the same technique already proven on the storm draw
at `0x0046377A`. A shim there can compose PEMF's extra rows immediately before
calling the original, then inspect what the player picked and either handle it
as ours or hand the game back a value it would have produced itself.

### ⛔ There is no "nothing happened" value to return

This was the last open question and the answer is that the premise was wrong.
Every possible return is meaningful:

- `0`–`5` each dispatch a **real action** (see the table above).
- Anything above `5` **leaves the settlement**.
- Negatives do **not** help: the compare is `CMP ESI,5` / `JA`, which is
  **unsigned**, so `-1` and the polled form's `-2` are both "above 5" and leave
  the settlement too.

So the shim cannot pick a harmless sentinel — there isn't one.

**The shim must own a loop instead.** When the player picks a PEMF row, the shim
handles it and then *re-presents the menu itself*, calling the original
`ShowMessage` again, and only ever returns a genuine game row id to
`FUN_00410D30`. The game never sees an index it did not produce.

That needs the composed message preserved across the re-show, because
`ShowMessage` consumes the buffer: snapshot `PGA_MSGBUF` (`0x008E9F58`) before
the first call and restore it before each re-show. PEMF already composes into
that buffer, so this is existing ground rather than new.

One consequence worth stating before it is designed around: a PEMF row that
*should* close the menu (say, "Sail on") cannot be expressed by returning the
leave id and hoping — it would have to fall through to the real leave path
deliberately, which is `6`+ and therefore already available. That direction
works; it is only "do nothing" that has no representation.

### Where the polled form comes in

`0x004119DF` calls `ShowMessage` with `EAX = 0xA` — form 10, the non-blocking
one. It sits inside a dispatch target, after a float at `0x008CAC44` is set to
`1.0f` or `2.0f` depending on whether the town field at `0x00860B74` is 5 or 6.

### Where PEMF's rows should sit

Between **Check Status (id 5)** and the leave row. That keeps every game id
stable, keeps "leave" last where players expect it, and means our rows occupy
positions `6…n` on screen — all of which the game would route to the departure
path, and all of which the shim intercepts before it can.

### Confirmed in game (2026-08-06, Steam)

A PEMF row composed, placed directly above the leave row, picked, and fired its
event, with every game option keeping its position. Two things the playtest
corrected:

- The leave row read **"Sail away"** — see the five-string table above.
- **"Divide the Plunder" can appear greyed out and unselectable** (observed with
  no plunder aboard). It is drawn as a row but is not one. Our row still
  resolved correctly, so the engine's numbering and ours agreed here — but this
  has **not** been proven in general, and it is the most likely source of an
  off-by-one if PEMF rows ever land on the wrong action. Worth reading the
  greyed path properly before relying on it.
- "English **Village** of Antigua" showed the full Governor/Tavern/Merchant/
  Shipwright menu, so "Village" in the description is a **size**, not the
  settlement type behind the 5/6 remap. That remap remains untested; Indian
  villages and missions are the cases to try.

### The backdrop is a scene, with named cameras

`FUN_004108A0(city)` — the colonial branch of `FUN_00410BF0` — does not load a
flat image. It builds a scene:

```c
FUN_004F2600("tm_%c_menu.nif", letters[kind]);        // scene, per nation
... Tm_%c_settlement.dds  or  Tm_%c_townback_%s.dds   // and its dressing
FUN_004BAAA0(0, "transparent.tga", ...);
FUN_004AE5B0("Camera01");                             // a NAMED camera
FUN_00533D10(0, 1);
FUN_00412840(1.0f, 100000.0f);                        // near / far
DAT_00722738 = city;
DAT_008CA904 = 1;                                     // "a backdrop is up"
```

Two things worth having:

- **Cameras are selected by name.** `FUN_004AE5B0` takes a string, and the town
  backdrop asks for `"Camera01"`. If the `.nif` files carry more than one
  camera, other shots of the same port are a string away. **What cameras exist
  has not been checked** — that needs reading the `.nif`s out of the archives
  (`tools/fpk.py`, see [`ASSETS.md`](ASSETS.md)), not the executable.
- **The backdrop is refcounted and cached.** `FUN_004108A0` returns immediately
  when `DAT_008CADEC` is already set, and `ShowMessage` releases it on the way
  out. So a second `ShowMessage` on the same screen rebuilds rather than
  leaking — which is the answer to whether PEMF re-presenting a menu thrashes
  it. It does not.

### Cards with a port behind them

A card presented in a settlement needs that settlement drawn behind it, and
`FUN_00430190` cannot do it — it composites over whatever the 3D scene is, which
in port is the overworld coastline. Playtested: right text, right port name,
completely wrong place.

The town backdrop is **not a scene**. It is background art drawn by
`ShowMessage` (`0x00410C50`), which calls `FUN_00410BF0` and then hands off to
the same `FUN_00430190` we already use:

```c
FUN_00410BF0(ecx);                        // draws the backdrop
uVar3 = FUN_00430190(w/12, h/24, sound, edx);
if (DAT_008CADEC) { ...release the art... }   // consumed, once
```

⚠️ **`ecx` is the CITY INDEX, not an index into an art table** — this file said
the latter for a long time and it is wrong in the way that matters.
`FUN_00410BF0` reads the settlement's own type from its record and picks from
that:

| Type | Art |
|---|---|
| 0–4 | `FUN_004108A0(city)` — the colonial backdrop, built from the settlement's own data |
| 5 | `native_back.dds` |
| 6 | `mission_back.dds` |
| else | `Tm_f_townback_rich.dds` |

**The call to imitate** is the game's own in-town narrative card at
`0x00411C91`, and every register matters:

```
00411C82  MOV ECX, [ESP+0x458]   ; the city index
00411C8C  OR  EAX, 0xFFFFFFFF    ; form -1, message box
00411C8F  XOR EDX, EDX           ; flags ZERO -- not the menu's flags
00411C91  CALL 0x00410C50
```

PEMF routes this through `game::g_portCardCity`, set only while presenting
inside a settlement, so an event's card *and* its outcome both pick it up — they
use different helpers but both land on `PresentDialog`.

### ⚠️ A menu holds the safe point open

The town menu runs the game's own nested pump, so PEMF's safe point keeps being
reached **while the world behind the menu is not being drawn**. An event fired
from in there presents its card over an empty background — measured: the card
came up 2ms after the row was picked, on a flat blue screen.

This is not re-entrancy; nothing is nested that should not be. The queue is
simply being drained at a moment when the screen is not what the player is
looking at. `events::Suspend()` / `Resume()` exists for exactly this, and
`townmenu` holds it for as long as the menu is up.

Anything else that presents from inside a game menu will meet the same problem.

### Still open

Nothing blocking. Remaining unknowns are cosmetic or belong to the
implementation:

- **`0x00860B74` is the settlement's KIND**, one byte, records **`0x20` apart**
  — read at `0x00411CA5` and by `FUN_00410BF0`:

  | Value | Meaning |
  |---|---|
  | 0–4 | a colonial settlement. The value indexes a five-entry letter table at `0x006F5FFC` used to build `tm_%c_menu.nif`, so **these are the five nations**. |
  | 5 | Indian village (`native_back.dds`) |
  | 6 | Jesuit mission (`mission_back.dds`) |

- ⚠️ **`0x00860B80` is a DIFFERENT field, and an earlier note here conflated the
  two.** It sits in the same record (`+0xC`, addressed as `int[city * 8]`) and
  has been seen holding **5, 6 and 7** — `FUN_004108A0` tests it against `7` to
  pick `Tm_%c_settlement.dds`. **It is `0x860B80`, not `0x860B74`, that the town
  menu's index remap keys on**, so the earlier claim that the remap keys on the
  village/mission kind is *not established*. Both fields take small integers in
  overlapping ranges, which is exactly how they came to be confused. Treat the
  meaning of `0x860B80` as open.
- `0x00860B7C` (`int[city * 8]`) is divided by `0x32` to choose between two
  suffixes for `Tm_%c_townback_%s.dds`, so it is a population or size figure.
- The five option sites at `0x00411677`, `0x004116AE`, `0x004116C3`,
  `0x00411729`, `0x004117A0` (strings `0x006F5D78`, `0x6F5CE8`, `0x6F5CD0`,
  `0x6F5CB4`, `0x6F5CA0`) are other menu variants — pirate haven and the
  special cases — and have not been read. They matter only when PEMF wants rows
  in those menus too.

**Answered:** what `0x00411BF4` does, where to hook, the full row→id table, and
why the shim must loop rather than return a sentinel.

---

## Crew morale — how the engine actually models it

> **Confidence: decompiled 2026-08-06 with live Ghidra.** The arithmetic below is
> read off `FUN_00404810`. What each input *means* is partly inferred and is
> marked where it is.

### ⚠️ Morale is COMPUTED, not stored

This is the single most important fact for anything built on top of it. There is
no morale variable to read or raise. `GetMoraleLevel` (`0x00404810`) derives a
0–4 level every time it is called:

```c
int GetMoraleLevel(void)
{
    int expect = DAT_00869A76 - 4 + DAT_0085A158;
    expect = expect * expect;                     // squared
    expect = (expect / 4) - 4 * DAT_00869B27;     // the writable lever
    if (expect < 1)   expect = 1;
    if (expect > 999) expect = 999;

    int level = ((DAT_00869AB4 + 500)
                 / (0x14 + DAT_00869AB0 - ((DAT_00869B34 & 0x80) ? 19 : 0)))
                / expect;

    if (level < 0) return 0;
    if (level > 4) level = 4;
    return level;
}
```

Read plainly: **morale is the crew's share measured against what they expect.**
`0x00869AB4` is the hold's first slot — Gold, i.e. undivided plunder — over a
crew-sized divisor, divided by an expectation that grows with the square of some
world term.

| Address | Role | Confidence |
|---|---|---|
| `0x00869AB4` | undivided plunder (hold slot 0) | **established** — the hold array is documented above |
| `0x00869AB0` | the crew-count divisor term | inferred from position and use |
| `0x00869B27` | **the morale modifier byte** — see below | **established as a lever** |
| `0x0085A158` | a world term, also read by the town menu's departure path | not established |
| `0x00869A76` | a world term | not established |
| `0x00869B34` | flags; bit 7 shifts the divisor by 19 | not established |

### The one lever: `0x00869B27`

It enters as `- 4 * value`, which *lowers* the expectation, which *raises* the
resulting level. So **writing this byte is how morale is moved** without
touching the player's gold or crew.

**Nothing in the game writes it.** `0x00869B27` has exactly **one** cross
reference in the whole executable — the READ at `0x0040482A`, inside
`GetMoraleLevel` itself. No code path anywhere assigns to it. It sits inside the
player record (which ends at `MessageText`, `0x00869B48`), so it is loaded and
saved wholesale with the career rather than being maintained by code.

Two consequences, and they are what make a morale system possible at all:

- **PEMF owns this byte during a session.** Write it and it stays written; the
  engine will not fight back, because no engine code touches it.
- **A save load replaces it**, along with the rest of the player record. So it
  has to be re-applied after a load — which is a moment PEMF already detects and
  already restores its own state at.

### ✅ Measured in game, 2026-08-06

**The reading of the formula is correct.** A probe that recomputes it from the
same inputs and compares against `GetMoraleLevel` reported `predicted=4 actual=4
-- formula agrees` at every sample, before and after a sweep.

**The byte has FULL AUTHORITY** — it moves morale across the entire 0–4 range.
Measured at 996 plunder, 62 crew, with `A(0x869A76)=0` and `B(0x85A158)=0`:

| byte | expect | level |
|---|---|---|
| −128 … −4 | 516 … 20 | **0** |
| −2 | 12 | 1 |
| −1 | 8 | 2 |
| 0 | 4 | **4** |
| +1 … +127 | 1 (clamped) | 4 |

Three things that matter more than the headline:

- ⚠️ **The useful range is tiny — roughly −8 to 0.** Everything below −8 is
  level 0 and everything above 0 is level 4. This is a lever with four usable
  notches, not a dial.
- ⚠️ **Level 3 was unreachable.** The expectation jumps 12 → 8 → 4 as the byte
  goes −2 → −1 → 0, and the division lands on 1, 2, then 4. Nothing produced 3
  at this wealth and crew. **Anything mapping a wide scale onto engine levels
  must accept that some levels are not reachable at some wealths**, rather than
  assuming a byte exists for every target.
- ⚠️ **`A` and `B` were both zero here**, so the expectation base was
  `(0−4+0)² / 4 = 4`. That tiny base is *why* the byte dominates. If either term
  is non-zero later in a career the base grows and the byte's authority shrinks
  — so the closed loop should solve for the byte from the **current** inputs
  each time, never from a table baked at these values.

Everything else in the formula is either the player's actual wealth and crew, or
a world term we have not identified. Raising morale by giving the player gold
would be a lie; raising it through this byte is the honest lever.

### Levels, tiers, and what the engine will render

The engine has **four** morale icons for **five** levels:

```
0x006FD8D4  moraleIcon_happy.nif
0x006FD8EC  moraleIcon_content.nif
0x006FD904  moraleIcon_unhappy.nif
0x006FD91C  moraleIcon_mutinous.nif
```

⛔ **The tier NAMES are not in the executable.** Searching the binary for
`content`, `happy`, `unhappy`, `angry`, `ecstatic` as standalone strings returns
nothing. They are rendered by the `@HAPPY` token, which resolves out of
`text.ini` — and `text.ini` is **not** loose-file overridable (see
[`ASSETS.md`](ASSETS.md); the text system never looks on disk).

Two consequences for anything extending morale:

- **The engine cannot be taught a new tier name.** `@HAPPY` will only ever
  render the words shipped in the archive, and there is no supported way to add
  to that list.
- **So an extended scale must be PEMF's own**, drawn with PEMF's own text. That
  is not a limitation in practice — PEMF already draws its own cards, notices
  and menus — but it does mean an extended morale scale is a PEMF concept that
  happens to *agree* with the engine at the five points the engine knows about,
  rather than an extension of the engine's own.

### Negative morale

The engine clamps at 0 and will not go below it: `if (level < 0) return 0;`.
There is no representation for "worse than mutinous" anywhere in the formula or
the icons. Anything below zero is therefore entirely PEMF's to define, store and
draw.

### What the engine does with mutinous crews

Real, and worth reusing rather than reinventing:

```
0x006F5898  "The crew is mutinous after @NUM months of sailing. Roll call
             reveals that @NUM crewmen have deserted."
0x006F5900  ... "1 crewman has deserted."
```

So desertion on long voyages at low morale is already the engine's own
behaviour, and `0x004125A0` (the mutiny message, already recorded in this file)
is its presentation.

---

## Loot — where plunder is awarded

> **Confidence: cross-references and one disassembled site, 2026-08-06.** Partial
> on purpose. The shortlist below is real; which entry is the *capture* award is
> not yet established, and is written here as open rather than guessed.

Undivided plunder is hold slot 0, `0x00869AB4`. It has **60 cross references**,
so "where does loot come from" is not one site but a family. The functions that
**write** it:

| Function | Notes |
|---|---|
| `FUN_0047A040` | writes at `0x0047EBE2` — **disassembled, see below** |
| `FUN_0045D2B0` | three writes (`0x45DCA9`, `0x45DDA9`, `0x45E066`) |
| `FUN_00406F60` | several; reached from the town menu (`0x00411D9E`) |
| `FUN_0040C730` | reached from the town menu (`0x0041203C`) |
| `FUN_004054A0` | reached from the town menu (`0x00410E26`) |
| `FUN_004102A0` | writes at `0x410852`, `0x410873` — this is **Divide the Plunder** (menu id 4), i.e. the *spend*, not the award |
| `FUN_00460480` | writes at `0x00460822`; `0x0046xxxx` is the sailing/overworld region |
| `FUN_004517B0`, `FUN_004741B0`, `FUN_00401000`, `FUN_004Dxxxx` | not examined |

### One award site, read in full

`0x0047EBD7`–`0x0047EBE2`:

```
0047EBD2  CALL 0x00488A80          ; positional audio -- a sound at a world position
0047EBD7  MOV  EAX,[0x00869AB4]    ; read plunder
0047EBDF  ADD  EAX,0x32            ; + 50
0047EBE2  MOV  [0x00869AB4],EAX    ; write it back
```

A **fixed +50** with a world-positioned sound immediately before it — the shape
of picking something up during a ship battle rather than the spoils of taking
her. Small, but it is a genuine, isolated, hookable loot award: read-add-write
against a known address, with the amount as a single `ADD` immediate.

**Why this matters for the officer system.** It is proof that loot is
interceptable at all. `0x0047EBDF` is an operand patch of exactly the kind
already shipping in `storms.h`, and a redirect of the surrounding call is the
kind already shipping in `townmenu.h`. Neither technique is new work.

### The capture handler

`FUN_00478730` carries *"As your ship approaches, the enemy strikes her colors."*
(`0x00708B90`), alongside *"The demoralized crew quickly surrenders."*
(`0x0070851C`) — so surrender and capture resolve in the `0x478xxx` region,
adjacent to the award site above.

### ✅ The plunder award — found

`FUN_004DCF20`, at `0x004DD01F`. Found from the text it eventually shows:
`"@NUM gold pieces plundered!"` (`0x0070F54C`, displayed by `FUN_004DE1E0`).

```
004DD01F  MOV EAX,[EDX]              ; the amount being awarded
004DD021  MOV ECX,[0x00869AB4]       ; current plunder
004DD027  ADD ECX,EAX
004DD029  MOV EAX,[0x00861FF8]       ; a running total
004DD02E  MOV [0x00869AB4],ECX       ; plunder written back
004DD034  MOV ECX,[EDX]
004DD036  ADD EAX,ECX
004DD038  MOV [0x00861FF8],EAX       ; the total updated too
004DD03D  MOV [EDX],ESI              ; pending amount cleared (ESI = 0)
```

⚠️ **The amount is read from memory, not an immediate.** So unlike the storm
scale, this cannot be changed by patching an operand — there is no constant to
patch. That rules out the technique this section previously assumed would apply.

### 🔑 `0x00861FF8` — plunder EARNED, and nothing else

The better lever, and it needs no code written at all.

`0x00861FF8` is a running total incremented **beside** plunder wherever plunder
is *awarded* — here, in `FUN_0045D2B0`, and in `FUN_004054A0`, always as the
same read-add-write pair. Crucially it is **not** touched where plunder is
*spent*: the payment site at `0x00451910` (`SUB [0x869AB4], EDI`) leaves it
alone.

So **`0x00861FF8` rises exactly when the player earns loot, and never when they
spend it.** That is a precise, unambiguous signal for "loot was just awarded",
and it covers every award site at once rather than one at a time.

### How PEMF should multiply loot

Observe and correct, rather than patch:

1. Sample `0x00861FF8` at the safe point.
2. When it rises by `N`, loot of `N` was just awarded.
3. Add `(multiplier − 1) × N` through `state::AddPlunder`, with a reason.
4. Re-baseline. If it ever *falls*, re-baseline rather than trusting the delta —
   `FUN_00404220` writes it (`0x0040451C`), plausibly a reset on a new career.

Why this is the right shape here:

- **No `.text` write.** None of the DRM risk, none of the Steam checksum
  problem, nothing to re-apply after a device reset.
- **Every award site at once**, including sacking a town and digging up
  treasure, without finding each one.
- **Traceable.** The addition goes through the validated layer and is logged
  with its reason like every other PEMF state change.
- **It reads well.** The engine's own *"@NUM gold pieces plundered!"* shows the
  base amount; PEMF's bonus arrives just after, which is exactly how "your
  quartermaster's eye finds a little more" should feel. The alternative —
  changing the number in the engine's own message — would require intercepting
  the amount before it is formatted.

### Still open

- Whether cargo and ship value are awarded separately from gold. `FUN_004DD8F0`
  and `FUN_004E0700` both read-modify-write plunder nearby and have not been
  read.
- What `FUN_00404220` does with `0x00861FF8` (assumed a career reset).

---

## Weather during a ship battle

Asked: can a battle keep the weather it started in?

**Half of it is done and needs no engine work.** The storm system is PEMF's own,
and it no longer ages while the player is off the overworld — see the note in
`storms.h`. A battle no longer retires the storm you were fighting in, so you
come out into the same weather you went in with. That was also the real cause of
"the storm music never comes back after a fight": the drums had nothing left to
play for, because the storm had been retired mid-battle without moving an inch.

### ✅ The battle already draws a cloud

Settled by cross-referencing the two prefabs rather than guessing:

| Prefab | Referenced from |
|---|---|
| **Storm** `0x008CCB58` | `0x0042A0CC` (registration) · `0x00463775` (**overworld only**) |
| **Fair cloud** `0x008CC498` | `0x0042A0CC` (registration) · `0x00463964` (overworld) · **`0x0047B3CB` in `FUN_0047A040`** |

`FUN_0047A040` is the battle function — it is the one carrying the in-battle
`+50` loot pickup at `0x0047EBE2`. So **the battle scene already draws the
fair-weather cloud**, which answers all three of the open questions at once: the
prefab system works there, the camera does show clouds, and there is a call site
to work with.

The storm prefab is only ever drawn from the overworld. It is registered
globally, though, at the same place the fair cloud is.

### The call site

```
0047B3BF  XOR  EDI, EDI
0047B3C1  PUSH EDI                 ; 0
0047B3C2  PUSH -1
0047B3C4  PUSH EDI                 ; 0
0047B3C5  PUSH EDI                 ; 0
0047B3C6  PUSH EDI                 ; 0
0047B3C7  PUSH 0x64                ; scale -- 100
0047B3C9  PUSH EAX                 ; position
0047B3CA  PUSH ECX                 ; position
0047B3CB  PUSH 0x008CC498          ; THE PREFAB, as a stack argument
0047B3D0  MOV  EDX, 0x3E8          ; 1000
0047B3D5  CALL 0x004BBC10
```

Two things make this straightforward. The prefab arrives as a **pushed
argument** rather than in `ecx` as it does on the overworld, so which cloud is
drawn is one value on the stack. And `FUN_004BBC10` is a wrapper that calls
`FUN_004BBC80` — the same routine PEMF already redirects for the storm cluster.

**So the technique transfers directly.** Redirect the `CALL` at `0x0047B3D5`,
and the shim decides which prefab to pass: the storm when the battle began in
weather, the fair cloud otherwise. A static patch of the immediate would be
simpler and is the wrong choice — it would put a storm over every battle,
including the ones fought in clear sky.

⚠️ `FUN_004BBC80` must never be skipped once entered — the instance pool leaks
and every cloud in the game disappears permanently. A redirect that decides
*which* prefab, and always calls through, does not have that problem; one that
sometimes returns early does.

### ⚠️ The battle sets its OWN weather intensity

Found while chasing "the battle is slightly darker but there is no rain and no
cloud". `FUN_0047A040` **writes** the weather-intensity global at `0x0047EE97`:

```
0047EE6E  CALL ESI
0047EE71  MOV  ECX, 3
0047EE76  IDIV ECX
0047EE78  MOV  EAX, [0x0085A0F8]     ; the weather intensity
0047EE7D  LEA  EAX, [EDX + EAX - 1]
0047EE81  CMP  EAX, 4
0047EE86  MOV  EAX, 4                ; floored at 4
0047EE8D  CMP  EAX, 0x14
0047EE92  MOV  EAX, 0x14             ; and capped at 20
0047EE97  MOV  [0x0085A0F8], EAX     ; written back
```

So a battle **always has weather**, between 4 and 20, whatever the sea outside
was doing. That explains the "slightly darker" — the battle is deriving its own
modest weather and nothing to do with our cloud.

It also names the lever. The global rain multiply this file already documents
(`0x0047BC3C`, `imul eax, [0x0085A0F8]`) is **inside this same function**, so the
rain a battle draws is scaled by the value written above. Raising
`0x0085A0F8` toward 20 when the fight began in a storm is the route to rain and
darkness in battle — and it needs no new hook, only a write at the right moment.

⚠️ Not free: `0x0085A0F8` is the **shared** weather intensity. `storms.h` reads
it for its own thresholds and the wind uses it too, so a write here is felt
outside the battle. The battle already overwrites it on entry, which is a point
in favour, but this wants measuring before it is trusted.

### ⛔ Measured, and it does not work. Turned off.

Driving the intensity up did produce rain — and the wrong rain. It drew
**heavy, static, and across only part of the screen**, which reads as a broken
texture rather than weather. Worse than the drizzle it replaced. `storms.ini`
ships `battleIntensity = 0` and the knob is kept only for experimenting.

Why, as far as it was taken: the rain quads are built in a loop around
`0x0047BC3C`, indexed off per-instance arrays at `0x008BC468` and `0x008BC474`,
calling `FUN_004DB210` several times per drop to place each one. The intensity
scales a term in that placement. So the value is not a simple "how much rain"
dial — it feeds positioning, and the battle's rain is evidently built for the
`4..20` the battle derives for itself rather than for a number pushed in from
outside.

**What would actually be needed**, and none of it is done:

1. What `FUN_004DB210` computes, and which of its results is the animation term.
   Static rain means something that should advance is not advancing.
2. What the arrays at `0x008BC468` / `0x008BC474` hold per instance, and how
   many instances the battle allocates — partial-screen coverage suggests the
   count or the extent is derived from something the battle set once at entry.
3. Whether the rain can be re-seeded after the fact at all, or whether intensity
   is only ever read when the effect is created.

**Recommendation: park the battle-weather VISUALS.** The parts of this that
work are worth keeping and are unrelated — weather no longer ages while the
player is off the overworld, so a fight keeps and returns to the storm it began
in, and a battle is no longer mistaken for the overworld. Those needed no
graphics work at all.

**Not yet built.** The remaining unknown is whether the storm prefab has
instances available in the battle scene — it is registered globally and the pool
allocates on demand, so probably, but "probably" is not "measured".

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
| Notice strip (`0x00876BF8` + `0x008CA9E8`) drives the game's own bottom-of-screen line | **Verified in-game** — PEMF's notices appear on it |
| Notice-strip buffer is 256 bytes | **Verified statically** — next referenced global is `0x00876CF8`, exactly `+0x100` |
| Notice strip has **no queue**; posting overwrites | **Verified statically** — single buffer, single counter, no backing store |
| `FUN_004888E0` is audio, not presentation | **Proven** — its own error string names the audio manager |
| `FUN_00509500` measures text; `FUN_00487E90` fits it to width | **Decompiled** |
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

---

## The profile folder is not one name

Everything per-player — `Config.ini`, saves, `KeyMap.ini`, `Custom\` — lives in
`My Documents\My Games\<game>\`, and **`<game>` differs between builds**:

| Build | Install folder | Profile folder |
|---|---|---|
| Steam | `Sid Meier's Pirates!` | `My Games\Sid Meier's Pirates!` |
| GOG | `Sid Meier's Pirates` | `My Games\Sid Meier's Pirates` |

The exclamation mark follows the install folder. **A machine that has run both
has both folders**, which is exactly how hardcoding the Steam spelling survived
testing here and broke every GOG install: the WASD keymap wrote into a directory
that did not exist, so no bindings changed and no `.pemf-backup` appeared, and
the flag scan silently missed the player's own folder.

`ResolveProfileDir()` in `core.cpp` enumerates `My Games\Sid Meier's Pirates*`
and takes the candidate **spelled exactly like the install folder** — both come
from the same title string in the build, so it identifies the copy being played
rather than guessing at it. Where no name matches (a renamed install), it falls
back to the newest `Config.ini`; the game rewrites that on exit every time.

The fallback is *only* a fallback on purpose. On a machine with both builds
installed, "most recently played" hands the GOG build the Steam folder whenever
it runs second. Every candidate and the winner go in the log. **Never spell this
path out; ask the resolver.**

## Key bindings — no reverse engineering required

The game ships a **documented, user-editable `KeyMap.ini`** in the profile
folder above, with a shortcut to it
in the install folder. UTF-16LE with a BOM; one section per context —
`[Battle]`, `[Dance]`, `[Duel]`, `[Fight]`, `[Menu]`, `[Sail]`, `[Shell]`,
`[Sneak]`.

Entries read `CommandName_default = key`. **The suffix on the left is
documentation** — the binding is the value on the right, and the names are worth
leaving alone so the file still matches the game's own reference.

### Why WASD is not simply a matter of typing it in

Two of the four letters are already taken in the sailing context:

| Collision | Was | Moved to |
|---|---|---|
| `AttackShip_a` | `a` | `Space` |
| `QuickSave_S` | `S` | `F5` (with Quick Load to `F9` to match) |

That is presumably why the game never shipped a WASD preset.

PEMF installs one **once**, backs the original up as `KeyMap.ini.pemf-backup`,
and leaves a marker line in the footer so it never touches the file again —
anything a player rebinds afterwards survives. Deleting the marker asks for the
layout back.

**Worth remembering generally:** before reverse engineering a behaviour, check
whether the game already exposes it. This one was a shortcut sitting in the
install folder the whole time.

---

## Store builds: GOG and Steam are not the same file

Verified 2026-07-28 against both live installs.

| Build | Size | Notes |
|---|---|---|
| **GOG** | 3,323,288 | **Byte-identical** to the reference this project was mapped against (`sha256 6e88b90e…`). Every address can be checked statically. |
| **Steam** | 1,189,888 | **DRM-packed.** `.text` has a virtual size of 5,177,344 against 1,176,576 on disk. |
| Challenge Pack | 3,334,144 | A **different build**. Not supported; contains a code cave the others lack. |

### ⛔ You cannot verify the Steam build from its file

The on-disk bytes at any address are compressed. Reading `Pirates!.exe` at
`0x00414FC0` and comparing against the reference is meaningless — it will differ,
and that says nothing about whether the address is correct at runtime.

**This produced a false alarm.** A parity script compared the GOG exe against the
staged *Challenge Pack* exe and reported fifteen mismatches, which read as "Steam
is broken" — while the Steam build was in fact running every feature correctly in
a playtest at that moment. Two errors at once: the wrong file, and a method that
could not work on the right one.

### How PEMF actually verifies it

At runtime, after the unpacker has run:

* `VerifyTarget()` gates loading entirely on a handful of load-bearing addresses
* `ReportFeatureProbes()` signature-checks **every engine function PEMF calls**
  and logs a per-feature verdict, so a drifted build loses one feature with an
  explanation rather than crashing
* hooks are installed **by absolute slot address** rather than by name, and
  retried, because a packed import table is not populated at load

**The rule:** for a packed target, static analysis maps the code and only runtime
checks confirm it. Never report parity from a file comparison alone.

## The sailing master's "far out to sea" card — NOT A BUG

Reported repeatedly, by two testers and by us, as PEMF's fault: *"We're far out
to sea captain, shall I set a course for..."* comes up while apparently sitting
beside a port. **It is stock behaviour. Measured, twice, independently. Do not
investigate this again without reading this section first.**

### How it actually works

It is a **timer**, not a map-bounds check. The sailing update keeps one:

```
00473749  INC  [0x0085A154]              ; sailing tick
0047374F  MOV  EAX, [0x0085A158]
00473761  IMUL EAX, EAX, 0x1F4           ; (B + 1) * 500
00473767  ADD  EAX, [0x008B98D4]         ; + next-prompt-due
00473769  CMP  ECX, EAX
0047376D  CALL 0x0045B890                ; -> the card
```

`FUN_0045B890` then decides whether to speak:

```
0045B8D0  CALL 0x0045FEE0    ; nearest city, ANY nation, all 128, exclude 0x80008000
0045B8D8  XOR  ECX, ECX      ; the player's ship, explicitly
0045B8DA  CALL 0x00405D90    ; distance
0045B8DF  CMP  EAX, 0xC
0045B8E2  JL   <stay quiet>  ; under 12 -> nothing
```

It re-arms itself: `+250` ticks if it stayed quiet, `+99999` if it spoke.

**Leagues are the distance × 5**, so the threshold is 12 → **60 leagues**. That
is easily crossed while coasting, which is why it feels wrong: you can be in
sight of land and still be 60 leagues from the nearest *port*.

### Two things that mislead

- `FUN_00405D90` takes BOTH arguments implicitly — `ECX` = ship index, `EAX` =
  city index, the latter being whatever the preceding search returned. That
  looks fragile and is not: every call site sets both deliberately.
- The per-nation LIST is a **different** function, `FUN_0045FCB0`, and it scans
  only **44** cities unless the mask is exactly `0x10` (Pirate), when it scans
  128 — `while (i < (uVar3 + 0x2c))` where `uVar3 = (mask != 0x10) - 1 & 0x54`.
  So the list and the gate do not consider the same set of ports. Still the
  game's own design.

### The measurement that closed it

A temporary shim on `0x0047376D` logged PEMF's own city lookup against the
engine's, every time the card fired:

```
sailmaster: card allowed -- nearest city 31 is 13249 away (engine 13).
            Both agree we are at sea.
```

`13 * 5 = 65` leagues, exactly what the card printed. For scale, alongside a port
reads ~984 in the same units. The two independent calculations agreed, so
nothing of ours was distorting it, and the shim was deleted.

### Ruled out, so nobody repeats it

- PEMF writes **none** of the inputs: `0x008B98D4`, `0x0085A154`, `0x0085A158`
  (read only, as morale's term B) and `0x0085A164` (read only).
- `MarkCitySentHunter` sets bit `0x800` on a city record; the searches exclude
  `0x80008000` / `0xC0008000`. Bit 11 is in neither, and `kCityRecStride = 32`
  matches the engine's own `puVar5 += 8` dwords.
- The flag-mesh arrays that end one dword below the city table (`0x00860B54 +
  nation*4`) are declared in `game.h` and never written.

If a player finds the card too talkative, that is a **preference**, and the
honest way to serve it is an opt-in setting — not a bug fix.
