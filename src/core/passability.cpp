#include "core/passability.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace tl::core {

namespace {

// `CLevel::mapPassable`: a cell of `+0x40` / `+0x48`
// is open below 1.
bool open_cell(int8_t v) { return v < 1; }

// `calculate`'s four probes at chest height, 0.3 out along +x, -x, +z and -z: meeting a
// `nocollide` face blocks the cell.
bool probe_meets_nocollide(const CollisionWorld& world, const CollisionHit& ground) {
    const Vec3 from{ground.point.x, ground.point.y + 1.5f, ground.point.z};
    const Vec3 ends[4] = {
        {ground.point.x + 0.3f, from.y + 0.0f, ground.point.z + 0.0f},
        {from.x + -0.3f, from.y + 0.0f, from.z + 0.0f},
        {from.x + 0.0f, from.y + 0.0f, from.z + 0.3f},
        {from.x + 0.0f, from.y + 0.0f, from.z + -0.3f},
    };
    for (const Vec3& end : ends) {
        CollisionHit probe;
        if (world.pre_sorted_ray_collision(from, end, false, probe) &&
            probe.material == kNoCollideMaterial) {
            return true;
        }
    }
    return false;
}

}  // namespace

// PORT: an accessor.
void PassabilityMap::Box::merge(const Vec3& other_low, const Vec3& other_high) {
    if (!finite) {
        low = other_low;
        high = other_high;
        finite = true;
        return;
    }
    low.x = std::min(low.x, other_low.x);
    low.y = std::min(low.y, other_low.y);
    low.z = std::min(low.z, other_low.z);
    high.x = std::max(high.x, other_high.x);
    high.y = std::max(high.y, other_high.y);
    high.z = std::max(high.z, other_high.z);
}

// `CLevel::clearPassabilityData`: the grids emptied.
void PassabilityMap::clear() {
    map_.clear();
    object_.clear();
    ground_.clear();
    width_ = 0;
    height_ = 0;
    origin_x_ = 0.0f;
    origin_z_ = 0.0f;
    far_x_ = 0.0f;
    far_z_ = 0.0f;
    passable_ = 0;
    passes_.clear();
}

// The cell of a coordinate as every `CLevel` function of the console takes it:
// `floor((v - origin) * 2.5)`, then `fctiwz` (`mapPassable`).
int PassabilityMap::cell_x(float x) const {
    return static_cast<int>(std::floor((x - origin_x_) * kInverseCell));
}

// The cell a position is in, `floor((z - origin) * 2.5)` then `fctiwz`
// (`CLevel::mapPassable`).
int PassabilityMap::cell_z(float z) const {
    return static_cast<int>(std::floor((z - origin_z_) * kInverseCell));
}

// A cell's centre as `calculatePassability` casts it:
// the corner, then the origin, then the half cell.
float PassabilityMap::centre_x(int x) const {
    return (static_cast<float>(x) * kCell + origin_x_) + 0.2f;
}

// A cell's centre as `CLevel::calculatePassability` casts it:
// `(z * 0.4 + origin) + 0.2`.
float PassabilityMap::centre_z(int z) const {
    return (static_cast<float>(z) * kCell + origin_z_) + 0.2f;
}

void PassabilityMap::clear_passability_data() {
    // CLevel::clearPassabilityData
    std::fill(map_.begin(), map_.end(), static_cast<int8_t>(-1));
    std::fill(object_.begin(), object_.end(), static_cast<int8_t>(0));
}

