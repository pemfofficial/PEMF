// render.h - taking control during the frame's RENDER phase.
//
// WHY THIS EXISTS
//
// Our safe point (the main loop's PeekMessageA) is the top of the frame, before
// anything is drawn. That is the right place to decide things and the wrong
// place to show them:
//
//   * the dialog renderer composites over the back buffer, so presented there
//     it lands on a stale, half-finished frame -- the "scene under the modal
//     looks broken" symptom
//   * HUD text (FUN_004B06C0) has no timer of its own and must be re-issued
//     every frame; drawn at the top of the frame the world paints over it
//
// FUN_004612B0 is the sailing render -- sea, ships, floating name labels, HUD
// text. It has exactly ONE caller, at 0x004726CA, so we redirect that call's
// rel32 to our stub rather than detouring the prologue: one 4-byte write, no
// trampoline, nothing relocated, reversible.
//
// ---------------------------------------------------------------------------
// STATUS: THIS APPROACH DOES NOT WORK. Default stage is 0 (not installed).
//
// A controlled experiment settled it, 2026-07-25:
//
//   stage 0, no hook                      -> reaches the world, sails, stable
//   stage 1, hook + callback that ONLY
//            does InterlockedIncrement    -> black screen on world entry, crash
//
// Since the callback does nothing at all, the fault is the redirection itself.
// Not the callback, and not the work it was going to do.
//
// RULED OUT along the way:
//   * Calling convention. FUN_004612B0 spans 0x004612B0-0x00463CB2 (~10.5 KB)
//     and ends in a plain `ret`: ordinary cdecl with arguments in eax/ecx, so
//     the stub's `ret` was correct. (An earlier note claiming "no ret" was a
//     too-short disassembly range, not a real finding.)
//   * Namespaced symbols in inline asm. Every symbol the stub touches is now
//     file-scope extern "C", and stage 1 still crashed.
//
// So something about taking control at 0x004726CA is unsound. Worth noting that
// the engine never raises its own dialogs from inside rendering -- it does so
// from game logic -- so the premise was already unlike anything the game does
// to itself.
//
// WHERE TO GO NEXT: hook the D3D9 device instead. `d3d9.dll` is loaded
// dynamically, so its vtable can be hooked at EndScene -- a documented,
// well-trodden interception point that is designed to be called at exactly the
// moment we want (scene complete, not yet presented). That is a better fit than
// finding another engine function to interpose on.
//
// The code below is kept, working and reversible, so that a future attempt has
// a starting point rather than a blank page.
//
// STAGES -- raise one at a time, testing in game between each:
//   0  not installed (default, and currently the only stage that works)
//   1  installed; callback ONLY increments a counter
//   2  + draw notices
//   3  + present queued events
// ---------------------------------------------------------------------------
#pragma once
#include <windows.h>
#include "log.h"
#include "game.h"

#ifndef PEMF_RENDER_STAGE
#define PEMF_RENDER_STAGE 0
#endif

// Everything the naked stub references lives at file scope with C linkage, so
// the inline assembler resolves plain unmangled symbols.
extern "C" {

void*         g_pemfOrigSailingRender = nullptr;
volatile LONG g_pemfRenderFrames      = 0;

// Implemented in core.cpp. Deliberately __cdecl and argument-less.
void __cdecl PemfAfterSailingRender(void);

// Naked so the game's register-passed arguments (eax, ecx) reach the original
// untouched, and its return value survives the callback.
__declspec(naked) void PemfSailingRenderStub(void)
{
    __asm {
        call dword ptr [g_pemfOrigSailingRender]   // eax / ecx pass through
        push eax                                   // preserve the return value
        pushfd
        push ecx
        push edx
        call PemfAfterSailingRender
        pop  edx
        pop  ecx
        popfd
        pop  eax
        ret
    }
}

} // extern "C"

namespace render {

// The `call rel32` in the sailing update that invokes the render function.
constexpr uintptr_t kCallSite   = 0x004726CA;      // E8 <rel32>
constexpr uintptr_t kCallTarget = game::addr::SailingRender;
constexpr int       kStage      = PEMF_RENDER_STAGE;

inline bool g_installed   = false;
inline bool g_loggedFirst = false;

// Rewrite the rel32 of the `call` at `site` so it targets `fn`.
inline bool RedirectCall(uintptr_t site, void* fn, void** previous)
{
    if (*(const BYTE*)site != 0xE8) return false;      // not a call rel32

    DWORD old = 0;
    if (!VirtualProtect((void*)(site + 1), 4, PAGE_EXECUTE_READWRITE, &old))
        return false;

    const int32_t prevRel = *(const int32_t*)(site + 1);
    if (previous) *previous = (void*)(site + 5 + prevRel);

    *(int32_t*)(site + 1) = (int32_t)((uintptr_t)fn - (site + 5));
    VirtualProtect((void*)(site + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)site, 5);
    return true;
}

inline bool Install()
{
    if (kStage <= 0) {
        Log("render: hook disabled (stage 0) -- dialogs present from the safe "
            "point, notices are not drawn");
        return false;
    }
    if (g_installed) return true;

    if (!RedirectCall(kCallSite, (void*)&PemfSailingRenderStub,
                      &g_pemfOrigSailingRender)) {
        Log("render: could NOT redirect the call at 0x%08X", (unsigned)kCallSite);
        return false;
    }
    if ((uintptr_t)g_pemfOrigSailingRender != kCallTarget) {
        // Not the target we expected -- put it back rather than run blind.
        Log("render: call at 0x%08X targets 0x%p, expected 0x%08X -- reverting",
            (unsigned)kCallSite, g_pemfOrigSailingRender, (unsigned)kCallTarget);
        void* dummy = nullptr;
        RedirectCall(kCallSite, g_pemfOrigSailingRender, &dummy);
        return false;
    }

    g_installed = true;
    Log("render: hooked 0x%08X -> 0x%08X, STAGE %d",
        (unsigned)kCallSite, (unsigned)kCallTarget, kStage);
    return true;
}

inline void Uninstall()
{
    if (!g_installed) return;
    void* dummy = nullptr;
    RedirectCall(kCallSite, g_pemfOrigSailingRender, &dummy);
    g_installed = false;
}

inline bool Active() { return g_installed; }

// Stage 1 only counts frames. Reporting is done from the SAFE POINT, which is
// known to be a sound place to log from -- the render callback itself stays as
// close to doing nothing as possible while the premise is being tested.
inline void ReportFromSafePoint()
{
    if (!g_installed || g_loggedFirst) return;
    if (g_pemfRenderFrames <= 0) return;
    g_loggedFirst = true;
    Log("render: STAGE %d alive -- %ld render frames seen, game still running",
        kStage, g_pemfRenderFrames);
}

// Do the visible work. At stage 1 this is deliberately empty.
inline bool WantsNotices() { return kStage >= 2; }
inline bool WantsPresent() { return kStage >= 3; }

} // namespace render
