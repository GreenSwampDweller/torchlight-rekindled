// A console `[UI]` layout drawn as it stands, with the few things a screen's logic changes at
// runtime: a window's text, visibility, size and tint. Read as `DeathScreen` reads its layout;
// for the fishing ring (`media/UI/fishing_xenon.layout`, `CFishingMenu`) and the modal message
// (`media/UI/modalmenu_xenon.layout`, `CModalMenu`), which ship in the Xbox archive only.
#pragma once

#include "formats/cegui.h"
#include "formats/uilayout.h"

#include <OgreColourValue.h>
#include <OgreMaterial.h>
#include <OgrePrerequisites.h>
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

/// A window of a PC `.layout` (`UnifiedPosition` and `UnifiedSize`) in the console export's form
/// (`UnifiedAreaRect`), hidden until shown, to stand in a console screen that has none. Its
/// `Type` is unset when the layout or the window is not in the mounted data.
tl::formats::GuiWindow pc_layout_window(const std::string& group, const std::string& layout,
                                        const std::string& name);

class LayoutScreen {
public:
    LayoutScreen() = default;
    ~LayoutScreen();

    LayoutScreen(const LayoutScreen&) = delete;
    LayoutScreen& operator=(const LayoutScreen&) = delete;

    /// Build `layout` under overlays named `tag`, drawn at `z_order` (text at
    /// `z_order + 2`). False, with one logged line, when the console interface
    /// data is not mounted.
    bool create(const std::string& group, unsigned width, unsigned height,
                const std::string& layout, const std::string& tag, unsigned short z_order);
    /// The same for a screen the console ships only as its CEGUI export
    /// (`*.uilayout`: `characterload`, `mainmenu`, `difficulty`): each window's
    /// `UnifiedAreaRect`, alignment, text and image read as the `[UI]` source
    /// of the same screen would have said them. A window is found by its
    /// `Name`; one authored `Visible` `False` is hidden as `set_hidden` hides
    /// it, so the screen's own logic can show it.
    /// `pc_windows`, windows of the screen's PC twin (`characterload.layout` beside
    /// `characterload.uilayout`) the console's lacks, are added to its root (pc_layout_window).
    bool create_export(const std::string& group, unsigned width, unsigned height,
                       const std::string& layout, const std::string& tag, unsigned short z_order,
                       const std::vector<std::string>& pc_windows = {});
    void destroy();
    bool ready() const { return built_; }

    void show();
    void hide();
    bool visible() const { return visible_; }
    /// Draw it at `z_order` from now on (text at `z_order + 2`), as `create` would have.
    void set_z_order(unsigned short z_order);

    void resize(unsigned width, unsigned height);

    /// Whether the layout has a window of that `WIDGET NAME`.
    bool has(const std::string& widget) const { return find_widget(widget) >= 0; }
    /// A `Text` window's caption.
    void set_caption(const std::string& widget, const std::string& text);
    /// Shown or hidden, on top of the layout's own `VISIBLE`.
    void set_hidden(const std::string& widget, bool hidden);
    /// A window's size in layout pixels, in place of its `WIDTH` / `HEIGHT`.
    /// Anchors still apply, so a centred window stays centred.
    void set_extent(const std::string& widget, float wide, float tall);
    /// A text window's colour (`TextColour`).
    void set_text_colour(const std::string& widget, const Ogre::ColourValue& colour);
    /// A window's authored offset inside its parent in layout pixels; false
    /// when there is no such window.
    bool offset(const std::string& widget, float& x, float& y) const;
    /// An image window's colour, multiplied into its art. White is untinted.
    void set_tint(const std::string& widget, const Ogre::ColourValue& tint);
    /// A button drawn with its `HOVER` art rather than its `NORMAL`: the pad's
    /// highlight. A window that authors no `HOVER` keeps its art.
    void set_hover(const std::string& widget, bool hover);
    /// A `Check Box`'s tick (`ceguiWidget/Checkbox`'s `select_mark`), shown
    /// or not.
    void set_checked(const std::string& widget, bool checked);
    /// A window's offset inside its parent in layout pixels, in place of its
    /// `OFFSET X` / `OFFSET Y`: a slider's tab along its bar.
    void set_offset(const std::string& widget, float x, float y);
    /// A window's alignment in its parent in place of its `HORIZONTAL` /
    /// `VERTICLE`: 0 left or top, 1 centre, 2 right or bottom.
    void set_anchor(const std::string& widget, int horizontal, int vertical);
    /// A window's authored size in layout pixels; false when there is none.
    bool extent(const std::string& widget, float& wide, float& tall) const;

