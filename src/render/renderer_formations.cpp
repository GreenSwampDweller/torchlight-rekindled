// A formation woken as the player comes near: `CFormationNode::updateLevelObject`.
// Split from renderer_world.cpp.
#include "render/renderer_impl.h"

namespace tl::render {
namespace {

constexpr float kFormationWakeDistance = 25.0f;  // `distance < 25.0`

}  // namespace

// `CFormationNode::updateLevelObject`.
void Renderer::Impl::wake_formations() {
    if (pending_formations.empty() || subject_node == nullptr) return;
    const Ogre::Vector3 player_at = subject_node->getPosition();
    for (PendingFormation& formation : pending_formations) {
        if (formation.woken) continue;
        // Horizontal, and the direction to the player becomes the formation's
        // forward: its right is UNIT_Y x forward, its up
        // UNIT_Y - a turn about the vertical.
        float fx = player_at.x - formation.at.x;
        float fz = player_at.z - formation.at.z;
        const float d = std::sqrt(fx * fx + fz * fz);
        if (!(d < kFormationWakeDistance)) continue;
        formation.woken = true;
        if (1e-08f < d) {
            fx /= d;
            fz /= d;
        }
        const Ogre::Quaternion turn(Ogre::Radian(std::atan2(fx, fz)), Ogre::Vector3::UNIT_Y);

        if (formation.spawners) {
            // updateLevelObject: the layout placed, shown,
            // activated, its Unit Spawners spawn. Released units do not join the formation:
            // only CCharacter entries
            // reach CAIManager::addFormationUnit.
            // DIVERGENCE: no hidden load, so the layout loads here as it wakes.
            // DIVERGENCE: the spawn() is never made. Its test
            // reads SPAWN ON CREATE past the descriptor returned by
            // GetObjectsCreatedByADescriptor, heap garbage;
            // read as meant it never spawns again.
            const int64_t first_new = kSpawnedNodeBase + static_cast<int64_t>(spawn_serial);
            place_layout(formation.path, to_ogre(formation.at), turn, /*depth=*/1);
            std::vector<int64_t> formation_ids;
            for (const auto& owner : spawned) {
                for (const SpawnedUnit& made : owner.second) {
                    if (made.id > first_new) formation_ids.push_back(made.id);
                }
            }
            const size_t armed = script.arm_hostiles();
            TL_LOG_INFO(
                "formation %s wakes %.1f from the player: its spawners release %zu "
                "unit(s), %zu hostile",
                formation.path.c_str(), static_cast<double>(d), formation_ids.size(), armed);
            continue;
        }

        const size_t woke = wake_formation_monsters(formation, turn);
        std::vector<int64_t> formation_ids;
        formation_ids.reserve(woke);
        for (size_t i = spawned[-2007].size() - woke; i < spawned[-2007].size(); ++i) {
            formation_ids.push_back(spawned[-2007][i].id);
        }
        script.link_formation(formation_ids);
        const size_t armed = script.arm_hostiles();
        TL_LOG_INFO("formation %s wakes %.1f from the player: %zu unit(s), %zu hostile",
                    formation.path.c_str(), static_cast<double>(d), woke, armed);
    }
}

// `CFormationNode::updateLevelObject` of a formation without
// spawners: each `Monster` node's unit placed on the ground, turned with the formation.
size_t Renderer::Impl::wake_formation_monsters(const PendingFormation& formation,
                                               const Ogre::Quaternion& turn) {
    static const tl::formats::CmpScene kNoScene;
    const tl::formats::CmpScene* scene = layout_file(formation.path);
    if (scene == nullptr) scene = &kNoScene;
    namespace monster = tl::formats::cmp_field::monster;
    size_t woke = 0;
    size_t made = 0;
    for (const tl::formats::CmpNode& node : scene->nodes) {
        if (node.type != tl::formats::cmp_type::kMonster) continue;
        // The unit `setLayoutFile` made for this node (`add_pending_formation`).
        const tl::core::LevelScript::UnitCreation* creation =
            made < formation.made.size() ? &formation.made[made] : nullptr;
        ++made;
        const tl::formats::CmpField* who = node.field(monster::kMonster);
        if (who == nullptr) continue;
        const std::string unit = who->as_text();
        Ogre::Vector3 local = Ogre::Vector3::ZERO;
        if (const tl::formats::CmpField* p = node.field(monster::kPosition)) {
            if (p->count() == 3) local = {p->as_float(0), p->as_float(1), p->as_float(2)};
        }
        Ogre::Vector3 at = to_ogre(formation.at) + turn * local;
        float surface = 0.0f;
        if (collision.ground_under(at.x, at.z, at.y + 8.0f, surface)) at.y = surface;
        const Ogre::Quaternion facing = turn * node_orientation(node);
        if (World::spawn_unit(unit, from_ogre(at), -2007, formation.monster_level,
                              /*champion=*/false, /*spawn_anim=*/false, from_ogre(facing),
                              creation != nullptr ? creation->scale : 0.0f) == nullptr) {
            continue;
        }
        const Ogre::Vector3 forward = facing * Ogre::Vector3::UNIT_Z;
        script.add_spawned_unit(spawned[-2007].back().id, unit, tl::core::Vec3{at.x, at.y, at.z},
                                /*hunts=*/false, std::atan2(forward.x, forward.z), creation);
        apply_spawn_level(spawned[-2007].back().id);
        ++woke;
    }
    return woke;
}

}  // namespace tl::render
