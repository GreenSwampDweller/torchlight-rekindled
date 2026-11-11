// The Android build's entry point.
//
// A NativeActivity loads libtorchlight.so and the NDK's glue calls
// android_main() on a thread of its own. This waits for the activity's first
// window, reads the run's arguments from `args.txt` in the data root (an
// `adb push` sets a run's switches, where the desktop has a command line), and
// hands over to the engine's main(), which on this target is tl_main().
//
// The window going away (the app sent to the background) does not end the
// run: the engine's thread waits in pump_events() until a window is back, with
// the GL context kept and only its surface released (the renderer's hooks).
//
// When the engine returns, the activity is finished and the process ends with
// it: OGRE's singletons and the engine's statics are made for one run per
// process, and Android would otherwise start the next launch in the same one.
#include "platform/android/android_glue.h"

#include "core/platform.h"
#include "core/touch_pad.h"

#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

// main.cpp's main() on this target.
int tl_main(int argc, char** argv);

namespace tl::core::android {

namespace {

android_app* g_app = nullptr;
bool g_started = false;
bool g_in_background = false;
SurfaceHooks g_hooks;

float g_frame_rate = 0.0f;
int g_screen_width = 0;
int g_screen_height = 0;
float g_render_scale = 1.0f;
char g_scaling_factor[32] = {};

// The window's own size. A size OGRE set for its buffers (a render scale)
// stays on the window and is what ANativeWindow_getWidth() reports, so it is
// put back to the window's own first.
void measure_window(ANativeWindow* window) {
    ANativeWindow_setBuffersGeometry(window, 0, 0, 0);
    g_screen_width = ANativeWindow_getWidth(window);
    g_screen_height = ANativeWindow_getHeight(window);
}

void on_command(android_app* glue, int32_t command) {
    switch (command) {
        case APP_CMD_INIT_WINDOW:
            measure_window(glue->window);
            if (g_frame_rate > 0.0f) {
                ANativeWindow_setFrameRate(glue->window, g_frame_rate,
                                           ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT);
            }
            __android_log_print(ANDROID_LOG_INFO, "torchlight", "window up: %dx%d", g_screen_width,
                                g_screen_height);
            if (g_in_background) {
                g_in_background = false;
                if (g_hooks.restored) g_hooks.restored(glue->window, glue->config);
                set_audio_paused(false);
                __android_log_print(ANDROID_LOG_INFO, "torchlight", "back from the background");
            }
            break;
        case APP_CMD_TERM_WINDOW:
            // The surface must be gone before this returns: the glue lets the
            // system take the window as soon as the command has been handled.
            if (g_started) {
                g_in_background = true;
                if (g_hooks.lost) g_hooks.lost();
                set_audio_paused(true);
                release_pad();
                __android_log_print(ANDROID_LOG_INFO, "torchlight",
                                    "window taken away: waiting for it to come back");
            }
            break;
        case APP_CMD_LOST_FOCUS:
            // A button released while another window had the events would
            // otherwise stay held.
            release_pad();
            break;
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONTENT_RECT_CHANGED: {
            // The same window at another size (a desktop mode's window maximised or
            // dragged): the surface is remade on it as it is after a fold, which lays the
            // viewports and the camera out for the new size. Measuring puts the window's
            // buffers back to its own size, so when nothing changed the size OGRE set for
            // them (the render scale) is put back at once.
            ANativeWindow* window = glue->window;
            if (window == nullptr || !g_started || g_in_background) break;
            const int old_width = g_screen_width, old_height = g_screen_height;
            measure_window(window);
            if (g_screen_width == old_width && g_screen_height == old_height) {
                if (g_render_scale != 1.0f) {
                    ANativeWindow_setBuffersGeometry(window, render_width(), render_height(), 0);
                }
                break;
            }
            __android_log_print(ANDROID_LOG_INFO, "torchlight", "window resized: %dx%d -> %dx%d",
                                old_width, old_height, g_screen_width, g_screen_height);
            if (g_hooks.lost) g_hooks.lost();
            if (g_hooks.restored) g_hooks.restored(window, glue->config);
            break;
        }
        default: break;
    }
}

// The pad's events are the game's (platform_input.cpp); everything else -
// the phone's own Back gesture first - stays the system's. Back sends the game
// to the background (android/java, GameActivity), as Home does.
int32_t on_input(android_app* glue, AInputEvent* event) {
    (void)glue;
    return handle_input_event(event) ? 1 : 0;
}

void wait_for(android_app* glue, bool (*done)(android_app*)) {
    while (!done(glue)) {
        android_poll_source* source = nullptr;
        int events = 0;
        if (ALooper_pollOnce(-1, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0 &&
            source != nullptr) {
            source->process(glue, source);
        }
    }
}

// What the platform knows first: the window's own size (the interface and the
// camera's aspect are laid out for the size the engine is told, and the
// phone's is not the desktop's 1280x720) and where OGRE's shader library was
// pushed: `RTShaderLib/` at the data root (by the Android deploy step; the console names
// its directory at build time).
// Then `args.txt`: the arguments of the run, separated by white space - double
// quotes keep one together, as a shell's do (a `--pad-script` has `wait x30`)
// - which may override them. Absent is the ordinary case, and the defaults
// (main.cpp's device build) are the game.
// The first line of a small text file, or empty.
std::string first_line(const std::string& path) {
    std::FILE* in = std::fopen(path.c_str(), "rb");
    if (in == nullptr) return {};
    char buffer[1024] = {};
    const bool read = std::fgets(buffer, sizeof buffer, in) != nullptr;
    std::fclose(in);
    std::string line = read ? buffer : "";
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    return line;
}

// Where the game's files are: the folder the launcher activity had the player
// choose (android/java, `game_data.txt` in the app's directory), else the
// app's directory itself, where a deploy over adb pushes them.
std::string game_data_folder() {
    std::string folder = first_line(platform_data_root() + "game_data.txt");
    if (folder.empty()) return platform_data_root();
    if (folder.back() != '/') folder += '/';
    return folder;
}

// Where OGRE's shader library is: unpacked by the launcher from the APK into
// the app's internal files (`RTShaderLib/`), else beside the game data, where a
// deploy over adb pushes it.
std::string shader_library_root() {
    const char* internal = app()->activity->internalDataPath;
    if (internal != nullptr) {
        const std::string root = std::string(internal) + "/";
        if (std::FILE* probe =
                std::fopen((root + "RTShaderLib/RTSLib_Lighting.glsl").c_str(), "rb")) {
            std::fclose(probe);
            return root;
        }
    }
    return platform_data_root();
}

// The launcher's resolution choice: a percentage of the screen's width and
// height, 100 without the file. OGRE takes it as the factor the screen is
// larger than the buffer by, and renders at its window size times 1/factor;
// the same arithmetic here, so the engine is told the size OGRE makes.
void read_render_scale() {
    const std::string line = first_line(platform_data_root() + "render_scale.txt");
    const int percent = std::clamp(line.empty() ? 100 : std::atoi(line.c_str()), 25, 100);
    if (percent == 100) return;
    std::snprintf(g_scaling_factor, sizeof g_scaling_factor, "%.6f", 100.0 / percent);
    g_render_scale = 1.0f / static_cast<float>(std::strtod(g_scaling_factor, nullptr));
}

// The console interface for a PC pak: `console_ui/` beside it when the player
// put one there, else the port's own interface pack, which the launcher
// unpacked from the APK into the app's internal files. Beside the Xbox pak
// neither is mounted (the pak has its own).
std::string console_interface(const std::string& data) {
    struct stat info {};
    const std::string beside = data + "console_ui";
    if (stat(beside.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) return beside;
    const char* internal = app()->activity->internalDataPath;
    if (internal == nullptr) return beside;
    const std::string pack = std::string(internal) + "/ui_pack";
    return stat(pack.c_str(), &info) == 0 && S_ISDIR(info.st_mode) ? pack : beside;
}

std::vector<std::string> read_arguments() {
    const std::string data = game_data_folder();
    std::vector<std::string> args{"torchlight",
                                  "--width",
                                  std::to_string(render_width()),
                                  "--height",
                                  std::to_string(render_height()),
                                  "--ogre-media",
                                  shader_library_root(),
                                  "--game-data",
                                  data,
                                  "--console-ui",
                                  console_interface(data)};
    const std::string path = platform_data_root() + "args.txt";
    std::FILE* in = std::fopen(path.c_str(), "rb");
    if (in == nullptr) return args;
    std::string word;
    bool quoted = false;
    bool any = false;  // "" is an argument, an empty one
    for (int c = std::fgetc(in); c != EOF; c = std::fgetc(in)) {
        if (c == '"') {
            quoted = !quoted;
            any = true;
        } else if (std::isspace(c) != 0 && !quoted) {
            if (any) args.push_back(word);
            word.clear();
            any = false;
        } else {
            word.push_back(static_cast<char>(c));
            any = true;
        }
    }
    if (any) args.push_back(word);
    std::fclose(in);
    return args;
}

}  // namespace

android_app* app() { return g_app; }

void wait_for_window() {
    wait_for(g_app,
             [](android_app* g) { return g->window != nullptr || g->destroyRequested != 0; });
}

void set_surface_hooks(SurfaceHooks hooks) { g_hooks = std::move(hooks); }

float render_scale() { return g_render_scale; }

const char* content_scaling_factor() {
    return g_scaling_factor[0] != '\0' ? g_scaling_factor : nullptr;
}

int screen_width() { return g_screen_width; }
int screen_height() { return g_screen_height; }

int render_width() { return static_cast<int>(static_cast<float>(g_screen_width) * g_render_scale); }
int render_height() {
    return static_cast<int>(static_cast<float>(g_screen_height) * g_render_scale);
}

void measure_screen() {
    if (g_app != nullptr && g_app->window != nullptr) measure_window(g_app->window);
}

// A request: the system may run the display at that rate, or keep its own.
void set_frame_rate(float fps) {
    g_frame_rate = fps;
    if (g_app == nullptr || g_app->window == nullptr || !(fps > 0.0f)) return;
    ANativeWindow_setFrameRate(g_app->window, fps, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT);
}

}  // namespace tl::core::android

void android_main(android_app* glue) {
    using namespace tl::core::android;
    g_app = glue;
    glue->onAppCmd = on_command;
    glue->onInputEvent = on_input;

    wait_for(glue, [](android_app* g) { return g->window != nullptr || g->destroyRequested != 0; });
    if (glue->destroyRequested != 0) return;

    read_render_scale();
    std::vector<std::string> args = read_arguments();
    std::vector<char*> argv;
    for (std::string& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    std::string line;
    for (const std::string& arg : args) line += " " + arg;
    __android_log_print(ANDROID_LOG_INFO, "torchlight", "starting:%s", line.c_str());

    // The on-screen pad (core/touch_pad.h) starts off the screen: a tap puts
    // it there, and a controller's first press takes it off again.
    tl::core::touch_pad().set_screen(static_cast<float>(render_width()),
                                     static_cast<float>(render_height()));

    g_started = true;
    const int result = tl_main(static_cast<int>(args.size()), argv.data());
    __android_log_print(ANDROID_LOG_INFO, "torchlight", "the engine returned %d", result);

    g_hooks = SurfaceHooks{};
    ANativeActivity_finish(glue->activity);
    wait_for(glue, [](android_app* g) { return g->destroyRequested != 0; });
    _exit(result);
}
