// market.h - a living market on top of the game's own prices.
//
// The game's prices are left exactly as it computes them; PEMF ADDS movement
// on top: shortages and gluts that come and go around a port, and a premium on
// food and cannon in towns whose crown is at war.
//
// ============================================================================
// WHERE PRICES COME FROM (decompiled, FUN_00428570)
// ============================================================================
// FUN_00428570(city), cdecl, one argument, computes the town's price table:
// int16 at 0x0085BFB4 + city*0x94 + good*2, good 0..6 = Gold, Food, Luxuries,
// Goods, Spice, Sugar, Cannon ([ITEM] in text.ini; gold's entry is always 1).
// Inputs are the town's size (0x00860B75), wealth (0x00860B7C) and the world
// averages (0x00861B70/74, rebuilt by FUN_00428A30); a switch on city index
// already gives some ports their specialties. It is called fresh before the
// numbers are used:
//
//     0x0041010B  E8 60 84 01 00   the merchant
//     0x004110B2  E8 B9 74 01 00   the town menu
//     0x00428E0D  E8 5E F7 FF FF   the world recompute
//     0x0044E8C7  E8 A4 9C FD FF   (add esp,4 after)
//
// PEMF redirects those four calls to a function that runs the original and
// then scales the result. Nothing else in the game is touched.
#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "log.h"
#include "game.h"
#include "nations.h"
#include "render.h"   // RedirectCall
#include "suspicion.h"   // Say

namespace market {

constexpr uintptr_t kPriceFn    = 0x00428570;
constexpr uintptr_t kPriceCalls[] = { 0x0041010B, 0x004110B2, 0x00428E0D, 0x0044E8C7 };
constexpr uintptr_t kPrices     = 0x0085BFB4;   // int16[7] per city, stride 0x94
constexpr int       kCityStride = 0x94;
constexpr int       kGoods      = 7;
inline const char* const kGoodName[kGoods] = {
    "gold", "food", "luxuries", "goods", "spice", "sugar", "cannon" };

struct Settings {
    int enabled      = 1;
    int eventMinSec  = 240;    // between market news, at sea
    int eventMaxSec  = 600;
    int eventLastMin = 600;    // how long a shortage or glut lasts, seconds
    int eventLastMax = 1500;
    int shortageMin  = 40;     // percent dearer
    int shortageMax  = 90;
    int glutMin      = 25;     // percent cheaper
    int glutMax      = 50;
    int radius       = 60000;  // city units around the port it starts at
    int warFood      = 25;     // percent dearer in a town whose crown is at war
    int warCannon    = 40;
};
inline Settings g_set;

struct Event {
    bool  active = false;
    int   good = 0;
    int   city = -1;
    int   pct = 0;             // +dearer / -cheaper
    DWORD endsAt = 0;
};
constexpr int kMaxEvents = 3;
inline Event g_events[kMaxEvents];
inline DWORD g_nextAt = 0;
inline bool  g_installed = false;
inline void* g_orig = nullptr;

inline void LoadSettings(const char* gameDir)
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\PEMF\\market.ini", gameDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") == 0 && f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char key[64] = {0};
            int v = 0;
            if (sscanf_s(line, " %63[A-Za-z_] = %d", key, (unsigned)sizeof(key), &v) != 2) continue;
            struct E { const char* n; int* p; };
            const E t[] = {
                { "enabled", &g_set.enabled }, { "eventMinSec", &g_set.eventMinSec },
                { "eventMaxSec", &g_set.eventMaxSec }, { "eventLastMin", &g_set.eventLastMin },
                { "eventLastMax", &g_set.eventLastMax }, { "shortageMin", &g_set.shortageMin },
                { "shortageMax", &g_set.shortageMax }, { "glutMin", &g_set.glutMin },
                { "glutMax", &g_set.glutMax }, { "radius", &g_set.radius },
                { "warFood", &g_set.warFood }, { "warCannon", &g_set.warCannon },
            };
            for (const E& e : t) if (_stricmp(key, e.n) == 0) { *e.p = v < 0 ? 0 : v; break; }
        }
        fclose(f);
    }
    if (g_set.eventMaxSec < g_set.eventMinSec) g_set.eventMaxSec = g_set.eventMinSec;
    if (g_set.eventLastMax < g_set.eventLastMin) g_set.eventLastMax = g_set.eventLastMin;
    if (g_set.shortageMax < g_set.shortageMin) g_set.shortageMax = g_set.shortageMin;
    if (g_set.glutMax < g_set.glutMin) g_set.glutMax = g_set.glutMin;
    if (g_set.glutMax > 90) g_set.glutMax = 90;
    Log("market: %s -- news every %d-%d s, lasting %d-%d s; war +%d%% food, +%d%% cannon",
        g_set.enabled ? "on" : "OFF", g_set.eventMinSec, g_set.eventMaxSec,
        g_set.eventLastMin, g_set.eventLastMax, g_set.warFood, g_set.warCannon);
}

inline int RandIn(int lo, int hi) { return hi <= lo ? lo : lo + rand() % (hi - lo + 1); }

inline int CityDist(int a, int b)
{
    const int* pa = (const int*)(game::addr::CityPositions + (size_t)a * 16);
    const int* pb = (const int*)(game::addr::CityPositions + (size_t)b * 16);
    int dx = pa[0] - pb[0], dy = pa[1] - pb[1];
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return ((dy < dx) ? dy + dx * 2 : dx + dy * 2) / 2;
}

inline bool AtWarWithAnyone(int nation)
{
    if (nation < 0 || nation >= game::addr::kNationsWithRank) return false;
    for (int o = 0; o < game::addr::kNationsWithRank; ++o)
        if (o != nation && nations::AtWar(nation, o)) return true;
    return false;
}

