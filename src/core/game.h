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
// Asset lookup and texture loading, both taking their name in ESI.
#define PGA_ASSETEXISTS   0x004F4ED0
#define PGA_LOADTEXTURE   0x00500850

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
    // SETTLED SINCE: writing the player's field does NOT change the flag drawn
    // on the ship -- that is a texture, see below. So the two are separate
    // switches, and PEMF has only ever thrown the visual one. Whether the AI
    // consults this field is the open question, and the reason it matters:
    // if it does, the disguise stops being cosmetic. state::SetNationality()
    // already writes it safely and reversibly.
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

    // The names behind those counts. Each container is the game's own growable
    // array, filled by the directory scan:
    //     +0x04  char** entries
    //     +0x08  capacity
    //     +0x0C  count
    constexpr uintptr_t FlagList = 0x00726AA8;
    constexpr uintptr_t SailList = 0x00726A90;
    constexpr int       kListEntries  = 0x04;
    constexpr int       kListCapacity = 0x08;
    constexpr int       kListCount    = 0x0C;

    // The currently-selected names, as written to Config.ini. Pointers to the
    // engine's own string type: characters at the pointer, LENGTH IN THE DWORD
    // AT -4. Reading past the pointer without honouring that is how you get a
    // name that looks right and is not.
    constexpr uintptr_t CustomSailName = 0x00726A88;
    constexpr uintptr_t CustomFlagName = 0x00726A8C;

    // Asset lookup and texture load, recovered from the config-load path at
    // 0x004293D7 -- the code that turns `CustomFlag = <name>` into the texture
    // the mast flies. Both take the name in ESI and end in a plain `ret`, so
    // there is no stack to clean.
    //
    //   char  AssetExists()   esi = name
    //   void* LoadTexture()   esi = name, eax = format struct, or 0 for
    //                         defaults (with 0 it requires the name to end
    //                         ".dds" and picks the format itself)
    //
    // The same path also shows the ownership rules, which is why it is worth
    // copying wholesale rather than paraphrasing: release the outgoing texture
    // (decrement +4, and if it reaches zero call vtable[0](1)), store the new
    // pointer, then AddRef it. Skipping the AddRef is what crashed the game the
    // first time PEMF held one of these.
    constexpr uintptr_t AssetExists = 0x004F4ED0;
    constexpr uintptr_t LoadTexture = 0x00500850;

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

    // ------------------------------------------------ who hates whom
    // The nation relations matrix. An 8x8 grid of int32, indexed
    // [a * 8 + b], holding the state of the relationship between two powers:
    //
    //     1  at war        -1  treaty        0  neutral
    //
    // Size is not a guess. The new-game reset at 0x00404229 clears it with
    //     mov ecx, 0x40 / mov edi, 0x85a168 / rep stosd
    // which is exactly 64 dwords, and the next global (0x0085A268, the screen
    // width) begins immediately after. Only 6x6 of it is ever used.
    //
    // The indexing came from gameplay sites that reach it through a CITY's
    // nation byte, which pins both the stride and the meaning of the values:
    //     0x0040C9A0  movsx ecx, byte [ebp + 0x860B74]   ; city nation
    //     0x0040C9A7  lea   edx, [ebx + ecx*8]
    //     0x0040C9AA  cmp   dword [edx*4 + 0x85A168], 1  ; ...at war?
    // and the mirror of it at 0x0040CB2E comparing against -1 for a treaty.
    // The Pedia's "International Relations" page (FUN_0043cde0) draws the same
    // grid from the same memory, which is what made it findable.
    //
    // Slots 4 and 5 are set AT WAR with all four crowns and left that way --
    // the reset loop at 0x004042E0-0x0040431D writes 1 into row 4, row 5, and
    // columns 4 and 5 of every nation's row. Slot 4 is Pirate (it matches the
    // flag-mesh table above); slot 5 is a sixth power the framework has not
    // needed to identify yet.
    constexpr uintptr_t NationRelations = 0x0085A168;
    constexpr int       kRelationStride = 8;    // ints per row
    constexpr int       kRelationSlots  = 6;    // 4 crowns + Pirate + one more
    constexpr int       kAtWar          =  1;
    constexpr int       kTreaty         = -1;

    // ------------------------------------------- the player's standing
    // Two parallel word[4] arrays in the player record, both indexed
    // nation*2. They are what the game means by "how do they feel about you",
    // and between them they replace the search for a single "chosen faction"
    // global -- see DeriveHomeNation() in nations.h.
    //
    // Found together at the promotion check, 0x0040D38F onward:
    //     movsx eax, byte [ebp + 0x860B74]        ; nation
    //     cmp   word [eax*2 + 0x869A88], 9        ; already at the top rank?
    //     cmp   word [eax*2 + 0x869A78], 3        ; reputation high enough?
    //     inc   word [eax*2 + 0x869A88]           ; promote
    //
    // Rank 0 means NO LETTER OF MARQUE with that nation, and selects the
    // "you hold no commission here" governor dialogue (0x0040C90C, 0x0040C92E,
    // 0x0040CAB2). Ranks run 0..9 and index the name table below.
    constexpr uintptr_t PlayerReputation = 0x00869A78;  // int16, stride 2
    constexpr uintptr_t PlayerRank       = 0x00869A88;  // int16, stride 2

    // THE NATION THE PLAYER SERVES. An int16, and the answer to a question this
    // project spent a long time treating as unanswerable.
    //
    // The search kept looking for a value written at character creation and
    // never finding one, because the game does not store the choice -- it
    // stores a CONSEQUENCE of it, and recomputes it. At 0x0040D690, on every
    // promotion:
    //     esi = 1
    //     for (n = 0; n <= 3; ++n)              ; every crown
    //         if (n != cand && rank[n] >= rank[cand]) esi = 0
    //     if (esi) PlayerNation = cand          ; strictly the highest rank
    //
    // So "your nation" is the one you outrank the others with, and this global
    // is where the engine caches its own answer. Reading it beats deriving it:
    // it is the value the game itself acts on.
    //
    // Corroborated three further ways:
    //   * compared straight against a city's nation byte  (0x0040DA19)
    //   * pushed where @NATIONALITY is expected, for "We do not trade with
    //     @NATIONALITY heretics."                          (0x0040FF62)
    //   * MEASURED: four careers begun under four different crowns read four
    //     distinct values in 0..3, at record offset 56     (2026-07-28)
    //
    // Not to be confused with the ship record's nationality field, which was
    // measured stuck at 0 for the player no matter which crown was chosen.
    constexpr uintptr_t PlayerNation = 0x00869AA8;      // int16
    constexpr int       kNationsWithRank = 4;           // the crowns only
    constexpr int       kMaxRank         = 9;

    // char* [10], indexed by rank. Read at 0x004BCDD9 while building the
    // player's own outfit texture name:
    //   Grunt, Grunt, Captain, Major, Colonel, Admiral, Baron, Count,
    //   Marquis, Duke
    constexpr uintptr_t RankNames    = 0x007272B4;
    constexpr int       kRankNameMax = 10;

    // The pending-career record. At career start the whole player block is
    // copied out of here, at 0x00401BA6:
    //     mov ecx, 0x2E / mov esi, 0x72C6B8 / mov edi, 0x869A70 / rep movsd
    //     mov ecx, 0x24 / mov esi, 0x72C6E0 / mov edi, 0x869AA8 / rep movsd
    // followed by zeroing the odd (high) bytes of the two arrays above.
    //
    // This address lies PAST the end of the file's raw .data, so it holds
    // nothing on disk -- it is filled at runtime, by character creation. That
    // makes it the place the game records what you chose before there is a
    // career to record it in.
    // Both buffers have EXACT sizes, and they come from the save serializer at
    // 0x00401400 rather than from a guess. That function pushes (address, size)
    // for every block the game persists and calls read-or-write on each; the
    // player record goes over as 0xD8 bytes and the staging buffer as 0xB8:
    //     push 0xB8 / push 0x72C6B8   ; only when the mode word is 2
    //     push 0xD8 / push 0x869A70   ; otherwise
    // Mode 2 is character creation -- that is the one case where the pending
    // record is the thing worth saving, because no live career exists yet.
    //
    // The same function is the reason kRelationBytes below is not an estimate:
    // the matrix is written out as exactly 0x100 bytes.
    constexpr uintptr_t CareerStaging    = 0x0072C6B8;
    constexpr size_t    kCareerStgBytes  = 0xB8;   // 184
    constexpr uintptr_t PlayerRecord     = 0x00869A70;
    constexpr size_t    kPlayerRecBytes  = 0xD8;   // 216, ending at MessageText
    constexpr size_t    kRelationBytes   = 0x100;

    // From the same serializer: the overworld ship array is written out as
    // 0x45C00 bytes, which over a 0x45C stride is exactly 256 SLOTS -- not the
    // 24 this framework has always scanned. The low indices are what the game
    // keeps near the player, so 24 has been a serviceable window rather than a
    // correct one, and anything that needs the whole sea has to know better.
    constexpr int       kMaxShips = 256;

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

