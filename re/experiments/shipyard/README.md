# The shipyard: making ships, and giving them somewhere to go

## The short version

**PEMF can build a vessel of any nation, at any port, and send it somewhere.**
Verified in a running game, using the engine's own ship factory.

That is the consequence false colours needed. Being unmasked can mean a crown
*dispatches a warship*, which is what the game itself does when it decides you
are a problem — it does not turn a passing merchant hostile, it builds a hunter.

The route here went through two wrong ideas. Both are written down, because the
second cost a live test to find and the mistake behind it is repeatable.

## Five things worth carrying away

1. **There is no hostility flag, and looking for one was the mistake.** Nothing
   in the binary reads the player's nationality. The game expresses "this crown
   is angry with you" by *creating a ship*, not by flagging one.
2. **The lever is the DESTINATION, not the role.** Every builder hands back a
   ship whose destination equals the port it was built at. It has arrived. That
   is the whole reason the first spawns looked broken.
3. **Derive an offset, then check the arithmetic against a known value.** Three
   field offsets were subtracted wrong. The first live test consequently
   reported every ship as role 0 — including two built by functions that
   demonstrably write 4 and 3.
4. **When one reading is wrong and another is right, the right one is the
   clue.** The flags came out distinct per builder throughout. That said plainly
   that the calls were fine and the readout was not, and it was not noticed.
5. **A game's own function can be a landmine.** One builder reads a home port
   from a global it never validates, and builds ships at a null settlement.

---

## The factory

### Slot allocation

| Slots | Use |
|---|---|
| 0 | the player |
| 1–7 | reserved |
| 8–255 | the AI pool |

A free record is marked by a **type word of `-1`** at `+0x00`. The allocator
walks from slot 8 and stops at `0x00859EF8` — which is `0x008142F8 + 256 *
0x45C`, a third independent confirmation that the array holds 256.

Slots are **reclaimed**: the free count fluctuates upward between spawns as the
game retires vessels, so nothing leaks.

### The family

Three callable builders, each producing a complete ship:

| Function | Args | Role | Flags | Nationality |
|---|---|---|---|---|
| `FUN_00414FC0` | `(city, kind)`, cdecl | 3 | `0x800` | from the city |
| `FUN_00415290` | `eax = city`, type on stack | 4 | `0x200` | from the city |
| `FUN_004154F0` | `(type)`, cdecl | 3 | `0x1C00` | hardcoded `0`, port from a global |

`FUN_00415290` takes its city in a register and its type on the stack — a mixed
convention no compiler emits, so it needs a naked shim.

Roles **1 and 2 cannot be produced by any callable builder**. They are written
only inside the encounter spawner (`FUN_00413710`) and the governor's dispatch,
both too entangled to call. They can be stamped afterwards, which is the only
reason their behaviour could be tested at all.

### Verified in game

```
shipyard:   H = role 0 (0x00414FC0) MATCHES
shipyard: building at city 26 (English), 221 free slot(s) before
shipyard: factory returned 17, 220 free slot(s) after (delta 1)
shipyard: slot 17 -- type 11, nationality 1 (English), flags 0x00000800
```

A slot consumed, the port's nationality inherited, the ship placed at the city.
An English port gives an English ship; a Spanish port a Spanish one.

---

## The finding: destination is the lever

Every builder returns a ship whose **destination city is the port it was built
at**:

```
slot 20 AS BUILT -- ROLE 4, dest city 0, home city 0
```

It has arrived. So it does what an arrived ship does — turns back into port, or
sits at anchor. Neither was misbehaving; neither had anywhere to be.

Write a different city into `+0x3E` and **she sails for it**:

```
slot 20 ORDERED -- role 1, dest city 72 (Spanish, 87828 away)
```

Tested across all four role values. **Every one travels once it has a
destination**, so role does not gate movement.

### What that makes possible

A pirate hunter is a ship built at the offended crown's nearest port, with a
destination past the player. No hostility flag is required — and since there is
none to find, this is *the* mechanism rather than a workaround.

**Still open:** a hunter sails to a *place*, not at a *ship*. Pointing it past
the player reads as pursuit, but whether true target-chasing exists in the
overworld has not been established.

---

## Two wrong turns

### Roles looked like the answer and were not

Three builders writing three different roles is a strong hint that role means
"what this ship is for". It does not appear to gate anything observable. Ships
sat still under every value until they were given somewhere to go.

The hint was not wrong, exactly — role is clearly *a* classification. It simply
was not the thing standing between us and a moving ship, and chasing it first
cost two test rounds.

