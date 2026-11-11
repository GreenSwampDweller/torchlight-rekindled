#include "core/combat.h"

#include "core/item_effects.h"
#include "core/ogre_math.h"
#include "core/runic_math.h"

#include "formats/float_convert.h"

#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace tl::core {

namespace {

/// The console's float to int, `fctiwz` (`ppc_fctiwz`): truncation, saturating
/// out of range (NaN to the minimum, 0x80000000).
// PORT: a name for it.
int32_t truncate(float value) { return tl::formats::ppc_fctiwz(value); }

/// Integer subtraction as the machine does it: wrapping.
// PORT: integer subtraction as the machine does it, wrapping.
int32_t wrapping_sub(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}

// The difficulty's curve `CCharacter::setLevel(unsigned, bool)`
// reads: `HEALTH_MONSTER_BYLEVEL` and its
// difficulty suffix.
const char* health_graph(int difficulty) {
    switch (difficulty) {
        case difficulty::kEasy: return "HEALTH_MONSTER_BYLEVEL_EASY";
        case difficulty::kNormal: return "HEALTH_MONSTER_BYLEVEL";
        case difficulty::kHard: return "HEALTH_MONSTER_BYLEVEL_HARD";
        case difficulty::kVeryHard: return "HEALTH_MONSTER_BYLEVEL_VERYHARD";
        default: return nullptr;
    }
}

// The difficulty's curve `CCharacter::setLevel(unsigned, bool)`
// reads: `ARMOR_MONSTER_BYLEVEL` and its
// difficulty suffix.
const char* armor_graph(int difficulty) {
    switch (difficulty) {
        case difficulty::kEasy: return "ARMOR_MONSTER_BYLEVEL_EASY";
        case difficulty::kNormal: return "ARMOR_MONSTER_BYLEVEL";
        case difficulty::kHard: return "ARMOR_MONSTER_BYLEVEL_HARD";
        case difficulty::kVeryHard: return "ARMOR_MONSTER_BYLEVEL_VERYHARD";
        default: return nullptr;
    }
}

// The difficulty's curve `CCharacter::setLevel(unsigned, bool)`
// reads: `DAMAGE_MONSTER` and its difficulty
// suffix.
const char* damage_graph(int difficulty) {
    switch (difficulty) {
        case difficulty::kEasy: return "DAMAGE_MONSTER_EASY";
        case difficulty::kNormal: return "DAMAGE_MONSTER";
        case difficulty::kHard: return "DAMAGE_MONSTER_HARD";
        case difficulty::kVeryHard: return "DAMAGE_MONSTER_VERYHARD";
        default: return nullptr;
    }
}

// The difficulty's curve `CCharacter::setLevel(unsigned, bool)`
// reads: `EXPERIENCE_MONSTER` and its
// difficulty suffix.
const char* experience_graph(int difficulty) {
    switch (difficulty) {
        case difficulty::kEasy: return "EXPERIENCE_MONSTER_EASY";
        case difficulty::kNormal: return "EXPERIENCE_MONSTER";
        case difficulty::kHard: return "EXPERIENCE_MONSTER_HARD";
        case difficulty::kVeryHard: return "EXPERIENCE_MONSTER_VERYHARD";
        default: return nullptr;
    }
}

// PORT: ASCII case folding, the case-blind compare of names in the port's narrow strings.
std::string upper(std::string text) {
    for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

/// `CGraph::getValue(level, 0)`; a graph that does not exist reads 0.
float curve(const StatBook& graphs, const std::string& name, int level) {
    if (name.empty()) return 0.0f;
    return graphs.value(upper(name), static_cast<float>(level), 0.0f);
}

}  // namespace

namespace {

// PORT: a record's value word as an integer, as `lwz r, 8(r3)` reads it.
int32_t word_int(const tl::formats::AdmSection& unit, const std::string& key, int32_t fallback) {
    const tl::formats::AdmRecord* record = unit.find(key);
    if (record == nullptr) return fallback;
    const float f = record->word_as_float();
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof bits);
    return static_cast<int32_t>(bits);
}

// PORT: a record's value word as a float, as `lfs f, 8(r3)` reads it.
float word_float(const tl::formats::AdmSection& unit, const char* key, float fallback) {
    const tl::formats::AdmRecord* record = unit.find(key);
    return record != nullptr ? record->word_as_float() : fallback;
}

// `CDataGroup::GetDataValue(name, L"")` then: a string's text,
// and the empty string for a record of any other type.
std::string word_text(const tl::formats::AdmSection& unit, const char* key) {
    const tl::formats::AdmRecord* record = unit.find(key);
    if (record == nullptr) return std::string();
    const bool text = record->type == tl::formats::AdmType::String ||
                      record->type == tl::formats::AdmType::Translate;
    return text ? record->text : std::string();
}

}  // namespace

void read_set_level_data(const tl::formats::AdmSection& unit, UnitStats& stats) {
    // `CCharacter::unitInit` reads `XP` (0 when not there)
    // into `+0x3bc`, which `setLevel` squares the curve for.
    stats.xp = word_int(unit, "XP", 0);
    // `CCharacter::setLevel(unsigned, bool)`,
    // in the order it reads them.
    stats.min_health = word_float(unit, "MINHP", 1.0f);
    stats.max_health = word_float(unit, "MAXHP", 1.0f);
    stats.health_graph = word_text(unit, "HEALTH_GRAPH");
    // `CCharacter::makeChampion` reads both again with 0 for
    // unauthored, and `unitInit` its two flags.
    //
    stats.champion_min_health = word_float(unit, "MINHP", 0.0f);
    stats.champion_max_health = word_float(unit, "MAXHP", 0.0f);
    {
        const tl::formats::AdmRecord* record = unit.find("CHAMPION");
        stats.champion = record != nullptr && record->word_first_byte() != 0;
    }
    {
        const tl::formats::AdmRecord* record = unit.find("UNIQUENAME");
        stats.unique_name = record != nullptr && record->word_first_byte() != 0;
    }
    stats.set_hp_pct = word_float(unit, "SET_HP_PCT", 1.0f);
    stats.armor = word_int(unit, "ARMOR", 0);
    stats.armor_graph = word_text(unit, "ARMOR_GRAPH");
    // `ARMOR_` + each of `gDAMAGE_TYPES`, physical first.
    for (int type = 0; type < kDamageTypeCount; ++type) {
        const int32_t pct = word_int(unit, std::string("ARMOR_") + kDamageTypeKeys[type], -1);
        if (type == 0) {
            stats.armor_physical = pct;
        } else {
            stats.armor_by_type[static_cast<size_t>(type)] = pct;
        }
    }
    stats.min_damage = static_cast<float>(word_int(unit, "MINDAMAGE", 0));
    stats.max_damage = static_cast<float>(word_int(unit, "MAXDAMAGE", 0));
    stats.damage_graph = word_text(unit, "DAMAGE_GRAPH");
    {
        // the flag's byte.
        const tl::formats::AdmRecord* record = unit.find("NO_UNARMED_ATTACKS");
        stats.no_unarmed_attacks = record != nullptr && record->word_first_byte() != 0;
    }
    // `DAMAGE_` + each of `gDAMAGE_TYPES`.
    for (int type = 0; type < kDamageTypeCount; ++type) {
        stats.damage_by_type[static_cast<size_t>(type)] =
            word_int(unit, std::string("DAMAGE_") + kDamageTypeKeys[type], -1);
    }
}

namespace {

// The champion curves `CCharacter::makeChampion` reads, by
// the difficulty (the client's `+0x141c`): none for any other value.
const char* champion_graph(const char* stem, int difficulty) {
    static std::string names[4];
    switch (difficulty) {
        case difficulty::kEasy: return (names[0] = std::string(stem) + "_EASY").c_str();
        case difficulty::kNormal: return (names[1] = stem).c_str();
        case difficulty::kHard: return (names[2] = std::string(stem) + "_HARD").c_str();
        case difficulty::kVeryHard: return (names[3] = std::string(stem) + "_VERYHARD").c_str();
        default: return nullptr;
    }
}

}  // namespace

