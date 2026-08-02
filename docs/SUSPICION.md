# Suspicion

False colours, and what it costs to wear them.

**Status: built and running in game.** Suspicion climbs, the beats fire, being
unmasked drops your reputation and strikes your colours, and pirate-hunters
sail. The engine work it rests on is in [`GAME_API.md`](GAME_API.md),
[`re/experiments/nations/`](../re/experiments/nations/README.md) and
[`re/experiments/shipyard/`](../re/experiments/shipyard/README.md).

Tuning lives in **`PEMF\suspicion.ini`**, which is commented and never
overwritten by a rebuild. This will be balanced by playing it.

## Verified in game

- Suspicion rises near the ships and ports of the crown whose flag you wear,
  and falls in open water
- All four beats fire on the way up
- Unmasking drops reputation, strikes the false colours, and dispatches hunters
- Hunters spawn from a port of the offended nation, hover as
  `"Dutch pirate-hunter"`, and are treated as **genuinely hostile by the
  engine** — they hail you with the game's own `"Stand and fight you
  yellow-bellied Pirate!"`, which is not a line PEMF wrote
- Hunter strength and number both scale with reputation

---

## The problem this solves

PEMF can already fly any flag, remember it per career, and put it back. But it
is **cosmetic**: nothing in the world reacts, because the engine has no concept
of a disguise. Nothing in the entire binary reads the player's nationality —
checked exhaustively, not assumed.

That is not a gap to be patched. It means the whole mechanic is ours to define,
and nothing we build has to stay in step with engine behaviour we do not
control.

## The one idea the rest hangs off

**A disguise buys you closeness. Closeness is what gets you caught.**

The reason to fly Spanish colours is to get near Spanish ships and ports —
to trade, to slip past, or to close the distance before you attack. But being
near them is exactly what lets them look at you properly.

So the player is always in the middle of the same decision: *one more minute?*
The mechanic paces itself, and it needs no timer to do it.

### Why not a timer

An earlier version of this design had suspicion rising *the longer you are at
sea*. That is backwards twice over. Open water is where a disguise is **safest**
— nobody is looking at you — and a timer punishes the player for playing
normally, which is what makes a system feel like nagging rather than tension.

**Suspicion rises from being observed, not from elapsed time.** Time alone at
sea *lowers* it. The trail goes cold.

---

## What raises it

Per nation. You are not "suspicious" in general; you are suspicious **to the
crown whose colours you are wearing**, because they are the ones who know their
own navy.

| Source | Why it fits |
|---|---|
| **Proximity** to that nation's vessels and ports | The core loop. Nearer and longer both count. |
| **A close pass** — inside hailing distance | A step change, not a gradient. They got a proper look. |
| **Your rank and reputation with them** | A famous captain is harder to pass off as a stranger. Both readable: `0x00869A88`, `0x00869A78`. |
| **Wrong waters** | Spanish colours where Spain has no business. Every settlement's nation is readable at `0x00860B74`. |
| **Wrong ship** | Sailing a captured English frigate under Spanish colours. The type field is `+0x00`. |

## What lowers it

- Distance, and time unobserved
- Returning to your true colours
- Being somewhere the disguise is plausible

## Thresholds

Everything below the top rung is presentation, and PEMF already has the
machinery for all of it — notice channels, anchored world text, and the modal
choice card.

| Level | What happens |
|---|---|
| Low | The disguise holds. This is the *payoff* — you get to be near them. |
| ~30 | Status notice: they are signalling for your colours. |
| ~60 | Narrative notice: she is coming about. |
| ~90 | **Challenge card** — hold the ruse / strike your colours / run. |
| 100 | **Unmasked.** True colours go back up, and the crown dispatches a hunter. |

---

## The best part: blame

This is the mechanic worth building the system for.

**If you attack while flying someone else's colours, and a witness escapes,
they get blamed.**

You can frame a nation into a war. That is a far more interesting consequence
than "everyone trusts you less", and it turns witnesses into the currency:

- **Sink or take everything** → nobody saw anything. You got away clean.
- **Let one runner escape** → the story spreads, and it is a *Spanish* story.

The player now has a real reason to chase down the last ship, and a real reason
to hesitate before firing at all. Both are good pirate decisions.

### How blame should land

Blame accrues in **PEMF's own ledger** first, not in the engine's relations
matrix. A framed nation's standing with the victim's nation worsens in our
model, and the player sees it.

### Blame lands on reputation, and that is not a hack

**Negative reputation with a nation IS that nation being hostile to you** — the
game's own model, not one we invent. Below zero its ports stop being available
to you; below `-1` there is a price on your head and an amnesty to buy.

