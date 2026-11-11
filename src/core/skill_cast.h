// What a skill *does*: the join between the skill book (`core/skills.h`) and the effects a
// unit holds (`core/effect_manager.h`). `CSkillManager::startPassiveSkills`,
// `CSkillManager::executeSkill`,
// `CSkill::startSkill`, `CSkill::triggerEvent`,
// `CSkillEvent::applyEffects`,
// `CSkillEffectAndAffixes::applyAffixesAndEffects`.
// Kept apart from `skills.h` so that one depends only on `formats/adm.h`.
#pragma once

#include "core/collision.h"
#include "core/effect_manager.h"
#include "core/skills.h"
#include "core/unit_types.h"

#include <deque>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tl::core {

/// `gSKILL_EVENT_TYPE_NAMES` index 0. `CSkill::startSkill`
/// ends with `triggerEvent(0, ...)`, so
/// starting a skill is exactly firing this group.
inline constexpr int kSkillEventStart = 0;
/// Index 1, which `CSkill::stopSkill` fires:
/// the blocks that undo what the start put on.
inline constexpr int kSkillEventEnd = 1;

struct SkillCaster;
class UnitThemeBook;
class UnitThemeSet;

/// A model's animations in its own order (`CGenericModel +0x1e0 -> +0x28`), each with its
/// length in seconds; the index is what every `getAnimationIndex*` returns.
using AnimationCatalog = std::vector<std::pair<std::string, float>>;

/// `CSkill +0xe4` .. `+0xf4` and `+0x100`: the caster's animations a skill plays, as
/// indices into its `AnimationCatalog`, -1 for none (`assign_skill_animations`).
struct SkillAnimations {
    int animation = -1;     ///< `+0xe4`, `getAnimationIndex`
    int animation_dw = -1;  ///< `+0xe8`, `getAnimationIndexDW`
    int loop_into = -1;     ///< `+0xec`, `getAnimationIndexLoopInto`
    int dw_loop_into = -1;  ///< `+0xf0`, `getAnimationIndexDWLoopInto`
    int loop_end = -1;      ///< `+0xf4`, `getAnimationIndexLoopEnd`
    /// `+0x100`: the main animation's length in seconds (the dual-wield one's when longer), 1
    /// for a caster with no model. `startSkill` copies it
    /// into the run's countdown, so a cast runs until its animation is over.
    float length = 0.0f;
};

/// What `CSkill::triggerEvent` hands each
/// `CSkillEvent::startEvent` beside the caster: the
/// target unit (7th argument) and point (8th). `startSkill`
/// passes the cast's point for `EVENT_START`; `CSkillManager::triggerSkillEvent`
/// the stored point only for target types 1, 5 and 6;
/// `invokeHitSkills` the unit struck and no point.
struct SkillEventTarget {
    /// 1st argument: the origin a layout without `PLACEONTARGET` stands at, and
    /// the 4th, its turn (a yaw). Unset is the caster's own; a unit hit
    /// hands the struck unit's centre and turn.
    std::optional<Vec3> origin;
    std::optional<float> yaw;
    std::optional<Vec3> position;
    /// The target unit, as the caller names it (a node of `machine`); none
    /// when `has_unit` is false.
    bool has_unit = false;
    int64_t unit = 0;
    const void* machine = nullptr;
    /// With no target unit, the caster's target item (a breakable, by id; 0 for none), which
    /// `CCharacter::performSkill` hands `executeSkill` in
    /// its place. `castSkill` finds one only through
    /// `CLevel::findItemWithinView`.
    int64_t item = 0;
    /// Which copy of the skill (`SkillRunState::copy`) the group is fired on, and so whose
    /// events it starts (a missile's hit goes to the run that fired it).
    uint32_t run = 0;
};

struct SkillCastContext {
    /// The unit the effects land on - for a passive and for a self-buff, the
    /// caster's own manager.
    EffectManager* target = nullptr;
    /// `media/EffectsList.dat`, for the inline `[EFFECT]` blocks a skill level
    /// authors.
    const EffectTypeTable* types = nullptr;
    /// `media/UNITTYPES.HIE`, for the blocks' `TARGETTYPE`s
    /// (`CHierarchy::getTypeIDByName` in `CSkillEffectAndAffixes`'s
    /// constructor). Null leaves them 0.
    const UnitTypeHierarchy* unit_types = nullptr;
    /// An affix by `NAME`, as `CResourceManager::getNewAffixByName`
    /// hands one out. The level is applied by
    /// the manager, so this only has to find the file.
    std::function<const Affix*(const std::string&)> affix;

