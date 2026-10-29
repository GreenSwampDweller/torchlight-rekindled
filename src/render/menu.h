// The main menu, drawn from the game's own interface data: `media/UI/mainmenuframe.layout` (the
// bars, logo, button row and copyright line) with `media/UI/logo.imageset` and
// `media/UI/hud.imageset` for where its images live, or the console's `mainmenu.uilayout`. It
// draws over the backdrop level the renderer has up (`MAINMENU_TOWN.LAYOUT`).
#pragma once

#include "core/difficulty_menu.h"
#include "core/input.h"
#include "core/new_game_menu.h"
#include "core/randomizer.h"
#include "core/units.h"
#include "formats/cegui.h"
#include "render/renderer.h"

#include <OgrePrerequisites.h>

#include <OgreColourValue.h>
#include <OgreMaterial.h>
#include <OgreTexture.h>

#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Ogre {
class Overlay;
class OverlayContainer;
class OverlaySystem;
class PanelOverlayElement;
class SceneManager;
class TextAreaOverlayElement;
}  // namespace Ogre

namespace tl::render {

// `MenuAction` is declared in render/renderer.h, so that main.cpp can act on
// one without including an OGRE header.

/// One of the console interface's imagesets, with what it takes to draw from
/// it: the front end's screens each need three of these (`widgets`, `widgets1`
/// and `widgets2` under `media/UI/icons2/`) beside the two the PC screens use.
struct MenuArtSet {
    tl::formats::Imageset set;
    Ogre::TexturePtr texture;
    Ogre::MaterialPtr material;
    /// The same art at half opacity, for a window whose `Alpha` is 0.5.
    Ogre::MaterialPtr dim_material;
    bool ok = false;
};

/// The loading screen, drawn from `media/UI/loading.layout`: a full-screen fill, the 1024x725
/// artwork centred, the menu's bars and a tip in the lower bar. A single painted frame: loading
/// a level is synchronous, so painting it once before the load leaves it up for the duration.
class LayoutScreen;

class LoadingScreen {
public:
    LoadingScreen() = default;
    ~LoadingScreen();

    LoadingScreen(const LoadingScreen&) = delete;
    LoadingScreen& operator=(const LoadingScreen&) = delete;

    /// Build it, if it is not built, and show it at this size. False when the
    /// interface data cannot be read, which is survivable - the screen stays
    /// black, as it did before.
    bool show(const std::string& group, unsigned width, unsigned height);

    /// Show it under the menus, without a tip: the mods page's backdrop. `hide()` puts it back
    /// over them.
    bool show_backdrop(const std::string& group, unsigned width, unsigned height);
    /// Show it again as it was before the last `hide()`: over the menus or behind them.
    bool show_as_before(const std::string& group, unsigned width, unsigned height) {
        return hidden_backdrop_ ? show_backdrop(group, width, height) : show(group, width, height);
    }

    void hide();
    void destroy();
    bool visible() const { return visible_; }

private:
    /// Over the menu's three layers (400-402) and under the HUD; the bars one above.
    static constexpr unsigned short kZOrder = 450;
    /// Under the main menu's frame.
    static constexpr unsigned short kBackdropZOrder = 390;
    /// How bright the artwork is behind the menus.
    static inline const Ogre::ColourValue kBackdropDim{0.5f, 0.5f, 0.5f};
    void set_z_order(unsigned short z_order);
    void dim_pc_artwork(bool dim);

    /// Read the 21 authored tips, once. Empty when the archive has none, which
    /// leaves the bottom bar bare rather than failing.
    void load_tips(const std::string& group);

    /// Which tip this appearance shows, and put it in the bar: the globals'
    /// tip randomizer drawn on `g_Rand` (menu.cpp, `CGameUI::setLoadingVisible`).
    void choose_tip();

