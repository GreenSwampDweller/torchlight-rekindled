#include "render/cinematic_menu.h"

#include "core/audio.h"
#include "core/input.h"
#include "core/log.h"
#include "core/text.h"
#include "formats/adm.h"
#include "render/menu_chrome.h"
#include "render/overlay_set.h"
#include "render/text_layout.h"

#include <OgreFontManager.h>
#include <OgreHardwarePixelBuffer.h>
#include <OgreMaterialManager.h>
#include <OgreOverlay.h>
#include <OgreOverlayContainer.h>
#include <OgreOverlayManager.h>
#include <OgrePanelOverlayElement.h>
#include <OgrePass.h>
#include <OgreResourceGroupManager.h>
#include <OgreTechnique.h>
#include <OgreTextAreaOverlayElement.h>
#include <OgreTextureManager.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace tl::render {
namespace {

constexpr const char* kLoadingImageset = "media/UI/loading.imageset";
constexpr const char* kWidgets1Imageset = "media/UI/icons2/widgets1.imageset";

struct Slice {
    float x, y, w, h;
};

/// A `FrameComponent`'s nine pieces over `x, y, w, h`, in `FrameLook::image` order (the four
/// corners at their own size, then the left, right, top and bottom edges, then the middle).
std::array<Slice, 9> frame_slices(float x, float y, float w, float h, float cw, float ch) {
    const float mw = std::max(0.0f, w - 2.0f * cw);
    const float mh = std::max(0.0f, h - 2.0f * ch);
    const float r = x + w - cw;
    const float b = y + h - ch;
    return {{{x, y, cw, ch},
             {r, y, cw, ch},
             {x, b, cw, ch},
             {r, b, cw, ch},
             {x, y + ch, cw, mh},
             {r, y + ch, cw, mh},
             {x + cw, y, mw, ch},
             {x + cw, b, mw, ch},
             {x + cw, y + ch, mw, mh}}};
}

}  // namespace

CinematicMenu::~CinematicMenu() { destroy(); }

void CinematicMenu::set_uv(Ogre::PanelOverlayElement* panel, const tl::formats::Imageset& set,
                           const Ogre::TexturePtr& texture, const std::string& image) const {
    if (!panel || !texture) return;
    const tl::formats::ImagesetImage* rect = set.find(image);
    if (!rect) return;
    const float tw = static_cast<float>(texture->getWidth());
    const float th = static_cast<float>(texture->getHeight());
    if (tw <= 0.0f || th <= 0.0f) return;
    panel->setUV(static_cast<float>(rect->x) / tw, static_cast<float>(rect->y) / th,
                 static_cast<float>(rect->x + rect->width) / tw,
                 static_cast<float>(rect->y + rect->height) / th);
}

// CCinematicMenu::setText: the art is the
// `image` of the cinematic's own imageset, the text has its literal backslash-n
// turned into newlines, and the crawl lasts 0.015 s a byte and never under 10
// s.
namespace {

// An imageset the screen can do without: the Xbox 360 pak carries neither
// `logo.imageset` nor `hud.imageset`'s art. Not `ok` when it is not there.
tl::formats::Imageset read_optional_imageset(const char* file, const std::string& group) {
    // Absent is expected, so it is not read (read_menu_resource() warns).
    if (!Ogre::ResourceGroupManager::getSingleton().resourceExists(group, file)) return {};
    std::vector<uint8_t> bytes;
    if (!read_menu_resource(file, group, bytes)) return {};
    return tl::formats::parse_imageset(bytes.data(), bytes.size());
}

// Its texture, null when it is missing: the Xbox 360 pak keeps `hud.imageset`
// but not its `ui1.dds`, and a texture missing is a frame left undrawn.
Ogre::TexturePtr load_optional_texture(const tl::formats::Imageset& set, const std::string& group) {
    if (!set.ok) return {};
    try {
        return Ogre::TextureManager::getSingleton().load(set.image_file, group);
    } catch (const Ogre::Exception& e) {
        TL_LOG_INFO("cinematic: no %s (%s); its pieces are not drawn", set.image_file.c_str(),
                    e.getDescription().c_str());
        return {};
    }
}

}  // namespace

