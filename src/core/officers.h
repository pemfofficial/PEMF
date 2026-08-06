// officers.h - named officers, and the hiring of them.
//
// The headline feature. An officer is a person the player remembers: a name, a
// standing, a line of history, and one to three things they are good at. All of
// it is authored in `PEMF\officers\roster.json` and all of it is meant to be
// edited -- names, bios, skills, roles, costs.
//
// -------------------------------------------------------------- what is real
// A skill's `target` says what it touches, and PEMF only offers targets it can
// actually deliver. Today that is two:
//
//     loot    -- adds to PEMF's share of what the crew takes (loot.h). Real,
//                measured in game: 660 plundered became 990 at +50%.
//     morale  -- feeds PEMF's own morale, once that exists.
//
// ⛔ Ship stats, player stats and world stats are NOT here. They are on the
// wish list and no engine site has been mapped for them, so a skill naming one
// is REJECTED AT LOAD with the reason. A framework that silently accepts a
// target it cannot honour teaches authors to distrust the whole schema; one
// that refuses by name teaches them where the edge is. The list grows as sites
// are mapped, and never advertises reach it does not have.
//
// ------------------------------------------------------------- what you buy
// Paying searches for an officer of that standing. It does NOT buy one. The
// gold goes on sending the crew ashore to ask after somebody, and whether
// anyone turns up is a roll -- a dearer search is a better roll, not a
// certainty. A Master is expensive and often fruitless, which is the point.
#pragma once
#include <string>
#include <vector>
#include <windows.h>

#include "../vendor/json.hpp"
#include "log.h"
#include "game.h"
#include "state.h"
#include "content.h"
#include "loot.h"

