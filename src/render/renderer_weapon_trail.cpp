// Weapon trails: the scene's half of CWeaponTrail (core/weapon_trail.h) - the ManualObject,
// the four materials CGame::createMaterials builds and CCharacter's calls.
#include "render/renderer_impl.h"

#include <algorithm>

namespace tl::render {

namespace {

// CWeaponTrail::init and setVisible: queue
// 0x5b, drawn after the scene's own (see map_render_queue).
constexpr unsigned kTrailRenderQueue = 0x5b;

Ogre::uint8 trail_render_queue() {
    const unsigned offset = kTrailRenderQueue - 0x5a;
    return static_cast<Ogre::uint8>(
        std::min<unsigned>(renderer_detail::kParticleGroupBase + offset, 90));
}

}  // namespace

// Makes a material: not const, whatever its members say.
// NOLINTNEXTLINE(readability-make-member-function-const)
void Renderer::Impl::ensure_trail_material(const std::string& name) {
    auto& materials = Ogre::MaterialManager::getSingleton();
    if (name.empty() || materials.resourceExists(name, kGameGroup)) return;

    // CGame::createMaterials: unlit, depth
    // test without write, SBT_ADD, two-sided; the arrow pair also unfogged. Any other name is
    // a key's LAYOUT, the name itself the texture.
    std::string texture = name;
    bool arrow = false;
    bool reflect = false;
    if (name == "WeaponTrail") {
        texture = "media/sharedTextures/weapontrail.dds";
    } else if (name == "WeaponTrailEvil") {
        texture = "media/sharedTextures/weapontrailevil.dds";
    } else if (name == "ArrowTrail") {
        texture = "media/sharedTextures/arrowtrail.dds";
        arrow = true;
    } else if (name == "ArrowTrailReflect") {
        texture = "media/sharedTextures/arrowtrailreflect.dds";
        arrow = true;
        reflect = true;
    }
    const std::string path = resolve_archive_path(texture);
    if (path.empty())
        TL_LOG_WARN("weapon trail: no texture '%s' for '%s'", texture.c_str(), name.c_str());

    Ogre::MaterialPtr material = materials.create(name, kGameGroup);
    material->setLightingEnabled(false);
    Ogre::Pass* pass = material->getTechnique(0)->getPass(0);
    pass->setDepthCheckEnabled(true);
    if (reflect) {
        pass->setSelfIllumination(1.0f, 1.0f, 1.0f);
        pass->setAmbient(Ogre::ColourValue(1.0f, 0.0f, 0.0f, 1.0f));
        pass->setDiffuse(Ogre::ColourValue(1.0f, 0.0f, 0.0f, 1.0f));
    } else {
        pass->setSelfIllumination(0.0f, 0.0f, 0.0f);
    }
    pass->setDepthWriteEnabled(false);
    if (arrow) pass->setFog(true, Ogre::FOG_NONE, Ogre::ColourValue::White, 0.001f, 0.0f, 1.0f);
    material->setSceneBlending(Ogre::SBT_ADD);
    material->setCullingMode(Ogre::CULL_NONE);
    if (!path.empty()) pass->createTextureUnitState(path);
    // OGRE 1.6's fixed function fed an unlit pass its vertex colour, which is
    // how the trail fades; OGRE 14's generated shaders read it only when asked.
    pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
}

// PORT: the player as `TrailCharacter` reads it: GOOD, the hand in use
// (`selectAttack`'s), each hand's item and its NOTRAIL type.
tl::core::TrailCharacter Renderer::Impl::player_trail_character() {
    tl::core::TrailCharacter who;
    // The player is `GOOD` (alignment 1). The weapon in use is the hand
    // `selectAttack` took (`attack_location`); each hand's item is drawn by
    // `weapon_entity` (the right's, or the left's alone) or `worn_left_hand`.
    who.good = true;
    load_unit_types();
    const auto& slots = player_inventory.slots();
    if (has_weapon) {
        who.in_use = attack_location == tl::core::equip_location::kLeftHand
                         ? tl::core::TrailWeaponInUse::Left
                         : tl::core::TrailWeaponInUse::Right;
    }
    const bool right_held = slots[tl::core::equip_location::kRightHand].count > 0;
    for (int location = 0; location < 2; ++location) {
        if (slots[static_cast<size_t>(location)].count <= 0) continue;
        who.hands[location].held = true;
        who.hands[location].no_trail =
            unit_types.isa(unit_types.id_of(slots[static_cast<size_t>(location)].item.unittype),
                           tl::core::unit_type::kNoTrail);
        Ogre::Entity* drawn = location == 0 || !right_held ? weapon_entity : worn_left_hand;
        if (drawn != nullptr && drawn->getParentNode() != nullptr) {
            player_trails.hands[location].node = drawn->getParentNode();
            who.hands[location].node = &player_trails.hands[location];
        }
    }
    return who;
}

tl::core::TrailCharacter Renderer::Impl::unit_trail_character(Ogre::Entity* entity,
                                                              DrawnTrails& drawn, bool good,
                                                              int64_t node) {
    tl::core::TrailCharacter who;
    who.good = good;
    const auto found = unit_held_items.find(entity);
    if (found == unit_held_items.end()) return who;
    load_unit_types();
    const UnitHeldItems& held = found->second;
    for (int slot = 0; slot < 2; ++slot) {
        if (held.unittype[slot].empty()) continue;
        tl::core::TrailHand& hand = who.hands[slot];
        hand.held = true;
        hand.no_trail =
            unit_types.isa(unit_types.id_of(held.unittype[slot]), tl::core::unit_type::kNoTrail);
        if (held.held[slot] != nullptr && held.held[slot]->getParentNode() != nullptr) {
            drawn.hands[slot].node = held.held[slot]->getParentNode();
            hand.node = &drawn.hands[slot];
        }
    }
    // `selectAttack`: the hand the
    // character's machine took (`+0x3ec`), which alternates between two
    // weapons; else slot 0's item when there is one, else slot 1's when it is
    // a weapon.
    tl::core::LevelScript* machine = node != 0 ? script_owning(node) : nullptr;
    if (machine != nullptr && machine->unit_attacks_left(node)) {
        who.in_use = tl::core::TrailWeaponInUse::Left;
    } else if (who.hands[0].held) {
        who.in_use = tl::core::TrailWeaponInUse::Right;
    } else if (who.hands[1].held &&
               unit_types.isa(unit_types.id_of(held.unittype[1]), tl::core::unit_type::kWeapon)) {
        who.in_use = tl::core::TrailWeaponInUse::Left;
    }
    return who;
}

void Renderer::Impl::weapon_trail_key(DrawnTrails& drawn, Ogre::Entity* entity,
                                      const tl::core::AnimationTag& tag,
                                      const tl::core::TrailCharacter& who) {
    if (tag.name == "SHOWWEAPONTRAIL") {
        // The key's bone, when the skeleton has it (`hasBone`, `getBone`).
        tl::core::WeaponTrailNode* bone = nullptr;
        if (!tag.bone.empty() && entity != nullptr && entity->hasSkeleton() &&
            entity->getSkeleton()->hasBone(tag.bone)) {
            OgreTrailNode& slot =
                drawn.bones[tl::core::trail_key_is_left(tag.particle_count) ? 1 : 0];
            slot.node = entity->getSkeleton()->getBone(tag.bone);
            bone = &slot;
        }
        const tl::core::TrailKeyMaterial material = drawn.trails.show_key(tag, who, bone);
        ensure_trail_material(material.material);
        const tl::core::WeaponTrail* trail = tl::core::trail_key_is_left(tag.particle_count)
                                                 ? drawn.trails.left()
                                                 : drawn.trails.right();
        TL_LOG_INFO("weapon trail: %s shows its %s hand's (%s, %s)",
                    entity != nullptr ? entity->getMesh()->getName().c_str() : "?",
                    tl::core::trail_key_is_left(tag.particle_count) ? "left" : "right",
                    trail != nullptr && trail->is_active() ? "on" : "not started",
                    material.material.c_str());
    } else if (tag.name == "HIDEWEAPONTRAIL") {
        drawn.trails.hide_key(tag, who);
    }
}

void Renderer::Impl::step_weapon_trails(float player_dt, float world_dt) {
    // CCharacter::updateAnimation: the hands follow their
    // nodes through the model's node (+0x58); before the frame's keys, as there.
    const auto owner_of = [](Ogre::Entity* entity) {
        tl::core::WeaponTrailOwner owner;
        const Ogre::SceneNode* node = entity != nullptr ? entity->getParentSceneNode() : nullptr;
        if (node == nullptr) return owner;
        const Ogre::Quaternion q = node->_getDerivedOrientation();
        const Ogre::Vector3 p = node->_getDerivedPosition();
        owner.orientation = tl::core::TrailQuaternion{q.w, q.x, q.y, q.z};
        owner.position = tl::core::Vec3{p.x, p.y, p.z};
        return owner;
    };
    if (subject != nullptr && player_control) {
        // INVENTED: selectAttack asked once a frame; the original asks from its attack and
        // range questions.
        player_trails.trails.select_attack(player_trail_character(), true);
        player_trails.trails.update(player_dt, owner_of(subject));
    }
    // A unit that has left the scene takes its trails and hands with it.
    const auto placed = [this](Ogre::Entity* entity) {
        return std::any_of(scene_units.begin(), scene_units.end(),
                           [entity](const auto& e) { return e.second.entity == entity; });
    };
    for (auto it = unit_held_items.begin(); it != unit_held_items.end();) {
        it = placed(it->first) ? std::next(it) : unit_held_items.erase(it);
    }
    for (auto it = unit_trails.begin(); it != unit_trails.end();) {
        if (!placed(it->first)) {
            destroy_character_trails(it->second);
            it = unit_trails.erase(it);
            continue;
        }
        it->second.trails.update(world_dt, owner_of(it->first));
        ++it;
    }
}

void Renderer::Impl::draw_character_trails(DrawnTrails& drawn) {
    if (scene == nullptr) return;
    tl::core::WeaponTrail* trails[3] = {drawn.trails.right(), drawn.trails.left(),
                                        drawn.trails.tracer()};
    for (int i = 0; i < 3; ++i) {
        tl::core::WeaponTrail* trail = trails[i];
        if (trail == nullptr) continue;
        DrawnTrails::Drawn& out = drawn.drawn[i];
        if (out.object == nullptr) {
            // `CWeaponTrail::init`: `2 *
            // MAXSEGMENTS` vertices, a dynamic triangle strip begun on
            // `WeaponTrail`, under a node of its own at the scene's root - the
            // strip is in the world - and hidden.
            ensure_trail_material("WeaponTrail");
            const size_t count = static_cast<size_t>(trail->max_segments()) * 2;
            out.object = scene->createManualObject();
            out.object->setDynamic(true);
            out.object->estimateVertexCount(count);
            out.object->setRenderQueueGroup(trail_render_queue());
            out.object->begin("WeaponTrail", Ogre::RenderOperation::OT_TRIANGLE_STRIP, kGameGroup);
            for (size_t v = 0; v < count; ++v) {
                out.object->position(0.0f, 0.0f, 0.0f);
                out.object->textureCoord(0.0f, 0.0f);
                out.object->colour(Ogre::ColourValue::White);
            }
            out.object->end();
            out.material = "WeaponTrail";
            out.node = scene->getRootSceneNode()->createChildSceneNode();
            out.node->attachObject(out.object);
            out.object->setVisible(false);
        }
        if (trail->material() != out.material) {
            // `setMaterialName`.
            ensure_trail_material(trail->material());
            out.object->setMaterialName(0, trail->material(), kGameGroup);
            out.material = trail->material();
        }
        if (trail->take_rebuilt()) {
            out.object->beginUpdate(0);
            for (const tl::core::WeaponTrailVertex& v : trail->vertices()) {
                out.object->position(v.position.x, v.position.y, v.position.z);
                if (!v.attributed) continue;
                out.object->textureCoord(v.u, v.v);
                out.object->colour(
                    Ogre::ColourValue(v.colour.r, v.colour.g, v.colour.b, v.colour.a));
            }
            out.object->end();
        }
        out.object->setVisible(trail->drawn_visible());
    }
}

void Renderer::Impl::draw_weapon_trails() {
    draw_character_trails(player_trails);
    for (auto& entry : unit_trails) draw_character_trails(entry.second);
}

void Renderer::Impl::destroy_character_trails(DrawnTrails& drawn) {
    // `CWeaponTrail::~CWeaponTrail` through
    // `CWeaponTrail::uninit`: the object off its node, the node and
    // the object destroyed. (`CWeaponTrail::clear` is empty.)
    for (DrawnTrails::Drawn& out : drawn.drawn) {
        if (scene != nullptr && out.object != nullptr) {
            if (out.node != nullptr) out.node->detachAllObjects();
            scene->destroyManualObject(out.object);
        }
        if (scene != nullptr && out.node != nullptr) scene->destroySceneNode(out.node);
        out = DrawnTrails::Drawn{};
    }
}

// PORT: a unit's weapon trails destroyed with it.
void Renderer::Impl::forget_unit_trails(Ogre::Entity* entity) {
    unit_held_items.erase(entity);
    const auto found = unit_trails.find(entity);
    if (found == unit_trails.end()) return;
    destroy_character_trails(found->second);
    unit_trails.erase(found);
}

}  // namespace tl::render
