// The player's skills as the console's skill screen sees them: skill files, the class's grid,
// skill points and the pad mapping (`CSkill`, `CSkillProperty`, `CSkillManager`, `CCharacter`,
// `CPlayer`; the screen `CSkillMenuXenon`, `CCharSkillsMenuXenon`). Casting is elsewhere.
#pragma once

#include "core/character_effects.h"
#include "formats/adm.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tl::core {

class EffectManager;

/// `ESKILL_ACTIVATION_TYPE`, in the order `CSkillParser`'s table names them
/// (`ANY PROC WEAPON NORMAL PASSIVE`, the static initialiser).
enum class SkillActivation : int32_t {
    Any = 0,
    Proc = 1,
    Weapon = 2,
    Normal = 3,
    Passive = 4,
};

/// `SKILL_TYPE`: `SKILL OFFENSIVE DEFENSIVE CHARM`, the same table. The Xbox
/// screen's type caption reads it.
enum class SkillType : int32_t {
    Skill = 0,
    Offensive = 1,
    Defensive = 2,
    Charm = 3,
};

/// `TARGET_TYPE`, in `gSKILL_TARGET_TYPE_NAMES` order, eleven names;
/// `CCharacter::canCastCurrentSkill` tests `0xb`, one past.
/// `CSkill::requirementsMet` checks only Position, Target,
/// Item and UnidentifiedItem; the rest, `Self` included, go straight to the weapon gate.
enum class SkillTargetType : int32_t {
    None = 0,
    Position = 1,
    Target = 2,
    Self = 3,
    Everybody = 4,
    PositionRandom = 5,
    PositionFlee = 6,
    Item = 7,
    UnidentifiedItem = 8,
    Pets = 9,
    SelfAndPets = 10,
};

/// The four stats the screen's "Bonuses from stat investment" row names, in
/// the order `CSkillProperty::fillOutStatBonuses` fills them.
inline constexpr int kSkillStatBonusCount = 4;

