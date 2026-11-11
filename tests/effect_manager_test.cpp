// An affix applied to a unit, ticked, and the totals the rest of the engine reads off it.
#include "core/effect_manager.h"

#include "core/combat.h"

#include <cmath>
#include "core/skills.h"
#include "test_support.h"

namespace {

using tl::core::Affix;
using tl::core::DamageType;
using tl::core::Effect;
using tl::core::EffectActivation;
using tl::core::EffectManager;
using tl::core::EffectTypeTable;
using tl::core::kDamageTypeAny;
using tl::core::kDurationAlways;
using tl::core::kEffectTypeAttackSpellBonus;
using tl::core::kEffectTypeSkillBonus;
using tl::formats::AdmFile;
using tl::formats::AdmRecord;
using tl::formats::AdmSection;
using tl::formats::AdmType;

AdmRecord str(const std::string& key, const std::string& value) {
    AdmRecord r;
    r.key = key;
    r.type = AdmType::String;
    r.text = value;
    return r;
}

/// An effects-list row with no `VALUE` tags, so the reader falls back to
/// `VALUE`, `VALUE2`.. - which is all these tests need.
AdmSection plain_row(const std::string& name) {
    AdmSection row;
    row.name = "EFFECT";
    row.records.push_back(str("NAME", name));
    row.records.push_back(str("TYPE", "Value"));
    return row;
}

/// The effect type rows this file names, at their shipped indices (the index is the type).
EffectTypeTable shipped_types() {
    AdmFile file;
    file.ok = true;
    file.root.name = "EFFECTLIST";
    static const struct {
        int index;
        const char* name;
    } kNamed[] = {
        {0x07, "HP RECHARGE"},         {0x34, "DAMAGE"},
        {0x52, "SKILL BONUS"},         {0x55, "ATTACK SPELL BONUS"},
        {0x56, "DEFENSE SPELL BONUS"}, {0x57, "CHARM SPELL BONUS"},
    };
    for (int index = 0; index <= 0x7a; ++index) {
        const char* name = nullptr;
        for (const auto& entry : kNamed) {
            if (entry.index == index) name = entry.name;
        }
        file.root.children.push_back(
            plain_row(name != nullptr ? name : "FILLER " + std::to_string(index)));
    }
    EffectTypeTable table;
    table.load(file);
    return table;
}

/// One effect built directly, the way a `[EFFECT]` block would read. `value` is
/// its single number; with no graph, `effect_base_value` uses it as it stands.
Effect make_effect(const std::string& name, int type, float value, float duration,
                   EffectActivation activation = EffectActivation::Dynamic,
                   DamageType damage = DamageType::Physical) {
    Effect effect;
    effect.name = name;
    effect.type = type;
    effect.values[0] = value;
    effect.values[1] = value;
    effect.duration = duration;
    effect.authored_duration = duration != kDurationAlways;
    effect.activation = activation;
    effect.damage_type = damage;
    return effect;
}

Affix make_affix(const std::string& name, float duration, const std::vector<Effect>& effects) {
    Affix affix;
    affix.name = name;
    affix.min_spawn_range = 0;
    affix.max_spawn_range = 100;
    affix.duration = duration;
    affix.authored_duration = duration > 0.0f;
    affix.effects = effects;
    return affix;
}

EffectManager::Context context(const EffectTypeTable& types, const tl::core::StatBook& graphs) {
    EffectManager::Context ctx;
    ctx.types = &types;
    ctx.graphs = &graphs;
    ctx.owner.present = true;
    ctx.owner.level = 1;
    return ctx;
}

TL_TEST(an_affix_puts_its_effects_where_the_queries_find_them) {
    const EffectTypeTable types = shipped_types();
    const tl::core::StatBook graphs;
    EffectManager manager;
    manager.set_context(context(types, graphs));

    // An `ALWAYS` effect, the shape a passive's affix authors.
    manager.add_affix(
        make_affix("CRITICALSTRIKES", -1.0f,
                   {make_effect("EMBERBOLT", kEffectTypeSkillBonus, 2.0f, kDurationAlways)}),
        1);

    TL_CHECK_EQ(manager.affixes().size(), 1u);
    TL_CHECK(std::fabs(manager.effect_value(kEffectTypeSkillBonus) - 2.0f) < 0.001f);
    // The by-name form is what `+N to <skill>` uses, and it matches the
    // effect's own `NAME` against the skill's.
    TL_CHECK(std::fabs(manager.effect_value(kEffectTypeSkillBonus, "EMBERBOLT") - 2.0f) < 0.001f);
    TL_CHECK(std::fabs(manager.effect_value(kEffectTypeSkillBonus, "EMBERLANCE")) < 0.001f);
    TL_CHECK(manager.has_effect(kEffectTypeSkillBonus));
}

TL_TEST(an_always_effect_on_a_permanent_affix_never_ages) {
    const EffectTypeTable types = shipped_types();
    const tl::core::StatBook graphs;
    EffectManager manager;
    manager.set_context(context(types, graphs));
    manager.add_affix(make_affix("MAGICWEAPONSMASTER", -1.0f,
                                 {make_effect("MAGICWEAPONS", kEffectTypeAttackSpellBonus, 1.0f,
                                              kDurationAlways, EffectActivation::Passive)}),
                      1);

    // A passive holds for as long as the unit does: an `ALWAYS` duration never
    // ages, and an affix with a negative duration never expires.
    for (int frame = 0; frame < 600; ++frame) {
        manager.update_effects(1.0f / 60.0f);
        manager.update_affixes(1.0f / 60.0f);
        manager.delete_dead_effects();
    }
    TL_CHECK_EQ(manager.affixes().size(), 1u);
    TL_CHECK(std::fabs(manager.effect_value(kEffectTypeAttackSpellBonus) - 1.0f) < 0.001f);
    // A `Passive` effect sits in its own list, and the narrowed query finds it
    // there and not in the `Dynamic` one.
    TL_CHECK(std::fabs(manager.effect_value(EffectActivation::Passive, kEffectTypeAttackSpellBonus,
                                            kDamageTypeAny) -
                       1.0f) < 0.001f);
    TL_CHECK(std::fabs(manager.effect_value(EffectActivation::Dynamic, kEffectTypeAttackSpellBonus,
                                            kDamageTypeAny)) < 0.001f);
}

TL_TEST(a_timed_affix_takes_its_effects_with_it_when_it_runs_out) {
    const EffectTypeTable types = shipped_types();
    const tl::core::StatBook graphs;
    EffectManager manager;
    manager.set_context(context(types, graphs));
    // Ember Shield's shape: a buff with a real duration on both the affix and
    // the effect.
    manager.add_affix(
        make_affix("EMBERSHIELD", 30.0f, {make_effect("SHIELD", 0x34, 120.0f, 30.0f)}), 1);
    TL_CHECK(std::fabs(manager.effect_value(0x34) - 120.0f) < 0.001f);

    for (int frame = 0; frame < 60 * 29; ++frame) {
        manager.update_effects(1.0f / 60.0f);
        manager.update_affixes(1.0f / 60.0f);
        manager.delete_dead_effects();
    }
    TL_CHECK_EQ(manager.affixes().size(), 1u);

    for (int frame = 0; frame < 60 * 2; ++frame) {
        manager.update_effects(1.0f / 60.0f);
        manager.update_affixes(1.0f / 60.0f);
        manager.delete_dead_effects();
    }
    TL_CHECK_EQ(manager.affixes().size(), 0u);
    TL_CHECK(std::fabs(manager.effect_value(0x34)) < 0.001f);
    TL_CHECK(!manager.has_effect(0x34));
}

TL_TEST(recasting_retires_the_copy_that_has_already_ticked) {
    const EffectTypeTable types = shipped_types();
    const tl::core::StatBook graphs;
    EffectManager manager;
    manager.set_context(context(types, graphs));

    // `DAMAGE` with a real duration, which the parse forces exclusive.
    Effect shield = make_effect("SHIELD", 0x34, 100.0f, 30.0f);
    shield.exclusive = true;
    manager.add_effect(shield);
    for (int frame = 0; frame < 60 * 10; ++frame) manager.update_effects(1.0f / 60.0f);
    // The same effect cast again: `addNewEffect` marks the copy that
    // has already ticked rather than letting the two stack.
    manager.add_effect(shield);
    manager.calculate_effect_values();
    TL_CHECK(std::fabs(manager.effect_value(0x34) - 100.0f) < 0.001f);
    manager.delete_dead_effects();
    TL_CHECK_EQ(manager.all_effects().size(), 1u);
}

TL_TEST(a_second_copy_that_is_not_exclusive_stacks) {
    const EffectTypeTable types = shipped_types();
    const tl::core::StatBook graphs;
    EffectManager manager;
    manager.set_context(context(types, graphs));

    // `addNewEffect` runs its retire scan only for a new effect
    // whose `+0x31` is set; two same-named copies without it both count.
    manager.add_effect(make_effect("STR", 0x07, 10.0f, kDurationAlways, EffectActivation::Passive));
    manager.add_effect(make_effect("STR", 0x07, 5.0f, kDurationAlways, EffectActivation::Passive));
    manager.calculate_effect_values();
    TL_CHECK(std::fabs(manager.effect_value(0x07) - 15.0f) < 0.001f);
    manager.delete_dead_effects();
    TL_CHECK_EQ(manager.all_effects().size(), 2u);

    // Marked exclusive, a third copy retires both.
    Effect third = make_effect("STR", 0x07, 1.0f, kDurationAlways, EffectActivation::Passive);
    third.exclusive = true;
    manager.add_effect(third);
    manager.calculate_effect_values();
    TL_CHECK(std::fabs(manager.effect_value(0x07) - 1.0f) < 0.001f);
}

TL_TEST(parse_effect_reads_exclusive_and_forces_it_for_timed_damage) {
    const EffectTypeTable types = shipped_types();
    auto block = [](const std::string& type, const std::string& duration, bool exclusive_key,
                    bool exclusive) {
        AdmSection section;
        section.name = "EFFECT";
        section.records.push_back(str("TYPE", type));
        if (!duration.empty()) section.records.push_back(str("DURATION", duration));
        if (exclusive_key) {
            AdmRecord r;
            r.key = "EXCLUSIVE";
            r.type = AdmType::Bool;
            r.integer = exclusive ? 1u : 0u;
            section.records.push_back(r);
        }
        return section;
    };
    // `CEffect::CEffect(CDataGroup*)`: default 0, the key read over it.
    TL_CHECK(!tl::core::parse_effect(block("HP RECHARGE", "", false, false), types).exclusive);
    TL_CHECK(tl::core::parse_effect(block("HP RECHARGE", "", true, true), types).exclusive);
    // `DAMAGE` (0x34) with a real duration is forced on, even over a 0 key;
    // `ALWAYS` and `INSTANT` are not durations and leave it as read.
    TL_CHECK(tl::core::parse_effect(block("DAMAGE", "5", true, false), types).exclusive);
    TL_CHECK(!tl::core::parse_effect(block("DAMAGE", "ALWAYS", false, false), types).exclusive);
    TL_CHECK(!tl::core::parse_effect(block("DAMAGE", "INSTANT", false, false), types).exclusive);
}

TL_TEST(link_name_kills_the_effect_it_names) {
    const EffectTypeTable types = shipped_types();
    const tl::core::StatBook graphs;
    EffectManager manager;
    manager.set_context(context(types, graphs));

    Effect leader = make_effect("LEADER", 0x34, 10.0f, 1.0f);
    leader.link_name = "FOLLOWER";
    manager.add_effect(leader);
    manager.calculate_effect_values();
    manager.add_effect(make_effect("FOLLOWER", 0x34, 5.0f, kDurationAlways));
    manager.calculate_effect_values();
    TL_CHECK(std::fabs(manager.effect_value(0x34) - 15.0f) < 0.001f);

    // The leader runs out; `deleteDeadEffects` kills what its
    // `LINK_NAME` names on the way out, so the `ALWAYS` partner goes too.
    for (int frame = 0; frame < 120; ++frame) manager.update_effects(1.0f / 60.0f);
    manager.delete_dead_effects();
    manager.delete_dead_effects();
    TL_CHECK_EQ(manager.all_effects().size(), 0u);
    // `deleteDeadEffects` leaves the totals to the unit's next update.
    TL_CHECK(std::fabs(manager.effect_value(0x34) - 15.0f) < 0.001f);
    manager.calculate_effect_values();
    TL_CHECK(std::fabs(manager.effect_value(0x34)) < 0.001f);
}

TL_TEST(the_damage_type_filter_narrows_what_the_cache_totals) {
    const EffectTypeTable types = shipped_types();
    const tl::core::StatBook graphs;
    EffectManager manager;
    manager.set_context(context(types, graphs));
    manager.add_effect(make_effect("FIREPART", 0x34, 10.0f, kDurationAlways,
                                   EffectActivation::Dynamic, DamageType::Fire));
    manager.calculate_effect_values();
    manager.add_effect(make_effect("ICEPART", 0x34, 4.0f, kDurationAlways,
                                   EffectActivation::Dynamic, DamageType::Ice));
    manager.calculate_effect_values();

    // `kDamageTypeAny` is answered from the cache, so it is the sum of both.
    TL_CHECK(std::fabs(manager.effect_value(0x34, kDamageTypeAny) - 14.0f) < 0.001f);
    TL_CHECK(std::fabs(manager.effect_value(0x34, static_cast<int>(DamageType::Fire)) - 10.0f) <
             0.001f);
    TL_CHECK(std::fabs(manager.effect_value(0x34, static_cast<int>(DamageType::Ice)) - 4.0f) <
             0.001f);
    TL_CHECK(std::fabs(manager.effect_value(0x34, static_cast<int>(DamageType::Poison))) < 0.001f);
}

TL_TEST(the_effective_level_takes_the_skill_and_spell_bonuses) {
    const EffectTypeTable types = shipped_types();
    const tl::core::StatBook graphs;
    EffectManager manager;
    manager.set_context(context(types, graphs));

    tl::core::SkillDef def;
    def.name = "EMBERBOLT";
    def.type = tl::core::SkillType::Offensive;
    def.levels.resize(10);

    tl::core::KnownSkill skill;
    skill.entry.name = "EMBERBOLT";
    skill.def = &def;
    skill.invested = 3;

    // With nothing applied the effective level is what was invested.
    tl::core::SkillBook::calculate_effective_level(skill, &manager);
    TL_CHECK_EQ(skill.effective, 3u);

    // `+2 to Ember Bolt` is a `SKILL BONUS` effect whose `NAME` is the skill's.
    manager.add_effect(make_effect("EMBERBOLT", kEffectTypeSkillBonus, 2.0f, kDurationAlways));
    manager.calculate_effect_values();
    tl::core::SkillBook::calculate_effective_level(skill, &manager);
    TL_CHECK_EQ(skill.effective, 5u);

    // `+1 to all attack spells` stacks on top of it for an offensive skill.
    manager.add_effect(
        make_effect("ALLATTACK", kEffectTypeAttackSpellBonus, 1.0f, kDurationAlways));
    manager.calculate_effect_values();
    tl::core::SkillBook::calculate_effective_level(skill, &manager);
    TL_CHECK_EQ(skill.effective, 6u);

    // A skill nobody has spent a point on takes no bonus at all: the original
    // gates the whole block on the invested level being non-zero.
    tl::core::KnownSkill unlearned;
    unlearned.entry.name = "EMBERBOLT";
    unlearned.def = &def;
    unlearned.invested = 0;
    tl::core::SkillBook::calculate_effective_level(unlearned, &manager);
    TL_CHECK_EQ(unlearned.effective, 0u);

    // And the level count caps it: ten `[LEVELn]` blocks, so ten is the top.
    skill.invested = 9;
    tl::core::SkillBook::calculate_effective_level(skill, &manager);
    TL_CHECK_EQ(skill.effective, 10u);
}

TL_TEST(deleting_an_affix_removes_only_its_own_effects) {
    const EffectTypeTable types = shipped_types();
    const tl::core::StatBook graphs;
    EffectManager manager;
    manager.set_context(context(types, graphs));
    manager.add_affix(make_affix("A", -1.0f, {make_effect("ONE", 0x34, 1.0f, kDurationAlways)}), 1);
    manager.add_affix(make_affix("B", -1.0f, {make_effect("TWO", 0x34, 2.0f, kDurationAlways)}), 1);
    TL_CHECK(std::fabs(manager.effect_value(0x34) - 3.0f) < 0.001f);

    TL_CHECK(manager.delete_affix("A"));
    TL_CHECK(std::fabs(manager.effect_value(0x34) - 2.0f) < 0.001f);
    TL_CHECK(!manager.delete_affix("A"));

    // `clearEffects(true)` takes the effects and the affixes but
    // leaves the cache, which the caller recalculates.
    manager.clear_effects(true);
    TL_CHECK_EQ(manager.affixes().size(), 0u);
    TL_CHECK(std::fabs(manager.effect_value(0x34) - 2.0f) < 0.001f);
    manager.calculate_effect_values();
    TL_CHECK(std::fabs(manager.effect_value(0x34)) < 0.001f);
}

TL_TEST(clear_effects_false_keeps_the_lists_and_true_keeps_only_block_chance) {
    const EffectTypeTable types = shipped_types();
    const tl::core::StatBook graphs;
    EffectManager manager;
    manager.set_context(context(types, graphs));
    manager.add_effect(
        make_effect("BLOCK", 0x3e, 12.0f, kDurationAlways, EffectActivation::Passive));
    manager.calculate_effect_values();
    manager.add_affix(make_affix("A", -1.0f, {make_effect("ONE", 0x34, 1.0f, kDurationAlways)}), 1);

    // False skips the list pass, but each affix's destructor still takes
    // its effects off (`clearEffectsFromOwner`).
    manager.clear_effects(false);
    TL_CHECK_EQ(manager.affixes().size(), 0u);
    manager.calculate_effect_values();
    TL_CHECK(std::fabs(manager.effect_value(0x34)) < 0.001f);
    TL_CHECK(std::fabs(manager.effect_value(0x3e) - 12.0f) < 0.001f);

    // True: everything but type 0x3e is swap-removed.
    manager.add_effect(make_effect("ONE", 0x34, 1.0f, kDurationAlways));
    manager.clear_effects(true);
    manager.calculate_effect_values();
    TL_CHECK(std::fabs(manager.effect_value(0x34)) < 0.001f);
    TL_CHECK(std::fabs(manager.effect_value(0x3e) - 12.0f) < 0.001f);
}

TL_TEST(clear_out_affix_effects_strips_affix_effects_and_keeps_the_affixes) {
    const EffectTypeTable types = shipped_types();
    const tl::core::StatBook graphs;
    EffectManager manager;
    manager.set_context(context(types, graphs));
    manager.add_effect(make_effect("OWN", 0x34, 4.0f, kDurationAlways));
    manager.add_affix(make_affix("A", -1.0f, {make_effect("ONE", 0x34, 1.0f, kDurationAlways)}), 1);
    manager.clear_out_affix_effects();
    TL_CHECK_EQ(manager.affixes().size(), 1u);
    manager.calculate_effect_values();
    // 5 before; the affix's 1 is gone, the free-standing 4 stays.
    TL_CHECK(std::fabs(manager.effect_value(0x34) - 4.0f) < 0.001f);
}

TL_TEST(add_affix_by_name_clones_from_the_book_and_needs_one) {
    const EffectTypeTable types = shipped_types();
    const tl::core::StatBook graphs;
    tl::core::AffixBook book;
    book.add(make_affix("SET_BONUS", -1.0f, {make_effect("ONE", 0x34, 3.0f, kDurationAlways)}));
    EffectManager manager;
    manager.set_context(context(types, graphs));
    // No resource manager: nothing.
    TL_CHECK(!manager.add_affix_by_name("SET_BONUS", 1, nullptr));
    TL_CHECK(!manager.add_affix_by_name("NO_SUCH", 1, &book));
    TL_CHECK(manager.add_affix_by_name("set_bonus", 1, &book));  // the book upper-cases
    TL_CHECK_EQ(manager.affixes().size(), 1u);
    TL_CHECK(std::fabs(manager.effect_value(0x34) - 3.0f) < 0.001f);
}

// `CAffix::addEffectsToEffectManager`: each effect cloned,
// given its affix owner, rolled and activated - never through the unit's `applyEffectOnUnit`,
// which only a skill event's effects go through (`offer_effect`). A unit hook that would refuse
// the burn or rewrite the summon's duration is not asked; both are held as authored.
TL_TEST(an_affixs_effects_are_held_without_asking_the_unit_hook) {
    const EffectTypeTable types = shipped_types();
    const tl::core::StatBook graphs;
    EffectManager manager;
    EffectManager::Context ctx = context(types, graphs);
    int offered = 0;
    ctx.on_unit = [&offered](Effect& effect) {
        ++offered;
        if (effect.type == 0x2f) effect.duration = effect.values[0];
        return effect.type != 0x34;  // the unit would refuse the burn
    };
    manager.set_context(ctx);
    TL_CHECK(manager.has_unit_hook());
    manager.add_affix(make_affix("EMBERSENTRY", -1.0f,
                                 {make_effect("SUMMON", 0x2f, 60.0f, 1.0f),
                                  make_effect("BURN", 0x34, 5.0f, 3.0f)}),
                      1);
    TL_CHECK_EQ(offered, 0);
    TL_CHECK(manager.has_effect(0x2f));
    TL_CHECK(manager.has_effect(0x34));
    const std::vector<tl::core::LiveEffect*> held = manager.live_effects_of_type(0x2f);
    TL_CHECK_EQ(held.size(), 1u);
    TL_CHECK(std::fabs(held[0]->effect.duration - 1.0f) < 0.001f);
    // A held affix's countdown can be set again (`getAffix` then `+0xa4`).
    TL_CHECK(manager.set_affix_duration("EMBERSENTRY", 2.0f));
    TL_CHECK(!manager.set_affix_duration("NOSUCHAFFIX", 2.0f));
}

// `CCharacter::AC()` over `armorBonus()`, and `damageDefense`:
// `PERCENT ARMOR BONUS` -20 takes a fifth off, rounded up.
TL_TEST(a_units_armour_reads_its_effects) {
    const EffectTypeTable types = shipped_types();
    const tl::core::StatBook graphs;
    EffectManager manager;
    manager.set_context(context(types, graphs));
    tl::core::UnitCombat combat;
    combat.armor = 21;
    combat.damage_defense[2] = 9;
    TL_CHECK_EQ(tl::core::armor_class(combat, 0, nullptr), 21);
    TL_CHECK_EQ(tl::core::damage_defense(combat, 2, 0, nullptr), 9);
    manager.add_effect(make_effect("FRAILTY DEBUFF1", 0x17, -20.0f, 6.0f, EffectActivation::Dynamic,
                                   DamageType::All));
    manager.calculate_effect_values();
    // ceil(21 * -0.2) = ceil(-4.2) = -4, so 17. The fire lookup sums the fire
    // and the all-types totals: 9 * (1 - 0.2) = 7.2, ceiled to 8.
    TL_CHECK_EQ(tl::core::armor_class(combat, 0, &manager), 17);
    TL_CHECK_EQ(tl::core::damage_defense(combat, 2, 0, &manager), 8);
}

// A weaponless unit's `maxDamage` is raised by `PERCENT DAMAGE BONUS`, `ceilf`.
TL_TEST(a_units_blow_reads_its_damage_bonus) {
    const EffectTypeTable types = shipped_types();
    const tl::core::StatBook graphs;
    EffectManager manager;
    manager.set_context(context(types, graphs));
    tl::core::UnitCombat imp;
    imp.attack.max_damage = 30;
    TL_CHECK_EQ(tl::core::max_damage(imp, nullptr), 30);
    manager.add_effect(make_effect("SUMMONIMP PERCENT DAMAGE BONUS", 0x19, 15.0f, kDurationAlways,
                                   EffectActivation::Passive));
    manager.calculate_effect_values();
    // ceil(30 * 0.15) = 5, plus 30.
    TL_CHECK_EQ(tl::core::max_damage(imp, &manager), 35);
}
}  // namespace

