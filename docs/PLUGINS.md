# Writing a PEMF plugin

Two ways to mod with PEMF. **Most mods should be JSON** — see
[`EVENT_AUTHORING.md`](EVENT_AUTHORING.md), which needs no compiler and covers
events, notices, triggers, town-menu rows and whole menu trees.

This page is the other way: **a plugin, written in code**, for when your mod
needs to *decide* something rather than *declare* it.

---

## What a plugin is

A 32-bit Windows DLL in `PEMF\plugins\` that exports one function. PEMF finds
it at startup and hands it a table of function pointers. That is the entire
contract — there is nothing to link against and no PEMF library to build.

Everything you need is one header: **`PEMF\sdk\pemf_sdk.h`**, which ships with
the mod. A complete worked example sits beside it.

---

## The whole thing

```c
#include "pemf_sdk.h"

static const PemfApi* g_pemf;

static void OnPicked(void* user)
{
    g_pemf->show_card("The harbourmaster has nothing for you today.", 0, 0);
}

PEMF_PLUGIN_EXPORT int PemfPluginInit(const PemfApi* api, PemfPlugin* me)
{
    if (api->abi_version != PEMF_ABI_VERSION) return PEMF_ERR_ABI;
    g_pemf = api;

    me->name    = "My Mod";
    me->version = "1.0";

    api->add_menu_row("Ask at the harbour", PEMF_ANY, PEMF_ANY, OnPicked, 0);
    return PEMF_OK;
}
```

Build it, drop the DLL in `PEMF\plugins\`, start the game.

```
cl /nologo /LD /O2 my_mod.c /I<path to sdk> /Fe:my_mod.dll
```

`pemf.log` will say:

```
plugins: scanning <game>\PEMF\plugins
plugins: loaded 'My Mod' 1.0  (my_mod.dll)
```

---

## What a plugin can do

| Area | Calls |
|---|---|
| Logging | `log` |
| Read state | `in_game`, `get_crew`, `get_morale`, `get_plunder`, `get_months`, `get_nation` |
| Change state | `add_crew`, `add_plunder`, `set_crew`, `set_plunder` |
| The world | `nearest_city`, `city_nation`, `city_type`, `nations_at_war` |
| Show something | `show_card`, `post_notice` |
| Town menu | `add_menu_row` |
| Content | `fire_event` |

Each is documented where it is declared, in `pemf_sdk.h`. Read that file — it is
the reference, and it is written to be read.

---

## Rules that matter

**32-bit.** The game is x86. A 64-bit DLL is refused with a line naming the
likely cause rather than failing mysteriously.

**One thread.** PEMF calls you on the game's own thread, at points where the
game is in a known state. Do not call back into PEMF from a thread of your own —
the engine has no locking.

**UI only from a callback.** `show_card` presents a modal. That is safe from
inside a callback PEMF invoked, and not from `PemfPluginInit`, where the game is
not running yet.

**State goes through PEMF.** `add_plunder` and friends are clamped to what the
game can hold, refused outside a career, and logged with the reason you pass:

```
  state: plunder 600 -> 300 [tide_charts]
```

That line is why the reason argument exists. Write the game's memory yourself
and nothing above is true any more.

**A fault is contained.** PEMF calls you inside a guard. A plugin that faults is
disabled for the session, with its name in the log, and the game carries on:

```
!! plugin 'My Mod' FAULTED in a menu row (0xC0000005) -- disabled for this session
!! the game is unaffected; report this to that plugin's author.
```

---

## Versions

`PEMF_ABI_VERSION` changes only when something already in the header changes
meaning. New calls are appended to the **end** of `PemfApi`, and the struct
carries its own `size`, so a plugin built against an older SDK keeps working.

If you use a call added after the version you targeted, check first:

```c
if (api->size >= offsetof(PemfApi, the_new_call) + sizeof(void*)) { ... }
```

Refuse rather than guess if the ABI is not one you know:

```c
if (api->abi_version != PEMF_ABI_VERSION) return PEMF_ERR_ABI;
```

PEMF will log that you declined and unload you cleanly.

---

## Limits

Honest about the current state:

- **Six menu rows total**, shared between JSON rows and every plugin. A refusal
  is logged rather than silent.
- **Sixteen plugins.**
- **Plugins do not persist anything.** PEMF's own state travels in a sidecar
  next to each save; a plugin that wants to remember something across saves must
  do it itself for now.
- **No hooks yet** — a plugin cannot yet ask to be called each frame, or when a
  battle ends, or when a port is entered. Menu rows are the only entry point.
  This is the obvious next thing to add, and the ABI is built to grow.
