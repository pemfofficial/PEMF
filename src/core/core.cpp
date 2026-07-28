// core.cpp - pemf_core.dll  (Pirates! Expanded Modding Framework)
//
// Layering:
//   game.h      raw engine addresses and calling shims
//   state.h     validated access to live game state (all mutation is policed)
//   session.h   career lifecycle, save/load detection, our own persistence
//   events.h    deferred dispatch -- events fire ONLY at a known-safe point
//   core.cpp    hooks, triggers, and event content
//
// Four IAT hooks, no inline patching anywhere:
//   WINMM!timeGetTime    cheap per-frame tick; triggers POST to the queue
//   USER32!PeekMessageA  the safe point; the queue is DRAINED here
//   KERNEL32!CreateFileA save/load detection for our sidecar state
//   KERNEL32!CreateFileW the same, for the wide path

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <intrin.h>
#include <shlwapi.h>
#include <shlobj.h>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

#include "log.h"
#include "game.h"
#include "state.h"
#include "nations.h"
#include "session.h"
#include "events.h"
#include "content.h"
#include "triggers.h"
#include "render.h"
#include "d3d9hook.h"

#pragma intrinsic(_ReturnAddress)

// ------------------------------------------------------------------ logging
static FILE*            g_log = nullptr;
static CRITICAL_SECTION g_logLock;

void Log(const char* fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

// ----------------------------------------------------------------- IAT hook
// Memory-safety helpers (defined with the diagnostics below). Reading a
// possibly-encrypted / mid-unpack import table must never fault -- a fault, even
// caught, can disturb a DRM packer's own exception-based unpacking.
static bool PageReadable(const void* p, size_t n);
static bool SafeStr(const char* s, char* out, size_t outsz);

static bool HookIAT(const char* dllName, const char* funcName,
                    void* replacement, void** original)
{
  // SEH guard as a backstop; the PageReadable/SafeStr checks below are what
  // actually keep us from faulting on a packed/unrecognised host.
  __try {
    HMODULE base = GetModuleHandleA(NULL);
    if (!PageReadable(base, sizeof(IMAGE_DOS_HEADER))) return false;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt  = (IMAGE_NT_HEADERS*)((BYTE*)base + dos->e_lfanew);
    if (!PageReadable(nt, sizeof(IMAGE_NT_HEADERS))) return false;

    DWORD impRva = nt->OptionalHeader
                     .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!impRva) return false;

    auto* desc = (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)base + impRva);
    for (; PageReadable(desc, sizeof(IMAGE_IMPORT_DESCRIPTOR)) && desc->Name; ++desc) {
        char name[64];
        if (!SafeStr((const char*)((BYTE*)base + desc->Name), name, sizeof(name))) continue;
        if (_stricmp(name, dllName) != 0) continue;

        auto* oft = (IMAGE_THUNK_DATA*)((BYTE*)base + desc->OriginalFirstThunk);
        auto* ft  = (IMAGE_THUNK_DATA*)((BYTE*)base + desc->FirstThunk);
        for (; PageReadable(oft, sizeof(IMAGE_THUNK_DATA)) && oft->u1.AddressOfData;
               ++oft, ++ft) {
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            auto* ibn = (IMAGE_IMPORT_BY_NAME*)((BYTE*)base + oft->u1.AddressOfData);
            char fn[96];
            if (!SafeStr((const char*)((BYTE*)ibn + 2), fn, sizeof(fn))) continue;
            if (strcmp(fn, funcName) != 0) continue;

            DWORD old = 0;
            if (!VirtualProtect(&ft->u1.Function, sizeof(void*),
                                PAGE_READWRITE, &old))
                return false;
            *original = (void*)ft->u1.Function;
            ft->u1.Function = (uintptr_t)replacement;
            VirtualProtect(&ft->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// True if p lies within the loaded image of dllName -- i.e. the slot holds a
// real function pointer into that module (the loader/unpacker has populated it).
static bool PointsIntoModule(const void* p, const char* dllName)
{
    HMODULE m = GetModuleHandleA(dllName);
    if (!m || !p || !PageReadable(m, sizeof(IMAGE_DOS_HEADER))) return false;
    auto* dos = (IMAGE_DOS_HEADER*)m;
    auto* nt  = (IMAGE_NT_HEADERS*)((BYTE*)m + dos->e_lfanew);
    if (!PageReadable(nt, sizeof(IMAGE_NT_HEADERS))) return false;
    uintptr_t base = (uintptr_t)m;
    return (uintptr_t)p >= base && (uintptr_t)p < base + nt->OptionalHeader.SizeOfImage;
}

// Hook an IAT slot by its ABSOLUTE address. Needed on the DRM-packed Steam build
// whose import name tables are destroyed (so HookIAT-by-name fails) but whose
// slots are still populated at their known addresses. Only patches once the slot
// holds a real pointer into the expected module, so calling it early -- before
// the unpacker has filled the slot -- is a harmless no-op we can retry.
static bool HookSlotByAddr(uintptr_t slotVA, const char* expectedDll,
                           void* replacement, void** original)
{
    void** slot = (void**)slotVA;
    if (!PageReadable(slot, sizeof(void*))) return false;
    void* cur = *slot;
    if (cur == replacement) return true;                    // already ours
    if (!PointsIntoModule(cur, expectedDll)) return false;  // not populated yet
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *original = cur;
    *slot = replacement;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// Restore a slot we hooked, by absolute address. Same physical slot HookIAT
// patches, so this restores hooks installed either way. Never stomps a slot that
// no longer holds our replacement.
static void UnhookSlot(uintptr_t slotVA, void* replacement, void* original)
{
    if (!original) return;
    void** slot = (void**)slotVA;
    if (!PageReadable(slot, sizeof(void*)) || *slot != replacement) return;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return;
    *slot = original;
    VirtualProtect(slot, sizeof(void*), old, &old);
}

// --------------------------------------------------------- build diagnostics
// Dumps the host image's layout and import table. On a DRM-packed build (e.g.
// the Steam executable) the on-disk imports are a tiny unpacker stub; the real
// game imports are rebuilt in memory only after the unpacker runs. Logging this
// at load time AND again a few seconds later shows whether -- and when -- the
// real import table appears, which tells us if the injection vector survives the
// packer and when our hooks could take. Harmless on a plain build.
// True only if [p, p+n) is entirely committed and readable -- so we can inspect a
// possibly-encrypted / mid-unpack image (a DRM-packed host) WITHOUT ever faulting.
// A deliberate fault, even one caught by SEH, can interfere with a packer's own
// exception-based unpacking (this corrupted the Steam build's character-creation
// screen), so we avoid faulting entirely and check readability up front.
static bool PageReadable(const void* p, size_t n)
{
    if (!p || n == 0) return false;
    const BYTE* a   = (const BYTE*)p;
    const BYTE* end = a + n;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(a, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT)                          return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))       return false;
        if (mbi.Protect == PAGE_EXECUTE)                      return false;  // exec-only
        a = (const BYTE*)mbi.BaseAddress + mbi.RegionSize;
    }
    return true;
}

// Copy a C string only from confirmed-readable memory, bounded. No faults.
static bool SafeStr(const char* s, char* out, size_t outsz)
{
    if (!s || outsz == 0 || !PageReadable(s, 1)) { if (outsz) out[0] = 0; return false; }
    size_t i = 0;
    for (; i < outsz - 1; ++i) {
        if (!PageReadable(s + i, 1)) break;
        char c = s[i];
        if (!c) break;
        out[i] = c;
    }
    out[i] = 0;
    return i > 0;
}

static void DiagnoseImage(const char* when)
{
    HMODULE base = GetModuleHandleA(NULL);
    if (!base || !PageReadable(base, sizeof(IMAGE_DOS_HEADER))) {
        Log("[diag %s] module header not readable", when);
        return;
    }
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt  = (IMAGE_NT_HEADERS*)((BYTE*)base + dos->e_lfanew);
    if (!PageReadable(nt, sizeof(IMAGE_NT_HEADERS))) {
        Log("[diag %s] NT header not readable", when);
        return;
    }
    Log("[diag %s] base=%p entry=0x%08X sizeOfImage=0x%X sections=%u",
        when, base, nt->OptionalHeader.AddressOfEntryPoint,
        nt->OptionalHeader.SizeOfImage, nt->FileHeader.NumberOfSections);

    DWORD impRva = nt->OptionalHeader
        .DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!impRva) { Log("[diag %s] no import directory", when); return; }

    auto* desc = (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)base + impRva);
    if (!PageReadable(desc, sizeof(IMAGE_IMPORT_DESCRIPTOR))) {
        Log("[diag %s] import table NOT READABLE (packed / mid-unpack)", when);
        return;
    }

    int dllCount = 0;
    bool haveTimeGetTime = false, havePeekMessageA = false, haveCreateFileA = false;
    for (; dllCount < 64; ++desc, ++dllCount) {
        if (!PageReadable(desc, sizeof(IMAGE_IMPORT_DESCRIPTOR)) || !desc->Name) break;
        char dll[64];
        if (!SafeStr((const char*)((BYTE*)base + desc->Name), dll, sizeof(dll))) break;

        DWORD thunkRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                  : desc->FirstThunk;
        auto* oft = (IMAGE_THUNK_DATA*)((BYTE*)base + thunkRva);
        int fnCount = 0;
        char first[96]; first[0] = 0;
        for (; fnCount < 4096; ++oft, ++fnCount) {
            if (!PageReadable(oft, sizeof(IMAGE_THUNK_DATA)) || !oft->u1.AddressOfData) break;
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            auto* ibn = (IMAGE_IMPORT_BY_NAME*)((BYTE*)base + oft->u1.AddressOfData);
            char fn[96];
            if (!SafeStr((const char*)((BYTE*)ibn + 2), fn, sizeof(fn))) break;
            if (!first[0]) strncpy_s(first, fn, _TRUNCATE);
            if (!_stricmp(dll, "WINMM.dll")    && !strcmp(fn, "timeGetTime"))  haveTimeGetTime = true;
            if (!_stricmp(dll, "USER32.dll")   && !strcmp(fn, "PeekMessageA")) havePeekMessageA = true;
            if (!_stricmp(dll, "KERNEL32.dll") && !strcmp(fn, "CreateFileA"))  haveCreateFileA = true;
        }
        Log("[diag %s]   %-16s %4d fn  e.g. %s", when, dll, fnCount, first);
    }
    Log("[diag %s] DLLs=%d | hook targets: timeGetTime=%d PeekMessageA=%d CreateFileA=%d",
        when, dllCount, haveTimeGetTime, havePeekMessageA, haveCreateFileA);
}

// Re-runs the diagnostic after the process has had time to unpack. Comparing the
// two dumps reveals a DRM stub IAT turning into the real one.
static DWORD WINAPI DelayedDiag(LPVOID)
{
    Sleep(10000);
    Log("--- delayed diagnostic (10s after load) ---");
    DiagnoseImage("t+10s");
    return 0;
}

// ------------------------------------------------------------------- state
static constexpr bool kDebugHotkeys = true;

// Return address of the main loop's PeekMessageA call (0x0042E206 + 6). Only
// that call site is the top of the frame; the modal dialogs run their own pumps
// from elsewhere and must not be mistaken for it.
static constexpr uintptr_t kMainLoopPeekRet = game::addr::MainLoopPeekRet;

