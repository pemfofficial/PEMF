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
//   * only @NUM and @HAPPY are permitted. Other tokens exist (@CITYNAME,
//     @NATIONALITY, ...) but we have not verified whether they consume an
//     argument, so allowing them would be guessing with the stack.
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
#include "render.h"
#include "d3d9hook.h"

namespace content {

using json = nlohmann::json;

// ------------------------------------------------------------------ limits
constexpr int kMaxOptions   = 6;      // beyond this the card stops being readable
constexpr int kMaxArgs      = 8;      // must not exceed game::kMaxTextArgs
constexpr int kMaxBodyChars = 1200;   // prompt budget shared with option lines

// --------------------------------------------------------------- arg source
// Where a token's value comes from at fire time.
enum class ArgSource { Literal, Crew, Morale, Plunder, Months };

struct Arg {
    ArgSource source = ArgSource::Literal;
    int       literal = 0;

    int Resolve() const
    {
        switch (source) {
            case ArgSource::Crew:    return state::Crew();
            case ArgSource::Morale:  return state::Morale();
            case ArgSource::Plunder: return state::Plunder();
            case ArgSource::Months:  return state::Months();
            default:                 return literal;
        }
    }
};

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
};

struct Trigger {
    TriggerType type     = TriggerType::None;
    int  seconds  = 0;     // ElapsedSailing
    int  distance = 0;     // NearPort: fire inside this
    int  rearm    = 0;     // NearPort: re-arm once further out than this
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
    static const char* kConsuming[] = { "@NUM", "@HAPPY" };
    int count = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '@') continue;
        bool matched = false;
        for (const char* t : kConsuming) {
            size_t len = strlen(t);
            if (s.compare(i, len, t) == 0) {
                ++count; i += len - 1; matched = true; break;
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
    else {
        *err = "unknown argument source '" + s +
               "' (expected crew, morale, plunder, months, or an integer)";
        return false;
    }
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
    } else {
        *err = "unknown trigger type '" + type +
               "' (expected elapsedSailing or nearPort)";
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
               "' is not supported. Only @NUM and @HAPPY may be used -- other "
               "tokens may consume a stack argument and have not been verified.";
        return false;
    }
    if ((size_t)tokens != argCount) {
        *err = what + ": " + std::to_string(tokens) + " token(s) but " +
               std::to_string(argCount) + " arg(s) supplied. These must match "
               "exactly -- a surplus token reads stack garbage.";
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
        if (!ValidateText("body", ev.body, ev.bodyArgs.size(), &err)) {
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
            if (!op.outcome.empty() &&
                !ValidateText("outcome", op.outcome, op.outcomeArgs.size(), &err)) {
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
struct ActiveNotice {
    std::string resolved;          // @-tokens already substituted, at post time
    DWORD       posted = 0;
    DWORD       until  = 0;
    bool        anchor = false;    // true: track the player's ship in the world
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

// Called from the RENDER phase, every frame. Draws whatever is live and drops
// whatever has expired.
// Drawing state, reported from the safe point rather than logged in place --
// the render hook stays free of allocation and file I/O.
inline volatile LONG g_drawOk      = 0;  // screen notices drawn without incident
inline volatile LONG g_drawWorldOk = 0;  // anchored notices drawn ditto
inline volatile LONG g_drawFaults  = 0;  // draws that raised an exception
inline bool          g_drawOff     = false;  // latched off after a fault

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
inline bool DrawWorldNotice(const char* text, int fade)
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

// Called from inside the frame (the D3D9 EndScene hook). Notices have no timer
// of their own, so they are re-drawn every frame until they expire.
//
// A fault here latches drawing OFF for the rest of the session instead of
// repeating every frame: one bad draw should cost a missing notice, never a
// crashing game. The safe point reports it.
// The world phase, issued at BeginScene: anchored notices only. They build
// scene-graph nodes, so they have to exist before the render walk.
// Set at the safe point: in a career, with the engine's label manager built.
// World text drawn outside those conditions is at best invisible and at worst
// touches a scene that does not exist yet.
inline bool g_worldLive = false;

inline void DrawWorldNotices()
{
    if (g_drawOff || !g_worldLive || g_noticeCount <= 0) return;

    DWORD now = GetTickCount();
    for (int i = 0; i < g_noticeCount; ++i) {
        ActiveNotice& n = g_notices[i];
        if (!n.anchor || (int)(now - n.until) >= 0) continue;
        if (DrawWorldNotice(n.resolved.c_str(), NoticeFade(n, now))) {
            InterlockedIncrement(&g_drawWorldOk);
        } else {
            InterlockedIncrement(&g_drawFaults);
            g_drawOff = true;
            return;
        }
    }
}

// The screen phase, issued at EndScene: fixed HUD lines, on top of everything.
// Expiry is settled here, once per frame, after both phases have had the list.
inline void DrawNotices()
{
    if (g_drawOff || g_noticeCount <= 0) return;

    DWORD now = GetTickCount();
    int y = 8;
    int write = 0;
    for (int i = 0; i < g_noticeCount; ++i) {
        ActiveNotice& n = g_notices[i];
        if ((int)(now - n.until) >= 0) continue;          // expired
        if (!n.anchor) {
            if (DrawScreenNotice(n.resolved.c_str(), y)) {
                InterlockedIncrement(&g_drawOk);
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
        Log("draw: a notice draw faulted -- drawing disabled for this session. "
            "The trigger and queue are unaffected.");
    }
}

// A notice with no authored event behind it, for exercising the draw path.
inline void PostDebugNotice(const char* text, int seconds, bool anchor = false)
{
    if (g_noticeCount >= kMaxNotices) {
        for (int i = 1; i < g_noticeCount; ++i) g_notices[i - 1] = g_notices[i];
        --g_noticeCount;
    }
    ActiveNotice& n = g_notices[g_noticeCount++];
    n.resolved = text;
    n.anchor   = anchor;
    n.posted   = GetTickCount();
    n.until    = n.posted + (DWORD)seconds * 1000;
    Log("debug: notice posted -- '%s' for %d s (%s)", text, seconds,
        anchor ? "anchored to the ship" : "top of screen");
}

inline void PostNotice(const Event& ev)
{
    if (g_noticeCount >= kMaxNotices) {
        // Drop the oldest rather than refuse the newest -- a stale notice is
        // less useful than the one that just happened.
        for (int i = 1; i < g_noticeCount; ++i) g_notices[i - 1] = g_notices[i];
        --g_noticeCount;
    }
    ActiveNotice& n = g_notices[g_noticeCount++];
    // Resolve the text ONCE, here at the safe point, rather than every frame
    // inside the render hook: composing uses the game's shared message buffer,
    // which is not something to be touching mid-frame.
    int  args[kMaxArgs] = {0};
    int  argc = 0;
    for (size_t i = 0; i < ev.bodyArgs.size() && i < kMaxArgs; ++i)
        args[argc++] = ev.bodyArgs[i].Resolve();
    char buf[512];
    game::ComposeText(ev.body.c_str(), args, argc, buf, sizeof(buf));
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
    for (size_t i = 0; i < ev->bodyArgs.size() && i < kMaxArgs; ++i)
        args[i] = ev->bodyArgs[i].Resolve();

    const char* opts[kMaxOptions] = {nullptr};
    int nOpts = 0;
    for (const auto& o : ev->options) {
        if (nOpts >= kMaxOptions) break;
        opts[nOpts++] = o.text.c_str();
    }

    int choice = game::AskChoiceN(ev->body.c_str(), opts, nOpts,
                                  args, (int)ev->bodyArgs.size(), 0x2C);

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
        g_outcome.argCount = 0;
        for (size_t i = 0; i < picked.outcomeArgs.size() && i < kMaxArgs; ++i)
            g_outcome.args[g_outcome.argCount++] = picked.outcomeArgs[i].Resolve();
        events::PostFollowUp(&ShowPendingOutcome, 0, "outcome");
    }
}

} // namespace content
