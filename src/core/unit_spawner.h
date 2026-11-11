// A layout's `Unit Spawner`: `CUnitSpawner` and the part of `CUnitSpawnerDescriptor` that drives
// it. `spawn`, `update`, `spawnUnitByIndex`,
// `reset`, `stop`,
// `destroyUnits`, `hideAndDisableUnits`,
// `createUnitsInMemory`, `addSpawnedUnit`,
// `unitStateChange`; `CUnitSpawnerDescriptor::InputLogicEvent`,
// the `Set_*` setters.
// This file decides; `UnitSpawnerHost` carries it out in the level.
#pragma once

#include "core/missile.h"
#include "core/random.h"
#include "formats/cmp.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tl::core {

/// `gSPAWN_TYPE_NAMES`, in the order the static initialiser builds
/// it. `GROUP` is compared exactly (`wmemcmp`, `std::operator==`).
enum class SpawnGroup { Monsters, Items, Particle, SpawnClass, Missiles, UnitType, Props, Other };
SpawnGroup spawn_group(const std::string& group);

/// A spawner node's properties, defaulting as `CUnitSpawner::CUnitSpawner`
/// leaves them.
struct UnitSpawnerSpec {
    SpawnerShape shape;
    uint32_t count = 10;             ///< +0x190 `COUNT`; 0 is stored as 1
    uint32_t max_allowed = 0;        ///< +0x19c `MAX ALLOWED`, 0 for no limit
    bool spawn_on_create = true;     ///< +0x17b `SPAWN ON CREATE`
    bool target_player = false;      ///< +0x1c2 `TARGET PLAYER`
    float duration = 1.0f;           ///< +0x180 `DURATION`
    uint32_t pulses = 0;             ///< +0x194 `PULSE RATE`
    bool spawn_in_center = false;    ///< +0x17a `SPAWN IN CENTER`
    bool requires_los = false;       ///< +0x17d `REQUIRES LOS`
    bool must_spawn = true;          ///< +0x17e `MUST SPAWN`
    bool play_spawn_anim = true;     ///< +0x17c `PLAY SPAWN ANIM`
    bool snap_to_ground = true;      ///< +0x230 `SNAP TO GROUND`
    bool give_xp = true;             ///< +0x178 `GIVE XP`
    bool give_loot = true;           ///< +0x179 `GIVE LOOT`
    int32_t level_delta = 0;         ///< +0x1a4 `UNIT LEVEL DELTA`
    bool destroy_body = false;       ///< +0x1c4 `DESTROY BODY`
    std::string group = "Monsters";  ///< +0x1b0 `GROUP`
    std::string resource;            ///< +0x1b8 `RESOURCE`
};

/// A `Unit Spawner` node's compiled fields (`cmp_field::unit_spawner`).
UnitSpawnerSpec read_unit_spawner(const tl::formats::CmpNode& node);

/// What a released unit turned out to be.
enum class SpawnedKind { None, Character, Item };

/// A unit the host made, as the spawner keeps it.
struct SpawnedUnitRef {
    SpawnedKind kind = SpawnedKind::None;
    int64_t id = 0;
    /// A `CEquipment` (what `unitStateChange` tells apart from other items).
    bool equipment = false;
};

/// Where a unit ends up once it is in the level (`spawnUnitByIndex` after
/// `addCharacter` / `addItem`).
enum class SpawnSettle {
    AsAdded,     ///< left where `addCharacter`/`addItem` put it (snapped to the ground)
    AtPosition,  ///< moved back to the placement (`SNAP TO GROUND` clear)
    AtSpawner,   ///< moved to the spawner's own point (`SPAWN IN CENTER`, or `+0x1c3`)
};

