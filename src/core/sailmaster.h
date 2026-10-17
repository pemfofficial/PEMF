// sailmaster.h - watch the engine's "far out to sea" card and record why it
// spoke -- and, since 2026-10-16, decide how talkative it is.
//
// The watcher below is read-only. The policy at the end of the file is one
// call-site redirect (0x0047376D), installed from the first safe point.
//
// ============================================================================
// WHY THIS EXISTS A SECOND TIME
// ============================================================================
// The card has been reported four times as PEMF's doing. A shim added for the
// third report logged our city lookup against the engine's, found them
// agreeing, and the card was declared stock behaviour.
//
// ⚠️ THAT MEASUREMENT WAS TAKEN IN OPEN WATER. The report it was answering was
// the card firing "while moored beside Santo Domingo". It sampled the scenario
// that was already fine and proved the engine right there, which says nothing
// about the scenario complained of. A measurement only closes the case it
// actually sampled -- and two independent numbers agreeing reads far more
// conclusive than it is.
//
// So this one is built to answer the question that was actually asked, and to
// distinguish the only two ways the card can be wrong:
//
//   (1) THE DISTANCE IS WRONG. Moored at a port and the gate still computes
//       "far". That is corruption, and it is what everyone pictures.
//   (2) THE DISTANCE IS RIGHT AND IT ASKS TOO OFTEN. After speaking, the card
//       is silent for 99999 ticks. If that cooldown is being eaten you get a
//       correct card over and over, which feels identical from the deck.
//
// ⛔ DO NOT "FIX" THE GATE BEFORE THIS SAYS WHICH. Suppressing a card the engine
// was right to show is the mistake that deleted the last shim.
//
// ============================================================================
// THE MECHANISM (docs/GAME_API.md has the full writeup)
// ============================================================================
// Sailing loop tail, 0x00473749 onward -- a timer, not geometry:
//
//     INC  [0x0085A154]                            ; one per loop pass
//     CMP  [0x0085A154], ([0x0085A158]+1)*500 + [0x008B98D4]
//     CALL 0x0045B890                              ; -> the card
//
// and inside 0x0045B890:
//
//     [0x008B98D4] = tick + 250                    ; ALWAYS, before the gates
//     if ([0x0085A158] == 0                        ; gate 1
//         && FUN_00405D90() > 11                   ; gate 2, the distance
//         && [0x0085A164] >= 0)                    ; gate 3, view flags
//         [0x008B98D4] = tick + 99999              ; only when it SPEAKS
//
// So a jump in [0x008B98D4] of far more than 250 is the card having spoken.
// That is how this detects it without hooking anything: no .text write, no
// trampoline, nothing to race the Steam DRM's checksum.
#pragma once
#include <windows.h>

#include <cstdio>

#include "log.h"
#include "game.h"
#include "render.h"   // RedirectCall