// `CCharacter::makeChampion(std::wstring name, std::wstring theme)`,
// the figures.
bool make_champion(UnitCombat& combat, const UnitStats& stats, int difficulty,
                   const StatBook& graphs, RunicRandom& rolls) {
    if (combat.champion) return false;
    combat.champion = true;
    const int level = combat.level;
    // The hit points' range: the larger `MAX < MIN ?
    // MIN : MAX`, the smaller `MIN < MAX ? MIN : MAX`; scaled by the curve
    // unless the larger is 0 (`fcmpu; beq`).
    const float min_pct = stats.champion_min_health;
    const float max_pct = stats.champion_max_health;
    float high = max_pct < min_pct ? min_pct : max_pct;
    float low = min_pct < max_pct ? min_pct : max_pct;
    if (high != 0.0f) {
        const char* fallback = champion_graph("HEALTH_CHAMPIONMONSTER_BYLEVEL", difficulty);
        const std::string name = !stats.health_graph.empty()
                                     ? stats.health_graph
                                     : std::string(fallback != nullptr ? fallback : "");
        const float g = curve(graphs, name, level);
        low = (g * low) * 0.01f;
        high = (g * high) * 0.01f;
    }
    // `XP` (+0x3bc), off the champion curve squared when there is one.
    combat.xp = stats.xp;
    if (combat.xp != 0) {
        const char* name = champion_graph("EXPERIENCE_CHAMPIONMONSTER", difficulty);
        const float x = curve(graphs, name != nullptr ? name : "", level);
        combat.xp = truncate((x * x) * 0.01f);
    }
    // The fame award (+0x3c0): 100, then the curve squared.
    combat.fame = 100;
    {
        const char* name = champion_graph("FAME_CHAMPIONMONSTER", difficulty);
        const float f = curve(graphs, name != nullptr ? name : "", level);
        combat.fame = truncate((f * f) * 0.01f);
    }
    // One volatile draw; the maximum (+0x380) and the hit points (+0x37c).
    combat.base_max_hp = truncate(rolls.between(low, high));
    combat.hp = static_cast<float>(combat.base_max_hp);
    // `ARMOR` (+0x38c) off `ARMOR_GRAPH` or the champion curve, rounded up.
    combat.armor = stats.armor;
    if (combat.armor != 0) {
        const char* fallback = champion_graph("ARMOR_CHAMPIONMONSTER_BYLEVEL", difficulty);
        const std::string name = !stats.armor_graph.empty()
                                     ? stats.armor_graph
                                     : std::string(fallback != nullptr ? fallback : "");
        const float a = curve(graphs, name, level);
        combat.armor = truncate(std::ceil((static_cast<float>(combat.armor) * a) * 0.01f));
    }
    // `ARMOR_<TYPE>` into +0x588 off the armour as it stands; a physical share
    // other than 0 then replaces the armour and is cleared. A type not authored
    // keeps what it had.
    for (int type = 0; type < kDamageTypeCount; ++type) {
        const int pct =
            type == 0 ? stats.armor_physical : stats.armor_by_type[static_cast<size_t>(type)];
        if (static_cast<float>(pct) == -1.0f) continue;
        combat.damage_defense[static_cast<size_t>(type)] = truncate(
            std::ceil((static_cast<float>(combat.armor) * static_cast<float>(pct)) * 0.01f));
    }
    if (combat.damage_defense[0] != 0) {
        combat.armor = combat.damage_defense[0];
        combat.damage_defense[0] = 0;
    }
    return true;
}

// `CCharacter::setLevel(unsigned, bool)`.
UnitCombat set_level(const UnitStats& stats, int level, int difficulty, const StatBook& graphs,
                     RunicRandom& rolls) {
    UnitCombat combat;
    combat.level = level;

    // Hit points: `MINHP`/`MAXHP` are percentages of the health curve.:
    // the larger is `MAX < MIN ? MIN : MAX` and the
    // smaller `MIN < MAX ? MIN : MAX` (`fcmpu; blt`), so a NaN on either side
    // is `MAX` both times.
    const float high_hp = stats.max_health < stats.min_health ? stats.min_health : stats.max_health;
    const float low_hp = stats.min_health < stats.max_health ? stats.min_health : stats.max_health;
    float high = high_hp;
    float low = low_hp;
    if (!(0.1f < high)) {
        low = 1.0f;
        high = 1.0f;
    }
    const std::string health =
        !stats.health_graph.empty()
            ? stats.health_graph
            : std::string(health_graph(difficulty) ? health_graph(difficulty) : "");
    // the curve times the percent, then times 0.01f.
    const float g = curve(graphs, health, level);
    low = (g * low) * 0.01f;
    high = (g * high) * 0.01f;

    if (stats.xp != 0) {
        const char* name = experience_graph(difficulty);
        const float x = curve(graphs, name != nullptr ? name : "", level);
        // the curve's value squared,
        // times 0.01f.
        combat.xp = truncate((x * x) * 0.01f);
    }

    combat.base_max_hp = truncate(rolls.between(low, high));
    const float base = static_cast<float>(combat.base_max_hp);
    const float scaled = stats.set_hp_pct * base;
    float hp = scaled < base ? scaled : base;
    if (hp <= 1.0f) hp = 1.0f;
    combat.hp = static_cast<float>(truncate(hp));

    // Armour: `ARMOR` is a percentage of the armour curve.
    combat.armor = stats.armor;
    if (combat.armor != 0) {
        const std::string name =
            !stats.armor_graph.empty()
                ? stats.armor_graph
                : std::string(armor_graph(difficulty) ? armor_graph(difficulty) : "");
        const float a = curve(graphs, name, level);
        // (armor * curve) * 0.01f, rounded up.
        combat.armor = truncate(std::ceil((static_cast<float>(combat.armor) * a) * 0.01f));
    }
    // `ARMOR_<TYPE>` over `gDAMAGE_TYPES` into +0x620, all off the armour as
    // it stands; then a physical share other than 0 replaces the armour and
    // its slot is cleared.
    for (int type = 0; type < kDamageTypeCount; ++type) {
        const int pct =
            type == 0 ? stats.armor_physical : stats.armor_by_type[static_cast<size_t>(type)];
        if (static_cast<float>(pct) == -1.0f) continue;
        // (armor * share) * 0.01f, rounded up.
        combat.damage_defense[static_cast<size_t>(type)] = truncate(
            std::ceil((static_cast<float>(combat.armor) * static_cast<float>(pct)) * 0.01f));
    }
    if (combat.damage_defense[0] != 0) {
        combat.armor = combat.damage_defense[0];
        combat.damage_defense[0] = 0;
    }

    combat.attack =
        own_attack(stats, level, difficulty, /*player=*/false, graphs, &combat.level_health);
    return combat;
}

AttackDescription own_attack(const UnitStats& stats, int level, int difficulty, bool player,
                             const StatBook& graphs, int* level_health) {
    // Damage: `MINDAMAGE`/`MAXDAMAGE` are percentages of the damage curve.
    AttackDescription attack;
    int min_damage = 0;
    int max_damage = 0;
    const int min_pct = static_cast<int>(stats.min_damage);
    const int max_pct = static_cast<int>(stats.max_damage);
    if (max_pct != 0) {
        std::string name =
            !stats.damage_graph.empty()
                ? stats.damage_graph
                : std::string(damage_graph(difficulty) ? damage_graph(difficulty) : "");
        if (player) name = "DAMAGE_MONSTER";
        const float d = curve(graphs, name, level);
        // (percent * curve) * 0.01f, rounded up.
        min_damage = truncate(std::ceil((static_cast<float>(min_pct) * d) * 0.01f));
        max_damage = truncate(std::ceil((static_cast<float>(max_pct) * d) * 0.01f));
        // the same two off the difficulty's health
        // curve (`HEALTH_MONSTER_BYLEVEL` and its suffixes, whatever
        // `HEALTH_GRAPH` says), each rounded up, then halved and truncated; the
        // curve's value truncated (`fctidz`) into +0x618.
        const char* health = health_graph(difficulty);
        const float h = curve(graphs, health != nullptr ? health : "", level);
        if (level_health != nullptr) *level_health = static_cast<int>(static_cast<int64_t>(h));
        const int min_h = truncate(std::ceil((static_cast<float>(min_pct) * h) * 0.01f));
        const int max_h = truncate(std::ceil((static_cast<float>(max_pct) * h) * 0.01f));
        attack.health_min_half = truncate(static_cast<float>(min_h) * 0.5f);
        attack.health_max_half = truncate(static_cast<float>(max_h) * 0.5f);
    }

    // No attack of its own yet: `CAttackDescription("ATTACK", false,
    // attack_range, strike_range, max, 0, min, 1)`.
    // `unitInit` calls `setLevel` (vtable
    // +0x158) before it reads `ATTACK_RANGE` and `STRIKE_RANGE`
    // into those two, so a unit's own attack takes
    // what `CCharacter::CCharacter` left there:
    // 0.375 and 0.5, whatever its data says.
    attack.animation = "ATTACK";
    attack.range = 0.375f;
    attack.strike_range = 0.5f;
    attack.max_damage = max_damage;
    attack.min_damage = min_damage;
    attack.speed = 1.0f;
    // `DAMAGE_<TYPE>` over `gDAMAGE_TYPES`: an authored share of the
    // maximum goes into `+0x24[type]` - the physical one over the maximum itself.
    for (int type = 0; type < kDamageTypeCount; ++type) {
        const int pct = stats.damage_by_type[static_cast<size_t>(type)];
        if (static_cast<float>(pct) == -1.0f) continue;
        // (maximum * share) * 0.01f, rounded up.
        const int share =
            truncate(std::ceil((static_cast<float>(max_damage) * static_cast<float>(pct)) * 0.01f));
        if (type == 0) {
            attack.max_damage = share;
        } else {
            attack.damage_by_type[static_cast<size_t>(type)] = share;
        }
    }
    return attack;
}

