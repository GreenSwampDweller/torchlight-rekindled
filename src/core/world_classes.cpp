// The playable classes the creation screen offers: the game's three, then each mod's own
// (core/world.h).
#include "core/text.h"
#include "core/world.h"
#include "formats/cegui.h"

#include <algorithm>

namespace tl::core {

namespace {

// The class a players' unit file describes, or false when it is none (a base, a sound table).
bool read_player_class(const World& m, const std::string& path, tl::core::PlayerClass& entry) {
    const std::vector<uint8_t> bytes = m.read_resource(path);
    if (bytes.empty()) return false;
    const tl::formats::AdmFile file = World::parse_data_group(bytes.data(), bytes.size());
    if (!file.ok) {
        TL_LOG_WARN("could not parse '%s': %s", path.c_str(), file.error.c_str());
        return false;
    }
    if (file.root.bool_or("DONTCREATE", false)) return false;
    entry.name = file.string_or("NAME", "");
    entry.display_name = tl::core::strip_markup(file.string_or("DISPLAYNAME", entry.name));
    entry.description = tl::core::strip_markup(file.string_or("DESCRIPTION", ""));
    // `RESOURCEDIRECTORY` plus `MESHFILE`, not the basename (the Destroyer's is
    // `player.MESH`). A mod may leave the directory's separator off.
    std::string dir = file.string_or("RESOURCEDIRECTORY", "");
    const std::string mesh = file.string_or("MESHFILE", "");
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
    if (!dir.empty() && !mesh.empty()) entry.mesh = m.resolve_archive_path(dir + mesh + ".MESH");
    return !entry.name.empty() && !entry.mesh.empty();
}

}  // namespace

/// Read the three playable classes out of `media/units/players/`, in the order
/// `charactercreate.layout` authors its radio buttons; a missing class file is left out.
/// Then each mod's own class, in the order its files are listed.
// INVENTED: the three classes the creation screen offers, by file and icon, in
// this order; the console's creation menu has its own list, not transcribed.
// DIVERGENCE: a mod's class is offered beside them, as the PC's creation screen offers the
// classes a mod's `charactercreate.layout` adds; every mod's, where on the PC one layout wins.
void World::load_player_classes() {
    if (!player_classes.empty()) return;

    // Each entry: the unit file and which of the layout's three icons goes with it.
    struct Known {
        const char* file;
        const char* icon;
    };
    static const Known kKnown[] = {
        {"media/units/players/Destroyer/DESTROYER.DAT.adm", "destroyericon"},
        {"media/units/players/Vanquisher/VANQUISHER.DAT.adm", "vanquishericon"},
        {"media/units/players/Alchemist/ALCHEMIST.DAT.adm", "alchemisticon"},
    };

    for (const Known& known : kKnown) {
        const std::string path = resolve_archive_path(known.file);
        if (path.empty()) {
            TL_LOG_WARN("no '%s' in the archive; that class is not offered", known.file);
            continue;
        }
        tl::core::PlayerClass entry;
        if (!read_player_class(*this, path, entry)) {
            TL_LOG_WARN("'%s' names no unit or no mesh; that class is not offered", path.c_str());
            continue;
        }
        entry.icon = known.icon;
        TL_LOG_INFO("class: %s, %s", entry.display_name.c_str(), entry.mesh.c_str());
        player_classes.push_back(std::move(entry));
    }
    for (const std::string& path : data_paths_under("media/units/players/")) {
        if (!mod_has_file(path)) continue;
        tl::core::PlayerClass entry;
        if (!read_player_class(*this, path, entry)) continue;
        const bool offered = std::any_of(
            player_classes.begin(), player_classes.end(), [&](const tl::core::PlayerClass& known) {
                return tl::core::upper_ascii(known.name) == tl::core::upper_ascii(entry.name);
            });
        if (offered) continue;
        find_mod_class_portrait(entry);
        TL_LOG_INFO("class: %s, %s (a mod's)", entry.display_name.c_str(), entry.mesh.c_str());
        player_classes.push_back(std::move(entry));
    }
    TL_LOG_INFO("%zu playable class(es)", player_classes.size());
}

// PORT: the radio button a mod's `charactercreate.layout` gives its class (the PC's creation
// screen names each class's button after it, `onClick` `guiSelect1`), and the imageset its
// images are in, looked for among the mods' imagesets by `Name`.
void World::find_mod_class_portrait(tl::core::PlayerClass& entry) const {
    const std::string want = tl::core::upper_ascii(entry.name);
    for (const std::vector<uint8_t>& bytes : read_mod_copies("media/UI/charactercreate.layout")) {
        const tl::formats::GuiLayout layout =
            tl::formats::parse_gui_layout(bytes.data(), bytes.size());
        if (!layout.ok) continue;
        const tl::formats::GuiWindow* button = nullptr;
        std::vector<const tl::formats::GuiWindow*> pending{&layout.root};
        while (!pending.empty() && button == nullptr) {
            const tl::formats::GuiWindow* window = pending.back();
            pending.pop_back();
            if (tl::core::upper_ascii(window->name) == want &&
                window->has_property("UnselectedImage")) {
                button = window;
            }
            for (const tl::formats::GuiWindow& child : window->children) pending.push_back(&child);
        }
        if (button == nullptr) continue;
        std::string set;
        const char* const kImages[] = {"UnselectedImage", "HoverImage", "SelectedImage"};
        for (size_t i = 0; i < 3; ++i) {
            std::string image_set;
            if (!tl::formats::parse_image_reference(button->property(kImages[i], std::string()),
                                                    image_set, entry.portrait[i])) {
                entry.portrait[i] = entry.portrait[0];
            }
            if (i == 0) set = image_set;
        }
        for (const std::string& path : archive_paths_under("media/", ".imageset")) {
            if (!mod_has_file(path)) continue;
            const std::vector<uint8_t> file = read_resource(path);
            const tl::formats::Imageset imageset =
                tl::formats::parse_imageset(file.data(), file.size());
            if (imageset.ok && tl::core::upper_ascii(imageset.name) == tl::core::upper_ascii(set)) {
                entry.portrait_imageset = path;
                return;
            }
        }
        TL_LOG_WARN("class %s: no mod imageset '%s' for its portrait", entry.name.c_str(),
                    set.c_str());
        return;
    }
    TL_LOG_WARN("class %s: no mod's charactercreate.layout names a button after it",
                entry.name.c_str());
}

}  // namespace tl::core
