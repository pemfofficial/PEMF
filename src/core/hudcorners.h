// hudcorners.h - on a wide screen, the HUD goes to the corners of the SCREEN.
//
// widescreen.h keeps the interface whole by height-fitting its camera: the
// 640x480 interface sits in the middle with empty margins either side. That is
// right for menus and paintings, but the HUD -- compass, fame and purse at the
// bottom left, the command keypad at the bottom right, the ship panels in a
// battle -- then sits at the edges of the 4:3 area instead of the screen.
//
// ============================================================================
// HOW THE HUD IS PLACED (decompiled, see docs/WIDESCREEN.md "The HUD")
// ============================================================================
// Every HUD element is positioned in the 640x480 interface space and drawn by
// one of five primitives, each taking its x as a stack argument:
//
//     FUN_004BBA60(node, x, y, scale, angle)       a HUD model (compass, icons)
//     FUN_004B06C0(text, x, y, ...)   eax = 0      text
//     FUN_004884A0(x, y)              eax/ecx/edx  a 9-slice panel
//     FUN_004AF080(tex, u, v, w, h, x, ...)        a textured quad
//     FUN_00412900(x, y, size)        esi = node   the keypad's small form
//
// and each converts with x - 640/2. The positions are fractions of the
// interface width (compass at W/13, keypad at W - 3*(W/16) - 6, battle panels
// at W/4 and 3W/4) or plain constants (the date at x = 8).
//
// So PEMF moves the HUD by its x ARGUMENTS, at the call sites, and nothing else:
// every redirected call goes through a stub that adds the group's offset to x
// and jumps to the game's own function with every register as it was. Sizes,
// textures, text and the game's code are untouched.
//
// Three ways a site is moved:
//   LEFT   by one margin to the left  (the bottom-left cluster, the date)
//   RIGHT  by one margin to the right (the keypad)
//   SIDE   left of centre goes left, right of centre goes right, the middle
//          stays (battle panels and buttons, drawn by shared code for both
//          ships)
// A group always moves as one piece, so nothing inside it spreads apart.
//
// ⛔ Things that did NOT work, and why:
//   - Changing ScreenW (0x0085A26C) for the HUD: the keypad derives its button
//     SIZE from it (W/16), so it grew.
//   - Changing the 640 at 0x0072637C, which every primitive centres with: the
//     text primitive also uses it as a SCALE (0x004B09EC, size * 640 / device
//     width), so text would have changed size.
//
// ---------------------------------------------------------------- hover/clicks
// Hover is tested against the UNMOVED positions: the HUD compares the cursor
// (0x008DC30C) with the x it computed. Those four reads are pointed at PEMF's
// copies of the cursor, already moved the opposite way, so hover lines up with
// what is drawn. Battle buttons and HUD quests act on the hovered id; the
// keypad also writes click boxes into the region table (0x0086C330), which the
// keypad wrapper moves after it returns.
//
// The cursor itself can only reach the margins if the game lets it: it converts
// the mouse with LOWORD(lParam) * 640 / device width (0x0042C23F on move,
// 0x0042C3C5 on button down), unsigned, so never below 0 or past 640. Both are
// replaced with a call that converts the REAL cursor position -- but only while
// a moved HUD has been drawn in the last quarter second. Menus keep exactly the
// mouse they have today.
//
// ⛔ A CODE WRITE -- ONLY FROM THE FIRST SAFE POINT. Every site is checked
// against the image first; if any differs, none is written.
#pragma once
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "log.h"
#include "render.h"       // RedirectCall
#include "widescreen.h"

