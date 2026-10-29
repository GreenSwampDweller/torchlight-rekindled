// The loading screen: the console's `loading.uilayout`, or the PC's artwork,
// bars and tip; split from menu.cpp.
#include <OgreFont.h>
#include <OgreFontManager.h>
#include <OgreMaterialManager.h>
#include <OgreOverlay.h>
#include <OgreOverlayManager.h>
#include <OgrePanelOverlayElement.h>
#include <OgrePass.h>
#include <OgreResourceGroupManager.h>
#include <OgreTechnique.h>
#include <OgreTextAreaOverlayElement.h>
#include <OgreTexture.h>
#include <OgreTextureManager.h>
#include <OgreTextureUnitState.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "core/log.h"
#include "core/text.h"
#include "formats/adm.h"
#include "render/layout_screen.h"
#include "render/menu.h"
#include "render/menu_chrome.h"
#include "render/overlay_set.h"
#include "render/text_layout.h"

namespace tl::render {

constexpr const char* kLoadingImageset = "media/UI/loading.imageset";
/// The console's loading screen: black, the artwork, the hourglass, the top
/// border and the tip in its framed strip (`TipText`).
constexpr const char* kConsoleLoadingFile = "media/UI/loading.uilayout";
/// What a backdrop leaves out of it: the hourglass, the "Loading" box and its text, the tip
/// strip and its text. And the artwork (`i_404`) dimmed, so the menu's text reads over it.
constexpr std::array<const char*, 5> kNotOnBackdrop = {"HOURGLASS", "i_407", "i_408", "i_402",
                                                       "TipText"};

/// Where the loading screen's tips are authored: `[LOADINGTIPS]`'s `<TRANSLATE>TIP:` entries,
/// read by `CGameGlobals::reload()`; `CGameUI::setLoadingVisible(true)` puts one in `TipText`
/// through `CGameGlobals::getRandomTip()`.
constexpr const char* kGlobalsFile = "media/GLOBALS.DAT.adm";
constexpr const char* kTipsGroup = "LOADINGTIPS";
constexpr const char* kTipKey = "TIP";

/// How tall the tip is drawn, as a fraction of the viewport height: `TipText`'s `Serif` font is
/// `BRLNSDB.TTF` at `Size="11"`, autoscaled against `NativeVertRes="768"` (`kPointsToPixels`).

LoadingScreen::~LoadingScreen() { destroy(); }

void LoadingScreen::load_tips(const std::string& group) {
    if (!tips_.empty()) return;
    std::vector<uint8_t> bytes;
    if (!read_menu_resource(kGlobalsFile, group, bytes) || bytes.empty()) return;

    const tl::formats::AdmFile globals = tl::formats::parse_adm(bytes.data(), bytes.size());
    if (!globals.ok) {
        TL_LOG_WARN("loading screen: %s: %s", kGlobalsFile, globals.error.c_str());
        return;
    }
    // `CGameGlobals::reload`: every `TIP` of
    // the first `[LOADINGTIPS]` group, in its order, one choice of odds 1 each
    // in the globals' tip randomizer (`CGameGlobals +0x200`, a normal
    // `CRandomizer`; X360 +0x4a0), an empty one too.
    bool read = false;
    const bool fill = tip_choices().choice_count() == 0;
    globals.visit([&](const tl::formats::AdmSection& section) {
        if (read || section.name != kTipsGroup) return;
        read = true;
        for (const tl::formats::AdmRecord& record : section.records) {
            if (record.key != kTipKey) continue;
            if (fill) tip_choices().add_choice(static_cast<int32_t>(tips_.size()), 1);
            // PORT: the tips carry the game's own colour runs -
            // `|cFFFFBA00[ALT]|u` - and nothing here draws per-run colours, so
            // they come out.
            tips_.push_back(tl::core::strip_markup(record.text));
        }
    });
    TL_LOG_INFO("loading screen: %zu tip(s) from %s", tips_.size(), kGlobalsFile);
}

namespace {
bool g_tips_pinned = false;
}  // namespace

// The globals' tip randomizer. The console's lives in its one `CGameGlobals`;
// `Renderer::restart()` builds a new `LoadingScreen`, so it is the process's
// here, filled by the first `load_tips`.
tl::core::Randomizer& LoadingScreen::tip_choices() {
    static tl::core::Randomizer choices;
    return choices;
}

// DIVERGENCE: a run whose frame is compared with another (`--loading-dump`)
// shows the first tip; the draw is still made, so `g_Rand` moves as the
// console's does.
void LoadingScreen::pin_tips() { g_tips_pinned = true; }

// `CGameUI::setLoadingVisible(true)`, the tip:
// `CGameGlobals::getRandomTip()` (inline on the console), the
// randomizer's `CRandomizer::getRandom()` on `g_Rand`, as an
// index into the tips.
void LoadingScreen::choose_tip() {
    if (tips_.empty() || (console_ == nullptr && tip_ == nullptr)) return;
    const int32_t drawn = tip_choices().get_random(tl::core::game_random());
    size_t which =
        drawn >= 0 && static_cast<size_t>(drawn) < tips_.size() ? static_cast<size_t>(drawn) : 0;
    if (g_tips_pinned) which = 0;
    // The console screen's own text window wraps and centres it as authored.
    if (console_ != nullptr) {
        console_->set_caption("TipText", tips_[which]);
        console_->refresh();
        TL_LOG_INFO("loading screen: tip %zu of %zu, \"%s\"", which + 1, tips_.size(),
                    tips_[which].c_str());
        return;
    }

    const float down = static_cast<float>(height_ ? height_ : 1);
    const float across = static_cast<float>(width_ ? width_ : 1);
    const float native =
        logo_set_.native_height > 0 ? static_cast<float>(logo_set_.native_height) : kNativeHeight;
    const float scale = down / native;

    // `TipText` spans the full width and the bottom bar's height (`{{0,0},{1,-89}}`,
    // `{{1,0},{0,89}}`), `WordWrapCentreAligned` and `CentreAligned`: wrapped at the whole width,
    // centred in the bar.
    const std::vector<std::string> lines =
        wrap_text(tip_->getFont(), tips_[which], 1.0f,
                  kFontSize * points_to_char_pixels() / kNativeHeight, across, down);
    std::string body;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) body += "\n";
        body += lines[i];
    }
    const float bar_top = down - 89.0f * scale;
    const float block = kFontSize * points_to_char_pixels() / kNativeHeight * 1.35f *
                        static_cast<float>(lines.size());
    tip_->setPosition(0.5f, (bar_top + (89.0f * scale - block * down) * 0.5f) / down);
    set_char_height(tip_, kFontSize * points_to_char_pixels() / kNativeHeight);
    tip_->setCaption(body);
    tip_->show();
    TL_LOG_INFO("loading screen: tip %zu of %zu, \"%s\"", which + 1, tips_.size(),
                tips_[which].c_str());
}

