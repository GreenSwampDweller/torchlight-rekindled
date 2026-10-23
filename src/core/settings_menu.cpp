#include "core/settings_menu.h"

#include "core/run_config.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <sstream>

namespace tl::core {

namespace {

// The constants, from the table at
// (offsets in the comments).
constexpr float kSliderFloor = 100.0f;             // +0x1a8: brightness and contrast start here
constexpr float kBrightnessRun = 500.0f;           // +0x30c
constexpr float kBrightnessPerUnit = 0.002f;       // +0x308
constexpr float kContrastRun = 300.0f;             // +0x314
constexpr float kContrastPerUnit = 0.0033333334f;  // +0x310
constexpr float kSliderStep = 0.05f;               // +0x1a0, and -0.05 at +0x478
constexpr float kBrightnessStepScale = 200.0f;     // +0x47c
constexpr float kContrastStepScale = 100.0f;       // +0x1a8

// The clamp both branches open-code: below 0 is 0, above 1 is 1.
float unit_clamp(float value) {
    if (!(value >= 0.0f)) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

// PORT: which options are check boxes on the port's options page.
bool is_checkbox(SettingsControl control) {
    return control == SettingsControl::MusicMute || control == SettingsControl::SoundMute ||
           control == SettingsControl::ShowTips || control == SettingsControl::ShowFloatyNumbers ||
           control == SettingsControl::ShowBlood;
}

// PORT: which options are sliders on the port's options page.
bool is_slider(SettingsControl control) {
    return control == SettingsControl::MusicVolume || control == SettingsControl::SoundVolume ||
           control == SettingsControl::Brightness || control == SettingsControl::Contrast;
}

}  // namespace

// PORT: the options written to the port's own settings file.
std::string write_game_settings(const GameSettings& s) {
    char text[640];
    std::snprintf(
        text, sizeof text,
        "SOUNDVOLUME:%.9g\nMUSICVOLUME:%.9g\nSOUNDMUTE:%d\nMUSICMUTE:%d\n"
        "SHOW_TIPS:%d\nFLOATY_NUMBERS:%d\nSHOW_BLOOD:%d\nBRIGHTNESS:%.9g\nCONTRAST:%.9g\n"
        "TEXT_SIZE:%d\nCAMERA_SENSITIVITY:%d\nCAMERA_PITCH_RANGE:%d\n"
        "CAMERA_ZOOM_SENSITIVITY:%d\nCAMERA_INVERT_PITCH:%d\n"
        "TOUCH_CONTROL_SIZE:%d\nTOUCH_CONTROL_OPACITY:%d\nTOUCH_CONTROL_STYLE:%d\n"
        "TOUCH_CONTROL_LAYOUT:%d\nCOMPACT_HUD:%d\nTOUCH_CONTROL_EDIT:%d\n",
        static_cast<double>(s.sound_volume), static_cast<double>(s.music_volume),
        s.sound_mute ? 1 : 0, s.music_mute ? 1 : 0, s.show_tips ? 1 : 0, s.floaty_numbers ? 1 : 0,
        s.show_blood ? 1 : 0, static_cast<double>(s.brightness), static_cast<double>(s.contrast),
        std::clamp(s.text_size, 0, 2), std::clamp(s.camera_sensitivity, 0, 2),
        std::clamp(s.camera_pitch_range, 0, 2), std::clamp(s.camera_zoom_sensitivity, 0, 2),
        s.camera_invert_pitch ? 1 : 0, std::clamp(s.touch_control_size, 0, 2),
        std::clamp(s.touch_control_opacity, 0, 2), std::clamp(s.touch_control_style, 0, 2),
        std::clamp(s.touch_control_layout, 0, 2), s.compact_hud ? 1 : 0,
        s.touch_control_edit ? 1 : 0);
    std::string result(text);
    char offset_line[64];
    for (int i = 0; i < 16; ++i) {
        std::snprintf(offset_line, sizeof offset_line, "TOUCH_LAYOUT_%d_X:%.7g\n", i,
                      static_cast<double>(s.touch_control_offsets[static_cast<size_t>(i) * 2]));
        result += offset_line;
        std::snprintf(offset_line, sizeof offset_line, "TOUCH_LAYOUT_%d_Y:%.7g\n", i,
                      static_cast<double>(s.touch_control_offsets[static_cast<size_t>(i) * 2 + 1]));
        result += offset_line;
    }
    return result;
}

// PORT: the options read from the port's own settings file.
void read_game_settings(const std::string& text, GameSettings& s) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = line.substr(0, colon);
        const std::string value = line.substr(colon + 1);
        char* end = nullptr;
        const float number = std::strtof(value.c_str(), &end);
        if (end == value.c_str()) continue;
        if (key == "SOUNDVOLUME") s.sound_volume = number;
        else if (key == "MUSICVOLUME") s.music_volume = number;
        else if (key == "SOUNDMUTE") s.sound_mute = number != 0.0f;
        else if (key == "MUSICMUTE") s.music_mute = number != 0.0f;
        else if (key == "SHOW_TIPS") s.show_tips = number != 0.0f;
        else if (key == "FLOATY_NUMBERS") s.floaty_numbers = number != 0.0f;
        else if (key == "SHOW_BLOOD") s.show_blood = number != 0.0f;
        else if (key == "BRIGHTNESS") s.brightness = number;
        else if (key == "CONTRAST") s.contrast = number;
        else if (key == "TEXT_SIZE") {
            s.text_size = number <= 0.0f ? 0 : number >= 2.0f ? 2 : 1;
        }
        else if (key == "CAMERA_SENSITIVITY") {
            s.camera_sensitivity = number <= 0.0f ? 0 : number >= 2.0f ? 2 : 1;
        } else if (key == "CAMERA_PITCH_RANGE") {
            s.camera_pitch_range = number <= 0.0f ? 0 : number >= 2.0f ? 2 : 1;
        } else if (key == "CAMERA_ZOOM_SENSITIVITY") {
            s.camera_zoom_sensitivity = number <= 0.0f ? 0 : number >= 2.0f ? 2 : 1;
        } else if (key == "CAMERA_INVERT_PITCH") {
            s.camera_invert_pitch = number != 0.0f;
        } else if (key == "TOUCH_CONTROL_SIZE") {
            s.touch_control_size = number <= 0.0f ? 0 : number >= 2.0f ? 2 : 1;
        } else if (key == "TOUCH_CONTROL_OPACITY") {
            s.touch_control_opacity = number <= 0.0f ? 0 : number >= 2.0f ? 2 : 1;
        } else if (key == "TOUCH_CONTROL_STYLE") {
            s.touch_control_style = number <= 0.0f ? 0 : number >= 2.0f ? 2 : 1;
        } else if (key == "TOUCH_CONTROL_LAYOUT") {
            s.touch_control_layout = number <= 0.0f ? 0 : number >= 2.0f ? 2 : 1;
        } else if (key == "COMPACT_HUD") {
            s.compact_hud = number != 0.0f;
        } else if (key == "TOUCH_CONTROL_EDIT") {
            s.touch_control_edit = number != 0.0f;
        } else if (key.rfind("TOUCH_LAYOUT_", 0) == 0) {
            int index = -1;
            char axis = '\0';
            if (std::sscanf(key.c_str(), "TOUCH_LAYOUT_%d_%c", &index, &axis) == 2 &&
                index >= 0 && index < 16 && (axis == 'X' || axis == 'Y')) {
                const size_t offset = static_cast<size_t>(index) * 2 + (axis == 'Y' ? 1 : 0);
                s.touch_control_offsets[offset] = std::clamp(number, -0.45f, 0.45f);
            }
        }
    }
}

