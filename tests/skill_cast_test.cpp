// Casting a skill: the gate it has to pass, the cooldown it charges, the clock a run keeps.
#include "core/skill_cast.h"
#include "core/effect_manager.h"
#include "core/unit_themes.h"

#include <cmath>
#include "core/random.h"
#include "test_support.h"

namespace {

using tl::core::KnownSkill;
using tl::core::SkillCastContext;
using tl::core::SkillCaster;
using tl::core::SkillCastResult;
using tl::core::SkillDef;
using tl::core::SkillProperty;
using tl::core::SkillRunner;
using tl::core::UnitThemeBook;
using tl::core::UnitThemeSet;
using tl::core::UnitTypeHierarchy;
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

AdmRecord num(const std::string& key, int64_t value) {
    AdmRecord r;
    r.key = key;
    r.type = AdmType::Integer;
    r.integer = static_cast<uint32_t>(value);
    r.number = static_cast<float>(value);
    return r;
}

AdmRecord boolean(const std::string& key, bool value) {
    AdmRecord r;
    r.key = key;
    r.type = AdmType::Bool;
    r.integer = value ? 1u : 0u;
    return r;
}

AdmRecord flt(const std::string& key, float value) {
    AdmRecord r;
    r.key = key;
    r.type = AdmType::Float;
    r.number = value;
    r.integer = static_cast<uint32_t>(value);
    return r;
}

/// A skill file with one `[LEVEL1]`, which is the shape every subject in M14's
/// scope has. `extra` goes on the level, `root_extra` on the root.
AdmFile skill_file(const std::string& name, std::vector<AdmRecord> root_extra,
                   std::vector<AdmRecord> level_extra) {
    AdmFile file;
    file.ok = true;
    file.root.name = "SKILL";
    file.root.records.push_back(str("NAME", name));
    file.root.records.push_back(str("ACTIVATION_TYPE", "NORMAL"));
    for (AdmRecord& r : root_extra) file.root.records.push_back(r);
    AdmSection level;
    level.name = "LEVEL1";
    for (AdmRecord& r : level_extra) level.records.push_back(r);
    file.root.children.push_back(level);
    return file;
}

/// The four unit type ids the weapon gate names. A two-hander is authored both left handed
/// and two handed.
UnitTypeHierarchy shipped_unit_types() {
    AdmFile file;
    file.ok = true;
    file.root.name = "HIERARCHY";
    AdmSection types;
    types.name = "UNITTYPES";
    const auto add = [&types](const char* name, int id, const std::vector<int>& ancestors) {
        AdmSection group;
        group.name = name;
        group.records.push_back(num("ID", id));
        int n = 0;
        for (int a : ancestors) {
            group.records.push_back(num("child" + std::to_string(n++), a));
        }
        types.children.push_back(group);
    };
    add("item", 2, {});
    add("Weapon", 8, {2});
    add("Two Handed", 10, {8, 2});
    add("Left Handed", 38, {8, 2});
    add("Sword", 50, {8, 2});
    add("Staff", 51, {10, 38, 8, 2});
    file.root.children.push_back(types);
    UnitTypeHierarchy hierarchy;
    hierarchy.parse(file.root);
    return hierarchy;
}

/// A `KnownSkill` over a definition the caller owns, learned to level 1.
KnownSkill learned(const SkillDef& def, uint32_t level = 1) {
    KnownSkill skill;
    skill.entry.name = def.name;
    skill.def = &def;
    skill.invested = level;
    skill.effective = level;
    return skill;
}
// `KnownSkill` keeps a pointer to its def: a temporary would dangle.
KnownSkill learned(SkillDef&&, uint32_t = 1) = delete;

// -- the cooldown ----------------------------------------------------------

TL_TEST(cooldown_is_the_monster_one_only_for_a_monster) {
    // `COOLDOWNMS` / `MONSTERCOOLDOWNMS` in ms come back as seconds;
    // `CSkillProperty::getCoolDown` picks one on `ISA([Monster])`.
    const AdmFile file =
        skill_file("Test", {}, {num("COOLDOWNMS", 2500), num("MONSTERCOOLDOWNMS", 8000)});
    const SkillDef def = tl::core::parse_skill_def(file);
    const SkillProperty* level = def.levels.at(0).section != nullptr ? &def.levels.at(0) : nullptr;
    TL_CHECK(level != nullptr);
    TL_CHECK(std::fabs(tl::core::skill_cooldown(*level, false) - 2.5f) < 0.0005f);
    TL_CHECK(std::fabs(tl::core::skill_cooldown(*level, true) - 8.0f) < 0.0005f);
}

TL_TEST(a_level_inherits_the_roots_cooldown_through_milliseconds) {
    // An inherited cooldown goes back to ms rounded up: 350 * 0.001f becomes 351 ms.
    // Kept.
    const AdmFile file = skill_file("Test", {num("COOLDOWNMS", 350)}, {num("MANACOST", 5)});
    const SkillDef def = tl::core::parse_skill_def(file);
    TL_CHECK(def.base.cooldown == 0.35000002f);
    TL_CHECK(def.levels.at(0).cooldown == 0.351f);
}

// -- the chance roll -------------------------------------------------------

TL_TEST(a_chance_of_a_hundred_draws_nothing) {
    // `CSkill::rollSkillChance` does not draw at 100 or above.
    SkillProperty property;
    TL_CHECK_EQ(property.chance, 100u);
    const uint64_t before = tl::core::volatile_random().state();
    TL_CHECK(tl::core::roll_skill_chance(property));
    TL_CHECK_EQ(tl::core::volatile_random().state(), before);

    // Below 100 it does draw. Seeded first: state 0 is the generator's fixed point.
    tl::core::volatile_random().set_seed(12345);
    const uint64_t seeded = tl::core::volatile_random().state();
    property.chance = 50;
    tl::core::roll_skill_chance(property);
    TL_CHECK(tl::core::volatile_random().state() != seeded);
}

TL_TEST(a_chance_of_zero_still_passes_on_a_draw_of_zero) {
    // `roll <= chance` over [0, 100]: `CHANCE:0` is one chance in 101. Kept.
    tl::core::volatile_random().set_seed(9876);
    SkillProperty property;
    property.chance = 0;
    int passes = 0;
    for (int i = 0; i < 2000; ++i) {
        if (tl::core::roll_skill_chance(property)) ++passes;
    }
    TL_CHECK(passes > 0);
    TL_CHECK(passes < 200);
}

// -- the weapon gate -------------------------------------------------------

TL_TEST(no_weapon_requirement_passes_bare_handed) {
    const SkillProperty property;
    SkillCaster caster;
    TL_CHECK(tl::core::weapon_requirements_met(property, caster));
}

TL_TEST(a_right_hand_requirement_is_met_by_the_right_hand) {
    const UnitTypeHierarchy types = shipped_unit_types();
    SkillProperty property;
    property.requirement_right = "Weapon";

    SkillCaster caster;
    caster.unit_types = &types;
    TL_CHECK(!tl::core::weapon_requirements_met(property, caster));  // empty hands

    caster.right_hand_type = "Sword";
    TL_CHECK(tl::core::weapon_requirements_met(property, caster));
}

TL_TEST(a_two_hander_in_the_left_hand_meets_a_right_hand_requirement) {
    // The gate's middle arm: a `[Left Handed]` `[Two Handed]` weapon is re-tested against the
    // right hand's requirement.
    const UnitTypeHierarchy types = shipped_unit_types();
    SkillProperty property;
    property.requirement_right = "Weapon";

    SkillCaster caster;
    caster.unit_types = &types;
    caster.left_hand_type = "Staff";
    TL_CHECK(tl::core::weapon_requirements_met(property, caster));

    // A left-hand weapon that is not two-handed does not.
    caster.left_hand_type = "Sword";
    TL_CHECK(!tl::core::weapon_requirements_met(property, caster));
}

TL_TEST(dual_wield_required_wants_both_hands) {
    const UnitTypeHierarchy types = shipped_unit_types();
    SkillProperty property;
    property.dual_wield_required = true;
    property.requirement_right = "Weapon";
    property.requirement_left = "Weapon";

    SkillCaster caster;
    caster.unit_types = &types;
    caster.right_hand_type = "Sword";
    TL_CHECK(!tl::core::weapon_requirements_met(property, caster));
    caster.left_hand_type = "Sword";
    TL_CHECK(tl::core::weapon_requirements_met(property, caster));
}

// -- the gate proper -------------------------------------------------------

TL_TEST(a_self_targeted_skill_needs_no_target_and_no_range) {
    const AdmFile file = skill_file("Shield", {str("TARGET_TYPE", "SELF")}, {num("MANACOST", 12)});
    const SkillDef def = tl::core::parse_skill_def(file);
    TL_CHECK_EQ(def.base.target_type, static_cast<int32_t>(tl::core::SkillTargetType::Self));
    const KnownSkill skill = learned(def);
    const SkillCaster caster;
    TL_CHECK(tl::core::requirements_met(skill, 1, caster));
}

TL_TEST(target_requires_unit_and_checks_range) {
    const AdmFile file = skill_file("Bolt", {str("TARGET_TYPE", "TARGET")}, {flt("RANGE", 10.0f)});
    const SkillDef def = tl::core::parse_skill_def(file);
    const KnownSkill skill = learned(def);

    SkillCaster caster;
    caster.position = {0.0f, 0.0f, 0.0f};

    // No target unit is refused
    TL_CHECK(!tl::core::requirements_met(skill, 1, caster));

    // Target unit at distance 5 passes
    caster.target_unit = tl::core::SkillTargetUnit{{5.0f, 0.0f, 0.0f}, false, false, false};
    TL_CHECK(tl::core::requirements_met(skill, 1, caster));

    // Target unit at distance 10.0 exactly passes
    caster.target_unit->position = {10.0f, 0.0f, 0.0f};
    TL_CHECK(tl::core::requirements_met(skill, 1, caster));

    // Target unit at distance 10.5 fails
    caster.target_unit->position = {10.5f, 0.0f, 0.0f};
    TL_CHECK(!tl::core::requirements_met(skill, 1, caster));
}

TL_TEST(range_min_refuses_closer_targets_and_passes_at_boundary) {
    const AdmFile file = skill_file("Nova", {str("TARGET_TYPE", "TARGET")},
                                    {flt("RANGE", 10.0f), flt("RANGEMIN", 3.0f)});
    const SkillDef def = tl::core::parse_skill_def(file);
    const KnownSkill skill = learned(def);

    SkillCaster caster;
    caster.position = {0.0f, 0.0f, 0.0f};

    // Distance 2.0 fails (< RANGEMIN)
    caster.target_unit = tl::core::SkillTargetUnit{{2.0f, 0.0f, 0.0f}, false, false, false};
    TL_CHECK(!tl::core::requirements_met(skill, 1, caster));

    // Distance 3.0 passes (== RANGEMIN)
    caster.target_unit->position = {3.0f, 0.0f, 0.0f};
    TL_CHECK(tl::core::requirements_met(skill, 1, caster));
}

TL_TEST(position_requires_target_position_and_checks_range) {
    const AdmFile file =
        skill_file("GroundSlam", {str("TARGET_TYPE", "POSITION")}, {flt("RANGE", 10.0f)});
    const SkillDef def = tl::core::parse_skill_def(file);
    const KnownSkill skill = learned(def);

    SkillCaster caster;
    caster.position = {0.0f, 0.0f, 0.0f};

    // No position given is refused
    TL_CHECK(!tl::core::requirements_met(skill, 1, caster));

    // Position in range passes
    caster.target_position = tl::core::Vec3{5.0f, 0.0f, 0.0f};
    TL_CHECK(tl::core::requirements_met(skill, 1, caster));

    // Position out of range fails
    caster.target_position = tl::core::Vec3{15.0f, 0.0f, 0.0f};
    TL_CHECK(!tl::core::requirements_met(skill, 1, caster));
}

TL_TEST(distance_is_full_three_dimensional_euclidean) {
    // A target 8 across and 6 up is 10 away in 3-D: out of `RANGE` 9 (2-D would say 8).
    const AdmFile file = skill_file("Beam", {str("TARGET_TYPE", "TARGET")}, {flt("RANGE", 9.0f)});
    const SkillDef def = tl::core::parse_skill_def(file);
    const KnownSkill skill = learned(def);

    SkillCaster caster;
    caster.position = {0.0f, 0.0f, 0.0f};
    caster.target_unit = tl::core::SkillTargetUnit{{8.0f, 6.0f, 0.0f}, false, false, false};

    // Full 3-D distance is 10.0, so RANGE 9.0 must fail
    TL_CHECK(!tl::core::requirements_met(skill, 1, caster));

    // With RANGE 10.0, it must pass
    const AdmFile file10 =
        skill_file("Beam", {str("TARGET_TYPE", "TARGET")}, {flt("RANGE", 10.0f)});
    const SkillDef def10 = tl::core::parse_skill_def(file10);
    const KnownSkill skill10 = learned(def10);
    TL_CHECK(tl::core::requirements_met(skill10, 1, caster));
}

TL_TEST(skip_range_flag_passes_out_of_range_target) {
    const AdmFile file = skill_file("Snipe", {str("TARGET_TYPE", "TARGET")}, {flt("RANGE", 5.0f)});
    const SkillDef def = tl::core::parse_skill_def(file);
    const KnownSkill skill = learned(def);

    SkillCaster caster;
    caster.position = {0.0f, 0.0f, 0.0f};
    caster.target_unit = tl::core::SkillTargetUnit{{50.0f, 0.0f, 0.0f}, false, false, false};

    // Out of range normally fails
    TL_CHECK(!tl::core::requirements_met(skill, 1, caster));

    // With skip_range = true, it passes to weapon gate
    caster.skip_range = true;
    TL_CHECK(tl::core::requirements_met(skill, 1, caster));
}

TL_TEST(line_of_sight_gate_respects_flag_and_callback) {
    const AdmFile file = skill_file(
        "ColossusBeam", {str("TARGET_TYPE", "TARGET"), boolean("REQUIRES_PASSABLE_LOS", true)},
        {flt("RANGE", 10.0f)});
    const SkillDef def = tl::core::parse_skill_def(file);
    TL_CHECK(def.requires_passable_los);
    const KnownSkill skill = learned(def);

    SkillCaster caster;
    caster.position = {0.0f, 0.0f, 0.0f};
    caster.target_unit = tl::core::SkillTargetUnit{{5.0f, 0.0f, 0.0f}, false, false, false};

    int los_calls = 0;
    caster.passable_between = [&](const tl::core::Vec3&, const tl::core::Vec3&) {
        los_calls++;
        return false;  // wall blocks LOS
    };

    // In range, but blocked by LOS -> fails
    TL_CHECK(!tl::core::requirements_met(skill, 1, caster));
    TL_CHECK_EQ(los_calls, 1);

    // If callback returns true -> passes
    caster.passable_between = [&](const tl::core::Vec3&, const tl::core::Vec3&) {
        los_calls++;
        return true;
    };
    TL_CHECK(tl::core::requirements_met(skill, 1, caster));
    TL_CHECK_EQ(los_calls, 2);

    // Skill WITHOUT requires_passable_los flag: callback is never called
    const AdmFile plain_file =
        skill_file("Fireball", {str("TARGET_TYPE", "TARGET")}, {flt("RANGE", 10.0f)});
    const SkillDef plain_def = tl::core::parse_skill_def(plain_file);
    TL_CHECK(!plain_def.requires_passable_los);
    const KnownSkill plain_skill = learned(plain_def);

    int plain_calls = 0;
    caster.passable_between = [&](const tl::core::Vec3&, const tl::core::Vec3&) {
        plain_calls++;
        return false;
    };
    TL_CHECK(tl::core::requirements_met(plain_skill, 1, caster));
    TL_CHECK_EQ(plain_calls, 0);
}

TL_TEST(item_target_type_requires_item_target) {
    const AdmFile file = skill_file("Transmute", {str("TARGET_TYPE", "ITEM")}, {});
    const SkillDef def = tl::core::parse_skill_def(file);
    const KnownSkill skill = learned(def);

    SkillCaster caster;

    // No target unit is refused
    TL_CHECK(!tl::core::requirements_met(skill, 1, caster));

    // Target that is not an item is refused
    caster.target_unit =
        tl::core::SkillTargetUnit{{0.0f, 0.0f, 0.0f}, /*is_item=*/false, false, false};
    TL_CHECK(!tl::core::requirements_met(skill, 1, caster));

    // Target that IS an item passes
    caster.target_unit->is_item = true;
    TL_CHECK(tl::core::requirements_met(skill, 1, caster));
}

TL_TEST(unidentified_item_target_type_behavior) {
    const AdmFile file = skill_file("Identify", {str("TARGET_TYPE", "UNIDENTIFIEDITEM")}, {});
    const SkillDef def = tl::core::parse_skill_def(file);
    const KnownSkill skill = learned(def);

    SkillCaster caster;

    // 1. Missing target passes to weapon gate
    TL_CHECK(tl::core::requirements_met(skill, 1, caster));

    // 2. Non-item target passes to weapon gate
    caster.target_unit =
        tl::core::SkillTargetUnit{{0.0f, 0.0f, 0.0f}, /*is_item=*/false, false, false};
    TL_CHECK(tl::core::requirements_met(skill, 1, caster));

    // 3. Item target that is not equipment fails (dynamic_cast<CEquipment*> null)
    caster.target_unit->is_item = true;
    caster.target_unit->is_equipment = false;
    caster.target_unit->is_identified = false;
    TL_CHECK(!tl::core::requirements_met(skill, 1, caster));

    // 4. Item equipment that is already identified fails (+0x348 != 0)
    caster.target_unit->is_equipment = true;
    caster.target_unit->is_identified = true;
    TL_CHECK(!tl::core::requirements_met(skill, 1, caster));

    // 5. Item equipment that is unidentified passes (+0x348 == 0)
    caster.target_unit->is_identified = false;
    TL_CHECK(tl::core::requirements_met(skill, 1, caster));
}

TL_TEST(range_gate_still_ends_in_weapon_gate) {
    const UnitTypeHierarchy types = shipped_unit_types();
    const AdmFile file = skill_file("Slash", {str("TARGET_TYPE", "TARGET")},
                                    {flt("RANGE", 10.0f), str("REQUIREMENT_RIGHT", "Sword")});
    const SkillDef def = tl::core::parse_skill_def(file);
    const KnownSkill skill = learned(def);

    SkillCaster caster;
    caster.unit_types = &types;
    caster.position = {0.0f, 0.0f, 0.0f};
    caster.target_unit = tl::core::SkillTargetUnit{{5.0f, 0.0f, 0.0f}, false, false, false};

    // Target is in range, but hands are empty -> weapon gate fails
    TL_CHECK(!tl::core::requirements_met(skill, 1, caster));

    // Equipping a Sword satisfies the weapon gate
    caster.right_hand_type = "Sword";
    TL_CHECK(tl::core::requirements_met(skill, 1, caster));
}

TL_TEST(range_keys_parse_and_inherit_from_parent) {
    // Root defines RANGEMIN, RANDOMRANGE, RANDOMRANGEMIN
    AdmFile file;
    file.ok = true;
    file.root.name = "SKILL";
    file.root.records.push_back(str("NAME", "RangedSkill"));
    file.root.records.push_back(flt("RANGE", 45.0f));
    file.root.records.push_back(flt("RANGEMIN", 5.0f));
    file.root.records.push_back(flt("RANDOMRANGE", 7.5f));
    file.root.records.push_back(flt("RANDOMRANGEMIN", 1.5f));
    file.root.records.push_back(boolean("REQUIRES_PASSABLE_LOS", true));

    // LEVEL1 overrides RANGE and RANGEMIN, inherits RANDOMRANGE and RANDOMRANGEMIN
    AdmSection level1;
    level1.name = "LEVEL1";
    level1.records.push_back(flt("RANGE", 50.0f));
    level1.records.push_back(flt("RANGEMIN", 6.0f));
    file.root.children.push_back(level1);

    // LEVEL2 overrides nothing, inherits all 4 from LEVEL1
    AdmSection level2;
    level2.name = "LEVEL2";
    file.root.children.push_back(level2);

    const SkillDef def = tl::core::parse_skill_def(file);
    TL_CHECK(def.requires_passable_los);

    // Root defaults
    TL_CHECK_EQ(def.base.range, 45.0f);
    TL_CHECK_EQ(def.base.range_min, 5.0f);
    TL_CHECK_EQ(def.base.random_range, 7.5f);
    TL_CHECK_EQ(def.base.random_range_min, 1.5f);

    // LEVEL1
    TL_CHECK_EQ(def.levels.at(0).range, 50.0f);
    TL_CHECK_EQ(def.levels.at(0).range_min, 6.0f);
    TL_CHECK_EQ(def.levels.at(0).random_range, 7.5f);
    TL_CHECK_EQ(def.levels.at(0).random_range_min, 1.5f);

    // LEVEL2 overrides nothing, inherits all 4 from LEVEL1: each level's parent
    // is the level before it
    TL_CHECK_EQ(def.levels.at(1).range, 50.0f);
    TL_CHECK_EQ(def.levels.at(1).range_min, 6.0f);
    TL_CHECK_EQ(def.levels.at(1).random_range, 7.5f);
    TL_CHECK_EQ(def.levels.at(1).random_range_min, 1.5f);
}

TL_TEST(an_unknown_target_type_word_leaves_the_parents_value) {
    // `TARGET_TYPE` is a word, and an unrecognised one has to leave the
    // inherited value rather than fall to zero - the same trap `TARGET` set.
    const AdmFile file =
        skill_file("Odd", {str("TARGET_TYPE", "SELF")}, {str("TARGET_TYPE", "NOT_A_TARGET_TYPE")});
    const SkillDef def = tl::core::parse_skill_def(file);
    TL_CHECK_EQ(def.levels.at(0).target_type,
                static_cast<int32_t>(tl::core::SkillTargetType::Self));
}

// -- the cast and the run --------------------------------------------------

TL_TEST(a_cast_charges_the_cooldown_and_refuses_the_next_one) {
    const AdmFile file = skill_file("Shield", {str("TARGET_TYPE", "SELF")},
                                    {num("MANACOST", 12), num("COOLDOWNMS", 3000)});
    const SkillDef def = tl::core::parse_skill_def(file);
    const KnownSkill skill = learned(def);

    SkillCaster caster;
    caster.mana = 40.0f;
    const SkillCastContext context;
    SkillRunner runner;

    float spent = 0.0f;
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, context, &spent)),
                static_cast<int>(SkillCastResult::Cast));
    TL_CHECK(std::fabs(spent - 12.0f) < 0.0005f);
    TL_CHECK(runner.is_cooling("Shield"));
    TL_CHECK(std::fabs(runner.cooling_time("Shield") - 3.0f) < 0.0005f);

    // A second cast while it cools is refused, and costs nothing.
    spent = -1.0f;
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, context, &spent)),
                static_cast<int>(SkillCastResult::Cooling));
    TL_CHECK(std::fabs(spent) < 0.0005f);
}

