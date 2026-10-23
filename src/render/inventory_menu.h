// The in-game Menu system, drawn from the Xbox 360 release's
// `media/UI/mastermenu_xenon.layout`, `media/UI/charctermenu_xenon.layout`,
// `media/UI/inventorymenu_xenon.layout`, `media/UI/statsmenu_xenon.layout`,
// `media/UI/journalmenu_xenon.layout`, `media/UI/questmenu_xenon.layout`, and
// for the skills tab `media/UI/skillsmenu_xenon.layout` and
// `media/UI/skillmap_xenon.layout` (`inventory_menu_skills.cpp`).
#pragma once

#include "core/character.h"
#include "core/effects.h"
#include "core/equipment_tooltip.h"
#include "core/inventory.h"
#include "core/item_roll.h"
#include "core/journal.h"
#include "core/pet.h"
#include "core/pet_commands.h"
#include "core/quest_book.h"
#include "core/skills.h"
#include "core/spell_remove.h"
#include "core/stats_page.h"
#include "core/tray_press.h"
#include "core/tray_row.h"
#include "core/unit_types.h"
#include "formats/cegui.h"
#include "formats/uilayout.h"

#include <OgreColourValue.h>
#include <OgreMaterial.h>
#include <OgrePrerequisites.h>
#include <OgreTexture.h>

#include <array>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Ogre {
class Overlay;
class OverlayContainer;
class PanelOverlayElement;
class TextAreaOverlayElement;
}  // namespace Ogre

namespace tl::render {

/// Which of the master menu's tabs is open, numbered as `tl::core::HousingTab`
/// (`core/inventory_screen.h`), which steps it. The ring is `Skills -> Quest -> Inventory ->
/// Stats -> Skills`; `CMasterHousingXenon`'s ring step wraps at both ends.
enum class MenuMode {
    Quest = 0,
    Inventory = 1,
    Stats = 2,
    Skills = 3,
};

/// The screen an interactive NPC opens: `CCharacter::approachAI`
/// sets an AI state by the unit's type, and `CGameUI::updateIngameUI`
/// opens that state's menu - merchant 0x13 (a `Gambler` too), combiner 0x11,
/// stash 0x24, shared stash 0x25, and `CEnchantMenu::setOpen(bool, EAIState)` for 0x15
/// (enchant), 0x19 (`SocketableRecoverer`), 0x1a (`SocketableDestroyer`) and 0x1b (retirement).
///
/// The vendor's column takes the character sheet's place: `inventorymenu_xenon.layout` with
/// `backroundbase_merchant`, the `merchant_tab_*` tabs and `traycolor_purple`; the enchanter
/// family's is the matching pane of `enchantermenu_xenon.layout`.
enum class VendorScreen {
    None,
    Merchant,
    Stash,
    SharedStash,
    Combine,
    Enchant,
    DestroyGems,
    RecoverGems,
    Retire,
    /// The `ENCHANTSHRINE` prop's page: `CEnchantMenuXenon` in mode 1, with no
    /// NPC (`vfunc_6`) and the shrine at `+0xf4`.
    EnchantShrine,
};

class InventoryMenu {
public:
    InventoryMenu() = default;
    ~InventoryMenu();

    InventoryMenu(const InventoryMenu&) = delete;
    InventoryMenu& operator=(const InventoryMenu&) = delete;

    /// Build against the mounted archive and extra data.
    bool create(const std::string& group, unsigned width, unsigned height);
    void destroy();
    bool ready() const { return built_; }

    void show();
    void hide();
    void toggle();
    bool visible() const { return visible_; }

    void resize(unsigned width, unsigned height);

    /// Update displayed items, counts, gold, the character sheet and the journal. `sheet` carries
    /// the four maxima (`CharacterProgression::display_minimum()` derives the low ends). `pet`
    /// drives the third column: its bag, or the "gone to town" panel. `quests` may be null.
    /// `effects` reads the four attributes (`CCharacter::strength()` & co.). `play_time` is the
    /// player's clock (`CPlayer +0x388`).
    void update(const tl::core::Inventory& inv, const tl::core::CharacterProgression& character,
                tl::core::CharacterEffects effects, const tl::core::CombatSheet& sheet,
                const tl::core::StatBook& graphs, const tl::core::Pet& pet, float play_time,
                const tl::core::QuestLog* quests = nullptr);