static bool  g_targetOK          = false;
static bool  g_prevKeyDown       = false;
static DWORD g_gameThreadId      = 0;
static bool  g_loggedOtherThread = false;
static DWORD g_tickCount         = 0;
static DWORD g_safePointHits     = 0;
static bool  g_safePointFound    = false;
static DWORD g_peekCalls         = 0;
static bool  g_fallbackWarned    = false;

// ------------------------------------------------------------ event content
// Events live in PEMF\events\*.json. This is the only bridge between the
// queue and the content library: the queue carries an INDEX, never a pointer,
// so reloading content can never leave a queued entry dangling.
static void RunContentEvent(int index)
{
    content::Fire(index);
}

// Defined with the false-colours probe below; called from the safe point.
static void WatchTheWater();
static void NoteColoursFlown(const char* flagName);
extern char g_trueFlagName[128];
extern bool g_haveTrueFlag;
static void ApplyCareerFlag();
// Latches cleared on a career change: a disguise, and our memory of the
// captain's honest colours, both belong to the career they were formed in.
extern bool g_careerFlagApplied;
extern bool g_haveTrueFlag;

// ------------------------------------------------------- the safe point
// Everything that touches the game or presents UI happens here, and nowhere
// else. Called from the top of the main loop.
static void RunSafePoint()
{
    ++g_safePointHits;
    if (!g_safePointFound) {
        g_safePointFound = true;
        Log("safe point reached (main loop PeekMessageA) -- deferred dispatch live");
    }

    // A career starting, ending, or being loaded invalidates all trigger
    // progress -- accumulated sailing time and armed/disarmed state belong to
    // the career they were earned in.
    if (session::Tick()) {
        triggers::Reset("career context changed");
        // Anything still on screen belongs to the career that just ended. A
        // lookout's call from the last captain's voyage has no business
        // hanging over this one's ship.
        content::ClearNotices();
        // Suspicion belongs to the career that earned it, hunters included.
        suspicion::ResetAll();
        // A disguise belongs to the career it was put on in. The new career's
        // own value is whatever the game loaded, so all we must do is stop
        // believing we have something of theirs to give back.
        state::ForgetNationalityOverride();
        g_careerFlagApplied = false;
        g_haveTrueFlag      = false;
    }

    triggers::Tick();       // may Post(); never presents anything

    // World-anchored drawing is only meaningful in the sailing view, and only
    // safe once the engine's label manager exists. The render hook cannot work
    // that out for itself without sampling the world, so it is decided here and
    // published for BeginScene to read.
    // Not just "in a career": world text drawn from a menu is projected onto
    // whatever that screen happens to be showing, which put a lookout's call
    // across the Load/Save map. The ship being under way is the same
    // playtest-validated test the triggers use, and it is false in every menu
    // because the position is frozen there.
    // Published WITH the screen signature it was decided from, because this
    // runs once per main-loop iteration and the render hook runs thousands of
    // times a second. Anything that changes the screen between two safe points
    // -- entering a town, opening a menu, dropping into a battle -- would
    // otherwise leave the flag saying "overworld" until the next tick caught
    // up. See content::WorldStillOnScreen().
    int screenId = 0, screenDepth = 0;
    __try {
        screenId    = *(const int*)game::addr::ScreenId;
        screenDepth = *(const int*)game::addr::ScreenDepth;
    } __except (EXCEPTION_EXECUTE_HANDLER) { screenId = screenDepth = 0; }

    content::PublishWorldLive(state::InGame() && triggers::WorldOnScreen() &&
                              *(void**)game::addr::WorldLabelManager != nullptr,
                              screenId, screenDepth);

    // A notice should not spend its life expiring behind a menu, so its clock
    // only runs while the overworld is actually on screen.
    content::HoldNoticeClock(content::g_worldLive);

    // ------------------------------------------------------- suspicion
    // Evaluated here, never in the render hook: it reads the ship array and the
    // city table, and it can decide to build a ship.
    {
        static DWORD lastSusAt = 0;
        const DWORD now = GetTickCount();
        const DWORD dt  = lastSusAt ? (now - lastSusAt) : 0;
        lastSusAt = now;

        suspicion::Tick(dt);
        suspicion::RefreshPanel();

        // Suspicion composes no text of its own -- it hands us a line and we
        // put it on screen through the channel it belongs in.
        // Being unmasked ends the ruse. Suspicion asks for the true colours
        // back rather than reaching into the flag layer itself.
        if (suspicion::g_pendingStrikeColours) {
            suspicion::g_pendingStrikeColours = false;
            if (g_haveTrueFlag && game::SetPlayerFlagByName(g_trueFlagName)) {
                session::RecordFlag(g_trueFlagName);
                NoteColoursFlown(g_trueFlagName);
                Log("suspicion: true colours run back up -- '%s'", g_trueFlagName);
            } else {
                Log("suspicion: unmasked, but no true colours were recorded to "
                    "go back to -- the flag stays as it is");
            }
        }

        if (suspicion::g_pendingNotice) {
            content::PostDebugNotice(suspicion::g_pendingNotice, 7, false,
                                     suspicion::g_pendingIsBeat
                                         ? content::kChannelNarrative
                                         : content::kChannelStatus);
            suspicion::g_pendingNotice = nullptr;
        }
    }

    // Measures whether vessels are steering at us, so the false-colours
    // experiment is answered by numbers rather than by an impression.
    WatchTheWater();

    // A career's colours belong to the career, so they are restored here rather
    // than left to Config.ini -- which is global and would otherwise carry one
    // captain's disguise into the next one's voyage.
    ApplyCareerFlag();


    // The game's Direct3D device only exists once the game has created it, and
    // the engine has been seen rebuilding its vtable mid-session -- so the hook
    // installs and re-verifies itself from here. A few guarded reads per frame.
    if (g_targetOK) d3d9hook::TryInstall();

    // One main-loop iteration is one displayed frame. Closing off the render
    // pass count here is what lets the frame hook tell the world pass from the
    // passes that follow it.
    d3d9hook::MarkFrameBoundary();

    // Reporting for the render hooks happens here, not inside them: this point
    // is known to be safe to log from.
    render::ReportFromSafePoint();
    d3d9hook::ReportFromSafePoint();
    content::ReportDrawFromSafePoint();

    // Presenting only moves to the render phase once stage 3 is reached. Until
    // then it happens here, which works but can leave a half-drawn background
    // behind a dialog.
    if (!render::WantsPresent()) events::Pump();

    // Periodic world sample: confirms what the triggers are seeing, and carries
    // the screen-state candidates we still need to identify.
    static DWORD lastSample = 0;
    DWORD now = GetTickCount();
    if (state::InGame() && now - lastSample >= 5000) {
        lastSample = now;
        triggers::LogSample();
    }
}

// ------------------------------------------------- the render phase
// Called immediately after the game's sailing render function returns, so the
// world is fully drawn behind us. This is where anything visible happens:
//
//   * notices are re-drawn (HUD text has no timer of its own)
//   * queued events are presented, over a complete frame
//
// Declared in render.h.
// STAGE 1 does nothing but count. No logging, no allocation, no engine calls --
// if the game survives with this, the stub and the premise are sound and the
// fault was in the work, not the mechanism. Reporting happens from the safe
// point instead (render::ReportFromSafePoint).
// Called from IDirect3DDevice9::EndScene, with a complete scene behind us and
// nothing presented yet. STAGE 1 does nothing but let the counter in the hook
// record that we got here -- no engine calls, no allocation, no logging.
extern "C" void __cdecl PemfOnEndScene(void* /*device*/)
{
    if (!d3d9hook::WantsNotices()) return;
    if (events::Faulted() || !g_targetOK) return;

    content::DrawNotices();                          // stage 2

    if (d3d9hook::WantsPresent()) events::Pump();    // stage 3
}

// Called from IDirect3DDevice9::BeginScene, with the frame open and empty.
// Anchored notices belong here and nowhere else: the game's world-text call
// builds scene-graph nodes, and the render walk that draws them has not run
// yet. Nothing else may go here -- a 2D blit issued now would be painted over.
extern "C" void __cdecl PemfOnBeginScene(void* /*device*/)
{
    if (!d3d9hook::WantsNotices()) return;
    if (events::Faulted() || !g_targetOK) return;

    content::DrawWorldNotices();                     // stage 2
}

extern "C" void __cdecl PemfAfterSailingRender(void)
{
    InterlockedIncrement(&g_pemfRenderFrames);

    if (!render::WantsNotices()) return;
    if (events::Faulted() || !g_targetOK) return;

    content::DrawNotices();                    // stage 2

    if (render::WantsPresent()) events::Pump();  // stage 3
}

// -------------------------------------------------------------- the hooks
typedef DWORD (WINAPI *timeGetTime_t)(void);
typedef BOOL  (WINAPI *PeekMessageA_t)(LPMSG, HWND, UINT, UINT, UINT);
// Defined with the file hooks below; used by the hotkey poll above them.
static void ProbeItemNames(int first, int last);
extern bool g_fileProbe;

// The false-colours probe writes game state, so it is armed by a marker file
// rather than being a key anyone can hit. See the hotkey handler.
bool g_falseColours = false;

// --------------------------------------------- which colours are we wearing
// Suspicion needs to know whose flag we are flying, and the flag layer works in
// NAMES. The nation flags are the five the game ships; anything else is a
// personal device, which is nobody's national colours and so is not a lie.
//
// Matched on the stem rather than the whole filename so a player's
// "flag_spa_weathered.dds" still reads as Spanish -- if it looks like their
// ensign at a cable's length, it is their ensign.
static int NationFromFlagName(const char* name)
{
    if (!name || !*name) return -1;
    struct Stem { const char* stem; int nation; };
    static const Stem kStems[] = {
        { "flag_spa", game::addr::kSpanish },
        { "flag_eng", game::addr::kEnglish },
        { "flag_fre", game::addr::kFrench  },
        { "flag_dut", game::addr::kDutch   },
    };
    for (const Stem& s : kStems) {
        if (_strnicmp(name, s.stem, strlen(s.stem)) == 0) return s.nation;
    }
    return -1;      // our own device, or the black flag: honest either way
}

// Called wherever the flown flag changes.
//
// Whose colours count as OURS is the nation the player serves -- read from the
// engine's own PlayerNation, not inferred from the name of whatever flag the
// career happens to have recorded. The first version compared against the true
// FLAG NAME, and since a career's own flag is usually a personal device
// ("flag_jack.dds") that resolves to no nation at all, an English captain
// flying English colours was treated as an impostor and hunted by his own
// crown. Reported from a playtest, and obvious in hindsight: a flag name is not
// an allegiance.
static void NoteColoursFlown(const char* flagName)
{
    const int worn = NationFromFlagName(flagName);
    const int own  = nations::HomeNation();
    const int wearing = (worn >= 0 && worn != own) ? worn : -1;

    if (wearing != suspicion::g_wearing) {
        Log("suspicion: colours now %s (flag '%s'; we serve %s)",
            wearing >= 0 ? game::NationName(wearing) : "honest",
            flagName ? flagName : "?",
            own >= 0 ? game::NationName(own) : "no crown");
    }
    suspicion::SetWearing(wearing);
}

// The shipyard experiment gets its own marker. It does not write game state so
// much as CREATE it, and unlike every other probe here that cannot be undone by
// pressing the key a second time.
bool g_shipyard = false;

// The last slot we built, so Ctrl+Shift+P can diff it against a ship the game
// made. Declared here because the shipyard sets it well before the diff code.
static int g_lastSpawned = -1;

