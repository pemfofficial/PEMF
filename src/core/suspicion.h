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
// The one thing we build ourselves is the hunter. The CHASE is the game's own:
// a ship whose purpose is 1 (pirate-hunter) steers for the player every tick
// (FUN_00467F90, see game.h "the pirate-hunter chase"). The game also leashes
// every hunter to her home port and calls her off past it; PEMF lifts that
// leash for its own hunters only, through one redirected call, and decides
// itself when a hunt is over. Strength comes from the engine's own formula
// (2 - reputation/5, clamped 2..4), so a crown that loathes you sends worse.
#pragma once
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>   // rand
#include <stdarg.h>

#include "log.h"
#include "officerfx.h"
#include "game.h"
#include "state.h"
#include "nations.h"
#include "standing.h"
#include "render.h"   // RedirectCall

// Referenced by the leash stub's inline assembly, so file scope, C linkage.
extern "C" {
unsigned char g_pemfHunterSlot[256] = {0};   // 1 = a hunter of ours
void*         g_pemfLeashOrig = nullptr;     // FUN_0045E6B0
}

namespace suspicion {

// ------------------------------------------------------------------- tuning
// Everything a play session might want to argue about lives here and is
// reloadable from PEMF\suspicion.ini. Defaults are a starting point, not a
// balance pass -- this system will be tuned by playing it.
struct Tuning {
    // How fast suspicion rises, in TENTHS of a point per second (V2, 2026-10-17:
    // the first version used whole points and every crown unmasked a captain in
    // seconds). A watcher counts for nothing at the edge of its range, once at
    // mid range, twice alongside; at most `maxWatchers` count at once.
    int rateShip     = 5;     // per watcher of theirs, mid range
    int ratePort     = 4;     // their settlement, mid range
    int rateClose    = 8;     // per watcher inside closeRange (a proper look)
    int rateInfamy   = 3;     // added while they already have cause to know you
    int maxWatchers  = 3;
    int enemyPercent = 60;    // enemy colours rise at this share of a disguise's rate
    int piratePercent = 70;   // the black flag likewise

    int decayWatched   = 0;    // points/s while still under observation
    int decayClear     = 2;    // nobody in sight
    int decayFarAway   = 6;    // and well clear of where it was earned

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

    // An aggressive act -- the game itself taking reputation for something we
    // did -- is what turns suspicion into certainty: the crown that suffered it
    // jumps by this much at once.
    int aggressionSpike = 35;

    // At the top of the meter the nearest ship of that crown falls in astern and
    // TRAILS us. She backs off once we have done nothing aggressive for this
    // long, or after `trailMaxSec` whatever happens.
    int trailBackOffSec = 90;
    int trailMaxSec     = 300;

    // What being unmasked costs (a disguise seen through by an act). Through the
    // ledger, which forgives it if nothing more comes of it.
    int repPenalty = 6;

    // Hunters: only for a crown we are below zero with. How many depends on how
    // badly they think of us and how notorious we are; they come one at a time.
    int  hunterReaimMs    = 5000;   // how often the log reports on her
    int  hunterGiveUpMs   = 240000; // ...before losing interest
    int  hunterEscapeDist = 70000;  // or you simply get clear
    int  hunterMaxSpawnDist = 9000; // past this she is culled before arriving
    bool hunterEnabled    = true;
    int  maxPerNation     = 3;      // ceiling per crown
    int  maxHuntersTotal  = 3;      // ceiling across all crowns
    int  huntIntervalSec  = 180;    // a crown sends at most one ship this often
    int  huntCooldownSec  = 240;    // after one of its hunts ends
    int  harbourWaitSec   = 30;     // hunters give up after we sit in port this long

    int enemySeesHonestFlag = 1;    // 0: only FALSE colours draw enemy suspicion

    // A new career serves no crown and flies the black flag until a governor
    // grants a letter of marque (core.cpp, ApplyCareerFlag).
    int startUnaligned = 1;
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
    //
    // Since the distance falloff the rate is in THOUSANDTHS of a point per
    // second, so the carry is in point-microseconds and needs 64 bits.
    long long carry = 0;

    // What the meter is currently about -- deceit, enemy colours, the black
    // flag -- so the top rung knows what being found out means.
    int   why = 0;

