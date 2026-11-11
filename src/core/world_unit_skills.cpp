// The gameplay half of render/renderer_unit_skills.cpp (core/world.h).
#include "core/unit_skills_reader.h"
#include "core/world.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tl::core {

std::vector<tl::core::LevelScript::UnitSkill> World::unit_skill_list(const std::string& unit) {
    std::vector<tl::core::LevelScript::UnitSkill> out;
    ensure_monster_index();
    const auto found = monster_files.find(lowered(unit));
    if (found == monster_files.end()) return out;
    ensure_player_skill_files();
    // The unit with its BASEFILE merged under it:
    // the base's [SKILL] blocks come first.
    const tl::formats::AdmFile file = read_unit_file(found->second);
    if (!file.ok) return out;
    out = tl::core::read_unit_skills(
        file,
        [this](const std::string& upper_name) -> const tl::core::SkillDef* {
            const auto skill = player_skill_files.find(upper_name);
            return skill == player_skill_files.end() ? nullptr : &skill->second->def;
        },
        [&unit](const std::string& name) {
            // `addSkillByName` found nothing: the original logs it and moves on.
            TL_LOG_WARN("skills: Skill not found : %s (%s)", name.c_str(), unit.c_str());
        });
    if (!out.empty()) {
        std::string names;
        for (const auto& s : out) {
            names += (names.empty() ? "" : ", ") + s.known.entry.name + " (" +
                     std::to_string(s.cast_chance) + "%)";
        }
        TL_LOG_INFO("skills: %s holds %zu skill(s): %s", unit.c_str(), out.size(), names.c_str());
    }
    return out;
}

// PORT: a mesh's animation catalogue, read once and kept.
const tl::core::AnimationCatalog* World::unit_catalog(const std::string& mesh) {
    if (mesh.empty()) return nullptr;
    const auto cached = unit_catalogs.find(mesh);
    if (cached != unit_catalogs.end()) return &cached->second;
    return &unit_catalogs.emplace(mesh, model_catalog(mesh, dir_of_asset(mesh))).first->second;
}

// PORT: a unit's known skill found by name in its casting list.
const tl::core::KnownSkill* World::unit_skill_named(tl::core::LevelScript& machine, int64_t node,
                                                    const std::string& name) {
    tl::core::LevelScript::UnitCasting* k = machine.unit_casting(node);
    if (k == nullptr) return nullptr;
    for (const tl::core::LevelScript::UnitSkill& s : k->skills) {
        if (s.known.entry.name == name) return &s.known;
    }
    return nullptr;
}

void World::aim_unit_position_skill(tl::core::LevelScript& machine, int64_t node,
                                    const tl::core::KnownSkill& skill) {
    // `CCharacter::castSkill`: a skill aimed at a place
    // (`POSITION`, 0xb, `POSITIONRANDOM`, `POSITIONFLEE`) is cast at the point
    // the level machine's `cast_skill` left as the final target position.
    const tl::core::SkillProperty* property = skill.property(skill.effective);
    if (property == nullptr) return;
    const int32_t type = property->target_type;
    if (type != 1 && type != 0xb && type != 5 && type != 6) return;
    tl::core::Vec3 at;
    if (machine.unit_final_target(node, at)) unit_cast_caster.target_position = at;
}

