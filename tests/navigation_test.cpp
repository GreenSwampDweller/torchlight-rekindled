// The navigation grid and the route it finds across it, on hand-built worlds.
#include "core/navigation.h"
#include "core/collision.h"
#include "core/input.h"
#include "core/player.h"
#include "test_support.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using tl::core::CollisionWorld;
using tl::core::NavGrid;
using tl::core::PassabilityMap;
using tl::core::Vec3;

/// A flat square of ground, `size` on a side, centred on the origin, facing up.
void add_floor(CollisionWorld& world, float size, float y = 0.0f) {
    const float h = size * 0.5f;
    world.add_triangle({-h, y, -h}, {-h, y, h}, {h, y, h});
    world.add_triangle({-h, y, -h}, {h, y, h}, {h, y, -h});
}

/// A `nocollide` wall from (x0, z0) to (x1, z1), `height` tall, with a face on
/// each side.
void add_wall(CollisionWorld& world, float x0, float z0, float x1, float z1, float height) {
    const Vec3 a{x0, 0.0f, z0}, b{x1, 0.0f, z1}, c{x1, height, z1}, d{x0, height, z0};
    world.add_triangle(a, b, c, tl::core::kNoCollideMaterial);
    world.add_triangle(a, c, d, tl::core::kNoCollideMaterial);
    world.add_triangle(a, c, b, tl::core::kNoCollideMaterial);
    world.add_triangle(a, d, c, tl::core::kNoCollideMaterial);
}

/// The route grid over a world, through its passability bake. The grid is kept
/// alive by the caller's `PassabilityMap`.
void build_nav(NavGrid& nav, PassabilityMap& grid, const CollisionWorld& world) {
    grid.bake(world, nullptr);
    nav.build(grid);
}

float route_length(const Vec3& from, const std::vector<Vec3>& route) {
    float total = 0.0f;
    Vec3 at = from;
    for (const Vec3& step : route) {
        const float dx = step.x - at.x;
        const float dz = step.z - at.z;
        total += std::sqrt(dx * dx + dz * dz);
        at = step;
    }
    return total;
}

}  // namespace

TL_TEST(a_nav_grid_over_open_ground_is_walkable_in_the_middle_and_not_outside) {
    CollisionWorld world;
    add_floor(world, 20.0f);
    world.build();

    NavGrid nav;
    PassabilityMap grid;
    build_nav(nav, grid, world);
    TL_CHECK(nav.ready());
    TL_CHECK(nav.walkable_count() > 0);

    TL_CHECK(nav.walkable(0.0f, 0.0f));
    TL_CHECK(nav.walkable(5.0f, -5.0f));
    // Off the end of the floor there is no ground, so there is nowhere to stand.
    TL_CHECK(!nav.walkable(50.0f, 0.0f));
}

// `CAstarPathfinder` never strings a route: a step costs 1.0, diagonal or not, so open
// ground is a diagonal run then straight steps, one waypoint per cell.
TL_TEST(a_route_across_open_ground_is_the_literal_cell_by_cell_staircase) {
    CollisionWorld world;
    add_floor(world, 40.0f);
    world.build();

    NavGrid nav;
    PassabilityMap grid;
    build_nav(nav, grid, world);

    const Vec3 from{-8.0f, 0.0f, 0.0f};
    const Vec3 to{8.0f, 0.0f, 0.0f};
    const std::vector<Vec3> route = nav.find_path(from, to, grid);
    TL_CHECK(!route.empty());
    // 16 units at 0.4 a cell is 40 steps - no corner is ever dropped.
    TL_CHECK(route.size() > size_t{30});
    TL_CHECK(std::fabs(route.back().x - 8.0f) < 0.6f);
    TL_CHECK(std::fabs(route.back().z) < 0.6f);
    // Every step is exactly one cell (or a diagonal of one), never a strung
    // long leg.
    Vec3 at = from;
    for (const Vec3& step : route) {
        const float dx = std::fabs(step.x - at.x);
        const float dz = std::fabs(step.z - at.z);
        TL_CHECK(dx < PassabilityMap::kCell * 1.6f);
        TL_CHECK(dz < PassabilityMap::kCell * 1.6f);
        at = step;
    }
}

