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
// Inline asm cannot see constexpr, so the world-text drawer needs a macro too.
#define PGA_DRAWWORLDTEXT 0x004AEC30

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

    // Positional audio. NOT a text function -- an earlier reading of this
    // project had it as the world-label drawer, which was wrong: it opens with
    // the audio manager at [0x008ECD78], calls its "is initialised" method at
    // +0x18, and bails to the string "The audio manager has not been properly
    // initialized yet". It plays sound `id` at a world position.
    //
    //   FUN_00488A80(int id, float wx, float wy, float wz,
    //                int, int, int, int, int, float gain, int)   -- cdecl
    //
    // The sailing render uses it for ship hails: id 9 at the player's ship,
    // 10 and 11 for other ships depending on nationality standing, with
    // 0xFFFFFFFF for the unused ints and -1.0f for gain. This is the way in
    // for spoken callouts.
    constexpr uintptr_t PlayWorldSound = 0x00488A80;
    constexpr float     kWorldScale    = 0.001f;   // map units -> sound-space
    constexpr int       kSoundPlayerHail = 9;

    // The real world-anchored text drawer: the floating labels that hang over
    // ships and track them as the camera moves ("'We're 2 days out of
    // Antigua.'", "@NATIONALITY @SHIPTYPE '@SHIPNAME'"). 13 call sites.
    //
    //   DrawWorldText(ecx = const char* text, eax = uint32 colour,
    //                 int wx, int wy, int wz, int a4, int a5,
    //                 int a6, int a7, int a8, int a9)
    //
    // Register parameters, then nine cdecl stack arguments -- the caller does
    // `add esp, 0x24`. Verified against both sailing call sites (0x0046231A
    // for the ship-name label, 0x00462E88 for ship speech), which are
    // identical but for wz, a5 and a7.
    //
    // It is NOT a 2D blit: it builds Gamebryo scene-graph nodes that the
    // render walk then draws. That is why it has to be issued at BeginScene,
    // before the world is built, and not at EndScene with the HUD text.
    //
    // The text is a genuine parameter, taken from ecx: the callee saves it
    // (`mov ebx, ecx`) and the string builder at 0x004AEB20 walks it with a
    // plain strlen and copies it. The game passes the shared message buffer
    // 0x00869B48 at both sites, but nothing requires that -- we pass our own.
    //
    // World coordinates are map units divided by 1000 (plain ints, not the
    // 0.001 float scaling the audio call uses).
    constexpr uintptr_t DrawWorldText = 0x004AEC30;

    // Arguments, as the sailing render passes them.
    //   wz  625 for the name label, 500 for speech -- height above sea level.
    //   a4  0 at both sites.
    //   a5  round((flags & kViewFlagClose ? -90 : -70) * [0x00713600]) -- a
    //       camera-relative vertical offset; the game recomputes it per frame.
    //   a6  the camera-height global at 0x008B98D8.
    //   a7  500 for the name label; for speech the game ramps it as
    //       clamp(age * 30 + 200, 0, 500) while the phrase is up.
    //   a8  12 at both sites.
    //   a9  0 -- the target context, which makes the callee use the default
    //       label manager at [0x008C9DD8].
    constexpr uintptr_t WorldTextTilt   = 0x00713600;  // double
    constexpr uintptr_t WorldTextCamera = 0x008B98D8;  // int
    constexpr uintptr_t WorldTextSize   = 0x0085A11C;  // int, read by the drawer
    constexpr uintptr_t ViewFlags       = 0x0085A164;  // int
    constexpr int       kViewFlagClose  = 0x40;
    constexpr unsigned  kWorldTextColour = 0xFF000000; // what both sites pass
    constexpr int       kWorldTextA7Max  = 500;
    constexpr int       kWorldTextA8     = 12;
    constexpr int       kWorldTextZ      = 625;        // the name label's height

    // The scene-graph label manager the drawer attaches to when a9 is 0. Null
    // before the world exists, which is the cheap test for "is it safe to draw
    // world text at all".
    constexpr uintptr_t WorldLabelManager = 0x008C9DD8;

    // `a5` is an ANGLE, not a length: 0x00713600 holds 2^32/360, the classic
    // degrees-to-binary-angle constant, so the game is passing -70 or -90
    // degrees as a full-circle fixed-point value. Worth knowing before anyone
    // tries to "fix" the absurd-looking integer this produces.
    constexpr double    kDegToBam       = 11930464.711111112;

    // The game's own labels are sized by camera distance through this global,
    // recomputed every frame. We draw at BeginScene, before the sailing render
    // touches it, so we set it ourselves. The game's ship labels sit near 50
    // at a normal camera height; 100 (what the render leaves behind) is roughly
    // twice the size a notice wants.
    constexpr int       kWorldTextSize  = 34;

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

    // ...and those two turn out to be FIELDS of a record. The overworld ship
    // array is based at 0x008142F8 with a stride of 0x45C, and the player is
    // entry 0 -- PlayerX and PlayerY land exactly on +0x0C and +0x10 of it,
    // which is how the array was found at all.
    //
    // Recovered from the "She's flying @NATIONALITY colors." site (0x0046BA80),
    // which reads +0x04 of the ship being looked at:
    //     FUN_004f6090("She's flying @NATIONALITY colors.",
    //                  *(short*)(0x008142FC + idx * 0x45C))
    //
    // So +0x04 is the nationality a vessel is SEEN to be -- the colours she
    // flies. The player has one like everybody else; the game simply never
    // offers a way to change it. That is the lever false colours would use.
    // 84 code sites read it, so it is load-bearing rather than decorative.
    //
    // NOT YET ESTABLISHED (this is what the probe is for): whether writing the
    // player's field changes the flag drawn on the ship, changes how the AI
    // treats you, both, or neither.
    constexpr uintptr_t ShipArray       = 0x008142F8;
    constexpr int       kShipStride     = 0x45C;
    constexpr uintptr_t ShipNationality = 0x008142FC;   // int16, +0x04
    constexpr uintptr_t ShipFlags       = 0x00814350;   // dword, +0x58

    // Nation indices, fixed by the flag-mesh table that FUN_0046baa0 builds:
    //   prototypes 0x00860B40 + nation*4   (Flag_Sp/En/Fr/Du/Pi .nif)
    //   live nodes 0x00860B54 + nation*4   (clones via FUN_004bb500)
    // Five slots, hardcoded -- the same shape as the cargo array. Custom nation
    // ART is possible (retexture the node); a SIXTH NATION is not.
    constexpr uintptr_t FlagMeshProto = 0x00860B40;
    constexpr uintptr_t FlagMeshLive  = 0x00860B54;
    constexpr int       kNationCount  = 5;
    enum Nation { kSpanish = 0, kEnglish = 1, kFrench = 2, kDutch = 3, kPirate = 4 };

    // ------------------------------------------------- the PLAYER's flag
    // The nation table above dresses AI vessels. The player's own flag is a
    // TEXTURE, held here and re-applied by FUN_004AF760 to every scene node
    // named "flag*":
    //
    //     if (PlayerFlagTex != 0) {
    //         node = FindNode("flag*");
    //         if (node->texture != PlayerFlagTex) ApplyTexture(PlayerFlagTex);
    //     }
    //
    // So the player's flag is driven by a single pointer, re-asserted every
    // time round -- which is exactly the shape a false-colours feature wants.
    // Written by the Options picker (0x4B888E / 0x4B88B5 / 0x4B89CF) and by
    // config load (0x4293FB), i.e. the "Change Sails and Flags" screen and the
    // CustomFlag line in Config.ini.
    //
    // MEASURED THE HARD WAY (2026-07-28): the ship-record nationality field is
    // NOT the player's flag. Cycling it changed nothing on screen, and it reads
    // 0 on a career started under the English flag -- so it does not even track
    // the nation you sail for. Kept above as a real find about AI vessels; it
    // is simply not this.
    constexpr uintptr_t PlayerColorTex   = 0x008E8FB0;  // ship_playercolor*
    constexpr uintptr_t PlayerFlagTex    = 0x008E8FB4;  // flag*
    constexpr uintptr_t PlayerSailLrgTex = 0x008E8FB8;  // ship_sail_emblem_lrg*
    constexpr uintptr_t PlayerSailSmlTex = 0x008E8FBC;  // ship_sail_emblem_sml*

    // Re-applies the four textures above to the matching scene nodes. Cheap,
    // and a no-op when nothing has changed -- it compares before applying.
    constexpr uintptr_t RefreshPlayerSkin = 0x004AF760;

    // How many custom flags / sails the startup scan found. See
    // re/experiments/screen_state for how that enumeration works.
    constexpr uintptr_t CustomFlagCount = 0x008C9560;
    constexpr uintptr_t CustomSailCount = 0x008C9564;

    // The game's own nearest-city search, FUN_0045FD40.
    //   int FindNearestCity(int x, int y, uint typeMask, int maxDist, uint exclude)
    // Walks the city tables, uses an octagonal distance approximation, and
    // returns the nearest matching city index, or -1 if none is within maxDist.
    constexpr uintptr_t FindNearestCity = 0x0045FD40;

    // City tables it walks. Positions: stride 16 bytes, x at +0, y at +4.
    // Records: stride 0x20, flags at +0.
    constexpr uintptr_t CityPositions = 0x0085B170;
    constexpr uintptr_t CityRecords   = 0x00860B70;

    // ------------------------------------------------- city text arguments
    // What the engine's own text calls pass for a city. Recovered from the
    // sailing render at 0x00462548-0x0046257A, which formats
    // "'We're bound for @CITYNAME.' (@NATIONALITY @LOCTYPE)":
    //
    //   lea  ecx, [ebx + ebx*2]        ; ebx = city index
    //   lea  edx, [ecx*4 + 0x8DBD08]   ; -> record base + index*12
    //   mov  ecx, [edx] / [edx+4] / [edx+8]   ; THREE dwords, all pushed
    //   mov  al,  [edx + 0x860B74]     ; nation, a SIGNED BYTE, stride 32
    //   mov  ecx, ebx / shl ecx, 5
    //   mov  edx, [ecx + 0x860B80]     ; location type, an int, stride 32
    //
    // The critical part is that **@CITYNAME consumes three arguments, not
    // one** -- it is a three-word name record, not a string pointer. Passing
    // one leaves the other two reading stack garbage.
    constexpr uintptr_t CityNameRecords = 0x008DBD08;  // stride 12, 3 dwords
    constexpr size_t    kCityNameStride = 12;
    constexpr size_t    kCityNameWords  = 3;
    constexpr uintptr_t CityNations     = 0x00860B74;  // signed byte, stride 32
    constexpr uintptr_t CityLocTypes    = 0x00860B80;  // int, stride 32
    constexpr size_t    kCityRecStride  = 32;

    // The engine walks 128 settlement slots.
    constexpr int       kMaxCities      = 128;

    // Screen state. NOT an enum -- an earlier playtest established that much and
    // recorded it as a dead end, which was too strong a conclusion. The values
    // read as a bitfield (DAT_007263bc = (&DAT_007268f4)[DAT_00726a84 * 4] in
    // the town code), but taken TOGETHER the pair is a stable per-screen
    // signature, and a 2026-07-28 session that visited every screen separates
    // them cleanly:
    //
    //   sailing / overworld   0x0FFFEFDF, 0x0FFFFFDF   depth 3
    //   town                  0x0FFFEFFA, 0x0FFFFFFA   depth 3
    //   Load / Save           0x0FFBE770, 0x0FFBE750   depth 4
    //   battle                0x8FFFEFFF, 0x8FFFFFFF   depth 4-5
    //   main menu             0x0FFFEFF0, 0x0FFFFFF0   depth 1
    //
    // These are recorded as EVIDENCE, not as constants to compare against --
    // nothing in the framework hardcodes them. triggers.h::WorldOnScreen()
    // learns the overworld's signature at runtime instead, from ticks where the
    // ship demonstrably moved. See the note there for why.
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

    // ---------------------------------------------------------- the renderer
    // The renderer singleton, and the game's real IDirect3DDevice9 inside it.
    // Found from the device-creation sequence around 0x005C60E0: it calls
    // IDirect3D9::CreateDevice on the interface at 0x00728D74 and has it write
    // the new device into renderer+0x60 ("lea edi,[esi+0x60]; push edi").
    // Confirmed independently by 19 sites that read [[0x00727C30]+0x60] and
    // call COM methods on it.
    //
    // This is how the framework gets inside the frame (see d3d9hook.h): read
    // the game's own device, patch its vtable. No throwaway device (its vtable
    // is per-instance heap memory and dies with it), and nothing in the game's
    // code is rewritten.
    constexpr uintptr_t RendererPtr       = 0x00727C30;  // renderer object*
    constexpr uintptr_t RendererDeviceOfs = 0x60;        // IDirect3DDevice9* at +0x60
    constexpr uintptr_t D3D9Ptr           = 0x00728D74;  // IDirect3D9*
}