/// One `CSkillProperty`: the skill file's root, or one of its `[LEVELn]`
/// blocks. A level inherits every value it does not author from the root
/// (`CSkillProperty::CSkillProperty`).
struct SkillProperty {
    uint32_t level_required = 0;    ///< `LEVEL_REQUIRED` (+0x44)
    float mana_cost = 0.0f;         ///< `MANACOST` (+0x78)
    float mana_cost_ot = 0.0f;      ///< `MANACOSTOT` (+0x7c)
    float cooldown = 0.0f;          ///< `COOLDOWNMS` / 1000 (+0x98)
    float monster_cooldown = 0.0f;  ///< `MONSTERCOOLDOWNMS` / 1000 (+0x9c)
    /// `MINIMUMTIMEMS` / 1000 (+0xa0): `CSkill::getCanStop` is `elapsed >= this`;
    /// `updateSkill` holds `EVENT_END` until then.
    float minimum_time = 0.0f;
    /// `DURATIONOVERRIDEMS` / 1000 (+0xa4): when positive, `updateSkill` waits for it rather
    /// than for the animation.
    float duration_override = 0.0f;
    /// `RANGE` (+0x80). The constructor's default is **100**, not zero.
    float range = 100.0f;
    /// `RANGEMIN` (+0x84). Constructor default 0.0.
    float range_min = 0.0f;
    /// `RANDOMRANGE` (+0x88). Constructor default 2.0.
    float random_range = 2.0f;
    /// `RANDOMRANGEMIN` (+0x8c). Constructor default 0.0.
    float random_range_min = 0.0f;
    /// `FINDTARGETANGLE` (+0x90), the parent's value as default (0 at the
    /// root). `CSkill::getFindTargetAngle`.
    float find_target_angle = 0.0f;
    /// `FIREATTARGET` (X360 +0x8a; `CSkillProperty::CSkillProperty`), the
    /// parent's as default: `castSkill` turns the caster's final target onto its target.
    bool fire_at_target = false;
    /// `CHANCE` (+0xac), a percentage, default **100**. `CSkill::rollSkillChance`
    /// does not draw at 100 or above; a wasted `g_RandVolatile` draw would move every later roll.
    uint32_t chance = 100;
    /// `TARGET_TYPE` (+0x70), default 0. `SkillTargetType` names the values.
    int32_t target_type = 0;
    /// `TARGET_UNITTYPE` (+0x74, X360 +0x48), the authored name: the original keeps
    /// `getUnitTypeByName`'s id. Empty (0) for none.
    std::string target_unit_type;
    bool single_target = false;       ///< `SINGLETARGET` (+0xb4, X360 +0x88)
    float slowdown = 0.0f;            ///< `SLOWDOWNMS` / 1000 (+0xa8, X360 +0x7c)
    bool interruptable = false;       ///< `INTERRUPTABLE` (+0xb1)
    bool continuous_looping = false;  ///< `CONTINUOUSLOOPING` (+0xb2)
    bool exclusive = false;           ///< `EXCLUSIVE` (+0xb3)
    std::string description;          ///< `DESCRIPTION`, `\n` escapes expanded (+0xf0)
    /// `REQUIREMENT_RIGHT` (+0x10) and `REQUIREMENT_LEFT` (+0x14), kept as the authored name;
    /// the original stores a unit type id (`CResourceManager::getUnitTypeByName`).
    std::string requirement_right;
    std::string requirement_left;
    bool dual_wield_required = false;  ///< `DUELWIELD_REQUIRED` (sic)
    std::string theme_required;        ///< `THEMEREQUIRED` (+0xb8)
    std::string not_themed;            ///< `NOTTHEMED` (+0xc0)
    /// `SPEED` (+0x94), default 1 (`CSkill::getAnimationSpeedMult`): the animation
    /// speed of `castSkill`.
    float animation_speed = 1.0f;
    /// `USEWEAPONANIMATION` (+0xb5): play the held weapon's attack animation
    /// instead of `ANIMATION`.
    bool use_weapon_animation = false;
    /// `ANIMATION`, `ANIMATIONDW`, `ANIMATIONLOOP`, `ANIMATIONDWLOOP`, `ANIMATIONLOOPEND`
    /// (`CSkill::assignSkillAnimations`).
    std::string animation;
    std::string animation_dw;
    std::string animation_loop;
    std::string animation_dw_loop;
    std::string animation_loop_end;
    /// The `[EVENT_*]` / `[AFFIXES]` / `[EFFECT]` blocks, kept whole for the
    /// description text (`CSkillProperty::getSkillStats`, see `skill_text.h`).
    const tl::formats::AdmSection* section = nullptr;
    /// The section whose `[EVENT_*]` groups run: its own when it authors any, else its parent's,
    /// so a `[LEVEL2]` of numbers only casts `[LEVEL1]`'s events.
    const tl::formats::AdmSection* events = nullptr;
    /// The `duration_override` of the property whose section `events` is
    /// (`CSkillEvent::applyEffects`).
    float events_duration_override = 0.0f;
    /// `+0x24`: 0 for the root, n for `[LEVELn]` (`CSkill::CSkill`); the level
    /// an executed skill's text is asked at (`getSkillStats`).
    uint32_t number = 0;
    /// `events`, or the property's own section when nothing set it.
    // PORT: the section a property's events are read from.
    const tl::formats::AdmSection& event_section() const {
        return events != nullptr ? *events : *section;
    }
};

