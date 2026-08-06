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
    void (*fn)(int) = nullptr;
    int  arg = 0;
    bool enabled = true;
};

inline Row  g_rows[kMaxRows];
inline int  g_rowCount = 0;
inline bool g_installed = false;
inline void* g_orig = nullptr;          // the real ShowMessage

// Diagnostics -- these are the numbers to ask for when something looks wrong.
inline volatile LONG g_presents = 0;    // menus we passed through
inline volatile LONG g_picks    = 0;    // times a PEMF row was chosen
inline bool g_faulted = false;          // latched off after a fault

inline void Clear() { g_rowCount = 0; }

inline bool Add(const char* label, int eventIndex, void (*fn)(int), int arg)
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
    r.fn = fn;
    r.arg = arg;
    r.enabled = true;
    return true;
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

// ------------------------------------------------------------------ the logic
// Runs with the menu composed and about to be shown. Returns the index the
// game should act on -- always one the game itself produced.
inline int Present(int bg, int flags, int form)
{
    InterlockedIncrement(&g_presents);

    char snapshot[kMsgMax];
    strncpy_s(snapshot, sizeof(snapshot),
              (const char*)game::addr::MessageText, _TRUNCATE);

    if (EnabledCount() <= 0)
        return CallOriginal(bg, flags, form);

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
        if (pick < ourFirst || pick >= ourFirst + ourCount)
            return pick;

        // One of ours. Find which, counting only enabled rows.
        int want = pick - ourFirst;
        Row* row = nullptr;
        for (int i = 0; i < g_rowCount; ++i) {
            if (!g_rows[i].enabled) continue;
            if (want-- == 0) { row = &g_rows[i]; break; }
        }
        if (!row) return pick;    // cannot happen; do not invent an index

        InterlockedIncrement(&g_picks);
        Log("townmenu: picked '%s'", row->label);

        if (row->fn) {
            row->fn(row->arg);
        } else if (row->eventIndex >= 0) {
            // Posted, not presented. A menu row is a trigger like any other,
            // and the invariant that nothing presents from where it fires is
            // what keeps this framework stable -- see events.h. The card comes
            // up at the safe point once the player is out of the menu.
            const content::Event* ev = content::Get(row->eventIndex);
            events::Post([](int idx) { content::Fire(idx); },
                         row->eventIndex, ev ? ev->id.c_str() : "menu row");
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
    int added = 0;
    for (const content::MenuRowDef& d : content::g_menuRows) {
        const int idx = content::FindByIdIndex(d.eventId);
        if (idx < 0) {
            Log("townmenu: REJECTED row '%s' -- no event with id '%s'",
                d.label.c_str(), d.eventId.c_str());
            continue;
        }
        if (Add(d.label.c_str(), idx, nullptr, 0)) ++added;
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
