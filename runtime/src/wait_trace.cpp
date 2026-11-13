#include <windows.h>
#include <runtime.h>
#include <gpu_wake.h>
#include <gpu_state.h>
#include <gpu/frame_queue.h>

// Guest GPU waits (callers of the D3D spin-wait sub_821FA428), for SCDA_TIMELINE: one 'A' event at
// return for waits over 100 us; a = function address, b = duration us.
// sub_821F33A8 waits for the GPU fence at writeback +4 (ring position), sub_821F3440 for ring space.
extern "C" PPC_FUNC(__imp__sub_821F33A8);
extern "C" PPC_FUNC(__imp__sub_821F3440);
extern "C" PPC_FUNC(__imp__sub_821F4088);
extern "C" PPC_FUNC(__imp__sub_8220A638);

static void TimedWait(PPCFunc* wait, uint32_t address, PPCContext& ctx, uint8_t* base)
{
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    wait(ctx, base);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    const uint32_t us = uint32_t((t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart);
    if (us > 100)
        TimelineEvent('A', address, us);
}

// sub_821F33A8(device, position, generation) polls *(*(device + 10768) + 4) (frame_queue.h
// g_earlyFenceAddress). 'a' event: a = position, b = generation.
PPC_FUNC(sub_821F33A8)
{
    static const bool noEarly = getenv("SCDA_NO_EARLY_FENCES") != nullptr;
    if (!noEarly && !g_earlyFenceAddress)
        g_earlyFenceAddress = GuestPhysical(PPC_LOAD_U32(ctx.r3.u32 + 10768) + 4);
    TimelineEvent('a', ctx.r4.u32, ctx.r5.u32);
    TimedWait(__imp__sub_821F33A8, 0x821F33A8, ctx, base);
}
PPC_FUNC(sub_821F3440) { TimedWait(__imp__sub_821F3440, 0x821F3440, ctx, base); }
PPC_FUNC(sub_821F4088) { TimedWait(__imp__sub_821F4088, 0x821F4088, ctx, base); }
PPC_FUNC(sub_8220A638) { TimedWait(__imp__sub_8220A638, 0x8220A638, ctx, base); }

// sub_821FA428 is one iteration of D3D's GPU spin-wait (8x db16cyc, then a 5 s hang check; r3 = 1
// keep waiting, 0 = hung). The recompiled spin pins a host core (issue #4: ~60% of the main thread),
// so after a short spin the thread sleeps until the GPU makes progress (gpu_wake.h) or 1 ms passes.
// SCDA_NO_GPU_WAIT_SLEEP=1 = old pure spin.
extern "C" PPC_FUNC(__imp__sub_821FA428);
PPC_FUNC(sub_821FA428)
{
    static const bool spinOnly = getenv("SCDA_NO_GPU_WAIT_SLEEP") != nullptr;
    thread_local uint32_t spins;
    thread_local LONGLONG lastCall;
    const uint32_t seen = GpuProgressValue();
    __imp__sub_821FA428(ctx, base);
    if (spinOnly || ctx.r3.u32 == 0)
        return;
    static const LONGLONG newWaitGap = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart / 10000; }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (now.QuadPart - lastCall > newWaitGap) // > 100 us since this thread's last iteration: a new wait
        spins = 0;
    if (++spins > 32)
        GpuProgressWait(seen, 1);
    else
        YieldProcessor();
    QueryPerformanceCounter(&now);
    lastCall = now.QuadPart;
}
