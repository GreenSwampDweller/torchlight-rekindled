// The effects a placed character holds: its CEffectManager (+0x1b8), the gate on the way in
// (CCharacter::applyEffectOnUnit) and the per-frame half of CCharacter::update (updateEffects,
// updateHP). The player's own manager is elsewhere.
#include "core/world.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tl::core {

namespace {
// `EEFFECT_TYPE` rows of `media/EffectsList.dat` this file branches on.
constexpr int kKnockBackEffect = 0x2b;
constexpr int kAdvanceEffect = 0x91;
constexpr int kTransform = 0x39;
constexpr int kTransformPermanent = 0x49;
constexpr int kInterruptChance = 0x38;
constexpr int kVelocityMult = 0x3f;
constexpr int kStun = 0x40;
constexpr int kFleeEffect = 0x4f;
constexpr int kTurnAlignment = 0x50;
constexpr int kFreeze = 0x6a;
constexpr int kBurn = 0x6b;
constexpr int kPoison = 0x6c;
constexpr int kShock = 0x6d;
constexpr int kKill = 0x6e;
constexpr int kFadeOut = 0x6f;
constexpr int kSetVisible = 0x70;
constexpr int kSilence = 0x80;
constexpr int kSummonDuration = 0x2f;
constexpr int kPercentKnockBackResistance = 0x35;
constexpr int kFleeResist = 0x79;
constexpr int kInterruptResistance = 0x88;
constexpr int kCharmResistance = 0x89;
constexpr int kSilenceResistance = 0x8a;
constexpr int kStunResistance = 0x8b;
constexpr int kPercentBlind = 0x90;

// Burn, poison and shock's second chance: `+0x618` (the level's health curve) against
// `m_DamageDefense[type]` (`+0x588`), each made a float, then `randomBetweenVolatile(0, 1)`.
bool resists_status(const tl::core::UnitCombat* combat, int damage_type) {
    if (combat == nullptr) return false;
    const float level = static_cast<float>(static_cast<uint32_t>(combat->level_health));
    const float defense =
        static_cast<float>(combat->damage_defense[static_cast<size_t>(damage_type)]);
    const float chance = level / (defense + level);
    return chance < tl::core::volatile_random().between(0.0f, 1.0f);
}

// The value a unit's manager holds for an effect type, none without one.
float held_value(const tl::core::EffectManager* own, int type) {
    return own != nullptr ? own->effect_value(type) : 0.0f;
}

// The damage type whose defense resists a status: FIRE (`+0x590`) for a burn, POISON (`+0x59c`)
// for a poison, ELECTRIC (`+0x598`) for a shock.
int status_damage_type(int effect_type) {
    if (effect_type == kBurn) return 2;
    return effect_type == kPoison ? 5 : 4;
}

// `INTERRUPT CHANCE`'s value: none on a `BOSS_CHAMPION` (`+0x69a`), a third on a champion
// (`+0x63a`).
float interrupt_chance(float chance, const tl::core::UnitStats& stats,
                       const tl::core::UnitCombat* combat) {
    if (stats.boss_champion && !stats.champion) return 0.0f;
    if (combat != nullptr && combat->champion) return chance * 0.333333343f;
    return chance;
}

// `cloneEffect`'s three rolls: `clone`'s `setOwner`, the constructor's
// `calculateBaseValue(0)`, then `setOwner(source, true)`.
void roll_cloned_effect(tl::core::Effect& effect, const tl::core::StatBook& graphs,
                        const tl::core::EffectOwner& source) {
    for (int roll_n = 0; roll_n < 3; ++roll_n) {
        effect.rolled_value = tl::core::effect_base_value(
            effect, tl::core::EffectCalculate::Roll, graphs, source, &tl::core::volatile_random());
    }
}
}  // namespace

tl::core::EffectManager* World::unit_effect_manager(tl::core::LevelScript* machine, int64_t node) {
    if (machine == nullptr) return nullptr;
    refresh_skill_effects();
    load_unit_themes();
    // What the player's manager reads, with the player as the owner whose
    // stats `CEffect::calculateBaseValue` scales by: every effect reaching a
    // unit here was applied by one of the player's skills.
    tl::core::EffectManager::Context context;
    context.types = &effect_types;
    context.graphs = &stat_graphs;
    context.owner = skill_effects.owner;
    context.theme_book = &unit_themes;
    context.on_unit = [this, machine, node](tl::core::Effect& effect) {
        return unit_effect_on_unit(machine, node, effect);
    };
    // `CBaseUnit::activateEffect` on every
    // effect the unit holds.
    context.on_activate = [this, machine, node](tl::core::Effect& effect) {
        unit_activate_effect(machine, node, effect);
    };
    tl::core::EffectManager* manager = machine->ensure_unit_effects(node, context);
    if (manager != nullptr) manager->set_context(context);
    return manager;
}

