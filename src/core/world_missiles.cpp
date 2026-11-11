// The missiles of the gameplay half of render/renderer_effects.cpp (core/world.h).
#include "core/section_profile.h"
#include "core/world.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace tl::core {

namespace {

/// The level's characters a missile can meet, gathered across every machine.
std::vector<tl::core::LevelScript::Reachable> missile_candidates(
    const std::vector<tl::core::LevelScript*>& machines) {
    std::vector<tl::core::LevelScript::Reachable> out;
    for (const tl::core::LevelScript* machine : machines) machine->reachable_characters(out);
    return out;
}

/// The first `t` in [0, 1] at which a sphere of `radius` from `from` to `to` touches a
/// sphere of `other` at `centre`, or a value above 1.
float sphere_sweep_t(const tl::core::Vec3& from, const tl::core::Vec3& to, float radius,
                     const tl::core::Vec3& centre, float other) {
    const tl::core::Vec3 d = to - from;
    const tl::core::Vec3 f = from - centre;
    const float reach = radius + other;
    const float c = f.x * f.x + f.y * f.y + f.z * f.z - reach * reach;
    if (c <= 0.0f) return 0.0f;
    const float a = d.x * d.x + d.y * d.y + d.z * d.z;
    if (!(a > 0.0f)) return 2.0f;
    const float b = 2.0f * (f.x * d.x + f.y * d.y + f.z * d.z);
    const float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f) return 2.0f;
    const float t = (-b - std::sqrt(disc)) / (2.0f * a);
    return t >= 0.0f ? t : 2.0f;
}

/// The first `t` in [0, 1] at which a sphere of `radius` from `from` to `to` touches the
/// box `low`..`high` (grown by the radius, cut slab by slab), or a value above 1.
float box_sweep_t(const tl::core::Vec3& from, const tl::core::Vec3& to, float radius,
                  const tl::core::Vec3& low, const tl::core::Vec3& high) {
    const float start[3] = {from.x, from.y, from.z};
    const float delta[3] = {to.x - from.x, to.y - from.y, to.z - from.z};
    const float lo[3] = {low.x - radius, low.y - radius, low.z - radius};
    const float hi[3] = {high.x + radius, high.y + radius, high.z + radius};
    float enter = 0.0f;
    float leave = 1.0f;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(delta[axis]) < 1e-8f) {
            if (start[axis] < lo[axis] || hi[axis] < start[axis]) return 2.0f;
            continue;
        }
        float a = (lo[axis] - start[axis]) / delta[axis];
        float b = (hi[axis] - start[axis]) / delta[axis];
        if (b < a) std::swap(a, b);
        enter = std::max(enter, a);
        leave = std::min(leave, b);
        if (leave < enter) return 2.0f;
    }
    return enter;
}

/// `CLevel::findCharacterWithinView` as a missile asks it (`CMissile::getClosestTarget`):
/// the nearest enemy of the player's in the cone.
int64_t missile_closest(const std::vector<tl::core::LevelScript::Reachable>& candidates,
                        const tl::core::Vec3& at, const tl::core::Vec3& forward, float angle,
                        float sight) {
    const float facing = std::atan2(forward.x, forward.z);
    int64_t best = 0;
    float best_d = 0.0f;
    for (const auto& u : candidates) {
        if (u.alignment != tl::core::UnitStats::Alignment::Hostile) continue;
        if (u.mimic_idle) continue;  // `findCharacterWithinView` passes it over
        if (!tl::core::can_see(at, facing, u.center, 0.0f, angle, sight)) continue;
        const float dx = u.center.x - at.x;
        const float dz = u.center.z - at.z;
        const float d = std::sqrt(dx * dx + dz * dz);
        if (best == 0 || d < best_d) {
            best = u.id;
            best_d = d;
        }
    }
    return best;
}

bool missile_unit(const std::vector<tl::core::LevelScript::Reachable>& candidates, int64_t id,
                  tl::core::Vec3& c, float& r, bool& alive) {
    for (const auto& u : candidates) {
        if (u.id != id) continue;
        c = u.center;
        r = u.radius;
        alive = true;
        return true;
    }
    return false;
}

template <typename ImplT>
std::vector<tl::core::LevelScript::Reachable> gather_missile_candidates(const ImplT* impl,
                                                                        bool level_placed) {
    std::vector<tl::core::LevelScript::Reachable> pool = missile_candidates(impl->level_machines);
    if (level_placed && !impl->player_health.dead && impl->subject_node != nullptr) {
        const float body = impl->player_stats.radius > 0.0f ? impl->player_stats.radius
                                                            : tl::core::kDefaultCollisionRadius;
        const ogre::Vector3 feet = impl->view_position(impl->subject_node);
        tl::core::LevelScript::Reachable you;
        you.id = kMissilePlayer;
        you.unit = "the player";
        you.center = {feet.x, feet.y + body, feet.z};
        you.radius = body;
        you.alignment = tl::core::UnitStats::Alignment::Friendly;
        pool.push_back(you);
    }
    return pool;
}

/// What a missile can meet, with the player (a level's or a unit's missile) and without: kept
/// from one missile to the next, gathered again where a blow lands or a skill event runs.
struct MissilePools {
    std::vector<tl::core::LevelScript::Reachable> pools[2];
    bool pooled[2] = {false, false};

    template <typename ImplT>
    std::vector<tl::core::LevelScript::Reachable>& take(const ImplT* impl, bool with_player) {
        const int kind = with_player ? 1 : 0;
        if (!pooled[kind]) {
            pools[kind] = gather_missile_candidates(impl, with_player);
            pooled[kind] = true;
        }
        return pools[kind];
    }
    /// The candidates gathered again for `with_player`; the other kind is gathered when next
    /// taken.
    template <typename ImplT>
    std::vector<tl::core::LevelScript::Reachable> refresh(const ImplT* impl, bool with_player) {
        pooled[with_player ? 0 : 1] = false;
        return gather_missile_candidates(impl, with_player);
    }
    void forget() { pooled[0] = pooled[1] = false; }
};

