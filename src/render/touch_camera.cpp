// The touch camera (render/touch_camera.h).
#include "render/touch_camera.h"

#include "core/gamepad.h"
#include "core/log.h"
#include "core/run_config.h"

#include <algorithm>
#include <cmath>

namespace tl::render {

bool TouchCamera::enabled() { return tl::core::run_config().free_camera; }

void TouchCamera::set_enabled(bool on) { (void)tl::core::save_run_config_value("free_camera", on); }

void TouchCamera::take(const tl::core::InputState& in, bool blocked) {
    const bool on = enabled() && !blocked;
    orbit_input = on ? in.look_x : 0.0f;
    pitch_input = on ? in.camera_pitch : 0.0f;
    zoom_delta = on ? in.zoom_delta : 0.0f;
    const bool r3 = in.map_toggle && !r3_held;
    r3_held = in.map_toggle;
    took_r3 = r3 && on && (orbit != 0.0f || pitch_adjust != 0.0f || zoomed);
}

void TouchCamera::step(float dt, bool cinematic, float& camera_yaw, float& dolly,
                       float camera_mult, float sensitivity, float pitch_range,
                       float zoom_sensitivity, bool invert_pitch) {
    // A heading this did not leave is a new level's: it starts the turn and tilt again.
    if (tracking && camera_yaw != heading) {
        orbit = 0.0f;
        pitch_adjust = 0.0f;
    }
    if (orbit == 0.0f) base_yaw = camera_yaw;
    if (took_r3) {
        // R3: the console's view again - its heading, its angle, the dolly's far end.
        camera_yaw = base_yaw;
        orbit = 0.0f;
        pitch_adjust = 0.0f;
        dolly = tl::core::kZoomDefaultScale * camera_mult;
        took_r3 = false;
        TL_LOG_INFO("camera: R3 put the free camera back");
    }
    if (!enabled()) {
        if (orbit != 0.0f) camera_yaw = base_yaw;
        orbit = 0.0f;
        pitch_adjust = 0.0f;
    } else if (!cinematic) {
        if (orbit_input != 0.0f && dt > 0.0f) {
            const float turn = orbit_input * 1.8f * sensitivity * dt;
            orbit += turn;
            camera_yaw += turn;
        }
        if (pitch_input != 0.0f && dt > 0.0f) {
            const float direction = invert_pitch ? -1.0f : 1.0f;
            pitch_adjust = std::clamp(pitch_adjust + direction * pitch_input * 0.8f * sensitivity * dt,
                                      -pitch_range, pitch_range);
        }
        if (zoom_delta != 0.0f) {
            const float farthest = tl::core::kZoomDefaultScale * camera_mult;
            dolly += zoom_delta * zoom_sensitivity;
            if (dolly < tl::core::kZoomMin) dolly = tl::core::kZoomMin;
            if (dolly > farthest) dolly = farthest;
        }
    }
    zoomed = dolly != tl::core::kZoomDefaultScale * camera_mult;
    heading = camera_yaw;
    tracking = true;
}

// The tilt moves the eye along an arc about the focus, at the same distance; the turn is
// about +Y.
Ogre::Vector3 TouchCamera::turned(Ogre::Vector3 offset, float dolly) const {
    if (pitch_adjust != 0.0f) {
        const float across = std::sqrt(0.5f) * dolly;
        const float eye_distance = std::sqrt(across * across + offset.y * offset.y);
        const float angle = std::clamp(std::atan2(offset.y, across) + pitch_adjust, 0.2f, 1.35f);
        const float k = eye_distance * std::cos(angle) / across;
        offset = Ogre::Vector3(offset.x * k, eye_distance * std::sin(angle), offset.z * k);
    }
    if (orbit != 0.0f) {
        const float c = std::cos(orbit);
        const float s = std::sin(orbit);
        offset = Ogre::Vector3(offset.x * c + offset.z * s, offset.y, offset.z * c - offset.x * s);
    }
    return offset;
}

}  // namespace tl::render
