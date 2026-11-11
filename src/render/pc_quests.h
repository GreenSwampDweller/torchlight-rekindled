// The PC's quest log (`media/UI/questmenu.layout`), the right pane: up to six quests by
// name, the chosen one's journal text and rewards, and `Abandon`.
#pragma once

#include "render/layout_screen.h"
#include "render/pc_pane.h"

#include <string>
#include <vector>

namespace tl::render {

class PcQuests {
public:
    /// How many quests the pane lists (`Quest1` .. `Quest6`).
    static constexpr int kRows = 6;

    /// What the pane shows.
    struct View {
        std::vector<std::string> names;  ///< at most `kRows`
        int selected = -1;
        std::string details;
        int experience = 0;
        int gold = 0;
        int fame = 0;
        std::string reward_icon;
        bool can_abandon = false;
    };

    /// Build it against the mounted data. False when `questmenu.layout` is not there.
    bool create(const std::string& group, unsigned width, unsigned height);
    void destroy();
    bool ready() const { return screen_.ready(); }
    bool visible() const { return screen_.visible(); }
    void set_visible(bool on);
    void resize(unsigned width, unsigned height);

    void set(const View& view);

    /// The window under the pointer; empty over none.
    std::string point(float x, float y) const { return screen_.window_at(x, y); }
    /// The pane's `Blocker`: the pointer inside it is the pane's.
    bool blocks(float x, float y) const;

private:
    PcPane pane_;
    LayoutScreen screen_;
};

}  // namespace tl::render
