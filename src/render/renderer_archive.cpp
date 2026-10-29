// The renderer's asset lookup: the archive index (every entry by basename, the
// material scripts by directory, the spawn classes by name) and the rules that
// qualify a bare or relative reference against it. The port's own (ADR 0002).
// Split from renderer.cpp; the shared declarations are in renderer_impl.h.
#include "render/renderer_impl.h"

#include <dirent.h>

namespace tl::render::renderer_detail {

/// Every archive entry, indexed by lower-cased basename.
///
/// This exists for one reason: `ZipArchive::findFileInfo()` is a **linear scan
/// of the whole archive** with a case-insensitive pattern match per entry, and
/// `ResourceGroupManager::findResourceFileInfo()` calls it once per lookup.
/// `Pak.zip` holds 22,426 entries, and loading the playable town performs
/// thousands of lookups - every mesh, every collision mesh, every texture named
/// by every material. That is tens of millions of string comparisons before a
/// single byte of geometry is read, and it is where the town's load time went:
/// 74 seconds on the console, 27 on the desktop.
///
/// Building the index costs one enumeration. It is safe to cache because the
/// archive is mounted read-only and never changes while it is mounted, and it
/// is dropped and rebuilt if resources are mounted again.
///
/// Keyed on basename rather than on full path because that is the question
/// every caller here actually asks - which is itself a consequence of building
/// OGRE with OGRE_RESOURCEMANAGER_STRICT=0. See ADR 0002.
std::unordered_map<std::string, std::vector<Ogre::FileInfo>> g_archive_index;

/// Material scripts, grouped by the directory they sit in.
///
/// Built from the same enumeration, for the same reason: locating a mesh's
/// material by scanning the archive for `*.material` is another full 22,426
/// entry pass, and parse_sibling_material() does it once per mesh whose stem
/// has no script of its own. On the town that is hundreds of scans.
///
/// Keyed by the directory lower-cased, as every lookup here is (ADR 0002): the
/// zip reader hands the Xbox 360 pak's names back lower-cased
/// (`media/levelsets/cave/`) while its level data names the meshes as the
/// archive spells them (`media/levelSets/cave/`), and an exact key left every
/// level set from the cave on without its materials - white geometry.
std::unordered_map<std::string, std::vector<std::string>> g_materials_by_dir;
bool g_archive_indexed = false;
// PORT: asset lookup. Every material any script declares, by name lower-cased:
// the first script in the archive's order that declares it (parse_material_declaring).
std::unordered_map<std::string, std::string> g_material_scripts;
bool g_material_scripts_indexed = false;
// PORT: asset lookup. archive_paths_under()'s answers, by directory and suffix: a unit's
// animation is looked up there every time it changes, and the archive does not change.
std::unordered_map<std::string, std::vector<std::string>> g_paths_under;

std::vector<std::string> g_mod_roots;

/// `CMod +0x18`, built by `CMod::reloadModFiles`: each mod's files, by their
/// path relative to its folder (lower case, forward slashes), to the file's own path. In the
/// order of `g_mod_roots`.
struct ModFiles {
    std::string root;
    std::unordered_map<std::string, std::string> files;
    std::vector<std::string> relative;  ///< as spelt on disk, in walk order
};
std::vector<ModFiles> g_mod_files;

std::string normalise_mod_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    return path;
}

void index_mod_folder(ModFiles& mod) {
    std::vector<std::string> pending{mod.root};
    while (!pending.empty()) {
        const std::string current = pending.back();
        pending.pop_back();
        DIR* directory = opendir(current.c_str());
        if (!directory) continue;
        while (dirent* entry = readdir(directory)) {
            if (entry->d_name[0] == '.') continue;
            const std::string child = join_path(current, entry->d_name);
            if (is_dir(child)) {
                pending.push_back(child);
                continue;
            }
            const std::string relative = child.substr(mod.root.size() + 1);
            mod.files.emplace(lowered(relative), child);
            mod.relative.push_back(relative);
        }
        closedir(directory);
    }
}

// The file a mod holds at `relative`, or empty.
const std::string* mod_file(const ModFiles& mod, const std::string& relative) {
    const auto found = mod.files.find(lowered(normalise_mod_path(relative)));
    return found == mod.files.end() ? nullptr : &found->second;
}

