// `CUnitSpawner`: spawn, update, spawnUnitByIndex,
// unitStateChange, destroyUnits, the constructor's defaults,
// and CShape's updatePositionAndOrientation.
#include "core/unit_spawner.h"
#include "core/logic_events.h"
#include "test_support.h"

#include <cmath>
#include <cstring>
#include <set>

namespace {

using tl::core::SpawnAxes;
using tl::core::SpawnClassPick;
using tl::core::SpawnedKind;
using tl::core::SpawnedUnitRef;
using tl::core::SpawnGroup;
using tl::core::SpawnRelease;
using tl::core::SpawnSettle;
using tl::core::UnitSpawner;
using tl::core::UnitSpawnerSpec;
using tl::core::Vec3;
namespace logic = tl::core::logic;

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

/// A level with room everywhere (every open-position search moves the point a
/// unit east), no walls, and every unit made as asked.
class FakeLevel : public tl::core::UnitSpawnerHost {
public:
    int32_t level_monster_level() const override { return 7; }
    Vec3 random_open_position(const Vec3& at, float radius, bool avoid_no_spawn) override {
        searches.push_back(radius);
        last_avoid = avoid_no_spawn;
        if (crowded) return at;
        return {at.x + 1.0f, at.y, at.z};
    }
    bool ray_collision(const Vec3&, const Vec3&, Vec3& normal) override {
        normal = wall_normal;
        return wall;
    }
    float range_random(float lo, float hi) override { return lo + (hi - lo) * 0.5f; }
    tl::core::RunicRandom& volatile_random() override { return rng; }
    void create_particles(const std::string& file, const Vec3&, const SpawnAxes&,
                          uint32_t index) override {
        particles.push_back(file);
        particle_indices.push_back(index);
    }
    void fire_missile(const std::string& resource, const Vec3&, const SpawnAxes&) override {
        missiles.push_back(resource);
    }
    SpawnClassPick roll_spawn_class(const std::string& name, int32_t level) override {
        SpawnClassPick pick;
        pick.found_class = name == "TABLE";
        pick.unit = "picked";
        pick.enchant = true;
        rolled_level = level;
        return pick;
    }
    SpawnedUnitRef create_unit(const SpawnRelease& release, const std::string& name,
                               bool enchant) override {
        releases.push_back(release);
        names.push_back(name);
        enchants.push_back(enchant);
        SpawnedUnitRef made;
        made.id = ++serial;
        made.kind = make_items ? SpawnedKind::Item : SpawnedKind::Character;
        made.equipment = make_items && serial % 2 == 1;
        alive.insert(made.id);
        existing.insert(made.id);
        return made;
    }
    bool unit_exists(int64_t id) const override { return existing.count(id) != 0; }
    bool character_alive(int64_t id) const override { return alive.count(id) != 0; }
    bool resource_names_spawner(const std::string& resource) override {
        return spawner_resources.count(resource) != 0;
    }
    std::set<std::string> spawner_resources;
    void kill_character(int64_t id) override {
        killed.push_back(id);
        alive.erase(id);
    }
    void hide_character(int64_t id, bool hide) override {
        if (hide) hidden.push_back(id);
    }
    void run_particles(bool run) override { particles_running = run; }
    void output(uint32_t event) override { outputs.push_back(event); }

    size_t count_of(uint32_t event) const {
        size_t n = 0;
        for (uint32_t e : outputs) n += e == event ? 1 : 0;
        return n;
    }