// `CCharacter::maxHP`.
int max_hp(const UnitCombat& combat) {
    int hp = combat.base_max_hp;
    if (hp < 1) hp = 1;
    return hp;
}

// `CCharacter::attackRange`.
float attack_range(const UnitStats& stats, const AttackDescription& attack,
                   const HeldHands* hands) {
    return attack_range(stats, stats.scale, attack, hands);
}

float attack_range(const UnitStats& stats, float scale, const AttackDescription& attack,
                   const HeldHands* hands) {
    float range = attack.range;
    if (hands != nullptr && hands->using_weapon && hands->left.held && hands->right.held &&
        hands->left.weapon) {
        // The left hand's first, then the right's.
        if (hands->left.ranged || hands->right.ranged) {
            if (!(hands->left.range <= range)) range = hands->left.range;
            if (!(hands->right.range <= range)) range = hands->right.range;
        } else {
            if (!(hands->left.range >= range)) range = hands->left.range;
            if (!(hands->right.range >= range)) range = hands->right.range;
        }
    }
    // The reach bonus (+0x434) times the unit's own scale (+0x84).
    return std::fma(stats.reach, scale, range) + 0.2f;
}

// `CCharacter::meleeRange`.
float melee_range(const UnitStats& stats, const AttackDescription& attack, const HeldHands& hands) {
    return melee_range(stats, stats.scale, attack, hands);
}

float melee_range(const UnitStats& stats, float scale, const AttackDescription& attack,
                  const HeldHands& hands) {
    float range = attack.range;
    if (hands.using_weapon) {
        if (hands.left.held && hands.left.melee && !(hands.left.range >= range)) {
            range = hands.left.range;
        }
        if (hands.right.held && hands.right.melee && !(hands.right.range >= range)) {
            range = hands.right.range;
        }
    }
    return std::fma(stats.reach, scale, range) + 0.2f;
}

// `CCharacter::rangedRange`.
float ranged_range(const UnitStats& stats, const AttackDescription& attack,
                   const HeldHands& hands) {
    return ranged_range(stats, stats.scale, attack, hands);
}

float ranged_range(const UnitStats& stats, float scale, const AttackDescription& attack,
                   const HeldHands& hands) {
    float range = attack.range;
    if (hands.using_weapon) {
        if (hands.left.held && hands.left.ranged && !(hands.left.range <= range)) {
            range = hands.left.range;
        }
        if (hands.right.held && hands.right.ranged && !(hands.right.range <= range)) {
            range = hands.right.range;
        }
    }
    return std::fma(stats.reach, scale, range) + 0.2f;
}

// `CCharacter::selectAttack(EATTACK_RANGE_TYPE)`,
// which attack it takes.
AttackHand select_attack_hand(const HeldHands& hands, int mode, bool alternate) {
    if (!hands.right.held) {
        // The left hand alone, when it holds a weapon (its `+0x23c`).
        if (hands.left.held && hands.left.weapon) return AttackHand::Left;
        return AttackHand::Own;
    }
    if (hands.left.held && hands.left.weapon) {
        bool left = false;
        if (mode == 0) {
            left = alternate;
        } else if (mode == 2) {
            left = hands.left.ranged;
        } else if (mode == 1) {
            left = hands.left.melee;
        }
        if (left) return AttackHand::Left;
    }
    // the right hand's attack (`+0x238`).
    return AttackHand::Right;
}

// `CCharacter::attack`.
AttackHandStep attack_hand_step(const HeldHands& hands) {
    if (!hands.left.held || !hands.right.held || !hands.left.weapon) {
        return AttackHandStep::SelectAny;
    }
    // the two ranges are asked only when exactly one
    // hand is a ranged weapon; otherwise both stand at `attackRange` and are
    // equal, and no attack is selected.
    if (hands.left.ranged == hands.right.ranged) return AttackHandStep::Keep;
    return AttackHandStep::Mixed;
}

bool attack_melee_hand_is_right(const HeldHands& hands) { return !hands.right.ranged; }

// `CCharacter::inAttackRange(EATTACK_RANGE_TYPE)`.
//
bool in_attack_range(const UnitStats& stats, const AttackDescription& attack, const Vec3& from,
                     const Vec3& to, float target_radius, bool ranged, float ranged_bonus,
                     const HeldHands* hands) {
    return in_attack_range(stats, stats.scale, attack, from, to, target_radius, ranged,
                           ranged_bonus, hands);
}

bool in_attack_range(const UnitStats& stats, float scale, const AttackDescription& attack,
                     const Vec3& from, const Vec3& to, float target_radius, bool ranged,
                     float ranged_bonus, const HeldHands* hands) {
    if (2.5f < std::fabs(to.y - from.y) && !ranged) return false;
    const float dx = to.x - from.x;
    const float dz = to.z - from.z;
    const float own = 0.0f <= stats.radius ? stats.radius : 0.0f;
    const float theirs = 0.0f <= target_radius ? target_radius : 0.0f;
    // the distance by `fmuls` and `fmadds`; the reach
    // multiplied, then the bonus added apart.
    return (std::sqrt(std::fma(dz, dz, dx * dx)) - theirs) - own <=
           stats.range_multiplier * attack_range(stats, scale, attack, hands) +
               (ranged ? ranged_bonus : 0.0f);
}

// `CCharacter::inStrikeRange(CCharacter*, CItem*, CEquipment*)`.
//
bool in_strike_range(const UnitStats& stats, const AttackDescription& attack, const Vec3& from,
                     const Vec3& to, float target_radius, bool ranged, float ranged_bonus,
                     bool console_player) {
    return in_strike_range(stats, stats.scale, attack, from, to, target_radius, ranged,
                           ranged_bonus, console_player);
}

bool in_strike_range(const UnitStats& stats, float scale, const AttackDescription& attack,
                     const Vec3& from, const Vec3& to, float target_radius, bool ranged,
                     float ranged_bonus, bool console_player) {
    if (2.5f < std::fabs(to.y - from.y) && !ranged) return false;
    const float dx = to.x - from.x;
    const float dz = to.z - from.z;
    const float own = 0.0f <= stats.radius ? stats.radius : 0.0f;
    const float theirs = 0.0f <= target_radius ? target_radius : 0.0f;
    // `fma(reach, scale, strike range)`, plus 0.2,
    // then `fma` of that by `RANGE_MULTIPLIER` onto the ranged bonus.
    float reach = std::fma(std::fma(stats.reach, scale, attack.strike_range) + 0.2f,
                           stats.range_multiplier, ranged ? ranged_bonus : 0.0f);
    // `ISA` the player.
    if (console_player) reach = reach + 0.5f;
    return (std::sqrt(std::fma(dz, dz, dx * dx)) - theirs) - own <= reach;
}

bool melee_attack_walled(const CollisionWorld& world, const Vec3& here, float facing,
                         const Vec3& at) {
    // `CCharacter::performAttack`: `getForward` (+0xb4) and
    // `getRight` (+0xa8), the third and first columns of the yaw.
    const Vec3 fwd{std::sin(facing), 0.0f, std::cos(facing)};
    const Vec3 right{fwd.z, 0.0f, -fwd.x};
    Vec3 from{here.x, here.y + 5.0f, here.z};
    Vec3 to{at.x, here.y + 5.0f, at.z};
    CollisionHit hit;
    if (!world.ray_collision(from, to, true, hit)) return false;
    //
    from = {from.x - fwd.x * 1.25f, from.y - fwd.y * 1.25f, from.z - fwd.z * 1.25f};
    from = {from.x + right.x * 0.75f, from.y + right.y * 0.75f, from.z + right.z * 0.75f};
    to = {to.x + right.x * 0.75f, to.y + right.y * 0.75f, to.z + right.z * 0.75f};
    if (!world.ray_collision(from, to, true, hit)) return false;
    //
    from = {from.x - right.x * 1.25f, from.y - right.y * 1.25f, from.z - right.z * 1.25f};
    to = {to.x - right.x * 1.25f, to.y - right.y * 1.25f, to.z - right.z * 1.25f};
    return world.ray_collision(from, to, true, hit);
}

