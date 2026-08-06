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
#include "session.h"
#include "loot.h"
#include "officerfx.h"
#include "crewmorale.h"

namespace officers {

using json = nlohmann::json;

constexpr int kMaxSkillsPerOfficer = 4;   // up to 3 talents plus one flaw
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
    std::vector<int> flawIndices;
    std::vector<std::string> skillIds;
    std::vector<std::string> flawIds;
};

struct Tier {
    std::string id, name;
    int skills = 1, cost = 0, chance = 50;
    // How often a man of this standing carries a flaw. A Novice is cheap and
    // often shabby; a Master is dear and usually not. This is what makes the
    // expensive search worth making rather than merely likelier to succeed.
    int flawChance = 0;
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
    int skills[kMaxSkillsPerOfficer] = { -1, -1, -1, -1 };
    int skillCount = 0;
    int hiredMonth = 0;      // months at sea when he signed; tenure is derived
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
    return t == "loot"       || t == "morale"     || t == "cargoGuard"
        || t == "discretion" || t == "surgeon";
}

inline const char* SupportedTargets()
{
    return "loot, morale, cargoGuard, discretion, surgeon";
}

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
        for (const auto& fj : j.value("flaws", json::array()))
            r.flawIds.push_back(fj.get<std::string>());
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
        t.flawChance = j.value("flawChance", 0);
        if (t.id.empty() || t.name.empty()) {
            Log("officers: REJECTED tier -- needs 'id' and 'name'");
            continue;
        }
        if (t.skills < 1) t.skills = 1;
        if (t.skills > kMaxSkillsPerOfficer) t.skills = kMaxSkillsPerOfficer;
        if (t.chance < 1)   t.chance = 1;
        if (t.chance > 100) t.chance = 100;
        if (t.flawChance < 0)   t.flawChance = 0;
        if (t.flawChance > 100) t.flawChance = 100;
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
        for (const std::string& id : r.flawIds) {
            const int idx = FindSkill(id);
            if (idx < 0) {
                Log("officers: role '%s' names flaw '%s', which does not exist "
                    "(or was rejected above)", r.id.c_str(), id.c_str());
                continue;
            }
            r.flawIndices.push_back(idx);
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

    // ...and he may carry a flaw, which is what makes two men of the same
    // standing different men. Rolled AFTER the talents so it is always the
    // last line on his card, where a player will read it as the catch.
    if (!role.flawIndices.empty() &&
        out->skillCount < kMaxSkillsPerOfficer &&
        Roll(100) < tier.flawChance) {
        out->skills[out->skillCount++] =
            role.flawIndices[(size_t)Roll((int)role.flawIndices.size())];
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
    officerfx::g_loot       = TotalFor("loot");
    officerfx::g_morale     = TotalFor("morale");
    officerfx::g_cargoGuard = TotalFor("cargoGuard");
    officerfx::g_discretion = TotalFor("discretion");
    officerfx::g_surgeon    = TotalFor("surgeon");

    Log("officers: effects (%s) -- loot %+d%%, morale %+d, cargo %+d%%, "
        "discretion %+d%%, surgeon %+d%%", why ? why : "roster changed",
        officerfx::g_loot, officerfx::g_morale, officerfx::g_cargoGuard,
        officerfx::g_discretion, officerfx::g_surgeon);
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

    found.hiredMonth    = state::Months();
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

// ------------------------------------------------------------- the roster
// One card per man rather than a list, because an officer the player cannot
// look at is a line of text, not a person.

inline int Tenure(const Officer& o)
{
    const int now = state::Months();
    return now > o.hiredMonth ? now - o.hiredMonth : 0;
}

// What he actually does for you. Contributions are read from the same totals
// the systems use, so this card cannot claim something the game is not doing.
inline void ShowOfficerDetail(int roleIndex)
{
    if (!RosterHas(roleIndex)) return;
    const Officer& o = g_roster[roleIndex];

    char card[1024];
    int n = _snprintf_s(card, sizeof(card), _TRUNCATE,
                        "%s, %s %s.\n",
                        o.name.c_str(),
                        g_tiers[(size_t)o.tierIndex].name.c_str(),
                        g_roles[(size_t)o.roleIndex].name.c_str());

    const int months = Tenure(o);
    n += _snprintf_s(card + n, sizeof(card) - n, _TRUNCATE,
                     "%s\n",
                     months <= 0 ? "Signed on this month."
                                 : (months == 1 ? "One month aboard."
                                                : "@NUM months aboard."));

    if (!o.bio.empty())
        n += _snprintf_s(card + n, sizeof(card) - n, _TRUNCATE,
                         "%s\n", o.bio.c_str());

    if (o.skillCount == 0) {
        _snprintf_s(card + n, sizeof(card) - n, _TRUNCATE,
                    "\nHe has no particular talent to speak of.");
    } else {
        for (int i = 0; i < o.skillCount; ++i) {
            const Skill& sk = g_skills[(size_t)o.skills[i]];
            const char* what =
                  (sk.target == "loot")       ? "to what we take"
                : (sk.target == "morale")     ? "to the men's temper"
                : (sk.target == "cargoGuard") ? "of the cargo saved in a blow"
                : (sk.target == "discretion") ? "to how long false colours hold"
                : (sk.target == "surgeon")    ? "of the wounded who live"
                                              : "";
            const int w = _snprintf_s(card + n, sizeof(card) - n, _TRUNCATE,
                                      "\n%s -- %s (+%d %s)",
                                      sk.name.c_str(), sk.text.c_str(),
                                      sk.value, what);
            if (w < 0) break;
            n += w;
        }
    }

    int args[1] = { months };
    game::ShowModalTextN(card, args, months > 1 ? 1 : 0);
}

// Talking to him is how the crew is managed -- through the man whose job it is,
// rather than from a menu the captain has no business having.
inline void SpeakWith(int roleIndex)
{
    if (!RosterHas(roleIndex)) return;
    const Officer& o = g_roster[roleIndex];

    // PEMF's own tier, not the engine's five -- this is the one place a player
    // sees the wider scale, and the word for it is ours.
    char card[512];
    _snprintf_s(card, sizeof(card), _TRUNCATE,
                "%s knuckles his forehead. \"Crew of @NUM, captain, and they "
                "stand %s. What would you have of them?\"",
                o.name.c_str(), crewmorale::Name());

    int args[1] = { state::Crew() };
    const char* opts[3] = {
        "\"How do the men find the voyage?\"",
        "\"Nothing for now.\"",
        nullptr
    };
    const int pick = game::AskChoiceN(card, opts, 2, args, 1);
    if (pick != 0) return;

    // Honest about its own limits: this reports what the engine actually says
    // rather than inventing a crew system that does not exist yet. The morale
    // work is what turns this into orders that mean something.
    const int m = crewmorale::TargetEngineLevel(crewmorale::g_value);
    const char* answer =
        (m >= 4) ? "\"Well enough that they'd follow you into a lee shore, and "
                   "say so where you can hear it.\""
      : (m == 3) ? "\"No complaints worth carrying to you, captain.\""
      : (m == 2) ? "\"They've had worse. They've had better, too, and they "
                   "remember which.\""
      : (m == 1) ? "\"Poorly, captain. There's talk, and I'll not pretend "
                   "otherwise.\""
                 : "\"Badly. Divide the plunder before somebody else divides "
                   "it for you.\"";

    game::ShowModalTextN(answer, nullptr, 0);
}

inline void ShowOfficer(int roleIndex)
{
    for (int guard = 0; guard < 16; ++guard) {
        if (!RosterHas(roleIndex)) return;
        const Officer& o = g_roster[roleIndex];

        char title[256];
        _snprintf_s(title, sizeof(title), _TRUNCATE, "%s, %s %s.",
                    o.name.c_str(),
                    g_tiers[(size_t)o.tierIndex].name.c_str(),
                    g_roles[(size_t)o.roleIndex].name.c_str());

        const char* opts[3] = {
            "Speak with him",
            "What he does for you",
            "Never mind."
        };
        const int pick = game::AskChoiceN(title, opts, 3, nullptr, 0);

        if (pick == 0)      SpeakWith(roleIndex);
        else if (pick == 1) ShowOfficerDetail(roleIndex);
        else                return;
    }
}

// The roster: one selectable row per officer.
inline void ShowRoster()
{
    for (int guard = 0; guard < 16; ++guard) {
        char labels[5][96];
        int  idx[5];
        const char* opts[6] = { nullptr };
        int n = 0;

        for (int i = 0; i < kMaxRoster && i < (int)g_roles.size() && n < 5; ++i) {
            if (!g_hired[i]) continue;
            const Officer& o = g_roster[i];
            _snprintf_s(labels[n], sizeof(labels[n]), _TRUNCATE, "%s, %s %s",
                        o.name.c_str(),
                        g_tiers[(size_t)o.tierIndex].name.c_str(),
                        g_roles[(size_t)o.roleIndex].name.c_str());
            opts[n] = labels[n];
            idx[n]  = i;
            ++n;
        }

        if (n == 0) {
            game::ShowModalTextN("You keep no officers. The ship is run by "
                                 "whoever is nearest and willing.", nullptr, 0);
            return;
        }

        const int backRow = n;
        opts[n++] = "Never mind.";

        const int pick = game::AskChoiceN("Your officers.", opts, n, nullptr, 0);
        if (pick < 0 || pick >= n || pick == backRow) return;

        ShowOfficer(idx[pick]);
    }
}

// ------------------------------------------------------------- persistence
// Officers are written as AUTHORED IDS, never indices. An index is a position
// in roster.json, so a player who adds a name or reorders a role would turn
// every saved officer into a different man. Ids survive an edited file; a trait
// that has since been deleted is dropped, with a line saying whose it was.
//
//   roleId|tierId|hiredMonth|Name|Bio|skillId,skillId,...
//
// Pipes, because a bio contains commas and spaces and will not contain a pipe.
inline int FindTier(const std::string& id)
{
    for (size_t i = 0; i < g_tiers.size(); ++i)
        if (g_tiers[i].id == id) return (int)i;
    return -1;
}

inline void Serialize()
{
    session::ClearSavedOfficers();

    for (int i = 0; i < kMaxRoster && i < (int)g_roles.size(); ++i) {
        if (!g_hired[i]) continue;
        const Officer& o = g_roster[i];

        char skills[192] = {0};
        for (int k = 0; k < o.skillCount; ++k) {
            if (o.skills[k] < 0 || o.skills[k] >= (int)g_skills.size()) continue;
            if (skills[0]) strncat_s(skills, sizeof(skills), ",", _TRUNCATE);
            strncat_s(skills, sizeof(skills),
                      g_skills[(size_t)o.skills[k]].id.c_str(), _TRUNCATE);
        }

        char line[320];
        _snprintf_s(line, sizeof(line), _TRUNCATE, "%s|%s|%d|%s|%s|%s",
                    g_roles[(size_t)o.roleIndex].id.c_str(),
                    g_tiers[(size_t)o.tierIndex].id.c_str(),
                    o.hiredMonth, o.name.c_str(), o.bio.c_str(), skills);
        session::AddSavedOfficer(line);
    }
}

inline void Restore()
{
    for (int i = 0; i < kMaxRoster; ++i) g_hired[i] = false;

    const int n = session::SavedOfficerCount();
    int restored = 0;

    for (int i = 0; i < n; ++i) {
        const char* src = session::SavedOfficer(i);
        if (!src) continue;

        char buf[320];
        strncpy_s(buf, sizeof(buf), src, _TRUNCATE);

        char* field[6] = { nullptr };
        int nf = 0;
        field[nf++] = buf;
        for (char* p = buf; *p && nf < 6; ++p)
            if (*p == '|') { *p = 0; field[nf++] = p + 1; }

        if (nf < 5) {
            Log("officers: a saved officer line is malformed -- skipped");
            continue;
        }

        const int role = FindRole(field[0]);
        const int tier = FindTier(field[1]);
        if (role < 0 || tier < 0) {
            // roster.json was edited and this post or standing is gone. Said by
            // name: the alternative is an officer who silently vanishes between
            // one launch and the next.
            Log("officers: saved officer '%s' had role '%s' tier '%s', which "
                "roster.json no longer defines -- dropped",
                field[3] ? field[3] : "?", field[0], field[1]);
            continue;
        }

        Officer o;
        o.roleIndex  = role;
        o.tierIndex  = tier;
        o.hiredMonth = atoi(field[2]);
        o.name       = field[3];
        o.bio        = (nf > 4 && field[4]) ? field[4] : "";
        o.skillCount = 0;

        if (nf > 5 && field[5] && *field[5]) {
            char* start = field[5];
            for (char* sp = field[5];; ++sp) {
                if (*sp == ',' || *sp == 0) {
                    const char end = *sp;
                    *sp = 0;
                    if (*start && o.skillCount < kMaxSkillsPerOfficer) {
                        const int sk = FindSkill(start);
                        if (sk >= 0) o.skills[o.skillCount++] = sk;
                        else Log("officers: %s had trait '%s', which no longer "
                                 "exists -- dropped", o.name.c_str(), start);
                    }
                    if (end == 0) break;
                    start = sp + 1;
                }
            }
        }

        g_roster[role] = o;
        g_hired[role]  = true;
        ++restored;
    }

    ApplyEffects("roster restored");
    if (restored) Log("officers: %d restored from the save", restored);
}

} // namespace officers
