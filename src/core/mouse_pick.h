// The PC's pointer: what is under it, and where on the ground it points.
// `CLevel::findCharacterAtScreenCoordinates`,
// `CLevel::findItemAtScreenCoordinates` and the plane half of
// `CGameClient::findWorldLocation`. The console has no pointer
// and none of these; they run only under the mouse control scheme.
//
// Pure geometry: the caller supplies the matrices, the boxes and the ray.
#pragma once

#include "core/collision.h"

#include <cstddef>
#include <cstdint>

namespace tl::core {

/// Projection times view, row-major, as both picks build it before their loop.
struct ScreenMatrix {
    float m[4][4] = {};
};

/// `projection * view`, both row-major 4x4.
ScreenMatrix screen_matrix(const float projection[16], const float view[16]);

/// A point through the matrix and the divide: x and y in -1..1 on screen (y up), z the depth.
Vec3 to_screen(const ScreenMatrix& matrix, const Vec3& point);

/// The first test of both picks: the unit's own position within 1.1 of the screen's
/// middle on both axes (x open below, as the original compares it).
bool pick_on_screen(const ScreenMatrix& matrix, const Vec3& position);

/// A unit's box on screen: the extremes of its projected corners.
struct ScreenBox {
    Vec3 low;
    Vec3 high;
};

/// A character's box: x and z from the local minimum times 0.7
/// held to -0.55 at most, against a fixed 0.55; y from the minimum to 0.9 of the maximum.
/// Seven corners are projected - the low-x, top, low-z one is never visited.
ScreenBox character_screen_box(const ScreenMatrix& matrix, const Vec3& position,
                               const Vec3& local_min, const Vec3& local_max);

/// An item's box: both local bounds times (0.7, 0.9, 0.7).
ScreenBox item_screen_box(const ScreenMatrix& matrix, const Vec3& position, const Vec3& local_min,
                          const Vec3& local_max);

/// One thing the pointer could be over.
struct PickCandidate {
    ScreenBox box;
    /// `CCharacter::alignment()`: 0 neutral, 1 good, 2 evil. Items pass 2.
    int32_t alignment = 2;
    /// Passed through.
    std::uint64_t id = 0;
};

/// The depth a candidate competes with: the box's nearest depth, with 800 added for a
/// neutral character and 1200 for a good one, so an enemy in front of the pointer wins.
float pick_priority(const PickCandidate& candidate);

/// The candidate under (x, y), in the callers' order with the smallest priority winning
/// and the first of equals kept. False when none is under the pointer.
bool pick_at(const PickCandidate* candidates, std::size_t count, float x, float y,
             std::uint64_t* out_id);

/// `MATH::distanceToPlane`: how far along `direction` from `origin` the
/// plane `dot(normal, p) == d` lies; -1 when the ray runs along it.
float distance_to_plane(const Vec3& origin, const Vec3& direction, const Vec3& normal, float d);

/// The point of the pointer's ray on the horizontal plane at `plane_y`, its y left 0
/// (`findWorldLocation`).
Vec3 pointer_on_plane(const Vec3& ray_origin, const Vec3& ray_direction, float plane_y);

/// How far above the player's feet the plane sits when the caller asks for the raised
/// point (the aim of an attack or a skill, not a walk).
inline constexpr float kPointerRaise = 1.7f;

/// The pointer's ray is cast this far against the level to find the floor it points at.
inline constexpr float kPointerRayLength = 200.0f;
/// A hit nearer the camera than this is not a floor to stand on.
inline constexpr float kPointerNearHit = 10.0f;
/// The floor under the hit is sought this far up and down.
inline constexpr float kPointerFloorProbe = 1000.0f;
/// The floor found must face up at least this much.
inline constexpr float kPointerFloorNormal = 0.2f;
/// A floor further than this above or below the player is not followed.
inline constexpr float kPointerFloorReach = 14.0f;

/// `CCameraControl::processInput`, the pointer's share: the dolly
/// (`+0x38`) moved 0.01 a wheel unit towards the player, held between 14 and the far
/// distance (28.5 times the camera multiplier, `+0x8c`); the middle button puts it at the
/// far distance.
float wheel_dolly(float dolly, int wheel, bool middle_pressed, float camera_multiplier);

}  // namespace tl::core
