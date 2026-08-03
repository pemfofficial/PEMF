// stormaudio.h - a music bed that comes up when the weather does.
//
// Plays a looping track while the player is in a storm and fades it away as
// they sail clear. The "am I in a storm" question is already answered by the
// engine: FUN_0045FA70 returns a weather intensity that rises as you close, and
// storms.h calls it for the cargo loss. This reuses the same number, so the
// music, the rain and the cargo all agree about what counts as bad weather.
//
// Playback is audiomix.h -- our own XAudio2 mixer -- because MCI cannot change
// the volume of what it is playing and the game's own Miles path for new clips
// is unfinished. The full reasoning is at the top of audiomix.h; the short
// version is that a music bed without a fade is worse than no music bed.
//
// TWO THRESHOLDS, not one: a player sitting on the edge of a storm should not
// get the track switching on and off every few seconds, and since weather is a
// gradient with no hard edge, that boundary is wide.
#pragma once
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "game.h"
#include "audiomix.h"

namespace stormaudio {

struct Tuning {
    int  enabled     = 1;
    int  volume      = 800;    // 0..1000, the level it settles at
    int  startAt     = 8;      // engine weather intensity that brings it in
    int  stopAt      = 5;      // ...and the lower one that takes it away
    int  fadeMs      = 2500;
    int  settleMs    = 15000;  // overworld must be up this long first
    int  duckGameMusic = 1;    // silence the ship's own theme while ours plays
    char file[128]   = "storm.mp3";
};

inline Tuning g_tune;

// ------------------------------------------------- ducking the game's music
// PEMF's storm track is on its own mixer, so the game's sailing theme happily
// plays over the top of it. The game keeps its four volume settings in globals
// and copies them into the live channel array when its options screen applies
// them:
//
//   FUN_004D4480 (void):
//       channels[0] = 0x00726388     (sound)
//       channels[1] = 0x0072638C     <-- MUSIC
//       channels[2] = 0x00726390
//       channels[3] = 0x00726394
//
// The key names are right beside them in the settings writer at 0x004D260F,
// which is how "MusicVolume" was matched to 0x0072638C rather than guessed.
//
// So ducking is: remember the player's setting, write 0, call the apply. And
// restoring is the same in reverse -- which makes it ESSENTIAL that we never
// capture our own zero as "the player's setting", or a storm would leave the
// game permanently silent.
namespace duck {
    constexpr uintptr_t MusicVolume = 0x0072638C;
    constexpr uintptr_t ApplyFn     = 0x004D4480;
    constexpr unsigned char kApplySig[] = {
        0x8B, 0x0D, 0xC0, 0xD4, 0x8E, 0x00, 0x33, 0xC0
    };
    typedef void (__cdecl *Apply_t)(void);

    inline bool g_ducked  = false;
    inline int  g_saved   = 0;

    inline bool Callable()
    {
        return game::BytesMatch(ApplyFn, kApplySig, sizeof(kApplySig));
    }