// ------------------------------------------------------------- state access
inline int32_t&  CrewCount()        { return *(int32_t*)addr::CrewCount; }
inline int32_t&  UndividedPlunder() { return *(int32_t*)addr::UndividedPlunder; }
inline int16_t&  MonthsAtSea()      { return *(int16_t*)addr::MonthsAtSea; }
inline int32_t&  StateFlags()       { return *(int32_t*)addr::StateFlags; }

typedef int (*GetMoraleLevel_t)();
inline int GetMoraleLevel() { return ((GetMoraleLevel_t)addr::GetMoraleLevel)(); }

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

// ------------------------------------------------------- overworld vessels
// Raw record access. The player is index 0; see the note on ShipArray.
inline uintptr_t ShipRecord(int index)
{
    return addr::ShipArray + (uintptr_t)index * addr::kShipStride;
}

// The nationality a vessel is SEEN to be -- what "She's flying @NATIONALITY
// colors." reports. Read as int16, which is how the engine reads it.
inline int ShipNationality(int index)
{
    return *(const short*)(addr::ShipNationality + (uintptr_t)index * addr::kShipStride);
}

// Deliberately raw and deliberately NOT called from content: writing this is
// the whole open question, so it goes through state.h where it can be
// validated, logged and reverted. Nothing else should call it.
inline void SetShipNationalityRaw(int index, int nation)
{
    *(short*)(addr::ShipNationality + (uintptr_t)index * addr::kShipStride) = (short)nation;
}

