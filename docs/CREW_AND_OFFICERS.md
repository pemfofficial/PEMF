# Crew & Officers — the plan

The design for PEMF's expanded crew morale and named officer systems, and the
reverse engineering it rests on.

**Nothing here is built yet.** This document exists so the build starts from
measured ground rather than assumption, and so the parts that are *not* yet
known are visible instead of being discovered halfway through.

Engine detail lives in [`GAME_API.md`](GAME_API.md) under *Crew morale* and
*Loot*; this page is the design that sits on top of it.

---

## What the engine actually gives us

Three findings shaped everything below. Each was read from the binary.

### 1. Morale is computed, not stored

There is no morale variable. `GetMoraleLevel` (`0x00404810`) derives a 0–4 level
on every call from **the crew's share measured against what they expect**:

```
expect = ((worldTerm)² / 4) − 4 × [0x869B27]     clamped 1..999
level  = ((plunder + 500) / (crew term)) / expect  clamped 0..4
```

So "raise morale" cannot mean writing a number, and raising it by handing the
player gold would be a lie about what happened.

### 2. One byte is ours

`0x00869B27` enters as `− 4 × value`, lowering the expectation and raising the
level. It has **exactly one cross reference in the executable** — the read
inside `GetMoraleLevel`. **No engine code writes it.** It lives in the player
record, so it is saved and loaded with the career rather than maintained by code.

That is what makes a morale system possible: PEMF owns the byte during a
session, and re-applies it after a load at a moment PEMF already detects.

⚠️ **Its authority has never been measured.** As a signed byte it shifts the
expectation by at most ±508, against a base of `(worldTerm)² / 4` of unknown
magnitude. Small base and it is a complete lever; large base and it is a trim.
**Phase B measures this before anything is designed around it.**

### 3. The engine cannot be taught new tier names

Four icons exist — `happy`, `content`, `unhappy`, `mutinous` — for five levels.
The tier *names* are **not in the executable**; they come from `@HAPPY` out of
`text.ini`, which is the one data file the engine will not read loose from disk
(see [`ASSETS.md`](ASSETS.md)).

So an extended scale, and every negative tier, is **PEMF's own** — our number,
our names, our text. It agrees with the engine at the five points the engine
knows about, and goes beyond it everywhere else.

The engine already deserts crew on long voyages at low morale (`0x006F5898`), so
the crew system should **drive that**, not reinvent it.

---

## How our morale reaches the game

A closed loop, not a parallel fiction:

1. PEMF keeps its own morale on a wide scale, in the save sidecar, including
   negatives.
2. Everything that should move morale moves *that* number — storms, cargo lost,
   events, officers, time at sea.
3. Each tick PEMF maps its number to a target engine level, solves the engine's
   own formula backwards for the `0x869B27` that produces it, and writes it.

The engine then computes its morale from our byte. The HUD icon, the desertion
behaviour and any engine logic reading morale all follow our number, because we
moved the input its own arithmetic depends on.

**Fidelity depends on Phase B.** If the byte turns out to have full authority the
loop is exact; if not, PEMF's scale still drives everything PEMF owns and the
engine's own level tracks as closely as the lever allows. Either way the design
holds — only the tightness changes.

---

## How a skill reaches the game

Officer skills are data. What a skill can *actually do* falls into three rings,
and the schema makes the ring visible rather than letting an author write a
target that silently does nothing.

**Ring 1 — verified addresses, through `state.h`.** Plunder, crew, cargo slots,
reputation, nationality, the morale byte. Clamped, career-gated, logged with the
skill's own reason. These change the real game today.

**The observe-and-correct pattern generalises.** Loot proved it: sample a value
the engine owns at the safe point, notice it move, and apply PEMF's adjustment
through `state.h`. Nothing is patched, every award site is covered at once, and
the change is logged with a reason. **Crew losses are readable the same way**,
which is what makes a surgeon who saves men a real skill rather than a label.

**Ring 2 — outcomes PEMF already mediates.** Our events, menus, cargo loss,
hunters, suspicion. A skill here changes what PEMF does, which is no less real.

**Ring 3 — engine outcomes we intercept.** Loot is the first, and it is **solved
without writing any code into the game.** `0x00861FF8` is a running total that
rises whenever plunder is *earned* and never when it is *spent*, so PEMF samples
it at the safe point, sees loot of `N` awarded, and adds `(multiplier − 1) × N`
through the validated layer. That covers every award site at once — combat,
sacking a town, digging up treasure — with no `.text` write, no DRM risk, and a
logged reason on every adjustment. Detail in `GAME_API.md`.

⛔ **Ship stats, player stats and world stats are not mapped.** They are on the
wish list and not in any ring yet. A skill naming them is **rejected at load with
the reason**, exactly like every other invalid field. The target list grows as
sites are mapped, and never advertises reach it does not have.

---

## The menus

