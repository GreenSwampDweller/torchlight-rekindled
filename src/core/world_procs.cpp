// The gameplay half of render/renderer_procs.cpp (core/world.h).
#include "core/world.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tl::core {

namespace {

/// A placed unit's known list (`CSkillManager +0x60`) as the procs ask of it.
tl::core::SkillCopies known_list(const tl::core::LevelScript::UnitCasting& casting,
                                 bool one_per_name = false) {
    tl::core::SkillCopies known(one_per_name);
    for (const tl::core::LevelScript::UnitSkill& held : casting.skills) {
        tl::core::SkillCopy copy;
        copy.name = held.known.entry.name;
        copy.level = held.known.invested;
        copy.enabled = held.known.enabled;
        copy.hidden = held.known.def != nullptr && held.known.def->hidden;
        copy.charges = held.known.charges;
        known.push_back(copy);
    }
    return known;
}

}  // namespace

tl::core::SkillFileLookup World::skill_file_lookup() {
    ensure_player_skill_files();
    return [this](const std::string& name) -> std::optional<tl::core::SkillFileFacts> {
        const auto found = player_skill_files.find(tl::core::upper_ascii(name));
        if (found == player_skill_files.end()) return std::nullopt;
        // `CSkill::CSkill`: `NAME`, the
        // `[LEVELn]` count (`+0xa8`), `HIDDEN` and `CHARGES` (default -1).
        tl::core::SkillFileFacts facts;
        const tl::core::SkillDef& def = found->second->def;
        facts.name = def.name;
        facts.level_count = static_cast<uint32_t>(def.levels.size());
        facts.hidden = def.hidden;
        facts.charges = static_cast<int32_t>(found->second->file.integer_or("CHARGES", -1));
        return facts;
    };
}

// PORT: a level unit as a proc's target (its machine, node and centre).
World::ProcTarget World::proc_target_unit(int64_t node) {
    ProcTarget target;
    if (node == 0) return target;
    tl::core::LevelScript* machine = script_owning(node);
    tl::core::Vec3 feet;
    if (machine == nullptr || !machine->unit_position(node, feet)) return target;
    target.machine = machine;
    target.node = node;
    target.radius = machine->unit_radius(node);
    target.centre = tl::core::Vec3{feet.x, feet.y + target.radius, feet.z};
    target.placed = true;
    return target;
}

void World::update_worn_skill_managers(float delta) {
    // `CInventory::updateSkillManagers` (X360 inline):
    // locations 0..11 in order, the first item at each.
    const std::vector<tl::core::InventorySlot>& slots = player_inventory.slots();
    const auto lookup = [this](const std::string& n) -> const tl::core::KnownSkill* {
        return player_skill(n);
    };
    for (size_t location = 0; location < worn_skill_runners.size(); ++location) {
        const bool worn = location < slots.size() && slots[location].count > 0;
        const std::string item = worn ? slots[location].item.name : std::string();
        if (item != worn_skill_runner_items[location]) {
            worn_skill_runners[location].clear();
            worn_skill_runner_items[location] = item;
        }
        if (!worn || worn_skill_runners[location].entries().empty()) continue;
        worn_skill_runners[location].update(delta, player_cast_context(), lookup);
    }
}

// PORT: the runner that is running a skill: the player's, or one of the worn items'.
tl::core::SkillRunner& World::skill_runner_running(const std::string& name) {
    if (player_skill_runner.is_running(name)) return player_skill_runner;
    for (tl::core::SkillRunner& runner : worn_skill_runners) {
        if (runner.is_running(name)) return runner;
    }
    return player_skill_runner;
}