TL_TEST(the_mana_gate_compares_integers) {
    // `getManaCost` truncates the property's float to a `long`, so
    // 11 mana does not pay a cost of 12 and 12 exactly does.
    const AdmFile file =
        skill_file("Shield", {str("TARGET_TYPE", "SELF")}, {flt("MANACOST", 12.0f)});
    const SkillDef def = tl::core::parse_skill_def(file);
    const KnownSkill skill = learned(def);
    const SkillCastContext context;

    SkillCaster caster;
    caster.mana = 11.9f;
    SkillRunner low;
    TL_CHECK_EQ(static_cast<int>(low.cast(skill, caster, context)),
                static_cast<int>(SkillCastResult::NotEnoughMana));

    caster.mana = 12.0f;
    SkillRunner exact;
    TL_CHECK_EQ(static_cast<int>(exact.cast(skill, caster, context)),
                static_cast<int>(SkillCastResult::Cast));
}

TL_TEST(a_skill_with_no_points_in_it_cannot_be_cast) {
    const AdmFile file = skill_file("Shield", {str("TARGET_TYPE", "SELF")}, {num("MANACOST", 1)});
    const SkillDef def = tl::core::parse_skill_def(file);
    KnownSkill skill = learned(def);
    skill.invested = 0;
    skill.effective = 0;

    SkillCaster caster;
    caster.mana = 100.0f;
    const SkillCastContext context;
    SkillRunner runner;
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, context)),
                static_cast<int>(SkillCastResult::NotLearned));
}