// The thing a grid exists for: a wall between the two ends, with a gap at one
// side, and the route has to use the gap.
TL_TEST(a_route_goes_round_a_wall_rather_than_through_it) {
    CollisionWorld world;
    add_floor(world, 40.0f);
    // Across the middle, from z = -20 up to z = 6, leaving a gap at the top.
    add_wall(world, 0.0f, -20.0f, 0.0f, 6.0f, 4.0f);
    world.build();

    NavGrid nav;
    PassabilityMap grid;
    build_nav(nav, grid, world);
    TL_CHECK(nav.ready());

    // The wall itself is not walkable, and neither is the body's width beside it.
    TL_CHECK(!nav.walkable(0.0f, 0.0f));

    const Vec3 from{-8.0f, 0.0f, 0.0f};
    const Vec3 to{8.0f, 0.0f, 0.0f};
    const std::vector<Vec3> route = nav.find_path(from, to, grid);
    TL_CHECK(!route.empty());

    // It ends where it was asked to.
    TL_CHECK(std::fabs(route.back().x - 8.0f) < 1.0f);
    TL_CHECK(std::fabs(route.back().z) < 1.0f);

    // It is longer than the straight line, because it went round.
    TL_CHECK(route_length(from, route) > 16.0f + 4.0f);

    // And every cell of it is walkable ground.
    for (const Vec3& step : route) TL_CHECK(nav.walkable(step.x, step.z));

    // It went past the *open* end of the wall, north of z = 6.
    float furthest = 0.0f;
    for (const Vec3& step : route) furthest = std::max(furthest, step.z);
    TL_CHECK(furthest > 5.0f);
}

// `createPath` caps the search at 150 expansions and falls back to the closest
// explored node; empty only when nothing was explored or both ends share a cell.
TL_TEST(a_walled_off_destination_gets_the_closest_approach_not_a_failure) {
    CollisionWorld world;
    add_floor(world, 40.0f);
    // All the way across, both ends past the floor: no gap at all.
    add_wall(world, 0.0f, -21.0f, 0.0f, 21.0f, 4.0f);
    world.build();

    NavGrid nav;
    PassabilityMap grid;
    build_nav(nav, grid, world);
    const std::vector<Vec3> route = nav.find_path({-8.0f, 0.0f, 0.0f}, {8.0f, 0.0f, 0.0f}, grid);
    TL_CHECK(!route.empty());
    // It gets closer to the target than it started, but never past the wall.
    TL_CHECK(route.back().x > -8.0f);
    TL_CHECK(route.back().x < -0.5f);
    for (const Vec3& step : route) TL_CHECK(step.x < -0.5f);
}

// A destination inside a wall is snapped to the nearest open ground.
TL_TEST(a_destination_inside_a_wall_is_snapped_to_the_nearest_open_ground) {
    CollisionWorld world;
    add_floor(world, 40.0f);
    add_wall(world, 6.0f, -20.0f, 6.0f, 20.0f, 4.0f);
    world.build();

    NavGrid nav;
    PassabilityMap grid;
    build_nav(nav, grid, world);
    TL_CHECK(!nav.walkable(6.0f, 0.0f));

    const std::vector<Vec3> route = nav.find_path({-8.0f, 0.0f, 0.0f}, {6.0f, 0.0f, 0.0f}, grid);
    TL_CHECK(!route.empty());
    TL_CHECK(nav.walkable(route.back().x, route.back().z));
    // Near it, not at it.
    TL_CHECK(std::fabs(route.back().x - 6.0f) < 2.5f);
}

// A path blocker goes onto the map grid (`CPropertyNode::updatePathNodes`,
// `incrementMapPassability`); the decrement opens it again.
TL_TEST(a_path_blocker_closes_the_map_grid_and_its_decrement_opens_it) {
    CollisionWorld world;
    add_floor(world, 20.0f);
    world.build();

    NavGrid nav;
    PassabilityMap grid;
    build_nav(nav, grid, world);
    TL_CHECK(nav.walkable(0.0f, 0.0f));
    TL_CHECK(!nav.blocked(0.0f, 0.0f));

    const Vec3 low{-1.0f, -1000.0f, -1.0f};
    const Vec3 high{1.0f, 1000.0f, 1.0f};
    grid.increment_map_passability(low, high, 0.0f);
    nav.refresh(grid, low, high);
    nav.mark_blocker(low, high, true);
    TL_CHECK(!grid.map_passable(Vec3{0.0f, 0.0f, 0.0f}));
    TL_CHECK(!nav.walkable(0.0f, 0.0f));
    TL_CHECK(nav.blocked(0.0f, 0.0f));
    TL_CHECK(nav.walkable(5.0f, 5.0f));
    TL_CHECK(!nav.blocked(5.0f, 5.0f));

    grid.decrement_map_passability(low, high, 0.0f);
    nav.refresh(grid, low, high);
    nav.mark_blocker(low, high, false);
    TL_CHECK(grid.map_passable(Vec3{0.0f, 0.0f, 0.0f}));
    TL_CHECK(nav.walkable(0.0f, 0.0f));
    TL_CHECK(!nav.blocked(0.0f, 0.0f));

    nav.mark_blocker(low, high, true);
    nav.clear_blockers();
    TL_CHECK(!nav.blocked(0.0f, 0.0f));
}

