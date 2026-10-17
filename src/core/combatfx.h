// combatfx.h - a louder, messier sea battle.
//
// Everything here ADDS to the game's own effects through the game's own
// functions; nothing it draws is replaced or removed.
//
// ============================================================================
// WHAT THE BATTLE ALREADY HAS (decompiled, sea battle FUN_0047A040)
// ============================================================================
//   FUN_00476F10(type, x, y, height, f5, f6)   cdecl -- FLYING DEBRIS. A pool of
//       40 pieces at 0x008B99D8 (0x40 bytes each); a random slot is taken and the
//       call does nothing if it is busy. type 0/1 = a crewman in the water (with
//       a splash), 2 = cannon_barrel.nif, 3 = cannon_truck.nif (a gun carriage),
//       4 = barrel_01.nif. Each piece gets a random velocity and spin.
//       A hit throws ONE piece, sometimes: calls at 0x0047DE17 (crewman),
//       0x0047DE91 (type 2), 0x0047E11B (type 4).
//   FUN_00477CB0(side, ship, x, y)             -- fires ONE shot; called once per
//       gun of a broadside at 0x0047E4E6. side 0 is the player.
//   0x0047E56D  mov edx, 0x46 -- the size handed to the draw of the smoke and
//       flash that follows each shot (FUN_004BBC10, edx).
//   FUN_004AB290  a TIMED EFFECT: 12 arguments, callee-cleaned (ret 0x30), the
//       list in EDI. A node (0x5C bytes) holds the effect object (arg 1), its
//       position (args 3, 4, 5), facing (6), a lifetime (arg 10, the effect's own
//       length) and a playback RATE (arg 11, percent; +0x2C = rate/100).
//       FUN_004AB530 ages every node by dt * rate and removes it when its age
//       passes the lifetime -- so a lower rate plays the effect slower and
//       keeps it on screen longer.
//       The battle's cannon-hit blowout is one of these: 0x0047DF65..0x0047E044
//       picks one of three effects from 0x008E66B4 (fx_cannon_hit_blowout01/02
//       .nif and a third, loaded at 0x00480B26..0x00480C72), pushes rate 0x64
//       at 0x0047DF75 (6A 64), and adds it to the list at 0x007268DC.
//   0x008CAB70..88 -- the battle camera shake: amplitudes (4 floats), start
//       (game clock 0x008C8494), duration ms, active. The game shakes it only
//       when the player's ship is struck.
//
// PEMF: each struck hull throws extra pieces too; the player's broadside kicks
// the camera; the smoke and flash after each shot are larger; each blowout
// plays slower (it lingers) and brings more blowouts with it.
#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "render.h"   // RedirectCall