// ------------------------------------------------------ watching the water
// "Did they attack me?" is not a measurement. Whether a vessel is CLOSING on
// the player or drifting away is, so the probe tracks each one's distance over
// time and reports the trend. That way the question the experiment actually
// asks -- does changing our colours change how the AI behaves toward us -- is
// answered by numbers in the log rather than by an impression of the sea.
//
// Distance uses the engine's own octagonal approximation (game::CityDistance
// reproduces it for cities) so our figures agree with the game's.
// FIRST ATTEMPT WAS WRONG, and the way it was wrong is worth keeping: it
// measured the DISTANCE between us and each vessel and called a shrinking gap
// "closing". That reads the player's own sailing, not the AI's intent -- a
// session showed eight vessels "converging" in the same tick, each by an
// identical 802 units, which was simply the player moving. It also reported
// closing just as often under our true colours as under a false flag, so it
// could not have answered the question it existed for.
//
// What actually matters is whether a vessel is STEERING TOWARD US regardless of
// what we do. That is its own displacement between samples, projected onto the
// direction from it to the player: positive means it chose to come at us.
// Player movement cancels out entirely.
struct VesselTrack {
    int  lastX = 0, lastY = 0;
    int  pursuing = 0;      // consecutive samples moving toward the player
    bool have  = false;
    bool seen  = false;
};
static VesselTrack g_track[24];

static int OctagonalDistance(int dx, int dy)
{
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    const int lo = dx < dy ? dx : dy;
    const int hi = dx < dy ? dy : dx;
    return (lo + hi * 2) / 2;
}

