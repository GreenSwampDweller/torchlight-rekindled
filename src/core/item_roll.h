// The item roll: a base item made into a rolled copy (affixes, sockets, identification,
// requirements, prices, the enchanter's services), and the loot a dying monster or a broken
// container leaves. `CEquipment::enchant`, `CEquipment::setRequirements`, `CEquipment::unitInit`,
// `CLevel::rollTreasure`, `CResourceManager::createUnitsBySpawnClass`.
//
// Two generators, as in the original: `g_RandVolatile` for every `UTILITIES::*Volatile` roll and
// `g_Rand` for every `CRandomizer` pick, the affix pick included. Each function says which it
// draws.
#pragma once

#include "core/affix_book.h"
#include "core/collision.h"
#include "core/effect_manager.h"
#include "core/effects.h"
#include "core/random.h"
#include "core/sound_data.h"
#include "core/spawn_class.h"
#include "core/stats.h"
#include "core/unit_types.h"
#include "core/units.h"
#include "formats/adm.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace tl::core {

/// The `CGameGlobals` fields the item roll reads (`media/GLOBALS.DAT`); 0 for a missing key
/// (`CGameGlobals::CGameGlobals`, `reload`).
struct ItemRollGlobals {
    float random_enchant_chance = 0.0f;        ///< `RANDOM_ENCHANT_CHANCE`, percent
    float random_socket_chance = 0.0f;         ///< `RANDOM_SOCKET_CHANCE`, percent
    float second_socket_chance = 0.0f;         ///< `SECOND_SOCKET_CHANCE`, percent
    int32_t min_random_enchant_slots = 0;      ///< `MIN_RANDOM_ENCHANT_SLOTS`
    int32_t max_random_enchant_slots = 0;      ///< `MAX_RANDOM_ENCHANT_SLOTS`
    int32_t min_magic_item_slots = 0;          ///< `MIN_MAGIC_ITEM_SLOTS`
    int32_t max_magic_item_slots = 0;          ///< `MAX_MAGIC_ITEM_SLOTS`
    int32_t min_unique_item_slots = 0;         ///< `MIN_UNIQUE_ITEM_SLOTS`
    int32_t max_unique_item_slots = 0;         ///< `MAX_UNIQUE_ITEM_SLOTS`
    float enchanter_price_per_enchant = 0.0f;  ///< `ENCHANTER_PRICE_PER_ENCHANT`

    // The enchanter's and the enchant shrine's odds (`core/enchanting.h`), percent.
    float enchanter_socket_chance = 0.0f;          ///< `ENCHANTER_SOCKET_CHANCE`
    float enchanter_magic_chance = 0.0f;           ///< `ENCHANTER_MAGIC_CHANCE`
    float enchanter_disenchant_chance = 0.0f;      ///< `ENCHANTER_DISENCHANT_CHANCE`
    float enchanter_max_disenchant_chance = 0.0f;  ///< `ENCHANTER_MAX_DISENCHANT_CHANCE`
    float shrine_socket_chance = 0.0f;             ///< `ENCHANTSHRINE_SOCKET_CHANCE`
    float shrine_magic_chance = 0.0f;              ///< `ENCHANTSHRINE_MAGIC_CHANCE`
    float shrine_disenchant_chance = 0.0f;         ///< `ENCHANTSHRINE_DISENCHANT_CHANCE`
    float shrine_max_disenchant_chance = 0.0f;     ///< `ENCHANTSHRINE_MAX_DISENCHANT_CHANCE`
    int32_t enchanter_max_enchants = 0;            ///< `ENCHANTER_MAX_ENCHANTS`
    float enchanter_disenchant_increase = 0.0f;    ///< `ENCHANTER_DISENCHANT_INCREASE`
    int32_t shrine_max_enchants = 0;               ///< `ENCHANTSHRINE_MAX_ENCHANTS`
    float shrine_disenchant_increase = 0.0f;       ///< `ENCHANTSHRINE_DISENCHANT_INCREASE`

    /// `RETIREMENT_AGE`, the level the retirement NPC asks for: the one field here the
    /// constructor sets, to 20 (`CGameGlobals::CGameGlobals`).
    int32_t retirement_age = 20;
};

