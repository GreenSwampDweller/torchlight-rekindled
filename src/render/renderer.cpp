// The renderer's implementation: resources, materials, scene, meshes, animation, particles, camera.
// Split from renderer.cpp; the shared
// declarations are in renderer_impl.h.
#include "formats/dds.h"
#include "core/touch_pad.h"
#include "render/menu_chrome.h"
#include "render/renderer_impl.h"
#include "render/skinned_entities.h"

namespace tl::render {

namespace renderer_detail {
/// Parse the script that declares `material` wherever it is in the archive
/// (renderer_archive.cpp).
void parse_material_declaring(const std::string& material);
/// `CMeshListener`, installed on the mesh manager once the archives are mounted.
Ogre::MeshSerializerListener& game_mesh_listener();
}  // namespace renderer_detail
Renderer::Renderer() : impl_(std::make_unique<Impl>()) { impl_->wire_bag_item_events(); }

Renderer::~Renderer() { shutdown(); }

namespace renderer_detail {
namespace {
/// `CMeshListener` (Runic's `Ogre::MeshSerializerListener`), its skeleton half.
struct GameMeshListener final : Ogre::MeshSerializerListener {
    // `processMaterialName` is the material scripts' naming,
    // which the port does where it parses them (`parse_material_script`).
    // PORT: a material the mesh's own directory does not declare is looked for
    // in every script before OGRE gives up on it (parse_material_declaring).
    void processMaterialName(Ogre::Mesh*, Ogre::String* name) override {
        if (name == nullptr || name->empty()) return;
        if (Ogre::MaterialManager::getSingleton().resourceExists(*name, kGameGroup)) return;
        parse_material_declaring(*name);
    }

    // `CMeshListener::processSkeletonName`: the mesh's directory
    // (`FILESYSTEM::RemoveFileName` of its name, cut to start at `MEDIA`) in
    // front of the name the mesh records. So `media/models/NPCs/Brink/
    // Merc.MESH` asks for its own `Merc.SKELETON` and never the one of the same
    // name in `media/models/Mercenary/` - which the archive index, keyed by
    // basename, cannot tell apart. The port's mesh names start at `media/`
    // already; the archive's own spelling of the joined path is used, as
    // `archive_path_in_dir` gives it, so the lookup does not hang on case.
    void processSkeletonName(Ogre::Mesh* mesh, Ogre::String* name) override {
        if (mesh == nullptr || name == nullptr || name->empty()) return;
        const std::string& path = mesh->getName();
        const size_t slash = path.find_last_of("/\\");
        if (slash == std::string::npos) return;
        const std::string dir = path.substr(0, slash + 1);
        const std::string spelled = archive_path_in_dir(*name, dir);
        *name = spelled.empty() ? dir + *name : spelled;
    }

    void processMeshCompleted(Ogre::Mesh*) override {}
};
}  // namespace

Ogre::MeshSerializerListener& game_mesh_listener() {
    static GameMeshListener listener;
    return listener;
}

/// Parse one material script, at most once per run.
///
/// Re-parsing is not merely wasteful: OGRE throws ItemIdentityException on a
/// duplicate material name, so a script parsed twice would raise an error that
/// looks like corrupt data.
/// The names a material script declares, in order.
///
/// Only top-level `material` blocks count, so brace depth has to be tracked - a
/// script's passes and texture units are full of words that would otherwise
/// look like declarations.
std::vector<std::string> declared_materials(const std::string& text) {
    std::vector<std::string> names;
    int depth = 0;
    size_t pos = 0;

    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;

        const size_t comment = line.find("//");
        if (comment != std::string::npos) line.resize(comment);

        size_t first = line.find_first_not_of(" \t\r");
        if (depth == 0 && first != std::string::npos && line.compare(first, 9, "material ") == 0) {
            size_t name_start = line.find_first_not_of(" \t", first + 9);
            if (name_start != std::string::npos) {
                size_t name_end = line.find_first_of(" \t:{\r", name_start);
                if (name_end == std::string::npos) name_end = line.size();
                names.push_back(line.substr(name_start, name_end - name_start));
            }
        }

        for (char c : line) {
            if (c == '{') ++depth;
            else if (c == '}' && depth > 0) --depth;
        }
    }
    return names;
}

/// Rewrite this script's own `material <Name>` declarations to
/// `material <Name>@<dir>`, for exactly the names in `renames`.
///
/// Same line-and-brace walk as declared_materials(), because that is the
/// grammar this has to agree with: a name that function would not have
/// counted as a top-level declaration must not be renamed here either. Only
/// the declaration line changes - Torchlight's own material scripts never
/// reference one material from within another, so nothing else in the file
/// names it.
std::string qualify_material_names(
    const std::string& text, const std::vector<std::pair<std::string, std::string>>& renames) {
    if (renames.empty()) return text;
    std::string out;
    out.reserve(text.size());
    int depth = 0;
    size_t pos = 0;

    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        const bool has_nl = end != std::string::npos;
        if (!has_nl) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = has_nl ? end + 1 : end;

        size_t first = line.find_first_not_of(" \t\r");
        if (depth == 0 && first != std::string::npos && line.compare(first, 9, "material ") == 0) {
            size_t name_start = line.find_first_not_of(" \t", first + 9);
            if (name_start != std::string::npos) {
                size_t name_end = line.find_first_of(" \t:{\r", name_start);
                if (name_end == std::string::npos) name_end = line.size();
                const std::string name = line.substr(name_start, name_end - name_start);
                for (const auto& rename : renames) {
                    if (rename.first == name) {
                        line.replace(name_start, name_end - name_start, rename.second);
                        break;
                    }
                }
            }
        }

        for (char c : line) {
            if (c == '{') ++depth;
            else if (c == '}' && depth > 0) --depth;
        }
        out += line;
        if (has_nl) out += '\n';
    }
    return out;
}

/// Remove this script's own `material <Name> { ... }` blocks entirely, for
/// exactly the names in `drop`.
///
/// For a name that has already been aliased once on this same directory's
/// behalf - a second `collision_*.material` beside the first, both naming
/// the generic `colliders/nocollide` - qualifying it again would declare
/// `colliders/nocollide@<dir>` a second time, which is the identical
/// "material with this name already exists" failure one level down. The
/// first file's alias already speaks for the directory, so a repeat is
/// dropped rather than requalified.
///
/// Same line-and-brace walk as qualify_material_names(), extended to track
/// the whole span of a matching block rather than only its first line.
std::string strip_material_blocks(const std::string& text, const std::vector<std::string>& drop) {
    if (drop.empty()) return text;
    std::string out;
    out.reserve(text.size());
    int depth = 0;
    bool dropping = false;
    size_t pos = 0;

    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        const bool has_nl = end != std::string::npos;
        if (!has_nl) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = has_nl ? end + 1 : end;

        if (!dropping && depth == 0) {
            size_t first = line.find_first_not_of(" \t\r");
            if (first != std::string::npos && line.compare(first, 9, "material ") == 0) {
                size_t name_start = line.find_first_not_of(" \t", first + 9);
                if (name_start != std::string::npos) {
                    size_t name_end = line.find_first_of(" \t:{\r", name_start);
                    if (name_end == std::string::npos) name_end = line.size();
                    const std::string name = line.substr(name_start, name_end - name_start);
                    if (std::find(drop.begin(), drop.end(), name) != drop.end()) dropping = true;
                }
            }
        }

        const bool was_dropping = dropping;
        for (char c : line) {
            if (c == '{') ++depth;
            else if (c == '}' && depth > 0) {
                --depth;
                if (dropping && depth == 0) dropping = false;
            }
        }
        if (was_dropping) continue;  // every line of the dropped block, closer included

        out += line;
        if (has_nl) out += '\n';
    }
    return out;
}

/// Rewrite a material script so no material is declared twice.
///
/// Two different collisions happen, and only one of them is between files:
///
///  * The same name appears in two scripts - `town_enchanter02_shadow` is in
///    several - so a script re-declares something already loaded.
///  * **The same name appears twice in one script.**
///    `menu_billboard_backdrop.material` declares
///    `Material_#238684871/town_enchanter02_shadow` at blocks 2 and 5 of 7.
///
/// Either way OGRE throws ItemIdentityException and abandons the *entire*
/// script, so everything after the collision is lost - which is how the menu's
/// backdrop ended up a grey slab: the collision was on an unrelated material
/// three blocks above `billboard_town_mat`, the one the mesh actually needs.
///
/// Keeping the first declaration and dropping later ones matches what the
/// original engine must do, since these files ship as they are and the game
/// renders them.
// PORT: material scripts, a later declaration of a name dropped (OGRE loading).
std::string deduplicate_material_script(const std::string& text, size_t& dropped) {
    std::string out;
    out.reserve(text.size());

    std::set<std::string> seen;
    int depth = 0;
    bool skipping = false;
    bool entered_block = false;
    size_t pos = 0;
    dropped = 0;

    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;

        std::string code = line;
        const size_t comment = code.find("//");
        if (comment != std::string::npos) code.resize(comment);

        if (depth == 0 && !skipping) {
            const size_t first = code.find_first_not_of(" \t\r");
            if (first != std::string::npos && code.compare(first, 9, "material ") == 0) {
                const size_t name_start = code.find_first_not_of(" \t", first + 9);
                if (name_start != std::string::npos) {
                    size_t name_end = code.find_first_of(" \t:{\r", name_start);
                    if (name_end == std::string::npos) name_end = code.size();
                    const std::string name = code.substr(name_start, name_end - name_start);
                    if (!seen.insert(name).second) {
                        skipping = true;
                        entered_block = false;
                        ++dropped;
                    }
                }
            }
        }

        if (!skipping) out += line + "\n";

        for (char c : code) {
            if (c == '{') ++depth;
            else if (c == '}' && depth > 0) --depth;
        }

        if (skipping) {
            if (depth > 0) entered_block = true;
            else if (entered_block) skipping = false;
        }
    }
    return out;
}

/// Supply the sources OGRE 1.6 defaulted for a bare `colour_op_ex`.
///
/// Two shipped scripts - `media/levelSets/crypt/crypt_water_01.material` and
/// `media/levelSets/palace/palace_water.material` - write
///
///     colour_op_ex modulate_x4
///
/// with no source arguments. Torchlight's OGRE accepted it; OGRE 14's script
/// compiler does not, and rejects the line with "colour_op_ex must have at
/// least 3 arguments", leaving that texture layer blended as though the
/// directive were absent.
///
/// The values to fill in are not a guess: `TextureUnitState::setColourOperationEx`
/// declares `source1 = LBS_TEXTURE, source2 = LBS_CURRENT` as its defaults, and
/// the script form is a thin wrapper over that call - so a script omitting them
/// meant exactly `src_texture src_current`.
///
/// Done here rather than as a patch to `external/ogre` deliberately: this is not
/// a Switch workaround, it is our reader being compatible with the data, and the
/// project's rule is that OGRE is never edited in place.
std::string complete_colour_op_ex(const std::string& text, size_t& fixed) {
    fixed = 0;
    std::string out;
    out.reserve(text.size());

    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        const bool last = (end == std::string::npos);
        if (last) end = text.size();
        std::string line = text.substr(start, end - start);

        // Tokenise, ignoring a trailing carriage return.
        std::string body = line;
        while (!body.empty() && (body.back() == '\r')) body.pop_back();

        size_t first = body.find_first_not_of(" \t");
        if (first != std::string::npos) {
            const std::string trimmed = body.substr(first);
            if (trimmed.compare(0, 13, "colour_op_ex ") == 0 ||
                trimmed.compare(0, 13, "color_op_ex  ") == 0) {
                size_t tokens = 0;
                size_t at = 0;
                while (at < trimmed.size()) {
                    while (at < trimmed.size() && (trimmed[at] == ' ' || trimmed[at] == '\t')) ++at;
                    if (at >= trimmed.size()) break;
                    ++tokens;
                    while (at < trimmed.size() && trimmed[at] != ' ' && trimmed[at] != '\t') ++at;
                }
                if (tokens == 2) {  // the directive plus its operation, and nothing else
                    line = body + " src_texture src_current";
                    ++fixed;
                }
            }
        }

        out += line;
        if (!last) out += '\n';
        if (last) break;
        start = end + 1;
    }
    return out;
}

/// Material scripts already parsed into the current OGRE session.
///
/// File scope rather than a function-local static so that it can be forgotten
/// from `mount_resources()`; see the note there.
std::set<std::string> g_parsed_materials;

/// How many times each script has been parsed, so a storm is visible.
///
/// **Material names collide between scripts, and re-parsing on that basis does
/// not work.** The obvious repair for a name that has changed hands is to
/// re-parse whichever script wants it back - but scripts in one directory
/// declare overlapping names, `parse_sibling_material()` parses a whole
/// directory when a mesh has no script of its own, and each parse invalidates
/// the others. Making the cache a freshness check turned the town's 349 parses
/// into **10,637**, every one of them redefining, and the console never
/// finished loading the level. The desktop did, which is exactly why the log
/// grew from 600 lines to 13,000 and the run still exited zero.
///
/// So the cache stays a gate, and a caller that genuinely needs a script's own
/// definitions back asks for them - see the `force` argument, which only the
/// character-creation swap uses.
std::map<std::string, size_t> g_parse_counts;

/// Which directory currently owns a plain (unqualified) material name.
///
/// Most redefinitions are the same asset coming back - a level transition
/// reusing the menu's own names, or the character-creation swap asking for
/// its own wardrobe piece back - and those are meant to clobber: the newer
/// script IS the current truth for that name. But two *unrelated* meshes can
/// land on the same generic exporter name by accident - `media/models/NPCs/
/// Mage/Mage.material` and `.../Mage2/Mage.material` both declare
/// `Standard_20`, one green and one red, because that is what the export tool
/// calls an untitled material. Clobbering there does not update stale state,
/// it destroys information: whichever NPC is placed - or whose script parses
/// - second silently repaints the first. See qualify_cross_directory_names().
std::map<std::string, std::string> g_material_home;

/// A material name a cross-directory collision could not be allowed to
/// clobber, so it was parsed under a qualified alias instead. Keyed by
/// `<placing directory>\x1f<name as the .MESH file embeds it>`, valued by the
/// alias actually registered with OGRE's MaterialManager. Consulted once per
/// placed entity, in place_mesh().
std::map<std::string, std::string> g_material_alias;

/// Which script currently owns a plain material name, so that two scripts of
/// one directory can be put in the original's order. See the note on
/// `material_script_rank()`.
std::map<std::string, std::string> g_material_script;

void forget_parsed_materials() {
    g_parsed_materials.clear();
    g_parse_counts.clear();
    g_material_home.clear();
    g_material_alias.clear();
    g_material_script.clear();
}

/// The scene a material fix has to reach.
///
/// File scope for the same reason `g_parsed_materials` is: parsing is done by
/// free functions on purpose, and a redefinition there invalidates entities
/// anywhere in the scene, not only the ones the caller happens to hold.
Ogre::SceneManager* g_material_scene = nullptr;

/// `force` re-parses a script that is already in the cache, so that its own
/// definitions take back the names it declares.
///
/// Only the character-creation swap passes it. It is not a general repair: see
/// the note on g_parse_counts for what happens when re-parsing is driven by
/// name ownership instead of by an explicit caller.
void parse_material_script(const std::string& script, std::vector<std::string>* redefined,
                           bool force) {
    if (!g_parsed_materials.insert(script).second && !force) return;
    const size_t parses = ++g_parse_counts[script];
    if (parses == kNoisyParseCount) {
        TL_LOG_WARN(
            "material script '%s' has been parsed %zu times; something is thrashing the "
            "cache - see g_parse_counts",
            script.c_str(), parses);
    }

    auto& groups = Ogre::ResourceGroupManager::getSingleton();
    auto& materials = Ogre::MaterialManager::getSingleton();
    try {
        Ogre::DataStreamPtr stream = groups.openResource(script, kGameGroup);
        const std::string raw = stream->getAsString();

        size_t dropped = 0;
        std::string text = deduplicate_material_script(raw, dropped);

        size_t completed = 0;
        text = complete_colour_op_ex(text, completed);
        if (completed > 0) {
            TL_LOG_INFO("  %zu bare colour_op_ex completed with OGRE's own defaults in %s",
                        completed, script.c_str());
        }

        size_t filled = 0;
        text = fill_empty_texture_units(text, script, filled);
        if (filled > 0) {
            TL_LOG_INFO("  %zu empty texture_unit(s) in %s filled from the set's own artwork",
                        filled, script.c_str());
        }

        // Now the between-file collisions. Removing is safe: an entity already
        // built holds a shared_ptr to the material it was created with, and the
        // new definition serves everything created afterwards.
        //
        // **The shader generator has to be told, and this is the only place
        // that knows.** The RTShaderSystem keeps a table of the shader
        // techniques it has built, keyed by material name and group, and
        // `createShaderBasedTechnique()` refuses to build a second one for a
        // name it already has. Removing the material here and re-parsing makes
        // a *new* `Material` with no shader technique on it, while that table
        // still holds an entry - pointing at a technique that no longer exists.
        // The next time anything draws with it,
        // `ShaderResolver::handleSchemeNotFound` cannot make a technique, the
        // fixed-function one is used instead, and on a render system that has
        // no fixed-function pipeline - both of this port's - that is fatal:
        //
        //     RenderSystem does not support FixedFunction, but technique of
        //     'mine_lantern01' has no Vertex Shader. Use the RTSS
        //
        // The town's material scripts redefine 23 names the main menu
        // backdrop's already declared, so it is the level transition that
        // exposes this; but the same removal happens *within* a single level
        // load - the town redefines 20 of its own - so this was latent all
        // along and only survived because nothing had yet drawn with the
        // stale entry.
        //
        // Through the singleton rather than a member: this is a free function
        // deliberately, and the generator is a process-wide singleton anyway.
        size_t replaced = 0;
        auto* shaders = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
        const std::string script_dir = dir_of_asset(script);
        std::vector<std::pair<std::string, std::string>> aliased;
        std::vector<std::string> dropped_duplicate_alias;
        std::vector<std::string> kept_by_earlier;
        for (const std::string& name : declared_materials(text)) {
            if (materials.resourceExists(name, kGameGroup)) {
                const auto home = g_material_home.find(name);
                if (home != g_material_home.end() && home->second != script_dir) {
                    // A different, unrelated directory already owns this
                    // name - two NPC skins sharing an exporter's default
                    // "Standard_20", not the same asset coming back. Removing
                    // the existing one would make it as dead as the RTSS
                    // problem this whole function exists to avoid, just for
                    // colour instead of a crash: the earlier NPC would follow
                    // whichever redefines the name last. Parsed under an
                    // alias instead, and place_mesh() re-points only the
                    // entities actually built from this directory at it.
                    //
                    // **A second file in this same directory can hit the very
                    // same collision.** `media/levelSets/props/lava_props/`
                    // alone has a dozen `collision_*.material` scripts that
                    // all declare the generic `colliders/nocollide`, and every
                    // one of them collides with whatever directory got that
                    // name first. The first file already aliased it on this
                    // directory's behalf; declaring the alias a second time is
                    // the identical "already exists" failure one name over, so
                    // a repeat is dropped instead of requalified.
                    const std::string alias_key = script_dir + "\x1f" + name;
                    if (g_material_alias.count(alias_key) != 0) {
                        dropped_duplicate_alias.push_back(name);
                    } else {
                        const std::string qualified = name + "@" + script_dir;
                        aliased.emplace_back(name, qualified);
                        g_material_alias[alias_key] = qualified;
                    }
                    continue;
                }
                // **Two scripts of one directory: the one earlier in the
                // archive keeps the name.** Runic's OGRE parses a directory's
                // scripts together, in archive order, the first time anything
                // in it is looked up (`ResourceGroupManager::
                // addScriptForParsingLater` keys them by directory,
                // `parseCachedScripts` runs them from
                // `ResourceManager::getByName`, libOgreMain-1.6.5),
                // and names them `<DIRECTORY>/<name>`
                // (`CScriptListener::createObject`, `CMeshListener::processMaterialName`).
                // A later script's `create` of a taken name throws,
                // and `MaterialTranslator::translate` catches it,
                // logs "failed to find or create material" and skips just that
                // material. So `menu_billboard_sky.material`'s scrolling
                // `billboard_wind*_mat` survive `menu_billboard_terrain2
                // .material`'s still copies. The port parses per mesh, in
                // placement order, so the order is restored here by rank.
                // `force` (the character-creation swap) still takes names back.
                const auto owner = g_material_script.find(name);
                if (!force && owner != g_material_script.end() && owner->second != script &&
                    material_script_rank(owner->second) < material_script_rank(script)) {
                    kept_by_earlier.push_back(name);
                    continue;
                }
                if (redefined != nullptr) redefined->push_back(name);
                if (shaders != nullptr) {
                    shaders->removeAllShaderBasedTechniques(name, kGameGroup);
                }
                materials.remove(name, kGameGroup);
                ++replaced;
            }
            g_material_home[name] = script_dir;
            g_material_script[name] = script;
        }
        if (!aliased.empty()) {
            text = qualify_material_names(text, aliased);
            TL_LOG_INFO(
                "  %zu material name(s) in %s collide with another directory's; "
                "parsed under an alias instead of replacing them",
                aliased.size(), script.c_str());
        }
        if (!dropped_duplicate_alias.empty()) {
            text = strip_material_blocks(text, dropped_duplicate_alias);
        }
        if (!kept_by_earlier.empty()) {
            text = strip_material_blocks(text, kept_by_earlier);
            TL_LOG_INFO(
                "  %zu material name(s) in %s are an earlier script's in the archive; "
                "kept theirs",
                kept_by_earlier.size(), script.c_str());
        }

        // This script now owns every name it declares, and whoever owned them
        // before does not - which is what makes them re-parse if they are asked
        // for again.
        // **Everything already built with one of those names is now holding a
        // dead object**, and the RTShaderSystem will not serve it - so this has
        // to reach the whole scene, not just whatever the caller is assembling.
        // The town's own scripts redefine 23 names the menu backdrop already
        // declared and 20 of each other's, so this is the ordinary case rather
        // than the exceptional one.
        //
        // **Must run after parseScript(), not before.** `materials.remove()`
        // above only vacates the name; rebind_scene_materials() re-resolves
        // every stale submesh by looking the name up again with
        // `getByName()`, and between the removal and the parse below that
        // lookup finds nothing to point at - `rebind_materials()` treats a
        // null `current` as "nothing to do" and silently leaves the entity on
        // its now-orphaned `Material`. A name a floor's props redefine only
        // once survives on luck: some *other*, later redefinition's own sweep
        // eventually catches the strays as a side effect. A name that is
        // never redefined again after its own last rewrite - `mine_supports`,
        // rewritten six times running by six different mine-prop scripts on
        // a three-chunk mines floor, with nothing after it to trigger one
        // more sweep - never gets rescued: the stray entities keep pointing
        // at a `Material` object the manager no longer owns, RTShaderSystem
        // still has recorded as *someone else's* technique, and the first
        // frame that actually draws one throws
        // `RenderSystem does not support FixedFunction, but technique of
        // 'mine_supports' has no Vertex Shader` - uncaught, since the live
        // game loop draws every frame outside any try/catch.
        Ogre::DataStreamPtr copy(
            new Ogre::MemoryDataStream(const_cast<char*>(text.data()), text.size(), false, true));
        materials.parseScript(copy, kGameGroup);

        // Every material this parse just declared gets its textures qualified
        // right here, not only the one name whichever caller happens to ask
        // about afterwards by its own mesh's submaterial name.
        //
        // **Without this, a material reached only as a side effect of another
        // mesh's directory-wide scan sits with its bare, unqualified
        // `texture_unit` names indefinitely** - parse_sibling_material()'s
        // fallback parses every script in a mesh's directory looking for the
        // *one* material that mesh's own submesh actually names, and nothing
        // has ever qualified the rest. Usually harmless, because most of
        // those other materials belong to meshes that get placed later and
        // are qualified then, under their own name. Not harmless when a
        // mesh's own submaterial name does not literally match its script's
        // filename - `Mine_railing_large.material` can declare a material
        // that some *other* prop's submesh uses by name, while
        // `mine_railing01.material`'s own material is never asked for by
        // that exact name at all - which is why this looked seed-dependent:
        // whether the file holding a given texture reference got a qualifying
        // caller at all depended on which room pieces the generator happened
        // to pick, not on anything about the reference itself.
        for (const std::string& name : declared_materials(text)) {
            qualify_texture_paths(materials.getByName(name, kGameGroup), script_dir, script_dir);
        }

        // The re-pointing, after the names above: a material handed to a
        // submesh is loaded there and then, and one still carrying its bare
        // `texture miscellaneous.png` logged "Texture layer will be blank" for
        // every unit before the loop above put the full path in.
        if (replaced > 0 && g_material_scene != nullptr) {
            const size_t rebound = rebind_scene_materials(g_material_scene);
            if (rebound > 0) {
                TL_LOG_INFO("  %zu submesh(es) re-pointed after %s redefined %zu name(s)", rebound,
                            script.c_str(), replaced);
            }
        }

        if (dropped > 0 || replaced > 0) {
            TL_LOG_INFO("parsed material script %s (%zu duplicate(s) dropped, %zu redefined)",
                        script.c_str(), dropped, replaced);
        } else {
            TL_LOG_INFO("parsed material script %s", script.c_str());
        }
    } catch (const Ogre::Exception& e) {
        TL_LOG_WARN("could not parse '%s': %s", script.c_str(), e.getDescription().c_str());
    }
}

