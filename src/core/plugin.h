// plugin.h - loading other people's code.
//
// PEMF is a framework, and until this file existed that was only half true: the
// JSON engine let anyone author content, but writing a mod in CODE meant
// forking PEMF and rebuilding the core, which is not third-party modding and
// does not let two mods coexist in one install.
//
// A plugin is a 32-bit DLL in `PEMF\plugins\` exporting `PemfPluginInit`. PEMF
// hands it a table of function pointers -- `PemfApi` in sdk/pemf_sdk.h -- and
// that is the entire contract.
//
// -------------------------------------------------------------- why a C ABI
// Not a C++ interface, deliberately. The boundary carries only plain structs,
// integers and `const char*`, so a plugin built with a different compiler, a
// different standard library, or a different set of warning flags still works,
// and a PEMF rebuild does not silently break every plugin in the wild. The
// struct carries its own `size` and an `abi_version` so it can grow without
// invalidating what already exists: new calls go on the END, and a plugin
// checks `size` before reaching for anything newer than it was built against.
//
// ---------------------------------------------------------------- containment
// Someone else's code runs inside the game's process, so:
//
//   * every call into a plugin is wrapped in a fault guard, and a plugin that
//     faults is disabled for the session with its name in the log rather than
//     taking the game down;
//   * everything a plugin can do to game state goes through the same validated
//     layer PEMF uses, so it is clamped, career-gated and logged with the
//     plugin's own reason string;
//   * a 64-bit or otherwise unloadable DLL is reported and skipped, because
//     "the mod I installed does nothing and there is no message" is the worst
//     possible outcome for an author.
#pragma once
#include <windows.h>
#include <string.h>

#include "log.h"
#include "game.h"
#include "state.h"
#include "nations.h"
#include "content.h"
#include "events.h"
#include "crewmorale.h"
#include "townmenu.h"
#include "../../sdk/pemf_sdk.h"

