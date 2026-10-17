// widescreen.h - widescreen and higher resolutions: every display mode in the
// menu, a wider 3D view, and the 4:3 interface centred with a working mouse.
//
// ============================================================================
// WHY THE FIRST ATTEMPT BROKE, AND WHAT THIS DOES INSTEAD
// ============================================================================
// See docs/WIDESCREEN.md for the full story. In short, the July attempt
// corrected projections in SetTransform, and ships tore in half: the engine
// also feeds shader-drawn geometry through vertex-shader constants, which
// SetTransform never sees. It also patched one of eight mouse conversions.
//
// This version works one layer up, where both of those problems disappear.
//
// 1. THE CAMERA CHOKE POINT. FUN_004CCE40 is NiCamera's set-frustum: every
//    camera in the game gets its view volume through it (24 call sites, all
//    `call rel32`, verified), and the renderer derives BOTH the fixed-function
//    projection and the shader constants from the camera afterwards. Decompiled:
//
//        ecx = camera, edx = NiFrustum* {l, r, t, b, near, far, ortho}
//        near = max(near, far / 10000.0, 0.1)        ; 0x007177E4, 0x007177E0
//        camera+0x128..0x140 = l, r, t, b, near, far, ortho
//        jmp FUN_00536F80                             ; mark dependents dirty
//
//    We redirect all 24 calls to a stub that corrects the frustum, then calls
//    the ORIGINAL function with it. The game's own code still does the work.
//
//    The original preserves ECX (FUN_00536F80 never writes it) and this is an
//    optimised MSVC 7.1 build, so a caller may rely on that. The stub saves and
//    restores ECX around our C++ code. EAX and EDX it clobbers, as the original.
//
//    - World cameras are exactly 4:3 at every resolution. Their left/right are
//      scaled by (3W / 4H): the vertical field of view is kept and more of the
//      world shows at the sides ("Hor+").
//    - The UI camera (far/near = 5/3, left/right = -/+0.5) is built for a 4:3
//      screen; at 16:9 SetResolution gives it top/bottom = (H/W)*0.5 = 0.28,
//      which cuts off the top and bottom of the 480-tall interface. We fit the
//      HEIGHT instead: top/bottom = +/-0.375, left/right = -/+0.375 * W/H. The
//      interface is whole and centred, with margins at the sides.
//
//    Many call sites copy the camera's CURRENT frustum, change near/far, and
//    set it again. The correction is therefore written to be idempotent: a
//    corrected world frustum is no longer 4:3, a corrected UI frustum is no
//    longer +/-0.5 wide, and neither matches again.
//
// 2. THE MOUSE. The interface is now centred, but the game converts the cursor
//    with the full window width. Every mouse message reaches the game's handler
//    (FUN_0042C170, via the relay at 0x004D3E00) carrying the Windows lParam,
//    and BOTH the UI cursor and the widget hit-test (FUN_00504310) are computed
//    from that lParam. So we subclass the game window -- a Windows API, no code
//    write -- and move the cursor X from the centred 4:3 area back to the range
//    the game expects, before the game sees it. One place, not eight operands.
//
// 3. THE MODE LIST. FUN_004B2DF0 keeps only modes with height*100/width == 75,
//    i.e. 4:3. PEMF replaces it at its one outside call with a builder that
//    lists every 32-bit resolution once (see "mode list" below for why simply
//    lifting the 4:3 test was not enough). Selecting one runs the game's own
//    switch (FUN_004D3AB0), which July showed to be crash-free from the menu.
//
// ⛔ ALL OF THIS IS A CODE WRITE -- ONLY FROM THE FIRST SAFE POINT, like storms
// and sailkeys. Patching while the Steam wrapper is still checking its image
// gives "Application corrupt."
//
// Cross-checked against Pirates! Unbound (MIT, github.com/twitchplayskh/
// Pirates-Unbound), which hooks the same choke point and handler. Nothing of
// its code is used; every address here was confirmed against our own
// decompilation of the GOG build (the Steam build is the same code unpacked).
// 4. FULL-SCREEN 2D LAYERS KEEP THEIR SHAPE. Painted menu screens, fades, the
//    dialogue shading and the cutscene letterbox bars are 640-wide quads drawn
//    by the UI camera, so after height-fitting they cover only the centre --
//    which left empty sides behind menus, and black bars only in the middle of
//    cutscenes with the wider 3D showing past them. Paintings are scaled evenly
//    to fill the width ("cover"), solid strips are drawn full width, and the
//    sailing map's parchment stays 4:3 with black sides. See "backdrops" below.
#pragma once
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "log.h"
#include "game.h"
#include "render.h"   // RedirectCall

