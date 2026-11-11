// What a skill does when it starts: the `EVENT_START` group's affixes and
// effects onto a unit. See `skill_cast.h`.
#include "core/skill_cast.h"

#include "core/item_use.h"
#include "core/ogre_math.h"
#include "core/random.h"
#include "core/skill_animation.h"
#include "core/unit_themes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace tl::core {
namespace {

std::string upper(std::string text) {
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return text;
}

/// `gSKILL_EVENT_TYPE_NAMES`, the eleven groups in the order `CSkillProperty`'s
/// constructor allocates them (the same indices as `skill_stats.cpp`).
const char* const kEventGroupNames[] = {
    "EVENT_START",       "EVENT_END",       "EVENT_TRIGGER",     "EVENT_TRIGGER_TWO",
    "EVENT_UNITHIT",     "EVENT_UNITDIE",   "EVENT_MISSILEHIT",  "EVENT_MISSILEDIE",
    "EVENT_DIEBYEFFECT", "EVENT_CASTERDIE", "EVENT_UNIT_CREATE",
};
constexpr int kEventGroupCount = 11;
/// `gSKILL_EVENT_TYPE_CLONE_ALLOWED`: the `CAN_CLONE` default
/// `CSkillEvent::CSkillEvent` takes by group.
constexpr bool kEventGroupCloneAllowed[kEventGroupCount] = {
    false, false, false, true, true, true, true, true, false, true, false,
};

/// The `TARGET` test `CSkillEffectAndAffixes::applyAffixesAndEffects`
/// runs before each affix and effect group. With no caster or no target
/// character every arm falls through and applies, as `Any` does here.
bool passes_target_filter(SkillTargetFilter filter, const SkillCastContext& context) {
    if (context.no_caster_character) return true;
    switch (filter) {
        case SkillTargetFilter::Self: return context.target_is_caster;
        case SkillTargetFilter::Pet: return context.target_is_casters_pet;
        case SkillTargetFilter::Friend:
            // A friend, and explicitly not the caster itself.
            return !context.target_is_enemy && !context.target_is_caster;
        case SkillTargetFilter::Enemy:
            // The original compares the two characters' alignments and skips a
            // match, so "not an ally" rather than "hostile to me".
            return context.target_is_enemy;
        case SkillTargetFilter::Everybody: break;
    }
    return true;
}

/// `CAffix::effectHasDuration(type)`: any of the affix's
/// effects of that type with a duration above 0.
bool affix_has_timed_effect(const Affix& affix, int type) {
    for (const Effect& effect : affix.effects) {
        if (effect.duration > 0.0f && effect.type == type) return true;
    }
    return false;
}

/// The effects half of the same test: a held effect whose
/// duration is above 0 and whose type is one of the three.
void observe_for_effect(const SkillCastContext& context, const Effect& effect,
                        const std::string& skill_name) {
    if (!context.observe_target || !(effect.duration > 0.0f)) return;
    if (effect.type == kEffectTypeDamage || effect.type == kEffectTypeHpRecharge ||
        effect.type == kEffectTypeHpRechargePlayer) {
        context.observe_target(skill_name);
    }
}

}  // namespace

bool apply_affixes_and_effects(const SkillEffectAndAffixes& block, const SkillCastContext& context,
                               const std::string& skill_name, bool ignore_target_filter) {
    // `CSkillEffectAndAffixes::applyAffixesAndEffects`:
    // the affixes first, in the order the
    // block names them, then the inline effect groups.
    if (context.target == nullptr) return false;
    const EffectSourceScope owned(context.target, context.source);
    bool applied = false;

    for (const SkillEffectAndAffixes::AffixSpec& spec : block.affix_specs) {
        if (!ignore_target_filter && !passes_target_filter(spec.target, context)) continue;
        if (!context.affix) continue;
        const Affix* source = context.affix(spec.name);
        if (source == nullptr) continue;
        // The spec's `DURATION` reaches the affix only when the affix file did
        // not author one of its own (`CAffix +0xa8`); `addAffix` takes any
        // positive one it is handed.
        context.target->add_affix(*source, spec.level,
                                  source->authored_duration ? -1.0f : spec.duration, skill_name,
                                  context.owner_level);
        applied = true;
        // the skill watches a unit that now holds an affix with a
        // timed effect that can kill it (`CAffix::effectHasDuration`:
        // an effect of the type whose duration is above 0).
        if (context.observe_target &&
            (affix_has_timed_effect(*source, kEffectTypeDamage) ||
             affix_has_timed_effect(*source, kEffectTypeHpRecharge) ||
             affix_has_timed_effect(*source, kEffectTypeHpRechargePlayer))) {
            context.observe_target(skill_name);
        }
    }

    for (size_t g = 0; g < block.effect_groups.size(); ++g) {
        const SkillEffectAndAffixes::EffectGroup& group = block.effect_groups[g];
        if (!ignore_target_filter && !passes_target_filter(group.target, context)) continue;
        for (size_t k = 0; k < group.effects.size(); ++k) {
            const Effect& authored = group.effects[k];
            // runs while the effect's owner (`+0x40`) is null.
            if (context.ownerless_rolls != nullptr &&
                (context.ownerless_owned == nullptr ||
                 context.ownerless_owned->insert({g, k}).second)) {
                Effect discarded = authored;
                context.ownerless_rolls->roll(discarded);
            }
            // `setOwner(effect, caster, true)` before it goes on.
            Effect effect = authored;
            set_owner_level(effect, context.owner_level);
            // A unit with its own `applyEffectOnUnit` (a monster's manager)
            // takes every effect through it, instant ones included.
            if (context.target->has_unit_hook()) {
                // one the unit took (`applyEffectOnUnit`, vtable
                // +0x114 there) whose duration is above 0 makes the skill watch
                // it, as an affix's does above.
                if (context.target->offer_effect(effect, std::string(), skill_name)) {
                    observe_for_effect(context, effect, skill_name);
                }
                applied = true;
                continue;
            }
            // `applyEffectOnUnit` (vtable +0x240):
            // an `INSTANT` effect goes to `applyInstantEffect` and returns true
            // without being added.
            if (effect.duration == kDurationInstant) {
                if (context.instant_effect) context.instant_effect(effect);
                applied = true;
                continue;
            }
            context.target->add_effect(effect, std::string(), skill_name);
            observe_for_effect(context, effect, skill_name);
            applied = true;
        }
    }

    // `CSkillEffectAndAffixes::removeAffixesAndEffects`, run
    // by `CSkillEvent::applyEffects` straight after the apply
    // and ORed into its result: every name of both removal lists, with no `TARGET` filter.
    for (const std::string& name : block.remove_affix_names) {
        applied = context.target->delete_affix(name) || applied;
    }
    for (const std::string& name : block.remove_effect_names) {
        applied = context.target->remove_effect(name) || applied;
    }
    return applied;
}

float skill_event_duration_override(const tl::formats::AdmSection& event, float property_override) {
    // `CSkillEvent::CSkillEvent`: own `DURATIONOVERRIDEMS`
    // as a signed integer times 0.001f, then the larger of it and the
    // property's, the property's when unordered.
    const float own =
        static_cast<float>(static_cast<int>(event.integer_or("DURATIONOVERRIDEMS", 0))) * 0.001f;
    return own >= property_override ? own : property_override;
}

