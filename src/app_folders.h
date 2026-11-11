// Where the program finds its folders, and the Mac app's way out. Shared by main.cpp and
// console_interface.cpp.
#pragma once

#include "core/platform.h"

#include <string>

bool is_directory(const std::string& path);
bool is_regular_file(const std::string& path);

/// The desktop run from a player's folder rather than from the source tree: a pak.zip in the
/// working directory. It then reads the game and OGRE's shader library from there and keeps
/// config.txt there, as a device does.
bool player_folder();

/// DIVERGENCE (platform): the Mac's app bundle. It has no working folder: the game data is the
/// user's `Documents/Torchlight Rekindled/`, OGRE's shader library is in the bundle, the log is
/// always written (into Application Support), and the files are always checked.
#if defined(TL_PLATFORM_MACOS)
inline constexpr bool kMacApp = true;
#else
inline constexpr bool kMacApp = false;
#endif

/// The Mac app's folders into the three options; false, and nothing touched, elsewhere.
bool mac_app_folders(std::string& game_data, std::string& console_ui, std::string& ogre_media);

/// DIVERGENCE (platform): the Mac app leaves without OGRE taking its Cocoa window down, which
/// can hang after Quit; the game has saved by then. Returns, having done nothing, elsewhere.
void mac_quit();

/// `value`, the Mac app having left when it is `quit`.
template <class T>
T mac_quit_on(T value, T quit) {
    if (value == quit) mac_quit();
    return value;
}

/// The menu left without a game to start: false, the Mac app having left when the choice was
/// Quit or its window was closed.
inline bool mac_left_menu(bool quit) {
    if (kMacApp && (quit || !tl::core::should_keep_running())) mac_quit();
    return false;
}

/// The renderer's shutdown; on the Mac, with its window closed, the character saved and the app
/// left instead.
template <class Renderer>
void shut_down(Renderer& renderer, const std::string& save_path) {
    if (kMacApp && !tl::core::should_keep_running()) {
        if (!save_path.empty()) renderer.save_character(save_path);
        mac_quit();
    }
    renderer.shutdown();
}
