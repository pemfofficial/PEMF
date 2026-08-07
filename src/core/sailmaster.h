// sailmaster.h - the sailing master's "far out to sea" card.
//
// ============================================================================
// THE REPORT
// ============================================================================
// "We're far out to sea captain, shall I set a course for..." comes up while
// the ship is sitting beside a port. Players say it only ever did this near the
// edges of the map in the stock game.
//
// ============================================================================
// WHAT THE GAME ACTUALLY DOES -- read from the binary, twice, after two wrong
// readings of mine
// ============================================================================
// It is not a map-bounds check. The sailing update keeps a timer:
//
//     00473749  INC  [0x0085A154]                  ; sailing tick
//     0047375A  MOV  ECX, [0x0085A154]
//     0047374F  MOV  EAX, [0x0085A158]
//     00473761  IMUL EAX, EAX, 0x1F4               ; (B + 1) * 500
//     00473767  ADD  EAX, [0x008B98D4]             ; + next-prompt-due
//     00473769  CMP  ECX, EAX
//     0047376D  CALL 0x0045B890                    ; -> the card
//
// and FUN_0045B890 decides whether to speak:
//
//     0045B8D0  CALL 0x0045FEE0    ; nearest city, ANY nation, within 9999
//     0045B8D8  XOR  ECX, ECX      ; the player's ship, explicitly
//     0045B8DA  CALL 0x00405D90    ; distance to it
//     0045B8DF  CMP  EAX, 0xC
//     0045B8E2  JL   <stay quiet>  ; under 12 -> say nothing
//
// It then re-arms itself: +250 ticks if it stayed quiet, +99999 if it spoke.
//
// ⚠️ THE ENGINE CODE IS WELL FORMED. Both of FUN_00405D90's implicit arguments
// are set deliberately -- EAX by the search that just returned the city index,
// ECX zeroed for the player's ship. I claimed twice that it was fragile
// (leftover EAX, then leftover ECX) and both claims were wrong. It is ordinary
// compiler output.
//
// ⚠️ AND PEMF WRITES NONE OF ITS INPUTS. Checked individually: 0x008B98D4
// (next-prompt-due), 0x0085A154 (tick), 0x0085A158 (interval scale, read only
// as morale's term B), 0x0085A164 (view flags, read only). The flag-mesh arrays
// that end one dword below the city table are declared in game.h and never
// used. The ship factory never writes slot 0. There is no corruption route left
// that I can find, so this file stops looking for one and measures instead.
//
// ============================================================================
// WHAT THIS DOES
// ============================================================================
// Redirects the one CALL at 0x0047376D -- the same technique as the storm draw
// and the town menu -- and before letting the card through, asks PEMF's own
// city lookup whether a settlement is in fact close by.
//
// The two disagreeing IS the bug, so both numbers go in the log every time.
// That is the measurement the last several rounds of argument needed and did
// not have.
//
// ⚠️ THE SUPPRESSION IS A SYMPTOM FIX AND IS LABELLED AS ONE. It stops the card
// appearing when a settlement is demonstrably nearby; it does not explain why
// the engine thought otherwise. If the log shows the two agreeing, then the
// card is stock behaviour, the nearby settlement is a village or mission (the
// engine's search covers the five NATIONS only, so villages and missions are
// invisible to it), and this file should be deleted rather than kept.
#pragma once
#include <windows.h>

#include "log.h"
#include "game.h"
#include "render.h"

