// The PC's game screen (`media/UI/bottomhud.layout`): the bar along the bottom with the two
// orbs, the experience bar, ten numbered slots, the menu buttons, and the target's panel
// at the top; and the pet's panel (`media/UI/pethud.layout`), top left, with its two bars
// and three stances. Shown in place of the console's HUD under the mouse controls when a PC pak
// is mounted. It is fed by the same `HudState` as the console's.
#pragma once

#include "render/game_hud.h"
#include "render/layout_screen.h"

#include <string>

namespace tl::render {

class PcHud {
public:
    /// Build it against the mounted data. False when `bottomhud.layout` is not there.
    bool create(const std::string& group, unsigned width, unsigned height);
    void destroy() {
        screen_.destroy();
        pet_.destroy();
    }
    bool ready() const { return screen_.ready(); }
    bool visible() const { return screen_.visible(); }
    void set_visible(bool on);
    void resize(unsigned width, unsigned height) {
        screen_.resize(width, height);
        pet_.resize(width, height);
    }

    void set_state(const HudState& state);
    /// What `HudState` does not carry: whether stat and skill points wait to be spent
    /// (`StatsUp` and `SkillUp`, the arrows at the screen's two edges), and the picture of
    /// the right button's skill (`PlayerSkillRight`). Shown from the next `set_state`.
    void set_extras(bool stat_points, bool skill_points, const std::string& right_skill_icon);

    /// The pointer at (x, y), 0 to 1 across and down the view: the window under it is
    /// lit, and its name returned; empty over the game.
    std::string point(float x, float y);
    /// The HUD's windows, for the art of its imagesets.
    const LayoutScreen& screen() const { return screen_; }
    /// `CGameUI::mouseOverPanel`'s three standing rectangles: the layout's
    /// `BottomHudBlocker`, `BottomHudBlocker2` and, with the pet's panel up, `PetHudBlocker`.
    /// Inside one the pointer is the interface's, whatever window it is over.
    bool blocks(float x, float y) const;
    /// A window's `onClick`, the function a click on it asks for.
    std::string on_click(const std::string& window) const {
        const std::string pets = pet_.on_click(window);
        return pets.empty() ? screen_.on_click(window) : pets;
    }

private:
    LayoutScreen screen_;
    LayoutScreen pet_;
    std::string lit_;
    HudState shown_;
    bool have_state_ = false;
    bool stat_points_ = false;
    bool skill_points_ = false;
    std::string right_skill_icon_;
};

}  // namespace tl::render
