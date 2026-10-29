// `config.txt`: what a player can switch on beside the game - the port's own
// tools, not the original's settings (those are `save/settings.txt`).
//
//   debug=0   the debug tools: the debug menu, the bestiary, the wardrobe editor,
//             Choose Level, the frame dumps and the two-stick screenshot, and the
//             log. Off, the menus do not offer them at all.
//   logs=0    the log file, on its own (debug=1 turns it on too).
//   fps=0     read, not written: the frame-rate overlay is a mod now (core/mods.h), and this
//             line of a file from before says whether that mod starts on.
//   fps_limit=0   the most frames a second the game draws, 0 for no limit (Android: 60).
//   free_camera=0 read, not written: the free camera is a mod now (core/mods.h), and this line
//                 of a file from before says whether that mod starts on (iOS: 1).
//   mods_menu=1   the mods page before the game (not on iOS, `kModsPage`); 0 skips it and
//                 starts without any mods. The page's "Don't show this menu again" sets it, and
//                 the Android launcher's "Mod menu" switch.
//
// The desktop builds read four more (a device's screen is its own):
//
//   width=1280, height=720   the window's size, or the screen's in fullscreen
//   fullscreen=0             1: fullscreen at that resolution
//   vsync=1                  0: frames as fast as the machine draws them
//
// A device makes the file with these defaults where its game files are (the
// Switch's `sdmc:/switch/torchlight/`, the folder the Android launcher was
// pointed at). The desktop reads one in its working directory if there is
// one and makes none: its debug tools stay on, which the verification runs
// rely on.
#pragma once

#include <string>

namespace tl::core {

/// Whether this platform shows the mods page before the game (render/renderer_mods_menu.cpp):
/// all but iOS, which takes `mods.dat` as it is.
#if defined(TL_PLATFORM_SWITCH) || defined(TL_PLATFORM_ANDROID) || !defined(TL_PLATFORM_IOS)
inline constexpr bool kModsPage = true;
#else
inline constexpr bool kModsPage = false;
#endif

struct RunConfig {
    bool debug = false;
    bool logs = false;
    bool fps = false;
    /// The desktop's resolution; 0 leaves the build's own (and keeps the lines out of a
    /// new file, as a device's).
    unsigned width = 0;
    unsigned height = 0;
    bool fullscreen = false;
    bool vsync = true;
    unsigned fps_limit = 0;
#if defined(TL_PLATFORM_IOS)
    bool free_camera = true;
#else
    bool free_camera = false;
#endif
    /// Where `kModsPage`: false skips the page and starts with no mods at all.
    bool mods_menu = true;
};

/// The text a new `config.txt` holds: `defaults` written out, with a line
/// saying what each value does; the resolution's lines only when `defaults`
/// has one (the desktop's).
std::string run_config_text(const RunConfig& defaults);

/// `text`'s values over `defaults`: `key=value` lines, `0` or `1` (anything
/// else non-zero is on), keys and values without regard to case or spaces;
/// `#` starts a comment; unknown keys are passed over.
RunConfig parse_run_config(const std::string& text, const RunConfig& defaults);

/// `text` with `key` set to `value`: its line rewritten in place, or a line
/// added when it has none. Every other line - comments included - is kept.
std::string set_run_config_value(const std::string& text, const std::string& key, bool value);

/// Read `path` over `defaults`; when it is absent and `create` is set, write
/// the defaults there first. The result becomes the run's (run_config()).
RunConfig load_run_config(const std::string& path, const RunConfig& defaults, bool create);

/// Keep one value in the run's file and in run_config(). False when the file
/// cannot be written.
bool save_run_config_value(const std::string& key, bool value);

/// The run's values, as load_run_config() left them (the defaults before it).
const RunConfig& run_config();

/// Whether the port's debug tools are offered this run.
inline bool debug_tools() { return run_config().debug; }

}  // namespace tl::core
