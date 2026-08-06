// crewmorale.h - PEMF's own crew morale, and how it reaches the game.
//
// ------------------------------------------------------------- why our own
// The engine has no morale variable. `GetMoraleLevel` (0x00404810) DERIVES a
// 0-4 level every call, from the crew's share against what they expect:
//
//     expect = ((A - 4 + B)^2 / 4) - 4 * [0x869B27]     clamped 1..999
//     level  = ((plunder + 500) / (crew term)) / expect  clamped 0..4
//
// So there is nothing to raise. And the engine's five levels have no room for
// what this system needs: no negative, no resolution, and their NAMES are not
// even in the executable -- they come from `@HAPPY` out of `text.ini`, the one
// data file the engine will not read loose from disk. It cannot be taught a new
// tier.
//
// PEMF therefore keeps its own number, on a wide scale, with its own names.
//
// ------------------------------------------------------------ the closed loop
// Not a parallel fiction. Each tick PEMF maps its number to a target engine
// level and then SOLVES THE ENGINE'S OWN FORMULA BACKWARDS for the byte that
// produces it, and writes that. The engine then computes its morale from our
// input, so the HUD icon, the desertion behaviour and anything else reading
// morale all follow our number -- because we moved the term its own arithmetic
// depends on.
//
// `0x00869B27` is ours to move: it has exactly ONE cross reference in the
// executable, the read inside GetMoraleLevel, and no engine code writes it.
//
// ⚠️ MEASURED IN GAME, AND THE MEASUREMENT SHAPES THIS FILE:
//
//   * The byte has FULL authority -- it reaches every level from 0 to 4.
//   * But only about FOUR USABLE NOTCHES (-8..0). Below -8 is level 0 and
//     above 0 is level 4.
//   * LEVEL 3 WAS UNREACHABLE at the wealth tested. The expectation jumped
//     12 -> 8 -> 4 and the division landed on 1, 2, then 4.
//
// So the solver SEARCHES rather than calculating, and takes the closest level
// it can actually reach. A table of byte values baked at one wealth would be
// wrong at another -- the expectation base moved with `A` and `B`, both of
// which were zero in the test and may not always be.
#pragma once
#include <windows.h>
#include <string.h>

#include "log.h"
#include "game.h"
#include "state.h"
#include "officerfx.h"

