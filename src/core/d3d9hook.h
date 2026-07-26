// d3d9hook.h - hooking the game's own IDirect3DDevice9.
//
// HOW WE REACH THE DEVICE
//
// The renderer singleton lives at game::addr::RendererPtr and stores the
// IDirect3DDevice9* at +0x60 (established from the CreateDevice call sequence
// and 19 independent read sites -- see game.h). We poll that pointer from the
// safe point; once the game has created its device we read the pointer, take
// its vtable, and patch the slots we need. Because it is the game's real
// device, the hooks are guaranteed to be on the vtable actually in use --
// unlike the earlier throwaway-device attempt, which discovered the vtable is
// per-instance and died with the throwaway.
//
// WHAT THE HOOKS DO
//
//   EndScene (slot 42)     counter + PemfOnEndScene (stages 2/3: draw/present)
//   Reset (slot 16)        logged; the vtable survives a reset, hooks persist
//   SetTransform (slot 44) the widescreen 2D-UI fix (below) + projection recon
//
// THE WIDESCREEN 2D-UI FIX
//
// The 2D UI is drawn through an orthographic projection sized for 4:3; at a
// wide resolution the same projection fills the whole viewport, so the UI
// stretches and the cursor stops lining up. Every earlier attempt to fix this
// at the source (patching the frustum constants, rewriting the heap-cached
// camera) failed because the value is copied long before it reaches the GPU.
// SetTransform is the one place it MUST pass through on its way to Direct3D,
// every frame. When the device aspect is wider than 4:3 we rescale the
// orthographic projection's X row about the viewport centre so the UI keeps
// its 4:3 proportions, centred, with the 3D world still full-width behind it.
// Orthographic matrices are identified by _34 == 0 and _44 == 1; the 3D
// perspective projection (_34 == 1, _44 == 0) passes through untouched.
//
// The mouse hit-test patch in core.cpp is the other half of this fix: the
// cursor transform is (x/width - 0.5) * 1024 -- centre-relative -- so with the
// UI occupying the centred 4:3 region the correct multiplier is
// 768 * (w/h), pure scale, no bias. The two halves engage together.
//
// RECON: every distinct projection the game sets is logged once (matrix
// diagonal + translation), so if a build routes the UI through something
// unexpected the log says exactly what came through.
//
// STAGES -- raise one at a time, testing in game between each:
//   0  not installed
//   1  installed; EndScene only counts; SetTransform fix + recon active
//   2  + draw notices from EndScene
//   3  + present queued events from EndScene
#pragma once
#include <windows.h>
#include <string.h>
#include "log.h"
#include "game.h"

#ifndef PEMF_D3D9_STAGE
#define PEMF_D3D9_STAGE 1
#endif

// IDirect3DDevice9 vtable slots. Order is fixed by the COM interface:
// IUnknown occupies 0-2, Reset is 16, EndScene 42, SetTransform 44.
#define PEMF_VTBL_RESET        16
#define PEMF_VTBL_ENDSCENE     42
#define PEMF_VTBL_SETTRANSFORM 44

#define PEMF_D3DTS_PROJECTION  3

