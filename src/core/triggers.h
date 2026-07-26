// triggers.h - deciding when an event should fire.
//
// Triggers are evaluated at the SAFE POINT and never present anything: they
// only ever `events::Post()`. That keeps the invariant that nothing re-enters
// the game from an arbitrary frame position.
//
// "Sailing" is the gate on everything. The player must be out on the overworld
// with the ship actually under way -- not in a town, not in a menu, not in a
// battle or a mini-game. See Sailing() for how that is currently determined and
// what is still provisional about it.
#pragma once
#include <windows.h>
#include <vector>

#include "log.h"
#include "game.h"
#include "state.h"
#include "session.h"
#include "events.h"
#include "content.h"

namespace triggers {

// How far out we look when reporting the nearest port. Generous: this is for
// observation, not for firing.
constexpr int kScanRadius = 20000;

// The ship must have moved within this window to count as under way. Long
// enough to survive a stationary frame, short enough that sitting in a town
// stops counting almost immediately.
constexpr DWORD kMovingWindowMs = 2500;

// ------------------------------------------------------------------ sampling
struct WorldSample {
    bool inGame      = false;
    bool moving      = false;
    int  x = 0, y = 0;
    int  nearestCity = -1;
    int  nearestDist = -1;
    int  screenId    = 0;   // diagnostic only
    int  screenDepth = 0;   // diagnostic only
};

// Drawing wants a TIGHTER test than firing does. kMovingWindowMs deliberately
// tolerates a stationary moment so a trigger is not lost to one frozen frame;
// for drawing that same tolerance is a bug, because opening a menu freezes the
// ship and the notice would go on being painted over the menu for the whole
// window. A notice leaking onto a menu for two seconds is very visible, and
// losing a few frames of a notice while becalmed is not.
constexpr DWORD kDrawWindowMs = 350;

// Is the overworld actually on screen right now? This is a heuristic, like
// Sailing() -- the game's obvious screen-id globals turned out to be
// pointer-like rather than an enum (see game.h). It holds because every menu
// and town screen freezes the ship's position.
inline bool WorldOnScreen();

inline int   g_lastX = 0, g_lastY = 0;
inline DWORD g_lastMovedAt = 0;
inline bool  g_havePos = false;

inline WorldSample Sample()
{
    WorldSample s;
    __try {
        s.inGame = state::InGame();
        if (!s.inGame) { g_havePos = false; return s; }

        s.x = game::PlayerX();
        s.y = game::PlayerY();

        DWORD now = GetTickCount();
        if (!g_havePos) {
            g_havePos = true;
            g_lastX = s.x; g_lastY = s.y;
            g_lastMovedAt = now;
        } else if (s.x != g_lastX || s.y != g_lastY) {
            g_lastX = s.x; g_lastY = s.y;
            g_lastMovedAt = now;
        }
        s.moving = (now - g_lastMovedAt) < kMovingWindowMs;

        s.nearestCity = game::NearestCity(kScanRadius);
        if (s.nearestCity >= 0)
            s.nearestDist = game::CityDistance(s.nearestCity);

        s.screenId    = *(const int*)game::addr::ScreenId;
        s.screenDepth = *(const int*)game::addr::ScreenDepth;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("triggers: EXCEPTION 0x%08X sampling world state", GetExceptionCode());
        s.inGame = false;
    }
    return s;
}

// "Sailing" means: in a career, and the ship's position has changed recently.
// In a town or a menu the position is frozen, so this separates them.
//
// VALIDATED BY PLAYTEST (2026-07-25): across a ~10 minute session `sailing`
// read 1 throughout open-sea travel and dropped to 0 exactly while the ship was
// stationary. It remains a heuristic rather than a screen-state read -- see the
// note on ScreenId in game.h for why the obvious candidates turned out not to
// be usable -- but it behaves correctly in practice.
//
// A consequence worth knowing: pausing while at sea also reads as not sailing,
// so events will not fire while paused. That is the desirable behaviour anyway.
inline bool Sailing(const WorldSample& s)
{
    return s.inGame && s.moving;
}

inline bool WorldOnScreen()
{
    if (!g_havePos) return false;
    return (GetTickCount() - g_lastMovedAt) < kDrawWindowMs;
}

// ------------------------------------------------------- per-event runtime
// Kept parallel to the content library and indexed the same way. Deliberately
// NOT part of content::Event: content is reloadable, this is runtime state.
struct Runtime {
    DWORD sailingMs = 0;      // accumulated sailing time toward ElapsedSailing
    DWORD lastFired = 0;
    int   fireCount = 0;
    bool  armed     = true;   // NearPort: false while still inside the radius
};

inline std::vector<Runtime> g_rt;
inline DWORD g_lastTick = 0;

// Closest the ship has come to any port this career. Reported in the world
// sample so authors can choose `distance` from measured numbers rather than
// guessing -- guessing 400 against a real minimum of 988 is what made the first
// nearPort event unable to fire at all.
inline int g_closestEver = -1;

inline void Reset(const char* why)
{
    g_rt.assign(content::Count(), Runtime{});
    g_lastTick = 0;
    g_havePos = false;
    g_closestEver = -1;
    Log("triggers: armed %d event(s) (%s)", (int)g_rt.size(), why);
}

// --------------------------------------------------------------- evaluation
inline bool Eligible(const content::Event& ev, Runtime& rt, DWORD now)
{
    if (ev.trigger.once && rt.fireCount > 0) return false;
    if (ev.trigger.cooldown > 0 && rt.lastFired &&
        (now - rt.lastFired) < (DWORD)ev.trigger.cooldown * 1000) return false;
    return true;
}

inline void Tick()
{
    if (events::Faulted()) return;
    if (!session::Ready())  { g_lastTick = 0; return; }
    if ((int)g_rt.size() != content::Count()) Reset("content count changed");

    const DWORD now = GetTickCount();
    const DWORD dt  = g_lastTick ? (now - g_lastTick) : 0;
    g_lastTick = now;

    const WorldSample s = Sample();
    const bool sailing = Sailing(s);

    if (sailing && s.nearestDist >= 0 &&
        (g_closestEver < 0 || s.nearestDist < g_closestEver)) {
        g_closestEver = s.nearestDist;
    }

    for (int i = 0; i < content::Count(); ++i) {
        const content::Event* ev = content::Get(i);
        if (!ev || ev->trigger.type == content::TriggerType::None) continue;
        Runtime& rt = g_rt[i];

        switch (ev->trigger.type) {

        case content::TriggerType::ElapsedSailing: {
            // Only accumulate while actually under way, so time in port does
            // not count toward it.
            if (!sailing) break;
            rt.sailingMs += dt;
            if (rt.sailingMs < (DWORD)ev->trigger.seconds * 1000) break;
            if (!Eligible(*ev, rt, now)) { rt.sailingMs = 0; break; }
            rt.sailingMs = 0;
            rt.lastFired = now;
            ++rt.fireCount;
            Log("trigger: '%s' after %d s sailing", ev->id.c_str(),
                ev->trigger.seconds);
            events::Post([](int idx){ content::Fire(idx); }, i, ev->id.c_str());
            break;
        }

        case content::TriggerType::NearPort: {
            if (!sailing || s.nearestCity < 0) break;
            const int d = s.nearestDist;

            // Edge-triggered: fire on entering the radius, and only re-arm once
            // clearly outside it, so drifting along the boundary cannot spam.
            if (!rt.armed) {
                if (d > ev->trigger.rearm) {
                    rt.armed = true;
                    Log("trigger: '%s' re-armed (dist %d > %d)",
                        ev->id.c_str(), d, ev->trigger.rearm);
                }
                break;
            }
            if (d > ev->trigger.distance) break;
            if (!Eligible(*ev, rt, now)) break;

            rt.armed = false;
            rt.lastFired = now;
            ++rt.fireCount;
            Log("trigger: '%s' near port (city %d at dist %d <= %d)",
                ev->id.c_str(), s.nearestCity, d, ev->trigger.distance);
            events::Post([](int idx){ content::Fire(idx); }, i, ev->id.c_str());
            break;
        }

        case content::TriggerType::StateCrosses: {
            // Not gated on sailing: a crew going hungry or a purse running dry
            // is worth saying wherever it happens.
            if (!s.inGame) break;

            int value = 0;
            const char* what = "";
            switch (ev->trigger.field) {
            case content::StateField::Crew:
                value = state::Crew();    what = "crew";   break;
            case content::StateField::Gold:
                value = state::Plunder(); what = "gold";   break;
            case content::StateField::Morale:
                value = state::Morale();  what = "morale"; break;
            case content::StateField::Months:
                value = state::Months();  what = "months"; break;
            }

            const int  limit  = ev->trigger.useBelow ? ev->trigger.below
                                                     : ev->trigger.above;
            const bool inside = ev->trigger.useBelow ? (value <  limit)
                                                     : (value >  limit);

            // Edge-triggered, exactly as NearPort is: fire on crossing in, and
            // re-arm only on crossing back out. A value that merely sits past
            // the threshold must not fire every frame.
            if (!rt.armed) {
                if (!inside) {
                    rt.armed = true;
                    Log("trigger: '%s' re-armed (%s back to %d)",
                        ev->id.c_str(), what, value);
                }
                break;
            }
            if (!inside) break;
            if (!Eligible(*ev, rt, now)) break;

            rt.armed = false;
            rt.lastFired = now;
            ++rt.fireCount;
            Log("trigger: '%s' -- %s %s %d (now %d)", ev->id.c_str(), what,
                ev->trigger.useBelow ? "below" : "above", limit, value);
            events::Post([](int idx){ content::Fire(idx); }, i, ev->id.c_str());
            break;
        }

        default: break;
        }
    }
}

// ------------------------------------------------------------- diagnostics
// Logged periodically so the world sample can be checked against what is
// actually on screen -- and so the real "am I sailing" signal can be found.
inline void LogSample()
{
    const WorldSample s = Sample();
    Log("world: inGame=%d sailing=%d pos=(%d,%d) nearest=city%d dist=%d "
        "closestEver=%d",
        (int)s.inGame, (int)Sailing(s), s.x, s.y,
        s.nearestCity, s.nearestDist, g_closestEver);
}

} // namespace triggers
