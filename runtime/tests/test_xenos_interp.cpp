// Hand-assembled microcode checks for the reference interpreter (tests/xenos_interp.*).
#include "check.h"
#include "xenos_asm.h"
#include "xenos_interp.h"
#include <cmath>
#include <cstring>

using namespace xasm;

static XenosInterpOutput RunPs(const std::vector<uint32_t>& code, XenosInterpInput in = {})
{
    in.pixel = true;
    in.ucode = code.data();
    in.dwords = code.size();
    XenosInterpOutput out;
    CHECK(XenosInterpret(in, out));
    if (!out.skip.empty())
        printf("  skip: %s\n", out.skip.c_str());
    return out;
}
static void Set4(float* d, float x, float y, float z, float w) { d[0] = x, d[1] = y, d[2] = z, d[3] = w; }

TEST(xenos_interp_add_export)
{
    auto code = Build({ Exec(1, 1, 0, true) }, { Alu(0, 0, 0xF, 50, 0, 0, R(0), C(0), R(0), true) });
    XenosInterpInput in;
    Set4(in.regs[0], 1, 2, 3, 4);
    Set4(in.consts[0], 0.5f, -1, 10, 0);
    auto o = RunPs(code, in);
    CHECK_EQ(o.exportMask[0], 0xF);
    CHECK_EQ(o.exports[0][0], 1.5f);
    CHECK_EQ(o.exports[0][1], 1.0f);
    CHECK_EQ(o.exports[0][2], 13.0f);
    CHECK_EQ(o.exports[0][3], 4.0f);
}

// cexec b5 / cexec !b5: exactly one clause runs.
TEST(xenos_interp_cond_exec_bool)
{
    const uint32_t s = FirstSlot(3);
    auto code = Build({ CondExec(s, 1, 5, true), CondExec(s + 1, 1, 5, false), Exec(0, 0, 0, true) },
        { Alu(2, 0, 0x1, 50, 0, 0, C(0), C(0), R(0), true), Alu(2, 0, 0x1, 50, 0, 0, C(1), C(1), R(0), true) });
    XenosInterpInput in;
    in.consts[0][0] = 7, in.consts[1][0] = 9;
    in.bools[0] = 1u << 5;
    CHECK_EQ(RunPs(code, in).exports[0][0], 7.0f);
    in.bools[0] = 0;
    CHECK_EQ(RunPs(code, in).exports[0][0], 9.0f);
}

// setp_gt on r0.w, then (p0) / (!p0) predicated exports; plus cexec on the predicate.
TEST(xenos_interp_predicates)
{
    const uint32_t s = FirstSlot(3);
    auto code = Build({ Exec(s, 3), CondExecPred(s + 3, 1, true), Exec(0, 0, 0, true) },
        { Alu(0, 0, 0, 29, 0, 0, R(0), R(0), R(0)),                         // setp_gt r0.w
            Alu(2, 0, 0x1, 50, 0, 0, C(0), C(0), R(0), true, 1),               // (p0) oC0.x = c0.x
            Alu(2, 0, 0x1, 50, 0, 0, C(1), C(1), R(0), true, 0),               // (!p0) oC0.x = c1.x
            Alu(2, 0, 0x2, 50, 0, 0, C(2), C(2), R(0), true) });               // cexec p0: oC0.y = c2.y
    XenosInterpInput in;
    in.consts[0][0] = 7, in.consts[1][0] = 9, in.consts[2][1] = 5;
    in.regs[0][3] = 1;
    auto a = RunPs(code, in);
    CHECK_EQ(a.exports[0][0], 7.0f);
    CHECK_EQ(a.exportMask[0], 0x3);
    in.regs[0][3] = -1;
    auto b = RunPs(code, in);
    CHECK_EQ(b.exports[0][0], 9.0f);
    CHECK_EQ(b.exportMask[0], 0x1);
}

// loop i0 (count 3, start 2, step 1): r1 += c[aL] -> c2 + c3 + c4.
TEST(xenos_interp_loop_al)
{
    const uint32_t s = FirstSlot(4);
    Ins body = Alu(0, 1, 0xF, 50, 0, 0, R(1), C(0), R(0));
    body.d[1] |= 1u << 31; // const_0_rel_abs: the first constant operand (src1 is a temp) is aL-relative
    auto code = Build({ LoopStart(3, 0), Exec(s, 1), LoopEnd(1, 0), Exec(s + 1, 1, 0, true) },
        { body, Alu(2, 0, 0xF, 50, 0, 0, R(1), R(1), R(0), true) });
    XenosInterpInput in;
    for (int i = 0; i < 8; i++)
        Set4(in.consts[i], float(1 << i), 0, 0, 0);
    in.loops[0] = 3 | 2 << 8 | 1 << 16;
    CHECK_EQ(RunPs(code, in).exports[0][0], 4.0f + 8 + 16);
    in.loops[0] = 0; // count 0: body skipped
    CHECK_EQ(RunPs(code, in).exports[0][0], 0.0f);
}

// Vector writes r0 while the co-issued scalar reads r0.w: the scalar sees the old value.
TEST(xenos_interp_coissue_reads_before_writes)
{
    auto code = Build({ Exec(1, 2, 0, true) },
        { Alu(1, 0, 0xF, 19, 1, 0x1, R(0), C(0), R(0)),                 // r0 = r0 * c0; r1.x = rcp(r0.w)
            Alu(2, 0, 0xF, 50, 0, 0, R(1, 0, 0, 0, 0), R(1, 0, 0, 0, 0), R(0), true) });
    XenosInterpInput in;
    Set4(in.regs[0], 1, 1, 1, 4);
    Set4(in.consts[0], 8, 8, 8, 8);
    CHECK_EQ(RunPs(code, in).exports[0][0], 0.25f);
}

