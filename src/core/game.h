// game.h - address map and calling shims for Sid Meier's Pirates! (2004)
//
// Target: Pirates!.exe.gog-original, sha256 6E88B90E4E2E3024..., 3323288 bytes
// The exe has no ASLR (DllCharacteristics=0, ImageBase 0x400000), so these
// absolute VAs are valid every launch. Verify with VerifyTarget() anyway.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "log.h"

// MSVC inline asm cannot resolve namespace-qualified constexpr names, so the
// handful of addresses referenced from __asm blocks are also plain macros.
// Keep these in sync with game::addr below.
#define PGA_ADDTEXT0    0x004F6090
#define PGA_ADDTEXT1    0x004F60B0
#define PGA_WRAPTEXT    0x004879F0
#define PGA_SHOWMESSAGE 0x00410C50
#define PGA_MSGBUF      0x008E9F58
#define PGA_DRAWHUDTEXT 0x004B06C0

namespace game {

// ---------------------------------------------------------------- addresses
namespace addr {
    // Text composition. AddText is printf-style with named tokens:
    //   @NUM @HAPPY @NAME @CITYNAME @NATIONALITY @CITYTYPE @MONTH @DIFFICULTY @SHARE
    // Tokens consume varargs left-to-right exactly like printf conversions.
    constexpr uintptr_t AddText0      = 0x004F6090;  // cdecl, needs esi = MsgBuf
    constexpr uintptr_t AddText1      = 0x004F60B0;  // same, internal flag 1
    constexpr uintptr_t WrapText      = 0x004879F0;  // stdcall(width, mode)
    constexpr uintptr_t ShowMessage   = 0x00410C50;  // fastcall(ecx bg, edx flags, eax form)
    // The underlying dialog renderer. ShowMessage is a convenience wrapper
    // around it; the game's modal confirm prompts call it directly.
    constexpr uintptr_t ShowDialogDirect = 0x00430190;  // cdecl, 10 args, ret 1 = yes

    // The message-buffer object AddText writes through. Callers set esi to
    // this before calling; the wrappers do NOT do it themselves. The game
    // holds esi across the whole compose->wrap->show sequence.
    constexpr uintptr_t MsgBufObject  = 0x008E9F58;

    // The actual message text, a plain NUL-terminated char buffer. WrapText
    // reads it via `mov ebx, 0x869B48` before calling the string-assign at
    // 0x00412F10, and the game's own event code resets it afterwards with
    // `DAT_00869b48 = 0`.
    constexpr uintptr_t MessageText   = 0x00869B48;

    // NOT an output buffer: this is the shared empty-string sentinel that
    // string locals are initialised to. It always reads as length 0.
    constexpr uintptr_t EmptyStringSentinel = 0x008CACD0;

    // Crew / morale state.
    constexpr uintptr_t GetMoraleLevel   = 0x00404810;  // -> int 0..4
    constexpr uintptr_t CrewCount        = 0x00869AB0;  // int32
    constexpr uintptr_t UndividedPlunder = 0x00869AB4;  // int32
    constexpr uintptr_t MonthsAtSea      = 0x00869B1A;  // int16
    constexpr uintptr_t MoraleByte       = 0x00869B27;  // int8
    constexpr uintptr_t StateFlags       = 0x00869B34;  // int32, bit 0x80

    // Screen metrics used to position the card.
    constexpr uintptr_t ScreenW = 0x0085A26C;
    constexpr uintptr_t ScreenH = 0x0085A268;

    // ---------------------------------------------------------- world / map
    // Player ship position on the overworld, in milli-units. The game divides
    // these by 1000 before comparing against city coordinates.
    // Seen at 0x004691xx: FindNearestCity(DAT_00814304/1000, DAT_00814308/1000, ...)
    constexpr uintptr_t PlayerX = 0x00814304;
    constexpr uintptr_t PlayerY = 0x00814308;

    // The game's own nearest-city search, FUN_0045FD40.
    //   int FindNearestCity(int x, int y, uint typeMask, int maxDist, uint exclude)
    // Walks the city tables, uses an octagonal distance approximation, and
    // returns the nearest matching city index, or -1 if none is within maxDist.
    constexpr uintptr_t FindNearestCity = 0x0045FD40;

