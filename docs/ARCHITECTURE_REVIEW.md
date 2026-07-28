# Architecture Review — 2026-07-25

Audit taken at the end of milestone 0, before building the JSON event engine.
The goal was to find the failure modes that would become expensive to debug once
there is a lot of content and simulation state on top.

Verdict: **the foundation is sound.** The injection method, the address strategy,
and the presentation path are all the right choices for this target. Four real
hazards were found; three are fixed, one is a design change that should land
before the event engine.

---

## Fixed in this pass

### 1. Off-thread execution — HIGH severity

`Pirates!.exe` imports both `CreateThread` and `_beginthreadex`, so the game
spawns worker threads. Any of them calling `timeGetTime` reached our hook, where
we read game state and could present a modal dialog. Game state is not
thread-safe; doing that off the main thread is a crash, and an intermittent one
that would appear unrelated to its cause.

**Fixed:** the first thread to reach the hook is latched as the game thread;
every other thread returns immediately. Logged once if it happens.

> Mss32 (Miles) and binkw32 also call `timeGetTime`, but through **their own**
> import tables. Our hook is on the exe's IAT only, so they never reach us.

### 2. Vararg slot exhaustion — HIGH severity (latent)

`AddText` shims passed exactly four argument slots. Every `@`-token consumes one.
Hardcoded events were fine, but once events are authored in JSON the token count
is **content-controlled** — a five-token body would read stack garbage, printing
nonsense or faulting. Exactly the kind of bug that surfaces as "some events crash
sometimes."

**Fixed:** widened to eight slots (`kMaxTextArgs`). Surplus slots are harmless —
the callee reads only what its tokens require and cdecl means we clean up. The
JSON loader should still validate token count against supplied arguments.

### 3. Continuing after a fault — MEDIUM severity

Every engine call is wrapped in SEH, which was right for exploration. But we
logged the exception and carried on. An access violation *inside* engine code
means game state may be half-modified; continuing to fire events into it risks a
crash minutes later with no visible connection to the original fault.

**Fixed:** `NoteFault()` latches on any fault in an engine call and disables all
further events for the session, with a loud log line.

### 4. Input polling cost — LOW severity

`GetAsyncKeyState` ran on every one of the ~84 `timeGetTime` call sites, many
times per frame. Throttled to 50 ms, still far finer than a human keypress.
Also added: IAT restored on explicit `FreeLibrary` (not on process teardown,
where touching locks risks a hang).

---

## Fixed in the second pass (2026-07-25)

### 5. Modal dialogs fired from an arbitrary point in the frame — FIXED

**Safe point identified:** the main loop is `FUN_0042E1D0` (PeekMessage /
TranslateMessage / DispatchMessage, then per-frame work). Its `PeekMessageA`
call is at `0x0042E206`, so the **return address `0x0042E20C` uniquely
identifies the top of the frame** — message queue drained, no rendering in
progress, no game locks held.

`PeekMessageA` is IAT-imported, so this needed no inline hooking.

**New rule: triggers never present anything.** They `events::Post()` to a queue;
the queue is drained only from the safe point, one event per frame. The nested
message pump a modal dialog runs uses a *different* call site, so it can never be
mistaken for the safe point.

If the expected return address is never observed, the mod logs a loud warning
after 2000 calls rather than silently doing nothing.

### 6. Save / load invalidation — FIXED

**Sidecar per save file**, detected by IAT-hooking `CreateFileA` and watching for
the `.pirates_savegame` extension. No game RE required.

```
<name>.pirates_savegame     the game's save
<name>.pemf                ours, written alongside
```

- opened for **write** → the game is saving; we write our sidecar
- opened for **read** → the game is loading; we **discard all state first**, then
  read the sidecar (absent or version-mismatched → clean state)
- entering a career with no preceding load → **new career**, state reset

Keying to the save *file* rather than a career fingerprint is deliberate: our
state then travels with the save, so multiple slots and save-scumming behave
exactly as the player expects.

### 7. Validated state access — FIXED

`state.h` is now the only sanctioned way to mutate game state. Every write is

- **refused** unless a career is active (`crewCount > 0`)
- **clamped** to a guard-rail range (corruption prevention, not game balance)
- **logged**, so an event's full footprint is traceable after the fact

`state::Capture()` / `LogDelta()` record an event's net effect in one line.

---

