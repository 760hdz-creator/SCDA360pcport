#include <windows.h>
#include <runtime.h>

// Issue #4 (one host core pinned during gameplay): the title's timer/work thread loop sub_821A01A0
// (thread object Run, created from sub_821A5194) waits on a manual-reset work event that stays set,
// walks its item list, then calls Sleep(this+236) through sub_821E2070 with 0 ms. On the 360 that
// is a yield on a spare hardware thread; recompiled it spins one host core at ~90%. From that call
// site only, 0 ms becomes 1 ms (items are timer-driven: 1 ms granularity is plenty).
// SCDA_NO_WORKER_SLEEP=1 = old Sleep(0).
extern "C" PPC_FUNC(__imp__sub_821E2070);
PPC_FUNC(sub_821E2070)
{
    static const bool yieldOnly = getenv("SCDA_NO_WORKER_SLEEP") != nullptr;
    if (uint32_t(ctx.lr) == 0x821A0224 && ctx.r3.u32 == 0 && !yieldOnly)
        ctx.r3.u64 = 1;
    __imp__sub_821E2070(ctx, base);
}
