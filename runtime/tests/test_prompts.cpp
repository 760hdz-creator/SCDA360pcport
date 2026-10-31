#include "check.h"
#include <cstring>
#include <gpu/draw_state.h>
#include <prompts.h>
#include <settings.h>

static uint64_t Fnv(const uint8_t* p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

// The icon hash is FNV-1a over the DDS-order blocks, whatever the guest tiling and 8-in-16 endian.
TEST(block_linear_hash_ignores_tiling_and_endian)
{
    constexpr uint32_t kBlocks = 16; // 64x64 DXT5 = 16x16 blocks of 16 bytes
    std::vector<uint8_t> dds(kBlocks * kBlocks * 16);
    for (size_t i = 0; i < dds.size(); i++)
        dds[i] = uint8_t(i * 131 + (i >> 7));
    std::vector<uint8_t> guest(32 * 32 * 16); // tiled storage is 32x32 blocks
    for (uint32_t by = 0; by < kBlocks; by++)
        for (uint32_t bx = 0; bx < kBlocks; bx++)
        {
            uint8_t* dst = &guest[size_t(TiledTexelIndex(bx, by, kBlocks, 4)) * 16];
            const uint8_t* src = &dds[(size_t(by) * kBlocks + bx) * 16];
            for (int i = 0; i < 16; i += 2) // guest is 8-in-16 swapped
                dst[i] = src[i + 1], dst[i + 1] = src[i];
        }
    TextureFetch t;
    t.valid = t.tiled = true;
    t.format = 20, t.endian = 1, t.width = t.height = t.pitch = 64;
    CHECK_EQ(BlockLinearHash(t, guest.data()), Fnv(dds.data(), dds.size()));
    t.format = 6; // not DXT
    CHECK_EQ(BlockLinearHash(t, guest.data()), 0ull);
}

TEST(prompt_setting_parsed)
{
    CHECK(ParsePromptSetting("PlayStation") == PromptSetting::PlayStation);
    CHECK(ParsePromptSetting("keyboard") == PromptSetting::Keyboard);
    CHECK(ParsePromptSetting("XBOX") == PromptSetting::Xbox);
    CHECK(ParsePromptSetting("Auto") == PromptSetting::Auto);
    CHECK(ParsePromptSetting("junk") == PromptSetting::Auto);
    CHECK(ParsePromptSetting(nullptr) == PromptSetting::Auto);
}

// Auto follows the last device used; before any input, the connected pad, else the keyboard.
TEST(prompt_style_follows_last_device)
{
    PromptTracker t;
    CHECK(t.Style(PromptSetting::Auto, PadKind::None) == PromptStyle::Keyboard);
    CHECK(t.Style(PromptSetting::Auto, PadKind::PS5) == PromptStyle::PS5);
    t.Note(PadKind::PS5, false, false); // idle pad: no choice yet
    CHECK(t.last == -1);
    t.Note(PadKind::PS5, false, true);
    CHECK(t.Style(PromptSetting::Auto, PadKind::PS5) == PromptStyle::Keyboard);
    t.Note(PadKind::PS4, true, false);
    CHECK(t.Style(PromptSetting::Auto, PadKind::PS4) == PromptStyle::PS4);
    t.Note(PadKind::Xbox, true, false);
    CHECK(t.Style(PromptSetting::Auto, PadKind::Xbox) == PromptStyle::Xbox);
    t.Note(PadKind::None, true, false); // no pad: nothing to pick
    CHECK(t.Style(PromptSetting::Auto, PadKind::None) == PromptStyle::Xbox);
    // Fixed settings; PlayStation is PS4 only after a DualShock 4.
    CHECK(t.Style(PromptSetting::Keyboard, PadKind::Xbox) == PromptStyle::Keyboard);
    CHECK(t.Style(PromptSetting::Xbox, PadKind::PS4) == PromptStyle::Xbox);
    CHECK(t.Style(PromptSetting::PlayStation, PadKind::Xbox) == PromptStyle::PS4);
    PromptTracker fresh;
    CHECK(fresh.Style(PromptSetting::PlayStation, PadKind::None) == PromptStyle::PS5);
}

// Every table icon draws something for PlayStation and keyboard; Xbox keeps the game's texels.
TEST(prompt_icons_redraw)
{
    CHECK_EQ(PromptIconIndex(0x1234, 64, 64), -1);
    for (int i = 0; i < 64; i++)
    {
        const std::string name = PromptIconName(i);
        if (name == "?")
            break;
        const uint32_t size = name.rfind("btn_opsat_", 0) == 0 ? 32 : 64;
        for (PromptStyle s : { PromptStyle::Xbox, PromptStyle::PS4, PromptStyle::PS5, PromptStyle::Keyboard })
        {
            std::vector<uint32_t> texels(size * size, 0x12345678);
            const bool drawn = PromptRedraw(i, s, texels, size, size);
            CHECK(drawn == (s != PromptStyle::Xbox));
            // SCDA_DUMP_PROMPTS=<dir>: each drawn icon as <name>_<style>.rgba (RGBA8, size x size), for review.
            if (const char* dir = getenv("SCDA_DUMP_PROMPTS"); dir && drawn)
                if (FILE* f = fopen((std::string(dir) + "/" + name + "_" + std::to_string(int(s)) + ".rgba").c_str(), "wb"))
                {
                    fwrite(texels.data(), 4, texels.size(), f);
                    fclose(f);
                }
            uint32_t opaque = 0, clear = 0;
            for (uint32_t p : texels)
                opaque += (p >> 24) > 200, clear += (p >> 24) == 0;
            if (drawn && (opaque < size * size / 10 || clear < size * 2))
            {
                printf("icon %s style %d: opaque %u clear %u\n", name.c_str(), int(s), opaque, clear);
                CHECK(false);
            }
        }
    }
    CHECK(std::string(PromptIconName(0)) == "button_a");
}

TEST(button_prompts_setting_read)
{
    CHECK(LoadInputSettings("C:/nonexistent/settings.ini").buttonPrompts == "Auto");
}
