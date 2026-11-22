// Reference Xenos microcode interpreter. See xenos_interp.h. Bitfield layouts: Xenia ucode.h.
#include "xenos_interp.h"
#include <cfloat>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>

bool XenosInterpVertexFormatSupported(uint32_t f) { return XenosInterpVertexFormatComponents(f) != 0; }
uint32_t XenosInterpVertexFormatComponents(uint32_t f)
{
    switch (f)
    {
    case 33: case 36: return 1;
    case 25: case 31: case 34: case 37: return 2;
    case 16: case 17: case 57: return 3;
    case 6: case 7: case 26: case 32: case 35: case 38: return 4;
    default: return 0;
    }
}

namespace
{
uint32_t Bits(uint32_t v, uint32_t lo, uint32_t n) { return (v >> lo) & ((1u << n) - 1u); }
int32_t SBits(uint32_t v, uint32_t lo, uint32_t n) { return int32_t(v << (32 - lo - n)) >> (32 - n); }

float AsFloat(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
uint32_t AsUint(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
float Flush(float f) { uint32_t u = AsUint(f); return (u & 0x7F800000u) ? f : AsFloat(u & 0x80000000u); }
bool Finite(float f) { return std::isfinite(f); }
float Sat(float f) { return f > 0.0f ? (f < 1.0f ? f : 1.0f) : 0.0f; } // NaN -> 0 (D3D saturate)
// D3D9 / Xenos multiply: +-0 (or denormal) times anything is +0.
float Mul(float a, float b) { return (a != 0.0f && b != 0.0f) ? a * b : 0.0f; }
uint32_t Swz(uint32_t swizzle, uint32_t c) { return ((swizzle >> (2 * c)) + c) & 3; } // ALU source swizzle

// Xenia ucode.cc: components each vector operand reads (0 = operand unused).
const uint8_t kVecOperands[32][3] = {
    { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 },
    { 15, 0, 0 }, { 15, 0, 0 }, { 15, 0, 0 }, { 15, 15, 15 }, { 15, 15, 15 }, { 15, 15, 15 }, { 15, 15, 15 }, { 15, 15, 0 },
    { 7, 7, 0 }, { 3, 3, 1 }, { 15, 15, 0 }, { 15, 0, 0 }, { 9, 9, 0 }, { 9, 9, 0 }, { 9, 9, 0 }, { 9, 9, 0 },
    { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 }, { 15, 15, 0 }, { 6, 10, 0 }, { 15, 15, 0 }, { 0, 0, 0 }, { 0, 0, 0 },
};
// Scalar opcodes: operand form (0 none, 1 = src3.a, 2 = src3.ab, 3 = c#.w + r#.x), -1 = invalid.
const int8_t kScalarForm[64] = {
    2, 1, 2, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1,
    1, 0, 1, 1, 1, 1, 1, 1, 1, -1, 3, 3, 3, 3, 3, 3, 1, 1, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

// Xenos 16-bit float: no Inf/NaN (exponent 31 is a normal number), denormals flushed (Xenia math.h).
float XenosHalf(uint32_t h)
{
    uint32_t m = h & 0x3FF, e = (h >> 10) & 0x1F;
    if (!e)
        return AsFloat((h & 0x8000u) << 16);
    return AsFloat(((h & 0x8000u) << 16) | ((e + 112) << 23) | (m << 13));
}

uint32_t Swap(uint32_t v, uint32_t endian)
{
    switch (endian)
    {
    case 1: return ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u);
    case 2: return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    case 3: return (v >> 16) | (v << 16);
    default: return v;
    }
}

struct Machine
{
    const XenosInterpInput& in;
    XenosInterpOutput& out;
    float r[128][4] = {};
    float ps = 0.0f;
    bool p0 = false;
    int32_t a0 = 0;
    struct Loop { uint32_t constant, iterator; };
    Loop loops[4] = {};
    uint32_t loopDepth = 0;
    uint32_t calls[4] = {};
    uint32_t callDepth = 0;
    uint32_t vfetchSlot = 0, vfetchAddress = 0; // last vfetch_full (mini fetches reuse them)

    Machine(const XenosInterpInput& i, XenosInterpOutput& o) : in(i), out(o) {}

    bool Fail(const std::string& why)
    {
        if (out.skip.empty())
            out.skip = why;
        return false;
    }

    int32_t AL() const
    {
        if (!loopDepth)
            return 0;
        const Loop& l = loops[loopDepth - 1];
        int32_t v = int32_t(l.iterator) * SBits(l.constant, 16, 8) + int32_t(Bits(l.constant, 8, 8));
        return v < -256 ? -256 : v > 256 ? 256 : v;
    }
    bool Bool(uint32_t index) const { return (in.bools[index >> 5] >> (index & 31)) & 1; }
    float* Temp(uint32_t index, bool relative) { return r[(int32_t(index) + (relative ? AL() : 0)) & 127]; }
    void Const(uint32_t index, bool addressed, bool useA0, float o[4])
    {
        int32_t i = int32_t(index) + (addressed ? (useA0 ? a0 : AL()) : 0);
        if (i < 0 || i > 255)
            out.special = true; // out-of-range relative constant read: hosts disagree (D3D11 cbuffer OOB)
        for (int c = 0; c < 4; c++)
            o[c] = (i >= 0 && i < 256) ? in.consts[i][c] : 0.0f;
    }

    void Alu(const uint32_t* d, uint32_t slot);
    void Log(const char* fmt, ...);
    bool Fetch(const uint32_t* d);
    bool VertexFetch(const uint32_t* d);
    bool Run();
};

void Machine::Log(const char* fmt, ...)
{
    if (!in.trace)
        return;
    char buf[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof buf, fmt, a);
    va_end(a);
    *in.trace += buf;
}

void Machine::Alu(const uint32_t* d, uint32_t slot)
{
    const uint32_t d0 = d[0], d1 = d[1], d2 = d[2];
    const uint32_t vdest = Bits(d0, 0, 6), sdest = Bits(d0, 8, 6);
    const bool vdestRel = Bits(d0, 6, 1), absConst = Bits(d0, 7, 1), sdestRel = Bits(d0, 14, 1);
    const bool isExport = Bits(d0, 15, 1), vclamp = Bits(d0, 24, 1), sclamp = Bits(d0, 25, 1);
    const uint32_t vmaskRaw = Bits(d0, 16, 4), smaskRaw = Bits(d0, 20, 4), sop = Bits(d0, 26, 6);
    const uint32_t vop = Bits(d2, 24, 5);
    const bool constRelA0 = Bits(d1, 29, 1), const1Rel = Bits(d1, 30, 1), const0Rel = Bits(d1, 31, 1);
    auto srcReg = [&](int i) { return Bits(d2, 16 - 8 * (i - 1), 8); };     // i = 1..3
    auto srcSwz = [&](int i) { return Bits(d1, 16 - 8 * (i - 1), 8); };
    auto srcNeg = [&](int i) { return bool(Bits(d1, 26 - (i - 1), 1)); };
    auto srcTemp = [&](int i) { return bool(Bits(d2, 31 - (i - 1), 1)); };
    // Which of const_0_rel_abs / const_1_rel_abs applies: whether an earlier operand was a constant.
    auto constAddressed = [&](int i) {
        if (i == 1)
            return const0Rel;
        if (i == 2)
            return srcTemp(1) ? const0Rel : const1Rel;
        return (srcTemp(1) && srcTemp(2)) ? const0Rel : const1Rel;
    };
    // Full operand with swizzle, abs, negate (denormals flushed).
    auto load = [&](int i, float o[4]) {
        float raw[4];
        bool absv;
        const uint32_t reg = srcReg(i);
        if (srcTemp(i))
        {
            memcpy(raw, Temp(reg & 0x3F, reg & 0x40), sizeof raw);
            absv = reg & 0x80;
        }
        else
        {
            Const(reg, constAddressed(i), constRelA0, raw);
            absv = absConst;
        }
        for (uint32_t c = 0; c < 4; c++)
        {
            float v = Flush(raw[Swz(srcSwz(i), c)]);
            if (absv)
                v = std::fabs(v);
            o[c] = srcNeg(i) ? -v : v;
        }
    };

    const uint32_t vecOperandCount = kVecOperands[vop][2] ? 3 : kVecOperands[vop][1] ? 2 : kVecOperands[vop][0] ? 1 : 0;
    const uint32_t vmask = isExport ? (vmaskRaw & ~smaskRaw) : vmaskRaw;
    const uint32_t smask = isExport ? (smaskRaw & ~vmaskRaw) : smaskRaw;
    const bool vopChangesState = (vop >= 20 && vop <= 27) || vop == 29;

    // Every source is read before anything is written (vector and scalar issue together).
    float vs[3][4] = {};
    for (uint32_t i = 0; i < vecOperandCount; i++)
        load(int(i + 1), vs[i]);
    float sa = 0.0f, sb = 0.0f;
    const int form = kScalarForm[sop];
    if (form == 1 || form == 2)
    {
        float t[4];
        load(3, t); // swizzle already applied per destination component
        sa = t[3];
        sb = t[vecOperandCount == 3 ? 2 : 0];
    }
    else if (form == 3)
    {
        float c[4], *t = Temp(Bits(sop, 0, 1) | (Bits(d2, 29, 1) << 1) | (srcSwz(3) & 0x3C), false);
        Const(srcReg(3), constAddressed(3), constRelA0, c);
        sa = Flush(c[Swz(srcSwz(3), 3)]);
        sb = Flush(t[Swz(srcSwz(3), 0)]);
        if (absConst)
            sa = std::fabs(sa), sb = std::fabs(sb);
        if (srcNeg(3))
            sa = -sa, sb = -sb;
    }

    float v[4] = {};
    bool vfinite = true;
    for (uint32_t i = 0; i < vecOperandCount; i++)
        for (int c = 0; c < 4; c++)
            vfinite &= Finite(vs[i][c]);
    if (vmask || vopChangesState)
    {
        const float(&a)[4] = vs[0], (&b)[4] = vs[1], (&c)[4] = vs[2];
        bool rep = false;
        switch (vop)
        {
        case 0: for (int i = 0; i < 4; i++) v[i] = a[i] + b[i]; break;
        case 1: for (int i = 0; i < 4; i++) v[i] = Mul(a[i], b[i]); break;
        case 2: for (int i = 0; i < 4; i++) v[i] = a[i] >= b[i] ? a[i] : b[i]; break;
        case 3: for (int i = 0; i < 4; i++) v[i] = a[i] < b[i] ? a[i] : b[i]; break;
        case 4: for (int i = 0; i < 4; i++) v[i] = a[i] == b[i] ? 1.0f : 0.0f; break;
        case 5: for (int i = 0; i < 4; i++) v[i] = a[i] > b[i] ? 1.0f : 0.0f; break;
        case 6: for (int i = 0; i < 4; i++) v[i] = a[i] >= b[i] ? 1.0f : 0.0f; break;
        case 7: for (int i = 0; i < 4; i++) v[i] = a[i] != b[i] ? 1.0f : 0.0f; break;
        case 8: for (int i = 0; i < 4; i++) v[i] = a[i] - std::floor(a[i]); break;
        case 9: for (int i = 0; i < 4; i++) v[i] = std::trunc(a[i]); break;
        case 10: for (int i = 0; i < 4; i++) v[i] = std::floor(a[i]); break;
        case 11: for (int i = 0; i < 4; i++) v[i] = Mul(a[i], b[i]) + c[i]; break;
        case 12: for (int i = 0; i < 4; i++) v[i] = a[i] == 0.0f ? b[i] : c[i]; break;
        case 13: for (int i = 0; i < 4; i++) v[i] = a[i] >= 0.0f ? b[i] : c[i]; break;
        case 14: for (int i = 0; i < 4; i++) v[i] = a[i] > 0.0f ? b[i] : c[i]; break;
        case 15: v[0] = Mul(a[0], b[0]) + Mul(a[1], b[1]) + Mul(a[2], b[2]) + Mul(a[3], b[3]); rep = true; break;
        case 16: v[0] = Mul(a[0], b[0]) + Mul(a[1], b[1]) + Mul(a[2], b[2]); rep = true; break;
        case 17: v[0] = Mul(a[0], b[0]) + Mul(a[1], b[1]) + c[0]; rep = true; break;
        case 18: // cube: operand 0 is .zzxy; result = T, S, 2 * major axis, face id
        {
            float x = a[2], y = a[3], z = a[0], ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
            if (az >= ax && az >= ay)
            {
                bool n = z < 0.0f;
                v[0] = -y, v[1] = n ? -x : x, v[2] = z, v[3] = n ? 5.0f : 4.0f;
            }
            else if (ay >= ax)
            {
                bool n = y < 0.0f;
                v[0] = n ? -z : z, v[1] = x, v[2] = y, v[3] = n ? 3.0f : 2.0f;
            }
            else
            {
                bool n = x < 0.0f;
                v[0] = -y, v[1] = n ? z : -z, v[2] = x, v[3] = n ? 1.0f : 0.0f;
            }
            v[2] *= 2.0f;
            break;
        }
        case 19:
            v[0] = (a[0] >= a[1] && a[0] >= a[2] && a[0] >= a[3]) ? a[0] : (a[1] >= a[2] && a[1] >= a[3]) ? a[1] : a[2] >= a[3] ? a[2] : a[3];
            rep = true;
            break;
        case 20: case 21: case 22: case 23: // setp_*_push
        {
            auto test = [&](float x) { return vop == 20 ? x == 0.0f : vop == 21 ? x != 0.0f : vop == 22 ? x > 0.0f : x >= 0.0f; };
            p0 = a[3] == 0.0f && test(b[3]);
            v[0] = (a[0] == 0.0f && test(b[0])) ? 0.0f : a[0] + 1.0f;
            rep = true;
            break;
        }
        case 24: case 25: case 26: case 27: // kill_*
        {
            bool k = false;
            for (int i = 0; i < 4; i++)
                k |= vop == 24 ? a[i] == b[i] : vop == 25 ? a[i] > b[i] : vop == 26 ? a[i] >= b[i] : a[i] != b[i];
            v[0] = k ? 1.0f : 0.0f;
            rep = true;
            if (k && in.pixel)
                out.killed = true;
            break;
        }
        case 28: v[0] = 1.0f, v[1] = Mul(a[1], b[1]), v[2] = a[2], v[3] = b[3]; break;
        case 29: // maxa
        {
            float w = a[3] < -256.0f ? -256.0f : a[3] > 255.0f ? 255.0f : a[3];
            a0 = int32_t(std::floor(w + 0.5f));
            for (int i = 0; i < 4; i++)
                v[i] = a[i] >= b[i] ? a[i] : b[i];
            break;
        }
        default:
            Fail("vector opcode " + std::to_string(vop));
            return;
        }
        if (rep)
            v[1] = v[2] = v[3] = v[0];
        if (vfinite)
            for (int i = 0; i < 4; i++)
                if ((vmask >> i) & 1 && !Finite(v[i]))
                    out.special = true;
    }

    // Scalar operation (always runs: it updates the previous-scalar register).
    const bool sfinite = Finite(sa) && Finite(sb) && Finite(ps);
    const float prev = ps;
    auto setp = [&](bool p, float value) { p0 = p; ps = value; };
    switch (sop)
    {
    case 0: case 44: case 45: ps = sa + sb; break;
    case 1: ps = sa + prev; break;
    case 2: case 42: case 43: ps = Mul(sa, sb); break;
    case 3: ps = Mul(sa, prev); break;
    case 4: ps = (prev == -FLT_MAX || !Finite(prev) || !Finite(sb) || sb <= 0.0f) ? -FLT_MAX : Mul(sa, prev); break;
    case 5: ps = sa >= sb ? sa : sb; break;
    case 6: ps = sa < sb ? sa : sb; break;
    case 7: ps = sa == 0.0f ? 1.0f : 0.0f; break;
    case 8: ps = sa > 0.0f ? 1.0f : 0.0f; break;
    case 9: ps = sa >= 0.0f ? 1.0f : 0.0f; break;
    case 10: ps = sa != 0.0f ? 1.0f : 0.0f; break;
    case 11: ps = sa - std::floor(sa); break;
    case 12: ps = std::trunc(sa); break;
    case 13: ps = std::floor(sa); break;
    case 14: ps = std::exp2(sa); break;
    case 15: ps = std::log2(sa); if (ps == -INFINITY) ps = -FLT_MAX; break;
    case 16: ps = std::log2(sa); break;
    case 17: ps = 1.0f / sa; if (std::isinf(ps)) ps = ps < 0 ? -FLT_MAX : FLT_MAX; break;
    case 18: ps = 1.0f / sa; if (std::isinf(ps)) ps = ps < 0 ? -0.0f : 0.0f; break;
    case 19: ps = 1.0f / sa; break;
    case 20: ps = 1.0f / std::sqrt(sa); if (std::isinf(ps)) ps = ps < 0 ? -FLT_MAX : FLT_MAX; break;
    case 21: ps = 1.0f / std::sqrt(sa); if (std::isinf(ps)) ps = ps < 0 ? -0.0f : 0.0f; break;
    case 22: ps = 1.0f / std::sqrt(sa); break;
    case 23: case 24: // maxas / maxasf
    {
        float w = sa < -256.0f ? -256.0f : sa > 255.0f ? 255.0f : sa;
        a0 = int32_t(std::floor(sop == 23 ? w + 0.5f : w));
        ps = sa >= sb ? sa : sb;
        break;
    }
    case 25: case 46: case 47: ps = sa - sb; break;
    case 26: ps = sa - prev; break;
    case 27: setp(sa == 0.0f, sa == 0.0f ? 0.0f : 1.0f); break;
    case 28: setp(sa != 0.0f, sa != 0.0f ? 0.0f : 1.0f); break;
    case 29: setp(sa > 0.0f, sa > 0.0f ? 0.0f : 1.0f); break;
    case 30: setp(sa >= 0.0f, sa >= 0.0f ? 0.0f : 1.0f); break;
    case 31: setp(sa == 1.0f, sa == 1.0f ? 0.0f : (sa == 0.0f ? 1.0f : sa)); break; // setp_inv
    case 32: { float n = sa - 1.0f; setp(n <= 0.0f, n <= 0.0f ? 0.0f : n); break; } // setp_pop
    case 33: setp(false, FLT_MAX); break;                                           // setp_clr
    case 34: setp(sa == 0.0f, sa == 0.0f ? 0.0f : sa); break;                       // setp_rstr
    case 35: case 36: case 37: case 38: case 39: // kills_*
    {
        bool k = sop == 35 ? sa == 0.0f : sop == 36 ? sa > 0.0f : sop == 37 ? sa >= 0.0f : sop == 38 ? sa != 0.0f : sa == 1.0f;
        ps = k ? 1.0f : 0.0f;
        if (k && in.pixel)
            out.killed = true;
        break;
    }
    case 40: ps = std::sqrt(sa); break;
    case 48: ps = std::sin(sa); break;
    case 49: ps = std::cos(sa); break;
    case 50: break; // retain_prev
    default:
        Fail("scalar opcode " + std::to_string(sop));
        return;
    }
    if (sfinite && !Finite(ps))
        out.special = true;

    if (vclamp)
        for (float& x : v)
            x = Sat(x);
    const float s = sclamp ? Sat(ps) : ps;
    Log("  %3u alu v%-2u %s%u.%x=(%g %g %g %g)  s%-2u %s%u.%x=%g  p0=%d a0=%d aL=%d%s\n", slot, vop, isExport ? "o" : "r", vdest, vmask, v[0], v[1], v[2],
        v[3], sop, isExport ? "o" : "r", isExport ? vdest : sdest, smask, s, int(p0), a0, AL(), Bits(d1, 28, 1) ? (Bits(d1, 27, 1) ? " (p0)" : " (!p0)") : "");

    if (isExport)
    {
        const uint32_t e = vdest;
        const bool known = in.pixel ? (e <= 3 || e == 61) : (e <= 15 || e == 62 || e == 63);
        if (!known)
        {
            Fail(e >= 32 && e <= 37 ? "memexport" : "export register " + std::to_string(e));
            return;
        }
        const uint32_t one = vmaskRaw & smaskRaw;
        const uint32_t zero = sdestRel ? (0xF & ~(vmaskRaw | smaskRaw)) : 0;
        for (int i = 0; i < 4; i++)
        {
            const uint32_t bit = 1u << i;
            if (vmask & bit)
                out.exports[e][i] = v[i];
            else if (smask & bit)
                out.exports[e][i] = s;
            else if (one & bit)
                out.exports[e][i] = 1.0f;
            else if (zero & bit)
                out.exports[e][i] = 0.0f;
            else
                continue;
            out.exportMask[e] |= bit;
        }
        return;
    }
    if (vmask)
    {
        float* dst = Temp(vdest, vdestRel);
        for (int i = 0; i < 4; i++)
            if ((vmask >> i) & 1)
                dst[i] = v[i];
    }
    if (smask)
    {
        float* dst = Temp(sdest, sdestRel);
        for (int i = 0; i < 4; i++)
            if ((smask >> i) & 1)
                dst[i] = s;
    }
}

// Fetch destination swizzle: 3 bits per component, 0-3 = XYZW, 4 = 0, 5 = 1, 7 = keep.
static void StoreFetch(float* dst, uint32_t swizzle, const float value[4])
{
    for (int i = 0; i < 4; i++)
    {
        const uint32_t s = (swizzle >> (3 * i)) & 7;
        if (s == 7)
            continue;
        dst[i] = s < 4 ? value[s] : s == 5 ? 1.0f : 0.0f;
    }
}

bool Machine::VertexFetch(const uint32_t* d)
{
    const uint32_t d0 = d[0], d1 = d[1], d2 = d[2];
    const bool mini = Bits(d1, 30, 1);
    const uint32_t format = Bits(d1, 16, 6);
    const bool isSigned = Bits(d1, 12, 1), normalized = !Bits(d1, 13, 1), noZero = Bits(d1, 14, 1);
    if (!XenosInterpVertexFormatSupported(format))
        return Fail("vfetch format " + std::to_string(format));
    if (!mini)
    {
        vfetchSlot = Bits(d0, 20, 5) * 3 + Bits(d0, 25, 2);
        const float idx = Temp(Bits(d0, 5, 6), Bits(d0, 11, 1))[Bits(d0, 30, 2)] + (Bits(d1, 15, 1) ? 0.5f : 0.0f);
        const uint32_t base = in.fetch[vfetchSlot * 2] >> 2;
        vfetchAddress = Bits(d2, 0, 8) * uint32_t(int32_t(std::floor(idx))) + base;
    }
    const uint32_t fc0 = in.fetch[vfetchSlot * 2], fc1 = in.fetch[vfetchSlot * 2 + 1];
    const uint32_t base = fc0 >> 2, size = Bits(fc1, 2, 24), endian = fc1 & 3;
    const uint32_t first = uint32_t(int32_t(vfetchAddress) + SBits(d2, 8, 23));
    uint32_t w[4];
    for (uint32_t i = 0; i < 4; i++)
    {
        const uint32_t a = first + i;
        uint32_t raw = 0;
        const size_t byte = size_t(a & 0x07FFFFFF) * 4;
        if (a >= base && a < base + size && byte + 4 <= in.memorySize)
            memcpy(&raw, in.memory + byte, 4);
        w[i] = Swap(raw, endian);
    }
    float res[4] = {};
    auto packed = [&](const uint32_t* widths, const uint32_t* offsets, uint32_t n, bool second) {
        for (uint32_t i = 0; i < n; i++)
        {
            const uint32_t word = (second && i >= 2) ? w[1] : w[0];
            const uint32_t wd = widths[i];
            if (isSigned)
            {
                float x = float(int32_t(word << (32 - wd - offsets[i])) >> (32 - wd));
                if (normalized)
                    x = noZero ? (x + 0.5f) * 2.0f / float((1u << wd) - 1) : std::fmax(-1.0f, x / float((1u << (wd - 1)) - 1));
                res[i] = x;
            }
            else
            {
                float x = float((word >> offsets[i]) & ((1u << wd) - 1));
                res[i] = normalized ? x / float((1u << wd) - 1) : x;
            }
        }
    };
    switch (format)
    {
    case 6: { const uint32_t W[] = { 8, 8, 8, 8 }, O[] = { 0, 8, 16, 24 }; packed(W, O, 4, false); break; }
    case 7: { const uint32_t W[] = { 10, 10, 10, 2 }, O[] = { 0, 10, 20, 30 }; packed(W, O, 4, false); break; }
    case 16: { const uint32_t W[] = { 11, 11, 10 }, O[] = { 0, 11, 22 }; packed(W, O, 3, false); break; }  // 10_11_11
    case 17: { const uint32_t W[] = { 10, 11, 11 }, O[] = { 0, 10, 21 }; packed(W, O, 3, false); break; }  // 11_11_10
    case 25: { const uint32_t W[] = { 16, 16 }, O[] = { 0, 16 }; packed(W, O, 2, false); break; }
    case 26: { const uint32_t W[] = { 16, 16, 16, 16 }, O[] = { 0, 16, 0, 16 }; packed(W, O, 4, true); break; }
    case 31: case 32:
        res[0] = XenosHalf(w[0] & 0xFFFF), res[1] = XenosHalf(w[0] >> 16);
        if (format == 32)
            res[2] = XenosHalf(w[1] & 0xFFFF), res[3] = XenosHalf(w[1] >> 16);
        break;
    case 33: case 34: case 35:
        for (uint32_t i = 0; i < XenosInterpVertexFormatComponents(format); i++)
        {
            if (isSigned)
                res[i] = normalized ? (noZero ? (float(int32_t(w[i])) + 0.5f) / 2147483647.5f : float(int32_t(w[i])) / 2147483647.0f) : float(int32_t(w[i]));
            else
                res[i] = normalized ? float(w[i]) / 4294967295.0f : float(w[i]);
        }
        break;
    default: // 32-bit floats
        for (uint32_t i = 0; i < XenosInterpVertexFormatComponents(format); i++)
            res[i] = AsFloat(w[i]);
        break;
    }
    const int32_t expAdjust = SBits(d1, 24, 6);
    if (expAdjust)
        for (float& x : res)
            x *= std::ldexp(1.0f, expAdjust);
    float* dst = Temp(Bits(d0, 12, 6), Bits(d0, 18, 1));
    StoreFetch(dst, Bits(d1, 0, 12), res);
    Log("      vfetch r%u = (%g %g %g %g) fmt %u\n", Bits(d0, 12, 6), dst[0], dst[1], dst[2], dst[3], format);
    return true;
}

bool Machine::Fetch(const uint32_t* d)
{
    const uint32_t op = Bits(d[0], 0, 5);
    if (op == 0)
        return VertexFetch(d);
    if (op == 1) // tfetch: the per-slot constant (textures are not sampled)
    {
        float* dst = Temp(Bits(d[0], 12, 6), Bits(d[0], 18, 1));
        StoreFetch(dst, Bits(d[1], 0, 12), in.tex[Bits(d[0], 20, 5)]);
        Log("      tfetch r%u = (%g %g %g %g) slot %u\n", Bits(d[0], 12, 6), dst[0], dst[1], dst[2], dst[3], Bits(d[0], 20, 5));
        return true;
    }
    if (op >= 24 && op <= 26) // set texture LOD / gradients: no register result
        return true;
    return Fail("fetch opcode " + std::to_string(op));
}

bool Machine::Run()
{
    uint32_t steps = 0;
    for (uint32_t cf = 0, next;; cf = next)
    {
        next = cf + 1;
        if (++steps > 100000)
            return Fail("runaway control flow");
        if (3 * (cf >> 1) + 2 >= in.dwords)
            return Fail("control flow past end");
        const uint32_t* pair = in.ucode + 3 * (cf >> 1);
        const uint32_t w0 = (cf & 1) ? (pair[1] >> 16) | (pair[2] << 16) : pair[0];
        const uint32_t w1 = (cf & 1) ? pair[2] >> 16 : pair[1] & 0xFFFF;
        const uint32_t op = Bits(w1, 12, 4);
        Log("cf %u op %u w0=%08x w1=%04x p0=%d loop=%u\n", cf, op, w0, w1, int(p0), loopDepth);
        switch (op)
        {
        case 0: case 12: case 15: break; // nop, alloc, mark_vs_fetch_done
        case 1: case 2: case 3: case 4: case 5: case 6: case 13: case 14:
        {
            if (op == 3 || op == 4 || op == 13 || op == 14)
            {
                if (Bits(w1, 10, 1) != uint32_t(Bool(Bits(w1, 2, 8))))
                    break; // condition not met: no execution, and no end
            }
            else if (op == 5 || op == 6)
            {
                if (Bits(w1, 10, 1) != uint32_t(p0))
                    break;
            }
            const uint32_t addr = Bits(w0, 0, 12), count = Bits(w0, 12, 3), seq = Bits(w0, 16, 12);
            for (uint32_t i = 0; i < count; i++)
            {
                if (3 * (addr + i) + 2 >= in.dwords)
                    return Fail("instruction past end");
                const uint32_t* d = in.ucode + 3 * (addr + i);
                const bool isFetch = (seq >> (2 * i)) & 1;
                // Predication: ALU d1 bit 28 / cond bit 27; fetch d1 bit 31 / cond d2 bit 31.
                const bool predicated = isFetch ? Bits(d[1], 31, 1) : Bits(d[1], 28, 1);
                const bool cond = isFetch ? Bits(d[2], 31, 1) : Bits(d[1], 27, 1);
                if (predicated && cond != p0)
                    continue;
                out.executed++;
                if (isFetch)
                {
                    if (!Fetch(d))
                        return false;
                }
                else
                {
                    Alu(d, addr + i);
                    if (!out.skip.empty())
                        return false;
                }
            }
            if (op == 2 || op == 4 || op == 6 || op == 14)
                return true;
            break;
        }
        case 7: // loop_start
        {
            if (loopDepth >= 4)
                return Fail("loop depth > 4");
            Loop l{ in.loops[Bits(w0, 16, 5)], 0 }; // is_repeat (bit 13) not modeled: iterator restarts
            if (l.iterator >= Bits(l.constant, 0, 8))
            {
                next = Bits(w0, 0, 13);
                break;
            }
            loops[loopDepth++] = l;
            break;
        }
        case 8: // loop_end: iterator increments before the count and break checks
        {
            if (!loopDepth)
                break;
            Loop& l = loops[loopDepth - 1];
            l.iterator++;
            const bool brk = Bits(w0, 21, 1) && Bits(w1, 10, 1) == uint32_t(p0);
            if (l.iterator < Bits(l.constant, 0, 8) && !brk)
            {
                next = Bits(w0, 0, 13);
                break;
            }
            loopDepth--;
            break;
        }
        case 9: case 11: // cond_call, cond_jmp
        {
            if (!Bits(w0, 13, 1))
            {
                const bool c = Bits(w0, 14, 1) ? p0 : Bool(Bits(w1, 2, 8));
                if (Bits(w1, 10, 1) != uint32_t(c))
                    break;
            }
            if (op == 9)
            {
                if (callDepth >= 4)
                    return Fail("call depth > 4");
                calls[callDepth++] = cf + 1;
            }
            next = Bits(w0, 0, 13);
            break;
        }
        case 10: // return (no-op outside a call)
            if (callDepth)
                next = calls[--callDepth];
            break;
        }
    }
}
} // namespace

bool XenosInterpret(const XenosInterpInput& in, XenosInterpOutput& out)
{
    out = XenosInterpOutput{};
    Machine m(in, out);
    memcpy(m.r, in.regs, sizeof in.regs);
    return m.Run() && out.skip.empty();
}
