// standing.h - PEMF's own reputation ledger.
//
// THE ONE IDEA: the game's reputation word stops being where anything is
// STORED. It becomes an OUTPUT we project onto, so that the engine keeps
// reacting the way it always has -- ports close, hunters sail, prices move --
// while the truth about why lives here.
//
// It has to work that way because the two systems disagree about time. Vanilla
// reputation is a single number with no memory of how it got there, so a false
// flag that frightened a harbourmaster and a career spent burning that nation's
// shipping are indistinguishable once written. We need them to be different:
// one should lift if you never follow through, the other should not.
//
// So a nation's standing is three numbers:
//
//   baseline   what the game would say about you if we had never touched it
//   debt       frightened them, temporary, decays in GAME MONTHS
//   notoriety  earned by what you actually did, permanent
//
// and what the engine sees is  baseline - notoriety - debt.
//
// ---------------------------------------------------------------------------
// WHY `applied` EXISTS, WHICH IS THE PART THAT IS EASY TO GET WRONG
//
// The game writes reputation too. It does so for promotions, missions, gifts,
// and for every hostile act the player commits. If we simply wrote our target
// value every tick we would silently erase all of that, and the bug would look
// like "reputation sometimes doesn't change", which is close to undiagnosable
// months later.
//
// So we remember the last value WE wrote. When the live word differs from it,
// the difference is the game's own doing, and it belongs in `baseline` -- their
// change is real standing, and it survives.
//
// That same check answers the question the design turns on: DID THE PLAYER
// ACTUALLY DO ANYTHING? The engine already penalises attacking and plundering.
// A downward write we did not make IS the act. We need no combat hook and no
// list of what counts as hostile -- the game has one and we read its verdict.
#pragma once
#include <string.h>
#include "log.h"
#include "game.h"
#include "nations.h"