namespace sailmaster {

constexpr uintptr_t kCallSite     = 0x0047376D;   // CALL 0x0045B890
constexpr uintptr_t kCallTarget   = 0x0045B890;
constexpr uintptr_t kCityDistance = 0x00405D90;   // __fastcall, ecx=ship eax=city
constexpr uintptr_t kTick         = 0x0085A154;
constexpr uintptr_t kNextPrompt   = 0x008B98D4;
constexpr uintptr_t kIntervalB    = 0x0085A158;

// The engine's own threshold: under 12 it stays quiet. Ours has to be the same
// number or the log is comparing two different questions.
constexpr int kEngineQuietUnder = 12;

// How close a settlement has to be, in the units game::CityDistance reports --
// the same ones the world sample logs as `dist=`. A port approach measures
// around 1000 and being tied up alongside under 1500, so 3000 is "there is
// clearly a settlement right here" without being so wide it silences the card
// in genuinely open water.
inline int  g_nearRadius = 3000;

inline void* g_orig      = nullptr;
inline bool  g_installed = false;
inline bool  g_suppress  = false;
inline long  g_seen      = 0;
inline long  g_stopped   = 0;

// The engine's distance, called exactly as the card calls it: ship 0 (player),
// and the city index in EAX. Pure -- it reads two arrays and writes nothing --
// so calling it ourselves has no side effect on the game.
__declspec(naked) inline int EngineDistance(int /*ship*/, int /*city*/)
{
    __asm {
        mov  ecx, dword ptr [esp + 4]
        mov  eax, dword ptr [esp + 8]
        call kCityDistance
        ret
    }
}

extern "C" void __cdecl PemfSailMasterDecide()
{
    g_suppress = false;
    ++g_seen;

    __try {
        const int city = game::NearestCity(content::kCityNameScanRadius);
        if (city < 0) return;                       // nothing near: let it speak

        const int ours   = game::CityDistance(city);
        const int theirs = EngineDistance(0, city);

        // The engine would stay quiet anyway -- nothing to say and nothing to
        // suppress. Not logged: this is the common case, every 750 ticks.
        if (theirs < kEngineQuietUnder) return;

        if (ours <= g_nearRadius) {
            g_suppress = true;
            ++g_stopped;
            Log("sailmaster: card SUPPRESSED -- city %d is %d away by our "
                "reckoning but the engine makes it %d (>= %d, so it would have "
                "called us far out to sea)",
                city, ours, theirs, kEngineQuietUnder);
        } else {
            Log("sailmaster: card allowed -- nearest city %d is %d away (engine "
                "%d). Both agree we are at sea.", city, ours, theirs);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_suppress = false;                          // never eat the card on a fault
    }
}

// Re-arm the timer the way the engine does when it decides to stay quiet, so a
// suppressed card does not simply fire again on the very next tick -- which
// would put this shim in the pump thousands of times a second.
extern "C" void __cdecl PemfSailMasterRearm()
{
    __try {
        *(int*)kNextPrompt = *(const int*)kTick + 0xFA;   // +250, as at 0045B89x
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// FUN_0045B890 takes no arguments and returns nothing, so the shim only has to
// decide whether to tail-call it.
__declspec(naked) inline void SailMasterShim()
{
    __asm {
        pushad
        call PemfSailMasterDecide
        popad

        cmp  byte ptr [g_suppress], 0
        jne  suppressed
        jmp  dword ptr [g_orig]        // tail call: it returns to our caller

    suppressed:
        pushad
        call PemfSailMasterRearm
        popad
        ret
    }
}

// ⛔ CODE WRITE -- ONLY FROM THE SAFE POINT, like storms and the town menu.
inline bool Install()
{
    if (g_installed) return true;

    if (!render::RedirectCall(kCallSite, (void*)&SailMasterShim, &g_orig)) {
        Log("sailmaster: no call rel32 at 0x%08X -- not installed",
            (unsigned)kCallSite);
        return false;
    }
    if ((uintptr_t)g_orig != kCallTarget) {
        Log("sailmaster: call at 0x%08X targets 0x%p, expected 0x%08X -- reverting",
            (unsigned)kCallSite, g_orig, (unsigned)kCallTarget);
        render::RedirectCall(kCallSite, g_orig, nullptr);
        g_orig = nullptr;
        return false;
    }

    g_installed = true;
    Log("sailmaster: hooked 0x%08X -- the 'far out to sea' card is checked "
        "against our own city lookup before it shows (near = %d)",
        (unsigned)kCallSite, g_nearRadius);
    return true;
}

inline void Restore()
{
    if (!g_installed || !g_orig) return;
    render::RedirectCall(kCallSite, g_orig, nullptr);
    g_installed = false;
    Log("sailmaster: restored (%ld check(s), %ld suppressed)", g_seen, g_stopped);
}

} // namespace sailmaster