tl::core::Vec3 World::knock_back_push(tl::core::EffectManager& effects, float resistance, float dt,
                                      const std::string& who) {
    // CCharacter::updateEffects: KNOCK BACK EFFECT (0x2b)
    // force less PERCENT KNOCK BACK RESISTANCE (0x35, held to 0..1), and ADVANCE EFFECT (0x91)
    // whole, along their directions onto the outside push (+0x240), in the dynamic list's order;
    // the force falls to a thousandth per second.
    float share = resistance / 100.0f;
    if (!(share < 1.0f)) share = 1.0f;
    const float kept = 1.0f - (0.0f < share ? share : 0.0f);
    std::vector<tl::core::LiveEffect*> pushes = effects.live_effects_of_type(kKnockBackEffect);
    for (tl::core::LiveEffect* live : effects.live_effects_of_type(kAdvanceEffect))
        pushes.push_back(live);
    std::sort(pushes.begin(), pushes.end());
    tl::core::Vec3 add;
    for (tl::core::LiveEffect* live : pushes) {
        if (live->dead || live->expired()) continue;
        if (live->effect.activation != tl::core::EffectActivation::Dynamic) continue;
        const bool knock = live->effect.type == kKnockBackEffect;
        // `CEffect::value(0)`, not the rolled +0xc0.
        const float value = tl::core::effect_column_value(live->effect, 0, stat_graphs);
        const float force = knock ? value * kept : value;
        if (knock && live->elapsed <= dt) {
            TL_LOG_INFO(
                "effects: %s is knocked back, force %.2f along (%.2f, %.2f), resistance "
                "kept %.2f",
                who.c_str(), static_cast<double>(value),
                static_cast<double>(live->effect.direction[0]),
                static_cast<double>(live->effect.direction[2]), static_cast<double>(kept));
        }
        add.x = live->effect.direction[0] * force + add.x;
        add.y = live->effect.direction[1] * force + add.y;
        add.z = live->effect.direction[2] * force + add.z;
        // `+0xe0 = powf(0.001, dt) * value(0)`, then `calculateBaseValue(0)`: a roll between it
        // and the effect's larger end.
        live->effect.values[0] = std::pow(0.001f, dt) * value;
        live->value =
            tl::core::effect_base_value(live->effect, tl::core::EffectCalculate::Roll, stat_graphs,
                                        effects.context().owner, &tl::core::volatile_random());
    }
    return add;
}

// `isEffectValidForUnit` (vtable +0x110) for a placed unit, before the instant path.
bool World::effect_valid_for_placed_unit(tl::core::LevelScript* machine, int64_t node,
                                         const std::string& unit, tl::core::EffectManager* own,
                                         const tl::core::Effect& effect) {
    load_unit_types();
    const tl::core::UnitStats& stats = stats_for_unit(unit);
    tl::core::ItemUseTarget target;
    target.damageable = stats.damageable;
    target.unittype = stats.unittype;
    target.effects = own;
    // INVENTED: a placed unit's own `[THEMES]` are not carried; its effects' themes are.
    if (own != nullptr) {
        for (const tl::core::UnitTheme* theme : own->themes()) {
            if (theme != nullptr && theme->name == "STUNNED") target.stunned = true;
        }
    }
    const int32_t state = machine->unit_ai(node);
    target.dying_or_dead =
        state == tl::core::ai_state::kDying || state == tl::core::ai_state::kDead;
    const std::optional<tl::core::UnitStats::Alignment> side = machine->unit_side(node);
    target.alignment = side == tl::core::UnitStats::Alignment::Hostile    ? 2
                       : side == tl::core::UnitStats::Alignment::Friendly ? 1
                                                                          : 0;
    target.has_master = machine->unit_has_master(node);
    target.master_is_player = machine->unit_is_player_pet(node);
    const float first = tl::core::effect_column_value(effect, 0, stat_graphs);
    return tl::core::effect_valid_for_unit(target, nullptr, effect, first, unit_types);
}

// After the branch: whether the effect provokes the unit that took it.
void World::provoke_after_effect(tl::core::LevelScript* machine, int64_t node,
                                 const tl::core::Effect& effect, bool copy,
                                 const tl::core::EffectManager* own,
                                 std::vector<tl::core::ScriptEvent>& events) {
    // The player applied it: an enemy of the
    // unit's that leaves an idle unit provoked - its friends near it first
    // (`CLevel::reactToAreaDamage`), then the unit
    // (`reactToDamage`) - unless the effect is `PERCENT BLIND`. The `POTION EFFICIENCY`
    // scaling after it is a potion's (`ISA(0x21)` of the source), never a skill's. A prop's
    // effect has no source: nothing is provoked.
    // The state as the branch left it: a reaction above may have made it hunt or flee.
    const int32_t ai_now = machine->unit_ai(node);
    // `isEnemy(this, source)`: the side as it is now, a held `TURN ALIGNMENT` swapping it.
    // One taken here is already held there (`copyEffect` at the branch); the port adds it on return.
    // The source is the unit whose skill event or proc applied the effect when one did (its own
    // create-event effect provokes nothing: MAIN:19's Ice Wisp, 2026-11-10), else the player.
    bool enemy =
        instant_caster_machine != nullptr
            ? machine->unit_is_enemy_of(node, instant_caster_machine, instant_caster_node)
        : unit_event_caster.machine != nullptr
            ? machine->unit_is_enemy_of(node, unit_event_caster.machine, unit_event_caster.node)
            : machine->unit_is_enemy_of(node, nullptr, 0);
    if (effect.type == kTurnAlignment && copy && own != nullptr &&
        !own->has_effect(kTurnAlignment)) {
        const std::optional<tl::core::UnitStats::Alignment> side = machine->unit_side(node);
        if (side == tl::core::UnitStats::Alignment::Hostile) enemy = false;
        if (side == tl::core::UnitStats::Alignment::Friendly) enemy = true;
    }
    if (!instant_caster_none && effect.type != kPercentBlind && enemy &&
        (ai_now == tl::core::ai_state::kIdle || ai_now == tl::core::ai_state::kNone)) {
        tl::core::Vec3 at;
        if (machine->unit_position(node, at)) {
            machine->react_to_area_damage(0, nullptr, machine, node, at, machine->unit_radius(node),
                                          quest_log.state(), events);
        }
        TL_LOG_DEBUG("effects: %s provoked by '%s' (type %d) from %s",
                     machine->unit_of(node).c_str(), effect.name.c_str(), effect.type,
                     instant_caster_machine != nullptr      ? "a unit's proc"
                     : unit_event_caster.machine != nullptr ? "a unit's skill event"
                                                            : "the player");
        machine->provoke_unit(node, quest_log.state(), events);
    }
}