bool apply_event_effects(const tl::formats::AdmSection& event, const SkillCastContext& context,
                         const std::string& skill_name, const tl::formats::AdmSection* property,
                         SkillTargetFilter unauthored, float property_duration_override) {
    // `CSkillEvent::applyEffects`: the event's own block,
    // else the property's. INVENTED: its line-of-sight ray (`CLevel::rayCollision`, the
    // `+0x14a` flag) is not run; no player skill authors that flag.
    if (context.types == nullptr) return false;
    // The event's block exists when it has an `[EFFECT]`, `[EFFECTS]`,
    // `[AFFIXES]` or `[AFFIXESREMOVE]` child (`CSkillEvent +0x54` not null),
    // however empty it comes out.
    const bool authored = has_skill_effect_and_affixes(event);
    SkillEffectAndAffixes block =
        parse_skill_effect_and_affixes(event, *context.types, unauthored, context.unit_types);
    if (authored) {
        const float seconds = skill_event_duration_override(
            event, property != nullptr ? property_duration_override : 0.0f);
        if (0.0f < seconds) set_duration_override(block, seconds);
    } else if (property != nullptr) {
        block = parse_skill_effect_and_affixes(*property, *context.types, unauthored,
                                               context.unit_types);
        // `CSkillProperty::CSkillProperty`:
        // its own `DURATIONOVERRIDEMS` on its own block.
        if (0.0f < property_duration_override) {
            set_duration_override(block, property_duration_override);
        }
    }
    return apply_affixes_and_effects(block, context, skill_name);
}

// `CSkillEffectAndAffixes::isDamaging`.
bool event_is_damaging(const tl::formats::AdmSection& event, const SkillCastContext& context,
                       const tl::formats::AdmSection* property, SkillTargetFilter unauthored) {
    if (context.types == nullptr) return false;
    // The event's block exists when it has an `[EFFECT]`, `[EFFECTS]`,
    // `[AFFIXES]` or `[AFFIXESREMOVE]` child (`CSkillEvent +0x54` not null),
    // however empty it comes out.
    const bool authored = has_skill_effect_and_affixes(event);
    SkillEffectAndAffixes block =
        parse_skill_effect_and_affixes(event, *context.types, unauthored, context.unit_types);
    if (!authored && property != nullptr) {
        block = parse_skill_effect_and_affixes(*property, *context.types, unauthored,
                                               context.unit_types);
    }
    const auto damaging = [](int type) { return type == 0x34 || type == 7 || type == 0x7c; };
    // The affixes first (`CAffix::effectExists`), then the inline effects.
    for (const SkillEffectAndAffixes::AffixSpec& spec : block.affix_specs) {
        const Affix* affix = context.affix ? context.affix(spec.name) : nullptr;
        if (affix == nullptr) continue;
        for (const Effect& effect : affix->effects) {
            if (damaging(effect.type)) return true;
        }
    }
    for (const SkillEffectAndAffixes::EffectGroup& group : block.effect_groups) {
        for (const Effect& effect : group.effects) {
            if (damaging(effect.type)) return true;
        }
    }
    return false;
}

// `CSkillEvent::effectsTargetEnemy(CBaseUnit*)`.
bool event_effects_target_enemy(const tl::formats::AdmSection& event,
                                const SkillCastContext& context,
                                const tl::formats::AdmSection* property,
                                SkillTargetFilter unauthored) {
    if (context.types == nullptr) return false;
    // The event's own block, else the level property's; with neither, false.
    // The event's block exists when it has an `[EFFECT]`, `[EFFECTS]`,
    // `[AFFIXES]` or `[AFFIXESREMOVE]` child (`CSkillEvent +0x54` not null),
    // however empty it comes out.
    const bool authored = has_skill_effect_and_affixes(event);
    SkillEffectAndAffixes block =
        parse_skill_effect_and_affixes(event, *context.types, unauthored, context.unit_types);
    if (!authored && property != nullptr) {
        block = parse_skill_effect_and_affixes(*property, *context.types, unauthored,
                                               context.unit_types);
    }
    // `CSkillEffectAndAffixes::effectsTargetEnemy`: the first spec
    // the unit `ISA`s answers - true for `ENEMY`, else the walk goes on.
    for (const SkillEffectAndAffixes::AffixSpec& spec : block.affix_specs) {
        if (spec.target == SkillTargetFilter::Enemy) return true;
    }
    return false;
}

namespace {

/// The `TARGET` arms, which an effect group and an affix
/// take almost alike: only `SELF` differs, an affix also refusing a missing
/// user. 3 (`EVERYBODY`) and 4 (`FRIEND`) have no arm.
bool target_arm_passes(SkillTargetFilter target, const SkillApplyFacts& f, bool affix) {
    switch (target) {
        case SkillTargetFilter::Enemy:
            if (!f.has_user) return false;
            return !(f.unit_is_character && f.same_alignment);
        case SkillTargetFilter::Self:
            if (affix && !f.has_user) return false;
            return !(f.unit_is_character && !f.unit_is_user);
        case SkillTargetFilter::Pet: return f.unit_is_character && f.has_user && f.master_is_user;
        case SkillTargetFilter::Everybody:
        case SkillTargetFilter::Friend: break;
    }
    return true;
}

}  // namespace

bool skill_block_can_apply(const SkillEffectAndAffixes& block, const SkillApplyFacts& facts) {
    // the effect groups (`+0x1c`) first.
    for (const SkillEffectAndAffixes::EffectGroup& group : block.effect_groups) {
        if (!facts.isa || !facts.isa(group.target_type)) return false;
        if (!target_arm_passes(group.target, facts, /*affix=*/false)) return false;
        for (const Effect& effect : group.effects) {
            if (!facts.effect_valid || !facts.effect_valid(effect)) return false;
        }
    }
    // Then the affixes (`+0xc`), whose effects are not asked about.
    for (const SkillEffectAndAffixes::AffixSpec& spec : block.affix_specs) {
        if (!facts.isa || !facts.isa(spec.target_type)) return false;
        if (!target_arm_passes(spec.target, facts, /*affix=*/true)) return false;
    }
    return true;
}

bool skill_property_can_apply(const tl::formats::AdmSection& property, const EffectTypeTable& types,
                              SkillTargetFilter unauthored, const SkillApplyFacts& facts,
                              const tl::formats::AdmSection* events,
                              const UnitTypeHierarchy* unit_types) {
    // the level's own block (`+0x14`; Linux `+0x20`) answers
    // alone.
    if (has_skill_effect_and_affixes(property)) {
        return skill_block_can_apply(
            parse_skill_effect_and_affixes(property, types, unauthored, unit_types), facts);
    }
    // Else the event groups (`+0x34`, eleven lists) in order, each event's
    // block (`+0x54`; Linux `+0xa0`).
    for (int group = 0; group < kEventGroupCount; ++group) {
        for (const tl::formats::AdmSection& child :
             (events != nullptr ? *events : property).children) {
            if (upper(child.name) != kEventGroupNames[group]) continue;
            if (!has_skill_effect_and_affixes(child)) continue;
            const SkillEffectAndAffixes block =
                parse_skill_effect_and_affixes(child, types, unauthored, unit_types);
            if (!skill_block_can_apply(block, facts)) return false;
        }
    }
    return true;
}

