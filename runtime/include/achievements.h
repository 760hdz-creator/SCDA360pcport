// Local achievements. The title reports unlocks through XMsgStartIORequest(app 0xFB, msg 0xB0008,
// {u32 count, XUSER_ACHIEVEMENT* {u32 user, u32 id}}); names, gamerscore and 64x64 PNG icons come
// from the title's XDBF (SPA) resource in the image (xex resource info 0x2FF, "555307D7").
// Unlocks persist in <profile dir>\achievements.txt ("<id> <unix time>" lines).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct Achievement
{
    uint16_t id = 0;
    uint16_t gamerscore = 0;
    uint32_t imageId = 0;
    uint32_t flags = 0;
    std::string name;        // label
    std::string description; // achieved description
    std::string howTo;       // unachieved description, empty for secret ones
    int64_t unlockedAt = 0;  // unix time, 0 locked
};

// Parses the XDBF achievement table (XACH) with strings from string table `language`
// (XCONFIG language id, 1 English ... falls back to 1). Empty on a malformed blob.
std::vector<Achievement> ParseXdbfAchievements(const uint8_t* xdbf, size_t size, uint32_t language);
// Image `imageId` (XDBF namespace 2): the PNG bytes, empty when absent.
std::vector<uint8_t> XdbfImage(const uint8_t* xdbf, size_t size, uint32_t imageId);

// Runtime store (thread-safe). The table loads lazily from guest memory on first use.
void AchievementsUnlock(uint32_t id);   // idempotent: only the first unlock persists and toasts
std::vector<Achievement> AchievementsList();
std::vector<uint8_t> AchievementImage(uint32_t imageId);
// Next unlock to announce (FIFO). False when none is pending.
bool AchievementsPopToast(Achievement& out);
bool AchievementsToastPending();

// PNG -> RGBA8 texels (R in the low byte, straight alpha); empty on failure.
std::vector<uint32_t> DecodePngRgba(const std::vector<uint8_t>& png, uint32_t& width, uint32_t& height);
