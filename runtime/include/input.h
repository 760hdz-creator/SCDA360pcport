// Controller input for XamInput*: pad 0 (pad.h: XInput, else SDL3), else the keyboard while the game window has focus,
// plus SCDA_AUTOPRESS scripted presses (unattended menu testing).
#pragma once
#include <windows.h>
#include <xinput.h>
#include <cstdint>
#include <vector>

// Writes a guest X_INPUT_STATE (packet number + XINPUT_GAMEPAD, big-endian) at addr.
void StoreInputState(uint32_t addr, uint32_t packet, const XINPUT_GAMEPAD& g);

// Keyboard/mouse buttons -> gamepad: Enter/Space A, Backspace/Ctrl B, E X, Q Y, Esc Start, Tab Back,
// arrows d-pad, WASD left stick, IJKL right stick, Z/right mouse LT, C/left mouse RT, 1/3 shoulders,
// F left stick click, R/middle mouse right stick click (the keyboard prompts in prompts.cpp show these).
XINPUT_GAMEPAD KeyboardGamepad(bool (*down)(int vk));
// Issue #35: [Input] WheelMoveSpeed=1 (default): in gameplay the mouse wheel picks how far the movement
// keys push the left stick (Sam's speed follows the stick, like the PC version's wheel). Levels
// kWheelMoveLevels; wheel up = faster. Starts at full speed. Walk still scales it by WalkSpeed.
// The title zeroes the stick inside its own dead zone (SplinterCell4User.ini JoyX/JoyY DeadZone=0.3), so the
// levels are the PC version's 25/50/75/100% steps placed above it (0.3 + 0.7 * step), and no keyboard move
// ever lands inside it (kMinKeyboardMove): the old 0.3 bottom step stopped Sam instead of sneaking (#35).
// The levels apply only while Sam walks (WheelSpeedStateOn); menus, keypads and minigames get full deflection (#38).
constexpr float kStickDeadZone = 0.3f;
constexpr float kWheelMoveLevels[] = { 0.475f, 0.65f, 0.825f, 1.0f }; // 4 steps like the PC version (#35)
constexpr float kMinKeyboardMove = 0.35f;
int WheelMoveLevelStep(int level, int wheel); // clamps to the table
// Player controller script states where Sam walks under the movement keys (s_PlayerWalking, targeting,
// against a wall, swimming, carrying): only these use the wheel level. fov.cpp checks the state and
// Level.Pauser each engine tick and calls SetWheelSpeedState; input polls read it.
bool WheelSpeedState(const char* stateName);
void SetWheelSpeedState(bool on);
bool WheelSpeedStateOn();
void SetKeyboardMoveScale(float scale);       // KeyboardGamepad stick scale (1 = full), floored at kMinKeyboardMove

// Mouse look. The window thread adds raw motion and wheel notches; each XamInputGetState poll
// takes them (MouseTake zeroes the totals) and turns the motion into the right stick: full
// deflection at 50 / sensitivity pixels, +dy (down) pulls the stick down unless inverted.
void MouseAccumulate(int dx, int dy, int wheel);
void MouseTake(int& dx, int& dy, int& wheel);
void MouseToStick(int dx, int dy, float sensitivity, bool invertY, int16_t& rx, int16_t& ry);
// [Input] MouseMode=Direct (default): mouse motion goes to the player controller's aMouseX/aMouseY,
// the engine's own PC mouse path (issue #29: linear, no dead zone, no full-tilt boost), instead of
// the right stick. The script turns yaw by 40000 per unit aMouseX and pitch by 25000 per unit
// aMouseY (positive = down) each tick, not scaled by frame time. kMouseUnitsPerCount rotation units
// (65536 = 360 deg) per count at sensitivity 1, i.e. ~0.066 deg.
constexpr float kMouseUnitsPerCount = 12.0f;
void MouseToLook(int dx, int dy, float sensitivity, bool invertY, float& aMouseX, float& aMouseY);
// The tick side (fov.cpp) marks the direct path live each engine tick it can write; the poll side
// then feeds MouseLookAdd instead of the stick. MouseLookTake drains the total for one tick.
void MouseLookSetLive(bool live);
bool MouseLookLive();
void MouseLookAdd(float aMouseX, float aMouseY);
void MouseLookTake(float& aMouseX, float& aMouseY);

// The F1 menu is open: the title sees an idle pad (no pad, keyboard or mouse input reaches it).
void SetInputBlocked(bool blocked);
bool InputBlocked();
// Menu shortcut hygiene: L3+R3 held together never reaches the title (it toggles the menu), and
// after the menu closes (waitRelease) the title sees an idle pad until every button and Esc are released
// (the Start, Back, L3+R3 or Esc that closed it must not reach the game).
void FilterMenuShortcut(XINPUT_GAMEPAD& g, bool escapeDown, bool& waitRelease);

// A pad stick at rest inside the XInput deadzone (pads never rest at exactly 0): keyboard and mouse
// may drive that stick instead.
bool StickIdle(int16_t x, int16_t y, int deadzone);

struct AutoPress
{
    uint32_t frame;   // guest frame number (as SCDA_DUMP_FRAME counts)
    uint16_t buttons; // XINPUT_GAMEPAD_* bits
    uint32_t frames;  // frames held
    int axis;         // -1 none; 0-3 LX LY RX RY; 4-5 LT RT
    float value;      // axis value: sticks [-1, 1], triggers [0, 1]
};
// "<frame>:<name>[=<value>][:<frames>],..." with buttons A B X Y START BACK UP DOWN LEFT RIGHT LB RB,
// sticks LX LY RX RY (value required, [-1, 1]) and triggers LT RT ([0, 1]). Each entry is held for
// <frames> frames (default kAutoPressFrames). Bad entries are skipped.
constexpr uint32_t kAutoPressFrames = 4;
std::vector<AutoPress> ParseAutoPress(const char* text);
// Applies the script at a frame: buttons are ORed in, held axes replace the pad's.
void AutoPressApply(const std::vector<AutoPress>& script, uint32_t frame, XINPUT_GAMEPAD& g);
// Buttons the script holds at a frame.
uint16_t AutoPressButtons(const std::vector<AutoPress>& script, uint32_t frame);
