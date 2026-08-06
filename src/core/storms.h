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
#include "officerfx.h"
#include "crewmorale.h"
#include "game.h"
#include "render.h"   // RedirectCall
#include "events.h"   // Busy -- a card freezes the world, and the weather with it
#include "stormaudio.h"

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

    // A storm is ONE cloud in vanilla, and the slot count cannot be raised (see
    // kCountMax). So a squall line is built by drawing the storm prefab extra
    // times ourselves, around the real one.
    int clusterCount  = 0;    // extra clouds around the storm; 0 = vanilla
    int clusterSpread = 7000; // how far out they sit, in map units
    int clusterScale  = 70;   // their size as a PERCENT of the main storm

    // Weather that costs you something. Storms already hurt the hull; this adds
    // a little cargo going over the side.
    int cargoLossEnabled  = 0;
    int cargoLossRadius   = 9000;  // kept for the log only; the engine decides
    int cargoLossIntensity = 12;   // engine weather value that counts as "in it"

    // Bitmask of hold slots the sea will NOT take. Bit 0 = gold, bit 6 =
    // cannon: 0x41. Losing plunder reads as theft, and losing CANNON is not
    // cargo attrition -- it is disarming the ship, which a playtest found out
    // the hard way ("The sea takes 2 Cannon over the side" was real, and it
    // really did remove them).
    int cargoLossProtect  = 0x41;

    // The engine's weather-curve numerator. VANILLA IS 128000. Higher = heavier
    // rain further out -- and stronger wind with it. See WeatherPowerSite.
    int weatherPower = 128000;

    // GLOBAL RAIN, decoupled from the weather curve.
    //
    // Rain falls across the WHOLE screen, including clear sky with no cloud
    // above it, and it draws over the storm rather than under it. That is
    // because it is not emitted by the cloud at all -- the cloud carries its
    // own rain underneath, and this is a separate global effect scaled by the
    // weather intensity:
    //
    //     0047BC3C  imul eax, dword ptr [0x0085A0F8]
    //
    // Seven bytes, where `imul eax, eax, imm8` is three. So the multiply can be
    // replaced with a constant of our choosing and the global rain stops
    // tracking the weather -- WITHOUT touching 0x0085A0F8 itself, which also
    // drives the wind and every threshold in this file.
    //
    //   -1  leave the engine's behaviour alone
    //    0  no global rain at all -- only the rain the storm cloud carries
    //   1+  a fixed amount regardless of how bad the weather is
    int rainAmount = -1;

    // How long a storm takes to grow to full size after the engine re-seeds it,
    // so it does not simply appear. 0 disables. See PemfStormScale.
    int growMs        = 6000;   // how long it takes to reach full size
    int fadeOutMs     = 6000;   // ...and how long it takes to leave
    // What counts as a NEW SYSTEM rather than the old one drifting. Measured
    // drift is ~350 units/second, so anything at or below a few thousand will
    // mistake ordinary weather movement for a re-seed -- which is exactly what
    // made storms disappear as the player sailed into them.
    int growJump      = 20000;
    int probe         = 0;      // 1 = watch and report, manage nothing
    int restMs        = 180000; // quiet between systems
    int firstDelayMs  = 20000;  // ...but the FIRST one comes sooner
    int lifeMs        = 420000; // how long one lives before it is spent
    int driftPerSec   = 350;    // MEASURED: westward drift, map units/second
    int spawnDistance = 26000;  // born this far to windward -- off screen
    int killDistance  = 55000;  // once this far off, it is gone
    int spreadPerScale = 19;    // ring spread = stormScale * this / 10
    // How far off a storm must be before we will draw it. ⚠️ THE ENGINE SEEDS
    // AT ABOUT 8000 MAP UNITS DUE EAST, always -- so anything above that means
    // no weather ever appears. Measured the hard way. It cannot be used to
    // guarantee an off-screen birth; the grow-in does that job instead.
    int birthDistance = 6500;

    // The cull-bound fix. Scaling a cloud grows the visual but not its culling
    // sphere, so at high scale the engine drops the whole cloud while most of
    // it is still on screen -- reported from play as the storm vanishing once
    // it is 20-30% off the edge.
    //
    //   0 = off
    //   1 = REPORT ONLY. Dump the candidate bound fields once so the offsets
    //       can be checked against real numbers before anything is written.
    //   2 = report and fix.
    //
    // Defaults to report-only on purpose. The offsets are derived from the
    // Gamebryo member order rather than read out of this binary, and this file
    // has already spent one round on a layout that looked right and was not.
    int boundFix = 1;
    int boundScalePct = 100;   // MULTIPLIER on the radius the engine computed

    // PEMF owns where and when storms happen. The engine drops one at eight
    // coarse cells EAST of the player and leaves it there, which is why vanilla
    // weather always turns up in the same corner of the screen. Ours are far
    // bigger and more intense, so there should be fewer of them and they should
    // come from any quarter.
    int schedule       = 0;       // 1 = PEMF thins and redirects storms
    int everyNth       = 3;       // let 1 seed in N actually arrive
    int spawnAtStart   = 1;       // one storm on the first voyage, to teach them
    int placeDistance  = 11000;   // how far off it appears, in map units
    int parkDistance   = 400000;  // where a suppressed storm is sent instead
    int cargoLossEveryMs  = 20000; // at most one loss this often
    int cargoLossMax      = 3;     // units of ONE good, picked at random

    bool enabled    = false;
};

inline Tuning g_tune;