bool ends_with_lower(const std::string& text, const char* suffix) {
    const size_t n = std::strlen(suffix);
    if (n > text.size()) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(text[text.size() - n + i])) != suffix[i]) {
            return false;
        }
    }
    return true;
}

std::string lowered(const std::string& text) {
    std::string out = text;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

void forget_archive_index() {
    g_archive_index.clear();
    g_materials_by_dir.clear();
    g_material_scripts.clear();
    g_material_scripts_indexed = false;
    g_archive_indexed = false;
    g_paths_under.clear();
}

/// Build the index if it does not exist yet. Idempotent and cheap after the
/// first call.
void ensure_archive_index() {
    if (g_archive_indexed) return;
    g_archive_indexed = true;

    auto& groups = Ogre::ResourceGroupManager::getSingleton();
    Ogre::FileInfoListPtr all = groups.listResourceFileInfo(kGameGroup, false);
    if (all) {
        for (const Ogre::FileInfo& info : *all) {
            g_archive_index[lowered(info.basename)].push_back(info);
            if (ends_with_lower(info.basename, ".material")) {
                g_materials_by_dir[lowered(info.path)].push_back(info.path + info.basename);
            }
        }
    }
    TL_LOG_INFO(
        "archive index: %zu distinct basename(s), %zu directory(ies) with "
        "material scripts",
        g_archive_index.size(), g_materials_by_dir.size());
}

/// Where a material script stands among its directory's scripts in the
/// archive's own order - the order Runic's OGRE parses them in (see the rank
/// test in parse_material_script()). A script the index does not know sorts
/// last.
// PORT: a material script's place in its directory's archive order (OGRE loading).
size_t material_script_rank(const std::string& script) {
    ensure_archive_index();
    const std::string dir = dir_of_asset(script);
    const auto scripts = g_materials_by_dir.find(lowered(dir));
    if (scripts == g_materials_by_dir.end()) return SIZE_MAX;
    const std::string wanted = lowered(script);
    for (size_t i = 0; i < scripts->second.size(); ++i) {
        if (lowered(scripts->second[i]) == wanted) return i;
    }
    return SIZE_MAX;
}

const std::vector<Ogre::FileInfo>* archive_entries(const std::string& basename) {
    ensure_archive_index();
    const auto it = g_archive_index.find(lowered(basename));
    return it == g_archive_index.end() ? nullptr : &it->second;
}

namespace {
/// A mod's files, as read, kept while the same mods are mounted (set_mod_roots). They are
/// loose files, each open several milliseconds on Android's shared storage, and a unit file
/// is read again for every unit that inherits from it, and on every level. Bounded like
/// `g_data_files`: a file past the budget is read the old way.
struct ModFileCache {
    std::unordered_map<std::string, std::vector<uint8_t>> files;
    size_t bytes = 0;
};
ModFileCache g_mod_bytes;
constexpr size_t kModFileBudget = 32u * 1024u * 1024u;

std::vector<uint8_t> read_mod_file(const std::string& file) {
    const auto found = g_mod_bytes.files.find(file);
    if (found != g_mod_bytes.files.end()) return found->second;
    std::vector<uint8_t> bytes = read_file(file);
    if (!bytes.empty() && g_mod_bytes.bytes + bytes.size() <= kModFileBudget) {
        g_mod_bytes.bytes += bytes.size();
        g_mod_bytes.files.emplace(file, bytes);
    }
    return bytes;
}
}  // namespace

void set_mod_roots(const std::vector<std::string>& roots) {
    if (roots != g_mod_roots) g_mod_bytes = ModFileCache{};
    g_mod_roots = roots;
    g_mod_files.clear();
    for (const std::string& root : roots) {
        ModFiles mod;
        mod.root = root;
        while (mod.root.size() > 1 && (mod.root.back() == '/' || mod.root.back() == '\\')) {
            mod.root.pop_back();
        }
        index_mod_folder(mod);
        TL_LOG_INFO("mod folder %s: %zu file(s)", mod.root.c_str(), mod.relative.size());
        g_mod_files.push_back(std::move(mod));
    }
}

// `CModFileFilter::getFilePath`: the first mod, in order, that has the file.
// A compiled name (`.adm`, `.cmp`) is also answered by the mod's text source: the PC compiles a
// mod's `.dat` and `.layout` beside themselves when it loads them, which the port does not need.
const std::string* find_mod_file(const std::string& path) {
    std::string source;
    if (ends_with_lower(path, ".adm") || ends_with_lower(path, ".cmp")) {
        source = path.substr(0, path.size() - 4);
    }
    for (const ModFiles& mod : g_mod_files) {
        if (const std::string* file = mod_file(mod, path)) return file;
        if (!source.empty()) {
            if (const std::string* file = mod_file(mod, source)) return file;
        }
    }
    return nullptr;
}

bool mod_has_file(const std::string& path) { return find_mod_file(path) != nullptr; }

std::string mod_file_path(const std::string& path) {
    const std::string* file = find_mod_file(path);
    return file == nullptr ? std::string() : *file;
}

std::vector<std::vector<uint8_t>> read_mod_copies(const std::string& path) {
    std::vector<std::vector<uint8_t>> copies;
    std::string source;
    if (ends_with_lower(path, ".adm") || ends_with_lower(path, ".cmp")) {
        source = path.substr(0, path.size() - 4);
    }
    for (const ModFiles& mod : g_mod_files) {
        const std::string* file = mod_file(mod, path);
        if (file == nullptr && !source.empty()) file = mod_file(mod, source);
        if (file != nullptr) copies.push_back(read_mod_file(*file));
    }
    return copies;
}

bool read_mod_resource(const std::string& path, std::vector<uint8_t>& bytes) {
    const std::string* file = find_mod_file(path);
    if (file == nullptr) return false;
    bytes = read_mod_file(*file);
    return !bytes.empty();
}

// `CModFileFilter::getFiles`: every mod's file under `prefix` (lower case)
// ending in `suffix`, relative to its folder.
void collect_mod_files(const ModFiles& mod, const std::string& prefix, const char* suffix,
                       std::vector<std::string>& paths) {
    for (const std::string& relative : mod.relative) {
        if (!ends_with_lower(relative, suffix)) continue;
        if (lowered(relative).compare(0, prefix.size(), prefix) == 0) paths.push_back(relative);
    }
}

std::vector<std::string> mod_paths_under(const std::string& dir, const char* suffix) {
    std::vector<std::string> paths;
    const std::string prefix = lowered(normalise_mod_path(dir));
    for (const ModFiles& mod : g_mod_files) collect_mod_files(mod, prefix, suffix, paths);
    std::sort(paths.begin(), paths.end(),
              [](const std::string& a, const std::string& b) { return lowered(a) < lowered(b); });
    paths.erase(std::unique(paths.begin(), paths.end(),
                            [](const std::string& a, const std::string& b) {
                                return lowered(a) == lowered(b);
                            }),
                paths.end());
    return paths;
}

// A mod's data files under `dir` that `paths` (the archive's) does not have, after them.
std::vector<std::string> with_mod_paths(std::vector<std::string> paths, const std::string& dir) {
    if (g_mod_roots.empty()) return paths;
    std::vector<std::string> mods = mod_paths_under(dir, ".dat");
    std::vector<std::string> mod_adm = mod_paths_under(dir, ".dat.adm");
    mods.insert(mods.end(), mod_adm.begin(), mod_adm.end());
    for (const std::string& mod_path : mods) {
        // A mod's `X.dat` is the archive's `X.dat.adm` when both are there: read_resource()
        // answers that name from the mod.
        const auto same_path = [&](const std::string& path) {
            return lowered(path) == lowered(mod_path) ||
                   lowered(path) == lowered(mod_path) + ".adm";
        };
        if (std::none_of(paths.begin(), paths.end(), same_path)) paths.push_back(mod_path);
    }
    return paths;
}

std::vector<std::string> data_paths_under(const std::string& dir) {
    return with_mod_paths(archive_paths_under(dir, ".dat.adm"), dir);
}

std::vector<std::string> data_paths_in_listing_order(const std::string& dir) {
    return with_mod_paths(archive_paths_in_listing_order(dir, ".dat.adm"), dir);
}

/// Every archive path under `dir` whose basename ends in `suffix`.
///
/// Not a glob, deliberately. **OGRE's own `find()` cannot match a path here**:
/// with `OGRE_RESOURCEMANAGER_STRICT=0`, `ZipArchive::load()` overwrites each
/// entry's `filename` with its *basename* (OgreZip.cpp), so a pattern
/// containing a directory - `media/sounds/*.adm` - matches nothing at all and
/// says so by returning an empty list rather than by failing. The archive index
/// this file already builds keeps the real path, so the walk goes through that.
///
/// `dir` is matched case-insensitively and as a prefix, so it reaches
/// subdirectories: `media/units/monsters/` finds `.../QuestUnits/MercPet/`.
std::vector<std::string> archive_paths_under(const std::string& dir, const char* suffix) {
    ensure_archive_index();
    const std::string prefix = lowered(dir);
    std::string key = prefix;
    key += '|';
    key += suffix;
    const auto known = g_paths_under.find(key);
    if (known != g_paths_under.end()) return known->second;
    std::vector<std::string>& out = g_paths_under[key];
    for (const auto& entry : g_archive_index) {
        for (const Ogre::FileInfo& info : entry.second) {
            if (!ends_with_lower(info.basename, suffix)) continue;
            const std::string path = lowered(info.path);
            if (path.size() < prefix.size()) continue;
            if (path.compare(0, prefix.size(), prefix) != 0) continue;
            out.push_back(info.path + info.basename);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// Every archive path under `dir` whose basename ends in `suffix`, in the
/// archive's own order: `CFileSystem::getFilesInDirectory`
/// in pak mode asks
/// `ResourceGroupManager::findResourceFileInfo`, which walks each archive's
/// entries as the zip lists them, and keeps that order.
std::vector<std::string> archive_paths_in_listing_order(const std::string& dir,
                                                        const char* suffix) {
    const std::string prefix = lowered(dir);
    std::vector<std::string> out;
    Ogre::FileInfoListPtr all =
        Ogre::ResourceGroupManager::getSingleton().listResourceFileInfo(kGameGroup, false);
    if (!all) return out;
    for (const Ogre::FileInfo& info : *all) {
        if (!ends_with_lower(info.basename, suffix)) continue;
        const std::string path = lowered(info.path);
        if (path.size() < prefix.size()) continue;
        if (path.compare(0, prefix.size(), prefix) != 0) continue;
        out.push_back(info.path + info.basename);
    }
    return out;
}

// PORT: asset lookup. Fill g_material_scripts: every script in the archive
// read once, its declared names filed, and nothing kept of the text.
void index_material_scripts() {
    g_material_scripts_indexed = true;
    ensure_archive_index();
    Ogre::Timer timer;
    size_t scripts = 0;
    auto& groups = Ogre::ResourceGroupManager::getSingleton();
    for (const auto& [dir, list] : g_materials_by_dir) {
        for (const std::string& script : list) {
            Ogre::DataStreamPtr stream = groups.openResource(script, kGameGroup);
            if (!stream) continue;
            for (const std::string& name : declared_materials(stream->getAsString())) {
                g_material_scripts.emplace(lowered(name), script);
            }
            ++scripts;
        }
    }
    TL_LOG_INFO("material index: %zu material(s) in %zu script(s), %llu ms",
                g_material_scripts.size(), scripts,
                static_cast<unsigned long long>(timer.getMilliseconds()));
}

// PORT: asset lookup. The original parses every material script in the
// archive when it starts, so a material is found wherever it is declared; the
// port parses a mesh's own directory (parse_sibling_material), because
// parsing the whole archive aborts on its duplicate names. A name declared
// only elsewhere - the palace pillar's `palace_tileset/palace_stairs02_mat`,
// in `media/models/palace_secret_door/palace.material` in both releases -
// left the mesh without one. This parses the script that declares it, from the
// index the mount builds (`mount_resources`).
void parse_material_declaring(const std::string& material) {
    if (!g_material_scripts_indexed) index_material_scripts();
    const auto it = g_material_scripts.find(lowered(material));
    if (it == g_material_scripts.end()) return;
    TL_LOG_INFO("material '%s': declared only in %s", material.c_str(), it->second.c_str());
    parse_material_script(it->second);
}

/// The directory a resource sits in inside the archive, with a trailing slash.
/// Empty if it cannot be located or the basename is ambiguous.
std::string archive_dir_of(const std::string& resource_name) {
    const std::vector<Ogre::FileInfo>* found = archive_entries(resource_name);
    if (!found || found->empty()) return {};

    if (found->size() > 1) {
        TL_LOG_WARN("'%s' is ambiguous: %zu copies in the archive, using %s", resource_name.c_str(),
                    found->size(), found->front().path.c_str());
    }
    return found->front().path;
}

/// The archive path of `basename` inside `dir`, cased as the archive cases it.
///
/// The same qualification rule as qualify_texture_paths(), for a different kind
/// of reference: a bare basename that occurs in more than one directory cannot
/// be opened at all, and Idle.SKELETON occurs in 145 of them. Matching is
/// case-insensitive, and the returned path carries the archive's own spelling
/// so that ZipArchive::exists(), which compares the basename exactly, agrees.
///
/// The directory is matched case-insensitively too: the Xbox 360 pak's own data
/// spells its paths in lower case (`media/levelsets/town1/`) where its archive
/// keeps `media/levelSets/town1/`, so an exact match qualified nothing that a
/// lower-cased path referred to - the town's birches, the mine's cobwebs and
/// rails drew blank. The PC data spells them as its archive does.
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
    // A mod's class first, found as the game finds a unit, by `NAME`: its file can be named
    // otherwise (`AmmutDisciple.dat` holds "Ammut Disciple").
    for (const std::string& path : data_paths_under("media/units/players/")) {
        if (!mod_has_file(path)) continue;
        const std::vector<uint8_t> bytes = read_resource(path);
        const tl::formats::AdmFile file = tl::formats::parse_adm(bytes.data(), bytes.size());
        if (file.ok && lowered(file.string_or("NAME", "")) == lowered(class_name)) return path;
    }
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

std::string archive_path_in_dir(const std::string& basename, const std::string& dir) {
    // A mod's file first, its text source answering a compiled name (`zealot_pc.animation` for
    // `zealot_pc.ANIMATION.adm`).
    if (mod_has_file(dir + basename)) return dir + basename;
    const std::vector<Ogre::FileInfo>* found = archive_entries(basename);
    if (!found) return {};

    const auto same_dir = [&dir](const std::string& path) {
        return path.size() == dir.size() &&
               std::equal(dir.begin(), dir.end(), path.begin(), [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) ==
                          std::tolower(static_cast<unsigned char>(b));
               });
    };
    for (const Ogre::FileInfo& info : *found) {
        if (info.path == dir) return info.path + info.basename;
    }
    for (const Ogre::FileInfo& info : *found) {
        if (same_dir(info.path)) return info.path + info.basename;
    }
    return {};
}

std::string archive_path_unique(const std::string& basename) {
    const std::vector<Ogre::FileInfo>* found = archive_entries(basename);
    if (!found || found->size() != 1) return {};
    return found->front().path + found->front().basename;
}

// PORT: asset lookup. The copy of a `basename` the archive holds more than once
// that is nearest the referring directory `dir`: one in a directory above it,
// the closest first; else the one whose path shares the most leading
// directories with it, two at least (`media/` alone says nothing); archive
// order breaks a tie. Empty when none qualifies. The Xbox 360 pak keeps some
// textures a material names only beside its parent (the sunken temple's
// `stemple_broadleaf_*/` water, `wave.dds` one directory up) or in another
// set's tree (`Destroyer_Chain_shoulder.dds` under `wardrobe/` and
// `models/armor/`, where the PC pak has one `.png`), and a bare basename that
// is not unique opens nothing (ADR 0002).
std::string archive_path_nearest(const std::string& basename, const std::string& dir) {
    const std::vector<Ogre::FileInfo>* found = archive_entries(basename);
    if (found == nullptr || found->empty()) return {};
    const std::string want = lowered(dir);
    const Ogre::FileInfo* best = nullptr;
    size_t best_score = 0;
    for (const Ogre::FileInfo& info : *found) {
        const std::string path = lowered(info.path);
        size_t shared = 0;  // whole leading directories in common
        for (size_t i = 0; i < path.size() && i < want.size() && path[i] == want[i]; ++i) {
            if (path[i] == '/') ++shared;
        }
        const bool above = want.compare(0, path.size(), path) == 0;
        const size_t score = above ? 1000 + shared : (shared >= 2 ? shared : 0);
        if (score > best_score) {
            best_score = score;
            best = &info;
        }
    }
    return best == nullptr ? std::string() : best->path + best->basename;
}

// PORT: asset lookup. resolve_texture()'s last resort: `name`'s basename, under
// its own extension or any other, at the copy nearest `dir`
// (archive_path_nearest).
std::string nearest_texture(const std::string& name, const std::string& dir) {
    const size_t slash = name.find_last_of("/\\");
    const std::string bare = slash == std::string::npos ? name : name.substr(slash + 1);
    const std::string stem = bare.substr(0, bare.find_last_of('.'));
    for (const std::string& candidate :
         {bare, stem + ".dds", stem + ".png", stem + ".tga", stem + ".jpg"}) {
        std::string path = archive_path_nearest(candidate, dir);
        if (!path.empty()) return path;
    }
    return {};
}

std::string resolve_texture(const std::string& name, const std::string& dir,
                            const std::string& fallback_dir) {
    auto try_name = [&](const std::string& candidate) -> std::string {
        std::string path = archive_path_in_dir(candidate, dir);
        if (path.empty() && !fallback_dir.empty() && fallback_dir != dir) {
            path = archive_path_in_dir(candidate, fallback_dir);
        }
        if (path.empty()) path = archive_path_unique(candidate);
        return path;
    };

    // A reference carrying path separators is a path, not a basename. Collapse
    // it against the referring material's directory and look it up whole; the
    // archive's own spelling is matched case-insensitively, which matters here
    // because the town's river asks for `WATER_Base.dds` and the file ships as
    // `WATER_base.dds`.
    if (const std::string relative = tl::core::AssetIndex::resolve_relative(dir, name);
        !relative.empty()) {
        std::string path = resolve_archive_path(relative);
        if (!path.empty()) return path;

        // The reference's own extension does not always match the archive's -
        // `mine_lantern01.png` ships as `mine_lantern01.dds`, which is exactly
        // this function's reason for existing - but until now that mismatch
        // was only ever tried against a bare basename, never against a
        // genuinely relative reference: `mine_elevator/mine_eievator01.mesh`
        // asks for `../mine_eievator01.png`, one directory up from where the
        // mesh sits, and the archive's copy is `.dds`. Collapsing already
        // found the right *directory*; only the extension is still a guess,
        // so the guess is retried here rather than only at the bare-basename
        // fallback below, which a `../`-prefixed name never reaches cleanly.
        const size_t dot = relative.find_last_of('.');
        if (dot != std::string::npos) {
            const std::string stem = relative.substr(0, dot);
            for (const char* ext : {".dds", ".png", ".tga", ".jpg"}) {
                path = resolve_archive_path(stem + ext);
                if (!path.empty()) return path;
            }
        }
    }

    std::string path = try_name(name);
    if (!path.empty()) return path;

    // Fall back to the basename alone. A reference with separators that did not
    // resolve as a path may still name a file that exists somewhere else, which
    // is the same forgiving behaviour every other reference here gets.
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) {
        path = try_name(name.substr(slash + 1));
        if (!path.empty()) return path;
    }

    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return {};
    const std::string stem = name.substr(0, dot);

    for (const char* ext : {".dds", ".png", ".tga", ".jpg"}) {
        const std::string candidate = stem + ext;
        if (candidate.size() == name.size() &&
            std::equal(candidate.begin(), candidate.end(), name.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            })) {
            continue;  // already tried
        }
        path = try_name(candidate);
        if (!path.empty()) return path;
    }

    // Last, a name the archive holds more than once and nowhere beside the
    // material: the copy nearest it.
    return nearest_texture(name, dir);
}

/// Resolve an archive path as a layout spells it to the archive's own spelling.
///
/// Layout data is uppercase throughout - `MEDIA/LIGHTS/PALACE/PALACE_GRADIENT_01.MESH`
/// - while the archive stores `media/lights/palace/palace_gradient_01.Mesh`.
/// Every component differs, including the extension. That matters more than it
/// looks: `ZipArchive::exists()`, which ResourceGroupManager falls back to for a
/// path its basename index cannot answer, compares the basename **exactly**, so
/// the uppercase form finds nothing at all.
///
/// The basename index is case-insensitive, so it answers the question; when a
/// basename occurs in several directories the referring path disambiguates.
namespace {
std::string resolve_archive_path_impl(const std::string& reference, bool quiet) {
    if (reference.empty()) return {};

    // Runic's data mixes separators freely - a particle node names
    // `media\particles\textures\sequence\flame2x4.dds` while the layout beside it
    // uses forward slashes - and a path split on '/' alone treats the whole
    // backslashed string as one basename, which matches nothing. The town's
    // torches had no flames for exactly this reason. tl::core::AssetIndex
    // already normalises separators for direct file access; this is the same
    // rule for the layer OGRE answers.
    std::string path = reference;
    for (char& c : path) {
        if (c == '\\') c = '/';
    }

    if (mod_has_file(path)) return path;

    const size_t slash = path.find_last_of('/');
    const std::string basename = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const std::string dir =
        (slash == std::string::npos) ? std::string() : path.substr(0, slash + 1);

    const std::vector<Ogre::FileInfo>* found = archive_entries(basename);
    if (!found || found->empty()) return {};
    if (found->size() == 1) return found->front().path + found->front().basename;

    for (const Ogre::FileInfo& info : *found) {
        if (info.path.size() != dir.size()) continue;
        if (std::equal(dir.begin(), dir.end(), info.path.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            })) {
            return info.path + info.basename;
        }
    }

    if (!quiet) {
        TL_LOG_WARN("'%s' is ambiguous and no copy sits in '%s'", basename.c_str(), dir.c_str());
    }
    return {};
}
}  // namespace