tl::core::SkillCaster World::unit_caster(tl::core::LevelScript& machine, int64_t node,
                                         tl::core::LevelScript* target_machine, int64_t target_node,
                                         bool has_target) {
    tl::core::SkillCaster caster;
    if (const tl::core::LevelScript::UnitCasting* k = machine.unit_casting(node)) {
        caster.mana = k->mana;
    }
    load_unit_types();
    caster.unit_types = &unit_types;
    // `CBaseUnit::ISA(0x1b)`: `MONSTERCOOLDOWNMS`.
    caster.is_monster = true;
    caster.is_character = true;
    caster.effects = machine.unit_effects(node);
    // `getWeaponInRightHand` / `InLeftHand`: what the unit was given to hold.
    std::string right_hand, left_hand;
    machine.unit_hands(node, right_hand, left_hand);
    ensure_item_index();
    if (!right_hand.empty()) {
        if (const tl::core::ItemStats* item = items.find(right_hand)) {
            caster.right_hand_type = item->unittype;
        }
    }
    if (!left_hand.empty()) {
        if (const tl::core::ItemStats* item = items.find(left_hand)) {
            caster.left_hand_type = item->unittype;
        }
    }
    if (SceneUnit* drawn = scene_unit_of(&machine, node)) caster.model = unit_catalog(drawn->mesh);
    // `getPosition(true)`: the sphere's centre.
    tl::core::Vec3 feet;
    if (machine.unit_position(node, feet)) {
        caster.position = {feet.x, feet.y + machine.unit_radius(node), feet.z};
    }
    if (has_target) {
        tl::core::Vec3 at;
        bool got = false;
        if (target_machine == nullptr) {
            const float body = player_stats.radius > 0.0f ? player_stats.radius
                                                          : tl::core::kDefaultCollisionRadius;
            at = {player.position().x, player.position().y + body, player.position().z};
            got = true;
        } else if (target_machine->unit_position(target_node, at)) {
            at.y += target_machine->unit_radius(target_node);
            got = true;
        }
        if (got) {
            tl::core::SkillTargetUnit target;
            target.position = at;
            caster.target_unit = target;
            caster.target_position = at;
        }
    }
    // `CLevel::passableBetween` on the map
    // grid. INVENTED: the caster and the target are not taken out of the object
    // grid first.
    caster.passable_between = [this](const tl::core::Vec3& from, const tl::core::Vec3& to) {
        return !passability.ready() || passability.passable_between(from, to, true);
    };
    return caster;
}

// PORT: the caster a unit's skill events are handed (its scene node and radius).
World::UnitEventCaster World::unit_event_caster_for(tl::core::LevelScript* machine, int64_t node) {
    UnitEventCaster caster;
    caster.machine = machine;
    caster.node = node;
    if (NodeMap* nodes = nodes_of(machine)) {
        const auto found = nodes->find(node);
        if (found != nodes->end()) caster.scene = found->second;
    }
    const float radius = machine->unit_radius(node);
    caster.radius = radius > 0.0f ? radius : tl::core::kDefaultCollisionRadius;
    return caster;
}

// PORT: the context `SkillRunner::cast` reads for a unit's skill: the caster's
// own effect manager, the tables, and the event hooks into the renderer.
// `setOwner(effect, caster)`: the casting unit owns what lands.
tl::core::SkillCastContext World::unit_cast_context(tl::core::LevelScript& machine, int64_t node,
                                                    tl::core::LevelScript* target_machine,
                                                    int64_t target_node, bool has_target) {
    ensure_effect_data();
    tl::core::SkillCastContext context;
    // A self-buff lands on the caster's own manager.
    context.target = unit_effect_manager(&machine, node);
    context.types = &effect_types;
    context.unit_types = &unit_types;
    context.affix = [this](const std::string& name) -> const tl::core::Affix* {
        return affixes.get_affix(name);
    };
    context.target_is_caster = true;
    context.target_is_enemy = false;
    context.target_is_casters_pet = false;
    // `setOwner(effect, caster)`: the casting unit owns what lands.
    context.source = tl::core::EffectSource{true, &machine, node};
    context.owner_level = event_caster_level(&machine, node);
    unit_cast_caster = unit_caster(machine, node, target_machine, target_node, has_target);
    context.caster = &unit_cast_caster;
    tl::core::LevelScript* const owner = &machine;
    context.on_event = [this, owner, node](const tl::core::KnownSkill& skill, uint32_t level,
                                           int group, const tl::formats::AdmSection& event,
                                           const tl::core::SkillEventTarget& target) {
        const UnitEventCaster saved = unit_event_caster;
        unit_event_caster = unit_event_caster_for(owner, node);
        start_skill_event(skill, level, group, event, target);
        unit_event_caster = saved;
    };
    context.events_alive = [this, owner, node](const std::string& skill, uint32_t run) {
        return skill_events_alive(skill, run, owner, node);
    };
    context.events_can_stop = [this, owner, node](const std::string& skill, uint32_t run) {
        return skill_events_can_stop(skill, run, owner, node);
    };
    context.reset_events = [this, owner, node](const std::string& skill, uint32_t run) {
        reset_skill_events(skill, run, owner, node);
    };
    context.reset_event = [this, owner, node](const std::string& skill, uint32_t run,
                                              const tl::formats::AdmSection& block) {
        detach_event_missiles(skill, run, block, owner, node);
    };
    if (has_target) {
        // A null machine with a unit is the player.
        context.event_target.has_unit = true;
        context.event_target.unit = target_machine != nullptr ? target_node : 0;
        context.event_target.machine = target_machine;
    }
    return context;
}

