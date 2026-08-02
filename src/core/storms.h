// storms.h - bigger, heavier weather on the overworld.
//
// Pirates! draws its storms as instances of `cloud00.nif` placed on the sailing
// map by the overworld tick. Everything about how they LOOK comes down to three
// immediates in one stretch of `FUN_004612B0`, so this is byte patching rather
// than anything clever:
//
// ⚠️ THERE ARE TWO CLOUD SPAWNS AND THEY ARE NOT THE SAME THING. The first
// version of this file scaled the wrong one: `cloud00.nif` is the ORDINARY
// fair-weather cloud, and the storm is a separate prefab registered beside it.
// The symptom was exactly what you would expect and was reported as such --
// "the normal clouds look larger for sure" -- with the storms untouched.
//
//   THE STORM (0x008CCB58):
//     0046374A  LEA  EDX, [EBX + EBX*4]  ; x5   \  together: EBX * 80
//     00463752  SHL  EDX, 0x4            ; x16  /   <-- STORM SCALE
//     0046376E  push 0x12C               ; <-- STORM HEIGHT (z = 300)
//     00463775  MOV  ECX, 0x8CCB58       ; the stormcloud prefab
//     0046377A  CALL FUN_004BBC80
//
//   ORDINARY CLOUDS (0x008CC498):
//     0046392A  MOV  EBX, [0x00725678]
//     0046393A  IMUL EBX, EBX, 0xFA      ; <-- FAIR CLOUD SCALE (x250)
//     0046395D  push 0x1F4               ; <-- FAIR CLOUD HEIGHT (z = 500)
//     00463964  MOV  ECX, 0x8CC498
//     00463973  CMP  EAX, 0x3            ; <-- how many of them
//
// and inside `FUN_004BBC80` the sixth argument lands on the scene node:
//
//   *(float*)(*(int*)(node + 0x1c) + 0x68) = ABS((float)param_6 * 0.01);
//
// so the drawn scale is `[0x00725678] * imm * 0.01`, and `imm` is ours.
//
// ⛔ DO NOT SCALE STORMS BY TOUCHING `0x00725678`. It is a shared world constant
// and the ship AI reads it for engagement distances (`FUN_00467F90` uses
// `DAT_00725678 * 1000` and `* 0x960`). Scaling weather through it would quietly
// change how ships fight. The `0xFA` immediate is the storm-only path, which is
// the whole reason this file patches an operand instead of a global.
//
// ⚠️ This is PURELY VISUAL. Whether a storm actually does more to your ship --
// damage, crew, speed -- lives somewhere else and has not been located. A larger
// cloud is a larger cloud.
#pragma once
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "game.h"

namespace storms {

// ------------------------------------------------------------------- tuning
struct Tuning {
    // Vanilla values. Defaults are a NO-OP on purpose: nothing about the
    // player's game changes until they ask for it.
    int stormScale  = 80;    // storm size    -- EBX * 80 at 0x0046374A
    int stormHeight = 300;   // storm altitude -- the 0x12C at 0x0046376E
    int cloudScale  = 250;   // FAIR-weather cloud size
    int cloudCount  = 3;     // weather SLOTS -- storms and clouds share these
    int cloudHeight = 500;   // fair-weather cloud altitude
    bool enabled    = false;
};

inline Tuning g_tune;

// Sanity rails. A scale of 0 makes storms vanish and a negative one is written
// through `ABS()` at the other end, so neither is a useful thing to allow. The
// upper bounds are generous but finite -- an immediate that overflows the
// multiply would produce a garbage scale rather than a big cloud.
constexpr int kScaleMin = 20,   kScaleMax = 4000;
// ⛔ THREE IS A HARD CEILING, NOT A TASTE JUDGEMENT. The loop at 0x0046354A
// walks weather slots and indexes two parallel arrays:
//
//     0x008B98E4   x positions
//     0x008B98F0   y positions
//
// which are TWELVE BYTES APART -- exactly three dwords. A count of 4 reads and
// writes the x array straight into the y array, and every count above that
// marches further into whatever follows. This knob looks like "how much
// weather" and is really "how far past the end of an array shall we go".
constexpr int kCountMin = 1,    kCountMax = 3;
constexpr int kHeightMin = 50,  kHeightMax = 8000;

// ------------------------------------------------------------------ the sites
namespace addr {
    // THE STORM. `8D 14 9B  C1 E2 04` = lea edx,[ebx+ebx*4] ; shl edx,4.
    // Six bytes, which is exactly the length of `imul edx, ebx, imm32`
    // (`69 D3 imm32`) -- so the pair is replaced wholesale with one instruction
    // that computes the same thing with an operand we control. Same registers,
    // same result, no displacement anywhere else in the function.
    constexpr uintptr_t StormScaleSite  = 0x0046374A;
    constexpr int       kStormScaleImmAt = 2;      // into the rewritten imul

