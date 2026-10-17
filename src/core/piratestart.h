// piratestart.h - a new career signs on with no crown and begins in a pirate haven.
//
// Governed by suspicion.ini startUnaligned (on by default). Off, character
// creation is exactly the shipped one.
//
// ============================================================================
// HOW A CAREER BEGINS (decompiled; FUN_0042B900's new-game case, 0x0042BDBD)
// ============================================================================
//   0x00471AB0  character creation. Copies the default settlement records
//               (0x007242D0 -> 0x00860B68, 0x400 dwords), then:
//   FUN_0046FE30  the tavern. "There be French, Spanish, Dutch and English
//               ships... Take yer pick!" -- a loop over four captains' tables
//               that runs while [0x00876BE0] == -1. Picking a table stores a
//               random large colonial city (index < 0x2C) of that crown there:
//                   0x00470391  cmp [0x876BE0], edi   ; edi = -1
//                   0x00470397  jne 0x00470782        ; loop done
//               Then the walk to the chosen table plays, from the crown's own
//               switch: [esp + ebp*4 + 0x80] at 0x004707BB is local_154[ebp],
//               the table's crown ({2,0,3,1} at [esp+0x70] in the loop), and
//               anything above 3 takes the default (no walk, straight on).
//   FUN_0046C8D0(0)  generates the settlements: villages, missions, and the
//               PIRATE HAVENS, nation byte 4 (0x0046D597 and the switch above it).
//   FUN_00471E00(0)  career setup. Picks the start city EDI: a random large
//               colonial city of the same crown as [0x00876BE0] (0x00471F9B..
//               0x00471FE6), then
//                   0x00471FE8  mov ecx, 8            ; table fill begins
//                   0x00472264  mov cl, [esi+0x860B74]; esi = EDI*32: its crown
//                   0x0047226D  index = (crown + era*4) * 16 into a 20-row table
//                               of starting ship / crew / cannon / gold
//                   0x004722F7  0x00869AA8 = crown    ; the "nation" cache
//                   0x00472318  crown != 0 -> at war with Spain (row 4 already is)
//                   0x0047235C  ship position from the city's harbour direction
//                               (word [city*0x94 + 0x85BFF6]) and position
//                               (0x0085B170); map position from 0x00860B68/6C
//
// PEMF:
//   1. 0x00470391  the tavern loop never starts: a sign-up city of a random
//      crown is chosen for it (the same test the game uses), and the table's
//      crown is set past 3 so the walk to a captain's table is skipped too.
//   2. 0x00471FE8  the start city becomes a pirate haven, if one exists.
//   3. 0x00472264  the starting-stats row is still read with a CROWN (the
//      sign-up crown), never the haven's 4, which would index past the table.
// Everything else -- position, map, the war-with-Spain step -- reads the haven
// itself and so places the ship there. ApplyCareerFlag (core.cpp) then sets the
// nation cache to Pirate and flies the black flag.
//
// ============================================================================
// THE CAPTAIN'S CLOTHES (decompiled, FUN_004BCD90; playtest 2026-10-17)
// ============================================================================
// The nation cache 0x00869AA8 is NOT only text. FUN_004BCD90(outfit, nation,
// head) dresses the player's model, and seven callers (0x0044F2D9, 0x0045337B,
// 0x0045338D, 0x00483745, 0x00483C7F, 0x00486031, 0x0048715E, 0x004D5FE1) pass
// it as `nation`:
//     sprintf(tex, "PC_%s_%c.dds", outfitName[outfit], 0x0070D594[nation]);
// 0x0070D594 is "SEFD" -- one letter per crown -- and the byte after it, the
// Pirate's, is 0. So a Pirate asked for "PC_Grunt_.dds", which does not exist,
// and the model drew pink. Only S, E, F and D textures ship (Pak0), so the
// Pirate gets a crown's colouring: PEMF writes that letter into the empty fifth
// slot (suspicion.ini pirateOutfit, 0..3). The four crowns' letters are not
// touched.
#pragma once
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "log.h"
#include "game.h"
#include "suspicion.h"   // g_tune.startUnaligned

