// The PC's pointer picks (CLevel::findCharacterAtScreenCoordinates,
// findItemAtScreenCoordinates) and the plane half of findWorldLocation.
#include "core/mouse_pick.h"
#include "test_support.h"

#include <cmath>

using tl::core::character_screen_box;
using tl::core::distance_to_plane;
using tl::core::item_screen_box;
using tl::core::pick_at;
using tl::core::pick_on_screen;
using tl::core::PickCandidate;
using tl::core::pointer_on_plane;
using tl::core::screen_matrix;
using tl::core::ScreenBox;
using tl::core::ScreenMatrix;
using tl::core::to_screen;
using tl::core::Vec3;

namespace {

/// A camera at the origin looking down -Z: the identity view and a simple
/// perspective (x and y divided by the distance in front).
ScreenMatrix simple_camera() {
    const float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    // near 1, far 100, as OpenGL writes it.
    const float n = 1.0f;
    const float f = 100.0f;
    const float projection[16] = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -(f + n) / (f - n), -2.0f * f * n / (f - n), 0, 0, -1, 0};
    return screen_matrix(projection, view);
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

}  // namespace

TL_TEST(mouse_pick_projects_through_both_matrices) {
    const ScreenMatrix m = simple_camera();
    const Vec3 at = to_screen(m, Vec3{2.0f, 1.0f, -10.0f});
    TL_CHECK(near(at.x, 0.2f));
    TL_CHECK(near(at.y, 0.1f));
    TL_CHECK(at.z > 0.0f && at.z < 1.0f);
    TL_CHECK(pick_on_screen(m, Vec3{2.0f, 1.0f, -10.0f}));
    TL_CHECK(!pick_on_screen(m, Vec3{20.0f, 1.0f, -10.0f}));
}

// The character's box: the low side no nearer than -0.55, the high side a fixed
// 0.55, the top 0.9 of the model's height.
TL_TEST(mouse_pick_character_box_has_a_floor_of_055) {
    const ScreenMatrix m = simple_camera();
    const Vec3 at{0.0f, 0.0f, -10.0f};
    // A thin model: the floor applies on the low side.
    const ScreenBox thin =
        character_screen_box(m, at, Vec3{-0.1f, 0.0f, -0.1f}, Vec3{0.1f, 2.0f, 0.1f});
    TL_CHECK(thin.low.x < -0.05f);
    TL_CHECK(thin.high.x > 0.05f);
    TL_CHECK(thin.high.y > 0.17f && thin.high.y < 0.2f);
    // A wide one: the low side grows with it, the high side does not.
    const ScreenBox wide =
        character_screen_box(m, at, Vec3{-4.0f, 0.0f, -0.1f}, Vec3{4.0f, 2.0f, 0.1f});
    TL_CHECK(wide.low.x < thin.low.x);
    TL_CHECK(near(wide.high.x, thin.high.x));
}

TL_TEST(mouse_pick_item_box_scales_both_bounds) {
    const ScreenMatrix m = simple_camera();
    const ScreenBox box = item_screen_box(m, Vec3{0.0f, 0.0f, -10.0f}, Vec3{-1.0f, 0.0f, -1.0f},
                                          Vec3{1.0f, 1.0f, 1.0f});
    TL_CHECK(box.low.x < -0.06f && box.low.x > -0.08f);
    TL_CHECK(box.high.x > 0.06f && box.high.x < 0.08f);
    TL_CHECK(box.high.y > 0.09f);
}

// Under the pointer, the nearest wins; an enemy beats a good or a neutral
// character even when it stands behind them.
TL_TEST(mouse_pick_prefers_enemies_then_the_nearest) {
    const ScreenMatrix m = simple_camera();
    const Vec3 lo{-0.5f, 0.0f, -0.5f};
    const Vec3 hi{0.5f, 2.0f, 0.5f};
    PickCandidate near_good;
    near_good.box = character_screen_box(m, Vec3{0.0f, 0.0f, -5.0f}, lo, hi);
    near_good.alignment = 1;
    near_good.id = 1;
    PickCandidate far_evil;
    far_evil.box = character_screen_box(m, Vec3{0.0f, 0.0f, -20.0f}, lo, hi);
    far_evil.alignment = 2;
    far_evil.id = 2;
    PickCandidate nearer_evil;
    nearer_evil.box = character_screen_box(m, Vec3{0.0f, 0.0f, -10.0f}, lo, hi);
    nearer_evil.alignment = 2;
    nearer_evil.id = 3;

    std::uint64_t id = 0;
    const PickCandidate two[] = {near_good, far_evil};
    TL_CHECK(pick_at(two, 2, 0.0f, 0.02f, &id));
    TL_CHECK_EQ(id, std::uint64_t{2});
    const PickCandidate three[] = {near_good, far_evil, nearer_evil};
    TL_CHECK(pick_at(three, 3, 0.0f, 0.02f, &id));
    TL_CHECK_EQ(id, std::uint64_t{3});
    // Off to the side of all of them: nothing.
    TL_CHECK(!pick_at(three, 3, 0.9f, 0.9f, &id));
}

TL_TEST(mouse_pick_distance_to_plane) {
    const Vec3 up{0.0f, 1.0f, 0.0f};
    TL_CHECK(near(distance_to_plane(Vec3{0, 10, 0}, Vec3{0, -1, 0}, up, 2.0f), 8.0f));
    // Along the plane: the original's -1.
    TL_CHECK(near(distance_to_plane(Vec3{0, 10, 0}, Vec3{1, 0, 0}, up, 2.0f), -1.0f));
}

TL_TEST(mouse_pick_pointer_on_plane) {
    // Looking down and forward from (0, 10, 0): the plane at y 0 is met 10 along.
    const float s = std::sqrt(0.5f);
    const Vec3 at = pointer_on_plane(Vec3{0.0f, 10.0f, 0.0f}, Vec3{0.0f, -s, s}, 0.0f);
    TL_CHECK(near(at.x, 0.0f));
    TL_CHECK(near(at.y, 0.0f));
    TL_CHECK(near(at.z, 10.0f));
    // The raised plane is met sooner.
    const Vec3 raised =
        pointer_on_plane(Vec3{0.0f, 10.0f, 0.0f}, Vec3{0.0f, -s, s}, tl::core::kPointerRaise);
    TL_CHECK(near(raised.z, 10.0f - tl::core::kPointerRaise));
}

// The wheel: 0.01 a unit towards the player, held between 14 and the far distance; the
// middle button resets to the far distance.
TL_TEST(mouse_wheel_dolly_moves_clamps_and_resets) {
    using tl::core::wheel_dolly;
    TL_CHECK(near(wheel_dolly(28.5f, 120, false, 1.0f), 27.3f));
    TL_CHECK(near(wheel_dolly(15.0f, 240, false, 1.0f), 14.0f));
    TL_CHECK(near(wheel_dolly(28.0f, -240, false, 1.0f), 28.5f));
    TL_CHECK(near(wheel_dolly(20.0f, 0, true, 1.0f), 28.5f));
    TL_CHECK(near(wheel_dolly(20.0f, 0, true, 1.2f), 34.2f));
    TL_CHECK(near(wheel_dolly(20.0f, 0, false, 1.0f), 20.0f));
}