// One line per vessel, at most every couple of seconds. Read-only.
static void WatchTheWater()
{
    if (!g_falseColours || !state::InGame()) return;

    static DWORD lastAt = 0;
    const DWORD now = GetTickCount();
    if (lastAt && (now - lastAt) < 2000) return;
    lastAt = now;

    __try {
        const int px = game::PlayerX() / 1000;
        const int py = game::PlayerY() / 1000;
        const int mine = state::Nationality();

        for (int i = 1; i < 24; ++i) {
            const int n = game::ShipNationality(i);
            const int x = *(const int*)(game::ShipRecord(i) + 0x0C) / 1000;
            const int y = *(const int*)(game::ShipRecord(i) + 0x10) / 1000;
            VesselTrack& t = g_track[i];

            if (n < 0 || n >= game::addr::kNationCount || (x == 0 && y == 0)) {
                t = VesselTrack{};
                continue;
            }

            const int d = OctagonalDistance(x - px, y - py);

            // The vessel's OWN movement since the last sample, and how much of
            // it points at us. Player motion is not in this figure at all.
            int   moved   = 0;
            long long toward = 0;
            if (t.have) {
                const int vx = x - t.lastX, vy = y - t.lastY;
                moved = OctagonalDistance(vx, vy);
                // Direction from the vessel to the player, unnormalised: the
                // sign of the dot product is all we need.
                toward = (long long)vx * (px - x) + (long long)vy * (py - y);
                if (moved > 20 && toward > 0) ++t.pursuing;
                else if (moved > 20)          t.pursuing = 0;
            }
            t.lastX = x; t.lastY = y; t.have = true;

            // Report only vessels near enough for an encounter to be possible.
            // The whole map is ~422,000 units across and a harbour is a few
            // thousand, so anything beyond this is on the far side of the sea
            // and its behaviour tells us nothing. The first session logged
            // vessels at 400,000 and drowned the signal.
            constexpr int kInterestingRange = 30000;
            if (d > kInterestingRange) { t.seen = true; continue; }

            const bool notable = (t.pursuing >= 2) || !t.seen;
            t.seen = true;
            if (notable) {
                Log("falsecolours: vessel %2d flies %-7s dist %6d  moved %5d  "
                    "%s   (we fly %s)",
                    i, game::NationName(n), d, moved,
                    t.pursuing >= 2 ? "** PURSUING **"
                                    : (moved > 20 ? "not toward us" : "idle"),
                    game::NationName(mine));
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("falsecolours: EXCEPTION 0x%08X watching the water; probe off",
            GetExceptionCode());
        g_falseColours = false;
    }
}

// --------------------------------------------------- flags, by name
// Textures are now loaded BY NAME through the engine's own loader, the way the
// config path does it (see game::SetPlayerFlagByName). That replaced an earlier
// approach which captured pointers as the player browsed the picker -- it
// worked, but it could only fly "texture #3" without knowing which flag that
// was, which is no use to an author or a player.
//
// What is still worth remembering is the name the player actually chose, so a
// disguise can always be taken off. Captured live rather than re-read from
// Config.ini, because they may have changed it this session.
char        g_trueFlagName[128] = {0};
bool        g_haveTrueFlag      = false;
static int  g_flagCursor        = 0;

// Puts the career's colours back on the mast after a load, and captures this
// captain's honest colours the first time we see them.
//
// Runs from the safe point, and only once the world exists: the flag is a
// texture on a scene node, and asking for one before the game has built any is
// a question with no good answer. Retried every frame until it takes, then
// latched -- a load can complete before the ship does.
bool g_careerFlagApplied = false;

static void ApplyCareerFlag()
{
    // session::InCareer(), NOT state::InGame(). The latter is "crew > 0", which
    // stays true on the main menu, so this ran while the player was LEAVING a
    // career and re-applied the disguise they had just abandoned.
    if (!session::InCareer()) { g_careerFlagApplied = false; return; }
    if (g_careerFlagApplied) return;

    __try {
        // Whatever Config.ini had is this captain's honest choice, unless the
        // sidecar already recorded one.
        if (!session::TrueFlagName()[0]) {
            int len = 0;
            const char* n = game::EngineString(game::addr::CustomFlagName, &len);
            char nm[128];
            if (n && len > 0 && SafeStr(n, nm, sizeof(nm)))
                session::RecordTrueFlag(nm);
        }

        // A career with no disguise recorded must still be SET, not left alone.
        // The flag texture is global -- it is the Config.ini custom flag, and
        // the game does not reset it when a career begins -- so leaving the mast
        // untouched means the previous captain's disguise simply stays on it.
        // (This was briefly "left alone" on the theory that the game picks a
        // flag from the nation chosen at character creation. It does not: what
        // looked like a correct faction flag was the global custom flag, which
        // happened to be a copy of the English one.)
        //
        // So: a recorded disguise, or failing that the player's own Config.ini
        // choice, which is what undisguised means.
        const char* want = session::FlagName();
        char fallback[128] = {0};
        const bool disguised = (want && *want);
        if (!disguised) {
            int len = 0;
            const char* n = game::EngineString(game::addr::CustomFlagName, &len);
            if (n && len > 0 && SafeStr(n, fallback, sizeof(fallback)))
                want = fallback;
        }

        if (want && *want) {
            if (game::SetPlayerFlagByName(want)) {
                Log("falsecolours: career colours set to '%s' -- %s", want,
                    disguised ? (session::Disguised() ? "a recorded disguise"
                                                      : "this career's own")
                              : "nothing recorded, so the player's own flag");
                g_careerFlagApplied = true;
                NoteColoursFlown(want);
            }
            // No latch on failure: the world may simply not be built yet, and
            // this costs one guarded call a frame until it is.
            return;
        }
        // No custom flag anywhere: the game is on its stock flag already.
        g_careerFlagApplied = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("falsecolours: EXCEPTION 0x%08X restoring career colours",
            GetExceptionCode());
        g_careerFlagApplied = true;      // never retry into a fault
    }
}

// ------------------------------------------------ our own flag enumeration
// The game finds custom flags with a directory scan, but only runs it when the
// "Change Sails and Flags" screen is first opened. Depending on that would mean
// telling a player to visit a menu before the framework works, every session --
// which is not a feature, it is a chore we would be inventing.
//
// So PEMF scans for itself, at startup. This is ordinary Windows file work and
// needs no engine call at all: the two folders are the same two the game looks
// in, the pattern is the same pattern, and the loader takes a bare filename --
// all measured, not assumed. The game's own list is still read when it exists,
// as a cross-check.
static char g_gameDir[MAX_PATH] = {0};
static char g_flagNames[64][128];
static int  g_flagNameCount = 0;

static void ScanFlagFolder(const char* folder)
{
    char pattern[MAX_PATH];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\flag_*.dds", folder);

    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (g_flagNameCount >= (int)(sizeof(g_flagNames) / sizeof(g_flagNames[0])))
            break;
        // The game de-duplicates across its two folders, so we do too.
        bool seen = false;
        for (int i = 0; i < g_flagNameCount && !seen; ++i)
            seen = _stricmp(g_flagNames[i], fd.cFileName) == 0;
        if (seen) continue;
        strncpy_s(g_flagNames[g_flagNameCount], sizeof(g_flagNames[0]),
                  fd.cFileName, _TRUNCATE);
        ++g_flagNameCount;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void ScanFlags()
{
    g_flagNameCount = 0;
    if (g_gameDir[0]) {
        char folder[MAX_PATH];
        _snprintf_s(folder, sizeof(folder), _TRUNCATE, "%s\\custom", g_gameDir);
        ScanFlagFolder(folder);
    }
    // The player's own folder, which the game reads in preference to nothing --
    // see re/experiments/flags.
    char docs[MAX_PATH];
    if (SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, docs) == S_OK) {
        char folder[MAX_PATH];
        _snprintf_s(folder, sizeof(folder), _TRUNCATE,
                    "%s\\My Games\\Sid Meier's Pirates!\\Custom", docs);
        ScanFlagFolder(folder);
    }
    Log("flags: %d flag(s) found by our own scan -- no need to open the "
        "picker first", g_flagNameCount);
    for (int i = 0; i < g_flagNameCount; ++i)
        Log("flags:   [%2d] %s", i, g_flagNames[i]);
}

// Our scan is authoritative because it always exists. The game's list is used
// only to confirm the two agree.
static int FlagCount() { return g_flagNameCount; }
static const char* FlagAt(int i)
{
    return (i >= 0 && i < g_flagNameCount) ? g_flagNames[i] : nullptr;
}

static void RememberTrueFlag()
{
    if (g_haveTrueFlag) return;
    __try {
        int len = 0;
        const char* n = game::EngineString(game::addr::CustomFlagName, &len);
        if (n && len > 0 && SafeStr(n, g_trueFlagName, sizeof(g_trueFlagName))) {
            g_haveTrueFlag = true;
            Log("falsecolours: true colours are '%s'", g_trueFlagName);
        } else {
            Log("falsecolours: no custom flag is selected, so there is nothing "
                "to restore to -- pick one in Options first");
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("falsecolours: EXCEPTION 0x%08X reading the chosen flag name",
            GetExceptionCode());
    }
}

// What the world looks like from where we are standing, logged around a change
// so the before and after can be compared. Reads only, and guarded: this walks
// other vessels' records, which is new ground.
static void ReportColours(const char* when)
{
    __try {
        const int mine = state::Nationality();
        const int px = game::PlayerX() / 1000, py = game::PlayerY() / 1000;

        int nameLen = 0;
        const char* chosen = game::EngineString(game::addr::CustomFlagName, &nameLen);
        char safeName[128] = "(none)";
        if (chosen && nameLen > 0) SafeStr(chosen, safeName, sizeof(safeName));

        Log("falsecolours [%s]: flag texture 0x%p, chosen name '%s' (len %d), "
            "ship-record nation %d (%s -- NOT the flag, see game.h)",
            when, game::PlayerFlagTexture(), safeName, nameLen, mine,
            game::NationName(mine));

        // The enumerated lists -- what a feature can actually refer to by name.
        const int nflags = game::FlagListCount();
        const int nsails = game::SailListCount();
        Log("falsecolours: %d flag(s) / %d sail(s) enumerated (counters say "
            "%d / %d)", nflags, nsails,
            game::CustomFlagCount(), game::CustomSailCount());
        for (int i = 0; i < nflags && i < 32; ++i) {
            char nm[128];
            const char* rawName = game::FlagName(i);
            if (rawName && SafeStr(rawName, nm, sizeof(nm)))
                Log("falsecolours:   flag[%2d] = '%s'", i, nm);
            else
                Log("falsecolours:   flag[%2d] = <unreadable>", i);
        }
        for (int i = 0; i < nsails && i < 32; ++i) {
            char nm[128];
            const char* rawName = game::SailName(i);
            if (rawName && SafeStr(rawName, nm, sizeof(nm)))
                Log("falsecolours:   sail[%2d] = '%s'", i, nm);
        }

        // Whatever else is on the water right now. The player is index 0, so
        // start at 1. A modest window: this is orientation, not a census.
        int shown = 0;
        for (int i = 1; i < 24 && shown < 8; ++i) {
            const int n = game::ShipNationality(i);
            if (n < 0 || n >= game::addr::kNationCount) continue;   // empty slot
            const int x = *(const int*)(game::ShipRecord(i) + 0x0C);
            const int y = *(const int*)(game::ShipRecord(i) + 0x10);
            if (x == 0 && y == 0) continue;
            Log("falsecolours:   vessel %2d flies %d (%s) at (%d,%d) dist %d",
                i, n, game::NationName(n), x / 1000, y / 1000,
                OctagonalDistance(x / 1000 - px, y / 1000 - py));
            ++shown;
        }
        if (shown == 0) Log("falsecolours:   no other vessels in the array");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("falsecolours: EXCEPTION 0x%08X reading the ship array",
            GetExceptionCode());
    }
}

// ----------------------------------------------------------- WASD by default
// The game already supports rebinding, and does it properly: a documented,
// user-editable KeyMap.ini in My Documents, one section per context, with a
// shortcut to it sitting in the install folder. No reverse engineering was
// needed for any of this.
//
// TWO COLLISIONS have to be resolved before WASD is even possible, which is
// presumably why the game did not ship it:
//     'a' is Attack Ship  -> moved to Space
//     'S' is Quick Save   -> moved to F5, with Quick Load to F9 to match
//
// The file is UTF-16LE with a BOM, and is written back the same way: the game
// reads what it wrote, and a plain-ASCII file is not what it wrote.
//
// INSTALLED ONCE. The original is backed up, a marker line goes in the footer,
// and PEMF never touches the file again -- so a player who rebinds something
// afterwards keeps it. Deleting the marker line asks for the layout back.
static void InstallWasdKeymap(const char* gameDir)
{
    char docs[MAX_PATH] = {0};
    if (SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, docs) != S_OK) {
        Log("keymap: could not find My Documents -- WASD not installed");
        return;
    }

    char target[MAX_PATH];
    _snprintf_s(target, sizeof(target), _TRUNCATE,
                "%s\\My Games\\Sid Meier's Pirates!\\KeyMap.ini", docs);

    // Already ours? Then the file belongs to the player now.
    {
        FILE* f = nullptr;
        if (fopen_s(&f, target, "rb") == 0 && f) {
            fseek(f, 0, SEEK_END);
            const long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0 && n < 200000) {
                char* buf = (char*)malloc((size_t)n);
                if (buf) {
                    const size_t got = fread(buf, 1, (size_t)n, f);
                    // The marker is UTF-16, so look for its letters spaced by
                    // nulls rather than decoding the whole file.
                    static const char kNeedle[] =
                        "P\0E\0M\0F\0-\0W\0A\0S\0D\0-\0I\0N\0S\0T\0A\0L\0L\0E\0D";
                    const size_t nlen = sizeof(kNeedle) - 1;
                    bool found = false;
                    for (size_t i = 0; i + nlen <= got; ++i) {
                        if (memcmp(buf + i, kNeedle, nlen) == 0) { found = true; break; }
                    }
                    free(buf);
                    if (found) {
                        fclose(f);
                        Log("keymap: WASD already installed -- the player's "
                            "KeyMap.ini is left alone");
                        return;
                    }
                }
            }
            fclose(f);
        }
    }

    char source[MAX_PATH];
    _snprintf_s(source, sizeof(source), _TRUNCATE,
                "%s\\PEMF\\KeyMap_WASD.ini", gameDir);

    FILE* in = nullptr;
    if (fopen_s(&in, source, "rb") != 0 || !in) {
        Log("keymap: no %s -- WASD not installed", source);
        return;
    }
    fseek(in, 0, SEEK_END);
    const long len = ftell(in);
    fseek(in, 0, SEEK_SET);
    char* ascii = (char*)malloc((size_t)len + 1);
    if (!ascii) { fclose(in); return; }
    const size_t read = fread(ascii, 1, (size_t)len, in);
    ascii[read] = 0;
    fclose(in);

    // Back up whatever is there, once, before replacing it.
    char backup[MAX_PATH];
    _snprintf_s(backup, sizeof(backup), _TRUNCATE, "%s.pemf-backup", target);
    if (GetFileAttributesA(backup) == INVALID_FILE_ATTRIBUTES)
        CopyFileA(target, backup, TRUE);

    FILE* out = nullptr;
    if (fopen_s(&out, target, "wb") != 0 || !out) {
        Log("keymap: could not write %s -- WASD not installed", target);
        free(ascii);
        return;
    }
    const unsigned char bom[2] = { 0xFF, 0xFE };
    fwrite(bom, 1, 2, out);
    for (size_t i = 0; i < read; ++i) {
        const unsigned char pair[2] = { (unsigned char)ascii[i], 0 };
        fwrite(pair, 1, 2, out);
    }
    fclose(out);
    free(ascii);

    Log("keymap: WASD installed -> %s (original saved as %s)", target, backup);
}

// ---------------------------------------------------- the shipyard experiment
// Ask the engine to build a ship, and find out whether it works.
//
// THE QUESTION: can PEMF call the game's own ship factory and get a real vessel
// on the water? Everything a false-colours consequence needs rests on the
// answer. If yes, being unmasked can mean a crown DISPATCHES A HUNTER, which is
// what the game itself does when it decides you are a problem. If no, the top
// of the suspicion ladder has no teeth and the design needs rethinking.
//
// It is arranged so that the log answers the question even if nothing visible
// happens on screen. Three things are recorded around the call:
//
//   1. the free-slot count before and after -- if the factory consumed a slot,
//      it did something, whatever appears on screen
//   2. the returned index, and whether it is in the range the allocator should
//      hand back (8..255)
//   3. the new record's own fields -- type, nationality, position, role, flags
//
// A returned index with an unchanged free count would mean the call reported
// success without allocating, which is a different and more interesting failure
// than a refusal.
//
// The ship is built AT A CITY, because that is the only form the factory takes:
// nationality comes from the city, and so does the starting position. The
// nearest port is used, so whatever appears should be close enough to see.
// After the first live test: which builder was called decides what the ship
// DOES, so all three are exposed and the experiment is now "which of these
// behaves like a hunter" rather than "does the call work" (it does).
enum ShipyardKind {
    kYardRole0 = 0,     // FUN_00414FC0, left exactly as built -- the control
    kYardRole4 = 1,     // FUN_00415290, then given a role AND a destination
    kYardRole3 = 2,     // FUN_004154F0, refused unless its global is sane
    kYardRoleOnly = 3   // FUN_00415290, then given a role and NOTHING ELSE
};

// The role to stamp on a newly built ship, cycled by its own key.
//
// Patching this was argued against at first, on the grounds that each builder
// sets its ship up completely and a patched role would leave the other fields
// belonging to a different kind of vessel. That argument was sound and it is
// now beside the point: BOTH builders produce a ship whose destination is the
// port it was built at, so the ship is already where it was going. Role 3 sails
// home (it is home) and role 4 sits still. Neither is misbehaving.
//
// Roles 1 and 2 cannot be produced by any callable builder -- they are written
// only inside the encounter spawner and the governor's dispatch, both far too
// entangled to call. So the only way to see what they DO is to stamp them.
static int g_testRole = 1;

// ...and the PURPOSE to stamp, which is the field that actually classifies a
// ship. 1 is pirate-hunter, straight out of the hover-label jump table. This is
// the value the whole false-colours consequence has been looking for.
static int g_testPurpose = game::kPurposePirateHunter;

// Somewhere for a ship to actually go. A destination equal to the origin is the
// whole reason nothing moved, so the experiment needs a port that is elsewhere.
// Picked from a distance band rather than "the farthest": across the map is a
// long wait before anything is visible, and next door is indistinguishable from
// staying put.
static int FindDistantCity(int notThisOne)
{
    int best = -1, bestDist = 0;
    for (int c = 0; c < game::addr::kMaxCities; ++c) {
        if (c == notThisOne) continue;
        const int d = game::CityDistance(c);
        if (d < 15000 || d > 90000) continue;
        if (d > bestDist) { bestDist = d; best = c; }
    }
    return best;
}

static void RunShipyardExperiment(ShipyardKind which)
{
    if (!g_shipyard) {
        Log("shipyard: not armed -- drop PEMF\\shipyard.on next to the exe. It "
            "creates permanent game state, so it is opt-in separately from "
            "everything else.");
        return;
    }
    if (!state::InGame()) {
        Log("shipyard: refused -- not in a career");
        return;
    }
    static const char* const kYardName[4] = {
        "role0 (FUN_00414FC0 -- ordinary traffic)",
        "role4 (FUN_00415290, then role + destination)",
        "role3 (FUN_004154F0 -- picks its own port)",
        "ROLE ONLY (FUN_00415290, role stamped, destination untouched)"
    };
    const bool callable = which == kYardRole0 ? game::SpawnShipCallable()
                        : which == kYardRole3 ? game::SpawnRole3Callable()
                                              : game::SpawnRole4Callable();
    if (!callable) {
        Log("shipyard: REFUSED -- %s does not match its expected bytes. "
            "Nothing was called.", kYardName[which]);
        content::PostDebugNotice("Shipyard refused: signature mismatch.", 6);
        return;
    }

    __try {
        // Role 3 chooses its own port, so a nearby city is only needed for the
        // other two. Still looked up either way, because the log wants to say
        // where the player was standing when the ship appeared.
        const int city = game::NearestCity(60000);
        if (city < 0 && which != kYardRole3) {
            Log("shipyard: no city within range to build at -- sail nearer a "
                "port and try again");
            content::PostDebugNotice("No port in range.", 5);
            return;
        }

        const int nation = city >= 0 ? game::CityNation(city) : -1;
        const int freeBefore = game::CountFreeShipSlots();

        Log("shipyard: building %s -- nearest city %d (%s), %d free slot(s) "
            "before", kYardName[which], city,
            nation >= 0 ? game::NationName(nation) : "n/a", freeBefore);

        // Ship type / kind 0xB, the value the governor's blockade dispatch
        // passes at 0x0040DA9A. Chosen because it is the one argument we have
        // actually SEEN the game use, rather than a number that seemed
        // reasonable.
        const int slot = which == kYardRole0 ? game::SpawnShipAtCity(city, 0x0B)
                       : which == kYardRole3 ? game::SpawnRole3Ship(0x0B)
                                             : game::SpawnRole4Ship(city, 0x0B);

        const int freeAfter = game::CountFreeShipSlots();
        Log("shipyard: factory returned %d, %d free slot(s) after (delta %d)",
            slot, freeAfter, freeBefore - freeAfter);

        if (slot == -2) {
            Log("shipyard: role3 REFUSED -- its home-city global at 0x%08X "
                "reads %d, which is not a real settlement. That is why the "
                "earlier attempt built ships at map position (1,2) and "
                "eventually crashed: the builder does not validate it.",
                (unsigned)game::Role3HomeCity, game::Role3HomeCityIndex());
            content::PostDebugNotice("Role 3 unavailable in this career.", 6);
            return;
        }
        if (slot < 0) {
            Log("shipyard: the factory refused -- array full, or it did not "
                "like the arguments");
            content::PostDebugNotice("Shipyard: factory refused.", 6);
            return;
        }
        if (slot < 8 || slot >= game::addr::kMaxShips) {
            Log("shipyard: ⚠ returned slot %d is OUTSIDE the pool the allocator "
                "should use (8..%d). Treating the reading as wrong rather than "
                "the game.", slot, game::addr::kMaxShips - 1);
            return;
        }

        const uintptr_t rec = game::ShipRecord(slot);
        Log("shipyard: slot %d AS BUILT -- type %d, nationality %d (%s), "
            "PURPOSE %d (%s), role %d, flags 0x%08X, dest city %d, home %d",
            slot, game::ShipType(slot), game::ShipNationality(slot),
            game::NationName(game::ShipNationality(slot)),
            game::ShipPurposeOf(slot),
            game::PurposeName(game::ShipPurposeOf(slot)),
            game::ShipRole(slot), game::ShipFlagBits(slot),
            game::ShipDestCity(slot), game::ShipHomeCity(slot));

        // ROLE, AND NOTHING ELSE.
        //
        // The earlier role test was confounded and the conclusion drawn from it
        // ("role does not gate movement") is not one that test could support:
        // every stamped ship was ALSO handed a fresh destination, so of course
        // it sailed. Role was never asked the question on its own.
        //
        // Here it is. The ship keeps the destination it was built with -- its
        // own home port, which it is already sitting in -- so it has nothing to
        // do unless the role itself gives it something. If a role means "hunt",
        // this is the only arrangement in which that can show.
        if (which == kYardRoleOnly) {
            // The game's own recipe, from 0x0045F060, rather than our guess:
            // build at the port, stamp pirate-hunter, and scale her to how
            // badly that crown thinks of us.
            // The whole recipe this time. The previous attempt computed the
            // strength and then never wrote it, which is the difference
            // between dispatching a hunter and labelling a ship.
            const int str = game::HunterStrengthFor(nation);
            game::SetShipPurposeRaw(slot, game::kPurposePirateHunter);
            game::SetShipRoleRaw(slot, str);          // +0x2A is STRENGTH
            game::MarkCitySentHunter(city, slot);     // the port owns her

            // ...plus the one thing the dispatch does NOT do, because in the
            // game's own flow the ship already has it: flag 0x8, without which
            // the AI branch at 0x0046A345 never looks at her at all. Applied
            // separately and logged separately, so if she moves we know which
            // half did it.
            const unsigned before = game::ShipFlagBits(slot);
            game::OrShipFlagsRaw(slot, 0x8);

            g_lastSpawned = slot;                 // for Ctrl+Shift+P to diff
            Log("shipyard: slot %d DISPATCHED -- %s pirate-hunter, strength %d "
                "(reputation %d -> 2 - rep/10, clamped 2..4)",
                slot, game::NationName(game::ShipNationality(slot)), str,
                nations::Reputation(nation));
            Log("shipyard:   purpose %d, +0x2A %d, city %d flagged as having "
                "sent her, flags 0x%08X -> 0x%08X (bit 0x8 added by us -- the "
                "AI branch tests it before anything else)",
                game::ShipPurposeOf(slot), game::ShipRole(slot), city,
                before, game::ShipFlagBits(slot));
            char m2[160];
            _snprintf_s(m2, sizeof(m2), _TRUNCATE,
                        "%s hunter dispatched, strength %d.",
                        game::NationName(game::ShipNationality(slot)), str);
            content::PostDebugNotice(m2, 8);
            return;
        }

        // Give it somewhere to be and something to be doing. Only on the
        // role-4 builder, so H stays a clean control: if H and J now behave
        // differently, the orders are what did it.
        if (which == kYardRole4) {
            const int dest = FindDistantCity(city);
            if (dest < 0) {
                Log("shipyard: no port in the 15k-90k band to send it to -- "
                    "orders unchanged");
            } else {
                game::SetShipRoleRaw(slot, g_testRole);
                game::SetShipDestCityRaw(slot, dest);
                Log("shipyard: slot %d ORDERED -- role %d, dest city %d (%s, "
                    "%d away). Watch whether she sails for it.",
                    slot, game::ShipRole(slot), dest,
                    game::NationName(game::CityNation(dest)),
                    game::CityDistance(dest));
            }
        }
        Log("shipyard: slot %d at (%d,%d); the player is at (%d,%d)",
            slot,
            *(const int*)(rec + 0x0C) / 1000, *(const int*)(rec + 0x10) / 1000,
            game::PlayerX() / 1000, game::PlayerY() / 1000);

        char msg[160];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "Built %s ship %d, role %d.",
                    game::NationName(game::ShipNationality(slot)), slot,
                    game::ShipRole(slot));
        content::PostDebugNotice(msg, 8);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // A fault here is a RESULT: it says the call is not safe in this form.
        // The marker is dropped so a second keypress cannot repeat it.
        g_shipyard = false;
        Log("shipyard: EXCEPTION 0x%08X -- the factory is not callable this "
            "way. Shipyard disarmed for the session; do NOT save this game.",
            GetExceptionCode());
    }
}

