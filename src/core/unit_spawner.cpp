#include "core/unit_spawner.h"

#include "core/level_memory.h"
#include "core/logic_events.h"

#include <cmath>

namespace tl::core {

namespace {

namespace sf = tl::formats::cmp_field::unit_spawner;

// PORT: whether two points are the same.
bool same_point(const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

/// The node's field when it carries one dword, else the constructor's value.
// PORT: asset parsing, a field read with its fallback.
uint32_t word_or(const tl::formats::CmpNode& node, size_t index, uint32_t fallback) {
    const tl::formats::CmpField* f = node.field(index);
    return f != nullptr && f->count() == 1 ? f->as_uint() : fallback;
}

// PORT: asset parsing, a field read with its fallback.
bool flag_or(const tl::formats::CmpNode& node, size_t index, bool fallback) {
    return word_or(node, index, fallback ? 1u : 0u) != 0u;
}

// PORT: asset parsing, a field read with its fallback.
std::string text_or(const tl::formats::CmpNode& node, size_t index, const std::string& fallback) {
    const tl::formats::CmpField* f = node.field(index);
    return f != nullptr ? f->as_text() : fallback;
}

/// `pulses - 1 < 0xfffffffe`: a pulse count other than 0 and -1 (`CUnitSpawner::update`).
///
bool pulsed(uint32_t pulses) { return pulses - 1u < 0xfffffffeu; }

}  // namespace

SpawnGroup spawn_group(const std::string& group) {
    // `gSPAWN_TYPE_NAMES`.
    if (group == "Monsters") return SpawnGroup::Monsters;
    if (group == "Items") return SpawnGroup::Items;
    if (group == "Particle") return SpawnGroup::Particle;
    if (group == "Spawn Class") return SpawnGroup::SpawnClass;
    if (group == "Missiles") return SpawnGroup::Missiles;
    if (group == "Unit Type") return SpawnGroup::UnitType;
    if (group == "Props") return SpawnGroup::Props;
    return SpawnGroup::Other;
}

UnitSpawnerSpec read_unit_spawner(const tl::formats::CmpNode& node) {
    UnitSpawnerSpec s;
    s.shape = read_spawner_shape(node);
    // `Set_setNumberOfUnitsToCreate` stores 0 as 1.
    const uint32_t count = word_or(node, sf::kCount, s.count);
    s.count = count != 0 ? count : 1u;
    s.max_allowed = word_or(node, sf::kMaxAllowed, s.max_allowed);
    s.spawn_on_create = flag_or(node, sf::kSpawnOnCreate, s.spawn_on_create);
    s.target_player = flag_or(node, sf::kTargetPlayer, s.target_player);
    if (const tl::formats::CmpField* f = node.field(sf::kDuration);
        f != nullptr && f->count() == 1) {
        s.duration = f->as_float();
    }
    s.pulses = word_or(node, sf::kPulseRate, s.pulses);
    s.spawn_in_center = flag_or(node, sf::kSpawnInCenter, s.spawn_in_center);
    s.requires_los = flag_or(node, sf::kRequiresLos, s.requires_los);
    s.must_spawn = flag_or(node, sf::kMustSpawn, s.must_spawn);
    s.play_spawn_anim = flag_or(node, sf::kPlaySpawnAnim, s.play_spawn_anim);
    s.snap_to_ground = flag_or(node, sf::kSnapToGround, s.snap_to_ground);
    s.give_xp = flag_or(node, sf::kGiveXp, s.give_xp);
    s.give_loot = flag_or(node, sf::kGiveLoot, s.give_loot);
    s.level_delta = static_cast<int32_t>(
        word_or(node, sf::kUnitLevelDelta, static_cast<uint32_t>(s.level_delta)));
    s.destroy_body = flag_or(node, sf::kDestroyBody, s.destroy_body);
    s.group = text_or(node, sf::kGroup, s.group);
    s.resource = text_or(node, sf::kResource, s.resource);
    return s;
}

// PORT: whether the spawner released this unit.
bool UnitSpawner::holds(int64_t id) const {
    for (const SpawnedUnitRef& u : characters_) {
        if (u.id == id) return true;
    }
    for (const SpawnedUnitRef& u : others_) {
        if (u.id == id) return true;
    }
    return false;
}

// `CUnitSpawnerDescriptor::InputLogicEvent`.
void UnitSpawner::input(uint32_t in, UnitSpawnerHost& host) {
    switch (in) {
        case logic::kStop: stop(); break;
        case logic::kSpawnUnits: spawn(host); break;
        case logic::kDestroySpawnedUnits: destroy_units(host); break;
        case logic::kHideAndDisableSpawnedUnits: hide_and_disable_units(true, host); break;
        case logic::kIncrementLevelDelta: ++spec_.level_delta; break;
        case logic::kDecrementLevelDelta: --spec_.level_delta; break;
        default: break;
    }
}

// `CUnitSpawner::reset`.
void UnitSpawner::reset(UnitSpawnerHost& host) {
    elapsed_ = 0.0f;
    pulse_timer_ = 0.0f;
    timer_ = 0.0f;
    released_ = 0;
    host.run_particles(false);
}

// `CUnitSpawner::stop`: it stops releasing; what it released stays.
void UnitSpawner::stop() { active_ = false; }

// `CUnitSpawner::createUnitsInMemory`.
void UnitSpawner::create_units_in_memory() {
    if (!characters_.empty() || !others_.empty() || spec_.resource.empty()) return;
    total_ = spec_.count;
    if (pulsed(spec_.pulses)) total_ = spec_.count * spec_.pulses;
}

// `CUnitSpawner::spawn`.
void UnitSpawner::spawn(UnitSpawnerHost& host) {
    // INVENTED: vtable +0x50 with 1 first is skipped (the spawner's own node is not drawn).
    reset(host);
    active_ = true;
    total_ = spec_.count;
    if (pulsed(spec_.pulses)) total_ = spec_.count * spec_.pulses;
    if (spec_.duration <= 0.0f) {
        // No duration: the whole count now.
        for (uint32_t i = 0; i < total_; ++i) {
            if (spawn_unit_by_index(i, host)) ++released_;
        }
        if (released_ != total_) active_ = true;
    }
}

// `CUnitSpawner::update`.
void UnitSpawner::update(float delta, UnitSpawnerHost& host) {
    // The particle layouts' own update (vtable +0x208) is the renderer's.
    if (active_) {
        timer_ = timer_ - delta;
        pulse_timer_ = pulse_timer_ - delta;
        elapsed_ = delta + elapsed_;
        bool flush = false;
        bool settle = false;
        if (0.0f < timer_ || 0.0f < pulse_timer_) {
            // A unit is not due yet; past the duration the rest come at once.
            flush = !(elapsed_ < spec_.duration);
        } else if (spec_.duration <= elapsed_) {
            flush = true;
        } else if (pulsed(spec_.pulses)) {
            // A pulse: `COUNT` at once, by index one past what is out.
            while (pulse_timer_ < 0.0f) {
                for (uint32_t i = 0; i < spec_.count; ++i) {
                    if (spawn_unit_by_index(released_ + 1u, host)) ++released_;
                }
                ++pulses_done_;
                pulse_timer_ = spec_.duration / static_cast<float>(spec_.pulses) + pulse_timer_;
            }
        } else {
            // One at a time, `DURATION / max(total, 1)` apart; a unit that may
            // not come out yet (`MAX ALLOWED`) is asked for again next frame.
            while (timer_ < 0.0f) {
                if (!spawn_unit_by_index(released_, host)) {
                    timer_ = 0.0f;
                    break;
                }
                ++released_;
                float n = static_cast<float>(total_);
                if (n <= 1.0f) n = 1.0f;
                timer_ = spec_.duration / n + timer_;
            }
            settle = released_ == total_;
        }
        if (flush) {
            uint32_t i = released_;
            if (i < total_) {
                do {
                    if (spawn_unit_by_index(i, host)) ++released_;
                    ++i;
                } while (i < total_);
            }
            settle = total_ == released_;
        }
        if (settle) active_ = false;
    }

    // Who is still standing: from a second after the first check could run,
    // and only once the spawner is done or is holding a number alive.
    if ((active_ && spec_.max_allowed == 0) || characters_.empty()) return;
    dead_check_ = dead_check_ - delta;
    if (!(dead_check_ <= 0.0f)) return;
    size_t i = 0;
    while (i < characters_.size()) {
        const int64_t id = characters_[i].id;
        if (host.unit_exists(id)) {
            if (host.character_alive(id)) {
                ++i;
                continue;
            }
            host.output(logic::kMonsterKilled);
        }
        characters_[i] = characters_.back();
        characters_.pop_back();
    }
    if (!characters_.empty()) return;
    if (dead_check_ < 0.0f) host.output(logic::kAllMonstersDead);
}

// `CUnitSpawner::destroyUnits`: every
// character it released dies; the lists are then forgotten, items and all.
void UnitSpawner::destroy_units(UnitSpawnerHost& host) {
    active_ = false;
    for (const SpawnedUnitRef& unit : characters_) {
        if (host.unit_exists(unit.id)) host.kill_character(unit.id);
    }
    host.run_particles(false);
    characters_.clear();
    others_.clear();
}

// `CUnitSpawner::hideAndDisableUnits`.
void UnitSpawner::hide_and_disable_units(bool hide, UnitSpawnerHost& host) {
    for (const SpawnedUnitRef& unit : characters_) {
        if (host.unit_exists(unit.id)) host.hide_character(unit.id, hide);
    }
    host.run_particles(!hide);
}

// `CUnitSpawner::addSpawnedUnit`: a character
// on one list, anything else on the other (where `unitStateChange` hears about
// it).
void UnitSpawner::add_spawned_unit(const SpawnedUnitRef& unit) {
    if (unit.kind == SpawnedKind::Character) {
        characters_.push_back(unit);
    } else {
        others_.push_back(unit);
    }
}

// `CUnitSpawner::unitStateChange`.
void UnitSpawner::unit_state_change(int64_t id, int state, UnitSpawnerHost& host) {
    for (size_t i = 0; i < others_.size(); ++i) {
        if (others_[i].id != id) continue;
        others_[i] = others_.back();
        others_.pop_back();
        break;
    }
    if (state == 2) {
        // Interacted with: all of them once no item that is not equipment is left.
        host.output(logic::kItemInteracted);
        for (const SpawnedUnitRef& unit : others_) {
            if (unit.kind == SpawnedKind::Item && !unit.equipment) return;
        }
        host.output(logic::kAllItemsInteractedWith);
    } else if (state == 3) {
        // Picked up: all of them once no equipment is left.
        host.output(logic::kItemPickedUp);
        for (const SpawnedUnitRef& unit : others_) {
            if (unit.equipment) return;
        }
        host.output(logic::kAllItemsPickedUp);
    }
}

namespace {

// `spawnUnitByIndex`'s open point: a quarter unit round the point, then two, then five round the
// spawner itself; a place nothing finds is no place (false) unless the unit must come out anyway.
bool open_point(UnitSpawnerHost& host, const UnitSpawnerSpec& spec, const Vec3& center, Vec3& at) {
    const bool avoid_no_spawn = !spec.must_spawn;
    Vec3 open = host.random_open_position(at, 0.25f, avoid_no_spawn);
    if (same_point(open, at)) {
        open = host.random_open_position(at, 2.0f, avoid_no_spawn);
        if (same_point(open, at)) {
            at = center;
            open = host.random_open_position(center, 5.0f, avoid_no_spawn);
            if (same_point(open, at) && !spec.must_spawn) return false;
        }
    }
    at = open;
    return true;
}

}  // namespace

// `CUnitSpawner::spawnUnitByIndex`. False
// only when nothing may come out now; a unit that finds no room, no line of
// sight or no data still counts.
bool UnitSpawner::spawn_unit_by_index(uint32_t index, UnitSpawnerHost& host) {
    const uint32_t cap = spec_.max_allowed;
    if (cap != 0 && (cap <= others_.size() || cap <= characters_.size())) return false;
    if (total_ - 1u <= index) active_ = false;

    // How far through its duration, and where in the release this unit is.
    float percent = 1.0f;
    if (0.0f < spec_.duration || std::isnan(spec_.duration)) {
        const float ratio = elapsed_ / spec_.duration;
        percent = ratio < 1.0f ? ratio : 1.0f;
    }
    float spread = static_cast<float>(total_) - 1.0f;
    if (spec_.shape.shape == 0) {
        const DynamicAttribute& angle = spec_.shape.angle;
        float release = 0.0f;
        if (angle.draws()) {
            float low = 0.0f;
            float high = 0.0f;
            angle.range(percent, low, high);
            release = host.range_random(low, high);
        } else {
            release = angle.at(percent);
        }
        if (release == 360.0f) spread = static_cast<float>(total_);
    }
    const float pulses = static_cast<float>(spec_.pulses);
    spread = spread / (1.0f < pulses ? pulses : 1.0f);
    const float divisor = 1.0f < spread ? spread : 1.0f;
    const float fraction =
        static_cast<float>(static_cast<uint32_t>(index - pulses_done_ * spec_.count)) / divisor;

    const RangeRandom range = [&host](float lo, float hi) { return host.range_random(lo, hi); };
    const SpawnPlacement placed = shape_position_and_orientation(
        spec_.shape, frame_, percent, fraction, range, host.volatile_random());
    Vec3 at = placed.position;
    SpawnAxes axes = placed.axes;
    const SpawnGroup group = spawn_group(spec_.group);

    if (group != SpawnGroup::Particle && group != SpawnGroup::Missiles &&
        !open_point(host, spec_, frame_.center, at)) {
        return true;
    }

    if (!spec_.spawn_in_center && spec_.requires_los) {
        // A wall between the spawner and the point, half a unit up.
        const Vec3 from{frame_.center.x, frame_.center.y + 0.5f, frame_.center.z};
        const Vec3 to{at.x, frame_.center.y + 0.5f, at.z};
        Vec3 normal;
        if (host.ray_collision(from, to, normal) && std::fabs(normal.y) < 0.25f) return true;
    }

    if (group == SpawnGroup::Particle) {
        host.create_particles("media/particles/" + spec_.resource, at, axes, index);
        return true;
    }
    if (group == SpawnGroup::Missiles) {
        host.fire_missile(spec_.resource, at, axes);
        return true;
    }
    if (group == SpawnGroup::UnitType) return true;

    SpawnRelease release;
    release.index = index;
    release.group = group;
    release.group_upper = upper_ascii(spec_.group);
    release.resource = spec_.resource;
    release.level = host.level_monster_level() + spec_.level_delta;

    std::string name = spec_.resource;
    bool enchant = false;
    if (group == SpawnGroup::SpawnClass) {
        const SpawnClassPick pick = host.roll_spawn_class(spec_.resource, release.level);
        if (!pick.found_class) return true;
        name = pick.unit;
        enchant = pick.enchant;
        if (!name.empty()) {
            // Turned to face along the shape's z axis on the ground, the x axis
            // built as z cross up: a mirrored basis, which `FromAxes` is given
            // as it is.
            Vec3 z{axes.z.x, 0.0f, axes.z.z};
            const float n = std::sqrt(z.x * z.x + 0.0f + z.z * z.z);
            if (1e-08f < n) {
                const float inv = 1.0f / n;
                z = {z.x * inv, 0.0f * inv, z.z * inv};
            }
            const Vec3 up{0.0f, 1.0f, 0.0f};
            axes.z = {z.x * up.y - up.x * z.y, up.z, z.z};
            axes.y = {z.z * up.x - z.x * up.z, up.y, z.y};
            axes.x = {z.y * up.z - z.z * up.y, up.x, z.x};
        }
    }
    if (name.empty()) return true;

    release.position = at;
    release.axes = axes;
    release.spawner_position = frame_.center;
    // `+0x1a7` (`setResourceSpawn`): never for a group among
    // `gSPAWN_TYPE_NAMES[2..5]` (the others returned above); else the resource names a `SPAWNER`.
    const bool resource_names_spawner =
        group != SpawnGroup::SpawnClass && host.resource_names_spawner(spec_.resource);
    if (spec_.spawn_in_center || resource_names_spawner) {
        release.settle = SpawnSettle::AtSpawner;
    } else {
        release.settle = spec_.snap_to_ground ? SpawnSettle::AsAdded : SpawnSettle::AtPosition;
    }
    release.item_snap = spec_.snap_to_ground;
    release.item_drop = spec_.snap_to_ground && !spec_.spawn_in_center;
    release.give_xp = spec_.give_xp;
    release.give_loot = spec_.give_loot;
    release.destroy_body = spec_.destroy_body;
    release.play_spawn_anim = spec_.play_spawn_anim;
    release.target_player = spec_.target_player;

    const SpawnedUnitRef made = host.create_unit(release, name, enchant);
    if (made.kind == SpawnedKind::None) return true;
    add_spawned_unit(made);
    if (total_ - 1u == index) host.output(logic::kAllUnitsSpawned);
    // The listeners at +0x218 (vtable +0x10 with the spawner and the unit).
    host.unit_spawned(made);
    return true;
}

}  // namespace tl::core
