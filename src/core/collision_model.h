// A level's collision mesh as `World::add_collision_mesh` folds it in.
#pragma once

#include "core/collision.h"
#include "core/ogre_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tl::core {

/// A collision mesh as add_collision_mesh() folds it in, in its own space, read once per
/// level; each placement transforms its points with the same `Matrix4 * Vector3`.
/// Defaults: `CCollisionModel::CCollisionModel`.
struct LevelCollisionModel {
    enum class State { Loaded, Threw, Missing } state = State::Missing;
    /// The load's exception, repeated for every placement as it was before.
    std::string error;
    struct Submesh {
        /// `tl::core::collision_material` of the submesh's name and index.
        uint32_t material = 0;
        std::vector<ogre::Vector3> points;
        /// Three per triangle, each already checked against `points`.
        std::vector<uint32_t> corners;
    };
    std::vector<Submesh> submeshes;
    bool has_bounds = false;
    Vec3 low, high;
};

}  // namespace tl::core