/// What a skill file authors that the screen reads (`CSkill::CSkill`
/// and its root `CSkillProperty`).
struct SkillDef {
    std::string name;           ///< `NAME`, the key a class entry names it by
    std::string display_name;   ///< `DISPLAYNAME`, `\n` escapes expanded
    std::string icon;           ///< `SKILL_ICON`
    std::string icon_inactive;  ///< `SKILL_ICON_INACTIVE`
    SkillActivation activation = SkillActivation::Normal;  ///< `ACTIVATION_TYPE`
    SkillType type = SkillType::Skill;                     ///< `SKILL_TYPE`
    uint32_t max_invest_level = 0;                         ///< `MAX_INVEST_LEVEL` (+0x158)
    bool hidden = false;                                   ///< `HIDDEN` (+0x6b)
    /// `MOVES_TO_TARGET` (+0x68): the caster keeps its path while the animation plays
    /// (`CCharacter::update`).
    bool moves_to_target = false;
    /// `ALLOWS_TURNING` (+0x66), true unless authored: clear, the caster turns along its aim,
    /// not towards its target, while the cast has 0.1 s or more to run.
    bool allows_turning = true;
    bool requires_passable_los = false;  ///< `REQUIRES_PASSABLE_LOS` (`CSkill +0x69`)
    /// `DONT_STOP_ON_DEATH` (X360 `CSkill +0x3f`): `stopAllSkills`' second flag spares the skill.
    bool dont_stop_on_death = false;
    /// `CAN_BE_SILENCED` (X360 `CSkill +0x48`), true unless authored.
    bool can_be_silenced = true;
    /// `REQUIRES_PATHABLE` (`+0x6a`), read by `getRequiresPathable` only while the
    /// skill has a level.
    bool requires_pathable = false;
    int64_t guid = -1;  ///< `UNIQUE_GUID` (+0x150)
    /// `TARGET_ALIGNMENT` (+0x120), `EAlignment`: 0 `NEUTRAL`, 1 `GOOD`,
    /// 2 `EVIL` (the constructor's default), 3 `ALL`, then the berserk three.
    int32_t target_alignment = 2;
    /// `TARGET_CORPSES` (+0x6e): `getTargetIsValid` wants the
    /// dead; `CLevel::findCharacterWithinView` lets them by.
    bool target_corpses = false;
    /// `[TARGET_SPECIFIC_UNITS]` (+0x78, count +0x80): every string value, upper-cased. The
    /// original keeps the unit's data group (`getDataGroupByObjectName`); the port the name.
    std::vector<std::string> target_specific_units;
    SkillProperty base;                 ///< the root (+0x90)
    std::vector<SkillProperty> levels;  ///< `[LEVEL1]`.. (+0xa0, count +0xa8)
};

/// Read a skill file's root. The returned `SkillProperty::section` pointers
/// point into `file`, which must outlive the result.
SkillDef parse_skill_def(const tl::formats::AdmFile& file);

/// One `[SKILL]` block of a class's unit file: which skill, and where on the
/// (Xbox, two-column) skill screen it sits.
struct ClassSkill {
    std::string name;             ///< `NAME`
    int32_t pane = 0;             ///< `PANE`, 0..2
    int32_t row = -1;             ///< `ROW`, 0..5
    int32_t column = -1;          ///< `COLUMN`, 0..1
    uint32_t level_required = 1;  ///< `LEVEL_REQUIRED` (+0xd8, default 1)
    uint32_t level = 0;           ///< `LEVEL`: what the class starts with
    /// `SKILL_REQUIRED`: a skill that must hold a level first.
    std::string skill_required;
    /// `ENABLED` (CSkill X360 +0x45): the block's, else the unit's, else true.
    bool enabled = true;
    /// `CHARGES` (X360 +0xec): the block's, else the unit's, else -1 - over the
    /// skill file's own.
    int32_t charges = -1;
    /// `CHANCE` (X360 +0xe0, default 40) and `CANCEL_CHANCE` (+0xe4, default 0).
    int32_t chance = 40;
    int32_t cancel_chance = 0;
    /// `ANIMATION_OVERRIDE` (X360 +0xf0), `ANIMATION_OVERRIDEDW` (+0x10c),
    /// `ANIMATION_OVERRIDELOOP` (+0x128), `ANIMATION_OVERRIDELOOPDW` (+0x144).
    std::string animation_override;
    std::string animation_override_dw;
    std::string animation_override_loop;
    std::string animation_override_loop_dw;
};

/// The `[SKILL]` blocks under a unit file's root, in file order (`CBaseUnit::unitInit`):
/// an empty `NAME` is skipped; `LEVEL` falls back to the
/// root's, then to `unit_level` (`+0xf0`).
std::vector<ClassSkill> parse_class_skills(const tl::formats::AdmFile& file,
                                           uint32_t unit_level = 0);

