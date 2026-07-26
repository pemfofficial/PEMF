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

### Known unknowns

Worth stating plainly, since this file may be read as authoritative:

- The **morale formula** is decompiled and its behaviour matches observation
  (spending plunder lowered the morale level as predicted), but the meaning of
  `0x00869A76`, `0x0085A158` and `0x00869B27` is inferred, not confirmed.
- `FUN_00430190`'s ten parameters are reproduced from a working call site. Only
  the first three (x, y, sound) are understood.
- The `.pirates_savegame` format itself has not been examined at all. We detect
  saves and load by file access, never by parsing them.