// The console's screen, `media/UI/loading.uilayout`, when its art is there. It
// draws nothing of the PC's (no `logo.imageset`, which the Xbox 360 pak does
// not carry), so it is tried first.
bool LoadingScreen::build_console(const std::string& group) {
    // Absent beside a PC pak (the interface pack carries none): the PC's screen is drawn instead.
    if (!Ogre::ResourceGroupManager::getSingleton().resourceExists(group, kConsoleLoadingFile)) {
        TL_LOG_INFO("loading screen: no %s, the PC's screen instead", kConsoleLoadingFile);
        return false;
    }
    console_ = std::make_unique<LayoutScreen>();
    if (!console_->create_export(group, width_, height_, kConsoleLoadingFile, "tl/loading/console",
                                 kZOrder)) {
        console_.reset();
        return false;
    }
    load_tips(group);
    TL_LOG_INFO("loading screen: the console screen, from %s, at %u x %u", kConsoleLoadingFile,
                width_, height_);
    return true;
}

// The PC screen's tip text, in the bars' overlay.
void LoadingScreen::build_tip() {
    auto& overlays = Ogre::OverlayManager::getSingleton();
    // `Overlay::add2D` takes an `OverlayContainer`, not a text area: a material-less `Panel` at
    // the origin is the frame to position in, in plain viewport units.
    tip_box_ = static_cast<Ogre::OverlayContainer*>(
        overlays.createOverlayElement("Panel", "tl/loading/tipbox"));
    tip_box_->setMetricsMode(Ogre::GMM_RELATIVE);
    tip_box_->setPosition(0.0f, 0.0f);
    tip_box_->setDimensions(1.0f, 1.0f);
    bars_->add2D(tip_box_);

    tip_ = static_cast<Ogre::TextAreaOverlayElement*>(
        overlays.createOverlayElement("TextArea", "tl/loading/tip"));
    tip_->setMetricsMode(Ogre::GMM_RELATIVE);
    tip_->setFontName(kFontName, Ogre::RGN_INTERNAL);
    set_char_height(tip_, kFontSize * points_to_char_pixels() / kNativeHeight);
    tip_->setAlignment(Ogre::TextAreaOverlayElement::Center);
    // `TextColour` is `FFFFFFFF` in the layout: plain white.
    tip_->setColour(Ogre::ColourValue::White);
    tip_box_->addChild(tip_);
    tip_->hide();
}