    /// The three questions `applyAffixesAndEffects`' `TARGET` filter asks about
    /// the caster and the target. For a passive or a self-buff the target *is*
    /// the caster, which is this struct's default.
    bool target_is_caster = true;
    bool target_is_enemy = false;
    bool target_is_casters_pet = false;
    /// No character casts (a prop's trap): every `TARGET` arm applies.
    bool no_caster_character = false;
    /// `CEffect::setOwner(effect, the skill's owner, true)`, the roll
    /// `applyAffixesAndEffects` makes for an effect with no owner yet, before the caster's. A
    /// prop's skill, its owner null, leaves the effect owned by the prop after it, so only its
    /// first apply rolls it. Null: none.
    const EffectRolls* ownerless_rolls = nullptr;
    /// The block's effects (group, effect) already owned; an effect enters at its first apply.
    /// Null: every apply rolls.
    std::set<std::pair<size_t, size_t>>* ownerless_owned = nullptr;

    /// The unit casting, for the weapon gate `CSkillManager::startPassiveSkills`
    /// runs before each passive. Null skips that gate.
    const SkillCaster* caster = nullptr;

    /// `CSkillEvent::startEvent`'s layout half: called for
    /// every `[EVENT_*]` block a fired group reaches, after its effects. Placing and starting
    /// the block's `FILE` is the caller's.
    std::function<void(const KnownSkill&, uint32_t, int, const tl::formats::AdmSection&,
                       const SkillEventTarget&)>
        on_event;
    /// The target the next fired group's events are handed (see
    /// `SkillEventTarget`). `SkillRunner` fills the point from what it stored.
    SkillEventTarget event_target;
    /// Whether any event the named skill started is still alive (`CSkill +0xc0`;
    /// `CSkillEvent::updateEvent`). Null is "none", which ends a
    /// skill on its animation alone. Asked per copy (`SkillEventTarget::run`).
    std::function<bool(const std::string&, uint32_t)> events_alive;
    /// `CSkillEvent::canStop` over every live event of one
    /// copy. Unlike `events_alive`, a missile still flying does not count. Null answers yes.
    std::function<bool(const std::string&, uint32_t)> events_can_stop;
    /// `CSkill::stopSkill`'s second half:
    /// `CSkillEvent::resetEvent` on every event of the property (layout, particles,
    /// damage shapes stopped). Null resets nothing.
    std::function<void(const std::string&, uint32_t)> reset_events;
    /// `CSkillEvent::reset`, run first by `startEvent`: the
    /// event leaves the listener list of every missile it fired that still flies, so a block
    /// started again leaves its last start's missiles with no listener. Null detaches nothing.
    std::function<void(const std::string&, uint32_t, const tl::formats::AdmSection&)> reset_event;
    /// The event blocks the fired copy has started (`CSkillEvent +0x140`):
    /// `CSkill::triggerEvent` passes over a started block
    /// unless it may clone (`CAN_CLONE`), and then starts a clone. Null starts every block
    /// every time.
    std::vector<const tl::formats::AdmSection*>* started_events = nullptr;

    /// PORT: called once per passive `start_passive_skills` considered, with its name, level
    /// and whether anything was applied (a passive applying nothing is a data path that failed).
    std::function<void(const std::string&, uint32_t, bool)> on_skill;

    /// `CCharacter::applyInstantEffect` on the target, for
    /// an `INSTANT` effect, which `applyEffectOnUnit` does
    /// not add to the manager. Null applies nothing.
    std::function<void(const Effect&)> instant_effect;
    /// `CLevel::addListenerToUnit(target, skill)` as
    /// `applyAffixesAndEffects` runs it, so the skill hears the target die
    /// of an effect (`CSkill::unitStateChange`). Null: nothing watches.
    std::function<void(const std::string& skill)> observe_target;
    /// The caster, as the owner every effect this apply puts on is held with
    /// (`CEffect::setOwner(effect, caster)`; the affixes'
    /// through `CBaseUnit::addAffix`). Unset: nobody's.
    EffectSource source;
    /// That caster's level (X360 `+0xf0`), which `setOwner` gives a
    /// `USEOWNERLEVEL` effect (`set_owner_level`). Empty with no caster.
    std::optional<int32_t> owner_level;
};

/// `CSkillEffectAndAffixes::applyAffixesAndEffects(CBaseUnit*, CCharacter*,
/// const Vector3*, bool)`: each affix and inline effect
/// group onto the target, subject to the block's `TARGET` filter (skipped with
/// `ignore_target_filter`, the fourth argument). Whether anything was applied.
bool apply_affixes_and_effects(const SkillEffectAndAffixes& block, const SkillCastContext& context,
                               const std::string& skill_name, bool ignore_target_filter = false);

/// One `[EVENT_*]` block's `[AFFIXES]`/`[EFFECTS]`, applied:
/// `CSkillEvent::applyEffects(CBaseUnit*, const Vector3*, bool)`.
/// The event's own block, or the level's when it authors none (Ember Bolt).
/// `unauthored` is what a block with no `TARGET` filters by. No line-of-sight ray.
/// `CSkillEffectAndAffixes::isDamaging` on that block:
/// an affix or inline effect of type 0x34, 7 or 0x7c.
bool event_is_damaging(const tl::formats::AdmSection& event, const SkillCastContext& context,
                       const tl::formats::AdmSection* property, SkillTargetFilter unauthored);

