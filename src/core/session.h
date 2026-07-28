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
// NEW CAREER vs LOADED SAVE -- and this is the hard part, so read it before
// changing anything here. FOUR APPROACHES WERE TRIED AND MEASURED WRONG:
//
//   1. "A save file was read, so a load happened." No. Starting a brand new
//      career makes the game read a save file too, at the same distance in
//      time, with nothing about the access to tell them apart.
//   2. "A burst of reads is the load screen listing saves; a lone read is a
//      load." True as far as it goes -- a listing put seven files inside 13 ms
//      -- but browsing SLOWLY produces lone reads as well.
//   3. "Commit the loaded state when a career begins." Loading from INSIDE a
//      career never begins one, so the load was silently never applied.
//   4. "state::InGame() tells me whether a career is running." It does not.
//      That is "crew > 0", and the crew count does NOT return to zero at the
//      main menu, so after the first career of a session it stays true forever
//      and no transition is ever seen again.
//
// WHAT ACTUALLY WORKS: ask the GAME, not the filesystem.
//   * Career presence comes from the screen state (InCareer), not the crew.
//   * A read only STAGES a sidecar; nothing is applied on the strength of it.
//   * The sidecar carries a FINGERPRINT of its career, and staged state is
//     committed only once that fingerprint matches the career on screen.
//   * A career entered with nothing committed is new, and inherits nothing.
//
// Full account, including the failures, in re/experiments/career_state/.
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

    // The colours this career flies, and the ones that are honestly its own.
    // The game keeps a single flag choice in Config.ini, which is global -- the
    // same for every captain you ever play. A disguise is not a preference, it
    // is something THIS captain is doing right now, so it belongs to the save.
    //
    // Fixed buffers rather than std::string on purpose: this struct is written
    // from the game thread and read by the file hooks on any thread, and the
    // note below about that race is the reason. Plain characters keep it benign.
    char flagName[128]     = {0};   // what is on the mast now
    char trueFlagName[128] = {0};   // what this captain flies honestly

    // A FINGERPRINT OF THE CAREER THIS STATE BELONGS TO, written at save time.
    //
    // Detecting "new career versus loaded save" by watching save FILES cannot
    // work, and that is measured rather than suspected: starting a brand new
    // career makes the game read a save file exactly as loading one does, at
    // the same distance in time, with nothing to tell them apart. A player who
    // went straight to New Career still produced a read of slot1, and inherited
    // slot1's colours.
    //
    // So the question is answered from the GAME instead. These are cheap,
    // already-mapped values that a career carries with it; if they do not match
    // what is on screen, this state is somebody else's and must not be used.
    // A new career reads months=0 with starting gold and crew, which will not
    // match a career that has been played.
    int fpMonths = -1;              // -1 = written before fingerprints existed
    int fpGold   = -1;
    int fpCrew   = -1;

    void Clear()
    {
        version = kStateVersion;
        eventsFired = 0;
        lastEventTick = 0;
        flagName[0] = 0;
        trueFlagName[0] = 0;
        fpMonths = fpGold = fpCrew = -1;
        valid = false;
    }
};

inline ModState g_state;
inline char     g_currentSave[MAX_PATH] = {0};   // last save file touched
inline bool     g_wasInGame   = false;

// A sidecar read but NOT yet applied. See OnSaveFileOpened for why reading and
// committing must be separate acts.
inline ModState g_staged;
inline bool     g_hasStaged  = false;
inline DWORD    g_lastReadAt = 0;
inline DWORD    g_stagedAt   = 0;

// How long a staged load may wait to be recognised. A real load has the save in
// memory within a second or two; anything still unmatched after this was never
// our career's state to begin with.
constexpr DWORD kStagedTimeoutMs = 15000;

// True once a loaded save's state has been applied, and until the player next
// leaves a career. It is what separates "this career came from a save" from
// "this career is brand new", which no single moment in time can answer: the
// load and the career entry are seconds apart, and loading from inside a career
// never produces an entry at all.
inline bool     g_committed  = false;

// Opens closer together than this are the load screen listing saves rather than
// the player loading one. Measured: a listing put seven files inside 13 ms; the
// real load that followed stood alone 1.6 s later. Generous either way.
constexpr DWORD kBurstMs = 250;

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

