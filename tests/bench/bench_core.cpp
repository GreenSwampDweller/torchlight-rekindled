// Micro-benchmarks for the engine's hot paths, on a seeded synthetic level (a 96 x 96 floor
// of 4-unit tiles, a grid of rooms with doorways). Compare ratios between runs of one machine.
//
//   tl_bench [filter] [--quick]
#include "core/collision.h"
#include "core/navigation.h"
#include "core/passability.h"
#include "core/random.h"
#include "core/script.h"
#include "core/stats.h"
#include "core/units.h"
#include "formats/cmp.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

using tl::core::CollisionHit;
using tl::core::CollisionWorld;
using tl::core::NavGrid;
using tl::core::PassabilityMap;
using tl::core::RunicRandom;
using tl::core::Vec3;

constexpr float kExtent = 96.0f;
constexpr float kTile = 4.0f;
constexpr float kRoom = 16.0f;

void add_quad(CollisionWorld& world, const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
              uint32_t material = 0) {
    world.add_triangle(a, b, c, material);
    world.add_triangle(a, c, d, material);
}

/// A two-faced wall along x = `x` or z = `z`, from `lo` to `hi`, of the
/// `nocollide` material levels author walls in: the only kind that closes
/// passability cells.
void add_wall(CollisionWorld& world, bool along_z, float at, float lo, float hi) {
    const float h = 4.0f;
    const uint32_t m = tl::core::kNoCollideMaterial;
    if (along_z) {
        add_quad(world, {at, 0, lo}, {at, 0, hi}, {at, h, hi}, {at, h, lo}, m);
        add_quad(world, {at, 0, lo}, {at, h, lo}, {at, h, hi}, {at, 0, hi}, m);
    } else {
        add_quad(world, {lo, 0, at}, {lo, h, at}, {hi, h, at}, {hi, 0, at}, m);
        add_quad(world, {lo, 0, at}, {hi, 0, at}, {hi, h, at}, {lo, h, at}, m);
    }
}

/// Walls every `kRoom` units both ways, each segment with a 4-unit doorway.
/// 1,152 floor faces and 480 wall faces.
void add_level(CollisionWorld& world) {
    for (float x = 0.0f; x < kExtent; x += kTile) {
        for (float z = 0.0f; z < kExtent; z += kTile) {
            add_quad(world, {x, 0, z}, {x, 0, z + kTile}, {x + kTile, 0, z + kTile},
                     {x + kTile, 0, z});
        }
    }
    for (float at = kRoom; at < kExtent; at += kRoom) {
        for (float lo = 0.0f; lo < kExtent; lo += kRoom) {
            const float door = lo + 6.0f;
            add_wall(world, true, at, lo, door);
            add_wall(world, true, at, door + 4.0f, lo + kRoom);
            add_wall(world, false, at, lo, door);
            add_wall(world, false, at, door + 4.0f, lo + kRoom);
        }
    }
}

struct Level {
    CollisionWorld world;
    PassabilityMap grid;
    NavGrid nav;
    Level() {
        add_level(world);
        world.build();
        grid.bake(world, nullptr);
        nav.build(grid);
    }
};

Level& level() {
    static Level l;
    return l;
}

Vec3 random_point(RunicRandom& rng) {
    return {rng.between(1.0f, kExtent - 1.0f), 0.0f, rng.between(1.0f, kExtent - 1.0f)};
}

struct Bench {
    const char* name;
    /// Operations per rep, for the per-op figure.
    size_t ops;
    /// Returns how much work was done (hits, route nodes, events): the same
    /// every run, so a change that makes a bench fast by making it do nothing
    /// shows.
    std::function<double()> run;
};

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

}  // namespace

