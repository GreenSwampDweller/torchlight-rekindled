// The mods page before the title: the port's own, for every platform but iOS. The main menu's
// frame with the wardrobe editor's column (`MainMenu::show_editor`), scrolled by the page, and
// to the right a "Don't show this menu again" checkbox; Start plays.
#include "core/log.h"
#include "core/run_config.h"
#include "render/menu_chrome.h"
#include "render/overlay_set.h"
#include "render/renderer_impl.h"

#include <OgreOverlay.h>
#include <OgreOverlayManager.h>
#include <OgrePanelOverlayElement.h>
#include <OgreTextAreaOverlayElement.h>

namespace tl::render {

namespace {

/// Characters a line of the text beside the column holds.
constexpr size_t kModsInfoWidth = 40;

/// The checkbox's place, in the interface's authored pixels: level with the text beside the
/// column (`kEditorInfoFromRight` in menu.cpp), above the bottom bar (89 tall).
constexpr float kCheckboxFromRight = 470.0f;
constexpr float kCheckboxFromBottom = 211.0f;
/// The options screen's `Check Box` (`settingsmenu_xenon.layout`'s Show Blood): no size of its
/// own, so a check box's default, and its caption's place and font within it.
constexpr float kCheckboxWide = 50.0f;
constexpr float kCheckboxTall = 40.0f;
constexpr float kCheckboxLabelX = 46.0f;
constexpr float kCheckboxLabelY = 9.0f;
constexpr float kCheckboxLabelPoints = 14.0f;  // Serif14
const Ogre::ColourValue kCheckboxLabelColour(0xFC / 255.0f, 0xD6 / 255.0f, 0x15 / 255.0f);

constexpr const char* kCheckboxLabel = "Don't show this menu again";

/// The checkbox beside the column, drawn as the options screen draws its check boxes
/// (`LayoutScreen`): the console's `ceguiWidget/Checkbox` where it is mounted - `checkbox_off`,
/// `checkbox_on` under the focus, and the tick `check` over the whole box - else the PC's
/// `uiicons` (`Check`, `CheckChecked` and their `Highlight`s).
class ModsCheckbox {
public:
    ModsCheckbox() = default;
    ~ModsCheckbox() { destroy(); }
    ModsCheckbox(const ModsCheckbox&) = delete;
    ModsCheckbox& operator=(const ModsCheckbox&) = delete;

    void create(const std::string& group, unsigned width, unsigned height);
    void set(bool ticked, bool focused);
    void destroy();

private:
    bool load(const char* file, const std::string& group);
    Ogre::PanelOverlayElement* add_panel(const std::string& suffix, float left, float top);
    Ogre::TextAreaOverlayElement* add_text(const std::string& suffix,
                                           const Ogre::ColourValue& colour);
    void paint(Ogre::PanelOverlayElement* panel, const std::string& image) const;