// `CSoundManager::updateAudioLevels`.
AudioGains audio_gains(const AudioLevels& levels) {
    AudioGains gains;
    // The master sound group and the master channel group both take `sound`.
    gains.effects = levels.sound * levels.sound;
    // The music's sound group takes `music`; its channel is under the master
    // channel group.
    gains.music = levels.music * levels.sound;
    if (levels.music_mute) gains.music = 0.0f;
    if (levels.sound_mute) {
        gains.effects = 0.0f;
        gains.music = 0.0f;
    }
    return gains;
}

// PORT: an option's window in the console's options layout.
const char* settings_control_window(SettingsControl control) {
    switch (control) {
        case SettingsControl::MusicVolume: return "MusicVolume";
        case SettingsControl::MusicMute: return "MusicMute";
        case SettingsControl::SoundVolume: return "SoundVolume";
        case SettingsControl::SoundMute: return "SoundMute";
        case SettingsControl::ShowTips: return "ShowTips";
        case SettingsControl::ShowFloatyNumbers: return "ShowFloatyNumbers";
        case SettingsControl::ShowBlood: return "ShowBlood";
        case SettingsControl::Brightness: return "Brightness";
        case SettingsControl::Contrast: return "Contrast";
        case SettingsControl::Storage: return "Storage";
        case SettingsControl::Defaults: return "Defaults";
        case SettingsControl::None: break;
    }
    return "";
}