void World::player_execute_procs(int type, const ProcTarget& target) {
    // CCharacter::executeProcs, with
    // CInventory::executeProcs.
    load_stat_graphs();
    load_unit_types();
    const tl::core::SkillFileLookup files = skill_file_lookup();
    // The worn items' managers, rebuilt with what their `activateEffect`
    // taught them.
    std::array<tl::core::EffectManager, 12> worn_effects;
    std::array<tl::core::SkillCopies, 12> worn_skills;
    std::array<tl::core::ProcHolder, 12> worn{};
    const std::vector<tl::core::InventorySlot>& slots = player_inventory.slots();
    for (size_t location = 0; location < worn.size() && location < slots.size(); ++location) {
        if (slots[location].count <= 0) continue;
        worn_effects[location] =
            tl::core::weapon_proc_effects(slots[location].item, item_effect_context(), stat_graphs,
                                          files, &worn_skills[location]);
        worn[location] = tl::core::ProcHolder{&worn_effects[location], &worn_skills[location]};
    }
    // `getPosition(true)` of the character and of the target (the
    // character's own with none).
    const float body =
        player_stats.radius > 0.0f ? player_stats.radius : tl::core::kDefaultCollisionRadius;
    const tl::core::Vec3 feet = player.position();
    const tl::core::Vec3 me{feet.x, feet.y + body, feet.z};
    const tl::core::Vec3 there = target.any() ? target.centre : me;
    tl::core::execute_procs(type, tl::core::ProcHolder{&player_effects, &player_proc_skills}, worn,
                            stat_graphs, tl::core::volatile_random(),
                            [&](const tl::core::ProcCast& cast) {
                                // Held by value: what the skill puts on the player can teach a
                                // skill, and the list it came from grows.
                                const tl::core::SkillCopy skill = *cast.skill;
                                if (cast.lockout) player_proc_lockout = 1.0f;
                                const tl::core::Vec3 start = cast.from_target ? there : me;
                                char why[48];
                                std::snprintf(why, sizeof why, "effect 0x%x%s", type,
                                              cast.location >= 0 ? " on a worn item" : "");
                                run_proc_skill(skill, start, there, target, why, cast.location);
                            });
}

void World::player_weapon_procs(const ProcTarget& struck) {
    // CCharacter::doWeaponProcs
    if (!has_weapon || !struck.any()) return;
    load_stat_graphs();
    const tl::core::SkillCopies known = tl::core::weapon_known_skills(
        weapon, item_effect_context(), stat_graphs, skill_file_lookup());
    if (known.empty()) return;
    const float body =
        player_stats.radius > 0.0f ? player_stats.radius : tl::core::kDefaultCollisionRadius;
    const tl::core::Vec3 feet = player.position();
    const tl::core::Vec3 point = tl::core::weapon_proc_point(
        tl::core::Vec3{feet.x, feet.y + body, feet.z}, struck.centre, struck.radius);
    for (const tl::core::SkillCopy& copy : known.copies()) {
        run_proc_skill(copy, point, point, struck, "the weapon's proc", attack_location);
    }
}

// PORT: an attacker as a proc's target.
World::ProcTarget World::proc_target_attacker(int64_t attacker) {
    if (attacker == tl::core::LevelScript::kNoAttacker || attacker == 0) return ProcTarget{};
    return proc_target_unit(attacker);
}

void World::unit_activate_effect(tl::core::LevelScript* machine, int64_t node,
                                 tl::core::Effect& effect) {
    // CBaseUnit::activateEffect: the machine's.
    if (machine != nullptr) machine->activate_unit_effect(node, effect);
}

World::ProcTarget World::proc_target_breakable(int index) {
    ProcTarget target;
    if (index < 0 || static_cast<size_t>(index) >= breakables.size()) return target;
    const BreakableWorld& b = breakables[static_cast<size_t>(index)];
    if (b.node == nullptr) return target;
    const ogre::Vector3 p = view_position(b.node);
    target.breakable_id = b.id;
    // `CBaseUnit::CBaseUnit`: a breakable's
    // `+0x194` is 0.3.
    target.radius = 0.3f;
    target.centre = tl::core::Vec3{p.x, p.y + target.radius, p.z};
    target.placed = true;
    return target;
}

