// Differential shader test: each Xenos shader runs through the reference CPU interpreter
// (tests/xenos_interp.*) and through TranslateShader + CompileHlsl on a D3D11 device; outputs are diffed.
//   SCDA_SHADER_DIFF=<dir>            corpus root (<name>.vs.bin / <name>.ps.bin, big-endian microcode)
//   SCDA_SHADER_DIFF_FILTER=<hex>     only this shader hash
//   SCDA_SHADER_DIFF_SEEDS=<n>        input sets per shader (default 4, 4 vertices / pixels each)
//   SCDA_SHADER_DIFF_LIMIT=<n>        stop after n unique shaders
//   SCDA_SHADER_DIFF_MUTATE=1         self-check: corrupt a copy of each HLSL (flip a predicate / bool
//                                     test) before compiling; expect DIFFs
//   SCDA_SHADER_DIFF_WARP=1           force the WARP device (default: hardware, WARP fallback)
//   SCDA_SHADER_DIFF_DUMP=1           write each shader's HLSL next to the report
//   SCDA_SHADER_DIFF_TRACE=1          with _DUMP: write the interpreter trace of the first differing vector
//   SCDA_SHADER_DIFF_FXC=od|ieee      debug: compile with /Od or /Gis instead of CompileHlsl's flags
//   SCDA_SHADER_DIFF_PROBE=...        debug: see RunShader
//   SCDA_SHADER_DIFF_STAGE=vs|ps      only one stage
//   SCDA_SHADER_DIFF_VFETCH_FORMATS=  comma list of vfetch formats to patch in (default: all supported)
//   SCDA_SHADER_DIFF_VFETCH_ALL=1     also patch vfetch variants with known translator gaps
// Report: _run/shader_diff/report.txt (or SCDA_SHADER_DIFF_REPORT).
//
// Inputs: tfetch results are a per-slot constant on both sides (the GPU binds 1x1 point-sampled
// textures of that value). The corpus vfetches are unpatched (format 0, stride 0), so the harness
// patches every vfetch in a copy of the microcode to a supported format/stride/offset; both sides
// get the patched blob. VS positions are read before the viewport transform through the user clip
// plane outputs (g_Ucp = unit vectors -> SV_ClipDistance0 = guest position).
//
// Tolerance per component: equal (+-0 equal, NaN == NaN, denormals flushed), or ulp <= 4, or
// |a - b| <= tol * (max(|a|, |b|) + 1) with tol = 1e-3 (summation order / fused mad on
// cancelling dot products), or 4e-3 when the shader has a transcendental
// (exp/log/rcp/rsq/sqrt/sin/cos: hosts approximate them differently). A mismatch in a run where the
// interpreter produced NaN/Inf from finite operands is reported as DIFF-numeric (expected
// convention differences: D3D9 0*Inf=0, rsq/log of negatives, etc.), not DIFF.
#include "check.h"
#include "xenos_asm.h"
#include "xenos_interp.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <gpu/shader_cache.h>
#include <gpu/xenos/translate.h>