namespace sailmaster {

constexpr uintptr_t kTick  = 0x0085A154;   // int, ++ per sailing loop pass
constexpr uintptr_t kGate  = 0x0085A158;   // int, card disabled unless 0
constexpr uintptr_t kDue   = 0x008B98D4;   // int, next-prompt-due baseline
constexpr uintptr_t kFlags = 0x0085A164;   // int, must be >= 0

// ⚠️ NOT addr::CityPositions. The card's distance function reads a DIFFERENT
// table from the one the rest of PEMF uses:
//
//     FUN_00405D90   0x00860B68 + c*32,  x@+0 y@+4,  multiplied by 1000
//     CityDistance   0x0085B170 + c*16,  x@+0 y@+4,  as-is
//
// If those two ever disagree, the card is working from coordinates nothing else
// in the game agrees with -- which would be the whole bug, and no amount of
// checking our own numbers would ever have found it. Both are logged.
constexpr uintptr_t kCardCityPos = 0x00860B68;
constexpr int       kCardCityStride = 32;

inline bool  g_primed = false;
inline int   g_prevDue = 0;
inline int   g_prevTick = 0;
inline DWORD g_prevMs = 0;
inline DWORD g_lastFireMs = 0;
inline int   g_fires = 0;

// FUN_00405D90 reproduced exactly, from the disassembly rather than the
// decompiler -- the decompiled form looks like it has a scale bug (ship coords
// divided by 1000, city coords multiplied by 1000) and does not: the table
// stores city positions in thousands of map units.
inline int CardDistance(int cityIndex)
{
    if (cityIndex < 0 || cityIndex >= (int)game::addr::kMaxCities) return -1;
    const int* pos = (const int*)(kCardCityPos +
                                  (uintptr_t)cityIndex * kCardCityStride);

    // City coordinates here are thousands of map units, so a real one is a few
    // hundred -- the map tops out around 215,000 map units. Anything outside
    // that is an unused slot, and multiplying it by 1000 would overflow and
    // hand the scan below a bogus winner. The engine never sees these because
    // its own search filters on the record flags first; we are walking the
    // table raw, so we filter on the only thing we can trust.
    if (pos[0] < 0 || pos[0] > 100000 || pos[1] < 0 || pos[1] > 100000)
        return -1;

    int dx = game::PlayerX() / 1000 - pos[0] * 1000;
    int dy = game::PlayerY() / 1000 - pos[1] * 1000;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;

    // min + max*2, then /2, then /1000 -- the engine's octagonal approximation.
    const int d = (dx <= dy) ? (dx + dy * 2) : (dy + dx * 2);
    return (d / 2) / 1000;
}

// The nearest port by the card's own table and arithmetic, which is the only
// measure that decides whether it speaks.
inline int CardNearest(int* outDist)
{
    int best = -1, bestD = 0x7FFFFFFF;
    for (int c = 0; c < (int)game::addr::kMaxCities; ++c) {
        const int d = CardDistance(c);
        if (d < 0) continue;
        if (d < bestD) { bestD = d; best = c; }
    }
    if (outDist) *outDist = (best < 0) ? -1 : bestD;
    return best;
}

// Called from the safe point. Reads only.
inline void Poll()
{
    const DWORD now = GetTickCount();

    int tick, due, gate, flags;
    __try {
        tick  = *(const int*)kTick;
        due   = *(const int*)kDue;
        gate  = *(const int*)kGate;
        flags = *(const int*)kFlags;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    if (!g_primed) {
        g_primed   = true;
        g_prevDue  = due;
        g_prevTick = tick;
        g_prevMs   = now;
        Log("sailmaster: watching -- tick=%d due=%d gate(0x85A158)=%d "
            "flags(0x85A164)=0x%08X%s",
            tick, due, gate, (unsigned)flags,
            gate != 0 ? " [gate != 0: the card is disabled entirely]" : "");
        return;
    }

    // ---- the cooldown being thrown away -------------------------------------
    // 0x0085A154 is a MONOTONIC game clock -- 0x0047246D only ever advances it,
    // in a catch-up loop, and nothing resets it. So the 99999-tick silence is
    // held entirely in 0x008B98D4, and 0x00471F41 writes that on entering the
    // sailing view.
    //
    // If that write is a zero, the silence is worthless: the threshold becomes
    // 0 + 500, the clock passed 500 long ago, and the card fires on the first
    // pass after every single return to the sailing view. Near a port the
    // distance gate still hides it -- which is why this has been so hard to
    // pin -- but ALREADY out at sea, every battle, port menu or anything else
    // that re-enters the view would bring it straight back.
    //
    // A reset is the one thing that makes `due` go DOWN, so it is cheap to see.
    if (due < g_prevDue) {
        Log("sailmaster: ⛔ COOLDOWN RESET -- due %d -> %d while tick=%d. "
            "Threshold is now %d, and the clock is already past it, so the card "
            "is free to speak again immediately.",
            g_prevDue, due, tick, due + 500);
    }

    // The card speaks -> due jumps to tick+99999. Staying quiet -> tick+250.
    // Anything past halfway between them is unambiguously a fire.
    const bool spoke = (due - tick) > 50000 && (g_prevDue - g_prevTick) <= 50000;

    if (spoke) {
        ++g_fires;

        // Ticks per second, measured over the window since the last poll pair.
        // This is the number that decides between "too often" and "correct":
        // the cooldown is 99999 TICKS, so how long that is in seconds depends
        // entirely on the rate, and nothing has ever measured it.
        const DWORD elapsed = (now > g_prevMs) ? (now - g_prevMs) : 1;
        const double rate   = (double)(tick - g_prevTick) * 1000.0 / (double)elapsed;

        int cardD = -1;
        const int cardCity = CardNearest(&cardD);

        // ...and the same question asked of the table PEMF and the rest of the
        // game use. Disagreement here is the finding.
        const int ourCity = game::NearestCity(99999);
        const int ourD    = (ourCity >= 0) ? game::CityDistance(ourCity) : -1;

        Log("sailmaster: CARD SPOKE (#%d) -- tick=%d due %d -> %d  rate=%.1f/s",
            g_fires, tick, g_prevDue, due, rate);
        Log("sailmaster:   card's own table: nearest city %d at %d (%d leagues), "
            "gate is >11 (60 leagues)", cardCity, cardD, cardD * 5);
        Log("sailmaster:   PEMF's table:     nearest city %d at %d",
            ourCity, ourD);

        if (g_lastFireMs != 0) {
            const double since = (double)(now - g_lastFireMs) / 1000.0;
            Log("sailmaster:   %.1fs since the last one. The cooldown is 99999 "
                "ticks, so at %.1f/s it should be ~%.0fs.",
                since, rate, rate > 0.01 ? 99999.0 / rate : -1.0);
            if (rate > 0.01 && since < (99999.0 / rate) * 0.5)
                Log("sailmaster:   ⛔ TOO SOON -- something is eating the "
                    "cooldown. The distance is not the bug.");
        }

        if (cardD >= 0 && cardD <= 11)
            Log("sailmaster:   ⛔ SPOKE WHILE INSIDE THE GATE (%d <= 11). The "
                "distance the card used is not the one this reads.", cardD);

        if (ourCity >= 0 && cardCity >= 0 && ourCity != cardCity)
            Log("sailmaster:   ⚠️ the two tables disagree about which port is "
                "nearest (%d vs %d) -- suspect the city tables, not the timer.",
                cardCity, ourCity);

        g_lastFireMs = now;
    }

    g_prevDue  = due;
    g_prevTick = tick;
    g_prevMs   = now;
}

// ============================================================================
// THE POLICY -- how far out before the sailing master speaks
// ============================================================================
// Settled 2026-10-16 (journal): the card is right by its own rule -- the
// playtest log had the ship go from 3 to 13.8 units off the nearest port in 80
// seconds, a storm helping, and it spoke at 12 (60 leagues). The complaint is
// the rule, not the arithmetic: 60 leagues from a PORT is easy to be within
// sight of islands, and players read the card as "you hit the edge of the
// map". So the threshold is a setting.
//
// FUN_0045B890 takes no arguments, saves its own registers, and its result is
// not used (the next instruction is an unrelated compare). Its quiet path is a
// single store, `[0x008B98D4] = tick + 250` (0x0045B8A1); the replacement
// stays quiet the same way, so the timer behaves exactly as when the game's own
// distance gate holds it back.
constexpr uintptr_t kCardCall = 0x0047376D;   // E8 -> kCardFn
constexpr uintptr_t kCardFn   = 0x0045B890;
constexpr int       kQuietRearm = 250;

struct Policy {
    int mode    = 1;     // 0 = never, 1 = speak when far enough out
    int leagues = 150;   // stock is 60; the engine's own gate still applies
};
inline Policy g_policy;
inline void*  g_cardOrig = nullptr;
inline LONG   g_held = 0;

inline void LoadPolicy(const char* gameDir)
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\PEMF\\storms.ini", gameDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") == 0 && f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char key[64] = {0};
            int v = 0;
            if (sscanf_s(line, " %63[A-Za-z_] = %d", key, (unsigned)sizeof(key), &v) != 2) continue;
            if      (_stricmp(key, "farOutCard")    == 0) g_policy.mode    = v != 0;
            else if (_stricmp(key, "farOutLeagues") == 0) g_policy.leagues = v < 60 ? 60 : v;
        }
        fclose(f);
    }
    Log("sailmaster: the far-out-to-sea card %s",
        g_policy.mode == 0 ? "is OFF" : "speaks beyond the stock 60 leagues only if also");
    if (g_policy.mode) Log("sailmaster:   at least %d leagues from the nearest port", g_policy.leagues);
}

} // namespace sailmaster