    bool crowded = false;
    bool wall = false;
    Vec3 wall_normal{1.0f, 0.0f, 0.0f};
    bool make_items = false;
    bool last_avoid = false;
    bool particles_running = true;
    int32_t rolled_level = 0;
    int64_t serial = 0;
    tl::core::RunicRandom rng{1u};
    std::vector<float> searches;
    std::vector<SpawnRelease> releases;
    std::vector<std::string> names;
    std::vector<bool> enchants;
    std::vector<std::string> particles;
    std::vector<uint32_t> particle_indices;
    std::vector<std::string> missiles;
    std::set<int64_t> alive;
    std::set<int64_t> existing;
    std::vector<int64_t> killed;
    std::vector<int64_t> hidden;
    std::vector<uint32_t> outputs;
};

/// A point-shaped spawner at (10, 0, 20) facing +Z.
UnitSpawnerSpec point_spec(uint32_t count, float duration) {
    UnitSpawnerSpec spec;
    spec.shape.shape = 3;  // Point
    spec.count = count;
    spec.duration = duration;
    spec.resource = "Ratlin";
    return spec;
}

tl::core::ShapeFrame frame_at(float x, float z) {
    tl::core::ShapeFrame frame;
    frame.center = {x, 0.0f, z};
    return frame;
}

tl::formats::CmpField uint_field(uint32_t value) {
    tl::formats::CmpField f;
    f.words.push_back(value);
    return f;
}

tl::formats::CmpField float_field(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    return uint_field(bits);
}

tl::formats::CmpField text_field(const std::string& text) {
    const size_t dwords = ((text.size() + 1) * 2 + 3) / 4;
    std::vector<uint8_t> payload(dwords * 4, 0);
    for (size_t i = 0; i < text.size(); ++i) payload[2 * i] = static_cast<uint8_t>(text[i]);
    tl::formats::CmpField f;
    f.words.resize(dwords);
    for (size_t i = 0; i < dwords; ++i) {
        f.words[i] = static_cast<uint32_t>(payload[4 * i]) |
                     (static_cast<uint32_t>(payload[4 * i + 1]) << 8) |
                     (static_cast<uint32_t>(payload[4 * i + 2]) << 16) |
                     (static_cast<uint32_t>(payload[4 * i + 3]) << 24);
    }
    return f;
}

}  // namespace

/// Without a field the constructor's values stand: ten units over a second,
/// must spawn, snap, give XP and loot, `Monsters`.
TL_TEST(unit_spawner_defaults_are_the_constructors) {
    tl::formats::CmpNode node;
    const UnitSpawnerSpec spec = tl::core::read_unit_spawner(node);
    TL_CHECK(spec.count == 10u);
    TL_CHECK(near(spec.duration, 1.0f));
    TL_CHECK(spec.must_spawn && spec.snap_to_ground && spec.give_xp && spec.give_loot);
    TL_CHECK(spec.spawn_on_create && spec.play_spawn_anim);
    TL_CHECK(!spec.spawn_in_center && !spec.requires_los && !spec.target_player);
    TL_CHECK(spec.group == "Monsters");
    TL_CHECK(near(spec.shape.box_size.x, 10.0f) && near(spec.shape.box_size.y, 1.0f));
}

/// The compiled fields by index; a `COUNT` of 0 is stored as 1.
TL_TEST(unit_spawner_reads_its_fields) {
    namespace f = tl::formats::cmp_field::unit_spawner;
    tl::formats::CmpNode node;
    node.fields.resize(31);
    node.fields[f::kCount] = uint_field(0);
    node.fields[f::kMaxAllowed] = uint_field(3);
    node.fields[f::kDuration] = float_field(2.5f);
    node.fields[f::kPulseRate] = uint_field(4);
    node.fields[f::kRequiresLos] = uint_field(1);
    node.fields[f::kMustSpawn] = uint_field(0);
    node.fields[f::kSnapToGround] = uint_field(0);
    node.fields[f::kUnitLevelDelta] = uint_field(static_cast<uint32_t>(-2));
    node.fields[f::kDestroyBody] = uint_field(1);
    node.fields[f::kGroup] = text_field("Spawn Class");
    node.fields[f::kResource] = text_field("TREASURE");
    tl::formats::CmpField size;
    for (float v : {8.0f, 1.0f, 6.0f}) size.words.push_back(float_field(v).words[0]);
    node.fields[f::kSize] = size;
    const UnitSpawnerSpec spec = tl::core::read_unit_spawner(node);
    TL_CHECK(spec.count == 1u);
    TL_CHECK(spec.max_allowed == 3u);
    TL_CHECK(near(spec.duration, 2.5f));
    TL_CHECK(spec.pulses == 4u);
    TL_CHECK(spec.requires_los && !spec.must_spawn && !spec.snap_to_ground && spec.destroy_body);
    TL_CHECK(spec.level_delta == -2);
    TL_CHECK(spec.group == "Spawn Class" && spec.resource == "TREASURE");
    TL_CHECK(near(spec.shape.box_size.x, 8.0f) && near(spec.shape.box_size.z, 6.0f));
}

