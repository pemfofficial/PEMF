// gameaudio.h - the game's own volume settings, and the one place that ducks
// its music. Shared by every PEMF track, because two tracks each ducking on
// their own would race and one of them would lose the player's setting.
//
// ============================================================================
// THEY ARE FLOATS, 0.0 TO 1.0 -- NOT INTEGERS
// ============================================================================
// Settled from the settings writer FUN_004D2170, which pairs each global with
// the key it writes and passes them all through the FLOAT writer (FUN_004F9AC0,
// the same one Gamma uses) rather than the integer one:
//
//     0x00726388   MasterVolume
//     0x0072638C   MusicVolume
//     0x00726390   SFXVolume
//     0x00726394   DanceVolume
//
// ⚠️ AND A CORRECTION: stormaudio.h described 0x00726388 as "sound". It is
// MasterVolume. SFX is 0x00726390. The duck only ever touched MusicVolume so
// nothing was actually mis-set, but anything reading the others from that note
// would have had the wrong one.
//
// The old duck read and wrote these through `int*`. That worked, but only by
// accident: it never did arithmetic, it copied the bit pattern out and put the
// same bits back, and `if (saved <= 0) return` happens to be true for +0.0f
// because its bits are zero. The moment anything wanted to SCALE by the
// player's setting -- which is the whole point of this file -- the integer view
// would have read 1056964608 for a half-volume slider.
//
// ============================================================================
// ⛔ THE DUCK MUST NEVER OUTLIVE US
// ============================================================================
// The game writes Config.ini from these globals on exit. Duck the music, have
// the process end before the duck is lifted, and the player's MusicVolume is
// saved as 0 -- their game is permanently silent and nothing in it explains
// why. Reinstalling PEMF would not fix it; the setting is in their documents.
//
// This was not hypothetical. Found in the dev box's own Config.ini while
// working on this file:
//
//     MasterVolume = 0.263158
//     MusicVolume  = 0.000000     <- ours
//     SFXVolume    = 1.000000
//     DanceVolume  = 1.000000
//
// Music alone at zero, with everything else untouched, is our fingerprint --
// nobody sets exactly that by hand. stormaudio.h's own comment predicted this
// exact failure and the code still had no path that guaranteed the restore.
//
// So: Restore() is idempotent, safe to call from anywhere, and is called on the
// way out whatever the reason -- unload, shutdown, career end, focus loss.
#pragma once
#include <windows.h>

#include "log.h"
#include "game.h"

namespace gameaudio {

constexpr uintptr_t MasterVolume = 0x00726388;   // float 0..1
constexpr uintptr_t MusicVolume  = 0x0072638C;
constexpr uintptr_t SFXVolume    = 0x00726390;
constexpr uintptr_t DanceVolume  = 0x00726394;

// Copies the four globals into the live channel array. The settings screen
// calls it when the player moves a slider; we call it so a duck takes effect.
constexpr uintptr_t ApplyFn = 0x004D4480;
constexpr unsigned char kApplySig[] = {
    0x8B, 0x0D, 0xC0, 0xD4, 0x8E, 0x00, 0x33, 0xC0
};
typedef void (__cdecl *Apply_t)(void);

inline bool  g_ducked     = false;
inline float g_savedMusic = 0.0f;

inline bool Callable()
{
    return game::BytesMatch(ApplyFn, kApplySig, sizeof(kApplySig));
}

inline float ReadF(uintptr_t addr)
{
    __try { return *(const float*)addr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0f; }
}

inline float Master() { return ReadF(MasterVolume); }

// The player's music setting as they last chose it -- the SAVED one while we
// are ducking, because the live global is our zero at that point. Anything
// scaling itself by "how loud does this player want music" wants this.
inline float Music() { return g_ducked ? g_savedMusic : ReadF(MusicVolume); }

// What a PEMF music track should multiply its own level by, so our tracks obey
// the same two sliders the game's own music does. Clamped because these are
// read out of the game's memory and a garbage read should go quiet, not loud.
inline float MusicMix()
{
    float m = Master() * Music();
    if (!(m >= 0.0f)) m = 0.0f;      // also catches NaN
    if (m > 1.0f)     m = 1.0f;
    return m;
}

inline void Duck(bool on)
{
    if (on == g_ducked) return;
    if (!Callable()) return;

    __try {
        if (on) {
            const float cur = *(const float*)MusicVolume;
            // Never capture our own silence as "the player's setting". If the
            // music is already off, there is nothing to duck and nothing to
            // restore, and pretending otherwise is how the setting gets lost.
            if (!(cur > 0.0f)) return;
            g_savedMusic = cur;
            *(float*)MusicVolume = 0.0f;
        } else {
            *(float*)MusicVolume = g_savedMusic;
        }
        ((Apply_t)ApplyFn)();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    g_ducked = on;
    Log("game audio: the ship's music %s (MusicVolume %.3f)",
        on ? "falls away" : "returns", on ? 0.0f : g_savedMusic);
}

// Idempotent, and deliberately blunt. Call it on any way out.
inline void Restore()
{
    if (!g_ducked) return;
    Duck(false);
}

} // namespace gameaudio