// Player::update() refuses a step into a cell an active blocker covers.
TL_TEST(the_stick_is_refused_a_step_into_an_active_path_blocker) {
    CollisionWorld world;
    add_floor(world, 40.0f);
    world.build();

    NavGrid nav;
    PassabilityMap grid;
    build_nav(nav, grid, world);
    nav.mark_blocker({2.0f, -1000.0f, -20.0f}, {6.0f, 1000.0f, 20.0f}, true);
    TL_CHECK(nav.blocked(4.0f, 0.0f));

    tl::core::Player player;
    player.place({0.0f, 0.0f, 0.0f}, 0.0f);
    player.set_body(tl::core::Body{});
    player.set_stats(tl::core::MovementStats{6.25f, 6.25f});

    tl::core::InputState stick;
    // At camera_yaw 0, Player::update()'s own formula gives dir_x = -move_x:
    // -1 here is what walks towards +x, where the blocker sits.
    stick.move_x = -1.0f;
    stick.move_y = 0.0f;

    // Run enough frames to reach the blocker if nothing stopped the character;
    // at RUNNINGSPEED 6.25 units/s, 4 units away is under a second.
    for (int i = 0; i < 180; ++i) {
        player.update(1.0f / 60.0f, stick, 0.0f, world, &grid, &nav);
    }
    // Stopped at the blocker's edge, not through it.
    TL_CHECK(std::fabs(player.position().x) < 4.0f);
}

// A slide along a wall must not end in a closed cell: from inside one the map's
// refusal lifts, and the town's northern border was a way off the level.
TL_TEST(sliding_along_a_wall_never_leaves_the_character_in_a_closed_cell) {
    CollisionWorld world;
    add_floor(world, 40.0f);
    // A slanted wall, so a push into it slides.
    add_wall(world, 3.0f, -10.0f, 9.0f, 10.0f, 3.0f);
    world.build();

    NavGrid nav;
    PassabilityMap grid;
    build_nav(nav, grid, world);

    for (const float move_y : {1.0f, -1.0f}) {
        tl::core::Player player;
        player.place({0.0f, 0.0f, 0.0f}, 0.0f);
        player.set_body(tl::core::Body{});
        player.set_stats(tl::core::MovementStats{6.25f, 6.25f});

        tl::core::InputState stick;
        stick.move_x = -1.0f;  // towards +x, into the wall
        stick.move_y = move_y;
        for (int i = 0; i < 600; ++i) {
            player.update(1.0f / 60.0f, stick, 0.0f, world, &grid, &nav);
            TL_CHECK(grid.map_passable(player.position()));
        }
    }
}

// The route grid is the passability grid: a cell is walkable exactly when
// `CLevel::mapPassable` says so.
TL_TEST(every_walkable_cell_is_an_open_passability_cell) {
    CollisionWorld world;
    add_floor(world, 20.0f);
    add_wall(world, -3.0f, -3.0f, 3.0f, -3.0f, 4.0f);
    add_wall(world, 3.0f, -3.0f, 3.0f, 3.0f, 4.0f);
    world.build();

    NavGrid nav;
    PassabilityMap grid;
    build_nav(nav, grid, world);

    size_t checked = 0;
    for (float z = -9.0f; z <= 9.0f; z += 0.4f) {
        for (float x = -9.0f; x <= 9.0f; x += 0.4f) {
            TL_CHECK_EQ(nav.walkable(x, z), grid.map_passable(Vec3{x, 0.0f, z}));
            if (nav.walkable(x, z)) ++checked;
        }
    }
    TL_CHECK(checked > 100);
    TL_CHECK(!nav.walkable(0.2f, -3.0f));
}