// PORT: a slider's tab window in the console's options layout.
const char* settings_slider_tab_window(SettingsControl control) {
    switch (control) {
        case SettingsControl::MusicVolume: return "sliderTabMusic";
        case SettingsControl::SoundVolume: return "sliderTabSound";
        case SettingsControl::Brightness: return "sliderTabBrightness";
        case SettingsControl::Contrast: return "sliderTabContrast";
        default: return nullptr;
    }
}

// `CSettingsXenonTabNavigationHandler::vfunc_1`. The shared
// handler's also answers for the D-pad's left and right and the
// stick's four diagonals; this screen's does not.
int settings_navigation(int code) {
    switch (code) {
        case kMenuPadUp:
        case kMenuPadStickUp:
        case kMenuPadStickLeft: return 2;
        case kMenuPadDown:
        case kMenuPadStickDown:
        case kMenuPadStickRight: return 1;
        default: return 0;
    }
}

// `CSettingsMenuXenon::vfunc_7`.
void SettingsMenu::set_open(bool opening, GameSettings& settings) {
    if (!open) {
        if (opening) {
            finished = false;
            accepted = false;
            opened_brightness = settings.brightness;
            opened_contrast = settings.contrast;
            sound_mute = settings.sound_mute;
            music_mute = settings.music_mute;
            // Read before the first slider is set: a volume slider writes the
            // mutes and calls `updateAudioLevels`, the other volume as it is.
            const float sound = settings.sound_volume;
            const float music = settings.music_volume;
            set_slider(SettingsControl::SoundVolume, sound, settings);
            set_slider(SettingsControl::MusicVolume, music, settings);
            set_slider(SettingsControl::Brightness, opened_brightness, settings);
            set_slider(SettingsControl::Contrast, opened_contrast, settings);
            show_tips = settings.show_tips;
            floaty_numbers = settings.floaty_numbers;
            show_blood = settings.show_blood;
            ios_picker = 0;
        }
    } else if (!opening) {
        // The window is taken down here; what the close does to the settings
        // waits for `update`.
        finished = false;
    }
    open = opening;
}

//
void SettingsMenu::set_slider(SettingsControl control, float value, GameSettings& settings) {
    float along = 0.0f;
    if (control == SettingsControl::Brightness) {
        if (value < kSliderFloor) value = kSliderFloor;
        along = unit_clamp((value - kSliderFloor) * kBrightnessPerUnit);
        settings.brightness = along * kBrightnessRun + kSliderFloor;
        brightness_tab = along;
    } else if (control == SettingsControl::Contrast) {
        if (value < kSliderFloor) value = kSliderFloor;
        along = unit_clamp((value - kSliderFloor) * kContrastPerUnit);
        settings.contrast = along * kContrastRun + kSliderFloor;
        contrast_tab = along;
    } else if (control == SettingsControl::SoundVolume || control == SettingsControl::MusicVolume) {
        along = unit_clamp(value);
        if (control == SettingsControl::SoundVolume) {
            settings.sound_volume = along;
            sound_tab = along;
        } else {
            settings.music_volume = along;
            music_tab = along;
        }
        // The mutes are written from the checkboxes as they stand, and the
        // levels applied at once: a volume is heard while it is being set.
        settings.sound_mute = sound_mute;
        settings.music_mute = music_mute;
        if (update_audio_levels) {
            update_audio_levels(
                AudioLevels{settings.sound_volume, settings.music_volume, sound_mute, music_mute});
        }
    }
}

// DIVERGENCE: brightness and contrast to the identity, not
// the console's 290 and 210 (`kDefaultBrightness`).
void SettingsMenu::reset_defaults(GameSettings& settings) {
    set_slider(SettingsControl::Brightness, kDefaultBrightness, settings);
    set_slider(SettingsControl::Contrast, kDefaultContrast, settings);
    set_slider(SettingsControl::SoundVolume, 0.8f, settings);
    set_slider(SettingsControl::MusicVolume, 0.5f, settings);
    music_mute = false;
    sound_mute = false;
    if (update_audio_levels) update_audio_levels(AudioLevels{0.8f, 0.5f, false, false});
    show_tips = true;
    floaty_numbers = true;
    show_blood = true;
    settings.text_size = 1;
    settings.camera_sensitivity = 1;
    settings.camera_pitch_range = 1;
    settings.camera_zoom_sensitivity = 1;
    settings.camera_invert_pitch = false;
    settings.touch_control_size = 1;
    settings.touch_control_opacity = 1;
    settings.touch_control_style = 0;
    settings.touch_control_layout = 0;
    settings.touch_control_offsets.fill(0.0f);
    settings.touch_control_edit = false;
    settings.compact_hud = false;
    ios_picker = 0;
}

