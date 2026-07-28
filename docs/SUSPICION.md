# Suspicion — design

False colours, and what it costs to wear them.

**Status: design. Nothing here is built.** The engine work it rests on is
recorded in [`GAME_API.md`](GAME_API.md) and
[`re/experiments/nations/`](../re/experiments/nations/README.md).

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

⚠️ **Open decision:** whether a large enough frame should actually write the
engine's relations matrix (`0x0085A168`) and start a real war. It is one int,
it is saved with the game, and the payoff would be spectacular — but it is the
single most invasive thing PEMF would do to a career, and `nations.h` currently
states as policy that the matrix is the engine's to maintain. **Not to be
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

   Still open underneath it: a hunter currently sails to a *place*, not at a
   *ship*. Pointing it past the player is close enough to read as pursuit, and
   whether true target-chasing exists has not been established.
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

## Principles

- **Suspicion is PEMF's. The world stays the engine's.** We read game state
  freely and write almost none of it.
- **Nothing evaluates in the render hook.** The safe point decides; the frame
  draws.
- **Every threshold has a visible consequence.** A hidden number that only
  matters at 100 is not a mechanic, it is a trap.
- **The disguise must be worth wearing.** If the safest play is always true
  colours, the system has failed regardless of how well it models anything.