std::string resolve_archive_path(const std::string& reference) {
    return resolve_archive_path_impl(reference, false);
}

// PORT: asset lookup, the same without the warning.
std::string resolve_archive_path_quiet(const std::string& reference) {
    return resolve_archive_path_impl(reference, true);
}

namespace {
/// The game's `.adm` data files, as read, kept across `Renderer::restart()`.
///
/// Every level change is a restart, and a restart drops every table built out
/// of the archive with the Impl that held it: the skill files (read three
/// times over), the affixes, the monster and prop units, the quests, the stat
/// curves, `masterresourceunits` - a couple of thousand small files, each a
/// seek, a read and an inflate, which on the console is an unbuffered SD card
/// read per file. The archive itself does not change while the process runs,
/// so the second level onwards takes them from here; parsing them is still
/// done afresh, by the same code, so what the tables hold cannot differ.
///
/// Only `.adm`: meshes, textures and sounds are read once by OGRE or the mixer
/// and are far larger. Bounded, because applet mode is the tighter budget: a
/// file that would take it past the limit is simply read the old way.
struct DataFileCache {
    /// What was mounted when the cache was filled; see remember_mounts().
    std::string mounts;
    std::unordered_map<std::string, std::vector<uint8_t>> files;
    size_t bytes = 0;
};
DataFileCache g_data_files;
constexpr size_t kDataFileBudget = 32u * 1024u * 1024u;
}  // namespace

