// suspicion.h - what it costs to wear another nation's colours.
//
// The design, and why it is shaped this way, is in docs/SUSPICION.md. The short
// version:
//
//   A disguise buys you CLOSENESS. Closeness is what gets you caught.
//
// Suspicion rises from being LOOKED AT -- near the ships and ports of the crown
// whose flag you are wearing -- and falls when nobody is watching. It is not a
// timer. Open water is where a disguise is safest, and a system that punished
// you for sailing normally would just nag.
//
// ------------------------------------------------------------------ locality
// Suspicion is per nation, but it also has a PLACE. Whoever saw something saw
// it somewhere, and sailing two hundred miles away puts you in front of people
// who have not heard. So each nation's suspicion remembers where it was last
// raised, and decays much faster once you are well clear of that water. Getting
// out of the area really does help, which is what a captain would expect.
//
// That mirrors something the engine already does: reputation exists per CITY as
// well as per nation (0x0085BF7C, stride 0x94), so "Havana knows, Cartagena
// does not yet" is a distinction the game itself draws.
//
// --------------------------------------------------------------- consequences
// PEMF owns suspicion. It does not own the world. At the top of the ladder it
// writes exactly one number the game already reads -- REPUTATION -- and lets
// the engine's own machinery do the rest: ports close, a price appears on your
// head, an amnesty becomes purchasable. Everything below that threshold is
// text.
//
// The one thing we build ourselves is the hunter, because a ship needs steering
// and the game has no "chase that vessel" behaviour to borrow. Its strength
// comes from the engine's own formula (2 - reputation/10, clamped 2..4), so a
// crown that loathes you sends something worse.
#pragma once
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "game.h"
#include "state.h"
#include "nations.h"

namespace suspicion {

// ------------------------------------------------------------------- tuning
// Everything a play session might want to argue about lives here and is
// reloadable from PEMF\suspicion.ini. Defaults are a starting point, not a
// balance pass -- this system will be tuned by playing it.
struct Tuning {
    // How fast suspicion moves, in points per second.
    int riseNearShip   = 4;    // per vessel of that nation within shipRange
    int riseNearPort   = 3;    // their settlement within portRange
    int riseCloseRange = 9;    // ANY vessel inside closeRange got a proper look
    int riseInfamy     = 2;    // extra when they already have cause to know you

    int decayWatched   = 0;    // while still under observation
    int decayClear     = 3;    // nobody in sight
    int decayFarAway   = 8;    // and well clear of where it was earned

    // Distances, in the game's city units (the map is ~422,000 across).
    int shipRange  = 14000;
    int portRange  = 9000;
    int closeRange = 4500;
    int heatRadius = 45000;    // beyond this, the trail is going cold

    // The ladder.
    int threshNotice    = 30;
    int threshWarning   = 60;
    int threshChallenge = 90;
    int threshUnmask    = 100;

    // What being rumbled costs. Negative reputation is the game's own hostility
    // model: below 0 their ports close, below -1 there is a price on your head.
    int repPenalty = 8;

    // The hunter.
    int  hunterReaimMs   = 5000;    // how often she re-aims at you
    int  hunterGiveUpMs  = 240000;  // ...before losing interest
    int  hunterEscapeDist = 70000;  // or you simply get clear
    bool hunterEnabled   = true;

    int  maxPerNation = 1;          // hunters at sea per crown
};

inline Tuning g_tune;

// --------------------------------------------------------------- live state
struct NationSuspicion {
    int   level   = 0;      // 0..100
    int   heatX   = 0;      // where it was last raised
    int   heatY   = 0;
    bool  hasHeat = false;
    int   lastRate = 0;     // points/sec last tick, for the panel
    DWORD lastBeat = 0;     // threshold we have already spoken about
    int   spokenAt = 0;

