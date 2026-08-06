// townmenu.h - PEMF's own rows in the game's town menu.
//
// The town menu is FUN_00410D30. It composes exactly the way our own cards do
// -- AddText0 for the description, AddText1 per selectable row, ShowMessage to
// present -- and then dispatches on the index the player picked. The full map,
// with the disassembly it was read from, is in docs/GAME_API.md.
//
// ------------------------------------------------------ why a shim, not a hook
// We redirect ONE call: `call ShowMessage` at 0x0041191E. That is the whole
// mechanism. At that instant the game has finished composing every row into the
// shared message buffer and is about to present it, so it is the only point
// where our rows can join the list and our pick can be caught before the game
// acts on it. Same technique as the storm draw at 0x0046377A, and for the same
// reason: redirecting the game's own call puts us at exactly the right place in
// its sequence rather than at a place we chose.
//
// ⛔ WE CANNOT HAND THE GAME AN INDEX OF OUR OWN. Every value means something:
//
//     0  Talk to the Mayor/Chief/Abbot     3  Consult with the Shipwright
//     1  Visit the Tavern                  4  Divide the Plunder
//     2  Trade with the Merchant/etc       5  Check Status
//     6+ LEAVE the settlement
//
// and the dispatch guard is `CMP ESI,5` / `JA`, which is UNSIGNED -- so -1 and
// the polled form's -2 are "above 5" too and also leave. There is no sentinel
// for "nothing happened". A PEMF row whose index reached the dispatch would
// walk the player out of the settlement: not a crash, but something far worse
// to diagnose, because leaving town is a plausible thing for the game to have
// done and would never point back at us.
//
// So THE SHIM OWNS A LOOP. It handles our row itself and re-presents the menu,
// and the only value FUN_00410D30 ever receives is one the game produced.
//
// -------------------------------------------------------- where our rows sit
// Immediately BEFORE the leave row, which is always the last one the game
// composes (0x004118A8). That keeps every game action id stable -- only the
// leave row's index moves, and it was already "above 5" so it still routes to
// the same place. Putting ours after the leave row would work identically but
// reads wrong: "Leave Town" belongs at the bottom of a menu.
//
// ⚠️ The index the player picks is REMAPPED after we return, when the
// settlement is a village or mission (see GAME_API.md). We never have to think
// about it: we return the raw index untouched and let the game remap its own
// rows, and our own rows never get that far. Do not try to be clever here.
#pragma once
#include <windows.h>
#include <string.h>

#include "log.h"
#include "game.h"
#include "render.h"
#include "events.h"
#include "content.h"

namespace townmenu {

// The call site, and what it must be pointing at before we touch it.
constexpr uintptr_t kCallSite   = 0x0041191E;
constexpr uintptr_t kCallTarget = 0x00410C50;   // ShowMessage

constexpr int    kMaxRows    = 6;
constexpr int    kLabelLen   = 64;
constexpr int    kMaxLoops   = 32;     // belt and braces; see Present()

// ⚠️ THE GAME'S BUFFER CAPACITY IS NOT KNOWN. `MessageText` (0x00869B48) has
// never been measured, so this is a self-imposed ceiling rather than a reading
// of the engine. The menus the game composes run to roughly 200 bytes and our
// six rows can add at most ~400 more, so 1 KB is comfortable for the real case
// while staying far below anything that could plausibly overrun it.
//
// Everything we build is bounded to this before it goes anywhere near the
// game's memory, and Compose() refuses rather than truncates -- a half-written
// menu would be worse than no PEMF rows at all.
constexpr size_t kMsgMax     = 1024;

// A row PEMF adds. `eventIndex` is an index into content::g_events, or -1 for
// a row driven by native code.
struct Row {
    char label[kLabelLen] = {0};
    int  eventIndex = -1;
    int  menuIndex  = -1;   // opens a PEMF menu instead of firing an event
    void (*fn)(int) = nullptr;
    int  arg = 0;