bool World::run_proc_skill(const tl::core::SkillCopy& copy, const tl::core::Vec3& start,
                           const tl::core::Vec3& aim, const ProcTarget& target, const char* why,
                           int location) {
    // CSkillManager::executeSkill, on the
    // manager that holds the copy.
    tl::core::SkillRunner& runner =
        0 <= location && location < static_cast<int>(worn_skill_runners.size())
            ? worn_skill_runners[static_cast<size_t>(location)]
            : player_skill_runner;
    ensure_player_skill_files();
    const auto file = player_skill_files.find(tl::core::upper_ascii(copy.name));
    if (file == player_skill_files.end() || subject_node == nullptr) return false;
    // The copy at the level it holds, where the event code finds the skills
    // the player runs by name (`player_skill`).
    tl::core::KnownSkill& skill = executed_skills[tl::core::upper_ascii(copy.name)];
    skill.entry.name = file->second->def.name;
    skill.def = &file->second->def;
    skill.invested = copy.level;
    skill.effective = copy.level;
    skill.enabled = copy.enabled;

    // `executeSkill(skill, owner, type, start, orientation, aim, target)`: the
    // run keeps `aim` (`+0x50`) and the target; `EVENT_START` is handed both.
    tl::core::SkillCaster caster = player_caster();
    caster.position = start;
    caster.target_position = aim;
    tl::core::SkillCastContext context = player_cast_context();
    context.event_target = tl::core::SkillEventTarget{};
    context.event_target.origin = start;
    context.event_target.yaw = view_orientation(subject_node).getYaw().valueRadians();
    if (target.machine != nullptr) {
        tl::core::SkillTargetUnit aimed;
        aimed.position = target.centre;
        caster.target_unit = aimed;
        context.event_target.has_unit = true;
        context.event_target.unit = target.node;
        context.event_target.machine = target.machine;
    } else if (target.breakable_id != 0) {
        context.event_target.item = target.breakable_id;
    }
    const bool started = runner.execute(skill, copy.level, caster, context);
    TL_LOG_INFO("procs: %s casts '%s' at level %u%s%s", why, skill.entry.name.c_str(), copy.level,
                started ? "" : " (not started)",
                &runner != &player_skill_runner ? " on the worn item's manager" : "");
    return started;
}

void World::unit_execute_procs(tl::core::LevelScript& machine, int64_t node, int type,
                               tl::core::LevelScript* target_machine, int64_t target_node,
                               bool has_target) {
    // CCharacter::executeProcs
    tl::core::LevelScript::UnitCasting* k = machine.unit_casting(node);
    const tl::core::EffectManager* effects = machine.unit_effects(node);
    if (k == nullptr || effects == nullptr || !effects->has_effect(type)) return;
    load_stat_graphs();
    const tl::core::SkillCopies known = known_list(*k);
    // `getPosition(true)` of the unit, and of the target (the unit's own with
    // none).
    tl::core::Vec3 feet;
    if (!machine.unit_position(node, feet)) return;
    const tl::core::Vec3 me{feet.x, feet.y + machine.unit_radius(node), feet.z};
    tl::core::Vec3 there = me;
    if (has_target) {
        if (target_machine == nullptr) {
            const float body = player_stats.radius > 0.0f ? player_stats.radius
                                                          : tl::core::kDefaultCollisionRadius;
            there = {player.position().x, player.position().y + body, player.position().z};
        } else if (target_machine->unit_position(target_node, there)) {
            there.y += target_machine->unit_radius(target_node);
        } else {
            has_target = false;
        }
    }
    float yaw = 0.0f;
    machine.unit_facing(node, yaw);
    const std::string unit = machine.unit_of(node);
    // INVENTED: CInventory::executeProcs not walked for a
    // placed unit; its weapons are kept by name only.
    const std::array<tl::core::ProcHolder, 12> worn{};
    tl::core::execute_procs(
        type, tl::core::ProcHolder{effects, &known}, worn, stat_graphs, tl::core::volatile_random(),
        [&](const tl::core::ProcCast& cast) {
            if (cast.lockout) machine.lock_unit_procs(node);
            // The copy `getSkill` found, held by value: the cast can teach the
            // unit more skills, and its list grows.
            tl::core::LevelScript::UnitCasting* casting = machine.unit_casting(node);
            if (casting == nullptr) return;
            const std::string key = tl::core::upper_ascii(cast.skill->name);
            const uint32_t level = cast.skill->level;
            tl::core::KnownSkill skill;
            for (const tl::core::LevelScript::UnitSkill& held : casting->skills) {
                if (tl::core::upper_ascii(held.known.entry.name) == key &&
                    held.known.invested == level) {
                    skill = held.known;
                    break;
                }
            }
            if (skill.def == nullptr) return;
            const tl::core::Vec3 start = cast.from_target ? there : me;
            // executeSkill(skill, this, 1, ...). A proc can
            // land mid-way through another unit's cast: unit_cast_caster is restored after.
            const tl::core::SkillCaster outer_caster = unit_cast_caster;
            tl::core::SkillCastContext context =
                unit_cast_context(machine, node, target_machine, target_node, has_target);
            unit_cast_caster.position = start;
            unit_cast_caster.target_position = there;
            context.event_target.origin = start;
            context.event_target.yaw = yaw;
            const UnitEventCaster saved = unit_event_caster;
            unit_event_caster = unit_event_caster_for(&machine, node);
            const bool started = casting->runner.execute(skill, level, unit_cast_caster, context);
            unit_event_caster = saved;
            // The count `decrementSkillCharges` took is the held skill's.
            if (tl::core::LevelScript::UnitCasting* after = machine.unit_casting(node)) {
                for (tl::core::LevelScript::UnitSkill& held : after->skills) {
                    if (tl::core::upper_ascii(held.known.entry.name) == key &&
                        held.known.invested == level) {
                        held.known.charges = skill.charges;
                        break;
                    }
                }
            }
            unit_cast_caster = outer_caster;
            TL_LOG_INFO("procs: %s's effect 0x%x casts '%s' at level %u%s", unit.c_str(), type,
                        skill.entry.name.c_str(), level, started ? "" : " (not started)");
        });
}