template <typename ImplT, typename FlyingT>
std::function<bool(int64_t, const tl::core::Vec3&, float, float)> make_missile_harm_hook(
    ImplT* impl, const FlyingT& flying, std::vector<tl::core::LevelScript::Reachable>& candidates,
    const ogre::Vector3& struck_at,
    const std::function<std::vector<tl::core::LevelScript::Reachable>()>& gather) {
    return [impl, &flying, &candidates, &struck_at, gather](
               int64_t unit, const tl::core::Vec3& push, float damage_scale, float soak) {
        const bool landed =
            flying.level_placed || flying.detached
                ? impl->level_missile_harm(flying.missile.spec, unit, struck_at, damage_scale, push)
            : flying.by_weapon
                ? (flying.source.caster_machine != nullptr
                       ? impl->unit_weapon_missile_harm(flying.source.caster_machine,
                                                        flying.source.caster_node, unit, struck_at,
                                                        damage_scale, soak)
                       : impl->weapon_missile_harm(unit, struck_at, damage_scale, soak))
                : impl->missile_harm(
                      flying.source, unit, soak, struck_at, damage_scale,
                      std::atan2(flying.missile.forward.x, flying.missile.forward.z));
        if (landed) candidates = gather();
        return landed;
    };
}

}  // namespace

// `CMissile::doDamageToCharacter` with no listeners:
// `DAMAGE_MONSTER` (`CResourceManager::getGraphDamage`) at
// the victim's level x `randomBetweenVolatile(+0x284, +0x288)` x 1.0, no attacker.
bool World::level_missile_harm(const tl::core::MissileSpec& spec, int64_t unit,
                               const ogre::Vector3& at, float damage_scale,
                               const tl::core::Vec3& push) {
    const bool you = unit == kMissilePlayer;
    tl::core::LevelScript* owner = nullptr;
    std::string victim = "the player";
    int level = player_character.level();
    if (!you) {
        owner = script_owning(unit);
        if (owner == nullptr || owner->unit_is_dead(unit)) return false;
        std::vector<tl::core::LevelScript::Reachable> found;
        owner->reachable_characters(found);
        const auto it = std::find_if(found.begin(), found.end(),
                                     [unit](const auto& r) { return r.id == unit; });
        if (it == found.end()) return false;
        victim = it->unit;
        level = effective_monster_level(unit);
    } else if (player_health.dead) {
        return false;
    }
    const std::string curve =
        std::string(tl::core::kMonsterDamageGraph) + tl::core::difficulty_suffix(config.difficulty);
    const float graph = stat_graphs.value(curve, static_cast<float>(level), 0.0f);
    const float roll =
        tl::core::volatile_random().between(spec.min_damage_percent, spec.max_damage_percent);
    // `doDamageToCharacter` hands its scale only to the
    // listeners.
    (void)damage_scale;
    const float amount = graph * roll * 1.0f;
    if (you) {
        hurt_player(amount, spec.name, tl::core::LevelScript::kNoAttacker, /*struck=*/true);
    } else {
        // The direction the missile hands on, and its `FORCE` (+0x28c).
        tl::core::KillingBlow blow;
        blow.has_direction = true;
        blow.direction = push;
        blow.force = spec.knockback;
        hurt_unit(unit, victim, amount, false, 0, owner, &blow, /*struck=*/true);
    }
    TL_LOG_INFO(
        "missile: the level's %s strikes %s at (%.2f, %.2f, %.2f) for %.1f (graph %.1f x %.2f)",
        spec.name.c_str(), victim.c_str(), static_cast<double>(at.x), static_cast<double>(at.y),
        static_cast<double>(at.z), static_cast<double>(amount), static_cast<double>(graph),
        static_cast<double>(roll));
    return true;
}

bool World::skill_event_reaches_height(const ogre::Vector3& at, tl::core::LevelScript* machine,
                                       int64_t node) {
    // CSkillEvent::canEffectPosObject: the body's centre.
    float centre = 0.0f;
    if (machine == nullptr) {
        const float body =
            player_stats.radius > 0.0f ? player_stats.radius : tl::core::kDefaultCollisionRadius;
        centre = player.position().y + body;
    } else {
        tl::core::Vec3 feet;
        if (!machine->unit_position(node, feet)) return false;
        centre = feet.y + machine->unit_radius(node);
    }
    return std::fabs(at.y - centre) <= 3.0f;
}

