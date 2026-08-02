// stormaudio.h - a music bed that comes up when the weather does.
//
// Plays a looping track while the player is in a storm and fades it away as
// they sail clear. The "am I in a storm" question is already answered by the
// engine: FUN_0045FA70 returns a weather intensity that rises as you close, and
// storms.h calls it for the cargo loss. This reuses the same number, so the
// music, the rain and the cargo all agree about what counts as bad weather.
//
// ------------------------------------------------------------------ why MCI
// The game's own audio is Miles, and it CAN play mp3 -- FUN_0052CDC0 builds
// `<base>.mp3` and probes for it. But playing a brand-new clip by name through
// Miles needs the mgr `this` and the lower-level filename path, which is
// unfinished RE (see the audio notes in GAME_API.md). Rather than block a music
// bed on that, this uses MCI from winmm, which the build already links.
//
// The trade, stated plainly:
//   + works today, no new RE, no new dependency, mp3 decoded by Windows
//   + volume 0..1000 per command, which is all a fade needs
//   - it is OUR mixer, not the game's, so the game's volume slider does not
//     touch it. Gated on actually sailing so it does not play over menus.
//
// If the by-name Miles path is ever finished this should move onto it.
#pragma once
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

namespace stormaudio {

struct Tuning {
    int  enabled     = 1;
    int  volume      = 800;    // 0..1000, the level it settles at
    int  startAt     = 8;      // engine weather intensity that brings it in
    int  stopAt      = 5;      // ...and the lower one that takes it away
    int  fadeMs      = 2500;
    char file[128]   = "storm.mp3";
};

inline Tuning g_tune;

inline bool  g_opened   = false;
inline bool  g_playing  = false;
inline int   g_level    = 0;      // current volume, 0..1000
inline DWORD g_lastTick = 0;
inline DWORD g_lastPoll = 0;
inline char  g_path[MAX_PATH] = {0};

// Two thresholds rather than one, so a player sitting exactly on the edge of a
// storm does not get the track switching on and off every few seconds.
inline int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline bool Mci(const char* cmd, bool logFailure = true)
{
    char err[256] = {0};
    const MCIERROR rc = mciSendStringA(cmd, nullptr, 0, nullptr);
    if (rc == 0) return true;
    if (logFailure) {
        if (!mciGetErrorStringA(rc, err, sizeof(err))) err[0] = 0;
        Log("storm audio: '%s' failed -- %s", cmd, err[0] ? err : "(no detail)");
    }
    return false;
}

inline void Configure(const char* gameDir)
{
    _snprintf_s(g_path, sizeof(g_path), _TRUNCATE, "%s\\PEMF\\audio\\%s",
                gameDir, g_tune.file);
}

inline bool Open()
{
    if (g_opened) return true;
    if (!g_path[0]) return false;

    if (GetFileAttributesA(g_path) == INVALID_FILE_ATTRIBUTES) {
        Log("storm audio: no %s -- storms stay quiet", g_path);
        g_tune.enabled = 0;                   // do not retry every frame
        return false;
    }

    char cmd[MAX_PATH + 64];
    // Quoted because the game folder has spaces in it, and an alias so every
    // later command is short and unambiguous.
    _snprintf_s(cmd, sizeof(cmd), _TRUNCATE,
                "open \"%s\" type mpegvideo alias pemfstorm", g_path);
    if (!Mci(cmd)) {
        Log("storm audio: could not open the track -- disabling");
        g_tune.enabled = 0;
        return false;
    }
    g_opened = true;
    Log("storm audio: opened %s", g_path);
    return true;
}

inline bool g_volumeWorks   = true;
inline bool g_volumeChecked = false;

inline void SetLevel(int level)
{
    char cmd[96];
    _snprintf_s(cmd, sizeof(cmd), _TRUNCATE,
                "setaudio pemfstorm volume to %d", Clamp(level, 0, 1000));

    // Report the FIRST failure and then go quiet. Not every MCI driver honours
    // volume for mp3, and if this one does not then the fade is silently doing
    // nothing -- which reads in play as the track snapping on and off. Better
    // to say so once than to leave it looking like a bug in the ramp.
    const bool ok = Mci(cmd, !g_volumeChecked);

    // ⚠️ MCI RETURNING SUCCESS IS NOT EVIDENCE THE VOLUME CHANGED. A driver may
    // accept the command and ignore it, which is indistinguishable from a
    // working fade until you listen. So read it back and compare.
    if (!g_volumeChecked && level > 0) {
        g_volumeChecked = true;
        char got[64] = {0};
        const bool readable =
            mciSendStringA("status pemfstorm volume", got, sizeof(got), nullptr) == 0;
        const int reported = readable ? atoi(got) : -1;
        g_volumeWorks = ok && readable && reported > 0;

        Log("storm audio: volume check -- set %d, driver %s, reads back %s (%d)",
            level, ok ? "accepted" : "REFUSED",
            readable ? got : "(unreadable)", reported);
        if (!g_volumeWorks) {
            Log("storm audio: volume is not actually being applied -- the track "
                "will cut in and out rather than fade. Needs a real mixer.");
        }
    }
}

inline void Start()
{
    if (g_playing) return;
    SetLevel(g_level);
    Mci("seek pemfstorm to start", false);
    if (Mci("play pemfstorm")) {
        g_playing = true;
        Log("storm audio: in");
    }
}

inline void Stop()
{
    if (!g_playing) return;
    Mci("stop pemfstorm", false);
    g_playing = false;
    g_level = 0;
    Log("storm audio: out");
}

// Looping without relying on `play ... repeat`, which not every MCI driver
// honours for mp3. Poll the transport now and then and start it again if it has
// run to the end. Cheap at twice a second.
inline void KeepLooping(DWORD now)
{
    if (!g_playing) return;
    if (now - g_lastPoll < 500) return;
    g_lastPoll = now;

    char mode[64] = {0};
    if (mciSendStringA("status pemfstorm mode", mode, sizeof(mode), nullptr) != 0)
        return;
    if (_stricmp(mode, "playing") == 0) return;

    Mci("seek pemfstorm to start", false);
    Mci("play pemfstorm", false);
}

// `intensity` is the engine's own weather value; `sailing` gates the whole
// thing so the track never plays over a menu or a town.
inline void Tick(int intensity, bool sailing)
{
    if (!g_tune.enabled) return;

    const DWORD now = GetTickCount();
    const int dt = g_lastTick ? (int)(now - g_lastTick) : 0;
    g_lastTick = now;

    const bool want = sailing && intensity >= g_tune.startAt;
    const bool keep = sailing && intensity >= g_tune.stopAt && g_playing;
    const int target = (want || keep) ? Clamp(g_tune.volume, 0, 1000) : 0;

    if (target > 0 && !g_opened && !Open()) return;

    // Ramp rather than jump. A storm arriving with a hard cut sounds like a
    // bug; arriving over a couple of seconds sounds like weather.
    if (dt > 0 && g_tune.fadeMs > 0) {
        const int step = 1000 * dt / g_tune.fadeMs;
        if (g_level < target) g_level = (g_level + step > target) ? target : g_level + step;
        else if (g_level > target) g_level = (g_level - step < target) ? target : g_level - step;
    } else {
        g_level = target;
    }

    if (g_level > 0) {
        Start();
        SetLevel(g_level);
        KeepLooping(now);
    } else {
        Stop();
    }
}

// Career change, or leaving the sailing view for good.
inline void Silence()
{
    if (g_playing) Stop();
    g_level = 0;
}

inline void Shutdown()
{
    Silence();
    if (g_opened) {
        Mci("close pemfstorm", false);
        g_opened = false;
    }
}

} // namespace stormaudio