namespace hudcorners {

// -------------------------------------------------------------------- targets
constexpr uintptr_t kHudModel   = 0x004BBA60;
constexpr uintptr_t kHudText    = 0x004B06C0;
constexpr uintptr_t kHudPanel   = 0x004884A0;
constexpr uintptr_t kHudQuad    = 0x004AF080;
constexpr uintptr_t kKeypadIcon = 0x00412900;
constexpr uintptr_t kKeypad     = 0x00473B70;

constexpr uintptr_t kCursorX    = 0x008DC30C;   // int, interface units
constexpr uintptr_t kRegionN    = 0x0086C330;   // int16 count
constexpr uintptr_t kRegionId   = 0x0086C1A0;   // int16[]
constexpr uintptr_t kRegionX0   = 0x0086C010;   // int16[]
constexpr uintptr_t kRegionX1   = 0x0086BCF0;   // int16[]
constexpr int       kRegionMax  = 200;          // arrays are 0x190 bytes apart

constexpr uintptr_t kUiWidth    = 0x0072637C;   // int, 640, never written
constexpr uintptr_t kDeviceW    = 0x008CAC18;   // int

enum Mode : uint8_t { Left, Right, Side };
enum Kind : uint8_t { Model, Text, Panel, Quad, Icon };

struct Site { uintptr_t at; Kind kind; Mode mode; };

// Every site was read from the disassembly of the GOG build; each must be an
// `E8 rel32` to its kind's function, or nothing is written.
constexpr Site kSites[] = {
    // ---- the keypad, FUN_00473B70 (sailing, battle, on land, duel, dance)
    { 0x00473BE2, Icon,  Right },
    { 0x00473CCF, Model, Right }, { 0x00473DD3, Model, Right },
    { 0x00473DFC, Model, Right }, { 0x00473E25, Model, Right },
    { 0x00473E4E, Model, Right }, { 0x00473E77, Model, Right },
    { 0x00473E9D, Model, Right }, { 0x00473EC3, Model, Right },
    { 0x00473EE9, Model, Right }, { 0x00473FF3, Model, Right },
    { 0x00473F97, Text,  Right }, { 0x00473FC0, Text,  Right },

    // ---- the sailing HUD, FUN_004741B0. The torn-paper notice strip
    // (0x004742E1, 0x0047432C) is centred and is left where it is.
    { 0x004743CE, Panel, Left },                               // quests panel
    { 0x004743F1, Text,  Left },                               // "Quests:"
    { 0x0047457F, Model, Left }, { 0x00474779, Model, Left },  // quest icons
    { 0x00474A0F, Model, Left }, { 0x00474A47, Model, Left },  // compass
    { 0x00474AB6, Model, Left }, { 0x00474B0B, Model, Left },
    { 0x00474B4D, Model, Left }, { 0x00474C61, Model, Left },
    { 0x00474B9C, Text,  Left }, { 0x00474BD9, Text,  Left },  // knots, gold
    { 0x00474C13, Text,  Left }, { 0x00474CA3, Text,  Left },  // months, crew
    { 0x00474D13, Text,  Left }, { 0x00474D5C, Text,  Left },  // fame
    { 0x00474E41, Text,  Left }, { 0x00474EA9, Text,  Left },  // tooltip
    { 0x00474ECA, Text,  Left },                               // date

    // ---- the battle HUD buttons and compass, FUN_00476190
    { 0x00476326, Model, Side }, { 0x00476373, Model, Side },
    { 0x004763DB, Model, Side }, { 0x00476490, Model, Side },
    { 0x004764C3, Model, Side }, { 0x00476516, Model, Side },
    { 0x00476558, Model, Side },
    { 0x004765A3, Text,  Side }, { 0x00476609, Text,  Side },

    // ---- the battle ship panels, inside FUN_0047A040 (one loop, both ships)
    { 0x0047B708, Text,  Side }, { 0x0047B749, Model, Side },
    { 0x0047B7D1, Text,  Side }, { 0x0047B86E, Panel, Side },
    { 0x0047B8DB, Model, Side }, { 0x0047B9C6, Panel, Side },
    { 0x0047BA0C, Text,  Side },
    // ...and the guns-loaded bar at the bottom left
    { 0x0047BAF1, Quad,  Left }, { 0x0047BB08, Model, Left },
    { 0x0047BBB0, Text,  Left },
};
constexpr int kSiteCount = (int)(sizeof(kSites) / sizeof(kSites[0]));

// Every call of the keypad.
constexpr uintptr_t kKeypadCalls[] = {
    0x00418013, 0x0041A128, 0x0041A144, 0x0041A160, 0x0041A17C, 0x0041A198,
    0x0041A1B4, 0x0041A1D0, 0x0043656E, 0x004365D2, 0x004365F3, 0x00474227,
    0x004761E7,
};
constexpr int kKeypadCallCount = (int)(sizeof(kKeypadCalls) / sizeof(kKeypadCalls[0]));

// The hover reads of the cursor X, and which copy each is pointed at.
struct CursorRead { uintptr_t at; int operand; Mode mode; };
constexpr CursorRead kCursorReads[] = {
    { 0x00474009, 2, Right },   // keypad buttons      mov ecx, [0x008DC30C]
    { 0x00474434, 1, Left  },   // quest icons         mov eax, [0x008DC30C]
    { 0x004745F1, 1, Left  },   // quest icons         mov eax, [0x008DC30C]
    { 0x0047626C, 2, Side  },   // battle buttons      mov edx, [0x008DC30C]
};
constexpr int kCursorReadCount = (int)(sizeof(kCursorReads) / sizeof(kCursorReads[0]));

// The mouse conversions: movzx eax,bx / imul eax,[0x0072637C] / cdq /
// idiv [0x008CAC18] -- 17 bytes, result in eax, lParam in ebx.
constexpr uintptr_t kCursorConvert[] = { 0x0042C23F, 0x0042C3C5 };
constexpr uint8_t kConvertBytes[17] = {
    0x0F, 0xB7, 0xC3, 0x0F, 0xAF, 0x05, 0x7C, 0x63, 0x72, 0x00,
    0x99, 0xF7, 0x3D, 0x18, 0xAC, 0x8C, 0x00,
};

// Elements within this distance of the centre count as centred for SIDE.
constexpr int kSideBand = 64;
// How long after the last moved HUD draw the cursor may still use the margins.
constexpr DWORD kHudLingerMs = 250;

// ---------------------------------------------------------------------- state
inline bool  g_installed   = false;
inline int   g_margin      = 0;      // interface units each side, this frame
inline int   g_lastMargin  = -1;     // for the log
inline DWORD g_lastHudTick = 0;
inline bool  g_convertWritten = false;

// The keypad's click boxes, and the x each had once we moved it (so one moved
// in an earlier frame is not moved again).
constexpr int kKeypadIds[] = { 200, 201, 202, 203, 204, 205, 206, 207, 208,
                               0xD2, 0xD3, 0xD4 };
inline int16_t g_movedX0[sizeof(kKeypadIds) / sizeof(kKeypadIds[0])] = {};
inline bool    g_movedValid[sizeof(kKeypadIds) / sizeof(kKeypadIds[0])] = {};

inline bool Active()
{
    return g_installed && widescreen::g_set.enabled && widescreen::g_set.centerUi &&
           widescreen::g_set.hudCorners;
}

// The interface is height-fitted: 480 units tall, 480 * W / H wide.
inline int Margin()
{
    if (!Active()) return 0;
    int w = 0, h = 0;
    widescreen::DeviceSize(&w, &h);
    if (!widescreen::IsWide(w, h)) return 0;
    const int visible = (int)((480LL * w) / h);
    return visible > 640 ? (visible - 640) / 2 : 0;
}

inline int Shift(Mode mode, int x, int m)
{
    switch (mode) {
    case Left:  return -m;
    case Right: return m;
    default:
        if (x < 320 - kSideBand) return -m;
        if (x > 320 + kSideBand) return m;
        return 0;
    }
}

} // namespace hudcorners

