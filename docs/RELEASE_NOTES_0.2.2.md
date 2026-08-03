# PEMF 0.2.2 — Fixes

A fix release. No new mechanics — three things reported from play, one of which
had been quietly broken for every GOG owner since WASD shipped.

Everything in 0.2.1 is unchanged. Extract over your existing install; your
`.ini` edits and saves are untouched.

---

## The WASD keys never worked on GOG

If you own the game on **GOG** and the WASD layout did nothing — no keys
changed, and no `KeyMap.ini.pemf-backup` ever appeared next to your bindings —
this is why, and it was our fault rather than anything you did.

The game keeps your `KeyMap.ini`, your saves and your custom flags in a folder
under `My Documents\My Games\`. **That folder is named differently depending on
where you bought the game:**

| Build | Folder |
|---|---|
| Steam | `My Games\Sid Meier's Pirates!` |
| GOG | `My Games\Sid Meier's Pirates` |

The exclamation mark follows the install folder. PEMF only ever looked for the
Steam spelling, so on a GOG install it wrote the new bindings into a directory
that does not exist. Nothing changed, nothing was backed up, and nothing in the
log said which path it had even tried — which is what turned a one-line bug into
a mystery that got reported twice.

It went unnoticed here for an embarrassing reason worth admitting: the machine
PEMF is developed on has **both** folders, because it has run both builds. The
hardcoded name worked in every test.

PEMF now finds the folder whichever build you own, and **writes the one it chose
into `pemf.log`** — so if this ever misbehaves again, one line in the log says
where it looked.

Two things follow from the same fix:

- **Custom flags in your personal `Custom\` folder** are found on GOG now too.
  They were being missed for exactly the same reason.
- **PEMF will not replace a `KeyMap.ini` it could not back up first.** Losing
  your own bindings with no way to get them back is worth failing loudly for —
  the alternative to WASD is only the game's default layout.

Already on Steam and happy with your keys? Nothing changes. PEMF still installs
the layout **once** and then leaves the file alone forever.

---

## Notices ran off the parchment

A notice longer than the strip spilled past both ends of the torn-paper art
instead of wrapping onto a second line.

This was supposed to have been fixed in 0.2.1, and the fix had the same bug it
was fixing — which is why you saw it again. The wrap broke the line at the first
space **after** the 32-character budget, which leaves the word that went over
the limit sitting on the line that was already too long. *"She's coming about:
They don't believe you"* came out as a 38-character first line.

It now breaks at the last space that **fits**. A single word longer than the
strip still overhangs rather than being chopped in half, which is deliberate —
a broken word reads worse than a wide one.

---

## Storms no longer clear behind an event card

If an event fired during a storm, the weather changed to clear and sunny while
the consequence box was on screen, then went back to stormy once you dismissed
it.

The game keeps drawing the sea behind a dialog box, so PEMF's weather kept
running — but you could not move and time was not really passing, and a card
that stayed up long enough could age a storm to death behind it. A fresh system
then made up as soon as you clicked through.

The weather now **holds while a card is up**, and the paused seconds are handed
back afterwards, so a dialog neither ages a storm nor shortens the fair weather
between them.

---

## Installing

Extract into your Pirates! folder, next to `Pirates!.exe`, over the top of your
existing install. Works on **GOG and Steam**. Remove `version.dll` and
`pemf_core.dll` to uninstall.

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

## Reporting

Post `pemf.log` from your game folder with a sentence about what you were doing.
This release adds a `profile:` line near the top naming the folder PEMF chose
for your bindings and flags — worth including if anything there looks wrong.