    tl::formats::Imageset set_;
    Ogre::TexturePtr texture_;
    Ogre::MaterialPtr material_;
    bool art_ = false;
    bool console_ = false;
    float across_ = 1.0f;
    float down_ = 1.0f;
    float scale_ = 1.0f;
    /// The art, and over it the text: within one overlay OGRE orders transparent pieces by pass,
    /// so text and art each get their own, as the main menu's do.
    Ogre::Overlay* overlay_ = nullptr;
    Ogre::Overlay* text_overlay_ = nullptr;
    Ogre::OverlayContainer* root_ = nullptr;
    Ogre::OverlayContainer* text_root_ = nullptr;
    std::vector<std::pair<Ogre::OverlayContainer*, Ogre::OverlayElement*>> elements_;
    Ogre::PanelOverlayElement* box_ = nullptr;
    Ogre::PanelOverlayElement* tick_ = nullptr;
    Ogre::TextAreaOverlayElement* label_ = nullptr;
    Ogre::TextAreaOverlayElement* label_shadow_ = nullptr;
};

bool ModsCheckbox::load(const char* file, const std::string& group) {
    std::vector<uint8_t> bytes;
    if (!Ogre::ResourceGroupManager::getSingleton().resourceExists(group, file) ||
        !read_menu_resource(file, group, bytes)) {
        return false;
    }
    set_ = tl::formats::parse_imageset(bytes.data(), bytes.size());
    if (!set_.ok) return false;
    texture_ = Ogre::TextureManager::getSingleton().load(set_.image_file, group);
    if (!texture_) return false;
    material_ = build_menu_material("tl/mods/checkbox/" + set_.name, texture_);
    return true;
}

Ogre::PanelOverlayElement* ModsCheckbox::add_panel(const std::string& suffix, float left,
                                                   float top) {
    auto* panel = static_cast<Ogre::PanelOverlayElement*>(
        Ogre::OverlayManager::getSingleton().createOverlayElement("Panel",
                                                                  "tl/mods/checkbox/" + suffix));
    panel->setMetricsMode(Ogre::GMM_RELATIVE);
    panel->setPosition(left / across_, top / down_);
    panel->setDimensions(kCheckboxWide * scale_ / across_, kCheckboxTall * scale_ / down_);
    root_->addChild(panel);
    elements_.emplace_back(root_, panel);
    return panel;
}

Ogre::TextAreaOverlayElement* ModsCheckbox::add_text(const std::string& suffix,
                                                     const Ogre::ColourValue& colour) {
    auto* text = static_cast<Ogre::TextAreaOverlayElement*>(
        Ogre::OverlayManager::getSingleton().createOverlayElement("TextArea",
                                                                  "tl/mods/checkbox/" + suffix));
    text->setMetricsMode(Ogre::GMM_RELATIVE);
    text->setFontName(kFontName, Ogre::RGN_INTERNAL);
    text->setColour(colour);
    text_root_->addChild(text);
    elements_.emplace_back(text_root_, text);
    return text;
}

void ModsCheckbox::paint(Ogre::PanelOverlayElement* panel, const std::string& image) const {
    const tl::formats::ImagesetImage* found = art_ ? set_.find(image) : nullptr;
    if (panel == nullptr || found == nullptr) return;
    const auto tw = static_cast<float>(texture_->getWidth());
    const auto th = static_cast<float>(texture_->getHeight());
    panel->setMaterial(material_);
    panel->setUV(static_cast<float>(found->x) / tw, static_cast<float>(found->y) / th,
                 static_cast<float>(found->x + found->width) / tw,
                 static_cast<float>(found->y + found->height) / th);
}

void ModsCheckbox::create(const std::string& group, unsigned width, unsigned height) {
    destroy();
    if (!ensure_menu_font()) return;
    console_ = load("media/UI/icons2/widgets2.imageset", group);
    art_ = console_ || load(kUiIconsImageset, group);

    auto& overlays = Ogre::OverlayManager::getSingleton();
    const auto layer = [&](const std::string& name, unsigned short z_order,
                           Ogre::OverlayContainer*& root) {
        Ogre::Overlay* overlay = overlays.create(name);
        overlay->setZOrder(z_order);
        root = static_cast<Ogre::OverlayContainer*>(
            overlays.createOverlayElement("Panel", name + "/root"));
        root->setMetricsMode(Ogre::GMM_RELATIVE);
        root->setPosition(0.0f, 0.0f);
        root->setDimensions(1.0f, 1.0f);
        overlay->add2D(root);
        return overlay;
    };
    // Over the main menu's text (402).
    overlay_ = layer("tl/mods/checkbox", 403, root_);
    text_overlay_ = layer("tl/mods/checkbox/text", 404, text_root_);

    across_ = static_cast<float>(width ? width : 1);
    down_ = static_cast<float>(height ? height : 1);
    scale_ = down_ / kNativeHeight;
    const float left = across_ - kCheckboxFromRight * scale_;
    const float top = down_ - kCheckboxFromBottom * scale_;
    if (art_) {
        box_ = add_panel("box", left, top);
        if (console_) {
            tick_ = add_panel("tick", left, top);
            paint(tick_, "check");
        }
    }
    label_shadow_ = add_text("label-a", Ogre::ColourValue::Black);
    label_ = add_text("label-b", kCheckboxLabelColour);
    const float line = kCheckboxLabelPoints * ui_points_to_pixels() * scale_;
    const float label_left = art_ ? left + kCheckboxLabelX * scale_ : left;
    for (Ogre::TextAreaOverlayElement* text : {label_shadow_, label_}) {
        const float drop = text == label_shadow_ ? kLabelShadow * scale_ : 0.0f;
        set_char_height(text, line / down_);
        text->setPosition((label_left + drop) / across_,
                          (top + kCheckboxLabelY * scale_ + drop) / down_);
    }
    overlay_->show();
    text_overlay_->show();
}

void ModsCheckbox::set(bool ticked, bool focused) {
    if (label_ == nullptr) return;
    std::string caption = kCheckboxLabel;
    if (box_ == nullptr) caption = std::string(ticked ? "[x] " : "[ ] ") + caption;
    for (Ogre::TextAreaOverlayElement* label : {label_shadow_, label_}) label->setCaption(caption);
    if (console_) {
        paint(box_, focused ? "checkbox_on" : "checkbox_off");
        if (ticked) {
            tick_->show();
        } else {
            tick_->hide();
        }
    } else {
        paint(box_, std::string(ticked ? "CheckChecked" : "Check") + (focused ? "Highlight" : ""));
    }
}

void ModsCheckbox::destroy() {
    if (overlay_ == nullptr) return;
    auto& overlays = Ogre::OverlayManager::getSingleton();
    for (auto& [parent, element] : elements_) {
        parent->removeChild(element->getName());
        overlays.destroyOverlayElement(element);
    }
    elements_.clear();
    for (auto [overlay, root] :
         {std::pair{overlay_, root_}, std::pair{text_overlay_, text_root_}}) {
        overlay->remove2D(root);
        overlays.destroyOverlayElement(root);
        overlays.destroy(overlay);
    }
    overlay_ = text_overlay_ = nullptr;
    root_ = text_root_ = nullptr;
    box_ = tick_ = nullptr;
    label_ = label_shadow_ = nullptr;
}

/// The page while it is up: the column (the main menu's, a window of `kModsPageRows` rows of
/// it), the checkbox and the text beside the column, and what the pad does to them.
class ModsPage {
public:
    ModsPage(MainMenu& menu, tl::core::ModList& list)
        : menu_(menu),
          list_(list),
          rows_(tl::core::mods_menu_row_count(list)),
          shown_(std::min(tl::core::kModsPageRows, rows_)) {}