// Sanity rails. A scale of 0 makes storms vanish and a negative one is written
// through `ABS()` at the other end, so neither is a useful thing to allow. The
// upper bounds are generous but finite -- an immediate that overflows the
// multiply would produce a garbage scale rather than a big cloud.
constexpr int kScaleMin = 20,   kScaleMax = 2032;   // 127 * 16, the imm8 ceiling
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
    // THE STORM SIZE.
    //
    //   0046374A  lea edx,[ebx+ebx*4]   8D 14 9B      <-- we patch THIS
    //   0046374D  push esi              56
    //   0046374E  lea eax,[esp+0x60]    8D 44 24 60
    //   00463752  shl edx,0x4           C1 E2 04      <-- left alone
    //
    // ⚠️ THE `lea` AND THE `shl` ARE NOT ADJACENT. Two unrelated instructions
    // sit between them. An earlier version of this file treated them as one
    // six-byte run and tried to replace both with `imul edx, ebx, imm32`; the
    // signature never matched, the site check refused every single time, and
    // the storm patch silently did nothing for several rounds of "looks
    // bigger". Had the check not been there it would have written over
    // `push esi`.
    //
    // `lea edx,[ebx+ebx*4]` is 3 bytes and `imul edx, ebx, imm8` (`6B D3 imm8`)
    // is also 3 bytes, so the multiply-by-five becomes a multiply-by-ours and
    // the existing `shl edx,4` still multiplies by 16 afterwards:
    //
    //     drawn scale = imm8 * 16          (vanilla imm8 = 5 -> 80)
    constexpr uintptr_t StormScaleSite  = 0x0046374A;
    constexpr int       kStormScaleImmAt = 2;
    constexpr int       kStormScaleStep  = 16;    // the surviving shl

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

    // HOW HEAVY THE WEATHER IS. `B8 00 F4 01 00` = mov eax, 0x1F400, the
    // numerator of the engine's own weather curve inside FUN_0045FA70:
    //
    //     intensity = 0x1F400 / (distance + 4000)
    //
    // That value drives the RAIN density (0x0047BC3C multiplies by it), and it
    // is the same number PEMF's cargo loss reads. Raising it makes weather bite
    // harder at every distance rather than only close in.
    //
    // ⚠️ IT IS NOT RAIN-ONLY. Five other sites read 0x0085A0F8, including the
    // overworld tick and the sailing update, which is almost certainly where a
    // storm's push on your ship comes from. Turning this up makes storms wetter
    // AND windier. That is arguably the right bargain for weather, but it is a
    // gameplay change and should not arrive by surprise.
    constexpr uintptr_t WeatherPowerSite = 0x0045FAF6;
    constexpr int       kWeatherPowerImmAt = 1;

    // GLOBAL RAIN AMOUNT.
    //
    //   0047BC3C  imul eax, dword ptr [0x0085A0F8]   0F AF 05 F8 A0 85 00
    //
    // Seven bytes, where `imul eax, eax, imm8` (`6B C0 imm8`) is three -- so the
    // multiply by the weather value becomes a multiply by a constant of ours,
    // plus four NOPs. That decouples the screen-wide rain from the weather
    // curve WITHOUT touching 0x0085A0F8, which also drives the wind and every
    // intensity threshold in this file.
    constexpr uintptr_t RainAmountSite = 0x0047BC3C;

    // Where the engine says the storm is. Read-only for us -- see the note on
    // the scheduler for why writing these is a losing fight.
    constexpr uintptr_t StormX = 0x008B98F0;
    constexpr uintptr_t StormY = 0x008B98E4;
}

// Opcode prefixes only -- the immediates are what we are about to change, so
// they are deliberately NOT part of the signature. Verifying the whole
// instruction would make the patch un-reapplicable and, worse, would make a
// second call look like a failed target check.
constexpr unsigned char kScaleOp[]  = { 0x69, 0xDB };
constexpr unsigned char kCountOp[]  = { 0x83, 0xF8 };
constexpr unsigned char kHeightOp[] = { 0x68 };
constexpr unsigned char kWeatherPowerOp[] = { 0xB8 };
constexpr unsigned char kRainOrig[]    = { 0x0F, 0xAF, 0x05 };   // imul eax,[mem]
constexpr unsigned char kRainPatched[] = { 0x6B, 0xC0 };         // imul eax,eax,imm8
constexpr unsigned char kStormHeightOp[] = { 0x68 };

// The storm scale site is the one place we replace whole instructions, so it is
// checked against BOTH forms: the original lea+shl, or our own imul if this is
// a re-apply. Anything else and we leave it alone.
constexpr unsigned char kStormScaleOrig[]    = { 0x8D, 0x14, 0x9B };   // lea
constexpr unsigned char kStormScalePatched[] = { 0x6B, 0xD3 };         // our imul

inline bool g_applied = false;

// ------------------------------------------------------------------ cluster
// The storm draw is `call FUN_004BBC80` at 0x0046377A. Redirecting it puts our
// extra instances in exactly the right place in the frame -- same phase, same
// ordering -- which is worth far more than picking a hook point ourselves.
//
// It works at all because the prefab keeps a POOL: FUN_004BB4B0 walks a list at
// +0x10 and allocates another instance when the current one is already marked
// used this frame (+0x24). That is how three weather slots become three
// separate clouds, and it is why calling the draw again yields another cloud
// rather than moving the first one.
constexpr uintptr_t kStormCallSite = 0x0046377A;
constexpr int kMaxCluster = 12;

extern "C" void __cdecl PemfFixStormBounds(void* prefab, int drawnScale);
extern "C" int  __cdecl PemfStormScale(int gameScale);

extern "C" {
    inline void* g_stormOrig  = nullptr;   // the real FUN_004BBC80
    inline int   g_clusterN   = 0;
    inline int   g_clusterSc  = 0;
    inline int   g_clusterPct = 70;   // satellites, as a percent of the core
    inline int   g_drawScale  = 0;    // the size decided for THIS frame
    inline int   g_drawX = 0, g_drawY = 0;   // ...and WHERE, which is also ours
    inline int   g_lastParam8 = 0;           // the draw's last argument
    inline int   g_offX[kMaxCluster] = {0};
    inline int   g_offY[kMaxCluster] = {0};
}