/// The one texture in `dir` that belongs to this set and this slot, or empty.
/// See fill_empty_texture_units() for where the rule comes from.
std::vector<std::string> underscore_words(const std::string& name) {
    std::vector<std::string> words;
    std::string word;
    for (const char c : name) {
        if (c == '_') {
            words.push_back(word);
            word.clear();
        } else {
            word.push_back(c);
        }
    }
    words.push_back(word);
    return words;
}

std::string wardrobe_texture_for(const std::string& mesh_name, const std::string& dir,
                                 const std::string& slot, const std::string& set_word) {
    (void)mesh_name;
    if (set_word.empty() || dir.empty() || slot.empty()) return {};

    // The slot as a whole trailing word, with the other number allowed:
    // `shoulders` is stored as `..._Shoulder`.
    std::vector<std::string> endings{slot};
    if (!slot.empty() && slot.back() == 's') {
        endings.push_back(slot.substr(0, slot.size() - 1));
    } else {
        endings.push_back(slot + "s");
    }

    // **`Body` is the chest slot's other authored name, in exactly one set.**
    //
    // This is a reading of the item data, which is where the original gets a
    // worn set's texture from in the first place - see the note above
    // fill_empty_texture_units(). Every armour item carries one `[WARDROBE]`
    // group per class naming a `MESH` and a `TEXTURE`, and across all 3,081
    // item files those name 34 wardrobe meshes and 102 textures: three per set,
    // one each for boots, chest and gloves, spelled with the slot's own word.
    // The single exception is `vanquisher_assassin_set`, whose chest items say
    //
    //     [WARDROBE]
    //       CLASS:VANQUISHER
    //       MESH:media/wardrobe/vanquisher/vanquisher_assassin_set.mesh
    //       TEXTURE:media/wardrobe/vanquisher/vanq_assassin_body.png
    //
    // and there is no `Vanq_Assassin_Chest.png` in the archive at all. Without
    // this the assassin set drew a flat yellow torso.
    //
    // Only for `chest`, and only as a *fallback* after the slot's own word
    // finds nothing: `body.png` on its own is the character's bare skin, and
    // letting boots or gloves fall through to it would dress them in it.
    std::vector<std::vector<std::string>> rounds{endings};
    if (slot == "chest") rounds.push_back({"body"});

    // Whole words, not substrings. `plate` is a substring of `heavyplate`, and
    // matching loosely made the Destroyer's plate boots ambiguous against his
    // heavy-plate boots - two candidates, so the scan gave up and the boots
    // stayed flat colour.
    for (const std::vector<std::string>& round : rounds) {
        std::vector<std::string> found;
        for (const char* extension : {".png", ".dds"}) {
            for (const std::string& path : archive_paths_under(dir, extension)) {
                std::string name = lowered(path.substr(path.find_last_of('/') + 1));
                const size_t dot_at = name.find_last_of('.');
                if (dot_at != std::string::npos) name.resize(dot_at);
                const std::vector<std::string> words = underscore_words(name);
                if (std::find(words.begin(), words.end(), set_word) == words.end()) continue;
                if (std::find(round.begin(), round.end(), words.back()) == round.end()) continue;
                found.push_back(path);
            }
        }
        if (found.size() == 1) return found.front();
    }
    return {};
}

/// Point a freshly built entity at the *current* material of each of its names.
///
/// **An `Ogre::Mesh` is a cached resource and its submeshes cache a
/// `MaterialPtr`, not just a name.** `SubMesh::mMaterial` is resolved when the
/// mesh is first loaded, and `Entity::_initialise()` copies it straight into
/// each `SubEntity` (`OgreEntity.cpp`, "if (subMesh->getMaterial())"). So an
/// entity built from a mesh that was loaded earlier inherits whatever
/// `Material` object was current *then* - and if some script has redeclared
/// that name since, the object is one the manager has already dropped.
///
/// That is fatal rather than untidy, because the RTShaderSystem keys its
/// generated techniques by material *name* and can hold one per name: the
/// dropped object gets no shader technique, falls back to fixed-function, and
/// neither of this port's render systems has one. It is what killed the
/// character-creation screen on the fourth swap - Destroyer, Vanquisher,
/// Alchemist, Vanquisher - where the Vanquisher's hair was rebuilt from a
/// cached mesh whose five `Material_#1/<slot>` pointers dated from the swap
/// before the Alchemist's script replaced them all.
///
/// Cheap, and it must run on *every* new entity: this is a property of mesh
/// caching, not of the wardrobe. repoint_materials() beside it is the other
/// half - it fixes entities that already existed when a name was redefined.
size_t rebind_materials(Ogre::Entity* entity) {
    if (entity == nullptr) return 0;
    auto& materials = Ogre::MaterialManager::getSingleton();
    size_t rebound = 0;
    for (unsigned i = 0; i < entity->getNumSubEntities(); ++i) {
        Ogre::SubEntity* sub = entity->getSubEntity(i);
        const Ogre::MaterialPtr held = sub->getMaterial();
        if (!held) continue;
        const Ogre::MaterialPtr current = materials.getByName(held->getName(), held->getGroup());
        if (current && current.get() != held.get()) {
            sub->setMaterial(current);
            ++rebound;
        }
    }
    unlight_full_emissive(entity);
    return rebound;
}

// `CGenericModel::reInitialize`, for
// each record: technique 0, pass 0 of its material - when that pass has a
// texture unit and its emissive red, blue and green are each exactly 1 - has
// its lighting turned off (`Pass +0xcc` = 0). The material is the shared one,
// so every user of it draws unlit from then on. The console's captured shaders
// for such passes (the crypt's `candle_additive_nolight` and `smoke`, the town
// menu's `billboard_town_mat`) do no lighting: a constant colour of 1 where the
// port, lit, reached 1.1-1.7.
void unlight_full_emissive(Ogre::Entity* entity) {
    if (entity == nullptr) return;
    for (unsigned i = 0; i < entity->getNumSubEntities(); ++i) {
        const Ogre::MaterialPtr& held = entity->getSubEntity(i)->getMaterial();
        if (!held) continue;
        Ogre::Technique* technique = nullptr;
        for (Ogre::Technique* t : held->getTechniques()) {
            if (t->getSchemeName() == Ogre::MSN_DEFAULT) {
                technique = t;
                break;
            }
        }
        if (technique == nullptr || technique->getNumPasses() == 0) continue;
        Ogre::Pass* pass = technique->getPass(0);
        const Ogre::ColourValue& emissive = pass->getSelfIllumination();
        if (pass->getNumTextureUnitStates() == 0 || !pass->getLightingEnabled() ||
            !(emissive.r == 1.0f && emissive.b == 1.0f && emissive.g == 1.0f)) {
            continue;
        }
        pass->setLightingEnabled(false);
        if (auto* shaders = Ogre::RTShader::ShaderGenerator::getSingletonPtr()) {
            shaders->removeAllShaderBasedTechniques(held->getName(), held->getGroup());
        }
    }
}

/// Give a pass the original's rim-light texture unit, or return the one it
/// already has (`CGenericModel::setRimLighting`).
///
/// Appended last, which is where `setHighlightTexture`
/// looks for it: it takes
/// `getTextureUnitState(count - 1)` and swaps its cube map. That is the whole
/// mechanism - the unit is always present, and *which cube map it samples* is
/// the difference between a character standing there and the one the player has
/// selected.
/// The name `setRimLighting` gives a pass it has lit.
const char* const kRimLitPass = "RIMLIT";

Ogre::TextureUnitState* rim_unit(Ogre::Pass* pass, const std::string& texture) {
    if (pass == nullptr) return nullptr;
    Ogre::TextureUnitState* unit = nullptr;
    if (pass->getNumTextureUnitStates() > 1) {
        unit = pass->getTextureUnitState(pass->getNumTextureUnitStates() - 1);
        if (unit->getTextureName() == texture) return unit;
    }
    if (unit == nullptr || unit->getTextureType() != Ogre::TEX_TYPE_CUBE_MAP) {
        unit = pass->createTextureUnitState();
    }
    unit->setTextureName(texture, Ogre::TEX_TYPE_CUBE_MAP);
    unit->setTextureCoordSet(1);
    unit->setTextureAddressingMode(Ogre::TextureUnitState::TAM_CLAMP);
    unit->setEnvironmentMap(true, Ogre::TextureUnitState::ENV_NORMAL);
    unit->setColourOperation(Ogre::LBO_ADD);
    return unit;
}

/// Give every material of a placed character the level's rim light.
///
/// `CLevel::addCharacter` does this to each
/// character it takes in, with the level rules' `RIMLIGHT` - so in the original
/// *every* unit carries this sheen and the selected one differs only by which
/// cube map the unit samples. Without it the highlight reads as a much bigger
/// jump than it does on the console, which is what was seen on the first
/// build.
namespace {
/// The bounds each `RENDERALWAYS` mesh shipped with, by mesh name.
std::unordered_map<std::string, Ogre::AxisAlignedBox>& authored_bounds_by_mesh() {
    static std::unordered_map<std::string, Ogre::AxisAlignedBox> bounds;
    return bounds;
}
}  // namespace

// `CCharacter::unitInit`: a `RENDERALWAYS`
// unit's mesh is given bounds of +-100000 (`Mesh::_setBounds`), so it is drawn
// wherever its animation carries it - the Ember Colossus climbs down its tree
// in `spawn` and Krag down his cliff for 17 s, far from their nodes at the
// foot. The box that followed the skeleton instead (until 2026-10-11) is only
// as fresh as the last frame the unit was drawn, and on the console Krag came
// and went all the way down. The mesh's own bounds are kept for the two readers
// that measure the model (`authored_mesh_bounds`).
void apply_render_always(Ogre::Entity* entity, bool render_always) {
    if (entity == nullptr || !render_always) return;
    const Ogre::MeshPtr& mesh = entity->getMesh();
    if (!mesh) return;
    if (authored_bounds_by_mesh().emplace(mesh->getName(), mesh->getBounds()).second) {
        mesh->_setBounds(Ogre::AxisAlignedBox(-100000.0f, -100000.0f, -100000.0f, 100000.0f,
                                              100000.0f, 100000.0f),
                         false);
    }
}

const Ogre::AxisAlignedBox& authored_mesh_bounds(const Ogre::Entity* entity) {
    const Ogre::MeshPtr& mesh = entity->getMesh();
    const auto kept = authored_bounds_by_mesh().find(mesh->getName());
    return kept != authored_bounds_by_mesh().end() ? kept->second : mesh->getBounds();
}

// `CGenericModel::setRimLighting`: a pass
// takes the rim light once. Its first pass, when it has a texture unit and is
// not yet named `RIMLIT`, is renamed `RIMLIT` and given the unit; a pass
// already `RIMLIT` keeps the cube map it has. The materials are shared by
// name, so a material keeps the rim light of the first level that lit it for
// as long as the game runs: the player and the pet, lit by the town's
// `rimlighttown.dds`, wear it down the mine too (the console's Mines 1 frame),
// and a monster first met in the mine gets the default `rimlight.dds`.
//
// The port builds a level's character materials again on a warp (the
// wardrobe's, the script reloads), which drops the `RIMLIT` mark the console's
// materials keep, so the cube map a material was first lit with is kept here
// by its name for as long as the game runs, and given back to its rebuilt
// passes.
void apply_rim_light(Ogre::Entity* entity, const std::string& level_texture) {
    if (entity == nullptr || level_texture.empty()) return;
    static std::unordered_map<std::string, std::string> first_lit;
    bool added = false;
    for (unsigned i = 0; i < entity->getNumSubEntities(); ++i) {
        const Ogre::MaterialPtr held = entity->getSubEntity(i)->getMaterial();
        if (!held) continue;
        for (unsigned t = 0; t < held->getNumTechniques(); ++t) {
            Ogre::Technique* technique = held->getTechnique(t);
            if (technique->getSchemeName() != Ogre::MSN_DEFAULT) continue;
            if (technique->getNumPasses() == 0) continue;
            Ogre::Pass* pass = technique->getPass(0);
            if (pass->getNumTextureUnitStates() == 0 || pass->getName() == kRimLitPass) continue;
            const std::string& texture =
                first_lit.emplace(held->getName(), level_texture).first->second;
            pass->setName(kRimLitPass);
            rim_unit(pass, texture);
            added = true;
        }
    }
    if (!added) return;
    if (auto* shaders = Ogre::RTShader::ShaderGenerator::getSingletonPtr()) {
        for (unsigned i = 0; i < entity->getNumSubEntities(); ++i) {
            const Ogre::MaterialPtr held = entity->getSubEntity(i)->getMaterial();
            if (held) shaders->removeAllShaderBasedTechniques(held->getName(), held->getGroup());
        }
    }
}

/// Light up the unit the console is pointing at, the way the original does.
///
/// `CGenericModel::setHighlighted` flags every sub-part of the model
/// and the draw picks that up as a **cube-map glow added over the skin**: the
/// engine gives the pass a texture unit whose cubic texture is
/// `media/sharedtextures/highlight.dds`, on coordinate set 1, clamped,
/// `setEnvironmentMap(true, ENV_NORMAL)` and `setColourOperation(LBO_ADD)`
/// (read off `CGenericModel::setRimLighting`, which
/// builds the same unit for a level's ambient rim light). Which of the two cube
/// maps is used is `CCharacter::setAlignment`'s
/// decision: **`highlightred.dds` for a unit
/// the player can fight, `highlight.dds` for everybody else**.
///
/// The mechanism is the port's own - OGRE 14 and a per-entity material clone,
/// rather than the original's own model class - because rendering is this
/// engine's side of the line. The *appearance* is the original's, down to the
/// texture it lights up with.
///
/// Not done here: `setRenderBehindTexture`
/// draws the same unit's silhouette through walls with `outlinered.dds` /
/// `outlineblue.dds`.
void highlight_entity(Ogre::Entity* entity, bool on, bool hostile, const std::string& rim_texture) {
    if (entity == nullptr) return;
    auto& materials = Ogre::MaterialManager::getSingleton();
    const std::string cube = on ? (hostile ? "media/sharedtextures/highlightred.dds"
                                           : "media/sharedtextures/highlight.dds")
                                : rim_texture;
    const std::string suffix = hostile ? "/tl_highlight_red" : "/tl_highlight";

    for (unsigned i = 0; i < entity->getNumSubEntities(); ++i) {
        Ogre::SubEntity* sub = entity->getSubEntity(i);
        const Ogre::MaterialPtr held = sub->getMaterial();
        if (!held) continue;
        const std::string& name = held->getName();
        const bool lit = name.size() > suffix.size() &&
                         name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
        if (on == lit) continue;

        if (!on) {
            // Back to the material the clone was made from, by name: the
            // wardrobe and the alias table may both have had a say in which
            // one that is, and neither wants re-deriving here.
            const Ogre::MaterialPtr plain =
                materials.getByName(name.substr(0, name.size() - suffix.size()), held->getGroup());
            if (plain) sub->setMaterial(plain);
            continue;
        }

        // A clone per material, not per entity: two Ratlins share a skin and
        // would otherwise light up together - and a duplicate material name
        // under RTSS is its own trap.
        const std::string clone_name = name + suffix;
        Ogre::MaterialPtr clone = materials.getByName(clone_name, held->getGroup());
        if (!clone) {
            clone = held->clone(clone_name);

            // Drop the techniques RTSS generated for the material this was
            // cloned from. They carry compiled shaders that sample the units
            // they were built with, so a unit added to one of them is simply
            // not read - which is what "the glow is applied and nothing
            // appears" looks like. Left with the fixed-function source
            // technique, ShaderResolver regenerates on the first draw and the
            // new unit is in the shader it writes.
            for (unsigned t = clone->getNumTechniques(); t-- > 0;) {
                if (clone->getTechnique(t)->getSchemeName() != Ogre::MSN_DEFAULT) {
                    clone->removeTechnique(t);
                }
            }
            for (unsigned t = 0; t < clone->getNumTechniques(); ++t) {
                Ogre::Technique* technique = clone->getTechnique(t);
                if (technique->getNumPasses() == 0) continue;
                rim_unit(technique->getPass(0), cube);
            }
            if (auto* shaders = Ogre::RTShader::ShaderGenerator::getSingletonPtr()) {
                shaders->removeAllShaderBasedTechniques(clone_name, clone->getGroup());
            }
        }
        sub->setMaterial(clone);
    }
}

/// Point a just-placed entity's submeshes at their directory-qualified alias,
/// for any material name that collided across two unrelated directories - see
/// `g_material_alias` and the note in parse_material_script(). A no-op for
/// every entity but the ones built from a directory that actually lost a
/// naming collision, which in the whole archive is the Mage/Mage2 NPC skins
/// and whatever else shares an exporter's default name with something else
/// placed in the same level.
///
/// Must run *after* rebind_materials(): that call re-resolves each submesh's
/// held pointer by the plain name, which for the losing directory's entity is
/// exactly the wrong material to settle on.
void apply_qualified_materials(Ogre::Entity* entity, const std::string& dir) {
    if (entity == nullptr || g_material_alias.empty()) return;
    auto& materials = Ogre::MaterialManager::getSingleton();
    for (unsigned i = 0; i < entity->getNumSubEntities(); ++i) {
        Ogre::SubEntity* sub = entity->getSubEntity(i);
        const Ogre::SubMesh* submesh = sub->getSubMesh();
        if (submesh == nullptr) continue;
        const auto it = g_material_alias.find(dir + "\x1f" + submesh->getMaterialName());
        if (it == g_material_alias.end()) continue;
        const Ogre::MaterialPtr aliased = materials.getByName(it->second, kGameGroup);
        if (aliased) sub->setMaterial(aliased);
    }
}

/// Report any submesh still holding a `Material` the manager has replaced.
///
/// **Two live `Material` objects with one name is this port's most expensive
/// failure**, because the RTShaderSystem keys its techniques by name and can
/// only serve one of them; the other draws fixed-function and kills the frame.
/// It is also invisible until something draws, which on the character-creation
/// screen meant a crash three swaps after the mistake. This walks every entity
/// in the scene and says who is holding what, which turns that into one line.
size_t rebind_scene_materials(Ogre::SceneManager* scene) {
    if (scene == nullptr) return 0;
    size_t rebound = 0;
    for (const auto& entry : scene->getMovableObjects(Ogre::MOT_ENTITY)) {
        rebound += rebind_materials(static_cast<Ogre::Entity*>(entry.second));
        // A floor's thousands of room pieces, once per redefining script.
        tl::core::keep_responsive();
    }
    return rebound;
}

/// Say so if anything is still holding a `Material` the manager has replaced.
///
/// Nothing should be, now that rebind_materials() runs on every new entity and
/// rebind_scene_materials() after every redefining parse. This is the check
/// that says so out loud, because the failure is otherwise invisible until
/// something draws - on the character-creation screen that was a crash three
/// swaps after the mistake was made.
size_t report_stale_materials(Ogre::SceneManager* scene, const char* when) {
    if (scene == nullptr) return 0;
    auto& materials = Ogre::MaterialManager::getSingleton();
    size_t stale = 0;
    for (const auto& entry : scene->getMovableObjects(Ogre::MOT_ENTITY)) {
        auto* entity = static_cast<Ogre::Entity*>(entry.second);
        if (entity == nullptr) continue;
        for (unsigned i = 0; i < entity->getNumSubEntities(); ++i) {
            Ogre::SubEntity* sub = entity->getSubEntity(i);
            const Ogre::MaterialPtr held = sub->getMaterial();
            if (!held) continue;
            const Ogre::MaterialPtr current =
                materials.getByName(held->getName(), held->getGroup());
            if (current && current.get() != held.get()) {
                TL_LOG_WARN("  stale material after %s: '%s' on %s holds %p, the manager has %p",
                            when, held->getName().c_str(),
                            entity->getMesh() ? entity->getMesh()->getName().c_str() : "?",
                            static_cast<const void*>(held.get()),
                            static_cast<const void*>(current.get()));
                ++stale;
            }
        }
    }
    return stale;
}

