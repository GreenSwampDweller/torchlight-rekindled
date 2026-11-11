// The interface's screens, made once when the renderer starts. Split from renderer.cpp.
#include "render/renderer_impl.h"

namespace tl::render {

void Renderer::Impl::create_interface() {
#if defined(TL_PLATFORM_IOS)
    // DIVERGENCE (platform): the touch screen's text size setting, before any text is made.
    if (config.settings != nullptr)
        set_ui_text_scale(ui_text_size_scale(config.settings->text_size));
#endif
    // The interface. Not fatal when it cannot be built: the game ran without
    // one before there was one, and it says so in the log rather than refusing
    // to start.
    hud.create(scene, config.width, config.height,
               // By full archive path: `itemicons.imageset` is one of
               // seventeen files under `media/UI/itemicons/` whose names all
               // begin that way, and the index is by basename.
               "media/UI/itemicons/itemicons.imageset");

    set_stats_overlay(config.stats_overlay);

    // The game screen's HUD. Also not fatal: it needs the console release's
    // interface data, which a PC install has not got, and it says so once.
    if (config.game_hud) game_hud.create(kGameGroup, config.width, config.height);

    // The quest offer window, from the same interface data. Not fatal either.
    quest_dialog.create(kGameGroup, config.width, config.height);
    death_screen.create(kGameGroup, config.width, config.height);
    // Below the quest window (590) and above the HUD: the ring is drawn over
    // the game, the modal over every menu but the death screen (600).
    fishing_screen.create(kGameGroup, config.width, config.height, "media/UI/fishing_xenon.layout",
                          "tl/fishing", 560);
    // The console loads the CEGUI export of the same screen (`fishing.uilayout`),
    // which lists `FishingRing` last, above the fixed ring
    // the `[UI]` source lists after it.
    fishing_screen.raise("FishingRing");
    modal_screen.create(kGameGroup, config.width, config.height, "media/UI/modalmenu_xenon.layout",
                        "tl/modal", 594);
    // The waypoint menu (`CWaypointMenu`, the Xbox's `waypointmenu_xenon.layout`),
    // above the HUD and below the quest window and the modal.
    waypoint_screen.create(kGameGroup, config.width, config.height,
                           "media/UI/waypointmenu_xenon.layout", "tl/waypoints", 570);

    // The Pause Menu and Options (`COptionsMenu`, `CSettingsMenuXenon`).
    create_options_screens();
    // The context tip window (`CTipMenu`).
    create_tip_screen();

    // The console inventory menu, from the same interface data.
    init_character_preview();
    inventory_menu.create(kGameGroup, config.width, config.height);
    if (config.show_inventory) {
        inventory_menu.show();
    }
}

// CGameUI::setInteractiveMenuVisible(bool): a count,
// held at 0. At 1 the bars open and the automap goes; back at 0 they close and it returns
// (the minimap's half is step_hud_visibility's).
void Renderer::Impl::set_interactive_menu_visible(bool on) {
    interactive_menus += on ? 1 : -1;
    if (interactive_menus < 0) {
        interactive_menus = 0;
    } else if (interactive_menus == 1) {
        if (!hud.letterboxed()) hud.set_letterboxed(true);
    } else if (interactive_menus == 0 && hud.letterboxed()) {
        hud.set_letterboxed(false);
    }
}

}  // namespace tl::render
