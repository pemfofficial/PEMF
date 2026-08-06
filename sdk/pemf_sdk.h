// pemf_sdk.h - write a Pirates! mod in code.
//
// Copy this one header into your project. There is nothing to link and nothing
// to build against: PEMF hands your plugin a table of function pointers, and
// that table is a plain C struct, so any compiler that can produce a 32-bit
// Windows DLL will do.
//
// ---------------------------------------------------------------- the shape
//
//     #include "pemf_sdk.h"
//
//     static const PemfApi* g_pemf;
//
//     static void OnPicked(void* user) {
//         g_pemf->log("the player asked at the harbour");
//         g_pemf->add_plunder(-50, "my_mod");
//     }
//
//     PEMF_PLUGIN_EXPORT int PemfPluginInit(const PemfApi* api, PemfPlugin* me)
//     {
//         g_pemf = api;
//         me->name    = "My Mod";
//         me->version = "1.0";
//         api->add_menu_row("Ask at the harbour", PEMF_ANY, PEMF_ANY,
//                           OnPicked, 0);
//         return PEMF_OK;
//     }
//
// Build it as a 32-bit DLL and drop it in `PEMF\plugins\`. That is the whole
// installation step.
//
// ------------------------------------------------------------------ the rules
//
//  * 32-BIT. The game is x86. A 64-bit DLL will be refused with a clear line in
//    pemf.log rather than failing mysteriously.
//
//  * ONE THREAD. Everything PEMF calls you on runs on the game's own thread, at
//    a point where the game is in a known state. Do not call back into PEMF
//    from a thread of your own -- the engine has no locking and will not
//    survive it.
//
//  * CALL UI ONLY FROM A CALLBACK. show_card() and friends present a modal, and
//    that is only safe from inside a callback PEMF invoked. From PemfPluginInit
//    the game is not yet running and there is nothing to draw onto.
//
//  * STATE GOES THROUGH PEMF. add_crew/add_plunder are clamped, career-gated
//    and logged with the reason you give. Write the game's memory yourself and
//    you own whatever happens next.
//
//  * A FAULT IN YOUR PLUGIN IS CONTAINED. PEMF calls you inside a guard; if you
//    fault, your plugin is disabled for the session and the game carries on.
//    You will see it in pemf.log with your plugin's name on it.
//
// -------------------------------------------------------------- versioning
// `PEMF_ABI_VERSION` changes only when the meaning of something already in this
// header changes. New calls are added to the END of PemfApi and the struct
// carries its own size, so a plugin built against an older SDK keeps working:
// check `api->size` before using anything added after the version you targeted.
#ifndef PEMF_SDK_H
#define PEMF_SDK_H