void PassabilityMap::bake(const CollisionWorld& world, const std::vector<Box>* sections,
                          const std::vector<Box>* recalculate, const std::vector<Box>* held,
                          const std::function<void()>& add_held) {
    // CLevel::bakeCollision, from
    // "Passability Calculation Time" back to the grid's allocation.
    clear();
    Vec3 low, high;
    if (sections != nullptr) {
        // the grid's box is the rooms' boxes merged (the Linux
        // build sizes it from the level list's bounds).
        Box rooms;
        for (const Box& box : *sections) {
            if (box.finite) rooms.merge(box.low, box.high);
        }
        if (!rooms.finite) return;
        low = rooms.low;
        high = rooms.high;
    } else if (!world.level_bounds(low, high)) {
        return;
    }

    // all in single precision, the size truncated.
    origin_x_ = std::floor(low.x * kInverseCell) * kCell;
    origin_z_ = std::floor(low.z * kInverseCell) * kCell;
    far_x_ = std::ceil(high.x * kInverseCell) * kCell;
    far_z_ = std::ceil(high.z * kInverseCell) * kCell;
    width_ = static_cast<int>(static_cast<int64_t>((far_x_ - origin_x_) * kInverseCell));
    height_ = static_cast<int>(static_cast<int64_t>((far_z_ - origin_z_) * kInverseCell));
    if (width_ <= 0 || height_ <= 0) {
        width_ = 0;
        height_ = 0;
        return;
    }

    const size_t cells = static_cast<size_t>(width_) * static_cast<size_t>(height_);
    map_.assign(cells, 0);
    object_.assign(cells, 0);
    ground_.assign(cells, 0.0f);
    clear_passability_data();

    if (sections == nullptr) {
        // No chunks (`CLevel+0x1e8` empty): the whole list at once.
        passes_.push_back(calculate(world, low, high));
        if (after_pass) after_pass(*this, passes_.back());
    } else {
        // after each room's pass its four edges, 4 wide, join the
        // boxes cast again: west, south, east, north.
        std::vector<Box> again;
        if (recalculate != nullptr) again = *recalculate;
        for (const Box& box : *sections) {
            // A null box - a room layout with no collision-enabled piece - is skipped.
            if (!box.finite) continue;
            passes_.push_back(calculate(world, box.low, box.high, &box.cached));
            if (after_pass) after_pass(*this, passes_.back());
            const Vec3& lo = box.low;
            const Vec3& hi = box.high;
            const float dx = hi.x - lo.x;
            const float dz = hi.z - lo.z;
            const Box strips[4] = {
                {lo, {lo.x + kStrip, lo.y, lo.z + dz}, true, {}},
                {lo, {dx + lo.x, lo.y, lo.z + kStrip}, true, {}},
                {{hi.x - kStrip, hi.y, hi.z - dz}, hi, true, {}},
                {{hi.x - dx, hi.y, hi.z - kStrip}, hi, true, {}},
            };
            for (const Box& strip : strips) again.push_back(strip);
        }
        // the held pieces join the collision, their boxes after the
        // strips.
        if (held != nullptr) {
            if (add_held) add_held();
            for (const Box& box : *held) again.push_back(box);
        }
        // each cast again from open, without a `.pth`.
        for (const Box& box : again) {
            if (!box.finite) continue;
            passes_.push_back(calculate(world, box.low, box.high, nullptr, true));
            if (after_pass) after_pass(*this, passes_.back());
        }
    }

    // Whatever no pass reached is blocked.
    for (int8_t& v : map_) {
        if (v == -1) v = 1;
    }
}

// the room's `.pth` - a width and a height (little-endian words)
// and a byte a cell, z by z - is used when its size is the box's, or within one cell of it each
// way (then the box takes the file's size); otherwise the cells are cast.
bool PassabilityMap::take_cached(const std::vector<uint8_t>& cached, int x0, int z0, int& x1,
                                 int& z1, Pass& pass) {
    if (cached.size() < 8) return false;
    const auto word = [&cached](size_t at) {
        return static_cast<int>(static_cast<uint32_t>(cached[at]) |
                                static_cast<uint32_t>(cached[at + 1]) << 8 |
                                static_cast<uint32_t>(cached[at + 2]) << 16 |
                                static_cast<uint32_t>(cached[at + 3]) << 24);
    };
    const int file_width = word(0);
    const int file_height = word(4);
    if (!(std::abs(file_width - (x1 - x0)) < 2 && std::abs(file_height - (z1 - z0)) < 2 &&
          0 < file_width && 0 < file_height &&
          8 + static_cast<size_t>(file_width) * static_cast<size_t>(file_height) <=
              cached.size())) {
        return false;
    }
    x1 = x0 + file_width;
    z1 = z0 + file_height;
    // each cell the file's, open ones counted; the pass that closes
    // one-cell gaps is not run.
    for (int x = x0; x < x1; ++x) {
        for (int z = z0; z < z1; ++z) {
            if (z < 0 || x < 0 || width_ <= x || height_ <= z) continue;
            const auto value = static_cast<int8_t>(
                cached[8 + static_cast<size_t>(z - z0) * static_cast<size_t>(file_width) +
                       static_cast<size_t>(x - x0)]);
            map_[index_of(x, z)] = value;
            if (value == 0) ++pass.passable;
        }
    }
    passable_ += pass.passable;
    return true;
}

