// Which console interface a PC pak is drawn with. Split from main.cpp.
#include "console_interface.h"

#include "app_folders.h"

#include "core/platform.h"

std::string console_interface(const std::string& asked,
                              const std::vector<std::string>& extra_data) {
#if defined(TL_PLATFORM_SWITCH)
    // No `console_ui/` on the card: the pack in the .nro's RomFS.
    (void)extra_data;
    if (!is_directory(asked) && is_directory("romfs:/ui_pack")) return "romfs:/ui_pack";
#elif defined(TL_PLATFORM_ANDROID)
    // android_main.cpp has chosen already: `console_ui/` beside the pak, else the pack.
    (void)extra_data;
#else
    // No --console-ui: a player's folder's own `console_ui/` (main.cpp's player folder:
    // the program run beside its pak.zip), else the pack beside the program. Not over an
    // --extra-data that already has the console's interface; `--console-ui none` runs a
    // PC pak without either.
    if (!asked.empty()) return asked;
    for (const std::string& extra : extra_data) {
        if (is_regular_file(extra + "/media/UI/hud_xenon.layout")) return asked;
    }
    const bool player_folder = is_regular_file("pak.zip") || is_regular_file("Pak.zip");
    if (player_folder && is_directory("console_ui")) return "console_ui";
    std::string pack = tl::core::platform_program_folder() + "ui_pack";
    if (is_directory(pack)) return pack;
#endif
    return asked;
}
