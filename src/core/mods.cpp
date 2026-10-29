#include "core/mods.h"

#include "core/log.h"
#include "core/run_config.h"
#include "formats/dat_text.h"

#include <dirent.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>

namespace tl::core {

namespace {

std::string upper_ascii(std::string text) {
    for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

bool is_directory(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// The names in `dir` that are directories (`want_dirs`) or not, in name order.
// DIVERGENCE (platform): `FILESYSTEM::GetDirectoryList` lists in the filesystem's own order;
// sorted here, as NTFS lists, so every device orders a mods directory alike.
std::vector<std::string> list_entries(const std::string& dir, bool want_dirs) {
    std::vector<std::string> names;
    DIR* listing = opendir(dir.c_str());
    if (listing == nullptr) return names;
    while (dirent* entry = readdir(listing)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (is_directory(dir + name) == want_dirs) names.push_back(name);
    }
    closedir(listing);
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        return upper_ascii(a) < upper_ascii(b);
    });
    return names;
}

// `CMod::reloadModFiles` lists `*.*` under the folder; a mod with none is
// left off (`+0x70`). Only whether there is one matters here.
bool has_any_file(const std::string& dir) {
    if (!list_entries(dir, false).empty()) return true;
    for (const std::string& child : list_entries(dir, true)) {
        if (has_any_file(dir + child + "/")) return true;
    }
    return false;
}

bool read_file(const std::string& path, std::vector<uint8_t>& bytes) {
    std::FILE* in = std::fopen(path.c_str(), "rb");
    if (in == nullptr) return false;
    bool read = std::fseek(in, 0, SEEK_END) == 0;
    const long size = read ? std::ftell(in) : -1;
    read = size >= 0 && std::fseek(in, 0, SEEK_SET) == 0;
    bytes.assign(read ? static_cast<size_t>(size) : 0u, 0);
    if (read && !bytes.empty())
        read = std::fread(bytes.data(), 1, bytes.size(), in) == bytes.size();
    std::fclose(in);
    return read;
}

// A file of the mods directory in any case: the PC's filesystem is case-blind, a device's not.
std::string find_in(const std::string& dir, const std::string& name) {
    for (const std::string& file : list_entries(dir, false)) {
        if (upper_ascii(file) == upper_ascii(name)) return dir + file;
    }
    return dir + name;
}

bool make_directory(const std::string& path) {
#if defined(_WIN32)
    return _mkdir(path.c_str()) == 0;
#else
    return mkdir(path.c_str(), 0777) == 0;
#endif
}

bool g_free_camera = false;
bool g_fps_overlay = false;

std::string with_slash(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    if (!path.empty() && path.back() != '/') path += '/';
    return path;
}

// The folder a `mods.dat` DIRECTORY names: its last component.
// DIVERGENCE (platform): the PC writes the folder's absolute path; a device's game folder can
// move, so the folder's own name is written, and either form is read.
std::string folder_of(const std::string& directory) {
    std::string path = with_slash(directory);
    path.pop_back();
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void append_utf16(std::vector<uint8_t>& out, const std::string& text) {
    // The text is UTF-8; what a data file holds is UTF-16 code units.
    for (size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        uint32_t code = c;
        size_t length = 1;
        if (c >= 0xF0 && i + 3 < text.size()) {
            code = ((c & 0x07u) << 18) | ((text[i + 1] & 0x3Fu) << 12) |
                   ((text[i + 2] & 0x3Fu) << 6) | (text[i + 3] & 0x3Fu);
            length = 4;
        } else if (c >= 0xE0 && i + 2 < text.size()) {
            code = ((c & 0x0Fu) << 12) | ((text[i + 1] & 0x3Fu) << 6) | (text[i + 2] & 0x3Fu);
            length = 3;
        } else if (c >= 0xC0 && i + 1 < text.size()) {
            code = ((c & 0x1Fu) << 6) | (text[i + 1] & 0x3Fu);
            length = 2;
        }
        i += length;
        if (code >= 0x10000) {
            code -= 0x10000;
            const uint32_t high = 0xD800 + (code >> 10);
            const uint32_t low = 0xDC00 + (code & 0x3FF);
            out.push_back(static_cast<uint8_t>(high));
            out.push_back(static_cast<uint8_t>(high >> 8));
            code = low;
        }
        out.push_back(static_cast<uint8_t>(code));
        out.push_back(static_cast<uint8_t>(code >> 8));
    }
}

}  // namespace

std::string mods_directory(const std::string& game_data) {
    const std::string root = with_slash(game_data.empty() ? std::string("./") : game_data);
    for (const std::string& dir : list_entries(root, true)) {
        if (upper_ascii(dir) == "MODS") return root + dir + "/";
    }
    return root + "mods/";
}

// `CModFileFilter::CModFileFilter`, its ordering. A listed mod takes the slot
// of its PRIORITY; one whose slot is taken, or that is switched off, goes after them; the
// folders `mods.dat` does not name come last, at `kNewModPriority`.
ModList arrange_mods(const tl::formats::AdmSection* mods_dat,
                     const std::vector<std::string>& folders) {
    ModList list;
    std::vector<std::optional<ModInfo>> slots;
    std::vector<ModInfo> after;
    if (mods_dat != nullptr) {
        list.check_for_new = mods_dat->bool_or("CHECKFORNEW", true);
        for (const tl::formats::AdmSection& listed : mods_dat->children) {
            const auto priority = static_cast<int32_t>(listed.integer_or("PRIORITY", UINT32_MAX));
            if (!listed.bool_or("COMPRESSED", false) && priority != -1) list.found_new = true;
            const std::string directory = listed.string_or("DIRECTORY", "");
            if (directory.empty()) continue;
            ModInfo mod;
            mod.folder = folder_of(directory);
            mod.priority = priority;
            while (static_cast<int32_t>(slots.size()) <= priority) slots.emplace_back();
            if (priority < 0 || slots[static_cast<size_t>(priority)].has_value()) {
                after.push_back(std::move(mod));
            } else {
                slots[static_cast<size_t>(priority)] = std::move(mod);
            }
        }
    }
    for (std::optional<ModInfo>& slot : slots) {
        if (slot.has_value()) list.mods.push_back(std::move(*slot));
    }
    for (ModInfo& mod : after) list.mods.push_back(std::move(mod));
    if (!list.check_for_new) return list;
    for (const std::string& folder : folders) {
        const bool known = std::any_of(list.mods.begin(), list.mods.end(), [&](const ModInfo& mod) {
            return upper_ascii(mod.folder) == upper_ascii(folder);
        });
        if (known) continue;
        ModInfo mod;
        mod.folder = folder;
        mod.priority = kNewModPriority;
        list.mods.push_back(std::move(mod));
        list.found_new = true;
    }
    return list;
}

// `CModFileFilter::CModFileFilter` with `CMod::CMod` for
// each mod: its name is its folder's until `mod.dat` gives one.
ModList load_mods(const std::string& mods_dir) {
    const std::string dir = with_slash(mods_dir);
    if (!is_directory(dir)) return ModList{};
    std::vector<uint8_t> bytes;
    tl::formats::AdmFile listed;
    if (read_file(find_in(dir, "mods.dat"), bytes)) {
        listed = tl::formats::parse_dat_text(bytes.data(), bytes.size());
        if (!listed.ok)
            TL_LOG_WARN("mods: could not read %smods.dat: %s", dir.c_str(), listed.error.c_str());
    }
    const std::vector<std::string> folders = list_entries(dir, true);
    ModList list = arrange_mods(listed.ok ? &listed.root : nullptr, folders);
    list.mods_dir = dir;
    for (ModInfo& mod : list.mods) {
        // The folder as it is spelt on disk, which a device's filesystem minds.
        for (const std::string& folder : folders) {
            if (upper_ascii(folder) == upper_ascii(mod.folder)) mod.folder = folder;
        }
        mod.directory = dir + mod.folder + "/";
        mod.name = mod.folder;
        if (read_file(find_in(mod.directory, "mod.dat"), bytes)) {
            const tl::formats::AdmFile info =
                tl::formats::parse_dat_text(bytes.data(), bytes.size());
            if (info.ok) {
                mod.name = info.string_or("NAME", mod.name);
                mod.author = info.string_or("AUTHOR", "");
                mod.description = info.string_or("DESCRIPTION", "");
                mod.free_camera = info.bool_or("FREE_CAMERA", false);
                mod.fps_overlay = info.bool_or("FPS_OVERLAY", false);
            }
        }
        // `reloadModFiles` runs for a mod that is on, and only that one learns it is empty.
        if (mod.priority >= 0)
            mod.has_files = is_directory(mod.directory) && has_any_file(mod.directory);
        TL_LOG_INFO("mod: '%s' (%s), priority %d%s", mod.name.c_str(), mod.directory.c_str(),
                    mod.priority,
                    mod.active()       ? ""
                    : mod.priority < 0 ? ", switched off"
                                       : ", empty");
    }
    return list;
}

// DIVERGENCE (platform): the free camera and the frame-rate overlay are the port's own, each a
// mod of its own so it is switched where the mods are. Made once: a `mods.dat` naming one (a
// player who deleted it) is left be.
bool make_port_mod(const std::string& dir, const char* mod_folder, const std::string& info) {
    for (const std::string& folder : list_entries(dir, true)) {
        if (upper_ascii(folder) == upper_ascii(mod_folder)) return false;
    }
    std::vector<uint8_t> bytes;
    if (read_file(find_in(dir, "mods.dat"), bytes)) {
        const tl::formats::AdmFile listed = tl::formats::parse_dat_text(bytes.data(), bytes.size());
        for (const tl::formats::AdmSection& mod : listed.root.children) {
            if (upper_ascii(folder_of(mod.string_or("DIRECTORY", ""))) == upper_ascii(mod_folder))
                return false;
        }
    }
    make_directory(dir.substr(0, dir.size() - 1));
    const std::string folder = dir + mod_folder;
    std::vector<uint8_t> text{0xFF, 0xFE};
    append_utf16(text, "[MOD]\r\n" + info + "[/MOD]\r\n");
    std::FILE* out =
        make_directory(folder) ? std::fopen((folder + "/mod.dat").c_str(), "wb") : nullptr;
    if (out == nullptr) return false;
    const bool written = std::fwrite(text.data(), 1, text.size(), out) == text.size();
    return std::fclose(out) == 0 && written;
}

bool free_camera_mod_on() { return g_free_camera; }

bool fps_overlay_mod_on() { return g_fps_overlay; }

// `CFileSystem::reload`.
ModList load_game_mods(const std::string& game_data, const std::string& mods_dir, bool free_camera,
                       bool fps_overlay) {
    const std::string dir = with_slash(mods_dir.empty() ? mods_directory(game_data) : mods_dir);
    const bool made_camera = make_port_mod(
        dir, kFreeCameraMod,
        "<STRING>NAME:Free Camera\r\n<STRING>AUTHOR:Torchlight Rekindled\r\n"
        "<STRING>DESCRIPTION:The right stick turns the camera and R3 resets it; a pinch "
        "zooms.\r\n<BOOL>FREE_CAMERA:true\r\n");
    const bool made_overlay = make_port_mod(
        dir, kFpsOverlayMod,
        "<STRING>NAME:FPS Overlay\r\n<STRING>AUTHOR:Torchlight Rekindled\r\n"
        "<STRING>DESCRIPTION:The frame rate and the screen's size in a corner. L3 hides and "
        "shows it while the game runs.\r\n<BOOL>FPS_OVERLAY:true\r\n");
    ModList mods = load_mods(dir);
    g_free_camera = false;
    g_fps_overlay = false;
    for (ModInfo& mod : mods.mods) {
        const std::string folder = upper_ascii(mod.folder);
        if ((made_camera && !free_camera && folder == upper_ascii(kFreeCameraMod)) ||
            (made_overlay && !fps_overlay && folder == upper_ascii(kFpsOverlayMod))) {
            mod.priority = -1;
        }
        g_free_camera = g_free_camera || (mod.active() && mod.free_camera);
        g_fps_overlay = g_fps_overlay || (mod.active() && mod.fps_overlay);
    }
    // DIVERGENCE (platform): `mods.dat` is written now rather than by the filter's destructor at
    // exit (`CModFileFilter::~CModFileFilter`), which a device's game seldom reaches.
    if ((mods.found_new || made_camera || made_overlay) && !save_mods(mods)) {
        TL_LOG_WARN("mods: could not write %smods.dat", mods.mods_dir.c_str());
    }
    return mods;
}

// `CFileSystem::reload` makes no mod filter under `SAFEMODE`.
void mods_from_command_line(int argc, char** argv, const std::string& game_data,
                            std::vector<std::string>& roots, std::vector<std::string>& names,
                            ModList* shown) {
    std::string mods_dir;
    bool safe_mode = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_next = i + 1 < argc;
        if (arg == "--mod-data" && has_next) roots.emplace_back(argv[++i]);
        if (arg == "--mods-dir" && has_next) mods_dir = argv[++i];
        if (arg == "--safemode") safe_mode = true;
    }
    // DIVERGENCE (platform): `config.txt`'s `mods_menu=0` is the mods page's "Don't show this menu
    // again", and starts as `SAFEMODE` does.
    g_free_camera = false;
    g_fps_overlay = false;
    if (safe_mode || (kModsPage && !run_config().mods_menu)) return;
    const ModList mods =
        load_game_mods(game_data, mods_dir, run_config().free_camera, run_config().fps);
    for (const std::string& root : active_mod_roots(mods)) roots.push_back(root);
    names = enabled_mod_names(mods);
    if (shown != nullptr) *shown = mods;
}

namespace {

// The free camera's and the overlay's: they change no game data.
bool port_mod(const ModInfo& mod) {
    const std::string folder = upper_ascii(mod.folder);
    return folder == upper_ascii(kFreeCameraMod) || folder == upper_ascii(kFpsOverlayMod);
}

}  // namespace

// The port's own mods are not looked in, so switching one remounts nothing.
std::vector<std::string> active_mod_roots(const ModList& list) {
    std::vector<std::string> roots;
    for (const ModInfo& mod : list.mods) {
        if (mod.active() && !port_mod(mod))
            roots.push_back(mod.directory.substr(0, mod.directory.size() - 1));
    }
    return roots;
}

// `CModFileFilter::getEnabledModNames`.
// DIVERGENCE: the original tests only `+0x70`, which a switched-off mod keeps, so it lists
// that mod too; switching a mod off here takes it out of a save's list. The port's own mods
// change no game data and are left out.
std::vector<std::string> enabled_mod_names(const ModList& list) {
    std::vector<std::string> names;
    for (const ModInfo& mod : list.mods) {
        if (mod.active() && !port_mod(mod)) names.push_back(mod.name);
    }
    return names;
}

// `CModFileFilter::saveModFile`.
std::vector<uint8_t> mods_dat_bytes(const ModList& list) {
    std::string text = "[MODS]\r\n";
    text += std::string("<BOOL>CHECKFORNEW:") + (list.check_for_new ? "true" : "false") + "\r\n";
    int32_t index = 0;
    for (const ModInfo& mod : list.mods) {
        // The original numbers by the mod's place in the whole list, not among the active ones.
        if (mod.priority >= 0) {
            text += "[MOD]\r\n<STRING>DIRECTORY:" + mod.folder +
                    "\r\n<INTEGER>PRIORITY:" + std::to_string(index) +
                    "\r\n<BOOL>COMPRESSED:true\r\n[/MOD]\r\n";
        }
        ++index;
    }
    for (const ModInfo& mod : list.mods) {
        if (mod.priority < 0) {
            text += "[MOD]\r\n<STRING>DIRECTORY:" + mod.folder +
                    "\r\n<INTEGER>PRIORITY:-1\r\n<BOOL>COMPRESSED:false\r\n[/MOD]\r\n";
        }
    }
    text += "[/MODS]\r\n";
    std::vector<uint8_t> bytes{0xFF, 0xFE};
    append_utf16(bytes, text);
    return bytes;
}

bool save_mods(const ModList& list) {
    if (list.mods_dir.empty()) return false;
    const std::vector<uint8_t> bytes = mods_dat_bytes(list);
    std::FILE* out = std::fopen(find_in(list.mods_dir, "mods.dat").c_str(), "wb");
    if (out == nullptr) return false;
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), out) == bytes.size();
    return std::fclose(out) == 0 && written;
}