/// Point everything already built at the *new* definition of a material.
///
/// **A redefined material leaves two live `Material` objects with the same
/// name.** `parse_material_script()` removes the old one and lets the script
/// create a new one, and an entity built earlier keeps a `MaterialPtr` to the
/// old object - which is fine for the geometry and fatal for the
/// RTShaderSystem, because the generator keys its techniques by material *name*
/// and can therefore serve only one of the two. The other draws with its
/// fixed-function technique, and neither of this port's render systems has one:
///
///     RenderSystem does not support FixedFunction, but technique of
///     'Material_#1/helmet' has no Vertex Shader
///
/// That is what stopped the Vanquisher rendering at all. Her body's mesh and
/// every one of her wardrobe pieces name their materials `Material_#1/<slot>` -
/// a default the exporter left in, shared across 12 scripts - so equipping
/// anything redeclares the names the body is already drawn with.
///
/// Re-pointing by name is idempotent and cheap: `SubEntity::setMaterialName()`
/// looks the name up again, which now finds the new object.
void repoint_materials(const std::vector<Ogre::Entity*>& entities,
                       const std::vector<std::string>& redefined, const std::string& body_dir) {
    if (redefined.empty() || entities.empty()) return;
    auto& materials = Ogre::MaterialManager::getSingleton();
    size_t repointed = 0;
    for (Ogre::Entity* entity : entities) {
        if (entity == nullptr) continue;
        for (unsigned i = 0; i < entity->getNumSubEntities(); ++i) {
            Ogre::SubEntity* sub = entity->getSubEntity(i);
            const std::string name = sub->getMaterialName();
            if (std::find(redefined.begin(), redefined.end(), name) == redefined.end()) continue;
            // **The adopted material has not been qualified**, and it has to
            // be qualified *before* anything points at it. Only the materials
            // of the *new* piece's own submeshes are qualified, in equip(), and
            // a script routinely declares more names than its mesh uses - the
            // Vanquisher's hair redeclares all six `Material_#1/<slot>` and
            // uses one. The body then adopts names whose texture units still
            // say a bare `body.png`, which is ambiguous across every character
            // directory and opens nothing: "Texture layer will be blank".
            //
            // The order matters: `SubEntity::setMaterialName()` loads the
            // material it is given, so rewriting the paths afterwards is too
            // late - OGRE has already tried the bare name, logged the error and
            // cached a blank layer.
            qualify_texture_paths(materials.getByName(name, kGameGroup), body_dir, body_dir);
            sub->setMaterialName(name, kGameGroup);
            ++repointed;
        }
    }
    if (repointed > 0) {
        TL_LOG_INFO("  %zu submesh(es) re-pointed at redefined material(s)", repointed);
    }
}

void parse_sibling_material(const std::string& mesh_name, std::vector<std::string>* redefined,
                            bool force) {
    const size_t dot = mesh_name.find_last_of('.');
    const std::string script =
        (dot == std::string::npos ? mesh_name : mesh_name.substr(0, dot)) + ".material";

    // From the index: `resourceExists` answers a path through `ZipArchive::exists`, a
    // lower-cased pass over every entry comparing basenames (STRICT=0), once per placed mesh;
    // most of a dungeon floor's load went there. The index answers the same question.
    std::string basename;
    std::string path;
    Ogre::StringUtil::splitFilename(script, basename, path);
    if (archive_entries(basename) != nullptr) {
        parse_material_script(script, redefined, force);
        return;
    }

    // No script with the mesh's own stem. The material is defined somewhere
    // else, and in practice that is another script in the same directory:
    // `palace_gradient_01.Mesh` names `palace_lights_mat`, which lives in
    // `palace_circle_01.material` beside it. Parse the directory rather than
    // the archive - parsing all 1,181 scripts is fatal, because the archive
    // defines the same material name in more than one file and OGRE aborts the
    // whole parse on the duplicate.
    const std::string dir = dir_of_asset(mesh_name);
    if (dir.empty()) return;

    // From the index rather than by scanning the archive for "*.material",
    // which is another full pass over 22,426 entries and used to happen once per
    // mesh that had no script of its own.
    ensure_archive_index();
    const auto scripts = g_materials_by_dir.find(lowered(dir));
    size_t parsed = 0;
    if (scripts != g_materials_by_dir.end()) {
        for (const std::string& script : scripts->second) {
            parse_material_script(script, redefined, force);
            ++parsed;
        }
    }
    if (parsed == 0) {
        TL_LOG_INFO("no material script for '%s' in '%s'", mesh_name.c_str(), dir.c_str());
    }
}

/// The animation *state* one `.SKELETON` beside a mesh plays.
///
/// Torchlight names an animation file `<STATE>[_Armed|_Unarmed][<n>]`, and
/// `TorchED/Core.dll` carries the state names as one run of literals at
/// onwards: `IDLE`, `RUN`, `WALK`, `ATTACK`, `DIE`, `HIT`,
/// `STUNNED`, `ASLEEP`, `FIDGET`, `SPAWN`, `DROP`, `BIND`, `JUMP_DOWN`,
/// `JUMP_LAND`, `DEATH_LAND`, `DEATH_FLIGHT`, `SPECTRAL`, `FISHING_IDLE`,
/// `FISHING_CAST`, `FISHING_BITE`, `FISHING_CATCH`. Everything under
/// `media/models/` follows it: the Alchemist has `Idle`, `Run`, `Die1`,
/// `Attack1..3`, and the skeleton warriors have `Idle_Armed`, `Run_Unarmed`,
/// `Die_Armed2`.
///
/// This is what turns a filename back into the state, so a caller can ask for
/// `Die` and be given `Die_Armed2` on a mesh that only ships that.
// PORT: an animation file's stem read back as its state, ARMED flag and variation (asset lookup).
std::string animation_state_of(const std::string& stem, bool& armed, bool& has_armed_suffix,
                               int& variation) {
    std::string name = lowered(stem);
    armed = true;
    has_armed_suffix = false;
    variation = 0;

    // A trailing number is the variation. `Die_Armed2` is the second death.
    size_t end = name.size();
    while (end > 0 && name[end - 1] >= '0' && name[end - 1] <= '9') --end;
    if (end < name.size()) {
        variation = std::atoi(name.c_str() + end);
        name.resize(end);
    }

    // ... and a trailing `_armed` / `_unarmed` is the unit's `ARMED` flag.
    const char* suffixes[] = {"_unarmed", "_armed"};
    for (const char* suffix : suffixes) {
        const size_t n = std::strlen(suffix);
        if (name.size() > n && name.compare(name.size() - n, n, suffix) == 0) {
            armed = suffix[1] != 'u';
            has_armed_suffix = true;
            name.resize(name.size() - n);
            break;
        }
    }
    return name;
}

/// The `.SKELETON` beside `dir` that plays `state` for a unit that is or is not
/// armed, as a full archive path. Empty when the mesh ships none.
///
/// Preference order, and each step is a fact about the shipped data rather than
/// a taste: the file whose name matches exactly; then the one whose `_Armed` /
/// `_Unarmed` suffix agrees with the unit; then any file for that state at all,
/// because a mesh that ships only one is not offering a choice.
std::string archive_animation_in_dir(const std::string& dir, const std::string& state, bool armed,
                                     int variation) {
    const std::string wanted = lowered(state);

    std::string exact, matching, any, other;
    int matching_variation = -1, any_variation = -1, other_variation = -1;
    for (const std::string& path : archive_paths_under(dir, ".skeleton")) {
        std::string stem = path;
        const size_t slash = stem.find_last_of('/');
        if (slash != std::string::npos) stem = stem.substr(slash + 1);
        const size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem.resize(dot);

        bool is_armed = true, has_suffix = false;
        int number = 0;
        const std::string named = animation_state_of(stem, is_armed, has_suffix, number);
        if (lowered(stem) == wanted) exact = path;
        if (named != wanted) continue;
        if (has_suffix && is_armed != armed) {
            // The state under the other suffix: the last resort below. The
            // Enslaved is an armed unit on `skeleton_unarmed.MESH`, whose
            // files are `Idle_Unarmed`, `Walk_Unarmed`, `Run_Unarmed` and
            // `Die_Unarmed` and nothing else for those states - it slid about
            // in its last attack's pose and vanished when killed.
            if (other_variation < 0 || number < other_variation) {
                other = path;
                other_variation = number;
            }
            continue;
        }
        // The variation asked for, or the lowest-numbered one there is.
        if (has_suffix || is_armed == armed) {
            if (number == variation) return path;
            if (matching_variation < 0 || number < matching_variation) {
                matching = path;
                matching_variation = number;
            }
        }
        if (any_variation < 0 || number < any_variation) {
            any = path;
            any_variation = number;
        }
    }
    if (!exact.empty()) return exact;
    if (!matching.empty()) return matching;
    if (!any.empty()) return any;
    return other;
}

/// Every `.SKELETON` stem beside `dir` whose name contains `substring`.
///
/// **A substring, not a state.** `archive_animation_in_dir()` above asks for an
/// animation *class* and understands the `_Armed` and numeric suffixes the
/// archive uses; that is the wrong tool for a fidget, because the dog's two are
/// `fidget_alert` and `fidget_shake` and neither reduces to the state `fidget`.
/// The original asks for these by substring for exactly that reason -
/// `CMonster::idleAI` calls `CGenericModel::animationExistsSubstring("FIDGET")`
/// and then `findRandomAnimation("FIDGET")` - so this mirrors it.
///
/// Stems rather than paths, because `play_scene_animation()` and
/// `link_subject_animation()` both take a name and resolve the file themselves.
/// Sorted, so two targets choose the same one from the same roll.
std::vector<std::string> archive_animation_stems_matching(const std::string& dir,
                                                          const std::string& substring) {
    const std::string wanted = lowered(substring);
    std::vector<std::string> stems;
    for (const std::string& path : archive_paths_under(dir, ".skeleton")) {
        std::string stem = path;
        const size_t slash = stem.find_last_of('/');
        if (slash != std::string::npos) stem = stem.substr(slash + 1);
        const size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem.resize(dot);
        if (lowered(stem).find(wanted) == std::string::npos) continue;
        stems.push_back(stem);
    }
    std::sort(stems.begin(), stems.end());
    return stems;
}

/// The archive path of `basename` when exactly one file has that name.
///
/// Only useful when the name is unambiguous, which is the point: an ambiguous
/// basename is the trap this codebase keeps hitting, so this deliberately
/// refuses to guess between candidates.
/// A playable class's own `.DAT`, the console's copy first.
///
/// Spelled as a path rather than looked up by basename, because the
/// basename stopped being unique when `scripts/extract_console_ui.py` began
/// putting the Xbox release's own class files under `xbox/` (that release
/// re-laid every class's skills for a pad). `archive_path_unique` rightly
/// refuses to choose between two candidates, so every caller of it for a
/// class file quietly got nothing: the character sheet lost its strength,
/// dexterity, magic, defense, health and mana, and the wardrobe lost the
/// base textures. The console's copy wins, as it does everywhere else the
/// console data is read (`build_player_skills`).
///
/// `console` reports which of the two roots answered, so a caller can
/// follow the file's `BASEFILE` in the same tree.
// PORT: asset lookup: a class's unit file, the console's tree first.
std::string class_unit_path(const std::string& class_name, bool* console) {
    if (console != nullptr) *console = false;
    if (class_name.empty()) return {};
    const std::string want = lowered(class_name) + ".dat.adm";
    for (const char* root : {"xbox/media/units/players/", "media/units/players/"}) {
        for (const std::string& path : archive_paths_under(root, ".dat.adm")) {
            const size_t slash = path.find_last_of('/');
            const std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
            if (lowered(base) != want) continue;
            if (console != nullptr) *console = root[0] == 'x';
            return path;
        }
    }
    return {};
}

/// Rewrites bare texture names in a material to full archive paths.
///
/// Material scripts name textures relative to their own directory - the
/// Alchemist's materials just say "body.png" - but the archive index is keyed
/// on basename alone, and *every* character in Torchlight has a body.png. Faced
/// with that ambiguity OGRE's ZipArchive deliberately opens none of them, and
/// the model renders untextured with only a log line to say why.
///
/// Qualifying the name against the mesh's own directory is the same rule
/// AssetIndex::resolve_near() applies to intra-asset references: a sibling of
/// the referring asset wins.
/// Whether a texture unit's name is the archive's to qualify: not empty, and
/// not one of the port's own render textures (`tl/ProjectorRTTLight`, the
/// light map's), which a material is given after its first qualification.
bool archive_texture_name(const std::string& name) {
    return !name.empty() && name.rfind("tl/", 0) != 0;
}

/// Qualify every frame of an `anim_texture` texture unit, in place.
///
/// Separate from the single-frame path because that path cannot be reused:
/// `setTextureName()` collapses the unit to one frame, which is exactly the
/// defect this exists to avoid. A frame that cannot be resolved is left as it
/// was authored rather than dropped - a flipbook with a hole in it still
/// animates, one whose length changed does not.
void qualify_animated_frames(Ogre::TextureUnitState* unit, const std::string& dir,
                             const std::string& fallback_dir) {
    const unsigned int frames = unit->getNumFrames();
    size_t resolved = 0, missing = 0;
    for (unsigned int i = 0; i < frames; ++i) {
        const Ogre::String name = unit->getFrameTextureName(i);
        if (name.empty()) continue;
        // Already this function's own earlier output - see the note on the
        // same test below.
        if (name.find('/') != Ogre::String::npos && !resolve_archive_path_quiet(name).empty()) {
            ++resolved;
            continue;
        }
        const std::string path = resolve_texture(name, dir, fallback_dir);
        if (path.empty()) {
            ++missing;
            continue;
        }
        unit->setFrameTextureName(path, i);
        ++resolved;
    }
    TL_LOG_INFO("  animated texture: %zu of %u frame(s) resolved%s, %.2fs loop", resolved, frames,
                missing > 0 ? " (the rest left as authored)" : "",
                static_cast<double>(unit->getAnimationDuration()));
}

void qualify_texture_paths(const Ogre::MaterialPtr& material, const std::string& dir,
                           const std::string& fallback_dir) {
    if (!material || dir.empty()) return;

    for (Ogre::Technique* technique : material->getTechniques()) {
        for (Ogre::Pass* pass : technique->getPasses()) {
            for (Ogre::TextureUnitState* unit : pass->getTextureUnitStates()) {
                // **A flipbook is many frames, and only one of them is what
                // `getTextureName()` returns.** `anim_texture <base> <n>
                // <seconds>` fills `mFramePtrs` with n textures; OGRE's
                // `getTextureName()` hands back whichever frame is showing -
                // frame 0, at load - and `setTextureName()` goes through
                // `TextureUnitState::setTexture()`, whose second line is
                // `mFramePtrs.resize(1)`. So qualifying an animated unit the
                // ordinary way *threw the other frames away*, and every
                // flipbook in the game stopped: the mine's and crypt's water,
                // the town river's second pass, the waterfalls.
                //
                // That is the whole of why the water does not move.
                // `crypt_water` carries no `wave_xform` at all - its entire
                // animation is `anim_texture ../../sharedtextures/water/
                // WATER.dds 80 7.4`, eighty frames over 7.4 seconds - and
                // `river_water`'s visible motion is the same thing on its
                // additive second pass, the sine scroll on the first being
                // 0.07 of a texel. An earlier pass measured the scroll going
                // live and concluded the uniform was never reaching the GPU;
                // the scroll *was* live, and almost invisible, and the
                // flipbook beside it was a single frame.
                //
                // `setFrameTextureName()` replaces one frame in place and
                // leaves the frame count and `mAnimDuration` alone, so the
                // controller `_load()` builds still has something to step.
                if (unit->getNumFrames() > 1) {
                    qualify_animated_frames(unit, dir, fallback_dir);
                    continue;
                }
                const Ogre::String name = unit->getTextureName();
                if (!archive_texture_name(name)) continue;

                // A name with separators in it is normally already a full
                // archive path - this function's own output, on a second pass -
                // and re-resolving it would be wasted work. But a *relative*
                // reference also has separators, and skipping every
                // slash-bearing name on the strength of that alone is why the
                // town's river rendered as a flat yellow band: `water.material`
                // asks for `../../sharedtextures/watertown/WATER_Base.dds`,
                // which is neither a basename the index can answer nor a path
                // the archive contains as written, so OGRE reported "Cannot
                // locate resource" and left the layer blank.
                //
                // **A leading `.` is not what tells the two apart.** That was
                // the first fix here, and it missed `cave_gate_doorway_01.
                // material`'s own `cave_tusk_fencing_gate_01/
                // cave_tusk_fencing_gate.png` - a relative reference with no
                // leading dot at all, one directory *down* rather than up.
                // Checked instead by asking the archive directly: a name that
                // already resolves as a full path is this function's own
                // earlier output and needs nothing further; one that does not
                // is a reference still waiting to be qualified, whichever way
                // it is spelled.
                if (name.find('/') != Ogre::String::npos &&
                    !resolve_archive_path_quiet(name).empty()) {
                    continue;
                }

                // Looked up rather than merely prefixed, for two reasons. The
                // archive supplies the entry's real spelling, which matters
                // because ZipArchive::exists() compares basenames exactly. And
                // a texture that is not a sibling is caught here instead of
                // becoming a path that resolves to nothing: a worn set's
                // material asks for `body.png`, which lives with the character
                // model and not in the wardrobe directory at all.
                const std::string path = resolve_texture(name, dir, fallback_dir);
                if (path.empty()) {
                    TL_LOG_WARN("  texture '%s' not found near '%s'; layer will be blank",
                                name.c_str(), dir.c_str());
                    continue;
                }
                // Of the unit's own type: `setTextureName`'s default is 2D, and
                // a `cubic_texture ... combinedUVW` (a ghost's `falloffcube.dds`,
                // `envmap.dds`) qualified as one was refused by `env_map
                // cubic_*` and drew nothing.
                unit->setTextureName(path, unit->getTextureType());
                TL_LOG_INFO("  texture %s -> %s", name.c_str(), path.c_str());
            }
        }
    }
}

/// The directory part of an asset name, with a trailing slash.
///
/// A name carrying a '/' is already an archive path and answers for itself -
/// which matters because findResourceFileInfo() cannot answer for it. Under
/// OGRE_RESOURCEMANAGER_STRICT=0 the archive indexes entries by basename, so a
/// full-path pattern matches nothing at all. Only a bare basename can be looked
/// up, and only then is the index worth consulting.
std::string dir_of_asset(const std::string& name) {
    const size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) return name.substr(0, slash + 1);
    return archive_dir_of(name);
}

/// The equipment slot a material name denotes, lowercased.
///
/// Torchlight names every character material `<Set>/<slot>` - `Starter_Set/chest`
/// on the base body, `Plate_Set/chest` on the worn armour - so the part after
/// the slash says which part of the body the submesh is, independently of which
/// set it came from. That is what makes it possible to work out what a worn set
/// replaces without a hardcoded table.
/// The name this entity's skeleton actually gives an animation, matched without
/// regard to case, or empty when it has none like it.
///
/// **A skeleton file names its animation internally and the three playable
/// classes do not agree on the spelling.** `media/models/Alchemist/Idle.SKELETON`
/// and `media/models/Vanquisher/Idle.SKELETON` both call theirs `Idle`;
/// `media/models/warrior/idle.SKELETON` calls its own `idle`, lower case. An
/// exact match therefore finds the Alchemist's and the Vanquisher's and misses
/// the Destroyer's, and a missed animation is not an error anywhere - the model
/// simply stands in its bind pose, arms out, which is what it did.
///
/// The same rule the rest of this engine applies to every other name the data
/// carries: asset lookup is case-insensitive. The filename is
/// no guide either: `Sage/Special_Focus_Cast.SKELETON` is not obliged to call
/// its animation `Special_Focus_Cast`.
std::string animation_named(const Ogre::Entity* entity, const std::string& wanted) {
    if (entity == nullptr || entity->getAllAnimationStates() == nullptr) return {};
    const Ogre::AnimationStateSet* states = entity->getAllAnimationStates();
    if (states->hasAnimationState(wanted)) return wanted;
    const std::string want = lowered(wanted);
    for (const auto& entry : states->getAnimationStates()) {
        if (lowered(entry.first) == want) return entry.first;
    }
    return {};
}

std::string slot_of_material(const std::string& material_name) {
    const size_t slash = material_name.find_last_of('/');
    std::string slot =
        (slash == std::string::npos) ? material_name : material_name.substr(slash + 1);
    for (char& c : slot) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return slot;
}

