// content.h - JSON-authored narrative events.
//
// Events are data, not code. A file of events is loaded once, validated hard,
// and thereafter referenced by INDEX -- never by pointer, so a reload can never
// leave a queued event pointing at freed data.
//
// VALIDATION IS THE POINT. Once players author content, every malformed field
// is a potential crash inside a 2004 engine with no bounds checking. Everything
// is rejected at load time with a precise message rather than discovered at
// runtime:
//
//   * @-token count must exactly match the supplied argument count. The engine
//     formatter consumes varargs positionally like printf; one token too many
//     reads stack garbage. This is the single most dangerous authoring mistake.
//   * only tokens whose vararg appetite has been READ OUT OF THE DISASSEMBLY
//     are permitted: @NUM, @HAPPY, @CITYNAME, @NATIONALITY, @LOCTYPE. Others
//     exist but would be guessing with the stack. Note @CITYNAME consumes
//     THREE arguments, not one -- it is a three-word name record.
//   * text must be 7-bit ASCII. The engine predates UTF-8; a curly quote or an
//     accented character pasted from a word processor would render as garbage.
//   * effect operations come from a fixed whitelist. An unknown op is a load
//     error, never a silently ignored no-op.
//
// Note: a '%' in authored text is SAFE. The engine formatter is @-token based
// and never calls printf-family (verified: zero vsprintf/sprintf/_snprintf call
// sites in the formatter region), so there is no format-string hazard.
#pragma once
#include <string>
#include <vector>
#include <windows.h>

#include "../vendor/json.hpp"
#include "log.h"
#include "state.h"
#include "nations.h"
#include "suspicion.h"
#include "render.h"
#include "d3d9hook.h"

namespace content {

using json = nlohmann::json;

// ------------------------------------------------------------------ limits
constexpr int kMaxOptions   = 6;      // beyond this the card stops being readable
constexpr int kMaxArgs      = 8;      // must not exceed game::kMaxTextArgs
constexpr int kMaxBodyChars = 1200;   // prompt budget shared with option lines

// --------------------------------------------------------------- arg source
// How far out to look when an event asks about "the port you are approaching".
// Generous on purpose: the event has already decided it wants to talk about a
// port, so the name should resolve even from further off than a nearPort
// trigger would fire at.
constexpr int kCityNameScanRadius = 20000;

// Where a token's value comes from at fire time.
//
// NOTE that a source does not always supply ONE value. @CITYNAME is a
// three-word name record, so `nearestCity` fills three argument slots from one
// authored entry. Everything downstream counts SLOTS, not entries.
enum class ArgSource {
    Literal, Crew, Morale, Plunder, Months,
    NearestCity,        // 3 slots -- @CITYNAME
    NearestCityNation,  // 1 slot  -- @NATIONALITY
    NearestCityType,    // 1 slot  -- @LOCTYPE
};

// Resolved once per event, so every city token in one card refers to the same
// port even if the ship moves between substitutions.
inline int ResolveNearestCity()
{
    return game::NearestCity(kCityNameScanRadius);
}

struct Arg {
    ArgSource source = ArgSource::Literal;
    int       literal = 0;

    // How many argument slots this entry supplies.
    int Slots() const
    {
        return source == ArgSource::NearestCity
                   ? (int)game::addr::kCityNameWords : 1;
    }

    // Write this entry's value(s) into `out`, returning how many were written.
    // `city` is the index resolved once for the whole event.
    int Resolve(int* out, int city) const
    {
        switch (source) {
            case ArgSource::Crew:    out[0] = state::Crew();    return 1;
            case ArgSource::Morale:  out[0] = state::Morale();  return 1;
            case ArgSource::Plunder: out[0] = state::Plunder(); return 1;
            case ArgSource::Months:  out[0] = state::Months();  return 1;
            case ArgSource::NearestCityNation:
                out[0] = game::CityNation(city); return 1;
            case ArgSource::NearestCityType:
                out[0] = game::CityLocType(city); return 1;
            case ArgSource::NearestCity:
                if (!game::CityNameWords(city, out)) out[0] = out[1] = out[2] = 0;
                return (int)game::addr::kCityNameWords;
            default:                 out[0] = literal;          return 1;
        }
    }
};

// Total slots a list of authored args supplies. This is what must match the
// token count -- not the number of entries.
inline size_t ArgSlots(const std::vector<Arg>& args)
{
    size_t n = 0;
    for (const Arg& a : args) n += (size_t)a.Slots();
    return n;
}

// Expand authored args into the flat vararg list the engine formatter reads,
// and return how many slots were filled. The nearest city is resolved ONCE for
// the whole list, so every city token in one piece of text names the same port.
//
// An entry that would overrun the buffer is dropped whole rather than in part:
// half a three-word name is worse than none, because the remaining tokens
// would then read whatever follows.
inline int ResolveArgs(const std::vector<Arg>& args, int* out, int maxSlots)
{
    int city = -1, n = 0;
    for (const Arg& a : args) {
        const int need = a.Slots();
        if (n + need > maxSlots) break;
        if (city < 0 && (a.source == ArgSource::NearestCity ||
                         a.source == ArgSource::NearestCityNation ||
                         a.source == ArgSource::NearestCityType))
            city = ResolveNearestCity();
        n += a.Resolve(out + n, city);
    }
    return n;
}

// ------------------------------------------------------------------ effects
enum class EffectOp { AddPlunder, SetPlunder, AddCrew, SetCrew };

struct Effect {
    EffectOp op    = EffectOp::AddPlunder;
    int      value = 0;
};

struct Option {
    std::string         text;        // rendered with a leading space
    std::vector<Effect> effects;
    std::string         outcome;     // shown after the choice; may be empty
    std::vector<Arg>    outcomeArgs;
};

// ----------------------------------------------------------------- triggers
// What makes an event fire on its own. An event with no trigger can still be
// fired by hand (the debug hotkeys) but will never occur during play.
enum class TriggerType {
    None,
    ElapsedSailing,   // N seconds of actual sailing in the overworld
    NearPort,         // came within N units of any port
    StateCrosses,     // a live game value crossed a threshold
};

// Which live value a StateCrosses trigger watches. One trigger type covering
// every readable value beats a trigger type per value: adding a new one is a
// line here and a line in the name table, and authors learn one shape.
enum class StateField {
    Crew,
    Gold,       // undivided plunder
    Morale,     // 0 (mutinous) .. 4 (devoted)
    Months,     // months at sea this voyage
};

struct Trigger {
    TriggerType type     = TriggerType::None;
    int  seconds  = 0;     // ElapsedSailing
    int  distance = 0;     // NearPort: fire inside this
    int  rearm    = 0;     // NearPort: re-arm once further out than this
    StateField field = StateField::Crew;  // StateCrosses
    int  below    = 0;     // StateCrosses: fire when the value drops under this
    int  above    = 0;     // StateCrosses: ... or rises over this
    bool useBelow = false; // which of the two was authored
    bool once     = false; // fire at most once per career
    int  cooldown = 0;     // seconds before this event may fire again
};

// How an event presents itself. These are genuinely different kinds of thing,
// not a style flag: a Choice interrupts the player and asks something; a Notice
// is a passing piece of information that never takes control away.
enum class EventKind {
    Choice,   // modal card with selectable options; the default
    Notice,   // on-screen text while sailing, no options, no interruption
};

struct Event {
    EventKind           kind = EventKind::Choice;
    std::string         id;
    std::string         body;
    std::vector<Arg>    bodyArgs;
    std::vector<Option> options;      // Choice only
    Trigger             trigger;
    int                 seconds = 4;  // Notice: how long it stays on screen
    bool                anchorShip = false;  // Notice: track the player's ship
};

// ------------------------------------------------------------------ parsing
inline bool IsAscii(const std::string& s, int* badIndex)
{
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 && c != '\n' && c != '\t') { *badIndex = (int)i; return false; }
        if (c > 0x7E)                           { *badIndex = (int)i; return false; }
    }
    return true;
}

