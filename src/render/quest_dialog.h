// The window an NPC offers a quest in, from the Xbox 360 release's
// `media/UI/questdialogmenu_xenon.layout`. A separate screen from the dialogue box in
// `render/hud.h`, as in the original: `CGameUI::forceInteractiveDialog` branches on the line's
// `FLOATYTEXT` key, and a non-floaty line opens `CQuestDialogMenu`, a menu that stops the player.
// The layout ships in the Xbox archive only; without it there is no offer window.
#pragma once

#include "core/quest_book.h"
#include "formats/cegui.h"
#include "formats/uilayout.h"

#include <OgrePrerequisites.h>

#include <OgreMaterial.h>
#include <OgreTexture.h>

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

/// What the window is showing.
struct QuestOfferState {
    /// The speaker's `PORTRAIT`, a name in `media/UI/itemicons/itemicons.imageset`
    /// (`CQuestDialogMenu::setNPC` hands it to `CGameUI::getImageFromImageSet`). Empty draws the
    /// window without one.
    std::string portrait;

    /// The line, with its `|c` markup intact: the runs are drawn.
    std::string text;

    /// What accepting would pay. A figure of zero hides its box, which is what
    /// `setNPC` does either side of each `getQuestReward*()` call.
    tl::core::QuestReward reward;

    /// The item the quest hands over, as an icon name in `media/UI/itemicons/`, and its name:
    /// `[REWARD] TREASURECLASS` rolled by `CQuestRewards::calculateRewards` through
    /// `CSpawnClass::rollSpawnClass`. Empty when the quest pays no item, or its class picks by
    /// `UNITTYPE` (no item roller here; logged).
    std::string reward_icon;
    std::string reward_item;

    /// An offer (`A Accept` / `B Decline`) or a line only to be read (the single `OK`).
    /// `CQuestDialogMenu::onClick` serves both; `[COMPLETE]` and `[PASSIVE]` are never offers.
    bool offer = true;

    /// An offer of a `FORCEACCEPT` quest (`CQuest +0x28` to `CQuestDialogMenu +0x1ea`, `setNPC`):
    /// `Decline` is hidden and closing (`handle_CloseButton`
    /// `handle_ExitButton`) accepts. Clear from
    /// `CQuestDialogMenu::CQuestDialogMenu`.
    bool force_accept = false;

    /// Whether the reward row is up. `<LStick> Rewards` toggles it, which is
    /// the only thing the left stick does while this window is open.
    bool rewards = true;
};

/// The offer window.
class QuestDialogScreen {
public:
    QuestDialogScreen() = default;
    ~QuestDialogScreen();

    QuestDialogScreen(const QuestDialogScreen&) = delete;
    QuestDialogScreen& operator=(const QuestDialogScreen&) = delete;

    /// Build it against the mounted archive. False - with one logged line - when
    /// the console interface data is not mounted, which is survivable: the game
    /// runs exactly as it did before there was an offer window.
    bool create(const std::string& group, unsigned width, unsigned height);
    void destroy();
    bool ready() const { return built_; }

    void show();
    void hide();
    bool visible() const { return visible_; }

    void set_state(const QuestOfferState& state);
    // PORT: the renderer's interface to the offer window.
    const QuestOfferState& state() const { return state_; }

    /// Toggle the reward row, which is what the left stick does.
    void toggle_rewards();

    void resize(unsigned width, unsigned height);

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
        float points = 0.0f;
        int h_text = 0;
        int v_text = 0;
        std::string widget;
        bool hidden = false;          ///< authored `VISIBLE:false`
        bool runtime_hidden = false;  ///< taken down by set_state()

        float left = 0.0f, top = 0.0f, width = 0.0f, height = 0.0f;
    };

    struct Sprite {
        const tl::formats::Imageset* set = nullptr;
        const tl::formats::ImagesetImage* rect = nullptr;
        Ogre::TexturePtr texture;
        Ogre::MaterialPtr material;
    };

    bool load_art(const std::string& group);
    bool add_layout(const std::string& path, const std::string& group);
    void build(const tl::formats::UiObject& object, int parent, const std::string& prefix,
               size_t& serial, bool inherited_hidden);
    void layout();
    void apply_state();
    void lay_out_body();
    bool is_hidden(int index) const;

    const Sprite* sprite(const std::string& name) const;
    int find_widget(const std::string& name) const;
    void set_widget_text(const std::string& name, const std::string& text);
    void set_widget_visible(const std::string& name, bool visible);

    /// The nine slices `ceguiWidget/ItemText` draws its panel with: `ceguiWidget/StaticShared`'s
    /// `frame` in `ceguiWidgets.looknfeel`, the nine `tooltip*` images of `widgets1` over
    /// `LeftEdge -24, TopEdge -9, Width +48, Height +18`.
    void add_item_text_frame(int owner, const std::string& prefix, size_t& serial);

    std::vector<tl::formats::Imageset> sets_;
    std::vector<Ogre::TexturePtr> textures_;
    std::vector<Ogre::MaterialPtr> materials_;
    std::map<std::string, Sprite> sprites_;

    std::vector<Node> nodes_;

    /// The body text, one element per coloured run per line: a pool, since OGRE's overlay element
    /// names are global and set_state() runs on every advance.
    std::vector<Ogre::TextAreaOverlayElement*> body_;

    Ogre::Overlay* overlay_ = nullptr;
    Ogre::Overlay* text_overlay_ = nullptr;
    Ogre::OverlayContainer* panel_root_ = nullptr;
    Ogre::OverlayContainer* text_root_ = nullptr;

    QuestOfferState state_;
    unsigned width_ = 0;
    unsigned height_ = 0;
    float scale_ = 1.0f;
    bool built_ = false;
    bool visible_ = false;
};

}  // namespace tl::render