// `CSkill::getTargetIsValid(CCharacter*, CCharacter*, const Vector3*, bool)`.
//
bool skill_target_is_valid(const SkillTargetQuery& q) {
    // A `SELF` property tests the owner, anything else the candidate.
    const bool self = q.has_property && q.target_type == 3;
    const SkillTargetFacts& t = self ? q.owner : q.candidate;
    // The 4th argument or +0x6e: only the dead; otherwise only the living.
    if (q.dead_wanted ? t.alive : !t.alive) return false;
    if (q.has_property && !t.unit_type_matches) return false;
    if (!t.specific_unit) return false;
    if (q.has_property) {
        // 10 `SELFANDPETS`: the owner, its pet, or its master; 9 `PETS`: its
        // pet. Anything else falls through to the sides.
        if (q.target_type == 10 && (t.is_owner || t.master_is_owner || t.is_owners_master)) {
            return true;
        }
        if (q.target_type == 9 && t.master_is_owner) return true;
    }
    if (q.owner_alignment == 0) return false;
    if (q.has_property && q.target_type == 4) return true;  // `EVERYBODY`
    if (q.owner_alignment == 4) return true;
    // `TARGET_ALIGNMENT`: 2 `EVIL` an enemy, 1 `GOOD` anyone else, else none.
    if (q.target_alignment == 2) return t.enemy;
    if (q.target_alignment == 1) return !t.enemy;
    return false;
}

// PORT: asset parsing, a skill's `BONE` word as its index.
int skill_bone_attachment(const std::string& name) {
    static const char* const kNames[] = {"",         "CENTER",        "HEAD",         "RIGHTHAND",
                                         "LEFTHAND", "RIGHTSHOULDER", "LEFTSHOULDER", "POSITION"};
    const std::string wanted = upper(name);
    for (int i = 0; i < 8; ++i) {
        if (wanted == kNames[i]) return i;
    }
    return kSkillBoneNone;
}

namespace {

// PORT: one block's draws, as `CSkillEffectAndAffixes::CSkillEffectAndAffixes`
// makes them.
void roll_block(const tl::formats::AdmSection& section, const SkillConstruction& with) {
    if (!has_skill_effect_and_affixes(section)) return;
    SkillEffectAndAffixes block = parse_skill_effect_and_affixes(
        section, *with.types, SkillTargetFilter::Enemy, with.unit_types);
    // `CEffect::CEffect(group, 0)`, a roll each.
    for (SkillEffectAndAffixes::EffectGroup& group : block.effect_groups) {
        for (Effect& effect : group.effects) with.rolls.roll(effect);
    }
    // `createAffixesForDisplay`, each affix through
    // `getNewAffixByName(name, level)`, then `CAffix::setSkillOwner(skill)`:
    // no unit yet, so no `CAffix::setOwner`; each ownerless effect gets
    // `CEffect::setOwner(null, true)`, a roll.
    if (with.affixes == nullptr) return;
    for (const SkillEffectAndAffixes::AffixSpec& spec : block.affix_specs) {
        std::optional<Affix> affix =
            new_affix_by_name(*with.affixes, spec.name, spec.level, with.rolls);
        if (!affix) continue;
        for (Effect& effect : affix->effects) with.rolls.roll(effect);
    }
}

// PORT: one property's draws (`CSkillProperty::CSkillProperty`).
void roll_property(const SkillProperty& property, const SkillConstruction& with) {
    if (property.section == nullptr) return;
    // the eleven groups' events gathered in order; none, and the
    // parent's lists are taken as they are, nothing made.
    if (property.events == property.section) {
        for (const char* group : kEventGroupNames) {
            for (const tl::formats::AdmSection& child : property.section->children) {
                if (upper(child.name) != group) continue;
                // the event, then `CLONECOUNT` (its
                // word) more of it, each made whole from the same group.
                int32_t clones = 0;
                if (const tl::formats::AdmRecord* r = child.find("CLONECOUNT"))
                    clones = static_cast<int32_t>(r->word_as_integer());
                for (int32_t k = 0; k <= clones; ++k) {
                    // `CSkillEvent::CSkillEvent`: a `FILE`
                    // that is not empty is loaded into the event's own
                    // `CLayout`, its objects made.
                    const std::string file = child.string_or("FILE", std::string());
                    if (!file.empty() && with.load_event_file) with.load_event_file(file);
                    if (!file.empty() && with.game_rng != nullptr && with.layout_adds_sample &&
                        with.layout_adds_sample(file)) {
                        with.game_rng->set_seed(
                            static_cast<int32_t>(static_cast<uint32_t>(with.game_rng->state())));
                    }
                    roll_block(child, with);
                }
            }
        }
    }
    // the property's own block.
    roll_block(*property.section, with);
}

}  // namespace

void roll_skill_construction(const SkillDef& def, const SkillConstruction& with) {
    // `CSkill::CSkill`: the root, then each
    // `[LEVELn]`.
    if (with.types == nullptr || !with.rolls.on()) return;
    roll_property(def.base, with);
    for (const SkillProperty& level : def.levels) roll_property(level, with);
}

SkillEventSpec read_skill_event(const tl::formats::AdmSection& event) {
    // `CSkillEvent::CSkillEvent`.
    SkillEventSpec spec;
    spec.file = event.string_or("FILE", std::string());
    spec.apply_effects = event.bool_or("APPLYEFFECTS", false);
    spec.target_ahead = event.float_or("TARGET_AHEAD_OF_UNIT", 0.0f);
    spec.cast_from_target = event.bool_or("CASTFROMTARGET", false);
    // The layout's keys are read only for an event with a `FILE`:
    // without one every key keeps the constructor's value (`MAKEPET` and `ATTACH_LINE_EMITTER`
    // 1, the rest 0). With one, `MAKEPET` reads over 1 and the rest over 0.
    if (!spec.file.empty()) {
        spec.make_pet = event.bool_or("MAKEPET", true);
        spec.attach_line_emitter = event.bool_or("ATTACH_LINE_EMITTER", false);
        spec.attach_to_max_distance = event.bool_or("ATTACH_LINE_EMITTER_TO_MAX_DISTANCE", false);
        spec.line_emitter_collides = event.bool_or("LINE_EMITTER_COLLIDES", false);
        spec.damage_requires_los = event.bool_or("DAMAGE_REQUIRES_LOS", false);
        spec.match_pet_duration = event.bool_or("MATCH_PET_DURATION", false);
        spec.attaches = event.bool_or("ATTACHES", false);
        spec.attach_to_target = event.bool_or("ATTACHTOTARGET", false);
        spec.attach_owner = event.bool_or("ATTACHOWNER", false);
        spec.attach_ignores_units = event.bool_or("ATTACHIGNORESUNITS", false);
        spec.attach_ignores_obstructions = event.bool_or("ATTACHIGNORESOBSTRUCTIONS", false);
        spec.scale_attach_distance = event.bool_or("SCALEATTACHDISTANCE", false);
        spec.place_on_target = event.bool_or("PLACEONTARGET", false);
        spec.synch_pets_to_owner = event.bool_or("SYNCH_PETS_TO_OWNER", false);
        spec.fire_from_bone = skill_bone_attachment(event.string_or("FIREFROMBONE", ""));
        spec.fire_at_bone = skill_bone_attachment(event.string_or("FIREATBONE", ""));
    }
    for (const tl::formats::AdmSection& part : event.children) {
        if (upper(part.name) != "EXECUTE_SKILL") continue;
        spec.execute_skills.push_back(part.string_or("SKILL", ""));
    }
    spec.execute_skill_count = static_cast<int>(event.integer_or("EXECUTE_SKILL_COUNT", 0));
    // each percent **times 0.01f** (the Linux
    // build divided by 100), kept only when above 0 - a NaN is 0 too.
    const float weapon = event.float_or("WEAPONDAMAGEPCT", 0.0f) * 0.01f;
    spec.weapon_damage = 0.0f < weapon ? weapon : 0.0f;
    const float soak = event.float_or("SOAKSCALEPCT", 100.0f) * 0.01f;
    spec.soak_scale = 0.0f < soak ? soak : 0.0f;
    spec.target_corpses = event.bool_or("TARGET_CORPSES", false);
    spec.max_units_hit = static_cast<int>(event.integer_or("MAX_UNITS_HIT", -1));
    return spec;
}

