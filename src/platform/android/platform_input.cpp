// Android input: a game controller, read as the desktop reads an Xbox pad.
//
// Key and joystick-motion events arrive through the glue's looper; handle_input_event() keeps the
// pad's state, and poll_platform_input() reads it as XInput is read, axes scaled to XInput's
// integer ranges so the console's deadzones and trigger filter (`core/gamepad.h`) apply. Without a
// controller the touch screen's on-screen pad (`core/touch_pad.h`) is folded in.
#include "core/input.h"

#include "core/gamepad.h"
#include "core/log.h"
#include "core/platform.h"
#include "core/touch_pad.h"
#include "platform/android/android_glue.h"

#include <android/input.h>
#include <android/keycodes.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>

namespace tl::core {

namespace {

enum Button : std::uint32_t {
    kA = 1u << 0,
    kB = 1u << 1,
    kX = 1u << 2,
    kY = 1u << 3,
    kL1 = 1u << 4,
    kR1 = 1u << 5,
    kL2 = 1u << 6,
    kR2 = 1u << 7,
    kThumbL = 1u << 8,
    kThumbR = 1u << 9,
    kStart = 1u << 10,
    kSelect = 1u << 11,
    kUp = 1u << 12,
    kDown = 1u << 13,
    kLeft = 1u << 14,
    kRight = 1u << 15,
};

struct Pad {
    std::uint32_t keys = 0;                            ///< buttons held, from key events
    std::uint32_t pressed = 0;                         ///< pressed since the last poll, held or not
    float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;  ///< Android's -1..1, y down
    float lt = 0.0f, rt = 0.0f;                        ///< 0..1
    float hat_x = 0.0f, hat_y = 0.0f;                  ///< the D-pad as axes, y down
};

Pad g_pad;

static_assert(static_cast<std::uint32_t>(kA) == TouchPad::kA &&
                  static_cast<std::uint32_t>(kStart) == TouchPad::kStart &&
                  static_cast<std::uint32_t>(kRight) == TouchPad::kRight,
              "the on-screen pad's buttons are this pad's bits");

/// A stick or trigger moved this far is the player's hand on the controller,
/// not a resting stick's noise.
constexpr float kPadInUse = 0.5f;

/// The touch screen's events, to the on-screen pad, in the frame's pixels: a
/// render scale makes those fewer than the window's, where touches arrive.
bool handle_touch(const AInputEvent* event) {
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION ||
        (AInputEvent_getSource(event) & AINPUT_SOURCE_TOUCHSCREEN) != AINPUT_SOURCE_TOUCHSCREEN) {
        return false;
    }
    TouchPad& pad = touch_pad();
    const int32_t action = AMotionEvent_getAction(event);
    const int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
    const auto index = static_cast<size_t>((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                                           AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    const float sx = android::screen_width() > 0 ? static_cast<float>(android::render_width()) /
                                                       static_cast<float>(android::screen_width())
                                                 : 1.0f;
    const float sy = android::screen_height() > 0 ? static_cast<float>(android::render_height()) /
                                                        static_cast<float>(android::screen_height())
                                                  : 1.0f;
    switch (masked) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
            pad.touch_down(AMotionEvent_getPointerId(event, index),
                           AMotionEvent_getX(event, index) * sx,
                           AMotionEvent_getY(event, index) * sy);
            break;
        case AMOTION_EVENT_ACTION_MOVE:
            for (size_t i = 0; i < AMotionEvent_getPointerCount(event); ++i) {
                pad.touch_move(AMotionEvent_getPointerId(event, i),
                               AMotionEvent_getX(event, i) * sx, AMotionEvent_getY(event, i) * sy);
            }
            break;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP:
            pad.touch_up(AMotionEvent_getPointerId(event, index));
            break;
        case AMOTION_EVENT_ACTION_CANCEL: pad.release_all(); break;
        default: break;
    }
    return true;
}

std::uint32_t button_of(int32_t key) {
    switch (key) {
        case AKEYCODE_BUTTON_A: return kA;
        case AKEYCODE_BUTTON_B: return kB;
        case AKEYCODE_BUTTON_X: return kX;
        case AKEYCODE_BUTTON_Y: return kY;
        case AKEYCODE_BUTTON_L1: return kL1;
        case AKEYCODE_BUTTON_R1: return kR1;
        case AKEYCODE_BUTTON_L2: return kL2;
        case AKEYCODE_BUTTON_R2: return kR2;
        case AKEYCODE_BUTTON_THUMBL: return kThumbL;
        case AKEYCODE_BUTTON_THUMBR: return kThumbR;
        case AKEYCODE_BUTTON_START: return kStart;
        case AKEYCODE_BUTTON_SELECT: return kSelect;
        case AKEYCODE_DPAD_UP: return kUp;
        case AKEYCODE_DPAD_DOWN: return kDown;
        case AKEYCODE_DPAD_LEFT: return kLeft;
        case AKEYCODE_DPAD_RIGHT: return kRight;
        default: return 0;
    }
}

bool from_pad(const AInputEvent* event) {
    const int32_t source = AInputEvent_getSource(event);
    return (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD ||
           (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK ||
           (source & AINPUT_SOURCE_DPAD) == AINPUT_SOURCE_DPAD;
}

/// Android's -1..1 as XInput's signed 16-bit axis, with up positive.
std::int16_t stick_axis(float value, bool flip) {
    const float v = std::clamp(flip ? -value : value, -1.0f, 1.0f);
    return static_cast<std::int16_t>(std::lround(v * 32767.0f));
}

/// Android's 0..1 as XInput's 8-bit trigger.
std::uint8_t trigger_axis(float value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

}  // namespace

namespace android {

bool handle_input_event(const AInputEvent* event) {
    if (handle_touch(event)) return true;
    if (!from_pad(event)) return false;
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
        int32_t key = AKeyEvent_getKeyCode(event);
        // Some pads send their B as Back. From a pad it is B; the phone's own
        // Back gesture comes from no pad and stays the system's.
        if (key == AKEYCODE_BACK) key = AKEYCODE_BUTTON_B;
        const std::uint32_t bit = button_of(key);
        if (bit == 0) return false;
        const int32_t action = AKeyEvent_getAction(event);
        // A press is also latched until the game next polls: a press and
        // its release can both arrive between two frames, and the game must
        // still see the press once.
        if (action == AKEY_EVENT_ACTION_DOWN) g_pad.keys |= bit, g_pad.pressed |= bit;
        if (action == AKEY_EVENT_ACTION_DOWN) touch_pad().set_visible(false);
        if (action == AKEY_EVENT_ACTION_UP) g_pad.keys &= ~bit;
        return true;
    }
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION &&
        (AInputEvent_getSource(event) & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
        const auto axis = [event](int32_t which) {
            return AMotionEvent_getAxisValue(event, which, 0);
        };
        g_pad.lx = axis(AMOTION_EVENT_AXIS_X);
        g_pad.ly = axis(AMOTION_EVENT_AXIS_Y);
        g_pad.rx = axis(AMOTION_EVENT_AXIS_Z);
        g_pad.ry = axis(AMOTION_EVENT_AXIS_RZ);
        // A pad reports its triggers as one pair or the other.
        g_pad.lt = std::max(axis(AMOTION_EVENT_AXIS_LTRIGGER), axis(AMOTION_EVENT_AXIS_BRAKE));
        g_pad.rt = std::max(axis(AMOTION_EVENT_AXIS_RTRIGGER), axis(AMOTION_EVENT_AXIS_GAS));
        g_pad.hat_x = axis(AMOTION_EVENT_AXIS_HAT_X);
        g_pad.hat_y = axis(AMOTION_EVENT_AXIS_HAT_Y);
        const float moved = std::max({std::fabs(g_pad.lx), std::fabs(g_pad.ly), std::fabs(g_pad.rx),
                                      std::fabs(g_pad.ry), g_pad.lt, g_pad.rt,
                                      std::fabs(g_pad.hat_x), std::fabs(g_pad.hat_y)});
        if (moved > kPadInUse) touch_pad().set_visible(false);
        return true;
    }
    return false;
}

void release_pad() {
    g_pad = Pad{};
    touch_pad().release_all();
}

}  // namespace android

InputState poll_platform_input() {
    InputState in;
    // The on-screen pad folded in: its buttons with the controller's, and on
    // each axis whichever of the two is pushed further.
    const TouchPad::State touch = touch_pad().take();
    Pad p = g_pad;
    const auto further = [](float a, float b) { return std::fabs(b) > std::fabs(a) ? b : a; };
    p.lx = further(p.lx, touch.lx);
    p.ly = further(p.ly, touch.ly);
    p.rx = further(p.rx, touch.rx);
    p.ry = further(p.ry, touch.ry);
    const std::uint32_t b = p.keys | p.pressed | touch.held | touch.pressed |
                            (p.hat_x < -0.5f ? kLeft : 0u) | (p.hat_x > 0.5f ? kRight : 0u) |
                            (p.hat_y < -0.5f ? kUp : 0u) | (p.hat_y > 0.5f ? kDown : 0u);

    std::int16_t lx = stick_axis(p.lx, false);
    std::int16_t ly = stick_axis(p.ly, true);
    filter_stick(lx, ly, kLeftStickDeadzone);
    float x = static_cast<float>(lx) / kStickMax;
    float y = static_cast<float>(ly) / kStickMax;
    in.stick_x = x;
    in.stick_y = y;
    if (b & kLeft) x -= 1.0f, in.dpad_x -= 1.0f;
    if (b & kRight) x += 1.0f, in.dpad_x += 1.0f;
    if (b & kDown) y -= 1.0f;
    if (b & kUp) y += 1.0f;

    std::int16_t rx = stick_axis(p.rx, false);
    std::int16_t ry = stick_axis(p.ry, true);
    filter_stick(rx, ry, kRightStickDeadzone);
    in.zoom = static_cast<float>(ry) / kStickMax;
    in.look_x = static_cast<float>(rx) / kStickMax;
    // DIVERGENCE (platform): a pinch zooms, under the Free Camera mod (render/touch_camera.h).
    in.zoom_delta = touch.pinch_dolly;

    in.confirm = (b & kA) != 0;
    in.skip = (b & kB) != 0;
    in.attack = (b & kX) != 0;
    in.action_y = (b & kY) != 0;
    in.quit = (b & kStart) != 0;
    in.inventory = (b & kSelect) != 0;
    // The triggers through the console's own dead range; a
    // pad that sends them as buttons instead counts as pulled all the way.
    std::uint8_t lt = (b & kL2) != 0 ? 255 : trigger_axis(p.lt);
    std::uint8_t rt = (b & kR2) != 0 ? 255 : trigger_axis(p.rt);
    filter_trigger(lt);
    filter_trigger(rt);
    in.menu_prev = lt > 0;
    in.menu_next = rt > 0;
    in.tab_prev = (b & kL1) != 0;
    in.tab_next = (b & kR1) != 0;
    in.map_toggle = (b & kThumbR) != 0;
    in.fps_toggle = (b & kThumbL) != 0;
    // The same two D-pad jobs the console and the desktop give it.
    in.pet_stance = (b & kUp) != 0;
    in.pet_town = (b & kDown) != 0;

    const float m = std::sqrt(x * x + y * y);
    if (m > 1.0f) {
        x /= m;
        y /= m;
    }
    in.move_x = x;
    in.move_y = y;
    g_pad.pressed = 0;
    return in;
}

namespace {

/// The activity's method `name` with the JNI signature `signature`, or null (an exception cleared).
jmethodID activity_method(JNIEnv* env, jobject activity, const char* name, const char* signature) {
    jclass type = env->GetObjectClass(activity);
    jmethodID method = env->GetMethodID(type, name, signature);
    env->DeleteLocalRef(type);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return method;
}

}  // namespace

// `XamShowKeyboardUI`'s counterpart: the system keyboard in a dialog with the
// console's title and guide line, capped at `max_chars` (GameActivity.startTextEntry). Blocks
// until it closes, as on the 360, with the activity's events pumped meanwhile.
bool text_entry(const std::string& title, const std::string& guide, std::string& text,
                unsigned max_chars) {
    std::string scripted;
    if (take_scripted_text(scripted)) {
        if (scripted.size() > max_chars) scripted.resize(max_chars);
        text = scripted;
        return true;
    }
    android_app* glue = android::app();
    if (glue == nullptr || glue->activity == nullptr) return false;
    JavaVM* vm = glue->activity->vm;
    jobject activity = glue->activity->clazz;
    JNIEnv* env = nullptr;
    // A thread already attached stays attached after.
    const bool attached = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK;
    if (!attached && vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
    const jmethodID start =
        activity_method(env, activity, "startTextEntry",
                        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V");
    const jmethodID state = activity_method(env, activity, "textEntryState", "()I");
    const jmethodID typed = activity_method(env, activity, "textEntryText", "()[B");
    int answer = 2;
    if (start != nullptr && state != nullptr && typed != nullptr) {
        jstring j_title = env->NewStringUTF(title.c_str());
        jstring j_guide = env->NewStringUTF(guide.c_str());
        jstring j_text = env->NewStringUTF(text.c_str());
        env->CallVoidMethod(activity, start, j_title, j_guide, j_text,
                            static_cast<jint>(max_chars));
        env->DeleteLocalRef(j_title);
        env->DeleteLocalRef(j_guide);
        env->DeleteLocalRef(j_text);
        answer = env->ExceptionCheck() ? 2 : 0;
        if (answer != 0) env->ExceptionClear();
        while (answer == 0 && should_keep_running()) {
            pump_events();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            answer = env->CallIntMethod(activity, state);
        }
        if (answer == 1) {
            auto* bytes = static_cast<jbyteArray>(env->CallObjectMethod(activity, typed));
            if (bytes != nullptr) {
                const jsize length = env->GetArrayLength(bytes);
                std::string answered(static_cast<size_t>(length), '\0');
                env->GetByteArrayRegion(bytes, 0, length,
                                        reinterpret_cast<jbyte*>(answered.data()));
                env->DeleteLocalRef(bytes);
                text = answered;
            }
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (!attached) vm->DetachCurrentThread();
    if (answer == 1) TL_LOG_INFO("keyboard: '%s'", text.c_str());
    return answer == 1;
}

// The keyboard is the system's (text_entry); nothing is typed into the window.
bool typing_in_window() { return false; }
std::string take_platform_typed_text() { return {}; }

}  // namespace tl::core
