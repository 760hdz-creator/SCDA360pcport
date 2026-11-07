#include "check.h"
#include <steam_shortcut.h>

#include <algorithm>
#include <cstring>

TEST(steam_shortcut_new_file)
{
    std::vector<uint8_t> vdf;
    bool there = true;
    CHECK(steam::AppendShortcut(vdf, "Game", "\"C:\\g\\scda.exe\"", "\"C:\\g\\\"", "", there));
    CHECK(!there);
    static const char kHead[] = "\0shortcuts\0\0" "0";
    CHECK(vdf.size() > sizeof(kHead));
    CHECK(!memcmp(vdf.data(), kHead, sizeof(kHead))); // root map, "shortcuts", entry map "0"
    CHECK_EQ(int(vdf[vdf.size() - 1]), 8);
    CHECK_EQ(int(vdf[vdf.size() - 2]), 8);
    CHECK_EQ(int(vdf[vdf.size() - 3]), 8); // end of entry
    CHECK_EQ(int(vdf[vdf.size() - 4]), 8); // end of tags

    // Second add of the same exe (other case) is a no-op; another exe becomes entry "1".
    const std::vector<uint8_t> once = vdf;
    CHECK(steam::AppendShortcut(vdf, "Game", "\"c:\\G\\SCDA.exe\"", "", "", there));
    CHECK(there);
    CHECK(vdf == once);
    CHECK(steam::AppendShortcut(vdf, "Other", "\"C:\\o.exe\"", "", "", there));
    CHECK(!there);
    CHECK(vdf.size() > once.size());
    static const char kSecond[] = "\0" "1\0";
    CHECK(std::search(vdf.begin(), vdf.end(), kSecond, kSecond + sizeof(kSecond) - 1) == vdf.begin() + (once.size() - 2));
}

TEST(steam_shortcut_rejects_garbage)
{
    bool there = false;
    std::vector<uint8_t> bad = { 'x', 'y', 'z' };
    CHECK(!steam::AppendShortcut(bad, "a", "b", "", "", there));
    std::vector<uint8_t> truncated;
    CHECK(steam::AppendShortcut(truncated, "a", "b", "", "", there));
    truncated.resize(truncated.size() - 5);
    CHECK(!steam::AppendShortcut(truncated, "a", "c", "", "", there));
}

TEST(steam_shortcut_appid)
{
    // crc32("ab") = 0x9E83486D.
    CHECK_EQ(steam::ShortcutAppId("a", "b"), 0x9E83486Du | 0x80000000u);
}