/// Give a wardrobe material script the textures it left out.
///
/// **The original does not read a worn set's texture from its material script
/// at all** - it takes it from the item that granted the piece. Every armour
/// item's `.adm` carries one `[WARDROBE]` group per class:
///
///     [WARDROBE]
///       CLASS:VANQUISHER
///       MESH:media/wardrobe/vanquisher/vanquisher_assassin_set.mesh
///       TEXTURE:media/wardrobe/vanquisher/vanq_assassin_body.png
///
/// which is why 202 of the archive's 322 wardrobe materials leave their
/// `texture_unit` empty: the sheet is not a property of the mesh, it is a
/// property of the item, and one set mesh is recoloured by many items. Across
/// all 3,081 item files those groups name 34 wardrobe meshes and 102 textures.
///
/// **This engine has no inventory yet**, so `--equip <mesh>` names a mesh with
/// no item behind it and there is nothing to ask. What follows stands in for
/// that lookup until there is one, and a measurement script checks it
/// against the item data. When items arrive, this should be replaced by reading
/// the granting item's `[WARDROBE] TEXTURE`, not extended.
///
/// **The shipped scripts are incomplete, and consistently so.** The Destroyer's
/// `media/wardrobe/destroyer/plate_set.material` declares all seven of the
/// set's materials and names a texture in only two of them; the other five
/// carry an empty `texture_unit`, and the artwork they want -
/// `Destroyer_Plate_Chest.png` and its siblings - is sitting in the same
/// directory. 202 of the archive's 322 wardrobe materials are like this, and
/// without something here every class but the Alchemist wears flat colour.
///
/// The rule for an empty unit: a texture in the script's own directory whose
/// name carries the *set's* word - the script's own filename with any trailing
/// `_set` dropped, taken as whole underscore-separated words - and whose last
/// word is the material's slot, allowing the other number (`shoulders` is
/// stored as `..._Shoulder`). Only when exactly one file matches; an ambiguous
/// scan says nothing.
///
/// `base` and `face` are the bare skin a set carries around its armour, and no
/// set art exists for them. The Alchemist's complete scripts give them plain
/// `body.png`, which `qualify_texture_paths()` then resolves against the
/// *wearer's* own model directory - so that is what is written, and it is a
/// fallback rather than part of the rule: two of the Vanquisher's scripts point
/// their skin materials at a chest texture instead.
///
/// **Checked against the scripts that are complete**, which is the oracle the
/// archive provides for free. Over the 120 wardrobe materials that *do* name a
/// texture the rule predicts 37 exactly, says nothing about 74, and disagrees
/// with 9 - every one of those a `Material_#1` family in one of the
/// Vanquisher's two scripts, where that family is the skin and names
/// `body.png`. All 9 name their texture, so nothing here touches them: **on the
/// 85 empty units this does fill, it disagrees with nothing.**
///
/// Done to the script text before OGRE parses it, like the two passes above and
/// for a harder reason than tidiness: setting a texture on a `Material` that
/// has already been loaded leaves the RTShaderSystem holding a technique built
/// against the old pass, and the next draw fails with "RenderSystem does not
/// support FixedFunction" - fatal on both of this port's render systems. See
/// the long note in parse_material_script().
std::string fill_empty_texture_units(const std::string& text, const std::string& script,
                                     size_t& filled) {
    filled = 0;
    // Only the wardrobe. A level's materials leave units empty for their own
    // reasons and nothing here knows what those are.
    const std::string where = lowered(script);
    if (where.compare(0, 16, "media/wardrobe/") != 0 &&
        where.find("/wardrobe/") == std::string::npos) {
        return text;
    }

    const std::string dir = dir_of_asset(script);
    std::string stem = script.substr(script.find_last_of('/') + 1);
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem.resize(dot);
    std::vector<std::string> stem_words = underscore_words(lowered(stem));
    if (!stem_words.empty() && stem_words.back() == "set") stem_words.pop_back();
    if (stem_words.empty() || dir.empty()) return text;
    const std::string set_word = stem_words.back();

    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        const bool last = (end == std::string::npos);
        if (last) end = text.size();
        lines.push_back(text.substr(start, end - start));
        if (last) break;
        start = end + 1;
    }

    auto trimmed_of = [](const std::string& line) {
        std::string body = line;
        while (!body.empty() && (body.back() == '\r' || body.back() == ' ')) body.pop_back();
        const size_t first = body.find_first_not_of(" \t");
        return first == std::string::npos ? std::string() : body.substr(first);
    };
    auto indent_of = [](const std::string& line) {
        const size_t first = line.find_first_not_of(" \t");
        return first == std::string::npos ? std::string() : line.substr(0, first);
    };

    std::string slot;
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string trimmed = trimmed_of(lines[i]);
        if (trimmed.compare(0, 9, "material ") == 0) {
            slot = slot_of_material(trimmed.substr(9));
        }
        out += lines[i];
        if (i + 1 < lines.size()) out += "\n";
        if (trimmed != "texture_unit" || slot.empty()) continue;

        // Look ahead to the matching close brace: is there a `texture` in it?
        size_t depth = 0;
        size_t at = i + 1;
        bool has_texture = false;
        size_t insert_after = i;
        for (; at < lines.size(); ++at) {
            const std::string inner = trimmed_of(lines[at]);
            if (inner == "{") {
                ++depth;
                insert_after = at;
                continue;
            }
            if (inner == "}") {
                if (depth <= 1) break;
                --depth;
                continue;
            }
            if (inner.compare(0, 8, "texture ") == 0) has_texture = true;
        }
        if (has_texture || depth == 0) continue;

        const std::string texture = wardrobe_texture_for(script, dir, slot, set_word);
        const std::string chosen =
            texture.empty()
                ? ((slot == "base" || slot == "face") ? std::string("body.png") : std::string())
                : texture;
        if (chosen.empty()) continue;

        // Emit the lines up to and including the opening brace, then the
        // texture, and let the loop carry on from there.
        for (size_t j = i + 1; j <= insert_after; ++j) {
            out += lines[j];
            out += "\n";
        }
        out += indent_of(lines[insert_after]) + "    texture " + chosen + "\n";
        i = insert_after;
        ++filled;
    }
    return out;
}

/// Re-attaches a skeleton that OGRE could not open, qualified by directory.
///
/// A mesh names its skeleton by bare basename, and under
/// OGRE_RESOURCEMANAGER_STRICT=0 an ambiguous basename opens nothing at all:
/// `plate_set.SKELETON` ships for both the Alchemist and the Destroyer, so
/// `plate_set.MESH` loses its rig and loads unanimated, reporting only
/// "This Mesh will not be animated" in the log. The archive has four more of
/// these, all of them units the player meets: `Merc.SKELETON` (Brink, and the
/// Mercenary), `Mage.SKELETON` (both town mages), `gel.SKELETON` (three gels)
/// and `Rat.SKELETON` (the critter rat and the imaginary pet). `Alchemist.SKELETON` happens to
/// be unique, which is the only reason the body has never needed this.
///
/// The sibling-with-the-same-stem convention is Torchlight's own, and the same
/// one parse_sibling_material() relies on. Must be called before the entity is
/// created: Mesh::_initAnimationState() compiles the bone assignments against
/// whatever skeleton is attached at that moment, and that runs at entity
/// creation.
void ensure_skeleton(const Ogre::MeshPtr& mesh, const std::string& mesh_name,
                     const std::string& dir) {
    if (!mesh || dir.empty()) return;

    // What to look for in the mesh's own directory. Two cases: a mesh that
    // records no skeleton, and one that *names* a skeleton OGRE could not open,
    // which still answers hasSkeleton(), because that only asks whether a name
    // was recorded. The second used to bite on every name that ships in two
    // directories (`Merc.skeleton`: Brink loaded skinned with no bones and
    // thrashed); `CMeshListener::processSkeletonName` (`game_mesh_listener`)
    // now qualifies the name as the mesh loads, so this is the fallback.
    // `getSkeleton()` is the honest test: null means the load failed however
    // the name got there.
    std::string wanted;
    if (mesh->hasSkeleton()) {
        if (mesh->getSkeleton()) return;
        wanted = mesh->getSkeletonName();
        const size_t slash = wanted.find_last_of("/\\");
        if (slash != std::string::npos) wanted = wanted.substr(slash + 1);
    } else {
        std::string stem = mesh_name.substr(dir.size() > mesh_name.size() ? 0 : dir.size());
        const size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem.resize(dot);
        wanted = stem + ".SKELETON";
    }
    if (wanted.empty()) return;

    const std::string path = archive_path_in_dir(wanted, dir);
    if (path.empty()) return;

    mesh->setSkeletonName(path);
    skinned_entities().rescan();
    if (mesh->getSkeleton()) {
        TL_LOG_INFO("  skeleton recovered by qualifying to %s", path.c_str());
    }
}

/// Read a file off the filesystem, beside the archive rather than inside it.
///
/// Almost nothing needs this - the whole game is in `Pak.zip` - but the music
/// is not: the level rules author `MUSIC:../music/title.ogg`, and that `../`
/// steps out of `media/` and into a `music/` directory next to the archive.
/// Empty on any failure, which the caller treats as "no music", because on the
/// console the directory is only there if it was deployed.
std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0) return {};
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!file) return {};
    return bytes;
}

/// Logs every animation the entity can reach, with its length.
///
/// This is the check that matters after a link: an animation that failed to
/// link is not an error anywhere, it is simply a state that never appears.
// PORT: a diagnostic, an entity's animation states logged.
void log_animation_states(const Ogre::Entity* entity) {
    const Ogre::AnimationStateSet* states = entity->getAllAnimationStates();
    if (!states || states->getAnimationStates().empty()) {
        TL_LOG_WARN("  no animation states");
        return;
    }
    for (const auto& entry : states->getAnimationStates()) {
        TL_LOG_INFO("  animation state: %s (%.3fs)", entry.first.c_str(),
                    static_cast<double>(entry.second->getLength()));
    }
}

/// Whether a `.SKELETON` carries any animation *track*, as opposed to only a
/// pose.
///
/// Torchlight ships both, and they look identical from the outside: each is a
/// complete 48-bone hierarchy with one named animation. `Idle.SKELETON` declares
/// `Idle` at 4.8 seconds over 43 tracks; `Dead1.SKELETON` declares `Dead1` at
/// **zero seconds over zero tracks** - and puts the corpse's pose in the bind
/// transforms of its own bones, 43 of the 48 of which differ from the mesh's.
///
/// That distinction is not cosmetic. `addLinkedSkeletonAnimationSource` borrows
/// *animations*, so linking a pose file borrows an empty one, and the town's
/// four dead Varkolyn and two dead trolls stood up straight - the level said
/// `Dead1` and got nothing. See pose_from_skeleton().
bool skeleton_has_tracks(const Ogre::SkeletonPtr& skeleton) {
    if (!skeleton) return false;
    for (unsigned short i = 0; i < skeleton->getNumAnimations(); ++i) {
        if (skeleton->getAnimation(i)->getNumNodeTracks() > 0) return true;
    }
    return false;
}

/// Pose an entity from a track-less `.SKELETON` by copying its bind transforms.
///
/// This is how Torchlight can ship a "dead" pose as a whole skeleton file rather
/// than as a one-frame animation, and it says something about the original: an
/// animation file there is not a set of tracks bolted onto a rig, it *is* a rig,
/// and playing one means using it in place of the mesh's own. Every animation
/// skeleton in the archive carries the full bone hierarchy, which only makes
/// sense under that reading.
///
/// Reproduced here by copying the pose onto the entity's own skeleton instance -
/// which clones its bones from the master, so this is per-entity and does not
/// leak into another copy of the same monster - and marking those bones manually
/// controlled, which is what stops `Skeleton::reset()` putting them back at the
/// start of every frame.
///
/// Bones are matched by name rather than by handle. The handles do agree here,
/// but a name is what a mismatched rig would fail on loudly instead of silently
/// deforming, and the count is logged so a partial match is visible.
size_t pose_from_skeleton(Ogre::Entity* entity, const Ogre::SkeletonPtr& pose) {
    if (!entity || !entity->hasSkeleton() || !pose) return 0;

    Ogre::Skeleton* skeleton = entity->getSkeleton();
    size_t posed = 0;
    for (Ogre::Bone* bone : skeleton->getBones()) {
        if (!pose->hasBone(bone->getName())) continue;
        const Ogre::Bone* from = pose->getBone(bone->getName());
        bone->setPosition(from->getPosition());
        bone->setOrientation(from->getOrientation());
        bone->setScale(from->getScale());
        // Without this the next frame's reset() restores the binding pose and
        // the corpse stands back up.
        bone->setManuallyControlled(true);
        ++posed;
    }
    return posed;
}

/// Borrow one `.SKELETON`'s animations onto an entity's own rig.
///
/// **Not `addLinkedSkeletonAnimationSource`, and the difference is the whole
/// of what a Torchlight run cycle looks like.** A linked source is applied by
/// `Skeleton::setAnimationState()`, which calls `reset()` - putting every bone
/// back on *this* skeleton's binding pose - and then `Animation::apply()`,
/// which post-multiplies each keyframe onto it (`OgreSkeleton.cpp`,
/// `OgreNodeAnimationTrack.cpp`: `LocalTransform = BindTransform * KeyFrame`).
/// That is only correct when the two files share a binding pose.
///
/// Torchlight's do not. Every animation file is a **whole rig**, exported on
/// its own, and its bones' rest transforms differ from the mesh's:
/// `Run.SKELETON` and `Alchemist.SKELETON` disagree on 37 of their 39 shared
/// bones - 0.055 units on `Bip01` and a tenth of a radian on `Bip01 L Calf`.
/// Applying Run's keys to the Alchemist's binding pose folded the character up
/// into a crouch with the legs splayed sideways, which is exactly what a
/// half-rebased pose looks like.
///
/// `Skeleton::_mergeSkeletonAnimations()` is OGRE's own answer to this, and its
/// header says so: "It's useful for exporting skeleton animations separately
/// ... even if used completely difference binding pose". It copies the tracks
/// and rebases every key:
///
///     DestKeyFrame = inverse(DestBindTransform) * SrcBindTransform * SrcKeyFrame
///
/// which is the same convention pose_from_skeleton() already uses for the
/// track-less pose files - the posed bone comes from the animation's rig and
/// the inverse binding transform from the mesh's.
///
/// Merged into the **master** skeleton rather than the entity's instance, for
/// the same reason a linked source went there: animations are shared, so the
/// second and later copies of a monster find the state already present. Bones
/// are mapped **by name**; the handles happen to agree throughout the archive,
/// but a rig that disagreed would silently deform under a handle map and is
/// caught by a name one.
bool link_path_onto(Ogre::Entity* entity, const std::string& path) {
    if (!entity || !entity->hasSkeleton()) return false;

    Ogre::SkeletonPtr source;
    try {
        source = Ogre::static_pointer_cast<Ogre::Skeleton>(
            Ogre::SkeletonManager::getSingleton().load(path, kGameGroup));
    } catch (const Ogre::Exception& e) {
        TL_LOG_ERROR("could not load '%s': %s", path.c_str(), e.getDescription().c_str());
        return false;
    }

    const Ogre::SkeletonPtr& master = entity->getMesh()->getSkeleton();
    if (!master) return false;

    // Only what is not there yet: createAnimation() throws on a duplicate name,
    // and the ordinary case is a second copy of a monster whose master skeleton
    // already carries the animation the first one merged.
    Ogre::StringVector wanted;
    for (unsigned short i = 0; i < source->getNumAnimations(); ++i) {
        const Ogre::String& name = source->getAnimation(i)->getName();
        if (!master->hasAnimation(name)) wanted.push_back(name);
    }

    if (!wanted.empty()) {
        Ogre::Skeleton::BoneHandleMap bones;
        master->_buildMapBoneByName(source.get(), bones);

        // Would the merge have to *add* bones? The usual answer is no - an
        // animation rig is a subset of the body's, 39 bones to 43 - and OGRE's
        // own merge is then the right thing and does the hierarchy checking
        // with it. When the answer is yes, do it by hand instead and keep only
        // the tracks this skeleton has bones for; see merge_shared_animations()
        // for why growing a live skeleton crashes.
        const unsigned short existing = master->getNumBones();
        bool grows = false;
        for (unsigned short handle : bones) {
            if (handle >= existing) {
                grows = true;
                break;
            }
        }

        try {
            if (grows) {
                const size_t dropped = merge_shared_animations(master, source.get(), bones, wanted);
                TL_LOG_INFO("  '%s' animates bones '%s' has not got; %zu track(s) left out",
                            path.c_str(), master->getName().c_str(), dropped);
            } else {
                master->_mergeSkeletonAnimations(source.get(), bones, wanted);
            }
        } catch (const Ogre::Exception& e) {
            TL_LOG_ERROR("could not merge '%s': %s", path.c_str(), e.getDescription().c_str());
            return false;
        }
    }

    entity->refreshAvailableAnimationState();
    return true;
}

}  // namespace renderer_detail
bool Renderer::Impl::setup_shader_generator() {
    if (!Ogre::RTShader::ShaderGenerator::initialize()) {
        TL_LOG_ERROR("RTShaderSystem failed to initialise");
        return false;
    }
    shader_generator = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
    shader_generator->addSceneManager(scene);

    // Fog per pixel, not RTSS's default per vertex. Fixed-function GL, which
    // Torchlight's OGRE 1.6 drew with, interpolates the fog distance and
    // clamps the factor per fragment; a per-vertex factor is clamped at the
    // corners instead, so a level's 2000-unit WATER/MIDWATER quad, every
    // corner far beyond the fog's end, came out as flat fog colour (the
    // Sunken Temple's lake as solid teal).
    if (auto* fog =
            shader_generator->getRenderState(Ogre::MSN_SHADERGEN)->getSubRenderState("FFP_Fog")) {
        fog->setParameter("calc_mode", "per_pixel");
    }

    // Lighting per vertex, as the Xbox build's generated shaders light: every
    // lit vertex program in a captured town frame calls
    // FFP_Light_Directional_Diffuse in the vertex shader and hands the colour
    // to the pixel shader (rtshaderlib/FFPLib_Lighting.hlsl).
    // OGRE 14's RTSS lights per pixel by default (SGX_PerPixelLighting).
    if (Ogre::RTShader::RenderState* state =
            shader_generator->getRenderState(Ogre::MSN_SHADERGEN)) {
        if (Ogre::RTShader::SubRenderState* per_pixel =
                state->getSubRenderState("SGX_PerPixelLighting")) {
            state->removeSubRenderState(per_pixel);
            state->addTemplateSubRenderState(
                shader_generator->createSubRenderState("FFP_Lighting"));
        }
    }

    shader_resolver = std::make_unique<ShaderResolver>(shader_generator, &gpu_skinning);
    Ogre::MaterialManager::getSingleton().addListener(shader_resolver.get());

    gpu_skinning.start(shader_generator);
    g_view_renderables.gpu = &gpu_skinning;
    render_behind_materials = std::make_unique<RenderBehindMaterials>(&gpu_skinning);
    g_view_renderables.behind = render_behind_materials.get();
    return true;
}

/// After the window, because that is when the render system has looked at the
/// driver and knows whether it hands out program binaries at all; before any
/// material, because a program links the first time it is drawn.
///
/// What the file holds is keyed by a hash of each program's GLSL, and the
/// driver checks a binary before it takes it: a file from another build or
/// another driver costs a failed load and the ordinary link, never a wrong
/// shader. So does a file that does not read.
void Renderer::Impl::load_shader_cache() {
    if (shader_cache_file.empty()) return;
    if (!Ogre::GpuProgramManager::canGetCompiledShaderBuffer()) {
        TL_LOG_INFO("shader cache: the driver offers no program binaries; nothing kept");
        return;
    }
    Ogre::GpuProgramManager& programs = Ogre::GpuProgramManager::getSingleton();
    programs.setSaveMicrocodesToCache(true);

    std::vector<uint8_t> bytes = read_file(shader_cache_file);
    if (bytes.empty()) return;
    try {
        programs.loadMicrocodeCache(std::make_shared<Ogre::MemoryDataStream>(
            bytes.data(), bytes.size(), /*freeOnClose=*/false, /*readOnly=*/true));
        TL_LOG_INFO("shader cache: %zu KB from %s", bytes.size() / 1024, shader_cache_file.c_str());
    } catch (const std::exception& e) {
        TL_LOG_WARN("shader cache: '%s' did not read (%s); linking afresh",
                    shader_cache_file.c_str(), e.what());
    }
}

/// Before Root goes, which takes the programs with it. Only when something was
/// linked that the file did not have, so a level that brought nothing new
/// writes nothing. Through a second file, so a write that stops half way
/// leaves no cache rather than half of one.
// PORT: OGRE's shader cache written to the card.
void Renderer::Impl::save_shader_cache() {
    if (shader_cache_file.empty() || !Ogre::GpuProgramManager::getSingletonPtr()) return;
    Ogre::GpuProgramManager& programs = Ogre::GpuProgramManager::getSingleton();
    if (!programs.getSaveMicrocodesToCache() || !programs.isCacheDirty()) return;

    const std::string partial = shader_cache_file + ".part";
    {
        std::fstream file(partial, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!file) {
            TL_LOG_WARN("shader cache: could not write '%s'", partial.c_str());
            return;
        }
        try {
            programs.saveMicrocodeCache(
                std::make_shared<Ogre::FileStreamDataStream>(&file, /*freeOnClose=*/false));
        } catch (const std::exception& e) {
            TL_LOG_WARN("shader cache: not saved (%s)", e.what());
            file.close();
            std::remove(partial.c_str());
            return;
        }
    }
    // Removed first: the console's file system will not rename onto a file
    // that exists.
    std::remove(shader_cache_file.c_str());
    if (std::rename(partial.c_str(), shader_cache_file.c_str()) != 0) {
        TL_LOG_WARN("shader cache: could not move '%s' into place", partial.c_str());
        return;
    }
    TL_LOG_INFO("shader cache: saved to %s", shader_cache_file.c_str());
}

namespace renderer_detail {
/// The vertical FOV to use for a viewport of this aspect.
///
/// The horizontal angle is what the game holds constant, so a wider viewport
/// gets a *narrower* vertical one. See the note in setup_scene().

ViewRenderables g_view_renderables;

}  // namespace renderer_detail
void Renderer::Impl::apply_scheme(Ogre::Viewport* viewport) const {
    viewport->setBackgroundColour(view_background);
    viewport->setMaterialScheme(Ogre::MSN_SHADERGEN);
}

void Renderer::Impl::setup_scene() {
    skinned_entities().install(*root);
    scene = root->createSceneManager();
    // So a redefining material parse can reach every entity in it; see
    // g_material_scene. Re-set here rather than only once, because
    // Renderer::restart() throws the whole scene manager away and builds
    // another, and a stale pointer here would be a crash rather than a defect.
    g_material_scene = scene;
    transparents_listener = std::make_unique<TransparentsWithScenery>(scene);
    scene->addRenderQueueListener(transparents_listener.get());
    frame_phases.timer = root->getTimer();
    frame_phases.window = window;
    frame_phases.scene = scene;
    scene->addListener(&frame_phases);
    scene->addRenderQueueListener(&frame_phases);
    // The view's queue rules (ViewRenderables); no merged scenery yet.
    g_view_renderables.opaque = nullptr;
    scene->getRenderQueue()->setRenderableListener(&g_view_renderables);
    scene->setAmbientLight(Ogre::ColourValue(0.45f, 0.45f, 0.50f));

    camera = scene->createCamera("tl/camera");

    // Field of view: `CGame::createCamera` gives `PlayerCam`
    // a vertical 35 degrees (`setFOVy(35 * DEG2RAD)`), whatever the
    // aspect. A captured town frame's projection agrees (1/tan(fovy/2) =
    // 3.1716 at 16:9). The port used the PC's settings file
    // (`FOV : 45` at 4:3, the horizontal kept), 34.5 at 16:9; the console reads
    // no such setting.
    const float aspect = config.height > 0
                             ? static_cast<float>(config.width) / static_cast<float>(config.height)
                             : 4.0f / 3.0f;
    const float fovy_degrees = config.fov_degrees > 0.0f ? config.fov_degrees : 35.0f;
    camera->setFOVy(Ogre::Radian(Ogre::Degree(fovy_degrees).valueRadians()));
    TL_LOG_INFO("camera: vertical FOV %.2f deg at %ux%u (aspect %.3f)",
                static_cast<double>(fovy_degrees), config.width, config.height,
                static_cast<double>(aspect));

    camera->setNearClipDistance(0.05f);
    camera->setAutoAspectRatio(false);

    // OGRE is built without nodeless positioning, so cameras and lights must be
    // attached to scene nodes rather than positioned directly.
    camera_node = scene->getRootSceneNode()->createChildSceneNode();
    camera_node->attachObject(camera);

    key_light = scene->createLight("tl/key");
    key_light->setType(Ogre::Light::LT_DIRECTIONAL);
    key_light->setDiffuseColour(Ogre::ColourValue(1.0f, 0.96f, 0.88f));
    key_light->setSpecularColour(Ogre::ColourValue(0.3f, 0.3f, 0.3f));

    key_light_node = scene->getRootSceneNode()->createChildSceneNode();
    key_light_node->attachObject(key_light);
    key_light_node->setDirection(Ogre::Vector3(-0.4f, -0.8f, -0.45f).normalisedCopy());

    subject_node = scene->getRootSceneNode()->createChildSceneNode();
}