    /// The console's screen, when its art is mounted; false otherwise.
    bool build_console(const std::string& group);
    /// The PC screen's tip text window, over the bottom bar.
    void build_tip();

public:
    /// Show the first tip from now on, for a run whose frame is diffed
    /// (`Renderer::dump_loading_screen()`); the draw is still made.
    static void pin_tips();

private:
    /// The globals' tip randomizer, one choice per tip (menu.cpp).
    static tl::core::Randomizer& tip_choices();

private:
    /// The console's own screen, `media/UI/loading.uilayout`, when its art is
    /// mounted (the Xbox 360 pak, or a PC pak's `console_ui`); the PC screen
    /// below otherwise.
    std::unique_ptr<LayoutScreen> console_;
    Ogre::Overlay* overlay_ = nullptr;  ///< the fill and the artwork
    Ogre::Overlay* bars_ = nullptr;     ///< the framing bars, over them
    std::vector<Ogre::PanelOverlayElement*> panels_;
    Ogre::OverlayContainer* tip_box_ = nullptr;
    Ogre::TextAreaOverlayElement* tip_ = nullptr;
    std::vector<std::string> tips_;
    tl::formats::Imageset loading_set_;
    tl::formats::Imageset logo_set_;
    Ogre::TexturePtr loading_texture_;
    Ogre::TexturePtr logo_texture_;
    unsigned width_ = 0;
    unsigned height_ = 0;
    bool built_ = false;
    bool visible_ = false;
    bool backdrop_ = false;
    bool hidden_backdrop_ = false;  ///< `backdrop_` as the last `hide()` found it
    /// The panels `dim_pc_artwork` moved onto the dimmed material, with their own.
    std::vector<std::pair<Ogre::PanelOverlayElement*, Ogre::MaterialPtr>> undimmed_;
};

/// The character creation screen. With the Xbox data, the console's own
/// `media/UI/charactercreate.uilayout`, driven by `CNewGameMenu`'s pad logic
/// (`core/new_game_menu.h`) as a grid; without it, the PC's `media/UI/charactercreate.layout`
/// on the same pad. Class names, descriptions and meshes come from `media/units/players/`
/// (`tl::core::PlayerClass`).
class CharacterCreate {
public:
    CharacterCreate() = default;
    ~CharacterCreate();

    CharacterCreate(const CharacterCreate&) = delete;
    CharacterCreate& operator=(const CharacterCreate&) = delete;

    /// One choice on the screen: a class, its `NAME`, `DISPLAYNAME`, `DESCRIPTION`, the
    /// `logo.imageset` icon and, a mod's, its portrait.
    using Choice = tl::core::PlayerClass;

    /// Build it over whatever backdrop is up. False when the interface data
    /// cannot be read, which the caller treats as "start the game anyway".
    bool create(const std::string& group, unsigned width, unsigned height,
                std::vector<Choice> choices);

    void destroy();

    /// One frame of input. `NewGame` when Done (or OK) was pressed, `Quit` for
    /// Back.
    MenuAction update(const tl::core::InputState& input);

    /// Open on a named class rather than the layout's default. Ignored when
    /// the name is not one of the offered classes, so a misspelling leaves the
    /// screen as authored rather than blank.
    void select(const std::string& name);

    /// Which class is chosen, and which pet.
    const std::string& chosen() const;
    // PORT: the renderer's interface to the creation screen.
    const std::string& pet() const { return pet_; }

    /// The two names the console screen asks for. They open as the console's
    /// own defaults, "Player" and "Spot", and change only through the platform
    /// keyboard (`tl::core::text_entry`).
    // PORT: the renderer's interface to the creation screen.
    const std::string& player_name() const { return player_name_; }
    // PORT: the renderer's interface to the creation screen.
    const std::string& pet_name() const { return pet_name_; }

    /// Whether the chosen class changed since the last call, so the caller can
    /// swap the model standing on the backdrop.
    bool take_selection_changed();

    /// The same for the pet, which is a second model on the same backdrop.
    bool take_pet_changed();

    /// Choose a pet by its unit name: `Dog`, `Cat`, or - on the console's
    /// screen, which is the only one that offers it - `Chakawary`.
    void select_pet(const std::string& name);

private:
    struct Widget {
        Ogre::PanelOverlayElement* panel = nullptr;
        Ogre::TextAreaOverlayElement* label = nullptr;
        Ogre::TextAreaOverlayElement* shadow = nullptr;
        /// Where it sits, in authored pixels against 1024x768, and which edge
        /// each offset hangs from.
        float rel_x = 0.0f, off_x = 0.0f, rel_y = 0.0f, off_y = 0.0f;
        float wide = 0.0f, tall = 0.0f;
        /// The imageset rectangle to draw, by name. Empty for a text-only one.
        std::string normal, hover, selected;
        /// Which imageset it comes from, when it is not the PC screen's `logo`
        /// set - the console's portraits, pets and buttons are spread over the
        /// three `icons2` sets.
        const tl::formats::Imageset* set = nullptr;
        Ogre::TexturePtr texture;
        /// The console's art: which of the three `icons2` sets each of the two
        /// images comes from. The layout names the set with every image
        /// (`set:widgets1 image:gem_button_small_rollover`), and a button's two
        /// states are not always in the same one.
        MenuArtSet* normal_art = nullptr;
        MenuArtSet* hover_art = nullptr;
        /// The layout window this draws, for the focus and for visibility.
        std::string window;
        /// The name box whose focus this follows: `CNewGameMenu` sets `Alpha`
        /// 1.0 on a name box and on its frame when it is focused and 0.5 when
        /// it is not.
        std::string dims_with;
        /// One of the nine pieces of a `FrameComponent`, 0-8 in the order
        /// top-left, top-right, bottom-left, bottom-right, left, right, top,
        /// bottom, middle; -1 for anything else. `off_x`..`tall` are then the
        /// whole frame's area, and `corner_w`/`corner_h` its corner images'.
        int slice = -1;
        float corner_w = 0.0f, corner_h = 0.0f;
    };