namespace
{
template <class T> struct Com
{
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    ~Com() { if (p) p->Release(); }
    T** operator&() { if (p) p->Release(); p = nullptr; return &p; }
    T* operator->() const { return p; }
    operator T*() const { return p; }
};

constexpr uint32_t kVerts = 4;           // vertices / pixels per input set
constexpr uint32_t kMemBytes = 1 << 16;  // guest memory window bound at t96
constexpr uint32_t kVertexBase = 0x1000; // fetch constant address (bytes)
constexpr float kSentinel = -7777.0f;    // render target clear (killed pixels keep it)

// ---- static microcode scan --------------------------------------------------------------------
struct Scan
{
    std::vector<uint32_t> fetches;    // instruction slots of vfetch instructions (sorted)
    std::set<uint32_t> fetchOps;      // fetch opcodes present
    uint8_t exportMask[64] = {};      // union of export ALU write masks
    bool cube = false, kill = false, transcendental = false;
    bool loops = false, jumps = false, calls = false, predExec = false, condExec = false;
};

Scan ScanUcode(const std::vector<uint32_t>& u)
{
    Scan s;
    uint32_t minAddr = ~0u;
    std::set<uint32_t> alu, fetch;
    for (uint32_t cf = 0; cf < 2 * uint64_t(minAddr) && 3 * (cf >> 1) + 2 < u.size(); cf++)
    {
        const uint32_t* p = &u[3 * (cf >> 1)];
        const uint32_t w0 = (cf & 1) ? (p[1] >> 16) | (p[2] << 16) : p[0], w1 = (cf & 1) ? p[2] >> 16 : p[1] & 0xFFFF;
        const uint32_t op = (w1 >> 12) & 15;
        s.loops |= op == 7, s.calls |= op == 9, s.jumps |= op == 11, s.predExec |= op == 5 || op == 6;
        s.condExec |= op == 3 || op == 4 || op == 13 || op == 14;
        if (!(op >= 1 && op <= 6) && op != 13 && op != 14)
            continue;
        const uint32_t addr = w0 & 0xFFF, count = (w0 >> 12) & 7, seq = (w0 >> 16) & 0xFFF;
        if (count)
            minAddr = std::min(minAddr, addr);
        for (uint32_t i = 0; i < count; i++)
            (((seq >> (2 * i)) & 1) ? fetch : alu).insert(addr + i);
    }
    for (uint32_t i : fetch)
    {
        if (3 * i + 2 >= u.size())
            continue;
        const uint32_t op = u[3 * i] & 31;
        s.fetchOps.insert(op);
        if (op == 0)
            s.fetches.push_back(i);
    }
    for (uint32_t i : alu)
    {
        if (3 * i + 2 >= u.size())
            continue;
        const uint32_t d0 = u[3 * i], vop = (u[3 * i + 2] >> 24) & 31, sop = d0 >> 26;
        s.cube |= vop == 18;
        s.kill |= (vop >= 24 && vop <= 27) || (sop >= 35 && sop <= 39);
        s.transcendental |= (sop >= 14 && sop <= 22) || sop == 40 || sop == 48 || sop == 49;
        if ((d0 >> 15) & 1)
            s.exportMask[d0 & 63] |= ((d0 >> 16) & 15) | ((d0 >> 20) & 15) | (((d0 >> 14) & 1) ? 15 : 0);
    }
    return s;
}

// ---- vfetch patching ----------------------------------------------------------------------------
struct FetchPatch { uint32_t format; bool isSigned, normalized; int32_t expAdjust; uint32_t offset; };

// Formats both sides decode, cycled per fetch (start rotated per shader); filtered by the components the dest swizzle reads.
const uint32_t kFormatCycle[] = { 38, 6, 26, 32, 37, 57, 7, 25, 31, 34, 36, 33, 35, 16, 17 };

std::vector<FetchPatch> PatchFetches(std::vector<uint32_t>& u, const Scan& s, uint32_t& stride, uint32_t rotate)
{
    std::vector<FetchPatch> out;
    const uint32_t n = uint32_t(s.fetches.size());
    stride = std::min<uint32_t>(std::max<uint32_t>(4 * n, 4), 252);
    for (uint32_t k = 0; k < n; k++)
    {
        uint32_t* d = &u[3 * s.fetches[k]];
        uint32_t need = 0;
        for (int c = 0; c < 4; c++)
        {
            const uint32_t sw = (d[1] >> (3 * c)) & 7;
            if (sw < 4)
                need = std::max(need, sw + 1);
        }
        // SCDA_SHADER_DIFF_VFETCH_FORMATS=<n,n,...> restricts the cycle (triage of one format at a time).
        static std::vector<uint32_t> cycle = [] {
            std::vector<uint32_t> c;
            if (const char* e = getenv("SCDA_SHADER_DIFF_VFETCH_FORMATS"))
                for (const char* p = e; *p; p += (*p == ',') ? 1 : 0)
                {
                    char* end;
                    c.push_back(uint32_t(strtoul(p, &end, 10)));
                    if (end == p)
                        break;
                    p = end;
                }
            if (c.empty())
                c.assign(std::begin(kFormatCycle), std::end(kFormatCycle));
            return c;
        }();
        // Known translator gaps (report triage), skipped unless SCDA_SHADER_DIFF_VFETCH_ALL so they do not
        // mask other differences: 10_11_11 / 11_11_10 bit layouts are swapped relative to Xenia.
        const bool all = getenv("SCDA_SHADER_DIFF_VFETCH_ALL") != nullptr;
        uint32_t format = 38; // fallback: covers every component
        for (uint32_t j = 0; j < cycle.size(); j++)
        {
            const uint32_t f = cycle[(k + j + rotate) % cycle.size()];
            if ((f == 16 || f == 17) && !all)
                continue;
            if (XenosInterpVertexFormatComponents(f) >= need)
            {
                format = f;
                break;
            }
        }
        FetchPatch p{ format, bool((k + rotate) & 1), ((k + rotate) % 3) != 2, ((k + rotate) % 5 == 4) ? -2 : 0, (4 * k) % stride };
        // Known translator gap: normalized 32-bit integer formats are not normalized.
        if (format >= 33 && format <= 35 && !all)
            p.normalized = false;
        d[1] = (d[1] & ~((1u << 12) | (1u << 13) | (1u << 14) | (63u << 16) | (63u << 24))) | uint32_t(p.isSigned) << 12 |
               uint32_t(!p.normalized) << 13 | p.format << 16 | (uint32_t(p.expAdjust) & 63) << 24;
        const bool mini = (d[1] >> 30) & 1;
        d[2] = (d[2] & 0x80000000u) | (p.offset & 0x7FFFFF) << 8 | (mini ? (d[2] & 0xFF) : stride);
        out.push_back(p);
    }
    return out;
}

// ---- inputs ----------------------------------------------------------------------------------
struct Rng
{
    std::mt19937 g;
    explicit Rng(uint64_t seed) : g(uint32_t(seed ^ (seed >> 32))) {}
    uint32_t U(uint32_t n) { return g() % n; }
    // mode 0: positive multiples of 1/16 in [0.25, 4]; 1: same, random sign; 2: positive random
    // floats in [0.25, 4]; 3: random sign.
    float F(uint32_t mode)
    {
        float v = (mode & 2) ? std::uniform_real_distribution<float>(0.25f, 4.0f)(g) : float(U(61) + 4) / 16.0f;
        return ((mode & 1) && (g() & 1)) ? -v : v;
    }
};

void PutBE(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }
uint32_t FBits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

struct Inputs
{
    XenosInterpInput base;
    std::vector<uint8_t> memory;
    float regs[kVerts][16][4] = {};
};

void MakeInputs(Inputs& in, uint64_t seed, uint32_t mode, bool pixel, const std::vector<FetchPatch>& fetches, uint32_t stride)
{
    Rng r(seed);
    in.base.pixel = pixel;
    for (auto& c : in.base.consts)
        for (float& x : c)
            x = r.F(mode);
    for (uint32_t& b : in.base.bools)
        b = r.g();
    for (uint32_t& l : in.base.loops)
        l = r.U(4) | r.U(17) << 8 | (uint32_t(int32_t(r.U(5)) - 2) & 0xFF) << 16;
    for (uint32_t s = 0; s < 96; s++)
    {
        in.base.fetch[s * 2] = kVertexBase | 3;
        in.base.fetch[s * 2 + 1] = 2 | ((kMemBytes - kVertexBase) / 4) << 2; // endian 8-in-32, rest of the window
    }
    for (auto& t : in.base.tex)
        for (float& x : t)
            x = float(r.U(17)) / 16.0f;
    in.memory.assign(kMemBytes, 0);
    for (uint32_t a = 0; a < kMemBytes; a += 4)
        PutBE(&in.memory[a], FBits(r.F(mode)));
    for (uint32_t v = 0; v < kVerts; v++)
        for (uint32_t k = 0; k < fetches.size(); k++)
        {
            const FetchPatch& p = fetches[k];
            uint8_t* d = &in.memory[kVertexBase + 4 * (v * stride + p.offset)];
            for (uint32_t i = 0; i < 4; i++)
            {
                uint32_t w;
                if (p.format == 31 || p.format == 32) // halves with exponent 13..17 (no Xenos/IEEE exp-31 split)
                {
                    auto h = [&] { return (r.U(2) << 15) | ((13 + r.U(5)) << 10) | r.U(1024); };
                    w = h() | h() << 16;
                }
                else if (p.format >= 33 && p.format <= 35) // small integers (huge ones only measure cancellation)
                    w = p.isSigned ? uint32_t(int32_t(r.U(2001)) - 1000) : r.U(1001);
                else if (p.format == 36 || p.format == 37 || p.format == 38 || p.format == 57)
                    w = FBits(r.F(mode));
                else
                    w = r.g();
                PutBE(d + 4 * i, w);
            }
        }
    for (uint32_t v = 0; v < kVerts; v++)
    {
        if (pixel)
            for (auto& reg : in.regs[v])
                for (float& x : reg)
                    x = r.F(mode);
        else
            in.regs[v][0][0] = float(v);
    }
}

// ---- comparison ------------------------------------------------------------------------------
float FlushF(float f) { uint32_t u = FBits(f); if (!(u & 0x7F800000u)) u &= 0x80000000u; float o; memcpy(&o, &u, 4); return o; }
bool Close(float c, float g, bool approx)
{
    c = FlushF(c), g = FlushF(g);
    if (c == g || (std::isnan(c) && std::isnan(g)))
        return true;
    if (!std::isfinite(c) || !std::isfinite(g))
        return false;
    auto ord = [](float f) { int32_t i; memcpy(&i, &f, 4); return i < 0 ? int64_t(INT32_MIN) - i : int64_t(i); };
    if (std::llabs(ord(c) - ord(g)) <= 4)
        return true;
    const double tol = approx ? 4e-3 : 1e-3;
    return std::fabs(double(c) - double(g)) <= tol * (std::max(std::fabs(double(c)), std::fabs(double(g))) + 1.0);
}

// ---- D3D11 -----------------------------------------------------------------------------------
struct Gpu
{
    Com<ID3D11Device> dev;
    Com<ID3D11DeviceContext> ctx;
    std::string adapter;
    Com<ID3D11Texture2D> tex2d[32], texCube[32];
    Com<ID3D11ShaderResourceView> srv2d[32], srvCube[32], memSrv, inSrv;
    Com<ID3D11Buffer> mem, inBuf, cb[3];
    Com<ID3D11SamplerState> samp;
    Com<ID3D11Texture2D> rt[4], rtStage[4];
    Com<ID3D11RenderTargetView> rtv[4];
    Com<ID3D11VertexShader> passVs;
    Com<ID3D11RasterizerState> rast;