/// No duration: the whole count at once, and `All Units Spawned` with the last.
TL_TEST(unit_spawner_without_duration_releases_everything_at_once) {
    FakeLevel level;
    UnitSpawner spawner(point_spec(3, 0.0f), frame_at(10.0f, 20.0f));
    spawner.spawn(level);
    TL_CHECK(level.releases.size() == 3u);
    TL_CHECK(spawner.released() == 3u);
    TL_CHECK(!spawner.releasing());
    TL_CHECK(level.count_of(logic::kAllUnitsSpawned) == 1u);
    // The level's monster level plus the delta, and the group upper-cased.
    TL_CHECK(level.releases[0].level == 7);
    TL_CHECK(level.releases[0].group_upper == "MONSTERS");
}

/// A duration spreads the count: one every `DURATION / COUNT`, the first on the
/// first update, and whatever is left comes at once when the duration is up.
TL_TEST(unit_spawner_spreads_a_count_over_its_duration) {
    FakeLevel level;
    UnitSpawner spawner(point_spec(4, 1.0f), frame_at(0.0f, 0.0f));
    spawner.spawn(level);
    TL_CHECK(level.releases.empty());
    TL_CHECK(spawner.releasing());
    spawner.update(0.1f, level);  // t 0.1: the first, next due in 0.25
    TL_CHECK(level.releases.size() == 1u);
    spawner.update(0.1f, level);  // t 0.2
    TL_CHECK(level.releases.size() == 1u);
    spawner.update(0.1f, level);  // t 0.3: the second
    TL_CHECK(level.releases.size() == 2u);
    spawner.update(0.8f, level);  // t 1.1: past the duration, the rest
    TL_CHECK(level.releases.size() == 4u);
    TL_CHECK(!spawner.releasing());
    TL_CHECK(level.count_of(logic::kAllUnitsSpawned) == 1u);
}

/// `MAX ALLOWED` holds the rest back until the death check, a second after it
/// may first run, takes the dead off the list.
TL_TEST(unit_spawner_max_allowed_waits_for_the_dead) {
    FakeLevel level;
    UnitSpawnerSpec spec = point_spec(5, 0.0f);
    spec.max_allowed = 2;
    UnitSpawner spawner(spec, frame_at(0.0f, 0.0f));
    spawner.spawn(level);
    TL_CHECK(spawner.released() == 2u);
    TL_CHECK(spawner.releasing());
    spawner.update(0.5f, level);
    TL_CHECK(spawner.released() == 2u);
    level.alive.erase(1);
    spawner.update(0.6f, level);  // the check runs now: one dead
    TL_CHECK(level.count_of(logic::kMonsterKilled) == 1u);
    TL_CHECK(spawner.characters().size() == 1u);
    TL_CHECK(spawner.released() == 2u);
    spawner.update(0.1f, level);  // and one more may come out
    TL_CHECK(spawner.released() == 3u);
    TL_CHECK(level.count_of(logic::kAllMonstersDead) == 0u);
}

/// Everything it released dead: `Monster Killed` for each, then
/// `All Monsters Dead` once.
TL_TEST(unit_spawner_raises_all_monsters_dead_once) {
    FakeLevel level;
    UnitSpawner spawner(point_spec(2, 0.0f), frame_at(0.0f, 0.0f));
    spawner.spawn(level);
    level.alive.clear();
    spawner.update(0.5f, level);           // too soon
    TL_CHECK(level.outputs.size() == 1u);  // All Units Spawned
    spawner.update(0.6f, level);
    TL_CHECK(level.count_of(logic::kMonsterKilled) == 2u);
    TL_CHECK(level.count_of(logic::kAllMonstersDead) == 1u);
    spawner.update(1.0f, level);
    TL_CHECK(level.count_of(logic::kAllMonstersDead) == 1u);
}

