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
    // force less PERCENT KNOCK BACK RESISTANCE (0x35, held to 0..1) along its direction
    // onto the outside push (+0x240); the force falls to a thousandth per second.
    float share = resistance / 100.0f;
    if (!(share < 1.0f)) share = 1.0f;
    const float kept = 1.0f - (0.0f < share ? share : 0.0f);
    tl::core::Vec3 add;
    for (tl::core::LiveEffect* live : effects.live_effects_of_type(kKnockBackEffect)) {
        if (live->dead || live->expired()) continue;
        if (live->effect.activation != tl::core::EffectActivation::Dynamic) continue;
        const float force = live->value * kept;
        if (live->elapsed <= dt) {
            TL_LOG_INFO(
                "effects: %s is knocked back, force %.2f along (%.2f, %.2f), resistance "
                "kept %.2f",
                who.c_str(), static_cast<double>(live->value),
                static_cast<double>(live->effect.direction[0]),
                static_cast<double>(live->effect.direction[2]), static_cast<double>(kept));
        }
        add.x = live->effect.direction[0] * force + add.x;
        add.y = live->effect.direction[1] * force + add.y;
        add.z = live->effect.direction[2] * force + add.z;
        // `+0xc4 = powf(0.001, dt) * value()`, then `calculateBaseValue`:
        // with no graph the base value is that number.
        live->effect.values[0] = std::pow(0.001f, dt) * live->value;
        live->value = live->effect.values[0];
    }
    return add;
}