namespace {

/// Both `start_skill` and `stop_skill` are one event group applied, so they
/// differ only in which.
bool fire_event_group(const KnownSkill& skill, uint32_t level, int group,
                      const SkillCastContext& context) {
    if (skill.def == nullptr || context.target == nullptr) return false;
    const SkillProperty* property = skill.property(level);
    if (property == nullptr || property->section == nullptr) return false;

    // An unauthored `TARGET`: the blocks were made with the skill. INVENTED: a port clone takes
    // the same; the console's clone past the `CLONECOUNT` pool would take
    // `default_effect_target(property->target_type, TARGET_ALIGNMENT)`.
    const SkillTargetFilter unauthored = kSkillBlockTargetDefault;
    bool applied = false;
    // The property's events: its own, or its parent's when it has none.
    for (const tl::formats::AdmSection& child : property->event_section().children) {
        if (upper(child.name) != kEventGroupNames[group]) continue;
        // `CSkill::triggerEvent`: a block already started this
        // run is passed over unless it may clone (`CAN_CLONE`), and then a clone of it
        // (`CSkillProperty::cloneEventForSkill`) starts instead.
        bool clone = false;
        if (context.started_events != nullptr) {
            std::vector<const tl::formats::AdmSection*>& started = *context.started_events;
            if (std::find(started.begin(), started.end(), &child) != started.end()) {
                if (!child.bool_or("CAN_CLONE", kEventGroupCloneAllowed[group])) continue;
                clone = true;
            } else {
                started.push_back(&child);
            }
        }
        // `startEvent`'s `reset` on the block itself; a clone is a new event
        // with no missiles yet.
        if (!clone && context.reset_event) {
            context.reset_event(skill.entry.name, context.event_target.run, child);
        }
        // The event's own property (`+0x4c`): an inherited event's is the
        // parent's, whose section `events` is.
        applied = apply_event_effects(child, context, skill.entry.name, &property->event_section(),
                                      unauthored, property->events_duration_override) ||
                  applied;
        // `CSkillEvent::startEvent` applies
        // the effects to the caster first, then places and starts the layout.
        if (context.on_event) context.on_event(skill, level, group, child, context.event_target);
    }
    return applied;
}

}  // namespace

bool start_skill(const KnownSkill& skill, uint32_t level, const SkillCastContext& context) {
    // `CSkill::startSkill` ends with `triggerEvent(0, ...)`,
    // `EVENT_START`. Its animation, `rollSkillChance` and cooldown
    // (`CSkillProperty::getCoolDown`) are elsewhere.
    return fire_event_group(skill, level, kSkillEventStart, context);
}

bool stop_skill(const KnownSkill& skill, uint32_t level, const SkillCastContext& context) {
    // `CSkill::stopSkill` fires group 1, `EVENT_END`: for a
    // passive an `[AFFIXESREMOVE]` naming the start's affix (`EffectManager::delete_affix`).
    return fire_event_group(skill, level, kSkillEventEnd, context);
}

int start_passive_skills(const SkillBook& book, const SkillCastContext& context) {
    // `CSkillManager::startPassiveSkills`: every `PASSIVE`
    // skill at an *effective* level above zero, through `CSkill::weaponRequirementsMet`
    // when there is a caster. Every running passive is
    // stopped first, which keeps this idempotent.
    for (const KnownSkill& skill : book.skills()) {
        if (!skill.passive()) continue;
        // An effective level of zero is still stopped, at level 1's block, which names the affix.
        stop_skill(skill, skill.effective == 0 ? 1 : skill.effective, context);
    }

    int started = 0;
    for (const KnownSkill& skill : book.skills()) {
        if (!skill.passive()) continue;
        if (skill.effective == 0) continue;
        if (context.caster != nullptr) {
            const SkillProperty* property = skill.property(skill.effective);
            if (property != nullptr && !weapon_requirements_met(*property, *context.caster)) {
                continue;
            }
        }
        const bool ok = start_skill(skill, skill.effective, context);
        if (context.on_skill) context.on_skill(skill.entry.name, skill.effective, ok);
        if (ok) ++started;
    }
    return started;
}

// ---------------------------------------------------------------------------
// The gate, the cooldown and the cast.
// ---------------------------------------------------------------------------

// PORT: a diagnostic name.
const char* skill_cast_result_name(SkillCastResult result) {
    switch (result) {
        case SkillCastResult::Cast: return "cast";
        case SkillCastResult::NoSuchSkill: return "no such skill";
        case SkillCastResult::NotLearned: return "not learned";
        case SkillCastResult::NotEnoughMana: return "not enough mana";
        case SkillCastResult::Cooling: return "cooling";
        case SkillCastResult::RequirementsNotMet: return "requirements not met";
        case SkillCastResult::ChanceFailed: return "chance roll failed";
        case SkillCastResult::AlreadyRunning: return "already running";
    }
    return "?";
}

namespace {
// `m_gDisableSkillFromBeingCasted`.
bool g_disable_skill_from_being_casted = false;
}  // namespace

// `CSkillManager::globallyDisableSkills(bool)`.
void globally_disable_skills(bool disable) { g_disable_skill_from_being_casted = disable; }

// `CSkillManager::globallyDisableSkills(bool)`'s flag, read.
bool skills_globally_disabled() { return g_disable_skill_from_being_casted; }

float skill_cooldown(const SkillProperty& property, bool caster_is_monster) {
    // `CSkillProperty::getCoolDown`. The original also
    // requires the owner's `+0x188` (unidentified) to be zero for the monster branch, beside
    // `CBaseUnit::ISA(0x1b)`; a player never reaches it.
    return caster_is_monster ? property.monster_cooldown : property.cooldown;
}