    // Fractional carry, in point-milliseconds.
    //
    // Without this the system cannot work at all: the safe point runs every
    // ~16 ms, so `level += rate * dt / 1000` is `12 * 16 / 1000`, which is ZERO
    // in integer arithmetic, on every tick, forever. The panel showed a correct
    // rate of +12 beside a level that never left 0 -- a rate is not progress
    // until something keeps the remainder.
    int   carry = 0;
};

inline NationSuspicion g_sus[game::addr::kNationsWithRank];

struct Hunt {
    bool  active   = false;
    int   slot     = -1;
    int   nation   = -1;
    int   strength = 0;
    DWORD startedAt = 0;
    DWORD lastAimAt = 0;
    int   aimCity  = -1;
};

// Several per nation, because a crown that truly wants you does not send one
// ship. How many actually sail is decided by reputation at the moment of
// dispatch -- see HunterCountFor().
constexpr int kMaxHuntsPerNation = 4;
inline Hunt g_hunts[game::addr::kNationsWithRank][kMaxHuntsPerNation];

// The nation whose colours we are currently wearing, or -1 for honest sailing.
// Set by the flag layer; suspicion does not guess it.
inline int g_wearing = -1;

inline void SetWearing(int nation) { g_wearing = nation; }

inline void ResetAll()
{
    for (int n = 0; n < game::addr::kNationsWithRank; ++n) {
        g_sus[n] = NationSuspicion{};
        for (int k = 0; k < kMaxHuntsPerNation; ++k) g_hunts[n][k] = Hunt{};
    }
    g_wearing = -1;
}

// How many ships a crown sends. One while they merely dislike you; a squadron
// once there is a price on your head. Reputation is the same number the engine
// uses to decide how STRONG each one is, so the two scale together and a
// thoroughly hated captain gets four strong ships rather than one weak one.
inline int HunterCountFor(int nation)
{
    const int rep = nations::Reputation(nation);
    int n = 1;
    if (rep <= -10) n = 2;
    if (rep <= -25) n = 3;
    if (rep <= -45) n = 4;
    if (n > g_tune.maxPerNation) n = g_tune.maxPerNation;
    if (n > kMaxHuntsPerNation)  n = kMaxHuntsPerNation;
    return n;
}

// ------------------------------------------------------------------ tuning io
inline void LoadTuning(const char* gameDir)
{
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\PEMF\\suspicion.ini", gameDir);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) {
        Log("suspicion: no %s -- using built-in defaults", path);
        return;
    }

    char line[256];
    int applied = 0;
    while (fgets(line, sizeof(line), f)) {
        char key[64] = {0};
        int  value = 0;
        if (sscanf_s(line, " %63[A-Za-z_] = %d", key, (unsigned)sizeof(key),
                     &value) != 2) {
            continue;                       // comment, blank, or malformed
        }
        struct Entry { const char* name; int* slot; };
        const Entry table[] = {
            { "riseNearShip",   &g_tune.riseNearShip },
            { "riseNearPort",   &g_tune.riseNearPort },
            { "riseCloseRange", &g_tune.riseCloseRange },
            { "riseInfamy",     &g_tune.riseInfamy },
            { "decayWatched",   &g_tune.decayWatched },
            { "decayClear",     &g_tune.decayClear },
            { "decayFarAway",   &g_tune.decayFarAway },
            { "shipRange",      &g_tune.shipRange },
            { "portRange",      &g_tune.portRange },
            { "closeRange",     &g_tune.closeRange },
            { "heatRadius",     &g_tune.heatRadius },
            { "threshNotice",   &g_tune.threshNotice },
            { "threshWarning",  &g_tune.threshWarning },
            { "threshChallenge",&g_tune.threshChallenge },
            { "threshUnmask",   &g_tune.threshUnmask },
            { "repPenalty",     &g_tune.repPenalty },
            { "hunterReaimMs",  &g_tune.hunterReaimMs },
            { "hunterGiveUpMs", &g_tune.hunterGiveUpMs },
            { "hunterEscapeDist", &g_tune.hunterEscapeDist },
            { "maxPerNation",   &g_tune.maxPerNation },
        };
        for (const Entry& e : table) {
            if (_stricmp(key, e.name) == 0) { *e.slot = value; ++applied; break; }
        }
        if (_stricmp(key, "hunterEnabled") == 0) {
            g_tune.hunterEnabled = (value != 0); ++applied;
        }
    }
    fclose(f);
    Log("suspicion: loaded %d setting(s) from %s", applied, path);
}