/// `CGameGlobals::reload` for the fields above, from the root
/// of the parsed `media/GLOBALS.DAT`.
ItemRollGlobals read_item_roll_globals(const tl::formats::AdmFile& globals);

/// The item data keys the roll reads, each with the original's default (`VALUE` 100,
/// `MAX_SOCKETS` 2, `SHADOWS` true, the rest 0, false or empty), from the item's `[ITEMS]` group,
/// the armour keys included (`ItemStats::armor`). Base data only; the per-copy fields are left
/// alone. `CEquipment::getSet`.
void read_item_roll_data(ItemStats& item, const tl::formats::AdmSection& section);

/// Effect types (`media/EffectsList.dat` rows) read through
/// `CCharacter::getEffectValue(type, 7)`.
namespace item_effect {
/// `rollMoney`, percent
inline constexpr int kGoldFind = 0x33;
inline constexpr int kMagicFind = 0x1b;          ///< `CSpawnClassData::rollSpawnClassData`
inline constexpr int kPriceBonus = 0x53;         ///< `sellPrice`/`buyPrice`, percent
inline constexpr int kRequirementsAll = 0x5d;    ///< every requirement getter
inline constexpr int kRequirementsArmor = 0x5e;  ///< ISA 0xd (Armor)
inline constexpr int kRequirementsSpell = 0x5f;  ///< ISA 0x81 (Spell)
/// `sendToTown`, percent
inline constexpr int kPetTownReturn = 0x60;
inline constexpr int kRequirementsMartial = 0x62;  ///< ISA 0xa2
inline constexpr int kRequirementsRanged = 0x64;   ///< ISA 0xa3
inline constexpr int kRequirementsMagic = 0x65;    ///< ISA 0xa4
inline constexpr int kBlockChance = 0x3e;  ///< `BLOCK_CHANCE`'s effect; `isMagical` ignores it
}  // namespace item_effect

// --------------------------------------------------------------------------
// Part A: the affix pool and the magic roll

/// `CResourceManager::createAffixesForUnit(CBaseUnit*, unsigned, unsigned)`:
/// the book's roll (`AffixBook::roll_and_create_random_affixes`) for the item's
/// unit type, each affix added at `level` in pick order (`CBaseUnit::addAffix`);
/// nothing for a count of 0. `rolls` draws the value rolls on the way.
void create_affixes_for_unit(ItemStats& item, const AffixBook& book, uint32_t level, uint32_t count,
                             RunicRandom& game_rng, const UnitTypeHierarchy& unit_types,
                             const EffectRolls& rolls = EffectRolls{});

/// `CEffectManager::addAffix(CAffix*, unsigned, CBaseUnit*, float)`
/// on an item's manager, for an affix made at the same `level`
/// (`CAffix::CAffix(CAffix*, unsigned)`): `setLevel` (unless -1), `CAffix::setOwner(item)`,
/// `addEffectsToEffectManager`, with their rolls; the affix joins the item.
void add_affix_to_item(ItemStats& item, Affix affix, uint32_t level, const EffectRolls& rolls);

/// The value rolls of an item's roll context: its curves and `g_RandVolatile`,
/// and no character (an item's effects have none).
EffectRolls item_effect_rolls(const struct ItemRollContext& context);

/// What the roll needs from around the item.
struct ItemRollContext {
    const ItemRollGlobals* globals = nullptr;
    const AffixBook* affixes = nullptr;
    const UnitTypeHierarchy* unit_types = nullptr;
    const StatBook* graphs = nullptr;
    RunicRandom* volatile_rng = nullptr;  ///< `g_RandVolatile`
    RunicRandom* game_rng = nullptr;      ///< `g_Rand`
    /// The item's resource manager has a level. Without one `enchant`'s socket roll,
    /// `addSockets` and `addEnchant` do nothing; every item made in play has one.
    bool in_level = true;
    /// The item's owner ISAs `kGambler` (0x7f): `recalculatePrice` then buys at
    /// `PRICE_PLAYERGAMBLE_MAGIC`.
    bool gambler_owner = false;
    /// What the item's own effect manager is built with, for `createElementalDamages`; null
    /// skips that step.
    const EffectManager::Context* effects = nullptr;
    /// The sound definitions, for what `CEquipment::unitInit` does to `g_Rand` when it fills the
    /// item's sound bank. Null: no sound data.
    const SoundDataInformation* sounds = nullptr;
};