bool CinematicMenu::create(const std::string& group, unsigned width, unsigned height,
                           const tl::core::Cinematic& cinematic) {
    destroy();
    width_ = width;
    height_ = height;
    name_ = cinematic.name;

    narrative_text_ = tl::core::cinematic_display_text(cinematic.text);
    crawl_ = tl::core::CinematicCrawl{};
    crawl_.duration = tl::core::cinematic_crawl_seconds(narrative_text_);

    // Load imagesets
    std::vector<uint8_t> bytes;
    if (!read_menu_resource(cinematic.image, group, bytes)) {
        destroy();
        return false;
    }
    art_set_ = tl::formats::parse_imageset(bytes.data(), bytes.size());

    if (!read_menu_resource(kLoadingImageset, group, bytes)) {
        destroy();
        return false;
    }
    loading_set_ = tl::formats::parse_imageset(bytes.data(), bytes.size());

    // The PC's frame - the ornate bars (`logo.imageset`) and the Continue
    // button's art (`hud.imageset`) - only where the archive has them: the Xbox
    // 360 pak carries neither, and a screen without them still shows the art,
    // the crawl and the button's caption, which is what the story needs. Every
    // panel below is skipped when its image is not there.
    logo_set_ = read_optional_imageset(kLogoImageset, group);
    hud_set_ = read_optional_imageset(kGuiImageset, group);
    frame_set_ = read_optional_imageset(kWidgets1Imageset, group);

    if (!art_set_.ok || !loading_set_.ok) {
        destroy();
        return false;
    }

    if (!ensure_menu_font()) {
        destroy();
        return false;
    }

    try {
        auto& textures = Ogre::TextureManager::getSingleton();
        art_texture_ = textures.load(art_set_.image_file, group);
        loading_texture_ = textures.load(loading_set_.image_file, group);
        logo_texture_ = load_optional_texture(logo_set_, group);
        hud_texture_ = load_optional_texture(hud_set_, group);
        frame_texture_ = load_optional_texture(frame_set_, group);

        if (!art_texture_ || !loading_texture_) {
            destroy();
            return false;
        }

        art_material_ = build_menu_material("tl/intro/art", art_texture_);
        loading_material_ = build_menu_material("tl/intro/loading", loading_texture_);
        if (logo_texture_) logo_material_ = build_menu_material("tl/intro/logo", logo_texture_);
        if (hud_texture_) hud_material_ = build_menu_material("tl/intro/hud", hud_texture_);

        if (frame_texture_) {
            frame_material_ = build_menu_material("tl/intro/frame", frame_texture_);
        }

        auto& overlays = Ogre::OverlayManager::getSingleton();
        backdrop_overlay_ = overlays.create("tl/intro/backdrop");
        backdrop_overlay_->setZOrder(519);

        art_overlay_ = overlays.create("tl/intro/art");
        art_overlay_->setZOrder(520);

        text_bg_overlay_ = overlays.create("tl/intro/text_bg");
        text_bg_overlay_->setZOrder(521);

        text_overlay_ = overlays.create("tl/intro/text");
        text_overlay_->setZOrder(522);

        bars_overlay_ = overlays.create("tl/intro/bars");
        bars_overlay_->setZOrder(523);

        button_overlay_ = overlays.create("tl/intro/button");
        button_overlay_->setZOrder(524);

        layout();

        backdrop_overlay_->show();
        art_overlay_->show();
        text_bg_overlay_->show();
        bars_overlay_->show();
        text_overlay_->show();
        button_overlay_->show();

        built_ = true;
        // The A that got here is not a press on this screen.
        confirm_held_ = skip_held_ = true;
        return true;
    } catch (const Ogre::Exception& e) {
        TL_LOG_WARN("cinematic: could not create overlays: %s", e.getDescription().c_str());
        destroy();
        return false;
    }
}

