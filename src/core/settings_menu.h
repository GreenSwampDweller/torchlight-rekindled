// The Options screen and the Pause Menu that opens it, two `CDropdownMenu`s of the console
// build: the Pause Menu `COptionsMenu` (`media/ui/optionsmenu.uilayout`) and Options,
// `CSettingsMenuXenon` (`media/ui/settingsmenu_xenon.uilayout`, name).
// From the Xbox 360 build (the PC's `CSettingsMenu` is a different window), with the Linux
// build where they agree: `COptionsMenu::onClick`, `CSettingsMenu::onClick`,
// `CGameUI::toggleOptions`, `toggleSettings`, `CSoundManager::updateAudioLevels`.
// Pure: what the pad does and what is written into the settings, not the drawing.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace tl::core {

/// The console's brightness and contrast defaults: Reset Defaults and
/// the screen's two statics.
inline constexpr float kConsoleBrightness = 290.0f;
inline constexpr float kConsoleContrast = 210.0f;

/// DIVERGENCE (2026-10-07): the port's defaults are the pair whose ramp is the
/// identity, so a fresh settings file and Reset Defaults draw the frame untouched.
inline constexpr float kDefaultBrightness = 257.0f;
inline constexpr float kDefaultContrast = 256.0f;

/// The keys of `CSettings` these two screens read and write. Initial values are Reset
/// Defaults', except brightness and contrast (above).
struct GameSettings {
    /// iOS-only text size for item tooltips and quest reward/dialog text: 0 small, 1 default,
    /// 2 large. Other platforms keep the authored UI size.
    int text_size = 1;
    float sound_volume = 0.8f;              ///< `KSETTINGS_F_SOUNDVOLUME`
    float music_volume = 0.5f;              ///< `KSETTINGS_F_MUSICVOLUME`
    bool sound_mute = false;                ///< `KSETTINGS_SOUNDMUTE`
    bool music_mute = false;                ///< `KSETTINGS_MUSICMUTE`
    bool show_tips = true;                  ///< `KSETTINGS_SHOW_TIPS`
    bool floaty_numbers = true;             ///< `KSETTINGS_FLOATY_NUMBERS`, "Show Damage Info"
    bool show_blood = true;                 ///< `KSETTINGS_SHOW_BLOOD`
    float brightness = kDefaultBrightness;  ///< X360 key: 100 .. 600
    float contrast = kDefaultContrast;      ///< X360 key: 100 .. 400
};

/// The settings as a text file and back. The format is the port's own (the
/// console's is a `CDynamicPropertyFile` on its storage device): one
/// `KEY:value` a line, unknown keys passed over, a missing key left as it was.
std::string write_game_settings(const GameSettings& settings);
void read_game_settings(const std::string& text, GameSettings& settings);

/// One `CSoundManager::updateAudioLevels(sound, music, soundMute, musicMute)`
/// call.
struct AudioLevels {
    float sound = 1.0f;
    float music = 1.0f;
    bool sound_mute = false;
    bool music_mute = false;
};

/// What the mixer plays an `AudioLevels` at.
struct AudioGains {
    float effects = 1.0f;  ///< every sound but the music
    float music = 1.0f;
};

/// `CSoundManager::updateAudioLevels`. Quirk kept: the
/// sound volume is set on both FMOD's master sound group and master channel group, so an
/// effect is scaled twice; the music also plays through the master channel group, so the
/// sound mute silences it too.
AudioGains audio_gains(const AudioLevels& levels);

/// The pad as a housing or dropdown screen is told of it (the button codes of
/// and the handlers below).
inline constexpr int kMenuPadUp = 1;
inline constexpr int kMenuPadDown = 2;
inline constexpr int kMenuPadLeft = 3;
inline constexpr int kMenuPadRight = 4;
inline constexpr int kMenuPadA = 5;
inline constexpr int kMenuPadB = 6;
inline constexpr int kMenuPadX = 7;
inline constexpr int kMenuPadY = 8;
inline constexpr int kMenuPadStart = 0xd;
inline constexpr int kMenuPadBack = 0xe;
inline constexpr int kMenuPadStickUp = 0x11;
inline constexpr int kMenuPadStickDown = 0x12;
inline constexpr int kMenuPadStickLeft = 0x13;
inline constexpr int kMenuPadStickRight = 0x14;