// __thiscall: ecx = prefab, seven stack args, CALLEE cleans (0x1C). Verified at
// both call sites -- neither follows the call with an `add esp`.
__declspec(naked) void StormDrawShim()
{
    __asm {
        push ebp
        mov  ebp, esp
        push ebx
        push esi
        push edi

        mov  ebx, ecx                       // the prefab; ecx is clobbered by
                                            // each call, so keep it somewhere

        // Ask what size this storm should be drawn at RIGHT NOW -- full size
        // normally, smaller while it is growing in after a relocation. The
        // game's own draw uses the answer too, or the core would pop while the
        // ring around it grew.
        mov  eax, dword ptr [ebp+0x20]      // the draw's last argument
        mov  dword ptr [g_lastParam8], eax
        push dword ptr [ebp+0x18]
        call PemfStormScale
        add  esp, 4
        mov  dword ptr [g_drawScale], eax

        // Zero means "there is no storm right now". Skip OUR extra draws --
        // but the game's own call still happens below, at a size too small to
        // see.
        //
        // ⛔ DO NOT SKIP THE GAME'S CALL. That was the first version and it
        // takes the sky with it: FUN_004BBC80 is where an instance is marked
        // used for this frame (+0x24), and FUN_004BB4B0 allocates a NEW one
        // whenever the current is already marked. Miss the call and the flags
        // are never cleared, so the next draw allocates instead of reusing --
        // every frame, for as long as the weather is quiet. The pool grows, the
        // scene-node budget it shares with everything else runs out, and
        // clouds stop appearing ANYWHERE, permanently, spreading to prefabs we
        // never touched. Reported from play as "the sky is empty and I never
        // see another cloud again".
        //
        // Drawing it at scale 1 -- a hundredth of a unit -- is invisible and
        // keeps every bit of the engine's bookkeeping intact.
        test eax, eax
        jnz  have_storm
        mov  dword ptr [g_drawScale], 1     // invisible, but still drawn
        jmp  no_extras

    have_storm:
        mov  edx, dword ptr [g_clusterPct]
        imul eax, edx
        mov  ecx, 100
        cdq
        idiv ecx
        mov  dword ptr [g_clusterSc], eax

        xor  esi, esi
    next_extra:
        cmp  esi, dword ptr [g_clusterN]
        jge  main_draw

        push dword ptr [ebp+0x20]           // p8
        push dword ptr [ebp+0x1C]           // p7
        push dword ptr [g_clusterSc]        // scale (satellites are smaller)
        push dword ptr [ebp+0x14]           // rotation matrix
        push dword ptr [ebp+0x10]           // z
        mov  eax, dword ptr [g_drawY]       // OUR y + ring offset
        add  eax, dword ptr g_offY[esi*4]
        push eax
        mov  eax, dword ptr [g_drawX]       // OUR x + ring offset
        add  eax, dword ptr g_offX[esi*4]
        push eax
        mov  ecx, ebx
        call dword ptr [g_stormOrig]

        inc  esi
        jmp  next_extra

    no_extras:
    main_draw:
        // The game's own storm -- everything as it asked for except the size,
        // which is ours so the grow-in applies to the core cloud too.
        push dword ptr [ebp+0x20]
        push dword ptr [ebp+0x1C]
        push dword ptr [g_drawScale]
        push dword ptr [ebp+0x14]
        push dword ptr [ebp+0x10]
        push dword ptr [g_drawY]
        push dword ptr [g_drawX]
        mov  ecx, ebx
        call dword ptr [g_stormOrig]

        // Every instance now exists and has been positioned, so this is the
        // moment its cull sphere is meaningful. cdecl, we clean.
        push dword ptr [ebp+0x18]      // the scale the game itself asked for
        push ebx                       // the prefab
        call PemfFixStormBounds
        add  esp, 8

        pop  edi
        pop  esi
        pop  ebx
        mov  esp, ebp
        pop  ebp
        ret  0x1C
    }
}

// ------------------------------------------------------------- the cull bound
// A prefab keeps its instances on a list at +0x10, and each instance's scene
// node is at +0x1C. Inside the node, the Gamebryo member order is
//
//     NiBound m_kWorldBound;   // NiPoint3 centre + float radius = 16 bytes
//     NiTransform m_kLocal;    // rotation 36 + translation 12 + scale 4
//     NiTransform m_kWorld;
//
// and we KNOW m_kLocal starts at +0x38, because +0x68 is the scale this file
// patches and FUN_004C0740 confirms it independently (it writes param_1[0x1A],
// which is byte offset 0x68). So the bound sits immediately before it:
//
//     centre  node + 0x28
//     radius  node + 0x34
//
// ⚠️ DERIVED, NOT READ OUT OF THIS BINARY. Report-only mode exists so the
// numbers can be checked before anything is written -- a radius should be a
// positive, finite float of roughly model size, and if +0x34 comes back as
// zero, enormous, or NaN then the layout is wrong and nothing should be
// written on the strength of it.
constexpr int kNodeAt        = 0x1C;
constexpr int kInstNextAt    = 0x10;
constexpr int kBoundRadiusAt = 0x34;
constexpr int kBoundCentreAt = 0x28;
constexpr int kLocalScaleAt  = 0x68;

inline bool g_boundReported = false;

inline bool LooksLikeRadius(float f)
{
    return f > 0.0f && f < 1.0e9f && f == f;      // positive, sane, not NaN
}

