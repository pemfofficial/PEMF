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
#pragma comment(lib, "shlwapi.lib")

#include "log.h"
#include "game.h"
#include "state.h"
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
    if (session::Tick()) triggers::Reset("career context changed");

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
    content::g_worldLive = state::InGame() && triggers::WorldOnScreen() &&
                           *(void**)game::addr::WorldLabelManager != nullptr;

    // A notice should not spend its life expiring behind a menu, so its clock
    // only runs while the overworld is actually on screen.
    content::HoldNoticeClock(content::g_worldLive);

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
    bool down = k1 || k2 || k3 || k4 || k5 || k6 || k7;

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
        "(past the stock list), Ctrl+Shift+7 = toggle the data-file probe");
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