void remember_mounts(const std::string& mounts) {
    if (mounts == g_data_files.mounts) return;
    g_data_files = DataFileCache{};
    g_data_files.mounts = mounts;
}

/// Read a resource out of the mounted archive. Empty on any failure.
std::vector<uint8_t> read_resource(const std::string& path) {
    const bool data_file = ends_with_lower(path, ".adm");
    if (data_file) {
        const auto found = g_data_files.files.find(path);
        if (found != g_data_files.files.end()) return found->second;
    }
    std::vector<uint8_t> mod_bytes;
    if (read_mod_resource(path, mod_bytes)) {
        TL_LOG_DEBUG("mod override: %s", path.c_str());
        if (data_file && !mod_bytes.empty() &&
            g_data_files.bytes + mod_bytes.size() <= kDataFileBudget) {
            g_data_files.bytes += mod_bytes.size();
            g_data_files.files.emplace(path, mod_bytes);
        }
        return mod_bytes;
    }
    auto& groups = Ogre::ResourceGroupManager::getSingleton();
    try {
        Ogre::DataStreamPtr stream = groups.openResource(path, kGameGroup);
        if (!stream) return {};
        std::vector<uint8_t> bytes(stream->size());
        if (!bytes.empty()) stream->read(bytes.data(), bytes.size());
        if (data_file && !bytes.empty() && g_data_files.bytes + bytes.size() <= kDataFileBudget) {
            g_data_files.bytes += bytes.size();
            g_data_files.files.emplace(path, bytes);
        }
        return bytes;
    } catch (const Ogre::Exception& e) {
        TL_LOG_WARN("could not read '%s': %s", path.c_str(), e.getDescription().c_str());
        return {};
    }
}

}  // namespace tl::render::renderer_detail

