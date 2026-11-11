// Procs and a weapon's own skills: `CBaseUnit::activateEffect`, `CCharacter::executeProcs`
// with its inventory's, and the points `doWeaponSkill` and `doWeaponProcs` cast from.

#include "core/weapon_procs.h"
#include "core/effect_manager.h"
#include "core/effects.h"
#include "core/random.h"
#include "core/stats.h"

#include <cmath>
#include <map>
#include <vector>
#include "test_support.h"

namespace {

using tl::core::Effect;
using tl::core::EffectActivation;
using tl::core::EffectManager;
using tl::core::ProcCast;
using tl::core::ProcHolder;
using tl::core::RunicRandom;
using tl::core::SkillCopies;
using tl::core::SkillFileFacts;
using tl::core::StatBook;
using tl::core::Vec3;

bool near_to(float a, float b) { return std::fabs(a - b) < 1e-5f; }

/// Three skill files: a ten-level hidden one, a one-level one, and nothing else.
std::optional<SkillFileFacts> files(const std::string& name) {
    SkillFileFacts facts;
    if (name == "Spectral Wash" || name == "SPECTRAL WASH") {
        facts.name = "Spectral Wash";
        facts.level_count = 10;
        facts.hidden = true;
        return facts;
    }
    if (name == "GOBLIN FIRE BALL") {
        facts.name = "GOBLIN FIRE BALL";
        facts.level_count = 0;  // no `[LEVELn]`: one level
        return facts;
    }
    return std::nullopt;
}

Effect cast_effect(int type, const std::string& skill, float chance, uint32_t level,
                   EffectActivation list = EffectActivation::Dynamic) {
    Effect effect;
    effect.type = type;
    effect.name = skill;
    effect.activation = list;
    effect.values[0] = chance;
    effect.values[1] = chance;
    effect.level = level;
    return effect;
}

}  // namespace

// `type - 0x4a` below 0x20 and a bit of 0x80000087.
TL_TEST(procs_the_five_cast_skill_types_teach) {
    for (int type : {0x4a, 0x4b, 0x4c, 0x51, 0x69}) TL_CHECK(tl::core::effect_teaches_skill(type));
    for (int type : {0x49, 0x4d, 0x50, 0x52, 0x68, 0x6a, 0x2e, 0}) {
        TL_CHECK(!tl::core::effect_teaches_skill(type));
    }
}

// -1 is 1, the rest held to the level count (1 for none), 0 kept.
TL_TEST(procs_a_copy_takes_its_level_within_the_file) {
    TL_CHECK_EQ(tl::core::skill_level_from_manager(0xffffffffu, 10), 1u);
    TL_CHECK_EQ(tl::core::skill_level_from_manager(3, 10), 3u);
    TL_CHECK_EQ(tl::core::skill_level_from_manager(12, 10), 10u);
    TL_CHECK_EQ(tl::core::skill_level_from_manager(4, 0), 1u);
    TL_CHECK_EQ(tl::core::skill_level_from_manager(0, 10), 0u);
}

// `addSkill(name, true)` never reuses a copy; `setSkillLevel` re-levels them all;
// `getSkill(name, level)` wants the level exactly.
TL_TEST(procs_each_add_is_a_new_copy_and_relevels_its_name) {
    SkillCopies skills;
    TL_CHECK(skills.add("SPECTRAL WASH", 2, files) != nullptr);
    TL_CHECK(skills.add("Spectral Wash", 5, files) != nullptr);
    TL_CHECK_EQ(skills.copies().size(), 2u);
    TL_CHECK_EQ(skills.copies()[0].level, 5u);
    TL_CHECK_EQ(skills.copies()[1].level, 5u);
    TL_CHECK(skills.copies()[0].hidden);
    TL_CHECK(skills.find("spectral wash", 5) != nullptr);
    TL_CHECK(skills.find("spectral wash", 2) == nullptr);
    // No file: nothing added.
    TL_CHECK(skills.add("NO SUCH SKILL", 1, files) == nullptr);
    TL_CHECK_EQ(skills.copies().size(), 2u);

    SkillCopies player(/*one_per_name=*/true);
    player.add("Spectral Wash", 2, files);
    player.add("Spectral Wash", 3, files);
    TL_CHECK_EQ(player.copies().size(), 1u);
    TL_CHECK_EQ(player.copies()[0].level, 3u);
}

