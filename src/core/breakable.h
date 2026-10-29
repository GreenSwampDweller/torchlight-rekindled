// A breakable: a barrel, an urn or a basket, which a blow smashes, which may
// spring a trap as it goes, and which leaves treasure. `CBreakable`
// (`CBreakable::CBreakable`, `unitInit`,
// `interact`, `update`,
// `CBreakable::setActiveInLevel`),
// `CBaseUnit::selectRandomSkill` and
// `CLevel::findItemWithinView`.
//
// A `CItem`, not a character: no health, and any blow that reaches it breaks it:
// the swing (`CCharacter::performAttack`), a damage
// shape (`CDamageShape::doDamageToItem`), a skill's weapon damage
// (`CSkillEvent::applyWeaponDamage`) or a missile (`CMissile::handleMissileHitUnit`,
// `doAOEDamage`).
//
// No OGRE and no file I/O.
#pragma once

#include "core/collision.h"
#include "core/ogre_math.h"
#include "core/random.h"
#include "formats/adm.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace tl::core {

/// `COLLISIONFILE`'s default in `CBreakable::unitInit`.
///
inline constexpr const char* kBreakableDefaultCollision = "media/models/collider.mesh";
/// `CBaseUnit::CBaseUnit` sets 0.3, and a
/// breakable never reads `COLLISION_RADIUS` (only `CCharacter::unitInit`
/// and `CTriggerUnit::unitInit` do), whatever it authors.
inline constexpr float kBreakableRadius = 0.3f;
/// `interact` starts the particle `0.1` along the breakable's up vector.
inline constexpr float kBreakableParticleLift = 0.1f;

/// What `CBreakable::unitInit` and the base
/// `unitInit`s it calls read from the unit's data.
struct BreakableData {
    /// `RESOURCEDIRECTORY` + `/` + `MESHFILE` + `.mesh`, through
    /// `FILESYSTEM::CleanPath`; empty when `MESHFILE` is (then no model is made).
    std::string mesh;
    /// `COLLISIONFILE`, default `media/models/collider.mesh`, through `CleanPath`.
    std::string collision = kBreakableDefaultCollision;
    /// `INTERACT_PARTICLE`, upper-cased; empty makes no particle (`+0x240`).
    std::string interact_particle;
    /// `INTERACT_SOUND`, upper-cased: sample 0 of the unit's bank (`+0x1d8`).
    std::string interact_sound;
    bool shadows = true;  ///< `SHADOWS`, `setCastsShadows`
    /// `SCALE` (default 1) and `SCALE_VARIATION` (default 0), which
    /// `CBaseUnit::unitInit` rolls into the
    /// unit's scale.
    float scale = 1.0f;
    float scale_variation = 0.0f;
    /// `COLLIDEABLE` (+0x18d), `CBaseUnit::unitInit`
    bool collideable = true;
    /// `BLOCK` (X360 +0x156, Linux +0x18e), default false, the same reader:
    /// whether a ray meets the collision model (`CLevel::objectRayCollision`).
    ///
    bool block = false;
    /// Each `[SKILL]` block's `NAME`, in file order, that names a skill file
    /// (`addSkillByName` passes over one that does not): the
    /// skill manager's known list (`+0x1c8 -> +0x60`) `selectRandomSkill` picks
    /// from.
    std::vector<std::string> skills;
    /// Each block's `LEVEL` as `CSkill::_setLevelOfSkillFromSkillManager`
    /// keeps it: -1 is 1, and a level above the skill's count of
    /// `[LEVELn]` blocks (at least 1) is that count. Its default, the unit's own
    /// level, is not kept for an item; 1 stands in.
    std::vector<uint32_t> skill_levels;
};

/// The skills a breakable's `[SKILL]` blocks may name: a skill's count of
/// `[LEVELn]` blocks, or none for a name no skill file has. Unset, every block
/// is kept with its `LEVEL` as authored.
using BreakableSkillLevels = std::function<std::optional<uint32_t>(const std::string& upper_name)>;

/// `CleanPath` as the paths above need it: backslashes to slashes, doubled
/// slashes collapsed.
std::string breakable_clean_path(const std::string& path);

/// `CBreakable::unitInit`'s reads of the
/// unit's data group - the file over its `BASEFILE` chain, which the caller
/// has merged (`core/unit_resource.h`).
BreakableData read_breakable(const tl::formats::AdmFile& unit,
                             const BreakableSkillLevels& skill_levels = {});