/// `CSkillEvent::effectsTargetEnemy(CBaseUnit*)` over
/// `CSkillEffectAndAffixes::effectsTargetEnemy`, on the block
/// `apply_event_effects` would apply: an `[AFFIXES]` spec whose `TARGETTYPE` the unit is and
/// whose `TARGET` is `ENEMY`; inline `[EFFECTS]` do not count. `startEvent` asks
/// it of a breakable target before breaking it.
bool event_effects_target_enemy(const tl::formats::AdmSection& event,
                                const SkillCastContext& context,
                                const tl::formats::AdmSection* property,
                                SkillTargetFilter unauthored);

/// What `CSkillEffectAndAffixes::canAffixesAndEffectsBeAppliedToUnit` asks of
/// the unit (2nd argument) and the character applying the blocks (3rd).
struct SkillApplyFacts {
    /// `CHierarchy::ISA` on the unit's own hierarchy (X360 `+0x60 -> +0x10`)
    /// of a block's `TARGETTYPE`; unset is a unit with no hierarchy,
    /// which every block refuses.
    std::function<bool(int32_t type)> isa;
    /// The unit is a character (X360 `+0x150`, Linux `+0x188`, clear).
    bool unit_is_character = true;
    bool has_user = true;       ///< the 3rd argument is not null
    bool unit_is_user = false;  ///< the unit is that character
    /// `CCharacter::alignment` of the two compare equal (both characters).
    bool same_alignment = false;
    /// The unit's master (X360 `+0x5a4`, Linux `+0x640`) is the user.
    bool master_is_user = false;
    /// The unit's `isEffectValidForUnit(user, user, effect)` (vtable +0x110),
    /// for each effect of an effect group.
    std::function<bool(const Effect&)> effect_valid;
};

/// `CSkillEffectAndAffixes::canAffixesAndEffectsBeAppliedToUnit(CBaseUnit*, CCharacter*)`:
/// every effect group, then every affix, must be of a
/// type the unit `ISA`s and pass its `TARGET` (`ENEMY`, `SELF`, `PET`; `EVERYBODY` and
/// `FRIEND` are not tested), and every effect must be valid for the unit.
bool skill_block_can_apply(const SkillEffectAndAffixes& block, const SkillApplyFacts& facts);

/// `CSkillProperty::canAffixesAndEffectsBeAppliedToUnit`
/// on a level's section: its own block's answer, else every `[EVENT_*]`'s
/// (`CSkillEvent::canAffixesAndEffectsBeAppliedToUnit`, inlined on the
/// console; true with no block). `events` is the section whose event groups the property
/// runs (`SkillProperty::events`); `property` itself when null.
bool skill_property_can_apply(const tl::formats::AdmSection& property, const EffectTypeTable& types,
                              SkillTargetFilter unauthored, const SkillApplyFacts& facts,
                              const tl::formats::AdmSection* events = nullptr,
                              const UnitTypeHierarchy* unit_types = nullptr);

/// What `CSkill::getTargetIsValid` asks of
/// the unit it tests.
struct SkillTargetFacts {
    bool alive = false;             ///< `CCharacter::HP() > 0`
    bool is_owner = false;          ///< it is the owner (2nd argument)
    bool master_is_owner = false;   ///< its master (`+0x640`) is the owner
    bool is_owners_master = false;  ///< the owner's master (`+0x640`) is it
    bool enemy = false;             ///< `owner->isEnemy(it)`
    bool unit_type_matches = true;  ///< `ISA(TARGET_UNITTYPE)` (property +0x74); true for none
    bool specific_unit = true;      ///< among `TARGET_SPECIFIC_UNITS` (+0x78); true for none
};

/// `CSkill::getTargetIsValid(candidate, owner, at, dead)`.
///
struct SkillTargetQuery {
    bool has_property = true;      ///< `+0x90`, the level's property
    int32_t target_type = 0;       ///< its `TARGET_TYPE` (+0x70)
    int32_t target_alignment = 2;  ///< `TARGET_ALIGNMENT` (`CSkill +0x120`)
    /// The 4th argument, or the skill's own +0x6e (`TARGET_CORPSES`): only the dead.
    bool dead_wanted = false;
    /// `owner->alignment()`: +0x338, swapped
    /// 1 <-> 2 under effect 0x50. 0 refuses everyone; 4 takes everyone the
    /// earlier tests let by.
    int32_t owner_alignment = 1;
    SkillTargetFacts candidate;
    SkillTargetFacts owner;  ///< the owner as the unit tested, for a `SELF` property
};
bool skill_target_is_valid(const SkillTargetQuery& query);

/// The duration `CSkillEvent`'s constructor sets on the
/// event's block (`setDurationOverride`, when above 0): the larger of the
/// event's own `DURATIONOVERRIDEMS` in seconds and its property's.
float skill_event_duration_override(const tl::formats::AdmSection& event, float property_override);