// Count tokens that consume a vararg. Deliberately conservative: anything
// starting with '@' that is not on the allow-list is reported so the author is
// told rather than silently risking the stack.
inline int CountTokens(const std::string& s, std::string* unknownToken)
{
    // Token -> how many argument slots it consumes. @CITYNAME takes THREE:
    // it is a three-word name record, not a string pointer. Longest first, so
    // @NATIONALITY is never mistaken for a prefix of something shorter.
    struct TokenSlots { const char* name; int slots; };
    static const TokenSlots kConsuming[] = {
        { "@NATIONALITY", 1 },
        { "@CITYNAME",    3 },
        { "@LOCTYPE",     1 },
        { "@HAPPY",       1 },
        { "@NUM",         1 },
    };
    int count = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '@') continue;
        bool matched = false;
        for (const TokenSlots& t : kConsuming) {
            size_t len = strlen(t.name);
            if (s.compare(i, len, t.name) == 0) {
                count += t.slots; i += len - 1; matched = true; break;
            }
        }
        if (!matched) {
            size_t end = i + 1;
            while (end < s.size() && (isupper((unsigned char)s[end]))) ++end;
            *unknownToken = s.substr(i, end - i);
            return -1;
        }
    }
    return count;
}

inline bool ParseArg(const json& j, Arg* out, std::string* err)
{
    if (j.is_number_integer()) {
        out->source = ArgSource::Literal;
        out->literal = j.get<int>();
        return true;
    }
    if (!j.is_string()) { *err = "argument must be a string or an integer"; return false; }
    std::string s = j.get<std::string>();
    if      (s == "crew")    out->source = ArgSource::Crew;
    else if (s == "morale")  out->source = ArgSource::Morale;
    else if (s == "plunder") out->source = ArgSource::Plunder;
    else if (s == "months")  out->source = ArgSource::Months;
    else if (s == "nearestCity")       out->source = ArgSource::NearestCity;
    else if (s == "nearestCityNation") out->source = ArgSource::NearestCityNation;
    else if (s == "nearestCityType")   out->source = ArgSource::NearestCityType;
    else {
        *err = "unknown argument source '" + s +
               "' (expected crew, morale, plunder, months, nearestCity, "
               "nearestCityNation, nearestCityType, or an integer)";
        return false;
    }
    return true;
}

// ------------------------------------------------------------- placeholders
// The authoring layer PEMF puts over the engine's own tokens.
//
// Writing directly against the engine means knowing that @HAPPY wants a 0-4
// mood value, that @CITYNAME wants three arguments rather than one, and that
// getting either wrong reads stack garbage. That is a reasonable thing to ask
// of this codebase and an unreasonable thing to ask of someone writing a line
// of dialogue. So an author can instead write:
//
//     "body": "Land ho! {port} off the bow!"
//
// with NO args at all. Each placeholder carries both halves of the contract --
// the engine token and where its value comes from -- so the slot arithmetic
// stops being the author's problem. {port} expanding to three arguments is
// invisible from the outside, which is the point.
struct Placeholder {
    const char* name;
    const char* token;
    ArgSource   source;
};

inline const Placeholder kPlaceholders[] = {
    { "crew",       "@NUM",         ArgSource::Crew              },
    { "morale",     "@HAPPY",       ArgSource::Morale            },
    { "gold",       "@NUM",         ArgSource::Plunder           },
    { "months",     "@NUM",         ArgSource::Months            },
    { "port",       "@CITYNAME",    ArgSource::NearestCity       },
    { "portNation", "@NATIONALITY", ArgSource::NearestCityNation },
    { "portType",   "@LOCTYPE",     ArgSource::NearestCityType   },
};