// ------------------------------------------- diffing a ship against a real one
// Four rounds of "set the field the disassembly points at, watch her sit there"
// is enough. The engine builds ships that work; we build ships that do not. The
// difference is in the 1116 bytes of the record, and reading both is cheaper
// and more honest than guessing which field is next.
//
// Dumps our most recently built ship beside a vessel the GAME made, as
// offset-by-offset differences. Anything that matters is in that list.
static void DiffShipAgainstNatural()
{
    if (!state::InGame()) { Log("shipdiff: refused -- not in a career"); return; }
    if (g_lastSpawned < 0) {
        Log("shipdiff: nothing built yet this session -- press Ctrl+Shift+O "
            "first, then this");
        return;
    }

    __try {
        // A ship the game made: any occupied slot that is not ours and not the
        // player's. Prefer one that is actually moving, since a natural ship
        // sitting in port is no better a reference than ours.
        int natural = -1;
        for (int i = 1; i < game::addr::kMaxShips; ++i) {
            if (i == g_lastSpawned) continue;
            if (game::ShipType(i) == -1) continue;
            natural = i;
            if (game::ShipPurposeOf(i) != 0) break;   // an interesting one
        }
        if (natural < 0) {
            Log("shipdiff: no other ship on the water to compare against");
            return;
        }

        const uintptr_t ours = game::ShipRecord(g_lastSpawned);
        const uintptr_t nat  = game::ShipRecord(natural);

        Log("shipdiff: ours = slot %d (purpose %d), natural = slot %d "
            "(purpose %d). Differences across the whole 0x45C record:",
            g_lastSpawned, game::ShipPurposeOf(g_lastSpawned),
            natural, game::ShipPurposeOf(natural));

        int shown = 0;
        for (int off = 0; off + 4 <= game::addr::kShipStride; off += 4) {
            const unsigned a = *(const unsigned*)(ours + off);
            const unsigned b = *(const unsigned*)(nat + off);
            if (a == b) continue;
            // Position, heading and anything obviously per-ship will always
            // differ; they are printed anyway because "obviously" is exactly
            // the judgement that has been wrong four times running.
            Log("shipdiff:   +0x%03X  ours 0x%08X (%d)   natural 0x%08X (%d)",
                off, a, (int)a, b, (int)b);
            if (++shown > 60) { Log("shipdiff:   ...truncated"); break; }
        }
        if (shown == 0)
            Log("shipdiff:   none -- the records are identical, which would be "
                "a finding in itself");

        content::PostDebugNotice("Ship diff written to pemf.log.", 6);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("shipdiff: EXCEPTION 0x%08X", GetExceptionCode());
    }
}

// ------------------------------------- borrowing a working ship's control state
// The diff is short, so rather than guess which of seven fields matters, copy
// them all from a vessel the game made and see whether ours comes alive. If it
// does, bisecting the list is a couple of minutes; if it does not, the answer
// is not in the record at all and that is worth knowing before another round of
// disassembly.
//
// Position and nationality are deliberately NOT copied -- ours should stay
// where she is, flying her own colours, so any movement is her own.
static void BorrowControlState()
{
    if (!g_shipyard) { Log("borrow: needs PEMF\\shipyard.on"); return; }
    if (!state::InGame()) { Log("borrow: refused -- not in a career"); return; }
    if (g_lastSpawned < 0) {
        Log("borrow: build one first with Ctrl+Shift+O");
        return;
    }

    __try {
        // Prefer a ship that is demonstrably underway: one whose position has
        // changed since we last looked is the only kind worth copying from.
        int natural = -1;
        for (int i = 1; i < game::addr::kMaxShips; ++i) {
            if (i == g_lastSpawned || game::ShipType(i) == -1) continue;
            if (game::ShipPurposeOf(i) == 0) continue;   // wants a real errand
            natural = i;
            break;
        }
        if (natural < 0) {
            for (int i = 1; i < game::addr::kMaxShips; ++i) {
                if (i == g_lastSpawned || game::ShipType(i) == -1) continue;
                natural = i; break;
            }
        }
        if (natural < 0) { Log("borrow: no ship to copy from"); return; }

        const uintptr_t ours = game::ShipRecord(g_lastSpawned);
        const uintptr_t nat  = game::ShipRecord(natural);

        // Every offset the diff flagged, except position (+0x0C/+0x10) and
        // nationality (+0x04).
        static const int kCopy[] = {
            0x14,   // heading
            0x18,   // ?
            0x24, 0x28, 0x2C, 0x30,
            0x38,   // ours 0, natural 1
            0x3C, 0x40,
            0x44,   // ours 0, natural 0x80000000
            0x48, 0x4C,
            0x50,   // ours 0, natural 100
            0x58,   // flags: ours 0x208, natural 0x14600
            0x5C, 0x64
        };
        for (int k = 0; k < (int)(sizeof(kCopy) / sizeof(int)); ++k) {
            const int off = kCopy[k];
            *(unsigned*)(ours + off) = *(const unsigned*)(nat + off);
        }

        Log("borrow: copied %d control fields from slot %d onto our slot %d. "
            "Position and colours left alone, so any movement is hers.",
            (int)(sizeof(kCopy) / sizeof(int)), natural, g_lastSpawned);
        Log("borrow:   ours now -- purpose %d, flags 0x%08X, dest city %d, "
            "+0x50 %d",
            game::ShipPurposeOf(g_lastSpawned),
            game::ShipFlagBits(g_lastSpawned),
            game::ShipDestCity(g_lastSpawned),
            *(const short*)(ours + 0x50));
        content::PostDebugNotice("Control state borrowed. Watch her.", 8);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("borrow: EXCEPTION 0x%08X", GetExceptionCode());
    }
}