// the strike particle of the effect's owner (the unit itself
// with none) at the unit, whether the blow killed it or not.
void World::play_instant_strike(tl::core::LevelScript* machine, int64_t node,
                                const std::string& unit) {
    const bool by_unit = instant_caster_machine != nullptr;
    tl::core::Vec3 feet;
    if (machine->unit_position(node, feet)) {
        const float radius = machine->unit_radius(node);
        const ogre::Vector3 at(feet.x, feet.y + radius, feet.z);
        ogre::Vector3 from = at;
        if (by_unit) {
            tl::core::Vec3 theirs;
            if (instant_caster_machine->unit_position(instant_caster_node, theirs)) {
                from = ogre::Vector3(
                    theirs.x, theirs.y + instant_caster_machine->unit_radius(instant_caster_node),
                    theirs.z);
            }
            play_strike_particle(
                stats_for_unit(instant_caster_machine->unit_of(instant_caster_node)), from, at,
                radius, false, false, character_ref(instant_caster_node, instant_caster_machine));
        } else if (!instant_caster_none && subject_node != nullptr) {
            from = view_position(subject_node);
            play_strike_particle(player_stats, from, at, radius, false, false, CharacterRef{});
        } else {
            play_strike_particle(stats_for_unit(unit), at, at, radius, false, false,
                                 character_ref(node, machine));
        }
    }
}

// `CDamageShape::doDamageToCharacter`: `CEffect(0x34, 0, DYNAMIC,
// INSTANT, amount, 0)` with no owner, the shape's damage type at `+0xc` and the
// direction at `+0xac`, handed to the character's `applyEffectOnUnit` (vtable +0x114), then deleted.
void World::shape_hit_unit(tl::core::LevelScript& machine, int64_t node, float amount,
                           int32_t damage_type, const tl::core::Vec3& direction) {
    tl::core::Effect effect =
        tl::core::value_effect(tl::core::kEffectTypeDamage, tl::core::kDurationInstant, amount,
                               tl::core::EffectActivation::Dynamic);
    effect.damage_type = static_cast<tl::core::DamageType>(damage_type);
    tl::core::LevelScript* const saved_machine = instant_caster_machine;
    const int64_t saved_node = instant_caster_node;
    const bool saved_none = instant_caster_none;
    const bool saved_set = effect_direction_set;
    const std::array<float, 3> saved_direction = effect_direction;
    instant_caster_machine = nullptr;
    instant_caster_node = 0;
    instant_caster_none = true;
    effect_direction_set = true;
    effect_direction = {direction.x, direction.y, direction.z};
    unit_effect_on_unit(&machine, node, effect);
    instant_caster_machine = saved_machine;
    instant_caster_node = saved_node;
    instant_caster_none = saved_none;
    effect_direction_set = saved_set;
    effect_direction = saved_direction;
}

