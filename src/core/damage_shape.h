// A Damage Shape: a region of a layout that hurts the characters inside it,
// once when it is switched on or every frame over a timer. `CDamageShape`
// (constructor, `setEnabled`,
// `updateLevelObject`,
// `characterUpdatedInLevel`, `doDamageToCharacter`)
// and `CDamageShapeDescriptor`'s setters. The shape itself is
// `CShape` (`core/missile.h`).
//
// Nothing here touches a level: `LevelScript` owns the node and applies the
// damage these functions decide.
#pragma once

#include "core/missile.h"
#include "core/random.h"
#include "core/stats.h"
#include "formats/cmp.h"

#include <cstdint>
#include <vector>

namespace tl::core {

class UnitTypeHierarchy;

/// A character as a shape's hit list and listeners know it: the machine that
/// runs it and its node, or the player (null machine, node 0).
struct DamageShapeUnit {
    const void* machine = nullptr;
    int64_t node = 0;
    bool operator==(const DamageShapeUnit& other) const {
        return machine == other.machine && node == other.node;
    }
};

/// An item as `CLevel::updateItems` hands it
/// to the level's update listeners: one that is in range (`+0x199`) and `ISA
/// 0x1d` (`BREAKABLE`).
struct DamageShapeItem {
    int64_t id = 0;
    Vec3 at;                 ///< `getPosition`
    float radius = 0.0f;     ///< `+0x194`
    uint32_t unit_type = 0;  ///< `+0x1ac`, for the shape's `UNITTYPE`
};

/// An item's entry in a shape's hit list (`+0x1e0` holds `CBaseUnit*`, items
/// beside characters).
DamageShapeUnit damage_shape_item_unit(int64_t id);

class DamageShapeListener;

/// `CDamageShape`, with the defaults of `CDamageShape::CDamageShape`
/// after `CShape`'s: enabled only in the editor,
/// visible, and the graph `DAMAGE_MONSTER` fetched (read by the damage itself).
struct DamageShape {
    SpawnerShape shape;  ///< the `CShape` half

    int32_t damage_type = 1;       ///< +0x184 `DAMAGE TYPE`, `gDAMAGE_TYPES` (1 `Magical`)
    uint32_t unit_type = 0;        ///< +0x188 `UNITTYPE`, a unit-type id (0 `any`)
    int32_t alignment = 3;         ///< +0x18c `ALIGNMENT`, `KALIGNMENT_STRINGS` (3 `ALL`)
    float timer = 0.0f;            ///< +0x19c `TIMER`
    float pause_timer = 0.0f;      ///< +0x1a0 `PAUSE TIMER`
    bool loops = true;             ///< +0x1a4 `LOOPS`, for ever
    bool update_in_editor = true;  ///< +0x1a5 `UPDATE IN EDITOR`
    bool dot = true;               ///< +0x1a7 `DOT`
    bool target_corpse = false;    ///< +0x1a8 `TARGET CORPSE`
    int32_t loop_count = 1;        ///< +0x1ac `LOOP COUNT`
    float min_percent = 1.0f;      ///< +0x1b4 `MIN`
    float max_percent = 2.0f;      ///< +0x1b8 `MAX`
    int32_t max_units_to_hit = 0;  ///< +0x1f8 `MAX UNITS TO HIT`, below 1 no cap