namespace officers {

using json = nlohmann::json;

constexpr int kMaxSkillsPerOfficer = 3;
constexpr int kMaxRoster           = 6;    // one of each role
constexpr int kMaxBioChars         = 160;  // fits a card beside name and skills

// ----------------------------------------------------------------- the data
struct Skill {
    std::string id, name, text, target;
    int value = 0;
};

struct Role {
    std::string id, name, blurb;
    std::vector<int> skillIndices;         // resolved after load
    std::vector<std::string> skillIds;
};

struct Tier {
    std::string id, name;
    int skills = 1, cost = 0, chance = 50;
};

inline std::vector<std::string> g_names;
inline std::vector<std::string> g_bios;
inline std::vector<Skill>       g_skills;
inline std::vector<Role>        g_roles;
inline std::vector<Tier>        g_tiers;
inline bool g_loaded = false;

// A hired officer.
struct Officer {
    std::string name, bio;
    int roleIndex = -1;
    int tierIndex = -1;
    int skills[kMaxSkillsPerOfficer] = { -1, -1, -1 };
    int skillCount = 0;
};

inline Officer g_roster[kMaxRoster];
inline bool    g_hired[kMaxRoster] = { false };

// ------------------------------------------------------------------ lookups
inline int FindSkill(const std::string& id)
{
    for (size_t i = 0; i < g_skills.size(); ++i)
        if (g_skills[i].id == id) return (int)i;
    return -1;
}

inline int FindRole(const std::string& id)
{
    for (size_t i = 0; i < g_roles.size(); ++i)
        if (g_roles[i].id == id) return (int)i;
    return -1;
}

// Targets PEMF can actually honour. Kept as one list so the error message and
// the validator can never disagree about what is supported.
inline bool TargetSupported(const std::string& t)
{
    return t == "loot" || t == "morale";
}

inline const char* SupportedTargets() { return "loot, morale"; }

// ------------------------------------------------------------------ loading
inline bool Load(const char* gameDir)
{
    g_names.clear(); g_bios.clear(); g_skills.clear();
    g_roles.clear(); g_tiers.clear();
    g_loaded = false;

    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE,
                "%s\\PEMF\\officers\\roster.json", gameDir);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        Log("officers: no %s -- hiring is unavailable", path);
        return false;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);

    json root;
    try {
        root = json::parse(text, nullptr, true, /*ignore_comments=*/true);
    } catch (const std::exception& e) {
        Log("officers: PARSE ERROR in %s", path);
        Log("officers:   %s", e.what());
        return false;
    }

    // ---- names and bios. Both are shown on a card, so both are validated the
    // way every other authored string is: ASCII, and short enough to fit.
    for (const auto& j : root.value("names", json::array())) {
        std::string s = j.get<std::string>();
        int bad = -1;
        if (s.empty() || !content::IsAscii(s, &bad)) {
            Log("officers: REJECTED name '%s' -- non-ASCII or empty", s.c_str());
            continue;
        }
        g_names.push_back(s);
    }
    for (const auto& j : root.value("bios", json::array())) {
        std::string s = j.get<std::string>();
        int bad = -1;
        if (s.empty() || !content::IsAscii(s, &bad)) {
            Log("officers: REJECTED bio -- non-ASCII or empty");
            continue;
        }
        if ((int)s.size() > kMaxBioChars) {
            // Refused rather than truncated: the card also has to hold a name,
            // a rank and up to three skills, and a bio that overflows pushes
            // one of those off the bottom in front of a player.
            Log("officers: REJECTED bio -- %d chars, the budget is %d: \"%.40s...\"",
                (int)s.size(), kMaxBioChars, s.c_str());
            continue;
        }
        g_bios.push_back(s);
    }

    // ---- skills
    for (const auto& j : root.value("skills", json::array())) {
        Skill s;
        s.id     = j.value("id", "");
        s.name   = j.value("name", "");
        s.text   = j.value("text", "");
        s.target = j.value("target", "");
        s.value  = j.value("value", 0);

        if (s.id.empty() || s.name.empty()) {
            Log("officers: REJECTED skill -- needs 'id' and 'name'");
            continue;
        }
        if (FindSkill(s.id) >= 0) {
            Log("officers: REJECTED skill '%s' -- duplicate id", s.id.c_str());
            continue;
        }
        int bad = -1;
        if (!content::IsAscii(s.name, &bad) || !content::IsAscii(s.text, &bad)) {
            Log("officers: REJECTED skill '%s' -- non-ASCII text", s.id.c_str());
            continue;
        }
        if (!TargetSupported(s.target)) {
            // The important refusal. See the note at the top of this file.
            Log("officers: REJECTED skill '%s' -- target '%s' is not something "
                "PEMF can change yet (supported: %s)",
                s.id.c_str(), s.target.c_str(), SupportedTargets());
            continue;
        }
        if (s.value == 0)
            Log("officers: skill '%s' has value 0 and will do nothing",
                s.id.c_str());
        g_skills.push_back(s);
    }

    // ---- roles
    for (const auto& j : root.value("roles", json::array())) {
        Role r;
        r.id    = j.value("id", "");
        r.name  = j.value("name", "");
        r.blurb = j.value("blurb", "");
        if (r.id.empty() || r.name.empty()) {
            Log("officers: REJECTED role -- needs 'id' and 'name'");
            continue;
        }
        for (const auto& sj : j.value("skills", json::array()))
            r.skillIds.push_back(sj.get<std::string>());
        g_roles.push_back(r);
    }

    // ---- tiers
    for (const auto& j : root.value("tiers", json::array())) {
        Tier t;
        t.id     = j.value("id", "");
        t.name   = j.value("name", "");
        t.skills = j.value("skills", 1);
        t.cost   = j.value("cost", 0);
        t.chance = j.value("chance", 50);
        if (t.id.empty() || t.name.empty()) {
            Log("officers: REJECTED tier -- needs 'id' and 'name'");
            continue;
        }
        if (t.skills < 1) t.skills = 1;
        if (t.skills > kMaxSkillsPerOfficer) t.skills = kMaxSkillsPerOfficer;
        if (t.chance < 1)   t.chance = 1;
        if (t.chance > 100) t.chance = 100;
        if (t.cost < 0)     t.cost = 0;
        g_tiers.push_back(t);
    }

    // ---- resolve role -> skill, once everything is in. A role pointing at a
    // skill that was itself rejected is reported by name rather than quietly
    // producing officers with nothing to offer.
    for (Role& r : g_roles) {
        for (const std::string& id : r.skillIds) {
            const int idx = FindSkill(id);
            if (idx < 0) {
                Log("officers: role '%s' names skill '%s', which does not exist "
                    "(or was rejected above)", r.id.c_str(), id.c_str());
                continue;
            }
            r.skillIndices.push_back(idx);
        }
        if (r.skillIndices.empty())
            Log("officers: role '%s' has no usable skills -- officers of that "
                "post will have none", r.id.c_str());
    }

    g_loaded = !g_names.empty() && !g_roles.empty() && !g_tiers.empty();
    Log("officers: %d name(s), %d bio(s), %d skill(s), %d role(s), %d tier(s)%s",
        (int)g_names.size(), (int)g_bios.size(), (int)g_skills.size(),
        (int)g_roles.size(), (int)g_tiers.size(),
        g_loaded ? "" : "  -- NOT ENOUGH TO HIRE ANYONE");
    return g_loaded;
}

