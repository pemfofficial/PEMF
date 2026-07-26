// session.h - career lifecycle, save/load detection, and our own persistence.
//
// PROBLEM: once we keep simulation state of our own (officers, event history,
// crew opinions), that state must follow the player's saves exactly. If it does
// not, loading an old save leaves officers alive who died, events "already
// fired" that never happened, and references to people who do not exist.
//
// APPROACH: a sidecar file per save file. The game writes
//     <name>.pirates_savegame
// and we write
//     <name>.pemf
// alongside it. Our state then travels with the save, so save-scumming and
// multiple slots behave exactly as the player expects.
//
// DETECTION: no game reverse engineering needed. We IAT-hook CreateFileA and
// watch for the save extension -- opened for write means the game is saving,
// opened for read means it is loading.
//
// NEW CAREER: starting a career without loading produces an in-game transition
// with no preceding load. We watch for that and reset to empty state, so a new
// captain never inherits the last one's officers.
#pragma once
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "log.h"
#include "state.h"

namespace session {

constexpr const char* kSaveExt    = ".pirates_savegame";
constexpr const char* kSidecarExt = ".pemf";
constexpr int kStateVersion = 1;

// --------------------------------------------------------------- our state
// Everything here is OURS -- it lives in our memory and our sidecar, never in
// the game's data structures. Officers and crew opinion will grow into this.
struct ModState {
    int  version      = kStateVersion;
    int  eventsFired  = 0;      // lifetime count for this career
    int  lastEventTick = 0;     // pacing, so events do not cluster
    bool valid        = false;  // false = no career loaded yet

    void Clear()
    {
        version = kStateVersion;
        eventsFired = 0;
        lastEventTick = 0;
        valid = false;
    }
};

inline ModState g_state;
inline char     g_currentSave[MAX_PATH] = {0};   // last save file touched
inline bool     g_loadPending = false;           // a load happened, expect a career
inline bool     g_wasInGame   = false;

// Session state is written from the safe point (game thread) and read by the
// file hooks, which can run on ANY thread. With three ints the race is benign
// on x86, but the moment officers add strings and collections it becomes a real
// crash. Guard it now, before it can hurt.
inline CRITICAL_SECTION g_lock;
inline bool             g_lockReady = false;

inline void InitLock()
{
    if (g_lockReady) return;
    InitializeCriticalSection(&g_lock);
    g_lockReady = true;
}

struct Guard {
    Guard()  { if (g_lockReady) EnterCriticalSection(&g_lock); }
    ~Guard() { if (g_lockReady) LeaveCriticalSection(&g_lock); }
};

// What a file-hook observation turned out to be. The caller acts on this --
// notably, a load must also discard anything queued.
enum class FileEvent { None, Saved, Loaded };

// ------------------------------------------------------------------ helpers
inline bool EndsWithNoCase(const char* s, const char* suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && _stricmp(s + ls - lx, suffix) == 0;
}

// <name>.pirates_savegame -> <name>.pemf
inline void SidecarPathFor(const char* savePath, char* out, size_t cch)
{
    strncpy_s(out, cch, savePath, _TRUNCATE);
    size_t n = strlen(out), lx = strlen(kSaveExt);
    if (n >= lx) out[n - lx] = 0;
    strncat_s(out, cch, kSidecarExt, _TRUNCATE);
}

// ------------------------------------------------------------ persistence
// Plain key=value text: trivially debuggable, and a player can inspect or hand
// edit it. Binary would buy nothing here.
inline bool Save(const char* savePath)
{
    char path[MAX_PATH];
    SidecarPathFor(savePath, path, sizeof(path));

    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f) {
        Log("session: could NOT write sidecar %s", path);
        return false;
    }
    fprintf(f, "version=%d\n",       kStateVersion);
    fprintf(f, "eventsFired=%d\n",   g_state.eventsFired);
    fprintf(f, "lastEventTick=%d\n", g_state.lastEventTick);
    fclose(f);
    Log("session: wrote sidecar (eventsFired=%d) -> %s",
        g_state.eventsFired, path);
    return true;
}

inline bool Load(const char* savePath)
{
    char path[MAX_PATH];
    SidecarPathFor(savePath, path, sizeof(path));

    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) {
        // Perfectly normal: a save made before the mod, or by an older version.
        g_state.Clear();
        g_state.valid = true;
        Log("session: no sidecar for this save -- starting clean state");
        return false;
    }

    ModState loaded;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int v = 0;
        if (sscanf_s(line, "version=%d", &v) == 1)            loaded.version = v;
        else if (sscanf_s(line, "eventsFired=%d", &v) == 1)   loaded.eventsFired = v;
        else if (sscanf_s(line, "lastEventTick=%d", &v) == 1) loaded.lastEventTick = v;
    }
    fclose(f);

    if (loaded.version != kStateVersion) {
        // Refuse rather than misinterpret fields from another layout.
        Log("session: sidecar version %d != %d -- discarding, starting clean",
            loaded.version, kStateVersion);
        g_state.Clear();
        g_state.valid = true;
        return false;
    }

    loaded.valid = true;
    g_state = loaded;
    Log("session: loaded sidecar (eventsFired=%d) <- %s",
        g_state.eventsFired, path);
    return true;
}

// --------------------------------------------------------------- lifecycle
inline void BeginNewCareer()
{
    g_state.Clear();
    g_state.valid = true;
    g_currentSave[0] = 0;
    Log("session: NEW CAREER -- state reset");
}

inline void Invalidate(const char* reason)
{
    g_state.Clear();
    Log("session: state invalidated (%s)", reason);
}

// Called from the CreateFile hooks. Pure bookkeeping plus file I/O -- it never
// touches game memory, so it is safe from any thread (and is locked).
inline FileEvent OnSaveFileOpened(const char* path, bool forWrite)
{
    Guard g;
    strncpy_s(g_currentSave, sizeof(g_currentSave), path, _TRUNCATE);
    if (forWrite) {
        Log("session: SAVE detected -> %s", path);
        Save(path);
        return FileEvent::Saved;
    }
    Log("session: LOAD detected <- %s", path);
    // Drop everything first: nothing from the previous career may survive into
    // the loaded one.
    Invalidate("load in progress");
    Load(path);
    g_loadPending = true;
    return FileEvent::Loaded;
}

// Called from the safe point each frame. Detects a career starting without a
// preceding load, which means the player began a brand new game.
inline bool Tick()      // returns true if the career context just changed
{
    Guard g;
    bool changed = false;
    bool inGame = state::InGame();
    if (inGame && !g_wasInGame) {
        changed = true;
        if (g_loadPending) {
            Log("session: career resumed from save");
            g_loadPending = false;
        } else if (!g_state.valid) {
            BeginNewCareer();
        }
    } else if (!inGame && g_wasInGame) {
        Log("session: left the career (menu)");
        g_loadPending = false;
        changed = true;
    }
    g_wasInGame = inGame;
    return changed;
}

// Is it safe for content to run right now?
inline bool Ready()
{
    Guard g;
    return g_state.valid && !g_loadPending && state::InGame();
}

inline void NoteEventFired(DWORD tick)
{
    Guard g;
    ++g_state.eventsFired;
    g_state.lastEventTick = (int)tick;
}

} // namespace session