bool apply_event_effects(const tl::formats::AdmSection& event, const SkillCastContext& context,
                         const std::string& skill_name,
                         const tl::formats::AdmSection* property = nullptr,
                         SkillTargetFilter unauthored = SkillTargetFilter::Enemy,
                         float property_duration_override = 0.0f);

/// An `[EVENT_*]` block's own keys, as `CSkillEvent::CSkillEvent`
/// reads them.
struct SkillEventSpec {
    std::string file;              ///< `FILE`: the layout `startEvent` places (+0x38)
    bool attaches = false;         ///< `ATTACHES`: the layout rides the caster
    bool place_on_target = false;  ///< `PLACEONTARGET`: placed on the target instead
    /// +0x138 `APPLYEFFECTS`: `startEvent`
    /// applies the event's effects and weapon damage to its target character
    /// and invokes the hit skills, with no shape or missile between.
    bool apply_effects = false;
    /// +0x13e `ATTACHTOTARGET`: the layout stands on the target and follows
    /// it (`CSkillEvent::updateEvent`).
    bool attach_to_target = false;
    /// +0x148 `ATTACH_LINE_EMITTER_TO_MAX_DISTANCE`: an `ATTACHTOTARGET`
    /// layout goes at the skill's range along the caster's forward instead.
    bool attach_to_max_distance = false;
    /// +0x149 `LINE_EMITTER_COLLIDES`: that point is cut short at the first
    /// thing a ray to it meets.
    bool line_emitter_collides = false;
    /// +0x14a `DAMAGE_REQUIRES_LOS`: `applyEffects` and
    /// `applyWeaponDamage` skip a target the geometry hides
    /// from the skill's master owner.
    bool damage_requires_los = false;
    /// +0x147 (X360 +0xca) `ATTACH_LINE_EMITTER`: 1, read over 0 for an event with a `FILE`.
    /// `updateEvent` stretches the layout's `Line` emitters
    /// from the caster's `FIREFROMBONE` to the target's `FIREATBONE`, or to the skill's range.
    bool attach_line_emitter = true;
    /// +0x108 `FIREFROMBONE` and +0x10c `FIREATBONE`, upper-cased and looked
    /// up in `gSKILL_BONE_ATTACHMENT_NAMES` (`kSkillBone*`); 0 for none or an
    /// unknown name (`STRINGS::getStringIndex(..., 8, 0, false)`).
    int fire_from_bone = 0;
    int fire_at_bone = 0;
    /// +0x164 `TARGET_AHEAD_OF_UNIT`: how far ahead of a moving target to aim.
    float target_ahead = 0.0f;
    /// +0x139 `ATTACHOWNER`: while the layout runs, its caster is moved to the event's first
    /// damage shape (`updateEvent`), with `ATTACHIGNORESUNITS`,
    /// `ATTACHIGNORESOBSTRUCTIONS` and `SCALEATTACHDISTANCE`.
    bool attach_owner = false;
    bool attach_ignores_units = false;
    bool attach_ignores_obstructions = false;
    bool scale_attach_distance = false;
    /// +0x13a `CASTFROMTARGET`: the event's `EXECUTE_SKILL`s are cast by its
    /// target unit, from the target's point, with no target of their own.
    bool cast_from_target = false;
    /// +0x110: the `SKILL` of each `[EXECUTE_SKILL]` block, in file order
    /// (`CExecuteSkillProps::CExecuteSkillProps`).
    std::vector<std::string> execute_skills;
    /// +0x130 `EXECUTE_SKILL_COUNT`: above 0, that many picks from
    /// `execute_skills` through a `CRandomizer` with every choice at odds 100;
    /// otherwise each one in order.
    int execute_skill_count = 0;
    /// +0x150, `WEAPONDAMAGEPCT / 100`, floored at 0: the share of a weapon
    /// blow `applyWeaponDamage` strikes with;
    /// 0 strikes none.
    float weapon_damage = 0.0f;
    /// +0x154, `SOAKSCALEPCT / 100` (default 100), floored at 0: the soak
    /// factor of that blow.
    float soak_scale = 1.0f;
    bool target_corpses = false;  ///< +0x14d `TARGET_CORPSES`: only the dead
    int max_units_hit = -1;       ///< +0x168 `MAX_UNITS_HIT`
    /// +0x143 `MAKEPET`, default true: a unit the event's spawners release becomes the caster's
    /// pet (`unitSpawnedFromUnitSpawner`).
    bool make_pet = true;
    /// +0x142 `MATCH_PET_DURATION`: the event's time left (+0x15c) becomes
    /// the released unit's effect 0x2f value.
    bool match_pet_duration = false;
    /// +0x14e `SYNCH_PETS_TO_OWNER`: a released unit plays the caster's
    /// animation from the caster's point in it.
    bool synch_pets_to_owner = false;
};
/// `gSKILL_BONE_ATTACHMENT_NAMES` (`_GLOBAL__I_CSkillEvent`): the
/// eight names `FIREFROMBONE`/`FIREATBONE` are read against.
enum SkillBone {
    kSkillBoneNone = 0,
    kSkillBoneCenter = 1,
    kSkillBoneHead = 2,
    kSkillBoneRightHand = 3,
    kSkillBoneLeftHand = 4,
    kSkillBoneRightShoulder = 5,
    kSkillBoneLeftShoulder = 6,
    kSkillBonePosition = 7,
};
/// The index of `name` (any case) in that table; 0 when it is not there.
int skill_bone_attachment(const std::string& name);