So a frame that sticks does exactly what it should: it drives the framed
nation's reputation down, and the world responds using machinery that already
exists. Ports close. The amnesty offer appears. That is a real consequence, in
the game's own terms, from one word of player state that is already saved with
the career.

This is a much smaller step than it first looked. Reputation lives in the
216-byte player record — **player state**, ours to affect. It is the nation
relations matrix that is world state.

⚠️ **Open decision, narrowed:** whether a large enough frame should *also* write
the relations matrix (`0x0085A168`) and start an actual war between two crowns.
That remains the most invasive thing PEMF could do to a career, and `nations.h`
states as policy that the matrix is the engine's to maintain. Reputation alone
gives the mechanic teeth, so this can stay unanswered for now. **Not to be
decided quietly during implementation.**

---

## What already exists to build this

Every input the design needs is confirmed working in a running game.

| Need | Source | State |
|---|---|---|
| What colours we are wearing | PEMF's own flag layer | ✅ verified |
| Who is nearby, and their nationality | ship array, `+0x04` | ✅ verified |
| Whether they are steering at us | `WatchTheWater()` dot-product | ✅ verified |
| Where ports are and whose they are | `0x00860B74`, `0x00860B68` | ✅ verified |
| Which crown we actually serve | `PlayerNation`, `0x00869AA8` | ✅ verified |
| Who is at war with whom | relations matrix, `0x0085A168` | ✅ verified |
| Our standing with each crown | `0x00869A78`, `0x00869A88` | ⚠️ addresses confirmed, values not yet seen to move |
| Showing all of it | notice channels + choice card | ✅ verified |
| Persisting it per career | sidecar | ✅ verified |
| **Dispatching a hunter** | ship factory + destination write | ✅ **verified — builds, sails, arrives** |

## What is missing

