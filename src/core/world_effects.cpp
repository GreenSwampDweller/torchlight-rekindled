// The gameplay half of render/renderer_effects.cpp (core/world.h).
#include "core/skill_grid.h"
#include "core/world.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace tl::core {

// PORT: an item of the port's index as a spawn class sees it (`SpawnUnitData::from_adm`'s fields).
SpawnUnitData spawn_data_of(const tl::core::ItemStats& item) {
    tl::core::SpawnUnitData u;
    u.name = item.name;
    u.unittype = item.unittype;
    u.rarity = item.spawn_rarity;
    u.rarity_hardcore = item.rarity_hardcore;
    u.create_as = item.create_as;
    u.level = item.level;
    u.min_level = item.spawn_min_level;
    u.max_level = item.spawn_max_level;
    return u;
}

// PORT: the tables an item roll reads, gathered once.
tl::core::ItemRollContext World::item_roll_context() {
    ensure_effect_data();
    load_stat_graphs();
    load_unit_types();
    if (!item_roll_globals_loaded) {
        item_roll_globals_loaded = true;
        const std::vector<uint8_t> bytes = read_resource("media/GLOBALS.DAT.adm");
        const tl::formats::AdmFile globals =
            bytes.empty() ? tl::formats::AdmFile{} : parse_data_group(bytes.data(), bytes.size());
        if (globals.ok) {
            item_roll_globals = tl::core::read_item_roll_globals(globals);
            item_label_colours = tl::core::read_item_label_colours(globals);
        } else {
            TL_LOG_WARN("items: media/GLOBALS.DAT did not read; items roll no enchants or sockets");
        }
    }
    tl::core::ItemRollContext context;
    context.globals = &item_roll_globals;
    context.affixes = &affixes;
    context.unit_types = &unit_types;
    context.graphs = &stat_graphs;
    context.volatile_rng = &tl::core::volatile_random();
    context.game_rng = &tl::core::game_random();
    context.effects = &item_effect_context();
    // Filling a new item's sound bank draws on `g_Rand` (`init_item_copy`), device or not.
    ensure_sound_index();
    context.sounds = &sound_info;
    // `CBaseUnit::unitInit` on an item: each `[SKILL]` made, its effects rolled.
    context.make_skills = [this](const tl::core::ItemStats& item) {
        if (item.skills.empty()) return;
        const tl::core::SkillConstruction with = skill_construction();
        for (const tl::core::ItemStats::Skill& given : item.skills) {
            const auto skill = player_skill_files.find(tl::core::upper_ascii(given.name));
            if (skill != player_skill_files.end())
                tl::core::roll_skill_construction(skill->second->def, with);
        }
    };
    return context;
}

// PORT: the tables an item's effects read, gathered once.
const tl::core::EffectManager::Context& World::item_effect_context() {
    ensure_effect_data();
    load_stat_graphs();
    load_unit_themes();
    item_effect_context_.types = &effect_types;
    item_effect_context_.graphs = &stat_graphs;
    item_effect_context_.owner.present = false;
    item_effect_context_.theme_book = &unit_themes;
    return item_effect_context_;
}

// PORT: the inventories handed the effect tables.
void World::refresh_inventory_effects() {
    ensure_effect_data();
    load_stat_graphs();
    const tl::core::InventoryEffectContext context{&effect_types, &stat_graphs, &item_sets,
                                                   &affixes};
    player_inventory.set_effect_context(context);
    pet.bag().set_effect_context(context);
    refresh_pet_effects();
}

void World::start_player_passives() {
    // `CSkillManager::startPassiveSkills` stops every
    // running passive first, so re-running is not additive.
    refresh_skill_effects();
    tl::core::SkillCastContext context = player_cast_context();
    context.on_skill = [](const std::string& name, uint32_t level, bool ok) {
        if (ok) return;
        // Usually its `EVENT_START` names an affix no file answers to.
        TL_LOG_WARN("skills: passive '%s' at level %u applied nothing", name.c_str(), level);
    };
    const int started = tl::core::start_passive_skills(player_skills, context);
    // A passive's affix can carry `+N to <skill>`: recompute the ranks
    // (`calculateEffectiveSkillLevel` inside the loop).
    player_skills.set_effect_managers(player_total(), nullptr);
    TL_LOG_INFO("skills: %d passive(s) started, %zu affix(es) held, %zu effect(s)", started,
                player_effects.affixes().size(), player_effects.all_effects().size());
}

void World::load_recharge_rates() {
    if (recharge_rates_loaded) return;
    recharge_rates_loaded = true;
    // `CGameGlobals::reload`: a missing key keeps the
    // constructor's value.
    const std::vector<uint8_t> bytes = read_resource("media/GLOBALS.DAT.adm");
    if (bytes.empty()) return;
    const tl::formats::AdmFile globals = parse_data_group(bytes.data(), bytes.size());
    if (!globals.ok) return;
    recharge_rates.hp = globals.float_or("HP_RECHARGE_RATE", recharge_rates.hp);
    recharge_rates.pet_hp = globals.float_or("PET_HP_RECHARGE_RATE", recharge_rates.pet_hp);
    recharge_rates.mana = globals.float_or("MANA_RECHARGE_RATE", recharge_rates.mana);
}

void World::refresh_player_maxima() {
    // `CCharacter::maxHP` and `maxMana`:
    // the curve's base, permanent bonuses, `MAX`/`PERCENT` effects.
    const tl::core::CharacterEffects total = player_total();
    player_health.max = static_cast<float>(player_character.max_hp_of(total));
    player_character.set_mp(player_character.mp(),
                            static_cast<float>(player_character.max_mp_of(total)));
}

void World::respec_player() {
    // `CPlayer::resetSkills`: `stopAllSkills(true, false,
    // true)`, the passives too, restarted at the new levels as `setSkillLevel`
    // does.
    player_skill_runner.stop_all(
        player_cast_context(),
        [this](const std::string& name) -> const tl::core::KnownSkill* {
            return player_skill(name);
        },
        /*force=*/true, /*passives=*/true);
    const int refunded = player_skills.reset_skills();
    player_character.give_unspent_skill_points(refunded);
    start_player_passives();
    TL_LOG_INFO(
        "respec: skills back to the class's starting levels, %d point(s) refunded (%d "
        "unspent)",
        refunded, player_character.unspent_skill_points());
}

void World::update_player_meters(float dt) {
    refresh_player_maxima();
    // `CCharacter::updateHP`/`updateMana` return at once in state 5 or 6.
    if (player_health.dead || !(player_health.current > 0.0f)) return;
    load_recharge_rates();

    const float hp = tl::core::hp_change(player_health.max, recharge_rates, player_total(), dt);
    if (hp > 0.0f) {
        // `modifyHP`: up to `maxHP()`, an
        // integer; in god mode straight to `maxHP()`.
        const float ceiling = static_cast<float>(static_cast<int>(player_health.max));
        player_health.current = god_mode ? ceiling : std::min(ceiling, player_health.current + hp);
    } else if (hp < 0.0f) {
        // The effect owner taking the credit, for `kill_player`.
        const tl::core::EffectSource credit = player_effects.death_credit();
        player_effect_killer = !credit.set       ? tl::core::LevelScript::kNoAttacker
                               : credit.player() ? 0
                                                 : credit.node;
        player_hurt_by_effects = true;
        hurt_player(-hp, "an effect", tl::core::LevelScript::kNoAttacker, false, /*shown=*/false);
        player_hurt_by_effects = false;
        // HP under 1, `updateHP` broadcasts death by effect (inline
        // e14) before `die`'s broadcast, which `kill_player` held back.
        if (static_cast<int>(std::floor(player_health.current)) < 1) {
            player_effects.take_death_credit();
            unit_observers.broadcast({nullptr, 0}, tl::core::unit_state::kDiedOfEffect);
            if (player_health.dead) {
                unit_observers.broadcast({nullptr, 0}, tl::core::unit_state::kDied);
            }
        }
    }
    // `updateHP`: the effects' damage goes onto the number.
    const float effect_damage = tl::core::effect_values_of_damage(player_total()) * dt;
    if (effect_damage < 0.0f) {
        player_damage_shown.pending = player_damage_shown.pending - effect_damage;
    }

    const float mana =
        tl::core::mana_change(player_character.max_mp(), recharge_rates, player_total(), dt);
    if (mana != 0.0f) modify_player_mana(mana);
    // `displayDamageAbsorbed(delta)`, after `updateMana`.
    display_player_damage(dt);
}

// `CCharacter::modifyMana`.
void World::modify_player_mana(float delta) {
    const float ceiling = static_cast<float>(static_cast<int>(player_character.max_mp()));
    // `getIsInGodMode` (vtable +0x290, `CPlayer`): the mana is `maxMana()`.
    //
    if (god_mode) {
        player_character.set_mp(ceiling, player_character.max_mp());
        return;
    }
    const float next = std::max(0.0f, std::min(ceiling, player_character.mp() + delta));
    player_character.set_mp(next, player_character.max_mp());
}

// PORT: the player's inventory handed to `identify_item_in`.
bool World::identify_player_item(size_t slot_index) {
    return identify_item_in(player_inventory, slot_index);
}

void World::bag_default_action(int slot_idx) {
    if (slot_idx < 0 || static_cast<size_t>(slot_idx) >= player_inventory.slots().size() ||
        player_inventory.slots()[static_cast<size_t>(slot_idx)].count <= 0) {
        return;
    }
    const auto& slot = player_inventory.slots()[static_cast<size_t>(slot_idx)];
    load_unit_types();
    if (unit_types.isa(slot.item.unittype, tl::core::unit_type::kSocketable)) {
        // a gem (ISA 0x78) on an ordinary tray goes to
        // the item-select dialog.
        open_gem_socketing();
    } else if (unit_types.isa(slot.item.unittype, tl::core::unit_type::kFish)) {
        // a fish (ISA 0x7c) is used on the player's first pet.
        feed_pet_from(player_inventory, static_cast<size_t>(slot_idx));
    } else if (unit_types.isa(slot.item.unittype, tl::core::unit_type::kSpell)) {
        // a spell goes to the learn test without the empty slot,
        // then (the learn, or the replace dialog).
        if (tl::core::spell_learnable(slot.item, slot.item.uses, player_use_target(), unit_types,
                                      stat_graphs)) {
            learn_spell_from_tray(/*pet_owner=*/false, static_cast<size_t>(slot_idx));
        }
    } else if (tl::core::item_is_useable(slot.item, unit_types)) {
        // `processIngameInput`'s slot use: a useable item
        // (`CItem::isUseable`) goes to `useItem`.
        use_player_item(static_cast<size_t>(slot_idx), /*from_tray=*/true);
    } else {
        // any other is identified, worn or taken off; a quest item
        // stays.
        wear_player_item(static_cast<size_t>(slot_idx));
    }
    // Every branch above can change slot 0/1: keep the held-item mesh in sync.
    sync_equipped_weapon();
    update_menu();
    update_character_preview(0.0f);
    refresh_game_hud();
}

tl::core::Vec3 World::pad_skill_point(const tl::core::KnownSkill& skill, float stick_x,
                                      float stick_y, float left_trigger,
                                      float right_trigger) const {
    const tl::core::Vec3 me = player.position();
    // a stick past 0.1 aims along itself at twice its deflection, turned by the
    // camera's heading, height 0; at rest takes position plus forward.
    tl::core::Vec3 point;
    if (0.1f < std::sqrt(stick_x * stick_x + stick_y * stick_y)) {
        const float sin_yaw = std::sin(camera_yaw);
        const float cos_yaw = std::cos(camera_yaw);
        const float sx = stick_x * 2.0f;
        const float sy = stick_y * 2.0f;
        point = {me.x + (sy * sin_yaw - sx * cos_yaw), 0.0f, me.z + (sy * cos_yaw + sx * sin_yaw)};
    } else {
        const float yaw = player.facing();
        point = {me.x + std::sin(yaw), me.y, me.z + std::cos(yaw)};
    }
    // the distance along that line, between `RANGEMIN` (+0x58) and `RANGE`
    // (+0x54) less 0.1 (`CSkill +0x5c`): the LT slot (5 or 8, `CGameUI +0xf88`) by the left
    // trigger's pull, the RT slot (6 or 9) by the right's; else
    // the middle.
    const tl::core::SkillProperty* property = skill.property(skill.effective);
    const float far_end = property != nullptr ? property->range : 1.0f;
    const float near_end = property != nullptr ? property->range_min : 0.0f;
    const size_t set_offset = player_skills.skill_set() != 0 ? 3 : 0;
    const auto in_slot = [&](int slot) {
        return player_skills.slots()[static_cast<size_t>(slot) + set_offset] == skill.guid();
    };
    float distance = (far_end + near_end) * 0.5f;
    if (0.0f < left_trigger && in_slot(5)) {
        distance = (far_end - 0.1f - near_end) * left_trigger + near_end;
    } else if (0.0f < right_trigger && in_slot(6)) {
        distance = (far_end - 0.1f - near_end) * right_trigger + near_end;
    }
    float dx = point.x - me.x;
    float dz = point.z - me.z;
    const float length = std::sqrt(dx * dx + dz * dz);
    if (1e-8f < length) {
        dx /= length;
        dz /= length;
    }
    return {me.x + dx * distance, me.y, me.z + dz * distance};
}

// `CCharacter::setTargetDirection`.
void World::set_player_target_direction(const tl::core::Vec3& point) {
    player_turn_point = {point.x, 0.0f, point.z};
    const tl::core::Vec3 me = player.position();
    float dx = player_turn_point.x - me.x;
    float dz = player_turn_point.z - me.z;
    const float length = std::sqrt(dx * dx + 0.0f + dz * dz);
    if (1e-08f < length) {
        dx = dx * (1.0f / length);
        dz = dz * (1.0f / length);
    }
    player_turn_direction = {dx, 0.0f, dz};
}

// `CCharacter::turnTowardPosition` and
// `turnToward`.
void World::turn_player_toward(const tl::core::Vec3& at, float dt) {
    player_turn_point = {at.x, 0.0f, at.z};
    const tl::core::Vec3 me = player.position();
    float dx = at.x - me.x;
    float dy = at.y - me.y;
    float dz = at.z - me.z;
    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (1e-08f < length) {
        dx = dx * (1.0f / length);
        dy = dy * (1.0f / length);
        dz = dz * (1.0f / length);
    }
    // `turnToward` stores nothing in states 5 and 6 or with no turn rate.
    if (player_death.dying || player_stats.turn_rate == 0.0f) return;
    player_turn_direction = {dx, dy, dz};
    player.face(tl::core::turn_toward(player.facing(), dx, dz, player_stats.turn_rate, dt));
}

void World::aim_position_skill(const tl::core::KnownSkill& skill, tl::core::SkillCaster& caster) {
    const tl::core::SkillProperty* property = skill.property(skill.effective);
    const int32_t type = property != nullptr
                             ? property->target_type
                             : (skill.def != nullptr ? skill.def->base.target_type : 0);
    // Target types 1 `POSITION`, 11, 5 `POSITIONRANDOM` and 6 `POSITIONFLEE`.
    if (type != 1 && type != 0xb && type != 5 && type != 6) return;
    const tl::core::Vec3 me = player.position();
    // The point stored (x and z, height 0); `--cast` aims at rest.
    tl::core::Vec3 point = pad_skill_target.has_value()
                               ? *pad_skill_target
                               : pad_skill_point(skill, 0.0f, 0.0f, 0.0f, 0.0f);
    point.y = 0.0f;
    // (not Linux's `findWorldLocation`): a ray from 100 above to 100 below;
    // a hit puts the point 0.1 over the ground.
    tl::core::CollisionHit hit;
    if (collision.ray_collision({point.x, point.y + 100.0f, point.z},
                                {point.x, point.y - 100.0f, point.z}, true, hit)) {
        point = hit.point;
        point.y += 0.1f;
    }
    // `setTarget(0)` and the target item let go.
    set_player_target(0);
    const float ground = point.y;
    // Within `RANGE` (`CSkill::getRange`) of the caster on the ground plane.
    tl::core::Vec3 offset{point.x - me.x, 0.0f, point.z - me.z};
    const float range = property != nullptr ? property->range : 1.0f;
    float length = std::sqrt(offset.x * offset.x + offset.z * offset.z);
    if (range < length) {
        offset.x = offset.x / length * range;
        offset.z = offset.z / length * range;
        length = range;
    }
    point = {me.x + offset.x, me.y, me.z + offset.z};
    if (skill.def != nullptr && skill.def->requires_pathable && property != nullptr) {
        // Walked back 0.4 at a time, `length / 0.4 - 2` times, until `CCharacter::canPathTo`
        // answers; then the route's last node, else the
        // caster's position.
        tl::core::Vec3 step{0.0f, 0.0f, 0.0f};
        if (1e-8f < length) step = {offset.x / length * -0.4f, 0.0f, offset.z / length * -0.4f};
        const int tries = static_cast<int>(length / 0.4f) + -2;
        bool found = false;
        for (int i = 0; i < tries; ++i) {
            std::vector<tl::core::Vec3> nodes;
            if (passability.ready() &&
                nav.find_nodes(me, point, passability, /*ignore_objects=*/true, nodes) &&
                !nodes.empty()) {
                const tl::core::Vec3 goal = nav.sample_at(point.x, point.z);
                const tl::core::Vec3 last = nav.sample_at(nodes.back().x, nodes.back().z);
                if (goal.x == last.x && goal.z == last.z) {
                    point.x = nodes.back().x;
                    point.z = nodes.back().z;
                    found = true;
                    break;
                }
            }
            point = {point.x + step.x, point.y + step.y, point.z + step.z};
        }
        if (!found) point = me;
    } else {
        point.y = ground;
    }
    caster.target_position = point;
    // `castSkill` keeps the point in `+0x228` for the skill's turn.
    player_turn_point = point;
    TL_LOG_INFO("skills: '%s' aimed at (%.2f, %.2f, %.2f), %.2f away", skill.entry.name.c_str(),
                static_cast<double>(point.x), static_cast<double>(point.y),
                static_cast<double>(point.z),
                static_cast<double>(std::sqrt((point.x - me.x) * (point.x - me.x) +
                                              (point.z - me.z) * (point.z - me.z))));
}

