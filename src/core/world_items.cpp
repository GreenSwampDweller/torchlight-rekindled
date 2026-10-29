// The gameplay half of render/renderer_items.cpp (core/world.h).
#include "core/equipment_attach.h"
#include "core/item_ai.h"
#include "core/targeting.h"
#include "core/world.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tl::core {

void World::load_recipes() {
    if (recipes_loaded) return;
    recipes_loaded = true;
    load_unit_types();
    // `CRecipes::reload`: one recipe per file.
    std::vector<tl::formats::AdmFile> files;
    for (const std::string& path : data_paths_under("media/recipes/")) {
        const std::vector<uint8_t> bytes = read_resource(path);
        if (bytes.empty()) continue;
        tl::formats::AdmFile file = parse_data_group(bytes.data(), bytes.size());
        if (file.ok) files.push_back(std::move(file));
    }
    std::vector<const tl::formats::AdmFile*> pointers;
    for (const tl::formats::AdmFile& file : files) pointers.push_back(&file);
    recipes.reload(pointers, unit_types);
    TL_LOG_INFO("items: %zu recipe(s)", recipes.size());
}

// PORT: the level as `PetDepartureLevel` reads it, from the port's level state.
tl::core::PetDepartureLevel World::pet_departure_level() const {
    tl::core::PetDepartureLevel level;
    // A layout loaded on its own is no dungeon's level and has no template.
    level.has_template = !level_dungeon.empty();
    level.allow_pet_return = level_allows_pet_return;
    level.is_town = level_is_town;
    return level;
}

void World::transmute_selected() {
    const auto found = combiners.find(combining_with);
    if (found == combiners.end()) return;
    load_unit_types();
    ensure_item_index();
    load_recipes();
    if (!loot_tables_built) {
        loot_tables_built = true;
        build_spawn_context(loot_tables, /*items_by_type=*/true);
    }
    tl::core::CombineContext context;
    context.recipes = &recipes;
    context.unit_types = &unit_types;
    context.items = &items;
    context.spawn_class = loot_tables.context.get_spawn_class;
    context.create_unit = loot_unit_maker();
    // `rollSpawnClass(player, player, player level +0xf0, -1, 0)`.
    context.player.level = player_character.level();
    context.player_level = player_character.level();
    context.spawn_context = &loot_tables.context;
    context.game_rng = &tl::core::game_random();

    const tl::core::CombineOutcome out =
        tl::core::transmute(found->second.inventory, player_inventory, context);
    if (out.sample == tl::core::kCombineNoSample) {
        TL_LOG_INFO("transmute: no recipe matches");
        return;
    }
    // `Reveal` on the interface bank, `VOICEFORTUNEGOOD` 0.1 s on.
    play_ui(tl::core::InterfaceSound::Reveal);
    if (out.owner_sample) play_voice(tl::core::SoundSlot::VoiceFortuneGood, 0.1f);
    // A leftover the bag would not take: `CLevel::addItem` at the player and
    // `drop()`.
    for (const tl::core::InventorySlot& left : out.dropped) {
        drop_item_near_player(left.item, left.count);
    }
    // CCombineMenu::performInteraction: incrementJournalStatistic(16, 1)
    if (out.journal_counted) count_journal(tl::core::JournalStatistic::ItemsTransmuted);
    std::string made;
    for (int slot : out.result_slots) {
        if (!made.empty()) made += ", ";
        made += found->second.inventory.slots()[static_cast<size_t>(slot)].item.name;
    }
    TL_LOG_INFO("transmute: %zu ingredient(s) used up, %zu returned to the floor, made '%s'%s",
                out.consumed.size(), out.dropped.size(), made.c_str(),
                out.unknown_spawn_class ? " (unknown spawn class)" : "");
    sync_equipped_weapon();
    update_menu();
    update_character_preview(0.0f);
    refresh_game_hud();
}

void World::restore_combiner(const tl::formats::CharacterSaveState& saved) {
    // `CCharacter::applySaveState`: saved items back in.
    load_unit_types();
    const tl::core::UnitStats stats = stats_for_unit(saved.name);
    if (!unit_types.isa(unit_types.id_of(stats.unittype), tl::core::unit_type::kCombiner)) return;
    CombinerState state;
    state.unit = saved.name;
    const std::string key = level_dungeon + ":" + std::to_string(level_depth);
    const auto remembered = remembered_combiners.find(key);
    if (remembered != remembered_combiners.end()) {
        const auto kept = remembered->second.find(saved.object_id);
        if (kept != remembered->second.end()) {
            state.inventory = kept->second.inventory;
            combiners[saved.object_id] = std::move(state);
            return;
        }
    }
    ensure_item_index();
    for (const tl::formats::ItemSaveState& item : saved.inventory) {
        const tl::core::ItemStats* data = items.find(item.name);
        if (data == nullptr) continue;
        const tl::core::ItemStats copy = create_item(*data, true, true);
        const int count = std::max(1, item.value);
        if (item.slot < 0 || !state.inventory.pickup_into_slot(copy, count, item.slot)) {
            state.inventory.pickup(copy, count, unit_types);
        }
    }
    combiners[saved.object_id] = std::move(state);
}

void World::trigger_range_collision(int64_t key, bool active) {
    // `CTriggerUnit::update`: with `COLLIDEABLE`,
    // `+0x19c = +0x199 && !+0x218`.
    if (!collision.has_group(key) || trigger_not_collidable.count(key) != 0) return;
    if (!active) {
        if (collision.group_enabled(key)) {
            collision.set_group_enabled(key, false);
            trigger_range_off.insert(key);
        }
    } else if (trigger_range_off.erase(key) > 0) {
        collision.set_group_enabled(key, true);
    }
}

// PORT: the player's held items as `HeldHands` reads them (`calculateCombatStats`).
tl::core::HeldHands World::player_held_hands() {
    load_unit_types();
    tl::core::HeldHands hands;
    hands.using_weapon = has_weapon;
    const auto& slots = player_inventory.slots();
    const auto read = [&](int location, tl::core::HeldHand& hand) {
        if (static_cast<size_t>(location) >= slots.size() || slots[location].count <= 0) return;
        const tl::core::ItemStats& item = slots[static_cast<size_t>(location)].item;
        const uint32_t type = unit_types.id_of(item.unittype);
        hand.held = true;
        hand.weapon = unit_types.isa(type, tl::core::unit_type::kWeapon);
        hand.ranged = unit_types.isa(type, tl::core::unit_type::kRangedWeapon);
        hand.melee = unit_types.isa(type, tl::core::unit_type::kMelee);
        // `calculateCombatStats` gives either hand's attack the item's own `RANGE`.
        hand.range = item.range;
    };
    read(tl::core::equip_location::kRightHand, hands.right);
    read(tl::core::equip_location::kLeftHand, hands.left);
    return hands;
}

void World::select_player_attack(int mode) {
    // the player's attack with more than 0.2 s of swing left is kept.
    if (swing.active && 0.2f < (swing.length - swing.elapsed) / swing_rate()) return;
    const tl::core::HeldHands hands = player_held_hands();
    const tl::core::AttackHand chosen = tl::core::select_attack_hand(hands, mode, alternate_hand);
    if (chosen == tl::core::AttackHand::Own) {
        if (has_weapon) TL_LOG_INFO("the character attacks bare-handed");
        has_weapon = false;
        attack_location = -1;
        attack_class = "Attack";
        return;
    }
    const bool right = chosen == tl::core::AttackHand::Right;
    const int location =
        right ? tl::core::equip_location::kRightHand : tl::core::equip_location::kLeftHand;
    const tl::core::ItemStats& item = player_inventory.slots()[static_cast<size_t>(location)].item;
    if (has_weapon && attack_location == location && weapon.name == item.name) return;
    weapon = item;
    has_weapon = true;
    attack_location = location;
    attack_class = tl::core::attack_animation_class(
        weapon.unittype, right ? tl::core::Hand::Right : tl::core::Hand::Left);
    TL_LOG_INFO("the character attacks with %s in the %s hand ('%s')", weapon.display_name.c_str(),
                right ? "right" : "left", attack_class.c_str());
}