TL_TEST(a_cooldown_runs_down_after_the_run_has_stopped) {
    // `CSkill::updateSkill` counts the cooldown down before the running test; a
    // run lasts its animation, 1 s with no model (`assignSkillAnimations`).
    const AdmFile file =
        skill_file("Shield", {str("TARGET_TYPE", "SELF")},
                   {num("MANACOST", 1), num("COOLDOWNMS", 2000), num("MINIMUMTIMEMS", 200)});
    const SkillDef def = tl::core::parse_skill_def(file);
    const KnownSkill skill = learned(def);
    const auto lookup = [&skill](const std::string& name) -> const KnownSkill* {
        return name == skill.entry.name ? &skill : nullptr;
    };

    SkillCaster caster;
    caster.mana = 100.0f;
    const SkillCastContext context;
    SkillRunner runner;
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, context)),
                static_cast<int>(SkillCastResult::Cast));
    TL_CHECK(runner.is_running("Shield"));

    // Past `MINIMUMTIMEMS` but inside the animation: still running.
    runner.update(0.25f, context, lookup);
    TL_CHECK(runner.is_running("Shield"));
    // Past the animation: the run ends, the cooldown has not.
    runner.update(0.75f, context, lookup);
    TL_CHECK(!runner.is_running("Shield"));
    TL_CHECK(runner.is_cooling("Shield"));

    // And the cooldown keeps draining with nothing running.
    runner.update(0.5f, context, lookup);
    TL_CHECK(runner.is_cooling("Shield"));
    runner.update(0.5f, context, lookup);
    TL_CHECK(!runner.is_cooling("Shield"));
    TL_CHECK(std::fabs(runner.cooling_time("Shield")) < 0.0005f);

    // And now it can be cast again.
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, context)),
                static_cast<int>(SkillCastResult::Cast));
}