    void open(const std::string& group, unsigned width, unsigned height) {
        checkbox_.create(group, width, height);
        show_column();
        // The main menu's Start prompt (Credits there): Start plays here.
        menu_.set_bar_caption("credits", "Play");
    }

    /// One frame of the pad; `MenuAction::Back` for Start (or B) and `MenuAction::None`
    /// otherwise.
    MenuAction step(tl::core::InputState input);

    bool dont_show_again() const { return dont_show_again_; }
    void close() { checkbox_.destroy(); }

private:
    using On = tl::core::ModsPageFocus::On;

    void show_column();
    void show_focus();
    /// Up and down: the page's own, as it scrolls the column.
    void step_up_down(float move_y);
    /// Right to the checkbox, left back to the column.
    void step_sideways(float move_x);
    /// The checkbox has the focus: the column only keeps which buttons are down.
    MenuAction step_checkbox(const tl::core::InputState& input);
    /// A on a row.
    void switch_row();

    MainMenu& menu_;
    tl::core::ModList& list_;
    const size_t rows_;
    const size_t shown_;
    ModsCheckbox checkbox_;
    tl::core::ModsPageFocus focus_;
    bool dont_show_again_ = false;
    tl::core::InputState held_;
    bool up_down_held_ = true;
    bool side_held_ = true;
};

void ModsPage::show_column() {
    const std::vector<std::string> captions = tl::core::mods_menu_rows(list_);
    const auto first = captions.begin() + static_cast<std::ptrdiff_t>(focus_.top);
    menu_.show_editor({first, first + static_cast<std::ptrdiff_t>(shown_)}, std::string());
    menu_.hide_copyright();
    show_focus();
}

void ModsPage::show_focus() {
    const bool on_row = focus_.on == On::Row;
    menu_.set_focus(on_row ? static_cast<int>(focus_.row - focus_.top) : -1);
    checkbox_.set(dont_show_again_, !on_row);
    std::string info =
        on_row ? tl::core::mods_menu_info(list_, static_cast<int>(focus_.row), kModsInfoWidth)
               : tl::core::mods_menu_checkbox_info(kModsInfoWidth);
    const std::string note = tl::core::mods_menu_scroll_note(rows_, focus_.top);
    if (!note.empty()) info += "\n\n" + note;
    menu_.set_editor_info(info);
}

void ModsPage::step_up_down(float move_y) {
    const int direction = move_y > 0.5f ? -1 : move_y < -0.5f ? 1 : 0;
    if (direction != 0 && !up_down_held_) {
        const size_t top = focus_.top;
        focus_ = tl::core::step_mods_page_focus(focus_, rows_, direction);
        if (focus_.top != top) {
            show_column();
        } else {
            show_focus();
        }
    }
    up_down_held_ = direction != 0;
}

void ModsPage::step_sideways(float move_x) {
    const int side = move_x > 0.5f ? 1 : move_x < -0.5f ? -1 : 0;
    if (side != 0 && !side_held_) {
        const On before = focus_.on;
        focus_ = tl::core::step_mods_page_side(focus_, side);
        if (focus_.on != before) show_focus();
    }
    side_held_ = side != 0;
}

MenuAction ModsPage::step_checkbox(const tl::core::InputState& input) {
    menu_.hold(input);
    if (input.confirm && !held_.confirm) {
        dont_show_again_ = !dont_show_again_;
        checkbox_.set(dont_show_again_, true);
    }
    const bool leave = (input.skip && !held_.skip) || (input.quit && !held_.quit);
    return leave ? MenuAction::Back : MenuAction::None;
}

void ModsPage::switch_row() {
    tl::core::switch_mods_menu_row(list_, focus_.top + static_cast<size_t>(menu_.editor_row()));
    const std::vector<std::string> captions = tl::core::mods_menu_rows(list_);
    for (size_t i = 0; i < shown_; ++i) menu_.set_row_caption(i, captions[focus_.top + i]);
}

MenuAction ModsPage::step(tl::core::InputState input) {
    step_up_down(input.move_y);
    step_sideways(input.move_x);
    // A row is switched by A alone: the editor's sideways steps and bumpers are not the page's.
    input.move_y = 0.0f;
    input.move_x = 0.0f;
    input.tab_prev = input.tab_next = false;
    MenuAction action = MenuAction::None;
    if (focus_.on == On::Checkbox) {
        action = step_checkbox(input);
    } else {
        action = menu_.update(input);
        if (action == MenuAction::EditorStep) {
            switch_row();
            action = MenuAction::None;
        }
    }
    held_ = input;
    // Every frame: a resize lays the menu out again, copyright line and all.
    menu_.hide_copyright();
    return action;
}

}  // namespace

// The mods page's own: it has no copyright line.
void MainMenu::hide_copyright() {
    if (copyright_ != nullptr) copyright_->hide();
}

// The mods page's own: its Start prompt says what Start does there.
void MainMenu::set_bar_caption(const std::string& window, const std::string& text) {
    for (BarItem& item : bar_items_) {
        if (item.window != window) continue;
        for (Ogre::TextAreaOverlayElement* label : {item.shadow, item.label}) {
            if (label != nullptr) label->setCaption(text);
        }
    }
}

// PORT: the FPS overlay as the mods page leaves its mod.
void Renderer::set_stats_overlay(bool on) {
    impl_->config.stats_overlay = on;
    impl_->set_stats_overlay(on);
}

MenuAction Renderer::run_mods_menu(tl::core::ModList& list, bool& dont_show_again,
                                   const std::string& dump_png) {
    Impl& m = *impl_;
    if (!m.window) return MenuAction::Quit;

    // No level is loaded yet: the loading screen's artwork stands behind it.
    m.loading.show_backdrop(kGameGroup, m.config.width, m.config.height);
    m.menu.set_continue_available(false);
    if (!m.menu.create(kGameGroup, m.config.width, m.config.height)) {
        TL_LOG_WARN("the mods page could not be built; the mods stay as mods.dat has them");
        m.loading.hide();
        return MenuAction::Back;
    }
    ModsPage page(m.menu, list);
    page.open(kGameGroup, m.config.width, m.config.height);

    MenuAction action = MenuAction::None;
    if (!dump_png.empty()) {
        keep_loading_screen = true;
        dump_frame(dump_png);
        keep_loading_screen = false;
    }
    if (m.config.headless || !dump_png.empty()) action = MenuAction::Back;

    Ogre::Timer* timer = m.root->getTimer();
    unsigned long last = timer->getMilliseconds();
    while (action == MenuAction::None && !m.window->isClosed() && tl::core::should_keep_running()) {
        tl::core::pump_events();
        const unsigned long now = timer->getMilliseconds();
        const float delta = std::min(static_cast<float>(now - last) * 0.001f, 0.1f);
        last = now;

        action = page.step(tl::core::poll_input());
        m.advance(delta);
        if (!m.root->renderOneFrame()) break;
        m.save_scripted_shot();
    }
    dont_show_again = page.dont_show_again();
    page.close();
    m.menu.destroy();
    m.loading.hide();

    if (action == MenuAction::None) action = MenuAction::Quit;
    TL_LOG_INFO("mods page: %s%s", action == MenuAction::Back ? "play" : "quit",
                dont_show_again ? ", not to be shown again" : "");
    return action;
}

namespace {

// The mods page as `start_renderer` shows it; false when the run is to end.
bool run_start_mods_menu(Renderer& renderer, RendererConfig& rc, int argc, char** argv) {
    // iOS takes `mods.dat` as it is.
    if (!tl::core::kModsPage) return true;
    std::string dump_png;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--mods-menu-dump") dump_png = argv[i + 1];
    }
    if (rc.headless && dump_png.empty()) return true;
    std::vector<std::string> roots;
    std::vector<std::string> names;
    tl::core::ModList list;
    // Empty under `--safemode` and `mods_menu=0`: no page then.
    tl::core::mods_from_command_line(argc, argv, rc.game_data, roots, names, &list);
    if (list.mods.empty()) return true;

