// loot.h - PEMF's share of what the crew takes.
//
// An officer skill that says "your quartermaster's eye finds a little more"
// has to actually change what lands in the hold, or it is decoration. This is
// the mechanism behind that.
//
// ------------------------------------------------------- observe, don't patch
// The award site is `FUN_004DCF20`, at 0x004DD01F:
//
//     004DD01F  MOV EAX,[EDX]           ; the amount awarded
//     004DD021  MOV ECX,[0x00869AB4]    ; plunder
//     004DD027  ADD ECX,EAX
//     004DD029  MOV EAX,[0x00861FF8]    ; a running total
//     004DD02E  MOV [0x00869AB4],ECX
//     004DD038  MOV [0x00861FF8],EAX
//     004DD03D  MOV [EDX],ESI           ; pending amount cleared
//
// ⚠️ The amount comes from MEMORY, not an immediate, so there is no constant to
// patch -- the storm-scale technique does not apply here and nothing is gained
// by forcing it.
//
// The lever is the other global. `0x00861FF8` is incremented BESIDE plunder at
// every site that AWARDS it, and untouched by the site that SPENDS it (the
// payment at 0x00451910 subtracts plunder and never writes this). So it rises
// exactly when the player EARNS loot and never when they part with it.
//
// That makes it a signal rather than a hook: sample it at the safe point, and a
// rise of N means N was just plundered. Which is better than patching in three
// ways that matter --
//
//   * NOTHING IS WRITTEN TO THE GAME'S CODE. No DRM race of the kind that cost
//     0.2.1 and 0.2.2 two releases, nothing to re-apply after a device reset.
//   * EVERY AWARD SITE AT ONCE -- a captured ship, a sacked town, a dug-up
//     treasure -- without having to find each one first.
//   * IT READS BETTER. The game says "@NUM gold pieces plundered!" with its own
//     figure, and PEMF's share arrives just after it, which is how someone
//     finding a little extra should feel. Rewriting the engine's own number
//     would mean intercepting the amount before it is formatted, for a worse
//     result.
//
// ------------------------------------------------------------------ the rules
// The bonus goes through `state.h` like every other change PEMF makes: clamped,
// career-gated, and logged with a reason. A player can always see where the
// gold came from.
#pragma once
#include <windows.h>

#include "log.h"
#include "game.h"
#include "state.h"
#include "content.h"

namespace loot {

// The running "plunder earned" total. Not in game.h's address block because
// nothing else uses it yet; move it there if a second caller appears.
constexpr uintptr_t kEarnedTotal = 0x00861FF8;

// Percent ADDED to what the game awarded. 0 means PEMF takes no part, which is
// the default and what a stock install does. 25 means a hundred pieces of
// plunder becomes a hundred and twenty five.
inline int  g_bonusPercent = 0;

// Anything above this is refused as a typo rather than applied. Ten times the
// loot is not a balance choice, it is a mistake in an ini file.
constexpr int kMaxBonusPercent = 500;

inline int   g_lastTotal = -1;      // -1 = no baseline yet
inline bool  g_faulted   = false;
inline LONG  g_awards    = 0;       // how many awards we have seen
inline LONG  g_granted   = 0;       // total bonus handed out this session

inline int EarnedTotal()
{
    __try { return *(const int*)kEarnedTotal; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Called when a career starts, or is loaded. The total belongs to the career,
// so a stale baseline from the previous one would read as an enormous award the
// moment the new one is sampled.
inline void Rebase(const char* why)
{
    g_lastTotal = EarnedTotal();
    Log("loot: baseline %d (%s)", g_lastTotal, why);
}

inline void SetBonusPercent(int pct, const char* why)
{
    if (pct < 0) pct = 0;
    if (pct > kMaxBonusPercent) {
        Log("loot: %d%% refused as a mistake -- clamped to %d%%",
            pct, kMaxBonusPercent);
        pct = kMaxBonusPercent;
    }
    if (pct == g_bonusPercent) return;
    g_bonusPercent = pct;
    Log("loot: share is now +%d%% (%s)", pct, why ? why : "set");
}

// Sampled once per safe point. Cheap: one read and a compare in the common case.
inline void Tick()
{
    if (g_faulted) return;
    if (!state::InGame()) { g_lastTotal = -1; return; }

    const int now = EarnedTotal();
    if (now < 0) return;

    if (g_lastTotal < 0) {          // first sample of this career
        g_lastTotal = now;
        return;
    }

    if (now == g_lastTotal) return;

    if (now < g_lastTotal) {
        // The total went backwards. FUN_00404220 writes this global and is
        // assumed to reset it on a new career, but that is an assumption -- so
        // re-baseline rather than trusting a negative delta, whatever the cause.
        Log("loot: earned-total fell %d -> %d, re-baselining", g_lastTotal, now);
        g_lastTotal = now;
        return;
    }

    const int awarded = now - g_lastTotal;
    g_lastTotal = now;
    InterlockedIncrement(&g_awards);

    if (g_bonusPercent <= 0) return;

    // Integer maths, deliberately: a percentage of an int award, rounded down.
    // Small awards can round to nothing, which is correct -- a 5% share of 10
    // pieces is not a piece.
    const int bonus = (int)(((long long)awarded * g_bonusPercent) / 100);
    if (bonus <= 0) return;

    __try {
        state::AddPlunder(bonus, "loot share");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faulted = true;
        Log("!! loot: FAULT applying a share (0x%08X) -- disabled for this "
            "session", GetExceptionCode());
        return;
    }

    InterlockedExchangeAdd(&g_granted, bonus);
    Log("loot: %d plundered -> +%d (%d%%)", awarded, bonus, g_bonusPercent);
}

// Read from PEMF\crew.ini. Deliberately one key and the Windows profile API
// rather than a parser of our own: the officer system will drive this number,
// and the ini exists so the mechanism can be tested and tuned before officers
// exist to drive it.
inline void LoadTuning(const char* pemfDir)
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\crew.ini", pemfDir);

    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        Log("loot: no %s -- PEMF takes no share (the stock game)", path);
        return;
    }
    const int pct = (int)GetPrivateProfileIntA("crew", "lootBonusPercent", 0,
                                               path);
    SetBonusPercent(pct, "crew.ini");
}

inline void Report()
{
    Log("loot: %ld award(s) seen this session, %ld granted as PEMF's share "
        "(+%d%%)", g_awards, g_granted, g_bonusPercent);
}

} // namespace loot
