// crewwatch.h - men a surgeon saves.
//
// The same shape as `loot.h`, and for the same reason: it works, it needs no
// code written into the game, and it covers every cause at once.
//
// PEMF samples the crew count at the safe point. When it FALLS, men were lost --
// to a boarding action, to disease, to desertion, to a storm. A surgeon aboard
// puts a percentage of them back, and a butcher takes a few more.
//
// ------------------------------------------------------------ why not a hook
// Crew is lost in many places and we have mapped none of them. Finding and
// hooking each would be weeks of reverse engineering for a worse result: a
// sampled difference catches causes we have never looked at, including any the
// game adds in a path we do not know about. The cost is that we cannot tell
// WHY the men were lost, so a surgeon saves men from disease and from a
// cutlass alike. For a first version that is a fair trade, and it is written
// down here rather than discovered later.
//
// ⚠️ A RISE IS NOT A SAVE. Crew goes up when the player recruits in a tavern,
// and putting a surgeon's percentage on top of that would be nonsense. Only a
// FALL is acted on, and a rise simply re-baselines.
#pragma once
#include <windows.h>

#include "log.h"
#include "state.h"
#include "officerfx.h"

namespace crewwatch {

inline int  g_last    = -1;     // -1 = no baseline
inline bool g_faulted = false;
inline LONG g_saved   = 0;      // men put back this session
inline LONG g_lost    = 0;      // men lost this session

inline void Rebase(const char* why)
{
    g_last = state::InGame() ? state::Crew() : -1;
    Log("crew: baseline %d (%s)", g_last, why);
}

inline void Tick()
{
    if (g_faulted) return;
    if (!state::InGame()) { g_last = -1; return; }

    const int now = state::Crew();
    if (now < 0) return;

    if (g_last < 0)   { g_last = now; return; }
    if (now == g_last) return;

    if (now > g_last) {          // recruited, not saved
        g_last = now;
        return;
    }

    const int lost = g_last - now;
    g_last = now;
    InterlockedExchangeAdd(&g_lost, lost);

    const int pct = officerfx::g_surgeon;
    if (pct == 0) return;

    // A surgeon cannot save a man who was never at risk, so this is a share of
    // what was actually lost. Rounded down: half a man is nobody.
    int saved = (int)(((long long)lost * pct) / 100);
    if (saved == 0) return;

    // Never give back more than were lost -- at 100% or above, everybody lives,
    // and beyond that the number would be inventing men out of nothing.
    if (saved > lost) saved = lost;

    __try {
        state::AddCrew(saved, saved > 0 ? "surgeon" : "no surgeon");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faulted = true;
        Log("!! crew: FAULT adjusting the crew (0x%08X) -- disabled for this "
            "session", GetExceptionCode());
        return;
    }

    // Re-baseline to what the crew IS after our own change, or the next tick
    // reads our correction as a fresh loss and pays it again.
    g_last = state::Crew();

    if (saved > 0) {
        InterlockedExchangeAdd(&g_saved, saved);
        Log("crew: %d lost -> %d saved by the surgeon (%d%%)", lost, saved, pct);
    } else {
        Log("crew: %d lost -> %d more died for want of a better surgeon (%d%%)",
            lost, -saved, pct);
    }
}

inline void Report()
{
    Log("crew: %ld lost this session, %ld put back by the surgeon (%d%%)",
        g_lost, g_saved, officerfx::g_surgeon);
}

} // namespace crewwatch
