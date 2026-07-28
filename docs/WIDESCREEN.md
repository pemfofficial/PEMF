# Widescreen — why the first attempt broke, and the mapping that fixes it

The first attempt got 1920×1080 running (`b746275`) and was reverted in full
(`6f89612`) because the UI misbehaved: elements in the wrong place, the mouse
landing away from buttons, slider thumbs offset and hard to hit.

Every one of those symptoms has the same cause, and it is now measured rather than
inferred.

---

## The two virtual spaces

The game never lays anything out in real pixels. It has **two fixed virtual
coordinate spaces**, both 4:3, and both independent of the display resolution:

| Space | Size | Used by | Where |
|---|---|---|---|
| Draw | **640 × 480** | the quad blit, HUD text, everything PEMF draws | `0x0072637C` / `0x00726380` |
| Input | **1024 × 768** | the widget hit-test and widget interaction | constants `0x007135C8` / `0x007135C4` |

`0x0072637C` and `0x00726380` are **never written** — no store instruction anywhere
in the image targets them. They are constants: 640 and 480, always.

This is the single most useful fact here: **`game::ScreenW()` and `ScreenH()` already
return a virtual 640×480, not the real resolution.** Everything PEMF draws is already
resolution-independent. Nothing in the drawing layer needs to change to support a new
resolution — only the mapping from those spaces to the screen does.

`SetResolution` (`0x004D3AB0`) makes it explicit:

```c
DAT_0085a26c = DAT_0072637c;   // ScreenW <- UI width  (640)
DAT_0085a268 = DAT_00726380;   // ScreenH <- UI height (480)
```

The resolution arguments are used for the window and the frustum. They never reach
the coordinate space.

---

## Why it looks wrong at 16:9

`SetResolution` rebuilds the view frustum as

```
FUN_0054AA00(-Cx, +Cx, +Cy, -Cy, near, far)      // NiFrustum(l, r, t, b, n, f)
    Cx = C                  (constant, 0.5)
    Cy = (H / W) * C
```

`FUN_0054AA00` is `NiFrustum` — confirmed against the Gamebryo source, and against
the startup call `FUN_00503CA0` which passes exactly
`(-0.5, +0.5, +0.375, -0.375, 921.6, 1536.0)`. `0.375 / 0.5 = 0.75`, i.e. 4:3.

Content is placed in virtual pixels, so with `s` view-units per virtual pixel the UI
occupies `640s × 480s`. The game is **width-fit**: `640s = 2Cx`, giving `s = C/320`.
The height it then needs is `480s = 1.5C`, but the frustum only offers `2Cy = 2(H/W)C`.

- At 4:3 — `2(H/W)C = 1.5C`. Exactly fits. This is why the game only ever offered 4:3.
- At 16:9 — `2(H/W)C = 1.125C < 1.5C`. **The UI is a quarter taller than the frustum
  shows, so the top and bottom are cut off.**

That is the "overflowed off screen" symptom, and it is not a bug in the port — it is
the whole 4:3 assumption expressed in one inequality.

## The corrected frustum

Keep square pixels (`Cx/Cy = W/H`) and fit the **height** instead:

```
Cy = 0.75 * C
Cx = 0.75 * C * (W / H)
```

At 4:3 this reduces to `Cx = C`, `Cy = 0.75C` — the stock values, unchanged. At 16:9
it widens the horizontal extent by 4/3, so about 853 virtual pixels are visible
across, with the 640-wide UI centred and empty margins either side.

Bars are the honest outcome for 4:3 artwork on a 16:9 panel. The alternatives are
stretching it or cropping it.

---

## The mouse — and the mistake that cost the first attempt

The hit-test at `0x00504310` maps a cursor position into the input space:

```
  x:  (px / W - 0.5)      * 1024      ; fmul operand at 0x0050436D
  y:  (1.0 - py / H - 0.5) * 768      ; fmul operand at 0x005043C8
```

For the plane to still read ±512 at the edges of the centred 4:3 region, the X
multiplier must become **`768 × (W / H)`** — which at 4:3 is 1024, unchanged. The Y
multiplier stays 768. (This matches what the first attempt found empirically; it is
derived here independently, and the two agree.)

