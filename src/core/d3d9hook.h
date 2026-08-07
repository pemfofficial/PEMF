// d3d9hook.h - the per-frame render hook, via the game's own Direct3D device.
//
// This is the framework's way onto the screen. Everything we want to draw
// ourselves -- sailing notices, "LAND HO!" callouts, indicators, panels --
// needs a point inside the frame, and this is it.
//
// HOW WE REACH THE DEVICE
//
// The renderer singleton lives at game::addr::RendererPtr and stores its
// IDirect3DDevice9* at +0x60. That was established from the device-creation
// sequence (the CreateDevice call writes the device into renderer+0x60) and
// confirmed by 19 independent sites that read [[RendererPtr]+0x60] and use it
// as a COM object. We poll that pointer from the safe point; the moment the
// game has created its device we read it, take its vtable, and patch the
// slots we want.
//
// Two earlier approaches failed, and both are worth remembering:
//
//   * Redirecting the sailing-render call site (render.h) black-screened the
//     game even with a callback that did nothing but increment a counter --
//     the redirection itself was the fault, not the work.
//   * Creating a throwaway device to read "the" vtable cannot work: the
//     vtable is PER-INSTANCE heap memory, freed with the throwaway device
//     (VirtualQuery reported MEM_RESERVE right after Release). Patching it
//     could never have affected the game's device.
//
// Using the game's real device avoids both problems: the hooks are on the
// vtable actually in use, and nothing in the game's code is rewritten.
//
// THE VTABLE IS REBUILT UNDER US -- this is not theoretical, it was observed
// repeatedly in-game. So TryInstall re-verifies every safe point that our
// hook is still in the slot, and re-installs it when it is not. Without that
// check the hook silently dies and the log goes quiet with no explanation.
//
// STAGES -- raise one at a time, testing in game between each:
//   0  not installed
//   1  installed; the hook ONLY counts frames and calls the original
//   2  + draw notices
//   3  + present queued events
#pragma once
#include <windows.h>
#include "log.h"
#include "game.h"

#ifndef PEMF_D3D9_STAGE
#define PEMF_D3D9_STAGE 2
#endif

// IDirect3DDevice9 vtable indices. Fixed by the COM interface: IUnknown
// occupies 0-2, Reset is 16, Present 17, BeginScene 41, EndScene 42.
#define PEMF_VTBL_RESET      16
#define PEMF_VTBL_PRESENT    17
#define PEMF_VTBL_BEGINSCENE 41
#define PEMF_VTBL_ENDSCENE   42

