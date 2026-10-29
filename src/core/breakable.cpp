#include "core/breakable.h"
#include "core/runic_math.h"

#include "core/randomizer.h"

#include <cctype>
#include <cmath>

namespace tl::core {

namespace {

// PORT: ASCII case folding, the case-blind compare of names in the port's narrow strings.
std::string upper(std::string text) {
    for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

}  // namespace

std::string breakable_clean_path(const std::string& path) {
    // `FILESYSTEM::CleanPath`: every `\` to
    // `/`, then every `//` to `/`, each from the front until none is left.
    std::string out = path;
    for (char& c : out) {
        if (c == '\\') c = '/';
    }
    for (size_t at = out.find("//"); at != std::string::npos; at = out.find("//")) {
        out.replace(at, 2, "/");
    }
    return out;
}

BreakableData read_breakable(const tl::formats::AdmFile& unit,
                             const BreakableSkillLevels& skill_levels) {
    // `CBreakable::unitInit`.
    BreakableData data;
    const std::string directory = unit.string_or("RESOURCEDIRECTORY", "");
    const std::string mesh = unit.string_or("MESHFILE", "");
    if (!mesh.empty()) data.mesh = breakable_clean_path(directory + "/" + mesh + ".mesh");
    data.collision =
        breakable_clean_path(unit.string_or("COLLISIONFILE", kBreakableDefaultCollision));
    data.interact_particle = upper(unit.string_or("INTERACT_PARTICLE", ""));
    data.interact_sound = upper(unit.string_or("INTERACT_SOUND", ""));
    data.shadows = unit.bool_or("SHADOWS", true);
    data.scale = unit.float_or("SCALE", 1.0f);
    data.scale_variation = unit.float_or("SCALE_VARIATION", 0.0f);
    // `CBaseUnit::unitInit`: `COLLIDEABLE`,
    // default true, and its `[SKILL]` blocks in order.
    data.collideable = unit.bool_or("COLLIDEABLE", true);
    data.block = unit.bool_or("BLOCK", false);
    unit.visit([&data, &skill_levels](const tl::formats::AdmSection& section) {
        if (section.name != "SKILL") return;
        const std::string name = section.string_or("NAME", "");
        if (name.empty()) return;
        uint32_t level = section.integer_or("LEVEL", 1u);
        if (skill_levels) {
            // `addSkillByName` adds nothing for a name no skill file has;
            // `CSkill::_setLevelOfSkillFromSkillManager` keeps -1 as 1 and
            // caps the level at the skill's count of `[LEVELn]` blocks, 1 at least.
            const std::optional<uint32_t> count = skill_levels(upper(name));
            if (!count) return;
            const uint32_t cap = *count == 0 ? 1u : *count;
            if (level == 0xffffffffu) {
                level = 1;
            } else if (cap < level) {
                level = cap;
            }
        }
        data.skills.push_back(name);
        data.skill_levels.push_back(level);
    });
    return data;
}

float breakable_scale(const BreakableData& data, RunicRandom& volatile_rng) {
    // `CBaseUnit::unitInit`.
    return data.scale + volatile_rng.between(0.0f, data.scale_variation);
}

void breakable_unit_init(Breakable& b, const BreakableData& data) {
    // `CBaseUnit::unitInit` clears `+0x19c` for a unit that does not collide;
    // `CBreakable::unitInit` sets it for one that does, then, after
    // `setVisible(false, true)`, `+0x1f0`.
    b.collideable = data.collideable;
    b.collision_live = data.collideable;
    b.usable = true;
}

void breakable_update(Breakable& b) {
    // `CBreakable::update`.
    if (!b.collideable) return;
    b.collision_live = b.active && !b.used;
}

int32_t select_random_skill(uint32_t known, RunicRandom& rng) {
    // `CBaseUnit::selectRandomSkill`:
    // `knownSkills(0)`, then `CRandomizer(0)` with `addChoice(i, 1)` for each.
    if (known == 0) return -1;
    Randomizer choice;
    for (uint32_t i = 0; i < known; ++i) choice.add_choice(static_cast<int32_t>(i), 1);
    return choice.get_random(rng);
}

BreakableInteraction breakable_interact(Breakable& b, bool in_level, const Vec3& position,
                                        const Vec3& up, const Vec3* breaker, uint32_t known_skills,
                                        RunicRandom& game_rng) {
    // `CBreakable::interact(CCharacter*)`.
    BreakableInteraction out;
    if (b.interacted) return out;
    b.interacted = true;
    if (!in_level) return out;

    // Unusable, and off the avoidance map (vtable `+0x268`).
    b.usable = false;

    // The skill, cast with the breakable's own collision switched off and put
    // back afterwards: at the position lowered by `+0x194`.
    out.skill = select_random_skill(known_skills, game_rng);
    out.skill_at = Vec3{position.x + 0.0f, position.y + -kBreakableRadius, position.z + 0.0f};
    out.skill_target = breaker != nullptr ? *breaker : position;

    out.particle_at =
        Vec3{up.x * kBreakableParticleLift + position.x, up.y * kBreakableParticleLift + position.y,
             up.z * kBreakableParticleLift + position.z};

    // `setVisible(false, true)` (vtable `+0x2c0`), the collision model out,
    // `+0x218`; then `CItem::interact`.
    if (b.collideable) b.collision_live = false;
    b.used = true;
    out.broke = true;
    return out;
}

bool breakable_trap_noticed(bool executed, bool by_player, int32_t skill_type) {
    //
    return executed && by_player && skill_type != 2;
}

float attack_view_reach(float scale, float reach_bonus, float strike_range,
                        float range_multiplier) {
    // `CCharacter::performAttack`: `+0x98 * +0x4e0 + 0.2 + attack
    // +0x6c`, times `+0x4dc`, plus 1.
    return (scale * reach_bonus + 0.2f + strike_range) * range_multiplier + 1.0f;
}

bool item_within_view(const ViewFrame& frame, const Vec3& at, float max_angle, float max_distance) {
    // `CLevel::findItemWithinView`: the point
    // through the inverse of the attacker's transform (`Matrix4::inverse`);
    // the distance in the x-z plane in one `fmadds`, the
    // console's `atan2(x, z)` rounded and in degrees.
    const Vec3 local =
        matrix_transform(matrix_inverse(object_transform(frame.orientation, frame.position)), at);
    const float distance = std::sqrt(std::fma(local.x, local.x, local.z * local.z));
    const float angle =
        static_cast<float>(crt_atan2(static_cast<double>(local.x), static_cast<double>(local.z))) *
        57.29582595825195f;
    bool near = !(std::fabs(angle) > max_angle) && !(distance > max_distance);
    // anything within 1.5 is in view.
    if (!(distance > 1.5f)) near = true;
    return near;
}

// The tests before the geometry in `CLevel::findItemWithinView`:
// usable, active, visible.
bool breakable_candidate(const Breakable& b, bool visible) {
    return b.usable && b.active && visible;
}

}  // namespace tl::core
