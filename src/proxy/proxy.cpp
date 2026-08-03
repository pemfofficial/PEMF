// proxy.cpp - version.dll shim that loads pemf_core.dll
//
// This DLL takes over the module name "version.dll" for the entire process, so
// it must export *everything* the real one does -- not merely the three
// functions Pirates!.exe itself imports. d3d9.dll (loaded later, dynamically)
// imports GetFileVersionInfoW, and any missing export is a hard load failure
// with an "Entry Point Not Found" dialog.
//
// The export list is generated from the real system DLL by gen_proxy.py into
// generated.inc, so it tracks whatever Windows actually ships.
//
// Each stub is a naked tail-call: it jumps to the real function with the stack
// frame untouched, which makes it correct for every calling convention and
// signature without us having to know any of them.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "pemf_version.h"

// ---------------------------------------------------------- forward pointers
#define PROXY_EXPORT(name) extern "C" void* g_p_##name = nullptr;
#include "generated.inc"
#undef PROXY_EXPORT

namespace {

struct Entry { const char* name; void** slot; };

Entry g_table[] = {
#define PROXY_EXPORT(name) { #name, &g_p_##name },
#include "generated.inc"
#undef PROXY_EXPORT
};

HMODULE   g_real = nullptr;
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK DoResolve(PINIT_ONCE, PVOID, PVOID*)
{
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16) return FALSE;
    strcat_s(path, sizeof(path), "\\version.dll");

    // Absolute path: loading plain "version.dll" would find this file again.
    g_real = LoadLibraryA(path);
    if (!g_real) {
        MessageBoxA(nullptr, path, "Pirates! mod: cannot load real version.dll",
                    MB_OK | MB_ICONERROR);
        return FALSE;
    }
    for (auto& e : g_table) {
        *e.slot = (void*)GetProcAddress(g_real, e.name);
    }
    return TRUE;
}

} // namespace

// Called from the naked stubs on first use. Deliberately not done in DllMain:
// that would mean calling LoadLibrary under the loader lock.
extern "C" void __cdecl ProxyResolve()
{
    InitOnceExecuteOnce(&g_once, DoResolve, nullptr, nullptr);
}

// -------------------------------------------------------------- the stubs
// mov eax, <slot>; if null -> resolve, reload; jmp eax.
// ProxyResolve is __cdecl and takes no arguments, so it preserves ebx/esi/edi/
// ebp and only clobbers eax/ecx/edx -- all scratch for the conventions used by
// version.dll's exports.
#define PROXY_EXPORT(name)                              \
    extern "C" __declspec(naked) void Proxy_##name()    \
    {                                                   \
        __asm { mov  eax, g_p_##name }                  \
        __asm { test eax, eax }                         \
        __asm { jnz  ready }                            \
        __asm { call ProxyResolve }                     \
        __asm { mov  eax, g_p_##name }                  \
        __asm { ready: }                                \
        __asm { jmp  eax }                              \
    }
#include "generated.inc"
#undef PROXY_EXPORT

// ------------------------------------------------------------- proxy logging
// Independent of the core's log, so we still get diagnostics if the core never
// loads (e.g. a DRM-packed host interfering). Written next to the executable.
static void ProxyLog(const char* fmt, ...)
{
    char dir[MAX_PATH]{};
    GetModuleFileNameA(GetModuleHandleA(NULL), dir, MAX_PATH);
    if (char* slash = strrchr(dir, '\\')) *slash = 0;
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\pemf_proxy.log", dir);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") != 0 || !f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ---------------------------------------------------------------- core load
static DWORD WINAPI LoadCore(LPVOID)
{
    char path[MAX_PATH]{};
    GetModuleFileNameA(GetModuleHandleA(NULL), path, MAX_PATH);
    if (char* slash = strrchr(path, '\\')) *slash = 0;
    strcat_s(path, sizeof(path), "\\pemf_core.dll");

    // The version leads the proxy log for the same reason it leads the core's:
    // a half-updated install reads as a broken release. A player who extracts a
    // new zip into a subfolder by accident -- which is what Explorer's "Extract
    // All" offers to do by default -- keeps running the old DLLs while being
    // certain they are on the new ones, and nothing on screen disagrees.
    // Two logs, each naming its own version, settle that in one glance.
    ProxyLog("proxy: PEMF %s attempting to load core: %s", PEMF_VER_S, path);

    // Hand our version to the core before it loads, so it can say plainly in
    // pemf.log whether the two halves of the install match. Cheaper and safer
    // than the core reading this file's version resource -- that would make it
    // import from version.dll, which in this process is us.
    SetEnvironmentVariableA("PEMF_PROXY_VERSION", PEMF_VER_S);
    HMODULE core = LoadLibraryA(path);
    if (core) {
        ProxyLog("proxy: core loaded OK at %p", (void*)core);
    } else {
        DWORD err = GetLastError();
        ProxyLog("proxy: core FAILED to load, GetLastError=%lu", err);
        char msg[MAX_PATH + 96];
        wsprintfA(msg, "Failed to load:\n%s\n\nGetLastError = %lu", path, err);
        MessageBoxA(nullptr, msg, "Pirates! mod", MB_OK | MB_ICONERROR);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        CloseHandle(CreateThread(nullptr, 0, LoadCore, nullptr, 0, nullptr));
    }
    return TRUE;
}
