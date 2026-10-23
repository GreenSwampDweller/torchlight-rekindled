// The on-screen pad (touch_pad.h). PORT: the port's own throughout.
#include "core/touch_pad.h"

#include <algorithm>
#include <cmath>

namespace tl::core {

namespace {

/// How far a D-pad touch must be from its centre, as a fraction of an arm's
/// offset, before it presses a direction; and the share of its distance a
/// direction must have to count, which gives eight sectors of 45 degrees
/// (sin 22.5 = 0.38).
constexpr float kDpadDead = 0.3f;
constexpr float kDpadSector = 0.38f;

/// The part of the screen the floating left stick can come down in: the
/// left of the screen, below the shoulder buttons.
constexpr float kLeftZoneWidth = 0.45f;  ///< of the width
constexpr float kLeftZoneTop = 0.20f;    ///< of the shorter side

void clamp_unit(float& x, float& y) {
    const float m = std::sqrt(x * x + y * y);
    if (m > 1.0f) {
        x /= m;
        y /= m;
    }
}

}  // namespace

TouchPad& touch_pad() {
    static TouchPad pad;
    return pad;
}

void TouchPad::set_screen(float width, float height) {
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    for (Touch& t : touches_) t = Touch{};
    layout();
    update();
}

void TouchPad::set_visual_options(float scale, float opacity, int style, int layout_mode) {
    scale = std::clamp(scale, 0.8f, 1.25f);
    opacity = std::clamp(opacity, 0.5f, 1.4f);
    style = std::clamp(style, 0, 2);
    layout_mode = std::clamp(layout_mode, 0, 2);
    if (scale == control_scale_ && opacity == opacity_ && style == style_ &&
        layout_mode == layout_mode_) return;
    const bool resized = scale != control_scale_ || layout_mode != layout_mode_;
    control_scale_ = scale;
    opacity_ = opacity;
    style_ = style;
    layout_mode_ = layout_mode;
    if (resized && width_ > 0.0f && height_ > 0.0f) layout();
    update();
}

void TouchPad::set_layout_offsets(const std::array<float, 32>& offsets) {
    auto clamped = offsets;
    for (float& value : clamped) value = std::clamp(value, -0.45f, 0.45f);
    if (clamped == layout_offsets_) return;
    layout_offsets_ = clamped;
    if (width_ > 0.0f && height_ > 0.0f) layout();
}

// Sizes in units of the screen's shorter side, so the pad is the same size
// under the thumbs on a foldable phone's narrow cover screen and its square
// inner one.
void TouchPad::layout() {
    controls_.clear();
    const float u = std::min(width_, height_);
    const float w = width_;
    const float h = height_;
    stick_radius_ = 0.13f * u * control_scale_;
    knob_radius_ = 0.055f * u * control_scale_;
    right_stick_radius_ = 0.10f * u * control_scale_;
    right_knob_radius_ = 0.045f * u * control_scale_;
    dpad_arm_ = 0.085f * u * control_scale_;
    dpad_button_ = 0.05f * u * control_scale_;

    const auto add = [this](Kind kind, std::uint32_t bit, float x, float y, float radius,
                            float reach, const char* label, int tint) {
        controls_.push_back(Control{kind, bit, x, y, radius, reach, label, tint});
    };
    const bool right_hand = layout_mode_ == 1;
    const bool left_hand = layout_mode_ == 2;
    const float left_stick_x = right_hand ? w - 0.25f * u : left_hand ? 0.25f * u : 0.24f * u;
    add(Kind::kLeftStick, 0, left_stick_x, h - 0.25f * u, stick_radius_, 0.0f, "", 0);
    add(Kind::kRightStick, 0, w - 0.60f * u, h - 0.20f * u, right_stick_radius_, 0.16f * u, "", 0);
    add(Kind::kButton, kThumbR, w - 0.60f * u, h - 0.38f * u,
        0.04f * u * control_scale_, 0.06f * u * control_scale_, "R3", 0);
    // L3 (the frame-rate overlay): right of the left stick, clear of the D-pad above it.
    add(Kind::kButton, kThumbL, 0.44f * u, h - 0.40f * u,
        0.04f * u * control_scale_, 0.06f * u * control_scale_, "L3", 0);
    const float dpad_x = right_hand ? w - 0.25f * u : left_hand ? 0.25f * u : 0.20f * u;
    add(Kind::kDpad, 0, dpad_x, h - 0.62f * u, dpad_arm_ + dpad_button_,
        0.17f * u * control_scale_, "", 0);

    const float face_x = left_hand ? 0.23f * u : w - 0.23f * u;
    // Above the HUD's minimap, which sits in the bottom right corner.
    const float face_y = h - 0.40f * u;
    const float arm = 0.11f * u * control_scale_;
    const float face = 0.06f * u * control_scale_;
    const float face_reach = 0.075f * u * control_scale_;
    add(Kind::kButton, kA, face_x, face_y + arm, face, face_reach, "A", 1);
    add(Kind::kButton, kB, face_x + arm, face_y, face, face_reach, "B", 2);
    add(Kind::kButton, kX, face_x - arm, face_y, face, face_reach, "X", 3);
    add(Kind::kButton, kY, face_x, face_y - arm, face, face_reach, "Y", 4);
    // A separate, larger Use target near the action cluster. It shares A's
    // game action so it works for interaction, loot and menu confirmation.
    add(Kind::kButton, kA, face_x, h - 0.13f * u, 0.055f * u * control_scale_,
        0.075f * u * control_scale_, "Use", 1);

    const float shoulder = 0.06f * u * control_scale_;
    const float shoulder_reach = 0.08f * u * control_scale_;
    // Inside the HUD's health and mana orbs, which fill the top corners.
    add(Kind::kButton, kL2, 0.31f * u, 0.09f * u, shoulder, shoulder_reach, "LT", 0);
    add(Kind::kButton, kL1, 0.46f * u, 0.09f * u, shoulder, shoulder_reach, "LB", 0);
    add(Kind::kButton, kR1, w - 0.46f * u, 0.09f * u, shoulder, shoulder_reach, "RB", 0);
    add(Kind::kButton, kR2, w - 0.31f * u, 0.09f * u, shoulder, shoulder_reach, "RT", 0);

    // Between the shoulders where the screen is wide enough, under them where
    // it is nearly square (a foldable phone's inner screen).
    const float menu_y = w < 1.4f * u ? 0.24f * u : 0.07f * u;
    add(Kind::kButton, kSelect, 0.5f * w - 0.09f * u, menu_y, 0.045f * u * control_scale_,
        0.06f * u * control_scale_, "Back", 0);
    add(Kind::kButton, kStart, 0.5f * w + 0.09f * u, menu_y, 0.045f * u * control_scale_,
        0.06f * u * control_scale_, "Start", 0);

    for (std::size_t i = 0; i < controls_.size() && 2 * i + 1 < layout_offsets_.size(); ++i) {
        controls_[i].x += layout_offsets_[2 * i] * u;
        controls_[i].y += layout_offsets_[2 * i + 1] * u;
    }
}

// The nearest control whose reach the touch is in; then the left stick's
// zone; then nothing.
int TouchPad::control_at(float x, float y) const {
    int best = -1;
    float best_d = 0.0f;
    for (int i = 0; i < static_cast<int>(controls_.size()); ++i) {
        const Control& c = controls_[static_cast<std::size_t>(i)];
        if (!edit_mode_ && c.kind == Kind::kLeftStick) continue;
        const float d = std::hypot(x - c.x, y - c.y);
        const float hit_radius = edit_mode_ ? std::max(c.reach, c.radius * 1.4f) : c.reach;
        if (d <= hit_radius && (best < 0 || d < best_d)) {
            best = i;
            best_d = d;
        }
    }
    if (best >= 0) return best;
    const float u = std::min(width_, height_);
    const bool in_stick_zone = layout_mode_ == 1 ? x > (1.0f - kLeftZoneWidth) * width_
                              : layout_mode_ == 2 ? x < kLeftZoneWidth * width_
                                                  : x < kLeftZoneWidth * width_;
    if (in_stick_zone && y > kLeftZoneTop * u) {
        for (int i = 0; i < static_cast<int>(controls_.size()); ++i) {
            if (controls_[static_cast<std::size_t>(i)].kind == Kind::kLeftStick) return i;
        }
    }
    return -1;
}

void TouchPad::touch_down(int id, float x, float y) {
    // The touch that brings the pad onto the screen does only that: it
    // presses nothing, whatever it lands on, until it lifts.
    const bool revealing = !visible_;
    visible_ = true;
    Touch* slot = nullptr;
    for (Touch& t : touches_) {
        if (t.id == id) slot = &t;
    }
    for (Touch& t : touches_) {
        if (slot == nullptr && t.id < 0) slot = &t;
    }
    if (slot == nullptr) return;
    *slot = Touch{};
    slot->id = id;
    slot->x = x;
    slot->y = y;
    slot->start_x = x;
    slot->start_y = y;
    slot->control = revealing && !edit_mode_ ? -1 : control_at(x, y);
    // A second thumb on the left stick's zone while one already steers it
    // is ignored rather than allowed to take it over.
    if (slot->control >= 0) {
        for (const Touch& t : touches_) {
            if (&t != slot && t.id >= 0 && t.control == slot->control &&
                controls_[static_cast<std::size_t>(t.control)].kind == Kind::kLeftStick) {
                slot->control = -1;
            }
        }
    }
    if (slot->control >= 0) {
        const Control& c = controls_[static_cast<std::size_t>(slot->control)];
        slot->editing = edit_mode_;
        slot->base_x = c.kind == Kind::kLeftStick ? x : c.x;
        slot->base_y = c.kind == Kind::kLeftStick ? y : c.y;
    }
    update();
}

void TouchPad::touch_move(int id, float x, float y) {
    for (Touch& t : touches_) {
        if (t.id == id) {
            if (t.editing && t.control >= 0) {
                const float u = std::max(1.0f, std::min(width_, height_));
                Control& c = controls_[static_cast<std::size_t>(t.control)];
                const float dx = x - t.x;
                const float dy = y - t.y;
                const float margin = 0.08f * u;
                const float next_x = std::clamp(c.x + dx, margin, width_ - margin);
                const float next_y = std::clamp(c.y + dy, margin, height_ - margin);
                const float moved_x = next_x - c.x;
                const float moved_y = next_y - c.y;
                c.x = next_x;
                c.y = next_y;
                const std::size_t index = static_cast<std::size_t>(t.control) * 2;
                if (index + 1 < layout_offsets_.size()) {
                    layout_offsets_[index] = std::clamp(layout_offsets_[index] + moved_x / u,
                                                        -0.45f, 0.45f);
                    layout_offsets_[index + 1] = std::clamp(layout_offsets_[index + 1] + moved_y / u,
                                                            -0.45f, 0.45f);
                }
                t.base_x = c.x;
                t.base_y = c.y;
                layout_change_pending_ = layout_change_pending_ || moved_x != 0.0f || moved_y != 0.0f;
            }
            t.x = x;
            t.y = y;
        }
    }
    update();
}

void TouchPad::touch_up(int id) {
    for (Touch& t : touches_) {
        if (t.id == id) {
            bool other_unassigned = false;
            for (const Touch& other : touches_) {
                if (&other != &t && other.id >= 0 && other.control < 0) {
                    other_unassigned = true;
                    break;
                }
            }
            const float u = std::max(1.0f, std::min(width_, height_));
            if (!t.editing && t.control < 0 && !other_unassigned &&
                std::hypot(t.x - t.start_x, t.y - t.start_y) < 0.035f * u) {
                state_.tap = true;
                state_.tap_x = t.x;
                state_.tap_y = t.y;
            }
            if (t.editing && layout_change_pending_) state_.layout_changed = true;
            t = Touch{};
        }
    }
    update();
}

void TouchPad::release_all() {
    for (Touch& t : touches_) {
        if (t.editing && layout_change_pending_) state_.layout_changed = true;
        t = Touch{};
    }
    update();
}

void TouchPad::update() {
    const std::uint32_t before = state_.held;
    state_.held = 0;
    state_.lx = state_.ly = state_.rx = state_.ry = 0.0f;
    for (const Touch& t : touches_) {
        if (t.id < 0 || t.control < 0) continue;
        if (t.editing) continue;
        const Control& c = controls_[static_cast<std::size_t>(t.control)];
        switch (c.kind) {
            case Kind::kButton: state_.held |= c.bit; break;
            case Kind::kDpad: {
                const float vx = t.x - c.x;
                const float vy = t.y - c.y;
                const float m = std::hypot(vx, vy);
                if (m < kDpadDead * dpad_arm_) break;
                if (vx > kDpadSector * m) state_.held |= kRight;
                if (vx < -kDpadSector * m) state_.held |= kLeft;
                if (vy > kDpadSector * m) state_.held |= kDown;
                if (vy < -kDpadSector * m) state_.held |= kUp;
                break;
            }
            case Kind::kLeftStick:
                state_.lx = (t.x - t.base_x) / stick_radius_;
                state_.ly = (t.y - t.base_y) / stick_radius_;
                clamp_unit(state_.lx, state_.ly);
                break;
            case Kind::kRightStick:
                state_.rx = (t.x - t.base_x) / right_stick_radius_;
                state_.ry = (t.y - t.base_y) / right_stick_radius_;
                clamp_unit(state_.rx, state_.ry);
                break;
        }
    }
    const Touch* pinch_a = nullptr;
    const Touch* pinch_b = nullptr;
    for (const Touch& t : touches_) {
        if (t.id < 0 || t.control >= 0) continue;
        if (!pinch_a) {
            pinch_a = &t;
        } else {
            pinch_b = &t;
            break;
        }
    }
    if (pinch_a && pinch_b) {
        const float distance = std::hypot(pinch_a->x - pinch_b->x, pinch_a->y - pinch_b->y);
        if (pinch_distance_ > 0.0f) {
            // Spreading fingers zooms in. Express the gesture as dolly units so it is
            // independent of frame rate and behaves consistently across screen sizes.
            const float short_side = std::max(1.0f, std::min(width_, height_));
            pinch_dolly_ -= (distance - pinch_distance_) * (60.0f / short_side);
        }
        pinch_distance_ = distance;
    } else {
        pinch_distance_ = 0.0f;
    }
    state_.pressed |= state_.held & ~before;
    state_.pinch_dolly = pinch_dolly_;
}

TouchPad::State TouchPad::take() {
    State now = state_;
    now.layout_changed = state_.layout_changed;
    now.layout_offsets = layout_offsets_;
    now.tap = state_.tap;
    now.tap_x = state_.tap_x;
    now.tap_y = state_.tap_y;
    state_.pressed = 0;
    state_.layout_changed = false;
    state_.tap = false;
    layout_change_pending_ = false;
    pinch_dolly_ = 0.0f;
    state_.pinch_dolly = 0.0f;
    return now;
}

std::vector<TouchPad::Shape> TouchPad::shapes() const {
    std::vector<Shape> out;
    for (int i = 0; i < static_cast<int>(controls_.size()); ++i) {
        const Control& c = controls_[static_cast<std::size_t>(i)];
        const Touch* held = nullptr;
        for (const Touch& t : touches_) {
            if (t.id >= 0 && t.control == i) held = &t;
        }
        switch (c.kind) {
            case Kind::kButton:
                out.push_back(Shape{Shape::kButton, c.x, c.y, c.radius, c.label,
                                    (state_.held & c.bit) != 0 || (edit_mode_ && held != nullptr),
                                    c.tint});
                break;
            case Kind::kDpad: {
                out.push_back(Shape{Shape::kDpadBase, c.x, c.y, c.radius, "",
                                    edit_mode_ && held != nullptr, 0});
                const struct {
                    std::uint32_t bit;
                    float dx, dy;
                } arms[] = {{kUp, 0, -1}, {kDown, 0, 1}, {kLeft, -1, 0}, {kRight, 1, 0}};
                for (const auto& arm : arms) {
                    out.push_back(Shape{Shape::kButton, c.x + arm.dx * dpad_arm_,
                                        c.y + arm.dy * dpad_arm_, dpad_button_, "",
                                        (state_.held & arm.bit) != 0, 0});
                }
                break;
            }
            case Kind::kLeftStick:
            case Kind::kRightStick: {
                const bool left = c.kind == Kind::kLeftStick;
                const float bx = held != nullptr ? held->base_x : c.x;
                const float by = held != nullptr ? held->base_y : c.y;
                const float r = left ? stick_radius_ : right_stick_radius_;
                const float ax = left ? state_.lx : state_.rx;
                const float ay = left ? state_.ly : state_.ry;
                out.push_back(Shape{Shape::kStickBase, bx, by, r, "", held != nullptr, 0});
                out.push_back(Shape{Shape::kStickKnob, bx + ax * r, by + ay * r,
                                    left ? knob_radius_ : right_knob_radius_, "", held != nullptr,
                                    0});
                break;
            }
        }
    }
    return out;
}

}  // namespace tl::core