// `applyInstantEffect`'s case 0x34 (`DAMAGE`) on a placed character.
void World::unit_instant_damage(tl::core::LevelScript* machine, int64_t node,
                                const tl::core::Effect& effect, float value,
                                const std::string& unit) {
    if (machine->unit_is_dead(node)) return;
    // Case 0x34. A value that is not damage is `modifyHP`.
    // `instant_damage`'s own test, before the defender is
    // looked up.
    const float v = 0.0f < value ? -value : value;
    if (0.0f <= v) return;
    tl::core::Defender defender;
    if (!machine->unit_defender(node, defender)) return;
    defender.on_absorbed = [this, machine, node] { show_unit_absorbed(machine, node); };
    // The effect's owner (`+0x48`): the unit whose skill event this is, the
    // player, or for a prop's nobody.
    const bool by_unit = instant_caster_machine != nullptr;
    const tl::core::CharacterEffects attacker =
        by_unit
            ? tl::core::CharacterEffects(instant_caster_machine->unit_effects(instant_caster_node))
            : player_total();
    const int64_t attacker_node = instant_caster_none ? tl::core::LevelScript::kNoAttacker
                                  : by_unit           ? instant_caster_node
                                                      : 0;
    const int type = static_cast<int>(effect.damage_type);
    const tl::core::InstantDamage hit = tl::core::instant_damage(
        value, type, effect.values[2], effect.values[3], defender,
        instant_caster_none ? nullptr : &attacker, tl::core::volatile_random());
    if (hit.blocked) {
        // the `Blocked!` text over the target and its block sound.
        tl::core::Vec3 at;
        if (machine->unit_position(node, at)) {
            add_combat_text(tl::core::CombatText::Blocked, ogre::Vector3(at.x, at.y, at.z));
        }
        TL_LOG_INFO("combat: %s blocked '%s'", unit.c_str(), effect.name.c_str());
        return;
    }
    if (hit.interrupts) {
        std::vector<tl::core::ScriptEvent> events;
        machine->interrupt_unit(node, quest_log.state(), events);
        if (NodeMap* nodes = nodes_of(machine); nodes != nullptr && !events.empty()) {
            apply_script_events(events, *nodes);
        }
    }
    // `applyDamage(..., &effect +0x98, force, ...)`: the
    // effect's direction - `CEffect::initValues`
    // leaves it zero when the event set
    // none - and `hit.force`, the effect's values 2 and 3.
    tl::core::KillingBlow blow;
    blow.has_direction = true;
    if (effect_direction_set) {
        blow.direction = {effect_direction[0], effect_direction[1], effect_direction[2]};
    }
    blow.force = hit.force;
    // `applyDamage`'s burst flag (stack +0x67) is always clear here: a critical does not burst.
    blow.critical = false;
    hurt_unit(node, unit, static_cast<float>(hit.damage), hit.critical, attacker_node, machine,
              &blow, /*struck=*/true);
    // `applyDamageEffects(this, this, damage, {type, damage}, 1, false)`:
    // the level asked is the target's own. The console's own loop
    // rolls it whether the blow killed or not.
    {
        std::array<int, tl::core::kDamageTypeCount> by_type{};
        if (0 <= type && type < tl::core::kDamageTypeCount) {
            by_type[static_cast<size_t>(type)] = hit.damage;
        }
        int level = 1;
        machine->unit_level(node, level);
        // The unit is its own attacker (`applyDamageEffects(this, this, ...)`).
        apply_damage_status(machine, node, static_cast<float>(hit.damage), by_type, 1, type, false,
                            level, tl::core::EffectSource{true, machine, node});
    }
    // the strike particle, then the unit's hit sample, whether the
    // blow killed it or not.
    play_instant_strike(machine, node, unit);
    std::vector<tl::core::ScriptEvent> heard;
    machine->play_hit_sample(node, type, heard);
    if (NodeMap* nodes = nodes_of(machine); nodes != nullptr && !heard.empty()) {
        apply_script_events(heard, *nodes);
    }
}

// `updateEffects`' conversions: each held status into its affix.
void World::convert_held_statuses(int32_t ai, tl::core::EffectManager* effects,
                                  const std::string& unit) {
    // `FREEZE`, `BURN`, `POISON`, `SHOCK` held: the matching
    // `DAMAGEEFFECT*` affix (its duration refreshed when already held),
    // and the effect itself spent.
    const std::pair<int, const char*> conversions[] = {
        {kBurn, "DAMAGEEFFECTONFIRE"},
        {kFreeze, "DAMAGEEFFECTFROZEN"},
        {kShock, "DAMAGEEFFECTELECTRICITY"},
        {kPoison, "DAMAGEEFFECTPOISONED"},
    };
    for (const auto& conversion : conversions) {
        // Not while the unit is in state 0xc, dying (5), dead (6), 0x29 or 0x10.
        if (ai == 0xc || ai == tl::core::ai_state::kDying || ai == tl::core::ai_state::kDead ||
            ai == 0x29 || ai == 0x10)
            continue;
        for (;;) {
            // Asked again each time: the affix's effects go into the same lists.
            tl::core::LiveEffect* live = nullptr;
            for (tl::core::LiveEffect* held : effects->live_effects_of_type(conversion.first)) {
                if (!held->expired()) {
                    live = held;
                    break;
                }
            }
            if (live == nullptr) break;
            const float duration = live->effect.duration;
            const uint32_t level = live->effect.level;
            if (duration != tl::core::kDurationInstant && duration != tl::core::kDurationAlways) {
                live->elapsed = duration;
            }
            live->dead = true;
            // An `INSTANT` or `ALWAYS` one is spent without an affix.
            if (duration == tl::core::kDurationInstant || duration == tl::core::kDurationAlways)
                continue;
            if (!effects->set_affix_duration(conversion.second, duration)) {
                effects->add_affix_by_name(conversion.second, level, &affixes, duration);
                TL_LOG_INFO("effects: %s takes %s for %.2fs", unit.c_str(), conversion.second,
                            static_cast<double>(duration));
            }
        }
    }
}

