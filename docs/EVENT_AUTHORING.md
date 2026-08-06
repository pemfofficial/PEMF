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
      "body": "A hush falls over the deck. Your crew of {crew} stands {morale}.",
      "options": [
        { "text": "Say nothing and hold your course." }
      ]
    }
  ]
}
```

In game that renders as a card in the game's own style, with `{crew}` replaced
by your crew size and `{morale}` by their mood. You do not declare those
anywhere — see [Placeholders](#placeholders--the-easy-way).

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
  "body": "Land ho! {port} off the bow!",
  "seconds": 5,
  "anchor": "ship"
}
```

| Field | Notice |
|---|---|
| `body` | the text; [placeholders](#placeholders--the-easy-way) work as usual |
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
- **Notices only appear in the sailing view.** Not in a town, not in a menu, not
  in battle — a notice is drawn against the world, so anywhere else it would be
  painted over whatever is on screen. This applies to both `"screen"` and
  `"ship"` notices.
- **`seconds` counts only while the notice is actually visible.** Its clock is
  held whenever the overworld is off screen, so a player who opens the map with
  a notice up comes back to it with the time it had left, rather than to nothing.
  You can write a 4-second notice and trust it will be seen for 4 seconds.
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
| `args` | rarely | Only needed if you write `@` tokens by hand instead of `{placeholders}`. |
| `options` | yes | 1–6 choices. Three have been tested in game; more than three is permitted but untried. |

### Option

| Field | Required | What it does |
|---|---|---|
| `text` | yes | The selectable line. A leading space is added for you. Plain text only — tokens are not substituted here. |
| `effects` | no | What happens to the game if picked. |
| `outcome` | no | Follow-up text shown after the choice, as another page of the same card with a Continue. |
| `outcomeArgs` | rarely | As `args`, for the outcome text. Placeholders work here too. |

---

## Placeholders — the easy way

Write a `{placeholder}` in your text and PEMF fills it in. **No `args`, no
counting, nothing else to get right.**

```json
{
  "id": "landfall_sighted",
  "kind": "notice",
  "trigger": { "type": "nearPort", "distance": 3000 },
  "body": "Land ho! {port} off the bow!",
  "anchor": "ship"
}
```

> *"Land ho! Nevis off the bow!"*

| Placeholder | Becomes |
|---|---|
| `{crew}` | your crew size |
| `{morale}` | the crew's mood — `DEVOTED`, `CONTENT`, … |
| `{gold}` | undivided plunder — the share not yet split, not your total wealth |
| `{months}` | months at sea this voyage |
| `{port}` | the nearest port's name — `Nevis`, `Port Royale`, … |
| `{portNation}` | who holds it — `Spanish`, `English`, … |
| `{portType}` | `village`, `town`, `city`, … |

Use as many as you like, in any order:

```json
"body": "The {portNation} {portType} of {port} lies ahead, and your crew of {crew} stands {morale}."
```

Two small rules:

- **The `{port…}` placeholders name the port nearest your ship** when the event
  fires — not the one you are steering for. They pair naturally with the
  `nearPort` trigger, where those are the same thing. With no port in range they
  come out blank, so word the line to survive that, or use `nearPort`.
- Need a literal `{`? Write `{{`.

That is the whole of it. **If you only read one section, read this one** — the
rest of this page is the machinery underneath, which you need only if you want
it.

---

## Tokens — the engine underneath

Placeholders compile down to the game's own `@` tokens, which you may also
write directly. There is no advantage to doing so, and one real hazard, but it
is documented because the log talks in these terms when something is wrong.

| Token | Slots | Supply with |
|---|---|---|
| `@NUM` | 1 | `"crew"`, `"gold"`, `"months"`, or an integer |
| `@HAPPY` | 1 | `"morale"` |
| `@CITYNAME` | **3** | `"nearestCity"` |
| `@NATIONALITY` | 1 | `"nearestCityNation"` |
| `@LOCTYPE` | 1 | `"nearestCityType"` |

```json
"body": "Land ho! @CITYNAME off the bow!",
"args": ["nearestCity"]
```

**The number of argument slots the tokens need must match `args` exactly.** The
engine reads arguments positionally, like `printf`; a token with nothing behind
it reads whatever happens to be in memory. The loader refuses the event rather
than allow it, and tells you both numbers.

### Why `@CITYNAME` costs three

A city's name is not text — it is a **record of three values** the engine
assembles into a name. So `@CITYNAME` consumes three arguments. One
`"nearestCity"` arg supplies all three, so this is correct:

```json
"body": "The @NATIONALITY @LOCTYPE of @CITYNAME lies ahead.",
"args": ["nearestCityNation", "nearestCityType", "nearestCity"]
```

Three tokens, three args, **five slots**. The loader counts slots. A maximum of
**8 slots** is available in one piece of text.

This is exactly the sort of detail `{port}` exists to spare you.

### Don't mix the two

Placeholders supply their own values, so an event using `{...}` must not also
give an `args` list. The loader rejects that rather than guess at the order.

### Other tokens

The game has more — ship names, pirate names, dates. They are **not permitted**,
and the loader rejects them by name.

Each token consumes a specific number of arguments, and that number is only
knowable by reading the game's own code. `@CITYNAME` needing three rather than
one is precisely the sort of thing that looks fine, loads fine, and then reads
stack garbage. A token is enabled here once its appetite has been read out of
the disassembly — never guessed. When one is, it gets a `{placeholder}` too.

## Argument values

Only needed when writing `@` tokens by hand.

| Value | Slots | Meaning |
|---|---|---|
| `"crew"` | 1 | current crew size |
| `"morale"` | 1 | crew mood, 0–4 (use with `@HAPPY`) |
| `"plunder"` | 1 | undivided plunder |
| `"months"` | 1 | months at sea |
| `"nearestCity"` | **3** | the nearest port's name record |
| `"nearestCityNation"` | 1 | which nation holds it |
| `"nearestCityType"` | 1 | village / town / city |
| any integer | 1 | that literal number |

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

## Menu rows — your own option in the town menu

An event does not have to wait for the world to fire it. A **menu row** puts
your own line in the game's town menu, so the player can ask for it.

Rows live in the same file as your events, in a top-level `menuRows` array:

```json
{
  "events": [
    {
      "id": "harbourmaster_word",
      "body": "The harbourmaster of {port} keeps a ledger...",
      "options": [ { "text": "Pay for a look." } ]
    }
  ],

  "menuRows": [
    { "label": "Ask after the harbourmaster", "event": "harbourmaster_word" }
  ]
}
```

| Field | Meaning |
|---|---|
| `label` | the line the player sees. Plain ASCII, same rule as event text. |
| `event` | the `id` of the event this row fires. It may live in another file. |

A file may contain `menuRows` and no `events` at all, if the events it points
at are defined elsewhere.

**Where the row appears.** Just above the row that leaves the settlement —
*Sail away*, or *Leave Town* depending on where you are. Every one of the game's
own options keeps the position it has always had, and leaving stays at the
bottom where players reach for it.

**When the event happens.** Immediately, in the port, exactly like the game's
own menu options — the town is behind the card, and you are returned to the
menu afterwards. Its outcome follows straight away rather than waiting.

*(This changed. Menu rows used to queue the event for later, which meant the
card appeared over open sea after you sailed, and picking the row looked like it
had done nothing. Rows present in place now.)*

### Limiting a row to one port

Add `port`, `nation`, or both. A row with neither is offered everywhere.

```json
{ "label": "Ask after the harbourmaster",
  "event": "harbourmaster_word",
  "port": 19 }
```

| Field | Meaning |
|---|---|
| `port` | the settlement's index in the game's table. Offered only there. |
| `nation` | the owning nation's index. Offered only in that crown's ports. |

**Finding a port's number:** enter it once and read `pemf.log`. Every menu
prints what it is looking at before it does anything:

```
townmenu: menu #1 -- port 19 (nation 1), 7 game row(s), 245 bytes, form -1 -- 1 PEMF row(s) offered here
```

The index is used rather than the name because names are not unique or stable —
map mods rename and move settlements, and two can share a name. The number is
the settlement.

A row that is filtered out never reaches the screen and never takes up a
position, so the rest of the menu is unaffected.

**Six rows maximum**, across all files. A menu that runs past the bottom of the
screen is not a feature.

**If your row does not appear**, check `pemf.log`. A row naming an event that
does not exist is rejected by name at load, rather than silently doing nothing
when picked:

```
townmenu: REJECTED row 'Ask after the harbourmaster' -- no event with id 'harbourmster_word'
```

---

## Menus — your own screens

A menu row can open a **menu** instead of firing an event, and a menu's options
can open further menus. That is enough to build a whole feature out of data.

```json
{
  "menuRows": [
    { "label": "Manage yer crew!", "menu": "crew_root" }
  ],

  "menus": [
    {
      "id": "crew_root",
      "title": "Your crew of {crew} stands {morale}. Do you...",
      "options": [
        { "text": "Look over the roster", "menu":    "crew_roster" },
        { "text": "Speak to the bosun",   "event":   "bosun_asks_for_rum" },
        { "text": "Count the shares",     "outcome": "There is {gold} undivided." }
      ]
    },
    {
      "id": "crew_roster",
      "title": "The roster.",
      "options": [
        { "text": "Close the book", "outcome": "You close the book." }
      ]
    }
  ]
}
```

### A menu

| Field | Meaning |
|---|---|
| `id` | unique name, used by rows and other menus to point at it |
| `title` | the prose at the top. [Placeholders](#placeholders--the-easy-way) work. |
| `options` | 1–5 of them |

### An option

Each option does **exactly one** of three things — not none, not two:

| Field | What happens |
|---|---|
| `menu` | opens that menu. Returning from it brings you back to this one. |
| `event` | fires that event, card and outcome, then closes the menu. |
| `outcome` | shows a closing card with that text. Placeholders work. |

`text` is the selectable line, and like an event's option text it is plain —
placeholders are not substituted there.

### Things that are handled for you

- **A way out.** `Never mind.` is added as the last row of every menu. You do
  not write it, and you cannot lose it — which is why the limit is five options
  rather than six.
- **Going back.** `Never mind.` in a submenu returns to the menu above it, one
  level at a time. From the top menu it returns to the town.
- **The backdrop.** Every screen is drawn against the port you are standing in,
  through the game's own card renderer.
- **Depth.** Six levels. A cycle in your data — A opens B opens A — is walkable
  by the player as long as they like; it just cannot recurse past six.

### When something is wrong

Everything is checked at load, and reported by name:

```
content: REJECTED menu 'crew_root' -- needs an 'options' array
content: REJECTED menu 'crew_root' option 'Look over the roster' -- needs exactly one of 'menu', 'event' or 'outcome' (found 2)
content: menu 'crew_root' option 'Speak to the bosun' -- no event with id 'bosun_asks_for_rm'
townmenu: REJECTED row 'Manage yer crew!' -- no menu with id 'crew_rot'
```

Menu titles and outcomes go through the **same** token and ASCII validation as
event text, for the same reason: the engine reads arguments positionally, and
one `@` token too many reads stack garbage.

---

## Effects

What an option does to the game.

| Op | Meaning |
|---|---|
| `addPlunder` | add to undivided plunder (negative to spend) |
| `setPlunder` | set it outright |
| `addCrew` | add or remove crew |
| `setCrew` | set crew size |
| `addMorale` | move the crew's temper by this many points, up or down |

**About `addMorale`.** That is points on PEMF's own crew scale, which runs from
−100 to +100 across seven states from `MUTINOUS` to `DEVOTED` — not the game's
0–4 level. For a sense of scale: cargo lost in a storm is **−2**, losing men is
up to **−12**, and dividing a good haul of plunder is up to **+25**. An event
worth more than that should be a rare one.

There is deliberately no `setMorale`. An event describes something that
*happened* to the crew; "the men are now exactly content" is not something that
happens.


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