## Historical: the original finding

### Modal dialogs fire from an arbitrary point in the frame

*(Resolved above — retained for context.)*

`timeGetTime` is called from ~84 sites scattered through the codebase. We do not
know *where* in the frame we are when our hook runs — it could be mid-render,
inside a resource lock, or deep in an update. From there we call a **modal**
dialog, which runs its own nested message loop until the player answers.

It works today. But it means re-entering the game loop from an unpredictable
stack depth, and the failure mode is exactly the kind described as a nightmare:
rare, state-dependent, and disconnected from its cause.

**Recommended fix — defer, don't fire in place:**

```
hook (any call site)  ->  set "event pending" flag, return immediately
safe point (one known site) -> drain the queue, present the card
```

This needs one identified per-frame call site that is known to be safe — the top
of the main loop, or a specific UI update. Finding it is bounded RE work of the
same kind that found the text API.

Doing this **before** the event engine is strongly preferable: afterwards, every
event becomes a potential repro of a bug that is hard to attribute.

---

## Open: design constraints to bake in now

### Save / load invalidation

The moment we keep our own simulation state (officers, event history, crew
opinions), loading a save must invalidate it. Otherwise officers persist across
careers and events reference people who no longer exist.

Required before officer state exists:
- detect save and load (hook or poll a save-slot identifier)
- key our sidecar file to the save slot
- reset in-memory state on load

Cheap to design in now, painful to retrofit.

### No validation layer on state writes

We currently write game globals directly (`UndividedPlunder() = 0`). That is fine
for two events; it will not stay fine. Recommended: a thin accessor layer that

- clamps to sane ranges
- refuses writes when not in a game (`crewCount == 0`)
- optionally logs every mutation, so an event's effects are traceable

This is also where "diverge from vanilla where necessary" gets enforced safely.

### Two sources of truth for addresses

`re/out/offsets.json` and `src/core/game.h` both hold the address map and can
drift. Pick one as canonical — ideally generate the header from the JSON — or
accept the JSON as documentation only and say so explicitly.

### `VerifyTarget` covers three probes

Fine today. As the address surface grows, extend the probe set, or checksum the
regions we depend on. A silently wrong address that happens to be readable is far
worse than a mismatch that refuses to run.

### Minor

- `AskChoice` truncates at 2048 bytes silently — should log when it does.
- Debug hotkeys are behind `kDebugHotkeys` but still compiled in.

---

## What is right and should not change

- **IAT hooking over inline patching.** No trampoline, no length disassembler,
  trivially reversible, guaranteed correct.
- **Fixed addresses over pattern scanning.** ASLR is off; scanning would be
  slower and add failure modes for no benefit.
- **Reusing the engine's own presenter.** Enormous saving over building UI, and
  the result is visually native.
- **Never modifying the exe on disk.** Install and uninstall are file copies, and
  the game can always be restored.
- **`VerifyTarget` before touching memory.** Already prevented one real incident
  (the Challenge Pack binary).
- **Simulation in our memory, not the exe's structures.** The only approach that
  scales to the intended scope.

---

## Verification — all fixes confirmed in-game 2026-07-25

One session exercised every fix:

```
game thread latched: 15180
NOTE: timeGetTime also called from thread 8368 -- ignored
safe point reached (main loop PeekMessageA) -- deferred dispatch live
session: LOAD detected <- slot1.pirates_savegame
session: state invalidated (load in progress)
session: no sidecar for this save -- starting clean state
session: career resumed from save
events: firing 'bosun' (0 still queued)
  state: plunder 600 -> 400 [bosun: rum]
  event effect: crew 40->40  plunder 600->400  morale 4->3
session: SAVE detected -> quit.pirates_savegame
session: wrote sidecar (eventsFired=3)
```

**The off-thread hazard was real, not theoretical.** Thread 8368 genuinely calls
`timeGetTime`. Before the fix we were reading game state — and could have
presented a modal dialog — from it. That is precisely the rare, unattributable
crash class this pass existed to eliminate.

Also confirmed: the "send him back" branch logs no `state:` line at all, because
it makes no writes. Effects appear only when something actually changed.

---

## Third pass (2026-07-25) — hazards found and fixed

With the architecture in place, a further review looked for what would break as
content and simulation state grow.

### 8. Queued events survived a load — FIXED