    /// Open on an NPC's screen. The menu is the inventory tab with the vendor's
    /// column in place of the character sheet; closing the menu ends it. A
    /// merchant's, the Transmuter's and a stash's columns list
    /// `set_vendor_stock`'s inventory; the enchanter's is its pane
    /// (`update_enchant_pane`), over the bag and the pet's trays only.
    void open_vendor(VendorScreen screen);
    VendorScreen vendor() const { return vendor_; }

    /// The trays of a vendor's screen, left to right as
    /// `CMerchantMenuXenon::vfunc_7` places them: the
    /// vendor's at -1, the bag at 300, the pet's at 603.
    enum class Tray { Vendor, Bag, Pet };

    /// What the merchant's (or the Transmuter's) tray lists (not owned; null
    /// for none) and whether the merchant is a gambler. Call before `open_vendor`, which puts the
    /// tray on the first tab listing anything.
    void set_vendor_stock(const tl::core::Inventory* stock, bool gambler);
    /// The player's gold and effect 0x53, which the price captions are
    /// worked out and tinted. Null bonus is none.
    void set_trade_figures(int gold, const float* price_bonus);

    /// The `GLOBALS.DAT` odds the enchanter's pane shows.
    /// Not owned; null leaves the pane's figures empty.
    void set_enchant_rules(const tl::core::ItemRollGlobals* globals) { enchant_rules_ = globals; }
    /// The last enchant's line in `resultofenchant`, its
    /// `TextColour` `FFFFFF00` when the service `succeeded` and `FFFF0000`
    /// when not. It stays until the selected item changes (`vfunc_9`
    /// refreshes the pane, empties it).
    void set_enchant_result(const std::string& text, bool succeeded);
    /// Whether the screen is one of `CEnchantMenuXenon`'s: the enchanter, the
    /// gem services or the retirement.
    bool enchanter_screen() const {
        return vendor_ == VendorScreen::Enchant || vendor_ == VendorScreen::DestroyGems ||
               vendor_ == VendorScreen::RecoverGems || vendor_ == VendorScreen::Retire ||
               vendor_ == VendorScreen::EnchantShrine;
    }
    /// `CEnchantMenuXenon +0xf4`'s `+0x19c`: the shrine page's shrine still
    /// usable (`CTriggerUnit +0x1f0` on Linux). True on an enchanter's page,
    /// where `+0xf4` is null. False hides "Enchant" from the legend
    /// and `pressbutton`.
    void set_enchant_allowed(bool allowed) { enchant_allowed_ = allowed; }
    /// The gem services' result line (writes window 7,
    /// `destroysocketresult`; window 6, `getsocketresult`), for the
    /// page that is open. Nothing else writes those two windows: the refresh
    /// leaves them, so a line lasts until the next A on its page,
    /// across closing and opening the page again. A refusal writes "".
    void set_gem_result(const std::string& text);

    /// The item-select dialog (`CMessageItemSelectXenon`, `CGameUI +0x1894`),
    /// opened by A on a gem: `messagehousing_xenon.layout`
    /// over the screen, with one more copy of the bag's tray in it at (0, 0)
    /// (`vfunc_4`, `+0x180` = 2, `+0x184` set) listing every item of
    /// the gem's owner that takes a gem (`tl::core::item_takes_gem`),
    /// the close button centred under it (`vfunc_5`). False when the
    /// console data carries no housing layout.
    bool open_item_select();
    /// Closes it (`vfunc_3(0)`). The choice stays for `item_select_choice`.
    void close_item_select();
    bool item_select_open() const { return select_open_; }
    /// A in the dialog (`vfunc_2`, code 5): the selected row's slot
    /// becomes the choice (`+0x30`), -1 with nothing selected; then it closes.
    void confirm_item_select();
    /// The dialog's choice, -1 for none: what opening cleared (`vfunc_3(1)`),
    /// and what closing without A leaves.
    int item_select_choice() const { return select_choice_; }
    /// Up and down in the dialog (codes 0x11 and 0x12).
    void item_select_step(int direction);
    /// The stick's left and right in the dialog: code 0x13 moves the tab left,
    /// stopping at the first (and playing `NAVIGATE`), code 0x14 steps it right.
    /// The list itself is not filtered by the tab.
    void item_select_step_tab(int direction);