// An opened door opens the route grid too: `refresh` carries `removeFromAvoidanceMap` into
// the snapshot `NavGrid::build` took.
TL_TEST(refreshing_the_route_grid_after_a_door_opens_lets_routes_through) {
    CollisionWorld world;
    add_floor(world, 40.0f);
    // The door: its own group, a slab standing across x = 0 (its top face is
    // what the per-cell probe hits).
    constexpr int64_t kDoor = 77;
    world.set_group(kDoor);
    const float y = 2.0f;
    world.add_triangle({-0.4f, y, -20.0f}, {-0.4f, y, 20.0f}, {0.4f, y, 20.0f});
    world.add_triangle({-0.4f, y, -20.0f}, {0.4f, y, 20.0f}, {0.4f, y, -20.0f});
    world.set_group(0);
    world.build();

    PassabilityMap grid;
    grid.bake(world, nullptr);
    Vec3 low, high;
    TL_CHECK(world.group_bounds(kDoor, low, high));
    grid.increment_map_passability_collision(low, high, world, kDoor);
    NavGrid nav;
    nav.build(grid);
    TL_CHECK(!nav.walkable(0.0f, 0.0f));

    grid.decrement_map_passability_collision(low, high, world, kDoor);
    TL_CHECK(!nav.walkable(0.0f, 0.0f));  // the snapshot, until refreshed
    nav.refresh(grid, low, high);
    TL_CHECK(nav.walkable(0.0f, 0.0f));
    const std::vector<Vec3> route = nav.find_path({-5.0f, 0.0f, 0.0f}, {5.0f, 0.0f, 0.0f}, grid);
    TL_CHECK(!route.empty());
    TL_CHECK(route_length({-5.0f, 0.0f, 0.0f}, route) < 11.0f);
}

namespace {

/// A floor 20 units square with a band two cells deep closed right across it at
/// z = 0, but for a gap `gap_cells` wide starting at the cell under x = 0.
void build_band_with_gap(NavGrid& nav, PassabilityMap& grid, int gap_cells) {
    CollisionWorld world;
    add_floor(world, 20.0f);
    world.build();
    build_nav(nav, grid, world);
    const float cell = PassabilityMap::kCell;
    // Mid-cell coordinates, so each box covers whole cells: from the cell under
    // `low` up to, not including, the cell under `high`.
    auto at_x = [&](int cx) {
        return grid.origin_x() + cell * static_cast<float>(cx) + cell * 0.5f;
    };
    auto at_z = [&](int cz) {
        return grid.origin_z() + cell * static_cast<float>(cz) + cell * 0.5f;
    };
    const int gap = grid.cell_x(0.0f);
    const int row = grid.cell_z(0.0f);
    const Vec3 left_low{at_x(0), -1000.0f, at_z(row)};
    const Vec3 left_high{at_x(gap), 1000.0f, at_z(row + 2)};
    const Vec3 right_low{at_x(gap + gap_cells), -1000.0f, at_z(row)};
    const Vec3 right_high{at_x(grid.width()), 1000.0f, at_z(row + 2)};
    grid.increment_map_passability(left_low, left_high, 0.0f);
    grid.increment_map_passability(right_low, right_high, 0.0f);
    nav.refresh(grid, left_low, left_high);
    nav.refresh(grid, right_low, right_high);
}

}  // namespace

// `CAstarPathfinder::generateSuccessors`: an orthogonal step needs a
// diagonal beside it open, so a search never enters a one-cell passage.
TL_TEST(a_route_search_does_not_enter_a_passage_one_cell_wide) {
    NavGrid nav;
    PassabilityMap grid;
    build_band_with_gap(nav, grid, 1);
    const float gap_x = grid.centre_x(grid.cell_x(0.0f));
    TL_CHECK(grid.map_passable(Vec3{gap_x, 0.0f, 0.1f}));
    TL_CHECK(!grid.map_passable(Vec3{gap_x - PassabilityMap::kCell, 0.0f, 0.1f}));

    std::vector<Vec3> nodes;
    const bool found = nav.find_nodes({gap_x, 0.0f, -3.0f}, {gap_x, 0.0f, 3.0f}, grid, true, nodes);
    TL_CHECK(found);
    TL_CHECK(!nodes.empty());
    for (const Vec3& node : nodes) TL_CHECK(node.z < 0.0f);
}