// ------------------------------------------------ reputation, on a hotkey
// Suspicion's whole consequence is that it drives reputation, and reputation is
// what the game reads to decide how hard to come after you. Testing any of that
// by earning it honestly would take hours a session, so this sets it.
//
// The presets are the thresholds the ENGINE uses, not round numbers:
//
//   +30  comfortably above the +3 promotion gate; welcome everywhere
//     0  neutral, and what a fresh career has
//   -10  hostile: their ports drop out of the settlement search entirely
//   -20  a price on your head, and the maximum-strength hunter (2 - rep/10
//        clamps at 4 here)
//   -40  well past every threshold, to check nothing changes further
//
// Applied to the nation of the NEAREST PORT, so it always concerns the crown
// whose water you are testing in.
static const int kRepPresets[] = { 0, -10, -20, -40, 30 };
static int g_repPreset = 0;

static void CycleReputation()
{
    if (!g_shipyard) {
        Log("reputation: not armed -- this writes career state, so it needs "
            "PEMF\\shipyard.on like the shipyard keys");
        return;
    }
    if (!state::InGame()) { Log("reputation: refused -- not in a career"); return; }

    __try {
        const int city = game::NearestCity(60000);
        if (city < 0) {
            Log("reputation: no port in range -- sail nearer one so there is a "
                "crown to be in trouble with");
            content::PostDebugNotice("No port in range.", 5);
            return;
        }
        const int nation = game::CityNation(city);
        if (nation < 0 || nation >= game::addr::kNationsWithRank) {
            Log("reputation: nearest port %d has nation %d, which is not a "
                "crown -- nothing to set", city, nation);
            return;
        }

        g_repPreset = (g_repPreset + 1) % (int)(sizeof(kRepPresets) / sizeof(int));
        const int before = nations::Reputation(nation);
        game::SetReputationRaw(nation, kRepPresets[g_repPreset]);
        const int after = nations::Reputation(nation);

        Log("reputation: %s %d -> %d  (port %d, its own opinion of us is %d)",
            game::NationName(nation), before, after, city,
            game::CityReputationOf(city));
        Log("reputation:   at %d -- ports %s, %s, a hunter would sail at "
            "strength %d",
            after,
            after < 0 ? "CLOSED to us" : "open",
            after < -1 ? "there is a PRICE ON OUR HEAD" : "no bounty",
            game::HunterStrengthFor(nation));

        char msg[160];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "%s reputation now %d.",
                    game::NationName(nation), after);
        content::PostDebugNotice(msg, 6);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("reputation: EXCEPTION 0x%08X", GetExceptionCode());
    }
}

// ------------------------------------------------------- the nations probe
// Everything nations.h claims, printed from a running game so it can be
// checked rather than believed. The addresses behind it were read out of the
// disassembly, which establishes SHAPE but not meaning: that the relations
// matrix is 8x8 int32 is settled by the instruction that clears it, but that
// 1 means war is an inference until a war is declared on screen and the grid
// is seen to change.
//
// So this dump is arranged around the three claims that could be wrong:
//
//   1. rank[] identifies the nation chosen at character creation
//      -> start a career per crown; exactly one rank should be non-zero
//   2. the relations grid is live
//      -> sail until the news announces a war; the grid should change to match
//   3. slots 4 and 5 are permanently hostile to every crown
//      -> should be a solid row and column of W, in every career, always
//
// Read-only from first line to last.
static void ReportNations(const char* when)
{
    static const char* const kSlotName[game::addr::kRelationSlots] = {
        "Spanish", "English", "French", "Dutch", "Pirate", "slot5"
    };

    __try {
        Log("nations [%s]: -------------------------------------------", when);

        // The grid. Rows are "how does A regard B", though the engine keeps it
        // symmetrical -- the reset writes both [a][b] and [b][a] together, so
        // an asymmetry appearing here would itself be a finding.
        Log("nations: relations grid (W = at war, T = treaty, . = neutral)");
        Log("nations:            Sp  En  Fr  Du  Pi  s5");
        for (int a = 0; a < game::addr::kRelationSlots; ++a) {
            char row[96];
            int  used = _snprintf_s(row, sizeof(row), _TRUNCATE,
                                    "nations:   %-7s ", kSlotName[a]);
            for (int b = 0; b < game::addr::kRelationSlots; ++b) {
                const int  v = nations::Relation(a, b);
                const char c = (a == b) ? '-'
                             : (v == game::addr::kAtWar)  ? 'W'
                             : (v == game::addr::kTreaty) ? 'T'
                             : (v == 0)                   ? '.' : '?';
                // A '?' is worth seeing raw: it means the matrix holds a value
                // this framework has no name for, and the reading is incomplete.
                used += _snprintf_s(row + used, sizeof(row) - used, _TRUNCATE,
                                    "  %c ", c);
            }
            Log("%s", row);
        }

        // Where the player stands. Rank 0 is the interesting value, not a gap:
        // it is how the game says "you hold no commission with this crown".
        Log("nations: the player's standing --");
        for (int n = 0; n < game::addr::kNationsWithRank; ++n) {
            const int rank = nations::Rank(n);
            Log("nations:   %-7s rank %d (%-8s)  reputation %d",
                game::NationName(n), rank, nations::RankName(rank),
                nations::Reputation(n));
        }

        // The engine's cached answer, and our reimplementation of how it got
        // there. They are logged together so a disagreement cannot go unseen.
        const int home    = nations::HomeNation();
        const int derived = nations::DeriveHomeNation();
        Log("nations: home nation = %d (%s)%s",
            home, home >= 0 ? game::NationName(home) : "none",
            nations::ServesMoreThanOne() ? "  [holds more than one commission]"
                                         : "");
        if (derived != home) {
            Log("nations: NOTE derived-from-rank says %d (%s) -- the engine's "
                "own value disagrees. Expected while every rank is 0; a "
                "disagreement once ranks are set means an address is wrong.",
                derived, derived >= 0 ? game::NationName(derived) : "none");
        }

        // The flag we fly against the ship record's nationality field.
        //
        // MEASURED 2026-07-28 and worth stating plainly, because it closed off
        // the approach this probe was built to open: across four careers begun
        // under DIFFERENT crowns, the player's field read 0 every single time.
        // It does not track the nation you chose -- it is simply never set for
        // entry 0. So it cannot be read to learn who we are, and writing it is
        // unlikely to tell the AI anything either, since a game that decided
        // hostility from it would treat every player as Spanish.
        //
        // Kept in the dump because a value that is reliably 0 is worth SEEING
        // stay 0: if it ever moves, that is a finding.
        int nameLen = 0;
        const char* chosen = game::EngineString(game::addr::CustomFlagName, &nameLen);
        char safeName[128] = "(none)";
        if (chosen && nameLen > 0) SafeStr(chosen, safeName, sizeof(safeName));
        const int seen = state::Nationality();
        Log("nations: we FLY '%s' (a texture); ship-record nationality reads "
            "%d (%s) -- measured vestigial for the player, expected 0",
            safeName, seen, game::NationName(seen));

        // The whole player record, and the pending one it was copied from.
        //
        // Sizes are the save serializer's own (see game.h): 216 bytes live, 184
        // staging. Dumped ENTIRE and raw, because the point is no longer to
        // read a field we have already identified -- it is to find one we have
        // not. Whatever records the nation chosen at character creation is
        // inside these bytes, and the way to find it is to start a career under
        // each crown and see which offset tracks the choice.
        //
        // Naming the bytes would be pretending to know. Printing all of them
        // and diffing four careers is the measurement.
        for (int half = 0; half < 2; ++half) {
            const uintptr_t base = half ? game::addr::PlayerRecord
                                        : game::addr::CareerStaging;
            const size_t    len  = half ? game::addr::kPlayerRecBytes
                                        : game::addr::kCareerStgBytes;
            const char*     tag  = half ? "live   " : "staging";

            for (size_t row = 0; row < len; row += 16) {
                char line[160];
                int  used = _snprintf_s(line, sizeof(line), _TRUNCATE,
                                        "nations:   %s +%03u:", tag,
                                        (unsigned)row);
                for (size_t i = 0; i < 16 && row + i < len; ++i) {
                    const unsigned char* p =
                        (const unsigned char*)(base + row + i);
                    if (!PageReadable(p, 1)) {
                        used += _snprintf_s(line + used, sizeof(line) - used,
                                            _TRUNCATE, " --");
                        continue;
                    }
                    used += _snprintf_s(line + used, sizeof(line) - used,
                                        _TRUNCATE, " %02x", *p);
                }
                Log("%s", line);
            }

            // The same bytes again, as offset=value pairs for the non-zero ones
            // only. Most of the record is zero, so this is the line that can
            // actually be read side by side across four careers -- the hex
            // block above is there so nothing is lost if the summary misleads.
            char sum[512];
            int  used = _snprintf_s(sum, sizeof(sum), _TRUNCATE,
                                    "nations:   %s non-zero:", tag);
            for (size_t i = 0; i < len; ++i) {
                const unsigned char* p = (const unsigned char*)(base + i);
                if (!PageReadable(p, 1) || *p == 0) continue;
                const int n = _snprintf_s(sum + used, sizeof(sum) - used,
                                          _TRUNCATE, " %u=%02x",
                                          (unsigned)i, *p);
                if (n < 0) break;              // summary full; the hex has it all
                used += n;
            }
            Log("%s", sum);
        }
        Log("nations: -------------------------------------------------");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("nations: EXCEPTION 0x%08X -- one of the addresses in game.h is "
            "wrong, or this was hit outside a career", GetExceptionCode());
    }
}

typedef HANDLE(WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                      DWORD, DWORD, HANDLE);
typedef HANDLE(WINAPI *CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                      DWORD, DWORD, HANDLE);

static timeGetTime_t  g_origTimeGetTime = nullptr;
static PeekMessageA_t g_origPeekMessage = nullptr;
static CreateFileA_t  g_origCreateFile  = nullptr;
static CreateFileW_t  g_origCreateFileW = nullptr;