/// A unit whose body is gone is dropped without `Monster Killed`.
TL_TEST(unit_spawner_forgets_a_vanished_unit_quietly) {
    FakeLevel level;
    UnitSpawner spawner(point_spec(1, 0.0f), frame_at(0.0f, 0.0f));
    spawner.spawn(level);
    level.existing.clear();
    spawner.update(1.5f, level);
    TL_CHECK(level.count_of(logic::kMonsterKilled) == 0u);
    TL_CHECK(level.count_of(logic::kAllMonstersDead) == 1u);
}

/// Pulses: `COUNT` a pulse, `total` = `COUNT x PULSE RATE`, asked for by index
/// one past what is out (the original's own off-by-one).
TL_TEST(unit_spawner_pulses_release_count_at_a_time) {
    FakeLevel level;
    UnitSpawnerSpec spec = point_spec(2, 3.0f);
    spec.pulses = 3;
    UnitSpawner spawner(spec, frame_at(0.0f, 0.0f));
    spawner.spawn(level);
    TL_CHECK(spawner.total() == 6u);
    spawner.update(0.1f, level);
    TL_CHECK(level.releases.size() == 2u);
    TL_CHECK(level.releases[0].index == 1u && level.releases[1].index == 2u);
    spawner.update(0.5f, level);  // next pulse at 1.0
    TL_CHECK(level.releases.size() == 2u);
    spawner.update(0.5f, level);
    TL_CHECK(level.releases.size() == 4u);
}

/// Somewhere open a quarter unit round; the searches avoid no-spawn regions
/// only when the unit need not come out.
TL_TEST(unit_spawner_finds_an_open_place) {
    FakeLevel level;
    UnitSpawner spawner(point_spec(1, 0.0f), frame_at(10.0f, 20.0f));
    spawner.spawn(level);
    TL_CHECK(level.searches.size() == 1u && near(level.searches[0], 0.25f));
    TL_CHECK(!level.last_avoid);
    TL_CHECK(near(level.releases[0].position.x, 11.0f));
    TL_CHECK(near(level.releases[0].position.z, 20.0f));
}

/// Nowhere open: 0.25, then 2, then 5 round the spawner. `MUST SPAWN` puts it
/// on the spawner; without it nothing comes out, and it still counts.
TL_TEST(unit_spawner_crowded_falls_back_to_the_spawner) {
    FakeLevel level;
    level.crowded = true;
    UnitSpawner must(point_spec(1, 0.0f), frame_at(10.0f, 20.0f));
    must.spawn(level);
    TL_CHECK(level.searches.size() == 3u);
    TL_CHECK(near(level.searches[1], 2.0f) && near(level.searches[2], 5.0f));
    TL_CHECK(level.releases.size() == 1u);
    TL_CHECK(near(level.releases[0].position.x, 10.0f));

    FakeLevel other;
    other.crowded = true;
    UnitSpawnerSpec spec = point_spec(1, 0.0f);
    spec.must_spawn = false;
    UnitSpawner may(spec, frame_at(10.0f, 20.0f));
    may.spawn(other);
    TL_CHECK(other.last_avoid);
    TL_CHECK(other.releases.empty());
    TL_CHECK(may.released() == 1u);
}

/// `REQUIRES LOS`: a wall (a face whose normal is nearly flat) between the
/// spawner and the point stops the unit; a floor does not.
TL_TEST(unit_spawner_line_of_sight_stops_at_walls) {
    FakeLevel level;
    level.wall = true;
    UnitSpawnerSpec spec = point_spec(1, 0.0f);
    spec.requires_los = true;
    UnitSpawner blocked(spec, frame_at(0.0f, 0.0f));
    blocked.spawn(level);
    TL_CHECK(level.releases.empty());

    level.wall_normal = {0.0f, 1.0f, 0.0f};
    UnitSpawner floor(spec, frame_at(0.0f, 0.0f));
    floor.spawn(level);
    TL_CHECK(level.releases.size() == 1u);
}