// `CCharacter::updateEffects`, `CEffect::fxShouldPlay` and
// `CEffect::playFX`: a held effect's particle starts once.
TL_TEST(a_held_effect_starts_its_particle_once) {
    EffectManager manager;
    Effect potion = make_effect("POTION", 3, 10.0f, 4.0f);
    potion.particle_fx = "HEALTH_POTION";
    Effect plain = make_effect("PLAIN", 3, 10.0f, 4.0f);
    Effect carried = make_effect("CARRIED", 3, 10.0f, 4.0f, EffectActivation::Transfer);
    carried.particle_fx = "GENERICPOTION";
    manager.add_effect(potion);
    manager.add_effect(plain);
    manager.add_effect(carried);

    const std::vector<std::string> first = manager.take_effect_fx();
    TL_CHECK_EQ(first.size(), size_t{1});
    TL_CHECK_EQ(first[0], std::string("HEALTH_POTION"));
    TL_CHECK(manager.take_effect_fx().empty());

    // The walk asks before it looks at what has run out: an effect whose time
    // is up on its first update still shows its particle.
    Effect late = make_effect("LATE", 3, 10.0f, 1.0f);
    late.particle_fx = "MANA_POTION";
    manager.add_effect(late);
    manager.update_effects(2.0f);
    const std::vector<std::string> second = manager.take_effect_fx();
    TL_CHECK_EQ(second.size(), size_t{1});
    TL_CHECK_EQ(second[0], std::string("MANA_POTION"));
}