/// `CCharacter::getSkillTabName(n)` for n =
/// 1..3: the class file's `SKILL_TAB<n>`, and `Skills` when it authors none.
std::array<std::string, 3> parse_skill_tab_names(const tl::formats::AdmFile& file);

/// One skill the player holds: a class entry and its file, with the levels.
struct KnownSkill {
    ClassSkill entry;
    const SkillDef* def = nullptr;  ///< null when no file names it
    uint32_t invested = 0;          ///< `+0xdc` (Xbox `+0xa4`): points spent
    uint32_t effective = 0;         ///< `+0xe0` (Xbox `+0xa8`): what the skill runs at
    /// +0x6d, default 1; switched by CAISkillManager::skillAdded/skillRemoved.
    bool enabled = true;
    /// Added by `CCharacter::learnSpell`, not the class: on no
    /// grid cell, saved by name in the four spell slots.
    bool spell = false;
    /// `+0x124`, `CHARGES`: executions left, -1 for no limit. From the file (`CSkill::CSkill`),
    /// the unit's `[SKILL]` (`CBaseUnit::unitInit`)
    /// or an effect's `MAX` (`activateEffect`).
    /// Mutable: every execution counts it down through a const view.
    mutable int32_t charges = -1;

    // PORT: an accessor of a known skill.
    int64_t guid() const { return def != nullptr ? def->guid : -1; }
    bool passive() const { return def != nullptr && def->activation == SkillActivation::Passive; }
    /// How many `[LEVELn]` blocks the file has, at least one (`+0xa8`, 1 when
    /// zero, the fallback every reader of it applies).
    uint32_t level_count() const;
    /// The rank's denominator, `MAX_INVEST_LEVEL` or the level count
    /// (Xbox `+0x188`, else `+0x68`, else 1:).
    uint32_t max_rank() const;
    /// The property for level `level` (1-based), or the root when the file has
    /// no such level (`CSkill::getSkillLevelDescription` the function's lookup).
    const SkillProperty* property(uint32_t level) const;
    /// `CSkill +0x5c`: level property `effective - 1`, the last past them, the root with no
    /// levels or no effective level (`CSkill::_setLevelOfSkillFromSkillManager`).
    /// Null with no def.
    const SkillProperty* current_property() const;
};

/// The pad's function slots: ten (X360 `+0x840`, "skill slots") and the ten beside them
/// (X360 `+0x890`, "second skill slots") that a mapping clears (map handler,
/// `CGameClient::loadCharacter`).
inline constexpr int kSkillFunctionSlots = 10;

/// `CPlayer`'s other two lists of function skills, twelve each (Linux `+0x950`
/// and `+0x9b0`, X360 `+0x8e0` and `+0x940`), saved as the file's pairs.
inline constexpr int kMappedFunctionSlots = 12;

/// The four buttons a skill can be mapped to on the console, and the function
/// slot each writes in each of the two skill sets (`CSkillMenuXenon`'s map
/// handler). The layout's `SkillSlotN` windows are slot N-1.
enum class SkillButton { Y, B, LT, RT };
int skill_button_slot(SkillButton button, int skill_set);

/// The skills a player can see on the screen, with the points to spend and
/// the mapping. `CSkillManager` plus the player's side of it.
class SkillBook {
public:
    /// Rebuild from a class's `[SKILL]` blocks. `lookup` finds a skill file by
    /// `NAME` (case-insensitively, as `CSkillParser` upper-cases the key).
    /// Starting levels are the class's `LEVEL`.
    void set_class(const std::vector<ClassSkill>& entries,
                   const std::function<const SkillDef*(const std::string&)>& lookup);

    // PORT: an accessor of the port's skill book.
    const std::vector<KnownSkill>& skills() const { return skills_; }
    std::vector<KnownSkill>& skills() { return skills_; }
    KnownSkill* find_by_guid(int64_t guid);
    const KnownSkill* find_by_guid(int64_t guid) const;
    KnownSkill* find_by_name(const std::string& name);
    const KnownSkill* find_by_name(const std::string& name) const;
    /// The skill at a grid cell, or null.
    const KnownSkill* at(int pane, int row, int column) const;