// `CCharacter::updateHP` for a placed character, after its effects were ticked.
void World::update_unit_hp_of(const MachineView& view, int64_t node,
                              tl::core::EffectManager* effects, const std::string& unit, float dt,
                              std::vector<tl::core::ScriptEvent>& events) {
    tl::core::LevelScript* machine = view.script;
    // CCharacter::updateHP: base rate, then the
    // effects; an INVINCIBLE character with a master regenerates at
    // PET_HP_RECHARGE_RATE. The player's animal reads its bag as well.
    float hp = 0.0f;
    float max = 0.0f;
    if (!machine->unit_health(node, hp, max)) return;
    const bool invincible_pet =
        machine->unit_is_player_pet(node) && stats_for_unit(unit).invincible_flag;
    const float change =
        machine == &script && node == pet_node_id
            ? tl::core::hp_change(max, recharge_rates, pet_total(), dt, invincible_pet)
            : tl::core::hp_change(max, recharge_rates, effects, dt, invincible_pet);
    // the effects' damage this frame goes onto the number
    // over the unit, whatever the regeneration gave back.
    const float effect_damage =
        (machine == &script && node == pet_node_id ? tl::core::effect_values_of_damage(pet_total())
                                                   : tl::core::effect_values_of_damage(effects)) *
        dt;
    if (effect_damage < 0.0f) machine->add_unit_damage_shown(node, -effect_damage);
    if (change > 0.0f) {
        machine->heal_unit(node, change);
    } else if (change < 0.0f) {
        // `modifyHP` alone: no reaction.
        machine->set_defer_death_sample(true);
        const bool killed = machine->damage_unit(node, -change, tl::core::LevelScript::kNoAttacker,
                                                 quest_log.state(), events, nullptr, nullptr, false,
                                                 /*shown=*/false);
        machine->set_defer_death_sample(false);
        if (NodeMap* nodes = view.nodes; nodes != nullptr && !events.empty()) {
            apply_script_events(events, *nodes);
        }
        if (killed) {
            // the effect that killed credits its owner,
            // whom `CCharacter::update` hands `die` as the attacker
            // - the player 0, a placed unit its node,
            // nobody none (no experience).
            const tl::core::EffectSource credit = effects->take_death_credit();
            const int64_t killer = !credit.set       ? tl::core::LevelScript::kNoAttacker
                                   : credit.player() ? 0
                                                     : credit.node;
            TL_LOG_INFO("effects: %s dies of its effects%s", unit.c_str(),
                        !credit.set       ? ", nobody's"
                        : credit.player() ? ", the player's"
                                          : "");
            // under 1 HP, whoever watches the unit hears
            // that it died of an effect (inline e14),
            // before `die` says it died.
            unit_observers.broadcast({machine, node}, tl::core::unit_state::kDiedOfEffect);
            kill_unit(node, unit, /*fall=*/false, killer);
        }
    }
}

