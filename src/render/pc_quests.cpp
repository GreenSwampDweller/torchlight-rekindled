#include "render/pc_quests.h"

#include <cstdio>

namespace tl::render {

namespace {

constexpr const char* kLayout = "media/UI/questmenu.layout";

std::string numbered(const char* stem, int number) {
    char name[32];
    std::snprintf(name, sizeof name, "%s%d", stem, number);
    return name;
}

}  // namespace

bool PcQuests::create(const std::string& group, unsigned width, unsigned height) {
    pane_.create(group, "quest", /*right=*/true, width, height, "tl/pcquests", 545);
    // The PC's sheets, and the item icons a reward is drawn from.
    std::vector<std::string> sets = {"media/UI/hud.imageset", "media/UI/ui2.imageset",
                                     "media/UI/uiicons.imageset",
                                     "media/UI/itemicons/itemicons.imageset"};
    for (int sheet = 1; sheet <= 16; ++sheet) {
        sets.push_back("media/UI/itemicons/itemicons" + std::to_string(sheet) + ".imageset");
    }
    screen_.use_imagesets(std::move(sets));
    if (!screen_.create_pc(group, width, height, kLayout, "tl/pcquests", 546)) {
        pane_.destroy();
        return false;
    }
    // One item is carried as a quest's reward: the first slot's.
    for (const char* spare :
         {"ItemRewardBkg2", "ItemRewardSlot2", "ItemRewardBkg3", "ItemRewardSlot3"}) {
        screen_.set_hidden(spare, true);
    }
    screen_.refresh();
    return true;
}

void PcQuests::destroy() {
    screen_.destroy();
    pane_.destroy();
}

void PcQuests::set_visible(bool on) {
    if (!screen_.ready() || on == screen_.visible()) return;
    if (on) {
        pane_.show();
        screen_.show();
    } else {
        screen_.hide();
        pane_.hide();
    }
}

void PcQuests::resize(unsigned width, unsigned height) {
    pane_.resize(width, height);
    screen_.resize(width, height);
}

// The log's windows, by their names in the layout.
void PcQuests::set(const View& view) {
    if (!screen_.ready()) return;
    for (int row = 0; row < kRows; ++row) {
        const bool used = static_cast<size_t>(row) < view.names.size();
        const std::string button = numbered("Quest", row + 1);
        screen_.set_hidden(button, !used);
        screen_.set_caption(numbered("QuestText", row + 1),
                            used ? view.names[static_cast<size_t>(row)] : std::string());
        // The chosen quest is named in the game's highlight colour.
        screen_.set_text_colour(
            numbered("QuestText", row + 1),
            row == view.selected ? Ogre::ColourValue(1.0f, 0.73f, 0.0f) : Ogre::ColourValue::White);
    }
    const bool chosen =
        view.selected >= 0 && static_cast<size_t>(view.selected) < view.names.size();
    screen_.set_caption("DescriptionFrame", chosen ? view.details : std::string());
    screen_.set_caption("XPReward", chosen ? std::to_string(view.experience) : std::string());
    screen_.set_caption("GoldReward", chosen ? std::to_string(view.gold) : std::string());
    screen_.set_caption("FameReward", chosen ? std::to_string(view.fame) : std::string());
    const bool item = chosen && !view.reward_icon.empty();
    screen_.set_hidden("ItemRewardBkg1", !item);
    screen_.set_image("ItemRewardSlot1", item ? view.reward_icon : std::string());
    screen_.set_hidden("Abandon", !chosen || !view.can_abandon);
    screen_.refresh();
}

bool PcQuests::blocks(float x, float y) const {
    return screen_.visible() && screen_.window_contains("Blocker", x, y);
}

}  // namespace tl::render