void World::sync_equipped_weapon() {
    // The right hand (`equip_location::kRightHand`, slot 0), then the left (`kLeftHand`):
    // the two locations `CInventory` lets a `Weapon` into.
    const auto& slots = player_inventory.slots();
    const tl::core::ItemStats* eq_weapon = nullptr;
    int weapon_location = -1;
    if (slots[tl::core::equip_location::kRightHand].count > 0) {
        eq_weapon = &slots[tl::core::equip_location::kRightHand].item;
        weapon_location = tl::core::equip_location::kRightHand;
    } else if (slots[tl::core::equip_location::kLeftHand].count > 0) {
        eq_weapon = &slots[tl::core::equip_location::kLeftHand].item;
        weapon_location = tl::core::equip_location::kLeftHand;
    }

    if (eq_weapon != nullptr) {
        equip_weapon_item(*eq_weapon, weapon_location);
    } else {
        unequip_weapon();
    }
    sync_worn_attachments(weapon_location);
    // `CCharacter::updateAI`'s `selectAttack(0)` settles the hand.
    select_player_attack(0);
    // `CCharacter::equipmentEquipped` / `equipmentUnequipped`
    // call `CWardrobe::update`.
    dress_player();
}

// PORT: the level's quest units placed through `QuestLog::populate_level`.
void World::populate_quests() {
    load_quest_book();
    quest_log.populate_level(
        level_dungeon, level_depth,
        [this](const tl::core::QuestEntry& entry, const tl::core::QuestObjective& objective,
               const tl::core::QuestUnitTag& tag) {
            create_quest_unit(objective, tag,
                              entry.definition != nullptr ? entry.definition->guid : -1);
        });
}

// PORT: an item's quest tag from its quest GUID, through the port's quest book.
tl::core::QuestUnitTag World::quest_tag_of_item(const tl::core::ItemStats& item) {
    if (item.quest_guid == -1) return {};
    load_quest_book();
    tl::core::QuestUnitTag tag;
    tag.objective = item.quest_unit_id;
    if (const tl::core::QuestDefinition* quest = quest_book.find_by_guid(item.quest_guid)) {
        tag.quest = quest->name;
        return tag;
    }
    // A guid the book has not got is still another quest's.
    tag.quest = "#" + std::to_string(item.quest_guid);
    return tag;
}

// PORT: a quest tag's GUID, through the port's quest book.
int64_t World::quest_guid_of(const tl::core::QuestUnitTag& tag) {
    if (tag.empty()) return -1;
    load_quest_book();
    const tl::core::QuestDefinition* quest = quest_book.find(tag.quest);
    return quest != nullptr ? quest->guid : -1;
}

// PORT: the bags' item events wired to the renderer.
void World::wire_bag_item_events() {
    bag_item_events.added = [this](tl::core::ItemStats& item) { bag_item_added(item); };
    bag_item_events.removed = [this](const tl::core::ItemStats& item) { bag_item_removed(item); };
    housing_item_events.removed = bag_item_events.removed;
    player_inventory.set_item_events(&bag_item_events);
    pet.bag().set_item_events(&bag_item_events);
}

void World::load_unit_themes() {
    if (unit_themes_loaded) return;
    unit_themes_loaded = true;
    // `CUnitThemes::reload` over `media/unitthemes/*.dat`.
    // No two share a `NAME`, so the listing's order decides nothing.
    std::vector<tl::formats::AdmFile> files;
    for (const std::string& path : data_paths_under("media/unitthemes/")) {
        const std::vector<uint8_t> bytes = read_resource(path);
        if (bytes.empty()) continue;
        tl::formats::AdmFile file = parse_data_group(bytes.data(), bytes.size());
        if (file.ok) files.push_back(std::move(file));
    }
    unit_themes.reload(files);
    TL_LOG_INFO("unit themes: %zu", unit_themes.size());
}

World::CombinerState* World::combiner_named(const std::string& unit) {
    load_unit_types();
    const auto range = scene_units.equal_range(unit);
    for (auto it = range.first; it != range.second; ++it) {
        const auto found = combiners.find(it->second.node);
        if (found != combiners.end()) return &found->second;
    }
    // The inventory every character has from its `unitInit`.
    const tl::core::UnitStats stats = stats_for_unit(unit);
    if (!unit_types.isa(unit_types.id_of(stats.unittype), tl::core::unit_type::kCombiner)) {
        return nullptr;
    }
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second.node == 0) continue;
        CombinerState& state = combiners[it->second.node];
        state.unit = unit;
        return &state;
    }
    return nullptr;
}

void World::clear_weapon_element() {
    destroy_particle_holders(weapon_element_particles);
    weapon_element_particles.clear();
}

void World::init_unit_themes(UnitThemeState& state, const tl::core::UnitStats& stats) {
    // `CBaseUnit::unitInitThemes`, from
    // `CCharacter::unitInit`; here when first wanted.
    if (state.initialised) return;
    state.initialised = true;
    load_unit_themes();
    state.themes.init_themes(stats.themes_root, &unit_themes);
}

// What `CItem::calculateActiveRange` reads
// (`ItemActiveRangeInput`): ground distance, height between, the level's ranges.
tl::core::ItemActiveRangeInput World::item_range_input(const ogre::Vector3& at, bool interactable,
                                                       bool casts_shadows) const {
    const ogre::Vector3 view = item_view_point();
    tl::core::ItemActiveRangeInput in;
    in.distance =
        std::sqrt((view.x - at.x) * (view.x - at.x) + 0.0f + (view.z - at.z) * (view.z - at.z));
    in.height_difference = view.y - at.y;
    in.interactable = interactable;
    in.light_fade = level_light_fade;
    in.indoor_range = level_indoor_range;
    in.outdoor_range = level_outdoor_range;
    in.trigger_near_range = level_trigger_near_range;
    in.unit_near_range = level_unit_near_range;
    in.casts_shadows = casts_shadows;
    in.unit_shadow_range = level_unit_shadow_range;
    return in;
}

std::vector<World::TriggerItem> World::level_trigger_items() {
    std::vector<TriggerItem> out;
    load_unit_types();
    const std::vector<MachineView> views = machines();
    for (size_t v = 0; v < views.size(); ++v) {
        const MachineView& view = views[v];
        for (const tl::core::LevelScript::UnitTriggerView& unit :
             view.script->every_unit_trigger()) {
            TriggerItem item;
            item.key = instance_key(unit.id, view.scene, view.hierarchy);
            item.machine = static_cast<int>(v) - 1;
            item.node = unit.id;
            item.node_map = view.nodes;
            if (view.nodes != nullptr) {
                const auto range = view.nodes->equal_range(unit.id);
                for (auto it = range.first; it != range.second; ++it)
                    item.nodes.push_back(it->second);
            }
            // `setUnitModelName` adds the unit to the
            // level's items (`CLevel::addItem`) when a unit file names it.
            if (item.nodes.empty() && !collision.has_group(item.key)) {
                TriggerProp named;
                trigger_prop(unit.unit, named);
                if (!named.found) continue;
            }
            tl::core::Vec3 low, high;
            if (!item.nodes.empty()) {
                item.at = view_derived_position(item.nodes.front());
            } else if (collision.group_bounds(item.key, low, high)) {
                item.at = ogre::Vector3((low.x + high.x) * 0.5f, low.y, (low.z + high.z) * 0.5f);
            } else {
                item.at = ogre::Vector3(unit.at.x, unit.at.y, unit.at.z);
            }
            auto found = trigger_presence.find(item.key);
            if (found == trigger_presence.end()) {
                // `addItem`: opacity 1, on
                // the near list.
                TriggerPresence fresh;
                fresh.presence.opacity = 1.0f;
                TriggerProp prop;
                trigger_prop(unit.unit, prop);
                fresh.interactable = unit_types.isa(unit_types.id_of(prop.unittype),
                                                    tl::core::unit_type::kInteractable);
                fresh.shadows = prop.shadows;
                // `CTriggerUnit::unitInit`:
                // a `LAYOUTFILE` makes the trigger's own layout (`+0x240`).
                fresh.presence.own_layout = !prop_layout_path(unit.unit).empty();
                found = trigger_presence.emplace(item.key, fresh).first;
            }
            item.state = &found->second;
            out.push_back(std::move(item));
        }
    }
    return out;
}

