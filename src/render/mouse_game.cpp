// The mouse layer: the frame, the PC's HUD and keys (the rest is in mouse_game.h's files).
#include "render/mouse_game.h"

namespace tl::render {

// --- The frame -------------------------------------------------------------------------------

/// The nearest living character called `name`, or with `?hostile` the nearest enemy.
bool Renderer::Impl::MouseGame::nearest_named_character(const std::string& name,
                                                        Ogre::Vector3& at) const {
    const Ogre::Vector3 me = g.subject_node->getPosition();
    bool have = false;
    float nearest = 0.0f;
    const auto consider = [&](const Ogre::Entity* entity, const std::string& unit, int64_t node) {
        if (entity == nullptr || node == 0) return;
        const Ogre::SceneNode* where = entity->getParentSceneNode();
        tl::core::LevelScript* owner = g.script_owning(node);
        if (where == nullptr || owner == nullptr || owner->unit_is_dead(node)) return;
        const tl::core::UnitStats* stats = g.unit_book.find(unit);
        const bool wanted = name == "?hostile" ? stats != nullptr && g.unit_hostile(node, *stats)
                                               : lowered(unit) == lowered(name);
        const Ogre::Vector3 p = where->_getDerivedPosition();
        const float d = p.squaredDistance(me);
        if (!wanted || (have && nearest <= d)) return;
        have = true;
        nearest = d;
        at = p;
    };
    for (const auto& entry : g.scene_units) {
        consider(entry.second.entity, entry.first, entry.second.node);
    }
    for (const auto& owner : g.spawned) {
        for (const SpawnedUnit& made : owner.second) consider(made.entity, made.unit, made.id);
    }
    // A character's middle rather than its feet.
    if (have) at.y += 0.6f;
    return have;
}

/// A floor item, a breakable or a prop called `name`; `?item` and `?breakable` are the
/// nearest of their kind.
bool Renderer::Impl::MouseGame::named_thing(const std::string& name, Ogre::Vector3& at) const {
    const Ogre::Vector3 me = g.subject_node->getPosition();
    bool have = false;
    float nearest = 0.0f;
    const auto take = [&](const Ogre::Vector3& p) {
        const float d = p.squaredDistance(me);
        if (have && nearest <= d) return;
        have = true;
        nearest = d;
        at = p;
    };
    for (const DroppedWorldItem& dropped : g.dropped_items) {
        if (dropped.flying) continue;
        if (name == "?item" || lowered(dropped.item.name) == lowered(name)) {
            take(dropped.node->getPosition());
        }
    }
    for (const BreakableWorld& b : g.breakables) {
        if (b.node == nullptr || !b.state.usable) continue;
        if (name == "?breakable" || lowered(b.unit) == lowered(name)) {
            take(b.node->_getDerivedPosition());
        }
    }
    const auto props = [&](const tl::core::LevelScript& owner) {
        for (const tl::core::LevelScript::UnitTriggerView& prop : owner.unit_triggers()) {
            if (lowered(prop.unit) == lowered(name)) {
                take(Ogre::Vector3(prop.at.x, prop.at.y, prop.at.z));
            }
        }
    };
    props(g.script);
    for (const NestedScript& nested : g.nested_scripts) props(nested.script);
    return have;
}

/// A script's `mouse_on=<unit>`: the pointer where that unit is drawn, or off the view a
/// few steps towards it on the ground, so a click walks that way.
void Renderer::Impl::MouseGame::scripted_pointer() {
    const std::string& follow = tl::core::scripted_pointer_unit();
    if (follow.empty() || g.camera == nullptr || g.subject_node == nullptr) return;
    Ogre::Vector3 at = Ogre::Vector3::ZERO;
    if (!nearest_named_character(follow, at) && !named_thing(follow, at)) {
        input.pointer = false;
        return;
    }
    const auto project = [&](const Ogre::Vector3& world, float& x, float& y) {
        const Ogre::Vector3 view = g.camera->getViewMatrix() * world;
        const Ogre::Vector3 clip = g.camera->getProjectionMatrix() * view;
        x = clip.x * 0.5f + 0.5f;
        y = -clip.y * 0.5f + 0.5f;
        return view.z < 0.0f && x >= 0.02f && x <= 0.98f && y >= 0.02f && y <= 0.98f;
    };
    float x = 0.5f;
    float y = 0.5f;
    if (!project(at, x, y)) {
        const Ogre::Vector3 me = g.subject_node->getPosition();
        Ogre::Vector3 step(at.x - me.x, 0.0f, at.z - me.z);
        step.normalise();
        project(me + step * 5.0f, x, y);
    }
    input.pointer = true;
    input.pointer_x = x;
    input.pointer_y = y;
}

/// The PC's HUD in place of the console's: made once a PC pak's `bottomhud.layout` is
/// found, shown while the game wants a HUD, fed the console HUD's state. Every frame,
/// whoever has the input.
void Renderer::Impl::MouseGame::sync_hud(float delta) {
    panes.sync(delta);
    if (!pc_hud_tried) {
        pc_hud_tried = true;
        if (pc_hud.create(kGameGroup, g.config.width, g.config.height)) {
            g.game_hud.set_layout_hidden(true);
        }
    }
    if (!pc_hud.ready()) return;
    if (hud_width != g.config.width || hud_height != g.config.height) {
        hud_width = g.config.width;
        hud_height = g.config.height;
        pc_hud.resize(hud_width, hud_height);
    }
    // A headless run shows its HUD only for the frame it dumps.
    const bool wanted = g.game_hud.wanted() || g.config.headless;
    pc_hud.set_visible(wanted && !g.hud.letterboxed() && !g.inventory_menu.visible());
    if (!pc_hud.visible()) return;
    // The right button's skill: its picture.
    std::string right_icon;
    if (const tl::core::KnownSkill* active = skill(right_skill());
        active != nullptr && active->def != nullptr) {
        right_icon = active->def->icon;
    }
    pc_hud.set_extras(g.player_character.unspent_stat_points() > 0,
                      g.player_character.unspent_skill_points() > 0, right_icon);
    pc_hud.set_state(g.game_hud.state());
}

/// The window of the HUD the pointer is over, if any.
void Renderer::Impl::MouseGame::step_hud() {
    over_hud.clear();
    hud_blocks = input.pointer && panes.point(input.pointer_x, input.pointer_y);
    if (pc_hud.ready() && pc_hud.visible() && input.pointer && panes.over.empty()) {
        over_hud = pc_hud.point(input.pointer_x, input.pointer_y);
        hud_blocks =
            hud_blocks || !over_hud.empty() || pc_hud.blocks(input.pointer_x, input.pointer_y);
    }
    // The foldout stands over the game: the pointer inside it is the interface's.
    if (input.pointer && foldout.holds(input.pointer_x, input.pointer_y)) {
        over_hud.clear();
        hud_blocks = true;
    }
    if (foldout.shown() && !pc_hud.visible()) foldout.hide();
    MousePanes::HudOver hud;
    hud.skill = hud_skill_over();
    hud.item_slot = hud_item_over();
    hud.art = &pc_hud.screen();
    panes.sync_tooltip(input.pointer, hud);
}

/// The bag slot of the item bound to the numbered slot of the HUD the pointer is on; -1 for
/// no such slot, no item on it, or none of it left in the bag.
int Renderer::Impl::MouseGame::hud_item_over() {
    if (over_hud.rfind("SkillSlot", 0) != 0) return -1;
    const long slot = std::strtol(over_hud.c_str() + 9, nullptr, 10) - 1;
    if (slot < 0 || slot >= tl::core::kMouseSlots) return -1;
    const std::int64_t guid = g.player_skills.second_slots()[static_cast<size_t>(slot)];
    return guid == -1 ? -1 : tl::core::equipment_of_guid(g.player_inventory, guid);
}

/// The skill of the HUD window the pointer is on - a numbered slot's, or the right
/// button's - or null.
const tl::core::KnownSkill* Renderer::Impl::MouseGame::hud_skill_over() {
    if (const int cell = foldout.point(input.pointer_x, input.pointer_y); cell >= 0) {
        return foldout_items[static_cast<size_t>(cell)]
                   ? nullptr
                   : skill(foldout_skills[static_cast<size_t>(cell)]);
    }
    if (over_hud == "PlayerSkillRight") return skill(right_skill());
    if (over_hud.rfind("SkillSlot", 0) != 0) return nullptr;
    const long slot = std::strtol(over_hud.c_str() + 9, nullptr, 10) - 1;
    if (slot < 0 || slot >= tl::core::kMouseSlots) return nullptr;
    return skill(slot_skill(static_cast<int>(slot)));
}

/// The pet panel's buttons: a stance, or the trip to town. False for any other function.
bool Renderer::Impl::MouseGame::click_pet_hud(const std::string& function) {
    if (function == "guiPetSell") {
        g.send_pet_to_town();
        g.update_menu();
        return true;
    }
    tl::core::PetStance stance = tl::core::PetStance::Aggressive;
    if (function == "guiPetDefensive") {
        stance = tl::core::PetStance::Defensive;
    } else if (function == "guiPetPassive") {
        stance = tl::core::PetStance::Passive;
    } else if (function != "guiPetAggressive") {
        return false;
    }
    if (!g.pet.alive()) return true;
    g.pet.set_stance(stance);
    g.apply_pet_stance();
    g.refresh_game_hud();
    return true;
}

// INVENTED: the PC HUD's buttons open the console's screens, until the PC's own
// (`CGameUI::onClick`) are carried.
void Renderer::Impl::MouseGame::click_hud(const std::string& function) {
    TL_LOG_INFO("mouse: the HUD's '%s' asks for %s", over_hud.c_str(),
                function.empty() ? "nothing" : function.c_str());
    if (function == "guiToggleOptions" || function == "guiPause") return g.open_pause_menu();
    if (function == "guiToggleAutoMap") return g.automap.cycle_mode();
    if (click_pet_hud(function)) return;
    // The PC's own pane, when its layout is mounted.
    if (panes.toggle_for(function)) return;
    MenuMode tab = MenuMode::Inventory;
    if (function == "guiToggleStats" || function == "guiStatsUp") {
        tab = MenuMode::Stats;
    } else if (function == "guiToggleSkills" || function == "guiSkillUp") {
        tab = MenuMode::Skills;
    } else if (function == "guiToggleQuests") {
        tab = MenuMode::Quest;
    } else if (function != "guiToggleInventory" && function != "guiTogglePet") {
        return;
    }
    if (g.inventory_menu.visible() && g.inventory_menu.menu_mode() == tab) {
        return g.close_master_menu();
    }
    if (!g.inventory_menu.visible()) {
        g.inventory_menu.toggle();
        g.play_ui(tl::core::InterfaceSound::MenuOpen);
    }
    g.inventory_menu.set_menu_mode(tab);
    g.update_menu();
    g.update_character_preview(0.0f);
}

// CGameUI::handle_SkillClick on one of the ten numbered slots. The right
// button uses the slot's item, or casts its skill when the skill needs no aim (target
// types 3, 10, 9, 7, 8 and 4); Ctrl with the left button empties the slot.
void Renderer::Impl::MouseGame::click_slot(int slot, bool right) {
    const auto at = static_cast<size_t>(slot);
    // With an item of the bag on the pointer: its GUID into the slot's item half, the
    // skill half cleared.
    if (panes.held.owner == 'p') {
        const tl::core::ItemStats& item =
            g.player_inventory.slots()[static_cast<size_t>(panes.held.slot)].item;
        std::vector<int64_t> skills(g.player_skills.slots().begin(), g.player_skills.slots().end());
        std::vector<int64_t> held(g.player_skills.second_slots().begin(),
                                  g.player_skills.second_slots().end());
        skills[at] = -1;
        held[at] = item.unit_guid;
        g.player_skills.set_slots(skills, held);
        g.refresh_game_hud();
        TL_LOG_INFO("mouse: '%s' onto slot %d", item.name.c_str(), slot + 1);
        return panes.release();
    }
    if (right) {
        if (g.player_skills.second_slots()[at] != -1) return g.activate_item_slot(slot);
        const std::int64_t guid = slot_skill(slot);
        if (guid == tl::core::kNoSkill) return;
        const int type = skill_target_type(guid);
        if (type != 3 && type != 10 && type != 9 && type != 7 && type != 8 && type != 4) return;
        tl::core::click_right(client, *this, guid, type != 7 && type != 8, false);
        return;
    }
    if (input.control) {
        std::vector<int64_t> skills(g.player_skills.slots().begin(), g.player_skills.slots().end());
        std::vector<int64_t> held(g.player_skills.second_slots().begin(),
                                  g.player_skills.second_slots().end());
        skills[at] = -1;
        held[at] = -1;
        g.player_skills.set_slots(skills, held);
        g.refresh_game_hud();
        TL_LOG_INFO("mouse: slot %d emptied", slot + 1);
        return;
    }
    // A plain left click: the slot's skill foldout (the PC also takes a dragged item).
    toggle_foldout(slot, over_hud);
}

// CInventoryMenu::toggleWeaponSet: alive and neither swinging nor casting,
// the weapon switch is flipped - the two hands' items change places with the second set's
// (CInventory::swapWeaponSet); otherwise sample 0x18.
// INVENTED: the sample on a swap (`WeaponSwap`): the switch's own handler was not read.
void Renderer::Impl::MouseGame::toggle_weapon_set() {
    if (!player_alive() || performing_attack_loose() || performing_skill_loose()) {
        g.play_ui(tl::core::InterfaceSound::Error);
        return;
    }
    g.player_inventory.swap_weapon_set();
    g.sync_equipped_weapon();
    g.update_menu();
    g.refresh_game_hud();
    g.play_ui(tl::core::InterfaceSound::ItemTake);
    TL_LOG_INFO("mouse: the weapon set is swapped");
}

/// The wheel and the middle button on the camera, while the game itself has them.
void Renderer::Impl::MouseGame::step_camera() {
    const bool middle = input.pointer && input.pointer_middle;
    const bool pressed = middle && !middle_was;
    middle_was = middle;
    const int wheel = panes.scroll(input.pointer_wheel) ? 0 : input.pointer_wheel;
    if (wheel == 0 && !pressed) return;
    if (!(g.camera_dolly > 0.0f) || g.hud.letterboxed()) return;
    g.camera_dolly = tl::core::wheel_dolly(g.camera_dolly, wheel, pressed, g.camera_mult);
}

// CGameUI::handleKeyPresses, the keys that open and close things, on
// the press: each runs the toggle its HUD button runs. The weapon set, the journal and
// the item names key have nothing to act on here.
void Renderer::Impl::MouseGame::step_pc_keys() {
    const std::uint32_t pressed = input.pc_keys & ~pc_keys_was;
    pc_keys_was = input.pc_keys;
    if (pressed == 0) return;
    namespace key = tl::core::pc_key;
    if ((pressed & key::kInventory) != 0) click_hud("guiToggleInventory");
    if ((pressed & key::kStats) != 0) click_hud("guiToggleStats");
    if ((pressed & key::kSkills) != 0) click_hud("guiToggleSkills");
    if ((pressed & key::kQuests) != 0) click_hud("guiToggleQuests");
    if ((pressed & key::kPet) != 0) click_hud("guiTogglePet");
    if ((pressed & key::kAutomap) != 0) click_hud("guiToggleAutoMap");
    if ((pressed & key::kWeaponSet) != 0) toggle_weapon_set();
    // `KEYMAP_CLOSEALL`: every pane down.
    if ((pressed & key::kCloseAll) != 0) panes.close_all();
    // `CCameraControl::processInput`: the zoom keys move the dolly 1 a press.
    const int zoom =
        ((pressed & key::kZoomIn) != 0 ? 100 : 0) - ((pressed & key::kZoomOut) != 0 ? 100 : 0);
    if (zoom != 0 && g.camera_dolly > 0.0f && !g.hud.letterboxed()) {
        g.camera_dolly = tl::core::wheel_dolly(g.camera_dolly, zoom, false, g.camera_mult);
    }
}

/// Over the HUD the click is the HUD's: a numbered slot's, or a button's `onClick`. True
/// while the pointer is over it, click or not.
bool Renderer::Impl::MouseGame::over_hud_takes(const tl::core::MouseButtons& buttons, bool taken) {
    if (!hud_blocks) return false;
    const bool pressed = (buttons.left_pressed || buttons.right_pressed) && !taken;
    if (!pressed) return true;
    if (panes.pointed()) {
        panes.click(buttons.right_pressed && !buttons.left_pressed, input.control,
                    input.hold_position);
        return true;
    }
    if (over_hud.empty()) return true;
    if (over_hud.rfind("SkillSlot", 0) == 0) {
        const long number = std::strtol(over_hud.c_str() + 9, nullptr, 10);
        click_slot(static_cast<int>(number) - 1, buttons.right_pressed);
    } else if (buttons.left_pressed && over_hud == "PlayerSkillRight") {
        toggle_foldout(kFoldoutRight, over_hud);
    } else if (buttons.left_pressed) {
        click_hud(pc_hud.on_click(over_hud));
    }
    return true;
}

/// The interface's share of a press: the foldout, the HUD and the panes, then an item on
/// the pointer put down outside them all - onto the ground. True when the press is not
/// the game's.
bool Renderer::Impl::MouseGame::interface_takes(const tl::core::MouseButtons& buttons, bool taken) {
    if (click_foldout(buttons)) taken = true;
    if (over_hud_takes(buttons, taken)) taken = true;
    const bool pressed = buttons.left_pressed || buttons.right_pressed;
    return taken || (pressed && panes.drop_held());
}

/// The in-game share of the frame under the mouse controls, in place of the pad's
/// selector, buttons and hotbar.
void Renderer::Impl::MouseGame::frame(float delta, const tl::core::InputState& in) {
    input = in;
    scripted_pointer();
    if (input.pointer && !pointer_seen) {
        pointer_seen = true;
        TL_LOG_INFO("mouse: the pointer is in the view, at (%.3f, %.3f)",
                    static_cast<double>(input.pointer_x), static_cast<double>(input.pointer_y));
    }
    step_hud();
    step_ai(delta);
    step_camera();
    step_pc_keys();
    tl::core::MouseButtons buttons;
    buttons.left = input.pointer && input.pointer_left;
    buttons.right = input.pointer && input.pointer_right;
    buttons.left_pressed = buttons.left && !left_was;
    buttons.right_pressed = buttons.right && !right_was;

    // A click moves a line of dialogue on, and is spent on it.
    bool taken = false;
    if (buttons.left_pressed && g.script_dialog_open()) {
        std::vector<tl::core::ScriptEvent> events;
        const int machine = g.dialog_machine();
        g.machine_script(machine)->dismiss_dialog(g.quest_log.state(), events);
        g.apply_machine_events(machine, events);
        taken = true;
    }
    taken = interface_takes(buttons, taken);
    const bool free_to_act = !g.script_dialog_open() && !g.script_player_disabled &&
                             !g.hud.letterboxed() && player_alive();
    if (buttons.left_pressed || buttons.right_pressed) {
        TL_LOG_INFO("mouse: %s press at (%.3f, %.3f)%s%s", buttons.left_pressed ? "left" : "right",
                    static_cast<double>(input.pointer_x), static_cast<double>(input.pointer_y),
                    taken ? ", taken by the interface" : "",
                    free_to_act ? "" : ", the player cannot act");
    }
    update_over(free_to_act && !taken);
    aim.reset();
    if (taken || !free_to_act) {
        // `processIngameInput` when the interface took the input: the buttons are
        // remembered as held, so a click on a window is not also a click on the level.
        tl::core::mouse_taken_by_interface(client, buttons);
    } else {
        tl::core::mouse_ingame_input(client, *this, buttons, delta);
    }
    left_was = buttons.left;
    right_was = buttons.right;
    keys_was = input.slot_keys;
    // The registry of items is let go once nothing holds a handle into it.
    if (client.over_item == 0 && client.click_item == 0 && target_item_handle == 0 &&
        items.size() > 64) {
        items.clear();
    }
}

// DIVERGENCE: the console's windows have no pointer. While one has the input a left
// click is its A and a right click its B, so they can be answered with the mouse - once
// both buttons have been let go since it opened (the click that walked to an NPC does not
// also accept what the NPC offers).
tl::core::InputState Renderer::Impl::mouse_as_pad(tl::core::InputState in) {
    if (!tl::core::mouse_controls()) return in;
    // A window can be up before the game first has the input (a tip as a floor opens).
    if (!mouse) mouse = std::make_shared<MouseGame>(*this);
    if (inventory_menu.visible()) return mouse->master_menu_pointer(in);
    const bool window = quest_dialog.visible() || modal_screen.visible() || tip_open ||
                        waypoint_screen.visible() || death_screen_up;
    const bool up = !in.pointer_left && !in.pointer_right;
    if (!window || !mouse->window_armed) {
        mouse->window_armed = window && up;
        return in;
    }
    in.confirm = in.confirm || in.pointer_left;
    in.skip = in.skip || in.pointer_right;
    return in;
}

// DIVERGENCE (mouse controls): the console's inventory screen under the pointer. A
// pointer that moves onto a tray's row selects it, the left button on a row is A, a press on
// a tray's filter icon chooses that tab, the right button is B, and the wheel steps the
// selection as the stick's up and down do.
tl::core::InputState Renderer::Impl::MouseGame::master_menu_pointer(tl::core::InputState in) {
    if (!in.pointer) return in;
    const bool moved = in.pointer_x != menu_x || in.pointer_y != menu_y;
    menu_x = in.pointer_x;
    menu_y = in.pointer_y;
    // A press on a tray's filter icon chooses that tab; it is not also an A.
    const bool press = in.pointer_left && !menu_left_was;
    menu_left_was = in.pointer_left;
    if (press && g.inventory_menu.click_filter(in.pointer_x, in.pointer_y)) {
        g.update_menu();
        TL_LOG_INFO("mouse: a tray's filter clicked; the bag is on tab %d",
                    g.inventory_menu.current_tab());
        return in;
    }
    const InventoryMenu::Pointed over =
        g.inventory_menu.point_row(in.pointer_x, in.pointer_y, moved || in.pointer_left);
    if (over == InventoryMenu::Pointed::Selected) g.update_menu();
    if (over != InventoryMenu::Pointed::Nothing) in.confirm = in.confirm || in.pointer_left;
    in.skip = in.skip || in.pointer_right;
    if (in.pointer_wheel != 0) in.move_y = in.pointer_wheel > 0 ? 1.0f : -1.0f;
    return in;
}

bool Renderer::Impl::mouse_opens_storage(VendorScreen screen) {
    const bool kept = screen == VendorScreen::Merchant || screen == VendorScreen::Stash ||
                      screen == VendorScreen::SharedStash;
    return kept && tl::core::mouse_controls() && mouse && mouse->panes.open_storage(screen);
}

bool Renderer::Impl::mouse_storage_open() const {
    return mouse && mouse->panes.storage_open != VendorScreen::None;
}

void Renderer::Impl::sync_mouse_hud(float delta) {
    if (mouse) mouse->sync_hud(delta);
}

void Renderer::Impl::drive_player_mouse(float delta, const tl::core::InputState& in) {
    if (!mouse) {
        mouse = std::make_shared<MouseGame>(*this);
        mouse->sync_hud(0.0f);
    }
    mouse->frame(delta, in);
    confirm_held = in.confirm;
    attack_held = false;
    finish_player_frame(delta, in, in);
}

/// A headless run's scene advanced with a script's pointer driving the player as the run
/// loop would, so the mouse controls can be verified with no window. False when the
/// mouse controls are off or no script is running.
bool Renderer::Impl::play_scripted_mouse(float seconds) {
    if (!tl::core::mouse_controls() || !tl::core::pad_script_running()) return false;
    if (!player_control || subject_node == nullptr) return false;
    constexpr float kStep = 1.0f / 60.0f;
    const int steps = static_cast<int>(seconds / kStep);
    TL_LOG_INFO("mouse: playing the script for %.2fs at 60 Hz", static_cast<double>(seconds));
    for (int i = 0; i < steps; ++i) {
        drive_player(kStep);
        // Not over a swing's or a cast's own animation.
        if (!swing.active && player_active_skill.empty()) follow_player_animation();
        advance(kStep);
        if (!root->renderOneFrame(kStep)) break;
    }
    return true;
}

}  // namespace tl::render
