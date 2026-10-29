// A barrel smashed: `CBreakable::unitInit`, `interact`,
// `update`, `CBaseUnit::selectRandomSkill` and
// `CLevel::findItemWithinView`.
#include "core/breakable.h"
#include "test_support.h"

#include <cmath>
#include <optional>
#include <string>

namespace {

using tl::core::Breakable;
using tl::core::BreakableData;
using tl::core::Vec3;

bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

tl::formats::AdmRecord text(const char* key, const char* value) {
    tl::formats::AdmRecord record;
    record.key = key;
    record.type = tl::formats::AdmType::String;
    record.text = value;
    return record;
}

tl::formats::AdmRecord flag(const char* key, bool value) {
    tl::formats::AdmRecord record;
    record.key = key;
    record.type = tl::formats::AdmType::Bool;
    record.integer = value ? 1u : 0u;
    return record;
}

tl::formats::AdmSection skill(const char* name) {
    tl::formats::AdmSection section;
    section.name = "SKILL";
    section.records.push_back(text("NAME", name));
    return section;
}

/// `media/units/props/Barrel/BARREL.DAT`, as the archive authors it.
tl::formats::AdmFile barrel() {
    tl::formats::AdmFile file;
    file.ok = true;
    file.root.name = "UNIT";
    file.root.records.push_back(text("NAME", "Barrel"));
    file.root.records.push_back(text("RESOURCEDIRECTORY", "media/models/barrel"));
    file.root.records.push_back(text("MESHFILE", "barrel"));
    file.root.records.push_back(text("UNITTYPE", "BREAKABLE"));
    file.root.records.push_back(text("INTERACT_PARTICLE", "BarrelBreak"));
    file.root.records.push_back(text("INTERACT_SOUND", "BarrelBreak"));
    file.root.records.push_back(flag("COLLIDEABLE", true));
    file.root.children.push_back(skill("EXPLOSION TRAP"));
    file.root.children.push_back(skill("SKELETON TRAP"));
    return file;
}

}  // namespace

/// The paths as `CleanPath` leaves them, the collider when none is named, the
/// two names upper-cased, and the skills in file order.
TL_TEST(breakable_reads_the_unit) {
    const BreakableData data = tl::core::read_breakable(barrel());
    TL_CHECK(data.mesh == "media/models/barrel/barrel.mesh");
    TL_CHECK(data.collision == "media/models/collider.mesh");
    TL_CHECK(data.interact_particle == "BARRELBREAK");
    TL_CHECK(data.interact_sound == "BARRELBREAK");
    TL_CHECK(data.shadows);
    TL_CHECK(data.collideable);
    TL_CHECK(data.skills.size() == 2);
    TL_CHECK(data.skills[0] == "EXPLOSION TRAP");
    TL_CHECK(data.skills[1] == "SKELETON TRAP");

    tl::formats::AdmFile bare;
    bare.ok = true;
    bare.root.name = "UNIT";
    const BreakableData none = tl::core::read_breakable(bare);
    TL_CHECK(none.mesh.empty());
    TL_CHECK(none.interact_particle.empty());
    TL_CHECK(none.skills.empty());
}

/// With the skill files known: a block naming none is passed over, a `LEVEL` above the
/// skill's count of `[LEVELn]` blocks is that count, and -1 is 1.
TL_TEST(breakable_skills_known_and_capped) {
    tl::formats::AdmFile file = barrel();
    const auto level = [](const char* value) {
        tl::formats::AdmRecord record;
        record.key = "LEVEL";
        record.type = tl::formats::AdmType::Integer;
        record.integer = static_cast<uint32_t>(std::stoul(value));
        return record;
    };
    file.root.children[0].records.push_back(level("7"));
    file.root.children[1].records.push_back(level("4294967295"));
    file.root.children.push_back(skill("NO SUCH TRAP"));
    file.root.children.push_back(skill("THIRD TRAP"));
    file.root.children[3].records.push_back(level("2"));
    const BreakableData data =
        tl::core::read_breakable(file, [](const std::string& upper) -> std::optional<uint32_t> {
            if (upper == "EXPLOSION TRAP") return 1u;
            if (upper == "SKELETON TRAP") return 0u;
            if (upper == "THIRD TRAP") return 3u;
            return std::nullopt;
        });
    TL_CHECK(data.skills.size() == 3);
    TL_CHECK(data.skills[2] == "THIRD TRAP");
    TL_CHECK(data.skill_levels[0] == 1u);
    TL_CHECK(data.skill_levels[1] == 1u);
    TL_CHECK(data.skill_levels[2] == 2u);
}

