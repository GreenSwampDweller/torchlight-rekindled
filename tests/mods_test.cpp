// The mods directory as `CModFileFilter` orders it, and `mods.dat` as it writes it.
#include "core/mods.h"
#include "core/new_game_menu.h"
#include "formats/adm.h"
#include "formats/dat_text.h"
#include "test_support.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

tl::formats::AdmFile parse(const std::string& text) {
    return tl::formats::parse_dat_text(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

std::vector<std::string> folders_of(const tl::core::ModList& list) {
    std::vector<std::string> out;
    out.reserve(list.mods.size());
    for (const tl::core::ModInfo& mod : list.mods) out.push_back(mod.folder);
    return out;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::FILE* out = std::fopen(path.string().c_str(), "wb");
    TL_CHECK(out != nullptr);
    if (out == nullptr) return;
    std::fwrite(text.data(), 1, text.size(), out);
    std::fclose(out);
}

}  // namespace

/// A listed mod takes its PRIORITY's place; a taken place or a switched-off mod goes after; a
/// folder `mods.dat` does not name comes last, on.
TL_TEST(mods_arranged_by_priority_then_new) {
    const tl::formats::AdmFile listed = parse(
        "[MODS]\n<BOOL>CHECKFORNEW:true\n"
        "[MOD]\n<STRING>DIRECTORY:D:/Games/Mods/Second\n<INTEGER>PRIORITY:1\n[/MOD]\n"
        "[MOD]\n<STRING>DIRECTORY:First\n<INTEGER>PRIORITY:0\n[/MOD]\n"
        "[MOD]\n<STRING>DIRECTORY:Off\n<INTEGER>PRIORITY:-1\n[/MOD]\n"
        "[MOD]\n<STRING>DIRECTORY:Clash\n<INTEGER>PRIORITY:0\n[/MOD]\n"
        "[/MODS]\n");
    TL_CHECK(listed.ok);
    const tl::core::ModList list =
        tl::core::arrange_mods(&listed.root, {"Clash", "first", "New", "Off", "Second"});
    TL_CHECK(
        (folders_of(list) == std::vector<std::string>{"First", "Second", "Off", "Clash", "New"}));
    TL_CHECK(list.mods[2].priority == -1);
    TL_CHECK(!list.mods[2].active());
    TL_CHECK(list.mods[4].priority == tl::core::kNewModPriority);
    TL_CHECK(list.found_new);
}

/// CHECKFORNEW false: a folder `mods.dat` does not name is left out.
TL_TEST(mods_check_for_new_off) {
    const tl::formats::AdmFile listed = parse(
        "[MODS]\n<BOOL>CHECKFORNEW:false\n"
        "[MOD]\n<STRING>DIRECTORY:Kept\n<INTEGER>PRIORITY:0\n<BOOL>COMPRESSED:true\n[/MOD]\n"
        "[/MODS]\n");
    const tl::core::ModList list = tl::core::arrange_mods(&listed.root, {"Kept", "Unlisted"});
    TL_CHECK((folders_of(list) == std::vector<std::string>{"Kept"}));
    TL_CHECK(!list.found_new);
}

/// No `mods.dat`: every folder, in name order, on.
TL_TEST(mods_without_a_list_are_all_on) {
    const tl::core::ModList list = tl::core::arrange_mods(nullptr, {"A", "B"});
    TL_CHECK(list.mods.size() == 2);
    TL_CHECK(list.mods[0].active() && list.mods[1].active());
}

/// `saveModFile`'s file reads back into the same order and the same switched-off mods.
TL_TEST(mods_dat_round_trips) {
    tl::core::ModList list = tl::core::arrange_mods(nullptr, {"Alpha", "Beta", "Gamma"});
    list.mods[1].priority = -1;
    const std::vector<uint8_t> bytes = tl::core::mods_dat_bytes(list);
    TL_CHECK(bytes.size() > 2 && bytes[0] == 0xFF && bytes[1] == 0xFE);
    const tl::formats::AdmFile file = tl::formats::parse_dat_text(bytes.data(), bytes.size());
    TL_CHECK(file.ok);
    TL_CHECK(file.section() == "MODS");
    const tl::core::ModList again = tl::core::arrange_mods(&file.root, {"Alpha", "Beta", "Gamma"});
    TL_CHECK((folders_of(again) == std::vector<std::string>{"Alpha", "Gamma", "Beta"}));
    TL_CHECK(again.mods[0].priority == 0);
    TL_CHECK(again.mods[1].priority == 2);
    TL_CHECK(again.mods[2].priority == -1);
    TL_CHECK(!again.found_new);
    TL_CHECK((tl::core::enabled_mod_names(again).size() == 2));
}

/// A mods directory on disk: `mod.dat` names a mod, an empty folder is passed over, and the
/// active ones are the roots files are looked for in.
TL_TEST(mods_loaded_from_a_directory) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "tl_mods_test";
    std::filesystem::remove_all(dir);
    write_text(dir / "Necro" / "mod.dat",
               "[MOD]\n<STRING>NAME:Necromancer\n<STRING>AUTHOR:Someone\n[/MOD]\n");
    write_text(dir / "Necro" / "media" / "units" / "x.dat", "[UNIT]\n[/UNIT]\n");
    write_text(dir / "Plain" / "media" / "a.txt", "a");
    std::filesystem::create_directories(dir / "Empty");
    const tl::core::ModList list = tl::core::load_mods(dir.string());
    TL_CHECK(list.mods.size() == 3);
    TL_CHECK(list.mods[0].folder == "Empty" && !list.mods[0].active());
    TL_CHECK(list.mods[1].name == "Necromancer" && list.mods[1].author == "Someone");
    TL_CHECK(list.mods[2].name == "Plain");
    TL_CHECK(
        (tl::core::enabled_mod_names(list) == std::vector<std::string>{"Necromancer", "Plain"}));
    TL_CHECK(tl::core::active_mod_roots(list).size() == 2);
    TL_CHECK(tl::core::save_mods(list));
    TL_CHECK(std::filesystem::exists(dir / "mods.dat"));
    std::filesystem::remove_all(dir);
}