bool World::unit_effect_on_unit(tl::core::LevelScript* machine, int64_t node,
                                tl::core::Effect& effect) {
    // CCharacter::applyEffectOnUnit, the player as source;
    // true copies it. INVENTED:
    // isEffectValidForUnit (vtable +0x238) passes every effect on a living unit.
    const std::string unit = machine->unit_of(node);
    tl::core::EffectManager* own = machine->unit_effects(node);
    const auto resist = [own](int type) { return own != nullptr ? own->effect_value(type) : 0.0f; };
    // `CAffix::setDirection`: the direction the skill event applied
    // its affix along, `UNIT_Z` when it had none.
    if (effect_direction_set) {
        effect.direction = effect_direction;
    } else {
        effect.direction = {0.0f, 0.0f, 1.0f};
    }
    const auto roll = [] {
        return static_cast<float>(tl::core::volatile_random().integer_between(0, 100));
    };
    const float value =
        tl::core::effect_base_value(effect, tl::core::EffectCalculate::Roll, stat_graphs,
                                    skill_effects.owner, &tl::core::volatile_random());
    const int32_t ai = machine->unit_ai(node);
    const bool dying = ai == tl::core::ai_state::kDying || ai == tl::core::ai_state::kDead;
    std::vector<tl::core::ScriptEvent> events;
    const auto flush = [this, machine, &events] {
        if (events.empty()) return;
        if (NodeMap* nodes = nodes_of(machine)) apply_script_events(events, *nodes);
        events.clear();
    };
    const auto react = [&] { machine->provoke_unit(node, quest_log.state(), events); };

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
    bool after = true;  // the tail's provocation test
    switch (effect.type) {
        case tl::core::kEffectTypeHpRecharge:
        case tl::core::kEffectTypeHpRechargePlayer: copy = true; break;
        case kInterruptChance: {
            // INVENTED: `+0x772` (no interrupt at all) and `+0x702` (a
            // champion's third) are not read.
            if (static_cast<float>(tl::core::volatile_random().integer_between(1, 100)) <= value) {
                if (resist(kInterruptResistance) < roll()) {
                    machine->interrupt_unit(node, quest_log.state(), events);
                }
                react();
            }
            break;
        }
        case kVelocityMult:
            // INVENTED: `+0x24c..+0x254`, this frame's velocity, is zeroed here;
            // the machine sets it again on its next step, so nothing
            // is cleared.
            copy = true;
            break;
        case kStun:
            if (!dying && roll() <= value) {
                if (resist(kStunResistance) < roll() &&
                    resist(kPercentKnockBackResistance) < roll()) {
                    machine->interrupt_unit(node, quest_log.state(), events);
                    copy = true;
                }
            }
            react();
            break;
        case kFleeEffect: {
            // The port's `apply_flee_effect` is this branch (its own rolls,
            // `FLEE RESIST`, the turn and the run 3 to 5 away from the source).
            tl::core::Vec3 from = player.position();
            const bool turned = machine->apply_flee_effect(node, value, effect.duration, &from,
                                                           quest_log.state(), events);
            if (turned) {
                TL_LOG_INFO("skill event: %s flees", unit.c_str());
                tl::core::Vec3 where;
                if (machine->unit_position(node, where)) {
                    play_sound(stats_for_unit(unit).sounds[tl::core::SoundSlot::Flee],
                               ogre::Vector3(where.x, where.y, where.z));
                }
                copy = true;
            }
            (void)kFleeResist;
            break;
        }
        case kTurnAlignment:
            // INVENTED: a charmed unit's change of side while it holds the
            // effect is not modelled; the effect is only held.
            if (!dying && !(value < roll()) && resist(kCharmResistance) < roll()) copy = true;
            break;
        case kFreeze:
            if (!dying && roll() <= value) {
                machine->interrupt_unit(node, quest_log.state(), events);
                copy = true;
            }
            react();
            break;
        case kBurn:
        case kPoison:
        case kShock:
            // INVENTED: the second chance, the unit's level curve (`+0x6d0`)
            // against its fire, poison or electric defence, is not rolled.
            if (!dying && roll() <= value) {
                machine->interrupt_unit(node, quest_log.state(), events);
                copy = true;
            }
            react();
            break;
        case kKill: kill_unit(node, unit, /*fall=*/true, 0); return false;
        case kSilence:
            if (!dying && !(value < roll()) && resist(kSilenceResistance) < roll()) copy = true;
            break;
        default:
            // `copyEffect` for an effect activated passively, dynamically or by
            // transfer - every activation there is.
            copy = effect.activation == tl::core::EffectActivation::Passive ||
                   effect.activation == tl::core::EffectActivation::Dynamic ||
                   effect.activation == tl::core::EffectActivation::Transfer;
            break;
    }
    // The player applied it: an enemy of the
    // unit's that leaves an idle unit provoked - its friends near it first
    // (`CLevel::reactToAreaDamage`), then the unit
    // (`reactToDamage`) - unless the effect is `PERCENT BLIND`.
    if (after && effect.type != kPercentBlind && unit_hostile(node, stats_for_unit(unit)) &&
        (ai == tl::core::ai_state::kIdle || ai == tl::core::ai_state::kNone)) {
        tl::core::Vec3 at;
        if (machine->unit_position(node, at)) {
            machine->react_to_area_damage(0, nullptr, machine, node, at, machine->unit_radius(node),
                                          quest_log.state(), events);
        }
        react();
    }
    flush();
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
        if (machine->unit_is_dead(node)) return;
        // Case 0x34. A value that is not damage is `modifyHP`.
        // `instant_damage`'s own test, before the defender is
        // looked up.
        const float v = 0.0f < value ? -value : value;
        if (0.0f <= v) return;
        tl::core::Defender defender;
        if (!machine->unit_defender(node, defender)) return;
        defender.on_absorbed = [this, machine, node] { show_unit_absorbed(machine, node); };
        // The effect's owner (`+0x48`): the unit whose skill event this is, or
        // the player.
        const bool by_unit = instant_caster_machine != nullptr;
        const tl::core::CharacterEffects attacker =
            by_unit ? tl::core::CharacterEffects(
                          instant_caster_machine->unit_effects(instant_caster_node))
                    : player_total();
        const int64_t attacker_node = by_unit ? instant_caster_node : 0;
        const int type = static_cast<int>(effect.damage_type);
        const tl::core::InstantDamage hit =
            tl::core::instant_damage(value, type, effect.values[2], effect.values[3], defender,
                                     &attacker, tl::core::volatile_random());
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
        blow.critical = hit.critical;
        hurt_unit(node, unit, static_cast<float>(hit.damage), hit.critical, attacker_node, machine,
                  &blow, /*struck=*/true);
        // `applyDamageEffects(this, this, damage, {type, damage}, 1, false)`:
        // the level asked is the target's own.
        if (!machine->unit_is_dead(node)) {
            std::array<int, tl::core::kDamageTypeCount> by_type{};
            if (0 <= type && type < tl::core::kDamageTypeCount) {
                by_type[static_cast<size_t>(type)] = hit.damage;
            }
            int level = 1;
            machine->unit_level(node, level);
            // The unit is its own attacker (`applyDamageEffects(this, this, ...)`).
            apply_damage_status(machine, node, static_cast<float>(hit.damage), by_type, 1, type,
                                false, level, tl::core::EffectSource{true, machine, node});
        }
        return;
    }
    // Cases 7 HP RECHARGE and 0x7c HP RECHARGE PLAYER: a value >= 0 is
    // modifyHP(value), on a body too (only a whole-maxHP heal fills it, and clears +0x454:
    // no experience on its next death). isEffectValidForUnit
    // refuses both on a non-DAMAGEABLE unit. INVENTED: a negative value is not
    // carried; it warns below.
    if (effect.type == tl::core::kEffectTypeHpRecharge ||
        effect.type == tl::core::kEffectTypeHpRechargePlayer) {
        if (!stats_for_unit(unit).damageable) return;
        if (0.0f <= value) {
            if (machine->modify_unit_hp(node, value)) no_xp_units.insert(node);
            return;
        }
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
            const std::string unit = machine->unit_of(node);
            std::vector<tl::core::ScriptEvent> events;
            // `CCharacter::updateEffects`:
            // effects age, then per type.
            effects->update_effects(dt);
            tl::core::Vec3 feet;
            if (machine->unit_position(node, feet)) {
                play_effect_fx(*effects,
                               ogre::Vector3(feet.x, feet.y + machine->unit_radius(node), feet.z),
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
                node, knock_back_push(*effects, effects->effect_value(kPercentKnockBackResistance),
                                      dt, unit));
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
                for (tl::core::LiveEffect* live : effects->live_effects_of_type(conversion.first)) {
                    if (live->expired()) continue;
                    if (!effects->set_affix_duration(conversion.second, live->effect.duration)) {
                        effects->add_affix_by_name(conversion.second, live->effect.level, &affixes,
                                                   live->effect.duration);
                        TL_LOG_INFO("effects: %s takes %s for %.2fs", unit.c_str(),
                                    conversion.second, static_cast<double>(live->effect.duration));
                    }
                    if (live->effect.duration != tl::core::kDurationInstant &&
                        live->effect.duration != tl::core::kDurationAlways) {
                        live->elapsed = live->effect.duration;
                    }
                    live->dead = true;
                }
            }
            // The stun's state (`set_unit_stunned`) and its `STUNNED` unit
            // theme: `addUnitTheme` with the state, `removeUnitTheme`
            // once no stun is live.
            const bool stun_changed =
                machine->set_unit_stunned(node, stunned, quest_log.state(), events);
            if (SceneUnit* drawn = scene_unit_of(machine, node)) {
                load_unit_themes();
                if (stunned && stun_changed) {
                    drawn->themes.themes.add_unit_theme("STUNNED", &unit_themes);
                } else if (!stunned &&
                           drawn->themes.themes.has_unit_theme("STUNNED", &unit_themes)) {
                    drawn->themes.themes.remove_unit_theme("STUNNED", &unit_themes);
                }
            }
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
                continue;
            }
            // CCharacter::updateHP: base rate, then the
            // effects; an INVINCIBLE character with a master regenerates at
            // PET_HP_RECHARGE_RATE. The player's animal reads its bag as well.
            float hp = 0.0f;
            float max = 0.0f;
            if (!machine->unit_health(node, hp, max)) continue;
            const bool invincible_pet =
                machine->unit_is_player_pet(node) && stats_for_unit(unit).invincible_flag;
            const float change =
                machine == &script && node == pet_node_id
                    ? tl::core::hp_change(max, recharge_rates, pet_total(), dt, invincible_pet)
                    : tl::core::hp_change(max, recharge_rates, effects, dt, invincible_pet);
            // the effects' damage this frame goes onto the number
            // over the unit, whatever the regeneration gave back.
            const float effect_damage = (machine == &script && node == pet_node_id
                                             ? tl::core::effect_values_of_damage(pet_total())
                                             : tl::core::effect_values_of_damage(effects)) *
                                        dt;
            if (effect_damage < 0.0f) machine->add_unit_damage_shown(node, -effect_damage);
            if (change > 0.0f) {
                machine->heal_unit(node, change);
            } else if (change < 0.0f) {
                // `modifyHP` alone: no reaction.
                machine->set_defer_death_sample(true);
                const bool killed = machine->damage_unit(
                    node, -change, tl::core::LevelScript::kNoAttacker, quest_log.state(), events,
                    nullptr, nullptr, false, /*shown=*/false);
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
    }
}

}  // namespace tl::core