void CinematicMenu::layout() {
    auto& overlays = Ogre::OverlayManager::getSingleton();
    const float across = static_cast<float>(width_ ? width_ : 1);
    const float down = static_cast<float>(height_ ? height_ : 1);
    // `logo.imageset`'s height, else `loading.imageset`'s (768 on both releases).
    const float scale =
        down / authored_height(logo_set_.ok ? logo_set_ : loading_set_, kNativeHeight);

    size_t serial = 0;
    auto add_panel = [&](Ogre::Overlay* into, const tl::formats::Imageset& set,
                         const Ogre::MaterialPtr& material, const Ogre::TexturePtr& texture,
                         const char* image_name, float left, float top, float w, float h) {
        const tl::formats::ImagesetImage* img = set.find(image_name);
        if (!img || !texture) return static_cast<Ogre::PanelOverlayElement*>(nullptr);
        auto* p = static_cast<Ogre::PanelOverlayElement*>(
            overlays.createOverlayElement("Panel", "tl/intro/p_" + std::to_string(serial++)));
        p->setMetricsMode(Ogre::GMM_RELATIVE);
        p->setMaterial(material);
        p->setPosition(left / across, top / down);
        p->setDimensions(w / across, h / down);
        const float tw = static_cast<float>(texture->getWidth());
        const float th = static_cast<float>(texture->getHeight());
        p->setUV(static_cast<float>(img->x) / tw, static_cast<float>(img->y) / th,
                 static_cast<float>(img->x + img->width) / tw,
                 static_cast<float>(img->y + img->height) / th);
        into->add2D(p);
        panels_.push_back(p);
        return p;
    };

    // 1. Base dark tiled backdrop: full screen fill on its own overlay below the art
    add_panel(backdrop_overlay_, loading_set_, loading_material_, loading_texture_, "background",
              0.0f, 0.0f, across, down);

    // 2. The art: `LoadingB` at {{.5,0},{0,.5}}, 1024x725, and `Background`
    // inside it at {{-.5,0},{0,-.5}} - centred across, its top at the screen's.
    // `setText` gives `Background` the cinematic imageset's `image`.
    const char* art_img = "image";
    const float art_w = 1024.0f * scale;
    const float art_h = 725.0f * scale;
    const float art_x = across * 0.5f - art_w * 0.5f;
    const float art_y = 0.0f;
    add_panel(art_overlay_, art_set_, art_material_, art_texture_, art_img, art_x, art_y, art_w,
              art_h);

    // 3. The text window, `i_962` of `cinematic.uilayout`: a `ceguiWidget/ItemText` 900x200
    // at {0,464}, centred across. `DialogTextFrame` and `DialogText` fill it.
    const float box_w = 900.0f * scale;
    const float box_h = 200.0f * scale;
    const float box_x = across * 0.5f - box_w * 0.5f;
    const float box_top_y = 464.0f * scale;
    window_height_px_ = box_h;

    // Its `Enabled` imagery draws `StaticShared`'s `frame` behind the text: corners at their
    // own size, edges and middle stretched between them.
    if (const tl::formats::ImagesetImage* corner = frame_set_.find(kPanelFrame.image[0])) {
        const FrameLook& look = kPanelFrame;
        const std::array<Slice, 9> slices = frame_slices(
            box_x + look.left * scale, box_top_y + look.top * scale, box_w + look.grow_w * scale,
            box_h + look.grow_h * scale, static_cast<float>(corner->width) * scale,
            static_cast<float>(corner->height) * scale);
        for (size_t i = 0; i < slices.size(); ++i) {
            add_panel(text_bg_overlay_, frame_set_, frame_material_, frame_texture_, look.image[i],
                      slices[i].x, slices[i].y, slices[i].w, slices[i].h);
        }
    }

    // 4. Ornate framing bars: top and bottom
    for (const MenuBar& bar : kMenuBars) {
        const float w = bar.wide * scale;
        const float h = bar.tall * scale;
        const float x = bar.rel_x * across + bar.off_x * scale;
        const float y = bar.rel_y * down + bar.off_y * scale;
        add_panel(bars_overlay_, logo_set_, logo_material_, logo_texture_, bar.image, x, y, w, h);
    }

    // 4. Continue Button: in bottom bar
    // authored at {{.5, 251}, {1, -64}} with size {246, 44}
    const float btn_w = 246.0f * scale;
    const float btn_h = 44.0f * scale;
    const float btn_x = (across * 0.5f) + (251.0f * scale);
    const float btn_y = down - (64.0f * scale);

    button_panel_ = add_panel(button_overlay_, hud_set_, hud_material_, hud_texture_, kButtonNormal,
                              btn_x, btn_y, btn_w, btn_h);

    auto* btn_container = static_cast<Ogre::OverlayContainer*>(
        overlays.createOverlayElement("Panel", "tl/intro/btn_text_container"));
    btn_container->setMetricsMode(Ogre::GMM_RELATIVE);
    btn_container->setPosition(0.0f, 0.0f);
    btn_container->setDimensions(1.0f, 1.0f);
    button_overlay_->add2D(btn_container);
    panels_.push_back(btn_container);

    const float label_pts = 14.0f;
    const float label_h_rel = label_pts * ui_points_to_pixels() / kNativeHeight;
    const float shadow_off = kLabelShadow / down;
    const float btn_center_x = (btn_x + btn_w * 0.5f) / across;
    const float btn_center_y = (btn_y + (btn_h - label_h_rel * down) * 0.5f) / down;

    button_shadow_ = static_cast<Ogre::TextAreaOverlayElement*>(
        overlays.createOverlayElement("TextArea", "tl/intro/btn/a_shadow"));
    button_shadow_->setMetricsMode(Ogre::GMM_RELATIVE);
    button_shadow_->setFontName(kFontName, Ogre::RGN_INTERNAL);
    button_shadow_->setColour(Ogre::ColourValue(0.0f, 0.0f, 0.0f, 0.85f));
    set_char_height(button_shadow_, label_h_rel);
    button_shadow_->setAlignment(Ogre::TextAreaOverlayElement::Center);
    button_shadow_->setCaption("Continue");
    button_shadow_->setPosition(btn_center_x + shadow_off, btn_center_y + shadow_off);
    btn_container->addChild(button_shadow_);

    button_label_ = static_cast<Ogre::TextAreaOverlayElement*>(
        overlays.createOverlayElement("TextArea", "tl/intro/btn/b_label"));
    button_label_->setMetricsMode(Ogre::GMM_RELATIVE);
    button_label_->setFontName(kFontName, Ogre::RGN_INTERNAL);
    button_label_->setColour(Ogre::ColourValue(1.0f, 0.95f, 0.8f, 1.0f));
    set_char_height(button_label_, label_h_rel);
    button_label_->setAlignment(Ogre::TextAreaOverlayElement::Center);
    button_label_->setCaption("Continue");
    button_label_->setPosition(btn_center_x, btn_center_y);
    btn_container->addChild(button_label_);

    // 5. The crawling text: `DialogTextFrame` clips `DialogText`, a window of the frame's size
    // that `updateTextDisplay` slides up into it.
    box_left_rel_ = box_x / across;
    box_top_rel_ = box_top_y / down;
    box_w_rel_ = box_w / across;
    box_h_rel_ = box_h / down;

    auto* text_container = static_cast<Ogre::OverlayContainer*>(
        overlays.createOverlayElement("Panel", "tl/intro/text_container"));
    text_container->setMetricsMode(Ogre::GMM_RELATIVE);
    text_container->setPosition(0.0f, 0.0f);
    text_container->setDimensions(1.0f, 1.0f);
    text_overlay_->add2D(text_container);
    panels_.push_back(text_container);

    const auto& fonts = Ogre::FontManager::getSingleton();
    Ogre::FontPtr font = fonts.getByName(kFontName, Ogre::RGN_INTERNAL);

    // `DialogText`'s `Serif` (`Size="11"`). CEGUI steps a line by the face's FreeType height:
    // `BRLNSDB.TTF`'s hhea 1893 + 419 + 0 over its em of 2048.
    constexpr float kTextPoints = 11.0f;
    constexpr float kLineSpacingPerEm = 2312.0f / 2048.0f;
    line_height_rel_ = kTextPoints * points_to_char_pixels() / kNativeHeight;
    line_step_rel_ = kTextPoints * ui_points_to_pixels() * kLineSpacingPerEm / kNativeHeight;

    std::vector<std::string> wrapped_lines =
        wrap_text(font, narrative_text_, box_w_rel_, line_height_rel_, across, down);

    lines_.clear();
    lines_.reserve(wrapped_lines.size());

    // Each line's place below the text window's top.
    float cur_y_rel = 0.0f;
    for (size_t i = 0; i < wrapped_lines.size(); ++i) {
        const std::string& line_text = wrapped_lines[i];

        TextLine tl;
        tl.base_y_rel = cur_y_rel;

        if (!line_text.empty()) {
            tl.shadow = static_cast<Ogre::TextAreaOverlayElement*>(overlays.createOverlayElement(
                "TextArea", "tl/intro/t_" + std::to_string(i) + "/a_shadow"));
            tl.shadow->setMetricsMode(Ogre::GMM_RELATIVE);
            tl.shadow->setFontName(kFontName, Ogre::RGN_INTERNAL);
            tl.shadow->setColour(Ogre::ColourValue(0.0f, 0.0f, 0.0f, 0.8f));
            set_char_height(tl.shadow, line_height_rel_);
            tl.shadow->setAlignment(Ogre::TextAreaOverlayElement::Left);
            tl.shadow->setCaption(line_text);
            text_container->addChild(tl.shadow);

            tl.text = static_cast<Ogre::TextAreaOverlayElement*>(overlays.createOverlayElement(
                "TextArea", "tl/intro/t_" + std::to_string(i) + "/b_text"));
            tl.text->setMetricsMode(Ogre::GMM_RELATIVE);
            tl.text->setFontName(kFontName, Ogre::RGN_INTERNAL);
            tl.text->setColour(Ogre::ColourValue(1.0f, 1.0f, 1.0f, 1.0f));
            set_char_height(tl.text, line_height_rel_);
            tl.text->setAlignment(Ogre::TextAreaOverlayElement::Left);
            tl.text->setCaption(line_text);
            text_container->addChild(tl.text);
        }

        lines_.push_back(tl);
        cur_y_rel += line_step_rel_;
    }

    // The window is 200 tall and cuts whatever text runs past it, in the
    // original as here. Worth a line when this engine's font makes that happen.
    const float text_h_rel =
        lines_.empty() ? 0.0f : (lines_.size() - 1) * line_step_rel_ + line_height_rel_;
    if (text_h_rel > box_h_rel_) {
        TL_LOG_WARN("cinematic: '%s' wraps to %zu lines, %.0f px of text in a %.0f px window",
                    name_.c_str(), lines_.size(), static_cast<double>(text_h_rel * down),
                    static_cast<double>(box_h));
    }

    place_text(crawl_.step(0.0f, window_height_px_));
}