namespace {

tl::core::ModInfo* find_mod(tl::core::ModList& list, const char* folder) {
    for (tl::core::ModInfo& mod : list.mods) {
        if (mod.folder == folder) return &mod;
    }
    return nullptr;
}

}  // namespace

/// The port's own mods - the free camera and the FPS overlay - are made once, each on as its old
/// `config.txt` line said; neither is a mod a save records.
TL_TEST(port_mods_made_once) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "tl_mods_camera";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string mods = (dir / "mods").string();
    tl::core::ModList list = tl::core::load_game_mods("", mods, false, true);
    TL_CHECK(std::filesystem::exists(dir / "mods" / "FreeCamera" / "mod.dat"));
    TL_CHECK(std::filesystem::exists(dir / "mods" / "FpsOverlay" / "mod.dat"));
    TL_CHECK(list.mods.size() == 2);
    const tl::core::ModInfo* camera = find_mod(list, tl::core::kFreeCameraMod);
    const tl::core::ModInfo* overlay = find_mod(list, tl::core::kFpsOverlayMod);
    TL_CHECK(camera != nullptr && camera->free_camera && camera->priority == -1);
    TL_CHECK(overlay != nullptr && overlay->fps_overlay && overlay->priority >= 0);
    TL_CHECK(!tl::core::free_camera_mod_on());
    TL_CHECK(tl::core::fps_overlay_mod_on());
    TL_CHECK(tl::core::enabled_mod_names(list).empty());
    // Deleted by the player: mods.dat still names it, so it is not made again.
    std::filesystem::remove_all(dir / "mods" / "FreeCamera");
    list = tl::core::load_game_mods("", mods, true, true);
    TL_CHECK(!std::filesystem::exists(dir / "mods" / "FreeCamera"));
    TL_CHECK(!tl::core::free_camera_mod_on());
    std::filesystem::remove_all(dir);
}

