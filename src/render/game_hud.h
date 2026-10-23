// The in-game HUD, drawn from the Xbox 360 release's own interface data:
// `media/UI/hud_xenon.layout` (the orbs, potion slots, level name, skill slots, bag, experience
// bar and target health) and `media/UI/pethud_xenon.layout` (the pet panel), Runic's `[UI]`
// format (`src/formats/uilayout.h`), with the art of the four imagesets under `media/UI/icons2/`.
// The minimap is not here (no layout places it; see automap.h). The art ships in the Xbox
// archive only: against a PC install this logs once and draws nothing.
#pragma once

#include "core/hotbar.h"
#include "formats/cegui.h"
#include "formats/uilayout.h"

#include <OgrePrerequisites.h>

#include <OgreColourValue.h>

#include <OgreMaterial.h>
#include <OgreTexture.h>

#include <array>
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

/// What the HUD is showing: the layout says where a bar is and how big, this says how full.
struct HudState {
    float health = 1.0f;      ///< 0..1. Real: `Renderer::Impl::player_health`.
    float mana = 1.0f;        ///< 0..1. Placeholder: no mana in the engine yet.
    float experience = 0.0f;  ///< 0..1. Placeholder: no experience yet.

    /// The level's own `LEVELNAME`, top right.
    std::string level_name;

    /// The two banners across the middle: `LargeMessageText` (32 point) and `SmallMessageText`
    /// (16), `CGameUI::setBigText` and `setSmallText`. Filled on arrival
    /// (`CGameClient::loadLevel`), by quests (`giveQuest`, `questEventUpdate`)
    /// and on level-up (`CPlayer::levelUp`).
    std::string announce;
    std::string sub_announce;

    /// How opaque each banner is, 0..1: `setBigText` sets alpha
    /// `+0x1984` to 1 and the hold timer `+0x1980`, `setSmallText` `+0x1990`/`+0x1994`;
    /// `CGameUI::updateIngameUI` runs the timer down, then fades by 2.0 a second.
    float announce_alpha = 1.0f;
    float sub_announce_alpha = 1.0f;

    /// The Xbox `+` over the bag (`AvailableSkillsIcon`) and its glow
    /// (`AvailableSkillsGlowIcon`): shown while there are stat or skill points
    /// to spend, the glow coming and going once a second
    /// (`tl::core::AvailablePointsIcon`).
    bool available_points = false;
    bool available_points_glow = false;

    /// The ten function slots as `CGameUI::updateSlots`
    /// dresses them: `SkillSlot<N+1>` the picture, `SkillText<N+1>` the caption, except slots 0
    /// and 1 caption into `POTIONCOUNT` and `MANACOUNT`.
    std::array<tl::core::HotbarSlotView, tl::core::kSkillFunctionSlots> slots{};
    /// Which bank of four is up, `SkillSet1` (0) or `SkillSet2` (1): the
    /// skills tab's D-pad choice (`CGameUI +0x1464`).
    int skill_set = 0;

    /// Whether to draw `pethud_xenon.layout` at all: true while there is an
    /// animal in the level, false while it is away in town.
    bool pet = false;

    /// The two bars in the panel. Health is the animal's own; mana is a placeholder.
    float pet_health = 1.0f;
    float pet_mana = 1.0f;

    /// The `FleeingText` window, shown only while the animal flees
    /// (`CCharacter::isPetNearDeath()`; see `core/pet.h`).
    bool pet_fleeing = false;

    /// What the console's target selector points at, for the `TargetHealth` panel (with
    /// `TargetName`, `TargetLevel`, `TargetDescription` and `TargetHealthBar`), `runtime only` in
    /// the layout. `target_description` stays empty: `CGameUI::updateIngameUI` fills it and that
    /// part is not transcribed.
    bool target = false;
    std::string target_name;
    /// `CCharacter +0x702`: `CGameUI::updateIngameUI`
    /// sets the name's `TextColour` by it at
    /// (1, 0.88, 0.24) for a champion, white for anything else.
    bool target_champion = false;
    int target_level = 0;
    std::string target_description;
    float target_health = 1.0f;  ///< 0..1