void World::fire_missile_hit_event(const SkillEventSource& source, tl::core::LevelScript* machine,
                                   int64_t node, const ogre::Vector3& at, float yaw) {
    // `missileApplyingEffects`: not for a turned-back missile (`+0x294`, never
    // set here) or an `EVENT_MISSILEHIT` event.
    constexpr int kMissileHit = 6;
    if (source.group == kMissileHit) return;
    // Whether the skill authors the group at all, for the log.
    const auto authors_it = [&source](const tl::core::KnownSkill& skill) {
        const tl::core::SkillProperty* property = skill.property(source.level);
        if (property == nullptr || property->section == nullptr) return false;
        for (const tl::formats::AdmSection& child : property->event_section().children) {
            if (tl::core::upper_ascii(child.name) == "EVENT_MISSILEHIT") return true;
        }
        return false;
    };
    tl::core::SkillEventTarget target;
    target.run = source.run;
    target.origin = tl::core::Vec3{at.x, at.y, at.z};
    target.yaw = yaw;
    target.has_unit = true;
    target.unit = machine != nullptr ? node : 0;
    target.machine = machine;
    if (source.caster_machine == nullptr) {
        const tl::core::KnownSkill* skill = player_skill(source.skill);
        if (skill == nullptr) return;
        tl::core::SkillCastContext context = player_cast_context();
        context.event_target = target;
        tl::core::SkillRunner& runner = skill_runner_running(skill->entry.name);
        const bool running = runner.is_running(skill->entry.name);
        runner.trigger_skill_event(*skill, kMissileHit, context);
        if (authors_it(*skill)) {
            TL_LOG_INFO("missile: '%s' EVENT_MISSILEHIT%s", source.skill.c_str(),
                        running ? "" : " (the skill is not running: nothing)");
        }
        return;
    }
    tl::core::LevelScript& owner = *source.caster_machine;
    tl::core::LevelScript::UnitCasting* casting = owner.unit_casting(source.caster_node);
    const tl::core::KnownSkill* skill = unit_skill_named(owner, source.caster_node, source.skill);
    if (casting == nullptr || skill == nullptr) return;
    // The hit can come mid-cast of another unit (`unit_cast_caster`): put back after.
    const tl::core::SkillCaster outer_caster = unit_cast_caster;
    tl::core::SkillCastContext context =
        unit_cast_context(owner, source.caster_node, machine, node, true);
    context.event_target = target;
    const UnitEventCaster saved = unit_event_caster;
    unit_event_caster = unit_event_caster_for(&owner, source.caster_node);
    const bool running = casting->runner.is_running(skill->entry.name);
    casting->runner.trigger_skill_event(*skill, kMissileHit, context);
    unit_event_caster = saved;
    unit_cast_caster = outer_caster;
    if (authors_it(*skill)) {
        TL_LOG_INFO("missile: %s's '%s' EVENT_MISSILEHIT%s",
                    owner.unit_of(source.caster_node).c_str(), source.skill.c_str(),
                    running ? "" : " (the skill is not running: nothing)");
    }
}

bool World::missile_harm(const SkillEventSource& source, int64_t unit, float soak_scale,
                         const ogre::Vector3& at, float damage_scale, float yaw) {
    // `CSkillEvent::getCharacterCanBeHarmedByMissile`: the
    // owner itself under `MISSILE_TARGET_SELF`, else the damage-shape test with no shape, no
    // corpse flag, and the missile (never turned back here).
    const auto harmed = [this, &source](tl::core::LevelScript* machine, int64_t node) {
        const bool self = !source.prop_owner && machine == source.caster_machine &&
                          (machine == nullptr || node == source.caster_node);
        if (self && source.event != nullptr &&
            source.event->bool_or("MISSILE_TARGET_SELF", false)) {
            return true;
        }
        return skill_event_can_affect(source, skill_event_specific_units(source.event),
                                      tl::core::DamageShapeUnit{machine, node}, false, false,
                                      nullptr, false);
    };
    if (source.caster_machine != nullptr) {
        const bool you = unit == kMissilePlayer;
        tl::core::LevelScript* machine = you ? nullptr : script_owning(unit);
        const int64_t node = you ? 0 : unit;
        if (!you && machine == nullptr) return false;
        if (!harmed(machine, node)) return false;
        const std::string victim = you ? std::string("the player") : machine->unit_of(unit);
        // `missileApplyingEffects`: `canEffectPosObject`,
        // `EVENT_MISSILEHIT`, `applyWeaponDamage`, `applyEffects`,
        // `invokeHitSkills`.
        if (!skill_event_reaches_height(at, machine, node)) return true;
        fire_missile_hit_event(source, machine, node, at, yaw);
        const float weapon_part =
            skill_event_weapon_blow(source, machine, node, damage_scale, soak_scale);
        const bool applied = skill_event_effects_on(source, machine, node);
        invoke_hit_skills(source, source.group, machine, node);
        TL_LOG_INFO("missile: %s's '%s' strikes %s at (%.2f, %.2f, %.2f): weapon %.1f%s",
                    source.caster_machine->unit_of(source.caster_node).c_str(),
                    source.skill.c_str(), victim.c_str(), static_cast<double>(at.x),
                    static_cast<double>(at.y), static_cast<double>(at.z),
                    static_cast<double>(weapon_part), applied ? ", effects applied" : "");
        return true;
    }
    tl::core::LevelScript* owner = script_owning(unit);
    if (owner == nullptr || owner->unit_is_dead(unit)) return false;
    std::vector<tl::core::LevelScript::Reachable> found;
    owner->reachable_characters(found);
    const auto it =
        std::find_if(found.begin(), found.end(), [unit](const auto& r) { return r.id == unit; });
    if (it == found.end()) return false;
    const tl::core::LevelScript::Reachable& target = *it;

    // A unit it may not affect is flown through.
    if (!harmed(owner, unit)) return false;

    // `missileApplyingEffects`: the height test
    // (`canEffectPosObject`), `EVENT_MISSILEHIT`, then
    // `applyWeaponDamage`.
    if (!skill_event_reaches_height(at, owner, unit)) return true;
    fire_missile_hit_event(source, owner, unit, at, yaw);
    const float weapon_part =
        skill_event_weapon_blow(source, owner, unit, damage_scale, soak_scale);

    // `applyEffects` onto the unit's own manager
    // (`skill_event_effects_on`).
    float effect_part = 0.0f;
    if (source.event != nullptr && !owner->unit_is_dead(unit)) {
        float before = 0.0f;
        float max = 0.0f;
        owner->unit_health(unit, before, max);
        skill_event_effects_on(source, owner, unit);
        float after = 0.0f;
        owner->unit_health(unit, after, max);
        effect_part = std::max(0.0f, before - after);
    }
    // `invokeHitSkills`.
    invoke_hit_skills(source, source.group, owner, unit);
    TL_LOG_INFO("missile: '%s' strikes %s at (%.2f, %.2f, %.2f): weapon %.1f, effects %.1f",
                source.skill.c_str(), target.unit.c_str(), static_cast<double>(at.x),
                static_cast<double>(at.y), static_cast<double>(at.z),
                static_cast<double>(weapon_part), static_cast<double>(effect_part));
    return true;
}

