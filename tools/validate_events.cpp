// validate_events.cpp - load event files with PEMF's own loader, outside the
// game, and report what it accepts and rejects. The rules are the loader's,
// not a copy of them, so a file that passes here loads in game.
//
//   tools\validate_events.ps1 [folder]      (default: content\PEMF\events)
//
// Only parsing runs. Nothing here touches game memory.
#include <stdio.h>
#include <stdarg.h>

void Log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

// The same order core.cpp includes them in; content.h relies on it.
#include "game.h"
#include "state.h"
#include "nations.h"
#include "standing.h"
#include "session.h"
#include "events.h"
#include "content.h"

// Referenced by headers the loader pulls in (the render and Direct3D hooks);
// defined in core.cpp and widescreen.h in the real build, never called by parsing.
extern "C" {
void __cdecl PemfAfterSailingRender(void) {}
void __cdecl PemfOnBeginScene(void*) {}
void __cdecl PemfOnEndScene(void*) {}
void __cdecl PemfWidescreenPassBegin(void) {}
int  __cdecl PemfWidescreenOnDrawIndexed(void*, INT, UINT) { return 0; }
void __cdecl PemfWidescreenAfterDraw(void*) {}
void __cdecl PemfWidescreenPassEnd(void*) {}
void __cdecl PemfWidescreenOnPresent(void*) {}
void __cdecl PemfWidescreenBeforeReset(void) {}
int  __cdecl PemfWidescreenTracing(void) { return 0; }
void __cdecl PemfWidescreenTraceDraw(void*, UINT, INT, UINT, UINT, UINT, UINT) {}
void __cdecl PemfWidescreenTraceDrawUP(void*, const char*, UINT, UINT, UINT, const void*, UINT) {}
}

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : "content\\PEMF\\events";
    const int n = content::LoadFolder(dir);
    printf("\n%d event(s) loaded from %s\n", n, dir);
    for (int i = 0; i < content::Count(); ++i) {
        const content::Event* ev = content::Get(i);
        if (ev) printf("  [%2d] %s\n", i, ev->id.c_str());
    }
    // Menus: every option must lead somewhere (a menu, an event, or a card).
    // Links are resolved after loading, as the game does (ResolveMenus).
    content::ResolveMenus();
    int menus = 0;
    for (const content::MenuDef* m; (m = content::GetMenu(menus)) != nullptr; ++menus) {
        int bad = 0;
        for (const content::MenuOption& opt : m->options)
            if (opt.menuIndex < 0 && opt.eventIndex < 0 && opt.outcome.empty()) ++bad;
        printf("  menu '%s': %d option(s)%s\n", m->id.c_str(), (int)m->options.size(),
               bad ? "  ** some go nowhere **" : "");
    }
    printf("%d menu(s)\n", menus);
    return 0;
}