/// The item's own unit type id (`CBaseUnit +0x1ac`), from `UNITTYPE`.
uint32_t item_unit_type(const ItemStats& item, const UnitTypeHierarchy& unit_types);

/// `CBaseUnit::ISA(type)` for an item.
bool item_isa(const ItemStats& item, uint32_t want, const UnitTypeHierarchy& unit_types);

/// The passive effects the item's effect manager holds: its own, its affixes', and the
/// `BLOCK_CHANCE` effect (type 0x3e) `CEquipment::unitInit`
/// adds before it calls `enchant`.
uint32_t item_passive_effect_count(const ItemStats& item);

/// `CEquipment::enchant(bool)`: the affix roll, a `UNIQUE`'s
/// second roll at level + 1 while it holds no passive effect, the socket roll, identification,
/// then `recalculatePrice`. Every draw is on `g_RandVolatile` but the affix picks (`g_Rand`).
void enchant_item(ItemStats& item, bool force, const ItemRollContext& context);

/// `CEquipment::removeAffixesThatDontSupportUnitType(EUNITTYPES)`:
/// every affix that `type` cannot take is
/// deleted, in list order, each by `CEffectManager::deleteAffix`,
/// which swap-removes.
void remove_affixes_that_dont_support_unit_type(ItemStats& item, uint32_t type,
                                                const UnitTypeHierarchy& unit_types);

/// `CEquipment::isMagical()`: `MAGIC` (0x37) always,
/// `SOCKETABLE` (0x78) never; otherwise a damage bonus of any type, any affix, or any passive
/// effect but the block chance's (type 0x3e).
bool item_is_magical(const ItemStats& item, const UnitTypeHierarchy& unit_types);

// --------------------------------------------------------------------------
// Part B: requirements

/// `CEquipment::setRequirements()`, from
/// `authored_requirements`. An unauthored level on a Weapon, Armor or Trinket comes from
/// `ITEM_LEVEL_REQUIREMENTS`, an authored stat on a Weapon or Armor is a percentage of
/// `ITEM_<STAT>_REQUIREMENTS`; each less the heirloom steps, kept only when above 1 (else 0).
void set_item_requirements(ItemStats& item, const StatBook& graphs,
                           const UnitTypeHierarchy& unit_types);

/// The character's effect totals the requirement getters subtract
/// (`CCharacter::getEffectValue(type, 7)`).
struct RequirementReductions {
    float all = 0.0f;      ///< 0x5d
    float martial = 0.0f;  ///< 0x62, an item ISA 0xa2
    float ranged = 0.0f;   ///< 0x64, ISA 0xa3
    float magic = 0.0f;    ///< 0x65, ISA 0xa4
    float armor = 0.0f;    ///< 0x5e, ISA 0xd
    float spell = 0.0f;    ///< 0x5f, ISA 0x81
};

/// `CEquipment::getLevelRequirement(CCharacter*)` and the four
/// stat getters (`getStrengthRequirement`, `getDexterityRequirement`
/// `getMagicRequirement`,
/// `getDefenseRequirement`): the stored requirement less
/// `(int)(all + category)`, never below 0. No character returns the stored figure.
int item_level_requirement(const ItemStats& item, const RequirementReductions* character,
                           const UnitTypeHierarchy& unit_types);
int item_strength_requirement(const ItemStats& item, const RequirementReductions* character,
                              const UnitTypeHierarchy& unit_types);
int item_dexterity_requirement(const ItemStats& item, const RequirementReductions* character,
                               const UnitTypeHierarchy& unit_types);
int item_magic_requirement(const ItemStats& item, const RequirementReductions* character,
                           const UnitTypeHierarchy& unit_types);
int item_defense_requirement(const ItemStats& item, const RequirementReductions* character,
                             const UnitTypeHierarchy& unit_types);

// --------------------------------------------------------------------------
// Part C: prices