namespace plugin {

constexpr int kMaxPlugins = 16;

struct Loaded {
    HMODULE     module = nullptr;
    char        name[64] = {0};
    char        version[32] = {0};
    char        file[64] = {0};
    bool        faulted = false;
};

inline Loaded g_plugins[kMaxPlugins];
inline int    g_count = 0;

// Which plugin are we currently inside? Used only so a fault, or a log line,
// can be attributed to the right one.
inline int g_current = -1;

inline const char* CurrentName()
{
    if (g_current < 0 || g_current >= g_count) return "plugin";
    return g_plugins[g_current].name;
}

// ------------------------------------------------------------- the API calls
// Thin, boring, and all of them go through PEMF's own layers rather than
// touching the game directly. That is the point: a plugin gets the same
// clamping and logging PEMF holds itself to.

inline void Api_Log(const char* message)
{
    if (message) Log("[%s] %s", CurrentName(), message);
}

inline int Api_InGame()      { return state::InGame() ? 1 : 0; }
inline int Api_GetCrew()     { return state::Crew(); }
inline int Api_GetMorale()   { return state::Morale(); }
inline int Api_GetPlunder()  { return state::Plunder(); }
inline int Api_GetMonths()   { return state::Months(); }
inline int Api_GetNation()   { return nations::HomeNation(); }

inline int Api_AddCrew(int d, const char* why)
{
    state::AddCrew(d, why && *why ? why : CurrentName());
    return state::Crew();
}
inline int Api_AddPlunder(int d, const char* why)
{
    state::AddPlunder(d, why && *why ? why : CurrentName());
    return state::Plunder();
}
inline int Api_SetCrew(int v, const char* why)
{
    state::SetCrew(v, why && *why ? why : CurrentName());
    return state::Crew();
}
inline int Api_SetPlunder(int v, const char* why)
{
    state::SetPlunder(v, why && *why ? why : CurrentName());
    return state::Plunder();
}

inline int Api_NearestCity(int radius)
{
    if (radius <= 0) radius = content::kCityNameScanRadius;
    return game::NearestCity(radius);
}
inline int Api_CityNation(int city) { return game::CityNation(city); }

// Village / town / city. NOT the same field as city_nation, which carries the
// owner and the two ownerless kinds; this is the size @LOCTYPE renders.
inline int Api_CityType(int city) { return game::CityLocType(city); }

inline int Api_NationsAtWar(int a, int b)
{
    return nations::AtWar(a, b) ? 1 : 0;
}

inline int Api_ShowCard(const char* body, const char* const* options, int count)
{
    if (!body) return -1;
    if (count < 0) count = 0;
    if (count > content::kMaxOptions) count = content::kMaxOptions;
    if (count == 0) {
        game::ShowModalTextN(body, nullptr, 0);
        return -1;
    }
    return game::AskChoiceN(body, options, count, nullptr, 0);
}

inline void Api_PostNotice(const char* text, int seconds, int anchorToShip)
{
    if (!text || !*text) return;
    if (seconds < 1)  seconds = 1;
    if (seconds > 30) seconds = 30;
    content::PostDebugNotice(text, seconds, anchorToShip != 0,
                             content::kChannelStatus);
}

// A plugin's menu row. The callback belongs to someone else, so it is invoked
// through the same guard as everything else they give us.
struct RowBinding {
    PemfRowFn fn = nullptr;
    void*     user = nullptr;
    int       owner = -1;
};

inline RowBinding g_bindings[townmenu::kMaxRows];
inline int        g_bindingCount = 0;

inline void InvokeBinding(int index);   // defined below, needs the guard

inline int Api_AddMenuRow(const char* label, int port, int nation,
                          PemfRowFn fn, void* user)
{
    if (!label || !*label || !fn) return 0;
    if (g_bindingCount >= townmenu::kMaxRows) {
        Log("[%s] no room for another menu row ('%s')", CurrentName(), label);
        return 0;
    }

    RowBinding& b = g_bindings[g_bindingCount];
    b.fn    = fn;
    b.user  = user;
    b.owner = g_current;

    // townmenu takes a plain void(*)(int); the int is the binding index, which
    // is how a C callback with user data is carried across a C ABI that has no
    // closures.
    const bool ok = townmenu::Add(label, -1, -1,
                                  [](int i) { InvokeBinding(i); },
                                  g_bindingCount, port, nation);
    if (!ok) return 0;

    ++g_bindingCount;
    Log("[%s] menu row '%s' (port %d, nation %d)",
        CurrentName(), label, port, nation);
    return 1;
}

inline int Api_FireEvent(const char* eventId)
{
    if (!eventId || !*eventId) return 0;
    const int idx = content::FindByIdIndex(eventId);
    if (idx < 0) {
        Log("[%s] no event with id '%s'", CurrentName(), eventId);
        return 0;
    }
    content::Fire(idx);
    content::ShowPendingOutcome(0);
    events::ClearFollowUp();
    return 1;
}

inline int Api_GetMood() { return crewmorale::g_value; }

inline const char* Api_MoodName() { return crewmorale::Name(); }

inline void Api_NudgeMood(int delta, const char* reason)
{
    // Bounded per call. A plugin that wants to swing the crew from mutinous to
    // devoted in one go is not expressing an event, and the log would give no
    // hint where it came from. It can call again if it means it.
    if (delta >  25) delta =  25;
    if (delta < -25) delta = -25;
    char why[96];
    _snprintf_s(why, sizeof(why), _TRUNCATE, "%s: %s", CurrentName(),
                (reason && *reason) ? reason : "no reason given");
    crewmorale::Nudge(delta, why);
}

// The one table, built once.
inline PemfApi g_api = {
    sizeof(PemfApi), PEMF_ABI_VERSION,
    &Api_Log,
    &Api_InGame, &Api_GetCrew, &Api_GetMorale, &Api_GetPlunder,
    &Api_GetMonths, &Api_GetNation,
    &Api_AddCrew, &Api_AddPlunder, &Api_SetCrew, &Api_SetPlunder,
    &Api_NearestCity, &Api_CityNation, &Api_CityType, &Api_NationsAtWar,
    &Api_ShowCard, &Api_PostNotice,
    &Api_AddMenuRow, &Api_FireEvent,
    &Api_GetMood, &Api_MoodName, &Api_NudgeMood,
};

// ------------------------------------------------------------------ the guard
inline void Disable(int which, const char* what, DWORD code)
{
    if (which < 0 || which >= g_count) return;
    if (g_plugins[which].faulted) return;
    g_plugins[which].faulted = true;
    Log("!! plugin '%s' FAULTED in %s (0x%08X) -- disabled for this session",
        g_plugins[which].name, what, code);
    Log("!! the game is unaffected; report this to that plugin's author.");
}

inline void InvokeBinding(int index)
{
    if (index < 0 || index >= g_bindingCount) return;
    RowBinding& b = g_bindings[index];
    if (!b.fn) return;
    if (b.owner >= 0 && b.owner < g_count && g_plugins[b.owner].faulted) return;

    const int prev = g_current;
    g_current = b.owner;
    __try {
        b.fn(b.user);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Disable(b.owner, "a menu row", GetExceptionCode());
    }
    g_current = prev;
}

// ------------------------------------------------------------------- loading
inline bool LoadOne(const char* dir, const char* fileName)
{
    if (g_count >= kMaxPlugins) {
        Log("plugins: more than %d, skipping '%s'", kMaxPlugins, fileName);
        return false;
    }

    char full[MAX_PATH];
    _snprintf_s(full, sizeof(full), _TRUNCATE, "%s\\%s", dir, fileName);

    HMODULE mod = LoadLibraryA(full);
    if (!mod) {
        const DWORD e = GetLastError();
        // 193 is ERROR_BAD_EXE_FORMAT, which here means a 64-bit build. Worth
        // saying plainly -- it is the single most likely first mistake.
        Log("plugins: could NOT load '%s' (error %lu)%s", fileName, e,
            e == 193 ? " -- that looks like a 64-bit DLL; the game is 32-bit"
                     : "");
        return false;
    }

    auto init = (PemfPluginInitFn)GetProcAddress(mod, "PemfPluginInit");
    if (!init) {
        Log("plugins: '%s' has no PemfPluginInit -- not a PEMF plugin, skipped",
            fileName);
        FreeLibrary(mod);
        return false;
    }

    const int slot = g_count;
    Loaded& p = g_plugins[slot];
    p.module = mod;
    p.faulted = false;
    strncpy_s(p.file, sizeof(p.file), fileName, _TRUNCATE);
    strncpy_s(p.name, sizeof(p.name), fileName, _TRUNCATE);   // until it says
    p.version[0] = 0;

    PemfPlugin me = {};
    ++g_count;                 // visible to the API during init
    g_current = slot;

    int rc = PEMF_ERR_FAILED;
    __try {
        rc = init(&g_api, &me);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Disable(slot, "PemfPluginInit", GetExceptionCode());
        g_current = -1;
        --g_count;
        FreeLibrary(mod);
        return false;
    }
    g_current = -1;

    if (rc != PEMF_OK) {
        Log("plugins: '%s' declined to start (returned %d)%s", fileName, rc,
            rc == PEMF_ERR_ABI ? " -- it wants a different PEMF ABI" : "");
        --g_count;
        FreeLibrary(mod);
        return false;
    }

    if (me.name && *me.name)
        strncpy_s(p.name, sizeof(p.name), me.name, _TRUNCATE);
    if (me.version && *me.version)
        strncpy_s(p.version, sizeof(p.version), me.version, _TRUNCATE);

    Log("plugins: loaded '%s' %s  (%s)", p.name,
        p.version[0] ? p.version : "", fileName);
    return true;
}

// Called after content is loaded, so a plugin can reference events by id and
// so its rows sit alongside the authored ones rather than being wiped by
// townmenu::LoadFromContent, which clears the row list.
inline int LoadAll(const char* pemfDir)
{
    char dir[MAX_PATH];
    _snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\plugins", pemfDir);

    char pattern[MAX_PATH];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*.dll", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        // Not a problem, and not worth a scary line: most installs have none.
        Log("plugins: none found in %s", dir);
        return 0;
    }

    Log("plugins: scanning %s", dir);
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        LoadOne(dir, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    Log("plugins: %d loaded", g_count);
    return g_count;
}

} // namespace plugin