    /// Which of the stance tray's three buttons is lit: 0 Aggressive, 1 Defensive, 2 Passive. An
    /// unchosen stance draws its `DISABLED` (`_INACTIVE`) art.
    int pet_stance = 1;
};

/// The name over one thing on the floor - an item, gold, a door, a chest - as
/// the console draws it: a `ceguiWidget/ItemTextButton` window
/// (creates one per item), which `ceguiWidgets.looknfeel` draws as
/// the `StaticShared` panel with the text centred on it, and, in its `Enabled`
/// state only, `widgets1`'s `a_button` 20 pixels left of it. Enabled is
/// highlighted (`setItemTextHighlighted`).
/// Which labels exist and their colours are `core/item_label.h`'s.
struct GroundLabel {
    std::string text;
    /// Where the anchor (the item's position lifted by the camera's up
    /// vector) projects, in screen pixels.
    float at_x = 0.0f;
    float at_y = 0.0f;
    Ogre::ColourValue colour = Ogre::ColourValue::White;
    bool enabled = false;  ///< highlighted: the A button is drawn
};

/// The game screen's interface, from `hud_xenon.layout` and `pethud_xenon.layout`.
class GameHud {
public:
    GameHud() = default;
    ~GameHud();

    GameHud(const GameHud&) = delete;
    GameHud& operator=(const GameHud&) = delete;

    /// Build it against the mounted archive. False, with one logged line, when the console
    /// interface data is not mounted (the game runs without a HUD).
    bool create(const std::string& group, unsigned width, unsigned height);

    void destroy();
    bool ready() const { return built_; }

    void show();
    void hide();
    bool visible() const { return visible_; }

    void set_state(const HudState& state);
    const HudState& state() const { return state_; }

    /// Re-lay-out for a new viewport size.
    void resize(unsigned width, unsigned height);
    /// Scale the authored HUD footprint for small touch screens.
    void set_compact(bool compact);

    /// This frame's floor labels, drawn over the scene and under the HUD. The
    /// last one given is drawn on top (the highlighted label's `moveToFront`);
    /// past `kMaxGroundLabels` the rest are not drawn.
    void set_ground_labels(const std::vector<GroundLabel>& labels);
    static constexpr size_t kMaxGroundLabels = 32;

private:
    /// One drawn element, with the layout's authored numbers kept so `resize()` can place it
    /// again.
    struct Node {
        /// Where it sits inside its parent, in authored pixels, plus the
        /// fraction of the parent its size takes. See uilayout.h for the rule.
        float off_x = 0.0f, off_y = 0.0f;
        float wide = 0.0f, tall = 0.0f;
        float scale_wide = 0.0f, scale_tall = 0.0f;
        /// 0 left/top, 1 centre, 2 right/bottom - the layout's `HORIZONTAL` and
        /// `VERTICLE`, which the CEGUI export writes as `HorizontalAlignment`.
        int h_anchor = 0, v_anchor = 0;

        int parent = -1;  ///< index into `nodes_`, or -1 for a top-level object

        Ogre::PanelOverlayElement* panel = nullptr;
        Ogre::TextAreaOverlayElement* text = nullptr;
        Ogre::TextAreaOverlayElement* drop = nullptr;  ///< `Text Outlined`'s shadow

        /// The imageset rectangle this panel draws, and the texture it is in.
        std::string image;

        /// A button's `NORMAL` (lit) and `DISABLED` pictures: the pet's stance tray swaps them at
        /// runtime.
        std::string normal_image;
        std::string disabled_image;
        float points = 0.0f;  ///< the authored font size, for a text node
        int h_text = 0;       ///< `HORIZONTAL ALIGN`
        int v_text = 0;       ///< `VERTICAL ALIGN`

        /// `WIDGET NAME` - the handle the original's engine drives a window by,
        /// and the only stable identifier in these files.
        std::string widget;

        /// How this element is filled, 0..1, and from which edge it empties.
        /// -1 when it is not a bar at all, which is nearly all of them.
        int fill = -1;

        bool hidden = false;  ///< authored `VISIBLE:false`, or runtime-only
        bool grey = false;    ///< drawn through `grey_materials_`, 0.6 grey
        /// Which bank of skill slots this sits in: 0 under `SkillSet1`, 1 under
        /// `SkillSet2`, -1 elsewhere.
        int skill_set = -1;

