// Readers for Torchlight's CEGUI 0.6 interface files: imagesets and window layouts.
// Parsing only; Falagard (`GuiLook.looknfeel`) is not interpreted.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tl::formats {

// ---------------------------------------------------------------------------
// A minimal XML tree. Enough for these files and no more: elements, attributes
// and nesting. No text nodes, no namespaces, no DTD.
// ---------------------------------------------------------------------------

struct XmlElement {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<XmlElement> children;

    /// An attribute's value, or `fallback` when it is absent. A copy: callers
    /// pass a temporary fallback, which a reference would outlive.
    std::string attribute(const std::string& key, const std::string& fallback) const;
    bool has_attribute(const std::string& key) const;
};

struct XmlDocument {
    XmlElement root;
    bool ok = false;
    std::string error;
};

XmlDocument parse_xml(const uint8_t* data, size_t size);

// ---------------------------------------------------------------------------
// Imagesets: a texture plus named rectangles within it.
// ---------------------------------------------------------------------------

struct ImagesetImage {
    std::string name;
    int x = 0;
    int y = 0;
    /// Negative in the port's interface pack only: the rectangle drawn mirrored, `x` its right
    /// edge.
    int width = 0;
    int height = 0;
};

struct Imageset {
    /// The set's name, as a layout's `set:` prefix spells it - "Logo", "GuiLook".
    std::string name;

    /// The texture, as an archive path: `media/ui/logo.dds`.
    std::string image_file;

    /// The screen size the layouts' pixel offsets were authored against (1024x768 for
    /// all of Torchlight's). The rectangles below are texture pixels, unaffected.
    /// tl::render::MainMenu scales the interface by `viewportHeight / native_height`.
    int native_width = 640;
    int native_height = 480;
    bool auto_scaled = false;

    /// `Tint="RRGGBB"`, the port's interface pack only: every image multiplied by this colour.
    uint32_t tint = 0xFFFFFF;

    std::vector<ImagesetImage> images;

    /// A named rectangle, or nullptr. Matched case-insensitively.
    const ImagesetImage* find(const std::string& image_name) const;

    bool ok = false;
    std::string error;
};

Imageset parse_imageset(const uint8_t* data, size_t size);

// ---------------------------------------------------------------------------
// Window layouts: a tree of typed windows carrying named properties.
// ---------------------------------------------------------------------------

struct GuiWindow {
    /// The widget type, as the scheme maps it: "GuiLook/StandardButton".
    std::string type;
    std::string name;
    std::vector<std::pair<std::string, std::string>> properties;
    std::vector<GuiWindow> children;

    /// A property's value, or `fallback` when it is absent (a copy, as `attribute`).
    std::string property(const std::string& key, const std::string& fallback) const;
    bool has_property(const std::string& key) const;
};

struct GuiLayout {
    GuiWindow root;
    bool ok = false;
    std::string error;

    /// Depth-first search by window name, exact match. Null when absent.
    const GuiWindow* find(const std::string& window_name) const;
};

GuiLayout parse_gui_layout(const uint8_t* data, size_t size);

// ---------------------------------------------------------------------------
// UDim: CEGUI's "a fraction of the parent, plus a number of pixels".
// ---------------------------------------------------------------------------

struct UDim {
    float scale = 0.0f;
    float offset = 0.0f;

    /// Resolve against a parent extent of `parent` pixels, with pixel offsets
    /// scaled by `pixel_scale`. CEGUI itself uses raw pixels; the game scales them.
    float resolve(float parent, float pixel_scale) const {
        return scale * parent + offset * pixel_scale;
    }
};

struct UVector2 {
    UDim x;
    UDim y;
};

/// Parse `{{0.5,-507},{1,-64}}`. False when it is not that shape.
bool parse_uvector2(const std::string& text, UVector2& out);

/// Split `set:Logo image:topbar` into its two halves. False when it is not that
/// shape - an empty `Image` property is normal and means "draw nothing".
bool parse_image_reference(const std::string& text, std::string& set, std::string& image);

/// Parse CEGUI's `AARRGGBB` hex colour into four 0..1 components.
bool parse_colour(const std::string& text, float& r, float& g, float& b, float& a);

}  // namespace tl::formats
