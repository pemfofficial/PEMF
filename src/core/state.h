// state.h - validated access to the game's live state.
//
// Direct writes like `UndividedPlunder() = 0` were fine for two hand-written
// events. Once effects are authored in JSON, a typo or a hostile value must not
// be able to corrupt a career. Everything that mutates game state goes through
// here so that it is:
//
//   * refused entirely when we are not in a game (menu, intro, loading)
//   * clamped to a sane range
//   * logged, so an event's effects are traceable after the fact
//
// Reads are cheap and unguarded; only mutation is policed.
#pragma once
#include "game.h"
#include "log.h"

namespace state {

// ------------------------------------------------------------------- limits
// Deliberately generous -- these are guard rails against corruption, not
// game-design balance. Balance belongs in the event content.
constexpr int kMaxCrew    = 2000;
constexpr int kMaxPlunder = 100000000;

// --------------------------------------------------------------- predicates
// The game zeroes crew count outside an active career, which makes it a
// reliable "are we actually in a game" test -- confirmed by the heartbeat
// reading crew=0 at the menu and crew=40 once sailing.
inline bool InGame()
{
    __try {
        return game::CrewCount() > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Single gate every mutation passes through.
inline bool CanMutate(const char** whyNot)
{
    if (!InGame()) { *whyNot = "not in a game"; return false; }
    return true;
}

// ------------------------------------------------------------------- reads
inline int Crew()    { return game::CrewCount(); }
inline int Plunder() { return game::UndividedPlunder(); }
inline int Morale()  { return game::GetMoraleLevel(); }
inline int Months()  { return (int)game::MonthsAtSea(); }

// ------------------------------------------------------------------ writes
// Each returns whether the write actually happened.

inline int Clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

inline bool SetPlunder(int value, const char* reason)
{
    const char* why = nullptr;
    if (!CanMutate(&why)) {
        Log("  state: REFUSED SetPlunder(%d) [%s] -- %s", value, reason, why);
        return false;
    }
    int clamped = Clamp(value, 0, kMaxPlunder);
    int before  = game::UndividedPlunder();
    game::UndividedPlunder() = clamped;
    if (clamped != value)
        Log("  state: plunder %d -> %d [%s] (clamped from %d)",
            before, clamped, reason, value);
    else
        Log("  state: plunder %d -> %d [%s]", before, clamped, reason);
    return true;
}

inline bool AddPlunder(int delta, const char* reason)
{
    if (!InGame()) {
        Log("  state: REFUSED AddPlunder(%d) [%s] -- not in a game",
            delta, reason);
        return false;
    }
    return SetPlunder(game::UndividedPlunder() + delta, reason);
}

inline bool SetCrew(int value, const char* reason)
{
    const char* why = nullptr;
    if (!CanMutate(&why)) {
        Log("  state: REFUSED SetCrew(%d) [%s] -- %s", value, reason, why);
        return false;
    }
    int clamped = Clamp(value, 0, kMaxCrew);
    int before  = game::CrewCount();
    game::CrewCount() = clamped;
    Log("  state: crew %d -> %d [%s]%s", before, clamped, reason,
        (clamped != value) ? " (clamped)" : "");
    return true;
}

inline bool AddCrew(int delta, const char* reason)
{
    if (!InGame()) {
        Log("  state: REFUSED AddCrew(%d) [%s] -- not in a game", delta, reason);
        return false;
    }
    return SetCrew(game::CrewCount() + delta, reason);
}

// Snapshot for logging an event's net effect.
struct Snapshot { int crew, plunder, morale, months; };

inline Snapshot Capture()
{
    Snapshot s{};
    __try {
        s.crew = Crew(); s.plunder = Plunder();
        s.morale = Morale(); s.months = Months();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
    return s;
}

inline void LogDelta(const char* label, const Snapshot& a, const Snapshot& b)
{
    Log("  %s: crew %d->%d  plunder %d->%d  morale %d->%d",
        label, a.crew, b.crew, a.plunder, b.plunder, a.morale, b.morale);
}

} // namespace state
