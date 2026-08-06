// events.h - deferred event dispatch.
//
// PROBLEM: we used to present modal dialogs directly from the timeGetTime hook.
// That function is called from ~84 sites, so we had no idea where in the frame
// we were -- possibly mid-render, inside a resource lock, or halfway through an
// update. A modal dialog runs a nested message loop from there, re-entering the
// game from an unpredictable stack depth. It worked, but the failure mode is
// rare, state-dependent and effectively unattributable.
//
// APPROACH: nothing fires where it is triggered. Triggers only ever Post() to
// this queue; the queue is drained at exactly ONE known-safe point -- the top of
// the main loop, identified by the return address of its PeekMessageA call
// (0x0042E20C). At that instant the message queue is drained, no rendering is in
// progress, and no game locks are held.
#pragma once
#include <windows.h>
#include "log.h"
#include "session.h"

namespace events {

// The payload is an int rather than a pointer so that content-driven events can
// be queued BY INDEX. A content reload replaces the event vector, and a queued
// raw pointer would dangle.
using EventFn = void (*)(int arg);

constexpr int   kQueueCapacity  = 8;
constexpr int   kNameLen        = 64;
constexpr DWORD kMinGapMs       = 1000;   // no two cards back to back

// ------------------------------------------------------------- fault latch
// A fault inside an engine call means game state may be half-modified.
// Continuing to fire events into it risks a crash minutes later with no visible
// connection to the cause, so we stop for the session.
inline bool g_faulted = false;

inline void NoteFault(const char* where, DWORD code)
{
    if (g_faulted) return;
    g_faulted = true;
    Log("!! FAULT in %s (0x%08X) -- events DISABLED for this session.", where, code);
    Log("!! Game state may be inconsistent; restart before trusting it.");
}

inline bool Faulted() { return g_faulted; }

// ------------------------------------------------------------------- queue
// The name is COPIED, not referenced: a content reload frees the strings the
// event ids live in, and a queued pointer into them would dangle.
struct Entry {
    EventFn fn;
    int     arg;
    char    name[kNameLen];
};

inline Entry g_queue[kQueueCapacity];
inline int   g_count   = 0;
inline volatile bool g_inEvent = false;   // reentrancy guard
inline DWORD g_lastFired = 0;

// A follow-up runs on the NEXT frame, ahead of the queue and without the
// inter-event gap.
//
// This exists because presenting two dialogs back to back inside one frame
// leaves the second compositing over a stale backbuffer -- the world behind it
// renders half-finished. Letting the game draw one frame in between fixes it,
// and the pause is imperceptible.
inline bool  g_haveFollowUp = false;
inline Entry g_followUp{};

inline void PostFollowUp(EventFn fn, int arg, const char* name)
{
    if (g_faulted) return;
    g_followUp.fn  = fn;
    g_followUp.arg = arg;
    strncpy_s(g_followUp.name, sizeof(g_followUp.name), name ? name : "?", _TRUNCATE);
    g_haveFollowUp = true;
}

// Safe to call from anywhere, including a trigger deep in the frame.
// Never presents anything; only records intent.
inline bool Post(EventFn fn, int arg, const char* name)
{
    if (g_faulted) return false;
    if (g_count >= kQueueCapacity) {
        Log("events: queue full, dropping '%s'", name);
        return false;
    }
    Entry& e = g_queue[g_count++];
    e.fn  = fn;
    e.arg = arg;
    strncpy_s(e.name, sizeof(e.name), name ? name : "?", _TRUNCATE);
    return true;
}

inline bool Busy() { return g_inEvent; }
inline int  Pending() { return g_count; }

// ------------------------------------------------------------------ suspend
// A menu that runs the game's own nested pump reaches the safe point WITHOUT
// the world being drawn behind it -- the town menu is the case that found this.
// An event fired from in there presents its card over an empty background,
// because the scene the card composites onto was never rendered that frame.
//
// This is not the same hazard as g_inEvent. Nothing is re-entering; the queue
// is simply being drained somewhere the screen is not what the player is
// looking at. So the caller holding such a menu open suspends the queue, and
// the card comes up at the next ordinary safe point once the menu has closed --
// which is what "posted, not presented" was always supposed to mean.
inline int g_suspend = 0;

inline void Suspend()
{
    ++g_suspend;
}

inline void Resume()
{
    if (g_suspend > 0) --g_suspend;
}

inline bool Suspended() { return g_suspend > 0; }

// Drain one event. Called ONLY from the safe point.
//
// One per frame deliberately: presenting a second card while the first is still
// unwinding is how the ~610-deep recursion happened during development.
inline void Pump()
{
    if (g_faulted || g_inEvent) return;
    if (g_suspend > 0) return;                   // a menu is up; see Suspend()
    if (!session::Ready()) return;               // no career loaded

    DWORD now = GetTickCount();
    Entry e;

    if (g_haveFollowUp) {
        // Runs on the frame after whatever posted it, so the game has had a
        // chance to render in between. No inter-event gap applies.
        e = g_followUp;
        g_haveFollowUp = false;
    } else {
        if (g_count == 0) return;
        if (g_lastFired && now - g_lastFired < kMinGapMs) return;
        e = g_queue[0];
        for (int i = 1; i < g_count; ++i) g_queue[i - 1] = g_queue[i];
        --g_count;
    }

    g_inEvent = true;
    Log("events: firing '%s' (%d still queued)", e.name, g_count);
    __try {
        e.fn(e.arg);
        session::NoteEventFired(now);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        NoteFault(e.name, GetExceptionCode());
    }
    g_inEvent = false;
    g_lastFired = GetTickCount();   // measure the gap from when the card CLOSED
}

inline void Clear(const char* reason)
{
    if (g_count || g_haveFollowUp)
        Log("events: dropping %d queued%s (%s)", g_count,
            g_haveFollowUp ? " + a follow-up" : "", reason);
    g_count = 0;
    g_haveFollowUp = false;
}

} // namespace events
