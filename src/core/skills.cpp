#include "core/skills.h"

#include "core/effect_manager.h"
#include "formats/float_convert.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace tl::core {
namespace {

// PORT: ASCII upper-casing, the case-blind compare of the skill parser's keys.
std::string upper(std::string text) {
    for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

/// `STRINGS::replaceWString(text, L"\\n", L"\n")`, which `CSkillProperty`
/// applies to `DESCRIPTION` and the Xbox name line to the name.
std::string expand_newlines(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'n') {
            out.push_back('\n');
            ++i;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

/// `CSkill::CSkill`: the word upper-cased and looked up in
/// the five names; 0 (`ANY`) stays when the key is absent, empty or names none of them.
SkillActivation activation_of(const std::string& word) {
    const std::string w = upper(word);
    if (w == "PROC") return SkillActivation::Proc;
    if (w == "WEAPON") return SkillActivation::Weapon;
    if (w == "NORMAL") return SkillActivation::Normal;
    if (w == "PASSIVE") return SkillActivation::Passive;
    return SkillActivation::Any;
}

// PORT: asset parsing, `SKILL_TYPE`'s word as the port's enum.
SkillType type_of(const std::string& word) {
    const std::string w = upper(word);
    if (w == "OFFENSIVE") return SkillType::Offensive;
    if (w == "DEFENSIVE") return SkillType::Defensive;
    if (w == "CHARM") return SkillType::Charm;
    return SkillType::Skill;
}

/// `gSKILL_TARGET_TYPE_NAMES` (filled, twelve names), matched upper-cased; an
/// unauthored or unknown word leaves the parent's value.
int32_t target_type_of(const std::string& word, int32_t fallback) {
    static const char* const kNames[] = {
        "NONE",         "POSITION", "TARGET",           "SELF", "EVERYBODY",   "POSITIONRANDOM",
        "POSITIONFLEE", "ITEM",     "UNIDENTIFIEDITEM", "PETS", "SELFANDPETS", "TARGET_POS",
    };
    const std::string w = upper(word);
    if (w.empty()) return fallback;
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
        if (w == kNames[i]) return static_cast<int32_t>(i);
    }
    return fallback;
}

/// How every `*MS` key of a skill property is read: integer milliseconds, defaulting to the
/// parent's seconds back in milliseconds **rounded up** (the `ceilf` is the original's;
/// without it 0.35 s comes back as 349 ms). Console arithmetic (`CSkillProperty::
/// CSkillProperty`): ms are **multiplied by 0.001f**.
float ms_to_seconds(int32_t ms) { return static_cast<float>(ms) * 0.001f; }

// PORT: asset parsing, a millisecond key read with the parent's seconds as its
// fallback (`fctiwz(ceil(seconds * 1000))`).
float read_ms(const tl::formats::AdmSection& section, const char* key, float parent_seconds) {
    const uint32_t fallback =
        static_cast<uint32_t>(tl::formats::ppc_fctiwz(std::ceil(parent_seconds * 1000.0f)));
    return ms_to_seconds(static_cast<int32_t>(section.integer_or(key, fallback)));
}

/// `CSkillProperty::CSkillProperty`: each key
/// read with the parent's value as its default.
SkillProperty read_property(const tl::formats::AdmSection& section, const SkillProperty* parent) {
    SkillProperty p;
    const SkillProperty none;
    const SkillProperty& from = parent != nullptr ? *parent : none;
    p.section = &section;
    // `gSKILL_EVENT_TYPE_NAMES`: does this property author any event group?
    static const char* const kEventGroups[] = {
        "EVENT_START",       "EVENT_END",       "EVENT_TRIGGER",    "EVENT_TRIGGER_TWO",
        "EVENT_UNITHIT",     "EVENT_UNITDIE",   "EVENT_MISSILEHIT", "EVENT_MISSILEDIE",
        "EVENT_DIEBYEFFECT", "EVENT_CASTERDIE", "EVENT_UNIT_CREATE"};
    bool own_events = false;
    for (const tl::formats::AdmSection& child : section.children) {
        const std::string name = upper(child.name);
        for (const char* group : kEventGroups) own_events = own_events || name == group;
    }
    // None: the parent's events, when the parent has any.
    p.events =
        own_events || parent == nullptr || parent->events == nullptr ? &section : parent->events;
    p.level_required = section.integer_or("LEVEL_REQUIRED", from.level_required);
    p.mana_cost = section.float_or("MANACOST", from.mana_cost);
    p.mana_cost_ot = section.float_or("MANACOSTOT", from.mana_cost_ot);
    // `COOLDOWNMS` is an integer of milliseconds; the default handed to the
    // read is the parent's seconds back in milliseconds, rounded up.
    p.cooldown = read_ms(section, "COOLDOWNMS", from.cooldown);
    // `MONSTERCOOLDOWNMS` defaults to the parent's, rounded up; at the root,
    // to this property's own cooldown, not rounded.
    const int32_t monster_default =
        parent != nullptr ? tl::formats::ppc_fctiwz(std::ceil(from.monster_cooldown * 1000.0f))
                          : tl::formats::ppc_fctiwz(p.cooldown * 1000.0f);
    p.monster_cooldown = ms_to_seconds(static_cast<int32_t>(
        section.integer_or("MONSTERCOOLDOWNMS", static_cast<uint32_t>(monster_default))));
    // `MINIMUMTIMEMS` (+0xa0) and `DURATIONOVERRIDEMS` (+0xa4) are the same
    // milliseconds-with-the-parent's-seconds-rounded-up shape as the cooldown.
    p.minimum_time = read_ms(section, "MINIMUMTIMEMS", from.minimum_time);
    p.duration_override = read_ms(section, "DURATIONOVERRIDEMS", from.duration_override);
    p.range = section.float_or("RANGE", from.range);
    p.range_min = section.float_or("RANGEMIN", from.range_min);
    p.random_range = section.float_or("RANDOMRANGE", from.random_range);
    p.random_range_min = section.float_or("RANDOMRANGEMIN", from.random_range_min);
    p.find_target_angle = section.float_or("FINDTARGETANGLE", from.find_target_angle);
    p.fire_at_target = section.bool_or("FIREATTARGET", from.fire_at_target);
    p.chance = section.integer_or("CHANCE", from.chance);
    // `CSkillProperty::CSkillProperty`: `TARGET_TYPE`, and when that
    // is empty `TARGET` - which is the key the shipped skills author (Nether
    // Imp's `TARGET:POSITION`, Ember Shield's `TARGET:SELF`).
    std::string target_word = section.string_or("TARGET_TYPE", "");
    if (target_word.empty()) target_word = section.string_or("TARGET", "");
    p.target_type = target_type_of(target_word, from.target_type);
    // `TARGET_UNITTYPE` and `SINGLETARGET`, each the default
    // property's when not authored.
    p.target_unit_type = section.string_or("TARGET_UNITTYPE", "");
    if (p.target_unit_type.empty()) p.target_unit_type = from.target_unit_type;
    p.single_target = section.bool_or("SINGLETARGET", from.single_target);
    p.slowdown = read_ms(section, "SLOWDOWNMS", from.slowdown);
    p.interruptable = section.bool_or("INTERRUPTABLE", from.interruptable);
    p.continuous_looping = section.bool_or("CONTINUOUSLOOPING", from.continuous_looping);
    p.exclusive = section.bool_or("EXCLUSIVE", from.exclusive);
    p.description = expand_newlines(section.string_or("DESCRIPTION", from.description));
    p.requirement_right = section.string_or("REQUIREMENT_RIGHT", from.requirement_right);
    p.requirement_left = section.string_or("REQUIREMENT_LEFT", from.requirement_left);
    p.dual_wield_required = section.bool_or("DUELWIELD_REQUIRED", from.dual_wield_required);
    p.theme_required = section.string_or("THEMEREQUIRED", from.theme_required);
    p.not_themed = section.string_or("NOTTHEMED", from.not_themed);
    p.animation_speed = section.float_or("SPEED", from.animation_speed);
    p.use_weapon_animation = section.bool_or("USEWEAPONANIMATION", from.use_weapon_animation);
    p.animation = section.string_or("ANIMATION", from.animation);
    p.animation_dw = section.string_or("ANIMATIONDW", from.animation_dw);
    p.animation_loop = section.string_or("ANIMATIONLOOP", from.animation_loop);
    p.animation_dw_loop = section.string_or("ANIMATIONDWLOOP", from.animation_dw_loop);
    p.animation_loop_end = section.string_or("ANIMATIONLOOPEND", from.animation_loop_end);
    // the five upper-cased (`STRINGS::StringUpper`), since
    // `CGenericModel::findRandomAnimation` compares its prefix as passed.
    p.animation = upper(p.animation);
    p.animation_dw = upper(p.animation_dw);
    p.animation_loop = upper(p.animation_loop);
    p.animation_dw_loop = upper(p.animation_dw_loop);
    p.animation_loop_end = upper(p.animation_loop_end);
    // a looping animation with no positive duration override
    // runs for one second (a NaN too).
    if (!p.animation_loop.empty() && !(p.duration_override > 0.0f)) p.duration_override = 1.0f;
    // inherited events are the parent's own `CSkillEvent`s, so they keep the
    // parent's duration override.
    // Without a parent `p.events` is `&section` (above), so `parent` is never null here.
    // NOLINTBEGIN(clang-analyzer-core.NullDereference)
    p.events_duration_override =
        p.events == &section ? p.duration_override : parent->events_duration_override;
    // NOLINTEND(clang-analyzer-core.NullDereference)
    return p;
}

}  // namespace

SkillDef parse_skill_def(const tl::formats::AdmFile& file) {
    // CSkill::CSkill
    const tl::formats::AdmSection& root = file.root;
    SkillDef def;
    def.name = root.string_or("NAME", "");
    def.display_name = expand_newlines(root.string_or("DISPLAYNAME", def.name));
    def.icon = root.string_or("SKILL_ICON", "");
    // `CSkillProperty::CSkillProperty`: at the
    // root, `SKILL_ICON_INACTIVE` defaults to the `SKILL_ICON` just read.
    def.icon_inactive = root.string_or("SKILL_ICON_INACTIVE", def.icon);
    def.activation = activation_of(root.string_or("ACTIVATION_TYPE", ""));
    def.type = type_of(root.string_or("SKILL_TYPE", "SKILL"));
    def.max_invest_level = root.integer_or("MAX_INVEST_LEVEL", 0);
    def.hidden = root.bool_or("HIDDEN", false);
    def.moves_to_target = root.bool_or("MOVES_TO_TARGET", false);
    def.allows_turning = root.bool_or("ALLOWS_TURNING", true);
    def.requires_passable_los = root.bool_or("REQUIRES_PASSABLE_LOS", false);
    def.dont_stop_on_death = root.bool_or("DONT_STOP_ON_DEATH", false);
    def.can_be_silenced = root.bool_or("CAN_BE_SILENCED", true);
    def.requires_pathable = root.bool_or("REQUIRES_PATHABLE", false);
    def.guid = root.guid_or("UNIQUE_GUID", -1);
    // `CSkill::CSkill`: `TARGET_ALIGNMENT`
    // against `KALIGNMENT_STRINGS`, left at 2 (`EVIL`) when unauthored or
    // unknown.
    {
        static const char* const kAlignments[] = {"NEUTRAL", "GOOD",        "EVIL",       "ALL",
                                                  "BERSERK", "EVILBERSERK", "GOODBERSERK"};
        const std::string word = upper(root.string_or("TARGET_ALIGNMENT", ""));
        for (size_t i = 0; i < sizeof(kAlignments) / sizeof(kAlignments[0]); ++i) {
            if (!word.empty() && word == kAlignments[i])
                def.target_alignment = static_cast<int32_t>(i);
        }
    }
    // `TARGET_SPECIFIC_UNITS` (`GetDataGroupByName`, matched exactly): its
    // `STRING` and `TRANSLATE` values, the key unread. Then `TARGET_CORPSES`.
    for (const tl::formats::AdmSection& child : root.children) {
        if (child.name != "TARGET_SPECIFIC_UNITS") continue;
        for (const tl::formats::AdmRecord& record : child.records) {
            if (record.type == tl::formats::AdmType::String ||
                record.type == tl::formats::AdmType::Translate) {
                def.target_specific_units.push_back(upper(record.text));
            }
        }
        break;
    }
    def.target_corpses = root.bool_or("TARGET_CORPSES", false);
    def.base = read_property(root, nullptr);
    // `[LEVEL1]`, `[LEVEL2]`, ... in order. Each takes its defaults from the
    // level before it, the first from the root
    // (the parent is the list's last property while it has one).
    for (uint32_t level = 1;; ++level) {
        const std::string want = "LEVEL" + std::to_string(level);
        const tl::formats::AdmSection* found = nullptr;
        for (const tl::formats::AdmSection& child : root.children) {
            if (upper(child.name) == want) {
                found = &child;
                break;
            }
        }
        if (found == nullptr) break;
        SkillProperty property =
            read_property(*found, def.levels.empty() ? &def.base : &def.levels.back());
        property.number = static_cast<uint32_t>(def.levels.size() + 1);
        def.levels.push_back(std::move(property));
    }
    return def;
}

std::vector<ClassSkill> parse_class_skills(const tl::formats::AdmFile& file, uint32_t unit_level) {
    // `CBaseUnit::unitInit`: the `SKILL` groups
    // (`GetDataGroupsMatchingName`), each one's keys read before `addSkillByName`,
    // then written onto the skill it made, its level last (`setSkillLevel`).
    std::vector<ClassSkill> out;
    const tl::formats::AdmSection& root = file.root;
    for (const tl::formats::AdmSection& child : root.children) {
        if (upper(child.name) != "SKILL") continue;
        ClassSkill entry;
        entry.name = child.string_or("NAME", "");
        if (entry.name.empty()) continue;
        entry.level = child.integer_or("LEVEL", root.integer_or("LEVEL", unit_level));
        entry.level_required = child.integer_or("LEVEL_REQUIRED", 1);
        entry.skill_required = child.string_or("SKILL_REQUIRED", "");
        entry.column = static_cast<int32_t>(child.integer_or("COLUMN", 0xffffffffu));
        entry.row = static_cast<int32_t>(child.integer_or("ROW", 0xffffffffu));
        entry.pane = static_cast<int32_t>(child.integer_or("PANE", 0));
        entry.charges = static_cast<int32_t>(
            child.integer_or("CHARGES", root.integer_or("CHARGES", 0xffffffffu)));
        // `ENABLED` is read as its byte (`+8` of the value).
        const tl::formats::AdmRecord* enabled = child.find("ENABLED");
        if (enabled == nullptr) enabled = root.find("ENABLED");
        entry.enabled = enabled == nullptr || enabled->word_first_byte() != 0;
        entry.animation_override = child.string_or("ANIMATION_OVERRIDE", "");
        entry.animation_override_dw = child.string_or("ANIMATION_OVERRIDEDW", "");
        entry.animation_override_loop = child.string_or("ANIMATION_OVERRIDELOOP", "");
        entry.animation_override_loop_dw = child.string_or("ANIMATION_OVERRIDELOOPDW", "");
        entry.chance = static_cast<int32_t>(child.integer_or("CHANCE", 40));
        entry.cancel_chance = static_cast<int32_t>(child.integer_or("CANCEL_CHANCE", 0));
        out.push_back(entry);
    }
    return out;
}

std::array<std::string, 3> parse_skill_tab_names(const tl::formats::AdmFile& file) {
    // CCharacter::getSkillTabName
    std::array<std::string, 3> out;
    for (int i = 0; i < 3; ++i) {
        out[static_cast<size_t>(i)] =
            file.root.string_or("SKILL_TAB" + std::to_string(i + 1), "Skills");
    }
    return out;
}

// How many `[LEVELn]` blocks the file has, at least one (`CSkill +0xa8`, 1 when
// zero). PORT: an accessor.
uint32_t KnownSkill::level_count() const {
    if (def == nullptr || def->levels.empty()) return 1;
    return static_cast<uint32_t>(def->levels.size());
}

// The rank's denominator, `MAX_INVEST_LEVEL` or the level count (Xbox `+0x188`,
// else `+0x68`, else 1:).
uint32_t KnownSkill::max_rank() const {
    if (def != nullptr && def->max_invest_level != 0) return def->max_invest_level;
    if (def != nullptr && !def->levels.empty()) return static_cast<uint32_t>(def->levels.size());
    return 1;
}

const SkillProperty* KnownSkill::property(uint32_t level) const {
    // CSkill::getSkillLevelDescription: `level - 1`, and a level of
    // zero (or the -1 "current" the callers pass) means the effective one.
    if (def == nullptr) return nullptr;
    uint32_t index = level - 1;
    if (0xfffffffdu < index) index = effective - 1;
    const uint32_t count = static_cast<uint32_t>(def->levels.size());
    const uint32_t bound = count == 0 ? 1 : count;
    if (index <= bound && index < count) return &def->levels[index];
    return count == 0 ? &def->base : &def->levels[0];
}

// `CSkill +0x5c`, the property the skill runs with
// (`CSkill::_setLevelOfSkillFromSkillManager`): level
// property `effective - 1`, the last past them, the root with no levels or effective level.
const SkillProperty* KnownSkill::current_property() const {
    if (def == nullptr) return nullptr;
    const uint32_t count = static_cast<uint32_t>(def->levels.size());
    if (count == 0 || effective == 0) return &def->base;
    const uint32_t index = effective - 1 < count ? effective - 1 : count - 1;
    return &def->levels[index];
}

int skill_button_slot(SkillButton button, int skill_set) {
    // CSkillMenuXenon's map handler: Y 8 -> 3 / 2, B 6 -> 4 / 7,
    // RT 9 -> 6 / 9, the fourth (LT) -> 5 / 8, by `CGameUI +0x1464`.
    const bool second = skill_set != 0;
    switch (button) {
        case SkillButton::Y: return second ? 2 : 3;
        case SkillButton::B: return second ? 7 : 4;
        case SkillButton::RT: return second ? 9 : 6;
        case SkillButton::LT: return second ? 8 : 5;
    }
    return -1;
}

// PORT: the book made from a class's `[SKILL]` blocks (`parse_class_skills`), each
// skill's effective level worked out (`calculate_effective_level`).
void SkillBook::set_class(const std::vector<ClassSkill>& entries,
                          const std::function<const SkillDef*(const std::string&)>& lookup) {
    skills_.clear();
    for (const ClassSkill& entry : entries) {
        KnownSkill skill;
        skill.entry = entry;
        skill.def = lookup ? lookup(entry.name) : nullptr;
        // What `CBaseUnit::unitInit` writes over the skill
        // `addSkillByName` made.
        skill.enabled = entry.enabled;
        skill.charges = entry.charges;
        skill.invested = entry.level;
        calculate_effective_level(skill, owner_effects_, pet_effects_);
        skills_.push_back(skill);
    }
}

// PORT: a skill of the book by GUID.
KnownSkill* SkillBook::find_by_guid(int64_t guid) {
    if (guid == -1) return nullptr;
    for (KnownSkill& skill : skills_) {
        if (skill.guid() == guid) return &skill;
    }
    return nullptr;
}

// PORT: a skill of the book by GUID.
const KnownSkill* SkillBook::find_by_guid(int64_t guid) const {
    return const_cast<SkillBook*>(this)->find_by_guid(guid);
}

// PORT: a skill of the book by name, case-blind.
KnownSkill* SkillBook::find_by_name(const std::string& name) {
    const std::string want = upper(name);
    for (KnownSkill& skill : skills_) {
        if (upper(skill.entry.name) == want) return &skill;
    }
    return nullptr;
}

// PORT: a skill of the book by name, case-blind.
const KnownSkill* SkillBook::find_by_name(const std::string& name) const {
    return const_cast<SkillBook*>(this)->find_by_name(name);
}

// PORT: the skill at a cell of the tab.
const KnownSkill* SkillBook::at(int pane, int row, int column) const {
    for (const KnownSkill& skill : skills_) {
        if (skill.entry.pane == pane && skill.entry.row == row && skill.entry.column == column) {
            return &skill;
        }
    }
    return nullptr;
}

uint32_t SkillBook::level_required_for_investment(const KnownSkill& skill) {
    // CSkill::getLevelRequiredForInvestment: `levels[invested]`'s `LEVEL_REQUIRED`
    // when it authors one, else the class entry's plus the points already in.
    const uint32_t invested = skill.invested;
    const SkillProperty* property = nullptr;
    if (skill.def != nullptr) {
        const uint32_t count = static_cast<uint32_t>(skill.def->levels.size());
        const uint32_t bound = count == 0 ? 1 : count;
        property = count == 0 ? &skill.def->base : &skill.def->levels[0];
        if (static_cast<int>(invested) <= static_cast<int>(bound) &&
            static_cast<int>(invested) < static_cast<int>(count)) {
            property = &skill.def->levels[invested];
        }
    }
    if (property != nullptr && property->level_required != 0) return property->level_required;
    return skill.entry.level_required + invested;
}

namespace {

/// The effect type that carries `+N to all <type> spells`, by `SKILL_TYPE`; a class skill
/// has none. `CSkill::calculateEffectiveSkillLevel`'s
/// three-way test.
int all_spells_effect_type(SkillType type) {
    switch (type) {
        case SkillType::Offensive: return kEffectTypeAttackSpellBonus;
        case SkillType::Defensive: return kEffectTypeDefenseSpellBonus;
        case SkillType::Charm: return kEffectTypeCharmSpellBonus;
        case SkillType::Skill: break;
    }
    return -1;
}

/// One unit's contribution to a skill's effective level: `SKILL BONUS` effects naming it
/// plus the all-spells bonus for its type, each sum truncated to int separately.
/// `CSkill::calculateEffectiveSkillLevel`.
int skill_level_bonus(const CharacterEffects& effects, const KnownSkill& skill) {
    const std::string& name =
        skill.def != nullptr && !skill.def->name.empty() ? skill.def->name : skill.entry.name;
    int bonus = static_cast<int>(effects.effect_value(kEffectTypeSkillBonus, name));
    const int all_spells =
        all_spells_effect_type(skill.def != nullptr ? skill.def->type : SkillType::Skill);
    if (all_spells >= 0) {
        bonus += static_cast<int>(effects.effect_value(all_spells, kDamageTypeAny));
    }
    return bonus;
}

}  // namespace

bool SkillBook::calculate_effective_level(KnownSkill& skill, CharacterEffects owner,
                                          CharacterEffects pet) {
    // CSkill::calculateEffectiveSkillLevel:
    // the points invested, plus what the owner and its pet add, capped at the
    // skill's level count.
    int level = static_cast<int>(skill.invested);
    // The bonuses are gated on there being a point in the skill at all, so a
    // skill nobody has learned stays at zero however much `+N to` is worn.
    if (level != 0) {
        if (owner) level += skill_level_bonus(owner, skill);
        // `CCharacter +0x640` is the pet, and the original runs the identical
        // pair of lookups against it.
        if (pet) level += skill_level_bonus(pet, skill);
    }
    const int cap = static_cast<int>(skill.level_count());
    if (cap <= level) level = cap;
    const bool changed = static_cast<uint32_t>(level) != skill.effective;
    skill.effective = static_cast<uint32_t>(level);
    return changed;
}

// PORT: the two effect managers `calculate_effective_level` consults set, and
// every skill's effective level worked out again.
void SkillBook::set_effect_managers(CharacterEffects owner, CharacterEffects pet) {
    owner_effects_ = owner;
    pet_effects_ = pet;
    for (KnownSkill& skill : skills_) {
        calculate_effective_level(skill, owner_effects_, pet_effects_);
    }
}

bool SkillBook::can_learn(const KnownSkill& skill, uint32_t player_level, int points) const {
    // the level the next point needs has been reached, a
    // `SKILL_REQUIRED` holds a level, there is a point to spend, and the
    // effective level is under the rank's top.
    bool allowed = level_required_for_investment(skill) <= player_level;
    if (!skill.entry.skill_required.empty()) {
        const KnownSkill* required = find_by_name(skill.entry.skill_required);
        if (required != nullptr && required->invested < 1) allowed = false;
    }
    if (points <= 0 || !allowed) return false;
    return skill.effective < skill.max_rank();
}

bool SkillBook::learn(KnownSkill& skill, uint32_t player_level, int& points) {
    // button 5 (A): the learn test, then `+0x3c8 -= 1` and
    // `setSkillLevel(skill, invested + 1)`.
    if (!can_learn(skill, player_level, points)) return false;
    if (points <= 0) return false;
    --points;
    ++skill.invested;
    calculate_effective_level(skill, owner_effects_, pet_effects_);
    return true;
}

bool SkillBook::can_map(const KnownSkill& skill) {
    // a learned (effective level above 0) skill, not a
    // passive, not `HIDDEN` (`+0x43`), with the constructor's always-true flag
    // (`+0x45`, Linux `+0x6d`) set.
    if (skill.def == nullptr) return false;
    if (skill.effective == 0) return false;
    if (skill.def->activation == SkillActivation::Passive) return false;
    return !skill.def->hidden;
}

bool SkillBook::map(const KnownSkill& skill, SkillButton button, int skill_set) {
    // inline: the GUID into the slot (`+0x840`), and the slot
    // beside it (`+0x890`) cleared when the GUID is a real one. Nothing else
    // that holds the same GUID is cleared.
    if (!can_map(skill)) return false;
    const int index = skill_button_slot(button, skill_set);
    if (index < 0 || index >= kSkillFunctionSlots) return false;
    const int64_t guid = skill.guid();
    slots_[static_cast<size_t>(index)] = guid;
    if (guid != -1) second_slots_[static_cast<size_t>(index)] = -1;
    return true;
}

// PORT: an accessor of the book's button slots.
int64_t SkillBook::slot(int index) const {
    if (index < 0 || index >= kSkillFunctionSlots) return -1;
    return slots_[static_cast<size_t>(index)];
}

// The slots as `CGameClient::loadCharacter`
// reads them: each skill clears its slot's item, then each item its slot's skill.
void SkillBook::set_slots(const std::vector<int64_t>& slots, const std::vector<int64_t>& second) {
    slots_.fill(-1);
    second_slots_.fill(-1);
    for (size_t i = 0; i < slots.size() && i < slots_.size(); ++i) {
        slots_[i] = slots[i];
        if (slots[i] != -1) second_slots_[i] = -1;
    }
    for (size_t i = 0; i < second.size() && i < second_slots_.size(); ++i) {
        second_slots_[i] = second[i];
        if (second[i] != -1) slots_[i] = -1;
    }
}

// `CPlayer::setMappedFunctionSkill(unsigned, long long)` (X360
// inline in `CGameClient::loadCharacter`): the GUID out of every other
// slot of the first list, into slot `index`, and that slot of the second list
// cleared when the GUID is a real one.
void SkillBook::set_mapped_function_skill(int index, int64_t guid) {
    if (index < 0 || index >= kMappedFunctionSlots) return;
    for (int64_t& held : mapped_)
        if (held == guid) held = -1;
    mapped_[static_cast<size_t>(index)] = guid;
    if (guid != -1) left_mapped_[static_cast<size_t>(index)] = -1;
}

// `CPlayer::setLeftMappedFunctionSkill(unsigned, long long)` (X360
// inline in `CGameClient::loadCharacter`): the same on the second
// list, clearing the first.
void SkillBook::set_left_mapped_function_skill(int index, int64_t guid) {
    if (index < 0 || index >= kMappedFunctionSlots) return;
    for (int64_t& held : left_mapped_)
        if (held == guid) held = -1;
    left_mapped_[static_cast<size_t>(index)] = guid;
    if (guid != -1) mapped_[static_cast<size_t>(index)] = -1;
}

// `CGameClient::loadCharacter`: the file's
// pairs, in order, through the two setters.
void SkillBook::set_mapped_pairs(const std::vector<std::pair<int64_t, int64_t>>& pairs) {
    mapped_.fill(-1);
    left_mapped_.fill(-1);
    for (size_t i = 0; i < pairs.size(); ++i) {
        set_mapped_function_skill(static_cast<int>(i), pairs[i].first);
        set_left_mapped_function_skill(static_cast<int>(i), pairs[i].second);
    }
}

// PORT: the mapped pairs, for the save file.
std::vector<std::pair<int64_t, int64_t>> SkillBook::mapped_pairs() const {
    std::vector<std::pair<int64_t, int64_t>> out;
    for (size_t i = 0; i < mapped_.size(); ++i) out.emplace_back(mapped_[i], left_mapped_[i]);
    return out;
}

void SkillBook::set_item_slot(int index, int64_t guid) {
    // `CPlayer::unitInit` (for slot 0): `+0x900 + 8i` takes the GUID,
    // and `+0x8b0 + 8i` is cleared only when it is not -1.
    if (index < 0 || index >= kSkillFunctionSlots) return;
    second_slots_[static_cast<size_t>(index)] = guid;
    if (guid != -1) slots_[static_cast<size_t>(index)] = -1;
}

const KnownSkill* SkillBook::bind_default_skill_slot() {
    // The skill manager's list (`CSkillManager +0x60`) holds the skills that
    // have a file: `CBaseUnit::addSkillByName` makes none for the others.
    std::vector<KnownSkill*> list;
    for (KnownSkill& skill : skills_) {
        if (skill.def != nullptr) list.push_back(&skill);
    }
    const auto usable = [](const KnownSkill& skill, uint32_t level) {
        return level != 0 && !skill.passive() && skill.enabled && !skill.def->hidden;
    };
    const KnownSkill* active = nullptr;
    if (!list.empty()) {
        // `unitInit` tests the first on its invested level (`+0xdc`).
        size_t current = 0;
        if (!usable(*list[0], list[0]->invested)) {
            // `cycleSkill(1)`: from the next skill on, wrapping, the first
            // whose effective level (`+0xe0`) is not 0; back at the current
            // one, that one is taken whatever it is.
            size_t i = 1 % list.size();
            while (i != current && !usable(*list[i], list[i]->effective)) i = (i + 1) % list.size();
            current = i;
        }
        active = list[current];
    }
    // The slot takes the GUID (Xbox `+0x870`); its item half (`+0x8c0`) is
    // cleared only when it is a real one.
    const int64_t guid = active != nullptr ? active->guid() : -1;
    slots_[kDefaultSkillSlot] = guid;
    if (guid != -1) second_slots_[kDefaultSkillSlot] = -1;
    return active;
}

// PORT: invested levels in class order, for the save file's `+0x1e8`.
std::vector<int32_t> SkillBook::levels() const {
    std::vector<int32_t> out;
    out.reserve(skills_.size());
    for (const KnownSkill& skill : skills_) {
        if (!skill.spell) out.push_back(static_cast<int32_t>(skill.invested));
    }
    return out;
}

// PORT: invested levels in class order, from the save file's `+0x1e8`, each
// skill's effective level worked out again.
void SkillBook::set_levels(const std::vector<int32_t>& levels) {
    size_t next = 0;
    for (KnownSkill& skill : skills_) {
        if (skill.spell) continue;
        if (next >= levels.size()) break;
        skill.invested = static_cast<uint32_t>(std::max(0, levels[next++]));
        calculate_effective_level(skill, owner_effects_, pet_effects_);
    }
}

bool SkillBook::can_learn_spell(const std::string& name, int32_t level, bool pet,
                                bool need_empty_slot) const {
    // `CCharacter::canLearnSpell`:
    // `CSkillManager::getSkill(name, level)`
    // matches the upper-cased name and `+0xdc == level`.
    for (const KnownSkill& skill : skills_) {
        if (upper(skill.entry.name) == upper(name) &&
            static_cast<int64_t>(skill.invested) == level) {
            return false;
        }
    }
    // a slot that already holds the name (compared exactly)
    // refuses - the first two for a pet (`+0x5a4`, a master, or ISA 0x57), all
    // four for anyone else.
    const size_t slots = pet ? 2 : 4;
    for (size_t i = 0; i < slots; ++i) {
        if (!spells[i].empty() && spells[i] == name) return false;
    }
    if (!need_empty_slot) return true;
    for (size_t i = 0; i < slots; ++i) {
        if (spells[i].empty()) return true;
    }
    return false;
}

const KnownSkill* SkillBook::known_spell(size_t slot) const {
    // `CCharacter::getKnownSpell`: an empty slot is none; otherwise
    // `CSkillManager::getSkill(name, level)`.
    if (slot >= spells.size() || spells[slot].empty()) return nullptr;
    for (const KnownSkill& skill : skills_) {
        if (upper(skill.entry.name) == upper(spells[slot]) &&
            static_cast<int64_t>(skill.invested) == spell_levels[slot]) {
            return &skill;
        }
    }
    return nullptr;
}

void SkillBook::unlearn_spell(size_t slot) {
    // `CCharacter::unLearnSpell(int)`: an
    // empty slot does nothing.
    if (slot >= spells.size() || spells[slot].empty()) return;
    // `unLearnSpell(name, level)`: the slot's skill deleted
    // from the manager (`CSkillManager::deleteSpecificSkill`), the
    // last entry moved into its place. The mapped-skill fields it clears are
    // the PC's mouse buttons and the cast, which resolve to nothing here.
    const KnownSkill* known = known_spell(slot);
    if (known != nullptr) {
        const size_t at = static_cast<size_t>(known - skills_.data());
        if (at + 1 != skills_.size()) skills_[at] = std::move(skills_.back());
        skills_.pop_back();
    }
    // The slots after it move up one; the fourth is emptied, level 0.
    for (size_t i = slot; i + 1 < spells.size(); ++i) {
        spells[i] = spells[i + 1];
        spell_levels[i] = spell_levels[i + 1];
    }
    spells.back().clear();
    spell_levels.back() = 0;
}

KnownSkill* SkillBook::learn_spell(const std::string& name, int32_t level, const SkillDef* def) {
    // `CCharacter::learnSpell`: the first
    // empty of `+0x3c8 + 8i`, its level at `+0x3e8 + 4i`; with all four taken,
    // nothing.
    size_t slot = spells.size();
    for (size_t i = 0; i < spells.size(); ++i) {
        if (spells[i].empty()) {
            slot = i;
            break;
        }
    }
    if (slot == spells.size()) return nullptr;
    spells[slot] = name;
    spell_levels[slot] = level;
    if (def == nullptr) return nullptr;
    // `CBaseUnit::addSkillByName` -> `CSkillManager::addSkill`,
    // then level required 1, unlimited `CHARGES` (-1; `CSkillManager::decrementSkillCharges`
    // counts them down) and `setSkillLevel(level)`.
    // DIVERGENCE: the original's manager keeps a second instance of a skill under the same
    // name; the book holds one per name, so a spell learned again re-levels it.
    KnownSkill* skill = find_by_name(name);
    if (skill == nullptr) {
        KnownSkill added;
        added.entry.name = def->name;
        added.def = def;
        added.spell = true;
        skills_.push_back(added);
        skill = &skills_.back();
    }
    skill->entry.level_required = 1;
    // `CSkillManager::setSkillLevel` ->
    // `CSkill::_setLevelOfSkillFromSkillManager`: -1 is 1; otherwise held
    // (unsigned) to the `[LEVELn]` count, or to 1 when there are none (a monster's spell
    // learned "at 3" is level 1). Then `calculateEffectiveSkillLevel`.
    const uint32_t levels = static_cast<uint32_t>(def->levels.size());
    const uint32_t cap = levels != 0 ? levels : 1u;
    const uint32_t asked = static_cast<uint32_t>(level);
    skill->invested = level == -1 ? 1u : (asked > cap ? cap : asked);
    calculate_effective_level(*skill, owner_effects_, pet_effects_);
    return skill;
}

int SkillBook::reset_skills() {
    // `CPlayer::resetSkills`.
    const auto counted = [](const KnownSkill& skill) {
        return skill.def != nullptr && skill.enabled && !skill.def->hidden &&
               skill.def->type == SkillType::Skill;
    };
    int points = 0;
    for (const KnownSkill& skill : skills_) {
        if (counted(skill)) points += static_cast<int>(skill.invested);
    }
    for (KnownSkill& skill : skills_) {
        if (!counted(skill)) continue;
        skill.invested = 0;
        calculate_effective_level(skill, owner_effects_, pet_effects_);
    }
    // The unit's `[SKILL]` groups: the class's entries, in file order (a
    // learned spell is none of them). `getSkill(name)` finds the first skill of
    // the name the manager holds; a name with no file is in no list.
    for (const KnownSkill& entry : skills_) {
        if (entry.spell) continue;
        points -= static_cast<int>(entry.entry.level);
        const std::string want = upper(entry.entry.name);
        for (KnownSkill& skill : skills_) {
            if (skill.def == nullptr || upper(skill.entry.name) != want) continue;
            skill.invested = entry.entry.level;
            calculate_effective_level(skill, owner_effects_, pet_effects_);
            break;
        }
    }
    return points < 0 ? 0 : points;
}

// The four spell slots emptied and their skills dropped, before a saved character's are
// learned again (`CCharacter::applySaveState`).
void SkillBook::clear_spells() {
    spells = {};
    spell_levels = {};
    skills_.erase(std::remove_if(skills_.begin(), skills_.end(),
                                 [](const KnownSkill& skill) { return skill.spell; }),
                  skills_.end());
}

int SkillCursor::index() const {
    //
    if (!in_spells) return std::min((pane * 6 + row) * 2 + column, 35);
    return std::min(spell + 36, 39);
}

namespace {

/// A step lands on a hidden cell and takes another; a grid with no shown cell
/// at all would never stop. DIVERGENCE: the recursion is bounded, which only
/// matters for a class with no skills.
constexpr int kMaxSkips = kSkillPanes * kSkillRows * kSkillColumns + kSkillSpellSlots;

// The skills tab's cursor: a cell no skill sits in is passed over.
bool skips(const SkillCursor& cursor, const SkillCellShown& shown) {
    return !cursor.in_spells && shown && !shown(cursor);
}

/// Leaving the spell row puts the cursor over the column the spell slot sits
/// under, as map it.
void spell_to_cell(SkillCursor& cursor) {
    switch (cursor.spell) {
        case 1:
            cursor.column = 0;
            cursor.pane = 1;
            break;
        case 2:
            cursor.pane = 1;
            cursor.column = 1;
            break;
        case 0:
            cursor.pane = 0;
            cursor.column = 1;
            break;
        default:
            cursor.column = 0;
            cursor.pane = 2;
            break;
    }
}

// The skills tab's cursor: entering the spell row, the slot under the
// cursor's column.
void cell_to_spell(SkillCursor& cursor) {
    if (cursor.pane == 0) cursor.spell = 0;
    if (cursor.pane == 1) cursor.spell = cursor.column != 0 ? 2 : 1;
    if (cursor.pane == 2) cursor.spell = 3;
}

void up(SkillCursor& cursor, const SkillCellShown& shown, int depth) {
    //
    --cursor.row;
    if (!cursor.in_spells) {
        if (cursor.row < 0) {
            cursor.in_spells = true;
            cell_to_spell(cursor);
        }
    } else {
        cursor.in_spells = false;
        cursor.row = 5;
        spell_to_cell(cursor);
    }
    if (depth < kMaxSkips && skips(cursor, shown)) up(cursor, shown, depth + 1);
}

void down(SkillCursor& cursor, const SkillCellShown& shown, int depth) {
    //
    ++cursor.row;
    if (!cursor.in_spells) {
        if (5 < cursor.row) {
            cursor.in_spells = true;
            cell_to_spell(cursor);
        }
    } else {
        cursor.in_spells = false;
        cursor.row = 0;
        spell_to_cell(cursor);
    }
    if (depth < kMaxSkips && skips(cursor, shown)) down(cursor, shown, depth + 1);
}

void right(SkillCursor& cursor, const SkillCellShown& shown, int depth) {
    //
    if (!cursor.in_spells) {
        if (1 < ++cursor.column) {
            if (2 < ++cursor.pane) cursor.pane = 0;
            cursor.column = 0;
        }
    } else if (3 < ++cursor.spell) {
        cursor.spell = 0;
    }
    if (depth < kMaxSkips && skips(cursor, shown)) right(cursor, shown, depth + 1);
}

void left(SkillCursor& cursor, const SkillCellShown& shown, int depth) {
    //
    if (!cursor.in_spells) {
        if (--cursor.column < 0) {
            if (--cursor.pane < 0) cursor.pane = 2;
            cursor.column = 1;
        }
    } else if (--cursor.spell < 0) {
        cursor.spell = 3;
    }
    if (depth < kMaxSkips && skips(cursor, shown)) left(cursor, shown, depth + 1);
}

}  // namespace

// The skills tab's cursor moves.
void skill_cursor_up(SkillCursor& cursor, const SkillCellShown& shown) { up(cursor, shown, 0); }
// The skills tab's cursor moves.
void skill_cursor_down(SkillCursor& cursor, const SkillCellShown& shown) { down(cursor, shown, 0); }
// The skills tab's cursor moves.
void skill_cursor_right(SkillCursor& cursor, const SkillCellShown& shown) {
    right(cursor, shown, 0);
}
// The skills tab's cursor moves.
void skill_cursor_left(SkillCursor& cursor, const SkillCellShown& shown) { left(cursor, shown, 0); }

}  // namespace tl::core