inline std::string PlaceholderNames()
{
    std::string s;
    for (const Placeholder& p : kPlaceholders) {
        if (!s.empty()) s += ", ";
        s += "{"; s += p.name; s += "}";
    }
    return s;
}

// Rewrite {placeholders} into engine tokens, building the argument list as we
// go so the two cannot fall out of step. Returns false with a precise reason.
//
// `{{` is a literal brace, so text that genuinely wants one is still writable.
inline bool ExpandPlaceholders(const std::string& what, std::string* text,
                               std::vector<Arg>* args, std::string* err)
{
    const bool hadExplicitArgs = !args->empty();
    std::string out;
    std::vector<Arg> built;
    out.reserve(text->size());

    for (size_t i = 0; i < text->size(); ) {
        if ((*text)[i] != '{') { out += (*text)[i++]; continue; }
        if (i + 1 < text->size() && (*text)[i + 1] == '{') {
            out += '{'; i += 2; continue;             // escaped brace
        }
        const size_t end = text->find('}', i);
        if (end == std::string::npos) {
            *err = what + ": unclosed '{' -- write '{{' for a literal brace";
            return false;
        }
        const std::string name = text->substr(i + 1, end - i - 1);
        const Placeholder* found = nullptr;
        for (const Placeholder& p : kPlaceholders)
            if (name == p.name) { found = &p; break; }
        if (!found) {
            *err = what + ": unknown placeholder '{" + name + "}'. Available: " +
                   PlaceholderNames();
            return false;
        }
        out += found->token;
        Arg a; a.source = found->source;
        built.push_back(a);
        i = end + 1;
    }

    if (built.empty()) return true;          // nothing to do; leave text alone
    if (hadExplicitArgs) {
        *err = what + ": don't mix {placeholders} with an explicit 'args' list "
               "-- placeholders supply their own values. Use one or the other.";
        return false;
    }
    *text = out;
    *args = built;
    return true;
}

inline bool ParseArgs(const json& j, std::vector<Arg>* out, std::string* err)
{
    if (j.is_null()) return true;
    if (!j.is_array()) { *err = "args must be an array"; return false; }
    if (j.size() > kMaxArgs) {
        *err = "too many args (" + std::to_string(j.size()) + " > " +
               std::to_string(kMaxArgs) + ")";
        return false;
    }
    for (const auto& e : j) {
        Arg a;
        if (!ParseArg(e, &a, err)) return false;
        out->push_back(a);
    }
    return true;
}

inline bool ParseTrigger(const json& j, Trigger* out, std::string* err)
{
    if (j.is_null()) return true;                 // hand-fired only
    if (!j.is_object()) { *err = "trigger must be an object"; return false; }

    std::string type = j.value("type", "");
    out->once     = j.value("once", false);
    out->cooldown = j.value("cooldown", 0);

    if (type == "elapsedSailing") {
        out->type = TriggerType::ElapsedSailing;
        out->seconds = j.value("seconds", 0);
        if (out->seconds <= 0) {
            *err = "trigger 'elapsedSailing' needs a positive 'seconds'";
            return false;
        }
    } else if (type == "nearPort") {
        out->type = TriggerType::NearPort;
        out->distance = j.value("distance", 0);
        if (out->distance <= 0) {
            *err = "trigger 'nearPort' needs a positive 'distance'";
            return false;
        }
        // Re-arm further out than we fire, so hovering on the boundary cannot
        // retrigger every frame.
        out->rearm = j.value("rearm", out->distance * 2);
        if (out->rearm <= out->distance) {
            *err = "trigger 'nearPort': 'rearm' must be greater than 'distance'";
            return false;
        }
    } else if (type == "stateCrosses") {
        out->type = TriggerType::StateCrosses;

        const std::string field = j.value("field", "");
        if      (field == "crew")   out->field = StateField::Crew;
        else if (field == "gold")   out->field = StateField::Gold;
        else if (field == "morale") out->field = StateField::Morale;
        else if (field == "months") out->field = StateField::Months;
        else {
            *err = "trigger 'stateCrosses' needs a 'field' of crew, gold, "
                   "morale or months (got '" + field + "')";
            return false;
        }

        const bool hasBelow = j.contains("below"), hasAbove = j.contains("above");
        if (hasBelow == hasAbove) {
            *err = "trigger 'stateCrosses' needs exactly one of 'below' or "
                   "'above'";
            return false;
        }
        out->useBelow = hasBelow;
        if (hasBelow) out->below = j.value("below", 0);
        else          out->above = j.value("above", 0);

        if (out->field == StateField::Morale) {
            const int v = hasBelow ? out->below : out->above;
            if (v < 0 || v > 4) {
                *err = "trigger 'stateCrosses' on 'morale' takes 0..4 "
                       "(0 mutinous, 4 devoted)";
                return false;
            }
        }
    } else {
        *err = "unknown trigger type '" + type +
               "' (expected elapsedSailing, nearPort or stateCrosses)";
        return false;
    }
    return true;
}

inline bool ParseEffect(const json& j, Effect* out, std::string* err)
{
    if (!j.is_object() || !j.contains("op")) {
        *err = "effect must be an object with an 'op'"; return false;
    }
    std::string op = j.value("op", "");
    if      (op == "addPlunder") out->op = EffectOp::AddPlunder;
    else if (op == "setPlunder") out->op = EffectOp::SetPlunder;
    else if (op == "addCrew")    out->op = EffectOp::AddCrew;
    else if (op == "setCrew")    out->op = EffectOp::SetCrew;
    else {
        *err = "unknown effect op '" + op +
               "' (expected addPlunder, setPlunder, addCrew, setCrew)";
        return false;
    }
    if (!j.contains("value") || !j["value"].is_number_integer()) {
        *err = "effect '" + op + "' needs an integer 'value'"; return false;
    }
    out->value = j["value"].get<int>();
    return true;
}

