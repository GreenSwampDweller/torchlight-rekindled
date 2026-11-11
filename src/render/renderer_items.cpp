// The renderer's implementation: crafting, sockets, player death, particle triggers,
// dropped items, themes.
#include "core/equipment_attach.h"
#include "core/item_ai.h"
#include "core/targeting.h"
#include "render/renderer_impl.h"
#include "render/unit_opacity.h"

namespace tl::render {

void Renderer::Impl::gem_service_selected() {
    const VendorScreen page = inventory_menu.vendor();
    const bool recover = page == VendorScreen::RecoverGems;
    if (!recover && page != VendorScreen::DestroyGems) return;
    // the focused tray's selected item.
    const int slot = inventory_menu.focused_tray() == InventoryMenu::Tray::Bag
                         ? inventory_menu.selected_slot_index()
                         : -1;
    tl::core::ItemStats* item =
        slot >= 0 ? player_inventory.item_in_slot(static_cast<size_t>(slot)) : nullptr;
    load_unit_types();
    // without an item or a gem in it, the result window is
    // emptied and `ERROR` plays ((0x18, player node)`).
    if (item == nullptr || !tl::core::item_has_gems(*item)) {
        inventory_menu.set_gem_result("");
        play_ui(tl::core::InterfaceSound::Error);
        TL_LOG_INFO("%s: nothing to do", recover ? "recover gems" : "destroy gems");
        update_menu();
        return;
    }
    const std::string host = item->name;
    const size_t gems = item->socketed.size();
    std::string line;
    if (recover) {
        // `REVEAL`, each gem to the bag (`pickupEquipment`, forced)
        // or the floor, then the item removed (`removeEquipment`) and deleted.
        play_ui(tl::core::InterfaceSound::Reveal);
        const tl::core::ItemRollContext context = item_roll_context();
        std::vector<tl::core::ItemStats> out = tl::core::take_out_socketed_gems(*item, context);
        for (const tl::core::ItemStats& gem : out) {
            if (player_inventory.pickup(gem, 1, unit_types) < 0) {
                drop_item_near_player(gem, 1);
                TL_LOG_INFO("recover gems: '%s' does not fit, dropped", gem.name.c_str());
            }
        }
        player_inventory.remove_item(static_cast<size_t>(slot),
                                     player_inventory.slots()[static_cast<size_t>(slot)].count);
        line = tl::core::gems_recovered_line();
    } else {
        // the line, `REVEAL`, the gems deleted.
        const std::string name = tl::core::item_name(*item);
        line = tl::core::destroy_socketed_gems(*item, name);
        play_ui(tl::core::InterfaceSound::Reveal);
    }
    // Both: the equipment cache recalculated, the
    // line in the result window, the tray's refresh.
    player_inventory.calculate_effect_values();
    inventory_menu.set_gem_result(line);
    TL_LOG_INFO("%s: '%s', %zu gem(s); \"%s\"", recover ? "recover gems" : "destroy gems",
                host.c_str(), gems, line.c_str());
    sync_equipped_weapon();
    update_menu();
    update_character_preview(0.0f);
    refresh_game_hud();
}

void Renderer::Impl::retire_selected() {
    if (inventory_menu.vendor() != VendorScreen::Retire) return;
    // a Weapon, an Armor or a Trinket goes on, anything
    // else does nothing.
    const int slot = inventory_menu.focused_tray() == InventoryMenu::Tray::Bag
                         ? inventory_menu.selected_slot_index()
                         : -1;
    tl::core::ItemStats* item =
        slot >= 0 ? player_inventory.item_in_slot(static_cast<size_t>(slot)) : nullptr;
    load_unit_types();
    if (item == nullptr || !tl::core::can_become_heirloom(*item, unit_types)) return;

    // mode 4: the target cleared, every menu closed,
    // the item out of the bag (`removeEquipment`).
    tl::core::ItemStats heirloom = *item;
    clear_talk_target();
    player_inventory.remove_item(static_cast<size_t>(slot),
                                 player_inventory.slots()[static_cast<size_t>(slot)].count);
    player_inventory.calculate_effect_values();
    sync_equipped_weapon();
    inventory_menu.hide();
    sync_stats_page();

    // The item: renamed the first time, one heirloom level up, improved.
    const tl::core::ItemRollContext rolls = item_roll_context();
    tl::core::EffectManager::Context effects;
    effects.types = &effect_types;
    effects.graphs = &stat_graphs;
    effects.owner.present = false;
    effects.theme_book = &unit_themes;
    const std::string was = tl::core::item_name(heirloom);
    tl::core::make_heirloom(heirloom, player_character.name(), rolls, effects);

    // Into the client (`+0x1458`) with the generation (`+0x145c`), the player retired
    // (`+0x9a4`); then (not read), the menu's close, and `setGameState(0, 2)`:
    // leaving saves, New Game opens in retirement mode.
    // DIVERGENCE: the console's: the PC (`CEnchantMenu::update`) waits for the
    // NPC's animation; `CPlayer::update`'s fade has no frame to show in.
    heirloom_handoff.generation = player_character.generation() + 1;
    TL_LOG_INFO("retire: '%s' becomes '%s' (heirloom level %d), generation %d next", was.c_str(),
                tl::core::item_full_name(heirloom, true, unit_types).c_str(),
                heirloom.heirloom_level, heirloom_handoff.generation);
    heirloom_handoff.item = std::move(heirloom);
    player_character.set_retired(true);
    retirement_requested = true;
    quit_requested = true;
    refresh_game_hud();
}

bool Renderer::Impl::open_gem_socketing() {
    // a first pet in the player's list (`+0x5ac`, the port's one record), the
    // gem in the player's or that pet's inventory, then the dialog (`CGameUI +0x1894`).
    const int slot = inventory_menu.selected_slot_index();
    if (slot < 0 || !player_inventory.occupied(static_cast<size_t>(slot)) || !pet.alive()) {
        return false;
    }
    if (!inventory_menu.open_item_select()) {
        TL_LOG_WARN(
            "item select: no '%s' in the console data; re-run "
            "scripts/extract_console_ui.py",
            "media/UI/messagehousing_xenon.layout");
        return false;
    }
    item_select_stick_y_held = true;
    item_select_stick_x_held = true;
    TL_LOG_INFO("item select: open for '%s'",
                player_inventory.slots()[static_cast<size_t>(slot)].item.name.c_str());
    update_menu();
    return true;
}

void Renderer::Impl::handle_item_select(const tl::core::InputState& in) {
    // `CMessageItemSelectXenon::vfunc_2`, by the pad's code.
    if (std::abs(in.move_y) > 0.5f) {
        if (!item_select_stick_y_held) {
            // 0x11 / 0x12: (-1 / +1), then the tray's refresh.
            inventory_menu.item_select_step(in.move_y > 0.5f ? -1 : 1);
            update_menu();
        }
        item_select_stick_y_held = true;
    } else {
        item_select_stick_y_held = false;
    }
    if (std::abs(in.move_x) > 0.5f) {
        if (!item_select_stick_x_held) {
            // 0x13: the tab left and `NAVIGATE` (0x43); 0x14:.
            inventory_menu.item_select_step_tab(in.move_x > 0.0f ? 1 : -1);
            play_ui(tl::core::InterfaceSound::Navigate);
            update_menu();
        }
        item_select_stick_x_held = true;
    } else {
        item_select_stick_x_held = false;
    }
    if (in.confirm && !confirm_held) {
        // Code 5: the choice, then the default case's `vfunc_3(0)`.
        inventory_menu.confirm_item_select();
        socket_selected_gem();
    } else if (in.skip && !skip_held) {
        // Any other code closes it through the default case, the choice left empty.
        inventory_menu.close_item_select();
        socket_selected_gem();
    }
}

void Renderer::Impl::refresh_pet_commands() {
    // `vfunc_5`: nothing without a pet (`+0x2c`).
    if (!pet.alive()) return;
    const bool can_depart = tl::core::pet_can_depart(pet_departure_level(), pet.state());
    inventory_menu.set_pet_commands_view(tl::core::pet_commands_view(pet.stance(), can_depart));
}

bool Renderer::Impl::open_pet_commands() {
    // code 0xd hands the dialog the player's first pet (`--pet none`: none).
    if (!pet.alive()) return false;
    refresh_pet_commands();
    if (!inventory_menu.open_pet_commands()) return false;
    TL_LOG_INFO("pet commands: open");
    update_menu();
    return true;
}

void Renderer::Impl::handle_pet_commands(const tl::core::InputState& in,
                                         const PetCommandEdges& edges) {
    // `CMessagePetCommandsXenon::vfunc_2`, by the pad's code; left and
    // right (0x13, 0x14) once per push; up, down, A and X are ignored.
    int code = -1;
    if (std::abs(in.move_x) > 0.5f) {
        if (!pet_commands_stick_x_held) code = in.move_x > 0.0f ? 0x14 : 0x13;
        pet_commands_stick_x_held = true;
    } else {
        pet_commands_stick_x_held = false;
    }
    if (code < 0) {
        if (edges.y) {
            code = 8;
        } else if (in.skip && !skip_held) {
            code = 6;
        } else if (edges.start) {
            code = 0xd;
        } else if (edges.back) {
            code = 0xe;
        } else if (edges.other) {
            code = 9;  // RT; the shoulders, LT and the D-pad fall to the same default
        }
    }
    if (code < 0) return;

    switch (tl::core::pet_command_for_code(code)) {
        case tl::core::PetCommand::Ignore: return;
        case tl::core::PetCommand::StanceLeft:
        case tl::core::PetCommand::StanceRight: {
            const int direction =
                tl::core::pet_command_for_code(code) == tl::core::PetCommand::StanceLeft ? -1 : 1;
            pet.set_stance(tl::core::step_pet_stance(pet.stance(), direction));
            // `CCharacter::setTarget(pet, nullptr)`.
            apply_pet_stance();
            const tl::core::PetStance stance = pet.stance();
            TL_LOG_INFO("pet commands: %s", stance == tl::core::PetStance::Aggressive ? "aggressive"
                                            : stance == tl::core::PetStance::Defensive ? "defensive"
                                                                                       : "passive");
            refresh_pet_commands();
            refresh_game_hud();
            update_menu();
            return;
        }
        case tl::core::PetCommand::SendToTown:
            // first; the voice (`VOICEPETDEPARTED`) and `sendToTown`
            // only when it says yes. Then the dialog closes either way.
            if (tl::core::pet_can_depart(pet_departure_level(), pet.state())) {
                send_pet_to_town();
            }
            break;
        case tl::core::PetCommand::Close: break;
    }
    inventory_menu.close_pet_commands();
    TL_LOG_INFO("pet commands: closed");
    update_menu();
}

void Renderer::Impl::drop_selected_pet_item() {
    // on the pet's tray: not a quest item (ISA 0x67);
    // `removeEquipment`, `CLevel::addItem` at the owner's position, `drop`.
    if (!pet.alive() || !pet.in_world()) return;
    const int slot = inventory_menu.selected_pet_slot();
    if (slot < 0 || !pet.bag().occupied(static_cast<size_t>(slot))) return;
    load_unit_types();
    const tl::core::InventorySlot held = pet.bag().slots()[static_cast<size_t>(slot)];
    if (unit_types.isa(held.item.unittype, tl::core::unit_type::kQuestItem)) {
        // `Error` at the player, `VOICEIMPOSSIBLE` 0.1 s on.
        play_ui(tl::core::InterfaceSound::Error);
        play_voice(tl::core::SoundSlot::VoiceImpossible, 0.1f);
        return;
    }
    // DIVERGENCE: a worn item stays, as on the bag's tray (see swap_bag_and_pet).
    if (tl::core::Inventory::is_equip_location(static_cast<size_t>(slot))) return;
    pet.bag().remove_item(static_cast<size_t>(slot), held.count);
    const tl::core::Vec3& at = pet.position();
    drop_item(held.item, held.count, Ogre::Vector3(at.x, at.y, at.z));
    TL_LOG_INFO("pet tray: dropped %d x %s", held.count, held.item.name.c_str());
    update_menu();
}

void Renderer::Impl::socket_selected_gem() {
    // the bag's poll once the dialog is down:.
    const int gem_slot = inventory_menu.selected_slot_index();
    const int host_slot = inventory_menu.item_select_choice();
    tl::core::ItemStats* host =
        host_slot >= 0 ? player_inventory.item_in_slot(static_cast<size_t>(host_slot)) : nullptr;
    const bool have_gem = gem_slot >= 0 && player_inventory.occupied(static_cast<size_t>(gem_slot));
    if (host == nullptr || !have_gem || gem_slot == host_slot) {
        // no character, gem or item -> `ERROR` at the player.
        play_ui(tl::core::InterfaceSound::Error);
        TL_LOG_INFO("item select: closed with nothing chosen");
        update_menu();
        return;
    }
    load_unit_types();
    // `removeEquipment` takes the gem out (gems do not stack),
    // `addContainerItem` sets it in the item, and `createElementalDamages`
    // folds the item's damage effects into its bonus.
    const tl::core::ItemStats gem = player_inventory.slots()[static_cast<size_t>(gem_slot)].item;
    const int count = player_inventory.slots()[static_cast<size_t>(gem_slot)].count;
    player_inventory.remove_item(static_cast<size_t>(gem_slot), count);
    const bool set = tl::core::add_container_item(*host, gem, unit_types);
    tl::core::create_elemental_damages(*host, item_effect_context(), unit_types);
    // The equipment cache recalculated; then sample
    // 0x11 (`LAND_SOUND`) of the item's bank at the player.
    player_inventory.calculate_effect_values();
    if (subject_node != nullptr) play_sound(host->land_sound, subject_node->getPosition());
    TL_LOG_INFO("item select: '%s' set in '%s'%s (%zu of %d socket(s))", gem.name.c_str(),
                host->name.c_str(), set ? "" : " - no free socket, the gem is lost",
                host->socketed.size(), host->sockets);
    // The tray's tab follows the item and the selection lands on it.
    inventory_menu.select_slot(static_cast<size_t>(host_slot));
    sync_equipped_weapon();
    update_menu();
    update_character_preview(0.0f);
    refresh_game_hud();
}

bool Renderer::Impl::enchant_selected() {
    // A on an enchanter's or a shrine's tray (modes 0 and 1).
    const bool shrine = inventory_menu.vendor() == VendorScreen::EnchantShrine;
    if ((inventory_menu.vendor() != VendorScreen::Enchant && !shrine) ||
        inventory_menu.focused_tray() != InventoryMenu::Tray::Bag) {
        return false;
    }
    const int slot = inventory_menu.selected_slot_index();
    tl::core::ItemStats* item =
        slot >= 0 ? player_inventory.item_in_slot(static_cast<size_t>(slot)) : nullptr;
    if (item == nullptr) return false;
    const tl::core::ItemRollContext context = item_roll_context();
    // `canEnchant` refuses: an unidentified item goes to the tray's A,
    // any other does nothing.
    if (!tl::core::item_can_enchant(*item, unit_types)) return !item->identified;
    // `CEnchantMenuXenon +0xf4`: a spent shrine does nothing (`vfunc_6`).
    if (shrine && enchant_shrine_spent()) return false;
    // With an NPC, too little gold is `VOICEINSUFFICIENTGOLD` after 0.1 s and nothing else.
    if (!shrine && player_inventory.gold() <
                       tl::core::item_enchant_price(*item, stat_graphs, item_roll_globals)) {
        play_voice(tl::core::SoundSlot::VoiceInsufficientGold, 0.1f);
        TL_LOG_INFO("enchanter: '%s' costs %d, the player has %d", item->name.c_str(),
                    tl::core::item_enchant_price(*item, stat_graphs, item_roll_globals),
                    player_inventory.gold());
        return false;
    }

    // > `CEnchantMenu::performInteraction` with an NPC, the
    // shrine's without one.
    const std::string name = tl::core::item_name(*item);
    int gold = player_inventory.gold();
    const tl::core::EnchantOutcome out = tl::core::perform_enchant(
        shrine ? tl::core::EnchantService::Shrine : tl::core::EnchantService::Enchanter, *item,
        gold, static_cast<uint32_t>(player_character.level()), name, context);
    player_inventory.set_gold(gold);
    // `GOLDBUY` is the enchanter's alone: the shrine takes no gold.
    switch (out.result) {
        case tl::core::EnchantResult::Refused: play_ui(tl::core::InterfaceSound::Error); break;
        case tl::core::EnchantResult::Sockets:
        case tl::core::EnchantResult::Enchanted:
            if (!shrine) play_ui(tl::core::InterfaceSound::GoldBuy);
            play_ui(tl::core::InterfaceSound::Reveal);
            play_voice(tl::core::SoundSlot::VoiceFortuneGood, 1.0f);
            break;
        case tl::core::EnchantResult::NothingHappens:
        case tl::core::EnchantResult::Disenchanted:
            if (!shrine) play_ui(tl::core::InterfaceSound::GoldBuy);
            play_voice(tl::core::SoundSlot::VoiceFortuneBad, 1.0f);
            play_ui(tl::core::InterfaceSound::Disenchant);
            break;
    }
    if (out.served()) {
        // The tail: the item's text rebuilt, the equipment cache recalculated.
        player_inventory.calculate_effect_values();
        if (shrine) {
            // (PC case 0x16): the shrine's `interact(player)`,
            // `CTriggerUnit::interact` -> `trigger`;
            // then clears the player's target, which the port does not keep.
            if (tl::core::LevelScript* owner = machine_script(enchant_shrine_machine)) {
                std::vector<tl::core::ScriptEvent> events;
                owner->interact_unit_trigger(enchant_shrine_node, quest_log.state(), events);
                if (enchant_shrine_machine >= 0) {
                    nested_scripts[static_cast<size_t>(enchant_shrine_machine)].running = true;
                }
                apply_machine_events(enchant_shrine_machine, events);
                TL_LOG_INFO("enchant shrine: unit trigger %lld used, %zu event(s)%s",
                            static_cast<long long>(enchant_shrine_node), events.size(),
                            enchant_shrine_spent() ? ", spent" : "");
            }
        }
        // CEnchantMenu::performInteraction: incrementJournalStatistic(17, 1)
        count_journal(tl::core::JournalStatistic::ItemsEnchanted);
    }
    inventory_menu.set_enchant_result(out.message, out.succeeded());
    inventory_menu.set_enchant_allowed(!shrine || !enchant_shrine_spent());
    TL_LOG_INFO("%s: '%s' %s for %d gold (enchants %u, sockets %d, affixes %zu); %d left",
                shrine ? "enchant shrine" : "enchanter", item->name.c_str(),
                out.result == tl::core::EnchantResult::Refused          ? "refused"
                : out.result == tl::core::EnchantResult::NothingHappens ? "nothing happens"
                : out.result == tl::core::EnchantResult::Sockets        ? "new sockets"
                : out.result == tl::core::EnchantResult::Enchanted      ? "new enchantments"
                                                                        : "disenchanted",
                out.price, item->enchant_count, item->sockets, item->affixes.size(),
                player_inventory.gold());
    sync_equipped_weapon();
    update_menu();
    update_character_preview(0.0f);
    refresh_game_hud();
    return false;
}

// PORT: a vendor screen's inventory, with the item events wired.
tl::core::Inventory* Renderer::Impl::housing_inventory_for(VendorScreen screen) {
    tl::core::Inventory* housing = housing_inventory_of(screen);
    // An item taken out of it is told so (`bag_item_removed`).
    if (housing != nullptr) housing->set_item_events(&housing_item_events);
    return housing;
}

tl::core::Inventory* Renderer::Impl::housing_inventory_of(VendorScreen screen) {
    switch (screen) {
        case VendorScreen::Combine: {
            const auto found = combiners.find(combining_with);
            return found != combiners.end() ? &found->second.inventory : nullptr;
        }
        // page 7's stash is the player's `+0x9f4`, page 8's `CSharedStash`.
        case VendorScreen::Stash: return &player_stash;
        case VendorScreen::SharedStash:
            return config.shared_stash != nullptr ? &config.shared_stash->inventory() : nullptr;
        default: return nullptr;
    }
}

// PORT: where the camera's subject is, for the items' active range.
ogre::Vector3 Renderer::Impl::item_view_point() const {
    if (scripted_camera.active) {
        return from_ogre(scripted_camera.have_follow_position ? scripted_camera.follow_position
                                                              : scripted_camera.position);
    }
    return from_ogre(subject_node != nullptr ? subject_node->getPosition() : Ogre::Vector3::ZERO);
}

// PORT: whether an item's OGRE node is in the camera's view.
bool Renderer::Impl::item_in_camera(Ogre::SceneNode* node, bool from_model) const {
    if (camera == nullptr || node == nullptr) return true;
    const Ogre::AxisAlignedBox box = culling_box(node, from_model);
    return box.isNull() || camera->isVisible(box);
}

void Renderer::Impl::draw_item_presence(Ogre::SceneNode* node, const tl::core::ItemPresence& p,
                                        const tl::core::ItemPresenceChange& change) {
    if (node == nullptr) return;
    // `CSceneNodeObject::setVisible`, `CGenericModel::setOpacity`
    // and `setCastsShadows`.
    if (change.visibility_changed) node->setVisible(p.visible);
    if (change.opacity_written) tl::render::set_node_opacity(node, p.opacity);
    if (change.shadow_set) tl::render::set_node_casts_shadows(node, change.casts_shadow);
}

// PORT: drawing, a `Unit Trigger`'s own layout restarted where it had stopped.
void Renderer::Impl::view_restart_trigger_layout(int64_t node, const NodeMap* nodes) {
    for (ParticleSystem& system : particle_systems) {
        if (system.trigger_own_layout && system.owner == node && system.owner_nodes == nodes &&
            (system.pu_dying || system.finished)) {
            restart_particle_system(system);
        }
    }
}

// PORT: drawing, an entity put in the scene on a node of its own.
Ogre::SceneNode* Renderer::Impl::view_place_entity(Ogre::Entity* entity, const Ogre::Vector3& at,
                                                   const Ogre::Quaternion& turn) const {
    Ogre::SceneNode* node = scene->getRootSceneNode()->createChildSceneNode(at, turn);
    node->attachObject(entity);
    return node;
}

// PORT: drawing, a node scaled evenly.
void Renderer::Impl::view_set_scale(Ogre::SceneNode* node, float scale) {
    node->setScale(scale, scale, scale);
}

// PORT: drawing, nodes shown or hidden.
void Renderer::Impl::view_set_visible(const std::vector<Ogre::SceneNode*>& holders, bool on) {
    for (Ogre::SceneNode* holder : holders) holder->setVisible(on);
}

// PORT: drawing, an entity gone from the scene.
void Renderer::Impl::view_destroy_entity(Ogre::Entity* entity) { scene->destroyEntity(entity); }

// PORT: drawing, the size of a laid-down item's model in the world, for the log.
ogre::Vector3 Renderer::Impl::view_landed_size(Ogre::SceneNode* node, Ogre::Entity* entity) {
    node->_update(true, false);
    return from_ogre(entity->getWorldBoundingBox(true).getSize());
}

// PORT: drawing, the name of the mesh an entity draws, empty for none.
std::string Renderer::Impl::view_entity_mesh(const Ogre::Entity* entity) const {
    if (entity == nullptr || !entity->getMesh()) return std::string();
    return entity->getMesh()->getName();
}

// PORT: the death screen taken down.
void Renderer::Impl::view_hide_death_screen() { death_screen.hide(); }

// PORT: the minimap put back.
void Renderer::Impl::view_show_minimap() { automap.show(); }

int Renderer::Impl::breakable_within_view(float max_angle, float max_distance) const {
    if (subject_node == nullptr) return -1;
    // `CLevel::findItemWithinView`, newest first, from
    // the attacker's frame.
    tl::core::ViewFrame frame;
    const Ogre::Vector3 me = subject_node->getPosition();
    const Ogre::Quaternion turn = subject_node->getOrientation();
    frame.position = tl::core::Vec3{me.x, me.y, me.z};
    frame.orientation = tl::core::OgreQuaternion{turn.w, turn.x, turn.y, turn.z};
    for (size_t i = breakables.size(); i-- > 0;) {
        const BreakableWorld& b = breakables[i];
        if (b.node == nullptr) continue;
        if (!tl::core::breakable_candidate(b.state, b.entity != nullptr && b.presence.visible)) {
            continue;
        }
        const Ogre::Vector3 p = b.node->getPosition();
        const tl::core::Vec3 at{p.x, p.y, p.z};
        if (!tl::core::item_within_view(frame, at, max_angle, max_distance)) continue;
        // `sightUnobstructed(from, to, false)`, both ends 5 up, the objects tested too.
        tl::core::CollisionHit hit;
        if (collision.ray_collision({me.x, me.y + 5.0f, me.z}, {p.x, p.y + 5.0f, p.z},
                                    /*objects=*/true, hit)) {
            continue;
        }
        return static_cast<int>(i);
    }
    return -1;
}

// CPlayer::updateAnimation and CPlayer::update.
void Renderer::Impl::step_player_death(float delta) {
    if (!player_control || !player_death.dying) return;
    bool falling = false;
    if (!player_death.animation.empty() && subject != nullptr) {
        const std::string name = animation_named(subject, player_death.animation);
        if (!name.empty()) {
            Ogre::AnimationState* state = subject->getAnimationState(name);
            if (state->getEnabled() && !state->hasEnded()) {
                player_death.timer = tl::core::kDeathScreenDelay;
                falling = true;
            }
        }
    }
    // `CCharacter::completeDeath`: every weapon trail
    // off, and no outline.
    if (!falling) {
        player_trails.trails.hide_all();
        if (const RenderBehind* behind = render_behind_of(subject);
            behind != nullptr && behind->on) {
            set_subject_render_behind(false, false);
        }
    }
    player_death.timer -= delta;
    if (!(player_death.timer <= 0.0f)) return;
    open_death_screen();
    player_death.timer = tl::core::kDeathTimerParked;
}

// CGameUI::toggleDeath: unpause, close every menu, open `CDieMenu`.
void Renderer::Impl::open_death_screen() {
    if (death_screen_up) return;
    if (inventory_menu.visible()) inventory_menu.hide();
    sync_stats_page();
    if (quest_dialog.visible()) close_quest_offer();
    // CDieMenu::setOpen takes the automap down.
    if (automap_initialized) automap.hide();
    death_screen_up = true;
    death_selected = 0;
    refresh_death_screen();
    if (death_screen.ready()) {
        death_screen.show();
    } else {
        TL_LOG_WARN("death screen: no console interface data mounted; the pad still chooses");
    }
    const tl::core::DeathPenalty f =
        tl::core::death_screen_figures(stat_graphs, player_character.level(),
                                       player_character.fame_level(), player_inventory.gold());
    TL_LOG_INFO("death screen%s: here -%d xp -%d fame, entrance -%d gold, town free",
                config.hardcore ? " (hardcore)" : "", f.experience, f.fame, f.gold);
}

// PORT: the death screen's text (`death_screen_figures`, `resurrect_*`) handed to it.
void Renderer::Impl::refresh_death_screen() {
    const tl::core::DeathPenalty f =
        tl::core::death_screen_figures(stat_graphs, player_character.level(),
                                       player_character.fame_level(), player_inventory.gold());
    DeathScreenState state;
    state.title = tl::core::death_screen_title(config.hardcore);
    state.choices[0] = tl::core::resurrect_here_text(f);
    state.choices[1] = tl::core::resurrect_entrance_text(f);
    state.choices[2] = tl::core::resurrect_town_text();
    state.hardcore = config.hardcore;
    state.selected = death_selected;
    death_screen.set_state(state);
}

/// Land every hunting unit's swing on its manifest's `HIT` frame, against the `Hunt`
/// target (either side: the player or any unit). The blow is spotted by the animation's
/// time passing the frame, one hit per loop, so `ATTACKSPEED` sets the rate.
void Renderer::Impl::step_monster_swings(float delta) {
    (void)delta;
    // `CCharacter::updateAttack` runs on each `HIT` event; the unit's machine
    // decides what the blow does.
    std::vector<tl::core::ScriptEvent> events;
    for (auto& entry : scene_units) {
        SceneUnit& unit = entry.second;
        if (unit.playing_state == nullptr || !unit.playing_state->getEnabled()) continue;
        if (unit.node == 0 || script.unit_is_dead(unit.node)) continue;

        const float hit_at = manifest_for(unit.mesh).seconds_of(unit.playing, "HIT", -1.0f);
        const float now = unit.playing_state->getTimePosition();
        const float was = unit.last_time;
        unit.last_time = now;
        if (hit_at < 0.0f) continue;
        // Wrapped round the loop, or stepped past the frame this frame.
        const bool passed =
            now < was ? (was < hit_at || now >= hit_at) : (was < hit_at && now >= hit_at);
        if (!passed) continue;
        if (tl::core::LevelScript* owner = script_owning(unit.node)) {
            owner->attack_hit(unit.node, quest_log.state(), events);
            if (owner == &script) {
                if (!events.empty()) apply_script_events(events, scripted_nodes);
            } else {
                for (size_t i = 0; i < nested_scripts.size(); ++i) {
                    // By index: the events can link a layout, a push_back that
                    // invalidates a deque's iterators.
                    NestedScript& machine = nested_scripts[i];
                    if (&machine.script != owner || events.empty()) continue;
                    apply_script_events(events, machine.nodes);
                }
            }
            events.clear();
        }
    }
}

/// Take the subject apart: the body, everything worn on it, its weapon and the particles
/// it carries (the character creation screen swaps bodies without a restart).
void Renderer::Impl::clear_subject() {
    // First: `fidgets` holds `Ogre::Entity*` keys and `Ogre::AnimationState*` values that
    // are about to be freed.
    fidgets.clear();
    // The level-up particle rides the player's node, like the unit it was made for.
    clear_level_up_particle();

    // The trails let go of the weapon's tag point and the skeleton's bones.
    player_trails.trails.hide_all();
    clear_worn_attachments();
    if (weapon_entity != nullptr && subject != nullptr) {
        subject->detachObjectFromBone(weapon_entity);
        scene->destroyEntity(weapon_entity);
    }
    weapon_entity = nullptr;
    has_weapon = false;
    clear_weapon_element();
    destroy_fishing_gear();
    fishing_state = tl::core::FishingState::Off;
    for (Ogre::SceneNode*& hand : hand_nodes) {
        if (hand != nullptr) scene->destroySceneNode(hand);
        hand = nullptr;
    }

    // The theme particles hang off the subject's node and bones.
    clear_unit_themes(player_themes);

    // The animations' particles and the model's own (equip_carried_particles),
    // with the nodes that follow their bones.
    clear_particle_triggers();

    for (Ogre::Entity* piece : equipment) {
        if (piece == nullptr) continue;
        if (Ogre::SceneNode* node = piece->getParentSceneNode()) node->detachObject(piece);
        scene->destroyEntity(piece);
    }
    equipment.clear();
    worn_pieces.fill(nullptr);
    worn_signature.clear();

    if (subject != nullptr) {
        if (Ogre::SceneNode* node = subject->getParentSceneNode()) node->detachObject(subject);
        scene->destroyEntity(subject);
        subject = nullptr;
    }

    linked_paths.clear();
    playing.clear();
    subject_dir.clear();
    player_stats = tl::core::UnitStats{};
    // The model's list and anything playing on it went with the entity.
    player_catalog.clear();
    player_catalog_mesh.clear();
    skill_playback = SkillPlayback{};
}

void Renderer::Impl::fire_particle_trigger(const std::string& animation, size_t index,
                                           const tl::core::AnimationTag& tag) {
    if (subject == nullptr || tag.layout.empty()) return;
    Ogre::SceneNode* model = subject->getParentSceneNode();
    if (model == nullptr) return;

    ParticleTrigger* trigger = nullptr;
    for (ParticleTrigger& candidate : particle_triggers) {
        if (candidate.animation == animation && candidate.tag == index) trigger = &candidate;
    }
    const Ogre::Bone* bone = nullptr;
    if (subject->hasSkeleton() && !tag.bone.empty() && subject->getSkeleton()->hasBone(tag.bone)) {
        bone = subject->getSkeleton()->getBone(tag.bone);
    }

    if (trigger == nullptr) {
        ParticleTrigger made;
        made.animation = animation;
        made.tag = index;
        made.bone = tag.bone;
        made.offset = Ogre::Vector3(tag.offset_x, tag.offset_y, tag.offset_z);
        made.attaches = tag.attaches;
        made.stop_on_death = tag.name == "SPAWNPARTICLE_STOP_ON_DEATH";
        // `configureParticleTrigger`: an attached
        // trigger's particle follows the bone, else the character.
        if (made.attaches) made.follower = model->createChildSceneNode();
        std::string reference = tag.layout;
        for (char& c : reference) {
            if (c == '\\') c = '/';
        }
        // Started below, with every later key's start.
        made.holders =
            place_particle_layout(reference, Ogre::Vector3::ZERO, Ogre::Quaternion::IDENTITY,
                                  made.follower, ParticleStart::Held);
        for (Ogre::SceneNode* holder : made.holders) made.inside.push_back(holder->getPosition());
        particle_triggers.push_back(std::move(made));
        trigger = &particle_triggers.back();
        update_particle_triggers();
        TL_LOG_INFO("animation particle: '%s' key %zu releases %s on '%s'%s (%zu system node(s))",
                    animation.c_str(), index, reference.c_str(), tag.bone.c_str(),
                    trigger->attaches ? ", attached" : "", trigger->holders.size());
    }

    // `CCharacter::updateAnimation`, types 4 and 5: an
    // attached particle at the key's offset under its bone's node, else placed in the world;
    // then `CParticle::Start`.
    Ogre::Vector3 at = trigger->offset;
    if (!(trigger->attaches && trigger->follower != nullptr)) {
        const Ogre::Vector3 local =
            (bone != nullptr ? bone->_getDerivedPosition() : Ogre::Vector3::ZERO) + trigger->offset;
        at = model->_getFullTransform() * local;
    }
    for (size_t i = 0; i < trigger->holders.size(); ++i) {
        trigger->holders[i]->setPosition(at + trigger->inside[i]);
    }
    std::set<uint32_t> started;
    for (ParticleSystem& system : particle_systems) {
        Ogre::SceneNode* holder = system.holder();
        if (std::find(trigger->holders.begin(), trigger->holders.end(), holder) ==
            trigger->holders.end()) {
            continue;
        }
        restart_particle_system(system);
        if (system.cparticle != 0) started.insert(system.cparticle);
    }
    for (const uint32_t id : started) cparticle_started(id);
}

// PORT: the player's hand nodes and particle followers moved onto their bones.
void Renderer::Impl::update_particle_triggers() {
    if (subject == nullptr || !subject->hasSkeleton()) return;
    // The hand nodes (+0x2e8, +0x2f8): `getBones`' bone, `tag_` first, else `Bip01`.
    static const char* const kHandBones[2][2] = {{"tag_righthand", "Bip01 R Hand"},
                                                 {"tag_lefthand", "Bip01 L Hand"}};
    for (int hand = 0; hand < 2; ++hand) {
        if (hand_nodes[hand] == nullptr) continue;
        const Ogre::Bone* bone = nullptr;
        for (const char* name : kHandBones[hand]) {
            if (bone == nullptr && subject->getSkeleton()->hasBone(name)) {
                bone = subject->getSkeleton()->getBone(name);
            }
        }
        if (bone == nullptr) continue;
        hand_nodes[hand]->setPosition(bone->_getDerivedPosition());
        hand_nodes[hand]->setOrientation(bone->_getDerivedOrientation());
    }
    for (ParticleTrigger& trigger : particle_triggers) {
        if (trigger.follower == nullptr || !subject->getSkeleton()->hasBone(trigger.bone)) continue;
        const Ogre::Bone* bone = subject->getSkeleton()->getBone(trigger.bone);
        trigger.follower->setPosition(bone->_getDerivedPosition());
        trigger.follower->setOrientation(bone->_getDerivedOrientation());
    }
}

// PORT: the player's particle triggers destroyed.
void Renderer::Impl::clear_particle_triggers() {
    for (ParticleTrigger& trigger : particle_triggers) {
        destroy_particle_holders(trigger.holders);
        if (trigger.follower != nullptr) scene->destroySceneNode(trigger.follower);
    }
    particle_triggers.clear();
}

size_t Renderer::Impl::equip_carried_particles() {
    if (subject == nullptr || !subject->getMesh()) return 0;

    const std::string mesh_path = subject->getMesh()->getName();
    const tl::core::AnimationManifest& manifest = manifest_for(mesh_path);
    if (manifest.empty()) return 0;

    // The manifest keys on the skeleton's stem, which is the mesh's.
    std::string stem = mesh_path;
    const size_t slash = stem.find_last_of('/');
    if (slash != std::string::npos) stem = stem.substr(slash + 1);
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem.resize(dot);

    size_t released = 0;
    for (const tl::core::AnimationTag& tag : manifest.tags_for(stem)) {
        if (tag.name != "SPAWNPARTICLE" && tag.name != "SPAWNPARTICLE_STOP_ON_DEATH") continue;
        if (tag.layout.empty() || tag.bone.empty()) continue;
        // A `UNITTHEME` key runs only while the unit has the theme (`unitThemeUpdated`).
        //
        if (!tag.unit_theme.empty() && player_theme_names.count(tag.unit_theme) == 0) continue;

        // The manifest spells its paths with backslashes.
        std::string reference = tag.layout;
        for (char& c : reference) {
            if (c == '\\') c = '/';
        }

        if (!subject->hasSkeleton() || !subject->getSkeleton()->hasBone(tag.bone)) {
            TL_LOG_WARN("carried particle '%s' wants bone '%s', which this rig has not",
                        reference.c_str(), tag.bone.c_str());
            continue;
        }

        // The model's own keys are triggers (`CCharacter::loadModel`),
        // each hung by `configureParticleTrigger`
        // under a node following its bone (`updateBonePositions`).
        // Not a TagPoint: ParticleUniverse places from the entity's node.
        Ogre::SceneNode* model = subject->getParentSceneNode();
        if (model == nullptr) continue;
        const size_t index = static_cast<size_t>(&tag - manifest.tags_for(stem).data());
        const bool placed =
            std::any_of(particle_triggers.begin(), particle_triggers.end(),
                        [&](const ParticleTrigger& trigger) {
                            return trigger.animation == stem && trigger.tag == index;
                        });
        if (placed) continue;
        ParticleTrigger made;
        made.animation = stem;
        made.tag = index;
        made.bone = tag.bone;
        made.offset = Ogre::Vector3(tag.offset_x, tag.offset_y, tag.offset_z);
        made.stop_on_death = tag.name == "SPAWNPARTICLE_STOP_ON_DEATH";
        made.unit_theme = tag.unit_theme;
        made.follower = model->createChildSceneNode();
        const size_t before = particle_systems.size();
        made.holders = place_particle_layout(reference, Ogre::Vector3::ZERO,
                                             Ogre::Quaternion::IDENTITY, made.follower);
        for (Ogre::SceneNode* holder : made.holders) {
            made.inside.push_back(holder->getPosition());
            holder->setPosition(made.offset + holder->getPosition());
        }
        released += particle_systems.size() - before;
        particle_triggers.push_back(std::move(made));
        TL_LOG_INFO("carried particle: %s on '%s' (%zu system(s))", reference.c_str(),
                    tag.bone.c_str(), particle_systems.size() - before);
    }
    if (released > 0) update_particle_triggers();
    return released;
}

bool Renderer::Impl::equip_starting_weapon() {
    if (!subject || !subject->hasSkeleton()) return false;
    if (player_stats.right_hand.empty() && player_stats.left_hand.empty()) return false;

    ensure_item_index();

    const bool right = !player_stats.right_hand.empty();
    const std::string wanted = right ? player_stats.right_hand : player_stats.left_hand;
    // The copy the inventory made of it, when it holds one in a hand.
    for (const size_t hand : {static_cast<size_t>(tl::core::equip_location::kRightHand),
                              static_cast<size_t>(tl::core::equip_location::kLeftHand)}) {
        const tl::core::ItemStats* held = player_inventory.item_in_slot(hand);
        if (held != nullptr && held->name == wanted) return equip_weapon_item(*held);
    }
    const tl::core::ItemStats* item = items.find(wanted);
    if (item == nullptr) {
        TL_LOG_WARN(
            "the character is meant to hold '%s' and nothing in the archive is called "
            "that",
            wanted.c_str());
        return false;
    }
    // No inventory yet (the menu's): a copy as `createEquipmentAndEquip`
    // makes one.
    return equip_weapon_item(create_item(*item, false, false));
}

// PORT: a level unit's rolled hands equipped on its OGRE entity once its machine is built.
void Renderer::Impl::sync_unit_equipment() {
    for (auto& entry : scene_units) {
        SceneUnit& drawn = entry.second;
        if (drawn.equipment_synced || drawn.entity == nullptr) continue;
        const tl::core::UnitStats stats = stats_for_unit(entry.first);
        if (stats.spawn_right_hand.empty() && stats.spawn_left_hand.empty()) {
            // Nothing rolled: what it holds went on as it was placed.
            drawn.equipment_synced = true;
            continue;
        }
        const Ogre::SceneNode* parent = drawn.entity->getParentSceneNode();
        for (MachineView& view : machines()) {
            NodeMap* nodes = nodes_of(view.script);
            if (nodes == nullptr) continue;
            bool mine = false;
            const auto range = nodes->equal_range(drawn.node);
            for (auto it = range.first; it != range.second; ++it)
                mine = mine || it->second == parent;
            std::string right, left;
            // Not made yet (a layout's script is built after its meshes): later.
            if (!mine || !view.script->unit_hands(drawn.node, right, left)) continue;
            drawn.equipment_synced = true;
            equip_unit_items(drawn.entity, entry.first, stats.right_hand.empty() ? right : "",
                             stats.left_hand.empty() ? left : "");
            break;
        }
    }
}

void Renderer::Impl::equip_unit_items(Ogre::Entity* entity, const std::string& unit,
                                      const std::string& right_item, const std::string& left_item) {
    if (entity == nullptr || !entity->hasSkeleton() || unit.empty()) return;
    if (right_item.empty() && left_item.empty()) return;
    const tl::core::UnitStats stats = stats_for_unit(unit);
    ensure_item_index();
    const std::string unit_dir = dir_of_asset(entity->getMesh()->getName());
    for (const bool right : {true, false}) {
        const std::string& wanted = right ? right_item : left_item;
        if (wanted.empty()) continue;
        const tl::core::ItemStats* item = items.find(wanted);
        if (item == nullptr || item->mesh.empty()) {
            TL_LOG_WARN("%s: item '%s' %s", unit.c_str(), wanted.c_str(),
                        item == nullptr ? "is not in the item index" : "names no mesh");
            continue;
        }
        // `CEquipment::attachToGivenLocation` at location
        // 0 or 1: the hand's bone, a shield's `tag_leftarm`, nothing for a wardrobe item,
        // the wearer's `WEAPON_SCALE` or `SHIELD_SCALE`.
        load_unit_types();
        const tl::core::EquipmentAttachment where = tl::core::equipment_attachment(
            *item, right ? 0 : 1, unit, stats.weapon_scale, stats.shield_scale, unit_types);
        if (!where.attach) continue;
        const char* bone = where.bone.c_str();
        if (!entity->getSkeleton()->hasBone(bone)) {
            TL_LOG_WARN("%s: no '%s' bone for '%s'", unit.c_str(), bone, wanted.c_str());
            continue;
        }
        const std::string dir = dir_of_asset(item->mesh);
        std::string stem = item->mesh;
        const size_t slash = stem.find_last_of('/');
        if (slash != std::string::npos) stem = stem.substr(slash + 1);
        // The item's directory is matched without case (`Shields` against `shields`).
        std::string path;
        if (const std::vector<Ogre::FileInfo>* found = archive_entries(stem + ".MESH")) {
            for (const Ogre::FileInfo& info : *found) {
                if (lowered(info.path) == lowered(dir)) path = info.path + info.basename;
            }
        }
        if (path.empty()) {
            TL_LOG_WARN("%s: no '%s.MESH' under '%s'", unit.c_str(), stem.c_str(), dir.c_str());
            continue;
        }
        parse_sibling_material(path);
        Ogre::MeshPtr mesh;
        try {
            mesh = Ogre::MeshManager::getSingleton().load(path, kGameGroup);
        } catch (const Ogre::Exception& e) {
            TL_LOG_WARN("could not load '%s': %s", path.c_str(), e.getDescription().c_str());
            continue;
        }
        auto& materials = Ogre::MaterialManager::getSingleton();
        for (const Ogre::SubMesh* sub : mesh->getSubMeshes()) {
            qualify_texture_paths(materials.getByName(sub->getMaterialName(), kGameGroup), dir,
                                  unit_dir);
        }
        Ogre::Entity* held = scene->createEntity(mesh);
        rebind_materials(held);
        // `CCharacter::setRimlight` lights the equipped items too
        // (`CEquipment` vtable +0x150).
        apply_rim_light(held, rim_light_texture);
        Ogre::TagPoint* tag = entity->attachObjectToBone(bone, held);
        if (const RenderBehind* behind = render_behind_of(entity)) {
            set_render_behind(held, behind->on, behind->hostile);  // `CEquipment::setRenderBehind`
        }
        // What the weapon trails ask of the hands (`getEquipmentEquippedAt`).
        UnitHeldItems& hands = unit_held_items[entity];
        hands.held[right ? 0 : 1] = held;
        hands.unittype[right ? 0 : 1] = item->unittype;
        const float scale = where.scale;
        tag->setScale(scale, scale, scale);
        TL_LOG_INFO("%s holds %s in the %s hand (on '%s', scale %.2f)", unit.c_str(),
                    item->display_name.c_str(), right ? "right" : "left", bone,
                    static_cast<double>(scale));
        // The hand's node is `getBones`', scaled by `WEAPON_SCALE` whatever the
        // item is.
        attach_unit_weapon_element(entity, *item, bone,
                                   stats.weapon_scale > 0.0f ? stats.weapon_scale : 1.0f);
    }
}

void Renderer::Impl::attach_unit_weapon_element(Ogre::Entity* entity,
                                                const tl::core::ItemStats& item,
                                                const std::string& bone, float scale) {
    Ogre::SceneNode* model = entity->getParentSceneNode();
    if (model == nullptr) return;
    // `createParticles`: only a weapon (ISA 8) carries an
    // element.
    load_unit_types();
    const uint32_t type = unit_types.id_of(item.unittype);
    if (!unit_types.isa(type, tl::core::unit_type::kWeapon)) return;
    const tl::core::EffectManager::Context& effects = item_effect_context();
    // INVENTED: a placed unit's hands are item names, not `CEquipment` instances
    // (`createDefaultEquipment`); the element's share is asked of
    // the item's `DAMAGE_<TYPE>` split and its authored effects (`createElementalDamages`).
    tl::core::ItemStats held = item;
    held.damage_types.clear();
    held.damage_bonus = {};
    held.inherent_damage = {};
    for (const int element : {tl::core::kDamageElectric, tl::core::kDamageFire,
                              tl::core::kDamageIce, tl::core::kDamagePoison}) {
        const int share = held.damage_split[static_cast<size_t>(element)];
        if (0 < share) tl::core::add_inherent_damage(held, element, share);
    }
    tl::core::create_elemental_damages(held, effects, unit_types);
    const std::string name = tl::core::elemental_particle_name(
        unit_types.isa(type, tl::core::unit_type::kPistol),
        [&](int damage_type) { return tl::core::weapon_damage_bonus(held, damage_type, effects); });
    if (name.empty()) return;
    const std::string file = preloaded_particle_file(name);
    if (file.empty()) {
        TL_LOG_WARN("'%s': no particle '%s' in media/particles/preload.dat",
                    item.display_name.c_str(), name.c_str());
        return;
    }
    // `getBones`: the hand's node, a child of the unit's,
    // scaled by `WEAPON_SCALE`, on the weapon's bone.
    UnitWeaponElement made;
    made.entity = entity;
    made.bone = bone;
    made.hand = model->createChildSceneNode();
    made.hand->setScale(scale, scale, scale);
    const Ogre::Bone* hand_bone = entity->getSkeleton()->getBone(bone);
    made.hand->setPosition(hand_bone->_getDerivedPosition());
    made.hand->setOrientation(hand_bone->_getDerivedOrientation());
    made.holders =
        place_particle_layout(file, Ogre::Vector3::ZERO, Ogre::Quaternion::IDENTITY, made.hand);
    TL_LOG_INFO("  its %s carries %s: %s (%zu holder(s))", item.display_name.c_str(), name.c_str(),
                file.c_str(), made.holders.size());
    unit_weapon_elements.push_back(std::move(made));
}

// PORT: a unit's weapon particles destroyed with it.
void Renderer::Impl::clear_unit_weapon_elements(Ogre::Entity* entity) {
    for (size_t i = 0; i < unit_weapon_elements.size();) {
        UnitWeaponElement& held = unit_weapon_elements[i];
        const bool gone =
            entity != nullptr
                ? held.entity == entity
                : std::none_of(scene_units.begin(), scene_units.end(),
                               [&held](const auto& e) { return e.second.entity == held.entity; });
        if (!gone) {
            ++i;
            continue;
        }
        destroy_particle_holders(held.holders);
        if (held.hand != nullptr && scene != nullptr) scene->destroySceneNode(held.hand);
        unit_weapon_elements.erase(unit_weapon_elements.begin() + static_cast<long>(i));
    }
}

bool Renderer::Impl::equip_weapon_item(const tl::core::ItemStats& item, int location) {
    if (!subject || !subject->hasSkeleton()) return false;

    // A copy: its combat stats were rolled when it was made (`create_item`).
    weapon = item;
    has_weapon = true;

    // `CEquipment::attachToGivenLocation`: the
    // location's own bone, so a left-hand-only weapon is drawn in the left hand.
    const bool right = location >= 0 ? location == tl::core::equip_location::kRightHand
                                     : (lowered(weapon.unittype).find("bow") == std::string::npos);
    const tl::core::Hand hand = right ? tl::core::Hand::Right : tl::core::Hand::Left;
    attack_class = tl::core::attack_animation_class(weapon.unittype, hand);
    attack_location =
        right ? tl::core::equip_location::kRightHand : tl::core::equip_location::kLeftHand;

    const char* bone = right ? "tag_righthand" : "tag_lefthand";
    if (!subject->getSkeleton()->hasBone(bone)) {
        TL_LOG_WARN("no '%s' bone on the character; '%s' cannot be held", bone,
                    weapon.display_name.c_str());
        return false;
    }

    if (weapon.mesh.empty()) {
        TL_LOG_WARN("'%s' names no mesh", weapon.display_name.c_str());
        return false;
    }
    const std::string dir = dir_of_asset(weapon.mesh);
    std::string stem = weapon.mesh;
    const size_t slash = stem.find_last_of('/');
    if (slash != std::string::npos) stem = stem.substr(slash + 1);
    const std::string path = archive_path_in_dir(stem + ".MESH", dir);
    if (path.empty()) {
        TL_LOG_WARN("no '%s.MESH' under '%s'", stem.c_str(), dir.c_str());
        return false;
    }

    parse_sibling_material(path);

    Ogre::MeshPtr mesh;
    try {
        mesh = Ogre::MeshManager::getSingleton().load(path, kGameGroup);
    } catch (const Ogre::Exception& e) {
        TL_LOG_WARN("could not load '%s': %s", path.c_str(), e.getDescription().c_str());
        return false;
    }
    auto& materials = Ogre::MaterialManager::getSingleton();
    for (const Ogre::SubMesh* sub : mesh->getSubMeshes()) {
        qualify_texture_paths(materials.getByName(sub->getMaterialName(), kGameGroup), dir,
                              subject_dir);
    }

    if (weapon_entity != nullptr) {
        // `CCharacter::equipmentUnequipped`:
        // a weapon (ISA 8) going puts the hands' trails off.
        player_trails.trails.hide_hands();
        subject->detachObjectFromBone(weapon_entity);
        scene->destroyEntity(weapon_entity);
        weapon_entity = nullptr;
    }
    weapon_entity = scene->createEntity(mesh);
    rebind_materials(weapon_entity);
    // `CCharacter::setRimlight` hands its texture to every equipped
    // item's `setRimlight` (`CEquipment` vtable +0x150).
    apply_rim_light(weapon_entity, rim_light_texture);
    Ogre::TagPoint* tag = subject->attachObjectToBone(bone, weapon_entity);
    // `CCharacter::equipmentEquipped`: `setRenderBehind(+0x4a1)`,
    // which reaches the new item (`CEquipment::setRenderBehind`).
    if (const RenderBehind* behind = render_behind_of(subject)) {
        set_render_behind(weapon_entity, behind->on, behind->hostile);
    }
    const float scale = player_stats.weapon_scale > 0.0f ? player_stats.weapon_scale : 1.0f;
    tag->setScale(scale, scale, scale);

    TL_LOG_INFO("holding %s (%s) in the %s hand, %s at %.2f scale", weapon.display_name.c_str(),
                weapon.unittype.c_str(), right ? "right" : "left", path.c_str(),
                static_cast<double>(scale));
    TL_LOG_INFO("  swings on '%s', %.0f-%.0f%% of BASE_WEAPON_DAMAGE at level %d",
                attack_class.c_str(), static_cast<double>(weapon.min_damage),
                static_cast<double>(weapon.max_damage), weapon.level);
    attach_weapon_element(right);
    return true;
}

void Renderer::Impl::attach_weapon_element(bool right) {
    clear_weapon_element();
    if (subject == nullptr || !subject->hasSkeleton()) return;
    Ogre::SceneNode* model = subject->getParentSceneNode();
    if (model == nullptr) return;

    // `createParticles`, its second half: only a weapon
    // (ISA 8) carries an element; the drop particle (no owner, +0x290) is not for a hand.
    load_unit_types();
    const uint32_t type = unit_types.id_of(weapon.unittype);
    if (!unit_types.isa(type, tl::core::unit_type::kWeapon)) return;
    const tl::core::EffectManager::Context& effects = item_effect_context();
    const std::string name = tl::core::elemental_particle_name(
        unit_types.isa(type, tl::core::unit_type::kPistol), [&](int damage_type) {
            return tl::core::weapon_damage_bonus(weapon, damage_type, effects);
        });
    if (name.empty()) return;
    const std::string file = preloaded_particle_file(name);
    if (file.empty()) {
        TL_LOG_WARN("'%s': no particle '%s' in media/particles/preload.dat",
                    weapon.display_name.c_str(), name.c_str());
        return;
    }

    // `getBones`: the hand's node, a child of
    // the unit's, scaled by `WEAPON_SCALE`.
    const int hand = right ? 0 : 1;
    if (hand_nodes[hand] == nullptr) {
        hand_nodes[hand] = model->createChildSceneNode();
        const float scale = player_stats.weapon_scale > 0.0f ? player_stats.weapon_scale : 1.0f;
        hand_nodes[hand]->setScale(scale, scale, scale);
    }
    // `attachToGivenLocation`, locations 0 and 1: the
    // particle's node moved under the hand's, at `createParticles`' origin.
    weapon_element_hand = hand;
    weapon_element_particles = place_particle_layout(file, Ogre::Vector3::ZERO,
                                                     Ogre::Quaternion::IDENTITY, hand_nodes[hand]);
    update_particle_triggers();
    TL_LOG_INFO("  carries %s: %s on the %s hand (%zu holder(s))", name.c_str(), file.c_str(),
                right ? "right" : "left", weapon_element_particles.size());
}

void Renderer::Impl::unequip_weapon() {
    clear_weapon_element();
    // `CCharacter::equipmentUnequipped`, for a weapon.
    player_trails.trails.hide_hands();
    if (weapon_entity != nullptr && subject != nullptr) {
        subject->detachObjectFromBone(weapon_entity);
        scene->destroyEntity(weapon_entity);
        weapon_entity = nullptr;
    }
    has_weapon = false;
    attack_location = -1;
    attack_class = "Attack";
}

void Renderer::Impl::choose_player_attack_hand() {
    const tl::core::HeldHands hands = player_held_hands();
    switch (tl::core::attack_hand_step(hands)) {
        case tl::core::AttackHandStep::SelectAny:
            select_player_attack(0);
            return;
        case tl::core::AttackHandStep::Keep: return;  // equal ranges, nothing selected
        case tl::core::AttackHandStep::Mixed: break;
    }
    // the attack in use as `selectAttack(0)` left it.
    tl::core::AttackDescription attack =
        has_weapon ? tl::core::AttackDescription{}
                   : tl::core::own_attack(player_stats, player_stats.level, config.difficulty,
                                          /*player=*/true, stat_graphs);
    if (has_weapon) attack.range = weapon.range;
    const float ranged = tl::core::ranged_range(player_stats, attack, hands);
    const float melee = tl::core::melee_range(player_stats, attack, hands);
    if (melee == ranged) return;
    // with no melee weapon arc, the ranged hand (`selectAttack(2)`);
    // else the melee hand when a foe stands within its reach plus a metre, in the weapon's
    // arc from 1.5 out (`findCharacterWithinView`), else the ranged hand.
    load_unit_types();
    const bool right = tl::core::attack_melee_hand_is_right(hands);
    const tl::core::ItemStats& held =
        player_inventory
            .slots()[static_cast<size_t>(right ? tl::core::equip_location::kRightHand
                                               : tl::core::equip_location::kLeftHand)]
            .item;
    const uint32_t type = unit_types.id_of(held.unittype);
    const float arc =
        tl::core::console_weapon_arc(unit_types.isa(type, tl::core::unit_type::kSword),
                                     unit_types.isa(type, tl::core::unit_type::kAxe),
                                     unit_types.isa(type, tl::core::unit_type::kMace),
                                     unit_types.isa(type, tl::core::unit_type::kPolearm),
                                     unit_types.isa(type, tl::core::unit_type::kStaff));
    int mode = 2;
    if (arc != 0.0f && subject_node != nullptr) {
        ViewUnit found;
        if (find_character_within_view(player.position(), from_ogre(subject_node->getOrientation()),
                                       1.5f, arc, melee + 1.0f, /*alignment=*/5, /*nearest=*/true,
                                       ViewUnit{}, found)) {
            mode = 1;
        }
    }
    select_player_attack(mode);
}

void Renderer::Impl::clear_worn_attachments() {
    for (Ogre::Entity* worn : worn_attachments) {
        if (subject != nullptr) subject->detachObjectFromBone(worn);
        if (scene != nullptr) scene->destroyEntity(worn);
    }
    worn_attachments.clear();
}

void Renderer::Impl::sync_worn_attachments(int weapon_location) {
    // `CEquipment::equipped` runs `CEquipment::attachToGivenLocation`
    // per item put on, unless the wardrobe draws it; the
    // held weapon is `equip_weapon_item`'s. `detachFromLocation` undoes it.
    clear_worn_attachments();
    worn_left_hand = nullptr;
    if (subject == nullptr || !subject->hasSkeleton()) return;
    load_unit_types();
    const auto& slots = player_inventory.slots();
    for (int location = 0; location < 12; ++location) {
        if (location == weapon_location) continue;
        if (static_cast<size_t>(location) >= slots.size() || slots[location].count <= 0) continue;
        const tl::core::ItemStats& item = slots[static_cast<size_t>(location)].item;
        const tl::core::EquipmentAttachment where = tl::core::equipment_attachment(
            item, location, class_profile.name, player_stats.weapon_scale,
            player_stats.shield_scale, unit_types);
        if (!where.attach) continue;
        if (!subject->getSkeleton()->hasBone(where.bone)) {
            TL_LOG_WARN("no '%s' bone on the character for '%s'", where.bone.c_str(),
                        item.display_name.c_str());
            continue;
        }
        Ogre::Entity* worn = create_item_entity(item);
        if (worn == nullptr) continue;
        apply_rim_light(worn, rim_light_texture);  // `CCharacter::setRimlight`, as the weapon
        Ogre::TagPoint* tag = subject->attachObjectToBone(where.bone, worn);
        if (const RenderBehind* behind = render_behind_of(subject)) {
            set_render_behind(worn, behind->on, behind->hostile);
        }
        tag->setScale(where.scale, where.scale, where.scale);
        worn_attachments.push_back(worn);
        if (location == tl::core::equip_location::kLeftHand) worn_left_hand = worn;
        TL_LOG_INFO("the character wears %s on '%s' (location %d, scale %.2f)",
                    item.display_name.c_str(), where.bone.c_str(), location,
                    static_cast<double>(where.scale));
    }
}

// PORT: an item's mesh loaded and its OGRE entity made.
Ogre::Entity* Renderer::Impl::create_item_entity(const tl::core::ItemStats& item) {
    if (item.mesh.empty() || scene == nullptr) return nullptr;

    const std::string dir = dir_of_asset(item.mesh);
    std::string stem = item.mesh;
    const size_t slash = stem.find_last_of('/');
    if (slash != std::string::npos) stem = stem.substr(slash + 1);
    const std::string path = archive_path_in_dir(stem + ".MESH", dir);
    if (path.empty()) {
        TL_LOG_WARN("item '%s' names no mesh under '%s'", item.display_name.c_str(), dir.c_str());
        return nullptr;
    }

    parse_sibling_material(path);
    Ogre::MeshPtr mesh;
    try {
        mesh = Ogre::MeshManager::getSingleton().load(path, kGameGroup);
    } catch (const Ogre::Exception& e) {
        TL_LOG_WARN("could not load item mesh '%s': %s", path.c_str(), e.getDescription().c_str());
        return nullptr;
    }
    auto& materials = Ogre::MaterialManager::getSingleton();
    for (const Ogre::SubMesh* sub : mesh->getSubMeshes()) {
        qualify_texture_paths(materials.getByName(sub->getMaterialName(), kGameGroup), dir, dir);
    }

    Ogre::Entity* entity = scene->createEntity(mesh);
    rebind_materials(entity);
    return entity;
}

// PORT: a unit as the quest log's events read it.
tl::core::QuestEventUnit Renderer::Impl::quest_event_unit(const std::string& name,
                                                          const std::string& unittype,
                                                          const Ogre::Vector3& at,
                                                          const tl::core::QuestUnitTag& tag) {
    load_unit_types();
    tl::core::QuestEventUnit unit;
    unit.name = name;
    unit.tag = tag;
    const uint32_t type = unit_types.id_of(unittype);
    unit.isa = [this, type](uint32_t want) { return unit_types.isa(type, want); };
    unit.position = tl::core::Vec3{at.x, at.y, at.z};
    return unit;
}

Ogre::Vector3 Renderer::Impl::bag_event_position() const {
    if (bag_event_at_set) return bag_event_at;
    return subject_node != nullptr ? subject_node->getPosition() : Ogre::Vector3::ZERO;
}

// `CEquipment::addedToInventory`, the quest half:
// `questEventFire(0, player, item)`; an untagged item counted on pickup is stamped with
// its quest (`unitPickedUp`). The event 5 for an item held elsewhere (`+0x240`)
// is raised by the item leaving its bag first.
void Renderer::Impl::bag_item_added(tl::core::ItemStats& item) {
    const tl::core::QuestUnitTag carried = quest_tag_of_item(item);
    tl::core::QuestEventUnit taken =
        quest_event_unit(item.name, item.unittype, bag_event_position(), carried);
    taken.shown_name = tl::core::item_name(item);  // `getItemName()`
    quest_event(quest_log.item_picked_up(taken));
    if (!taken.tag.empty() && taken.tag.quest != carried.quest) {
        item.quest_guid = quest_guid_of(taken.tag);
    }
}

// `CEquipment::removedFromInventory`: `questEventFire(5, player, item)`,
// whatever takes the item out.
void Renderer::Impl::bag_item_removed(const tl::core::ItemStats& item) {
    tl::core::QuestEventUnit gone =
        quest_event_unit(item.name, item.unittype, bag_event_position(), quest_tag_of_item(item));
    quest_event(quest_log.item_dropped(gone));
}

void Renderer::Impl::quest_event(const std::vector<const tl::core::QuestDefinition*>& finished) {
    // `CGameUI::refreshQuestMenu` (X360 inline in `questEventUpdate`):
    // the open quest tab rebuilds its rows.
    if (quest_log.take_quest_menu_refresh()) inventory_menu.refresh_quest_tab();
    // `unitDefeated` and `unitPickedUp`: the
    // progress lines, for 1.0.
    for (const tl::core::QuestProgressText& text : quest_log.take_progress_texts()) {
        set_big_text(tl::core::strip_markup(text.big), 1.0f);
        set_small_text(text.small, 1.0f);
        TL_LOG_INFO("quest progress text: '%s' / '%s'", big_banner.text.c_str(),
                    small_banner.text.c_str());
        refresh_game_hud();
    }
    // `CQuestManager::questEventUpdate`: a quest this
    // event completed gets the "Quest Complete" banner and sample 0x20.
    for (const tl::core::QuestDefinition* quest : finished) {
        TL_LOG_INFO("quest '%s': objectives complete", quest->name.c_str());
        announce_quest(*quest, /*complete=*/true);
        // `SHOWTIPS` (`+0x2c`) or `CANABANDON` (`+0x208`).
        if (quest->show_tips || quest->can_abandon) {
            queue_tip(tl::core::ContextTip::QuestComplete);
        }
    }
    if (!finished.empty()) refresh_game_hud();
}

// PORT: a unit's theme particles destroyed.
void Renderer::Impl::clear_unit_themes(UnitThemeState& state) {
    for (UnitThemeState::Drawn& drawn : state.drawn) {
        destroy_particle_holders(drawn.holders);
        if (drawn.tag != nullptr) scene->destroySceneNode(drawn.tag);
    }
    state = UnitThemeState{};
}

void Renderer::Impl::unit_theme_updated(UnitThemeState& state, Ogre::Entity* entity,
                                        uint32_t unit_type, const tl::core::UnitTheme* theme,
                                        bool removed) {
    // The player's model keys of this theme start and stop with it
    // (`CCharacter::unitThemeUpdated`).
    if (&state == &player_themes && theme != nullptr) {
        std::string name = theme->name;
        for (char& c : name) {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
        if (removed) {
            player_theme_names.erase(name);
            for (size_t i = 0; i < particle_triggers.size();) {
                ParticleTrigger& trigger = particle_triggers[i];
                if (trigger.unit_theme != name) {
                    ++i;
                    continue;
                }
                destroy_particle_holders(trigger.holders);
                if (trigger.follower != nullptr) scene->destroySceneNode(trigger.follower);
                particle_triggers.erase(particle_triggers.begin() + static_cast<long>(i));
            }
        } else if (player_theme_names.insert(name).second) {
            equip_carried_particles();
        }
    }
    Ogre::SceneNode* unit_node = entity != nullptr ? entity->getParentSceneNode() : nullptr;
    if (unit_node == nullptr) return;
    Ogre::SkeletonInstance* skeleton = entity->hasSkeleton() ? entity->getSkeleton() : nullptr;

    const tl::core::ThemeParticleUnit unit = trigger_unit(
        state, entity, unit_type, theme != nullptr ? theme->name : std::string(), true);
    const std::vector<tl::core::ThemeParticleStep> steps =
        state.triggers.unit_theme_updated(theme, removed, unit, tl::core::volatile_random());
    configure_trigger_nodes(state, entity);

    for (const tl::core::ThemeParticleStep& step : steps) {
        const tl::core::ThemeParticleTrigger& trigger = state.triggers.triggers()[step.trigger];
        UnitThemeState::Drawn& drawn = state.drawn[static_cast<size_t>(trigger.particle)];
        if (step.start) {
            Ogre::Bone* bone =
                trigger.bone_found && skeleton != nullptr && skeleton->hasBone(trigger.bone)
                    ? skeleton->getBone(trigger.bone)
                    : nullptr;
            if (drawn.tag != nullptr && trigger.attaches && bone != nullptr) {
                drawn.tag->setPosition(bone->_getDerivedPosition());
                if (!trigger.follows) drawn.tag->setOrientation(bone->_getDerivedOrientation());
            } else if (drawn.tag != nullptr && !trigger.attaches) {
                // The unit's transform on the bone, or on the trigger's offset (+0x28).
                const Ogre::Vector3 local =
                    bone != nullptr
                        ? bone->_getDerivedPosition()
                        : Ogre::Vector3(trigger.offset_x, trigger.offset_y, trigger.offset_z);
                drawn.tag->setPosition(unit_node->_getFullTransform() * local);
            }
        }
        // `CParticle::Start` / `Stop` are the port's particle engine's: a stopped theme
        // particle is hidden, and shown when the theme returns.
        for (Ogre::SceneNode* holder : drawn.holders) holder->setVisible(step.start);
    }
}

tl::core::ThemeParticleUnit Renderer::Impl::trigger_unit(UnitThemeState& state,
                                                         Ogre::Entity* entity, uint32_t unit_type,
                                                         const std::string& label, bool light_map) {
    load_unit_types();
    Ogre::SceneNode* unit_node = entity->getParentSceneNode();
    Ogre::SkeletonInstance* skeleton = entity->hasSkeleton() ? entity->getSkeleton() : nullptr;

    tl::core::ThemeParticleUnit unit;
    unit.isa = [this, unit_type](const std::string& name) {
        return unit_types.isa(unit_type, unit_types.id_of(name));
    };
    unit.has_skeleton = skeleton != nullptr;
    unit.bone_count = skeleton != nullptr ? skeleton->getNumBones() : 0;
    unit.bone_name = [skeleton](uint16_t index) -> std::string {
        if (skeleton == nullptr || index >= skeleton->getNumBones()) return std::string();
        return skeleton->getBone(index)->getName();
    };
    unit.has_bone = [skeleton](const std::string& name) {
        return skeleton != nullptr && skeleton->hasBone(name);
    };
    // `CParticlePreloader::GetParticle`: the layout placed at the unit's node, stopped.
    unit.get_particle = [this, &state, unit_node, label](const std::string& asked) -> int {
        // A key spells its layout with backslashes.
        std::string file = asked;
        for (char& c : file) {
            if (c == '\\') c = '/';
        }
        std::vector<Ogre::SceneNode*> holders = place_particle_layout(
            file, Ogre::Vector3::ZERO, Ogre::Quaternion::IDENTITY, unit_node, ParticleStart::Held);
        if (holders.empty()) {
            TL_LOG_WARN("unit '%s': no particle '%s'", label.c_str(), file.c_str());
            return -1;
        }
        for (Ogre::SceneNode* holder : holders) holder->setVisible(false);
        // A theme's `IS LIGHT` particles reach the light map like any (place_particle()).
        UnitThemeState::Drawn drawn;
        drawn.holders = std::move(holders);
        state.drawn.push_back(std::move(drawn));
        return static_cast<int>(state.drawn.size() - 1);
    };
    return unit;
}

void Renderer::Impl::configure_trigger_nodes(UnitThemeState& state, Ogre::Entity* entity) {
    // `configureParticleTrigger`, the node half: an
    // attached particle on a bone rides a node under the unit's (+0x38), without a bone the
    // unit's node; one that does not attach stands in the world. At the trigger's offset.
    Ogre::SceneNode* unit_node = entity->getParentSceneNode();
    for (const tl::core::ThemeParticleTrigger& trigger : state.triggers.triggers()) {
        if (trigger.particle < 0) continue;
        UnitThemeState::Drawn& drawn = state.drawn[static_cast<size_t>(trigger.particle)];
        if (drawn.configured) continue;
        drawn.configured = true;
        if (!trigger.attaches || trigger.bone_found) {
            drawn.tag = trigger.attaches ? unit_node->createChildSceneNode()
                                         : scene->getRootSceneNode()->createChildSceneNode();
            for (Ogre::SceneNode* holder : drawn.holders) {
                if (Ogre::Node* up = holder->getParent()) up->removeChild(holder);
                drawn.tag->addChild(holder);
            }
        }
        const Ogre::Vector3 offset(trigger.offset_x, trigger.offset_y, trigger.offset_z);
        if (offset != Ogre::Vector3::ZERO) {
            for (Ogre::SceneNode* holder : drawn.holders) holder->translate(offset);
        }
    }
}

void Renderer::Impl::load_model_triggers(UnitThemeState& state, Ogre::Entity* entity,
                                         const std::string& mesh, uint32_t unit_type) {
    if (state.model_loaded) return;
    state.model_loaded = true;
    if (entity == nullptr || entity->getParentSceneNode() == nullptr) return;
    // The base skeleton is the one named as the mesh (`CGenericModel::loadAnimations`),
    // its animation the one called `BIND` (+0x5a).
    const std::vector<tl::core::AnimationTag>* keys = manifest_for(mesh).tags_of_file(mesh);
    if (keys == nullptr || keys->empty()) return;
    bool bind = false;
    if (entity->hasSkeleton()) {
        const Ogre::SkeletonInstance* skeleton = entity->getSkeleton();
        for (unsigned short i = 0; i < skeleton->getNumAnimations(); ++i) {
            if (tl::core::upper_ascii(skeleton->getAnimation(i)->getName()) == "BIND") bind = true;
        }
    }
    const size_t before = state.triggers.triggers().size();
    const tl::core::ThemeParticleUnit unit = trigger_unit(state, entity, unit_type, mesh, false);
    state.triggers.load_model(*keys, bind, &unit_themes, unit, tl::core::volatile_random());
    configure_trigger_nodes(state, entity);
    const size_t made = state.triggers.triggers().size() - before;
    if (made > 0) {
        TL_LOG_INFO("unit '%s': %zu particle(s) on its model%s", mesh.c_str(), made,
                    bind ? ", worn while shown" : "");
    }
}

// `CParticle::looping`: any emitter that does not expire
// or loops for ever (loop count -1, `+0x200`; X360 `+0x12c`).
bool Renderer::Impl::cparticle_looping(const UnitThemeState& state, int particle) const {
    if (particle < 0 || static_cast<size_t>(particle) >= state.drawn.size()) return false;
    const UnitThemeState::Drawn& drawn = state.drawn[static_cast<size_t>(particle)];
    for (const ParticleSystem& system : particle_systems) {
        if (system.pu.system == nullptr || std::find(drawn.holders.begin(), drawn.holders.end(),
                                                     system.holder()) == drawn.holders.end()) {
            continue;
        }
        if (cparticle_system_loops(system.pu)) return true;
    }
    return false;
}

void Renderer::Impl::apply_trigger_steps(UnitThemeState& state,
                                         const std::vector<tl::core::ThemeParticleStep>& steps) {
    for (const tl::core::ThemeParticleStep& step : steps) {
        const tl::core::ThemeParticleTrigger& trigger = state.triggers.triggers()[step.trigger];
        UnitThemeState::Drawn& drawn = state.drawn[static_cast<size_t>(trigger.particle)];
        if (step.park) {
            // `setVisible` hiding: `setPosition((0,
            // -99999, 0))`.
            const Ogre::Vector3 away(0.0f, -99999.0f, 0.0f);
            if (drawn.tag != nullptr) {
                drawn.tag->setPosition(away);
            } else {
                for (Ogre::SceneNode* holder : drawn.holders) holder->setPosition(away);
            }
            continue;
        }
        // `CParticle::Start`: systems from their start.
        // `Stop(true)` is gone at once, `Stop(false)` releases no more; hidden stops at once.
        bool shown = false;
        for (Ogre::SceneNode* holder : drawn.holders) {
            for (Ogre::MovableObject* object : holder->getAttachedObjects()) {
                shown = shown || object->getVisible();
            }
        }
        const bool at_once = !step.start && (step.at_once || !shown);
        if (step.start || at_once) {
            for (Ogre::SceneNode* holder : drawn.holders) holder->setVisible(step.start);
        }
        std::set<uint32_t> started;
        for (ParticleSystem& system : particle_systems) {
            if (std::find(drawn.holders.begin(), drawn.holders.end(), system.holder()) ==
                drawn.holders.end()) {
                continue;
            }
            if (step.start) {
                restart_particle_system(system);
                if (system.cparticle != 0) started.insert(system.cparticle);
            } else if (!at_once) {
                stop_particle_emission(system);
            }
        }
        for (const uint32_t id : started) cparticle_started(id);
    }
}

void Renderer::Impl::update_theme_particle_positions(UnitThemeState& state, Ogre::Entity* entity) {
    // `CCharacter::updateBonePositions` over `+0x738`: an
    // attached particle's node follows its bone, turning unless it `FOLLOWS`; an unattached
    // one that `FOLLOWS` moves to the bone's place, unturned.
    Ogre::SceneNode* unit_node = entity != nullptr ? entity->getParentSceneNode() : nullptr;
    if (unit_node == nullptr || !entity->hasSkeleton()) return;
    Ogre::SkeletonInstance* skeleton = entity->getSkeleton();
    for (const tl::core::ThemeParticleTrigger& trigger : state.triggers.triggers()) {
        if (trigger.particle < 0 || !trigger.bone_found) continue;
        UnitThemeState::Drawn& drawn = state.drawn[static_cast<size_t>(trigger.particle)];
        if (drawn.tag == nullptr || !skeleton->hasBone(trigger.bone)) continue;
        Ogre::Bone* bone = skeleton->getBone(trigger.bone);
        if (trigger.attaches) {
            drawn.tag->setPosition(bone->_getDerivedPosition());
            if (!trigger.follows) drawn.tag->setOrientation(bone->_getDerivedOrientation());
        } else if (trigger.follows) {
            drawn.tag->setPosition(unit_node->_getFullTransform() * bone->_getDerivedPosition());
        }
    }
}

void Renderer::Impl::step_unit_themes() {
    load_unit_types();
    load_unit_themes();
    // `CGameClient::update` refreshes the NPC icons when
    // the cinematic state changes.
    const bool cinematic = ui_in_cinematic();
    if (cinematic != npc_icons_cinematic) {
        npc_icons_cinematic = cinematic;
        update_npc_icons();
    }
    if (npc_icons_dirty && player_control && subject != nullptr) {
        npc_icons_dirty = false;
        update_npc_icons();
    }
    // The player's themes once it is in control (`load_player_stats` reads `[THEMES]`).
    if (subject != nullptr && player_control) {
        init_unit_themes(player_themes, player_stats);
        const uint32_t type = unit_types.id_of(player_stats.unittype);
        player_themes.themes.update(player_effects.themes(), [&](const tl::core::UnitTheme* theme,
                                                                 bool removed) {
            TL_LOG_INFO("player: theme %s %s", theme != nullptr ? theme->name.c_str() : "(none)",
                        removed ? "off" : "on");
            unit_theme_updated(player_themes, subject, type, theme, removed);
        });
        update_theme_particle_positions(player_themes, subject);
    }
    // `CBaseUnit::update`, the theme half: each placed
    // unit's own `CEffectManager` (`+0x1b8`) themes; none without a manager.
    static const std::vector<const tl::core::UnitTheme*> kNoEffectThemes;
    const UnitThemeSources sources = unit_theme_sources();
    for (auto& entry : scene_units) {
        SceneUnit& su = entry.second;
        if (su.entity == nullptr) continue;
        const tl::core::UnitStats* stats = unit_book.find(entry.first);
        const uint32_t type = unit_types.id_of(stats != nullptr ? stats->unittype : std::string());
        // `CCharacter::unitInit`: `loadModel` makes the
        // base skeleton's triggers, then `unitInitThemes`.
        load_model_triggers(su.themes, su.entity, su.mesh, type);
        if (!su.themes.initialised) {
            init_unit_themes(su.themes, stats != nullptr ? *stats : tl::core::UnitStats{});
        }
        bool dead = false;
        int32_t visible = -1;
        int32_t ai_state = -1;
        for (const tl::core::LevelScript* machine : sources.machines) {
            if (machine->unit_is_dead(su.node)) dead = true;
            if (visible < 0) {
                visible = machine->unit_visible(su.node);
                ai_state = machine->unit_ai_state(su.node);
            }
        }
        // A unit no machine runs (the menu's): its node's own flag.
        if (visible < 0) visible = su.entity->getVisible() ? 1 : 0;
        if ((visible == 1) != su.themes.shown) {
            // `CCharacter::setVisible(bool, bool)`; the
            // port's changes are immediate (`LevelScript::set_character_visible`): `Stop(true)`.
            su.themes.shown = visible == 1;
            apply_trigger_steps(su.themes, su.themes.triggers.set_visible(
                                               su.themes.shown, true, ai_state,
                                               [&su](const tl::core::UnitTheme* theme) {
                                                   return su.themes.themes.has_unit_theme(theme);
                                               },
                                               [this, &su](int particle) {
                                                   return cparticle_looping(su.themes, particle);
                                               }));
        }
        if (dead) {
            // `CCharacter::die`: every trigger (`+0x738`)
            // that stops on death is `CParticle::Stop`ped, and a dead unit's themes are not
            // updated; what does not stop stays on the body.
            if (!su.themes.stopped_by_death) {
                su.themes.stopped_by_death = true;
                apply_trigger_steps(su.themes, su.themes.triggers.die());
            }
            update_theme_particle_positions(su.themes, su.entity);
            // `CCharacter::completeDeath`, once the fall
            // is over: `DESTROY_ON_DEATH` marks it for deletion (`+0x190`); trails off.
            // INVENTED: its other two reasons (`attachesToMaster`, effect 0x81) are unread.
            if (su.playing_state == nullptr || su.playing_state->hasEnded()) {
                if (const auto trails = unit_trails.find(su.entity); trails != unit_trails.end()) {
                    trails->second.trails.hide_all();
                }
                // And `setRenderBehind(false)`: no outline on a body.
                if (const RenderBehind* behind = render_behind_of(su.entity);
                    behind != nullptr && behind->on) {
                    set_render_behind(su.entity, false, behind->hostile);
                }
            }
            if (stats != nullptr && stats->destroy_on_death &&
                (su.playing_state == nullptr || su.playing_state->hasEnded())) {
                tl::core::LevelScript* owner = script_owning(su.node);
                if (owner != nullptr && owner->mark_unit_for_deletion(su.node)) {
                    TL_LOG_INFO("unit '%s': its death is complete and it is destroyed",
                                entry.first.c_str());
                }
            }
            continue;
        }
        // `CCharacter::makeChampion` adds the `CHAMPION`
        // theme (the glow), from `populateSectionOfLevel`,
        // `unitInit` and `spawnUnit`.
        //
        if (!su.themes.champion_checked) {
            su.themes.champion_checked = true;
            if (spawn_marked_champion(su.node)) {
                su.themes.themes.add_unit_theme("CHAMPION", &unit_themes);
            }
        }
        const auto held = sources.effect_themes.find(&su);
        su.themes.themes.update(
            held != sources.effect_themes.end() ? *held->second : kNoEffectThemes,
            [&](const tl::core::UnitTheme* theme, bool removed) {
                TL_LOG_INFO("unit '%s': theme %s %s", entry.first.c_str(),
                            theme != nullptr ? theme->name.c_str() : "(none)",
                            removed ? "off" : "on");
                unit_theme_updated(su.themes, su.entity, type, theme, removed);
            });
        update_theme_particle_positions(su.themes, su.entity);
    }
}

// PORT: what step_unit_themes() reads of the machines, gathered once a frame.
Renderer::Impl::UnitThemeSources Renderer::Impl::unit_theme_sources() {
    UnitThemeSources out;
    for (const MachineView& view : machines()) {
        // One with no characters and no dead unit says not dead, -1 and -1 for every node,
        // and has no unit with effects.
        if (!view.script->has_characters() && !view.script->has_dead_units()) continue;
        out.machines.push_back(view.script);
        for (const int64_t node : view.script->units_with_effects()) {
            const tl::core::EffectManager* effects = view.script->unit_effects(node);
            const SceneUnit* drawn = scene_unit_of(view.script, node);
            if (effects != nullptr && drawn != nullptr) {
                out.effect_themes[drawn] = &effects->themes();
            }
        }
    }
    return out;
}

bool Renderer::Impl::ui_in_cinematic() const {
    // `CGameUI::getUIIsInCinematic`, the interface's half:
    // `+0x10bc`, the cinematic menu, or a menu `setInteractiveMenuVisible`
    // counts, the quest window's among them. The level's half is LevelScript's.
    return tl::core::LevelScript::client_paused() || cinematic_open || 0 < interactive_menus;
}

void Renderer::Impl::update_npc_icons() {
    // `CLevel::updateNPCIcons`: needs a player in level.
    if (subject == nullptr || !player_control) return;
    load_unit_types();
    load_unit_themes();
    load_quest_book();
    std::vector<MachineView> views = machines();
    const tl::core::UnitStats none;
    for (auto& entry : scene_units) {
        SceneUnit& su = entry.second;
        if (su.entity == nullptr) continue;
        const tl::core::UnitStats* stats = unit_book.find(entry.first);
        const tl::core::UnitStats& data = stats != nullptr ? *stats : none;
        init_unit_themes(su.themes, data);
        // `getEnabled() || !+0x70d`: a disabled, hidden unit is passed over; `+0x190`
        // (pending deletion) stands for a dead unit.
        bool enabled = true;
        bool dead = false;
        for (const MachineView& view : views) {
            if (!view.script->node_enabled(su.node)) enabled = false;
            if (view.script->unit_is_dead(su.node)) dead = true;
        }
        const bool shown = enabled || su.entity->isVisible();
        tl::core::update_npc_icon(quest_log, entry.first, data.quests,
                                  unit_types.id_of(data.unittype), unit_types,
                                  tl::core::volatile_random(), ui_in_cinematic(), shown, dead,
                                  su.themes.themes, &unit_themes);
    }
}

void Renderer::Impl::step_ground_labels() {
    if (!game_hud.ready()) return;
    std::vector<tl::render::GroundLabel> labels;
    // no modal window or `m_PauseMode` (+0x1811, set under one),
    // `m_bItemTextVisible` (+0x1812, the master menu shut), no full-screen menu.
    const bool open = player_control && camera != nullptr && subject_node != nullptr &&
                      !modal_dialog_open() && !inventory_menu.visible() && !full_screen_menu_up();
    if (!open) {
        game_hud.set_ground_labels(labels);
        return;
    }
    load_unit_types();
    const Ogre::Matrix4 view = camera->getViewMatrix();
    const Ogre::Matrix4 projection = camera->getProjectionMatrix();
    const Ogre::Vector3 lift = camera->getDerivedUp() * tl::core::kItemLabelLift;
    const Ogre::Vector3 me = subject_node->getPosition();
    const float across = static_cast<float>(config.width);
    const float down = static_cast<float>(config.height);

    // `CLevel::findItemsOnscreen`: the projection inside
    // [-1, 1] on both axes (strictly past -1 across), in front of the camera.
    auto on_screen = [&](const Ogre::Vector3& at) {
        const Ogre::Vector3 eye = view * at;
        if (!(eye.z < 0.0f)) return false;
        const Ogre::Vector3 clip = projection * eye;
        return !(clip.x <= -1.0f || 1.0f < clip.x || clip.y < -1.0f || 1.0f < clip.y);
    };
    // the anchor, lifted, projected to the screen's pixels.
    auto project = [&](const Ogre::Vector3& at, float& x, float& y) {
        const Ogre::Vector3 clip = projection * (view * (at + lift));
        x = across * 0.5f * clip.x + across * 0.5f;
        y = down * 0.5f - down * 0.5f * clip.y;
    };
    int highlighted_index = -1;
    auto add = [&](const std::string& name, const Ogre::Vector3& at, bool highlighted,
                   uint32_t type, const tl::core::ItemLabelFacts& facts) {
        tl::core::ItemLabelQuery query;
        query.highlighted = highlighted;
        query.container = unit_types.isa(type, tl::core::unit_type::kContainer);
        query.breakable = unit_types.isa(type, tl::core::unit_type::kBreakable);
        query.distance_squared = (at - me).squaredLength();
        // DIVERGENCE: no `KSETTINGS_KEYMAP_SHOWITEMS` key on a pad and no `TOGGLE DROPPED
        // ITEMS` setting; its default, 0, is used.
        query.show_items_held = false;
        query.toggle_dropped_items = 0;
        if (!tl::core::item_label_wanted(query)) return;
        tl::render::GroundLabel label;
        label.text = tl::core::item_label_text(name);
        project(at, label.at_x, label.at_y);
        const tl::core::LabelColour colour = tl::core::item_label_colour(
            tl::core::item_label_tint(facts), highlighted, item_label_colours);
        label.colour = Ogre::ColourValue(colour.r, colour.g, colour.b, colour.a);
        label.enabled = highlighted;
        if (highlighted) highlighted_index = static_cast<int>(labels.size());
        labels.push_back(label);
    };

    for (size_t i = 0; i < dropped_items.size(); ++i) {
        const DroppedWorldItem& dropped = dropped_items[i];
        // Skipped: a flying item (disabled by `drop`, vtable +0x40) or a hidden one (`+0x81`).
        if (dropped.flying || dropped.node == nullptr || !dropped.presence.visible) continue;
        const Ogre::Vector3 at = dropped.node->getPosition();
        if (!on_screen(at)) continue;
        const tl::core::ItemStats& item = dropped.item;
        const uint32_t type = unit_types.id_of(item.unittype);
        tl::core::ItemLabelFacts facts;
        if (!dropped.is_gold) {
            // A `CEquipment`; gold (`CItemGold`) is never quest, unique or magical.
            facts.quest =
                unit_types.isa(type, tl::core::unit_type::kQuestItem) || item.quest_guid != -1;
            facts.set = !item.set.empty();
            facts.unique = unit_types.isa(type, tl::core::unit_type::kUnique);
            facts.magical = tl::core::item_is_magical(item, unit_types);
            facts.random_magic_socketable =
                unit_types.isa(type, tl::core::unit_type::kRandomMagicSocketable);
            facts.magic = unit_types.isa(type, tl::core::unit_type::kMagic);
        }
        // Gold's name is `CItem::getItemName`, `+0x210`.
        const std::string& name = dropped.is_gold ? item.display_name : tl::core::item_name(item);
        add(name, at, static_cast<int>(i) == item_targeted, type, facts);
    }

    // Every `CTriggerUnit` is a `CItem`, labelled the same way; a non-takeable one only
    // while `+0x1f0` is set (cleared by `CTriggerUnit::trigger`,
    // on a one-shot's last state), never one with `NONSELECTABLE`.
    const auto triggers = [&](int machine, const tl::core::LevelScript& owner) {
        for (const tl::core::LevelScript::UnitTriggerView& view_of : owner.unit_triggers()) {
            if (!view_of.armed) continue;
            if (non_selectable_prop(view_of.unit)) continue;
            // Passed over when hidden by its range or the camera (`+0x81`).
            const auto shown = trigger_presence.find(instance_key(view_of.id, machine));
            if (shown != trigger_presence.end() && !shown->second.presence.visible) continue;
            TriggerProp prop;
            trigger_prop(view_of.unit, prop);
            if (prop.display_name.empty()) continue;  // no such unit to label
            const Ogre::Vector3 at(view_of.at.x, view_of.at.y, view_of.at.z);
            if (!on_screen(at)) continue;
            const bool highlighted =
                machine == trigger_targeted_machine && view_of.id == trigger_targeted_node;
            add(prop.display_name, at, highlighted, unit_types.id_of(prop.unittype),
                tl::core::ItemLabelFacts{});
        }
    };
    triggers(kLevelMachine, script);
    for (size_t i = 0; i < nested_scripts.size(); ++i) {
        triggers(static_cast<int>(i), nested_scripts[i].script);
    }
    for (size_t i = 0; i < open_portals.size(); ++i) {
        const OpenPortal& portal = open_portals[i];
        if (!portal.enabled) continue;
        if (!on_screen(to_ogre(portal.at))) continue;
        add(portal.display_name, to_ogre(portal.at), static_cast<int>(i) == portal_targeted,
            unit_types.id_of(portal.unittype), tl::core::ItemLabelFacts{});
    }
    // `CLevel::findItemsOnscreen` lists the `MIMICIDLE` mimics too, worded by
    // `CGameUI::updateIngameUI` with `CCharacter::getMimicName`.
    // INVENTED: the label's tint and distance rule are a chest's (`OPENABLE`).
    const auto mimics = [&](int machine, const tl::core::LevelScript& owner) {
        for (const tl::core::LevelScript::UnitTriggerView& mimic : owner.idle_mimics()) {
            const Ogre::Vector3 at(mimic.at.x, mimic.at.y, mimic.at.z);
            if (!on_screen(at)) continue;
            const std::string& dormant = stats_for_unit(mimic.unit).dormant_name;
            add(dormant.empty() ? std::string("Chest") : dormant, at,
                machine == mimic_targeted_machine && mimic.id == mimic_targeted_node,
                unit_types.id_of("OPENABLE"), tl::core::ItemLabelFacts{});
        }
    };
    mimics(kLevelMachine, script);
    for (size_t i = 0; i < nested_scripts.size(); ++i) {
        mimics(static_cast<int>(i), nested_scripts[i].script);
    }

    // `setItemTextHighlighted(true)` ends with `moveToFront`: drawn last.
    if (highlighted_index >= 0 && highlighted_index + 1 < static_cast<int>(labels.size())) {
        std::rotate(labels.begin() + highlighted_index, labels.begin() + highlighted_index + 1,
                    labels.end());
    }
    // More than the pool draws: keep the highlighted one among those drawn.
    const size_t pool = tl::render::GameHud::kMaxGroundLabels;
    if (highlighted_index >= 0 && labels.size() > pool) std::swap(labels[pool - 1], labels.back());
    game_hud.set_ground_labels(labels);
}

/// `CCharacter::getItemAI`'s targeting half: the nearest
/// `dropped_items` entry within `kTargetItemRadius` of a point `kTargetItemBehind` behind
/// the player. `active` false (a unit is targeted,
/// `CCharacter::setTargetItem`) clears the pick without searching.
bool Renderer::Impl::update_item_selection(bool active) {
    int chosen = -1;
    int machine_chosen = kNoMachine;
    int64_t node_chosen = 0;
    int portal_chosen = -1;
    int mimic_machine_chosen = kNoMachine;
    int64_t mimic_node_chosen = 0;
    Ogre::Entity* entity = nullptr;
    bool second = false;
    if (active && subject_node != nullptr) {
        const Ogre::Vector3 forward = subject_node->getOrientation() * Ogre::Vector3::UNIT_Z;
        const Ogre::Vector3 from =
            subject_node->getPosition() - forward * tl::core::kTargetItemBehind;
        float best = tl::core::kTargetItemRadius;
        // The cone's selection (given this point and 3.5):
        // in the ground plane, height does not count.
        const auto planar = [&](const Ogre::Vector3& at) {
            const float dx = at.x - from.x;
            const float dz = at.z - from.z;
            return std::sqrt(dx * dx + dz * dz);
        };
        for (size_t i = 0; i < dropped_items.size(); ++i) {
            // Nothing to take while it flies (`drop` / `updateDrop`, vtable +0x40).
            if (dropped_items[i].flying) continue;
            const float dist = planar(dropped_items[i].node->getPosition());
            if (dist <= best) {
                best = dist;
                chosen = static_cast<int>(i);
                entity = dropped_items[i].entity;
            }
        }
        // The props the same pass finds: every enabled `Unit Trigger`, where its node
        // stands. A `No Loop` (`+0x254` 0) one-shot past its last state is not clickable
        // (`CTriggerUnit::trigger`, clears `+0x1f0`).
        const auto consider = [&](int machine, const tl::core::LevelScript& owner,
                                  const NodeMap& nodes) {
            for (const tl::core::LevelScript::UnitTriggerView& view : owner.unit_triggers()) {
                if (!view.armed) continue;
                if (non_selectable_prop(view.unit)) continue;
                const float dist = planar(Ogre::Vector3(view.at.x, view.at.y, view.at.z));
                if (!(dist <= best)) continue;
                best = dist;
                chosen = -1;
                machine_chosen = machine;
                node_chosen = view.id;
                entity = trigger_entity(view.id, view.unit, nodes);
            }
        };
        consider(kLevelMachine, script, scripted_nodes);
        for (size_t i = 0; i < nested_scripts.size(); ++i) {
            const NestedScript& machine = nested_scripts[i];
            consider(static_cast<int>(i), machine.script, machine.nodes);
        }
        // The portal props `createPortals` added: `CTriggerUnit`s like the rest.
        for (size_t i = 0; i < open_portals.size(); ++i) {
            if (!open_portals[i].enabled) continue;
            const float dist = planar(to_ogre(open_portals[i].at));
            if (!(dist <= best)) continue;
            best = dist;
            chosen = -1;
            machine_chosen = kNoMachine;
            node_chosen = 0;
            portal_chosen = static_cast<int>(i);
            entity = nullptr;
        }
        // A mimic lying in wait, among the props it passes for.
        const auto consider_mimics = [&](int machine, const tl::core::LevelScript& owner) {
            for (const tl::core::LevelScript::UnitTriggerView& mimic : owner.idle_mimics()) {
                const float dist = planar(Ogre::Vector3(mimic.at.x, mimic.at.y, mimic.at.z));
                if (!(dist <= best)) continue;
                best = dist;
                chosen = -1;
                machine_chosen = kNoMachine;
                node_chosen = 0;
                portal_chosen = -1;
                mimic_machine_chosen = machine;
                mimic_node_chosen = mimic.id;
                entity = nullptr;
                for (const auto& entry : scene_units) {
                    if (entry.second.node == mimic.id) entity = entry.second.entity;
                }
            }
        };
        consider_mimics(kLevelMachine, script);
        for (size_t i = 0; i < nested_scripts.size(); ++i) {
            consider_mimics(static_cast<int>(i), nested_scripts[i].script);
        }

        // the function's last call: with nothing found, the second pick
        // over the level's items (shown, usable, active, selectable, not breakable) in the
        // character's frame; the sight line runs 5 above both, through units' models.
        if (chosen < 0 && machine_chosen == kNoMachine && portal_chosen < 0 &&
            mimic_machine_chosen == kNoMachine) {
            const Ogre::Vector3 here = subject_node->getPosition();
            const Ogre::Quaternion back = subject_node->getOrientation().Inverse();
            std::vector<tl::core::SecondPickCandidate> picks;
            struct Source {
                int dropped = -1;
                int machine = kNoMachine;
                int64_t node = 0;
                int portal = -1;
                Ogre::Entity* entity = nullptr;
            };
            std::vector<Source> sources;
            const auto add = [&](const Ogre::Vector3& at, bool fishing_hole, const Source& source) {
                const Ogre::Vector3 local = back * (at - here);
                tl::core::SecondPickCandidate pick;
                pick.local_x = local.x;
                pick.local_z = local.z;
                pick.fishing_hole = fishing_hole;
                tl::core::CollisionHit hit;
                pick.in_sight =
                    collision.empty() ||
                    !collision.ray_collision(
                        tl::core::Vec3{here.x, here.y + tl::core::kTargetSightHeight, here.z},
                        tl::core::Vec3{at.x, at.y + tl::core::kTargetSightHeight, at.z}, false,
                        hit);
                picks.push_back(pick);
                sources.push_back(source);
            };
            for (size_t i = 0; i < dropped_items.size(); ++i) {
                const DroppedWorldItem& dropped = dropped_items[i];
                if (dropped.flying || !dropped.presence.active || !dropped.presence.visible)
                    continue;
                Source source;
                source.dropped = static_cast<int>(i);
                source.entity = dropped.entity;
                add(dropped.node->getPosition(), false, source);
            }
            const auto consider_props = [&](int machine, const tl::core::LevelScript& owner,
                                            const NodeMap& nodes) {
                for (const tl::core::LevelScript::UnitTriggerView& view : owner.unit_triggers()) {
                    const int64_t key = instance_key(view.id, machine);
                    if (!view.armed) continue;
                    if (non_selectable_prop(view.unit)) continue;
                    const auto presence = trigger_presence.find(key);
                    if (presence != trigger_presence.end() &&
                        (!presence->second.presence.active || !presence->second.presence.visible)) {
                        continue;
                    }
                    Source source;
                    source.machine = machine;
                    source.node = view.id;
                    source.entity = trigger_entity(view.id, view.unit, nodes);
                    add(Ogre::Vector3(view.at.x, view.at.y, view.at.z), is_fishing_hole(view.unit),
                        source);
                }
            };
            consider_props(kLevelMachine, script, scripted_nodes);
            for (size_t i = 0; i < nested_scripts.size(); ++i) {
                consider_props(static_cast<int>(i), nested_scripts[i].script,
                               nested_scripts[i].nodes);
            }
            for (size_t i = 0; i < open_portals.size(); ++i) {
                if (!open_portals[i].enabled) continue;
                Source source;
                source.portal = static_cast<int>(i);
                add(to_ogre(open_portals[i].at), false, source);
            }
            const int pick = tl::core::second_pick(picks);
            if (pick >= 0) {
                const Source& source = sources[static_cast<size_t>(pick)];
                chosen = source.dropped;
                machine_chosen = source.machine;
                node_chosen = source.node;
                portal_chosen = source.portal;
                entity = source.entity;
                second = true;
            }
        }
    }

    if (entity != item_highlighted) {
        if (item_highlighted != nullptr) {
            bool still_there = entity_still_placed(item_highlighted);
            for (const DroppedWorldItem& dropped : dropped_items) {
                if (dropped.entity == item_highlighted) {
                    still_there = true;
                    break;
                }
            }
            // Not still there: picked up this frame, its entity already destroyed.
            if (still_there) highlight_entity(item_highlighted, false, false, rim_light_texture);
        }
        if (entity != nullptr) highlight_entity(entity, true, false, rim_light_texture);
        item_highlighted = entity;
    }
    if (machine_chosen != trigger_targeted_machine || node_chosen != trigger_targeted_node) {
        if (machine_chosen != kNoMachine) {
            TL_LOG_INFO("pad prop: unit trigger %lld%s", static_cast<long long>(node_chosen),
                        entity != nullptr ? "" : " (no model to light)");
        }
    }
    if (portal_chosen != portal_targeted && portal_chosen >= 0) {
        TL_LOG_INFO("pad prop: portal '%s'",
                    open_portals[static_cast<size_t>(portal_chosen)].display_name.c_str());
    }
    if (second && (chosen != item_targeted || machine_chosen != trigger_targeted_machine ||
                   node_chosen != trigger_targeted_node || portal_chosen != portal_targeted)) {
        TL_LOG_INFO("pad item: the second pick (%s)", chosen >= 0          ? "a floor item"
                                                      : portal_chosen >= 0 ? "a portal"
                                                                           : "a prop");
    }
    item_second_pick = second;
    if (mimic_node_chosen != mimic_targeted_node && mimic_node_chosen != 0) {
        TL_LOG_INFO("pad prop: a mimic lying in wait (%lld)",
                    static_cast<long long>(mimic_node_chosen));
    }
    item_targeted = chosen;
    trigger_targeted_machine = machine_chosen;
    trigger_targeted_node = node_chosen;
    portal_targeted = portal_chosen;
    mimic_targeted_machine = mimic_machine_chosen;
    mimic_targeted_node = mimic_node_chosen;
    return chosen >= 0 || machine_chosen != kNoMachine || portal_chosen >= 0 ||
           mimic_machine_chosen != kNoMachine;
}

void Renderer::Impl::target_selected_item() {
    // (A's handler): with no unit target, the selector's item into
    // `+0x1cc` and `setTargetItem`, which drops the unit
    // target; AI state 7, `getItemAI`.
    PlayerItemTarget target;
    if (portal_targeted >= 0 && static_cast<size_t>(portal_targeted) < open_portals.size()) {
        const OpenPortal& portal = open_portals[static_cast<size_t>(portal_targeted)];
        target.kind = PlayerItemTarget::Kind::Portal;
        target.portal = portal.unit;
        target.portal_at = to_ogre(portal.at);
    } else if (trigger_targeted_machine != kNoMachine) {
        target.kind = PlayerItemTarget::Kind::Trigger;
        target.machine = trigger_targeted_machine;
        target.node = trigger_targeted_node;
    } else if (item_targeted >= 0 && static_cast<size_t>(item_targeted) < dropped_items.size()) {
        target.kind = PlayerItemTarget::Kind::Dropped;
        target.dropped = dropped_items[static_cast<size_t>(item_targeted)].node;
    } else {
        return;
    }
    pending_talk.clear();
    clear_talk_target();
    player_item = target;
    TL_LOG_INFO("pad item: A sets the target item (%s%s)",
                target.kind == PlayerItemTarget::Kind::Dropped  ? "a floor item"
                : target.kind == PlayerItemTarget::Kind::Portal ? "a portal"
                                                                : "a prop",
                item_second_pick ? ", the second pick" : "");
}

void Renderer::Impl::step_mimic_attack(float delta) {
    const auto let_go = [this](const char* why) {
        TL_LOG_INFO("pad prop: the mimic is let go (%s)", why);
        player_item = PlayerItemTarget{};
    };
    if (!player_control || subject_node == nullptr) {
        player_item = PlayerItemTarget{};
        return;
    }
    // `CCharacter::attackAI` on `+0x340`: a dying, dead
    // or gone target is let go. INVENTED: one that woke is let go too.
    tl::core::LevelScript* owner = machine_script(player_item.machine);
    tl::core::Vec3 at;
    if (owner == nullptr || owner->unit_is_dead(player_item.node) ||
        !owner->unit_position(player_item.node, at)) {
        return let_go("gone");
    }
    if (!owner->unit_mimic_idle(player_item.node)) return let_go("awake");
    load_unit_types();
    load_stat_graphs();
    const tl::core::Vec3 me = player.position();
    const float dx = at.x - me.x;
    const float dz = at.z - me.z;
    const float d = std::sqrt(dx * dx + 0.0f + dz * dz);
    // Beyond `FOLLOW_RADIUS` (`+0x370`), with no master: stopped and let go.
    if (!(d < player_stats.follow_radius)) {
        player.stop_following();
        return let_go("past the follow radius");
    }
    // `inAttackRange(0)` with the attack in use (weapon
    // `RANGE`, or bare hands via `selectAttack`); a `RANGED WEAPON`
    // (ISA 0x23) skips the height test and adds effect 0x4e.
    select_player_attack(0);
    const tl::core::HeldHands hands = player_held_hands();
    tl::core::AttackDescription attack =
        has_weapon ? tl::core::AttackDescription{}
                   : tl::core::own_attack(player_stats, player_stats.level, config.difficulty,
                                          /*player=*/true, stat_graphs);
    if (has_weapon) attack.range = weapon.range;
    const bool ranged = has_weapon && unit_types.isa(unit_types.id_of(weapon.unittype),
                                                     tl::core::unit_type::kRangedWeapon);
    tl::core::UnitStats body = player_stats;
    body.radius = player.body().radius;
    const bool in_range = tl::core::in_attack_range(
        body, attack, me, at, owner->unit_radius(player_item.node), ranged,
        ranged ? player_total().effect_value(0x4e) : 0.0f, &hands);
    // `validLineOfSight(level, false)`: 1.5
    // above both feet, the rise clamped over 5; units' models block
    // (`LevelScript::valid_line_of_sight`).
    bool sight = true;
    {
        const tl::core::Vec3 from{me.x, me.y + 1.5f, me.z};
        tl::core::Vec3 to{at.x, at.y + 1.5f, at.z};
        const float limit = std::sqrt(to.x * to.x + from.y * from.y + to.z * to.z) / 5.0f;
        float rise = to.y - from.y;
        if (limit <= rise) rise = limit;
        if (rise <= -limit) rise = -limit;
        to.y = rise + from.y;
        tl::core::CollisionHit hit;
        sight = collision.empty() || !collision.ray_collision(from, to, true, hit);
    }
    if (!in_range || !sight) {
        // A swing with more than 0.2 s left or a cast holding: nothing; else `setAIState(4)`.
        if (swing.active && 0.2f < (swing.length - swing.elapsed) / swing_rate()) return;
        if (player_performing_skill()) return;
        if (!player.following()) {
            std::vector<tl::core::Vec3> route;
            if (nav.ready()) route = nav.find_path(me, at, passability);
            if (route.empty()) return let_go("no route");
            player.follow(std::move(route));
        }
        return;
    }
    // In reach and sight: stopped, turned (`turnTowardPosition`), and once facing it (half
    // of `ATTACKANGLE`) `attack()`, then `setAIState(0)`.
    player.stop_following();
    player.face(tl::core::turn_toward(player.facing(), dx, dz, player_stats.turn_rate, delta));
    if (!tl::core::facing_target(player.facing(), me, at, player_stats.attack_angle)) return;
    // `CCharacter::attack` on a `MIMICIDLE` target: ranged,
    // `setDestination` to it; else `CMonster::reactToDamage` (vtable +0x350)
    // and no blow.
    if (ranged) {
        std::vector<tl::core::Vec3> route;
        if (nav.ready()) route = nav.find_path(me, at, passability);
        if (!route.empty()) player.follow(std::move(route));
        return;
    }
    mimic_targeted_machine = player_item.machine;
    mimic_targeted_node = player_item.node;
    player_item = PlayerItemTarget{};
    disturb_targeted_mimic();
}

void Renderer::Impl::step_item_ai(float delta) {
    using Kind = PlayerItemTarget::Kind;
    if (player_item.kind == Kind::None) return;
    if (player_item.kind == Kind::Mimic) return step_mimic_attack(delta);
    const auto let_go = [this](const char* why) {
        TL_LOG_INFO("pad item: the target item is let go (%s)", why);
        player_item = PlayerItemTarget{};
    };
    if (!player_control || subject_node == nullptr) {
        player_item = PlayerItemTarget{};
        return;
    }
    load_unit_types();

    // The target as `getItemAI` reads it; one the level no longer holds is let go
    // (`notifyOfDeletion`).
    tl::core::ItemAITarget item;
    std::string unittype;
    int dropped_index = -1;
    int portal_index = -1;
    switch (player_item.kind) {
        case Kind::Dropped:
            for (size_t i = 0; i < dropped_items.size(); ++i) {
                if (dropped_items[i].node == player_item.dropped)
                    dropped_index = static_cast<int>(i);
            }
            if (dropped_index < 0) return let_go("gone");
            {
                const DroppedWorldItem& dropped = dropped_items[static_cast<size_t>(dropped_index)];
                const Ogre::Vector3 at = dropped.node->getPosition();
                item.at = tl::core::Vec3{at.x, at.y, at.z};
                // `CBaseUnit::CBaseUnit`: 0.3;
                // `CItem::unitInit` reads no `COLLISION_RADIUS`.
                item.radius = 0.3f;
                unittype = dropped.item.unittype;
            }
            break;
        case Kind::Trigger: {
            tl::core::LevelScript* owner = machine_script(player_item.machine);
            if (owner == nullptr) return let_go("gone");
            bool found = false;
            for (const tl::core::LevelScript::UnitTriggerView& view : owner->unit_triggers()) {
                if (view.id != player_item.node) continue;
                found = true;
                item.at = view.at;
                // `CTriggerUnit::unitInit`: `COLLISION_RADIUS`.
                TriggerProp prop;
                item.radius = trigger_prop(view.unit, prop) ? prop.collision_radius : 0.3f;
                unittype = prop.unittype;
            }
            if (!found) return let_go("disabled");
            break;
        }
        case Kind::Portal:
            for (size_t i = 0; i < open_portals.size(); ++i) {
                if (open_portals[i].enabled && open_portals[i].unit == player_item.portal &&
                    to_ogre(open_portals[i].at) == player_item.portal_at) {
                    portal_index = static_cast<int>(i);
                }
            }
            if (portal_index < 0) return let_go("gone");
            {
                const OpenPortal& portal = open_portals[static_cast<size_t>(portal_index)];
                item.at = tl::core::Vec3{portal.at.x, portal.at.y, portal.at.z};
                TriggerProp prop;
                item.radius = trigger_prop(portal.unit, prop) ? prop.collision_radius : 0.3f;
                unittype = portal.unittype;
            }
            break;
        case Kind::None:
        case Kind::Mimic: return;
    }
    namespace ut = tl::core::unit_type;
    const uint32_t type = unit_types.id_of(unittype);
    item.container = unit_types.isa(type, 0x1e);
    item.breakable = unit_types.isa(type, 0x1d);
    item.interactable = unit_types.isa(type, 0x20);
    item.fishing_hole = unit_types.isa(type, ut::kFishingHole);
    item.enchant_shrine = unit_types.isa(type, ut::kEnchantShrine);
    item.stat_shrine = unit_types.isa(type, ut::kStatShrine);
    item.gem_shrine = unit_types.isa(type, ut::kGemShrine);

    tl::core::ItemAICharacter me;
    me.at = player.position();
    me.radius = player.body().radius;
    me.walking = player.following();
    // `validLineOfSight(level, true)` towards `+0x350`:
    // level, at 1.5 above the character - the item's y is never loaded.
    // Through `sightUnobstructed`, `CLevel::rayCollision`
    // skips the units' own models.
    {
        const tl::core::Vec3 from{me.at.x, me.at.y + 1.5f, me.at.z};
        const tl::core::Vec3 to{item.at.x, from.y, item.at.z};
        tl::core::CollisionHit hit;
        me.line_of_sight = collision.empty() || !collision.ray_collision(from, to, false, hit);
    }
    me.facing = tl::core::facing_target(player.facing(), me.at, item.at, player_stats.attack_angle);

    const tl::core::ItemAIStep step = tl::core::item_ai(item, me);
    using Step = tl::core::ItemAIStep;
    switch (step) {
        case Step::Wait: return;
        case Step::Walk: {
            // `setDestination(item)` and vtable +0x268; no route: `setTargetItem(nullptr)`.
            std::vector<tl::core::Vec3> route;
            if (nav.ready()) route = nav.find_path(me.at, item.at, passability);
            if (route.empty()) return let_go("no route");
            player.follow(std::move(route));
            return;
        }
        case Step::GiveUp: return let_go("given up");
        case Step::Attack:
            // A breakable container: no item pass offers one (the breakables
            // are the swing's).
            return let_go("a breakable container");
        case Step::Turn: {
            // Stopped; `updateMotion` turns it (`turnToward`).
            player.stop_following();
            const float dx = item.at.x - me.at.x;
            const float dz = item.at.z - me.at.z;
            player.face(
                tl::core::turn_toward(player.facing(), dx, dz, player_stats.turn_rate, delta));
            return;
        }
        case Step::Take:
            player.stop_following();
            player_item = PlayerItemTarget{};
            item_targeted = dropped_index;
            if (dropped_index >= 0) {
                pick_up_targeted_item();
            } else {
                TL_LOG_INFO("pad item: a prop that is not interactable is not taken");
            }
            return;
        case Step::Interact:
        case Step::Fish:
        case Step::EnchantShrine:
        case Step::StatShrine:
        case Step::GemShrine:
            // `interact_targeted_trigger` is the rest: fishing, a shrine's state 0x16, and
            // the prop's own `interact`.
            player.stop_following();
            {
                const PlayerItemTarget used = player_item;
                player_item = PlayerItemTarget{};
                if (used.kind == Kind::Portal) {
                    portal_targeted = portal_index;
                    use_targeted_portal();
                } else if (used.kind == Kind::Trigger) {
                    trigger_targeted_machine = used.machine;
                    trigger_targeted_node = used.node;
                    interact_targeted_trigger();
                }
            }
            return;
    }
}

}  // namespace tl::render