// ------------------------------------------------------- am I in a career?
// NOT state::InGame(). That asks "is there a crew", and the crew count DOES NOT
// go back to zero when the player returns to the main menu -- so after the
// first career of a session it stays true forever, no career transition is ever
// seen again, and every later career silently inherits the last one's state.
// Measured: a session that abandoned a career and started another produced no
// "left the career" transition at all.
//
// The screen state answers it properly, and it is the same pair the notice gate
// uses (see triggers.h). Depth 1 is the main menu and depth 2 is character
// creation; a career occupies depth 3 and above -- sailing and towns at 3, the
// Load/Save screen at 4, battle at 4-5.
constexpr int kCareerScreenDepth = 3;

// Guarded read, in its own function: Tick() holds a lock guard, and MSVC will
// not allow __try in a function that needs unwinding.
inline int ScreenDepth()
{
    __try { return *(const int*)game::addr::ScreenDepth; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

inline bool InCareer()
{
    return state::InGame() && ScreenDepth() >= kCareerScreenDepth;
}

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
    // Written only when set, so a career that never touched its colours leaves
    // a sidecar identical to the ones earlier builds produced.
    if (g_state.flagName[0])     fprintf(f, "flag=%s\n",     g_state.flagName);
    if (g_state.trueFlagName[0]) fprintf(f, "trueFlag=%s\n", g_state.trueFlagName);
    // The fingerprint of the career this state belongs to, stamped from the
    // live game so a load can prove the state is its own. Without these three
    // lines every sidecar reads as "unknown" and no load can ever be verified,
    // which is precisely what happened when this write was accidentally left
    // out: the field, the parser and the comparison all existed, and nothing
    // ever wrote the values.
    fprintf(f, "fpMonths=%d\n", state::Months());
    fprintf(f, "fpGold=%d\n",   state::Plunder());
    fprintf(f, "fpCrew=%d\n",   state::Crew());
    fclose(f);
    Log("session: wrote sidecar (eventsFired=%d) -> %s",
        g_state.eventsFired, path);
    return true;
}

// Copy a value from a sidecar line: everything up to the newline, and nothing
// that could not be a filename. A sidecar is a plain text file a player could
// edit, so what comes out of it is checked rather than trusted -- this string
// is handed to the engine's asset loader.
inline void ReadName(const char* src, char* out, size_t outsz)
{
    size_t n = 0;
    for (; src[n] && src[n] != '\n' && src[n] != '\r' && n + 1 < outsz; ++n) {
        const char c = src[n];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) { out[0] = 0; return; }     // refuse the whole value
        out[n] = c;
    }
    out[n] = 0;
}

inline bool LoadInto(const char* savePath, ModState& out)
{
    char path[MAX_PATH];
    SidecarPathFor(savePath, path, sizeof(path));

    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) {
        // Perfectly normal: a save made before the mod, or by an older version.
        Log("session: no sidecar for this save -- clean state staged");
        return false;
    }

    ModState loaded;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int v = 0;
        if (sscanf_s(line, "version=%d", &v) == 1)            loaded.version = v;
        else if (sscanf_s(line, "eventsFired=%d", &v) == 1)   loaded.eventsFired = v;
        else if (sscanf_s(line, "lastEventTick=%d", &v) == 1) loaded.lastEventTick = v;
        else if (strncmp(line, "flag=", 5) == 0)
            ReadName(line + 5, loaded.flagName, sizeof(loaded.flagName));
        else if (strncmp(line, "trueFlag=", 9) == 0)
            ReadName(line + 9, loaded.trueFlagName, sizeof(loaded.trueFlagName));
        else if (sscanf_s(line, "fpMonths=%d", &v) == 1)       loaded.fpMonths = v;
        else if (sscanf_s(line, "fpGold=%d", &v) == 1)         loaded.fpGold = v;
        else if (sscanf_s(line, "fpCrew=%d", &v) == 1)         loaded.fpCrew = v;
    }
    fclose(f);

    if (loaded.version != kStateVersion) {
        // Refuse rather than misinterpret fields from another layout.
        Log("session: sidecar version %d != %d -- discarding, starting clean",
            loaded.version, kStateVersion);
        return false;
    }

    loaded.valid = true;
    out = loaded;
    Log("session: sidecar read (eventsFired=%d, flag='%s') <- %s",
        out.eventsFired, out.flagName[0] ? out.flagName : "-", path);
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

