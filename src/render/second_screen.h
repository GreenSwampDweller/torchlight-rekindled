// PORT: what a second display (a two-screen handheld's other screen) shows besides the map:
// the skill slots (GameHud::set_second_screen) over a backdrop of its own.
//
// OGRE draws every visible overlay in every viewport that has overlays on, so the two screens
// are told apart here: for the second window's viewport the overlays up on the main screen are
// hidden and this screen's own shown, and put back after it.
//
// The backdrop is drawn, not loaded: a bezel of ticks round the map and a scatter of ember
// specks in the corners, on black. Nearly every pixel is off, for an OLED panel.
#pragma once

#include <OgreMaterial.h>
#include <OgreRenderTargetListener.h>
#include <OgreTexture.h>

#include <vector>

namespace Ogre {
class Overlay;
class PanelOverlayElement;
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

private:
    void preViewportUpdate(const Ogre::RenderTargetViewportEvent& evt) override;
    void postViewportUpdate(const Ogre::RenderTargetViewportEvent& evt) override;

    void build_backdrop(unsigned width, unsigned height, const Shape& shape);
    void destroy_backdrop();

    Ogre::RenderWindow* window_ = nullptr;
    GameHud* hud_ = nullptr;

    Ogre::Overlay* backdrop_ = nullptr;
    Ogre::PanelOverlayElement* panel_ = nullptr;
    Ogre::TexturePtr texture_;
    Ogre::MaterialPtr material_;
    unsigned backdrop_width_ = 0;
    unsigned backdrop_height_ = 0;
    Shape backdrop_shape_;

    /// Between the two viewport events: the main screen's overlays, hidden for this viewport,
    /// and this screen's, shown for it.
    std::vector<Ogre::Overlay*> hidden_;
    std::vector<Ogre::Overlay*> shown_;
    bool swapped_ = false;
};

}  // namespace tl::render
