// The mouse layer's panes: the PC's skills pane.
#include "core/level_memory.h"
#include "core/log.h"
#include "render/mouse_game.h"

#include <cmath>
#include <cstdlib>

namespace tl::render {

namespace {

/// The number of a window named `<stem><digits>`; -1 for any other name (`Skill Points`
/// and `SkillFrameA` are windows of the layout).
long cell_number(const std::string& name, const std::string& stem) {
    if (name.size() <= stem.size() || name.compare(0, stem.size(), stem) != 0) return -1;
    for (size_t at = stem.size(); at < name.size(); ++at) {
        if (name[at] < '0' || name[at] > '9') return -1;
    }
    return std::strtol(name.c_str() + stem.size(), nullptr, 10);
}

}  // namespace

bool Renderer::Impl::MousePanes::toggle_for(const std::string& function) {
    if (function == "guiToggleStats" || function == "guiStatsUp") return toggle_stats();
    if (function == "guiToggleSkills" || function == "guiSkillUp") return toggle_skills();
    if (function == "guiToggleQuests") return toggle_quests();
    if (function == "guiToggleInventory") return toggle_inventory();
    return function == "guiTogglePet" && toggle_pet();
}

/// The skills pane up or down.
bool Renderer::Impl::MousePanes::toggle_skills() {
    if (!skills_tried) {
        skills_tried = true;
        skills.create(kGameGroup, g.config.width, g.config.height);
    }
    if (!skills.ready()) return false;
    skills_open = !skills_open;
    if (!skills_open) skills.set_visible(false);
    // One pane a side: the skills take the inventory's and the log's.
    if (skills_open && inventory_open) toggle_inventory();
    if (skills_open && quests_open) toggle_quests();
    g.play_ui(skills_open ? tl::core::InterfaceSound::MenuOpen
                          : tl::core::InterfaceSound::MenuClose);
    TL_LOG_INFO("mouse: the skills pane %s", skills_open ? "opens" : "closes");
    return true;
}

/// Where the PC puts the class's skills: the `PANE`, `COLUMN` and `ROW` of each `[SKILL]`
/// of the PC's own class file, and its three tab names. The console's class file, which
/// the skill book is read from when it is mounted, places them for its two columns.
void Renderer::Impl::MousePanes::load_skill_tree() {
    if (skill_tree_class == g.player_class) return;
    skill_tree_class = g.player_class;
    skill_tree.clear();
    skill_tabs = {"Skills", "Skills", "Skills"};
    const std::string want = tl::core::upper_ascii(g.player_class);
    for (const std::string& path : g.data_paths_under("media/units/players/")) {
        const std::vector<uint8_t> bytes = g.read_resource(path);
        if (bytes.empty()) continue;
        const tl::formats::AdmFile file =
            tl::core::World::parse_data_group(bytes.data(), bytes.size());
        if (!file.ok || tl::core::upper_ascii(file.string_or("NAME", "")) != want) continue;
        for (const tl::core::ClassSkill& placed : tl::core::parse_class_skills(file, 1)) {
            skill_tree[tl::core::upper_ascii(placed.name)] = placed;
        }
        skill_tabs = tl::core::parse_skill_tab_names(file);
        TL_LOG_INFO("mouse: %zu skill(s) placed by %s", skill_tree.size(), path.c_str());
        break;
    }
}

// CSkillMenu::updateLayout: the open tab's skills that author a column,
// each in its row (`ROW`, or a fifth of the level its next point needs), and the spells.
void Renderer::Impl::MousePanes::sync_skills() {
    if (!skills.ready()) return;
    const bool show = skills_open && !g.hud.letterboxed() && !g.inventory_menu.visible();
    skills.set_visible(show);
    if (!show) return;
    load_skill_tree();
    const int points = g.player_character.unspent_skill_points();
    const auto level = static_cast<uint32_t>(g.player_character.level());
    PcSkills::View view;
    view.tabs = skill_tabs;
    view.tab = skills_tab;
    view.points = "Points Remaining:" + std::to_string(points);
    skill_cells.clear();
    for (const tl::core::KnownSkill& skill : g.player_skills.skills()) {
        if (skill.spell || skill.def == nullptr) continue;
        const auto placed = skill_tree.find(tl::core::upper_ascii(skill.entry.name));
        if (placed == skill_tree.end()) continue;
        if (placed->second.pane != skills_tab || placed->second.column == -1) continue;
        PcSkills::Cell cell;
        cell.column = placed->second.column;
        cell.row = placed->second.row != -1
                       ? placed->second.row
                       : static_cast<int>(std::floor(
                             static_cast<float>(
                                 tl::core::SkillBook::level_required_for_investment(skill)) /
                             5.0f));
        cell.passive = skill.passive();
        cell.icon = skill.effective != 0 ? skill.def->icon : skill.def->icon_inactive;
        if (skill.invested > 0) cell.rank = std::to_string(skill.invested);
        cell.plus = g.player_skills.can_learn(skill, level, points);
        view.cells.push_back(cell);
        skill_cells.push_back(skill.guid());
    }
    for (size_t slot = 0; slot < view.spells.size(); ++slot) {
        const tl::core::KnownSkill* spell = g.player_skills.known_spell(slot);
        if (spell != nullptr && spell->def != nullptr) view.spells[slot] = spell->def->icon;
    }
    skills.set(view);
}

void Renderer::Impl::MousePanes::click_skills() {
    if (over == "Close") {
        toggle_skills();
        return;
    }
    static constexpr const char* kTabs[3] = {"TabA", "TabB", "TabC"};
    for (int tab = 0; tab < 3; ++tab) {
        if (over == kTabs[tab]) skills_tab = tab;
    }
    const long plus_cell = cell_number(over, "Plus");
    const bool plus = plus_cell >= 0;
    const long cell = plus ? plus_cell : cell_number(over, "Skill");
    if (cell < 0 || static_cast<size_t>(cell) >= skill_cells.size()) return;
    tl::core::KnownSkill* skill =
        g.player_skills.find_by_guid(skill_cells[static_cast<size_t>(cell)]);
    if (skill == nullptr) return;
    if (plus) {
        // CSkillMenu::handle_SpendSkill: a point spent, the skill a level
        // up, sample 0x1b.
        if (!g.player_skills.learn(*skill, static_cast<uint32_t>(g.player_character.level()),
                                   g.player_character.unspent_skill_points_ref())) {
            return;
        }
        g.play_ui(tl::core::InterfaceSound::PointAssign);
        TL_LOG_INFO("mouse: '%s' to level %u, %d point(s) left", skill->entry.name.c_str(),
                    skill->invested, g.player_character.unspent_skill_points());
        g.start_player_passives();
        g.refresh_game_hud();
        return;
    }
    // CSkillMenu::handle_SetSkill: a skill with a level that is not
    // passive, is enabled and not hidden becomes the active one - the right button's -
    // with sample 0x1e.
    const bool castable = skill->effective != 0 && !skill->passive() && skill->enabled &&
                          skill->def != nullptr && !skill->def->hidden;
    if (!castable || !g.mouse) return;
    g.mouse->active_skill = skill->guid();
    g.play_ui(tl::core::InterfaceSound::SkillAssign);
    TL_LOG_INFO("mouse: '%s' is the right button's skill", skill->entry.name.c_str());
}

const tl::core::KnownSkill* Renderer::Impl::MousePanes::skill_over() const {
    long cell = cell_number(over, "Plus");
    if (cell < 0) cell = cell_number(over, "Skill");
    if (cell >= 0) {
        return static_cast<size_t>(cell) < skill_cells.size()
                   ? g.player_skills.find_by_guid(skill_cells[static_cast<size_t>(cell)])
                   : nullptr;
    }
    const long spell = cell_number(over, "Spell");
    return spell >= 0 ? g.player_skills.known_spell(static_cast<size_t>(spell)) : nullptr;
}

// INVENTED: the PC's own window is `skilltooltip.layout` (the icon, the name, the rank, the
// type, the description, the mana cost, the cooldown and three damage shares); this is the
// name, the rank and the description in the item's window.
bool Renderer::Impl::MousePanes::skill_tooltip(bool pointer, const tl::core::KnownSkill* hud,
                                               const LayoutScreen* hud_art) {
    const bool in_pane = pointer && over_skills;
    const tl::core::KnownSkill* skill = in_pane ? skill_over() : pointer ? hud : nullptr;
    if (skill == nullptr || skill->def == nullptr) return false;
    const LayoutScreen& art = in_pane || hud_art == nullptr ? skills.screen() : *hud_art;
    const std::string key =
        "k" + std::to_string(skill->guid()) + ":" + std::to_string(skill->invested);
    if (key == tip_key && over_x == tip_x && over_y == tip_y) return true;
    tip_key = key;
    tip_x = over_x;
    tip_y = over_y;
    g.load_unit_types();
    g.refresh_skill_effects();
    tl::core::EquipmentTooltip text;
    text.name.shown = true;
    text.name.text = skill->def->display_name;
    text.type.shown = true;
    text.type.text =
        "Rank " + std::to_string(skill->invested) + "/" + std::to_string(skill->max_rank());
    text.description.shown = true;
    text.description.text =
        tl::core::skill_description_text(*skill, -1, g.unit_types, g.skill_effects);
    // A skill's description is prose: a wider window than an item's.
    tip.show_at(text, PcTooltip::Place{over_x, over_y, g.config.width, g.config.height, 300.0f},
                art);
    return true;
}

}  // namespace tl::render