// ------------------------------------------------------------------ geometry
inline int Octagonal(int dx, int dy)
{
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    const int lo = dx < dy ? dx : dy;
    const int hi = dx < dy ? dy : dx;
    return (lo + hi * 2) / 2;
}

// -------------------------------------------------------------- observation
// Who can see us, and how hard they are looking. Called once per safe point.
struct Look {
    int ships = 0;      // vessels of the deceived nation within shipRange
    int close = 0;      // ANY vessel inside closeRange
    int port  = -1;     // their settlement within portRange, or -1
    int portDist = 0;
    int nearest  = 0;   // closest vessel of theirs, for the panel
};

inline Look Observe(int nation)
{
    Look look;
    look.nearest = 0x7FFFFFFF;

    const int px = game::PlayerX() / 1000;
    const int py = game::PlayerY() / 1000;

    for (int i = 1; i < 24; ++i) {
        const int type = game::ShipType(i);
        if (type == -1) continue;
        const int x = *(const int*)(game::ShipRecord(i) + 0x0C) / 1000;
        const int y = *(const int*)(game::ShipRecord(i) + 0x10) / 1000;
        if (x == 0 && y == 0) continue;

        const int d = Octagonal(x - px, y - py);
        if (d <= g_tune.closeRange) ++look.close;

        if (game::ShipNationality(i) == nation) {
            if (d <= g_tune.shipRange) ++look.ships;
            if (d < look.nearest) look.nearest = d;
        }
    }
    if (look.nearest == 0x7FFFFFFF) look.nearest = -1;

    // Their nearest port. Cities are cheap to walk and there are only 128.
    int bestDist = g_tune.portRange + 1;
    for (int c = 0; c < game::addr::kMaxCities; ++c) {
        if (game::CityNation(c) != nation) continue;
        const int d = game::CityDistance(c);
        if (d < 0 || d >= bestDist) continue;
        bestDist = d; look.port = c;
    }
    look.portDist = (look.port >= 0) ? bestDist : -1;
    return look;
}

// The rate, in points per second. Positive means they are working it out.
inline int RateFor(int nation, const Look& look)
{
    int rate = 0;
    rate += look.ships * g_tune.riseNearShip;
    rate += look.close * g_tune.riseCloseRange;
    if (look.port >= 0) rate += g_tune.riseNearPort;

    // A captain they already have reason to know is harder to pass off. Uses
    // the game's own standing rather than a number of ours.
    if (rate > 0 && nations::Reputation(nation) < 0) rate += g_tune.riseInfamy;

    if (rate > 0) return rate - g_tune.decayWatched;

    // Nobody watching. How fast it cools depends on whether we are still in the
    // water where it was earned.
    const NationSuspicion& s = g_sus[nation];
    if (s.hasHeat) {
        const int px = game::PlayerX() / 1000, py = game::PlayerY() / 1000;
        if (Octagonal(px - s.heatX, py - s.heatY) > g_tune.heatRadius)
            return -g_tune.decayFarAway;
    }
    return -g_tune.decayClear;
}