    void layout();
    void apply_look();

    /// Read the console screen and its art. False without the Xbox data.
    bool load_console_screen(const std::string& group);
    /// The PC screen's imagesets, and their textures and materials.
    bool load_pc_imagesets(const std::string& group);
    bool load_pc_textures(const std::string& group);
    /// Build the console screen's panel, its captions and its prompt bar.
    void build_console_screen();
    /// Place all of that, in viewport pixels.
    void layout_console(float across, float down);
    /// One `StaticText` of the layout, at its authored rectangle within a
    /// parent at (`dx`, `dy`) that is `parent_wide` across (or the whole screen
    /// when that is negative), with the font, colour, alignment and drop
    /// shadow the layout and its look give it.
    void add_console_label(const char* window, float dx, float dy, float parent_wide);
    Ogre::TextAreaOverlayElement* console_label_text(const char* window);
    /// One `ImageButton` or `StaticImage` of the layout at its authored
    /// rectangle within a parent at (`dx`, `dy`), drawn with the images the
    /// layout names for it.
    void add_console_widget(const char* window, std::vector<Widget>& into, float dx, float dy);
    /// The nine-piece frame a window's look draws around it
    /// (`ceguiWidget/StaticShared` in the scheme's looknfeel).
    void add_console_frame(const char* window, bool item_frame, float dx, float dy,
                           const std::string& dims_with);
    /// The classes offered, and the focus grid with the mods' among them.
    void take_choices(const std::string& group, std::vector<Choice> choices);
    /// The class row: the console's three by their windows, then the mods'.
    void build_class_row();
    /// A mod's class on the console screen, from its own portrait art, in the class row.
    void add_mod_portrait(const Choice& choice);
    /// The class row's cells, the console's three first, sharing the three's width.
    void fit_class_row(size_t built_in);
    /// Move the focus a step on the grid, past what is hidden.
    void step_focus(int dx, int dy);
    /// A on a mod's class.
    void choose_mod_class(const std::string& name);
    MenuArtSet* art_named(const std::string& set);
    /// The set to draw `image` from: the one named when it has the image, else any loaded set
    /// that does (a look's fixed image the pack leaves to the PC's `GuiLook`). Null when none.
    MenuArtSet* art_for(const std::string& set, const std::string& image);
    /// The PC's `Logo` and `GuiLook` beside the console sheets, on a PC pak.
    bool load_pc_art(const std::string& group);
    /// Whether a window of the console screen is showing, for the focus.
    bool console_window_visible(const std::string& window) const;
    /// A on the focused window: its `onClick`, as `CNewGameMenu` answers it.
    MenuAction fire_console(const std::string& window);
    /// A frame's new presses on the console screen: a step of the focus, A, B.
    struct ConsolePress {
        int dx = 0, dy = 0;
        bool confirm = false, back = false;
    };
    MenuAction update_console(const ConsolePress& press);
    /// What the keyboard gave for a name, taken as `CNewGameMenu::update` takes it.
    void take_name(bool player, const std::string& text);
    /// A name box's caption: the name, or the text being typed into it and a caret.
    std::string shown_name(bool player) const;
    /// One frame of a name typed into its box (`tl::core::typing_in_window`): the keys,
    /// then Enter or A to keep it, Escape or B to leave the name as it was.
    void step_typing(bool confirm_edge, bool back_edge);

    Ogre::PanelOverlayElement* make_panel(const std::string& name, Ogre::Overlay* overlay,
                                          const Ogre::MaterialPtr& material);
    Ogre::TextAreaOverlayElement* make_text(const std::string& name, float points,
                                            const Ogre::ColourValue& colour, bool centred);
    void set_uv(Ogre::PanelOverlayElement* panel, const tl::formats::Imageset& set,
                const Ogre::TexturePtr& texture, const std::string& image) const;