bool LoadingScreen::show(const std::string& group, unsigned width, unsigned height) {
    if (built_ && (width != width_ || height != height_)) destroy();
    width_ = width;
    height_ = height;

    if (!built_) built_ = build_console(group);
    if (!built_) {
        std::vector<uint8_t> bytes;
        // Read for its structure (the artwork 1024x725 centred, the menu's bars, the tip in the
        // lower bar); if it is not in the archive, neither are the images.
        if (!read_menu_resource(kLoadingImageset, group, bytes)) return false;
        loading_set_ = tl::formats::parse_imageset(bytes.data(), bytes.size());
        if (!read_menu_resource(kLogoImageset, group, bytes)) return false;
        logo_set_ = tl::formats::parse_imageset(bytes.data(), bytes.size());
        if (!loading_set_.ok || !logo_set_.ok) return false;

        try {
            auto& textures = Ogre::TextureManager::getSingleton();
            loading_texture_ = textures.load(loading_set_.image_file, group);
            logo_texture_ = textures.load(logo_set_.image_file, group);
            if (!loading_texture_ || !logo_texture_) return false;

            Ogre::MaterialPtr loading_material =
                build_menu_material("tl/loading/" + loading_set_.name, loading_texture_);
            Ogre::MaterialPtr logo_material =
                build_menu_material("tl/loading/" + logo_set_.name, logo_texture_);

            auto& overlays = Ogre::OverlayManager::getSingleton();
            overlay_ = overlays.create("tl/loading");
            // Over the menu's three layers and under the HUD.
            overlay_->setZOrder(kZOrder);
            // The bars get an overlay of their own: within one overlay OGRE sorts transparent
            // renderables by pass, and the artwork and bars use different textures.
            bars_ = overlays.create("tl/loading/bars");
            bars_->setZOrder(kZOrder + 1);

            const float across = static_cast<float>(width_ ? width_ : 1);
            const float down = static_cast<float>(height_ ? height_ : 1);
            const float scale = down / authored_height(logo_set_, kNativeHeight);

            size_t serial = 0;
            auto add = [&](Ogre::Overlay* into, const tl::formats::Imageset& set,
                           const Ogre::MaterialPtr& material, const Ogre::TexturePtr& texture,
                           const char* image_name, float left, float top, float w, float h) {
                const tl::formats::ImagesetImage* image = set.find(image_name);
                if (image == nullptr) return;
                auto* panel = static_cast<Ogre::PanelOverlayElement*>(overlays.createOverlayElement(
                    "Panel", "tl/loading/" + std::to_string(serial++)));
                panel->setMetricsMode(Ogre::GMM_RELATIVE);
                panel->setMaterial(material);
                panel->setPosition(left / across, top / down);
                panel->setDimensions(w / across, h / down);
                const float tw = static_cast<float>(texture->getWidth());
                const float th = static_cast<float>(texture->getHeight());
                panel->setUV(static_cast<float>(image->x) / tw, static_cast<float>(image->y) / th,
                             static_cast<float>(image->x + image->width) / tw,
                             static_cast<float>(image->y + image->height) / th);
                into->add2D(panel);
                panels_.push_back(panel);
            };

            // `Backdrop`: {{0,0},{0,0}} size {{1,0},{1,0}} - the 64x64
            // `background` patch stretched over everything.
            add(overlay_, loading_set_, loading_material, loading_texture_, "background", 0.0f,
                0.0f, across, down);

            // `LoadingB`/`Loading`: a 1024x725 image centred both ways, the
            // same container-plus-child idiom the menu's bars use.
            const float art_w = 1024.0f * scale;
            const float art_h = 725.0f * scale;
            add(overlay_, loading_set_, loading_material, loading_texture_, "loading",
                (across - art_w) * 0.5f, (down - art_h) * 0.5f, art_w, art_h);

            // The bars, exactly as `mainmenuframe.layout` places them - this
            // layout repeats them verbatim.
            const float bar_w = 1024.0f * scale;
            add(bars_, logo_set_, logo_material, logo_texture_, "topbar", (across - bar_w) * 0.5f,
                0.0f, bar_w, 140.0f * scale);
            add(bars_, logo_set_, logo_material, logo_texture_, "topbarleft", 0.0f, 0.0f,
                225.0f * scale, 88.0f * scale);
            add(bars_, logo_set_, logo_material, logo_texture_, "topbarright",
                across - 225.0f * scale, 0.0f, 225.0f * scale, 88.0f * scale);
            add(bars_, logo_set_, logo_material, logo_texture_, "bottombar",
                (across - bar_w) * 0.5f, down - 89.0f * scale, bar_w, 89.0f * scale);
            add(bars_, logo_set_, logo_material, logo_texture_, "bottombarleft", 0.0f,
                down - 89.0f * scale, 225.0f * scale, 89.0f * scale);
            add(bars_, logo_set_, logo_material, logo_texture_, "bottombarright",
                across - 225.0f * scale, down - 89.0f * scale, 225.0f * scale, 89.0f * scale);

            // The tip on the bottom bar: full width, bar height, wrapped and centred both ways,
            // `AlwaysOnTop`, so in the bars' overlay above both.
            load_tips(group);
            if (!tips_.empty() && ensure_menu_font()) build_tip();

            built_ = true;
            TL_LOG_INFO("loading screen: %zu piece(s) from %s at %u x %u", panels_.size(),
                        loading_set_.image_file.c_str(), width_, height_);
        } catch (const Ogre::Exception& e) {
            TL_LOG_WARN("no loading screen: %s", e.getDescription().c_str());
            destroy();
            return false;
        }
    }

    // A new tip each time the screen comes up, not each time show() is called:
    // the renderer paints it again on every `load_layout()` and would otherwise
    // step through the list within one load.
    if (!visible_) choose_tip();

    if (console_ != nullptr) console_->show();
    for (Ogre::Overlay* overlay : {overlay_, bars_}) {
        if (overlay != nullptr) overlay->show();
    }
    visible_ = true;
    return true;
}