// Validate one text field: ASCII, and token count matching the args given.
inline bool ValidateText(const std::string& what, const std::string& text,
                         size_t argCount, std::string* err)
{
    int bad = 0;
    if (!IsAscii(text, &bad)) {
        *err = what + ": non-ASCII or control character at offset " +
               std::to_string(bad) +
               " (the 2004 engine is ASCII only -- check for smart quotes)";
        return false;
    }
    if (text.size() > kMaxBodyChars) {
        *err = what + ": " + std::to_string(text.size()) + " chars exceeds " +
               std::to_string(kMaxBodyChars);
        return false;
    }
    std::string unknown;
    int tokens = CountTokens(text, &unknown);
    if (tokens < 0) {
        *err = what + ": token '" + unknown +
               "' is not supported. Use @NUM, @HAPPY, @CITYNAME, "
               "@NATIONALITY or @LOCTYPE -- other tokens consume stack "
               "arguments in ways that have not been verified.";
        return false;
    }
    // The vararg buffer is fixed at kMaxArgs. Anything past it is silently
    // dropped when the args are resolved, which would leave the trailing
    // tokens reading stack garbage -- exactly what this validation exists to
    // prevent. Easy to hit now that one @CITYNAME costs three slots, so it is
    // checked rather than assumed.
    if (tokens > kMaxArgs) {
        *err = what + ": tokens need " + std::to_string(tokens) +
               " argument slots, but at most " + std::to_string(kMaxArgs) +
               " are available. Remember @CITYNAME costs three each.";
        return false;
    }
    if ((size_t)tokens != argCount) {
        *err = what + ": tokens need " + std::to_string(tokens) +
               " argument slot(s) but " + std::to_string(argCount) +
               " were supplied. These must match exactly -- a surplus token "
               "reads stack garbage. Note @CITYNAME needs THREE slots, which "
               "one \"nearestCity\" arg supplies.";
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ library
inline std::vector<Event> g_events;

inline const Event* Get(int index)
{
    if (index < 0 || index >= (int)g_events.size()) return nullptr;
    return &g_events[index];
}

inline int FindByIdIndex(const std::string& id)
{
    for (size_t i = 0; i < g_events.size(); ++i)
        if (g_events[i].id == id) return (int)i;
    return -1;
}

inline int Count() { return (int)g_events.size(); }

// Load and validate ONE file, APPENDING to the library. Returns how many events
// it contributed. A malformed file never throws into the game: it is reported
// and skipped, so one bad mod cannot take the others down with it.
inline int LoadFile(const char* path)
{
    const size_t before = g_events.size();

    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) {
        Log("content: no event file at %s", path);
        return 0;
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
        Log("content: PARSE ERROR in %s", path);
        Log("content:   %s", e.what());
        return 0;
    }

    if (!root.contains("events") || !root["events"].is_array()) {
        Log("content: %s has no top-level 'events' array", path);
        return 0;
    }

    int rejected = 0;
    for (const auto& je : root["events"]) {
        Event ev;
        std::string err;
        std::string id = je.value("id", "");

        auto fail = [&](const std::string& why) {
            Log("content: REJECTED event '%s' -- %s",
                id.empty() ? "<no id>" : id.c_str(), why.c_str());
            ++rejected;
        };

        if (id.empty())                  { fail("missing 'id'"); continue; }
        if (FindByIdIndex(id) >= 0)      { fail("duplicate id"); continue; }
        ev.id = id;

        ev.body = je.value("body", "");
        if (ev.body.empty())             { fail("missing 'body'"); continue; }
        if (!ParseArgs(je.contains("args") ? je["args"] : json(),
                       &ev.bodyArgs, &err)) { fail(err); continue; }
        if (!ExpandPlaceholders("body", &ev.body, &ev.bodyArgs, &err)) {
            fail(err); continue;
        }
        if (!ValidateText("body", ev.body, ArgSlots(ev.bodyArgs), &err)) {
            fail(err); continue;
        }

        if (!ParseTrigger(je.contains("trigger") ? je["trigger"] : json(),
                          &ev.trigger, &err)) { fail(err); continue; }

        const std::string kind = je.value("kind", "choice");
        if      (kind == "choice") ev.kind = EventKind::Choice;
        else if (kind == "notice") ev.kind = EventKind::Notice;
        else { fail("unknown kind '" + kind + "' (expected choice or notice)");
               continue; }

        if (ev.kind == EventKind::Notice) {
            // A notice never interrupts, so options make no sense on one.
            if (je.contains("options")) {
                fail("a 'notice' cannot have options -- it is informational "
                     "only. Use kind 'choice' if the player must decide.");
                continue;
            }
            ev.seconds = je.value("seconds", 4);
            if (ev.seconds < 1 || ev.seconds > 30) {
                fail("notice 'seconds' must be between 1 and 30");
                continue;
            }
            // "anchor": "screen" (default) pins the line to the top of the
            // screen; "ship" hangs it over the player's vessel in the world,
            // the way the game labels other ships, so it tracks as you sail.
            {
                std::string anchor = je.value("anchor", "screen");
                if (anchor == "ship")        ev.anchorShip = true;
                else if (anchor != "screen") {
                    fail("notice 'anchor' must be \"screen\" or \"ship\"");
                    continue;
                }
            }
            g_events.push_back(ev);
            continue;
        }

        if (!je.contains("options") || !je["options"].is_array() ||
            je["options"].empty()) {
            fail("needs a non-empty 'options' array"); continue;
        }
        if (je["options"].size() > kMaxOptions) {
            fail("more than " + std::to_string(kMaxOptions) + " options");
            continue;
        }

        bool ok = true;
        for (const auto& jo : je["options"]) {
            Option op;
            op.text = jo.value("text", "");
            if (op.text.empty()) { fail("an option has no 'text'"); ok = false; break; }
            if (!ValidateText("option text", op.text, 0, &err)) {
                fail(err); ok = false; break;
            }
            if (jo.contains("effects")) {
                if (!jo["effects"].is_array()) {
                    fail("'effects' must be an array"); ok = false; break;
                }
                for (const auto& jf : jo["effects"]) {
                    Effect ef;
                    if (!ParseEffect(jf, &ef, &err)) { fail(err); ok = false; break; }
                    op.effects.push_back(ef);
                }
                if (!ok) break;
            }
            op.outcome = jo.value("outcome", "");
            if (!ParseArgs(jo.contains("outcomeArgs") ? jo["outcomeArgs"] : json(),
                           &op.outcomeArgs, &err)) { fail(err); ok = false; break; }
            if (!ExpandPlaceholders("outcome", &op.outcome, &op.outcomeArgs,
                                    &err)) { fail(err); ok = false; break; }
            if (!op.outcome.empty() &&
                !ValidateText("outcome", op.outcome, ArgSlots(op.outcomeArgs), &err)) {
                fail(err); ok = false; break;
            }
            ev.options.push_back(op);
        }
        if (!ok) continue;

        g_events.push_back(ev);
    }

    const int added = (int)(g_events.size() - before);
    const char* leaf = strrchr(path, '\\');
    Log("content:   %-28s %d event(s)%s", leaf ? leaf + 1 : path, added,
        rejected ? (" (" + std::to_string(rejected) + " rejected)").c_str() : "");
    return added;
}

// Load every *.json in a folder. This is what makes PEMF a framework rather
// than a mod: each add-on ships its own file, they are loaded side by side, and
// a broken one is skipped with a reason instead of breaking the rest.
inline int LoadFolder(const char* dir)
{
    g_events.clear();

    char pattern[MAX_PATH];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*.json", dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        Log("content: no event files found in %s", dir);
        return 0;
    }

    Log("content: scanning %s", dir);
    int files = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char full[MAX_PATH];
        _snprintf_s(full, sizeof(full), _TRUNCATE, "%s\\%s", dir, fd.cFileName);
        LoadFile(full);
        ++files;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    Log("content: %d event(s) from %d file(s)", (int)g_events.size(), files);
    return (int)g_events.size();
}

