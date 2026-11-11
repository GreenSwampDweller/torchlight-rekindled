// The desktop keyboard's keys the pad has none like: straight to the skills tab, and the
// hotbar's four skills by number. Read by drive_player.
#include "core/hotbar.h"
#include "core/input.h"
#include "render/renderer_impl.h"

namespace tl::render {

// DIVERGENCE (desktop keyboard): 'K' opens the master menu on its skills tab, and closes it
// when that tab is already showing; '1'..'4' are the hotbar's skill slots as they sit on the
// screen from left to right - Y, LT, RT, B. Not under the mouse controls, which have the PC's
// own keys for both.
void Renderer::Impl::step_keyboard_extras(const tl::core::InputState& in) {
    const bool keys = !tl::core::mouse_controls();
    for (size_t i = 0; i < hotbar_key_held.size(); ++i) {
        hotbar_key_down[i] = keys && in.hotbar_key[i];
        hotbar_key_edge[i] = hotbar_key_down[i] && !hotbar_key_held[i];
        hotbar_key_held[i] = in.hotbar_key[i];
    }

    const bool skills_edge = keys && in.skills && !skills_key_held;
    skills_key_held = in.skills;
    if (!skills_edge || inventory_menu.pet_commands_open() || inventory_menu.spell_remove_open()) {
        return;
    }
    if (inventory_menu.visible() && inventory_menu.menu_mode() == MenuMode::Skills) {
        close_master_menu();
        return;
    }
    if (!inventory_menu.visible()) {
        inventory_menu.toggle();
        play_ui(tl::core::InterfaceSound::MenuOpen);
    }
    inventory_menu.set_menu_mode(MenuMode::Skills);
    update_menu();
    update_character_preview(0.0f);
}

void Renderer::Impl::add_hotbar_keys(std::array<bool, 7>& pressed,
                                     std::array<bool, 7>& held) const {
    using tl::core::HotbarButton;
    constexpr HotbarButton kOrder[4] = {HotbarButton::Y, HotbarButton::LT, HotbarButton::RT,
                                        HotbarButton::B};
    for (size_t i = 0; i < hotbar_key_down.size(); ++i) {
        const auto button = static_cast<size_t>(kOrder[i]);
        if (hotbar_key_edge[i]) pressed[button] = true;
        if (hotbar_key_down[i] && !hotbar_key_edge[i]) held[button] = true;
    }
}

}  // namespace tl::render
