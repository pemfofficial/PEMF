// d3d9hook.h - intercepting the frame at IDirect3DDevice9::EndScene.
//
// WHY THIS RATHER THAN AN ENGINE FUNCTION
//
// Redirecting the sailing-render call site (render.h) black-screens the game --
// proven with a callback that did nothing but increment a counter, so the
// interception itself was at fault. EndScene is a different proposition: it is
// a documented, stable API boundary that Direct3D calls at exactly the moment we
// want -- the scene is complete, nothing has been presented yet -- and it is the
// standard place overlays attach.
//
// HOW WE REACH THE DEVICE
//
// Every IDirect3DDevice9 created by a given d3d9.dll shares one vtable, which
// lives in that DLL's read-only data. So we do not need the game's device at
// all: we create a throwaway windowed device of our own, read the vtable
// address from it, release it, and patch the EndScene slot. The game's device
// then goes through our hook because it uses the same vtable.
//
// This is done during startup, before the game creates its own device, so the
// throwaway device never coexists with a fullscreen one.
//
// STAGES -- raise one at a time, testing in game between each:
//   0  not installed
//   1  installed; the hook ONLY increments a counter and calls the original
//   2  + draw notices
//   3  + present queued events
#pragma once
#include <windows.h>
#include "log.h"

// ---------------------------------------------------------------------------
// STATUS: the throwaway-device trick does NOT work here. Default stage is 0.
//
// Tested 2026-07-25. The good news: the game stayed completely stable -- it
// reached the world and sailed normally -- so unlike the call-site redirection,
// this approach is not destructive. It simply cannot reach the right vtable.
//
// Creating the throwaway device succeeded, and reading its vtable pointer gave
// 0x03AEB03C. After releasing the device, VirtualQuery on that address reports:
//
//     state = 0x2000 (MEM_RESERVE, i.e. decommitted)   protect = 0
//     VirtualProtect -> error 487 (ERROR_INVALID_ADDRESS)
//
// The vtable was freed together with the device. That means it is a
// PER-INSTANCE table on the heap, not the shared one in d3d9.dll's .rdata that
// the technique assumes -- so patching it would never have affected the game's
// device even if the write had succeeded.
//
// WHERE TO GO NEXT: get the game's OWN device pointer instead of manufacturing
// one. The engine is Gamebryo/NiDX9, and NiDX9Renderer holds the
// IDirect3DDevice9*. Finding where it is stored is ordinary RE of the kind that
// found the text API -- look for the renderer object and the device pointer
// inside it -- and then the same vtable patch applies to the real table.
//
// The code below is kept and fails safely (it logs and installs nothing), so a
// future attempt starts from something.
// ---------------------------------------------------------------------------
#ifndef PEMF_D3D9_STAGE
#define PEMF_D3D9_STAGE 0
#endif

// IDirect3DDevice9 vtable index of EndScene. Order is fixed by the interface:
// IUnknown occupies 0-2, and EndScene sits at 42 (BeginScene is 41).
#define PEMF_VTBL_ENDSCENE 42

extern "C" {

typedef HRESULT (WINAPI *PemfEndScene_t)(void* device);

void*          g_pemfOrigEndScene = nullptr;   // the real EndScene
void**         g_pemfDeviceVTable = nullptr;   // the shared device vtable
volatile LONG  g_pemfEndSceneCalls = 0;

// Implemented in core.cpp. Runs with a complete scene behind it.
void __cdecl PemfOnEndScene(void* device);

HRESULT WINAPI PemfEndSceneHook(void* device)
{
    InterlockedIncrement(&g_pemfEndSceneCalls);
    PemfOnEndScene(device);
    return ((PemfEndScene_t)g_pemfOrigEndScene)(device);
}

} // extern "C"