SkillEventSpec read_skill_event(const tl::formats::AdmSection& event);

/// What making a `CSkill` from its file needs to draw what the console draws.
struct SkillConstruction {
    const EffectTypeTable* types = nullptr;
    const UnitTypeHierarchy* unit_types = nullptr;
    /// The resource manager's affixes, for the display affixes each block
    /// makes (`createAffixesForDisplay`); none, and they are not made.
    const AffixBook* affixes = nullptr;
    /// `g_RandVolatile` and the curves, no owner: the skill has none yet.
    EffectRolls rolls;
    /// `g_Rand`, and whether loading an event's `FILE` makes a `Sound` node whose `GUID` names
    /// a sound: `CSoundObject::setSoundBankGuid` adds the sample
    /// (`CSoundBank::addSample`), which keeps only `g_Rand`'s low word.
    RunicRandom* game_rng = nullptr;
    std::function<bool(const std::string& file)> layout_adds_sample;
    /// An event's `FILE` loaded into its own `CLayout` (`CLayout::loadLayoutFile`):
    /// counted and seeded while a level is loaded.
    std::function<void(const std::string& file)> load_event_file;
};

/// The draws of `CSkill::CSkill(CResourceManager*, CDataGroup*)`, run by
/// `CSkillManager::addSkill` for every skill a unit is given, in this
/// order: the root property, then each `[LEVELn]` (`CSkillProperty::CSkillProperty`).
/// Its events, group by group in file order, each
/// `CSkillEvent::CSkillEvent` followed by `CLONECOUNT` clones
/// (`cloneEventForSkill`); none authored shares the parent's.
/// Then the property's own block. Each block
/// rolls its effects and ends in `createAffixesForDisplay`.
/// An event with a `FILE` loads it first (`CLayout::loadLayoutFile`).
///
void roll_skill_construction(const SkillDef& def, const SkillConstruction& with);

/// `CSkill::startSkill` reduced to the level's
/// `EVENT_START` group applied to the target. `level` is the **effective** level, as
/// `CSkill::getSkillLevelStats` and every other reader picks the property by.
bool start_skill(const KnownSkill& skill, uint32_t level, const SkillCastContext& context);

/// `CSkill::stopSkill` reduced the same way: the level's
/// `EVENT_END` group, where `[AFFIXESREMOVE]` lives (a passive's affix comes off by name).
bool stop_skill(const KnownSkill& skill, uint32_t level, const SkillCastContext& context);

/// `CSkillManager::startPassiveSkills(CBaseUnit*, ...)`:
/// every passive at an effective level above zero is started. Stopping one already running
/// is the caller's (`EffectManager::delete_affix`). Returns how many started.
int start_passive_skills(const SkillBook& book, const SkillCastContext& context);

// ---------------------------------------------------------------------------
// The gate, the cooldown and the cast.
// ---------------------------------------------------------------------------

/// What the gate needs to know about a target unit.
struct SkillTargetUnit {
    Vec3 position{};
    bool is_item = false;        ///< `CBaseUnit::ISA(2 [item])`
    bool is_equipment = false;   ///< `dynamic_cast<CEquipment*>`
    bool is_identified = false;  ///< `CEquipment +0x348 != 0`
};

/// What the gate has to know about the unit doing the casting.
struct SkillCaster {
    /// `CCharacter::mana()`. Both mana tests compare **integers**:
    /// `CSkill::getManaCost` truncates the float, so 12.7 costs 12 and 11.9 mana
    /// does not pay 12.
    float mana = 0.0f;

    /// The unit type of whatever is in each hand, by name, empty for an empty
    /// hand (`CCharacter::getWeaponInRightHand`, `InLeftHand`).
    ///
    std::string right_hand_type;
    std::string left_hand_type;

    /// `media/UNITTYPES.HIE`, for the `CBaseUnit::ISA` tests
    /// of the weapon gate. Without it the gate refuses any skill with a weapon requirement.
    const UnitTypeHierarchy* unit_types = nullptr;

    /// `CBaseUnit::ISA(0x1b)` in `CSkillProperty::getCoolDown`:
    /// a monster takes `MONSTERCOOLDOWNMS`
    /// where a player takes `COOLDOWNMS`.
    bool is_monster = false;