// -------------------------------------------------------------- execution
// An event's outcome text, held between the choice being made and the next
// frame, when it is presented. Resolved values are stored, not Arg sources, so
// the numbers reflect the state at the moment the choice was made.
struct PendingOutcome {
    std::string text;
    int         args[kMaxArgs] = {0};
    int         argCount = 0;
};

inline PendingOutcome g_outcome;

inline void ShowPendingOutcome(int)
{
    if (g_outcome.text.empty()) return;
    game::ShowModalTextN(g_outcome.text.c_str(), g_outcome.args,
                         g_outcome.argCount);
    g_outcome.text.clear();
}

// Run one event by INDEX. Called from the safe point via the event queue --
// never directly from a trigger.
//
// Index rather than pointer is deliberate: a content reload replaces the vector,
// and a queued raw pointer would dangle.
// ------------------------------------------------------------- notices
// A notice is drawn every frame until it expires, which is how the game's own
// HUD text works -- there is no timed-message call to borrow. The text is
// composed once, here, and re-drawn from the render hook.
// ------------------------------------------------------------- channels
// Not all on-screen text wants the same behaviour, and treating it as one kind
// produced a wall of stacked lines during testing.
//
//   Narrative  a lookout's call, an observation. Several can be true at once,
//              so they STACK, oldest pushed off, exactly as before.
//   Status     what the framework is doing right now -- which flag is flying,
//              what a debug key just did. Only the LATEST is ever meaningful,
//              so a new one REPLACES the old in place rather than queueing
//              behind it.
//
// The channel is a property of the message, not of the caller, so anything can
// post to either. Adding a third (warnings, say) is one enum value and one
// line in PostToChannel.
enum NoticeChannel {
    kChannelNarrative = 0,
    kChannelStatus    = 1,
};

struct ActiveNotice {
    std::string  resolved;         // @-tokens already substituted, at post time
    DWORD        posted  = 0;
    DWORD        until   = 0;
    bool         anchor  = false;  // true: track the player's ship in the world
    NoticeChannel channel = kChannelNarrative;
};

// How long an anchored notice spends easing out at the end of its life. The
// game ramps its own ship labels over roughly ten frames; a little longer
// reads as a deliberate fade rather than a dropped frame.
constexpr DWORD kNoticeFadeMs = 900;

constexpr int kMaxNotices = 3;
inline ActiveNotice g_notices[kMaxNotices];
inline int          g_noticeCount = 0;

inline void ClearNotices()
{
    g_noticeCount = 0;
}

// Find a slot for a notice on this channel. A status notice reuses the slot its
// predecessor held, so the newest simply overwrites the old one and nothing
// accumulates; a narrative notice takes a fresh slot, dropping the oldest when
// full -- a stale line is less useful than the one that just happened.
inline ActiveNotice* SlotFor(NoticeChannel channel)
{
    if (channel == kChannelStatus) {
        for (int i = 0; i < g_noticeCount; ++i)
            if (g_notices[i].channel == kChannelStatus) return &g_notices[i];
    }
    if (g_noticeCount >= kMaxNotices) {
        for (int i = 1; i < g_noticeCount; ++i) g_notices[i - 1] = g_notices[i];
        --g_noticeCount;
    }
    return &g_notices[g_noticeCount++];
}