bool LoadingScreen::show_backdrop(const std::string& group, unsigned width, unsigned height) {
    // Up as though already showing, so no tip is drawn: its roll is on `g_Rand`, which a screen
    // of the port's own must not move.
    const bool was_visible = visible_;
    visible_ = true;
    if (!show(group, width, height)) {
        visible_ = was_visible;
        return false;
    }
    set_z_order(kBackdropZOrder);
    for (const char* window : kNotOnBackdrop) {
        if (console_ != nullptr) console_->set_hidden(window, true);
    }
    if (console_ != nullptr) console_->set_tint("i_404", kBackdropDim);
    dim_pc_artwork(true);
    if (console_ != nullptr) console_->refresh();
    if (tip_ != nullptr) tip_->hide();
    backdrop_ = true;
    return true;
}

// The PC screen's fill and artwork (the panels drawn from `loading.texture`), on a dimmed copy of
// their material, or back on their own.
void LoadingScreen::dim_pc_artwork(bool dim) {
    if (!dim) {
        for (auto& [panel, own] : undimmed_) panel->setMaterial(own);
        undimmed_.clear();
        return;
    }
    if (!loading_texture_ || !undimmed_.empty()) return;
    Ogre::MaterialPtr dimmed = build_menu_material("tl/loading/backdrop", loading_texture_);
    dimmed->getTechnique(0)->getPass(0)->getTextureUnitState(0)->setColourOperationEx(
        Ogre::LBX_MODULATE, Ogre::LBS_TEXTURE, Ogre::LBS_MANUAL, Ogre::ColourValue::White,
        kBackdropDim);
    for (Ogre::PanelOverlayElement* panel : panels_) {
        if (panel == nullptr) continue;
        const Ogre::MaterialPtr& own = panel->getMaterial();
        const Ogre::TextureUnitState* unit =
            own ? own->getTechnique(0)->getPass(0)->getTextureUnitState(0) : nullptr;
        if (unit == nullptr || unit->_getTexturePtr() != loading_texture_) continue;
        undimmed_.emplace_back(panel, own);
        panel->setMaterial(dimmed);
    }
}

