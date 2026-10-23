// The on-screen pad: an Xbox controller drawn on a touch screen. PORT: the port's own.
// It produces a controller's state (-1..1 axes, y down) that the platform folds into the
// physical pad's before the console's deadzones and filters. The left stick floats
// (centred where the thumb lands); a touch keeps its control until it lifts.
// No OGRE: the platform feeds touches in window pixels, the renderer draws `shapes()`.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace tl::core {

class TouchPad {
public:
    /// The controller's buttons, as bits; the Android pad's own, which asserts it.
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

    /// What the pad reads this frame.
    struct State {
        std::uint32_t held = 0;     ///< buttons down now
        std::uint32_t pressed = 0;  ///< pressed since the last take(), held or not
        float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;  ///< -1..1, y down
        float pinch_dolly = 0.0f;   ///< signed dolly delta accumulated since the last take()
        bool layout_changed = false;
        std::array<float, 32> layout_offsets{}; ///< normalized per-control x/y adjustments
        bool tap = false;
        float tap_x = 0.0f, tap_y = 0.0f;
    };

    /// One thing to draw: a disc at a centre, in window pixels.
    struct Shape {
        enum Kind : std::uint8_t { kButton, kStickBase, kStickKnob, kDpadBase };
        Kind kind = kButton;
        float x = 0.0f, y = 0.0f, radius = 0.0f;
        const char* label = "";  ///< empty for none
        bool pressed = false;
        /// The face buttons' own colours: 0 none, 1 A green, 2 B red, 3 X
        /// blue, 4 Y yellow.
        int tint = 0;
    };

    /// The window's size in pixels, the space the touches arrive in. Lays the
    /// controls out again when it changes; touches already down let go.
    void set_screen(float width, float height);
    float width() const { return width_; }
    float height() const { return height_; }
    /// Scale touch targets and their drawn discs; alpha scales their visibility.
    void set_visual_options(float scale, float opacity, int style = 0, int layout = 0);
    float opacity() const { return opacity_; }
    int style() const { return style_; }
    int layout_mode() const { return layout_mode_; }
    void set_edit_mode(bool enabled) { edit_mode_ = enabled; }
    bool edit_mode() const { return edit_mode_; }
    void set_layout_offsets(const std::array<float, 32>& offsets);

    void touch_down(int id, float x, float y);
    void touch_move(int id, float x, float y);
    void touch_up(int id);
    /// Every touch lifted: the window lost the events.
    void release_all();

    /// Whether the pad is on the screen. Off at first; a touch puts it there
    /// (and presses nothing), the platform takes it off when a physical
    /// controller is used.
    bool visible() const { return visible_; }
    void set_visible(bool visible) { visible_ = visible; }

    /// The pad's state, and the presses latched since the last call forgotten.
    State take();
    /// The same without forgetting anything, for a look.
    const State& peek() const { return state_; }

    /// What to draw, in a fixed number and order for a given screen size.
    std::vector<Shape> shapes() const;

private:
    enum class Kind : std::uint8_t { kButton, kDpad, kLeftStick, kRightStick };
    struct Control {
        Kind kind = Kind::kButton;
        std::uint32_t bit = 0;
        float x = 0.0f, y = 0.0f;  ///< centre (a stick's resting place)
        float radius = 0.0f;       ///< drawn
        float reach = 0.0f;        ///< hit
        const char* label = "";
        int tint = 0;
    };
    struct Touch {
        int id = -1;  ///< -1: free
        int control = -1;
        float base_x = 0.0f, base_y = 0.0f;  ///< a stick's centre while held
        float x = 0.0f, y = 0.0f;
        float start_x = 0.0f, start_y = 0.0f;
        bool editing = false;
    };

    void layout();
    int control_at(float x, float y) const;
    void update();

    float width_ = 0.0f, height_ = 0.0f;
    float stick_radius_ = 0.0f, knob_radius_ = 0.0f;
    float right_stick_radius_ = 0.0f, right_knob_radius_ = 0.0f;
    float dpad_arm_ = 0.0f, dpad_button_ = 0.0f;
    float pinch_distance_ = 0.0f;
    float pinch_dolly_ = 0.0f;
    float control_scale_ = 1.0f;
    float opacity_ = 1.0f;
    int style_ = 0;
    int layout_mode_ = 0;
    bool edit_mode_ = false;
    bool layout_change_pending_ = false;
    std::array<float, 32> layout_offsets_{};
    std::vector<Control> controls_;
    std::array<Touch, 10> touches_{};
    State state_;
    bool visible_ = false;
};

/// The one on-screen pad of the run.
TouchPad& touch_pad();

}  // namespace tl::core