/// One unit for the host to make and put in the level, everything the
/// original decides about it decided.
struct SpawnRelease {
    uint32_t index = 0;
    SpawnGroup group = SpawnGroup::Other;
    /// `createUnit(StringUpper(GROUP), RESOURCE, level, false)`;
    /// `Spawn Class` names a table instead.
    std::string group_upper;
    std::string resource;
    /// `CLevel +0x1a8` (-1 without a level) plus `UNIT LEVEL DELTA`.
    int32_t level = 0;
    /// Where it is set before it is added, and the orientation's axes.
    Vec3 position;
    SpawnAxes axes;
    Vec3 spawner_position;
    SpawnSettle settle = SpawnSettle::AsAdded;

    // An item (`addItem(item, position, drop)`).
    bool item_drop = false;  ///< `SNAP TO GROUND` and not `SPAWN IN CENTER`
    bool item_snap = true;   ///< clear: `CItem +0x1f2` = 0 before it is added

    // A character.
    bool give_xp = true;          ///< clear: +0x454 = 0
    bool give_loot = true;        ///< clear: +0x703 and +0x704
    bool destroy_body = false;    ///< effect 0x81, marked +0x33
    bool play_spawn_anim = true;  ///< `CCharacter::spawn`, else AI state 4
    bool target_player = false;   ///< `setTarget(player)`, `MONSTER_HUNT`, AI flag 4
};

/// A roll of a `Spawn Class` spawner's table: the first unit it gave, and
/// whether that one is enchanted.
struct SpawnClassPick {
    bool found_class = false;  ///< `getSpawnClassByName` found the table
    std::string unit;          ///< empty: the roll gave nothing
    bool enchant = false;
};

/// The level, as a spawner sees it.
class UnitSpawnerHost {
public:
    virtual ~UnitSpawnerHost() = default;

    /// `CLevel +0x1a8`, or -1 when the spawner has no level.
    virtual int32_t level_monster_level() const = 0;
    /// `CLevel::randomOpenPosition(at, radius, avoid_no_spawn)`.
    virtual Vec3 random_open_position(const Vec3& at, float radius, bool avoid_no_spawn) = 0;
    /// `CLevel::rayCollision`: true on a hit,
    /// with its face normal.
    virtual bool ray_collision(const Vec3& from, const Vec3& to, Vec3& normal) = 0;
    /// `Ogre::Math::RangeRandom`.
    virtual float range_random(float lo, float hi) = 0;
    /// `g_RandVolatile`.
    virtual RunicRandom& volatile_random() = 0;

    /// `createParticleSystemAt(file, position, orientation, index)`.
    ///
    virtual void create_particles(const std::string& file, const Vec3& at, const SpawnAxes& axes,
                                  uint32_t index) = 0;
    /// `createAndFireMissile(resource, position, orientation)`.
    ///
    virtual void fire_missile(const std::string& resource, const Vec3& at,
                              const SpawnAxes& axes) = 0;
    /// `getSpawnClassByName` and
    /// `CSpawnClass::rollSpawnClass` with
    /// `(level, 0xffffffff, 0, -1, 0, 0)`, on `g_Rand`.
    virtual SpawnClassPick roll_spawn_class(const std::string& name, int32_t level) = 0;
    /// Make the unit, put it in the level as `release` says, and say what it
    /// was. `name` is the unit to make: the resource, or the spawn-class pick.
    virtual SpawnedUnitRef create_unit(const SpawnRelease& release, const std::string& name,
                                       bool enchant) = 0;

    /// A released unit still exists (its `TSafePointer` is not cleared).
    virtual bool unit_exists(int64_t id) const = 0;
    /// `CCharacter::alive`.
    virtual bool character_alive(int64_t id) const = 0;
    /// `CCharacter::die` (vtable +0x330), with no one to blame.
    virtual void kill_character(int64_t id) = 0;
    /// `setEnabled(!hide)` (vtable +0x40) and `CCharacter::setVisible(!hide, true)`.
    virtual void hide_character(int64_t id, bool hide) = 0;
    /// The spawner's particle layouts (+0x1d0): `CLayout::stop(false)`, or
    /// `CLayout::start`.
    virtual void run_particles(bool run) = 0;
    /// The spawner raises one of its outputs (vtable +0x30).
    virtual void output(uint32_t event) = 0;
    /// The spawn listeners (`+0x218`), told last about each unit released. Only a skill event
    /// registers one (`CSkillEvent::addListeners`).
    virtual void unit_spawned(const SpawnedUnitRef& unit) { (void)unit; }
    /// The resource's data names a `SPAWNER` (`setResourceSpawn`: its data group's
    /// `SPAWNER` value is not empty); false when the resource has no data.
    virtual bool resource_names_spawner(const std::string& resource) {
        (void)resource;
        return false;
    }
};