namespace {

// `text` broken at spaces into lines of at most `width` characters; a longer word (a mods
// directory's path) is cut every `width` characters.
std::string wrap_text(const std::string& text, size_t width) {
    std::string out;
    size_t line = 0;
    size_t at = 0;
    while (at < text.size()) {
        if (text[at] == '\n') {
            out += '\n';
            line = 0;
            ++at;
            continue;
        }
        const size_t end =
            std::min(text.find_first_of(" \n", at), std::min(text.size(), at + width));
        const std::string word = text.substr(at, end - at);
        if (line > 0 && line + 1 + word.size() > width) {
            out += '\n';
            line = 0;
        } else if (line > 0) {
            out += ' ';
            ++line;
        }
        out += word;
        line += word.size();
        at = end < text.size() && text[end] == ' ' ? end + 1 : end;
    }
    return out;
}

bool menu_on(const ModInfo& mod) { return mod.priority >= 0; }

}  // namespace

bool mods_menu_row_on(const ModList& list, size_t row) {
    if (row == 0) return std::all_of(list.mods.begin(), list.mods.end(), menu_on);
    return row <= list.mods.size() && menu_on(list.mods[row - 1]);
}

std::vector<std::string> mods_menu_rows(const ModList& list) {
    std::vector<std::string> rows{std::string("All mods: ") +
                                  (mods_menu_row_on(list, 0) ? "On" : "Off")};
    for (const ModInfo& mod : list.mods)
        rows.push_back(mod.name + (menu_on(mod) ? ": On" : ": Off"));
    return rows;
}

