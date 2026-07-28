# Which career am I in, and is this state its own?

## The short version

PEMF keeps its own state per save, in a sidecar. For that to be correct it has
to answer two questions: **is a career running**, and **did this career come
from a save or is it new?**

Both looked trivial. Both were wrong in ways that took six attempts and a live
trace to see. The symptom that surfaced them was a flag — a disguise following
the player into careers it had no business in — but the same faults were
silently corrupting event counts and trigger progress the whole time.

**Two pre-existing bugs turned up on the way, neither of which had anything to
do with flags:**

- **Opening the Load/Save screen replaced your live state.** The screen opens
  every save it can see in order to list them, and each read was being applied
  on the spot. Merely *looking* at the load screen overwrote the running
  career's state with whichever save was listed last.
- **Loading a save from inside a career never applied its state.** The commit
  was tied to a career *transition*, and loading while already at sea does not
  produce one. The player kept the previous career's history on a freshly
  loaded save.

**The conclusion that matters: ask the game, not the filesystem.**

## Four things worth carrying away

1. **A file being read is not an event.** It is an access. What it *means*
   depends on state the filesystem cannot see.
2. **`crew > 0` is not "in a career".** The crew count does not reset when the
   player returns to the main menu, so anything built on it works exactly once
   per session and then silently stops.
3. **Verify a write landed, not that a symbol exists.** The fingerprint feature
   was complete except for the three lines that wrote it. A grep for the field
   name matched the struct, the parser and the comparison — three hits, all
   real, and the feature still did nothing.
4. **Trace the inputs, not the outcomes.** Five fixes were reasoned from logs
   showing only what was decided. One log showing what the decision was *made
   from* ended it in a single read.

---

## What was tried, and how each failed

### 1. "A save file was read, so a load happened" ❌

The original design. `OnSaveFileOpened(read)` applied the sidecar immediately.

**Fails because the Load/Save screen reads every save to list them.** Measured:
seven files opened inside 13 ms. Each was applied in turn, so the live career
ended up holding whichever save sorted last.

### 2. "A burst is browsing; a lone read is a load" ❌

The listing is a burst, and a genuine load stands alone — in one session the
real load arrived 1.6 s after the burst, by itself. So: discard bursts, stage
lone reads.

**Fails twice.** Browsing *slowly* — clicking saves one at a time to preview
them — produces lone reads too, seconds apart. And more fundamentally:

**Starting a brand new career also reads a save file.** A player who went
straight to New Career, never touching Load/Save, still produced a lone read of
`slot1` 3.1 s before the career began — indistinguishable from a load in every
respect. **No refinement of file-watching can work.** That is the result that
retired the whole approach.

### 3. "Commit when a career begins" ❌

Stage on read, apply on the out-of-game → in-game edge.

**Fails for loads made from inside a career**, which never cross that edge. The
loaded state was never applied at all, and then sat staged until the *next*
career entry — usually a new career, which swallowed it. This is what made a
fresh game start under the previous captain's colours.

### 4. "`state::InGame()` tells me a career is running" ❌

`InGame()` is `crew > 0`.

**The crew count does not return to zero at the main menu.** So after the first
career of a session, `InGame()` is true forever: no "left the career", no career
entry, and every subsequent career inherits the last one's state. A session that
abandoned a career and started another produced **no transition at all**.

This one is worth dwelling on, because it had been load-bearing since the
framework's first milestone and looked obviously correct.

---

## What works

### Career presence comes from the screen state

The same `ScreenId`/`ScreenDepth` pair that gates notice drawing
(see [`../screen_state/`](../screen_state/README.md)):

| Depth | Screen |
|---|---|
| 1 | main menu |
| 2 | character creation |
| 3 | sailing, town |
| 4 | Load/Save |
| 4–5 | battle |

`session::InCareer()` is `crew > 0 && depth >= 3`. That gives real transitions
in both directions.

### A read stages; a fingerprint commits

The sidecar carries a fingerprint of the career it belongs to — months at sea,
gold, crew — stamped at save time. A staged sidecar is applied **only** when
that fingerprint matches the career on screen.

- **New career:** months 0 with starting gold and crew, which will not match a
  career that has been played. State discarded, career begins empty.
- **Real load:** the values match once the save is in memory, and it commits —
  including loads made from inside a career, which no longer depend on a
  transition.
- **Browsing:** nothing matches, nothing is committed, the live career is
  untouched.

**Known limit, stated rather than discovered later:** two saves made at the same
moment in near-identical careers have the same fingerprint and cannot be told
apart. It separates "a played career" from "a fresh one", which is what the
correctness of the sidecar actually depends on. If finer resolution is ever
needed, the game date is the natural addition.

---

## The trace

`session/trace` prints the decision's inputs — career presence, screen depth,
what is staged, what is committed, the live fingerprint and the staged one:

```
session/trace: inCareer=1 crew>0=1 depth=4 staged=1 committed=0 |
               live months=0 gold=600 crew=40 |
               staged fp months=-1 gold=-1 crew=-1 flag='flag_fre.dds'
```

It logs on a change to any value that drives a branch, and once a second while
something is staged. That single line ended a five-round debugging cycle by
showing two things at once that no outcome log could: the live fingerprint never
changed (`0/600/40` throughout — all the test careers were fresh), and every
staged sidecar read `-1`, because the three `fprintf` calls that write the
fingerprint had never been added.

It is cheap and it stays. The next question about this code should be answered
by reading it.