    bool Init(bool warp)
    {
        const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = E_FAIL;
        if (!warp)
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
        if (FAILED(hr))
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
        if (FAILED(hr))
            return false;
        Com<IDXGIDevice> dx;
        Com<IDXGIAdapter> ad;
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dx))) && SUCCEEDED(dx->GetAdapter(&ad)) && SUCCEEDED(ad->GetDesc(&desc)))
        {
            char name[256];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name, nullptr, nullptr);
            adapter = name;
        }
        for (int i = 0; i < 32; i++)
        {
            D3D11_TEXTURE2D_DESC td{ 1, 1, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex2d[i])) || FAILED(dev->CreateShaderResourceView(tex2d[i], nullptr, &srv2d[i])))
                return false;
            td.ArraySize = 6;
            td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &texCube[i])) || FAILED(dev->CreateShaderResourceView(texCube[i], nullptr, &srvCube[i])))
                return false;
        }
        D3D11_BUFFER_DESC bd{ kMemBytes, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0 };
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &mem)))
            return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sd.BufferEx.NumElements = kMemBytes / 4;
        sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        if (FAILED(dev->CreateShaderResourceView(mem, &sd, &memSrv)))
            return false;
        // Interpolators for the pass-through VS: kVerts x 16 float4.
        bd = { kVerts * 16 * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16 };
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &inBuf)) || FAILED(dev->CreateShaderResourceView(inBuf, nullptr, &inSrv)))
            return false;
        D3D11_SAMPLER_DESC smp{};
        smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        smp.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(dev->CreateSamplerState(&smp, &samp)))
            return false;
        for (int i = 0; i < 4; i++)
        {
            D3D11_TEXTURE2D_DESC td{ kVerts, 1, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0 };
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &rt[i])) || FAILED(dev->CreateRenderTargetView(rt[i], nullptr, &rtv[i])))
                return false;
            td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &rtStage[i])))
                return false;
        }
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE, rd.DepthClipEnable = FALSE;
        if (FAILED(dev->CreateRasterizerState(&rd, &rast)))
            return false;
        // Pass-through VS: one point per pixel at its center, interpolators straight from the buffer
        // (points are not interpolated). Output layout matches the translated PS input signature.
        char src[512];
        snprintf(src, sizeof src,
            "StructuredBuffer<float4> g_In : register(t0);\n"
            "void main(uint id : SV_VertexID, out float4 pos : SV_Position, out float4 t[16] : TEXCOORD0)\n"
            "{ pos = float4((id + 0.5) * 2.0 / %u.0 - 1.0, 0.0, 0.5, 1.0);\n"
            "  [unroll] for (uint i = 0; i < 16; i++) t[i] = g_In[id * 16 + i]; }\n", kVerts);
        std::string err;
        auto code = CompileHlsl(src, "vs_5_0", &err);
        if (code.empty() || FAILED(dev->CreateVertexShader(code.data(), code.size(), nullptr, &passVs)))
        {
            printf("  pass-through VS: %s\n", err.c_str());
            return false;
        }
        return true;
    }

    bool MakeCb(int i, uint32_t bytes)
    {
        if (cb[i])
        {
            D3D11_BUFFER_DESC d;
            cb[i]->GetDesc(&d);
            if (d.ByteWidth == bytes)
                return true;
        }
        D3D11_BUFFER_DESC bd{ bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
        return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &cb[i]));
    }

    void BindCommon(bool pixel)
    {
        ID3D11ShaderResourceView* t2[32];
        ID3D11ShaderResourceView* tc[32];
        for (int i = 0; i < 32; i++)
            t2[i] = srv2d[i], tc[i] = srvCube[i];
        ID3D11SamplerState* s[16];
        for (auto& x : s)
            x = samp;
        ID3D11Buffer* cbs[3] = { cb[0], cb[1], cb[2] };
        ID3D11ShaderResourceView* m = memSrv;
        if (pixel)
        {
            ctx->PSSetShaderResources(kHlslTex2DSlot, 32, t2);
            ctx->PSSetShaderResources(kHlslTexCubeSlot, 32, tc);
            ctx->PSSetShaderResources(kHlslMemorySlot, 1, &m);
            ctx->PSSetSamplers(0, 16, s);
            ctx->PSSetConstantBuffers(0, 3, cbs);
            ID3D11ShaderResourceView* in = inSrv;
            ctx->VSSetShaderResources(0, 1, &in);
        }
        else
        {
            ctx->VSSetShaderResources(kHlslTex2DSlot, 32, t2);
            ctx->VSSetShaderResources(kHlslTexCubeSlot, 32, tc);
            ctx->VSSetShaderResources(kHlslMemorySlot, 1, &m);
            ctx->VSSetSamplers(0, 16, s);
            ctx->VSSetConstantBuffers(0, 3, cbs);
        }
    }

    void Upload(const Inputs& in, const std::vector<uint8_t>& shared, bool pixel)
    {
        ctx->UpdateSubresource(cb[pixel ? 1 : 0], 0, nullptr, in.base.consts, 0, 0);
        ctx->UpdateSubresource(cb[2], 0, nullptr, shared.data(), 0, 0);
        ctx->UpdateSubresource(mem, 0, nullptr, in.memory.data(), 0, 0);
        for (int i = 0; i < 32; i++)
        {
            ctx->UpdateSubresource(tex2d[i], 0, nullptr, in.base.tex[i], 16, 16);
            for (UINT f = 0; f < 6; f++)
                ctx->UpdateSubresource(texCube[i], f, nullptr, in.base.tex[i], 16, 16);
        }
        if (pixel)
            ctx->UpdateSubresource(inBuf, 0, nullptr, in.regs, 0, 0);
    }
};