bool weapon_requirements_met(const SkillProperty& property, const SkillCaster& caster) {
    // `CSkill::weaponRequirementsMet(CCharacter*)`.
    //
    const bool wants_right = !property.requirement_right.empty();
    const bool wants_left = !property.requirement_left.empty();
    if (!wants_right && !wants_left && !property.dual_wield_required) return true;

    const UnitTypeHierarchy* types = caster.unit_types;
    if (types == nullptr || types->empty()) return false;

    // The original holds resolved type ids and compares the right hand's
    // against zero to mean "no requirement authored"; the port holds the
    // authored names, so an empty name is the same test.
    const uint32_t want_right = wants_right ? types->id_of(property.requirement_right) : 0;
    const uint32_t want_left = wants_left ? types->id_of(property.requirement_left) : 0;

    const bool right_ok =
        !caster.right_hand_type.empty() && types->isa(caster.right_hand_type, want_right);
    const bool left_ok =
        !caster.left_hand_type.empty() && types->isa(caster.left_hand_type, want_left);

    if (property.dual_wield_required) {
        // Both hands, both matching; an empty left hand fails here.
        return right_ok && left_ok;
    }
    if (right_ok || !wants_right) return true;
    // Quirk kept: a two-handed weapon lives in the **left** slot, so the left hand is tested
    // for `[Left Handed]` (0x26) and `[Two Handed]` (10) and then against the right hand's
    // requirement.
    if (!caster.left_hand_type.empty() &&
        types->isa(caster.left_hand_type, unit_type::kLeftHanded) &&
        types->isa(caster.left_hand_type, unit_type::kTwoHanded)) {
        return types->isa(caster.left_hand_type, want_right);
    }
    return false;
}

bool is_in_range(const KnownSkill& skill, uint32_t level, const Vec3& from, const Vec3& to,
                 const SkillCaster& caster) {
    // `CSkill::getIsInRange`.
    const float dx = from.x - to.x;
    const float dy = from.y - to.y;
    const float dz = from.z - to.z;
    // `fmuls dy*dy`, `fmadds dz*dz + that`,
    // `fmadds dx*dx + that`.
    const float dist = std::sqrt(std::fma(dx, dx, std::fma(dz, dz, dy * dy)));

    // `REQUIRES_PASSABLE_LOS` (`CSkill +0x69`), skipped for a skill with no level;
    // an empty callback is that case.
    if (skill.def != nullptr && skill.def->requires_passable_los && caster.passable_between &&
        !caster.passable_between(from, to)) {
        return false;
    }

    const SkillProperty* property = skill.property(level);
    // `CSkill::getRangeMin`: property +0x84, fallback 0.0f.
    const float min_range = property != nullptr ? property->range_min : 0.0f;
    if (dist < min_range) return false;

    // `CSkill::getRange`: property +0x80, fallback 1.0f.
    const float max_range = property != nullptr ? property->range : 1.0f;
    return dist <= max_range;
}

namespace {

// `CAN_BE_SILENCED` (X360 `CSkill +0x48`) and the caster holding `SILENCE`.
bool silenced(const SkillDef& def, const SkillCaster& caster) {
    return def.can_be_silenced && caster.effects != nullptr &&
           caster.effects->has_effect(kEffectTypeSilence);
}

// The activation type asked against the skill's (X360 `CSkill +0x38`), either 0 passing.
bool activation_asked(SkillActivation own, SkillActivation asked) {
    return asked == SkillActivation::Any || own == SkillActivation::Any || own == asked;
}

}  // namespace

bool requirements_met(const KnownSkill& skill, uint32_t level, const SkillCaster& caster,
                      SkillActivation activation) {
    // `CSkill::requirementsMet`. `+0x6d`, enabled for use;
    // only `CAISkillManager` switches it, so a player's skill always passes.
    if (!skill.enabled) return false;
    if (skill.def == nullptr) return false;
    const SkillProperty* property = skill.property(level);
    if (property == nullptr) return false;
    if (!activation_asked(skill.def->activation, activation)) return false;
    // `+0x124`, the charges left ('s first test,
    // with the two above): none, and the skill is refused. -1 is no limit.
    if (skill.charges == 0) return false;

    // `THEMEREQUIRED` and `NOTTHEMED`: `CBaseUnit::hasUnitTheme`
    // on the caster, not the target; an unknown name skips
    // its test.
    // DIVERGENCE: resolved here, not when the property is built (`CSkillProperty::
    // CSkillProperty`); the table is loaded first, so same.
    // INVENTED: with no table, a skill authoring either key is refused.
    if (!property->theme_required.empty() || !property->not_themed.empty()) {
        if (caster.theme_book == nullptr) {
            return false;
        }
        if (!property->theme_required.empty()) {
            const UnitTheme* required =
                caster.theme_book->get_unit_theme(upper(property->theme_required));
            if (required != nullptr) {
                const bool has =
                    (caster.themes != nullptr) && caster.themes->has_unit_theme(required);
                if (!has) return false;
            }
        }
        if (!property->not_themed.empty()) {
            const UnitTheme* forbidden =
                caster.theme_book->get_unit_theme(upper(property->not_themed));
            if (forbidden != nullptr) {
                const bool has =
                    (caster.themes != nullptr) && caster.themes->has_unit_theme(forbidden);
                if (has) return false;
            }
        }
    }

    if (silenced(*skill.def, caster)) return false;

    switch (static_cast<SkillTargetType>(property->target_type)) {
        case SkillTargetType::Position: {
            if (!caster.target_position.has_value()) return false;
            if (!caster.skip_range) {
                if (!is_in_range(skill, level, caster.position, *caster.target_position, caster)) {
                    return false;
                }
            }
            break;
        }
        case SkillTargetType::Target: {
            if (!caster.target_unit.has_value()) return false;
            if (!caster.skip_range) {
                if (!is_in_range(skill, level, caster.position, caster.target_unit->position,
                                 caster)) {
                    return false;
                }
            }
            break;
        }
        case SkillTargetType::Item: {
            if (!caster.target_unit.has_value()) return false;
            if (!caster.target_unit->is_item) return false;
            break;
        }
        case SkillTargetType::UnidentifiedItem: {
            if (caster.target_unit.has_value() && caster.target_unit->is_item) {
                if (!caster.target_unit->is_equipment) return false;
                if (caster.target_unit->is_identified) return false;
            }
            break;
        }
        default: break;
    }
    return weapon_requirements_met(*property, caster);
}

bool roll_skill_chance(const SkillProperty& property) {
    // `CSkill::rollSkillChance`. At 100 or above there is no draw; the comparison
    // is `<=`, so a `CHANCE` of 0 still passes on a draw of 0.
    if (property.chance >= 100) return true;
    const int roll = volatile_random().integer_between(0, 100);
    return roll <= static_cast<int>(property.chance);
}

// PORT: the runner's latest copy of a skill.
SkillRunState* SkillRunner::find(const std::string& skill_name) {
    // The copy started last, which is the cast the character is in.
    SkillRunState* found = nullptr;
    for (std::pair<std::string, SkillRunState>& e : entries_) {
        if (e.first != skill_name) continue;
        if (found == nullptr || e.second.started_serial > found->started_serial) found = &e.second;
    }
    return found;
}

// PORT: the runner's latest copy of a skill.
const SkillRunState* SkillRunner::find(const std::string& skill_name) const {
    const SkillRunState* found = nullptr;
    for (const std::pair<std::string, SkillRunState>& e : entries_) {
        if (e.first != skill_name) continue;
        if (found == nullptr || e.second.started_serial > found->started_serial) found = &e.second;
    }
    return found;
}

// PORT: one copy of a skill in the runner.
SkillRunState* SkillRunner::find_copy(const std::string& skill_name, uint32_t copy) {
    for (std::pair<std::string, SkillRunState>& e : entries_) {
        if (e.first == skill_name && e.second.copy == copy) return &e.second;
    }
    return nullptr;
}

size_t SkillRunner::index_of(const SkillRunState& state) const {
    for (size_t k = 0; k < entries_.size(); ++k) {
        if (&entries_[k].second == &state) return k;
    }
    return entries_.size();
}