    // Chose to RUN at the challenge: they know something is wrong, so there is
    // no talking our way out of it, but out of sight the trail goes cold as fast
    // as if we were already far away. Cleared at zero or when unmasked.
    bool  running = false;
};

inline NationSuspicion g_sus[game::addr::kNationsWithRank];

struct Hunt {
    bool  active   = false;
    int   slot     = -1;
    int   nation   = -1;
    int   strength = 0;
    DWORD startedAt = 0;
    DWORD lastAimAt = 0;
    int   aimCity  = -1;    // where she was bound when she left harbour (+0x40)
    // The game can still call her off (her purpose leaves 1). PEMF puts her
    // back; one the game keeps calling off is let go. See HoldChase.
    int   rearms   = 0;
    DWORD firstRearmAt = 0;
    // The port that sent her. Recorded so the flag PEMF sets on that port's
    // record can be taken back when the hunt ends -- without this there was no
    // way to know which city to clear, and so it was never cleared at all.
    int   fromCity = -1;
    // Where she was at the previous re-aim, so the log can say whether the
    // re-issued order actually kept her under way or she is sitting in harbour.
    int   lastX = 0, lastY = 0;
    bool  havePos = false;
    int   lastDist = -1;   // so the log can say whether she is CLOSING
};

// Several per nation, because a crown that truly wants you does not send one
// ship. How many actually sail is decided by reputation at the moment of
// dispatch -- see HunterCountFor().
constexpr int kMaxHuntsPerNation = 4;
inline Hunt g_hunts[game::addr::kNationsWithRank][kMaxHuntsPerNation];

// The ship of each crown trailing us, if any (see "the trail").
struct Trail {
    bool  active = false;
    int   slot = -1;
    short purpose = 0;       // hers before we borrowed her
    DWORD startedAt = 0;
};
inline Trail g_trail[game::addr::kNationsWithRank];
inline DWORD g_lastAggression[game::addr::kNationsWithRank] = {0};
// When each crown may next send a hunter.
inline DWORD g_nextHuntAt[game::addr::kNationsWithRank] = {0};

// The nation whose colours we are currently wearing, or -1 for honest sailing.
// Set by the flag layer; suspicion does not guess it.
inline int g_wearing = -1;

inline void SetWearing(int nation) { g_wearing = nation; }

// What is actually on the mast, honest or not, as the world sees it: the crown
// whose flag it is (-1 for none -- a personal device), or the black flag.
inline int  g_flown     = -1;
inline bool g_blackFlag = false;

inline void SetFlown(int nation, bool black) { g_flown = black ? -1 : nation; g_blackFlag = black; }

// Does this crown still have someone out looking for us? Handed to standing.h,
// which must not forgive a fright while the warship it caused is still at sea --
// see the guard in its Tick(). Installed rather than included because standing.h
// comes first and the reverse include would be a cycle.
inline bool HasHunterAtSea(int nation)
{
    if (nation < 0 || nation >= game::addr::kNationsWithRank) return false;
    for (int k = 0; k < kMaxHuntsPerNation; ++k)
        if (g_hunts[nation][k].active) return true;
    return false;
}

inline void ResetAll()
{
    for (int n = 0; n < game::addr::kNationsWithRank; ++n) {
        g_sus[n] = NationSuspicion{};
        for (int k = 0; k < kMaxHuntsPerNation; ++k) g_hunts[n][k] = Hunt{};
    }
    g_wearing = -1;
    for (int n = 0; n < game::addr::kNationsWithRank; ++n) {
        g_trail[n] = Trail{};
        g_lastAggression[n] = 0;
        g_nextHuntAt[n] = 0;
    }
    // The leash marks go with the hunts: a slot the game reuses for a new ship
    // must not inherit our hunter's unlimited reach.
    memset(g_pemfHunterSlot, 0, sizeof(g_pemfHunterSlot));

    // Careers end and saves get loaded, and the hunts we were tracking go with
    // them -- but the flags we set on the CITY records do not, because they live
    // in the game's memory rather than ours. Clearing every one is always right:
    // the bit is ours to set and ours to take back, and after this reset there
    // is no hunt of ours at sea anywhere.
    const int cleared = game::ClearAllCitySentHunter();
    if (cleared)
        Log("suspicion: cleared the hunter flag from %d port record(s)", cleared);
}

// How many ships a crown wants at sea after us. None unless we are below zero
// with them; more the worse it gets and the more notorious we are -- the
// ledger's notoriety is what we have actually done to them.
inline int HunterCountFor(int nation)
{
    const int rep = nations::Reputation(nation);
    if (rep >= 0) return 0;
    int n = 1;
    if (rep <= -20) n = 2;
    if (rep <= -40) n = 3;
    if (nation >= 0 && nation < standing::kNations && standing::g_led[nation].notoriety >= 30) ++n;
    if (n > g_tune.maxPerNation) n = g_tune.maxPerNation;
    if (n > kMaxHuntsPerNation)  n = kMaxHuntsPerNation;
    return n;
}

// standing.h calls this; defined below, after the ladder it needs.
inline void OnAggression(int victim, int severity);
inline void OnAggressionHook(int victim, int severity) { OnAggression(victim, severity); }

// ------------------------------------------------------------------ tuning io
inline void LoadTuning(const char* gameDir)
{
    // Earliest point suspicion is guaranteed to run, and standing.h only reads
    // the hook inside its Tick, so installing it here is in time.
    standing::g_debtHeldBy = &HasHunterAtSea;
    standing::g_onAggression = &OnAggressionHook;

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
            // V2 names: the rates are in tenths now, so the old rise* keys of
            // an older suspicion.ini are deliberately NOT read.
            { "rateShip",       &g_tune.rateShip },
            { "ratePort",       &g_tune.ratePort },
            { "rateClose",      &g_tune.rateClose },
            { "rateInfamy",     &g_tune.rateInfamy },
            { "maxWatchers",    &g_tune.maxWatchers },
            { "enemyPercent",   &g_tune.enemyPercent },
            { "piratePercent",  &g_tune.piratePercent },
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
            { "aggressionSpike",&g_tune.aggressionSpike },
            { "trailBackOffSec",&g_tune.trailBackOffSec },
            { "trailMaxSec",    &g_tune.trailMaxSec },
            { "unmaskPenalty",  &g_tune.repPenalty },
            { "hunterReaimMs",  &g_tune.hunterReaimMs },
            { "hunterGiveUpMs", &g_tune.hunterGiveUpMs },
            { "hunterEscapeDist", &g_tune.hunterEscapeDist },
            { "hunterMaxSpawnDist", &g_tune.hunterMaxSpawnDist },
            { "maxPerNation",   &g_tune.maxPerNation },
            { "maxHuntersTotal",&g_tune.maxHuntersTotal },
            { "huntIntervalSec",&g_tune.huntIntervalSec },
            { "huntCooldownSec",&g_tune.huntCooldownSec },
            { "harbourWaitSec", &g_tune.harbourWaitSec },
            { "enemySeesHonestFlag", &g_tune.enemySeesHonestFlag },
            { "startUnaligned", &g_tune.startUnaligned },
            // The standing ledger lives in its own header but tunes from the
            // same file -- one place a player edits, not two.
            { "debtForgetMonths",  &standing::kDebtForgetMonths },
            { "debtCap",           &standing::kDebtCap },
            { "notorietyCap",      &standing::kNotorietyCap },
            { "actNotorietyShare", &standing::kActNotorietyShare },
            { "actSpillPercent",   &standing::kActSpillPercent },
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
// Who can see us, and how hard they are looking. Called once per nation per
// safe point.
//
// DISTANCE COUNTS. A ship at the edge of sight barely registers; one alongside
// counts double. Each watcher weighs 2 * (range - d) / range -- zero at the
// edge, two at point blank, one on average -- so the old per-ship rates still
// read as "about this much per ship" while closing the range now matters all the
// way in, not only inside closeRange.
struct Look {
    int ships = 0;      // vessels of this nation within shipRange
    // ⛔ THEIR vessels inside closeRange -- NOT "any vessel", which is what this
    // counted until 0.2.7 and is the single worst bug the mechanic has had.
    //
    // Observe() is called once PER NATION and its result drives THAT nation's
    // meter. Counting any hull at all therefore meant one unrelated merchant
    // sailing past raised the Spanish, English, French, Dutch and Pirate meters
    // at once, each by riseCloseRange -- the heaviest term there is.
    //
    // A ship that cannot report to them cannot help them see through you.
    int close = 0;      // vessels of this nation inside closeRange
    int shipWeight = 0; // sum of watcher weights, in thousandths
    int port  = -1;     // their settlement within portRange, or -1
    int portDist = 0;
    int portWeight = 0; // its weight, in thousandths
    int nearest  = 0;   // closest vessel of theirs, for the panel
    int nearestSlot = -1;
};

// The most recent Observe() per nation, kept so the panel can say what is
// actually driving the meter. "Something is watching you" with nothing visible
// on screen reads as a bug; "their harbour" or "2 sail" reads as a situation.
inline Look g_looks[game::addr::kNationsWithRank];

inline int Weight(int d, int range)
{
    if (range <= 0 || d >= range) return 0;
    if (d < 0) d = 0;
    return (int)((2000LL * (range - d)) / range);
}

inline Look Observe(int nation)
{
    Look look;
    look.nearest = 0x7FFFFFFF;

    const int px = game::PlayerX() / 1000;
    const int py = game::PlayerY() / 1000;

    // THE WHOLE SEA, not the first 24 slots. The ship array is 256 slots, and
    // scanning only 24 of them is why a disguise could be tested by a PORT but
    // almost never by a SHIP. Observed in a playtest as sailing right alongside
    // a Dutch West Indiaman while the meter sat at zero.
    //
    // ⛔ FROM SLOT 8. Slots 1-7 are the player's OWN FLEET (GAME_API.md, slot
    // allocation), and a prize keeps the nationality it was taken under -- so a
    // captured Dutch ship sailing with us counted as a Dutch watcher at point
    // blank, everywhere we went. Reported from play as "the meter keeps rising
    // no matter where I am", and it did.
    for (int i = 8; i < game::addr::kMaxShips; ++i) {
        const int type = game::ShipType(i);
        if (type == -1) continue;
        if (game::ShipNationality(i) != nation) continue;   // THEIRS only
        const int x = *(const int*)(game::ShipRecord(i) + 0x0C) / 1000;
        const int y = *(const int*)(game::ShipRecord(i) + 0x10) / 1000;
        if (x == 0 && y == 0) continue;

        const int d = Octagonal(x - px, y - py);
        if (d <= g_tune.shipRange) { ++look.ships; look.shipWeight += Weight(d, g_tune.shipRange); }
        if (d <= g_tune.closeRange) ++look.close;
        if (d < look.nearest) { look.nearest = d; look.nearestSlot = i; }
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
    look.portWeight = (look.port >= 0) ? Weight(bestDist, g_tune.portRange) : 0;
    return look;
}

// What, if anything, this crown has to be suspicious ABOUT.
enum class Why { None, Deceit, Enemy, Piracy };

// How a crown regards us decides what it makes of our colours (V2.1, owner,
// 2026-10-17: "it should depend on the relationship with the faction"):
//
//   the black flag            every crown is suspicious
//   their own colours         commissioned (a letter of marque from them): a
//                             friend, no suspicion at all; otherwise a disguise
//   an enemy crown's colours  suspicious only if they already think badly of us
//                             (reputation below zero); otherwise they know us
//   anything else             nothing to be suspicious of
//
// How fast it rises then depends on that same opinion (RelationPercent), and an
// aggressive act against them is what really changes their mind.
inline Why WhyFor(int nation)
{
    if (g_blackFlag) return Why::Piracy;
    if (g_flown == nation) return (g_wearing == nation) ? Why::Deceit : Why::None;
    if (g_flown >= 0 && nations::AtWar(nation, g_flown) && nations::Reputation(nation) < 0)
        return Why::Enemy;
    return Why::None;
}

// The better they think of us, the slower they grow suspicious: 100% at a
// reputation of 0, down to 40% when they like us, up to 250% when they loathe us.
inline int RelationPercent(int nation)
{
    const int rep = nations::Reputation(nation);
    if (rep >= 0) { const int p = 100 - rep * 4; return p < 40 ? 40 : p; }
    const int p = 100 + (-rep) * 8;
    return p > 250 ? 250 : p;
}

inline const char* WhyName(Why w)
{
    return w == Why::Deceit ? "deceit" : w == Why::Enemy ? "enemy colours"
         : w == Why::Piracy ? "the black flag" : "none";
}

// The rate, in thousandths of a point per second. Positive means they are
// working it out. SLOW by design: watching alone builds it over minutes; what
// makes it jump is an aggressive act (OnAggression).
inline int RateMilli(int nation, Why why, const Look& look)
{
    const int cap = g_tune.maxWatchers * 1000;
    const int ships = look.shipWeight < cap ? look.shipWeight : cap;
    const int close = look.close < g_tune.maxWatchers ? look.close : g_tune.maxWatchers;

    // tenths/s * thousandths-of-a-watcher / 10 = thousandths of a point/s
    int rate = ships * g_tune.rateShip / 10 + look.portWeight * g_tune.ratePort / 10 +
               close * g_tune.rateClose * 100;
    if (why == Why::Enemy)  rate = rate * g_tune.enemyPercent / 100;
    if (why == Why::Piracy) rate = rate * g_tune.piratePercent / 100;
    rate = rate * RelationPercent(nation) / 100;

    // An officer who keeps his mouth shut buys you time under false colours;
    // one who cannot costs you it. Only a LIE is helped by discretion.
    if (why == Why::Deceit && officerfx::g_discretion != 0)
        rate = officerfx::ApplyPercent(rate, -officerfx::g_discretion);

    // A captain they already have reason to know is harder to pass off.
    if (rate > 0 && nations::Reputation(nation) < 0) rate += g_tune.rateInfamy * 100;

    if (rate > 0) return rate - g_tune.decayWatched * 1000;

    // Nobody watching. How fast it cools depends on whether we are still in the
    // water where it was earned -- or whether we ran for it.
    const NationSuspicion& s = g_sus[nation];
    if (s.running) return -g_tune.decayFarAway * 1000;
    if (s.hasHeat) {
        const int px = game::PlayerX() / 1000, py = game::PlayerY() / 1000;
        if (Octagonal(px - s.heatX, py - s.heatY) > g_tune.heatRadius)
            return -g_tune.decayFarAway * 1000;
    }
    return -g_tune.decayClear * 1000;
}

// ------------------------------------------------------------------ hunters
// Sending one, and keeping her on us.
// Distance from an arbitrary point to a city, in the same units CityDistance
// uses. The stock helper measures from the PLAYER, which is exactly the thing
// the probe must not assume.
inline int CityDistanceFrom(int cityIndex, int x, int y)
{
    if (cityIndex < 0) return -1;
    const int* pos = (const int*)(game::addr::CityPositions + (size_t)cityIndex * 16);
    int dx = pos[0] - x, dy = pos[1] - y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return ((dy < dx) ? (dy + dx * 2) : (dx + dy * 2)) / 2;
}

inline void Say(const char* text, bool beat);              // the ladder's, below

// THE LEASH. Decompiled 2026-10-16 (0x0046AECC): every tick a pirate-hunter is
// still a hunter only while she is within reach of her home port,
//
//     FUN_00405D90(home) <= FUN_0045E6B0(home)       ; distance <= reach
//
// and past it the game makes her a raider and sends her home. Reach is short --
// a couple of dozen map cells -- which is why hunters seemed to lose interest,
// and why the old "aim her at a port beyond the player" trick appeared to work:
// that order moved her home port along behind her, so she never left its reach.
//
// PEMF's hunters, and only PEMF's, are given unlimited reach. The call to the
// reach function at 0x0046AEF0 -- one of its two callers -- goes through the
// stub below, which answers "unlimited" for a slot PEMF marked as its own and
// asks the game for every other ship. Stock pirate-hunters are unchanged.
} // namespace suspicion

extern "C" {

// Same contract as FUN_0045E6B0: eax = city in, reach out in eax. At its one
// redirected call site EBP holds slot * 0x45C (the code reads [ebp+0x814336]
// for this ship's home port two instructions earlier). ECX is dead there -- the
// caller reloads it straight after -- but it is preserved anyway.
__declspec(naked) void PemfHunterLeashStub(void)
{
    __asm {
        push eax
        push ecx
        push edx
        mov  eax, ebp
        xor  edx, edx
        mov  ecx, 0x45C
        div  ecx                    // eax = slot, edx = remainder
        test edx, edx
        jnz  stock
        cmp  eax, 256
        jae  stock
        cmp  byte ptr g_pemfHunterSlot[eax], 0
        je   stock
        pop  edx
        pop  ecx
        pop  eax
        mov  eax, 0x7FFFFFFF        // ours: no leash
        ret
    stock:
        pop  edx
        pop  ecx
        pop  eax
        jmp  dword ptr [g_pemfLeashOrig]
    }
}

} // extern "C"

namespace suspicion {

constexpr uintptr_t kLeashCall   = 0x0046AEF0;   // E8 -> kLeashReach
constexpr uintptr_t kLeashReach  = 0x0045E6B0;

// ⛔ First safe point only (a code write).
inline bool InstallLeash()
{
    if (g_pemfLeashOrig) return true;
    const uintptr_t s = kLeashCall;
    __try {
        if (*(const uint8_t*)s != 0xE8 || s + 5 + *(const int32_t*)(s + 1) != kLeashReach) {
            Log("suspicion: hunter leash call at 0x%08X is not what we expect -- "
                "our hunters keep the stock leash", (unsigned)s);
            return false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    void* prev = nullptr;
    if (!render::RedirectCall(s, (void*)&PemfHunterLeashStub, &prev)) return false;
    g_pemfLeashOrig = prev;
    Log("suspicion: hunter leash lifted for PEMF's own hunters (call at 0x%08X)", (unsigned)s);
    return true;
}

inline void RestoreLeash()
{
    if (!g_pemfLeashOrig) return;
    render::RedirectCall(kLeashCall, g_pemfLeashOrig, nullptr);
    g_pemfLeashOrig = nullptr;
}

inline void MarkOurs(int slot, bool ours)
{
    if (slot >= 0 && slot < 256) g_pemfHunterSlot[slot] = ours ? 1 : 0;
}

inline int ShipX(int slot) { return *(const int*)(game::ShipRecord(slot) + 0x0C) / 1000; }
inline int ShipY(int slot) { return *(const int*)(game::ShipRecord(slot) + 0x10) / 1000; }

// Her heading, wanted (+0x44) and actual (+0x14), in degrees -- for the log, so
// it can show her turning onto us. Full circle = 2^32.
inline int HeadingDeg(int slot, int off)
{
    return (int)(((unsigned long long)*(const unsigned*)(game::ShipRecord(slot) + off) * 360ULL) >> 32);
}

inline bool DispatchOneHunter(int nation, Hunt& h)
{
    if (!game::SpawnShipCallable()) {
        Log("suspicion: cannot dispatch -- the ship factory did not verify");
        return false;
    }

    // From one of THEIR ports, so she comes from somewhere plausible -- and
    // not one another hunt already sailed from, so two never leave together
    // and sail on top of each other (their labels and hails piled up in play).
    int from = -1, bestDist = 0x7FFFFFFF;
    for (int c = 0; c < game::addr::kMaxCities; ++c) {
        if (game::CityNation(c) != nation) continue;
        bool used = false;
        for (int n = 0; n < game::addr::kNationsWithRank && !used; ++n)
            for (int k = 0; k < kMaxHuntsPerNation && !used; ++k)
                used = g_hunts[n][k].active && g_hunts[n][k].fromCity == c;
        if (used) continue;
        const int d = game::CityDistance(c);
        if (d < 0 || d >= bestDist) continue;
        bestDist = d; from = c;
    }
    if (from < 0) {
        Log("suspicion: %s has no port near enough to send anyone from",
            game::NationName(nation));
        return false;
    }

    // A ship built too far away is DEAD ON ARRIVAL. Measured during the chase
    // probe: every pursuer spawned within ~7,000 of the player lived for as long
    // as we watched her, and every one spawned beyond ~11,000 was culled inside
    // two seconds. The overworld does not keep vessels that far out.
    //
    // "Nearest port of that crown" can still be a long way off, so without this
    // a hunt silently produces nothing and the player just never sees anyone --
    // which is a much worse bug than declining to send one.
    if (bestDist > g_tune.hunterMaxSpawnDist) {
        Log("suspicion: %s wants to send someone but their nearest port (city "
            "%d) is %d away -- past %d she would be culled before reaching us",
            game::NationName(nation), from, bestDist, g_tune.hunterMaxSpawnDist);
        return false;
    }

    const int slot = game::SpawnShipAtCity(from, 0x0B);
    if (slot < 8 || slot >= game::addr::kMaxShips) {
        Log("suspicion: the yard could not build a hunter (returned %d)", slot);
        return false;
    }

    // The engine's own recipe: strength scales with how badly they think of us.
    const int strength = game::HunterStrengthFor(nation);
    game::SetShipRoleRaw(slot, strength);
    game::MarkCitySentHunter(from, slot);
    h.fromCity = from;          // so EndHunt can give the port its flag back

    h.active = true; h.slot = slot; h.nation = nation;
    h.strength = strength;
    h.startedAt = h.lastAimAt = GetTickCount();
    h.lastDist = -1; h.havePos = false;
    h.rearms = 0; h.firstRearmAt = 0;

    // Ours BEFORE she is a hunter, so the very first sailing tick that asks the
    // leash question gets our answer.
    MarkOurs(slot, true);
    game::SetShipPurposeRaw(slot, game::kPurposePirateHunter);

    // Get her out of harbour. A ship in port stays docked while the port she is
    // bound for (+0x40) is the port she returns to (+0x3E), and the factory
    // leaves them in step. Bound anywhere else she sails -- and as a hunter the
    // destination is then ignored in favour of us. Her home stays the port that
    // sent her, which is where she goes when the hunt is over.
    game::SetShipShortRaw(slot, game::kShipReturnPort, from);
    int bound = -1, boundDist = 0x7FFFFFFF;
    for (int c = 0; c < game::addr::kMaxCities; ++c) {
        if (c == from) continue;
        const int d = game::CityDistance(c);
        if (d < 0 || d >= boundDist) continue;
        boundDist = d; bound = c;
    }
    if (bound >= 0) game::SetShipShortRaw(slot, game::kShipBoundFor, bound);
    h.aimCity = bound;

    Log("suspicion: %s dispatches a pirate-hunter from city %d (dist %d) -- "
        "slot %d, strength %d, leash %s (reputation %d)",
        game::NationName(nation), from, bestDist, slot, strength,
        g_pemfLeashOrig ? "lifted" : "STOCK -- she will give up near home",
        nations::Reputation(nation));
    return true;
}

// When each crown may next send someone, and when the harbour wait began.
inline DWORD g_stillSince = 0;
inline int   g_stillX = 0, g_stillY = 0;

inline int HuntersAtSea(int nation)
{
    int have = 0;
    for (int k = 0; k < kMaxHuntsPerNation; ++k) if (g_hunts[nation][k].active) ++have;
    return have;
}

inline int HuntersAtSeaTotal()
{
    int t = 0;
    for (int n = 0; n < game::addr::kNationsWithRank; ++n) t += HuntersAtSea(n);
    return t;
}

// ONE more ship, if this crown wants one and may send it now.
inline void DispatchHunters(int nation)
{
    if (!g_tune.hunterEnabled) return;
    if (nation < 0 || nation >= game::addr::kNationsWithRank) return;
    const DWORD now = GetTickCount();
    if (g_nextHuntAt[nation] && (int)(now - g_nextHuntAt[nation]) < 0) return;
    const int want = HunterCountFor(nation);
    const int have = HuntersAtSea(nation);
    if (have >= want || HuntersAtSeaTotal() >= g_tune.maxHuntersTotal) return;

    for (int k = 0; k < kMaxHuntsPerNation; ++k) {
        if (g_hunts[nation][k].active) continue;
        if (DispatchOneHunter(nation, g_hunts[nation][k])) {
            g_nextHuntAt[nation] = now + (DWORD)g_tune.huntIntervalSec * 1000;
            Log("suspicion: %s now has %d hunter(s) at sea (wants %d, reputation %d, "
                "notoriety %d)", game::NationName(nation), have + 1, want,
                nations::Reputation(nation), standing::g_led[nation].notoriety);
        }
        break;
    }
}

// Ending a hunt hands her back to the game the way the game ends its own:
// a raider (purpose 3) going home (flag 0x80) to the port that sent her --
// what FUN_0045B740 does when a stock hunter gives up.
inline void EndHunt(Hunt& h, const char* why, bool tellPlayer = true)
{
    if (!h.active) return;
    Log("suspicion: the %s hunter breaks off -- %s",
        game::NationName(h.nation), why);

    // TELL THE PLAYER. A hunt that ends in silence is indistinguishable from a
    // hunt that is still coming, and the whole point of an escape is knowing you
    // made it. Only worth saying for the endings the player earned -- a ship
    // that sank announces itself.
    if (tellPlayer) {
        char line[160];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "The %s hunter breaks off -- %s.",
                    game::NationName(h.nation), why);
        Say(line, true);
    }

    MarkOurs(h.slot, false);
    if (game::ShipType(h.slot) != -1 &&
        game::ShipShort(h.slot, game::kShipPurpose) == game::kPurposePirateHunter) {
        game::SetShipPurposeRaw(h.slot, 3);
        game::OrShipFlagsRaw(h.slot, game::kShipFlagGoingHome);
        if (h.fromCity >= 0) game::SetShipShortRaw(h.slot, game::kShipReturnPort, h.fromCity);
    }

    // Give the port its record back. The hunt is over however it ended, so the
    // flag saying "this port has one at sea" is no longer true -- and leaving it
    // set was PEMF quietly editing the game's city table for the rest of the
    // session.
    if (h.fromCity >= 0) game::ClearCitySentHunter(h.fromCity);

    // Escaping is worth something: the crown waits before sending again.
    if (h.nation >= 0 && h.nation < game::addr::kNationsWithRank)
        g_nextHuntAt[h.nation] = GetTickCount() + (DWORD)g_tune.huntCooldownSec * 1000;

    h.active = false; h.slot = -1; h.aimCity = -1; h.lastDist = -1;
    h.fromCity = -1;
}

// Keep her a hunter. With the leash lifted the game has two other ways to call
// her off (0x0046AEFD): view flag 0x400, and a damage word it reads from the
// PLAYER's record. Both turn her into a raider heading home. PEMF owns when its
// hunts end, so it puts her back -- but a hunter the game calls off three times
// in ten seconds is let go, because something it knows about and we do not
// (the player in port, say) is holding.
inline bool HoldChase(Hunt& h)
{
    if (game::ShipShort(h.slot, game::kShipPurpose) == game::kPurposePirateHunter) return true;

    const DWORD now = GetTickCount();
    if (h.rearms == 0 || (int)(now - h.firstRearmAt) > 10000) {
        h.rearms = 0;
        h.firstRearmAt = now;
    }
    if (++h.rearms > 3) return false;

    game::SetShipPurposeRaw(h.slot, game::kPurposePirateHunter);
    game::ClearShipFlagsRaw(h.slot, game::kShipFlagGoingHome);
    if (h.fromCity >= 0) game::SetShipShortRaw(h.slot, game::kShipReturnPort, h.fromCity);
    unsigned view = 0;
    __try { view = *(const unsigned*)game::addr::ViewFlags; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    Log("suspicion: the game called the %s hunter (slot %d) off -- put back "
        "(%d in 10 s, view flags 0x%X)", game::NationName(h.nation), h.slot, h.rearms, view);
    return true;
}

// A crown that wants you keeps wanting you -- but sends its ships one at a time,
// spaced out, and only while we are below zero with it.
inline DWORD g_lastRetryAt = 0;
constexpr int kDispatchRetryMs = 15000;

inline void RetryDispatches()
{
    const DWORD now = GetTickCount();
    if ((int)(now - g_lastRetryAt) < kDispatchRetryMs) return;
    g_lastRetryAt = now;
    for (int n = 0; n < game::addr::kNationsWithRank; ++n)
        if (HunterCountFor(n) > HuntersAtSea(n)) DispatchHunters(n);
}

inline void TickHunts()
{
    const DWORD now = GetTickCount();

    // How long the player has sat still (in port, or paused) -- see below.
    {
        const int px = game::PlayerX() / 1000, py = game::PlayerY() / 1000;
        if (!g_stillSince || px != g_stillX || py != g_stillY) {
            g_stillX = px; g_stillY = py;
            g_stillSince = now;              // moved: the clock starts again
        }
    }
    RetryDispatches();
    for (int n = 0; n < game::addr::kNationsWithRank; ++n)
    for (int k = 0; k < kMaxHuntsPerNation; ++k) {
        Hunt& h = g_hunts[n][k];
        if (!h.active) continue;

        // Gone: sunk, taken, or culled. The overworld culls a ship that falls
        // far behind the player, so one that vanishes while well astern is one
        // we outran -- and the player has earned hearing it.
        if (game::ShipType(h.slot) == -1) {
            if (h.lastDist > g_tune.hunterMaxSpawnDist) EndHunt(h, "we've lost her");
            else EndHunt(h, "her ship is gone", false);
            continue;
        }

        if ((int)(now - h.startedAt) > g_tune.hunterGiveUpMs) {
            EndHunt(h, "she has searched long enough"); continue;
        }

        // THE WAY BACK. A hunt exists because a crown thinks you are a pirate,
        // so it has to end when they stop thinking so -- otherwise the standing
        // ledger can forgive a fright, their ports can reopen, and a warship is
        // still out there hunting a captain nobody wants any more.
        //
        // Reputation is the same number that opened the ports again, so the two
        // can never disagree.
        if (nations::Reputation(h.nation) >= 0) {
            EndHunt(h, "their quarrel with us is settled"); continue;
        }

        if (!HoldChase(h)) { EndHunt(h, "she turns for home"); continue; }

        // In port the game's chase target is the harbour itself, and a hunter
        // with no leash circles it -- seen in play. So if we sit in port, she
        // gives up waiting (and the crown's cooldown starts).
        if (g_stillSince && (int)(now - g_stillSince) > g_tune.harbourWaitSec * 1000) {
            int nearest = 0x7FFFFFFF;
            for (int c = 0; c < game::addr::kMaxCities; ++c) {
                const int d = game::CityDistance(c);
                if (d >= 0 && d < nearest) nearest = d;
            }
            if (nearest < 3500) { EndHunt(h, "she gives up waiting outside the harbour"); continue; }
        }

        const int px = game::PlayerX() / 1000, py = game::PlayerY() / 1000;
        const int hx = ShipX(h.slot), hy = ShipY(h.slot);
        const int dist = Octagonal(hx - px, hy - py);
        if (dist > g_tune.hunterEscapeDist) {
            EndHunt(h, "we are clear away"); continue;
        }

        // The game does the steering; this only reports on it. `closed` is the
        // health check: positive while she is gaining. Wanted/actual heading show
        // her turning onto us.
        if ((int)(now - h.lastAimAt) >= g_tune.hunterReaimMs) {
            h.lastAimAt = now;
            const int moved = h.havePos ? Octagonal(hx - h.lastX, hy - h.lastY) : -1;
            const int closed = h.lastDist < 0 ? 0 : h.lastDist - dist;
            Log("suspicion: %s hunter slot %d -- dist %d, closed %d, moved %d, "
                "heading %d (wants %d)%s",
                game::NationName(h.nation), h.slot, dist, closed, moved,
                HeadingDeg(h.slot, 0x14), HeadingDeg(h.slot, 0x44),
                (game::ShipFlags(h.slot) & game::kShipFlagInPort) ? ", still in harbour" : "");
            h.lastDist = dist;
            h.lastX = hx; h.lastY = hy; h.havePos = true;
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

    // Through the ledger, NOT straight into the engine's word. Being seen
    // through is a fright, and a fright lapses if the captain never makes good
    // on it -- standing.h decides whether this one does. The reputation the
    // world reacts to is written by its Tick, not here.
    const int before = nations::Reputation(nation);
    standing::NoteUnmasked(nation, penalty);
    const int after = standing::Effective(nation);

    Log("suspicion: UNMASKED by the %s -- reputation %d -> %d (penalty %d, "
        "nearest of theirs %d in slot %d) -- %s",
        game::NationName(nation), before, after,
        penalty, look.nearest, look.nearestSlot,
        after < 0 ? "their ports are now closed to us"
                  : "still tolerated, but barely");

    // The ruse is over, so END it. Saying "colours struck" and then leaving the
    // false flag flying is what made the meter reset to zero and climb again
    // on the spot, over and over, with the player never told why.
    g_sus[nation].level = 0;
    g_sus[nation].carry = 0;
    g_sus[nation].spokenAt = 0;
    g_sus[nation].hasHeat = false;
    g_sus[nation].running = false;
    g_wearing = -1;
    g_pendingStrikeColours = true;

    // Hunters are not sent from here. If this has put us below zero with them,
    // RetryDispatches sends one -- spaced, one at a time, like every hunt.
    (void)after;

    char msg[192];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                "The %s see through it. Colours struck!",
                game::NationName(nation));
    Say(msg, true);
}

// ------------------------------------------------------------- the trail
// At the top of the meter nothing is lost yet. The nearest ship of that crown
// falls in astern and TRAILS us -- a real shadow, through the same pirate-hunter
// steering as a hunt (purpose 1, PEMF's leash). She backs off if we do nothing
// aggressive for trailBackOffSec, and the meter drops back to the first rung.
// What turns a shadow into consequences is an ACT: see OnAggression.

inline bool IsHunted(int slot)
{
    for (int n = 0; n < game::addr::kNationsWithRank; ++n)
        for (int k = 0; k < kMaxHuntsPerNation; ++k)
            if (g_hunts[n][k].active && g_hunts[n][k].slot == slot) return true;
    return false;
}

inline void EndTrail(int nation, const char* why, bool tell)
{
    Trail& t = g_trail[nation];
    if (!t.active) return;
    if (t.slot >= 8 && game::ShipType(t.slot) != -1 && !IsHunted(t.slot))
        game::SetShipPurposeRaw(t.slot, t.purpose);
    MarkOurs(t.slot, false);
    Log("suspicion: the %s ship in slot %d stops trailing us -- %s",
        game::NationName(nation), t.slot, why);
    if (tell) {
        char m[160];
        _snprintf_s(m, sizeof(m), _TRUNCATE, "The %s ship falls away astern. %s",
                    game::NationName(nation), why);
        Say(m, true);
    }
    t = Trail{};
}

inline void StartTrail(int nation)
{
    Trail& t = g_trail[nation];
    if (t.active) return;
    // At most every few seconds: with nobody of theirs in range the meter sits
    // at the top and this would otherwise be asked sixty times a second.
    static DWORD lastTry[game::addr::kNationsWithRank] = {0};
    const DWORD now = GetTickCount();
    if (lastTry[nation] && now - lastTry[nation] < 5000) return;
    lastTry[nation] = now;
    const int px = game::PlayerX() / 1000, py = game::PlayerY() / 1000;
    int best = -1, bestD = g_tune.shipRange + 1;
    for (int i = 8; i < game::addr::kMaxShips; ++i) {
        if (game::ShipType(i) == -1 || game::ShipNationality(i) != nation) continue;
        if (game::ShipFlags(i) & game::kShipFlagInPort) continue;
        if (IsHunted(i)) continue;
        const int d = Octagonal(ShipX(i) - px, ShipY(i) - py);
        if (d < bestD) { bestD = d; best = i; }
    }
    if (best < 0) {
        Log("suspicion: the %s have no ship near enough to trail us", game::NationName(nation));
        return;
    }
    t.active = true;
    t.slot = best;
    t.purpose = (short)game::ShipShort(best, game::kShipPurpose);
    t.startedAt = GetTickCount();
    MarkOurs(best, true);
    game::SetShipPurposeRaw(best, game::kPurposePirateHunter);
    Log("suspicion: the %s ship in slot %d (dist %d) falls in astern of us", game::NationName(nation),
        best, bestD);
    char m[160];
    _snprintf_s(m, sizeof(m), _TRUNCATE,
                "A %s ship has fallen in astern of us. She means to keep us in sight.",
                game::NationName(nation));
    Say(m, true);
}

inline void TickTrails()
{
    const DWORD now = GetTickCount();
    for (int n = 0; n < game::addr::kNationsWithRank; ++n) {
        Trail& t = g_trail[n];
        if (!t.active) continue;
        if (game::ShipType(t.slot) == -1) { EndTrail(n, "She is gone.", false); continue; }
        // Keep her a shadow: the game may call her off like any hunter.
        if (game::ShipShort(t.slot, game::kShipPurpose) != game::kPurposePirateHunter)
            game::SetShipPurposeRaw(t.slot, game::kPurposePirateHunter);
        const DWORD quietSince = g_lastAggression[n] > t.startedAt ? g_lastAggression[n] : t.startedAt;
        if ((int)(now - quietSince) > g_tune.trailBackOffSec * 1000 ||
            (int)(now - t.startedAt) > g_tune.trailMaxSec * 1000) {
            EndTrail(n, "They have lost interest.", true);
            NationSuspicion& s = g_sus[n];
            s.level = g_tune.threshNotice > 0 ? g_tune.threshNotice - 1 : 0;
            s.carry = 0; s.spokenAt = 0;
        }
    }
}

// THE GAME SAID WE DID SOMETHING. Called by the standing ledger whenever the
// engine takes reputation from us that PEMF did not -- an attack, a capture, a
// sacking (FUN_00404FF0 for towns; the battle code for ships). The crown that
// suffered it is certain now, not suspicious; and a disguise worn while doing it
// is blown on the spot.
inline void OnAggression(int victim, int severity)
{
    if (victim < 0 || victim >= game::addr::kNationsWithRank) return;
    const DWORD now = GetTickCount();
    g_lastAggression[victim] = now;
    NationSuspicion& s = g_sus[victim];
    // Worse acts, bigger jumps: the game's own penalty (1-3 points as a rule)
    // scales the spike.
    const int sev = severity < 1 ? 1 : (severity > 3 ? 3 : severity);
    s.level += g_tune.aggressionSpike * sev;
    if (s.level > 100) s.level = 100;
    s.carry = 0;
    s.hasHeat = true;
    s.heatX = game::PlayerX() / 1000; s.heatY = game::PlayerY() / 1000;
    Log("suspicion: aggression against the %s (reputation -%d) -- their meter now %d",
        game::NationName(victim), severity, s.level);

    // Their colours, worn while attacking anyone: the ruse is over.
    if (g_wearing >= 0 && g_wearing < game::addr::kNationsWithRank) {
        g_lastAggression[g_wearing] = now;
        NationSuspicion& w = g_sus[g_wearing];
        if (g_wearing == victim || w.level >= g_tune.threshWarning) Unmask(g_wearing);
        else { w.level += g_tune.aggressionSpike / 2; if (w.level > 100) w.level = 100; }
    }
}

// ------------------------------------------------------------- the challenge
// At threshChallenge a ship comes about to ask who you are. If any loaded event
// watches the `suspicion` field (content/PEMF/events/suspicion_events.json) the
// card is that event, and its options call the three choices below; otherwise
// the ladder just says so.
inline bool g_challengeByEvent = false;

// For the `suspicion` trigger field: how far the colours we wear now have been
// seen through. 0 under true colours.
inline int CurrentLevel()
{
    if (g_wearing < 0 || g_wearing >= game::addr::kNationsWithRank) return 0;
    return g_sus[g_wearing].level;
}

// HOLD THE RUSE. Brazen it out: a gamble. `chance` is the base percent; a
// discreet officer improves it and a loose tongue spoils it, by the same
// percentage that already scales how fast suspicion rises. Carried off, they
// lower their glasses and the meter falls back below the first rung, so they
// can still grow curious again. Failed, and it is over now.
inline void HoldRuse(int chance)
{
    const int nation = g_wearing;
    if (nation < 0 || nation >= game::addr::kNationsWithRank) return;
    int odds = chance + officerfx::g_discretion;
    if (odds < 5)  odds = 5;
    if (odds > 95) odds = 95;
    const int roll = rand() % 100;
    Log("suspicion: holding the ruse with the %s -- odds %d%% (base %d, discretion %+d), "
        "rolled %d", game::NationName(nation), odds, chance, officerfx::g_discretion, roll);
    if (roll < odds) {
        NationSuspicion& s = g_sus[nation];
        s.level = g_tune.threshNotice > 0 ? g_tune.threshNotice - 1 : 0;
        s.carry = 0;
        s.spokenAt = 0;
        Say("They lower their glasses. The ruse holds -- for now.", true);
    } else {
        Unmask(nation);
    }
}

// STRIKE OUR COLOURS. Come clean before they make you. The false flag comes
// down and the matter ends there; `penalty` (default 0) is any reputation it
// costs, through the same ledger as being unmasked. No hunter is sent for it --
// though a crown that already wanted us still does (RetryDispatches).
inline void StrikeColours(int penalty)
{
    const int nation = g_wearing;
    if (nation < 0 || nation >= game::addr::kNationsWithRank) return;
    if (penalty > 0) standing::NoteUnmasked(nation, penalty);
    NationSuspicion& s = g_sus[nation];
    s.level = 0; s.carry = 0; s.spokenAt = 0; s.hasHeat = false; s.running = false;
    g_wearing = -1;
    g_pendingStrikeColours = true;
    Log("suspicion: struck our colours to the %s before being unmasked (penalty %d)",
        game::NationName(nation), penalty);
}

// ------------------------------------------------------- on the waterfront
// The crown that holds the port we are standing in, or -1. In town the ship is
// at the port, so the nearest settlement is it.
inline int PortCrown()
{
    const int city = game::NearestCity(20000);
    if (city < 0) return -1;
    const int n = game::CityNation(city);
    return (n >= 0 && n < game::addr::kNationsWithRank) ? n : -1;
}

// GREASE A PALM. `cost` gold to this port's crown makes the recent fright go
// away: their suspicion meter is emptied and the forgivable part of what being
// unmasked or marked cost us (the ledger's DEBT) is written off. What we
// actually DID -- notoriety -- is not for sale. Speaks for itself on a card.
inline void Bribe(int cost)
{
    const int n = PortCrown();
    char m[320];
    if (n < 0) {
        game::ShowEventCard("There is nobody here worth bribing -- no crown keeps an office "
                            "in this place.");
        return;
    }
    if (state::Plunder() < cost) {
        _snprintf_s(m, sizeof(m), _TRUNCATE,
                    "The harbourmaster looks at your purse, then at you. 'I said %d, "
                    "Captain. Come back when you can count.'", cost);
        game::ShowEventCard(m);
        return;
    }
    standing::Ledger& L = standing::g_led[n];
    const int debt = L.debt;
    const bool watched = g_sus[n].level > 0;
    if (debt <= 0 && !watched) {
        game::ShowEventCard("He pockets nothing. 'There is nothing in my books against you, "
                            "Captain. Keep your money.'");
        return;
    }
    state::AddPlunder(-cost, "waterfront: bribe");
    L.debt = 0;
    standing::Project(n);
    g_sus[n].level = 0; g_sus[n].carry = 0; g_sus[n].spokenAt = 0;
    g_sus[n].hasHeat = false; g_sus[n].running = false;
    Log("suspicion: bribed the %s harbourmaster (%d gold) -- debt %d written off, meter cleared",
        game::NationName(n), cost, debt);
    _snprintf_s(m, sizeof(m), _TRUNCATE,
                "The coins vanish into his coat. 'Strange -- I have no record of any "
                "trouble with your ship. None at all.' Whatever the %s were saying about "
                "you, they are not saying it here any more.", game::NationName(n));
    game::ShowEventCard(m);
}

// DOCKSIDE GOSSIP. What the waterfront knows about us, from our own state:
// who has hunters out, whose ports are shut to us, who is watching closely,
// and what colours are in the locker.
inline void Gossip(unsigned flagsOwned)
{
    char m[640];
    size_t at = 0;
    auto add = [&](const char* fmt, ...) {
        if (at >= sizeof(m) - 1) return;
        va_list ap; va_start(ap, fmt);
        const int w = _vsnprintf_s(m + at, sizeof(m) - at, _TRUNCATE, fmt, ap);
        va_end(ap);
        if (w > 0) at += (size_t)w; else at = sizeof(m) - 1;
    };
    add("You stand a few rounds and keep your ears open.");
    bool any = false;
    for (int n = 0; n < game::addr::kNationsWithRank; ++n) {
        int hunters = 0;
        for (int k = 0; k < kMaxHuntsPerNation; ++k) if (g_hunts[n][k].active) ++hunters;
        if (hunters) { add(" The %s have %d pirate-hunter%s out looking for you.",
                           game::NationName(n), hunters, hunters > 1 ? "s" : ""); any = true; }
        else if (nations::Reputation(n) < 0) {
            add(" The %s ports are shut to you.", game::NationName(n)); any = true;
        }
        if (g_sus[n].level >= g_tune.threshWarning) {
            add(" %s captains have been asking questions about your ship.", game::NationName(n));
            any = true;
        }
    }
    if (!any) add(" Nobody seems to be talking about you, which is how you like it.");
    int flags = 0;
    for (int n = 0; n < game::addr::kNationsWithRank; ++n) if (flagsOwned & (1u << n)) ++flags;
    if (flags) add(" Your flag locker holds %d crown%s colours.", flags, flags > 1 ? "s'" : "'s");
    game::ShowEventCard(m);
}

// RUN FOR IT. Keep the false flag and crowd on sail.
inline void Run()
{
    const int nation = g_wearing;
    if (nation < 0 || nation >= game::addr::kNationsWithRank) return;
    g_sus[nation].running = true;
    Log("suspicion: running from the %s challenge at %d", game::NationName(nation),
        g_sus[nation].level);
}

// Integrate one meter. `rateMilli` is thousandths of a point per second; the
// remainder is carried so that a slow rate is still progress (see `carry`).
inline void Integrate(NationSuspicion& s, int rateMilli, DWORD dtMs)
{
    s.lastRate = rateMilli / 1000 + (rateMilli > 0 && rateMilli < 1000 ? 1 : 0);
    s.carry += (long long)rateMilli * (long long)dtMs;
    const long long whole = s.carry / 1000000;
    if (whole != 0) { s.level += (int)whole; s.carry -= whole * 1000000; }
    if (s.level < 0)   { s.level = 0;   s.carry = 0; }
    if (s.level > 100) { s.level = 100; s.carry = 0; }
}

// The lines spoken on the way up, per kind of suspicion. Rung 90 for a
// disguise is the challenge card when one is loaded.
inline void SpeakRung(int nation, Why why, int rung)
{
    char m[192];
    const char* n = game::NationName(nation);
    const char* flown = g_flown >= 0 ? game::NationName(g_flown) : "our";
    switch (why) {
    case Why::Deceit:
        if (rung == 1) _snprintf_s(m, sizeof(m), _TRUNCATE, "They're signalling for our colours.");
        else if (rung == 2) _snprintf_s(m, sizeof(m), _TRUNCATE, "A %s captain is studying our rigging.", n);
        else _snprintf_s(m, sizeof(m), _TRUNCATE, "She's coming about. They don't believe you.");
        break;
    case Why::Enemy:
        if (rung == 1) _snprintf_s(m, sizeof(m), _TRUNCATE, "%s sails have marked our %s colours.", n, flown);
        else if (rung == 2) _snprintf_s(m, sizeof(m), _TRUNCATE, "A %s ship is shadowing us.", n);
        else _snprintf_s(m, sizeof(m), _TRUNCATE, "The %s are beating to quarters.", n);
        break;
    case Why::Piracy:
        if (rung == 1) _snprintf_s(m, sizeof(m), _TRUNCATE, "The %s have seen the black flag.", n);
        else if (rung == 2) _snprintf_s(m, sizeof(m), _TRUNCATE, "Word of a pirate is spreading among the %s.", n);
        else _snprintf_s(m, sizeof(m), _TRUNCATE, "The %s are calling for a pirate-hunter.", n);
        break;
    default: return;
    }
    Say(m, true);
}

// One evaluation. `dtMs` is real time since the last call.
inline void Tick(DWORD dtMs)
{
    TickHunts();
    TickTrails();

    if (!state::InGame()) return;
    if (dtMs == 0 || dtMs > 5000) return;      // paused, loading, or first tick

    for (int nation = 0; nation < game::addr::kNationsWithRank; ++nation) {
        NationSuspicion& s = g_sus[nation];
        const Why why = WhyFor(nation);

        // Nothing for them to be suspicious of: it only cools. How fast still
        // depends on where it was earned, and on whether we ran.
        if (why == Why::None) {
            if (s.level <= 0) { s.carry = 0; s.lastRate = 0; s.running = false; continue; }
            Integrate(s, RateMilli(nation, why, Look{}), dtMs);
            if (s.level < s.spokenAt) s.spokenAt = 0;
            if (s.level == 0) { s.hasHeat = false; s.running = false; }
            continue;
        }

        const Look look = Observe(nation);
        g_looks[nation] = look;
        const int rate = RateMilli(nation, why, look);
        if (g_trail[nation].active) { s.level = 100; s.carry = 0; s.lastRate = 0; }
        else Integrate(s, rate, dtMs);
        s.why = (int)why;

        // Remember WHERE this was earned, so leaving the area cools it faster.
        if (rate > 0) {
            s.heatX = game::PlayerX() / 1000;
            s.heatY = game::PlayerY() / 1000;
            s.hasHeat = true;
        } else if (s.level == 0) {
            s.hasHeat = false;
            s.running = false;
        }

        // Rungs. Each is spoken once on the way up; falling back below one arms
        // it again, so a long chase can build twice without nagging every frame.
        const int lvl = s.level;
        if (lvl < s.spokenAt) s.spokenAt = 0;

        if (lvl >= g_tune.threshUnmask) {
            if (!g_trail[nation].active) StartTrail(nation);
        } else if (lvl >= g_tune.threshChallenge && s.spokenAt < g_tune.threshChallenge) {
            s.spokenAt = g_tune.threshChallenge;
            // A disguise is challenged by the card when one is loaded; a chase
            // already under way (we ran) is not asked twice.
            if (why != Why::Deceit || (!g_challengeByEvent && !s.running))
                SpeakRung(nation, why, 3);
        } else if (lvl >= g_tune.threshWarning && s.spokenAt < g_tune.threshWarning) {
            s.spokenAt = g_tune.threshWarning;
            SpeakRung(nation, why, 2);
        } else if (lvl >= g_tune.threshNotice && s.spokenAt < g_tune.threshNotice) {
            s.spokenAt = g_tune.threshNotice;
            SpeakRung(nation, why, 1);
        }
    }
}

// ------------------------------------------------------------------ the panel
// Top right of the SCREEN, at any resolution: content.h places it from the
// interface camera's own frustum. The strings are built here, at the safe
// point; the render phase only blits them.
inline char g_panel[4][96];
inline int  g_panelLines = 0;

inline void RefreshPanel()
{
    g_panelLines = 0;
    if (!state::InGame()) return;

    // The meter that matters most right now.
    int worst = -1;
    for (int n = 0; n < game::addr::kNationsWithRank; ++n)
        if (g_sus[n].level > 0 && (worst < 0 || g_sus[n].level > g_sus[worst].level)) worst = n;
    if (g_wearing >= 0 && (worst < 0 || g_sus[g_wearing].level >= g_sus[worst].level))
        worst = g_wearing;

    // Shown while our colours could draw suspicion from anyone, and while any
    // meter has yet to cool -- so it never vanishes with something still
    // building, and never lingers when there is nothing to say.
    bool exposed = g_wearing >= 0 || g_blackFlag;
    for (int n = 0; n < game::addr::kNationsWithRank && !exposed; ++n)
        exposed = WhyFor(n) != Why::None;
    if (!exposed && worst < 0) return;

    int hunters = 0;
    for (int n = 0; n < game::addr::kNationsWithRank; ++n)
        for (int k = 0; k < kMaxHuntsPerNation; ++k)
            if (g_hunts[n][k].active) ++hunters;

    if (g_blackFlag)       _snprintf_s(g_panel[g_panelLines++], 96, _TRUNCATE, "The black flag");
    else if (g_flown >= 0) _snprintf_s(g_panel[g_panelLines++], 96, _TRUNCATE, "%s colours",
                                       game::NationName(g_flown));
    else                   _snprintf_s(g_panel[g_panelLines++], 96, _TRUNCATE, "Our own colours");

    // '=' and '.', NOT '|'. The game's font has no pipe glyph and rendered the
    // whole bar as a row of question marks. Stick to characters the game uses.
    if (worst >= 0) {
        const NationSuspicion& s = g_sus[worst];
        char bar[13];
        const int filled = (s.level * 12) / 100;
        for (int i = 0; i < 12; ++i) bar[i] = (i < filled) ? '=' : '.';
        bar[12] = 0;
        _snprintf_s(g_panel[g_panelLines++], 96, _TRUNCATE, "%s %s %s",
                    game::NationName(worst), bar, s.lastRate > 0 ? "<" : " ");

        // WHAT is looking. A port is the usual culprit and the one you cannot
        // see coming.
        if (s.lastRate > 0) {
            const Look& L = g_looks[worst];
            if (L.port >= 0 && L.ships > 0)
                _snprintf_s(g_panel[g_panelLines++], 96, _TRUNCATE, "their harbour, %d sail", L.ships);
            else if (L.port >= 0)
                _snprintf_s(g_panel[g_panelLines++], 96, _TRUNCATE, "their harbour");
            else if (L.ships > 0)
                _snprintf_s(g_panel[g_panelLines++], 96, _TRUNCATE, "%d sail watching", L.ships);
        }
    }

    for (int n = 0; n < game::addr::kNationsWithRank && g_panelLines < 4; ++n)
        if (g_trail[n].active)
            _snprintf_s(g_panel[g_panelLines++], 96, _TRUNCATE, "%s ship trailing",
                        game::NationName(n));
    if (hunters > 0 && g_panelLines < 4)
        _snprintf_s(g_panel[g_panelLines++], 96, _TRUNCATE, "%d hunting", hunters);
}

}  // namespace suspicion
