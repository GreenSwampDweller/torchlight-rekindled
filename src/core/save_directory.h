// The save directory: where characters are saved (`FILESYSTEM::GetSaveDataPath`),
// the files in it as the menus list them, and the settings file beside them.
#pragma once

#include "core/continue_menu.h"
#include "core/settings_menu.h"
#include "formats/save_file.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tl::core {

// Where characters are saved: `FILESYSTEM::GetSaveDataPath`. Each target's own
// writable place, like the log.
std::string save_directory();

bool read_whole_file(const std::string& path, std::vector<uint8_t>& bytes);

// Read a character save, falling back to the newest valid rotating backup and
// restoring it to the primary path when the primary file is damaged.
bool read_character_with_backup(const std::string& path,
                                tl::formats::CharacterFile& file);

// The settings the Options screen keeps (`CSettings`; the console's file is on
// its storage device, this one beside the saves). A missing file leaves the
// defaults.
void load_game_settings(const std::string& path, GameSettings& settings);

void make_save_directory();

// The save files, newest first: `CContinueGameMenu::reloadFiles`
// lists `*.svt` and sorts it with
// `FileTimeSorter`.
std::vector<std::string> list_saves();

// `CNewGameMenu::getEmptySave`: the lowest
// number no save file is named after, then `.svt`.
std::string empty_save_path();

// `CContinueGameMenu::canContinue` for the
// newest save: a character whose saved hit points are above 0. Fills `file` and
// `path` when there is one.
bool newest_living_save(tl::formats::CharacterFile& file, std::string& path);

// The save directory as Load Character lists it: `CContinueGameMenu::reloadFiles`,
// in `list_saves()`'s order, unreadable
// files passed over. The trial-version check is not applied.
std::vector<ContinueEntry> list_continue_entries();

// The save `menu_backdrop` reads: `from`, or the newest. Empty with none.
std::string menu_backdrop_file(const std::string& from);

}  // namespace tl::core
