// The port's Debug menu (Pause Menu > Debug, with `debug` in config.txt): test switches, none
// of them the game's. Split from renderer_screens.cpp.
#include "render/renderer_impl.h"

namespace tl::render {

namespace {

/// The debug menu's rows, top to bottom.
enum DebugRow {
    kDebugGodspeed = 0,
    kDebugStats,
    kDebugGpuSkinning,
    kDebugGlThread,
    kDebugRenderSystem,
    kDebugLevel,
    kDebugLeave,
    kDebugRowCount
};
constexpr int kDebugLevelTarget = 100;

}  // namespace

void Renderer::Impl::refresh_debug_menu() {
    if (!modal_screen.ready()) return;
    const auto row = [this](int index, const std::string& text) {
        return std::string(debug_menu_row == index ? "> " : "   ") + text;
    };
    const bool gl3plus_wanted = RenderSystemChoice::gl3plus_wanted();
    const bool gl3plus_running =
        root->getRenderSystem()->getName().find("OpenGL 3+") != Ogre::String::npos;
    modal_screen.set_caption("Title", "Debug");
    modal_screen.set_caption(
        "Dialog",
        row(kDebugGodspeed, std::string("GODSPEED: ") + (god_mode ? "ON" : "OFF")) + "\n" +
            row(kDebugStats, std::string("FPS: ") + (stats_overlay_on ? "ON" : "OFF")) + "\n" +
            row(kDebugGpuSkinning,
                std::string("GPU SKINNING: ") + (!gpu_skinning.available() ? "N/A"
                                                 : GpuSkinning::enabled()  ? "ON"
                                                                           : "OFF")) +
            "\n" +
            // Asked as each GL context is made, so it
            // changes with the next level, not now.
            row(kDebugGlThread,
                std::string("GL THREAD: ") +
                    (!GlThread::driver_supports() ? "N/A"
                     : GlThread::wanted() == GlThread::started()
                         ? (GlThread::wanted() ? "ON" : "OFF")
                         : (GlThread::wanted() ? "ON NEXT LEVEL" : "OFF NEXT LEVEL"))) +
            "\n" +
            // Installed as each Root is made, so it too
            // changes with the next level.
            row(kDebugRenderSystem,
                std::string("RENDERER: ") +
                    (!RenderSystemChoice::selectable()
                         ? "N/A"
                         : std::string(gl3plus_wanted ? "GL3PLUS" : "GLES2") +
                               (gl3plus_wanted == gl3plus_running ? "" : " NEXT LEVEL"))) +
            "\n" +
            row(kDebugLevel, "LEVEL " + std::to_string(kDebugLevelTarget) + " (now " +
                                 std::to_string(player_character.level()) + ")") +
            "\n" + row(kDebugLeave, "Exit to main menu"));
    modal_screen.set_hidden("Accept", true);
    modal_screen.set_hidden("Decline", true);
    modal_screen.set_hidden("Ok", false);
    modal_screen.refresh();
}

void Renderer::Impl::open_debug_menu() {
    if (inventory_menu.visible()) {
        inventory_menu.hide();
        sync_stats_page();
        if (automap_initialized && config.minimap) automap.show();
    }
    if (!modal_screen.ready()) {
        // No console interface data to draw it with: + is the switch itself.
        set_godspeed(!god_mode);
        return;
    }
    debug_menu_open = true;
    debug_menu_row = 0;
    debug_stick_held = true;
    // The level stands still under it, as under any modal screen
    // (`game_paused`).
    refresh_debug_menu();
    modal_screen.show();
    play_ui(tl::core::InterfaceSound::MenuOpen);
    TL_LOG_INFO("debug: menu open");
}

void Renderer::Impl::close_debug_menu() {
    if (!debug_menu_open) return;
    debug_menu_open = false;
    modal_screen.hide();
    play_ui(tl::core::InterfaceSound::MenuClose);
    TL_LOG_INFO("debug: menu closed");
}

void Renderer::Impl::drive_debug_menu(const tl::core::InputState& in) {
    if (std::abs(in.move_y) > 0.5f) {
        if (!debug_stick_held) {
            const int step = in.move_y > 0.0f ? -1 : 1;
            debug_menu_row = (debug_menu_row + step + kDebugRowCount) % kDebugRowCount;
            play_ui(tl::core::InterfaceSound::Navigate);
            refresh_debug_menu();
        }
        debug_stick_held = true;
    } else {
        debug_stick_held = false;
    }
    const bool confirm_edge = in.confirm && !confirm_held;
    const bool skip_edge = in.skip && !skip_held;
    if (confirm_edge) {
        switch (debug_menu_row) {
            case kDebugGodspeed:
                set_godspeed(!god_mode);
                refresh_game_hud();
                break;
            case kDebugStats: set_stats_overlay(!stats_overlay_on); break;
            case kDebugGpuSkinning:
                // Applied at the end of this frame (GpuSkinning::step), so the FPS
                // row beside it can show the difference straight away.
                if (gpu_skinning.available()) GpuSkinning::set_enabled(!GpuSkinning::enabled());
                break;
            case kDebugGlThread:
                if (GlThread::driver_supports()) GlThread::set_wanted(!GlThread::wanted());
                break;
            case kDebugRenderSystem:
                if (RenderSystemChoice::selectable()) {
                    RenderSystemChoice::set_gl3plus_wanted(!RenderSystemChoice::gl3plus_wanted());
                }
                break;
            case kDebugLevel:
                debug_level_row(kDebugLevelTarget);
                refresh_game_hud();
                break;
            default:  // kDebugLeave
                close_debug_menu();
                quit_requested = true;
                return;
        }
        refresh_debug_menu();
    } else if (skip_edge) {
        close_debug_menu();
    }
}

}  // namespace tl::render