namespace widescreen {

// ------------------------------------------------------------------ addresses
constexpr uintptr_t kSetViewFrustum = 0x004CCE40;  // NiCamera set-frustum
constexpr uintptr_t kDeviceW        = 0x008CAC18;  // int, real backbuffer width
constexpr uintptr_t kDeviceH        = 0x008CAC1C;  // int, real backbuffer height
constexpr uintptr_t kUiCamera       = 0x008E9FE0;  // NiCamera*, the interface
constexpr uintptr_t kGameWindow     = 0x00726B8C;  // HWND, as passed to MoveWindow
// The town / full-screen card scene (tm_<n>_menu.nif). FUN_004108A0 sets it
// before activating the scene's camera; ShowMessage (FUN_00410C50) zeroes it
// when the card closes. Non-zero exactly while such a scene is up.
constexpr uintptr_t kCardScene      = 0x008CADEC;

// Every caller of kSetViewFrustum. All 24 are `E8 rel32` to it -- checked
// against the image, and checked again at runtime before anything is written.
constexpr uintptr_t kFrustumCalls[] = {
    0x004253E5, 0x004630DE, 0x0046331F, 0x004673F4, 0x0046F488, 0x00486796,
    0x00490EB6, 0x00491354, 0x00494CC2, 0x004C81F9, 0x004C829C, 0x004CEAB9,
    0x004CF316, 0x004D3511, 0x004D3B78, 0x004D975D, 0x004DA9E8, 0x004FF57E,
    0x004FF5E3, 0x00500E85, 0x00501055, 0x00503DC2, 0x00510F40, 0x005115FC,
};
constexpr int kFrustumCallCount = (int)(sizeof(kFrustumCalls) / sizeof(kFrustumCalls[0]));

// Gamebryo's NiFrustum, as FUN_004CCE40 reads it.
struct NiFrustum {
    float l, r, t, b, n, f;
    unsigned char ortho;
};
static_assert(offsetof(NiFrustum, ortho) == 0x18, "NiFrustum layout");
constexpr size_t kCameraFrustum = 0x128;   // where the camera keeps its copy

// ------------------------------------------------------------------- settings
struct Settings {
    int enabled     = 1;   // master switch
    int unlockModes = 1;   // offer every resolution, not only 4:3
    int widenWorld  = 1;   // more world at the sides instead of a cropped view
    int centerUi    = 1;   // whole 4:3 interface, centred, mouse corrected
    int autoShots   = 0;   // seconds between automatic screenshots, 0 = off
    int hudCorners  = 1;   // HUD in the screen corners (hudcorners.h)
};
inline Settings g_set;
inline char     g_shotDir[MAX_PATH] = {0};

// ---------------------------------------------------------------------- state
inline bool    g_installed      = false;
inline void*   g_frustumOrig    = nullptr;   // == kSetViewFrustum once verified
inline HWND    g_wnd            = nullptr;
inline WNDPROC g_wndOrig        = nullptr;
inline LONG    g_worldFixes     = 0;
inline LONG    g_uiFixes        = 0;
inline LONG    g_loggedFixes    = 0;

// The last mouse message as the window delivered it, before MapX, so
// hudcorners.h can put the cursor in a margin when a HUD element is there.
struct RawMouse { bool valid; int x, mappedX, cw, ch; };
inline RawMouse g_rawMouse = {};

typedef void(__fastcall* SetViewFrustumFn)(void* camera, const NiFrustum* f);

inline bool Close(float a, float b) { return fabsf(a - b) < 0.0002f; }

inline bool Readable(const void* p, size_t bytes)
{
    MEMORY_BASIC_INFORMATION m{};
    if (!p || !VirtualQuery(p, &m, sizeof(m))) return false;
    if (m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return (uintptr_t)p + bytes <= (uintptr_t)m.BaseAddress + m.RegionSize;
}

inline void DeviceSize(int* w, int* h)
{
    *w = *(const int*)kDeviceW;
    *h = *(const int*)kDeviceH;
}

// Wider than 4:3? Narrower and square screens are left exactly as shipped.
inline bool IsWide(int w, int h) { return w > 0 && h > 0 && 3LL * w > 4LL * h; }

// -------------------------------------------------------- frustum correction
enum class Kind { None, World, Ui, Card };

// An interface camera, as the game asks for it: far/near = 5/3 (921.6/1536 at
// startup, 0.9D/1.5D from SetResolution), +/-0.5 across, symmetric, no taller
// than 4:3. A corrected one is wider than +/-0.5 and no longer matches.
inline bool IsUiFrustum(const NiFrustum& f)
{
    return !f.ortho && f.n > 0.f && f.f > f.n && Close(f.f / f.n, 5.f / 3.f) &&
           Close(f.l, -0.5f) && Close(f.r, 0.5f) && f.t > 0.f && f.t <= 0.3751f &&
           Close(f.t, -f.b);
}

// Correct one frustum for a W x H backbuffer. Returns what it changed.
inline Kind Correct(NiFrustum& f, int w, int h)
{
    if (!g_set.enabled || !IsWide(w, h)) return Kind::None;
    if (f.ortho || !(f.n > 0.f) || !(f.f > f.n)) return Kind::None;
    const float dx = f.r - f.l, dy = f.t - f.b;
    if (dx <= 0.f || dy <= 0.f) return Kind::None;

    if (IsUiFrustum(f)) {
        if (!g_set.centerUi) return Kind::None;
        const float half = 0.375f * (float)w / (float)h;
        f.l = -half; f.r = half; f.t = 0.375f; f.b = -0.375f;
        return Kind::Ui;
    }

    // A world camera: exactly 4:3, whatever the resolution.
    if (!g_set.widenWorld || fabsf(dx / dy - 4.f / 3.f) >= 0.0002f) return Kind::None;

    // A town or card scene is a composed picture (painting, clouds, flags), all
    // seen through this one camera. Showing "more world" there only reveals
    // the edge of the set, so it gets COVER: same width, the top and bottom
    // trimmed evenly. Everything in the picture scales together.
    const void* card = nullptr;
    __try { card = *(const void* const*)kCardScene; }
    __except (EXCEPTION_EXECUTE_HANDLER) { card = nullptr; }
    if (card) {
        const float c = (4.f * (float)h) / (3.f * (float)w);
        f.t *= c; f.b *= c;
        return Kind::Card;
    }

    const float s = (3.f * (float)w) / (4.f * (float)h);
    f.l *= s; f.r *= s;
    return Kind::World;
}

inline int g_lastFixW = 0, g_lastFixH = 0;

// ⚠️ DIAGNOSTIC. Every camera call site, once, with what it asked for and what
// PEMF did about it. A UI camera that slips past the classifier shows here as
// "unchanged" -- the suspected cause of the clipped dialog frame (round 7).
inline uintptr_t g_sitesSeen[64] = {0};
inline LONG      g_sitesCount = 0;

inline void NoteSite(uintptr_t site, const NiFrustum& f, Kind k)
{
    const LONG n = g_sitesCount;
    for (LONG i = 0; i < n && i < 64; ++i) if (g_sitesSeen[i] == site) return;
    if (n >= 64) return;
    g_sitesSeen[n] = site;
    g_sitesCount = n + 1;
    Log("widescreen: camera site 0x%08X -> %s | l=%.4f r=%.4f t=%.4f b=%.4f near=%.2f "
        "far=%.2f ortho=%d", (unsigned)(site - 5), k == Kind::None ? "unchanged" :
        k == Kind::Ui ? "UI height-fit" : k == Kind::Card ? "town/card cover" : "world Hor+",
        f.l, f.r, f.t, f.b, f.n, f.f, (int)f.ortho);
}

inline void NoteFix(Kind k, const NiFrustum& f, int w, int h)
{
    if (k == Kind::Ui) InterlockedIncrement(&g_uiFixes);
    else               InterlockedIncrement(&g_worldFixes);
    // A new resolution starts a new sample, so every mode tried is on record.
    if (w != g_lastFixW || h != g_lastFixH) {
        g_lastFixW = w;
        g_lastFixH = h;
        InterlockedExchange(&g_loggedFixes, 0);
        Log("widescreen: resolution now %dx%d", w, h);
    }
    // The first few, in full, so a log shows exactly what each camera became.
    if (InterlockedIncrement(&g_loggedFixes) <= 8) {
        Log("widescreen: %s camera at %dx%d -> l=%.4f r=%.4f t=%.4f b=%.4f "
            "near=%.1f far=%.1f", k == Kind::World ? "world" : k == Kind::Card ? "town/card (cover)" : "UI",
            w, h,
            f.l, f.r, f.t, f.b, f.n, f.f);
    }
}

// NiAVObject::Update(time, updateControllers), thiscall, `ret 8`. Through the
// camera's world-data update it rebuilds the six culling planes from the
// frustum (FUN_005373F0) -- the only place they are built. Setting a frustum
// alone leaves the old planes in force. The game calls it after every
// set-frustum it makes outside the per-frame paths.
constexpr uintptr_t kObjectUpdate = 0x00533D10;
constexpr uint8_t   kObjectUpdateEntry[] = { 0x8B, 0x54, 0x24, 0x04, 0x56, 0x8B, 0xF1 };
typedef void(__fastcall* ObjectUpdateFn)(void* obj, void* unusedEdx, float time, int controllers);

inline bool UpdateCamera(void* cam)
{
    if (memcmp((const void*)kObjectUpdate, kObjectUpdateEntry, sizeof(kObjectUpdateEntry)) != 0)
        return false;
    ((ObjectUpdateFn)kObjectUpdate)(cam, nullptr, 0.0f, 1);   // as 0x004D3BBB does
    return true;
}

// ----------------------------------------------------------- interface layers
// FUN_00503CA0 builds an interface LAYER with a camera of its own (a heap
// object, camera at +0; built from 0x004B1794, 0x004D5AE2 and 0x004DD1DD) and
// always gives it the fixed 4:3 view: l/r -/+0.5, t/b +/-0.375, near/far
// 921.6/1536. That set-frustum passes through us, so a layer built at a wide
// resolution is corrected. But SetResolution re-fits only the main interface
// camera (0x008E9FE0). A layer built at 4:3 stayed 4:3 after a switch to 16:9
// and was stretched across the width -- round 11: the Options slider knobs, the
// menu's icon bar, character creation, and mouse targets that no longer matched
// what was drawn. A layer built wide would stay wide after a switch back.
//
// So every interface camera that passes through is remembered with the frustum
// the game asked for, and re-fitted, then updated, when SetResolution runs.
// Before touching one we check it is still the camera we set: same vtable, a
// live reference count (Gamebryo deletes at zero), and exactly the frustum we
// left in it. Anything else is forgotten, never written.
struct UiLayer {
    void*     cam;
    uintptr_t vtbl;
    NiFrustum asked;     // what the game passed
    NiFrustum applied;   // what the camera holds since we set it
};
constexpr int       kMaxUiLayers      = 32;
constexpr uintptr_t kSetResolutionRet = 0x004D3B7D;   // return of the call at 0x004D3B78
constexpr size_t    kCameraSpan       = kCameraFrustum + sizeof(NiFrustum);
inline UiLayer g_uiLayers[kMaxUiLayers] = {};
inline int     g_uiLayerNext = 0;

inline bool ReadFrustum(void* cam, NiFrustum* out)
{
    if (!Readable(cam, kCameraSpan)) return false;
    memcpy(out, (const uint8_t*)cam + kCameraFrustum, sizeof(*out));
    return true;
}

inline bool SameFrustum(const NiFrustum& a, const NiFrustum& b)
{
    return a.l == b.l && a.r == b.r && a.t == b.t && a.b == b.b && a.n == b.n &&
           a.f == b.f && a.ortho == b.ortho;
}

inline bool LayerAlive(const UiLayer& e)
{
    NiFrustum now;
    if (!e.cam || !ReadFrustum(e.cam, &now)) return false;
    return *(const uintptr_t*)e.cam == e.vtbl && *(const LONG*)((const uint8_t*)e.cam + 4) > 0 &&
           SameFrustum(now, e.applied);
}

inline void RememberUiLayer(void* cam, const NiFrustum& asked)
{
    if (!cam || cam == *(void* const*)kUiCamera) return;   // SetResolution handles that one
    UiLayer e{};
    e.cam = cam;
    e.asked = asked;
    if (!ReadFrustum(cam, &e.applied)) return;
    e.vtbl = *(const uintptr_t*)cam;
    for (int i = 0; i < kMaxUiLayers; ++i)
        if (g_uiLayers[i].cam == cam) { g_uiLayers[i] = e; return; }
    for (int i = 0; i < kMaxUiLayers; ++i)
        if (!g_uiLayers[i].cam || !LayerAlive(g_uiLayers[i])) { g_uiLayers[i] = e; return; }
    g_uiLayers[g_uiLayerNext] = e;                           // full: oldest goes
    g_uiLayerNext = (g_uiLayerNext + 1) % kMaxUiLayers;
}

inline void RefitUiLayers(SetViewFrustumFn orig, int w, int h)
{
    int refit = 0, dropped = 0;
    for (int i = 0; i < kMaxUiLayers; ++i) {
        UiLayer& e = g_uiLayers[i];
        if (!e.cam) continue;
        if (!LayerAlive(e)) { e = UiLayer{}; ++dropped; continue; }
        NiFrustum local = e.asked;
        Correct(local, w, h);              // at 4:3 or narrower: the game's own
        orig(e.cam, &local);
        UpdateCamera(e.cam);
        ReadFrustum(e.cam, &e.applied);
        ++refit;
    }
    Log("widescreen: %d interface layer camera(s) re-fitted for %dx%d (%d gone)",
        refit, w, h, dropped);
}

} // namespace widescreen

extern "C" {

// Same contract as FUN_004CCE40: ecx = camera, edx = frustum (__fastcall).
// The stub adds the game's return address as a third (stack) argument, so the
// first visit of each call site can be logged -- see NoteSite.
// Not inline: only the assembly below references it, and it must be emitted.
void __fastcall PemfSetViewFrustum(void* camera, const widescreen::NiFrustum* f, uintptr_t site)
{
    using namespace widescreen;
    const auto orig = (SetViewFrustumFn)g_frustumOrig;
    int w = 0, h = 0;
    DeviceSize(&w, &h);

    NiFrustum local = *f;          // never write through the caller's pointer
    const Kind k = Correct(local, w, h);
    NoteSite(site, *f, k);
    if (k == Kind::None) {
        orig(camera, f);
    } else {
        NoteFix(k, local, w, h);
        orig(camera, &local);
    }

    // Interface layers keep their camera across a resolution change; see
    // RefitUiLayers. Remember each, and re-fit them all when SetResolution runs.
    if (g_set.centerUi) {
        __try {
            if (IsUiFrustum(*f)) RememberUiLayer(camera, *f);
            if (site == kSetResolutionRet) RefitUiLayers(orig, w, h);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("widescreen: faulted re-fitting interface layers -- list cleared");
            memset(g_uiLayers, 0, sizeof(g_uiLayers));
        }
    }
}

// The call sites land here. Keep ECX for the caller -- the original does.
// After `push ecx`, [esp+4] is the game's return address: passed on as the
// third (__fastcall stack) argument, which the callee pops.
__declspec(naked) inline void PemfSetViewFrustumStub()
{
    __asm {
        push ecx
        push dword ptr [esp + 4]
        call PemfSetViewFrustum     // ecx, edx are already the first two
        pop  ecx
        ret
    }
}

} // extern "C"

namespace widescreen {

// ---------------------------------------------------------------------- mouse
// Map a client-area X from the centred 4:3 area to the full width, which is
// what the game's conversion (x * 640 / width) and hit-test assume.
inline int MapX(int x, int cw, int ch)
{
    const double safeW = ch * 4.0 / 3.0;
    const double left  = (cw - safeW) * 0.5;
    int nx = (int)floor((x - left) * cw / safeW + 0.5);
    if (nx < 0) nx = 0;
    if (nx > cw - 1) nx = cw - 1;
    return nx;
}

// ⚠️ DIAGNOSTIC: clicks were reported "hit or miss" after changing resolution.
// Each button press is logged with every size involved, so the next log says
// whether the window, the device and the renderer agree, and what we did.
inline LONG g_clicksLogged = 0;

inline void LogClick(UINT msg, int inX, int outX, int y, int cw, int ch, bool mapped)
{
    if (msg != WM_LBUTTONDOWN && msg != WM_RBUTTONDOWN) return;
    const LONG n = InterlockedIncrement(&g_clicksLogged);
    if (n > 40 && (n % 25) != 0) return;
    int dw = 0, dh = 0;
    DeviceSize(&dw, &dh);
    Log("widescreen/mouse: click #%ld client=%dx%d device=%dx%d x %d -> %d y=%d%s",
        n, cw, ch, dw, dh, inX, outX, y,
        mapped ? "" : " (not mapped: not wide)");
}

inline LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg >= WM_MOUSEFIRST && msg <= 0x020E && g_set.enabled && g_set.centerUi) {
        RECT rc{};
        if (GetClientRect(hwnd, &rc)) {
            POINT p{ (short)LOWORD(lp), (short)HIWORD(lp) };
            const int inX = p.x;
            const bool wide = IsWide(rc.right, rc.bottom);
            if (wide) {
                // Wheel messages carry SCREEN coordinates; the rest are client.
                const bool screen = (msg == WM_MOUSEWHEEL || msg == 0x020E);
                if (screen) ScreenToClient(hwnd, &p);
                const int rawX = p.x;
                p.x = MapX(p.x, rc.right, rc.bottom);
                g_rawMouse = { true, rawX, p.x, (int)rc.right, (int)rc.bottom };
                if (screen) ClientToScreen(hwnd, &p);
                lp = MAKELPARAM((WORD)(short)p.x, (WORD)(short)p.y);
            }
            LogClick(msg, inX, p.x, p.y, rc.right, rc.bottom, wide);
        }
    }
    return CallWindowProcA(g_wndOrig, hwnd, msg, wp, lp);
}