    // `68 2C 01 00 00`     push 0x12C            -- imm32 at +1
    constexpr uintptr_t StormHeightSite = 0x0046376E;
    constexpr int       kStormHeightImmAt = 1;

    // `69 DB FA 00 00 00`  imul ebx, ebx, 0xFA   -- imm32 at +2
    constexpr uintptr_t ScaleSite   = 0x0046393A;
    constexpr int       kScaleImmAt = 2;

    // `83 F8 03`           cmp eax, 3            -- imm8 at +2
    constexpr uintptr_t CountSite   = 0x00463973;
    constexpr int       kCountImmAt = 2;

    // `68 F4 01 00 00`     push 0x1F4            -- imm32 at +1
    constexpr uintptr_t HeightSite  = 0x0046395D;
    constexpr int       kHeightImmAt = 1;
}

// Opcode prefixes only -- the immediates are what we are about to change, so
// they are deliberately NOT part of the signature. Verifying the whole
// instruction would make the patch un-reapplicable and, worse, would make a
// second call look like a failed target check.
constexpr unsigned char kScaleOp[]  = { 0x69, 0xDB };
constexpr unsigned char kCountOp[]  = { 0x83, 0xF8 };
constexpr unsigned char kHeightOp[] = { 0x68 };
constexpr unsigned char kStormHeightOp[] = { 0x68 };

// The storm scale site is the one place we replace whole instructions, so it is
// checked against BOTH forms: the original lea+shl, or our own imul if this is
// a re-apply. Anything else and we leave it alone.
constexpr unsigned char kStormScaleOrig[]  = { 0x8D, 0x14, 0x9B, 0xC1, 0xE2, 0x04 };
constexpr unsigned char kStormScalePatched[] = { 0x69, 0xD3 };

inline bool g_applied = false;

// --------------------------------------------------------------------- io
inline int Clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

inline void LoadTuning(const char* gameDir)
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\PEMF\\storms.ini", gameDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) {
        Log("storms: no %s -- weather left as the game ships it", path);
        return;
    }

    char line[256];
    int applied = 0;
    while (fgets(line, sizeof(line), f)) {
        char key[64] = {0};
        int  value = 0;
        if (sscanf_s(line, " %63[A-Za-z_] = %d", key, (unsigned)sizeof(key),
                     &value) != 2) {
            continue;
        }
        if      (_stricmp(key, "stormScale")  == 0) { g_tune.stormScale  = value; ++applied; }
        else if (_stricmp(key, "stormHeight") == 0) { g_tune.stormHeight = value; ++applied; }
        else if (_stricmp(key, "cloudScale")  == 0) { g_tune.cloudScale  = value; ++applied; }
        else if (_stricmp(key, "cloudCount")  == 0) { g_tune.cloudCount  = value; ++applied; }
        else if (_stricmp(key, "cloudHeight") == 0) { g_tune.cloudHeight = value; ++applied; }
        else if (_stricmp(key, "enabled")     == 0) { g_tune.enabled = (value != 0); ++applied; }
    }
    fclose(f);

    g_tune.stormScale  = Clamp(g_tune.stormScale,  kScaleMin,  kScaleMax);
    g_tune.stormHeight = Clamp(g_tune.stormHeight, kHeightMin, kHeightMax);
    g_tune.cloudScale  = Clamp(g_tune.cloudScale,  kScaleMin,  kScaleMax);
    g_tune.cloudCount  = Clamp(g_tune.cloudCount,  kCountMin,  kCountMax);
    g_tune.cloudHeight = Clamp(g_tune.cloudHeight, kHeightMin, kHeightMax);

    Log("storms: loaded %d setting(s) from %s -- STORM scale %d height %d | "
        "fair clouds scale %d count %d height %d%s",
        applied, path, g_tune.stormScale, g_tune.stormHeight,
        g_tune.cloudScale, g_tune.cloudCount, g_tune.cloudHeight,
        g_tune.enabled ? "" : " (DISABLED -- set enabled = 1)");
}