bool World::skill_event_effects_on(const SkillEventSource& src, tl::core::LevelScript* machine,
                                   int64_t node) {
    if (src.event == nullptr) return false;
    // Out of sight: nothing is applied, and the call still answers true.
    if (skill_event_sight_blocked(src, machine, node)) return true;
    if (machine == nullptr) {
        // `applyEffects` on the player: its own manager.
        tl::core::SkillCastContext context = player_cast_context();
        if (src.caster_machine != nullptr || src.prop_owner) {
            // A placed unit's or a prop's event: the player is its target, not its caster.
            context.target_is_caster = false;
            // A prop is no character: `no_caster_character` below passes every filter.
            context.target_is_enemy =
                src.caster_machine != nullptr &&
                src.caster_machine->unit_is_enemy_of(src.caster_node, nullptr, 0);
            context.target_is_casters_pet = false;
        }
        // `addListenerToUnit` from `applyAffixesAndEffects`: the skill watches the player.
        context.observe_target = [this, &src](const std::string& skill) {
            unit_observers.add({nullptr, 0}, skill_observer_for(src.caster_machine, src.caster_node,
                                                                skill, src.run));
        };
        // `setOwner(effect, caster)`: the event's caster owns what lands.
        context.source = tl::core::EffectSource{
            true, src.caster_machine, src.caster_machine != nullptr ? src.caster_node : 0};
        context.owner_level = event_caster_level(src.caster_machine, src.caster_node);
        const tl::core::EffectRolls ownerless{&stat_graphs, &tl::core::volatile_random(), {}};
        if (src.prop_owner) {
            context.source = tl::core::EffectSource{};
            context.owner_level = src.prop_level;
            context.no_caster_character = true;
            context.observe_target = nullptr;
            context.ownerless_rolls = &ownerless;
            context.ownerless_owned = &prop_effect_owners[{src.prop_id, src.event}];
        }
        tl::core::LevelScript* const saved_machine = instant_caster_machine;
        const int64_t saved_node = instant_caster_node;
        const bool saved_none = instant_caster_none;
        instant_caster_machine = src.caster_machine;
        instant_caster_node = src.caster_node;
        instant_caster_none = src.prop_owner;
        const bool on_player = tl::core::apply_event_effects(
            *src.event, context, src.skill, src.property, src.unauthored, src.duration_override);
        // `CCharacter::updateEffects(target, 0)`: a status laid becomes its affix
        // at once. Its visuals wait for the frame's update.
        player_effects.update_effects(0.0f);
        convert_held_statuses(player_health.dead ? tl::core::ai_state::kDead : 0, &player_effects,
                              "the player");
        instant_caster_machine = saved_machine;
        instant_caster_node = saved_node;
        instant_caster_none = saved_none;
        return on_player;
    }
    // `applyEffects` on a placed character, through its
    // `applyEffectOnUnit` (`unit_effect_on_unit`).
    const tl::core::UnitStats::Alignment side =
        machine->unit_side(node).value_or(tl::core::UnitStats::Alignment::Neutral);
    tl::core::EffectManager* manager = unit_effect_manager(machine, node);
    if (manager == nullptr) return false;
    tl::core::SkillCastContext context;
    context.target = manager;
    context.types = &effect_types;
    context.unit_types = &unit_types;
    context.affix = [this](const std::string& name) -> const tl::core::Affix* {
        return affixes.get_affix(name);
    };
    context.target_is_caster = false;
    context.target_is_enemy = side == tl::core::UnitStats::Alignment::Hostile;
    context.target_is_casters_pet = machine->unit_is_player_pet(node);
    // `addListenerToUnit` from `applyAffixesAndEffects`: the skill watches the unit.
    context.observe_target = [this, &src, machine, node](const std::string& skill) {
        unit_observers.add({machine, node},
                           skill_observer_for(src.caster_machine, src.caster_node, skill, src.run));
    };
    // `setOwner(effect, caster)`: the event's caster owns what lands.
    context.source = tl::core::EffectSource{true, src.caster_machine,
                                            src.caster_machine != nullptr ? src.caster_node : 0};
    context.owner_level = event_caster_level(src.caster_machine, src.caster_node);
    const tl::core::EffectRolls ownerless{&stat_graphs, &tl::core::volatile_random(), {}};
    if (src.prop_owner) {
        context.source = tl::core::EffectSource{};
        context.owner_level = src.prop_level;
        context.no_caster_character = true;
        context.observe_target = nullptr;
        context.ownerless_rolls = &ownerless;
        context.ownerless_owned = &prop_effect_owners[{src.prop_id, src.event}];
    } else if (src.caster_machine != nullptr) {
        // From the caster's side (`CCharacter::isEnemy`).
        // INVENTED: a placed unit's own pets are not known.
        context.target_is_caster = machine == src.caster_machine && node == src.caster_node;
        context.target_is_enemy = !context.target_is_caster && src.caster_machine->unit_is_enemy_of(
                                                                   src.caster_node, machine, node);
        context.target_is_casters_pet = false;
    }
    tl::core::LevelScript* const saved_machine = instant_caster_machine;
    const int64_t saved_node = instant_caster_node;
    const bool saved_none = instant_caster_none;
    instant_caster_machine = src.caster_machine;
    instant_caster_node = src.caster_node;
    instant_caster_none = src.prop_owner;
    // The direction `applyEffects` hands on. INVENTED:
    // caster to target; the original's is from the event's object (`startEvent`),
    // a shape's (`characterEffectedByDamageShape`)
    // or the missile's (`missileApplyingEffects`).
    const bool saved_set = effect_direction_set;
    const std::array<float, 3> saved_direction = effect_direction;
    {
        tl::core::Vec3 from;
        tl::core::Vec3 to;
        bool known = machine->unit_position(node, to);
        if (src.caster_machine != nullptr) {
            known = known && src.caster_machine->unit_position(src.caster_node, from);
        } else {
            from = player.position();
        }
        const float dx = to.x - from.x;
        const float dy = to.y - from.y;
        const float dz = to.z - from.z;
        const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
        effect_direction_set = known && 1e-08f < length;
        if (effect_direction_set) effect_direction = {dx / length, dy / length, dz / length};
    }
    // `alive()`: a body with hit points after gets up (`HP()`,
    // `setAIState(2)`).
    const bool was_alive = !machine->unit_is_dead(node);
    const bool applied = tl::core::apply_event_effects(*src.event, context, src.skill, src.property,
                                                       src.unauthored, src.duration_override);
    effect_direction_set = saved_set;
    effect_direction = saved_direction;
    instant_caster_machine = saved_machine;
    instant_caster_node = saved_node;
    instant_caster_none = saved_none;
    std::vector<tl::core::ScriptEvent> events;
    // `updateEffects(target, 0.0f)` straight: a status lands as its
    // affix now, not on the unit's next update.
    unit_update_effects(machine, node, 0.0f, events);
    if (machine->unit_gets_up(node, was_alive, quest_log.state(), events)) {
        TL_LOG_INFO("skill event: %s gets up (%s)", machine->unit_of(node).c_str(),
                    src.skill.c_str());
    }
    if (NodeMap* nodes = nodes_of(machine); nodes != nullptr && !events.empty()) {
        apply_script_events(events, *nodes);
    }
    return applied;
}

}  // namespace tl::core