void World::fire_spawner_missile(int64_t node_id, const std::string& resource,
                                 const ogre::Vector3& at, const ogre::Vector3& forward) {
    const auto found = skill_spawners.find(node_id);
    // A spawner no skill placed is the level's: no owner, no listeners
    // (`createAndFireMissile`).
    const bool level_placed = found == skill_spawners.end();
    const SkillSpawner* spawner = level_placed ? nullptr : &found->second;
    // `CResourceManager::createMissile` -> `CMissilePreloader::createNewMissileRef`:
    // the missile of that name, upper-cased.
    const tl::formats::CmpNode* node = missile_named(resource);
    if (node == nullptr) {
        TL_LOG_WARN("missiles: no missile named '%s' in media/missiles/", resource.c_str());
        return;
    }
    const tl::core::MissileSpec spec = tl::core::read_missile_spec(*node);

    // The owner: the player, its body sphere and target; none for a level's own missile.
    const float body =
        player_stats.radius > 0.0f ? player_stats.radius : tl::core::kDefaultCollisionRadius;
    const ogre::Vector3 feet =
        subject_node != nullptr ? view_position(subject_node) : ogre::Vector3::ZERO;
    // A placed unit's skill: the owner is that unit, and the missile can meet
    // the player as well as the level's characters.
    tl::core::LevelScript* const caster = level_placed ? nullptr : spawner->source.caster_machine;
    const int64_t caster_node = level_placed ? 0 : spawner->source.caster_node;
    const bool by_unit = caster != nullptr;
    const auto gather = [this, level_placed, by_unit]() {
        return gather_missile_candidates(this, level_placed || by_unit);
    };
    std::vector<tl::core::LevelScript::Reachable> candidates = gather();
    int64_t target = 0;
    if (by_unit) {
        // The owner's target (`CCharacter +0x340`).
        tl::core::LevelScript* target_machine = nullptr;
        int64_t target_node = 0;
        if (caster->unit_target(caster_node, target_machine, target_node)) {
            target = target_machine == nullptr ? kMissilePlayer : target_node;
        }
    } else if (!level_placed) {
        for (const auto& r : candidates) {
            if (r.id == player_target_node &&
                r.alignment == tl::core::UnitStats::Alignment::Hostile) {
                target = r.id;
            }
        }
    }

    FlyingMissile flying;
    flying.level_placed = level_placed;
    if (!level_placed) flying.source = spawner->source;
    flying.missile = tl::core::missile_from_template(spec);
    flying.missile.owner = 0;
    if (by_unit) {
        flying.missile.owner = caster_node;
        tl::core::Vec3 caster_feet;
        if (caster->unit_position(caster_node, caster_feet)) {
            const float radius = caster->unit_radius(caster_node);
            flying.missile.owner_center = {caster_feet.x, caster_feet.y + radius, caster_feet.z};
            flying.missile.owner_radius = radius;
        }
    } else if (!level_placed) {
        flying.missile.owner_center = {feet.x, feet.y + body, feet.z};
        flying.missile.owner_radius = body;
    }
    flying.missile.target = target;

    // `CMissile::fireMissile`: on first launch (`+0x1b5`) the event's
    // `CSkillEvent::missileValidateTargetBeforeLaunch`
    // settles the target: `MISSILE_TARGET_SELF` (`+0x14f`) the owner (hittable, `+0x144`);
    // target type 0 nobody; a target unit (`CSkill +0x40`) unless type 1 or 11; else the
    // event's point.
    if (!level_placed && spawner->source.event != nullptr && !flying.missile.fired) {
        if (spawner->source.event->bool_or("MISSILE_TARGET_SELF", false)) {
            // INVENTED: aimed at the target's centre, not the owner's bone
            // (`getBonePosition(owner, 1, 1)`); a player-owned one cannot meet the player.
            if (by_unit) {
                flying.missile.target = caster_node;
                flying.missile.hits_owner = true;
            }
        } else if (spawner->source.target_type == 0) {
            flying.missile.target = 0;
        }
        // INVENTED: the skill's target (`+0x40`) is not kept per run; the owner's stands in.
    }

    launch_missile(std::move(flying), at, forward,
                   level_placed ? std::string("the level") : spawner->source.skill);
}