    // City tables it walks. Positions: stride 16 bytes, x at +0, y at +4.
    // Records: stride 0x20, flags at +0, type at +4.
    constexpr uintptr_t CityPositions = 0x0085B170;
    constexpr uintptr_t CityRecords   = 0x00860B70;

    // DEAD END -- do not re-investigate these as a screen enum.
    // They looked like a current-screen id and a screen-stack depth
    // (DAT_007263bc = (&DAT_007268f4)[DAT_00726a84 * 4] in the town code), but a
    // playtest showed the values are POINTER-LIKE, not an enum:
    //   0x1000EF5F (120 samples), 0x1000EF7A (4), 0x1000FF5F (1)
    // "Am I sailing" is determined from ship movement instead. See
    // triggers.h::Sailing(), which is playtest-validated.
    constexpr uintptr_t ScreenId      = 0x007263BC;
    constexpr uintptr_t ScreenDepth   = 0x00726A84;

    // ------------------------------------------------------- HUD / notices
    // The sailing render function: sea, ships, floating name labels and the
    // on-screen HUD text. Exactly one caller, at 0x004726CA. See render.h.
    constexpr uintptr_t SailingRender = 0x004612B0;

    // The game's in-world text renderer -- ship labels, "Wind: @NUM",
    // "Battle Sails", "Press 'r' to return to ship.". 307 call sites, and it
    // reads the SAME message buffer we compose into, so the whole @-token
    // pipeline works with it unchanged.
    //
    //   void DrawHudText(char* text, int x, int y, int size,
    //                    uint colour, uint flags, int p7, int p8)   // eax = 0
    //
    // It is a PER-FRAME draw with no timer of its own: the game re-issues it
    // every frame while the text should be visible.
    constexpr uintptr_t DrawHudText = 0x004B06C0;

    // Set to 0x4B immediately before a DrawHudText call at every site we have
    // examined -- a text style/size the renderer reads.
    constexpr uintptr_t HudTextStyle = 0x0085A11C;

    // The main loop, FUN_0042E1D0, and its PeekMessageA call. The RETURN
    // address of that call uniquely identifies the top of the frame, which is
    // the only point at which we present anything. See events.h.
    constexpr uintptr_t MainLoop          = 0x0042E1D0;
    constexpr uintptr_t MainLoopPeekCall  = 0x0042E206;
    constexpr uintptr_t MainLoopPeekRet   = MainLoopPeekCall + 6;  // 0x0042E20C

    // Absolute addresses of the IAT slots for the functions we hook. On the
    // GOG build these are found by name; on the DRM-packed Steam build the
    // import name tables are destroyed, but the slots themselves live here and
    // are populated at runtime -- so we hook them by address. Same build, no
    // ASLR, so these VAs are valid for both.
    constexpr uintptr_t SlotTimeGetTime  = 0x006C0430;  // WINMM!timeGetTime
    constexpr uintptr_t SlotPeekMessageA = 0x006C03A4;  // USER32!PeekMessageA
    constexpr uintptr_t SlotCreateFileA  = 0x006C0074;  // KERNEL32!CreateFileA
    constexpr uintptr_t SlotCreateFileW  = 0x006C0154;  // KERNEL32!CreateFileW