bool SkillRunner::in_active(size_t index) const {
    return std::find(active_.begin(), active_.end(), index) != active_.end();
}

SkillRunState& SkillRunner::idle_copy(const std::string& skill_name) {
    // `CSkillManager::executeSkill(wstring)`: the first copy
    // of the name not running; with every one running, `addSkill(last, true, false)`
    // adds one before `startSkill`, kept even if the roll fails.
    uint32_t copies = 0;
    for (std::pair<std::string, SkillRunState>& e : entries_) {
        if (e.first != skill_name) continue;
        if (!e.second.running) return e.second;
        ++copies;
    }
    entries_.emplace_back(skill_name, SkillRunState());
    entries_.back().second.copy = copies;
    return entries_.back().second;
}

// PORT: the runner's latest copy of a skill.
const SkillRunState* SkillRunner::state(const std::string& skill_name) const {
    return find(skill_name);
}

// PORT: the runner's latest copy of a skill.
SkillRunState* SkillRunner::state(const std::string& skill_name) { return find(skill_name); }

bool SkillRunner::is_cooling(const std::string& skill_name) const {
    // `CSkillManager::getSkillIsCooling`: any copy sharing
    // the skill's data with a cooldown strictly above zero.
    for (const std::pair<std::string, SkillRunState>& e : entries_) {
        if (e.first == skill_name && e.second.cooldown_remaining > 0.0f) return true;
    }
    return false;
}

// PORT: a cooldown set from outside.
void SkillRunner::set_cooldown_remaining(const std::string& skill_name, float seconds) {
    // A cooling copy is on the active list, where `update` runs its cooldown down.
    bool found = false;
    for (size_t k = 0; k < entries_.size(); ++k) {
        if (entries_[k].first != skill_name) continue;
        entries_[k].second.cooldown_remaining = seconds;
        if (seconds > 0.0f && !in_active(k)) active_.push_back(k);
        found = true;
    }
    if (found || !(seconds > 0.0f)) return;
    SkillRunState fresh;
    fresh.cooldown_remaining = seconds;
    entries_.emplace_back(skill_name, fresh);
    active_.push_back(entries_.size() - 1);
}

float SkillRunner::cooling_time(const std::string& skill_name) const {
    // `CSkillManager::getSkillCoolingTime`:
    // the largest positive remaining cooldown among the entries naming this
    // skill.
    float longest = 0.0f;
    for (const std::pair<std::string, SkillRunState>& e : entries_) {
        if (e.first == skill_name && e.second.cooldown_remaining > longest) {
            longest = e.second.cooldown_remaining;
        }
    }
    return longest;
}

// PORT: whether any copy of a skill runs.
bool SkillRunner::is_running(const std::string& skill_name) const {
    for (const std::pair<std::string, SkillRunState>& e : entries_) {
        if (e.first == skill_name && e.second.running) return true;
    }
    return false;
}

// PORT: whether any skill runs.
bool SkillRunner::any_running() const {
    for (const std::pair<std::string, SkillRunState>& e : entries_) {
        if (e.second.running) return true;
    }
    return false;
}

bool SkillRunner::can_be_executed(const KnownSkill& skill, uint32_t level,
                                  const SkillCaster& caster, SkillActivation activation) const {
    // `CSkillManager::getSkillCanBeExecuted`.
    if (is_cooling(skill.entry.name)) return false;
    return requirements_met(skill, level, caster, activation);
}

bool SkillRunner::execute(const KnownSkill& skill, uint32_t level, const SkillCaster& caster,
                          const SkillCastContext& context) {
    // `CSkillManager::executeSkill`: nothing
    // for a null skill or while skills are globally disabled.
    if (skill.def == nullptr || g_disable_skill_from_being_casted) return false;
    const SkillProperty* property = skill.property(level);
    if (property == nullptr) return false;

    // `EXCLUSIVE` (+0x87): every copy on the active list stopped (`stopSkill`), running or not;
    // a stopped copy stays on the list until `update`.
    if (property->exclusive) {
        for (size_t k = 0; k < active_.size(); ++k) {
            std::pair<std::string, SkillRunState>& e = entries_[active_[k]];
            if (e.first == skill.entry.name) stop_copy(skill, e.second, context);
        }
    }
    // The copy handed is the skill's own, the first; one the active list holds goes through the
    // by-name overload, which takes the first copy not
    // running or clones a new one, and refuses one the active list still holds.
    SkillRunState* first = find_copy(skill.entry.name, 0);
    if (first == nullptr) first = &idle_copy(skill.entry.name);
    SkillRunState* chosen = first;
    if (in_active(index_of(*first))) {
        chosen = &idle_copy(skill.entry.name);
        if (in_active(index_of(*chosen))) return false;
    }
    SkillRunState& state = *chosen;

    // `CSkill::startSkill`: the end flag
    // cleared and the animations assigned **before** the chance roll, so both
    // happen even on a failed one; then the roll, then `EVENT_START`.
    state.end_fired = false;
    assign_skill_animations(skill.def->base, caster, state.animations);
    if (!roll_skill_chance(*property)) return false;

    // The cooldown and both clocks are charged before the start group fires, whether or not
    // it applies anything. The countdown is the animation's length, so the skill runs until
    // the `HIT` key that fires `EVENT_TRIGGER`.
    state.level = level;
    state.cooldown_remaining = skill_cooldown(*property, caster.is_monster);
    state.speed = 1.0f;
    state.minimum_time_remaining = state.animations.length;
    state.elapsed = 0.0f;
    state.running = true;
    state.end_fired = false;
    state.started_serial = ++serial_;
    state.started_events.clear();
    // `+0x50`: the point the run keeps; `EVENT_START` is handed it and the
    // caster's target (`triggerEvent(0, ...)`'s 7th and 6th arguments).
    state.target_position = caster.target_position;
    state.target_type = property->target_type;
    state.target_unit = SkillEventTarget{};
    state.target_unit.has_unit = context.event_target.has_unit;
    state.target_unit.unit = context.event_target.unit;
    state.target_unit.machine = context.event_target.machine;
    state.target_unit.item = context.event_target.item;
    SkillCastContext started = context;
    started.event_target.position = caster.target_position;
    started.event_target.run = state.copy;
    started.started_events = &state.started_events;
    start_skill(skill, level, started);
    active_.push_back(index_of(state));
    decrement_skill_charges(skill);
    return true;
}

namespace {

// `Ogre::Vector3::normalise` on three floats.
void normalise_in_place(float& x, float& y, float& z) {
    const Vec3 n = ogre_normalised(Vec3{x, y, z});
    x = n.x;
    y = n.y;
    z = n.z;
}

}  // namespace

