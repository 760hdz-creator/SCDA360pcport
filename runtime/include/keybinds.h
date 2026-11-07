// Keyboard/mouse -> gamepad bindings ([Keys] in settings.ini, issue #1).
// Each action takes one or more keys, comma-separated: "A=Enter,Space". Key names: A-Z, 0-9,
// F2-F12 (F1 opens the menu), Space Enter Escape Tab Backspace Shift LShift RShift Ctrl LCtrl RCtrl
// Alt LAlt RAlt CapsLock Up Down Left Right Insert Delete Home End PageUp PageDown Numpad0-Numpad9
// Mouse1 (left) Mouse2 (right) Mouse3 (middle) Mouse4 Mouse5, None (unbound). Unknown names are
// ignored; an action left out keeps its default.
// Actions: A B X Y Start Back Up Down Left Right LB RB LS RS LT RT (pad buttons, d-pad, triggers),
// MoveForward MoveBack MoveLeft MoveRight (left stick), LookUp LookDown LookLeft LookRight (right
// stick), Walk (held: the left stick at [Input] WalkSpeed, 0.1..1, default 0.5 = sneaking pace).
#pragma once
#include <string>
#include <utility>
#include <vector>

enum class KeyAction
{
    A, B, X, Y, Start, Back, Up, Down, Left, Right, LB, RB, LS, RS, LT, RT,
    MoveForward, MoveBack, MoveLeft, MoveRight, LookUp, LookDown, LookLeft, LookRight, Walk,
    Count
};

struct KeyBindings
{
    std::vector<int> keys[int(KeyAction::Count)]; // virtual-key codes per action
    float walkSpeed = 0.5f;
};

KeyBindings DefaultKeyBindings();
// Defaults overridden by [Keys] key=value pairs (lowercased, as settings.cpp reads them).
KeyBindings ParseKeyBindings(const std::vector<std::pair<std::string, std::string>>& keys, float walkSpeed);
int KeyCodeFromName(const std::string& lowercaseName); // 0 when unknown ("none" too)
const char* KeyActionName(KeyAction action);
// The bindings KeyboardGamepad uses (defaults until set).
void SetKeyBindings(const KeyBindings& bindings);
const KeyBindings& CurrentKeyBindings();