namespace {
// config.txt's `fullscreen`, the desktop's; a headless run's window is never seen.
bool window_fills_screen(const RendererConfig& config) {
    return config.fullscreen && !config.headless;
}

Ogre::NameValuePairList window_params(const RendererConfig& config) {
    Ogre::NameValuePairList params{{"FSAA", "0"}};
    tl::core::platform_window_params(params);
    if (config.headless) {
        // GL still needs a context and a drawable; there is no windowless path.
        // The frame itself is rendered offscreen, so the window is never read.
        params["hidden"] = "true";
    }
#if !defined(TL_PLATFORM_SWITCH) && !defined(TL_PLATFORM_ANDROID) && !defined(TL_PLATFORM_IOS)
    // OGRE's WGL and GLX windows swap unsynchronised unless asked; the devices keep their
    // own swap interval.
    params["vsync"] = config.vsync && !config.headless ? "true" : "false";
#endif
    return params;
}

/// Every stream OGRE opens - a mesh, a texture, a material, a layout - lets the system's
/// events through while a level loads (tl::core::keep_responsive).
struct ResponsiveLoading final : Ogre::ResourceLoadingListener {
    Ogre::DataStreamPtr resourceLoading(const Ogre::String&, const Ogre::String&,
                                        Ogre::Resource*) override {
        tl::core::keep_responsive();
        return {};
    }
};

/// This thread pumps the events; OGRE's resource opens keep them moving through a load.
void keep_responsive_while_loading() {
    tl::core::mark_event_thread();
    static ResponsiveLoading listener;
    Ogre::ResourceGroupManager::getSingleton().setLoadingListener(&listener);
}
}  // namespace

bool Renderer::init(const RendererConfig& requested) {
    Impl& m = *impl_;
    m.config = requested;
    // The window may not come out the size asked for (adopt_window_size); everything after it
    // is laid out for the size it has.
    const RendererConfig& config = m.config;
    if (config.settings != nullptr) {
        static constexpr float kTouchSizes[] = {0.85f, 1.0f, 1.2f};
        static constexpr float kTouchOpacities[] = {0.7f, 1.0f, 1.35f};
        set_ui_text_scale(ui_text_size_scale(config.settings->text_size));
        tl::core::touch_pad().set_visual_options(
            kTouchSizes[std::clamp(config.settings->touch_control_size, 0, 2)],
            kTouchOpacities[std::clamp(config.settings->touch_control_opacity, 0, 2)],
            std::clamp(config.settings->touch_control_style, 0, 2),
            std::clamp(config.settings->touch_control_layout, 0, 2));
        tl::core::touch_pad().set_layout_offsets(config.settings->touch_control_offsets);
        tl::core::touch_pad().set_edit_mode(config.settings->touch_control_edit);
        m.game_hud.set_compact(config.settings->compact_hud);
    }

    // Our LogManager, created before Root so Root does not make its own.
    // suppressFileOutput keeps OGRE from writing Ogre.log next to the binary:
    // everything goes through tl::core::log instead, which on the console is a
    // file on the SD card flushed line by line.
    m.log_manager = new Ogre::LogManager();
    m.log_bridge = std::make_unique<OgreLogBridge>();
    Ogre::Log* log = m.log_manager->createLog("", true, false, true);
    log->addListener(m.log_bridge.get());

    // Empty paths: no plugins.cfg, no ogre.cfg, no log file.
    m.root = std::make_unique<Ogre::Root>("", "", "");
    m.frame_clock.timer = m.root->getTimer();
    m.frame_clock.owner = &m;
    m.root->addFrameListener(&m.frame_clock);
    keep_responsive_while_loading();

    m.shader_cache_file = config.shader_cache;
#if defined(TL_PLATFORM_SWITCH) && defined(TL_SWITCH_GL3PLUS)
    // The experiment's build (render/render_system_choice.h): whichever of the
    // two this Root is to run, and only that one. GLES2 unless asked.
    const bool gl3plus = RenderSystemChoice::gl3plus_wanted();
    if (gl3plus) {
        m.gl3plus_plugin = std::make_unique<Ogre::GL3PlusPlugin>();
        m.root->installPlugin(m.gl3plus_plugin.get());
        if (!m.shader_cache_file.empty()) m.shader_cache_file += ".gl3plus";
    } else {
        m.gles2_plugin = std::make_unique<Ogre::GLES2Plugin>();
        m.root->installPlugin(m.gles2_plugin.get());
    }
    const char* const kPreferredRenderSystem = gl3plus ? "OpenGL 3+" : "OpenGL ES 2";
#elif defined(TL_PLATFORM_SWITCH) || defined(TL_PLATFORM_ANDROID) || defined(TL_PLATFORM_IOS)
    m.gles2_plugin = std::make_unique<Ogre::GLES2Plugin>();
    m.root->installPlugin(m.gles2_plugin.get());
    constexpr const char* kPreferredRenderSystem = "OpenGL ES 2";
#else
    m.gl3plus_plugin = std::make_unique<Ogre::GL3PlusPlugin>();
    m.root->installPlugin(m.gl3plus_plugin.get());
    m.gl_plugin = std::make_unique<Ogre::GLPlugin>();
    m.root->installPlugin(m.gl_plugin.get());
    constexpr const char* kPreferredRenderSystem = "OpenGL 3+";
#endif
    Ogre::STBIImageCodec::startup();

    // GL3Plus first: like the console's GLES2 it has no fixed-function
    // pipeline, so it exercises the RTSS path that M4 depends on. Legacy GL is
    // kept installed as a fallback for telling "RTSS is wrong" apart from
    // "our scene is wrong".
    Ogre::RenderSystem* chosen = nullptr;
    for (Ogre::RenderSystem* rs : m.root->getAvailableRenderers()) {
        TL_LOG_INFO("available render system: %s", rs->getName().c_str());
        const bool preferred = rs->getName().find(kPreferredRenderSystem) != Ogre::String::npos;
        if (!chosen || preferred) chosen = rs;
        if (preferred) break;
    }
    if (!chosen) {
        TL_LOG_ERROR("no render system available");
        return false;
    }
    TL_LOG_INFO("using render system: %s", chosen->getName().c_str());
    m.root->setRenderSystem(chosen);

    m.root->initialise(false);

    // ParticleUniverse registers its MovableObject factories and script
    // translators with Root, so it comes up as soon as Root has. Without it
    // the game still runs: nothing builds a ParticleUniverse system yet.
    m.particle_universe.start();

    // Sound, before the window: it is independent of everything OGRE does, it
    // may fail, and when it does the engine carries on silently rather than not
    // starting. A headless run does not open a device at all - there is nobody
    // to hear a frame dump, and a build machine has no device to open. `--mute`
    // keeps a windowed run just as silent.
    if (!config.mute && (!config.headless || config.force_audio)) {
        tl::core::audio_init();
    } else {
        m.audio_enabled = false;
    }

    Ogre::NameValuePairList params = window_params(config);

    // The driver reads it as it makes the context, which this does.
    GlThread::request();
    try {
        m.window = m.root->createRenderWindow("Torchlight Rekindled", config.width, config.height,
                                              window_fills_screen(config), &params);
    } catch (const Ogre::Exception& e) {
        // How a driver that cannot make the context this render system asks
        // for says so. The console's GL3Plus is the case in mind.
        TL_LOG_ERROR("%s: %s", chosen->getName().c_str(), e.getDescription().c_str());
        if (RenderSystemChoice::selectable() && RenderSystemChoice::gl3plus_wanted()) {
            TL_LOG_ERROR(
                "GLES2 is what this build starts on once sdmc:/switch/torchlight/gl3plus is "
                "deleted");
        }
        m.window = nullptr;
    }
    if (!m.window) {
        TL_LOG_ERROR("failed to create a render window");
        return false;
    }
    m.adopt_window_size();
    if (GlThread::started()) {
        // Before any buffer of the game's exists: see gl_thread.h.
        m.root->getRenderSystem()->getMutableCapabilities()->unsetCapability(Ogre::RSC_MAPBUFFER);
        TL_LOG_INFO("gl thread: on (core 1); buffers are shadowed and uploaded, not mapped");
    } else if (GlThread::driver_supports()) {
        TL_LOG_INFO("gl thread: off");
    }

    m.load_shader_cache();
    if (!m.mount_resources()) return false;

    Ogre::MovableObject::setDefaultVisibilityFlags(kVisibilityWorld);
    m.setup_scene();
    if (!m.setup_shader_generator()) return false;

    Ogre::Viewport* viewport = m.window->addViewport(m.camera);
    viewport->setVisibilityMask(kVisibilityWorld);
    m.apply_scheme(viewport);
    m.fit_camera_to_window();
    m.watch_window_surface();

    // Brightness and contrast: the window's last viewport.
    //
    if (!m.display_gamma.create(m.root.get(), m.window, &m.game_settings())) {
        TL_LOG_WARN("display gamma: not created; brightness and contrast will not show");
    }

    m.create_interface();

    // The in-game minimap / automap system.
    const float y_ratio = static_cast<float>(config.height) / 768.0f;
    m.automap.init(m.scene, m.window, y_ratio);
    if (config.fullscreen_map) {
        m.automap.set_mode(MapMode::Fullscreen);
    } else if (!config.minimap) {
        m.automap.set_mode(MapMode::Hidden);
    } else {
        m.automap.set_mode(MapMode::Minimap);
    }
    m.automap.set_zoom(config.map_zoom);
    m.automap.hide();  // Never render outside active gameplay
    m.automap_initialized = true;

    // The first thing on the display, before the character or the level is
    // read. Startup and `restart()` both come through here, so the New Game
    // transition is covered from the moment the new window exists rather than
    // only from the moment the layout starts loading.
    m.paint_loading_screen();

    TL_LOG_INFO("renderer ready (%ux%u%s)", config.width, config.height,
                config.headless ? ", headless" : "");
    return true;
}

void Renderer::Impl::frame_camera_on(Ogre::Entity* entity) {
    const Ogre::AxisAlignedBox bounds = entity->getBoundingBox();
    const Ogre::Vector3 centre = bounds.getCenter();
    const Ogre::Real radius = bounds.getHalfSize().length();

    // Far enough that the whole bounding sphere fits the vertical field of
    // view, with a small margin so the silhouette is not flush against the
    // edge. The sphere is a conservative fit for a tall thin character, so the
    // margin stays tight - too generous and the model is a speck in the frame,
    // which defeats the point of looking at it.
    const Ogre::Radian fov = camera->getFOVy();
    const Ogre::Real distance = (radius / Ogre::Math::Tan(fov * 0.5f)) * 1.15f;

    // Three-quarter view, slightly above the centre: a front-on orthographic
    // stare makes it hard to tell a correct mesh from a broken one.
    const Ogre::Vector3 direction = Ogre::Vector3(0.55f, 0.35f, 1.0f).normalisedCopy();
    camera_node->setPosition(centre + direction * distance);
    camera_node->lookAt(centre, Ogre::Node::TS_WORLD);

    camera->setFarClipDistance(distance + radius * 4.0f);
    camera->setNearClipDistance(std::max(0.05f, static_cast<float>(distance - radius * 2.0f)));

    TL_LOG_INFO("bounds min(%.3f, %.3f, %.3f) max(%.3f, %.3f, %.3f), camera at %.2f",
                bounds.getMinimum().x, bounds.getMinimum().y, bounds.getMinimum().z,
                bounds.getMaximum().x, bounds.getMaximum().y, bounds.getMaximum().z,
                static_cast<double>(distance));
}

bool Renderer::show_mesh(const std::string& mesh_name) {
    Impl& m = *impl_;

    // Before the mesh, so its submeshes find the materials they name.
    std::vector<std::string> redefined;
    parse_sibling_material(mesh_name, &redefined, m.force_subject_materials);
    // Anything already drawn with a name this script has just redeclared has to
    // follow it to the new object; see repoint_materials().
    repoint_materials(m.animated(), redefined, m.subject_dir);

    Ogre::MeshPtr mesh;
    try {
        mesh = Ogre::MeshManager::getSingleton().load(mesh_name, kGameGroup);
    } catch (const Ogre::Exception& e) {
        TL_LOG_ERROR("could not load mesh '%s': %s", mesh_name.c_str(), e.getDescription().c_str());
        return false;
    }

    // Qualify texture names before the entity exists. Creating an Entity loads
    // its materials, and a material loaded while its texture is still a bare
    // ambiguous basename caches a blank texture layer.
    const std::string dir = dir_of_asset(mesh_name);
    m.subject_dir = dir;
    auto& materials = Ogre::MaterialManager::getSingleton();
    for (const Ogre::SubMesh* sub : mesh->getSubMeshes()) {
        const Ogre::String& material_name = sub->getMaterialName();
        TL_LOG_INFO("  submesh material: %s", material_name.c_str());
        qualify_texture_paths(materials.getByName(material_name, kGameGroup), dir, dir);
    }

    ensure_skeleton(mesh, mesh_name, dir);

    Ogre::Entity* entity = m.scene->createEntity(mesh);
    // A cached mesh hands its submeshes' *old* MaterialPtrs to a new entity.
    rebind_materials(entity);
    m.subject_node->attachObject(entity);

    TL_LOG_INFO("loaded %s: %zu submesh(es), skeleton %s", mesh_name.c_str(),
                mesh->getSubMeshes().size(),
                entity->hasSkeleton() ? entity->getSkeleton()->getName().c_str() : "none");

    // The silent failure this project keeps tripping over: the mesh names its
    // skeleton in a different case than the file on disk, OGRE misses, and the
    // model loads unanimated with nothing but a log line to show for it.
    if (!entity->hasSkeleton()) {
        TL_LOG_WARN("'%s' loaded without a skeleton - it will not animate", mesh_name.c_str());
    } else {
        log_animation_states(entity);
    }

    m.subject = entity;
    m.frame_camera_on(entity);
    return true;
}

bool Renderer::Impl::link_subject_animation(const std::string& name) {
    if (!subject || !subject->hasSkeleton()) {
        TL_LOG_ERROR("no skinned mesh to link '%s' onto", name.c_str());
        return false;
    }

    const std::string file = name + ".SKELETON";
    const std::string path = archive_path_in_dir(file, subject_dir);
    if (path.empty()) {
        TL_LOG_ERROR("no '%s' beside the mesh in '%s'", file.c_str(), subject_dir.c_str());
        return false;
    }
    // Already done. Asking again is the ordinary case for a swing that comes
    // round a second time, and a merge that has already happened adds nothing.
    for (const std::string& already : linked_paths) {
        if (already == path) return true;
    }

    // Onto the body and onto every piece worn on it. Each has its own skeleton
    // instance, so each needs its own link; reaching through a SkeletonInstance
    // modifies the shared master skeleton it was cloned from, which is what
    // makes this a resource-level operation rather than a per-entity one.
    // A piece with no skeleton simply does not animate; that is a property of
    // that piece, not a failure of the link, and it must not stop the body from
    // being animated.
    if (!link_path_onto(subject, path)) return false;
    for (Ogre::Entity* entity : equipment) {
        if (entity->hasSkeleton()) link_path_onto(entity, path);
    }
    linked_paths.push_back(path);

    TL_LOG_INFO("linked %s onto %zu entit%s", path.c_str(), animated().size(),
                animated().size() == 1 ? "y" : "ies");
    return true;
}

bool Renderer::link_animation(const std::string& name) {
    Impl& m = *impl_;
    if (!m.link_subject_animation(name)) return false;
    log_animation_states(m.subject);
    return true;
}

void Renderer::Impl::set_subject_render_behind(bool on, bool hostile) {
    set_render_behind(subject, on, hostile);
    for (Ogre::Entity* worn : equipment) set_render_behind(worn, on, hostile);
}

Ogre::Entity* Renderer::Impl::attach_worn_mesh(const std::string& mesh_name) {
    Impl& m = *this;
    std::vector<std::string> redefined;
    parse_sibling_material(mesh_name, &redefined, m.force_subject_materials);
    repoint_materials(m.animated(), redefined, m.subject_dir);

    Ogre::MeshPtr mesh;
    try {
        mesh = Ogre::MeshManager::getSingleton().load(mesh_name, kGameGroup);
    } catch (const Ogre::Exception& e) {
        TL_LOG_ERROR("could not load wardrobe mesh '%s': %s", mesh_name.c_str(),
                     e.getDescription().c_str());
        return nullptr;
    }

    // Same rule as the body: qualify before the entity exists, because creating
    // it loads the materials and a bare ambiguous texture name caches blank.
    const std::string dir = dir_of_asset(mesh_name);
    auto& materials = Ogre::MaterialManager::getSingleton();
    for (const Ogre::SubMesh* sub : mesh->getSubMeshes()) {
        qualify_texture_paths(materials.getByName(sub->getMaterialName(), kGameGroup), dir,
                              m.subject_dir);
    }

    // A skinned piece is bound to the body's skeleton, never to the one it names:
    // `CWardrobe::update` gives its merged mesh the base
    // model's skeleton. The pieces' bone indices count the body's bones, so a set's own
    // skeleton that lacks one (the ponytail) moves every later bone's vertices onto the next.
    // "Skinned" is read off the bone assignments: OGRE forgets a skeleton it could not load
    // (the Xbox's `360_set.mesh` names one its archive does not carry).
    bool skinned = !mesh->getBoneAssignments().empty();
    for (const Ogre::SubMesh* sub : mesh->getSubMeshes()) {
        skinned = skinned || !sub->getBoneAssignments().empty();
    }
    const Ogre::Mesh* body = m.subject != nullptr ? m.subject->getMesh().get() : nullptr;
    if (skinned && body != nullptr && body->getSkeleton()) {
        if (mesh->getSkeletonName() != body->getSkeletonName()) {
            mesh->setSkeletonName(body->getSkeletonName());
            skinned_entities().rescan();
            TL_LOG_INFO("  '%s' bound to the body's skeleton (%s)", mesh_name.c_str(),
                        body->getSkeletonName().c_str());
        }
    } else {
        ensure_skeleton(mesh, mesh_name, dir);
    }

    Ogre::Entity* entity = m.scene->createEntity(mesh);
    // A cached mesh hands its submeshes' *old* MaterialPtrs to a new entity.
    rebind_materials(entity);
    // A material whose name another directory had already taken was parsed
    // under an alias (g_material_alias): the piece draws with its own script's,
    // not with the body's of the same name - the Vanquisher's hair says
    // `Material_#1/helmet`, and so does her body.
    for (unsigned i = 0; i < entity->getNumSubEntities(); ++i) {
        Ogre::SubEntity* sub = entity->getSubEntity(i);
        const auto alias =
            g_material_alias.find(dir + "\x1f" + sub->getSubMesh()->getMaterialName());
        if (alias == g_material_alias.end()) continue;
        const Ogre::MaterialPtr own = materials.getByName(alias->second, kGameGroup);
        if (own) sub->setMaterial(own);
    }
    m.subject_node->attachObject(entity);
    m.equipment.push_back(entity);
    // The body's outline state, as an equipped piece takes it
    // (`CEquipment::setRenderBehind`, for each of the 12
    // slots - the wardrobe's helmet and shoulders included). Without it the
    // hair drew on the main queue ahead of the body's outline, which then
    // showed through it: the Alchemist's neck (the hair mesh's skin piece)
    // glowed blue all game.
    if (const RenderBehind* behind = render_behind_of(m.subject)) {
        set_render_behind(entity, behind->on, behind->hostile);
    }

    if (!entity->hasSkeleton()) {
        TL_LOG_WARN("'%s' has no skeleton; it will not follow the body", mesh_name.c_str());
    }

    // Catch the new piece up to whatever is already linked and playing, so the
    // order of --equip and --animation on the command line does not matter.
    if (entity->hasSkeleton()) {
        for (const std::string& path : m.linked_paths) {
            link_path_onto(entity, path);
        }
    }
    if (!m.playing.empty() && entity->getAllAnimationStates() &&
        !animation_named(entity, m.playing).empty()) {
        Ogre::AnimationState* state = entity->getAnimationState(animation_named(entity, m.playing));
        state->setEnabled(true);
        state->setLoop(true);
        state->setTimePosition(m.playing_time);
    }
    return entity;
}

// PORT: a headless verification switch (`--equip`): a worn mesh put on the model.
bool Renderer::equip(const std::string& mesh_name, const std::string& only_slot) {
    Impl& m = *impl_;
    if (!m.subject) {
        TL_LOG_ERROR("nothing to equip '%s' onto", mesh_name.c_str());
        return false;
    }
    Ogre::Entity* entity = m.attach_worn_mesh(mesh_name);
    if (entity == nullptr) return false;
    const Ogre::MeshPtr& mesh = entity->getMesh();

    // Hide the body underneath. A worn set carries its own geometry for every
    // slot it covers, including the bare skin around it, so leaving the body's
    // own submeshes drawn puts two versions of the same limb in the same place -
    // the starter gear pokes through the plate, which is what it looked like
    // before this. Only the slots the set actually provides are hidden, so a
    // set with no face submesh still shows the character's face.
    //
    // When only one slot is wanted, the rest of the piece is hidden first, and
    // only the slot actually drawn counts as replacing anything on the body.
    std::vector<std::string> provided;
    for (unsigned i = 0; i < entity->getNumSubEntities(); ++i) {
        Ogre::SubEntity* worn = entity->getSubEntity(i);
        const std::string slot = slot_of_material(worn->getMaterialName());
        if (!only_slot.empty() && slot != only_slot) {
            worn->setVisible(false);
            continue;
        }
        provided.push_back(slot);
    }
    if (!only_slot.empty()) {
        TL_LOG_INFO("  showing only the '%s' slot of %s", only_slot.c_str(), mesh_name.c_str());
    }
    for (size_t i = 0; i < m.subject->getNumSubEntities(); ++i) {
        Ogre::SubEntity* sub_entity = m.subject->getSubEntity(i);
        const std::string slot = slot_of_material(sub_entity->getMaterialName());
        if (std::find(provided.begin(), provided.end(), slot) == provided.end()) continue;
        if (!sub_entity->isVisible()) continue;
        sub_entity->setVisible(false);
        TL_LOG_INFO("  hiding body slot '%s', replaced by the worn set", slot.c_str());
    }

    TL_LOG_INFO("equipped %s (%zu submesh(es), skeleton %s)", mesh_name.c_str(),
                mesh->getSubMeshes().size(),
                entity->hasSkeleton() ? entity->getSkeleton()->getName().c_str() : "none");
    return true;
}

