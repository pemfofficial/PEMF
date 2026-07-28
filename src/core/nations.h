// nations.h - who the powers are, how they feel about each other, and where
// the player stands with each of them.
//
// All of this is the game's own state, read where it lies. Nothing here writes:
// the relations matrix is the engine's to maintain, and a framework that edits
// it would be rewriting the world rather than reacting to it. What the reads
// are FOR is false colours -- a disguise only means something if we can tell
// who would care.
//
// The single find that makes the rest usable: the game does not keep "the
// nation you chose at character creation" anywhere obvious, and looking for it
// was the wrong question. It keeps a RANK with each crown, and at the start of
// a career exactly one of them is non-zero. See DeriveHomeNation().
//
// Addresses and the disassembly they were read from live in game.h.
#pragma once
#include "game.h"
#include "log.h"

namespace nations {

// ------------------------------------------------------------------ reading
// Every read is guarded. These addresses are correct for a running career, but
// they are also read from probe hotkeys that a player can hit at the main menu,
// during a load, or mid-transition -- and a framework that faults there takes
// the game down with it.

// The relationship between two powers: game::addr::kAtWar, kTreaty, or 0 for
// neutral. Out-of-range slots read as neutral rather than as anything alarming.
inline int Relation(int a, int b)
{
    if (a < 0 || b < 0 ||
        a >= game::addr::kRelationSlots || b >= game::addr::kRelationSlots) {
        return 0;
    }
    __try {
        const int index = a * game::addr::kRelationStride + b;
        return *(const int*)(game::addr::NationRelations + (uintptr_t)index * 4);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

inline bool AtWar(int a, int b)  { return Relation(a, b) == game::addr::kAtWar; }
inline bool HasTreaty(int a, int b) { return Relation(a, b) == game::addr::kTreaty; }

// The player's rank with a crown. 0 means no letter of marque at all, which is
// the state you are in with three of the four the moment a career begins.
inline int Rank(int nation)
{
    if (nation < 0 || nation >= game::addr::kNationsWithRank) return 0;
    __try {
        return *(const short*)(game::addr::PlayerRank + (uintptr_t)nation * 2);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// The player's reputation with a crown. The engine's own promotion check wants
// this at 3 or better before it will raise your rank.
inline int Reputation(int nation)
{
    if (nation < 0 || nation >= game::addr::kNationsWithRank) return 0;
    __try {
        return *(const short*)(game::addr::PlayerReputation + (uintptr_t)nation * 2);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// "Grunt", "Captain", ... "Duke", out of the engine's own table so the words
// match what the governor says. Never returns null.
inline const char* RankName(int rank)
{
    if (rank < 0 || rank >= game::addr::kRankNameMax) return "none";
    __try {
        const char* name =
            *(const char* const*)(game::addr::RankNames + (uintptr_t)rank * 4);
        return name ? name : "none";
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return "none"; }
}

// ------------------------------------------------------- the home nation
// The crown the player serves. READ, not derived -- the engine keeps its own
// answer and acts on it, so anything we computed ourselves could only ever
// agree with it or be wrong.
//
// This began as a derivation, because the nation chosen at character creation
// appeared to be stored nowhere. It is not: the game recomputes it from the
// rank array whenever a rank changes, and caches the result at PlayerNation.
// The derivation below turned out to be the very algorithm the engine uses,
// which is a good sign about the reading and no reason at all to keep using
// it in preference to the value itself.
inline int HomeNation()
{
    __try {
        const int n = *(const short*)game::addr::PlayerNation;
        return (n >= 0 && n < game::addr::kNationsWithRank) ? n : -1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// The engine's own rule, reimplemented: the crown you outrank all others with,
// strictly. Kept as a CROSS-CHECK rather than as the answer -- if this and
// HomeNation() ever disagree, one of the two addresses is wrong and that is
// worth finding out on the spot rather than months later.
//
// Returns -1 when no crown is strictly highest, which is the honest answer for
// a captain holding no commission and for one holding two equal ones.
inline int DeriveHomeNation()
{
    for (int cand = 0; cand < game::addr::kNationsWithRank; ++cand) {
        bool highest = true;
        for (int n = 0; n < game::addr::kNationsWithRank; ++n)
            if (n != cand && Rank(n) >= Rank(cand)) { highest = false; break; }
        if (highest) return cand;
    }
    return -1;
}

// True when the player holds a commission from more than one crown.
inline bool ServesMoreThanOne()
{
    int held = 0;
    for (int n = 0; n < game::addr::kNationsWithRank; ++n) {
        if (Rank(n) > 0) ++held;
    }
    return held > 1;
}

}  // namespace nations