namespace piratestart {

constexpr uintptr_t kTavernSite = 0x00470391;   // 39 3D E0 6B 87 00 (then 0F 85)
constexpr uintptr_t kStartSite  = 0x00471FE8;   // B9 08 00 00 00
constexpr uintptr_t kStatsSite  = 0x00472264;   // 8A 8E 74 0B 86 00
constexpr uintptr_t kSignUpCity = 0x00876BE0;   // int32, -1 until a table is picked

constexpr uintptr_t kRecords    = 0x00860B70;   // flags +0, nation +4, size +5, economy +0xC
constexpr uintptr_t kHarbourDir = 0x0085BFF6;   // int16, stride 0x94: 0..23
constexpr int       kColonial   = 0x2C;         // indices below are the named cities
constexpr int       kSlots      = 128;
constexpr int       kPirate     = 4;

const uint8_t kTavernBytes[] = { 0x39, 0x3D, 0xE0, 0x6B, 0x87, 0x00, 0x0F, 0x85 };
const uint8_t kStartBytes[]  = { 0xB9, 0x08, 0x00, 0x00, 0x00 };
const uint8_t kStatsBytes[]  = { 0x8A, 0x8E, 0x74, 0x0B, 0x86, 0x00 };

constexpr uintptr_t kOutfitLetters = 0x0070D594;   // "SEFD" then 0
const uint8_t kOutfitBytes[] = { 'S', 'E', 'F', 'D', 0 };

inline bool g_installed = false;
inline bool g_outfitPatched = false;
inline int  g_pirateOutfit = 1;   // the crown whose colouring a Pirate wears

inline void LoadSettings(const char* gameDir)
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\PEMF\\suspicion.ini", gameDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") == 0 && f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char key[64] = {0};
            int v = 0;
            if (sscanf_s(line, " %63[A-Za-z_] = %d", key, (unsigned)sizeof(key), &v) != 2) continue;
            if (_stricmp(key, "pirateOutfit") == 0) g_pirateOutfit = v < 0 ? 0 : (v > 3 ? 3 : v);
        }
        fclose(f);
    }
}