    /// The caster's own effects, for the `SILENCE` test. Null is "nothing
    /// applied", which passes.
    const EffectManager* effects = nullptr;
    /// The caster's inventory (`CCharacter +0x490`), the other half of its
    /// effect totals (`CharacterEffects`) where `castSkill` reads them. Null is
    /// a caster without one.
    const Inventory* inventory = nullptr;

    /// The unit theme catalog (`CUnitThemes::getSingleton`).
    /// Needed to resolve theme names for THEMEREQUIRED and NOTTHEMED.
    const UnitThemeBook* theme_book = nullptr;

    /// The caster's unit themes (`CBaseUnit` theme lists).
    /// Null means a unit with no themes.
    const UnitThemeSet* themes = nullptr;

    /// Caster's position (`CPositionableObject::getPosition`).
    ///
    Vec3 position{};

    /// Target position for `SkillTargetType::Position` (1).
    std::optional<Vec3> target_position;

    /// Target unit for `SkillTargetType::Target` (2), `Item` (7), `UnidentifiedItem` (8).
    std::optional<SkillTargetUnit> target_unit;

    /// `CSkill::requirementsMet`'s skip-range flag, set by
    /// `CCharacter::skillApproachAI` through
    /// `canCastCurrentSkill` while approaching.
    bool skip_range = false;

    /// Line-of-sight passability callback: `CLevel::passableBetween`
    /// with the caster and the target taken
    /// out of the map (vtable +0x268/+0x270).
    std::function<bool(const Vec3& from, const Vec3& to)> passable_between;

    /// The caster's model (vtable +0x1e0), null for a unit without one. What
    /// `CSkill::assignSkillAnimations` looks
    /// the skill's animations up in.
    const AnimationCatalog* model = nullptr;
    /// Whether the caster is a `CCharacter`: the weapon-animation and
    /// dual-wield halves of `assignSkillAnimations` run only for one.
    bool is_character = true;
    /// The attack animation of whatever each hand holds (`CEquipment +0x2a0`
    /// right, `+0x2a8` left, each a `CAttackDescription` whose `+0x10` is the
    /// animation prefix), for `USEWEAPONANIMATION`. Empty for an empty hand.
    std::string right_hand_attack_animation;
    std::string left_hand_attack_animation;
};

/// PORT: why a cast did not happen (the original's `castSkill` returns a bare 0 for all).
enum class SkillCastResult {
    Cast = 0,
    NoSuchSkill,
    NotLearned,
    NotEnoughMana,
    Cooling,
    RequirementsNotMet,
    ChanceFailed,
    /// Already in the running list; `executeSkill`
    /// reissues through the by-name overload rather than starting a second copy.
    AlreadyRunning,
};

const char* skill_cast_result_name(SkillCastResult result);

/// `CSkillManager::globallyDisableSkills(bool)`: the process-wide flag
/// `executeSkill` tests first, for every caster. Set by
/// a `Game State Controller` in `DISABLE SKILLS`, held by a `Skip Cutscene` with it, and
/// cleared every front-end frame by `CGameClient::updateMenu`.
///
void globally_disable_skills(bool disable);
bool skills_globally_disabled();

/// `CSkillProperty::getCoolDown`:
/// `MONSTERCOOLDOWNMS` for a monster, `COOLDOWNMS` for everything else, in
/// seconds.
float skill_cooldown(const SkillProperty& property, bool caster_is_monster);

/// `CSkill::getIsInRange(Vector3 const&, Vector3 const&, CBaseUnit*, CBaseUnit*)`:
/// 3-D distance and optional line-of-sight
/// test against `RANGEMIN <= distance <= RANGE`.
bool is_in_range(const KnownSkill& skill, uint32_t level, const Vec3& from, const Vec3& to,
                 const SkillCaster& caster);

/// `CSkill::weaponRequirementsMet(CCharacter*)`. Quirk:
/// with no right-hand requirement anything held passes, and a two-handed weapon in the left
/// hand (types 0x26, 10) can satisfy a right-hand one. `DUELWIELD_REQUIRED` wants both
/// hands matching.
bool weapon_requirements_met(const SkillProperty& property, const SkillCaster& caster);

/// `CSkill::requirementsMet(CBaseUnit*, ESKILL_ACTIVATION_TYPE, const Vector3*,
/// CBaseUnit*, bool)`. `Position` (1), `Target` (2),
/// `Item` (7) and `UnidentifiedItem` (8) are tested; everything else, `Self` included,
/// falls through to the weapon gate. `m_gAllowSkillTesting` is not modelled. `activation`
/// is the type asked: a skill of another type is refused unless either is 0 (`Any`).
bool requirements_met(const KnownSkill& skill, uint32_t level, const SkillCaster& caster,
                      SkillActivation activation = SkillActivation::Any);