// the skill at the effect's level, the charges from `MAX`, and the
// level the copy took written back.
TL_TEST(procs_activating_a_cast_skill_effect_teaches_its_skill) {
    StatBook graphs;
    SkillCopies skills;
    Effect effect = cast_effect(tl::core::kEffectCastSkill, "Spectral Wash", 100.0f, 3);
    tl::core::activate_effect(effect, skills, graphs, files);
    TL_CHECK_EQ(skills.copies().size(), 1u);
    TL_CHECK_EQ(skills.copies()[0].level, 3u);
    TL_CHECK_EQ(skills.copies()[0].charges, 100);
    TL_CHECK_EQ(effect.level, 3u);

    // Above the file's ten levels: the copy and the effect both at 10.
    Effect high = cast_effect(tl::core::kEffectCastSkill, "Spectral Wash", 50.0f, 12);
    high.values[1] = 0.0f;  // `MAX` 0: no charges, -1
    tl::core::activate_effect(high, skills, graphs, files);
    TL_CHECK_EQ(high.level, 10u);
    TL_CHECK_EQ(skills.copies()[1].charges, -1);
    // ... and the first copy re-levelled with it, so the first effect's own
    // level no longer finds a copy.
    TL_CHECK(skills.find("Spectral Wash", effect.level) == nullptr);

    // Another type, and a skill no file names: nothing.
    Effect other = cast_effect(0x4d, "Spectral Wash", 100.0f, 3);
    tl::core::activate_effect(other, skills, graphs, files);
    Effect unknown = cast_effect(tl::core::kEffectCastSkill, "NO SUCH SKILL", 100.0f, 4);
    tl::core::activate_effect(unknown, skills, graphs, files);
    TL_CHECK_EQ(skills.copies().size(), 2u);
    TL_CHECK_EQ(unknown.level, 4u);
}

// the character's own lists, one draw per effect of the type, the
// cast when `value(0)` is not below it; the lockout for 0x4a.
TL_TEST(procs_the_character_rolls_each_effect_of_the_type) {
    StatBook graphs;
    SkillCopies skills(true);
    EffectManager own;
    EffectManager::Context context;
    context.on_activate = [&](Effect& e) { tl::core::activate_effect(e, skills, graphs, files); };
    own.set_context(context);
    own.add_effect(cast_effect(tl::core::kEffectCastSkill, "Spectral Wash", 100.0f, 2));
    own.add_effect(cast_effect(tl::core::kEffectCastSkill, "Spectral Wash", 0.0f, 2));
    own.add_effect(cast_effect(tl::core::kEffectCastSkillOnStruck, "Spectral Wash", 100.0f, 2));
    TL_CHECK_EQ(skills.copies().size(), 1u);

    const std::array<ProcHolder, 12> worn{};
    RunicRandom rolls(12345);
    RunicRandom expected(12345);
    std::vector<ProcCast> casts;
    tl::core::execute_procs(tl::core::kEffectCastSkill, ProcHolder{&own, &skills}, worn, graphs,
                            rolls, [&](const ProcCast& c) { casts.push_back(c); });
    // Two effects of the type, two draws; the 100% one always casts, the 0%
    // one only on a draw of 0.
    expected.integer_between(0, 100);
    const bool second = expected.integer_between(0, 100) == 0;
    TL_CHECK_EQ(rolls.state(), expected.state());
    TL_CHECK_EQ(casts.size(), second ? 2u : 1u);
    TL_CHECK(casts[0].lockout);
    TL_CHECK(!casts[0].from_target);
    TL_CHECK_EQ(casts[0].location, -1);
    TL_CHECK(casts[0].skill != nullptr && casts[0].skill->name == "Spectral Wash");

    // No skill manager: no draw at all.
    const uint64_t before = rolls.state();
    tl::core::execute_procs(tl::core::kEffectCastSkill, ProcHolder{&own, nullptr}, worn, graphs,
                            rolls, [&](const ProcCast&) { TL_CHECK(false); });
    TL_CHECK_EQ(rolls.state(), before);
}