    std::vector<Choice> choices_;
    int selected_ = 0;
    bool selection_changed_ = false;
    bool pet_changed_ = false;
    std::string pet_ = tl::core::kDefaultPet;
    std::string player_name_ = tl::core::kDefaultPlayerName;
    std::string pet_name_ = tl::core::kDefaultPetName;
    /// A name being typed into its box, the player's or the pet's, and the text so far.
    bool typing_ = false;
    bool typing_player_ = true;
    std::string typed_name_;

    tl::formats::Imageset logo_set_;
    tl::formats::Imageset gui_set_;
    Ogre::TexturePtr logo_texture_;
    Ogre::TexturePtr gui_texture_;
    Ogre::MaterialPtr logo_material_;
    Ogre::MaterialPtr gui_material_;

    /// **The console's own character creation**, `media/UI/charactercreate.uilayout`.
    /// Empty without the console data, and then the PC screen is drawn.
    tl::formats::GuiLayout console_;
    bool console_mode_ = false;

    /// Which window has the focus (`CNewGameMenu +0x20C`, `+0x210`).
    tl::core::NewGameFocus focus_;
    /// The grid it moves on: the console's, and the mods' classes on its top row.
    tl::core::NewGameGrid grid_;
    /// The mods' portraits' art; a deque, as each `Widget` points into it.
    std::deque<MenuArtSet> portrait_art_;
    std::string group_;

    MenuArtSet widgets_art_;
    MenuArtSet widgets1_art_;
    MenuArtSet widgets2_art_;
    /// `Logo` and `GuiLook` for the console screen on a PC pak: what the pack's sheets leave out.
    MenuArtSet logo_art_;
    MenuArtSet gui_art_;

    /// The screen's captions, each at its own authored rectangle.
    struct ConsoleLabel {
        Ogre::TextAreaOverlayElement* text = nullptr;
        /// The look's black copies behind the text: one at (+2, +2) for a
        /// `StaticText`, four at (+-1, +-1) for a `StaticTextOutline`.
        std::vector<Ogre::TextAreaOverlayElement*> drops;
        std::vector<std::pair<float, float>> drop_offsets;
        std::string window;
        /// The box, in authored units from the left of its parent.
        float x = 0.0f, y = 0.0f, wide = 0.0f, tall = 0.0f;
        /// `HorizontalAlignment` `Centre` on a window whose parent is the whole
        /// screen: the box is centred on the screen.
        bool centred_on_screen = false;
        /// Anchored to the bottom border strip, like the prompt bar's words.
        bool in_bottom_bar = false;
        /// `HorzTextFormatting` `CentreAligned` (or its word-wrapping twin).
        bool text_centred = false;
        /// `VertFormatting` `CentreAligned`.
        bool v_centred = false;
        /// `WordWrap...`: the text is wrapped to the box's width.
        bool wraps = false;
        float points = 11.0f;
        Ogre::ColourValue colour = Ogre::ColourValue::White;
        std::string dims_with;
        /// Shown only while this window is, which is how Done's caption goes
        /// with the button.
        std::string shown_with;
    };
    std::vector<ConsoleLabel> console_labels_;

    /// The console screen's non-interactive pictures: the border pieces, the
    /// frames and the pets' boxes.
    std::vector<Widget> console_frame_;

    /// The prompt bar's glyphs, anchored to the bottom border strip.
    std::vector<Widget> console_bar_;

    Ogre::Overlay* frame_ = nullptr;    ///< the bars
    Ogre::Overlay* widgets_ = nullptr;  ///< the icons and the buttons
    Ogre::Overlay* text_overlay_ = nullptr;
    Ogre::OverlayContainer* text_root_ = nullptr;

    std::vector<Ogre::PanelOverlayElement*> bars_;
    std::vector<Widget> icons_;    ///< one per class
    std::vector<Widget> buttons_;  ///< PC: Back, OK, Dog, Cat. Console: the pets, then Done
    Ogre::TextAreaOverlayElement* header_ = nullptr;
    Ogre::TextAreaOverlayElement* class_name_ = nullptr;
    Ogre::TextAreaOverlayElement* description_ = nullptr;
    Ogre::TextAreaOverlayElement* pet_title_ = nullptr;

    unsigned width_ = 0;
    unsigned height_ = 0;
    float scale_ = 1.0f;
    bool created_ = false;
    bool confirm_held_ = false;
    bool move_held_ = false;
    bool turn_held_ = false;
    bool skip_held_ = false;
};

class MainMenu {
public:
    MainMenu() = default;
    ~MainMenu();