    const std::vector<uint8_t> before = tl::core::mods_dat_bytes(list);
    bool dont_show_again = false;
    if (renderer.run_mods_menu(list, dont_show_again, dump_png) != MenuAction::Back) return false;
    const bool switched = tl::core::mods_dat_bytes(list) != before;
    if (switched && !tl::core::save_mods(list)) {
        TL_LOG_WARN("mods page: could not write %smods.dat; the mods stay as they were",
                    list.mods_dir.c_str());
    }
    // Off from this start on, as `mods_menu=0` is read at the next.
    if (dont_show_again && !tl::core::save_run_config_value("mods_menu", false)) {
        TL_LOG_WARN(
            "mods page: no config.txt to keep mods_menu=0 in; the page comes back next time");
    }
    if (!switched && !dont_show_again) return true;

    const std::vector<std::string> mounted = rc.mod_data;
    rc.mod_data.clear();
    rc.mod_names.clear();
    tl::core::mods_from_command_line(argc, argv, rc.game_data, rc.mod_data, rc.mod_names);
    if (rc.stats_overlay_from_mods && rc.stats_overlay != tl::core::fps_overlay_mod_on()) {
        rc.stats_overlay = tl::core::fps_overlay_mod_on();
        renderer.set_stats_overlay(rc.stats_overlay);
    }
    if (rc.mod_data == mounted) return true;
    TL_LOG_INFO("mods page: %zu mod(s) on; restarting on them", rc.mod_names.size());
    if (!renderer.restart(rc)) {
        TL_LOG_ERROR("mods page: the restart failed");
        return false;
    }
    return true;
}

}  // namespace

std::optional<int> start_renderer(Renderer& renderer, RendererConfig& rc,
                                  const RendererStart& start) {
    if (!start.logic_trace.empty()) renderer.set_logic_trace(start.logic_trace);
    if (!renderer.init(rc)) {
        TL_LOG_ERROR("renderer initialisation failed");
        tl::core::log_shutdown();
        return 1;
    }
    if (!start.passability_dump.empty()) renderer.set_passability_dump(start.passability_dump);
    if (start.start_in_menu && !run_start_mods_menu(renderer, rc, start.argc, start.argv)) {
        renderer.shutdown();
        tl::core::log_shutdown();
        return 0;
    }
    return std::nullopt;
}

}  // namespace tl::render