RangedShot ranged_shot(const CollisionWorld* world, const Vec3& start, const Vec3& here,
                       float own_radius, const Vec3* target, float target_radius,
                       const Vec3& forward, float reach) {
    // `CCharacter::performAttack`, from
    //
    RangedShot shot;
    Vec3 end;
    float height;
    if (target != nullptr) {
        // (a character) and (an item): the same sums.
        end = *target;
        height = ((target->y - target_radius) - (here.y - own_radius)) + start.y;
    } else {
        //
        end = {forward.x * reach + start.x, 0.0f, forward.z * reach + start.z};
        height = forward.y * reach + start.y;
    }
    // the rise held within a fifth of |(end.x, start.y, end.z)|.
    float rise = height - start.y;
    const float limit = std::sqrt(end.x * end.x + start.y * start.y + end.z * end.z) / 5.0f;
    rise = rise < limit ? rise : limit;
    rise = rise > -limit ? rise : -limit;
    end.y = rise + start.y;
    shot.end = end;
    if (world == nullptr || world->empty()) return shot;
    // the ray 0.8 above both ends (`rayCollision(..., false)`).
    CollisionHit hit;
    if (world->ray_collision({start.x, start.y + 0.8f, start.z}, {end.x, end.y + 0.8f, end.z}, true,
                             hit)) {
        //
        shot.walled = true;
        shot.end = {hit.point.x, start.y, hit.point.z};
    }
    return shot;
}

// `CCharacter::facingTarget(CCharacter*, CItem*)`,
// from the two centres (`getPosition(true)`); only x and z are read.
bool facing_target(float facing, const Vec3& from, const Vec3& to, float attack_angle,
                   float weapon_arc) {
    const float dx = to.x - from.x;
    const float dz = to.z - from.z;
    // `fmuls dz*dz`, `fmadds dx*dx + that`; nearer
    // than 1.5 it is faced.
    if (std::sqrt(std::fma(dx, dx, dz * dz)) < 1.5f) return true;
    // `Vector3::normalise` of the ground offset, then `atan2f` of
    // the facing vector (+0xa0, +0xa8) and of the offset, in degrees; the
    // second fused with the first's negation (`fmsubs`).
    const Vec3 toward = ogre_normalised(Vec3{dx, 0.0f, dz});
    const float mine = static_cast<float>(crt_atan2(static_cast<double>(std::sin(facing)),
                                                    static_cast<double>(std::cos(facing)))) *
                       57.29582595825195f;
    const float wanted = static_cast<float>(crt_atan2(toward.x, toward.z));
    float off = std::fma(wanted, 57.29582595825195f, -mine);
    if (180.0f < off) off = off - 360.0f;
    if (off < -180.0f) off = off + 360.0f;
    // with a weapon in use, twice its arc when that is more than
    // `ATTACKANGLE`.
    float angle = attack_angle;
    if (angle < weapon_arc * 2.0f) angle = weapon_arc * 2.0f;
    return !(std::fabs(off) > angle * 0.5f);
}

// `CEquipment::calculateCombatStats`.
UnitStats::WeaponAttack weapon_attack_of(const ItemStats& item, Hand hand,
                                         const UnitTypeHierarchy& types) {
    UnitStats::WeaponAttack out;
    if (weapon_damage_kind(item.unittype) == WeaponDamageKind::None) return out;
    std::string cls = attack_animation_class(item.unittype, hand);
    for (char& ch : cls) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    out.present = true;
    out.attack.animation = cls;
    out.attack.range = item.range;
    out.attack.strike_range = item.strike_range;
    out.attack.speed = item.speed / 100.0f;
    out.cooldown = item.attack_cooldown;
    const uint32_t type = types.id_of(item.unittype);
    out.missile = item.missile;
    out.ranged = types.isa(type, unit_type::kRangedWeapon);
    out.melee = types.isa(type, unit_type::kMelee);
    out.arc =
        console_weapon_arc(types.isa(type, unit_type::kSword), types.isa(type, unit_type::kAxe),
                           types.isa(type, unit_type::kMace), types.isa(type, unit_type::kPolearm),
                           types.isa(type, unit_type::kStaff));
    return out;
}

//
float console_weapon_arc(bool sword, bool axe, bool mace, bool polearm, bool staff) {
    if (sword || axe) return 50.0f;
    if (mace) return 40.0f;
    if (polearm || staff) return 70.0f;
    return 0.0f;
}

// `CCharacter::turnToward(Vector3 const&, float)`.
//
float turn_toward(float facing, float dir_x, float dir_z, float turn_rate, float delta,
                  bool* turned, float factor) {
    // `CCharacter::turnToward`: no turn rate (+0x218), no turn.
    if (turn_rate == 0.0f) return facing;
    // the console's `atan2f` of the heading and of the
    // direction, in degrees, the difference fused (`fmsubs`).
    const float mine = std::atan2(std::sin(facing), std::cos(facing));
    const float mine_degrees =
        static_cast<float>(crt_atan2(static_cast<double>(std::sin(facing)),
                                     static_cast<double>(std::cos(facing)))) *
        57.29582595825195f;
    const float wanted =
        static_cast<float>(crt_atan2(static_cast<double>(dir_x), static_cast<double>(dir_z)));
    float off = std::fma(wanted, 57.29582595825195f, -mine_degrees);
    if (180.0f < off) off = off - 360.0f;
    if (off < -180.0f) off = off + 360.0f;
    // +0x1fd, the turning flag.
    if (turned != nullptr && 5.0f < std::fabs(off)) *turned = true;
    float step = 0.0f;
    if (off < 0.0f) {
        // at least a degree's worth, the rate held to
        // -1000, no further than the angle left.
        const float scale = off < -1.0f ? off : -1.0f;
        float rate = -(std::fabs(scale) * turn_rate);
        if (rate < -1000.0f) rate = -1000.0f;
        step = rate * delta;
        if (!(step >= off)) step = off;
    } else if (off > 0.0f) {
        const float scale = off > 1.0f ? off : 1.0f;
        float rate = std::fabs(scale) * turn_rate;
        if (rate > 1000.0f) rate = 1000.0f;
        step = rate * delta;
        if (step > off) step = off;
    }
    // the speed factor (`turn_factor`) times the step
    // - after the step was held to the angle, so a fast unit turns past it.
    return mine + (factor * step) * 0.01745329238474369f;
}

// `CCharacter::rollCritical(CCharacter*)`.
bool roll_critical(RunicRandom& rolls, float chance) {
    // the roll kept to at most 97 (a NaN becomes 97,
    // a critical), rounded up, compared with 97. The console's always-critical
    // flag is not carried.
    const float roll = chance + rolls.between(1.0f, 100.0f);
    const float kept = roll < 97.0f ? roll : 97.0f;
    return truncate(std::ceil(kept)) == 97;
}

int max_damage(int base, int type, int stat, const CharacterEffects& effects,
               const WeaponTraits& weapon, const CharacterEffects& master, float scale) {
    // The console's chain:
    // the weapon kind's percentage times 0.01f, then each term
    // fused on times 0.01f (`fmadds`): the attribute, the dual-wielding bonus,
    // the weapon's category, `PERCENT DAMAGE BONUS` of the attack's type without
    // the other hand's and of all, and the
    // master's `PERCENT PET DAMAGE`.
    float total = effects.effect_value(weapon.ranged ? 0x10 : 0xf, 7) * 0.01f;
    total = std::fma(static_cast<float>(stat), 0.01f, total);
    if (weapon.dual) total = std::fma(effects.effect_value(0x58, 7), 0.01f, total);
    if (weapon.ranged) {
        if (weapon.ranged_item) total = std::fma(effects.effect_value(0x66, 7), 0.01f, total);
        if (weapon.magic_item) total = std::fma(effects.effect_value(0x67, 7), 0.01f, total);
    } else if (weapon.held) {
        if (weapon.martial) total = std::fma(effects.effect_value(99, 7), 0.01f, total);
        if (weapon.magic_item) total = std::fma(effects.effect_value(0x67, 7), 0.01f, total);
    }
    total = std::fma(effects.effect_value_minus_location(0x19, weapon.minus_location, type), 0.01f,
                     total);
    total = std::fma(effects.effect_value(0x19, 6), 0.01f, total);
    if (master) total = std::fma(master.effect_value(0x5b, 7), 0.01f, total);
    const int scaled = truncate(std::ceil(static_cast<float>(base) * total));
    // `MELEEDAMAGEBONUS` (0) or `RANGEDDAMAGEBONUS` (1), then `DAMAGE BONUS` (10)
    // of the attack's type, again without the other hand.
    const int flat = truncate(std::ceil(effects.effect_value(weapon.ranged ? 1 : 0, 7)));
    const int bonus =
        truncate(effects.effect_value_minus_location(10, weapon.minus_location, type));
    return truncate(static_cast<float>(bonus + (scaled + flat + base)) * scale);
}

// PORT: a placed unit's top damage, through `max_damage` with no weapon and no strength.
int max_damage(const UnitCombat& attacker, const EffectManager* effects,
               const CharacterEffects& master) {
    // A placed unit holds no weapon and has no `strength()`.
    return max_damage(attacker.attack.max_damage, attacker.attack.damage_type, 0,
                      CharacterEffects(effects), WeaponTraits{}, master, attacker.damage_scale);
}