// Two cells wide is enough: each step into and through it has a diagonal open
// beside it.
TL_TEST(a_route_search_goes_through_a_passage_two_cells_wide) {
    NavGrid nav;
    PassabilityMap grid;
    build_band_with_gap(nav, grid, 2);
    const float gap_x = grid.centre_x(grid.cell_x(0.0f));

    std::vector<Vec3> nodes;
    TL_CHECK(nav.find_nodes({gap_x, 0.0f, -3.0f}, {gap_x, 0.0f, 3.0f}, grid, true, nodes));
    TL_CHECK(!nodes.empty());
    TL_CHECK(std::fabs(nodes.back().z - 3.0f) < PassabilityMap::kCell);
    // The cell-by-cell staircase, never a long leg through the band.
    for (size_t i = 1; i < nodes.size(); ++i) {
        TL_CHECK(std::fabs(nodes[i].z - nodes[i - 1].z) < PassabilityMap::kCell * 1.5f);
    }
}

namespace {

/// A 60-unit floor cluttered with closed boxes on the map grid and unit circles on the object
/// grid, from a fixed generator, so a run of searches meets dead ends, detours and ties.
void build_cluttered(NavGrid& nav, PassabilityMap& grid, uint32_t& seed) {
    CollisionWorld world;
    add_floor(world, 60.0f);
    world.build();
    build_nav(nav, grid, world);
    auto next = [&seed](float span) {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24) * span;
    };
    for (int i = 0; i < 70; ++i) {
        const float x = next(56.0f) - 28.0f;
        const float z = next(56.0f) - 28.0f;
        const bool across = next(1.0f) < 0.5f;
        const Vec3 low{x, -1000.0f, z};
        const Vec3 high{x + (across ? next(12.0f) + 0.4f : 0.9f), 1000.0f,
                        z + (across ? 0.9f : next(12.0f) + 0.4f)};
        grid.increment_map_passability(low, high, 0.0f);
        nav.refresh(grid, low, high);
    }
    for (int i = 0; i < 120; ++i) {
        const Vec3 at{next(56.0f) - 28.0f, 0.0f, next(56.0f) - 28.0f};
        grid.increment_object_passability(at, at, 0.5f + next(0.6f));
    }
}

}  // namespace

// `CAstarPathfinder::createPath`: thousands of searches, both expansion caps,
// with and without the object grid, every route folded into one number. The number was taken
// from the open list as the console keeps it (a linked list, each new node before the first
// whose f as it stands is at least its own, an improved node left in place); any other order
// of pops changes it.
TL_TEST(route_searches_over_a_cluttered_floor_pop_in_the_consoles_order) {
    uint32_t seed = 12345u;
    NavGrid nav;
    PassabilityMap grid;
    build_cluttered(nav, grid, seed);
    auto next = [&seed](float span) {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24) * span;
    };
    uint64_t digest = 1469598103934665603ull;
    auto fold = [&digest](uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            digest ^= (value >> (8 * i)) & 0xffu;
            digest *= 1099511628211ull;
        }
    };
    auto bits = [](float value) {
        uint32_t out = 0;
        std::memcpy(&out, &value, sizeof out);
        return out;
    };
    std::vector<Vec3> nodes;
    int found = 0;
    for (int i = 0; i < 3000; ++i) {
        const Vec3 from{next(64.0f) - 32.0f, 0.0f, next(64.0f) - 32.0f};
        const Vec3 to{next(64.0f) - 32.0f, 0.0f, next(64.0f) - 32.0f};
        const bool ignore_objects = (i % 3) == 0;
        const int cap = (i % 2) == 0 ? 150 : 800;
        const bool ok = nav.find_nodes(from, to, grid, ignore_objects, nodes, cap);
        found += ok ? 1 : 0;
        fold(ok ? 1u : 0u);
        fold(static_cast<uint32_t>(nodes.size()));
        for (const Vec3& node : nodes) {
            fold(bits(node.x));
            fold(bits(node.z));
        }
    }
    TL_CHECK_EQ(found, 2985);
    // Taken again on 2026-10-29 when the floor's bake took the console's fused collision tests
    // (`pointIn`, `classifyPoint`, the plane intersections; xbox-oracle.md finding 56): the
    // floor's edge cells changed, not the search.
    TL_CHECK_EQ(digest, 0x3559c258b186b16full);
}