namespace crewmorale {

// PEMF's scale. Wider than the engine's on purpose, and signed, so a crew can
// be worse than the engine's worst.
constexpr int kMin = -100;
constexpr int kMax =  100;

inline int  g_value   = 0;      // where the crew actually stand
inline bool g_enabled = true;
inline bool g_faulted = false;

// The engine terms the formula reads. Only the byte is ours.
constexpr uintptr_t kTermA = 0x00869A76;
constexpr uintptr_t kTermB = 0x0085A158;
constexpr uintptr_t kFlags = 0x00869B34;

inline signed char& Byte() { return *(signed char*)game::addr::MoraleByte; }

// ------------------------------------------------------------------- tiers
// Ours, because the engine's cannot be extended. Ordered worst to best; the
// threshold is the LOWEST value that earns the name.
struct Tier { int at; const char* name; };

// ⚠️ ZERO MUST BE THE NEUTRAL TIER. A fresh career starts at 0, and the first
// version put STEADY at +5, so every new captain was told his crew were UNEASY
// before anything had happened to them. The neutral band straddles zero.
inline const Tier kTiers[] = {
    { -100, "MUTINOUS"  },
    {  -70, "SEETHING"  },
    {  -45, "SULLEN"    },
    {  -20, "UNEASY"    },
    {   -8, "STEADY"    },   // <- 0 lands here
    {   25, "WILLING"   },
    {   60, "DEVOTED"   },
};

inline const char* TierName(int v)
{
    const char* name = kTiers[0].name;
    for (const Tier& t : kTiers)
        if (v >= t.at) name = t.name;
    return name;
}

inline const char* Name() { return TierName(g_value); }

// Our scale onto the engine's five. Deliberately coarse: the engine only has
// five levels and three of them do useful work, so pretending to more precision
// than that would be a lie about what the player can observe.
inline int TargetEngineLevel(int v)
{
    // Boundaries follow the tier table above, so the word the player is shown
    // and the level the engine is driven to never disagree about where they sit.
    if (v <= -45) return 0;   // SULLEN and worse
    if (v <= -20) return 1;   // UNEASY
    if (v <   25) return 2;   // STEADY
    if (v <   60) return 3;   // WILLING
    return 4;                 // DEVOTED
}

// ---------------------------------------------------------------- the solver
// The engine's own arithmetic, so we can ask "what would it say?" without
// calling it 256 times.
inline int LevelForByte(int b)
{
    const int a       = *(const signed char*)kTermA;
    const int bb      = *(const int*)kTermB;
    const int crew    = game::CrewCount();
    const int plunder = game::UndividedPlunder();
    const int flags   = *(const unsigned char*)kFlags;

    int expect = a - 4 + bb;
    expect = expect * expect;
    expect = (expect / 4) - 4 * b;
    if (expect < 1)   expect = 1;
    if (expect > 999) expect = 999;

    const int divisor = 0x14 + crew - ((flags & 0x80) ? 19 : 0);
    if (divisor == 0) return -1;

    int level = ((plunder + 500) / divisor) / expect;
    if (level < 0) return 0;
    if (level > 4) level = 4;
    return level;
}

// Find the byte that gets the engine closest to `want`. Searches because some
// levels are simply not reachable at a given wealth and crew -- level 3 was
// not, in the measurement this design is built on -- and because the reachable
// set moves with the player's gold.
//
// Ties go to the value NEAREST ZERO, so we disturb the engine's own reckoning
// as little as the target allows.
inline signed char SolveByte(int want)
{
    const int cur = (int)Byte();
    int bestByte = cur, bestErr = 99;

    // Seed with what we are already using, so an equally good answer never
    // displaces it. See the note on churn below.
    const int curLvl = LevelForByte(cur);
    if (curLvl >= 0)
        bestErr = (curLvl > want) ? (curLvl - want) : (want - curLvl);

    for (int b = -128; b <= 127; ++b) {
        const int lvl = LevelForByte(b);
        if (lvl < 0) continue;
        const int err = (lvl > want) ? (lvl - want) : (want - lvl);

        // STRICTLY better only. An equal answer leaves the byte alone.
        //
        // ⚠️ WITHOUT THIS THE HUD ICON FLAPS. The engine's levels are coarse and
        // the reachable set moves with the player's gold, so the target is
        // often unreachable and TWO different bytes sit equally far from it --
        // one above, one below. Re-solving each tick then alternated between
        // them, and the player watched the morale icon swing between two levels
        // while PEMF's own number climbed smoothly. Seen in play as
        // "wants engine level 2, nearest reachable is 3" followed later by
        // "nearest reachable is 1".
        if (err < bestErr) {
            bestErr  = err;
            bestByte = b;
            if (err == 0) break;      // exact; nothing will beat it
        }
    }
    return (signed char)bestByte;
}

// ------------------------------------------------------------------ moving it
inline void Clamp()
{
    if (g_value < kMin) g_value = kMin;
    if (g_value > kMax) g_value = kMax;
}

// The one way anything changes the crew's temper. Everything -- storms, events,
// officers, time -- comes through here, so there is a single place to look when
// morale moved and nobody knows why.
// `quiet` suppresses the routine line and keeps the tier-change one --
// drift happens every fifteen seconds forever and does not need narrating.
inline void Nudge(int delta, const char* why, bool quiet = false)
{
    if (delta == 0) return;
    const int before = g_value;
    const char* wasName = Name();

    g_value += delta;
    Clamp();

    if (g_value == before) return;

    const bool tierChanged = strcmp(wasName, Name()) != 0;

    if (tierChanged)
        Log("morale: %d -> %d [%s]  -- the crew are now %s (were %s)",
            before, g_value, why ? why : "?", Name(), wasName);
    else if (!quiet)
        Log("morale: %d -> %d [%s]", before, g_value, why ? why : "?");
}

inline void Set(int v, const char* why)
{
    g_value = v;
    Clamp();
    Log("morale: set to %d [%s] -- the crew are %s", g_value,
        why ? why : "?", Name());
}

// Where morale settles when nothing is happening to it. Officers are a standing
// presence rather than an event, so they move the RESTING POINT rather than
// shoving the number about -- a well-liked bosun makes a crew that recovers to
// a better place, which is what having him aboard should feel like.
inline int RestingPoint()
{
    return officerfx::g_morale * 4;
}

// ------------------------------------------------------------------- the tick
// Slow on purpose. Morale should be something a player notices over a voyage,
// not something that visibly ticks. Everything sharp comes through Nudge().
inline DWORD g_lastDrift = 0;
inline DWORD g_lastPush  = 0;
constexpr DWORD kDriftEveryMs = 15000;   // one step per 15 seconds of play
constexpr DWORD kPushEveryMs  = 2000;    // and the byte re-solved this often

inline void Tick()
{
    if (!g_enabled || g_faulted) return;
    if (!state::InGame()) return;

    const DWORD now = GetTickCount();

    // Drift toward the resting point, one point at a time.
    if (now - g_lastDrift >= kDriftEveryMs) {
        g_lastDrift = now;
        const int rest = RestingPoint();
        if (g_value < rest)      Nudge(1,  "settling", true);
        else if (g_value > rest) Nudge(-1, "settling", true);
    }

    // Push our number into the engine.
    if (now - g_lastPush >= kPushEveryMs) {
        g_lastPush = now;
        __try {
            const int want = TargetEngineLevel(g_value);
            const signed char b = SolveByte(want);
            if (Byte() != b) {
                const int got = LevelForByte(b);
                Byte() = b;
                // Worth a line when the engine cannot give us what we asked
                // for -- it is expected (level 3 was unreachable in testing)
                // and it should not look like a bug when it happens.
                if (got != want)
                    Log("morale: %s (%d) wants engine level %d, nearest "
                        "reachable is %d (byte %d) -- the engine's levels are "
                        "coarser than ours and this is expected",
                        Name(), g_value, want, got, (int)b);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_faulted = true;
            Log("!! morale: FAULT driving the engine (0x%08X) -- PEMF morale "
                "disabled for this session", GetExceptionCode());
        }
    }
}

// A career starting or loading. The byte belongs to the player record and is
// replaced by the save, so it has to be re-applied -- which the next Tick does.
inline void Reset(int value, const char* why)
{
    g_value     = value;
    g_faulted   = false;
    g_lastDrift = g_lastPush = GetTickCount();
    Clamp();
    Log("morale: %d [%s] -- the crew are %s", g_value, why ? why : "?", Name());
}

inline void Report()
{
    if (!state::InGame()) { Log("morale: no career"); return; }
    Log("morale: %d (%s) -> engine level %d (byte %d, resting point %d)",
        g_value, Name(), game::GetMoraleLevel(), (int)Byte(), RestingPoint());
}

} // namespace crewmorale