namespace tl::render {

// PORT: the archive hook (core/world.h) answered by the mounted archive.
std::string Renderer::Impl::archive_animation_in_dir(const std::string& dir,
                                                     const std::string& state, bool armed,
                                                     int variation) const {
    return renderer_detail::archive_animation_in_dir(dir, state, armed, variation);
}

// PORT: the archive hook (core/world.h) answered by the mounted archive.
std::string Renderer::Impl::archive_path_in_dir(const std::string& basename,
                                                const std::string& dir) const {
    return renderer_detail::archive_path_in_dir(basename, dir);
}

// PORT: the archive hook (core/world.h) answered by the mounted archive.
std::string Renderer::Impl::archive_path_unique(const std::string& basename) const {
    return renderer_detail::archive_path_unique(basename);
}

// PORT: the archive hook (core/world.h) answered by the mounted archive.
std::vector<std::string> Renderer::Impl::archive_paths_under(const std::string& dir,
                                                             const char* suffix) const {
    return renderer_detail::archive_paths_under(dir, suffix);
}

std::vector<std::string> Renderer::Impl::data_paths_under(const std::string& dir) const {
    return renderer_detail::data_paths_under(dir);
}

std::vector<std::string> Renderer::Impl::data_paths_in_listing_order(const std::string& dir) const {
    return renderer_detail::data_paths_in_listing_order(dir);
}

// PORT: the archive hook (core/world.h) answered by the mounted archive.
bool Renderer::Impl::console_game_data() const { return tl::render::console_game_data(); }

// PORT: the archive hook (core/world.h) answered by the mounted archive.
std::string Renderer::Impl::dir_of_asset(const std::string& name) const {
    return renderer_detail::dir_of_asset(name);
}

// PORT: the archive hook (core/world.h) answered by the mounted archive.
tl::core::DungeonManager& Renderer::Impl::loaded_dungeon_manager() const {
    return tl::render::loaded_dungeon_manager();
}

// PORT: the archive hook (core/world.h) answered by the mounted archive.
std::vector<uint8_t> Renderer::Impl::read_resource(const std::string& path) const {
    return renderer_detail::read_resource(path);
}

// PORT: the archive hook (core/world.h) answered by the mounted archive.
std::string Renderer::Impl::resolve_archive_path(const std::string& reference) const {
    return renderer_detail::resolve_archive_path(reference);
}

// PORT: the archive hook (core/world.h) answered by the mounted archive.
tl::core::DungeonFiles Renderer::Impl::archive_dungeon_files() const {
    return tl::render::archive_dungeon_files();
}

// PORT: the archive hook (core/world.h) answered by the mounted archive.
std::string Renderer::Impl::class_unit_path(const std::string& class_name, bool* console) const {
    return renderer_detail::class_unit_path(class_name, console);
}

// PORT: the archive hook (core/world.h) answered by the mounted archive.
std::string Renderer::Impl::resolve_archive_path_quiet(const std::string& reference) const {
    return renderer_detail::resolve_archive_path_quiet(reference);
}

// PORT: the archive hook (core/world.h) answered by the mounted archive.
std::vector<std::string> Renderer::Impl::archive_paths_in_listing_order(const std::string& dir,
                                                                        const char* suffix) const {
    return renderer_detail::archive_paths_in_listing_order(dir, suffix);
}

}  // namespace tl::render