// SharedConstants cbuffer image built from reflection offsets (robust to layout changes).
struct SharedLayout
{
    uint32_t size = 0;
    std::map<std::string, uint32_t> offset;
};

bool ReflectShared(ID3D11ShaderReflection* refl, SharedLayout& out, std::string& err)
{
    ID3D11ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByName("SharedConstants");
    D3D11_SHADER_BUFFER_DESC bd{};
    if (!cb || FAILED(cb->GetDesc(&bd)))
        return err = "no SharedConstants cbuffer", false;
    out.size = bd.Size;
    for (UINT i = 0; i < bd.Variables; i++)
    {
        D3D11_SHADER_VARIABLE_DESC vd{};
        if (SUCCEEDED(cb->GetVariableByIndex(i)->GetDesc(&vd)))
            out.offset[vd.Name] = vd.StartOffset;
    }
    for (const char* need : { "g_BoolWords", "g_Loops", "g_Fetch", "g_Ucp", "g_UcpEnable", "g_Tex3DIs2D", "g_AlphaFunc", "g_PositionScale" })
        if (!out.offset.count(need))
            return err = std::string("SharedConstants lacks ") + need, false;
    return true;
}

std::vector<uint8_t> BuildShared(const SharedLayout& l, const XenosInterpInput& in)
{
    std::vector<uint8_t> b((l.size + 15) & ~15u, 0);
    auto put = [&](const char* name, const void* data, size_t bytes) { memcpy(&b[l.offset.at(name)], data, bytes); };
    put("g_BoolWords", in.bools, sizeof in.bools);
    put("g_Loops", in.loops, sizeof in.loops);
    put("g_Fetch", in.fetch, sizeof in.fetch);
    const float ucp[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
    put("g_Ucp", ucp, sizeof ucp);
    const uint32_t ucpEnable = 0xF, all = ~0u, always = 7;
    put("g_UcpEnable", &ucpEnable, 4);
    put("g_Tex3DIs2D", &all, 4);
    put("g_AlphaFunc", &always, 4);
    const float one2[2] = { 1, 1 };
    put("g_PositionScale", one2, 8);
    return b;
}

// ---- mutation self-check ---------------------------------------------------------------------
bool Mutate(std::string& h)
{
    const size_t body = h.find("void xenos_main(");
    const size_t end = h.find("\nvoid main(", body);
    if (body == std::string::npos || end == std::string::npos)
        return false;
    struct R { const char* from; const char* to; };
    for (R r : { R{ "if (p0)", "if (!p0)" }, R{ "if (!p0)", "if (p0)" }, R{ " != 0)", " == 0)" }, R{ " == 0)", " != 0)" } })
    {
        size_t p = h.find(r.from, body);
        if (p != std::string::npos && p < end)
            return h.replace(p, strlen(r.from), r.to), true;
    }
    size_t p = h.find(" + ", h.find("{", body)); // no branches: an add becomes a subtract
    if (p != std::string::npos && p < end)
        return h.replace(p, 3, " - "), true;
    return false;
}

// ---- one shader ------------------------------------------------------------------------------
struct Result
{
    enum Kind { Pass, Diff, DiffNumeric, Skip } kind = Skip;
    std::string detail;
};

const char* OutName(bool pixel, uint32_t e, char* buf)
{
    if (pixel)
        snprintf(buf, 16, e == 61 ? "oDepth" : "oC%u", e);
    else
        snprintf(buf, 16, e == 62 ? "oPos" : e == 63 ? "oPts" : "o%u", e);
    return buf;
}

Result RunShader(Gpu& gpu, ShaderType type, const std::vector<uint32_t>& ucodeIn, uint32_t seeds, bool mutate, const std::string& dumpPath)
{
    uint32_t rotate = 0; // vfetch format rotation, varied per shader so the corpus covers every format
    for (uint32_t d : ucodeIn)
        rotate = rotate * 31 + d;
    const bool pixel = type == ShaderType::Pixel;
    std::vector<uint32_t> ucode = ucodeIn;
    Scan scan = ScanUcode(ucode);
    for (uint32_t op : scan.fetchOps)
        if (op != 0 && op != 1 && !(op >= 24 && op <= 26))
            return { Result::Skip, "fetch opcode " + std::to_string(op) };
    if (pixel && scan.exportMask[61])
        return { Result::Skip, "oDepth" };
    uint32_t stride = 4;
    std::vector<FetchPatch> patches = PatchFetches(ucode, scan, stride, rotate);

    std::vector<uint32_t> be(ucode.size());
    for (size_t i = 0; i < ucode.size(); i++)
        be[i] = _byteswap_ulong(ucode[i]);
    const ShaderBlob* blob = ShaderCacheInsert(type, be.data(), uint32_t(be.size()));
    TranslateResult tr = TranslateShader(*blob);
    if (!tr.ok)
        return { Result::Skip, "translate failed" };
    std::string hlsl = tr.hlsl;
    if (mutate && !Mutate(hlsl))
        return { Result::Skip, "mutate: no site" };
    // Debug: SCDA_SHADER_DIFF_PROBE="<text>|<e0>,<e1>,<e2>,<e3>" (PS only) returns the four HLSL expressions
    // through oC0..oC3 right after the first line of xenos_main containing <text>; prints seed-0 values.
    const char* probe = pixel ? getenv("SCDA_SHADER_DIFF_PROBE") : nullptr;
    if (probe)
    {
        std::string pr = probe, at = pr.substr(0, pr.find('|')), ex = pr.substr(pr.find('|') + 1), ins = "\n";
        for (int i = 0; i < 4; i++)
        {
            const size_t c = ex.find(',');
            ins += "oC" + std::to_string(i) + " = " + ex.substr(0, c) + ";";
            ex = c == std::string::npos ? "0" : ex.substr(c + 1);
        }
        const size_t p = hlsl.find(at, hlsl.find("void xenos_main("));
        if (p == std::string::npos)
            return { Result::Skip, "probe: text not found" };
        hlsl.insert(hlsl.find('\n', p), ins + " return;");
    }
    if (!dumpPath.empty())
        std::ofstream(dumpPath) << hlsl;
    std::string err;
    auto code = CompileHlsl(hlsl, pixel, &err);
    // Debug: SCDA_SHADER_DIFF_FXC=od|ieee recompiles with /Od or /Gis to tell fxc optimizations from translation bugs.
    if (const char* fx = getenv("SCDA_SHADER_DIFF_FXC"))
    {
        Com<ID3DBlob> blobOut, errs;
        static const D3D_SHADER_MACRO kVs[] = { { "SCDA_VERTEX", "1" }, { nullptr, nullptr } };
        const UINT flags = strcmp(fx, "od") == 0 ? D3DCOMPILE_SKIP_OPTIMIZATION : (D3DCOMPILE_OPTIMIZATION_LEVEL1 | D3DCOMPILE_IEEE_STRICTNESS);
        code.clear();
        if (SUCCEEDED(D3DCompile(hlsl.data(), hlsl.size(), "shader.hlsl", pixel ? nullptr : kVs, nullptr, "main", pixel ? "ps_5_0" : "vs_5_0",
                flags, 0, &blobOut, &errs)))
            code.assign((uint8_t*)blobOut->GetBufferPointer(), (uint8_t*)blobOut->GetBufferPointer() + blobOut->GetBufferSize());
    }
    if (code.empty())
        return { Result::Skip, "compile failed" };
    Com<ID3D11ShaderReflection> refl;
    if (FAILED(D3DReflect(code.data(), code.size(), IID_PPV_ARGS(&refl))))
        return { Result::Skip, "reflect" };
    SharedLayout layout;
    if (!ReflectShared(refl, layout, err))
        return { Result::Skip, err };
    if (!gpu.MakeCb(0, 4096) || !gpu.MakeCb(1, 4096) || !gpu.MakeCb(2, (layout.size + 15) & ~15u))
        return { Result::Skip, "cbuffer" };

    // Pipeline objects.
    Com<ID3D11VertexShader> vs;
    Com<ID3D11GeometryShader> so;
    Com<ID3D11PixelShader> ps;
    Com<ID3D11Buffer> soBuf, soStage;
    struct SoField { uint32_t reg; int comp0, count; uint32_t offset; }; // reg: export register
    std::vector<SoField> fields;
    UINT soStride = 0;
    D3D11_SHADER_DESC sdesc{};
    refl->GetDesc(&sdesc);
    if (pixel)
    {
        for (UINT i = 0; i < sdesc.InputParameters; i++)
        {
            D3D11_SIGNATURE_PARAMETER_DESC p{};
            refl->GetInputParameterDesc(i, &p);
            const std::string sem = p.SemanticName;
            if (!(sem == "SV_Position" || sem == "SV_IsFrontFace" || (sem == "TEXCOORD" && p.SemanticIndex < 16)))
                return { Result::Skip, "ps input " + sem };
        }
        if (FAILED(gpu.dev->CreatePixelShader(code.data(), code.size(), nullptr, &ps)))
            return { Result::Skip, "CreatePixelShader" };
    }
    else
    {
        std::vector<D3D11_SO_DECLARATION_ENTRY> decl;
        for (UINT i = 0; i < sdesc.OutputParameters; i++)
        {
            D3D11_SIGNATURE_PARAMETER_DESC p{};
            refl->GetOutputParameterDesc(i, &p);
            const std::string sem = p.SemanticName;
            uint32_t reg;
            if (sem == "TEXCOORD")
                reg = p.SemanticIndex;
            else if (sem == "POINTSIZE")
                reg = 63;
            else if (sem == "SV_ClipDistance" && p.SemanticIndex == 0)
                reg = 62;
            else
                continue;
            int c0 = 0;
            while (c0 < 4 && !((p.Mask >> c0) & 1))
                c0++;
            int n = 0;
            while (c0 + n < 4 && ((p.Mask >> (c0 + n)) & 1))
                n++;
            decl.push_back({ 0, p.SemanticName, p.SemanticIndex, BYTE(c0), BYTE(n), 0 });
            fields.push_back({ reg, c0, n, soStride });
            soStride += 4 * n;
        }
        if (FAILED(gpu.dev->CreateVertexShader(code.data(), code.size(), nullptr, &vs)))
            return { Result::Skip, "CreateVertexShader" };
        if (FAILED(gpu.dev->CreateGeometryShaderWithStreamOutput(code.data(), code.size(), decl.data(), UINT(decl.size()), &soStride, 1,
                D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &so)))
            return { Result::Skip, "stream output" };
        D3D11_BUFFER_DESC bd{ soStride * kVerts, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, 0 };
        if (FAILED(gpu.dev->CreateBuffer(&bd, nullptr, &soBuf)))
            return { Result::Skip, "so buffer" };
        bd.Usage = D3D11_USAGE_STAGING, bd.BindFlags = 0, bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(gpu.dev->CreateBuffer(&bd, nullptr, &soStage)))
            return { Result::Skip, "so staging" };
    }

    static Inputs in; // large: keep off the stack
    static float got[kVerts][64][4];
    for (uint32_t s = 0; s < seeds; s++)
    {
        in = Inputs{};
        MakeInputs(in, blob->hash * 31 + s, s % 4, pixel, patches, stride);
        in.base.ucode = ucode.data(), in.base.dwords = ucode.size();
        in.base.memory = in.memory.data(), in.base.memorySize = in.memory.size();
        static XenosInterpOutput ref[kVerts];
        for (uint32_t v = 0; v < kVerts; v++)
        {
            static XenosInterpInput vin;
            vin = in.base;
            memcpy(vin.regs, in.regs[v], sizeof vin.regs);
            if (!XenosInterpret(vin, ref[v]))
                return { Result::Skip, ref[v].skip };
        }

        // GPU run.
        std::vector<uint8_t> shared = BuildShared(layout, in.base);
        gpu.ctx->ClearState();
        gpu.Upload(in, shared, pixel);
        gpu.BindCommon(pixel);
        gpu.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        memset(got, 0, sizeof got);
        bool gpuKilled[kVerts] = {};
        if (pixel)
        {
            gpu.ctx->VSSetShader(gpu.passVs, nullptr, 0);
            gpu.ctx->PSSetShader(ps, nullptr, 0);
            gpu.ctx->RSSetState(gpu.rast);
            D3D11_VIEWPORT vp{ 0, 0, float(kVerts), 1, 0, 1 };
            gpu.ctx->RSSetViewports(1, &vp);
            ID3D11RenderTargetView* rtvs[4] = { gpu.rtv[0], gpu.rtv[1], gpu.rtv[2], gpu.rtv[3] };
            const float clear[4] = { kSentinel, kSentinel, kSentinel, kSentinel };
            for (auto* r : rtvs)
                gpu.ctx->ClearRenderTargetView(r, clear);
            gpu.ctx->OMSetRenderTargets(4, rtvs, nullptr);
            gpu.ctx->Draw(kVerts, 0);
            for (int i = 0; i < 4; i++)
            {
                gpu.ctx->CopyResource(gpu.rtStage[i], gpu.rt[i]);
                D3D11_MAPPED_SUBRESOURCE m;
                if (FAILED(gpu.ctx->Map(gpu.rtStage[i], 0, D3D11_MAP_READ, 0, &m)))
                    return { Result::Skip, "map rt" };
                for (uint32_t v = 0; v < kVerts; v++)
                    memcpy(got[v][i], (const float*)m.pData + 4 * v, 16);
                gpu.ctx->Unmap(gpu.rtStage[i], 0);
            }
            for (uint32_t v = 0; v < kVerts; v++)
            {
                gpuKilled[v] = true;
                for (int i = 0; i < 4; i++)
                    for (int c = 0; c < 4; c++)
                        gpuKilled[v] &= got[v][i][c] == kSentinel;
            }
        }
        else
        {
            gpu.ctx->VSSetShader(vs, nullptr, 0);
            gpu.ctx->GSSetShader(so, nullptr, 0);
            UINT off = 0;
            ID3D11Buffer* b = soBuf;
            gpu.ctx->SOSetTargets(1, &b, &off);
            gpu.ctx->Draw(kVerts, 0);
            gpu.ctx->SOSetTargets(0, nullptr, nullptr);
            gpu.ctx->CopyResource(soStage, soBuf);
            D3D11_MAPPED_SUBRESOURCE m;
            if (FAILED(gpu.ctx->Map(soStage, 0, D3D11_MAP_READ, 0, &m)))
                return { Result::Skip, "map so" };
            for (uint32_t v = 0; v < kVerts; v++)
                for (const SoField& f : fields)
                    memcpy(&got[v][f.reg][f.comp0], (const uint8_t*)m.pData + v * soStride + f.offset, 4 * f.count);
            gpu.ctx->Unmap(soStage, 0);
        }

        if (probe)
        {
            for (uint32_t v = 0; v < kVerts; v++)
                printf("  probe vec %u: (%g %g %g %g) (%g %g %g %g) (%g %g %g %g) (%g %g %g %g)\n", v, got[v][0][0], got[v][0][1], got[v][0][2],
                    got[v][0][3], got[v][1][0], got[v][1][1], got[v][1][2], got[v][1][3], got[v][2][0], got[v][2][1], got[v][2][2], got[v][2][3],
                    got[v][3][0], got[v][3][1], got[v][3][2], got[v][3][3]);
            return { Result::Skip, "probe" };
        }
        // Compare. With SCDA_SHADER_DIFF_TRACE (and _DUMP), a DIFF writes the interpreter trace of that vector.
        auto traced = [&](uint32_t v, Result r) {
            if (getenv("SCDA_SHADER_DIFF_TRACE") && !dumpPath.empty())
            {
                std::string t;
                XenosInterpInput vin = in.base;
                memcpy(vin.regs, in.regs[v], sizeof vin.regs);
                vin.trace = &t;
                XenosInterpOutput o;
                XenosInterpret(vin, o);
                char hdr[512];
                snprintf(hdr, sizeof hdr, "seed %u vec %u: %s\nregs r0..r3 = (%g %g %g %g) (%g %g %g %g) (%g %g %g %g) (%g %g %g %g)\n", s, v,
                    r.detail.c_str(), vin.regs[0][0], vin.regs[0][1], vin.regs[0][2], vin.regs[0][3], vin.regs[1][0], vin.regs[1][1], vin.regs[1][2],
                    vin.regs[1][3], vin.regs[2][0], vin.regs[2][1], vin.regs[2][2], vin.regs[2][3], vin.regs[3][0], vin.regs[3][1], vin.regs[3][2],
                    vin.regs[3][3]);
                std::ofstream(dumpPath + ".trace.txt") << hdr << t;
            }
            return r;
        };
        for (uint32_t v = 0; v < kVerts; v++)
        {
            char nm[16];
            const Result::Kind kind = ref[v].special ? Result::DiffNumeric : Result::Diff;
            if (pixel && (ref[v].killed || gpuKilled[v]))
            {
                if (ref[v].killed != gpuKilled[v])
                {
                    std::ostringstream os;
                    os << "kill: cpu=" << ref[v].killed << " gpu=" << gpuKilled[v] << " seed=" << s << " vec=" << v;
                    return traced(v, { kind, os.str() });
                }
                continue;
            }
            for (uint32_t e = 0; e < 64; e++)
                for (uint32_t c = 0; c < 4; c++)
                {
                    if (!((scan.exportMask[e] >> c) & 1))
                        continue;
                    // Components the CPU did not export this run read as 0 (the translator zero-inits outputs).
                    const bool written = (ref[v].exportMask[e] >> c) & 1;
                    const float cpu = written ? ref[v].exports[e][c] : 0.0f;
                    const float g = got[v][e][c];
                    if (Close(cpu, g, scan.transcendental))
                        continue;
                    char buf[256];
                    snprintf(buf, sizeof buf, "%s.%c cpu=%.9g gpu=%.9g seed=%u vec=%u%s%s", OutName(pixel, e, nm), "xyzw"[c], cpu, g, s, v,
                        written ? "" : " (cpu: not exported this run)", scan.cube ? " [cube]" : "");
                    return traced(v, { kind, buf });
                }
        }
    }
    return { Result::Pass, "" };
}