// ------------------------------------------------------------ the shipyard
// Calling the engine's own ship factory.
//
// This is the primitive a false-colours consequence needs, and it exists
// because the alternative does not: NOTHING in the binary reads the player's
// nationality, so there is no "make that ship hostile" switch to throw. What
// the game does when a crown decides you are a problem is BUILD A SHIP AND SEND
// IT -- a pirate hunter, a privateer, a blockade squadron. So we build one too,
// through the same function it uses.
//
//     FUN_00414FC0(cityIndex, kind) -> new slot index, or -1
//
// Plain __cdecl. It walks the array from slot 8 looking for a free record (type
// word == -1), zeroes the whole 0x45C, sets flags |= 0x800, and fills the ship
// in from the city. Slot 0 is the player and 1-7 are reserved, which is why the
// scan starts where it does.
//
// ⚠️ THIS CREATES REAL GAME STATE THAT PERSISTS INTO SAVES. It is not a probe
// that can be undone by pressing the key again. Everything that calls it is
// behind its own marker file, and the first use of it belongs on a save nobody
// minds losing.
constexpr uintptr_t SpawnShipFn = 0x00414FC0;

// The first bytes of that function on both the GOG and the packed builds:
//     83 EC 08        sub esp, 8
//     56              push esi
//     BE 08 00 00 00  mov esi, 8        <- the slot scan's starting index
// Checked before every call. Calling a wrong address does not fail politely, it
// executes whatever is there, and "the address moved" is exactly the failure a
// signature catches cheaply.
constexpr unsigned char kSpawnShipSig[] = {
    0x83, 0xEC, 0x08, 0x56, 0xBE, 0x08, 0x00, 0x00, 0x00
};