// `CCharacter::damageDefense(EDAMAGE_TYPES)`.
int damage_defense(const UnitCombat& combat, int type, int defense, const EffectManager* effects) {
    // gDAMAGE_DEFENSE_EFFECT_TYPES.
    static constexpr std::array<int, kDamageTypeCount> kDefenseEffects{0x23, 0x24, 0x25, 0x26,
                                                                       0x27, 0x28, 0x00};
    const auto value = [effects](int effect, int damage_type) {
        return effects != nullptr ? effects->effect_value(effect, damage_type) : 0.0f;
    };
    // (its one caller is `modifyDamage`):
    // `fma(type% + all%, 0.01f, 1) * own`, kept above 0 (a NaN is 0); the type's
    // own defence effect added; `defense() * 0.01f` of that sum; then the raised
    // figure plus the sum, less the degrade.
    return damage_defense_formula(
        combat.damage_defense[static_cast<size_t>(type)], value(0x17, type), value(0x17, 6),
        value(kDefenseEffects[static_cast<size_t>(type)], 7), defense, value(0x42, 7));
}

int damage_defense_formula(int own, float pct_type, float pct_all, float own_effect, int defense,
                           float degrade) {
    float d = std::fma(pct_type + pct_all, 0.01f, 1.0f) * static_cast<float>(own);
    if (!(d > 0.0f)) d = 0.0f;
    const float sum = own_effect + d;
    const float raised = std::ceil((static_cast<float>(defense) * 0.01f) * sum);
    const float with_raise = static_cast<float>(truncate(raised)) + sum;
    return truncate(std::ceil(with_raise - static_cast<float>(truncate(std::ceil(degrade)))));
}

// `CCharacter::defense()` for a unit that is
// not a player: `+0x430` is the 10 `CCharacter::CCharacter`
// sets - only `CPlayer::unitInit`
// reads a unit's `DEFENSE` over it.
int unit_defense(const EffectManager* effects) {
    const auto value = [effects](int effect, int damage_type) {
        return effects != nullptr ? effects->effect_value(effect, damage_type) : 0.0f;
    };
    const float any = value(0x11, 7);
    const float all = value(0x11, 6);
    const int own = kCharacterBaseDefense;
    // `CCharacter::defense`: the two percents summed, times
    // 0.01f, times the base.
    const float percent = std::ceil(static_cast<float>(own) * ((all + any) * 0.01f));
    const float flat = std::ceil(value(2, 7));
    return truncate(percent) + truncate(flat) + own;
}

int armor_class(const UnitCombat& combat, int defense, const EffectManager* effects,
                const CharacterEffects& master, int worn_armor) {
    const auto value = [effects](int effect) {
        return effects != nullptr ? effects->effect_value(effect, 7) : 0.0f;
    };
    // `CCharacter::armorBonus`, then
    // `CCharacter::AC`.
    const float pet = master ? master.effect_value(0x61, 7) : 0.0f;
    // `+0x38c` plus the inventory's `+0x08`.
    const int bonus = armor_bonus_formula(combat.armor + worn_armor, value(0x17),
                                          master ? &pet : nullptr, value(0x8));
    return ac_formula(bonus, defense, value(0x42));
}

int armor_bonus_formula(int armor, float pct, const float* pet_pct, float flat) {
    // the percent times 0.01f, the master's fused
    // on (`fmadds`).
    float percent = pct * 0.01f;
    if (pet_pct != nullptr) percent = std::fma(*pet_pct, 0.01f, percent);
    return truncate(std::ceil(static_cast<float>(armor) * percent)) + truncate(std::ceil(flat)) +
           armor;
}

int ac_formula(int bonus, int defense, float degrade) {
    // (defense * 0.01f) * bonus, rounded up; the
    // degrade's ceiling off; a negative is 0.
    const float raised =
        std::ceil((static_cast<float>(defense) * 0.01f) * static_cast<float>(bonus));
    const int ac = (truncate(raised) + bonus) - truncate(std::ceil(degrade));
    return -1 < ac ? ac : 0;
}

namespace {

// `CEquipment::addInherentDamage`'s list of
// the item's damage types: a type once.
void note_damage_type(ItemStats& item, int type) {
    for (const int held : item.damage_types) {
        if (held == type) return;
    }
    item.damage_types.push_back(type);
}

}  // namespace

// `CEquipment::addInherentDamage(EDAMAGE_TYPES, int)`.
//
void add_inherent_damage(ItemStats& item, int type, int amount) {
    note_damage_type(item, type);
    item.inherent_damage[static_cast<size_t>(type)] += amount;
}

// `CEquipment::addDamageBonus(EDAMAGE_TYPES, int)`.
//
void add_damage_bonus(ItemStats& item, int type, int amount) {
    note_damage_type(item, type);
    item.damage_bonus[static_cast<size_t>(type)] += amount;
}

// `CEquipment::calculateCombatStats(bool)`,
// the damage half, and `CEquipment::setGraphDamage(unsigned)`.
//
void roll_weapon_damage(ItemStats& item, bool is_weapon, const StatBook& graphs, RunicRandom& rolls,
                        int heirloom) {
    // `MINDAMAGE` and `MAXDAMAGE` off the unit's
    // group into +0x304 and +0x308, every time.
    int damage = static_cast<int>(item.max_damage);
    if (damage != 0) {
        int roll = rolls.integer_between(static_cast<int>(item.min_damage), damage);
        if (0 < heirloom) roll = damage;
        // each modifier times 0.01f, the roll by the
        // speed's first, then the rarity's.
        const float rarity = static_cast<float>(item.rarity_damage_mod) * 0.01f;
        const float speed = static_cast<float>(item.speed_damage_mod) * 0.01f;
        const uint32_t scaled =
            static_cast<uint32_t>(truncate((static_cast<float>(roll) * speed) * rarity));
        const float base = graphs.value(kWeaponDamageGraph, static_cast<float>(item.level), 0.0f);
        const uint32_t steps = static_cast<uint32_t>(heirloom < 6 ? heirloom : 5);
        // The sum is zero-extended and converted as a 64-bit integer.
        // `setGraphDamage`: (points * curve)
        // times 0.01f, rounded up, into +0x308, +0x304 and +0x314.
        damage = truncate(std::ceil(
            (static_cast<float>(static_cast<int64_t>(scaled + steps * 10u)) * base) * 0.01f));
        item.kept_damage = damage;
    }
    item.damage = damage;
    item.combat_rolled = true;
    split_weapon_damage(item, is_weapon);
}

void split_weapon_damage(ItemStats& item, bool is_weapon) {
    // CEquipment::calculateInherentDamages (inlined into
    // calculateCombatStats(bool) on Linux): the inherent damage emptied (each type
    // kept), and for a weapon (ISA 8) the figure split by `DAMAGE_<TYPE>`.
    item.inherent_damage = {};
    if (!is_weapon) return;
    // the damage times 0.01f, each part's
    // percent times that.
    const float per = static_cast<float>(item.damage) * 0.01f;
    // `addInherentDamage` in the order the original asks: electric, fire,
    // ice, poison.
    for (int type : {4, 2, 3, 5}) {
        const int pct = item.damage_split[static_cast<size_t>(type)];
        if (0 < pct) {
            add_inherent_damage(item, type, truncate(static_cast<float>(pct) * per));
        }
    }
    // the figure a save keeps, before `DAMAGE_PHYSICAL`.
    item.kept_damage = item.damage;
    const int physical = item.damage_split[0];
    if (!(physical < 0)) item.damage = truncate(static_cast<float>(physical) * per);
}

