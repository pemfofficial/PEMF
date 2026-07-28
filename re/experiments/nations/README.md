# Nations: who hates whom, and which crown you serve

## The short version

Two things stood in front of the Suspicion system, and both had been written
down as unlocated. Both are now found, and neither was where the search had
been looking.

1. **Nation relations** live in an 8x8 int32 matrix at **`0x0085A168`**, where
   `1` is war and `-1` is a treaty. It is live state, not a display cache.
2. **The nation the player serves** is an int16 at **`0x00869AA8`**. The reason
   it went unfound for so long is that *the game does not store your choice at
   character creation* — it stores a consequence of it, and recomputes it.

A third thing was disproved along the way: the ship record's nationality field
is **not** the player's nation, and writing it is not the lever a disguise
needs. That had been the plan of record.

## Five things worth carrying away

1. **"Not stored anywhere" often means "stored as something else."** The chosen
   faction is never written as a choice. It is derived from the rank array and
   cached. Searching for the *choice* could not have succeeded.
2. **When the engine already computes an answer, read the answer.** The
   derivation this project designed independently turned out to be the exact
   algorithm at `0x0040D690`. Being right about the method was not a reason to
   use it over the value the game itself acts on.
3. **A save serializer is a map of every piece of state a game owns.** One
   function gave exact sizes for twenty blocks and settled two open questions
   as a side effect.
4. **A value that is reliably zero is evidence, not a gap.** Four careers under
   four crowns all reading `0` is what *disproved* the ship-record theory.
   A single career would have looked like a bad measurement.
5. **A stale flag is a bug even when every value in it is correct.** See the
   town-screen fault at the end.

---

## The relations matrix

**`0x0085A168`, 8x8 int32, `[a * 8 + b]`.**

| Value | Meaning |
|---|---|
| `1` | at war |
| `-1` | treaty |
| `0` | neutral |

The size is not inferred. The new-game reset at `0x00404229` clears it with
`mov ecx, 0x40 / mov edi, 0x85A168 / rep stosd` — exactly 64 dwords — and the
save serializer writes it out as exactly `0x100` bytes. Two independent
statements of the same 256.

The indexing came from gameplay sites that reach it through a **city's** nation
byte, which pins the stride and the meaning together:

```
0x0040C9A0  movsx ecx, byte [ebp + 0x860B74]   ; city nation
0x0040C9A7  lea   edx, [ebx + ecx*8]
0x0040C9AA  cmp   dword [edx*4 + 0x85A168], 1  ; ...at war with ebx?
```

with the mirror at `0x0040CB2E` comparing against `-1`. The Pedia's
"International Relations" page (`FUN_0043cde0`) draws the same grid from the
same memory, which is what made it findable at all.

**Slots 4 and 5 are permanently at war with all four crowns.** The reset loop at
`0x004042E0`-`0x0040431D` writes `1` into row 4, row 5, and columns 4 and 5 of
every nation's row. Slot 4 is Pirate; slot 5 is a sixth power this project has
not needed to identify. In game they read as a solid wall of war — **and as
neutral toward each other**, which was not predicted.

### Verified live

Two probes 24 seconds apart, same career:

```
11:56:08              11:56:32
  Sp En Fr Du           Sp En Fr Du
Sp -  W  W  W         Sp -  .  .  W
En W  -  W  .         En .  -  .  .
Fr W  W  -  W         Fr .  .  -  W
Du W  .  W  -         Du W  .  W  -
```

Three wars ended between those reads. That is what makes `1 = war` a
measurement rather than a plausible reading of a constant.

---

## The player's standing

Two parallel `word[4]` arrays in the player record, both indexed `nation * 2`:

| Address | Meaning |
|---|---|
| `0x00869A78` | reputation with nation N |
| `0x00869A88` | rank with nation N; `0` = no letter of marque |

Found together at the promotion check, `0x0040D38F` onward:

```
movsx eax, byte [ebp + 0x860B74]      ; nation
cmp   word [eax*2 + 0x869A88], 9      ; already at the top rank?
cmp   word [eax*2 + 0x869A78], 3      ; reputation high enough?
inc   word [eax*2 + 0x869A88]         ; promote
```

Ranks run 0..9 and index a `char*[10]` at `0x007272B4`: Grunt, Grunt, Captain,
Major, Colonel, Admiral, Baron, Count, Marquis, Duke. Rank `0` selects the "you
hold no commission here" governor dialogue (`0x0040C90C`, `0x0040C92E`,
`0x0040CAB2`).

**Both arrays read 0 across every career tested**, because a captain who has not
visited a governor holds no commission from anyone. That is the correct value,
not a failed read — but it means these arrays cannot answer "which crown do you
serve" at the start of a career, which is what the plan had assumed.

---

## The nation the player serves

**`0x00869AA8`, int16.**

The write, at `0x0040D690`, is the whole story:

```
esi = 1                                        ; assume this crown is highest
for (eax = 0; eax <= 3; ++eax)                 ; every crown
    if (eax != ecx && rank[eax] >= rank[ecx])
        esi = 0                                ; someone ranks as high or higher
if (esi)
    PlayerNation = ecx                         ; strictly the highest rank wins
```

So "your nation" is the crown you **strictly outrank the others with**, and this
global is where the engine caches its own answer. It is recomputed whenever a
rank changes, and written directly at character creation.

This is why the search kept failing. It was looking for a value written *once*,
at creation, recording a *choice*. There is no such value.

### Confirmed four ways

1. **The write above** — derived from the rank array, cached here.
2. **Compared straight against a city's nation byte** at `0x0040DA19`:
   `movsx dx, byte [ecx + 0x860B74]` / `cmp word [0x869AA8], dx`.
3. **Passed where `@NATIONALITY` is expected** at `0x0040FF62`, for the string
   *"We do not trade with @NATIONALITY heretics."*