**The first attempt patched one operand out of eight.**

Searching both constants across the image finds four matched X/Y pairs in the UI:

| X site (1024) | Y site (768) | What |
|---|---|---|
| `0x0050436D` | `0x005043C8` | the hit-test — **the only one patched** |
| `0x00503985` | `0x005038ED` | a second screen→plane conversion |
| `0x0050C7EF` | `0x0050C841` | preceded by `fdivr`, followed by `fsub [esi+0x298]` — a widget's stored x. **Drag math.** |
| `0x0050CAFC` | `0x0050CB40` | preceded by `fdivr` — the same screen→plane shape again |

The earlier note recorded these three as "widget *positions*, not input", and left
them alone because patching them moved HUD elements. That reading was wrong for at
least two of them: `0x0050C7EF` and `0x0050CAFC` are each preceded by `fdivr`, the
divide-by-screen-size that only a **screen→plane** conversion performs. A layout
computation has nothing to divide by the backbuffer size.

So the cursor was being mapped into a corrected plane while sliders and widget
interaction still mapped into the raw one. **That is exactly the reported symptom:
the mouse missed buttons, and slider thumbs were offset and hard to find.** Two
coordinate systems disagreeing by a factor of `(W/H)/(4/3)` — 1.333 at 16:9, which
at the right-hand edge of a 1920-wide screen is a miss of hundreds of pixels.

**The rule: all eight operands move together, or none do.** They are the same
mapping, and the failure mode of a partial patch is silent misalignment rather than
an error.

Five further references to 1024 exist at `0x00446BD1`–`0x00446D5F`. They are a
different subsystem — each preceded by `fadd [const]` with no divide — and must
**not** be touched. Repointing the operands rather than editing the shared constant
is what keeps them out of it.

---

## Still open

- **3D geometry stays 4:3-proportioned.** The engine feeds shader-drawn geometry its
  projection through **vertex-shader constants**, not `SetTransform`, so correcting
  only the fixed-function path splits objects — ships tore in half. The device hook
  PEMF already owns can reach `SetVertexShaderConstantF` (vtable slot 94; PEMF's own
  confirmed BeginScene 41 / EndScene 42 / SetTransform 44 validate the numbering).
  The game does use programmable shaders — `vs_1_1`, `vs_2_0`, `ps_2_0` appear in the
  image, with a Gamebryo shader set in `Assets\Shaders\FX` (`HullShader`, `SailShader`,
  `WaterShader`).
- **Missing scenery at the sides in character creation.** Consistent with the above:
  3D models draw through the corrected path while their environment does not.
- **Toggling shaders off and on leaves the video options unclickable.** Not yet
  investigated, and probably unrelated to the mapping — a widget list rebuilt after a
  device reset is the first thing to look at. Worth confirming it reproduces on a
  *stock* install before attributing it to the mod.

## The engine's own letterbox

Gamebryo's `NiCamera` carries a viewport rect `m_kPort` (normalised, default
`0,1,1,0`), and **both** the projection and `WindowPointToRay` derive from it:

```
window pixel -> buffer point [0,1] -> m_kPort -> m_kViewFrustum -> view plane
```

So a camera-level port is the engine-native way to pillarbox, and it keeps drawing
and hit-testing consistent by construction. It is only usable if the game's UI
actually hit-tests through `WindowPointToRay` — and it does not; `0x00504310` is a
hand-rolled mapping that ignores the camera. Recorded because it is the right shape
for anything that *does* go through the camera, and because it is the reason the two
paths could diverge at all.

---

## Order of work

1. Frustum correction, alone. The UI should become fully visible and centred, with
   the mouse now wrong *everywhere* by a known constant factor.
2. All eight operands, together. Drawing and input line up.
3. Side-strip clear on UI-only frames.
4. 3D via the vertex-shader constant path — separate, and the largest piece.

Step 1 without step 2 is expected to look broken; that is not a reason to stop
between them. Do not ship a partial operand patch — it is the exact failure the first
attempt produced.