// `CMissile::fireMissile`, with the world it flies in.
void World::launch_missile(FlyingMissile flying, const ogre::Vector3& at,
                           const ogre::Vector3& forward, const std::string& who) {
    const tl::core::MissileSpec spec = flying.missile.spec;
    const bool level_placed = flying.level_placed;
    const int64_t target = flying.missile.target;
    // What it can meet: the level's characters, and the player for a level's or unit's.
    const bool with_player = level_placed || flying.source.caster_machine != nullptr;
    const auto gather = [this, with_player]() {
        return gather_missile_candidates(this, with_player);
    };
    std::vector<tl::core::LevelScript::Reachable> candidates = gather();

    tl::core::MissileWorld world;
    world.unit = [&candidates](int64_t id, tl::core::Vec3& c, float& r, bool& alive) {
        return missile_unit(candidates, id, c, r, alive);
    };
    world.closest_target = [&candidates](const tl::core::Vec3& from, const tl::core::Vec3& fwd,
                                         float angle, float sight) {
        return missile_closest(candidates, from, fwd, angle, sight);
    };

    // For fire_missile's point-blank hit.
    ogre::Vector3 struck_at = at;
    world.harm = make_missile_harm_hook(this, flying, candidates, struck_at, gather);
    world.get_active_units_at_position = [this, &candidates, &spec](
                                             const tl::core::Vec3& position, float radius,
                                             std::vector<int64_t>* items_out) {
        std::vector<tl::core::ActiveUnitCandidate> active_candidates;
        active_candidates.reserve(candidates.size());
        for (const auto& c : candidates) {
            active_candidates.push_back({c.id, c.center});
        }
        std::vector<int64_t> hit = tl::core::get_active_units_at_position(
            active_candidates, position, radius, breakable_item_candidates(), items_out);
        TL_LOG_INFO("missile: '%s' area %.2f at (%.2f, %.2f, %.2f): %zu unit(s), %zu item(s)",
                    spec.name.c_str(), static_cast<double>(radius), static_cast<double>(position.x),
                    static_cast<double>(position.y), static_cast<double>(position.z), hit.size(),
                    items_out != nullptr ? items_out->size() : size_t{0});
        return hit;
    };
    world.interact_item = [this](int64_t item) { return queue_item_interact(item); };
    world.unit_kind = [this](int64_t unit) {
        return standing_breakable(unit) != nullptr ? tl::core::MissileUnitKind::Item
                                                   : tl::core::MissileUnitKind::Character;
    };
    // `CLevel::rayCollision`, objects included.
    world.ray = [this](const tl::core::Vec3& from, const tl::core::Vec3& to,
                       tl::core::Vec3& point) {
        tl::core::CollisionHit hit;
        if (!collision.ray_collision(from, to, /*objects=*/true, hit)) return false;
        point = hit.point;
        return true;
    };
    world.volatile_rolls = &tl::core::volatile_random();
    world.reflect = [this](int64_t unit) {
        return missile_reflect_share(unit == kMissilePlayer ? 0 : unit);
    };
    tl::core::fire_missile(flying.missile, tl::core::Vec3{at.x, at.y, at.z},
                           tl::core::Vec3{forward.x, forward.y, forward.z}, world);

    const tl::core::Vec3& p = flying.missile.position;
    const tl::core::Vec3& f = flying.missile.forward;
    flying.node = view_create_node(ogre::Vector3(p.x, p.y, p.z));
    view_aim_node(flying.node, ogre::Vector3(p.x, p.y, p.z), ogre::Vector3(f.x, f.y, f.z));
    flying.active = place_particle_layout(spec.active, ogre::Vector3::ZERO,
                                          ogre::Quaternion::IDENTITY, flying.node);
    if (!spec.release.empty()) {
        transient_particles.push_back(TransientParticles{
            place_particle_layout(spec.release, ogre::Vector3(p.x, p.y, p.z),
                                  view_orientation(flying.node), nullptr, ParticleStart::Cached),
            kMissileParticleStopSeconds, false, false, true});
    }
    missiles.push_back(std::move(flying));
    if (level_placed) {
        TL_LOG_INFO("missiles: %s deals %.3f to %.3f of the damage graph", spec.name.c_str(),
                    static_cast<double>(spec.min_damage_percent),
                    static_cast<double>(spec.max_damage_percent));
    }
    TL_LOG_INFO("missiles: '%s' fires %s from (%.2f, %.2f, %.2f) along (%.2f, %.2f, %.2f)%s",
                who.c_str(), spec.name.c_str(), static_cast<double>(at.x),
                static_cast<double>(at.y), static_cast<double>(at.z),
                static_cast<double>(forward.x), static_cast<double>(forward.y),
                static_cast<double>(forward.z), target != 0 ? ", at its target" : "");
}

// `CEquipment::fireMissiles(CCharacter*, CCharacter*)`.
bool World::fire_weapon_missiles(int64_t target, int breakable) {
    // no `MISSILE` (`+0x400`), no missile.
    if (!has_weapon || weapon.missile.empty() || subject_node == nullptr) return false;

    // The hand's tag point (`+0x2e8`, or `+0x2f8` left; vtable +0x200). INVENTED: with no
    // weapon model, the character's centre.
    const float body =
        player_stats.radius > 0.0f ? player_stats.radius : tl::core::kDefaultCollisionRadius;
    const ogre::Vector3 feet = view_position(subject_node);
    ogre::Vector3 hand;
    float height = 0.0f;
    const bool drawn = view_held_item(weapon_entity, hand, height);
    ogre::Vector3 start = drawn ? hand : feet + ogre::Vector3(0.0f, body, 0.0f);

    // `getForwardAbsolute` (vtable +0x138), or from the hand to the target's centre.
    ogre::Vector3 direction = view_derived_orientation(subject_node) * ogre::Vector3::UNIT_Z;
    tl::core::Vec3 aim;
    bool aimed = false;
    if (target != 0) {
        if (tl::core::LevelScript* owner = script_owning(target)) {
            aimed = owner->unit_position(target, aim);
        }
    } else if (breakable >= 0 && breakables[static_cast<size_t>(breakable)].node != nullptr) {
        const ogre::Vector3 p = view_position(breakables[static_cast<size_t>(breakable)].node);
        aim = tl::core::Vec3{p.x, p.y, p.z};
        aimed = true;
    }
    if (aimed) {
        direction = ogre::Vector3(aim.x - start.x, aim.y - start.y, aim.z - start.z);
        const float length = direction.length();
        if (1e-08f < length) direction = direction * (1.0f / length);
    }

    // out to the tip, the model's box height (vtable +0xd8) times
    // `WEAPON_SCALE * 0.8`; one unit with no model.
    if (drawn) {
        start += direction * height * (player_stats.weapon_scale * 0.8f);
    } else {
        start += direction;
    }

    // `CMissilePreloader::createNewMissileRef`; none of
    // that name fires nothing.
    const tl::formats::CmpNode* node = missile_named(weapon.missile);
    if (node == nullptr) {
        TL_LOG_WARN("missiles: the weapon's '%s' is not a missile in media/missiles/",
                    weapon.missile.c_str());
        return false;
    }

    // `CMissile::fireMissile(start, target or ZERO, missile, getEquippedTo(), ..., target)`,
    // the weapon its listener.
    FlyingMissile flying;
    flying.by_weapon = true;
    flying.missile = tl::core::missile_from_template(tl::core::read_missile_spec(*node));
    flying.missile.owner = 0;
    flying.missile.owner_center = {feet.x, feet.y + body, feet.z};
    flying.missile.owner_radius = body;
    flying.missile.target = target;
    launch_missile(std::move(flying), start, direction, "the weapon");
    return true;
}

