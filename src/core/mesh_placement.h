// One mesh a layout places: what the renderer draws and the collision world takes in.
// Split from core/world_state.h.
#pragma once

#include "core/ogre_types.h"

#include <cstdint>
#include <string>

namespace tl::core {

/// One thing a layout asks to be placed, with everything the layout says about it. Scale
/// and brightness are apart from the transform: they come from named per-descriptor fields
/// (tl::formats::cmp_field).
struct MeshPlacement {
    std::string mesh;

    /// A `Unit Trigger`'s prop: its unit's `LAYOUT_STATE_1`, hung on it once
    /// it stands (`Renderer::Impl::set_trigger_state_layout`).
    std::string trigger_resting_layout;

    /// The layout node this came from (its 64-bit id, how wires and timelines address it);
    /// zero for anything not placed from a node.
    int64_t node_id = 0;

    /// For a collision placement: its `CollisionWorld` group, so it can be switched off;
    /// zero (the level) for all but a `Unit Trigger`'s prop.
    int64_t collision_group = 0;
    /// The unit's `BLOCK`, for a unit's own collision model
    /// (`CollisionWorld::set_group_blocks`).
    bool collision_blocks = false;

    ogre::Vector3 position = ogre::Vector3::ZERO;
    ogre::Quaternion orientation = ogre::Quaternion::IDENTITY;
    ogre::Vector3 scale = ogre::Vector3::UNIT_SCALE;

    /// What the layout asked this thing to play: a `.SKELETON` stem beside the mesh (empty
    /// or `bind`: the bind pose). `unit` is a `Monster`'s `MONSTER` name, the join an
    /// `Animation Controller` uses.
    std::string animation;
    std::string unit;
    float animation_rate = 1.0f;
    bool loops = true;

    /// True when `animation` is the default idle rather than named: a mesh without
    /// `Idle.SKELETON` is then no fault.
    bool idle_fallback = false;

    /// Whether this mesh is somebody rather than something: units are skipped by
    /// `ground_under()`, so one placed on a marker is not ground for the next.
    bool is_unit = false;

    /// `VISIBLE:false` at placement: placed and registered, started invisible, so a later
    /// `Show` finds a scene node.
    bool start_hidden = false;

    /// A `Room Piece`: its materials take the light map's `LIGHTPASS` (`CLevel::bake`
    /// into `CLevel +0x290`; `CLevel::setProjectorPass`).
    ///
    bool room_piece = false;
    /// The piece has a model of its own (`CRoomPiece +0x100`); one that is only a collision
    /// mesh is no section of `CLevel::populate`.
    bool has_mesh = true;
    /// A piece under a random Group (`CRoomPiece +0x159`, set by
    /// `CRoomPiece::vfunc_2`): its box is cast again after the baked passes.
    bool recalculate = false;
    /// A room piece's place in the order the level's objects were made (a link's at its node):
    /// the order `bakeCollision` and `populate` walk a room's pieces in.
    uint64_t made = 0;
    /// A piece of a linked layout: `bakeCollision` collides it, `CLevel::populate`
    /// takes no section of it (only the room scene's own `Room Piece`s).
    bool linked = false;
    /// The piece's `PRERENDER` (-1) or `POSTRENDER` (+1) in its tileset table, 0 for neither:
    /// `CLevel::bake` batches them into `stgeompre_` on queue 0x30 and
    /// `stgeompost_` on 0x59, the rest into `stgeom_` on 0x31.
    int render_phase = 0;
    /// A room piece's `BAKE` (`CRoomPieceDescriptor::Set_setIsBakeable`, X360 +0x14f):
    /// whether `CLevel::bake` batches it; true by default (`CRoomPiece::CRoomPiece`).
    bool bake = true;

    /// A layout `Light`'s `LIGHT DENSITY` (`CLight +0x100`, 1 from the constructor),
    /// else 0: drawn into the light map only, that many times over.
    unsigned light_density = 0;
};

}  // namespace tl::core