void World::step_hotbar(const std::array<bool, 7>& pressed, const std::array<bool, 7>& held,
                        float stick_x, float stick_y) {
    const auto at = [](const std::array<bool, 7>& state, tl::core::HotbarButton button) {
        return state[static_cast<size_t>(button)];
    };
    const tl::core::HotbarPress press = tl::core::hotbar_press(
        player_skills.slots(), player_skills.second_slots(),
        [this](int64_t guid) { return player_skills.find_by_guid(guid) != nullptr; },
        player_skills.skill_set(),
        [&](tl::core::HotbarButton button) { return at(pressed, button); },
        [&](tl::core::HotbarButton button) { return at(held, button); });
    if (press.slot < 0) return;
    if (!press.skill) {
        // The pending use (`CGameUI +0x149c`), for `CGameUI::activateItemSlot`.
        activate_item_slot(press.slot);
        return;
    }
    const tl::core::KnownSkill* skill =
        player_skills.find_by_guid(player_skills.slots()[static_cast<size_t>(press.slot)]);
    if (skill == nullptr) return;
    // The skill-use call ends, past, in `CCharacter::castSkill`
    // twice, the first result dropped. Every path first runs the stick's aim
    // and `setTarget(0)` (clearing `+0x28c`), so the
    // target set
    // never reaches the cast: `find_player_skill_target` picks in the cone.
    set_player_target(0);
    // The point it aims.
    // DIVERGENCE (platform): ZL and ZR are buttons, so a held trigger is a full pull.
    const auto down = [&](tl::core::HotbarButton button) {
        return pressed[static_cast<size_t>(button)] || held[static_cast<size_t>(button)];
    };
    pad_skill_target =
        pad_skill_point(*skill, stick_x, stick_y, down(tl::core::HotbarButton::LT) ? 1.0f : 0.0f,
                        down(tl::core::HotbarButton::RT) ? 1.0f : 0.0f);
    set_player_target_direction(*pad_skill_target);
    cast_player_skill(*skill, !press.continuing);
    cast_player_skill(*skill, false);
    pad_skill_target.reset();
}

void World::release_held_skill(const std::array<bool, 7>& pressed, const std::array<bool, 7>& held,
                               bool face_pressed) {
    // The Xbox in-game dispatch: a face button press skips the test for the
    // frame. `CCharacter::performingSkill`, the dispatch's.
    if (face_pressed || !player_performing_skill()) return;
    const tl::core::KnownSkill* active = player_skills.find_by_name(player_active_skill.skill);
    const int64_t guid = active != nullptr ? active->guid() : -1;
    // The X button's own skill (`CPlayer +0x2d0`) is never active: the port has none on X.
    const auto at = [](const std::array<bool, 7>& state, tl::core::HotbarButton button) {
        return state[static_cast<size_t>(button)];
    };
    if (!tl::core::hotbar_lets_go(
            player_skills.slots(), guid, player_skills.skill_set(),
            [&](tl::core::HotbarButton button) { return at(pressed, button); },
            [&](tl::core::HotbarButton button) { return at(held, button); })) {
        return;
    }
    // The second argument is `+0x55 == 0`: X not down at the last dispatch.
    attempt_to_stop_player_skill(!attack_button.was_down);
}

// PORT: the player's skill asked of `still_casting` (core/skill_cast).
bool World::player_performing_skill() {
    const std::string& name = player_active_skill.skill;
    const tl::core::SkillRunState* state = player_skill_runner.state(name);
    return tl::core::still_casting(
        player_active_skill, state != nullptr ? state->animations : tl::core::SkillAnimations{},
        player_skill_runner.is_running(name));
}

// `CCharacter::interrupt(bool hit)` on the player.
void World::interrupt_player(bool hit) {
    // INVENTED: the player holds no AI flags here, so `CANNOT INTERRUPT` never stops it.
    // State 0x10, an `Animation Controller` holding the player: nothing.
    if (!scripted_player_animation.empty()) return;
    bool has_hit = false;
    for (const auto& entry : player_catalog) {
        if (tl::core::upper_ascii(entry.first) == "HIT") has_hit = true;
    }
    // a skill is active and its animation up, or its swing not over.
    const bool performing =
        !player_active_skill.empty() &&
        (player_active_skill.casting ||
         (0.0f < player_active_skill.animation_left && !player_active_skill.attack_ended));
    if (performing) {
        // `+0x2cc`, the cast's `INTERRUPTABLE`.
        const tl::core::KnownSkill* active = player_skill(player_active_skill.skill);
        if (active == nullptr || active->def == nullptr || !active->def->base.interruptable) {
            return;
        }
        if (!has_hit) {
            // A random `IDLE` (its draws kept; the player's idle is its gait) and state 0.
            hit = false;
            (void)tl::core::model_find_random_animation(player_catalog, "IDLE");
            skill_playback = SkillPlayback{};
        }
    }
    if (hit && !has_hit) return;
    if (player_death.dying || player_health.dead) return;
    // The swing (`+0x2ac`) and the cast (`+0x2b4`, `+0x1ff`) dropped, every skill stopped.
    swing = Swing{};
    player_active_skill.animation_left = 0.0f;
    player_active_skill.casting = false;
    player_skill_runner.stop_all(
        player_cast_context(),
        [this](const std::string& n) -> const tl::core::KnownSkill* { return player_skill(n); },
        /*force=*/false, /*passives=*/false);
    if (hit) {
        // A random `HIT` without a loop, `IDLE` (the gait) behind it; state 0x1d.
        // INVENTED: the player has no AI state 0x1d here, so its input is not held meanwhile.
        const int index = tl::core::model_find_random_animation(player_catalog, "HIT");
        if (index >= 0 && static_cast<size_t>(index) < player_catalog.size()) {
            tl::core::SkillAnimationCue cue;
            cue.animation = index;
            cue.speed = 1.0f;
            cue.duration = player_catalog[static_cast<size_t>(index)].second;
            play_skill_cue(cue);
        }
        TL_LOG_INFO("combat: the player is interrupted");
    }
    // The hands' weapon trails off.
    player_hide_hand_trails();
}

void World::attempt_to_stop_player_skill(bool stop_path) {
    // `CPlayer::attemptToStopPlayerSkill(bool)`.
    if (player_active_skill.empty()) return;
    const std::string name = player_active_skill.skill;
    const tl::core::KnownSkill* skill = player_skills.find_by_name(name);
    const tl::core::SkillRunState* state = player_skill_runner.state(name);
    const tl::core::SkillAnimations* animations = state != nullptr ? &state->animations : nullptr;
    const int loop_end = animations != nullptr ? animations->loop_end : -1;
    // `CSkill::getCanStop`: no property, or the run has lasted `MINIMUMTIMEMS`.
    bool can_stop = true;
    if (skill != nullptr && state != nullptr) {
        if (const tl::core::SkillProperty* property = skill->property(state->level)) {
            can_stop = property->minimum_time <= state->elapsed;
        }
    }
    // `CCharacter::attemptStopOfActiveSkill`.
    const tl::core::SkillStop stop =
        tl::core::attempt_stop_of_active_skill(player_active_skill, animations, can_stop);
    const auto lookup = [this](const std::string& n) -> const tl::core::KnownSkill* {
        return player_skill(n);
    };
    if (stop == tl::core::SkillStop::StopByName) {
        const float elapsed = state != nullptr ? state->elapsed : 0.0f;
        player_skill_runner.stop_by_name(name, player_cast_context(), lookup);
        TL_LOG_INFO("skills: '%s' let go after %.2fs", name.c_str(), static_cast<double>(elapsed));
    } else if (stop == tl::core::SkillStop::StopAll) {
        // `stopAllSkills(false, false, false)`.
        player_skill_runner.stop_all(player_cast_context(), lookup, /*force=*/false,
                                     /*passives=*/false);
    }
    if (player_performing_skill()) return;
    // `blendAnimation(loopEnd, 0.2, 1.0, -1)` with `IDLE` queued behind it.
    if (loop_end >= 0 && static_cast<size_t>(loop_end) < player_catalog.size()) {
        tl::core::SkillAnimationCue end;
        end.animation = loop_end;
        end.duration = player_catalog[static_cast<size_t>(loop_end)].second;
        end.then_idle = true;
        play_skill_cue(end);
    }
    // `MOVES_TO_TARGET` (`CSkill +0x68`): `stopPathing`.
    if (stop_path && skill != nullptr && skill->def != nullptr && skill->def->moves_to_target) {
        player.stop_following();
    }
}

void World::bind_default_item_slots() {
    // `CPlayer::unitInit`: `Health Potion` and `Mana
    // Potion` GUIDs into item slots 0 and 1 (+0x890/+0x898; skill map +0x840/+0x848
    // cleared), the bumpers. The console puts nothing on slot 9; the link map (+0x6a8,
    // +0x6b0) is not kept.
    ensure_item_index();
    const std::pair<int, const char*> defaults[] = {{0, "Health Potion"}, {1, "Mana Potion"}};
    for (const auto& [index, name] : defaults) {
        const tl::core::ItemStats* item = items.find(name);
        if (item != nullptr && item->unit_guid != -1)
            player_skills.set_item_slot(index, item->unit_guid);
    }
}

// PORT: the class file found by NAME and handed to `parse_class_skills` and
// `SkillBook::set_class`.
void World::build_player_skills() {
    player_skills = tl::core::SkillBook{};
    player_skills_source.clear();
    if (player_class.empty()) return;
    ensure_player_skill_files();
    const std::string want = tl::core::upper_ascii(player_class);
    tl::formats::AdmFile class_file;
    for (const char* root : {"xbox/media/units/players/", "media/units/players/"}) {
        for (const std::string& path : data_paths_under(root)) {
            const std::vector<uint8_t> bytes = read_resource(path);
            if (bytes.empty()) continue;
            tl::formats::AdmFile file = parse_data_group(bytes.data(), bytes.size());
            if (!file.ok || tl::core::upper_ascii(file.string_or("NAME", "")) != want) continue;
            class_file = std::move(file);
            player_skills_source = path;
            break;
        }
        if (!player_skills_source.empty()) break;
    }
    if (player_skills_source.empty()) {
        TL_LOG_WARN("player skills: no class file names '%s'", player_class.c_str());
        return;
    }
    std::vector<tl::core::ClassSkill> entries = tl::core::parse_class_skills(class_file);
    bool repacked = false;
    const size_t off_grid = tl::core::pack_two_column_skill_grid(entries, &repacked);
    if (repacked) {
        TL_LOG_INFO("player skills: a three-column tree, re-packed onto two columns (%zu left off)",
                    off_grid);
    }
    const std::array<std::string, 3> tab_names = tl::core::parse_skill_tab_names(class_file);
    size_t missing = 0;
    player_skills.set_class(entries, [&](const std::string& name) -> const tl::core::SkillDef* {
        const auto it = player_skill_files.find(tl::core::upper_ascii(name));
        if (it != player_skill_files.end()) return &it->second->def;
        ++missing;
        TL_LOG_WARN("player skills: no skill file is called '%s'", name.c_str());
        return nullptr;
    });
    player_skills.tab_names = tab_names;
    TL_LOG_INFO("player skills: %zu from %s (%zu without a file)", entries.size(),
                player_skills_source.c_str(), missing);
}

// PORT: the port's index of the monster files, by NAME (archive lookup).
void World::ensure_monster_index() {
    if (monster_files_loaded) return;
    monster_files_loaded = true;
    for (const std::string& path : data_paths_under("media/units/monsters/")) {
        const std::vector<uint8_t> bytes = read_resource(path);
        if (bytes.empty()) continue;
        const tl::formats::AdmFile file = parse_data_group(bytes.data(), bytes.size());
        if (!file.ok || file.section() != "UNIT") continue;
        const std::string name = file.string_or("NAME", "");
        if (!name.empty()) monster_files.emplace(lowered(name), path);
    }
    TL_LOG_INFO("unit index: %zu monster unit(s)", monster_files.size());
}

void World::ensure_prop_index() {
    if (prop_files_loaded) return;
    prop_files_loaded = true;
    for (const std::string& path : data_paths_under("media/units/props/")) {
        const std::vector<uint8_t> bytes = read_resource(path);
        if (bytes.empty()) continue;
        const tl::formats::AdmFile file = parse_data_group(bytes.data(), bytes.size());
        if (!file.ok || file.section() != "UNIT") continue;
        const std::string name = file.string_or("NAME", "");
        if (!name.empty()) prop_files.emplace(lowered(name), path);
    }
    TL_LOG_INFO("prop index: %zu prop unit(s)", prop_files.size());
}

bool World::trigger_prop(const std::string& unit, TriggerProp& out) {
    const std::string key = lowered(unit);
    const auto cached = trigger_props.find(key);
    if (cached != trigger_props.end()) {
        out = cached->second;
        return !out.mesh.empty();
    }

    TriggerProp prop;
    ensure_prop_index();
    const auto found = prop_files.find(key);
    if (found != prop_files.end()) {
        const std::vector<uint8_t> bytes = read_resource(found->second);
        if (!bytes.empty()) {
            const tl::formats::AdmFile file = parse_data_group(bytes.data(), bytes.size());
            if (file.ok) {
                std::string directory = file.string_or("RESOURCEDIRECTORY", "");
                const std::string mesh_file = file.string_or("MESHFILE", "");
                const std::string collision_file = file.string_or("COLLISIONFILE", "");
                for (char& c : directory) {
                    if (c == 92) c = '/';
                }
                if (!directory.empty() && directory.back() != '/') directory.push_back('/');
                if (!directory.empty() && !mesh_file.empty()) {
                    prop.mesh = directory + mesh_file + ".MESH";
                }
                // A collision model needs no drawn one (`generic_trigger_collision_01`).
                if (!directory.empty() && !collision_file.empty()) {
                    prop.collision = directory + collision_file + ".MESH";
                }
                // `CTriggerUnit::addAnimations()`: a
                // state's `ANIMATION_STATE_LOOP_<n>` (looping, `+0x280`), else
                // `ANIMATION_STATE_<n>`; the transition only when that gave one.
                // INVENTED: names, looked up when played; the original keeps indices
                // (`+0x268`, `+0x298`), exact case, index 0 for a missing one.
                prop.resting = file.string_or("ANIMATION_STATE_1", "");
                prop.resting_loop = file.string_or("ANIMATION_STATE_LOOP_1", "");
                prop.activated = file.string_or("ANIMATION_STATE_2", "");
                prop.activated_loop = file.string_or("ANIMATION_STATE_LOOP_2", "");
                if (!prop.activated.empty() || !prop.activated_loop.empty()) {
                    prop.opening = file.string_or("ANIMATION_TRANSITION_STATE_2", "");
                }
                if (!prop.resting.empty() || !prop.resting_loop.empty()) {
                    prop.closing = file.string_or("ANIMATION_TRANSITION_STATE_1", "");
                }
                prop.found = true;
                prop.max_states = static_cast<int32_t>(file.integer_or("MAXSTATES", 2));
                {
                    const std::string loop = tl::core::upper_ascii(file.string_or("LOOPTYPE", ""));
                    static const char* const kLoopTypes[] = {"NO LOOP", "CYCLE", "BACK AND FORTH"};
                    for (int32_t i = 0; i < 3; ++i) {
                        if (loop == kLoopTypes[i]) {
                            prop.loop_type = i;
                            break;
                        }
                    }
                }
                {
                    // `CTriggerUnit::addSoundsToState(state, key)`:
                    // the key's sound, else `INTERACT_SOUND`.
                    const std::string interact = file.string_or("INTERACT_SOUND", "");
                    prop.state_sound[0] = file.string_or("SOUND_STATE_ONE", "");
                    prop.state_sound[1] = file.string_or("SOUND_STATE_TWO", "");
                    for (std::string& sound : prop.state_sound) {
                        if (sound.empty()) sound = interact;
                    }
                }
                prop.collideable = file.bool_or("COLLIDEABLE", true);
                prop.block = file.bool_or("BLOCK", false);
                prop.occupies_nodes = file.bool_or("OCCUPIESNODES", true);
                prop.shadows = file.bool_or("SHADOWS", true);
                prop.state_layout[0] = file.string_or("LAYOUT_STATE_1", "");
                prop.state_layout[1] = file.string_or("LAYOUT_STATE_2", "");
                prop.display_name = tl::core::strip_markup(file.string_or("DISPLAYNAME", ""));
                if (prop.display_name.empty()) prop.display_name = file.string_or("NAME", "ITEM");
                prop.unittype = file.string_or("UNITTYPE", "");
                prop.collision_radius = file.float_or("COLLISION_RADIUS", 0.3f);
                prop.scale = file.float_or("SCALE", 1.0f);
                prop.scale_variation = file.float_or("SCALE_VARIATION", 0.0f);
                for (const tl::formats::AdmSection& group : file.root.children) {
                    if (group.name == "TREASURE") {
                        prop.treasure_class = group.string_or("SPAWNCLASS", "");
                        prop.treasure_min = static_cast<int32_t>(group.integer_or("MINCOUNT", 1));
                        prop.treasure_max = static_cast<int32_t>(group.integer_or("MAXCOUNT", 1));
                    } else if (group.name == "SKILL") {
                        const std::string skill = group.string_or("NAME", "");
                        if (skill.empty()) continue;
                        prop.skills.push_back(skill);
                        prop.skill_levels.push_back(
                            static_cast<uint32_t>(group.integer_or("LEVEL", 1)));
                    }
                }
            }
        }
    }
    trigger_props.emplace(key, prop);
    out = prop;
    return !prop.mesh.empty();
}

std::string World::prop_layout_path(const std::string& unit) {
    ensure_prop_index();
    const auto found = prop_files.find(lowered(unit));
    if (found == prop_files.end()) return std::string();

    const std::vector<uint8_t> bytes = read_resource(found->second);
    if (bytes.empty()) return std::string();
    const tl::formats::AdmFile file = parse_data_group(bytes.data(), bytes.size());
    if (!file.ok) return std::string();

    std::string directory = file.string_or("RESOURCEDIRECTORY", "");
    const std::string layout_file = file.string_or("LAYOUTFILE", "");
    if (directory.empty() || layout_file.empty()) return std::string();
    for (char& c : directory) {
        if (c == '\\') c = '/';
    }
    if (directory.back() != '/') directory.push_back('/');
    return directory + layout_file + ".LAYOUT";
}

