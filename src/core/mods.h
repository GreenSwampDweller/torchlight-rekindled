// The game's mods: the folders of a `mods/` directory beside the game data, each laid out as the
// archive is (`media/...`), whose files are found before the archive's. `CModFileFilter` and
// `CMod` (the PC releases'; the Xbox 360 build has no mods).
#pragma once

#include "formats/adm.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tl::core {

/// The priority `CModFileFilter::CModFileFilter` gives a folder `mods.dat` does not list yet.
inline constexpr int32_t kNewModPriority = 10000;

struct ModInfo {
    std::string folder;        ///< its folder's name in the mods directory
    std::string directory;     ///< `CMod +0x68`: the folder's path, ending in '/'
    std::string name;          ///< `+0x50`: `mod.dat`'s NAME, else the folder's name
    std::string author;        ///< `+0x58`
    std::string description;   ///< `+0x60`
    int32_t priority = -1;     ///< `+0x74`: negative is switched off
    bool has_files = true;     ///< `+0x70` as `reloadModFiles` leaves it: false for an empty folder
    bool free_camera = false;  ///< the port's own `mod.dat` key, `FREE_CAMERA`
    bool fps_overlay = false;  ///< the port's own `mod.dat` key, `FPS_OVERLAY`
    /// `CModFileFilter::getFilePath`'s test: what it looks in.
    bool active() const { return priority >= 0 && has_files; }
};

struct ModList {
    std::string mods_dir;       ///< `+0x10`, ending in '/'
    bool check_for_new = true;  ///< `+0x30`, `mods.dat`'s CHECKFORNEW
    bool found_new = false;     ///< `+0x31`
    /// `+0x18`, in the order a file is looked for in them: by priority, then the folders
    /// `mods.dat` does not list.
    std::vector<ModInfo> mods;
};

/// Where a game data folder keeps its mods: its `mods/` (found in any case), or `mods/` under
/// it when it has none yet.
std::string mods_directory(const std::string& game_data);

/// The order `CModFileFilter::CModFileFilter` puts the mods in, from `mods.dat`'s root group
/// (null when there is none) and the folders of the mods directory: `folder` and `priority`
/// of each entry filled, nothing read from the folders.
ModList arrange_mods(const tl::formats::AdmSection* mods_dat,
                     const std::vector<std::string>& folders);

/// `CModFileFilter::CModFileFilter`: the mods directory's `mods.dat` and folders, each mod's
/// `mod.dat` read and its folder looked into. No directory, no mods.
ModList load_mods(const std::string& mods_dir);

/// The port's own mods: `mods/FreeCamera/`, a `mod.dat` whose `FREE_CAMERA` turns the free
/// camera on (render/touch_camera.h) while the mod is, and `mods/FpsOverlay/`, whose
/// `FPS_OVERLAY` starts the game with the frame-rate overlay.
inline constexpr const char* kFreeCameraMod = "FreeCamera";
inline constexpr const char* kFpsOverlayMod = "FpsOverlay";

/// `CFileSystem::reload`: the mods of `mods_dir`, or of the game data's `mods/` when that is
/// empty; `mods.dat` written when a folder it did not name turned up. The port's own mods are
/// made the first time, on as `free_camera` and `fps_overlay` say (`config.txt`'s lines from
/// before they were mods), and the free camera and the overlay set from the active mods.
ModList load_game_mods(const std::string& game_data, const std::string& mods_dir, bool free_camera,
                       bool fps_overlay);

/// The command line's mods: `--mod-data <dir>` (one mod's folder, first), `--mods-dir <dir>` (in
/// place of the game data's `mods/`), `--safemode` (no mods directory). The folders files are
/// looked for in are added to `roots`, the active mods' names put in `names`.
/// `shown`, when given, receives the mods directory's list (empty under `--safemode`).
void mods_from_command_line(int argc, char** argv, const std::string& game_data,
                            std::vector<std::string>& roots, std::vector<std::string>& names,
                            ModList* shown = nullptr);

/// Whether an active mod turns the free camera on (load_game_mods).
bool free_camera_mod_on();

/// Whether an active mod starts the game with the frame-rate overlay (load_game_mods).
bool fps_overlay_mod_on();

/// The mods files are looked for in, in that order: each one's folder, without the separator.
/// Not the port's own: they have nothing to look in.
std::vector<std::string> active_mod_roots(const ModList& list);

/// `CModFileFilter::getEnabledModNames`: what a save records and Load Character compares.
std::vector<std::string> enabled_mod_names(const ModList& list);

/// `CModFileFilter::saveModFile`'s `mods.dat`: the active mods renumbered from 0 in their
/// order, then the switched-off ones at -1. UTF-16LE with a byte-order mark, as the PC writes
/// its data files.
std::vector<uint8_t> mods_dat_bytes(const ModList& list);

/// Write `mods.dat` into the mods directory; false when it cannot be written.
bool save_mods(const ModList& list);

// The mods page before the title: the port's own, as the Android launcher's was. Its column holds
// "All mods" (row 0) and a row per mod (row 1 + i the list's i-th); to the right, the "Don't show
// this menu again" checkbox (`config.txt`'s `mods_menu=0`), reached by right only. Start plays.

/// How many rows the column shows at once (the console column's nine slots); it scrolls
/// through the rest.
inline constexpr size_t kModsPageRows = 9;

/// The column's rows: "All mods" and one per mod.
inline size_t mods_menu_row_count(const ModList& list) { return list.mods.size() + 1; }

/// Each row's caption: "All mods: On", then "<name>: On" or "Off" per mod.
std::vector<std::string> mods_menu_rows(const ModList& list);

/// Switch a row. A mod switched on that was off takes `kNewModPriority`, one switched off -1;
/// "All mods" switches every one on, or every one off when all are.
void switch_mods_menu_row(ModList& list, size_t row);

/// Whether a row reads On: every mod for row 0, the mod for the rest.
bool mods_menu_row_on(const ModList& list, size_t row);

/// Where the page's focus is - on a row (`row`) or on the checkbox - and the first row the
/// column shows (`top`). On the checkbox, `row` is the row it was reached from.
struct ModsPageFocus {
    enum class On { Row, Checkbox };
    On on = On::Row;
    size_t row = 0;
    size_t top = 0;
};

/// The focus moved up (`direction` -1) or down (1) over `rows` rows, round from either end;
/// never onto the checkbox, and not off it. The column scrolls to keep the focused row shown.
ModsPageFocus step_mods_page_focus(ModsPageFocus focus, size_t rows, int direction);

/// The focus moved left (`side` -1) or right (1): right from a row is the checkbox, left from
/// the checkbox the row it was reached from.
ModsPageFocus step_mods_page_side(ModsPageFocus focus, int side);

/// The text beside the column for the focused `row`: a mod's description and author, wrapped to
/// `width` characters; for row 0 or none, where mods go.
std::string mods_menu_info(const ModList& list, int row, size_t width);

/// The same for the checkbox: what it does and how it is undone.
std::string mods_menu_checkbox_info(size_t width);

/// Under the text, when the column scrolls: which rows it shows. Empty when they all fit.
std::string mods_menu_scroll_note(size_t rows, size_t top);

}  // namespace tl::core
