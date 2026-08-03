**PEMF 0.2.2 — fixes.**

No new mechanics. Three things reported from play, one of which had been broken for every GOG owner since WASD shipped.

**Install** — extract over your existing install, next to `Pirates!.exe`. Your `.ini` edits and saves are untouched. **GOG and Steam both work.**

**If you're on GOG and the WASD keys never worked — that was us.**
The game keeps your `KeyMap.ini`, saves and custom flags in a folder under `My Documents\My Games\`, and **that folder is named differently depending on where you bought the game**: `Sid Meier's Pirates!` on Steam, `Sid Meier's Pirates` on GOG. PEMF only ever looked for the Steam spelling, so on GOG it wrote your new bindings into a folder that doesn't exist — no keys changed, no `.pemf-backup` appeared, and nothing in the log said where it had looked.

It survived every test here for a dumb reason worth owning: the dev machine has **both** folders, because it's run both builds.

PEMF now finds the right folder whichever build you own, and writes the one it picked into `pemf.log`. Same fix means **custom flags in your personal folder work on GOG now too**. And PEMF will no longer replace a `KeyMap.ini` it couldn't back up first — losing your own bindings with nothing to restore from is worth failing loudly for.

Already on Steam and happy with your keys? Nothing changes.

**Notices ran off the parchment.**
Long lines spilled past both ends of the torn-paper strip. This was meant to be fixed in 0.2.1 and the fix had the same bug it was fixing, which is why you saw it twice — it broke the line at the first space *after* the budget, leaving the word that blew the limit on the line that was already too long. It now breaks at the last space that fits.

**Storms cleared behind event cards.**
Fire an event in a storm and the sky went clear and sunny behind the consequence box, then stormy again when you dismissed it. The game keeps drawing the sea behind a dialog, so PEMF's weather kept running while you couldn't move — long enough for a card to age a storm to death. The weather now holds while a card is up, and the paused seconds are given back after.

**Bugs:** post `pemf.log` from your game folder with a sentence about what you were doing. This build adds a `profile:` line near the top naming the folder it chose for your bindings — worth including if anything there looks off.

Thanks to everyone reporting. Every one of these came from someone playing it and saying so.
