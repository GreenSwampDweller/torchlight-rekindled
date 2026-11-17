// Switch 2 only: the EGL display lives as long as the process. OGRE ends it whenever its Root goes
// (`EGLSupport::stop`), and New Game's restart makes a new Root at once; the Switch 2's driver
// cannot be initialised again in the same process ("Couldn't initialize EGLDisplay"). So
// eglTerminate (wrapped, cmake/switch.cmake) releases what the old Root made on the display - its
// window surfaces and contexts, which OGRE's Switch window leaves to eglTerminate - and keeps the
// display itself; the new Root's eglInitialize on it succeeds. The display is ended for real at
// exit, by an atexit handler registered after Mesa's own, so it runs before Mesa frees its
// displays and well before libounce closes the GPU driver.
#if defined(__SWITCH2__)

#include <EGL/egl.h>

#include <cstdlib>

extern "C" {

EGLBoolean __real_eglTerminate(EGLDisplay display);
EGLSurface __real_eglCreateWindowSurface(EGLDisplay display, EGLConfig config,
                                         EGLNativeWindowType window, const EGLint* attribs);
EGLContext __real_eglCreateContext(EGLDisplay display, EGLConfig config, EGLContext share,
                                   const EGLint* attribs);

namespace {

constexpr int kTracked = 16;

EGLDisplay g_kept = EGL_NO_DISPLAY;
bool g_at_exit = false;
// What OGRE made on the display, released at the next eglTerminate. Destroying a handle already
// destroyed is refused by EGL (EGL_BAD_SURFACE / EGL_BAD_CONTEXT) and harmless.
EGLSurface g_surfaces[kTracked];
EGLContext g_contexts[kTracked];
int g_surface_count = 0;
int g_context_count = 0;

void release_made(EGLDisplay display) {
    (void)eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    for (int i = 0; i < g_surface_count; ++i) (void)eglDestroySurface(display, g_surfaces[i]);
    for (int i = 0; i < g_context_count; ++i) (void)eglDestroyContext(display, g_contexts[i]);
    g_surface_count = 0;
    g_context_count = 0;
}

void end_display_at_exit() {
    if (g_kept == EGL_NO_DISPLAY) return;
    release_made(g_kept);
    (void)__real_eglTerminate(g_kept);
    g_kept = EGL_NO_DISPLAY;
}

}  // namespace

EGLSurface __wrap_eglCreateWindowSurface(EGLDisplay display, EGLConfig config,
                                         EGLNativeWindowType window, const EGLint* attribs) {
    const EGLSurface surface = __real_eglCreateWindowSurface(display, config, window, attribs);
    if (surface != EGL_NO_SURFACE && g_surface_count < kTracked)
        g_surfaces[g_surface_count++] = surface;
    return surface;
}

EGLContext __wrap_eglCreateContext(EGLDisplay display, EGLConfig config, EGLContext share,
                                   const EGLint* attribs) {
    const EGLContext context = __real_eglCreateContext(display, config, share, attribs);
    if (context != EGL_NO_CONTEXT && g_context_count < kTracked)
        g_contexts[g_context_count++] = context;
    return context;
}

EGLBoolean __wrap_eglTerminate(EGLDisplay display) {
    if (display == EGL_NO_DISPLAY) return __real_eglTerminate(display);
    release_made(display);
    g_kept = display;
    if (!g_at_exit) g_at_exit = std::atexit(end_display_at_exit) == 0;
    return EGL_TRUE;
}

}  // extern "C"

#endif  // __SWITCH2__