/// `ELayoutFunction`, as these two screens switch on it.
inline constexpr int kLayoutFunctionExitGame = 0;  ///< `guiExitGame`
inline constexpr int kLayoutFunctionClose = 6;
inline constexpr int kLayoutFunctionCancel = 8;           ///< settings: close, changes dropped
inline constexpr int kLayoutFunctionAccept = 9;           ///< settings: close, changes kept
inline constexpr int kLayoutFunctionSettingsMenu = 0x5e;  ///< `GUISETTINGSMENU`
/// The names' table, one `std::string` per function (static constructors,
/// e.g.): `GUISETTINGSMENU` 0x5e .. `GUIHELP` 0x64.
inline constexpr int kLayoutFunctionHelpAndOptions = 0x62;  ///< the main menu's Help
inline constexpr int kLayoutFunctionHelp = 0x64;            ///< `GUIHELP`, the Pause Menu's
/// Not the original's: the port's debug menu, on the Pause Menu's
/// Achievements slot (see `options_row`).
inline constexpr int kPortFunctionDebugMenu = -2;

// ---------------------------------------------------------------------------
// Options: CSettingsMenuXenon

/// The windows of the Options screen that take the focus, numbered by their
/// `tabOrder` in `settingsmenu_xenon.uilayout`.
enum class SettingsControl {
    None = 0,
    MusicVolume = 1,
    MusicMute = 2,
    SoundVolume = 3,
    SoundMute = 4,
    ShowTips = 5,
    ShowFloatyNumbers = 6,
    ShowBlood = 7,
    Brightness = 8,
    Contrast = 9,
    Storage = 10,
    Defaults = 11,
};
inline constexpr int kSettingsControlCount = 11;

/// The window's name in the layout.
const char* settings_control_window(SettingsControl control);
/// A slider bar's tab window (`sliderTabMusic`, ...); null for the rest.
const char* settings_slider_tab_window(SettingsControl control);

/// `CSettingsXenonTabNavigationHandler::vfunc_1`: 2 is the previous
/// window (D-pad up, stick up **and stick left**), 1 the next (D-pad down, stick down and
/// right), 0 neither. D-pad left and right change a value instead.
int settings_navigation(int code);

/// What one button did on the Options screen.
struct SettingsPress {
    bool closed = false;   ///< the screen closed (B, or Y on its way to Help)
    bool help = false;     ///< Y: Help is toggled once the screen has closed
    bool storage = false;  ///< A on Set Storage Device (the console's device picker)
    bool changed = false;  ///< a window's look changed
};

/// What `update` did.
struct SettingsUpdate {
    bool finished = false;  ///< the close completed on this update
    bool save = false;      ///< ... accepted: the settings file is written
};

struct SettingsMenu {
    /// `CDropdownMenu +0x30` (X360 `+0x18`).
    bool open = false;
    /// `+0x31` (X360 `+0x19`): closed and done. Cleared by an opening and by
    /// a close, set by the `update` that carries the close out.
    bool finished = true;
    /// X360 `+0x109` (Linux `+0xc1`): closed through function 9.
    bool accepted = false;

    // The five checkboxes' `isSelected()` (X360 `+0x11c` .. `+0x12c`).
    bool music_mute = false;
    bool sound_mute = false;
    bool show_tips = true;
    bool floaty_numbers = true;
    bool show_blood = true;

    // Where each slider's tab stands along its bar, 0 to 1 (X360 `+0x130` ..
    // `+0x13c`; the fraction multiplies the bar's free run by).
    float music_tab = 0.0f;
    float sound_tab = 0.0f;
    float brightness_tab = 0.0f;
    float contrast_tab = 0.0f;