    MainMenu(const MainMenu&) = delete;
    MainMenu& operator=(const MainMenu&) = delete;

    /// Read the interface data out of `group` and build the overlay. False (logged) when the data
    /// cannot be read; the backdrop then runs with no menu. `system` is the HUD's
    /// `OverlaySystem`: a second one would draw everything twice.
    bool create(const std::string& group, unsigned width, unsigned height);

    void destroy();
    void resize(unsigned width, unsigned height);

    /// One frame of input. Returns what the player asked for, if anything.
    ///
    /// A pad, as on the console: the D-pad or the left stick moves the focus,
    /// which opens on the first button (`CMainMenu::vfunc_7`,
    /// focuses the head of its tab list), and A acts on it.
    MenuAction update(const tl::core::InputState& input);

    /// A frame under another screen (Options, Help): nothing acts, but which
    /// buttons are down is kept, so the press that closes that screen is not
    /// read again here - Start closing Help would otherwise quit the game.
    void hold(const tl::core::InputState& input);

    void show();
    void hide();

    /// Swap the button row for the Choose Level list's first tier: one button per biome, tagged
    /// `MenuAction::ChooseLevelBiome` with `data` the biome key `Renderer::list_main_floors()`
    /// gave it, plus a `Back`. The port's own screen, laid out generically.
    void show_biome_list(const std::vector<std::pair<std::string, std::string>>& biomes);

    /// Swap the buttons for a debug editor (the wardrobe, the bestiary): one
    /// row per entry of `rows` (their captions), then `Back`, in a column on
    /// both screens, and a block of text beside them (`set_editor_info`). Up
    /// and down pick a row; left and right (held: repeating) step it by one
    /// and the bumpers by ten, each returning `MenuAction::EditorStep`. A
    /// steps it by one. `back` captions the last button; empty, there is none (the mods page).
    void show_editor(const std::vector<std::string>& rows, const std::string& back = "Back");
    /// A row's caption, once its choice has changed.
    void set_row_caption(size_t row, const std::string& text);
    /// The editor's text block; lines split on '\n'.
    void set_editor_info(const std::string& text);
    /// The row and the step the last `EditorStep` was for.
    int editor_row() const { return editor_row_; }
    int editor_step() const { return editor_step_; }
    /// The editor row the focus is on; -1 on the last button.
    /// The copyright line off, until the next `layout()` shows it (the mods page, which has none;
    /// renderer_mods_menu.cpp).
    void hide_copyright();
    /// A prompt of the bottom bar captioned `text`, found by its group's window (the mods page's
    /// Start, `credits`). Until the bar is built again.
    void set_bar_caption(const std::string& window, const std::string& text);
    /// Put the focus on button `index`, or on none for -1 (the mods page's checkbox has it).
    void set_focus(int index) {
        selected_ = index;
        apply_button_look();
    }
    int focused_row() const {
        if (selected_ < 0 || selected_ >= static_cast<int>(buttons_.size())) return -1;
        const Button& button = buttons_[static_cast<size_t>(selected_)];
        return button.action == MenuAction::EditorStep ? button.slot : -1;
    }

    /// Swap the button row for one biome's own floors: `entries` is
    /// (label, global floor number) pairs, each tagged `MenuAction::
    /// LoadDungeon`; `chosen_dungeon()` returns `"Main"` and
    /// `chosen_dungeon_floor()` the floor picked. Plus a `Back`.
    void show_floor_list(const std::vector<std::pair<std::string, int>>& entries);

    /// Swap back to New Character / Quit Game.
    void show_main_buttons();
    /// The console's column, without the debug tools when they are off.
    void add_console_column();

    /// Swap the column for the difficulty screen, `CDifficultyMenu`: the console's
    /// `media/UI/difficulty.uilayout` inside this menu's frame. `state` is the screen's own and
    /// outlives this menu; the focus it remembers is where it opens (`core/difficulty_menu.h`).
    /// `update()` then walks the tab list, flips the checkbox on A, and returns
    /// `MenuAction::NewGame` for a pick or `MenuAction::Back` for B; `difficulty_click()` says
    /// what was asked for.
    void show_difficulty(tl::core::DifficultyMenu& state);
    const tl::core::DifficultyClick& difficulty_click() const { return difficulty_click_; }

    /// Whether `show_main_buttons()` puts `ContinueLast` up.
    // PORT: the renderer's interface to the main menu.
    void set_continue_available(bool available) { continue_available_ = available; }

