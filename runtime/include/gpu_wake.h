#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>

// Lets guest threads that spin on GPU progress (D3D's sub_821FA428 wait loop) sleep instead of
// burning a host core. Every producer of guest-visible GPU progress (CP rptr writeback, fence /
// memory writes, scratch registers, vsync interrupts, occlusion answers) calls GpuProgressSignal;
// a waiter reads GpuProgressValue before checking its condition and, if it must keep waiting,
// blocks in GpuProgressWait until the value changes (or the timeout, a safety net for producers
// this does not cover).
inline std::atomic<uint32_t>& GpuProgressCounter()
{
    static std::atomic<uint32_t> counter{ 0 };
    return counter;
}

inline uint32_t GpuProgressValue() { return GpuProgressCounter().load(std::memory_order_acquire); }

inline void GpuProgressSignal()
{
    GpuProgressCounter().fetch_add(1, std::memory_order_release);
    WakeByAddressAll(&GpuProgressCounter());
}

inline void GpuProgressWait(uint32_t seen, DWORD timeoutMs)
{
    WaitOnAddress(&GpuProgressCounter(), &seen, sizeof(seen), timeoutMs);
}