TL_TEST(a_unit_hit_fires_on_its_own_skill_only_while_it_runs) {
    // `CSkillEvent::invokeHitSkills` -> `CSkill::triggerEvent`, group 4:
    // a stopped skill takes nothing but group 8.
    AdmFile file = skill_file("Summon", {str("TARGET_TYPE", "POSITION")}, {num("MANACOST", 1)});
    AdmSection hit;
    hit.name = "EVENT_UNITHIT";
    hit.records.push_back(str("FILE", "media/skills/x/hit.layout"));
    file.root.children.at(0).children.push_back(hit);
    const SkillDef def = tl::core::parse_skill_def(file);
    const KnownSkill skill = learned(def);
    const auto lookup = [&skill](const std::string& name) -> const KnownSkill* {
        return name == skill.entry.name ? &skill : nullptr;
    };

    tl::core::EffectManager target;
    SkillCastContext context;
    context.target = &target;
    int fired = 0;
    int64_t unit = 0;
    bool had_point = true;
    context.on_event = [&](const KnownSkill&, uint32_t, int group, const AdmSection& event,
                           const tl::core::SkillEventTarget& to) {
        if (group != 4) return;
        ++fired;
        TL_CHECK_EQ(event.string_or("FILE", ""), std::string("media/skills/x/hit.layout"));
        unit = to.has_unit ? to.unit : 0;
        had_point = to.position.has_value();
    };
    SkillRunner runner;
    // Not running yet: refused.
    TL_CHECK(!runner.trigger_skill_event(skill, 4, context));
    TL_CHECK_EQ(fired, 0);

    SkillCaster caster;
    caster.mana = 10.0f;
    caster.target_position = tl::core::Vec3{1.0f, 0.0f, 0.0f};
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, context)),
                static_cast<int>(SkillCastResult::Cast));
    SkillCastContext struck = context;
    struck.event_target.has_unit = true;
    struck.event_target.unit = 42;
    runner.trigger_skill_event(skill, 4, struck);
    TL_CHECK_EQ(fired, 1);
    TL_CHECK_EQ(unit, int64_t{42});
    TL_CHECK(!had_point);

    // Its run over, the skill takes no more hits.
    runner.update(2.0f, context, lookup);
    TL_CHECK(!runner.is_running("Summon"));
    runner.trigger_skill_event(skill, 4, struck);
    TL_CHECK_EQ(fired, 1);
}

TL_TEST(a_run_keeps_the_item_it_was_aimed_at) {
    // `CCharacter::performSkill`: the run keeps the target, and a group fired with
    // no unit takes it.
    AdmFile file = skill_file("Bolt", {}, {num("MANACOST", 1)});
    AdmSection trigger;
    trigger.name = "EVENT_UNITHIT";
    trigger.records.push_back(str("FILE", "media/skills/x/bolt.layout"));
    file.root.children.at(0).children.push_back(trigger);
    const SkillDef def = tl::core::parse_skill_def(file);
    const KnownSkill skill = learned(def);

    tl::core::EffectManager target;
    SkillCastContext context;
    context.target = &target;
    std::vector<std::pair<int64_t, int64_t>> seen;  // (item, unit)
    context.on_event = [&](const KnownSkill&, uint32_t, int group, const AdmSection&,
                           const tl::core::SkillEventTarget& to) {
        if (group == 4) seen.push_back({to.item, to.has_unit ? to.unit : 0});
    };
    SkillCaster caster;
    caster.mana = 10.0f;
    SkillCastContext aimed = context;
    aimed.event_target.item = 77;
    SkillRunner runner;
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, aimed)),
                static_cast<int>(SkillCastResult::Cast));
    // A group fired with nothing handed (`EVENT_UNITHIT`, which may clone): the run's item.
    runner.trigger_skill_event(skill, 4, context);
    TL_CHECK_EQ(seen.size(), size_t{1});
    TL_CHECK_EQ(seen.back().first, int64_t{77});
    TL_CHECK_EQ(seen.back().second, int64_t{0});
    // Handed a unit: the unit, and no item.
    SkillCastContext struck = context;
    struck.event_target.has_unit = true;
    struck.event_target.unit = 5;
    runner.trigger_skill_event(skill, 4, struck);
    TL_CHECK_EQ(seen.size(), size_t{2});
    TL_CHECK_EQ(seen.back().first, int64_t{0});
    TL_CHECK_EQ(seen.back().second, int64_t{5});
}

TL_TEST(an_events_affixes_aimed_at_enemies_break_an_item) {
    // `CSkillEvent::effectsTargetEnemy`: true for an `[AFFIXES]` spec targeting
    // `ENEMY`, the default; not inline `[EFFECTS]` or `SELF`.
    const tl::core::EffectTypeTable types;
    SkillCastContext context;
    context.types = &types;
    const auto event_with = [](const char* block, std::vector<AdmRecord> records) {
        AdmSection event;
        event.name = "EVENT_TRIGGER";
        event.records.push_back(boolean("APPLYEFFECTS", true));
        AdmSection inner;
        inner.name = block;
        for (AdmRecord& r : records) inner.records.push_back(r);
        event.children.push_back(inner);
        return event;
    };
    const AdmSection lightning =
        event_with("AFFIXES", {num("AFFIXLEVEL", 10), str("AFFIX", "EMBERLIGHTNING")});
    TL_CHECK(tl::core::event_effects_target_enemy(lightning, context, nullptr,
                                                  tl::core::SkillTargetFilter::Enemy));
    // The same spec on a `SELF` skill is `ENEMY` too: the skill's default is
    // the root's and `[EFFECTS]`', not an `[AFFIXES]` block's.
    TL_CHECK(tl::core::event_effects_target_enemy(lightning, context, nullptr,
                                                  tl::core::SkillTargetFilter::Self));
    const AdmSection mine =
        event_with("AFFIXES", {str("AFFIX", "EMBERLIGHTNING"), str("TARGET", "SELF")});
    TL_CHECK(!tl::core::event_effects_target_enemy(mine, context, nullptr,
                                                   tl::core::SkillTargetFilter::Enemy));
    const AdmSection inline_only = event_with("EFFECTS", {});
    TL_CHECK(!tl::core::event_effects_target_enemy(inline_only, context, nullptr,
                                                   tl::core::SkillTargetFilter::Enemy));
    // An event with no block of its own asks the level's.
    AdmSection bare;
    bare.name = "EVENT_TRIGGER";
    AdmSection level;
    level.name = "LEVEL1";
    AdmSection affixes;
    affixes.name = "AFFIXES";
    affixes.records.push_back(str("AFFIX", "EMBERFIREBALL"));
    level.children.push_back(affixes);
    TL_CHECK(tl::core::event_effects_target_enemy(bare, context, &level,
                                                  tl::core::SkillTargetFilter::Enemy));
    TL_CHECK(!tl::core::event_effects_target_enemy(bare, context, nullptr,
                                                   tl::core::SkillTargetFilter::Enemy));
}

TL_TEST(a_skill_takes_a_living_target_on_the_side_it_aims_at) {
    // `CSkill::getTargetIsValid`.
    using tl::core::SkillTargetQuery;
    SkillTargetQuery q;
    q.target_type = 0;       // `NONE`, Ember Lightning's
    q.target_alignment = 2;  // `EVIL`
    q.owner_alignment = 1;   // the player
    q.owner.alive = true;
    q.owner.is_owner = true;
    q.candidate.alive = true;
    q.candidate.enemy = true;
    TL_CHECK(tl::core::skill_target_is_valid(q));
    q.candidate.enemy = false;
    TL_CHECK(!tl::core::skill_target_is_valid(q));
    // `GOOD`: anyone but an enemy.
    q.target_alignment = 1;
    TL_CHECK(tl::core::skill_target_is_valid(q));
    // Anything else: no one.
    q.target_alignment = 0;
    TL_CHECK(!tl::core::skill_target_is_valid(q));
    // `EVERYBODY` and an owner of alignment 4: anyone alive.
    q.target_type = 4;
    TL_CHECK(tl::core::skill_target_is_valid(q));
    q.target_type = 0;
    q.owner_alignment = 4;
    TL_CHECK(tl::core::skill_target_is_valid(q));
    // An owner of alignment 0: no one, `EVERYBODY` included.
    q.owner_alignment = 0;
    q.target_type = 4;
    TL_CHECK(!tl::core::skill_target_is_valid(q));
    q.owner_alignment = 1;
    q.target_type = 0;
    q.target_alignment = 2;
    q.candidate.enemy = true;
    // The dead only when asked for (`TARGET_CORPSES`), and then only them.
    q.candidate.alive = false;
    TL_CHECK(!tl::core::skill_target_is_valid(q));
    q.dead_wanted = true;
    TL_CHECK(tl::core::skill_target_is_valid(q));
    q.candidate.alive = true;
    TL_CHECK(!tl::core::skill_target_is_valid(q));
    q.dead_wanted = false;
    // `TARGET_UNITTYPE` and `TARGET_SPECIFIC_UNITS`.
    q.candidate.unit_type_matches = false;
    TL_CHECK(!tl::core::skill_target_is_valid(q));
    q.has_property = false;  // with no property the type is not asked
    TL_CHECK(tl::core::skill_target_is_valid(q));
    q.has_property = true;
    q.candidate.unit_type_matches = true;
    q.candidate.specific_unit = false;
    TL_CHECK(!tl::core::skill_target_is_valid(q));
    q.candidate.specific_unit = true;
}

