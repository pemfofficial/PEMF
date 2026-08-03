# PEMF 0.2.1 — Weather

Storms you can see coming.

Sid Meier's Pirates! has always had weather, drawn small enough that most
captains never looked twice at it. This release makes it something you sail
around: large storm systems that make up to windward, drift across the map with
the trade wind, rain on you, take cargo over the side, and bring their own music
with them.

Everything here is **tunable in `PEMF\storms.ini`**, which is commented, and a
reinstall will never overwrite your edits.

---

## What is new

### Storm systems

A storm is no longer one small cloud sitting in the corner of the screen. It is
a **system** — a cluster of clouds drawn as one mass — that:

- **makes up to windward, off screen**, and comes over the horizon rather than
  appearing
- **drifts west** with the trade wind at the rate the game's own storms use
- **passes over you and recedes**, then a rest, then another makes up

Size, spread, height, cloud count and drift are all settings. Shrinking a storm
shrinks its whole footprint rather than scattering the same clouds more thinly.

### Weather that costs you something

Storms already damage your hull through the game's own lightning. Now a system
will also take a little cargo over the side while you are in it.

**Gold and cannon are protected.** Losing plunder reads as theft rather than
weather, and losing cannon is not cargo attrition — it is disarming your ship
mid-voyage. Both are off the table by default, and the protection is a bitmask
if you disagree.

How bad the weather is comes from **the game's own weather curve**, so the
cargo, the rain and the music all agree with the clouds actually on screen.

### Storm music

Drop a track in `PEMF\audio\` and it fades in as you sail into bad weather and
fades out as you leave. The ship's own theme ducks away while it plays and
returns afterwards, at whatever volume you had it set to.

Any format Windows can decode — mp3, wav, wma, m4a.

### Rain that falls where it should

The game draws a screen-wide rain effect that covers clear sky and paints over
the top of the clouds. PEMF can switch it off, leaving only the rain the storm
cloud itself carries — which falls **under** the cloud, where it belongs.

---

## Fixes

- **Pirate hunters that stay on you.** A hunt is now aimed at a port *beyond*
  the player rather than near them, so a hunter sails at you the whole way
  instead of docking when she arrives. Measured holding 873–3,874 units for 55
  seconds.
- **Hunters no longer evaporate.** Ships built more than ~11,000 units from the
  player are culled by the overworld within seconds, so a crown whose nearest
  port was far away silently sent nobody. Dispatch now declines and says so, and
  retries every 15 seconds while you remain hostile.
- **Hunter strength was undersized.** The engine computes `2 - reputation/5`;
  PEMF read it as `/10`. Correct in the middle of the range now.
- **A hunt ends when the quarrel does.** Previously the standing ledger could
  forgive you and reopen a nation's ports while one of its warships was still
  out there hunting you.
- **Break-offs are announced.** A hunt ending in silence is indistinguishable
  from one still coming.

---

## Known limits

Written down because they are real, not because they are likely to change soon.

- **Storms cannot fade.** There is no opacity control on the cloud draw — scale
  is size, so ramping it inflates or shrinks rather than fading. Systems appear
  and disappear; the rest period between them is what makes that rare.
- **Wind and cloud can disagree.** PEMF draws its own storm while the engine
  keeps its own for wind purposes, so a strong breeze may not line up exactly
  with the weather you can see.
- **Denser rain is not available.** The one dial that scales rain changes drop
  *size* and wind together, not how much of it there is.
- **Weather is visual and cargo.** It does not yet affect crew, speed or
  navigation beyond what the game already did.

---

## Installing

Extract into your Pirates! folder, next to `Pirates!.exe`. Works on **GOG and
Steam**. Remove `version.dll` and `pemf_core.dll` to uninstall.

PEMF is **unsigned**, so Smart App Control will block it and Defender may flag
the download. See [`WINDOWS_SECURITY.md`](WINDOWS_SECURITY.md) — the short
version is that this is what every PC game mod has looked like for twenty years
and there is no certificate to be had.

## Tuning

| File | What it controls |
|---|---|
| `PEMF\storms.ini` | Everything about weather |
| `PEMF\suspicion.ini` | False colours, hunters, standing |

Both are commented, and both survive a reinstall.
