**PEMF / False Flag — FAQ**

**Does it work with my version?**
GOG and Steam, both. The **Challenge Pack** executable is a different build and isn't supported — PEMF detects it and switches itself off rather than risk your save.

**Will it break my saves?**
It doesn't touch them. PEMF keeps its own state in a small `.pemf` file beside each save, so it travels with that save and save-scumming works normally. Delete the mod and your saves are untouched.

**Does it modify the game?**
No. `Pirates!.exe` is never written to. PEMF loads alongside it and hooks the game's imports in memory.

**I get "Bad Image… 0xC0E90002" and nothing loads.**
That's Windows **Smart App Control** refusing an unsigned DLL. This build isn't code-signed yet. You'll need to turn SAC off, or wait for a signed release.

**Why do I start with an English flag?**
Unfinished feature, not a bug. The flag you begin with isn't wired to your chosen faction yet. It's next on the list.

**Suspicion fills way too fast.**
On purpose, so you can see the whole arc in a minute. Open `PEMF\suspicion.ini` and lower the three `rise` values — try a third of the defaults. It's commented, and reinstalling won't overwrite your edits.

**Can I add my own flags?**
Yes — and you always could. Drop `flag_*.dds` files in `custom\`. The game has read that folder since 2004; the old claim that you must *replace* flags was never true for your own. PEMF just lets you change them at sea.

**How do I get my old key bindings back?**
`My Documents\My Games\Sid Meier's Pirates!\KeyMap.ini.pemf-backup` — rename it over `KeyMap.ini`.

**Can I write my own events?**
Yes, in JSON, no compiling. See `PEMF\docs\EVENT_AUTHORING.md`. A broken file is skipped with a reason in the log rather than taking the rest down.

**Something's wrong / it crashed.**
Post `pemf.log` from your game folder with a sentence about what you were doing. For a detailed repro, drop an empty file named `dev.on` into `PEMF\` to turn on the developer probes.