/// `CEquipment::recalculatePrice()`: buy and sell from the
/// `PRICE_PLAYER{BUY,SELL}_<quality>` curves scaled by `VALUE` (1 for a `VALUE` of 0), and the
/// `NORMAL` pair into the unidentified prices.
void recalculate_item_price(ItemStats& item, const StatBook& graphs,
                            const UnitTypeHierarchy& unit_types, bool gambler_owner = false);

/// `recalculatePrice`'s arithmetic with its two type tests already answered:
/// `unique` is ISA 0x36, `magical` the vtable `+0x2b0` call (`isMagical`).
void set_item_prices(ItemStats& item, const StatBook& graphs, bool unique, bool magical,
                     bool gambler_owner);

/// `CEquipment::sellPrice()`: `max(stack, 1)` times the
/// per-unit sell price, the `NORMAL` one while unidentified. An identified copy adds the
/// player's price bonus (effect 0x53); `player_price_bonus` null is "no player".
int item_sell_price_of(const ItemStats& item, int stack, const float* player_price_bonus);

/// `CEquipment::buyPrice()`: the same with
/// the buy prices; the bonus is subtracted, and only from a positive total,
/// never below 1.
int item_buy_price_of(const ItemStats& item, int stack, const float* player_price_bonus);

/// `CEquipment::canEnchant()`: a Weapon, Armor, Ring,
/// `RANDOMMAGIC SOCKETABLE` (0xa0) or Necklace.
bool item_can_enchant(const ItemStats& item, const UnitTypeHierarchy& unit_types);

/// `CEquipment::enchantPrice()`: with `p =
/// PRICE_ENCHANT(item level)`, `(int)(ENCHANTER_PRICE_PER_ENCHANT * p *
/// enchant_count) + (int)p`, at least 300.
int item_enchant_price(const ItemStats& item, const StatBook& graphs,
                       const ItemRollGlobals& globals);

// --------------------------------------------------------------------------
// Part D: sockets

/// `CEquipment::getMaxSockets()`:
/// `MAX_SOCKETS`, default 2.
int item_max_sockets(const ItemStats& item);

/// `CEquipment::addSockets()`, the enchanter's socket service:
/// below the maximum, in a level, on a Weapon, Armor, Ring or Necklace. With room for two, a
/// `SECOND_SOCKET_CHANCE` roll on `g_RandVolatile` adds 2, else 1; with room for one, 1 undrawn.
void add_sockets(ItemStats& item, const ItemRollContext& context);

// --------------------------------------------------------------------------
// Part F: the paid enchant and a fresh copy

/// `CEquipment::addEnchant(int level, int min, int max)`: in a
/// level, on a Weapon, Armor, Ring or Necklace, `randomIntegerBetween(min, max)` on
/// `g_RandVolatile` affix slots at `level`; then `setRequirements` and `recalculatePrice`. The
/// enchant count is the caller's (`CEnchantMenu`).
void add_enchant(ItemStats& item, int level, int min_slots, int max_slots,
                 const ItemRollContext& context);

/// `CBaseUnit::reapplyAffixes(bool reapply)`: each name of the
/// item's `[AFFIXES]` in order, cloned from the book at the item's `LEVEL`; an unknown name adds
/// nothing, and with `reapply` a name the copy already holds is passed over.
/// `CBaseUnit::addAffix`, `CEffectManager::addAffix`,
/// `getNewAffixByName`,
/// `CEffectManager::getAffix`.
void reapply_affixes(ItemStats& item, const AffixBook& affixes, bool reapply,
                     const EffectRolls& rolls = EffectRolls{});

/// `CEquipment::unitInit(CDataGroup*, bool)`, the item-roll
/// calls only, in its order (`CBaseUnit::unitInit`;
/// `calculateCombatStats`). The authored `[AFFIXES]` go on first
/// because `enchant` rolls a `UNIQUE` only while it has no affixes. `random_enchant_allowed` and
/// `force_enchant` are `createItem`'s third and fourth arguments;
/// the fourth calls `enchant(true)` once more afterwards.
void init_item_copy(ItemStats& item, bool from_save, bool random_enchant_allowed,
                    bool force_enchant, const ItemRollContext& context);

// --------------------------------------------------------------------------
// Part E: loot