// ------------------------------------------------------ the player's flag
// The texture currently flown. A pointer to a Gamebryo texture object, or null
// before one has been chosen.
inline void* PlayerFlagTexture() { return *(void**)addr::PlayerFlagTex; }

// Fly a texture the game has already loaded. Deliberately takes a pointer we
// captured from the game itself rather than a name: obtaining a texture by name
// means driving the engine's loader and its refcounting, and this answers
// whether the pointer drives the flag at all before any of that is worth doing.
//
// The game re-asserts the texture on its own (RefreshPlayerSkin compares before
// applying), so writing this is enough -- there is nothing to call afterwards.
inline void SetPlayerFlagTexture(void* tex) { *(void**)addr::PlayerFlagTex = tex; }

inline int CustomFlagCount() { return *(const int*)addr::CustomFlagCount; }
inline int CustomSailCount() { return *(const int*)addr::CustomSailCount; }

inline const char* NationName(int n)
{
    switch (n) {
        case addr::kSpanish: return "Spanish";
        case addr::kEnglish: return "English";
        case addr::kFrench:  return "French";
        case addr::kDutch:   return "Dutch";
        case addr::kPirate:  return "Pirate";
        default:             return "?";
    }
}

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

// The three words of a city's name record, as @CITYNAME consumes them. Returns
// false for an out-of-range index rather than reading a wild address, so a
// stale or -1 city index costs an unsubstituted token, never a crash.
inline bool CityNameWords(int cityIndex, int* out3)
{
    if (cityIndex < 0 || cityIndex >= addr::kMaxCities) return false;
    const int* rec = (const int*)(addr::CityNameRecords +
                                  (size_t)cityIndex * addr::kCityNameStride);
    out3[0] = rec[0]; out3[1] = rec[1]; out3[2] = rec[2];
    return true;
}

