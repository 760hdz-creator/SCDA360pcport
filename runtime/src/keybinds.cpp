#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <keybinds.h>

namespace {

const char* const kActionNames[] = {
    "A", "B", "X", "Y", "Start", "Back", "Up", "Down", "Left", "Right", "LB", "RB", "LS", "RS", "LT", "RT",
    "MoveForward", "MoveBack", "MoveLeft", "MoveRight", "LookUp", "LookDown", "LookLeft", "LookRight", "Walk",
};
static_assert(sizeof(kActionNames) / sizeof(kActionNames[0]) == size_t(KeyAction::Count));

const struct { const char* name; int vk; } kKeyNames[] = {
    { "space", VK_SPACE }, { "enter", VK_RETURN }, { "return", VK_RETURN }, { "escape", VK_ESCAPE }, { "esc", VK_ESCAPE },
    { "tab", VK_TAB }, { "backspace", VK_BACK }, { "shift", VK_SHIFT }, { "lshift", VK_LSHIFT }, { "rshift", VK_RSHIFT },
    { "ctrl", VK_CONTROL }, { "control", VK_CONTROL }, { "lctrl", VK_LCONTROL }, { "rctrl", VK_RCONTROL },
    { "alt", VK_MENU }, { "lalt", VK_LMENU }, { "ralt", VK_RMENU }, { "capslock", VK_CAPITAL },
    { "up", VK_UP }, { "down", VK_DOWN }, { "left", VK_LEFT }, { "right", VK_RIGHT },
    { "insert", VK_INSERT }, { "delete", VK_DELETE }, { "home", VK_HOME }, { "end", VK_END },
    { "pageup", VK_PRIOR }, { "pagedown", VK_NEXT },
    { "mouse1", VK_LBUTTON }, { "mouse2", VK_RBUTTON }, { "mouse3", VK_MBUTTON }, { "mouse4", VK_XBUTTON1 }, { "mouse5", VK_XBUTTON2 },
    { "lmb", VK_LBUTTON }, { "rmb", VK_RBUTTON }, { "mmb", VK_MBUTTON },
};

KeyBindings g_bindings = DefaultKeyBindings();

} // namespace

KeyBindings DefaultKeyBindings()
{
    KeyBindings b;
    auto set = [&](KeyAction a, std::initializer_list<int> vks) { b.keys[int(a)] = vks; };
    set(KeyAction::A, { VK_RETURN, VK_SPACE });
    set(KeyAction::B, { VK_BACK, VK_CONTROL });
    set(KeyAction::X, { 'E' });
    set(KeyAction::Y, { 'Q' });
    set(KeyAction::Start, { VK_ESCAPE });
    set(KeyAction::Back, { VK_TAB });
    set(KeyAction::Up, { VK_UP });
    set(KeyAction::Down, { VK_DOWN });
    set(KeyAction::Left, { VK_LEFT });
    set(KeyAction::Right, { VK_RIGHT });
    set(KeyAction::LB, { '1' });
    set(KeyAction::RB, { '3' });
    set(KeyAction::LS, { 'F' });
    set(KeyAction::RS, { 'R', VK_MBUTTON });
    set(KeyAction::LT, { 'Z', VK_RBUTTON });
    set(KeyAction::RT, { 'C', VK_LBUTTON });
    set(KeyAction::MoveForward, { 'W' });
    set(KeyAction::MoveBack, { 'S' });
    set(KeyAction::MoveLeft, { 'A' });
    set(KeyAction::MoveRight, { 'D' });
    set(KeyAction::LookUp, { 'I' });
    set(KeyAction::LookDown, { 'K' });
    set(KeyAction::LookLeft, { 'J' });
    set(KeyAction::LookRight, { 'L' });
    set(KeyAction::Walk, { VK_SHIFT });
    return b;
}

int KeyCodeFromName(const std::string& n)
{
    if (n.size() == 1 && (isalpha((unsigned char)n[0]) || isdigit((unsigned char)n[0])))
        return toupper((unsigned char)n[0]);
    if (n.size() >= 2 && n[0] == 'f' && isdigit((unsigned char)n[1]))
    {
        const int f = atoi(n.c_str() + 1);
        return f >= 2 && f <= 12 ? VK_F1 + f - 1 : 0; // F1 is the menu key
    }
    if (n.rfind("numpad", 0) == 0 && n.size() == 7 && isdigit((unsigned char)n[6]))
        return VK_NUMPAD0 + (n[6] - '0');
    for (auto& k : kKeyNames)
        if (n == k.name)
            return k.vk;
    return 0;
}

const char* KeyActionName(KeyAction action) { return kActionNames[int(action)]; }

KeyBindings ParseKeyBindings(const std::vector<std::pair<std::string, std::string>>& keys, float walkSpeed)
{
    KeyBindings b = DefaultKeyBindings();
    if (walkSpeed >= 0.1f && walkSpeed <= 1.0f)
        b.walkSpeed = walkSpeed;
    for (auto& [key, value] : keys)
        for (int a = 0; a < int(KeyAction::Count); a++)
        {
            std::string name = kActionNames[a];
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(tolower(c)); });
            if (key != name)
                continue;
            std::vector<int> vks;
            bool any = false;
            for (size_t p = 0; p <= value.size();)
            {
                size_t comma = value.find(',', p);
                if (comma == std::string::npos)
                    comma = value.size();
                std::string token = value.substr(p, comma - p);
                token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char c) { return isspace(c); }), token.end());
                if (token == "none")
                    any = true;
                else if (const int vk = KeyCodeFromName(token))
                    vks.push_back(vk), any = true;
                p = comma + 1;
            }
            if (any) // "None" unbinds; only unknown names keeps the default
                b.keys[a] = vks;
        }
    return b;
}

void SetKeyBindings(const KeyBindings& bindings) { g_bindings = bindings; }
const KeyBindings& CurrentKeyBindings() { return g_bindings; }
