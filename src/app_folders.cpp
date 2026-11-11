#include "app_folders.h"

#include "core/log.h"

#include <sys/stat.h>

#include <cstdlib>
#if defined(TL_PLATFORM_MACOS)
#include <filesystem>
#endif

bool is_directory(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool is_regular_file(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool player_folder() { return is_regular_file("pak.zip") || is_regular_file("Pak.zip"); }

bool mac_app_folders(std::string& game_data, std::string& console_ui, std::string& ogre_media) {
#if defined(TL_PLATFORM_MACOS)
    const char* home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') {
        game_data = std::string(home) + "/Documents/Torchlight Rekindled/";
        const std::string own_ui = game_data + "console_ui";
        if (is_directory(own_ui)) console_ui = own_ui;
    }
    const std::string resources = tl::core::platform_program_folder() + "../Resources/";
    const std::string bundled_ogre_media = resources + "OGRE/Media/";
    if (is_directory(bundled_ogre_media + "RTShaderLib")) ogre_media = bundled_ogre_media;
    if (!game_data.empty()) {
        std::error_code error;
        std::filesystem::create_directories(game_data, error);
    }
    return true;
#else
    (void)game_data;
    (void)console_ui;
    (void)ogre_media;
    return false;
#endif
}

void mac_quit() {
#if defined(TL_PLATFORM_MACOS)
    tl::core::log_shutdown();
    std::_Exit(0);
#endif
}
