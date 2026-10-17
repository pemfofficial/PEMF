// wind.h - a wind that changes: backing, veering, and the occasional front.
//
// ============================================================================
// WHAT THE GAME DOES (decompiled 2026-10-16, see docs/JOURNAL.md)
// ============================================================================
// The wind direction is one int32 angle at 0x007253BC, a full turn = 2^32, so
// 0x40000000 is 90 degrees -- the trade wind, blowing weather west. It is READ
// in about twenty places and WRITTEN in three:
//
//   0x00461E51  every sailing tick, inside the render FUN_004612B0 that both
//               the overworld and sea battles use:
//                   target = 3 * (DAT_008CA930 * DAT_0085A158 + 0x15555540)
//                   wind  += (target - wind) / 32
//               0x15555540 * 3 is ~90 degrees; the product nudges it, which
//               in practice leaves the wind all but fixed.
//   0x0047A0C9  sea battle start (FUN_0047A040): wind = 0x40000000
//   0x0047EDE3  inside the same battle function
//
// What reads it, and so what a new wind changes on its own:
//   - ship speed (FUN_00467F90 at 0x0046885E) -- the point of sail, for the
//     player and every AI ship alike;
//   - the weather drift (0x0046359C..0x004635D2): each slot moves by
//     -(sin w, cos w) * speed, via FUN_004DB210 -- why storms always came from
//     the same side;
//   - the HUD wind needle (FUN_004741B0), sail and wave rendering.
//
// ============================================================================
// WHAT PEMF DOES
// ============================================================================
// Steers the TARGET, never the store. The 12 bytes at 0x00461E23 that load and
// multiply the two globals become one load of g_pemfWindTerm (+7 NOPs), so the
// game's own easing still turns the wind smoothly. On the overworld PEMF sets
// that term to the wind it wants; everywhere else -- a sea battle above all --
// it puts back exactly the product the game would have computed, so a battle
// sees the stock wind. 3 is odd, so 3*x is invertible mod 2^32 and the target
// can be hit exactly.
//
// The model: the trade wind prevails. Every few minutes the wind shifts to a
// new direction within `windVariance` of it; now and then a front swings it
// further. It always turns at a sailor's pace, never snaps.
//
// ⛔ A CODE WRITE -- ONLY FROM THE FIRST SAFE POINT, like storms and sailkeys.
#pragma once
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log.h"

extern "C" {
// Read by the patched instruction at 0x00461E23. Must stay at a fixed address.
inline volatile int32_t g_pemfWindTerm = 0;
}

