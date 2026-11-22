// Field of view override (F1 menu): the player controller's FOV properties, found through UE2
// reflection on the controller object seen by the script-event thunk (travel_trace.cpp hook of
// sub_8258E908). Layout (STATE.md): UObject +0x20 name, +0x24 class; UField +0x2C super, +0x30 next;
// UStruct +0x40 children; UProperty +0x48 offset (high 16 bits).
#include <windows.h>
#include <runtime.h>
#include <fov.h>
#include <input.h>
#include "gpu/draw_state.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

// GNames = TArray<FNameEntry*> at 0x82F284D4; entry +12 = name (ANSI, or UTF-16 BE when the first byte is 0).
std::string GuestFNameOf(uint32_t index)
{
    const uint32_t data = GuestLoad32(0x82F284D4);
    const uint32_t num = GuestLoad32(0x82F284D8);
    if (!data || index >= num)
        return "?";
    const uint32_t entry = GuestLoad32(data + index * 4);
    if (!entry)
        return "?";
    std::string s;
    const bool wide = *(g_base + entry + 12) == 0;
    for (uint32_t i = 0; i < 64; i++)
    {
        const uint32_t c = wide ? GuestLoad16(entry + 12 + i * 2) : *(g_base + entry + 12 + i);
        if (!c)
            break;
        s += c < 0x80 ? char(c) : '?';
    }
    return s;
}

namespace {

std::string NameOf(uint32_t obj) { return obj ? GuestFNameOf(GuestLoad32(obj + 0x20)) : ""; }
std::string ClassName(uint32_t obj) { return obj ? NameOf(GuestLoad32(obj + 0x24)) : ""; }

// Byte offset of a property by name in a class or its supers, 0 when absent.
uint32_t PropertyOffset(uint32_t cls, const char* name)
{
    for (uint32_t depth = 0; cls && depth < 16; cls = GuestLoad32(cls + 0x2C), depth++)
        for (uint32_t f = GuestLoad32(cls + 0x40), n = 0; f && n < 4000; f = GuestLoad32(f + 0x30), n++)
            if (NameOf(f) == name)
                return GuestLoad32(f + 0x48) >> 16;
    return 0;
}

uint32_t g_controller, g_controllerClass;
float g_gameDefault = 0; // the controller's DefaultFOV before any override (75)
std::atomic<float> g_userFov{ 0.0f };
std::unordered_map<uint32_t, bool> g_classIsController; // class object -> name contains PlayerController
struct Offsets
{
    uint32_t samCamera = 0, desired = 0, def = 0, angle = 0;
} g_off;

float LoadFloat(uint32_t a)
{
    const uint32_t bits = GuestLoad32(a);
    float v;
    memcpy(&v, &bits, 4);
    return v;
}
void StoreFloat(uint32_t a, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, 4);
    GuestStore32(a, bits);
}

} // namespace

void SetFovOverride(float degrees) { g_userFov = degrees; }

void FovReset()
{
    g_controller = g_controllerClass = 0;
    g_gameDefault = 0;
    g_classIsController.clear();
    g_off = {};
}

void FovSeeObject(uint32_t obj)
{
    if (!obj)
        return;
    const uint32_t cls = GuestLoad32(obj + 0x24);
    if (!cls)
        return;
    if (cls == g_controllerClass)
    {
        g_controller = obj;
        return;
    }
    auto it = g_classIsController.find(cls);
    if (it == g_classIsController.end())
        it = g_classIsController.emplace(cls, NameOf(cls).find("PlayerController") != std::string::npos).first;
    if (!it->second)
        return;
    g_controller = obj;
    g_controllerClass = cls;
    g_off.samCamera = PropertyOffset(cls, "m_samCameraFOV");
    g_off.desired = PropertyOffset(cls, "DesiredFOV");
    g_off.def = PropertyOffset(cls, "DefaultFOV");
    g_off.angle = PropertyOffset(cls, "FovAngle");
    if (g_off.def && !g_gameDefault)
        g_gameDefault = LoadFloat(obj + g_off.def);
    RuntimeLog("[fov] controller %08X %s: m_samCameraFOV +%X %g, DesiredFOV +%X %g, DefaultFOV +%X %g, FovAngle +%X %g\n", obj,
        NameOf(cls).c_str(), g_off.samCamera, LoadFloat(obj + g_off.samCamera), g_off.desired, LoadFloat(obj + g_off.desired), g_off.def,
        LoadFloat(obj + g_off.def), g_off.angle, LoadFloat(obj + g_off.angle));
}