inline bool SubclassWindow()
{
    HWND hwnd = *(const HWND*)kGameWindow;
    if (!hwnd || !IsWindow(hwnd)) {
        Log("widescreen: no game window at 0x%08X yet -- mouse left as is",
            (unsigned)kGameWindow);
        return false;
    }
    WNDPROC prev = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)&WndProc);
    if (!prev) {
        Log("widescreen: could not subclass the game window (err=%lu)", GetLastError());
        return false;
    }
    g_wnd = hwnd;
    g_wndOrig = prev;
    Log("widescreen: mouse follows the centred interface (window 0x%p)", hwnd);
    return true;
}

inline void UnsubclassWindow()
{
    if (!g_wnd || !g_wndOrig) return;
    // Only if nobody has subclassed on top of us; otherwise leave the chain be.
    if (IsWindow(g_wnd) && (WNDPROC)GetWindowLongPtrA(g_wnd, GWLP_WNDPROC) == &WndProc)
        SetWindowLongPtrA(g_wnd, GWLP_WNDPROC, (LONG_PTR)g_wndOrig);
    g_wnd = nullptr;
    g_wndOrig = nullptr;
}

// ------------------------------------------------------------------ mode list
// FUN_004B2DF0 builds the Options resolution list (decompiled):
//
//     int __cdecl BuildModeList(Entry** table)     ; Entry = {mode, w, h}, 12 bytes
//     renderer = *(0x008EA08C); info = *(renderer + 0x280)
//     modes = (D3DDISPLAYMODE**)(info + 0x454), count = *(info + 0x45C)
//     keep: Format == X8R8G8B8 (0x16), Width >= 640, Height*100/Width == 75
//     *table == 0: allocate (n+1)*12 via 0x00533C9D, fill, end with {-1,-1,-1}
//
// Lifting only the 4:3 test (the July patch) exposes EVERY adapter mode: 62 on
// the development machine, 1440x1080 alone twelve times over, one per refresh
// rate. The slider was built for a handful of stops; with dozens of near-
// identical ones it lurches between duplicates and misses what you aim for.
//
// So PEMF replaces the function at its one outside call (0x004B3631): every
// resolution once, at the desktop's refresh rate where offered, else 60 Hz,
// else the highest. The recursive call inside the original is not ours to
// touch -- our version never recurses.
constexpr uintptr_t kModeListCall   = 0x004B3631;   // E8 -> kModeListBuild
constexpr uintptr_t kModeListBuild  = 0x004B2DF0;
constexpr uintptr_t kGameAlloc      = 0x00533C9D;   // cdecl void* (size)
constexpr uintptr_t kRendererPtr    = 0x008EA08C;
constexpr uintptr_t kModeTable      = 0x008CABF0;   // ModeEntry*, the list
constexpr uintptr_t kModeCount      = 0x00726238;   // int, entries in it

