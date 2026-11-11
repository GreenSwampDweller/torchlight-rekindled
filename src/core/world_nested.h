// The nested layouts a level placed, as World keeps them: a machine for a linked layout with a
// script of its own, and a room's linked file that makes no machine but whose objects are stored.
#pragma once
#include "core/script.h"
#include "core/view_handles.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tl::core {

/// One machine per placed nested layout with a script of its own (a wire never crosses
/// a file); each has its own node map, as copies share their layout's node ids.
struct NestedScript {
    std::string reference;
    int64_t owner = 0;  ///< the `Layout Link` node in the parent
    /// The `Layout Link` of the parent machine's own file it was reached through (its
    /// `owner`, or a link above it when the files between own no machine): whose turn of
    /// the parent's activation activates it.
    int64_t turn_link = 0;
    std::string owner_name;
    /// The node map of the file holding that link (`Play` on the link is
    /// `CLayout::start` on its layout).
    const NodeMap* owner_nodes = nullptr;
    /// In the level's coordinates (`LevelScript::set_frame` before it is built); its
    /// characters join the level's list (`set_level_machines`).
    tl::core::LevelScript script;
    NodeMap nodes;

    bool running = false;
    bool retired = false;  ///< an ended skill event's: not stepped, off `level_machines`
    /// Its link's `START ON LOAD` (`CLayout +0x160`): what the parent's activation does to
    /// its layout.
    bool starts_on_load = false;

    /// Where its nodes sit as `CLogicNodeState` records them: the room layout's index in
    /// `CLevel+0x10` (+0x30) and the parent hierarchy hash (+0x40) (`placing_scene`).
    uint32_t scene = 0;
    int64_t hierarchy = 0;

    bool reported = false;  ///< its first action logged (once, not every frame)
    /// Made by the `LEVELPARTICLE` layout, outside the scenes `storeLevelSavedState` walks.
    bool level_particle = false;
    /// Linked through a `Layout Link Particle`: the file is the particle's own layout
    /// (`CParticlePreloader::LoadParticle`), not the scene's, so the console stores none of
    /// its logic states.
    bool particle = false;
    std::vector<uint64_t> link_path;  ///< `room_link_path` when placed: its place in the walk
};

/// A room's linked layout that makes no machine (a prop file of Groups and Property Nodes):
/// its objects, built only to be stored, since `CPlayer::storeLevelSavedState` stores the
/// logic nodes of every layout the level loaded. `before` is how many `nested_scripts` were
/// made before it, which places it in the walk's order among them. Never stepped.
struct StoredLayout {
    size_t before = 0;
    uint32_t scene = 0;
    int64_t hierarchy = 0;
    int64_t owner = 0;
    bool built = false;
    bool level_particle = false;      ///< as `NestedScript::level_particle`
    bool particle = false;            ///< as `NestedScript::particle`
    std::vector<uint64_t> link_path;  ///< as `NestedScript::link_path`
    tl::core::LevelScript script;
};

}  // namespace tl::core
