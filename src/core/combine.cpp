// Recipes and the combine. See `combine.h`.
#include "core/combine.h"

#include "core/merchant.h"

#include <algorithm>
#include <optional>

namespace tl::core {

Recipe load_recipe(const tl::formats::AdmSection& root, const UnitTypeHierarchy& unit_types) {
    // `CRecipe::load(CDataGroup*)`.
    Recipe recipe;
    for (const tl::formats::AdmSection& group : root.children) {
        if (group.name != "INGREDIENT") continue;
        RecipeIngredient ingredient;
        ingredient.unit = group.string_or("UNIT", "");
        const std::string type = group.string_or("UNITTYPE", "");
        ingredient.count = static_cast<int32_t>(group.integer_or("COUNT", 1));
        // An empty `UNITTYPE` is 0x16, "by unit"; anything else is looked up.
        ingredient.unit_type = type.empty() ? unit_type::kNone : unit_types.id_of(type);
        recipe.ingredients.push_back(std::move(ingredient));
    }
    for (const tl::formats::AdmSection& group : root.children) {
        if (group.name != "RESULT") continue;
        RecipeResult result;
        result.unit = group.string_or("UNIT", "");
        result.spawn_class = group.string_or("SPAWNCLASS", "");
        recipe.results.push_back(std::move(result));
    }
    return recipe;
}

void RecipeBook::reload(const std::vector<const tl::formats::AdmFile*>& files,
                        const UnitTypeHierarchy& unit_types) {
    // `CRecipes::reload()`.
    recipes_.clear();
    for (const tl::formats::AdmFile* file : files) {
        if (file == nullptr) continue;
        recipes_.push_back(load_recipe(file->root, unit_types));
    }
}

namespace {

/// `UNIT_GUID` of the `ITEMS` entry by that name, or nothing when there is none.
// `CCombineMenu::performInteraction()`: by unit.
std::optional<int64_t> ingredient_guid(const CombineContext& context, const std::string& unit) {
    if (context.items == nullptr) return std::nullopt;
    const ItemStats* item = context.items->find(unit);
    if (item == nullptr) return std::nullopt;
    return item->unit_guid;
}

/// The unit-type ingredient's test, identical in the match and in the
/// consume: `ISA(item, type)`, or for `RANDOMMAGIC` (0x87) an item that
/// `isMagical()` (vtable `+0x2b0`) and is neither `UNIQUE` (0x36) nor `MAGIC`
/// (0x37).
// `CCombineMenu::performInteraction()`.
bool type_ingredient_takes(const ItemStats& item, uint32_t type, const UnitTypeHierarchy& types) {
    bool random_magic = false;
    if (type == unit_type::kRandomMagic) {
        random_magic = item_is_magical(item, types) && !item_isa(item, unit_type::kUnique, types) &&
                       !item_isa(item, unit_type::kMagic, types);
    }
    return item_isa(item, type, types) || random_magic;
}

/// Whether the four slots satisfy every ingredient of `recipe`. A slot is
/// read afresh for each ingredient, so one stack can satisfy several.
// `CCombineMenu::performInteraction()`.
bool recipe_matches(const Recipe& recipe, const Inventory& inventory,
                    const CombineContext& context) {
    for (const RecipeIngredient& ingredient : recipe.ingredients) {
        uint32_t owed = static_cast<uint32_t>(ingredient.count);
        if (ingredient.unit_type == unit_type::kNone) {
            const std::optional<int64_t> guid = ingredient_guid(context, ingredient.unit);
            if (guid) {
                for (int slot : kCombineSlots) {
                    if (!inventory.occupied(static_cast<size_t>(slot))) continue;
                    const InventorySlot& held = inventory.slots()[static_cast<size_t>(slot)];
                    if (held.item.unit_guid == *guid) owed -= static_cast<uint32_t>(held.count);
                }
            }
        } else {
            for (int slot : kCombineSlots) {
                if (!inventory.occupied(static_cast<size_t>(slot))) continue;
                const InventorySlot& held = inventory.slots()[static_cast<size_t>(slot)];
                if (type_ingredient_takes(held.item, ingredient.unit_type, *context.unit_types)) {
                    owed -= static_cast<uint32_t>(held.count);
                }
            }
        }
        // `0 < (int)remaining` rejects: a surplus wraps below 0 and passes.
        if (0 < static_cast<int32_t>(owed)) return false;
    }
    return true;
}

/// One slot's share of an ingredient: a stack of 1 costs 1 and
/// the item goes; a larger one gives `min(owed, stack)` and goes only if that
/// empties it.
void consume_from(Inventory& inventory, int slot, int32_t& owed, const CombineContext& context,
                  CombineOutcome& outcome) {
    const InventorySlot& held = inventory.slots()[static_cast<size_t>(slot)];
    const int stack = held.count;
    if (stack < 2) {
        owed -= 1;
    } else {
        const int taken = owed <= stack ? owed : stack;
        owed -= taken;
        if (0 < stack - taken) {
            // `incrementStackBy(-taken)` (vtable `+0x338`) and no more.
            inventory.remove_item(static_cast<size_t>(slot), taken);
            return;
        }
    }
    // `CInventory::removeEquipment`, then the
    // menu's `itemUpdatedInMenu(item, false)` and the item's destructor.
    outcome.consumed.push_back(held.item);
    inventory.remove_item(static_cast<size_t>(slot), stack);
    if (context.destroy_item) context.destroy_item(outcome.consumed.back());
}

/// The pick: `best` starts
/// at 0 and the winner at the first match; a match whose summed counts exceed
/// `best` wins, and `best` becomes its number of ingredients.
const Recipe* pick_recipe(const std::vector<const Recipe*>& matches) {
    const Recipe* winner = matches.front();
    uint32_t best = 0;
    for (const Recipe* match : matches) {
        uint32_t sum = 0;
        for (const RecipeIngredient& ingredient : match->ingredients) {
            sum += static_cast<uint32_t>(ingredient.count);
        }
        if (best < sum) {
            best = static_cast<uint32_t>(match->ingredients.size());
            winner = match;
        }
    }
    return winner;
}

/// Consuming the winner from the four combine slots of `slots`, ingredient by
/// ingredient, each slot while anything is still owed.
// `CCombineMenu::performInteraction()`.
void consume_recipe(const Recipe& winner, Inventory& slots, const CombineContext& context,
                    CombineOutcome& outcome) {
    const UnitTypeHierarchy& types = *context.unit_types;
    for (const RecipeIngredient& ingredient : winner.ingredients) {
        int32_t owed = ingredient.count;
        if (ingredient.unit_type == unit_type::kNone) {
            const std::optional<int64_t> found = ingredient_guid(context, ingredient.unit);
            if (!found) continue;  // no such item: this ingredient takes nothing
            const int64_t guid = *found;
            for (int slot : kCombineSlots) {
                if (!(0 < owed) || !slots.occupied(static_cast<size_t>(slot))) continue;
                if (slots.slots()[static_cast<size_t>(slot)].item.unit_guid != guid) continue;
                consume_from(slots, slot, owed, context, outcome);
            }
        } else {
            for (int slot : kCombineSlots) {
                if (!(0 < owed) || !slots.occupied(static_cast<size_t>(slot))) continue;
                if (!type_ingredient_takes(slots.slots()[static_cast<size_t>(slot)].item,
                                           ingredient.unit_type, types)) {
                    continue;
                }
                consume_from(slots, slot, owed, context, outcome);
            }
        }
    }
}

/// A winner with a result: whatever is left in the four combine slots of
/// `slots` goes to `returns` (`pickupEquipment(item, true)`), or to the floor
/// when it will not take it; then the first result is made into the combine
/// slots, and the journal counts it.
// `CCombineMenu::performInteraction()`.
void make_result(const Recipe& winner, Inventory& slots, Inventory& returns,
                 const CombineContext& context, CombineOutcome& outcome) {
    const UnitTypeHierarchy& types = *context.unit_types;
    for (int slot : kCombineSlots) {
        if (!slots.occupied(static_cast<size_t>(slot))) continue;
        const InventorySlot held = slots.slots()[static_cast<size_t>(slot)];
        slots.remove_item(static_cast<size_t>(slot), held.count);
        if (returns.pickup(held.item, held.count, types) < 0) {
            outcome.dropped.push_back(held);
        }
    }

    // Only the first result is made.
    const RecipeResult& result = winner.results.front();
    if (result.spawn_class.empty()) {
        ItemStats made;
        if (context.create_unit &&
            context.create_unit(result.unit, context.player_level, false, made)) {
            if (slots.pickup_into_slot(made, 1, kCombineSlots[0])) {
                outcome.result_slots.push_back(kCombineSlots[0]);
            }
        }
    } else {
        const SpawnClass* spawn_class =
            context.spawn_class ? context.spawn_class(result.spawn_class) : nullptr;
        if (spawn_class == nullptr || context.game_rng == nullptr ||
            context.spawn_context == nullptr) {
            outcome.unknown_spawn_class = true;
            return;
        }
        std::vector<std::string> units;
        std::vector<bool> enchant;
        spawn_class->roll_spawn_class(units, &enchant, &context.player, &context.player,
                                      context.player_level, 0xffffffffu, 0, -1, 0, 0,
                                      *context.game_rng, *context.spawn_context);
        const size_t made_count = std::min<size_t>(units.size(), kCombineSlots.size());
        for (size_t i = 0; i < made_count; ++i) {
            // The flag list read as `TArrayList` reads: past the end, entry 0.
            bool force = false;
            if (!enchant.empty()) force = i < enchant.size() ? enchant[i] : enchant[0];
            ItemStats made;
            if (!context.create_unit ||
                !context.create_unit(units[i], context.player_level, force, made)) {
                continue;
            }
            if (slots.pickup_into_slot(made, 1, kCombineSlots[i])) {
                outcome.result_slots.push_back(kCombineSlots[i]);
            }
        }
    }
    outcome.journal_counted = true;
}

}  // namespace

std::vector<const Recipe*> matching_recipes(const Inventory& inventory,
                                            const CombineContext& context) {
    // (on the PC, `performInteraction`'s first loop).
    std::vector<const Recipe*> matches;
    if (context.recipes == nullptr || context.unit_types == nullptr) return matches;
    for (const Recipe& recipe : context.recipes->recipes()) {
        if (recipe_matches(recipe, inventory, context)) matches.push_back(&recipe);
    }
    return matches;
}

CombineOutcome perform_combine(Inventory& inventory, const CombineContext& context) {
    // `CCombineMenu::performInteraction()`.
    CombineOutcome outcome;
    const std::vector<const Recipe*> matches = matching_recipes(inventory, context);
    if (matches.empty()) {
        outcome.sample = kCombineFailSample;
        return outcome;
    }
    const Recipe* winner = pick_recipe(matches);
    consume_recipe(*winner, inventory, context, outcome);
    outcome.matched = true;
    outcome.sample = kCombineSuccessSample;
    outcome.owner_sample = true;
    if (winner->results.empty()) return outcome;
    // The PC's slots are the player's own, so the leftovers go back to the
    // inventory they are in.
    make_result(*winner, inventory, inventory, context, outcome);
    return outcome;
}

CombineOutcome transmute(Inventory& combiner, Inventory& player_bag,
                         const CombineContext& context) {
    // The gate is the function's count above 0; without a
    // match nothing happens, not even the fail sample the function ends with.
    CombineOutcome outcome;
    const std::vector<const Recipe*> matches = matching_recipes(combiner, context);
    if (matches.empty()) {
        outcome.sample = kCombineNoSample;
        return outcome;
    }
    const Recipe* winner = pick_recipe(matches);
    consume_recipe(*winner, combiner, context, outcome);
    // 0x24 on the interface bank at the player, and 0x3f on the player's own
    // bank (`+0x224`) 0.1 s on.
    outcome.matched = true;
    outcome.sample = kCombineSuccessSample;
    outcome.owner_sample = true;
    if (winner->results.empty()) return outcome;
    // The leftovers go to the player's bag, not back to the Transmuter; the
    // result is made in the Transmuter's slots.
    make_result(*winner, combiner, player_bag, context, outcome);
    return outcome;
}

}  // namespace tl::core