std::optional<Vec3> position_for_skill(const SkillProperty& property, const Vec3& skill_point,
                                       const SkillPositionUnit* owner,
                                       const SkillPositionUnit* target, RunicRandom& rolls) {
    // `CSkillManager::getPositionForSkill(CSkill*, CBaseUnit*)`.
    // The manager's `+0x78` starts as the
    // skill's `+0x50`.
    float x = skill_point.x;
    float y = skill_point.y;
    float z = skill_point.z;
    const SkillTargetType type = static_cast<SkillTargetType>(property.target_type);
    if (type == SkillTargetType::Position) return Vec3{x, y, z};
    if (type != SkillTargetType::PositionRandom && type != SkillTargetType::PositionFlee) {
        return std::nullopt;
    }

    // Both draw x then y from `randomBetweenVolatile(-1, 1)`, keep z, normalise, and stretch
    // by the band between the two ranges (`RANDOMRANGE`/`RANDOMRANGEMIN`;
    // `RANGE`/`RANGEMIN`), then add the inner range along the same line.
    const bool flee = type == SkillTargetType::PositionFlee;
    const float outer = flee ? property.range : property.random_range;
    const float inner = flee ? property.range_min : property.random_range_min;
    x = rolls.between(-1.0f, 1.0f);
    y = rolls.between(-1.0f, 1.0f);
    normalise_in_place(x, y, z);
    const float band = outer - inner;
    x *= band;
    y *= band;
    z *= band;
    float nx = x;
    float ny = y;
    float nz = z;
    normalise_in_place(nx, ny, nz);
    x = nx * inner + x;
    y = ny * inner + y;
    z = nz * inner + z;

    if (!flee) {
        if (owner == nullptr) {
            // round the unit handed in, at its feet; with neither,
            // the offset alone.
            if (target == nullptr) return Vec3{x, y, z};
            x = target->centre.x + x;
            y = (-target->radius + target->centre.y) + y;
            z = target->centre.z + z;
            return Vec3{x, y, z};
        }
        if (target == nullptr) {
            // round the owner's feet.
            x = owner->centre.x + x;
            y = (-owner->radius + owner->centre.y) + y;
            z = owner->centre.z + z;
            return Vec3{x, y, z};
        }
        // from the owner's feet towards the unit, level, as far as
        // it stands but held between `RANGEMIN` and `RANGE`.
        float dx = target->centre.x - owner->centre.x;
        float dy = 0.0f;
        float dz = target->centre.z - owner->centre.z;
        const float length = std::sqrt(std::fma(dz, dz, dx * dx));
        float reach = length < property.range ? length : property.range;
        reach = reach > property.range_min ? reach : property.range_min;
        normalise_in_place(dx, dy, dz);
        x = (dx * reach + owner->centre.x) + x;
        y = ((dy * reach + owner->centre.y) + -owner->radius) + y;
        z = (dz * reach + owner->centre.z) + z;
        return Vec3{x, y, z};
    }

    // `POSITIONFLEE`: a quarter of the offset round the owner, and half the range
    // back along the line from the owner to the unit (with no unit, against its facing).
    x *= 0.25f;
    y *= 0.25f;
    z *= 0.25f;
    if (owner == nullptr) {
        // DIVERGENCE: with a unit and no owner the console reads the radius through a null owner
        // and faults; the offset alone, as with neither.
        return Vec3{x, y, z};
    }
    x += owner->centre.x;
    y += owner->centre.y;
    z += owner->centre.z;
    float bx = owner->forward.x;
    float by = owner->forward.y;
    float bz = owner->forward.z;
    if (target != nullptr) {
        bx = target->centre.x - owner->centre.x;
        by = target->centre.y - owner->centre.y;
        bz = target->centre.z - owner->centre.z;
        normalise_in_place(bx, by, bz);
    }
    x = bx * -0.5f * property.range + x;
    y = (by * -0.5f * property.range - owner->radius) + y;
    z = bz * -0.5f * property.range + z;
    return Vec3{x, y, z};
}

void decrement_skill_charges(const KnownSkill& skill) {
    // `CSkillManager::decrementSkillCharges(CSkill*)`, after
    // `startSkill` succeeded: a count above 0 goes down by one.
    if (0 < skill.charges) --skill.charges;
}

SkillCastResult SkillRunner::cast(const KnownSkill& skill, const SkillCaster& caster,
                                  const SkillCastContext& context, float* mana_spent) {
    // `CCharacter::castSkill(long long)`, reduced: target
    // search, facing, walk-in and animation are the callers'.
    if (mana_spent != nullptr) *mana_spent = 0.0f;
    if (skill.def == nullptr) return SkillCastResult::NoSuchSkill;

    // `castSkill` casts at the skill's **effective** level, which is what every
    // other reader picks the property by. A skill with no points in it has no
    // level to cast at.
    const uint32_t level = skill.effective;
    if (level == 0) return SkillCastResult::NotLearned;
    const SkillProperty* property = skill.property(level);
    if (property == nullptr) return SkillCastResult::NoSuchSkill;

    // Two mana tests, both integer; `MANACOSTOT` is tested against the mana held, not what is
    // left after the first cost.
    const int64_t mana = static_cast<int64_t>(caster.mana);
    const int64_t cost = static_cast<int64_t>(property->mana_cost);
    const int64_t cost_ot = static_cast<int64_t>(property->mana_cost_ot);
    if (mana < cost || mana < cost_ot) return SkillCastResult::NotEnoughMana;

    if (is_cooling(skill.entry.name)) return SkillCastResult::Cooling;
    // `castSkill` asks for `NORMAL` (3).
    if (!requirements_met(skill, level, caster, SkillActivation::Normal)) {
        return SkillCastResult::RequirementsNotMet;
    }

    // A running skill is not refused here: `castSkill` holds back only while the character is
    // in the last cast (`still_casting`), and `executeSkill` starts another copy.
    if (!execute(skill, level, caster, context)) return SkillCastResult::ChanceFailed;

    // `modifyMana(-getManaCost())`, and only the up-front cost: `MANACOSTOT` is
    // charged per tick by the run, which is the `_OT` in its name.
    if (mana_spent != nullptr) *mana_spent = static_cast<float>(cost);
    return SkillCastResult::Cast;
}

bool SkillRunner::stop_copy(const KnownSkill& skill, SkillRunState& state,
                            const SkillCastContext& context) {
    // `CSkill::stopSkill`. `EVENT_END` fires here only once,
    // for a passive or a looping skill; a one-shot's is `updateSkill`'s.
    if (!state.running) return false;
    bool fired = false;
    if ((skill.passive() || state.animations.loop_into != -1) && !state.end_fired) {
        SkillCastContext fired_context = context;
        fired_context.event_target.run = state.copy;
        fired_context.started_events = &state.started_events;
        fired = stop_skill(skill, state.level, fired_context);
        state.end_fired = true;
    }
    state.running = false;
    state.elapsed = 0.0f;
    state.speed = 1.0f;
    // `resetEvent` on every event: `+0x140` cleared.
    state.started_events.clear();
    if (context.reset_events) context.reset_events(skill.entry.name, state.copy);
    return fired;
}

// PORT: every copy of a skill stopped (`stop_copy`, which holds the rule).
bool SkillRunner::stop(const KnownSkill& skill, const SkillCastContext& context) {
    bool fired = false;
    for (std::pair<std::string, SkillRunState>& e : entries_) {
        if (e.first != skill.entry.name) continue;
        fired = stop_copy(skill, e.second, context) || fired;
    }
    return fired;
}

void SkillRunner::stop_by_name(const std::string& skill_name, const SkillCastContext& context,
                               const std::function<const KnownSkill*(const std::string&)>& lookup) {
    // `CSkillManager::stopSkillsByName`:
    // every copy of the name.
    const KnownSkill* skill = lookup ? lookup(skill_name) : nullptr;
    if (skill != nullptr) stop(*skill, context);
}