// cast again, every cell of the box is made open first.
void PassabilityMap::open_cells(int x0, int z0, int x1, int z1) {
    for (int x = x0; x < x1; ++x) {
        for (int z = z0; z < z1; ++z) {
            if (z < 0 || x < 0 || width_ <= x || height_ <= z) continue;
            map_[index_of(x, z)] = 0;
        }
    }
}

PassabilityMap::Pass PassabilityMap::calculate(const CollisionWorld& world, const Vec3& low,
                                               const Vec3& high, const std::vector<uint8_t>* cached,
                                               bool zero_first) {
    // CLevel::calculatePassability
    Pass pass;
    pass.low = low;
    pass.high = high;
    if (!ready()) return pass;

    // times 2.5 as every cell index is.
    const int x0 = static_cast<int>(std::floor((low.x - origin_x_) * kInverseCell)) - 1;
    const int z0 = static_cast<int>(std::floor((low.z - origin_z_) * kInverseCell)) - 1;
    int x1 = static_cast<int>(std::ceil((high.x - origin_x_) * kInverseCell)) + 1;
    int z1 = static_cast<int>(std::ceil((high.z - origin_z_) * kInverseCell)) + 1;

    if (cached != nullptr && take_cached(*cached, x0, z0, x1, z1, pass)) return pass;
    if (zero_first) open_cells(x0, z0, x1, z1);

    // First pass: cast every cell, and count the ones left open.
    for (int x = x0; x < x1; ++x) {
        for (int z = z0; z < z1; ++z) {
            if (z < 0 || x < 0 || width_ <= x || height_ <= z) continue;

            int8_t& cell = map_[index_of(x, z)];
            const int8_t was = cell;
            int8_t value = was == -1 ? 0 : was;
            // Only an untouched (-1) or open (0) cell is cast again; a cell an
            // earlier pass blocked keeps its value and is not counted.
            if (static_cast<uint8_t>(was + 1) > 1) {
                cell = value;
                if (value == 0) ++pass.passable;
                continue;
            }

            const float px = centre_x(x);
            const float pz = centre_z(z);
            // The box is the centre less 0.2 less 1, and the centre plus 1.2 in
            // one add.
            world.sort_for_collision({(px - 0.2f) - 1.0f, -200.0f, (pz - 0.2f) - 1.0f},
                                     {px + 1.2f, 200.0f, pz + 1.2f});
            ++pass.ray_casts;

            CollisionHit ground;
            const bool hit = world.pre_sorted_sphere_collision({px, 200.0f, pz}, {px, -200.0f, pz},
                                                               0.1f, false, ground);
            if (!hit || 80.0f < ground.point.y || ground.point.y < -80.0f ||
                ground.material == kNoCollideMaterial) {
                value = 1;
            } else {
                if (probe_meets_nocollide(world, ground)) value = 1;
                ground_[index_of(x, z)] = ground.point.y;
            }
            cell = value;
            if (value == 0) ++pass.passable;
        }
    }

    // Second pass: close one-cell gaps. In place, x then z, so a cell sees the
    // neighbours this pass has already closed. It does not touch the count.
    auto blocked = [&](int x, int z) { return 0 < map_[index_of(x, z)]; };
    auto open = [&](int x, int z) { return map_[index_of(x, z)] < 1; };
    int x = x0;
    do {
        for (int z = z0; z < z1; ++z) {
            if (z < 0 || x < 0 || width_ <= x || height_ <= z) continue;
            int8_t& cell = map_[index_of(x, z)];
            if (cell != 0) continue;

            const bool left_blocked = x - 1 < 0 || width_ <= x - 1 || blocked(x - 1, z);
            const bool right_blocked = x + 1 < 0 || width_ <= x + 1 || blocked(x + 1, z);
            if (left_blocked && right_blocked) {
                cell = 1;
                continue;
            }

            // Diagonal neighbours: +x -z, and -x +z.
            auto plus_minus_open = [&]() {
                return -1 < z - 1 && x + 1 < width_ && z - 1 < height_ && open(x + 1, z - 1);
            };
            auto minus_plus_open = [&]() {
                return -1 < z + 1 && -1 < x - 1 && x - 1 < width_ && z + 1 < height_ &&
                       open(x - 1, z + 1);
            };

            bool keep = false;
            bool check_minus_minus = false;
            bool check_plus_plus = false;
            const bool down_blocked = z - 1 < 0 || height_ <= z - 1 || blocked(x, z - 1);
            if (down_blocked) {
                const bool up_open = -1 < z + 1 && z + 1 < height_ && open(x, z + 1);
                if (up_open) {
                    if (-1 < z - 1) {
                        check_minus_minus = true;
                    } else {
                        check_plus_plus = true;
                    }
                }
            } else {
                check_minus_minus = true;
            }

            if (check_minus_minus) {
                const bool minus_minus_blocked =
                    x - 1 < 0 || width_ <= x - 1 || height_ <= z - 1 || blocked(x - 1, z - 1);
                if (minus_minus_blocked) {
                    check_plus_plus = true;
                } else {
                    if (-1 < x + 1 && plus_minus_open()) {
                        keep = true;
                    } else if (minus_plus_open()) {
                        keep = true;
                    }
                }
            }
            if (check_plus_plus) {
                const bool plus_plus_open = -1 < z + 1 && -1 < x + 1 && x + 1 < width_ &&
                                            z + 1 < height_ && open(x + 1, z + 1);
                if (plus_plus_open) {
                    if (plus_minus_open()) {
                        keep = true;
                    } else if (minus_plus_open()) {
                        keep = true;
                    }
                }
            }
            if (!keep) cell = 1;
        }
        ++x;
    } while (x < x1);

    passable_ += pass.passable;
    return pass;
}