const World::ParticleFileExtras* World::particle_extras(const std::string& reference) {
    std::string path = resolve_archive_path(reference);
    if (path.empty()) path = resolve_archive_path(reference + ".cmp");
    if (path.empty()) return nullptr;
    const auto found = particle_file_extras.find(path);
    if (found != particle_file_extras.end()) return &found->second;
    ParticleFileExtras& extras = particle_file_extras[path];
    const tl::formats::CmpScene* const layout = layout_file(path);
    if (layout == nullptr || !layout->ok) return &extras;

    // `CParticlePreloader::LoadParticle`: the file's
    // `Sound` objects whose GUID names a definition, each at its node's `VOLUME`
    // (`CSoundObjectDescriptor::Set_setVolume`) or 1. A looping one is refused.
    ensure_sound_index();
    namespace type = tl::formats::cmp_type;
    for (const tl::formats::CmpNode& node : layout->nodes) {
        if (node.type != type::kSound) continue;
        namespace f = tl::formats::cmp_field::sound;
        const tl::formats::CmpField* guid = node.field(f::kGuid);
        if (guid == nullptr || guid->as_text().empty() || guid->as_text() == "-1") continue;
        const SoundDefinition* definition = sound_by_guid(guid->as_text());
        if (definition == nullptr) continue;
        if (definition->loops) {
            // The original's own line (`logMessage(..., 3)`), once per cached file; the
            // shipped data has such files.
            TL_LOG_INFO("Particles can't have sounds that loop. : %s", path.c_str());
            continue;
        }
        ParticleFileExtras::Sound sound;
        sound.guid = guid->as_text();
        if (const tl::formats::CmpField* volume = node.field(f::kVolume);
            volume != nullptr && volume->count() == 1) {
            sound.volume = volume->as_float();
        }
        extras.sounds.push_back(sound);
    }

    // Its `Camera Shake` objects, cloned (`CCameraShake::clone`):
    // `CCameraShakeDescriptor`'s setters over the constructor's
    // defaults.
    for (const tl::formats::CmpNode& node : layout->nodes) {
        if (node.type != type::kCameraShake) continue;
        load_camera_shake_graphs();
        extras.shakes.push_back(tl::core::camera_shake_from_node(node, camera_shake_graphs));
    }
    return &extras;
}

const tl::formats::CmpScene* World::cached_layout(const std::string& reference) {
    std::string key = tl::core::upper_ascii(reference);
    for (char& c : key) {
        if (c == '\\') c = '/';
    }
    const auto found = cached_layouts.find(key);
    if (found != cached_layouts.end()) return found->second;
    std::string path = resolve_archive_path(key);
    if (path.empty()) path = resolve_archive_path(key + ".cmp");
    const tl::formats::CmpScene* out = path.empty() ? nullptr : layout_file(path);
    cached_layouts.emplace(key, out);
    return out;
}

// `CSkillEvent::applyEffects` and
// `applyWeaponDamage`: `CLevel::rayCollision`
// from master owner to target, each `getPosition(true)` (the
// body's centre) raised by 1, skipping `objectRayCollision`: level geometry only.
bool World::skill_event_sight_blocked(const SkillEventSource& src, tl::core::LevelScript* machine,
                                      int64_t node) {
    if (!src.spec.damage_requires_los) return false;
    const float player_body =
        player_stats.radius > 0.0f ? player_stats.radius : tl::core::kDefaultCollisionRadius;
    const tl::core::Vec3 player_centre = {player.position().x, player.position().y + player_body,
                                          player.position().z};
    tl::core::Vec3 from = player_centre;
    tl::core::Vec3 to = player_centre;
    if (src.caster_machine != nullptr && !src.caster_machine->unit_centre(src.caster_node, from)) {
        return false;
    }
    if (machine != nullptr && !machine->unit_centre(node, to)) return false;
    from = {from.x, from.y + 1.0f, from.z};
    to = {to.x, to.y + 1.0f, to.z};
    tl::core::CollisionHit hit;
    if (!collision.ray_collision(from, to, /*objects=*/false, hit)) return false;
    TL_LOG_INFO(
        "skills: '%s' has no line of sight: (%.2f, %.2f, %.2f) to (%.2f, %.2f, %.2f) meets "
        "the level at (%.2f, %.2f, %.2f); nothing applied",
        src.skill.c_str(), static_cast<double>(from.x), static_cast<double>(from.y),
        static_cast<double>(from.z), static_cast<double>(to.x), static_cast<double>(to.y),
        static_cast<double>(to.z), static_cast<double>(hit.point.x),
        static_cast<double>(hit.point.y), static_cast<double>(hit.point.z));
    return true;
}

// `CSkillEvent::applyWeaponDamage(null, item, 1, 1, false, false)`:
// nothing with no character invoking (a prop's trap) or no `WEAPONDAMAGEPCT`; the
// invoker's centre within 3 of the item's height; `DAMAGE_REQUIRES_LOS` with the level between
// them (each raised by 1) is true with no blow. Then the item's `interact`, from the weapon in hand
// or, with none, `performAttack` at the item set as target.
// With `SINGLETARGET` an item passes only when the event has no original target.
bool World::skill_event_item_blow(const SkillEventSource& src,
                                  const tl::core::DamageShapeItem& item) {
    if (src.prop_owner || src.spec.weapon_damage == 0.0f) return false;
    if (src.single_target && src.has_original_target) return false;
    tl::core::Vec3 from = player.position();
    if (src.caster_machine == nullptr) {
        from.y +=
            player_stats.radius > 0.0f ? player_stats.radius : tl::core::kDefaultCollisionRadius;
    } else if (!src.caster_machine->unit_centre(src.caster_node, from)) {
        return false;
    }
    if (std::fabs(from.y - item.at.y) > 3.0f) return false;
    if (src.spec.damage_requires_los) {
        tl::core::CollisionHit hit;
        if (collision.ray_collision({from.x, from.y + 1.0f, from.z},
                                    {item.at.x, item.at.y + 1.0f, item.at.z},
                                    /*objects=*/false, hit)) {
            return true;
        }
    }
    queue_item_interact(item.id, /*by_player=*/src.caster_machine == nullptr);
    return true;
}

bool World::skill_event_strike(const SkillEventSource& src, tl::core::LevelScript* machine,
                               int64_t node) {
    // `characterEffectedByDamageShape` and `startEvent`'s
    // `APPLYEFFECTS`: `applyEffects`, `applyWeaponDamage`,
    // `invokeHitSkills`.
    const bool applied = skill_event_effects_on(src, machine, node);
    // A prop's trap: `applyWeaponDamage` does nothing with no character invoking.
    // INVENTED: its hit skills, which the trap's own skill would fire, are not run
    // (no shipped trap authors one).
    if (src.prop_owner) return applied;
    skill_event_weapon_blow(src, machine, node, 1.0f, 1.0f);
    invoke_hit_skills(src, src.group, machine, node);
    return applied;
}

// PORT: a player skill found in the book, or among the skills executed by name.
const tl::core::KnownSkill* World::player_skill(const std::string& name) {
    if (const tl::core::KnownSkill* known = player_skills.find_by_name(name)) return known;
    const auto found = executed_skills.find(tl::core::upper_ascii(name));
    return found != executed_skills.end() ? &found->second : nullptr;
}

const tl::core::KnownSkill* World::add_executed_skill(const std::string& name, uint32_t level) {
    // `CSkillManager::addSkill(name, false)`: the class's
    // own skill, else a hidden new one (`+0x6b`); then `_setLevelOfSkillFromSkillManager`.
    if (tl::core::KnownSkill* known = player_skills.find_by_name(name)) return known;
    ensure_player_skill_files();
    const std::string key = tl::core::upper_ascii(name);
    const auto file = player_skill_files.find(key);
    if (file == player_skill_files.end()) return nullptr;
    tl::core::KnownSkill& skill = executed_skills[key];
    skill.entry.name = file->second->def.name;
    skill.def = &file->second->def;
    skill.invested = level;
    skill.effective = level;
    return &skill;
}

bool World::player_skill_target_valid(const tl::core::KnownSkill& skill, const ViewUnit& unit) {
    // `CSkill::getTargetIsValid(candidate, owner, at, false)`,
    // the player `GOOD` (1); `TARGET_SPECIFIC_UNITS` read as
    // `LevelScript::unit_skill_target_valid` reads it.
    tl::core::SkillTargetQuery query;
    const tl::core::SkillProperty* property = skill.property(skill.effective);
    query.has_property = property != nullptr;
    query.target_type = property != nullptr ? property->target_type : 0;
    query.target_alignment = skill.def != nullptr ? skill.def->target_alignment : 2;
    query.dead_wanted = skill.def != nullptr && skill.def->target_corpses;
    query.owner_alignment = 1;
    query.owner.alive = !player_health.dead;
    query.owner.is_owner = true;
    // `TARGET_UNITTYPE` (property +0x74): `ISA`, an id of 0 for none. INVENTED: the player's
    // own type is its class; `Player`, which every class ISAs.
    const uint32_t want_type = property != nullptr && !property->target_unit_type.empty()
                                   ? unit_types.id_of(property->target_unit_type)
                                   : 0;
    query.owner.unit_type_matches =
        want_type == 0 || unit_types.isa(tl::core::unit_type::kPlayer, want_type);
    // `+0x1b0` among `TARGET_SPECIFIC_UNITS`, when it lists any; no list names a class.
    const bool listed = skill.def != nullptr && !skill.def->target_specific_units.empty();
    query.owner.specific_unit = !listed;
    tl::core::SkillTargetFacts& t = query.candidate;
    if (unit.player) {
        t = query.owner;
    } else if (unit.machine != nullptr) {
        t.alive = !unit.machine->unit_is_dead(unit.node);
        if (listed) {
            const std::string wanted = tl::core::upper_ascii(unit.machine->unit_of(unit.node));
            const std::vector<std::string>& names = skill.def->target_specific_units;
            t.specific_unit = std::find(names.begin(), names.end(), wanted) != names.end();
        }
        t.unit_type_matches = want_type == 0 || unit.machine->character_isa(unit.node, want_type);
        // `+0x640`: the player's pet has the player for its master.
        t.master_is_owner = unit.machine->unit_is_player_pet(unit.node);
        t.enemy = unit.machine->unit_is_enemy_of(unit.node, nullptr, 0);
    }
    return tl::core::skill_target_is_valid(query);
}

void World::skill_unit_spawned(const SkillEventSource& source, tl::core::LevelScript& machine,
                               int64_t node) {
    // `CSkillEvent::unitSpawnedFromUnitSpawner`: the
    // caster's side and level (`setLevel(+0x100, false)`, vtable +0x318), then `MAKEPET`.
    if (source.caster_machine != nullptr) {
        // A placed unit's summon is on that unit's side, at its level.
        machine.set_unit_side(node, source.caster_machine->unit_side(source.caster_node)
                                        .value_or(tl::core::UnitStats::Alignment::Hostile));
        const int level = effective_monster_level(source.caster_node);
        machine.set_unit_level(node, level);
        if (source.spec.make_pet) {
            // `CCharacter::addPet` -> `setMaster`.
            add_monster_pet(machine, node, *source.caster_machine, source.caster_node);
        }
        skill_event_effects_on(source, &machine, node);
        TL_LOG_INFO("skill event: %s's '%s' summons %s (level %d)",
                    source.caster_machine->unit_of(source.caster_node).c_str(),
                    source.skill.c_str(), machine.unit_of(node).c_str(), level);
        return;
    }
    machine.set_unit_side(node, tl::core::UnitStats::Alignment::Friendly);
    machine.set_unit_level(node, static_cast<int>(player_character.level()));
    if (source.spec.make_pet) add_player_pet(machine, node);
    if (source.spec.synch_pets_to_owner) {
        // INVENTED: the caster's animation and its time are not handed on.
        TL_LOG_WARN("skill event: '%s' SYNCH_PETS_TO_OWNER is not carried", source.skill.c_str());
    }
    // `applyEffects(unit)`: onto the summon's manager, filtered for the caster's pet.
    skill_event_effects_on(source, &machine, node);
    const std::string unit = machine.unit_of(node);
    if (source.spec.match_pet_duration) {
        // INVENTED: `+0x15c = getEffectValue(unit, 0x2f, 7)`: no per-event time left, so
        // `SUMMON DURATION` is not read back.
        TL_LOG_INFO("skill event: '%s' MATCH_PET_DURATION: the summon holds %.1fs",
                    source.skill.c_str(),
                    static_cast<double>(machine.unit_effects(node) != nullptr
                                            ? machine.unit_effects(node)->effect_value(0x2f)
                                            : 0.0f));
    }
    TL_LOG_INFO("skill event: '%s' summons %s (level %u)%s", source.skill.c_str(), unit.c_str(),
                player_character.level(), source.spec.make_pet ? " as a pet" : "");
}

const tl::formats::CmpNode* World::missile_named(const std::string& name) {
    // `CMissilePreloader::CMissilePreloader` loads
    // nothing until asked; `CMissilePreloader::reloadMissiles`:
    // every missile `*.layout` (`CMissilePreloader::loadMissile`),
    // each `Missile` kept by its upper-cased `MISSILE NAME` (`CMissile +0x2c8`),
    // which a spawner's `RESOURCE` names. Empty names skipped; the first of a name kept.
    // INVENTED: `getFileList`'s order is not read; the sorted paths.
    if (!missiles_indexed) {
        missiles_indexed = true;
        for (const std::string& path : archive_paths_under("media/missiles/", ".layout.cmp")) {
            const std::string reference = path.substr(0, path.size() - 4);
            const tl::formats::CmpScene* layout = cached_layout(reference);
            if (layout == nullptr) continue;
            for (const tl::formats::CmpNode& n : layout->nodes) {
                if (n.type != tl::formats::cmp_type::kMissile) continue;
                std::string key = tl::core::read_missile_spec(n).name;
                if (key.empty()) continue;
                missiles_by_name.emplace(std::move(key), &n);
            }
        }
        TL_LOG_INFO("missiles: %zu by name from media/missiles/", missiles_by_name.size());
    }
    const auto found = missiles_by_name.find(tl::core::upper_ascii(name));
    return found != missiles_by_name.end() ? found->second : nullptr;
}

// `CSpawnClassParser::createEmptySpawnClass`:
// the file read and parsed (`parse_spawn_class`), and the root's `LEVEL_BONUS`.
std::optional<tl::core::SpawnClass> World::load_spawn_class(
    const std::string& name, const tl::core::SpawnContext& context) const {
    const std::string path = spawn_class_path(name);
    if (path.empty()) return std::nullopt;
    const std::vector<uint8_t> bytes = read_resource(path);
    if (bytes.empty()) return std::nullopt;
    const tl::formats::AdmFile file = parse_data_group(bytes.data(), bytes.size());
    if (!file.ok) return std::nullopt;
    tl::core::SpawnClass made = tl::core::parse_spawn_class(file.root, context);
    // `createEmptySpawnClass`'s one value: the root's `LEVEL_BONUS` (0).
    made.set_level_bonus(static_cast<int32_t>(file.root.integer_or("LEVEL_BONUS", 0)));
    return made;
}

/// Resolve a `Unit Spawner`'s `RESOURCE` (by `GROUP`: a unit name, or a weighted spawn
/// class) to a unit: CSpawnClass::rollSpawnClass and
/// CSpawnClassData::rollSpawnClassData, on `g_Rand` as
/// it stands; the first unit, as `loadAndEquipSpawnClass` uses.
std::string World::resolve_spawn_resource(const std::string& group, const std::string& resource,
                                          int depth, int32_t level, uint32_t flags) {
    if (resource.empty()) return {};
    // A table may name another; four stops a cycle.
    constexpr int kMaxDepth = 4;
    if (depth > kMaxDepth) return {};

    const bool spawn_class = group == "Spawn Class";
    if (!spawn_class) {
        // `Monsters` and `Props` name a unit; `Items` and anything else are not handled.
        if (group == "Monsters" || group == "Props" || group.empty()) return resource;
        return {};
    }

    SpawnTables tables;
    build_spawn_context(tables, /*items_by_type=*/false);
    std::optional<tl::core::SpawnClass> loaded = load_spawn_class(resource, tables.context);
    if (!loaded) return {};
    const tl::core::SpawnContext& context = tables.context;
    const tl::core::SpawnClass& sc = *loaded;
    std::vector<std::string> rolled;

    // DIVERGENCE: rollSpawnClass can return several units (MINCOUNT/MAXCOUNT); only the
    // first is returned.
    sc.roll_spawn_class(rolled, nullptr, nullptr, nullptr, level, 0xffffffffu,
                        /*level_bonus=*/0, /*rarity_override=*/-1, /*magic_find=*/0, flags,
                        tl::core::game_random(), context);

    if (rolled.empty()) return {};
    return rolled[0];
}

// PORT: the port's own id for a spawned unit.
int64_t World::next_spawned_id() { return kSpawnedNodeBase + static_cast<int64_t>(++spawn_serial); }

bool World::entity_still_placed(const Ogre::Entity* entity) const {
    if (entity == nullptr) return false;
    for (const auto& entry : scene_units) {
        if (entry.second.entity == entity) return true;
    }
    for (const auto& owner : spawned) {
        for (const SpawnedUnit& made : owner.second) {
            if (made.entity == entity) return true;
        }
    }
    return false;
}

tl::core::ItemStats World::create_item(const tl::core::ItemStats& data, bool from_save,
                                       bool random_enchant_allowed, bool force_enchant) {
    // `CResourceManager::createItem` (by GUID,
    // by name): `createNewItem`, `+0x20a`, `unitInit(data, from_save)`,
    // then (by GUID only) the forced `enchant(true)`.
    tl::core::ItemStats copy = data;
    if (copy.is_gold()) {
        // `createNewItem` makes a `CItemGold`
        // (`CItemGold::unitInit`); `enchant` is `CEquipment`'s.
        load_stat_graphs();
        tl::core::init_gold_copy(copy, from_save, level_monster_level, config.difficulty,
                                 stat_graphs, tl::core::volatile_random(), gold_sounds());
        return copy;
    }
    tl::core::init_item_copy(copy, from_save, random_enchant_allowed, force_enchant,
                             item_roll_context());
    return copy;
}