// CCinematicMenu::updateTextDisplay, the
// `setPosition` half: the text window's top sits `offset_pixels` below the
// frame's.
void CinematicMenu::place_text(float offset_pixels) {
    const float down = static_cast<float>(height_ ? height_ : 1);
    const float text_top_rel = box_top_rel_ + offset_pixels / down;

    for (TextLine& line : lines_) {
        if (!line.text) continue;

        const float y = text_top_rel + line.base_y_rel;

        // DIVERGENCE (drawing hookup): CEGUI clips the text to the frame's
        // rectangle, whose lower edge is above the bottom bar. An
        // OGRE overlay text area has no clip rectangle, so the text is drawn
        // whole and rises from behind the bar, which is drawn over it: the
        // same wipe, glyph row by glyph row, that much lower. A line still
        // wholly under the bar is not drawn at all. Nothing crosses the
        // frame's upper edge: the text comes to rest there.
        const float bar_top_rel = 1.0f - 89.0f * (down / kNativeHeight) / down;
        if (y >= bar_top_rel) {
            line.text->hide();
            if (line.shadow) line.shadow->hide();
            continue;
        }

        line.text->show();
        if (line.shadow) line.shadow->show();
        line.text->setColour(Ogre::ColourValue(1.0f, 1.0f, 1.0f, 1.0f));
        line.text->setPosition(box_left_rel_, y);

        if (line.shadow) {
            const float s_off = kLabelShadow / down;
            line.shadow->setColour(Ogre::ColourValue(0.0f, 0.0f, 0.0f, 0.85f));
            line.shadow->setPosition(box_left_rel_ + s_off, y + s_off);
        }
    }
}