TL_TEST(a_self_or_pet_skill_tests_whom_it_names) {
    using tl::core::SkillTargetQuery;
    SkillTargetQuery q;
    q.target_alignment = 2;
    q.owner_alignment = 1;
    q.owner.alive = true;
    q.owner.is_owner = true;
    q.candidate.alive = true;
    q.candidate.enemy = false;
    // 9 `PETS`: the owner's pet, whatever the sides.
    q.target_type = 9;
    TL_CHECK(!tl::core::skill_target_is_valid(q));
    q.candidate.master_is_owner = true;
    TL_CHECK(tl::core::skill_target_is_valid(q));
    // 10 `SELFANDPETS`: the pet, the owner, or the owner's own master.
    q.target_type = 10;
    TL_CHECK(tl::core::skill_target_is_valid(q));
    q.candidate.master_is_owner = false;
    q.candidate.is_owners_master = true;
    TL_CHECK(tl::core::skill_target_is_valid(q));
    q.candidate.is_owners_master = false;
    TL_CHECK(!tl::core::skill_target_is_valid(q));
    // 3 `SELF`: the owner is tested whoever the candidate is - a dead owner
    // fails, a dead candidate does not matter.
    q.target_type = 3;
    q.candidate.alive = false;
    q.owner.enemy = true;  // `isEnemy` of the owner itself, never in play
    TL_CHECK(tl::core::skill_target_is_valid(q));
    q.owner.alive = false;
    TL_CHECK(!tl::core::skill_target_is_valid(q));
}

TL_TEST(a_live_event_holds_the_run_past_its_animation) {
    // `CSkill::updateSkill`: while an event it started lives, a
    // skill with no `DURATIONOVERRIDEMS` and no loop does not end.
    const AdmFile file = skill_file("Summon", {str("TARGET_TYPE", "SELF")}, {num("MANACOST", 1)});
    const SkillDef def = tl::core::parse_skill_def(file);
    const KnownSkill skill = learned(def);
    const auto lookup = [&skill](const std::string& name) -> const KnownSkill* {
        return name == skill.entry.name ? &skill : nullptr;
    };
    bool alive = true;
    SkillCastContext context;
    context.events_alive = [&alive](const std::string& name, uint32_t) {
        return alive && name == "Summon";
    };
    SkillCaster caster;
    caster.mana = 10.0f;
    SkillRunner runner;
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, context)),
                static_cast<int>(SkillCastResult::Cast));
    runner.update(1.5f, context, lookup);  // past the 1 s animation
    TL_CHECK(runner.is_running("Summon"));
    alive = false;
    runner.update(0.1f, context, lookup);
    TL_CHECK(!runner.is_running("Summon"));
}

TL_TEST(a_continuous_looping_skill_does_not_end_itself) {
    const AdmFile file = skill_file(
        "Beam", {str("TARGET_TYPE", "SELF")},
        {num("MANACOST", 1), num("MINIMUMTIMEMS", 100), boolean("CONTINUOUSLOOPING", true)});
    const SkillDef def = tl::core::parse_skill_def(file);
    TL_CHECK(def.levels.at(0).continuous_looping);
    const KnownSkill skill = learned(def);
    const auto lookup = [&skill](const std::string& name) -> const KnownSkill* {
        return name == skill.entry.name ? &skill : nullptr;
    };

    SkillCaster caster;
    caster.mana = 100.0f;
    const SkillCastContext context;
    SkillRunner runner;
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, context)),
                static_cast<int>(SkillCastResult::Cast));
    for (int i = 0; i < 100; ++i) runner.update(0.1f, context, lookup);
    TL_CHECK(runner.is_running("Beam"));

    // Only an explicit stop ends it.
    runner.stop(skill, context);
    TL_CHECK(!runner.is_running("Beam"));
}

TL_TEST(casting_a_running_skill_starts_a_second_copy) {
    // `CSkillManager::executeSkill(wstring)`: with every copy running, a clone is
    // added and started, not refused.
    const AdmFile file = skill_file("Beam", {str("TARGET_TYPE", "SELF")},
                                    {num("MANACOST", 1), num("MINIMUMTIMEMS", 5000)});
    const SkillDef def = tl::core::parse_skill_def(file);
    const KnownSkill skill = learned(def);
    const auto lookup = [&skill](const std::string& name) -> const KnownSkill* {
        return name == skill.entry.name ? &skill : nullptr;
    };

    SkillCaster caster;
    caster.mana = 100.0f;
    SkillCastContext context;
    std::vector<uint32_t> reset;
    bool first_alive = true;
    context.events_alive = [&first_alive](const std::string&, uint32_t run) {
        return run == 0 && first_alive;
    };
    context.reset_events = [&reset](const std::string&, uint32_t run) { reset.push_back(run); };
    SkillRunner runner;
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, context)),
                static_cast<int>(SkillCastResult::Cast));
    TL_CHECK_EQ(runner.state("Beam")->copy, 0u);
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, context)),
                static_cast<int>(SkillCastResult::Cast));
    TL_CHECK_EQ(runner.state("Beam")->copy, 1u);
    TL_CHECK_EQ(runner.entries().size(), size_t{2});

    // The second copy ends on its own clock; the first is held by its event.
    runner.update(1.0f, context, lookup);
    TL_CHECK(runner.is_running("Beam"));
    TL_CHECK_EQ(reset.size(), size_t{1});
    TL_CHECK_EQ(reset.at(0), 1u);
    first_alive = false;
    runner.update(0.1f, context, lookup);
    TL_CHECK(!runner.is_running("Beam"));
    TL_CHECK_EQ(reset.size(), size_t{2});
    TL_CHECK_EQ(reset.at(1), 0u);

    // Both idle again: the next cast takes the first copy, not a third.
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, context)),
                static_cast<int>(SkillCastResult::Cast));
    TL_CHECK_EQ(runner.state("Beam")->copy, 0u);
    TL_CHECK_EQ(runner.entries().size(), size_t{2});
}

TL_TEST(an_event_block_starts_once_per_run_unless_it_may_clone) {
    // `CSkill::triggerEvent` skips a started block unless `CAN_CLONE` (default:
    // `EVENT_TRIGGER` no, `EVENT_UNITHIT` yes).
    AdmFile file = skill_file("Bolt", {str("TARGET_TYPE", "SELF")},
                              {num("MANACOST", 1), num("MINIMUMTIMEMS", 5000)});
    AdmSection trigger;
    trigger.name = "EVENT_TRIGGER";
    trigger.records.push_back(str("FILE", "media/skills/x/bolt.layout"));
    file.root.children.at(0).children.push_back(trigger);
    AdmSection hit;
    hit.name = "EVENT_UNITHIT";
    hit.records.push_back(str("FILE", "media/skills/x/hit.layout"));
    file.root.children.at(0).children.push_back(hit);
    const SkillDef def = tl::core::parse_skill_def(file);
    const KnownSkill skill = learned(def);
    const auto lookup = [&skill](const std::string& name) -> const KnownSkill* {
        return name == skill.entry.name ? &skill : nullptr;
    };

    tl::core::EffectManager target;
    SkillCastContext context;
    context.target = &target;
    std::vector<uint32_t> triggers;
    int hits = 0;
    context.on_event = [&](const KnownSkill&, uint32_t, int group, const AdmSection&,
                           const tl::core::SkillEventTarget& to) {
        if (group == 2) triggers.push_back(to.run);
        if (group == 4) ++hits;
    };
    SkillCaster caster;
    caster.mana = 100.0f;
    SkillRunner runner;
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, context)),
                static_cast<int>(SkillCastResult::Cast));
    TL_CHECK_EQ(runner.trigger_event(2, context, lookup), 0);
    TL_CHECK_EQ(triggers.size(), size_t{1});
    // A second key in the same run: nothing.
    runner.trigger_event(2, context, lookup);
    TL_CHECK_EQ(triggers.size(), size_t{1});

    // A second cast while the first runs: only the new copy's trigger fires.
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, context)),
                static_cast<int>(SkillCastResult::Cast));
    runner.trigger_event(2, context, lookup);
    TL_CHECK_EQ(triggers.size(), size_t{2});
    TL_CHECK_EQ(triggers.at(1), 1u);

    // Hits clone: each one fires.
    SkillCastContext struck = context;
    struck.event_target.has_unit = true;
    struck.event_target.unit = 7;
    struck.event_target.run = 0;
    runner.trigger_skill_event(skill, 4, struck);
    runner.trigger_skill_event(skill, 4, struck);
    TL_CHECK_EQ(hits, 2);

    // A copy that stopped forgets what it started. Recast once a frame has passed: until then
    // the active list holds the stopped copies, and a cast of one is refused.
    runner.stop(skill, context);
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, context)),
                static_cast<int>(SkillCastResult::ChanceFailed));
    runner.update(0.01f, context, lookup);
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, context)),
                static_cast<int>(SkillCastResult::Cast));
    runner.trigger_event(2, context, lookup);
    TL_CHECK_EQ(triggers.size(), size_t{3});
}