tl::core::SkillCastContext World::player_cast_context() {
    // Self-targeted or passive: caster and target are the same character.
    tl::core::SkillCastContext context;
    context.target = &player_effects;
    context.types = &effect_types;
    context.unit_types = &unit_types;
    context.affix = [this](const std::string& name) -> const tl::core::Affix* {
        return affixes.get_affix(name);
    };
    context.target_is_caster = true;
    context.target_is_enemy = false;
    context.target_is_casters_pet = false;
    context.source = tl::core::EffectSource{true, nullptr, 0};
    context.owner_level = player_character.level();
    // Re-read the caster every time (mana, weapons).
    cast_caster = player_caster();
    context.caster = &cast_caster;
    // `CSkillEvent::startEvent`'s layout, for every block a group fires.
    context.on_event = [this](const tl::core::KnownSkill& skill, uint32_t level, int group,
                              const tl::formats::AdmSection& event,
                              const tl::core::SkillEventTarget& target) {
        start_skill_event(skill, level, group, event, target);
    };
    context.events_alive = [this](const std::string& skill, uint32_t run) {
        return skill_events_alive(skill, run, nullptr, 0);
    };
    context.events_can_stop = [this](const std::string& skill, uint32_t run) {
        return skill_events_can_stop(skill, run, nullptr, 0);
    };
    context.reset_events = [this](const std::string& skill, uint32_t run) {
        reset_skill_events(skill, run, nullptr, 0);
    };
    context.reset_event = [this](const std::string& skill, uint32_t run,
                                 const tl::formats::AdmSection& block) {
        detach_event_missiles(skill, run, block, nullptr, 0);
    };
    // `applyEffectOnUnit` on the player:
    // `isEffectValidForUnit` (vtable +0x238), then `applyInstantEffect`:
    // `OPEN TOWN PORTAL` is `openPortal`; `DESUMMON MONSTER`
    // (0x43) is `CCharacter::removePet(NAME)`.
    // INVENTED: other instant types apply nothing.
    context.instant_effect = [this](const tl::core::Effect& effect) {
        player_instant_effect(effect);
    };
    return context;
}

void World::player_instant_effect(const tl::core::Effect& effect) {
    if (effect.type >= 0 && effect.type == effect_types.index_of("DESUMMON MONSTER")) {
        int removed = 0;
        for (const MachineView& view : machines()) {
            std::vector<tl::core::ScriptEvent> events;
            removed += view.script->desummon_player_pets(effect.name, quest_log.state(), events);
            if (!events.empty()) apply_script_events(events, *view.nodes);
        }
        TL_LOG_INFO("skills: %d pet(s) called '%s' desummoned", removed, effect.name.c_str());
        return;
    }
    if (effect.type == tl::core::kEffectTypeDamage) {
        player_instant_damage(effect);
        return;
    }
    // Cases 7 HP RECHARGE and 0x7c HP RECHARGE PLAYER: a value of 0 or more is
    // `modifyHP(value)`, held to `maxHP()`; at 0 health only a whole-`maxHP()` heal lands.
    if (effect.type == tl::core::kEffectTypeHpRecharge ||
        effect.type == tl::core::kEffectTypeHpRechargePlayer) {
        const tl::core::EffectOwner source =
            instant_caster_none ? tl::core::EffectOwner{} : skill_effects.owner;
        const float value =
            tl::core::effect_base_value(effect, tl::core::EffectCalculate::Roll, stat_graphs,
                                        source, &tl::core::volatile_random());
        // Below 0 it goes on as case 0x34 `DAMAGE` does.
        if (value < 0.0f) {
            player_instant_damage(effect, &value);
            return;
        }
        if (!(0.0f <= value) || player_health.dead || !(player_health.max > 0.0f)) return;
        const float ceiling = static_cast<float>(static_cast<int>(player_health.max));
        if (player_health.current <= 0.0f && 0.0f < value && value < ceiling) return;
        player_health.current = std::min(ceiling, player_health.current + value);
        return;
    }
    // Case 0x7f `STOP SKILL`: a `v(0, 100)` at or under the value
    // stops every copy of `NAME`, `stopSkillsByName(NAME, true)`.
    if (effect.type == tl::core::kEffectTypeStopSkill) {
        const tl::core::EffectOwner source =
            instant_caster_none ? tl::core::EffectOwner{} : skill_effects.owner;
        const float value =
            tl::core::effect_base_value(effect, tl::core::EffectCalculate::Roll, stat_graphs,
                                        source, &tl::core::volatile_random());
        const int roll = tl::core::volatile_random().integer_between(0, 100);
        if (value < static_cast<float>(roll)) return;
        player_skill_runner.stop_by_name(effect.name, player_cast_context(),
                                         [this](const std::string& n) { return player_skill(n); });
        return;
    }
    // Cases 0x72..0x78 (the permanent attributes, `AWARD STATPOINT`) and 0x8f `RESPEC`,
    // as an item's use applies them.
    if ((tl::core::kEffectTypePermanentStrength <= effect.type &&
         effect.type <= tl::core::kEffectTypeAwardStatpoint) ||
        effect.type == tl::core::kEffectTypeRespec) {
        const float value = tl::core::effect_base_value(effect, tl::core::EffectCalculate::Roll,
                                                        stat_graphs, tl::core::EffectOwner{});
        switch (effect.type) {
            case tl::core::kEffectTypePermanentStrength:
                player_character.add_permanent_attribute(tl::core::StatType::Strength, value);
                break;
            case tl::core::kEffectTypePermanentDexterity:
                player_character.add_permanent_attribute(tl::core::StatType::Dexterity, value);
                break;
            case tl::core::kEffectTypePermanentMagic:
                player_character.add_permanent_attribute(tl::core::StatType::Magic, value);
                break;
            case tl::core::kEffectTypePermanentDefense:
                player_character.add_permanent_attribute(tl::core::StatType::Defense, value);
                break;
            case tl::core::kEffectTypePermanentHealth:
                player_character.add_permanent_hp(value);
                refresh_player_maxima();
                break;
            case tl::core::kEffectTypePermanentMana:
                player_character.add_permanent_mp(value);
                refresh_player_maxima();
                break;
            case tl::core::kEffectTypeAwardStatpoint:
                player_character.award_stat_points(value);
                break;
            case tl::core::kEffectTypeRespec: respec_player(); break;
            default: break;
        }
        return;
    }
    // Case 0x71, `WARP`, the player's only:
    // `CGameClient::warpLevels(NAME, 0, (int)value(), false, L"", false)`.
    constexpr int kEffectTypeWarp = 0x71;
    if (effect.type == kEffectTypeWarp) {
        const float value = tl::core::effect_column_value(effect, 0, stat_graphs);
        warp_by_effect(effect.name, static_cast<int32_t>(value));
        return;
    }
    if (effect.type != tl::core::kEffectTypeOpenTownPortal &&
        effect.type != tl::core::kEffectTypeOpenPortal) {
        return;
    }
    load_unit_types();
    const tl::core::ItemUseTarget target = player_use_target();
    if (!tl::core::effect_valid_for_unit(target, nullptr, effect, 0.0f, unit_types)) return;
    if (effect.type == tl::core::kEffectTypeOpenPortal) {
        // Case 0x8d: `openMapPortal(NAME, level)` (vtable +0x1c4).
        open_map_portal(effect.name);
        return;
    }
    // Case 0x2d: `openPortal(level)` (vtable +0x1c0),
    // then `incrementJournalStatistic(0xd, 1)`.
    open_town_portal();
    count_journal(tl::core::JournalStatistic::PortalsUsed);
}

tl::core::ItemUseTarget World::player_use_target() {
    tl::core::ItemUseTarget target;
    target.hp = player_health.current;
    target.max_hp = player_health.max;
    target.mana = player_character.mp();
    target.max_mana = player_character.max_mp();
    target.damageable = true;
    target.unittype = "PLAYER";
    target.level = player_character.level();
    target.effects = &player_effects;
    target.inventory = &player_inventory;
    target.level_allows_portals = tl::core::town_portal_allowed(portal_level());
    target.level_allows_map_portals = tl::core::map_portal_allowed(portal_level());
    target.stunned = player_themes.themes.has_unit_theme("STUNNED", &unit_themes);
    target.dying_or_dead = player_health.dead;
    // `LEARN SKILL`: the player's skill manager and spell slots.
    target.skills = &player_skills;
    target.pet = false;
    target.character = &player_character;
    target.skill_holds = tl::core::skill_holds_character(player_active_skill);
    target.effect_types = &effect_types;
    target.skill_def = [this](const std::string& name) -> const tl::core::SkillDef* {
        const auto it = player_skill_files.find(tl::core::upper_ascii(name));
        if (it != player_skill_files.end()) return &it->second->def;
        // `learnSpell`'s "Skill not found : <name>".
        TL_LOG_WARN("Skill not found : %s", name.c_str());
        return nullptr;
    };
    return target;
}

tl::core::SkillCaster World::player_caster() const {
    tl::core::SkillCaster caster;
    caster.mana = player_character.mp();
    caster.unit_types = &unit_types;
    caster.effects = &player_effects;
    caster.inventory = &player_inventory;
    // `THEMEREQUIRED` / `NOTTHEMED` ask the caster's themes.
    caster.theme_book = &unit_themes;
    caster.themes = &player_themes.themes;
    // `CCharacter::getWeaponInRightHand` and `InLeftHand`: the two
    // equip locations.
    const std::vector<tl::core::InventorySlot>& slots = player_inventory.slots();
    const auto hand = [&slots](int location) -> std::string {
        if (location < 0 || static_cast<size_t>(location) >= slots.size()) return std::string();
        if (slots[static_cast<size_t>(location)].count <= 0) return std::string();
        return slots[static_cast<size_t>(location)].item.unittype;
    };
    caster.right_hand_type = hand(tl::core::equip_location::kRightHand);
    caster.left_hand_type = hand(tl::core::equip_location::kLeftHand);
    caster.model = player_catalog.empty() ? nullptr : &player_catalog;
    caster.is_character = true;
    // `CPositionableObject::getPosition(true)`: the radius
    // (`+0x194`) above the feet.
    const float body =
        player_stats.radius > 0.0f ? player_stats.radius : tl::core::kDefaultCollisionRadius;
    caster.position = {player.position().x, player.position().y + body, player.position().z};
    // `CAttackDescription +0x10` of each hand's attack (`CEquipment +0x238`, `+0x23c`): the
    // prefix `calculateCombatStats` gives the item.
    if (!caster.right_hand_type.empty()) {
        caster.right_hand_attack_animation =
            tl::core::attack_animation_class(caster.right_hand_type, tl::core::Hand::Right);
    }
    if (!caster.left_hand_type.empty()) {
        caster.left_hand_attack_animation =
            tl::core::attack_animation_class(caster.left_hand_type, tl::core::Hand::Left);
    }
    return caster;
}

// `CSkillEvent::reset`: the missile list half (vtable
// +0x24 and safe pointers are the port's lifetime). Matched by skill, copy, caster and
// block, so a clone's missiles go too.
void World::detach_event_missiles(const std::string& skill, uint32_t run,
                                  const tl::formats::AdmSection& block,
                                  tl::core::LevelScript* caster_machine, int64_t caster_node) {
    for (FlyingMissile& flying : missiles) {
        if (flying.level_placed || flying.by_weapon || flying.detached) continue;
        if (flying.source.event != &block || flying.source.skill != skill ||
            flying.source.run != run || flying.source.caster_machine != caster_machine ||
            flying.source.caster_node != caster_node) {
            continue;
        }
        flying.detached = true;
        TL_LOG_INFO("missiles: '%s' %s left without its event (restarted)", skill.c_str(),
                    flying.missile.spec.name.c_str());
    }
}

float World::skill_event_weapon_blow(const SkillEventSource& source, tl::core::LevelScript* machine,
                                     int64_t node, float damage_scale, float soak_scale) {
    // `applyWeaponDamage`: with `WEAPONDAMAGEPCT`, the
    // caster's right-hand `performAttack` at that share, soak by `SOAKSCALEPCT`;
    // `DAMAGE_REQUIRES_LOS` first.
    if (skill_event_sight_blocked(source, machine, node)) return 0.0f;
    if (source.caster_machine != nullptr) {
        // A placed unit's skill: its own `rollAttack` at the event's shares.
        if (source.spec.weapon_damage == 0.0f) return 0.0f;
        if (machine != nullptr && machine->unit_is_dead(node)) return 0.0f;
        std::vector<tl::core::ScriptEvent> events;
        if (!source.caster_machine->unit_skill_blow(source.caster_node, machine, node,
                                                    damage_scale * source.spec.weapon_damage,
                                                    soak_scale * source.spec.soak_scale, events)) {
            return 0.0f;
        }
        const float amount = events.empty() ? 0.0f : events.back().amount;
        if (NodeMap* nodes = nodes_of(source.caster_machine)) apply_script_events(events, *nodes);
        return amount;
    }
    if (source.spec.weapon_damage == 0.0f || machine == nullptr || machine->unit_is_dead(node)) {
        return 0.0f;
    }
    const float weapon_part = player_blow(node, damage_scale * source.spec.weapon_damage,
                                          soak_scale * source.spec.soak_scale);
    const bool critical = last_blow_critical;
    const tl::core::KillingBlow killing = last_killing_blow;
    show_player_blow(node, machine->unit_of(node), weapon_part);
    if (weapon_part > 0.0f) {
        hurt_unit(node, machine->unit_of(node), weapon_part, critical, 0, machine, &killing,
                  /*struck=*/true);
    }
    // The weapon half is `rollAttack`'s, which slows the game on a kill.
    slow_on_player_kill(machine->unit_is_dead(node));
    return weapon_part;
}

// `CSkillEvent::invokeHitSkills(CCharacter*)`: the
// event's skill fires group 4 (`EVENT_UNITHIT`) at the struck unit, and group 5
// (`EVENT_UNITDIE`) when its `HP()` is under 1, never its own group.
void World::invoke_hit_skills(const SkillEventSource& source, int group,
                              tl::core::LevelScript* machine, int64_t node) {
    tl::core::LevelScript* const caster_machine = source.caster_machine;
    tl::core::LevelScript::UnitCasting* casting =
        caster_machine != nullptr ? caster_machine->unit_casting(source.caster_node) : nullptr;
    const tl::core::KnownSkill* skill =
        caster_machine != nullptr
            ? unit_skill_named(*caster_machine, source.caster_node, source.skill)
            : player_skill(source.skill);
    if (skill == nullptr || (caster_machine != nullptr && casting == nullptr)) return;
    // The unit's centre (`getPosition(true)`) and turn, the unit itself, no point.
    tl::core::SkillEventTarget target;
    target.has_unit = true;
    target.machine = machine;
    target.unit = node;
    float hp = 0.0f;
    if (machine == nullptr) {
        const float body =
            player_stats.radius > 0.0f ? player_stats.radius : tl::core::kDefaultCollisionRadius;
        const tl::core::Vec3 feet = player.position();
        target.origin = tl::core::Vec3{feet.x, feet.y + body, feet.z};
        target.yaw = player.facing();
        hp = player_health.dead ? 0.0f : player_health.current;
    } else {
        tl::core::Vec3 feet;
        if (!machine->unit_position(node, feet)) return;
        target.origin = tl::core::Vec3{feet.x, feet.y + machine->unit_radius(node), feet.z};
        float yaw = 0.0f;
        if (machine->unit_facing(node, yaw)) target.yaw = yaw;
        float max = 0.0f;
        machine->unit_health(node, hp, max);
    }
    target.run = source.run;
    constexpr int kUnitHit = 4;
    constexpr int kUnitDie = 5;
    if (caster_machine != nullptr) {
        tl::core::SkillCastContext context =
            unit_cast_context(*caster_machine, source.caster_node, machine, node, true);
        context.event_target = target;
        const UnitEventCaster saved = unit_event_caster;
        unit_event_caster = unit_event_caster_for(caster_machine, source.caster_node);
        if (group != kUnitHit) casting->runner.trigger_skill_event(*skill, kUnitHit, context);
        if (hp < 1.0f && group != kUnitDie) {
            casting->runner.trigger_skill_event(*skill, kUnitDie, context);
        }
        unit_event_caster = saved;
        return;
    }
    tl::core::SkillCastContext context = player_cast_context();
    context.event_target = target;
    // On the manager the skill runs on: the player's, or a worn item's.
    tl::core::SkillRunner& runner = skill_runner_running(skill->entry.name);
    if (group != kUnitHit) runner.trigger_skill_event(*skill, kUnitHit, context);
    if (hp < 1.0f && group != kUnitDie) runner.trigger_skill_event(*skill, kUnitDie, context);
}

void World::add_monster_pet(tl::core::LevelScript& machine, int64_t node,
                            tl::core::LevelScript& master_machine, int64_t master_node) {
    // `CCharacter::addPet` on a placed master: its pets
    // (`+0x648`) are the characters with that master, oldest first.
    const std::string unit = machine.unit_of(node);
    const int most = stats_for_unit(unit).maximum_pet_instances;
    if (0 < most) {
        std::vector<std::pair<tl::core::LevelScript*, int64_t>> same;
        for (const MachineView& view : machines()) {
            for (int64_t id : view.script->pets_of(&master_machine, master_node)) {
                const std::string name = view.script->unit_of(id);
                if (name != unit) continue;
                if (stats_for_unit(name).invincible_flag) continue;
                same.emplace_back(view.script, id);
            }
        }
        const int count = static_cast<int>(same.size());
        if (most <= count) {
            int left = count + 1 - most;
            for (const auto& pet : same) {
                TL_LOG_INFO("skills: %s's %s is past MAXIMUM_PET_INSTANCES %d and dies",
                            master_machine.unit_of(master_node).c_str(), unit.c_str(), most);
                kill_unit(pet.second, unit, /*fall=*/true, tl::core::LevelScript::kNoAttacker);
                if (--left == 0) break;
            }
        }
    }
    std::vector<tl::core::ScriptEvent> events;
    machine.set_unit_master(node, master_machine, master_node, quest_log.state(), events);
    if (NodeMap* nodes = nodes_of(&machine); nodes != nullptr && !events.empty()) {
        apply_script_events(events, *nodes);
    }
}

void World::add_player_pet(tl::core::LevelScript& machine, int64_t node) {
    // `hasPet`: already in the player's list.
    if (machine.unit_listed_as_player_pet(node)) return;
    const std::string unit = machine.unit_of(node);
    const int most = stats_for_unit(unit).maximum_pet_instances;
    if (0 < most) {
        // The player's pets of that name (`+0x40`), oldest first; `INVINCIBLE` ones not
        // counted.
        std::vector<std::pair<tl::core::LevelScript*, int64_t>> same;
        for (const MachineView& view : machines()) {
            for (int64_t id : view.script->player_pets()) {
                const std::string name = view.script->unit_of(id);
                if (name != unit) continue;
                if (stats_for_unit(name).invincible_flag) continue;
                same.emplace_back(view.script, id);
            }
        }
        const int count = static_cast<int>(same.size());
        if (most <= count) {
            // `die` (vtable +0x330) on as many as make room for this one.
            int left = count + 1 - most;
            for (const auto& pet : same) {
                TL_LOG_INFO("skills: %s is past MAXIMUM_PET_INSTANCES %d and dies", unit.c_str(),
                            most);
                kill_unit(pet.second, unit, /*fall=*/true, tl::core::LevelScript::kNoAttacker);
                if (--left == 0) break;
            }
        }
    }
    std::vector<tl::core::ScriptEvent> events;
    machine.make_player_pet(node, quest_log.state(), events);
    if (NodeMap* nodes = nodes_of(&machine); nodes != nullptr && !events.empty()) {
        apply_script_events(events, *nodes);
    }
}