extern "C" {

typedef HRESULT (WINAPI *PemfEndScene_t)(void* device);
typedef HRESULT (WINAPI *PemfReset_t)(void* device, void* params);
typedef HRESULT (WINAPI *PemfSetTransform_t)(void* device, DWORD state,
                                             const float* m);

void*          g_pemfOrigEndScene     = nullptr;
void*          g_pemfOrigReset        = nullptr;
void*          g_pemfOrigSetTransform = nullptr;
void*          g_pemfGameDevice       = nullptr;
void**         g_pemfDeviceVTable     = nullptr;
volatile LONG  g_pemfEndSceneCalls    = 0;
volatile LONG  g_pemfResetCount       = 0;

// X scale applied to orthographic projections. 1.0 = no-op (4:3 or fix off);
// (4/3)/(w/h) at a wide aspect. Written from the safe point on resolution
// change, read on the render thread -- a torn read of a float that only
// changes on a resolution switch is harmless.
volatile float g_pemfUiAspectScaleX   = 1.0f;

// Implemented in core.cpp. Runs with a complete scene behind it.
void __cdecl PemfOnEndScene(void* device);

// ---- projection recon: log each distinct projection once ------------------
struct PemfProjSeen { float m00, m11, m33, m41; };
PemfProjSeen  g_pemfProjSeen[24];
volatile LONG g_pemfProjSeenCount = 0;

static void PemfReconProjection(const float* m)
{
    LONG n = g_pemfProjSeenCount;
    if (n >= 24) return;
    for (LONG i = 0; i < n; ++i) {
        const PemfProjSeen& s = g_pemfProjSeen[i];
        if (s.m00 == m[0] && s.m11 == m[5] && s.m33 == m[10] && s.m41 == m[12])
            return;
    }
    if (InterlockedCompareExchange(&g_pemfProjSeenCount, n + 1, n) != n) return;
    g_pemfProjSeen[n] = { m[0], m[5], m[10], m[12] };
    Log("d3d9: projection #%ld  _11=%.6f _22=%.6f _33=%.6f _41=%.6f _34=%.3f _44=%.3f",
        n, m[0], m[5], m[10], m[12], m[11], m[15]);
}

// Per-frame classification, written by the SetTransform hook:
//  - SawUi:   the squeezed 4:3 UI camera passed through this frame
//  - SawWide: a genuinely widescreen projection (the 3D world camera at 16:9,
//             _22/_11 well above 4:3) passed through this frame
volatile LONG g_pemfSawUiProj   = 0;
volatile LONG g_pemfSawWideProj = 0;

// Written from the safe point (core.cpp) so the render thread never has to
// read game globals itself.
volatile LONG g_pemfDevW = 0;
volatile LONG g_pemfDevH = 0;

// On a UI-only widescreen frame the strips either side of the centred 4:3
// region are never drawn to -- they show stale pixels from whatever came
// before (verified in-game: fragments of earlier scenes). Black them out.
// Frames that contain full-width 3D (sailing, land) are left alone.
static void PemfClearSideBars(void* device)
{
    struct D3DRect { LONG x1, y1, x2, y2; };
    typedef HRESULT (WINAPI *Clear_t)(void*, DWORD, const D3DRect*, DWORD,
                                      DWORD, float, DWORD);
    LONG w = g_pemfDevW, h = g_pemfDevH;
    if (w <= 0 || h <= 0) return;
    LONG content = (h * 4) / 3;              // width of the centred 4:3 region
    LONG bar = (w - content) / 2;
    if (bar < 4) return;
    D3DRect rects[2] = { { 0, 0, bar, h }, { w - bar, 0, w, h } };
    void** vt = *(void***)device;
    ((Clear_t)vt[43])(device, 2, rects, 1 /*D3DCLEAR_TARGET*/,
                      0xFF000000, 1.0f, 0);
}

HRESULT WINAPI PemfEndSceneHook(void* device)
{
    InterlockedIncrement(&g_pemfEndSceneCalls);
    if (g_pemfUiAspectScaleX < 0.999f &&
        g_pemfSawUiProj && !g_pemfSawWideProj)
        PemfClearSideBars(device);
    g_pemfSawUiProj = 0;
    g_pemfSawWideProj = 0;
    PemfOnEndScene(device);
    return ((PemfEndScene_t)g_pemfOrigEndScene)(device);
}

HRESULT WINAPI PemfResetHook(void* device, void* params)
{
    InterlockedIncrement(&g_pemfResetCount);
    Log("d3d9: device Reset #%ld", g_pemfResetCount);
    return ((PemfReset_t)g_pemfOrigReset)(device, params);
}

HRESULT WINAPI PemfSetTransformHook(void* device, DWORD state, const float* m)
{
    if (state == PEMF_D3DTS_PROJECTION && m) {
        PemfReconProjection(m);
        float s = g_pemfUiAspectScaleX;
        // The 2D-UI camera, and only it: a perspective matrix with the fixed
        // 4:3 frustum from FUN_00503CA0 -- _11 == 2/(r-l) == 2.0 and
        // _33 == far/(far-near) == 1536/614.4 == 2.5, values no other camera
        // in the game produces (the 3D world camera has _33 ~= 1.0001).
        //
        // At a wide aspect the game lays the UI out to FIT THE WIDTH -- every
        // element keeps its proportions but the 4:3 whole is taller than the
        // screen and the top/bottom are cut off (verified in-game at
        // 1920x1080). Widening the camera's view uniformly by (4/3)/(w/h) on
        // both axes turns that width-fit into a height-fit: the full UI
        // becomes visible, centred, pixels stay square, and the centre-
        // relative mouse patch (768*(w/h)) lines up exactly.
        bool isPersp = (m[11] == 1.0f && m[15] == 0.0f);
        bool isUiCam = (isPersp && m[0] == 2.0f &&
                        m[10] > 2.49f && m[10] < 2.51f);
        // Frame classification only (no modification): a 4:3-ratio
        // perspective that is not the UI camera is a full-screen 3D camera
        // (sailing, land, taverns) -- those frames own their edge pixels and
        // must not get the side-bar cleanup. The 1:1 character-portrait
        // camera matches neither test, so portrait screens still count as
        // UI-only and get clean bars.
        if (isPersp && !isUiCam && m[5] > m[0] * 1.32f && m[5] < m[0] * 1.35f)
            g_pemfSawWideProj = 1;
        // The proven, banked widescreen fix -- and ONLY it: squeeze the UI
        // camera uniformly so the game's width-fit UI layout becomes a
        // height-fit one. Full UI visible, centred, square pixels, and the
        // adaptive mouse hit-test lines up (all verified in-game).
        //
        // DO NOT scale the 3D cameras here. It was tried: geometry that the
        // engine draws through VERTEX SHADERS receives its projection via
        // shader constants, not SetTransform, so scaling only the
        // SetTransform path tears mixed-pipeline objects (ships) apart.
        // True-widescreen 3D needs the shader-constant path handled too --
        // a documented future target, not a one-line patch.
        if (s < 0.999f && isUiCam) {
            g_pemfSawUiProj = 1;
            static float loggedScale = 1.0f;
            if (loggedScale != s) {
                loggedScale = s;
                Log("d3d9: ui height-fit engaged (s=%.4f)", s);
            }
            float t[16];
            memcpy(t, m, sizeof(t));
            t[0] *= s;
            t[5] *= s;
            return ((PemfSetTransform_t)g_pemfOrigSetTransform)(device, state, t);
        }
    }
    return ((PemfSetTransform_t)g_pemfOrigSetTransform)(device, state, m);
}

} // extern "C"