TL_TEST(a_block_started_again_resets_and_a_clone_does_not) {
    // `CSkillEvent::startEvent` runs `reset` first, detaching
    // the last start's missiles.
    AdmFile file = skill_file("Bolt", {str("TARGET_TYPE", "SELF")},
                              {num("MANACOST", 1), num("MINIMUMTIMEMS", 5000)});
    AdmSection trigger;
    trigger.name = "EVENT_TRIGGER";
    trigger.records.push_back(str("FILE", "media/skills/x/bolt.layout"));
    file.root.children.at(0).children.push_back(trigger);
    AdmSection hit;
    hit.name = "EVENT_UNITHIT";
    hit.records.push_back(str("FILE", "media/skills/x/hit.layout"));
    file.root.children.at(0).children.push_back(hit);
    const SkillDef def = tl::core::parse_skill_def(file);
    const KnownSkill skill = learned(def);
    const auto lookup = [&skill](const std::string& name) -> const KnownSkill* {
        return name == skill.entry.name ? &skill : nullptr;
    };

    tl::core::EffectManager target;
    SkillCastContext context;
    context.target = &target;
    std::vector<std::string> resets;
    context.reset_event = [&](const std::string& name, uint32_t run, const AdmSection& block) {
        resets.push_back(name + ":" + std::to_string(run) + ":" + block.name);
    };
    SkillCaster caster;
    caster.mana = 100.0f;
    SkillRunner runner;
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, context)),
                static_cast<int>(SkillCastResult::Cast));
    runner.trigger_event(2, context, lookup);
    TL_CHECK_EQ(resets.size(), size_t{1});
    TL_CHECK(resets.at(0) == "Bolt:0:EVENT_TRIGGER");
    // Passed over in the same run: no start, no reset.
    runner.trigger_event(2, context, lookup);
    TL_CHECK_EQ(resets.size(), size_t{1});

    // The first hit starts the block itself; the second is a clone.
    SkillCastContext struck = context;
    struck.event_target.has_unit = true;
    struck.event_target.unit = 7;
    struck.event_target.run = 0;
    runner.trigger_skill_event(skill, 4, struck);
    runner.trigger_skill_event(skill, 4, struck);
    TL_CHECK_EQ(resets.size(), size_t{2});
    TL_CHECK(resets.at(1) == "Bolt:0:EVENT_UNITHIT");

    // Stopped and, a frame later, cast again: the same copy's block starts as itself again.
    runner.stop(skill, context);
    runner.update(0.01f, context, lookup);
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, context)),
                static_cast<int>(SkillCastResult::Cast));
    runner.trigger_event(2, context, lookup);
    TL_CHECK_EQ(resets.size(), size_t{3});
    TL_CHECK(resets.at(2) == "Bolt:0:EVENT_TRIGGER");
}

TL_TEST(an_exclusive_skill_restarts_itself_instead) {
    const AdmFile file =
        skill_file("Shield", {str("TARGET_TYPE", "SELF")},
                   {num("MANACOST", 1), num("MINIMUMTIMEMS", 5000), boolean("EXCLUSIVE", true)});
    const SkillDef def = tl::core::parse_skill_def(file);
    TL_CHECK(def.levels.at(0).exclusive);
    const KnownSkill skill = learned(def);

    SkillCaster caster;
    caster.mana = 100.0f;
    const SkillCastContext context;
    SkillRunner runner;
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, context)),
                static_cast<int>(SkillCastResult::Cast));
    // Exclusive: the running copy is stopped and started again, so the clock
    // goes back to the top rather than the cast being refused.
    runner.update(1.0f, context, [&skill](const std::string& name) -> const KnownSkill* {
        return name == skill.entry.name ? &skill : nullptr;
    });
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, context)),
                static_cast<int>(SkillCastResult::Cast));
    const tl::core::SkillRunState* state = runner.state("Shield");
    TL_CHECK(state != nullptr);
    TL_CHECK(std::fabs(state->elapsed) < 0.0005f);
}

TL_TEST(cooling_time_is_zero_for_a_skill_that_never_ran) {
    SkillRunner runner;
    TL_CHECK(!runner.is_cooling("Nothing"));
    TL_CHECK(std::fabs(runner.cooling_time("Nothing")) < 0.0005f);
    TL_CHECK(runner.state("Nothing") == nullptr);
}

TL_TEST(a_disabled_skill_fails_requirements) {
    const AdmFile file = skill_file("SelfBuff", {str("TARGET_TYPE", "SELF")}, {});
    const SkillDef def = tl::core::parse_skill_def(file);
    KnownSkill skill = learned(def);
    const SkillCaster caster;

    // Enabled by default: passes requirements
    TL_CHECK(skill.enabled);
    TL_CHECK(tl::core::requirements_met(skill, 1, caster));

    // Disabled: fails requirements.
    skill.enabled = false;
    TL_CHECK(!tl::core::requirements_met(skill, 1, caster));
}

namespace {

UnitThemeBook champion_theme_book() {
    AdmFile file;
    file.ok = true;
    file.root.records.push_back(str("NAME", "CHAMPION"));
    UnitThemeBook book;
    book.reload(std::vector<AdmFile>{file});
    return book;
}

}  // namespace

TL_TEST(theme_required_passes_only_with_the_theme) {
    const UnitThemeBook book = champion_theme_book();

    const AdmFile file =
        skill_file("ThemeSkill", {str("TARGET_TYPE", "SELF")}, {str("THEMEREQUIRED", "CHAMPION")});
    const SkillDef def = tl::core::parse_skill_def(file);
    const KnownSkill skill = learned(def);

    SkillCaster caster;
    caster.theme_book = &book;

    // 1. Without themes (caster.themes == nullptr): fails
    TL_CHECK(!tl::core::requirements_met(skill, 1, caster));

    // 2. With empty UnitThemeSet: fails
    UnitThemeSet themes;
    caster.themes = &themes;
    TL_CHECK(!tl::core::requirements_met(skill, 1, caster));

    // 3. With theme on own_themes: passes
    themes.add_unit_theme(book.get_unit_theme("CHAMPION"));
    TL_CHECK(tl::core::requirements_met(skill, 1, caster));

    // 4. Remove from own_themes, add through effect_themes: passes
    themes.remove_unit_theme(book.get_unit_theme("CHAMPION"));
    themes.update({book.get_unit_theme("CHAMPION")});
    TL_CHECK(tl::core::requirements_met(skill, 1, caster));
}

TL_TEST(not_themed_fails_with_the_theme) {
    const UnitThemeBook book = champion_theme_book();

    const AdmFile file =
        skill_file("ThemeSkill", {str("TARGET_TYPE", "SELF")}, {str("NOTTHEMED", "CHAMPION")});
    const SkillDef def = tl::core::parse_skill_def(file);
    const KnownSkill skill = learned(def);

    SkillCaster caster;
    caster.theme_book = &book;

    // 1. Without themes (caster.themes == nullptr): passes
    TL_CHECK(tl::core::requirements_met(skill, 1, caster));

    // 2. With empty UnitThemeSet: passes
    UnitThemeSet themes;
    caster.themes = &themes;
    TL_CHECK(tl::core::requirements_met(skill, 1, caster));

    // 3. With theme on own_themes: fails
    themes.add_unit_theme(book.get_unit_theme("CHAMPION"));
    TL_CHECK(!tl::core::requirements_met(skill, 1, caster));

    // 4. Remove from own_themes, add through effect_themes: fails
    themes.remove_unit_theme(book.get_unit_theme("CHAMPION"));
    themes.update({book.get_unit_theme("CHAMPION")});
    TL_CHECK(!tl::core::requirements_met(skill, 1, caster));
}