// on `settings_navigation`'s answer.
bool SettingsMenu::navigate(int code) {
    const int direction = settings_navigation(code);
    if (direction == 0) return false;
    // Every window of this screen is visible and enabled (but see
    // `storage_hidden`), so the walk is one step, wrapping at both ends.
    do {
        focus = direction == 1 ? (focus + 1) % kSettingsControlCount
                               : (focus + kSettingsControlCount - 1) % kSettingsControlCount;
    } while (storage_hidden && focused() == SettingsControl::Storage);
    return true;
}

// `CSettingsXenonTabNavigationHandler::vfunc_6`.
SettingsPress SettingsMenu::press(int code, GameSettings& settings) {
    SettingsPress result;
    const SettingsControl control = focused();
    if (is_checkbox(control)) {
        if (code == kMenuPadA) {
            bool* box = control == SettingsControl::MusicMute           ? &music_mute
                        : control == SettingsControl::SoundMute         ? &sound_mute
                        : control == SettingsControl::ShowTips          ? &show_tips
                        : control == SettingsControl::ShowFloatyNumbers ? &floaty_numbers
                                                                        : &show_blood;
            *box = !*box;
            result.changed = true;
        }
    } else if (control == SettingsControl::Defaults || control == SettingsControl::Storage) {
        if (code == kMenuPadA) {
            if (control == SettingsControl::Defaults) {
                reset_defaults(settings);
                result.changed = true;
            } else {
                if (!storage_hidden) {
                    ios_picker = (ios_picker + 1) % 11;
                    result.changed = true;
                } else {
                    result.storage = true;
                }
            }
        } else if (!storage_hidden && (code == kMenuPadLeft || code == kMenuPadRight)) {
            const int step = code == kMenuPadRight ? 1 : 2;
            if (ios_picker == 4) {
                settings.camera_invert_pitch = !settings.camera_invert_pitch;
            } else if (ios_picker == 9) {
                settings.compact_hud = !settings.compact_hud;
            } else if (ios_picker == 10) {
                settings.touch_control_edit = !settings.touch_control_edit;
            } else {
                int* value = ios_picker == 1 ? &settings.camera_sensitivity
                              : ios_picker == 2 ? &settings.camera_pitch_range
                              : ios_picker == 3 ? &settings.camera_zoom_sensitivity
                              : ios_picker == 5 ? &settings.touch_control_size
                              : ios_picker == 6 ? &settings.touch_control_opacity
                              : ios_picker == 7 ? &settings.touch_control_style
                              : ios_picker == 8 ? &settings.touch_control_layout
                                                : &settings.text_size;
                *value = (std::clamp(*value, 0, 2) + step) % 3;
            }
            result.changed = true;
        }
    } else if (is_slider(control) && (code == kMenuPadLeft || code == kMenuPadRight)) {
        const float step = code == kMenuPadRight ? kSliderStep : -kSliderStep;
        if (control == SettingsControl::Brightness) {
            set_slider(control, step * kBrightnessStepScale + settings.brightness, settings);
        } else if (control == SettingsControl::Contrast) {
            set_slider(control, step * kContrastStepScale + settings.contrast, settings);
        } else if (control == SettingsControl::MusicVolume) {
            set_slider(control, settings.music_volume + step, settings);
        } else {
            set_slider(control, settings.sound_volume + step, settings);
        }
        result.changed = true;
    }

    // B is Done and Y is Help; both close the screen through function 9, so
    // neither drops what was changed.
    if (code == kMenuPadB || code == kMenuPadY) {
        on_click(kLayoutFunctionAccept, settings);
        result.closed = true;
        result.help = code == kMenuPadY;
    }
    return result;
}

// `CSettingsMenu::onClick`.
void SettingsMenu::on_click(int function, GameSettings& settings) {
    if (!open) return;
    if (function == kLayoutFunctionAccept) accepted = true;
    set_open(false, settings);
}