// ------------------------------------------------------------- our colours
// Recorded here, applied by core.cpp at the safe point. The split matters: this
// file must stay free of game-memory access because the file hooks call into it
// from other threads, so it remembers WHAT should be flying and never sets it.
inline void RecordFlag(const char* name)
{
    Guard g;
    if (!name) return;
    strncpy_s(g_state.flagName, sizeof(g_state.flagName), name, _TRUNCATE);
}

inline void RecordTrueFlag(const char* name)
{
    Guard g;
    if (!name || !*name) return;
    // Only the first honest answer counts. Recording it again while disguised
    // would make the disguise permanent -- the thing the player could never
    // undo, and exactly the bug worth designing out rather than testing for.
    if (g_state.trueFlagName[0]) return;
    strncpy_s(g_state.trueFlagName, sizeof(g_state.trueFlagName), name, _TRUNCATE);
    Log("session: this captain's own colours are '%s'", g_state.trueFlagName);
}

inline const char* FlagName()     { return g_state.flagName; }
inline const char* TrueFlagName() { return g_state.trueFlagName; }

// True when the career is flying somebody else's colours.
inline bool Disguised()
{
    return g_state.flagName[0] && g_state.trueFlagName[0] &&
           strcmp(g_state.flagName, g_state.trueFlagName) != 0;
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
    // A SAVE FILE BEING READ IS NOT A LOAD. The Load/Save screen opens every
    // save it can see just to list them -- a measured session showed seven
    // files opened inside 13 ms -- and committing each one in turn meant that
    // merely LOOKING at the load screen replaced the live career's state with
    // whichever save happened to be listed last. Flags made it visible; event
    // counts and trigger progress were being overwritten the same way.
    //
    // The shape of the access tells the two apart, and the shape is not a
    // matter of timing luck: a browse is a BURST of opens milliseconds apart,
    // while an actual load is a LONE open. In the same session the real load
    // arrived 1.6 s after the burst, by itself.
    //
    // So a read only ever STAGES a sidecar. Tick() commits it when the career
    // is actually entered, and a career entered with nothing staged is a new
    // one. Nothing is dropped or overwritten until then, which is what makes
    // browsing harmless.
    const DWORD now = GetTickCount();
    const bool  partOfBurst = g_lastReadAt && (now - g_lastReadAt) < kBurstMs;
    g_lastReadAt = now;

    if (partOfBurst) {
        // The first file of a burst looked lone when it arrived; the second
        // proves otherwise, so throw the whole burst away.
        if (g_hasStaged) {
            g_hasStaged = false;
            Log("session: that was the load screen listing saves, not a load "
                "-- nothing staged");
        }
        return FileEvent::None;
    }

    Log("session: LOAD staged <- %s", path);
    g_staged = ModState{};
    g_stagedAt = GetTickCount();
    if (LoadInto(path, g_staged)) g_hasStaged = true;
    else {
        // No sidecar: still a real load, of a save this framework has never
        // seen. Staging clean state is the correct answer, not ignoring it.
        g_staged.Clear();
        g_staged.valid = true;
        g_hasStaged    = true;
    }
    return FileEvent::Loaded;
}

