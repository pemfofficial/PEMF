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
// occupies 0-2, Reset is 16, BeginScene 41, EndScene 42.
#define PEMF_VTBL_RESET    16
#define PEMF_VTBL_ENDSCENE 42

extern "C" {

typedef HRESULT (WINAPI *PemfEndScene_t)(void* device);
typedef HRESULT (WINAPI *PemfReset_t)(void* device, void* params);

void*          g_pemfOrigEndScene  = nullptr;   // the real EndScene
void*          g_pemfOrigReset     = nullptr;   // the real Reset
void*          g_pemfGameDevice    = nullptr;   // the game's IDirect3DDevice9*
void**         g_pemfDeviceVTable  = nullptr;   // its vtable
volatile LONG  g_pemfEndSceneCalls = 0;
volatile LONG  g_pemfResetCount    = 0;

// Implemented in core.cpp. Runs with a complete scene behind it.
void __cdecl PemfOnEndScene(void* device);

HRESULT WINAPI PemfEndSceneHook(void* device)
{
    InterlockedIncrement(&g_pemfEndSceneCalls);
    PemfOnEndScene(device);
    return ((PemfEndScene_t)g_pemfOrigEndScene)(device);
}

// A device reset means a resolution change or a lost device; worth seeing in
// the log because it is also when the vtable tends to be rebuilt.
HRESULT WINAPI PemfResetHook(void* device, void* params)
{
    InterlockedIncrement(&g_pemfResetCount);
    Log("d3d9: device Reset #%ld", g_pemfResetCount);
    return ((PemfReset_t)g_pemfOrigReset)(device, params);
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
            vt[PEMF_VTBL_ENDSCENE] == (void*)&PemfEndSceneHook)
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
    if (vtable[PEMF_VTBL_ENDSCENE] == (void*)&PemfEndSceneHook) {
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
    g_pemfGameDevice   = device;
    g_pemfDeviceVTable = vtable;
    g_pemfOrigReset    = vtable[PEMF_VTBL_RESET];
    g_pemfOrigEndScene = vtable[PEMF_VTBL_ENDSCENE];
    vtable[PEMF_VTBL_RESET]    = (void*)&PemfResetHook;
    vtable[PEMF_VTBL_ENDSCENE] = (void*)&PemfEndSceneHook;
    VirtualProtect(first, span, old, &old);

    g_installed = true;
    Log("d3d9: hooked the game's device 0x%p (vtable 0x%p, try %d) -- "
        "EndScene 0x%p  Reset 0x%p  STAGE %d",
        device, vtable, g_tryCount, g_pemfOrigEndScene, g_pemfOrigReset, kStage);
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
        if (g_pemfOrigReset)    vtable[PEMF_VTBL_RESET]    = g_pemfOrigReset;
        if (g_pemfOrigEndScene) vtable[PEMF_VTBL_ENDSCENE] = g_pemfOrigEndScene;
        VirtualProtect(first, span, old, &old);
    }
    g_installed = false;
    g_pemfDeviceVTable = nullptr;
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
    static DWORD lastT = 0;
    static LONG  lastCalls = 0;
    DWORD now = GetTickCount();
    if (lastT == 0) { lastT = now; lastCalls = g_pemfEndSceneCalls; return; }
    if (now - lastT < 15000) return;
    LONG calls = g_pemfEndSceneCalls;
    if (calls == lastCalls)
        Log("d3d9: WARNING -- EndScene silent for 15s (calls stuck at %ld)", calls);
    else
        Log("d3d9: heartbeat -- %ld EndScene calls (+%ld)", calls, calls - lastCalls);
    lastT = now; lastCalls = calls;
}

} // namespace d3d9hook