    /// Which dungeon the button that returned `LoadDungeon` was for: `"Main"`,
    /// from the Choose Level list.
    // PORT: the renderer's interface to the main menu.
    const std::string& chosen_dungeon() const { return chosen_dungeon_; }

    /// The floor that goes with `chosen_dungeon()` - 1 unless it came off
    /// the Choose Level list.
    // PORT: the renderer's interface to the main menu.
    int chosen_dungeon_floor() const { return chosen_dungeon_floor_; }

    /// Which biome the button that returned `ChooseLevelBiome` was for.
    const std::string& chosen_biome() const { return chosen_biome_; }

private:
    /// One drawable rectangle out of the layout: a bar, the logo, a button.
    struct Piece {
        Ogre::PanelOverlayElement* panel = nullptr;
        /// Where it sits, in the layout's own authored units.
        tl::formats::UVector2 position;
        tl::formats::UVector2 size;
        /// Which imageset, and the rectangle within its texture.
        const tl::formats::Imageset* set = nullptr;
        std::string image;
        /// Which material draws it, when the piece comes from one of the
        /// console's own imagesets rather than the two the PC screen uses.
        /// Empty for a PC piece, which picks its material from `set`.
        Ogre::MaterialPtr material;

        /// Drawn against the whole viewport rather than clipped to the
        /// centred band its parent defines - `ClippedByParent` is False on the
        /// bars, and the logo overhangs its own bar by a hundred pixels.
        bool centred = false;
    };

    using ArtSet = MenuArtSet;

    struct Button {
        Piece piece;
        Ogre::TextAreaOverlayElement* label = nullptr;
        Ogre::TextAreaOverlayElement* shadow = nullptr;
        std::string text;
        MenuAction action = MenuAction::None;
        /// Position in the row, 0 at the left. The row is centred on the
        /// authored pitch; see the note in menu.cpp.
        int slot = 0;
        /// The authored vertical offset from the bottom edge when the button sits
        /// off the main row (`ContinueLast` at -140), or 0 for the row itself.
        float row_offset = 0.0f;

        /// The authored rectangle this button occupies on the console screen,
        /// in `mainmenu.uilayout`'s own units and already flattened into the
        /// screen's frame of reference (the layout hangs them off a container).
        /// Unused by the PC row, which is placed on a pitch instead.
        float col_x = 0.0f, col_y = 0.0f, col_w = 0.0f, col_h = 0.0f;
        /// The console caption's size relative to the layout's: below 1 only
        /// when a list longer than the column's nine slots squeezes them.
        float caption_scale = 1.0f;

        /// A `LoadDungeon` button's full dungeon name, or a
        /// `ChooseLevelBiome` button's biome key - `text` is shortened or
        /// prettified for the row, this is the underlying value. Unused by
        /// every other action.
        std::string data;

        /// A `LoadDungeon` button's global floor number, when it came off
        /// the Choose Level list. 0 (the default) means "none" - a Test
        /// list pick, which `run_main_menu()` reads as floor 1.
        int floor = 0;

        /// The difficulty screen's `Hardcore`: a `ceguiWidget/Checkbox`, drawn
        /// from `widgets2` and flipped by A rather than chosen.
        bool checkbox = false;
    };

    bool load_imagesets(const std::string& group);

    /// Read `media/UI/mainmenu.uilayout` and the three `icons2` imagesets it
    /// draws from. False when the console data is not mounted, which is not an
    /// error: the PC row is drawn instead.
    bool load_console_menu(const std::string& group);

    /// The copyright line, bottom right, the console screen's or the PC row's.
    void build_copyright();

    /// One `icons2` imageset, its texture and its material.
    bool load_art_set(const char* file, const std::string& group, ArtSet& art);

    /// Where a console-screen image is drawn from: the sheet its reference names, else the PC's
    /// own `Logo` or `GuiLook` when they are mounted, else any sheet holding an image of that
    /// name. Null `set` when none has it.
    struct ArtRef {
        const tl::formats::Imageset* set = nullptr;
        Ogre::MaterialPtr material;
    };
    ArtRef resolve_art(const std::string& set_name, const std::string& image) const;

    /// The console column's button images, as its first button (`ContinueLast`) names them.
    void resolve_console_buttons();

    /// The console frame - the four border pieces - and the bottom bar's
    /// groups, from the console layout.
    void build_console_frame();

    /// Place the console column and its copyright line, in viewport pixels.
    void layout_console_buttons(float across, float down);
    void layout_console_copyright(float across, float down);

