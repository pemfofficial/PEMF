# PEMF 0.2.3 — Launch fix

**If you are on Steam and 0.2.1 or 0.2.2 sometimes refused to start, update to
this.** That was our bug, and this release fixes it.

---

## "Application corrupt." on Steam

Roughly every other launch, the Steam version would put up an **Application
corrupt.** box and refuse to start. Try again and it would boot perfectly
normally, which made it look random.

It was not random, and it was not your install.

The Steam executable is wrapped in DRM, and that wrapper checks its own code
before the game starts. PEMF's weather feature patches a handful of numbers in
that code — cloud size, storm height, that sort of thing — and it was doing so
about **four milliseconds after loading**, while the wrapper was still
checking. Whichever got there first won. Lose the race and the wrapper decides
the executable has been tampered with, which, from where it is standing, it
had.

PEMF now waits until the game is actually running — the first frame of the main
loop — before changing anything. By then the wrapper has finished and the game
is up. There was never a reason to write that early; weather does not need
anything before the first frame.

Two things worth saying plainly:

- **Nothing was ever wrong with your game files.** The wrapper checks the copy
  in memory, not on disk. Nothing was modified permanently, and there is
  nothing to repair or verify.
- **This arrived with weather in 0.2.1.** Before that, PEMF never wrote to the
  game's code at all, which is why 0.2.0 never did this.

**GOG was never affected** — there is no DRM wrapper to upset.

If you saw `hooks installed: 0/4` in your log alongside this, that was a
symptom rather than a second problem. The game had already stopped, so there
was nothing left for PEMF to attach to.

---

## A half-updated install now says so

Windows' own **"Extract All" does not put files where you point it** — it
creates a subfolder named after the zip and puts them in there. Extract a new
PEMF release "into" your game folder that way and you get:

```
Sid Meier's Pirates!\PEMF-0.2.3\version.dll     <- does nothing
```

while the **old** DLLs carry on running from the game folder. The download was
new, so it is entirely reasonable to assume the version running is new too —
and until now nothing told you otherwise. This cost at least one person a round
of bug reports against fixes that were never running on their machine.

PEMF ships two DLLs and they must match. When they do not, `pemf.log` now says
so on the third line:

```
*** MIXED INSTALL: version.dll is 0.2.2.0 but pemf_core.dll is 0.2.3.0 ***
*** Both files must come from the same zip, and both must sit NEXT TO
    Pirates!.exe -- not in a subfolder. ***
```

The safe way to install is to open the zip, select everything, and drag it into
the game folder. `INSTALL.txt` now spells this out.

---

## Installing

Extract into your Pirates! folder, next to `Pirates!.exe`, over the top of your
existing install — **not into a subfolder**, see above. Works on **GOG and
Steam**. Your saves, key bindings and edited `.ini` files are untouched.

To uninstall, remove `version.dll` and `pemf_core.dll`.

PEMF is **unsigned**, so Smart App Control will block it and Defender may flag
the download. See [`WINDOWS_SECURITY.md`](WINDOWS_SECURITY.md) — the short
version is that this is what every PC game mod has looked like for twenty years
and there is no certificate to be had.

## Checking it worked

The top of `pemf.log` now tells you three things before anything else: which
version is running, whether both DLLs agree, and which profile folder your key
bindings went to.

You should also see `storms: applied` appear **after** `safe point reached`.
That ordering is the fix.

## Tuning

| File | What it controls |
|---|---|
| `PEMF\storms.ini` | Everything about weather |
| `PEMF\suspicion.ini` | False colours, hunters, standing |

Both are commented, and both survive a reinstall.

## Reporting

Post `pemf.log` from your game folder with a sentence about what you were
doing. If the game will not start at all, `pemf_proxy.log` from the same folder
is written even when nothing else is.