/// One `CUnitSpawner`.
class UnitSpawner {
public:
    UnitSpawner() = default;
    UnitSpawner(UnitSpawnerSpec spec, const ShapeFrame& frame)
        : spec_(std::move(spec)), frame_(frame) {}

    // PORT: an accessor of the port's own bookkeeping.
    const UnitSpawnerSpec& spec() const { return spec_; }
    // PORT: an accessor of the port's own bookkeeping.
    const ShapeFrame& frame() const { return frame_; }
    void set_frame(const ShapeFrame& frame) { frame_ = frame; }

    /// `CUnitSpawnerDescriptor::InputLogicEvent`.
    ///
    void input(uint32_t in, UnitSpawnerHost& host);

    void spawn(UnitSpawnerHost& host);
    void update(float delta, UnitSpawnerHost& host);
    void reset(UnitSpawnerHost& host);
    void stop();
    void destroy_units(UnitSpawnerHost& host);
    void hide_and_disable_units(bool hide, UnitSpawnerHost& host);
    void create_units_in_memory();
    /// `unitStateChange` for one of the items it released: 2 interacted with,
    /// 3 picked up.
    void unit_state_change(int64_t id, int state, UnitSpawnerHost& host);
    bool spawn_unit_by_index(uint32_t index, UnitSpawnerHost& host);

    // PORT: an accessor of the port's own bookkeeping.
    bool releasing() const { return active_; }
    // PORT: an accessor of the port's own bookkeeping.
    float elapsed() const { return elapsed_; }
    /// `CGameClient::restoreLevelState`'s spawner case: releasing is cleared once all
    /// `COUNT` are out.
    void restore_state(bool releasing, uint32_t released, float elapsed) {
        active_ = releasing;
        released_ = released;
        if (released_ == spec_.count) active_ = false;
        elapsed_ = elapsed;
    }
    // PORT: an accessor of the port's own bookkeeping.
    uint32_t released() const { return released_; }
    // PORT: an accessor of the port's own bookkeeping.
    uint32_t total() const { return total_; }
    int32_t level_delta() const { return spec_.level_delta; }
    const std::vector<SpawnedUnitRef>& characters() const { return characters_; }
    const std::vector<SpawnedUnitRef>& others() const { return others_; }
    /// `CUnitSpawner::addSpawnedUnit` from a unit's `applySaveState`: a
    /// restored unit taken back into the lists.
    void take_back(const SpawnedUnitRef& unit) { add_spawned_unit(unit); }
    /// Whether `id` is one of the units this spawner holds.
    bool holds(int64_t id) const;

private:
    void add_spawned_unit(const SpawnedUnitRef& unit);

    UnitSpawnerSpec spec_;
    ShapeFrame frame_;

    bool active_ = false;                     ///< +0x1c1, releasing
    float timer_ = 0.0f;                      ///< +0x184, to the next unit
    float elapsed_ = 0.0f;                    ///< +0x188
    uint32_t released_ = 0;                   ///< +0x18c
    uint32_t pulses_done_ = 0;                ///< +0x198
    float pulse_timer_ = 0.0f;                ///< +0x1a0
    uint32_t total_ = 0;                      ///< +0x1c8
    float dead_check_ = 1.0f;                 ///< +0x1cc, 1.0 from the constructor and never reset
    std::vector<SpawnedUnitRef> others_;      ///< +0x1e8, everything not a character
    std::vector<SpawnedUnitRef> characters_;  ///< +0x200
};

}  // namespace tl::core
