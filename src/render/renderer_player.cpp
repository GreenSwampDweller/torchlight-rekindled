// The renderer's implementation: camera follow, stepping and driving the player, script events.
// Split from renderer.cpp; the shared
// declarations are in renderer_impl.h.
#include "core/section_profile.h"
#include "render/renderer_impl.h"
#include "render/unit_opacity.h"

#include <fstream>

namespace tl::render {
bool Renderer::dump_loading_screen(const std::string& png_path) {
    Impl& m = *impl_;
    // The tip is chosen at random, once per process, exactly as the original
    // does - so a run whose whole purpose is to be diffed against another has
    // to pin it first. See LoadingScreen::choose_tip().
    LoadingScreen::pin_tips();
    if (!m.loading.show(kGameGroup, m.config.width, m.config.height)) {
        TL_LOG_WARN("no loading screen to dump");
        return false;
    }
    keep_loading_screen = true;
    const bool ok = dump_frame(png_path);
    keep_loading_screen = false;
    m.loading.hide();
    return ok;
}

/// Point the follow camera, and remember the framing it starts from.
/// INVENTED: the base distance and pitch are not in the data; this keeps the layout's own
/// `Camera`/`Camera Target` framing (aim_camera()), moving with the character, never
/// turning, the distance scaled by CAMERAMULT.

void Renderer::Impl::follow_subject() {
    if (!camera_node) return;

    // A `Camera Controller` outranks the follow camera while it holds the shot: it sits
    // where authored and watches what it was told to follow.
    if (scripted_camera.active) {
        // CCameraController::update writes the look-at
        // point the game camera frames, pan and ease included (core/script.cpp).
        const Ogre::Vector3 focus = scripted_camera.have_follow_position
                                        ? scripted_camera.follow_position
                                        : scripted_camera.position;
        // CGameClient::updateIngame hands
        // updateGameCamera the controller's point (+0x38e0) in state 0, and the quest
        // window's unit in state 2 while it is up.
        if (!layout_has_camera &&
            ((script_dialog_window && quest_dialog.visible()) || game_camera.was_zoomed_in)) {
            update_game_camera(focus);
            return;
        }
        game_camera.focus = focus;
        game_camera.have_focus = true;
        camera_node->setPosition(focus + camera_offset);
        camera_node->lookAt(focus + camera_look_offset, Ogre::Node::TS_WORLD);
        if (sky_node) sky_node->setPosition(camera_node->getPosition());
        aim_light_map(focus);
        return;
    }

    if (!camera_follows || !subject_node) return;

    // CGameClient::updateIngame hands the camera
    // getPosition(false) (+0x70): the collision sphere's centre, one COLLISION_RADIUS above
    // the feet. The port's node stands at the feet.
    Ogre::Vector3 at = subject_node->getPosition();
    if (player_control) at.y += player.body().radius;
    if (!layout_has_camera) {
        update_game_camera(at);
        return;
    }
    camera_node->setPosition(at + camera_offset);
    aim_light_map(at);

    // And aim it: a translated camera keeps its old orientation. The look point travels
    // with the subject.
    camera_node->lookAt(at + camera_look_offset, Ogre::Node::TS_WORLD);

    // The sky travels with the viewer; it is a dome around the camera, not a
    // thing at a place. Without this it slides off as the character walks.
    if (sky_node) sky_node->setPosition(camera_node->getPosition());
}

// CGameClient::updateIngame picks state and target;
// CCameraControl::updateGameCamera moves the camera.
// States 0 (following) and 2 (close-up) with the shake walk; no state 1, 3 or dolly input.
// State 2: the quest window (AI 0x1f), a merchant (0x14), the enchanter (0x1c), fishing
// (0x20..0x22). Focus eases in by 0.001^dt, dolly 14, offset by 0.0001^dt; out by
// (0.0001^dt)^2 until within 0.01.
// DIVERGENCE: with a layout's own Camera Target the camera looks at camera_look_offset
// from its focus; the original looks at the focus itself.
void Renderer::Impl::update_game_camera(const Ogre::Vector3& player_at) {
    const float dt = camera_delta;
    Ogre::Vector3 target = player_at;
    bool close_up = false;
    if (!talk_unit.empty() && quest_dialog.visible()) {
        Ogre::Vector3 speaker;
        std::string found;
        if (unit_position(talk_unit, speaker, found)) {
            target = speaker + talk_camera_mod;
            close_up = true;
        }
    } else if (script_dialog_window && quest_dialog.visible()) {
        // A script's line is the quest window too:
        // state 2 at the player's target (+0x340) plus getQuestDialogCameraOffset, or at
        // the player. Under a Camera Controller (+0x390c) the aim is getQuestDialogNPC;
        // INVENTED: the port keeps the controller's focus for a prop there.
        close_up = true;
        Ogre::Vector3 speaker;
        if (script_talk_node != 0 && unit_node_position(script_talk_node, speaker)) {
            target = speaker + script_talk_camera_mod;
        }
    }
    const VendorScreen vendor =
        inventory_menu.visible() ? inventory_menu.vendor() : VendorScreen::None;
    if (vendor == VendorScreen::Merchant || vendor == VendorScreen::Enchant ||
        vendor == VendorScreen::DestroyGems || vendor == VendorScreen::RecoverGems ||
        vendor == VendorScreen::Retire || vendor == VendorScreen::EnchantShrine) {
        close_up = true;
    }
    // Fishing, AI states 0x20 and 0x21, and 0x22 when no tip menu
    // is up (the port has none): state 2 on the player.
    if (fishing_state != tl::core::FishingState::Off) close_up = true;

    GameCamera& cam = game_camera;
    if (close_up != cam.close_up) {
        cam.close_up = close_up;
        TL_LOG_INFO("camera: %s (focus (%.2f, %.2f, %.2f))", close_up ? "close-up" : "following",
                    static_cast<double>(target.x), static_cast<double>(target.y),
                    static_cast<double>(target.z));
    }
    if (!cam.have_focus) {
        cam.focus = target;
        cam.have_focus = true;
    }
    Ogre::Vector3 focus = target;
    if (close_up) {
        const float k = std::pow(0.001f, dt);
        focus = (cam.focus - target) * k + target;
        cam.was_zoomed_in = true;
    } else if (cam.was_zoomed_in) {
        const float k = std::pow(0.0001f, dt);
        const Ogre::Vector3 d = (cam.focus - target) * k * k;
        focus = target + d;
        cam.was_zoomed_in = 0.01f <= d.length();
    }
    cam.focus = focus;

    // The dolly: `+0x38`, which the right stick moves (`zoom_dolly()`)
    // between 14 and `28.5 * CAMERAMULT` and which starts at
    // the far end, or 14 in state 2; and the pitch that follows from it.
    const float full = kDefaultCameraDistance * camera_mult;
    if (!(camera_dolly > 0.0f)) camera_dolly = full;
    // Not in cinematic mode (`CCameraControl::processInput`, `+0x98`).
    static constexpr float kCameraSpeeds[] = {0.65f, 1.0f, 1.5f};
    static constexpr float kCameraTilts[] = {0.20f, 0.35f, 0.55f};
    static constexpr float kPinchSpeeds[] = {0.7f, 1.0f, 1.4f};
    const tl::core::GameSettings& camera_options = game_settings();
    cam.touch.step(dt, camera_cinematic.on, camera_yaw, camera_dolly, camera_mult,
                   kCameraSpeeds[std::clamp(camera_options.camera_sensitivity, 0, 2)],
                   kCameraTilts[std::clamp(camera_options.camera_pitch_range, 0, 2)],
                   kPinchSpeeds[std::clamp(camera_options.camera_zoom_sensitivity, 0, 2)],
                   camera_options.camera_invert_pitch);
    if (camera_zoom_input != 0.0f && dt > 0.0f && !camera_cinematic.on) {
        const float stick = std::max(-1.0f, std::min(1.0f, camera_zoom_input));
        camera_dolly = tl::core::zoom_dolly(
            camera_dolly, static_cast<std::int16_t>(std::lround(stick * tl::core::kStickMax)),
            camera_mult, dt);
    }
    const float dolly = close_up ? 14.0f : camera_dolly;
    const float pitch = ((1.0f - (dolly - 14.0f) / (full - 14.0f)) * -0.5f + 1.0f) * 0.75f;
    Ogre::Vector3 offset =
        cam.touch.turned(Ogre::Vector3(0.5f * dolly, pitch * dolly, -0.5f * dolly), dolly);
    if (cam.offset.length() == 0.0f) cam.offset = offset;
    offset = (cam.offset - offset) * std::pow(0.0001f, dt) + offset;
    cam.offset = offset;

    const Ogre::Vector3 eye_unclamped = focus + offset;
    const float nearest = camera_mult * 14.0f;
    const float distance = offset.length();
    if (distance < nearest) focus.y += 1.0f - distance / nearest;
    Ogre::Vector3 eye = eye_unclamped;
    if (eye.y < focus.y + 0.1f) eye.y = focus.y + 0.1f;

    // The shake walk, aged against the focus (+0x4c): aimed from the
    // unshaken eye, then translated by the sum, not re-aimed.
    const tl::core::Vec3f shake =
        camera_shakes.update(dt, tl::core::Vec3f{cam.focus.x, cam.focus.y, cam.focus.z}, false);

    camera_node->setPosition(eye);
    camera_node->lookAt(focus + camera_look_offset, Ogre::Node::TS_WORLD);
    camera_node->setPosition(eye + Ogre::Vector3(shake.x, shake.y, shake.z));
    if (sky_node) sky_node->setPosition(camera_node->getPosition());
    // `ProjectorCam` above the focus with the near-clamp's lift and without
    // the shake (places it from the same
    // point it aims at).
    aim_light_map(focus);
}

/// The player's shadow flag: CCharacter::calculateActiveRange
/// `+0x530` (others: LevelScript::calculate_active_range()), handed each frame
/// to its model and what it wears.
/// DIVERGENCE: a fallen player stays in AI state 5, so it casts until it rises.
void Renderer::Impl::update_player_shadow(const Ogre::Vector3& focus) {
    if (subject_node == nullptr) return;
    const Ogre::Vector3 at = subject_node->getPosition();
    tl::core::ActiveRangeInput in;
    in.distance =
        std::sqrt((focus.x - at.x) * (focus.x - at.x) + 0.0f + (focus.z - at.z) * (focus.z - at.z));
    in.forced = true;
    in.unit_active_range = level_unit_active_range;
    in.unit_near_range = level_unit_near_range;
    in.unit_shadows = player_stats.shadows;
    in.shadows_enabled = (config.no_shadows || config.no_torchlight) ? 0 : 1;
    in.unit_shadow_range = level_unit_shadow_range;
    in.height_difference = focus.y - at.y;
    in.ai_state = player_death.dying ? tl::core::ai_state::kDying : tl::core::ai_state::kNone;
    const tl::core::ActiveRange r = tl::core::calculate_active_range(in);
    tl::render::set_node_casts_shadows(subject_node, r.casts_shadow);
}

/// CCharacter::update and CItem::update:
/// each unit's light by its distance from the
/// camera's focus, through CGenericModel::setLightOverride: at least a
/// quarter, in sixteenths; the outline shows only while light^4 > 0.4. Only with the
/// global flags word bit 1 and UNIT_LIGHT_FADE; 1 in the menu.
void Renderer::Impl::update_unit_lights(const Ogre::Vector3& focus) {
    tl::core::LightFadeInput in;
    in.focus_x = focus.x;
    in.focus_y = focus.y;
    in.focus_z = focus.z;
    in.unit_light_fade = level_light_fade;
    in.fade_distance = level_unit_fade_distance;
    in.fade_start = level_unit_fade_start;
    in.in_play = renderer_detail::g_view_renderables.behind_in_play;
    // The unit's stored position is its collision sphere's centre, a radius
    // above the feet its node stands on (follow_subject()).
    const auto light_at = [&](Ogre::SceneNode* node, float radius) {
        const Ogre::Vector3 at = node->_getDerivedPosition();
        in.unit_x = at.x;
        in.unit_y = at.y + radius;
        in.unit_z = at.z;
        const tl::core::LightOverride o = tl::core::light_override(tl::core::character_light(in));
        tl::render::set_node_light(node, o.scale, o.outline_hidden);
    };
    if (subject_node != nullptr && player_control) light_at(subject_node, player.body().radius);
    for (auto& [name, su] : scene_units) {
        if (su.entity == nullptr || su.entity->getParentSceneNode() == nullptr) continue;
        auto radius = unit_radius_cache.find(name);
        if (radius == unit_radius_cache.end()) {
            radius = unit_radius_cache.emplace(name, stats_for_unit(name).radius).first;
        }
        // The pet among them: spawn_unit() made its model.
        light_at(su.entity->getParentSceneNode(), radius->second);
    }
    for (DroppedWorldItem& item : dropped_items) {
        if (item.node == nullptr) continue;
        const Ogre::Vector3 at = item.node->_getDerivedPosition();
        const tl::core::LightOverride o =
            tl::core::light_override(tl::core::item_light(at.x, at.z, focus.x, focus.z));
        tl::render::set_node_light(item.node, o.scale, o.outline_hidden);
    }
}

void Renderer::Impl::step_player(float delta, const tl::core::InputState& stick) {
    if (!subject_node) return;

    // CCharacter::update: the active skill's cast stops
    // the path and setDestination takes no new one until the animation lets go.
    tl::core::InputState in = stick;
    if (player_motion_held()) {
        in.move_x = 0.0f;
        in.move_y = 0.0f;
        player.stop_following();
    }
    // X360 `CCharacter::update`: under the hold position flag (the
    // attack button down) the path stops too, unless a skill that
    // `MOVES_TO_TARGET` is being performed.
    if (attack_button.hold && player.following()) {
        const tl::core::KnownSkill* known = player_skills.find_by_name(player_active_skill.skill);
        const bool moves = known != nullptr && known->def != nullptr && known->def->moves_to_target;
        if (!(moves && player_performing_skill())) player.stop_following();
    }

    // `CCharacter::walkingSpeed` and
    // `runningSpeed`: the unit's two speeds
    // with the character's `PERCENT SPEED` on them, asked for every time the
    // motion reads them.
    {
        tl::core::MovementStats moving = player_stats.movement;
        const tl::core::CharacterEffects total = player_total();
        moving.walking_speed = tl::core::character_speed(moving.walking_speed, total);
        moving.running_speed = tl::core::character_speed(moving.running_speed, total);
        player.set_stats(moving);
    }

    // CGameClient::updateIngame: paused, the player's
    // update runs only beside a conversation.
    if (!game_paused() || conversation_open()) {
        // CCharacter::update's turn, then updatePathing and updateMotion. Standing, the
        // turn is Player::update's, towards the stick; the console's branch
        // (towards +0x1c0, reset by the stick walk) is not carried.
        step_player_skill_turn(delta);
        player.update(delta, in, camera_yaw, collision, &passability, &nav);
    }

    // No push out of other units: the original's player walks through a unit that
    // has no collision model, and routes around the rest (`CBaseUnit::
    // addToAvoidanceMap`).

    const tl::core::Vec3 at = player.position();

    subject_node->setPosition(at.x, at.y, at.z);

    // The character models face +Z at the bind pose - the same convention
    // stand_subject_at() relies on to turn the menu's Alchemist towards the
    // camera - so the facing yaw applies directly about +Y.
    subject_node->setOrientation(
        Ogre::Quaternion(Ogre::Radian(player.facing()), Ogre::Vector3::UNIT_Y));

    camera_delta = delta;
    follow_subject();
    camera_delta = 0.0f;
}

// The console's X: the in-game dispatch's share of the button,
// its handler and the attack in place,
// decided by `core::attack_button_step` and applied here.
void Renderer::Impl::step_attack_button(float delta, bool down, bool pressed, float move_x,
                                        float move_y) {
    attack_button_stepped = true;
    tl::core::AttackButtonInput input;
    // A disabled player does not answer the pad (as the stick, `drive_player`):
    // the button reads as up, and the hold lets go.
    input.down = down && !script_player_disabled;
    input.pressed = pressed && !script_player_disabled;
    // a player alive and enabled. A conversation's
    // window holds the level as the console's modal menus do.
    input.can_act = player_control && subject_node != nullptr && !player_death.dying &&
                    !player_health.dead && !script_dialog_open();
    input.paused = game_paused();
    // not in AI state 0x22, the fishing catch, nor 0x14, which the
    // port's player never enters.
    input.state_allows = fishing_state != tl::core::FishingState::Catch;
    input.stick_pushed = tl::core::kStickThreshold < std::sqrt(move_x * move_x + move_y * move_y);
    input.skill_holds = tl::core::skill_holds_character(player_active_skill);
    const tl::core::AttackButtonFrame frame =
        tl::core::attack_button_step(attack_button, input, delta);

    const tl::core::Vec3 me = player.position();
    const float forward_x = std::sin(player.facing());
    const float forward_z = std::cos(player.facing());
    if (frame.aim) {
        // with the fallback ahead (`getForward`), then
        // `setTargetDirection`.
        float x = 0.0f;
        float z = 0.0f;
        tl::core::stick_destination(move_x, move_y, camera_yaw, me.x, me.z, forward_x, forward_z,
                                    true, &x, &z);
        set_player_target_direction({x, 0.0f, z});
    }
    if (frame.let_targets_go) {
        // `setTarget(0)` (which empties the item slot too)
        // and the target item (`+0x28c`).
        set_player_target(0);
        player_item = PlayerItemTarget{};
    }
    if (frame.reset_ai) {
        // `setAIState(0)` (`CPlayer` vtable +0x170): the walk to
        // somebody (`approachAI`, 9) and to an item (`getItemAI`, 7) end.
        pending_talk.clear();
        clear_talk_target();
    }
    if (frame.destination) {
        // the stick's point without the fallback; with the stick
        // at rest, (0, 0).
        float x = 0.0f;
        float z = 0.0f;
        if (!tl::core::stick_destination(move_x, move_y, camera_yaw, me.x, me.z, forward_x,
                                         forward_z, false, &x, &z)) {
            x = 0.0f;
            z = 0.0f;
        }
        set_player_destination_held(x, z);
    }
}

/// The player has walked over to somebody they chose: say what they say. Called every
/// frame while a talk is pending.
void Renderer::Impl::step_pending_talk() {
    if (pending_talk.empty()) return;
    if (script_dialog_open()) return;

    Ogre::Vector3 target;
    std::string found;
    if (!unit_position(pending_talk, target, found)) {
        // They died, or the level moved on. Nothing to walk to.
        pending_talk.clear();
        return;
    }

    // `CCharacter::approachAI`, on the ground
    // plane: within 2.0 the unit is reached; a character that has stopped is
    // also within reach up to 2.75, and gives up (`setTarget(nullptr)`, AI
    // state 2) beyond it.
    const tl::core::Vec3 me = player.position();
    const float dx = target.x - me.x;
    const float dz = target.z - me.z;
    const float away = std::sqrt(dx * dx + dz * dz);
    if (2.0f < away) {
        if (player.following()) return;
        if (!(away <= 2.75f)) {
            TL_LOG_INFO("gave up walking to '%s': %.2f away", found.c_str(),
                        static_cast<double>(away));
            pending_talk.clear();
            clear_talk_target();
            return;
        }
    }

    const std::string unit = found;
    pending_talk.clear();
    player.stop_following();

    // A `Unit Trigger`'s prop reached (the nearest of that unit's): the pad's
    // A on it, `interact_targeted_trigger`, which is how a headless run opens
    // an enchant shrine.
    {
        int trigger_machine = kNoMachine;
        int64_t trigger_node = 0;
        float nearest = std::numeric_limits<float>::max();
        const auto consider = [&](int machine, const tl::core::LevelScript& owner) {
            for (const tl::core::LevelScript::UnitTriggerView& view : owner.unit_triggers()) {
                if (lowered(view.unit) != lowered(unit)) continue;
                const float ddx = view.at.x - target.x;
                const float ddz = view.at.z - target.z;
                const float d = ddx * ddx + ddz * ddz;
                if (d >= nearest) continue;
                nearest = d;
                trigger_machine = machine;
                trigger_node = view.id;
            }
        };
        consider(kLevelMachine, script);
        for (size_t i = 0; i < nested_scripts.size(); ++i) {
            consider(static_cast<int>(i), nested_scripts[i].script);
        }
        if (trigger_machine != kNoMachine) {
            trigger_targeted_machine = trigger_machine;
            trigger_targeted_node = trigger_node;
            interact_targeted_trigger();
            return;
        }
    }
    // The reached unit's half of `approachAI`: it targets the player and stops.
    // Not a stash (0x80) or the shared stash (0xaa): their branches set the
    // player's state and return.
    load_unit_types();
    const uint32_t reached_type = unit_types.id_of(stats_for_unit(unit).unittype);
    if (!unit_types.isa(reached_type, tl::core::unit_type::kStash) &&
        !unit_types.isa(reached_type, tl::core::unit_type::kSharedStash)) {
        int64_t node = 0;
        const int machine = unit_machine(unit, node);
        if (tl::core::LevelScript* owner = machine_script(machine)) {
            std::vector<tl::core::ScriptEvent> events;
            owner->approached_by_player(node, events);
            apply_machine_events(machine, events);
        }
    }
    // Face them, as CGameClient::updateIngame does; the model faces +Z, so yaw is atan2(x, z).
    player.place(me, std::atan2(dx, dz));

    interact_with(unit);
}

/// One frame of the player controller, from whatever the platform reports.
void Renderer::Impl::drive_player(float delta) {
    // Whether the hotbar read the pad last frame; any return before it is
    // reached below leaves this false for the next.
    const bool hotbar_had_pad_before = hotbar_had_pad;
    hotbar_had_pad = false;
    if (!player_control || !subject_node) return;

    // `CGameClient::processIngameInput` takes
    // nothing while the interface is in the cinematic menu; the screen reads
    // the pad itself.
    const tl::core::InputState in =
        cinematic_open ? tl::core::InputState{} : tl::core::poll_input();
    controller_connected = in.controller_connected;
    if (in.touch_tap && inventory_menu.visible()) {
        inventory_menu.touch_select_at(in.touch_x, in.touch_y);
    }
    if (in.touch_layout_changed) {
        tl::core::GameSettings& settings = game_settings();
        settings.touch_control_offsets = in.touch_layout_offsets;
        if (!config.settings_path.empty()) {
            std::ofstream file(config.settings_path, std::ios::binary | std::ios::trunc);
            file << tl::core::write_game_settings(settings);
            if (!file) TL_LOG_WARN("touch controls: could not save layout to '%s'",
                                   config.settings_path.c_str());
        }
    }
    // + / Start: the Pause Menu. It,
    // Options and the debug menu (a DIVERGENCE, debug_menu_open) have the pad.
    const bool quit_edge = in.quit && !quit_held;
    quit_held = in.quit;
    step_settings_menu();
    // Help has the pad. A full-screen menu (CGameUI +0x1798), not a modal one, so the level
    // is not held under it.
    if (howto_menu.open) {
        const bool back_edge = in.inventory && !inventory_button_held;
        inventory_button_held = in.inventory;
        drive_howto_menu(in, delta, quit_edge, back_edge);
        confirm_held = in.confirm;
        skip_held = in.skip;
        step_player(delta, tl::core::InputState{});
        return;
    }
    if (pause_menu.open || settings_menu.open) {
        if (settings_menu.open) {
            drive_settings_menu(in);
        } else {
            drive_pause_menu(in, quit_edge);
        }
        confirm_held = in.confirm;
        skip_held = in.skip;
        step_player(delta, tl::core::InputState{});
        return;
    }
    if (debug_menu_open) {
        if (quit_edge) {
            close_debug_menu();
        } else {
            drive_debug_menu(in);
        }
        confirm_held = in.confirm;
        skip_held = in.skip;
        step_player(delta, tl::core::InputState{});
        return;
    }
    // Start on the inventory tab - the bag's own screen, not a vendor's - is
    // `CInventoryMenuXenon`'s code 0xd: the pet commands
    // dialog. While that dialog is up, Start is one of its codes too.
    const bool pet_commands_start = quit_edge && inventory_menu.visible() &&
                                    inventory_menu.menu_mode() == MenuMode::Inventory &&
                                    inventory_menu.vendor() == VendorScreen::None &&
                                    !inventory_menu.item_select_open() &&
                                    !inventory_menu.spell_remove_open() && pet.alive();
    // On the skills tab Start is the tab's code 0xd, which closes the menu.
    //
    const bool skills_tab_start =
        quit_edge && inventory_menu.visible() && inventory_menu.menu_mode() == MenuMode::Skills &&
        inventory_menu.vendor() == VendorScreen::None && !inventory_menu.item_select_open() &&
        !inventory_menu.pet_commands_open() && !inventory_menu.spell_remove_open();
    if (quit_edge && !pet_commands_start && !skills_tab_start &&
        !inventory_menu.pet_commands_open() && !inventory_menu.spell_remove_open() &&
        !player_death.dying && !death_screen_up && !modal_screen.visible() && !tip_open &&
        !waypoint_screen.visible() && !quest_dialog.visible() && !script_dialog_open()) {
        open_pause_menu();
        confirm_held = in.confirm;
        skip_held = in.skip;
        step_player(delta, tl::core::InputState{});
        return;
    }
    // The right stick zooms only while the game itself has the pad
    // (is the in-game dispatch; the menus take their own).
    const bool camera_blocked = player_death.dying || inventory_menu.visible() ||
                                quest_dialog.visible() || script_dialog_open();
    camera_zoom_input = camera_blocked ? 0.0f : in.zoom;
    game_camera.touch.take(in, camera_blocked);

    // The menu's edges are taken every frame, so a button held from before the menu
    // opened is not a fresh press.
    const bool menu_next_edge = in.menu_next && !menu_next_held;
    const bool menu_prev_edge = in.menu_prev && !menu_prev_held;
    const bool tab_next_edge = in.tab_next && !tab_next_held;
    const bool tab_prev_edge = in.tab_prev && !tab_prev_held;
    const bool action_y_edge = in.action_y && !action_y_held;
    const bool action_x_edge = in.attack && !action_x_held;
    // The releases, for the skills tab's map sheet (`GamepadButtonUp`).
    const bool menu_next_release = !in.menu_next && menu_next_held;
    const bool menu_prev_release = !in.menu_prev && menu_prev_held;
    const bool action_y_release = !in.action_y && action_y_held;
    menu_next_held = in.menu_next;
    menu_prev_held = in.menu_prev;
    tab_next_held = in.tab_next;
    tab_prev_held = in.tab_prev;
    action_y_held = in.action_y;
    action_x_held = in.attack;

    // Dead: nothing but the death screen answers the pad (`CGameClient::clickLeft`
    // and `processIngameInput` refuse a dead player). Up and down move the
    // highlight, A takes it, Y is `Exit to Title` (`GUIEXITGAME`).
    if (player_death.dying) {
        const bool confirm_edge = in.confirm && !confirm_held;
        confirm_held = in.confirm;
        if (death_screen_up) {
            if (std::abs(in.move_y) > 0.5f) {
                if (!death_stick_held && !config.hardcore) {
                    death_selected = (death_selected + (in.move_y > 0.5f ? 2 : 1)) % 3;
                    refresh_death_screen();
                }
                death_stick_held = true;
            } else {
                death_stick_held = false;
            }
            if (action_y_edge) {
                // `CDieMenu::onClick(0)` -> `requestSetGameState(0)`: the title, unsaved.
                death_screen_up = false;
                death_screen.hide();
                quit_requested = true;
            } else if (confirm_edge && !config.hardcore) {
                choose_resurrection(static_cast<tl::core::ResurrectChoice>(death_selected));
            }
        }
        tl::core::InputState zero_in;
        step_player(delta, zero_in);
        return;
    }

    // The quest offer window has the pad before the menu: A accepts, B declines, the left
    // stick toggles the reward row; a read-only line has one OK.
    if (quest_dialog.visible()) {
        const bool confirm_edge = in.confirm && !confirm_held;
        const bool skip_edge = in.skip && !skip_held;
        confirm_held = in.confirm;
        skip_held = in.skip;

        // DIVERGENCE: the speaker's highlight goes out while the
        // camera is on them, and the selector picks up again once the window
        // closes. Running the selector inactive drops the pick and restores
        // the unit's rim light.
        update_pad_selection(false);

        if (std::abs(in.move_x) > 0.5f) {
            if (!inventory_stick_x_held) quest_dialog.toggle_rewards();
            inventory_stick_x_held = true;
        } else {
            inventory_stick_x_held = false;
        }

        if ((confirm_edge || skip_edge) && script_dialog_window) {
            // A line a `Quest Controller` opened: `CQuestDialogMenu::onClick`
            // with 10 (`OK`, event 2), 9
            // (`Accept`, event 3) or 8 (`Decline`, event 4). Accepting takes
            // the quest first.
            const tl::core::QuestDefinition* quest = offered_quest;
            const bool was_offer = quest_dialog.state().offer;
            // B on a `FORCEACCEPT` offer is the close button, which takes the
            // quest too (`handle_CloseButton`); `Decline` is hidden.
            const bool close_button = !confirm_edge && quest_dialog.state().force_accept;
            bool accepts = confirm_edge || quest_dialog.state().force_accept;
            script_dialog_window = false;
            // onClick takes the quest and
            // giveOrRemoveItems makes the speaker a pet before the window closes. A full
            // log goes on as a decline.
            if (accepts && was_offer && quest != nullptr) accepts = accept_offered_quest(*quest);
            close_quest_offer();
            std::vector<tl::core::ScriptEvent> events;
            const int machine = dialog_machine();
            if (tl::core::LevelScript* owner = machine_script(machine)) {
                if (was_offer && close_button) {
                    owner->close_button_dialog(quest_log.state(), events);
                } else if (was_offer) {
                    owner->answer_dialog(accepts, quest_log.state(), events);
                } else {
                    owner->dismiss_dialog(quest_log.state(), events);
                }
            }
            apply_machine_events(machine, events);
        } else if (confirm_edge || skip_edge) {
            const tl::core::QuestDefinition* quest = offered_quest;
            const bool was_offer = quest_dialog.state().offer;
            // Accepted before the window closes, as onClick
            // does: MAKE_PET_ON_ACCEPT needs the speaker the window holds.
            // B on a FORCEACCEPT offer: handle_CloseButton accepts regardless.
            const bool close_button = !confirm_edge && quest_dialog.state().force_accept;
            bool accepts = confirm_edge || quest_dialog.state().force_accept;
            // A full log goes on as a decline (`onClick`).
            if (accepts && was_offer && quest != nullptr) accepts = accept_offered_quest(*quest);
            // `OK` (10) is menu event 2, `Accept` (9) 3 and `Decline` (8) 4.
            finish_talk_window(!was_offer     ? 2
                               : close_button ? tl::core::LevelScript::kCloseButtonAccepts
                               : accepts      ? 3
                                              : 4);
        }
        // `CQuestDialogMenu::onClick` calls
        // `CLevel::updateNPCIcons`.
        if (confirm_edge || skip_edge) update_npc_icons();

        // The player neither walks nor swings while it is open.
        tl::core::InputState zero_in = in;
        zero_in.move_x = 0.0f;
        zero_in.move_y = 0.0f;
        zero_in.confirm = false;
        step_player(delta, zero_in);
        return;
    }

    // The modal message has the pad while it is up (`CModalMenu`): A is its
    // `Ok`, the only button `openModalDialog` leaves shown.
    if (modal_screen.visible()) {
        const bool confirm_edge = in.confirm && !confirm_held;
        confirm_held = in.confirm;
        skip_held = in.skip;
        if (confirm_edge) modal_screen.hide();
        tl::core::InputState zero_in;
        step_player(delta, zero_in);
        return;
    }

    // An open context tip has the pad: its handler answers B and Y only,
    // and processIngameInput takes none under a modal.
    if (tip_open) {
        if (in.skip && !skip_held) {
            press_tip_menu(tl::core::kMenuPadB);
        } else if (action_y_edge) {
            press_tip_menu(tl::core::kMenuPadY);
        }
        confirm_held = in.confirm;
        skip_held = in.skip;
        tl::core::InputState zero_in;
        step_player(delta, zero_in);
        return;
    }

    // The waypoint menu (`CWaypointMenu`) has the pad while it is up, and the
    // character stands.
    if (waypoint_screen.visible()) {
        const bool confirm_edge = in.confirm && !confirm_held;
        const bool skip_edge = in.skip && !skip_held;
        confirm_held = in.confirm;
        skip_held = in.skip;
        drive_waypoint_menu(in, confirm_edge, skip_edge);
        tl::core::InputState zero_in;
        step_player(delta, zero_in);
        return;
    }

    // Fishing (AI 0x20..0x22): the ring's two buttons in 0x21; no movement.
    // INVENTED: nothing else on the pad is read in 0x20 and 0x22 either.
    if (fishing_state != tl::core::FishingState::Off) {
        const bool confirm_edge = in.confirm && !confirm_held;
        const bool skip_edge = in.skip && !skip_held;
        confirm_held = in.confirm;
        skip_held = in.skip;
        step_fishing(delta, confirm_edge, skip_edge);
        tl::core::InputState zero_in;
        step_player(delta, zero_in);
        return;
    }

    // Toggle Inventory/Stats/Quest menu with "-" button / 'I' / Tab. The pet
    // commands dialog hears Back first (code 0xe) and closes only itself.
    const bool back_edge = in.inventory && !inventory_button_held;
    if (back_edge && !inventory_menu.pet_commands_open() && !inventory_menu.spell_remove_open()) {
        // Menus play a sound in and out (CInventoryMenu: InventoryOpen / InventoryClose,
        // played by setOpen); on the console the
        // skills tab plays no close (close_master_menu).
        if (inventory_menu.visible()) {
            close_master_menu();
        } else {
            inventory_menu.toggle();
            play_ui(tl::core::InterfaceSound::MenuOpen);
            update_menu();
            update_character_preview(0.0f);
        }
    }
    inventory_button_held = in.inventory;

    // Automap controls: toggle with 'M' / Right Stick click (a turned free camera's reset), zoom
    const bool map_toggle_edge = in.map_toggle && !map_toggle_held && !game_camera.touch.took_r3;
    map_toggle_held = in.map_toggle;
    if (map_toggle_edge) {
        automap.cycle_mode();
    }
    if (in.map_zoom_in) {
        automap.zoom_in();
    }
    if (in.map_zoom_out) {
        automap.zoom_out();
    }

    // The pet's two commands. On the press, like every other one-shot here: a
    // held button would cycle the stance once a frame.
    const bool pet_stance_edge = in.pet_stance && !pet_stance_held;
    const bool pet_town_edge = in.pet_town && !pet_town_held;
    pet_stance_held = in.pet_stance;
    pet_town_held = in.pet_town;
    // The skills tab gives the D-pad's up and down to the two skill sets
    // (buttons 1 and 2), so the pet's two jobs wait.
    const bool skills_tab_open =
        inventory_menu.visible() && inventory_menu.menu_mode() == MenuMode::Skills;
    // Under the pet commands dialog the D-pad is a code like any other, and
    // closes it (`vfunc_2`'s default case).
    const bool pet_dialog_up =
        inventory_menu.pet_commands_open() || inventory_menu.spell_remove_open();
    if (pet_stance_edge && pet.alive() && !skills_tab_open && !pet_dialog_up) {
        pet.cycle_stance();
        apply_pet_stance();
        const tl::core::PetStance stance = pet.stance();
        TL_LOG_INFO("pet: %s", stance == tl::core::PetStance::Aggressive  ? "aggressive"
                               : stance == tl::core::PetStance::Defensive ? "defensive"
                                                                          : "passive");
        refresh_game_hud();
    }
    if (pet_town_edge && !skills_tab_open && !pet_dialog_up) {
        send_pet_to_town();
        update_menu();
    }

    // A closed menu forgets which item its details were for, so the next
    // opening starts the 0.65 s again.
    if (!inventory_menu.visible() || inventory_menu.menu_mode() != MenuMode::Inventory) {
        item_tip_key = -1;
        item_tip_shown = false;
    }

    // When menu is open, handle navigation and consume controls
    if (inventory_menu.visible()) {
        update_character_preview(delta);

        // Start on the bag's tab opens the pet commands dialog, which then
        // takes the pad as the item select does: both are `CGameUI`'s modal
        // messages (`+0x189c`, `+0x1894`).
        if (pet_commands_start && open_pet_commands()) {
            play_ui(tl::core::InterfaceSound::MenuOpen);
        } else if (inventory_menu.pet_commands_open()) {
            PetCommandEdges edges;
            edges.y = action_y_edge;
            edges.start = quit_edge;
            edges.back = back_edge;
            edges.other = menu_next_edge || menu_prev_edge || tab_next_edge || tab_prev_edge ||
                          pet_stance_edge || pet_town_edge;
            handle_pet_commands(in, edges);
        } else if (inventory_menu.spell_remove_open()) {
            // The replace-spell dialog, the third of `CGameUI`'s modal messages
            // (`+0x1898`), takes the pad the same way.
            handle_spell_remove(in, action_x_edge, action_y_edge, quit_edge, back_edge,
                                menu_next_edge || menu_prev_edge || tab_next_edge ||
                                    tab_prev_edge || pet_stance_edge || pet_town_edge);
            confirm_held = in.confirm;
            skip_held = in.skip;
            tl::core::InputState zero_in = in;
            zero_in.move_x = 0.0f;
            zero_in.move_y = 0.0f;
            zero_in.confirm = false;
            step_player(delta, zero_in);
            return;
        }
        if (inventory_menu.pet_commands_open() || pet_commands_start) {
            confirm_held = in.confirm;
            skip_held = in.skip;
            tl::core::InputState zero_in = in;
            zero_in.move_x = 0.0f;
            zero_in.move_y = 0.0f;
            zero_in.confirm = false;
            step_player(delta, zero_in);
            return;
        }

        // The item-select dialog takes the pad while it is up: its housing is
        // the screen's modal message (`CGameUI +0x1894`), and the menu under
        // it hears nothing.
        if (inventory_menu.item_select_open()) {
            handle_item_select(in);
            confirm_held = in.confirm;
            skip_held = in.skip;
            tl::core::InputState zero_in = in;
            zero_in.move_x = 0.0f;
            zero_in.move_y = 0.0f;
            zero_in.confirm = false;
            step_player(delta, zero_in);
            return;
        }

        // The skills tab's handler takes B, LT and RT itself:
        // under its map sheet they are three of the four buttons a skill goes.
        //
        const bool skills_tab = inventory_menu.menu_mode() == MenuMode::Skills &&
                                inventory_menu.vendor() == VendorScreen::None;

        // B button / Escape closes menu
        if (in.skip && !skip_held && !skills_tab) {
            close_master_menu();
        }

        // LT / RT tab switching. A vendor's screen has no ring:
        // CMerchantMenuXenon::vfunc_6 and CEnchantMenuXenon::vfunc_6
        // empty the list (step answers 0xc; opens 0-3).
        const bool ring = inventory_menu.vendor() == VendorScreen::None;
        if (inventory_menu.visible() && menu_next_edge && !skills_tab && ring) {
            step_master_menu_tab(1);
        }
        if (inventory_menu.visible() && menu_prev_edge && !skills_tab && ring) {
            step_master_menu_tab(-1);
        }

        if (inventory_menu.visible() && skills_tab) {
            SkillsTabPad pad;
            pad.dpad_up = pet_stance_edge;
            pad.dpad_down = pet_town_edge;
            pad.lt = menu_prev_edge;
            pad.rt = menu_next_edge;
            pad.y = action_y_edge;
            pad.x = action_x_edge;
            pad.start = skills_tab_start;
            pad.lt_up = menu_prev_release;
            pad.rt_up = menu_next_release;
            pad.y_up = action_y_release;
            handle_skills_tab(in, pad);
        }

        if (inventory_menu.menu_mode() == MenuMode::Inventory) {
            const bool vendor_screen = inventory_menu.vendor() != VendorScreen::None;
            const bool vendor_tray = inventory_menu.focused_tray() == InventoryMenu::Tray::Vendor;
            if (vendor_screen) {
                tray_bumpers(true, tab_next_edge, tab_prev_edge);
                if (std::abs(in.stick_x) > 0.5f) {
                    if (!inventory_stick_x_held) {
                        inventory_menu.step_tray_tab(in.stick_x > 0.0f ? 1 : -1);
                        play_ui(tl::core::InterfaceSound::Navigate);
                        update_menu();
                    }
                    inventory_stick_x_held = true;
                } else {
                    inventory_stick_x_held = false;
                }
            } else {
                tray_bumpers(false, tab_next_edge, tab_prev_edge);
                if (std::abs(in.stick_x) > 0.5f) {
                    if (!inventory_stick_x_held) {
                        inventory_menu.step_bag_tab(in.stick_x > 0.0f ? 1 : -1);
                        play_ui(tl::core::InterfaceSound::Navigate);
                        update_menu();
                    }
                    inventory_stick_x_held = true;
                } else {
                    inventory_stick_x_held = false;
                }
            }
            // The D-pad's left and right (codes 3 and 4) on the focused tray:
            // item compare.
            const bool compare_edge = in.dpad_x != 0.0f && !item_compare_held;
            item_compare_held = in.dpad_x != 0.0f;
            if (compare_edge) toggle_item_compare();
            step_item_tooltips(delta);

            const bool pet_tray = inventory_menu.pet_tray_focused();

            // Stick Y / D-pad up/down for item selection
            if (std::abs(in.move_y) > 0.5f) {
                if (!inventory_stick_y_held) {
                    if (in.move_y > 0.5f) inventory_menu.select_prev();
                    else inventory_menu.select_next();
                    update_menu();
                }
                inventory_stick_y_held = true;
            } else {
                inventory_stick_y_held = false;
            }

            // A button: use / drink / equip / unequip selected item. On a
            // combiner's page A does nothing, on either tray.
            const bool combine_screen = inventory_menu.vendor() == VendorScreen::Combine;
            // On the pet's tray A with the pet as the owner
            // (`pet_tray_press`), outside a combiner's page.
            if (in.confirm && !confirm_held && pet_tray && !combine_screen) pet_tray_press();
            bool own_a =
                in.confirm && !confirm_held && !vendor_tray && !combine_screen && !pet_tray;
            if (own_a && (inventory_menu.vendor() == VendorScreen::Enchant ||
                          inventory_menu.vendor() == VendorScreen::EnchantShrine)) {
                // on an enchanter's or a shrine's page:.
                own_a = enchant_selected();
            } else if (own_a && (inventory_menu.vendor() == VendorScreen::RecoverGems ||
                                 inventory_menu.vendor() == VendorScreen::DestroyGems)) {
                // on a gem service's page: (mode 2)
                // or (mode 3), whatever the item.
                gem_service_selected();
                own_a = false;
            } else if (own_a && inventory_menu.vendor() == VendorScreen::Retire) {
                // on the retirement's page:.
                retire_selected();
                own_a = false;
            } else if (own_a && inventory_menu.enchanter_screen()) {
                own_a = false;
            }
            if (own_a) bag_default_action(inventory_menu.selected_slot_index());

            // Y drops the selected item (the layout's buttonYtext); equipped gear is not
            // offered.
            if (action_y_edge && combine_screen) {
                // code 8 on a page of type 5, either tray:
                // `CMerchantMenuXenon`'s transmute.
                transmute_selected();
            } else if (action_y_edge && pet_tray) {
                drop_selected_pet_item();
            } else if (action_y_edge && !vendor_tray && !inventory_menu.enchanter_screen()) {
                // (On `CEnchantMenuXenon`'s pages Y does nothing:
                // returns once /a668/a6d8/a740 see the menu.)
                const int slot_idx = inventory_menu.selected_slot_index();
                if (slot_idx >= 0 &&
                    static_cast<size_t>(slot_idx) < player_inventory.slots().size() &&
                    player_inventory.slots()[static_cast<size_t>(slot_idx)].count > 0 &&
                    !tl::core::Inventory::is_equip_location(static_cast<size_t>(slot_idx))) {
                    const auto& slot = player_inventory.slots()[static_cast<size_t>(slot_idx)];
                    const tl::core::ItemStats item = slot.item;
                    const int count = slot.count;
                    player_inventory.remove_item(static_cast<size_t>(slot_idx), count);
                    drop_item_near_player(item, count);
                    update_menu();
                    refresh_game_hud();
                }
            }

            // X: the tray dispatch by the screen and the tray.
            if (action_x_edge && housing_inventory_for(inventory_menu.vendor()) != nullptr) {
                // into the Transmuter or the stash from the
                // bag, back out from their own tray.
                swap_selected();
            } else if (action_x_edge && inventory_menu.vendor() == VendorScreen::Merchant) {
                // on the merchant's tray a buy,
                // on the bag a sale.
                trade_selected();
            } else if (action_x_edge && !vendor_screen) {
                // on the bag's screen: from the pet's tray to
                // the player's bag, from the bag to the pet's when
                // allows.
                swap_bag_and_pet();
            }
        } else if (inventory_menu.menu_mode() == MenuMode::Stats) {
            // CStatsMenuXenon's handler: codes 0x11 and 0x12,
            // up and down, step the selection.
            if (std::abs(in.move_y) > 0.5f) {
                if (!inventory_stick_y_held) {
                    if (tl::core::stats_page_step(stats_page, in.move_y > 0.5f ? -1 : 1)) {
                        play_ui(tl::core::InterfaceSound::Navigate);
                    }
                    update_menu();
                }
                inventory_stick_y_held = true;
            } else {
                inventory_stick_y_held = false;
            }

            // Stick X left can also switch back to Inventory tab
            if (in.move_x < -0.5f) {
                if (!inventory_stick_x_held) {
                    inventory_menu.prev_menu_tab();
                    update_menu();
                }
                inventory_stick_x_held = true;
            } else {
                inventory_stick_x_held = false;
            }

            // Code 5, A: one more point pending on the selection, `POINTASSIGN`
            // (slot 0x1b) when one moved. The character keeps its points until
            // the page closes (`sync_stats_page`).
            if (in.confirm && !confirm_held) {
                if (tl::core::stats_page_add_point(stats_page,
                                                   player_character.unspent_stat_points())) {
                    play_ui(tl::core::InterfaceSound::PointAssign);
                }
                if (stats_page.dirty) update_menu();
            }

            // Code 8, Y: one pending point back (with -1).
            if (action_y_edge && tl::core::stats_page_remove_point(
                                     stats_page, player_character.unspent_stat_points())) {
                play_ui(tl::core::InterfaceSound::PointAssign);
                update_menu();
            }
        } else if (inventory_menu.menu_mode() == MenuMode::Quest) {
            // Up and down walk the six trays.
            if (std::abs(in.move_y) > 0.5f) {
                if (!inventory_stick_y_held) {
                    if (in.move_y > 0.5f) inventory_menu.select_prev();
                    else inventory_menu.select_next();
                    update_menu();
                }
                inventory_stick_y_held = true;
            } else {
                inventory_stick_y_held = false;
            }

            // Stick right switches to the Inventory tab, the way stick left
            // does on the Stats tab: the two ends of the same ring.
            if (in.move_x > 0.5f) {
                if (!inventory_stick_x_held) {
                    inventory_menu.next_menu_tab();
                    update_menu();
                }
                inventory_stick_x_held = true;
            } else {
                inventory_stick_x_held = false;
            }

            // Y abandons a quest that authors CANABANDON. The confirmation window is not
            // wired (the layout ships it hidden).
            if (action_y_edge && inventory_menu.selected_quest_can_be_abandoned()) {
                const std::string name = inventory_menu.selected_quest_name();
                if (quest_log.abandon(name)) {
                    // CQuestMenu::abandonQuest plays slot 35 (BADEFFECT_SOUND),
                    // bound to `LowMana` - the engine's own choice.
                    play_ui(tl::core::InterfaceSound::QuestAbandoned);
                    TL_LOG_INFO("quest abandoned: %s", name.c_str());
                    // `CQuestManager::removeQuest`:
                    // `cleanUp(false)` takes
                    // the quest's items back, then the icons.
                    apply_quest_item_removals();
                    // The console's tab refreshes itself after
                    // `removeQuest` (its vtable +0x20).
                    inventory_menu.refresh_quest_tab();
                    update_automap_icons();
                    update_npc_icons();
                    update_menu();
                }
            }
        }

        confirm_held = in.confirm;
        skip_held = in.skip;

        // Player does not walk or swing while menu is open
        tl::core::InputState zero_in = in;
        zero_in.move_x = 0.0f;
        zero_in.move_y = 0.0f;
        zero_in.confirm = false;
        step_player(delta, zero_in);
        return;
    }

    // A through a line on the press, not the hold; the press is spent on the line, not
    // also on the item or trigger below.
    bool line_took_a = false;
    if (in.confirm && !confirm_held && script_dialog_open()) {
        std::vector<tl::core::ScriptEvent> events;
        const int machine = dialog_machine();
        machine_script(machine)->dismiss_dialog(quest_log.state(), events);
        apply_machine_events(machine, events);
        line_took_a = true;
    }
    // The buttons act on the console's target selector;
    // no pointer. A walks to a friendly pick and talks; X ignores the pick
    // (step_attack_button). X = 7, A = 5.
    const unsigned long long select_started = root->getTimer()->getMicroseconds();
    const bool have_selection = update_pad_selection(true);
    const bool free_to_act = !script_dialog_open() && !script_player_disabled && !line_took_a;
    // The item target's own slot (`CCharacter::setTargetItem`), only sought
    // while nothing is targeted for combat/talk - the original clears
    // whichever of the two it is not using.
    const bool have_item = update_item_selection(!have_selection);
    prof_select_us += root->getTimer()->getMicroseconds() - select_started;
    // The stick walk reads the hold flag (+0x1fe)
    // before the X block sets it: the frame X goes down still walks.
    const bool held_before = attack_button.hold;
    const bool attack_down = in.attack || auto_attack;
    // A is not handled while X is down.
    const bool a_free = !attack_down;
    if (in.confirm && !confirm_held && have_selection && !pad_selected_hostile && free_to_act &&
        a_free && pad_highlighted != nullptr && pad_highlighted->getParentSceneNode() != nullptr) {
        // Somebody friendly: a walk, then the conversation on arrival
        // (CCharacter::approachAI; step_pending_talk()). Routed at them (talk_to_unit()).
        pending_talk = pad_selected;
        set_talk_target(pad_selected);
        // `setTarget` lets the target item go.
        player_item = PlayerItemTarget{};
        const Ogre::Vector3 at = pad_highlighted->getParentSceneNode()->_getDerivedPosition();
        if (nav.ready()) {
            std::vector<tl::core::Vec3> route =
                nav.find_path(player.position(), tl::core::Vec3{at.x, at.y, at.z});
            if (route.empty()) {
                TL_LOG_INFO("no route to '%s'", pad_selected.c_str());
            } else {
                player.follow(std::move(route));
            }
        }
    }
    // The stick walk: past 0.1 it lets the targets go
    // (setTarget(0), +0x28c). Under the hold the X handler does it (step_attack_button).
    if (!held_before && player_item.kind != PlayerItemTarget::Kind::None &&
        0.1f < std::sqrt(in.move_x * in.move_x + in.move_y * in.move_y)) {
        TL_LOG_INFO("pad item: the target item is let go (the stick)");
        player_item = PlayerItemTarget{};
    }
    // A on the targeted item: setTargetItem, then getItemAI walks and uses it
    // (step_item_ai). A waiting mimic becomes the target and attackAI wakes it
    // (step_mimic_attack). DIVERGENCE (2026-10-09): the console's selector never
    // offers a sleeping mimic; the port keeps the PC's way.
    if (in.confirm && !confirm_held && have_item && free_to_act && a_free) {
        if (mimic_targeted_machine != kNoMachine) {
            pending_talk.clear();
            clear_talk_target();
            player_item = PlayerItemTarget{};
            player_item.kind = PlayerItemTarget::Kind::Mimic;
            player_item.machine = mimic_targeted_machine;
            player_item.node = mimic_targeted_node;
            TL_LOG_INFO("pad prop: A makes the mimic %lld the target",
                        static_cast<long long>(mimic_targeted_node));
        } else {
            target_selected_item();
        }
    }
    // The hotbar: LB/RB potions; Y, B, LT, RT the mapped skills (over
    // CPlayer +0x840/+0x890). Nothing under the letterbox; B stays a cutscene's skip.
    if (free_to_act && !hud.letterboxed()) {
        using tl::core::HotbarButton;
        std::array<bool, 7> pressed{};
        std::array<bool, 7> held{};
        // A button already down when the hotbar gets the pad back - the B that
        // closed the inventory or skipped a scene - is not a skill
        // held down: it is ignored until it is let go.
        const auto set = [&](HotbarButton button, bool down, bool edge) {
            bool& waiting = hotbar_wait_release[static_cast<size_t>(button)];
            if (!hotbar_had_pad_before && down) waiting = true;
            if (!down) waiting = false;
            if (waiting) return;
            pressed[static_cast<size_t>(button)] = edge;
            held[static_cast<size_t>(button)] = down && !edge;
        };
        hotbar_had_pad = true;
        const bool skipping = script.busy();
        set(HotbarButton::LB, in.tab_prev, tab_prev_edge);
        set(HotbarButton::RB, in.tab_next, tab_next_edge);
        set(HotbarButton::Y, in.action_y, action_y_edge);
        set(HotbarButton::B, in.skip && !skipping, in.skip && !skip_held && !skipping);
        set(HotbarButton::LT, in.menu_prev, menu_prev_edge);
        set(HotbarButton::RT, in.menu_next, menu_next_edge);
        release_held_skill(pressed, held, (in.confirm && !confirm_held) || action_x_edge);
        step_hotbar(pressed, held, in.move_x, in.move_y);
    }
    // After the slot loop: the hold and aim, the press, the
    // handler. attack_held is +0x55; a menu marks X in +0x57 instead,
    // so X still down when a menu goes is a fresh press.
    step_attack_button(delta, attack_down, attack_down && !attack_held, in.move_x, in.move_y);
    confirm_held = in.confirm;
    attack_held = attack_down;

    // Throwing the whole scene away. On the press, and only while something is
    // actually running - a `Skip Cutscene` node fired at an idle moment would
    // run a scene's cleanup over a scene that never started.
    if (in.skip && !skip_held && script.busy()) {
        std::vector<tl::core::ScriptEvent> events;
        script.skip_cutscene(quest_log.state(), events);
        if (!events.empty()) {
            TL_LOG_INFO("script: cutscene skipped, %zu event(s)", events.size());
            apply_script_events(events, scripted_nodes);
        }
        player.stop_following();
        scripted_walk_path = 0;
    }
    skip_held = in.skip;

    // A disabled player character does not answer the stick; a scripted walk
    // still moves it (`CPlayer::update` runs `updatePathFollowing(true)`).
    if (script_player_disabled) {
        // CCharacter::handInteractiveSequence, AI state
        // 0x2c each frame: a performed skill is asked to stop; once it has,
        // with no path (+0x66c), the route is dropped (+0x1fc, timer +0x208 to 10). The
        // console leaves the destination where it was.
        bool skill_held = false;
        if (player_performing_skill()) {
            attempt_to_stop_player_skill(false);
            skill_held = player_performing_skill();
        }
        if (!skill_held && scripted_walk_path == 0 && player.following()) player.stop_following();
        tl::core::InputState held_in = in;
        held_in.move_x = 0.0f;
        held_in.move_y = 0.0f;
        held_in.confirm = false;
        step_player(delta, held_in);
        return;
    }
    // Under the hold the stick aims and does not walk (the walk
    // is called only with `+0x1fe` clear).
    tl::core::InputState walk_in = in;
    if (held_before) {
        walk_in.move_x = 0.0f;
        walk_in.move_y = 0.0f;
    }
    // The stick routes the character to a point two units off
    // (`gamepad.h`'s `stick_destination`): a new order, and the conversation's
    // target is let go.
    if (walk_in.move_x != 0.0f || walk_in.move_y != 0.0f) {
        clear_talk_target();
        pending_talk.clear();
    }
    step_player(delta, walk_in);
}

// CCinematicMenu::setOpen hides the in-game interface
// under a letterbox (CGameUI::setIngameUIVisible(false); back).
// DIVERGENCE: the same for a conversation; the PC leaves its HUD up there.
void Renderer::Impl::step_hud_visibility() {
    if (!player_control) return;
    const bool scene =
        hud.letterboxed() || quest_dialog.visible() || script_dialog_open() || !talk_unit.empty();
    const bool hide_hud = scene || controller_connected;
    // DIVERGENCE: the conversation's end lets the unit go as well -
    // no window, no vendor screen, no line over its head, no walk still on
    // its way to it.
    if (!stash_opened_unit.empty() && !inventory_menu.visible()) {
        animate_stash(stash_opened_unit, false);
    }
    if (talk_target_machine != kNoMachine && !scene && pending_talk.empty() &&
        inventory_menu.vendor() == VendorScreen::None && !(floaty_remaining > 0.0f)) {
        clear_talk_target();
    }
    if (hide_hud == hud_hidden_for_scene) return;
    hud_hidden_for_scene = hide_hud;
    if (hide_hud) {
        hud_hidden_game_hud = game_hud.visible();
        if (hud_hidden_game_hud) game_hud.hide();
        // `CLevel::setAutomapVisible(false)`,
        // from `CGameUI::setInteractiveMenuVisible`: whether the map was up is
        // kept (`gbWasVisible`), and `setAutomapVisible(true)` brings back only
        // a map that was.
        hud_hidden_automap = automap_initialized && automap.visible() &&
                             automap.mode() != tl::render::MapMode::Hidden;
        if (hud_hidden_automap) automap.hide();
        return;
    }
    if (hud_hidden_game_hud) game_hud.show();
    if (hud_hidden_automap) automap.show();
    hud_hidden_game_hud = false;
    hud_hidden_automap = false;
}

// CStashMenu::setOpen: OPEN once and IDLE queued as it opens, CLOSE as it
// shuts. INVENTED (from memory of play): the chest's `opening`/`closed` stand for OPEN/CLOSE,
// which CGenericModel::playAnimation would not match.
void Renderer::Impl::animate_stash(const std::string& unit, bool open) {
    // A spawned unit is listed in both places; animate each model once.
    std::set<const Ogre::Entity*> done;
    // `closed` is a pose, not an animation: whatever is still enabled - the
    // `opening` one-shot, held on its last frame - has to be switched off
    // first, or the two fight over the lid every frame.
    const auto quiet = [](Ogre::Entity* entity) {
        if (Ogre::AnimationStateSet* states = entity->getAllAnimationStates()) {
            for (auto& entry : states->getAnimationStates()) entry.second->setEnabled(false);
        }
    };
    const auto units = scene_units.equal_range(unit);
    for (auto it = units.first; it != units.second; ++it) {
        SceneUnit& placed = it->second;
        if (placed.entity == nullptr || !done.insert(placed.entity).second) continue;
        if (!open) quiet(placed.entity);
        Ogre::AnimationState* started = nullptr;
        const std::string animation = open ? "opening" : "closed";
        if (!play_scene_animation(placed.entity, placed.mesh, animation, false, placed.rate,
                                  /*optional=*/true, &started, placed.armed)) {
            continue;
        }
        if (started != nullptr) remember_state(placed.entity, animation, started);
        placed.playing = animation;
        placed.playing_state = started;
    }
    // The town's two are released by `Unit Spawner`s.
    for (const auto& owner : spawned) {
        for (const SpawnedUnit& made : owner.second) {
            if (made.unit != unit || made.entity == nullptr) continue;
            if (!done.insert(made.entity).second) continue;
            if (!open) quiet(made.entity);
            Ogre::AnimationState* started = nullptr;
            const std::string animation = open ? "opening" : "closed";
            if (play_scene_animation(made.entity, made.entity->getMesh()->getName(), animation,
                                     false, 1.0f, /*optional=*/true, &started, false) &&
                started != nullptr) {
                remember_state(made.entity, animation, started);
            }
        }
    }
    stash_opened_unit = open ? unit : std::string();
}

// PORT: which of the port's machines holds a scene unit of this name, and its node.
int Renderer::Impl::unit_machine(const std::string& unit, int64_t& node) const {
    node = 0;
    if (unit.empty()) return kNoMachine;
    const Ogre::SceneNode* where = nullptr;
    const auto it = scene_units.find(unit);
    if (it != scene_units.end() && it->second.node != 0) {
        node = it->second.node;
        if (it->second.entity != nullptr) where = it->second.entity->getParentSceneNode();
    } else {
        for (const auto& owner : spawned) {
            for (const SpawnedUnit& made : owner.second) {
                if (made.unit == unit && made.id != 0) {
                    node = made.id;
                    where = made.node;
                }
            }
        }
    }
    if (node == 0) return kNoMachine;
    const auto holds = [&](const NodeMap& nodes) {
        const auto range = nodes.equal_range(node);
        for (auto n = range.first; n != range.second; ++n) {
            if (where == nullptr || n->second == where) return true;
        }
        return false;
    };
    for (size_t i = 0; i < nested_scripts.size(); ++i) {
        if (nested_scripts[i].script.has_character(node) && holds(nested_scripts[i].nodes)) {
            return static_cast<int>(i);
        }
    }
    return script.has_character(node) ? kLevelMachine : kNoMachine;
}

// CQuestDialogMenu::updateDialogTags, end of setNPC:
// each `<LOOKAT=NAME>` makes the speaker
// target PLAYER or the level's unit of that data group
// (CUnitResourceList::getDataGroupByObjectName, CLevel::getBaseUnitByDataGroup).
// INVENTED: an item name (setTargetItem) is passed over.
void Renderer::Impl::update_dialog_tags(const std::string& unit, const std::string& text) {
    std::string look_at;
    (void)tl::core::strip_dialog_directives(text, &look_at);
    if (look_at.empty()) return;
    int64_t node = 0;
    const int machine = unit_machine(unit, node);
    tl::core::LevelScript* owner = machine_script(machine);
    if (owner == nullptr) return;
    std::vector<tl::core::ScriptEvent> events;
    if (tl::core::upper_ascii(look_at) == "PLAYER") {
        owner->target_player(node, events);
    } else {
        int64_t target = 0;
        std::string found;
        Ogre::Vector3 at;
        if (unit_position(look_at, at, found)) {
            tl::core::LevelScript* there = machine_script(unit_machine(found, target));
            if (there != nullptr) owner->target_unit(node, there, target, events);
        }
        if (target == 0) {
            TL_LOG_WARN("  <LOOKAT=%s>: nothing of that name in the scene", look_at.c_str());
        }
    }
    TL_LOG_INFO("  <LOOKAT=%s>: '%s' targets it", look_at.c_str(), unit.c_str());
    apply_machine_events(machine, events);
}

void Renderer::Impl::apply_script_events(const std::vector<tl::core::ScriptEvent>& events,
                                         NodeMap& nodes) {
    using tl::core::ScriptAction;
    const tl::core::SectionScope timed(tl::core::Section::ScriptEvents);

    for (const tl::core::ScriptEvent& event : events) {
        const auto range = nodes.equal_range(event.node_id);

        switch (event.action) {
            case ScriptAction::SetVisible: {
                for (auto it = range.first; it != range.second; ++it) {
                    it->second->setVisible(event.on);
                }
                // And the particle systems this node placed: a Layout Link Particle's
                // content is a nested layout, not in scripted_nodes.
                const size_t systems = show_node_particles(event, nodes);
                // How far from the player, on the ground, a unit is shown or
                // hidden: what a report of pop-in is checked against (the near
                // list's ranges, the update's camera test).
                float away = -1.0f;
                Ogre::Vector3 at = Ogre::Vector3::ZERO;
                if (range.first != range.second) {
                    at = range.first->second->_getDerivedPosition();
                    const tl::core::Vec3 me = player.position();
                    away = std::sqrt((at.x - me.x) * (at.x - me.x) + (at.z - me.z) * (at.z - me.z));
                }
                TL_LOG_INFO("script: %s '%s'%s, %.1f from the player at (%.1f, %.1f)",
                            event.on ? "show" : "hide", event.name.c_str(),
                            systems > 0 ? " (with its particles)" : "", static_cast<double>(away),
                            static_cast<double>(at.x), static_cast<double>(at.z));
                break;
            }

            case ScriptAction::SetEnabled:
                // A pathing blocker is the one thing whose enabled flag is a
                // fact about the *world*: it is a region a route may not cross,
                // and the town's cutscene opens the way to the mine by
                // switching two of them off.
                apply_path_blocker(event.node_id, event.on, &nodes, /*live=*/true);
                // Otherwise enabling is the script object's own state (a trigger stops
                // testing, a timer counting), not the scene's.
                break;

            case ScriptAction::ParticleLinkPause: {
                // `CParticle::Pause`: a shown `CParticle`
                // (`+0x81`) pauses each of its systems (`+0x108`). The link's
                // own, not another copy of its file's.
                size_t systems = 0;
                for (ParticleSystem& system : link_or_free_systems(nodes, event.node_id)) {
                    if (system.owner != event.node_id || system.pu.system == nullptr) continue;
                    if (system.owner_nodes != nullptr && system.owner_nodes != &nodes) continue;
                    if (pause_torchlight_particle(system.pu)) ++systems;
                }
                TL_LOG_INFO("script: particle link '%s' pauses (%zu system(s))", event.name.c_str(),
                            systems);
                break;
            }

            case ScriptAction::ParticleNode: {
                // Start/Stop Particle on a level Particle node: CParticleTechWrapper::start
                // / stop
                // on this machine's copy.
                size_t systems = 0;
                for (ParticleSystem& system : node_wrappers(nodes, event.node_id)) {
                    if (!system.wrapper || system.node != event.node_id ||
                        system.node_owner != &nodes) {
                        continue;
                    }
                    if (event.on) {
                        start_torchlight_wrapper(system.pu);
                    } else {
                        stop_torchlight_wrapper(system.pu);
                    }
                    ++systems;
                }
                if (systems > 0) {
                    TL_LOG_INFO("script: particle '%s' %s (%zu system(s))", event.name.c_str(),
                                event.on ? "starts" : "stops", systems);
                }
                break;
            }

            case ScriptAction::EmitterEnabled: {
                for (ParticleSystem& system : map_wrappers(nodes)) {
                    if (!system.wrapper || system.node_owner != &nodes) continue;
                    if (set_torchlight_emitter_enabled(system.pu, event.node_id, event.on)) {
                        TL_LOG_INFO("script: emitter '%s' %s", event.name.c_str(),
                                    event.on ? "on" : "off");
                    }
                }
                break;
            }

            case ScriptAction::AffectorEnabled: {
                // `CAffectorWrapper::setEnabled`, on a wrapper's
                // affector only (see ParticleNode). A track sets its value
                // every frame its timeline moves; the log has the changes.
                for (ParticleSystem& system : map_wrappers(nodes)) {
                    if (!system.wrapper || system.node_owner != &nodes) continue;
                    bool changed = false;
                    if (set_torchlight_affector_enabled(system.pu, event.node_id, event.on,
                                                        &changed) &&
                        changed) {
                        TL_LOG_INFO("script: affector '%s' %s", event.name.c_str(),
                                    event.on ? "on" : "off");
                    }
                }
                break;
            }

            case ScriptAction::MoveNode: {
                // Already in the world: a room's machine turns its file's keys
                // into the level's coordinates itself (`LevelScript::set_frame`).
                const Ogre::Vector3 to(event.position.x, event.position.y, event.position.z);
                for (auto it = range.first; it != range.second; ++it) {
                    it->second->setPosition(to);
                }
                // A `Layout Link Particle` has no node of its own here: its
                // file's systems move with it.
                for (ParticleSystem& system : link_systems(nodes, event.node_id)) {
                    if (system.owner != event.node_id || system.owner_nodes != &nodes) continue;
                    Ogre::SceneNode* holder = system.holder();
                    if (holder == nullptr) continue;
                    system.link_position = to;
                    holder->setPosition(to + system.link_orientation * system.offset_from_link);
                }
                break;
            }

            case ScriptAction::HideHandTrails: {
                // `CCharacter::interrupt`: `CWeaponTrail::setActive(false)` and
                // hidden, for both hands (X360 `+0x5f8`, `+0x5fc`).
                for (const auto& entry : scene_units) {
                    const SceneUnit& drawn = entry.second;
                    if (drawn.entity == nullptr) continue;
                    const Ogre::SceneNode* parent = drawn.entity->getParentSceneNode();
                    bool here = false;
                    for (auto it = range.first; it != range.second; ++it) {
                        if (it->second == parent) here = true;
                    }
                    if (!here) continue;
                    const auto trails = unit_trails.find(drawn.entity);
                    if (trails != unit_trails.end()) trails->second.trails.hide_hands();
                }
                break;
            }

            case ScriptAction::ScaleNode: {
                // The model's offset under the node is -radius / scale in the
                // original (`CCharacter::scaleUpdated`); the port's
                // node stands at the feet, so the scale alone is the change.
                const Ogre::Real s = static_cast<Ogre::Real>(event.amount);
                if (!(s > 0.0f)) break;
                for (auto it = range.first; it != range.second; ++it) {
                    it->second->setScale(s, s, s);
                }
                break;
            }

            case ScriptAction::OrientNode: {
                // `CPositionableObject::setOrientation`: the node
                // turned to `Quaternion::FromRotationMatrix` of the columns as
                // they stand - OGRE 14's is the same Shoemake code as Runic's
                // 1.6.5, and both normalise in `Node::setOrientation`.
                Ogre::Matrix3 axes;
                axes.FromAxes(Ogre::Vector3(event.axis_x.x, event.axis_x.y, event.axis_x.z),
                              Ogre::Vector3(event.axis_y.x, event.axis_y.y, event.axis_y.z),
                              Ogre::Vector3(event.axis_z.x, event.axis_z.y, event.axis_z.z));
                Ogre::Quaternion turned;
                turned.FromRotationMatrix(axes);
                if (turned.isNaN() || turned.Norm() <= 0.0f) break;
                turned.normalise();
                for (auto it = range.first; it != range.second; ++it) {
                    it->second->setOrientation(turned);
                }
                // ... and a `Layout Link Particle`'s systems turn with it.
                for (ParticleSystem& system : link_systems(nodes, event.node_id)) {
                    if (system.owner != event.node_id || system.owner_nodes != &nodes) continue;
                    Ogre::SceneNode* holder = system.holder();
                    if (holder == nullptr) continue;
                    system.link_orientation = turned;
                    holder->setPosition(system.link_position + turned * system.offset_from_link);
                    holder->setOrientation(turned * system.turn_from_link);
                }
                break;
            }

            case ScriptAction::JumpLanded:
                // `jumpDownAI`: `LAND_SOUND`
                // (slot 0x11) where the jump ends.
                play_sound(stats_for_unit(event.unit).sounds[tl::core::SoundSlot::Land],
                           Ogre::Vector3(event.position.x, event.position.y, event.position.z));
                break;

            case ScriptAction::DeathLanded: {
                // `dyingAI`: `LAND_SOUND`
                // (slot 0x11) at the end of a flight, unless the body fell.
                if (event.on) {
                    play_sound(stats_for_unit(event.unit).sounds[tl::core::SoundSlot::Land],
                               Ogre::Vector3(event.position.x, event.position.y, event.position.z));
                }
                break;
            }

            case ScriptAction::PlayerPet: {
                // `CMonsterDescriptor::InputLogicEvent`
                // cases 0x48 and 0x49 on the
                // player: `addPet`, with its `MAXIMUM_PET_INSTANCES`, or
                // `removePet`.
                tl::core::LevelScript* machine = script_owning(event.node_id);
                if (machine == nullptr) break;
                if (event.on) {
                    add_player_pet(*machine, event.node_id);
                } else {
                    machine->remove_player_pet(event.node_id);
                }
                TL_LOG_INFO("script: %s %s the player's pets", event.name.c_str(),
                            event.on ? "joins" : "leaves");
                break;
            }

            case ScriptAction::CastShadows: {
                // `CGenericModel::setCastsShadows` on the model and its worn
                // pieces (`updateAnimation`).
                for (auto it = range.first; it != range.second; ++it) {
                    set_node_casts_shadows(it->second, event.on);
                }
                break;
            }

            case ScriptAction::StopUnitParticles: {
                // `CCharacter::stopParticles`
                // on the unit this node placed.
                for (auto& entry : scene_units) {
                    SceneUnit& su = entry.second;
                    if (su.node != event.node_id || su.entity == nullptr) continue;
                    apply_trigger_steps(su.themes, su.themes.triggers.stop_particles(event.on));
                }
                break;
            }

            case ScriptAction::Opacity: {
                // `CGenericModel::setOpacity`
                // on the model and its worn pieces.
                for (auto it = range.first; it != range.second; ++it) {
                    set_node_opacity(it->second, event.amount);
                }
                break;
            }

            case ScriptAction::CameraOn: {
                // The camera is the script's until `CameraOff`; where it looks
                // comes in `CameraLookAt` every frame, the pan already applied.
                close_for_camera();
                scripted_camera.active = true;
                scripted_camera.position =
                    Ogre::Vector3(event.position.x, event.position.y, event.position.z);
                scripted_camera.follow = event.follow_id;
                scripted_camera.follow_position = scripted_camera.position;
                scripted_camera.have_follow_position = true;
                scripted_camera.elapsed = 0.0f;
                scripted_camera.pan_time = event.pan_time;
                scripted_camera.ease_at = event.ease_at;
                TL_LOG_INFO(
                    "script: camera '%s' takes over at (%.2f, %.2f, %.2f)%s, "
                    "panning for %.2fs",
                    event.name.c_str(), static_cast<double>(event.position.x),
                    static_cast<double>(event.position.y), static_cast<double>(event.position.z),
                    event.follow_id != 0 ? ", following a unit" : "",
                    static_cast<double>(event.pan_time));
                break;
            }

            case ScriptAction::CameraLookAt:
                if (scripted_camera.active) {
                    scripted_camera.follow_position =
                        Ogre::Vector3(event.position.x, event.position.y, event.position.z);
                    scripted_camera.have_follow_position = true;
                }
                break;

            case ScriptAction::PlayerEnabled:
                if (script_player_disabled == event.on) {
                    TL_LOG_INFO("script: player %s", event.on ? "enabled" : "disabled");
                }
                script_player_disabled = !event.on;
                break;

            case ScriptAction::CameraShake: {
                // CCameraShake::startCameraShake(position, false). DIVERGENCE: the camera
                // owns a copy, so a second Activate adds a second shake (the machine holds
                // no shake objects).
                if (event.node == nullptr) break;
                load_camera_shake_graphs();
                tl::core::CameraShake shake =
                    tl::core::camera_shake_from_node(*event.node, camera_shake_graphs);
                shake.start(tl::core::Vec3f{event.position.x, event.position.y, event.position.z},
                            /*owned=*/true, camera_shakes);
                TL_LOG_INFO("camera: '%s' shakes '%s' (%.2f s, x%.2f)%s", event.name.c_str(),
                            shake.name.c_str(), static_cast<double>(shake.duration),
                            static_cast<double>(shake.magnitude),
                            shake.curve != nullptr ? "" : ", no curve: nothing moves");
                break;
            }

            case ScriptAction::CameraOff:
                if (scripted_camera.active) {
                    TL_LOG_INFO("script: camera '%s' hands back", event.name.c_str());
                }
                scripted_camera = ScriptedCamera{};
                break;

            case ScriptAction::GameState:
                // Logged: it is what makes a cutscene's progress verifiable on
                // the console, where nobody is watching.
                TL_LOG_INFO("script: game state '%s' (%s, %u) %s", event.name.c_str(),
                            event.text.empty() ? "unauthored" : event.text.c_str(), event.input,
                            event.on ? "on" : "off");
                game_state_interface(event.input, event.on);
                break;

            case ScriptAction::PlayerInvincible:
                // `performStateControl`: the
                // player's `+0x52e`.
                if (subject_node == nullptr) break;
                player_invincible = event.on;
                TL_LOG_INFO("script: the player is %s", event.on ? "invincible" : "mortal again");
                break;

            case ScriptAction::ClearDungeonHistory: {
                // `CDungeonObject::clearHistory`
                // on the player.
                if (config.travel == nullptr) break;
                tl::core::clear_dungeon_history(*config.travel, event.name);
                tl::core::update_stored_levels(*config.travel, level_dungeon, level_depth, {},
                                               level_memory_rules());
                TL_LOG_INFO("script: the player's history of '%s' is cleared", event.name.c_str());
                break;
            }

            case ScriptAction::TeleportPlayer: {
                // CTeleport::activate on the player:
                // stopPathing, setPosition, setDirection, dropToGround(level, 4.0, false),
                // teleportToMaster(1.0) for each minion (+0x648).
                if (subject_node == nullptr) break;
                player.stop_following();
                scripted_walk_path = 0;
                const Ogre::Vector3 ground = dropped_to_ground(
                    Ogre::Vector3(event.position.x, event.position.y, event.position.z));
                player.place({ground.x, ground.y, ground.z}, event.facing);
                subject_node->setPosition(ground);
                subject_node->setOrientation(
                    Ogre::Quaternion(Ogre::Radian(event.facing), Ogre::Vector3::UNIT_Y));
                size_t brought = 0;
                for (const MachineView& view : machines()) {
                    for (const int64_t id : view.script->player_pets()) {
                        if (view.script->put_pet_by_master(
                                id, tl::core::Vec3{ground.x, ground.y, ground.z}, 1.0f)) {
                            ++brought;
                        }
                    }
                }
                TL_LOG_INFO(
                    "script: '%s' teleports the player to (%.2f, %.2f, %.2f), %zu minion(s) "
                    "with it",
                    event.name.c_str(), static_cast<double>(ground.x),
                    static_cast<double>(ground.y), static_cast<double>(ground.z), brought);
                break;
            }

            case ScriptAction::HealPlayer:
                TL_LOG_INFO("script: '%s' heals the player", event.name.c_str());
                heal_player();
                break;

            case ScriptAction::ShowTip:
                // `CGameStateController::showHelp`: `CGameUI::queueTip`.
                TL_LOG_INFO("script: '%s' shows tip %u", event.name.c_str(), event.input);
                queue_tip(static_cast<tl::core::ContextTip>(event.input));
                break;

            case ScriptAction::PlayerMeshVisible: set_player_mesh_visible(event.on); break;

            case ScriptAction::CompleteQuest:
                // `CQuestDialogMenu::onClick`: `OK` on a `[COMPLETE]`
                // line, `CQuestManager::completeQuest`.
                //
                TL_LOG_INFO("script: '%s' is completed by its line", event.quest.c_str());
                complete_quest_by_dialog(event.quest);
                break;

            case ScriptAction::QuestChanged:
                TL_LOG_INFO("script: quest '%s' is now %s", event.text.c_str(),
                            event.status == tl::core::QuestStatus::Complete ? "complete"
                            : event.status == tl::core::QuestStatus::Active ? "active"
                                                                            : "not held");
                // CQuestController::setQuestComplete
                // ends in CQuestManager::completeQuest, whose consequences are run here.
                // Force Not Accepted / Not Complete: removeQuest(quest, false), resetQuest.
                if (event.status == tl::core::QuestStatus::Unknown) quest_log.drop(event.text);
                if (event.status == tl::core::QuestStatus::Complete) {
                    load_quest_book();
                    if (const tl::core::QuestDefinition* quest = quest_book.find(event.text)) {
                        quest_log.apply_completion_consequences(*quest);
                    }
                    pay_quest_completions();
                }
                report_gates_waiting_on(event.text);
                // `giveQuest` / `completeQuest`, which a `Quest Controller`
                // calls, both end in `updateAutomapIcons` and `updateNPCIcons`.
                update_automap_icons();
                update_npc_icons();
                break;

            case ScriptAction::MoveUnit: {
                const Ogre::Vector3 to(event.position.x, event.position.y, event.position.z);
                // The first frame a path moves a unit, log where it picked it up.
                if (!event.name.empty() && unit_path[event.node_id] != event.name) {
                    unit_path[event.node_id] = event.name;
                    TL_LOG_INFO("script: path '%s' takes %s from (%.2f, %.2f, %.2f)",
                                event.name.c_str(), event.unit.c_str(), static_cast<double>(to.x),
                                static_cast<double>(to.y), static_cast<double>(to.z));
                }
                // Addressed by *node*, not by unit name: a path drives every
                // monster of its unit and each has its own place along it, so
                // moving them all to one position stacks them.
                for (auto it = range.first; it != range.second; ++it) {
                    Ogre::SceneNode* node = it->second;
                    node->setPosition(to);
                    // Turned only when it moved (models face +Z); a path that moves nobody
                    // leaves the layout's orientation alone.
                    if (event.turned) {
                        node->setOrientation(
                            Ogre::Quaternion(Ogre::Radian(event.facing), Ogre::Vector3::UNIT_Y));
                    }
                }
                break;
            }

            case ScriptAction::Animation: {
                // An Animation Controller naming no animation has nothing to do.
                if (event.text.empty()) break;
                if (event.to_player) {
                    // The level's player. A class without the file is passed
                    // over, as `setAIPlayAnimation`
                    // does.
                    if (!event.on) {
                        if (scripted_player_animation == event.text) {
                            scripted_player_animation.clear();
                            TL_LOG_INFO("script: '%s' lets the player go back to idle",
                                        event.name.c_str());
                        }
                    } else if (subject != nullptr &&
                               !archive_path_in_dir(event.text + ".SKELETON", subject_dir)
                                    .empty() &&
                               link_subject_animation(event.text)) {
                        scripted_player_animation = event.text;
                        scripted_player_loops = event.loops;
                        // From its start, also when it is the one last played.
                        player_animation.clear();
                        TL_LOG_INFO("script: '%s' plays '%s' on the player (%s)",
                                    event.name.c_str(), event.text.c_str(),
                                    event.loops ? "looping" : "once");
                    } else {
                        TL_LOG_INFO(
                            "script: '%s' asks the player for '%s', which its model "
                            "does not have",
                            event.name.c_str(), event.text.c_str());
                    }
                    break;
                }
                const auto units = scene_units.equal_range(event.unit);
                if (units.first == units.second) {
                    TL_LOG_WARN("script: no unit '%s' to %s '%s' on", event.unit.c_str(),
                                event.on ? "play" : "stop", event.text.c_str());
                    break;
                }
                // One unit or all: an Animation Controller names a MONSTER (all of that
                // name); a hunt names its node.
                bool one_only = false;
                if (event.node_id != 0) {
                    for (auto it = units.first; it != units.second; ++it) {
                        if (it->second.node == event.node_id) one_only = true;
                    }
                }

                size_t reached = 0;
                for (auto it = units.first; it != units.second; ++it) {
                    if (one_only && it->second.node != event.node_id) continue;
                    if (event.on) {
                        Ogre::AnimationState* started = nullptr;
                        // The class that actually started (see play_scene_animation()):
                        // HIT frames and FOOTSTEP tags are looked up by it.
                        std::string played = event.text;
                        // Blended in over the event's time (`setAIPlayAnimation`
                        // `blendAnimation(.., blend, speed)`,
                        // or a play when the blend or the length is 0).
                        if (play_scene_animation(it->second.entity, it->second.mesh, event.text,
                                                 event.loops, it->second.rate * event.rate,
                                                 /*optional=*/true, &started, it->second.armed,
                                                 &played, event.blend)) {
                            ++reached;
                            // Under both names when they differ: a later
                            // `Stop` names what the level asked for, not what
                            // the rig turned out to have.
                            remember_state(it->second.entity, event.text, started);
                            if (played != event.text) {
                                remember_state(it->second.entity, played, started);
                            }
                            it->second.playing = played;
                            it->second.playing_state = started;
                            it->second.last_time = 0.0f;
                            // `setAnimationDuration`'s step at once.
                            if (0.0f < event.advance) {
                                step_entity_animation(it->second.entity, event.advance);
                            }
                            // A one-shot goes back to the unit's idle when it
                            // has run: `setAIPlayAnimation` queues `IDLE`.
                            if (!event.queue.empty() && started != nullptr) {
                                // One-shots queued behind it, then the idle.
                                PendingIdle next;
                                next.node = it->second.node;
                                next.unit = event.unit;
                                next.state = started;
                                next.animation = event.queue.front();
                                next.loops = false;
                                for (size_t q = 1; q < event.queue.size(); ++q) {
                                    next.after.emplace_back(event.queue[q], false);
                                }
                                if (event.then_idle)
                                    next.after.emplace_back(kUnitIdleAnimation, true);
                                pending_idles.push_back(next);
                            } else if (event.then_idle && started != nullptr) {
                                PendingIdle idle;
                                idle.node = it->second.node;
                                idle.unit = event.unit;
                                idle.state = started;
                                // `queueBlendAnimation("IDLE", true, 0.2, speed)`,
                                // at the one-shot's own speed.
                                Ogre::AnimationState* queued = nullptr;
                                if (play_scene_animation(
                                        it->second.entity, it->second.mesh, kUnitIdleAnimation,
                                        true, it->second.rate * event.rate,
                                        /*optional=*/true, &queued, it->second.armed, nullptr,
                                        /*blend=*/0.2f, /*queue=*/true)) {
                                    idle.queued = queued;
                                }
                                pending_idles.push_back(idle);
                            }
                        }
                    } else {
                        // Only this controller's animation, never everything on the unit
                        // (a Play and a Stop on one unit can land together).
                        if (Ogre::AnimationState* state =
                                remembered_state(it->second.entity, event.text)) {
                            state->setEnabled(false);
                            ++reached;
                        }
                        if (event.then_idle) {
                            Ogre::AnimationState* idle = nullptr;
                            play_scene_animation(it->second.entity, it->second.mesh,
                                                 kUnitIdleAnimation, true, it->second.rate,
                                                 /*optional=*/true, &idle, it->second.armed);
                            remember_state(it->second.entity, kUnitIdleAnimation, idle);
                        }
                    }
                }
                TL_LOG_INFO("script: %s '%s' on %zu %s entit%s", event.on ? "play" : "stop",
                            event.text.c_str(), reached, event.unit.c_str(),
                            reached == 1 ? "y" : "ies");
                break;
            }

            case ScriptAction::Dialog:
                // What matters to the scene is that the line is up: everything after the
                // Interact that opened it waits on it.
                if (event.on) {
                    // Two passes: strip_markup (|cAARRGGBB ... |u colour runs), then
                    // strip_dialog_directives (`<LOOKAT=NAME>`).
                    std::string look_at;
                    const std::string said = tl::core::strip_dialog_directives(
                        tl::core::strip_markup(event.text), &look_at);

                    if (event.floaty) {
                        // Over the speaker's head, no name and no box: nobody
                        // is being addressed and there is nothing to dismiss.
                        floaty_line = said;
                        floaty_unit = event.unit;
                        floaty_remaining = event.seconds;
                    } else {
                        // What a person calls them, not the key the data joins
                        // on: the town's `seer` is Syl and its `MercPet` is
                        // Brink.
                        dialog_line = said;
                        const tl::core::UnitStats& who = stats_for_unit(event.unit);
                        dialog_speaker = who.display_name;
                        if (dialog_speaker.empty()) dialog_speaker = event.unit;
                        // The quest window, which is what the original opens;
                        // the HUD line only where that window cannot load.
                        if (!open_script_dialog(event,
                                                tl::core::strip_dialog_directives(event.text))) {
                            hud.set_line(dialog_speaker, dialog_line, who.portrait);
                        }
                    }
                    // Floaty or boxed, the line's recording plays as it is shown
                    // (`forceInteractiveDialog` and
                    // `setNPC`).
                    play_dialog_sound(event.sound);
                    TL_LOG_INFO("script: %s says \"%s\" (%.2fs, %s)", event.unit.c_str(),
                                said.c_str(), static_cast<double>(event.seconds),
                                event.floaty ? "over their head" : "in a box, until dismissed");

                    // ... and then what the directive said (`updateDialogTags`).
                    update_dialog_tags(event.unit, event.text);
                } else if (event.floaty) {
                    floaty_line.clear();
                    floaty_unit.clear();
                    floaty_remaining = 0.0f;
                    TL_LOG_INFO("script: %s's line has faded", event.unit.c_str());
                } else {
                    // Closed by the script itself (a headless run's reading
                    // time): the window goes, and an accepted offer is taken.
                    if (script_dialog_window) {
                        script_dialog_window = false;
                        const tl::core::QuestDefinition* quest = offered_quest;
                        if (event.menu_event == 3 && quest != nullptr) accept_offered_quest(*quest);
                        close_quest_offer();
                    }
                    dialog_line.clear();
                    dialog_speaker.clear();
                    hud.set_line(std::string(), std::string());
                    TL_LOG_INFO("script: %s has finished speaking", event.unit.c_str());
                }
                break;

            case ScriptAction::Music:
                // Play carries the node's FILE NAME; Play Level Music means the rules'
                // track; Stop is on == false. Logged is what the wire asked for.
                if (!event.on) {
                    TL_LOG_INFO("script: '%s' asks for the music to stop", event.name.c_str());
                } else if (event.input == tl::core::logic::kPlayLevelMusic) {
                    TL_LOG_INFO("script: '%s' asks for the level's own music back",
                                event.name.c_str());
                } else {
                    TL_LOG_INFO("script: '%s' asks for %s (%s)", event.name.c_str(),
                                event.text.c_str(), event.loops ? "looping" : "once");
                }
                music_node_input(event);
                break;

            case ScriptAction::Transform: {
                // `convertCharacter`'s scene half, on the machine these nodes
                // are this machine's.
                tl::core::LevelScript* owner = nullptr;
                for (const MachineView& view : machines()) {
                    if (view.nodes == &nodes) owner = view.script;
                }
                if (owner == nullptr) owner = script_owning(event.node_id);
                pending_idles.erase(
                    std::remove_if(pending_idles.begin(), pending_idles.end(),
                                   [&](const PendingIdle& p) { return p.node == event.node_id; }),
                    pending_idles.end());
                unit_skill_playbacks.erase(
                    std::remove_if(unit_skill_playbacks.begin(), unit_skill_playbacks.end(),
                                   [&](const UnitSkillPlayback& p) {
                                       return p.machine == owner && p.node == event.node_id;
                                   }),
                    unit_skill_playbacks.end());
                convert_unit_body(owner, event.node_id, event.unit);
                if (owner == &script && event.node_id == pet_node_id) pet_converted(event.unit);
                break;
            }

            case ScriptAction::Kill: {
                // A body falls where it stands, addressed by node. INVENTED: the death
                // animation is `Die`, else `Die1`, else hidden (no data names one).
                // A swing cut short by death queued its idle; a body must not play it.
                pending_idles.erase(
                    std::remove_if(pending_idles.begin(), pending_idles.end(),
                                   [&](const PendingIdle& p) { return p.node == event.node_id; }),
                    pending_idles.end());
                bool fell = false;
                for (auto it = scene_units.begin(); it != scene_units.end(); ++it) {
                    if (it->second.node != event.node_id) continue;
                    // `CCharacter::die`:
                    // sample 0xc, `DEATH_SOUND`, of the unit's own bank at its
                    // node - every character, not only the player.
                    if (it->second.entity != nullptr) {
                        if (const Ogre::SceneNode* at = it->second.entity->getParentSceneNode()) {
                            // before the sound: the death particle (PARTICLE_DEATH
                            // or its censored twin) for a death the level script deals.
                            play_death_particle(stats_for_unit(it->first),
                                                at->_getDerivedPosition(),
                                                character_ref(event.node_id));
                            play_sound(stats_for_unit(it->first).sounds[tl::core::SoundSlot::Death],
                                       at->_getDerivedPosition());
                        }
                    }
                    // The burst or the flight, which take the death from the
                    // animation below.
                    if (die_burst_or_flight(event, it->second, nodes)) {
                        fell = true;
                        break;
                    }
                    for (const char* stem : {"Die", "Die1"}) {
                        Ogre::AnimationState* started = nullptr;
                        if (!play_scene_animation(it->second.entity, it->second.mesh, stem,
                                                  /*loops=*/false, it->second.rate,
                                                  /*optional=*/true, &started, it->second.armed)) {
                            continue;
                        }
                        remember_state(it->second.entity, stem, started);
                        // What it plays now, so the animation's keys are
                        // followed: a death's particles and sounds
                        // (`step_unit_particle_keys`, `step_unit_sounds`).
                        it->second.playing = stem;
                        it->second.playing_state = started;
                        fell = true;
                        TL_LOG_INFO("script: %s dies where it stands ('%s')", event.name.c_str(),
                                    stem);
                        break;
                    }
                    break;
                }
                if (!fell) {
                    for (auto it = range.first; it != range.second; ++it) {
                        it->second->setVisible(false);
                    }
                    TL_LOG_INFO("script: %s dies (no death animation beside its mesh)",
                                event.name.c_str());
                }
                break;
            }

            case ScriptAction::WalkPlayer: {
                // The level routes the player during a cutscene, through the navigation
                // grid: the waypoint is a destination.
                if (!player_control) {
                    TL_LOG_INFO("script: '%s' would walk the player, but nobody is holding it",
                                event.name.c_str());
                    break;
                }
                const tl::core::Vec3 to{event.position.x, event.position.y, event.position.z};
                std::vector<tl::core::Vec3> route;
                if (nav.ready()) route = nav.find_path(player.position(), to);
                if (route.empty()) route.push_back(to);
                player.follow(route);
                scripted_walk_path = event.node_id;
                TL_LOG_INFO("script: '%s' walks the player to (%.2f, %.2f), %zu leg(s)",
                            event.name.c_str(), static_cast<double>(to.x),
                            static_cast<double>(to.z), route.size());
                break;
            }

            case ScriptAction::NestedTimeline: {
                // Play on a Layout Link Timeline: run the linked file's machine (its
                // LAYOUT CONTROL timelines).
                size_t reached = 0;
                // By index, over the machines there now: the events applied below can
                // link another layout, a push_back that invalidates a deque's iterators.
                const size_t machines = nested_scripts.size();
                for (size_t i = 0; i < machines; ++i) {
                    NestedScript& machine = nested_scripts[i];
                    if (machine.owner != event.node_id) continue;
                    // The link's own layout (`CLayoutDescriptor::InputLogicEvent`
                    // on the one object), not that
                    // of another copy of the file holding it: four flame traps
                    // side by side share their `WarningParticles` id.
                    if (machine.owner_nodes != &nodes) continue;
                    std::vector<tl::core::ScriptEvent> inner;
                    if (event.on) {
                        if (!machine.running) {
                            machine.running = true;
                            machine.script.level_activated(quest_log.state(), inner);
                        }
                        machine.script.play_layout_timelines(quest_log.state(), inner,
                                                             event.backwards);
                    } else {
                        machine.script.stop_layout_timelines(quest_log.state(), inner);
                    }
                    if (!inner.empty()) {
                        apply_script_events(inner, machine.nodes);
                    }
                    ++reached;
                }
                TL_LOG_INFO("script: %s '%s' on %zu linked machine(s)",
                            event.on ? (event.backwards ? "play backwards" : "play") : "stop",
                            event.name.c_str(), reached);
                break;
            }

            case ScriptAction::LinkInput: {
                // CLayoutDescriptor::InputLogicEvent,
                // default: CLayout::processInputs on the
                // link's own layout, else the link's own base.
                bool taken = false;
                const size_t machines_now = nested_scripts.size();
                for (size_t i = 0; i < machines_now; ++i) {
                    NestedScript& machine = nested_scripts[i];
                    if (machine.owner != event.node_id) continue;
                    if (machine.owner_nodes != &nodes) continue;
                    std::vector<tl::core::ScriptEvent> inner;
                    taken = machine.script.process_inputs(event.input, quest_log.state(), inner) ||
                            taken;
                    if (!inner.empty()) apply_script_events(inner, machine.nodes);
                }
                TL_LOG_INFO("script: input 0x%x on link '%s' %s", event.input, event.name.c_str(),
                            taken ? "goes to its layout's timelines" : "stays with the link");
                if (taken) break;
                for (const MachineView& view : machines()) {
                    if (view.nodes != &nodes) continue;
                    std::vector<tl::core::ScriptEvent> back;
                    view.script->link_input_declined(event.node_id, event.input, quest_log.state(),
                                                     back);
                    if (!back.empty()) apply_script_events(back, nodes);
                    break;
                }
                break;
            }

            case ScriptAction::SpawnerInput:
                // A room's spawner stands where the room put it: its machine
                // runs in the level's coordinates.
                spawner_input(event);
                break;

            // `CUnitTriggerDescriptor::InputLogicEvent`,
            // inputs 0x4d and 0x4e: the
            // unit's collidable flag (`+0x18d`) and
            // `CBaseUnit::addToAvoidanceMap`
            // or `removeFromAvoidanceMap`.
            case ScriptAction::TriggerCollidable: {
                const int64_t key = instance_key(event.node_id, nodes);
                if (!collision.has_group(key)) break;
                set_trigger_collidable(key, event.on);
                TL_LOG_INFO("script: unit trigger '%s' is %s", event.name.c_str(),
                            event.on ? "collidable" : "not collidable");
                break;
            }

            case ScriptAction::ItemInteracted: {
                // `CItem::interact`:
                // `questEventFire(2, character, prop)`, which counts only for
                // the player.
                Ogre::Vector3 at = Ogre::Vector3::ZERO;
                const auto units = scene_units.equal_range(event.unit);
                for (auto it = units.first; it != units.second; ++it) {
                    if (it->second.node != event.node_id || it->second.entity == nullptr) continue;
                    if (const Ogre::SceneNode* where = it->second.entity->getParentSceneNode()) {
                        at = where->_getDerivedPosition();
                        break;
                    }
                }
                tl::core::QuestEventUnit prop =
                    quest_event_unit(event.unit, stats_for_unit(event.unit).unittype, at, {});
                quest_event(quest_log.unit_interacted(prop, event.by_player));
                break;
            }

            case ScriptAction::TriggerState: {
                // `CTriggerUnit::setTriggerState`
                // put the prop in
                // `trigger_state` (0 its first); the machine holds the state.
                TriggerProp prop;
                if (event.unit.empty() || !trigger_prop(event.unit, prop)) break;
                const int64_t key = instance_key(event.node_id, nodes);
                const int32_t state = event.trigger_state;

                // This machine's copies of the prop: each hangs off a scene
                // node in this machine's map (as trigger_entity()). A map with
                // no node for it keeps every copy of the node.
                std::vector<SceneUnit*> copies;
                const auto units = scene_units.equal_range(event.unit);
                for (auto it = units.first; it != units.second; ++it) {
                    if (it->second.node != event.node_id) continue;
                    if (range.first != range.second && it->second.entity != nullptr) {
                        const Ogre::SceneNode* where = it->second.entity->getParentSceneNode();
                        bool mine = false;
                        for (auto n = range.first; n != range.second && !mine; ++n) {
                            mine = n->second == where;
                        }
                        if (!mine) continue;
                    }
                    copies.push_back(&it->second);
                }

                // `trigger` before its
                // `setTriggerState`: a one-shot's last state spawns its
                // `[TREASURE]`, and the prop's skill is rolled (that, or any
                // `Cycle` firing).
                if (event.trigger_spent || event.trigger_skill) {
                    std::vector<const Ogre::SceneNode*> mine;
                    for (auto n = range.first; n != range.second; ++n) mine.push_back(n->second);
                    finish_trigger_unit(event.node_id, event.unit, prop, event.by_player, mine,
                                        event.trigger_spent, event.trigger_skill);
                }

                // `setTriggerState` itself leaves the collision alone: a door
                // stops blocking at its state animation's `DISABLECOLLISION` key
                // and blocks again at an `ENABLECOLLISION` one
                // (`CItem::updateAnimation`, `step_unit_particle_keys`), or
                // through the layout's `Collidable` inputs.
                (void)key;

                // The state's transition (+0x298) blended once, not looping,
                // then its own animation (looping with ANIMATION_STATE_LOOP_n); active for
                // the transition plus 0.3 s (+0x1e0). Not animated
                // (applySaveState): DIVERGENCE: in a later state the original jumps to the
                // last frame; this plays the transition.
                const bool first = state == event.trigger_first;
                const std::string& transition = first ? prop.closing : prop.opening;
                const std::string& held = first ? prop.resting : prop.activated;
                const std::string& looped = first ? prop.resting_loop : prop.activated_loop;
                const bool animate = event.trigger_animate || !first;
                float transition_length = 0.0f;
                size_t reached = 0;
                for (SceneUnit* unit : copies) {
                    set_trigger_state_layout(unit->entity, prop, state + 1);
                    ++reached;
                    if (!animate) continue;
                    std::vector<std::pair<std::string, bool>> steps;
                    if (!transition.empty()) steps.emplace_back(transition, false);
                    if (!held.empty()) steps.emplace_back(held, false);
                    if (!looped.empty()) steps.emplace_back(looped, true);
                    if (steps.empty()) continue;
                    Ogre::AnimationState* started = nullptr;
                    if (!play_scene_animation(unit->entity, unit->mesh, steps.front().first,
                                              steps.front().second, unit->rate,
                                              /*optional=*/true, &started, unit->armed)) {
                        continue;
                    }
                    if (!transition.empty() && started != nullptr) {
                        transition_length = started->getLength();
                    }
                    remember_state(unit->entity, steps.front().first, started);
                    unit->playing = steps.front().first;
                    unit->playing_state = started;
                    unit->last_time = 0.0f;
                    for (size_t step = 1; step < steps.size(); ++step) {
                        pending_trigger_loops.push_back({started, event.node_id, event.unit,
                                                         steps[step].first, steps[step].second});
                    }
                }
                if (animate && range.first != range.second) {
                    const auto presence = trigger_presence.find(key);
                    if (presence != trigger_presence.end()) {
                        presence->second.presence.forced_time = transition_length + 0.3f;
                    }
                }

                // `playSample(+0x1d8, state, node)` while the level is active:
                // `SOUND_STATE_ONE` / `SOUND_STATE_TWO`, else `INTERACT_SOUND`
                // (`addSoundsToState`).
                if (event.trigger_sound && state >= 0 && state < 2 &&
                    !prop.state_sound[state].empty() && !copies.empty() &&
                    copies.front()->entity != nullptr) {
                    if (const Ogre::SceneNode* at = copies.front()->entity->getParentSceneNode()) {
                        play_sound(prop.state_sound[state], at->_getDerivedPosition());
                    }
                }
                TL_LOG_INFO("script: unit trigger '%s' to state %d of %d..%d%s (%zu prop(s))",
                            event.name.c_str(), state, event.trigger_first, event.trigger_last,
                            event.on ? (event.trigger_spent ? ", fired and spent" : ", fired")
                                     : (event.trigger_forced ? ", forced" : ""),
                            reached);
                break;
            }

            case ScriptAction::WeaponMissile: {
                fire_unit_weapon_missile(script_owning(event.node_id), event.node_id, event.text,
                                         event.on, event.aimed, event.target_machine,
                                         event.target_node);
                break;
            }
            case ScriptAction::Strike: {
                // `CCharacter::rollAttack` decided the blow; `applyDamage` lands
                // it on whoever it names.
                const tl::core::UnitStats attacker = stats_for_unit(event.unit);
                const std::string who =
                    attacker.display_name.empty() ? event.unit : attacker.display_name;
                // What rollAttack shows: the hit particle
                // on the one struck (not for 0 damage), the strike particle.
                // INVENTED: no "Glancing Blow!" (no glancing flag here).
                {
                    Ogre::Vector3 from = Ogre::Vector3::ZERO;
                    Ogre::Vector3 at = Ogre::Vector3::ZERO;
                    if (unit_node_position(event.node_id, from) &&
                        unit_node_position(event.target_node, at)) {
                        tl::core::UnitStats victim = player_stats;
                        CharacterRef struck;
                        if (event.target_node != 0) {
                            tl::core::LevelScript* of = event.target_machine != nullptr
                                                            ? event.target_machine
                                                            : script_owning(event.target_node);
                            victim = stats_for_unit(of != nullptr ? of->unit_of(event.target_node)
                                                                  : std::string());
                            struck = character_ref(event.target_node, of);
                        }
                        if (!event.blocked && event.amount != 0.0f) {
                            play_hit_particle(victim, from, at, struck);
                        }
                        for (int i = 0; i < event.absorbed; ++i) show_absorbed(at, victim);
                        play_strike_particle(attacker, from, at, victim.radius, event.critical,
                                             event.blocked, character_ref(event.node_id));
                    }
                }
                // `rollAttack`: the knock-back, the degraded
                // armour and the attacker's `TRANSFER` effects onto the one
                // struck, blocked or not, ahead of `applyDamage`.
                if (event.roll_attack) {
                    tl::core::LevelScript* struck = nullptr;
                    if (event.target_node != 0) {
                        struck = event.target_machine != nullptr ? event.target_machine
                                                                 : script_owning(event.target_node);
                    }
                    unit_attack_effects(event.node_id, struck, event.target_node);
                }
                if (event.target_node == 0) {
                    if (event.blocked) {
                        // "Blocked!" over the one who blocked.
                        if (subject_node != nullptr) {
                            add_combat_text(tl::core::CombatText::Blocked,
                                            subject_node->getPosition());
                        }
                        TL_LOG_INFO("combat: player blocked attack from %s", who.c_str());
                    } else {
                        // `rollAttack`: `applyDamageEffects` ahead of
                        // `applyDamage`, at the attacker's level.
                        int level = 1;
                        tl::core::LevelScript* by = script_owning(event.node_id);
                        if (by != nullptr) by->unit_level(event.node_id, level);
                        apply_damage_status(
                            nullptr, 0, event.amount, event.by_type, event.parts, event.first_type,
                            event.critical, level,
                            tl::core::EffectSource{by != nullptr, by, event.node_id});
                        // `rollCritical`: over the target.
                        if (event.critical && subject_node != nullptr) {
                            add_combat_text(tl::core::CombatText::CriticalStrike,
                                            subject_node->getPosition());
                        }
                        // A damage-over-time shape's passes no struck flag.
                        hurt_player(event.amount, who, event.node_id, !event.over_time);
                    }
                } else {
                    std::string unit;
                    for (const auto& entry : scene_units) {
                        if (entry.second.node == event.target_node) unit = entry.first;
                    }
                    for (const auto& owner_entry : spawned) {
                        for (const SpawnedUnit& made : owner_entry.second) {
                            if (made.id == event.target_node) unit = made.unit;
                        }
                    }
                    tl::core::KillingBlow blow;
                    blow.has_direction = event.has_direction;
                    blow.direction = event.direction;
                    blow.force = event.force;
                    blow.critical = event.critical;
                    hurt_unit(event.target_node, unit, event.amount, event.critical, event.node_id,
                              event.target_machine, &blow, /*struck=*/true);
                }
                break;
            }

            case ScriptAction::Miss:
                play_sound(stats_for_unit(event.unit).miss_sound(),
                           range.first != range.second ? range.first->second->_getDerivedPosition()
                                                       : Ogre::Vector3::ZERO);
                break;

            case ScriptAction::DamageText:
                if (range.first != range.second) {
                    const Ogre::Vector3 over = range.first->second->_getDerivedPosition();
                    add_text_event(tl::core::make_damage_text(
                        over.x, over.y, over.z, static_cast<int>(event.amount), event.on));
                }
                break;

            case ScriptAction::UnitSample:
                // The bank is the unit's own (`CSoundBank`, `+0x298`); the slot
                // names the definition, which carries its own chance and radius.
                if (event.input < tl::core::kSoundSlots && range.first != range.second) {
                    const std::string sample =
                        stats_for_unit(event.unit)
                            .sounds[static_cast<tl::core::SoundSlot>(event.input)];
                    TL_LOG_INFO("script: %s plays its sample %u ('%s')", event.unit.c_str(),
                                event.input, sample.c_str());
                    // The machine made the sample's draws as it played it.
                    if (event.on) {
                        play_sound_drawn(event.sound, event.text, event.amount,
                                         range.first->second->_getDerivedPosition());
                    } else {
                        play_sound(sample, range.first->second->_getDerivedPosition());
                    }
                }
                break;

            case ScriptAction::Hunt:
                if (event.on && event.text.empty()) {
                    TL_LOG_INFO("script: %s starts hunting", event.unit.c_str());
                } else if (event.on) {
                    hunt_target[event.node_id] = event.target_node;
                    TL_LOG_INFO("script: %s hunts %s at (%.2f, %.2f, %.2f)", event.unit.c_str(),
                                event.text.c_str(), static_cast<double>(event.position.x),
                                static_cast<double>(event.position.y),
                                static_cast<double>(event.position.z));
                } else {
                    hunt_target.erase(event.node_id);
                    TL_LOG_INFO("script: %s has nothing left to hunt", event.unit.c_str());
                }
                break;

            case ScriptAction::Warp: {
                // CWarper::activate: with WAYPOINT, first
                // CPlayer::addWaypoint; then DUNGEON NAME, LEVEL DELTA, LEVEL ABSOLUTE to
                // CGameClient::warpLevels (recorded for performWarp, core/warp.h).
                if (event.waypoint && config.travel != nullptr) {
                    const bool added =
                        tl::core::add_waypoint(*config.travel, level_dungeon, level_depth);
                    TL_LOG_INFO("waypoint: %s depth %d %s by warper '%s'", level_dungeon.c_str(),
                                level_depth, added ? "added" : "not added", event.name.c_str());
                }
                const tl::core::TravelRecord none;
                const tl::core::TravelRecord& travel =
                    config.travel != nullptr ? *config.travel : none;
                const tl::core::WarpRequest request = tl::core::warp_levels(
                    event.text, event.level_delta, event.level_absolute, travel);
                const tl::core::WarpDestination to =
                    tl::core::warp_destination(request, warp_origin(), travel);
                if (!to.go) {
                    TL_LOG_INFO(
                        "script: warper '%s' leads nowhere (up out of depth %d of '%s', no "
                        "parent dungeon and no last dungeon)",
                        event.name.c_str(), level_depth, level_dungeon.c_str());
                    break;
                }
                if (to.dungeon == level_dungeon && request.absolute == 0) {
                    TL_LOG_INFO("dungeon: stairs %s taken on floor %d of '%s'",
                                to.depth < level_depth ? "up" : "down", level_depth + 1,
                                level_dungeon.c_str());
                }
                warp_pending = true;
                warp_dungeon = to.dungeon;
                // DIVERGENCE: a depth below 0 loads as 0 (no placed Warper reaches one).
                warp_floor = std::max(0, to.depth) + 1;
                warp_level_delta = request.delta;
                warp_level_absolute = request.absolute;
                warp_waypoint = event.waypoint;
                // `performWarp` plays `TOWNPORTALENTER` for a waypoint warp.
                if (event.waypoint) play_ui(tl::core::InterfaceSound::TownPortalEnter);
                dungeon_transition_pending = true;
                TL_LOG_INFO(
                    "script: warper '%s' ('%s', delta %d, absolute %d) sends the player to "
                    "'%s' depth %d",
                    event.name.c_str(), event.text.c_str(), event.level_delta, event.level_absolute,
                    warp_dungeon.c_str(), to.depth);
                break;
            }

            case ScriptAction::WaypointActivated: activate_waypoint(); break;

            case ScriptAction::OpenCinematic:
                // `CGameUI::setCinematicOpen(name)` from a layout's `Cinematic`
                // node: the run loop opens it between frames
                // (`Renderer::run`). A headless run has nobody to press
                // Continue; the menu closes at once (event 2).
                TL_LOG_INFO("script: cinematic '%s' opens '%s'", event.name.c_str(),
                            event.text.c_str());
                if (config.headless) {
                    for (const MachineView& view : machines()) view.script->cinematic_menu_event(2);
                } else {
                    pending_layout_cinematic = event.text;
                }
                break;

            case ScriptAction::Unhandled: break;
        }
    }
}

void Renderer::Impl::reset_script() {
    if (!script_ready) return;

    // Everything the script hid comes back. Nodes the layout itself hid with
    // VISIBLE were never placed at all, so there is nothing here to un-hide by
    // mistake.
    for (const auto& entry : scripted_nodes) {
        entry.second->setVisible(true);
        // And at full opacity: the fresh machine starts every character at 1
        // and says nothing until its fade moves.
        set_node_opacity(entry.second, 1.0f);
    }

    scripted_camera = ScriptedCamera{};
    interactive_menus = 0;
    quest_dialog_interactive = false;
    // What a `Game State Controller` left on the camera, the player, the client
    // and the skills.
    if (camera_cinematic.on) {
        tl::core::set_cinematic_mode(camera_cinematic, camera_dolly, false, camera_mult);
    }
    player_invincible = false;
    set_player_mesh_visible(true);
    tl::core::LevelScript::reset_client_paused();
    tl::core::globally_disable_skills(false);
    if (script_dialog_window) {
        script_dialog_window = false;
        close_quest_offer();
    }
    dialog_line.clear();
    dialog_speaker.clear();
    floaty_line.clear();
    floaty_unit.clear();
    floaty_remaining = 0.0f;
    // `CGameUI::hideTextEvents` (inline in the console's
    // `CGameClient::unloadCurrentLevel`): every
    // live text event's window off its parent and the event out of the live
    // list (interface `+0x14cc`) into the free one (`+0x14c8`).
    text_events.clear();
    hunt_target.clear();
    clear_missiles();
    hud.set_line(std::string(), std::string());
    hud.set_floaties({});
    hud.set_letterboxed(false);
    unit_path.clear();
    scripted_walk_path = 0;
    player.stop_following();
    // The whole log, not only the gate state: a `--visit` that accepted a
    // quest on the way past must not leave the player holding it. See
    // `Renderer::reset_script()`'s own note above.
    quest_log.reset(initial_quests);

    pending_animations.clear();
    pending_trigger_loops.clear();
    for (auto& held : trigger_state_layouts) destroy_particle_holders(held.second.holders);
    trigger_state_layouts.clear();
    pending_trigger_layouts = trigger_layout_props;
    door_avoidance_added.clear();
    trigger_not_collidable.clear();
    unit_states.clear();

    // The renderer's half of the fight's state (health and wounds), as rebuilding the
    // machine clears the script's.
    unit_health.clear();
    swing = Swing{};
    skill_playback = SkillPlayback{};
    scripted_player_animation.clear();
    player_animation.clear();
    player_health.current = player_health.max;
    player_health.dead = false;
    for (auto& entry : scene_units) {
        entry.second.playing.clear();
        entry.second.playing_state = nullptr;
        entry.second.last_time = 0.0f;
    }

    // Particle systems switched off or on by the script go back.
    for (ParticleSystem& system : particle_systems) restart_particle_system(system);

    // Units a path walked go back to where the layout placed them, and to the
    // way they were facing. Position alone leaves a townsperson standing in the
    // right spot pointing at the mine.
    for (const auto& entry : scripted_nodes) {
        const tl::formats::CmpNode* node = script_layout.node_by_id(entry.first);
        if (node == nullptr || node->type != tl::formats::cmp_type::kMonster) continue;
        namespace monster = tl::formats::cmp_field::monster;
        if (const tl::formats::CmpField* p = node->field(monster::kPosition)) {
            if (p->count() == 3) {
                entry.second->setPosition(p->as_float(0), p->as_float(1), p->as_float(2));
            }
        }
        entry.second->setOrientation(node_orientation(*node));
    }

    // And whatever the script left an animation doing. Rebuilding the machine
    // un-spends the controllers; this un-does what they already did, and then
    // puts each unit back on whatever the layout gave it at load.
    for (auto& entry : scene_units) {
        const SceneUnit& unit = entry.second;
        if (Ogre::AnimationStateSet* states = unit.entity->getAllAnimationStates()) {
            for (const auto& state : states->getAnimationStates()) {
                state.second->setEnabled(false);
                state.second->setTimePosition(0.0f);
            }
        }
        if (!unit.animation.empty()) {
            // Recorded, not merely played: a later Stop finds the state by its record.
            Ogre::AnimationState* state = nullptr;
            play_scene_animation(unit.entity, unit.mesh, unit.animation, unit.loops, unit.rate,
                                 /*optional=*/true, &state, unit.armed);
            remember_state(unit.entity, unit.animation, state);
            entry.second.playing = unit.animation;
            entry.second.playing_state = state;
        }
    }

    // The pathing blockers go back on: the script is what took them off, and
    // rebuilding the machine un-spends the wires that did it.
    lift_path_blockers();
    apply_path_blockers(script_layout, script_gated);

    // The trigger props' range starts again with the nodes shown; what it took
    // out of the collision goes back in.
    for (int64_t key : trigger_range_off) collision.set_group_enabled(key, true);
    trigger_range_off.clear();
    trigger_presence.clear();

    // Linked machines stop and un-hide too. They are rebuilt rather than
    // reset, for the same reason the level's own machine is: rebuilding is what
    // un-spends a wire.
    for (NestedScript& machine : nested_scripts) {
        machine.running = false;
        for (const auto& entry : machine.nodes) entry.second->setVisible(true);
    }

    // And everything a spawner released. Rebuilding the machine un-spends the
    // trigger that released them, so leaving them standing would double the
    // room's population on the next run.
    for (auto& entry : spawned) {
        for (const SpawnedUnit& unit : entry.second) {
            if (unit.node != nullptr) unit.node->setVisible(false);
        }
        entry.second.clear();
    }
    // The items a spawner released, and its particle layouts, go the same way:
    // the rebuilt spawners have released nothing yet.
    for (size_t i = dropped_items.size(); i-- > 0;) {
        if (dropped_items[i].unit_id == 0) continue;
        remove_ground_item(dropped_items[i]);
        dropped_items.erase(dropped_items.begin() + static_cast<long>(i));
        item_targeted = -1;
    }
    for (auto& entry : spawner_particles) {
        for (auto& layout : entry.second) destroy_particle_holders(layout.second);
    }
    spawner_particles.clear();
    // Every breakable is a spawner's, so none survives the rebuild either.
    for (BreakableWorld& b : breakables) remove_breakable(b);
    breakables.clear();
    player_skill_item = 0;
    merchants.clear();
    combiners.clear();
    combining_with = 0;
    merchant_checked.clear();
    spawner_units.clear();
    // The level's observer map goes with the level, and the skills' halves
    // with it.
    unit_observers.clear();
    skill_observers.clear();
    // The machine is rebuilt below, which drops the synthetic objects with it.

    script.set_equipment_roller(
        [this](const std::string& unit, tl::core::LevelScript::UnitCreation& made) {
            roll_unit_equipment(unit, made);
        });
    script.set_unit_equipment(unit_equipment_hooks());
    script.build(script_layout, script_gated);
    // As a level load does once its layout is built (`CGameClient::loadLevel`).
    //
    tl::core::reseed_level_generators();
    std::vector<tl::core::ScriptEvent> events;
    if (logic_trace != nullptr) std::fputs("# level load\n", logic_trace);
    script.level_activated(quest_log.state(), events);
    flush_logic_trace(script);
    apply_script_events(events, scripted_nodes);

    if (have_player_start && player_control) {
        player.place(tl::core::Vec3{player_start.x, player_start.y, player_start.z},
                     player.facing());
        subject_node->setPosition(to_ogre(player_start));
        follow_subject();
    }
    TL_LOG_INFO("level script reset: triggers unspent, camera released");
}

// CGameUI::setInteractiveMenuVisible(bool): a count,
// held at 0. At 1 the bars open and the automap goes; back at 0 they close and it returns
// (the minimap's half is step_hud_visibility's).
void Renderer::Impl::set_interactive_menu_visible(bool on) {
    interactive_menus += on ? 1 : -1;
    if (interactive_menus < 0) {
        interactive_menus = 0;
    } else if (interactive_menus == 1) {
        if (!hud.letterboxed()) hud.set_letterboxed(true);
    } else if (interactive_menus == 0 && hud.letterboxed()) {
        hud.set_letterboxed(false);
    }
}

// doActualGameState state 6: CGameUI::closeLeft,
// closeRight, CGameUI::closeMenus,
// closeAll; the script's
// lines in LevelScript::close_all_dialogs.
void Renderer::Impl::close_ingame_ui() {
    if (inventory_menu.visible()) close_master_menu();
    if (pause_menu.open) close_pause_menu();
    if (waypoint_screen.visible()) close_waypoint_menu();
    if (quest_dialog.visible()) close_quest_offer();
    if (modal_screen.visible()) modal_screen.hide();
    if (tip_open) {
        tip_screen.hide();
        tip_open = false;
    }
    TL_LOG_INFO("script: the in-game interface is closed");
}

// CGameUI::closeAll: closeLeft, closeRight, the console's screens, then
// hideModalDialogs. The waypoint menu (+0x3c0) stays open.
void Renderer::Impl::close_for_camera() {
    if (inventory_menu.visible()) close_master_menu();
    if (quest_dialog.visible()) close_quest_offer();
    if (modal_screen.visible()) modal_screen.hide();
    if (tip_open) {
        tip_screen.hide();
        tip_open = false;
    }
    if (pause_menu.open) close_pause_menu();
    if (settings_menu.open) {
        settings_menu.set_open(false, game_settings());
        settings_screen.hide();
    }
}

// `CCharacter::setMeshVisible(visible, true)`
// on the player: `+0x70d`, then `setVisible(visible, true)`,
// at once rather than through the fade - the
// model and what hangs on its bones.
void Renderer::Impl::set_player_mesh_visible(bool visible) {
    if (subject_node == nullptr) return;
    if (player_mesh_hidden == !visible) return;
    player_mesh_hidden = !visible;
    subject_node->setVisible(visible, true);
    TL_LOG_INFO("script: the player is %s", visible ? "shown" : "hidden");
}

}  // namespace tl::render
