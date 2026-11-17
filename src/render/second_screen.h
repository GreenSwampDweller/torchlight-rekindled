// PORT: what a second display (a two-screen handheld's other screen) shows besides the map:
// the skill slots (GameHud::set_second_screen) over a backdrop of its own.
//
// OGRE draws every visible overlay in every viewport that has overlays on, so the two screens
// are told apart here: for the second window's viewport the overlays up on the main screen are
// hidden and this screen's own shown, and put back after it.
//
// Beside those: a readout of the character (level, fame, gold) and the floor's name in the
// corners under the strip, a button that uses a Town Portal Scroll, with how many are
// carried, and a button for each spell learned from a scroll, which casts it. hit() says what
// a tap on that screen is on.
//
// While the inventory is open on the main screen: the details of the item under its cursor,
// beside those of the worn items it would replace, large (set_item_tips).
//
// The backdrop is drawn, not loaded: a bezel of ticks round the map and a scatter of ember
// specks in the corners, on black. Nearly every pixel is off, for an OLED panel.
#pragma once

#include "core/equipment_tooltip.h"

#include <OgreMaterial.h>
#include <OgreRenderTargetListener.h>
#include <OgreTexture.h>

#include <array>
#include <string>
#include <vector>

namespace Ogre {
class Overlay;
class OverlayContainer;
class PanelOverlayElement;
class TextAreaOverlayElement;
class RenderWindow;
}  // namespace Ogre

namespace tl::render {

class GameHud;

class SecondScreen final : public Ogre::RenderTargetListener {
public:
    SecondScreen() = default;
    ~SecondScreen() override;

    SecondScreen(const SecondScreen&) = delete;
    SecondScreen& operator=(const SecondScreen&) = delete;

    /// Where the backdrop draws round, in the window's pixels: the map's circle, and how far
    /// down from the top the HUD's strip reaches (nothing is scattered under it).
    struct Shape {
        float centre_x = 0.0f;
        float centre_y = 0.0f;
        float radius = 0.0f;
        float strip_bottom = 0.0f;
        bool operator==(const Shape& other) const {
            return centre_x == other.centre_x && centre_y == other.centre_y &&
                   radius == other.radius && strip_bottom == other.strip_bottom;
        }
    };

    /// Draw on `window`, whose full-window viewport (the map's backdrop, Automap) gets the
    /// overlays; `hud` for its strip. The same window again takes its new size and shape.
    void attach(Ogre::RenderWindow* window, GameHud* hud, const Shape& shape);
    /// Let the window go, before it or the overlay system does.
    void detach();

    /// What the readout and the portal button say.
    struct Readout {
        int level = 0;
        int fame_level = 0;
        int gold = 0;
        std::string floor;        ///< the level's name, as the HUD's own top right has it
        int portal_scrolls = 0;   ///< Town Portal Scrolls carried
        std::string portal_icon;  ///< the scroll's icon, while one is carried
        /// The spells learned from scrolls, by their slot: whether there is one, its picture,
        /// and the whole seconds left before it can be cast again (0 when it can).
        struct Spell {
            bool known = false;
            std::string icon;
            int cooling = 0;
            bool operator==(const Spell& other) const {
                return known == other.known && icon == other.icon && cooling == other.cooling;
            }
        };
        static constexpr size_t kSpells = 4;
        std::array<Spell, kSpells> spells{};
    };
    void set_readout(const Readout& readout);

    /// The details of the item the inventory has under its cursor and of the worn items it
    /// stands against (`CGameUI`'s three item windows), drawn side by side as large as the
    /// screen under the strip takes them: the main screen's own windows are small on a
    /// handheld. While any are given they have the screen, in place of the map, the readout
    /// and the buttons. Empty gives it back.
    void set_item_tips(std::vector<tl::core::EquipmentTooltip> tips);
    bool item_tips_shown() const { return !item_tips_.empty(); }