// Walk the prefab's instance list and hand every node a cull sphere that
// matches how big it is actually drawn.
inline void FixBounds(void* prefab, int drawnScale)
{
    if (!prefab || g_tune.boundFix == 0) return;

    // MULTIPLY the radius the engine worked out; do not replace it. Measured
    // in game: a storm drawn at scale 3.99 reports radius 1464.28, and
    // 1464.28 / 3.99 = 367.0 exactly -- so Gamebryo IS scaling the bound
    // properly and the bound is not "broken".
    //
    // It is too small for a different reason: the storm is a PARTICLE SYSTEM
    // (FUN_004C0740 rescales its emitter parameters), so the bound covers the
    // EMITTER at ~367 units while the particles it throws spread across tens of
    // thousands. The cull sphere is correct for the node and useless for the
    // picture, which is why the cloud vanishes with most of itself on screen.
    const float mult = (float)g_tune.boundScalePct * 0.01f;
    int touched = 0;

    __try {
        int inst = (int)prefab;
        for (int guard = 0; inst != 0 && guard < 64; ++guard) {
            const int node = *(const int*)(inst + kNodeAt);
            if (node) {
                const float radius = *(const float*)(node + kBoundRadiusAt);
                const float scale  = *(const float*)(node + kLocalScaleAt);

                if (!g_boundReported) {
                    g_boundReported = true;
                    Log("storms: bound probe -- node 0x%08X, radius(+0x34) %.3f, "
                        "scale(+0x68) %.3f, centre(+0x28) %.1f/%.1f/%.1f",
                        (unsigned)node, radius, scale,
                        *(const float*)(node + kBoundCentreAt),
                        *(const float*)(node + kBoundCentreAt + 4),
                        *(const float*)(node + kBoundCentreAt + 8));
                    if (!LooksLikeRadius(radius)) {
                        Log("storms: bound probe -- +0x34 does NOT look like a "
                            "radius. Layout is wrong; NOT writing it.");
                    }
                }

                // Only ever GROW it, and only when the existing value reads as
                // a sane radius. Shrinking a bound hides things that should be
                // drawn, which is a worse bug than the one we are fixing.
                if (g_tune.boundFix == 2 && LooksLikeRadius(radius) &&
                    mult > 1.0f) {
                    const float want = radius * mult;
                    if (LooksLikeRadius(want) && want > radius) {
                        *(float*)(node + kBoundRadiusAt) = want;
                        ++touched;
                    }
                }
            }
            inst = *(const int*)(inst + kInstNextAt);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    (void)touched;
}

// The naked shim calls this; keeping the SEH and the walking in normal C++
// means the assembly stays small enough to read.
extern "C" void __cdecl PemfFixStormBounds(void* prefab, int drawnScale)
{
    FixBounds(prefab, drawnScale);
}

// ---------------------------------------------------------------- the pop-in
// The engine re-seeds the storm wherever it likes and the cloud simply EXISTS
// at full size on the next frame. At vanilla scale nobody notices. At five
// times that, a storm appearing whole in the corner of the screen is the most
// obtrusive thing in the mod.
//
// We cannot move it -- the position is rewritten ten times a second and is not
// ours (see the scheduler note above). But the SCALE passed to the draw is
// ours, on every instance, every frame. So: notice when the storm has jumped,
// and grow it from nothing over a second or two instead of letting it appear.
//
// This is the shape of every fix that is going to work here. Placement belongs
// to the engine; appearance belongs to us.
inline int   g_lastSeedX = 0, g_lastSeedY = 0;
inline DWORD g_bornAt     = 0;
inline DWORD g_quietUntil = 0;
inline DWORD g_fadeFrom   = 0;      // when the fade-out began
inline bool  g_seedSeen   = false;
inline bool  g_showing    = false;
inline int   g_seedCount  = 0;      // how many systems have come through
inline bool  g_fading     = false;
inline int   g_heldX = 0, g_heldY = 0;   // where it was when it started leaving

// ---------------------------------------------------------------- the probe
// Set stormProbe = 1 and PEMF stops managing weather entirely: no suppression,
// no rest period, no size changes. It only WATCHES, once a second, and reports
// what the engine is really doing.
//
// This exists because three designs in a row were built on beliefs about the
// storm position that turned out to be wrong -- that it is written once per
// flag (it is not), that its range varies (it does not), and that movement of
// it means the storm was retired (unclear). Guessing again is not worth
// another round.
//
// The two questions:
//   - HOW does the position move? Does it track the player, jump occasionally,
//     or stay put while the player sails away?
//   - What is the LAST draw argument? FUN_004BBC80 stores it as
//     `*(float*)(inst + 0x28) = param_8 * 0.001`, which has the shape of a
//     0..1000 fade value -- the same convention the world-text call uses. If it
//     is opacity, a real fade becomes possible; if it is constant, it is not.
inline void ProbeTick(int x, int y, int dist, int gameScale, int param8)
{
    static DWORD s_last = 0;
    static int   s_prevX = 0, s_prevY = 0;
    static bool  s_have = false;
    const DWORD now = GetTickCount();
    if (s_last != 0 && now - s_last < 1000) return;
    s_last = now;

    int mx = s_have ? x - s_prevX : 0, my = s_have ? y - s_prevY : 0;
    if (mx < 0) mx = -mx;
    if (my < 0) my = -my;

    Log("storms: PROBE pos=(%d,%d) moved=%d dist=%d playerDist=%d scale=%d "
        "param8=%d", x, y, mx + my, dist,
        dist, gameScale, param8);
    s_prevX = x; s_prevY = y; s_have = true;
}

// ------------------------------------------------------- OUR weather system
// PEMF owns the storm you SEE. The engine still owns its own -- we never write
// its position -- but we stopped drawing it, and draw ours instead.
//
// This is possible because the shim owns every argument to the draw: the size,
// and the x/y. So a system can be born where we choose, drift as we choose, and
// live as long as we choose, without touching a single engine global.
//
// The model is the one the probe measured, because the engine's own behaviour
// was right and only its timing was inconvenient:
//
//   - a system is born to WINDWARD (east), off screen
//   - it DRIFTS WEST at ~350 map units a second, latitude unchanged
//   - it passes over, recedes, and eventually is gone
//   - then a rest, and another makes up
//
// Everything scales together off `stormScale`, so shrinking a system shrinks
// its whole footprint rather than leaving nine clouds rattling around inside a
// spread meant for bigger ones. Rain is deliberately NOT scaled -- it comes
// from the cloud art itself, and the only useful control over it is how much
// the instances overlap.
inline bool  g_sysUp     = false;
inline int   g_sysX = 0, g_sysY = 0;
inline DWORD g_sysBorn   = 0;
inline DWORD g_sysNext   = 0;      // earliest the next one may make up
inline DWORD g_sysLastMs = 0;      // for the drift integration
inline int   g_sysDist   = -1;     // player -> our system, for everything else

inline int OctDist(int dx, int dy)
{
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    const int lo = dx < dy ? dx : dy, hi = dx < dy ? dy : dx;
    return (lo + hi * 2) / 2;
}

// The spread of the ring scales with the storm, so "make it smaller" makes the
// whole SYSTEM smaller instead of scattering the same clouds more thinly.
inline int SpreadForScale()
{
    if (g_tune.clusterSpread > 0) return g_tune.clusterSpread;
    int sp = g_tune.stormScale * g_tune.spreadPerScale / 10;
    if (sp < 500) sp = 500;
    return sp;
}

extern "C" int __cdecl PemfStormScale(int gameScale)
{
    if (!g_tune.enabled) return gameScale;

    const DWORD now = GetTickCount();
    const int px = game::PlayerX() / 1000;
    const int py = game::PlayerY() / 1000;

    if (g_tune.probe) {
        int ex = 0, ey = 0;
        __try {
            ex = *(const int*)addr::StormX;
            ey = *(const int*)addr::StormY;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return gameScale; }
        ProbeTick(ex, ey, OctDist(ex - px, ey - py), gameScale, g_lastParam8);
        return gameScale;
    }

    // ---------------------------------------------- a card freezes the weather
    // While an event card is up the game still renders the overworld behind the
    // dialog, so this shim keeps being called -- but the safe point does not
    // run, the player cannot move, and nothing about the weather can honestly
    // change. Letting the model keep running there means a card can retire a
    // system mid-dialog: the sky clears behind the consequence box and a fresh
    // one makes up once the player clicks through.
    //
    // Reported from play as "if you're in a storm and a choice event fires,
    // when the consequence box renders the weather is changed to clear and
    // sunny, then returns to stormy after the box is acknowledged" -- which is
    // a storm being retired and replaced across the two cards.
    //
    // Holding is also simply correct. The player did not live through those
    // seconds, so neither should the weather: the paused time is handed back to
    // whichever clock was running, and the last size and position are drawn
    // again unchanged.
    if (events::Busy()) {
        const int paused = g_sysLastMs ? (int)(now - g_sysLastMs) : 0;
        g_sysLastMs = now;
        if (paused > 0 && paused < 60000) {
            g_sysBorn += (DWORD)paused;      // the storm does not age behind a card
            g_sysNext += (DWORD)paused;      // nor does a rest run down
        }
        return g_sysUp ? gameScale : 0;      // exactly what the last frame drew
    }

    const int dt = g_sysLastMs ? (int)(now - g_sysLastMs) : 0;
    g_sysLastMs = now;

    if (!g_sysUp) {
        g_sysDist = -1;

        // ⚠️ THE FIRST SYSTEM MUST NOT WAIT A FULL REST. Seeding g_sysNext with
        // `now + restMs` on the first call meant no weather at all for the
        // first 90 seconds of a session -- and because we skip the engine's
        // draw while resting, that reads as "storms are broken", not "the
        // weather is fair". A short opening delay instead, so a career begins
        // in clear weather without the sky being empty for two minutes.
        if (g_sysNext == 0) g_sysNext = now + (DWORD)g_tune.firstDelayMs;
        if ((int)(now - g_sysNext) < 0) return 0;

        // Make up to WINDWARD and far enough out to be born off screen. The
        // player never sees it appear; it comes over the horizon.
        g_sysUp   = true;
        g_sysBorn = now;
        // Always to windward and always off screen, but no further out than it
        // needs to be -- every extra thousand units is another three seconds of
        // empty sky before the weather arrives.
        g_sysX    = px + g_tune.spawnDistance;
        g_sysY    = py;
        Log("storms: a system makes up to windward, %d off (arrives in ~%d s)",
            g_tune.spawnDistance,
            g_tune.driftPerSec > 0
                ? (g_tune.spawnDistance - 18000) / g_tune.driftPerSec : 0);
    }

    // Drift west with the trade wind, at the rate the probe measured.
    if (dt > 0 && dt < 1000) {
        static int carry = 0;                 // keep the sub-unit remainder
        carry += g_tune.driftPerSec * dt;
        g_sysX -= carry / 1000;
        carry  %= 1000;
    }

    g_sysDist = OctDist(g_sysX - px, g_sysY - py);

    {   // Say where the system is now and then. A sky with no weather in it
        // should never leave us guessing whether that is intended.
        static DWORD s_beat = 0;
        if (s_beat == 0 || now - s_beat > 20000) {
            s_beat = now;
            Log("storms: system at (%d,%d), %d off, %d s old",
                g_sysX, g_sysY, g_sysDist, (int)(now - g_sysBorn) / 1000);
        }
    }

    // Gone: either it has run its course or it is simply far astern.
    const bool spent = (int)(now - g_sysBorn) > g_tune.lifeMs;
    const bool lost  = g_sysDist > g_tune.killDistance;
    if (spent || lost) {
        // TWO DIFFERENT ENDINGS, and they should not cost the same wait.
        //
        //   SPENT   the weather genuinely blew itself out. A proper rest is
        //           right -- fair weather is part of the cycle.
        //   ASTERN  the player sailed away from it. Nothing "happened"; they
        //           simply went somewhere else, and somewhere else should have
        //           its own weather without a minute of empty sky first.
        //
        // Charging the full rest for leaving an area is what made the sky go
        // blank every time the player relocated.
        const int wait = spent ? g_tune.restMs : g_tune.restMs / 5;
        g_sysUp   = false;
        g_sysNext = now + (DWORD)wait;
        g_sysDist = -1;
        // The numbers go in the line because "left astern" and "blows itself
        // out" have looked identical in a log before, and the difference
        // between a storm the player sailed away from and one a dialog aged to
        // death is exactly these two figures.
        Log("storms: the system %s after %d s -- %d off (life %d s, kill %d) "
            "-- next in %d s",
            spent ? "blows itself out" : "is left astern",
            (int)(now - g_sysBorn) / 1000, g_sysDist,
            g_tune.lifeMs / 1000, g_tune.killDistance, wait / 1000);
        return 0;
    }

    g_drawX = g_sysX;
    g_drawY = g_sysY;
    return gameScale;
}

// Offsets are computed ONCE and never move. Jittering them per frame would make
// the squall line strobe, which is the opposite of weather.
inline void BuildClusterOffsets(int count, int spread)
{
    if (count > kMaxCluster) count = kMaxCluster;
    // A ring, with every other cloud pulled inwards so it does not read as a
    // circle of identical blobs.
    static const int kCos[12] = { 100,  87,  50,   0, -50, -87,
                                 -100, -87, -50,   0,  50,  87 };
    static const int kSin[12] = {   0,  50,  87, 100,  87,  50,
                                    0, -50, -87,-100, -87, -50 };
    for (int i = 0; i < count; ++i) {
        const int r = (i & 1) ? (spread * 60 / 100) : spread;
        g_offX[i] = kCos[i % 12] * r / 100;
        g_offY[i] = kSin[i % 12] * r / 100;
    }
    g_clusterN = count;
}

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
        // The one setting that is not a number, so it has to be taken before
        // the integer scanner below gets a look at the line.
        {
            char nameBuf[128] = {0};
            if (sscanf_s(line, " stormMusicFile = %127[^\r\n]", nameBuf,
                         (unsigned)sizeof(nameBuf)) == 1) {
                char* end = nameBuf + strlen(nameBuf);
                while (end > nameBuf && (end[-1] == ' ' || end[-1] == '\t')) --end;
                *end = 0;
                if (nameBuf[0]) {
                    strncpy_s(stormaudio::g_tune.file,
                              sizeof(stormaudio::g_tune.file), nameBuf, _TRUNCATE);
                    ++applied;
                }
                continue;
            }
        }

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
        else if (_stricmp(key, "clusterCount")  == 0) { g_tune.clusterCount  = value; ++applied; }
        else if (_stricmp(key, "clusterSpread") == 0) { g_tune.clusterSpread = value; ++applied; }
        else if (_stricmp(key, "clusterScale")  == 0) { g_tune.clusterScale  = value; ++applied; }
        else if (_stricmp(key, "cargoLoss")       == 0) { g_tune.cargoLossEnabled = value; ++applied; }
        else if (_stricmp(key, "cargoLossRadius") == 0) { g_tune.cargoLossRadius  = value; ++applied; }
        else if (_stricmp(key, "cargoLossIntensity")== 0) { g_tune.cargoLossIntensity = value; ++applied; }
        else if (_stricmp(key, "cargoLossProtect")  == 0) { g_tune.cargoLossProtect   = value; ++applied; }
        else if (_stricmp(key, "weatherPower")      == 0) { g_tune.weatherPower       = value; ++applied; }
        else if (_stricmp(key, "rainAmount")        == 0) { g_tune.rainAmount         = value; ++applied; }
        else if (_stricmp(key, "stormGrowMs")       == 0) { g_tune.growMs             = value; ++applied; }
        else if (_stricmp(key, "stormFadeOutMs")    == 0) { g_tune.fadeOutMs          = value; ++applied; }
        else if (_stricmp(key, "stormGrowJump")     == 0) { g_tune.growJump           = value; ++applied; }
        else if (_stricmp(key, "stormProbe")        == 0) { g_tune.probe              = value; ++applied; }
        else if (_stricmp(key, "stormRestMs")       == 0) { g_tune.restMs             = value; ++applied; }
        else if (_stricmp(key, "stormLifeMs")       == 0) { g_tune.lifeMs             = value; ++applied; }
        else if (_stricmp(key, "stormFirstDelayMs")== 0) { g_tune.firstDelayMs       = value; ++applied; }
        else if (_stricmp(key, "stormDriftPerSec")  == 0) { g_tune.driftPerSec        = value; ++applied; }
        else if (_stricmp(key, "stormSpawnDistance")== 0) { g_tune.spawnDistance      = value; ++applied; }
        else if (_stricmp(key, "stormKillDistance") == 0) { g_tune.killDistance       = value; ++applied; }
        else if (_stricmp(key, "clusterSpreadScale")== 0) { g_tune.spreadPerScale     = value; ++applied; }
        else if (_stricmp(key, "stormBirthDistance")== 0) { g_tune.birthDistance      = value; ++applied; }
        else if (_stricmp(key, "boundFix")          == 0) { g_tune.boundFix           = value; ++applied; }
        else if (_stricmp(key, "boundScalePct")     == 0) { g_tune.boundScalePct      = value; ++applied; }
        else if (_stricmp(key, "stormEveryNth")     == 0) { g_tune.everyNth       = value; ++applied; }
        else if (_stricmp(key, "stormMusic")        == 0) { stormaudio::g_tune.enabled = value; ++applied; }
        else if (_stricmp(key, "stormMusicVolume")  == 0) { stormaudio::g_tune.volume  = value; ++applied; }
        else if (_stricmp(key, "stormMusicStartAt") == 0) { stormaudio::g_tune.startAt = value; ++applied; }
        else if (_stricmp(key, "stormMusicStopAt")  == 0) { stormaudio::g_tune.stopAt  = value; ++applied; }
        else if (_stricmp(key, "stormMusicFadeMs")  == 0) { stormaudio::g_tune.fadeMs  = value; ++applied; }
        else if (_stricmp(key, "stormMusicDuck")    == 0) { stormaudio::g_tune.duckGameMusic = value; ++applied; }
        else if (_stricmp(key, "stormMusicSettleMs")== 0) { stormaudio::g_tune.settleMs  = value; ++applied; }
        else if (_stricmp(key, "cargoLossEveryMs")== 0) { g_tune.cargoLossEveryMs = value; ++applied; }
        else if (_stricmp(key, "cargoLossMax")    == 0) { g_tune.cargoLossMax     = value; ++applied; }
        else if (_stricmp(key, "enabled")     == 0) { g_tune.enabled = (value != 0); ++applied; }
    }
    fclose(f);

    g_tune.stormScale  = Clamp(g_tune.stormScale,  kScaleMin,  kScaleMax);
    g_tune.stormHeight = Clamp(g_tune.stormHeight, kHeightMin, kHeightMax);
    g_tune.cloudScale  = Clamp(g_tune.cloudScale,  kScaleMin,  kScaleMax);
    g_tune.cloudCount  = Clamp(g_tune.cloudCount,  kCountMin,  kCountMax);
    g_tune.cloudHeight = Clamp(g_tune.cloudHeight, kHeightMin, kHeightMax);
    g_tune.clusterCount  = Clamp(g_tune.clusterCount, 0, kMaxCluster);
    g_tune.clusterSpread = Clamp(g_tune.clusterSpread, 500, 60000);
    g_tune.clusterScale  = Clamp(g_tune.clusterScale, 10, 200);
    g_tune.cargoLossRadius  = Clamp(g_tune.cargoLossRadius, 1000, 60000);
    g_tune.cargoLossEveryMs = Clamp(g_tune.cargoLossEveryMs, 2000, 600000);
    g_tune.cargoLossMax     = Clamp(g_tune.cargoLossMax, 1, 20);
    g_tune.cargoLossIntensity = Clamp(g_tune.cargoLossIntensity, 1, 32);
    g_tune.cargoLossProtect |= 1;              // gold is never on the table
    g_tune.weatherPower = Clamp(g_tune.weatherPower, 32000, 2000000);
    g_tune.boundFix      = Clamp(g_tune.boundFix, 0, 2);
    g_tune.boundScalePct = Clamp(g_tune.boundScalePct, 10, 4000);
    g_tune.everyNth      = Clamp(g_tune.everyNth, 1, 20);
    g_tune.placeDistance = Clamp(g_tune.placeDistance, 2000, 80000);

    stormaudio::Configure(gameDir);

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
inline bool WriteRainAmount(int amount)
{
    if (amount < 0)   return true;            // leave it alone
    if (amount > 127) amount = 127;           // signed imm8

    unsigned char blob[7] = { 0x6B, 0xC0, (unsigned char)amount,
                              0x90, 0x90, 0x90, 0x90 };
    DWORD old = 0;
    void* at = (void*)addr::RainAmountSite;
    if (!VirtualProtect(at, sizeof(blob), PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, blob, sizeof(blob));
    VirtualProtect(at, sizeof(blob), old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(blob));
    return true;
}

inline bool WriteStormScale(int scale)
{
    int mult = scale / addr::kStormScaleStep;            // imm8 for `imul edx,ebx,imm8`
    if (mult < 1)   mult = 1;
    if (mult > 127) mult = 127;                    // signed imm8

    unsigned char blob[3] = { 0x6B, 0xD3, (unsigned char)mult };

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
    const bool okPower  = game::BytesMatch(addr::WeatherPowerSite, kWeatherPowerOp, sizeof(kWeatherPowerOp));
    const bool okRain   =
        game::BytesMatch(addr::RainAmountSite, kRainOrig, sizeof(kRainOrig)) ||
        game::BytesMatch(addr::RainAmountSite, kRainPatched, sizeof(kRainPatched));

    if (!okStormScale || !okStormHeight || !okScale || !okCount || !okHeight ||
        !okPower || !okRain) {
        Log("storms: NOT patching -- site check failed (stormScale %d, "
            "stormHeight %d, scale %d, count %d, height %d, power %d). Weather "
            "stays as the game ships it.",
            okStormScale, okStormHeight, okScale, okCount, okHeight, okPower);
        (void)okRain;
        return;
    }

    __try {
        WriteStormScale(g_tune.stormScale);
        WriteImm32(addr::StormHeightSite, addr::kStormHeightImmAt, g_tune.stormHeight);
        WriteImm32(addr::ScaleSite,  addr::kScaleImmAt,  g_tune.cloudScale);
        WriteImm8 (addr::CountSite,  addr::kCountImmAt,  g_tune.cloudCount);
        WriteImm32(addr::HeightSite, addr::kHeightImmAt, g_tune.cloudHeight);
        WriteImm32(addr::WeatherPowerSite, addr::kWeatherPowerImmAt, g_tune.weatherPower);
        WriteRainAmount(g_tune.rainAmount);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("storms: faulted while patching -- weather may be half-applied");
        return;
    }

    // The cluster is a call redirect, not an immediate, so it installs
    // separately and is allowed to fail without taking the rest with it.
    if (g_tune.clusterCount > 0) {
        g_clusterPct = g_tune.clusterScale;
        g_clusterSc  = g_tune.stormScale * g_tune.clusterScale / 100;
        if (g_clusterSc < kScaleMin) g_clusterSc = kScaleMin;
        BuildClusterOffsets(g_tune.clusterCount, SpreadForScale());
        if (render::RedirectCall(kStormCallSite, (void*)&StormDrawShim, &g_stormOrig)) {
            Log("storms: cluster on -- %d extra cloud(s) at spread %d, scale %d "
                "(%d%% of the storm)", g_clusterN, SpreadForScale(),
                g_clusterSc, g_tune.clusterScale);
        } else {
            g_clusterN = 0;
            Log("storms: cluster NOT installed -- 0x%08X is not a call rel32",
                (unsigned)kStormCallSite);
        }
    }

    g_applied = true;
    Log("storms: applied -- STORM scale %d (was 80, x%.2f) height %d (was 300) | "
        "fair clouds scale %d (was 250) count %d (was 3) height %d (was 500) | "
        "weather power %d (was 128000, x%.2f)",
        g_tune.stormScale, (double)g_tune.stormScale / 80.0, g_tune.stormHeight,
        g_tune.cloudScale, g_tune.cloudCount, g_tune.cloudHeight,
        g_tune.weatherPower, (double)g_tune.weatherPower / 128000.0);
}

// ------------------------------------------------------------- weather bites
// Storms already cost you hull, through the game's own lightning. This adds the
// other thing a captain would expect: a little cargo going over the side.
//
// The storm's position is the same pair the draw call reads --
//   x = 0x008B98F0, y = 0x008B98E4
// -- in MAP UNITS, the same scale as `PlayerX() / 1000` and city coordinates.
// (The game builds them from 0x008B96B4 / 0x008B96B0, which are the player's
// own position divided by a million, so a storm is placed on a coarse grid
// relative to us.)
//
// GOLD IS NEVER TAKEN. Slot 0 of the hold is plunder, and having a squall empty
// the strongbox would feel like theft rather than weather. Only trade goods go.
namespace addr {
    // The game's OWN weather-proximity function, and the reason this file no
    // longer guesses a radius. Used by the ship AI at 0x0045FAB0.
    //
    //   int __cdecl StormIntensity(int x, int y, int playSound)
    //
    // walks the three weather slots, takes the octagonal distance to the
    // nearest (with a +2000 penalty on slots 1 and 2, so slot 0 -- the storm --
    // dominates) and returns
    //
    //     128000 / (distance + 4000)
    //
    // so it rises smoothly as you close: ~32 on top of the storm, ~14 at 5,000,
    // ~10 at 9,000, ~4 out at 28,000. There is NO hard edge; weather is a
    // gradient, which is why "am I in the storm" was always a judgement call.
    //
    // ✅ SAFE TO CALL WITH playSound = 0: the write to 0x0085A0F8 at the end
    // stores back the value it already read, so the call has no side effect.
    //
    // ⚠️ AND IT SETTLES A QUESTION: this reads the POSITION ARRAYS, never the
    // cloud's drawn scale. So making a storm bigger on screen does not make it
    // hit harder or blow you along faster -- the gameplay footprint is fixed.
    // A larger cloud simply means you spend longer inside the same gradient.
    constexpr uintptr_t StormIntensityFn = 0x0045FA70;
}

constexpr unsigned char kStormIntensitySig[] = {
    0xB8, 0xD3, 0x4D, 0x62, 0x10, 0xF7, 0x6C, 0x24, 0x08
};

typedef int (__cdecl *StormIntensity_t)(int x, int y, int playSound);

inline int StormIntensityHere()
{
    if (!game::BytesMatch(addr::StormIntensityFn, kStormIntensitySig,
                          sizeof(kStormIntensitySig))) return -1;
    __try {
        return ((StormIntensity_t)addr::StormIntensityFn)(
                    game::PlayerX(), game::PlayerY(), 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

constexpr int kHoldSlots = 7;    // slot 0 = gold/plunder, 1..6 = trade goods

inline DWORD g_lastLossAt = 0;
inline const char* g_pendingNotice = nullptr;
inline char g_noticeBuf[160];

inline int StormDistanceToPlayer()
{
    __try {
        const int sx = *(const int*)addr::StormX;
        const int sy = *(const int*)addr::StormY;
        const int px = game::PlayerX() / 1000;
        const int py = game::PlayerY() / 1000;
        int dx = sx - px, dy = sy - py;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        const int lo = dx < dy ? dx : dy;
        const int hi = dx < dy ? dy : dx;
        return (lo + hi * 2) / 2;          // the engine's own octagonal approx
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Called from the safe point while sailing. Deliberately does nothing at all
// unless the player asked for it -- losing cargo is a real consequence and it
// should never arrive as a surprise from a visual mod.
// Called from the safe point while sailing. Reads the engine's weather value
// once and hands it to everything that cares.
// ----------------------------------------------- why there is no scheduler
// Two attempts at owning storm placement lived here and both are gone, because
// the premise under them was wrong. Kept as a note so it is not tried a third
// time:
//
//   1. WRITE THE POSITION EACH SEED. Rests on the belief that the engine
//      writes it once when a flag fires. It does not.
//   2. HOLD THE POSITION EVERY TICK. "Wins" the fight and is worse -- it pins
//      the storm to one coordinate forever, so it can never drift or be
//      retired, and weather stops behaving like weather.
//
// What the probe actually found (see PemfStormScale) is that a storm is a real
// weather system: seeded to windward, then DRIFTING WEST at ~350 map units a
// second with its latitude unchanged. The position changes every frame because
// the storm is MOVING. There was never a fight to win -- only a misreading.
//
// ⇒ Placement and drift belong to the engine and are correct. Everything PEMF
// wants -- rarer weather, no pop, a different look -- comes from the DRAW,
// which we own outright: the shim decides the size AND the x/y passed in, so it
// can show a system, skip one, or put it somewhere else entirely without
// touching a single engine global.

// The weather value everything else reads. It is derived from OUR system, not
// the engine's, so the music, the cargo and the rain all agree with the clouds
// actually on screen -- which they would not if we drew one storm and measured
// another.
inline int OurIntensity()
{
    if (g_sysDist < 0) return 0;
    const int p = g_tune.weatherPower > 0 ? g_tune.weatherPower : 128000;
    return p / (g_sysDist + 4000);
}

inline void TickWeather(bool sailing)
{
    const int intensity = sailing ? OurIntensity() : -1;
    stormaudio::Tick(intensity < 0 ? 0 : intensity, sailing);
}

inline void TickCargoLoss()
{
    if (!g_tune.enabled || !g_tune.cargoLossEnabled) return;

    const DWORD now = GetTickCount();
    if ((int)(now - g_lastLossAt) < g_tune.cargoLossEveryMs) return;

    // Ask the ENGINE how bad the weather is here rather than measuring it
    // ourselves against a radius we invented.
    const int intensity = OurIntensity();
    const int d = g_sysDist;
    if (intensity < 0) return;                      // could not ask; do nothing
    if (intensity < g_tune.cargoLossIntensity) return;
    g_lastLossAt = now;

    // Pick a good we actually carry. Walking from a random start means a full
    // hold does not always lose the same barrel.
    const int start = 1 + (int)(now % (kHoldSlots - 1));
    int slot = -1, have = 0;
    for (int n = 0; n < kHoldSlots - 1; ++n) {
        const int i = 1 + ((start - 1 + n) % (kHoldSlots - 1));
        int v = 0;
        __try { v = *(const int*)(game::addr::UndividedPlunder + (uintptr_t)i * 4); }
        __except (EXCEPTION_EXECUTE_HANDLER) { v = 0; }
        if (v > 0 && ((g_tune.cargoLossProtect >> i) & 1) == 0) {
            slot = i; have = v; break;
        }
    }
    if (slot < 0) return;                  // nothing loose to lose

    int lost = 1 + (int)(now % (DWORD)g_tune.cargoLossMax);

    // A carpenter who stows well saves some of it; a careless one loses more.
    // Applied here rather than to the tunable, so the player's own
    // cargoLossMax keeps meaning what they set it to.
    if (officerfx::g_cargoGuard != 0) {
        const int before = lost;
        lost = officerfx::ApplyPercent(lost, -officerfx::g_cargoGuard);
        if (lost != before)
            Log("storms: cargo loss %d -> %d (carpenter %+d%%)",
                before, lost, officerfx::g_cargoGuard);
        if (lost <= 0) return;      // nothing went over the side
    }

    // Losing cargo in a blow is exactly the sort of thing a crew takes badly,
    // and it is why the morale system has to be able to be moved by anything.
    // Small: a storm is a bad day, not a mutiny.
    crewmorale::Nudge(-2, "cargo lost in a storm");
    if (lost > have) lost = have;

    __try {
        *(int*)(game::addr::UndividedPlunder + (uintptr_t)slot * 4) = have - lost;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    char item[64] = {0};
    if (!game::ItemName(slot, item, sizeof(item)) || !item[0])
        strncpy_s(item, sizeof(item), "cargo", _TRUNCATE);

    _snprintf_s(g_noticeBuf, sizeof(g_noticeBuf), _TRUNCATE,
                "The sea takes %d %s over the side.", lost, item);
    g_pendingNotice = g_noticeBuf;
    Log("storms: cargo loss -- slot %d (%s) %d -> %d, storm %d away, "
        "intensity %d", slot, item, have, have - lost, d, intensity);
}

// Put the game back exactly as we found it, including the two instructions we
// replaced with one. Worth having even though the process is about to end: an
// unload that leaves patched code behind is the kind of thing that turns "PEMF
// crashed" into "the game crashed".
inline void Restore()
{
    if (g_clusterN > 0 && g_stormOrig) {
        render::RedirectCall(kStormCallSite, g_stormOrig, nullptr);
        g_clusterN = 0;
    }
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
    WriteImm32(addr::WeatherPowerSite, addr::kWeatherPowerImmAt, 128000);
    {   // put the rain multiply back exactly as it was
        const unsigned char orig[7] = { 0x0F, 0xAF, 0x05, 0xF8, 0xA0, 0x85, 0x00 };
        DWORD old = 0;
        void* at = (void*)addr::RainAmountSite;
        if (VirtualProtect(at, sizeof(orig), PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(at, orig, sizeof(orig));
            VirtualProtect(at, sizeof(orig), old, &old);
            FlushInstructionCache(GetCurrentProcess(), at, sizeof(orig));
        }
    }
    g_applied = false;
    Log("storms: restored vanilla weather");
}

} // namespace storms