namespace d3d9hook {

constexpr int kStage = PEMF_D3D9_STAGE;

inline bool g_installed    = false;
inline bool g_loggedFirst  = false;
inline int  g_tryCount     = 0;

inline bool WantsNotices() { return kStage >= 2; }
inline bool WantsPresent() { return kStage >= 3; }
inline bool Active()       { return g_installed; }

// Never-fault readability check (the DRM-packed build makes this mandatory).
inline bool SlotReadable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    if ((const BYTE*)p + n > (const BYTE*)mbi.BaseAddress + mbi.RegionSize)
        return false;
    return true;
}

// Update the ortho X scale (and report what it implies). Called from the safe
// point whenever the device resolution changes.
inline void SetAspect(int devW, int devH)
{
    g_pemfDevW = devW;
    g_pemfDevH = devH;
    float scale = 1.0f;
    if (devW > 0 && devH > 0) {
        float aspect = (float)devW / (float)devH;
        if (aspect > 4.0f / 3.0f + 0.01f)
            scale = (4.0f / 3.0f) / aspect;      // e.g. 0.75 at 16:9
    }
    g_pemfUiAspectScaleX = scale;
    Log("d3d9: ui aspect scale = %.4f for %dx%d%s", scale, devW, devH,
        g_installed ? "" : "  (device not hooked yet)");
}

