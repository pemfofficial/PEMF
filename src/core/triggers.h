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
    int  screenId    = 0;   // together, the screen signature -- see WorldOnScreen
    int  screenDepth = 0;
};

// ------------------------------------------------- is the overworld on screen
// Drawing needs a different question answered than firing does, and for a long
// time it asked a worse version of the same one: "has the ship moved in the
// last 350 ms". That is not a screen test. Opening a menu freezes the ship, so
// a notice went on painting over the menu until the window lapsed -- an
// anchored one projected onto whatever the menu was showing, which put a
// lookout's call across the Load/Save map (reported from a playtest 2026-07-28).
// Shortening the window could not fix it either: a becalmed or paused ship at
// sea is indistinguishable from a menu under a motion test, so a tighter window
// only trades a menu leak for notices vanishing at sea.
//
// What works is a POSITIVE signal, and it turns out the screen-state globals
// give one. game.h once recorded them as a dead end, which was too strong a
// conclusion: what was actually established is that they are not an ENUM, and
// they are not -- but they are a stable per-screen SIGNATURE. Measured over a
// session that visited every screen:
//
//   sailing / overworld   0x0FFFEFDF, 0x0FFFFFDF   depth 3
//   town                  0x0FFFEFFA, 0x0FFFFFFA   depth 3
//   Load / Save           0x0FFBE770, 0x0FFBE750   depth 4
//   battle                0x8FFFEFFF, 0x8FFFFFFF   depth 4-5
//   main menu             0x0FFFEFF0, 0x0FFFFFF0   depth 1
//
// Those read as a bitfield rather than an identifier, so hardcoding the sailing
// values would be a constant nobody could maintain: one HUD state we did not
// happen to visit and notices stop, silently, which is the failure mode this
// project likes least.
//
// So the gate CALIBRATES ITSELF. A ship whose position changed this very tick
// is unambiguously out on the overworld, whatever the numbers happen to be --
// so that is when a signature is learned. Afterwards the overworld is "on
// screen" whenever the live signature matches one we learned. Motion is used to
// LEARN the answer, never to be the answer.
//
// That fixes both halves at once. A menu never matches, so nothing leaks and
// there is no tail to wait out. A becalmed ship still matches, so notices stop
// dropping out when you come to a stop -- which the old window did too.
//
// It also fails in the safe direction: an unrecognised screen draws nothing
// until the ship moves and teaches us the signature, so the worst case is a
// missing notice rather than one painted over a menu.
//
// Battle is deliberately NOT learned. It has its own signature and its own ship
// array, and the overworld position is frozen throughout, so a notice anchored
// to a map position would hang in the wrong place anyway.
struct ScreenSignature {
    int id    = 0;
    int depth = 0;
};

// Four is comfortably more than the two variants the overworld has been seen to
// use -- one bit of the id flickers -- while still being small enough that a
// wrong entry could never accumulate into "everything matches".
constexpr int kMaxWorldSignatures = 4;

inline ScreenSignature g_worldSig[kMaxWorldSignatures];
inline int             g_worldSigCount = 0;

inline bool WorldOnScreen();
inline void LearnWorldSignature(int id, int depth);

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

        s.screenId    = *(const int*)game::addr::ScreenId;
        s.screenDepth = *(const int*)game::addr::ScreenDepth;

        DWORD now = GetTickCount();
        if (!g_havePos) {
            g_havePos = true;
            g_lastX = s.x; g_lastY = s.y;
            g_lastMovedAt = now;
        } else if (s.x != g_lastX || s.y != g_lastY) {
            g_lastX = s.x; g_lastY = s.y;
            g_lastMovedAt = now;
            // The position changed on THIS tick, so the overworld is certainly
            // what is on screen. That is the only moment worth learning from --
            // a moment later the ship could be stationary in a menu with the
            // same recent-movement history.
            LearnWorldSignature(s.screenId, s.screenDepth);
        }
        s.moving = (now - g_lastMovedAt) < kMovingWindowMs;

        s.nearestCity = game::NearestCity(kScanRadius);
        if (s.nearestCity >= 0)
            s.nearestDist = game::CityDistance(s.nearestCity);
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

// Learned only from a tick where the position actually changed. See the note
// above: motion teaches the signature, it does not stand in for it.
inline void LearnWorldSignature(int id, int depth)
{
    for (int i = 0; i < g_worldSigCount; ++i)
        if (g_worldSig[i].id == id && g_worldSig[i].depth == depth) return;

    if (g_worldSigCount >= kMaxWorldSignatures) {
        // More variants than the overworld has ever been seen to use. Report it
        // rather than silently evicting: it means the assumption behind this
        // gate needs re-measuring, and a quiet gate is exactly what we were
        // trying to get away from.
        static bool warned = false;
        if (!warned) {
            warned = true;
            Log("triggers: more than %d overworld screen signatures seen "
                "(latest 0x%08X depth 0x%08X) -- the screen-state assumption "
                "needs re-checking", kMaxWorldSignatures,
                (unsigned)id, (unsigned)depth);
        }
        return;
    }

    g_worldSig[g_worldSigCount].id    = id;
    g_worldSig[g_worldSigCount].depth = depth;
    ++g_worldSigCount;
    Log("triggers: learned overworld screen signature 0x%08X depth 0x%08X (%d)",
        (unsigned)id, (unsigned)depth, g_worldSigCount);
}

inline bool WorldOnScreen()
{
    if (!g_havePos || g_worldSigCount <= 0) return false;

    __try {
        const int id    = *(const int*)game::addr::ScreenId;
        const int depth = *(const int*)game::addr::ScreenDepth;
        for (int i = 0; i < g_worldSigCount; ++i)
            if (g_worldSig[i].id == id && g_worldSig[i].depth == depth)
                return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

// ------------------------------------------------------- per-event runtime
// Kept parallel to the content library and indexed the same way. Deliberately
// NOT part of content::Event: content is reloadable, this is runtime state.
struct Runtime {
    DWORD sailingMs = 0;      // accumulated sailing time toward ElapsedSailing
    DWORD lastFired = 0;
    int   fireCount = 0;
    bool  armed     = true;   // NearPort: false while still inside the radius

    // "armed" above is an assumption, not an observation: a fresh Runtime says
    // armed because nothing has been seen yet, which is fine at sea and wrong
    // in a harbour. Resetting the triggers while the ship is already INSIDE a
    // nearPort radius armed a trigger whose entering edge had long since
    // passed, and it fired at once -- four times in five seconds during a run
    // of career switches, at an identical distance of 2896, because each
    // career change reset the triggers and the ship had not moved.
    //
    // So the first evaluation after a reset observes rather than fires: it
    // sets armed from the world as it actually is, and only then does the
    // edge logic mean anything.
    bool  fresh     = true;
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

            // First look after a reset: adopt the world's state rather than
            // firing on an edge that was crossed before we were watching.
            if (rt.fresh) {
                rt.fresh = false;
                rt.armed = (d > ev->trigger.distance);
                if (!rt.armed)
                    Log("trigger: '%s' starts disarmed -- already inside the "
                        "radius (dist %d <= %d)",
                        ev->id.c_str(), d, ev->trigger.distance);
                break;
            }

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

            // Same first-look rule as NearPort: a career that begins already
            // past the threshold has not CROSSED it in front of us.
            if (rt.fresh) {
                rt.fresh = false;
                rt.armed = !inside;
                if (!rt.armed)
                    Log("trigger: '%s' starts disarmed -- %s is already %s %d "
                        "(now %d)", ev->id.c_str(), what,
                        ev->trigger.useBelow ? "below" : "above", limit, value);
                break;
            }

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
