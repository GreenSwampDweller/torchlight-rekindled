// The cinematic screen (`CCinematicMenu`, `media/ui/cinematicmenu.layout`): art, crawling text, a
// recording, and Continue. What opens it and how the crawl runs is `core/cinematics.h`.
#pragma once

#include "core/cinematics.h"
#include "formats/cegui.h"
#include "render/menu.h"
#include "render/menu_chrome.h"

#include <OgreColourValue.h>
#include <OgreMaterial.h>
#include <OgreOverlay.h>
#include <OgreOverlayContainer.h>
#include <OgrePanelOverlayElement.h>
#include <OgreTextAreaOverlayElement.h>
#include <OgreTexture.h>

#include <string>
#include <vector>

namespace tl::core {
struct InputState;
}

namespace tl::render {

class CinematicMenu {
public:
    CinematicMenu() = default;
    ~CinematicMenu();

    CinematicMenu(const CinematicMenu&) = delete;
    CinematicMenu& operator=(const CinematicMenu&) = delete;

    /// Build the screen for one cinematic (`CCinematicMenu::setText`).
    /// Group is the OGRE resource group (e.g.
    /// kGameGroup).
    bool create(const std::string& group, unsigned width, unsigned height,
                const tl::core::Cinematic& cinematic);

    /// Clean up overlays, textures, and materials. Safe to call multiple times.
    void destroy();

    /// One frame of input and crawl (`CCinematicMenu::update`).
    /// Returns true once Continue was taken.
    bool update(float delta, const tl::core::InputState& input);

    /// The crawl, for a frame dump at a chosen moment.
    const tl::core::CinematicCrawl& crawl() const { return crawl_; }
    /// Run the crawl forward by `seconds` with nobody at the pad.
    void advance_crawl(float seconds);

    const std::string& name() const { return name_; }

private:
    struct TextLine {
        Ogre::TextAreaOverlayElement* text = nullptr;
        Ogre::TextAreaOverlayElement* shadow = nullptr;
        float base_y_rel = 0.0f;  ///< below the text window's top
    };

    void layout();
    void place_text(float offset_pixels);
    void set_uv(Ogre::PanelOverlayElement* panel, const tl::formats::Imageset& set,
                const Ogre::TexturePtr& texture, const std::string& image) const;

    bool built_ = false;
    unsigned width_ = 0;
    unsigned height_ = 0;
    std::string name_;
    std::string narrative_text_;
    tl::core::CinematicCrawl crawl_;

    // Overlays, each at a distinct z-order so pass pointers cannot invert their order.
    Ogre::Overlay* backdrop_overlay_ = nullptr;  // z 519: full-screen tiled background
    Ogre::Overlay* art_overlay_ = nullptr;       // z 520: the cinematic's artwork
    Ogre::Overlay* text_bg_overlay_ = nullptr;   // z 521: the text window's frame
    Ogre::Overlay* text_overlay_ = nullptr;      // z 522: crawling dialogue text
    Ogre::Overlay* bars_overlay_ = nullptr;      // z 523: top and bottom ornate borders
    Ogre::Overlay* button_overlay_ = nullptr;    // z 524: continue button

    // Textures & materials
    tl::formats::Imageset art_set_;
    tl::formats::Imageset loading_set_;
    tl::formats::Imageset logo_set_;
    tl::formats::Imageset hud_set_;
    tl::formats::Imageset frame_set_;  ///< `widgets1`, the frame's nine slices

    Ogre::TexturePtr art_texture_;
    Ogre::TexturePtr loading_texture_;
    Ogre::TexturePtr logo_texture_;
    Ogre::TexturePtr hud_texture_;
    Ogre::TexturePtr frame_texture_;  ///< shared with the other screens: not removed

    Ogre::MaterialPtr art_material_;
    Ogre::MaterialPtr loading_material_;
    Ogre::MaterialPtr logo_material_;
    Ogre::MaterialPtr hud_material_;
    Ogre::MaterialPtr frame_material_;

    // Overlay elements
    std::vector<Ogre::OverlayElement*> panels_;
    std::vector<TextLine> lines_;

    // Continue button
    Ogre::PanelOverlayElement* button_panel_ = nullptr;
    Ogre::TextAreaOverlayElement* button_label_ = nullptr;
    Ogre::TextAreaOverlayElement* button_shadow_ = nullptr;
    bool button_hovered_ = false;

    // `DialogTextFrame`, the rectangle the text is clipped to, in relative
    // units (0..1), and `DialogText`'s height in pixels.
    float box_left_rel_ = 0.0f;
    float box_top_rel_ = 0.0f;
    float box_w_rel_ = 0.0f;
    float box_h_rel_ = 0.0f;
    float window_height_px_ = 0.0f;
    float line_height_rel_ = 0.0f;
    float line_step_rel_ = 0.0f;

    bool confirm_held_ = false;
    bool skip_held_ = false;
};

}  // namespace tl::render