    /// Brightness and contrast at the opening, put
    /// back by a close that is not accepted. The volumes have no such copy.
    float opened_brightness = kDefaultBrightness;
    float opened_contrast = kDefaultContrast;

    /// The levels `update` last saw.
    ///
    AudioLevels last;

    /// The tab list's current window, an index into the list sorted by
    /// `tabOrder` (so `SettingsControl` value - 1). Kept across openings, as
    /// the shared `TabNavigationHandler` does.
    int focus = 0;

    /// DIVERGENCE (platform): a Switch has no Xbox 360 storage device; a port that hides Set
    /// Storage Device sets this, and the walk passes over it.
    bool storage_hidden = false;
    /// DIVERGENCE (platform): iOS's Storage row sets `GameSettings::text_size` instead.
    bool text_size_row = false;

    /// `CSoundManager::updateAudioLevels`, wherever the screen calls it.
    std::function<void(const AudioLevels&)> update_audio_levels;

    SettingsControl focused() const { return static_cast<SettingsControl>(focus + 1); }

    /// `CSettingsMenuXenon::vfunc_7`.
    void set_open(bool opening, GameSettings& settings);

    /// a slider set to `value`, in its setting's own units.
    /// `control` is the bar (`MusicVolume`, `SoundVolume`, `Brightness`,
    /// `Contrast`).
    void set_slider(SettingsControl control, float value, GameSettings& settings);

    /// Reset Defaults.
    void reset_defaults(GameSettings& settings);

    /// The tab list's own half of a button: one step for a code
    /// `settings_navigation` gives a direction. True when
    /// the focus moved.
    bool navigate(int code);

    /// `CSettingsXenonTabNavigationHandler::vfunc_6`: a
    /// button on the focused window.
    SettingsPress press(int code, GameSettings& settings);

    /// `CSettingsMenu::onClick` (X360 `vfunc_9`).
    void on_click(int function, GameSettings& settings);

    /// `CSettingsMenuXenon::vfunc_3`, once a frame.
    SettingsUpdate update(GameSettings& settings);
};

// ---------------------------------------------------------------------------
// The Pause Menu: COptionsMenu

/// One button of the Pause Menu, as `optionsmenu.uilayout` authors it.
struct OptionsRow {
    const char* window;  ///< the `[UI]` twin's name for it (`options_Xenon.layout`)
    int tab_order;
    int function;  ///< its `onClick`, as an `ELayoutFunction`; -1 where the port has none
    bool offered;  ///< drawn by this port
};
inline constexpr int kOptionsRowCount = 7;
/// The seven rows sorted by `tabOrder`: Options, Help, Achievements, Leaderboards, Exit to
/// Title, Unlock Full Game, Gift a Friend.
/// DIVERGENCE (2026-09-30): the Achievements slot carries the port's debug menu.
const OptionsRow& options_row(int index);
/// Whether the row is drawn and reached: `offered`, and the debug menu's only
/// with the debug tools on (core/run_config.h).
bool options_row_offered(int index);

/// What a click on the Pause Menu asks of the game.
struct OptionsClick {
    bool handled = false;
    bool closed = false;         ///< the menu closed
    bool open_settings = false;  ///< `CGameUI::toggleSettings`
    bool open_help = false;      ///< the Help screen's toggle
    bool exit_to_title = false;  ///< `requestSetGameState(0, 0)`
    bool open_debug = false;     ///< the port's debug menu (`kPortFunctionDebugMenu`)
};

struct OptionsMenu {
    /// `CDropdownMenu +0x30` (X360 `+0x18`), closed from `CDropdownMenu::CDropdownMenu`
    /// and `COptionsMenu::COptionsMenu`.
    ///
    bool open = false;
    /// The tab list's current row, kept across openings.
    int focus = 0;

    /// One step of the tab list over the rows this port offers.
    /// True when the focus moved.
    bool navigate(bool forward);

    /// `COptionsMenu::onClick` (X360 `vfunc_9`).
    OptionsClick on_click(int function);
};

}  // namespace tl::core