// `CSettingsMenuXenon::vfunc_3`. The PC window's update
// writes a different set of keys and is not what is transcribed.
SettingsUpdate SettingsMenu::update(GameSettings& settings) {
    SettingsUpdate result;
    if (!open && finished) return result;

    const AudioLevels now{settings.sound_volume, settings.music_volume, sound_mute, music_mute};
    if (last.music != now.music || last.sound != now.sound || last.music_mute != now.music_mute ||
        last.sound_mute != now.sound_mute) {
        if (update_audio_levels) update_audio_levels(now);
    }
    last = now;

    if (finished || open) return result;
    finished = true;
    result.finished = true;
    if (!accepted) {
        settings.brightness = opened_brightness;
        settings.contrast = opened_contrast;
        // The volumes are not put back - the sliders wrote them - only the
        // levels re-applied from what the settings now hold.
        if (update_audio_levels) {
            update_audio_levels(AudioLevels{settings.sound_volume, settings.music_volume,
                                            settings.sound_mute, settings.music_mute});
        }
        return result;
    }
    settings.show_tips = show_tips;
    settings.floaty_numbers = floaty_numbers;
    settings.show_blood = show_blood;
    settings.sound_mute = sound_mute;
    settings.music_mute = music_mute;
    if (update_audio_levels) {
        update_audio_levels(
            AudioLevels{settings.sound_volume, settings.music_volume, sound_mute, music_mute});
    }
    accepted = false;
    result.save = true;
    return result;
}

// ---------------------------------------------------------------------------

namespace {

// `optionsmenu.uilayout`, by `tabOrder`. Offered: what the port has a screen or action
// for; Leaderboards, Gift a Friend and Unlock Full Game are platform services, and the
// Achievements slot is the port's debug menu's (a DIVERGENCE, see the header).
constexpr OptionsRow kOptionsRows[kOptionsRowCount] = {
    {"Settings", 1, kLayoutFunctionSettingsMenu, true},
    {"HowToPlay", 2, kLayoutFunctionHelp, true},
    {"Achievements", 3, kPortFunctionDebugMenu, true},
    {"Leaderboards", 4, -1, false},
    {"ExitToTitle", 5, kLayoutFunctionExitGame, true},
    {"UnlockFullGame", 6, -1, false},
    {"invitefriend", 7, -1, false},
};

}  // namespace

// PORT: an accessor of the options page's rows.
const OptionsRow& options_row(int index) { return kOptionsRows[index]; }

// PORT: the debug menu's row only with the debug tools on (config.txt).
bool options_row_offered(int index) {
    const OptionsRow& row = kOptionsRows[index];
    return row.offered && (row.function != kPortFunctionDebugMenu || debug_tools());
}

// wrapping, past a window that is hidden.
bool OptionsMenu::navigate(bool forward) {
    int at = focus;
    do {
        at = forward ? (at + 1) % kOptionsRowCount : (at + kOptionsRowCount - 1) % kOptionsRowCount;
        if (options_row_offered(at)) {
            const bool moved = at != focus;
            focus = at;
            return moved;
        }
    } while (at != focus);
    return false;
}

// `COptionsMenu::onClick`.
OptionsClick OptionsMenu::on_click(int function) {
    OptionsClick click;
    if (!open) return click;
    if (function == kLayoutFunctionClose) {
        click.handled = true;
        click.closed = true;
        open = false;
    } else if (function == kLayoutFunctionSettingsMenu) {
        // `CGameUI::toggleSettings` closes this menu and opens
        // Options.
        click.handled = true;
        click.closed = true;
        click.open_settings = true;
        open = false;
    } else if (function == kLayoutFunctionHelpAndOptions || function == kLayoutFunctionHelp) {
        // The console only: `CGameUI`'s Help toggle, which
        // closes this menu and opens `CHowToMenu` (`core/howto_menu.h`).
        click.handled = true;
        click.closed = true;
        click.open_help = true;
        open = false;
    } else if (function == kPortFunctionDebugMenu) {
        click.handled = true;
        click.closed = true;
        click.open_debug = true;
        open = false;
    } else if (function == kLayoutFunctionExitGame) {
        // The PC waits for the close (`COptionsMenu::update`) before
        // `requestSetGameState(0, 0)`; the console asks at once. Nothing animates a close here.
        click.handled = true;
        click.closed = true;
        click.exit_to_title = true;
        open = false;
    }
    return click;
}

}  // namespace tl::core
