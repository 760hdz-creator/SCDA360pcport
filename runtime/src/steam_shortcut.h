// "Add to Steam" (issue #9, F1 menu, Update tab): adds this exe as a non-Steam game to every Steam
// user's userdata\<id>\config\shortcuts.vdf (binary KeyValues). Steam reads that file only at start
// and rewrites it on exit, so the add refuses while Steam runs.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace steam {

// Blocking. True when at least one Steam user got the shortcut (or already had it); message says
// what happened either way, for the menu.
bool AddShortcut(std::string& message);

// Pure helpers (tests).
// Steam's id for a shortcut: crc32(exe + name) | 0x80000000 (exe as stored, quotes included).
uint32_t ShortcutAppId(const std::string& exe, const std::string& name);
// Appends a shortcut entry to a shortcuts.vdf image (empty = new file). False if the image does not
// parse. alreadyThere: an entry with the same Exe exists (case-insensitive); the image is then unchanged.
bool AppendShortcut(std::vector<uint8_t>& vdf, const std::string& name, const std::string& exe, const std::string& startDir,
                    const std::string& icon, bool& alreadyThere);

} // namespace steam