// ------------------------------------------------------------- generation
// The game's own rand(), so a career started from the same seed behaves the
// same way the rest of the game does.
inline int Roll(int n) { return n > 0 ? (rand() % n) : 0; }

inline void Generate(Officer* out, int roleIndex, int tierIndex)
{
    const Role& role = g_roles[(size_t)roleIndex];
    const Tier& tier = g_tiers[(size_t)tierIndex];

    out->roleIndex = roleIndex;
    out->tierIndex = tierIndex;
    out->name = g_names.empty() ? "A stranger" : g_names[(size_t)Roll((int)g_names.size())];
    out->bio  = g_bios.empty()  ? ""           : g_bios[(size_t)Roll((int)g_bios.size())];
    out->skillCount = 0;

    // Draw without replacement, so nobody is good at the same thing twice.
    std::vector<int> pool = role.skillIndices;
    const int want = tier.skills;
    for (int i = 0; i < want && !pool.empty(); ++i) {
        const int pick = Roll((int)pool.size());
        out->skills[out->skillCount++] = pool[(size_t)pick];
        pool.erase(pool.begin() + pick);
    }
}

// ------------------------------------------------------------- the totals
// What the roster is currently worth, recomputed rather than accumulated --
// the same principle standing.h works on. A total that is derived cannot drift
// out of step with the thing it describes.
inline int TotalFor(const char* target)
{
    int sum = 0;
    for (int i = 0; i < kMaxRoster; ++i) {
        if (!g_hired[i]) continue;
        const Officer& o = g_roster[i];
        for (int s = 0; s < o.skillCount; ++s) {
            const Skill& sk = g_skills[(size_t)o.skills[s]];
            if (sk.target == target) sum += sk.value;
        }
    }
    return sum;
}

// Push the roster's effects out to the systems that own them. Called whenever
// the roster changes, so there is exactly one place where officers become
// consequences.
inline void ApplyEffects(const char* why)
{
    loot::SetOfficerPercent(TotalFor("loot"), why);
    // `morale` totals are read by the morale system when it exists. Nothing to
    // push yet, and inventing a sink for it now would be a guess at an
    // interface that has not been designed.
}

// ------------------------------------------------------------- the hiring
// Presented from a town-menu callback, so the port is behind the card and the
// player is returned to the menu afterwards -- see townmenu.h.

inline bool RosterHas(int roleIndex)
{
    return roleIndex >= 0 && roleIndex < kMaxRoster && g_hired[roleIndex];
}

inline void DescribeOfficer(const Officer& o, char* out, size_t outsz)
{
    const Role& role = g_roles[(size_t)o.roleIndex];
    const Tier& tier = g_tiers[(size_t)o.tierIndex];

    int n = _snprintf_s(out, outsz, _TRUNCATE,
                        "%s %s\n%s\n",
                        tier.name.c_str(), role.name.c_str(),
                        o.name.c_str());
    if (n < 0) return;

    if (!o.bio.empty())
        n += _snprintf_s(out + n, outsz - n, _TRUNCATE, "%s\n", o.bio.c_str());

    for (int i = 0; i < o.skillCount; ++i) {
        const Skill& sk = g_skills[(size_t)o.skills[i]];
        const int w = _snprintf_s(out + n, outsz - n, _TRUNCATE,
                                  "\n%s -- %s",
                                  sk.name.c_str(), sk.text.c_str());
        if (w < 0) break;      // out of room; better a short card than a broken one
        n += w;
    }
}