    /// `CSkill::getLevelRequiredForInvestment`.
    static uint32_t level_required_for_investment(const KnownSkill& skill);
    /// `CSkill::calculateEffectiveSkillLevel`; returns whether
    /// it changed. `owner` and `pet` (`CCharacter +0x640`) give the `+N` skill bonuses, each the
    /// unit's total (`CCharacter::getEffectValue`); null is nothing applied.
    static bool calculate_effective_level(KnownSkill& skill, CharacterEffects owner = nullptr,
                                          CharacterEffects pet = nullptr);
    /// The screen's learn test. `points` is the character's unspent skill
    /// points (`CCharacter +0x460`, Xbox `+0x3c8`).
    bool can_learn(const KnownSkill& skill, uint32_t player_level, int points) const;
    /// `A` on the screen (button 5): a point off `points`, the
    /// skill a level up (`CSkillManager::setSkillLevel`).
    bool learn(KnownSkill& skill, uint32_t player_level, int& points);

    /// Whether the map handler may put it on a button.
    static bool can_map(const KnownSkill& skill);
    /// The map handler's write: the GUID into the slot.
    bool map(const KnownSkill& skill, SkillButton button, int skill_set);
    int64_t slot(int index) const;
    // PORT: an accessor of the port's skill book.
    const std::array<int64_t, kSkillFunctionSlots>& slots() const { return slots_; }
    const std::array<int64_t, kSkillFunctionSlots>& second_slots() const { return second_slots_; }
    /// The slots as `CGameClient::loadCharacter` reads them:
    /// each skill clears its slot's item, then each item its slot's skill.
    void set_slots(const std::vector<int64_t>& slots, const std::vector<int64_t>& second);
    /// `CPlayer::setMappedFunctionSkill(unsigned, long long)` (X360 inline in
    /// the loader): the GUID out of every other slot of the first list, into `index`, and that
    /// slot of the second list cleared when the GUID is a real one.
    void set_mapped_function_skill(int index, int64_t guid);
    /// `CPlayer::setLeftMappedFunctionSkill(unsigned, long long)`
    /// (X360 inline): the same on the second list, clearing the first.
    void set_left_mapped_function_skill(int index, int64_t guid);
    const std::array<int64_t, kMappedFunctionSlots>& mapped_function_skills() const {
        return mapped_;
    }
    const std::array<int64_t, kMappedFunctionSlots>& left_mapped_function_skills() const {
        return left_mapped_;
    }
    /// The file's pairs, in order, through the two setters as the loader calls
    /// them (the second of each pair from save version 12 on).
    void set_mapped_pairs(const std::vector<std::pair<int64_t, int64_t>>& pairs);
    std::vector<std::pair<int64_t, int64_t>> mapped_pairs() const;
    /// An item's `UNIT_GUID` into slot `index`'s item half, the skill half cleared when the
    /// GUID is a real one: `CPlayer::unitInit`'s defaults.
    void set_item_slot(int index, int64_t guid);
    /// `CPlayer::unitInit`'s skill half: the active skill is the manager's first, or
    /// `CCharacter::cycleSkill(1)`'s pick when that one
    /// cannot be used, its GUID into `kDefaultSkillSlot`. Returns it, or null.
    const KnownSkill* bind_default_skill_slot();
    /// RT in the first skill set (Xbox `+0x870`); the PC writes slot 2.
    static constexpr int kDefaultSkillSlot = 6;

    /// Which of the two sets the pad's four buttons fire (`CGameUI +0x1464`,
    /// switched from the D-pad).
    int skill_set() const { return skill_set_; }
    void set_skill_set(int set) { skill_set_ = set == 0 ? 0 : 1; }

    /// The four spells learned from scrolls, by name (`+1000`, saved at the
    /// player state's `+0x118`/`+0x138`).
    std::array<std::string, 4> spells;
    std::array<int32_t, 4> spell_levels{};

