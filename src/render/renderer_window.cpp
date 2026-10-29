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
        TL_LOG_INFO("window: surface made again, %ux%u", window->getWidth(), window->getHeight());
    };
    tl::core::android::set_surface_hooks(std::move(hooks));
#endif
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
