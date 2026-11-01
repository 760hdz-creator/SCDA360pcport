// Light / shadow draw distance (GitHub issue #1). The title fades lights and their shadows out by
// distance through three per-light script properties (STATE.md, found via GObjects + reflection):
//   Light +0x5BC TurnOffDistance, +0x5C4 ShadowsTurnOffStart, +0x5C8 ShadowsTurnOffDistance.
// The PC widescreen fix lifts the same cut-off by forcing GetShadowTurnOffRatio to 4. Here: every
// engine tick, each Light's non-zero distances are scaled by the user factor (written back only when
// the game changed the value, so scripts that rewrite them are scaled again, never compounded).
// GObjects = TArray<UObject*> at 0x82E1F994 (UObject +4 = index). Called from fov.cpp FovTick.
#include <runtime.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "gpu/draw_state.h"

std::string GuestFNameOf(uint32_t index); // fov.cpp

namespace {

constexpr uint32_t kGObjects = 0x82E1F994;
constexpr uint32_t kFields[3] = { 0x5BC, 0x5C4, 0x5C8 };

std::atomic<float> g_scale{ 1.0f };

struct LightEntry
{
    uint32_t obj, cls;
    uint32_t written[3]; // bits last written (0 = none)
};
std::vector<LightEntry> g_lights;
std::unordered_map<uint32_t, bool> g_isLight; // class -> IsA Light
uint32_t g_controller = 0, g_ticks = 0;
float g_appliedScale = 1.0f;

std::string NameOf(uint32_t obj) { return obj ? GuestFNameOf(GuestLoad32(obj + 0x20)) : ""; }

bool ClassIsLight(uint32_t cls)
{
    auto it = g_isLight.find(cls);
    if (it != g_isLight.end())
        return it->second;
    bool light = false;
    for (uint32_t c = cls, d = 0; c && d < 24 && !light; c = GuestLoad32(c + 0x2C), d++)
        light = NameOf(c) == "Light";
    return g_isLight.emplace(cls, light).first->second;
}

void Scan()
{
    // Keep the written bits of lights already known, so a rescan never re-scales our own values.
    std::unordered_map<uint32_t, LightEntry> known;
    for (const LightEntry& l : g_lights)
        known.emplace(l.obj, l);
    g_lights.clear();
    const uint32_t data = GuestLoad32(kGObjects), num = GuestLoad32(kGObjects + 4);
    if (!data || num > 4000000 || !GuestRangeReadable(g_base + data, size_t(num) * 4))
        return;
    std::unordered_map<uint32_t, bool> pageOk; // 64 KB page -> readable (one VirtualQuery per page, not per object)
    auto readable = [&](uint32_t a, uint32_t n) {
        for (uint32_t p = a >> 16; p <= (a + n - 1) >> 16; p++)
        {
            auto it = pageOk.find(p);
            if (it == pageOk.end())
                it = pageOk.emplace(p, GuestRangeReadable(g_base + (uint64_t(p) << 16), 0x10000)).first;
            if (!it->second)
                return false;
        }
        return true;
    };
    for (uint32_t i = 0; i < num; i++)
    {
        const uint32_t o = GuestLoad32(data + i * 4);
        if (!o || !readable(o, 0x600))
            continue;
        const uint32_t cls = GuestLoad32(o + 0x24);
        if (!cls || !ClassIsLight(cls))
            continue;
        auto it = known.find(o);
        g_lights.push_back(it != known.end() && it->second.cls == cls ? it->second : LightEntry{ o, cls, { 0, 0, 0 } });
    }
    static const bool trace = getenv("SCDA_TRACE_LIGHTDIST") != nullptr;
    if (known.size() == g_lights.size() && !trace)
        return;
    RuntimeLog("[lightdist] %zu lights, scale %g\n", g_lights.size(), double(g_scale.load()));
    if (trace)
        for (const LightEntry& l : g_lights)
        {
            float v[3];
            for (int f = 0; f < 3; f++)
            {
                const uint32_t bits = GuestLoad32(l.obj + kFields[f]);
                memcpy(&v[f], &bits, 4);
            }
            RuntimeLog("[lightdist] %08X %s turnoff %g shadows %g..%g\n", l.obj, NameOf(l.obj).c_str(), v[0], v[1], v[2]);
        }
}

} // namespace

void SetLightDistanceScale(float scale) { g_scale = scale < 1.0f ? 1.0f : scale; }

void LightDistanceTick(uint32_t controller)
{
    static const char* env = getenv("SCDA_LIGHT_DIST_SCALE");
    if (env)
        g_scale = float(atof(env));
    const float scale = g_scale;
    if (controller != g_controller)
    {
        g_controller = controller;
        g_lights.clear();
        g_isLight.clear();
        g_ticks = 0;
    }
    if (!controller || (scale == 1.0f && g_appliedScale == 1.0f))
        return;
    // Rescan once the level has settled, then every ~10 s (spawned lights).
    if (g_ticks++ % 600 == 30)
        Scan();
    const bool rescale = scale != g_appliedScale;
    for (LightEntry& l : g_lights)
    {
        if (GuestLoad32(l.obj + 0x24) != l.cls)
            continue;
        for (int f = 0; f < 3; f++)
        {
            const uint32_t addr = l.obj + kFields[f], bits = GuestLoad32(addr);
            float v, base;
            memcpy(&v, &bits, 4);
            if (l.written[f] && bits == l.written[f])
            {
                if (!rescale)
                    continue;
                base = v / g_appliedScale; // undo our previous factor
            }
            else
                base = v; // fresh value from the game
            if (!(base > 0.0f))
                continue;
            const float out = base * scale;
            uint32_t ob;
            memcpy(&ob, &out, 4);
            GuestStore32(addr, ob);
            l.written[f] = ob;
        }
    }
    g_appliedScale = scale;
}