// `CEquipment::fireMissiles(CCharacter*, CCharacter*)`,
// for a placed character.
void World::fire_unit_weapon_missile(tl::core::LevelScript* machine, int64_t node,
                                     const std::string& missile, bool left, bool aimed,
                                     tl::core::LevelScript* target_machine, int64_t target_node) {
    if (machine == nullptr || missile.empty()) return;
    tl::core::Vec3 feet;
    if (!machine->unit_position(node, feet)) return;
    const std::string unit = machine->unit_of(node);
    const tl::core::UnitStats stats = stats_for_unit(unit);
    const float radius = machine->unit_radius(node);

    // The hand's node (`+0x2f8` left, else `+0x2e8`). INVENTED: no item drawn, the centre.
    Ogre::Entity* body = nullptr;
    for (const auto& entry : scene_units) {
        if (entry.second.node == node) body = entry.second.entity;
    }
    if (body == nullptr) {
        for (const auto& owner : spawned) {
            for (const SpawnedUnit& made : owner.second) {
                if (made.id == node) body = made.entity;
            }
        }
    }
    Ogre::Entity* held = nullptr;
    if (body != nullptr) {
        const auto found = unit_held_items.find(body);
        if (found != unit_held_items.end()) held = found->second.held[left ? 1 : 0];
    }
    ogre::Vector3 hand;
    float height = 0.0f;
    const bool drawn = view_held_item(held, hand, height);
    ogre::Vector3 start = drawn ? hand : ogre::Vector3(feet.x, feet.y + radius, feet.z);

    // `getForwardAbsolute`, or from the hand to the target's centre.
    float facing = 0.0f;
    machine->unit_facing(node, facing);
    ogre::Vector3 direction(std::sin(facing), 0.0f, std::cos(facing));
    int64_t target = 0;
    if (aimed) {
        tl::core::Vec3 aim;
        bool have = false;
        if (target_machine == nullptr) {
            if (subject_node != nullptr) {
                const float own = player_stats.radius > 0.0f ? player_stats.radius
                                                             : tl::core::kDefaultCollisionRadius;
                const ogre::Vector3 p = view_position(subject_node);
                aim = {p.x, p.y + own, p.z};
                have = true;
                target = kMissilePlayer;
            }
        } else if (target_machine->unit_position(target_node, aim)) {
            aim.y += target_machine->unit_radius(target_node);
            have = true;
            target = target_node;
        }
        if (have) {
            direction = ogre::Vector3(aim.x - start.x, aim.y - start.y, aim.z - start.z);
            const float length = direction.length();
            if (1e-08f < length) direction = direction * (1.0f / length);
        }
    }

    // Out to the weapon's tip: its model's height times `WEAPON_SCALE * 0.8`;
    // one unit with no model.
    if (drawn) {
        const float scale = stats.weapon_scale > 0.0f ? stats.weapon_scale : 1.0f;
        start += direction * height * (scale * 0.8f);
    } else {
        start += direction;
    }

    const tl::formats::CmpNode* spec = missile_named(missile);
    if (spec == nullptr) {
        TL_LOG_WARN("missiles: %s's weapon '%s' is not a missile in media/missiles/", unit.c_str(),
                    missile.c_str());
        return;
    }
    // `CMissile::fireMissile(start, ..., getEquippedTo(), ..., target)`.
    FlyingMissile flying;
    flying.by_weapon = true;
    flying.source.caster_machine = machine;
    flying.source.caster_node = node;
    flying.missile = tl::core::missile_from_template(tl::core::read_missile_spec(*spec));
    flying.missile.owner = node;
    flying.missile.owner_center = {feet.x, feet.y + radius, feet.z};
    flying.missile.owner_radius = radius;
    flying.missile.target = target;
    launch_missile(std::move(flying), start, direction, unit + "'s weapon");
}

// `CEquipment::missileApplyingEffects` and
// `CEquipment::getCharacterCanBeHarmedByMissile`, for a
// placed wielder: a turned-back missile (`CMissile +0x294`, X360 `+0x28c`, never set here)
// harms anyone; else the wielder's `isEnemy`.
bool World::unit_weapon_missile_harm(tl::core::LevelScript* wielder, int64_t wielder_node,
                                     int64_t unit, const ogre::Vector3& at, float damage_scale,
                                     float soak_scale) {
    const bool you = unit == kMissilePlayer;
    tl::core::LevelScript* machine = you ? nullptr : script_owning(unit);
    if (!you && machine == nullptr) return false;
    const int64_t node = you ? 0 : unit;
    if (you ? player_health.dead : machine->unit_is_dead(unit)) return false;
    if (!wielder->unit_is_enemy_of(wielder_node, machine, node)) return false;
    // `rollAttack(level, unit, weapon, flags 0, damage scale, soak scale, 7)`.
    std::vector<tl::core::ScriptEvent> events;
    if (!wielder->unit_skill_blow(wielder_node, machine, node, damage_scale, soak_scale, events)) {
        return true;
    }
    const float amount = events.empty() ? 0.0f : events.back().amount;
    if (NodeMap* nodes = nodes_of(wielder)) apply_script_events(events, *nodes);
    TL_LOG_INFO("missile: %s's weapon strikes %s at (%.2f, %.2f, %.2f): %.1f",
                wielder->unit_of(wielder_node).c_str(),
                you ? "the player" : machine->unit_of(unit).c_str(), static_cast<double>(at.x),
                static_cast<double>(at.y), static_cast<double>(at.z), static_cast<double>(amount));
    return true;
}