void SkillRunner::stop_all(const SkillCastContext& context,
                           const std::function<const KnownSkill*(const std::string&)>& lookup,
                           bool force, bool passives, bool keep_on_death) {
    // `CSkillManager::stopAllSkills(bool, bool, bool)` over
    // the active list. By index: a stop's `EVENT_END` may start more.
    for (size_t i = 0; i < active_.size(); ++i) {
        const size_t k = active_[i];
        if (!entries_[k].second.running) continue;
        const std::string name = entries_[k].first;
        const KnownSkill* skill = lookup ? lookup(name) : nullptr;
        if (skill == nullptr) continue;
        if (!passives && skill->passive()) continue;
        if (keep_on_death) {
            const SkillProperty* property = skill->property(entries_[k].second.level);
            if (property == nullptr || property->section == nullptr ||
                (skill->def != nullptr && skill->def->dont_stop_on_death)) {
                continue;
            }
            bool caster_die = false;
            for (const tl::formats::AdmSection& child : property->event_section().children) {
                if (upper(child.name) == kEventGroupNames[9]) caster_die = true;
            }
            if (caster_die) continue;
        }
        if (force) {
            stop_copy(*skill, entries_[k].second, context);
            continue;
        }
        // `CSkill::attemptToStopSkill`:
        // `+0x104 = 0.001` (0x3a83126f) once every event `canStop`.
        SkillRunState& state = entries_[k].second;
        if (!context.events_can_stop || context.events_can_stop(name, state.copy)) {
            state.minimum_time_remaining = 0.001f;
        }
    }
}

int SkillRunner::trigger_event(int group, const SkillCastContext& context,
                               const std::function<const KnownSkill*(const std::string&)>& lookup) {
    // `CSkillManager::triggerSkillEvent`: the stored target
    // position goes to `CSkill::triggerEvent` for target types 1, 5 and 6 (mask `0x62`) only;
    // the unit is the caller's. Over the active list; a started block is passed over.
    if (group < 0 || group >= kEventGroupCount) return 0;
    int applied = 0;
    for (size_t i = 0; i < active_.size(); ++i) {
        const size_t k = active_[i];
        const std::string name = entries_[k].first;
        const KnownSkill* skill = lookup ? lookup(name) : nullptr;
        if (skill == nullptr) continue;
        SkillCastContext fired_context = context;
        const int32_t type = entries_[k].second.target_type;
        const bool keeps_point = 0 <= type && type <= 6 && ((1 << type) & 0x62) != 0;
        fired_context.event_target.position =
            keeps_point ? entries_[k].second.target_position : std::optional<Vec3>();
        fired_context.event_target.run = entries_[k].second.copy;
        if (trigger_skill_event(*skill, group, fired_context)) ++applied;
    }
    return applied;
}

bool SkillRunner::trigger_skill_event(const KnownSkill& skill, int group,
                                      const SkillCastContext& context) {
    // `CSkill::triggerEvent`: a skill that is
    // not running takes only group 8, `EVENT_DIEBYEFFECT`, which fires group 5
    // (`EVENT_UNITDIE`) first - itself refused when not running.
    if (group < 0 || group >= kEventGroupCount) return false;
    SkillRunState* state = find_copy(skill.entry.name, context.event_target.run);
    if (state == nullptr) state = find(skill.entry.name);
    if (state == nullptr) return false;
    // No unit handed: `startEvent` takes the run's own (`CSkill +0x40`), the
    // character or the item the cast was aimed at.
    SkillCastContext fired_context = context;
    fired_context.event_target.run = state->copy;
    fired_context.started_events = &state->started_events;
    if (!fired_context.event_target.has_unit && fired_context.event_target.item == 0) {
        fired_context.event_target.has_unit = state->target_unit.has_unit;
        fired_context.event_target.unit = state->target_unit.unit;
        fired_context.event_target.machine = state->target_unit.machine;
        fired_context.event_target.item = state->target_unit.item;
    }
    bool fired = false;
    if (group == 8) {
        if (state->running) fired = fire_event_group(skill, state->level, 5, fired_context);
    } else if (!state->running) {
        return false;
    }
    return fire_event_group(skill, state->level, group, fired_context) || fired;
}

void SkillRunner::update(float dt, const SkillCastContext& context,
                         const std::function<const KnownSkill*(const std::string&)>& lookup) {
    // `CSkill::updateSkill(float)` over the active list
    // (`CSkillManager::update`). Takes the frame's delta.
    if (dt <= 0.0f) return;
    const auto step = [&](size_t at) {
        const std::string name = entries_[at].first;
        SkillRunState& state = entries_[at].second;
        // The cooldown runs down **before** the running test, which is what lets
        // it outlive the run that charged it.
        state.cooldown_remaining -= dt;
        if (!state.running) return;

        // Both clocks advance at the skill's own rate, not at the frame's.
        state.elapsed += dt * state.speed;
        // The countdown is skipped entirely while the property is
        // `CONTINUOUSLOOPING`, so such a skill never reaches the end test below
        // on its own.
        const KnownSkill* skill = lookup ? lookup(name) : nullptr;
        const SkillProperty* property = skill != nullptr ? skill->property(state.level) : nullptr;
        const bool looping = property != nullptr && property->continuous_looping;
        if (!looping) state.minimum_time_remaining -= dt * state.speed;

        // Then the run's live events (`CSkillEvent::updateEvent`): while any is alive, a skill
        // with no `DURATIONOVERRIDEMS` and no loop is held.
        const float duration = property != nullptr ? property->duration_override : 0.0f;
        if (context.events_alive && context.events_alive(name, state.copy) && duration <= 0.0f &&
            state.animations.loop_into == -1) {
            return;
        }

        // `EVENT_END` fires when the countdown is spent, the skill is not a
        // passive, and the run has lasted at least `DURATIONOVERRIDEMS`.
        if (state.minimum_time_remaining > 0.0f) return;
        if (skill != nullptr && skill->passive()) return;
        if (duration > state.elapsed) return;

        // A group that applied something keeps the skill running one more
        // frame; the next frame finds `end_fired` set and stops it.
        if (skill != nullptr && !state.end_fired) {
            SkillCastContext fired_context = context;
            fired_context.event_target.run = state.copy;
            fired_context.started_events = &state.started_events;
            const bool fired = stop_skill(*skill, state.level, fired_context);
            state.end_fired = true;
            if (fired) return;
        }
        // `CSkill::stopSkill`, with nothing
        // left to fire.
        state.running = false;
        state.elapsed = 0.0f;
        state.speed = 1.0f;
        state.started_events.clear();
        if (context.reset_events) context.reset_events(name, state.copy);
    };
    for (size_t k = 0; k < active_.size();) {
        const size_t at = active_[k];
        step(at);
        // `CSkillManager::update`: neither running nor cooling, the copy leaves the list, the
        // last taking its place.
        const SkillRunState& state = entries_[at].second;
        if (!state.running && state.cooldown_remaining <= 0.0f) {
            active_[k] = active_.back();
            active_.pop_back();
            continue;
        }
        ++k;
    }
}

// Make sure the group-name table and the two indices `skill_cast.h` publishes
// stay in step, since the index is what `triggerEvent` takes.
static_assert(kSkillEventStart == 0, "EVENT_START is group 0");
static_assert(kSkillEventEnd == 1, "EVENT_END is group 1");
static_assert(sizeof(kEventGroupNames) / sizeof(kEventGroupNames[0]) == kEventGroupCount,
              "the eleven event groups");

}  // namespace tl::core