// Called from the safe point each frame. Detects a career starting without a
// preceding load, which means the player began a brand new game.
inline bool Tick()      // returns true if the career context just changed
{
    Guard g;
    bool changed = false;
    bool inGame = InCareer();

    // ------------------------------------------------------------- tracing
    // Every fix to this decision so far has been reasoned from a log that only
    // showed the OUTCOMES, and several of those guesses were wrong. This prints
    // the INPUTS -- once a second while anything is staged, and on every change
    // of the values that drive the branches -- so the next question is answered
    // by reading rather than by inference.
    {
        static DWORD lastTraceAt = 0;
        static int   lastDepth = -12345;
        static bool  lastIn = false, lastStaged = false, lastCommitted = false;
        const DWORD now = GetTickCount();
        const int depth = ScreenDepth();

        // Only what bears on the decision: whether we are in a career, whether
        // something is staged, and whether it was committed. Screen depth alone
        // changes constantly in normal play and is reported alongside the
        // others rather than triggering a line of its own.
        const bool shapeChanged = (inGame != lastIn) ||
                                  (g_hasStaged != lastStaged) ||
                                  (g_committed != lastCommitted);
        if (shapeChanged || (g_hasStaged && now - lastTraceAt > 1000)) {
            lastTraceAt = now;
            lastIn = inGame; lastDepth = depth;
            lastStaged = g_hasStaged; lastCommitted = g_committed;
            Log("session/trace: inCareer=%d crew>0=%d depth=%d staged=%d "
                "committed=%d | live months=%d gold=%d crew=%d | staged fp "
                "months=%d gold=%d crew=%d flag='%s'",
                (int)inGame, (int)state::InGame(), depth, (int)g_hasStaged,
                (int)g_committed, state::Months(), state::Plunder(),
                state::Crew(), g_staged.fpMonths, g_staged.fpGold,
                g_staged.fpCrew,
                g_staged.flagName[0] ? g_staged.flagName : "-");
        }
    }
    // A STAGED LOAD IS COMMITTED ONLY ONCE THE CAREER ON SCREEN PROVES TO BE
    // THE ONE IT CAME FROM.
    //
    // Everything simpler than this was tried and measured wrong. Committing on
    // a career transition missed loads made from inside a career. Committing on
    // sight handed the next career whatever was last read. And telling a load
    // from a new career by watching save FILES cannot work at all: starting a
    // new career makes the game read a save file exactly as loading one does.
    //
    // So the sidecar carries a fingerprint of its career, and it is compared
    // against the live game. A new career reads months=0 with starting gold and
    // crew, which will not match a career that has been played -- so the state
    // is discarded and the career correctly begins empty.
    if (g_hasStaged && inGame) {
        const int months = state::Months();
        const int gold   = state::Plunder();
        const int crew   = state::Crew();

        bool matches;
        if (g_staged.fpMonths < 0) {
            // Written before fingerprints existed. All we can say is that a
            // career at month zero is a fresh one, and fresh careers own
            // nothing -- so accept only a career already under way.
            matches = months > 0;
            if (matches)
                Log("session: sidecar predates fingerprints -- accepted "
                    "because this career is already under way");
        } else {
            matches = (g_staged.fpMonths == months) &&
                      (g_staged.fpGold   == gold) &&
                      (g_staged.fpCrew   == crew);
        }

        if (matches) {
            g_state     = g_staged;
            g_hasStaged = false;
            g_committed = true;
            changed     = true;
            Log("session: state applied from the loaded save (eventsFired=%d, "
                "flag='%s')", g_state.eventsFired,
                g_state.flagName[0] ? g_state.flagName : "-");
        } else if (GetTickCount() - g_stagedAt > kStagedTimeoutMs) {
            Log("session: the staged save does not match this career "
                "(months %d vs %d, gold %d vs %d, crew %d vs %d) -- discarded",
                g_staged.fpMonths, months, g_staged.fpGold, gold,
                g_staged.fpCrew, crew);
            g_hasStaged = false;
            if (!g_committed) BeginNewCareer();
        }
    }

    if (inGame && !g_wasInGame) {
        changed = true;
        if (g_committed) {
            Log("session: career resumed from save");
        } else {
            // NOTHING PROVEN BY NOW MEANS NOTHING WILL BE. The fingerprint
            // check above runs before this, and a genuine load has its values
            // in place the instant the career becomes live -- InGame() is
            // "crew > 0", and crew is part of the fingerprint. So a load that
            // was going to match has already matched.
            //
            // Waiting instead was a real bug: it left the PREVIOUS career's
            // state live while a new career played on, so the new captain sailed
            // under the old one's colours until a timeout eventually fired --
            // or never, if the player moved on first.
            if (g_hasStaged) {
                Log("session: a save was read but it is not this career's -- "
                    "discarding it");
                g_hasStaged = false;
            }
            BeginNewCareer();
        }
    } else if (!inGame && g_wasInGame) {
        Log("session: left the career (menu)");
        g_hasStaged  = false;
        g_committed  = false;
        g_lastReadAt = 0;
        changed = true;
    }

    g_wasInGame = inGame;
    return changed;
}

// Is it safe for content to run right now?
inline bool Ready()
{
    Guard g;
    return g_state.valid && state::InGame();
}

inline void NoteEventFired(DWORD tick)
{
    Guard g;
    ++g_state.eventsFired;
    g_state.lastEventTick = (int)tick;
}

} // namespace session
