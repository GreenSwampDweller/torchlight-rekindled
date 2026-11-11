// The level's half of a `Unit Spawner` (core/unit_spawner.h), the World's.
#pragma once

#include "core/unit_spawner.h"
#include "core/world.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tl::core {

/// A `Unit Spawner` as the level sees it (core/unit_spawner.h): the level's half of
/// `CUnitSpawner` for one spawner; its outputs go back to its machine.
class World::SpawnerHost : public tl::core::UnitSpawnerHost {
public:
    SpawnerHost(World& world, tl::core::LevelScript* machine, int64_t node)
        : world_(world), machine_(machine), node_(node) {}
    int32_t level_monster_level() const override;
    tl::core::Vec3 random_open_position(const tl::core::Vec3& at, float radius,
                                        bool avoid_no_spawn) override;
    bool ray_collision(const tl::core::Vec3& from, const tl::core::Vec3& to,
                       tl::core::Vec3& normal) override;
    float range_random(float lo, float hi) override;
    tl::core::RunicRandom& volatile_random() override;
    void create_particles(const std::string& file, const tl::core::Vec3& at,
                          const tl::core::SpawnAxes& axes, uint32_t index) override;
    void fire_missile(const std::string& resource, const tl::core::Vec3& at,
                      const tl::core::SpawnAxes& axes) override;
    tl::core::SpawnClassPick roll_spawn_class(const std::string& name, int32_t level) override;
    tl::core::SpawnedUnitRef create_unit(const tl::core::SpawnRelease& release,
                                         const std::string& name, bool enchant) override;
    bool unit_exists(int64_t id) const override;
    bool character_alive(int64_t id) const override;
    void kill_character(int64_t id) override;
    void hide_character(int64_t id, bool hide) override;
    void run_particles(bool run) override;
    void output(uint32_t event) override { outputs_.push_back(event); }
    void unit_spawned(const tl::core::SpawnedUnitRef& unit) override;
    bool resource_names_spawner(const std::string& resource) override;

    /// Hand the outputs raised so far to the spawner's machine.
    void deliver();

private:
    World& world_;
    tl::core::LevelScript* machine_;
    int64_t node_;
    std::vector<uint32_t> outputs_;
};

}  // namespace tl::core