void switch_mods_menu_row(ModList& list, size_t row) {
    const auto set = [](ModInfo& mod, bool on) {
        if (on != menu_on(mod)) mod.priority = on ? kNewModPriority : -1;
    };
    if (row == 0) {
        const bool all = mods_menu_row_on(list, 0);
        for (ModInfo& mod : list.mods) set(mod, !all);
    } else if (row <= list.mods.size()) {
        ModInfo& mod = list.mods[row - 1];
        set(mod, !menu_on(mod));
    }
}

ModsPageFocus step_mods_page_focus(ModsPageFocus focus, size_t rows, int direction) {
    using On = ModsPageFocus::On;
    if (focus.on != On::Row || rows == 0) return focus;
    const size_t shown = std::min(kModsPageRows, rows);
    if (direction > 0) {
        if (focus.row + 1 < rows) {
            ++focus.row;
            if (focus.row >= focus.top + shown) focus.top = focus.row + 1 - shown;
        } else {
            focus = ModsPageFocus{On::Row, 0, 0};
        }
    } else if (direction < 0) {
        if (focus.row > 0) {
            --focus.row;
            focus.top = std::min(focus.top, focus.row);
        } else {
            focus.row = rows - 1;
            focus.top = rows - shown;
        }
    }
    return focus;
}