// As above, but a notice whose text is ALREADY LIVE reuses that slot instead of
// taking a new one.
//
// Two notices reading the same words are not two pieces of news, and anchored
// ones share a world position, so a duplicate does not stack visibly -- it
// draws over itself and comes out bold and slightly wrong, which is a much
// worse symptom than a missing line because it looks like a font problem.
// (Observed with four copies of "Land ho! St. Eustatius off the bow!".)
//
// The cause of that particular pile-up is fixed in triggers.h, where the event
// should never have fired four times. This is the guard rail underneath it:
// whatever fires, identical text can only ever occupy one slot, and reposting
// refreshes its clock.
inline ActiveNotice* SlotForText(NoticeChannel channel, const char* text)
{
    if (text) {
        for (int i = 0; i < g_noticeCount; ++i)
            if (g_notices[i].channel == channel && g_notices[i].resolved == text)
                return &g_notices[i];
    }
    return SlotFor(channel);
}

// A notice's life is measured in wall-clock time, which quietly meant it kept
// running down while nobody could see it: open a menu with a notice up, stay a
// while, and it had expired by the time you came back. The player loses a
// lookout's call to having glanced at the map.
//
// So the clock is HELD whenever the overworld is not on screen. Every frame the
// world is not live, both timestamps move forward by exactly the time that
// passed, which leaves the REMAINING time untouched -- a notice resumes with
// what it had left rather than restarting or vanishing. Called once per frame
// from the safe point, next to where g_worldLive is decided.
inline DWORD g_noticeClockAt = 0;

inline void HoldNoticeClock(bool worldLive)
{
    const DWORD now = GetTickCount();
    if (g_noticeClockAt && !worldLive) {
        const DWORD held = now - g_noticeClockAt;
        for (int i = 0; i < g_noticeCount; ++i) {
            g_notices[i].posted += held;
            g_notices[i].until  += held;
        }
    }
    g_noticeClockAt = now;
}

// Called from the RENDER phase, every frame. Draws whatever is live and drops
// whatever has expired.
// Drawing state, reported from the safe point rather than logged in place --
// the render hook stays free of allocation and file I/O.
inline volatile LONG g_drawOk      = 0;  // screen notices drawn without incident
inline volatile LONG g_drawWorldOk = 0;  // anchored notices drawn ditto
inline volatile LONG g_drawFaults  = 0;  // draws that raised an exception
inline bool          g_drawOff      = false; // screen phase latched off
inline bool          g_drawWorldOff = false; // world phase latched off