1. ~~The hunter spawn is unproven.~~ **Done.** PEMF can build a ship of any
   nation at any port and send it anywhere, using the engine's own factory. The
   lever turned out to be the destination field, not a role or a hostility bit —
   a ship built with its destination equal to its origin has arrived, which is
   why the first attempts appeared to do nothing. Full account in
   [`GAME_API.md`](GAME_API.md#-the-lever-is-the-destination-not-the-role).

   And a hunter is not something we have to write: **`+0x02` on a ship record
   classifies it, and `1` means pirate-hunter** — the value behind the game's
   own `"@NATIONALITY pirate-hunter"` hover label. Whether stamping it produces
   hunting *behaviour* is the outstanding test.

   Still open underneath that: a spawned ship sails to a *place*, not at a
   *ship*. If the purpose field does not bring behaviour with it, pointing a
   hunter past the player is the fallback.
2. **Infamy is not located.** The game tracks an "@ORDINAL most notorious pirate"
   ranking, so a value exists, but the HUD only draws the *word* "Fame" and the
   two references to that string push no value. Wanted for the "a famous captain
   is harder to disguise" input; rank and reputation stand in until then.
3. **Role values 1–4 are unmapped.** Only role 2 is known (a governor's blockade
   dispatch). If one of them means "hunt the player", the hunter is a field
   assignment rather than a behaviour we have to write.
4. **Witness detection is undesigned.** Which vessels count as having seen an
   attack, and how an escape is recognised, both need working out — probably
   from the battle instance rather than the overworld.

---

## Build order

1. **The bounded experiment.** Call `FUN_00414FC0` behind the marker file, on a
   throwaway save, and see whether a ship appears. Everything else is shaped by
   the answer.
2. **The suspicion model**, evaluated at the safe point, surfaced through the
   existing notice channels, persisted in the sidecar. No engine writes.
3. **The challenge card** at the top of the ladder, using the existing dispatch.
4. **The hunter**, if step 1 says yes.
5. **Blame and witnesses** last — it is the most interesting part and the least
   understood, and it should be built on a system that is already working.

---

## Faults found by playing it

Every one of these was invisible in the code and obvious within a minute of
sailing.

### A rate is not progress

The panel showed a correct `+12` beside a level that never left `0`. The safe
point runs about every 16 ms, so `level += rate * dt / 1000` is `12 * 16 / 1000`
— **zero**, in integer arithmetic, sixty times a second, forever. The remainder
was discarded on every tick.

Fixed by carrying the fraction in point-milliseconds. The same truncation was in
the decay path.

### A flag name is not an allegiance

Whose colours counted as *ours* was decided by comparing against the name of the
career's true flag. A career's own flag is usually a personal device
(`flag_jack.dds`) which resolves to no nation at all — so **an English captain
flying English colours was treated as an impostor and hunted by his own crown**.

Now compared against `PlayerNation`, the engine's own record of who you serve.

### Saying it is not doing it

Unmasking announced *"Colours struck!"* and left the false flag flying. So
suspicion reset to zero and immediately began climbing again, over and over,
with the player never told why. It now actually strikes the colours and ends the
ruse.

### Drawing anything dirties the shared buffer

The panel rendered *"She's coming about"* in enormous letters across the middle
of the sea, once per frame. The engine's HUD call uses `0x00869B48` as scratch,
so **drawing at all** leaves text in it, and the sailing render paints whatever
is there over the water next frame. The buffer was only being cleared when a
*notice* had drawn; the panel was not counted.

### Instrumentation is not a HUD

The first panel stacked four lines of diagnostics into the corner, at the same
height as the centred notices, and the two overlapped into a smear. It is now
two lines — whose colours, and how far the lie has got. The rate and the hunter
count are diagnostics and belong in the log.

---

## The standing ledger — `src/core/standing.h`

> Added after the first playtests. Suspicion decides *when* a lie comes apart;
> this decides *what it costs*, and for how long.

### The one idea

**The engine's reputation word stops being where anything is stored.** It
becomes an output we project onto, so the world keeps reacting the way it always
has — ports close, hunters sail — while the reason lives with us.

It has to work that way because the two disagree about time. Vanilla reputation
is a single number with no memory of how it got there, so a false flag that
frightened a harbourmaster and a career spent burning that nation's shipping are
indistinguishable once written. They should not be: one ought to lift if you
never follow through, the other ought not.

So a nation's standing is three numbers, and what the engine sees is their sum:

| Field | Meaning | Lifetime |
|---|---|---|
| `baseline` | standing genuinely theirs to give | the game's business |
| `debt` | a fright — being seen through | lapses after `debtForgetMonths` |
| `notoriety` | what you actually did | **permanent** |

```
engine reputation  =  baseline - notoriety - debt
```

### `applied` is the field that makes it safe

The game writes reputation too — promotions, missions, and every hostile act.
Projecting our target blindly each tick would erase all of it, and the bug would
present as *"reputation sometimes doesn't change"*, which is close to
undiagnosable months later.

So we remember the last value **we** wrote. When the live word differs, the
difference is the game's doing and is folded into `baseline`. Their change is
real standing and it survives.

### The same check detects piracy, for free

A **downward** write we did not make **is** the hostile act. The engine already
knows what counts as attacking or plundering — we read its verdict instead of
keeping our own list, so there is no combat hook to write and nothing to keep in
step with the game's own rules.

That event routes through `NoteHostileAct(target, severity)`, which:

- adds `severity` to the target's `notoriety`,
- adds `actSpillPercent` of it to **every other crown** — everybody hates a
  pirate, nobody hates one as much as the nation he robbed,
- **hardens any outstanding `debt`** into notoriety. The suspicion was earned.

### The rules

| Situation | Result |
|---|---|
| Unmasked, clean record | `debt` — lapses after `debtForgetMonths` of doing nothing |
| Unmasked while `baseline - notoriety < 0` | hardens immediately — they already had your measure |
| Any hostile act | `notoriety`, permanently, plus the spill |
| `Amnesty(nation)` | the **only** thing that clears notoriety |

Notoriety never decays. A pardon, a bribe, or a service done for that crown is
the intended route back, and is a content hook rather than a timer.

### Persistence

Written to the `.pemf` sidecar as
`standing<N>=baseline,debt,notoriety,debtSetAt`, staged and applied on the same
career-fingerprint proof as the rest of the save state.

The restore arithmetic is worth stating because it is easy to get backwards: at
save time the engine word **already contains our projection**, so `ApplyStaged`
re-primes `baseline` from the engine and then adds `notoriety + debt` back on to
recover standing *without* us. Verified as an exact round trip.

Backward compatible — no `kStateVersion` bump. An older sidecar simply carries
no ledger and the career is primed fresh from the engine.

### Tuning

All of it lives in `PEMF\suspicion.ini`, read once at startup:

| Key | Default | Effect |
|---|---|---|
| `debtForgetMonths` | 12 | game months of honest sailing before a fright lapses |
| `actNotorietyShare` | 1 | notoriety remembered per point the engine itself took |
| `actSpillPercent` | 25 | what the other crowns take from an act against one of them |

> **Note:** `build.ps1` never overwrites an existing tuning file, so a rebuild
> cannot discard a balance pass. Editing the deployed copy is the way to retune.

---

## Principles

- **Suspicion is PEMF's. The world stays the engine's.** We read game state
  freely and write almost none of it.
- **Reputation is an output, never storage.** The ledger owns the truth; the
  engine's word is where we publish it.
- **Nothing evaluates in the render hook.** The safe point decides; the frame
  draws.
- **Every threshold has a visible consequence.** A hidden number that only
  matters at 100 is not a mechanic, it is a trap.
- **The disguise must be worth wearing.** If the safest play is always true
  colours, the system has failed regardless of how well it models anything.