// `CEquipment::calculateCombatStats(bool)`,
// the armour half, and `CEquipment::setGraphAC(unsigned)`.
//
void roll_item_armour(ItemStats& item, bool is_armor, bool from_save, const StatBook& graphs,
                      RunicRandom& rolls, int heirloom) {
    // each modifier times 0.01f.
    const float rarity = static_cast<float>(item.rarity_armor_mod) * 0.01f;
    const float special = static_cast<float>(item.special_armor_mod) * 0.01f;
    const auto set_graph_ac = [&](uint32_t points) {
        const float curve = graphs.value(kPlayerArmorGraph, static_cast<float>(item.level), 0.0f);
        const uint32_t steps = static_cast<uint32_t>(heirloom < 6 ? heirloom : 5);
        // As `setGraphDamage`: the sum is converted as a 64-bit integer.
        // `setGraphAC`: (curve * points) times
        // 0.01f, rounded up.
        const int ac = truncate(std::ceil(
            (curve * static_cast<float>(static_cast<int64_t>(points + steps * 10u))) * 0.01f));
        item.graph_ac = ac < 1 ? 1 : ac;
        item.kept_armour = item.graph_ac;
    };
    item.graph_ac = item.armor;
    if (item.graph_ac == 0) {
        if (item.armor_max != 0 && item.armor_min != 0) {
            const int top = item.armor_min <= item.armor_max ? item.armor_max : item.armor_min;
            const int roll = rolls.integer_between(item.armor_min, top);
            // by the special modifier first.
            set_graph_ac(
                static_cast<uint32_t>(truncate((static_cast<float>(roll) * special) * rarity)));
        }
    } else {
        // by the special modifier first.
        set_graph_ac(static_cast<uint32_t>(
            truncate((static_cast<float>(item.graph_ac) * special) * rarity)));
    }
    if (!is_armor) return;
    const float points = static_cast<float>(item.graph_ac);
    if (!from_save) {
        // Electric, fire, ice, poison, as the original reads them; effect
        // types 0x27, 0x25, 0x26, 0x28 (`CEffect::CEffect`
        // with duration -1000, activation 0).
        constexpr std::array<std::pair<int, int>, 4> kResists{
            {{4, 0x27}, {2, 0x25}, {3, 0x26}, {5, 0x28}}};
        for (const auto& [damage_type, effect_type] : kResists) {
            const int pct = item.armor_split[static_cast<size_t>(damage_type)];
            if (!(0 < pct)) continue;
            // `CEffect(type, true, PASSIVE, -1000,
            // (pct * 0.01f) * points, 1.0, false)`.
            item.combat_effects.push_back(value_effect(
                effect_type, kDurationAlways, (static_cast<float>(pct) * 0.01f) * points,
                EffectActivation::Passive, /*flag30=*/true, /*f38=*/1.0f));
        }
    }
    const int physical = item.armor_split[0];
    if (physical != -1) {
        const int ac =
            truncate((static_cast<float>(physical) * 0.01f) * points);
        item.graph_ac = ac < 1 ? 1 : ac;
        item.kept_armour = item.graph_ac;
    }
}

// `CEquipment::calculateCombatStats(bool)`.
void calculate_combat_stats(ItemStats& item, bool is_weapon, bool is_armor, bool from_save,
                            const StatBook& graphs, RunicRandom& rolls, int heirloom) {
    roll_item_armour(item, is_armor, from_save, graphs, rolls, heirloom);
    roll_weapon_damage(item, is_weapon, graphs, rolls, heirloom);
}

// `CCharacter::modifyDamage(int, EDAMAGE_TYPES, int, float)`.
//
int modify_damage(const Defender& defender, int damage, int type, int maximum, float factor,
                  RunicRandom& rolls) {
    int roll = 0;
    if (type == 0) {
        const int ac = defender.armor;
        const int minimum = truncate(std::ceil(static_cast<float>(ac) * 0.5f));
        roll = rolls.integer_between(minimum, ac);
    } else {
        int defence =
            truncate(static_cast<float>(defender.damage_defense[static_cast<size_t>(type)]));
        if (defence < 0) defence = 0;
        roll =
            rolls.integer_between(truncate(std::ceil(static_cast<float>(defence) * 0.5f)), defence);
    }
    const int given = damage;
    if (-1 < maximum && -1 < damage) {
        // `fdivs`, then the share times the roll, then the factor.
        const float share = static_cast<float>(damage) / static_cast<float>(maximum);
        damage = wrapping_sub(damage, truncate(share * static_cast<float>(roll) * factor));
    }
    // `CCharacter::damageDefensePercent(type)` (inlined on the
    // console): `PERCENT DAMAGE TAKEN` of the type and of all (6),
    // summed and rounded up; then `fmadds` for `percent * 0.01 + 1`.
    const float percent =
        std::ceil(defender.effects.effect_value(kEffectTypePercentDamageTaken, type) +
                  defender.effects.effect_value(kEffectTypePercentDamageTaken, 6));
    damage = truncate(std::fma(static_cast<float>(truncate(percent)), 0.01f, 1.0f) *
                      static_cast<float>(damage));
    // `CBaseUnit::hasEffect(SHIELD BUFFER)` on the unit's own
    // manager, then its shields spent; absorbed, the damage is 0.
    bool absorbed = false;
    if (defender.shields != nullptr && defender.shields->has_effect(kEffectTypeShieldBuffer)) {
        damage = defender.shields->spend_shield_buffers(damage, type, absorbed);
    }
    if (absorbed) {
        damage = 0;
        if (defender.on_absorbed) defender.on_absorbed();
    }
    // a physical hit nothing absorbed is never under 1; a negative
    // result is 0, but 1 when nothing was absorbed and the whole maximum, over
    // 10, was asked for.
    if (type == 0 && damage < 1 && !absorbed) return 1;
    if (0 <= damage) return damage;
    if (!absorbed && given == maximum && 10 < given) return 1;
    return 0;
}

// `CCharacter::blocked()`.
bool blocked(const Defender& defender, RunicRandom& rolls) {
    const int chance = defender.block_chance < 50 ? defender.block_chance : 50;
    const int roll = rolls.integer_between(0, 100);
    if (roll == 0 || roll == 100) return false;
    return roll < chance;
}

namespace {

/// `CCharacter::rollAttack`, one type's
/// weapon part in its loops: `bonus` scaled, and what the character's stats and
/// effects raise it by.
struct TypePart {
    int bonus = 0;  ///< the type's damage, after the damage scale
    int raise = 0;  ///< `ceilf(bonus * share)`
};
TypePart raised_bonus(const Strike& strike, int type) {
    TypePart part;
    part.bonus = strike.bonus[static_cast<size_t>(type)];
    // A scale other than 1 truncates the scaled bonus, in both type loops.
    if (strike.damage_scale != 1.0f) {
        part.bonus = truncate(static_cast<float>(part.bonus) * strike.damage_scale);
    }
    // The console's chain: the type's
    // `PERCENT DAMAGE BONUS` times 0.01f, then each term fused on times 0.01f
    // (`fmadds`): all's, the attack's, the attribute, the weapon's kinds,
    // `magic()` past physical, the dual-wielding bonus.
    float share = strike.percent_bonus[static_cast<size_t>(type)] * 0.01f;
    share = std::fma(strike.percent_all, 0.01f, share);
    share = std::fma(strike.attack_percent, 0.01f, share);
    share = std::fma(strike.share, 0.01f, share);
    for (size_t k = 0; k < strike.item_percent.size(); ++k) {
        if (strike.item_kind[k]) share = std::fma(strike.item_percent[k], 0.01f, share);
    }
    if (0 < type && strike.weapon) share = std::fma(strike.magic_share, 0.01f, share);
    if (strike.dual) share = std::fma(strike.dual_percent, 0.01f, share);
    part.raise = truncate(std::ceil(static_cast<float>(part.bonus) * share));
    return part;
}

/// A type's loop runs for a flat `DAMAGE BONUS` of it or damage of it on the
/// weapon (`(0 < (int)flat) || bonus >= 1`).
bool type_strikes(const Strike& strike, int type) {
    return 0 < strike.flat_bonus[static_cast<size_t>(type)] ||
           !(strike.bonus[static_cast<size_t>(type)] < 1);
}

/// `CCharacter::rollAttack`, a critical's
/// damage: half again, plus `PERCENT CRITICAL DAMAGE` (0x59) of the half.
int critical_damage(int maximum, float percent) {
    // (percent * 0.01f) * half.
    const int half = truncate(static_cast<float>(maximum) * 0.5f);
    return half + maximum + truncate((percent * 0.01f) * static_cast<float>(half));
}

}  // namespace