// One search. Returns true if somebody was found and taken on.
inline void Search(int roleIndex, int tierIndex)
{
    if (!g_loaded) {
        game::ShowModalTextN("There is nobody to be found in this port.",
                             nullptr, 0);
        return;
    }
    if (roleIndex < 0 || roleIndex >= (int)g_roles.size()) return;
    if (tierIndex < 0 || tierIndex >= (int)g_tiers.size()) return;

    const Role& role = g_roles[(size_t)roleIndex];
    const Tier& tier = g_tiers[(size_t)tierIndex];

    if (RosterHas(roleIndex)) {
        char msg[256];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "You already have a %s aboard. Pay him off before you go "
                    "looking for another.", role.name.c_str());
        game::ShowModalTextN(msg, nullptr, 0);
        return;
    }

    const int purse = state::Plunder();
    if (purse < tier.cost) {
        char msg[256];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "Asking after a %s %s costs @NUM pieces, and you have @NUM.",
                    tier.name.c_str(), role.name.c_str());
        int args[2] = { tier.cost, purse };
        game::ShowModalTextN(msg, args, 2);
        return;
    }

    // Paid whether or not anyone is found. That is the bargain: the gold buys
    // the asking, not the man.
    state::AddPlunder(-tier.cost, "officer search");

    if (Roll(100) >= tier.chance) {
        char msg[320];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "Your men spend @NUM pieces and the better part of a day "
                    "asking after a %s %s. Nobody of that standing is looking "
                    "for a berth.", tier.name.c_str(), role.name.c_str());
        int args[1] = { tier.cost };
        game::ShowModalTextN(msg, args, 1);
        Log("officers: search for %s %s -- found nobody (%d gold)",
            tier.name.c_str(), role.name.c_str(), tier.cost);
        return;
    }

    Officer found;
    Generate(&found, roleIndex, tierIndex);

    char card[1024];
    DescribeOfficer(found, card, sizeof(card));

    const char* opts[2] = { "Take him on.", "Not this one." };
    const int pick = game::AskChoiceN(card, opts, 2, nullptr, 0);

    if (pick != 0) {
        Log("officers: found %s (%s %s) -- declined", found.name.c_str(),
            tier.name.c_str(), role.name.c_str());
        return;
    }

    g_roster[roleIndex] = found;
    g_hired[roleIndex]  = true;
    ApplyEffects("officer hired");

    Log("officers: HIRED %s -- %s %s, %d skill(s)", found.name.c_str(),
        tier.name.c_str(), role.name.c_str(), found.skillCount);

    char msg[256];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                "%s signs the articles and has his chest brought aboard.",
                found.name.c_str());
    game::ShowModalTextN(msg, nullptr, 0);
}

// What the roster looks like, for the menu.
inline void ShowRoster()
{
    char card[1024];
    int n = _snprintf_s(card, sizeof(card), _TRUNCATE,
                        "Your officers.\n");

    bool any = false;
    for (int i = 0; i < kMaxRoster && i < (int)g_roles.size(); ++i) {
        if (!g_hired[i]) continue;
        any = true;
        const Officer& o = g_roster[i];
        const int w = _snprintf_s(card + n, sizeof(card) - n, _TRUNCATE,
                                  "\n%s, %s %s",
                                  o.name.c_str(),
                                  g_tiers[(size_t)o.tierIndex].name.c_str(),
                                  g_roles[(size_t)o.roleIndex].name.c_str());
        if (w < 0) break;
        n += w;
    }

    if (!any)
        _snprintf_s(card, sizeof(card), _TRUNCATE,
                    "You keep no officers. The ship is run by whoever is "
                    "nearest and willing.");

    game::ShowModalTextN(card, nullptr, 0);
}

} // namespace officers