4. **Measured**, 2026-07-28. Four careers begun under four crowns, read at
   player-record offset 56:

   | Career | Chosen | Value | Enum |
   |---|---|---|---|
   | 1 | English | `01` | `kEnglish = 1` |
   | 2 | Dutch | `03` | `kDutch = 3` |
   | 3 | Spanish | `00` | `kSpanish = 0` |
   | 4 | French | `02` | `kFrench = 2` |

   The same enum as the flag-mesh table, with no exceptions.

`nations::HomeNation()` reads it. `nations::DeriveHomeNation()` reimplements the
engine's rule and is kept purely as a **cross-check** — if the two disagree once
ranks are non-zero, one of the addresses is wrong and the log says so on the
spot rather than months later.

---

## What was disproved

### The ship record's nationality is not the player's nation

`ShipRecord(0) + 0x04` read **`0` in four careers begun under four different
crowns**. It does not track the nation you chose; it is simply never set for
entry 0.

This mattered, because the plan of record was to write that field alongside the
flag texture and let the engine's own AI react. That plan is dead: a game
deciding hostility from a field that is always `0` would treat every player as
Spanish, which is plainly not its behaviour.

The field remains a real find **about AI vessels** — 84 code sites read it, and
it is the colours a vessel is seen to fly. It is just not this.

### The staging buffer is not a record of the choice either

`0x0072C6B8`, 184 bytes, read all zeros mid-career. The save serializer explains
why: it is written out **only when a mode word is 2**, which is character
creation. It is the *pending* career record, live only before a career exists,
and the copy at `0x00401BA6` moves it into `0x00869A70` when the career begins.

---

## The save serializer, and what it settled

`0x00401400` pushes `(address, size)` for every block the game persists and
calls read-or-write on each. It is, in effect, a complete map of game state.
Two open questions fell out of it for free:

| Block | Size | What it settles |
|---|---|---|
| `0x0085A168` | `0x100` | the relations matrix is exactly 8x8 int32 |
| `0x00869A70` | `0xD8` | the player record is exactly 216 bytes |
| `0x0072C6B8` | `0xB8` | the staging buffer is 184 bytes, mode 2 only |
| `0x008142F8` | `0x45C00` | **the ship array is 256 slots**, not the 24 we scan |
| `0x00860B68` | `0x1000` | 128 settlement records, confirming `kMaxCities` |

The player-record bound is what made the faction hunt finite: a 216-byte window
to diff across four careers, rather than an open search. Offsets `64` and `68`
independently landed on crew `40` and gold `600`, which is how the base was
confirmed before the diff was trusted.

**The 256-slot ship array is a standing correction.** Every loop in this
framework walks indices 1..23. That has been a serviceable window on the water
near the player, never a correct one, and anything needing the whole sea has to
know better.

---

## Two framework faults found on the way

### A stale flag blanked the town screen

Entering a port with a notice on screen left the town rendered with no UI text
at all.

`content::g_worldLive` is published once per main-loop iteration. The render
hook runs far faster — the heartbeat measured **57,221 EndScene calls in 15
seconds** in town against ~3,000 while sailing. A town entry happens *between*
two safe points, so for that window the flag still said "the overworld is on
screen", `DrawNotices()` went on running against the town, and its shared-buffer
clear wiped the text the town screen had just composed.

It only showed when a notice was live, because an empty notice list
short-circuits before the clear — which is exactly what made it look like a
notice bug rather than a staleness bug.

Fixed twice over:

* the published flag now carries **the screen signature it was decided from**,
  and the render phase re-checks that the screen is still that one. Two int
  reads per frame, and it closes the whole class rather than this instance.
* the shared buffer is cleared **only when we actually drew into it**. It is the
  game's buffer; if this pass put nothing in it, there is nothing of ours to
  take out, and clearing anyway is a write with no justification.

### Triggers assumed "armed" instead of observing it

Four identical anchored notices drew on top of one another and came out bold,
which reads as a font fault rather than a duplicate.

```
12:24:05.708  'landfall_sighted' near port (city 1 at dist 2896)
12:24:07.447  'landfall_sighted' near port (city 1 at dist 2896)
12:24:08.446  'landfall_sighted' near port (city 1 at dist 2896)
12:24:10.569  'landfall_sighted' near port (city 1 at dist 2896)
```

An identical distance four times: the ship never moved. A fresh `Runtime` starts
`armed = true` because nothing has been observed yet — fine at sea, wrong in a
harbour. Each career change reset the triggers while the ship sat inside the
3000 radius, arming a trigger whose entering edge had long since passed.

The first evaluation after a reset now **observes rather than fires**: it sets
`armed` from the world as it actually is. Applied to `stateCrosses` as well,
which had the identical fault — a career beginning already below a threshold has
not crossed it in front of us.

Two supporting fixes: notices are cleared on a career change (a lookout's call
from the previous captain has no business over this one's ship), and identical
resolved text can no longer occupy two slots — reposting refreshes the existing
notice's clock.

Verified: twelve `starts disarmed -- already inside the radius` lines across a
run of career switches, and no fires at all.

---

## Open

* **What the AI actually uses to decide hostility toward the player.** The ship
  nationality lever is gone, so this is once again unlocated. The candidates not
  yet followed: ship-record `+0x58` flags, where bit `0x400000` is set on
  vessels spawned *because* two nations are at war (`0x0040AFF8`); and the
  ship-label ids 10 vs 11, selected "depending on nationality standing".
* **Slot 5 of the relations matrix.** At war with every crown, neutral toward
  pirates.
* **Whether rank and reputation move as expected.** Both read 0 in every career
  tested so far, which is consistent but not yet discriminating. A governor
  granting a letter of marque is the test.