        /// Where it landed last layout, in viewport fractions.
        float left = 0.0f, top = 0.0f, width = 0.0f, height = 0.0f;
    };

    /// Which edge a bar empties from. The orbs drain downwards like a liquid
    /// and the bars fill from the left; see the note in game_hud.cpp.
    enum FillKind { kFillNone = -1, kFillDown = 0, kFillRight = 1 };

    bool load_art(const std::string& group);
    bool add_layout(const std::string& path, const std::string& group, const char* what);
    void build(const tl::formats::UiObject& object, int parent, const std::string& prefix,
               size_t& serial, bool inherited_hidden);
    void layout();
    void apply_state();
    void set_uv(Node& node, float u0, float v0, float u1, float v1) const;

    /// The four `media/UI/icons2/*.imageset` files, and which texture each named rectangle is cut
    /// from. Their 282 names do not collide, so the lookup is flat.
    struct Sprite {
        const tl::formats::Imageset* set = nullptr;
        const tl::formats::ImagesetImage* rect = nullptr;
        Ogre::TexturePtr texture;
        Ogre::MaterialPtr material;
    };
    const Sprite* sprite(const std::string& name) const;

    /// Re-point one panel at a different rectangle of the artwork. Does nothing
    /// for a name the imagesets have not got, or one it is already showing.
    void set_node_image(Node& node, const std::string& name);

    /// A hotbar slot's picture: as `set_node_image`, but an empty name clears
    /// the panel back to the bare window, and `grey` draws it at the 0.6 grey
    /// `CGameUI::updateSlots` writes into
    /// `ImageColours`.
    void set_slot_image(Node& node, const std::string& name, bool grey);
    /// A sheet's material again, its texture modulated by 0.6 grey. Built the
    /// first time a slot asks for it.
    Ogre::MaterialPtr grey_material(const Sprite& art);
    std::map<std::string, Ogre::MaterialPtr> grey_materials_;

    std::vector<tl::formats::Imageset> sets_;
    std::vector<Ogre::TexturePtr> textures_;
    std::vector<Ogre::MaterialPtr> materials_;
    std::map<std::string, Sprite> sprites_;  ///< keyed by lower-cased name

    std::vector<Node> nodes_;

    /// Where `pethud_xenon.layout`'s windows start in `nodes_`, so one flag shows the panel.
    size_t pet_first_ = 0;

    /// The node range of the `TargetHealth` panel and everything under it, so
    /// the whole thing appears with a target and goes with it. Zero-zero while
    /// the layout has no such window.
    size_t target_first_ = 0;
    size_t target_last_ = 0;
    Ogre::Overlay* overlay_ = nullptr;       ///< the pictures
    Ogre::Overlay* text_overlay_ = nullptr;  ///< the words, above them
    /// Every panel is an absolutely placed child of one full-screen container. Children are named
    /// by a pre-order draw index, and a container keeps them in a map keyed by name, so the map's
    /// order is the layout tree's draw order.
    Ogre::OverlayContainer* panel_root_ = nullptr;
    Ogre::OverlayContainer* text_root_ = nullptr;

    /// One pooled label: the frame's nine pieces, the A button and the text.
    struct LabelSlot {
        std::array<Ogre::PanelOverlayElement*, 9> frame{};
        Ogre::PanelOverlayElement* button = nullptr;
        Ogre::TextAreaOverlayElement* text = nullptr;
    };
    void build_labels();
    void layout_labels();
    void destroy_labels();
    std::vector<LabelSlot> labels_;
    std::vector<GroundLabel> ground_labels_;
    Ogre::Overlay* label_overlay_ = nullptr;       ///< the panels and buttons
    Ogre::Overlay* label_text_overlay_ = nullptr;  ///< their words
    Ogre::OverlayContainer* label_root_ = nullptr;
    Ogre::OverlayContainer* label_text_root_ = nullptr;

    HudState state_;
    unsigned width_ = 0;
    unsigned height_ = 0;
    float scale_ = 1.0f;
    float compact_scale_ = 1.0f;
    bool compact_ = false;
    bool built_ = false;
    bool visible_ = false;
};

}  // namespace tl::render