#ifdef __cplusplus
extern "C" {
#endif

#define PEMF_ABI_VERSION 1

#define PEMF_PLUGIN_EXPORT __declspec(dllexport)

// Return values from PemfPluginInit.
#define PEMF_OK               0
#define PEMF_ERR_ABI          1   // "I cannot work with this PEMF"
#define PEMF_ERR_FAILED       2   // anything else; say why with api->log

// "anywhere" / "any nation" for menu-row placement.
#define PEMF_ANY (-1)

// Crew morale, as the game models it.
#define PEMF_MORALE_MUTINOUS  0
#define PEMF_MORALE_ANGRY     1
#define PEMF_MORALE_UNHAPPY   2
#define PEMF_MORALE_CONTENT   3
#define PEMF_MORALE_DEVOTED   4

// city_nation() returns one of these. The same byte carries both the owning
// nation and the two kinds of settlement that have no owner, which is the
// engine's own arrangement rather than a simplification here.
#define PEMF_NATION_MAX       4   // 0..4 are the five nations
#define PEMF_KIND_NATIVE      5   // an Indian village
#define PEMF_KIND_MISSION     6   // a Jesuit mission

typedef void (*PemfRowFn)(void* user);

// What your plugin tells PEMF about itself. Fill these in during init; the
// strings must outlive the call (a literal is ideal).
typedef struct PemfPlugin {
    const char* name;       // shown in the log. Required.
    const char* version;    // your version, not PEMF's. Optional.
    void*       user;       // yours; PEMF only hands it back to you.
} PemfPlugin;

// The table of everything a plugin may do. Do not construct one of these --
// PEMF passes you a pointer to its own.
typedef struct PemfApi {
    // Bytes in this struct. Check it before calling anything added after the
    // ABI version you built against.
    unsigned int size;
    unsigned int abi_version;      // == PEMF_ABI_VERSION at the host's build

    // --------------------------------------------------------------- logging
    // Goes to pemf.log, prefixed with your plugin's name. This is how you
    // debug: there is no console.
    void (*log)(const char* message);

    // ----------------------------------------------------------------- state
    // Reads are cheap and always safe. They return 0 when no career is loaded,
    // so check in_game() first if zero would be ambiguous for you.
    int (*in_game)(void);
    int (*get_crew)(void);
    int (*get_morale)(void);       // PEMF_MORALE_*
    int (*get_plunder)(void);      // undivided plunder, not total wealth
    int (*get_months)(void);       // months at sea this voyage
    int (*get_nation)(void);       // the crown you serve, or -1 outside a career

    // Writes are clamped to what the game can hold, refused outside a career,
    // and logged with `reason` so a change can always be traced back to you.
    // `reason` should be short and yours, e.g. "my_mod". Returns the value
    // actually applied, which may differ from what you asked for.
    int (*add_crew)(int delta, const char* reason);
    int (*add_plunder)(int delta, const char* reason);
    int (*set_crew)(int value, const char* reason);
    int (*set_plunder)(int value, const char* reason);

    // ----------------------------------------------------------------- world
    // The settlement nearest the ship within `radius`, or -1. In port that is
    // the port you are standing in.
    int (*nearest_city)(int radius);
    // Who holds it -- 0..PEMF_NATION_MAX -- or PEMF_KIND_NATIVE /
    // PEMF_KIND_MISSION for the two that nobody holds.
    int (*city_nation)(int city);
    // Village, town, city -- the size, which is a different thing entirely
    // from the value above. This is what @LOCTYPE renders.
    int (*city_type)(int city);
    // Are two nations at war? Reads the engine's own relations matrix.
    int (*nations_at_war)(int a, int b);

    // ------------------------------------------------------------------- ui
    // ⚠️ Only from inside a callback PEMF gave you.
    //
    // A card with up to 6 selectable lines. Returns the index picked, or -1 if
    // dismissed. With `count` 0 it is a plain acknowledge-and-continue card.
    // In a port it is drawn against that port; at sea, over the world.
    int (*show_card)(const char* body, const char* const* options, int count);

    // A line of on-screen text while sailing. Never interrupts. `seconds` is
    // 1..30. `anchor_to_ship` hangs it over the player's vessel and follows it.
    void (*post_notice)(const char* text, int seconds, int anchor_to_ship);

    // ----------------------------------------------------------------- menus
    // Add a row to the game's town menu. It appears just above the row that
    // leaves the settlement, so every one of the game's own options keeps its
    // place. `port` and `nation` limit where it is offered; PEMF_ANY for
    // everywhere. Your callback runs when the player picks it, with the town
    // still on screen, and the menu returns afterwards.
    //
    // Returns 1 if the row was added. Rows are limited; a refusal is logged.
    int (*add_menu_row)(const char* label, int port, int nation,
                        PemfRowFn fn, void* user);

    // Fire an event that exists in JSON, by its id. Returns 1 if it was found.
    // Presents immediately when called from a menu callback.
    int (*fire_event)(const char* event_id);
} PemfApi;

// Your entry point. PEMF looks for this exact name.
//
// Return PEMF_OK to stay loaded. Anything else and PEMF unloads you, with the
// reason in the log -- return PEMF_ERR_ABI if `api->abi_version` is not one you
// can work with.
typedef int (*PemfPluginInitFn)(const PemfApi* api, PemfPlugin* me);

#ifdef __cplusplus
}
#endif
#endif // PEMF_SDK_H