void World::item_near_list() {
    // `CLevel::updateNearUnitList`: every item measured
    // again (`CItem::calculateActiveRange`); one past its near
    // range is hidden at once (`setVisible(false, true)`, `hideItemText`) and left off the
    // near list `updateItems` walks.
    load_unit_types();
    for (BreakableWorld& b : breakables) {
        if (b.node == nullptr) continue;
        tl::core::ItemPresenceChange change;
        const uint32_t type = unit_types.id_of(stats_for_unit(b.unit).unittype);
        b.listed = tl::core::item_near_list_entry(
            b.presence,
            item_range_input(view_position(b.node),
                             unit_types.isa(type, tl::core::unit_type::kInteractable),
                             b.data.shadows),
            change);
        b.state.active = b.presence.active;
        draw_item_presence(b.node, b.presence, change);
    }
    for (DroppedWorldItem& dropped : dropped_items) {
        if (dropped.node == nullptr) continue;
        tl::core::ItemPresenceChange change;
        dropped.listed = tl::core::item_near_list_entry(
            dropped.presence,
            item_range_input(view_position(dropped.node), false, dropped.item.shadows), change);
        draw_item_presence(dropped.node, dropped.presence, change);
    }
    // The `Unit Trigger`s (doors, chests, levers, shrines), each a `CItem`.
    const std::vector<TriggerItem> triggers = level_trigger_items();
    size_t triggers_listed = 0;
    size_t triggers_active = 0;
    for (const TriggerItem& item : triggers) {
        TriggerPresence& t = *item.state;
        tl::core::ItemPresenceChange change;
        t.listed = tl::core::item_near_list_entry(
            t.presence, item_range_input(item.at, t.interactable, t.shadows && !item.nodes.empty()),
            change);
        for (Ogre::SceneNode* node : item.nodes) draw_item_presence(node, t.presence, change);
        triggers_listed += t.listed ? 1 : 0;
        triggers_active += t.presence.active ? 1 : 0;
    }
    std::array<size_t, 3> counts{triggers_listed, triggers_active,
                                 breakables.size() + dropped_items.size() + triggers.size()};
    float nearest = -1.0f;
    const auto count = [&](bool listed, bool active, const Ogre::SceneNode* node) {
        counts[0] += listed ? 1 : 0;
        counts[1] += active ? 1 : 0;
        if (node == nullptr) return;
        const float d = item_range_input(view_position(node), false, false).distance;
        if (nearest < 0.0f || d < nearest) nearest = d;
    };
    for (const BreakableWorld& b : breakables) count(b.listed, b.presence.active, b.node);
    for (const DroppedWorldItem& d : dropped_items) count(d.listed, d.presence.active, d.node);
    if (counts != item_near_logged) {
        item_near_logged = counts;
        TL_LOG_INFO("items: near list %zu of %zu, %zu active, the nearest %.1f away", counts[0],
                    counts[2], counts[1], static_cast<double>(nearest));
    }
}

