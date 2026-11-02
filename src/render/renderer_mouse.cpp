// DIVERGENCE (desktop mouse): the PC's pointer over the console's game. The original PC build
// walks to the floor clicked, attacks the monster clicked while the button is held, and walks to
// anything else clicked and uses it (`CGameClient::processIngameInput`). The port's game is the
// console's, run by the pad, so a click is carried out as the pad's own orders: a route the
// character follows, the stick turned towards the target with X held, or A pressed once the
// character stands at the thing clicked - where the console's selector, nearest first, finds it.
// The pad keeps the last word: a stick pushed drops whatever the mouse asked for.
#include "render/renderer_impl.h"

#include "core/gamepad.h"

#include <OgreCamera.h>
#include <OgreEntity.h>
#include <OgreRay.h>
#include <OgreSceneNode.h>

#include <cmath>
#include <limits>

namespace tl::render {

namespace {

/// Close enough to something clicked to press A on it: inside the console's item pass
/// (`kTargetItemRadius`) and its talk reach (2.0, `step_pending_talk`).
constexpr float kMouseInteractReach = 1.8f;
/// A floor click within this of the character is already reached.
constexpr float kMouseArrived = 0.4f;
/// While the button is held the route is found again this often, to follow the pointer.
constexpr float kMouseRepathSeconds = 0.2f;
/// A thing on the floor this near the point under the pointer counts as clicked.
constexpr float kMousePropRadius = 1.2f;

float planar(const Ogre::Vector3& a, const Ogre::Vector3& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

}  // namespace

bool Renderer::Impl::mouse_ground(const tl::core::InputState& in, Ogre::Ray& ray,
                                  Ogre::Vector3& ground) const {
    if (camera == nullptr) return false;
    ray = camera->getCameraToViewportRay(in.pointer_x, in.pointer_y);
    const Ogre::Vector3 start = ray.getOrigin();
    const Ogre::Vector3 end = ray.getPoint(300.0f);
    tl::core::CollisionHit hit;
    if (!collision.empty() &&
        collision.ray_collision(tl::core::Vec3{start.x, start.y, start.z},
                                tl::core::Vec3{end.x, end.y, end.z}, false, hit)) {
        ground = Ogre::Vector3(hit.point.x, hit.point.y, hit.point.z);
        return true;
    }
    const std::pair<bool, Ogre::Real> at =
        ray.intersects(Ogre::Plane(Ogre::Vector3::UNIT_Y, player.position().y));
    if (!at.first) return false;
    ground = ray.getPoint(at.second);
    return true;
}

void Renderer::Impl::steer_by_mouse(tl::core::InputState& in, float delta) {
    const bool press = in.pointer && in.click && !mouse_click_held;
    mouse_click_held = in.pointer && in.click;
    // The pad first: a pushed stick is a new order, as on the console.
    if (!in.pointer || tl::core::kStickThreshold < in.magnitude()) {
        mouse_goal = MouseGoal{};
        return;
    }

    const tl::core::Vec3 me = player.position();
    const Ogre::Vector3 here(me.x, me.y, me.z);
    // The stick pushed all the way towards a point, in the camera's frame: stick_destination's
    // turn run backwards.
    const auto aim_at = [&](const Ogre::Vector3& at) {
        const float dx = at.x - here.x;
        const float dz = at.z - here.z;
        const float length = std::sqrt(dx * dx + dz * dz);
        if (length < 1e-3f) return;
        const float angle = camera_yaw - 1.57079632679489661923f;
        const float sin_a = std::sin(angle);
        const float cos_a = std::cos(angle);
        in.move_x = (dz * cos_a + dx * sin_a) / length;
        in.move_y = (dx * cos_a - dz * sin_a) / length;
    };
    const auto route_to = [&](const Ogre::Vector3& at) {
        if (!nav.ready()) return;
        std::vector<tl::core::Vec3> route = nav.find_path(me, tl::core::Vec3{at.x, at.y, at.z});
        if (!route.empty()) player.follow(std::move(route));
    };

    Ogre::Ray ray;
    Ogre::Vector3 ground;
    const bool over_level = mouse_ground(in, ray, ground);

    // The right button: X held, aimed at the pointer, where the character stands
    // (the PC's attack in place).
    if (in.click_alt) {
        mouse_goal = MouseGoal{};
        if (over_level) {
            player.stop_following();
            aim_at(ground);
            in.attack = true;
        }
        return;
    }

    if (press && over_level) {
        mouse_goal = MouseGoal{};
        // A unit under the pointer: the nearest whose bounds the ray passes through.
        Ogre::Entity* unit = nullptr;
        bool hostile = false;
        bool pet_clicked = false;
        float nearest = std::numeric_limits<float>::max();
        const auto consider = [&](Ogre::Entity* entity, const std::string& name, int64_t node) {
            if (entity == nullptr || !entity->isVisible() || name.empty()) return;
            const bool own_pet = node_is_player_pet(node);
            if (!own_pet && !node_targetable(node, entity->getParentSceneNode())) return;
            const std::pair<bool, Ogre::Real> hit = ray.intersects(entity->getWorldBoundingBox(true));
            if (!hit.first || !(hit.second < nearest)) return;
            nearest = hit.second;
            pet_clicked = own_pet;
            unit = own_pet ? nullptr : entity;
            if (own_pet) return;
            const tl::core::UnitStats* stats = unit_book.find(name);
            hostile = stats != nullptr && unit_hostile(node, *stats);
        };
        for (const auto& entry : scene_units) {
            consider(entry.second.entity, entry.first, entry.second.node);
        }
        for (const auto& owner : spawned) {
            for (const SpawnedUnit& made : owner.second) consider(made.entity, made.unit, made.id);
        }
        if (pet_clicked) {
            // The PC's click on the pet: its bag beside the hero's, the menu opened as I opens
            // it with the focus on the pet's tray.
            if (!inventory_menu.visible()) {
                inventory_menu.toggle();
                play_ui(tl::core::InterfaceSound::MenuOpen);
            }
            inventory_menu.set_menu_mode(MenuMode::Inventory);
            update_menu();
            if (inventory_menu.pet_tray_available() && !inventory_menu.pet_tray_focused()) {
                inventory_menu.swap_bag_focus(0);
                update_menu();
            }
            update_character_preview(0.0f);
            TL_LOG_INFO("mouse: the pet's bag");
            return;
        }
        if (unit != nullptr && unit->getParentSceneNode() != nullptr) {
            mouse_goal.kind = hostile ? MouseGoal::Kind::Attack : MouseGoal::Kind::Interact;
            mouse_goal.entity = unit;
            mouse_goal.at = unit->getParentSceneNode()->_getDerivedPosition();
        } else {
            // Something on the floor near the point: an item, a prop, a portal.
            bool thing = false;
            Ogre::Vector3 thing_at = ground;
            float best = kMousePropRadius;
            const auto near = [&](const Ogre::Vector3& at) {
                const float d = planar(at, ground);
                if (d <= best) {
                    best = d;
                    thing = true;
                    thing_at = at;
                }
            };
            for (const DroppedWorldItem& item : dropped_items) {
                if (item.node != nullptr && !item.flying) near(item.node->getPosition());
            }
            const auto props = [&](const tl::core::LevelScript& owner) {
                for (const tl::core::LevelScript::UnitTriggerView& view : owner.unit_triggers()) {
                    if (view.armed && !non_selectable_prop(view.unit)) {
                        near(Ogre::Vector3(view.at.x, view.at.y, view.at.z));
                    }
                }
            };
            props(script);
            for (const NestedScript& machine : nested_scripts) props(machine.script);
            for (const auto& portal : open_portals) {
                if (portal.enabled) near(to_ogre(portal.at));
            }
            mouse_goal.kind = thing ? MouseGoal::Kind::Interact : MouseGoal::Kind::Walk;
            mouse_goal.at = thing_at;
        }
        TL_LOG_INFO("mouse: %s at (%.1f, %.1f)",
                    mouse_goal.kind == MouseGoal::Kind::Attack     ? "attack"
                    : mouse_goal.kind == MouseGoal::Kind::Interact ? "use"
                                                                   : "walk",
                    static_cast<double>(mouse_goal.at.x), static_cast<double>(mouse_goal.at.z));
        pending_talk.clear();
        clear_talk_target();
        player_item = PlayerItemTarget{};
        mouse_goal.repath = 0.0f;
    } else if (in.click && over_level && mouse_goal.kind == MouseGoal::Kind::Walk) {
        // Held on the floor: the walk follows the pointer.
        mouse_goal.at = ground;
    }

    mouse_goal.repath -= delta;
    const bool repath = mouse_goal.repath <= 0.0f;
    if (repath) mouse_goal.repath = kMouseRepathSeconds;

    switch (mouse_goal.kind) {
        case MouseGoal::Kind::None: break;
        case MouseGoal::Kind::Walk:
            if (planar(here, mouse_goal.at) <= kMouseArrived) {
                mouse_goal = MouseGoal{};
            } else if (repath && (in.click || !player.following())) {
                route_to(mouse_goal.at);
            }
            break;
        case MouseGoal::Kind::Interact:
            if (mouse_goal.entity != nullptr) {
                if (!entity_still_placed(mouse_goal.entity) ||
                    mouse_goal.entity->getParentSceneNode() == nullptr) {
                    mouse_goal = MouseGoal{};
                    break;
                }
                mouse_goal.at = mouse_goal.entity->getParentSceneNode()->_getDerivedPosition();
            }
            if (planar(here, mouse_goal.at) <= kMouseInteractReach) {
                // There: face it and press A, once. The pad's selector picks it, the nearest.
                player.stop_following();
                player.face(std::atan2(mouse_goal.at.x - here.x, mouse_goal.at.z - here.z));
                in.confirm = true;
                mouse_goal = MouseGoal{};
            } else if (repath && !player.following()) {
                route_to(mouse_goal.at);
            }
            break;
        case MouseGoal::Kind::Attack: {
            if (!in.click || !entity_still_placed(mouse_goal.entity) ||
                mouse_goal.entity->getParentSceneNode() == nullptr) {
                mouse_goal = MouseGoal{};
                break;
            }
            mouse_goal.at = mouse_goal.entity->getParentSceneNode()->_getDerivedPosition();
            const float weapon_range =
                player_stats.weapon_attack.present ? player_stats.weapon_attack.attack.range : 0.0f;
            const float reach = std::max(weapon_range, 1.5f) + 0.5f;
            if (planar(here, mouse_goal.at) <= reach) {
                player.stop_following();
                aim_at(mouse_goal.at);
                in.attack = true;
            } else if (repath) {
                route_to(mouse_goal.at);
            }
            break;
        }
    }
}

}  // namespace tl::render