// `CCharacter::rollAttack(CLevel&, CCharacter*, CEquipment*, unsigned, float, float,
// EDAMAGE_TYPES)`, with flags 0, the strike's
// two scales and the attack's own damage type.
Blow roll_attack(const Strike& strike, const Defender& defender, RunicRandom& rolls) {
    Blow blow;
    blow.critical = roll_critical(rolls, strike.critical_chance);
    int maximum = strike.max_physical;
    // A scale other than 1 truncates the scaled maximum, straight after
    // `maxDamage`.
    if (strike.damage_scale != 1.0f) {
        maximum = truncate(static_cast<float>(maximum) * strike.damage_scale);
    }
    const int low = truncate(std::ceil(static_cast<float>(maximum) * 0.5f));
    int dealt = rolls.integer_between(low, maximum);
    if (blow.critical) dealt = critical_damage(maximum, strike.critical_percent);
    // The sixth argument is every `modifyDamage`'s factor.
    //
    int after = modify_damage(defender, dealt, strike.damage_type, dealt, strike.soak_scale, rolls);
    // The pairs `applyDamageEffects` is handed: this one first.
    if (0 <= strike.damage_type && strike.damage_type < kDamageTypeCount) {
        blow.by_type[static_cast<size_t>(strike.damage_type)] += after;
    }
    blow.parts = 1;

    // Every type's top, criticals included, added to the maximum.
    // The flat bonus joins it outside the critical.
    for (int type = 0; type < kDamageTypeCount; ++type) {
        if (!type_strikes(strike, type)) continue;
        const TypePart part = raised_bonus(strike, type);
        int top = part.raise + part.bonus;
        if (blow.critical) top = critical_damage(top, strike.critical_percent);
        maximum = strike.flat_bonus[static_cast<size_t>(type)] + maximum + top;
    }
    // Then each type rolled and put through the defender's defence for it,
    // against that whole maximum.
    int rolled = dealt;
    for (int type = 0; type < kDamageTypeCount; ++type) {
        if (!type_strikes(strike, type)) continue;
        // Here the flat bonus is inside the top, and so inside the critical.
        //
        const TypePart raised = raised_bonus(strike, type);
        const int top = raised.bonus + strike.flat_bonus[static_cast<size_t>(type)] + raised.raise;
        int part = rolls.integer_between(truncate(std::ceil(static_cast<float>(top) * 0.5f)), top);
        if (blow.critical) part = critical_damage(top, strike.critical_percent);
        if (0 < part) {
            rolled += part;
            part = modify_damage(defender, part, type, maximum, strike.soak_scale, rolls);
        }
        after += part;
        blow.by_type[static_cast<size_t>(type)] += part;
        ++blow.parts;
    }

    // Interrupting the target: `INTERRUPT CHANCE` (0x38) is the third way in,
    // and below 0 a way out.
    bool interrupts = false;
    if (0.8f < static_cast<float>(rolled) / static_cast<float>(maximum) ||
        static_cast<float>(defender.max_hp) * 0.5f < static_cast<float>(after) ||
        static_cast<float>(rolls.integer_between(1, 100)) < strike.interrupt_chance) {
        interrupts = true;
    } else {
        // Not a critical, and the target is not running a skill: no.
        interrupts = blow.critical;
    }
    if (strike.interrupt_chance < 0.0f &&
        static_cast<float>(rolls.integer_between(0, 100)) < -strike.interrupt_chance) {
        interrupts = false;
    }
    const int resist = rolls.integer_between(1, 100);
    blow.interrupts = defender.interrupt_resistance < static_cast<float>(resist) && interrupts;

    // The knockback force handed to `applyDamage`.
    if (!blow.critical) {
        const float chance = rolls.between(0.0f, 1.0f);
        blow.knockback = chance < 0.7f ? rolls.between(1.0f, 4.0f) : 0.0f;
    } else {
        blow.knockback = rolls.between(1.0f, 4.0f) * 1.5f;
    }

    // Armour that brought a real blow down to the floor of 1 is a glancing
    // blow: the rolled physical part against everything left after defence.
    blow.damage = after;
    if (!(dealt == after || after != 1)) {
        blow.glancing = true;
        blow.critical = false;
    }
    blow.blocked = blocked(defender, rolls);
    if (blow.blocked) {
        blow.damage = 0;
        blow.by_type = {};
        blow.parts = 0;
    }
    blow.direction_jitter = rolls.between(-10.0f, 10.0f);
    return blow;
}

// `CCharacter::rollAttack`, what it reads off
// the attacker's effects. The eighth argument (a forced damage type) is 7 at every call
// the port makes, so the `PERCENT DAMAGE BONUS` asked is the loop's own type.
void strike_effects(Strike& strike, const CharacterEffects& effects, const WeaponTraits& weapon) {
    if (!effects) return;
    for (int type = 0; type < kDamageTypeCount; ++type) {
        strike.flat_bonus[static_cast<size_t>(type)] =
            truncate(std::ceil(effects.effect_value(10, type)));
        const float of_type = effects.effect_value(0x19, type);
        const float of_all = effects.effect_value(0x19, 6);
        strike.percent_bonus[static_cast<size_t>(type)] = of_type;
        strike.percent_all = of_all;
    }
    // The values as they are; `raised_bonus` takes each times 0.01f.
    strike.attack_percent = effects.effect_value(weapon.held && weapon.ranged ? 0x10 : 0xf, 7);
    if (weapon.held) {
        strike.item_kind = {weapon.martial, weapon.ranged_item, weapon.magic_item};
        if (weapon.martial) strike.item_percent[0] = effects.effect_value(99, 7);
        if (weapon.ranged_item) strike.item_percent[1] = effects.effect_value(0x66, 7);
        if (weapon.magic_item) strike.item_percent[2] = effects.effect_value(0x67, 7);
    }
    strike.dual = weapon.dual;
    if (weapon.dual) strike.dual_percent = effects.effect_value(0x58, 7);
    strike.critical_chance = effects.effect_value(0x37, 7);
    strike.critical_percent = effects.effect_value(0x59, 7);
    strike.interrupt_chance = effects.effect_value(0x38, 7);
}

// `CCharacter::rollAttack` and `maxDamage`,
// what they test on the weapon in use and the hands.
WeaponTraits held_weapon_traits(const ItemStats& weapon, const UnitTypeHierarchy& types,
                                bool right_held, bool left_held, bool left_is_shield) {
    WeaponTraits traits;
    traits.held = true;
    const uint32_t type = types.id_of(weapon.unittype);
    traits.ranged = types.isa(type, unit_type::kRangedWeapon);
    traits.martial = types.isa(type, unit_type::kItemCategoryMartial);
    traits.ranged_item = types.isa(type, unit_type::kItemCategoryRanged);
    traits.magic_item = types.isa(type, unit_type::kItemCategoryMagic);
    traits.dual = right_held && left_held && !left_is_shield;
    traits.minus_location = (!right_held && left_held) ? 0 : 1;
    return traits;
}

// `CCharacter::rollAttack` with a weapon: `maxDamage` on the
// weapon's attack (its `+0x24` the copy's rolled damage, `+0x78` 0 from
// `CAttackDescription::CAttackDescription`), `getDamageBonus` of each type.
Strike armed_strike(const ItemStats& weapon, const WeaponTraits& traits, int strength,
                    int dexterity, int magic, const CharacterEffects& effects,
                    const EffectManager::Context& item_effects, float damage_scale,
                    float soak_scale) {
    Strike strike;
    strike.weapon = true;
    const int stat = traits.ranged ? dexterity : strength;
    strike.share = static_cast<float>(stat);
    strike.magic_share = static_cast<float>(magic);
    strike.damage_type = 0;
    strike.max_physical = max_damage(weapon.damage, strike.damage_type, stat, effects, traits,
                                     CharacterEffects{}, 1.0f);
    strike_effects(strike, effects, traits);
    for (int type = 0; type < kDamageTypeCount; ++type) {
        strike.bonus[static_cast<size_t>(type)] = weapon_damage_bonus(weapon, type, item_effects);
    }
    strike.damage_scale = damage_scale;
    strike.soak_scale = soak_scale;
    return strike;
}

// `CCharacter::rollAttack` with no weapon in use.
Strike unarmed_strike(const AttackDescription& attack, int strength,
                      const CharacterEffects& effects, float damage_scale, float soak_scale) {
    Strike strike;
    strike.share = static_cast<float>(strength);
    strike.damage_type = attack.damage_type;
    strike.max_physical = max_damage(attack.max_damage, attack.damage_type, strength, effects,
                                     WeaponTraits{}, CharacterEffects{}, 1.0f);
    strike_effects(strike, effects, WeaponTraits{});
    for (int type = 1; type < kDamageTypeCount; ++type) {
        strike.bonus[static_cast<size_t>(type)] = attack.damage_by_type[static_cast<size_t>(type)];
    }
    strike.damage_scale = damage_scale;
    strike.soak_scale = soak_scale;
    return strike;
}

// `CCharacter::getBlockChance()`.
int block_chance(const CharacterEffects& effects) {
    if (!effects) return 0;
    const float sum = (effects.effect_value(0x3e, 7) + effects.effect_value(0x21, 7)) + 0.0f;
    if (sum > 50.0f) return 50;
    return truncate(sum);
}