// `AT TARGET` starts at the target and sets no lockout.
TL_TEST(procs_cast_skill_at_target_starts_at_the_target) {
    StatBook graphs;
    SkillCopies skills(true);
    EffectManager own;
    EffectManager::Context context;
    context.on_activate = [&](Effect& e) { tl::core::activate_effect(e, skills, graphs, files); };
    own.set_context(context);
    own.add_effect(cast_effect(tl::core::kEffectCastSkillAtTarget, "Spectral Wash", 100.0f, 1,
                               EffectActivation::Passive));
    const std::array<ProcHolder, 12> worn{};
    RunicRandom rolls(7);
    std::vector<ProcCast> casts;
    tl::core::execute_procs(tl::core::kEffectCastSkillAtTarget, ProcHolder{&own, &skills}, worn,
                            graphs, rolls, [&](const ProcCast& c) { casts.push_back(c); });
    TL_CHECK_EQ(casts.size(), 1u);
    TL_CHECK(casts[0].from_target);
    TL_CHECK(!casts[0].lockout);
}

// after the character, each worn item that holds the type and knows
// skills, at its location, without the lockout.
TL_TEST(procs_a_worn_item_casts_from_its_own_manager) {
    StatBook graphs;
    SkillCopies item_skills;
    EffectManager item;
    EffectManager::Context context;
    context.on_activate = [&](Effect& e) {
        tl::core::activate_effect(e, item_skills, graphs, files);
    };
    item.set_context(context);
    item.add_effect(cast_effect(tl::core::kEffectCastSkillOnTarget, "GOBLIN FIRE BALL", 100.0f, 1,
                                EffectActivation::Dynamic));
    std::array<ProcHolder, 12> worn{};
    worn[3] = ProcHolder{&item, &item_skills};
    // Location 5 holds the same effects but no skill manager: passed by.
    worn[5] = ProcHolder{&item, nullptr};
    EffectManager own;
    SkillCopies none(true);
    RunicRandom rolls(99);
    std::vector<ProcCast> casts;
    tl::core::execute_procs(tl::core::kEffectCastSkillOnTarget, ProcHolder{&own, &none}, worn,
                            graphs, rolls, [&](const ProcCast& c) { casts.push_back(c); });
    TL_CHECK_EQ(casts.size(), 1u);
    TL_CHECK_EQ(casts[0].location, 3);
    TL_CHECK(!casts[0].lockout);
    TL_CHECK(!casts[0].from_target);

    // The gate is `hasEffect`, the passive and dynamic lists only: an item whose cast
    // effect sits on the transfer list alone casts nothing, though the roll walks all three.
    EffectManager transfer_only;
    transfer_only.set_context(context);
    transfer_only.add_effect(cast_effect(tl::core::kEffectCastSkillOnTarget, "GOBLIN FIRE BALL",
                                         100.0f, 1, EffectActivation::Transfer));
    std::array<ProcHolder, 12> held{};
    held[3] = ProcHolder{&transfer_only, &item_skills};
    casts.clear();
    tl::core::execute_procs(tl::core::kEffectCastSkillOnTarget, ProcHolder{&own, &none}, held,
                            graphs, rolls, [&](const ProcCast& c) { casts.push_back(c); });
    TL_CHECK_EQ(casts.size(), 0u);
}