struct ModeEntry { int mode, w, h; };
typedef void*(__cdecl* GameAllocFn)(size_t);

inline void*      g_modeListOrig  = nullptr;
inline ModeEntry* g_modeListTable = nullptr;   // the table WE allocated
inline int        g_modeListCap   = 0;         // ...and how many it holds

} // namespace widescreen

extern "C" int __cdecl PemfBuildModeList(widescreen::ModeEntry** table)
{
    using namespace widescreen;
    const auto orig = (int(__cdecl*)(ModeEntry**))g_modeListOrig;
    if (!g_set.enabled || !g_set.unlockModes) return orig(table);

    ModeEntry found[128];
    int n = 0;
    __try {
        const uint8_t* renderer = *(const uint8_t* const*)kRendererPtr;
        const uint8_t* info = renderer ? *(const uint8_t* const*)(renderer + 0x280) : nullptr;
        if (!info) return orig(table);
        const D3DDISPLAYMODE* const* modes = *(const D3DDISPLAYMODE* const* const*)(info + 0x454);
        const unsigned count = *(const unsigned*)(info + 0x45C);

        DEVMODEA dm{};
        dm.dmSize = sizeof(dm);
        const unsigned desktopHz =
            EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) ? dm.dmDisplayFrequency : 0;

        // Rank a refresh rate: the desktop's first, then 60, then the highest.
        auto rank = [desktopHz](unsigned hz) -> unsigned {
            if (desktopHz > 1 && hz == desktopHz) return 100000;
            if (hz == 60) return 50000;
            return hz;
        };
        unsigned bestHz[128] = {0};

        for (unsigned i = 0; i < count; ++i) {
            const D3DDISPLAYMODE* m = modes[i];
            if (!m || m->Format != D3DFMT_X8R8G8B8 || m->Width < 640 || m->Height < 480) continue;
            int k = 0;
            while (k < n && !(found[k].w == (int)m->Width && found[k].h == (int)m->Height)) ++k;
            if (k == n) {
                if (n == 128) continue;
                found[n] = { (int)i, (int)m->Width, (int)m->Height };
                bestHz[n] = m->RefreshRate;
                ++n;
            } else if (rank(m->RefreshRate) > rank(bestHz[k])) {
                found[k].mode = (int)i;
                bestHz[k] = m->RefreshRate;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return orig(table);
    }
    if (n == 0) return orig(table);

    // Smallest to largest. The adapter does not promise an order -- on the
    // development machine 1440x900 came last, after 1920x1080, and a slider is
    // read as a scale: an entry out of place reads as a broken end stop.
    for (int i = 1; i < n; ++i) {
        const ModeEntry e = found[i];
        int j = i - 1;
        while (j >= 0 && (found[j].w > e.w || (found[j].w == e.w && found[j].h > e.h))) {
            found[j + 1] = found[j];
            --j;
        }
        found[j + 1] = e;
    }

    // The game keeps the table and refills it later. Reuse ours while it is big
    // enough; never write past a table someone else sized.
    if (*table != g_modeListTable || n > g_modeListCap) {
        ModeEntry* t = (ModeEntry*)((GameAllocFn)kGameAlloc)((n + 1) * sizeof(ModeEntry));
        if (!t) return orig(table);
        g_modeListTable = t;
        g_modeListCap = n;
        *table = t;
    }
    for (int i = 0; i < n; ++i) (*table)[i] = found[i];
    (*table)[n] = { -1, -1, -1 };

    static LONG logged = 0;
    if (InterlockedIncrement(&logged) <= 2) {
        char line[512] = {0};
        size_t used = 0;
        for (int i = 0; i < n && used + 16 < sizeof(line); ++i)
            used += _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE, "%s%dx%d",
                                i ? " " : "", found[i].w, found[i].h);
        Log("widescreen: Options lists %d resolution(s), each once: %s", n, line);
    }
    return n;
}

