// World's small accessors, kept out of the class body (core/world.h).
#include "core/world.h"

#include "core/log.h"

namespace tl::core {

int32_t World::offer_seed(const tl::core::QuestDefinition& quest) const {
    const tl::core::QuestLog::Offered* rolled = quest_log.offered(quest);
    if (rolled != nullptr && rolled->seed != -1) return rolled->seed;
    return static_cast<int32_t>(static_cast<uint32_t>(quest.guid) ^ 0x9e3779b9u);
}

int World::effective_monster_level(int64_t node) const {
    const auto found = unit_spawn_level.find(node);
    if (found != unit_spawn_level.end() && found->second.level > 0) return found->second.level;
    return character_level;
}

std::string World::wanted_player_animation() const {
    if (player_death.dying) {
        return player_death.animation.empty() ? player_animation : player_death.animation;
    }
    if (fishing_state != tl::core::FishingState::Off && !fishing_animation.empty()) {
        return fishing_animation;
    }
    // An `Animation Controller` holding the player (`setAIPlayAnimation`,
    // state 0x10): the scene's animation, over whatever it was doing.
    if (!scripted_player_animation.empty()) return scripted_player_animation;
    if (skill_playback.active) return skill_playback.animation;
    if (swing.active) return swing.animation;
    return player.animation();
}

std::optional<int32_t> World::event_caster_level(tl::core::LevelScript* machine,
                                                 int64_t node) const {
    if (machine == nullptr) return player_character.level();
    int level = 0;
    if (!machine->unit_level(node, level)) return std::nullopt;
    return level;
}

void World::apply_spawn_level(int64_t node) {
    const auto found = unit_spawn_level.find(node);
    if (found != unit_spawn_level.end() && found->second.level > 0) {
        script.set_unit_level(node, found->second.level);
    }
}

const tl::core::UnitStats& World::stats_for_unit(const std::string& unit) const {
    static const tl::core::UnitStats kDefaults{};
    if (const tl::core::UnitStats* found = unit_book.find(unit)) return *found;
    return kDefaults;
}

void World::count_journal(tl::core::JournalStatistic stat, int32_t amount) {
    if (amount == 0) return;
    player_character.journal().increment(stat, amount);
    if (stat != tl::core::JournalStatistic::StepsTaken) {
        const int32_t total = player_character.journal().value(stat);
        TL_LOG_INFO("journal: %s +%d = %d", tl::core::journal_statistic_name(stat), amount, total);
    }
}

bool World::take_quest(const tl::core::QuestDefinition& quest, int level, uint32_t seed) {
    return !quest_log.log_full_for(quest) && quest_log.accept(quest, level, seed, stat_graphs);
}

bool World::start_menu_up() const {
    return debug_menu_open || pause_menu.open || settings_menu.open || !settings_menu.finished;
}

void World::set_player_target(int64_t node) {
    player_target_node = node;
    player_skill_item = 0;
}

bool World::spawn_marked_champion(int64_t node) const {
    const auto found = unit_spawn_level.find(node);
    return found != unit_spawn_level.end() && found->second.champion;
}

float World::swing_rate() const {
    return player_stats.attack_speed > 0.0f ? player_stats.attack_speed / 100.0f : 1.0f;
}

bool World::outline_hostile(const tl::core::UnitStats& stats) const {
    return stats.hostile() && !unit_types.isa(stats.unittype, tl::core::unit_type::kMimic);
}

bool World::unit_hostile(int64_t node, const tl::core::UnitStats& stats) const {
    return unit_side(node, stats) == tl::core::UnitStats::Alignment::Hostile;
}

int World::quest_level_of(const tl::core::QuestDefinition& quest) const {
    return quest_log.offer_level(quest, quest_level_for_offer);
}

tl::core::CharacterEffects World::player_total() const {
    return tl::core::CharacterEffects(&player_effects, &player_inventory);
}

tl::core::CharacterEffects World::pet_total() const {
    return tl::core::CharacterEffects(&pet.effects(), &pet.bag());
}

}  // namespace tl::core
