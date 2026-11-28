// Local achievements (achievements.h).
#include <achievements.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <windows.h>
#include <objidl.h> // GDI+ needs IStream/PROPID (WIN32_LEAN_AND_MEAN drops them)
namespace Gdiplus
{
using std::max;
using std::min;
}
#include <gdiplus.h>
#include <module.h>
#include <runtime.h>
#include <settings.h>

namespace {

uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t Be64(const uint8_t* p) { return uint64_t(Be32(p)) << 32 | Be32(p + 4); }

// XDBF: header {'XDBF', version, entry table length, entry count, free table length, free count},
// entries {u16 namespace, u64 id, u32 offset, u32 length} (offsets from the end of both tables).
struct Xdbf
{
    const uint8_t* data;
    size_t size;
    bool Find(uint16_t ns, uint64_t id, const uint8_t*& out, uint32_t& length) const
    {
        if (size < 24 || Be32(data) != 0x58444246) // 'XDBF'
            return false;
        const uint64_t tableLen = Be32(data + 8), count = Be32(data + 12), freeLen = Be32(data + 16);
        const uint64_t base = 24 + tableLen * 18 + freeLen * 8;
        if (count > tableLen || 24 + count * 18 > size || base > size)
            return false;
        for (uint64_t i = 0; i < count; i++)
        {
            const uint8_t* e = data + 24 + i * 18;
            if (Be16(e) != ns || Be64(e + 2) != id)
                continue;
            const uint64_t off = base + Be32(e + 10), len = Be32(e + 14);
            if (off + len > size)
                return false;
            out = data + off;
            length = uint32_t(len);
            return true;
        }
        return false;
    }
};

constexpr uint16_t kNsMetadata = 1, kNsImage = 2, kNsStrings = 3;
constexpr uint64_t kXach = 0x58414348, kXstc = 0x58535443;

// String table {'XSTR', version, size, u16 count, {u16 id, u16 length, UTF-8}...}.
std::map<uint16_t, std::string> Strings(const Xdbf& x, uint32_t language)
{
    std::map<uint16_t, std::string> out;
    const uint8_t* p;
    uint32_t len;
    if (!x.Find(kNsStrings, language, p, len) || len < 14)
        return out;
    const uint32_t count = Be16(p + 12);
    uint32_t at = 14;
    for (uint32_t i = 0; i < count && at + 4 <= len; i++)
    {
        const uint16_t id = Be16(p + at), n = Be16(p + at + 2);
        if (at + 4 + n > len)
            break;
        std::string s(reinterpret_cast<const char*>(p + at + 4), n);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
            s.pop_back();
        out[id] = std::move(s);
        at += 4 + n;
    }
    return out;
}

} // namespace

std::vector<Achievement> ParseXdbfAchievements(const uint8_t* data, size_t size, uint32_t language)
{
    std::vector<Achievement> out;
    const Xdbf x{ data, size };
    const uint8_t* p;
    uint32_t len;
    if (!x.Find(kNsMetadata, kXach, p, len) || len < 14 || Be32(p) != kXach)
        return out;
    // Language: the asked one, else the title's default (XSTC {magic, version, size, u32 lang}), else English.
    auto strings = Strings(x, language);
    const uint8_t* stc;
    uint32_t stcLen;
    if (strings.empty() && x.Find(kNsMetadata, kXstc, stc, stcLen) && stcLen >= 16)
        strings = Strings(x, Be32(stc + 12));
    if (strings.empty())
        strings = Strings(x, 1);
    auto str = [&](uint16_t id) { auto it = strings.find(id); return it == strings.end() ? std::string() : it->second; };
    // XACH {magic, version, size, u16 count}, entries 0x24: {u16 id, u16 label, u16 description,
    // u16 unachieved, u32 image, u16 gamerscore, u16, u32 flags, 16 bytes}.
    const uint32_t count = Be16(p + 12);
    for (uint32_t i = 0; i < count && 14 + (i + 1) * 0x24 <= len; i++)
    {
        const uint8_t* e = p + 14 + i * 0x24;
        Achievement a;
        a.id = Be16(e);
        a.name = str(Be16(e + 2));
        a.description = str(Be16(e + 4));
        a.howTo = str(Be16(e + 6));
        a.imageId = Be32(e + 8);
        a.gamerscore = Be16(e + 12);
        a.flags = Be32(e + 16);
        out.push_back(std::move(a));
    }
    return out;
}

std::vector<uint8_t> XdbfImage(const uint8_t* data, size_t size, uint32_t imageId)
{
    const uint8_t* p;
    uint32_t len;
    if (!Xdbf{ data, size }.Find(kNsImage, imageId, p, len))
        return {};
    return std::vector<uint8_t>(p, p + len);
}

extern std::string g_profileDir; // xam.cpp