/// Where it settles: centre and `SPAWNER` model on the spawner, else the placement; an item
/// is tossed only when it snaps and is not centred.
TL_TEST(unit_spawner_settle_rules) {
    FakeLevel level;
    UnitSpawnerSpec spec = point_spec(1, 0.0f);
    UnitSpawner plain(spec, frame_at(0.0f, 0.0f));
    plain.spawn(level);
    TL_CHECK(level.releases.back().settle == SpawnSettle::AsAdded);
    TL_CHECK(level.releases.back().item_drop && level.releases.back().item_snap);

    spec.snap_to_ground = false;
    UnitSpawner loose(spec, frame_at(0.0f, 0.0f));
    loose.spawn(level);
    TL_CHECK(level.releases.back().settle == SpawnSettle::AtPosition);
    TL_CHECK(!level.releases.back().item_drop && !level.releases.back().item_snap);

    spec.snap_to_ground = true;
    spec.spawn_in_center = true;
    UnitSpawner centred(spec, frame_at(0.0f, 0.0f));
    centred.spawn(level);
    TL_CHECK(level.releases.back().settle == SpawnSettle::AtSpawner);
    TL_CHECK(!level.releases.back().item_drop);

    spec.spawn_in_center = false;
    level.spawner_resources.insert(spec.resource);
    UnitSpawner hut(spec, frame_at(0.0f, 0.0f));
    hut.spawn(level);
    TL_CHECK(level.releases.back().settle == SpawnSettle::AtSpawner);
}

/// `Spawn Class`: the table's first pick at the level, enchanted on its flag,
/// facing along the shape's z on the ground with the mirrored x axis.
TL_TEST(unit_spawner_spawn_class_pick_and_axes) {
    FakeLevel level;
    UnitSpawnerSpec spec = point_spec(1, 0.0f);
    spec.group = "Spawn Class";
    spec.resource = "TABLE";
    spec.level_delta = 2;
    tl::core::ShapeFrame frame = frame_at(0.0f, 0.0f);
    frame.forward = {0.6f, 0.0f, 0.8f};
    frame.right = {0.8f, 0.0f, -0.6f};
    UnitSpawner spawner(spec, frame);
    spawner.spawn(level);
    TL_CHECK(level.rolled_level == 9);
    TL_CHECK(level.names.size() == 1u && level.names[0] == "picked");
    TL_CHECK(level.enchants[0]);
    const SpawnAxes& axes = level.releases[0].axes;
    TL_CHECK(near(axes.z.x, 0.6f) && near(axes.z.z, 0.8f));
    TL_CHECK(near(axes.x.x, -0.8f) && near(axes.x.z, 0.6f));
    TL_CHECK(near(axes.y.y, 1.0f));

    // A table nobody has: nothing, and it counts.
    FakeLevel other;
    spec.resource = "NONE";
    UnitSpawner missing(spec, frame);
    missing.spawn(other);
    TL_CHECK(other.releases.empty() && missing.released() == 1u);
}

/// `Particle` and `Missiles` skip the open-place search; `Unit Type` makes
/// nothing.
TL_TEST(unit_spawner_particle_missile_and_unit_type_groups) {
    FakeLevel level;
    UnitSpawnerSpec spec = point_spec(2, 0.0f);
    spec.group = "Particle";
    spec.resource = "Fire-Large-Brazier.layout";
    UnitSpawner fire(spec, frame_at(0.0f, 0.0f));
    fire.spawn(level);
    TL_CHECK(level.particles.size() == 2u);
    TL_CHECK(level.particles[0] == "media/particles/Fire-Large-Brazier.layout");
    TL_CHECK(level.particle_indices[1] == 1u);
    spec.group = "Missiles";
    spec.resource = "EMBERBOLT";
    UnitSpawner bolt(spec, frame_at(0.0f, 0.0f));
    bolt.spawn(level);
    TL_CHECK(level.missiles.size() == 2u);
    TL_CHECK(level.searches.empty());
    spec.group = "Unit Type";
    UnitSpawner type(spec, frame_at(0.0f, 0.0f));
    type.spawn(level);
    TL_CHECK(level.releases.empty());
}