### Three offsets, subtracted wrong

Role, destination and home city were recorded as `+0x22`, `+0x36` and `+0x38`
against a base of `0x008142F8`. The correct values are **`+0x2A`, `+0x3E` and
`+0x40`**.

The consequence in the log:

```
slot 17 -- nationality 1 (English), role 0, flags 0x00000800
slot 17 -- nationality 1 (English), role 0, flags 0x00000200
slot 20 -- nationality 0 (Spanish), role 0, flags 0x00001C00
```

Every ship "role 0", including two from builders that write 4 and 3 — while the
**flags came out correctly distinct for all three**. That contrast was the
evidence that the calls were right and the readout was wrong, and it went
unread for a round.

**The rule that follows:** derive an offset by subtracting, then verify the
arithmetic against a field whose value is already known. The player record was
confirmed exactly this way — crew at `+0x40` reading 40 and gold at `+0x44`
reading 600 — and the ship record was not.

---

## A landmine in the game's own code

`FUN_004154F0` reads its home port from the global at **`0x00722A08`** and does
not validate it. In a fresh career that global reads `-1`, and the builder
happily constructs ships at a null settlement:

```
slot 20 -- nationality 0 (Spanish), flags 0x00001C00
slot 20 at (1000,2000); the player is at (441505,72414)
```

Map position `(1,2)` — the corner of the world. A handful of those took the game
down.

PEMF now checks the global before calling and refuses with a reason. Worth
remembering generally: **an engine function being callable does not mean it
validates its own inputs**, and the ones that read globals rather than taking
parameters are the ones to distrust.

---

---

## The field that mattered was a different one

`+0x2A` was called "role" here for three test rounds. The classification the
game actually uses is **`+0x02`**, and the hover label gives it away completely
(`0x00462098`, jump table at `0x00463CB4`):

| `+0x02` | Label |
|---|---|
| 0 | ordinary merchant |
| **1** | **pirate-hunter** |
| 2 | privateer |
| 3 | raider |
| 4 | smuggler |
| 5, 6 | written, unlabelled |

**A pirate hunter is a value in a field.** Not a behaviour to be written, not a
hostility flag to be found — a `1` at `+0x02`.

### The symptom that should have caught this a round earlier

A spawned ship showed **no hover text at all**. That was reported and passed
over. It is exactly what a ship with an unnamed `+0x02` looks like, and chasing
it straight away would have led here without the intervening rounds.

**Unexplained readings are data.** "Nothing appeared when I hovered it" is a
measurement of the classification field, and it was treated as an absence.

---

## Reputation is the hostility model

Recorded here because it was found in the same sweep and it changes what a
spawned hunter is *for*.

**Negative reputation with a nation is that nation being hostile to you** — the
game's own model, per-nation rather than per-ship, which is why a search for a
per-ship flag came up empty.

| Reputation `0x00869A78 + nation*2` | Effect |
|---|---|
| `> 0` | welcome |
| `< 0` | that nation's settlements are **skipped** by the port search (`0x00406265`, and five sibling sites) |
| `< -1` | *"the @NATIONALITY have put a price on your head"*, and an amnesty is offered (`0x0040B5DC`) |

Related machinery already in the game: `"Pirate hunter sails from @CITYNAME."`,
`"there's a price on your head in @CITYNAME"`, `"@NATION offers Pirate
Amnesty."`, and a Jesuit priest who travels to a named port to arrange one.

Writers: `0x00405010`, `0x00405029`, `0x00405050`, `0x00405085`, `0x004051DC`,
`0x00405AA6`, `0x0040D5AF`, `0x0040D86E`. Reputation is **player state**, inside
the 216-byte saved player record — a far smaller thing to write than the nation
relations matrix, which is world state.

## Open

* **True pursuit.** Whether a vessel can be pointed at a *ship* rather than a
  place. Nothing found yet; re-targeting the destination as the player moves is
  the fallback and would probably read convincingly.
* **Roles 1 and 2.** Producible only by stamping. What they classify is still
  unknown — role 2 is what the governor's blockade dispatch uses.
* **Ship type.** `kind`/`type` was always passed as `0x0B`, the one value the
  game itself is known to pass (the governor's blockade at `0x0040DA9A`). The
  range and meaning of the other values are unmapped, and they presumably choose
  the vessel class.
* **Whether spawned ships survive a save.** They are written into the ship array
  and the array is serialised, so they should. Untested.