    /// The pet commands dialog (`CMessagePetCommandsXenon`, `CGameUI +0x189c`),
    /// opened by Start on the inventory tab (code 0xd):
    /// `messagehousing_xenon.layout` with `petcommands_xenon.layout` in its
    /// `parent` window (`vfunc_4`), the housing's own close button
    /// hidden. False when the console data lacks either layout.
    bool open_pet_commands();
    // PORT: the renderer's interface to the pet commands dialog.
    void close_pet_commands() { pet_commands_open_ = false; }
    // PORT: the renderer's interface to the pet commands dialog.
    bool pet_commands_open() const { return pet_commands_open_; }
    /// `vfunc_5`: the three stance images, `stancetext`, and
    /// whether the Y row is shown. Taken on the next `update`.
    void set_pet_commands_view(const tl::core::PetCommandsView& view) { pet_commands_view_ = view; }

    /// The replace-spell dialog (`CMessageSpellRemoveXenon`, `CGameUI +0x1898`),
    /// opened by A on a spell scroll for an owner whose slots are full:
    /// `messagehousing_xenon.layout` with
    /// `spellremove_xenon.layout` in its `parent` window (`vfunc_4`),
    /// the housing's close button hidden. False when the console data lacks
    /// either layout.
    bool open_spell_remove();
    void close_spell_remove() { spell_remove_open_ = false; }
    bool spell_remove_open() const { return spell_remove_open_; }
    /// `vfunc_5`: the four rows. Taken on the next
    /// `update`.
    void set_spell_remove_rows(const std::array<tl::core::SpellRemoveRow, 4>& rows) {
        spell_remove_rows_ = rows;
    }

    /// The tray X acts on. Outside a vendor's screen, the bag.
    Tray focused_tray() const { return vendor_ == VendorScreen::None ? Tray::Bag : focus_; }
    /// RB (+1) and LB (-1) on a vendor's screen: codes 10 and
    /// 12 -> (direction, false), the focus to the next tray, wrapping.
    void focus_next_tray(int direction);
    /// The left stick's left and right (codes 0x13 and 0x14): the focused
    /// tray's tab; past either end the focus moves on and the tray it lands
    /// on starts at the tab on that side.
    void step_tray_tab(int direction);
    void cycle_vendor_tab(int direction);
    /// The merchant tray's selected slot, -1 for none.
    int selected_vendor_slot() const;

    /// Top-level menu tab switching (Inventory <-> Stats)
    void next_menu_tab();
    void prev_menu_tab();
    void set_menu_mode(MenuMode mode);
    MenuMode menu_mode() const { return menu_mode_; }

    /// Navigation within inventory tab
    void select_next();
    void select_prev();
    void next_tab();
    void prev_tab();

    int current_tab() const { return current_tab_; }
    void set_current_tab(int tab);
    /// The bag's selection onto the item in `slot`, its tab with it
    /// (the item's tab, then the row); applied at the
    /// next `update`.
    void select_slot(size_t slot);
    int selected_slot_index() const;
    /// Select the visible inventory row under a screen-pixel tap. Returns
    /// true when a bag, pet, or vendor row consumed the tap.
    bool touch_select_at(float x, float y);

    /// The bag's screen with no vendor (`CInventoryMenuXenon`): the focus is on
    /// the bag or on the first pet's tray, and the left column follows it.
    /// RB/LB (codes 10, 12) swap it (with 0); stepping a tray's
    /// tab off either end swaps it too and the tray it lands on starts at the
    /// end it came in from (with -1: tab 5, with +1: tab 0). Nothing
    /// without a pet in the world to own the second tray.
    void swap_bag_focus(int landing);
    /// The stick's left and right on that screen (handler codes
    /// 0x13, 0x14): the focused tray's tab, and past either end the swap.
    void step_bag_tab(int direction);
    bool pet_tray_focused() const { return vendor_ == VendorScreen::None && focus_ == Tray::Pet; }
    /// The pet's tray's selected slot in the pet's bag, -1 for none.
    int selected_pet_slot() const;

