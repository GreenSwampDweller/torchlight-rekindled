// An item leaving a character's hands and landing on the floor:
// `CEquipment::drop`, its flight
// `CEquipment::updateDrop`, where it lands
// `CLevel::randomOpenItemPositionRange`, and the three-point `DROP`
// path it flies along, which is a `CPath` evaluated by
// `CPath::GetSplinePositionAtDistance`.
// Every drop is tossed: `drop`'s straight-placement branch never runs, since its flag is set
// once in `CEquipment::CEquipment`. No OGRE and no file I/O.
#pragma once

#include "core/collision.h"
#include "core/random.h"
#include "core/unit_types.h"

#include <array>
#include <functional>

namespace tl::core {

/// How `drop` lays the model down before its random heading.
enum class DropTilt {
    None,        ///< every other item: the model as authored
    CrossbowX,   ///< `ISA CROSSBOW` (0x6e): `matrixRotationX(-1.2217305)`, -70 degrees
    WeaponZ,     ///< `ISA WEAPON` (8): `matrixRotationZ(1.5707964)`
    ShieldZ,     ///< `ISA SHIELD` (0x15): `matrixRotationZ(-1.5707964)`
    ResetModel,  ///< `ISA SHOULDER ARMOR` (0x14) or `HELMET` (0xc): the model's own transform reset
};

/// The tilt for an item of `type`, tested in `drop`'s order - a crossbow is
/// also a weapon, and is asked about first.
DropTilt drop_tilt(const UnitTypeHierarchy& types, uint32_t type);

/// The tilt's rotation: an axis (0 X, 2 Z) and an angle in radians, or axis -1
/// for none.
struct DropTiltRotation {
    int axis = -1;
    float radians = 0.0f;
};
DropTiltRotation drop_tilt_rotation(DropTilt tilt);

/// Whether the item turns once round its own Z while it flies - `updateDrop`
/// spins `ISA` 8 (weapon), 0x15 (shield), 0x21 and 0x2a.
bool drop_spins(const UnitTypeHierarchy& types, uint32_t type);

/// `drop`'s numbers.
inline constexpr float kDropTossRadius = 1.5f;   ///< `randomOpenItemPosition(p, 1.5)`
inline constexpr float kDropFloorLift = 0.01f;   ///< added to `floorHeight`
inline constexpr float kDropWeaponLift = 0.05f;  ///< and again for `ISA WEAPON`
inline constexpr float kDropRiseMin = 3.5f;      ///< the arc's middle point, over the midpoint
inline constexpr float kDropRiseMax = 4.5f;
/// `updateDrop`: distance per second along the path
inline constexpr float kDropSpeed = 20.0f;

/// `CLevel::randomOpenItemPosition` (Linux
/// `randomOpenItemPositionRange`): a point between `lo` and `hi` from
/// `at`, at a random bearing, that the level's map calls passable; `at` after a
/// hundred tries. Two draws per try on `rng`: the distance, then the bearing in degrees.
Vec3 random_open_item_position(const Vec3& at, float lo, float hi,
                               const std::function<bool(const Vec3&)>& passable, RunicRandom& rng);

/// `CLevel::randomOpenPosition` (Linux
/// `randomOpenPositionRange`): the same two draws a try as the item
/// version above, but a point must pass `open`. After five tries the outer radius becomes
/// `max(5, hi + 0.1)`; after a hundred it gives up and returns `at`.
Vec3 random_open_position(const Vec3& at, float lo, float hi,
                          const std::function<bool(const Vec3&)>& open, RunicRandom& rng);

/// `CLevel::rayCollision` as `snap_item_to_ground` asks it: a hit between the
/// two points, its point, and the face's material (`CCollisionTriangle`'s code;
/// 100 is a `nocollide` face).
using GroundRay =
    std::function<bool(const Vec3& from, const Vec3& to, Vec3& hit, uint32_t& material)>;

/// `CItem::snapToGround`: a ray from 100 above the level's
/// zero to 100 below; a hit on anything but a material-100 face is where the item goes,
/// otherwise a new open point and again, thirty times.
Vec3 snap_item_to_ground(const Vec3& at, const GroundRay& ray,
                         const std::function<bool(const Vec3&)>& open, RunicRandom& rng);

/// `CCharacter::dropToGround` in its search, as `CLevel::addCharacter` calls it:
/// the same ray; a miss or a material-100 face moves the point by `random_open_position` within
/// 0..reach, the reach from 0.1 up by 0.1 to `max_reach`, thirty times. The ground point (the
/// caller stands the unit its radius above it), or the last point at a height of 100.
Vec3 drop_character_to_ground(const Vec3& at, float max_reach, const GroundRay& ray,
                              const std::function<bool(const Vec3&)>& open, RunicRandom& rng);

/// A `CPath` of three points, as `drop` builds it: where the item is, a point
/// over the middle raised by `kDropRiseMin..kDropRiseMax`, and where it lands.
struct DropArc {
    std::array<Vec3, 3> points{};
    /// The distance along the path at each point, straight lines between them
    /// (`AddPoint`).
    std::array<float, 3> along{};
    /// The whole length.
    float length = 0.0f;
};

/// The arc from `from` to `to`, taking the rise from `rng` (one draw).
DropArc make_drop_arc(const Vec3& from, const Vec3& to, RunicRandom& rng);

/// `CPath::GetSplinePositionAtDistance` on an
/// open path: a Catmull-Rom spline through the points, parameterised by distance.
Vec3 spline_position_at_distance(const DropArc& arc, float distance);

}  // namespace tl::core
