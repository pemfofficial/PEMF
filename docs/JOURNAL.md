# PEMF Journal

A dated record of every working session: what was attempted, what was found,
what changed, and what is still open. It is the place to look first when picking
the work back up.

## How this journal works

- **One entry per session**, newest at the bottom, headed `## YYYY-MM-DD — topic`.
- Each entry has the same five parts, in this order:
  - **Goal** — what the session set out to do.
  - **Done** — what was actually carried out, including tooling and setup.
  - **Found** — results, each marked by how it is known:
    *observed* (seen in game or in a log), *decompiled* (read from analysis of the
    binary), *reference* (taken from another project's notes or source, not yet
    checked here), or *inferred* (reasoned, not yet checked). Addresses are
    absolute VAs in the GOG build; the Steam build is the same code once unpacked.
  - **Changed** — files edited, with a line on why.
  - **Open** — questions left, and the next concrete step.
- Findings that hold up graduate out of the journal into the reference docs:
  addresses into [`GAME_API.md`](GAME_API.md) and
  [`re/out/offsets.json`](../re/out/offsets.json), status into
  [`ROADMAP.md`](../ROADMAP.md), subsystem write-ups such as
  [`WIDESCREEN.md`](WIDESCREEN.md), and the full method and numbers of an
  investigation into [`re/experiments/`](../re/experiments/). The journal entry
  links to them rather than repeating them.
- Dead ends are recorded as carefully as successes, with the reason, so they are
  not tried again blind.

---

## 2026-10-16 — REA tooling; widescreen and resolution, second attempt (start)

**Goal.** Faster static analysis, then widescreen and higher resolutions done
properly: more resolution options, a centred UI with a working mouse, 3D that
does not split, and sharper output at modern sizes. Storms follow.

**Done.**
- Installed **REA** 6.5.0, a reverse-engineering toolkit that
  drives Ghidra headless and answers queries on demand: decompiled functions,
  callers and callees, strings, cross-references, byte reads.