/// The flags of `CItem` and `CBreakable` the class runs on.
struct Breakable {
    bool interacted = false;      ///< +0x22c, set by the first `interact`
    bool usable = true;           ///< +0x1f0, `CItem` constructor; cleared on the break
    bool used = false;            ///< +0x218, the model stays hidden
    bool collision_live = false;  ///< +0x19c, the collision model takes part
    bool active = false;          ///< +0x199, the level's "in range" flag
    bool collideable = true;      ///< +0x18d
    bool on_grid = false;         ///< registered by `addToAvoidanceMap`
};

/// `CBaseUnit::unitInit`, the first thing
/// `CItem::unitInit` runs: `SCALE +
/// randomBetween(0, SCALE_VARIATION)` on `g_RandVolatile`.
float breakable_scale(const BreakableData& data, RunicRandom& volatile_rng);

/// The end of `CBreakable::unitInit`: the
/// collision model live when `COLLIDEABLE`, and `+0x1f0` set.
void breakable_unit_init(Breakable& b, const BreakableData& data);

/// `CBreakable::update`, after
/// `CItem::update`: the collision model takes part only while the breakable is
/// active and not used.
void breakable_update(Breakable& b);

/// `CBaseUnit::selectRandomSkill`: one
/// `CRandomizer` choice per known skill, each of weight 1, and one draw on
/// `rng` (`g_Rand`). -1 when the unit knows none (no draw then).
int32_t select_random_skill(uint32_t known, RunicRandom& rng);

/// What `CBreakable::interact(CCharacter*)`
/// did, for the caller to carry out in the same order: the skill, the particle,
/// the sound, the treasure, the model hidden, the collision off, and
/// `CItem::interact`.
struct BreakableInteraction {
    bool broke = false;  ///< the return value
    /// `selectRandomSkill`'s pick, an index into `BreakableData::skills`; -1 for
    /// none. The caller executes it with the breakable as its owner, at
    /// `skill_at`, aimed at `skill_target`, and then asks
    /// `breakable_trap_noticed`.
    int32_t skill = -1;
    Vec3 skill_at;
    /// `executeSkill`'s last argument: the breaker's position, or the
    /// breakable's own when nobody broke it.
    Vec3 skill_target;
    Vec3 particle_at;  ///< the position plus 0.1 along the up vector
};

/// `CBreakable::interact(CCharacter*)`, the
/// decisions. Only the first call does anything: `+0x22c` is set before the
/// level test, so a breakable asked outside a level is spent too. `position` is
/// its position, `up` its up vector (`CPositionableObject +0x9c`), `breaker`
/// the position of the character that broke it (null for none).
BreakableInteraction breakable_interact(Breakable& b, bool in_level, const Vec3& position,
                                        const Vec3& up, const Vec3* breaker, uint32_t known_skills,
                                        RunicRandom& game_rng);

/// After a skill `interact` executed: the character's sample 0x36 and journal
/// statistic 10 when the skill started, the breaker is a player (ISA 0x1c)
/// and the skill's `SKILL_TYPE` (`CSkill +0x5c`) is not 2 (`DEFENSIVE`).
bool breakable_trap_noticed(bool executed, bool by_player, int32_t skill_type);

/// `CCharacter::performAttack`'s view distance for `findItemWithinView` and
/// `findCharacterWithinView`: `(scale * REACH_BONUS + 0.2 + the attack's strike
/// range) * RANGE_MULTIPLIER + 1`.
float attack_view_reach(float scale, float reach_bonus, float strike_range, float range_multiplier);

/// The attacker's transform as `findItemWithinView` is handed it (an object's
/// vtable +0x7c): its node's derived orientation and where it
/// stands.
struct ViewFrame {
    Vec3 position;
    OgreQuaternion orientation;
};

/// `CLevel::findItemWithinView`, the test on
/// one candidate: through the inverse of the attacker's transform, the angle
/// `atan2(x, z)` within `max_angle` degrees either side and the distance in
/// that plane within `max_distance` - or the distance within 1.5, whatever the
/// angle. The height is left out. The sight line the original then casts
/// (both ends 5 up) is the caller's.
bool item_within_view(const ViewFrame& frame, const Vec3& at, float max_angle, float max_distance);

/// `findItemWithinView`'s candidate filter: `+0x1f0` usable, `+0x199` active,
/// visible (vtable `+0x48`) and ISA `BREAKABLE` (0x1d).
bool breakable_candidate(const Breakable& b, bool visible);

}  // namespace tl::core