extern "C" {

// The cursor as each group sees it: the real position moved the opposite way
// to the group, so the game's hover test against its UNMOVED x lines up.
int g_pemfCursorLeft  = 0;
int g_pemfCursorRight = 0;
int g_pemfCursorSide  = 0;

inline void PemfHudCursorCopies(int m)
{
    const int p = *(const int*)hudcorners::kCursorX;
    g_pemfCursorLeft  = p + m;
    g_pemfCursorRight = p - m;
    // The inverse of Shift(Side): left of the band came from further right,
    // right of it from further left, inside it from where it is.
    const int lo = 320 - hudcorners::kSideBand - m;
    const int hi = 320 + hudcorners::kSideBand + m;
    g_pemfCursorSide = p < lo ? p + m : (p > hi ? p - m : p);
}

// Called by every stub with the site's return address and its x argument.
void __cdecl PemfHudAdjust(uintptr_t ret, int* x)
{
    using namespace hudcorners;
    const int m = g_margin;
    g_lastHudTick = GetTickCount();
    PemfHudCursorCopies(m);
    if (m <= 0) return;
    const uintptr_t site = ret - 5;
    for (int i = 0; i < kSiteCount; ++i) {
        if (kSites[i].at != site) continue;
        *x += Shift(kSites[i].mode, *x, m);
        return;
    }
}

// One stub per primitive. Each keeps EAX, ECX and EDX for the game's function
// (three of them take arguments in registers), moves the x argument where it
// sits on the stack, and jumps on with the caller's return address in place.
//
//   after the three pushes:  [esp+12] return address, [esp+16] first argument
#define PEMF_HUD_STUB(name, xOffset, target)                                   \
    __declspec(naked) void name(void)                                          \
    {                                                                          \
        __asm push eax                                                         \
        __asm push ecx                                                         \
        __asm push edx                                                         \
        __asm lea  eax, [esp + 16 + (xOffset)]                                 \
        __asm push eax                                                         \
        __asm push dword ptr [esp + 16]                                        \
        __asm call PemfHudAdjust                                               \
        __asm add  esp, 8                                                      \
        __asm pop  edx                                                         \
        __asm pop  ecx                                                         \
        __asm pop  eax                                                         \
        __asm jmp  dword ptr [target]                                          \
    }

uintptr_t g_pemfHudModel   = hudcorners::kHudModel;
uintptr_t g_pemfHudText    = hudcorners::kHudText;
uintptr_t g_pemfHudPanel   = hudcorners::kHudPanel;
uintptr_t g_pemfHudQuad    = hudcorners::kHudQuad;
uintptr_t g_pemfHudIcon    = hudcorners::kKeypadIcon;

PEMF_HUD_STUB(PemfHudModelStub, 4,    g_pemfHudModel)   // (node, x, ...)
PEMF_HUD_STUB(PemfHudTextStub,  4,    g_pemfHudText)    // (text, x, ...)
PEMF_HUD_STUB(PemfHudPanelStub, 0,    g_pemfHudPanel)   // (x, y)
PEMF_HUD_STUB(PemfHudQuadStub,  0x14, g_pemfHudQuad)    // (tex, u, v, w, h, x, ...)
PEMF_HUD_STUB(PemfHudIconStub,  0,    g_pemfHudIcon)    // (x, y, size)

#undef PEMF_HUD_STUB

// The keypad, drawn by the game, then its click boxes moved with it.
typedef void(__cdecl* KeypadFn)(unsigned, int, unsigned);

void __cdecl PemfKeypad(unsigned buttons, int compact, unsigned mode)
{
    using namespace hudcorners;
    g_margin = Margin();
    ((KeypadFn)kKeypad)(buttons, compact, mode);
    const int m = g_margin;
    if (m <= 0) return;
    __try {
        const int n = *(const int16_t*)kRegionN;
        for (int i = 0; i < n && i < kRegionMax; ++i) {
            const int16_t id = ((const int16_t*)kRegionId)[i];
            int k = -1;
            for (int j = 0; j < (int)(sizeof(kKeypadIds) / sizeof(kKeypadIds[0])); ++j)
                if (kKeypadIds[j] == id) { k = j; break; }
            if (k < 0) continue;
            int16_t& x0 = ((int16_t*)kRegionX0)[i];
            int16_t& x1 = ((int16_t*)kRegionX1)[i];
            // Already moved (the keypad keeps a box that exists rather than
            // writing it again): leave it.
            if (g_movedValid[k] && x0 == g_movedX0[k]) continue;
            x0 = (int16_t)(x0 + m);
            x1 = (int16_t)(x1 + m);
            g_movedX0[k] = x0;
            g_movedValid[k] = true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// The mouse: what `movzx eax,bx; imul eax,[640]; cdq; idiv [device width]`
// computed, unless a moved HUD is on screen -- then the real cursor, which may
// lie in a margin.
int __cdecl PemfCursorX(unsigned lParam)
{
    using namespace hudcorners;
    const int devW = *(const int*)kDeviceW;
    const int stock = devW ? (int)(lParam & 0xFFFF) * *(const int*)kUiWidth / devW : 0;
    const int m = g_margin;
    if (m <= 0 || GetTickCount() - g_lastHudTick > kHudLingerMs) return stock;

    // The client position before widescreen.h centred it, if this message is
    // the one it saw.
    const auto& raw = widescreen::g_rawMouse;
    if (!raw.valid || raw.mappedX != (int)(lParam & 0xFFFF) || raw.ch <= 0) return stock;
    const double safeW = raw.ch * 4.0 / 3.0;
    const double left  = (raw.cw - safeW) * 0.5;
    int v = (int)floor((raw.x - left) * 640.0 / safeW);
    if (v < -m) v = -m;
    if (v > 640 + m - 1) v = 640 + m - 1;
    return v;
}

// ebx = lParam in, eax = interface X out; ecx kept (the game's code after the
// conversion does not need it, but the original sequence never touched it).
__declspec(naked) void PemfCursorXStub(void)
{
    __asm {
        push ecx
        push ebx
        call PemfCursorX
        add  esp, 4
        pop  ecx
        ret
    }
}

} // extern "C"

namespace hudcorners {

inline void* StubFor(Kind k)
{
    switch (k) {
    case Model: return (void*)&PemfHudModelStub;
    case Text:  return (void*)&PemfHudTextStub;
    case Panel: return (void*)&PemfHudPanelStub;
    case Quad:  return (void*)&PemfHudQuadStub;
    default:    return (void*)&PemfHudIconStub;
    }
}

inline uintptr_t TargetFor(Kind k)
{
    switch (k) {
    case Model: return kHudModel;
    case Text:  return kHudText;
    case Panel: return kHudPanel;
    case Quad:  return kHudQuad;
    default:    return kKeypadIcon;
    }
}

inline bool CallsTo(uintptr_t site, uintptr_t target)
{
    return *(const uint8_t*)site == 0xE8 && site + 5 + *(const int32_t*)(site + 1) == target;
}

inline int* CopyFor(Mode m)
{
    return m == Left ? &g_pemfCursorLeft : (m == Right ? &g_pemfCursorRight : &g_pemfCursorSide);
}

inline bool WriteBytes(uintptr_t at, const void* src, size_t n)
{
    DWORD old = 0;
    if (!VirtualProtect((void*)at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)at, src, n);
    VirtualProtect((void*)at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, n);
    return true;
}

inline bool Verify()
{
    for (int i = 0; i < kSiteCount; ++i)
        if (!CallsTo(kSites[i].at, TargetFor(kSites[i].kind))) {
            Log("hudcorners: 0x%08X is not the call we expect -- HUD left as shipped",
                (unsigned)kSites[i].at);
            return false;
        }
    for (int i = 0; i < kKeypadCallCount; ++i)
        if (!CallsTo(kKeypadCalls[i], kKeypad)) {
            Log("hudcorners: keypad call 0x%08X differs -- HUD left as shipped",
                (unsigned)kKeypadCalls[i]);
            return false;
        }
    for (int i = 0; i < kCursorReadCount; ++i)
        if (*(const uint32_t*)(kCursorReads[i].at + kCursorReads[i].operand) != kCursorX) {
            Log("hudcorners: cursor read 0x%08X differs -- HUD left as shipped",
                (unsigned)kCursorReads[i].at);
            return false;
        }
    for (uintptr_t at : kCursorConvert)
        if (memcmp((const void*)at, kConvertBytes, sizeof(kConvertBytes)) != 0) {
            Log("hudcorners: mouse conversion 0x%08X differs -- HUD left as shipped",
                (unsigned)at);
            return false;
        }
    return true;
}

// call PemfCursorXStub; jmp over the rest of the original 17 bytes.
inline bool WriteConvert(uintptr_t at)
{
    uint8_t b[7];
    b[0] = 0xE8;
    *(int32_t*)(b + 1) = (int32_t)((uintptr_t)&PemfCursorXStub - (at + 5));
    b[5] = 0xEB;
    b[6] = (uint8_t)(sizeof(kConvertBytes) - 7);
    return WriteBytes(at, b, sizeof(b));
}

// Once a frame, from the safe point: this frame's margin, and the cursor copies
// for hover tests that run before any HUD draw has.
inline void Tick()
{
    if (!g_installed) return;
    g_margin = Margin();
    PemfHudCursorCopies(g_margin);
    if (g_margin != g_lastMargin) {
        g_lastMargin = g_margin;
        if (g_margin > 0)
            Log("hudcorners: HUD in the screen corners, %d interface units out each side",
                g_margin);
        else
            Log("hudcorners: no margins at this size -- HUD where the game puts it");
    }
}

// ⛔ First safe point only (a code write).
inline bool Install()
{
    if (g_installed) return true;
    if (!widescreen::g_set.enabled || !widescreen::g_set.centerUi ||
        !widescreen::g_set.hudCorners) {
        Log("hudcorners: off (widescreen.ini) -- HUD where the game puts it");
        return false;
    }
    __try {
        if (!Verify()) return false;
        for (int i = 0; i < kSiteCount; ++i)
            render::RedirectCall(kSites[i].at, StubFor(kSites[i].kind), nullptr);
        for (int i = 0; i < kKeypadCallCount; ++i)
            render::RedirectCall(kKeypadCalls[i], (void*)&PemfKeypad, nullptr);
        for (int i = 0; i < kCursorReadCount; ++i) {
            const uint32_t to = (uint32_t)(uintptr_t)CopyFor(kCursorReads[i].mode);
            WriteBytes(kCursorReads[i].at + kCursorReads[i].operand, &to, 4);
        }
        for (uintptr_t at : kCursorConvert) WriteConvert(at);
        g_convertWritten = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("hudcorners: faulted while installing");
        return false;
    }
    g_installed = true;
    Log("hudcorners: %d HUD draw(s), %d keypad call(s), %d hover read(s) and the "
        "mouse conversion redirected", kSiteCount, kKeypadCallCount, kCursorReadCount);
    Tick();
    return true;
}

inline void Restore()
{
    if (!g_installed) return;
    __try {
        for (int i = 0; i < kSiteCount; ++i)
            render::RedirectCall(kSites[i].at, (void*)TargetFor(kSites[i].kind), nullptr);
        for (int i = 0; i < kKeypadCallCount; ++i)
            render::RedirectCall(kKeypadCalls[i], (void*)kKeypad, nullptr);
        const uint32_t cursor = (uint32_t)kCursorX;
        for (int i = 0; i < kCursorReadCount; ++i)
            WriteBytes(kCursorReads[i].at + kCursorReads[i].operand, &cursor, 4);
        if (g_convertWritten)
            for (uintptr_t at : kCursorConvert) WriteBytes(at, kConvertBytes, sizeof(kConvertBytes));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
    g_installed = false;
}

} // namespace hudcorners