namespace widescreen {

inline bool RedirectModeList()
{
    const uintptr_t s = kModeListCall;
    if (*(const uint8_t*)s != 0xE8 || s + 5 + *(const int32_t*)(s + 1) != kModeListBuild) {
        Log("widescreen: mode-list call at 0x%08X is not what we expect -- Options "
            "keeps its stock list", (unsigned)s);
        return false;
    }
    void* prev = nullptr;
    if (!render::RedirectCall(s, (void*)&PemfBuildModeList, &prev)) return false;
    g_modeListOrig = prev;
    Log("widescreen: Options resolution list now built by PEMF (one entry per size)");

    // The game builds the list once, at startup -- before the first safe
    // point, so before the redirect. Rebuild it now, exactly as its caller
    // FUN_004B3590 does: `count = BuildModeList(&table)`.
    if (*(void* const*)kModeTable)
        *(int*)kModeCount = PemfBuildModeList((ModeEntry**)kModeTable);
    return true;
}

inline void RestoreModeList()
{
    if (!g_modeListOrig) return;
    render::RedirectCall(kModeListCall, g_modeListOrig, nullptr);
    g_modeListOrig = nullptr;
}

// ------------------------------------------------------------------- cameras
inline bool RedirectFrustumCalls()
{
    // Check every site first. A build we do not recognise gets nothing at all,
    // rather than some cameras corrected and others not.
    for (int i = 0; i < kFrustumCallCount; ++i) {
        const uintptr_t s = kFrustumCalls[i];
        if (*(const uint8_t*)s != 0xE8 ||
            s + 5 + *(const int32_t*)(s + 1) != kSetViewFrustum) {
            Log("widescreen: call at 0x%08X is not `call 0x%08X` -- cameras left "
                "as shipped", (unsigned)s, (unsigned)kSetViewFrustum);
            return false;
        }
    }
    g_frustumOrig = (void*)kSetViewFrustum;
    int done = 0;
    for (; done < kFrustumCallCount; ++done) {
        void* prev = nullptr;
        if (!render::RedirectCall(kFrustumCalls[done], (void*)&PemfSetViewFrustumStub, &prev))
            break;
    }
    if (done < kFrustumCallCount) {
        Log("widescreen: redirect failed at 0x%08X -- undoing %d",
            (unsigned)kFrustumCalls[done], done);
        for (int i = 0; i < done; ++i)
            render::RedirectCall(kFrustumCalls[i], g_frustumOrig, nullptr);
        g_frustumOrig = nullptr;
        return false;
    }
    Log("widescreen: %d camera call sites now pass through PEMF", done);
    return true;
}

inline void RestoreFrustumCalls()
{
    if (!g_frustumOrig) return;
    for (int i = 0; i < kFrustumCallCount; ++i)
        render::RedirectCall(kFrustumCalls[i], g_frustumOrig, nullptr);
    g_frustumOrig = nullptr;
}

// The interface camera was built at startup, before the first safe point, so
// it never went through us. Correct it once in place, through the game's own
// function, exactly as a call site would -- and then update it, exactly as
// SetResolution does after its own set-frustum (0x004D3BBB: push 1; push 0;
// ecx = camera; call 0x00533D10). Without the update the culling planes kept
// the stock top/bottom of +/-0.28 at 16:9, and small pieces drawn near the top
// or bottom of the 480-tall interface -- the top corners of the attack prompt's
// frame -- were culled before they reached Direct3D, while larger pieces on the
// same row, whose bounds straddle the stale plane, still drew.
inline void CorrectExistingUiCamera()
{
    void* cam = *(void* const*)kUiCamera;
    if (!cam || !Readable((const uint8_t*)cam + kCameraFrustum, sizeof(NiFrustum))) return;
    NiFrustum f;
    memcpy(&f, (const uint8_t*)cam + kCameraFrustum, sizeof(f));
    PemfSetViewFrustum(cam, &f, 0);

    if (!UpdateCamera(cam)) {
        Log("widescreen: camera update at 0x%08X not as expected -- interface culling "
            "keeps the stock bounds until the next resolution change", (unsigned)kObjectUpdate);
        return;
    }
    Log("widescreen: interface camera updated -- culling planes rebuilt");
}

// ---------------------------------------------------------------------- api
inline void LoadSettings(const char* gameDir)
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\PEMF\\widescreen.ini", gameDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) {
        Log("widescreen: no %s -- defaults (on)", path);
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64] = {0};
        int value = 0;
        if (sscanf_s(line, " %63[A-Za-z_] = %d", key, (unsigned)sizeof(key), &value) != 2)
            continue;
        if      (_stricmp(key, "enabled")     == 0) g_set.enabled     = value != 0;
        else if (_stricmp(key, "unlockModes") == 0) g_set.unlockModes = value != 0;
        else if (_stricmp(key, "widenWorld")  == 0) g_set.widenWorld  = value != 0;
        else if (_stricmp(key, "centerUi")    == 0) g_set.centerUi    = value != 0;
        else if (_stricmp(key, "autoShots")   == 0) g_set.autoShots   = value < 0 ? 0 : value;
        else if (_stricmp(key, "hudCorners")  == 0) g_set.hudCorners  = value != 0;
    }
    fclose(f);
    _snprintf_s(g_shotDir, sizeof(g_shotDir), _TRUNCATE, "%s\\PEMF\\shots", gameDir);
    Log("widescreen: enabled=%d unlockModes=%d widenWorld=%d centerUi=%d hudCorners=%d "
        "autoShots=%d (Ctrl+Shift+X saves a screenshot to %s)", g_set.enabled,
        g_set.unlockModes, g_set.widenWorld, g_set.centerUi, g_set.hudCorners,
        g_set.autoShots, g_shotDir);
}

// ⛔ First safe point only (a code write).
inline bool Install()
{
    if (g_installed || !g_set.enabled) return g_installed;
    __try {
        if (g_set.unlockModes) RedirectModeList();
        if (RedirectFrustumCalls()) {
            CorrectExistingUiCamera();
            if (g_set.centerUi) SubclassWindow();
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("widescreen: faulted while installing -- display left as shipped");
        return false;
    }
    int w = 0, h = 0;
    DeviceSize(&w, &h);
    Log("widescreen: installed at %dx%d (%s)", w, h,
        IsWide(w, h) ? "wide -- corrections active" : "4:3 or narrower -- nothing to correct");
    g_installed = true;
    return true;
}

// ------------------------------------------------------------------ backdrops
// Full-screen 2D layers, drawn by the UI camera, which is height-fitted and so
// covers only the centred 4:3 area. Three kinds, three treatments -- all of
// them preserve proportions:
//
//   PAINTING   textured 640x480 quad (menus and other painted screens).
//              "Cover": scaled up evenly until it fills the width, a little
//              trimmed off its top and bottom. Nothing is stretched.
//   STRIP      untextured quad spanning the full 640/642 width: fades, the
//              dialogue shading, the black cinematic bars in cutscenes. Solid
//              colour, so it is simply drawn full width.
//   MAP        the sailing map's parchment backing (z -200..240). The chart and
//              its markers are separate draws that a zoom would misalign, so
//              this one stays 4:3 with black side strips (cleared at EndScene).
//
// Each changes the projection for that ONE draw and restores it straight after;
// nothing else the game draws is touched. Shapes are from the reference
// project's notes (cinematic bars: 642 x 40, black, at z = 221 / -222).
inline volatile LONG g_mapPass       = 0;   // this pass drew the map backing
inline LONG          g_quadsLogged   = 0;
inline D3DMATRIX     g_savedProj     = {};
inline unsigned      g_lastQuadFormat = 0;  // for the log: what the quad was made of

enum class Quad { None, Painting, Strip, Map };

inline Quad ClassifyQuad(IDirect3DDevice9* d, INT base, UINT minIndex)
{
    DWORD fvf = 0;
    if (FAILED(d->GetFVF(&fvf)) || fvf != (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1))
        return Quad::None;
    IDirect3DBaseTexture9* tex = nullptr;
    const bool textured = SUCCEEDED(d->GetTexture(0, &tex)) && tex;
    // A render-target texture is a COPY OF THE FRAME (glow, fades, the scene-
    // copy layers in taverns and the governor's room). It already holds the
    // whole wide image, so it must go back exactly full-screen. Enlarging it
    // like a painting misaligns it with the frame: the ghosting seen in test 3.
    //
    // Test round 6 found a second kind: the world behind a dialog card came out
    // enlarged, because THAT copy lives in an ordinary texture, not a render
    // target. The sturdier line is the format: painted screens ship as DDS art
    // and are DXT-compressed; a copy of the frame never is. So only DXT art is
    // a painting; anything else full-screen is laid back exactly.
    bool frameCopy = false;
    if (tex) {
        frameCopy = true;                       // until it proves to be art
        if (tex->GetType() == D3DRTYPE_TEXTURE) {
            D3DSURFACE_DESC td{};
            if (SUCCEEDED(((IDirect3DTexture9*)tex)->GetLevelDesc(0, &td))) {
                const bool dxt = td.Format == D3DFMT_DXT1 || td.Format == D3DFMT_DXT2 ||
                                 td.Format == D3DFMT_DXT3 || td.Format == D3DFMT_DXT4 ||
                                 td.Format == D3DFMT_DXT5;
                frameCopy = !dxt || (td.Usage & D3DUSAGE_RENDERTARGET) != 0;
                g_lastQuadFormat = (unsigned)td.Format;
            }
        }
        tex->Release();
    }

    IDirect3DVertexBuffer9* vb = nullptr;
    UINT offset = 0, stride = 0;
    if (FAILED(d->GetStreamSource(0, &vb, &offset, &stride)) || !vb) return Quad::None;

    Quad q = Quad::None;
    const int first = base + (int)minIndex;
    void* p = nullptr;
    if (first >= 0 && stride >= 12 &&
        SUCCEEDED(vb->Lock(offset + first * stride, 4 * stride, &p, D3DLOCK_READONLY))) {
        bool full = true, map = true, strip = true;
        unsigned corners = 0;
        const float y0 = ((const float*)p)[1];
        for (unsigned i = 0; i < 4; ++i) {
            const float* v = (const float*)((const uint8_t*)p + i * stride);
            const bool flat = fabsf(v[1]) <= 2.f && Close(v[1], y0);
            const float ax = fabsf(v[0]);
            if (!Close(ax, 320.f) || !flat || !Close(fabsf(v[2]), 240.f)) full = false;
            if (!Close(ax, 320.f) || !flat ||
                (!Close(v[2], -200.f) && !Close(v[2], 240.f))) map = false;
            if ((!Close(ax, 320.f) && !Close(ax, 321.f)) || !flat) strip = false;
            corners |= 1u << ((v[0] > 0.f ? 1 : 0) + (v[2] > 0.f ? 2 : 0));
        }
        vb->Unlock();
        if (corners == 15) {
            if (!textured || frameCopy) q = strip ? Quad::Strip : Quad::None;
            else if (full)  q = Quad::Painting;
            else if (map)   q = Quad::Map;
        }
    }
    vb->Release();
    return q;
}

// The UI camera's projection: _33 = far/(far-near) = 2.5, perspective.
inline bool IsUiProjection(const D3DMATRIX& m)
{
    return Close(m._33, 2.5f) && Close(m._34, 1.f) && Close(m._44, 0.f) && m._11 > 0.f;
}

} // namespace widescreen