// `CCharacter::modifyDamage`: `PERCENT DAMAGE TAKEN` of all
// (6) raises a fire hit by half - `fmadds(50, 0.01, 1) * 20`.
TL_TEST(modify_damage_applies_percent_damage_taken) {
    const EffectTypeTable types = shipped_types();
    const tl::core::StatBook graphs;
    EffectManager manager;
    manager.set_context(context(types, graphs));
    manager.add_effect(make_effect("TAKEN", tl::core::kEffectTypePercentDamageTaken, 50.0f,
                                   kDurationAlways, EffectActivation::Passive, DamageType::All));
    manager.calculate_effect_values();
    tl::core::Defender defender;
    defender.effects = tl::core::CharacterEffects(&manager);
    tl::core::RunicRandom rolls(1);
    TL_CHECK_EQ(tl::core::modify_damage(defender, 20, 2, -1, 1.0f, rolls), 30);
}

// A `SHIELD BUFFER` bigger than the hit takes it all and keeps the rest; the
// hit is 0 and `Absorbed!` is raised.
TL_TEST(modify_damage_shield_buffer_absorbs_the_hit) {
    const EffectTypeTable types = shipped_types();
    const tl::core::StatBook graphs;
    EffectManager manager;
    manager.set_context(context(types, graphs));
    manager.add_effect(make_effect("EMBERSHIELD", tl::core::kEffectTypeShieldBuffer, 30.0f, 10.0f,
                                   EffectActivation::Dynamic, DamageType::All));
    int raised = 0;
    tl::core::Defender defender;
    defender.shields = &manager;
    defender.on_absorbed = [&raised] { ++raised; };
    tl::core::RunicRandom rolls(1);
    TL_CHECK_EQ(tl::core::modify_damage(defender, 20, 2, -1, 1.0f, rolls), 0);
    TL_CHECK_EQ(raised, 1);
    // 10 left in it, and a second hit of 15 goes through by 5, spending it.
    TL_CHECK_EQ(tl::core::modify_damage(defender, 15, 2, -1, 1.0f, rolls), 5);
    TL_CHECK_EQ(raised, 1);
    // Spent, it is dead but still held (and seen by `hasEffect`) until its update removes it.
    TL_CHECK(manager.has_effect(tl::core::kEffectTypeShieldBuffer));
    TL_CHECK(manager.live_effects_of_type(tl::core::kEffectTypeShieldBuffer).front()->dead);
}

