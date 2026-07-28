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
`;` (`flag_*.dds;flag_*.tga`), scans the game's `custom\` **and**
`My Documents\My Games\Sid Meier's Pirates!\Custom\` with `FindFirstFileA`,
de-duplicates, and appends into an array it resizes as needed.

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

## Making ships — the factory

The engine has no way to make an existing vessel hostile: **nothing anywhere
reads the player's nationality field**, checked exhaustively. When a nation
decides you are a problem the game does not flip a bit on a passing merchant —
it *builds a ship and sends it*. That is what a pirate hunter is, what a
privateer is when two crowns go to war, and what a governor's blockade fleet is.

So the primitive worth having is not a hostility switch. It is the constructor.

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
str  = 2 - rep/10                       ; imul 0x66666667 / sar 1
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

---

## Key bindings — no reverse engineering required

The game ships a **documented, user-editable `KeyMap.ini`** at
`My Documents\My Games\Sid Meier's Pirates!\KeyMap.ini`, with a shortcut to it
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