    /// What a tap at (`x`, `y`), in the window's pixels, is on. Nothing while the game's HUD
    /// is down; the strip is the HUD's own (GameHud::second_tap_at says what of it); the map is
    /// everything under the strip that is not a button.
    enum class Tap { None, Map, Portal, Strip, Spell };
    /// `spell` is given the slot of the spell a `Tap::Spell` is on.
    Tap hit(float x, float y, int* spell = nullptr) const;

private:
    void preViewportUpdate(const Ogre::RenderTargetViewportEvent& evt) override;
    void postViewportUpdate(const Ogre::RenderTargetViewportEvent& evt) override;

    void build_backdrop(unsigned width, unsigned height, const Shape& shape);
    void destroy_backdrop();
    void build_widgets();
    void destroy_widgets();
    void layout_widgets();
    void apply_readout();

    Ogre::RenderWindow* window_ = nullptr;
    GameHud* hud_ = nullptr;

    Ogre::Overlay* backdrop_ = nullptr;
    Ogre::PanelOverlayElement* panel_ = nullptr;
    Ogre::TexturePtr texture_;
    Ogre::MaterialPtr material_;
    unsigned backdrop_width_ = 0;
    unsigned backdrop_height_ = 0;
    Shape backdrop_shape_;

    /// The readout's words and the portal button, in two overlays over the backdrop: pictures,
    /// then words.
    Ogre::Overlay* widgets_ = nullptr;
    Ogre::Overlay* words_ = nullptr;
    Ogre::OverlayContainer* widget_root_ = nullptr;
    Ogre::OverlayContainer* word_root_ = nullptr;
    Ogre::PanelOverlayElement* portal_frame_ = nullptr;
    Ogre::PanelOverlayElement* portal_icon_ = nullptr;
    Ogre::TextAreaOverlayElement* portal_count_ = nullptr;
    Ogre::TextAreaOverlayElement* portal_label_ = nullptr;
    Ogre::TextAreaOverlayElement* level_text_ = nullptr;
    Ogre::TextAreaOverlayElement* fame_text_ = nullptr;
    Ogre::TextAreaOverlayElement* floor_text_ = nullptr;
    Ogre::TextAreaOverlayElement* gold_text_ = nullptr;
    Readout readout_;
    std::string shown_icon_;  ///< the picture `portal_icon_` has now
    Shape shape_;
    /// The portal button, in the window's pixels.
    float portal_left_ = 0.0f, portal_top_ = 0.0f, portal_size_ = 0.0f;

    /// One button a learned spell: its frame, its picture and the seconds it is cooling for,
    /// up the right edge from the bottom corner in slot order.
    struct SpellButton {
        Ogre::PanelOverlayElement* frame = nullptr;
        Ogre::PanelOverlayElement* icon = nullptr;
        Ogre::TextAreaOverlayElement* cooling = nullptr;
        std::string shown_icon;
        float left = 0.0f, top = 0.0f;  ///< in the window's pixels; `portal_size_` a side
    };
    std::array<SpellButton, Readout::kSpells> spell_buttons_{};

    void build_item_overlays();
    void destroy_item_overlays();
    void layout_item_tips();

    /// The item windows: a plate each under their words, in two overlays of their own.
    struct ItemWindow {
        Ogre::PanelOverlayElement* edge = nullptr;
        Ogre::PanelOverlayElement* plate = nullptr;
        std::vector<Ogre::TextAreaOverlayElement*> lines;
    };
    static constexpr size_t kItemWindows = 3;
    std::array<ItemWindow, kItemWindows> item_windows_{};
    Ogre::Overlay* item_plates_ = nullptr;
    Ogre::Overlay* item_words_ = nullptr;
    Ogre::OverlayContainer* item_plate_root_ = nullptr;
    Ogre::OverlayContainer* item_word_root_ = nullptr;
    Ogre::TexturePtr item_plate_texture_, item_edge_texture_;
    Ogre::MaterialPtr item_plate_material_, item_edge_material_;
    std::vector<tl::core::EquipmentTooltip> item_tips_;
    std::string item_signature_;  ///< every text of `item_tips_`, to tell a change by

    /// Between the two viewport events: the main screen's overlays, hidden for this viewport,
    /// and this screen's, shown for it.
    std::vector<Ogre::Overlay*> hidden_;
    std::vector<Ogre::Overlay*> shown_;
    bool swapped_ = false;
};

}  // namespace tl::render
