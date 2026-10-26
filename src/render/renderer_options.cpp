// The Pause Menu and the Options screen it opens: drawing and the pad. What each button does
// is core/settings_menu (COptionsMenu, CSettingsMenuXenon); the screens are the Xbox
// release's options_Xenon.layout and settingsmenu_xenon.layout.
#include "render/renderer_impl.h"

#include "core/audio.h"
#include "core/run_config.h"
#include "core/settings_menu.h"
#include "core/touch_pad.h"

#include <cmath>
#include <fstream>

namespace tl::render {

namespace {

/// The caption under the Achievements button (the window's `NAME`; the layout
/// gives it no `WIDGET NAME`).
constexpr const char* kAchievementsCaption = "Text1_clone309121";

}  // namespace

void Renderer::Impl::create_options_screens() {
    // Above the HUD and the waypoint menu, below the quest window and the
    // modal message the debug menu is drawn on.
    pause_screen.create(kGameGroup, config.width, config.height, "media/UI/options_Xenon.layout",
                        "tl/pause", 574);
    settings_screen.create(kGameGroup, config.width, config.height,
                           "media/UI/settingsmenu_xenon.layout", "tl/options", 578);
    // Over Options, which Y on it closes on the way here.
    howto_screen.create(kGameGroup, config.width, config.height, "media/UI/howto_xenon.layout",
                        "tl/howto", 580);
    howto_menu = tl::core::HowToMenu{};
    if (howto_screen.ready()) {
        // The console loads the CEGUI export (`howto_xenon.uilayout`), whose
        // panel (`i_619`) is `HorizontalAlignment = Centre`; this `[UI]` twin
        // leaves it at the left edge.
        howto_screen.set_anchor("CEGUI Object0", 1, 0);
        howto_screen.refresh();
    }
    pause_menu = tl::core::OptionsMenu{};
    settings_menu = tl::core::SettingsMenu{};
    // DIVERGENCE (platform): no storage device to pick on a Switch.
    settings_menu.storage_hidden = true;
#if defined(TL_PLATFORM_IOS)
    settings_menu.storage_hidden = false;
#endif
    settings_menu.update_audio_levels = [](const tl::core::AudioLevels& levels) {
        const tl::core::AudioGains gains = tl::core::audio_gains(levels);
        tl::core::audio_set_levels(gains.effects, gains.music);
    };
    if (pause_screen.ready()) {
        for (int i = 0; i < tl::core::kOptionsRowCount; ++i) {
            pause_screen.set_hidden(tl::core::options_row(i).window,
                                    !tl::core::options_row_offered(i));
        }
        pause_screen.set_caption(kAchievementsCaption, "Debug");
    }
    if (settings_screen.ready()) {
        settings_screen.set_hidden(
            tl::core::settings_control_window(tl::core::SettingsControl::Storage),
            settings_menu.storage_hidden);
#if defined(TL_PLATFORM_IOS)
        settings_screen.set_hidden("PickerLeftArrow", false);
        settings_screen.set_hidden("PickerRightArrow", false);
#else
        settings_screen.set_hidden("PickerLeftArrow", true);
        settings_screen.set_hidden("PickerRightArrow", true);
#endif
    }
}

// `CGameUI::toggleOptions`: `closeAll`,
// Options closed, and the Pause Menu's `setOpen`.
void Renderer::Impl::open_pause_menu() {
    if (!pause_screen.ready()) {
        // No console interface data to draw it with: what Start did before,
        // with the debug tools on.
        if (tl::core::debug_tools()) open_debug_menu();
        return;
    }
    if (inventory_menu.visible()) {
        inventory_menu.hide();
        sync_stats_page();
        if (automap_initialized && config.minimap) automap.show();
    }
    pause_menu.open = true;
    pause_stick_held = true;
    // The level stands still under it, as under every modal menu
    // (`Renderer::Impl::game_paused`, read each frame by `advance`).
    refresh_pause_menu();
    pause_screen.show();
    play_ui(tl::core::InterfaceSound::MenuOpen);
    TL_LOG_INFO("pause menu: open");
}

void Renderer::Impl::close_pause_menu() {
    pause_menu.open = false;
    pause_screen.hide();
}

void Renderer::Impl::refresh_pause_menu() {
    for (int i = 0; i < tl::core::kOptionsRowCount; ++i) {
        pause_screen.set_hover(tl::core::options_row(i).window, i == pause_menu.focus);
    }
    pause_screen.refresh();
}

void Renderer::Impl::drive_pause_menu(const tl::core::InputState& in, bool start_edge) {
    // The shared tab list: up and left are the previous
    // window, down and right the next.
    int step = 0;
    if (std::abs(in.move_y) > 0.5f) step = in.move_y > 0.0f ? -1 : 1;
    else if (std::abs(in.move_x) > 0.5f) step = in.move_x < 0.0f ? -1 : 1;
    if (step != 0) {
        if (!pause_stick_held && pause_menu.navigate(step > 0)) {
            play_ui(tl::core::InterfaceSound::Navigate);
            refresh_pause_menu();
        }
        pause_stick_held = true;
    } else {
        pause_stick_held = false;
    }

    const bool confirm_edge = in.confirm && !confirm_held;
    const bool skip_edge = in.skip && !skip_held;
    tl::core::OptionsClick click;
    if (confirm_edge) {
        click = pause_menu.on_click(tl::core::options_row(pause_menu.focus).function);
    } else if (start_edge || skip_edge) {
        // Start is the layout's own `Close`. INVENTED: B closes it too; the
        // console's B on this menu was not read.
        click = pause_menu.on_click(tl::core::kLayoutFunctionClose);
    }
    if (!click.closed) return;

    if (click.open_settings) {
        // `CGameUI::toggleSettings`: this menu closed, Options opened.
        pause_screen.hide();
        open_settings_menu();
        return;
    }
    close_pause_menu();
    if (click.open_help) {
        // `GUIHELP` (0x64): `CGameUI`'s Help toggle.
        TL_LOG_INFO("pause menu: help");
        toggle_help();
        return;
    }
    if (click.open_debug) {
        TL_LOG_INFO("pause menu: the debug menu");
        open_debug_menu();
        return;
    }
    play_ui(tl::core::InterfaceSound::MenuClose);
    if (click.exit_to_title) {
        // `CGameUI::requestSetGameState(0, 0)`: the title.
        TL_LOG_INFO("pause menu: exit to title");
        quit_requested = true;
        return;
    }
    TL_LOG_INFO("pause menu: closed");
}

void Renderer::Impl::open_settings_menu() {
    if (!settings_screen.ready()) {
        TL_LOG_WARN("options: no settingsmenu_xenon.layout mounted; the screen cannot open");
        if (!title_menu_running) open_pause_menu();
        return;
    }
    settings_menu.set_open(true, game_settings());
    settings_stick_held = true;
    settings_dpad_held = true;
    settings_y_held = true;
    help_after_settings = false;
    refresh_settings_menu();
    settings_screen.show();
    play_ui(tl::core::InterfaceSound::MenuOpen);
    TL_LOG_INFO("options: open");
}

void Renderer::Impl::refresh_settings_menu() {
    using tl::core::SettingsControl;
    for (int i = 1; i <= tl::core::kSettingsControlCount; ++i) {
        const SettingsControl control = static_cast<SettingsControl>(i);
        settings_screen.set_hover(tl::core::settings_control_window(control),
                                  control == settings_menu.focused());
    }
    settings_screen.set_checked("MusicMute", settings_menu.music_mute);
    settings_screen.set_checked("SoundMute", settings_menu.sound_mute);
    settings_screen.set_checked("ShowTips", settings_menu.show_tips);
    settings_screen.set_checked("ShowFloatyNumbers", settings_menu.floaty_numbers);
    settings_screen.set_checked("ShowBlood", settings_menu.show_blood);
#if defined(TL_PLATFORM_IOS)
    static const char* const kTextSizeNames[] = {"Small", "Medium", "Large"};
    static const char* const kSensitivityNames[] = {"Slow", "Normal", "Fast"};
    static const char* const kPitchRangeNames[] = {"Narrow", "Normal", "Wide"};
    static const char* const kPitchInversionNames[] = {"Off", "On"};
    static const char* const kTouchSizeNames[] = {"Small", "Normal", "Large"};
    static const char* const kTouchOpacityNames[] = {"Low", "Normal", "High"};
    static const char* const kTouchStyleNames[] = {"Classic", "Neon", "Solid"};
    static const char* const kTouchLayoutNames[] = {"Two thumbs", "Right hand", "Left hand"};
    const int text_size = std::clamp(game_settings().text_size, 0, 2);
    static constexpr float kTouchSizes[] = {0.85f, 1.0f, 1.2f};
    static constexpr float kTouchOpacities[] = {0.7f, 1.0f, 1.35f};
    tl::core::touch_pad().set_visual_options(
        kTouchSizes[std::clamp(game_settings().touch_control_size, 0, 2)],
        kTouchOpacities[std::clamp(game_settings().touch_control_opacity, 0, 2)],
        std::clamp(game_settings().touch_control_style, 0, 2),
        std::clamp(game_settings().touch_control_layout, 0, 2));
    tl::core::touch_pad().set_layout_offsets(game_settings().touch_control_offsets);
    tl::core::touch_pad().set_edit_mode(game_settings().touch_control_edit);
    game_hud.set_compact(game_settings().compact_hud);
    std::string picker_caption;
    switch (settings_menu.ios_picker) {
        case 1:
            picker_caption = std::string("Camera Speed: ") +
                             kSensitivityNames[
                                 std::clamp(game_settings().camera_sensitivity, 0, 2)];
            break;
        case 2:
            picker_caption = std::string("Camera Tilt: ") +
                             kPitchRangeNames[
                                 std::clamp(game_settings().camera_pitch_range, 0, 2)];
            break;
        case 3:
            picker_caption = std::string("Pinch Zoom: ") +
                             kSensitivityNames[
                                 std::clamp(game_settings().camera_zoom_sensitivity, 0, 2)];
            break;
        case 4:
            picker_caption = std::string("Invert Camera Pitch: ") +
                             kPitchInversionNames[game_settings().camera_invert_pitch ? 1 : 0];
            break;
        case 5:
            picker_caption = std::string("Touch Size: ") +
                             kTouchSizeNames[std::clamp(game_settings().touch_control_size, 0, 2)];
            break;
        case 6:
            picker_caption = std::string("Touch Opacity: ") +
                             kTouchOpacityNames[
                                 std::clamp(game_settings().touch_control_opacity, 0, 2)];
            break;
        case 7:
            picker_caption = std::string("Touch Style: ") +
                             kTouchStyleNames[std::clamp(game_settings().touch_control_style, 0, 2)];
            break;
        case 8:
            picker_caption = std::string("Touch Layout: ") +
                             kTouchLayoutNames[
                                 std::clamp(game_settings().touch_control_layout, 0, 2)];
            break;
        case 9:
            picker_caption = std::string("Compact HUD: ") +
                             (game_settings().compact_hud ? "On" : "Off");
            break;
        case 10:
            picker_caption = std::string("Move Controls: ") +
                             (game_settings().touch_control_edit ? "On" : "Off");
            break;
        default:
            picker_caption = std::string("Text Size: ") + kTextSizeNames[text_size];
            break;
    }
    settings_screen.set_caption("Text24", picker_caption);
    const float previous_scale = ui_text_scale();
    set_ui_text_scale(ui_text_size_scale(text_size));
    if (previous_scale != ui_text_scale()) {
        hud.resize(config.width, config.height);
        game_hud.resize(config.width, config.height);
        inventory_menu.resize(config.width, config.height);
        quest_dialog.resize(config.width, config.height);
    }
#endif

    // the tab is put at the bar's size less its own, times
    // the slider's fraction - on both axes, so it also sinks the three pixels
    // the bar is taller than it.
    const auto place = [this](SettingsControl bar, float along) {
        float bar_wide = 0.0f, bar_tall = 0.0f, tab_wide = 0.0f, tab_tall = 0.0f;
        const char* tab = tl::core::settings_slider_tab_window(bar);
        if (!settings_screen.extent(tl::core::settings_control_window(bar), bar_wide, bar_tall) ||
            !settings_screen.extent(tab, tab_wide, tab_tall)) {
            return;
        }
        settings_screen.set_offset(tab, (bar_wide - tab_wide) * along,
                                   (bar_tall - tab_tall) * along);
    };
    place(SettingsControl::MusicVolume, settings_menu.music_tab);
    place(SettingsControl::SoundVolume, settings_menu.sound_tab);
    place(SettingsControl::Brightness, settings_menu.brightness_tab);
    place(SettingsControl::Contrast, settings_menu.contrast_tab);
    settings_screen.refresh();
}

void Renderer::Impl::drive_settings_menu(const tl::core::InputState& in) {
    tl::core::GameSettings& settings = game_settings();
    // The pad as the screen's handler is told of it: up and down (the D-pad
    // or the stick), the stick's left and right - which walk the list here -
    // and the D-pad's own left and right, which change a value.
    int code = 0;
    if (std::abs(in.move_y) > 0.5f) {
        if (!settings_stick_held)
            code = in.move_y > 0.0f ? tl::core::kMenuPadUp : tl::core::kMenuPadDown;
        settings_stick_held = true;
    } else if (std::abs(in.stick_x) > 0.5f) {
        if (!settings_stick_held) {
            code = in.stick_x < 0.0f ? tl::core::kMenuPadStickLeft : tl::core::kMenuPadStickRight;
        }
        settings_stick_held = true;
    } else {
        settings_stick_held = false;
    }
    if (std::abs(in.dpad_x) > 0.5f) {
        if (!settings_dpad_held)
            code = in.dpad_x < 0.0f ? tl::core::kMenuPadLeft : tl::core::kMenuPadRight;
        settings_dpad_held = true;
    } else {
        settings_dpad_held = false;
    }
    if (in.confirm && !confirm_held) code = tl::core::kMenuPadA;
    if (in.skip && !skip_held) code = tl::core::kMenuPadB;
    // Y is Help: the screen closes as Done closes it, then Help opens.
    // Start is not read.
    if (in.action_y && !settings_y_held) code = tl::core::kMenuPadY;
    settings_y_held = in.action_y;
    if (code == 0) return;

    if (settings_menu.navigate(code)) {
        play_ui(tl::core::InterfaceSound::Navigate);
        refresh_settings_menu();
        return;
    }
    const tl::core::SettingsPress press = settings_menu.press(code, settings);
    if (press.changed) {
        TL_LOG_INFO(
            "options: %s -> music %.2f%s, sound %.2f%s, tips %d, damage info %d, blood %d, "
            "brightness %.0f, contrast %.0f",
            tl::core::settings_control_window(settings_menu.focused()),
            static_cast<double>(settings.music_volume), settings_menu.music_mute ? " muted" : "",
            static_cast<double>(settings.sound_volume), settings_menu.sound_mute ? " muted" : "",
            settings_menu.show_tips ? 1 : 0, settings_menu.floaty_numbers ? 1 : 0,
            settings_menu.show_blood ? 1 : 0, static_cast<double>(settings.brightness),
            static_cast<double>(settings.contrast));
        refresh_settings_menu();
    }
    if (press.closed) {
        settings_screen.hide();
        play_ui(tl::core::InterfaceSound::MenuClose);
        help_after_settings = press.help;
    }
}

void Renderer::Impl::step_settings_menu() {
    tl::core::GameSettings& settings = game_settings();
    const tl::core::SettingsUpdate update = settings_menu.update(settings);
    if (!update.finished) return;
    if (update.save) {
        if (!config.settings_path.empty()) {
            std::ofstream file(config.settings_path, std::ios::binary | std::ios::trunc);
            file << tl::core::write_game_settings(settings);
            if (!file) TL_LOG_WARN("options: could not write '%s'", config.settings_path.c_str());
        }
        TL_LOG_INFO(
            "options: done - music %.2f%s, sound %.2f%s, tips %d, damage info %d, blood %d, "
            "brightness %.0f, contrast %.0f",
            static_cast<double>(settings.music_volume), settings.music_mute ? " muted" : "",
            static_cast<double>(settings.sound_volume), settings.sound_mute ? " muted" : "",
            settings.show_tips ? 1 : 0, settings.floaty_numbers ? 1 : 0,
            settings.show_blood ? 1 : 0, static_cast<double>(settings.brightness),
            static_cast<double>(settings.contrast));
    } else {
        TL_LOG_INFO("options: closed without Done");
    }
    // The close (in a game, client +0x1404): the Pause Menu
    // comes back. At the title CMainMenu::update
    // only puts focus back. After Y: onClick(9) closes, then the Help toggle
    // closes the Pause Menu again and opens Help.
    const bool help = help_after_settings;
    help_after_settings = false;
    if (title_menu_running) {
        if (!help) TL_LOG_INFO("options: back to the main menu");
    } else {
        open_pause_menu();
    }
    if (help) toggle_help();
}

// `CHowToMenu::setOpen`. No sound on either side.
void Renderer::Impl::set_help_open(bool open) {
    if (open == howto_menu.open) return;
    if (open && !howto_screen.ready()) {
        TL_LOG_WARN("help: no howto_xenon.layout mounted; the screen cannot open");
        if (!title_menu_running) open_pause_menu();
        return;
    }
    howto_menu.set_open(open);
    howto_stick_held = true;
    if (open) {
        howto_screen.show();
        TL_LOG_INFO("help: open");
        return;
    }
    howto_screen.hide();
    TL_LOG_INFO("help: closed");
    // in a game (the client's `+0x1404`) the Pause
    // Menu comes back.
    if (!title_menu_running) open_pause_menu();
}

bool Renderer::pause_menu_open() const { return impl_->pause_menu.open; }
bool Renderer::options_menu_open() const { return impl_->settings_menu.open; }

}  // namespace tl::render