    /// The Stats column's refresh (`CStatsXenon`) and its selection (`+0xa4`),
    /// written into the page's windows by `update`. The page itself is `tl::core::StatsPage`.
    void set_stats_view(const tl::core::StatsPageView& view, int selected);

    /// Which of the quest tab's six trays is lit, and the name of its quest (by name, so a
    /// changing log cannot leave a dangling pointer).
    int selected_quest() const { return selected_quest_; }
    void set_selected_quest(int index);
    // PORT: the renderer's interface to the journal.
    const std::string& selected_quest_name() const { return selected_quest_name_; }

    /// Whether `Y` would do anything on the quest tab: the highlighted quest
    /// exists and authors `CANABANDON`.
    // PORT: the renderer's interface to the journal.
    bool selected_quest_can_be_abandoned() const { return selected_quest_abandonable_; }

    /// `CQuestMenuXenon`'s refresh (vtable +0x20): while the
    /// tab is open, its rows are built again from the log at the next update.
    /// Opening the tab builds them too; between the two they stay as built.
    void refresh_quest_tab() { quest_rows_stale_ = true; }

    // ---- The skills tab (`CSkillMenuXenon`, `inventory_menu_skills.cpp`) ----

    /// Whether `skillsmenu_xenon.layout` was there to build the tab from. The
    /// ring skips nothing either way; without it the tab is empty.
    bool has_skills_tab() const { return have_skills_; }

    /// The four cursor moves,
    /// skipping cells no skill sits in. They do nothing in map mode.
    void skill_cursor_up(const tl::core::SkillBook& book);
    void skill_cursor_down(const tl::core::SkillBook& book);
    void skill_cursor_left(const tl::core::SkillBook& book);
    void skill_cursor_right(const tl::core::SkillBook& book);
    // PORT: the renderer's interface to the skills tab.
    const tl::core::SkillCursor& skill_cursor() const { return skill_cursor_; }
    /// Put the cursor on a cell outright. Only the headless dumps use it:
    /// on the console the four moves above are the only way there.
    void set_skill_cursor(const tl::core::SkillCursor& cursor) { skill_cursor_ = cursor; }

    /// The skill under the cursor, or null: an empty spell
    /// slot, or a class with nothing at that cell.
    const tl::core::KnownSkill* selected_skill(const tl::core::SkillBook& book) const;
    tl::core::KnownSkill* selected_skill(tl::core::SkillBook& book) const;

    /// The map-to-button sheet (`skillmap_xenon.layout`).
    bool skill_map_open() const { return skill_map_open_; }
    void set_skill_map_open(bool open);

    /// The unit type hierarchy the skill descriptions name weapons from
    /// (`getWeaponString`). Not owned; may be null.
    void set_unit_types(const tl::core::UnitTypeHierarchy* types) { unit_types_ = types; }
    /// What the A legend asks of a tray's owner about an
    /// item: the learn test, `canUse`, the Identify
    /// Scrolls to hand and whether the owner is away. `pet_tray` is the pet's
    /// tray, whose owner is the pet; the bag's is the player. Unset, every
    /// answer is no.
    using TrayCaptionTests =
        std::function<tl::core::TrayCaptionContext(const tl::core::ItemStats&, bool pet_tray)>;
    void set_tray_caption_tests(TrayCaptionTests tests) { tray_caption_tests_ = std::move(tests); }
    /// What every tray's row fill (`core/tray_row.h`) asks of
    /// the player: its `canEquip`, the figures the foot line tests and the
    /// twelve it compares with. The gambler flag is the menu's own. What it
    /// points at is not owned.
    void set_tray_row_context(const tl::core::TrayRowContext& context) { row_context_ = context; }

