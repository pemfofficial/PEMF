// flagtrader.h - the man at the back of the tavern sometimes has a flag too.
//
// A crown's colours are earned with its letter of marque (core.cpp, CanFly) --
// or bought, here, from the trinket seller, for a price. His own wares are left
// exactly as they are: the flag is an EXTRA offer, never a replacement.
//
// ============================================================================
// HOW HIS OFFER WORKS (decompiled, FUN_00406F60 the tavern, 0x00409650-0x00409B78)
// ============================================================================
// He picks a random item (EDI = 0..16), checks the owned/level byte at
// 0x00869B20[item], prices it (EBP), and composes the offer through AddText into
// the message object (0x008E9F58). Then:
//
//     0x00409A94  call WrapText(0x30, 0)
//     0x00409AB0  call 0x0042F7D0      ; the item's picture
//     0x00409AEF  call 0x00430190      ; ShowDialogDirect     (E8 9C 66 02 00)
//     0x00409AF7  test eax, eax / jne -> declined path (0x00409B4C)
//     0x00409B27  sub [0x00869AB4], ebp ; bought: the gold goes
//
// With options on the card ShowDialogDirect returns the option picked; his are
// "buy" (0) and "no" (1).
//
// PEMF redirects the presentation call and nothing else. His card is shown and
// answered first, untouched; then, some of the time, PEMF shows a second card
// offering a flag and settles that purchase itself. His answer is handed back
// unchanged, so whatever he sold is sold exactly as the game does it. If he has
// just sold something, its price (EBP at the call) is counted against the purse
// before the flag is offered, because the game takes it a moment later.
#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "log.h"
#include "game.h"
#include "nations.h"
#include "session.h"
#include "render.h"   // RedirectCall
#include "suspicion.h"

namespace flagtrader {

constexpr uintptr_t kPresentCall = 0x00409AEF;   // E8 -> kPresent
constexpr uintptr_t kPresent     = 0x00430190;   // ShowDialogDirect
constexpr uintptr_t kGold        = 0x00869AB4;   // int32

struct Settings {
    int chance = 60;     // percent of visits on which he also has a flag
    int price  = 1000;   // gold
};
inline Settings g_set;
inline bool g_installed = false;

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
            if      (_stricmp(key, "flagOfferChance") == 0) g_set.chance = v < 0 ? 0 : (v > 100 ? 100 : v);
            else if (_stricmp(key, "flagPrice")       == 0) g_set.price  = v < 0 ? 0 : v;
        }
        fclose(f);
    }
    Log("flagtrader: the tavern trader also has a flag %d%% of the time, for %d gold",
        g_set.chance, g_set.price);
}

// A crown's flag the captain does not have, chosen at random; -1 if none.
inline int PickUnowned()
{
    int choices[4], n = 0;
    for (int c = 0; c < game::addr::kNationsWithRank; ++c) {
        if (c == nations::HomeNation()) continue;
        if (session::OwnsFlag(c) || nations::Rank(c) > 0) continue;
        choices[n++] = c;
    }
    return n ? choices[rand() % n] : -1;
}

// After his own card: the flag, perhaps. `spent` is what he is about to take.
inline void OfferFlag(game::ShowDialog_t present, int a0, int a1, int a2, int a3, int a4,
                      int a5, int a6, int a7, int a8, int a9, int spent)
{
    // Every visit is logged with the reason, so "he never offered" can be
    // answered from the log: no flag left to sell, or the roll.
    if (!session::InCareer() || g_set.chance <= 0) {
        Log("flagtrader: visit -- no offer (in career %d, chance %d)", session::InCareer() ? 1 : 0, g_set.chance);
        return;
    }
    const int nation = PickUnowned();
    if (nation < 0) { Log("flagtrader: visit -- every crown's flag is already ours"); return; }
    const int roll = rand() % 100;
    if (roll >= g_set.chance) {
        Log("flagtrader: visit -- no flag this time (rolled %d, needs < %d)", roll, g_set.chance);
        return;
    }

    const int gold = *(const int*)kGold - spent;
    const bool afford = gold >= g_set.price;

    char body[384];
    _snprintf_s(body, sizeof(body), _TRUNCATE,
        "As you rise to go he catches your sleeve and draws a folded bundle of "
        "bunting from under his coat. 'One more thing, Captain. Not every man "
        "cares to be known for who he is. A %s ensign, good as the King's own, "
        "for %d gold.'\n", game::NationName(nation), g_set.price);

    game::ResetMessage();
    game::AddText(body);
    if (afford) {
        game::AddTextOption(" Buy it.\n");
        game::AddTextOption(" Not today.\n");
    } else {
        game::AddTextOption(" I haven't the gold.\n");
    }
    game::WrapText(0x30, 0);
    Log("flagtrader: also offering the %s flag for %d (gold after his sale %d)",
        game::NationName(nation), g_set.price, gold);

    const int r = present(a0, a1, (const char*)a2, a3, a4, a5, a6, a7, a8, a9);
    if (!afford || r != 0) return;

    *(int*)kGold -= g_set.price;
    session::GrantFlag(nation);
    Log("flagtrader: bought the %s flag for %d", game::NationName(nation), g_set.price);
    char msg[160];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                "The %s colours are in the flag locker.", game::NationName(nation));
    suspicion::Say(msg, true);
}

} // namespace flagtrader

extern "C" {

void* g_pemfPresentOrig = nullptr;
int   g_pemfTraderPrice = 0;      // EBP at the call: the price of his item

// Same contract as ShowDialogDirect: cdecl, ten arguments, result in EAX.
int __cdecl PemfTraderPresent(int a0, int a1, int a2, int a3, int a4,
                              int a5, int a6, int a7, int a8, int a9)
{
    using namespace flagtrader;
    const auto orig = (game::ShowDialog_t)g_pemfPresentOrig;
    const int price = g_pemfTraderPrice;

    // His card, exactly as the game shows it, and his answer kept for the game.
    const int r = orig(a0, a1, (const char*)a2, a3, a4, a5, a6, a7, a8, a9);

    // Bought it (option 0 with the gold to pay), so the game is about to take
    // `price`; anything else costs nothing.
    int spent = 0;
    __try {
        if (r == 0 && *(const int*)kGold >= price) spent = price;
        OfferFlag(orig, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, spent);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("flagtrader: fault while offering a flag -- his sale is unaffected");
    }
    return r;
}

// Record his price (EBP), then become the call above: the ten arguments and
// the return address are exactly where a cdecl callee expects them.
__declspec(naked) void PemfTraderPresentStub(void)
{
    __asm {
        mov  dword ptr [g_pemfTraderPrice], ebp
        jmp  PemfTraderPresent
    }
}

} // extern "C"

namespace flagtrader {

// ⛔ First safe point only (a code write).
inline bool Install()
{
    if (g_installed) return true;
    __try {
        const uintptr_t s = kPresentCall;
        if (*(const uint8_t*)s != 0xE8 || s + 5 + *(const int32_t*)(s + 1) != kPresent) {
            Log("flagtrader: call at 0x%08X is not what we expect -- no flags for sale",
                (unsigned)s);
            return false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    void* prev = nullptr;
    if (!render::RedirectCall(kPresentCall, (void*)&PemfTraderPresentStub, &prev)) return false;
    g_pemfPresentOrig = prev;
    g_installed = true;
    Log("flagtrader: the tavern trader can also sell flags");
    return true;
}

inline void Restore()
{
    if (!g_installed) return;
    render::RedirectCall(kPresentCall, g_pemfPresentOrig, nullptr);
    g_installed = false;
}

} // namespace flagtrader