namespace combatfx {

constexpr uintptr_t kDebrisFn     = 0x00476F10;
constexpr uintptr_t kDebrisCalls[] = { 0x0047DE17, 0x0047DE91, 0x0047E11B };
constexpr uintptr_t kShotFn       = 0x00477CB0;
constexpr uintptr_t kShotCall     = 0x0047E4E6;
constexpr uintptr_t kSmokeSizeImm = 0x0047E56E;   // the 0x46 of BA 46 00 00 00 at 0x0047E56D
constexpr int       kSmokeSizeStock = 0x46;

constexpr uintptr_t kTimedFxFn    = 0x004AB290;
constexpr uintptr_t kBlowoutCall  = 0x0047E044;
constexpr uintptr_t kBlowoutRateImm = 0x0047DF76;  // the 0x64 of 6A 64 at 0x0047DF75
constexpr int       kBlowoutRateStock = 0x64;

constexpr uintptr_t kShake        = 0x008CAB70;   // float x4, int start, int ms, int active
constexpr uintptr_t kGameClock    = 0x008C8494;

struct Settings {
    int enabled      = 1;
    int extraDebris  = 8;     // extra pieces thrown per hit, 0..12
    int debrisSpread = 350;   // how far apart they start, battle units
    int crewOverboard = 25;   // percent of extra pieces that are a man in the water (with splash)
    int muzzleSize   = 260;   // percent of the stock smoke and flash after each shot
    int broadsideKick = 55;   // camera kick when YOUR broadside fires, percent of a hit; 0 = none
    int kickMs       = 240;
    int hitKick      = 20;    // a small jolt whenever any shot strikes a hull; 0 = none
    int smokeLinger  = 180;   // how long each hit's smoke burst stays, percent of stock (100..300)
    int extraBlowouts = 2;    // more smoke bursts with each one, 0..4
    int blowoutSpread = 6;    // how far apart they start, in the effect's units
};
inline Settings g_set;
inline bool  g_installed = false;
inline void* g_debrisOrig = nullptr;
inline bool  g_smokePatched = false;
inline bool  g_lingerPatched = false;
inline int   g_blowoutRate = kBlowoutRateStock;
inline DWORD g_lastKick = 0;

inline void LoadSettings(const char* gameDir)
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\PEMF\\combat.ini", gameDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") == 0 && f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char key[64] = {0};
            int v = 0;
            if (sscanf_s(line, " %63[A-Za-z_] = %d", key, (unsigned)sizeof(key), &v) != 2) continue;
            struct E { const char* n; int* p; int lo, hi; };
            const E t[] = {
                { "enabled", &g_set.enabled, 0, 1 },
                { "extraDebris", &g_set.extraDebris, 0, 12 },
                { "smokeLinger", &g_set.smokeLinger, 100, 300 },
                { "extraBlowouts", &g_set.extraBlowouts, 0, 4 },
                { "blowoutSpread", &g_set.blowoutSpread, 0, 100 },
                { "crewOverboard", &g_set.crewOverboard, 0, 100 },
                { "hitKick", &g_set.hitKick, 0, 100 },
                { "debrisSpread", &g_set.debrisSpread, 0, 2000 },
                { "muzzleSize", &g_set.muzzleSize, 50, 300 },
                { "broadsideKick", &g_set.broadsideKick, 0, 100 },
                { "kickMs", &g_set.kickMs, 50, 1000 },
            };
            for (const E& e : t)
                if (_stricmp(key, e.n) == 0) { *e.p = v < e.lo ? e.lo : (v > e.hi ? e.hi : v); break; }
        }
        fclose(f);
    }
    Log("combatfx: %s -- +%d debris per hit, smoke %d%%, bursts +%d lasting %d%%, broadside kick %d%%",
        g_set.enabled ? "on" : "OFF", g_set.extraDebris, g_set.muzzleSize, g_set.extraBlowouts,
        g_set.smokeLinger, g_set.broadsideKick);
}

inline int Jitter(int spread) { return spread ? (rand() % (2 * spread + 1)) - spread : 0; }