std::string Hex(uint64_t h)
{
    char b[20];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)h);
    return b;
}

uint32_t EnvU(const char* name, uint32_t def)
{
    const char* v = getenv(name);
    return v ? uint32_t(strtoul(v, nullptr, 0)) : def;
}
} // namespace

// Hand-assembled shaders through both paths (no corpus needed): must PASS, and a mutated copy must DIFF.
TEST(shader_diff_selftest)
{
    Gpu gpu;
    if (!gpu.Init(getenv("SCDA_SHADER_DIFF_WARP") != nullptr))
    {
        printf("  (skipped: no D3D11 device)\n");
        return;
    }
    using namespace xasm;
    // PS: setp_gt r0.w; (p0) oC0 = r1 * c3 + r2; (!p0) oC0 = tfetch slot 2; cexec b9: oC1 = r0 + c0.
    const uint32_t s = FirstSlot(3);
    auto ps = Build({ Exec(s, 4, 0x4), CondExec(s + 4, 1, 9, true), Exec(0, 0, 0, true) },
        { Alu(0, 0, 0, 29, 0, 0, R(0), R(0), R(0)), Alu(11, 0, 0xF, 50, 0, 0, R(1), C(3), R(2), true, 1), TFetch(4, 0, 2),
            Alu(2, 0, 0xF, 50, 0, 0, R(4), R(4), R(0), true, 0), Alu(0, 1, 0xF, 50, 0, 0, R(0), C(0), R(0), true) });
    // VS: vfetch (patched) r1; oPos = r1 + c0; o0 = loop i0 sum of c[aL] with o0.w = rcp(r1.x); o1.xy = r1.
    const uint32_t vsSlot = FirstSlot(5);
    Ins body = Alu(0, 2, 0xF, 50, 0, 0, R(2), C(4), R(0));
    body.d[1] |= 1u << 31; // c[4 + aL]
    auto vsCode = Build({ Exec(vsSlot, 2, 0x1), LoopStart(4, 0), Exec(vsSlot + 2, 1), LoopEnd(2, 0), Exec(vsSlot + 3, 2, 0, true) },
        { VFetch(1, 0, 0, 95, 0, false, true, 0, 0), Alu(0, 62, 0xF, 50, 0, 0, R(1), C(0), R(0), true), body,
            Alu(2, 0, 0x7, 19, 0, 0x8, R(2), R(2), R(1, 0, 0, 0, 0), true), Alu(2, 1, 0x3, 50, 0, 0, R(1), R(1), R(0), true) });
    for (bool mutate : { false, true })
    {
        Result a = RunShader(gpu, ShaderType::Pixel, ps, 4, mutate, "");
        Result b = RunShader(gpu, ShaderType::Vertex, vsCode, 4, mutate, "");
        printf("  %s: ps %d %s | vs %d %s\n", mutate ? "mutated" : "plain", int(a.kind), a.detail.c_str(), int(b.kind), b.detail.c_str());
        const int want = mutate ? int(Result::Diff) : int(Result::Pass);
        CHECK_EQ(int(a.kind), want);
        CHECK_EQ(int(b.kind), want);
    }
}

