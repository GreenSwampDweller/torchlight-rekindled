// What lies on the floor and what it looks like there: the name over an item (or a door, a
// chest, a lever - every `CTriggerUnit` is a `CItem`), its colour, when it shows, where it sits,
// and the particle a dropped piece of equipment carries. `CGameUI::updateIngameUI`,
// `CItem::setItemTextHighlighted`,
// `CLevel::findItemsOnscreen`.
//
// The highlighted label is enabled (`Window::setEnabled`), which draws the
// A button on it.
#pragma once

#include "formats/adm.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tl::core {

/// Which colour pair a label takes, in the order the Xbox tests for them.
enum class ItemLabelTint {
    Plain,          ///< white when highlighted, 0.8 grey when not
    Quest,          ///< `getIsQuestUnit`
    Set,            ///< a `CEquipment` whose `getSet` is not empty
    Unique,         ///< ISA `UNIQUE` (0x36)
    Rare,           ///< magical (or `RANDOMMAGIC SOCKETABLE`) and ISA `MAGIC` (0x37)
    RandomEnchant,  ///< magical (or `RANDOMMAGIC SOCKETABLE`), not `MAGIC`
};

/// What `setItemTextHighlighted` asks of an item to pick its tint. `set` is
/// only asked of a `CEquipment` - a `CItemGold` or a `CTriggerUnit` goes
/// through the base `CItem` version, which has no set test.
struct ItemLabelFacts {
    bool quest = false;    ///< ISA `QUESTITEM` (0x67), or a `QUEST_GUID` other than -1
    bool set = false;      ///< `SET` is not empty
    bool unique = false;   ///< ISA 0x36
    bool magical = false;  ///< `isMagical()` (vtable `+0x2b0` / X360 `+0x14c`)
    bool random_magic_socketable = false;  ///< ISA `RANDOMMAGIC SOCKETABLE` (0xa0)
    bool magic = false;                    ///< ISA `MAGIC` (0x37)
};

/// `CEquipment::setItemTextHighlighted`: quest, then set,
/// then unique, then the magical pair, else plain. The label's first colour at
/// creation takes the same order, unhighlighted.
ItemLabelTint item_label_tint(const ItemLabelFacts& facts);

/// A colour as CEGUI holds one, 0..1.
struct LabelColour {
    float a = 1.0f, r = 1.0f, g = 1.0f, b = 1.0f;
};

/// `CGameGlobals`' ten label colours (`reload`), each an
/// `AARRGGBB` string in `media/GLOBALS.DAT`. A missing key keeps the constructor's empty
/// string, which CEGUI reads as black; that is kept.
struct ItemLabelColours {
    LabelColour random_enchant, rare, unique, set, quest;
    LabelColour random_enchant_unselected, rare_unselected, unique_unselected, set_unselected,
        quest_unselected;
    /// `SOCKET_COLOR`, what a socketed gem's lines are printed in
    /// (`CEquipment::effectsDescription`).
    LabelColour socket;
};

/// A colour as the eight hex digits a `|c` markup run carries, `AARRGGBB`.
std::string label_colour_markup(const LabelColour& colour);

/// `reload`'s ten `*_COLOR` / `*_COLOR_UNSELECTED` keys and `SOCKET_COLOR`,
/// read from the root.
ItemLabelColours read_item_label_colours(const tl::formats::AdmFile& globals);

/// The pair `setItemTextHighlighted` sets `TextColour` from.
/// `Plain` is (1, 1, 1, 1) or (1, 0.8, 0.8, 0.8), written out in the code.
LabelColour item_label_colour(ItemLabelTint tint, bool highlighted,
                              const ItemLabelColours& colours);

/// An item on the floor within this squared distance of the player, across
/// all three axes, has its name up.
///
inline constexpr float kItemLabelNearSquared = 20.0f;

/// What asks of one on-screen item.
struct ItemLabelQuery {
    bool highlighted = false;       ///< the level's highlighted item (client `+0x50`)
    bool container = false;         ///< ISA `CONTAINER` (0x1e)
    bool breakable = false;         ///< ISA `BREAKABLE` (0x1d)
    float distance_squared = 0.0f;  ///< from the player, x, y and z
    /// The PC's `KSETTINGS_KEYMAP_SHOWITEMS` key held. A pad has no such key.
    bool show_items_held = false;
    /// `KSETTINGS_TOGGLE_ITEM_NAME`, the setting `TOGGLE DROPPED ITEMS`
    /// (`CSettings::CSettings`, default 0).
    int toggle_dropped_items = 0;
};

