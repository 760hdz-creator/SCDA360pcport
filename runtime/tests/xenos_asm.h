// Tiny Xenos microcode assembler for tests (layouts: Xenia ucode.h).
#pragma once
#include <cstdint>
#include <vector>

namespace xasm
{
struct Cf { uint32_t w0 = 0, w1 = 0; };
inline Cf Exec(uint32_t addr, uint32_t count, uint32_t fetchMask = 0, bool end = false)
{
    uint32_t seq = 0;
    for (uint32_t i = 0; i < count; i++)
        seq |= ((fetchMask >> i) & 1) << (2 * i);
    return { addr | count << 12 | seq << 16, (end ? 2u : 1u) << 12 };
}
inline Cf CondExec(uint32_t addr, uint32_t count, uint32_t boolIndex, bool cond, bool end = false, uint32_t fetchMask = 0)
{
    Cf c = Exec(addr, count, fetchMask);
    c.w1 = (end ? 4u : 3u) << 12 | boolIndex << 2 | uint32_t(cond) << 10;
    return c;
}
inline Cf CondExecPred(uint32_t addr, uint32_t count, bool cond, bool end = false)
{
    Cf c = Exec(addr, count);
    c.w1 = (end ? 6u : 5u) << 12 | uint32_t(cond) << 10;
    return c;
}
inline Cf LoopStart(uint32_t skipTo, uint32_t loopId) { return { skipTo | loopId << 16, 7u << 12 }; }
inline Cf LoopEnd(uint32_t bodyStart, uint32_t loopId) { return { bodyStart | loopId << 16, 8u << 12 }; }
inline Cf Jump(uint32_t target, uint32_t boolIndex, bool cond) { return { target, 11u << 12 | boolIndex << 2 | uint32_t(cond) << 10 }; }
inline Cf Nop() { return {}; }

// Source operand: temp register or constant; swizzle given as absolute components (x=0..w=3).
struct Src
{
    bool temp = true;
    uint32_t reg = 0;
    uint8_t c[4] = { 0, 1, 2, 3 };
    bool neg = false;
    bool abs = false; // temps only (constants use the instruction-wide abs flag)
    uint32_t Swizzle() const
    {
        uint32_t s = 0;
        for (uint32_t j = 0; j < 4; j++)
            s |= ((c[j] - j) & 3) << (2 * j);
        return s;
    }
    uint32_t Reg() const { return temp ? (reg & 0x3F) | (abs ? 0x80 : 0) : reg; }
};
inline Src R(uint32_t r, uint8_t x = 0, uint8_t y = 1, uint8_t z = 2, uint8_t w = 3) { Src s; s.reg = r; s.c[0] = x, s.c[1] = y, s.c[2] = z, s.c[3] = w; return s; }
inline Src C(uint32_t r, uint8_t x = 0, uint8_t y = 1, uint8_t z = 2, uint8_t w = 3) { Src s = R(r, x, y, z, w); s.temp = false; return s; }
inline Src Neg(Src s) { s.neg = true; return s; }

struct Ins { uint32_t d[3] = {}; };
// ALU: vector op vop -> vdest.vmask, scalar op sop -> sdest.smask (sop 50 retain_prev + smask 0 = none).
inline Ins Alu(uint32_t vop, uint32_t vdest, uint32_t vmask, uint32_t sop, uint32_t sdest, uint32_t smask, Src a, Src b, Src c,
    bool exportData = false, int pred = -1)
{
    Ins i;
    i.d[0] = vdest | sdest << 8 | uint32_t(exportData) << 15 | vmask << 16 | smask << 20 | sop << 26;
    i.d[1] = c.Swizzle() | b.Swizzle() << 8 | a.Swizzle() << 16 | uint32_t(c.neg) << 24 | uint32_t(b.neg) << 25 | uint32_t(a.neg) << 26;
    if (pred >= 0)
        i.d[1] |= uint32_t(pred) << 27 | 1u << 28;
    i.d[2] = c.Reg() | b.Reg() << 8 | a.Reg() << 16 | vop << 24 | uint32_t(c.temp) << 29 | uint32_t(b.temp) << 30 | uint32_t(a.temp) << 31;
    return i;
}
// vfetch_full: dest swizzle per component 0-3 xyzw, 4 = 0, 5 = 1, 7 = keep.
inline Ins VFetch(uint32_t dst, uint32_t src, uint32_t srcComp, uint32_t slot, uint32_t format, bool isSigned, bool normalized,
    uint32_t stride, int32_t offset, uint32_t dstSwz = 0x688 /* xyzw */, bool mini = false, int32_t expAdjust = 0)
{
    Ins i;
    i.d[0] = 0 | src << 5 | dst << 12 | 1u << 19 | (slot / 3) << 20 | (slot % 3) << 25 | srcComp << 30;
    i.d[1] = dstSwz | uint32_t(isSigned) << 12 | uint32_t(!normalized) << 13 | format << 16 | (uint32_t(expAdjust) & 63) << 24 | uint32_t(mini) << 30;
    i.d[2] = stride | (uint32_t(offset) & 0x7FFFFF) << 8;
    return i;
}
inline Ins TFetch(uint32_t dst, uint32_t src, uint32_t slot, uint32_t dstSwz = 0x688)
{
    Ins i;
    i.d[0] = 1 | src << 5 | dst << 12 | slot << 20;
    i.d[1] = dstSwz;
    i.d[2] = 0;
    return i;
}
constexpr uint32_t kSwzXYZW = 0 | 1 << 3 | 2 << 6 | 3 << 9;

// CF list (padded to an even count) followed by the instructions; exec addresses are absolute slots.
inline std::vector<uint32_t> Build(std::vector<Cf> cf, const std::vector<Ins>& ins)
{
    if (cf.size() & 1)
        cf.push_back(Nop());
    std::vector<uint32_t> out;
    for (size_t i = 0; i < cf.size(); i += 2)
    {
        out.push_back(cf[i].w0);
        out.push_back((cf[i].w1 & 0xFFFF) | (cf[i + 1].w0 << 16));
        out.push_back((cf[i + 1].w0 >> 16) | (cf[i + 1].w1 << 16));
    }
    for (auto& i : ins)
        out.insert(out.end(), i.d, i.d + 3);
    return out;
}
inline uint32_t FirstSlot(size_t cfCount) { return uint32_t((cfCount + 1) / 2); }
} // namespace xasm
