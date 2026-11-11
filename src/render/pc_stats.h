// The PC's character sheet (`media/UI/statsmenu.layout`), the left pane: the four
// attributes with their plus and minus buttons, the damage and resistance figures, and
// the character's name, level and pools. Its contents are the console's stats page
// (`core/stats_page.h`), which holds the same figures and the same pending points.
#pragma once

#include "core/stats_page.h"
#include "render/layout_screen.h"
#include "render/pc_pane.h"

#include <string>

namespace tl::render {

class PcStats {
public:
    /// What the pane shows that the stats page does not carry.
    struct Figures {
        std::string name;
        int level = 0;
        std::string experience;
        float experience_fill = 0.0f;
        std::string health;
        std::string mana;
    };

    /// Build it against the mounted data. False when `statsmenu.layout` is not there.
    bool create(const std::string& group, unsigned width, unsigned height);
    void destroy();
    bool ready() const { return screen_.ready(); }
    bool visible() const { return screen_.visible(); }
    void set_visible(bool on);
    void resize(unsigned width, unsigned height);

    void set(const tl::core::StatsPageView& view, const Figures& figures);

    /// The window under the pointer, lit when it has lit art; empty over none.
    std::string point(float x, float y);
    /// The pane's `Blocker`: the pointer inside it is the pane's.
    bool blocks(float x, float y) const;

private:
    PcPane pane_;
    LayoutScreen screen_;
    std::string lit_;
    bool have_ = false;
    tl::core::StatsPageView view_;
    Figures figures_;
};

}  // namespace tl::render