    // Where this row is offered. -1 means "anywhere".
    //
    // `port` is the settlement's own index in the game's 128-slot table, which
    // is the only identifier that is stable AND unique -- names are not, since
    // several maps rename or move towns. It is not a friendly thing to author
    // by hand, so every menu logs the index it is showing for; enter the port
    // once, read pemf.log, put the number in the JSON.
    int  port   = -1;
    int  nation = -1;

    bool enabled = true;   // recomputed per menu from the two above
};

// A row may declare itself unavailable right now. Used by the crew menu, which
// has nothing to say without a crew.
//
// ⚠️ Gated on CREW, deliberately, and never on morale. Morale reads 0 whenever
// the crew's share is under what they expect, which is ordinary for a poor
// captain with a loyal crew -- greying the menu out there would hide it exactly
// when the player most wants it.
inline bool (*g_gate[kMaxRows])() = { nullptr };

inline Row  g_rows[kMaxRows];
inline int  g_rowCount = 0;
inline bool g_installed = false;
inline void* g_orig = nullptr;          // the real ShowMessage

// Diagnostics -- these are the numbers to ask for when something looks wrong.
inline volatile LONG g_presents = 0;    // menus we passed through
inline volatile LONG g_picks    = 0;    // times a PEMF row was chosen
inline bool g_faulted = false;          // latched off after a fault

inline void Clear() { g_rowCount = 0; }

inline bool Add(const char* label, int eventIndex, int menuIndex,
                void (*fn)(int), int arg, int port = -1, int nation = -1)
{
    if (!label || !*label) return false;
    if (g_rowCount >= kMaxRows) {
        Log("townmenu: full, dropping '%s'", label);
        return false;
    }
    Row& r = g_rows[g_rowCount++];
    // The game's convention: one leading space marks a line as selectable.
    // Getting this wrong does not fail loudly -- the row renders as prose and
    // simply cannot be picked, which reads as "my option did not appear".
    if (label[0] == ' ')
        strncpy_s(r.label, sizeof(r.label), label, _TRUNCATE);
    else
        _snprintf_s(r.label, sizeof(r.label), _TRUNCATE, " %s", label);
    r.eventIndex = eventIndex;
    r.menuIndex  = menuIndex;
    r.fn = fn;
    r.arg = arg;
    r.port = port;
    r.nation = nation;
    r.enabled = true;
    return true;
}

// Decide which rows this settlement gets. Called once per menu, before the
// buffer is composed, so a row that does not belong here never reaches the
// screen and never occupies an index.
inline void ApplyContext(int port, int nation)
{
    for (int i = 0; i < g_rowCount; ++i) {
        Row& r = g_rows[i];
        r.enabled = (r.port   < 0 || r.port   == port)
                 && (r.nation < 0 || r.nation == nation)
                 && (g_gate[i] == nullptr || g_gate[i]());
    }
}

inline int EnabledCount()
{
    int n = 0;
    for (int i = 0; i < g_rowCount; ++i) if (g_rows[i].enabled) ++n;
    return n;
}

// ------------------------------------------------------------- buffer surgery
// Options are lines of one string, each beginning with a single space. Counting
// them tells us how many rows the game composed, and finding the start of the
// LAST one tells us where ours go.
inline int CountOptionLines(const char* s, size_t* lastLineStart)
{
    int n = 0;
    size_t start = 0;
    if (lastLineStart) *lastLineStart = 0;
    for (size_t i = 0; s[i]; ++i) {
        if (i == start && s[i] == ' ') {
            ++n;
            if (lastLineStart) *lastLineStart = start;
        }
        if (s[i] == '\n') start = i + 1;
    }
    return n;
}

// Rebuild the message buffer as: everything up to the leave row, then our rows,
// then the leave row. Returns the option index our first row lands on, or -1 if
// the buffer did not look like a menu we understand.
inline int Compose(const char* snapshot)
{
    size_t leaveAt = 0;
    const int gameRows = CountOptionLines(snapshot, &leaveAt);

    // A menu with no selectable rows is not a menu; leave it entirely alone.
    if (gameRows <= 0) return -1;

    // Work out the finished length BEFORE writing anything. Truncating a menu
    // would silently drop the leave row and strand the player in the
    // settlement, so this refuses instead.
    size_t need = strlen(snapshot) + 1;
    for (int i = 0; i < g_rowCount; ++i) {
        if (!g_rows[i].enabled) continue;
        need += strlen(g_rows[i].label) + 1;   // + '\n'
    }
    if (need > kMsgMax || leaveAt >= kMsgMax) {
        Log("townmenu: composed menu would be %u bytes -- standing aside",
            (unsigned)need);
        return -1;
    }

    char buf[kMsgMax];
    memcpy(buf, snapshot, leaveAt);            // everything before the leave row
    buf[leaveAt] = 0;

    for (int i = 0; i < g_rowCount; ++i) {     // ours
        if (!g_rows[i].enabled) continue;
        strncat_s(buf, sizeof(buf), g_rows[i].label, _TRUNCATE);
        strncat_s(buf, sizeof(buf), "\n", _TRUNCATE);
    }

    strncat_s(buf, sizeof(buf), snapshot + leaveAt, _TRUNCATE);   // leave, last

    // Straight into the game's buffer. No AddText call: the rows are already
    // formatted, and AddText is varargs -- handing it player-authored text as a
    // format string is exactly how a '%s' in someone's JSON reads the stack.
    strncpy_s((char*)game::addr::MessageText, kMsgMax, buf, _TRUNCATE);

    return gameRows - 1;   // the slot the leave row used to occupy
}

// ------------------------------------------------------------------- the call
// ShowMessage's convention is the game's own: ecx = background, edx = flags,
// eax = form, result in eax. No stack arguments, so the shim is small.
__declspec(naked) inline int CallOriginal(int /*ecx*/, int /*edx*/, int /*eax*/)
{
    __asm {
        mov  ecx, dword ptr [esp + 4]
        mov  edx, dword ptr [esp + 8]
        mov  eax, dword ptr [esp + 12]
        call dword ptr [g_orig]
        ret
    }
}

// ------------------------------------------------------------- our own menus
// A PEMF menu is drawn with the engine's own card renderer, against the port's
// backdrop, and returns the option the player picked. None of the town menu's
// index arithmetic applies here: every row is ours, the game never sees the
// result, and there is no leave row to work around. This is a plain modal.
//
// The one thing it must not do is run forever. Authored data can contain a
// cycle -- menu A offering menu B offering menu A -- and a player can walk it
// as long as they like, which is fine; what must not happen is unbounded
// RECURSION, so depth is capped and a menu too deep says so rather than
// growing the stack until the game dies.
inline void RunMenu(int menuIndex, int depth)
{
    const content::MenuDef* m = content::GetMenu(menuIndex);
    if (!m) return;

    if (depth >= content::kMaxMenuDepth) {
        Log("townmenu: menu '%s' is deeper than %d -- stopping here",
            m->id.c_str(), content::kMaxMenuDepth);
        return;
    }

    for (int guard = 0; guard < kMaxLoops; ++guard) {
        // Rebuilt every time round: an option's text may depend on state that
        // the last choice changed.
        const char* opts[content::kMaxMenuOptions + 1] = {nullptr};
        int n = 0;
        for (const content::MenuOption& o : m->options) {
            if (n >= content::kMaxMenuOptions) break;
            opts[n++] = o.text.c_str();
        }
        // Always a way out, and always last, where the town menu puts its own.
        const int backRow = n;
        opts[n++] = "Never mind.";

        // Resolve the title's arguments fresh each turn, so a value shown in
        // the title reflects whatever the last choice just changed.
        int targs[content::kMaxArgs] = {0};
        const int targc = content::ResolveArgs(m->titleArgs, targs,
                                               content::kMaxArgs);

        const int pick = game::AskChoiceN(m->title.c_str(), opts, n,
                                          targs, targc);

        if (pick < 0 || pick >= n || pick == backRow) return;

        const content::MenuOption& o = m->options[(size_t)pick];
        Log("townmenu: menu '%s' -> '%s'", m->id.c_str(), o.text.c_str());

        if (o.menuIndex >= 0) {
            RunMenu(o.menuIndex, depth + 1);
        } else if (o.eventIndex >= 0) {
            content::Fire(o.eventIndex);
            content::ShowPendingOutcome(0);
            events::ClearFollowUp();
            return;                       // an event ends the walk
        } else if (!o.outcome.empty()) {
            int oargs[content::kMaxArgs] = {0};
            const int oargc = content::ResolveArgs(o.outcomeArgs, oargs,
                                                   content::kMaxArgs);
            game::ShowModalTextN(o.outcome.c_str(), oargs, oargc);
            return;                       // so does a closing card
        } else {
            // Authored to go somewhere that does not exist. Reported at load;
            // say so again here rather than appear to ignore the click.
            Log("townmenu: menu '%s' option '%s' has no destination",
                m->id.c_str(), o.text.c_str());
            return;
        }
        // A submenu returned: fall round and show this one again.
    }
    Log("townmenu: menu '%s' -- %d turns without leaving, closing it",
        m->id.c_str(), kMaxLoops);
}

// ------------------------------------------------------------------ the logic
// Runs with the menu composed and about to be shown. Returns the index the
// game should act on -- always one the game itself produced.
inline int Present(int bg, int flags, int form)
{
    InterlockedIncrement(&g_presents);

    char snapshot[kMsgMax];
    strncpy_s(snapshot, sizeof(snapshot),
              (const char*)game::addr::MessageText, _TRUNCATE);

    // Which settlement is this? In town the ship is at the port, so the
    // nearest-city lookup is the port -- the same resolution {port} already
    // uses in authored text, and it named Nevis correctly in playtest.
    const int city   = game::NearestCity(content::kCityNameScanRadius);
    const int nation = city >= 0 ? game::CityNation(city) : -1;
    ApplyContext(city, nation);

    if (EnabledCount() <= 0) {
        Log("townmenu: port %d -- no PEMF rows offered here", city);
        return CallOriginal(bg, flags, form);
    }

    // Diagnostics for an unexplained R6025 ("pure virtual function call") seen
    // minutes after menu use. Nothing here is known to be the cause -- the
    // point is that the next occurrence should say what shape of menu we were
    // handed and how many times we re-presented it, because neither is known
    // today. Cheap: a town menu happens a few times a minute, not per frame.
    {
        size_t probe = 0;
        const int rows = CountOptionLines(snapshot, &probe);
        // `bg` is the city the GAME is drawing this menu for; `city` is the
        // one our own nearest-port lookup found. They should agree, and saying
        // so in the log makes a disagreement obvious rather than mysterious.
        Log("townmenu: menu #%ld -- port %d (nation %d), backdrop city %d%s, "
            "%d game row(s), %u bytes, form %d -- %d PEMF row(s) here",
            g_presents, city, nation, bg,
            (bg == city) ? "" : " ** DISAGREES **",
            rows, (unsigned)strlen(snapshot), form, EnabledCount());
    }

    const int ourFirst = Compose(snapshot);
    if (ourFirst < 0) {
        // Not a shape we recognise. Put back exactly what the game composed and
        // stand aside -- an unfamiliar menu is not an invitation to guess.
        strncpy_s((char*)game::addr::MessageText, kMsgMax, snapshot, _TRUNCATE);
        return CallOriginal(bg, flags, form);
    }

    const int ourCount = EnabledCount();

    // The loop exists because there is no "do nothing" index to return. It is
    // bounded only to make a bug finite: a row whose action somehow never lets
    // the menu close would otherwise hang the game with no log to show for it.
    for (int guard = 0; guard < kMaxLoops; ++guard) {
        const int pick = CallOriginal(bg, flags, form);

        // Below our block, or above it: the game's own row, untouched. The
        // remap that follows is the game's business and still works, because
        // this is exactly the number it would have seen without us.
        if (pick < ourFirst || pick >= ourFirst + ourCount) {
            if (guard > 0)
                Log("townmenu: handing back %d after %d re-present(s)",
                    pick, guard);
            return pick;
        }

        // One of ours. Find which, counting only enabled rows.
        int want = pick - ourFirst;
        Row* row = nullptr;
        for (int i = 0; i < g_rowCount; ++i) {
            if (!g_rows[i].enabled) continue;
            if (want-- == 0) { row = &g_rows[i]; break; }
        }
        if (!row) return pick;    // cannot happen; do not invent an index

        InterlockedIncrement(&g_picks);
        Log("townmenu: picked '%s' (index %d)", row->label, pick);

        if (row->fn) {
            // A native row gets the same treatment as an authored one. This
            // branch used to skip it, so anything driven by code drew against
            // the sea while the JSON rows beside it drew against the port --
            // which is exactly how it looked in game.
            events::EnterDirect();
            game::g_portCardCity  = bg;
            game::g_portCardFlags = 0;

            row->fn(row->arg);

            game::g_portCardCity  = -1;
            game::g_portCardFlags = 0;
            events::LeaveDirect();
        } else if (row->menuIndex >= 0) {
            // A menu row that opens a PEMF menu. Same presentation rules as a
            // card: in place, against this port, and back to the town menu
            // afterwards.
            events::EnterDirect();
            game::g_portCardCity  = bg;
            game::g_portCardFlags = 0;

            RunMenu(row->menuIndex, 0);

            game::g_portCardCity  = -1;
            game::g_portCardFlags = 0;
            events::LeaveDirect();
        } else if (row->eventIndex >= 0) {
            // PRESENTED HERE, NOT POSTED. The first build queued it, and that
            // was the wrong call twice over. The card waited for the overworld,
            // so from inside the town the row looked like it had done nothing
            // -- players click it again. And when the card did arrive it was
            // over the sea rather than the port the row belonged to.
            //
            // The game's own menu options present modal dialogs from exactly
            // this point. Ours doing the same is doing what the engine does,
            // where the engine does it, and the town is behind the card because
            // that is genuinely what is on screen.
            events::EnterDirect();

            // Draw the card against THIS port. `bg` is the city index the town
            // menu itself was given, so the backdrop is derived from the
            // settlement's own record -- the same one behind the menu the
            // player is standing in. Without this the card composites over the
            // 3D scene, which in port is the overworld coastline.
            //
            // Flags are ZERO, not the menu's. Read off the game's own in-town
            // narrative card at 0x00411C91, which is the exact thing we are
            // imitating:
            //     MOV ECX,[ESP+0x458]   city index
            //     OR  EAX,-1            form -1, message box
            //     XOR EDX,EDX           flags zero
            // Passing the menu's flags through was a guess and would have been
            // a different call from the one the engine makes here.
            game::g_portCardCity  = bg;
            game::g_portCardFlags = 0;

            content::Fire(row->eventIndex);

            // The outcome immediately after, rather than through the queue.
            // The usual reason for deferring it -- two dialogs in one frame
            // leaves the second compositing over a stale backbuffer -- does not
            // apply here: the card above ran the game's own pump until the
            // player dismissed it, so frames were drawn in between. Deferring
            // it is what put the half-drawn "Fifty pieces lighter" card over
            // open water in the second playtest.
            content::ShowPendingOutcome(0);
            events::ClearFollowUp();

            // Back to the ordinary dialog path. Leaving this set would put a
            // port backdrop behind cards fired at sea.
            game::g_portCardCity  = -1;
            game::g_portCardFlags = 0;

            events::LeaveDirect();
        }

        // Show the menu again. ShowMessage consumed the buffer, so it has to be
        // rebuilt from the snapshot every time round -- Compose() writes it.
        //
        // If it declines this time, put the game's own menu back before handing
        // over: Compose() refuses without writing, and the buffer at this point
        // has already been eaten by the ShowMessage above. Skipping the restore
        // would present an empty menu.
        if (Compose(snapshot) < 0) {
            strncpy_s((char*)game::addr::MessageText, kMsgMax, snapshot,
                      _TRUNCATE);
            return CallOriginal(bg, flags, form);
        }
    }

    Log("!! townmenu: %d loops without leaving the menu -- standing aside",
        kMaxLoops);
    strncpy_s((char*)game::addr::MessageText, kMsgMax, snapshot, _TRUNCATE);
    return CallOriginal(bg, flags, form);
}

// A fault anywhere in the above must not take the town menu with it. The game
// still needs an answer, so we give it the one it would have got on its own.
inline int PresentGuarded(int bg, int flags, int form)
{
    if (g_faulted) return CallOriginal(bg, flags, form);

    // Hold the event queue for as long as the menu is up. The menu runs the
    // game's own nested pump, so the safe point keeps being reached while the
    // world behind it is not being drawn -- an event fired from in there puts
    // its card over an empty background. Measured, not theorised: the first
    // build presented the card 2ms after the row was picked, and it came up on
    // a flat blue screen.
    //
    // Suspending here rather than inside Present() covers the fault path too:
    // whatever happens below, the queue is released on the way out.
    events::Suspend();

    int result;
    __try {
        result = Present(bg, flags, form);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faulted = true;
        Log("!! townmenu: FAULT (0x%08X) -- PEMF rows disabled for this session",
            GetExceptionCode());
        result = CallOriginal(bg, flags, form);
    }

    // Whatever happened above, cards go back to compositing over the world.
    game::g_portCardCity  = -1;
    game::g_portCardFlags = 0;

    events::Resume();
    return result;
}

// The redirect target. Marshals the game's register convention into a normal
// call and back again.
__declspec(naked) inline int MenuShim()
{
    __asm {
        push ebp
        mov  ebp, esp
        push ebx
        push esi
        push edi

        push eax                // form
        push edx                // flags
        push ecx                // background
        call PresentGuarded
        add  esp, 12            // cdecl, we clean

        pop  edi
        pop  esi
        pop  ebx
        mov  esp, ebp
        pop  ebp
        ret
    }
}

// -------------------------------------------------------------- from content
// Resolve the authored rows once every file is loaded, so a row may name an
// event defined in a file that loads later. A row naming an event that does not
// exist is rejected with a reason rather than left to do nothing in game, which
// is the failure an author would find hardest to diagnose.
inline int LoadFromContent()
{
    Clear();
    content::ResolveMenus();

    int added = 0;
    for (const content::MenuRowDef& d : content::g_menuRows) {
        int ev = -1, mn = -1;

        if (!d.menuId.empty()) {
            mn = content::FindMenuIndex(d.menuId);
            if (mn < 0) {
                Log("townmenu: REJECTED row '%s' -- no menu with id '%s'",
                    d.label.c_str(), d.menuId.c_str());
                continue;
            }
        } else {
            ev = content::FindByIdIndex(d.eventId);
            if (ev < 0) {
                Log("townmenu: REJECTED row '%s' -- no event with id '%s'",
                    d.label.c_str(), d.eventId.c_str());
                continue;
            }
        }
        if (Add(d.label.c_str(), ev, mn, nullptr, 0, d.port, d.nation)) ++added;
    }
    if (added) Log("townmenu: %d authored row(s)", added);
    return added;
}

// ------------------------------------------------------------------- install
// ⛔ CODE WRITE -- ONLY FROM THE SAFE POINT. Patching .text while the Steam
// build's DRM wrapper is still checksumming its own image produces
// "Application corrupt." and the game never starts. That cost a release. See
// the note in core.cpp.
inline bool Install()
{
    if (g_installed) return true;

    if (!render::RedirectCall(kCallSite, (void*)&MenuShim, &g_orig)) {
        Log("townmenu: no call rel32 at 0x%08X -- not installed",
            (unsigned)kCallSite);
        return false;
    }
    if ((uintptr_t)g_orig != kCallTarget) {
        Log("townmenu: call at 0x%08X targets 0x%p, expected 0x%08X -- reverting",
            (unsigned)kCallSite, g_orig, (unsigned)kCallTarget);
        render::RedirectCall(kCallSite, g_orig, nullptr);
        g_orig = nullptr;
        return false;
    }

    g_installed = true;
    Log("townmenu: hooked 0x%08X -> ShowMessage, %d row(s) registered",
        (unsigned)kCallSite, g_rowCount);
    return true;
}

inline void Restore()
{
    if (!g_installed || !g_orig) return;
    render::RedirectCall(kCallSite, g_orig, nullptr);
    g_installed = false;
    Log("townmenu: restored (%ld menus, %ld picks)", g_presents, g_picks);
}

} // namespace townmenu