    /// `CCharacter::canLearnSpell(std::wstring, int)`: not
    /// when `name` is held at exactly `level` (`CSkillManager::getSkill(name, level)`)
    /// or sits in a slot at any level (console only); an empty slot (first
    /// two for a pet, all four else) only with `need_empty_slot` (the tray, passes
    /// false so A on a scroll reaches a full book's replace dialog).
    bool can_learn_spell(const std::string& name, int32_t level, bool pet,
                         bool need_empty_slot = true) const;
    /// `CCharacter::getKnownSpell(unsigned)`: the
    /// skill a slot names, at its level, when the manager holds it.
    const KnownSkill* known_spell(size_t slot) const;
    /// `CCharacter::unLearnSpell(int)` ->
    /// `unLearnSpell(std::wstring, int)`: the slot's skill out
    /// of the manager, the later slots moved up one. Nothing for an empty or out-of-range slot.
    void unlearn_spell(size_t slot);
    /// `CCharacter::learnSpell(std::wstring, int)`: into the
    /// first empty slot (nothing when none is), then `CBaseUnit::addSkillByName` and
    /// `CSkillManager::setSkillLevel(level)`. A null `def` keeps the slot. Returns the skill.
    KnownSkill* learn_spell(const std::string& name, int32_t level, const SkillDef* def);
    /// `CPlayer::resetSkills`, the skill manager's half: the
    /// points held (`CSkillManager::getSkillPoints`) counted, the skills zeroed
    /// (`CSkillManager::resetSkillLevels`), each class `[SKILL]` set to its `LEVEL`
    /// (`setSkillLevel(name, level)`) and taken off the count. Returns what is left,
    /// at least 0, for the caller to add to the unspent points.
    int reset_skills();

    /// The four slots emptied and their skills dropped, before a saved
    /// character's are learned again (`CCharacter::applySaveState`).
    ///
    void clear_spells();

    /// The class's three pane names (`parse_skill_tab_names`).
    std::array<std::string, 3> tab_names{"Skills", "Skills", "Skills"};

    /// The player's and the pet's effect managers; setting either refreshes every
    /// skill's effective level.
    void set_effect_managers(CharacterEffects owner, CharacterEffects pet);

    /// Invested levels in class order, for the save file's `+0x1e8`, and back.
    std::vector<int32_t> levels() const;
    void set_levels(const std::vector<int32_t>& levels);

private:
    std::vector<KnownSkill> skills_;
    std::array<int64_t, kSkillFunctionSlots> slots_{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    std::array<int64_t, kSkillFunctionSlots> second_slots_{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    std::array<int64_t, kMappedFunctionSlots> mapped_{-1, -1, -1, -1, -1, -1,
                                                      -1, -1, -1, -1, -1, -1};
    std::array<int64_t, kMappedFunctionSlots> left_mapped_{-1, -1, -1, -1, -1, -1,
                                                           -1, -1, -1, -1, -1, -1};
    int skill_set_ = 0;
    CharacterEffects owner_effects_;
    CharacterEffects pet_effects_;
};

/// The skill screen's cursor (`CCharSkillsMenuXenon +0x650..+0x656`, `+0xb5`):
/// a cell of the three six-by-two panes, or one of the four spell slots under
/// them.
struct SkillCursor {
    int pane = 0;            ///< +0x650
    int row = 0;             ///< +0x652
    int column = 0;          ///< +0x654
    int spell = 0;           ///< +0x656
    bool in_spells = false;  ///< +0xb5

    /// The index the screen's forty-GUID table is read:
    /// `(pane * 6 + row) * 2 + column`, at most 35, or `36 + spell`.
    int index() const;
};

inline constexpr int kSkillPanes = 3;
inline constexpr int kSkillRows = 6;
inline constexpr int kSkillColumns = 2;
inline constexpr int kSkillSpellSlots = 4;

/// Whether a cell's frame is shown: a class skill sits in it. Each move below recurses
/// while the cell it landed on is hidden.
using SkillCellShown = std::function<bool(const SkillCursor&)>;

/// The four moves (up, down, right,
/// left).
void skill_cursor_up(SkillCursor& cursor, const SkillCellShown& shown);
void skill_cursor_down(SkillCursor& cursor, const SkillCellShown& shown);
void skill_cursor_right(SkillCursor& cursor, const SkillCellShown& shown);
void skill_cursor_left(SkillCursor& cursor, const SkillCellShown& shown);

}  // namespace tl::core