/// Whether the item's label is up this frame: the highlighted item always,
/// unless it is a container; any other when the show-items key and the toggle
/// disagree, or when it is near and not breakable.
bool item_label_wanted(const ItemLabelQuery& query);

/// `getItemName()` with its first `{...}` span removed when it holds at least one character,
/// as the label is built.
std::string item_label_text(const std::string& name);

/// The label's padding: 4 pixels over the text,
/// plus `36 x ratio - 36` from a UI setting (over the constant).
///
inline constexpr float kItemLabelPadding = 4.0f;
inline constexpr float kItemLabelScaleBase = 36.0f;
/// How far from the right and bottom edges a label is kept.
inline constexpr float kItemLabelEdge = 26.0f;
/// The world offset the label is anchored at: the camera's up vector, one unit
/// (added in the projection).
inline constexpr float kItemLabelLift = 1.0f;

/// A label's rectangle on screen, in pixels.
struct ItemLabelRect {
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
};

/// The window's size: the text's extent and the font's line height, each
/// padded. `ratio` is the setting multiplies by.
ItemLabelRect item_label_size(float text_width, float line_height, float ratio);

/// Where a label of `size` goes when its anchor projects to (`at_x`, `at_y`):
/// centred on it, then kept `kItemLabelEdge` inside the right and bottom and at
/// or past 0 at the left and top.
ItemLabelRect place_item_label(const ItemLabelRect& size, float at_x, float at_y,
                               float screen_width, float screen_height);

/// How far a label's drawn frame reaches past its rectangle: left and top are
/// where the frame starts (negative: before the rectangle), wide and tall what
/// it adds to the size.
struct ItemLabelFrame {
    float left = 0.0f, top = 0.0f, wide = 0.0f, tall = 0.0f;
};

/// DIVERGENCE (the Stacked Labels mod): labels placed by place_item_label moved
/// up or down so no two frames overlap, as an ARPG's loot labels stack. In the
/// given order, each keeps its own spot when that is free; otherwise it takes the
/// free spot nearest to it, just above or below a frame in the way, `gap` apart.
/// A spot past the top or the bottom edge (`kItemLabelEdge`) is not taken; with
/// none left, the label stays where it was. Only `y` changes.
void stack_item_labels(std::vector<ItemLabelRect>& rects, const ItemLabelFrame& frame, float gap,
                       float screen_height);

/// `CEquipment::createParticles`: the
/// particle a piece of equipment carries while it lies in the level. Its own
/// `DROPPARTICLE` when it authors one; else `QUEST_ITEM` for a quest unit,
/// `UNIQUE_WEAPON` for a unique, `MAGIC_WEAPON` when it is magical and
/// `GENERIC_WEAPON` for the rest - names in `media/particles/PRELOAD.DAT`.
std::string drop_particle_name(const std::string& authored, bool quest, bool unique, bool magical);

/// The damage types `getDamageBonus` is asked about, in `gDAMAGE_TYPES` order.
inline constexpr int kDamageFire = 2;
inline constexpr int kDamageIce = 3;
inline constexpr int kDamageElectric = 4;
inline constexpr int kDamagePoison = 5;

/// The second half of `createParticles`, for a weapon (ISA 8): the element
/// with the largest positive `getDamageBonus` - electricity asked first, then
/// fire, ice and poison, a later one winning only when strictly larger - picks
/// `WEAPON_<ELEMENT>`, or `WEAPON_<ELEMENT>_PISTOL` for ISA `PISTOL` (0x5a).
/// Empty when no bonus is positive. `bonus(type)` is `getDamageBonus(type)`.
template <typename Bonus>
std::string elemental_particle_name(bool pistol, Bonus bonus);

std::string elemental_particle_for(int element, bool pistol);

template <typename Bonus>
std::string elemental_particle_name(bool pistol, Bonus bonus) {
    //
    int best = 0;
    int element = 0;
    const int electric = bonus(kDamageElectric);
    if (0 < electric) {
        best = electric;
        element = kDamageElectric;
    }
    const int fire = bonus(kDamageFire);
    if (best < fire) {
        element = kDamageFire;
        best = fire;
    }
    const int ice = bonus(kDamageIce);
    if (best < ice) {
        element = kDamageIce;
        best = ice;
    }
    const int poison = bonus(kDamagePoison);
    if (best < poison) element = kDamagePoison;
    return elemental_particle_for(element, pistol);
}

}  // namespace tl::core