bool World::unit_effect_on_unit(tl::core::LevelScript* machine, int64_t node,
                                tl::core::Effect& effect) {
    // CCharacter::applyEffectOnUnit, the player as source;
    // true copies it.
    const std::string unit = machine->unit_of(node);
    tl::core::EffectManager* own = machine->unit_effects(node);
    // isEffectValidForUnit (vtable +0x110) first, the instant path after it.
    if (!effect_valid_for_placed_unit(machine, node, unit, own, effect)) return false;
    const auto resist = [own](int type) { return held_value(own, type); };
    // `CAffix::setDirection`: the direction the skill event applied
    // its affix along, `UNIT_Z` when it had none.
    effect.direction =
        effect_direction_set ? effect_direction : std::array<float, 3>{0.0f, 0.0f, 1.0f};
    const auto roll = [] {
        return static_cast<float>(tl::core::volatile_random().integer_between(0, 100));
    };
    const tl::core::EffectOwner source =
        instant_caster_none ? tl::core::EffectOwner{} : skill_effects.owner;
    const float value = tl::core::effect_base_value(
        effect, tl::core::EffectCalculate::Roll, stat_graphs, source, &tl::core::volatile_random());
    const int32_t ai = machine->unit_ai(node);
    const bool dying = ai == tl::core::ai_state::kDying || ai == tl::core::ai_state::kDead;
    std::vector<tl::core::ScriptEvent> events;
    const auto react = [&] {
        TL_LOG_DEBUG("effects: %s reacts to '%s' (type %d)", unit.c_str(), effect.name.c_str(),
                     effect.type);
        machine->provoke_unit(node, quest_log.state(), events);
    };

    // An `INSTANT` effect: `applyInstantEffect`,
    // never copied.
    if (effect.duration == tl::core::kDurationInstant) {
        unit_instant_effect(machine, node, effect, value);
        return false;
    }
    if (machine->unit_is_dead(node)) return false;

    // `SUMMON DURATION` with a duration that is not `ALWAYS`: the value is
    // how long it lasts (`+0x24 = +0xc0`).
    if (effect.type == kSummonDuration && effect.duration != tl::core::kDurationAlways) {
        effect.duration = value;
    }
    bool copy = false;
    // `copyEffect` -> `cloneEffect` at the branch's own point: an effect not
    // held by an affix of this manager becomes a new `CEffect` (`clone`'s `setOwner`, the
    // constructor's `calculateBaseValue(0)`), then `setOwner(source, true)` - three rolls.
    const bool clones = own != nullptr && own->offering_copy();
    const auto take = [&] {
        copy = true;
        if (clones) roll_cloned_effect(effect, stat_graphs, source);
    };
    bool after = true;  // the tail's provocation test
    const tl::core::UnitCombat* combat = machine->unit_combat(node);
    switch (effect.type) {
        case tl::core::kEffectTypeHpRecharge:
        case tl::core::kEffectTypeHpRechargePlayer: take(); break;
        case kInterruptChance: {
            // `CEffect::value(0)`: none on a `BOSS_CHAMPION` (`+0x69a`), a third on a champion
            // (`+0x63a`).
            const float chance =
                interrupt_chance(tl::core::effect_column_value(effect, 0, stat_graphs),
                                 stats_for_unit(unit), combat);
            if (static_cast<float>(tl::core::volatile_random().integer_between(1, 100)) <= chance) {
                if (resist(kInterruptResistance) < roll()) {
                    machine->interrupt_unit(node, quest_log.state(), events);
                }
                react();
            }
            break;
        }
        case kVelocityMult:
            // INVENTED: `+0x1e4..+0x1ec`, this frame's velocity, is zeroed here; the machine
            // sets it again on its next step, so nothing is cleared.
            take();
            break;
        case kStun:
            if (!dying && roll() <= value) {
                if (resist(kStunResistance) < roll() &&
                    resist(kPercentKnockBackResistance) < roll()) {
                    machine->interrupt_unit(node, quest_log.state(), events);
                    take();
                }
            }
            react();
            break;
        case kFleeEffect: {
            tl::core::Vec3 from = player.position();
            if (machine->apply_flee_effect(node, value, effect.duration, &from, quest_log.state(),
                                           events)) {
                take();
            }
            if (copy) TL_LOG_INFO("skill event: %s flees", unit.c_str());
            (void)kFleeResist;
            break;
        }
        case kTurnAlignment:
            // INVENTED: a charmed unit's change of side while it holds the
            // effect is not modelled; the effect is only held.
            // Only a copy goes on to the tail.
            if (!dying && !(value < roll()) && resist(kCharmResistance) < roll()) take();
            after = copy;
            break;
        case kFreeze:
            if (!dying && roll() <= value) {
                machine->interrupt_unit(node, quest_log.state(), events);
                take();
            }
            react();
            break;
        case kBurn:
        case kPoison:
        case kShock:
            if (!dying && roll() <= value) {
                const int damage_type = status_damage_type(effect.type);
                if (!resists_status(combat, damage_type)) {
                    machine->interrupt_unit(node, quest_log.state(), events);
                    take();
                }
            }
            react();
            break;
        case kKill:
            // `die` through its vtable (+0x164) with the source as the killer: no experience.
            kill_unit(node, unit, /*fall=*/true, 0, /*pays_experience=*/false);
            return false;
        case kSilence:
            // Only a copy goes on to the tail.
            if (!dying && !(value < roll()) && resist(kSilenceResistance) < roll()) take();
            after = copy;
            break;
        default:
            // `copyEffect` for an effect activated passively, dynamically or by
            // transfer - every activation there is.
            if (effect.activation == tl::core::EffectActivation::Passive ||
                effect.activation == tl::core::EffectActivation::Dynamic ||
                effect.activation == tl::core::EffectActivation::Transfer) {
                take();
            }
            break;
    }
    if (after) provoke_after_effect(machine, node, effect, copy, own, events);
    if (NodeMap* nodes = nodes_of(machine); nodes != nullptr && !events.empty()) {
        apply_script_events(events, *nodes);
    }
    if (copy) {
        TL_LOG_INFO("effects: %s holds '%s' (%s) for %.2fs", unit.c_str(), effect.name.c_str(),
                    effect_types.at(effect.type) != nullptr
                        ? effect_types.at(effect.type)->name.c_str()
                        : "?",
                    static_cast<double>(effect.duration));
    }
    return copy;
}

