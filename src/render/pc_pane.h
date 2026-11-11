// A PC side pane's background. The PC draws each pane (stats, inventory, skills ...) as a
// small model, `media/UI/models/<pane>/<pane>.MESH`: two quads, a top and a bottom half,
// in the pixels of a 1024 x 768 screen measured from its centre. This lays the same two
// quads flat, against the screen's left or right edge.
#pragma once

#include <OgreMaterial.h>
#include <OgrePrerequisites.h>
#include <OgreTexture.h>

#include <string>
#include <vector>

namespace Ogre {
class Overlay;
class OverlayContainer;
class PanelOverlayElement;
}  // namespace Ogre

namespace tl::formats {
struct OgreMeshVertices;
}

namespace tl::render {

class PcPane {
public:
    PcPane() = default;
    ~PcPane();
    PcPane(const PcPane&) = delete;
    PcPane& operator=(const PcPane&) = delete;

    /// Build the pane of `model` (`character`, `inventory`, ...) under overlays named `tag`,
    /// drawn at `z_order`; `right` puts it against the right edge. False when the model is
    /// not in the mounted data.
    // DIVERGENCE: the halves stand where the model's open pose has them; its slide in and
    // out (`open.SKELETON`, `close.SKELETON`) is not played.
    bool create(const std::string& group, const std::string& model, bool right, unsigned width,
                unsigned height, const std::string& tag, unsigned short z_order);
    void destroy();
    bool ready() const { return overlay_ != nullptr; }
    void show();
    void hide();
    void resize(unsigned width, unsigned height);

private:
    /// One half: where it is in the model's pixels, and what of its texture it draws.
    struct Quad {
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;  ///< model pixels, y up
        float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
        float depth = 0.0f;
        Ogre::PanelOverlayElement* panel = nullptr;
        Ogre::MaterialPtr material;
        Ogre::TexturePtr texture;
    };
    void layout();
    static void box_of(const tl::formats::OgreMeshVertices& v, Quad& quad);

    std::vector<Quad> quads_;
    Ogre::Overlay* overlay_ = nullptr;
    Ogre::OverlayContainer* root_ = nullptr;
    bool right_ = false;
    unsigned width_ = 0;
    unsigned height_ = 0;
};

}  // namespace tl::render
