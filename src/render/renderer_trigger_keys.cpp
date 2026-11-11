// The renderer: the collision keys of a placed trigger's animations.
#include "render/renderer_impl.h"

namespace tl::render {

// Keys 10 and 11 on an item: CItem::updateAnimation(float):
// the placed copy's collidable flag and its share of the map grid, on or off.
void Renderer::Impl::trigger_collision_key(const std::string& name, const SceneUnit& unit,
                                           bool enable) {
    const Ogre::SceneNode* model = unit.entity->getParentSceneNode();
    // The machine whose node map holds this copy's scene node (a prop is no character, so
    // `script_owning` misses it).
    NodeMap* nodes = nullptr;
    for (const MachineView& view : machines()) {
        if (view.nodes == nullptr) continue;
        const auto held = view.nodes->equal_range(unit.node);
        for (auto n = held.first; n != held.second && nodes == nullptr; ++n) {
            if (n->second == model) nodes = view.nodes;
        }
        if (nodes != nullptr) break;
    }
    TriggerProp prop;
    if (model == nullptr || nodes == nullptr || !trigger_prop(name, prop)) return;
    set_trigger_collidable(instance_key(unit.node, *nodes), enable);
    TL_LOG_INFO("unit trigger '%s': %s at its key", name.c_str(),
                enable ? "collidable" : "not collidable");
}

/// A state held as a pose plays nothing, so no clock crosses its keys: the collision keys
/// its animation authors at frame 0 fall once, as the pose is taken (the fortress entry
/// door's `opened` carries `DISABLECOLLISION`, its `closed` `ENABLECOLLISION`).
void Renderer::Impl::pose_collision_keys(const std::string& name, SceneUnit& unit) {
    if (unit.playing.empty() || unit.key_animation == unit.playing) return;
    unit.key_animation = unit.playing;
    unit.key_time = 0.0f;
    for (const tl::core::AnimationTag& tag : manifest_for(unit.mesh).tags_for(unit.playing)) {
        const bool enable = tag.name == "ENABLECOLLISION";
        if (!enable && tag.name != "DISABLECOLLISION") continue;
        if (tag.seconds() <= 0.0f) trigger_collision_key(name, unit, enable);
    }
}

}  // namespace tl::render
