// morale.h - measuring what the engine's morale actually responds to.
//
// ⚠️ THIS IS INSTRUMENTATION, NOT A SYSTEM. It answers one question that the
// whole extended-morale design depends on, and which cannot be answered from
// the disassembly alone.
//
// `GetMoraleLevel` (0x00404810) derives a 0-4 level rather than reading one:
//
//     expect = ((A - 4 + B)^2 / 4) - 4 * [0x869B27]     clamped 1..999
//     level  = ((plunder + 500) / (crew term)) / expect  clamped 0..4
//
// where A = [0x00869A76] and B = [0x0085A158].
//
// `0x00869B27` is the only term PEMF can move: it has exactly ONE cross
// reference in the executable -- the read inside GetMoraleLevel -- so no engine
// code writes it, and PEMF owns it for the session.
//
// THE UNKNOWN is how much authority that gives. The byte shifts `expect` by at
// most +/-508, against a base of `(A - 4 + B)^2 / 4` whose magnitude has never
// been seen in a running game. If the base is small the byte is a complete
// lever and morale can be driven anywhere; if it is large the byte is a trim.
// The design differs in each case, so this measures it instead of guessing.
//
// The sweep walks the byte across its range and records the level the engine
// returns at each step. Whatever the answer, it is then a fact rather than an
// assumption, and the byte is restored afterwards.
#pragma once
#include <windows.h>

#include "log.h"
#include "game.h"
#include "state.h"

namespace morale {

// The formula's inputs. Only MoraleByte is in game.h already -- the rest are
// here because nothing but this probe reads them yet.
constexpr uintptr_t kTermA = 0x00869A76;   // int8?  role unknown
constexpr uintptr_t kTermB = 0x0085A158;   // int32, also read by the town menu
constexpr uintptr_t kFlags = 0x00869B34;   // bit 7 shifts the crew divisor by 19

inline signed char& Byte() { return *(signed char*)game::addr::MoraleByte; }

// Recompute the engine's own arithmetic from the same inputs, so the log can
// show whether we have read the formula correctly. If PREDICTED and ACTUAL ever
// disagree, the reading is wrong and nothing built on it can be trusted.
inline int Predict(int* expectOut)
{
    const int a       = *(const signed char*)kTermA;
    const int b       = *(const int*)kTermB;
    const int crew    = game::CrewCount();
    const int plunder = game::UndividedPlunder();
    const int flags   = *(const unsigned char*)kFlags;

    int expect = a - 4 + b;
    expect = expect * expect;
    expect = (expect / 4) - 4 * (int)Byte();
    if (expect < 1)   expect = 1;
    if (expect > 999) expect = 999;
    if (expectOut) *expectOut = expect;

    const int divisor = 0x14 + crew - ((flags & 0x80) ? 19 : 0);
    if (divisor == 0) return -1;

    int level = ((plunder + 500) / divisor) / expect;
    if (level < 0) return 0;
    if (level > 4) level = 4;
    return level;
}

inline void LogState(const char* when)
{
    if (!state::InGame()) {
        Log("morale[%s]: no career loaded", when);
        return;
    }
    __try {
        int expect = 0;
        const int predicted = Predict(&expect);
        const int actual    = game::GetMoraleLevel();

        Log("morale[%s]: level=%d  byte=%d  expect=%d  plunder=%d  crew=%d",
            when, actual, (int)Byte(), expect,
            game::UndividedPlunder(), game::CrewCount());
        Log("morale[%s]:   A(0x869A76)=%d  B(0x85A158)=%d  flags(0x869B34)=0x%02X",
            when, (int)*(const signed char*)kTermA, *(const int*)kTermB,
            *(const unsigned char*)kFlags);
        Log("morale[%s]:   predicted=%d actual=%d %s", when, predicted, actual,
            predicted == actual ? "-- formula agrees"
                                : "** DISAGREES -- the formula is misread **");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("!! morale[%s]: fault reading the inputs (0x%08X)",
            when, GetExceptionCode());
    }
}

// Walk the byte across its range and record what the engine returns. This is
// the measurement the design is waiting on.
//
// The byte is restored at the end, including on the fault path -- a probe that
// left a career's morale pinned would be worse than no probe.
inline void Sweep()
{
    if (!state::InGame()) {
        Log("morale sweep: no career loaded -- load a save and try again");
        return;
    }

    const signed char saved = Byte();
    Log("morale sweep: BEGIN (byte currently %d, restored at the end)",
        (int)saved);
    LogState("before");

    __try {
        static const int steps[] = { -128, -96, -64, -32, -16, -8, -4, -2, -1,
                                     0, 1, 2, 4, 8, 16, 32, 64, 96, 127 };
        int lowest = 9, highest = -1;

        for (int i = 0; i < (int)(sizeof(steps) / sizeof(steps[0])); ++i) {
            Byte() = (signed char)steps[i];
            int expect = 0;
            Predict(&expect);
            const int level = game::GetMoraleLevel();
            if (level < lowest)  lowest = level;
            if (level > highest) highest = level;
            Log("morale sweep:   byte=%4d -> expect=%4d  level=%d",
                steps[i], expect, level);
        }

        Log("morale sweep: RESULT -- the byte moves morale between %d and %d",
            lowest, highest);
        if (lowest == 0 && highest == 4)
            Log("morale sweep: FULL AUTHORITY -- the closed loop can be exact.");
        else if (lowest == highest)
            Log("morale sweep: NO AUTHORITY at this wealth/crew -- the byte "
                "cannot move morale here. Try again with different plunder.");
        else
            Log("morale sweep: PARTIAL -- PEMF's scale can drive the engine "
                "only within %d..%d at this wealth and crew.", lowest, highest);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("!! morale sweep: fault (0x%08X)", GetExceptionCode());
    }

    Byte() = saved;
    Log("morale sweep: END (byte restored to %d)", (int)saved);
    LogState("after");
}

} // namespace morale