const World::BreakableWorld* World::standing_breakable(int64_t id) const {
    if (id == 0) return nullptr;
    for (const BreakableWorld& b : breakables) {
        if (b.id == id) return b.node != nullptr && b.state.usable ? &b : nullptr;
    }
    return nullptr;
}

// `CMissile::doAOEDamage`'s `CItem::interact(nullptr)` per item reached.
// DIVERGENCE: queued until the missiles have stepped, so the walked list cannot grow.
bool World::queue_item_interact(int64_t item, bool by_player) {
    if (standing_breakable(item) == nullptr) return false;
    for (const PendingItemInteract& pending : pending_item_interacts) {
        if (pending.item == item) return false;
    }
    pending_item_interacts.push_back(PendingItemInteract{item, by_player});
    return true;
}

void World::step_breakable_shapes() {
    // `CLevel::updateItems`: active `ISA 0x1d` items go
    // to every update listener; here before the script steps, not after each `update`.
    if (!script_ready || breakables.empty() || !level_items.list) return;
    for (const tl::core::DamageShapeItem& item : level_items.list()) {
        script.item_updated_in_level(item);
    }
}

void World::flush_item_interacts() {
    if (pending_item_interacts.empty()) return;
    std::vector<PendingItemInteract> items;
    items.swap(pending_item_interacts);
    for (const PendingItemInteract& pending : items) {
        for (size_t i = 0; i < breakables.size(); ++i) {
            if (breakables[i].id != pending.item) continue;
            // `CBreakable::interact`: by nobody, or the
            // skill's owner.
            break_breakable(i, pending.by_player);
            break;
        }
    }
}

World::NodeMap* World::nodes_of(tl::core::LevelScript* machine) {
    // machines()'s order, without building its list.
    if (machine == &script) return &scripted_nodes;
    for (NestedScript& nested : nested_scripts) {
        if (!nested.retired && &nested.script == machine) return &nested.nodes;
    }
    return nullptr;
}

// PORT: a combat text (`make_combat_text`) added to the port's text events.
void World::add_combat_text(tl::core::CombatText what, const ogre::Vector3& unit_at, int amount) {
    add_text_event(tl::core::make_combat_text(what, unit_at.x, unit_at.y, unit_at.z, amount));
}

// `CCharacter::modifyDamage`: `Absorbed` and `!` over the
// character, then `CSoundBank::playSample(7)`; the combat-log line is unused.
void World::show_absorbed(const ogre::Vector3& at, const tl::core::UnitStats& stats) {
    add_combat_text(tl::core::CombatText::Absorbed, at);
    play_sound(stats.sounds[tl::core::SoundSlot::ShieldBlock], at);
}

// PORT: presentation of an absorbed hit at the unit (its text and sound).
void World::show_unit_absorbed(tl::core::LevelScript* machine, int64_t node) {
    if (machine == nullptr) return;
    tl::core::Vec3 at;
    if (!machine->unit_position(node, at)) return;
    show_absorbed(ogre::Vector3(at.x, at.y, at.z), stats_for_unit(machine->unit_of(node)));
}

bool World::unit_node_position(int64_t node, ogre::Vector3& at) {
    if (node == 0) {
        if (subject_node == nullptr) return false;
        at = view_position(subject_node);
        return true;
    }
    for (const auto& entry : scene_units) {
        if (entry.second.node != node || entry.second.entity == nullptr) continue;
        const Ogre::SceneNode* sn = view_parent_node(entry.second.entity);
        if (sn == nullptr) continue;
        at = view_derived_position(sn);
        return true;
    }
    for (const auto& owner : spawned) {
        for (const SpawnedUnit& made : owner.second) {
            if (made.entity == nullptr || made.id != node) continue;
            const Ogre::SceneNode* sn =
                made.node != nullptr ? made.node : view_parent_node(made.entity);
            if (sn == nullptr) continue;
            at = view_derived_position(sn);
            return true;
        }
    }
    return false;
}

// `CCharacter::playHitParticle`.
void World::play_hit_particle(const tl::core::UnitStats& victim, const ogre::Vector3& from,
                              const ogre::Vector3& at, CharacterRef who) {
    const tl::core::ParticlePoint p = tl::core::blow_particle_position(
        {from.x, from.y, from.z}, {at.x, at.y, at.z}, victim.radius);
    start_slot_particle(victim, tl::core::hit_particle_slot(game_settings().show_blood),
                        ogre::Vector3(p.x, p.y, p.z), who);
}

// `CCharacter::playStrikeParticle`.
void World::play_strike_particle(const tl::core::UnitStats& attacker, const ogre::Vector3& from,
                                 const ogre::Vector3& at, float radius, bool critical, bool blocked,
                                 CharacterRef who) {
    const tl::core::ParticlePoint p =
        tl::core::blow_particle_position({from.x, from.y, from.z}, {at.x, at.y, at.z}, radius);
    start_slot_particle(attacker, tl::core::strike_particle_slot(critical, blocked),
                        ogre::Vector3(p.x, p.y, p.z), who);
}

// `CCharacter::die`. INVENTED: the point
// (`+0x210`, from the object at `+0x200`, unread); the unit's position stands in.
void World::play_death_particle(const tl::core::UnitStats& unit, const ogre::Vector3& at,
                                CharacterRef who) {
    start_slot_particle(unit, tl::core::death_particle_slot(game_settings().show_blood), at, who);
}

void World::start_slot_particle(const tl::core::UnitStats& unit, int slot, const ogre::Vector3& at,
                                CharacterRef who) {
    if (slot < 0 || tl::core::character_particle::kCount <= slot) return;
    const std::string name =
        tl::core::character_particle_names(unit.particles)[static_cast<size_t>(slot)];
    const std::string file = name.empty() ? std::string() : preloaded_particle_file(name);
    if (!file.empty()) play_particle_sounds(file, at);
    start_character_particle(unit, slot, at, who);
}

void World::play_particle_sounds(const std::string& file, const ogre::Vector3& at) {
    const ParticleFileExtras* extras = particle_extras(file);
    if (extras == nullptr) return;
    // Each sound in its own slot of the bank, at its `VOLUME` (the definition's when 0):
    // the chance, the instance among the recordings kept, and for a play heard at the
    // listener the variation (`CSoundBank::playSample`). The bank's radius
    // is 10 where the definition gives none.
    tl::core::RunicRandom& rolls = tl::core::volatile_random();
    for (const ParticleFileExtras::Sound& sound : extras->sounds) {
        const SoundDefinition* named = sound_by_guid(sound.guid);
        if (named == nullptr || named->files.empty()) continue;
        if (named->play_chance >= 0 && named->play_chance <= rolls.integer_between(1, 100))
            continue;
        const std::vector<size_t> kept = tl::core::bank_recordings(*named);
        if (kept.empty()) continue;
        const std::string& recording = named->files[kept[static_cast<size_t>(
            rolls.integer_between(0, static_cast<int>(kept.size()) - 1))]];
        const float volume = sound.volume != 0.0f ? sound.volume : named->volume;
        const float radius = named->radius != 0.0f ? named->radius : 10.0f;
        const float heard =
            volume * tl::core::configure_volume_scale(radius, sound_listener, {at.x, at.y, at.z});
        const float variation = 0.0f < heard ? rolls.between(0.0f, named->volume_variation) : 0.0f;
        play_recording(named->name, recording, variation, at);
    }
}

void World::particle_link_started(const tl::core::ScriptEvent& event) {
    const auto found = particle_link_files.find(event.node_id);
    if (found == particle_link_files.end()) return;
    play_particle_sounds(found->second,
                         ogre::Vector3(event.position.x, event.position.y, event.position.z));
}

// PORT: a level character's key for its particle slots.
World::CharacterRef World::character_ref(int64_t node, tl::core::LevelScript* machine) {
    if (node == 0) return CharacterRef{};
    return CharacterRef{machine != nullptr ? machine : script_owning(node), node};
}

// `CCharacter::rollAttack` for the player's blow, what it
// shows: "Blocked!", `applyDamage`'s hit particle and "Glancing Blow!"
// (not for 0 damage), and the strike particle.
// INVENTED: the `+N`/`-N` lines are not drawn; flag 0x800 is not
// handed over.
void World::show_player_blow(int64_t target, const std::string& unit, float damage) {
    ogre::Vector3 at = ogre::Vector3::ZERO;
    if (subject_node == nullptr || !unit_node_position(target, at)) return;
    const ogre::Vector3 from = view_position(subject_node);
    const tl::core::UnitStats victim = stats_for_unit(unit);
    if (last_blow_blocked) add_combat_text(tl::core::CombatText::Blocked, at);
    if (damage != 0.0f) {
        play_hit_particle(victim, from, at, character_ref(target));
        // The victim is the player's own when its master is one (`+0x640`).
        if (last_blow_glancing) add_combat_text(tl::core::CombatText::GlancingBlow, at);
    }
    play_strike_particle(player_stats, from, at, victim.radius, last_blow_critical,
                         last_blow_blocked, CharacterRef{});
}

// `CCharacter::awardFame`, the text.
bool World::award_player_fame(int amount) {
    const bool fame_up = player_character.award_fame(amount, stat_graphs, player_total());
    const int awarded = player_character.last_fame_award();
    if (awarded != 0 && subject_node != nullptr) {
        add_combat_text(tl::core::CombatText::Fame, view_position(subject_node), awarded);
    }
    return fame_up;
}

// `CCharacter::applyInstantEffect`, case
// 0x34, with the player as the character struck.
void World::player_instant_damage(const tl::core::Effect& effect, const float* rolled) {
    if (player_health.dead) return;
    const float value =
        rolled != nullptr
            ? *rolled
            : tl::core::effect_base_value(effect, tl::core::EffectCalculate::Roll, stat_graphs,
                                          skill_effects.owner, &tl::core::volatile_random());
    const float v = 0.0f < value ? -value : value;
    if (!(v < 0.0f)) return;
    // The effect's owner: the unit whose skill event this is, or the player.
    const bool by_unit = instant_caster_machine != nullptr;
    const tl::core::CharacterEffects attacker =
        by_unit
            ? tl::core::CharacterEffects(instant_caster_machine->unit_effects(instant_caster_node))
            : player_total();
    const int type = static_cast<int>(effect.damage_type);
    const tl::core::InstantDamage hit =
        tl::core::instant_damage(value, type, effect.values[2], effect.values[3], player_defender(),
                                 &attacker, tl::core::volatile_random());
    if (!hit.applied) return;
    const std::string from = by_unit ? instant_caster_machine->unit_of(instant_caster_node)
                                     : std::string("its own skill");
    if (hit.blocked) {
        // "Blocked!" over the character.
        if (subject_node != nullptr) {
            add_combat_text(tl::core::CombatText::Blocked, view_position(subject_node));
        }
        TL_LOG_INFO("combat: player blocked '%s' from %s", effect.name.c_str(), from.c_str());
        return;
    }
    // A critical's interrupt. INVENTED: the knock-back is not applied.
    if (hit.interrupts) interrupt_player(true);
    hurt_player(static_cast<float>(hit.damage), from,
                by_unit ? instant_caster_node : tl::core::LevelScript::kNoAttacker,
                /*struck=*/true);
    if (!player_health.dead) {
        std::array<int, tl::core::kDamageTypeCount> by_type{};
        if (0 <= type && type < tl::core::kDamageTypeCount) {
            by_type[static_cast<size_t>(type)] = hit.damage;
        }
        // `applyDamageEffects(this, this, ...)`: the character is its own attacker.
        apply_damage_status(nullptr, 0, static_cast<float>(hit.damage), by_type, 1, type, false,
                            player_character.level(), tl::core::EffectSource{true, nullptr, 0});
    }
}

// `CCharacter::displayDamageAbsorbed(float)` on the
// player: red, one above the character.
void World::display_player_damage(float delta) {
    int amount = 0;
    if (!tl::core::display_damage(player_damage_shown, player_stats.display_dmg, delta, amount)) {
        return;
    }
    if (subject_node == nullptr) return;
    const ogre::Vector3 over = view_position(subject_node);
    add_text_event(tl::core::make_damage_text(over.x, over.y, over.z, amount, true));
}

void World::step_player_skill_turn(float dt) {
    // taken holding a skill or with the hold position flag (`+0x1fe`).
    if (player_active_skill.empty() && !attack_button.hold) return;
    const std::string& name = player_active_skill.skill;
    const tl::core::KnownSkill* skill = player_skills.find_by_name(name);
    const tl::core::SkillRunState* state = player_skill_runner.state(name);
    const tl::core::SkillProperty* property =
        skill != nullptr ? skill->property(state != nullptr ? state->level : skill->effective)
                         : nullptr;
    // The target (`+0x284`), else the target item (`+0x28c`).
    std::optional<tl::core::Vec3> target;
    if (player_target_node != 0) {
        for (tl::core::LevelScript* machine : level_machines) {
            tl::core::Vec3 at;
            if (!machine->unit_position(player_target_node, at)) continue;
            target = at;
            break;
        }
    } else if (player_skill_item != 0) {
        const int index = breakable_index(player_skill_item);
        if (index >= 0) {
            const ogre::Vector3 p = view_position(breakables[static_cast<size_t>(index)].node);
            target = tl::core::Vec3{p.x, p.y, p.z};
        }
    }
    const bool has_def = skill != nullptr && skill->def != nullptr;
    const tl::core::PlayerSkillTurn turn = tl::core::player_skill_turn(
        player_active_skill, state != nullptr ? state->animations : tl::core::SkillAnimations{},
        player_skill_runner.is_running(name), !has_def || skill->def->allows_turning,
        has_def && skill->def->moves_to_target,
        property != nullptr ? property->find_target_angle : 0.0f, target.has_value(),
        attack_button.hold);
    switch (turn) {
        case tl::core::PlayerSkillTurn::None: break;
        case tl::core::PlayerSkillTurn::Aim: {
            const tl::core::Vec3 me = player.position();
            turn_player_toward({me.x + player_turn_direction.x, me.y + player_turn_direction.y,
                                me.z + player_turn_direction.z},
                               dt);
            break;
        }
        case tl::core::PlayerSkillTurn::Target:
            // `dt * 30`. Only ever chosen with a target.
            if (target.has_value()) turn_player_toward(*target, dt * 30.0f);
            break;
        case tl::core::PlayerSkillTurn::Point: turn_player_toward(player_turn_point, dt); break;
    }
}

bool World::place_skill_event_layout(const std::string& reference, const ogre::Vector3& origin,
                                     const ogre::Quaternion& basis) {
    const bool was_event = std::exchange(placing_skill_event_layout, true);
    const bool placed = place_layout(reference, origin, basis, /*depth=*/1);
    placing_skill_event_layout = was_event;
    return placed;
}

// an `EVENT_START` (group 0) whose property has a `SLOWDOWNMS` slows the game,
// `CGameSpeed::addSpeedModifier(seconds, 10.0f)`, type 0.
void World::start_event_slowdown(const tl::core::SkillProperty* slowing) {
    if (slowing == nullptr || !(slowing->slowdown > 0.0f)) return;
    load_game_speed();
    game_speed.add_modifier(static_cast<int32_t>(tl::core::GameSpeedType::Decelerate),
                            slowing->slowdown, 10.0f);
}

// The event's fields from its skill and property, as `CSkillEvent::startEvent`
// reads them; a `DURATIONOVERRIDEMS` hold is queued.
World::SkillEventSource World::skill_event_source_of(const tl::core::KnownSkill& skill,
                                                     uint32_t level, int group,
                                                     const tl::formats::AdmSection& event,
                                                     const tl::core::SkillEventTarget& target,
                                                     const tl::core::SkillEventSpec& spec) {
    SkillEventSource source;
    source.skill = skill.entry.name;
    source.level = level;
    source.event = &event;
    const tl::core::SkillProperty* property = skill.property(level);
    // The event's property (`CSkillEvent +0x4c`); an inherited event's is the parent's.
    //
    source.property =
        property != nullptr && property->section != nullptr ? &property->event_section() : nullptr;
    source.duration_override = property != nullptr ? property->events_duration_override : 0.0f;
    source.target_type = property != nullptr ? property->target_type : skill.def->base.target_type;
    source.target_unit_type =
        property != nullptr ? property->target_unit_type : skill.def->base.target_unit_type;
    source.single_target =
        property != nullptr ? property->single_target : skill.def->base.single_target;
    // The blocks were made with the skill (`kSkillBlockTargetDefault`).
    source.unauthored = tl::core::kSkillBlockTargetDefault;
    source.target_alignment = skill.def->target_alignment;
    source.spec = spec;
    source.group = group;
    source.caster_machine = unit_event_caster.machine;
    source.caster_node = unit_event_caster.node;
    source.run = target.run;
    // `startEvent`: `+0x15c = +0x158`, the block's
    // `DURATIONOVERRIDEMS` in seconds.
    const int hold_ms = static_cast<int>(event.integer_or("DURATIONOVERRIDEMS", 0));
    if (0 < hold_ms) {
        SkillEventHold hold;
        hold.skill = source.skill;
        hold.run = source.run;
        hold.caster_machine = source.caster_machine;
        hold.caster_node = source.caster_node;
        hold.left = static_cast<float>(hold_ms) * 0.001f;  // `fmuls` by 0.001f
        skill_event_holds.push_back(hold);
    }
    return source;
}

// `startEvent`'s first `applyEffects`: when the caster handed in is a character
// (`+0x34`, whatever the target), on the skill's master owner, ending in its `updateEffects(0)`
// - a status affix due lands now, before the layout. INVENTED: the owner's own
// `applyAffixesAndEffects` half is not carried, nor a player owner's update.
void World::caster_updates_at_event_start() {
    if (unit_event_caster.machine == nullptr) return;
    std::vector<tl::core::ScriptEvent> updated;
    unit_update_effects(unit_event_caster.machine, unit_event_caster.node, 0.0f, updated);
    if (NodeMap* nodes = nodes_of(unit_event_caster.machine);
        nodes != nullptr && !updated.empty()) {
        apply_script_events(updated, *nodes);
    }
}

