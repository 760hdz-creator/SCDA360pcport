// User settings (%LOCALAPPDATA%\SCDARecomp\settings.ini).
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

struct DisplaySettings
{
    uint32_t scale = 1; // internal resolution = 1280x720 * scale (1..4)
    enum Mode { Windowed, Borderless, Fullscreen } mode = Windowed;
    bool vsync = true;
    uint32_t anisotropy = 1; // minimum anisotropic filtering for linear-filtered textures (1 = off .. 16)
    // Quality of life, off by default (authentic): goggle vision modes without the scanline overlay
    // and without the thermal/EMF noise displacement ([Display] CleanGoggles=1).
    bool cleanGoggles = false;
    // Opaque surfaces drawn from both sides ([Display] DrawBackFaces=1). Default off = the console's
    // culling: the Iceland shack holes were the BSP index-offset bug, and drawing both sides put the
    // back of the mirror wall into the Hotel bathroom mirror's reflection pass.
    bool drawBackFaces = false;
    // Dynamic (shadow-mapped) shadows ([Display] DynamicShadows=0 turns them off; lights stay on).
    bool dynamicShadows = true;
    // Field of view in degrees ([Display] FOV=60..110). 75 is the game's own (nothing is changed).
    uint32_t fov = 75;
    // Post-processing on the presented frame (F1 menu, Graphics). Defaults leave the image as the
    // console made it. The game's own 4x MSAA is always on.
    enum AntiAliasing { AaOff, AaFxaa } antiAliasing = AaOff; // [Display] AntiAliasing=Off|FXAA
    uint32_t sharpening = 0;                                    // [Display] Sharpening=0..100 (percent)
    // [Display] Upscaling=Off|Native|Quality|Balanced|Performance|UltraPerformance: Off uses Scale;
    // the others pick Scale from the window height (UpscalingScale) and upscale to the window.
    enum Upscaling { UpOff, UpNative, UpQuality, UpBalanced, UpPerformance, UpUltraPerformance } upscaling = UpOff;
    // Display calibration, 1 = as the console ([Display] Gamma=0.5..2.0, Brightness=0.5..2.0).
    float gamma = 1.0f;
    float brightness = 1.0f;
    // [Display] PerformanceOverlay=Off|FPS|Detailed: corner overlay (fps; + frame time, CPU, resolution).
    enum Overlay { OverlayOff, OverlayFps, OverlayDetailed } performanceOverlay = OverlayOff;
    uint32_t monitor = 0; // [Display] Monitor=0..7: 0 primary, then the other displays (next start)
};

// Internal resolution scale for an upscaling mode: the window height divided by the mode's ratio
// (Native 1, Quality 1.5, Balanced 1.7, Performance 2, Ultra Performance 3), in 720-row steps,
// rounded, 1..4. UpOff returns manualScale.
uint32_t UpscalingScale(DisplaySettings::Upscaling mode, uint32_t windowHeight, uint32_t manualScale);
const char* UpscalingName(DisplaySettings::Upscaling mode);     // ini value ("UltraPerformance")
const char* AntiAliasingName(DisplaySettings::AntiAliasing aa); // ini value ("FXAA")

struct InputSettings
{
    bool mouseLook = true;         // raw mouse motion drives the right stick while the window has focus
    float mouseSensitivity = 1.0f; // full stick deflection at 50 / sensitivity pixels per poll (0.1..10)
    bool mouseInvertY = false;
    bool mouseDirect = true;       // [Input] MouseMode=Direct|Stick (input.h MouseToLook; Stick = old right-stick mapping)
    bool sdlGamepads = true;       // [Input] PlayStationNative: SDL3 gamepads (PlayStation, Switch) when no XInput pad
    std::string buttonPrompts = "Auto"; // [Input] ButtonPrompts: Auto|Xbox|PlayStation|Keyboard (prompts.h)
    bool gamepad = true;              // [Input] Gamepad=0: ignore controllers (keyboard and mouse only)
    float gamepadSensitivity = 1.0f;  // [Input] GamepadSensitivity=0.25..3: right stick (camera) scale
    float walkSpeed = 0.5f;           // [Input] WalkSpeed=0.1..1: left stick deflection while [Keys] Walk is held
    bool wheelMoveSpeed = true;       // [Input] WheelMoveSpeed=0: wheel is d-pad up/down in gameplay too (input.h)
};

struct UpdateSettings
{
    bool checkOnStart = true; // [Update] CheckOnStart: ask GitLab for the latest release at startup
    bool autoInstall = false; // [Update] AutoInstall: install a newer release found at startup (restarts the game)
    std::string repo;         // [Update] Repo=owner/name: overrides the build's repository (testing)
};

DisplaySettings LoadDisplaySettings(const std::filesystem::path& ini); // defaults if missing/invalid
InputSettings LoadInputSettings(const std::filesystem::path& ini);     // [Input] MouseLook, MouseSensitivity, MouseInvertY, PlayStationNative, ButtonPrompts
UpdateSettings LoadUpdateSettings(const std::filesystem::path& ini);
// [General] Language=English|French|German|Spanish|Italian (or the X_LANGUAGE number):
// the console language the title is told (XGetLanguage, XCONFIG_USER_LANGUAGE). Default English (1).
// Applies on the next start. Env SCDA_LANGUAGE overrides (tests).
uint32_t GameLanguage();
const char* GameLanguageName(uint32_t id); // "English" .. ; nullptr if not one the disc can carry
// Writes key=value into [section], replacing the key's line (any case) or adding it (and the
// section). Other lines are kept as they are. Written to a temporary file, then renamed over.
bool SaveSetting(const std::filesystem::path& ini, const std::string& section, const std::string& key, const std::string& value);
std::filesystem::path SettingsPath();
// Every key=value line of [section] (both lowercased), in file order (e.g. [Keys], keybinds.h).
std::vector<std::pair<std::string, std::string>> LoadSettingsSection(const std::filesystem::path& ini, const std::string& section);