// Cheap tick. Only ever POSTS work -- it must never present anything, because
// it can be called from anywhere in the frame.
static DWORD WINAPI Hook_timeGetTime(void)
{
    DWORD r = g_origTimeGetTime ? g_origTimeGetTime() : GetTickCount();

    // Thread affinity: Pirates!.exe imports CreateThread and _beginthreadex, so
    // its workers reach this hook too. Game state is not thread-safe.
    const DWORD tid = GetCurrentThreadId();
    if (g_gameThreadId == 0) {
        g_gameThreadId = tid;
        Log("game thread latched: %lu", tid);
    } else if (tid != g_gameThreadId) {
        if (!g_loggedOtherThread) {
            g_loggedOtherThread = true;
            Log("NOTE: timeGetTime also called from thread %lu -- ignored", tid);
        }
        return r;
    }

    ++g_tickCount;
    if (events::Faulted() || !g_targetOK || !kDebugHotkeys) return r;

    // Throttle: this runs many times per frame across ~84 call sites.
    static DWORD lastPoll = 0;
    DWORD now = GetTickCount();
    if (now - lastPoll < 50) return r;
    lastPoll = now;

    bool mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                (GetAsyncKeyState(VK_SHIFT)   & 0x8000);
    bool k1 = mods && (GetAsyncKeyState('1') & 0x8000);
    bool k2 = mods && (GetAsyncKeyState('2') & 0x8000);
    bool k3 = mods && (GetAsyncKeyState('3') & 0x8000);
    bool k4 = mods && (GetAsyncKeyState('4') & 0x8000);
    bool k5 = mods && (GetAsyncKeyState('5') & 0x8000);
    bool k6 = mods && (GetAsyncKeyState('6') & 0x8000);
    bool k7 = mods && (GetAsyncKeyState('7') & 0x8000);
    bool k8 = mods && (GetAsyncKeyState('8') & 0x8000);
    bool k9 = mods && (GetAsyncKeyState('9') & 0x8000);
    bool k0 = mods && (GetAsyncKeyState('0') & 0x8000);
    // The digits were all spoken for by the time the nations probe arrived.
    bool kN = mods && (GetAsyncKeyState('N') & 0x8000);
    bool kH = mods && (GetAsyncKeyState('H') & 0x8000);
    bool kJ = mods && (GetAsyncKeyState('J') & 0x8000);
    bool kK = mods && (GetAsyncKeyState('K') & 0x8000);
    bool kL = mods && (GetAsyncKeyState('L') & 0x8000);
    bool kO = mods && (GetAsyncKeyState('O') & 0x8000);
    bool kU = mods && (GetAsyncKeyState('U') & 0x8000);
    bool kP = mods && (GetAsyncKeyState('P') & 0x8000);
    bool kY = mods && (GetAsyncKeyState('Y') & 0x8000);
    bool down = k1 || k2 || k3 || k4 || k5 || k6 || k7 || k8 || k9 || k0 ||
                kN || kH || kJ || kK || kL || kO || kU || kP || kY;

    bool rising = down && !g_prevKeyDown;
    g_prevKeyDown = down;
    if (!rising) return r;

    // Ctrl+Shift+3 draws our own text with no authored event behind it -- the
    // shortest possible test of the frame hook. Posting is all that happens
    // here; the notice is drawn from inside the frame.
    if (k3) {
        content::PostDebugNotice("PEMF: drawing through the frame hook.", 8);
        return r;
    }
    // Ctrl+Shift+4 hangs a line over the player's ship in the world, so it
    // tracks the vessel as you sail -- the treatment the game gives other
    // ships' speech.
    if (k4) {
        content::PostDebugNotice("Anchored to the ship.", 8, true);
        return r;
    }

    // Ctrl+Shift+5 asks the engine what it thinks the trade goods are called,
    // for indices 0-6. Those are the known list, so this doubles as a check
    // that the probe itself is sound before anything is concluded from it.
    if (k5) {
        ProbeItemNames(0, 6);
        content::PostDebugNotice("Item names 0-6 written to pemf.log.", 6);
        return r;
    }
    // Ctrl+Shift+6 asks for index 7 -- PAST the end of the stock list. Kept on
    // its own key deliberately: the engine's lookup may well not be bounds
    // checked, so this is the one that could misbehave, and nobody should hit
    // it by accident while testing the others.
    if (k6) {
        Log("itemprobe: index 7 is PAST the end of the stock [ITEM] list. "
            "If text.ini has not been extended, expect empty or a fault.");
        ProbeItemNames(7, 7);
        content::PostDebugNotice("Item index 7 probed -- see pemf.log.", 6);
        return r;
    }
    // Ctrl+Shift+7 logs every data file the game opens OR FAILS to open. The
    // misses are the point: a probe for a loose file that is not there is the
    // evidence that dropping one in would be picked up.
    if (k7) {
        g_fileProbe = !g_fileProbe;
        Log("fileprobe: %s", g_fileProbe ? "ON -- .ini/.txt/.csv/.fpk opens and "
                                           "misses will be logged"
                                         : "off");
        content::PostDebugNotice(g_fileProbe ? "File probe ON."
                                             : "File probe off.", 5);
        return r;
    }
    // Ctrl+Shift+N dumps the relations grid and the player's standing with each
    // crown. Unlike the false-colours keys below it, this one is NOT behind the
    // marker file: it only reads, so there is nothing to opt in to.
    if (kN) {
        ReportNations("asked");
        content::PostDebugNotice("Nations reported to pemf.log.", 5);
        return r;
    }
    // Ctrl+Shift+H/J/K each ask a DIFFERENT one of the engine's ship builders
    // for a vessel. Behind their own marker file, because they create state
    // that a second keypress cannot take back.
    if (kH || kJ || kK || kO) {
        RunShipyardExperiment(kH ? kYardRole0 : kJ ? kYardRole4
                            : kK ? kYardRole3 : kYardRoleOnly);
        return r;
    }
    // Ctrl+Shift+L picks which role the next J-built ship is given. Roles 1 and
    // 2 exist only inside code we cannot call, so stamping is the only way to
    // find out what they mean.
    // Ctrl+Shift+U cycles our standing with the nearest port's crown through
    // the engine's own thresholds. Behind the shipyard marker: it writes career
    // state that a keypress cannot take back either.
    if (kU) {
        CycleReputation();
        return r;
    }
    // Ctrl+Shift+P diffs the last ship we built against one the game built.
    // Read-only, so no marker needed.
    if (kP) {
        DiffShipAgainstNatural();
        return r;
    }
    // Ctrl+Shift+Y borrows a working ship's control state wholesale.
    if (kY) {
        BorrowControlState();
        return r;
    }
    if (kL) {
        g_testPurpose = (g_testPurpose % 6) + 1;    // 1..6, round again
        Log("shipyard: next ship will be stamped PURPOSE %d (%s)",
            g_testPurpose, game::PurposeName(g_testPurpose));
        char msg[100];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "Next: %s",
                    game::PurposeName(g_testPurpose));
        content::PostDebugNotice(msg, 5);
        return r;
    }

    // ------------------------------------------------------ false colours
    // Ctrl+Shift+8 cycles the colours the player's vessel is seen to be flying,
    // Ctrl+Shift+9 puts the true ones back, Ctrl+Shift+0 just reports without
    // touching anything.
    //
    // This is the ONE probe in this build that writes game state, which is why
    // it is armed by a marker file rather than being live for anyone who
    // presses a key. It exists to answer, in game, what static analysis cannot:
    // does writing this field change the flag DRAWN on the ship, change how the
    // AI TREATS you, both, or neither?
    if (k8 || k9 || k0) {
        if (!g_falseColours) {
            Log("falsecolours: not armed -- drop PEMF\\falsecolours.on next to "
                "the exe to enable (it writes game state, so it is opt-in)");
            return r;
        }
        if (k0) {
            ReportColours("asked");
            content::PostDebugNotice("Colours reported to pemf.log.", 5);
            return r;
        }
        if (k9) {
            if (!g_haveTrueFlag) {
                Log("falsecolours: true colours were never captured -- nothing "
                    "to restore to");
                return r;
            }
            if (game::SetPlayerFlagByName(g_trueFlagName)) {
                session::RecordFlag(g_trueFlagName);
                NoteColoursFlown(g_trueFlagName);
                Log("falsecolours: true colours restored -- '%s'", g_trueFlagName);
                content::PostDebugNotice("True colours restored.", 5);
            } else {
                Log("falsecolours: could not reload '%s'", g_trueFlagName);
            }
            return r;
        }

        // Ctrl+Shift+8 flies the next flag from THE GAME'S OWN LIST, BY NAME.
        // That is the point of this pass: what flies is something an author
        // could write down, not a pointer we happened to catch.
        RememberTrueFlag();
        const int nflags = FlagCount();
        if (nflags <= 0) {
            Log("falsecolours: our scan found no flags in the custom folder(s)");
            content::PostDebugNotice("No custom flags found.", 6);
            return r;
        }

        g_flagCursor = (g_flagCursor + 1) % nflags;
        char pick[128];
        strncpy_s(pick, sizeof(pick), FlagAt(g_flagCursor), _TRUNCATE);

        if (game::SetPlayerFlagByName(pick)) {
            session::RecordFlag(pick);          // travels with the save
            NoteColoursFlown(pick);
            Log("falsecolours: flying flag[%d] '%s'", g_flagCursor, pick);
            char msg[160];
            _snprintf_s(msg, sizeof(msg), _TRUNCATE, "Flying %s.", pick);
            content::PostDebugNotice(msg, 6);
        } else {
            // A refusal is a result, not a failure: it says the name the list
            // holds is not the name the loader wants, which is exactly the
            // thing this pass exists to pin down.
            Log("falsecolours: REFUSED flag[%d] '%s' -- the loader would not "
                "resolve it. The enumerated name probably needs a path or a "
                "different extension.", g_flagCursor, pick);
            content::PostDebugNotice("That flag name would not load.", 6);
        }
        return r;
    }

    // POST only. The card is presented later, at the safe point.
    // Debug triggers fire content events by index. Ctrl+Shift+1 runs the first
    // authored event, Ctrl+Shift+2 the second -- so the hotkeys exercise the
    // real content path rather than a separate hardcoded one.
    int index = k1 ? 0 : 1;
    const content::Event* ev = content::Get(index);
    if (!ev) {
        Log("debug: no content event at index %d (%d loaded)",
            index, content::Count());
        return r;
    }
    events::Post(RunContentEvent, index, ev->id.c_str());
    return r;
}

// The safe point.
static BOOL WINAPI Hook_PeekMessageA(LPMSG msg, HWND wnd, UINT min, UINT max,
                                     UINT remove)
{
    BOOL r = g_origPeekMessage(msg, wnd, min, max, remove);

    if (!g_targetOK || GetCurrentThreadId() != g_gameThreadId) return r;

    ++g_peekCalls;
    uintptr_t ret = (uintptr_t)_ReturnAddress();
    if (ret == kMainLoopPeekRet) {
        RunSafePoint();
    } else if (!g_safePointFound && g_peekCalls > 2000 && !g_fallbackWarned) {
        // Defensive: if the expected call site never appears the mod would be
        // silently dead. Say so loudly rather than fail quietly.
        g_fallbackWarned = true;
        Log("WARNING: main-loop PeekMessageA (ret 0x%08X) never seen after %lu "
            "calls; last ret was 0x%08X. Deferred dispatch is NOT running.",
            (unsigned)kMainLoopPeekRet, g_peekCalls, (unsigned)ret);
    }
    return r;
}