int main(int argc, char** argv) {
    std::string filter;
    bool quick = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick") == 0) {
            quick = true;
        } else {
            filter = argv[i];
        }
    }
    const int reps = quick ? 1 : 7;

    std::vector<Bench> benches;

    benches.push_back({"level: collision build", 1, []() -> double {
                           CollisionWorld w;
                           add_level(w);
                           w.build();
                           return static_cast<double>(w.triangle_count());
                       }});
    benches.push_back({"level: passability bake", 1, []() -> double {
                           PassabilityMap g;
                           g.bake(level().world, nullptr);
                           return g.width();
                       }});
    benches.push_back({"level: nav build", 1, []() -> double {
                           NavGrid n;
                           n.build(level().grid);
                           return static_cast<double>(n.walkable_count());
                       }});

    constexpr size_t kRays = 20000;
    benches.push_back({"collision: ground_under", kRays, []() -> double {
                           RunicRandom rng(1);
                           size_t hits = 0;
                           for (size_t i = 0; i < kRays; ++i) {
                               const Vec3 p = random_point(rng);
                               float y = 0.0f;
                               hits += level().world.ground_under(p.x, p.z, 10.0f, y) ? 1 : 0;
                           }
                           return static_cast<double>(hits);
                       }});
    benches.push_back({"collision: horizontal ray", kRays, []() -> double {
                           RunicRandom rng(2);
                           size_t hits = 0;
                           for (size_t i = 0; i < kRays; ++i) {
                               Vec3 a = random_point(rng), b = random_point(rng);
                               a.y = b.y = 1.0f;
                               CollisionHit hit;
                               hits += level().world.ray_collision(a, b, true, hit) ? 1 : 0;
                           }
                           return static_cast<double>(hits);
                       }});
    benches.push_back(
        {"collision: sphere sweep 1u", kRays, []() -> double {
             RunicRandom rng(3);
             size_t hits = 0;
             for (size_t i = 0; i < kRays; ++i) {
                 Vec3 a = random_point(rng);
                 a.y = 0.6f;  // clear of the floor: only walls stop it
                 const Vec3 b{a.x + rng.between(-1.0f, 1.0f), 0.6f, a.z + rng.between(-1.0f, 1.0f)};
                 level().world.sort_for_collision({a.x - 2, -1, a.z - 2}, {a.x + 2, 3, a.z + 2});
                 CollisionHit hit;
                 hits += level().world.pre_sorted_sphere_collision(a, b, 0.5f, true, hit) ? 1 : 0;
             }
             return static_cast<double>(hits);
         }});

    constexpr size_t kPaths = 1000;
    benches.push_back({"nav: find_path across rooms", kPaths, []() -> double {
                           RunicRandom rng(4);
                           size_t nodes = 0;
                           for (size_t i = 0; i < kPaths; ++i) {
                               // Drawn in order: arguments are evaluated in an
                               // unspecified one, which x86 and AArch64 GCC differ on.
                               const Vec3 from = random_point(rng);
                               const Vec3 to = random_point(rng);
                               nodes += level().nav.find_path(from, to, level().grid).size();
                           }
                           return static_cast<double>(nodes);
                       }});
    benches.push_back({"nav: find_nodes (character)", kPaths, []() -> double {
                           RunicRandom rng(5);
                           size_t nodes = 0;
                           std::vector<Vec3> route;
                           for (size_t i = 0; i < kPaths; ++i) {
                               route.clear();
                               const Vec3 from = random_point(rng);
                               const Vec3 to = random_point(rng);
                               level().nav.find_nodes(from, to, level().grid, false, route);
                               nodes += route.size();
                           }
                           return static_cast<double>(nodes);
                       }});

    constexpr size_t kDraws = 1000000;
    benches.push_back({"random: between", kDraws, []() -> double {
                           RunicRandom rng(6);
                           double sum = 0.0;
                           for (size_t i = 0; i < kDraws; ++i) sum += rng.between(0.0f, 1.0f);
                           return sum;
                       }});
    benches.push_back({"stats: curved graph at()", kDraws, []() -> double {
                           static const tl::core::StatGraph graph = [] {
                               tl::core::StatGraph g(tl::core::GraphType::Curved, "BENCH");
                               for (int i = 0; i <= 100; i += 5) {
                                   g.add_point(static_cast<float>(i),
                                               static_cast<float>(i * i) * 0.5f);
                               }
                               g.process_points();
                               return g;
                           }();
                           double sum = 0.0;
                           for (size_t i = 0; i < kDraws; ++i) {
                               sum += graph.at(static_cast<float>(i % 1000) * 0.1f);
                           }
                           return sum;
                       }});

    constexpr size_t kFrames = 600;
    benches.push_back({"script: 100 hunters, 10 s at 60 fps", kFrames, []() -> double {
                           tl::core::UnitBook units;
                           tl::core::UnitStats grunt;
                           units.add("Grunt", grunt);
                           tl::core::LevelScript script;
                           script.set_units(&units);
                           script.set_navigation(&level().nav);
                           // Characters write their avoidance circles into the
                           // grid; each rep gets the baked one fresh.
                           PassabilityMap grid = level().grid;
                           script.set_passability(&grid);
                           script.set_collision(&level().world);
                           script.set_unit_active_range(1000.0f);
                           tl::formats::CmpScene scene;
                           script.build(scene, {});
                           RunicRandom rng(7);
                           for (int64_t id = 1; id <= 100; ++id) {
                               script.add_spawned_unit(id, "Grunt", random_point(rng),
                                                       /*hunts=*/true);
                           }
                           tl::core::QuestState quests;
                           std::vector<tl::core::ScriptEvent> events;
                           const Vec3 player{kExtent / 2 + 2, 0.0f, kExtent / 2 + 2};
                           size_t raised = 0;
                           for (size_t f = 0; f < kFrames; ++f) {
                               events.clear();
                               script.update(1.0f / 60.0f, player, quests, events);
                               raised += events.size();
                           }
                           return static_cast<double>(raised);
                       }});

    level();  // built once, outside every timing
    std::printf("%-38s %12s %14s %14s\n", "benchmark", "median ms", "per op", "work");
    int ran = 0;
    for (const Bench& b : benches) {
        if (!filter.empty() && std::string(b.name).find(filter) == std::string::npos) continue;
        ++ran;
        std::vector<double> ms;
        double work = 0.0;
        for (int r = 0; r < reps; ++r) {
            tl::core::LevelScript::reset_near_list_timer();
            const auto start = std::chrono::steady_clock::now();
            work = b.run();
            const auto end = std::chrono::steady_clock::now();
            ms.push_back(std::chrono::duration<double, std::milli>(end - start).count());
        }
        const double m = median(ms);
        const double per_op_ns = m * 1e6 / static_cast<double>(b.ops);
        char per_op[32];
        if (per_op_ns >= 1e6) {
            std::snprintf(per_op, sizeof(per_op), "%.2f ms", per_op_ns / 1e6);
        } else if (per_op_ns >= 1e3) {
            std::snprintf(per_op, sizeof(per_op), "%.2f us", per_op_ns / 1e3);
        } else {
            std::snprintf(per_op, sizeof(per_op), "%.1f ns", per_op_ns);
        }
        std::printf("%-38s %12.2f %14s %14.0f\n", b.name, m, per_op, work);
    }
    return ran == 0 ? 1 : 0;
}
