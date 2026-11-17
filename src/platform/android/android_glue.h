// What the Android platform files share: the NativeActivity's glue, which
// android_main() (android_main.cpp) is handed and keeps for the rest of the run.
#pragma once

#include <functional>

struct android_app;
struct AInputEvent;
struct ANativeWindow;
struct AConfiguration;

namespace tl::core::android {

/// Keep the game controller's state from one of the activity's input events
/// (platform_input.cpp). True when the event was the pad's, and so the game's.
bool handle_input_event(const AInputEvent* event);

/// Let go of every button and stick, when the activity loses the events.
void release_pad();

/// The activity's glue, set before the engine starts.
android_app* app();

/// Block until the activity has a window (or is being destroyed): the app in
/// the background has none, and the engine waits there instead of ending.
void wait_for_window();

/// What the renderer does when the window it draws into is taken away (the
/// app sent to the background, a foldable phone switching screens) and when a
/// new one arrives: release its EGL surface, make one on the new window. Both
/// run on the engine's thread, inside pump_events(). Empty while no render
/// window exists.
struct SurfaceHooks {
    std::function<void()> lost;
    std::function<void(ANativeWindow* window, AConfiguration* config)> restored;
};
void set_surface_hooks(SurfaceHooks hooks);

/// What the game does as it goes to the background (its window taken away: Home, Back, the
/// screen off), after the renderer has let its surface go: save the character, since the
/// system may end a program in the background without another word. On the engine's thread,
/// inside pump_events(). Empty for nothing.
void set_background_hook(std::function<void()> hook);

/// The fraction of the screen's pixels the game renders at: `render_scale.txt`
/// in the data root (a percentage, written by the launcher), 1 without it. The
/// system's compositor scales the smaller buffer up to the screen.
float render_scale();

/// What render_scale() is given to OGRE as: its window's "contentScalingFactor".
const char* content_scaling_factor();

/// The window's own size in pixels, where touches arrive, and the size the
/// game renders at (what OGRE makes of it with render_scale()).
int screen_width();
int screen_height();
int render_width();
int render_height();

/// Measure the window again, before a render window is made on it.
void measure_screen();

/// The frame rate asked of the display, for this window and every window after it (0 none).
void set_frame_rate(float fps);

/// Stop and restart the audio stream while the app is in the background
/// (platform_audio.cpp).
void set_audio_paused(bool paused);

}  // namespace tl::core::android