    /// The bottom bar's prompts, built once and placed on every layout pass.
    void build_console_bar();
    void layout_console_bar(float across, float down);
    /// One group of the bar: the window holding a glyph and the word beside it.
    struct BarGroup {
        std::string window;
        std::string glyph;
        std::string caption;
    };
    /// Build the bar from `layout`'s container `bar_window`. `authored_colour`
    /// takes each caption's own `TextColour`.
    void build_bar(const tl::formats::GuiLayout& layout, const std::string& bar_window,
                   const std::vector<BarGroup>& groups, bool authored_colour);
    void clear_bar();

    /// The difficulty screen's pieces that are not buttons.
    bool load_difficulty_layout();
    void build_difficulty_extras();
    void clear_extras();
    void layout_extras(float across, float down);
    /// Where the console column's container sits, in viewport pixels.
    void console_column_origin(float across, float down, float& left, float& top) const;
    MenuAction update_difficulty(const tl::core::InputState& input);

    /// Add one console button, taking its rectangle and its caption from the
    /// layout's own window of that name.
    void add_console_button(const char* window, const std::string& text, MenuAction action);
    /// On the console screen, give the Test and Choose Level lists' buttons
    /// (built by `add_button()`, which knows only the PC row) rectangles down
    /// the console column and its gem art. Nothing on the PC screen.
    void stack_console_list();
    bool load_font(const std::string& group);
    Ogre::MaterialPtr material_for(const tl::formats::Imageset& set, const std::string& group,
                                   Ogre::TexturePtr& texture);
    Ogre::PanelOverlayElement* make_panel(const std::string& name, Ogre::Overlay* overlay);
    /// Point a panel at one named rectangle of an imageset's texture.
    void set_uv(Ogre::PanelOverlayElement* panel, const tl::formats::Imageset& set,
                const std::string& image) const;
    void add_piece(const std::string& window, const char* fallback_image, bool centred);
    void add_button(const std::string& text, MenuAction action, int slot,
                    const std::string& data = {}, int floor = 0, float row_offset = 0.0f);
    /// Destroy every current button's overlay elements and empty `buttons_`,
    /// so `add_button()` can rebuild the row - what every `show_*()` does
    /// first. The wardrobe editor's text block goes with them.
    void clear_buttons();
    void clear_editor_info();
    /// Place the wardrobe editor's text block, in viewport pixels.
    void layout_editor_info(float across, float down);
    void layout();
    void apply_button_look();

    unsigned width_ = 0;
    unsigned height_ = 0;
    /// Pixels per authored unit: the viewport's height over the interface's native height.
    float scale_ = 1.0f;

    /// Three overlays, not one: within one overlay `OverlayElement::_updateRenderQueue` queues
    /// every element at the same priority and OGRE sorts transparent renderables by pass, so the
    /// buttons inside the bottom bar's rectangle could draw under it. Ascending z-orders give
    /// ordered priority groups; the HUD's overlay (500) is above all three.
    Ogre::Overlay* frame_ = nullptr;  ///< the bars and the logo
    Ogre::Overlay* buttons_overlay_ = nullptr;
    Ogre::Overlay* text_overlay_ = nullptr;

    /// A transparent container holding every piece of text (`Overlay::add2D` takes only an
    /// `OverlayContainer`).
    Ogre::PanelOverlayElement* text_root_ = nullptr;

    std::vector<Piece> pieces_;
    std::vector<Button> buttons_;
    Ogre::TextAreaOverlayElement* copyright_ = nullptr;
    std::string copyright_text_;

    tl::formats::GuiLayout menu_;
    tl::formats::Imageset logo_set_;
    tl::formats::Imageset gui_set_;
    Ogre::MaterialPtr logo_material_;
    Ogre::MaterialPtr gui_material_;
    Ogre::TexturePtr logo_texture_;
    Ogre::TexturePtr gui_texture_;

    /// The console's own main menu, `media/UI/mainmenu.uilayout` (Xbox archive only, as the CEGUI
    /// export): a column of nine slots down the left. Empty without the console data, and then
    /// the PC row is drawn.
    tl::formats::GuiLayout console_;
    bool console_mode_ = false;

    /// The resource group the screens are read from, kept for the layouts
    /// read after `create()`.
    std::string group_;