/// Every backslash turned, then every doubled slash collapsed - a directory
/// written with a trailing slash does not leave `//` in the path.
TL_TEST(breakable_clean_path_turns_and_collapses) {
    TL_CHECK(tl::core::breakable_clean_path("media\\models\\\\barrel//barrel.mesh") ==
             "media/models/barrel/barrel.mesh");
    TL_CHECK(tl::core::breakable_clean_path("media/models/barrel/") == "media/models/barrel/");
}

/// The first `interact` breaks it and hands back where the skill and the
/// particle go; a second does nothing.
TL_TEST(breakable_breaks_once) {
    const BreakableData data = tl::core::read_breakable(barrel());
    Breakable b;
    tl::core::breakable_unit_init(b, data);
    b.active = true;
    tl::core::breakable_update(b);
    TL_CHECK(b.usable);
    TL_CHECK(b.collision_live);

    tl::core::RunicRandom rng(7u);
    const Vec3 at{10.0f, 2.0f, -4.0f};
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const Vec3 breaker{11.0f, 2.0f, -3.0f};
    const tl::core::BreakableInteraction first =
        tl::core::breakable_interact(b, true, at, up, &breaker, 2, rng);
    TL_CHECK(first.broke);
    TL_CHECK(first.skill == 0 || first.skill == 1);
    // Cast at the position lowered by `+0x194`, the constructor's 0.3.
    TL_CHECK(near(first.skill_at.y, 1.7f));
    TL_CHECK(near(first.skill_at.x, 10.0f));
    TL_CHECK(near(first.particle_at.y, 2.1f));
    // Aimed at the breaker.
    TL_CHECK(near(first.skill_target.x, 11.0f) && near(first.skill_target.z, -3.0f));
    TL_CHECK(!b.usable);
    TL_CHECK(b.used);
    TL_CHECK(!b.collision_live);
    // `update` keeps it out while `+0x218` holds.
    tl::core::breakable_update(b);
    TL_CHECK(!b.collision_live);

    const tl::core::BreakableInteraction second =
        tl::core::breakable_interact(b, true, at, up, nullptr, 2, rng);
    TL_CHECK(!second.broke);
}

/// `+0x22c` is set before the level is asked for: outside a level the call
/// fails and the breakable is spent all the same.
TL_TEST(breakable_outside_a_level_is_spent) {
    Breakable b;
    tl::core::RunicRandom rng(1u);
    const tl::core::BreakableInteraction out =
        tl::core::breakable_interact(b, false, Vec3{}, Vec3{0.0f, 1.0f, 0.0f}, nullptr, 2, rng);
    TL_CHECK(!out.broke);
    TL_CHECK(b.interacted);
    TL_CHECK(b.usable);
    TL_CHECK(!tl::core::breakable_interact(b, true, Vec3{}, Vec3{0.0f, 1.0f, 0.0f}, nullptr, 2, rng)
                  .broke);
}

/// No known skill: no draw and -1. Otherwise exactly one `g_Rand` draw.
TL_TEST(breakable_select_random_skill_draws_once) {
    tl::core::RunicRandom rng(12345u);
    tl::core::RunicRandom copy(12345u);
    TL_CHECK(tl::core::select_random_skill(0, rng) == -1);
    TL_CHECK(rng.step() == copy.step());

    tl::core::RunicRandom a(99u);
    tl::core::RunicRandom b(99u);
    const int32_t pick = tl::core::select_random_skill(2, a);
    const float roll = b.between(0.0f, 1.0f);
    TL_CHECK(pick == (roll < 0.5f ? 0 : 1));
    TL_CHECK(a.step() == b.step());
}

/// Nobody broke it: the skill is aimed at the breakable itself.
TL_TEST(breakable_skill_aims_at_itself_without_a_breaker) {
    Breakable b;
    tl::core::RunicRandom rng(3u);
    const Vec3 at{4.0f, 1.0f, 2.0f};
    const tl::core::BreakableInteraction out =
        tl::core::breakable_interact(b, true, at, Vec3{0.0f, 1.0f, 0.0f}, nullptr, 1, rng);
    TL_CHECK(out.broke);
    TL_CHECK(out.skill == 0);
    TL_CHECK(near(out.skill_target.x, 4.0f) && near(out.skill_target.y, 1.0f));
}