TL_TEST(theme_gate_without_a_book_still_refuses) {
    const UnitThemeBook book = champion_theme_book();

    UnitThemeSet themes;
    themes.add_unit_theme(book.get_unit_theme("CHAMPION"));

    SkillCaster caster;
    caster.theme_book = nullptr;  // no book attached
    caster.themes = &themes;

    const AdmFile file_req =
        skill_file("ReqSkill", {str("TARGET_TYPE", "SELF")}, {str("THEMEREQUIRED", "CHAMPION")});
    const SkillDef def_req = tl::core::parse_skill_def(file_req);
    const KnownSkill skill_req = learned(def_req);
    TL_CHECK(!tl::core::requirements_met(skill_req, 1, caster));

    const AdmFile file_not =
        skill_file("NotSkill", {str("TARGET_TYPE", "SELF")}, {str("NOTTHEMED", "CHAMPION")});
    const SkillDef def_not = tl::core::parse_skill_def(file_not);
    const KnownSkill skill_not = learned(def_not);
    TL_CHECK(!tl::core::requirements_met(skill_not, 1, caster));
}

TL_TEST(unknown_theme_name_skips_the_gate) {
    UnitThemeBook book;
    // book does not contain UNKNOWN_THEME

    UnitThemeSet themes;
    SkillCaster caster;
    caster.theme_book = &book;
    caster.themes = &themes;

    const AdmFile file_req = skill_file("ReqSkill", {str("TARGET_TYPE", "SELF")},
                                        {str("THEMEREQUIRED", "UNKNOWN_THEME")});
    const SkillDef def_req = tl::core::parse_skill_def(file_req);
    const KnownSkill skill_req = learned(def_req);
    // Unknown theme resolves to nullptr in original, skipping gate:
    TL_CHECK(tl::core::requirements_met(skill_req, 1, caster));

    const AdmFile file_not =
        skill_file("NotSkill", {str("TARGET_TYPE", "SELF")}, {str("NOTTHEMED", "UNKNOWN_THEME")});
    const SkillDef def_not = tl::core::parse_skill_def(file_not);
    const KnownSkill skill_not = learned(def_not);
    TL_CHECK(tl::core::requirements_met(skill_not, 1, caster));
}

}  // namespace

TL_TEST(stop_all_without_force_only_asks_the_run_to_end) {
    // `CSkillManager::stopAllSkills` -> `CSkill::attemptToStopSkill`: the
    // countdown cut to 0.001; the run lasts while an event lives.
    const AdmFile file = skill_file("Spit", {str("TARGET_TYPE", "SELF")}, {num("MANACOST", 1)});
    const SkillDef def = tl::core::parse_skill_def(file);
    const KnownSkill skill = learned(def);
    const auto lookup = [&skill](const std::string& name) -> const KnownSkill* {
        return name == skill.entry.name ? &skill : nullptr;
    };
    bool alive = true;
    bool can_stop = true;
    SkillCastContext context;
    context.events_alive = [&alive](const std::string&, uint32_t) { return alive; };
    context.events_can_stop = [&can_stop](const std::string&, uint32_t) { return can_stop; };
    SkillCaster caster;
    caster.mana = 10.0f;
    SkillRunner runner;
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, context)),
                static_cast<int>(SkillCastResult::Cast));

    // An event that cannot stop leaves the countdown alone.
    can_stop = false;
    runner.stop_all(context, lookup, /*force=*/false, /*passives=*/false);
    TL_CHECK(runner.is_running("Spit"));
    TL_CHECK(0.5f < runner.state("Spit")->minimum_time_remaining);

    can_stop = true;
    runner.stop_all(context, lookup, /*force=*/false, /*passives=*/false);
    TL_CHECK(runner.is_running("Spit"));
    TL_CHECK_EQ(runner.state("Spit")->minimum_time_remaining, 0.001f);
    runner.update(0.1f, context, lookup);
    TL_CHECK(runner.is_running("Spit"));  // the live event holds it
    alive = false;
    runner.update(0.1f, context, lookup);
    TL_CHECK(!runner.is_running("Spit"));
}

TL_TEST(stop_all_with_force_stops_at_once_and_spares_a_passive_unless_asked) {
    const AdmFile active_file =
        skill_file("Bolt", {str("TARGET_TYPE", "SELF")}, {num("MANACOST", 1)});
    const AdmFile passive_file = skill_file("Aura", {str("TARGET_TYPE", "SELF")}, {});
    const SkillDef active_def = tl::core::parse_skill_def(active_file);
    SkillDef passive_def = tl::core::parse_skill_def(passive_file);
    passive_def.activation = tl::core::SkillActivation::Passive;
    const KnownSkill active = learned(active_def);
    const KnownSkill passive = learned(passive_def);
    TL_CHECK(passive.passive());
    const auto lookup = [&](const std::string& name) -> const KnownSkill* {
        if (name == active.entry.name) return &active;
        if (name == passive.entry.name) return &passive;
        return nullptr;
    };
    SkillCastContext context;
    context.events_alive = [](const std::string&, uint32_t) { return true; };
    SkillCaster caster;
    caster.mana = 10.0f;
    SkillRunner runner;
    TL_CHECK_EQ(static_cast<int>(runner.cast(active, caster, context)),
                static_cast<int>(SkillCastResult::Cast));
    TL_CHECK(runner.execute(passive, 1, caster, context));

    runner.stop_all(context, lookup, /*force=*/true, /*passives=*/false);
    TL_CHECK(!runner.is_running("Bolt"));
    TL_CHECK(runner.is_running("Aura"));
    runner.stop_all(context, lookup, /*force=*/true, /*passives=*/true);
    TL_CHECK(!runner.is_running("Aura"));
}

// `CHARGES`: `CSkill::requirementsMet` refuses a skill with none left;
// `CSkillManager::decrementSkillCharges` takes one an execution.
TL_TEST(a_skill_with_charges_runs_that_many_times) {
    const AdmFile file = skill_file("Dig", {str("TARGET_TYPE", "SELF")}, {});
    const SkillDef def = tl::core::parse_skill_def(file);
    KnownSkill skill = learned(def);
    skill.charges = 2;
    const SkillCastContext context;
    const SkillCaster caster;
    SkillRunner runner;

    TL_CHECK(runner.can_be_executed(skill, 1, caster));
    TL_CHECK(runner.execute(skill, 1, caster, context));
    TL_CHECK_EQ(skill.charges, 1);
    TL_CHECK(runner.execute(skill, 1, caster, context));
    TL_CHECK_EQ(skill.charges, 0);
    TL_CHECK(!tl::core::requirements_met(skill, 1, caster));
    TL_CHECK(!runner.can_be_executed(skill, 1, caster));
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, context)),
                static_cast<int>(SkillCastResult::RequirementsNotMet));

    // -1 is no limit, and is not counted.
    KnownSkill free_skill = learned(def);
    TL_CHECK_EQ(free_skill.charges, -1);
    TL_CHECK(runner.execute(free_skill, 1, caster, context));
    TL_CHECK_EQ(free_skill.charges, -1);
    // `executeSkill` asks nothing of the count: a proc still runs
    // a skill at 0, and it stays there.
    TL_CHECK(runner.execute(skill, 1, caster, context));
    TL_CHECK_EQ(skill.charges, 0);
}

// `CSkillManager::getPositionForSkill`: what is done with the two draws.
TL_TEST(a_position_skill_is_executed_at_its_own_point) {
    const AdmFile file = skill_file("Spot", {str("TARGET_TYPE", "POSITION")}, {});
    const SkillDef def = tl::core::parse_skill_def(file);
    tl::core::RunicRandom rolls(7);
    const std::optional<tl::core::Vec3> at = tl::core::position_for_skill(
        def.levels.at(0), tl::core::Vec3{1.0f, 2.0f, 3.0f}, nullptr, nullptr, rolls);
    TL_CHECK(at.has_value());
    TL_CHECK_EQ(at->x, 1.0f);
    TL_CHECK_EQ(at->y, 2.0f);
    TL_CHECK_EQ(at->z, 3.0f);

    const AdmFile self_file = skill_file("Self", {str("TARGET_TYPE", "SELF")}, {});
    const SkillDef self_def = tl::core::parse_skill_def(self_file);
    TL_CHECK(!tl::core::position_for_skill(self_def.levels.at(0), tl::core::Vec3{}, nullptr,
                                           nullptr, rolls)
                  .has_value());
}