namespace renderer_detail {
/// Decodes an image out of the mounted archive. False with the reason logged.
bool load_archive_image(const std::string& path, Ogre::Image& out) {
    const std::vector<uint8_t> bytes = read_resource(path);
    if (bytes.empty()) return false;
    try {
        Ogre::DataStreamPtr stream(
            new Ogre::MemoryDataStream(const_cast<uint8_t*>(bytes.data()), bytes.size(), false));
        const size_t dot = path.find_last_of('.');
        out.load(stream, dot == std::string::npos ? std::string() : path.substr(dot + 1));
        return true;
    } catch (const Ogre::Exception& e) {
        TL_LOG_WARN("could not decode '%s': %s", path.c_str(), e.getDescription().c_str());
        return false;
    }
}

}  // namespace renderer_detail
bool Renderer::wear_base_outfit() {
    Impl& m = *impl_;
    if (!m.subject) {
        TL_LOG_ERROR("nothing to dress");
        return false;
    }

    // The player unit's own .DAT. **The chosen class names it**; the subject's
    // mesh stem is only the fallback for a `--mesh` run showing something that
    // is not a class at all. The stem is not enough on its own: the Destroyer's
    // mesh is `warrior/player.MESH` and the archive's only `PLAYER.DAT.adm` is
    // a table of sound names.
    std::string stem = m.player_class;
    if (stem.empty()) {
        stem = m.subject->getMesh()->getName();
        const size_t slash = stem.find_last_of('/');
        if (slash != std::string::npos) stem = stem.substr(slash + 1);
        const size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem.resize(dot);
    }

    std::string unit_path = class_unit_path(stem);
    if (unit_path.empty()) unit_path = archive_path_unique(stem + ".DAT.adm");
    if (unit_path.empty()) {
        TL_LOG_WARN("no unique '%s.DAT.adm'; leaving the base textures alone", stem.c_str());
        return false;
    }
    const std::vector<uint8_t> bytes = read_resource(unit_path);
    if (bytes.empty()) return false;
    const tl::formats::AdmFile unit = tl::formats::parse_adm(bytes.data(), bytes.size());
    if (!unit.ok) {
        TL_LOG_WARN("could not parse '%s': %s", unit_path.c_str(), unit.error.c_str());
        return false;
    }

    // `CCharacter::unitInit` reads the six
    // keys into `+0x4f0..+0x518` and `CCharacter::loadModel`
    // hands them to `CWardrobe`, which files
    // them by slot (`CWardrobe::CWardrobe`): the
    // skin, the chest, boots and gloves overlays, the helmet and shoulder
    // meshes.
    using tl::core::WardrobeSlot;
    tl::core::WardrobeBase base;
    base.skin = unit.string_or("WARDROBE_BASE", "");
    base.textures[static_cast<size_t>(WardrobeSlot::Chest)] = unit.string_or("CHEST_BASE", "");
    base.textures[static_cast<size_t>(WardrobeSlot::Boots)] = unit.string_or("BOOTS_BASE", "");
    base.textures[static_cast<size_t>(WardrobeSlot::Gloves)] = unit.string_or("GLOVES_BASE", "");
    base.meshes[static_cast<size_t>(WardrobeSlot::Helmet)] = unit.string_or("HELMET_BASE_MESH", "");
    base.meshes[static_cast<size_t>(WardrobeSlot::Shoulders)] =
        unit.string_or("SHOULDER_BASE_MESH", "");
    if (base.skin.empty()) {
        TL_LOG_WARN("no WARDROBE_BASE for '%s'; leaving the base textures alone", stem.c_str());
        return false;
    }
    m.wardrobe_base = base;
    // The unit's own name (`+0x40`), which a `[WARDROBE]` group's `CLASS` is
    // compared with.
    m.wardrobe_class = unit.string_or("NAME", stem);
    m.wardrobe_stem = stem;
    m.wardrobe_ready = true;
    m.dress_player(true);
    TL_LOG_INFO("base outfit from %s", unit_path.c_str());
    return true;
}

namespace {

/// An archive image as RGBA8, or empty. The overlays and the skin share one UV
/// layout, so every image here is the skin's size or is not used.
std::vector<uint8_t> archive_rgba(const std::string& path, uint32_t& width, uint32_t& height) {
    Ogre::Image image;
    if (path.empty() || !load_archive_image(path, image)) return {};
    if (Ogre::PixelUtil::isCompressed(image.getFormat())) {
        TL_LOG_WARN("wardrobe: '%s' is compressed; not composited", path.c_str());
        return {};
    }
    width = image.getWidth();
    height = image.getHeight();
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    const Ogre::PixelBox into(width, height, 1, Ogre::PF_BYTE_RGBA, rgba.data());
    Ogre::PixelUtil::bulkPixelConversion(image.getPixelBox(), into);
    return rgba;
}

}  // namespace

void Renderer::Impl::dress_player(bool force) {
    if (!wardrobe_ready || subject == nullptr || scene == nullptr) return;
    load_unit_types();
    const std::array<tl::core::WardrobePiece, tl::core::kWardrobeSlotCount> pieces =
        tl::core::wardrobe_pieces(wardrobe_base, &player_inventory, wardrobe_class, unit_types);
    std::string signature;
    for (const tl::core::WardrobePiece& piece : pieces) {
        signature += piece.mesh + "|" + piece.texture + ";";
    }
    if (!force && signature == worn_signature) return;
    worn_signature = signature;

    auto resolve = [&](const std::string& named) {
        // The data spells a full path in the archive's own casing, so this
        // usually resolves outright; resolve_texture is the fallback for the
        // archive's habit of shipping a .dds where the data asks for a .png.
        std::string path = resolve_archive_path(named);
        if (path.empty()) {
            const size_t cut = named.find_last_of('/');
            path = resolve_texture(cut == std::string::npos ? named : named.substr(cut + 1),
                                   dir_of_asset(named), subject_dir);
        }
        return path;
    };

    // The skin: `WARDROBE_BASE`, then each slot's image over it in slot order
    // (chest, gloves, boots, helmet, shoulders).
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> skin = archive_rgba(resolve(wardrobe_base.skin), width, height);
    if (skin.empty()) {
        TL_LOG_WARN("wardrobe: no skin '%s'", wardrobe_base.skin.c_str());
        return;
    }
    for (size_t i = 3; i < skin.size(); i += 4) skin[i] = 0xff;
    for (int slot = 0; slot < tl::core::kWardrobeSlotCount; ++slot) {
        const tl::core::WardrobePiece& piece = pieces[static_cast<size_t>(slot)];
        if (piece.texture.empty()) continue;
        uint32_t w = 0;
        uint32_t h = 0;
        const std::string path = resolve(piece.texture);
        const std::vector<uint8_t> overlay = archive_rgba(path, w, h);
        if (overlay.empty()) {
            TL_LOG_WARN("wardrobe: %s names '%s', which is not in the archive",
                        tl::core::wardrobe_slot_name(slot), piece.texture.c_str());
            continue;
        }
        if (w != width || h != height) {
            // DIVERGENCE: the original walks the skin's rows over an image of
            // any size; one that is not the skin's is left out here.
            TL_LOG_WARN("wardrobe: %s '%s' is %ux%u against a %ux%u skin; skipped",
                        tl::core::wardrobe_slot_name(slot), path.c_str(), w, h, width, height);
            continue;
        }
        tl::core::wardrobe_blend(skin.data(), overlay.data(), static_cast<size_t>(width) * height);
    }

    // The composite is 24-bit, matching the body.png it replaces: an alpha
    // channel the material was not authored for turns the character into a
    // two-tone stencil of the texture.
    Ogre::Image composite;
    composite.create(Ogre::PF_BYTE_RGB, width, height);
    Ogre::PixelUtil::bulkPixelConversion(
        Ogre::PixelBox(width, height, 1, Ogre::PF_BYTE_RGBA, skin.data()), composite.getPixelBox());
    const std::string texture_name = "tl/wardrobe/" + wardrobe_stem;
    auto& textures = Ogre::TextureManager::getSingleton();
    if (textures.getByName(texture_name, kGameGroup)) textures.remove(texture_name, kGameGroup);
    const Ogre::TexturePtr skin_texture = textures.loadImage(texture_name, kGameGroup, composite);

    // The pieces: the meshes of the last outfit go, each slot's mesh comes.
    for (Ogre::Entity*& worn : worn_pieces) {
        if (worn == nullptr) continue;
        equipment.erase(std::remove(equipment.begin(), equipment.end(), worn), equipment.end());
        if (Ogre::SceneNode* node = worn->getParentSceneNode()) node->detachObject(worn);
        scene->destroyEntity(worn);
        worn = nullptr;
    }
    std::array<bool, tl::core::kWardrobeSlotCount> drawn{};
    for (int slot = 0; slot < tl::core::kWardrobeSlotCount; ++slot) {
        const tl::core::WardrobePiece& piece = pieces[static_cast<size_t>(slot)];
        if (piece.mesh.empty()) continue;
        const std::string path = resolve_archive_path(piece.mesh);
        Ogre::Entity* entity = path.empty() ? nullptr : attach_worn_mesh(path);
        if (entity == nullptr) {
            TL_LOG_WARN("wardrobe: %s mesh '%s' is not in the archive",
                        tl::core::wardrobe_slot_name(slot), piece.mesh.c_str());
            continue;
        }
        // `OGRE_UTILITIES::createEntity` succeeding is what fills the slot
        // (the per-slot entity array), whatever the mesh's submeshes turn out to be.
        drawn[static_cast<size_t>(slot)] = true;
        worn_pieces[static_cast<size_t>(slot)] = entity;
        for (unsigned i = 0; i < entity->getNumSubEntities(); ++i) {
            Ogre::SubEntity* sub = entity->getSubEntity(i);
            sub->setVisible(
                tl::core::wardrobe_keeps_piece_submesh(slot, sub->getSubMesh()->getMaterialName()));
        }
    }

    // The body keeps what no slot's mesh replaces.
    for (unsigned i = 0; i < subject->getNumSubEntities(); ++i) {
        Ogre::SubEntity* sub = subject->getSubEntity(i);
        // By the submesh's own material: the entity's is the skin's after the
        // first dressing.
        sub->setVisible(
            tl::core::wardrobe_keeps_body_submesh(sub->getSubMesh()->getMaterialName(), drawn));
    }

    // The merged mesh draws everything but the helmet and the shoulders with one
    // material, the body's first submesh's (`getSubMesh(0)`), and
    // `setTextureOverride(skin, "HELMET", "SHOULDERS")`
    // puts the skin on its first pass's first
    // texture unit, when it has one. Here each entity's covered submeshes take
    // that material.
    auto& materials = Ogre::MaterialManager::getSingleton();
    const Ogre::MaterialPtr main = subject->getNumSubEntities() > 0
                                       ? subject->getSubEntity(0)->getMaterial()
                                       : Ogre::MaterialPtr();
    // Then the lighting `update` gives the base model's materials:
    // self-illumination 0.25, ambient and diffuse 0.6.
    const auto light = [](const Ogre::MaterialPtr& mat, bool glow) {
        if (!mat) return;
        if (glow) mat->setSelfIllumination(0.25f, 0.25f, 0.25f);
        mat->setAmbient(0.6f, 0.6f, 0.6f);
        mat->setDiffuse(0.6f, 0.6f, 0.6f, 1.0f);
    };
    for (unsigned i = 0; i < subject->getNumSubEntities(); ++i) {
        light(subject->getSubEntity(i)->getMaterial(), true);
    }
    if (main) {
        for (Ogre::Technique* tech : main->getTechniques()) {
            if (tech->getNumPasses() == 0) continue;
            Ogre::Pass* pass = tech->getPass(0);
            if (pass->getTextureUnitStates().empty()) continue;
            // By pointer, not by name: the pass is already loaded, and a name
            // has to resolve through the group all over again.
            pass->getTextureUnitState(0)->setTexture(skin_texture);
        }
    }
    const auto cover = [&](Ogre::Entity* entity) {
        for (unsigned i = 0; i < entity->getNumSubEntities(); ++i) {
            Ogre::SubEntity* sub = entity->getSubEntity(i);
            if (!sub->isVisible()) continue;
            const std::string name = sub->getSubMesh()->getMaterialName();
            if (tl::core::wardrobe_skin_covers(name)) {
                if (main) sub->setMaterial(main);
                continue;
            }
            // A helmet's or shoulders' own material, as the original draws it:
            // a copy named with the suffix, ambient and diffuse 0.6,
            // and - once the copy already exists, which is every update after
            // the first - self-illumination 0.25 too.
            // From the material the entity was given, which is the piece's own
            // script's even where its name collides with the body's (parsed
            // under an alias: the Vanquisher's hair and her body both say
            // `Material_#1/helmet`).
            const Ogre::MaterialPtr own = sub->getMaterial();
            if (!own) continue;
            const std::string copy = own->getName() + "_wardrobe";
            const bool existed = materials.resourceExists(copy, kGameGroup);
            Ogre::MaterialPtr mat = existed ? materials.getByName(copy, kGameGroup) : own;
            if (!existed) mat = mat->clone(copy);
            light(mat, existed);
            sub->setMaterial(mat);
        }
    };
    cover(subject);
    for (Ogre::Entity* worn : worn_pieces) {
        if (worn != nullptr) cover(worn);
    }
    const auto drawn_list = [&](Ogre::Entity* entity) {
        std::string out;
        for (unsigned i = 0; i < entity->getNumSubEntities(); ++i) {
            Ogre::SubEntity* sub = entity->getSubEntity(i);
            if (!sub->isVisible()) continue;
            std::string tex = "-";
            const Ogre::MaterialPtr& mat = sub->getMaterial();
            if (mat && mat->getNumTechniques() > 0 && mat->getTechnique(0)->getNumPasses() > 0 &&
                !mat->getTechnique(0)->getPass(0)->getTextureUnitStates().empty()) {
                tex = mat->getTechnique(0)->getPass(0)->getTextureUnitState(0)->getTextureName();
            }
            out += (out.empty() ? "" : ", ") + sub->getMaterialName() + " (" + tex + ")";
        }
        return out;
    };
    TL_LOG_INFO("wardrobe: body draws %s", drawn_list(subject).c_str());
    for (int slot = 0; slot < tl::core::kWardrobeSlotCount; ++slot) {
        Ogre::Entity* worn = worn_pieces[static_cast<size_t>(slot)];
        if (worn == nullptr) continue;
        TL_LOG_INFO("wardrobe: %s %s draws %s", tl::core::wardrobe_slot_name(slot),
                    worn->getMesh()->getName().c_str(), drawn_list(worn).c_str());
    }

    std::string parts;
    for (int slot = 0; slot < tl::core::kWardrobeSlotCount; ++slot) {
        const tl::core::WardrobePiece& piece = pieces[static_cast<size_t>(slot)];
        if (piece.mesh.empty() && piece.texture.empty()) continue;
        parts += std::string(parts.empty() ? "" : "; ") + tl::core::wardrobe_slot_name(slot) + " " +
                 (piece.mesh.empty() ? "-" : piece.mesh) + " / " +
                 (piece.texture.empty() ? "-" : piece.texture);
    }
    TL_LOG_INFO("wardrobe: %s dressed: %s", wardrobe_class.c_str(), parts.c_str());
}

bool Renderer::Impl::apply_subject_animation(const std::string& name, float time, bool quiet) {
    Impl& m = *this;
    if (!m.subject || !m.subject->hasSkeleton()) {
        TL_LOG_ERROR("no skinned mesh to animate");
        return false;
    }

    // Only animations the skeleton can already reach are playable here, which
    // for a Torchlight character means the bind pose plus whatever
    // link_animation() has linked. Driving that off the .animation manifest
    // instead of an explicit list is M3's job.
    // The skeleton's own spelling, which is not always the one asked for.
    const std::string playing = animation_named(m.subject, name);
    if (playing.empty()) {
        TL_LOG_ERROR("no animation '%s' on this skeleton", name.c_str());
        return false;
    }

    size_t applied = 0;
    for (Ogre::Entity* entity : m.animated()) {
        Ogre::AnimationStateSet* states = entity->getAllAnimationStates();
        if (states == nullptr) continue;
        // Resolved per entity: a worn piece carries its own skeleton and need
        // not spell the animation the way the body does.
        const std::string here = animation_named(entity, name);
        if (here.empty()) continue;

        // Disable everything else first. Without this each call *adds* an
        // enabled state instead of replacing one, and OGRE blends every enabled
        // state - so after the player's first Idle/Run switch the skeleton was
        // driven by both at full weight at once, and the result reads as a
        // character that has stopped animating rather than as a blend. Switching
        // gait a few times left three states fighting.
        //
        // A real cross-fade is the next thing this wants, and it needs the same
        // loop plus a weight ramp; a hard cut is honest about not having it yet.
        for (const auto& entry : states->getAnimationStates()) {
            if (entry.first != here) entry.second->setEnabled(false);
        }

        Ogre::AnimationState* state = entity->getAnimationState(here);
        state->setEnabled(true);
        state->setLoop(true);
        state->setTimePosition(time);
        ++applied;
    }

    // Remembered so a piece equipped later can be caught up to the same instant
    // instead of standing in its bind pose beside a moving body. The *asked
    // for* name, not the skeleton's spelling of it: the piece resolves it
    // against its own skeleton, which may spell it differently again.
    m.playing = name;
    m.playing_time = time;

    if (!quiet) {
        TL_LOG_INFO("applied animation '%s' at %.3fs to %zu of %zu entit%s", name.c_str(),
                    static_cast<double>(time), applied, m.animated().size(),
                    m.animated().size() == 1 ? "y" : "ies");
    }
    return true;
}

bool Renderer::apply_animation(const std::string& name, float time) {
    return impl_->apply_subject_animation(name, time, /*quiet=*/false);
}

// PORT: the layout maths in OGRE's types (core/layout_maths.h).
Ogre::Quaternion orientation_from_basis(const Ogre::Vector3& forward, const Ogre::Vector3& right,
                                        const Ogre::Vector3& up) {
    return to_ogre(
        tl::core::orientation_from_basis(from_ogre(forward), from_ogre(right), from_ogre(up)));
}

// PORT: the layout maths in OGRE's types (core/layout_maths.h).
Ogre::Quaternion node_orientation(const tl::formats::CmpNode& node) {
    return to_ogre(tl::core::node_orientation(node));
}

// PORT: the layout maths in OGRE's types (core/layout_maths.h).
Ogre::Vector3 layout_scale(const tl::formats::CmpNode& node, float unit_scale) {
    return to_ogre(tl::core::layout_scale(node, unit_scale));
}

/// Load a layout's mesh: its submeshes' texture paths qualified by its own
/// directory, and its skeleton found. Null when OGRE cannot load it. The
/// material script is *not* parsed here - see place_layout(), which parses
/// every script in the layout before creating any entity.
Ogre::MeshPtr load_layout_mesh(const std::string& mesh_path) {
    Ogre::MeshPtr mesh;
    try {
        mesh = Ogre::MeshManager::getSingleton().load(mesh_path, kGameGroup);
    } catch (const Ogre::Exception& e) {
        TL_LOG_WARN("could not load '%s': %s", mesh_path.c_str(), e.getDescription().c_str());
        return Ogre::MeshPtr();
    }

    const std::string dir = dir_of_asset(mesh_path);
    auto& materials = Ogre::MaterialManager::getSingleton();
    for (const Ogre::SubMesh* sub : mesh->getSubMeshes()) {
        // A submesh's own material name is what its .MESH embeds, which is
        // not this entity's if that name lost a cross-directory collision -
        // see g_material_alias. Qualifying the *other* directory's material
        // by this one's path would be silently wrong rather than merely
        // useless, so the alias has to be resolved first.
        const auto alias = g_material_alias.find(dir + "\x1f" + sub->getMaterialName());
        const std::string& name =
            alias != g_material_alias.end() ? alias->second : sub->getMaterialName();
        qualify_texture_paths(materials.getByName(name, kGameGroup), dir, dir);
    }
    ensure_skeleton(mesh, mesh_path, dir);
    return mesh;
}