// ------------------------------------------------------------------ patching
inline bool WriteImm32(uintptr_t site, int offset, int value)
{
    DWORD old = 0;
    void* at = (void*)(site + offset);
    if (!VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(int*)at = value;
    VirtualProtect(at, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 4);
    return true;
}

inline bool WriteImm8(uintptr_t site, int offset, int value)
{
    DWORD old = 0;
    void* at = (void*)(site + offset);
    if (!VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(unsigned char*)at = (unsigned char)value;
    VirtualProtect(at, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 1);
    return true;
}

// Rewrite `lea edx,[ebx+ebx*4]; shl edx,4` as `imul edx, ebx, imm32`. Identical
// length (6 bytes), identical registers, identical meaning -- only the constant
// changes. Written as one blob so the instruction is never half-formed if the
// process is interrupted mid-write.
inline bool WriteStormScale(int value)
{
    unsigned char blob[6] = { 0x69, 0xD3, 0, 0, 0, 0 };
    *(int*)(blob + 2) = value;

    DWORD old = 0;
    void* at = (void*)addr::StormScaleSite;
    if (!VirtualProtect(at, sizeof(blob), PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, blob, sizeof(blob));
    VirtualProtect(at, sizeof(blob), old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(blob));
    return true;
}

// Called once the target has verified. Every site is checked by opcode before
// anything is written -- on a host we do not recognise we would rather ship
// vanilla weather than scribble on a random instruction.
inline void Apply()
{
    if (!g_tune.enabled) return;

    const bool okStormScale =
        game::BytesMatch(addr::StormScaleSite, kStormScaleOrig, sizeof(kStormScaleOrig)) ||
        game::BytesMatch(addr::StormScaleSite, kStormScalePatched, sizeof(kStormScalePatched));
    const bool okStormHeight = game::BytesMatch(addr::StormHeightSite, kStormHeightOp, sizeof(kStormHeightOp));
    const bool okScale  = game::BytesMatch(addr::ScaleSite,  kScaleOp,  sizeof(kScaleOp));
    const bool okCount  = game::BytesMatch(addr::CountSite,  kCountOp,  sizeof(kCountOp));
    const bool okHeight = game::BytesMatch(addr::HeightSite, kHeightOp, sizeof(kHeightOp));

    if (!okStormScale || !okStormHeight || !okScale || !okCount || !okHeight) {
        Log("storms: NOT patching -- site check failed (stormScale %d, "
            "stormHeight %d, scale %d, count %d, height %d). Weather stays as "
            "the game ships it.",
            okStormScale, okStormHeight, okScale, okCount, okHeight);
        return;
    }

    __try {
        WriteStormScale(g_tune.stormScale);
        WriteImm32(addr::StormHeightSite, addr::kStormHeightImmAt, g_tune.stormHeight);
        WriteImm32(addr::ScaleSite,  addr::kScaleImmAt,  g_tune.cloudScale);
        WriteImm8 (addr::CountSite,  addr::kCountImmAt,  g_tune.cloudCount);
        WriteImm32(addr::HeightSite, addr::kHeightImmAt, g_tune.cloudHeight);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("storms: faulted while patching -- weather may be half-applied");
        return;
    }

    g_applied = true;
    Log("storms: applied -- STORM scale %d (was 80, x%.2f) height %d (was 300) | "
        "fair clouds scale %d (was 250) count %d (was 3) height %d (was 500)",
        g_tune.stormScale, (double)g_tune.stormScale / 80.0, g_tune.stormHeight,
        g_tune.cloudScale, g_tune.cloudCount, g_tune.cloudHeight);
}

// Put the game back exactly as we found it, including the two instructions we
// replaced with one. Worth having even though the process is about to end: an
// unload that leaves patched code behind is the kind of thing that turns "PEMF
// crashed" into "the game crashed".
inline void Restore()
{
    if (!g_applied) return;
    DWORD old = 0;
    void* at = (void*)addr::StormScaleSite;
    if (VirtualProtect(at, sizeof(kStormScaleOrig), PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(at, kStormScaleOrig, sizeof(kStormScaleOrig));
        VirtualProtect(at, sizeof(kStormScaleOrig), old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, sizeof(kStormScaleOrig));
    }
    WriteImm32(addr::StormHeightSite, addr::kStormHeightImmAt, 300);
    WriteImm32(addr::ScaleSite,  addr::kScaleImmAt,  250);
    WriteImm8 (addr::CountSite,  addr::kCountImmAt,  3);
    WriteImm32(addr::HeightSite, addr::kHeightImmAt, 500);
    g_applied = false;
    Log("storms: restored vanilla weather");
}

} // namespace storms