    /// The item details windows (`CGameUI`'s three `CEquipmentTooltip`s): the
    /// selected item's, then up to two worn items it is compared with. Empty
    /// hides them all.
    void set_item_tooltips(std::vector<tl::core::EquipmentTooltip> tips);
    /// `CGameUI +0x14c4`, the item compare switch: the footer's caption
    /// reads "disable item compare" while it is on.
    void set_item_compare(bool on) { item_compare_ = on; }
    /// The vendor tray's inventory and whether its owner is a gambler.
    // PORT: the renderer's interface to the vendor tray.
    const tl::core::Inventory* vendor_stock() const { return vendor_stock_; }
    // PORT: the renderer's interface to the vendor tray.
    bool vendor_gambler() const { return vendor_gambler_; }

    /// What the skill stat lines and the stat-bonus row need: the effect-type
    /// table, the stat curves, the owner's four stats and the affix lookup.
    /// Not owned; may be null, in which case no stat line is printed.
    // PORT: the renderer's interface to the skills tab.
    void set_skill_effects(const tl::core::EffectDisplayContext* effects) {
        skill_effects_ = effects;
    }

    /// Fill the tab from the book. Called from `update()` while the tab is
    /// open, and by the renderer after anything on it changes.
    void update_skills_tab(const tl::core::SkillBook& book,
                           const tl::core::CharacterProgression& character);

private:
    struct Node {
        float off_x = 0.0f, off_y = 0.0f;
        float wide = 0.0f, tall = 0.0f;
        float scale_wide = 0.0f, scale_tall = 0.0f;
        int h_anchor = 0, v_anchor = 0;
        int parent = -1;

        Ogre::PanelOverlayElement* panel = nullptr;
        Ogre::TextAreaOverlayElement* text = nullptr;
        Ogre::TextAreaOverlayElement* drop = nullptr;

        std::string image;
        /// A text node's authored colour, which an alpha scales.
        Ogre::ColourValue colour = Ogre::ColourValue::White;
        /// The window's own `ALPHA` (clamped to 0..1), and that times its parent's: CEGUI's
        /// effective alpha, which a window inherits by default.
        float own_alpha = 1.0f;
        float alpha = 1.0f;
        float points = 0.0f;
        int h_text = 0;
        int v_text = 0;
        std::string widget;
        std::string prefix;
        bool hidden = false;
        bool runtime_hidden = false;
        bool always_on_top = false;

        /// In `top_overlay_` / `top_text_overlay_` rather than the main pair.
        bool floating = false;

        float left = 0.0f, top = 0.0f, width = 0.0f, height = 0.0f;
    };

    struct Sprite {
        tl::formats::ImagesetImage rect;
        Ogre::TexturePtr texture;
        Ogre::MaterialPtr material;
    };

    bool load_art(const std::string& group);
    void build(const tl::formats::UiObject& object, int parent, const std::string& prefix,
               size_t& serial, bool inherited_hidden, bool inherited_always_on_top,
               float extra_off_x = 0.0f, float extra_off_y = 0.0f, bool inherited_floating = false);
    void layout();

    /// Fill the quest tab: the six trays, the description pane and the reward
    /// row. Called from update() when the Quest tab is open.
    void update_quest_tab(const tl::core::QuestLog* quests);

    /// Build the skills tab's two layouts into the container (`create()`).
    void build_skills_tab(const std::string& group, int container_idx, size_t& serial);
    /// The selected skill into the description column.
    void describe_skill(const tl::core::KnownSkill& skill, const tl::core::SkillBook& book,
                        const tl::core::CharacterProgression& character);
    /// The legend beside the cursor.
    void update_skill_legend(const tl::core::KnownSkill* skill,
                             const tl::core::CharacterProgression& character);
    /// A panel drawn at `alpha` (a CEGUI window's `Alpha`), from a variant of
    /// its sprite's material.
    void set_widget_alpha(const std::string& widget_name, float alpha);
    /// The material `sprite` draws with at `alpha`: its own at 1, else a faded variant.
    std::string material_for(const Sprite& sprite, float alpha);
    /// A node's effective alpha; 1 for no node (a root's parent).
    float alpha_of(int node) const;
    /// Lay a run-coloured string over a text node, wrapped to its frame and
    /// clipped to it, from `pool` (grown on demand).
    void draw_rich_text(const std::string& text_widget, const std::string& frame_widget,
                        const std::string& text, std::vector<Ogre::TextAreaOverlayElement*>& pool);
    void lay_out_rich_texts();

