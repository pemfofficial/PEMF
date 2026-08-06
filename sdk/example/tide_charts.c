// tide_charts.c - a complete PEMF plugin, in one file.
//
// It adds a row to the town menu of every English port. Picking it offers to
// sell the captain a set of tide charts; buying costs plunder and says so.
//
// Nothing here is a sketch -- this is the whole plugin. Build it and drop the
// DLL in `PEMF\plugins\`.
//
//   BUILD (from a "x86 Native Tools" command prompt):
//       cl /nologo /LD /O2 tide_charts.c /I.. /Fe:tide_charts.dll
//
//   INSTALL:
//       copy tide_charts.dll "<game folder>\PEMF\plugins\"
//
// Then look in pemf.log for:
//       plugins: loaded 'Tide Charts' 1.0  (tide_charts.dll)
#include "../pemf_sdk.h"

static const PemfApi* g_pemf;

// Kept across calls so we can tell whether this captain already bought a set.
// A plugin's own state is its own business -- PEMF does not persist it.
static int g_bought;

#define CHART_PRICE 300

static void OnAskAboutCharts(void* user)
{
    const char* options[2];
    int picked;

    (void)user;

    if (g_bought) {
        g_pemf->show_card("You already have this season's charts aboard.",
                          0, 0);
        return;
    }

    if (g_pemf->get_plunder() < CHART_PRICE) {
        g_pemf->show_card(
            "The chartmaker looks you over, and then at your purse, and "
            "returns to his work without a word.", 0, 0);
        return;
    }

    options[0] = "Pay for the charts.";
    options[1] = "Not at that price.";

    picked = g_pemf->show_card(
        "A chartmaker keeps a stall by the water, selling tide tables for "
        "these waters. He wants three hundred pieces for the season.",
        options, 2);

    if (picked != 0) return;    // declined, or dismissed with -1

    // Through PEMF, so it is clamped, refused outside a career, and logged
    // with our reason next to it.
    g_pemf->add_plunder(-CHART_PRICE, "tide_charts");
    g_bought = 1;

    g_pemf->show_card("The charts are rolled, tied, and carried aboard.",
                      0, 0);
    g_pemf->log("sold the captain a set of charts");
}

PEMF_PLUGIN_EXPORT int PemfPluginInit(const PemfApi* api, PemfPlugin* me)
{
    if (!api) return PEMF_ERR_FAILED;

    // Refuse politely rather than crash if PEMF ever changes underneath us.
    if (api->abi_version != PEMF_ABI_VERSION) return PEMF_ERR_ABI;

    g_pemf = api;

    me->name    = "Tide Charts";
    me->version = "1.0";

    // English ports only. PEMF_ANY for the port means "any English one".
    if (!api->add_menu_row("Ask about tide charts", PEMF_ANY, /*English*/ 1,
                           OnAskAboutCharts, 0)) {
        api->log("could not add my menu row");
        return PEMF_ERR_FAILED;
    }

    return PEMF_OK;
}