// A short camera kick, never weaker than one already running.
inline void Kick(float amp, int ms)
{
    __try {
        float* a = (float*)kShake;
        int* start  = (int*)(kShake + 0x10);
        int* dur    = (int*)(kShake + 0x14);
        int* active = (int*)(kShake + 0x18);
        const int now = *(const int*)kGameClock;
        if (*active && now - *start < *dur && a[0] >= amp) return;
        a[0] = a[1] = a[2] = a[3] = amp;
        *start = now;
        *dur = ms;
        *active = 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

} // namespace combatfx

extern "C" {

void* g_pemfShotOrig = nullptr;

// Same contract as FUN_00476F10: cdecl, six arguments. The game's own piece
// first, then ours: cannon barrels, gun carriages and casks, scattered.
void __cdecl PemfHitDebris(int type, int x, int y, int h, int f5, int f6)
{
    using namespace combatfx;
    typedef void(__cdecl* DebrisFn)(int, int, int, int, int, int);
    const DebrisFn spawn = (DebrisFn)g_debrisOrig;
    spawn(type, x, y, h, f5, f6);
    if (!g_set.enabled) return;
    for (int i = 0; i < g_set.extraDebris; ++i) {
        // Mostly wreckage -- barrels, gun carriages, casks -- and now and then a
        // man thrown into the sea, which brings the game's own splash with it.
        const int t = (rand() % 100 < g_set.crewOverboard) ? 1 : 2 + rand() % 3;
        spawn(t, x + Jitter(g_set.debrisSpread), y + Jitter(g_set.debrisSpread), h, 0, 0);
    }
    if (g_set.hitKick > 0) Kick(g_set.hitKick / 100.f, 120);
}

// Each shot of a broadside: the player's first gun kicks the camera.
void __cdecl PemfOnShot(int side)
{
    using namespace combatfx;
    if (!g_set.enabled || side != 0 || g_set.broadsideKick <= 0) return;
    const DWORD now = GetTickCount();
    if (now - g_lastKick < 400) return;            // one kick per broadside
    g_lastKick = now;
    Kick(g_set.broadsideKick / 100.f, g_set.kickMs);
}

void* g_pemfBlowoutOrig = nullptr;
int   g_pemfBlowoutList = 0;     // EDI at the call: the timed-effect list

// FUN_004AB290 with the list in EDI and its twelve arguments, which it pops.
static void* CallTimedFx(const int* a)
{
    void* r = nullptr;
    void* fn = g_pemfBlowoutOrig;
    const int list = g_pemfBlowoutList;
    __asm {
        mov  esi, a
        mov  ecx, 11
    again:
        push dword ptr [esi + ecx*4]
        dec  ecx
        jns  again
        mov  edi, list
        call fn
        mov  r, eax
    }
    return r;
}

// Same contract as FUN_004AB290 (callee-cleaned, twelve arguments). The game's
// blowout first, returned to it untouched; then a few more about it, each
// facing its own way and running at a slightly different pace.
void* __stdcall PemfBlowout(int a0, int a1, int a2, int a3, int a4, int a5,
                            int a6, int a7, int a8, int a9, int a10, int a11)
{
    using namespace combatfx;
    int a[12] = { a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11 };
    void* first = CallTimedFx(a);
    if (!g_set.enabled || !a0) return first;
    __try {
        for (int i = 0; i < g_set.extraBlowouts; ++i) {
            int b[12];
            memcpy(b, a, sizeof(b));
            b[2] += Jitter(g_set.blowoutSpread);
            b[3] += Jitter(g_set.blowoutSpread);
            b[5]  = rand() * 0x10001;                          // any facing
            b[10] = g_blowoutRate * (85 + rand() % 31) / 100;  // 85..115% of the pace
            if (b[10] < 10) b[10] = 10;
            CallTimedFx(b);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return first;
}

__declspec(naked) void PemfBlowoutStub(void)
{
    __asm {
        mov  dword ptr [g_pemfBlowoutList], edi
        jmp  PemfBlowout
    }
}

// Sees the side, then becomes the game's call untouched.
__declspec(naked) void PemfShotStub(void)
{
    __asm {
        push eax
        push ecx
        push edx
        push dword ptr [esp + 0x10]    // side: the first argument, past our pushes and the return
        call PemfOnShot
        add  esp, 4
        pop  edx
        pop  ecx
        pop  eax
        jmp  dword ptr [g_pemfShotOrig]
    }
}

} // extern "C"

namespace combatfx {

// ⛔ First safe point only (a code write). Checked first, all of it.
inline bool Install()
{
    if (g_installed || !g_set.enabled) return g_installed;
    __try {
        for (uintptr_t s : kDebrisCalls)
            if (*(const uint8_t*)s != 0xE8 || s + 5 + *(const int32_t*)(s + 1) != kDebrisFn) {
                Log("combatfx: debris call at 0x%08X is not what we expect -- off", (unsigned)s);
                return false;
            }
        if (*(const uint8_t*)kShotCall != 0xE8 ||
            kShotCall + 5 + *(const int32_t*)(kShotCall + 1) != kShotFn) {
            Log("combatfx: shot call at 0x%08X is not what we expect -- off", (unsigned)kShotCall);
            return false;
        }
        if (*(const uint8_t*)kBlowoutCall != 0xE8 ||
            kBlowoutCall + 5 + *(const int32_t*)(kBlowoutCall + 1) != kTimedFxFn ||
            *(const uint8_t*)(kBlowoutRateImm - 1) != 0x6A ||
            *(const uint8_t*)kBlowoutRateImm != kBlowoutRateStock) {
            Log("combatfx: blowout call at 0x%08X is not what we expect -- off", (unsigned)kBlowoutCall);
            return false;
        }
        if (*(const uint8_t*)(kSmokeSizeImm - 1) != 0xBA || *(const int32_t*)kSmokeSizeImm != kSmokeSizeStock) {
            Log("combatfx: smoke size at 0x%08X is not what we expect -- off", (unsigned)kSmokeSizeImm);
            return false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

    void* prev = nullptr;
    for (uintptr_t s : kDebrisCalls) render::RedirectCall(s, (void*)&PemfHitDebris, &prev);
    g_debrisOrig = (void*)kDebrisFn;
    render::RedirectCall(kShotCall, (void*)&PemfShotStub, &prev);
    g_pemfShotOrig = (void*)kShotFn;
    render::RedirectCall(kBlowoutCall, (void*)&PemfBlowoutStub, &prev);
    g_pemfBlowoutOrig = (void*)kTimedFxFn;

    // Slower playback, longer on screen: rate = stock * 100 / linger.
    g_blowoutRate = kBlowoutRateStock * 100 / g_set.smokeLinger;
    if (g_blowoutRate < 10) g_blowoutRate = 10;
    {
        DWORD o = 0;
        if (VirtualProtect((void*)kBlowoutRateImm, 1, PAGE_EXECUTE_READWRITE, &o)) {
            *(uint8_t*)kBlowoutRateImm = (uint8_t)g_blowoutRate;
            VirtualProtect((void*)kBlowoutRateImm, 1, o, &o);
            FlushInstructionCache(GetCurrentProcess(), (void*)kBlowoutRateImm, 1);
            g_lingerPatched = true;
        }
    }

    const int size = (kSmokeSizeStock * g_set.muzzleSize) / 100;
    DWORD old = 0;
    if (VirtualProtect((void*)kSmokeSizeImm, 4, PAGE_EXECUTE_READWRITE, &old)) {
        *(int32_t*)kSmokeSizeImm = size;
        VirtualProtect((void*)kSmokeSizeImm, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)kSmokeSizeImm, 4);
        g_smokePatched = true;
    }
    g_installed = true;
    Log("combatfx: installed -- 3 hit-debris calls, the broadside shot call, smoke size %d -> %d, "
        "blowout rate %d -> %d (+%d each)", kSmokeSizeStock, size, kBlowoutRateStock, g_blowoutRate,
        g_set.extraBlowouts);
    return true;
}

inline void Restore()
{
    if (!g_installed) return;
    for (uintptr_t s : kDebrisCalls) render::RedirectCall(s, (void*)kDebrisFn, nullptr);
    render::RedirectCall(kShotCall, (void*)kShotFn, nullptr);
    render::RedirectCall(kBlowoutCall, (void*)kTimedFxFn, nullptr);
    if (g_lingerPatched) {
        DWORD o = 0;
        if (VirtualProtect((void*)kBlowoutRateImm, 1, PAGE_EXECUTE_READWRITE, &o)) {
            *(uint8_t*)kBlowoutRateImm = (uint8_t)kBlowoutRateStock;
            VirtualProtect((void*)kBlowoutRateImm, 1, o, &o);
            FlushInstructionCache(GetCurrentProcess(), (void*)kBlowoutRateImm, 1);
        }
        g_lingerPatched = false;
    }
    if (g_smokePatched) {
        DWORD old = 0;
        if (VirtualProtect((void*)kSmokeSizeImm, 4, PAGE_EXECUTE_READWRITE, &old)) {
            *(int32_t*)kSmokeSizeImm = kSmokeSizeStock;
            VirtualProtect((void*)kSmokeSizeImm, 4, old, &old);
            FlushInstructionCache(GetCurrentProcess(), (void*)kSmokeSizeImm, 4);
        }
        g_smokePatched = false;
    }
    g_installed = false;
}

} // namespace combatfx