namespace d3d9hook {

constexpr int kStage = PEMF_D3D9_STAGE;

inline bool g_installed   = false;
inline bool g_loggedFirst = false;

inline bool WantsNotices() { return kStage >= 2; }
inline bool WantsPresent() { return kStage >= 3; }
inline bool Active()       { return g_installed; }

// Create a throwaway device purely to read the vtable address.
inline void** AcquireDeviceVTable()
{
    HMODULE d3d9 = LoadLibraryA("d3d9.dll");
    if (!d3d9) { Log("d3d9: could not load d3d9.dll"); return nullptr; }

    typedef void* (WINAPI *Create9_t)(UINT);
    auto create9 = (Create9_t)GetProcAddress(d3d9, "Direct3DCreate9");
    if (!create9) { Log("d3d9: no Direct3DCreate9 export"); return nullptr; }

    void* d3d = create9(32);              // D3D_SDK_VERSION for D3D9
    if (!d3d) { Log("d3d9: Direct3DCreate9 returned null"); return nullptr; }

    // A hidden 1x1 window to own the throwaway device.
    HWND wnd = CreateWindowExA(0, "STATIC", "pemf", WS_POPUP,
                               0, 0, 1, 1, nullptr, nullptr,
                               GetModuleHandleA(nullptr), nullptr);
    if (!wnd) {
        Log("d3d9: could not create the temporary window");
        ((void(WINAPI**)(void*))(*(void***)d3d))[2](d3d);   // Release
        return nullptr;
    }

    // D3DPRESENT_PARAMETERS, laid out by hand so this header needs no SDK
    // include. Only the fields we set matter; the rest stay zero.
    struct PresentParams {
        UINT  BackBufferWidth, BackBufferHeight;
        DWORD BackBufferFormat;            // D3DFMT_UNKNOWN = 0
        UINT  BackBufferCount;
        DWORD MultiSampleType; DWORD MultiSampleQuality;
        DWORD SwapEffect;                  // D3DSWAPEFFECT_DISCARD = 1
        HWND  hDeviceWindow;
        BOOL  Windowed;
        BOOL  EnableAutoDepthStencil;
        DWORD AutoDepthStencilFormat;
        DWORD Flags;
        UINT  FullScreen_RefreshRateInHz;
        UINT  PresentationInterval;
    } pp = {};
    pp.SwapEffect    = 1;                  // DISCARD
    pp.hDeviceWindow = wnd;
    pp.Windowed      = TRUE;

    // IDirect3D9::CreateDevice is vtable index 16.
    typedef HRESULT (WINAPI *CreateDevice_t)(void*, UINT, DWORD, HWND, DWORD,
                                             PresentParams*, void**);
    void** d3dVt = *(void***)d3d;
    void*  device = nullptr;
    HRESULT hr = ((CreateDevice_t)d3dVt[16])(
        d3d, 0 /*D3DADAPTER_DEFAULT*/, 1 /*D3DDEVTYPE_HAL*/, wnd,
        0x00000020 /*SOFTWARE_VERTEXPROCESSING*/ | 0x00000800 /*NOWINDOWCHANGES*/,
        &pp, &device);

    void** vtable = nullptr;
    if (SUCCEEDED(hr) && device) {
        vtable = *(void***)device;
        ((void(WINAPI**)(void*))vtable)[2](device);        // device->Release()
    } else {
        Log("d3d9: CreateDevice failed (hr=0x%08X)", (unsigned)hr);
    }

    ((void(WINAPI**)(void*))(*(void***)d3d))[2](d3d);      // d3d->Release()
    DestroyWindow(wnd);
    return vtable;
}

inline bool Install()
{
    if (kStage <= 0) {
        Log("d3d9: EndScene hook disabled (stage 0)");
        return false;
    }
    if (g_installed) return true;

    g_pemfDeviceVTable = AcquireDeviceVTable();
    if (!g_pemfDeviceVTable) {
        Log("d3d9: could not acquire the device vtable -- hook not installed");
        return false;
    }

    void** slot = &g_pemfDeviceVTable[PEMF_VTBL_ENDSCENE];

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(slot, &mbi, sizeof(mbi)))
        Log("d3d9: vtable 0x%p slot 0x%p  state=0x%X protect=0x%X type=0x%X",
            g_pemfDeviceVTable, slot, mbi.State, mbi.Protect, mbi.Type);

    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        Log("d3d9: could not unprotect the EndScene slot (err=%lu)",
            GetLastError());
        return false;
    }
    g_pemfOrigEndScene = *slot;
    *slot = (void*)&PemfEndSceneHook;
    VirtualProtect(slot, sizeof(void*), old, &old);

    g_installed = true;
    Log("d3d9: hooked EndScene (vtable 0x%p, orig 0x%p), STAGE %d",
        g_pemfDeviceVTable, g_pemfOrigEndScene, kStage);
    return true;
}

inline void Uninstall()
{
    if (!g_installed || !g_pemfDeviceVTable) return;
    void** slot = &g_pemfDeviceVTable[PEMF_VTBL_ENDSCENE];
    DWORD old = 0;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        *slot = g_pemfOrigEndScene;
        VirtualProtect(slot, sizeof(void*), old, &old);
    }
    g_installed = false;
}

// Reported from the safe point, which is known to be sound to log from.
inline void ReportFromSafePoint()
{
    if (!g_installed || g_loggedFirst) return;
    if (g_pemfEndSceneCalls <= 0) return;
    g_loggedFirst = true;
    Log("d3d9: STAGE %d alive -- %ld EndScene calls, game still running",
        kStage, g_pemfEndSceneCalls);
}

} // namespace d3d9hook