void World::unit_instant_effect(tl::core::LevelScript* machine, int64_t node,
                                const tl::core::Effect& effect, float value) {
    // `CCharacter::applyInstantEffect` on a
    // placed character, the cases a player skill reaches it with.
    const std::string unit = machine->unit_of(node);
    if (effect.type == tl::core::kEffectTypeDamage) {
        unit_instant_damage(machine, node, effect, value, unit);
        return;
    }
    // Cases 7 HP RECHARGE and 0x7c HP RECHARGE PLAYER: a value >= 0 is
    // modifyHP(value), on a body too (only a whole-maxHP heal fills it, and clears +0x454:
    // no experience on its next death). isEffectValidForUnit
    // refuses both on a non-DAMAGEABLE unit. A negative value goes on
    // as case 0x34 `DAMAGE` does: `blocked()`, then the damage.
    if (effect.type == tl::core::kEffectTypeHpRecharge ||
        effect.type == tl::core::kEffectTypeHpRechargePlayer) {
        if (!stats_for_unit(unit).damageable) return;
        if (0.0f <= value) {
            if (machine->modify_unit_hp(node, value)) no_xp_units.insert(node);
            return;
        }
        unit_instant_damage(machine, node, effect, value, unit);
        return;
    }
    // Cases 0x6f `FADE OUT` and 0x70 `SET VISIBLE`: `+0x70d` set and the
    // character hidden - `FADE OUT` at a value of 1, through the fade;
    // `SET VISIBLE` at any other, at once.
    if (effect.type == kFadeOut || effect.type == kSetVisible) {
        const bool hide = (effect.type == kFadeOut) == (effect.min_value() == 1.0f);
        std::vector<tl::core::ScriptEvent> events;
        machine->set_unit_hidden(node, hide, effect.type == kSetVisible, events);
        if (NodeMap* nodes = nodes_of(machine); nodes != nullptr && !events.empty()) {
            apply_script_events(events, *nodes);
        }
        TL_LOG_INFO("skill event: %s %s", hide ? "hides" : "shows", unit.c_str());
        return;
    }
    // Cases 0x39 `TRANSFORM` and 0x49 `TRANSFORM PERMANENT`:
    // `transformCharacter(NAME, value)`, -999 for the permanent
    // one.
    if (effect.type == kTransform || effect.type == kTransformPermanent) {
        index_unit(effect.name);
        std::vector<tl::core::ScriptEvent> events;
        const float duration = effect.type == kTransform ? value : -999.0f;
        if (machine->transform_unit(node, effect.name, duration, quest_log.state(), events)) {
            TL_LOG_INFO("skill event: %s becomes %s for %.1fs", unit.c_str(), effect.name.c_str(),
                        static_cast<double>(duration));
        }
        if (NodeMap* nodes = nodes_of(machine); nodes != nullptr && !events.empty()) {
            apply_script_events(events, *nodes);
        }
        return;
    }
    // Case 0x7f `STOP SKILL`: a `v(0, 100)` at or under the value stops every
    // copy of `NAME`.
    if (effect.type == tl::core::kEffectTypeStopSkill) {
        const int roll = tl::core::volatile_random().integer_between(0, 100);
        if (static_cast<float>(roll) <= value)
            stop_unit_skills_by_name(*machine, node, effect.name);
        return;
    }
    // Case 0x8e `SWAP POS WITH PET`: a pet picked with `v(0, count - 1)`; the two
    // trade orientations and places, both read before either moves.
    if (effect.type == tl::core::kEffectTypeSwapPosWithPet) {
        std::vector<std::pair<tl::core::LevelScript*, int64_t>> pets;
        for (const MachineView& view : machines()) {
            for (int64_t pet : view.script->pets_of(machine, node))
                pets.emplace_back(view.script, pet);
        }
        if (pets.empty()) return;
        const int pick =
            tl::core::volatile_random().integer_between(0, static_cast<int>(pets.size()) - 1);
        if (pick < 0 || static_cast<size_t>(pick) >= pets.size()) return;
        tl::core::LevelScript* pet_machine = pets[static_cast<size_t>(pick)].first;
        const int64_t pet = pets[static_cast<size_t>(pick)].second;
        tl::core::Vec3 mine;
        tl::core::Vec3 theirs;
        float my_yaw = 0.0f;
        float their_yaw = 0.0f;
        if (!machine->unit_position(node, mine) || !pet_machine->unit_position(pet, theirs)) return;
        (void)machine->unit_facing(node, my_yaw);
        (void)pet_machine->unit_facing(pet, their_yaw);
        machine->place_unit(node, theirs, their_yaw);
        pet_machine->place_unit(pet, mine, my_yaw);
        TL_LOG_INFO("skill event: %s trades places with its pet %s", unit.c_str(),
                    pet_machine->unit_of(pet).c_str());
        return;
    }
    const tl::core::EffectTypeInfo* info = effect_types.at(effect.type);
    TL_LOG_WARN("effects: instant '%s' on %s is not carried",
                info != nullptr ? info->name.c_str() : "?", unit.c_str());
}

void World::update_unit_effects(float dt) {
    if (!(dt > 0.0f)) return;
    load_recharge_rates();
    for (const MachineView& view : machines()) {
        tl::core::LevelScript* machine = view.script;
        for (int64_t node : machine->units_with_effects()) {
            tl::core::EffectManager* effects = machine->unit_effects(node);
            if (effects == nullptr || machine->unit_is_dead(node)) continue;
            // In town (0x2a) `CCharacter::update`
            // runs nothing but the town
            // clock.
            if (machine->unit_ai_state(node) == tl::core::ai_state::kInTown) continue;
            update_unit_effects_of(view, node, dt);
        }
    }
}

// `CCharacter::update`: `updateEffects`, the manager's
// `calculateEffectValues` and `deleteDeadEffects`, then `updateHP`.
void World::update_unit_effects_of(const MachineView& view, int64_t node, float dt) {
    tl::core::LevelScript* machine = view.script;
    tl::core::EffectManager* effects = machine->unit_effects(node);
    if (effects == nullptr) return;
    load_recharge_rates();
    const std::string unit = machine->unit_of(node);
    std::vector<tl::core::ScriptEvent> events;
    const bool expired_summon = unit_update_effects(machine, node, dt, events);
    effects->update_affixes(dt);
    effects->calculate_effect_values();
    effects->delete_dead_effects();
    if (NodeMap* nodes = view.nodes; nodes != nullptr && !events.empty()) {
        apply_script_events(events, *nodes);
    }
    // `SUMMON DURATION` run out: `die` (vtable +0x330).
    if (expired_summon) {
        TL_LOG_INFO("effects: %s's summon duration is over", unit.c_str());
        kill_unit(node, unit, /*fall=*/true, tl::core::LevelScript::kNoAttacker);
        return;
    }
    update_unit_hp_of(view, node, effects, unit, dt, events);
}