/// One of the two units `position_for_skill` reads.
struct SkillPositionUnit {
    Vec3 centre;            ///< `getPosition(true)`
    float radius = 0.0f;    ///< X360 `+0x15c`, taken off the height
    Vec3 forward{0, 0, 1};  ///< X360 vtable `+0x98`, asked only by `POSITIONFLEE`
};

/// `CSkillManager::getPositionForSkill(CSkill*, CBaseUnit*)`:
/// `POSITION` is `skill_point`; `POSITIONRANDOM` and `POSITIONFLEE` draw twice
/// from the volatile stream round the owner and `target` (either may be null); no value
/// for other types. Quirk kept: x and y come from the draws and z stays `skill_point`'s.
std::optional<Vec3> position_for_skill(const SkillProperty& property, const Vec3& skill_point,
                                       const SkillPositionUnit* owner,
                                       const SkillPositionUnit* target, RunicRandom& rolls);

/// `CSkillManager::decrementSkillCharges(CSkill*)`: one
/// charge spent when the skill counts them (`KnownSkill::charges` above 0);
/// `SkillRunner::execute` calls it per start.
void decrement_skill_charges(const KnownSkill& skill);

/// `CSkill::rollSkillChance()`. A `CHANCE` of 100 or more **takes no draw**, so
/// `g_RandVolatile` is left alone.
bool roll_skill_chance(const SkillProperty& property);

/// `CSkill +0xf8` .. `+0x108` and the flags at `+0x64`, `+0x65`: what one running skill
/// keeps. It lives on the `CSkill`, which is why a cooldown outlives its run.
struct SkillRunState {
    uint32_t level = 1;
    /// The animations `assign_skill_animations` last found for this skill.
    SkillAnimations animations;
    /// `+0xfc`, counted down by `CSkill::updateSkill` every
    /// frame **before** the running test, so a cooldown runs on after the skill stops.
    float cooldown_remaining = 0.0f;
    /// `+0xf8`, the rate the run advances at, set by `castSkill`,
    /// floored at 0.2; 1 until set.
    float speed = 1.0f;
    /// `+0x104`, counting down, and `+0x108`, counting up, both at `speed`.
    /// `+0x104` starts at `SkillAnimations::length` - the cast animation's
    /// length, not `MINIMUMTIMEMS` - so `EVENT_END` waits for the animation.
    float minimum_time_remaining = 0.0f;
    float elapsed = 0.0f;
    bool running = false;    ///< `+0x64`
    bool end_fired = false;  ///< `+0x65`
    /// `+0x50`, the point `startSkill` stored
    /// for the run, which `triggerSkillEvent` hands on to the groups fired
    /// later.
    std::optional<Vec3> target_position;
    /// `CSkill::getTargetType`: the `TARGET_TYPE` (`+0x70`) of the
    /// property at `+0x90`, taken when the run starts.
    int32_t target_type = 0;
    /// `+0x40`, the unit `startSkill` was handed, kept for
    /// the run: `CSkillEvent::startEvent` falls back to it.
    /// Only `has_unit`, `unit` and `machine`.
    SkillEventTarget target_unit;
    /// Which copy of the skill this is, 0 the first. `CSkillManager::executeSkill(wstring)`
    /// starts the first copy not running or, with every copy
    /// running, clones the last (`addSkill(skill, true, false)`).
    uint32_t copy = 0;
    /// The order the copies were started in: the highest is the cast the
    /// character is in (`SkillRunner::state`).
    uint64_t started_serial = 0;
    /// `CSkillEvent +0x140` over this copy's events: the blocks started since
    /// the copy last stopped (`resetEvent`, clears it).
    std::vector<const tl::formats::AdmSection*> started_events;
};

/// `CSkillManager`'s running list and the per-frame pass over it. Cooling is keyed on the
/// shared skill data (`CSkill +0x28`), so copies share a cooldown; here, by name.
class SkillRunner {
public:
    /// `CSkillManager::getSkillIsCooling(CSkill*)`,
    /// over every copy of the name.
    bool is_cooling(const std::string& skill_name) const;
    /// PORT: a skill's cooldown set from outside (a test, a restore); an entry is made when
    /// there is none and `seconds` is above 0.
    void set_cooldown_remaining(const std::string& skill_name, float seconds);
    /// `CSkillManager::getSkillCoolingTime(CSkill*)`:
    /// the longest remaining cooldown among
    /// the entries naming this skill, 0 when none is.
    float cooling_time(const std::string& skill_name) const;
    /// Whether any copy of the skill is in the running list
    /// (`CSkillManager +0x48`).
    bool is_running(const std::string& skill_name) const;
    /// `CSkillManager +0x50`, the running list's count, is not 0.
    bool any_running() const;
    /// The copy of the skill started last - the one the character's cast is
    /// in - or null when it never ran.
    const SkillRunState* state(const std::string& skill_name) const;
    SkillRunState* state(const std::string& skill_name);