// Poll the game's device pointer; hook its vtable the moment it exists.
// Called from the safe point, so this runs on the game thread with the game
// idle -- no render call is in flight while we swap the slots.
inline bool g_gaveUp = false;

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

    // Already hooked and the device is unchanged: verify the hooks are still
    // in the slots (a destroy-and-recreate can land at the same address with a
    // rebuilt vtable, silently shedding them) -- healthy means nothing to do.
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
    if (!vtable || !SlotReadable(vtable, (PEMF_VTBL_SETTRANSFORM + 1) * sizeof(void*)))
        return;

    // A recreated device can come back with the SAME vtable. If our hooks are
    // already in these slots, re-hooking would capture our own hook as the
    // "original" -- just adopt the new device pointer.
    if (vtable[PEMF_VTBL_ENDSCENE] == (void*)&PemfEndSceneHook) {
        Log("d3d9: new device 0x%p reuses the hooked vtable 0x%p", device, vtable);
        g_pemfGameDevice = device;
        g_installed = true;
        return;
    }
    if (g_installed)
        Log("d3d9: device changed 0x%p -> 0x%p -- hooking the new vtable",
            g_pemfGameDevice, device);

    DWORD old = 0;
    void** first = &vtable[PEMF_VTBL_RESET];
    SIZE_T span  = (PEMF_VTBL_SETTRANSFORM - PEMF_VTBL_RESET + 1) * sizeof(void*);
    if (!VirtualProtect(first, span, PAGE_EXECUTE_READWRITE, &old)) {
        Log("d3d9: could not unprotect the vtable (err=%lu) -- giving up",
            GetLastError());
        g_gaveUp = true;
        return;
    }
    g_pemfGameDevice       = device;
    g_pemfDeviceVTable     = vtable;
    g_pemfOrigReset        = vtable[PEMF_VTBL_RESET];
    g_pemfOrigEndScene     = vtable[PEMF_VTBL_ENDSCENE];
    g_pemfOrigSetTransform = vtable[PEMF_VTBL_SETTRANSFORM];
    vtable[PEMF_VTBL_RESET]        = (void*)&PemfResetHook;
    vtable[PEMF_VTBL_ENDSCENE]     = (void*)&PemfEndSceneHook;
    vtable[PEMF_VTBL_SETTRANSFORM] = (void*)&PemfSetTransformHook;
    VirtualProtect(first, span, old, &old);

    g_installed = true;
    Log("d3d9: hooked the game's device 0x%p (vtable 0x%p, try %d) -- "
        "EndScene 0x%p  Reset 0x%p  SetTransform 0x%p  STAGE %d",
        device, vtable, g_tryCount,
        g_pemfOrigEndScene, g_pemfOrigReset, g_pemfOrigSetTransform, kStage);
}

inline void Uninstall()
{
    if (!g_pemfDeviceVTable) return;
    void** vtable = g_pemfDeviceVTable;
    DWORD old = 0;
    void** first = &vtable[PEMF_VTBL_RESET];
    SIZE_T span  = (PEMF_VTBL_SETTRANSFORM - PEMF_VTBL_RESET + 1) * sizeof(void*);
    if (SlotReadable(first, span) &&
        VirtualProtect(first, span, PAGE_EXECUTE_READWRITE, &old)) {
        if (g_pemfOrigReset)        vtable[PEMF_VTBL_RESET]        = g_pemfOrigReset;
        if (g_pemfOrigEndScene)     vtable[PEMF_VTBL_ENDSCENE]     = g_pemfOrigEndScene;
        if (g_pemfOrigSetTransform) vtable[PEMF_VTBL_SETTRANSFORM] = g_pemfOrigSetTransform;
        VirtualProtect(first, span, old, &old);
    }
    g_installed = false;
    g_pemfDeviceVTable = nullptr;
}

// Reported from the safe point, which is known to be sound to log from.
// After the first alive line, a heartbeat every 15s -- with an explicit
// warning when EndScene has gone silent, so a dead hook is never ambiguous.
inline void ReportFromSafePoint()
{
    if (!g_installed) return;
    if (!g_loggedFirst) {
        if (g_pemfEndSceneCalls <= 0) return;
        g_loggedFirst = true;
        Log("d3d9: STAGE %d alive -- %ld EndScene calls, %ld projections seen",
            kStage, g_pemfEndSceneCalls, g_pemfProjSeenCount);
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
        Log("d3d9: heartbeat -- calls=%ld (+%ld)  proj=%ld  scale=%.4f",
            calls, calls - lastCalls, g_pemfProjSeenCount,
            (double)g_pemfUiAspectScaleX);
    lastT = now; lastCalls = calls;
}

} // namespace d3d9hook