    /// +0x82, `CSceneNodeObject`'s enabled flag.
    bool enabled = false;
    bool ticked = false;  ///< +0x180, one update has run
    /// X360 +0x178, the position the last enabled update ended at; zero until then.
    Vec3 last_position{};
    float percent = 1.0f;     ///< +0x190, what the shape is tested at
    float time_left = 0.0f;   ///< +0x194
    float pause_left = 0.0f;  ///< +0x198
    bool done = false;        ///< +0x1a6
    int32_t loops_left = 0;   ///< +0x1b0
    /// +0x1c8, the `iDamageShape` listeners in the order they were added.
    std::vector<DamageShapeListener*> listeners;
    /// +0x1e0, the characters a non-DOT shape has already hit.
    std::vector<DamageShapeUnit> hit;
};

/// `iDamageShape`: what a skill's event is asked about each character a shape
/// reaches. `CSkillEvent` is the only implementer in the original.
class DamageShapeListener {
public:
    virtual ~DamageShapeListener() = default;
    /// Vtable `+0x10`, `CSkillEvent::getCharacterCanBeAffectedByDamageShape`.
    /// False keeps the shape's own damage
    /// off the character.
    virtual bool can_affect(const DamageShape& shape, const DamageShapeUnit& who) = 0;
    /// Vtable `+0x18`, `CSkillEvent::characterEffectedByDamageShape`:
    /// the listener acts on the character.
    /// True keeps the shape's own damage off it.
    virtual bool affect(const DamageShape& shape, const DamageShapeUnit& who) = 0;
    /// Vtable `+0x20`, `CSkillEvent::itemEffectedByDamageShape`:
    /// the listener acts on the item. False
    /// from any listener keeps the shape's own `interact` off it.
    virtual bool affect_item(const DamageShape& shape, const DamageShapeItem& item) {
        (void)shape;
        (void)item;
        return true;
    }
};

/// `CDamageShape::itemUpdatedInLevel` and `doDamageToItem`
/// for one breakable: inside the shape at its
/// percent, the shape enabled, `isa` (the item `ISA`s the shape's `UNITTYPE`);
/// a shape that is not `DOT` reaches an item once (the hit list); then every
/// listener is asked. True when the item's `interact(nullptr)` is to be called
/// - all listeners answered true, or there were none.
bool damage_shape_do_damage_to_item(DamageShape& s, const ShapeFrame& frame,
                                    const DamageShapeItem& item, bool isa);

/// A `Damage Shape` node's properties as `CDamageShapeDescriptor`'s setters
/// store them. `UNITTYPE` needs `unit_types` (`getUnitTypeIDByString`);
/// without it the shape stays at `any`. `ENABLED` is stored, not acted on:
/// the level switches the shape on when the scene is activated.
DamageShape read_damage_shape(const tl::formats::CmpNode& node,
                              const UnitTypeHierarchy* unit_types);

/// What `CDamageShape::setEnabled` asks of
/// the level afterwards.
enum class DamageShapeSwitch {
    Stop,     ///< `removeCharacterUpdateListener`
    Listen,   ///< `addCharacterUpdateListener`
    Instant,  ///< `doInstantDamage`, then `setEnabled(false)`
};
DamageShapeSwitch damage_shape_set_enabled(DamageShape& shape, bool on);

/// `CDamageShape::updateLevelObject`, its
/// first test: a shape that has run its last loop and its pause switches itself
/// off.
bool damage_shape_finished(const DamageShape& shape);

/// `CDamageShape::updateLevelObject`, the
/// timers of an enabled shape: the pause, the loop countdown and `percent`.
void damage_shape_step(DamageShape& shape, float delta);

/// `CDamageShape::characterUpdatedInLevel`: whether the character
/// is one the shape looks at and is inside it. `hidden` is the character's
/// `+0x70d` (its mesh hidden), `targetable` its `+0x531`, `radius` its
/// collision radius (`+0x194`).
bool damage_shape_reaches(const DamageShape& shape, const ShapeFrame& frame, bool hidden,
                          bool targetable, bool alive, const Vec3& at, float radius);

/// The character as `doDamageToCharacter` tests it.
struct DamageShapeVictim {
    DamageShapeUnit unit;
    bool isa = true;        ///< `CBaseUnit::ISA(+0x188)`
    int32_t alignment = 0;  ///< `CCharacter::alignment()`
    bool alive = true;      ///< `CCharacter::alive()`
};

/// What the shape does to one character.
struct DamageShapeHit {
    bool deal = false;  ///< the shape's own damage lands
    float amount = 0.0f;
    /// Add the character to the hit list once the damage has landed (a non-DOT
    /// shape no listener took the character on).
    bool record = false;
};

/// `CDamageShape::doDamageToCharacter`, less
/// the landing: the filters, the listeners (which may add to the hit list
/// themselves), then the amount from `damage_graph` (`DAMAGE_MONSTER`, set by
/// the constructor) at `level` (`CLevel +0x1a8`). One `rng` draw when `MIN` and
/// `MAX` differ.
DamageShapeHit damage_shape_do_damage_to_character(DamageShape& shape,
                                                   const DamageShapeVictim& victim,
                                                   const StatGraph* damage_graph, int32_t level,
                                                   float delta, RunicRandom& rng);

/// The amount half of `doDamageToCharacter`.
float damage_shape_amount(const DamageShape& shape, float graph_value, float delta,
                          RunicRandom& rng);

}  // namespace tl::core