`events::Post()` could queue an event that had not fired before the player
loaded a save. `session::Invalidate()` cleared our state but **not the queue**,
so a stale event could fire into a freshly loaded career and reference a world
that no longer existed. Exactly the failure this pass was meant to prevent.

The queue is now cleared on load and on leaving a career.

### 9. Only `CreateFileA` was hooked — FIXED

The exe imports **both** `CreateFileA` and `CreateFileW`. Saves happen to go
through the ANSI path today, but a single wide-char call would have bypassed
save/load detection entirely and silently desynchronised our state from the
player's saves. Both are now hooked.

### 10. Unsynchronised session state — FIXED

`session::g_state` is written from the safe point (game thread) and read by the
file hooks, which can run on any thread. With three `int` fields the race was
benign on x86, but the moment officers add strings and collections it becomes a
genuine crash. A critical section now guards session state and sidecar I/O —
cheap insurance taken *before* it can hurt.

### 11. `VerifyTarget` did not cover the safe point — FIXED

The safe-point return address `0x0042E20C` is as load-bearing as any function
address, and nothing validated it. A wrong value means deferred dispatch never
runs. The probe set now covers the dialog renderer, `WrapText`, and the
main-loop `PeekMessageA` call site, so a mismatched binary is refused rather
than silently half-working.

### 12. Silent truncation in `AskChoice` — FIXED

Prompts over 2048 bytes were truncated with no indication, which for
JSON-authored content would look like "some events are missing their options."
It now logs.

---

### Verification status of the third and fourth passes

Stated precisely, because these came after the last full play session:

| Fix | Status |
|---|---|
| Expanded `VerifyTarget` probes (incl. the safe-point call site) | **Verified** — `target verified` still logs on the stock binary |
| `CreateFileW` hook installs | **Verified** — logs `hooked KERNEL32.dll!CreateFileW` |
| A save actually arriving via `CreateFileW` | **Never observed.** Saves use the ANSI path today; the hook is insurance. |
| Event queue cleared on load | **Built, not observed.** Requires loading a save with an event still queued. |
| Session lock | **Built, not directly observable.** Correctness by construction. |
| `AskChoice` truncation warning | **Built, not triggered** — needs a >2048-byte prompt. |
| JSON loading and validation | **Verified** — 7 deliberately broken events rejected individually, 2 valid ones loaded |
| Firing a JSON event end to end | **Built, not re-run.** The render path was refactored into `AskChoiceN`. |

---

---

## Fifth pass (2026-07-25) — found by playtesting, not by audit

### 13. Presenting from the wrong point in the frame — ATTEMPTED, REVERTED

Reported as "the scene under the modal looks broken", with screenshots showing a
half-drawn world behind an event card.

The deferred-dispatch design (finding 5) was right that presenting from an
arbitrary point was wrong — but the replacement, the top of the main loop, is
*also* wrong for presenting. It is **before anything is drawn**:

- the dialog renderer composites over the back buffer, so it lands on a stale,
  half-finished frame
- HUD text drawn there is painted over by the world a moment later

The distinction the design was missing: **deciding and showing are different
points in the frame.**

| | Where | What runs |
|---|---|---|
| Decide | main loop `PeekMessageA`, `0x0042E20C` | triggers, session lifecycle |
| Show | after `FUN_004612B0` (sailing render) | notices, `events::Pump()` |

**Implementation — call-site redirection, not a prologue detour.**
`FUN_004612B0` has exactly one caller, so PEMF rewrites that `call rel32` to its
own stub. One 4-byte write, no trampoline, nothing relocated, reversible, and
`Install()` reverts if the previous target was not what it expected. This is the
first time PEMF has written to the game's code, and it is deliberately the
smallest possible form of it.

An intermediate fix — presenting the outcome one frame later via
`events::PostFollowUp()` — is kept, since two dialogs in a single frame have the
same stale-buffer problem regardless of where in the frame they run.