Ogre::Entity* Renderer::Impl::place_mesh(const MeshPlacement& placement) {
    const std::string& mesh_path = placement.mesh;
    const Ogre::MeshPtr mesh = load_layout_mesh(mesh_path);
    if (!mesh) return nullptr;
    const std::string dir = dir_of_asset(mesh_path);

    Ogre::Entity* entity = scene->createEntity(mesh);
    // A cached mesh hands its submeshes' *old* MaterialPtrs to a new entity.
    rebind_materials(entity);
    apply_qualified_materials(entity, dir);

    // Vertex colours on level geometry ARE colour: they are the surface's
    // diffuse reflectance, and they carry the night.
    //
    // This used to strip `diffuse vertexcolour` off every scenery pass, on the
    // reading that a mean vertex RGB of about (2, 0, 40) could not be lighting
    // because modulating by it would render the scene nearly black. That
    // reasoning treated the channel as a multiplier on the finished pixel. It is
    // not. The game's own shaders spell the model out -
    // the PC release's `programs/shaders.cg`, which is plain text:
    //
    //     colour = ambient * diffuse
    //            + diffuse * saturate(dot(lightDir, norm)) * lightDiffuseColour
    //            + emissive;
    //
    // `diffuse` there is the *surface* diffuse, so the vertex colour modulates
    // only the directional term. The ambient term is the pass's own `ambient`,
    // which every scenery material declares as 0.588 and which the level rules
    // override with `MATERIAL AMBIENT`. Nothing goes black.
    //
    // And the channel is not grey. Read as the ABGR the vertex declaration
    // actually names, the terrain's constant is (R 2, G 0, B 40) - a deep blue.
    // Through the warm directional (238, 219, 155) at 1.9 that lands on a light
    // of (0.232, 0.219, 0.389): green *below* red and blue well above, which is
    // the cool cast the original's ground has. Measured against the reference
    // screenshot the ground wants a green:red ratio of 0.948 and this gives
    // 0.944, where stripping the channel gave 1.244 - wrong in direction, not
    // just in size.
    //
    // So the material script is now honoured as written. A material that wants a
    // literal instead says so - `town_grass_animated_mat` declares
    // `diffuse 0.588 0.588 0.588` - and OGRE's own parser has already set the
    // tracking mode either way, which is why there is nothing to do here.

    // Order the opaque billboards by the file's own order - and *only* the
    // opaque ones.
    //
    // The backdrop, sky and terrain are near-coplanar painted planes, so their
    // depths tie and OGRE orders them by a pass hash derived from allocation
    // addresses, which differs between runs. Giving them an increasing priority
    // fixes that.
    //
    // Doing the same to transparent geometry is a mistake that looks like a
    // texture problem. OGRE renders each priority group as solids first, then
    // transparents; so a higher-priority *solid* is drawn after a lower-priority
    // *transparent*, and the terrain painted over the foliage's blended edges.
    // The symptom was a hard aliased fringe around every leaf and bush, with the
    // background showing through - easily mistaken for an alpha or mipmap fault
    // in the texture, which is where I looked first.
    //
    // So transparent meshes keep the default priority and stay in OGRE's normal
    // back-to-front pass at the end.
    // A pass that blends a texture with no alpha in it is solid in everything
    // but its place in the frame. The exporter wrote `scene_blend alpha_blend`
    // on most of the level sets' materials, cliffs and towers on opaque DXT1
    // included, so they sorted by distance with the truly blended props - and
    // a tower whose centre is nearer the camera than a chain's was drawn after
    // the chain, behind texels that had already written depth over the sky:
    // every link of the lava levels' chains wore an outline of the lava sea.
    // Drawn solid such a pass looks the same and comes before every blend.
    // Left alone: a pass that does not write depth, one whose diffuse alpha is
    // below 1 or follows the vertex colours, and any texture whose alpha is
    // not 255 throughout (`formats::dds_alpha`: the format alone says "alpha"
    // for every DXT3 and DXT5 the level sets ship).
    //
    // **And a cut-out is drawn after the other blends that write depth.** OGRE
    // queues a blended pass that writes and checks depth with the solids
    // (`RenderPriorityGroup::addRenderable`), where passes are ordered by a
    // hash and not by distance. The walls on `tileset_filler` and a chain are
    // both such passes, so which came first was the hash's choice: on the
    // desktop the wall, on the console the chain - whose filtered edge texels
    // then blended with the sky and kept the wall behind them from being
    // drawn. A mesh whose texture is cut away over a real share of its area
    // (`alpha_rejection` throws out 9.6% of `chain.dds`, 1.2% of the wall's)
    // takes the next priority up, after every wall.
    constexpr float kCutOutClearShare = 0.05f;
    bool transparent = false;
    bool cut_out = false;
    for (unsigned i = 0; i < entity->getNumSubEntities(); ++i) {
        const Ogre::MaterialPtr& mat = entity->getSubEntity(i)->getMaterial();
        if (!mat) continue;
        for (Ogre::Technique* tech : mat->getTechniques()) {
            for (Ogre::Pass* pass : tech->getPasses()) {
                if (!pass->isTransparent()) continue;
                bool solid = pass->getDepthWriteEnabled() &&
                             pass->getSourceBlendFactor() == Ogre::SBF_SOURCE_ALPHA &&
                             pass->getDestBlendFactor() == Ogre::SBF_ONE_MINUS_SOURCE_ALPHA &&
                             !(pass->getDiffuse().a < 1.0f) &&
                             (pass->getVertexColourTracking() & Ogre::TVC_DIFFUSE) == 0 &&
                             pass->getNumTextureUnitStates() != 0;
                float holes = 0.0f;
                for (Ogre::TextureUnitState* unit : pass->getTextureUnitStates()) {
                    const TextureAlpha& alpha = texture_alpha(unit);
                    if (!alpha.opaque) solid = false;
                    holes = std::max(holes, alpha.clear_share);
                }
                if (solid) {
                    pass->setSceneBlending(Ogre::SBT_REPLACE);
                    ++blends_drawn_solid;
                } else {
                    transparent = true;
                    if (pass->getDepthWriteEnabled() && pass->getDepthCheckEnabled() &&
                        pass->getAlphaRejectFunction() == Ogre::CMPF_GREATER &&
                        kCutOutClearShare <= holes) {
                        cut_out = true;
                    }
                }
            }
        }
    }
    if (cut_out) {
        entity->setRenderQueueGroupAndPriority(Ogre::RENDER_QUEUE_MAIN,
                                               OGRE_RENDERABLE_DEFAULT_PRIORITY + 1);
        ++cut_outs_drawn_last;
    }
    if (!transparent) {
        entity->setRenderQueueGroupAndPriority(Ogre::RENDER_QUEUE_MAIN,
                                               static_cast<Ogre::ushort>(layout_draw_order));
        if (layout_draw_order < kLayoutDrawOrderCeiling) {
            paint_ordered.insert(entity);
            ++layout_draw_order;
        }
    }
    // A `PRERENDER` piece is drawn before the level (`stgeompre_`, queue 0x30)
    // and a `POSTRENDER` one after it (`stgeompost_`, 0x59): `CLevel::bake`.
    // The town menu's `menu_billboard_sky` wind
    // streaks are mostly clear and write depth: drawn among the ground pieces,
    // they kept the path and the grass behind them from being drawn, and the
    // backdrop showed through.
    if (placement.render_phase != 0) {
        entity->setRenderQueueGroup(
            static_cast<Ogre::uint8>(placement.render_phase < 0 ? 0x30u : runic_queue(0x59)));
    }

    // Transparent foliage is deliberately left alone. Turning its sort off was
    // tried, to make the draw order the file's: it made reproducibility worse,
    // 2.8% of pixels varying becoming 6.2%, because an unsorted transparent
    // collection is grouped by `Pass*` and pointer order is more
    // address-dependent than the distance sort it replaced.

    Ogre::SceneNode* node = scene->getRootSceneNode()->createChildSceneNode();
    node->setPosition(to_ogre(placement.position));
    node->setOrientation(to_ogre(placement.orientation));
    node->setScale(to_ogre(placement.scale));
    node->attachObject(entity);
    // `VISIBLE:false` at placement - see MeshPlacement::start_hidden. Set on
    // the node rather than left for a caller to notice, so nothing that
    // places a mesh can forget it.
    if (placement.start_hidden) node->setVisible(false);

    // A layout `Light`: on the light map's first group (0x5f, which the view
    // never draws) and its visibility flag, once per density - `CLevel::bake`
    // adds it to `stgeomLight_` that many
    // times. Not scenery: no ground, no merge, no script handle.
    // A main-menu backdrop's lights are never built into the batch the light
    // map draws (loading_menu_level): placed, and drawn nowhere.
    if (placement.light_density > 0 && loading_menu_level) {
        entity->setVisible(false);
        return entity;
    }
    if (placement.light_density > 0) {
        entity->setRenderQueueGroup(static_cast<Ogre::uint8>(kLightMapGroupFirst));
        entity->setVisibilityFlags(kVisibilityLightMap);
        light_pool_meshes.push_back(entity);
        for (unsigned copy = 1; copy < placement.light_density; ++copy) {
            Ogre::Entity* again = scene->createEntity(mesh);
            rebind_materials(again);
            apply_qualified_materials(again, dir);
            again->setRenderQueueGroup(static_cast<Ogre::uint8>(kLightMapGroupFirst));
            again->setVisibilityFlags(kVisibilityLightMap);
            node->attachObject(again);
            light_pool_meshes.push_back(again);
        }
        return entity;
    }

    if (placement.room_piece) room_piece_meshes.push_back(entity);
    if (placement.room_piece && placement.bake) {
        bakeable_room_pieces.push_back({entity, placement.render_phase});
    }
    layout_meshes.push_back(entity);
    if (placement.is_unit) unit_meshes.insert(entity);

    // Remembered by layout-node id so the level script can reach it. More than
    // one mesh can come from one node - a room piece brings its collision twin,
    // and a `Monster` could in principle be several - so this is a multimap
    // rather than a map, and `Hide` on that node hides all of them.
    if (placement.node_id != 0) {
        // Into the nested layout's own map when one is being placed; see
        // NestedScript. Its ids are its file's, and another instance of the
        // same file uses the very same ones.
        (node_sink != nullptr ? *node_sink : scripted_nodes).emplace(placement.node_id, node);
    }
    return entity;
}

/// Whether a texture's alpha leaves it opaque, and how much of it is clear: read
/// once a texture from its `.dds` blocks (place_mesh()'s blend decisions).
const Renderer::Impl::TextureAlpha& Renderer::Impl::texture_alpha(Ogre::TextureUnitState* unit) {
    static const TextureAlpha kNone{};
    const std::string& name = unit->getTextureName();
    if (name.empty()) return kNone;
    const auto known = texture_alphas.find(name);
    if (known != texture_alphas.end()) return known->second;
    TextureAlpha read;
    tl::formats::DdsAlphaCensus census;
    try {
        Ogre::DataStreamPtr stream = Ogre::ResourceGroupManager::getSingleton().openResource(
            name, kGameGroup, nullptr, false);
        if (stream) {
            Ogre::MemoryDataStream bytes(stream);
            census = tl::formats::dds_alpha_census(bytes.getPtr(), bytes.size());
        }
    } catch (const Ogre::Exception&) {
        census = tl::formats::DdsAlphaCensus{};
    }
    if (census.kind == tl::formats::DdsAlpha::Unknown) {
        // Not a `.dds` the scan reads: what the loaded texture's format says.
        const Ogre::TexturePtr& texture = unit->_getTexturePtr();
        read.opaque = texture && texture->isLoaded() && !texture->hasAlpha();
    } else {
        read.opaque = census.kind != tl::formats::DdsAlpha::Varies;
        read.clear_share = census.clear_share();
    }
    return texture_alphas.emplace(name, read).first->second;
}

// CLevel::bake, its room pieces: every
// `Room Piece` of the level that is bakeable (`BAKE`, the piece's +0x14f,
// true unless the layout says otherwise), shown (+0x6d) and has a model goes
// into one of three `StaticGeometry` sets and its own model is hidden:
//
//   * `stgeompre_` on queue 0x30, a `PRERENDER` piece (+0x14e);
//   * `stgeompost_` on 0x59, a `POSTRENDER` one (+0x14d);
//   * `stgeom_` on 0x31, every other.
//
// The console sets nothing else on them: OGRE's default region (1000 units, so
// a level is a region or a few, one batch per material in each) and no
// rendering distance. The Linux build's `setRegionDimensions` (the level's
// template, 50 by default) and `setRenderingDistance` (48, or 30 in netbook
// mode) are the PC's alone. Nor does it ask whether the piece is animated or
// blends (Linux's `getRoomPieceIsAnimated` test is not on the console): a
// blended piece is batched like the rest and sorts with its region.
//
// The level's `Light`s go to its fourth set, `stgeomLight_` on 0x5f, that
// many times over; place_mesh() keeps those as copies on the light map's group
// instead (see `MeshPlacement::light_density`).
//
// The baked pieces keep their entities, hidden: ground_under() and
// apply_scenery_fill() read them, and StaticGeometry takes the materials the
// sub-entities carry, so every pass edit made to them still applies.
void Renderer::Impl::bake_static_scenery() {
    if (!scene || static_scenery != nullptr) return;

    struct Set {
        const char* name;
        unsigned queue;
        Ogre::StaticGeometry* geometry = nullptr;
        size_t pieces = 0;
    };
    Set sets[3] = {{"stgeompre_", 0x30u}, {"stgeom_", 0x31u}, {"stgeompost_", runic_queue(0x59)}};
    for (Set& set : sets) {
        set.geometry =
            scene->createStaticGeometry(std::string(set.name) + std::to_string(++baked_sets));
        set.geometry->setRenderQueueGroup(static_cast<Ogre::uint8>(set.queue));
    }

    size_t triangles = 0;
    for (const BakeablePiece& piece : bakeable_room_pieces) {
        Ogre::Entity* entity = piece.entity;
        Ogre::SceneNode* node = entity->getParentSceneNode();
        if (node == nullptr || !entity->getVisible()) continue;
        Set& set = sets[piece.render_phase < 0 ? 0 : piece.render_phase > 0 ? 2 : 1];
        set.geometry->addEntity(entity, node->_getDerivedPosition(), node->_getDerivedOrientation(),
                                node->_getDerivedScale());
        ++set.pieces;
        for (const Ogre::SubMesh* sub : entity->getMesh()->getSubMeshes()) {
            if (sub->indexData) triangles += sub->indexData->indexCount / 3;
        }
        entity->setVisible(false);
        // PORT: and out of the scene graph. Hidden, it was still visited by every
        // camera every frame (OGRE's default scene manager tests each node against
        // the frustum, then each object): on the console's Mines floor that was a
        // third of the game's thread, for nearly 4,000 pieces the batch draws
        // anyway. The node keeps its transform and the entity, which is all
        // ground_under() and apply_scenery_fill() read.
        if (Ogre::SceneNode* parent = node->getParentSceneNode()) parent->removeChild(node);
        tl::core::keep_responsive();
    }
    size_t regions = 0;
    for (Set& set : sets) {
        set.geometry->build();
        regions += set.geometry->getRegions().size();
        tl::core::keep_responsive();
    }
    static_scenery = sets[1].geometry;

    TL_LOG_INFO(
        "static scenery: %zu / %zu / %zu room piece(s) baked into stgeompre_ / stgeom_ / "
        "stgeompost_, %zu region(s), %zu triangles; %zu bakeable of %zu room pieces",
        sets[0].pieces, sets[1].pieces, sets[2].pieces, regions, triangles,
        bakeable_room_pieces.size(), room_piece_meshes.size());
}

/// Link an animation onto something the *layout* placed, and start it running.
///
/// Torchlight keeps one `.SKELETON` per animation beside the mesh, and a layout
/// node names the one it wants by stem: a `Monster` says `ANIMATION: Idle` and
/// `Idle.SKELETON` sits next to `Ratman.MESH`. That is the same arrangement
/// Renderer::link_animation() handles for the subject, and this is that
/// generalised from "the subject" to any entity - which is the second half of
/// what the town needed. The first half is advance() stepping it.
///
/// **`bind` is not an animation and must not be looked for.** 205 of the
/// archive's monsters author it and there is no `bind.SKELETON` anywhere; it
/// names the mesh's own bind pose, which is what an entity already shows. It is
/// not, however, an instruction to stand still - see the note on the idle
/// fallback in place_layout(), which is what makes the town's merchants breathe.
///
/// The state is found by *difference* rather than by name. A skeleton file names
/// its animation internally and there is no rule that the name matches the
/// filename - `good_zealot/idle.SKELETON` and `Sage/Special_Focus_Cast.SKELETON`
/// are not obliged to agree with their stems, and a name lookup that missed
/// would fail silently as a character that simply never moves. Taking whatever
/// state the link *added* cannot miss.
/// The first of `wanted` that names a state on this set, matched without case.
///
/// The two spellings do not have to agree and in the town they do not:
/// `RalinAnimation` asks for `idle` while `Idle.SKELETON` names its animation
/// `Idle`. And a caller asks for an animation *class* - `Attack`, `Die` - which
/// archive_animation_in_dir() resolves to whichever numbered or suffixed file
/// the mesh ships, so the Ratman's `Attack` arrives as `Attack1`; the source
/// file's own animation names are therefore candidates alongside the requested
/// one.
// PORT: an OGRE animation state found by any of several names.
Ogre::AnimationState* state_named_any_of(Ogre::AnimationStateSet* states,
                                         const std::vector<std::string>& wanted) {
    if (states == nullptr) return nullptr;
    for (const std::string& want : wanted) {
        for (const auto& entry : states->getAnimationStates()) {
            if (entry.first.size() != want.size()) continue;
            if (std::equal(want.begin(), want.end(), entry.first.begin(), [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                })) {
                return entry.second;
            }
        }
    }
    return nullptr;
}

unsigned Renderer::Impl::entity_track(EntityAnimation& model, Ogre::AnimationState* state) {
    for (size_t i = 0; i < model.states.size(); ++i) {
        if (model.states[i] == state) return static_cast<unsigned>(i);
    }
    const unsigned track =
        model.animator.add_animation(state->getAnimationName(), state->getLength());
    model.states.push_back(state);
    tl::core::ModelTrack seen;
    seen.length = state->getLength();
    seen.time = state->getTimePosition();
    seen.weight = state->getWeight();
    seen.enabled = state->getEnabled();
    seen.loop = state->getLoop();
    model.written.push_back(seen);
    // The animator starts from the state as it stands.
    model.animator.tracks()[track] = seen;
    return track;
}

void Renderer::Impl::write_entity_animation(EntityAnimation& model) {
    for (size_t i = 0; i < model.states.size(); ++i) {
        const tl::core::ModelTrack& now = model.animator.tracks()[i];
        tl::core::ModelTrack& was = model.written[i];
        Ogre::AnimationState* state = model.states[i];
        // Never loop something with no duration: `setTimePosition` wraps a
        // looping state with `fmod(t, length)`, and that is NaN for 0.
        if (now.loop != was.loop) state->setLoop(now.loop && state->getLength() > 0.0f);
        if (now.time != was.time) state->setTimePosition(now.time);
        if (now.weight != was.weight) state->setWeight(now.weight);
        if (now.enabled != was.enabled) state->setEnabled(now.enabled);
        was = now;
    }
}

bool Renderer::Impl::play_scene_animation(Ogre::Entity* entity, const std::string& mesh_path,
                                          const std::string& animation, bool loops, float rate,
                                          bool optional, Ogre::AnimationState** started, bool armed,
                                          std::string* played, float blend, bool queue) {
    if (!entity || names_the_bind_pose(animation)) return false;
    if (played != nullptr) *played = animation;

    if (!entity->hasSkeleton()) {
        if (!optional) {
            TL_LOG_WARN("  '%s' asks for animation '%s' but carries no skeleton", mesh_path.c_str(),
                        animation.c_str());
        }
        return false;
    }

    const std::string dir = dir_of_asset(mesh_path);
    std::string path = archive_path_in_dir(animation + ".SKELETON", dir);
    if (path.empty()) {
        // Not under that exact name. A layout, and this engine, name an
        // animation by its *state* - `Idle`, `Die`, `Run` - and a mesh is free
        // to ship it as `Idle_Armed` or `Die_Armed2`; see
        // archive_animation_in_dir(). Without this the skeleton warriors stood
        // in their bind pose and vanished instead of falling over, because
        // every one of their files carries the suffix.
        path = archive_animation_in_dir(dir, animation, armed, 0);
        if (!path.empty()) {
            TL_LOG_INFO("  '%s' plays '%s' as '%s'", mesh_path.c_str(), animation.c_str(),
                        path.c_str());
        }
    }
    // **A rig that has no RUN walks, and one that has no WALK runs**, and the
    // fallback is symmetric and explicit in the original.
    // `CCharacter::updateAnimation`
    // picks the locomotion animation twice over, once for each gait: wanting
    // RUN it calls `CGenericModel::animationExists("RUN")` and,
    // when that is false, jumps to the WALK block; wanting WALK it
    // calls `animationExists("WALK")` and, when *that* is false,
    // falls through to the RUN block.
    //
    // Seven model directories in the archive ship one gait and not the other -
    // `ratmanforeman`, the four elemental wisps, `wisp` and `invisible` - and
    // fourteen ship RUN with no WALK, the player classes among them. The
    // Ratlin Foreman is what this was found on: the hunt AI asks for `Run`,
    // `media/models/RatmanForeman/` holds only `Walk.SKELETON`, the request
    // failed quietly (it is `optional`), and the unit therefore kept whatever
    // it was playing - which, once it had swung at anything, was a *looping*
    // `Attack1`. It walked the level swinging a hammer at nothing, which is
    // exactly what the log shows: 12 `play 'Attack'`, 1 blow landed, and 15
    // `play 'Run' on 0 Ratlin Foreman entities` between them.
    //
    // Its `WALKINGSPEED` and `RUNNINGSPEED` are both authored 1.0, so the
    // substitution is not a compromise: one gait is all that unit has.
    std::string resolved = animation;
    if (path.empty()) {
        const std::string want = lowered(animation);
        const char* other = want == "run" ? "Walk" : (want == "walk" ? "Run" : nullptr);
        if (other != nullptr) {
            path = archive_path_in_dir(std::string(other) + ".SKELETON", dir);
            if (path.empty()) path = archive_animation_in_dir(dir, other, armed, 0);
            if (!path.empty()) {
                resolved = other;
                if (played != nullptr) *played = resolved;
                TL_LOG_INFO("  '%s' has no '%s'; plays '%s' as '%s'", mesh_path.c_str(),
                            animation.c_str(), other, path.c_str());
            }
        }
    }
    if (path.empty()) {
        // `optional` is the engine's default idle rather than something the
        // layout asked for by name, so a mesh that ships none is an ordinary
        // fact about that mesh and not a fault worth a warning.
        if (!optional) {
            TL_LOG_WARN("  no '%s.SKELETON' beside '%s'", animation.c_str(), mesh_path.c_str());
        }
        return false;
    }

    // A pose rather than an animation, which the archive does not distinguish by
    // name or extension - only by whether the file holds any track. Applied
    // directly and not stepped: there is nothing to advance.
    Ogre::SkeletonPtr source;
    try {
        // SkeletonManager inherits ResourceManager::load(), which is declared to
        // return a ResourcePtr; the cast is what every OGRE caller does here.
        source = Ogre::static_pointer_cast<Ogre::Skeleton>(
            Ogre::SkeletonManager::getSingleton().load(path, kGameGroup));
    } catch (const Ogre::Exception& e) {
        TL_LOG_WARN("  could not load '%s': %s", path.c_str(), e.getDescription().c_str());
        return false;
    }
    bool timed_pose = false;
    if (queue && !skeleton_has_tracks(source)) return false;
    if (!skeleton_has_tracks(source)) {
        // Whatever the entity was playing stops here. A state left enabled
        // keeps applying its keys to bones the pose now holds manually, which
        // `Skeleton::reset()` skips - each frame's keys land on the last
        // frame's result, and a mimic posed `Asleep` over its looping `Idle`
        // sat there turning its lid.
        if (Ogre::AnimationStateSet* playing = entity->getAllAnimationStates()) {
            for (const auto& entry : playing->getAnimationStates()) {
                if (entry.second->getEnabled()) entry.second->setEnabled(false);
            }
        }
        // And the animator lets go of them (`clearAnimations`), so the next
        // frame does not step them on again.
        if (const auto held = entity_animations.find(entity); held != entity_animations.end()) {
            held->second.animator.clear();
            for (size_t i = 0; i < held->second.written.size(); ++i) {
                held->second.written[i] = held->second.animator.tracks()[i];
            }
        }
        const size_t posed = pose_from_skeleton(entity, source);
        if (posed == 0) {
            TL_LOG_WARN("  '%s' holds no animation track and no bone the mesh shares",
                        path.c_str());
            return false;
        }
        TL_LOG_INFO(
            "  posing %s from '%s' (%zu bone(s); the file holds a pose, not an "
            "animation)",
            mesh_path.c_str(), animation.c_str(), posed);
        // A pose with a length still runs its clock: the elemental wisps'
        // `die` is 1.27 s over no track, and its `.ANIMATION` keys - the
        // impact burst at frame 0, `FADEOUTMESH` - fire on that time as on any
        // other animation's (`CCharacter::updateAnimation`
        // reads the keys against the playing
        // animation's time). `Dead1` is 0 s long and stays a pose alone.
        float length = 0.0f;
        for (unsigned short i = 0; i < source->getNumAnimations(); ++i) {
            length = std::max(length, source->getAnimation(i)->getLength());
        }
        if (!(length > 0.0f)) return true;
        timed_pose = true;
    }

    // A pose an earlier file left (`pose_from_skeleton`) holds its bones
    // manually, and `Skeleton::reset()` skips a manually controlled bone - so an
    // animation played over it adds each frame's keys onto the last frame's
    // result instead of the rest pose, and a chest's lid, posed `closed` and then
    // played `opening`, kept turning. The bones go back to the animation.
    if (entity->hasSkeleton() && !timed_pose) {
        for (Ogre::Bone* bone : entity->getSkeleton()->getBones()) {
            if (!bone->isManuallyControlled()) continue;
            bone->setManuallyControlled(false);
            bone->reset();
        }
    }

    std::set<std::string> before;
    if (const Ogre::AnimationStateSet* states = entity->getAllAnimationStates()) {
        for (const auto& entry : states->getAnimationStates()) before.insert(entry.first);
    }

    if (!link_path_onto(entity, path)) return false;

    Ogre::AnimationStateSet* states = entity->getAllAnimationStates();
    if (!states) return false;

    // Which state this file actually brought in.
    //
    // **By name first, and only then by what appeared.** The set difference
    // below used to come first, on the reasoning that the first copy of a
    // monster is the one that links the file and so the one new state must be
    // it. That is true only while one entity at a time is linking: OGRE keeps
    // linked-skeleton sources on the **shared master skeleton resource**, not
    // on the instance (see the note in the fallback), so an animation linked by
    // *another* entity of the same mesh turns up in this entity's state set the
    // next time it is refreshed - and then it is "new" here too, and the
    // difference picks it.
    //
    // Three Varkolyn coming out of one chute is exactly that case, and it is
    // what the shaft ambush produced: `Varkolyn_Chuteb` links
    // `Mine_ChuteA_Spawn2`, and a moment later the entity that had linked
    // `Mine_ChuteA_Spawn1` asks for `Idle`, resolves the path to
    // `Idle.SKELETON` correctly, and is handed `Mine_ChuteA_Spawn1` because
    // that is what was new in its set. The unit then played its entrance on a
    // loop instead of going back to standing.
    //
    // The candidate list is the requested name plus the source file's own
    // animation names, which is what `_mergeSkeletonAnimations()` merges under
    // - so it names the right state whoever linked it and in whatever order.
    std::vector<std::string> wanted_names{resolved};
    for (unsigned short i = 0; i < source->getNumAnimations(); ++i) {
        wanted_names.push_back(source->getAnimation(i)->getName());
    }
    Ogre::AnimationState* added = state_named_any_of(states, wanted_names);
    if (!added) {
        for (const auto& entry : states->getAnimationStates()) {
            if (before.count(entry.first) == 0) {
                added = entry.second;
                break;
            }
        }
    }
    if (!added) {
        // Already linked, which is the ordinary case for the second and later
        // copies of a monster: the town places four Varkolyn corpses from one
        // mesh and one animation, and OGRE's linked-skeleton sources live on the
        // shared master skeleton resource rather than on the instance, so the
        // second entity's link adds nothing new and its state is simply already
        // there.
        //
        // Matched case-insensitively, because the two spellings do not have to
        // agree and in the town they do not: `RalinAnimation` asks for `idle`
        // while `Idle.SKELETON` names its animation `Idle`, so an exact lookup
        // left two of the three Ratlins standing still while the first - which
        // took the "newly added" path and never consulted the name - moved.
        //
        // Matched against **the source file's own animation names** as well as
        // the requested one, because the two are routinely different. A caller
        // asks for an animation *class* - `Attack`, `Die`, `Idle` -
        // and archive_animation_in_dir() resolves that to whichever numbered or
        // suffixed file the mesh actually ships; `_mergeSkeletonAnimations()`
        // then copies the animations in under the names the *file* gives them.
        // So the Ratman's `Attack` resolves to `Attack1.SKELETON`, which merges
        // an animation called `Attack1`, and looking only for `Attack` finds
        // nothing. The first copy of the monster was unaffected - it takes the
        // "newly added" path above and never consults a name - so every Ratman
        // after the first stood still through the fight.
        // Asked and answered above: state_named_any_of() runs over exactly
        // these candidates before the set difference is consulted at all.
    }
    if (!added) {
        TL_LOG_WARN("  linked '%s' onto '%s' but it added no animation state", path.c_str(),
                    mesh_path.c_str());
        return false;
    }

    // The entity's `CGenericModel` animation half (core/model_animation.h):
    // `playAnimation` replaces whatever played, `blendAnimation` fades it in
    // over `blend` seconds while the old one fades out under it, at the
    // weights `updateAnimation` works out each frame. States switched on by a
    // path that does not go through it (a pose, the bestiary) are not its to
    // fade, and go off as before.
    EntityAnimation& model = entity_animations[entity];
    if (queue) {
        // Behind what plays; nothing is switched off and the rate of what
        // plays now stays as it is.
        const unsigned track = entity_track(model, added);
        model.animator.queue_blend(static_cast<int>(track), loops, blend, rate);
        write_entity_animation(model);
        if (started != nullptr) *started = added;
        TL_LOG_INFO("  queueing '%s' on %s behind what plays (blend %.2fs)",
                    added->getAnimationName().c_str(), mesh_path.c_str(),
                    static_cast<double>(blend));
        return true;
    }
    for (const auto& entry : states->getAnimationStates()) {
        if (entry.second != added && entry.second->getEnabled() &&
            std::find(model.states.begin(), model.states.end(), entry.second) ==
                model.states.end()) {
            entry.second->setEnabled(false);
        }
    }
    const unsigned track = entity_track(model, added);
    if (blend > 0.0f) {
        model.animator.blend(track, loops, blend, rate);
    } else {
        model.animator.play(track, loops, rate);
    }
    // What the state really is now, so the one started goes on even where
    // another path switched it off since it was last written.
    model.written[track].enabled = added->getEnabled();
    model.written[track].time = added->getTimePosition();
    // At the record's time from now, not from the next update: the callers time
    // the animation's end off the state, and a state replayed from its end
    // would read as already over.
    model.animator.tracks()[track].set_time(model.animator.active().front().time);
    write_entity_animation(model);
    if (started != nullptr) *started = added;

    // One entry per entity, replaced rather than appended. advance() steps every
    // enabled state of every entry, so an entity listed twice has its animation
    // advanced twice per frame - and the script starts and stops animations on
    // the same unit repeatedly through a cutscene, so the seer's collapse ran at
    // several times speed by the time the scene reached it.
    auto existing = std::find_if(scene_animations.begin(), scene_animations.end(),
                                 [entity](const SceneAnimation& a) { return a.entity == entity; });
    if (existing != scene_animations.end()) {
        existing->rate = rate;
    } else {
        scene_animations.push_back(SceneAnimation{entity, rate});
    }
    TL_LOG_INFO("  animating %s with '%s' (%.2fs%s, rate %.2f)", mesh_path.c_str(),
                added->getAnimationName().c_str(), static_cast<double>(added->getLength()),
                loops ? ", looping" : ", once", static_cast<double>(rate));
    return true;
}