// ------------------------------------------------------------------ hunters
// Sending one, and keeping her pointed at us.
//
// The game has no "chase that ship" behaviour to borrow -- a vessel sails to a
// PLACE. So a hunt is a ship rebuilt toward the player every few seconds, and
// from the deck that is a warship that will not let go.
inline bool DispatchOneHunter(int nation, Hunt& h)
{
    if (!game::SpawnShipCallable()) {
        Log("suspicion: cannot dispatch -- the ship factory did not verify");
        return false;
    }

    // From one of THEIR ports, so she comes from somewhere plausible.
    int from = -1, bestDist = 0x7FFFFFFF;
    for (int c = 0; c < game::addr::kMaxCities; ++c) {
        if (game::CityNation(c) != nation) continue;
        const int d = game::CityDistance(c);
        if (d < 0 || d >= bestDist) continue;
        bestDist = d; from = c;
    }
    if (from < 0) {
        Log("suspicion: %s has no port near enough to send anyone from",
            game::NationName(nation));
        return false;
    }

    const int slot = game::SpawnShipAtCity(from, 0x0B);
    if (slot < 8 || slot >= game::addr::kMaxShips) {
        Log("suspicion: the yard could not build a hunter (returned %d)", slot);
        return false;
    }

    // The engine's own recipe: strength scales with how badly they think of us.
    const int strength = game::HunterStrengthFor(nation);
    game::SetShipPurposeRaw(slot, game::kPurposePirateHunter);
    game::SetShipRoleRaw(slot, strength);
    game::MarkCitySentHunter(from, slot);

    h.active = true; h.slot = slot; h.nation = nation;
    h.strength = strength;
    h.startedAt = h.lastAimAt = GetTickCount();
    h.aimCity = -1;

    Log("suspicion: %s dispatches a pirate-hunter from city %d -- slot %d, "
        "strength %d (reputation %d)", game::NationName(nation), from, slot,
        strength, nations::Reputation(nation));
    return true;
}

inline void DispatchHunters(int nation)
{
    if (!g_tune.hunterEnabled) return;
    if (nation < 0 || nation >= game::addr::kNationsWithRank) return;

    const int want = HunterCountFor(nation);
    int have = 0;
    for (int k = 0; k < kMaxHuntsPerNation; ++k)
        if (g_hunts[nation][k].active) ++have;

    for (int k = 0; k < kMaxHuntsPerNation && have < want; ++k) {
        if (g_hunts[nation][k].active) continue;
        if (DispatchOneHunter(nation, g_hunts[nation][k])) ++have;
    }
    Log("suspicion: %s now has %d hunter(s) at sea (wanted %d, reputation %d)",
        game::NationName(nation), have, want, nations::Reputation(nation));
}

// Point her at us. Her destination is a PORT, so we pick whichever of theirs is
// nearest the player -- she then sails into our water rather than at a fixed
// spot, and re-aiming keeps her honest as we run.
inline void ReaimHunter(Hunt& h)
{
    int best = -1, bestDist = 0x7FFFFFFF;
    for (int c = 0; c < game::addr::kMaxCities; ++c) {
        const int d = game::CityDistance(c);
        if (d < 0 || d >= bestDist) continue;
        bestDist = d; best = c;
    }
    if (best < 0 || best == h.aimCity) return;
    game::SetShipDestCityRaw(h.slot, best);
    h.aimCity = best;
}

inline void EndHunt(Hunt& h, const char* why)
{
    if (!h.active) return;
    Log("suspicion: the %s hunter breaks off -- %s",
        game::NationName(h.nation), why);
    h.active = false; h.slot = -1; h.aimCity = -1;
}

inline void TickHunts()
{
    const DWORD now = GetTickCount();
    for (int n = 0; n < game::addr::kNationsWithRank; ++n)
    for (int k = 0; k < kMaxHuntsPerNation; ++k) {
        Hunt& h = g_hunts[n][k];
        if (!h.active) continue;

        // She may have been sunk, despawned, or had her slot reused.
        if (game::ShipType(h.slot) == -1) { EndHunt(h, "her ship is gone"); continue; }

        if ((int)(now - h.startedAt) > g_tune.hunterGiveUpMs) {
            EndHunt(h, "she has searched long enough"); continue;
        }

        const int px = game::PlayerX() / 1000, py = game::PlayerY() / 1000;
        const int hx = *(const int*)(game::ShipRecord(h.slot) + 0x0C) / 1000;
        const int hy = *(const int*)(game::ShipRecord(h.slot) + 0x10) / 1000;
        if (Octagonal(hx - px, hy - py) > g_tune.hunterEscapeDist) {
            EndHunt(h, "we are clear away"); continue;
        }

        if ((int)(now - h.lastAimAt) >= g_tune.hunterReaimMs) {
            h.lastAimAt = now;
            ReaimHunter(h);
        }
    }
}