/// Switched in mods.dat (as the mods page does), the port's own mods act only while they are on,
/// and have nothing to look in, so switching one remounts nothing.
TL_TEST(port_mods_switched) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "tl_mods_switched";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string mods = (dir / "mods").string();
    tl::core::ModList list = tl::core::load_game_mods("", mods, false, true);
    tl::core::ModInfo* camera = find_mod(list, tl::core::kFreeCameraMod);
    tl::core::ModInfo* overlay = find_mod(list, tl::core::kFpsOverlayMod);
    TL_CHECK(camera != nullptr && overlay != nullptr);
    if (camera == nullptr || overlay == nullptr) return;
    camera->priority = 0;
    overlay->priority = -1;
    TL_CHECK(tl::core::save_mods(list));
    // Made no second time: the old lines no longer decide.
    list = tl::core::load_game_mods("", mods, false, true);
    TL_CHECK(tl::core::free_camera_mod_on());
    TL_CHECK(!tl::core::fps_overlay_mod_on());
    TL_CHECK(tl::core::enabled_mod_names(list).empty());
    TL_CHECK(tl::core::active_mod_roots(list).empty());
    std::filesystem::remove_all(dir);
}

/// A compiled-data reader handed a mod's text file reads it as text.
TL_TEST(adm_reader_takes_text) {
    const std::string text = "[UNIT]\n<STRING>NAME:Necromancer\n[/UNIT]\n";
    const tl::formats::AdmFile file =
        tl::formats::parse_adm(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    TL_CHECK(file.ok);
    TL_CHECK(file.string_or("NAME", "") == "Necromancer");
}

/// The mods' classes after the console's three on the top row; the other rows repeat their last
/// window, so the focus goes down from a mod's class and across the pets as on the console.
TL_TEST(new_game_grid_takes_mod_classes) {
    const tl::core::NewGameGrid grid = tl::core::new_game_grid({"Necromancer", "Ammut Disciple"});
    TL_CHECK(grid.size() == static_cast<size_t>(tl::core::kNewGameRows));
    TL_CHECK(grid[0].size() == 5);
    TL_CHECK(grid[0][3] == "Necromancer" && grid[0][4] == "Ammut Disciple");
    TL_CHECK(grid[2][4] == "Chakawary" && grid[1][4] == "EditBox");
    const auto everything = [](const std::string&) { return true; };
    tl::core::NewGameFocus focus;
    focus.column = 2;
    focus = tl::core::step_new_game_focus(grid, focus, 1, 0, everything);
    TL_CHECK(tl::core::new_game_window(grid, focus) == "Necromancer");
    focus = tl::core::step_new_game_focus(grid, focus, 0, 1, everything);
    TL_CHECK(tl::core::new_game_window(grid, focus) == "EditBox");
    focus.row = 0;
    focus.column = 4;
    focus = tl::core::step_new_game_focus(grid, focus, 1, 0, everything);
    TL_CHECK(tl::core::new_game_window(grid, focus) == "Destroyer");
    // With none, the console's own grid.
    TL_CHECK(tl::core::new_game_grid({})[0].size() == 3);
}

namespace {

/// Two mods, the first on and the second off, the second with an author.
tl::core::ModList two_mods() {
    tl::core::ModList list;
    list.mods_dir = "mods/";
    for (const char* folder : {"Ammut", "Necro"}) {
        tl::core::ModInfo mod;
        mod.folder = folder;
        mod.name = folder;
        mod.priority = 0;
        list.mods.push_back(mod);
    }
    list.mods[1].priority = -1;
    list.mods[1].author = "Someone";
    return list;
}

/// The longest line of `text`.
size_t longest_line(const std::string& text) {
    size_t longest = 0;
    size_t start = 0;
    for (size_t at = 0; at <= text.size(); ++at) {
        if (at == text.size() || text[at] == '\n') {
            longest = std::max(longest, at - start);
            start = at + 1;
        }
    }
    return longest;
}

}  // namespace

/// The mods page: "All mods" switches every one on unless all are; a mod switched back on takes
/// the new-folder priority, so `mods.dat` lists it on in its place.
TL_TEST(mods_menu_switches_rows) {
    tl::core::ModList list = two_mods();
    TL_CHECK((tl::core::mods_menu_rows(list) ==
              std::vector<std::string>{"All mods: Off", "Ammut: On", "Necro: Off"}));
    TL_CHECK(tl::core::mods_menu_row_count(list) == 3);
    tl::core::switch_mods_menu_row(list, 0);
    TL_CHECK(list.mods[0].priority == 0 && list.mods[1].priority == tl::core::kNewModPriority);
    TL_CHECK(tl::core::mods_menu_rows(list)[0] == "All mods: On");
    tl::core::switch_mods_menu_row(list, 0);
    TL_CHECK(list.mods[0].priority == -1 && list.mods[1].priority == -1);
    tl::core::switch_mods_menu_row(list, 2);
    TL_CHECK(list.mods[1].priority == tl::core::kNewModPriority);
    tl::core::switch_mods_menu_row(list, 9);  // past the list: nothing
}

/// The text beside the column is wrapped.
TL_TEST(mods_menu_text) {
    tl::core::ModList list = two_mods();
    TL_CHECK(tl::core::mods_menu_info(list, 2, 40) == "Necro\n\nby Someone");
    list.mods_dir = "D:/Games/TorchlightRekindled/SavedGames/mods/";
    const std::string about = tl::core::mods_menu_info(list, -1, 20);
    TL_CHECK(about.rfind("Mods\n\n", 0) == 0);
    TL_CHECK(longest_line(about) <= 20);
    const std::string checkbox = tl::core::mods_menu_checkbox_info(40);
    TL_CHECK(checkbox.find("mods_menu=1") != std::string::npos);
    TL_CHECK(longest_line(checkbox) <= 40);
}

/// The focus: the first row first; up and down go round the rows and never reach the checkbox.
/// The column scrolls to keep the focused row shown.
TL_TEST(mods_page_focus_scrolls) {
    using Focus = tl::core::ModsPageFocus;
    using tl::core::kModsPageRows;
    using tl::core::step_mods_page_focus;
    const size_t rows = kModsPageRows + 4;
    Focus focus;
    TL_CHECK(focus.on == Focus::On::Row && focus.row == 0 && focus.top == 0);
    for (size_t i = 0; i + 1 < rows; ++i) focus = step_mods_page_focus(focus, rows, 1);
    TL_CHECK(focus.on == Focus::On::Row && focus.row == rows - 1);
    TL_CHECK(focus.top == rows - kModsPageRows);
    // Down from the last row: round to the first, not the checkbox.
    focus = step_mods_page_focus(focus, rows, 1);
    TL_CHECK(focus.on == Focus::On::Row && focus.row == 0 && focus.top == 0);
    // Up from the first row: round to the last.
    focus = step_mods_page_focus(focus, rows, -1);
    TL_CHECK(focus.on == Focus::On::Row && focus.row == rows - 1);
    TL_CHECK(focus.top == rows - kModsPageRows);
    // Up to the first row shown and one more: the column scrolls by one.
    const size_t top = focus.top;
    for (size_t i = 0; i < kModsPageRows; ++i) focus = step_mods_page_focus(focus, rows, -1);
    TL_CHECK(focus.row == top - 1 && focus.top == top - 1);
    // A short list never scrolls, and says nothing about it.
    focus = step_mods_page_focus(Focus{}, 3, -1);
    TL_CHECK(focus.on == Focus::On::Row && focus.row == 2 && focus.top == 0);
    TL_CHECK(tl::core::mods_menu_scroll_note(3, 0).empty());
    TL_CHECK(tl::core::mods_menu_scroll_note(rows, 2) == "Rows 3-11 of 13; up and down scroll.");
}

/// Right alone reaches the checkbox; up and down leave it be, and left goes back to the row it
/// was reached from.
TL_TEST(mods_page_checkbox_by_right) {
    using Focus = tl::core::ModsPageFocus;
    using tl::core::step_mods_page_focus;
    using tl::core::step_mods_page_side;
    const size_t rows = tl::core::kModsPageRows + 4;
    const Focus on_row{Focus::On::Row, 5, 2};
    Focus focus = step_mods_page_side(on_row, 1);
    TL_CHECK(focus.on == Focus::On::Checkbox);
    focus = step_mods_page_focus(focus, rows, 1);
    focus = step_mods_page_focus(focus, rows, -1);
    TL_CHECK(focus.on == Focus::On::Checkbox);
    focus = step_mods_page_side(focus, 1);
    TL_CHECK(focus.on == Focus::On::Checkbox);
    focus = step_mods_page_side(focus, -1);
    TL_CHECK(focus.on == Focus::On::Row && focus.row == on_row.row && focus.top == on_row.top);
    // Left on a row changes nothing.
    TL_CHECK(step_mods_page_side(focus, -1).on == Focus::On::Row);
}
