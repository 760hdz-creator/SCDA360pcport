#include "check.h"
#include <achievements.h>
#include <cstdio>
#include <vector>

// The title's XDBF resource "555307D7" sits at 0x832A0000 (image offset 0x12A0000), 0x51687 bytes.
static std::vector<uint8_t> LoadXdbf()
{
    std::vector<uint8_t> out;
    if (FILE* f = fopen(SCDA_ROOT "/assets/SC4_Offline.img", "rb"))
    {
        out.resize(0x60000);
        fseek(f, 0x12A0000, SEEK_SET);
        out.resize(fread(out.data(), 1, out.size(), f));
        fclose(f);
    }
    return out;
}

TEST(xdbf_achievements_parse_from_title_resource)
{
    const std::vector<uint8_t> x = LoadXdbf();
    CHECK(x.size() > 0x50000);
    const std::vector<Achievement> list = ParseXdbfAchievements(x.data(), x.size(), 1);
    CHECK_EQ(list.size(), size_t(38));
    if (list.size() != 38)
        return;
    CHECK_EQ(list[0].id, 6);
    CHECK(list[0].name == "Training Completed");
    CHECK_EQ(list[0].gamerscore, 10);
    CHECK_EQ(list[11].id, 17);
    CHECK(list[11].name == "Defeated the JBA"); // trailing newline in the table is trimmed
    CHECK(list[11].howTo.empty());               // secret: no unachieved text
    uint32_t total = 0;
    for (const Achievement& a : list)
        total += a.gamerscore;
    CHECK_EQ(total, 1000u);
    const std::vector<uint8_t> png = XdbfImage(x.data(), x.size(), list[0].imageId);
    CHECK(png.size() > 8 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
    uint32_t w = 0, h = 0;
    CHECK_EQ(DecodePngRgba(png, w, h).size(), size_t(64 * 64));
    CHECK_EQ(w, 64u);
    // An unknown language falls back to the title default.
    CHECK_EQ(ParseXdbfAchievements(x.data(), x.size(), 99).size(), size_t(38));
}

TEST(xdbf_rejects_malformed_blobs)
{
    const uint8_t junk[32] = { 'X', 'D', 'B', 'F', 0, 0, 0, 1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK(ParseXdbfAchievements(junk, sizeof(junk), 1).empty());
    CHECK(XdbfImage(junk, sizeof(junk), 1).empty());
    CHECK(ParseXdbfAchievements(junk, 4, 1).empty());
}
