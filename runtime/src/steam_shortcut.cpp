#include "steam_shortcut.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>

namespace steam {
namespace {

// Binary KeyValues: type byte, NUL-terminated key, value. 0 = nested map (ends with 8), 1 = string,
// 2 = int32, 3 = float32, 4 = pointer (int32), 5 = wide string, 6 = color (int32), 7 = uint64.
constexpr uint8_t kMap = 0, kString = 1, kInt = 2, kEnd = 8;

struct Reader
{
    const std::vector<uint8_t>& d;
    size_t p = 0;
    bool CStr(std::string* out)
    {
        const size_t start = p;
        while (p < d.size() && d[p])
            p++;
        if (p >= d.size())
            return false;
        if (out)
            out->assign(reinterpret_cast<const char*>(d.data() + start), p - start);
        p++;
        return true;
    }
    bool Skip(size_t n)
    {
        if (d.size() - p < n)
            return false;
        p += n;
        return true;
    }
    // Walks a map body up to and including its end byte. onString sees each direct string child.
    bool Map(const std::function<void(const std::string&, const std::string&)>& onString, int depth = 0)
    {
        if (depth > 16)
            return false;
        for (;;)
        {
            if (p >= d.size())
                return false;
            const uint8_t type = d[p++];
            if (type == kEnd)
                return true;
            std::string key;
            if (!CStr(&key))
                return false;
            switch (type)
            {
            case kMap:
                if (!Map([](const std::string&, const std::string&) {}, depth + 1)) // children: Exe is direct
                    return false;
                break;
            case kString:
            {
                std::string v;
                if (!CStr(&v))
                    return false;
                onString(key, v);
                break;
            }
            case 5: // wide string: UTF-16 until a 0 unit
                for (;;)
                {
                    if (d.size() - p < 2)
                        return false;
                    const bool end = !d[p] && !d[p + 1];
                    p += 2;
                    if (end)
                        break;
                }
                break;
            case kInt: case 3: case 4: case 6:
                if (!Skip(4))
                    return false;
                break;
            case 7:
                if (!Skip(8))
                    return false;
                break;
            default: return false;
            }
        }
    }
};

void PutStr(std::vector<uint8_t>& o, const std::string& s)
{
    o.insert(o.end(), s.begin(), s.end());
    o.push_back(0);
}
void PutString(std::vector<uint8_t>& o, const char* key, const std::string& v)
{
    o.push_back(kString);
    PutStr(o, key);
    PutStr(o, v);
}
void PutInt(std::vector<uint8_t>& o, const char* key, uint32_t v)
{
    o.push_back(kInt);
    PutStr(o, key);
    for (int i = 0; i < 4; i++)
        o.push_back(uint8_t(v >> (8 * i)));
}

bool IEquals(const std::string& a, const std::string& b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return tolower(uint8_t(x)) == tolower(uint8_t(y)); });
}

std::string Utf8(const std::wstring& w)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool SteamRunning()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W e{ sizeof(e) };
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e))
        found = !_wcsicmp(e.szExeFile, L"steam.exe");
    CloseHandle(snap);
    return found;
}

std::wstring SteamPath()
{
    wchar_t buf[MAX_PATH];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return buf;
    return {};
}

} // namespace

uint32_t ShortcutAppId(const std::string& exe, const std::string& name)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (const std::string* s : { &exe, &name })
        for (uint8_t c : *s)
        {
            crc ^= c;
            for (int k = 0; k < 8; k++)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
        }
    return ~crc | 0x80000000u;
}

