#include "core/item_drop.h"
#include "core/runic_math.h"

#include <algorithm>
#include <cmath>

namespace tl::core {

namespace {

// `UNITTYPES.HIE` ids the two functions pass to `CBaseUnit::ISA`.
constexpr uint32_t kCrossbow = 0x6e;
constexpr uint32_t kSpinA = 0x21;
constexpr uint32_t kSpinB = 0x2a;

}  // namespace

// `CEquipment::drop`, the `ISA` chain at its
// head.
DropTilt drop_tilt(const UnitTypeHierarchy& types, uint32_t type) {
    if (types.isa(type, kCrossbow)) return DropTilt::CrossbowX;
    if (types.isa(type, unit_type::kWeapon)) return DropTilt::WeaponZ;
    if (types.isa(type, unit_type::kShield)) return DropTilt::ShieldZ;
    if (types.isa(type, unit_type::kShoulderArmor) || types.isa(type, unit_type::kHelmet)) {
        return DropTilt::ResetModel;
    }
    return DropTilt::None;
}

// PORT: drawing, the dropped item's tilt as an axis and an angle.
DropTiltRotation drop_tilt_rotation(DropTilt tilt) {
    switch (tilt) {
        case DropTilt::CrossbowX: return {0, -1.2217305f};
        case DropTilt::WeaponZ: return {2, 1.5707964f};
        case DropTilt::ShieldZ: return {2, -1.5707964f};
        default: return {};
    }
}

// `CEquipment::updateDrop`, the `ISA` test
// around the spin.
bool drop_spins(const UnitTypeHierarchy& types, uint32_t type) {
    return types.isa(type, unit_type::kWeapon) || types.isa(type, unit_type::kShield) ||
           types.isa(type, kSpinA) || types.isa(type, kSpinB);
}

// `CLevel::randomOpenItemPosition(out, this, const Vector3&, float)`
// (Linux `randomOpenItemPositionRange`, whose `lo` the
// console has as 0).
Vec3 random_open_item_position(const Vec3& at, float lo, float hi,
                               const std::function<bool(const Vec3&)>& passable, RunicRandom& rng) {
    float reach = hi;
    for (unsigned tries = 0; tries < 100; ++tries) {
        // Past the sixth try the outer radius grows by 0.1 before the draws.
        if (tries > 5) reach = reach + 0.1f;
        const float distance = rng.between(lo, reach);
        const float degrees = rng.between(0.0f, 360.0f);
        // (0, 0, distance) turned by `MATH::rotateY` with the
        // console's own sine and cosine.
        const Vec3 offset = rotate_y(Vec3{0.0f, 0.0f, distance}, degrees * 0.017453292f);
        const Vec3 tried{at.x + offset.x, at.y + offset.y, at.z + offset.z};
        if (!passable || passable(tried)) return tried;
    }
    // A hundred tries refused: the point asked about, as it came.
    return at;
}

Vec3 random_open_position(const Vec3& at, float lo, float hi,
                          const std::function<bool(const Vec3&)>& open, RunicRandom& rng) {
    // `CLevel::randomOpenPositionRange`.
    float reach = hi;
    unsigned tries = 0;
    while (true) {
        const float distance = rng.between(lo, reach);
        const float degrees = rng.between(0.0f, 360.0f);
        // (0, 0, distance) turned by `MATH::rotateY` with the
        // console's own sine and cosine.
        const Vec3 offset = rotate_y(Vec3{0.0f, 0.0f, distance}, degrees * 0.017453292f);
        const Vec3 tried{at.x + offset.x, at.y + offset.y, at.z + offset.z};
        if (!open || open(tried)) return tried;
        ++tries;
        if (tries == 100) return at;
        if (5 < tries) reach = std::max(5.0f, reach + 0.1f);  // `maxss`
    }
}

Vec3 snap_item_to_ground(const Vec3& at, const GroundRay& ray,
                         const std::function<bool(const Vec3&)>& open, RunicRandom& rng) {
    // `CItem::snapToGround`.
    Vec3 tried = at;
    for (int attempt = 0; attempt < 30; ++attempt) {
        Vec3 hit;
        uint32_t material = 0;
        if (ray &&
            ray(Vec3{tried.x, 100.0f, tried.z}, Vec3{tried.x, -100.0f, tried.z}, hit, material) &&
            material != 100) {
            return hit;
        }
        tried = random_open_position(Vec3{tried.x, 100.0f, tried.z}, 0.0f, 4.0f, open, rng);
    }
    return Vec3{tried.x, at.y, tried.z};
}

Vec3 drop_character_to_ground(const Vec3& at, float max_reach, const GroundRay& ray,
                              const std::function<bool(const Vec3&)>& open, RunicRandom& rng) {
    // `CCharacter::dropToGround`: the open-point search draws
    // no no-spawn test (`randomOpenPosition`'s last argument 0); after thirty misses the point is
    // left at 100, kept.
    Vec3 tried = at;
    float reach = 0.1f;
    for (int attempt = 0; attempt < 30; ++attempt) {
        Vec3 hit;
        uint32_t material = 0;
        if (ray &&
            ray(Vec3{tried.x, 100.0f, tried.z}, Vec3{tried.x, -100.0f, tried.z}, hit, material) &&
            material != 100) {
            return hit;
        }
        tried = random_open_position(Vec3{tried.x, 100.0f, tried.z}, 0.0f, reach, open, rng);
        const float grown = reach + 0.1f;
        reach = max_reach < grown ? max_reach : grown;
    }
    return Vec3{tried.x, 100.0f, tried.z};
}

// `CEquipment::drop`'s three `CPath::AddPoint` calls.
DropArc make_drop_arc(const Vec3& from, const Vec3& to, RunicRandom& rng) {
    Vec3 middle;
    middle.z = (to.z + from.z) * 0.5f;
    middle.x = (to.x + from.x) * 0.5f;
    const float halfway = (to.y + from.y) * 0.5f;
    middle.y = rng.between(kDropRiseMin, kDropRiseMax) + halfway;

    DropArc arc;
    arc.points = {from, middle, to};
    arc.along[0] = 0.0f;
    arc.length = 0.0f;
    for (size_t i = 1; i < arc.points.size(); ++i) {
        const float dx = arc.points[i].x - arc.points[i - 1].x;
        const float dy = arc.points[i].y - arc.points[i - 1].y;
        const float dz = arc.points[i].z - arc.points[i - 1].z;
        const float step = std::sqrt(dx * dx + dy * dy + dz * dz);
        arc.length = step + arc.length;
        arc.along[i] = arc.along[i - 1] + step;
    }
    return arc;
}

// `CPath::GetSplinePositionAtDistance`, for a
// path that is not closed: the distance wraps rather than clamps, and a neighbour past
// either end is the end point.
Vec3 spline_position_at_distance(const DropArc& arc, float distance) {
    const float length = arc.length;
    const int count = static_cast<int>(arc.points.size());
    if (length == 0.0f) return arc.points[0];
    // As `Route::spline_position_at_distance`, an infinite distance never leaves these
    // loops; the renderer clamps it first.
    while (distance < 0.0f) distance = distance + length;
    while (length < distance) distance = distance - length;

    const int last = count - 1;
    int i0 = -1, i1 = 0, i2 = 1, i3 = 2;
    if (arc.along[static_cast<size_t>(last)] < distance) {
        i0 = count - 2;
        i1 = last;
        i2 = count;
        i3 = count + 1;
    } else {
        int above = last;
        int below = last - 1;
        bool found = true;
        while (true) {
            if (below == -1) {
                found = false;
                break;
            }
            if (!(distance <= arc.along[static_cast<size_t>(below)])) break;
            above = below;
            below = above - 1;
        }
        if (found) {
            i0 = above - 2;
            i1 = below;
            i2 = above;
            i3 = above + 1;
        }
    }

    if (distance == length) return arc.points[static_cast<size_t>(last)];
    if (distance == 0.0f) return arc.points[0];

    if (i0 == -1) i0 = 0;
    if (count <= i2) i2 = last;
    if (count <= i3) i3 = last;

    const float from = arc.along[static_cast<size_t>(i1)];
    float span = arc.along[static_cast<size_t>(i2)] - from;
    if (span < 0.0f) span = span + length;
    float into = distance - from;
    if (distance < from) into = into + length;
    const float t = into / span;
    const float t2 = t * t;

    const Vec3& a = arc.points[static_cast<size_t>(i0)];
    const Vec3& b = arc.points[static_cast<size_t>(i1)];
    const Vec3& c = arc.points[static_cast<size_t>(i2)];
    const Vec3& d = arc.points[static_cast<size_t>(i3)];
    const auto blend = [&](float p0, float p1, float p2, float p3) {
        return (p1 + p1 + (p2 - p0) * t + ((p0 + p0 + p1 * -5.0f + p2 * 4.0f) - p3) * t2 +
                ((p1 * 3.0f - p0) + p2 * -3.0f + p3) * t2 * t) *
               0.5f;
    };
    return Vec3{blend(a.x, b.x, c.x, d.x), blend(a.y, b.y, c.y, d.y), blend(a.z, b.z, c.z, d.z)};
}

}  // namespace tl::core