// The Pirate's letter in the outfit table. Independent of startUnaligned: a
// save made with a Pirate start still needs clothes.
inline void PatchOutfit()
{
    if (g_outfitPatched) return;
    __try {
        if (memcmp((const void*)kOutfitLetters, kOutfitBytes, sizeof(kOutfitBytes))) {
            Log("piratestart: outfit letters are not what we expect -- a Pirate may draw untextured");
            return;
        }
        DWORD old = 0;
        if (!VirtualProtect((void*)(kOutfitLetters + 4), 1, PAGE_EXECUTE_READWRITE, &old)) return;
        *(uint8_t*)(kOutfitLetters + 4) = kOutfitBytes[g_pirateOutfit];
        VirtualProtect((void*)(kOutfitLetters + 4), 1, old, &old);
        g_outfitPatched = true;
        Log("piratestart: a Pirate dresses in %c colours (pirateOutfit=%d)",
            kOutfitBytes[g_pirateOutfit], g_pirateOutfit);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

inline uint32_t Flags(int c)   { return *(const uint32_t*)(kRecords + c * 32); }
inline int      Nation(int c)  { return *(const signed char*)(kRecords + c * 32 + 4); }
inline int      Size(int c)    { return *(const uint8_t*)(kRecords + c * 32 + 5); }
inline int      Economy(int c) { return *(const int*)(kRecords + c * 32 + 0xC); }

// A colonial city of `crown` the game itself would accept as a sign-up city
// (FUN_0046FE30, 0x004706F5..0x0047073D): large, prosperous, not ruined.
// Falls back to any intact one of that crown; -1 if there is none.
inline int SignUpCityFor(int crown)
{
    int good[kColonial], ok[kColonial], ng = 0, no = 0;
    for (int c = 0; c < kColonial; ++c) {
        if (Nation(c) != crown || (Flags(c) & 0xC0000000u)) continue;
        ok[no++] = c;
        if (Size(c) >= 5 && Economy(c) >= 0x32) good[ng++] = c;
    }
    if (ng) return good[rand() % ng];
    if (no) return ok[rand() % no];
    return -1;
}

// A pirate haven to begin in: an intact haven slot with a harbour direction
// the position code can use. -1 if the world has none.
inline int PickHaven()
{
    int found[kSlots], n = 0;
    for (int c = kColonial; c < kSlots; ++c) {
        if (Nation(c) != kPirate || (Flags(c) & 0xC0000000u)) continue;
        const short dir = *(const short*)(kHarbourDir + c * 0x94);
        if (dir < 0 || dir >= 24) continue;
        const int* pos = (const int*)(game::addr::CityPositions + c * 16);
        if (pos[0] == 0 && pos[1] == 0) continue;
        found[n++] = c;
    }
    return n ? found[rand() % n] : -1;
}

} // namespace piratestart

extern "C" {

int g_pemfStartCrown = 0;   // the crown the starting-stats row is read with

// 1. Before the tavern loop: choose the sign-up city ourselves. `tableCrown`
// points at local_154[0], the crown of the first table, which selects the walk.
void __cdecl PemfTavernSignUp(int* tableCrown)
{
    using namespace piratestart;
    __try {
        if (!suspicion::g_tune.startUnaligned) return;
        if (*(const int*)kSignUpCity != -1) return;
        const int crown = rand() % 4;
        const int city = SignUpCityFor(crown);
        if (city < 0) { Log("piratestart: no sign-up city for crown %d -- the tavern choice stays", crown); return; }
        *(int*)kSignUpCity = city;
        *tableCrown = kPirate;   // past the switch: no walk to a crown's table
        Log("piratestart: no crown chosen at the tavern (stats from %s, sign-up city %d)",
            game::NationName(crown), city);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// cmp [0x876BE0], edi is redone after the call, so the jne that follows sees
// exactly the flags it would have.
__declspec(naked) void PemfTavernStub(void)
{
    __asm {
        pushad
        lea  eax, [esp + 0x20 + 4 + 0x70]   // past pushad and our return: local_154[0]
        push eax
        call PemfTavernSignUp
        add  esp, 4
        popad
        cmp  dword ptr ds:[0x00876BE0], edi
        ret
    }
}

// 2. The start city is chosen; EDI holds it. Keep its crown for the stats row,
// then move the start to a haven.
int __cdecl PemfStartCity(int city)
{
    using namespace piratestart;
    __try {
        g_pemfStartCrown = (city >= 0 && city < kColonial) ? Nation(city) : 0;
        if (g_pemfStartCrown < 0 || g_pemfStartCrown > 3) g_pemfStartCrown = 0;
        if (!suspicion::g_tune.startUnaligned) return city;
        const int haven = PickHaven();
        if (haven < 0) { Log("piratestart: no pirate haven in the world yet -- starting at city %d", city); return city; }
        Log("piratestart: career begins at pirate haven %d (the game picked city %d, %s)",
            haven, city, game::NationName(g_pemfStartCrown));
        return haven;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return city; }
}

__declspec(naked) void PemfStartStub(void)
{
    __asm {
        push eax
        push edx
        push edi
        call PemfStartCity
        add  esp, 4
        mov  edi, eax
        pop  edx
        pop  eax
        mov  ecx, 8          // the instruction we replaced
        ret
    }
}

// 3. mov cl, [esi + 0x860B74], but always a crown.
__declspec(naked) void PemfStatsStub(void)
{
    __asm {
        mov  cl, byte ptr [g_pemfStartCrown]
        ret
    }
}

} // extern "C"

namespace piratestart {

inline bool WriteCall(uintptr_t at, void* fn, size_t len)
{
    uint8_t b[8];
    b[0] = 0xE8;
    *(int32_t*)(b + 1) = (int32_t)((uintptr_t)fn - (at + 5));
    for (size_t i = 5; i < len; ++i) b[i] = 0x90;
    DWORD old = 0;
    if (!VirtualProtect((void*)at, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)at, b, len);
    VirtualProtect((void*)at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, len);
    return true;
}

inline void WriteBack(uintptr_t at, const uint8_t* bytes, size_t len)
{
    DWORD old = 0;
    if (!VirtualProtect((void*)at, len, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy((void*)at, bytes, len);
    VirtualProtect((void*)at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, len);
}

// ⛔ First safe point only (a code write). All three sites or none.
inline bool Install()
{
    PatchOutfit();
    if (g_installed) return true;
    if (!suspicion::g_tune.startUnaligned) {
        Log("piratestart: off (startUnaligned=0) -- character creation as shipped");
        return false;
    }
    __try {
        if (memcmp((const void*)kTavernSite, kTavernBytes, sizeof(kTavernBytes)) ||
            memcmp((const void*)kStartSite, kStartBytes, sizeof(kStartBytes)) ||
            memcmp((const void*)kStatsSite, kStatsBytes, sizeof(kStatsBytes))) {
            Log("piratestart: career-start code is not what we expect -- character creation as shipped");
            return false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

    if (!WriteCall(kTavernSite, (void*)&PemfTavernStub, 6)) return false;
    if (!WriteCall(kStartSite, (void*)&PemfStartStub, 5) ||
        !WriteCall(kStatsSite, (void*)&PemfStatsStub, 6)) {
        WriteBack(kTavernSite, kTavernBytes, 6);
        WriteBack(kStartSite, kStartBytes, sizeof(kStartBytes));
        return false;
    }
    g_installed = true;
    Log("piratestart: installed -- no crown at the tavern, careers begin in a pirate haven");
    return true;
}

inline void Restore()
{
    if (g_outfitPatched) {
        DWORD old = 0;
        if (VirtualProtect((void*)(kOutfitLetters + 4), 1, PAGE_EXECUTE_READWRITE, &old)) {
            *(uint8_t*)(kOutfitLetters + 4) = 0;
            VirtualProtect((void*)(kOutfitLetters + 4), 1, old, &old);
        }
        g_outfitPatched = false;
    }
    if (!g_installed) return;
    WriteBack(kTavernSite, kTavernBytes, 6);
    WriteBack(kStartSite, kStartBytes, sizeof(kStartBytes));
    WriteBack(kStatsSite, kStatsBytes, sizeof(kStatsBytes));
    g_installed = false;
}

} // namespace piratestart