extern "C" {

typedef HRESULT (WINAPI *PemfEndScene_t)(void* device);
typedef HRESULT (WINAPI *PemfBeginScene_t)(void* device);
typedef HRESULT (WINAPI *PemfPresent_t)(void* device, const RECT*, const RECT*,
                                        HWND, const void*);
typedef HRESULT (WINAPI *PemfReset_t)(void* device, void* params);

void*          g_pemfOrigEndScene   = nullptr;  // the real EndScene
void*          g_pemfOrigBeginScene = nullptr;  // the real BeginScene
void*          g_pemfOrigPresent    = nullptr;  // the real Present
void*          g_pemfOrigReset      = nullptr;  // the real Reset
void*          g_pemfGameDevice     = nullptr;  // the game's IDirect3DDevice9*
void**         g_pemfDeviceVTable   = nullptr;  // its vtable
volatile LONG  g_pemfEndSceneCalls  = 0;
volatile LONG  g_pemfBeginSceneCalls = 0;
volatile LONG  g_pemfPresentCalls   = 0;
volatile LONG  g_pemfResetCount     = 0;

// A displayed frame can contain SEVERAL BeginScene/EndScene pairs -- the game
// renders more than one pass, and they do not share a camera. World-anchored
// text drawn in every pass appears several times over in different places,
// which is what a stale-looking duplicate notice turns out to be.
//
// So world text is drawn in the FIRST pass of each frame only. The first pass
// is the world pass: the one that walks the scene graph our label was just
// attached to. Later passes have their own camera and their own graph, and a
// label built during one either lands in the wrong place or is never walked at
// all.
//
// WHICH BOUNDARY RESETS THE COUNT MATTERS MORE THAN IT LOOKS. Present seemed
// like the obvious answer and is the wrong one to depend on: this game does
// not call the device's Present, so a counter reset only there never resets,
// no pass is ever the first again, and anchored text silently stops drawing
// after the very first frame. The reset therefore comes from the safe point --
// the top of the game's own main loop, once per iteration, already proven as
// the place one-per-frame work happens. Present still resets it when it does
// fire, but nothing depends on that.
volatile LONG  g_pemfPassThisFrame  = 0;   // passes so far in the current frame
volatile LONG  g_pemfPassesLast     = 0;   // passes the previous frame used

// ---------------------------------------------------------- is the device well?
// ALT-TAB LOSES THE DEVICE, AND THAT KILLED THE GAME. Reported as an R6025
// "pure virtual function call" on returning to the game, twice, with
// `d3d9: device Reset #1` as the last line in the log both times.
//
// The mechanism: everything PEMF draws goes through the game's own routines,
// which walk refcounted Gamebryo objects. Across a lost device those objects
// are released and rebuilt, and calling a virtual on one that has been
// destructed is exactly what R6025 means. DEVELOPER.md has warned about this
// shape since the flag-texture crash; this is the same hazard arriving through
// a door nobody was watching.
//
// Two things were missing. Nothing knew a reset was in progress, and -- more
// subtly -- the pass counter is incremented BEFORE the real BeginScene runs, so
// when BeginScene failed on a lost device the counter still read 1 and the
// EndScene hook went ahead and drew into it.
//
// So: a device is well only between a BeginScene that SUCCEEDED and its
// EndScene, and never while a Reset is in flight.
volatile LONG  g_pemfDeviceLost     = 0;   // non-zero: do not draw
volatile LONG  g_pemfBeginOk        = 0;   // this pass's BeginScene succeeded
volatile LONG  g_pemfSkippedDraws   = 0;   // draws declined for the above

// ----------------------------------------------------- is the game rendering?
// g_pemfDeviceLost only covers the Reset CALL. It says nothing about the far
// longer window before it: alt-tab away and the game sits on a lost device for
// as long as the player is gone -- ten minutes, in the session this was found
// in -- calling Present ~8,000 times a second and never reaching BeginScene.
//
// PEMF could not see that. The safe point is the main loop, so it ran at that
// same 8,000/s, doing full ship-array scans, suspicion ticks, storm updates and
// flag work whose results nobody could see, on a machine whose owner had
// alt-tabbed away precisely because they wanted to do something else with it.
//
// So the pulse is measured directly: EndScene advancing means the game is
// drawing. It stops, and PEMF goes dormant until it starts again.
volatile LONG  g_pemfNotRendering   = 0;   // non-zero: game is not drawing

// ------------------------------------------------------ settling after a reset
// A SUCCESSFUL RESET IS NOT THE ALL-CLEAR. The device is usable again the
// instant Reset returns, but the ENGINE's own objects are not: the game rebuilds
// its render state over the frames that follow, and everything PEMF draws goes
// through the game's routines, which walk those objects.
//
// Clearing g_pemfDeviceLost on return therefore let PEMF draw into a half-rebuilt
// scene graph on the very next frame. That is the R6025 "pure virtual function
// call" testers hit on alt-tabbing back -- a virtual called on an object that has
// been destructed and not yet replaced. Reported three times, always on the way
// back into the game, and the log always showed a Reset just before it.
//
// So we wait. Not for a fixed count of frames -- a game that is still stuttering
// its way back would burn through those while nothing was really ready -- but for
// a stretch of wall clock after the last reset, which covers a slow recovery and
// a fast one alike.
volatile LONG  g_pemfSettleFrom     = 0;   // GetTickCount at the last reset
#define PEMF_RESET_SETTLE_MS 750

// Unsigned subtraction, so this stays correct across GetTickCount's 49-day wrap.
__inline int PemfSettlingAfterReset(void)
{
    const LONG from = g_pemfSettleFrom;
    if (!from) return 0;
    return ((DWORD)GetTickCount() - (DWORD)from) < PEMF_RESET_SETTLE_MS;
}

// Both implemented in core.cpp.
//
// The two phases are NOT interchangeable, and which one a draw belongs in is
// decided by how the game draws that kind of thing:
//
//   BeginScene -- the frame is empty and the world has not been built yet.
//                 World-anchored text goes here, because the game's own
//                 world-text call builds scene-graph nodes that the render
//                 walk then draws. Issued after the walk, they would be a
//                 frame late at best.
//   EndScene   -- the scene is complete. Screen-space HUD text goes here,
//                 because it is an immediate 2D blit that must land on top.
void __cdecl PemfOnBeginScene(void* device);
void __cdecl PemfOnEndScene(void* device);

HRESULT WINAPI PemfBeginSceneHook(void* device)
{
    InterlockedIncrement(&g_pemfBeginSceneCalls);
    const LONG pass = InterlockedIncrement(&g_pemfPassThisFrame) - 1;

    // The real BeginScene runs FIRST: the device must be inside a scene
    // before anything we do can contribute geometry to it.
    HRESULT hr = ((PemfBeginScene_t)g_pemfOrigBeginScene)(device);
    if (FAILED(hr)) {
        // A lost device fails here. Say so, so the EndScene hook -- which sees
        // only the pass counter, and that was already incremented above -- does
        // not go on to draw into it.
        InterlockedExchange(&g_pemfBeginOk, 0);
        return hr;
    }
    InterlockedExchange(&g_pemfBeginOk, 1);

    if (pass == 0 && !g_pemfDeviceLost && !PemfSettlingAfterReset())
        PemfOnBeginScene(device);
    else if (pass == 0)
        InterlockedIncrement(&g_pemfSkippedDraws);
    return hr;
}

// Present is hooked for completeness and for the counter, but nothing depends
// on it firing -- see the note above. Nothing is drawn here: by Present the
// frame is finished and gone.
HRESULT WINAPI PemfPresentHook(void* device, const RECT* src, const RECT* dst,
                               HWND wnd, const void* dirty)
{
    InterlockedIncrement(&g_pemfPresentCalls);
    const LONG passes = InterlockedExchange(&g_pemfPassThisFrame, 0);
    if (passes > 0) g_pemfPassesLast = passes;
    return ((PemfPresent_t)g_pemfOrigPresent)(device, src, dst, wnd, dirty);
}

HRESULT WINAPI PemfEndSceneHook(void* device)
{
    InterlockedIncrement(&g_pemfEndSceneCalls);

    // ONE PASS ONLY, exactly as BeginScene already does. A displayed frame runs
    // 2-3 Begin/EndScene pairs, and this hook used to fire on every one of them
    // -- so the screen notices were laid down two or three times per frame, each
    // run starting again at y=8 under a slightly different projection. On screen
    // that is one line of text with a second copy printed a few pixels off it,
    // which reads as a duplicated notice and was reported as exactly that.
    //
    // The counter is incremented by the BeginScene hook, so the first pair has
    // it at 1 by the time we get here. Reset comes from the safe point (see the
    // note on g_pemfPassThisFrame -- NOT from Present, which this game never
    // calls on the device).
    // Only draw when this pass's BeginScene actually succeeded and no reset is
    // in flight. Either check alone is not enough: the counter says which pass
    // we are in, not whether the device survived it.
    if (g_pemfPassThisFrame == 1) {
        // The settle check is the third condition and the newest: the other two
        // both read "clear" the instant Reset returns, which is exactly when the
        // engine's own objects are still being rebuilt. See the settle note.
        if (g_pemfBeginOk && !g_pemfDeviceLost && !PemfSettlingAfterReset())
            PemfOnEndScene(device);
        else InterlockedIncrement(&g_pemfSkippedDraws);
    }

    return ((PemfEndScene_t)g_pemfOrigEndScene)(device);
}

// A device reset means a resolution change or a lost device; worth seeing in
// the log because it is also when the vtable tends to be rebuilt.
// Guarded read of the engine's live-object count. Never faults: this runs on the
// path into a device reset, and a fault here would turn a diagnostic into the
// very crash it exists to explain.
static long PemfLiveObjects(void)
{
    __try { return *(const long*)game::addr::LiveSceneObjects; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

HRESULT WINAPI PemfResetHook(void* device, void* params)
{
    InterlockedIncrement(&g_pemfResetCount);

    // The count goes in the ENTRY line, not just the result line, because the
    // failures we are chasing do not reach a result line: one hangs inside the
    // driver's Reset and one returns D3DERR_INVALIDCALL. Whatever happens next,
    // this number was true going in -- and if it climbs reset over reset, the
    // engine is holding objects it should have released, which is exactly what
    // INVALIDCALL means. A flat number rules PEMF out just as usefully.
    Log("d3d9: device Reset #%ld -- PEMF standing down until it comes back "
        "(engine objects live: %ld)", g_pemfResetCount, PemfLiveObjects());

    // Down BEFORE the reset, not after. The game releases its render objects
    // inside this call, and anything of ours drawing while that happens is
    // walking a half-destroyed scene graph.
    InterlockedExchange(&g_pemfDeviceLost, 1);
    InterlockedExchange(&g_pemfBeginOk, 0);
    InterlockedExchange(&g_pemfPassThisFrame, 0);

    HRESULT hr = ((PemfReset_t)g_pemfOrigReset)(device, params);

    if (SUCCEEDED(hr)) {
        InterlockedExchange(&g_pemfDeviceLost, 0);
        // Not "drawing again" -- drawing again SHORTLY. See the settle note.
        InterlockedExchange(&g_pemfSettleFrom, (LONG)GetTickCount());
        Log("d3d9: device Reset #%ld succeeded -- settling for %d ms before "
            "PEMF draws again (%ld draw(s) declined meanwhile)",
            g_pemfResetCount, PEMF_RESET_SETTLE_MS, g_pemfSkippedDraws);
    } else {
        // Still lost. The game will call Reset again; we stay down until one
        // of them succeeds rather than guessing that this was the last.
        Log("d3d9: device Reset #%ld FAILED (0x%08lX) -- staying down",
            g_pemfResetCount, (unsigned long)hr);
    }
    return hr;
}

} // extern "C"

namespace d3d9hook {

constexpr int kStage = PEMF_D3D9_STAGE;

inline bool g_installed   = false;
inline bool g_loggedFirst = false;
inline bool g_gaveUp      = false;
inline int  g_tryCount    = 0;

inline bool WantsNotices() { return kStage >= 2; }
inline bool WantsPresent() { return kStage >= 3; }
inline bool Active()       { return g_installed; }

// Never-fault readability check. Mandatory on the DRM-packed build, where
// memory may not be committed yet -- a fault, even a caught one, can disturb
// the packer's own exception-based unpacking.
inline bool SlotReadable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (const BYTE*)p + n <= (const BYTE*)mbi.BaseAddress + mbi.RegionSize;
}

// Called from the safe point, so this runs on the game thread with the game
// idle -- no render call is in flight while the slots are swapped. Cheap
// once installed: a couple of guarded reads.
inline void TryInstall()
{
    if (kStage <= 0 || g_gaveUp) return;

    void** rendererSlot = (void**)game::addr::RendererPtr;
    if (!SlotReadable(rendererSlot, sizeof(void*))) return;
    BYTE* renderer = (BYTE*)*rendererSlot;
    if (!renderer) return;

    void** deviceSlot = (void**)(renderer + game::addr::RendererDeviceOfs);
    if (!SlotReadable(deviceSlot, sizeof(void*))) return;
    void* device = *deviceSlot;
    if (!device) return;

    // Already hooked: confirm the hooks are still in place. The engine has
    // been observed rebuilding the vtable, which silently sheds them.
    if (g_installed && device == g_pemfGameDevice) {
        void** vt = SlotReadable(device, sizeof(void*)) ? *(void***)device : nullptr;
        if (vt == g_pemfDeviceVTable &&
            SlotReadable(&vt[PEMF_VTBL_ENDSCENE], sizeof(void*)) &&
            vt[PEMF_VTBL_ENDSCENE]   == (void*)&PemfEndSceneHook &&
            vt[PEMF_VTBL_BEGINSCENE] == (void*)&PemfBeginSceneHook &&
            vt[PEMF_VTBL_PRESENT]    == (void*)&PemfPresentHook)
            return;
        Log("d3d9: hooks lost (vtable rebuilt under us) -- rehooking");
        g_installed = false;
    }
    ++g_tryCount;

    if (!SlotReadable(device, sizeof(void*))) return;
    void** vtable = *(void***)device;
    if (!vtable || !SlotReadable(vtable, (PEMF_VTBL_ENDSCENE + 1) * sizeof(void*)))
        return;

    // A recreated device can come back on the SAME vtable. If our hook is
    // already in the slot, re-hooking would capture our own hook as the
    // "original" and recurse -- just adopt the new device pointer.
    if (vtable[PEMF_VTBL_ENDSCENE]   == (void*)&PemfEndSceneHook ||
        vtable[PEMF_VTBL_BEGINSCENE] == (void*)&PemfBeginSceneHook ||
        vtable[PEMF_VTBL_PRESENT]    == (void*)&PemfPresentHook) {
        Log("d3d9: new device 0x%p reuses the hooked vtable 0x%p", device, vtable);
        g_pemfGameDevice = device;
        g_installed = true;
        return;
    }

    DWORD old = 0;
    void** first = &vtable[PEMF_VTBL_RESET];
    SIZE_T span  = (PEMF_VTBL_ENDSCENE - PEMF_VTBL_RESET + 1) * sizeof(void*);
    if (!VirtualProtect(first, span, PAGE_EXECUTE_READWRITE, &old)) {
        Log("d3d9: could not unprotect the vtable (err=%lu) -- giving up",
            GetLastError());
        g_gaveUp = true;
        return;
    }
    g_pemfGameDevice     = device;
    g_pemfDeviceVTable   = vtable;
    g_pemfOrigReset      = vtable[PEMF_VTBL_RESET];
    g_pemfOrigEndScene   = vtable[PEMF_VTBL_ENDSCENE];
    g_pemfOrigBeginScene = vtable[PEMF_VTBL_BEGINSCENE];
    g_pemfOrigPresent    = vtable[PEMF_VTBL_PRESENT];
    vtable[PEMF_VTBL_RESET]      = (void*)&PemfResetHook;
    vtable[PEMF_VTBL_ENDSCENE]   = (void*)&PemfEndSceneHook;
    vtable[PEMF_VTBL_BEGINSCENE] = (void*)&PemfBeginSceneHook;
    vtable[PEMF_VTBL_PRESENT]    = (void*)&PemfPresentHook;
    VirtualProtect(first, span, old, &old);

    g_installed = true;
    Log("d3d9: hooked the game's device 0x%p (vtable 0x%p, try %d) -- "
        "BeginScene 0x%p  EndScene 0x%p  Present 0x%p  Reset 0x%p  STAGE %d",
        device, vtable, g_tryCount, g_pemfOrigBeginScene, g_pemfOrigEndScene,
        g_pemfOrigPresent, g_pemfOrigReset, kStage);
}

inline void Uninstall()
{
    if (!g_pemfDeviceVTable) return;
    void** vtable = g_pemfDeviceVTable;
    void** first  = &vtable[PEMF_VTBL_RESET];
    SIZE_T span   = (PEMF_VTBL_ENDSCENE - PEMF_VTBL_RESET + 1) * sizeof(void*);
    DWORD old = 0;
    if (SlotReadable(first, span) &&
        VirtualProtect(first, span, PAGE_EXECUTE_READWRITE, &old)) {
        if (g_pemfOrigReset)      vtable[PEMF_VTBL_RESET]      = g_pemfOrigReset;
        if (g_pemfOrigEndScene)   vtable[PEMF_VTBL_ENDSCENE]   = g_pemfOrigEndScene;
        if (g_pemfOrigBeginScene) vtable[PEMF_VTBL_BEGINSCENE] = g_pemfOrigBeginScene;
        if (g_pemfOrigPresent)    vtable[PEMF_VTBL_PRESENT]    = g_pemfOrigPresent;
        VirtualProtect(first, span, old, &old);
    }
    g_installed = false;
    g_pemfDeviceVTable = nullptr;
}

// ------------------------------------------------------------- the pulse
// Called from the safe point, before anything else decides to do work.
//
// ⚠️ IT MUST ARM ITSELF FIRST. EndScene is not advancing during startup either
// -- the device does not exist yet -- and a naive "has it moved lately?" would
// declare PEMF dormant before the game ever drew a frame, which would stop the
// framework dead on every launch. So the pulse only begins judging once it has
// seen EndScene advance at least once.
//
// The threshold is deliberately generous. A game alt-tabbed away is quiet for
// minutes; a game that is merely slow, or sitting on a loading screen, must not
// be mistaken for one. A second of no frames at all is far outside anything the
// game does while someone is looking at it.
inline void SamplePulse()
{
    static LONG  armedAt   = 0;      // EndScene count when we first saw motion
    static LONG  lastCalls = 0;
    static DWORD lastMoved = 0;

    const LONG  calls = g_pemfEndSceneCalls;
    const DWORD now   = GetTickCount();

    if (!armedAt) {
        if (calls <= 0) return;      // never drawn yet -- nothing to judge
        armedAt = calls; lastCalls = calls; lastMoved = now;
        return;
    }

    if (calls != lastCalls) {        // drawing: awake
        lastCalls = calls;
        lastMoved = now;
        if (g_pemfNotRendering) {
            InterlockedExchange(&g_pemfNotRendering, 0);
            Log("d3d9: frames are back -- PEMF is awake again");
        }
        return;
    }

    if (!g_pemfNotRendering && (now - lastMoved) >= 1000) {
        InterlockedExchange(&g_pemfNotRendering, 1);
        Log("d3d9: no frames for 1s -- PEMF going dormant until they return");
    }
}

// True when there is no point doing any work: either a Reset is in flight, or
// the game has stopped drawing entirely.
inline bool Dormant()
{
    return g_pemfDeviceLost != 0
        || g_pemfNotRendering != 0
        || PemfSettlingAfterReset() != 0;
}

// Called from the safe point -- the top of the game's main loop, once per
// iteration. This is what makes "the first render pass of the frame" mean
// anything, and it is deliberately NOT tied to Present, which this game never
// calls on the device.
inline void MarkFrameBoundary()
{
    const LONG passes = InterlockedExchange(&g_pemfPassThisFrame, 0);
    if (passes > 0) g_pemfPassesLast = passes;
}

// Reported from the safe point, which is known to be sound to log from.
// After the first line, a heartbeat every 15s -- and an explicit warning if
// EndScene ever goes quiet, so a dead hook is never ambiguous.
inline void ReportFromSafePoint()
{
    if (!g_installed) return;
    if (!g_loggedFirst) {
        if (g_pemfEndSceneCalls <= 0) return;
        g_loggedFirst = true;
        Log("d3d9: STAGE %d alive -- %ld EndScene calls, game still running",
            kStage, g_pemfEndSceneCalls);
        return;
    }

    // How many render passes a displayed frame actually contains. Worth saying
    // once: more than one means world text has to pick a pass, and picking
    // wrong is what draws a notice twice in two different places.
    static LONG loggedPasses = 0;
    LONG passes = g_pemfPassesLast;
    if (passes > 0 && passes != loggedPasses) {
        loggedPasses = passes;
        Log("d3d9: %ld render pass(es) per frame (%ld BeginScene, %ld Present) "
            "-- world text draws in the first pass",
            passes, g_pemfBeginSceneCalls, g_pemfPresentCalls);
    }
    static DWORD lastT = 0;
    static LONG  lastCalls = 0;
    DWORD now = GetTickCount();
    if (lastT == 0) { lastT = now; lastCalls = g_pemfEndSceneCalls; return; }
    if (now - lastT < 15000) return;
    LONG calls = g_pemfEndSceneCalls;
    if (calls == lastCalls)
        Log("d3d9: WARNING -- EndScene silent for 15s (calls stuck at %ld)", calls);
    else
        Log("d3d9: heartbeat -- %ld EndScene calls (+%ld), engine objects live %ld",
            calls, calls - lastCalls, PemfLiveObjects());
    lastT = now; lastCalls = calls;
}

} // namespace d3d9hook