// `CCharacter::updateEffects` on a placed character: effects
// age, then per type. True when a `SUMMON DURATION` ran out.
bool World::unit_update_effects(tl::core::LevelScript* machine, int64_t node, float dt,
                                std::vector<tl::core::ScriptEvent>& events) {
    tl::core::EffectManager* effects = machine->unit_effects(node);
    if (effects == nullptr) return false;
    const std::string unit = machine->unit_of(node);
    effects->update_effects(dt);
    tl::core::Vec3 feet;
    if (machine->unit_position(node, feet)) {
        play_effect_fx(*effects, ogre::Vector3(feet.x, feet.y + machine->unit_radius(node), feet.z),
                       unit);
    }
    bool stunned = false;
    bool expired_summon = false;
    for (tl::core::LiveEffect* live : effects->live_effects_of_type(kSummonDuration)) {
        if (live->expired()) expired_summon = true;
    }
    for (tl::core::LiveEffect* live : effects->live_effects_of_type(kStun)) {
        if (!live->expired()) stunned = true;
    }
    machine->knock_back_unit(
        node,
        knock_back_push(*effects, effects->effect_value(kPercentKnockBackResistance), dt, unit));
    convert_held_statuses(machine->unit_ai(node), effects, unit);
    // The stun's state (`set_unit_stunned`) and its `STUNNED` unit
    // theme: `addUnitTheme` with the state, `removeUnitTheme`
    // once no stun is live.
    const bool stun_changed = machine->set_unit_stunned(node, stunned, quest_log.state(), events);
    if (SceneUnit* drawn = scene_unit_of(machine, node)) {
        load_unit_themes();
        if (stunned && stun_changed) {
            drawn->themes.themes.add_unit_theme("STUNNED", &unit_themes);
        } else if (!stunned && drawn->themes.themes.has_unit_theme("STUNNED", &unit_themes)) {
            drawn->themes.themes.remove_unit_theme("STUNNED", &unit_themes);
        }
    }
    return expired_summon;
}
// `CCharacter::applyDamageEffects` past its roll.
void World::apply_damage_status(tl::core::LevelScript* machine, int64_t node, float amount,
                                const std::array<int, tl::core::kDamageTypeCount>& by_type,
                                int parts, int first_type, bool forced, int attacker_level,
                                const tl::core::EffectSource& attacker) {
    std::array<int, tl::core::kDamageTypeCount> own{};  // the player's `+0x620` is all 0
    if (machine != nullptr && !machine->unit_raw_defense(node, own)) return;
    const tl::core::DamageStatus status =
        tl::core::roll_damage_status(amount, by_type, parts, first_type, own, forced,
                                     machine == nullptr, tl::core::volatile_random());
    if (status.type < 0) return;
    tl::core::EffectManager* manager =
        machine != nullptr ? unit_effect_manager(machine, node) : &player_effects;
    if (manager == nullptr) return;
    const std::string who = machine != nullptr ? machine->unit_of(node) : "the character";
    // `getAffix`, else `addAffix(name, level, attacker, 1.0)`: the attacker owns
    // the affix's effects, and a failed add returns before the duration is drawn.
    const tl::core::EffectSourceScope owned(manager, attacker);
    if (!manager->has_affix(status.affix) &&
        !manager->add_affix_by_name(status.affix, static_cast<uint32_t>(attacker_level), &affixes,
                                    1.0f)) {
        return;
    }
    const float duration =
        tl::core::volatile_random().between(status.duration_min, status.duration_max);
    manager->set_affix_duration(status.affix, duration);
    TL_LOG_INFO("effects: %s takes %s for %.2fs", who.c_str(), status.affix,
                static_cast<double>(duration));
}

// `CCharacter::applyDamage`, the reflection at its end;
// the flags test (`& 0x300`) is skipped, as no caller passes those bits. The
// reflected hit is not struck, so it is never reflected back.
void World::reflect_damage(const tl::core::CharacterEffects& victim, int64_t victim_node,
                           const std::string& victim_name, float amount, int64_t attacker) {
    if (!victim || attacker == tl::core::LevelScript::kNoAttacker) return;
    // The attacker dying or dead (`+0x278` 5 or 6) is left alone.
    tl::core::LevelScript* by = nullptr;
    tl::core::Defender defender;
    if (attacker == 0) {
        if (player_health.dead || player_death.dying) return;
        defender = player_defender();
    } else {
        by = script_owning(attacker);
        if (by == nullptr || by->unit_is_dead(attacker)) return;
        const int32_t state = by->unit_ai(attacker);
        if (state == 5 || state == 6 || !by->unit_defender(attacker, defender)) return;
        defender.on_absorbed = [this, by, attacker] { show_unit_absorbed(by, attacker); };
    }
    const float flat = victim.effect_value(0x30, 7);
    if (flat == 0.0f && victim.effect_value(0x20, 7) == 0.0f) return;
    const float share = victim.effect_value(0x20, 7) * 0.01f;
    // `modifyDamage((int)(amount * share), 0, (int)amount, share)` on the attacker.
    const int part =
        tl::core::modify_damage(defender, static_cast<int>(share * amount), 0,
                                static_cast<int>(amount), share, tl::core::volatile_random());
    const float total = victim.effect_value(0x30, 7) + static_cast<float>(part);
    TL_LOG_INFO("combat: %s reflects %.1f of %.1f (%.0f%% + %.1f)", victim_name.c_str(),
                static_cast<double>(total), static_cast<double>(amount),
                static_cast<double>(share * 100.0f), static_cast<double>(flat));
    if (attacker == 0) {
        hurt_player(total, victim_name, victim_node);
    } else {
        hurt_unit(attacker, by->unit_of(attacker), total, false, victim_node, by);
    }
}

}  // namespace tl::core
