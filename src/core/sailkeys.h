// sailkeys.h - stop the sailing view's hard-coded save/load keys from firing
// under WASD steering.
//
// ============================================================================
// THE BUG THIS EXISTS FOR
// ============================================================================
// Reported as: "after playing a while the game teleports you to a port, says a
// game was loaded, and throws you in the town menu -- and it keeps doing it."
//
// It is PEMF's fault, and it has been since WASD shipped.
//
// The sailing update (FUN_00471e00) dispatches some keys as RAW VIRTUAL KEYS,
// through a jump table, in code -- NOT through KeyMap.ini:
//
//     00472d74  ADD   EBX, -0x9                  ; EBX = virtual key
//     00472d77  CMP   EBX, 0x69
//     00472d7a  JA    default
//     00472d80  MOVZX ECX, byte ptr [EBX + 0x473968]   ; index table
//     00472d87  JMP   dword ptr [ECX*4 + 0x473934]     ; jump table
//
// Three of those entries are save/load, and the pushed string names each one:
//
//     'A' (0x41) -> slot 5 -> 0x00472FF7 -> FUN_004022e0("arrival")   LOAD
//     'L' (0x4C) -> slot 6 -> 0x00472FF0 -> FUN_004022e0("quick")     LOAD
//     'S' (0x53) -> slot 8 -> 0x00472FCA -> FUN_00401ee0("quick")     SAVE
//
// PEMF's WASD keymap binds 'A' to TurnLeft and 'S' to ReefedSails. So steering
// left ALSO loads the "arrival" save -- the autosave the game writes when you
// reach a port. That is the whole report: you are teleported to that port, the
// game says "Game loaded.", you land in the town menu, and steering left again
// does it again.
//
// ⛔ MOVING QuickSave/QuickLoad IN KeyMap.ini DOES NOT FIX THIS AND NEVER DID.
// The ini has `QuickLoad_L = F9` and `QuickSave_S = F5`, which is why this went
// unexplained for so long -- the keys looked rebound. They were not: this table
// is in the executable and the ini has no say over it. The ini entries are for
// a different handler.
//
// 'W' and 'D' are not in the table at all, which is exactly why only two of the
// four steering keys ever misbehaved.
//
// ============================================================================
// THE FIX
// ============================================================================
// Two bytes. Point 'A' and 'S' at the table's own default slot (0x0C ->
// 0x00473731, which falls through and does nothing), so the sailing view stops
// treating them as save/load.
//
// 'L' IS LEFT ALONE, DELIBERATELY. It is not a steering key, it is the stock
// quickload, and a player who knows it should keep it. We are removing a
// collision we introduced, not editing the game's design.
//
// ⛔ THIS IS A CODE WRITE -- ONLY FROM THE FIRST SAFE POINT. Patching .text
// while the Steam build's DRM wrapper is still checksumming its own image gives
// "Application corrupt." and the game never starts. Same rule as storms.
//
// ⚠️ AND ONLY WHEN WASD IS ACTUALLY ACTIVE. A player who kept the stock layout
// still has 'A' meaning "load arrival" as the game intended, and it is not our
// business to take that away from them.
#pragma once
#include <windows.h>

#include "log.h"
#include "game.h"

namespace sailkeys {

// The byte table the sailing view indexes with (virtual key - 9).
constexpr uintptr_t kIndexTable  = 0x00473968;
constexpr int       kVkBias      = 9;
constexpr unsigned char kDefaultSlot = 0x0C;   // -> 0x00473731, does nothing

struct Steal {
    unsigned char vk;         // the virtual key
    const char*   name;       // for the log
    unsigned char expect;     // the slot it must currently hold
    const char*   does;       // what that slot does, for the log
};

// Verified by reading the table out of the binary, not assumed. If a byte does
// not hold what we expect, we leave it alone and say so -- on a build we do not
// recognise, shipping stock keys beats scribbling on a dispatch table.
inline Steal kSteals[] = {
    { 0x41, "A", 0x05, "load the 'arrival' save" },
    { 0x53, "S", 0x08, "quick-save" },
};
constexpr int kStealCount = (int)(sizeof(kSteals) / sizeof(kSteals[0]));

inline unsigned char g_saved[kStealCount] = {0};
inline bool          g_applied = false;

inline unsigned char* SlotFor(unsigned char vk)
{
    return (unsigned char*)(kIndexTable + (vk - kVkBias));
}

inline bool Apply()
{
    if (g_applied) return true;

    int done = 0;
    __try {
        for (int i = 0; i < kStealCount; ++i) {
            unsigned char* at = SlotFor(kSteals[i].vk);

            if (*at != kSteals[i].expect) {
                // Already ours, or a build we do not know. Either way, no write.
                Log("sailkeys: '%s' holds slot %u, expected %u -- left alone",
                    kSteals[i].name, (unsigned)*at, (unsigned)kSteals[i].expect);
                continue;
            }

            DWORD old = 0;
            if (!VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old)) {
                Log("sailkeys: could not unprotect the '%s' slot (err=%lu)",
                    kSteals[i].name, GetLastError());
                continue;
            }
            g_saved[i] = *at;
            *at = kDefaultSlot;
            VirtualProtect(at, 1, old, &old);
            FlushInstructionCache(GetCurrentProcess(), at, 1);
            ++done;
            Log("sailkeys: '%s' no longer tries to %s while sailing -- it is a "
                "steering key now", kSteals[i].name, kSteals[i].does);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("sailkeys: faulted while patching -- steering keys may still "
            "save/load");
        return false;
    }

    g_applied = (done > 0);
    if (g_applied)
        Log("sailkeys: %d steering key(s) freed (L still quick-loads, as it "
            "does in the stock game)", done);
    return g_applied;
}

inline void Restore()
{
    if (!g_applied) return;
    __try {
        for (int i = 0; i < kStealCount; ++i) {
            if (!g_saved[i]) continue;
            unsigned char* at = SlotFor(kSteals[i].vk);
            DWORD old = 0;
            if (VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old)) {
                *at = g_saved[i];
                VirtualProtect(at, 1, old, &old);
                FlushInstructionCache(GetCurrentProcess(), at, 1);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
    g_applied = false;
}

} // namespace sailkeys