/// A character as the loot roll reads it.
struct LootCharacter {
    /// The number of the level the character stands in (the console's resource manager's level,
    /// its `+0x12c`).
    int32_t level_number = 1;
    /// `CCharacter::getEffectValue(0x33, 7)`: percent more gold.
    float gold_find = 0.0f;
    /// `DONTDROPGOLD` (`CCharacter::unitInit`).
    bool dont_drop_gold = false;
    /// For the spawn class's rarity and magic find (`SpawnCharacterContext`).
    SpawnCharacterContext spawn;
};

/// What a gold pile's sound bank reads, and the `g_Rand` its samples reseed (`make_gold_pile`).
struct GoldPileSounds {
    const SoundDataInformation* sounds = nullptr;
    RunicRandom* game_rng = nullptr;
};

/// `CItemGold::CItemGold(CResourceManager*, int amount)`: its
/// sound bank's two samples (with `bank`), then two draws on `g_RandVolatile`, a turn about Y of
/// 0..360 degrees and the size the pile grows to, 0.8..1.2. The model and particle are the caller's.
GoldPile make_gold_pile(int32_t amount, RunicRandom& volatile_rng, const GoldPileSounds& bank = {});

/// A `CREATEAS:GOLD` unit made from its data: `createNewItem`
/// constructs `CItemGold(rm, 1)`, then `CItemGold::unitInit` draws the
/// scale (`CBaseUnit::unitInit`) and, for a fresh copy, the
/// amount: `MINVALUE`..`MAXVALUE` percent of the difficulty's `GOLDDROP` curve at the level's
/// number plus one. The pile grows to the unit's scale.
void init_gold_copy(ItemStats& item, bool from_save, int32_t level_number, int32_t difficulty,
                    const StatBook& graphs, RunicRandom& volatile_rng,
                    const GoldPileSounds& bank = {});

/// Where a drop goes: `CLevel::addUnit` for what a spawn class
/// made, `CLevel::addItem(gold, at, true)` for `rollMoney`'s
/// piles. The level's two tests are the renderer's. `CEquipment::drop`,
/// `CItemGold::setActiveInLevel`,
/// `CItem::snapToGround`.
struct LootPlacement {
    Vec3 at;  ///< the dying character's or broken container's position
    /// `CLevel::mapPassable`
    std::function<bool(const Vec3&)> map_passable;
    /// `CLevel::positionPassable`
    std::function<bool(const Vec3&)> position_passable;
    std::function<void(const ItemStats& unit, const Vec3& where)> add_unit;
    std::function<void(const GoldPile& pile, const Vec3& where)> add_gold;
    /// What a pile's sound bank reads and reseeds (`make_gold_pile`).
    GoldPileSounds gold_sounds;
};

/// `CLevel::rollMoney(CCharacter* killer, CCharacter* dropper, const Vector3&)`:
/// nothing for a `DONTDROPGOLD` dropper; otherwise three tries at 0.1 on
/// `g_RandVolatile`, each pile made and placed before the next try (`randomOpenItemPosition`).
/// The draws are made with or without `place`. Returns each
/// pile's amount.
std::vector<int> roll_money(const LootCharacter* killer, const LootCharacter* dropper,
                            RunicRandom& volatile_rng, const LootPlacement* place = nullptr);

/// Makes one unit a spawn class rolled: `CResourceManager::createUnit(CDataGroup*, int level,
/// bool, bool force_enchant)`.
/// An item is `createItem`, so
/// `init_item_copy(out, false, true, force_enchant, ...)`, and keeps its authored `LEVEL`:
/// **`level` is not used** for it. Returns false when nothing was made.
using CreateLootUnit =
    std::function<bool(const std::string& unit, int32_t level, bool force_enchant, ItemStats& out)>;

/// What destroying an item draws: `~CEffectManager` `clearEffects(false)`
/// deletes every affix in order, and `~CAffix` runs
/// `CAffix::clearEffectsFromOwner`, which rolls each effect again,
/// the dead elemental ones too (`ItemStats::dead_affix_effects`).
void destroy_item_rolls(const ItemStats& item, const EffectRolls& rolls);