void World::start_skill_event(const tl::core::KnownSkill& skill, uint32_t level, int group,
                              const tl::formats::AdmSection& event,
                              const tl::core::SkillEventTarget& target) {
    const tl::core::SkillEventSpec spec = tl::core::read_skill_event(event);
    // The event's caster: the player, an `EXECUTE_SKILL`'s (`event_caster_override`), or a
    // placed unit casting its own skill (`unit_event_caster`).
    const bool unit_cast = unit_event_caster.machine != nullptr;
    Ogre::SceneNode* const caster_node = unit_cast ? unit_event_caster.scene
                                         : event_caster_override != nullptr ? event_caster_override
                                                                            : subject_node;
    if (caster_node == nullptr || skill.def == nullptr) return;
    // The caster's body radius (`+0x194`).
    const float caster_body = unit_cast                    ? unit_event_caster.radius
                              : player_stats.radius > 0.0f ? player_stats.radius
                                                           : tl::core::kDefaultCollisionRadius;
    const float player_body =
        player_stats.radius > 0.0f ? player_stats.radius : tl::core::kDefaultCollisionRadius;
    std::string reference = spec.file;
    std::replace(reference.begin(), reference.end(), '\\', '/');

    SkillEventSource source = skill_event_source_of(skill, level, group, event, target, spec);

    // `CSkillEvent::startEvent`, where the layout goes:
    // no target unit for target type 11 (`TARGET_POS`); the origin is `triggerEvent`'s.
    const ogre::Vector3 caster_feet = view_position(caster_node);
    ogre::Vector3 at = caster_feet;
    if (target.origin.has_value())
        at = ogre::Vector3(target.origin->x, target.origin->y, target.origin->z);
    ogre::Quaternion facing = view_orientation(caster_node);
    if (target.yaw.has_value())
        facing = ogre::Quaternion(ogre::Radian(*target.yaw), ogre::Vector3::UNIT_Y);
    const bool has_unit = target.has_unit && source.target_type != 0xb;
    source.has_original_target = has_unit;
    source.original_machine = has_unit ? target.machine : nullptr;
    source.original_node = has_unit ? target.unit : 0;
    caster_updates_at_event_start();
    // A target item (a breakable) takes the unit's place: radius 0, centre at its node.
    const int item_index = !has_unit && target.item != 0 && source.target_type != 0xb
                               ? breakable_index(target.item)
                               : -1;
    Ogre::SceneNode* item_node =
        item_index >= 0 ? breakables[static_cast<size_t>(item_index)].node : nullptr;
    // `startEvent`'s `+0x160` and `+0x1fc`: the skill's range along the forward; unless
    // `PLACEONTARGET`, to the target (less both radii) or the point; never past the range.
    const tl::core::SkillProperty* ranged = skill.property(level);
    const float skill_range = ranged != nullptr ? ranged->range : 1.0f;
    const ogre::Vector3 caster_forward = view_orientation(caster_node) * ogre::Vector3::UNIT_Z;
    float to_target_distance = skill_range;
    tl::core::Vec3 to_target{caster_forward.x * skill_range, 0.0f, caster_forward.z * skill_range};
    if (!spec.place_on_target) {
        const float caster_radius = caster_body;
        const auto* aimed = static_cast<const tl::core::LevelScript*>(target.machine);
        tl::core::Vec3 feet;
        if (has_unit && aimed == nullptr && unit_cast && subject_node != nullptr) {
            // The player, as a unit's target.
            const ogre::Vector3 you = view_position(subject_node);
            const float dx = you.x - at.x;
            const float dz = you.z - at.z;
            to_target_distance =
                std::max(0.0f, std::sqrt(dx * dx + 0.0f + dz * dz) - (player_body + caster_radius));
            to_target = {dx, 0.0f, dz};
        } else if (has_unit && aimed != nullptr && aimed->unit_position(target.unit, feet)) {
            const float dx = feet.x - at.x;
            const float dz = feet.z - at.z;
            to_target_distance =
                std::max(0.0f, std::sqrt(dx * dx + 0.0f + dz * dz) -
                                   (aimed->unit_radius(target.unit) + caster_radius));
            to_target = {dx, 0.0f, dz};
        } else if (item_node != nullptr) {
            const ogre::Vector3 there = view_position(item_node);
            const float dx = there.x - at.x;
            const float dz = there.z - at.z;
            to_target_distance =
                std::max(0.0f, std::sqrt(dx * dx + 0.0f + dz * dz) - caster_radius);
            to_target = {dx, 0.0f, dz};
        } else if (target.position.has_value()) {
            const float dx = target.position->x - at.x;
            const float dz = target.position->z - at.z;
            to_target_distance =
                std::max(0.0f, std::sqrt(dx * dx + 0.0f + dz * dz) - caster_radius);
            to_target = {dx, 0.0f, dz};
        }
    }
    to_target_distance = std::min(to_target_distance, skill_range);
    if (spec.place_on_target) {
        // `PLACEONTARGET`: the unit's feet, else the point, else the origin.
        bool placed_on_unit = false;
        if (has_unit) {
            if (target.machine == nullptr) {
                // A null machine is the player (the caster, for its own skills).
                at = subject_node != nullptr ? view_position(subject_node) : caster_feet;
                placed_on_unit = true;
            } else {
                const auto* machine = static_cast<const tl::core::LevelScript*>(target.machine);
                tl::core::Vec3 feet;
                if (machine->unit_position(target.unit, feet)) {
                    at = ogre::Vector3(feet.x, feet.y, feet.z);
                    placed_on_unit = true;
                }
            }
        } else if (item_node != nullptr) {
            at = view_position(item_node);
            placed_on_unit = true;
        }
        if (!placed_on_unit && target.position.has_value()) {
            at = ogre::Vector3(target.position->x, target.position->y, target.position->z);
        }
    }
    // A position skill's layout faces away from the caster once more than 0.5 off it.
    const int32_t skill_type = skill.def->base.target_type;
    if (skill_type == 1 || skill_type == 0xb || skill_type == 6 || skill_type == 5) {
        float dx = at.x - caster_feet.x;
        float dz = at.z - caster_feet.z;
        const float away = std::sqrt(dx * dx + 0.0f + dz * dz);
        if (0.5f < away) {
            dx /= away;
            dz /= away;
            ogre::Quaternion q;
            q.FromAxes(ogre::Vector3(dz, 0.0f, -dx), ogre::Vector3::UNIT_Y,
                       ogre::Vector3(dx, 0.0f, dz));
            facing = q;
        }
    }

    // `ATTACHTOTARGET` (`updateEvent`, +0x13e): with a target and
    // no `ATTACH_LINE_EMITTER_TO_MAX_DISTANCE`, on the target's `FIREATBONE`; otherwise
    // the range along the forward, cut 0.1 before a `LINE_EMITTER_COLLIDES` hit.
    // INVENTED: bones are body centres, no re-aim each frame, no lead (`+0x164`).
    Ogre::SceneNode* ride = spec.attaches ? caster_node : nullptr;
    if (spec.attach_to_target) {
        auto* followed = static_cast<tl::core::LevelScript*>(const_cast<void*>(target.machine));
        bool on_unit = false;
        if (has_unit && !spec.attach_to_max_distance) {
            if (followed == nullptr) {
                const float body = player_stats.radius > 0.0f ? player_stats.radius
                                                              : tl::core::kDefaultCollisionRadius;
                at = view_position(subject_node) + ogre::Vector3(0.0f, body, 0.0f);
                ride = subject_node;
                on_unit = true;
            } else if (tl::core::Vec3 feet; followed->unit_position(target.unit, feet)) {
                at = ogre::Vector3(feet.x, feet.y + followed->unit_radius(target.unit), feet.z);
                ride = nullptr;
                if (NodeMap* nodes = nodes_of(followed)) {
                    const auto found = nodes->find(target.unit);
                    if (found != nodes->end()) ride = found->second;
                }
                on_unit = true;
            }
        } else if (item_node != nullptr && !spec.attach_to_max_distance) {
            at = view_position(item_node);
            ride = item_node;
            on_unit = true;
        }
        if (!on_unit) {
            const ogre::Vector3 from =
                view_position(caster_node) + ogre::Vector3(0.0f, caster_body, 0.0f);
            const ogre::Vector3 forward = view_orientation(caster_node) * ogre::Vector3::UNIT_Z;
            const tl::core::SkillProperty* range_of = skill.property(level);
            ogre::Vector3 to = from + forward * (range_of != nullptr ? range_of->range : 1.0f);
            tl::core::CollisionHit hit;
            if (spec.line_emitter_collides &&
                collision.ray_collision({from.x, from.y, from.z}, {to.x, to.y, to.z}, true, hit)) {
                to = ogre::Vector3(hit.point.x, hit.point.y, hit.point.z) -
                     ogre::Vector3(hit.normal.x, hit.normal.y, hit.normal.z) * 0.1f;
            }
            at = to;
            ride = nullptr;
        }
    }

    // The layout, when the block names one (`FILE`); an event without one
    // still strikes its target and runs its skills.
    if (!reference.empty())
        [&] {
            // The spawners the layout carries, for the `Spawn Units` its timeline sends.
            if (const tl::formats::CmpScene* layout = cached_layout(reference)) {
                for (const tl::formats::CmpNode& node : layout->nodes) {
                    if (node.type != tl::formats::cmp_type::kUnitSpawner) continue;
                    skill_spawners[node.id] =
                        SkillSpawner{source, tl::core::read_spawner_shape(node)};
                }
            }

            const size_t before = nested_scripts.size();
            const size_t systems_before = view_particle_mark();
            if (!place_skill_event_layout(reference, at, facing)) {
                TL_LOG_WARN("skills: '%s' could not place '%s'", source.skill.c_str(),
                            reference.c_str());
                return;
            }
            // `ATTACHES` rides the caster, `ATTACHTOTARGET` the target; a machine it owns
            // keeps its placed point.
            if (ride != nullptr) view_ride_particles(systems_before, ride);
            // The event's listeners (`addListeners`) before the layout runs.
            hang_player_skill_event(source, before);
            LiveSkillEvent live;
            live.skill = source.skill;
            live.run = source.run;
            live.first = before;
            live.end = nested_scripts.size();
            live.caster_machine = source.caster_machine;
            live.caster_node = source.caster_node;
            {
                // `CSkill +0xec`, the run's loop animation, off the caster's runner.
                const tl::core::SkillRunner* runner = &player_skill_runner;
                if (source.caster_machine != nullptr) {
                    const tl::core::LevelScript::UnitCasting* k =
                        source.caster_machine->unit_casting(source.caster_node);
                    runner = k != nullptr ? &k->runner : nullptr;
                }
                const tl::core::SkillRunState* run =
                    runner != nullptr ? runner->state(skill.entry.name) : nullptr;
                live.loop = run != nullptr && run->animations.loop_into != -1;
            }
            // Not what a script of the layout placed meanwhile; `startEvent`'s
            // `CLineEmitterWrapper`s, each system of the layout holding a `Line` emitter.
            view_claim_particles(systems_before, live.holders, live.line_holders);
            // The owner whose bones `updateEvent` reads (`getOwnerCharacter`) and the target.
            if (unit_cast) {
                live.owner = ViewUnit{false, unit_event_caster.machine, unit_event_caster.node};
            } else if (event_caster_override != nullptr &&
                       event_caster_override_machine != nullptr) {
                live.owner =
                    ViewUnit{false, event_caster_override_machine, event_caster_override_node};
            } else {
                live.owner = ViewUnit{true, nullptr, 0};
            }
            if (has_unit) {
                live.has_target = true;
                live.target =
                    ViewUnit{target.machine == nullptr,
                             static_cast<tl::core::LevelScript*>(const_cast<void*>(target.machine)),
                             target.unit};
            } else if (item_node != nullptr) {
                live.has_target = true;
                live.target = ViewUnit{};
                live.target.item = target.item;
            }
            live.attach_line_emitter = spec.attach_line_emitter;
            live.to_max_distance = spec.attach_to_max_distance;
            live.collides = spec.line_emitter_collides;
            live.far_target = spec.attach_to_target &&
                              ((!has_unit && item_node == nullptr) || spec.attach_to_max_distance);
            live.far_point = at;
            live.fire_from = spec.fire_from_bone;
            live.fire_at = spec.fire_at_bone;
            live.range = skill_range;
            live.target_ahead = spec.target_ahead;
            if (spec.attach_owner) {
                for (size_t i = before; i < nested_scripts.size() && live.shape == 0; ++i) {
                    const std::vector<int64_t> shapes = nested_scripts[i].script.damage_shape_ids();
                    if (shapes.empty()) continue;
                    live.shape_machine = &nested_scripts[i].script;
                    live.shape = shapes.front();
                }
                live.attach_owner = live.shape != 0;
                live.ignores_units = spec.attach_ignores_units;
                live.ignores_obstructions = spec.attach_ignores_obstructions;
                live.scale_distance = spec.scale_attach_distance;
                live.at = at;
                live.facing = facing;
                live.to_target = to_target;
                live.distance = to_target_distance;
                live.skill_target_type = skill.def->base.target_type;
            }
            live_skill_events.push_back(live);
            // `startEvent` ends in `updateEvent`.
            update_skill_event_lines(live_skill_events.back());
            // Casting is the `Play` its timelines wait for (`startEvent` state 0).
            for (size_t i = before; i < nested_scripts.size(); ++i) {
                NestedScript& machine = nested_scripts[i];
                std::vector<tl::core::ScriptEvent> events;
                machine.running = true;
                machine.script.play_layout_timelines(quest_log.state(), events,
                                                     /*backwards=*/false, /*restart=*/true);
                if (!events.empty()) apply_script_events(events, machine.nodes);
            }
            TL_LOG_INFO("skills: '%s' places %s at (%.2f, %.2f, %.2f)%s", source.skill.c_str(),
                        reference.c_str(), static_cast<double>(at.x), static_cast<double>(at.y),
                        static_cast<double>(at.z), spec.attaches ? " (attached)" : "");
        }();

    // `APPLYEFFECTS` (+0x138) with a target: `applyEffects`, `applyWeaponDamage`,
    // `invokeHitSkills`; without, a unit group's event (4, 5, 6, 8) still lays its effects.
    // A breakable (`ISA(0x1d)`) is broken when `effectsTargetEnemy`
    // answers (vtable +0x280). INVENTED: target types 7 and 8 (`ITEM`, `UNIDENTIFIED_ITEM`),
    // `applyEffects` on a target that is neither, are not carried.
    auto* unit_machine = static_cast<tl::core::LevelScript*>(const_cast<void*>(target.machine));
    const bool unit_is_character =
        has_unit && (unit_machine == nullptr || !unit_machine->unit_of(target.unit).empty());
    bool struck = false;
    if (spec.apply_effects && unit_is_character) {
        skill_event_strike(source, unit_machine, target.unit);
        struck = true;
    }
    if (!struck && unit_is_character && (group == 4 || group == 5 || group == 6 || group == 8)) {
        skill_event_effects_on(source, unit_machine, target.unit);
    }
    if (spec.apply_effects && item_node != nullptr) {
        tl::core::SkillCastContext asked;
        asked.types = &effect_types;
        asked.unit_types = &unit_types;
        if (tl::core::event_effects_target_enemy(event, asked, source.property,
                                                 source.unauthored)) {
            // Queued for the frame's item pass (the break may place layouts); a unit's
            // skill breaks it with no breaker.
            const bool by_player = source.caster_machine == nullptr;
            if (queue_item_interact(target.item, by_player)) {
                TL_LOG_INFO("skills: '%s' breaks %s", source.skill.c_str(),
                            breakables[static_cast<size_t>(item_index)].unit.c_str());
            }
        }
    }
    if (group == 0) start_event_slowdown(skill.property(level));

    // The executed skills' point: the point given, else the target's centre, else origin.
    tl::core::Vec3 point{at.x, at.y, at.z};
    ViewUnit event_unit;
    if (has_unit) {
        event_unit.player = unit_machine == nullptr;
        event_unit.machine = unit_machine;
        event_unit.node = target.unit;
    }
    if (target.position.has_value()) {
        point = *target.position;
    } else if (has_unit && unit_machine != nullptr) {
        tl::core::Vec3 feet;
        if (unit_machine->unit_position(target.unit, feet)) {
            point = {feet.x, feet.y + unit_machine->unit_radius(target.unit), feet.z};
        }
    } else if (has_unit) {
        const float body =
            player_stats.radius > 0.0f ? player_stats.radius : tl::core::kDefaultCollisionRadius;
        point = {player.position().x, player.position().y + body, player.position().z};
    } else if (item_node != nullptr) {
        const ogre::Vector3 there = view_position(item_node);
        point = {there.x, there.y, there.z};
    }
    execute_event_skills(source, spec, has_unit ? &event_unit : nullptr, {at.x, at.y, at.z}, facing,
                         point);
}

namespace {

// The dead only for a corpse shape or `TARGET_CORPSES`; else the living. Alive is
// `(int)CCharacter::HP() > 0`, `HP` the floor: so 0 < HP < 1 is a corpse.
bool life_as_asked(bool corpses, float hp) {
    const bool alive = std::floor(hp) >= 1.0f;
    return corpses ? !alive : alive;
}

}  // namespace

bool World::skill_event_who(const tl::core::DamageShapeUnit& who, SkillEventWho& out) {
    if (who.machine == nullptr) {
        const float body =
            player_stats.radius > 0.0f ? player_stats.radius : tl::core::kDefaultCollisionRadius;
        out.centre_y = player.position().y + body;
        out.hp = player_health.dead ? 0.0f : player_health.current;
        out.side = tl::core::UnitStats::Alignment::Friendly;
        return true;
    }
    const auto* machine = static_cast<const tl::core::LevelScript*>(who.machine);
    tl::core::Vec3 centre;
    float max = 0.0f;
    if (!machine->unit_centre(who.node, centre) || !machine->unit_health(who.node, out.hp, max)) {
        return false;
    }
    out.centre_y = centre.y;
    out.unit = machine->unit_of(who.node);
    out.side = machine->unit_side(who.node).value_or(tl::core::UnitStats::Alignment::Neutral);
    out.pet = machine->unit_is_player_pet(who.node);
    return true;
}

std::vector<std::string> World::skill_event_specific_units(const tl::formats::AdmSection* event) {
    // `TARGET_SPECIFIC_UNITS` (+0xd8): `UNIT_GUID`s in the original, unit names here.
    std::vector<std::string> specific;
    if (event == nullptr) return specific;
    for (const tl::formats::AdmSection& part : event->children) {
        if (tl::core::upper_ascii(part.name) != "TARGET_SPECIFIC_UNITS") continue;
        for (const tl::formats::AdmRecord& record : part.records) {
            if (tl::core::upper_ascii(record.key) == "UNIT") {
                specific.push_back(tl::core::upper_ascii(record.text));
            }
        }
    }
    return specific;
}