// `CEquipment::missileApplyingEffects`.
bool World::weapon_missile_harm(int64_t unit, const ogre::Vector3& at, float damage_scale,
                                float soak_scale) {
    tl::core::LevelScript* owner = script_owning(unit);
    if (owner == nullptr || owner->unit_is_dead(unit)) return false;
    // `CCharacter::isEnemy(unit, the missile's owner)`, as in
    // `getCharacterCanBeHarmedByMissile`.
    if (!owner->unit_is_enemy_of(unit, nullptr, 0)) return false;
    // `getEquippedTo()->rollAttack(level, unit, weapon, 0, damage scale, soak scale, 7)`.
    const std::string name = owner->unit_of(unit);
    const float dealt = player_blow(unit, damage_scale, soak_scale);
    const bool critical = last_blow_critical;
    const tl::core::KillingBlow killing = last_killing_blow;
    show_player_blow(unit, name, dealt);
    if (dealt > 0.0f) hurt_unit(unit, name, dealt, critical, 0, owner, &killing, /*struck=*/true);
    slow_on_player_kill(owner->unit_is_dead(unit));
    TL_LOG_INFO("missile: the weapon's '%s' strikes %s at (%.2f, %.2f, %.2f): %.1f",
                weapon.missile.c_str(), name.c_str(), static_cast<double>(at.x),
                static_cast<double>(at.y), static_cast<double>(at.z), static_cast<double>(dealt));
    return true;
}

void World::dispose_transient_particles(float delta) {
    const tl::core::SectionScope timed(tl::core::Section::Disposal);
    // Every list whose time is up, asked together: one walk of the systems a frame.
    std::vector<const std::vector<Ogre::SceneNode*>*> due;
    std::vector<char> stop;
    std::vector<char> by_updating;
    std::vector<size_t> due_at;
    for (size_t i = 0; i < transient_particles.size(); ++i) {
        TransientParticles& transient = transient_particles[i];
        transient.left -= delta;
        if (transient.left > 0.0f) continue;
        // `CParticle::Stop(false)`, then wait for `getNumberOfParticlesUpdating`
        // to reach zero.
        due.push_back(&transient.holders);
        stop.push_back(!transient.stopped && !transient.self_timed ? 1 : 0);
        by_updating.push_back(transient.missile ? 1 : 0);
        due_at.push_back(i);
    }
    if (!due.empty()) {
        // Alive until `CParticle::updateLevelObject` has stopped it.
        std::vector<char> alive;
        view_transients_alive(due, stop, by_updating, alive);
        std::vector<Ogre::SceneNode*> dead;
        for (size_t k = due.size(); k-- > 0;) {
            TransientParticles& transient = transient_particles[due_at[k]];
            transient.stopped = true;
            if (alive[k] != 0) continue;
            dead.insert(dead.end(), transient.holders.begin(), transient.holders.end());
            transient_particles.erase(transient_particles.begin() + static_cast<long>(due_at[k]));
        }
        destroy_particle_holders(dead);
    }
}

