// Where the game's data files are found: a unit's links, a layout's file, and what a mod folder
// (`--mod-data`) brings beside them, text `.dat` files and `Merge/` layouts, which no shipped
// archive has.
#include "core/log.h"
#include "core/world.h"
#include "formats/dat_text.h"

#include <algorithm>

namespace tl::core {

namespace {
// Whether the mounted data has any `Merge/` layout; -1 until looked. The mounts are the same
// for the whole run.
int g_merge_layouts_present = -1;
}  // namespace

tl::formats::AdmFile World::parse_data_group(const uint8_t* data, size_t size) {
    if (tl::formats::looks_like_dat_text(data, size))
        return tl::formats::parse_dat_text(data, size);
    return tl::formats::parse_adm(data, size);
}

// `CDataGroup::LoadFile`'s link: under `first_root` (`xbox/`), then as named, compiled
// (`.adm`); failing both, a mod's text `.dat`. A mod's `.dat` named outright, as the unit
// index holds it, is read itself before a compiled file of the same name.
std::string World::resolve_unit_link(const std::string& wanted, const std::string& first_root,
                                     bool is_first) const {
    std::string resolved;
    // A mod's file stands in front of the archive's, `xbox/` copy included
    // (`CModFileFilter::getFilePath`, asked before the pak).
    for (const char* extension : {"", ".adm", ".dat"}) {
        if (!is_first && extension[0] == '\0') continue;
        if (mod_has_file(wanted + extension)) return resolve_archive_path(wanted + extension);
    }
    if (is_first && first_root.empty() && wanted.size() > 4 &&
        wanted.compare(wanted.size() - 4, 4, ".dat") == 0) {
        resolved = resolve_archive_path(wanted);
        if (resolved.empty()) resolved = resolve_archive_path(wanted + ".adm");
        return resolved;
    }
    for (const char* extension : {".adm", ".dat"}) {
        if (!first_root.empty()) resolved = resolve_archive_path(first_root + wanted + extension);
        if (resolved.empty()) resolved = resolve_archive_path(wanted + extension);
        if (!resolved.empty()) break;
    }
    return resolved;
}

// PORT: a layout reference as its archive path (with `.cmp` when the bare name is not there);
// empty when the archive has neither.
std::string World::layout_archive_path(const std::string& reference) const {
    if (reference.empty()) return std::string();
    std::string path = resolve_archive_path(reference);
    if (path.empty()) path = resolve_archive_path(reference + ".cmp");
    return path;
}

// INVENTED: a mod's `Merge/*.layout` beside a layout (no shipped archive has one) is
// placed with it, at the same transform, so a mod's town NPCs are part of the level.
// Not the dungeon's chunk walk, which leaves `MERGE/` out (dungeon.cpp).
void World::place_merge_layouts(const std::string& path, const ogre::Vector3& origin,
                                const ogre::Quaternion& basis, int depth) {
    if (g_merge_layouts_present < 0) {
        g_merge_layouts_present = 0;
        for (const char* suffix : {".layout.cmp", ".layout"}) {
            for (const std::string& any : archive_paths_in_listing_order("media/", suffix)) {
                if (tl::core::upper_ascii(any).find("/MERGE/") != std::string::npos) {
                    g_merge_layouts_present = 1;
                }
            }
        }
    }
    const size_t slash = path.find_last_of('/');
    // Not a layout made in place (`populate:`), nor one that is a merge itself.
    if (g_merge_layouts_present == 0 || slash == std::string::npos ||
        path.rfind("populate:", 0) == 0 ||
        tl::core::upper_ascii(path).find("/MERGE/") != std::string::npos) {
        return;
    }
    const std::string merge_dir = path.substr(0, slash + 1) + "Merge/";
    std::vector<std::string> merge_layouts = archive_paths_under(merge_dir, ".layout.cmp");
    for (const std::string& text_layout : archive_paths_under(merge_dir, ".layout")) {
        const std::string compiled = text_layout + ".cmp";
        if (resolve_archive_path(compiled).empty()) merge_layouts.push_back(text_layout);
    }
    const auto upper_less = [](const std::string& a, const std::string& b) {
        return tl::core::upper_ascii(a) < tl::core::upper_ascii(b);
    };
    const auto upper_same = [](const std::string& a, const std::string& b) {
        return tl::core::upper_ascii(a) == tl::core::upper_ascii(b);
    };
    std::sort(merge_layouts.begin(), merge_layouts.end(), upper_less);
    merge_layouts.erase(std::unique(merge_layouts.begin(), merge_layouts.end(), upper_same),
                        merge_layouts.end());
    for (const std::string& merge_layout : merge_layouts) {
        const std::string resolved = resolve_archive_path(merge_layout);
        if (resolved.empty()) continue;
        TL_LOG_INFO("  applying merge layout %s", resolved.c_str());
        place_layout(resolved, origin, basis, depth + 1);
    }
}

}  // namespace tl::core