// There is a FAMILY of these, not one function, and the difference between them
// is the ROLE the new ship gets. That is the finding that came out of the first
// live test: a ship built by the factory above turns straight round and sails
// home, because it is built with role 0 -- ordinary traffic with no orders.
//
//   FUN_00414FC0(city, kind)   role 0   flags 0x800    nationality from city
//   FUN_00415290(city, type)   role 4   flags 0x200    nationality from city
//   FUN_004154F0(type)         role 3   flags 0x1400   nationality 0, port from
//                                                      the global at 0x00722A08
//
// So "what is this ship for" is not a field to be patched after the fact; it is
// chosen by which builder is called, and each one sets up its ship completely.
// Patching role afterwards would leave the other fields belonging to a
// different kind of vessel, which is the sort of half-state that produces a bug
// nobody can reproduce.
constexpr uintptr_t SpawnShipRole4Fn = 0x00415290;
constexpr uintptr_t SpawnShipRole3Fn = 0x004154F0;

constexpr unsigned char kSpawnRole4Sig[] = {
    0x83, 0xEC, 0x0C, 0x53, 0x55, 0x8B, 0xD8, 0xBD, 0x08, 0x00
};
constexpr unsigned char kSpawnRole3Sig[] = {
    0x51, 0x56, 0xBE, 0x08, 0x00, 0x00, 0x00
};