bool World::skill_event_can_affect(const SkillEventSource& src,
                                   const std::vector<std::string>& specific,
                                   const tl::core::DamageShapeUnit& who, bool corpses,
                                   bool by_shape, const float* shape_y, bool turned_back) {
    // `CSkillEvent::getCharacterCanBeAffectedByDamageShape`,
    // with the player as the skill's master owner.
    SkillEventWho w;
    if (!skill_event_who(who, w)) return false;
    if (!life_as_asked(corpses || src.spec.target_corpses, w.hp)) return false;
    // `SINGLETARGET`: only the event's original target, or any for a
    // turned-back missile (`CMissile +0x28c`).
    const bool original = src.has_original_target && who.machine == src.original_machine &&
                          (who.machine == nullptr || who.node == src.original_node);
    if (src.single_target && !original && !turned_back) return false;
    // An owner that is not a character (the skill manager's unit, `+0x150` clear) passes
    // every living character here.
    if (src.prop_owner) return true;
    // `canEffectPosObject`, for a shape only.
    if (by_shape && (shape_y == nullptr || std::fabs(*shape_y - w.centre_y) > 3.0f)) {
        return false;
    }
    // `TARGET_UNITTYPE`: an id of 0 passes everyone.
    const uint32_t want = src.target_unit_type.empty() ? 0 : unit_types.id_of(src.target_unit_type);
    if (want != 0) {
        // INVENTED: the player's own type is its class; `Player`, which every class ISAs.
        const bool isa = who.machine == nullptr
                             ? unit_types.isa(tl::core::unit_type::kPlayer, want)
                             : static_cast<tl::core::LevelScript*>(const_cast<void*>(who.machine))
                                   ->character_isa(who.node, want);
        if (!isa) return false;
    }
    if (!specific.empty() && std::find(specific.begin(), specific.end(),
                                       tl::core::upper_ascii(w.unit)) == specific.end()) {
        return false;
    }
    if (src.target_type == 4) return true;  // `EVERYBODY`
    // `SELFANDPETS` (10): the owner, a pet of it (`+0x5a4` the owner) or its master; `PETS`
    // (9): a pet of it.
    const bool by_unit = src.caster_machine != nullptr;
    auto* const who_machine = static_cast<tl::core::LevelScript*>(const_cast<void*>(who.machine));
    bool self = who.machine == nullptr;
    bool pet = w.pet;
    bool owners_master = false;  // the player has no master
    if (by_unit) {
        tl::core::LevelScript* m = nullptr;
        int64_t n = 0;
        self = who_machine == src.caster_machine && who.node == src.caster_node;
        pet = who_machine != nullptr && who_machine->unit_master(who.node, m, n) &&
              m == src.caster_machine && n == src.caster_node;
        owners_master = who_machine == nullptr
                            ? src.caster_machine->unit_is_player_pet(src.caster_node)
                            : src.caster_machine->unit_master(src.caster_node, m, n) &&
                                  m == who_machine && n == who.node;
    }
    if (src.target_type == 10 && !self && !pet && !owners_master) return false;
    if (src.target_type == 9 && !pet) return false;
    // `CCharacter::isEnemy` from the owner's side; then a
    // berserk owner or one of side 4 passes everyone.
    // INVENTED: the player's AI flag 1 is never set, its side never 4.
    const bool enemy =
        by_unit ? src.caster_machine->unit_is_enemy_of(
                      src.caster_node,
                      static_cast<tl::core::LevelScript*>(const_cast<void*>(who.machine)), who.node)
                : w.side == tl::core::UnitStats::Alignment::Hostile;
    if (by_unit && src.caster_machine->unit_affects_anyone(src.caster_node)) return true;
    if (src.target_alignment == 2 && enemy) return true;
    if (src.target_alignment == 1 && !enemy) return true;
    // A turned-back missile harms anyone left.
    return turned_back;
}

void World::hang_player_skill_event(const SkillEventSource& source, size_t first) {
    if (first >= nested_scripts.size()) return;
    // The shapes this event's layout placed, by machine and node.
    std::vector<std::pair<tl::core::LevelScript*, int64_t>> shapes;
    for (size_t i = first; i < nested_scripts.size(); ++i) {
        for (int64_t id : nested_scripts[i].script.damage_shape_ids()) {
            shapes.emplace_back(&nested_scripts[i].script, id);
        }
    }
    if (shapes.empty()) return;

    const std::vector<std::string> specific = skill_event_specific_units(source.event);

    // The shape's position for `canEffectPosObject`:
    // within 3.0 of the unit's centre in height.
    const auto shape_height = [shapes](const tl::core::DamageShape& shape, float& y) {
        for (const auto& entry : shapes) {
            if (entry.first->damage_shape(entry.second) != &shape) continue;
            tl::core::Vec3 p;
            if (!entry.first->node_position(entry.second, p)) return false;
            y = p.y;
            return true;
        }
        return false;
    };
    auto listener = std::make_unique<SkillEventListener>();
    const SkillEventSource src = source;
    listener->can = [this, src, specific, shape_height](const tl::core::DamageShape& shape,
                                                        const tl::core::DamageShapeUnit& who) {
        float shape_y = 0.0f;
        const bool known = shape_height(shape, shape_y);
        return skill_event_can_affect(src, specific, who, shape.target_corpse, true,
                                      known ? &shape_y : nullptr, false);
    };
    // `CSkillEvent::characterEffectedByDamageShape`.
    listener->act = [this, src, shape_height](const tl::core::DamageShape& shape,
                                              const tl::core::DamageShapeUnit& who) {
        SkillEventWho w;
        if (!skill_event_who(who, w)) return false;
        float shape_y = 0.0f;
        if (!shape_height(shape, shape_y) || !(std::fabs(shape_y - w.centre_y) <= 3.0f))
            return false;
        auto* machine = static_cast<tl::core::LevelScript*>(const_cast<void*>(who.machine));
        (void)shape;
        return skill_event_strike(src, machine, who.node);
    };
    // `CSkillEvent::itemEffectedByDamageShape`: within 3
    // of the item's height (`canEffectPosObject`), then `applyWeaponDamage(null,
    // item, 1, 1)`, true also when `isDamaging`.
    //
    listener->item = [this, src, shape_height](const tl::core::DamageShape& shape,
                                               const tl::core::DamageShapeItem& item) {
        float shape_y = 0.0f;
        if (!shape_height(shape, shape_y) || std::fabs(shape_y - item.at.y) > 3.0f) return false;
        bool acted = skill_event_item_blow(src, item);
        if (src.event != nullptr) {
            tl::core::SkillCastContext context;
            context.types = &effect_types;
            context.unit_types = &unit_types;
            context.affix = [this](const std::string& name) -> const tl::core::Affix* {
                return affixes.get_affix(name);
            };
            if (tl::core::event_is_damaging(*src.event, context, src.property, src.unauthored)) {
                acted = true;
            }
        }
        return acted;
    };
    for (const auto& entry : shapes) {
        entry.first->damage_shape_take_event(entry.second, source.spec.target_corpses,
                                             source.spec.max_units_hit);
        entry.first->damage_shape_add_listener(entry.second, listener.get());
    }
    skill_event_listeners.push_back(std::move(listener));
}

bool World::find_character_within_view(const tl::core::Vec3& at, const ogre::Quaternion& facing,
                                       float near, float angle, float range, int32_t alignment,
                                       bool nearest, const ViewUnit& ignore, ViewUnit& found,
                                       const std::function<bool(const ViewUnit&)>& valid) {
    // `CLevel::findCharacterWithinView`: the player, then
    // each machine's characters; the first in view, or the nearest. Passed: dead, hidden,
    // untargetable, ignored, wrong alignment (3 any, 1/6 `GOOD`, 2/5 `EVIL`), dying, AI
    // 0x29/0x2a, or refused by `CSkill::getTargetIsValid`. Farther than `near`: within
    // `angle` (nearer than 2.0: 30) and `range`, sight clear 5.0 up (`sightUnobstructed`).
    const ogre::Quaternion inverse = facing.Inverse();
    const auto passes_side = [alignment](tl::core::UnitStats::Alignment side) {
        using A = tl::core::UnitStats::Alignment;
        if (alignment == 1 || alignment == 6) return side == A::Friendly;
        if (alignment == 2 || alignment == 5) return side == A::Hostile;
        return true;
    };
    float best = 9999.0f;
    bool got = false;
    const auto consider = [&](const ViewUnit& who, const tl::core::Vec3& centre) {
        const ogre::Vector3 local =
            inverse * ogre::Vector3(centre.x - at.x, centre.y - at.y, centre.z - at.z);
        const float distance = std::sqrt(local.x * local.x + 0.0f + local.z * local.z);
        if (near < distance) {
            const float off = std::fabs(std::atan2(local.x, local.z) * 57.295826f);
            if (angle < off && (angle <= 0.0f || 2.0f <= distance || 30.0f <= off)) return false;
            if (range < distance) return false;
        }
        tl::core::CollisionHit hit;  // the objects tested too (`li r10, 0`)
        if (collision.ray_collision({at.x, at.y + 5.0f, at.z},
                                    {centre.x, centre.y + 5.0f, centre.z}, /*objects=*/true, hit)) {
            return false;
        }
        if (!nearest) {
            found = who;
            got = true;
            return true;
        }
        if (best <= distance) return false;
        found = who;
        best = distance;
        got = true;
        return false;
    };
    if (!player_health.dead && subject_node != nullptr && !ignore.player &&
        passes_side(tl::core::UnitStats::Alignment::Friendly)) {
        const float body =
            player_stats.radius > 0.0f ? player_stats.radius : tl::core::kDefaultCollisionRadius;
        const tl::core::Vec3 feet = player.position();
        ViewUnit you;
        you.player = true;
        if ((!valid || valid(you)) && consider(you, {feet.x, feet.y + body, feet.z})) return true;
    }
    for (tl::core::LevelScript* machine : level_machines) {
        std::vector<tl::core::LevelScript::Reachable> list;
        machine->reachable_characters(list);
        for (const tl::core::LevelScript::Reachable& r : list) {
            if (!ignore.player && ignore.machine == machine && ignore.node == r.id) continue;
            // `ISA(0xa7)` with the `MIMICIDLE` theme.
            if (r.mimic_idle) continue;
            if (!passes_side(r.alignment)) continue;
            ViewUnit who;
            who.machine = machine;
            who.node = r.id;
            if (valid && !valid(who)) continue;
            if (consider(who, r.center)) return true;
        }
    }
    return got;
}

void World::find_player_skill_target(const tl::core::KnownSkill& skill) {
    // `CCharacter::castSkill`: no target, no
    // target item, and a `FINDTARGETANGLE` above 0.
    // The cone is the pad's; under the mouse the pointer names the target.
    if (tl::core::mouse_controls() || player_target_node != 0 || player_skill_item != 0) return;
    if (subject_node == nullptr) return;
    const tl::core::SkillProperty* property = skill.property(skill.effective);
    const float angle = property != nullptr ? property->find_target_angle : 0.0f;
    if (!(0.0f < angle)) return;
    // The reach: `(scale * REACH_BONUS + 0.2 + RANGE) * RANGE_MULTIPLIER`, and 1
    // more for the characters.
    // A null property has angle 0 and has returned above.
    // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
    const float range = property->range;
    const float reach =
        (player_stats.scale * player_stats.reach + 0.2f + range) * player_stats.range_multiplier;
    // `getTargetAlignment`: no AI manager, so 5, `EVIL`'s
    // side.
    const int32_t alignment = 5;
    // The frame: the character's position, turned by its facing.
    const tl::core::Vec3 me = player.position();
    const ogre::Quaternion facing = view_orientation(subject_node);
    ViewUnit ignore;
    ignore.player = true;
    ViewUnit found;
    // `findCharacterWithinView(frame, alignment, 0, angle, reach + 1, true, this, skill)`,
    // then `setTarget`.
    if (find_character_within_view(
            me, facing, 0.0f, angle, reach + 1.0f, alignment, /*nearest=*/true, ignore, found,
            [&](const ViewUnit& who) { return player_skill_target_valid(skill, who); }) &&
        !found.player) {
        set_player_target(found.node);
        TL_LOG_INFO("skills: '%s' finds its target in view, node %lld", skill.entry.name.c_str(),
                    static_cast<long long>(found.node));
        return;
    }
    // Nothing: `findItemWithinView`, the multiplier applied to the range twice
    // as the original computes it, and `setTargetItem` on the breakable.
    const float item_reach =
        (player_stats.scale * player_stats.reach + 0.2f + reach) * player_stats.range_multiplier +
        1.0f;
    const int index = breakable_within_view(angle, item_reach);
    if (index < 0) return;
    // `setTargetItem`: refused while dying or dead.
    if (player_health.dead) return;
    const BreakableWorld& b = breakables[static_cast<size_t>(index)];
    player_target_node = 0;
    player_skill_item = b.id;
    const ogre::Vector3 there = view_position(b.node);
    TL_LOG_INFO("skills: '%s' finds %s in view, %.1f away", skill.entry.name.c_str(),
                b.unit.c_str(),
                static_cast<double>(ogre::Vector3(there.x - me.x, 0.0f, there.z - me.z).length()));
}

void World::execute_event_skills(const SkillEventSource& source,
                                 const tl::core::SkillEventSpec& spec, const ViewUnit* unit,
                                 const tl::core::Vec3& origin, const ogre::Quaternion& facing,
                                 const tl::core::Vec3& point) {
    if (spec.execute_skills.empty()) return;
    // INVENTED: a placed unit's `EXECUTE_SKILL` is not run (no unit skill authors one yet).
    if (source.caster_machine != nullptr) return;
    const tl::core::KnownSkill* parent = player_skill(source.skill);
    // The 6th argument: the parent skill's invested level (`CSkill +0xdc`).
    const uint32_t level = parent != nullptr ? parent->invested : source.level;
    // `CASTFROMTARGET`: by the target, from the point, at no one; else by the caster.
    const bool from_target = spec.cast_from_target && unit != nullptr;
    const tl::core::Vec3 from = from_target ? point : origin;
    const auto run = [&](const std::string& name) {
        // `CExecuteSkillProps::executeSkill`.
        const tl::core::KnownSkill* skill = add_executed_skill(name, level);
        if (skill == nullptr || skill->def == nullptr) {
            TL_LOG_WARN("skills: '%s' executes '%s', which no skill file names",
                        source.skill.c_str(), name.c_str());
            return;
        }
        const tl::core::SkillProperty* property = skill->property(level);
        ViewUnit caster_unit;
        if (from_target) {
            caster_unit = *unit;
        } else {
            caster_unit.player = true;
        }
        ViewUnit target;
        bool has_target = !from_target && unit != nullptr;
        if (has_target) target = *unit;
        // No target: the nearest character in view from the point, `FINDTARGETANGLE` and
        // range plus 1.2, the caster passed by.
        if (!has_target) {
            const float range = property != nullptr ? property->range : 1.0f;
            has_target = find_character_within_view(
                from, facing, 0.0f, property != nullptr ? property->find_target_angle : 0.0f,
                range + 0.2f + 1.0f, skill->def->target_alignment, true, caster_unit, target);
        }
        tl::core::SkillCaster caster = player_caster();
        caster.position = from;
        caster.target_position = point;
        tl::core::SkillCastContext context = player_cast_context();
        context.event_target = tl::core::SkillEventTarget{};
        context.event_target.origin = from;
        context.event_target.yaw = facing.getYaw().valueRadians();
        if (has_target) {
            tl::core::Vec3 centre;
            if (target.player) {
                const float body = player_stats.radius > 0.0f ? player_stats.radius
                                                              : tl::core::kDefaultCollisionRadius;
                const tl::core::Vec3 feet = player.position();
                centre = {feet.x, feet.y + body, feet.z};
            } else {
                tl::core::Vec3 feet;
                target.machine->unit_position(target.node, feet);
                centre = {feet.x, feet.y + target.machine->unit_radius(target.node), feet.z};
            }
            tl::core::SkillTargetUnit aimed;
            aimed.position = centre;
            caster.target_unit = aimed;
            context.event_target.has_unit = true;
            context.event_target.unit = target.player ? 0 : target.node;
            context.event_target.machine = target.player ? nullptr : target.machine;
        }
        // `getSkillCanBeExecuted`.
        if (!player_skill_runner.can_be_executed(*skill, level, caster)) {
            TL_LOG_INFO("skills: '%s' executes '%s': %s", source.skill.c_str(),
                        skill->entry.name.c_str(), has_target ? "refused" : "no target");
            return;
        }
        // The events it starts belong to the unit that cast it.
        Ogre::SceneNode* cast_by = nullptr;
        if (from_target && !caster_unit.player) {
            if (NodeMap* nodes = nodes_of(caster_unit.machine)) {
                const auto at = nodes->find(caster_unit.node);
                if (at != nodes->end()) cast_by = at->second;
            }
        }
        Ogre::SceneNode* const saved = event_caster_override;
        tl::core::LevelScript* const saved_machine = event_caster_override_machine;
        const int64_t saved_node = event_caster_override_node;
        event_caster_override = cast_by;
        event_caster_override_machine = cast_by != nullptr ? caster_unit.machine : nullptr;
        event_caster_override_node = cast_by != nullptr ? caster_unit.node : 0;
        const bool started = player_skill_runner.execute(*skill, level, caster, context);
        event_caster_override = saved;
        event_caster_override_machine = saved_machine;
        event_caster_override_node = saved_node;
        TL_LOG_INFO("skills: '%s' executes '%s' at level %u%s", source.skill.c_str(),
                    skill->entry.name.c_str(), level, started ? "" : " (not started)");
    };
    if (spec.execute_skill_count <= 0) {
        for (const std::string& name : spec.execute_skills) {
            if (!name.empty()) run(name);
        }
        return;
    }
    // `CRandomizer::getRandom` on `g_Rand`, each at 100.
    tl::core::Randomizer picks;
    for (size_t i = 0; i < spec.execute_skills.size(); ++i) {
        picks.add_choice(static_cast<int32_t>(i), 100);
    }
    for (int k = 0; k < spec.execute_skill_count; ++k) {
        const int32_t index = picks.get_random(tl::core::game_random());
        run(tl::core::tarray_at(spec.execute_skills, static_cast<size_t>(index)));
    }
}

