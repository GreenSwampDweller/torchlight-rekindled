// The renderer's resource locations: OGRE's shader library, the game's archive
// and what is mounted beside it. Split from renderer.cpp.
#include "render/menu_chrome.h"
#include "render/renderer_impl.h"

namespace tl::render {

namespace renderer_detail {
/// `CMeshListener`, installed on the mesh manager once the archives are mounted.
Ogre::MeshSerializerListener& game_mesh_listener();
}  // namespace renderer_detail

namespace {

// The PC releases call it Pak.zip, the Xbox 360 pak.zip; a filesystem that minds
// the difference gets whichever is there. `--pak` names one outright.
std::string game_pak(const RendererConfig& config) {
    if (!config.pak.empty()) return config.pak;
    std::string pak = join_path(config.game_data, "Pak.zip");
    if (!is_file(pak) && is_file(join_path(config.game_data, "pak.zip"))) {
        return join_path(config.game_data, "pak.zip");
    }
    return pak;
}

// Which release the mounted archive is: the Xbox 360's carries the console
// interface (`hud_xenon.layout`) itself, a PC one does not and takes it from
// `console_ui` beside it, which is mounted here only then.
void mount_console_interface(Ogre::ResourceGroupManager& groups, const std::string& console_ui) {
    const bool console_pak = groups.resourceExists(kGameGroup, "media/UI/hud_xenon.layout");
    set_console_game_data(console_pak);
    const bool have_ui = !console_ui.empty() && is_dir(console_ui);
    if (console_pak) {
        TL_LOG_INFO("game data: the Xbox 360 pak, with the console interface in it");
        if (have_ui) {
            TL_LOG_INFO("console_ui at '%s' left unmounted: the pak has its own",
                        console_ui.c_str());
        }
    } else if (have_ui) {
        TL_LOG_INFO("game data: a PC pak, the console interface from '%s'", console_ui.c_str());
        groups.addResourceLocation(console_ui, "FileSystem", kGameGroup, true);
    } else {
        TL_LOG_INFO("game data: a PC pak; no --console-ui (without one: no HUD, plain menus)");
    }
}

// Every location mounted, one a line: what read_resource()'s cache is true for.
std::string mount_list(const RendererConfig& config) {
    std::string mounts = config.game_data;
    mounts += '\n' + config.console_ui;
    for (const std::string& mod : config.mod_data) mounts += '\n' + mod;
    for (const std::string& extra : config.extra_data) mounts += '\n' + extra;
    return mounts;
}

// INVENTED: mod folders (`--mod-data`), the port's own way to load an extracted mod.
void mount_mod_data(Ogre::ResourceGroupManager& groups, const std::vector<std::string>& mod_data) {
    for (const std::string& mod : mod_data) {
        if (!is_dir(mod)) {
            TL_LOG_INFO("mod media directory not found at '%s'", mod.c_str());
            continue;
        }
        TL_LOG_INFO("mounting mod folder %s (overrides base files)", mod.c_str());
        groups.addResourceLocation(mod, "FileSystem", kGameGroup, true);
    }
    renderer_detail::set_mod_roots(mod_data);
    set_mod_imagesets(renderer_detail::mod_paths_under("media/ui/", ".imageset"));
}

// OGRE's `Main/`: beside RTShaderLib in OGRE's own tree, inside it in a device's
// layout, whose one shader folder carries both. Empty when it is in neither.
std::string ogre_main_media(const std::string& media, const std::string& rtshader_lib) {
    for (const std::string& dir : {join_path(media, "Main"), join_path(rtshader_lib, "Main")}) {
        if (is_dir(dir)) return dir;
    }
    return {};
}

}  // namespace

bool Renderer::Impl::mount_resources() {
    // The archive is changing: drop the process-wide caches, which outlive the
    // resource managers that Renderer::restart() empties.
    forget_archive_index();
    forget_parsed_materials();
    // The data files read_resource() keeps are the exception: they are bytes,
    // not anything of OGRE's, and stay true for as long as the same archives
    // are mounted - which across a restart they are.
    remember_mounts(mount_list(config));

    auto& groups = Ogre::ResourceGroupManager::getSingleton();

    // OGRE's shader library: RTShaderSystem's GLSL includes. Shipped with the game on the console.
    const std::string media =
        config.ogre_media.empty() ? std::string(TL_OGRE_MEDIA_DIR) : config.ogre_media;
    const std::string rtshader_lib = join_path(media, "RTShaderLib");
    if (media.empty() || !is_dir(rtshader_lib)) {
        TL_LOG_ERROR("RTShaderLib not found at '%s'", rtshader_lib.c_str());
        TL_LOG_ERROR("OGRE's Media/RTShaderLib, with Media/Main beside or inside it, is needed");
        TL_LOG_ERROR("desktop: pass --ogre-media <dir>; console: it is in the .nro's RomFS");
        return false;
    }
    // Not recursive: OGRE's RTShaderLib is flat, and on a device `Main/` sits
    // inside it (below), which must not land in this group as well.
    groups.addResourceLocation(rtshader_lib, "FileSystem", Ogre::RGN_DEFAULT, false);

    // Media/Main (OgreUnifiedShader.h) must be in OgreInternal: the GLSL
    // preprocessor resolves #include against that group only.
    const std::string main_media = ogre_main_media(media, rtshader_lib);
    if (main_media.empty()) {
        TL_LOG_ERROR("OGRE Media/Main not found beside or inside '%s'", rtshader_lib.c_str());
        return false;
    }
    groups.addResourceLocation(main_media, "FileSystem", Ogre::RGN_INTERNAL, true);

    // The game's own data: Pak.zip read directly by OGRE, or an extracted directory.
    if (!is_dir(config.game_data)) {
        TL_LOG_ERROR("game data '%s' is not a readable directory", config.game_data.c_str());
        return false;
    }

    // Mods first: OGRE's first-location-wins resource index lets their files
    // override the archive's.
    mount_mod_data(groups, config.mod_data);
    read_mod_copies = renderer_detail::read_mod_copies;

    const std::string pak = game_pak(config);
    if (!config.pak.empty() && !is_file(pak)) {
        TL_LOG_ERROR("--pak '%s' is not a readable archive", pak.c_str());
        return false;
    }
    if (is_file(pak)) {
        TL_LOG_INFO("mounting %s", pak.c_str());
        groups.addResourceLocation(pak, "Zip", kGameGroup, true);
    } else {
        TL_LOG_INFO("no Pak.zip; mounting %s as loose files", config.game_data.c_str());
        groups.addResourceLocation(config.game_data, "FileSystem", kGameGroup, true);
    }

    mount_console_interface(groups, config.console_ui);

    // Extra data after the install, so the install's own files keep winning
    // (RendererConfig::extra_data).
    for (const std::string& extra : config.extra_data) {
        if (is_file(extra)) {
            TL_LOG_INFO("mounting %s", extra.c_str());
            groups.addResourceLocation(extra, "Zip", kGameGroup, true);
        } else if (is_dir(extra)) {
            TL_LOG_INFO("mounting %s as loose files", extra.c_str());
            groups.addResourceLocation(extra, "FileSystem", kGameGroup, true);
        } else {
            // Noted, not warned: the console has a default entry nobody asked for.
            TL_LOG_INFO("nothing to mount at '%s'", extra.c_str());
        }
    }

    // Only the default group is script-parsed. Parsing the game's material scripts is
    // fatal: duplicate material names across files throw ItemIdentityException.
    // show_mesh() parses the one it needs.
    groups.initialiseResourceGroup(Ogre::RGN_DEFAULT);
    TL_LOG_INFO("%zu duplicate basenames in the archive index (expected)",
                log_bridge ? log_bridge->duplicate_names : 0u);
    // Every mesh's skeleton is looked for beside it, as Runic's listener does.
    Ogre::MeshManager::getSingleton().setListener(&renderer_detail::game_mesh_listener());
    // Read now, under the loading screen, as the original reads every script when it starts: on
    // the first mesh that needed it, it landed in a fight (a monster's first cast).
    renderer_detail::index_material_scripts();
    return true;
}

}  // namespace tl::render
