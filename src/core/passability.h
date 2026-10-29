// The pathable grid: which 0.4-unit cells of a level a unit may be routed
// through, and how many there are.
//
// `CLevel::bakeCollision` sizes and clears the grid;
// `CLevel::calculatePassability` casts every cell and
// counts the open ones (`CLevel+0x58`, which `CLevel::populate` scales unit counts by).
// `CAstarPathfinder::tileFree` routes over these cells.
//
// No OGRE, no file I/O.
#pragma once

#include "core/collision.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace tl::core {

class PassabilityMap {
public:
    /// The cell size, in level units (every `/ 0.4` in the functions below).
    static constexpr float kCell = 0.4f;
    /// What the console multiplies by to find a cell: never a division by 0.4,
    /// which rounds differently on a cell's edge.
    static constexpr float kInverseCell = 2.5f;
    /// The width of a room's edge strips `bakeCollision` casts again.
    static constexpr float kStrip = 4.0f;

    /// An `Ogre::AxisAlignedBox` as `bakeCollision` builds one per room layout:
    /// null until the first piece is merged in.
    struct Box {
        Vec3 low;
        Vec3 high;
        bool finite = false;
        /// The room's `.pth` file as read (its layout's path less `LAYOUT`, plus `pth`), or
        /// empty: the cells `calculatePassability` takes instead of casting.
        std::vector<uint8_t> cached;

        /// Merge a finite box in, as the inlined `AxisAlignedBox::merge` in
        /// `bakeCollision` does.
        void merge(const Vec3& other_low, const Vec3& other_high);
    };

    /// What one `calculatePassability` call did, for the log.
    struct Pass {
        /// The ray-cast count, printed as "Level Pathnode Ray Casts :".
        uint32_t ray_casts = 0;
        /// How many cells this call added to `+0x58`.
        uint32_t passable = 0;
        /// The box it was given.
        Vec3 low;
        Vec3 high;
    };

    /// The passability half of `CLevel::bakeCollision`: one
    /// `calculatePassability` over the whole list when `sections` is null, otherwise one per
    /// finite box in order. Cells no pass reached end blocked.
    /// `recalculate`: the room pieces' boxes `bakeCollision` casts again after the rooms' passes
    /// (`CLevel +0xB0`), before each room's four edge strips.
    /// `held`: with pieces held back, `add_held` puts their faces into `world` once the rooms'
    /// passes are done, and their boxes are cast again after the strips.
    void bake(const CollisionWorld& world, const std::vector<Box>* sections,
              const std::vector<Box>* recalculate = nullptr, const std::vector<Box>* held = nullptr,
              const std::function<void()>& add_held = {});

    /// `CLevel::calculatePassability` over
    /// the box `low`..`high`.
    Pass calculate(const CollisionWorld& world, const Vec3& low, const Vec3& high,
                   const std::vector<uint8_t>* cached = nullptr, bool zero_first = false);

    /// `CLevel::clearPassabilityData`: map cells to -1, object cells
    /// to 0.
    void clear_passability_data();

    /// A circle of `radius` (`COLLISION_RADIUS + 0.4`) marked on a grid, as
    /// `CBaseUnit::addToAvoidanceMap` / `removeFromAvoidanceMap`
    /// mark most units: the object grid, or the map
    /// grid for a breakable (`CBaseUnit::ISA`, `Renderer::Impl`'s `breakable_avoidance`).
    /// Object:; map:. A count
    /// per cell, not a flag.
    void increment_map_passability(const Vec3& low, const Vec3& high, float radius);
    void decrement_map_passability(const Vec3& low, const Vec3& high, float radius);
    void increment_object_passability(const Vec3& low, const Vec3& high, float radius);
    void decrement_object_passability(const Vec3& low, const Vec3& high, float radius);

    /// `CLevel::incrementMapPassabilityCollision` / `decrementMapPassabilityCollision`,
    /// for an interactable: a
    /// ray from 1000 above to 1000 below at each cell's minimum corner (a kept quirk), and a
    /// hit on `group`'s faces (`CollisionWorld::ray_collision_group`) marks the map cell.
    /// `box` is the group's world bounds (`CollisionWorld::group_bounds`).
    void increment_map_passability_collision(const Vec3& box_low, const Vec3& box_high,
                                             const CollisionWorld& world, int64_t group);
    void decrement_map_passability_collision(const Vec3& box_low, const Vec3& box_high,
                                             const CollisionWorld& world, int64_t group);

    void clear();