// mulsc c0.w * r1.x co-issued; export constant 1 / 0 components.
TEST(xenos_interp_scalar_const_temp_and_export_constants)
{
    auto code = Build({ Exec(1, 1, 0, true) },
        { Alu(0, 0, 0x3, 42, 0, 0x6, R(0), R(0), C(2), true) });
    // mulsc: temp register = (sop & 1) | src3_sel << 1 | (src3 swizzle & 0x3C) = r0; a = c2.w, b = r0.x.
    XenosInterpInput in;
    Set4(in.regs[0], 3, 1, 0, 0);
    in.consts[2][3] = 2;
    auto o = RunPs(code, in);
    CHECK_EQ(o.exports[0][0], 6.0f);   // vector add r0+r0 (x)
    CHECK_EQ(o.exports[0][1], 1.0f);   // vector & scalar both masked: constant 1
    CHECK_EQ(o.exports[0][2], 6.0f);   // scalar: c2.w * r0.x
    CHECK_EQ(o.exportMask[0], 0x7);
}

TEST(xenos_interp_vfetch_formats)
{
    uint8_t mem[256] = {};
    auto be32 = [&](size_t off, uint32_t v) { mem[off] = v >> 24, mem[off + 1] = v >> 16, mem[off + 2] = v >> 8, mem[off + 3] = v; };
    float f[4] = { 1.5f, -2, 0.25f, 8 };
    for (int i = 0; i < 4; i++)
    {
        uint32_t u;
        memcpy(&u, &f[i], 4);
        be32(64 + 32 + i * 4, u); // vertex 1 at stride 8 dwords
    }
    be32(64 + 32 + 16, 0x00FF8040); // 8_8_8_8 unorm: x=0x40 y=0x80 z=0xFF w=0
    auto code = Build({ Exec(1, 3, 0x3, true) },
        { VFetch(1, 0, 0, 95, 38, false, false, 8, 0), VFetch(2, 0, 0, 95, 6, false, true, 8, 4, kSwzXYZW, true),
            Alu(2, 62, 0xF, 50, 0, 0, R(1), R(1), R(0), true) });
    XenosInterpInput in;
    in.ucode = code.data(), in.dwords = code.size();
    in.memory = mem, in.memorySize = sizeof mem;
    in.fetch[95 * 2] = 64 | 3, in.fetch[95 * 2 + 1] = 2 | (48u << 2); // address 64 bytes, endian 8in32, 48 dwords
    in.regs[0][0] = 1;
    XenosInterpOutput o;
    CHECK(XenosInterpret(in, o));
    CHECK_EQ(o.exports[62][0], 1.5f);
    CHECK_EQ(o.exports[62][1], -2.0f);
    CHECK_EQ(o.exports[62][3], 8.0f);
    in.regs[0][0] = 0; // vertex 0: zeros
    CHECK(XenosInterpret(in, o));
    CHECK_EQ(o.exports[62][0], 0.0f);
}

TEST(xenos_interp_vfetch_unorm8)
{
    uint8_t mem[64] = {};
    mem[16] = 0x00, mem[17] = 0xFF, mem[18] = 0x80, mem[19] = 0x40; // BE dword 0x00FF8040
    auto code = Build({ Exec(1, 2, 0x1, true) },
        { VFetch(1, 0, 0, 0, 6, false, true, 1, 0), Alu(2, 62, 0xF, 50, 0, 0, R(1), R(1), R(0), true) });
    XenosInterpInput in;
    in.ucode = code.data(), in.dwords = code.size();
    in.memory = mem, in.memorySize = sizeof mem;
    in.fetch[0] = 16 | 3, in.fetch[1] = 2 | (4u << 2);
    XenosInterpOutput o;
    CHECK(XenosInterpret(in, o));
    CHECK_EQ(o.exports[62][0], 0x40 / 255.0f);
    CHECK_EQ(o.exports[62][1], 0x80 / 255.0f);
    CHECK_EQ(o.exports[62][2], 1.0f);
    CHECK_EQ(o.exports[62][3], 0.0f);
}

TEST(xenos_interp_kill_and_tfetch)
{
    auto code = Build({ Exec(1, 3, 0x1, true) },
        { TFetch(2, 0, 3), Alu(25, 0, 0, 50, 0, 0, R(0), C(0), R(0)), Alu(2, 0, 0xF, 50, 0, 0, R(2), R(2), R(0), true) });
    XenosInterpInput in;
    Set4(in.tex[3], 0.25f, 0.5f, 0.75f, 1);
    Set4(in.regs[0], 0, 0, 0, 2); // kill_gt: r0.w > c0.w
    auto o = RunPs(code, in);
    CHECK(o.killed);
    CHECK_EQ(o.exports[0][2], 0.75f);
    in.regs[0][3] = 0;
    CHECK(!RunPs(code, in).killed);
}

TEST(xenos_interp_skips_memexport)
{
    auto code = Build({ Exec(1, 1, 0, true) }, { Alu(2, 33, 0xF, 50, 0, 0, R(0), R(0), R(0), true) });
    XenosInterpInput in;
    in.ucode = code.data(), in.dwords = code.size();
    XenosInterpOutput o;
    CHECK(!XenosInterpret(in, o));
    CHECK_EQ(o.skip, std::string("memexport"));
}