// Save/load detection. Touches no game memory, so it is safe from any thread;
// session state is guarded by its own lock.
//
// Shared by the A and W hooks -- the exe imports BOTH, and a save going through
// the wide path would otherwise bypass detection entirely and silently
// desynchronise our state from the player's saves.
static void HandleSaveFile(const char* path, bool forWrite)
{
    if (!path || !session::EndsWithNoCase(path, session::kSaveExt)) return;
    __try {
        if (session::OnSaveFileOpened(path, forWrite) == session::FileEvent::Loaded) {
            // Anything queued belongs to the career we just left. Firing it
            // into the loaded one would reference a world that no longer
            // exists -- exactly what the sidecar design exists to prevent.
            events::Clear("save loaded");
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("session: exception 0x%08X handling %s", GetExceptionCode(), path);
    }
}

// ------------------------------------------------------------- file probing
// Where does the game LOOK for a file, and in what order? The answer decides
// whether a loose text.ini can override the packed one, and it is not knowable
// from the binary alone -- but every candidate path goes through CreateFile,
// including the ones that fail.
//
// The FAILURES are the interesting half: a probe for a loose file that is not
// there is exactly the evidence that dropping one in would be picked up. The
// save-detection hook ignores failed opens, so this is separate.
//
// Off by default. Ctrl+Shift+7 turns it on, so it costs nothing until asked
// for, and it is capped so a game that opens thousands of files cannot fill
// the log.
bool         g_fileProbe      = false;
inline int   g_fileProbeCount = 0;
constexpr int kFileProbeMax   = 400;

static void LogFileProbe(const char* path, bool opened)
{
    if (!g_fileProbe || !path) return;
    if (g_fileProbeCount >= kFileProbeMax) return;

    // Data files only. Textures and meshes would drown the signal. Anything
    // named "text" is kept whatever its extension, since that is the file the
    // whole exercise is about and we should not assume how it is named.
    const char* dot  = strrchr(path, '.');
    const char* leaf = strrchr(path, '\\');
    leaf = leaf ? leaf + 1 : path;
    const bool interesting =
        (dot && (!_stricmp(dot, ".ini") || !_stricmp(dot, ".txt") ||
                 !_stricmp(dot, ".csv") || !_stricmp(dot, ".fpk"))) ||
        (StrStrIA(leaf, "text") != nullptr);
    if (!interesting) return;

    ++g_fileProbeCount;
    Log("fileprobe: %-5s %s", opened ? "OPEN" : "MISS", path);
    if (g_fileProbeCount == kFileProbeMax)
        Log("fileprobe: cap reached (%d) -- no more will be logged",
            kFileProbeMax);
}

// ------------------------------------------------------- trade-good probing
// Read back the engine's own name for each item index. Indices 0-6 are the
// known list, so they double as a check that the probe itself is sound: if
// they come back Gold/Food/Luxuries/Goods/Spice/Sugar/Cannon, the probe works
// and anything it says about index 7 can be believed.
static void ProbeItemNames(int first, int last)
{
    Log("itemprobe: reading @ITEM for indices %d..%d", first, last);
    for (int i = first; i <= last; ++i) {
        char name[128] = {0};
        bool ok = false;
        __try {
            ok = game::ItemName(i, name, sizeof(name));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("itemprobe: [%d] EXCEPTION 0x%08X -- the lookup is NOT bounds "
                "checked past the end of the list", i, GetExceptionCode());
            return;
        }
        Log("itemprobe: [%d] %s", i, ok ? name : "(empty)");
    }
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD access, DWORD share,
                                      LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                      DWORD flags, HANDLE tmpl)
{
    HANDLE h = g_origCreateFile(name, access, share, sa, disp, flags, tmpl);
    LogFileProbe(name, h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE)
        HandleSaveFile(name, (access & GENERIC_WRITE) != 0);
    return h;
}

static HANDLE WINAPI Hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share,
                                      LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                      DWORD flags, HANDLE tmpl)
{
    HANDLE h = g_origCreateFileW(name, access, share, sa, disp, flags, tmpl);
    if (h != INVALID_HANDLE_VALUE && name) {
        char narrow[MAX_PATH * 2];
        int n = WideCharToMultiByte(CP_ACP, 0, name, -1, narrow,
                                    sizeof(narrow), nullptr, nullptr);
        if (n > 0 && session::EndsWithNoCase(narrow, session::kSaveExt)) {
            Log("session: NOTE save reached us via CreateFileW");
            HandleSaveFile(narrow, (access & GENERIC_WRITE) != 0);
        }
    }
    return h;
}

// ------------------------------------------------------------------- hooks
// Each hook with the absolute IAT-slot address for its function. Hook-by-name
// works on the GOG build; the by-address slot patch covers the packed Steam
// build, whose import name tables are destroyed but whose slots are populated.
struct HookSpec { uintptr_t slot; const char* dll; const char* fn; void* hook; void** orig; };
static HookSpec g_hookSpecs[] = {
    { game::addr::SlotTimeGetTime,  "WINMM.dll",    "timeGetTime",  (void*)&Hook_timeGetTime,  (void**)&g_origTimeGetTime },
    { game::addr::SlotPeekMessageA, "USER32.dll",   "PeekMessageA", (void*)&Hook_PeekMessageA, (void**)&g_origPeekMessage },
    { game::addr::SlotCreateFileA,  "KERNEL32.dll", "CreateFileA",  (void*)&Hook_CreateFileA,  (void**)&g_origCreateFile  },
    { game::addr::SlotCreateFileW,  "KERNEL32.dll", "CreateFileW",  (void*)&Hook_CreateFileW,  (void**)&g_origCreateFileW },
};

// Try the proven name-based hook first (GOG, unchanged), then fall back to
// patching the slot by absolute address (Steam).
static bool InstallOneHook(const HookSpec& h)
{
    if (HookIAT(h.dll, h.fn, h.hook, h.orig)) return true;
    return HookSlotByAddr(h.slot, h.dll, h.hook, h.orig);
}

// --------------------------------------------------------------- entry point
static DWORD WINAPI Init(LPVOID)
{
    char dir[MAX_PATH]{};
    GetModuleFileNameA(GetModuleHandleA(NULL), dir, MAX_PATH);
    if (char* slash = strrchr(dir, '\\')) *slash = 0;

    char logPath[MAX_PATH];
    _snprintf_s(logPath, sizeof(logPath), _TRUNCATE, "%s\\pemf.log", dir);
    fopen_s(&g_log, logPath, "w");
    session::InitLock();

    SYSTEMTIME st; GetLocalTime(&st);
    Log("=== PEMF loaded === pid=%lu  %04d-%02d-%02d %02d:%02d:%02d",
        GetCurrentProcessId(), st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    Log("host: %s", dir);

    // The file probe has to be armed BEFORE the game opens anything, because
    // the text and asset systems load during startup -- long before a hotkey
    // could be pressed. Toggling it in-game shows only the handful of files
    // touched afterwards, which is how the first attempt found nothing but
    // Config.ini. So: drop a marker file next to the exe and it is on from the
    // first open.
    {
        char marker[MAX_PATH];
        _snprintf_s(marker, sizeof(marker), _TRUNCATE,
                    "%s\\PEMF\\fileprobe.on", dir);
        if (GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES) {
            g_fileProbe = true;
            Log("fileprobe: ARMED AT STARTUP (found %s) -- every .ini/.txt/"
                ".csv/.fpk open and miss will be logged from here", marker);
        }
    }

    strncpy_s(g_gameDir, sizeof(g_gameDir), dir, _TRUNCATE);
    ScanFlags();
    suspicion::LoadTuning(dir);
    InstallWasdKeymap(dir);

    // False colours writes game state, so it is opt-in the same way.
    {
        char marker[MAX_PATH];
        _snprintf_s(marker, sizeof(marker), _TRUNCATE,
                    "%s\\PEMF\\falsecolours.on", dir);
        if (GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES) {
            g_falseColours = true;
            Log("falsecolours: ARMED (found %s) -- pick flags in Options to "
                "capture them, then Ctrl+Shift+8 flies the next captured one, "
                "9 restores the first, 0 reports", marker);
        }
    }

    // The shipyard gets its OWN marker, deliberately not shared with false
    // colours. Everything else behind a marker can be undone by pressing the
    // key again; this one creates a ship that persists into the save. A
    // different risk deserves a different opt-in.
    {
        char marker[MAX_PATH];
        _snprintf_s(marker, sizeof(marker), _TRUNCATE,
                    "%s\\PEMF\\shipyard.on", dir);
        if (GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES) {
            g_shipyard = true;
            Log("shipyard: ARMED (found %s) -- Ctrl+Shift+H/J/K build a ship "
                "using each of the engine's three builders. THIS WRITES "
                "PERMANENT GAME STATE; use a save you do not mind losing.",
                marker);
            Log("shipyard:   H = role 0 (0x%08X) %s",
                (unsigned)game::SpawnShipFn,
                game::SpawnShipCallable() ? "MATCHES" : "MISMATCH");
            Log("shipyard:   J = role 4 (0x%08X) %s",
                (unsigned)game::SpawnShipRole4Fn,
                game::SpawnRole4Callable() ? "MATCHES" : "MISMATCH");
            Log("shipyard:   K = role 3 (0x%08X) %s",
                (unsigned)game::SpawnShipRole3Fn,
                game::SpawnRole3Callable() ? "MATCHES" : "MISMATCH");
            Log("shipyard:   O = role stamped, NO destination -- the isolated "
                "role test; L cycles which role");
        }
    }

    // Build diagnostics -- at load, and again after any unpacker has run.
    DiagnoseImage("at-load");
    CloseHandle(CreateThread(nullptr, 0, DelayedDiag, nullptr, 0, nullptr));

    // Wait for the host to be ready. The GOG build verifies immediately; the
    // DRM-packed Steam build unpacks its .text lazily, so poll the byte probes
    // until they match (or give up and load passively). Reading not-yet-unpacked
    // .text returns garbage, not a fault, so polling is safe.
    char why[256] = {0};
    int waited = 0;
    for (; waited < 150 && !game::VerifyTarget(why, sizeof(why)); ++waited)
        Sleep(100);                               // up to ~15s
    g_targetOK = game::VerifyTarget(why, sizeof(why));

    if (!g_targetOK) {
        Log("TARGET MISMATCH: %s", why);
        Log("PASSIVE LOAD: no hooks installed, no game memory touched");
    } else {
        if (waited) Log("target verified after %d ms (host unpacked)", waited * 100);
        else        Log("target verified: offsets match the expected build");

        // Install the hooks. On the packed build a slot may be filled slightly
        // after .text unpacks, so retry until all four take (or time out). Each
        // hook is installed at most once -- never re-patch an already-hooked slot
        // (that would capture our own thunk as the "original").
        int installed = 0;
        for (int attempt = 0; attempt < 100 && installed < 4; ++attempt) {
            installed = 0;
            for (auto& h : g_hookSpecs) {
                if (*h.orig) { ++installed; continue; }   // already hooked
                if (InstallOneHook(h)) ++installed;
            }
            if (installed < 4) Sleep(100);                // up to ~10s
        }
        for (auto& h : g_hookSpecs)
            Log("  %-12s %-13s slot 0x%08X  %s", h.dll, h.fn, (unsigned)h.slot,
                (*h.orig ? "hooked" : "NOT hooked"));
        Log("hooks installed: %d/4", installed);
    }

    // Content lives beside the exe so players can edit it without touching the
    // install's internals.
    char contentDir[MAX_PATH];
    _snprintf_s(contentDir, sizeof(contentDir), _TRUNCATE,
                "%s\\PEMF\\events", dir);
    content::LoadFolder(contentDir);
    triggers::Reset("startup");

    // The render-phase hook. Only attempted once the target is verified, since
    // it writes to the game's code.
    if (g_targetOK) render::Install();

    // The D3D9 render hook attaches to the game's own device from the safe
    // point, once that device exists -- nothing to do at init but announce it.
    if (g_targetOK)
        Log("d3d9: stage %d -- will hook the game's device from the safe point",
            d3d9hook::kStage);

    Log("debug hotkeys: Ctrl+Shift+1 = event #0, Ctrl+Shift+2 = event #1, "
        "Ctrl+Shift+3 = notice at the top, Ctrl+Shift+4 = notice on the ship, "
        "Ctrl+Shift+5 = probe item names 0-6, Ctrl+Shift+6 = probe item 7 "
        "(past the stock list), Ctrl+Shift+7 = toggle the data-file probe, "
        "Ctrl+Shift+8/9/0 = false colours cycle/restore/report (needs arming)");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        InitializeCriticalSection(&g_logLock);
        CloseHandle(CreateThread(nullptr, 0, Init, nullptr, 0, nullptr));
    }
    else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        render::Uninstall();
        d3d9hook::Uninstall();
        // Restore the slots only on an explicit FreeLibrary. On process teardown
        // the address space is going away and touching locks risks a hang.
        // By-address restore works whether the hook was installed by name or by
        // slot, so it covers both the GOG and Steam paths.
        for (auto& h : g_hookSpecs) UnhookSlot(h.slot, h.hook, *h.orig);
    }
    return TRUE;
}
