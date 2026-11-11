// The pet's items and spells: A on the pet's tray of the bag screen, the
// spell learn and replace dialog,
// and what the pet holds and regenerates.
//
#include "render/renderer_impl.h"

namespace tl::render {

namespace {

/// AI states 0x29 (`returnToTownAI`) and 0x2a (away) - the pet going or gone.
bool pet_away(const tl::core::Pet& pet) {
    return pet.state() == tl::core::PetState::Departing ||
           pet.state() == tl::core::PetState::InTown;
}

}  // namespace

tl::core::TrayCaptionContext Renderer::Impl::tray_caption_context(const tl::core::ItemStats& item,
                                                                  bool pet_tray) {
    ensure_effect_data();
    load_unit_types();
    tl::core::TrayCaptionContext owner;
    owner.owner_is_player = !pet_tray;
    owner.owner_is_pet = pet_tray;
    // The bag's owner is the player, the pet's tray's the player's first pet.
    owner.owner_is_players_own = true;
    owner.owner_away = pet_tray && pet_away(pet);
    owner.identify_scrolls = player_inventory.count_by_name("Identify Scroll");
    if (pet_tray) refresh_pet_effects();
    const tl::core::ItemUseTarget target = pet_tray ? pet_use_target() : player_use_target();
    owner.learnable = tl::core::spell_learnable(item, item.uses, target, unit_types, stat_graphs);
    owner.can_use = tl::core::can_use_on_target(item, target, unit_types, stat_graphs);
    return owner;
}

// codes 3 and 4 on the focused tray: its timer to 0.001
// (0x3a83126f), `NAVIGATE`, the switch flipped, the tray refreshed, and the
// selected item's window asked for.
void Renderer::Impl::toggle_item_compare() {
    item_tip_timer = 0.001f;
    play_ui(tl::core::InterfaceSound::Navigate);
    item_compare = !item_compare;
    inventory_menu.set_item_compare(item_compare);
    update_menu();
    show_item_tooltips();
}

// The focused tray's part, once a frame while no dialog has
// the pad.
void Renderer::Impl::step_item_tooltips(float dt) {
    int key = -1;
    const bool vendor_screen = inventory_menu.vendor() != VendorScreen::None;
    const InventoryMenu::Tray tray = inventory_menu.focused_tray();
    if (vendor_screen && tray == InventoryMenu::Tray::Vendor) {
        const int slot = inventory_menu.selected_vendor_slot();
        if (slot >= 0) key = slot;
    } else if (inventory_menu.pet_tray_focused() ||
               (vendor_screen && tray == InventoryMenu::Tray::Pet)) {
        const int slot = inventory_menu.selected_pet_slot();
        if (slot >= 0) key = 1000 + slot;
    } else {
        const int slot = inventory_menu.selected_slot_index();
        if (slot >= 0) key = 2000 + slot;
    }
    if (item_compare) {
        // With the switch on the tray's refresh shows the
        // selected row's window as it goes.
        if (key != item_tip_key || !item_tip_shown) {
            item_tip_key = key;
            show_item_tooltips();
        }
        return;
    }
    if (key != item_tip_key) {
        // A new selection: 0.65 s (0x3f266666) before its window comes up.
        item_tip_timer = 0.65f;
        item_tip_key = key;
        item_tip_shown = false;
        inventory_menu.set_item_tooltips({});
    }
    if (0.0f < item_tip_timer && key != -1) {
        item_tip_timer -= dt;
        if (item_tip_timer <= 0.0f) show_item_tooltips();
    }
}

// `CGameUI::showEquipmentTooltip` for the focused
// tray's selected item and, with item compare on, for the worn items
// `CInventory::getComparisonItems` names.
void Renderer::Impl::show_item_tooltips() {
    const bool vendor_screen = inventory_menu.vendor() != VendorScreen::None;
    const InventoryMenu::Tray tray = inventory_menu.focused_tray();
    const tl::core::Inventory* held_in = &player_inventory;
    int slot = -1;
    tl::core::EquipmentTooltipContext context;
    if (vendor_screen && tray == InventoryMenu::Tray::Vendor) {
        held_in = inventory_menu.vendor_stock();
        slot = inventory_menu.selected_vendor_slot();
        context.owner_is_player = false;
        context.owner_is_merchant = inventory_menu.vendor() == VendorScreen::Merchant;
        context.owner_is_gambler = context.owner_is_merchant && inventory_menu.vendor_gambler();
    } else if (inventory_menu.pet_tray_focused() ||
               (vendor_screen && tray == InventoryMenu::Tray::Pet)) {
        held_in = &pet.bag();
        slot = inventory_menu.selected_pet_slot();
        context.owner_is_player = false;
    } else {
        slot = inventory_menu.selected_slot_index();
    }
    item_tip_shown = true;
    if (held_in == nullptr || slot < 0 || !held_in->occupied(static_cast<size_t>(slot))) {
        inventory_menu.set_item_tooltips({});
        return;
    }
    context.merchant_open = inventory_menu.vendor() == VendorScreen::Merchant;
    inventory_menu.set_item_tooltips(item_tooltips(*held_in, slot, context, item_compare));
}

/// The details of the item in `slot` of `held_in` and, with `compare`, of the worn items
/// it stands against. `context` says whose the item is and whether a merchant is open.
std::vector<tl::core::EquipmentTooltip> Renderer::Impl::item_tooltips(
    const tl::core::Inventory& held_in, int slot, tl::core::EquipmentTooltipContext context,
    bool compare) {
    std::vector<tl::core::EquipmentTooltip> tips;
    ensure_effect_data();
    load_unit_types();
    refresh_skill_effects();
    const tl::core::TrayRowContext player = tray_row_context();
    const float bonus = player_total().effect_value(tl::core::item_effect::kPriceBonus);
    context.unit_types = &unit_types;
    context.effects = &item_effect_context();
    context.display = &skill_effects;
    context.colours = &item_label_colours;
    context.sets = &item_sets;
    context.owner_inventory = &held_in;
    context.reductions = player.reductions;
    context.level = player.level;
    context.strength = player.strength;
    context.dexterity = player.dexterity;
    context.magic = player.magic;
    context.defense = player.defense;
    context.gold = player_inventory.gold();
    context.price_bonus = &bonus;
    // `skillDescription`'s second half: `CSkill::getDescription(skill, item,
    // -1)` for each of the item's skills that is enabled (`+0x6d`) and not
    // `HIDDEN` (`+0x6b`), at the level it was given.
    context.describe_skill = [this](const tl::core::ItemStats::Skill& given, std::string& text) {
        if (!given.enabled) return false;
        ensure_player_skill_files();
        const auto file = player_skill_files.find(tl::core::upper_ascii(given.name));
        if (file == player_skill_files.end() || file->second->def.hidden) return false;
        tl::core::KnownSkill known;
        known.entry.name = file->second->file.string_or("NAME", given.name);
        known.def = &file->second->def;
        known.invested = given.level;
        known.effective = given.level;
        text = tl::core::skill_description_text(known, -1, unit_types, skill_effects);
        return true;
    };

    const tl::core::InventorySlot& held = held_in.slots()[static_cast<size_t>(slot)];
    {
        tl::core::ItemStats copy = held.item;
        context.full_name = tl::core::item_full_name(copy, false, unit_types);
        context.count = held.count;
        tips.push_back(tl::core::equipment_tooltip(held.item, context));
    }

    const bool wearable = tl::core::item_isa(held.item, tl::core::unit_type::kWeapon, unit_types) ||
                          tl::core::item_isa(held.item, tl::core::unit_type::kArmor, unit_types) ||
                          tl::core::item_isa(held.item, tl::core::unit_type::kTrinket, unit_types);
    if (compare && wearable) {
        const int own_slot = &held_in == &player_inventory ? slot : -1;
        int first = -1, second = -1;
        player_inventory.comparison_items(held.item, unit_types, own_slot, first, second);
        for (const int location : {first, second}) {
            if (location < 0) continue;
            const tl::core::InventorySlot& worn =
                player_inventory.slots()[static_cast<size_t>(location)];
            tl::core::EquipmentTooltipContext worn_context = context;
            worn_context.owner_is_player = true;
            worn_context.owner_is_merchant = false;
            worn_context.owner_is_gambler = false;
            worn_context.comparison = true;
            worn_context.owner_inventory = &player_inventory;
            worn_context.worn_location = location;
            worn_context.count = worn.count;
            tl::core::ItemStats copy = worn.item;
            worn_context.full_name = tl::core::item_full_name(copy, false, unit_types);
            tips.push_back(tl::core::equipment_tooltip(worn.item, worn_context));
        }
    }
    return tips;
}

void Renderer::Impl::pet_tray_press() {
    // the tray's owner (`+0xc0`), its equipment (`+0x188`)
    // and the selected item.
    if (!pet.alive() || !pet.in_world()) return;
    const int slot = inventory_menu.selected_pet_slot();
    if (slot < 0 || !pet.bag().occupied(static_cast<size_t>(slot))) return;
    load_unit_types();
    ensure_effect_data();
    refresh_inventory_effects();
    const size_t index = static_cast<size_t>(slot);
    const tl::core::ItemStats item = pet.bag().slots()[index].item;
    switch (tl::core::tray_press(item, index, unit_types)) {
        case tl::core::TrayPress::Learn:
            // on the owner first: a spell it cannot learn at all
            // does nothing.
            if (tl::core::spell_learnable(item, item.uses, pet_use_target(), unit_types,
                                          stat_graphs)) {
                learn_spell_from_tray(/*pet_owner=*/true, index);
            } else {
                TL_LOG_INFO("pet tray: the pet cannot learn '%s'", item.display_name.c_str());
            }
            break;
        case tl::core::TrayPress::UseOnOwner: use_pet_item(index); break;
        case tl::core::TrayPress::FeedPet:
            // `canUse(item, player, first pet)`, the use and the spend.
            feed_pet_from(pet.bag(), index);
            break;
        case tl::core::TrayPress::TownPortal:
            // On the owner, then the screen closes. A pet is refused a SCROLL (NOPETS); the
            // refusal queues slot 0x33 on the owner's bank.
            if (!tl::core::can_use_on_target(item, pet_use_target(), unit_types, stat_graphs)) {
                const std::string& line =
                    pet.stats().sounds[tl::core::SoundSlot::VoiceCantCastHere];
                queue_global_sample(tl::core::SoundSlot::VoiceCantCastHere, line, 0.0f, 0.1f);
                TL_LOG_INFO("pet tray: %s refused", item.display_name.c_str());
                break;
            }
            if (use_pet_item(index)) {
                inventory_menu.hide();
                play_ui(tl::core::InterfaceSound::MenuClose);
                sync_stats_page();
                return;
            }
            break;
        case tl::core::TrayPress::Identify: identify_item_in(pet.bag(), index); break;
        case tl::core::TrayPress::Wear:
        case tl::core::TrayPress::TakeOff: wear_pet_item(index); break;
        case tl::core::TrayPress::Nothing: break;
    }
    update_menu();
    refresh_game_hud();
}

bool Renderer::Impl::use_item_on_pet(const tl::core::ItemStats& item, int stack, bool by_player,
                                     tl::core::ItemUseResult& used) {
    if (!pet.alive() || pet_node_id == 0) return false;
    load_unit_types();
    ensure_effect_data();
    refresh_pet_effects();
    tl::core::ItemUseTarget target = pet_use_target();
    // `canUse(item, user, pet)`.
    if (!tl::core::can_use_on_target(item, target, unit_types, stat_graphs)) {
        TL_LOG_INFO("pet: %s refused", item.display_name.c_str());
        return false;
    }
    // `useOnTarget(item, user, pet)`.
    int uses = item.uses;
    used = tl::core::use_on_target(item, stack, uses, target, unit_types, stat_graphs);
    // `POTIONS USED` is the user's statistic, counted for an `ISA
    // 0x21` item alone: the pet counts nothing, and no fish is a potion.
    // `USE_SOUND` (slot 0x14 of the item's bank) at the user's node.
    if (used.play_use_sound) {
        Ogre::SceneNode* user = by_player ? subject_node : pet_node;
        if (user != nullptr) play_sound(item.use_sound, user->getPosition());
    }
    TL_LOG_INFO("pet: %s used on it by %s, %d effect(s)", item.display_name.c_str(),
                by_player ? "the player" : "the pet", used.applied);
    // OPEN TOWN PORTAL: for a pet CCharacter::openPortal is empty; only CPlayer
    // opens one.
    if (used.open_town_portal || !used.open_map_portals.empty()) {
        // `openMapPortal` (X360 vtable +0x1c4) is `CPlayer`'s alone as well.
        TL_LOG_INFO("pet: %s used on the pet; a pet's openPortal opens nothing",
                    item.display_name.c_str());
    }
    if (!used.learned_spell.empty()) {
        // `applyInstantEffect`: global slot 0x41
        // (`VOICESPELLLEARNED_SOUND`) is queued for `isPlayer()` alone; a pet
        // learns in silence.
        pet.skills().set_effect_managers(pet_total(), nullptr);
        give_pet_spells_to_script();
        TL_LOG_INFO("pet: the pet learned the spell '%s' (slots: '%s', '%s')",
                    used.learned_spell.c_str(), pet.skills().spells[0].c_str(),
                    pet.skills().spells[1].c_str());
    }
    // `applyInstantEffect`'s `TRANSFORM` and `TRANSFORM PERMANENT`:
    // the pet's `transformCharacter`, which re-makes its character
    // where it stands (`pet_converted` hears the scene's half).
    for (const tl::core::ItemUseResult::Transform& transform : used.transforms) {
        index_unit(transform.unit);
        std::vector<tl::core::ScriptEvent> events;
        if (script.transform_unit(pet_node_id, transform.unit, transform.duration,
                                  quest_log.state(), events)) {
            TL_LOG_INFO("pet: %s turns it into %s for %.0f s", item.display_name.c_str(),
                        transform.unit.c_str(), static_cast<double>(transform.duration));
        }
        if (!events.empty()) apply_script_events(events, scripted_nodes);
    }
    return true;
}

bool Renderer::Impl::wear_pet_item(size_t slot) {
    // the function's equipment branch, the pet as owner (canEquip: trinkets only); what
    // has no room drops at the pet.
    if (slot >= pet.bag().slots().size() || !pet.bag().occupied(slot)) return false;
    load_unit_types();
    const tl::core::ItemStats item = pet.bag().slots()[slot].item;
    const std::string name = tl::core::item_name(item);
    const bool worn = tl::core::Inventory::is_equip_location(slot);
    // A pet answers to no requirements (`CCharacter +0x4a0` is the player's).
    const tl::core::Inventory::WearOutcome out =
        worn ? pet.bag().take_off_item(slot, unit_types)
             : pet.bag().wear_item(slot, unit_types, nullptr, &pet.effects());
    const tl::core::Vec3& at = pet.position();
    const Ogre::Vector3 where(at.x, at.y, at.z);
    for (const tl::core::ItemStats& sounded : out.sounded) play_sound(sounded.land_sound, where);
    for (const tl::core::InventorySlot& dropped : out.dropped) {
        drop_item(dropped.item, dropped.count, where);
    }
    if (out.equipped) {
        TL_LOG_INFO("pet tray: '%s' worn by the pet at location %d", name.c_str(), out.location);
    } else if (out.taken_off) {
        TL_LOG_INFO("pet tray: '%s' taken off the pet%s", name.c_str(),
                    out.dropped.empty() ? "" : ", onto the floor: its bag is full");
    } else {
        TL_LOG_INFO("pet tray: '%s' is not worn by the pet", name.c_str());
    }
    if (out.equipped || out.taken_off) refresh_pet_effects();
    return out.equipped || out.taken_off;
}

bool Renderer::Impl::open_spell_remove(bool pet_owner, size_t slot) {
    // the tray's `+0x17c` = `CGameUI +0x1898`, the dialog's
    // owner (`+0x2c`) the tray's, its choice (`+0x38`) 0 once `vfunc_3(1)` has
    // run.
    spell_remove_pet = pet_owner;
    spell_remove_slot = slot;
    spell_remove_choice = 0;
    refresh_spell_remove();
    if (!inventory_menu.open_spell_remove()) return false;
    TL_LOG_INFO("spell remove: open for the %s (scroll at slot %zu)", pet_owner ? "pet" : "player",
                slot);
    update_menu();
    return true;
}

void Renderer::Impl::refresh_spell_remove() {
    // `vfunc_5`: the choice wrapped, then the four rows.
    spell_remove_choice = tl::core::wrap_spell_remove_choice(spell_remove_choice, spell_remove_pet);
    const tl::core::SkillBook& book = spell_remove_pet ? pet.skills() : player_skills;
    std::array<tl::core::SpellRemoveRow, 4> rows;
    for (size_t i = 0; i < rows.size(); ++i) {
        rows[i] =
            tl::core::spell_remove_row(static_cast<int>(i), spell_remove_pet, book.known_spell(i),
                                       spell_remove_choice == static_cast<int>(i));
    }
    inventory_menu.set_spell_remove_rows(rows);
}

void Renderer::Impl::handle_spell_remove(const tl::core::InputState& in, bool x_edge, bool y_edge,
                                         bool start_edge, bool back_edge, bool other_edge) {
    // `CMessageSpellRemoveXenon::vfunc_2`, by the pad's code: the
    // stick's left and right (0x13, 0x14) once per push.
    int code = -1;
    if (std::abs(in.move_x) > 0.5f) {
        if (!spell_remove_stick_x_held) code = in.move_x > 0.0f ? 0x14 : 0x13;
        spell_remove_stick_x_held = true;
    } else {
        spell_remove_stick_x_held = false;
    }
    if (code < 0) {
        if (in.confirm && !confirm_held) {
            code = 5;
        } else if (x_edge) {
            code = 7;
        } else if (y_edge) {
            code = 8;
        } else if (in.skip && !skip_held) {
            code = 6;
        } else if (start_edge) {
            code = 0xd;
        } else if (back_edge) {
            code = 0xe;
        } else if (other_edge) {
            code = 9;
        }
    }
    if (code < 0) return;

    switch (tl::core::spell_remove_command_for_code(code)) {
        case tl::core::SpellRemoveCommand::Ignore: return;
        case tl::core::SpellRemoveCommand::Left:
        case tl::core::SpellRemoveCommand::Right:
            spell_remove_choice +=
                tl::core::spell_remove_command_for_code(code) == tl::core::SpellRemoveCommand::Left
                    ? -1
                    : 1;
            refresh_spell_remove();
            update_menu();
            return;
        case tl::core::SpellRemoveCommand::Cancel: spell_remove_choice = -1; break;
        case tl::core::SpellRemoveCommand::Confirm: break;
    }
    // `vfunc_3(0)` closes it; the tray's poll finds it down.
    inventory_menu.close_spell_remove();
    if (spell_remove_choice != -1) {
        // `unLearnSpell(owner, choice)`, then with the
        // tray's selected item again.
        tl::core::SkillBook& book = spell_remove_pet ? pet.skills() : player_skills;
        const std::string removed = book.spells[static_cast<size_t>(spell_remove_choice)];
        book.unlearn_spell(static_cast<size_t>(spell_remove_choice));
        // ... which takes the skill off the owner's manager.
        if (spell_remove_pet && pet_node_id != 0) script.remove_unit_skill(pet_node_id, removed);
        TL_LOG_INFO("spell remove: the %s forgets '%s'", spell_remove_pet ? "pet" : "player",
                    removed.c_str());
        learn_spell_from_tray(spell_remove_pet, spell_remove_slot);
        if (!spell_remove_pet) player_skills.set_effect_managers(player_total(), nullptr);
    } else {
        TL_LOG_INFO("spell remove: cancelled");
    }
    update_menu();
    refresh_game_hud();
}

}  // namespace tl::render