    // PORT: an accessor of the port's copy of `CLevel +0x40`, `+0x48`.
    bool ready() const { return width_ > 0 && height_ > 0; }
    // PORT: an accessor of the port's copy of `CLevel +0x40`, `+0x48`.
    int width() const { return width_; }
    // PORT: an accessor of the port's copy of `CLevel +0x40`, `+0x48`.
    int height() const { return height_; }
    // PORT: an accessor of the port's copy of `CLevel +0x40`, `+0x48`.
    float origin_x() const { return origin_x_; }
    // PORT: an accessor of the port's copy of `CLevel +0x40`, `+0x48`.
    float origin_z() const { return origin_z_; }
    /// The grid's far corner, `ceil(high * 2.5) * 0.4` (X360 `CLevel +0x184`,
    /// `+0x188`).
    // PORT: an accessor of the port's copy of `CLevel +0x40`, `+0x48`.
    float far_x() const { return far_x_; }
    // PORT: an accessor of the port's copy of `CLevel +0x40`, `+0x48`.
    float far_z() const { return far_z_; }

    /// `CLevel+0x58`: cells left open by the first pass of every
    /// `calculatePassability`, before erosion.
    // PORT: an accessor of the port's copy of `CLevel +0x40`, `+0x48`.
    uint32_t passable_cells() const { return passable_; }
    // PORT: an accessor of the port's copy of `CLevel +0x40`; outside the grid, 1.
    int8_t map_value(int x, int z) const { return inside(x, z) ? map_[index_of(x, z)] : 1; }

    /// `+0x58 / 6.5`, as `CLevel::populate`
    /// prints it, in float.
    float pathable_area() const { return static_cast<float>(passable_) / 6.5f; }

    const std::vector<Pass>& passes() const { return passes_; }

    /// The cell a position is in: `floorf((x - origin) / 0.4)`.
    int cell_x(float x) const;
    int cell_z(float z) const;

    /// The centre of a cell, as `calculatePassability` casts it.
    float centre_x(int x) const;
    float centre_z(int z) const;

    /// `CLevel::mapPassable(int, int)`.
    bool map_passable(int x, int z) const;
    /// `CLevel::mapPassable(Vector3)`.
    bool map_passable(const Vec3& at) const;
    /// `CLevel::positionPassable(int, int)`.
    bool position_passable(int x, int z) const;
    /// The object grid's half of `positionPassable`: `object < 1`, as
    /// `CAstarPathfinder::tileFree` reads it.
    bool object_passable(int x, int z) const;
    /// `CLevel::positionPassable(Vector3)`.
    bool position_passable(const Vec3& at) const;

    /// `CLevel::passableBetween(Vector3, Vector3, bool)`:
    /// every 0.1 units along the line;
    /// `map_only` picks `mapPassable` over `positionPassable`.
    bool passable_between(const Vec3& from, const Vec3& to, bool map_only) const;

    /// The height the cell's sphere cast met, for a cell a pass left open. Not a field of the
    /// original.
    float ground_at(int x, int z, float fallback) const;

    // PORT: a test copies a level's grids in.
    friend struct PassabilityMapProbe;
    /// PORT: called after each pass of `bake`, for a check to read the grid between passes.
    std::function<void(const PassabilityMap&, const Pass&)> after_pass;

private:
    /// `increment`/`decrementMapPassability` and `...ObjectPassability`.
    ///
    void adjust_circle(std::vector<int8_t>& grid, const Vec3& low, const Vec3& high, float radius,
                       int delta);
    void adjust_map_passability_collision(const Vec3& box_low, const Vec3& box_high,
                                          const CollisionWorld& world, int64_t group, int delta);
    /// `calculate`'s use of a room's `.pth`: true when it fits the box and was copied into the
    /// map (`x1`, `z1` then the file's size).
    bool take_cached(const std::vector<uint8_t>& cached, int x0, int z0, int& x1, int& z1,
                     Pass& pass);
    /// `calculate`'s cast again: every cell of the box made open first.
    void open_cells(int x0, int z0, int x1, int z1);

    // `CLevel +0x40`'s order, column by column (x, then z). PORT: an index.
    size_t index_of(int x, int z) const {
        return static_cast<size_t>(x) * static_cast<size_t>(height_) + static_cast<size_t>(z);
    }
    // PORT: an accessor of the port's copy of `CLevel +0x40`, `+0x48`.
    bool inside(int x, int z) const { return x >= 0 && z >= 0 && x < width_ && z < height_; }

    /// `CLevel+0x40`: one short per cell, column by column (x, then z).
    std::vector<int8_t> map_;
    /// `CLevel+0x48`, the object (unit avoidance) grid.
    std::vector<int8_t> object_;
    std::vector<float> ground_;

    /// `+0x50`, `+0x54`.
    int width_ = 0;
    int height_ = 0;
    /// `+0x230`, `+0x234`.
    float origin_x_ = 0.0f;
    float origin_z_ = 0.0f;
    /// X360 `+0x184`, `+0x188` (the grid's far corner).
    float far_x_ = 0.0f;
    float far_z_ = 0.0f;
    /// `+0x58`.
    uint32_t passable_ = 0;

    std::vector<Pass> passes_;
};

}  // namespace tl::core