// The nation that holds a city, as @NATIONALITY consumes it. A SIGNED byte:
// the engine does `movsx eax, al` before pushing it.
inline int CityNation(int cityIndex)
{
    if (cityIndex < 0 || cityIndex >= addr::kMaxCities) return 0;
    return *(const signed char*)(addr::CityNations +
                                 (size_t)cityIndex * addr::kCityRecStride);
}

// Village / town / city, as @LOCTYPE consumes it.
inline int CityLocType(int cityIndex)
{
    if (cityIndex < 0 || cityIndex >= addr::kMaxCities) return 0;
    return *(const int*)(addr::CityLocTypes +
                         (size_t)cityIndex * addr::kCityRecStride);
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

// Resolve @-tokens into OUR OWN buffer, leaving the game's shared message
// buffer empty behind us.
//
// This matters: the game draws whatever is left in that buffer over the
// player's ship of its own accord, so composing there and walking away makes
// our text appear a second time, in the game's own style, anchored to the
// ship. Compose once when a notice is posted, keep the result, and hand the
// buffer back empty.
inline void ComposeText(const char* text, const int* args, int argCount,
                        char* out, size_t outsz)
{
    if (!out || outsz == 0) return;
    int v[kMaxTextArgs] = {0};
    if (args) {
        int m = argCount < kMaxTextArgs ? argCount : kMaxTextArgs;
        for (int i = 0; i < m; ++i) v[i] = args[i];
    }
    ResetMessage();
    AddText(text, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    strncpy_s(out, outsz, (const char*)addr::MessageText, _TRUNCATE);
    ResetMessage();                      // leave nothing for the game to redraw
}

// Draw already-resolved text centred at `y`. Touches no shared buffer.
inline void ShowNotice(const char* resolved, int y, unsigned colour)
{
    *(int*)addr::HudTextStyle = kNoticeStyle;
    DrawHudTextRaw(resolved, ScreenW() / 2, y, kNoticeStyle, colour, 4, -1, 0);
}

// Call the world-text drawer. Text goes in ecx and the colour in eax, so this
// needs a shim; the nine stack arguments are ordinary cdecl and we clean them
// up ourselves.
__declspec(naked) inline void DrawWorldTextRaw(
    const char* /*text*/, unsigned /*colour*/,
    int /*wx*/, int /*wy*/, int /*wz*/, int /*a4*/, int /*a5*/,
    int /*a6*/, int /*a7*/, int /*a8*/, int /*a9*/)
{
    __asm {
        push ebx
        push ebp
        push esi
        push edi
        // Nine stack arguments, pushed last-to-first. Our own args start at
        // esp+0x14 after the four saves plus the return address.
        mov  eax, [esp + 0x3C]      // a9
        push eax
        mov  eax, [esp + 0x3C]      // a8
        push eax
        mov  eax, [esp + 0x3C]      // a7
        push eax
        mov  eax, [esp + 0x3C]      // a6
        push eax
        mov  eax, [esp + 0x3C]      // a5
        push eax
        mov  eax, [esp + 0x3C]      // a4
        push eax
        mov  eax, [esp + 0x3C]      // wz
        push eax
        mov  eax, [esp + 0x3C]      // wy
        push eax
        mov  eax, [esp + 0x3C]      // wx
        push eax
        mov  eax, [esp + 0x3C]      // colour -> eax
        mov  ecx, [esp + 0x38]      // text   -> ecx
        mov  edx, PGA_DRAWWORLDTEXT
        call edx
        add  esp, 0x24              // cdecl: nine arguments are ours to clean
        pop  edi
        pop  esi
        pop  ebp
        pop  ebx
        ret
    }
}

// Draw already-resolved text anchored in the world at a map position, so it
// hangs there and tracks as the camera moves -- the treatment the game gives
// ship speech. Must be issued at BeginScene; see the note on DrawWorldText.
//
// `fade` is the game's seventh argument, which it ramps 200 -> 500 while a
// ship's line is up. We drive it the other way to bring a notice down.
inline void ShowWorldText(const char* resolved, int mapX, int mapY,
                          int fade, int size = addr::kWorldTextSize,
                          unsigned colour = addr::kWorldTextColour)
{
    if (!resolved || !*resolved) return;
    if (!*(void**)addr::WorldLabelManager) return;   // no world to hang it in

    const int flags  = *(int*)addr::ViewFlags;
    const int camera = *(int*)addr::WorldTextCamera;

    // The tilt, as an angle in binary-angle units.
    const int base = (flags & addr::kViewFlagClose) ? -90 : -70;
    const int a5   = (int)(base * addr::kDegToBam);

    // The ship-name label lifts itself by 500 map units only when the camera
    // global is zero -- `setne bl; dec ebx; and ebx, 0x1F4; sub ecx, ebx` at
    // 0x004622C2. Reproduced rather than approximated, because getting it
    // wrong puts the text below the hull instead of above it.
    const int lift = camera ? 0 : 500;

    *(int*)addr::WorldTextSize = size;

    DrawWorldTextRaw(resolved, colour,
                     mapX, mapY - lift, addr::kWorldTextZ,
                     0,
                     a5,
                     camera,
                     fade,
                     addr::kWorldTextA8,
                     0);
}

// ------------------------------------------------------- trade-good probing
// Resolve the engine's own name for a trade good, by asking its text system to
// substitute @ITEM. That token is a __VAR lookup into the [ITEM] list parsed
// from text.ini at runtime, so this reads back exactly what the game believes
// the item list to be -- which is the only honest way to find out whether the
// list length is the file's or the engine's.
//
// Returns false if nothing came back. NOTE that an index past the end of the
// list is, as far as we know, unguarded inside the engine: this is a probe, not
// something to call in normal operation.
inline bool ItemName(int index, char* out, size_t outsz)
{
    if (!out || outsz == 0) return false;
    out[0] = '\0';
    ResetMessage();
    AddText("@ITEM", index);
    strncpy_s(out, outsz, (const char*)addr::MessageText, _TRUNCATE);
    ResetMessage();
    return out[0] != '\0';
}

// The player ship's map position, in the units the world-text API expects:
// the raw milli-unit globals divided by 1000, exactly as the game does it.
inline void PlayerMapPos(int* mx, int* my)
{
    *mx = *(int*)addr::PlayerX / 1000;
    *my = *(int*)addr::PlayerY / 1000;
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