void LoadingScreen::set_z_order(unsigned short z_order) {
    if (console_ != nullptr) console_->set_z_order(z_order);
    if (overlay_ != nullptr) overlay_->setZOrder(z_order);
    if (bars_ != nullptr) bars_->setZOrder(static_cast<unsigned short>(z_order + 1));
}

void LoadingScreen::hide() {
    if (console_ != nullptr) console_->hide();
    for (Ogre::Overlay* overlay : {overlay_, bars_}) {
        if (overlay != nullptr) overlay->hide();
    }
    visible_ = false;
    hidden_backdrop_ = backdrop_;
    if (backdrop_) {
        set_z_order(kZOrder);
        for (const char* window : kNotOnBackdrop) {
            if (console_ != nullptr) console_->set_hidden(window, false);
        }
        if (console_ != nullptr) console_->set_tint("i_404", Ogre::ColourValue::White);
        dim_pc_artwork(false);
        if (console_ != nullptr) console_->refresh();
        backdrop_ = false;
    }
}

void LoadingScreen::destroy() {
    console_.reset();  // ~LayoutScreen() destroys it
    if (overlay_ != nullptr || bars_ != nullptr) {
        auto& overlays = Ogre::OverlayManager::getSingleton();
        hide();
        for (Ogre::PanelOverlayElement* panel : panels_) {
            if (panel == nullptr) continue;
            // The panel belongs to one of the two overlays; removing from the
            // other is a no-op, so both are asked.
            for (Ogre::Overlay* overlay : {overlay_, bars_}) {
                if (overlay != nullptr) overlay->remove2D(panel);
            }
            overlays.destroyOverlayElement(panel);
        }
        if (tip_box_ != nullptr && tip_ != nullptr) {
            tip_box_->removeChild(tip_->getName());
            overlays.destroyOverlayElement(tip_);
            tip_ = nullptr;
        }
        if (tip_box_ != nullptr) {
            if (bars_ != nullptr) bars_->remove2D(tip_box_);
            overlays.destroyOverlayElement(tip_box_);
            tip_box_ = nullptr;
        }
        for (Ogre::Overlay* overlay : {overlay_, bars_}) {
            if (overlay != nullptr) overlays.destroy(overlay);
        }
    }
    panels_.clear();
    tip_ = nullptr;
    tip_box_ = nullptr;
    overlay_ = nullptr;
    bars_ = nullptr;
    loading_texture_.reset();
    logo_texture_.reset();
    built_ = false;
    visible_ = false;
}

}  // namespace tl::render