    /// Drawn above every other window of the screen.
    void raise(const std::string& widget);

    /// Apply the changes above.
    void refresh() { layout(); }

private:
    struct Node {
        float off_x = 0.0f, off_y = 0.0f;
        float wide = 0.0f, tall = 0.0f;
        float scale_wide = 0.0f, scale_tall = 0.0f;
        /// The share of the parent's size added to the offset: the scale half
        /// of an export's `UnifiedAreaRect` corner. A `[UI]` source has none.
        float scale_x = 0.0f, scale_y = 0.0f;
        int h_anchor = 0, v_anchor = 0;
        int parent = -1;
        /// For a slice of a frame, the window it frames; -1 otherwise.
        int frame_of = -1;

        Ogre::PanelOverlayElement* panel = nullptr;
        Ogre::TextAreaOverlayElement* text = nullptr;
        Ogre::TextAreaOverlayElement* drop = nullptr;

        std::string image;
        std::string hover;  ///< `HOVER`, a button's highlighted art
        std::string caption;
        float points = 0.0f;
        int h_text = 0;
        int v_text = 0;
        bool wrap = false;
        std::string widget;
        std::string name;  ///< `NAME`, what `find_widget` falls back on
        int tick = -1;     ///< a `Check Box`'s tick window
        bool hidden = false;
        bool runtime_hidden = false;
        Ogre::MaterialPtr tinted;  ///< this window's own material for `tint_colour`
        Ogre::ColourValue tint_colour = Ogre::ColourValue::White;
        bool tint_on = false;
        float alpha = 1.0f;       ///< `ALPHA`, a window's own opacity
        Ogre::MaterialPtr faded;  ///< this window's own material for `alpha`

        float left = 0.0f, top = 0.0f, width = 0.0f, height = 0.0f;
    };

    struct Sprite {
        const tl::formats::ImagesetImage* rect = nullptr;
        Ogre::TexturePtr texture;
        Ogre::MaterialPtr material;
    };

    bool load_art(const std::string& group);
    void build(const tl::formats::UiObject& object, int parent, size_t& serial,
               bool inherited_hidden);
    /// The nine `<art>*` slices of a `ceguiWidget/StaticShared` frame around
    /// `owner`: `tooltip` for `ItemText`, `frame` for `LineFrame`.
    void add_frame(int owner, size_t& serial, const std::string& art, float outset_x = 24.0f,
                   float outset_y = 9.0f);
    bool prepare(const std::string& group, unsigned width, unsigned height, const std::string& tag);
    bool construct(const std::vector<tl::formats::UiObject>& objects,
                   const std::string& layout_path, unsigned short z_order);
    void paint(Node& node, const std::string& image);
    void layout();
    bool is_hidden(int index) const;
    const Sprite* sprite(const std::string& name) const;
    int find_widget(const std::string& name) const;
    std::string element_name(size_t index, const char* suffix) const;

    std::vector<tl::formats::Imageset> sets_;
    std::vector<Ogre::TexturePtr> textures_;
    std::vector<Ogre::MaterialPtr> materials_;
    std::map<std::string, Sprite> sprites_;
    std::vector<Node> nodes_;

    Ogre::Overlay* overlay_ = nullptr;
    Ogre::Overlay* text_overlay_ = nullptr;
    Ogre::OverlayContainer* panel_root_ = nullptr;
    Ogre::OverlayContainer* text_root_ = nullptr;

    std::string tag_;
    unsigned width_ = 0;
    unsigned height_ = 0;
    float native_height_ = 0.0f;
    bool built_ = false;
    bool visible_ = false;
};

}  // namespace tl::render