namespace {

/// `max(0, v)` capped to `limit`, applied to each cell bound by
/// `CLevel::incrementMapPassability` and its three siblings.
int clamp_cell(int v, int limit) {
    if (v < 0) return 0;
    if (v > limit) return limit;
    return v;
}

}  // namespace

void PassabilityMap::adjust_circle(std::vector<int8_t>& grid, const Vec3& low, const Vec3& high,
                                   float radius, int delta) {
    // CLevel::incrementMapPassability / incrementObjectPassability,
    // their decrement twins,
    // and the `AxisAlignedBox` overloads:
    // `CLevel::incrementMapPassability(AxisAlignedBox, float)`,
    // `CLevel::decrementMapPassability(AxisAlignedBox, float)`,
    // `CLevel::incrementObjectPassability(AxisAlignedBox, float)`,
    // `CLevel::decrementObjectPassability(AxisAlignedBox, float)`.
    // The centre is the box's middle, the cells its floor()ed corners clamped to the grid.
    if (!ready()) return;

    const int x_lo = clamp_cell(cell_x(low.x), width_);
    const int x_hi = clamp_cell(cell_x(high.x), width_);
    const int z_lo = clamp_cell(cell_z(low.z), height_);
    const int z_hi = clamp_cell(cell_z(high.z), height_);
    if (x_lo >= x_hi || z_lo >= z_hi) return;

    const bool test_radius = radius != 0.0f;
    const float centre_x = (low.x + high.x) * 0.5f;
    const float centre_z = (low.z + high.z) * 0.5f;

    for (int x = x_lo; x < x_hi; ++x) {
        for (int z = z_lo; z < z_hi; ++z) {
            if (test_radius) {
                // The cell's minimum corner, not its centre (a kept quirk). Fused as
                // the console has it.
                const float dx = centre_x - std::fma(static_cast<float>(x), kCell, origin_x_);
                const float dz = centre_z - std::fma(static_cast<float>(z), kCell, origin_z_);
                if (radius < std::sqrt(std::fma(dz, dz, dx * dx))) continue;
            }
            // A signed byte per cell on the console (`lbzx`/`stbx`): it wraps.
            int8_t& cell = grid[index_of(x, z)];
            cell = static_cast<int8_t>(cell + delta);
        }
    }
}

// `CLevel::incrementMapPassability`: the map
// grid's cells under the circle, plus one (`adjust_circle`).
void PassabilityMap::increment_map_passability(const Vec3& low, const Vec3& high, float radius) {
    adjust_circle(map_, low, high, radius, 1);
}

// `CLevel::decrementMapPassability`: the map
// grid's cells under the circle, less one (`adjust_circle`).
void PassabilityMap::decrement_map_passability(const Vec3& low, const Vec3& high, float radius) {
    adjust_circle(map_, low, high, radius, -1);
}

// `CLevel::incrementObjectPassability`: the
// object grid's cells under the circle, plus one (`adjust_circle`).
void PassabilityMap::increment_object_passability(const Vec3& low, const Vec3& high, float radius) {
    adjust_circle(object_, low, high, radius, 1);
}

