// The gameplay half of render/renderer_skill_data.cpp (core/world.h).
#include "core/world.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tl::core {

/// Index every skill by its `NAME`, once (ANIMATION and RANGE only).
// INVENTED: the port's skill index, for the monster attack lists index_unit builds; the
// console's characters hold CSkill objects made by their CSkillManager.
void World::ensure_skill_index() {
    if (skill_files_loaded) return;
    skill_files_loaded = true;
    for (const std::string& path : data_paths_under("media/skills/")) {
        const std::vector<uint8_t> bytes = read_resource(path);
        if (bytes.empty()) continue;
        const tl::formats::AdmFile file = parse_data_group(bytes.data(), bytes.size());
        if (!file.ok || file.section() != "SKILL") continue;
        const std::string name = file.string_or("NAME", "");
        const std::string animation = file.string_or("ANIMATION", "");
        if (name.empty() || animation.empty()) continue;
        tl::core::AttackSkill skill;
        skill.name = name;
        skill.animation = animation;
        skill.range = file.float_or("RANGE", 0.0f);
        skills.emplace(lowered(name), skill);
    }
    TL_LOG_INFO("skill index: %zu skill(s) with an animation", skills.size());
}

/// Every skill file by its upper-cased `NAME`, whole, the console's copies first, first of a
/// name kept. CSkillParser::parseSkills, per file
/// CSkillParser::parseSkill; also
/// CSkillParser::clearSkills,
/// CSkillParser::CSkillParser. Made once, here on first use.
/// DIVERGENCE: a file without `UNIQUE_GUID` keeps -1; the original draws one
/// (UTILITIES::createUniqueGuid) and writes the file back (CDataGroup::SaveToFile).
void World::ensure_player_skill_files() {
    if (player_skill_files_loaded) return;
    player_skill_files_loaded = true;
    size_t console = 0;
    for (const char* root : {"xbox/media/skills/", "media/skills/"}) {
        for (const std::string& path : data_paths_under(root)) {
            const std::vector<uint8_t> bytes = read_resource(path);
            if (bytes.empty()) continue;
            auto entry = std::make_unique<PlayerSkillFile>();
            entry->file = parse_data_group(bytes.data(), bytes.size());
            // A mod's skill is read whatever its group is called, as the PC reads every skill
            // (`CSkillParser::parseSkill`, asks only for its NAME).
            if (!entry->file.ok || (entry->file.section() != "SKILL" && !mod_has_file(path)))
                continue;
            const std::string name = tl::core::upper_ascii(entry->file.string_or("NAME", ""));
            if (name.empty() || player_skill_files.count(name) != 0) continue;
            entry->def = tl::core::parse_skill_def(entry->file);
            // An Xbox 360 pak's own are the console's; a PC pak's are not, and
            // only the `console_ui` overlay's (`xbox/`) are.
            if (root[0] == 'x' || console_game_data()) ++console;
            player_skill_files.emplace(name, std::move(entry));
        }
    }
    TL_LOG_INFO("player skills: %zu skill file(s), %zu of them the console's",
                player_skill_files.size(), console);
}

/// The effect types and the affixes, once. (CPlayer::unitInit's class skills are not here.)
void World::ensure_effect_data() {
    if (effect_data_loaded) return;
    effect_data_loaded = true;

    // `parseEffects` reads exactly one file;
    // the 145 rows in it are the effect types, and a row's index is the type an
    // `[EFFECT]` block's `TYPE` resolves to.
    const std::string effects_path = archive_path_unique("EFFECTSLIST.DAT.adm");
    if (!effects_path.empty()) {
        const std::vector<uint8_t> bytes = read_resource(effects_path);
        if (!bytes.empty()) {
            const tl::formats::AdmFile file = parse_data_group(bytes.data(), bytes.size());
            if (file.ok) effect_types.load(file);
        }
    }
    if (effect_types.empty()) {
        TL_LOG_WARN("effects: media/EffectsList.dat did not read; skills print no stat lines");
    }

    load_unit_types();
    // The console's reload reads media/AFFIXES.RAW: one row per file,
    // sorted by upper-cased path, the order the roll walks
    // (AffixBook::roll_and_create_random_affixes). DIVERGENCE: a file under
    // xbox/media/affixes/ replaces the PC's at the same path, as the console has one row.
    std::map<std::string, std::string> rows;  // upper-cased path under media/ -> archive path
    for (const char* root : {"media/affixes/", "xbox/media/affixes/"}) {
        const std::string prefix = root;
        for (const std::string& path : data_paths_under(root)) {
            std::string key = "media/affixes/" + path.substr(prefix.size());
            for (char& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            rows[key] = path;
        }
    }
    for (const auto& [key, path] : rows) {
        const std::vector<uint8_t> bytes = read_resource(path);
        if (bytes.empty()) continue;
        const tl::formats::AdmFile file = parse_data_group(bytes.data(), bytes.size());
        if (!file.ok || file.section() != "AFFIX") continue;
        affixes.add(tl::core::parse_affix(file, effect_types, &unit_types));
    }
    TL_LOG_INFO("effects: %zu effect type(s), %zu affix file(s)", effect_types.size(),
                affixes.size());

    // `CSets::reload`: one set per file under
    // media/sets/, each bonus resolved against the affix book just loaded.
    std::vector<tl::formats::AdmFile> set_files;
    for (const std::string& path : data_paths_under("media/sets/")) {
        const std::vector<uint8_t> bytes = read_resource(path);
        if (bytes.empty()) continue;
        tl::formats::AdmFile file = parse_data_group(bytes.data(), bytes.size());
        if (file.ok) set_files.push_back(std::move(file));
    }
    std::vector<const tl::formats::AdmFile*> set_pointers;
    set_pointers.reserve(set_files.size());
    for (const tl::formats::AdmFile& file : set_files) set_pointers.push_back(&file);
    item_sets.reload(set_pointers, affixes);
    TL_LOG_INFO("items: %zu set(s)", item_sets.size());
}

}  // namespace tl::core