/// Items: `Item Picked Up`, and `All Items Picked Up` once no equipment is
/// left; interacting counts the items that are not equipment.
TL_TEST(unit_spawner_item_outputs) {
    FakeLevel level;
    level.make_items = true;
    UnitSpawner spawner(point_spec(2, 0.0f), frame_at(0.0f, 0.0f));
    spawner.spawn(level);  // 1 equipment, 2 not
    TL_CHECK(spawner.others().size() == 2u);
    spawner.unit_state_change(1, 3, level);
    TL_CHECK(level.count_of(logic::kItemPickedUp) == 1u);
    TL_CHECK(level.count_of(logic::kAllItemsPickedUp) == 1u);
    spawner.unit_state_change(2, 2, level);
    TL_CHECK(level.count_of(logic::kItemInteracted) == 1u);
    TL_CHECK(level.count_of(logic::kAllItemsInteractedWith) == 1u);
}

/// `Stop` stops the release and keeps what is out; `Destroy` kills the living
/// and forgets everything; the level delta steps by one.
TL_TEST(unit_spawner_inputs) {
    FakeLevel level;
    UnitSpawner spawner(point_spec(4, 1.0f), frame_at(0.0f, 0.0f));
    spawner.input(logic::kSpawnUnits, level);
    spawner.update(0.1f, level);
    TL_CHECK(level.releases.size() == 1u);
    spawner.input(logic::kStop, level);
    spawner.update(2.0f, level);
    TL_CHECK(level.releases.size() == 1u);
    TL_CHECK(spawner.characters().size() == 1u);
    spawner.input(logic::kIncrementLevelDelta, level);
    spawner.input(logic::kIncrementLevelDelta, level);
    spawner.input(logic::kDecrementLevelDelta, level);
    TL_CHECK(spawner.level_delta() == 1);
    spawner.input(logic::kHideAndDisableSpawnedUnits, level);
    TL_CHECK(level.hidden.size() == 1u && !level.particles_running);
    spawner.input(logic::kDestroySpawnedUnits, level);
    TL_CHECK(level.killed.size() == 1u);
    TL_CHECK(spawner.characters().empty());
}

/// `CShape`: a full-circle `Angle` spreads `COUNT` round it (not `COUNT - 1`),
/// a `Box` walks its rim, a `Line` runs from end to end.
TL_TEST(unit_spawner_shape_placement) {
    tl::core::RunicRandom rng{1u};
    const tl::core::RangeRandom half = [](float lo, float hi) { return lo + (hi - lo) * 0.5f; };
    tl::core::ShapeFrame frame = frame_at(0.0f, 0.0f);

    tl::core::SpawnerShape angle;
    angle.shape = 0;
    angle.angle.value = 360.0f;
    angle.min_radius.value = 2.0f;
    angle.max_radius.value = 2.0f;
    // Index 0: the offset is -180, so the angle is -180 + 90 = -90 degrees.
    const tl::core::SpawnPlacement first =
        tl::core::shape_position_and_orientation(angle, frame, 0.0f, 0.0f, half, rng);
    TL_CHECK(near(first.position.x, 0.0f) && near(first.position.z, -2.0f));
    TL_CHECK(near(first.axes.z.z, 1.0f));

    tl::core::SpawnerShape box;
    box.shape = 4;
    box.box_size = {8.0f, 1.0f, 6.0f};
    const tl::core::SpawnPlacement rim =
        tl::core::shape_position_and_orientation(box, frame, 0.0f, 0.25f, half, rng);
    TL_CHECK(near(rim.position.x, 0.0f) && near(rim.position.z, 3.0f));
    box.release_order = 1;
    const tl::core::SpawnPlacement back =
        tl::core::shape_position_and_orientation(box, frame, 0.0f, 0.0f, half, rng);
    TL_CHECK(near(back.position.x, -4.0f));

    // Outward from the centre: z points away, y up, x = y cross z.
    box.release_order = 0;
    box.direction = 3;
    const tl::core::SpawnPlacement out =
        tl::core::shape_position_and_orientation(box, frame, 0.0f, 0.0f, half, rng);
    TL_CHECK(near(out.axes.z.x, 1.0f) && near(out.axes.x.z, -1.0f));
}