    bool is_node_hidden(int idx) const;
    void set_uv(Node& node, float u0, float v0, float u1, float v1) const;
    const Sprite* sprite(const std::string& name) const;

    Node* find_node_by_widget(const std::string& widget_name);
    int find_node_index_by_widget(const std::string& widget_name) const;
    void set_widget_text(const std::string& widget_name, const std::string& text);
    void set_widget_image(const std::string& widget_name, const std::string& image_name);
    void set_widget_visible(const std::string& widget_name, bool visible);
    /// Each direct child of the widget hidden, or back to its authored
    /// `VISIBLE`.
    void set_children_visible(const std::string& widget_name, bool visible);
    /// Show or hide the overlays of `target_indices` and everything under
    /// them by their `runtime_hidden` flags.
    void refresh_node_visibility(const std::vector<int>& target_indices);
    void set_widget_position(const std::string& widget_name, float off_x, float off_y);

    std::vector<tl::formats::Imageset> sets_;
    std::vector<Ogre::TexturePtr> textures_;
    std::vector<Ogre::MaterialPtr> materials_;
    std::map<std::string, Sprite> sprites_;

    std::vector<Node> nodes_;
    Ogre::Overlay* overlay_ = nullptr;
    Ogre::Overlay* text_overlay_ = nullptr;
    Ogre::OverlayContainer* panel_root_ = nullptr;
    Ogre::OverlayContainer* text_root_ = nullptr;

    /// A second pair of overlays above the first, for windows that float over the screen: the four
    /// stat tooltips and the add/remove-point panel. Panels and text otherwise live in separate
    /// overlays (z 550 and 560), so a panel could never cover a caption.
    Ogre::Overlay* top_overlay_ = nullptr;
    Ogre::Overlay* top_text_overlay_ = nullptr;
    Ogre::OverlayContainer* top_panel_root_ = nullptr;
    Ogre::OverlayContainer* top_text_root_ = nullptr;

    unsigned width_ = 0;
    unsigned height_ = 0;
    float scale_ = 1.0f;
    bool built_ = false;
    bool visible_ = false;

    MenuMode menu_mode_ = MenuMode::Inventory;
    VendorScreen vendor_ = VendorScreen::None;
    /// Whether `enchantermenu_xenon.layout` was there to build the enchanter
    /// family's panes from.
    bool have_enchanter_ = false;

    /// Fill the vendor's column (`vendor/` or `enchant/`) for `vendor_`.
    void update_vendor_column(const tl::core::StatBook& graphs);
    /// The enchanter's pane for the bag's selected item.
    void update_enchant_pane(const tl::core::Inventory& inv, const tl::core::StatBook& graphs);

    /// the first row shown. Each list keeps its own per tab
    /// (`+0xf4 + 4*tab`) and moves it only when the selection (`+0xdc +
    /// 4*tab`, clamped to the last row) leaves the five rows shown: up to it,
    /// or down until it is the last. A list that fits shows from row 0.
    int tray_scroll(const std::string& prefix, int tab, int selected, size_t row_count);
    std::map<std::string, int> tray_scrolls_;
    /// One tray's five rows (`<prefix>ItemTray0..4` and what is in them) from
    /// `rows` of `inv`, from row `scroll` (`tray_scroll`), with the price
    /// caption gives each: a merchant's buy price (tinted
    /// when the player is short of it) or anything else's sell price.
    void fill_tray_rows(const std::string& prefix, const tl::core::Inventory& inv,
                        const std::vector<size_t>& rows, int selected, int scroll,
                        bool merchant_owned, size_t framed_rows = 0);
    /// Place `<prefix>moveframe` over the selected row, or hide it.
    void place_tray_frame(const std::string& prefix, int selected, int scroll, size_t row_count,
                          bool shown);
    /// A tray's legend on a vendor's screen (`(tray, A, X,
    /// Y)`): each button shown only with a caption, `buttonB`
    /// ("Info") hidden. INVENTED: the buttons stay where the layout puts them;
    /// centres the row by the captions' widths.
    void set_tray_legend(const std::string& prefix, const std::string& a, const std::string& x,
                         const std::string& y);
    /// `text` broken into lines at the widget's own measured width, for a
    /// window that authors `WORD WRAP`.
    std::string wrap_to_widget(const std::string& widget_name, const std::string& text);
    /// A text node's colour, over what the layout authored.
    void set_widget_text_colour(const std::string& widget_name, const Ogre::ColourValue& colour);