void CinematicMenu::advance_crawl(float seconds) {
    place_text(crawl_.step(seconds, window_height_px_));
}

// CCinematicMenu::update, the crawl and the
// button. The music's share (`+0xf8`) is the renderer's, which outlives this
// screen.
bool CinematicMenu::update(float delta, const tl::core::InputState& input) {
    if (!built_) return true;

    // DIVERGENCE (a pad instead of a mouse): the original runs the crawl
    // tenfold while a mouse button is held anywhere, jumps it to its end on a
    // click, and closes on a click on `Skip` ("Continue", `guiContinueGame`).
    // Here A is that click: on a crawl still running it jumps to the end, on
    // one at rest it is the button. B and START are the button at once.
    const bool pressed = input.confirm && !confirm_held_;
    confirm_held_ = input.confirm;
    const bool was_finished = crawl_.finished();
    place_text(crawl_.step(
        tl::core::cinematic_crawl_delta(delta, false, pressed && !was_finished, crawl_.duration),
        window_height_px_));

    // The one button on the screen has the focus, so it is drawn lit - there
    // is nothing else a pad could move to.
    if (!button_hovered_) {
        button_hovered_ = true;
        set_uv(button_panel_, hud_set_, hud_texture_, kButtonHover);
        if (button_label_) button_label_->setColour(Ogre::ColourValue(1.0f, 1.0f, 1.0f, 1.0f));
    }

    bool triggered = pressed && was_finished;
    if (input.skip && !skip_held_) triggered = true;
    skip_held_ = input.skip;
    if (input.quit) triggered = true;

    // `CCinematicMenu::onClick` is the only
    // way out: the screen stays up after the crawl and the recording have
    // ended.
    return triggered;
}