// A shield of another damage type is passed over; a physical hit it does not
// take is never under 1.
TL_TEST(modify_damage_shield_buffer_keeps_to_its_damage_type) {
    const EffectTypeTable types = shipped_types();
    const tl::core::StatBook graphs;
    EffectManager manager;
    manager.set_context(context(types, graphs));
    manager.add_effect(make_effect("ICESHIELD", tl::core::kEffectTypeShieldBuffer, 100.0f, 10.0f,
                                   EffectActivation::Dynamic, DamageType::Ice));
    int raised = 0;
    tl::core::Defender defender;
    defender.shields = &manager;
    defender.on_absorbed = [&raised] { ++raised; };
    tl::core::RunicRandom rolls(1);
    TL_CHECK_EQ(tl::core::modify_damage(defender, 12, 2, -1, 1.0f, rolls), 12);
    TL_CHECK_EQ(tl::core::modify_damage(defender, 12, 3, -1, 1.0f, rolls), 0);
    TL_CHECK_EQ(raised, 1);
}

// a negative result is 0, but 1 when nothing was absorbed and the
// whole maximum, over 10, was asked for.
TL_TEST(modify_damage_negative_result_quirk) {
    tl::core::Defender defender;
    defender.damage_defense[2] = 100;
    tl::core::RunicRandom rolls(1);
    // A defence roll of at least 50 against 11 of 11, factor 1: negative.
    TL_CHECK_EQ(tl::core::modify_damage(defender, 11, 2, 11, 1.0f, rolls), 1);
    TL_CHECK_EQ(tl::core::modify_damage(defender, 11, 2, 12, 1.0f, rolls), 0);
    TL_CHECK_EQ(tl::core::modify_damage(defender, 10, 2, 10, 1.0f, rolls), 0);
}
