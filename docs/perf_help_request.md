# Help request: Xbox 360 GPU emulation pipeline stalls at ~25 fps

## Context

I'm working on a static recompilation of an Xbox 360 game (Splinter Cell: Double Agent, Unreal Engine 2.5) to
native Windows x64. Guest PowerPC code is recompiled to C++ with XenonRecomp. The Xenos GPU is emulated with three
host threads:

1. **Guest main thread**: the game's own code. It writes PM4 command packets into a ring buffer and indirect
   buffers.
2. **Command processor (CP) thread**: parses PM4 into a `Frame` (a draw list, a register-write log, and copies of
   the vertex/index memory). It hands a `Frame` to the renderer at the swap marker. It also hands one over as a
   "partial frame" when the guest blocks on a fence or query in the open frame.
3. **Renderer thread**: replays each `Frame` on D3D11 (one immediate context). It writes GPU fences
   (`EVENT_WRITE_SHD`) to guest memory when it reaches them, and only after earlier occlusion queries have returned
   their host results. This fence/query ordering is required for correctness: one map crashes if a fence passes a
   pending query.

## Problem

The heaviest map runs at ~25 fps instead of the game's 30 fps cap (vsync, present interval 2).
Hardware: RTX 4080 SUPER (GPU ~55% busy), 32 CPU threads.

## Measurements (heavy scene, ~9000 draws per frame)

- **Renderer** is busy ~26 ms per frame. It was 32-35 ms before three changes: rebinding only the SRV slots that
  changed, a SIMD index byte-swap, and a direct constant upload.
- **Guest main thread** spends 66% of its time spin-waiting on a GPU fence. The fence marks a ring position and is
  written by the renderer near the end of the previous frame's main chunk.
- **The serial chain:** after that fence, the guest builds the next main chunk (~8.6 ms of guest CPU). The CP then
  parses all of it in one tick (~4.3 ms) before the renderer can start. Total: a serial chain of ~31-34 ms per
  frame, right on the 33.3 ms vsync edge.
- **Frame times are bimodal:** 64% of frames take ~33 ms and 36% take ~50 ms.
- **CP thread profile:**
  - `SetGpuRegister` is 24%. Every `SET_CONSTANT` dword goes through a `std::function` callback, a guest-memory
    store, a shadow array write and a push onto a write log.
  - `memcpy` + `memcmp` are 27%: per-frame snapshots of vertex/index ranges, deduplicated by an epoch counter.
- **Renderer profile:**
  - `BindRegisterState` 12%, NVIDIA driver ~25%, register-log replay 6%.
  - Per-slot texture key hashing is ~5%: an FNV hash over 6 fetch dwords plus an `unordered_map` lookup.
  - Per draw, the renderer calls `UpdateSubresource` four times: a 1264-byte shared constant buffer, 4 KB of VS
    constants, 4 KB of PS constants, and a vertex-memory range in one big DEFAULT raw buffer.

## Already tried (no gain)

- Busy-yielding instead of `Sleep(1)` in the CP and renderer loops.
- Handing the open frame to the renderer immediately, instead of after 2 ms of CP idle time.
- Handing the open frame to the renderer every 1000 draws while the CP is still parsing. Fps stayed the same, and
  renderer ms per frame went up.
- Writing fences at CP parse time. One variant hangs at load. The other gives about +3 fps but breaks query
  ordering, so it was rejected.

## Constraints

- Fences must stay behind earlier occlusion queries.
- Textures are read from guest memory at render time. Letting the guest run further ahead risks races on reused
  texture memory.
- D3D11 only for now.

## Questions

1. What's the best way to break this serialization? For example:
   - letting the renderer start on a frame while the CP is still parsing it, without per-partial-frame overhead;
   - a safe way to write the ring-position fence earlier.
2. How do Xenia or other Xbox 360 emulators handle CP-to-renderer pipelining and `EVENT_WRITE_SHD` fences without
   stalling the guest like this?
3. What are the cheapest wins for per-draw D3D11 constant uploads at ~9000 draws per frame? For example:
   - dynamic ring buffers with `MAP_WRITE_NO_OVERWRITE` plus `VSSetConstantBuffers1` offsets;
   - uploading only the dirty constant ranges.
4. How would you bulk-handle PM4 `SET_CONSTANT` runs (thousands of dwords per frame) to cut CP cost?
5. Is there a standard fix for vsync quantization when an emulated frame lands right on the 33.3 ms edge? For
   example, decoupling the guest's flip-interval interrupt from the host present.

## Useful source files (if attached)

- `runtime/src/gpu/frame_queue.h`: frame hand-over, fences, partial frames.
- `runtime/src/command_processor.cpp`: PM4 parsing, `SetGpuRegister`, wait handling.
- `runtime/src/gpu/d3d11_renderer.cpp`: `BindRegisterState` (~line 1138) and the draw path (~line 2050).
- `runtime/src/interrupts.cpp`: vblank and the CPU-callback interrupt.