inline bool BytesMatch(uintptr_t va, const unsigned char* sig, size_t n)
{
    __try {
        const unsigned char* p = (const unsigned char*)va;
        for (size_t i = 0; i < n; ++i) if (p[i] != sig[i]) return false;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline bool SpawnShipCallable()
{
    return BytesMatch(SpawnShipFn, kSpawnShipSig, sizeof(kSpawnShipSig));
}
inline bool SpawnRole4Callable()
{
    return BytesMatch(SpawnShipRole4Fn, kSpawnRole4Sig, sizeof(kSpawnRole4Sig));
}
inline bool SpawnRole3Callable()
{
    return BytesMatch(SpawnShipRole3Fn, kSpawnRole3Sig, sizeof(kSpawnRole3Sig));
}

typedef int (__cdecl *SpawnShip_t)(int cityIndex, int kind);
typedef int (__cdecl *SpawnRole3_t)(int shipType);

// Returns the new ship's slot index, or -1 if the array is full or the
// signature check failed. Never called from content or from the render hook.
inline int SpawnShipAtCity(int cityIndex, int kind)
{
    if (!SpawnShipCallable()) return -1;
    if (cityIndex < 0 || cityIndex >= addr::kMaxCities) return -1;
    __try {
        return ((SpawnShip_t)SpawnShipFn)(cityIndex, kind);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// The city the role-3 builder uses. It reads this rather than taking a port,
// which is a problem: in a fresh career it is not set, and the builder happily
// makes ships at a null city. MEASURED -- three of them appeared at map
// position (1,2), the corner of the world, and a handful of those crashed the
// game. So the global is checked before the call rather than after.
constexpr uintptr_t Role3HomeCity = 0x00722A08;

inline int Role3HomeCityIndex()
{
    __try { return *(const int*)Role3HomeCity; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Role 3. Plain __cdecl, one argument -- but it picks its own port, so the only
// way to keep it safe is to refuse when that port is not a real one.
inline int SpawnRole3Ship(int shipType)
{
    if (!SpawnRole3Callable()) return -1;
    const int city = Role3HomeCityIndex();
    if (city <= 0 || city >= addr::kMaxCities) return -2;   // -2: bad global
    __try {
        return ((SpawnRole3_t)SpawnShipRole3Fn)(shipType);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Role 4 takes its city in EAX and its ship type on the stack -- a mixed
// convention the compiler will not produce, so it needs a shim. The callee
// cleans nothing; the caller pops the one argument.
constexpr uintptr_t PGA_SPAWN_ROLE4 = SpawnShipRole4Fn;

__declspec(naked) static int SpawnRole4Raw(int /*cityIndex*/, int /*shipType*/)
{
    __asm {
        push ebx
        mov  eax, [esp + 8]         // cityIndex -> eax, where it is expected
        mov  ebx, [esp + 12]        // shipType
        push ebx                    // ...goes on the stack
        mov  edx, PGA_SPAWN_ROLE4
        call edx
        add  esp, 4
        pop  ebx
        ret
    }
}

inline int SpawnRole4Ship(int cityIndex, int shipType)
{
    if (!SpawnRole4Callable()) return -1;
    if (cityIndex < 0 || cityIndex >= addr::kMaxCities) return -1;
    __try {
        return SpawnRole4Raw(cityIndex, shipType);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// A free slot is marked by a type word of -1. Exposed so a caller can count
// them before and after, which is how the experiment proves the call did
// anything at all rather than merely returning a number.
inline int ShipType(int index)
{
    __try {
        return *(const short*)(addr::ShipArray + (uintptr_t)index * addr::kShipStride);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Field offsets are derived from the absolute addresses the disassembly uses,
// and the arithmetic is worth doing carefully: role was first written down as
// +0x22 when 0x00814322 - 0x008142F8 is 0x2A, so the first live test read a
// neighbouring field and reported every ship as role 0 -- including two built
// by functions that demonstrably write 4 and 3. The flags were right, which is
// what showed the calls were fine and the readout was not.
constexpr int kShipRole     = 0x2A;   // 0x00814322
constexpr int kShipDestCity = 0x3E;   // 0x00814336
constexpr int kShipHomeCity = 0x40;   // 0x00814338

// WHAT THIS SHIP IS. The field that actually classifies a vessel, and not the
// one at +0x2A this project spent three test rounds calling "role".
//
// Proved by the label code at 0x00462098, which is the text you get when you
// hover a ship on the overworld:
//
//     movsx eax, word [edi + 0x8142FA]      ; <- +0x02
//     dec   eax
//     cmp   eax, 3
//     ja    no_label
//     jmp   dword [eax*4 + 0x00463CB4]      ; a four-entry jump table
//
// and the table resolves, in order, to the four strings at 0x00707A60..AA4:
//
//     1  "@NATIONALITY pirate-hunter"
//     2  "@NATIONALITY privateer"
//     3  "@NATIONALITY raider"
//     4  "@NATIONALITY smuggler"
//
// 0 is an ordinary merchant, which gets the plain "@NATIONALITY @SHIPTYPE
// '@SHIPNAME'" label instead. Values 5 and 6 are written by the game but have
// no label; 6 comes out of two builders, so it is probably "escort" or similar.
//
// This is why a spawned ship showed NO hover text: the builders leave it at a
// value the label code does not name.
constexpr int kShipPurpose = 0x02;    // 0x008142FA

enum ShipPurpose {
    kPurposeMerchant     = 0,
    kPurposePirateHunter = 1,
    kPurposePrivateer    = 2,
    kPurposeRaider       = 3,
    kPurposeSmuggler     = 4
};

inline const char* PurposeName(int p)
{
    switch (p) {
    case kPurposeMerchant:     return "merchant";
    case kPurposePirateHunter: return "PIRATE-HUNTER";
    case kPurposePrivateer:    return "privateer";
    case kPurposeRaider:       return "raider";
    case kPurposeSmuggler:     return "smuggler";
    case 5:                    return "5 (unlabelled)";
    case 6:                    return "6 (unlabelled)";
    default:                   return "?";
    }
}

inline int ShipPurposeOf(int index)
{
    __try {
        return *(const short*)(addr::ShipArray +
                               (uintptr_t)index * addr::kShipStride + kShipPurpose);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// ------------------------------------------------- dispatching a hunter
// The game's own pirate-hunter recipe, reproduced from 0x0045F060 (and its
// duplicate at 0x00465670), which is what a crown does when it has had enough
// of you:
//
//     slot = FUN_00414FC0(city, kind)          ; the factory PEMF already calls
//     rep  = reputation[CityNation(city)]
//     str  = 2 - rep/10                        ; imul 0x66666667 / sar 1
//     ship[slot].purpose = 1                   ; PIRATE-HUNTER
//     clamp str to [2, 4]
//
// **The worse your standing, the stronger the hunter.** That single line is the
// whole relationship between reputation and being pursued, and it means a
// suspicion system that drives reputation is not merely thematic -- it is
// turning the dial the game already reads.
//
// The kind argument comes from a table at 0x007252D0 in the original; we pass
// the one value the game is otherwise known to use.
constexpr uintptr_t HunterKindTable = 0x007252D0;

inline int HunterStrengthFor(int nation)
{
    // Reputation is a signed word; the divide matches the engine's.
    int rep = 0;
    __try { rep = *(const short*)(addr::PlayerReputation + (uintptr_t)nation * 2); }
    __except (EXCEPTION_EXECUTE_HANDLER) { rep = 0; }
    int s = 2 - (rep / 10);
    if (s < 2) s = 2;
    if (s > 4) s = 4;
    return s;
}

inline void SetShipPurposeRaw(int index, int purpose)
{
    __try {
        *(short*)(addr::ShipArray + (uintptr_t)index * addr::kShipStride
                  + kShipPurpose) = (short)purpose;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

inline int ShipRole(int index)
{
    __try {
        return *(const short*)(addr::ShipArray +
                               (uintptr_t)index * addr::kShipStride + kShipRole);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Orders, such as they are. A ship built by either factory comes out with its
// destination set to the port it was built at, which is to say: already there,
// nothing to do. That -- not a missing role -- is why the first spawns sailed
// home or sat still.
//
// Deliberately raw, deliberately only called from the shipyard experiment. If
// this turns into a shipped feature it belongs behind something validated.
inline void SetShipRoleRaw(int index, int role)
{
    __try {
        *(short*)(addr::ShipArray + (uintptr_t)index * addr::kShipStride
                  + kShipRole) = (short)role;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

inline void SetShipDestCityRaw(int index, int cityIndex)
{
    __try {
        *(short*)(addr::ShipArray + (uintptr_t)index * addr::kShipStride
                  + kShipDestCity) = (short)cityIndex;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

inline int ShipDestCity(int index)
{
    __try {
        return *(const short*)(addr::ShipArray +
                               (uintptr_t)index * addr::kShipStride + kShipDestCity);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

inline int ShipHomeCity(int index)
{
    __try {
        return *(const short*)(addr::ShipArray +
                               (uintptr_t)index * addr::kShipStride + kShipHomeCity);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

inline unsigned ShipFlagBits(int index)
{
    __try {
        return *(const unsigned*)(addr::ShipArray + (uintptr_t)index * addr::kShipStride + 0x58);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

inline int CountFreeShipSlots()
{
    int n = 0;
    for (int i = 8; i < addr::kMaxShips; ++i)
        if (ShipType(i) == -1) ++n;
    return n;
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

// ------------------------------------------------- refcounted engine objects
// The count is the dword at +4 and reaching zero calls vtable[0](1). Both
// halves are copied from the config-load path at 0x00429403, not guessed --
// and getting the AddRef wrong is what crashed the game the first time PEMF
// held a texture. See DEVELOPER.md's layer rules.
constexpr int kRefCount = 4;

inline void AddRef(void* obj)
{
    if (obj) ++*(long*)((char*)obj + kRefCount);
}

inline void Release(void* obj)
{
    if (!obj) return;
    long* rc = (long*)((char*)obj + kRefCount);
    if (--*rc == 0) {
        void** vtbl = *(void***)obj;
        if (vtbl && vtbl[0])
            ((void (__thiscall*)(void*, int))vtbl[0])(obj, 1);
    }
}

inline int CustomFlagCount() { return *(const int*)addr::CustomFlagCount; }
inline int CustomSailCount() { return *(const int*)addr::CustomSailCount; }

// ------------------------------------------------- the enumerated name lists
// Whatever the directory scan found, in the order the picker shows it. Both
// return null rather than reaching past the end.
inline int ListCount(uintptr_t list)
{
    return *(const int*)(list + addr::kListCount);
}

inline const char* ListName(uintptr_t list, int index)
{
    if (index < 0 || index >= ListCount(list)) return nullptr;
    const char* const* entries = *(const char* const**)(list + addr::kListEntries);
    return entries ? entries[index] : nullptr;
}

inline int FlagListCount()            { return ListCount(addr::FlagList); }
inline const char* FlagName(int i)    { return ListName(addr::FlagList, i); }
inline int SailListCount()            { return ListCount(addr::SailList); }
inline const char* SailName(int i)    { return ListName(addr::SailList, i); }

// The engine's string type: characters at the pointer, length at -4.
inline const char* EngineString(uintptr_t slot, int* lengthOut = nullptr)
{
    const char* s = *(const char* const*)slot;
    if (!s) { if (lengthOut) *lengthOut = 0; return nullptr; }
    if (lengthOut) *lengthOut = *(const int*)(s - 4);
    return s;
}

// ------------------------------------------------------ loading by name
// Both callees take the name in ESI and clean nothing, so the shims only have
// to place the register and get out of the way.
__declspec(naked) static char AssetExistsRaw(const char* /*name*/)
{
    __asm {
        push esi
        mov  esi, [esp + 8]         // name
        mov  eax, PGA_ASSETEXISTS
        call eax
        pop  esi
        ret
    }
}

__declspec(naked) static void* LoadTextureRaw(const char* /*name*/)
{
    __asm {
        push esi
        mov  esi, [esp + 8]         // name
        xor  eax, eax               // default format, as the config path uses
        mov  edx, PGA_LOADTEXTURE
        call edx
        pop  esi
        ret
    }
}

// Fly the flag with this name. The whole point of the exercise: a name an
// author or a player can write, rather than a pointer we happened to catch.
//
// The sequence is the config path's, in its order, because the ownership rules
// are not ours to invent: load, release the outgoing texture, store, AddRef.
// Doing the release before the store means a name that resolves to the texture
// already flying is handled by the early return rather than by a refcount that
// briefly hits zero.
//
// Returns false and changes nothing if the asset does not exist or will not
// load -- a refused swap is always better than a bad pointer on the mast.
inline bool SetPlayerFlagByName(const char* name)
{
    if (!name || !*name) return false;
    if (!AssetExistsRaw(name)) return false;

    void* tex = LoadTextureRaw(name);
    if (!tex) return false;

    void* old = *(void**)addr::PlayerFlagTex;
    if (old == tex) return true;             // already flying it

    *(void**)addr::PlayerFlagTex = tex;
    AddRef(tex);
    Release(old);
    return true;
}

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

// Empty the game's shared message buffer. Anything left in it is redrawn over
// the player's ship by the sailing render, on its own, next frame -- so this
// must be called after any use of it, including a plain HUD draw, which uses
// the buffer as scratch even when we composed nothing ourselves. Writing a lone
// terminator is the game's own idiom for the job.
inline void ClearMessageBuffer()
{
    __try { *(char*)addr::MessageText = 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
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