// The suspicion panel, top right, where the game parks nothing. The strings are
// composed at the safe point (suspicion::RefreshPanel) so this only blits.
//
// Right-aligned by eye rather than by measuring: the engine's text call centres
// on the x we hand it, and there is no width query. An x of three-quarters
// across sits the block clear of the compass and the button grid at every
// resolution tested.
// Returns true if it put anything on screen -- which matters, because drawing
// at all leaves our text in the game's shared buffer and the sailing render
// will paint whatever is left there across the middle of the sea next frame.
// The panel produced exactly that: "She's coming about" in enormous letters
// over the water, once per frame, because the buffer was only cleared when a
// NOTICE had drawn and the panel was not counted.
inline bool DrawSuspicionPanel()
{
    if (suspicion::g_panelLines <= 0) return false;
    __try {
        // Right-hand side, and BELOW the notice band -- the first placement put
        // it at y=12 where the centred notices are, and the two overlapped into
        // an unreadable smear at the top of the screen.
        const int x = (game::ScreenW() * 7) / 9;
        int y = 96;
        for (int i = 0; i < suspicion::g_panelLines; ++i) {
            game::DrawHudLineAt(suspicion::g_panel[i], x, y);
            y += 22;
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // One bad panel draw should cost the panel, never the frame.
        suspicion::g_panelLines = 0;
        return false;
    }
}

// Draw one line. Wrapped so the SEH frame contains nothing with a destructor.
inline bool DrawScreenNotice(const char* text, int y)
{
    __try {
        game::ShowNotice(text, y, game::kNoticeWhite);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Anchored over the player's ship, at the ship's live map position, so it
// follows the vessel for free -- the game re-projects the label every frame
// from the world coordinates we hand it.
inline bool DrawOneWorldNotice(const char* text, int fade)
{
    __try {
        int mx = 0, my = 0;
        game::PlayerMapPos(&mx, &my);
        // The lift above the hull is part of the label geometry, so it lives
        // with the rest of it in ShowWorldText.
        game::ShowWorldText(text, mx, my, fade);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The game's seventh argument, driven over a notice's life: full for most of
// it, easing to nothing over the last kNoticeFadeMs.
inline int NoticeFade(const ActiveNotice& n, DWORD now)
{
    const int left = (int)(n.until - now);
    if (left <= 0)                    return 0;
    if (left >= (int)kNoticeFadeMs)   return game::addr::kWorldTextA7Max;
    return (int)((__int64)left * game::addr::kWorldTextA7Max / kNoticeFadeMs);
}

// Set at the safe point: the overworld is on screen, with the engine's label
// manager built. Gates BOTH phases -- a notice belongs to the sailing view, and
// one painted over the Load/Save screen or the pause menu is a bug whether it
// is anchored in the world or pinned to the top of the screen.
//
// A notice's clock is HELD while this is false (see HoldNoticeClock), so one
// posted just before a menu is opened resumes with its remaining time rather
// than expiring unseen behind it.
inline bool g_worldLive = false;

// ...and the screen signature it was decided FROM. This flag is published once
// per main-loop iteration, but the render hook runs thousands of times a
// second -- a town entry, a menu, a battle all begin BETWEEN two safe points,
// and until the next one the flag still says "the overworld is on screen".
//
// That window is not theoretical. It blanked the town screen: entering port
// with a notice live left g_worldLive latched true, DrawNotices went on running
// against the town, and its buffer clear below wiped the text the town screen
// had just composed. The screen was fine the moment nothing was posted, which
// is what made it look like a notice bug rather than a staleness bug.
//
// So the flag is not trusted on its own. The signature it was taken with is
// recorded next to it, and the render phase checks that the screen is STILL
// that one before drawing or clearing anything. Two int reads per frame, and
// it closes the whole class rather than this one instance of it.
inline int   g_worldLiveId    = 0;
inline int   g_worldLiveDepth = 0;
inline DWORD g_worldLiveAt    = 0;

// How stale the published flag may be before the render phase stops trusting
// it. The main loop runs many times a second, so anything approaching this is
// not a slow frame -- it is the loop not running at all.
constexpr DWORD kWorldLiveMaxAgeMs = 250;

inline void PublishWorldLive(bool live, int screenId, int screenDepth)
{
    g_worldLive      = live;
    g_worldLiveId    = screenId;
    g_worldLiveDepth = screenDepth;
    g_worldLiveAt    = GetTickCount();
}

// Called from the render hook. False the moment the screen has changed out from
// under the published flag -- or the moment the flag is simply too old.
//
// The screen check alone was not enough. A MODAL DIALOG ("Which ship shall we
// attack with our flagship?") is drawn over the sailing view without changing
// the screen signature, and it runs its own loop, so the safe point stops
// ticking and the flag stays true. Notices went on drawing over the top of the
// game's own prompt.
//
// The age check catches that and everything like it, because it does not care
// WHAT blocked the main loop. If the loop is not running, no decision it made
// is worth acting on, and a stale "yes" is the most dangerous answer there is.
inline bool WorldStillOnScreen()
{
    if (!g_worldLive) return false;
    if (GetTickCount() - g_worldLiveAt > kWorldLiveMaxAgeMs) return false;
    __try {
        return *(const int*)game::addr::ScreenId    == g_worldLiveId &&
               *(const int*)game::addr::ScreenDepth == g_worldLiveDepth;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The world phase, issued at BeginScene in the last render pass of the frame.
// Anchored notices only: they build scene-graph nodes, so they have to exist
// before the render walk that draws them.
//
// A fault latches the WORLD phase off and leaves screen notices alone. The two
// are latched separately on purpose -- screen text is long proven, world text
// is newer, and one failing is no reason to lose the other.
inline void DrawWorldNotices()
{
    if (g_drawWorldOff || !WorldStillOnScreen() || g_noticeCount <= 0) return;

    DWORD now = GetTickCount();
    for (int i = 0; i < g_noticeCount; ++i) {
        ActiveNotice& n = g_notices[i];
        if (!n.anchor || (int)(now - n.until) >= 0) continue;
        if (DrawOneWorldNotice(n.resolved.c_str(), NoticeFade(n, now))) {
            InterlockedIncrement(&g_drawWorldOk);
        } else {
            InterlockedIncrement(&g_drawFaults);
            g_drawWorldOff = true;
            return;
        }
    }
}

// The screen phase, issued at EndScene: fixed HUD lines, on top of everything.
// Notices have no timer of their own, so they are re-drawn every frame until
// they expire, and expiry is settled HERE -- once per frame, after both phases
// have had the list. A fault latches the screen phase off for the session
// rather than repeating every frame: one bad draw should cost a missing
// notice, never a crashing game. The safe point reports it.
inline void DrawNotices()
{
    if (g_drawOff || !WorldStillOnScreen() || g_noticeCount <= 0) return;

    DWORD now = GetTickCount();
    int y = 8;
    int write = 0;
    int drew = 0;
    for (int i = 0; i < g_noticeCount; ++i) {
        ActiveNotice& n = g_notices[i];
        if ((int)(now - n.until) >= 0) continue;          // expired
        if (!n.anchor) {
            if (DrawScreenNotice(n.resolved.c_str(), y)) {
                InterlockedIncrement(&g_drawOk);
                ++drew;
            } else {
                InterlockedIncrement(&g_drawFaults);
                g_drawOff = true;
                break;
            }
            y += 24;                 // anchored lines do not stack at the top
        }
        if (write != i) g_notices[write] = n;
        ++write;
    }
    if (!g_drawOff) g_noticeCount = write;

    // THE SHARED BUFFER IS A TRAP, AND IT BITES TWICE.
    //
    // We already knew composing into 0x00869B48 and leaving text there makes
    // the game redraw it over the player's ship. This is the same fault by a
    // different route: the engine's HUD drawer uses that buffer as its own
    // scratch, so simply DRAWING leaves our line in it -- we never composed
    // anything. The next frame's sailing render then paints it over the ship,
    // and because each notice overwrites a little of the last, the result is
    // several messages piled illegibly on top of one another.
    //
    // Clearing after our draws is safe: the game re-composes immediately
    // before each of its own draws, so an empty buffer between frames is
    // exactly the state it expects. This is the engine's own idiom for the job.
    //
    if (DrawSuspicionPanel()) ++drew;

    // Only when we actually DREW, though. The buffer is the game's, not ours,
    // and clearing it is a write into shared state: if this pass put nothing
    // in it there is nothing of ours to take out, and clearing anyway is a
    // write we cannot justify. Not every screen re-composes every frame, so a
    // gratuitous clear can throw away text nobody is going to write again.
    if (drew > 0) game::ClearMessageBuffer();
}

// Safe-point reporting for the draw path: says plainly whether our own text
// actually reached the screen, and stays quiet afterwards.
inline void ReportDrawFromSafePoint()
{
    static bool okLogged = false, worldLogged = false, faultLogged = false;
    if (!okLogged && g_drawOk > 0) {
        okLogged = true;
        Log("draw: notices are rendering through the frame hook (%ld draws)",
            g_drawOk);
    }
    if (!worldLogged && g_drawWorldOk > 0) {
        worldLogged = true;
        Log("draw: anchored notices are rendering in the world (%ld draws)",
            g_drawWorldOk);
    }
    if (!faultLogged && g_drawFaults > 0) {
        faultLogged = true;
        Log("draw: a notice draw faulted -- %s notices disabled for this "
            "session. The trigger and queue are unaffected.",
            g_drawWorldOff ? (g_drawOff ? "all" : "anchored")
                           : "on-screen");
    }
}

// A notice with no authored event behind it, for exercising the draw path.
// Debug and framework messages default to the STATUS channel, so pressing a
// key five times leaves one line saying what is true now rather than five
// saying what used to be.
inline void PostDebugNotice(const char* text, int seconds, bool anchor = false,
                            NoticeChannel channel = kChannelStatus)
{
    ActiveNotice& n = *SlotForText(channel, text);
    n.resolved = text;
    n.anchor   = anchor;
    n.channel  = channel;
    n.posted   = GetTickCount();
    n.until    = n.posted + (DWORD)seconds * 1000;
    Log("debug: notice posted -- '%s' for %d s (%s, %s)", text, seconds,
        anchor ? "anchored to the ship" : "top of screen",
        channel == kChannelStatus ? "status" : "narrative");
}

inline void PostNotice(const Event& ev)
{
    // Resolve the text ONCE, here at the safe point, rather than every frame
    // inside the render hook: composing uses the game's shared message buffer,
    // which is not something to be touching mid-frame.
    //
    // Resolved BEFORE a slot is chosen, because the resolved words are what
    // decides whether this is news or a repeat -- two firings of the same event
    // for two different ports are two notices, and for the same port they are
    // one.
    int  args[kMaxArgs] = {0};
    int  argc = ResolveArgs(ev.bodyArgs, args, kMaxArgs);
    char buf[512];
    game::ComposeText(ev.body.c_str(), args, argc, buf, sizeof(buf));

    // Authored notices are narrative: several can be true at once, so they
    // stack and the oldest is dropped when full.
    ActiveNotice& n = *SlotForText(kChannelNarrative, buf);
    n.channel  = kChannelNarrative;
    n.resolved = buf;
    n.anchor   = ev.anchorShip;
    n.posted = GetTickCount();
    n.until  = n.posted + (DWORD)ev.seconds * 1000;
    Log("  notice '%s' for %d s", ev.id.c_str(), ev.seconds);
    if (!render::WantsNotices() && !d3d9hook::WantsNotices()) {
        Log("  !! it will NOT be visible: drawing needs a render hook at stage "
            "2+ (render=%d, d3d9=%d). The trigger itself fired correctly.",
            render::kStage, d3d9hook::kStage);
    }
}

inline void Fire(int index)
{
    const Event* ev = Get(index);
    if (!ev) { Log("content: fire index %d out of range", index); return; }

    // A notice never interrupts: post it and return immediately.
    if (ev->kind == EventKind::Notice) { PostNotice(*ev); return; }

    state::Snapshot before = state::Capture();
    Log("  event '%s' (crew=%d morale=%d plunder=%d)",
        ev->id.c_str(), before.crew, before.morale, before.plunder);

    int args[kMaxArgs] = {0};
    const int argc = ResolveArgs(ev->bodyArgs, args, kMaxArgs);

    const char* opts[kMaxOptions] = {nullptr};
    int nOpts = 0;
    for (const auto& o : ev->options) {
        if (nOpts >= kMaxOptions) break;
        opts[nOpts++] = o.text.c_str();
    }

    int choice = game::AskChoiceN(ev->body.c_str(), opts, nOpts,
                                  args, argc, 0x2C);

    if (choice < 0 || choice >= nOpts) {
        Log("  '%s': dismissed (returned %d)", ev->id.c_str(), choice);
        return;
    }
    const Option& picked = ev->options[choice];
    Log("  '%s': chose %d '%s'", ev->id.c_str(), choice, picked.text.c_str());

    // Effects always go through the validated layer: clamped, career-gated,
    // and logged with the event id so the footprint is traceable.
    for (const auto& ef : picked.effects) {
        std::string why = ev->id + ":" + std::to_string(choice);
        switch (ef.op) {
            case EffectOp::AddPlunder: state::AddPlunder(ef.value, why.c_str()); break;
            case EffectOp::SetPlunder: state::SetPlunder(ef.value, why.c_str()); break;
            case EffectOp::AddCrew:    state::AddCrew(ef.value,    why.c_str()); break;
            case EffectOp::SetCrew:    state::SetCrew(ef.value,    why.c_str()); break;
        }
    }

    state::Snapshot after = state::Capture();
    state::LogDelta("event effect", before, after);

    // The outcome is presented on the NEXT frame, not this one. Two dialogs in
    // a single frame leaves the second compositing over a stale backbuffer, so
    // the world behind it renders half-drawn. Arguments are resolved now --
    // after the effects above -- and the text is shown once the game has drawn
    // a frame in between.
    if (!picked.outcome.empty()) {
        g_outcome.text = picked.outcome;
        g_outcome.argCount = ResolveArgs(picked.outcomeArgs, g_outcome.args,
                                         kMaxArgs);
        events::PostFollowUp(&ShowPendingOutcome, 0, "outcome");
    }
}

} // namespace content
