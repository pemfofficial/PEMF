// officerfx.h - what the roster is currently worth, in numbers other systems read.
//
// A skill has to change something a player would notice, or it is a label. This
// is the one place officer effects live, and it is deliberately a handful of
// plain integers with no includes of its own.
//
// -------------------------------------------------------------- why a header
// `officers.h` writes these. `loot.h`, `storms.h`, `suspicion.h` and the crew
// watcher read them. Routing that through function calls would mean every one
// of those including `officers.h`, which includes `content.h`, which includes
// half the framework -- and the cycle would be immediate. A leaf header of
// integers costs nothing and breaks it.
//
// ---------------------------------------------------------------- recomputed
// Every value here is RECOMPUTED from the hired roster whenever it changes,
// never accumulated. A total that is derived cannot drift out of step with the
// thing it describes -- the same rule `standing.h` works on, and the reason the
// first draft of the loot bonus (which adjusted a shared figure by subtracting
// its own last contribution) was wrong.
//
// ------------------------------------------------------------------ negative
// EVERY ONE OF THESE MAY GO NEGATIVE, and that is the point. Officers carry
// flaws as well as talents: a drunkard costs the crew's temper, a butcher loses
// men a good surgeon would have saved. The systems that read these must handle
// a negative as readily as a positive, and must clamp at the point of USE
// rather than here -- what "too far" means belongs to the system, not the total.
#pragma once

namespace officerfx {

// Percent ADDED to what the crew takes. Negative means a light-fingered or
// careless officer loses you some of it.
inline int g_loot = 0;

// Points on PEMF's own morale scale. Negative is a man the crew dislike.
inline int g_morale = 0;

// Percent of storm cargo losses PREVENTED. Negative means worse than nobody --
// cargo badly stowed goes over the side sooner.
inline int g_cargoGuard = 0;

// Percent by which suspicion rises MORE SLOWLY under false colours. Negative is
// an officer who cannot keep his mouth shut ashore.
inline int g_discretion = 0;

// Percent of crew losses recovered -- men who would have died and did not.
// Negative is a butcher.
inline int g_surgeon = 0;

// Applies a percentage that may be negative, and never returns below zero.
// Used by the systems that read the values above, so the clamping rule is
// written once instead of five times slightly differently.
inline int ApplyPercent(int base, int percent)
{
    if (base <= 0) return base;
    long long v = (long long)base * (100 + percent) / 100;
    if (v < 0) v = 0;
    return (int)v;
}

inline void Clear()
{
    g_loot = g_morale = g_cargoGuard = g_discretion = g_surgeon = 0;
}

} // namespace officerfx