// `CLevel::decrementObjectPassability`: the
// object grid's cells under the circle, less one (`adjust_circle`).
void PassabilityMap::decrement_object_passability(const Vec3& low, const Vec3& high, float radius) {
    adjust_circle(object_, low, high, radius, -1);
}

void PassabilityMap::adjust_map_passability_collision(const Vec3& box_low, const Vec3& box_high,
                                                      const CollisionWorld& world, int64_t group,
                                                      int delta) {
    // `CLevel::incrementMapPassabilityCollision` /
    // `CLevel::decrementMapPassabilityCollision`.
    if (!ready()) return;

    const int x_lo = clamp_cell(cell_x(box_low.x), width_);
    const int x_hi = clamp_cell(cell_x(box_high.x), width_);
    const int z_lo = clamp_cell(cell_z(box_low.z), height_);
    const int z_hi = clamp_cell(cell_z(box_high.z), height_);
    if (x_lo >= x_hi || z_lo >= z_hi) return;

    for (int x = x_lo; x < x_hi; ++x) {
        // The cell's own minimum corner, not its centre - as in adjust_circle().
        const float wx = static_cast<float>(x) * kCell + origin_x_;
        for (int z = z_lo; z < z_hi; ++z) {
            // Unlike x, fused.
            const float wz = std::fma(static_cast<float>(z), kCell, origin_z_);
            CollisionHit hit;
            if (world.ray_collision_group(group, {wx, 1000.0f, wz}, {wx, -1000.0f, wz}, hit)) {
                int8_t& cell = map_[index_of(x, z)];
                cell = static_cast<int8_t>(cell + delta);
            }
        }
    }
}

// `CLevel::incrementMapPassabilityCollision`.
void PassabilityMap::increment_map_passability_collision(const Vec3& box_low, const Vec3& box_high,
                                                         const CollisionWorld& world,
                                                         int64_t group) {
    adjust_map_passability_collision(box_low, box_high, world, group, 1);
}

// `CLevel::decrementMapPassabilityCollision`.
void PassabilityMap::decrement_map_passability_collision(const Vec3& box_low, const Vec3& box_high,
                                                         const CollisionWorld& world,
                                                         int64_t group) {
    adjust_map_passability_collision(box_low, box_high, world, group, -1);
}

bool PassabilityMap::map_passable(int x, int z) const {
    // CLevel::mapPassable
    if (!inside(x, z)) return false;
    return open_cell(map_[index_of(x, z)]);
}

bool PassabilityMap::map_passable(const Vec3& at) const {
    // CLevel::mapPassable
    return map_passable(cell_x(at.x), cell_z(at.z));
}

bool PassabilityMap::position_passable(int x, int z) const {
    // CLevel::positionPassable
    if (!inside(x, z)) return false;
    return open_cell(map_[index_of(x, z)]) && open_cell(object_[index_of(x, z)]);
}

// The object grid's half of `CAstarPathfinder::tileFree`.
bool PassabilityMap::object_passable(int x, int z) const {
    if (!inside(x, z)) return false;
    return open_cell(object_[index_of(x, z)]);
}

bool PassabilityMap::position_passable(const Vec3& at) const {
    // CLevel::positionPassable
    return position_passable(cell_x(at.x), cell_z(at.z));
}

bool PassabilityMap::passable_between(const Vec3& from, const Vec3& to, bool map_only) const {
    // CLevel::passableBetween
    float dx = to.x - from.x;
    float dz = to.z - from.z;
    const float squared = dx * dx + 0.0f + dz * dz;
    const float distance = std::sqrt(squared);
    float dy = 0.0f;
    if (1e-08f < distance) {
        const float inv = 1.0f / distance;
        dx = dx * inv;
        dz = dz * inv;
        dy = inv * 0.0f;
    }
    Vec3 at = from;
    float travelled = 0.0f;
    if (0.0f <= distance) {
        while (true) {
            const bool ok = map_only ? map_passable(at) : position_passable(at);
            if (!ok) return false;
            travelled = travelled + 0.1f;
            at.x = dx * 0.1f + at.x;
            at.y = dy * 0.1f + at.y;
            if (distance < travelled) break;
            // The z step comes after the test, as in the original.
            at.z = at.z + dz * 0.1f;
        }
    }
    return true;
}

// PORT: the ground the port recorded at an open cell.
float PassabilityMap::ground_at(int x, int z, float fallback) const {
    if (!inside(x, z) || !map_passable(x, z)) return fallback;
    return ground_[index_of(x, z)];
}

}  // namespace tl::core