ModsPageFocus step_mods_page_side(ModsPageFocus focus, int side) {
    using On = ModsPageFocus::On;
    if (side > 0) focus.on = On::Checkbox;
    if (side < 0) focus.on = On::Row;
    return focus;
}

std::string mods_menu_checkbox_info(size_t width) {
    return wrap_text(
        "Don't show this menu again\n\nTicked, the game starts straight away from now on, "
        "without this menu and without any mods.\n\nTo bring both back, set mods_menu=1 in "
        "config.txt.",
        width);
}

std::string mods_menu_scroll_note(size_t rows, size_t top) {
    if (rows <= kModsPageRows) return {};
    return "Rows " + std::to_string(top + 1) + "-" + std::to_string(top + kModsPageRows) + " of " +
           std::to_string(rows) + "; up and down scroll.";
}

std::string mods_menu_info(const ModList& list, int row, size_t width) {
    if (row <= 0 || static_cast<size_t>(row) > list.mods.size()) {
        return wrap_text("Mods\n\nA mod is a folder in " + list.mods_dir +
                             ". Its files are read in place of the game's own.\n\nA switches "
                             "a mod on or off; Start plays.",
                         width);
    }
    const ModInfo& mod = list.mods[static_cast<size_t>(row) - 1];
    std::string text = mod.name;
    if (!mod.description.empty()) text += "\n\n" + mod.description;
    if (!mod.author.empty()) text += "\n\nby " + mod.author;
    return wrap_text(text, width);
}

}  // namespace tl::core