namespace renderer_detail {
namespace {
bool g_caching_particles = false;
}  // namespace

bool caching_particles() { return g_caching_particles; }

// `CLayout::setCacheingParticlesForLevel`: `g_bCachParticles = 1`,
// whatever it is passed.
void set_caching_particles() { g_caching_particles = true; }

/// Start a placed system over: `CParticle::Start`,
/// its clock back to zero.
void restart_particle_system(ParticleSystem& system) {
    system.elapsed = 0.0f;
    system.finished = false;
    system.pu_dying = false;
    start_torchlight_particle(system.pu);
}

/// `CParticle::Stop(false)`: every emitter
/// stops releasing now; what is alive lives out its life, then the system
/// stops.
void stop_particle_emission(ParticleSystem& system) {
    if (system.finished) return;
    stop_torchlight_particle_emission(system.pu);
    system.pu_dying = true;
}

}  // namespace renderer_detail
/// Place one `Particle` node: `CParticleWrapper::createParticleSystem`
/// through the glue, drawn by Torchlight's
/// ParticleUniverse fork.
bool Renderer::Impl::place_particle(const tl::formats::CmpScene& layout,
                                    const tl::formats::CmpNode& node, const Ogre::Vector3& position,
                                    const Ogre::Quaternion& orientation) {
    using namespace tl::formats;

    if (const CmpField* visible = node.field(cmp_field::particle::kVisible)) {
        if (visible->count() == 1 && !visible->as_bool()) return false;
    }

    if (!particle_universe.running() || !particle_universe_takes(placing_layout_path)) return false;
    std::string missing;
    if (!can_build_torchlight_particle(layout, node, &missing)) {
        TL_LOG_WARN("  particle '%s': %s is not in the ParticleUniverse fork; not drawn",
                    node.name.c_str(), missing.c_str());
        return false;
    }
    return place_torchlight_particle(layout, node, position, orientation);
}

void Renderer::Impl::arm_particle_timer(size_t first, size_t last) {
    float timeout = 0.0f;
    float window = 0.0f;
    bool never = false;
    for (size_t i = first; i < last && i < particle_systems.size(); ++i) {
        const ParticleSystem& system = particle_systems[i];
        if (system.timer_armed) continue;
        timeout = std::max(timeout, system.fixed_timeout);
        window = std::max(window, system.finite_window);
        never = never || system.non_expiring;
    }
    float stop_at = timeout > 0.0f ? timeout : window;
    if (never) stop_at = 0.0f;
    if (stop_at > 0.0f) stop_at += 0.1f;
    for (size_t i = first; i < last && i < particle_systems.size(); ++i) {
        if (particle_systems[i].timer_armed) continue;
        particle_systems[i].stop_at = stop_at;
        particle_systems[i].timer_armed = true;
    }
}

bool Renderer::Impl::place_torchlight_particle(const tl::formats::CmpScene& layout,
                                               const tl::formats::CmpNode& node,
                                               const Ogre::Vector3& position,
                                               const Ogre::Quaternion& orientation) {
    ParticleBuildContext context;
    context.scene = scene;
    context.camera = camera;
    context.resource_group = kGameGroup;
    context.resolve_texture = [](const std::string& reference) {
        return renderer_detail::resolve_archive_path(reference);
    };
    // The entity renderer's mesh, loaded as a layout's is, its own material
    // script first (a particle layout places no meshes, so place_layout() has
    // parsed none for it).
    context.load_mesh = [](const std::string& reference) {
        const std::string path = renderer_detail::resolve_archive_path(reference);
        if (path.empty()) {
            TL_LOG_WARN("particle mesh '%s' is not in the archive", reference.c_str());
            return std::string();
        }
        parse_sibling_material(path);
        const Ogre::MeshPtr mesh = load_layout_mesh(path);
        return mesh ? mesh->getName() : std::string();
    };
    // The original's queues are 90 + `SORT INDEX` (+ 5 for a light); this port
    // draws particles from `kParticleGroupBase` with the same offsets, after the
    // characters' group and below the overlay ("IS LIGHT").
    context.map_render_queue = [](unsigned queue) {
        const unsigned offset = queue >= 0x5a ? queue - 0x5a : 0u;
        return std::min<unsigned>(kParticleGroupBase + offset, 90);
    };

    ParticleSystem system;
    if (!build_torchlight_particle(context, layout, node, position, orientation, system.pu)) {
        return false;
    }
    // An `IS LIGHT` particle is drawn into the light map, and the view refuses
    // its groups (`CGame::renderableQueued`, ViewRenderables). The Xbox
    // build does this: a captured town frame draws the lanterns' `candleglow`
    // discs, a `glowcyan` and a sparkle
    // into the light map (target 208) and none of them in the view.
    // How the console's light-map pass admits them is not
    // settled: on Linux it sets the scene's mask to 2 (`CGameClient::
    // preRenderTargetUpdate`), which a system with the default flag 1
    // (`CGame::chooseSceneManager`, stores it too) would fail.
    if (system.pu.is_light && system.pu.movable != nullptr) {
        system.pu.movable->addVisibilityFlags(kVisibilityLightMap);
    } else {
        hold_particle_with_the_world(system);
    }
    system.owner = particle_owner;
    system.node = node.id;
    system.node_owner = node_sink != nullptr ? node_sink : &scripted_nodes;
    system.wrapper = placing_under_link && !placing_particle_file;
    system.fixed_timeout = system.pu.fixed_timeout;
    system.finite_window = system.pu.finite_window;
    system.non_expiring = system.pu.non_expiring;
    // `CParticle::Start`: a placed particle
    // plays from the start. A wrapper waits for its node's `Start Particle`.
    if (!system.wrapper) start_torchlight_particle(system.pu);
    TL_LOG_INFO("  particle '%s': ParticleUniverse (Torchlight's fork) at (%.2f, %.2f, %.2f)%s%s",
                node.name.c_str(), static_cast<double>(position.x), static_cast<double>(position.y),
                static_cast<double>(position.z), system.pu.non_expiring ? ", does not expire" : "",
                system.wrapper ? ", a wrapper of its layout: not started" : "");
    particle_systems.push_back(std::move(system));
    particle_index_add(particle_systems.size() - 1);
    return true;
}

// PORT: drawing, a system drawn only by the view hung under one node that says so
// (`SceneNode::setSubtreeVisibilityFlags`): the light map and shadow map passes skip it whole.
void Renderer::Impl::hold_particle_with_the_world(const ParticleSystem& system) {
    Ogre::SceneNode* holder = system.holder();
    Ogre::SceneNode* root = scene->getRootSceneNode();
    if (holder == nullptr || system.pu.movable == nullptr || holder->getParent() != root ||
        (system.pu.movable->getVisibilityFlags() & ~kVisibilityWorld) != 0) {
        return;
    }
    if (particle_holder_node == nullptr) {
        particle_holder_node = root->createChildSceneNode();
        particle_holder_node->setSubtreeVisibilityFlags(kVisibilityWorld);
    }
    root->removeChild(holder);
    particle_holder_node->addChild(holder);
}

void Renderer::Impl::step_torchlight_particle(ParticleSystem& system, float delta) {
    // `CParticle::updateLevelObject`: only
    // while the layout's timer runs or its particles are dying out. When the
    // timer is due - at once for a particle stopped without one - and nothing
    // is left alive of a dying one, `Stop(true)`.
    if (system.finished) return;
    const bool timed = system.stop_at > 0.0f;
    if (!timed && !system.pu_dying) return;
    system.elapsed += delta;
    const bool due = !timed || system.elapsed >= system.stop_at;
    if (due && (!system.pu_dying || !torchlight_particle_updating(system.pu))) {
        stop_torchlight_particle(system.pu);
        system.pu_dying = false;
        system.finished = true;
    }
}

void Renderer::Impl::step_particles(float delta) {
    forget_dead_cparticles();
    if (particle_systems.empty()) return;

    for (ParticleSystem& system : particle_systems) {
        if (system.pu.system == nullptr) continue;
        if (system.wrapper) {
            const bool was_playing = system.pu.playing;
            update_torchlight_wrapper(system.pu);
            if (was_playing && !system.pu.playing) {
                TL_LOG_INFO("script: particle wrapper %lld stops, its last particle gone",
                            static_cast<long long>(system.node));
            }
        } else {
            step_torchlight_particle(system, delta);
        }
    }
}

namespace renderer_detail {
Ogre::Vector3 default_camera_offset(float camera_mult) {
    const float distance = kDefaultCameraDistance * camera_mult;
    return Ogre::Vector3(0.5f * distance, 0.75f * distance, -0.5f * distance);
}

void for_each_triangle(
    const Ogre::MeshPtr& mesh, const Ogre::Matrix4& to_world,
    const std::function<void(const Ogre::Vector3&, const Ogre::Vector3&, const Ogre::Vector3&)>& fn,
    const std::function<bool(const std::string&)>& accept_material) {
    for (const Ogre::SubMesh* sub : mesh->getSubMeshes()) {
        if (accept_material && !accept_material(sub->getMaterialName())) continue;

        const Ogre::VertexData* vertex_data =
            sub->useSharedVertices ? mesh->sharedVertexData : sub->vertexData;
        if (!vertex_data || !sub->indexData) continue;

        const Ogre::VertexElement* pos_elem =
            vertex_data->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
        if (!pos_elem) continue;

        Ogre::HardwareVertexBufferSharedPtr vbuf =
            vertex_data->vertexBufferBinding->getBuffer(pos_elem->getSource());
        Ogre::HardwareIndexBufferSharedPtr ibuf = sub->indexData->indexBuffer;
        if (!vbuf || !ibuf) continue;

        std::vector<Ogre::Vector3> points(vertex_data->vertexCount);
        {
            Ogre::HardwareBufferLockGuard lock(vbuf, Ogre::HardwareBuffer::HBL_READ_ONLY);
            const unsigned char* base = static_cast<const unsigned char*>(lock.pData);
            for (size_t v = 0; v < vertex_data->vertexCount; ++v) {
                const float* p = nullptr;
                pos_elem->baseVertexPointerToElement(
                    const_cast<unsigned char*>(base + v * vbuf->getVertexSize()), &p);
                points[v] = to_world * Ogre::Vector3(p[0], p[1], p[2]);
            }
        }

        Ogre::HardwareBufferLockGuard lock(ibuf, Ogre::HardwareBuffer::HBL_READ_ONLY);
        const bool wide = ibuf->getType() == Ogre::HardwareIndexBuffer::IT_32BIT;
        const uint32_t* i32 = static_cast<const uint32_t*>(lock.pData);
        const uint16_t* i16 = static_cast<const uint16_t*>(lock.pData);
        const size_t triangles = sub->indexData->indexCount / 3;
        for (size_t t = 0; t < triangles; ++t) {
            const size_t a = wide ? i32[t * 3 + 0] : i16[t * 3 + 0];
            const size_t b = wide ? i32[t * 3 + 1] : i16[t * 3 + 1];
            const size_t c = wide ? i32[t * 3 + 2] : i16[t * 3 + 2];
            if (a >= points.size() || b >= points.size() || c >= points.size()) continue;
            fn(points[a], points[b], points[c]);
        }
    }
}

}  // namespace renderer_detail
bool Renderer::Impl::ground_under(const Ogre::Vector3& from, float& surface_y) const {
    // Straight down, against everything the layout placed. Both faces are
    // tested: a painted terrain billboard is a one-sided sheet and its winding
    // is whatever the exporter chose, so rejecting back faces loses hits.
    const Ogre::Ray ray(from, Ogre::Vector3::NEGATIVE_UNIT_Y);
    bool hit = false;
    Ogre::Real nearest = 0.0f;

    for (Ogre::Entity* entity : layout_meshes) {
        // Somebody standing there is not the ground under it.
        if (unit_meshes.count(entity) != 0) continue;
        // Derived, not cached: nothing has rendered yet when this runs, so the
        // cached world box is still null and every candidate would be skipped.
        const Ogre::AxisAlignedBox box = entity->getWorldBoundingBox(true);
        if (box.isNull()) continue;
        if (from.x < box.getMinimum().x || from.x > box.getMaximum().x) continue;
        if (from.z < box.getMinimum().z || from.z > box.getMaximum().z) continue;
        if (!entity->getParentSceneNode()) continue;

        const Ogre::Matrix4 to_world = entity->getParentSceneNode()->_getFullTransform();
        for_each_triangle(
            entity->getMesh(), to_world,
            [&](const Ogre::Vector3& a, const Ogre::Vector3& b, const Ogre::Vector3& c) {
                const std::pair<bool, Ogre::Real> r =
                    Ogre::Math::intersects(ray, a, b, c, true, true);
                if (!r.first) return;
                if (!hit || r.second < nearest) {
                    nearest = r.second;
                    hit = true;
                }
            });
    }

    if (hit) surface_y = static_cast<float>(from.y - nearest);
    return hit;
}

void Renderer::Impl::stand_subject_at(const tl::formats::CmpNode& start,
                                      const Ogre::Vector3& origin) {
    if (!subject_node || !start.has_position()) return;

    Ogre::Vector3 where =
        origin + Ogre::Vector3(start.position().x, start.position().y, start.position().z);

    // Player Start's Y is not a ground height. It sits 1.215 in the town menu
    // while the terrain below tops out at 0.5, so taking it literally leaves the
    // character standing in mid-air. Drop him onto whatever the layout actually
    // placed, starting the ray well above him so a hill between the marker and
    // the surface is still found.
    float surface = 0.0f;
    if (ground_under(Ogre::Vector3(where.x, where.y + 20.0f, where.z), surface)) {
        TL_LOG_INFO("  ground at %.3f under Player Start (marker Y %.3f)",
                    static_cast<double>(surface), static_cast<double>(where.y));
        where.y = surface;
    } else {
        TL_LOG_WARN("  no ground under Player Start; using its own Y %.3f",
                    static_cast<double>(where.y));
    }
    subject_node->setPosition(where);

    // Facing: the marker's first vector is the direction it points, not a
    // matrix column to install.
    //
    // Player Start's is (0.891, 0, 0.454) - straight at the camera, which sits
    // out along +X - and Torchlight's character meshes face +Z in their own
    // space. Treating the three vectors as a rotation basis and applying it
    // turned him a quarter-circle and left him facing away; rotating the mesh's
    // +Z onto that vector is what the data is actually saying.
    //
    // Flattened to the horizontal plane deliberately. These markers carry no
    // pitch - their up vector is exactly (0, 1, 0) - and a character leaning out
    // of the ground plane would be wrong however the numbers were read.
    if (start.vec3s.size() >= 2) {
        Ogre::Vector3 facing(start.vec3s[1].x, 0.0f, start.vec3s[1].z);
        if (!facing.isZeroLength()) {
            facing.normalise();
            subject_node->setOrientation(Ogre::Vector3::UNIT_Z.getRotationTo(facing));
            TL_LOG_INFO("  facing (%.3f, 0, %.3f)", facing.x, facing.z);
        }
    }

    TL_LOG_INFO("  subject at Player Start (%.2f, %.2f, %.2f)", where.x, where.y, where.z);
}

void Renderer::Impl::aim_camera(const tl::formats::CmpScene& layout, const Ogre::Vector3& origin) {
    const tl::formats::CmpNode* eye = layout.find("Camera");
    const tl::formats::CmpNode* target = layout.find("Camera Target");
    if (!eye || !target || !eye->has_position() || !target->has_position()) return;

    const Ogre::Vector3 eye_pos(eye->position().x, eye->position().y, eye->position().z);
    const Ogre::Vector3 target_pos(target->position().x, target->position().y,
                                   target->position().z);

    layout_has_camera = true;
    layout_camera_target = origin + target_pos;

    // Reset the orientation first, and pin the yaw axis. Without a fixed yaw
    // axis OGRE's lookAt takes the shortest rotation from the node's *current*
    // orientation - and frame_camera_on() has already aimed this node at the
    // Alchemist - so the second aim accumulates roll. The symptom is a scene
    // tilted about twenty degrees, which reads as wrongly placed geometry
    // rather than as a camera problem, and sent me looking at node rotations.
    camera_node->setOrientation(Ogre::Quaternion::IDENTITY);
    camera_node->setFixedYawAxis(true, Ogre::Vector3::UNIT_Y);
    camera_node->setPosition(origin + eye_pos);
    camera_node->lookAt(origin + target_pos, Ogre::Node::TS_WORLD);

    // The framing is the layout's, so the clip planes have to suit it rather
    // than the subject's bounding sphere that frame_camera_on() sized them for.
    const Ogre::Real distance = (target_pos - eye_pos).length();
    camera->setNearClipDistance(std::max(0.05f, static_cast<float>(distance) * 0.02f));
    camera->setFarClipDistance(std::max(200.0f, static_cast<float>(distance) * 20.0f));

    TL_LOG_INFO("camera from layout: eye (%.2f, %.2f, %.2f) -> target (%.2f, %.2f, %.2f)",
                eye_pos.x, eye_pos.y, eye_pos.z, target_pos.x, target_pos.y, target_pos.z);
}

}  // namespace tl::render