// PORT: one observer per running skill, made on first use.
World::SkillObserver* World::skill_observer_for(tl::core::LevelScript* caster_machine,
                                                int64_t caster_node, const std::string& skill,
                                                uint32_t run) {
    std::unique_ptr<SkillObserver>& held =
        skill_observers[std::make_tuple(caster_machine, caster_node, skill, run)];
    if (!held) {
        held = std::make_unique<SkillObserver>();
        held->impl = this;
        held->caster_machine = caster_machine;
        held->caster_node = caster_node;
        held->skill = skill;
        held->run = run;
    }
    return held.get();
}

// `CSkill::unitStateChange(CBaseUnit*, EUNIT_STATES)`: at
// 1 and 4 the skill stops watching; at 4, if the unit holds one of its effects
// (`CEffectManager::hasEffectFromSkill`, inline) and a node, group 8
// `EVENT_DIEBYEFFECT` fires at it (`triggerEvent`).
void World::skill_unit_state_change(SkillObserver& observer, tl::core::LevelScript* machine,
                                    int64_t node, int state) {
    if (state != tl::core::unit_state::kDied && state != tl::core::unit_state::kDiedOfEffect) {
        return;
    }
    unit_observers.remove({machine, node}, &observer);
    if (state != tl::core::unit_state::kDiedOfEffect) return;
    const tl::core::EffectManager* effects =
        machine != nullptr ? unit_effect_manager(machine, node) : &player_effects;
    if (effects == nullptr || !effects->has_effect_from_skill(observer.skill)) return;

    tl::core::SkillEventTarget target;
    target.has_unit = true;
    target.machine = machine;
    target.unit = node;
    target.run = observer.run;
    if (machine == nullptr) {
        if (subject_node == nullptr) return;
        const float body =
            player_stats.radius > 0.0f ? player_stats.radius : tl::core::kDefaultCollisionRadius;
        const tl::core::Vec3 feet = player.position();
        target.origin = tl::core::Vec3{feet.x, feet.y + body, feet.z};
        target.yaw = player.facing();
    } else {
        tl::core::Vec3 feet;
        if (!machine->unit_position(node, feet)) return;
        target.origin = tl::core::Vec3{feet.x, feet.y + machine->unit_radius(node), feet.z};
        float yaw = 0.0f;
        if (machine->unit_facing(node, yaw)) target.yaw = yaw;
    }
    constexpr int kDieByEffect = 8;
    tl::core::LevelScript* const caster_machine = observer.caster_machine;
    if (caster_machine != nullptr) {
        tl::core::LevelScript::UnitCasting* casting =
            caster_machine->unit_casting(observer.caster_node);
        const tl::core::KnownSkill* skill =
            unit_skill_named(*caster_machine, observer.caster_node, observer.skill);
        if (casting == nullptr || skill == nullptr) return;
        tl::core::SkillCastContext context =
            unit_cast_context(*caster_machine, observer.caster_node, machine, node, true);
        context.event_target = target;
        const UnitEventCaster saved = unit_event_caster;
        unit_event_caster = unit_event_caster_for(caster_machine, observer.caster_node);
        if (casting->runner.trigger_skill_event(*skill, kDieByEffect, context)) {
            TL_LOG_INFO("skills: %s's '%s' fires EVENT_DIEBYEFFECT",
                        caster_machine->unit_of(observer.caster_node).c_str(),
                        observer.skill.c_str());
        }
        unit_event_caster = saved;
        return;
    }
    const tl::core::KnownSkill* skill = player_skill(observer.skill);
    if (skill == nullptr) return;
    tl::core::SkillCastContext context = player_cast_context();
    context.event_target = target;
    if (skill_runner_running(skill->entry.name)
            .trigger_skill_event(*skill, kDieByEffect, context)) {
        TL_LOG_INFO("skills: the player's '%s' fires EVENT_DIEBYEFFECT", observer.skill.c_str());
    }
}

// PORT: the observer hands the change to its world (`CSkill::unitStateChange`).
void WorldState::SkillObserver::unit_state_change(const void* machine, int64_t unit, int state) {
    impl->skill_unit_state_change(
        *this, static_cast<tl::core::LevelScript*>(const_cast<void*>(machine)), unit, state);
}

std::vector<tl::core::ActiveUnitCandidate> World::breakable_item_candidates() const {
    // The near item list (`CLevel::updateNearUnitList`), newest
    // first (`CLevel::addItem`); broken ones too.
    std::vector<tl::core::ActiveUnitCandidate> out;
    for (size_t i = breakables.size(); i-- > 0;) {
        const BreakableWorld& b = breakables[i];
        if (b.node == nullptr || !b.listed || !b.state.usable) continue;
        const ogre::Vector3 at = view_position(b.node);
        out.push_back({b.id, {at.x, at.y, at.z}});
    }
    return out;
}

void World::refresh_skill_effects() {
    ensure_effect_data();
    load_stat_graphs();
    skill_effects.types = &effect_types;
    skill_effects.graphs = &stat_graphs;
    // `CEffect::calculateBaseValue` scales by the owner's four stats: who reads matters.
    const tl::core::CharacterEffects total = player_total();
    skill_effects.owner.strength = player_character.strength(total);
    skill_effects.owner.dexterity = player_character.dexterity(total);
    skill_effects.owner.magic = player_character.magic(total);
    skill_effects.owner.defense = player_character.defense(total);
    skill_effects.owner.level = static_cast<uint32_t>(std::max(1, player_character.level()));
    skill_effects.owner.present = true;
    skill_effects.affix = [this](const std::string& name, uint32_t) -> const tl::core::Affix* {
        return affixes.get_affix(name);
    };
    // What `CExecuteSkillProps::getSkillStats` reaches through the owner's
    // skill manager (`EffectDisplayContext::skill_level_property`).
    skill_effects.skill_level_property = [this](const std::string& name,
                                                uint32_t level) -> const tl::core::SkillProperty* {
        ensure_player_skill_files();
        const auto found = player_skill_files.find(tl::core::upper_ascii(name));
        if (found == player_skill_files.end()) return nullptr;
        tl::core::KnownSkill described;
        described.def = &found->second->def;
        described.effective = level == 0 ? 1 : level;
        return described.property(level);
    };
    skill_effects.skill_display_name = [this](const std::string& name) -> std::string {
        ensure_player_skill_files();
        const auto found = player_skill_files.find(tl::core::upper_ascii(name));
        if (found == player_skill_files.end()) return "???";
        const std::string display = found->second->def.display_name;
        return display.empty() ? "???" : display;
    };
    skill_effects.monster_display_name = [this](const std::string& name) -> std::string {
        ensure_monster_index();
        const auto found = monster_files.find(lowered(name));
        if (found == monster_files.end()) return "???";
        const std::vector<uint8_t> bytes = read_resource(found->second);
        if (bytes.empty()) return "???";
        const tl::formats::AdmFile file = parse_data_group(bytes.data(), bytes.size());
        const std::string display = file.string_or("DISPLAYNAME", "");
        return display.empty() ? "???" : display;
    };
    view_skill_effects_changed();

    // The applied manager reads the same tables and owner stats (`CEffect::calculateBaseValue`).
    load_unit_themes();
    tl::core::EffectManager::Context applied;
    applied.types = &effect_types;
    applied.graphs = &stat_graphs;
    applied.owner = skill_effects.owner;
    applied.theme_book = &unit_themes;
    // `CBaseUnit::activateEffect` on the
    // player: a cast-skill effect teaches the skill it names.
    applied.on_activate = [this](tl::core::Effect& effect) {
        tl::core::activate_effect(effect, player_proc_skills, stat_graphs, skill_file_lookup());
    };
    player_effects.set_context(applied);
    // `CSkill::calculateEffectiveSkillLevel` asks the
    // character's manager and its pet's (not held yet: null) for `+N to <skill>`.
    player_skills.set_effect_managers(player_total(), nullptr);
}

// `CSpawnClassParser::CSpawnClassParser`
// starts both its maps empty and runs `CSpawnClassParser::reloadFromDirectory`
// at once:
// every file of `media/SpawnClasses/` (`CFileSystem::getFileList`) through
// `CSpawnClassParser::createEmptySpawnClass`
// first, then `fillEmptySpawnClass` on each. The first pass files a class
// under its `NAME`, upper-cased - not under its file's name: 39 shipped files
// are spelt `...SPWAN...` and name themselves `...SPAWN...`, which is how the
// random dungeons' monster classes and the general merchant's stock ask for
// them. A file with an empty `NAME`, or `DONOTSPAWN`, makes none; a name
// already filed keeps its first file. `CSpawnClassParser::getSpawnClass` is
// then a lookup by the upper-cased name. The port parses a class's objects
// the first time it is asked for, from the file this finds.
std::string World::spawn_class_path(const std::string& name) const {
    if (!spawn_classes_indexed) {
        spawn_classes_indexed = true;
        for (const std::string& path : data_paths_in_listing_order("media/spawnclasses/")) {
            const std::vector<uint8_t> bytes = read_resource(path);
            if (bytes.empty()) continue;
            const tl::formats::AdmFile file = parse_data_group(bytes.data(), bytes.size());
            if (!file.ok) continue;
            const std::string key = tl::core::upper_ascii(file.root.string_or("NAME", ""));
            if (key.empty() || file.root.bool_or("DONOTSPAWN", false)) continue;
            spawn_class_paths.emplace(key, path);
        }
        TL_LOG_INFO("spawn classes: %zu by name", spawn_class_paths.size());
    }
    const auto it = spawn_class_paths.find(tl::core::upper_ascii(name));
    return it == spawn_class_paths.end() ? std::string() : it->second;
}

/// `CResourceManager::getSpawnClassByName`, as `chooseUnitSpawners` asks it.
tl::core::SpawnClassLookup World::archive_spawn_class_lookup() const {
    return [this](const std::string& name, std::vector<tl::core::SpawnClassRef>& entries) {
        // `CSpawnClassParser::getSpawnClass`: by its upper-cased `NAME`.
        const std::string path = spawn_class_path(name);
        if (path.empty()) return false;
        const std::vector<uint8_t> bytes = read_resource(path);
        if (bytes.empty()) return false;
        const tl::formats::AdmFile file = parse_data_group(bytes.data(), bytes.size());
        if (!file.ok) return false;
        // `CSpawnClassParser::createSpawnClassData`:
        // `UNITTYPE`, then `UNIT`, then
        // `SPAWNCLASS`; an object with none adds no entry, and neither does a
        // `SPAWNCLASS` naming no class (`createSpawnClassDataRefSpawnClass`).
        //
        for (const tl::formats::AdmSection& object : file.root.children) {
            if (object.name != "OBJECT") continue;
            tl::core::SpawnClassRef entry;
            if (object.string_or("UNITTYPE", "").empty() && object.string_or("UNIT", "").empty()) {
                entry.spawn_class = object.string_or("SPAWNCLASS", "");
                if (entry.spawn_class.empty() || spawn_class_path(entry.spawn_class).empty()) {
                    continue;
                }
            }
            const int32_t weight = static_cast<int32_t>(object.integer_or("WEIGHT", 0xffffffffu));
            entry.weight = weight == 0 ? -1 : weight;
            entries.push_back(std::move(entry));
        }
        return true;
    };
}

Ogre::Entity* World::spawn_unit(const std::string& unit, const ogre::Vector3& at, int64_t owner,
                                int level, bool champion, bool spawn_anim,
                                const ogre::Quaternion& facing, float scale) {
    // Its own numbers first: the script wants speed, radius and attacks at once.
    index_unit(unit);
    ensure_monster_index();
    const auto found = monster_files.find(lowered(unit));
    if (found == monster_files.end()) return nullptr;

    const std::vector<uint8_t> bytes = read_resource(found->second);
    if (bytes.empty()) return nullptr;
    const tl::formats::AdmFile file = parse_data_group(bytes.data(), bytes.size());
    if (!file.ok) return nullptr;

    const std::string mesh_path = unit_mesh_path(unit);
    if (mesh_path.empty()) return nullptr;
    // An id of its own, not the spawner's: `Hide` on the spawner must not reach it.
    const int64_t id = kSpawnedNodeBase + static_cast<int64_t>(++spawn_serial);
    // The drawn scale when the caller has it; else the authored one until `apply_unit_scale`.
    const float placed_scale = scale > 0.0f ? scale : stats_for_unit(unit).scale;
    Ogre::Entity* entity =
        view_spawn_unit_model(unit, mesh_path, id, at, facing, placed_scale, spawn_anim);
    if (entity == nullptr) return nullptr;
    Ogre::SceneNode* node = view_parent_node(entity);
    spawned[owner].push_back(SpawnedUnit{node, entity, unit, id});
    if (level > 0 || champion) {
        unit_spawn_level[id] = SpawnLevel{level, champion};
    }
    return entity;
}

// PORT: the mesh a unit names (`MESHFILE` under `RESOURCEDIRECTORY`), in the archive.
std::string World::unit_mesh_path(const std::string& unit) {
    index_unit(unit);
    ensure_monster_index();
    const auto found = monster_files.find(lowered(unit));
    if (found == monster_files.end()) return std::string();
    const std::vector<uint8_t> bytes = read_resource(found->second);
    if (bytes.empty()) return std::string();
    const tl::formats::AdmFile file = parse_data_group(bytes.data(), bytes.size());
    if (!file.ok) return std::string();
    // The stats first: both keys inherit through `BASEFILE` (`UnitStats::mesh`).
    std::string mesh_name = stats_for_unit(unit).mesh;
    if (mesh_name.empty()) {
        std::string directory = file.string_or("RESOURCEDIRECTORY", "");
        const std::string mesh_file = file.string_or("MESHFILE", "");
        if (directory.empty() || mesh_file.empty()) return std::string();
        if (directory.back() != '/' && directory.back() != '\\') directory += '/';
        mesh_name = directory + mesh_file;
    }
    const std::string mesh_path = resolve_archive_path(mesh_name + ".MESH");
    if (mesh_path.empty()) {
        TL_LOG_WARN("unit '%s' names no mesh at '%s.MESH'", unit.c_str(), mesh_name.c_str());
        return std::string();
    }
    view_prepare_mesh(mesh_path);
    return mesh_path;
}

void World::build_spawn_context(SpawnTables& tables, bool items_by_type) {
    load_unit_types();
    load_stat_graphs();

    tl::core::SpawnContext& context = tables.context;
    context.unit_types = &unit_types;
    context.stats = &stat_graphs;
    // `CGameGlobals`' rarity weights and magic-find influences, as the file
    // authors them.
    {
        const std::vector<uint8_t> bytes = read_resource("media/GLOBALS.DAT.adm");
        const tl::formats::AdmFile globals =
            bytes.empty() ? tl::formats::AdmFile{} : parse_data_group(bytes.data(), bytes.size());
        if (globals.ok) tl::core::read_spawn_weights(globals, context);
    }

    // `CSpawnClassParser::getSpawnClassDataByName(const std::wstring&)`:
    // keyed upper-case, null for none; each file read on first asking.
    context.get_spawn_class = [this,
                               &tables](const std::string& name) -> const tl::core::SpawnClass* {
        const std::string key = tl::core::upper_ascii(name);
        auto it = tables.classes.find(key);
        if (it != tables.classes.end()) return &it->second;
        std::optional<tl::core::SpawnClass> loaded = load_spawn_class(name, tables.context);
        if (!loaded) return nullptr;
        auto inserted = tables.classes.emplace(key, std::move(*loaded));
        return &inserted.first->second;
    };

    context.get_unit_by_name =
        [this, &tables](const std::string& name) -> const tl::core::SpawnUnitData* {
        auto it = tables.units.find(name);
        if (it != tables.units.end()) return &it->second;

        // 1. Check items (media/masterresourceunits.dat)
        ensure_item_index();
        if (const tl::core::ItemStats* item = items.find(name)) {
            auto inserted = tables.units.emplace(name, spawn_data_of(*item));
            return &inserted.first->second;
        }

        // 2. Check monsters (media/units/monsters/)
        ensure_monster_index();
        auto it_m = monster_files.find(lowered(name));
        if (it_m != monster_files.end()) {
            const std::vector<uint8_t> m_bytes = read_resource(it_m->second);
            if (!m_bytes.empty()) {
                const tl::formats::AdmFile m_file =
                    tl::formats::parse_adm(m_bytes.data(), m_bytes.size());
                if (m_file.ok) {
                    auto inserted =
                        tables.units.emplace(name, tl::core::SpawnUnitData::from_adm(m_file.root));
                    return &inserted.first->second;
                }
            }
        }

        // 3. Check props (media/units/props/)
        ensure_prop_index();
        auto it_p = prop_files.find(lowered(name));
        if (it_p != prop_files.end()) {
            const std::vector<uint8_t> p_bytes = read_resource(it_p->second);
            if (!p_bytes.empty()) {
                const tl::formats::AdmFile p_file =
                    tl::formats::parse_adm(p_bytes.data(), p_bytes.size());
                if (p_file.ok) {
                    auto inserted =
                        tables.units.emplace(name, tl::core::SpawnUnitData::from_adm(p_file.root));
                    return &inserted.first->second;
                }
            }
        }

        return nullptr;
    };

    context.get_units_by_type =
        [this, items_by_type](uint32_t type_id) -> std::vector<tl::core::SpawnUnitData> {
        if (!items_by_type) {
            static std::set<std::string> warned_unittypes;
            std::string type_name = unit_types.name_of(type_id);
            if (type_name.empty()) type_name = std::to_string(type_id);
            if (warned_unittypes.insert(type_name).second) {
                TL_LOG_WARN(
                    "spawn class picks by UNITTYPE '%s', which a spawner here does not index",
                    type_name.c_str());
            }
            // DIVERGENCE: a `Unit Spawner`'s roll has no units by UNITTYPE: empty.
            return {};
        }
        // `CUnitResourceList::getUnitsByType`: each unit
        // that `ISA`s the type, groups in order, each by upper-cased `NAME`; the order is
        // what a weighted pick draws against. DIVERGENCE: only the items are walked.
        ensure_item_index();
        std::vector<std::pair<std::string, const tl::core::ItemStats*>> ordered;
        for (const auto& [key, item] : items.all()) {
            if (item.unittype.empty() || !unit_types.isa(item.unittype, type_id)) continue;
            ordered.emplace_back(tl::core::upper_ascii(item.name), &item);
        }
        std::sort(ordered.begin(), ordered.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<tl::core::SpawnUnitData> found;
        for (const auto& [upper, item] : ordered) found.push_back(spawn_data_of(*item));
        return found;
    };
}

}  // namespace tl::core