    // Vendor screen state.
    Tray focus_ = Tray::Bag;
    const tl::core::Inventory* vendor_stock_ = nullptr;
    bool vendor_gambler_ = false;
    int vendor_tab_ = 0;
    /// The tray has just opened: its tab is chosen at the next fill.
    bool vendor_tab_pending_ = false;
    int vendor_selected_ = 0;
    std::vector<size_t> vendor_rows_;
    int trade_gold_ = 0;
    bool have_price_bonus_ = false;
    float price_bonus_ = 0.0f;

    // The enchanter's pane.
    const tl::core::ItemRollGlobals* enchant_rules_ = nullptr;
    /// `priceofenchantment`'s authored colour, which a red price replaces.
    Ogre::ColourValue enchant_price_colour_ = Ogre::ColourValue::White;
    std::string enchant_result_;
    bool enchant_result_ok_ = true;
    bool enchant_allowed_ = true;
    /// The item the pane was last refreshed for (`CEnchantMenuXenon +0x124`),
    /// by bag slot; -2 before the first refresh.
    int enchant_pane_slot_ = -2;
    /// Windows 7 (`destroysocketresult`, mode 2) and 6 (`getsocketresult`,
    /// mode 3).
    std::string recover_gems_result_;
    std::string destroy_gems_result_;

    // The item-select dialog.
    bool have_housing_ = false;
    bool select_open_ = false;
    int select_choice_ = -1;
    int select_selected_ = 0;
    int select_tab_ = 0;
    std::vector<size_t> select_rows_;

    // The pet's tray on the bag's screen: its tab (`+0x128`), its selected row,
    // and the slots of the pet's bag the tab lists. `pet_tray_available_` is
    // whether there was a pet in the world at the last `update`.
    int pet_tab_ = 0;
    int pet_selected_ = 0;
    std::vector<size_t> pet_rows_;
    bool pet_tray_available_ = false;
    /// The pet's page (`petmenu_xenon.layout`, `petpage/`), and its two spell
    /// rows' cache (X360 panel `+0x90`, -1 until a slot has been seen).
    bool have_pet_page_ = false;
    std::array<int64_t, 2> pet_page_spells_{-1, -1};
    void update_pet_page(const tl::core::Pet& pet);

    // The pet commands dialog.
    bool have_pet_commands_ = false;
    bool pet_commands_open_ = false;
    tl::core::PetCommandsView pet_commands_view_;
    // The replace-spell dialog.
    bool have_spell_remove_ = false;
    bool spell_remove_open_ = false;
    std::array<tl::core::SpellRemoveRow, 4> spell_remove_rows_{};
    void update_spell_remove();
    /// A row's two socket windows.
    void draw_row_sockets(const std::string& prefix, const std::string& s_tray,
                          const tl::core::ItemStats& item);
    /// Fill the dialog's tray and place its close button.
    void update_item_select(const tl::core::Inventory& inv, const tl::core::StatBook& graphs);
    void update_pet_commands();
    /// The gem services' pane ('s tail for modes 2 and 3).
    void update_gem_pane(const tl::core::Inventory& inv);