void CinematicMenu::destroy() {
    // Adhering to project destructor rule: clean up each individual pointer unconditionally
    auto& overlays = Ogre::OverlayManager::getSingleton();

    for (TextLine& tl : lines_) {
        if (tl.text) {
            overlays.destroyOverlayElement(tl.text);
            tl.text = nullptr;
        }
        if (tl.shadow) {
            overlays.destroyOverlayElement(tl.shadow);
            tl.shadow = nullptr;
        }
    }
    lines_.clear();

    if (button_label_) {
        overlays.destroyOverlayElement(button_label_);
        button_label_ = nullptr;
    }
    if (button_shadow_) {
        overlays.destroyOverlayElement(button_shadow_);
        button_shadow_ = nullptr;
    }
    button_panel_ = nullptr;

    for (Ogre::OverlayElement* p : panels_) {
        if (p) overlays.destroyOverlayElement(p);
    }
    panels_.clear();

    if (button_overlay_) {
        overlays.destroy(button_overlay_);
        button_overlay_ = nullptr;
    }
    if (text_overlay_) {
        overlays.destroy(text_overlay_);
        text_overlay_ = nullptr;
    }
    if (bars_overlay_) {
        overlays.destroy(bars_overlay_);
        bars_overlay_ = nullptr;
    }
    if (text_bg_overlay_) {
        overlays.destroy(text_bg_overlay_);
        text_bg_overlay_ = nullptr;
    }
    if (art_overlay_) {
        overlays.destroy(art_overlay_);
        art_overlay_ = nullptr;
    }
    if (backdrop_overlay_) {
        overlays.destroy(backdrop_overlay_);
        backdrop_overlay_ = nullptr;
    }

    auto& materials = Ogre::MaterialManager::getSingleton();
    if (art_material_) {
        materials.remove(art_material_->getHandle());
        art_material_.reset();
    }
    if (loading_material_) {
        materials.remove(loading_material_->getHandle());
        loading_material_.reset();
    }
    if (logo_material_) {
        materials.remove(logo_material_->getHandle());
        logo_material_.reset();
    }
    if (hud_material_) {
        materials.remove(hud_material_->getHandle());
        hud_material_.reset();
    }
    if (frame_material_) {
        materials.remove(frame_material_->getHandle());
        frame_material_.reset();
    }

    auto& textures = Ogre::TextureManager::getSingleton();
    if (art_texture_) {
        textures.remove(art_texture_->getHandle());
        art_texture_.reset();
    }
    if (loading_texture_) {
        textures.remove(loading_texture_->getHandle());
        loading_texture_.reset();
    }
    if (logo_texture_) {
        textures.remove(logo_texture_->getHandle());
        logo_texture_.reset();
    }
    if (hud_texture_) {
        textures.remove(hud_texture_->getHandle());
        hud_texture_.reset();
    }
    frame_texture_.reset();

    built_ = false;
}

}  // namespace tl::render