// ---------------------------------------------------------------- the ladder
// What the player is told, and what it costs. Everything below the top rung is
// text; the top rung writes reputation and lets the game react on its own.
//
// Notices are posted through content.h by the caller, which owns the include
// order -- suspicion stays free of it so it can be tested without the renderer.
inline const char* g_pendingNotice = nullptr;
inline bool        g_pendingIsBeat = false;   // narrative rather than status
inline char        g_noticeBuf[192];

inline void Say(const char* text, bool beat)
{
    strncpy_s(g_noticeBuf, sizeof(g_noticeBuf), text, _TRUNCATE);
    g_pendingNotice = g_noticeBuf;
    g_pendingIsBeat = beat;
}

// The flag layer owns the player's colours, so unmasking ASKS for them back
// rather than reaching over and setting them. Read and cleared at the safe
// point by the caller.
inline bool g_pendingStrikeColours = false;

// Unmasked. One number, and the world does the rest.
inline void Unmask(int nation)
{
    // How hard the lie lands depends on how close you were when it came apart.
    // Being seen through under the guns of their own harbour is a different
    // matter from being doubted at the horizon, and a captain would expect
    // that. Full penalty inside closeRange, half of it at the edge of sight.
    const Look look = Observe(nation);
    int penalty = g_tune.repPenalty;
    if (look.nearest > g_tune.closeRange && look.port < 0) penalty = (penalty + 1) / 2;

    const int before = nations::Reputation(nation);
    const int after  = before - penalty;
    game::SetReputationRaw(nation, after);

    Log("suspicion: UNMASKED by the %s -- reputation %d -> %d (penalty %d, "
        "nearest of theirs %d) -- %s", game::NationName(nation), before, after,
        penalty, look.nearest,
        after < 0 ? "their ports are now closed to us"
                  : "still tolerated, but barely");

    // The ruse is over, so END it. Saying "colours struck" and then leaving the
    // false flag flying is what made the meter reset to zero and climb again
    // on the spot, over and over, with the player never told why.
    g_sus[nation].level = 0;
    g_sus[nation].carry = 0;
    g_sus[nation].spokenAt = 0;
    g_sus[nation].hasHeat = false;
    g_wearing = -1;
    g_pendingStrikeColours = true;

    DispatchHunters(nation);

    char msg[192];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                "The %s see through it. Colours struck!",
                game::NationName(nation));
    Say(msg, true);
}