- Wired it to the existing Ghidra install (`C:\tools\ghidra_12.1.2_PUBLIC`,
  JDK 21). Full auto-analysis of the GOG `Pirates!.exe` takes **522 s** on this
  machine, past REA's default 330 s limit, so `REA_GHIDRA_STARTUP_TIMEOUT_MS` is
  raised. Analysis is paid once per session; queries after that are fast. Work
  files live in `D:\pemf-re\`, outside the repo — analysis databases are
  deliberately not shipped.
- Read the July record of the first widescreen attempt and
  [`WIDESCREEN.md`](WIDESCREEN.md), and the source of an independent project that
  has shipped widescreen for the same build (below).

**Found.**
- *Observed* (`GAME_API.md`, restated): once unpacked, the Steam executable is the
  GOG build. Static analysis runs on the GOG file because the Steam file is
  encrypted on disk; every address applies to both.
- *Observed* (`Config.ini`): current setting `WindowWidth = 1440`,
  `WindowHeight = 1080` — the 4:3 fallback chosen after the July revert.
- *Reference* — **Pirates! Unbound**
  ([github.com/twitchplayskh/Pirates-Unbound](https://github.com/twitchplayskh/Pirates-Unbound),
  MIT, beta.2 of 2026-10-09) ships widescreen up to 3840×2160 for the Steam 1.0.2.0
  build (`SHA-256 5342209C…`, matches the local Steam copy). It is a separate
  injected DLL using inline hooks; it is used **only as a reference**, nothing of
  it goes into PEMF, and each address is to be confirmed by our own decompilation.
  Its method, per its source:
  - *The camera choke point.* It hooks inside `FUN_004CCE40` at `0x004CCE74`
    (`mov eax,[edx]; mov [ecx+0x128],eax`), where an `NiFrustum` is copied into a
    camera. This is the function the July notes identified last — "the camera
    choke point, 24 call sites, all cameras; the renderer derives projections from
    it for both pipelines" — built then but never tested. A working release
    using it is strong evidence it is the right layer, and that it also covers the
    vertex-shader path that split ships in July.
  - *World (Hor+).* A world frustum (4:3, `far/near ≠ 5/3`, not orthographic)
    gets `left` and `right` scaled by `3W/4H`: vertical field of view kept, more
    world at the sides. A matching `_11 *= 4H/3W` is applied to 4:3 world
    projections in `SetTransform`.
  - *UI.* The UI frustum (`far/near = 5/3`, `left/right = ∓0.5`,
    `|top| ≤ 0.375`) gets `left/right = ∓3W/8H` and `top/bottom = ±0.375` —
    the height-fit correction `WIDESCREEN.md` derives, applied at the choke point
    rather than at `FUN_00503CA0`, which runs once and caches. In `SetTransform`
    the UI camera (recognised by its view matrix and `_11 = 2`, `_33 = 2.5`) gets
    `_11 *= 4H/3W`, `_22 = 8/3`, and a scissor over the centred 4:3 area.
    Full-screen 640×480 background quads and the 642×242 dialogue fade are drawn
    with the wide projection so they fill the screen.
  - *Already-built camera.* Rewrites the live camera's frustum directly: camera
    pointer `0x008E9FD8`, frustum at `+0x128`, then `0x00536F80` (`thiscall`,
    camera update). No hook needed for this one.
  - *Mouse.* One hook at `0x0042C170`, upstream of the hit-tests, remaps the
    cursor X from the centred 4:3 area to the stock range. World picking is not
    touched. `WIDESCREEN.md` instead plans to repoint all eight `1024`/`768`
    operands; a single upstream remap would make that unnecessary — to compare.
  - *Device.* Renderer `*(0x008EA08C)`, D3D9 device at `renderer + 0x60`. PEMF
    uses `[[0x00727C30] + 0x60]`; both to be reconciled.
  - *Extras.* MSAA through its own render target; anisotropic filtering scoped to
    world draws. It does not raise texture resolution.

- *Decompiled* (REA, Ghidra 12.1.2, GOG `SHA-256 6E88B90E…`):
  - `FUN_004CCE40` is NiCamera set-frustum: `ecx` = camera, `edx` =
    `NiFrustum*` `{l, r, t, b, near, far, ortho}`. It clamps
    `near = max(near, far / 10000.0, 0.1)` (constants `0x007177E4` = 10000.0,
    `0x007177E0` = 0.1), stores the seven fields at camera `+0x128…+0x140`, and
    tail-jumps to `FUN_00536F80`. Entry bytes `51 D9 42 10 D9 42 14`.
  - `FUN_00536F80` only marks the camera's dependents dirty (bit 0 of `+0x20` on
    each entry of the array at `+0x15C`, count `+0x164`). It clobbers `EAX` and
    `EDX` and **preserves `ECX`** — so the set-frustum as a whole preserves `ECX`.
  - **24 references to `0x004CCE40`, all `E8 rel32` calls** (list in
    `src/core/widescreen.h`). Most copy the camera's current frustum, change
    near/far (e.g. `1000.0` / `40000.0`), and set it again.
  - `FUN_004D3AB0` (SetResolution) builds the UI frustum as `l/r = ∓0.5`,
    `t/b = ±(H/W)·0.5`, `near/far = 0.9D/1.5D`, on the camera at `*(0x008E9FE0)`.
    At 16:9 that is `t = 0.28`: the cut-off top and bottom, read from the code.
    The startup builder `FUN_00503CA0` sets the same camera, before the first
    safe point.
  - `FUN_004B2DF0` (mode list) keeps a mode only if format is X8R8G8B8, width
    ≥ 640 and `height*100/width == 75`. The `jne` at `0x004B2E8A` is `75 21`.
  - `FUN_0042C170` is the game's input handler, reached only through the relay
    at `0x004D3E00`, which passes the window message's `msg` and `lParam`. Mouse
    messages convert `lParam` to the 640×480 space (`x·640/deviceW`) and, via
    `FUN_005025D0` → `FUN_005045A0` → `FUN_00504310`, hit-test widgets **from
    the same `lParam`**. Device size: `0x008CAC18` / `0x008CAC1C`. Game window:
    `0x00726B8C`.
- *Observed in source*: the "IAT hooks only" rule was outdated. PEMF has patched
  code since v0.2.1 (storms, town menu, render, sail keys); the rule actually in
  force (`core.cpp`, `RunSafePoint`) is that **code is written only at the first
  safe point**, after the Steam wrapper has finished checking its image.

**Changed.**
- `docs/JOURNAL.md` — created.
- `docs/README.md` — journal added to the index.
- `src/core/widescreen.h` — new. Lifts the 4:3 mode filter; redirects all 24
  set-frustum calls through an `ECX`-preserving stub that corrects the frustum
  (world: Hor+, `l/r × 3W/4H`; UI: height-fit, `t/b = ±0.375`,
  `l/r = ∓0.375·W/H`) and then calls the game's own function; corrects the UI
  camera built at startup once in place; subclasses the game window and maps
  mouse X from the centred 4:3 area to the full width. Verify-before-write,
  all-or-nothing, installed from the first safe point, restored on unload.
- `src/core/core.cpp` — include, `LoadSettings`, `Install` at the first safe
  point, `Restore` on unload.
- `content/PEMF/widescreen.ini` — new player settings (`enabled`,
  `unlockModes`, `widenWorld`, `centerUi`); `build.ps1` deploys it without
  overwriting edits.
- Built (0.2.6 tree including the owner's uncommitted work) and deployed to the
  Steam install for testing.

**Open.**
- **In-game test on Steam** at 1920×1080: menus whole and centred; mouse on
  buttons and sliders; sailing wider with ships intact (the July tearing);
  towns, dialogs, battle, map. Read `pemf.log` for the `widescreen:` lines.
- Side margins on interface-only screens are not filled yet: expect black or
  stale image there. Next: draw full-screen 640×480 backgrounds wide (the
  approach in the reference), or clear the strips.
- Mouse picking of 3D objects, if any screen uses it, now gets the remapped X.
  Watch for it.
- Upscaling: MSAA and anisotropic filtering at the device level; texture
  resolution is a separate, asset-side question.

### Test round 1 (Steam, 1920×1080) and the fix that followed

**Found.**
- *Observed* (log): all 24 call sites redirected; UI camera corrected to
  `l/r = ∓0.6667, t/b = ±0.375`; world cameras widened (`l/r = ∓0.4418`,
  `t/b = ±0.2485`, i.e. 4:3 × 1.333). Six resolution changes, one failed reset
  that recovered.
- *Observed* (owner): the game "looks upscaled"; but in scenes with a painted
  backdrop, 3D figures stand in blue empty space at the sides — the July symptom.
  And after changing resolution the Options mouse is "hit or miss".
- *Inferred*: the backdrop is a 640×480 quad drawn by the UI camera. Height-fit
  correctly keeps it 4:3 in the centre; the widened world camera draws figures
  past it; nothing covers the sides, so the clear colour shows.
- *Reference*: Pirates! Unbound fills these by drawing the quad with
  `_11 = 2` — its own notes say this "does not preserve the backdrop's horizontal
  artwork proportions". Owner requirement: **aspect ratio is preserved**. So
  PEMF pillarboxes instead of stretching.
- *Observed* (imports): the game reads no cursor position from Windows directly
  (`ScreenToClient` only, no `GetCursorPos`), so all mouse input arrives through
  window messages, where the remap sits. The "hit or miss" cause is still
  unknown; this build logs every click with window, device and mapped values.

**Changed.**
- `src/core/d3d9hook.h` — hooks `DrawIndexedPrimitive` (vtable 82) in the same
  span, with the same rebuild check and restore as the other slots; calls
  widescreen at each pass's begin and end and on four-vertex draws.
- `src/core/widescreen.h` — backdrop detection (textured `XYZ|DIFFUSE|TEX1`
  quad, x ±320, z ±240, or the map backing z −200..240; untextured fades
  excluded); on a pass that drew one, the side strips of the backbuffer are
  cleared to black at `EndScene` (scissor and viewport opened for the clear and
  restored). Per-resolution logging of camera fixes; click diagnostics.

### Test rounds 2–4: seeing the screen

**Found.**
- *Observed*: the owner cannot capture the game (every capture tool minimises
  it; the Steam overlay key did nothing). PEMF now saves the finished frame
  itself at `Present` (half-size BMP in `PEMF\shots`): Ctrl+Shift+X on demand,
  kept as `key_*.bmp`; `autoShots = N` in `widescreen.ini` adds a 40-file ring.
  This is what made the rest diagnosable.
- *Observed* (machine): 62 32-bit display modes, 1440×1080 twelve times over
  (one per refresh rate). Lifting only the 4:3 test put them all on the Options
  slider, which lurched between duplicates. *Decompiled*: the list is built once
  at startup by `FUN_004B2DF0` from `FUN_004B3590` (`count → 0x00726238`,
  table `0x008CABF0`, entries `{mode, w, h}`, game allocator `0x00533C9D`;
  modes are `D3DDISPLAYMODE*` at renderer `+0x280 → +0x454`, count `+0x45C`).
  PEMF replaces it at its one outside call `0x004B3631` and rebuilds the table
  on install: 20 entries, each size once. The slider is still reported odd;
  not yet seen.
- *Observed* (screenshots): main menu and character creation — painting now
  fills the width (cover) ✔; tavern recruitment cutscene — full width, bars fixed
  ✔; sailing — wide and intact ✔; map — 4:3 with black sides (by design).
- *Observed*: a "holo" ghost in the governor's room and tavern — e.g. the
  dialogue text doubled, larger and lower. *Inferred*: the game lays a copy of
  the frame (render-target texture) back over the screen as a 640×480 quad;
  "cover" enlarged it by 4/3. Frame copies now go back exactly full width.
- *Observed*: the town menu painting stayed a centred 4:3 box. *Decompiled*: the
  town menu is a scene file (`tm_<n>_menu.nif`) with its own `Camera01`
  (`FUN_004108A0`, activation `FUN_004AE5B0`, near/far `FUN_00412840` →
  `FUN_004FF590`, a shared call site). `DAT_008CADEC` holds the scene; ShowMessage
  (`FUN_00410C50`) zeroes it on close (`0x00410CFA`). While it is set, world
  frustums get cover instead of Hor+, so the painting, clouds and flags scale
  together.
- *Observed*: the sailing HUD (Fame dial, key pad) sits at the 4:3 edges, not the
  screen corners. Moving widgets needs their click areas moved with them —
  deferred, to be designed rather than patched.

**Changed.**
- `widescreen.h` — mode-list replacement; draw-level handling of full-screen 2D
  layers (painting → cover, solid strip or frame copy → exact full width, map
  backing → 4:3 with black sides); town/card cover; screenshots.
- `d3d9hook.h` — `Present` calls the screenshot check; the draw hook restores
  the projection after an adjusted draw.
- `re/out/widescreen_decomp.c` (local only, gitignored) — the 13 functions
  decompiled for this work. REA evidence: `D:\pemf-re\evidence\`.

**Open.**
- HUD anchoring to the screen corners, with matching click areas.

### Test round 5 — widescreen verified

**Found.**
- *Observed* (owner: "looks great"; 18 key screenshots): settings dialogs,
  Options, recruitment cutscene (full-width letterbox), governor's room (no
  ghost), shipwright, town menu — all correct at 1920×1080.
- *Observed* (log): the adapter lists 1440×900 *after* 1920×1080, so it sat at
  the top of the slider and read as a broken end stop. The PEMF list is now
  sorted by width, then height.

**Changed.**
- `widescreen.h` — mode list sorted.
- `docs/WIDESCREEN.md` — second attempt documented and marked verified.
- `ROADMAP.md` — display entry.

**Open.**
- HUD anchoring (as above). Upscaling proper — MSAA, anisotropic filtering,
  texture resolution — not started.

---

## 2026-10-16 (cont.) — Weather: what the game itself does, dynamic wind, one storm

**Goal.** Improve the shipped storm system with what the decompiler can now
show; owner asked specifically for a wind that changes, storms from different
directions, and a check of how much of the game depends on the wind.

**Found.**
- *Decompiled* — **the weather curve is the wind.** `FUN_0045FA70(x, y, sound)`
  = `128000 / (oct-distance to nearest weather slot + 4000)` (slots 1–2 count
  2000 further off than the storm slot 0). Its result is stored in
  `DAT_0085A0F8` and is the **"@NUM knots"** readout on the sailing HUD
  (`FUN_004741B0`), which also turns the wind needle by `knots/2 + 10`.
- *Decompiled* — **the game's own storm rules** (`FUN_00467F90`, the per-ship
  sailing update, `0x00468907–0x00468B35`; capstone disassembly):
  - ship speed folds the wind value in, scaled by a per-ship-type factor
    (`0x00723B0C + type*0x44`); floor 200;
  - storm radius `R = DAT_00725678 * 2000 / 3`; player inside R: **speed +400**,
    `DAT_008C8218 = 2`; inside the core `R / (2*DAT_00869B26 + 3)`: state 3 and
    **a random ship of the fleet takes +1 damage** (`0x00814318 + i*0x45C`,
    max 70) unless flags `0x0085A164 & 0x484`; near: state 1. AI ships inside R:
    1-in-3 per update, +1 damage, max 50. The byte `0x00869B26` shrinking the
    core is *inferred* to be a skill (Navigation) — unconfirmed.
  - so the August note "storm gameplay effects NOT located" is resolved: storms
    push you along, damage your fleet in the core, and drive the knots readout.
- *Decompiled* — `FUN_0045FBF0` re-seeds the three weather slots at random
  within ±10 cells of `(DAT_008B96B0, DAT_008B96B4)`; slot 0 offset by +10.
  Called from the sailing update at `0x00466F74`, `0x00471CEC`, `0x00472412`.
- *Decompiled* — **the wind direction** is `DAT_007253BC`, an int32 angle
  (2^32 = 360°, `0x40000000` = 90° = the trade wind). Read in ~20 places:
  ship speed (`0x0046885E`), weather drift, HUD needle, sails/waves
  (`0x004E4FA6`, `0x004E596D`), sea battle. Written in three:
  - `0x00461E51`, every sailing tick in the render shared by overworld and
    battle: `target = 3*(DAT_008CA930*DAT_0085A158 + 0x15555540)`,
    `wind += (target - wind)/32`. The constant is ~90°, so the wind barely moves;
  - `0x0047A0C9` (= 90°) and `0x0047EDE3`, both in `FUN_0047A040`, the sea
    battle instance (matches `re/out/battle_entry.c`).
- *Decompiled* — **weather drifts with the wind** (`0x0046359C–0x004635D2`):
  each slot moves `x -= v*sin(w)`, `y -= v*sin(w+90°)`, `v = DAT_00725684/16`,
  trig via `FUN_004DB210` (`FUN_004953D0`). Hence storms always arrived from
  the same side.
- *Observed in source* — **integration bug.** PEMF draws its own storm system
  (`g_sysX/g_sysY` in `storms.h`) and hides the engine's, but every rule above
  measures from the engine's slot 0 (`0x008B98F0/E4`). The push, fleet damage,
  knots, needle — and PEMF's own cargo loss, which reads the engine value — all
  followed an invisible storm somewhere else.

**Changed.**
- `src/core/wind.h` — new. Steers the wind's *target*, not its store: the 12
  bytes at `0x00461E23` (`mov eax,[0x8CA930]; imul eax,[0x85A158]`) become
  `mov eax,[g_pemfWindTerm]` + 7 NOPs, so the game's own 1/32 easing still turns
  the wind. On the overworld PEMF sets the term to its own model (trade wind
  prevails; shifts within ±`windVariance` every 90–240 s; 20% are fronts up to
  ±150°; turning 4°/s); everywhere else — battles — the term is the game's own
  product, so battles are unchanged. Exact: `3` is invertible mod 2^32
  (`0xAAAAAAAB`). Verify-before-write, first safe point, restored on unload.
- `src/core/storms.h` — systems are born **upwind** (`+(sin w, cos w)`) and
  drift **downwind** along the live wind; `syncEngineStorm` holds the engine's
  slot 0 on PEMF's system each frame and parks it 120000 off while the sky is
  clear, so the game's storm rules follow the storm the player sees.
- `src/core/game.h`, `state.h`, `content.h`, `triggers.h` — `stateCrosses`
  takes `field: "storm"` (0–3, the engine's `DAT_008C8218`) and `"wind"` (knots);
  both read as calm unless under way.
- `content/PEMF/events/storm_events.json` — new: warning, "shorten sail", man
  overboard (a choice), and the storm passing.
- `content/PEMF/storms.ini` — `syncEngineStorm`, `dynamicWind`, `wind*` knobs.
- `re/out/weather_decomp.c` (local only) — the weather functions.

**Open.**
- In-game test: wind turns (needle), storms arrive from new quarters, the push
  and "knots" now coincide with the visible storm, the storm events fire.
- Confirm `0x00869B26` is Navigation; if so, say so to the player when the
  navigator keeps the ship out of the core.
- Battles: the game sets 90° at battle start. Carrying the overworld wind into
  battle is possible later, but deliberately not done yet.

### Test round 6 — weather playtest; fixes; the hunter investigation begins

**Found.**
- *Observed* (log): the storm events fire as designed (warning → shorten sail →
  man overboard → passes). The wind shifted 90 → 72 → 145 → 130 → 92° — all
  inside ~70–150°, all "blowing west", so it read as barely moving. Owner also
  felt speed still favoured heading west.
- *Decompiled* (`0x0046885E`): the point-of-sail speed (`FUN_00477450`,
  `ecx` = wind, `eax` = heading `+0x81430C`, pushed ship type) reads
  `DAT_007253BC` **only for ships 0–7**; every other ship reads
  **`DAT_00725600`**, written once to 90° (`0x0045F99D`) and never again. So the
  AI fleet sailed on the stock trade wind while the player's changed.
- *Observed* (log, "you've drifted too far"): that is the sailing master's
  card, and it fired once, correctly by its rule — the trace shows the ship going
  from 3.1 to 13.8 units off the nearest port in 80 s (a storm helping) and the
  card speaking at 12 (60 leagues). The rule measures distance from a PORT, not
  the map edge, which is what reads as wrong. Policy, not arithmetic.
- *Decompiled*: `FUN_0045B890` (the card) takes no arguments; its quiet path is
  `[0x008B98D4] = tick + 250`; its one caller is `0x0047376D`.
- *Observed* (screenshots): the world behind a dialog card came out enlarged.
  *Inferred*: that frame copy lives in an ordinary texture, not a render target,
  so the "painting → cover" rule caught it.
- *Observed*: the "Map" label on key 3 of the sailing key pad is missing this
  session (present on 19:05). *Decompiled*: labels come from `@NAVKEYHELP`
  through `FUN_00473AA0` and are drawn last-to-first by `FUN_004B06C0`; "Map" is
  the ninth and last. *Inferred, unconfirmed*: a shared engine budget running
  out — PEMF's cloud cluster shares the scene-node budget (see August notes).
- *Decompiled* — **AI navigation is a waypoint graph, not a straight line.**
  Ships hold a destination (`+0x3E`) and a next waypoint city (`+0x40`); the next
  hop comes from a per-city table of `0x94`-byte rows (`0x004696EA`), limited by
  city-to-city distance (`FUN_00405D50`, `0x9A`) and city flags (`0x00860B70`
  bits `2`, `0xC0000000`). This is why the August hunters could only be aimed
  past the player, never made to chase.

**Changed.**
- `wind.h` — defaults widened (variance 100°, fronts 30% up to 180°, so the
  wind can come from any quarter); `DAT_00725600` held equal to the live wind on
  the overworld and restored to 90° everywhere else.
- `widescreen.h` — only DXT-compressed full-screen art is a painting (cover);
  every other textured full-screen quad is a frame copy (exact). The map's side
  strips are generated parchment (managed texture, D3D state captured and
  restored) instead of black; the state block is released before every device
  Reset, which D3D9 requires.
- `d3d9hook.h` — calls the release before Reset.
- `sailmaster.h` — a policy: the card speaks only beyond `farOutLeagues`
  (default 150; stock 60) or never (`farOutCard = 0`). Replaces the call at
  `0x0047376D`; when it holds the card back it re-arms the timer exactly as the
  game's own quiet path does.
- `storms.ini` — `farOutCard`, `farOutLeagues`; wind defaults updated.

**Open.**
- Playtest: wind from new quarters, AI ships following it; dialog backgrounds;
  map parchment; the card.
- "Map" label: bisect with `clusterCount = 0` in `storms.ini`.
- **Hunters** (owner's next big feature): read the waypoint routing (`+0x40`,
  the `0x94` table) to see whether a hunter can be routed directly — the key to
  a hunter that truly chases.

### Test round 7 — wind too timid; a clipped dialog frame

**Found.**
- *Observed* (log): a 4-minute session produced one wind shift, of 3° (90 → 93),
  2.5 minutes in. The far-out card was held correctly ("130 leagues out, setting
  is 150"). The resolution list and paintings ("cover", DXT formats
  `0x31545844` DXT1 and `0x33545844` DXT3) behaved.
- *Observed* (screenshots): the "Which ship shall we attack" box loses the top
  corners of its frame; the centred cards and the battle HUD are intact.
  *Decompiled*: that box is placed at `y = ScreenH/16 + 1` (`0x0047336D`), i.e.
  31 of 480 — near the top by design, and our vertical mapping matches stock
  4:3, so the box itself is where the game puts it. Cause of the missing corners
  not yet known: one of our full-width rules, the game's own scissor computed for
  the old screen shape, or engine culling.

**Changed.**
- `wind.h` — first shift after 45–90 s; every shift turns ≥ 45°, a front ≥ 120°;
  Ctrl+Shift+W forces a front; the game's actual wind (player and AI) is logged
  every 30 s.
- `widescreen.h`, `d3d9hook.h` — **draw trace**: Ctrl+Shift+D logs every
  indexed draw of the next frame (geometry bounds, texture, projection, view,
  world, scissor, viewport), as submitted by the game, before any PEMF change.

**Open.**
- Trace the clipped dialog (Ctrl+Shift+D with it open) and fix from the data.

### Test round 8 — wind confirmed; a realistic model; better instruments

**Found.**
- *Observed* (log): a forced front took the game's wind 90 → 10 → 326° and the
  AI ships' wind followed exactly (`DAT_00725600` = `DAT_007253BC`). Owner: the
  wind "seems ok"; asked for a realistic, random rate of change.
- *Observed* (log): the first draw trace filled its 400-line cap with world
  draws (sea, ships, sails) before any interface draw — no data on the clipped
  dialog frame.
- *Decompiled*: `FUN_00501040` (a UI camera "resize", one of the 24 frustum call
  sites) passes a frustum built by its caller to the setter, so its values exist
  only at runtime.

**Changed.**
- `wind.h` — the model: the trades MEANDER (within ±45°, each shift ≥ 15°,
  every 60–180 s, turning 2°/s) and every 6–12 minutes a FRONT comes through
  (≥ 90°, any quarter, turning 6°/s, holding 3–6 minutes) and then the wind
  backs to the trades. Ctrl+Shift+W forces a front. `storms.ini` documents it;
  a duplicate block of older wind keys there was removed (a later duplicate
  wins, and would have overridden the new minimum turn).
- `widescreen.h` — every camera call site is logged once (its frustum, and
  whether PEMF changed it); the frustum stub passes the game's return address
  as a third `__fastcall` argument. The trace logs only interface and
  screen-space draws, counts world draws, and covers all four draw calls.
- `d3d9hook.h` — `DrawPrimitive`, `DrawPrimitiveUP`, `DrawIndexedPrimitiveUP`
  hooked for the trace only (a flag test, then the original).

### Test round 9 — the clipped dialog, measured

**Found.**
- *Observed* (camera-site log): every frustum call site reported once. Two arrive
  already 16:9 — `0x0046F488` (`l/t` = 0.5523/0.3107) and `0x004D975D`
  (0.4974/0.2798): the game sizes those from the screen, so "unchanged" is right.
  No interface camera slips past the classifier. The suspected cause is ruled out.
- *Observed* (draw trace, the "Which ship shall we attack" box): the box is a
  nine-piece frame (texture 64×64 DXT5) drawn by the UI camera; the trace lists
  the top edge (x −214…138, z 209…225), the left edge, centre and right edge
  (z 65…209) and the bottom row including both bottom corners (z 34…65) —
  **seven pieces. The two top corners are never submitted to Direct3D.** The
  trace is taken before any PEMF change and every draw is passed through.
- *Decompiled*: `FUN_004881B0` builds the frame and creates **all nine** pieces
  unconditionally through `FUN_004AF080` (atlas `DAT_008E66F0`; top-left region
  (0,0) 16×16 and top-right (0x20,0) 31×16, on the top edge's row). So the corners
  exist and are dropped later. Their position cannot explain it — same row as the
  drawn top edge, same column as the drawn side pieces.
- *Inferred*: PEMF's change to the interface camera can only cull LESS (it widens
  left/right; top/bottom equal stock 4:3), so the stock game probably drops them
  too. To be settled by comparison at a 4:3 resolution, where PEMF's widescreen
  code changes nothing.

**Changed.**
- `wind.h` — Ctrl+Shift+W exists only in developer builds
  (`-Defines "PEMF_DEV_TOOLS=1"`); the owner does not want players able to
  summon weather. `storms.ini` no longer mentions it.

**Release checklist (raised this round).**
- `core.cpp`: `kDebugHotkeys = true` ships the developer hotkeys (Ctrl+Shift+1…0,
  N, H, J, K, L, O, U, P, Y, M, B, V) to players. Decide before release.
- Ctrl+Shift+D (draw trace) is diagnostic only — gate it with `PEMF_DEV_TOOLS`
  before release; Ctrl+Shift+X screenshots are worth keeping for bug reports.

### Test round 10 — the clipped dialog: stale culling planes

**Found.**
- *Observed* (draw trace at 1440×1080, the attack prompt open): at 4:3, where
  PEMF's widescreen code changes nothing, **all nine pieces** of the prompt's
  frame are submitted, both top corners included (x −230…−214 and 154…185,
  z 209…225). So the stock game draws them, and the 16:9 loss is PEMF's.
- *Decompiled* — **where culling planes come from.** `FUN_005373F0` builds the
  camera's six culling planes from its frustum at `+0x128` (near/far, then
  left/right/top/bottom, separate ortho and perspective builders
  `0x00535E20…0x00536500`). Its callers are the camera's world-data updates
  (`0x005374EE`, `0x005668A3`), each followed by the world-to-camera matrix
  (`FUN_005366D0`). They run from `FUN_00533D10` = NiAVObject::Update(time,
  updateControllers), thiscall, `ret 8`. The set-frustum `FUN_004CCE40` itself
  only marks dependents dirty — it does **not** rebuild the planes.
- *Decompiled*: the game always updates after setting the interface frustum —
  SetResolution `0x004D3BBB` (`push 1; push 0; ecx = [0x008E9FE0];
  call 0x00533D10`) and the startup builder `0x00503DC2` (then `0x004FF6B0`).
- *Inferred, consistent with every observation*: PEMF's one-time correction of
  the startup interface camera set the frustum but never updated the camera, so
  culling kept the stock 16:9 top/bottom (±0.28, i.e. z ±180 of the 480-tall
  interface). The 16×16 and 31×16 top corners lie wholly beyond that plane and
  were culled; the 368-wide top edge's bounds straddle it and drew. A resolution
  change in game (which runs SetResolution, through PEMF's stub, then the
  update) would have hidden the bug — which may also explain the key pad "Map"
  label coming and going between sessions. Unconfirmed.

**Changed.**
- `widescreen.h` — `CorrectExistingUiCamera` now calls `FUN_00533D10(cam, 0, 1)`
  after its set-frustum, as SetResolution does; entry bytes
  `8B 54 24 04 56 8B F1` checked first, logged either way.

**Open.**
- Playtest at 1920×1080 **without changing resolution first**: the attack
  prompt's top corners, and the "Map" label on the key pad. Log line expected:
  `widescreen: interface camera updated -- culling planes rebuilt`.
- If the "Map" label is still missing, bisect with `clusterCount = 0`
  (`storms.ini`), as planned.
- Then the hunters (waypoint routing, `+0x3E`/`+0x40`, the `0x94` table).
- Release checklist from round 9 still stands (`kDebugHotkeys`, Ctrl+Shift+D).

### Test round 11 — interface layers; the real hunter chase; the challenge card

**Found.**
- *Observed* (screenshots): Options slider knobs, the in-game menu's icon bar and
  the whole character-creation layout drawn stretched to full width, with clicks
  missing their targets. This session started at 1440×1080 and switched to
  1920×1080 in game; round 3 (correct) had started at 1920.
- *Decompiled*: `FUN_00503CA0` builds an interface LAYER with its own camera
  (heap object, camera at `+0`; callers `0x004B1794`, `0x004D5AE2`,
  `0x004DD1DD`), always at the fixed 4:3 view (±0.5, ±0.375, 921.6/1536).
  SetResolution re-fits only the main interface camera (`0x008E9FE0`), so a
  layer built at 4:3 stays 4:3 after a switch to 16:9 and is stretched.
- *Observed*: the attack prompt's top corners are drawn again (round 10 fix).
- *Decompiled* (REA, `FUN_00467F90`, the per-ship sailing update) — **AI ships
  DO chase the player, and the game does the steering.**
  - `+0x44` (`0x0081433C`) is the heading a ship wants, `+0x14` (`0x0081430C`)
    the one she has; each tick she turns toward the wanted one.
  - A ship with purpose (`+0x02`) **1, pirate-hunter**, steers every tick for
    the player's position (`0x008B96B4`/`0x008B96B0`, copied from slot 0 at the
    top of the same function) through the game's terrain-aware pathing
    (`FUN_004750C0`), and skips the "arrived, so dock" test.
  - She stays a hunter only while (`0x0046AECC`): distance to her home port
    `FUN_00405D90(+0x3E)` ≤ that port's reach `FUN_0045E6B0`
    = `(int16 [0x0085BF7E + c*0x94] / 50 − reputation[port's crown]) ×
    ([0x0085A158] + 2) / 2` cells; `!(0x0085A164 & 0x400)`; and the damage
    word at **absolute** `0x0081431A` (slot 0's, not hers) ≤ `(10 − rep) × 5`.
    Failing any: purpose 3 (raider) and `FUN_0045B740` (flag `0x80`, go home).
  - `FUN_004DB270` is the angle function (dx, dy → 2^32 = 360°).
  - ⚠️ **Correction**: `+0x3E` is the port a ship RETURNS to (and is leashed
    to); `+0x40` is where she is BOUND. `GAME_API.md` and `game.h` had them the
    other way round. A ship in port (flag `0x200`) stays docked while
    `+0x40 == +0x3E` and sails once they differ (`0x00469698`).
  - *Inferred, consistent with the August measurements*: the old "aim her at a
    port beyond the player" re-aim wrote `+0x3E`, the leash port, every 5 s —
    so it worked by keeping the leash next to the player, while the pursuit was
    the game's own all along.
- *Decompiled*: the engine's own AI-vs-AI chase/engagement (a hostile neighbour
  inside 3000 → wanted heading = angle to her; inside `R/6` → mutual
  engagement) is guarded `if (other != 0)`: the player is never engaged by the
  overworld AI. Contact remains the player's to make.

**Changed.**
- `widescreen.h` — every interface camera that passes through set-frustum is
  remembered (vtable, live refcount and exact frustum checked before any
  write); when SetResolution runs, each is re-fitted for the new size and
  updated. `UpdateCamera` shared with the round-10 fix.
- `suspicion.h` — hunters rebuilt on the game's chase: purpose 1, home port the
  port that sent her, bound elsewhere so she leaves harbour. **The leash is
  lifted for PEMF's hunters only**: the call at `0x0046AEF0` (E8 → `0x0045E6B0`,
  one of its two callers) goes through `PemfHunterLeashStub`, which returns
  unlimited reach when `EBP / 0x45C` is a slot PEMF marked, else jumps to the
  game. The re-aim is gone. If the game still calls her off (view flag 0x400 or
  the damage test) PEMF puts her back; three times in 10 s and she is let go
  ("she turns for home"). A hunt ending hands her back as the game ends its own
  (purpose 3, flag 0x80, home port). A hunter culled while far astern is
  announced: "we've lost her". Unmasking no longer dispatches a hunter while the
  crown still tolerates us (reputation ≥ 0 after the penalty) — that ship was
  called home on the next tick.
- **The challenge card** (design: `SUSPICION.md` thresholds, ~90) — new trigger
  field `suspicion` and effects `holdRuse` (gamble; base chance ± officer
  discretion; success drops the meter below the first rung, failure unmasks),
  `strikeColours` (come clean; reputation cost from the value, default 0; no
  hunter), `run` (keep the flag; out of sight it cools at the far-away rate at
  once). `content/PEMF/events/suspicion_events.json` holds the card; with it
  loaded the ladder's own "coming about" line is silent.
- `game.h` — the chase write-up and accessors (`kShipReturnPort`,
  `kShipBoundFor`, flags, `ShipCellDistanceToCity`, `HunterLeashReach`).
- `core.cpp` — `suspicion::InstallLeash` at the first safe point,
  `RestoreLeash` on unload.
- `re/out/ai_sailing_decomp.c`, `ai_sailing_asm.txt` (local only).

**Open.**
- Playtest the hunter: Ctrl+Shift+8 a flag and get unmasked, or Ctrl+Shift+U to
  drop standing near one of their ports. Log: `hunter leash lifted`,
  `dispatches a pirate-hunter … leash lifted`, then every 5 s `dist …, closed …,
  heading … (wants …)` — closed positive and heading tracking means she is on
  us.
- Playtest the card at suspicion 90, all three choices.
- Interface layers: start at 1440×1080, switch to 1920; Options knobs and
  character creation. Log: `interface layer camera(s) re-fitted`.
- Contact: a hunter alongside does not open fire (engine design, above). Whether
  PEMF should force the engagement is a design decision, not taken.

---

## 2026-10-16 (night) — Phase 2: flags you have, three kinds of suspicion, events

**Goal.** Owner's list for the next phase: (1) flags are earned — a letter of
marque gives that crown's flag; (1b) crowns at war with the flag you fly grow
suspicious; (1c) the black flag draws suspicion from every crown, faster the
closer they are; (1d) the tavern's item seller sometimes sells flags; (2) hunters
for any crown you are below zero with, and that crown stops dealing with you;
(3) new events and an economy overhaul; (5) the suspicion panel fixed to the
top right at any resolution; (6) the sailing HUD in the true screen corners;
(7) more town interactions; (8) more explosive sea combat; (9) clothing options
at character creation. Everything must stay a plain extract-and-play install.

**Done.**
- Two research passes run in parallel with REA, each writing only to
  `re/experiments/` and `re/out/`: HUD anchoring, the tavern seller, hostile
  towns, town-menu hooks and the letter of marque
  (`re/experiments/hud_and_towns/`); sea-combat effects, the price model and
  character looks (`re/experiments/combat_economy_look/`). Their results land
  in a later entry.
- `tools/validate_events.ps1` (new): builds PEMF's own event loader into a small
  x86 program and runs it over a folder of event files. 19 events in 4 files
  load with no rejections.

**Found.**
- *Decompiled* (SetResolution, `0x004D3AE6`–`0x004D3AFD`): `ScreenW`/`ScreenH`
  (`0x0085A26C`/`0x0085A268`) are the INTERFACE's virtual size, copied from
  `0x0072637C`/`0x00726380`; the device size is `0x008CAC18`/`0x008CAC1C`. The
  suspicion panel was placed at 7/8 of ScreenW, i.e. inside the centred 4:3
  area — "towards the centre" at 16:9.
- *Observed in source*: hunters for ANY crown below zero already retry every
  15 s (`RetryDispatches`), so item 2's hunting half was in place; the refusal
  half is in the research pass.

**Changed.**
- `session.h` — `flagsOwned` (bit per crown) in the sidecar; `OwnsFlag`,
  `GrantFlag`, `OwnedFlags`.
- `core.cpp` — `CanFly`: your own colours, the black flag and personal devices
  are always aboard; a crown's flag needs its letter of marque (rank > 0,
  `0x00869A88`) or a purchase. `GrantMarqueFlags` (once a second) stores the flag
  for good and tells the player. Ctrl+Shift+8 cycles only flags aboard ("We have
  no other colours aboard"). `NoteColoursFlown` reports the flag on the mast and
  the black flag to suspicion.
- `suspicion.h` — one meter per crown, fed by DECEIT (their colours, not ours),
  ENEMY (colours of a crown they are at war with; own colours count unless
  `enemySeesHonestFlag = 0`) or the BLACK FLAG (every crown). Each watcher
  weighs `2 × (range − d) / range`: nothing at the edge, double alongside. Rates
  in thousandths, carry 64-bit. At 100: deceit unmasks (as before); enemy and
  black flag `Brand` — same ledger penalty, hunters if below zero, colours stay
  up. Rung lines per kind. Panel: what is flying, the worst meter by crown, who
  is looking, hunters; shown while anything could draw suspicion or any meter
  is still cooling.
- `content.h` — the panel's corner comes from the interface camera's frustum
  (`[0x008E9FE0] + 0x128`): right = ScreenW/2 + r·ScreenW, top = ScreenH/2 −
  t·ScreenW; inset 80 / 40. Identical to before at stock 4:3 (560, 40).
- `content/PEMF/suspicion.ini` — the three kinds documented; new keys
  `riseEnemyShip/Port`, `enemySeesHonestFlag`, `risePirateShip/Port`.
- `content/PEMF/events/world_events.json` (new) — eight happenings: wreckage,
  fever, a castaway, deserters in harbour, a fair wind, an empty purse,
  dolphins, a long voyage.

**Open.**
- Playtest: flags aboard (Ctrl+Shift+8 without a letter offers only own colours
  and the black flag); the black flag near any crown; an enemy's colours; the
  panel's corner at 1920×1080.
- Items 1d, 2 (refusal), 3 (economy), 6, 7, 8, 9 wait on the research passes.

---

## 2026-10-16 (late night) — a hostile crown's towns; flags from the tavern

**Goal.** Items 2 (refusal half) and 1d of the Phase 2 list. Owner rule for 1d:
the trader's own wares stay; a flag is an addition, never a replacement.

**Done.**
- Both research passes were stopped before they wrote their READMEs, so
  `re/experiments/hud_and_towns/` and `combat_economy_look/` do not exist. The
  decompiled functions they pulled are kept locally: `re/out/town_decomp.c`,
  `tavern_decomp.c`, `governor_decomp.c`, `battle_decomp.c`.
- Built clean (no warnings) and installed to the Steam game at 23:16.

**Found.**
- *Decompiled* (`FUN_00406F60`, the tavern, `0x00409A28`–`0x00409B78`): the
  trinket seller's card is composed with AddText (`0x004F6090`/`0x004F60B0`),
  `WrapText(0x30, 0)` (`0x004879F0`), his item's picture (`0x0042F7D0`), then
  presented by ShowDialogDirect at `0x00409AEF`. Two options when the purse
  covers the price (EBP), none otherwise ("Unfortunately you only have …").
  Result 0 = bought: `sub [0x00869AB4], ebp` at `0x00409B27`, the item's level
  byte `0x00869B20[item]` +1.
- *Decompiled*: ShowDialogDirect clears the card picture on the way out
  (`0x00431127`: `0x00725408` = −1), so a second card shown after his does not
  carry his item's picture.
- *Decompiled*: in a village or mission (`0x00860B80` = 5 or 6) the town menu
  remaps the raw row after the present call (`0x00411925`): 1 → 2, higher +3.

**Changed.**
- `townmenu.h` — below zero with a crown, its governor (action 0), merchant (2)
  and shipwright (3) refuse with a card and the menu is shown again; the tavern
  stays open. The raw row is remapped as the game will before judging it.
  Applies on both the stock path and the path with PEMF rows.
- `flagtrader.h` (new) — redirects the call at `0x00409AEF`. His card is shown
  and answered first, untouched, and his answer is returned unchanged. Then, on
  `flagOfferChance`% of visits, a second card offers a crown's flag the captain
  has neither bought nor been given by letter of marque, for `flagPrice`
  (his price is counted against the purse first if he just sold something).
  Bought: the gold goes, `session::GrantFlag`.
- `core.cpp` — `flagtrader::Install`/`townmenu::Install` at the first safe
  point, `LoadSettings`, `Restore` on unload.
- `content/PEMF/suspicion.ini` — `flagOfferChance = 35`, `flagPrice = 1000`.
  The installed copy in the Steam folder is from August and does not have the
  Phase 2 keys (deploy never overwrites tuning files); the code defaults equal
  the repo values, so play is unaffected.

**Open.**
- Playtest the refusal: drop below zero (Ctrl+Shift+U) and walk into one of
  their towns; governor, merchant and shipwright refuse, tavern works, leaving
  works. Also a native village (not a crown: never refused). Log: `townmenu:
  refused at city …`.
- Playtest the flag seller: tavern visits until he offers; buy with and without
  the gold; buy his item AND the flag on one visit (the purse check). Log:
  `flagtrader: also offering …`, `bought the … flag`.
- Items 3 (events, economy), 6 (HUD corners), 7 (more town interactions), 8 (sea
  combat), 9 (clothing) not started.

---

## 2026-10-16 (late night, cont.) — the HUD in the screen corners

**Goal.** Phase 2 item 6: at 1920×1080 the HUD sits at the edges of the centred
4:3 area. Put it in the corners of the screen, with its mouse areas.

**Done.**
- REA (Ghidra, full analysis this session): `FUN_004741B0` (sailing HUD),
  `FUN_00473B70` (keypad), `FUN_00476190` (battle buttons and compass),
  `FUN_004BBA60`, `FUN_004AF080`, `FUN_00412900`, `FUN_004884A0`. Battle panel
  block of `FUN_0047A040` read from the disassembly. Saved locally:
  `re/out/hud_corners_decomp.c`.
- Every patch site checked offline against the GOG image: 51 draws, 13 keypad
  calls, 4 hover reads, 2 mouse conversions, all as expected.
- Built clean, installed to Steam at 23:44.

**Found.**
- *Decompiled*: all HUD primitives take x as a stack argument and centre with
  `x − [0x0072637C]/2`; the full table is in `WIDESCREEN.md` "The HUD in the
  screen corners". Layout is in fractions of ScreenW or constants.
- *Decompiled*: the text primitive also scales by `[0x0072637C]` (`0x004B09EC`),
  and the keypad sizes its buttons from ScreenW (`W/16`) — so neither global can
  be changed to move the HUD.
- *Decompiled*: the HUD hover-tests the cursor against its own computed x;
  battle buttons and quests act on the hovered id (`0x00725828`, `0x0072568C`);
  the keypad registers click boxes (region table at `0x0086C330`, layout in
  `WIDESCREEN.md`).
- *Decompiled*: the mouse handler converts `LOWORD(lParam) × 640 / device W`,
  unsigned (`0x0042C23F` move, `0x0042C3C5` button down); called synchronously
  from the window procedure through `0x004D3E00`.
- *Decompiled*: `0x008DC30C`/`0x008DC310` are the cursor in interface units
  (not a HUD anchor).

**Changed.**
- `hudcorners.h` (new) — call-site stubs move each HUD draw's x: LEFT (bottom
  left cluster, quests, date, guns-loaded bar), RIGHT (keypad), SIDE (battle
  panels and buttons). Hover reads see a cursor moved the opposite way; the
  keypad's click boxes are moved after it returns; the mouse reaches the
  margins only while a moved HUD is on screen.
- `widescreen.h` — `hudCorners` setting; the window subclass keeps the raw
  mouse position for hudcorners.
- `core.cpp` — install after widescreen, per-frame `Tick`, restore on unload.
- `content/PEMF/widescreen.ini` — `hudCorners = 1` documented.

**Open.**
- Playtest at 1920×1080: sailing (compass, fame, purse, quests, date bottom and
  top left at the screen edge; keypad bottom right), hover the keypad and click
  its buttons, hover a quest icon; a sea battle (both ship panels out to the
  sides, buttons and compass, guns-loaded bar); on land and in a duel the
  keypad. Log: `hudcorners: … redirected`, `HUD in the screen corners, 106
  interface units out each side`. Screenshots with Ctrl+Shift+X.
- At 4:3 nothing should move (margin 0).
- Not moved yet: HUDs other than the keypad in land battles, duels, dancing and
  town walking; the gamepad cursor cannot reach the margins.

---

## 2026-10-17 — the waterfront, a moving market, a louder sea battle

**Goal.** The rest of the Phase 2 list: item 7 (town interactions), item 3
(economy), item 8 (sea combat; the owner asked specifically for more smoke,
more debris and boomier shots), item 9 (clothing), item 4 (passes, journal).
Owner rule throughout: ADD to the game, never replace its own things.

**Done.**
- The stopped research passes left their transcripts; the decompiles they had
  pulled were recovered into `re/out/economy_decomp.c` (local only) instead of
  being paid for twice. REA re-analysed this session for the battle functions.
- Built clean, installed to Steam 08:05 (waterfront, market) and 08:14
  (combat). `tools/validate_events.ps1`: 23 events, 5 files, no rejections.

**Found.**
- *Decompiled* `FUN_00428570(city)`, cdecl: the town's PRICE TABLE, int16 at
  `0x0085BFB4 + city*0x94 + good*2`, good 0..6 = Gold (always 1), Food,
  Luxuries, Goods, Spice, Sugar, Cannon. From town size (`0x00860B75`), wealth
  (`0x00860B7C`), world averages (`0x00861B70/74`, rebuilt by `FUN_00428A30`,
  which runs from the world update `0x00460D12` and new-game setup
  `0x00404512`); a switch on city index gives some ports their specialties.
  Called fresh before use at `0x0041010B`, `0x004110B2`, `0x00428E0D`,
  `0x0044E8C7`. *Inferred*: these are the merchant's prices (magnitudes fit:
  food 3–9, goods 2–20, luxuries 5–46) — the `market:` log lines are there to
  confirm it against his screen.
- *Decompiled* `FUN_0040FE00(edx = city)`: the stock "will this port trade?"
  test (only Spanish ports ever say no — "We do not trade with @NATIONALITY
  heretics"). Callers `0x0040FF40` (merchant), `0x00410D30` (town menu).
- *Decompiled* `FUN_00476F10(type, x, y, height, f5, f6)`, cdecl: FLYING
  DEBRIS, a 40-piece pool at `0x008B99D8` (0x40 bytes; a busy random slot is a
  no-op). type 0/1 crewman in the water + splash, 2 `cannon_barrel.nif`, 3
  `cannon_truck.nif`, 4 `barrel_01.nif`. Hits throw one piece, sometimes:
  `0x0047DE17`, `0x0047DE91`, `0x0047E11B` (also `0x0047CBA9`, not a hit).
- *Decompiled* `FUN_00477CB0(side, ship, x, y)`: fires ONE shot into the shot
  table `0x008BA668` (24 per ship, 0x28 bytes); once per gun from the
  broadside loop at `0x0047E4E6`; side 0 is the player. The smoke and flash
  after each shot are drawn by `FUN_004BBC10` with size `EDX = 0x46`
  (`0x0047E56D`).
- *Decompiled*: the battle camera shake, `0x008CAB70..0x008CAB88` = four
  float amplitudes, start (game clock `0x008C8494`), duration ms, active. Stock
  shakes only when the player's ship is struck (500 ms) and on event `0x4D`.

**Changed.**
- `content/PEMF/events/waterfront.json` (new) — a "Walk the waterfront" row
  opening a menu: the harbourmaster (bribe), dockside gossip, dockhands (+3
  crew), the chandler (+morale).
- `suspicion.h` — `Bribe(cost)`: this port's crown forgets the fright (meter
  emptied, ledger DEBT written off, notoriety kept), so a hostile crown's
  harbourmaster is a way back; `Gossip`: hunters out, ports shut, who is asking
  questions, flags held. `content.h` — effects `bribe`, `gossip`; includes
  `session.h` itself.
- `market.h` (new), `content/PEMF/market.ini` (new) — the four price calls run
  the game's function, then scale its table: market news (a shortage +40–90%
  or a glut −25–50% of one good around one port, lasting 10–25 minutes,
  announced by name through `@CITYNAME`) and a war premium (food +25%, cannon
  +40% where the crown is at war). Logs the first 40 adjustments.
- `combatfx.h` (new), `content/PEMF/combat.ini` (new) — each struck hull
  throws `extraDebris` (2) more pieces (barrels, gun carriages, casks) through
  the game's own spawner; the player's broadside kicks the camera (35%,
  180 ms, never over a stronger shake); the smoke and flash after each shot
  150% (`0x0047E56E`, restored on unload). The impact draw's pushed 2.0 at
  `0x0047DC0B` was left alone: it is one of nine arguments and not proven to be
  the size.
- `core.cpp` — install/restore/settings for market and combatfx; the market
  ticks once a second with the career live.
- `build.ps1` — `market.ini`, `combat.ini` installed (never overwritten).

**Not done.**
- Item 9 (clothing at character creation): not started. Outfits are believed
  to be whole separate models; doing it properly is its own RE session
  (character creation screen, the model choice, material tint).

**Open.**
- Playtest: the waterfront (each option; the bribe in a hostile port — their
  ports should reopen if the debt was all that kept reputation below zero);
  market news at sea and the merchant's prices against `market: city … ->`;
  a sea battle (debris on every hull hit, the camera kick on your broadside,
  bigger gun smoke). Log: `combatfx: installed …`, `market: prices move …`.
- Release checklist (round 9) still stands, plus: the "Ask after the
  harbourmaster" TEST row and the crew "Test 1..5" menu in `core_events.json`
  are scaffolds — decide before release.

---

## 2026-10-17 (cont.) — Suspicion V2, a crown-less start, keys and menus

**Goal.** From the owner's playtest: hunters and suspicion far too aggressive;
meters rising everywhere; a captured ship seeming to count against us; no flag
ever offered in the tavern; a proper flag scene with the letter of marque and an
easy key to change colours; start serving no crown; gentler wind; louder sea
battles; overlapping text; the duplicated harbourmaster; a crew menu at sea with
officers that matter; an in-game guide.

**Found.**
- *Observed* (log): the black flag "marked" the player with the Dutch every
  ~6 s, reputation 0 → −80 in under a minute, French later −502; four French
  hunters dispatched in one instant from one port. Cause, *observed in source*:
  Observe() scanned from slot 1, and **slots 1–7 are the player's fleet** — a
  prize keeps its nationality, so a captured Dutch ship counted as a Dutch
  watcher at point blank everywhere; plus `Brand` re-firing on every refill.
- *Observed* (log): the hunter chase itself works (closed 1,600–2,300 per 5 s).
  Circling at a port = the chase target is the player, who is IN the port.
- *Observed* (screenshot): stacked "pirate-hunter" labels and hails — hunters
  spawned together from one port.
- *Decompiled*: `FUN_00404FF0(nation)` (EAX = amount, EBX = hostile) is the
  game's own hostility bookkeeping for sacking a town (one caller, the town
  approach `FUN_004054A0`): the victim's reputation falls (twice over while
  positive), its enemies' rises, its allies' falls. `FUN_004051C0(city, delta)`
  is the "good deed for a town" credit. Ship attacks change reputation through
  code that writes it indirectly; the standing ledger's "the engine took
  reputation we did not" is the general detector and now drives aggression.
- *Decompiled*: the rank array `0x00869A88` is only ever INCREMENTED, by the
  governor (`0x0040D3BF`, `0x0040D5FF`) — character creation grants no rank.
  The "nation you serve" cache `0x00869AA8` is set at career start to the
  STARTING PORT's crown (`0x004722F7`, with the starting ship, crew and gold),
  rewritten by the governor (`0x0040D6C9`), and otherwise only read as
  @NATIONALITY text or compared (`0x0040FE10` heretics, `0x0040DA19`) — never an
  index. `[NATIONALITY]` has a 5th entry, "Pirate".
- *Decompiled*: the sailing view's raw-key table (`0x00473968`) uses Tab, Esc,
  Space, +, -, =, A, L, P, S, num1, num8, F3; letters B C E G H I J K M N O Q R T
  U X Y Z are free (with KeyMap.ini's v f w a s d).

**Changed.**
- `suspicion.h` — V2. Observers from slot 8 (never the player's fleet). Rates in
  tenths of a point per second, at most `maxWatchers` counted; enemy colours and
  the black flag at 60%/70% of a disguise. `OnAggression` (from the ledger)
  spikes the victim crown's meter (+35) and blows a disguise worn at the time.
  At 100: no reputation loss — the nearest ship of that crown TRAILS us
  (purpose 1, PEMF leash) and backs off after `trailBackOffSec` without
  aggression, the meter dropping to the first rung. `Brand` removed. Hunters
  only below zero, count from reputation (−20/−40) and notoriety (30+), ONE at a
  time per crown every `huntIntervalSec`, `huntCooldownSec` after a hunt ends, a
  global cap, never two from the same port; they give up after `harbourWaitSec`
  with the player sitting in port.
- `standing.h` — `g_onAggression` hook; `kDebtCap` 15 / `kNotorietyCap` 25,
  applied every tick (repairs an affected save on load).
- `core.cpp` — keys Q (colours menu: every flag aboard, the one flying marked),
  C (crew: crew, plunder, months, officers roster, a walk on deck), H (the PEMF
  guide), read only at sea with the game in front and no card up; `keys.ini`.
  The letter of marque is now a card from the governor's secretary. Unaligned
  start: a new career (months 0, no rank) gets `0x00869AA8` = 4 (Pirate) and the
  black flag as its own colours. The officer hook for content.
- `content.h`, `triggers.h` — `requiresOfficer` on events, checked in
  `Eligible`. `content/PEMF/events/officer_events.json` (new): gun drill
  (master gunner), a stash (quartermaster), scurvy (surgeon), a passage
  (sailing master).
- `content/PEMF/events/pemf_guide.json` (new) — the guide: flags, suspicion,
  crowns and hunters, in port, at sea.
- `core_events.json` — the test row "Ask after the harbourmaster" (duplicating
  the waterfront), its event and the unused "Test 1..5" crew menus removed.
- `flagtrader.h` — every visit logged with its reason; chance 60%.
- `wind.h`, `storms.ini` — turning in tenths of a degree a second; meanders
  ±30°, ≥ 8°, every 150–360 s at 0.3°/s; fronts every 20–40 min at 2°/s.
- `combatfx.h`, `combat.ini` — 5 extra pieces per hit (25% a man overboard with
  his splash), spread 350, smoke 220%, broadside kick 55% / 240 ms, a 20% jolt on
  every hull hit.
- `suspicion.ini` — rewritten for V2 (old `rise*` keys deliberately ignored);
  `keys.ini` (new); `build.ps1` installs `keys.ini`.
- `tools/validate_events.cpp` — lists menus and checks every option leads
  somewhere (after `ResolveMenus`, as the game does): 26 events, 7 menus, clean.

**Open.**
- Playtest everything above; a NEW career for the unaligned start.
- The suspicion panel still draws over cards the sailing view shows itself
  (e.g. "News of your victory…"); the game's own "a message is up" state is not
  yet found. And "some things won't render" needs specific screens.
- Officer loyalty (a relationship per officer) — not built; the crew menu and
  officer-gated events are.
- The guide is on H rather than the pause screen: a pause-menu row needs the
  pause screen reverse-engineered.


## 2026-10-17 (cont.) — no crown at the tavern, smoke that lingers, suspicion by relationship

**Goal.** From the owner's playtest: more smoke in sea battles that lasts longer,
and more debris; Q/C/H must not work in a sea battle (Q also changed shot type);
no faction choice at character creation at all — start in a pirate port; and
suspicion still "rises no matter what": with a crown's letter of marque AND its
colours that crown should treat you as a friend until you act against it, worse
deeds costing more; the black flag makes everyone suspicious.

**Found.**
- *Decompiled* (`FUN_0046FE30`, the tavern of character creation): "Take yer
  pick!" is a loop that runs while `[0x00876BE0] == -1` (`0x00470391 cmp
  [0x876BE0], edi` / `jne 0x00470782`). Picking a table stores a random large
  colonial city (index < 0x2C, size ≥ 5, economy ≥ 0x32, flags & 0xC0000000
  clear) of that table's crown. The crowns of the four tables are local_154 =
  {2,0,3,1} at `[esp+0x70]`; after the loop `0x004707BB` switches on
  local_154[ebp] to play the walk to that table, and anything above 3 takes the
  default (no walk). `FUN_00419370` ("Select Nationality...", mutiny.pcx) is a
  developer duel-test menu, not character creation.
- *Decompiled* (new-career order, `0x00471AB0` then `0x0042BDF4`): default
  settlement records are copied in (`0x007242D0` → `0x00860B68`, 0x400 dwords),
  then the tavern, then `FUN_0046C8D0(0)` generates villages, missions and
  PIRATE HAVENS (nation byte 4, `0x0046D597`), then `FUN_00471E00(0)` career
  setup. It picks the start city EDI of the sign-up city's crown
  (`0x00471F9B..0x00471FE6`), reads the starting ship/crew/cannon/gold from a
  20-row table indexed (crown + era*4) at `0x00472264..0x004722D4`, writes the
  nation cache `0x00869AA8` (`0x004722F7`), sets that crown at war with Spain
  if it is not Spain (`0x00472318`), and places the ship from the city's
  harbour direction (word `[city*0x94 + 0x0085BFF6]`, 0..23) and position.
- *Decompiled* — CORRECTION to the previous entry: `0x00869AA8` IS used as an
  index. `FUN_004BCD90(outfit, nation, head)` dresses the player's model, and
  eight call sites pass the cache as `nation` (`0x0044F2D9`, `0x0045337B`,
  `0x0045338D`, `0x00483745`, `0x00483C7F`, `0x00486031`, `0x0048715E`,
  `0x004D5FE1`): `sprintf("PC_%s_%c.dds", outfitName[outfit],
  0x0070D594[nation])`. `0x0070D594` is "SEFD"; the Pirate's fifth letter is 0.
  *Observed* (owner screenshots `PEMF\shots\key_0943*.bmp`): a pink, untextured
  captain in the Port Freedom tavern. *Reference* (extracted assets): only
  `pc_<rank>_{s,e,f,d}.dds` exist.
- *Decompiled* (`FUN_004AB290`, `FUN_004AB530`): the TIMED EFFECT list. 12
  arguments, callee-cleaned (`ret 0x30`), the list in EDI; arg 10 = lifetime,
  arg 11 = playback rate in percent (+0x2C = rate/100). The update ages each
  node by dt × rate and removes it past its lifetime, so a lower rate = slower
  and longer on screen. The cannon-hit blowout (`0x0047DF65..0x0047E044`) picks
  one of three effects at `0x008E66B4` (`fx_cannon_hit_blowout01/02.nif`…, loaded
  `0x00480B26..`), rate `push 0x64` at `0x0047DF75`, list `0x007268DC`.
  The muzzle smoke at `0x0047E56D` is different: one per-side effect object
  re-placed on every shot (`FUN_004BBC80`), so it cannot be multiplied the same
  way; its size is what combatfx already scales.
- *Observed*: the installed `PEMF\suspicion.ini` and `storms.ini` were still
  the PRE-V2 files (deploy never overwrote an existing .ini), so the last
  playtest ran with the old ranges (`shipRange 6000`, `heatRadius 20000`), old
  decay and `maxPerNation 4`. Every player who updated had the same problem.
- *Observed*: the Steam exe is SteamStub-packed on disk (sections `.text`,
  `.rsrc` only), so patch sites can only be verified at runtime. Launching it
  directly exits silently before the first frame (no crash report); launch it
  with `steam://rungameid/3920`.

**Changed.**
- `piratestart.h` (new), on by `suspicion.ini startUnaligned`:
  `0x00470391` → call: a sign-up city of a random crown is chosen with the
  game's own test and local_154[0] set past 3, so the tavern loop and the walk
  never run (the original `cmp` is redone after the call for the `jne`);
  `0x00471FE8` → call: the start city becomes a random intact pirate haven with a
  valid harbour direction (none → the game's choice); `0x00472264` → call: the
  stats row is read with the sign-up crown, never the haven's 4.
  `0x0070D598` (the Pirate's outfit letter) = a crown's letter,
  `pirateOutfit` (default 1, English) — always, so older pirate-start saves get
  clothes too. All sites byte-verified first; all restored on unload.
  *Observed* (log, Steam): "career begins at pirate haven 81/99"; the owner
  confirmed the captain is dressed in the tavern.
- `combatfx.h`, `combat.ini`: the blowout call `0x0047E044` → `PemfBlowout`
  (the game's blowout, returned untouched, then `extraBlowouts` = 2 more, any
  facing, ±`blowoutSpread`, 85–115% pace); the blowout rate byte `0x0047DF76`
  100 → 55 (`smokeLinger` = 180%). Defaults `extraDebris` 8 (cap 12),
  `muzzleSize` 260. *Observed* (log): "blowout rate 100 -> 55 (+2 each)".
- `suspicion.h`: `WhyFor` by relationship — black flag: everyone; their own
  colours with their commission (rank > 0): no suspicion; without one: deceit;
  an enemy crown's colours: only if they already think badly of us (reputation
  < 0). The rate is scaled by `RelationPercent` (100% at 0, down to 40% liked,
  up to 250% loathed). `OnAggression` spikes by `aggressionSpike` × severity
  (the engine's reputation drop, 1–3). `core.cpp NoteColoursFlown`: a
  commissioned flag is honest, not worn. (`enemySeesHonestFlag` no longer read.)
- `core.cpp`: Q/C/H also refused in a sea battle and anywhere but the sailing
  view (screen depth 3).
- `build.ps1`: tuning files are now refreshed. A copy of each as installed is
  kept in `PEMF\defaults`; untouched files are replaced, hand-edited ones kept
  with the new one beside them as `.ini.new`, files older than the scheme
  backed up to `.ini.bak` and replaced.
- `suspicion.ini`, `pemf_guide.json`: text for the relationship model, the
  tavern without a crown, keys only at sea, lingering smoke; `pirateOutfit`.

**Open.**
- Playtest: the battle smoke and debris; suspicion while commissioned, under
  enemy colours, under the black flag; Q/C/H in battle.
- The pirate start skips the "Welcome aboard" walk; the opening text before it
  ("The captains at yonder tables will gladly take ye aboard") still shows.
- The war-with-Spain step at career start now sees the haven (nation 4, already
  at war with everyone) — a no-op; the sign-up crown is not put at war with
  Spain as a crown start would be.
- `flagOfferChance` in the shipped suspicion.ini is 35 (code default 60).

---

## 2026-10-17 (cont.) — PEMF checked against the unpacked Steam executable

**Goal.** Pick the project up from a fresh session and check, without
launching the game, that every address and patch PEMF depends on is correct
for the Steam build (the target).

**Done.**
- Read the journal, `GAME_API.md`, the source's address declarations and the
  uncommitted 0.2.6+ work (piratestart, combatfx, market, flagtrader,
  hudcorners, sailmaster, gameaudio, battlemusic, widescreen, suspicion V2).
- Used the **unpacked Steam image** from the reference workspace
  `D:\pirates-reference` (decoded statically from the packed file; the
  loader's own checksum matches) and its Ghidra disassembly.
- Wrote `re/experiments/steam_static_verification/verify_steam.py`. It is
  read-only and re-runnable, and compares GOG and Steam byte for byte, maps
  moved data, and checks every source address, signature and patch site.
  Write-up and results in that folder.
- No source code, no build and no deploy were changed.

**Found.**
- *Decompiled* (Steam image vs GOG file): **the code and all game-state
  addresses are the same in both builds.** All 417 absolute addresses in
  `src/` hold the same bytes on Steam where PEMF reads, writes or checks
  them. All 32 byte signatures are present on Steam at the address PEMF checks.
  Every code address is an instruction start (281), an operand (9) or a jump
  table (2). Widescreen's 24 set-frustum sites are exactly the 24 `E8` calls
  to `0x004CCE40` on Steam.
- *Decompiled*: **Steam is not byte-for-byte the GOG build.** It is a separate
  link of the same 1.0.2.0 source. 599 pointers to read-only data (vtables,
  float constants, some strings) point to different `.rdata` addresses,
  mostly 8 or 24 bytes apart. Steam has one extra 6-byte function at
  `0x004EDAC0`, which moves three small functions 16 bytes later up to
  `0x004EDB60`. Nothing PEMF uses is affected. The earlier "the Steam build
  is the GOG build once unpacked" is true for code and game state only.
- *Decompiled*: the one source address that is wrong on Steam is
  `WorldTextTilt` (`0x00713600`). On Steam the double is at `0x007135F8`.
  It is declared but never read (PEMF uses `kDegToBam`), so nothing breaks.
- *Decompiled*: docs cite three more GOG-only data addresses: the 1024/768
  input constants (`0x007135C4`/`C8` GOG, `0x007135BC`/`C0` Steam) and two
  sound-name strings. None are used in the source.
- *Decompiled*: patch logic read against the Steam disassembly and found
  correct. Pirate start: the tavern stub's `[esp+0x70]` matches the game's
  own `[esp+ebp*4+0x80]` with 16 bytes of arguments pushed, the `cmp` is
  redone for the `jne`, and EDI carries the start city. Combat effects: the
  blowout call takes 12 pushed arguments and `PemfBlowout` is a 12-argument
  `__stdcall`, so the stack balances. Rain: the 7-byte `imul` site is one
  whole instruction and is byte-checked before writing.
- *Observed* (file): the Steam packer is **PECompact 2** (`PEC2` marker, no
  `.bind`, no Steam API), not SteamStub as written in the previous entry.
  The first-safe-point rule for code writes still applies.
- *Decompiled* (reference workspace, for the record): `0x00538400` restores
  its vtable and decrements the instance count at `0x00727444`. It is a
  destructor, as `game.h` says; the reference had misnamed it a constructor
  and will be corrected there.

**Changed.**
- `re/experiments/steam_static_verification/` (new) — `verify_steam.py`,
  `README.md`, `results/*.json`.
- `docs/GAME_API.md` — dated update under "Store builds" (what can now be
  checked, what differs between the builds, PECompact); Steam addresses beside
  `WorldTextTilt` and the two sound-name strings.
- `docs/WIDESCREEN.md` — Steam addresses of the 1024/768 constants and a
  warning not to write them by GOG address.

**Open.**
- `game.h`: `WorldTextTilt` could be deleted or given a comment naming the
  Steam address. It is unused; left alone because this session changed no
  code.
- `storms.h` around line 1185: the comment above `WriteRainAmount` describes
  the storm-scale patch (`lea`/`shl` → `imul`, 6 bytes), not the rain patch it
  sits on (7-byte `imul eax,[0x0085A0F8]` → `imul eax,eax,imm8` + 4 NOPs).
  Comment only.
- Add `verify_steam.py` to the release checklist: run it whenever an address
  is added.

---

## 2026-10-17 (cont.) — The text and event system, mapped end to end

**Goal.** Start mapping the game one system at a time, beginning with what
PEMF already uses most: composing text, the token formatter, and presenting
cards, dialogs, HUD and world text. Use PEMF's verified findings as the
starting point and fill in what was still unknown.

**Done.**
- Took PEMF's recorded findings for the text system (`GAME_API.md`,
  `game.h`, `townmenu.h`, `widescreen.h`) and checked each against the
  unpacked Steam image (all identical; see the previous entry).
- Read the formatter, its token lookup, the loader that reads token types
  from `text.ini`, and the save code that writes name records.
- The reference workspace's Ghidra database now carries these names, each
  marked with where it came from (PEMF's docs, or the code). This repo gets
  only the facts; no decompiled code is committed.

**Found.**
- *Decompiled*: **every token's argument count, not just the five read by
  hand.** The count follows from the token's type in `text.ini`: one for
  `__NUM`/`__STR`/`__CHAR`/`__VAR`, three for the gender and name types.
  `text.ini` defines 72 tokens: 63 lookups, `NUM`, `CHAR` and 7 name tokens
  (`CITYNAME`, `SHIPNAME`, `PCNAME`, `TOP10`, `NPCNAME`, `EVILNAME`,
  `FAMTEXT`). An unknown type becomes `__STR`; an unknown `@WORD` is left as
  written and takes nothing. Full table in `GAME_API.md`.
- *Decompiled*: a name token's three arguments are `{gender, plural, string}`,
  where the string is the game's own type (length at `ptr - 4`). Passing a
  plain C string would read garbage as the length. The save code writes the
  city-name records in exactly this layout.
- *Decompiled*: **the message buffer holds 4,096 bytes** (`InitEngine` sets
  capacity `0x1000` at `0x004D2A49`), and the formatter bounds every copy to
  it. The "capacity never measured" note in `townmenu.h` is answered; the
  1 KB ceiling there is safe.
- *Decompiled*: **`AddText` mode 0 replaces, mode 1 appends.** This explains
  both the "AddText replaces the buffer" lesson and why mode 1 is the
  town-menu path: rows are appended one at a time.
- *Decompiled*: `0x0042E1D0`, PEMF's main loop and safe point, has 47 callers
  including `ShowDialog` and every modal loop. It is one frame of the game, run
  from wherever the game is waiting, which is why modal cards still see
  frames.

**Changed.**
- `docs/GAME_API.md` — "Every token, and how many arguments it takes" and
  "The message buffer and the two AddText modes".

**Open.**
- The content loader still allows only `@NUM` and `@HAPPY`. The next step
  there: allow `__VAR` tokens with a range-checked index (counts are in
  `text.ini`), and the name tokens via a helper that builds a game-style
  string. Code change; not made.
- Next system: storms and weather.

---

## 2026-10-17 (cont.) — Release 0.3.0, The Black Flag

**Done.** Owner tested and approved the build; released as 0.3.0.
`build.ps1 -Package` → `dist\PEMF-0.3.0.zip` (35 entries, forward-slash paths,
`INSTALL.txt` and both DLLs at the root). Hashes in `README.md` and, for the
DLLs, `tools/check_install.ps1`; 0.2.6 moved into the older-releases table of
`WINDOWS_SECURITY.md`, which never lists the release it ships in. Release notes
`RELEASE_NOTES_0.3.0.md`, Discord text `RELEASE_DISCORD_0.3.0.md`.

| File | SHA256 |
|---|---|
| `PEMF-0.3.0.zip` | `0097492AA6C6E7E277BAA06BC254A756D61BA1B320B150556C9493DB55E1C67F` |
| `pemf_core.dll` | `BC3CD55365913A1B1AA183931A6D9C2BED82C4D3173F9A23C25C8C0A97628DBB` |
| `version.dll` | `D5498ED9B373EFDB9E9A752F42058A04B82B26CFDC27C0433EDEC03CAF28E3B6` |

**Open.** Next system to map: storms and weather.