    // Resolution. SetResolution (FUN_004D3AB0, cdecl(w,h)) is the game's own
    // "switch to WxH": it resets the D3D device, resizes the window, and derives
    // the projection aspect from height/width -- so 16:9 is a true widescreen
    // view, not a 4:3 stretch. It copies ScreenW/H from UIWidth/UIHeight, which
    // are therefore set first. This bypasses the menu's 4:3-only resolution list.
    constexpr uintptr_t SetResolution = 0x004D3AB0;
    constexpr uintptr_t UIWidth       = 0x0072637C;  // becomes ScreenW
    constexpr uintptr_t UIHeight      = 0x00726380;  // becomes ScreenH
}

// ------------------------------------------------------------- state access
inline int32_t&  CrewCount()        { return *(int32_t*)addr::CrewCount; }
inline int32_t&  UndividedPlunder() { return *(int32_t*)addr::UndividedPlunder; }
inline int16_t&  MonthsAtSea()      { return *(int16_t*)addr::MonthsAtSea; }
inline int32_t&  StateFlags()       { return *(int32_t*)addr::StateFlags; }

typedef int (*GetMoraleLevel_t)();
inline int GetMoraleLevel() { return ((GetMoraleLevel_t)addr::GetMoraleLevel)(); }

// Force the game to a given resolution, bypassing the menu's 4:3-only list. Sets
// the UI dimensions (which become ScreenW/H) then invokes the game's own switch.
typedef void (__cdecl *SetResolution_t)(int w, int h);
inline void ForceResolution(int w, int h)
{
    *(int32_t*)addr::UIWidth  = w;
    *(int32_t*)addr::UIHeight = h;
    ((SetResolution_t)addr::SetResolution)(w, h);
}

// ---------------------------------------------------------------- the shims
//
// AddText is cdecl varargs, but requires esi = MsgBufObject on entry. Rather
// than forward a true varargs list (which needs a copy loop), we always pass
// five slots. Surplus args are harmless: the callee reads only what its format
// tokens require, and cdecl means we clean the stack ourselves.
//
// Stack on entry: [esp]=ret [esp+4]=fmt [esp+8]=a ... [esp+14]=d
// After `push esi` everything shifts by 4, and each subsequent push shifts the
// window again -- which is why the same `mov eax,[esp+0x18]` works five times.
// Eight argument slots, not four. Every @-token in the format consumes one, and
// once events are authored in JSON the token count is content-controlled -- a
// body with more tokens than we push would read stack garbage and could crash.
// Surplus slots are harmless (the callee reads only what its tokens require,
// and cdecl means we clean up), so we over-provide deliberately.
//
// kMaxTextArgs below must match the number of slots pushed here.
__declspec(naked) static void AddText(const char* fmt, int a = 0, int b = 0,
                                      int c = 0, int d = 0, int e = 0,
                                      int f = 0, int g = 0, int h = 0)
{
    __asm {
        push esi
        mov  esi, PGA_MSGBUF
        mov  eax, [esp + 0x28]      // h
        push eax
        mov  eax, [esp + 0x28]      // g
        push eax
        mov  eax, [esp + 0x28]      // f
        push eax
        mov  eax, [esp + 0x28]      // e
        push eax
        mov  eax, [esp + 0x28]      // d
        push eax
        mov  eax, [esp + 0x28]      // c
        push eax
        mov  eax, [esp + 0x28]      // b
        push eax
        mov  eax, [esp + 0x28]      // a
        push eax
        mov  eax, [esp + 0x28]      // fmt
        push eax
        mov  eax, PGA_ADDTEXT0
        call eax
        add  esp, 36                // 9 slots * 4, cdecl
        pop  esi
        ret
    }
}

constexpr int kMaxTextArgs = 8;

// Mode 1 is not a cosmetic variant: it appends a SELECTABLE MENU OPTION rather
// than narrative prose. 53 of the game's leading-space option strings
// (" Divide the Plunder", " Leave Town", " Accept her invitation.") go through
// it. Options are presented in call order and ShowMessage returns the index.
// By convention the game prefixes each option with a single space.
__declspec(naked) static void AddTextOption(const char* fmt, int a = 0, int b = 0,
                                            int c = 0, int d = 0)
{
    __asm {
        push esi
        mov  esi, PGA_MSGBUF
        mov  eax, [esp + 0x18]      // d
        push eax
        mov  eax, [esp + 0x18]      // c
        push eax
        mov  eax, [esp + 0x18]      // b
        push eax
        mov  eax, [esp + 0x18]      // a
        push eax
        mov  eax, [esp + 0x18]      // fmt
        push eax
        mov  eax, PGA_ADDTEXT1
        call eax
        add  esp, 20
        pop  esi
        ret
    }
}

// CDECL -- the caller cleans up. Confirmed by the plain `ret` (not `ret 8`) at
// 0x00487CD5. The game's own call site has no visible `add esp, 8` because
// MSVC defers that adjustment and folds it into subsequent [esp+N] offsets;
// reading that as stdcall and omitting the cleanup left esp 8 bytes low, so
// `pop esi` took an argument and `ret` jumped into it.
//
// esi is set for consistency with the game's sequence, though WrapText happens
// to save and overwrite it internally rather than reading it as an input.
__declspec(naked) static void WrapText(int width, int mode)
{
    __asm {
        push esi
        mov  esi, PGA_MSGBUF
        mov  eax, [esp + 0x0C]      // mode
        push eax
        mov  eax, [esp + 0x0C]      // width
        push eax
        mov  eax, PGA_WRAPTEXT
        call eax
        add  esp, 8                 // cdecl: we clean up
        pop  esi
        ret
    }
}

// Three-register call, and it RETURNS THE PLAYER'S CHOSEN OPTION INDEX in eax
// (the game does `test eax,eax / jne` on it at 0x00411C96).
//   ecx = index into the background-art table, consumed by FUN_00410BF0
//   edx = flags
//   eax = a real parameter, copied to esi at 0x410C5C and passed to the
//         renderer. The game passes -1 or 10; -1 is the common case.
// NOTE: this call is MODAL -- it does not return until the player dismisses
// or chooses. Callers must guard against reentrancy.
// `mode` is the eax parameter, and it appears to select the dialog FORM:
//   -1  message box  -- used by every narrative site (0x411C8C, 0x411F63, ...)
//   10  option menu  -- used by the interactive town menu (0x4119DA) and
//                       0x0041191C passes a computed value in ebp
// Passing -1 while appending options renders the prose but, we believe, never
// presents the options as selectable. Kept configurable until confirmed.
__declspec(naked) static int ShowMessage(int flags, int bgIndex = 0, int mode = -1)
{
    __asm {
        push esi
        push ebx
        mov  edx, [esp + 0x0C]      // flags   -> edx
        mov  ecx, [esp + 0x10]      // bgIndex -> ecx
        mov  eax, [esp + 0x14]      // mode    -> eax
        mov  esi, PGA_MSGBUF
        mov  ebx, PGA_SHOWMESSAGE
        call ebx                    // eax = result, preserved by the pops
        pop  ebx
        pop  esi
        ret
    }
}

// The FULL-SCREEN card renderer. Deliberately unused by events -- outcomes use
// ShowModalTextN so they stay in the choice card's style. Kept for the moments
// that should feel like a scene change: mutiny, marooning, the end of a career.
constexpr int kFormMessage = -1;    // plain acknowledge-and-dismiss card
// eax = 10 is the NON-BLOCKING town-menu form: it renders one frame and
// returns -2 ("nothing picked yet"), expecting an outer per-frame loop to keep
// calling it. Not usable as a one-shot modal.
constexpr int kFormPolled  = 10;

// Clear the pending message text. The game does exactly this (`DAT_00869b48 = 0`)
// after presenting a card, so composing without it appends to the last one.
inline void ResetMessage()
{
    *(char*)addr::MessageText = 0;
}

// ---------------------------------------------------------- modal yes/no
// The game's confirm prompts ("Do you wish to form a landing party and go
// ashore?") skip ShowMessage and call the renderer FUN_00430190 directly.
// Plain __cdecl, ten args, returns 1 when the player confirms.
// Reconstructed from 0x004692EF..0x0046932E.
typedef int (__cdecl *ShowDialog_t)(int x, int y, const char* sound,
                                    int a4, int a5, int a6, int a7,
                                    int a8, int a9, int a10);

inline int ScreenW() { return *(const int*)addr::ScreenW; }
inline int ScreenH() { return *(const int*)addr::ScreenH; }

// Present whatever text has been composed, and return the index of the option
// the player picked (0-based), or a negative value if there was nothing to pick.
inline int PresentDialog(const char* sound = "snap")
{
    auto fn = (ShowDialog_t)addr::ShowDialogDirect;
    int x = ScreenW() / 4;
    int y = ScreenH() / 8 + 1;
    return fn(x, y, sound, 0, -1, 0, 0, -1, 0, 0);
}

// ------------------------------------------------------------ choice events
//
// OPTIONS ARE LINES OF THE MESSAGE TEXT -- not separate calls. The game builds
// a choice prompt as one string: the body, then one line per option, each
// beginning with a single space, each terminated by '\n'. The renderer turns
// those lines into selectable rows and returns the index chosen.
//
// Verified in the landing-party prompt at 0x004692DA:
//     "Do you wish to form a landing party and go ashore?\n"
//     " No, sail away.\n"
//     " Yes, we'll anchor here.\n"
//   -> returns 1 when the player picks "Yes" (the second option).
//
// AddTextOption / mode 1 is a DIFFERENT system (the persistent town menu) and
// does not apply here.
//
// IMPORTANT: AddText REPLACES the message buffer, it does not append. The whole
// prompt -- body plus every option line -- must go in as ONE string, exactly as
// the game's own literal does. Calling AddText per option leaves only the last
// one, which renders as a bodyless card with a single Continue button.
//
// Body text may use @-tokens (consuming `a` then `b`); option strings are
// concatenated literally. A leading space is added to any option missing one.
// Argument-array form, for content-driven events whose token count is not known
// at compile time. `args` may be null; argCount is clamped to kMaxTextArgs.
inline int AskChoiceN(const char* body, const char* const* options, int count,
                      const int* args, int argCount, int width = 0x2C);

inline int AskChoice(const char* body, const char* const* options, int count,
                     int a = 0, int b = 0, int width = 0x2C)
{
    char prompt[2048];
    int n = _snprintf_s(prompt, sizeof(prompt), _TRUNCATE, "%s", body);
    if (n < 0) n = (int)strlen(prompt);
    // Ensure the body ends with a newline before the options begin.
    if (n > 0 && prompt[n - 1] != '\n' && n < (int)sizeof(prompt) - 2) {
        prompt[n++] = '\n';
        prompt[n]   = 0;
    }
    for (int i = 0; i < count; ++i) {
        const char* o = options[i];
        int w = _snprintf_s(prompt + n, sizeof(prompt) - n, _TRUNCATE,
                            "%s%s\n", (o[0] == ' ') ? "" : " ", o);
        if (w < 0) {
            // Silent truncation would look like "some events lose their
            // options" once content is authored externally. Say so.
            Log("  AskChoice: prompt exceeded %d bytes -- option %d of %d and "
                "beyond were DROPPED", (int)sizeof(prompt), i + 1, count);
            break;
        }
        n += w;
    }

    ResetMessage();
    AddText(prompt, a, b);
    WrapText(width, 0);
    return PresentDialog();
}

// Compose body + options into one string (see AskChoice) but forward up to
// kMaxTextArgs positional arguments.
inline int AskChoiceN(const char* body, const char* const* options, int count,
                      const int* args, int argCount, int width)
{
    char prompt[2048];
    int n = _snprintf_s(prompt, sizeof(prompt), _TRUNCATE, "%s", body);
    if (n < 0) n = (int)strlen(prompt);
    if (n > 0 && prompt[n - 1] != '\n' && n < (int)sizeof(prompt) - 2) {
        prompt[n++] = '\n';
        prompt[n]   = 0;
    }
    for (int i = 0; i < count; ++i) {
        const char* o = options[i];
        int w = _snprintf_s(prompt + n, sizeof(prompt) - n, _TRUNCATE,
                            "%s%s\n", (o[0] == ' ') ? "" : " ", o);
        if (w < 0) {
            Log("  AskChoiceN: prompt exceeded %d bytes -- option %d of %d and "
                "beyond were DROPPED", (int)sizeof(prompt), i + 1, count);
            break;
        }
        n += w;
    }

    int v[kMaxTextArgs] = {0};
    if (args) {
        int m = argCount < kMaxTextArgs ? argCount : kMaxTextArgs;
        for (int i = 0; i < m; ++i) v[i] = args[i];
    }

    ResetMessage();
    AddText(prompt, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    WrapText(width, 0);
    return PresentDialog();
}

// Present body text as a MODAL, with up to kMaxTextArgs positional arguments.
//
// This goes through the same renderer as AskChoiceN -- PresentDialog /
// FUN_00430190 -- so an event's outcome keeps the look of the choice card the
// player just answered. With no leading-space option lines the engine renders a
// single Continue.
//
// That is the engine's own pattern: the landing-party follow-up at 0x0046934D
// ("You must have at least 10 crew members...") is body-only text presented
// through FUN_00430190 in exactly this way.
//
// Do NOT use ShowMessage here. That is the full-screen card renderer
// (0x00410C50) and reads as a jarring scene change mid-event.
inline void ShowModalTextN(const char* body, const int* args, int argCount,
                           int width = 0x2C)
{
    int v[kMaxTextArgs] = {0};
    if (args) {
        int m = argCount < kMaxTextArgs ? argCount : kMaxTextArgs;
        for (int i = 0; i < m; ++i) v[i] = args[i];
    }
    ResetMessage();
    AddText(body, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    WrapText(width, 0);
    PresentDialog();
}

// Two-option convenience. Returns true when the player picks `yesText`, which
// is listed SECOND to match the game's own convention (decline first).
inline bool AskYesNo(const char* prompt,
                     const char* noText  = "No.",
                     const char* yesText = "Yes.",
                     int a = 0, int b = 0)
{
    const char* opts[2] = { noText, yesText };
    return AskChoice(prompt, opts, 2, a, b) == 1;
}

// Compose + wrap + present, the same sequence the game uses for a mutiny card.
// Returns the chosen option index, or 0 when there are no options.
inline int ShowEventCard(const char* body, const char* const* options = nullptr,
                         int optionCount = 0, int width = 0x2C)
{
    ResetMessage();
    AddText(body);
    for (int i = 0; i < optionCount; ++i)
        AddTextOption(options[i]);
    WrapText(width, 0);
    return ShowMessage(0);
}

// ------------------------------------------------------------ world queries
inline int PlayerX() { return *(const int*)addr::PlayerX; }
inline int PlayerY() { return *(const int*)addr::PlayerY; }

// The game's own nearest-city search. Plain cdecl, five int args.
// typeMask 0xFFFFFFFF = any city type; exclude 0x80000000 matches the game's
// own call. Returns a city index, or -1 if nothing qualifies within maxDist.
typedef int (__cdecl *FindNearestCity_t)(int x, int y, unsigned typeMask,
                                         int maxDist, unsigned exclude);

inline int NearestCity(int maxDist)
{
    auto fn = (FindNearestCity_t)addr::FindNearestCity;
    // City coordinates are 1/1000 of the player's units.
    return fn(PlayerX() / 1000, PlayerY() / 1000, 0xFFFFFFFFu, maxDist,
              0x80000000u);
}

// Distance from the player to a given city, in city units, using the same
// octagonal approximation the engine uses so our numbers match its own.
inline int CityDistance(int cityIndex)
{
    if (cityIndex < 0) return -1;
    const int* pos = (const int*)(addr::CityPositions + (size_t)cityIndex * 16);
    int dx = pos[0] - PlayerX() / 1000;
    int dy = pos[1] - PlayerY() / 1000;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int d = (dy < dx) ? (dy + dx * 2) : (dx + dy * 2);
    return d / 2;
}

// --------------------------------------------------------------- HUD text
// Draw already-composed text as an on-screen notice, exactly as the game does
// for its own. Must be called during the RENDER phase -- see render.h -- or the
// world is drawn over the top of it.
//
// The game's own pattern, from 0x0046338D:
//     AddText("Press 'r' to return to ship.");
//     DAT_0085a11c = 0x4b;
//     FUN_004b06c0(&DAT_00869b48, screenW/2, 8, 0x4b, 0xffffffff, 4, -1, 0);
//
// eax is a register parameter, 0 at every call site we have examined.
typedef void (__cdecl *DrawHudText_t)(const char* text, int x, int y, int size,
                                      unsigned colour, unsigned flags,
                                      int p7, int p8);

constexpr unsigned kNoticeWhite = 0xFFFFFFFFu;
constexpr unsigned kNoticeBlack = 0xFF000000u;
constexpr int      kNoticeStyle = 0x4B;

__declspec(naked) static void DrawHudTextRaw(const char* text, int x, int y,
                                             int size, unsigned colour,
                                             unsigned flags, int p7, int p8)
{
    __asm {
        mov  eax, [esp + 0x20]      // p8
        push eax
        mov  eax, [esp + 0x20]      // p7
        push eax
        mov  eax, [esp + 0x20]      // flags
        push eax
        mov  eax, [esp + 0x20]      // colour
        push eax
        mov  eax, [esp + 0x20]      // size
        push eax
        mov  eax, [esp + 0x20]      // y
        push eax
        mov  eax, [esp + 0x20]      // x
        push eax
        mov  eax, [esp + 0x20]      // text
        push eax
        xor  eax, eax               // the register parameter: 0 at every site
        mov  edx, PGA_DRAWHUDTEXT
        call edx
        add  esp, 32                // cdecl: we clean up
        ret
    }
}

// Compose `text` (with @-tokens) and draw it centred at `y`.
inline void ShowNotice(const char* text, int y, unsigned colour,
                       const int* args = nullptr, int argCount = 0)
{
    int v[kMaxTextArgs] = {0};
    if (args) {
        int m = argCount < kMaxTextArgs ? argCount : kMaxTextArgs;
        for (int i = 0; i < m; ++i) v[i] = args[i];
    }
    ResetMessage();
    AddText(text, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    *(int*)addr::HudTextStyle = kNoticeStyle;
    DrawHudTextRaw((const char*)addr::MessageText, ScreenW() / 2, y,
                   kNoticeStyle, colour, 4, -1, 0);
}

// ------------------------------------------------------------ sanity checks
// Confirm we are attached to the exe the offsets were derived from, by probing
// bytes we already know from disassembly. Cheap insurance against a different
// build silently corrupting memory.
inline bool VerifyTarget(char* whyNot, size_t cch)
{
    // Every address the mod depends on is load-bearing, including the
    // main-loop call site the safe point is keyed to -- a wrong value there
    // means deferred dispatch silently never runs.
    const struct { uintptr_t va; const uint8_t* bytes; size_t n; const char* what; }
    probes[] = {
        { addr::AddText0,    (const uint8_t*)"\x8B\x4C\x24\x04\x6A\x00", 6, "AddText0" },
        { addr::AddText1,    (const uint8_t*)"\x8B\x4C\x24\x04\x6A\x01", 6, "AddText1" },
        { addr::GetMoraleLevel, (const uint8_t*)"\x0F\xBF\x05",          3, "GetMoraleLevel" },
        { addr::WrapText,    (const uint8_t*)"\x83\xEC\x10\x53\x55",     5, "WrapText" },
        { addr::ShowMessage, (const uint8_t*)"\x83\xEC\x08\x55\x56\x57", 6, "ShowMessage" },
        // call dword ptr [0x006C03A4] -- the main loop's PeekMessageA
        { addr::MainLoopPeekCall,
          (const uint8_t*)"\xFF\x15\xA4\x03\x6C\x00",                    6, "MainLoopPeekCall" },
        { addr::SailingRender, (const uint8_t*)"\x81\xEC\x1C\x01\x00\x00", 6, "SailingRender" },
        { addr::DrawHudText,   (const uint8_t*)"\x81\xEC\x34\x04\x00\x00", 6, "DrawHudText" },
    };

    if ((uintptr_t)GetModuleHandleA(NULL) != 0x00400000) {
        _snprintf_s(whyNot, cch, _TRUNCATE,
                    "image base is 0x%p, expected 0x00400000",
                    GetModuleHandleA(NULL));
        return false;
    }
    for (const auto& p : probes) {
        if (IsBadReadPtr((void*)p.va, p.n) ||
            memcmp((void*)p.va, p.bytes, p.n) != 0) {
            _snprintf_s(whyNot, cch, _TRUNCATE,
                        "byte probe failed at %s (0x%08X)", p.what, (unsigned)p.va);
            return false;
        }
    }
    return true;
}

} // namespace game