    // Inventory tab state
    /// 0: Weapons, 1: Armor, 2: Jewelry, 3: Potions, 4: Spells, 5: Fish, in the layout's order.
    /// `next_tab()` and `prev_tab()` wrap, though the bumper hints are hidden at each end.
    int current_tab_ = 0;
    int selected_index_ = 0;  ///< Selected item index in filtered view
    int pending_select_slot_ = -1;
    std::vector<size_t> filtered_indices_;  ///< Mapping from display row to Inventory slot index

    // Stats tab state: the last refresh, written by every `update` on the tab.
    tl::core::StatsPageView stats_view_;
    int stats_selected_ = 0;
    /// `pieceToMove`'s place less `strength`'s, as the
    /// layout authors them; puts the frame there on the selection.
    float stat_frame_dx_ = 0.0f, stat_frame_dy_ = 0.0f;
    void show_stats_view();

    /// Quest tab state. `questmenu_xenon.layout` authors six trays, `ItemTray0`
    /// to `ItemTray5`, which is exactly how many journal-visible quests the
    /// player may carry - see `tl::core::kQuestLogSize`.
    int selected_quest_ = 0;
    /// The trays' quests by name, in the log's order, as the last build of
    /// the rows found them; the live entries are looked up
    /// by name.
    std::vector<std::string> quest_rows_;
    bool quest_rows_stale_ = true;
    bool quest_tab_was_open_ = false;
    std::string selected_quest_name_;
    bool selected_quest_abandonable_ = false;

    /// The journal page's rows, inside `journalwindow`: the
    /// label and the value, each with its look's drop copy under it.
    struct JournalRow {
        Ogre::TextAreaOverlayElement* label_drop = nullptr;
        Ogre::TextAreaOverlayElement* label = nullptr;
        Ogre::TextAreaOverlayElement* value_drop = nullptr;
        Ogre::TextAreaOverlayElement* value = nullptr;
    };
    std::vector<JournalRow> journal_rows_;
    /// Set once the Stats page has filled the journal since it opened.
    bool journal_filled_ = false;
    void hide_journal_rows();
    /// The rows' captions from `tl::core::journal_page_rows`.
    void fill_journal(const tl::core::JournalStatistics& store, float play_time);

    // Skills tab state.
    bool have_skills_ = false;
    int container_idx_ = -1;
    const tl::core::UnitTypeHierarchy* unit_types_ = nullptr;
    TrayCaptionTests tray_caption_tests_;
    tl::core::TrayRowContext row_context_;
    bool item_compare_ = false;
    /// One details window: the nine pieces of its frame and its lines.
    struct TipWindow {
        std::array<Ogre::PanelOverlayElement*, 9> frame{};
        std::vector<Ogre::TextAreaOverlayElement*> lines;
    };
    std::array<TipWindow, 3> tip_windows_;
    std::vector<tl::core::EquipmentTooltip> tooltips_;
    /// The focused tray's selected row (`<prefix>ItemTray<n>`), whose left and
    /// top the windows are placed; null for none.
    const Node* tooltip_anchor_row();
    void lay_out_tooltips();
    void destroy_tooltips();
    /// `tl::core::tray_a_caption` for the selected item of a tray.
    std::string tray_caption(const tl::core::ItemStats& item, size_t slot, bool pet_tray) const;
    tl::core::UnitTypeHierarchy empty_types_;
    const tl::core::EffectDisplayContext* skill_effects_ = nullptr;
    tl::core::EffectDisplayContext empty_effects_;
    tl::core::SkillCursor skill_cursor_;
    bool skill_map_open_ = false;
    std::map<std::string, Ogre::MaterialPtr> alpha_materials_;
    /// The three rich texts of the description column and their pools; kept so
    /// `layout()` can place them again after a resize.
    struct RichText {
        std::string text_widget;
        std::string frame_widget;
        std::string text;
        std::vector<Ogre::TextAreaOverlayElement*>* pool = nullptr;
    };
    std::vector<RichText> rich_texts_;
    std::vector<Ogre::TextAreaOverlayElement*> skill_name_pool_;
    std::vector<Ogre::TextAreaOverlayElement*> skill_desc_pool_;
    std::vector<Ogre::TextAreaOverlayElement*> skill_next_pool_;
    size_t rich_serial_ = 0;
};

}  // namespace tl::render