namespace standing {

// Nations only -- pirates keep no ledger and hold no grudge on paper.
constexpr int kNations = game::addr::kNationsWithRank;

struct Ledger {
    int  baseline  = 0;   // standing that is genuinely theirs to give
    int  debt      = 0;   // suspicion, forgiven if never acted upon
    int  notoriety = 0;   // earned, and it stays earned
    int  applied   = 0;   // the last value we wrote to the engine
    int  debtSetAt = 0;   // MonthsAtSea when the debt was last added to
    bool primed    = false;
};

inline Ledger g_led[kNations];
inline bool   g_live = false;      // a career is loaded and the ledger is real

// ------------------------------------------------------------------- tuning
// Months of honest sailing before a fright is forgiven outright. Long enough to
// be a decision rather than a wait, short enough to fit inside a career.
inline int kDebtForgetMonths = 12;

// How much of a hostile act's own penalty also becomes notoriety. The engine
// has already applied its number; this is the part that we make permanent so a
// pattern of raids outlives any single one of them.
inline int kActNotorietyShare = 1;   // per point the engine took

// What the OTHER crowns take from an act committed against one of them, as a
// percentage of it. Everybody hates a pirate; nobody hates one as much as the
// nation he robbed.
inline int kActSpillPercent = 25;

inline void Reset(const char* why)
{
    for (int n = 0; n < kNations; ++n) g_led[n] = Ledger{};
    g_live = false;
    Log("standing: ledger cleared (%s)", why);
}

// Adopt whatever the engine currently says as the baseline. Called when a
// career becomes live, BEFORE we have written anything.
inline void Prime()
{
    for (int n = 0; n < kNations; ++n) {
        Ledger& L = g_led[n];
        const int rep = nations::Reputation(n);
        L.baseline = rep;
        L.applied  = rep;
        L.primed   = true;
    }
    g_live = true;
    Log("standing: primed from the career -- Sp %d En %d Fr %d Du %d",
        g_led[0].baseline, g_led[1].baseline,
        g_led[2].baseline, g_led[3].baseline);
}

// Push the ledger's verdict into the engine word NOW, rather than waiting for
// the next Tick. Unmasking reads reputation immediately afterwards to decide how
// many hunters sail and how strong they are, and without this it read the value
// from before the penalty -- the log said "dispatches a pirate-hunter
// (reputation 0)" one line under "reputation 0 -> -8".
inline void Project(int n);

inline int Effective(int n)
{
    if (n < 0 || n >= kNations) return 0;
    const Ledger& L = g_led[n];
    return L.baseline - L.notoriety - L.debt;
}

// Are they already minded to think the worst of us? This decides whether a
// fright is forgiven or hardens on the spot.
inline bool AlreadyInBadOdour(int n)
{
    if (n < 0 || n >= kNations) return false;
    return (g_led[n].baseline - g_led[n].notoriety) < 0;
}

// Turn any outstanding fright into something permanent. Called when the player
// proves the suspicion was well founded.
inline void HardenDebt(int n, const char* why)
{
    if (n < 0 || n >= kNations) return;
    Ledger& L = g_led[n];
    if (L.debt <= 0) return;
    Log("standing: %s -- %d point(s) of suspicion become notoriety (%s)",
        game::NationName(n), L.debt, why);
    L.notoriety += L.debt;
    L.debt = 0;
}

inline void Project(int n)
{
    if (n < 0 || n >= kNations) return;
    Ledger& L = g_led[n];
    const int target = Effective(n);
    if (target != nations::Reputation(n)) {
        game::SetReputationRaw(n, target);
        L.applied = target;
    }
}

// Unmasked while wearing their colours. Temporary BY DEFAULT: this is the whole
// point of the system, and it is what lets a captain think better of it.
inline void NoteUnmasked(int n, int penalty)
{
    if (!g_live || n < 0 || n >= kNations || penalty <= 0) return;
    Ledger& L = g_led[n];

    L.debt += penalty;
    L.debtSetAt = game::MonthsAtSea();

    if (AlreadyInBadOdour(n)) {
        // No forgiveness available. A crown that already has your measure does
        // not put a second offence down to a misunderstanding.
        HardenDebt(n, "they already had cause to know us");
    } else {
        Log("standing: %s suspicion +%d (debt %d) -- forgiven in %d months if "
            "we do nothing", game::NationName(n), penalty, L.debt,
            kDebtForgetMonths);
    }

    Project(n);   // before the caller reads reputation to size the hunt
}

// A hostile act against `target`. Everyone minds, but they do not mind equally:
// the crown whose ship burned remembers it as an injury, and the rest file it
// under what is known about you. Without the spill a captain could raid one
// nation forever and remain a gentleman to the other three, which is not how a
// reputation for piracy has ever worked.
inline void NoteHostileAct(int target, int severity, const char* why)
{
    if (!g_live || severity <= 0) return;

    for (int n = 0; n < kNations; ++n) {
        if (n == target) continue;
        const int spill = (severity * kActSpillPercent) / 100;
        if (spill <= 0) continue;
        g_led[n].notoriety += spill;
        Project(n);
    }
    if (target >= 0 && target < kNations) {
        g_led[target].notoriety += severity;
        HardenDebt(target, why);
        Project(target);
    }
    Log("standing: piracy against the %s -- notoriety +%d there, +%d%% of it "
        "everywhere else (%s)",
        (target >= 0 && target < kNations) ? game::NationName(target) : "?",
        severity, kActSpillPercent, why);
}

// An explicit, in-fiction settling: a pardon, a bribe, a service done. The ONLY
// thing that clears notoriety -- it does not decay on its own.
inline void Amnesty(int n, const char* why)
{
    if (n < 0 || n >= kNations) return;
    Ledger& L = g_led[n];
    Log("standing: %s AMNESTY -- notoriety %d -> 0, debt %d -> 0 (%s)",
        game::NationName(n), L.notoriety, L.debt, why);
    L.notoriety = 0;
    L.debt      = 0;
}

// ---------------------------------------------------------------------------
// Once per safe point.
inline void Tick()
{
    if (!g_live) return;
    const int months = game::MonthsAtSea();

    for (int n = 0; n < kNations; ++n) {
        Ledger& L = g_led[n];
        if (!L.primed) continue;

        // 1. What did the ENGINE do since we last looked?
        const int live = nations::Reputation(n);
        if (live != L.applied) {
            const int delta = live - L.applied;
            L.baseline += delta;

            L.applied = live;   // resync before anything else reads it

            // Downward, and not by our hand: the player did something they are
            // known for. That is the proof the suspicion was earned.
            //
            // Routed through NoteHostileAct so the other crowns hear about it
            // too -- it is the same event whether we inferred it from the
            // engine's own penalty or were told about it directly.
            if (delta < 0) {
                Log("standing: %s took %d from us themselves", game::NationName(n), -delta);
                NoteHostileAct(n, (-delta) * kActNotorietyShare,
                               "we gave them the proof");
                continue;   // NoteHostileAct has already projected everyone
            }
        }

        // 2. Forgiveness. Only ever applies to debt, never to notoriety.
        if (L.debt > 0 && months - L.debtSetAt >= kDebtForgetMonths) {
            Log("standing: %s let it go -- %d point(s) of suspicion lapse after "
                "%d months with nothing to show for it",
                game::NationName(n), L.debt, months - L.debtSetAt);
            L.debt = 0;
        }

        // 3. Project. Only write when it actually differs, so we are not
        //    hammering a game global every frame for nothing.
        const int target = Effective(n);
        if (target != live) {
            game::SetReputationRaw(n, target);
            L.applied = target;
        }
    }
}

// ------------------------------------------------------------------ sidecar
// Written as plain "standing.<nation>=..." lines; unknown keys are ignored by
// the reader, so an older build simply loses the ledger rather than choking.
inline void Save(FILE* f)
{
    if (!f) return;
    for (int n = 0; n < kNations; ++n)
        fprintf(f, "standing%d=%d,%d,%d,%d\n", n,
                g_led[n].baseline, g_led[n].debt,
                g_led[n].notoriety, g_led[n].debtSetAt);
}

// Returns true if the line was ours.
inline bool LoadLine(const char* line, Ledger out[kNations])
{
    for (int n = 0; n < kNations; ++n) {
        char key[16];
        _snprintf_s(key, sizeof(key), _TRUNCATE, "standing%d=", n);
        const size_t klen = strlen(key);
        if (strncmp(line, key, klen) != 0) continue;
        int b = 0, d = 0, no = 0, at = 0;
        if (sscanf_s(line + klen, "%d,%d,%d,%d", &b, &d, &no, &at) == 4) {
            out[n].baseline = b; out[n].debt = d;
            out[n].notoriety = no; out[n].debtSetAt = at;
            out[n].primed = true;
        }
        return true;
    }
    return false;
}

// Apply a staged ledger to a career that has just become live. The BASELINE is
// deliberately re-read from the engine rather than trusted from the file: the
// save itself is the authority on plain standing, and only the parts the game
// cannot know -- what we forgave and what we remember -- come from us.
inline void ApplyStaged(const Ledger staged[kNations])
{
    Prime();
    for (int n = 0; n < kNations; ++n) {
        if (!staged[n].primed) continue;
        g_led[n].debt      = staged[n].debt;
        g_led[n].notoriety = staged[n].notoriety;
        g_led[n].debtSetAt = staged[n].debtSetAt;

        // The engine's word already includes whatever we projected before the
        // save, so lift it back off: baseline is standing WITHOUT us.
        g_led[n].baseline += staged[n].notoriety + staged[n].debt;
    }
    Log("standing: restored -- Sp(d%d/n%d) En(d%d/n%d) Fr(d%d/n%d) Du(d%d/n%d)",
        g_led[0].debt, g_led[0].notoriety, g_led[1].debt, g_led[1].notoriety,
        g_led[2].debt, g_led[2].notoriety, g_led[3].debt, g_led[3].notoriety);
}

} // namespace standing