// SCDA_SHADER_DIFF=<dir>: the corpus run (see the header comment).
TEST(shader_diff)
{
    const char* dir = getenv("SCDA_SHADER_DIFF");
    if (!dir)
    {
        printf("  (skipped: SCDA_SHADER_DIFF not set)\n");
        return;
    }
    Gpu gpu;
    if (!gpu.Init(getenv("SCDA_SHADER_DIFF_WARP") != nullptr))
    {
        printf("  no D3D11 device\n");
        CHECK(false);
        return;
    }
    const uint32_t seeds = EnvU("SCDA_SHADER_DIFF_SEEDS", 4), limit = EnvU("SCDA_SHADER_DIFF_LIMIT", ~0u);
    const char* filter = getenv("SCDA_SHADER_DIFF_FILTER");
    const bool mutate = getenv("SCDA_SHADER_DIFF_MUTATE") != nullptr, dump = getenv("SCDA_SHADER_DIFF_DUMP") != nullptr;
    const std::filesystem::path reportPath = getenv("SCDA_SHADER_DIFF_REPORT") ? std::filesystem::path(getenv("SCDA_SHADER_DIFF_REPORT"))
                                                                                 : std::filesystem::path(SCDA_ROOT) / "_run" / "shader_diff" / "report.txt";
    std::filesystem::create_directories(reportPath.parent_path());

    std::vector<std::filesystem::path> files;
    for (auto& e : std::filesystem::recursive_directory_iterator(dir))
    {
        const std::string n = e.path().filename().string();
        if (n.ends_with(".vs.bin") || n.ends_with(".ps.bin"))
            files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    std::set<uint64_t> seen;
    std::map<std::string, uint32_t> skipReasons;
    uint32_t counts[4][2] = {}, tested = 0;
    std::ostringstream lines, diffs;
    for (auto& path : files)
    {
        const bool pixel = path.filename().string().ends_with(".ps.bin");
        std::vector<uint8_t> bytes(std::filesystem::file_size(path));
        std::ifstream(path, std::ios::binary).read((char*)bytes.data(), bytes.size());
        if (bytes.size() < 12)
            continue;
        const ShaderType type = pixel ? ShaderType::Pixel : ShaderType::Vertex;
        const ShaderBlob* blob = ShaderCacheInsert(type, (const uint32_t*)bytes.data(), uint32_t(bytes.size() / 4));
        if (!blob || !seen.insert(blob->hash).second)
            continue;
        if (filter && Hex(blob->hash) != filter)
            continue;
        if (const char* st = getenv("SCDA_SHADER_DIFF_STAGE"); st && strcmp(st, pixel ? "ps" : "vs") != 0)
            continue;
        if (++tested > limit)
            break;
        const std::string dumpPath = dump ? (reportPath.parent_path() / (Hex(blob->hash) + (pixel ? ".ps.hlsl" : ".vs.hlsl"))).string() : "";
        const std::vector<uint32_t> ucode = blob->ucode;
        Result r = RunShader(gpu, type, ucode, seeds, mutate, dumpPath);
        counts[r.kind][pixel]++;
        static const char* kKind[] = { "PASS", "DIFF", "DIFF-numeric", "SKIP" };
        const std::string line = std::string(kKind[r.kind]) + " " + Hex(blob->hash) + (pixel ? " ps " : " vs ") + r.detail + "  (" + path.string() + ")";
        lines << line << "\n";
        if (r.kind == Result::Skip)
            skipReasons[r.detail]++;
        if (r.kind == Result::Diff || r.kind == Result::DiffNumeric)
            diffs << "  " << line << "\n";
    }
    std::ostringstream sum;
    sum << "shader diff: device " << gpu.adapter << ", seeds " << seeds << " x " << kVerts << " vectors" << (mutate ? ", MUTATED HLSL" : "") << "\n";
    sum << "  unique shaders tested " << std::min(tested, limit) << "\n";
    const char* names[] = { "PASS", "DIFF", "DIFF-numeric", "SKIP" };
    for (int k = 0; k < 4; k++)
        sum << "  " << names[k] << ": " << counts[k][0] + counts[k][1] << " (vs " << counts[k][0] << ", ps " << counts[k][1] << ")\n";
    std::vector<std::pair<uint32_t, std::string>> sk;
    for (auto& [k, v] : skipReasons)
        sk.push_back({ v, k });
    std::sort(sk.rbegin(), sk.rend());
    sum << "  skip reasons:\n";
    for (auto& [n, why] : sk)
        sum << "    " << n << "  " << why << "\n";
    sum << "  diffs:\n" << diffs.str();
    printf("%s", sum.str().c_str());
    std::ofstream(reportPath) << sum.str() << "\nper shader:\n" << lines.str();
    printf("  report: %s\n", reportPath.string().c_str());
}