`Manage yer crew!` is gated on **`crew == 0`** only — no career, or no crew.

> ⚠️ **Not on `morale <= 0`.** Morale reads 0 whenever the crew's share is below
> expectation, which is ordinary for a poor captain with a loyal crew. Greying
> the menu out there would hide it exactly when the player most needs it. Low
> morale changes what the menu *offers*, not whether it opens.

Six roles do not fit one card — the renderer shows six selectable lines and PEMF
adds `Never mind.` itself, so a menu holds five authored options. The roles are
therefore split by duty, which also reads like something the game would write:

```
Manage yer crew!
> Hire officers
  > Ship's officers
    - Find a Sailing Master   (handles better in a blow -- 300g + rum)
    - Find a Bosun            (crew work faster -- 250g + food)
    - Find a Carpenter        (repairs at sea -- 250g + goods)
  > The captain's men
    - Find a Quartermaster    (a fairer share -- 300g + rum)
    - Find a Master Gunner    (broadsides tell -- 350g + cannon)
    - Find a Surgeon          (fewer die of wounds -- 300g + food)
> The roster
> ...
```

Each `Find a…` opens the three tiers — Novice, Journeyman, Master — with the
cost shown. **Buying is buying the search, not the officer**: gold and goods are
spent sending the crew ashore to ask around, and finding someone is a roll. Cost
scales with tier; so does the chance and so does what you get.

| Tier | Skills | Cost |
|---|---|---|
| Novice | 1 | low |
| Journeyman | 2 | medium |
| Master | 3 | high |

---

## The data

All of it authored, all of it extensible, all validated at load.

| File | Holds |
|---|---|
| `PEMF\officers\names.json` | the master name pool — players add to it |
| `PEMF\officers\bios.json` | bio lines, in the game's voice |
| `PEMF\officers\skills.json` | skill definitions: name, text, target, operation, value |
| `PEMF\officers\roles.json` | the six roles, their tiers and costs |

An officer is generated as: role + tier → a name from the pool, a bio, and
1–3 skills drawn at random from those its role allows.

**Text is validated like every other authored string** — ASCII, token counts,
and a length budget. A bio long enough to overflow the card is rejected at load
**by name**, rather than overflowing in front of a player. The card wraps through
`WrapText` (`0x004879F0`), the routine the game uses for its own.

Officers live in the **save sidecar**, per career. `session::ModState` is three
ints today and its own comment already says to move to JSON when it grows — this
is that moment.

---

## Order of work

**Phase A — the loot hook. ✅ BUILT AND VERIFIED IN GAME (2026-08-06).**
`loot.h` samples `0x00861FF8` at the safe point. Measured on a real capture:
`660 plundered -> +330 (50%)`, applied through `state::AddPlunder` and logged as
`state: plunder 1260 -> 1590 [loot share]`. One award detected per capture, so
there is nothing to double-count. Off by default (`crew.ini`,
`lootBonusPercent = 0`).

**Phase B — measure morale. ✅ MEASURED (2026-08-06).** The formula reading is
confirmed (`predicted == actual` every sample). The byte has **full authority**
over the 0–4 range, but with **only about four usable notches (−8…0)**, and
**level 3 was unreachable** at the wealth tested. The expectation base was 4
because `A` and `B` were both zero, so the loop must solve from current inputs
rather than a baked table. Details in `GAME_API.md`. Remaining: build the wide
scale, the sidecar field, the tier names and the closed loop.

**Phase C — officers. ✅ BUILT AND PLAYED (2026-08-06).** Six roles, three
standings, hiring against a roll, an interactive roster where each man has his
own menu — speak with him, or see what he does for you (rank, tenure, history,
and each skill with its actual contribution). Content in
`PEMF\officers\roster.json`.

Two things learned in play and worth keeping:

- ⚠️ **`townmenu` sets the port backdrop for event rows and authored-menu rows,
  and native-callback rows were missed.** So an event drew against the port and
  a code-driven row drew against open sea, two rows apart in the same menu. Any
  new row type must set `game::g_portCardCity` or inherit a branch that does.
- **Odds are not shown on the tier rows.** Cost is. A captain would know what
  sending men ashore costs him and would not know his chances; a percentage on
  the row turns a gamble into arithmetic.

**Still open in Phase C:** officers do not persist — the roster is session-only
until the sidecar work lands. And a save/reload loses it silently, which is the
next thing to fix.

---

## Open questions

- Which site awards the spoils of a captured ship (Phase A).
- The magnitude of the morale expectation base (Phase B).
- Whether the six-selectable-line limit is the engine's or only PEMF's — it
  comes from PEMF's own docs, not the disassembly. If the renderer takes more,
  the menu split becomes a choice rather than a constraint.
- What `0x0085A158`, `0x00869A76` and `0x00869B34` are. All three feed the
  morale formula and none is identified.
