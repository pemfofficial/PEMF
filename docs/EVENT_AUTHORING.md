# Writing Events

Events live in **`PEMF\events\`**, inside your game folder. Edit any `.json` file
there — or drop in your own — restart the game, and your events are live. No
code, no compiler.

**Every `.json` in that folder is loaded**, so your add-on ships as its own file
and sits alongside everyone else's. A broken file is skipped with a reason rather
than taking the rest down.

If something is wrong with an event it is **rejected at load with a precise
reason** in `pemf.log` — and the rest of the file still loads. This has
been tested against a file of deliberately broken events covering every rule
below: seven were rejected individually and the two valid ones loaded normally.

The validation exists to stop authored text reaching a 2004 engine in a state it
cannot handle. It is thorough, but treat it as a strong safety net rather than a
formal guarantee.

---

## A minimal event

```json
{
  "events": [
    {
      "id": "hush_on_deck",
      "body": "A hush falls over the deck. Your crew of @NUM stands @HAPPY.",
      "args": ["crew", "morale"],
      "options": [
        { "text": "Say nothing and hold your course." }
      ]
    }
  ]
}
```

In game that renders as a card in the game's own style, with `@NUM` replaced by
your crew size and `@HAPPY` by their mood.

---

## Two kinds of event

Pick the one that matches what you are doing. They are genuinely different
things, not styling.

### `choice` — the default

A modal card that **interrupts** the player and asks them to decide. Options,
effects, an outcome. Use it when the answer should matter.

### `notice` — informational

On-screen text while sailing, in the same style as the game's own
`Press 'r' to return to ship.` It **never interrupts**: no options, no clicking,
it fades on its own. Use it for things the player should notice but not have to
answer — a lookout's call, a change in the weather, a passing observation.

> **Planned — sound on events.** A `"sound"` field is being worked on, so an event
> or notice can play a custom `.wav` (a voice line, a callout, a sting) when it
> fires. The audio side of the engine is mapped; this field is not live yet. When
> it lands it will be documented here with the exact format. Until then, `"sound"`
> is ignored if present.

```json
{
  "id": "landfall_sighted",
  "kind": "notice",
  "trigger": { "type": "nearPort", "distance": 3000, "rearm": 6000 },
  "body": "Land ho! Harbour off the bow!",
  "seconds": 5
}
```

| Field | Notice |
|---|---|
| `body` | the text; `@NUM` / `@HAPPY` work as usual |
| `seconds` | how long it stays up, 1–30, default 4 |
| `options` | **not allowed** — a notice never asks anything |

Notices only appear while sailing the overworld, which is where the game draws
its own. Up to three can be on screen at once; a fourth pushes the oldest off.

---

## Fields

### Event

| Field | Required | What it does |
|---|---|---|
| `id` | yes | Unique name. Used in logs and save data. |
| `kind` | no | `"choice"` (default) or `"notice"` — see above. |
| `body` | yes | The prose shown at the top of the card. |
| `args` | if body has tokens | Values for the tokens, in order. |
| `options` | yes | 1–6 choices. Three have been tested in game; more than three is permitted but untried. |

### Option

| Field | Required | What it does |
|---|---|---|
| `text` | yes | The selectable line. A leading space is added for you. Plain text only — tokens are not substituted here. |
| `effects` | no | What happens to the game if picked. |
| `outcome` | no | Follow-up text shown after the choice, as another page of the same card with a Continue. |
| `outcomeArgs` | if outcome has tokens | Values for the outcome's tokens. |

---

## Tokens

Only two are allowed:

| Token | Becomes |
|---|---|
| `@NUM` | a number you supply |
| `@HAPPY` | the crew's mood word — `DEVOTED`, `CONTENT`, and so on |

> **The number of tokens must exactly match the number of `args`.**
>
> This is the one rule worth internalising. The game engine reads arguments
> positionally, and a token with no argument behind it reads whatever happens to
> be in memory. The loader refuses the event rather than let that happen, but it
> is the mistake people make most.

Other tokens exist in the game (`@CITYNAME`, `@NATIONALITY`, and others). They
are **not permitted** — we have not confirmed whether they consume an argument,
and guessing would risk exactly the problem above. They will be enabled once
verified.

## Argument values

| Value | Meaning |
|---|---|
| `"crew"` | current crew size |
| `"morale"` | crew mood, 0–4 (use with `@HAPPY`) |
| `"plunder"` | undivided plunder |
| `"months"` | months at sea |
| any integer | that literal number |

```json
"body": "You have @NUM mouths to feed after @NUM months at sea.",
"args": ["crew", "months"]
```

---

## Triggers

Without a `trigger`, an event never fires on its own — it can only be fired by
hand with the debug hotkeys. Add one and it happens during play.

```json
{
  "id": "bosun_asks_for_rum",
  "trigger": { "type": "elapsedSailing", "seconds": 120, "cooldown": 600 },
  "body": "...",
  "options": [ ... ]
}
```

**Every trigger requires that you are sailing the overworld.** Events will not
interrupt you in a town, a menu, a battle or a mini-game.

### `elapsedSailing`

Fires after a number of seconds of **actual sailing**. Time spent in port, in
menus, or on any other screen does not count toward it.

```json
{ "type": "elapsedSailing", "seconds": 120, "cooldown": 600 }
```

| Field | Meaning |
|---|---|
| `seconds` | sailing time required (required) |
| `cooldown` | seconds before it may fire again |
| `once` | `true` to fire at most once per career |

### `nearPort`

Fires on coming within `distance` of **any** port.

```json
{ "type": "nearPort", "distance": 1200, "rearm": 2500 }
```

| Field | Meaning |
|---|---|
| `distance` | fire on getting this close (required) |
| `rearm` | must get further out than this before it can fire again — defaults to `distance * 2` |
| `cooldown` | seconds before it may fire again — usually leave this out |
| `once` | `true` to fire at most once per career |

`rearm` must be larger than `distance`. **That gap is the repeat guard**, and it
is normally the only one you want: it stops an event firing over and over while
you follow a coastline, but still lets it fire on every genuine approach. A
`cooldown` on top of that would suppress legitimate arrivals, so omit it unless
you specifically want a time limit.

### Choosing a distance

Distances are in the game's map units — the same ones the engine uses for its own
port searches. Measured over a real session:

| Distance | Roughly |
|---|---|
| ~1000 | sailing right into a harbour |
| ~1500–3000 | harbour in sight, closing on it |
| ~10000+ | open sea |
| ~20000 | nothing within range at all |

**Use ~3000 for "arriving at a port".** Do not set it to the smallest number you
have seen: the closest approach depends entirely on the route sailed. Two test
sessions produced minimums of **988** and **1620**, so a threshold of 1200 fired
in one and never fired in the other. Anything under ~2000 will miss most
approaches.

The first version of this event used `400` and could never fire at all.

`pemf.log` reports the live distance and the closest you have ever come, every
few seconds while sailing — use it rather than guessing:

```
world: inGame=1 sailing=1 pos=(434372378,81733118) nearest=city113 dist=1705 closestEver=988
```

### Shared options

| Field | Meaning |
|---|---|
| `once` | fire at most once per career. Reset when you start or load a career. |
| `cooldown` | minimum seconds between firings of this event |

Trigger progress belongs to the career it was earned in. Loading a save or
starting a new career resets accumulated sailing time and re-arms everything.

---

## Effects

What an option does to the game.

| Op | Meaning |
|---|---|
| `addPlunder` | add to undivided plunder (negative to spend) |
| `setPlunder` | set it outright |
| `addCrew` | add or remove crew |
| `setCrew` | set crew size |

```json
{
  "text": "Break open the rum. (200 gold)",
  "effects": [ { "op": "addPlunder", "value": -200 } ],
  "outcome": "The rum goes round. It costs you @NUM pieces of eight, but your crew of @NUM drinks your health.",
  "outcomeArgs": [200, "crew"]
}
```

Every effect is clamped to a sane range, refused if you are not in a career, and
written to the log with the event id — so you can always see what an event
actually did.

An unknown op is a load error, never a silent no-op.

---

## Text rules

**Plain ASCII only.** The 2004 engine predates UTF-8. Curly quotes, em dashes and
accented characters render as garbage, so they are rejected at load. If you draft
in a word processor, watch for `'` and `"` being turned into typographic quotes.