extern "C" void __cdecl PemfFarOutCard(void)
{
    using namespace sailmaster;
    bool speak = g_policy.mode != 0;
    int d = -1;
    if (speak && g_policy.leagues > 60) {
        __try { CardNearest(&d); } __except (EXCEPTION_EXECUTE_HANDLER) { d = -1; }
        speak = d >= 0 && d * 5 >= g_policy.leagues;
    }
    if (speak) { ((void(__cdecl*)(void))g_cardOrig)(); return; }

    __try { *(int*)kDue = *(const int*)kTick + kQuietRearm; }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
    if (InterlockedIncrement(&g_held) % 40 == 1 && d >= 0 && d * 5 >= 60)
        Log("sailmaster: held the card -- %d leagues out, setting is %d", d * 5, g_policy.leagues);
}

namespace sailmaster {

// ⛔ First safe point only (a code write).
inline bool InstallPolicy()
{
    if (g_cardOrig) return true;
    if (g_policy.mode == 1 && g_policy.leagues <= 60) return false;   // stock: nothing to do
    const uintptr_t s = kCardCall;
    __try {
        if (*(const uint8_t*)s != 0xE8 || s + 5 + *(const int32_t*)(s + 1) != kCardFn) {
            Log("sailmaster: card call at 0x%08X is not what we expect -- stock card", (unsigned)s);
            return false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    void* prev = nullptr;
    if (!render::RedirectCall(s, (void*)&PemfFarOutCard, &prev)) return false;
    g_cardOrig = prev;
    Log("sailmaster: card policy installed");
    return true;
}

inline void RestorePolicy()
{
    if (!g_cardOrig) return;
    render::RedirectCall(kCardCall, g_cardOrig, nullptr);
    g_cardOrig = nullptr;
}

} // namespace sailmaster