/// `SCALE + randomBetween(0, SCALE_VARIATION)`; no variation, no draw.
TL_TEST(breakable_scale_draws_only_with_a_variation) {
    BreakableData data;
    tl::core::RunicRandom rng(5u);
    tl::core::RunicRandom copy(5u);
    TL_CHECK(near(tl::core::breakable_scale(data, rng), 1.0f));
    TL_CHECK(rng.step() == copy.step());
    data.scale = 2.0f;
    data.scale_variation = 0.5f;
    const float s = tl::core::breakable_scale(data, rng);
    TL_CHECK(s >= 2.0f && s <= 2.5f);
}

/// Sample 0x36 and journal statistic 10 only for a player and a skill that is
/// not `DEFENSE`, and only when it started.
TL_TEST(breakable_trap_noticed_by_a_player) {
    TL_CHECK(tl::core::breakable_trap_noticed(true, true, 0));
    TL_CHECK(tl::core::breakable_trap_noticed(true, true, 1));
    TL_CHECK(!tl::core::breakable_trap_noticed(true, true, 2));
    TL_CHECK(!tl::core::breakable_trap_noticed(false, true, 0));
    TL_CHECK(!tl::core::breakable_trap_noticed(true, false, 0));
}

/// In front within 60 degrees and the reach, or within 1.5 whatever the angle;
/// the height is not looked at.
TL_TEST(breakable_item_within_view) {
    tl::core::ViewFrame frame;
    frame.position = Vec3{0.0f, 0.0f, 0.0f};
    TL_CHECK(tl::core::item_within_view(frame, Vec3{0.0f, 0.0f, 2.0f}, 60.0f, 2.5f));
    TL_CHECK(tl::core::item_within_view(frame, Vec3{0.0f, 7.0f, 2.0f}, 60.0f, 2.5f));
    TL_CHECK(!tl::core::item_within_view(frame, Vec3{0.0f, 0.0f, 3.0f}, 60.0f, 2.5f));
    TL_CHECK(!tl::core::item_within_view(frame, Vec3{0.0f, 0.0f, -2.0f}, 60.0f, 2.5f));
    // Behind, but within 1.5: in.
    TL_CHECK(tl::core::item_within_view(frame, Vec3{0.0f, 0.0f, -1.0f}, 60.0f, 2.5f));
    // 45 degrees to the side is in; 70 is out.
    TL_CHECK(tl::core::item_within_view(frame, Vec3{1.0f, 0.0f, 1.0f}, 60.0f, 2.5f));
    const float s = std::sin(70.0f / 57.295826f) * 2.0f, c = std::cos(70.0f / 57.295826f) * 2.0f;
    TL_CHECK(!tl::core::item_within_view(frame, Vec3{s, 0.0f, c}, 60.0f, 2.5f));
    // Turned to face +x: a quarter turn about y.
    frame.orientation = tl::core::OgreQuaternion{0.70710677f, 0.0f, 0.70710677f, 0.0f};
    TL_CHECK(tl::core::item_within_view(frame, Vec3{2.0f, 0.0f, 0.0f}, 60.0f, 2.5f));
    TL_CHECK(!tl::core::item_within_view(frame, Vec3{0.0f, 0.0f, 2.0f}, 60.0f, 2.5f));
}

/// `(scale * REACH_BONUS + 0.2 + strike range) * RANGE_MULTIPLIER + 1`.
TL_TEST(breakable_attack_view_reach) {
    TL_CHECK(near(tl::core::attack_view_reach(1.0f, 0.75f, 0.6f, 1.0f), 2.55f));
    TL_CHECK(near(tl::core::attack_view_reach(2.0f, 0.75f, 0.6f, 2.0f), 5.6f));
}

/// Only a usable, active, visible one is a candidate.
TL_TEST(breakable_candidate_filter) {
    Breakable b;
    b.active = true;
    TL_CHECK(tl::core::breakable_candidate(b, true));
    TL_CHECK(!tl::core::breakable_candidate(b, false));
    b.usable = false;
    TL_CHECK(!tl::core::breakable_candidate(b, true));
    b.usable = true;
    b.active = false;
    TL_CHECK(!tl::core::breakable_candidate(b, true));
}