namespace wind {

constexpr uintptr_t kWindDir    = 0x007253BC;   // int32 angle, 2^32 = 360 deg
constexpr uintptr_t kTargetLoad = 0x00461E23;   // the 12 bytes we replace
constexpr uintptr_t kTermA      = 0x008CA930;   // the game's two factors
constexpr uintptr_t kTermB      = 0x0085A158;
constexpr uint32_t  kBias       = 0x15555540;   // target = 3 * (term + kBias)
// THE AI SHIPS' WIND. The sailing update reads DAT_007253BC only for ships
// 0..7 (the player's fleet); every other ship reads DAT_00725600 instead
// (0x00468845). It is written ONCE, to 90 degrees (0x0045F99D), and never again
// -- so with a moving wind and nothing else, the player sailed on one wind and
// every other ship on the old trade wind. Held equal to the live wind on the
// overworld; given back its 90 degrees everywhere else.
constexpr uintptr_t kAiWindDir  = 0x00725600;
constexpr int32_t   kAiWindStock = 0x40000000;
constexpr uint32_t  kInv3       = 0xAAAAAAABu;  // 3 * kInv3 == 1 (mod 2^32)

constexpr uint8_t kOriginal[12] = { 0xA1, 0x30, 0xA9, 0x8C, 0x00,          // mov eax,[0x8CA930]
                                    0x0F, 0xAF, 0x05, 0x58, 0xA1, 0x85, 0x00 }; // imul eax,[0x85A158]

struct Tuning {
    // THE MODEL (owner, after the third playtest: "random, but realistic").
    // Most of the time the trade wind MEANDERS around its prevailing direction
    // -- small, unhurried shifts. Every so often a FRONT comes through: the wind
    // swings hard, sometimes right round, holds, and then backs to the trades.
    int enabled       = 1;     // 0 = the game's own, all but fixed, trade wind
    int prevailing    = 90;    // degrees; 90 = the stock trade wind
    // 2026-10-17, from play: "too aggressive -- it should change gradually,
    // with only rare sharp changes". Turning is in TENTHS of a degree a second
    // so a meander can be properly slow; fronts are rare.
    int variance      = 30;    // meanders stay within this of prevailing
    int minTurn       = 8;     // ...and each one moves at least this far
    int shiftMinSec   = 150;   // time between meanders, at random in this range
    int shiftMaxSec   = 360;
    int firstShiftSec = 90;    // the first meander of a voyage, about this soon
    int turnDegPerSec = 3;     // TENTHS of a degree a second (0.3 deg/s)
    int frontMinSec   = 1200;  // time between fronts, at random in this range
    int frontMaxSec   = 2400;
    int frontHoldMin  = 240;   // how long a front's wind holds
    int frontHoldMax  = 480;
    int minFrontTurn  = 70;    // a front swings at least this far...
    int frontVariance = 160;   // ...anywhere within this of prevailing
    int frontTurnDegPerSec = 20;  // TENTHS of a degree a second (2 deg/s)
};
inline Tuning g_tune;

inline bool    g_patched      = false;
inline uint8_t g_saved[12]    = {0};
inline double  g_dirDeg       = 90.0;   // the direction PEMF wants right now
inline double  g_goalDeg      = 90.0;   // ...and where it is heading
inline DWORD   g_nextShift    = 0;
inline DWORD   g_lastTick     = 0;
inline DWORD   g_overworldAt  = 0;      // last time the overworld drew
inline bool    g_front        = false;   // a front is through right now
inline DWORD   g_nextFront    = 0;
inline DWORD   g_frontEnds    = 0;
inline bool    g_aiSteered    = false;  // DAT_00725600 is ours right now

inline double Wrap(double d) { d = fmod(d, 360.0); return d < 0 ? d + 360.0 : d; }
inline double Delta(double from, double to)   // shortest signed turn
{
    double d = Wrap(to - from);
    return d > 180.0 ? d - 360.0 : d;
}

// The direction the game is actually using, in degrees (90 = trade wind).
inline double CurrentDeg()
{
    const uint32_t raw = *(const uint32_t*)kWindDir;
    return raw * (360.0 / 4294967296.0);
}

inline int Rand(int lo, int hi) { return hi <= lo ? lo : lo + rand() % (hi - lo + 1); }

// Pick a goal at least `need` degrees from where the wind is now, within
// `span` of prevailing (or the biggest turn available if that cannot be met).
inline double Goal(int span, int need)
{
    double best = g_dirDeg, bestTurn = -1;
    for (int tries = 0; tries < 24; ++tries) {
        const double cand = Wrap(g_tune.prevailing + Rand(-span, span));
        const double turn = fabs(Delta(g_dirDeg, cand));
        if (turn > bestTurn) { best = cand; bestTurn = turn; }
        if (turn >= need) break;
    }
    return best;
}

inline void StartFront(DWORD now, const char* why)
{
    g_front = true;
    g_goalDeg = Goal(g_tune.frontVariance, g_tune.minFrontTurn);
    g_frontEnds = now + (DWORD)Rand(g_tune.frontHoldMin, g_tune.frontHoldMax) * 1000;
    g_nextShift = g_frontEnds;            // no meanders while it holds
    Log("wind: A FRONT COMES THROUGH%s -- the wind %s round toward %.0f deg (now %.0f), "
        "holding ~%d s", why, Delta(g_dirDeg, g_goalDeg) > 0 ? "veers" : "backs",
        g_goalDeg, g_dirDeg, (int)(g_frontEnds - now) / 1000);
}

inline void Meander(DWORD now)
{
    g_goalDeg = Goal(g_tune.variance, g_tune.minTurn);
    g_nextShift = now + (DWORD)Rand(g_tune.shiftMinSec, g_tune.shiftMaxSec) * 1000;
    Log("wind: the wind %s toward %.0f deg (now %.0f, prevailing %d)",
        Delta(g_dirDeg, g_goalDeg) > 0 ? "veers" : "backs", g_goalDeg, g_dirDeg,
        g_tune.prevailing);
}

inline void Advance(DWORD now)
{
    if (g_front && (int)(now - g_frontEnds) >= 0) {
        g_front = false;
        g_nextFront = now + (DWORD)Rand(g_tune.frontMinSec, g_tune.frontMaxSec) * 1000;
        Log("wind: the front passes -- the wind backs to the trades");
        Meander(now);                      // home, by way of an ordinary shift
        return;
    }
    if (!g_front && (int)(now - g_nextFront) >= 0) { StartFront(now, ""); return; }
    if (!g_front && (int)(now - g_nextShift) >= 0) Meander(now);
}

inline int32_t TermFor(double deg)
{
    const uint32_t want = (uint32_t)(Wrap(deg) / 360.0 * 4294967296.0);
    return (int32_t)(want * kInv3 - kBias);
}

inline int32_t VanillaTerm()
{
    // imul wraps; a signed C++ multiply that overflows is undefined. Unsigned.
    return (int32_t)(*(const uint32_t*)kTermA * *(const uint32_t*)kTermB);
}

// Called from the overworld's own weather draw: the only place we know for
// certain the player is at sea on the overworld, not in a battle or a town.
inline void OverworldTick()
{
    if (!g_patched || !g_tune.enabled) return;
    const DWORD now = GetTickCount();
    g_overworldAt = now;

    const int dt = g_lastTick ? (int)(now - g_lastTick) : 0;
    g_lastTick = now;
    if (g_nextShift == 0) {
        g_dirDeg = CurrentDeg();          // start from whatever the game has
        g_goalDeg = g_dirDeg;
        g_nextShift = now + (DWORD)Rand(g_tune.firstShiftSec, g_tune.firstShiftSec * 2) * 1000;
        g_nextFront = now + (DWORD)Rand(g_tune.frontMinSec, g_tune.frontMaxSec) * 1000;
    }
    Advance(now);

#ifdef PEMF_DEV_TOOLS
    // Ctrl+Shift+W: a front, now. DEVELOPER BUILDS ONLY (owner, 2026-10-16:
    // players must not be able to summon weather in the release). Build with
    //   .\build.ps1 -Defines "PEMF_DEV_TOOLS=1"
    static bool keyWas = false;
    const bool key = (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                     (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState('W') & 0x8000);
    if (key && !keyWas) StartFront(now, " (Ctrl+Shift+W)");
    keyWas = key;
#endif

    // What the GAME is using, not just what we asked for, now and then.
    static DWORD lastReport = 0;
    if (now - lastReport > 30000) {
        lastReport = now;
        int aiRaw = 0;
        __try { aiRaw = *(const int32_t*)kAiWindDir; } __except (EXCEPTION_EXECUTE_HANDLER) { }
        Log("wind: game wind %.0f deg (AI ships %.0f), PEMF wants %.0f heading for %.0f, "
            "next shift in %d s", CurrentDeg(), (uint32_t)aiRaw * (360.0 / 4294967296.0),
            g_dirDeg, g_goalDeg, (int)(g_nextShift - now) / 1000);
    }

    // Turn toward the goal at a sailor's pace. A long gap (a battle, a town)
    // does not count: the wind waited, like the storms do.
    if (dt > 0 && dt < 1000) {
        const double step = (g_front ? g_tune.frontTurnDegPerSec : g_tune.turnDegPerSec) * dt / 10000.0;
        const double d = Delta(g_dirDeg, g_goalDeg);
        g_dirDeg = fabs(d) <= step ? g_goalDeg : Wrap(g_dirDeg + (d > 0 ? step : -step));
    }
    g_pemfWindTerm = TermFor(g_dirDeg);

    // Every other ship sails on the same wind as the player.
    __try { *(int32_t*)kAiWindDir = *(const int32_t*)kWindDir; g_aiSteered = true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// Every safe point. Off the overworld, hand the game its own term back.
inline void SafePointTick()
{
    if (!g_patched) return;
    if (!g_tune.enabled || GetTickCount() - g_overworldAt > 300) {
        g_pemfWindTerm = VanillaTerm();
        if (g_aiSteered) {             // a battle, a town: the stock AI wind
            __try { *(int32_t*)kAiWindDir = kAiWindStock; }
            __except (EXCEPTION_EXECUTE_HANDLER) { }
            g_aiSteered = false;
        }
    }
}

inline void LoadTuning(const char* gameDir)
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\PEMF\\storms.ini", gameDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64] = {0};
        int v = 0;
        if (sscanf_s(line, " %63[A-Za-z_] = %d", key, (unsigned)sizeof(key), &v) != 2) continue;
        auto clamp = [](int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); };
        if      (_stricmp(key, "dynamicWind")         == 0) g_tune.enabled       = v != 0;
        else if (_stricmp(key, "windPrevailing")      == 0) g_tune.prevailing    = v;
        else if (_stricmp(key, "windVariance")        == 0) g_tune.variance      = clamp(v, 0, 180);
        else if (_stricmp(key, "windMinTurn")         == 0) g_tune.minTurn       = clamp(v, 0, 180);
        else if (_stricmp(key, "windShiftMinSec")     == 0) g_tune.shiftMinSec   = clamp(v, 10, 3600);
        else if (_stricmp(key, "windShiftMaxSec")     == 0) g_tune.shiftMaxSec   = clamp(v, 10, 3600);
        else if (_stricmp(key, "windFirstShiftSec")   == 0) g_tune.firstShiftSec = clamp(v, 5, 3600);
        else if (_stricmp(key, "windTurnDegPerSec")   == 0) g_tune.turnDegPerSec = clamp(v * 10, 1, 450);
        else if (_stricmp(key, "windTurnTenths")      == 0) g_tune.turnDegPerSec = clamp(v, 1, 450);
        else if (_stricmp(key, "windFrontMinSec")     == 0) g_tune.frontMinSec   = clamp(v, 30, 7200);
        else if (_stricmp(key, "windFrontMaxSec")     == 0) g_tune.frontMaxSec   = clamp(v, 30, 7200);
        else if (_stricmp(key, "windFrontHoldMinSec") == 0) g_tune.frontHoldMin  = clamp(v, 10, 3600);
        else if (_stricmp(key, "windFrontHoldMaxSec") == 0) g_tune.frontHoldMax  = clamp(v, 10, 3600);
        else if (_stricmp(key, "windMinFrontTurn")    == 0) g_tune.minFrontTurn  = clamp(v, 0, 180);
        else if (_stricmp(key, "windFrontVariance")   == 0) g_tune.frontVariance = clamp(v, 0, 180);
        else if (_stricmp(key, "windFrontTurnDegPerSec") == 0) g_tune.frontTurnDegPerSec = clamp(v * 10, 1, 450);
        else if (_stricmp(key, "windFrontTurnTenths") == 0) g_tune.frontTurnDegPerSec = clamp(v, 1, 450);
    }
    fclose(f);
    if (g_tune.shiftMaxSec < g_tune.shiftMinSec) g_tune.shiftMaxSec = g_tune.shiftMinSec;
    if (g_tune.frontMaxSec < g_tune.frontMinSec) g_tune.frontMaxSec = g_tune.frontMinSec;
    if (g_tune.frontHoldMax < g_tune.frontHoldMin) g_tune.frontHoldMax = g_tune.frontHoldMin;
    Log("wind: dynamic=%d trades %d +/-%d (shift >= %d deg every %d-%d s at %d tenths deg/s); "
        "fronts every %d-%d s, holding %d-%d s, swinging >= %d deg at %d tenths deg/s",
        g_tune.enabled, g_tune.prevailing, g_tune.variance, g_tune.minTurn,
        g_tune.shiftMinSec, g_tune.shiftMaxSec, g_tune.turnDegPerSec, g_tune.frontMinSec,
        g_tune.frontMaxSec, g_tune.frontHoldMin, g_tune.frontHoldMax, g_tune.minFrontTurn,
        g_tune.frontTurnDegPerSec);
}

// ⛔ First safe point only (a code write).
inline bool Install()
{
    if (g_patched || !g_tune.enabled) return g_patched;
    uint8_t* at = (uint8_t*)kTargetLoad;
    __try {
        if (memcmp(at, kOriginal, sizeof(kOriginal)) != 0) {
            Log("wind: the target load at 0x%08X is not what we expect -- the wind "
                "stays the game's own", (unsigned)kTargetLoad);
            return false;
        }
        g_pemfWindTerm = VanillaTerm();   // identical behaviour until we steer
        DWORD old = 0;
        if (!VirtualProtect(at, sizeof(kOriginal), PAGE_EXECUTE_READWRITE, &old)) return false;
        memcpy(g_saved, at, sizeof(g_saved));
        at[0] = 0xA1;                                     // mov eax, [g_pemfWindTerm]
        *(uint32_t*)(at + 1) = (uint32_t)(uintptr_t)&g_pemfWindTerm;
        memset(at + 5, 0x90, 7);                          // the imul, gone
        VirtualProtect(at, sizeof(kOriginal), old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, sizeof(kOriginal));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("wind: faulted while patching -- the wind stays the game's own");
        return false;
    }
    g_patched = true;
    Log("wind: the overworld wind is PEMF's now (now %.0f deg); battles keep the game's",
        CurrentDeg());
    return true;
}

inline void Restore()
{
    if (!g_patched) return;
    __try {
        uint8_t* at = (uint8_t*)kTargetLoad;
        DWORD old = 0;
        if (VirtualProtect(at, sizeof(g_saved), PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(at, g_saved, sizeof(g_saved));
            VirtualProtect(at, sizeof(g_saved), old, &old);
            FlushInstructionCache(GetCurrentProcess(), at, sizeof(g_saved));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
    g_patched = false;
    if (g_aiSteered) {
        __try { *(int32_t*)kAiWindDir = kAiWindStock; } __except (EXCEPTION_EXECUTE_HANDLER) { }
        g_aiSteered = false;
    }
}

} // namespace wind
