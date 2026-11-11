// Recipes and the combine: `CRecipes`, `CRecipe` and what
// `CCombineMenu::performInteraction` does to the four combine slots: pick a
// recipe they satisfy, consume it, hand back what is left, make the first
// result. Sounds, the CEGUI slots and dropping into the level are the caller's
// (`CombineOutcome`).
//
// No OGRE and no file I/O: the caller hands the recipe files over in
// `CFileSystem::getFileList`'s order.
#pragma once

#include "core/inventory.h"
#include "core/item_roll.h"
#include "core/random.h"
#include "core/spawn_class.h"
#include "core/unit_types.h"
#include "core/units.h"
#include "formats/adm.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tl::core {

/// One `[INGREDIENT]` (0x18 bytes, allocated in `CRecipe::load`).
struct RecipeIngredient {
    /// +0x00: `UNITTYPE` through `CHierarchy::getTypeIDByName`,
    /// or `unit_type::kNone` (0x16) when
    /// `UNITTYPE` is empty, which means "match by `unit`".
    uint32_t unit_type = unit_type::kNone;
    std::string unit;   ///< +0x08, `UNIT`, default empty: the item's `NAME`
    int32_t count = 1;  ///< +0x10, `COUNT`, default 1
};

/// One `[RESULT]` (0x10 bytes: two strings).
struct RecipeResult {
    /// +0x00, `SPAWNCLASS`, default empty. When it is not empty the result is
    /// a roll of that spawn class and `unit` is not read.
    std::string spawn_class;
    std::string unit;  ///< +0x08, `UNIT`, default empty
};

/// `CRecipe` (0x38 bytes). Its `NAME` is not read by anything.
struct Recipe {
    std::vector<RecipeIngredient> ingredients;  ///< +0x08 (count +0x10)
    std::vector<RecipeResult> results;          ///< +0x20 (count +0x28)
};

/// `CRecipe::load(CDataGroup*)`: every
/// `[INGREDIENT]` child, then every `[RESULT]` child, in file order.
Recipe load_recipe(const tl::formats::AdmSection& root, const UnitTypeHierarchy& unit_types);

/// `CRecipes` (`CRecipes::getSingleton`): one recipe per file, a plain
/// list (`+0x18`, count `+0x20`), no index.
class RecipeBook {
public:
    /// `CRecipes::reload()`, which
    /// `CRecipes::CRecipes` runs once: the list emptied, then one
    /// recipe per file in the order given.
    void reload(const std::vector<const tl::formats::AdmFile*>& files,
                const UnitTypeHierarchy& unit_types);
    void add(Recipe recipe) { recipes_.push_back(std::move(recipe)); }
    // PORT: an accessor of the port's recipe list.
    const std::vector<Recipe>& recipes() const { return recipes_; }
    size_t size() const { return recipes_.size(); }

private:
    std::vector<Recipe> recipes_;
};

/// The combine menu's four slots, in the order it reads them:
/// `CCombineMenu::createMenus` stores 15, 16, 17, 18 at
/// `+0xe8..+0xf4`.
inline constexpr std::array<int, 4> kCombineSlots = {
    equip_location::kCombine1, equip_location::kCombine2, equip_location::kCombine3,
    equip_location::kCombine4};

/// The two samples `performInteraction` plays on the menu's own bank
/// (`+0xe0`), and the one it queues on the owner's (`+0x298`) after a success.
inline constexpr int kCombineFailSample = 0x18;
inline constexpr int kCombineSuccessSample = 0x24;
inline constexpr int kCombineOwnerSample = 0x3f;
inline constexpr float kCombineOwnerSampleDelay = 0.1f;
/// `incrementJournalStatistic(0x10, 1)`:
/// items transmuted.
inline constexpr int kJournalItemsTransmuted = 0x10;