void World::step_level_items(float delta) {
    // `CLevel::updateItems` over the near list: `update`,
    // or `deleteItem` for one marked (`+0x190`).
    load_unit_types();
    for (size_t i = 0; i < breakables.size();) {
        BreakableWorld& b = breakables[i];
        if (b.presence.deleted) {
            // `CLevel::deleteItem`: off the
            // level's lists and freed; its spawner let go of it when it broke.
            TL_LOG_INFO("breakable: %s deleted, broken and out of range", b.unit.c_str());
            remove_breakable(b);
            breakables.erase(breakables.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (b.listed && b.node != nullptr) {
            // `CBreakable::update`: `CItem::update`,
            // then the collision model while active.
            tl::core::ItemPresenceChange change;
            b.presence.used = b.state.used;
            const uint32_t type = unit_types.id_of(stats_for_unit(b.unit).unittype);
            tl::core::update_item(
                b.presence,
                item_range_input(view_position(b.node),
                                 unit_types.isa(type, tl::core::unit_type::kInteractable),
                                 b.data.shadows),
                true, item_in_camera(b.node), delta, change);
            b.state.active = b.presence.active;
            draw_item_presence(b.node, b.presence, change);
            const bool was = b.state.collision_live;
            tl::core::breakable_update(b.state);
            if (b.has_collision && was != b.state.collision_live) {
                collision.set_group_enabled(b.id, b.state.collision_live);
            }
        }
        ++i;
    }
    for (DroppedWorldItem& dropped : dropped_items) {
        if (!dropped.listed || dropped.node == nullptr) continue;
        tl::core::ItemPresenceChange change;
        tl::core::update_item(
            dropped.presence,
            item_range_input(view_position(dropped.node), false, dropped.item.shadows), false,
            item_in_camera(dropped.node), delta, change);
        draw_item_presence(dropped.node, dropped.presence, change);
        step_dropped_item(dropped, delta);
    }
    // `CTriggerUnit::update`: `CItem::update`, then the
    // collision model by the range. Its layouts and animation queue are the port's own.
    for (const TriggerItem& item : level_trigger_items()) {
        TriggerPresence& t = *item.state;
        if (!t.listed) continue;
        bool seen = item.nodes.empty();
        for (Ogre::SceneNode* node : item.nodes) seen = seen || item_in_camera(node, true);
        tl::core::ItemPresenceChange change;
        tl::core::update_item(
            t.presence, item_range_input(item.at, t.interactable, t.shadows && !item.nodes.empty()),
            false, seen, delta, change);
        for (Ogre::SceneNode* node : item.nodes) draw_item_presence(node, t.presence, change);
        // `CLayout::setVisible(true)` on the trigger's layout: restarted only if hidden.
        if (change.own_layout_shown) view_restart_trigger_layout(item.node, item.node_map);
        trigger_range_collision(item.key, t.presence.active && !t.presence.used);
        // The rest of `update`: a state change that asked to be told (`+0x25d`) broadcasts
        // `Triggered` and the state.
        tl::core::LevelScript* owner = machine_script(item.machine);
        if (owner == nullptr) continue;
        std::vector<tl::core::ScriptEvent> events;
        owner->update_unit_trigger(item.node, quest_log.state(), events);
        if (events.empty()) continue;
        // A room's machine that had gone quiet has something to run again.
        if (item.machine >= 0) nested_scripts[static_cast<size_t>(item.machine)].running = true;
        apply_machine_events(item.machine, events);
    }
}

// A prop's trap: `CSkillManager::executeSkill` on the
// prop's own manager, as `CBreakable::interact` and
// `CTriggerUnit::trigger` run it; one the player noticed
// is `queueGlobalSample(0x36)` and journal statistic 10. The caller takes the prop's
// collision out around it.
void World::cast_prop_trap(const std::string& unit, const std::string& name, uint32_t level,
                           const ogre::Vector3& skill_at, const ogre::Quaternion& facing,
                           bool by_player) {
    ensure_player_skill_files();
    ensure_effect_data();
    const auto file = player_skill_files.find(tl::core::upper_ascii(name));
    if (file == player_skill_files.end()) {
        TL_LOG_WARN("prop: %s knows no skill '%s'", unit.c_str(), name.c_str());
        return;
    }
    tl::core::KnownSkill skill;
    skill.entry.name = file->second->file.string_or("NAME", name);
    skill.def = &file->second->def;
    skill.invested = level;
    skill.effective = level;
    tl::core::SkillCaster caster;
    caster.is_character = false;
    caster.unit_types = &unit_types;
    // The prop's own effect manager, which nothing reads again.
    tl::core::EffectManager own;
    tl::core::SkillCastContext context;
    context.target = &own;
    context.types = &effect_types;
    context.unit_types = &unit_types;
    context.affix = [this](const std::string& n) -> const tl::core::Affix* {
        return affixes.get_affix(n);
    };
    context.caster = &caster;
    context.on_event = [this, skill_at, facing](const tl::core::KnownSkill& cast, uint32_t, int,
                                                const tl::formats::AdmSection& event,
                                                const tl::core::SkillEventTarget&) {
        // `CSkillEvent::startEvent`: the block's layout
        // at the cast point, facing the prop's way. INVENTED: the damage shape's listener
        // (`CDamageShape +0x1c8`) is not wired, so an explosion deals its shape's damage only.
        const tl::core::SkillEventSpec spec = tl::core::read_skill_event(event);
        if (spec.file.empty()) return;
        const size_t before = nested_scripts.size();
        if (!place_layout(spec.file, skill_at, facing, /*depth=*/1)) {
            TL_LOG_WARN("prop: '%s' could not place '%s'", cast.entry.name.c_str(),
                        spec.file.c_str());
            return;
        }
        for (size_t i = before; i < nested_scripts.size(); ++i) {
            NestedScript& machine = nested_scripts[i];
            std::vector<tl::core::ScriptEvent> events;
            machine.running = true;
            machine.script.play_layout_timelines(quest_log.state(), events, /*backwards=*/false);
            if (!events.empty()) apply_script_events(events, machine.nodes);
        }
        TL_LOG_INFO("prop: '%s' places %s", cast.entry.name.c_str(), spec.file.c_str());
    };
    tl::core::SkillRunner runner;
    const bool executed = runner.execute(skill, level, caster, context);
    TL_LOG_INFO("prop: %s springs '%s': %s", unit.c_str(), skill.entry.name.c_str(),
                executed ? "cast" : "the chance failed");
    if (tl::core::breakable_trap_noticed(executed, by_player,
                                         static_cast<int32_t>(skill.def->type))) {
        // CBreakable::interact: incrementJournalStatistic(10, 1)
        play_voice(tl::core::SoundSlot::VoiceTrapSprung, 1.0f);
        count_journal(tl::core::JournalStatistic::TrapsSprung);
    }
}

/// Take life off the character.
void World::hurt_player(float amount, const std::string& from, int64_t attacker, bool struck,
                        bool shown) {
    if (amount <= 0.0f || player_health.dead) return;
    if (!(player_health.max > 0.0f)) return;
    // `applyDamage`: onto the number over the character (`+0x1ec`). INVENTED: a
    // glancing blow on the player is not told apart, so it is added too.
    if (shown) player_damage_shown.pending = amount + player_damage_shown.pending;
    // `CCharacter::applyDamage`: struck, and `+0x708` run out, the
    // `CAST SKILL ON STRUCK` procs roll (`executeProcs(0x51, attacker)`), ahead
    // of `modifyHP` and its god mode.
    if (struck && !(0.0f < player_proc_lockout)) {
        player_execute_procs(tl::core::kEffectCastSkillOnStruck, proc_target_attacker(attacker));
        if (player_health.dead) return;
    }
    // `CCharacter::modifyHP`: in god mode (vtable +0x290,
    // `CPlayer`) the health is set to `maxHP()` instead of changed.
    if (god_mode) {
        player_health.current = static_cast<float>(static_cast<int>(player_health.max));
        return;
    }
    const float before_hit = player_health.current;
    player_health.current -= amount;
    // `+0x52e`, `INVINCIBLE`: under 1 the health is put back to 1
    // and `POISON` taken off.
    if (player_invincible && player_health.current < 1.0f) {
        player_effects.remove_effect("POISON");
        player_health.current = 1.0f;
    }

    // `CCharacter::modifyHP`: `VOICEHEALTHLOW_SOUND` only on crossing 0.2 of
    // `maxHP()` downwards, half a second.
    const float low_water = player_health.max * tl::core::kVoiceHealthLowFraction;
    if (before_hit > low_water && player_health.current <= low_water) {
        play_voice(tl::core::SoundSlot::VoiceHealthLow, tl::core::kVoiceHealthLowDelay);
    }
    TL_LOG_INFO("combat: %s hits the character for %.1f, %.1f of %.1f left", from.c_str(),
                static_cast<double>(amount),
                static_cast<double>(std::max(0.0f, player_health.current)),
                static_cast<double>(player_health.max));

    // `CCharacter::modifyHP` at 0: `die` (vtable +0x330), `CPlayer::die` for the player.
    if (player_health.current <= 0.0f) {
        // Under 0, `forceDisplayOfDamageAbsorbed` first.
        if (player_health.current < 0.0f) display_player_damage(tl::core::kForcedDamageDisplay);
        player_health.current = 0.0f;
        kill_player(attacker);
    }
    // `applyDamage`: struck by an attacker, the
    // character's friends near it turn on the attacker (`CLevel::reactToAreaDamage`); its
    // own `reactToDamage` does nothing.
    if (struck && attacker != 0 && subject_node != nullptr) {
        if (tl::core::LevelScript* by = script_owning(attacker)) {
            const ogre::Vector3 here = view_position(subject_node);
            std::vector<tl::core::ScriptEvent> events;
            by->react_to_area_damage(attacker, by, nullptr, 0,
                                     tl::core::Vec3{here.x, here.y, here.z}, player_stats.radius,
                                     quest_log.state(), events);
            if (NodeMap* nodes = nodes_of(by); nodes != nullptr && !events.empty()) {
                apply_script_events(events, *nodes);
            }
        }
    }
    if (0.0f < player_health.current && attacker != 0) {
        // `applyDamage`: still alive, the character answers the
        // attacker with what it reflects.
        reflect_damage(player_total(), 0, "the character", amount, attacker);
    }
}

// CPlayer::die
void World::kill_player(int64_t attacker) {
    // `alive()`, and `CGameClient +0x38d0 == 1`: a game being played.
    if (player_death.dying || !player_control) return;
    // Dying of its effects, the attacker is whom they credit (`updateHP`).
    if (player_hurt_by_effects) attacker = player_effect_killer;

    // CCharacter::die, the parts a player reaches: no
    // `DEATH_FLIGHT` and no burst. INVENTED gap: `stopAllSkills`
    // and `fireSkillsOnDeath` are not run for the player.
    player_death.dying = true;  // setAIState(5)
    // CCharacter::die: incrementJournalStatistic(5, 1)
    count_journal(tl::core::JournalStatistic::Deaths);
    player_health.current = 0.0f;  // modifyHP(-HP)
    player_health.dead = true;
    swing = Swing{};                            // +0x378, +0x380, +0x267
    player_active_skill.animation_left = 0.0f;  // +0x380
    player_active_skill.casting = false;        // +0x267
    skill_playback = SkillPlayback{};
    if (subject_node != nullptr) {  // sample 0xc
        play_sound(player_stats.sounds[tl::core::SoundSlot::Death], view_position(subject_node));
    }
    player_target_node = 0;   // setTarget(0)
    player.stop_following();  // CPath::Clear
    for (MachineView& view : machines()) view.script->set_player_alive(false);
    // `executeProcs(0x69, attacker)`: the `CAST SKILL ON DEATH` effects.
    player_execute_procs(tl::core::kEffectCastSkillOnDeath, proc_target_attacker(attacker));
    // the death particle, by "Show Blood".
    if (subject_node != nullptr) {
        play_death_particle(player_stats, view_position(subject_node), CharacterRef{});
    }

    // `findRandomAnimation("DIE")`: the first match, then
    // each later one on `randomBetweenVolatile(0, 1000) < 500`. None: `clearAnimations`.
    player_death.animation.clear();
    if (const std::string mesh = view_entity_mesh(subject); !mesh.empty()) {
        for (const std::string& stem : manifest_for(mesh).order()) {
            const std::string upper = tl::core::upper_ascii(stem);
            if (upper.compare(0, 3, "DIE") != 0) continue;
            if (player_death.animation.empty() ||
                tl::core::volatile_random().between(0.0f, 1000.0f) < 500.0f) {
                player_death.animation = stem;
            }
        }
    }
    if (!player_death.animation.empty() && !link_subject_animation(player_death.animation)) {
        player_death.animation.clear();
    }
    TL_LOG_WARN("combat: the character is down (%s)", player_death.animation.empty()
                                                          ? "no DIE animation"
                                                          : player_death.animation.c_str());
    // `unitBroadcastMessage(1)`; dying of its effects, `updateHP` sends it
    // after its own 4 instead (`update_player_meters`).
    if (!player_hurt_by_effects)
        unit_observers.broadcast({nullptr, 0}, tl::core::unit_state::kDied);

    // A hardcore character is saved dead: `saveCharacter(true, true)`.
    if (config.hardcore && config.save_path != nullptr) save_character(*config.save_path, true);
}

// CDieMenu::onClick, CDieMenu::update
// and CGameClient::setGameState(5).
void World::choose_resurrection(tl::core::ResurrectChoice choice) {
    if (!death_screen_up || config.hardcore) return;
    const tl::core::DeathPenalty taken =
        tl::core::death_penalty(choice, stat_graphs, player_character.level(),
                                player_character.fame_level(), player_inventory.gold());
    death_screen_up = false;
    view_hide_death_screen();

    // `giveGold(-gold)`, `awardFame(-fame)`, `awardExperience(-experience)`, in that order.
    player_inventory.give_gold(-taken.gold);
    award_player_fame(-taken.fame);
    award_player_experience(-taken.experience, false);
    TL_LOG_INFO("resurrect (choice %d): lost %d xp, %d fame, %d gold", static_cast<int>(choice),
                taken.experience, taken.fame, taken.gold);

    if (choice == tl::core::ResurrectChoice::Town && !level_is_town) {
        // `CPlayer::levelResetting`, then `warpLevels("Town", -99)` (main.cpp's,
        // `Renderer::dungeon_transition`); `TOWNPORTALENTER` (`CGameClient::create`).
        //
        level_resetting_player(choice);
        play_ui(tl::core::InterfaceSound::TownPortalEnter);
        death_town_pending = true;
        dungeon_transition_pending = true;
        return;
    }
    // `CLevel::restartLevel`: `levelResetting` on every character, newest first
    // (`LevelScript::restart_level`). INVENTED: rooms last placed first, the level's last.
    std::vector<MachineView> views = machines();
    for (auto view = views.rbegin(); view != views.rend(); ++view) {
        std::vector<tl::core::ScriptEvent> events;
        view->script->restart_level(quest_log.state(), events);
        if (!events.empty()) apply_script_events(events, *view->nodes);
    }
    level_resetting_player(choice);
    if (automap_initialized && config.minimap) view_show_minimap();
}

// CPlayer::levelResetting
void World::level_resetting_player(tl::core::ResurrectChoice choice) {
    player_health.current = player_health.max;  // modifyHP(maxHP)
    player_health.dead = false;
    player_character.set_mp(player_character.max_mp(), player_character.max_mp());
    // `CGameUI +0x1928 == 1`: the level's start (`CLevel +0x140`), dropped to the
    // ground, turned toward `+0x164`.
    if (choice == tl::core::ResurrectChoice::Entrance && have_player_start) {
        float surface = player_start.y;
        collision.ground_under(player_start.x, player_start.z, player_start.y + 100.0f, surface);
        player.place({player_start.x, surface, player_start.z}, player_start_yaw);
        step_player(0.0f, tl::core::InputState{});
        // The start stands inside the entrance's triggers: each machine is told the player
        // is already there (`setPosition` enters none).
        const ogre::Vector3 at(player_start.x, surface, player_start.z);
        for (const MachineView& view : machines()) {
            view.script->arrive_at(tl::core::Vec3{at.x, at.y, at.z});
        }
    }
    // Each pet `teleportToMaster(count * 0.25)` and healed. INVENTED: put beside the player.
    if (pet.alive() && pet_node_id != 0) {
        float hp = 0.0f;
        float max = 0.0f;
        script.unit_health(pet_node_id, hp, max);
        script.set_unit_hp(pet_node_id, max);
        place_pet_beside_player();
    }
    // setAIState(0) and `IDLE`.
    player_death.dying = false;
    player_death.animation.clear();
    player_animation.clear();
    for (MachineView& view : machines()) view.script->set_player_alive(true);
    // With an effect manager: four affixes deleted by name,
    // three of them the monsters' spellings (the console's,
    // kept); then every effect not `SAVE`d (`removeNonSavedEffects`).
    for (const char* affix : {"DAMAGEEFFECTFROZENPLAYER", "DAMAGEEFFECTONFIRE",
                              "DAMAGEEFFECTELECTRICITY", "DAMAGEEFFECTPOISONED"}) {
        player_effects.delete_affix(affix);
    }
    player_effects.remove_non_saved_effects();
    const tl::core::Vec3 at = player.position();
    TL_LOG_INFO("resurrected with %.0f hit points at (%.2f, %.2f, %.2f)",
                static_cast<double>(player_health.current), at.x, at.y, at.z);
}

/// The Y legend's "Drop": the item leaves from where the character stands. INVENTED: the
/// caller setting the position (`+0x84`) before `drop` was not read.
void World::drop_item_near_player(const tl::core::ItemStats& item, int count) {
    if (subject_node == nullptr) return;
    drop_item(item, count, view_position(subject_node));
}

/// A mesh-less item (no item index entry) is not dropped.
void World::drop_item(const tl::core::ItemStats& item, int count, const ogre::Vector3& placed) {
    Ogre::Entity* entity = create_item_entity(item);
    if (entity == nullptr) return;

    // `addItem`: `setPosition`, then `CEquipment::setActiveInLevel`
    // grounds it before `drop` takes its position.
    const ogre::Vector3 at = snap_item_to_ground(placed);

    // `CEquipment::drop`. Draws on `g_RandVolatile`, in
    // order: heading, landing spot, the arc's rise.
    load_unit_types();
    const uint32_t type = unit_types.id_of(item.unittype);
    const float heading = tl::core::volatile_random().between(0.0f, 360.0f);
    const tl::core::DropTiltRotation tilt =
        tl::core::drop_tilt_rotation(tl::core::drop_tilt(unit_types, type));
    ogre::Quaternion tilted = ogre::Quaternion::IDENTITY;
    if (tilt.axis == 0)
        tilted = ogre::Quaternion(ogre::Radian(tilt.radians), ogre::Vector3::UNIT_X);
    if (tilt.axis == 2)
        tilted = ogre::Quaternion(ogre::Radian(tilt.radians), ogre::Vector3::UNIT_Z);
    // `matrixRotationY(heading)` times the tilt: laid down, then turned.
    const ogre::Quaternion rest =
        ogre::Quaternion(ogre::Radian(heading * 0.017453292f), ogre::Vector3::UNIT_Y) * tilted;

    const tl::core::Vec3 from{at.x, at.y, at.z};
    play_sound(item.fall_sound, at);  // slot 0x10, before the spot is chosen

    tl::core::Vec3 land = tl::core::random_open_item_position(
        from, 0.0f, tl::core::kDropTossRadius,
        [&](const tl::core::Vec3& p) {
            return !passability.ready() || passability.map_passable(p);
        },
        tl::core::volatile_random());
    // `CLevel::floorHeight`: a ray from 400 down to -100,
    // 0 when it meets nothing.
    const float floor = collision.floor_height(land.x, land.z);
    land.y = floor + tl::core::kDropFloorLift;
    if (unit_types.isa(type, tl::core::unit_type::kWeapon))
        land.y = land.y + tl::core::kDropWeaponLift;

    DroppedWorldItem dropped;
    dropped.item = item;
    dropped.count = count;
    dropped.entity = entity;
    dropped.arc = tl::core::make_drop_arc(from, land, tl::core::volatile_random());
    dropped.flying = true;
    dropped.travelled = 0.0f;
    dropped.spins = tl::core::drop_spins(unit_types, type);
    dropped.rest = rest;

    dropped.node = view_place_entity(entity, at, rest);
    // `CLevel::addItem` puts it in the level before `drop` tosses it, so its particle
    // rides the toss.
    start_item_particles(dropped);
    // `addItem`: `+0x204` to 1 (`updateOpacity(0, true)`).
    dropped.presence.opacity = 1.0f;
    dropped_items.push_back(dropped);
    TL_LOG_INFO("dropped %d x %s: tossed from (%.2f, %.2f, %.2f) to (%.2f, %.2f, %.2f), %.2f along",
                count, item.display_name.c_str(), static_cast<double>(from.x),
                static_cast<double>(from.y), static_cast<double>(from.z),
                static_cast<double>(land.x), static_cast<double>(land.y),
                static_cast<double>(land.z), static_cast<double>(dropped.arc.length));
}

ogre::Vector3 World::snap_item_to_ground(const ogre::Vector3& at) {
    // `CItem::snapToGround`: a ray from 100 above zero to
    // 100 below (`CLevel::rayCollision`); a hit not on `nocollide` (material 100) is the
    // spot. Else `randomOpenPosition(here, 4)` and again, thirty times, then the last point.
    const tl::core::Vec3 rest = tl::core::snap_item_to_ground(
        {at.x, at.y, at.z},
        [&](const tl::core::Vec3& from, const tl::core::Vec3& to, tl::core::Vec3& point,
            uint32_t& material) {
            tl::core::CollisionHit hit;
            if (!collision.ray_collision(from, to, false, hit)) return false;
            point = hit.point;
            material = static_cast<uint32_t>(hit.material);
            return true;
        },
        [&](const tl::core::Vec3& p) {
            return !passability.ready() ||
                   (passability.map_passable(p) && passability.position_passable(p));
        },
        tl::core::volatile_random());
    return ogre::Vector3(rest.x, rest.y, rest.z);
}

ogre::Vector3 World::character_to_ground(const ogre::Vector3& at) {
    // CCharacter::dropToGround, as CLevel::addCharacter calls
    // it with 4.0.
    const tl::core::Vec3 ground = tl::core::drop_character_to_ground(
        {at.x, at.y, at.z}, 4.0f,
        [&](const tl::core::Vec3& from, const tl::core::Vec3& to, tl::core::Vec3& point,
            uint32_t& material) {
            tl::core::CollisionHit hit;
            const bool found = collision.ray_collision(from, to, false, hit);
            TL_LOG_DEBUG("  drop ray (%a, %a) -> %d y %a material %u", static_cast<double>(from.x),
                         static_cast<double>(from.z), found ? 1 : 0,
                         found ? static_cast<double>(hit.point.y) : 0.0, found ? hit.material : 0u);
            if (!found) return false;
            point = hit.point;
            material = hit.material;
            return true;
        },
        [&](const tl::core::Vec3& p) {
            return !passability.ready() ||
                   (passability.map_passable(p) && passability.position_passable(p));
        },
        tl::core::volatile_random());
    return ogre::Vector3(ground.x, ground.y, ground.z);
}

tl::core::GoldPileSounds World::gold_sounds() {
    ensure_sound_index();
    return tl::core::GoldPileSounds{&sound_info, &tl::core::game_random()};
}

void World::place_gold(const tl::core::GoldPile& pile, const ogre::Vector3& placed,
                       const tl::core::ItemStats* data, bool drop_sound) {
    // `CItemGold::loadModel` with `media/models/gold/gold.MESH`.
    tl::core::ItemStats item;
    if (data != nullptr) item = *data;
    item.mesh = "media/models/gold/gold";
    // `CItemGold +0x210`: the amount, a space, the translated `Gold`.
    item.display_name = std::to_string(pile.amount) + " Gold";
    Ogre::Entity* entity = create_item_entity(item);
    if (entity == nullptr) return;

    // `CItemGold::setActiveInLevel`: grounded, sparkling.
    const ogre::Vector3 at = snap_item_to_ground(placed);
    const ogre::Quaternion rest(ogre::Radian(pile.heading * 0.017453292f), ogre::Vector3::UNIT_Y);
    Ogre::SceneNode* node = view_place_entity(entity, at, rest);
    // `setScale(0.01)`; `CItemGold::update` grows it.
    view_set_scale(node, 0.01f);

    DroppedWorldItem dropped;
    dropped.item = item;
    dropped.count = 1;
    dropped.entity = entity;
    dropped.node = node;
    dropped.rest = rest;
    dropped.is_gold = true;
    dropped.pile = pile;
    dropped.grow_left = 0.35f;
    dropped.presence.opacity = 1.0f;  // `addItem`
    const std::string sparkle = preloaded_particle_file("GOLD_SPARKLE");
    if (!sparkle.empty()) {
        dropped.sparkle = place_particle_layout(sparkle, at, ogre::Quaternion::IDENTITY, nullptr);
    }
    dropped_items.push_back(dropped);
    // `addItem` plays the drop sound and `rollMoney` again.
    if (drop_sound) play_sound("GoldPickup", at);
    TL_LOG_INFO("gold: %d on the ground at (%.2f, %.2f, %.2f)", pile.amount,
                static_cast<double>(at.x), static_cast<double>(at.y), static_cast<double>(at.z));
}

void World::take_gold(size_t index) {
    if (index >= dropped_items.size() || !dropped_items[index].is_gold) return;
    const DroppedWorldItem dropped = dropped_items[index];
    const ogre::Vector3 at = view_position(dropped.node);
    // `CCharacter::getItem`: `CItemGold::playTakeSound`,
    // `giveGold`, the amount floating up,
    // `CLevel::deleteItem`.
    play_sound("GoldPickup", at);
    player_inventory.give_gold(dropped.pile.amount);
    // CCharacter::giveGold: incrementJournalStatistic(1, amount)
    if (dropped.pile.amount > 0)
        count_journal(tl::core::JournalStatistic::GoldGathered, dropped.pile.amount);
    add_combat_text(tl::core::CombatText::Gold, at, dropped.pile.amount);
    TL_LOG_INFO("gold: picked up %d, %d in the bag", dropped.pile.amount, player_inventory.gold());

    const int64_t gold_unit = dropped_items[index].unit_id;
    remove_ground_item(dropped_items[index]);
    dropped_items.erase(dropped_items.begin() + static_cast<long>(index));
    if (gold_unit != 0) spawned_item_state(gold_unit, 3);
    if (item_targeted == static_cast<int>(index)) {
        item_targeted = -1;
    } else if (item_targeted > static_cast<int>(index)) {
        --item_targeted;
    }
    update_menu();
    refresh_game_hud();
}

void World::auto_pickup_gold() {
    // `CGameClient::autoPickupGold`, every frame: the
    // player alive and moving (`+0x264`), every gold within 3 across the floor.
    if (!player_control || player_health.dead || subject_node == nullptr) return;
    if (!player.moving() && !player.following()) return;
    const ogre::Vector3 me = view_position(subject_node);
    for (size_t i = 0; i < dropped_items.size();) {
        const DroppedWorldItem& dropped = dropped_items[i];
        if (dropped.is_gold && dropped.node != nullptr) {
            const ogre::Vector3 there = view_position(dropped.node);
            const float dx = there.x - me.x;
            const float dz = there.z - me.z;
            if (std::sqrt(dx * dx + 0.0f + dz * dz) < 3.0f) {
                take_gold(i);
                continue;
            }
        }
        ++i;
    }
}

void World::place_ground_item(const tl::core::ItemStats& item, int count, const ogre::Vector3& at,
                              const ogre::Quaternion& orientation,
                              const tl::core::QuestUnitTag& tag) {
    Ogre::Entity* entity = create_item_entity(item);
    if (entity == nullptr) return;
    // `CItem::snapToGround`, which
    // `CQuestUnitData::createUnit`, `restoreLevelState`
    // and `setActiveInLevel` end an item with.
    const ogre::Vector3 rest_at = snap_item_to_ground(at);

    DroppedWorldItem placed;
    placed.item = item;
    placed.count = count;
    placed.entity = entity;
    placed.flying = false;
    placed.rest = orientation;
    placed.quest_tag = tag;
    placed.node = view_place_entity(entity, rest_at, orientation);
    // `CLevel::addItem(.., false)`, no toss:
    // `CEquipment::activateDropParticles` (vtable +0x368,
    // called) starts the drop particle (`+0x3d0`) on the item's node.
    start_item_particles(placed);
    placed.presence.opacity = 1.0f;  // `addItem`
    dropped_items.push_back(placed);
    TL_LOG_INFO("item: %d x %s on the ground at (%.2f, %.2f, %.2f)%s%s", count,
                item.display_name.c_str(), static_cast<double>(rest_at.x),
                static_cast<double>(rest_at.y), static_cast<double>(rest_at.z),
                tag.empty() ? "" : " for quest ", tag.quest.c_str());
}

// `CLevel::addCharacter(c, p, false)` ends every
// character it adds with `CCharacter::dropToGround(4.0)`;
// `CLevel::addUnit`. INVENTED: `dropToGround` is not read; 4.0
// is the ray's start above the point, and nothing under it keeps its height.
ogre::Vector3 World::dropped_to_ground(const ogre::Vector3& at) const {
    ogre::Vector3 place = at;
    float surface = 0.0f;
    if (collision.ground_under(at.x, at.z, at.y + 4.0f, surface)) place.y = surface;
    return place;
}

void World::create_quest_unit(const tl::core::QuestObjective& objective,
                              const tl::core::QuestUnitTag& tag, int64_t quest_guid) {
    // `spawnUnit` places nothing without the resolved
    // unit (`getBaseUnitDataGroup`).
    if (objective.resolved.empty()) {
        TL_LOG_WARN("quest '%s': objective '%s' resolved to no unit; nothing placed",
                    tag.quest.c_str(), objective.unit_name().c_str());
        return;
    }

    // `createUnit`: kind 0 the `Quest Boss` markers, kind
    // 1 the `Quest Item` ones; one picked by `randomBetween(1, count) - 1` on `g_Rand`.
    const std::vector<QuestMarker>& markers =
        objective.kind == 1 ? quest_item_markers : quest_boss_markers;
    ogre::Vector3 at;
    ogre::Quaternion facing = ogre::Quaternion::IDENTITY;
    if (!markers.empty()) {
        const float pick =
            tl::core::game_random().between(1.0f, static_cast<float>(markers.size()));
        size_t index = static_cast<size_t>(static_cast<int64_t>(pick - 1.0f));
        if (index >= markers.size()) index = 0;  // `TArrayList`'s out-of-range read
        at = markers[index].position;
        facing = markers[index].orientation;
    } else {
        // INVENTED: with no marker the original picks a level unit and a free spot within
        // 10 of it (`CLevel::randomOpenPosition`); the walkable ground nearest a random
        // placed unit stands in.
        std::vector<ogre::Vector3> near;
        for (const auto& entry : scene_units) {
            if (entry.second.node != 0 && entry.second.entity != nullptr) {
                if (Ogre::SceneNode* n = view_parent_node(entry.second.entity)) {
                    near.push_back(view_position(n));
                }
            }
        }
        if (near.empty() || !nav.ready()) {
            TL_LOG_WARN("quest '%s': nowhere to place '%s'", tag.quest.c_str(),
                        objective.unit_name().c_str());
            return;
        }
        const int pick =
            tl::core::game_random().integer_between(0, static_cast<int>(near.size()) - 1);
        const ogre::Vector3 from = near[static_cast<size_t>(pick)];
        const tl::core::Vec3 land = tl::core::random_open_item_position(
            {from.x, from.y, from.z}, 0.0f, 10.0f,
            [&](const tl::core::Vec3& p) {
                return !passability.ready() || passability.map_passable(p);
            },
            tl::core::game_random());
        at = ogre::Vector3(land.x, land.y, land.z);
    }

    const int64_t node = spawn_quest_unit(objective, tag, quest_guid, at, facing);
    if (node == 0) return;
    // `createUnit`: `UNIQUE_MONSTER_NAME` names the character (`+0x4c0`).
    if (!objective.unique_monster_name.empty()) unit_names[node] = objective.unique_monster_name;
    const auto named = unit_names.find(node);
    TL_LOG_INFO("quest '%s': %s%s%s placed at (%.2f, %.2f, %.2f)%s", tag.quest.c_str(),
                objective.resolved.c_str(), named != unit_names.end() ? " as " : "",
                named != unit_names.end() ? named->second.c_str() : "", static_cast<double>(at.x),
                static_cast<double>(at.y), static_cast<double>(at.z),
                objective.make_champion ? ", a champion" : "");
}

int64_t World::spawn_quest_unit(const tl::core::QuestObjective& objective,
                                const tl::core::QuestUnitTag& tag, int64_t quest_guid,
                                const ogre::Vector3& at_in, const ogre::Quaternion& facing) {
    // `CResourceManager::createUnit(+0x20, CLevel +0x1a8, false, false)`: item or character.
    ensure_item_index();
    if (const tl::core::ItemStats* item = items.find(objective.resolved)) {
        // `createUnit` -> `createItem`; the quest's
        // `QUEST_GUID` and the objective's id stamped, an equipment identified
        // (`+0x348`).
        tl::core::ItemStats stamped = create_item(*item, false, true);
        stamped.quest_guid = quest_guid;
        stamped.quest_unit_id = objective.id;
        stamped.identified = true;
        place_ground_item(stamped, 1, at_in, facing, tag);
        return 0;
    }
    // `CLevel::addUnit` at the level's level, then
    // `addCharacter`: dropped to the ground (a marker's Y is where the designer clicked).
    const ogre::Vector3 at = dropped_to_ground(at_in);
    if (spawn_unit(objective.resolved, at, /*owner=*/0, 0, false, false, facing) == nullptr) {
        TL_LOG_WARN("quest '%s': could not place '%s'", tag.quest.c_str(),
                    objective.resolved.c_str());
        return 0;
    }
    const int64_t node = kSpawnedNodeBase + static_cast<int64_t>(spawn_serial);
    quest_unit_tags[node] = tag;
    // `spawnUnit`: a `MAKECHAMPION` objective's character is made a champion,
    // `randomIntegerBetween(0, 2)` on `g_Rand` after the unit is in the level.
    // INVENTED: `makeChampion` is this engine's champion path (`apply_champion_curves`).
    // `createPathfinder` rolls `MINION_SPAWNCLASS` on
    // `g_Rand` first, before `makeChampion`'s draw.
    const std::vector<std::string> minions =
        roll_unit_minions(objective.resolved, level_monster_level);
    if (objective.make_champion) {
        const int level = level_monster_level + tl::core::game_random().integer_between(0, 2);
        unit_spawn_level[node] = SpawnLevel{level, true};
        if (!objective.champion_name.empty()) unit_names[node] = objective.champion_name;
    }
    // A character of the level like any other, run by its AI.
    const ogre::Vector3 ahead = facing * ogre::Vector3::UNIT_Z;
    script.add_spawned_unit(node, objective.resolved, tl::core::Vec3{at.x, at.y, at.z},
                            /*hunts=*/false, std::atan2(ahead.x, ahead.z));
    apply_spawn_level(node);
    place_unit_minions(node, /*owner=*/0, minions);
    return node;
}

tl::core::QuestUnitWorld World::quest_unit_world() {
    tl::core::QuestUnitWorld world;
    world.type_id = [this](const std::string& name) {
        load_unit_types();
        return unit_types.id_of(name);
    };
    // `CLevel::getUnitsByUnitType`: characters, then
    // items, each that `ISA(type)`. INVENTED: the port's order: each machine's characters
    // as made (no player), ground items as placed, then breakables; no unit triggers.
    world.units_of_type = [this](uint32_t type) {
        load_unit_types();
        std::vector<tl::core::QuestUnitWorld::LevelUnit> out;
        const auto isa = [this, type](const std::string& unittype) {
            return unit_types.isa(unit_types.id_of(unittype), type);
        };
        for (tl::core::LevelScript* machine : level_machines) {
            for (int64_t node : machine->character_ids()) {
                const std::string unit = machine->unit_of(node);
                if (!isa(stats_for_unit(unit).unittype)) continue;
                tl::core::QuestUnitWorld::LevelUnit found;
                found.handle = node;
                const auto tagged = quest_unit_tags.find(node);
                if (tagged != quest_unit_tags.end()) found.tag = tagged->second;
                out.push_back(found);
            }
        }
        for (size_t i = 0; i < dropped_items.size(); ++i) {
            const DroppedWorldItem& d = dropped_items[i];
            if (!isa(d.item.unittype)) continue;
            tl::core::QuestUnitWorld::LevelUnit found;
            found.handle = static_cast<int64_t>(i);
            found.item = true;
            found.tag = quest_tag_of_item(d.item);
            out.push_back(found);
        }
        for (const BreakableWorld& b : breakables) {
            if (!isa(stats_for_unit(b.unit).unittype)) continue;
            tl::core::QuestUnitWorld::LevelUnit found;
            found.handle = -1;  // a breakable carries no quest
            found.item = true;
            out.push_back(found);
        }
        return out;
    };
    // `populate`'s stamp: the quest's GUID and the objective's id.
    world.tag_unit = [this](const tl::core::QuestUnitWorld::LevelUnit& unit,
                            const tl::core::QuestUnitTag& tag) {
        if (!unit.item) {
            quest_unit_tags[unit.handle] = tag;
            return;
        }
        if (unit.handle < 0 || static_cast<size_t>(unit.handle) >= dropped_items.size()) return;
        DroppedWorldItem& d = dropped_items[static_cast<size_t>(unit.handle)];
        d.item.quest_guid = quest_guid_of(tag);
        d.item.quest_unit_id = tag.objective;
        d.quest_tag = tag;
        // `+0x348` and `destroyItemText`: identified, its label made again
        // from the item as it now is.
        d.item.identified = true;
    };
    // `CQuest +0x10`: the dungeon's kept template for the quest's floor.
    world.choose_spawners = [this](const tl::core::QuestEntry& entry, int32_t seed_depth,
                                   std::vector<std::string>& chosen) {
        if (entry.definition == nullptr) return false;
        tl::core::Dungeon* dungeon = loaded_dungeon_manager().dungeon_by_name(
            tl::core::upper_ascii(entry.definition->dungeon));
        if (dungeon == nullptr) return false;
        const tl::core::DungeonFiles files = archive_dungeon_files();
        // Its own generator: with no template the original returns before `resetGameSeed`.
        tl::core::RunicRandom floor_rand(0u);
        tl::core::reset_game_seed(tl::core::game_seed(), entry.depth, floor_rand);
        tl::core::LevelTemplate* found = dungeon->get_level_template_for_depth(
            static_cast<uint32_t>(std::max(0, entry.depth)), files, floor_rand);
        // The loop keeps no template that says `NO_RANDOM_QUESTS`.
        if (found == nullptr || found->no_random_quests) return false;
        tl::core::RunicRandom& g_rand = tl::core::game_random();
        tl::core::reset_game_seed(tl::core::game_seed(), seed_depth, g_rand);
        tl::core::choose_unit_spawners(*found, archive_spawn_class_lookup(), g_rand);
        chosen.assign(found->chosen_spawn_classes.begin(), found->chosen_spawn_classes.end());
        return true;
    };
    auto context = [this]() -> const tl::core::SpawnContext& {
        if (!loot_tables_built) {
            loot_tables_built = true;
            build_spawn_context(loot_tables, /*items_by_type=*/true);
        }
        return loot_tables.context;
    };
    world.has_spawn_class = [context](const std::string& name) {
        const tl::core::SpawnContext& c = context();
        return c.get_spawn_class && c.get_spawn_class(name) != nullptr;
    };
    world.roll_spawn_class = [context](const std::string& name, int32_t level, int32_t level_bonus,
                                       int32_t rarity_override) {
        const tl::core::SpawnContext& c = context();
        const tl::core::SpawnClass* table = c.get_spawn_class ? c.get_spawn_class(name) : nullptr;
        if (table == nullptr) return std::string();
        std::vector<std::string> units;
        table->roll_spawn_class(units, nullptr, nullptr, nullptr, level, 0xffffffffu, level_bonus,
                                rarity_override, 0, 0, tl::core::game_random(), c);
        return units.empty() ? std::string() : units[0];
    };
    world.random_open_position = [this](const tl::core::Vec3& at, float radius) {
        // `CLevel::randomOpenPosition(at, radius, false)`.
        return tl::core::random_open_position(
            at, 0.0f, radius,
            [this](const tl::core::Vec3& p) {
                return !passability.ready() ||
                       (passability.map_passable(p) && passability.position_passable(p));
            },
            tl::core::volatile_random());
    };
    world.spawn_unit = [this](const tl::core::QuestEntry& entry,
                              const tl::core::QuestObjective& objective,
                              const tl::core::QuestUnitTag& tag, const tl::core::Vec3& at) {
        const int64_t guid = entry.definition != nullptr ? entry.definition->guid : -1;
        spawn_quest_unit(objective, tag, guid, ogre::Vector3(at.x, at.y, at.z),
                         ogre::Quaternion::IDENTITY);
        TL_LOG_INFO("quest '%s': %s spawned at (%.2f, %.2f, %.2f) for an objective done",
                    tag.quest.c_str(), objective.resolved.c_str(), static_cast<double>(at.x),
                    static_cast<double>(at.y), static_cast<double>(at.z));
    };
    return world;
}

// `CEquipment::updateDrop`: 20 units of path a second,
// `LAND_SOUND` at the end. A weapon or shield turns once round its Z, before its rest.
void World::step_dropped_item(DroppedWorldItem& dropped, float delta) {
    if (dropped.is_gold && dropped.node != nullptr) {
        // `CItemGold::update`: the sparkle (`+0x280`)
        // follows the wish to be seen; the pile grows as `min(1, 1 - left / 0.35)`.
        if (!dropped.presence.want_visible) {
            view_set_visible(dropped.sparkle, false);
            return;
        }
        if (!dropped.presence.visible) return;
        view_set_visible(dropped.sparkle, true);
        if (0.0f < dropped.grow_left) {
            dropped.grow_left = dropped.grow_left - delta;
            float grown = 1.0f - dropped.grow_left / 0.35f;
            if (!(grown <= 1.0f)) grown = 1.0f;
            const float size = grown * dropped.pile.scale;
            view_set_scale(dropped.node, size);
        }
        return;
    }
    if (!dropped.flying || dropped.node == nullptr) return;
    // `CLevel::updateDroppingItems`, `updateDrop` inline;
    // the step along the path is one `fmadds`.
    float along = std::fma(delta, tl::core::kDropSpeed, dropped.travelled);
    dropped.travelled = along;
    if (dropped.arc.length <= along) {
        dropped.travelled = dropped.arc.length;
        dropped.flying = false;
        along = dropped.travelled;
    }
    const tl::core::Vec3 p = tl::core::spline_position_at_distance(dropped.arc, along);
    view_set_position(dropped.node, ogre::Vector3(p.x, p.y, p.z));
    if (dropped.spins) {
        // DIVERGENCE: a path of no length counts as flown; the console turns the item by
        // a NaN, which OGRE cannot hold.
        float progress = dropped.arc.length > 0.0f ? along / dropped.arc.length : 1.0f;
        if (progress > 1.0f) progress = 1.0f;
        view_set_orientation(dropped.node,
                             dropped.rest * ogre::Quaternion(ogre::Radian(progress * 6.2831855f),
                                                             ogre::Vector3::UNIT_Z));
    }
    if (!dropped.flying) {
        view_set_orientation(dropped.node, dropped.rest);
        play_sound(dropped.item.land_sound, ogre::Vector3(p.x, p.y, p.z));
        // How it lies: the model's height across the floor once laid down.
        const ogre::Vector3 size = view_landed_size(dropped.node, dropped.entity);
        TL_LOG_INFO("  %s landed at (%.2f, %.2f, %.2f), %.2f high by %.2f x %.2f",
                    dropped.item.display_name.c_str(), static_cast<double>(p.x),
                    static_cast<double>(p.y), static_cast<double>(p.z), static_cast<double>(size.y),
                    static_cast<double>(size.x), static_cast<double>(size.z));
    }
}

void World::start_item_particles(DroppedWorldItem& dropped) {
    if (dropped.is_gold || dropped.node == nullptr) return;
    load_unit_types();
    const tl::core::ItemStats& item = dropped.item;
    const uint32_t type = unit_types.id_of(item.unittype);
    // `CEquipment::createParticles`: the drop particle
    // by kind, then a weapon's element, each on the model's node.
    const bool quest =
        unit_types.isa(type, tl::core::unit_type::kQuestItem) || item.quest_guid != -1;
    std::vector<std::string> names;
    names.push_back(tl::core::drop_particle_name(item.drop_particle, quest,
                                                 unit_types.isa(type, tl::core::unit_type::kUnique),
                                                 tl::core::item_is_magical(item, unit_types)));
    if (unit_types.isa(type, tl::core::unit_type::kWeapon)) {
        const tl::core::EffectManager::Context& effects = item_effect_context();
        const std::string element = tl::core::elemental_particle_name(
            unit_types.isa(type, tl::core::unit_type::kPistol), [&](int damage_type) {
                return tl::core::weapon_damage_bonus(item, damage_type, effects);
            });
        if (!element.empty()) names.push_back(element);
    }
    for (const std::string& name : names) {
        // `CResourceManager::createParticle`: a preloaded name, or an authored path.
        std::string file = preloaded_particle_file(name);
        if (file.empty()) file = name;
        std::vector<Ogre::SceneNode*> holders = place_particle_layout(
            file, ogre::Vector3::ZERO, ogre::Quaternion::IDENTITY, dropped.node);
        if (holders.empty()) {
            TL_LOG_WARN("item '%s': no particle '%s'", item.name.c_str(), name.c_str());
            continue;
        }
        dropped.particles.insert(dropped.particles.end(), holders.begin(), holders.end());
    }
}

void World::remove_ground_item(DroppedWorldItem& dropped) {
    // `CEquipment::setActiveInLevel(false)`: the drop
    // particle stops, the label goes (`hideItemText`).
    destroy_particle_holders(dropped.particles);
    destroy_particle_holders(dropped.sparkle);
    if (item_highlighted == dropped.entity) item_highlighted = nullptr;
    if (dropped.node != nullptr) view_destroy_node(dropped.node);
    if (dropped.entity != nullptr) view_destroy_entity(dropped.entity);
    dropped.node = nullptr;
    dropped.entity = nullptr;
}

}  // namespace tl::core