// `CCharacter::applyInstantEffect(CEffect*)`,
// case 0x34.
InstantDamage instant_damage(float value, int type, float force_min, float force_max,
                             const Defender& target, const CharacterEffects* attacker,
                             RunicRandom& rolls) {
    InstantDamage out;
    // `if (0 < v) v = -v`: the arithmetic below runs on the negative.
    float v = 0.0f < value ? -value : value;
    // `0.0 <= v`: `modifyHP(v)` instead; a NaN goes on.
    if (0.0f <= v) {
        out.applied = false;
        return out;
    }
    out.blocked = blocked(target, rolls);
    if (out.blocked) return out;
    if (attacker != nullptr) {
        // `PERCENT DAMAGE BONUS` of the effect's type, then of all.
        // the type's times 0.01f, all's fused on.
        const float of_type = attacker->effect_value(0x19, type) * 0.01f;
        const float share = std::fma(attacker->effect_value(0x19, 6), 0.01f, of_type);
        v = static_cast<float>(truncate(std::ceil(share * v))) + v;
        out.critical = roll_critical(rolls, attacker->effect_value(0x37, 7));
        if (out.critical) {
            // the half, `PERCENT CRITICAL DAMAGE` of it, both truncated.
            int half = truncate(v * 0.5f);
            // (percent * 0.01f) * half.
            half = truncate((attacker->effect_value(0x59, 7) * 0.01f) * static_cast<float>(half)) +
                   half;
            v = static_cast<float>(half) + v;
            const int roll = rolls.integer_between(0, 100);
            out.interrupts = target.interrupt_resistance < static_cast<float>(roll);
        }
    }
    // `modifyDamage((int)|v|, type, (int)|v|, 1.0)`, and never nothing.
    const int magnitude = truncate(std::fabs(v));
    const int slot = (0 <= type && type < kDamageTypeCount) ? type : 0;
    out.damage = modify_damage(target, magnitude, slot, magnitude, 1.0f, rolls);
    if (out.damage == 0) out.damage = 1;
    // `randomBetweenVolatile(value(2), value(3))`.
    out.force = rolls.between(force_min, force_max);
    return out;
}

// `CCharacter::applyDamageEffects`.
DamageStatus roll_damage_status(float amount, const std::array<int, kDamageTypeCount>& by_type,
                                int parts, int first_type,
                                const std::array<int, kDamageTypeCount>& own_defense, bool forced,
                                bool target_is_player, RunicRandom& rolls) {
    DamageStatus none;
    if (!(0.0f < amount)) return none;
    if (parts == 1 && first_type == 0) return none;
    for (int type = 0; type < kDamageTypeCount; ++type) {
        // The physical pairs are never summed (the type-0 test in the summing loop).
        const int dealt = type == 0 ? 0 : by_type[static_cast<size_t>(type)];
        if (!(0 < dealt)) continue;
        if (!forced) {
            const float damage = static_cast<float>(dealt);
            const float defence = static_cast<float>(own_defense[static_cast<size_t>(type)]);
            if (!(rolls.between(0.0f, 1.0f) <= damage / ((defence + damage) * 5.0f))) continue;
        }
        DamageStatus status;
        status.type = type;
        switch (type) {
            case 2:
                status.affix = target_is_player ? "DAMAGEEFFECTONFIREPLAYER" : "DAMAGEEFFECTONFIRE";
                status.duration_min = 3.0f;
                status.duration_max = 7.0f;
                return status;
            case 3:
                status.affix = target_is_player ? "DAMAGEEFFECTFROZENPLAYER" : "DAMAGEEFFECTFROZEN";
                // a player (ISA 0x1c) thaws sooner.
                status.duration_min = target_is_player ? 1.5f : 3.0f;
                status.duration_max = target_is_player ? 2.0f : 4.0f;
                return status;
            case 4:
                status.affix =
                    target_is_player ? "DAMAGEEFFECTELECTRICITYPLAYER" : "DAMAGEEFFECTELECTRICITY";
                status.duration_min = 3.0f;
                status.duration_max = 5.0f;
                return status;
            case 5:
                status.affix =
                    target_is_player ? "DAMAGEEFFECTPOISONEDPLAYER" : "DAMAGEEFFECTPOISONED";
                status.duration_min = 3.0f;
                status.duration_max = 5.0f;
                return status;
            default: return none;
        }
    }
    return none;
}

Blow roll_attack(const UnitCombat& attacker, const Defender& defender, RunicRandom& rolls,
                 const EffectManager* effects, float damage_scale, float soak_scale,
                 const CharacterEffects& master) {
    Strike strike;
    strike.max_physical = max_damage(attacker, effects, master);
    strike_effects(strike, CharacterEffects(effects), WeaponTraits{});
    strike.damage_type = attacker.attack.damage_type;
    strike.damage_scale = damage_scale;
    strike.soak_scale = soak_scale;
    // `rollAttack` with no weapon: the
    // attack's own `+0x24[type]` from type 1 on.
    for (int type = 1; type < kDamageTypeCount; ++type) {
        strike.bonus[static_cast<size_t>(type)] =
            attacker.attack.damage_by_type[static_cast<size_t>(type)];
    }
    return roll_attack(strike, defender, rolls);
}

Effect knock_back_effect(float knock_back, const Vec3& attacker, const Vec3& target) {
    // `rollAttack`: `CEffect(0x2b, (float)(int)fmadds(k, 0.5,
    // 2), fmadds(k, 0.1, 2), 0, DYNAMIC, 0.0, 0)`.
    const float duration = static_cast<float>(static_cast<int>(std::fma(knock_back, 0.5f, 2.0f)));
    const float force = std::fma(knock_back, 0.1f, 2.0f);
    Effect effect =
        value_effect(kEffectTypeKnockBackEffect, duration, force, EffectActivation::Dynamic);
    // `CEffect::setName(L"KNOCKBACK")` (inlined on
    // the console), upper-cased.
    effect.name = "KNOCKBACK";
    // target less attacker on the ground, `y` 0, its length
    // `fsqrts(fmadds(x, x, z * z))`; divided through only above 1e-8.
    float x = target.x - attacker.x;
    const float y = 0.0f;
    float z = target.z - attacker.z;
    const float length = std::sqrt(std::fma(x, x, z * z));
    if (static_cast<double>(length) > 1e-08) {
        const float inverse = 1.0f / length;
        effect.direction = {inverse * x, inverse * y, z * inverse};
    } else {
        effect.direction = {x, y, z};
    }
    return effect;
}

Effect degrade_armor_effect(float degrade) {
    // `rollAttack`: `CEffect(0x42, (float)(int)degrade, 10.0,
    // 0, DYNAMIC, 0.0, 0)`, `setName(L"DegradeArmor")` upper-cased.
    const float duration = static_cast<float>(static_cast<int>(degrade));
    Effect effect =
        value_effect(kEffectTypeDegradeArmorEffect, duration, 10.0f, EffectActivation::Dynamic);
    effect.name = "DEGRADEARMOR";
    return effect;
}

float dmg_to_reflect_from_missile(float chance, const EffectManager* own, bool has_inventory,
                                  const std::array<const EffectManager*, 12>& worn,
                                  RunicRandom& rolls) {
    // `CCharacter::getDmgToReflectFromMissile`. Both the
    // "no" exits return 0; a NaN chance is refused (`ble` branches unless greater).
    if (!(0.0f < chance)) return 0.0f;
    if (chance < rolls.between(0.0f, 100.0f)) return 0.0f;
    // The character's own (X360 `+0x17c`), its third and fourth numbers
    // (`+0xe8` / `+0xec`).
    float low = 0.0f;
    float high = 0.0f;
    if (own != nullptr) {
        if (const LiveEffect* live = own->get_effect(kEffectTypeMissileReflect)) {
            low = live->effect.values[2];
            high = live->effect.values[3];
        }
    }
    // each worn item's own, the larger kept at each end.
    if (has_inventory) {
        for (const EffectManager* item : worn) {
            if (item == nullptr) continue;
            const LiveEffect* live = item->get_effect(kEffectTypeMissileReflect);
            if (live == nullptr) continue;
            if (low < live->effect.values[2]) low = live->effect.values[2];
            if (high < live->effect.values[3]) high = live->effect.values[3];
        }
    }
    //
    if (!(0.0f < low) && !(0.0f < high)) return 1.0f;
    if (!(0.0f < high)) return low * 0.01f;
    return rolls.between(low, high) * 0.01f;
}

Effect missile_reflect_visual_effect(float share) {
    // `rollAttack`: `CEffect(0x3d, 0.3, 1.0, true, PASSIVE,
    // 0.0, 0)`, then `+0xe8 = share` and `calculateBaseValue(0)`.
    Effect effect = value_effect(kEffectTypeMissileReflectVisual, 0.3f, 1.0f,
                                 EffectActivation::Passive, /*flag30=*/true);
    effect.values[2] = share;
    return effect;
}

void transfer_attack_effects(const AttackEffectSources& sources,
                             const std::function<bool(const Effect&)>& valid,
                             const std::function<void(Effect&)>& apply) {
    // `rollAttack`: the type outermost.
    if (sources.own == nullptr) return;
    for (int type = 0; type < kTransferEffectTypes; ++type) {
        sources.own->transfer_effects(type, valid, apply);
        if (!sources.has_inventory) continue;
        if (sources.inventory != nullptr) sources.inventory->transfer_effects(type, valid, apply);
        for (const EffectManager* item : sources.worn) {
            if (item != nullptr) item->transfer_effects(type, valid, apply);
        }
    }
}

}  // namespace tl::core