    /// `CSkillManager::getSkillCanBeExecuted`:
    /// not cooling, and `requirements_met`.
    bool can_be_executed(const KnownSkill& skill, uint32_t level, const SkillCaster& caster,
                         SkillActivation activation = SkillActivation::Any) const;

    /// `CSkillManager::executeSkill(CSkill*, ...)`: an
    /// exclusive skill stops every running skill sharing its GUID, then `startSkill`; on
    /// success it joins the running list, cooldown charged. A running skill is reissued via
    /// the by-name overload, which may start a new copy.
    bool execute(const KnownSkill& skill, uint32_t level, const SkillCaster& caster,
                 const SkillCastContext& context);

    /// `CCharacter::castSkill(long long)` reduced: the two
    /// mana gates, the execute, and the cost. `caster.mana` is only read; what was paid comes
    /// back in `mana_spent`.
    SkillCastResult cast(const KnownSkill& skill, const SkillCaster& caster,
                         const SkillCastContext& context, float* mana_spent = nullptr);

    /// `CSkill::stopSkill` on every running copy: it leaves
    /// the list, clock and rate reset. `EVENT_END` fires here only for a passive or a looping
    /// skill that has not fired it; a one-shot fires it from `update`. Cooldowns stay.
    bool stop(const KnownSkill& skill, const SkillCastContext& context);

    /// `CSkillManager::stopSkillsByName` with
    /// one name: every copy.
    void stop_by_name(const std::string& skill_name, const SkillCastContext& context,
                      const std::function<const KnownSkill*(const std::string&)>& lookup);

    /// `CSkillManager::stopAllSkills(force, keep_on_death, passives)`:
    /// passives only with `passives`; `keep_on_death` spares a skill with
    /// `EVENT_CASTERDIE` events, no property or `DONT_STOP_ON_DEATH`; with `force` each is
    /// stopped (`CSkill::stopSkill`), without it `CSkill::attemptToStopSkill`
    /// cuts the countdown to 0.001 when every event can stop, and `update`
    /// ends the run.
    void stop_all(const SkillCastContext& context,
                  const std::function<const KnownSkill*(const std::string&)>& lookup, bool force,
                  bool passives, bool keep_on_death = false);

    /// `CSkillManager::triggerSkillEvent(ESKILL_EVENT_TYPE, ...)`:
    /// the group on every listed skill through `CSkill::triggerEvent`,
    /// which takes only group 8 (`EVENT_DIEBYEFFECT`) on a skill
    /// not running. Returns how many applied something.
    int trigger_event(int group, const SkillCastContext& context,
                      const std::function<const KnownSkill*(const std::string&)>& lookup);
    /// `CSkill::triggerEvent` on one skill: one not running
    /// takes only group 8, which fires group 5 first. Groups get `context.event_target`
    /// (`invokeHitSkills`: the unit struck, no point); the copy is
    /// its `run`'s, else the last started. Whether anything fired.
    bool trigger_skill_event(const KnownSkill& skill, int group, const SkillCastContext& context);

    /// `CSkill::updateSkill(float)` over the active list, the
    /// loop of `CSkillManager::update(float)`: cooldowns
    /// run down, running skills advance, and one whose minimum time is up fires `EVENT_END`
    /// and stops; a copy neither running nor cooling leaves the list. Without `lookup` the
    /// clocks still tick but the group cannot fire.
    void update(float dt, const SkillCastContext& context,
                const std::function<const KnownSkill*(const std::string&)>& lookup);

    /// Every entry, in insertion order, for `--effects-dump` and the tests.
    // PORT: an accessor of the runner.
    const std::deque<std::pair<std::string, SkillRunState>>& entries() const { return entries_; }
    void clear() {
        entries_.clear();
        active_.clear();
    }

private:
    SkillRunState* find(const std::string& skill_name);
    const SkillRunState* find(const std::string& skill_name) const;
    /// The copy `executeSkill(wstring)`
    /// starts: the first of the name that is not running, else a new one.
    SkillRunState& idle_copy(const std::string& skill_name);
    SkillRunState* find_copy(const std::string& skill_name, uint32_t copy);
    /// Stop one copy: `stopSkill` and its
    /// events' `resetEvent`.
    bool stop_copy(const KnownSkill& skill, SkillRunState& state, const SkillCastContext& context);

    /// Insertion order (`TArrayList<CSkill*>::add`), the order `EVENT_END` fires in. A deque,
    /// because a fired group can start another skill while its copy must stay put.
    std::deque<std::pair<std::string, SkillRunState>> entries_;
    /// `CSkillManager +0x1c`, the active list: the copies started (indices into `entries_`),
    /// in the console's order. `update` swap-removes a copy neither running nor cooling.
    std::vector<size_t> active_;
    uint64_t serial_ = 0;
    // PORT: where a copy stands in `entries_`, and whether the active list holds it.
    size_t index_of(const SkillRunState& state) const;
    bool in_active(size_t index) const;
};

}  // namespace tl::core