bool AppendShortcut(std::vector<uint8_t>& vdf, const std::string& name, const std::string& exe, const std::string& startDir,
                    const std::string& icon, bool& alreadyThere)
{
    alreadyThere = false;
    static const char kHeader[] = "\0shortcuts"; // type 0, key "shortcuts", NUL
    if (vdf.empty())
    {
        vdf.assign(kHeader, kHeader + sizeof(kHeader));
        vdf.push_back(kEnd); // end of "shortcuts"
        vdf.push_back(kEnd); // end of the root
    }
    if (vdf.size() < sizeof(kHeader) + 2 || memcmp(vdf.data(), kHeader, sizeof(kHeader)))
        return false;
    // Entries "0", "1", ...: count them and look for our exe.
    Reader r{ vdf, sizeof(kHeader) };
    uint32_t count = 0;
    for (;;)
    {
        if (r.p >= vdf.size())
            return false;
        const uint8_t type = vdf[r.p++];
        if (type == kEnd)
            break;
        if (type != kMap || !r.CStr(nullptr))
            return false;
        if (!r.Map([&](const std::string& k, const std::string& v) {
                if (IEquals(k, "Exe") && IEquals(v, exe))
                    alreadyThere = true;
            }))
            return false;
        count++;
    }
    const size_t insertAt = r.p - 1; // our entry goes before the end of "shortcuts"
    if (r.p >= vdf.size() || vdf[r.p] != kEnd)
        return false;
    if (alreadyThere)
        return true;

    std::vector<uint8_t> e;
    e.push_back(kMap);
    PutStr(e, std::to_string(count));
    PutInt(e, "appid", ShortcutAppId(exe, name));
    PutString(e, "AppName", name);
    PutString(e, "Exe", exe);
    PutString(e, "StartDir", startDir);
    PutString(e, "icon", icon);
    PutString(e, "ShortcutPath", "");
    PutString(e, "LaunchOptions", "");
    PutInt(e, "IsHidden", 0);
    PutInt(e, "AllowDesktopConfig", 1);
    PutInt(e, "AllowOverlay", 1);
    PutInt(e, "OpenVR", 0);
    PutInt(e, "Devkit", 0);
    PutString(e, "DevkitGameID", "");
    PutInt(e, "DevkitOverrideAppID", 0);
    PutInt(e, "LastPlayTime", 0);
    PutString(e, "FlatpakAppID", "");
    e.push_back(kMap);
    PutStr(e, "tags");
    e.push_back(kEnd); // tags
    e.push_back(kEnd); // entry
    vdf.insert(vdf.begin() + insertAt, e.begin(), e.end());
    return true;
}

bool AddShortcut(std::string& message)
{
    namespace fs = std::filesystem;
    const std::wstring steamPath = SteamPath();
    std::error_code ec;
    if (steamPath.empty() || !fs::is_directory(fs::path(steamPath) / L"userdata", ec))
    {
        message = "Steam is not installed (no Steam folder found).";
        return false;
    }
    if (SteamRunning())
    {
        message = "Exit Steam first (Steam > Exit), then click again.";
        return false;
    }
    wchar_t exeBuf[MAX_PATH];
    GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
    const fs::path exePath(exeBuf);
    const std::string exe = "\"" + Utf8(exePath.wstring()) + "\"";
    const std::string startDir = "\"" + Utf8(exePath.parent_path().wstring()) + "\""; // as Steam writes it
    const std::string name = "Splinter Cell: Double Agent (Recomp)";

    int added = 0, had = 0, failed = 0;
    for (const fs::directory_entry& user : fs::directory_iterator(fs::path(steamPath) / L"userdata", ec))
    {
        const std::wstring id = user.path().filename().wstring();
        if (id == L"0" || id.find_first_not_of(L"0123456789") != std::wstring::npos)
            continue;
        const fs::path config = user.path() / L"config";
        if (!fs::is_directory(config, ec))
            continue;
        const fs::path file = config / L"shortcuts.vdf";
        std::vector<uint8_t> vdf;
        {
            std::ifstream in(file, std::ios::binary);
            vdf.assign(std::istreambuf_iterator<char>(in), {});
        }
        bool there = false;
        if (!AppendShortcut(vdf, name, exe, startDir, Utf8(exePath.wstring()), there))
        {
            failed++;
            continue;
        }
        if (there)
        {
            had++;
            continue;
        }
        if (fs::exists(file, ec))
            fs::copy_file(file, config / L"shortcuts.vdf.bak", fs::copy_options::overwrite_existing, ec);
        const fs::path tmp = config / L"shortcuts.vdf.scda";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(vdf.data()), std::streamsize(vdf.size()));
            if (!out)
            {
                failed++;
                continue;
            }
        }
        if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            fs::remove(tmp, ec);
            failed++;
            continue;
        }
        added++;
    }
    if (added + had == 0)
    {
        message = failed ? "Could not update Steam's shortcuts file." : "No Steam user found (log in to Steam once first).";
        return false;
    }
    message = added ? "Added to Steam. Start Steam and look under Non-Steam games in your library."
                    : "Already in your Steam library.";
    if (failed)
        message += " (Some Steam users could not be updated.)";
    return true;
}

} // namespace steam
