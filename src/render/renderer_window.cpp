// The render window's surface on Android, which the app going to the background takes
// away and gives back, the desktop window's size and handle as made, and the camera's aspect to the
// window. Split from renderer_world.cpp.
#include "render/renderer_impl.h"

#if defined(TL_PLATFORM_ANDROID)
#include "platform/android/android_glue.h"

#include <OgreAndroidEGLWindow.h>
#endif

#include <chrono>
#include <thread>
#include <utility>

namespace tl::render {

void Renderer::Impl::watch_window_surface() {
    if (!config.headless) tl::core::platform_set_frame_rate(static_cast<float>(config.fps_limit));
#if defined(TL_PLATFORM_ANDROID)
    tl::core::android::SurfaceHooks hooks;
    hooks.lost = [this] {
        if (window != nullptr)
            static_cast<Ogre::AndroidEGLWindow*>(window)->_notifySurfaceDestroyed();
    };
    hooks.restored = [this](ANativeWindow* native, AConfiguration* configuration) {
        if (window == nullptr) return;
        static_cast<Ogre::AndroidEGLWindow*>(window)->_notifySurfaceCreated(native, configuration);
        // Another screen of a foldable phone: the viewports and the camera to its size. The
        // interface stays laid out for the size the run started at.
        for (unsigned short i = 0; i < window->getNumViewports(); ++i) {
            window->getViewport(i)->_updateDimensions();
        }
        fit_camera_to_window();
        sync_gl_context();
        TL_LOG_INFO("window: surface made again, %ux%u", window->getWidth(), window->getHeight());
    };
    tl::core::android::set_surface_hooks(std::move(hooks));

    // A two-screen handheld's other display: the activity puts a window on it when asked and
    // its surface arrives here, now if a renderer before this one (New Game) already had it.
    const bool second_screen = config.second_screen && !config.headless;
    tl::core::android::SecondScreenHooks second;
    if (second_screen) {
        second.arrived = [this](ANativeWindow* native, AConfiguration* configuration) {
            second_screen_arrived(native, configuration);
        };
        second.gone = [this] { second_screen_gone(); };
    }
    tl::core::android::set_second_screen_hooks(std::move(second));
    tl::core::android::show_second_screen(second_screen);
    if (second_screen) {
        if (ANativeWindow* native = tl::core::android::second_screen_window()) {
            second_screen_arrived(native, nullptr);
        }
    }
#endif
}

#if defined(TL_PLATFORM_ANDROID)
// The second display's surface: a render window of its own on it the first time, a new EGL
// surface for that window after. The window shares the main one's GL objects (OGRE makes its
// context in the main context's share group) and is not held to its display's refresh: only
// the main window's swap paces the frame.
void Renderer::Impl::second_screen_arrived(void* native_window, void* configuration) {
    if (root == nullptr || window == nullptr || native_window == nullptr) return;
    try {
        if (second_window == nullptr) {
            // The depth and stencil sizes the main window asks for (platform_window_params):
            // the same EGL config, which sharing with its context needs.
            Ogre::NameValuePairList params{{"FSAA", "0"}};
            params["externalWindowHandle"] =
                std::to_string(reinterpret_cast<std::size_t>(native_window));
            if (configuration != nullptr) {
                params["androidConfig"] =
                    std::to_string(reinterpret_cast<std::size_t>(configuration));
            }
            params["preserveContext"] = "true";
            params["maxDepthBufferSize"] = "24";
            params["maxStencilBufferSize"] = "8";
            second_window =
                root->createRenderWindow("Torchlight Rekindled second screen", 0, 0, true, &params);
        } else {
            static_cast<Ogre::AndroidEGLWindow*>(second_window)
                ->_notifySurfaceCreated(native_window, configuration);
            for (unsigned short i = 0; i < second_window->getNumViewports(); ++i) {
                second_window->getViewport(i)->_updateDimensions();
            }
        }
    } catch (const Ogre::Exception& e) {
        TL_LOG_ERROR("second screen: no window on it: %s", e.getFullDescription().c_str());
        sync_gl_context();
        return;
    }
    try {
        second_window->setVSyncEnabled(false);
    } catch (const Ogre::Exception& e) {
        TL_LOG_WARN("second screen: held to its display's refresh: %s",
                    e.getFullDescription().c_str());
    }
    sync_gl_context();
    TL_LOG_INFO("second screen: window %ux%u", second_window->getWidth(),
                second_window->getHeight());
    apply_second_screen(true);
}

// The display's surface taken away (the game in the background, the display unplugged): the
// map goes back to the main window and the EGL surface is released. The window and its context
// stay for the surface that comes back.
void Renderer::Impl::second_screen_gone() {
    if (second_window == nullptr) return;
    apply_second_screen(false);
    // The frame's last swap was this window's, so its context and surface are the current
    // ones, and EGL keeps a current surface alive after it is destroyed: the main window's
    // made current first, so the surface is gone when this returns.
    if (root != nullptr && window != nullptr && window->isActive()) {
        root->getRenderSystem()->_setRenderTarget(window);
    }
    static_cast<Ogre::AndroidEGLWindow*>(second_window)->_notifySurfaceDestroyed();
}

// AndroidEGLWindow::create() and _notifySurfaceCreated() make the window's own context current
// in EGL without telling the render system, which goes on issuing the next target's calls to
// whichever context it believes current. With two windows that is two contexts: each is made
// the render target in turn, the main window last, so the two agree again.
void Renderer::Impl::sync_gl_context() {
    if (root == nullptr || window == nullptr || second_window == nullptr) return;
    Ogre::RenderSystem* render_system = root->getRenderSystem();
    if (render_system == nullptr) return;
    if (second_window->isActive()) render_system->_setRenderTarget(second_window);
    if (window->isActive()) render_system->_setRenderTarget(window);
}
#endif

// The second display's window taken up by, or taken from, everything drawn on it. Not before
// the map exists: its backdrop viewport there is what the rest is drawn in.
void Renderer::Impl::apply_second_screen(bool up) {
    const bool there = up && second_window != nullptr && second_window->isActive() &&
                       automap_initialized;
    if (automap_initialized) automap.set_second_screen(there ? second_window : nullptr);
    game_hud.set_second_screen(there, there ? second_window->getWidth() : 0,
                               there ? second_window->getHeight() : 0);
    if (!there) {
        second_screen.detach();
        return;
    }
    // The HUD's strip along the top, the map in the room under it, the backdrop round both.
    const float band = game_hud.second_band();
    automap.set_second_screen_top(band);
    SecondScreen::Shape shape;
    shape.strip_bottom = band * static_cast<float>(second_window->getHeight());
    float left = 0.0f, top = 0.0f, side = 0.0f;
    if (automap.second_screen_square(left, top, side)) {
        shape.centre_x = left + side * 0.5f;
        shape.centre_y = top + side * 0.5f;
        shape.radius = side * 0.5f;
    }
    second_screen.attach(second_window, &game_hud, shape);
}

void Renderer::Impl::adopt_window_size() {
#if !defined(TL_PLATFORM_SWITCH) && !defined(TL_PLATFORM_ANDROID) && !defined(TL_PLATFORM_IOS)
    if (window == nullptr) return;
    std::uintptr_t native_window = 0;
    window->getCustomAttribute("WINDOW", &native_window);
    tl::core::set_input_window(native_window);
    // A headless run draws offscreen at the size asked for; its hidden window is never read.
    if (config.headless) return;
#if !defined(_WIN32)
    // X11: where OGRE's XRandR mode switch does not take (XWayland, several monitors), the
    // window manager makes the fullscreen window the monitor's size, in its own time.
    if (config.fullscreen) {
        const unsigned asked_width = window->getWidth();
        const unsigned asked_height = window->getHeight();
        for (int tries = 0; tries < 30; ++tries) {
            window->windowMovedOrResized();
            if (window->getWidth() != asked_width || window->getHeight() != asked_height) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
#endif
    window->windowMovedOrResized();
    const unsigned width = window->getWidth();
    const unsigned height = window->getHeight();
    if (width == 0 || height == 0 || (width == config.width && height == config.height)) return;
    TL_LOG_INFO("window: asked for %ux%u, made %ux%u; laid out for %ux%u", config.width,
                config.height, width, height, width, height);
    config.width = width;
    config.height = height;
#endif
}

// After the present and before the next frame's input, so the wait adds no lag. A frame that
// ran late starts the count again rather than hurrying the next ones to catch up.
void Renderer::Impl::pace_frame() {
    if (config.fps_limit == 0 || config.headless) return;
    using Clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / static_cast<double>(config.fps_limit)));
    const Clock::time_point now = Clock::now();
    if (now < next_frame) {
        tl::core::platform_sleep_until(next_frame);
        next_frame += period;
    } else if (now - next_frame < period) {
        next_frame += period;
    } else {
        next_frame = now + period;
    }
}

void Renderer::Impl::fit_camera_to_window() const {
    if (camera == nullptr || window == nullptr || window->getNumViewports() == 0) return;
    const Ogre::Viewport* viewport = window->getViewport(0);
    camera->setAspectRatio(static_cast<Ogre::Real>(viewport->getActualWidth()) /
                           static_cast<Ogre::Real>(viewport->getActualHeight()));
}

}  // namespace tl::render