    inline void Set(bool on)
    {
        if (on == g_ducked) return;
        if (!Callable()) return;

        __try {
            if (on) {
                g_saved = *(const int*)MusicVolume;
                if (g_saved <= 0) return;     // already silent; leave it alone
                *(int*)MusicVolume = 0;
            } else {
                *(int*)MusicVolume = g_saved;
            }
            ((Apply_t)ApplyFn)();
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }

        g_ducked = on;
        Log("storm audio: the ship's music %s (game MusicVolume %d)",
            on ? "falls away" : "returns", on ? 0 : g_saved);
    }
}

inline bool  g_loaded   = false;
inline bool  g_failed   = false;
inline int   g_level    = 0;      // current volume, 0..1000
inline DWORD g_lastTick = 0;
inline char  g_path[MAX_PATH] = {0};
inline audiomix::Track g_track;

inline int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline void Configure(const char* gameDir)
{
    _snprintf_s(g_path, sizeof(g_path), _TRUNCATE, "%s\\PEMF\\audio\\%s",
                gameDir, g_tune.file);
}

inline bool Load()
{
    if (g_loaded) return true;
    if (g_failed) return false;
    if (!g_path[0]) return false;

    if (GetFileAttributesA(g_path) == INVALID_FILE_ATTRIBUTES) {
        Log("storm audio: no %s -- storms stay quiet", g_path);
        g_failed = true;                  // do not retry every frame
        return false;
    }
    if (!audiomix::Load(g_track, g_path)) {
        Log("storm audio: could not decode the track -- storms stay quiet");
        g_failed = true;
        return false;
    }
    g_loaded = true;
    return true;
}

// `intensity` is the engine's own weather value; `sailing` gates the whole
// thing so the track never plays over a menu or a town.
inline void Tick(int intensity, bool sailing)
{
    if (!g_tune.enabled || g_failed) return;

    const DWORD now = GetTickCount();
    const int dt = g_lastTick ? (int)(now - g_lastTick) : 0;
    g_lastTick = now;

    // A career that begins beside a storm should not OPEN with the storm
    // track -- it reads as the mod playing music at you rather than the
    // weather having a sound. So the overworld has to have been up for a
    // little while before the music is allowed in at all.
    static DWORD s_sailingSince = 0;
    if (!sailing) s_sailingSince = 0;
    else if (s_sailingSince == 0) s_sailingSince = now;
    const bool settled = sailing && s_sailingSince != 0 &&
                         (int)(now - s_sailingSince) >= g_tune.settleMs;

    const bool want = settled && intensity >= g_tune.startAt;
    const bool keep = sailing && intensity >= g_tune.stopAt && g_track.playing;
    const int target = (want || keep) ? Clamp(g_tune.volume, 0, 1000) : 0;

    // Say what is being seen, until the music has actually started once. Two
    // rounds have now been lost to this returning early in silence -- first
    // because the caller passed sailing unconditionally, then because a
    // lowered weatherPower put the intensity ceiling below the threshold. A
    // gate that declines without saying so is indistinguishable from a bug.
    static DWORD s_lastSay = 0;
    static bool  s_everPlayed = false;
    if (!s_everPlayed && (s_lastSay == 0 || now - s_lastSay > 5000)) {
        s_lastSay = now;
        Log("storm audio: waiting -- sailing %d, intensity %d, need %d "
            "(loaded %d)", sailing ? 1 : 0, intensity, g_tune.startAt,
            g_loaded ? 1 : 0);
    }
    if (target > 0) s_everPlayed = true;

    if (target > 0 && !g_loaded && !Load()) return;
    if (!g_loaded) return;

    // Ramp rather than jump. A storm arriving with a hard cut sounds like a
    // bug; arriving over a couple of seconds sounds like weather.
    // ⚠️ dt == 0 MUST MEAN "DO NOTHING", NOT "SNAP".
    //
    // GetTickCount has ~15.6 ms granularity, so two calls inside one tick give
    // a delta of zero -- and the old `else { g_level = target; }` treated that
    // as "no fade configured" and jumped the whole way. A single zero-delta
    // call anywhere in the ramp collapsed a 4-second fade into one frame,
    // which is why it was measured finishing in 140 ms and 0 ms. The ramp was
    // never wrong; one branch of it was.
    if (g_tune.fadeMs > 0) {
        if (dt > 0) {
            // Clamp the delta as well, so a frame hitch cannot cover the range
            // in one go either.
            const int dtc  = dt > 50 ? 50 : dt;
            int step = 1000 * dtc / g_tune.fadeMs;
            if (step < 1) step = 1;            // never stall
            if (g_level < target)
                g_level = (g_level + step > target) ? target : g_level + step;
            else if (g_level > target)
                g_level = (g_level - step < target) ? target : g_level - step;
        }
        // dt == 0: leave the level exactly where it is and wait for real time.
    } else {
        g_level = target;                      // fades switched off
    }

    // Prove the ramp rather than assume it. This has now been "fixed" twice by
    // adjusting a number, and a fade that is not happening looks exactly like a
    // fade that is too fast.
    static int   s_lastLogged = -1;
    static DWORD s_rampBegan  = 0;
    if (g_level != target && s_rampBegan == 0) s_rampBegan = now;
    if (g_level == target && s_rampBegan != 0) {
        Log("storm audio: ramp to %d finished in %u ms (fadeMs %d)",
            target, (unsigned)(now - s_rampBegan), g_tune.fadeMs);
        s_rampBegan = 0;
    }
    if (s_rampBegan && (s_lastLogged < 0 || abs(g_level - s_lastLogged) >= 150)) {
        s_lastLogged = g_level;
        Log("storm audio: level %d -> %d (dt %d)", g_level, target, dt);
    }

    if (g_level > 0) {
        if (!g_track.playing) {
            audiomix::SetLevel(g_track, 0.0f);   // always come UP from silence
            if (audiomix::Play(g_track)) Log("storm audio: in");
        }
        audiomix::SetLevel(g_track, (float)g_level / 1000.0f);
        // Duck once we are past the toe of the fade, so the ship's theme goes
        // out as ours comes in rather than both being audible at full tilt.
        if (g_tune.duckGameMusic && g_level > 40) duck::Set(true);
    } else if (g_track.playing) {
        audiomix::Stop(g_track);
        duck::Set(false);
        Log("storm audio: out");
    }
}

// Career change, or leaving the sailing view for good.
inline void Silence()
{
    if (g_track.playing) audiomix::Stop(g_track);
    duck::Set(false);          // never leave the game silent
    g_level = 0;
}

inline void Shutdown()
{
    duck::Set(false);          // restore the player's setting on the way out
    audiomix::Release(g_track);
    audiomix::Shutdown();
    g_loaded = false;
}

} // namespace stormaudio
