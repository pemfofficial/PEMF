**PEMF 0.2.3 — launch fix. If you're on Steam, update.**

If 0.2.1 or 0.2.2 sometimes refused to start with an **"Application corrupt."** box, that was us. Fixed.

**What was happening**
Roughly every other launch the Steam version would throw that error and not start; try again and it booted fine, so it looked random. It wasn't.

The Steam exe is wrapped in DRM, and the wrapper checks its own code before the game starts. PEMF's weather patches a few numbers in that code — cloud size, storm height — and it was doing it about **four milliseconds after loading**, while the wrapper was still checking. Whoever got there first won. Lose the race and the wrapper concludes the exe has been tampered with, which from where it's standing, it had.

PEMF now waits until the game is actually running before it changes anything. There was never a reason to write that early — weather doesn't need anything before the first frame.

**Nothing was ever wrong with your game files.** The wrapper checks the copy in memory, not on disk. Nothing to repair, nothing to verify.

This came in with weather in 0.2.1 — before that PEMF never wrote to the game's code at all, which is why 0.2.0 never did it. **GOG was never affected**, there's no wrapper to upset.

If you also saw `hooks installed: 0/4`, that was a symptom, not a second bug — the game had already stopped, so there was nothing left to attach to.

**Also: a half-updated install now tells you**
Windows' "Extract All" doesn't put files where you point it — it makes a subfolder named after the zip. Extract a release "into" your game folder that way and the **old** DLLs keep running while you're certain you're on the new ones. Nothing told you otherwise, and it cost at least one person a round of bug reports against fixes that were never running.

`pemf.log` now says `*** MIXED INSTALL ***` when the two DLLs don't match. Safest install: open the zip, select all, drag into the game folder.

**Install** — extract next to `Pirates!.exe`, over the top of what you have. Saves, key bindings and your `.ini` edits are untouched. **GOG and Steam both work.**

**Bugs:** post `pemf.log` with a sentence about what you were doing. If the game won't start at all, `pemf_proxy.log` from the same folder is written even when nothing else is.