    /// **The difficulty screen**, `media/UI/difficulty.uilayout` (the file the
    /// console's builder names). Read when first shown.
    tl::formats::GuiLayout difficulty_;
    bool difficulty_mode_ = false;
    tl::core::DifficultyMenu* difficulty_state_ = nullptr;
    tl::core::DifficultyClick difficulty_click_;
    /// The tab list, in the order `buttons_` holds its windows.
    std::vector<tl::core::DifficultyTab> difficulty_tabs_;
    /// Which of `buttons_` each window of the tab list is.
    std::vector<size_t> difficulty_button_;
    /// The windows from the layout's root down to the one holding the buttons,
    /// and down to the title.
    std::vector<const tl::formats::GuiWindow*> difficulty_column_path_;
    std::vector<const tl::formats::GuiWindow*> difficulty_title_path_;

    /// A picture that is not a button: a slice of the panel, or the tick.
    struct Extra {
        Piece piece;
        /// 0-8, a slice of the `ItemText` frame; -1 for a plain image.
        int slice = -1;
        /// The rectangle within the column, in authored units.
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
        float corner_w = 0.0f, corner_h = 0.0f;
        /// The checkbox's `select_mark`: shown only while it is ticked.
        bool tick = false;
    };
    std::vector<Extra> extras_;

    /// A line of text that is not a caption.
    struct ExtraLabel {
        Ogre::TextAreaOverlayElement* text = nullptr;
        Ogre::TextAreaOverlayElement* shadow = nullptr;
        /// The authored box, within the column or (the title) as resolved
        /// from `difficulty_title_path_`.
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
        float points = 16.0f;
        bool title = false;
    };
    std::vector<ExtraLabel> extra_labels_;

    /// `widgets`, `widgets1` and `widgets2` from `media/UI/icons2/`: the frame
    /// art, the gem buttons and the A/START/stick glyphs the bottom bar names.
    ArtSet widgets_art_;
    ArtSet widgets1_art_;
    ArtSet widgets2_art_;

    /// The PC's `Logo` and `GuiLook` loaded beside the console sheets (a PC pak only; the Xbox's
    /// carries neither), for the images those sheets leave out.
    bool pc_art_ = false;
    /// The column's button images and where they are drawn from (`resolve_console_buttons`).
    std::string console_normal_ = "gem_button";
    std::string console_hover_ = "gem_button_rollover";
    ArtRef console_button_art_;
    ArtRef checkbox_art_;

    /// The bottom bar's three groups - the glyph and the word beside it. The layout authors four;
    /// `invitefriend` is Xbox Live and is left out.
    struct BarItem {
        /// The group's window in the layout (`credits`, ...).
        std::string window;
        Piece glyph;
        Ogre::TextAreaOverlayElement* label = nullptr;
        Ogre::TextAreaOverlayElement* shadow = nullptr;
        /// The group's own authored rectangle, and the glyph's within it.
        float x = 0.0f, y = 0.0f;
        float glyph_x = 0.0f, glyph_y = 0.0f, glyph_w = 0.0f, glyph_h = 0.0f;
        float text_x = 0.0f, text_y = 0.0f, text_h = 0.0f;
    };
    std::vector<BarItem> bar_items_;

    /// The two images `GuiLook.looknfeel` names for a StandardButton.
    std::string button_normal_;
    std::string button_hover_;

    std::string font_;
    float font_height_ = 0.0f;

    /// Which button has the focus. The first, when a menu opens.
    int selected_ = 0;
    bool confirm_held_ = false;
    bool quit_held_ = false;
    bool skip_held_ = false;
    bool nav_held_ = false;
    bool created_ = false;

    /// Whether `show_main_buttons()` puts `ContinueLast` up.
    bool continue_available_ = false;
    /// Whether the button row is one of the generic lists (Choose Level's biomes, a biome's
    /// floors, the wardrobe editor) rather than the main buttons; see `layout()`.
    bool list_mode_ = false;
    std::string chosen_dungeon_;
    int chosen_dungeon_floor_ = 1;
    std::string chosen_biome_;
    size_t button_serial_ = 0;

    /// A debug editor (`show_editor`): its rows are walked up and down on
    /// both screens, and left/right step the focused one.
    bool editor_mode_ = false;
    int editor_row_ = 0;
    int editor_step_ = 0;
    /// Left or right held (-1, 0, 1), and when it next repeats (seconds on
    /// `std::chrono::steady_clock`).
    int step_held_ = 0;
    double step_repeat_at_ = 0.0;
    bool bumper_held_ = false;
    Ogre::TextAreaOverlayElement* editor_info_ = nullptr;
    Ogre::TextAreaOverlayElement* editor_info_shadow_ = nullptr;
};

}  // namespace tl::render