// Engine tick (travel_trace.cpp sub_825C6C68 hook). The view follows FovAngle, which the scripts ease
// toward DesiredFOV; zooms set DesiredFOV and ResetFOV restores DefaultFOV. So: DefaultFOV = the
// user's value, and DesiredFOV too while it sits at a default (not zoomed). The game's own value
// (75) writes nothing.
// SCDA_FOV_TEST=<sam|desired|angle>:<degrees>: write that property every tick instead (diagnostic).

void FovTick()
{
    // Reflection dumps, once a controller exists (finding script properties without the packages):
    // SCDA_DUMP_NAMES=<sub>[,<sub>..] logs every GNames entry containing a substring (case-insensitive);
    // SCDA_DUMP_PROPS=<class>[,<class>..] logs each class's own properties + byte offsets, found
    // through GObjects (TArray<UObject*> at 0x82E1F994).
    static const char* dumpNames = getenv("SCDA_DUMP_NAMES");
    static const char* dumpProps = getenv("SCDA_DUMP_PROPS");
    static bool dumped = false;
    if ((dumpNames || dumpProps) && !dumped && g_controller)
    {
        dumped = true;
        auto lower = [](std::string s) {
            for (char& c : s)
                c = char(tolower(uint8_t(c)));
            return s;
        };
        auto matches = [](const std::string& list, const std::string& s, bool substring) {
            for (size_t p = 0; p < list.size();)
            {
                const size_t e = list.find(',', p);
                const std::string item = list.substr(p, e == std::string::npos ? std::string::npos : e - p);
                if (!item.empty() && (substring ? s.find(item) != std::string::npos : s == item))
                    return true;
                p = e == std::string::npos ? list.size() : e + 1;
            }
            return false;
        };
        if (dumpNames)
            for (uint32_t i = 0, n = GuestLoad32(0x82F284D8); i < n; i++)
                if (const std::string name = GuestFNameOf(i); matches(lower(dumpNames), lower(name), true))
                    RuntimeLog("[names] %u %s\n", i, name.c_str());
        const uint32_t data = GuestLoad32(0x82E1F994), num = GuestLoad32(0x82E1F998);
        if (dumpProps && data && num < 4000000 && GuestRangeReadable(g_base + data, size_t(num) * 4))
            for (uint32_t i = 0; i < num; i++)
            {
                const uint32_t o = GuestLoad32(data + i * 4);
                if (!o || !GuestRangeReadable(g_base + o, 0x50) || ClassName(o) != "Class" || !matches(dumpProps, NameOf(o), false))
                    continue;
                for (uint32_t f = GuestLoad32(o + 0x40), n = 0; f && n < 4000; f = GuestLoad32(f + 0x30), n++)
                    RuntimeLog("[props] %s.%s %s +%X\n", NameOf(o).c_str(), NameOf(f).c_str(), ClassName(f).c_str(), GuestLoad32(f + 0x48) >> 16);
            }
    }
    if (!g_controller)
    {
        MouseLookSetLive(false);
        SetWheelSpeedState(false);
        return;
    }
    // Wheel move speed gate (input.h WheelSpeedState, issues #35/#38): the player controller's script state
    // is the UState at StateFrame (UObject +0xC) +4; the pause menu sets Level.Pauser. Keypads, locks,
    // computers, menus and every other non-walking state get full keyboard deflection.
    // SCDA_TRACE_STATE=1 logs each change.
    {
        static uint32_t cls = 0, levelOff = 0, pauserOff = 0, lastState = 0;
        static std::string stateName;
        static const bool traceState = getenv("SCDA_TRACE_STATE") != nullptr;
        if (cls != g_controllerClass)
            cls = g_controllerClass, levelOff = PropertyOffset(cls, "Level"), pauserOff = 0, lastState = 0;
        const uint32_t level = levelOff ? GuestLoad32(g_controller + levelOff) : 0;
        if (level && !pauserOff && GuestRangeReadable(g_base + level, 0x50))
            pauserOff = PropertyOffset(GuestLoad32(level + 0x24), "Pauser");
        const bool paused = level && pauserOff && GuestLoad32(level + pauserOff);
        const uint32_t frame = GuestLoad32(g_controller + 0xC);
        uint32_t state = 0;
        if (frame && GuestRangeReadable(g_base + frame, 8))
            state = GuestLoad32(frame + 4);
        if (state != lastState)
        {
            lastState = state;
            stateName = state && GuestRangeReadable(g_base + state, 0x50) && ClassName(state) == "State" ? NameOf(state) : "";
        }
        const bool moving = !paused && WheelSpeedState(stateName.c_str());
        static bool lastPaused = false;
        static std::string lastName;
        if (traceState && (stateName != lastName || paused != lastPaused))
            RuntimeLog("[state] %s%s wheel speed %d\n", stateName.empty() ? "?" : stateName.c_str(), paused ? " (paused)" : "", int(moving));
        lastName = stateName, lastPaused = paused;
        SetWheelSpeedState(moving);
    }
    // Mouse look (input.h MouseToLook): every tick, this tick's mouse total into aMouseX/aMouseY (0 when
    // idle, so a value never lingers). Only while the controller object still has its class (not stale
    // across a level change).
    {
        static uint32_t cls = 0, offX = 0, offY = 0;
        static const bool testMouse = getenv("SCDA_TEST_MOUSEX") != nullptr;
        // States that do not turn the view from aMouseX (briefings, menus, and any right-stick minigame
        // such as a lock) fall back to the stick mapping: 6 ticks of yaw writes with no yaw change switch
        // to the stick, any yaw change (stick, pad, script) switches back. A gap of over 100 ms since the
        // last tick (load hitch) drops the pending total instead of turning it all at once.
        static uint32_t rotOff = 0, lastYaw = 0;
        static int stuck = 0;
        static bool fallback = false;
        static uint64_t lastMs = 0;
        static float lastX = 0;
        if (cls != g_controllerClass)
            cls = g_controllerClass, offX = PropertyOffset(cls, "aMouseX"), offY = PropertyOffset(cls, "aMouseY"),
            rotOff = PropertyOffset(cls, "Rotation"), stuck = 0, fallback = false;
        const bool valid = offX && offY && rotOff && GuestLoad32(g_controller + 0x24) == g_controllerClass && !testMouse;
        const uint32_t yaw = valid ? GuestLoad32(g_controller + rotOff + 4) : 0;
        const bool yawMoved = yaw != lastYaw;
        lastYaw = yaw;
        if (fallback && yawMoved)
        {
            fallback = false, stuck = 0;
            RuntimeLog("[mouse] view turns again: direct mouse look\n");
        }
        const uint64_t now = GetTickCount64();
        const bool gap = now - lastMs > 100;
        lastMs = now;
        MouseLookSetLive(valid && !fallback);
        if (valid)
        {
            float x = 0, y = 0;
            MouseLookTake(x, y);
            if (gap || fallback)
                x = y = 0;
            stuck = lastX != 0 && !yawMoved ? stuck + 1 : 0; // last tick's write had its chance to turn
            lastX = x;
            if (stuck >= 6)
            {
                fallback = true, x = y = 0, lastX = 0;
                RuntimeLog("[mouse] view not turning: mouse drives the right stick\n");
            }
            StoreFloat(g_controller + offX, x);
            StoreFloat(g_controller + offY, y);
        }
    }
    // SCDA_TRACE_ROT=1: per tick, the controller's Rotation (yaw/pitch, 65536 = 360 deg) and its input
    // axes (issue #29: how the game turns stick deflection into view rotation).
    static const bool traceRot = getenv("SCDA_TRACE_ROT") != nullptr;
    if (traceRot)
    {
        static uint32_t cls = 0, rot = 0, turn = 0, baseX = 0, mouseX = 0, mouseY = 0, lookUp = 0;
        if (cls != g_controllerClass)
        {
            cls = g_controllerClass;
            rot = PropertyOffset(cls, "Rotation"), turn = PropertyOffset(cls, "aTurn"), baseX = PropertyOffset(cls, "aBaseX");
            mouseX = PropertyOffset(cls, "aMouseX"), mouseY = PropertyOffset(cls, "aMouseY"), lookUp = PropertyOffset(cls, "aLookUp");
            RuntimeLog("[rot] offsets Rotation +%X aTurn +%X aBaseX +%X aMouseX +%X aLookUp +%X\n", rot, turn, baseX, mouseX, lookUp);
        }
        auto f = [&](uint32_t o) { return o ? LoadFloat(g_controller + o) : 0.0f; };
        // SCDA_TEST_MOUSEX=<v> / SCDA_TEST_YAW=<units>: write aMouseX / add to yaw every tick (does it stick?).
        static const char* tmx = getenv("SCDA_TEST_MOUSEX");
        static const char* tyaw = getenv("SCDA_TEST_YAW");
        static int tick = 0;
        tick++;
        // SCDA_TEST_STALL=<every>:<ms>: sleep in the tick every N ticks (is the mouse scaled by DeltaTime?).
        static int stallEvery = 0, stallMs = 0;
        static const bool stallParsed = getenv("SCDA_TEST_STALL") && sscanf(getenv("SCDA_TEST_STALL"), "%d:%d", &stallEvery, &stallMs) == 2;
        if (stallParsed && stallEvery > 0 && tick % stallEvery == 0)
            Sleep(DWORD(stallMs));
        if (tmx && mouseX && mouseY)
        {
            // <x>,<y>[,<on ticks>,<off ticks>]
            float x = 0, y = 0;
            int on = 1, off = 0;
            sscanf(tmx, "%f,%f,%d,%d", &x, &y, &on, &off);
            const bool active = off <= 0 || tick % (on + off) < on;
            StoreFloat(g_controller + mouseX, active ? x : 0.0f);
            StoreFloat(g_controller + mouseY, active ? y : 0.0f);
        }
        if (tyaw && rot)
            GuestStore32(g_controller + rot + 4, GuestLoad32(g_controller + rot + 4) + uint32_t(atoi(tyaw)));
        if (rot)
            RuntimeLog("[rot] pitch %d yaw %d aTurn %g aBaseX %g aMouseX %g aMouseY %g aLookUp %g\n", int32_t(GuestLoad32(g_controller + rot)),
                int32_t(GuestLoad32(g_controller + rot + 4)), f(turn), f(baseX), f(mouseX), f(mouseY), f(lookUp));
    }
    static const char* test = getenv("SCDA_FOV_TEST");
    if (!test)
    {
        const float user = g_userFov;
        if (!g_off.def || !g_off.desired || !g_gameDefault || user < 1.0f)
            return;
        const float def = LoadFloat(g_controller + g_off.def), desired = LoadFloat(g_controller + g_off.desired);
        if (user == g_gameDefault && def == g_gameDefault)
            return;
        if (desired == def || desired == g_gameDefault)
            StoreFloat(g_controller + g_off.desired, user);
        StoreFloat(g_controller + g_off.def, user);
        return;
    }
    const char* colon = strchr(test, ':');
    const float v = colon ? float(atof(colon + 1)) : 90.0f;
    const std::string which(test, colon ? size_t(colon - test) : strlen(test));
    if (which == "sam" && g_off.samCamera)
        StoreFloat(g_controller + g_off.samCamera, v);
    else if (which == "desired" && g_off.desired && g_off.def)
    {
        StoreFloat(g_controller + g_off.desired, v);
        StoreFloat(g_controller + g_off.def, v);
    }
    else if (which == "angle" && g_off.angle)
        StoreFloat(g_controller + g_off.angle, v);
}