// PORT: unit_cast_context()'s lasting work, in its order, for a frame that reads no context.
void World::prepare_unit_cast(tl::core::LevelScript& machine, int64_t node) {
    ensure_effect_data();
    (void)unit_effect_manager(&machine, node);
    load_unit_types();
    ensure_item_index();
    if (SceneUnit* drawn = scene_unit_of(&machine, node)) (void)unit_catalog(drawn->mesh);
}

// `CCharacter::castSkill` for a level unit:
// the runner's cast (`SkillRunner::cast`), the mana spent, and the cast's
// animation cue played on the unit's model.
bool World::cast_unit_skill(tl::core::LevelScript& machine, int64_t node,
                            tl::core::LevelScript* target_machine, int64_t target_node,
                            bool has_target) {
    tl::core::LevelScript::UnitCasting* k = machine.unit_casting(node);
    if (k == nullptr || k->active < 0 || k->active >= static_cast<int>(k->skills.size())) {
        return false;
    }
    tl::core::LevelScript::UnitSkill& s = k->skills[static_cast<size_t>(k->active)];
    if (s.known.def == nullptr) return false;
    const std::string name = s.known.entry.name;
    const std::string unit = machine.unit_of(node);

    tl::core::SkillCastContext context =
        unit_cast_context(machine, node, target_machine, target_node, has_target);
    // `performSkill`: the target, else the target item (+0x28c).
    if (const int64_t item = machine.unit_target_item(node); !has_target && item != 0) {
        for (const BreakableWorld& b : breakables) {
            if (b.id != item || b.node == nullptr) continue;
            const ogre::Vector3 p = view_position(b.node);
            tl::core::SkillTargetUnit aimed;
            aimed.position = {p.x, p.y, p.z};
            aimed.is_item = true;
            unit_cast_caster.target_unit = aimed;
            context.event_target.item = item;
            break;
        }
    }
    aim_unit_position_skill(machine, node, s.known);
    float spent = 0.0f;
    const UnitEventCaster saved = unit_event_caster;
    unit_event_caster = unit_event_caster_for(&machine, node);
    const tl::core::SkillCastResult result =
        k->runner.cast(s.known, unit_cast_caster, context, &spent);
    unit_event_caster = saved;
    if (result != tl::core::SkillCastResult::Cast) {
        TL_LOG_INFO("skills: %s does not cast '%s': %s", unit.c_str(), name.c_str(),
                    tl::core::skill_cast_result_name(result));
        return false;
    }
    // `modifyMana(-getManaCost())`.
    k->mana = std::max(0.0f, k->mana - spent);

    // The animation: `castSkill`'s cue, played on the unit's own model.
    tl::core::SkillRunState* running = k->runner.state(name);
    if (running != nullptr) {
        const tl::core::SkillAnimationCue cue = tl::core::begin_skill_animation(
            k->cast, name, *s.known.current_property(), unit_cast_caster, *running);
        const tl::core::AnimationCatalog* catalog = unit_cast_caster.model;
        SceneUnit* drawn = scene_unit_of(&machine, node);
        if (catalog != nullptr && drawn != nullptr && 0 <= cue.animation &&
            static_cast<size_t>(cue.animation) < catalog->size()) {
            const auto& entry = (*catalog)[static_cast<size_t>(cue.animation)];
            Ogre::AnimationState* started = nullptr;
            // `blendAnimation(animation, .., 0.2, speed, duration)`, through the
            // unit's animator.
            if (play_scene_animation(drawn->entity, drawn->mesh, entry.first, cue.duration < 0.0f,
                                     drawn->rate * cue.speed, /*optional=*/true, &started,
                                     drawn->armed, nullptr, /*blend=*/0.2f)) {
                remember_state(drawn->entity, entry.first, started);
                drawn->playing = entry.first;
                drawn->playing_state = started;
                drawn->last_time = 0.0f;
                unit_skill_playbacks.erase(
                    std::remove_if(unit_skill_playbacks.begin(), unit_skill_playbacks.end(),
                                   [&](const UnitSkillPlayback& p) {
                                       return p.machine == &machine && p.node == node;
                                   }),
                    unit_skill_playbacks.end());
                UnitSkillPlayback playback;
                playback.machine = &machine;
                playback.node = node;
                playback.animation = entry.first;
                playback.index = cue.animation;
                playback.length = entry.second;
                playback.speed = cue.speed;
                playback.held = cue.duration;
                playback.then_loop = cue.then_loop;
                unit_skill_playbacks.push_back(playback);
            }
        }
    }
    TL_LOG_INFO("skills: %s casts '%s' at level %u%s", unit.c_str(), name.c_str(),
                s.known.effective,
                !has_target                 ? ""
                : target_machine == nullptr ? ", at the player"
                                            : ", at a unit");
    return true;
}