**Keep it under 1200 characters.** The card and its options share a fixed budget;
overflowing drops options off the end. The loader warns if you get close.

**`%` is safe.** The engine's text system is `@`-token based and never passes
your text to a C format function, so percent signs are literal.

---

## When something is wrong

Check `pemf.log` in the game folder:

```
content: loaded 3 event(s) from ...\events.json (1 rejected)
content: REJECTED event 'short_rations' -- body: 3 token(s) but 2 arg(s) supplied.
         These must match exactly -- a surplus token reads stack garbage.
```

Common messages:

| Message | Fix |
|---|---|
| `N token(s) but M arg(s) supplied` | Count your `@NUM`/`@HAPPY` and match `args`. |
| `token '@X' is not supported` | Only `@NUM` and `@HAPPY` are allowed. |
| `non-ASCII or control character at offset N` | Smart quotes or an accent. |
| `unknown argument source 'x'` | Use crew, morale, plunder, months, or a number. |
| `unknown effect op 'x'` | Check the effects table above. |
| `duplicate id` | Two events share an `id`. |
| `a 'notice' cannot have options` | Notices are informational. Use `kind: "choice"` if the player must decide. |
| `unknown kind 'x'` | Only `choice` and `notice` exist. |
| `PARSE ERROR` | Malformed JSON — a missing comma or brace. |

Comments are allowed in the file (`//` and `/* */`), which standard JSON does not
normally permit.

---

## Currently

Events are triggered by debug hotkeys while the trigger system is built:

| Keys | Fires |
|---|---|
| Ctrl+Shift+1 | the first event in the file |
| Ctrl+Shift+2 | the second |

Real triggers — morale thresholds, entering port, months at sea, battle
outcomes — are the next piece of work. When they land, events will gain a
`trigger` block and these hotkeys will go away.