// One evaluation. `dtMs` is real time since the last call.
inline void Tick(DWORD dtMs)
{
    TickHunts();

    if (!state::InGame()) return;
    if (dtMs == 0 || dtMs > 5000) return;      // paused, loading, or first tick

    // Honest sailing is not suspicious. Flying your own colours, or an open
    // pirate flag, is not a lie -- the game already makes people hostile for
    // what you DO, and that machinery is not ours to duplicate.
    if (g_wearing < 0 || g_wearing >= game::addr::kNationsWithRank) {
        for (int n = 0; n < game::addr::kNationsWithRank; ++n) {
            NationSuspicion& s = g_sus[n];
            if (s.level <= 0) { s.carry = 0; continue; }
            s.carry -= g_tune.decayClear * (int)dtMs;
            const int w = s.carry / 1000;
            if (w != 0) { s.level += w; s.carry -= w * 1000; }
            if (s.level < 0) { s.level = 0; s.carry = 0; }
            s.lastRate = -g_tune.decayClear;
        }
        return;
    }

    const int nation = g_wearing;
    NationSuspicion& s = g_sus[nation];

    const Look look = Observe(nation);
    const int  rate = RateFor(nation, look);
    s.lastRate = rate;

    const int before = s.level;
    s.carry += rate * (int)dtMs;
    const int whole = s.carry / 1000;
    if (whole != 0) { s.level += whole; s.carry -= whole * 1000; }
    if (s.level < 0)   { s.level = 0;   s.carry = 0; }
    if (s.level > 100) { s.level = 100; s.carry = 0; }

    // Remember WHERE this was earned, so leaving the area cools it faster.
    if (rate > 0) {
        s.heatX = game::PlayerX() / 1000;
        s.heatY = game::PlayerY() / 1000;
        s.hasHeat = true;
    } else if (s.level == 0) {
        s.hasHeat = false;
    }

    // Rungs. Each is spoken once on the way up; falling back below one arms it
    // again, so a long chase can build twice without nagging on every frame.
    const int lvl = s.level;
    if (lvl < s.spokenAt) s.spokenAt = 0;

    if (lvl >= g_tune.threshUnmask) {
        Unmask(nation);
    } else if (lvl >= g_tune.threshChallenge && s.spokenAt < g_tune.threshChallenge) {
        s.spokenAt = g_tune.threshChallenge;
        char m[192];
        _snprintf_s(m, sizeof(m), _TRUNCATE,
                    "She's coming about. They don't believe you.");
        Say(m, true);
    } else if (lvl >= g_tune.threshWarning && s.spokenAt < g_tune.threshWarning) {
        s.spokenAt = g_tune.threshWarning;
        char m[192];
        _snprintf_s(m, sizeof(m), _TRUNCATE,
                    "A %s captain is studying our rigging.",
                    game::NationName(nation));
        Say(m, true);
    } else if (lvl >= g_tune.threshNotice && s.spokenAt < g_tune.threshNotice) {
        s.spokenAt = g_tune.threshNotice;
        char m[192];
        _snprintf_s(m, sizeof(m), _TRUNCATE,
                    "They're signalling for our colours.");
        Say(m, true);
    }

    (void)before;
}

// ------------------------------------------------------------------ the panel
// Top right, where the game parks nothing. Drawn from the render phase, so it
// composes no text and allocates nothing -- the strings are built at the safe
// point by RefreshPanel() below.
inline char g_panel[4][96];
inline int  g_panelLines = 0;

inline void RefreshPanel()
{
    g_panelLines = 0;
    if (!state::InGame()) return;
    if (g_wearing < 0 || g_wearing >= game::addr::kNationsWithRank) return;

    // Shown for as long as we are wearing someone else's flag, even at zero.
    // Hiding it whenever nothing was happening made it look broken -- it
    // vanished and, since the level could never rise, never came back.
    const NationSuspicion& s = g_sus[g_wearing];

    // TWO LINES, and only what a captain would want at a glance: whose flag we
    // are wearing, and how far the lie has got. The first version stacked four
    // lines of instrumentation into the corner and read as clutter over the
    // sea -- the rate and the hunter count are diagnostics, and they belong in
    // the log, not on the horizon.
    int hunters = 0;
    for (int k = 0; k < kMaxHuntsPerNation; ++k)
        if (g_hunts[g_wearing][k].active) ++hunters;

    char bar[13];
    const int filled = (s.level * 12) / 100;
    for (int i = 0; i < 12; ++i) bar[i] = (i < filled) ? '|' : '.';
    bar[12] = 0;

    _snprintf_s(g_panel[g_panelLines++], 96, _TRUNCATE, "%s colours",
                game::NationName(g_wearing));

    // The watching mark is one character rather than a sentence: a dot when
    // nobody is looking, an eye-ish glyph when they are.
    _snprintf_s(g_panel[g_panelLines++], 96, _TRUNCATE, "%s %s",
                bar, s.lastRate > 0 ? "<" : " ");

    if (hunters > 0) {
        _snprintf_s(g_panel[g_panelLines++], 96, _TRUNCATE,
                    "%d hunting", hunters);
    }
}

}  // namespace suspicion