void World::fire_unit_skills_on_create(tl::core::LevelScript& machine, int64_t node) {
    // CSkillManager::fireSkillsOnCreate, from
    // CCharacter::unitInit: every EVENT_UNIT_CREATE skill
    // (group 10) executed without a target, then group 10 fired.
    machine.ensure_unit_skills(node);
    tl::core::LevelScript::UnitCasting* k = machine.unit_casting(node);
    if (k == nullptr || k->skills.empty()) return;
    // Once per unit: a spawner's release fires them as the unit is made, the renderer's first step
    // of the unit's skills otherwise.
    if (k->create_fired) return;
    k->create_fired = true;
    const auto has_unit_create = [](const tl::core::KnownSkill& skill) {
        const tl::core::SkillProperty* property = skill.property(skill.effective);
        if (property == nullptr || property->section == nullptr) return false;
        for (const tl::formats::AdmSection& child : property->event_section().children) {
            if (tl::core::upper_ascii(child.name) == "EVENT_UNIT_CREATE") return true;
        }
        return false;
    };
    bool any = false;
    size_t undefined = 0;
    for (const tl::core::LevelScript::UnitSkill& s : k->skills) {
        if (s.known.def == nullptr) ++undefined;
        if (s.known.def != nullptr && has_unit_create(s.known)) any = true;
    }
    TL_LOG_DEBUG("skills: %s is made with %zu skill(s), %zu undefined, create events %s",
                 machine.unit_of(node).c_str(), k->skills.size(), undefined, any ? "yes" : "no");
    if (!any) return;
    tl::core::SkillCastContext context = unit_cast_context(machine, node, nullptr, 0, false);
    const auto lookup = [this, &machine, node](const std::string& n) {
        return unit_skill_named(machine, node, n);
    };
    const UnitEventCaster saved = unit_event_caster;
    unit_event_caster = unit_event_caster_for(&machine, node);
    for (const tl::core::LevelScript::UnitSkill& s : k->skills) {
        if (s.known.def == nullptr || !has_unit_create(s.known)) continue;
        if (k->runner.execute(s.known, s.known.effective, unit_cast_caster, context)) {
            TL_LOG_INFO("skills: %s is made and starts '%s'", machine.unit_of(node).c_str(),
                        s.known.entry.name.c_str());
        } else {
            TL_LOG_DEBUG("skills: %s is made but '%s' did not start", machine.unit_of(node).c_str(),
                         s.known.entry.name.c_str());
        }
    }
    constexpr int kUnitCreate = 10;
    k->runner.trigger_event(kUnitCreate, context, lookup);
    unit_event_caster = saved;
}