namespace {

std::mutex g_mutex;
bool g_loaded;
std::vector<Achievement> g_table;
std::vector<uint8_t> g_xdbf; // host copy: the image reloads on relaunch
std::deque<Achievement> g_toasts;

std::string StorePath() { return g_profileDir + "\\achievements.txt"; }

// Under g_mutex. The image is loaded (and the XEX header copied) before any guest code runs.
void LoadLocked()
{
    if (g_loaded)
        return;
    uint32_t size = 0;
    const uint32_t at = XexResource("555307D7", &size);
    if (!at || !size)
    {
        RuntimeLog("[ach] no XDBF resource\n");
        return; // retried on the next call
    }
    g_loaded = true;
    const uint8_t* guest = static_cast<const uint8_t*>(GuestPtr(at));
    g_xdbf.assign(guest, guest + size);
    g_table = ParseXdbfAchievements(g_xdbf.data(), g_xdbf.size(), GameLanguage());
    // The offline title's achievement manager (*0x82C92B04, ids at +0x44) holds 21 slots: ids 6-23,
    // 27, 29, 31 (SCDA_TRACE_ACH=1 logs them; 600 G). The rest (ranks, trophies, map experts) are
    // multiplayer, which this recomp does not run.
    for (Achievement& a : g_table)
        a.multiplayer = !((a.id >= 6 && a.id <= 23) || a.id == 27 || a.id == 29 || a.id == 31);
    if (FILE* f = fopen(StorePath().c_str(), "r"))
    {
        unsigned id;
        long long when;
        while (fscanf(f, "%u %lld", &id, &when) == 2)
            for (Achievement& a : g_table)
                if (a.id == id && !a.unlockedAt)
                    a.unlockedAt = when ? when : 1;
        fclose(f);
    }
    RuntimeLog("[ach] %zu achievements from XDBF 0x%08X (%u bytes)\n", g_table.size(), at, size);
}

} // namespace

void AchievementsUnlock(uint32_t id)
{
    std::lock_guard lock(g_mutex);
    LoadLocked();
    // SCDA_TRACE_ACH=1: the title's achievement manager (*0x82C92B04) id table, 21 slots at +0x44
    // (sub_821C6AB8 rejects index >= 21; -1 = unused).
    static bool traced = false;
    if (!traced && getenv("SCDA_TRACE_ACH"))
        if (const uint32_t mgr = GuestLoad32(0x82C92B04))
        {
            traced = true;
            std::string ids;
            for (uint32_t i = 0; i < 21; i++)
                ids += " " + std::to_string(int32_t(GuestLoad32(mgr + 0x44 + i * 4)));
            RuntimeLog("[ach] manager 0x%08X ids%s\n", mgr, ids.c_str());
        }
    auto it = std::find_if(g_table.begin(), g_table.end(), [&](const Achievement& a) { return a.id == id; });
    if (it == g_table.end())
    {
        RuntimeLog("[ach] unknown achievement id %u\n", id);
        return;
    }
    if (it->unlockedAt)
        return;
    it->unlockedAt = int64_t(time(nullptr));
    CreateDirectoryA(g_profileDir.c_str(), nullptr);
    if (FILE* f = fopen(StorePath().c_str(), "a"))
    {
        fprintf(f, "%u %lld\n", id, (long long)it->unlockedAt);
        fclose(f);
    }
    g_toasts.push_back(*it);
    RuntimeLog("[ach] unlocked %u \"%s\" (%uG)\n", id, it->name.c_str(), it->gamerscore);
}

std::vector<Achievement> AchievementsList()
{
    std::lock_guard lock(g_mutex);
    LoadLocked();
    return g_table;
}

std::vector<uint8_t> AchievementImage(uint32_t imageId)
{
    std::lock_guard lock(g_mutex);
    LoadLocked();
    return XdbfImage(g_xdbf.data(), g_xdbf.size(), imageId);
}

bool AchievementsPopToast(Achievement& out)
{
    std::lock_guard lock(g_mutex);
    if (g_toasts.empty())
        return false;
    out = std::move(g_toasts.front());
    g_toasts.pop_front();
    return true;
}

bool AchievementsToastPending()
{
    std::lock_guard lock(g_mutex);
    return !g_toasts.empty();
}

std::vector<uint32_t> DecodePngRgba(const std::vector<uint8_t>& png, uint32_t& width, uint32_t& height)
{
    std::vector<uint32_t> out;
    width = height = 0;
    static std::once_flag once;
    std::call_once(once, []
    {
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token;
        Gdiplus::GdiplusStartup(&token, &input, nullptr); // kept for the process
    });
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, png.size());
    if (!mem || png.empty())
        return out;
    memcpy(GlobalLock(mem), png.data(), png.size());
    GlobalUnlock(mem);
    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(mem, TRUE, &stream) != S_OK)
    {
        GlobalFree(mem);
        return out;
    }
    {
        Gdiplus::Bitmap bmp(stream);
        Gdiplus::BitmapData data{};
        Gdiplus::Rect all(0, 0, int(bmp.GetWidth()), int(bmp.GetHeight()));
        if (bmp.GetLastStatus() == Gdiplus::Ok && all.Width > 0 && all.Height > 0 &&
            bmp.LockBits(&all, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) == Gdiplus::Ok)
        {
            width = uint32_t(all.Width), height = uint32_t(all.Height);
            out.resize(size_t(width) * height);
            for (uint32_t y = 0; y < height; y++)
            {
                const uint32_t* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(data.Scan0) + y * data.Stride);
                for (uint32_t x = 0; x < width; x++) // ARGB (B low) -> RGBA (R low)
                {
                    const uint32_t p = row[x];
                    out[size_t(y) * width + x] = (p & 0xFF00FF00) | ((p >> 16) & 0xFF) | ((p & 0xFF) << 16);
                }
            }
            bmp.UnlockBits(&data);
        }
    }
    stream->Release();
    return out;
}
