// battlemusic.h - PEMF's own track for a ship battle.
//
// THE RULE, and it is deliberately the whole of it:
//
//     our battle song plays IF AND ONLY IF a ship battle is on screen.
//
// Everywhere else -- the overworld, a boarding duel, the boarding menu, a town,
// the map -- our track is stopped and the game's own music is handed back. A
// boarding duel is NOT a ship battle for this purpose: it has its own music and
// the brief says to let it have it.
//
// Because the rule is an iff, there is no separate "stop" path to get wrong. It
// is one boolean, evaluated every safe point, and both edges are handled in the
// same place. The storm track's own bug -- fading out and then not coming back
// for minutes because the way back in had extra conditions the way out did not
// -- is exactly what that avoids.
//
// ⛔ THE STORM TRACK STOPS WHEN THIS STARTS. Two of our own tracks over each
// other is worse than either alone, and a battle is not weather. storms.h
// already freezes the WEATHER during a fight; this silences its SOUND.
//
// Volume follows the game's own Master and Music sliders through gameaudio.h,
// so a player who turned music down does not get ours at full blast.
#pragma once
#include <windows.h>

#include "log.h"
#include "game.h"
#include "audiomix.h"
#include "gameaudio.h"

namespace battlemusic {

struct Tuning {
    int  enabled = 1;
    int  volume  = 700;      // 0..1000, before the game's sliders scale it
    int  fadeMs  = 600;      // short: a battle starts abruptly and should sound it
    char file[128] = "battle_song_by_nastelborn.mp3";
};

inline Tuning g_tune;

inline bool  g_loaded = false;
inline bool  g_failed = false;
inline bool  g_on     = false;      // our belief about whether it should sound
inline bool  g_away   = false;
inline int   g_level  = 0;          // 0..1000
inline DWORD g_lastTick = 0;
inline char  g_path[MAX_PATH] = {0};
inline audiomix::Track g_track;

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
        Log("battle music: no %s -- battles keep the game's own music", g_path);
        g_failed = true;
        return false;
    }
    if (!audiomix::Load(g_track, g_path)) {
        Log("battle music: could not decode the track -- battles keep the "
            "game's own music");
        g_failed = true;
        return false;
    }
    Log("battle music: loaded %s", g_path);
    g_loaded = true;
    return true;
}

// The level actually written to the voice: our own tuning, scaled by the two
// game sliders. Kept in one place so the ramp and the alt-tab path cannot
// disagree about it.
inline float VoiceLevel()
{
    return ((float)g_level / 1000.0f) * gameaudio::MusicMix();
}

// Alt-tab. Same reasoning as the storm track: silence straight at the voice and
// leave g_level alone, so coming back resumes where the fight was rather than
// making it fade in again from nothing.
inline void SetAway(bool away)
{
    if (away == g_away) return;
    g_away = away;
    if (!g_loaded) return;
    audiomix::SetLevel(g_track, away ? 0.0f : VoiceLevel());
}

// Stop everything and give the game its music back. Safe to call repeatedly.
inline void Silence(const char* why)
{
    if (g_loaded && (g_on || g_level > 0)) {
        audiomix::SetLevel(g_track, 0.0f);
        audiomix::Stop(g_track);
        Log("battle music: out -- %s", why);
    }
    g_on = false;
    g_level = 0;
}

// `inBattle` is the only input. See triggers::LooksLikeBattle for how a ship
// battle is told apart from everything else -- it is the one screen whose
// signature sets the top bit.
inline void Tick(bool inBattle)
{
    if (!g_tune.enabled || g_failed) return;
    if (g_away) return;

    const DWORD now = GetTickCount();
    const int dt = g_lastTick ? (int)(now - g_lastTick) : 0;
    g_lastTick = now;

    if (inBattle && !g_on) {
        if (!Load()) return;
        // The game's own battle music would otherwise play underneath ours.
        gameaudio::Duck(true);
        g_level = 0;                       // always come up from silence
        audiomix::SetLevel(g_track, 0.0f);
        if (!audiomix::Play(g_track)) {
            Log("battle music: the voice would not start");
            g_failed = true;
            return;
        }
        g_on = true;
        Log("battle music: in -- a ship battle has started");
    } else if (!inBattle && g_on) {
        Silence("the ship battle is over");
        // Hand the music back immediately. Whatever comes next -- the
        // overworld, a boarding duel, a menu -- gets the game's own audio, per
        // the brief. If the player sails back into a storm, stormaudio ducks
        // again on its own terms.
        gameaudio::Restore();
        return;
    }

    if (!g_loaded || !g_on) return;

    // Ramp. dt == 0 means DO NOTHING, never "snap" -- GetTickCount has ~15.6 ms
    // granularity, so two calls inside one tick give a zero delta, and treating
    // that as "no fade" is what collapsed the storm track's four-second fade
    // into a single frame. Same trap, same answer.
    const int target = g_tune.volume < 0 ? 0
                     : (g_tune.volume > 1000 ? 1000 : g_tune.volume);
    if (dt > 0 && g_tune.fadeMs > 0 && g_level != target) {
        const int step = (1000 * dt) / g_tune.fadeMs;
        if (step > 0) {
            if (g_level < target)
                g_level = (g_level + step > target) ? target : g_level + step;
            else
                g_level = (g_level - step < target) ? target : g_level - step;
        }
    } else if (g_tune.fadeMs <= 0) {
        g_level = target;
    }

    // Written every tick rather than only on change, because the game's sliders
    // can move under us while the fight is going on.
    audiomix::SetLevel(g_track, VoiceLevel());
}

inline void Shutdown()
{
    Silence("shutting down");
    if (g_loaded) { audiomix::Release(g_track); g_loaded = false; }
    gameaudio::Restore();
}

} // namespace battlemusic