void World::step_missiles(float delta) {
    dispose_transient_particles(delta);
    flush_item_interacts();
    if (missiles.empty() || !(delta > 0.0f)) return;

    MissilePools pools;
    for (size_t i = 0; i < missiles.size();) {
        FlyingMissile& flying = missiles[i];
        // What it can meet: the characters, and the player for a level's missile.
        const bool with_player = flying.level_placed || flying.source.caster_machine != nullptr;
        const auto gather = [this, with_player, &pools]() {
            return pools.refresh(this, with_player);
        };
        std::vector<tl::core::LevelScript::Reachable>& candidates = pools.take(this, with_player);
        const SkillEventSource source = flying.source;
        ogre::Vector3 struck_at = ogre::Vector3::ZERO;

        tl::core::MissileWorld world;
        world.unit = [&candidates](int64_t id, tl::core::Vec3& c, float& r, bool& alive) {
            return missile_unit(candidates, id, c, r, alive);
        };
        world.closest_target = [&candidates](const tl::core::Vec3& at, const tl::core::Vec3& fwd,
                                             float angle, float sight) {
            return missile_closest(candidates, at, fwd, angle, sight);
        };
        // `CLevel::preSortedSphereCollision`: level faces and body spheres, first met.
        world.sweep = [this, &candidates](
                          const tl::core::Vec3& from, const tl::core::Vec3& to, float radius,
                          bool objects, const std::vector<int64_t>& ignore, tl::core::Vec3& centre,
                          tl::core::Vec3& normal, int64_t& unit) {
            float best = 2.0f;
            unit = 0;
            std::vector<const tl::core::LevelScript::Reachable*> met;
            std::vector<int64_t> ids;
            met.reserve(candidates.size());
            ids.reserve(candidates.size());
            for (const auto& u : candidates) {
                if (std::find(ignore.begin(), ignore.end(), u.id) != ignore.end()) continue;
                met.push_back(&u);
                ids.push_back(u.id);
            }
            // `CLevel::objectSphereCollision` asks each
            // unit's `CBaseUnit::sphereCollision`
            // (`unit_collider_sweeps`). INVENTED: the body sphere where nothing is drawn.
            std::vector<float> swept;
            std::vector<char> drawn;
            unit_collider_sweeps(ids, from, to, radius, swept, drawn);
            for (size_t k = 0; k < met.size(); ++k) {
                const float t = drawn[k] != 0 ? swept[k]
                                              : sphere_sweep_t(from, to, radius, met[k]->center,
                                                               met[k]->radius);
                if (t < best) {
                    best = t;
                    unit = met[k]->id;
                }
            }
            // A breakable is a unit too: the box of its collision group, whatever
            // `COLLIDES WITH OBJECTS` says.
            for (const BreakableWorld& b : breakables) {
                if (b.node == nullptr || !b.state.usable || !b.has_collision) continue;
                if (std::find(ignore.begin(), ignore.end(), b.id) != ignore.end()) continue;
                tl::core::Vec3 box_low;
                tl::core::Vec3 box_high;
                if (!collision.group_bounds(b.id, box_low, box_high)) continue;
                const float t = box_sweep_t(from, to, radius, box_low, box_high);
                if (t < best) {
                    best = t;
                    unit = b.id;
                }
            }
            if (!collision.empty()) {
                // CMissile::checkCollision: `sortForCollisionByPoints`.
                collision.sort_for_collision_by_points(from, to, radius);
                tl::core::CollisionHit hit;
                if (collision.pre_sorted_sphere_collision(from, to, radius, objects, hit)) {
                    const tl::core::Vec3 d = to - from;
                    const tl::core::Vec3 m = hit.centre - from;
                    const float len2 = d.x * d.x + d.y * d.y + d.z * d.z;
                    const float t =
                        len2 > 0.0f ? std::sqrt((m.x * m.x + m.y * m.y + m.z * m.z) / len2) : 0.0f;
                    if (t < best) {
                        // A face of a breakable's own collision is that breakable.
                        unit = standing_breakable(hit.group) != nullptr ? hit.group : 0;
                        centre = hit.centre;
                        normal = hit.normal;
                        return true;
                    }
                }
            }
            if (!(best <= 1.0f)) return false;
            const tl::core::Vec3 d = to - from;
            centre = {from.x + d.x * best, from.y + d.y * best, from.z + d.z * best};
            for (const auto& u : candidates) {
                if (u.id != unit) continue;
                const tl::core::Vec3 n = centre - u.center;
                const float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                normal = l > 0.0f ? tl::core::Vec3{n.x / l, n.y / l, n.z / l}
                                  : tl::core::Vec3{0.0f, 1.0f, 0.0f};
            }
            return true;
        };
        world.harm = make_missile_harm_hook(this, flying, candidates, struck_at, gather);
        world.get_active_units_at_position = [this, &candidates, &flying](
                                                 const tl::core::Vec3& position, float radius,
                                                 std::vector<int64_t>* items_out) {
            std::vector<tl::core::ActiveUnitCandidate> active_candidates;
            active_candidates.reserve(candidates.size());
            for (const auto& c : candidates) {
                active_candidates.push_back({c.id, c.center});
            }
            std::vector<int64_t> hit = tl::core::get_active_units_at_position(
                active_candidates, position, radius, breakable_item_candidates(), items_out);
            TL_LOG_INFO("missile: '%s' area %.2f at (%.2f, %.2f, %.2f): %zu unit(s), %zu item(s)",
                        flying.missile.spec.name.c_str(), static_cast<double>(radius),
                        static_cast<double>(position.x), static_cast<double>(position.y),
                        static_cast<double>(position.z), hit.size(),
                        items_out != nullptr ? items_out->size() : size_t{0});
            return hit;
        };
        world.interact_item = [this](int64_t item) { return queue_item_interact(item); };
        world.unit_kind = [this](int64_t unit) {
            return standing_breakable(unit) != nullptr ? tl::core::MissileUnitKind::Item
                                                       : tl::core::MissileUnitKind::Character;
        };
        world.volatile_rolls = &tl::core::volatile_random();
        world.reflect = [this](int64_t unit) {
            return missile_reflect_share(unit == kMissilePlayer ? 0 : unit);
        };

        const tl::core::Vec3 before = flying.missile.position;
        struck_at = ogre::Vector3(before.x, before.y, before.z);
        const tl::core::MissileStep step = tl::core::update_missile(flying.missile, delta, world);
        const tl::core::Vec3& p = flying.missile.position;
        const ogre::Vector3 now(p.x, p.y, p.z);
        if (flying.node != nullptr) {
            const tl::core::Vec3& f = flying.missile.forward;
            view_aim_node(flying.node, now, ogre::Vector3(f.x, f.y, f.z));
        }
        if (step.hit) {
            // `HIT` where it struck, facing along the velocity.
            transient_particles.push_back(TransientParticles{
                place_particle_layout(flying.missile.spec.hit, now,
                                      flying.node != nullptr ? view_orientation(flying.node)
                                                             : ogre::Quaternion::IDENTITY,
                                      nullptr, ParticleStart::Cached),
                kMissileParticleStopSeconds, false, false, true});
        }
        if (step.killed) {
            TL_LOG_INFO("missile: %s ends at (%.2f, %.2f, %.2f) after %.2f units",
                        flying.missile.spec.name.c_str(), static_cast<double>(p.x),
                        static_cast<double>(p.y), static_cast<double>(p.z),
                        static_cast<double>(flying.missile.travelled_total));
            transient_particles.push_back(TransientParticles{
                place_particle_layout(flying.missile.spec.die, now, ogre::Quaternion::IDENTITY,
                                      nullptr, ParticleStart::Cached),
                kMissileParticleStopSeconds, false, false, true});
            // `CSkillEvent::missileDieing`: unless the event is
            // `EVENT_MISSILEDIE`, group 7 from the missile's centre, no unit or point.
            constexpr int kMissileDie = 7;
            if (!flying.level_placed && !flying.by_weapon && !flying.detached &&
                flying.source.group != kMissileDie) {
                if (const tl::core::KnownSkill* skill = player_skill(flying.source.skill)) {
                    tl::core::SkillCastContext context = player_cast_context();
                    context.event_target = tl::core::SkillEventTarget{};
                    context.event_target.run = flying.source.run;
                    context.event_target.origin = p;
                    context.event_target.yaw =
                        flying.node != nullptr
                            ? view_orientation(flying.node).getYaw().valueRadians()
                            : 0.0f;
                    skill_runner_running(skill->entry.name)
                        .trigger_skill_event(*skill, kMissileDie, context);
                    pools.forget();
                }
            }
        }
        if (!step.active_visible && !flying.active.empty()) destroy_particle_holders(flying.active);
        if (step.finished) {
            destroy_particle_holders(flying.active);
            if (flying.node != nullptr) view_destroy_node(flying.node);
            missiles.erase(missiles.begin() + static_cast<long>(i));
            continue;
        }
        ++i;
    }
    flush_item_interacts();
}

void World::clear_missiles() {
    for (FlyingMissile& flying : missiles) {
        destroy_particle_holders(flying.active);
        if (flying.node != nullptr) view_destroy_node(flying.node);
    }
    missiles.clear();
    for (TransientParticles& t : transient_particles) destroy_particle_holders(t.holders);
    transient_particles.clear();
    skill_spawners.clear();
    live_skill_events.clear();
    skill_event_holds.clear();
}

}  // namespace tl::core