/// `CResourceManager::createUnitsBySpawnClass(CSpawnClass*, TArrayList&, unsigned count,
/// CCharacter*, CCharacter*, int level, int flags)`: `count`
/// rolls of the class on `g_Rand`, each unit made in turn. The enchant flags are **not** cleared
/// between rolls, so the n-th unit of a later roll reads the n-th flag of the first: kept.
std::vector<ItemStats> create_units_by_spawn_class(
    const SpawnClass& spawn_class, uint32_t count, const SpawnCharacterContext* character,
    const SpawnCharacterContext* finder, int32_t level, int32_t flags, RunicRandom& game_rng,
    const SpawnContext& spawn_context, const CreateLootUnit& create_unit);

/// What a death or a broken container leaves, for the caller to place.
struct LootDrop {
    std::vector<ItemStats> items;
    std::vector<int> gold;  ///< one `CItemGold` per entry
};

/// `CLevel::rollTreasure(CCharacter* killer, CCharacter* dropper, const Vector3&)`:
/// the `BARREL_TREASURE` spawn class once
/// (`createUnitsBySpawnClass(const std::wstring&, ...)`), the killer as the finder,
/// each unit added at the position itself; then `rollMoney(killer, dropper)`.
LootDrop roll_treasure(const SpawnClass* barrel_treasure, int32_t level_number,
                       const LootCharacter* killer, const LootCharacter* dropper,
                       RunicRandom& volatile_rng, RunicRandom& game_rng,
                       const SpawnContext& spawn_context, const CreateLootUnit& create_unit,
                       const LootPlacement* place = nullptr);

/// `CResourceManager::createUnitsBySpawnClassAtPositionInLevel`:
/// `count` rolls through `createUnitsBySpawnClass` (flags 0), each unit placed at
/// `spawned_unit_position` and handed to `place->add_unit` in the order made. The caller turns
/// the original's level -1 into the level's own number. Called by `CCharacter::die` and
/// `CTriggerUnit::spawnTreasureClass`.
std::vector<ItemStats> create_units_at_position(const SpawnClass& spawn_class, uint32_t count,
                                                const SpawnCharacterContext* character,
                                                const SpawnCharacterContext* finder, int32_t level,
                                                RunicRandom& volatile_rng, RunicRandom& game_rng,
                                                const SpawnContext& spawn_context,
                                                const CreateLootUnit& create_unit,
                                                const LootPlacement* place);

/// Where `createUnitsBySpawnClassAtPositionInLevel` puts its i-th unit: a
/// `CLevel::randomOpenPosition(at, max(0.5, i / 3), false)` is drawn for every unit;
/// a takeable that is not gold goes at `at` itself, anything else at the point's X and Z and
/// `at.y + 0.5`. Without `place` every point is open.
Vec3 spawned_unit_position(const ItemStats& unit, uint32_t index, const Vec3& at,
                           const LootPlacement* place, const UnitTypeHierarchy* unit_types,
                           RunicRandom& volatile_rng);

/// A dying character's own `[TREASURE]` (`CCharacter::unitInit`).
struct MonsterTreasure {
    const SpawnClass* spawn_class = nullptr;  ///< `SPAWNCLASS`
    int32_t min = 0;                          ///< `MIN`
    int32_t max = 0;                          ///< `MAX`
};

/// The drop at the end of `CCharacter::die`: nothing for a
/// player, an alignment-1 character, no killer, or `no_drops`. Without a treasure class,
/// `rollTreasure`; with one, `randomIntegerBetween(MIN, MAX)` rolls of it at the position, then
/// `rollMoney(killer, dying)`.
struct DyingCharacter {
    LootCharacter loot;
    bool is_player = false;  ///< ISA 0x1c
    int32_t alignment = 0;   ///< `CCharacter::alignment()`
    bool no_drops = false;   ///< +0x704
    MonsterTreasure treasure;
};
LootDrop roll_death_drop(const DyingCharacter& dying, const LootCharacter* killer,
                         const SpawnClass* barrel_treasure, RunicRandom& volatile_rng,
                         RunicRandom& game_rng, const SpawnContext& spawn_context,
                         const CreateLootUnit& create_unit, const LootPlacement* place = nullptr);

}  // namespace tl::core
