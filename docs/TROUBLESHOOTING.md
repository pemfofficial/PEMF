# PEMF does not seem to be running at all

The game starts, plays perfectly normally, and none of PEMF is there: no new
rows in the port menu, no meter when you fly someone else's flag, hotkeys dead.

That symptom has one useful property — it is nearly always the *install*, not
the mod. Work through this in order. The first two checks cost a minute between
them and they account for almost everything we have seen.

If you would rather have a script do it, there is one in the repository at
[`tools/check_install.ps1`](https://github.com/pemfofficial/PEMF/blob/main/tools/check_install.ps1).
It is not in the release archive, so download it and run it from wherever you
saved it:

```powershell
.\check_install.ps1 -GameDir "C:\path\to\Sid Meier's Pirates!"
```

It is read-only, it needs no administrator rights, and it prints a verdict
rather than a pile of data. Left to itself it will go looking for the game
folder, so run it from there and you can drop the argument. Send its output with
any bug report.

---

## Start here: the two log files

PEMF is two DLLs, and each writes its own log into the game folder — the one
with `Pirates!.exe` in it.

| File | Written by | When |
|---|---|---|
| `pemf_proxy.log` | `version.dll` | the moment Windows loads the mod into the game |
| `pemf.log` | `pemf_core.dll` | once PEMF itself is up |

Which of them exists tells you where the chain broke, and it is the single most
informative thing you can find out.

| `pemf_proxy.log` | `pemf.log` | What it means |
|---|---|---|
| ✅ | ✅ | Both halves loaded. Whatever is wrong is *inside* PEMF — read `pemf.log` from the top, it says what it detected and what it refused. |
| ✅ | ❌ | Windows loaded `version.dll` but `pemf_core.dll` was refused. The last line of `pemf_proxy.log` carries the error number. |
| ❌ | ❌ | **`version.dll` was never loaded at all.** The game did not read the file you installed. Everything below is about this case. |
| ❌ | ✅ | Not something we have seen. Please report it. |

⚠️ If neither file is there, **do not conclude that the mod is broken** — nothing
of ours has run yet. Windows has not reached our code.

Both files reappear on the next launch, so deleting them and starting the game
once is a clean way to be sure you are reading today's run rather than last
week's.

---

## 1. Are the files where the game will look?

`version.dll` and `pemf_core.dll` must sit **directly beside `Pirates!.exe`**.
Not in a subfolder. Not in `PEMF\`.

```
Sid Meier's Pirates!\
├── Pirates!.exe
├── version.dll        <- here
├── pemf_core.dll      <- and here
└── PEMF\
```

### The subfolder trap

Windows' own **Extract All** does not put files where you point it. It creates a
subfolder named after the zip and puts them in there, so you end up with

```
Sid Meier's Pirates!\PEMF-0.2.6\version.dll     <- does nothing
```

This is the most common install mistake by a wide margin. It is also quiet: on
an *upgrade* it is worse than doing nothing at all, because the old DLLs are
still sitting in the game folder, still loading, and the game keeps running the
version you thought you had replaced.

Open the zip, select everything inside it, and drag it into the game folder —
or extract wherever you like and move `version.dll`, `pemf_core.dll` and the
`PEMF` folder into place yourself.

---

## Linux, Proton and the Steam Deck

PEMF is built and tested on Windows. It is not tested on Linux, and the two
things below were found by reading a tester's report rather than by reproducing
them on a Linux machine, so treat this section as the best account we have
rather than a verified recipe. If you get PEMF running under Proton, we would
like to hear exactly what you did.

Two separate problems bite in sequence, which is why fixing the first one alone
still leaves you with nothing in the log.

### The zip extracted as filenames instead of folders

**Affects 0.2.6 and every release before it. Fixed in 0.3.0.**

Those builds were packaged by a Windows tool that wrote the paths inside the zip
with backslashes. The zip format says forward slashes, but Windows accepts
either, so this was invisible for six releases. On Linux a backslash is an
ordinary character in a filename, so instead of a `PEMF` folder you get a
flat pile of files in the game directory literally named

```
PEMF\events\core_events.json
PEMF\officers\roster.json
PEMF\crew.ini
```

That is one file whose name contains backslashes — not a folder. If you are on
an affected release, either re-extract with `unzip -j` and rebuild the tree by
hand, or run this in the game folder to turn the names back into directories:

```bash
for f in 'PEMF\'*; do
    [ -e "$f" ] || continue          # nothing matched; nothing to repair
    target="${f//'\'//}"
    mkdir -p "$(dirname "$target")"
    mv -- "$f" "$target"
done
```

Run it from the game folder. The quoting around the lone backslash is load
bearing: the more obvious `${f//\\//}` looks right, silently matches nothing,
and leaves you exactly where you started.

`version.dll` and `pemf_core.dll` are unaffected — they sit at the top level of
the zip and have no separator in their names.

### Wine ignores our `version.dll` unless you tell it not to

This is the one that leaves you with no log at all, and it is not a PEMF fault
so much as how Wine works. `version.dll` is a DLL that Wine provides itself.
Wine prefers its own builtin over a file sitting in the game folder, so our
proxy is never loaded, nothing of ours ever runs, and neither `pemf_proxy.log`
nor `pemf.log` is ever created. From the outside it is indistinguishable from
having installed nothing.

Tell Wine to prefer the local file. In Steam, right-click the game →
**Properties** → **Launch Options**:

```
WINEDLLOVERRIDES="version=n,b" %command%
```

`n,b` means *native first, builtin second* — load ours, and fall back to Wine's
own if ours is missing. This is the same override ReShade and the various script
extenders need, for the same reason.

Then start the game, quit, and look in the game folder for `pemf_proxy.log`. If
that file now exists, the proxy is loading and any remaining problem is a later
one. If it still does not exist, the override has not taken effect — check that
you kept `%command%` on the end, which Steam needs in order to append its own
launch command.

**Where the logs are:** PEMF writes them beside `Pirates!.exe`, so they are in
the real Linux filesystem under `steamapps/common/Sid Meier's Pirates!/`, not
buried in the Proton prefix. The save sidecars do live in the prefix, under
`steamapps/compatdata/<appid>/pfx/drive_c/users/steamuser/Documents/My Games/`.

`check_install.ps1` is Windows PowerShell and will not help you here.

---

## 2. Is the game you launch the game you modded?

This is the one that wastes afternoons, and it is worth checking even when you
are certain.

If there is more than one copy of the game on the machine — a second Steam
library on another drive, a GOG install as well as a Steam one, a folder left
behind by an older install — then PEMF can go into one copy while the Play
button starts the other. Every symptom is silence, and everything you look at
appears correct, because you are looking at the wrong folder.

Two ways to settle it:

- **In Steam:** right-click the game → **Manage** → **Browse local files**.
  Steam opens the folder it actually runs. That is the folder the DLLs go in.
- **While the game is running**, in PowerShell:

  ```powershell
  Get-Process | Where-Object ProcessName -eq 'Pirates!' | Select-Object Path
  ```

  That is the executable Windows started, with no ambiguity about it.

A related version of the same mistake: reinstalling or *verifying* the game
through Steam can put a fresh copy somewhere you did not expect, and it will
also delete files it does not recognise. If you reinstalled the game partway
through troubleshooting, install PEMF again afterwards and check the path.

---

## 3. The game loaded Windows' `version.dll` instead of ours

If you have looked with Process Explorer or Process Monitor and found the game
running with `C:\Windows\SYSTEM32\VERSION.dll` loaded while your `version.dll`
sits in the game folder untouched, this section is yours. `check_install.ps1`
reports the same thing under **version.dll loaded** while the game is running.

It is worth understanding *why* that happens, because it looks like Windows
ignoring the game folder and it is not.

The loader does search the executable's own folder first — that part works, and
it is why the whole approach works at all. But if it cannot **open** what it
finds there, it does not stop and it does not complain. It carries on down the
search order and loads Windows' own copy out of System32. The game then starts
perfectly, with no PEMF and no log of any kind. A failure that ought to be loud
is completely silent, and from the outside it is identical to never having
installed the mod.

So the question is not "did the loader look in the right folder" but "could it
open the file when it got there". Two things stop it:

- **Permissions on the folder.** This is the likely one. Permissions are
  inherited from the directory, so every file in it is affected at once and the
  DLL itself looks perfectly normal. `check_install.ps1` tests this directly and
  reports `CANNOT BE OPENED FOR READING`.
- **Something holding the file open exclusively** — an antivirus mid-scan, or a
  backup or sync client. Rarer, and usually intermittent rather than every
  launch.

### Why it follows the folder rather than the file

This is the confusing part, and the reason it gets misdiagnosed as a drive
problem or a Steam library problem. Because permissions are inherited from the
directory:

- copying the game somewhere else **fixes it**, because the copy inherits the
  new folder's permissions
- moving it to another Steam library **fixes it**, for exactly the same reason
- the file is byte-for-byte identical in both places, and its hash matches

Every one of those points away from permissions and toward the drive, the
library, or the path — which is where the investigation usually goes and stays.
A folder carried across from an older Windows install, restored from a backup,
or created by a different user account is the usual origin.

### Settling it

Fastest test, and it needs nothing installed: copy the whole game folder to
somewhere plainly yours, such as your desktop, and run `Pirates!.exe` from there
directly. If PEMF loads, it is permissions.

To repair in place, from an **administrator** PowerShell prompt:

```powershell
icacls "C:\path\to\Sid Meier's Pirates!" /reset /t
```

That resets the folder and everything in it to inherit from its parent. It
changes nothing about the game's files themselves.

If the folder turns out to be readable and the game is *still* loading
System32's copy, tell us — that is a case we have not seen, and it would mean
something in the process loaded `version.dll` before the game did. Overlays and
injectors are the candidates there: RivaTuner, MSI Afterburner, Discord's
overlay, recording software.

---

## 4. Clear the downloaded-from-the-internet tag

Files that come out of a downloaded zip carry a hidden marker saying where they
came from. On a normal Windows machine this does **not** stop a DLL loading, so
it is not usually the answer — but it costs nothing and it removes a variable:

```powershell
Unblock-File -LiteralPath .\version.dll, .\pemf_core.dll
```

Run that in the game folder. It is not an admin command and it changes nothing
else about the files.

---

## 5. Did Windows say something?

Two parts of Windows refuse unsigned code, and **both of them are loud**. If you
saw no message at all, neither of these is what happened to you.

- **Smart App Control** stops the game starting outright, with
  `Bad Image ... 0xC0E90002`.
- **Defender** refuses the *download*, as `Trojan:Win32/Wacatac.B!ml`. If it
  quarantined `pemf_core.dll` after you extracted it, the file is simply gone
  from the folder — which check 1 will show you.

Both are covered properly, with what we have already ruled out, in
[`WINDOWS_SECURITY.md`](WINDOWS_SECURITY.md).

---

## 6. Are both DLLs from the same zip?

They have to be. `pemf.log` says so in capitals on its third line if they are
not:

```
*** MIXED INSTALL: version.dll is 0.2.4 but pemf_core.dll is 0.2.6 ***
```

Almost always that is the subfolder trap from check 1: the new zip went into
`PEMF-0.2.6\`, one old DLL got overwritten and the other did not. Delete both
DLLs and extract one zip cleanly.

---

## Two things that are not the cause

Recorded here because both have been proposed in good faith, and chasing them
costs real time.

### Windows Prefetch

There is a persistent idea that `C:\Windows\Prefetch` caches a "DLL map" for an
executable, and that a stale entry can make Windows ignore a DLL you have since
placed next to the game.

**It cannot.** Prefetch is a disk optimisation and nothing else: while a program
starts, Windows records which pages of which files got touched, and on the next
launch it reads those pages in early so the program starts faster. The record is
of what happened; it is not consulted when Windows decides *which* file a name
like `version.dll` refers to. That decision is made fresh on every launch, by
the loader, in the documented search order — and the folder containing the
executable is searched before the Windows system folder, which is the entire
basis on which PEMF works.

So: **do not run scripts that purge Prefetch entries**, and be wary of any
troubleshooting advice that asks you to elevate in order to do it. It requires
administrator rights, it will not fix this, and nothing you gain is worth
running an elevated script you found attached to a mod.

### `version.dll` being a protected system DLL

Windows keeps a set of "known DLLs" that are always loaded from the system
folder, with the application's own folder skipped entirely. If `version.dll`
were on that list, no `version.dll` proxy could ever work — this one, or the
twenty years of mod loaders built the same way.

It is not on it. We checked the live list on Windows 11 rather than reasoning
about it: `version.dll` is absent from both the 64-bit and the 32-bit set.
`imagehlp.dll` *is* on the list and does mention `version.dll` inside itself,
which looks alarming until you look properly — it is a name it passes to
`LoadLibrary` at runtime, not something it imports, and runtime loads do not
extend the protected set.

---

## What to send us

Attach these to the report and we can usually answer without a second round:

- `pemf_proxy.log` and `pemf.log`, if they exist — and say plainly if they do
  not, because that is itself the finding
- the output of `check_install.ps1`
- which store the game came from (Steam or GOG), and the **full path** of the
  folder you installed into
- how you launch the game: the Steam Play button, a desktop shortcut, or the
  executable directly

That last one matters more than it sounds. If the game works when you start
`Pirates!.exe` yourself but not when you press Play, the fault is in how the
game is being launched rather than in where the files are, and that is a
different investigation entirely.