> **This did not work and is disabled.** Enabling the hook produced a black
> screen on entering the game world, then a crash. The log showed the hook
> installing but `AfterSailingRender()` never reaching its first line, so the
> fault is in the stub or in the premise — not in the callback.
>
> **Ruled out:** the calling convention. `FUN_004612B0` spans
> `0x004612B0`–`0x00463CB2` and ends in a plain `ret`, so it is ordinary cdecl
> with register arguments, and the stub's `ret` was right.
>
> **Still open:**
> - the stub's inline asm resolves `g_origTarget` and `AfterSailingRender` by
>   namespace-scope name, and MSVC inline asm has already caused one problem in
>   this project over namespaced symbols. Next attempt: file-scope `extern "C"`
>   pointers.
> - whether `0x004726CA` is a sound place to take control at all. The engine
>   raises its own dialogs from **game logic**, never from inside rendering, so
>   presenting from there is not something it ever does to itself. The premise
>   deserves testing before the mechanism is debugged further.
>
> **Lesson:** this was the first change to write to the game's code, and it went
> in alongside a schema change and new content in a single build. Isolating it
> would have identified the culprit in one run instead of by elimination.

### 14. A second event kind, not a styling flag

Notices (a lookout's call, a passing observation) were about to be implemented as
"a choice event that happens to have one option". That would have been wrong:
they never interrupt, never ask anything, and are drawn by a completely different
engine path.

They are a distinct `kind` in the schema, and putting `options` on one is a
**load error** rather than a silently ignored field.

The general principle, stated by the user and worth keeping: *engine internals
should be exposed properly and generically, for what they are actually for, so
modders can build on them* — not wired up as one-off special cases. The HUD text
wrapper (`game::ShowNotice`) and the render phase are general-purpose; a future
officer roster overlay uses the same anchor.

---

## Sixth pass (2026-07-28) — found by playtesting, not by audit

### 15. A heuristic standing in for a fact — FIXED

Drawing was gated on `triggers::WorldOnScreen()`, which asked whether the ship
had moved in the last 350 ms. **That is not a screen test.** Menus freeze the
ship, so a notice went on painting over one until the window lapsed — a player
sent a screenshot of a lookout's call printed across the Load/Save screen.

The window was a compromise the code was honest about in a comment, and it could
not be tuned out: a becalmed ship at sea is indistinguishable from a menu under
any motion test, so a shorter window only trades a menu leak for notices
vanishing at sea.

Two things made this worth recording as a review finding rather than a bug fix.

**The blocker was a dead-end note, not the engine.** The screen-state globals had
been investigated and written off with *do not re-investigate*. What that work
actually proved was that the values are **not an enum** — true, they read as a
bitfield. But "not an enum" is a far weaker claim than "cannot tell screens
apart", and the stronger reading is what kept this unsolved for a session or
two. Taken as a pair, id and depth separate every screen cleanly. **Re-read
dead-end notes for what they proved, not for their headline.**

**The fix uses the heuristic where it is trustworthy.** Ship movement is
unreliable as *"the overworld is on screen"* but completely reliable as *"the
overworld is on screen right now"*. So the gate learns the overworld's signature
on ticks where the position demonstrably changed, and afterwards matches the
live signature against what it learned. Motion learns the answer and is never
the answer — which turns a permanent approximation into a one-time calibration,
with no constants to maintain.

It fails closed: an unrecognised screen draws nothing until the ship moves and
teaches us its signature, and more than four overworld signatures logs a warning
rather than going quiet.

Full method, the two candidate signals that failed, and the measured table are in
[`re/experiments/screen_state/`](../re/experiments/screen_state/README.md).

### 16. Notices expired behind menus — FIXED

A notice's life was wall-clock, so it kept counting down while nobody could see
it: open the map with one up and it had gone by the time you came back. Its
clock is now held whenever the overworld is off screen — both timestamps shift
by exactly the elapsed time, leaving the remaining time untouched — so a notice
resumes with what it had left. `seconds` now means seconds *seen*, which is what
an author writing it meant.

---

## Seventh pass (2026-07-28) — career and save/load state

### 17. Browsing the load screen replaced your live state — FIXED

`OnSaveFileOpened` applied a sidecar on every read of a save file. The Load/Save
screen opens **every** save it can see in order to list them — measured at seven
files inside 13 ms — so merely opening that screen overwrote the running
career's state with whichever save was listed last. Event counts and trigger
progress were affected exactly as much as flags; flags were only the first thing
visible enough to notice.

### 18. A save loaded from inside a career was never applied — FIXED

The commit was tied to an out-of-game → in-game transition, and loading while
already at sea does not produce one. The player kept the previous career's
history on a freshly loaded save, and the uncommitted state then sat staged
until the next career entry — usually a **new** career, which swallowed it.

### 19. `crew > 0` is not "in a career" — FIXED

`state::InGame()` had been load-bearing since the first milestone. The crew
count does **not** return to zero at the main menu, so after the first career of
a session it reads true forever: no transitions, no `BeginNewCareer`, and every
later career inheriting the last one's state. Career presence now comes from the
screen state.

### 20. The fix that was never wired up — FIXED

The career fingerprint had its struct field, its parser and its comparison, and
nothing that **wrote** it. Every sidecar read as "unknown", every load failed
verification, and no load could ever commit. A grep for the field name returned
three hits — all real, none of them the missing one.

**The process failure is the finding.** Five rounds of fixes were reasoned from
logs that showed only what had been *decided*. Adding a trace of what the
decision was *made from* answered it in one read, and showed two independent
faults at once. The trace is now permanent; see
[`re/experiments/career_state/`](../re/experiments/career_state/README.md).

---

## Eighth pass (2026-07-28) — a stale flag and an assumed edge

### 21. A per-frame gate published once per main loop — FIXED

Entering a town with a notice on screen left the town rendered **with no UI text
at all**.

`content::g_worldLive` is decided at the safe point, once per main-loop
iteration. The render hook runs far faster: the heartbeat measured **57,221
`EndScene` calls in 15 seconds** in town, against ~3,000 while sailing. A town
entry happens *between* two safe points, so for that window the flag still said
"the overworld is on screen" — `DrawNotices()` kept running against the town and
its shared-buffer clear wiped the text the town had just composed.

The symptom only appeared with a notice live, because an empty list
short-circuits before the clear. That is what disguised a staleness bug as a
notice bug, and it cost one wrong hypothesis before the code was read.

Two fixes, deliberately at different levels:

* the published flag now carries **the screen signature it was decided from**,
  and the render phase re-checks the screen is still that one. Two int reads per
  frame, and it closes the class rather than the instance.
* the shared buffer is cleared **only when we actually drew into it**.

### 22. Triggers assumed "armed" rather than observing it — FIXED

Four identical anchored notices drew at the same world point and came out bold —
which reads as a font fault, not a duplicate.

```
12:24:05.708  'landfall_sighted' near port (city 1 at dist 2896)
12:24:07.447  'landfall_sighted' near port (city 1 at dist 2896)
12:24:08.446  'landfall_sighted' near port (city 1 at dist 2896)
12:24:10.569  'landfall_sighted' near port (city 1 at dist 2896)
```

An identical distance four times: the ship never moved. A fresh `Runtime` starts
`armed = true` because nothing has been observed yet — correct at sea, wrong in
a harbour. Every career change reset the triggers while the ship sat inside the
radius, arming an edge that had been crossed long before we were watching.

The first evaluation after a reset now **observes rather than fires**, setting
`armed` from the world as it is. Applied to `stateCrosses` too, which had the
identical fault. Supporting fixes: notices are cleared on a career change, and
identical resolved text can no longer occupy two slots.

Verified: twelve `starts disarmed` lines across a run of career switches, zero
fires.

---

## Remaining, in priority order

1. **Address single source of truth.** `re/out/offsets.json` and `src/core/game.h`
   can drift. Generate the header from the JSON, or declare the JSON
   documentation-only and say so.
2. **Expand `VerifyTarget`.** Three byte probes was right for three addresses; the
   surface is bigger now. A silently wrong address that happens to be readable is
   far worse than a mismatch that refuses to run.
3. `AskChoice` truncates at 2048 bytes silently — should log.
4. Debug hotkeys are behind `kDebugHotkeys` but still compiled in.

---

## Invariants — do not break these

These are the rules the fixes above establish. Violating one reintroduces a class
of bug that is expensive to diagnose.

| Invariant | Why |
|---|---|
| **Only the game thread touches game state.** | Worker threads reach our hooks; game state is not thread-safe. |
| **Triggers `Post()`; only the safe point presents.** | Modal dialogs from an arbitrary frame position re-enter the game at unpredictable stack depth. |
| **One event per frame.** | Presenting a card while another unwinds is how the 610-deep recursion happened. |
| **All mutation goes through `state.h`.** | Clamping, career gating, and traceability. |
| **A fault latches and disables events.** | Never keep firing into state that may already be corrupt. |
| **Loading a save discards all prior state first.** | Otherwise events reference people and history that do not exist. |
| **Loading also clears the event queue.** | A queued event belongs to the career it was posted in. |
| **Hook both `CreateFileA` and `CreateFileW`.** | The exe imports both; one missed path silently desyncs our state from saves. |
| **Session state is lock-guarded.** | Written on the game thread, read by file hooks on any thread. |
| **Decide at the top of the frame; show after the render.** | Presenting before the world is drawn gives a stale, half-drawn background. |
| **Never present two dialogs in one frame.** | Same stale-buffer problem — use `events::PostFollowUp()`. |
| **HUD text must be re-drawn every frame.** | The engine has no timed-message call; expiry is ours to track. |
| **Drawing is gated on a learned screen signature, never on ship movement.** | Menus freeze the ship, so motion cannot separate a menu from a becalmed ship at sea. Motion learns the signature; it is never the test. |
| **Measured screen signatures are evidence, never constants.** | Comparing against them means one unvisited HUD state stops all drawing, silently. |
| **A notice's clock stops while it is off screen.** | Otherwise it expires unseen behind a menu and the player loses it for having glanced at the map. |
| **A retained pointer to an engine object must be referenced.** | Gamebryo refcounts at `+4` and destroys at zero. Capturing flag textures without a reference crashed the game on first use. |
| **Confirm a limit exists before building around it.** | "Flags must be replaced" was accepted for years; the game had enumerated them since 2004. One call site settled it. |
| **A value published at the safe point is stale by the time the frame draws.** | The render hook runs an order of magnitude more often. Publish the evidence alongside the conclusion and re-check it where it is used. |
| **Edge-triggered state must be observed before it is trusted.** | A fresh runtime knows nothing; assuming "armed" fires on edges crossed before we were watching. The first look sets state, it does not act. |
| **Anything the player can see belongs to a career and dies with it.** | Notices, trigger progress, disguises. A line from the last captain's voyage over this one's ship is a bug. |
| **Never write to the engine's shared buffers unless we wrote to them first.** | `0x00869B48` is the game's scratch. Clearing it when we put nothing in it throws away text nobody will re-compose. |
| **When the engine already computes an answer, read the answer.** | The player's nation is derived and cached by the game. An independent derivation can only agree or be wrong. |
| **Derive a struct offset by subtracting, then check it against a field whose value you already know.** | Three ship-record offsets were subtracted wrong and a live test reported every ship as role 0. The player record was validated this way (crew 40, gold 600) and the ship record was not. |
| **When one reading is wrong and another is right, the right one is the clue.** | Flags came out correctly distinct per builder while role read 0 for all of them. That contrast said the calls were fine and the readout was not, and it went unread for a round. |
| **A callable engine function does not validate its inputs.** | `FUN_004154F0` reads its home port from a global that is `-1` in a fresh career and builds ships at a null settlement. Check what a function reads, not just what it takes. |
| **Calling engine functions is opt-in behind its own marker.** | Creating game state cannot be undone by pressing the key again, unlike every other probe. A different risk deserves a separate opt-in. |
| **An unexplained reading is data, not an absence.** | "No hover text on the spawned ship" was reported and passed over. It was a direct measurement of the classification field, and chasing it immediately would have found `+0x02` three rounds earlier. |
| **Change one thing.** | The role test stamped a role AND wrote a destination, so it could not answer the question it existed for. Its conclusion — "role does not gate movement" — was unsupported by its own design. |
| **What the game shows the player is a map of what it stores.** | The four hover labels (`pirate-hunter`, `privateer`, `raider`, `smuggler`) are a jump table over one field. UI strings are the cheapest route into an unknown enum. |
| **Career presence comes from the screen state, never from `crew > 0`.** | The crew count does not reset at the main menu, so that test is true forever after the first career and stops seeing transitions entirely. |
| **A save file being read is not a load.** | Starting a new career reads one too, and the load screen reads every save to list them. Only a fingerprint match against the live career proves it. |
| **Verify a write landed, not that a symbol exists.** | The fingerprint had its field, parser and comparison, and no `fprintf`. A grep matched three real sites and the feature still did nothing. |
| **Trace the inputs, not the outcomes.** | Five fixes were reasoned from logs showing only decisions. One log of what the decision was made *from* ended it immediately. |
| **Redirect a call site rather than detour a prologue, where possible.** | No trampoline, nothing relocated, one reversible write. |