TL_TEST(a_random_position_is_the_random_range_from_the_owners_feet) {
    // `RANDOMRANGE` 2 (the default), no unit: the point lies exactly 2 from the owner's feet.
    const AdmFile file = skill_file("Spawn", {str("TARGET_TYPE", "POSITIONRANDOM")}, {});
    const SkillDef def = tl::core::parse_skill_def(file);
    tl::core::SkillPositionUnit owner;
    owner.centre = tl::core::Vec3{10.0f, 1.5f, -4.0f};
    owner.radius = 0.5f;
    tl::core::RunicRandom rolls(7);
    tl::core::RunicRandom same(7);
    const float rx = same.between(-1.0f, 1.0f);
    const float ry = same.between(-1.0f, 1.0f);
    const float length = std::sqrt(rx * rx + ry * ry);

    const std::optional<tl::core::Vec3> at = tl::core::position_for_skill(
        def.levels.at(0), tl::core::Vec3{0.0f, 0.0f, 0.0f}, &owner, nullptr, rolls);
    TL_CHECK(at.has_value());
    TL_CHECK(std::fabs(at->x - (10.0f + rx / length * 2.0f)) < 0.0001f);
    TL_CHECK(std::fabs(at->y - (1.0f + ry / length * 2.0f)) < 0.0001f);
    TL_CHECK_EQ(at->z, -4.0f);
    // Two draws, no more.
    TL_CHECK_EQ(rolls.between(0.0f, 1.0f), same.between(0.0f, 1.0f));

    // A point with a z of its own swamps the draws: the offset runs along z.
    rolls.set_seed(7);
    const std::optional<tl::core::Vec3> along = tl::core::position_for_skill(
        def.levels.at(0), tl::core::Vec3{0.0f, 0.0f, 500.0f}, &owner, nullptr, rolls);
    TL_CHECK(along.has_value());
    TL_CHECK(std::fabs(along->z - (-4.0f + 2.0f)) < 0.001f);
    TL_CHECK(std::fabs(along->x - 10.0f) < 0.01f);
}

TL_TEST(a_random_position_with_a_unit_starts_from_the_reach_towards_it) {
    // `RANGE` 2, `RANGEMIN` 1: a unit 10 away along x is reached for 2 of
    // them, one 0.5 away for 1.
    const AdmFile file =
        skill_file("Spawn", {str("TARGET_TYPE", "POSITIONRANDOM")},
                   {flt("RANGE", 2.0f), flt("RANGEMIN", 1.0f), flt("RANDOMRANGE", 0.0f)});
    const SkillDef def = tl::core::parse_skill_def(file);
    tl::core::SkillPositionUnit owner;
    owner.centre = tl::core::Vec3{0.0f, 1.0f, 0.0f};
    owner.radius = 1.0f;
    tl::core::SkillPositionUnit far;
    far.centre = tl::core::Vec3{10.0f, 7.0f, 0.0f};
    tl::core::SkillPositionUnit near;
    near.centre = tl::core::Vec3{0.5f, 1.0f, 0.0f};
    tl::core::RunicRandom rolls(7);
    // `RANDOMRANGE` 0 leaves no offset, whatever is drawn.
    const std::optional<tl::core::Vec3> a =
        tl::core::position_for_skill(def.levels.at(0), tl::core::Vec3{}, &owner, &far, rolls);
    TL_CHECK(a.has_value());
    TL_CHECK_EQ(a->x, 2.0f);
    TL_CHECK_EQ(a->y, 0.0f);  // the owner's feet: the unit's height is not read
    TL_CHECK_EQ(a->z, 0.0f);
    const std::optional<tl::core::Vec3> b =
        tl::core::position_for_skill(def.levels.at(0), tl::core::Vec3{}, &owner, &near, rolls);
    TL_CHECK(b.has_value());
    TL_CHECK_EQ(b->x, 1.0f);
}

TL_TEST(a_flee_position_is_half_the_range_behind_the_owner) {
    const AdmFile file = skill_file("Flee", {str("TARGET_TYPE", "POSITIONFLEE")},
                                    {flt("RANGE", 8.0f), flt("RANGEMIN", 8.0f)});
    const SkillDef def = tl::core::parse_skill_def(file);
    tl::core::SkillPositionUnit owner;
    owner.centre = tl::core::Vec3{0.0f, 1.0f, 0.0f};
    owner.radius = 1.0f;
    owner.forward = tl::core::Vec3{0.0f, 0.0f, 1.0f};
    tl::core::SkillPositionUnit threat;
    threat.centre = tl::core::Vec3{5.0f, 1.0f, 0.0f};
    tl::core::RunicRandom rolls(7);
    // `RANGE` equal to `RANGEMIN`: the band is 0, so the draws count for nothing.
    const std::optional<tl::core::Vec3> away =
        tl::core::position_for_skill(def.levels.at(0), tl::core::Vec3{}, &owner, &threat, rolls);
    TL_CHECK(away.has_value());
    TL_CHECK_EQ(away->x, -4.0f);
    TL_CHECK_EQ(away->y, 0.0f);
    TL_CHECK_EQ(away->z, 0.0f);
    // No unit: back along the way the owner faces.
    const std::optional<tl::core::Vec3> back =
        tl::core::position_for_skill(def.levels.at(0), tl::core::Vec3{}, &owner, nullptr, rolls);
    TL_CHECK(back.has_value());
    TL_CHECK_EQ(back->x, 0.0f);
    TL_CHECK_EQ(back->y, 0.0f);
    TL_CHECK_EQ(back->z, -4.0f);
}

// `castSkill` asks `requirementsMet` for a `NORMAL` skill; a skill of another
// activation type is refused there, and passes when nothing is asked.
TL_TEST(cast_asks_for_a_normal_skill_and_refuses_a_proc) {
    AdmFile file = skill_file("Proc", {str("TARGET_TYPE", "SELF")}, {num("MANACOST", 1)});
    file.root.records.at(1) = str("ACTIVATION_TYPE", "PROC");
    const SkillDef def = tl::core::parse_skill_def(file);
    const KnownSkill skill = learned(def);
    SkillCaster caster;
    caster.mana = 100.0f;
    TL_CHECK(tl::core::requirements_met(skill, 1, caster));
    TL_CHECK(!tl::core::requirements_met(skill, 1, caster, tl::core::SkillActivation::Normal));
    SkillRunner runner;
    const SkillCastContext context;
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, context)),
                static_cast<int>(SkillCastResult::RequirementsNotMet));
}

// `CAN_BE_SILENCED` (`CSkill +0x48`, true unless authored) gates the `SILENCE` test.
TL_TEST(a_silenced_caster_still_meets_a_skill_that_cannot_be_silenced) {
    const AdmFile plain = skill_file("Bolt", {str("TARGET_TYPE", "SELF")}, {});
    const AdmFile free_of =
        skill_file("Shout", {str("TARGET_TYPE", "SELF"), boolean("CAN_BE_SILENCED", false)}, {});
    const SkillDef plain_def = tl::core::parse_skill_def(plain);
    const SkillDef free_def = tl::core::parse_skill_def(free_of);
    TL_CHECK(plain_def.can_be_silenced);
    TL_CHECK(!free_def.can_be_silenced);
    tl::core::EffectManager silence;
    silence.add_effect(tl::core::value_effect(tl::core::kEffectTypeSilence,
                                              tl::core::kDurationAlways, 1.0f,
                                              tl::core::EffectActivation::Passive));
    SkillCaster caster;
    caster.effects = &silence;
    TL_CHECK(!tl::core::requirements_met(learned(plain_def), 1, caster));
    TL_CHECK(tl::core::requirements_met(learned(free_def), 1, caster));
}

// `stopAllSkills(force, keep_on_death, passives)`: the middle flag, set by
// `die`, spares a `DONT_STOP_ON_DEATH` skill.
TL_TEST(a_death_stop_spares_a_skill_that_does_not_stop_on_death) {
    const AdmFile file =
        skill_file("Aura", {str("TARGET_TYPE", "SELF"), boolean("DONT_STOP_ON_DEATH", true)},
                   {num("MANACOST", 1), num("MINIMUMTIMEMS", 5000)});
    const SkillDef def = tl::core::parse_skill_def(file);
    TL_CHECK(def.dont_stop_on_death);
    const KnownSkill skill = learned(def);
    const auto lookup = [&skill](const std::string& name) -> const KnownSkill* {
        return name == skill.entry.name ? &skill : nullptr;
    };
    SkillCaster caster;
    caster.mana = 100.0f;
    const SkillCastContext context;
    SkillRunner runner;
    TL_CHECK_EQ(static_cast<int>(runner.cast(skill, caster, context)),
                static_cast<int>(SkillCastResult::Cast));
    runner.stop_all(context, lookup, /*force=*/true, /*passives=*/false, /*keep_on_death=*/true);
    TL_CHECK(runner.is_running("Aura"));
    runner.stop_all(context, lookup, /*force=*/true, /*passives=*/false);
    TL_CHECK(!runner.is_running("Aura"));
}