// The percentage PEMF adds to one good in one town, all causes together.
inline int Adjust(int city, int good)
{
    int pct = 0;
    const DWORD now = GetTickCount();
    for (const Event& e : g_events) {
        if (!e.active || e.good != good || (int)(now - e.endsAt) >= 0) continue;
        if (e.city == city || CityDist(e.city, city) <= g_set.radius) pct += e.pct;
    }
    if (AtWarWithAnyone(game::CityNation(city))) {
        if (good == 1) pct += g_set.warFood;
        if (good == 6) pct += g_set.warCannon;
    }
    if (pct < -90) pct = -90;
    return pct;
}

// Runs right after the game has written a town's prices.
inline void Apply(int city)
{
    if (!g_set.enabled || city < 0 || city >= game::addr::kMaxCities) return;
    short* p = (short*)(kPrices + (uintptr_t)city * kCityStride);
    for (int g = 1; g < kGoods; ++g) {
        const int pct = Adjust(city, g);
        if (pct == 0) continue;
        int v = (p[g] * (100 + pct) + 50) / 100;
        if (v < 1) v = 1;
        if (v > 30000) v = 30000;
        // The proof the table is what the merchant charges: compare these
        // lines with his screen. Rate-limited -- the world recompute runs this
        // for every town at once.
        static LONG logged = 0;
        if (InterlockedIncrement(&logged) <= 40)
            Log("market: city %d %s %d -> %d (%+d%%)", city, kGoodName[g], p[g], v, pct);
        p[g] = (short)v;
    }
}

// A real port of the crowns: one with a position and a nation.
inline bool IsTradePort(int c)
{
    const int* pos = (const int*)(game::addr::CityPositions + (size_t)c * 16);
    if (pos[0] <= 0 || pos[1] <= 0) return false;
    const int n = game::CityNation(c);
    return n >= 0 && n < game::addr::kNationsWithRank;
}

// Once a second at the safe point: news, now and then, while sailing.
inline void Tick(bool sailing)
{
    if (!g_set.enabled || !g_installed) return;
    const DWORD now = GetTickCount();
    for (Event& e : g_events)
        if (e.active && (int)(now - e.endsAt) >= 0) {
            Log("market: the %s %s around city %d is over", kGoodName[e.good],
                e.pct > 0 ? "shortage" : "glut", e.city);
            e.active = false;
        }
    if (!sailing) return;
    if (!g_nextAt) { g_nextAt = now + (DWORD)RandIn(g_set.eventMinSec, g_set.eventMaxSec) * 1000; return; }
    if ((int)(now - g_nextAt) < 0) return;
    g_nextAt = now + (DWORD)RandIn(g_set.eventMinSec, g_set.eventMaxSec) * 1000;

    Event* slot = nullptr;
    for (Event& e : g_events) if (!e.active) { slot = &e; break; }
    if (!slot) return;

    int ports[128], n = 0;
    for (int c = 0; c < game::addr::kMaxCities; ++c) if (IsTradePort(c)) ports[n++] = c;
    if (!n) return;

    Event e;
    e.active = true;
    e.city = ports[rand() % n];
    e.good = RandIn(1, 5);                      // the trade goods, not cannon
    const bool shortage = (rand() % 2) == 0;
    e.pct = shortage ? RandIn(g_set.shortageMin, g_set.shortageMax)
                     : -RandIn(g_set.glutMin, g_set.glutMax);
    e.endsAt = now + (DWORD)RandIn(g_set.eventLastMin, g_set.eventLastMax) * 1000;
    *slot = e;

    // The port by name, through the engine's own @CITYNAME (three words).
    char line[192];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                shortage ? "Word in the taverns: %s is scarce around @CITYNAME -- prices are up."
                         : "Word in the taverns: a glut of %s around @CITYNAME -- prices are down.",
                kGoodName[e.good]);
    int words[3] = {0};
    char msg[256];
    if (game::CityNameWords(e.city, words))
        game::ComposeText(line, words, 3, msg, sizeof(msg));
    else
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "%s", line);
    suspicion::Say(msg, false);
    Log("market: %s %s %+d%% around city %d for %d s", kGoodName[e.good],
        shortage ? "shortage" : "glut", e.pct, e.city, (int)((e.endsAt - now) / 1000));
}

} // namespace market

extern "C" void __cdecl PemfTownPrices(int city)
{
    ((void(__cdecl*)(int))market::g_orig)(city);
    __try { market::Apply(city); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

namespace market {

// ⛔ First safe point only (a code write). All four or none.
inline bool Install()
{
    if (g_installed || !g_set.enabled) return g_installed;
    __try {
        for (uintptr_t s : kPriceCalls)
            if (*(const uint8_t*)s != 0xE8 || s + 5 + *(const int32_t*)(s + 1) != kPriceFn) {
                Log("market: price call at 0x%08X is not what we expect -- stock prices",
                    (unsigned)s);
                return false;
            }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    void* prev = nullptr;
    for (uintptr_t s : kPriceCalls) render::RedirectCall(s, (void*)&PemfTownPrices, &prev);
    g_orig = (void*)kPriceFn;
    g_installed = true;
    Log("market: prices move -- 4 calls to 0x%08X go through PEMF", (unsigned)kPriceFn);
    return true;
}

inline void Restore()
{
    if (!g_installed) return;
    for (uintptr_t s : kPriceCalls) render::RedirectCall(s, (void*)kPriceFn, nullptr);
    g_installed = false;
}

} // namespace market