// PORT: the hooks a level's machines call for their units' skills, into the renderer.
tl::core::LevelScript::UnitSkills World::unit_skill_hooks() {
    tl::core::LevelScript::UnitSkills hooks;
    hooks.load = [this](const std::string& unit) { return unit_skill_list(unit); };
    hooks.skill_files = skill_file_lookup();
    hooks.skill_def = [this](const std::string& upper_name) -> const tl::core::SkillDef* {
        ensure_player_skill_files();
        const auto found = player_skill_files.find(upper_name);
        return found == player_skill_files.end() ? nullptr : &found->second->def;
    };
    hooks.layout_adds_sample = [this](const std::string& file) {
        ensure_sound_index();
        const tl::formats::CmpScene* scene = layout_file(file);
        return scene != nullptr && tl::core::layout_adds_sound_sample(*scene, sound_info);
    };
    hooks.load_event_file = [this](const std::string& file) { count_layout_load(file); };
    hooks.shape_hit = [this](tl::core::LevelScript& machine, int64_t node, float amount,
                             int32_t damage_type, const tl::core::Vec3& direction) {
        shape_hit_unit(machine, node, amount, damage_type, direction);
    };
    hooks.procs = [this](tl::core::LevelScript& machine, int64_t node, int type,
                         tl::core::LevelScript* target_machine, int64_t target_node,
                         bool has_target) {
        unit_execute_procs(machine, node, type, target_machine, target_node, has_target);
    };
    hooks.caster = [this](tl::core::LevelScript& machine, int64_t node,
                          tl::core::LevelScript* target_machine, int64_t target_node,
                          bool has_target) {
        return unit_caster(machine, node, target_machine, target_node, has_target);
    };
    hooks.cast = [this](tl::core::LevelScript& machine, int64_t node,
                        tl::core::LevelScript* target_machine, int64_t target_node,
                        bool has_target) {
        return cast_unit_skill(machine, node, target_machine, target_node, has_target);
    };
    hooks.stop_all = [this](tl::core::LevelScript& machine, int64_t node, bool force) {
        tl::core::LevelScript::UnitCasting* k = machine.unit_casting(node);
        if (k == nullptr) return;
        tl::core::SkillCastContext context = unit_cast_context(machine, node, nullptr, 0, false);
        const UnitEventCaster saved = unit_event_caster;
        unit_event_caster = unit_event_caster_for(&machine, node);
        k->runner.stop_all(
            context,
            [this, &machine, node](const std::string& n) {
                return unit_skill_named(machine, node, n);
            },
            force, /*passives=*/false);
        unit_event_caster = saved;
    };
    // `CLevel::findItemWithinView` from `castSkill`'s frame,
    // newest first.
    hooks.item_within_view = [this](tl::core::LevelScript&, int64_t, const tl::core::Vec3& at,
                                    const tl::core::Vec3& forward, float angle,
                                    float reach) -> int64_t {
        const tl::core::ViewFrame frame = tl::core::view_frame_along(at, forward);
        for (size_t i = breakables.size(); i-- > 0;) {
            const BreakableWorld& b = breakables[i];
            if (b.node == nullptr) continue;
            if (!tl::core::breakable_candidate(b.state, b.entity != nullptr && b.presence.visible))
                continue;
            const ogre::Vector3 p = view_position(b.node);
            if (!tl::core::item_within_view(frame, {p.x, p.y, p.z}, angle, reach)) continue;
            // `sightUnobstructed(from, to, false)`, both ends 5 up, the objects tested too.
            tl::core::CollisionHit hit;
            if (collision.ray_collision({at.x, at.y + 5.0f, at.z}, {p.x, p.y + 5.0f, p.z}, true,
                                        hit))
                continue;
            return b.id;
        }
        return 0;
    };
    hooks.item_position = [this](int64_t item, tl::core::Vec3& at) {
        for (const BreakableWorld& b : breakables) {
            if (b.id != item || b.node == nullptr) continue;
            const ogre::Vector3 p = view_position(b.node);
            at = {p.x, p.y, p.z};
            return true;
        }
        return false;
    };
    hooks.stop_by_name = [this](tl::core::LevelScript& machine, int64_t node,
                                const std::string& name) {
        stop_unit_skills_by_name(machine, node, name);
    };
    load_recharge_rates();
    hooks.mana_recharge_rate = recharge_rates.mana;
    return hooks;
}

// PORT: where a unit stands and faces, for `position_for_skill`: the player's
// from its OGRE node, a machine unit's from the machine.
bool World::skill_position_unit(tl::core::LevelScript* machine, int64_t node,
                                tl::core::SkillPositionUnit& out) {
    // A null machine is the player.
    if (machine == nullptr) {
        if (subject_node == nullptr) return false;
        out.radius =
            player_stats.radius > 0.0f ? player_stats.radius : tl::core::kDefaultCollisionRadius;
        out.centre = {player.position().x, player.position().y + out.radius, player.position().z};
        const ogre::Vector3 forward = view_orientation(subject_node) * ogre::Vector3::UNIT_Z;
        out.forward = {forward.x, forward.y, forward.z};
        return true;
    }
    tl::core::Vec3 feet;
    if (!machine->unit_position(node, feet)) return false;
    out.radius = machine->unit_radius(node);
    out.centre = {feet.x, feet.y + out.radius, feet.z};
    float yaw = 0.0f;
    machine->unit_facing(node, yaw);
    out.forward = {std::sin(yaw), 0.0f, std::cos(yaw)};
    return true;
}