extern "C" {

// Called by the device hook. Kept out of the hook so d3d9hook.h stays about
// the device, and everything widescreen-specific stays here.
void __cdecl PemfWidescreenPassBegin(void)
{
    widescreen::g_mapPass = 0;
}

// Before a four-vertex draw. Returns 1 if it changed the projection, in which
// case PemfWidescreenAfterDraw must run after the draw.
int __cdecl PemfWidescreenOnDrawIndexed(void* device, INT base, UINT minIndex)
{
    using namespace widescreen;
    if (!g_installed || !g_set.enabled || !g_set.centerUi) return 0;
    int w = 0, h = 0;
    DeviceSize(&w, &h);
    if (!IsWide(w, h)) return 0;
    IDirect3DDevice9* d = (IDirect3DDevice9*)device;

    __try {
        D3DMATRIX proj{};
        if (FAILED(d->GetTransform(D3DTS_PROJECTION, &proj)) || !IsUiProjection(proj)) return 0;
        const Quad q = ClassifyQuad(d, base, minIndex);
        if (q == Quad::None) return 0;

        if (InterlockedIncrement(&g_quadsLogged) <= 12)
            Log("widescreen: %s at %dx%d (proj _11=%.4f _22=%.4f, texture format %u)",
                q == Quad::Painting ? "painting -> cover" :
                q == Quad::Strip ? "strip / frame copy -> exact full width" : "map backing -> 4:3 + parchment",
                w, h, proj._11, proj._22, g_lastQuadFormat);

        if (q == Quad::Map) { g_mapPass = 1; return 0; }

        // _11 = 2 puts x = 320 at the screen edge, as the stock 4:3 camera does.
        D3DMATRIX m = proj;
        const float grow = 2.f / proj._11;
        m._11 = 2.f;
        if (q == Quad::Painting) m._22 = proj._22 * grow;   // same scale both ways
        g_savedProj = proj;
        return SUCCEEDED(d->SetTransform(D3DTS_PROJECTION, &m)) ? 1 : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

void __cdecl PemfWidescreenAfterDraw(void* device)
{
    __try {
        ((IDirect3DDevice9*)device)->SetTransform(D3DTS_PROJECTION, &widescreen::g_savedProj);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// The sides of the map screen. The chart, markers and toolbar are separate draws
// that a zoom would push off screen, so the map itself stays 4:3 -- and the
// strips beside it get PARCHMENT rather than black, so it reads as a wider
// sheet. The paper is generated once (managed pool: survives a device reset)
// and drawn as two pre-transformed quads; the device state is captured and put
// back around it, so nothing after this draw sees a difference.
} // extern "C"

namespace widescreen {

inline IDirect3DTexture9*   g_paper      = nullptr;
inline IDirect3DStateBlock9* g_paperState = nullptr;
inline bool                  g_paperFailed = false;

inline IDirect3DTexture9* Paper(IDirect3DDevice9* d)
{
    if (g_paper || g_paperFailed) return g_paper;
    IDirect3DTexture9* t = nullptr;
    if (FAILED(d->CreateTexture(256, 256, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) {
        g_paperFailed = true;
        return nullptr;
    }
    D3DLOCKED_RECT lr{};
    if (FAILED(t->LockRect(0, &lr, nullptr, 0))) { t->Release(); g_paperFailed = true; return nullptr; }
    unsigned seed = 0x50454D46;   // "PEMF"
    for (int y = 0; y < 256; ++y) {
        DWORD* row = (DWORD*)((uint8_t*)lr.pBits + y * lr.Pitch);
        // Faint horizontal fibres plus grain, on the map's own tan.
        const int fibre = ((y * 37) % 23 == 0) ? -6 : 0;
        for (int x = 0; x < 256; ++x) {
            seed = 1664525u * seed + 1013904223u;
            const int grain = (int)(seed >> 28) - 8 + fibre;
            const int r = 226 + grain, g = 200 + grain, b = 150 + grain;
            row[x] = D3DCOLOR_ARGB(255, r < 0 ? 0 : (r > 255 ? 255 : r),
                                   g < 0 ? 0 : (g > 255 ? 255 : g), b < 0 ? 0 : (b > 255 ? 255 : b));
        }
    }
    t->UnlockRect(0);
    g_paper = t;
    return g_paper;
}

struct PaperVertex { float x, y, z, rhw, u, v; };

inline bool DrawPaperStrips(IDirect3DDevice9* d, int w, int h, int bar)
{
    IDirect3DTexture9* paper = Paper(d);
    if (!paper) return false;
    if (!g_paperState && FAILED(d->CreateStateBlock(D3DSBT_ALL, &g_paperState))) return false;
    if (FAILED(g_paperState->Capture())) return false;

    d->SetVertexShader(nullptr);
    d->SetPixelShader(nullptr);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    d->SetTexture(0, paper);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    const D3DVIEWPORT9 full{ 0, 0, (DWORD)w, (DWORD)h, 0.f, 1.f };
    d->SetViewport(&full);

    // Texel-to-pixel: D3D9 pixel centres sit at -0.5. Texture repeats every
    // 256 screen pixels, so the grain is the same size at any resolution.
    const float x0[2] = { 0.f, (float)(w - bar) }, x1[2] = { (float)bar, (float)w };
    HRESULT hr = S_OK;
    for (int i = 0; i < 2 && SUCCEEDED(hr); ++i) {
        const float l = x0[i] - 0.5f, r = x1[i] - 0.5f, t = -0.5f, b = h - 0.5f;
        const PaperVertex v[4] = {
            { l, t, 0.f, 1.f, x0[i] / 256.f, 0.f },
            { r, t, 0.f, 1.f, x1[i] / 256.f, 0.f },
            { l, b, 0.f, 1.f, x0[i] / 256.f, h / 256.f },
            { r, b, 0.f, 1.f, x1[i] / 256.f, h / 256.f },
        };
        hr = d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(PaperVertex));
    }
    g_paperState->Apply();
    return SUCCEEDED(hr);
}

// The device is being reset: the state block holds default-pool references.
inline void ReleaseDeviceObjects()
{
    if (g_paperState) { g_paperState->Release(); g_paperState = nullptr; }
}

} // namespace widescreen

extern "C" {

void __cdecl PemfWidescreenPassEnd(void* device)
{
    using namespace widescreen;
    if (!g_mapPass) return;
    IDirect3DDevice9* d = (IDirect3DDevice9*)device;

    __try {
        // Only the real backbuffer. Scene transitions render to square textures.
        IDirect3DSurface9* rt = nullptr;
        if (FAILED(d->GetRenderTarget(0, &rt)) || !rt) return;
        D3DSURFACE_DESC desc{};
        const HRESULT hr = rt->GetDesc(&desc);
        rt->Release();
        int w = 0, h = 0;
        DeviceSize(&w, &h);
        if (FAILED(hr) || (int)desc.Width != w || (int)desc.Height != h) return;

        const int safeW = (int)(h * 4.0 / 3.0 + 0.5);
        const int bar = (w - safeW) / 2;
        if (bar <= 0) return;

        if (DrawPaperStrips(d, w, h, bar)) return;

        // No paper (texture creation failed): plain black, as before.
        DWORD scissor = 0;
        d->GetRenderState(D3DRS_SCISSORTESTENABLE, &scissor);
        D3DVIEWPORT9 vp{};
        d->GetViewport(&vp);
        const D3DVIEWPORT9 full{ 0, 0, (DWORD)w, (DWORD)h, vp.MinZ, vp.MaxZ };
        if (scissor) d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        d->SetViewport(&full);
        const D3DRECT bars[2] = { { 0, 0, bar, h }, { w - bar, 0, w, h } };
        d->Clear(2, bars, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
        d->SetViewport(&vp);
        if (scissor) d->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// From the device hook, just before Reset.
void __cdecl PemfWidescreenBeforeReset(void)
{
    widescreen::ReleaseDeviceObjects();
}

} // extern "C"

namespace widescreen {

// ----------------------------------------------------------------- draw trace
// ⚠️ DIAGNOSTIC. Ctrl+Shift+D logs every indexed draw of the NEXT frame -- the
// geometry's bounds, its texture, both projections, scissor, viewport and
// target -- so a broken piece of interface can be identified from one keypress
// instead of guessed at from a screenshot. Bounded: one frame, 400 lines.
inline volatile LONG g_traceArmed = 0;   // set by the key, frame starts at Present
inline volatile LONG g_tracing    = 0;   // the frame being traced
inline LONG          g_traceLines = 0;
inline LONG          g_traceFrame = 0;
inline LONG          g_traceSkipped = 0;   // world draws, counted not printed

// Interface or screen space? Only those are worth a line; the world is counted.
inline bool TraceWorthIt(IDirect3DDevice9* d, DWORD fvf)
{
    if ((fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW) return true;
    D3DMATRIX proj{};
    return SUCCEEDED(d->GetTransform(D3DTS_PROJECTION, &proj)) && IsUiProjection(proj);
}

// Common tail of a trace line: texture, transforms, scissor, viewport.
inline void TraceState(IDirect3DDevice9* d, char* out, size_t cap)
{
    char texs[96] = "untextured";
    IDirect3DBaseTexture9* t = nullptr;
    if (SUCCEEDED(d->GetTexture(0, &t)) && t) {
        if (t->GetType() == D3DRTYPE_TEXTURE) {
            D3DSURFACE_DESC td{};
            ((IDirect3DTexture9*)t)->GetLevelDesc(0, &td);
            _snprintf_s(texs, sizeof(texs), _TRUNCATE, "tex %ux%u fmt %u%s", td.Width, td.Height,
                        (unsigned)td.Format, (td.Usage & D3DUSAGE_RENDERTARGET) ? " RT" : "");
        } else _snprintf_s(texs, sizeof(texs), _TRUNCATE, "tex type %d", (int)t->GetType());
        t->Release();
    }
    D3DMATRIX proj{}, view{}, world{};
    d->GetTransform(D3DTS_PROJECTION, &proj);
    d->GetTransform(D3DTS_VIEW, &view);
    d->GetTransform(D3DTS_WORLD, &world);
    DWORD sc = 0, ztest = 0, atest = 0;
    d->GetRenderState(D3DRS_SCISSORTESTENABLE, &sc);
    d->GetRenderState(D3DRS_ZENABLE, &ztest);
    d->GetRenderState(D3DRS_ALPHATESTENABLE, &atest);
    RECT r{};
    d->GetScissorRect(&r);
    D3DVIEWPORT9 vp{};
    d->GetViewport(&vp);
    _snprintf_s(out, cap, _TRUNCATE, "%s | proj %.3f %.3f %.3f | view43 %.0f | world s%.3f "
                "t(%.1f,%.1f,%.1f) | scissor %s %ld,%ld-%ld,%ld | vp %lu,%lu %lux%lu | z%lu a%lu",
                texs, proj._11, proj._22, proj._33, view._43, world._11, world._41, world._42,
                world._43, sc ? "ON" : "off", r.left, r.top, r.right, r.bottom, vp.X, vp.Y,
                vp.Width, vp.Height, ztest, atest);
}

// Bounds of `n` vertices of `stride` bytes at `p` (x, y, z or screen x, y, z).
inline void TraceBounds(const void* p, UINT n, UINT stride, char* out, size_t cap)
{
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (UINT i = 0; i < n; ++i) {
        const float* v = (const float*)((const uint8_t*)p + i * stride);
        for (int k = 0; k < 3; ++k) { if (v[k] < mn[k]) mn[k] = v[k]; if (v[k] > mx[k]) mx[k] = v[k]; }
    }
    _snprintf_s(out, cap, _TRUNCATE, "x %.1f..%.1f y %.2f..%.2f z %.1f..%.1f",
                mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
}

// The UP draws hand us the vertices directly.
inline void TraceDrawUP(IDirect3DDevice9* d, const char* kind, UINT type, UINT numVerts,
                        UINT primCount, const void* data, UINT stride)
{
    DWORD fvf = 0;
    d->GetFVF(&fvf);
    if (!TraceWorthIt(d, fvf)) { InterlockedIncrement(&g_traceSkipped); return; }
    if (InterlockedIncrement(&g_traceLines) > 400) return;
    char geo[128] = "geo ?", st[512];
    if (data && stride >= 12 && numVerts > 0 && numVerts < 4096) TraceBounds(data, numVerts, stride, geo, sizeof(geo));
    TraceState(d, st, sizeof(st));
    Log("trace %ld: %s t%u v%u p%u fvf %X | %s | %s", g_traceFrame, kind, type, numVerts,
        primCount, (unsigned)fvf, geo, st);
}

inline void TraceDraw(IDirect3DDevice9* d, UINT type, INT base, UINT minIndex,
                      UINT numVerts, UINT startIndex, UINT primCount)
{
    DWORD fvf = 0;
    d->GetFVF(&fvf);
    if (!TraceWorthIt(d, fvf)) { InterlockedIncrement(&g_traceSkipped); return; }
    if (InterlockedIncrement(&g_traceLines) > 400) return;
    char geo[128] = "geo ?";
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT off = 0, stride = 0;
    if (SUCCEEDED(d->GetStreamSource(0, &vb, &off, &stride)) && vb) {
        void* p = nullptr;
        const int first = base + (int)minIndex;
        if ((fvf & D3DFVF_POSITION_MASK) && stride >= 12 && first >= 0 && numVerts > 0 && numVerts < 4096 &&
            SUCCEEDED(vb->Lock(off + first * stride, numVerts * stride, &p, D3DLOCK_READONLY))) {
            float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
            for (UINT i = 0; i < numVerts; ++i) {
                const float* v = (const float*)((const uint8_t*)p + i * stride);
                for (int k = 0; k < 3; ++k) { if (v[k] < mn[k]) mn[k] = v[k]; if (v[k] > mx[k]) mx[k] = v[k]; }
            }
            vb->Unlock();
            _snprintf_s(geo, sizeof(geo), _TRUNCATE, "x %.1f..%.1f y %.2f..%.2f z %.1f..%.1f",
                        mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
        }
        vb->Release();
    }
    char st[512];
    TraceState(d, st, sizeof(st));
    Log("trace %ld: DIP t%u v%u p%u fvf %X | %s | %s", g_traceFrame, type, numVerts, primCount,
        (unsigned)fvf, geo, st);
}

// ---------------------------------------------------------------- screenshots
// The game minimises on every outside capture tool, and the Steam overlay key
// is not reliable here, so PEMF saves the finished frame itself: Ctrl+Shift+X
// on demand, or every `autoShots` seconds. Half resolution, 24-bit BMP, a ring
// of 40 files in PEMF\shots -- enough to see a layout, small enough to keep.
constexpr int kShotRing = 40;
inline LONG  g_shotSeq     = 0;
inline DWORD g_shotLast    = 0;
inline bool  g_shotKeyDown = false;

inline LONG g_manualSeq = 0;

inline bool SaveShot(IDirect3DDevice9* d, const char* why, bool manual)
{
    IDirect3DSurface9* bb = nullptr;
    IDirect3DSurface9* sys = nullptr;
    bool ok = false;
    if (FAILED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    D3DSURFACE_DESC desc{};
    if (SUCCEEDED(bb->GetDesc(&desc)) && desc.MultiSampleType == D3DMULTISAMPLE_NONE &&
        (desc.Format == D3DFMT_X8R8G8B8 || desc.Format == D3DFMT_A8R8G8B8) &&
        SUCCEEDED(d->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                                 D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(d->GetRenderTargetData(bb, sys))) {
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            CreateDirectoryA(g_shotDir, nullptr);
            const LONG seq = InterlockedIncrement(&g_shotSeq);
            SYSTEMTIME t;
            GetLocalTime(&t);
            char path[MAX_PATH];
            // Pressed by hand: its own file, kept. Automatic: a ring of kShotRing.
            if (manual)
                _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\key_%02d%02d%02d_%03ld.bmp",
                            g_shotDir, t.wHour, t.wMinute, t.wSecond,
                            (long)InterlockedIncrement(&g_manualSeq));
            else
                _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\shot_%02ld.bmp", g_shotDir,
                            (long)((seq - 1) % kShotRing));
            const int w = (int)desc.Width / 2, h = (int)desc.Height / 2;
            const int row = (w * 3 + 3) & ~3;
            BITMAPFILEHEADER fh{};
            BITMAPINFOHEADER ih{};
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + row * h;
            ih.biSize = sizeof(ih);
            ih.biWidth = w;
            ih.biHeight = h;   // bottom-up
            ih.biPlanes = 1;
            ih.biBitCount = 24;
            FILE* f = nullptr;
            if (fopen_s(&f, path, "wb") == 0 && f) {
                fwrite(&fh, sizeof(fh), 1, f);
                fwrite(&ih, sizeof(ih), 1, f);
                uint8_t line[8192 * 3] = {0};
                for (int y = h - 1; y >= 0; --y) {
                    const uint8_t* src = (const uint8_t*)lr.pBits + (size_t)(y * 2) * lr.Pitch;
                    for (int x = 0; x < w && x * 3 + 2 < (int)sizeof(line); ++x) {
                        const uint8_t* px = src + x * 2 * 4;   // B G R X
                        line[x * 3 + 0] = px[0];
                        line[x * 3 + 1] = px[1];
                        line[x * 3 + 2] = px[2];
                    }
                    fwrite(line, row, 1, f);
                }
                fclose(f);
                ok = true;
                int dw = 0, dh = 0;
                DeviceSize(&dw, &dh);
                Log("widescreen: screenshot %s (%s) at %02d:%02d:%02d, %dx%d", path, why,
                    t.wHour, t.wMinute, t.wSecond, dw, dh);
            }
            sys->UnlockRect();
        }
    }
    if (sys) sys->Release();
    bb->Release();
    return ok;
}

} // namespace widescreen

extern "C" int __cdecl PemfWidescreenTracing(void) { return widescreen::g_tracing ? 1 : 0; }

extern "C" void __cdecl PemfWidescreenTraceDraw(void* device, UINT type, INT base, UINT minIndex,
                                                UINT numVerts, UINT startIndex, UINT primCount)
{
    __try { widescreen::TraceDraw((IDirect3DDevice9*)device, type, base, minIndex, numVerts,
                                  startIndex, primCount); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

extern "C" void __cdecl PemfWidescreenTraceDrawUP(void* device, const char* kind, UINT type,
                                                  UINT numVerts, UINT primCount,
                                                  const void* data, UINT stride)
{
    __try { widescreen::TraceDrawUP((IDirect3DDevice9*)device, kind, type, numVerts, primCount,
                                    data, stride); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

extern "C" void __cdecl PemfWidescreenOnPresent(void* device)
{
    using namespace widescreen;
    if (!g_shotDir[0]) return;
    const bool key = (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                     (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
                     (GetAsyncKeyState('X') & 0x8000);
    const bool pressed = key && !g_shotKeyDown;
    g_shotKeyDown = key;
    // The trace runs from one Present to the next: a whole frame.
    if (g_tracing) {
        InterlockedExchange(&g_tracing, 0);
        Log("trace %ld: frame ends (%ld interface draws logged, %ld world draws counted)",
            g_traceFrame, g_traceLines, g_traceSkipped);
    }
    if (g_traceArmed) {
        InterlockedExchange(&g_traceArmed, 0);
        InterlockedIncrement(&g_traceFrame);
        InterlockedExchange(&g_traceLines, 0);
        InterlockedExchange(&g_traceSkipped, 0);
        int dw = 0, dh = 0;
        DeviceSize(&dw, &dh);
        Log("trace %ld: frame begins at %dx%d", g_traceFrame, dw, dh);
        InterlockedExchange(&g_tracing, 1);
    }
    static bool traceKeyWas = false;
    const bool traceKey = (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                          (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState('D') & 0x8000);
    if (traceKey && !traceKeyWas) InterlockedExchange(&g_traceArmed, 1);
    traceKeyWas = traceKey;

    const DWORD now = GetTickCount();
    const bool timed = g_set.autoShots > 0 && now - g_shotLast >= (DWORD)g_set.autoShots * 1000;
    if (!pressed && !timed) return;
    g_shotLast = now;
    __try { SaveShot((IDirect3DDevice9*)device, pressed ? "Ctrl+Shift+X" : "auto", pressed); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

namespace widescreen {

inline void Restore()
{
    if (!g_installed) return;
    __try {
        UnsubclassWindow();
        RestoreFrustumCalls();
        RestoreModeList();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
    g_installed = false;
}

} // namespace widescreen
