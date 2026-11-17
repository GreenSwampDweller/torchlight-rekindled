// A second display's surface, from the activity's thread to the engine's.
//
// GameActivity (android/java) puts a Presentation - a window on another display - on the
// handheld's second screen when the engine asks (show_second_screen()), and calls
// secondSurface() from the UI thread with the surface of the view inside it: a Surface when it
// is made or changes size, null when it is taken away. The engine's thread picks that up in
// pump_events() (service_second_screen()) and tells the renderer through SecondScreenHooks,
// which makes an EGL surface on it or lets its own go.
//
// A surface taken away must not be drawn into after surfaceDestroyed() returns, so the UI
// thread waits there (briefly: the engine may have ended) until the engine has let go.
#include "platform/android/android_glue.h"

#include <android/log.h>
#include <android/looper.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>
#include <vector>

namespace tl::core::android {

namespace {

std::mutex g_mutex;
std::condition_variable g_let_go;
// The UI thread's: the newest surface (its own reference) or null, and a count of the changes.
ANativeWindow* g_latest = nullptr;
unsigned g_serial = 0;
// The engine's: the change up to which it holds nothing older (under the mutex, the UI thread
// waits on it), the surface it holds and the change that brought it.
unsigned g_released = 0;
ANativeWindow* g_current = nullptr;
unsigned g_shown = 0;
SecondScreenHooks g_second_hooks;

// GameActivity.secondSurface(Surface): on the UI thread.
void JNICALL second_surface(JNIEnv* env, jclass, jobject surface) {
    ANativeWindow* window = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
    std::unique_lock<std::mutex> lock(g_mutex);
    if (g_latest != nullptr) ANativeWindow_release(g_latest);
    g_latest = window;
    const unsigned serial = ++g_serial;
    // The engine may be waiting in the looper for the main window.
    if (android_app* glue = app(); glue != nullptr && glue->looper != nullptr) {
        ALooper_wake(glue->looper);
    }
    if (window != nullptr) return;
    // serial - g_released as a signed difference: a later change has let go of this one too.
    const bool let_go = g_let_go.wait_for(lock, std::chrono::milliseconds(2000), [serial] {
        return static_cast<int>(g_released - serial) >= 0;
    });
    if (!let_go) {
        __android_log_print(ANDROID_LOG_WARN, "torchlight",
                            "second screen: the engine did not let its surface go in time");
    }
}

// GameActivity.secondTap(float, float): on the UI thread. A few are kept; a tap made while the
// engine is not taking them (a loading screen) is not acted on long after.
struct Tap {
    float x;
    float y;
};
std::mutex g_tap_mutex;
std::vector<Tap> g_taps;
constexpr size_t kMostTaps = 8;

void JNICALL second_tap(JNIEnv*, jclass, jfloat x, jfloat y) {
    const std::lock_guard<std::mutex> lock(g_tap_mutex);
    if (g_taps.size() >= kMostTaps) g_taps.erase(g_taps.begin());
    g_taps.push_back(Tap{x, y});
}

// The engine's thread attached for a call into the activity, and detached after if it was not.
struct Jni {
    JavaVM* vm = nullptr;
    JNIEnv* env = nullptr;
    jobject activity = nullptr;
    bool attached_here = false;

    Jni() {
        android_app* glue = app();
        if (glue == nullptr || glue->activity == nullptr) return;
        vm = glue->activity->vm;
        activity = glue->activity->clazz;
        if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return;
        env = nullptr;
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            env = nullptr;
            return;
        }
        attached_here = true;
    }
    ~Jni() {
        if (env != nullptr && env->ExceptionCheck()) env->ExceptionClear();
        if (attached_here) vm->DetachCurrentThread();
    }
    Jni(const Jni&) = delete;
    Jni& operator=(const Jni&) = delete;
};

}  // namespace

// NativeActivity loads the library itself, so the activity's `native` methods are not found
// by name: they are registered here.
void register_second_screen() {
    Jni jni;
    if (jni.env == nullptr) return;
    jclass type = jni.env->GetObjectClass(jni.activity);
    const JNINativeMethod methods[] = {
        {"secondSurface", "(Landroid/view/Surface;)V", reinterpret_cast<void*>(second_surface)},
        {"secondTap", "(FF)V", reinterpret_cast<void*>(second_tap)},
    };
    const bool registered = jni.env->RegisterNatives(type, methods, 2) == JNI_OK;
    jni.env->DeleteLocalRef(type);
    if (!registered) {
        // An activity without the method (NativeActivity itself, started by hand).
        __android_log_print(ANDROID_LOG_WARN, "torchlight",
                            "second screen: the activity has no secondSurface()");
    }
}

void show_second_screen(bool on) {
    Jni jni;
    if (jni.env == nullptr) return;
    jclass type = jni.env->GetObjectClass(jni.activity);
    const jmethodID method = jni.env->GetMethodID(type, "setSecondScreen", "(Z)V");
    jni.env->DeleteLocalRef(type);
    if (method == nullptr) return;  // the exception is cleared as `jni` goes
    jni.env->CallVoidMethod(jni.activity, method, static_cast<jboolean>(on ? JNI_TRUE : JNI_FALSE));
}

bool take_second_screen_tap(float& x, float& y) {
    const std::lock_guard<std::mutex> lock(g_tap_mutex);
    if (g_taps.empty()) return false;
    x = g_taps.front().x;
    y = g_taps.front().y;
    g_taps.erase(g_taps.begin());
    return true;
}

void set_second_screen_hooks(SecondScreenHooks hooks) { g_second_hooks = std::move(hooks); }

ANativeWindow* second_screen_window() { return g_current; }

void service_second_screen(bool main_window_up) {
    ANativeWindow* next = nullptr;
    unsigned serial = 0;
    {
        const std::lock_guard<std::mutex> lock(g_mutex);
        serial = g_serial;
        const bool nothing_to_show = g_shown == serial || g_latest == nullptr || !main_window_up;
        if (g_released == serial && nothing_to_show) return;
        next = g_latest;
        if (next != nullptr) ANativeWindow_acquire(next);
    }
    if (g_current != nullptr && g_shown != serial) {
        if (g_second_hooks.gone) g_second_hooks.gone();
        ANativeWindow_release(g_current);
        g_current = nullptr;
        __android_log_print(ANDROID_LOG_INFO, "torchlight", "second screen: surface let go");
    }
    {
        const std::lock_guard<std::mutex> lock(g_mutex);
        g_released = serial;
    }
    g_let_go.notify_all();
    if (next == nullptr) return;
    if (!main_window_up || g_current != nullptr) {
        ANativeWindow_release(next);
        return;
    }
    g_current = next;
    g_shown = serial;
    __android_log_print(ANDROID_LOG_INFO, "torchlight", "second screen: surface up, %dx%d",
                        ANativeWindow_getWidth(next), ANativeWindow_getHeight(next));
    android_app* glue = app();
    if (g_second_hooks.arrived) g_second_hooks.arrived(next, glue != nullptr ? glue->config : nullptr);
}

}  // namespace tl::core::android