void World::fire_unit_skills_on_death(tl::core::LevelScript& machine, int64_t node) {
    // CCharacter::die: stopAllSkills(true, true, false)
    // spares EVENT_CASTERDIE and DONT_STOP_ON_DEATH;
    // then CSkillManager::fireSkillsOnDeath executes
    // every EVENT_CASTERDIE skill (group 9, CSkill::hasEvent) and fires group 9.
    tl::core::LevelScript::UnitCasting* k = machine.unit_casting(node);
    if (k == nullptr || k->skills.empty()) return;
    const auto has_caster_die = [](const tl::core::KnownSkill& skill) {
        const tl::core::SkillProperty* property = skill.property(skill.effective);
        if (property == nullptr || property->section == nullptr) return false;
        for (const tl::formats::AdmSection& child : property->event_section().children) {
            if (tl::core::upper_ascii(child.name) == "EVENT_CASTERDIE") return true;
        }
        return false;
    };
    tl::core::LevelScript* target_machine = nullptr;
    int64_t target_node = 0;
    const bool has_target = machine.unit_target(node, target_machine, target_node);
    tl::core::SkillCastContext context =
        unit_cast_context(machine, node, target_machine, target_node, has_target);
    const auto lookup = [this, &machine, node](const std::string& n) {
        return unit_skill_named(machine, node, n);
    };
    const UnitEventCaster saved = unit_event_caster;
    unit_event_caster = unit_event_caster_for(&machine, node);
    k->runner.stop_all(context, lookup, /*force=*/true, /*passives=*/false,
                       /*keep_on_death=*/true);
    for (const tl::core::LevelScript::UnitSkill& s : k->skills) {
        if (s.known.def == nullptr || !has_caster_die(s.known)) continue;
        const tl::core::SkillProperty* property = s.known.property(s.known.effective);
        const int32_t type = property != nullptr ? property->target_type : 0;
        const std::optional<tl::core::Vec3> aimed_at = unit_cast_caster.target_position;
        if (type == 1 || type == 5 || type == 6) {
            // getPositionForSkill(skill, null), no unit
            // - for every later skill too (the argument is cleared).
            // INVENTED: a never-started skill (CSkill +0x50, written by startSkill
            // and castSkill) uses the origin.
            context.event_target.has_unit = false;
            context.event_target.unit = 0;
            context.event_target.machine = nullptr;
            tl::core::Vec3 point{0.0f, 0.0f, 0.0f};
            if (const tl::core::SkillRunState* state = k->runner.state(s.known.entry.name)) {
                if (state->target_position.has_value()) point = *state->target_position;
            }
            tl::core::SkillPositionUnit owner;
            if (property != nullptr && skill_position_unit(&machine, node, owner)) {
                const std::optional<tl::core::Vec3> at = tl::core::position_for_skill(
                    *property, point, &owner, nullptr, tl::core::volatile_random());
                if (at.has_value()) {
                    unit_cast_caster.target_position = *at;
                    TL_LOG_INFO("skills: %s's '%s' is aimed at (%.2f, %.2f, %.2f)",
                                machine.unit_of(node).c_str(), s.known.entry.name.c_str(),
                                static_cast<double>(at->x), static_cast<double>(at->y),
                                static_cast<double>(at->z));
                }
            }
        }
        const tl::core::SkillCastContext started = context;
        if (k->runner.execute(s.known, s.known.effective, unit_cast_caster, started)) {
            TL_LOG_INFO("skills: %s dies and starts '%s'", machine.unit_of(node).c_str(),
                        s.known.entry.name.c_str());
        }
        unit_cast_caster.target_position = aimed_at;
    }
    constexpr int kCasterDie = 9;
    k->runner.trigger_event(kCasterDie, context, lookup);
    unit_event_caster = saved;
}

// `CSkillManager::stopSkillsByName(name, true)` on a placed
// character's manager.
void World::stop_unit_skills_by_name(tl::core::LevelScript& machine, int64_t node,
                                     const std::string& name) {
    tl::core::LevelScript::UnitCasting* k = machine.unit_casting(node);
    if (k == nullptr) return;
    tl::core::SkillCastContext context = unit_cast_context(machine, node, nullptr, 0, false);
    const UnitEventCaster saved = unit_event_caster;
    unit_event_caster = unit_event_caster_for(&machine, node);
    k->runner.stop_by_name(name, context, [this, &machine, node](const std::string& n) {
        return unit_skill_named(machine, node, n);
    });
    unit_event_caster = saved;
}

}  // namespace tl::core
