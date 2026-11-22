// Reference CPU interpreter for raw Xenos shader microcode (test oracle for the HLSL translator).
// Written independently of gpu/xenos/translate.cpp + shader_recompiler.cpp: instruction layouts and
// semantics follow Xenia's ucode.h / shader_translator.cc / shader_interpreter.cc (BSD).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

struct XenosInterpInput
{
    bool pixel = false;
    const uint32_t* ucode = nullptr; // host-order dwords
    size_t dwords = 0;
    float consts[256][4] = {};    // this stage's ALU constants, as the microcode indexes them
    uint32_t bools[8] = {};       // 256 bool constants (bit n of word n/32)
    uint32_t loops[32] = {};      // loop constants: count | start << 8 | step << 16 (signed byte)
    uint32_t fetch[192] = {};     // vertex fetch constants: 2 dwords per slot (96 slots)
    const uint8_t* memory = nullptr; // guest physical memory (as the GPU sees it, guest byte order)
    size_t memorySize = 0;
    float tex[32][4] = {};        // tfetch result per fetch constant slot (textures are not sampled)
    float regs[16][4] = {};       // initial r0..r15 (VS: r0.x = vertex index; PS: interpolators)
    std::string* trace = nullptr; // optional: one line per control flow / executed instruction
};

struct XenosInterpOutput
{
    float exports[64][4] = {};
    uint8_t exportMask[64] = {}; // components written per export register
    bool killed = false;
    // A NaN/Inf was produced from finite operands (rsq/log/sqrt domain, rcp(0), overflow): hosts may
    // legitimately disagree on such values (D3D9 0*Inf, rsq(|x|) conventions). Also set for relative
    // constant reads outside c0..c255.
    bool special = false;
    uint32_t executed = 0; // ALU + fetch instructions run
    std::string skip;      // non-empty: shader uses something the interpreter does not model
};

// Runs the shader once. Returns false (with out.skip set) for unsupported features.
bool XenosInterpret(const XenosInterpInput& in, XenosInterpOutput& out);

// Vertex formats the interpreter decodes (Xenia VertexFormat values).
bool XenosInterpVertexFormatSupported(uint32_t format);
uint32_t XenosInterpVertexFormatComponents(uint32_t format);
