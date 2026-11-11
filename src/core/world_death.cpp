// A unit's death, `CCharacter::die` (core/world.h): the half render/renderer_combat.cpp left.
#include "core/character_particles.h"
#include "core/world.h"

namespace tl::core {

// `CCharacter::die`, its `CRYSTALEXPLODE`, `DEATHEXPLODE` and
// `KILL` shakes.
bool World::bursts_on_death(tl::core::LevelScript& owner, int64_t node, const std::string& unit) {
    const tl::core::UnitStats& victim = stats_for_unit(unit);
    if (victim.is_champion || spawn_marked_champion(node)) return false;
    // A unit not yet given an effect or affix has no effect manager (`+0x17c`) and never bursts.
    const tl::core::EffectManager* effects = owner.unit_effects(node);
    if (effects == nullptr || !effects->made() || effects->has_affix("DAMAGEEFFECTFROZEN"))
        return false;
    const auto names = tl::core::character_particle_names(victim.particles);
    return !names[tl::core::character_particle::kExplode].empty() && game_settings().show_blood;
}

void World::start_death_shake(const char* name, float duration, float magnitude,
                              const ogre::Vector3& centre) {
    load_camera_shake_graphs();
    // `die` hands the shake its own `+0x9c`, `getUp()`: a character stands
    // upright.
    tl::core::CameraShake shake(name, tl::core::Vec3f{0.0f, 1.0f, 0.0f}, duration,
                                camera_shake_graphs);
    shake.magnitude = magnitude;
    shake.origin = tl::core::Vec3f{centre.x, centre.y, centre.z};
    shake.has_origin = true;
    shake.falloff = true;
    TL_LOG_INFO("camera: %s shake%s", name,
                shake.curve != nullptr ? "" : " has no curve; nothing moves");
    camera_shakes.add(shake);
}

// `CCharacter::die`: `questEventFire(1, attacker, this)`.
void World::kill_quest_event(int64_t node, const std::string& unit) {
    const auto tagged = quest_unit_tags.find(node);
    ogre::Vector3 body = ogre::Vector3::ZERO;
    unit_node_position(node, body);
    tl::core::QuestEventUnit dead = quest_event_unit(
        unit, stats_for_unit(unit).unittype, body,
        tagged == quest_unit_tags.end() ? tl::core::QuestUnitTag{} : tagged->second);
    // The character's name (`+0x4c0`): a champion's or a quest's
    // own over the unit's.
    const auto named = unit_names.find(node);
    const tl::core::UnitStats& dead_stats = stats_for_unit(unit);
    const tl::core::LevelScript* of = script_owning(node);
    const std::string champion = of != nullptr ? of->unit_champion_name(node) : std::string();
    dead.shown_name = named != unit_names.end()         ? named->second
                      : !champion.empty()               ? champion
                      : dead_stats.display_name.empty() ? unit
                                                        : dead_stats.display_name;
    quest_event(quest_log.unit_defeated(dead));
}

// `CCharacter::die`: a `DESUMMON ON DEATH` holder's
// `TRANSFORM` particle (slot 8) started at its centre, then its mesh hidden.
void World::desummon_on_death(int64_t node, const std::string& unit) {
    tl::core::LevelScript* const holder = script_owning(node);
    if (holder == nullptr) return;
    const tl::core::EffectManager* effects = holder->unit_effects(node);
    if (effects == nullptr || !effects->has_effect(tl::core::kEffectTypeDesummonOnDeath)) return;
    const tl::core::UnitStats& stats = stats_for_unit(unit);
    ogre::Vector3 centre = ogre::Vector3::ZERO;
    unit_node_position(node, centre);
    centre.y += stats.radius;
    start_slot_particle(stats, tl::core::character_particle::kTransform, centre,
                        character_ref(node));
    std::vector<tl::core::ScriptEvent> hidden;
    holder->set_unit_hidden(node, true, /*immediate=*/true, hidden);
    if (NodeMap* nodes = nodes_of(holder); nodes != nullptr && !hidden.empty()) {
        apply_script_events(hidden, *nodes);
    }
}

// The experience `applyDamage` pays for a hostile unit's death.
void World::kill_pays_experience(int64_t node, const std::string& unit,
                                 const tl::core::UnitStats& stats, int64_t attacker) {
    const bool champion = stats.is_champion || spawn_marked_champion(node);

    // Experience: EXPERIENCE_MONSTER / EXPERIENCE_CHAMPIONMONSTER at the difficulty,
    // squared, at the monster's level (`monster_experience()`); the authored XP only
    // switches it on. The level is the floor's (`unit_spawn_level`), else
    // `character_level`.
    const int monster_level = effective_monster_level(node);
    // `applyDamage` pays the attacker: no attacker, no experience; nor from a unit
    // released with `GIVE XP` clear (+0x454 = 0).
    const bool pays =
        attacker != tl::core::LevelScript::kNoAttacker && no_xp_units.count(node) == 0;
    const uint64_t xp = pays ? tl::core::monster_experience(monster_level, stats.xp, champion,
                                                            config.difficulty, stat_graphs)
                             : 0;
    // `CCharacter::awardExperience`: refused for a dying or dead character
    // with something to award; else incrementJournalStatistic(6, 1), and
    // (7, 1) for a champion.
    const bool dying = player_health.dead || player_death.dying;
    if (attacker != tl::core::LevelScript::kNoAttacker && !(dying && xp > 0)) {
        count_journal(tl::core::JournalStatistic::MonstersDefeated);
        if (champion) count_journal(tl::core::JournalStatistic::ChampionsDefeated);
    }
    const bool leveled = award_player_experience(static_cast<int>(xp), dying);
    TL_LOG_INFO("experience: %s pays %llu, level %d at %llu/%llu", unit.c_str(),
                static_cast<unsigned long long>(xp), player_character.level(),
                static_cast<unsigned long long>(player_character.experience()),
                static_cast<unsigned long long>(player_character.experience_gate()));
    if (leveled) player_levelled_up();

    update_menu();
    refresh_game_hud();
}

void World::burst_visuals(tl::core::LevelScript& owner, int64_t node, const std::string& unit) {
    const tl::core::UnitStats& victim = stats_for_unit(unit);
    if (tl::core::character_particle_names(
            victim.particles)[static_cast<size_t>(tl::core::character_particle::kExplode)]
            .empty()) {
        return;
    }
    ogre::Vector3 centre = ogre::Vector3::ZERO;
    unit_node_position(node, centre);
    centre.y += victim.radius;
    start_slot_particle(victim, tl::core::character_particle::kExplode, centre,
                        character_ref(node));
    const float magnitude = tl::core::volatile_random().between(0.5f, 0.75f);
    start_death_shake("DEATHEXPLODE", 0.3f, magnitude, centre);
    std::vector<tl::core::ScriptEvent> hidden;
    owner.set_unit_hidden(node, true, /*immediate=*/true, hidden);
    if (NodeMap* nodes = nodes_of(&owner); nodes != nullptr && !hidden.empty()) {
        apply_script_events(hidden, *nodes);
    }
}

/// A placed unit's life has run out. The fall is `Die`, then `Die1`: models name their
/// death animation either way and the unit data names neither.
void World::kill_unit(int64_t node, const std::string& unit, bool fall, int64_t attacker,
                      bool pays_experience) {
    // The machine that runs the unit: its state now, its `Monster Killed` wires after the treasure.
    tl::core::LevelScript* const runs = script_owning(node);
    tl::core::LevelScript& dying_in = runs != nullptr ? *runs : script;
    std::vector<tl::core::ScriptEvent> events;
    dying_in.set_defer_death_broadcast(true);
    dying_in.unit_killed(node, quest_log.state(), events);
    dying_in.set_defer_death_broadcast(false);
    kill_quest_event(node, unit);

    if (NodeMap* nodes = nodes_of(&dying_in); nodes != nullptr && !events.empty()) {
        apply_script_events(events, *nodes);
    }

    index_unit(unit);

    desummon_on_death(node, unit);

    // `CCharacter::die`: `stopAllSkills`,
    // `fireSkillsOnDeath`, then `executeProcs(0x69, attacker)`, the
    // `CAST SKILL ON DEATH` effects.
    tl::core::LevelScript* const owner = script_owning(node);
    if (owner != nullptr) {
        fire_unit_skills_on_death(*owner, node);
        const ProcTarget by = proc_target_attacker(attacker);
        unit_execute_procs(*owner, node, tl::core::kEffectCastSkillOnDeath, by.machine, by.node,
                           by.any() || attacker == 0);
    }
    {
        // `CCharacter::die`: the death particle, by "Show Blood", after the
        // procs and before the death sample.
        ogre::Vector3 body = ogre::Vector3::ZERO;
        if (unit_node_position(node, body)) {
            play_death_particle(stats_for_unit(unit), body, character_ref(node));
        }
    }
    if (owner != nullptr) {
        std::vector<tl::core::ScriptEvent> sample;
        owner->play_death_sample(node, sample);
        if (NodeMap* nodes = nodes_of(owner); nodes != nullptr && !sample.empty()) {
            apply_script_events(sample, *nodes);
        }
    }
    // `CCharacter::die`: a burst, after the death sample - the
    // explode particle at the centre, `DEATHEXPLODE` for 0.3 seconds at
    // `randomBetweenVolatile(0.5, 0.75)`, then `setVisible(false, true)`.
    bool burst = false;
    if (owner != nullptr && owner->take_burst_death(node) && bursts_on_death(*owner, node, unit)) {
        burst = true;
    }
    if (burst) {
        burst_visuals(*owner, node, unit);
    }
    // `CCharacter::die`: the `DEATH_FLIGHT` branch, after the burst
    // and before the treasure.
    if (!burst && owner != nullptr) {
        std::vector<tl::core::ScriptEvent> flown;
        owner->fly_killed_unit(node, flown);
        if (NodeMap* nodes = nodes_of(owner); nodes != nullptr && !flown.empty()) {
            apply_script_events(flown, *nodes);
        }
    }
    // a death with no blow to fly from and no burst:
    // `findRandomAnimation("DIE")` blended in.
    std::string animation;
    if (fall && !burst) {
        if (tl::core::LevelScript* owner = script_owning(node)) {
            animation = owner->death_animation(node);
        }
    }
    const bool fell = burst || !fall || view_unit_falls(node, animation);
    TL_LOG_INFO("combat: %s dies%s%s%s", unit.c_str(),
                fell ? "" : " (no death animation beside its mesh)",
                animation.empty() ? "" : ", playing ", animation.c_str());

    // The treasure at the end of `CCharacter::die`,
    // after `fireSkillsOnDeath` and before `applyDamage` pays the experience.
    drop_death_loot(node, unit, attacker);
    {
        // `die`: `BroadcastEventFromObject`, the level's wires hear of it.
        std::vector<tl::core::ScriptEvent> heard;
        dying_in.broadcast_owed_death(node, quest_log.state(), heard);
        if (NodeMap* nodes = nodes_of(&dying_in); nodes != nullptr && !heard.empty()) {
            apply_script_events(heard, *nodes);
        }
    }
    // after the money: whoever watches the unit hears that it died
    // (`unitBroadcastMessage(1)`, inline).
    unit_observers.broadcast({script_owning(node), node}, tl::core::unit_state::kDied);

    const tl::core::UnitStats stats = stats_for_unit(unit);
    if (pays_experience && unit_hostile(node, stats))
        kill_pays_experience(node, unit, stats, attacker);
}

}  // namespace tl::core
