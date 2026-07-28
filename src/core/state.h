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

// ------------------------------------------------------- colours we fly
// The nationality the player's vessel is SEEN to be. Every other write in this
// file adjusts a number the player already owns; this one changes what the rest
// of the game believes about them, and 84 code sites read it. So it is the most
// cautious thing here:
//
//   * the ORIGINAL value is remembered the first time it is changed, and
//     RestoreNationality() puts it back -- nothing should be able to strand a
//     career flying somebody else's flag;
//   * the value is checked against the five real nations rather than clamped,
//     because an out-of-range nation would index the flag-mesh table off the
//     end, and that table is exactly five entries long;
//   * every change is logged with both names, since the whole point of the
//     probe is reading afterwards what happened.
//
// Whether this write actually changes the flag drawn, the AI's behaviour, both
// or neither is UNVERIFIED -- that is what it exists to find out.
inline bool g_nationalityOverridden = false;
inline int  g_originalNationality   = -1;

inline int Nationality() { return game::ShipNationality(0); }

inline bool SetNationality(int nation, const char* reason)
{
    const char* why = nullptr;
    if (!CanMutate(&why)) {
        Log("  state: REFUSED SetNationality(%d) [%s] -- %s", nation, reason, why);
        return false;
    }
    if (nation < 0 || nation >= game::addr::kNationCount) {
        Log("  state: REFUSED SetNationality(%d) [%s] -- only 0..%d are real "
            "nations, and the flag table is exactly that long",
            nation, reason, game::addr::kNationCount - 1);
        return false;
    }

    int before = game::ShipNationality(0);
    if (!g_nationalityOverridden) {
        g_originalNationality   = before;
        g_nationalityOverridden = true;
        Log("  state: remembering true colours as %d (%s)",
            before, game::NationName(before));
    }

    game::SetShipNationalityRaw(0, nation);
    Log("  state: colours %d (%s) -> %d (%s) [%s]",
        before, game::NationName(before), nation, game::NationName(nation), reason);
    return true;
}

// Put the true colours back. Safe to call when nothing was changed.
inline bool RestoreNationality(const char* reason)
{
    if (!g_nationalityOverridden) return true;
    const char* why = nullptr;
    if (!CanMutate(&why)) {
        Log("  state: REFUSED RestoreNationality [%s] -- %s", reason, why);
        return false;
    }
    game::SetShipNationalityRaw(0, g_originalNationality);
    Log("  state: colours restored to %d (%s) [%s]", g_originalNationality,
        game::NationName(g_originalNationality), reason);
    g_nationalityOverridden = false;
    g_originalNationality   = -1;
    return true;
}

// A career change must never inherit a disguise. Called from the same place
// triggers are reset.
inline void ForgetNationalityOverride()
{
    g_nationalityOverridden = false;
    g_originalNationality   = -1;
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
