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
  "body": "Land ho! @CITYNAME off the bow!",
  "args": ["nearestCity"],
  "seconds": 5,
  "anchor": "ship"
}
```

| Field | Notice |
|---|---|
| `body` | the text; all [tokens](#tokens) work as usual |
| `seconds` | how long it stays up, 1–30, default 4 |
| `anchor` | `"screen"` (default) or `"ship"` — see below |
| `options` | **not allowed** — a notice never asks anything |

Notices only appear while sailing the overworld, which is where the game draws
its own. Up to three can be on screen at once; a fourth pushes the oldest off.

#### `anchor` — where the notice sits

**`"screen"`** (the default) puts the line at the top of the screen, where it
stays put. Good for status: something that is true right now, not something
happening in a particular place.

**`"ship"`** hangs the line **over your vessel in the world**, and it *follows
the ship* — it turns, moves, and drifts with the camera exactly like the labels
the game puts over other ships, because it is drawn by the same routine. It
eases out over its last second rather than blinking off. Good for anything that
is happening *to your ship*: a lookout's call, a crew shout, a sighting.

`anchor` is available on **every notice**, whatever fired it. It is a
presentation choice and is completely independent of the trigger — a notice
fired by `nearPort`, by `elapsedSailing`, by `stateCrosses`, or by hand can all
be anchored.

A few practical notes:

- An anchored notice only draws while a career is loaded and the world exists.
  Posted anywhere else it waits harmlessly and is dropped when it expires.
- Anchored lines do not stack — several at once will overlap on the ship. If you
  expect more than one at a time, put some on `"screen"`.
- Keep anchored text short. It is drawn at world scale over open water, and a
  long line will run past the edges of the screen.

---

## Fields

### Event

| Field | Required | What it does |
|---|---|---|
| `id` | yes | Unique name. Used in logs and save data. |
| `kind` | no | `"choice"` (default) or `"notice"` — see above. |
| `body` | yes | The prose shown at the top of the card. |
| `args` | if body has tokens | Values for the tokens, in order. See [Tokens](#tokens) — `@CITYNAME` needs three slots. |
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

A token is a placeholder in your text that the game fills in when the event
fires. You supply the values in `args`, in the order the tokens appear.

| Token | Becomes | Slots | Supply it with |
|---|---|---|---|
| `@NUM` | a number | 1 | `"crew"`, `"months"`, an integer… |
| `@HAPPY` | the crew's mood word — `DEVOTED`, `CONTENT`, … | 1 | `"morale"` |
| `@CITYNAME` | a port's name — `Barbados`, `Port Royale`, … | **3** | `"nearestCity"` |
| `@NATIONALITY` | the nation adjective — `Spanish`, `English`, … | 1 | `"nearestCityNation"` |
| `@LOCTYPE` | `village`, `town`, `city`… | 1 | `"nearestCityType"` |

```json
"body": "Port! @CITYNAME ahead!",
"args": ["nearestCity"]
```

> *"Port! Barbados ahead!"*

### The one rule: slots must match exactly

**The number of argument slots the tokens need must exactly match the number
`args` supplies.** The engine reads arguments positionally, like `printf`, and a
token with nothing behind it reads whatever happens to be in memory. The loader
refuses the event rather than let that happen, and tells you both numbers.

Most tokens cost one slot, so usually this is just "one token, one arg".

### `@CITYNAME` costs three slots

`@CITYNAME` is the exception, and it is worth understanding rather than
memorising. A city's name is not stored as a piece of text — it is a **record of
three values** that the engine assembles into a name. So `@CITYNAME` consumes
**three** arguments, not one.

You do not have to supply three things. **One `"nearestCity"` arg fills all
three slots**, because it expands to that whole record:

```json
"body": "The @NATIONALITY @LOCTYPE of @CITYNAME lies ahead.",
"args": ["nearestCityNation", "nearestCityType", "nearestCity"]
```

Three tokens, three args — but **five slots**: 1 + 1 + 3. The loader counts
slots, so this is correct and will load. If you ever see an error mentioning
slots, this is why.

### The city tokens name the nearest port

`nearestCity`, `nearestCityNation` and `nearestCityType` all resolve against
**the port nearest your ship at the moment the event fires**, and all three
agree with each other — the port is resolved once per piece of text, so it
cannot change halfway through a sentence.

They pair naturally with the `nearPort` trigger, where "nearest port" is the one
you are approaching, but they work with any trigger. A couple of things to know:

- If **no port is within range**, the tokens resolve blank rather than to
  nonsense. Word the text so it still reads if that happens, or use `nearPort`
  so a port is guaranteed nearby.
- They name the nearest port, **not** the one you are heading for. Sailing past
  one town toward another will name the one you are closest to.

### Other tokens

The game has more tokens — ship names, pirate names, dates. They are **not
permitted yet**, and the loader will reject them by name.

This is not caution for its own sake. Each token consumes a specific number of
arguments, and that number is only knowable by reading the game's own code:
`@CITYNAME` needing three rather than one is exactly the sort of thing that
looks fine, loads fine, and then reads stack garbage. Tokens are enabled here
once that count has been read out of the disassembly and confirmed — never
guessed.

## Argument values

| Value | Slots | Meaning |
|---|---|---|
| `"crew"` | 1 | current crew size |
| `"morale"` | 1 | crew mood, 0–4 (use with `@HAPPY`) |
| `"plunder"` | 1 | undivided plunder — the share not yet split, not your total wealth |
| `"months"` | 1 | months at sea |
| `"nearestCity"` | **3** | the nearest port's name record (use with `@CITYNAME`) |
| `"nearestCityNation"` | 1 | which nation holds it (use with `@NATIONALITY`) |
| `"nearestCityType"` | 1 | village / town / city (use with `@LOCTYPE`) |
| any integer | 1 | that literal number |

```json
"body": "You have @NUM mouths to feed after @NUM months at sea.",
"args": ["crew", "months"]
```

An event may use at most **8 argument slots** in one piece of text.

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

Events will not interrupt you in a town, a menu, a battle or a mini-game.
`elapsedSailing` and `nearPort` additionally require that you are **under way**
in the overworld; `stateCrosses` only requires that a career is loaded, because
a crew going hungry is worth saying wherever it happens.

Three trigger types exist today — `elapsedSailing`, `nearPort` and
`stateCrosses`. Any of them can fire either kind of event, and any of them can
fire an anchored notice.

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

### `stateCrosses`

Fires when a live game value **crosses** a threshold — your crew falling away,
your purse running dry, morale souring, a voyage dragging on.

```json
{ "type": "stateCrosses", "field": "morale", "below": 2 }
```

| Field | Meaning |
|---|---|
| `field` | which value to watch: `crew`, `gold`, `morale` or `months` (required) |
| `below` | fire when the value drops **under** this |
| `above` | fire when the value rises **over** this |
| `cooldown` | seconds before it may fire again |
| `once` | `true` to fire at most once per career |

Give **exactly one** of `below` or `above` — both, or neither, is rejected at
load time.

`morale` runs `0`–`4`: `0` mutinous, `1` angry, `2` content, `3` happy,
`4` devoted. `gold` is undivided plunder — the share not yet split with the
crew, not your total wealth. `months` is months at sea this voyage.

**It is edge-triggered, like `nearPort`.** It fires on *crossing* the threshold,
and cannot fire again until the value crosses back out. A value that merely sits
past the line does not re-fire every frame, so you rarely need a `cooldown`.

One consequence worth knowing: if a value is *already* past the threshold when a
career loads, it fires straight away. If that is not what you want, pick a
threshold play will cross rather than start behind, or add `"once": true`.

```json
{
  "id": "purse_running_dry",
  "kind": "notice",
  "trigger": { "type": "stateCrosses", "field": "gold", "below": 100 },
  "body": "The purse is near empty, Captain.",
  "seconds": 5,
  "anchor": "ship"
}
```

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

## Debug hotkeys

Triggers are live, so events fire during normal play. These remain for testing
your own content without waiting for the conditions to come around:

| Keys | Fires |
|---|---|
| Ctrl+Shift+1 | the first event in the file |
| Ctrl+Shift+2 | the second |
| Ctrl+Shift+3 | a test notice at the top of the screen |
| Ctrl+Shift+4 | a test notice anchored over your ship |

## What is not here yet

Honest about the edges, so you do not spend an evening on something that cannot
work:

- **Sound on events.** The `"sound"` field is not live; it is ignored if
  present.
- **Tokens beyond the five listed above.** Ship names, pirate names and dates
  exist in the engine but are rejected until their argument counts have been
  read out of its code.
- **Tokens in option text.** `text` on an option is plain text only.
- **Triggers on battle outcomes, cargo, or reputation.** Today: elapsed sailing,
  proximity to a port, and a live value crossing a threshold.
