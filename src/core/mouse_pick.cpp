#include "core/mouse_pick.h"

#include <cmath>

namespace tl::core {

namespace {

void expand(ScreenBox& box, const Vec3& p) {
    if (p.x < box.low.x) box.low.x = p.x;
    if (p.y < box.low.y) box.low.y = p.y;
    if (p.z < box.low.z) box.low.z = p.z;
    if (box.high.x < p.x) box.high.x = p.x;
    if (box.high.y < p.y) box.high.y = p.y;
    if (box.high.z < p.z) box.high.z = p.z;
}

}  // namespace

ScreenMatrix screen_matrix(const float projection[16], const float view[16]) {
    ScreenMatrix out;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out.m[r][c] =
                projection[r * 4 + 0] * view[0 * 4 + c] + projection[r * 4 + 1] * view[1 * 4 + c] +
                projection[r * 4 + 2] * view[2 * 4 + c] + projection[r * 4 + 3] * view[3 * 4 + c];
        }
    }
    return out;
}

Vec3 to_screen(const ScreenMatrix& matrix, const Vec3& p) {
    const auto& m = matrix.m;
    const float w = 1.0f / (m[3][0] * p.x + m[3][1] * p.y + m[3][2] * p.z + m[3][3]);
    return {(m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3]) * w,
            (m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3]) * w,
            (m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3]) * w};
}

bool pick_on_screen(const ScreenMatrix& matrix, const Vec3& position) {
    const Vec3 at = to_screen(matrix, position);
    return -1.1f < at.x && at.x <= 1.1f && -1.1f <= at.y && at.y <= 1.1f;
}

// CLevel::findCharacterAtScreenCoordinates, the box.
ScreenBox character_screen_box(const ScreenMatrix& matrix, const Vec3& position,
                               const Vec3& local_min, const Vec3& local_max) {
    // The low side is the minimum times 0.7, no nearer the middle than -0.55; the high
    // side is 0.55 whatever the model (the maximum is read for its height alone).
    float low_x = local_min.x * 0.7f;
    if (-0.55f < low_x) low_x = -0.55f;
    const float high_x = 0.55f;
    float low_z = local_min.z * 0.7f;
    if (-0.55f < low_z) low_z = -0.55f;
    const float high_z = 0.55f;
    const float bottom = local_min.y;
    const float top = local_max.y * 0.9f;

    const auto corner = [&](float x, float y, float z) {
        return to_screen(matrix, Vec3{position.x + x, position.y + y, position.z + z});
    };
    ScreenBox box;
    box.low = box.high = corner(low_x, bottom, low_z);
    expand(box, corner(high_x, top, high_z));
    expand(box, corner(low_x, bottom, high_z));
    expand(box, corner(high_x, bottom, high_z));
    expand(box, corner(high_x, bottom, low_z));
    expand(box, corner(low_x, top, high_z));
    expand(box, corner(high_x, top, high_z));
    expand(box, corner(high_x, top, low_z));
    return box;
}

// CLevel::findItemAtScreenCoordinates, the box.
ScreenBox item_screen_box(const ScreenMatrix& matrix, const Vec3& position, const Vec3& local_min,
                          const Vec3& local_max) {
    const Vec3 lo{local_min.x * 0.7f, local_min.y * 0.9f, local_min.z * 0.7f};
    const Vec3 hi{local_max.x * 0.7f, local_max.y * 0.9f, local_max.z * 0.7f};
    const auto corner = [&](float x, float y, float z) {
        return to_screen(matrix, Vec3{position.x + x, position.y + y, position.z + z});
    };
    ScreenBox box;
    box.low = box.high = corner(lo.x, lo.y, lo.z);
    expand(box, corner(hi.x, hi.y, hi.z));
    expand(box, corner(lo.x, lo.y, hi.z));
    expand(box, corner(hi.x, lo.y, hi.z));
    expand(box, corner(hi.x, lo.y, lo.z));
    expand(box, corner(lo.x, hi.y, hi.z));
    expand(box, corner(hi.x, hi.y, lo.z));
    expand(box, corner(lo.x, hi.y, lo.z));
    return box;
}

float pick_priority(const PickCandidate& candidate) {
    float priority = std::fabs(candidate.box.low.z);
    if (candidate.alignment == 0) priority += 800.0f;
    if (candidate.alignment == 1) priority += 1200.0f;
    return priority;
}

bool pick_at(const PickCandidate* candidates, std::size_t count, float x, float y,
             std::uint64_t* out_id) {
    float best = 99999.0f;
    bool found = false;
    for (std::size_t i = 0; i < count; ++i) {
        const PickCandidate& c = candidates[i];
        if (!(0.0f < c.box.high.z)) continue;
        const float priority = pick_priority(c);
        if (priority < best && c.box.low.x <= x && c.box.low.y <= y && x <= c.box.high.x &&
            y <= c.box.high.y) {
            best = priority;
            found = true;
            if (out_id != nullptr) *out_id = c.id;
        }
    }
    return found;
}

// MATH::distanceToPlane
float distance_to_plane(const Vec3& origin, const Vec3& direction, const Vec3& normal, float d) {
    const float along = direction.x * normal.x + direction.y * normal.y + direction.z * normal.z;
    if (along == 0.0f) return -1.0f;
    return (d - (normal.x * origin.x + normal.y * origin.y + normal.z * origin.z)) / along;
}

Vec3 pointer_on_plane(const Vec3& ray_origin, const Vec3& ray_direction, float plane_y) {
    const float t = distance_to_plane(ray_origin, ray_direction, Vec3{0.0f, 1.0f, 0.0f}, plane_y);
    return {t * ray_direction.x + ray_origin.x, 0.0f, ray_direction.z * t + ray_origin.z};
}

// CCameraControl::processInput
float wheel_dolly(float dolly, int wheel, bool middle_pressed, float camera_multiplier) {
    const float far = camera_multiplier * 28.5f;
    float moved = static_cast<float>(wheel) * -0.01f + dolly;
    if (14.0f <= moved) {
        if (far < moved) moved = far;
    } else {
        moved = 14.0f;
    }
    if (middle_pressed) moved = far;
    return moved;
}

}  // namespace tl::core