// a weapon's own `[EFFECT]`s teach first, then its `[SKILL]`s, then
// its affixes.
TL_TEST(procs_a_weapon_knows_its_skills_in_unit_init_order) {
    StatBook graphs;
    tl::core::ItemStats staff;
    staff.name = "Goblin Staff";
    staff.effects.push_back(
        cast_effect(tl::core::kEffectCastSkillOnStruck, "Spectral Wash", 10.0f, 1));
    tl::core::ItemStats::Skill block;
    block.name = "GOBLIN FIRE BALL";
    block.level = 1;
    staff.skills.push_back(block);
    tl::core::ItemStats::RolledAffix rolled;
    rolled.affix.name = "TEST AFFIX";
    rolled.affix.min_spawn_range = 0;
    rolled.affix.max_spawn_range = 100;
    rolled.affix.effects.push_back(
        cast_effect(tl::core::kEffectCastSkill, "Spectral Wash", 100.0f, 4));
    rolled.level = 4;
    staff.affixes.push_back(rolled);

    const SkillCopies known =
        tl::core::weapon_known_skills(staff, EffectManager::Context{}, graphs, files);
    TL_CHECK_EQ(known.copies().size(), 3u);
    TL_CHECK_EQ(known.copies()[0].name, std::string("Spectral Wash"));
    TL_CHECK_EQ(known.copies()[1].name, std::string("GOBLIN FIRE BALL"));
    TL_CHECK_EQ(known.copies()[2].name, std::string("Spectral Wash"));
    // `doWeaponSkill` runs the enabled, unhidden one only.
    TL_CHECK(!tl::core::weapon_skill_runs(known.copies()[0]));
    TL_CHECK(tl::core::weapon_skill_runs(known.copies()[1]));
    // The affix's copy re-levelled the first: both at the affix's level.
    TL_CHECK_EQ(known.copies()[0].level, known.copies()[2].level);
}

// the hand's reach in x and z, at least 0.6 of the radius,
// along the forward, 0.5 up; the aim 10 farther.
TL_TEST(procs_weapon_skill_points_from_the_hand) {
    const Vec3 at{1.0f, 2.0f, 3.0f};
    const Vec3 forward{0.0f, 0.0f, 1.0f};
    const tl::core::WeaponSkillPoints far =
        tl::core::weapon_skill_points(Vec3{1.3f, 3.5f, 3.4f}, at, 0.5f, forward);
    TL_CHECK(near_to(far.start.x, 1.0f));
    TL_CHECK(near_to(far.start.y, 2.5f));
    TL_CHECK(near_to(far.start.z, 3.5f));
    TL_CHECK(near_to(far.aim.z, 13.5f));
    TL_CHECK(near_to(far.aim.y, 2.5f));
    // A hand nearer than 0.6 of the radius: the radius' 0.6.
    const tl::core::WeaponSkillPoints near =
        tl::core::weapon_skill_points(Vec3{1.1f, 2.0f, 3.0f}, at, 1.0f, forward);
    TL_CHECK(near_to(near.start.z, 3.6f));
}

// the struck unit's place, its radius toward the attacker,
// 1.25 up.
TL_TEST(procs_weapon_proc_point_on_the_struck_unit) {
    const Vec3 p =
        tl::core::weapon_proc_point(Vec3{3.0f, 0.0f, 0.0f}, Vec3{0.0f, 2.0f, 0.0f}, 0.5f);
    TL_CHECK(near_to(p.x, 0.5f));
    TL_CHECK(near_to(p.y, 3.25f));
    TL_CHECK(near_to(p.z, 0.0f));
    // On top of each other: no direction, the target's place 1.25 up.
    const Vec3 q =
        tl::core::weapon_proc_point(Vec3{1.0f, 0.0f, 1.0f}, Vec3{1.0f, 0.0f, 1.0f}, 0.5f);
    TL_CHECK(near_to(q.x, 1.0f));
    TL_CHECK(near_to(q.y, 1.25f));
    TL_CHECK(near_to(q.z, 1.0f));
}