/// What `perform_combine` needs from around the inventory.
struct CombineContext {
    const RecipeBook* recipes = nullptr;
    const UnitTypeHierarchy* unit_types = nullptr;
    /// `CUnitResourceList::getDataGroupByObjectName(L"ITEMS", name)` then its
    /// `UNIT_GUID` (`CResourceManager::getUnitGuidByDataGroup`):
    /// what a by-unit ingredient is matched on. An
    /// item's own is `ItemStats::unit_guid`.
    const ItemBook* items = nullptr;
    /// `CResourceManager::getSpawnClassByName`.
    ///
    std::function<const SpawnClass*(const std::string& name)> spawn_class;
    /// Makes one item: `createEquipment(name, false, true)`
    /// (-> `createItem`), then
    /// `enchant(true)` when `force_enchant`: `CreateLootUnit`'s shape. An item
    /// does not read `level`.
    CreateLootUnit create_unit;
    /// The player: `rollSpawnClass(player, player, player level, ...)`.
    SpawnCharacterContext player;
    int32_t player_level = 1;  ///< `CCharacter +0x100`
    const SpawnContext* spawn_context = nullptr;
    RunicRandom* game_rng = nullptr;  ///< `g_Rand`, the spawn-class roll
    /// The deleting destructor of a used-up ingredient, as it is consumed: its effect
    /// manager's affixes rolling their effects again (`EffectManager::destroy`).
    std::function<void(const ItemStats& item)> destroy_item;
};

/// No sample at all: the console's transmute with nothing matched.
inline constexpr int kCombineNoSample = -1;

/// What the caller presents after `perform_combine` or `transmute`.
struct CombineOutcome {
    bool matched = false;
    /// The menu bank's sample: `kCombineFailSample` or `kCombineSuccessSample`
    /// (the interface bank's `Error` and `Reveal`); `kCombineNoSample` when
    /// the console's transmute found no recipe and did nothing.
    int sample = kCombineFailSample;
    /// A success also queues `kCombineOwnerSample` on the owner's bank, when
    /// the owner has one, `kCombineOwnerSampleDelay` seconds on.
    bool owner_sample = false;
    /// The ingredients used up whole, each of which the menu reports
    /// (`itemUpdatedInMenu(item, false)`) and destroys, in the order consumed.
    std::vector<ItemStats> consumed;
    /// Left-over items the bag would not take back: the caller puts each in
    /// the level at the owner's position (`CLevel::addItem`)
    /// and drops it (vtable `+0x360`,
    /// `CEquipment::drop`).
    std::vector<InventorySlot> dropped;
    /// The result's spawn class was not found: the combine returned there,
    /// after the leftovers went back and before any journal count.
    bool unknown_spawn_class = false;
    /// The result's items, by the combine slot each went into.
    std::vector<int> result_slots;
    /// `incrementJournalStatistic(kJournalItemsTransmuted, 1)` was called.
    bool journal_counted = false;
};

/// `CCombineMenu::performInteraction()` over
/// `inventory`'s slots 15-18. Matching, per recipe in book order and per
/// ingredient: `COUNT` less the **whole stack** of every slot item that is its
/// unit (by `UNIT_GUID`, type 0x16) or ISAs its type (`RANDOMMAGIC` 0x87:
/// magical, neither `UNIQUE` nor `MAGIC`); above 0 (signed) rejects. The pick:
/// the first match, then any whose summed `COUNT`s exceed `best`, where `best`
/// becomes the winner's **number of ingredients**.
/// Consuming takes `min(owed, stack)` per slot. Then, with a result, the
/// leftovers go back to the bag and the **first** result is made (a `UNIT` into
/// slot 15, or up to 4 units of a spawn class roll on `g_Rand`); then the
/// journal count.
CombineOutcome perform_combine(Inventory& inventory, const CombineContext& context);

/// every recipe the four combine slots of `inventory`
/// satisfy, in book order (the same test as `perform_combine`'s matching).
/// INVENTED: the console keeps the list and the four slots' item GUIDs
/// (`+0x118`) and answers from the list while the GUIDs are unchanged; the
/// port's items carry no instance GUID, so the list is built afresh.
std::vector<const Recipe*> matching_recipes(const Inventory& inventory,
                                            const CombineContext& context);

/// The console's combine: Y on a Transmuter's screen (`CMerchantMenuXenon`).
/// The ingredients are in the **Transmuter's own inventory**,
/// slots 15-18 (`move_to_inventory`). No recipe matched: nothing at all
/// happens, no sample. Otherwise `perform_combine`'s pick and consuming,
/// `Reveal` and the player's `VOICEFORTUNEGOOD` 0.1 s on; the leftovers go to
/// the player's bag (else dropped at the player), the first result into the
/// Transmuter's slots; then the journal count.
CombineOutcome transmute(Inventory& combiner, Inventory& player_bag, const CombineContext& context);

}  // namespace tl::core
