// The renderer's implementation: debug drivers, menus, dungeon transitions, level memory, saves.
// Split from renderer.cpp; the shared
// declarations are in renderer_impl.h.
#include "render/draw_dump.h"
#include "render/renderer_impl.h"

#include <OgreMaterialManager.h>
#include <OgrePixelFormat.h>

#include <chrono>

namespace tl::render {
bool Renderer::place_player_at(float x, float z, const float* feet_if_no_ground) {
    Impl& m = *impl_;
    if (!m.player_control) {
        TL_LOG_ERROR("place_player_at needs player control enabled");
        return false;
    }

    const tl::core::Vec3 was = m.player.position();

    float surface = 0.0f;
    // `CCharacter::dropToGround`'s ray runs +100 to -100 about the unit; casting
    // from 200 above left ground at or below the start height out of reach.
    if (!m.collision.empty() && !m.collision.ground_under(x, z, was.y + 100.0f, surface)) {
        if (feet_if_no_ground == nullptr) {
            TL_LOG_WARN("no ground at (%.2f, %.2f); leaving the character where it was", x, z);
            return false;
        }
        surface = *feet_if_no_ground;
        TL_LOG_INFO("no ground at (%.2f, %.2f); standing at the given height %.3f", x, z,
                    static_cast<double>(surface));
    }

    m.player.place({x, surface, z}, m.player.facing());
    m.step_player(0.0f, tl::core::InputState{});

    // The pet comes too: the original moves a character's pets whenever it moves the
    // character itself (CGameClient on every level change).
    m.place_pet_beside_player();
    // ... and so do the characters that took the player as their master
    // (`CCharacter +0x648`), beside them as `bring_player_pets` puts them.
    int n = 0;
    for (const Impl::MachineView& view : m.machines()) {
        for (const int64_t id : view.script->player_pets()) {
            if (view.script == &m.script && id == m.pet_node_id) continue;  // the animal, above
            ++n;
            Ogre::Vector3 at(x - 1.6f * static_cast<float>(n), surface, z);
            at = m.dropped_to_ground(at);
            view.script->move_unit(id, tl::core::Vec3{at.x, at.y, at.z});
        }
    }
    TL_LOG_INFO("character moved to (%.2f, %.2f, %.2f)", x, surface, z);
    return true;
}

void Renderer::probe_ground(float x, float z) {
    Impl& m = *impl_;
    float top = 100.0f;
    for (int i = 0; i < 16; ++i) {
        tl::core::CollisionHit hit;
        if (!m.collision.ray_collision({x, top, z}, {x, -100.0f, z}, true, hit)) break;
        TL_LOG_INFO(
            "ground probe (%.2f, %.2f): surface at y %.3f, material %d, group %lld, normal y %.2f",
            static_cast<double>(x), static_cast<double>(z), static_cast<double>(hit.point.y),
            static_cast<int>(hit.material), static_cast<long long>(hit.group),
            static_cast<double>(hit.normal.y));
        top = hit.point.y - 0.001f;
    }
}

// PORT: a headless verification switch (`--unit-heights`): where each unit stands, logged.
void Renderer::log_unit_heights() {
    Impl& m = *impl_;
    for (const auto& entry : m.scene_units) {
        if (entry.second.entity == nullptr) continue;
        const Ogre::SceneNode* node = entry.second.entity->getParentSceneNode();
        if (node == nullptr) continue;
        const Ogre::Vector3 at = node->_getDerivedPosition();
        float ground = 0.0f;
        const bool found = m.collision.ground_under(at.x, at.z, at.y + 4.0f, ground);
        // Where the machine that runs it has its feet, which is what a
        // remembered level stores.
        float script_y = 0.0f;
        bool in_script = false;
        for (const Impl::MachineView& view : m.machines()) {
            tl::core::Vec3 feet;
            if (view.script->unit_position(entry.second.node, feet)) {
                script_y = feet.y;
                in_script = true;
                break;
            }
        }
        TL_LOG_INFO(
            "unit height: %-24s at (%.2f, %.3f, %.2f), ground %s%.3f, %+.3f above, "
            "script %s%.3f",
            entry.first.c_str(), static_cast<double>(at.x), static_cast<double>(at.y),
            static_cast<double>(at.z), found ? "" : "none ", static_cast<double>(ground),
            static_cast<double>(found ? at.y - ground : 0.0f), in_script ? "" : "none ",
            static_cast<double>(script_y));
    }
}

bool Renderer::hold_stick(float move_x, float move_y, float seconds) {
    Impl& m = *impl_;
    if (!m.player_control) {
        TL_LOG_ERROR("hold_stick needs player control enabled");
        return false;
    }
    // The same fixed step as `simulate_walk`, for the same reason.
    constexpr float kStep = 1.0f / 60.0f;
    const int steps = static_cast<int>(seconds / kStep);
    const tl::core::Vec3 from = m.player.position();
    size_t blocked_steps = 0;
    for (int i = 0; i < steps; ++i) {
        tl::core::InputState in;
        in.move_x = move_x;
        in.move_y = move_y;
        m.step_player(kStep, in);
        if (m.player.blocked()) ++blocked_steps;
        const char* wanted = m.player.animation();
        if (m.player_animation != wanted) {
            apply_animation(wanted, 0.0f);
            m.player_animation = wanted;
        }
        m.advance(kStep);
    }
    const tl::core::Vec3 at = m.player.position();
    TL_LOG_INFO(
        "stick (%.2f, %.2f) held %.2fs: (%.2f, %.2f, %.2f) to (%.2f, %.2f, %.2f), "
        "blocked on %zu of %d step(s)",
        static_cast<double>(move_x), static_cast<double>(move_y), static_cast<double>(seconds),
        from.x, from.y, from.z, at.x, at.y, at.z, blocked_steps, steps);
    return true;
}

// PORT: a headless verification switch: the attack button held for a
// while, frame by frame as the run loop plays it.
bool Renderer::hold_attack(float move_x, float move_y, float seconds) {
    Impl& m = *impl_;
    if (!m.player_control) {
        TL_LOG_ERROR("hold_attack needs player control enabled");
        return false;
    }
    // The same fixed step as `hold_stick`. Each step is a frame of the pad:
    // the button's share of the dispatch, then the player's update, then the
    // level (whose `auto_attack` hook sees the button already stepped).
    constexpr float kStep = 1.0f / 60.0f;
    const int steps = static_cast<int>(seconds / kStep);
    const tl::core::Vec3 from = m.player.position();
    const float facing_from = m.player.facing();
    int swings = 0;
    bool was_swinging = m.swing.active;
    for (int i = 0; i < steps; ++i) {
        // The stick walk reads the hold before the frame's X sets it
        // (`drive_player`): from the second step on the stick only aims.
        const bool held_before = m.attack_button.hold;
        m.step_attack_button(kStep, true, i == 0 && !m.attack_held, move_x, move_y);
        m.attack_held = true;
        tl::core::InputState in;
        if (!held_before) {
            in.move_x = move_x;
            in.move_y = move_y;
        }
        m.step_player(kStep, in);
        if (m.swing.active && !was_swinging) ++swings;
        was_swinging = m.swing.active;
        if (!m.swing.active) {
            const char* wanted = m.player.animation();
            if (m.player_animation != wanted) {
                apply_animation(wanted, 0.0f);
                m.player_animation = wanted;
            }
        }
        m.advance(kStep);
    }
    // Let go: the next frame of the dispatch clears the hold.
    m.step_attack_button(kStep, false, false, 0.0f, 0.0f);
    m.attack_held = false;
    const tl::core::Vec3 at = m.player.position();
    TL_LOG_INFO(
        "attack held %.2fs with the stick at (%.2f, %.2f): (%.2f, %.2f) facing %.1f to "
        "(%.2f, %.2f) facing %.1f degrees, %d swing(s) begun",
        static_cast<double>(seconds), static_cast<double>(move_x), static_cast<double>(move_y),
        from.x, from.z, static_cast<double>(facing_from * 57.295826f), at.x, at.z,
        static_cast<double>(m.player.facing() * 57.295826f), swings);
    return true;
}

bool Renderer::simulate_walk(float seconds) {
    Impl& m = *impl_;
    if (!m.player_control) {
        TL_LOG_ERROR("simulate_walk needs player control enabled");
        return false;
    }
    if (!(seconds > 0.0f)) return true;

    // A fixed 60 Hz step, not real time, so both targets take the same path.
    constexpr float kStep = 1.0f / 60.0f;
    const int steps = static_cast<int>(seconds / kStep);

    const tl::core::Vec3 from = m.player.position();
    TL_LOG_INFO("scripted walk: %.2fs at %d Hz from (%.2f, %.2f, %.2f)",
                static_cast<double>(seconds), 60, from.x, from.y, from.z);

    size_t blocked_steps = 0;
    int next_report = 0;

    for (int i = 0; i < steps; ++i) {
        const float t = static_cast<float>(i) * kStep;
        const tl::core::InputState in = tl::core::scripted_walk(t);

        m.step_player(kStep, in);
        if (m.player.blocked()) ++blocked_steps;

        const char* wanted = m.player.animation();
        if (m.player_animation != wanted) {
            apply_animation(wanted, 0.0f);
            m.player_animation = wanted;
        }
        m.advance(kStep);

        // Every half second, so the log carries the path rather than just its
        // endpoints - which is what tells a run that stopped dead against a
        // wall apart from one that never started.
        if (i >= next_report) {
            const tl::core::Vec3 at = m.player.position();
            TL_LOG_INFO("  t=%.2f pos (%.2f, %.2f, %.2f) speed %.2f %s", static_cast<double>(t),
                        at.x, at.y, at.z, static_cast<double>(m.player.speed()),
                        m.player.blocked() ? "blocked" : "clear");
            next_report += 30;
        }
    }

    const tl::core::Vec3 at = m.player.position();
    const float dx = at.x - from.x, dz = at.z - from.z;
    TL_LOG_INFO(
        "scripted walk done: (%.2f, %.2f, %.2f), %.2f from the start, blocked on "
        "%zu of %d step(s)",
        at.x, at.y, at.z, static_cast<double>(std::sqrt(dx * dx + dz * dz)), blocked_steps, steps);
    return true;
}

bool Renderer::talk_to_unit(const std::string& name, float timeout) {
    Impl& m = *impl_;
    Ogre::Vector3 at;
    std::string found;
    if (!m.unit_position(name, at, found)) {
        TL_LOG_WARN("this level places no unit matching '%s'", name.c_str());
        return false;
    }

    // Route at the unit itself: find_path snaps the goal to the nearest walkable cell,
    // where pressing A ends up too. CCharacter::approachAI's reach (see step_pending_talk).
    constexpr float kReach = 2.0f;
    const tl::core::Vec3 me = m.player.position();
    const float dx0 = at.x - me.x, dz0 = at.z - me.z;
    const float away = std::sqrt(dx0 * dx0 + dz0 * dz0);
    TL_LOG_INFO("walking to '%s' at (%.2f, %.2f), %.2f away, close enough at %.2f", found.c_str(),
                static_cast<double>(at.x), static_cast<double>(at.z), static_cast<double>(away),
                static_cast<double>(kReach));

    // A screen the previous `--talk-to` left open goes first (B), or the wait
    // below would take it for this one's answer.
    if (m.inventory_menu.visible()) m.inventory_menu.hide();
    m.pending_talk = found;
    if (away > kReach) walk_route_to(at.x, at.z, timeout);

    // Then let the arrival be noticed, exactly as the run loop would.
    constexpr float kStep = 1.0f / 60.0f;
    const auto answered = [&]() {
        return m.script_dialog_open() || m.quest_dialog.visible() ||
               m.inventory_menu.vendor() != VendorScreen::None || m.floaty_remaining > 0.0f ||
               m.fishing_state != tl::core::FishingState::Off;
    };
    for (int i = 0; i < 30 && !answered() && !m.pending_talk.empty(); ++i) {
        m.step_pending_talk();
        m.advance(kStep);
    }
    if (!answered()) {
        TL_LOG_WARN("'%s' opened no dialogue", found.c_str());
        return false;
    }
    return true;
}

// PORT: a headless verification switch (`--fishing-keys`): the fishing states
// pressed through, frame by frame.
void Renderer::fishing_keys(const std::string& keys) {
    Impl& m = *impl_;
    constexpr float kStep = 1.0f / 60.0f;
    if (m.fishing_state == tl::core::FishingState::Off && m.subject_node != nullptr) {
        // Where the level's holes are, for a run that did not reach one.
        const Ogre::Vector3 me = m.subject_node->getPosition();
        const auto report = [&](const tl::core::LevelScript& owner) {
            for (const tl::core::LevelScript::UnitTriggerView& view : owner.unit_triggers()) {
                if (!m.is_fishing_hole(view.unit)) continue;
                const float dx = view.at.x - me.x;
                const float dz = view.at.z - me.z;
                TL_LOG_INFO("fishing keys: not fishing; a hole at (%.2f, %.2f, %.2f), %.2f away",
                            static_cast<double>(view.at.x), static_cast<double>(view.at.y),
                            static_cast<double>(view.at.z),
                            std::sqrt(static_cast<double>(dx * dx + dz * dz)));
            }
        };
        report(m.script);
        for (const Impl::NestedScript& machine : m.nested_scripts) report(machine.script);
    }
    // One frame as the run loop plays it: the fishing states, then the level.
    const auto frame = [&](bool catch_press, bool stop_press) {
        m.step_fishing(kStep, catch_press, stop_press);
        m.step_player(kStep, tl::core::InputState{});
        m.advance(kStep);
    };
    size_t start = 0;
    while (start <= keys.size()) {
        const size_t comma = keys.find(',', start);
        const std::string key =
            keys.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        start = comma == std::string::npos ? keys.size() + 1 : comma + 1;
        if (key.empty()) continue;
        TL_LOG_INFO("fishing keys: %s", key.c_str());
        if (key.rfind("wait=", 0) == 0) {
            const int frames = static_cast<int>(std::atof(key.c_str() + 5) / kStep);
            for (int i = 0; i < frames; ++i) frame(false, false);
        } else if (key == "bite") {
            int i = 0;
            for (; i < 7200 && m.fishing_state != tl::core::FishingState::Off &&
                   !(m.fishing_state == tl::core::FishingState::Waiting && 0.0f < m.bobber.bite);
                 ++i) {
                frame(false, false);
            }
            TL_LOG_INFO("fishing keys: %s after %.2f s",
                        0.0f < m.bobber.bite ? "a bite" : "no bite",
                        static_cast<double>(i) * static_cast<double>(kStep));
        } else if (key == "a" || key == "b") {
            frame(key == "a", key == "b");
        } else if (key == "land") {
            for (int i = 0; i < 600 && m.fishing_state != tl::core::FishingState::Off; ++i) {
                frame(false, false);
            }
        } else if (key == "ok") {
            m.modal_screen.hide();
            frame(false, false);
        } else if (key.rfind("shot=", 0) == 0) {
            dump_talk(key.substr(5));
        } else {
            TL_LOG_WARN("fishing keys: '%s' is not a key", key.c_str());
        }
    }
}

void Renderer::inventory_keys(const std::string& keys) {
    Impl& m = *impl_;
    m.ensure_item_index();
    m.load_unit_types();
    if (m.player_inventory.effect_context().types == nullptr) m.refresh_inventory_effects();
    if (!m.inventory_menu.visible()) open_inventory();
    const auto worn = [&m]() {
        std::string out;
        for (size_t loc = 0; loc < 12; ++loc) {
            if (!m.player_inventory.occupied(loc)) continue;
            out += (out.empty() ? "" : ", ") + std::to_string(loc) + " " +
                   tl::core::item_name(m.player_inventory.slots()[loc].item);
        }
        return out;
    };
    size_t start = 0;
    while (start <= keys.size()) {
        const size_t comma = keys.find(',', start);
        const std::string key =
            keys.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        start = comma == std::string::npos ? keys.size() + 1 : comma + 1;
        if (key.empty()) continue;
        TL_LOG_INFO("inventory keys: %s", key.c_str());
        const bool bag = key.rfind("bag=", 0) == 0;
        const bool magic = key.rfind("magic=", 0) == 0;
        if (bag || magic) {
            const std::string want = key.substr(bag ? 4 : 6);
            const tl::core::ItemStats* data = m.items.find(want);
            if (data == nullptr) data = m.items.find_by_display_name(want);
            if (data == nullptr) {
                TL_LOG_WARN("inventory keys: nothing in the item index is called '%s'",
                            want.c_str());
                continue;
            }
            const tl::core::ItemStats copy = m.create_item(*data, false, true, magic);
            const int slot = m.player_inventory.find_free_slot(copy, m.unit_types);
            if (slot < 0 || !m.player_inventory.pickup_into_slot(copy, 1, slot)) {
                TL_LOG_WARN("inventory keys: no room for '%s'", want.c_str());
            } else {
                TL_LOG_INFO("inventory keys: '%s' in slot %d, %s, level %d",
                            tl::core::item_name(copy).c_str(), slot,
                            copy.identified ? "identified" : "unidentified", copy.level);
            }
        } else if (key.rfind("sel=", 0) == 0) {
            const std::string want = key.substr(4);
            const std::vector<tl::core::InventorySlot>& slots = m.player_inventory.slots();
            bool found = false;
            for (size_t i = 0; i < slots.size() && !found; ++i) {
                if (slots[i].count <= 0) continue;
                const tl::core::ItemStats& item = slots[i].item;
                if (lowered(item.name) == lowered(want) ||
                    lowered(item.display_name) == lowered(want)) {
                    m.update_menu();
                    m.inventory_menu.select_slot(i);
                    found = true;
                }
            }
            if (!found) TL_LOG_WARN("inventory keys: no '%s' in the bag", want.c_str());
        } else if (key.rfind("stat=", 0) == 0) {
            // `stat=<strength|dexterity|magic|defense>:<n>`: n unspent points onto the
            // attribute, one at a time, as the Stats page's commit does.
            // PORT: a run's shortcut past the page.
            char which[16] = {0};
            int count = 0;
            if (std::sscanf(key.c_str() + 5, "%15[a-z]:%d", which, &count) != 2) {
                TL_LOG_WARN("inventory keys: stat wants \"<attribute>:<n>\", got '%s'",
                            key.c_str());
                continue;
            }
            const std::string name = which;
            const tl::core::StatType type = name == "strength"    ? tl::core::StatType::Strength
                                            : name == "dexterity" ? tl::core::StatType::Dexterity
                                            : name == "defense"   ? tl::core::StatType::Defense
                                                                  : tl::core::StatType::Magic;
            for (int n = 0; n < count && m.player_character.unspent_stat_points() > 0; ++n) {
                m.player_character.move_stat_point(type, 1);
            }
            TL_LOG_INFO("inventory keys: %s %d, %d point(s) left", name.c_str(),
                        m.player_character.base_stat(type),
                        m.player_character.unspent_stat_points());
        } else if (key == "a") {
            m.bag_default_action(m.inventory_menu.selected_slot_index());
            const tl::core::CharacterEffects total = m.player_total();
            TL_LOG_INFO("inventory keys: worn [%s]; armour %d, %zu item(s) held", worn().c_str(),
                        m.player_character.armour_class(total), m.player_inventory.size());
        } else if (key == "fill") {
            const tl::core::ItemStats* potion = m.items.find("Health Potion");
            const int begin = m.player_inventory.pane_start(tl::core::InventoryPane::Backpack);
            const int end = begin + m.player_inventory.pane_size(tl::core::InventoryPane::Backpack);
            for (int i = begin; potion != nullptr && i < end; ++i) {
                if (m.player_inventory.occupied(static_cast<size_t>(i))) continue;
                tl::core::ItemStats one = *potion;
                one.max_stack = 1;
                m.player_inventory.pickup_into_slot(one, 1, i);
            }
        } else if (key.rfind("shot=", 0) == 0) {
            dump_inventory(key.substr(5));
        } else if (key == "close") {
            // The screen shut, so the run can go on to walk and fight.
            close_inventory();
        } else {
            TL_LOG_WARN("inventory keys: '%s' means nothing", key.c_str());
        }
        m.update_menu();
    }
}

void Renderer::vendor_keys(const std::string& keys) {
    Impl& m = *impl_;
    if (m.inventory_menu.vendor() == VendorScreen::None) {
        TL_LOG_WARN("vendor keys: no vendor screen is open");
        return;
    }
    size_t start = 0;
    while (start <= keys.size()) {
        const size_t comma = keys.find(',', start);
        const std::string key =
            keys.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        start = comma == std::string::npos ? keys.size() + 1 : comma + 1;
        if (key.empty()) continue;
        TL_LOG_INFO("vendor keys: %s", key.c_str());
        if (key == "quests") {
            // Not a key: the quest log's objectives, between two presses.
            log_quest_progress();
            continue;
        }
        if (m.inventory_menu.item_select_open() &&
            (key == "a" || key == "b" || key == "up" || key == "down" || key == "left" ||
             key == "right")) {
            // The item-select dialog has the pad (`handle_item_select`).
            if (key == "a" || key == "b") {
                if (key == "a") {
                    m.inventory_menu.confirm_item_select();
                } else {
                    m.inventory_menu.close_item_select();
                }
                m.socket_selected_gem();
            } else if (key == "up" || key == "down") {
                m.inventory_menu.item_select_step(key == "up" ? -1 : 1);
            } else {
                m.inventory_menu.item_select_step_tab(key == "right" ? 1 : -1);
            }
        } else if (key.rfind("sel=", 0) == 0) {
            // The bag's selection onto the first item of that name (its tab
            // with it).
            const std::string want = key.substr(4);
            const std::vector<tl::core::InventorySlot>& slots = m.player_inventory.slots();
            bool found = false;
            for (size_t i = 0; i < slots.size() && !found; ++i) {
                if (slots[i].count <= 0) continue;
                const tl::core::ItemStats& item = slots[i].item;
                if (lowered(item.name) == lowered(want) ||
                    lowered(item.display_name) == lowered(want)) {
                    m.inventory_menu.select_slot(i);
                    found = true;
                }
            }
            if (!found) TL_LOG_WARN("vendor keys: no '%s' in the bag", want.c_str());
        } else if (key.rfind("shot=", 0) == 0) {
            // The frame with the screen kept up, as `--talk-to`'s own dump.
            dump_talk(key.substr(5));
        } else if (key == "a" && m.inventory_menu.focused_tray() == InventoryMenu::Tray::Bag &&
                   (m.inventory_menu.vendor() == VendorScreen::Merchant ||
                    m.inventory_menu.vendor() == VendorScreen::Stash ||
                    m.inventory_menu.vendor() == VendorScreen::SharedStash)) {
            // The bag's own A on a page that leaves it alone: a gem opens the
            // item-select dialog.
            const int slot = m.inventory_menu.selected_slot_index();
            m.load_unit_types();
            if (slot >= 0 && m.player_inventory.occupied(static_cast<size_t>(slot)) &&
                m.unit_types.isa(
                    m.player_inventory.slots()[static_cast<size_t>(slot)].item.unittype,
                    tl::core::unit_type::kSocketable)) {
                m.open_gem_socketing();
            } else {
                TL_LOG_WARN("vendor keys: 'a' here only opens a gem's dialog");
            }
        } else if (key == "x" && m.housing_inventory_for(m.inventory_menu.vendor()) != nullptr) {
            m.swap_selected();
        } else if (key == "x") {
            m.trade_selected();
        } else if (key == "y" && m.inventory_menu.vendor() == VendorScreen::Combine) {
            m.transmute_selected();
        } else if (key == "a" && (m.inventory_menu.vendor() == VendorScreen::Enchant ||
                                  m.inventory_menu.vendor() == VendorScreen::EnchantShrine)) {
            if (m.enchant_selected()) TL_LOG_INFO("vendor keys: 'a' is the tray's own A here");
        } else if (key == "a" && (m.inventory_menu.vendor() == VendorScreen::RecoverGems ||
                                  m.inventory_menu.vendor() == VendorScreen::DestroyGems)) {
            m.gem_service_selected();
        } else if (key == "a" && m.inventory_menu.vendor() == VendorScreen::Retire) {
            m.retire_selected();
            if (m.retirement_requested) break;  // the screen is gone with the game
        } else if (key == "rb" || key == "lb") {
            m.inventory_menu.focus_next_tray(key == "rb" ? 1 : -1);
        } else if (key == "left" || key == "right") {
            m.inventory_menu.step_tray_tab(key == "right" ? 1 : -1);
        } else if (key == "up") {
            m.inventory_menu.select_prev();
        } else if (key == "down") {
            m.inventory_menu.select_next();
        } else if (key.rfind("gold=", 0) == 0) {
            m.player_inventory.set_gold(std::atoi(key.c_str() + 5));
        } else if (key == "save") {
            // `saveCharacter` into the run's file, as leaving the game does:
            // what the screen changed, written where a later run reads it.
            if (m.config.save_path == nullptr || !m.save_character(*m.config.save_path, false)) {
                TL_LOG_WARN("vendor keys: nothing saved");
            }
        } else {
            TL_LOG_WARN("vendor keys: '%s' means nothing", key.c_str());
        }
        m.update_menu();
    }
}

bool Renderer::walk_route_to(float x, float z, float timeout) {
    Impl& m = *impl_;
    if (!m.player_control) {
        TL_LOG_ERROR("walk_route_to needs player control enabled");
        return false;
    }
    if (!m.nav.ready()) {
        TL_LOG_ERROR("walk_route_to: this level has no navigation grid");
        return false;
    }

    const tl::core::Vec3 from = m.player.position();
    const tl::core::Vec3 to{x, from.y, z};
    const std::vector<tl::core::Vec3> route = m.nav.find_path(from, to);
    if (route.empty()) {
        TL_LOG_WARN("no route from (%.2f, %.2f) to (%.2f, %.2f)", from.x, from.z, x, z);
        return false;
    }

    float length = 0.0f;
    tl::core::Vec3 at = from;
    for (const tl::core::Vec3& step : route) {
        const float dx = step.x - at.x, dz = step.z - at.z;
        length += std::sqrt(dx * dx + dz * dz);
        at = step;
    }
    TL_LOG_INFO("route to (%.2f, %.2f): %zu leg(s), %.2f units, from (%.2f, %.2f)", x, z,
                route.size(), static_cast<double>(length), from.x, from.z);
    for (const tl::core::Vec3& step : route) {
        TL_LOG_INFO("  via (%.2f, %.2f, %.2f)", step.x, step.y, step.z);
    }

    m.player.follow(route);

    // The same fixed 60 Hz simulate_walk() uses, and for the same reason: the
    // two targets have to produce comparable logs and frames from one route.
    constexpr float kStep = 1.0f / 60.0f;
    const int steps = static_cast<int>(timeout / kStep);
    int walked = 0;
    for (; walked < steps && m.player.following(); ++walked) {
        m.step_player(kStep, tl::core::InputState{});
        const char* wanted = m.player.animation();
        if (m.player_animation != wanted) {
            apply_animation(wanted, 0.0f);
            m.player_animation = wanted;
        }
        m.advance(kStep);
    }

    const tl::core::Vec3 ended = m.player.position();
    const float dx = ended.x - x, dz = ended.z - z;
    const float miss = std::sqrt(dx * dx + dz * dz);
    if (m.player.following()) {
        TL_LOG_WARN(
            "route not finished in %.1fs: at (%.2f, %.2f, %.2f), %.2f short, %zu "
            "leg(s) left",
            static_cast<double>(timeout), ended.x, ended.y, ended.z, static_cast<double>(miss),
            m.player.waypoints_left());
        m.player.stop_following();
        return false;
    }
    TL_LOG_INFO("route walked in %.2fs: at (%.2f, %.2f, %.2f), %.2f from the point asked for",
                static_cast<double>(static_cast<float>(walked) * kStep), ended.x, ended.y, ended.z,
                static_cast<double>(miss));
    return true;
}

void Renderer::skip_cutscene() {
    Impl& m = *impl_;
    std::vector<tl::core::ScriptEvent> events;
    m.script.skip_cutscene(m.quest_log.state(), events);
    TL_LOG_INFO("skip: %zu event(s)", events.size());
    if (!events.empty()) m.apply_script_events(events, m.scripted_nodes);
    m.player.stop_following();
    m.scripted_walk_path = 0;
}

// PORT: the renderer's interface to `main.cpp` (headless runs).
void Renderer::reset_script() { impl_->reset_script(); }

// PORT: a headless verification switch (`--quest`): a quest's state set before the level.
bool Renderer::set_quest(const std::string& specification) {
    Impl& m = *impl_;
    if (!m.quest_log.state().apply_specification(specification)) {
        TL_LOG_ERROR(
            "--quest wants \"<name>\", \"<name>:complete\" or \"<name>:active\", "
            "got '%s'",
            specification.c_str());
        return false;
    }
    // An active quest is also held, as a floor quest is given: no NPC, so the
    // seed stays -1. Its objectives are then placed like any held quest's.
    const std::string suffix = ":active";
    if (specification.size() > suffix.size() &&
        lowered(specification.substr(specification.size() - suffix.size())) == suffix) {
        m.load_quest_book();
        m.load_stat_graphs();
        const std::string name = specification.substr(0, specification.size() - suffix.size());
        if (const tl::core::QuestDefinition* quest = m.quest_book.find(name)) {
            m.quest_log.accept(*quest, m.quest_level_of(*quest), static_cast<uint32_t>(-1),
                               m.stat_graphs);
        }
    }
    return true;
}

void Renderer::give_starting_quests() {
    Impl& m = *impl_;
    // The class's file, found the way load_player_stats() finds it: by the
    // chosen class, or - a run that named none - by the subject mesh's stem.
    std::string stem = m.player_class;
    if (stem.empty() && m.subject != nullptr) {
        stem = m.subject->getMesh()->getName();
        const size_t slash = stem.find_last_of('/');
        if (slash != std::string::npos) stem = stem.substr(slash + 1);
        const size_t dot = stem.find_last_of('.');
        if (dot != std::string::npos) stem.resize(dot);
    }
    if (stem.empty()) return;
    // By class_unit_path(), not basename: with the console data mounted the class file
    // exists twice and archive_path_unique refuses both.
    std::string path = class_unit_path(stem);
    if (path.empty()) path = archive_path_unique(stem + ".DAT.adm");
    if (path.empty()) {
        TL_LOG_WARN("starting quests: no unit file for '%s'", stem.c_str());
        return;
    }
    const std::vector<uint8_t> bytes = read_resource(path);
    if (bytes.empty()) return;
    const tl::formats::AdmFile file = tl::formats::parse_adm(bytes.data(), bytes.size());
    if (!file.ok) return;
    m.load_quest_book();
    m.load_stat_graphs();
    for (const tl::formats::AdmSection& child : file.root.children) {
        if (child.name != "QUESTS") continue;
        for (const tl::formats::AdmRecord& record : child.records) {
            const tl::core::QuestDefinition* quest = m.quest_book.find(record.text);
            if (quest == nullptr) continue;
            // Given with no NPC: `CQuest +0x1c` stays -1, as in
            // apply_floor_quests().
            const uint32_t seed = static_cast<uint32_t>(-1);
            if (!m.quest_log.accept(*quest, m.quest_level_of(*quest), seed, m.stat_graphs))
                continue;
            TL_LOG_INFO("starting quests: '%s' given", quest->name.c_str());
        }
    }
}

bool Renderer::load_layout(const std::string& reference) {
    Impl& m = *impl_;

    // Timed: the console streams the archive from the SD card.
    log_memory("before the layout");
    m.paint_loading_screen();
    Ogre::Timer* timer = m.root ? m.root->getTimer() : nullptr;
    const unsigned long started = timer ? timer->getMilliseconds() : 0;

    m.level_reference = reference;
    // `CLevel::loadRoomLayout` caches
    // particles from here on; a title backdrop comes through `loadMenuLevel`'s
    // overload, which does not.
    std::string upper_reference = tl::core::upper_ascii(reference);
    for (char& c : upper_reference) {
        if (c == '\\') c = '/';
    }
    if (upper_reference.find("/MAINMENUS/") == std::string::npos) {
        renderer_detail::set_caching_particles();
    }
    m.loading_menu_level = upper_reference.find("/MAINMENUS/") != std::string::npos;
    // An ordinary level, not a generated floor - so run()'s exit check stays
    // off, defensively, even though the only path back here from a dungeon
    // already goes through restart() and a fresh Impl.
    m.dungeon_active = false;
    m.dungeon_transition_pending = false;
    // The town is the `Town` dungeon's only floor (TOWN.DAT: one stratum, IS_TOWN). Any
    // other layout loaded on its own belongs to no dungeon and is not remembered.
    {
        const std::string file = reference.substr(reference.find_last_of("/\\") + 1);
        if (tl::core::upper_ascii(file) == "TOWN.LAYOUT") {
            // `TOWN.DAT` authors `ALLOW_PORTALS:False`.
            m.set_level_identity("TOWN", 0, /*is_town=*/true, /*dont_store=*/false,
                                 /*allows_portals=*/false);
        } else {
            m.set_level_identity(std::string(), 0, false, false, false);
        }
    }
    // The town as CGameClient::loadLevel builds any
    // floor: resetGameSeed(0), the template, the level seed and CLevel::loadRoomLayout's
    // choice (the piece seeded from it and 1 << 0); resetGameSeed(0), chooseUnitSpawners,
    // both generators to the clock. Its ruleset says POPULATE:False.
    std::string piece = reference;
    tl::core::LevelTemplate* town_template = nullptr;
    if (m.level_is_town) {
        tl::core::GameSeed& seed = tl::core::game_seed();
        tl::core::RunicRandom& g_rand = tl::core::game_random();
        tl::core::Dungeon* town = loaded_dungeon_manager().dungeon_by_name("TOWN");
        seed.current_dungeon = town;
        m.layout_level_seed = tl::core::level_seed(tl::core::kLevelSeedSetting, seed, 0);
        if (town != nullptr) {
            const tl::core::DungeonFiles files = archive_dungeon_files();
            tl::core::reset_game_seed(seed, 0, g_rand);
            town_template = town->get_level_template_for_depth(0, files, g_rand);
        }
        TL_LOG_INFO("Seeding: %d", m.layout_level_seed);
        if (town_template != nullptr && !town_template->chunks.empty()) {
            if (town_template->randomized) town_template->delete_layouts();
            const tl::core::LevelLayout* layout =
                tl::core::choose_room_layout(*town_template, g_rand, m.layout_level_seed);
            if (layout != nullptr && !layout->pieces.empty() &&
                layout->pieces[0] < town_template->chunks.size()) {
                piece = town_template->chunks[layout->pieces[0]].path;
            }
        } else {
            TL_LOG_WARN("the Town dungeon has no template for depth 0; loading '%s' as asked",
                        reference.c_str());
        }
        if (tl::core::upper_ascii(piece) != tl::core::upper_ascii(reference)) {
            TL_LOG_INFO("town: the template's room piece is '%s'", piece.c_str());
            m.level_reference = piece;
        }
        m.layout_seeding = true;
    }
    const bool ok = m.place_layout(piece, Ogre::Vector3::ZERO, Ogre::Quaternion::IDENTITY, 0,
                                   m.level_is_town ? 1u : 0u);
    m.layout_seeding = false;
    if (ok && town_template != nullptr) {
        tl::core::reset_game_seed(tl::core::game_seed(), 0, tl::core::game_random());
        tl::core::choose_unit_spawners(*town_template, m.archive_spawn_class_lookup(),
                                       tl::core::game_random());
        // What a hooked fish rolls (`CPlayer::fishingAICatch` reads the level's
        // template, `+0x1d8 -> +0x5a8`).
        m.level_fish_spawn_class = town_template->chosen_spawn_classes[6];
        tl::core::reseed_level_generators();
    }
    if (ok) m.restore_level_state();
    if (ok) m.bake_static_scenery();

    if (timer) {
        const float seconds = static_cast<float>(timer->getMilliseconds() - started) * 0.001f;
        TL_LOG_INFO("layout loaded in %.2f s", static_cast<double>(seconds));
    }

    // After the layout, because the ambience comes out of the layout's own
    // `Sound` nodes and those are only known once it has been placed.
    if (ok) m.start_level_audio(reference);

    // The HUD, once the level it describes exists. Its banner starts running
    // down here rather than when the renderer came up, so a level that took
    // twenty seconds to load still gets its second.
    if (ok) {
        m.raise_arrival_banner();
        m.refresh_game_hud();
    }
    return ok;
}

void Renderer::set_passability_dump(const std::string& path) {
    impl_->passability_dump_path = path;
}

// PORT: a headless verification switch (`--logic-trace`): the port's own trace of the level logic.
void Renderer::set_logic_trace(const std::string& path) {
    Impl& m = *impl_;
    if (m.logic_trace != nullptr) {
        std::fclose(m.logic_trace);
        m.logic_trace = nullptr;
    }
    if (path.empty()) return;
    m.logic_trace = std::fopen(path.c_str(), "w");
    if (m.logic_trace == nullptr) {
        TL_LOG_WARN("could not open logic trace '%s'", path.c_str());
        return;
    }
    TL_LOG_INFO("logic trace: writing to '%s'", path.c_str());
}

void Renderer::set_rng_trace(const std::string& path) {
    Impl& m = *impl_;
    if (m.rng_trace_file != nullptr) {
        std::fclose(m.rng_trace_file);
        m.rng_trace_file = nullptr;
    }
    tl::core::game_random().set_trace(nullptr);
    if (path.empty()) return;
    m.rng_trace_file = std::fopen(path.c_str(), "a");
    if (m.rng_trace_file == nullptr) {
        TL_LOG_WARN("could not open rng trace '%s'", path.c_str());
        return;
    }
    m.rng_trace.user = m.rng_trace_file;
    m.rng_trace.write = [](void* user, const char* line) {
        std::FILE* file = static_cast<std::FILE*>(user);
        std::fputs(line, file);
        std::fputc('\n', file);
        std::fflush(file);
    };
    tl::core::game_random().set_trace(&m.rng_trace);
    TL_LOG_INFO("rng trace: appending to '%s'", path.c_str());
}

void Renderer::set_level_entry_type(int entry_type) { impl_->level_entry_type = entry_type; }

// PORT: the renderer's interface to `main.cpp`: a dungeon floor loaded, with the
// loading screen and the time it took. Generation is `core/dungeon`'s.
bool Renderer::load_dungeon(const std::string& name, int floor_number) {
    Impl& m = *impl_;

    log_memory("before the dungeon");
    m.paint_loading_screen();
    Ogre::Timer* timer = m.root ? m.root->getTimer() : nullptr;
    const unsigned long started = timer ? timer->getMilliseconds() : 0;

    renderer_detail::set_caching_particles();
    const bool ok = m.place_dungeon_floor(name, floor_number);
    report_stale_materials(m.scene, "a dungeon floor load");

    if (timer) {
        const float seconds = static_cast<float>(timer->getMilliseconds() - started) * 0.001f;
        TL_LOG_INFO("dungeon floor generated and loaded in %.2f s", static_cast<double>(seconds));
    }
    return ok;
}

namespace renderer_detail {
/// The directory segment right after `media/layouts/` in a ruleset path -
/// `media/layouts/mine/firstfloorrules.dat` -> `mine`. Empty when the path
/// does not have that shape.
std::string biome_key_from_ruleset(const std::string& ruleset) {
    const std::string prefix = "media/layouts/";
    const std::string lower = lowered(ruleset);
    if (lower.compare(0, prefix.size(), prefix) != 0) return {};
    const size_t start = prefix.size();
    const size_t slash = lower.find('/', start);
    if (slash == std::string::npos) return {};
    return lower.substr(start, slash - start);
}

/// A display name for a biome key: Main's seven by name, else the key capitalised.
std::string biome_display_name(const std::string& key) {
    static const std::map<std::string, std::string> kNames = {
        {"mine", "Mine"},     {"crypt", "Crypt"}, {"sunkentemple", "Sunken Temple"},
        {"cave", "Cave"},     {"lava", "Lava"},   {"fortress", "Fortress"},
        {"palace", "Palace"},
    };
    const auto found = kNames.find(key);
    if (found != kNames.end()) return found->second;
    std::string name = key;
    if (!name.empty())
        name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    return name;
}

}  // namespace renderer_detail
// PORT: the port's own Choose Level list (`main.cpp`): every floor of `Main`,
// by stratum, for a menu the console does not have.
std::vector<Renderer::MainFloor> Renderer::list_main_floors() {
    std::vector<MainFloor> floors;

    const std::string def_path = archive_path_unique(std::string("Main") + ".DAT.adm");
    if (def_path.empty()) {
        TL_LOG_WARN("no 'Main' dungeon in the archive; Choose Level has nothing to list");
        return floors;
    }
    const std::vector<uint8_t> bytes = read_resource(def_path);
    if (bytes.empty()) return floors;
    const tl::formats::AdmFile adm = tl::formats::parse_adm(bytes.data(), bytes.size());
    const tl::core::DungeonDef def = tl::core::parse_dungeon_def(adm);
    if (!def.ok) {
        TL_LOG_WARN("could not read '%s' as a dungeon definition", def_path.c_str());
        return floors;
    }

    int global_floor = 1;
    std::string previous_biome;
    int within_biome = 0;
    for (const tl::core::DungeonStrata& strata : def.strata) {
        const int floor_count = std::max(1, strata.floors);
        const std::string biome = biome_key_from_ruleset(strata.ruleset);
        const std::string biome_label = biome_display_name(biome);
        const std::string stem_lower = lowered(strata.ruleset);

        for (int i = 0; i < floor_count; ++i) {
            if (biome != previous_biome) {
                within_biome = 1;
                previous_biome = biome;
            } else {
                ++within_biome;
            }

            MainFloor f;
            f.floor = global_floor++;
            f.biome = biome;
            f.biome_label = biome_label;
            f.within_biome = within_biome;

            // A label, not a reading: the ruleset's filename hints at the room shape.
            std::string suffix;
            if (stem_lower.find("ordrak") != std::string::npos) {
                suffix = " (Ordrak)";
            } else if (stem_lower.find("miniboss") != std::string::npos) {
                suffix = " (Miniboss)";
            } else if (stem_lower.find("boss") != std::string::npos) {
                suffix = " (Boss)";
            }
            f.label = biome_label + " " + std::to_string(within_biome) + suffix;

            floors.push_back(std::move(f));
        }
    }
    return floors;
}

void Renderer::set_audio_enabled(bool on) { impl_->audio_enabled = on; }

// PORT: the renderer's interface to `main.cpp`: the pet chosen.
void Renderer::set_pet(const std::string& unit) {
    Impl& m = *impl_;
    m.config.pet = unit;
    // A different animal: whatever a fish had made of the one before goes with
    // it.
    m.pet.set_transformation(0.0f, std::string());
    if (unit.empty()) {
        m.destroy_pet_entity();
        m.pet.clear();
        return;
    }
    // Only if there is a level to stand in. Before `enable_player_control()`
    // this is a choice being recorded, not an animal being placed.
    if (m.player_control) m.spawn_pet(unit);
}

// PORT: the renderer's interface to `main.cpp`.
const std::string& Renderer::pet_unit() const { return impl_->config.pet; }

// PORT: a headless verification switch (`--send-pet-to-town`).
bool Renderer::send_pet_to_town() { return impl_->send_pet_to_town(); }

// PORT: the renderer's interface: the stance cycled (`Pet::cycle_stance`) and the HUD redrawn.
void Renderer::cycle_pet_stance() {
    Impl& m = *impl_;
    if (!m.pet.alive()) return;
    m.pet.cycle_stance();
    m.apply_pet_stance();
    const tl::core::PetStance stance = m.pet.stance();
    TL_LOG_INFO("pet: %s", stance == tl::core::PetStance::Aggressive  ? "aggressive"
                           : stance == tl::core::PetStance::Defensive ? "defensive"
                                                                      : "passive");
    m.refresh_game_hud();
}

// PORT: a headless verification switch (`--pet-fish`): a fish found by name and fed to the pet.
bool Renderer::feed_pet(const std::string& fish_name) {
    Impl& m = *impl_;
    m.ensure_item_index();
    // The bag first: two different fish share the name `Improbably Large Fish`.
    const tl::core::ItemStats* fish = nullptr;
    int from_bag = -1;
    for (size_t i = 0; i < m.player_inventory.slots().size(); ++i) {
        if (m.player_inventory.slots()[i].count <= 0) continue;
        const tl::core::ItemStats& item = m.player_inventory.slots()[i].item;
        if (item.unittype.find("FISH") == std::string::npos) continue;
        if (lowered(item.name) == lowered(fish_name) ||
            lowered(item.display_name) == lowered(fish_name)) {
            fish = &item;
            from_bag = static_cast<int>(i);
            break;
        }
    }
    // Otherwise the index: by `NAME` first, because a person naming a fish says
    // "Web Fish" and the data calls it `Trout`.
    if (fish == nullptr) fish = m.items.find(fish_name);
    if (fish == nullptr) fish = m.items.find_by_display_name(fish_name);
    if (fish == nullptr) {
        TL_LOG_WARN("pet: nothing in the item index is called '%s'", fish_name.c_str());
        return false;
    }
    // Out of the bag, naming a fish here means the same thing as pressing A on
    // it; one the character does not carry is used straight from the index.
    bool fed = false;
    if (from_bag >= 0) {
        fed = m.feed_pet_from(m.player_inventory, static_cast<size_t>(from_bag));
    } else {
        const tl::core::ItemStats chosen = *fish;
        tl::core::ItemUseResult used;
        fed = m.use_item_on_pet(chosen, 1, /*by_player=*/true, used);
    }
    if (fed) {
        m.update_menu();
        m.refresh_game_hud();
    }
    return fed;
}

// PORT: a headless verification switch (`--pet-stance`).
bool Renderer::set_pet_stance(const std::string& name) {
    Impl& m = *impl_;
    const std::string want = lowered(name);
    tl::core::PetStance stance;
    if (want == "aggressive") {
        stance = tl::core::PetStance::Aggressive;
    } else if (want == "defensive") {
        stance = tl::core::PetStance::Defensive;
    } else if (want == "passive") {
        stance = tl::core::PetStance::Passive;
    } else {
        return false;
    }
    // Set on the animal when there is one, so a run that names a stance before
    // the level is up is not silently ignored: `Pet::spawn()` carries the
    // stance across, which is what makes a level change keep it.
    m.pet.set_stance(stance);
    m.apply_pet_stance();
    m.refresh_game_hud();
    TL_LOG_INFO("pet: %s", want.c_str());
    return true;
}

// PORT: the renderer's interface to `main.cpp` (headless runs).
bool Renderer::pet_in_world() const {
    const Impl& m = *impl_;
    return m.pet.alive() && m.pet.in_world();
}

// PORT: a headless verification switch (`--hurt`).
void Renderer::hurt_player(float amount) { impl_->hurt_player(amount, "--hurt"); }

// PORT: a headless verification switch (`--item-slot`): a hotbar slot pressed, health and mana logged.
void Renderer::press_item_slot(int index, float seconds) {
    Impl& m = *impl_;
    TL_LOG_INFO("item slot %d: before, %.1f of %.1f health, %.1f of %.1f mana", index,
                static_cast<double>(m.player_health.current),
                static_cast<double>(m.player_health.max),
                static_cast<double>(m.player_character.mp()),
                static_cast<double>(m.player_character.max_mp()));
    m.activate_item_slot(index);
    advance_scene(seconds);
    TL_LOG_INFO("item slot %d: %.1fs later, %.1f of %.1f health, %.1f of %.1f mana", index,
                static_cast<double>(seconds), static_cast<double>(m.player_health.current),
                static_cast<double>(m.player_health.max),
                static_cast<double>(m.player_character.mp()),
                static_cast<double>(m.player_character.max_mp()));
}

// PORT: a headless verification switch (`--pick-up-quest-items`).
int Renderer::pick_up_quest_items() {
    Impl& m = *impl_;
    int taken = 0;
    for (size_t i = m.dropped_items.size(); i-- > 0;) {
        if (m.dropped_items[i].item.quest_guid == -1) continue;
        const size_t before = m.dropped_items.size();
        m.item_targeted = static_cast<int>(i);
        m.pick_up_targeted_item();
        if (m.dropped_items.size() < before) ++taken;
    }
    m.item_targeted = -1;
    return taken;
}

// PORT: a headless verification switch (`--take-item`): the dropped items of this name picked up.
int Renderer::pick_up_items(const std::string& name) {
    Impl& m = *impl_;
    const std::string want = tl::core::upper_ascii(name);
    int taken = 0;
    for (size_t i = m.dropped_items.size(); i-- > 0;) {
        if (m.dropped_items[i].is_gold) continue;
        if (tl::core::upper_ascii(m.dropped_items[i].item.name) != want) continue;
        const size_t before = m.dropped_items.size();
        m.item_targeted = static_cast<int>(i);
        m.pick_up_targeted_item();
        if (m.dropped_items.size() < before) ++taken;
    }
    m.item_targeted = -1;
    return taken;
}

// PORT: a headless verification switch (`--drop-quest-items`).
int Renderer::drop_quest_items() {
    Impl& m = *impl_;
    int dropped = 0;
    for (size_t slot = tl::core::Inventory::kFixedSlotCount;
         slot < m.player_inventory.slots().size(); ++slot) {
        const tl::core::InventorySlot held = m.player_inventory.slots()[slot];
        if (held.count <= 0 || held.item.quest_guid == -1) continue;
        m.player_inventory.remove_item(slot, held.count);
        m.drop_item_near_player(held.item, held.count);
        ++dropped;
    }
    return dropped;
}

// PORT: a headless verification switch (`--quest-progress`): the active quests' objectives logged.
void Renderer::log_quest_progress() {
    Impl& m = *impl_;
    const std::function<void(const std::vector<tl::core::QuestObjective>&, const char*, int)> walk =
        [&walk](const std::vector<tl::core::QuestObjective>& list, const char* group, int depth) {
            for (const tl::core::QuestObjective& o : list) {
                TL_LOG_INFO("quest progress:   %*s%s '%s' %d of %d%s%s", depth * 2, "", group,
                            o.resolved.empty() ? o.unit_name().c_str() : o.resolved.c_str(), o.have,
                            o.wanted, o.populated ? ", placed" : "", o.done ? ", done" : "");
                walk(o.children, "child", depth + 1);
            }
        };
    for (const tl::core::QuestEntry& entry : m.quest_log.entries()) {
        if (entry.status != tl::core::QuestStatus::Active) continue;
        if (entry.defeat.empty() && entry.acquire.empty() && entry.populate.empty()) continue;
        TL_LOG_INFO("quest progress: '%s' on floor %d, objectives %s", entry.name().c_str(),
                    entry.depth, entry.objectives_complete() ? "complete" : "open");
        walk(entry.acquire, "acquire", 0);
        walk(entry.defeat, "defeat", 0);
        walk(entry.populate, "populate", 0);
    }
}

void Renderer::bring_player_pets(const CarriedPlayer& carried) {
    Impl& m = *impl_;
    if (m.subject_node == nullptr) return;
    // CGameClient::loadLevel adds each back with the player; CPlayer::levelResetting
    // puts each pet by its master:
    // teleportToMaster(count * 0.25), landing at randomOpenPosition(8).
    const Ogre::Vector3 master = m.subject_node->getPosition();
    const float facing = m.subject_node->getOrientation().getYaw().valueRadians();
    const size_t pets = carried.companions.size() + (m.pet.alive() ? 1 : 0);
    const float distance = static_cast<float>(pets) * 0.25f;
    for (const CarriedPlayer::CarriedCompanion& companion : carried.companions) {
        Ogre::Vector3 at = master;
        if (m.spawn_unit(companion.unit, at, /*owner=*/0, companion.level, false, false,
                         m.subject_node->getOrientation()) == nullptr) {
            TL_LOG_WARN("pet: %s could not be brought along", companion.unit.c_str());
            continue;
        }
        const int64_t node = m.spawned[0].back().id;
        m.script.add_spawned_unit(node, companion.unit, tl::core::Vec3{at.x, at.y, at.z},
                                  /*hunts=*/false, facing);
        if (companion.level > 0) m.script.set_unit_level(node, companion.level);
        m.script.set_unit_experience(node, companion.experience);
        if (companion.hp > 0.0f) m.script.set_unit_hp(node, companion.hp);
        std::vector<tl::core::ScriptEvent> events;
        m.script.make_player_pet(node, m.quest_log.state(), events);
        if (!events.empty()) m.apply_script_events(events, m.scripted_nodes);
        m.script.put_pet_by_master(node, tl::core::Vec3{master.x, master.y, master.z}, distance);
        tl::core::Vec3 placed;
        if (m.script.unit_position(node, placed)) at = Ogre::Vector3(placed.x, placed.y, placed.z);
        TL_LOG_INFO("pet: %s brought along to (%.2f, %.2f, %.2f)", companion.unit.c_str(),
                    static_cast<double>(at.x), static_cast<double>(at.y),
                    static_cast<double>(at.z));
    }
}

bool Renderer::accept_offer() {
    Impl& m = *impl_;
    if (m.offered_quest == nullptr) {
        TL_LOG_INFO("accept: no offer is open");
        return false;
    }
    const tl::core::QuestDefinition* quest = m.offered_quest;
    const bool was_offer = m.quest_dialog.state().offer;
    // A line that is no offer has only its `OK`.
    const bool taken = was_offer && m.accept_offered_quest(*quest);
    m.finish_talk_window(!was_offer ? 2 : taken ? 3 : 4);
    return true;
}

void Renderer::set_godspeed(bool on) { impl_->set_godspeed(on); }
void Renderer::level_up_to(int level) { impl_->debug_level_row(level); }

bool Renderer::use_prop(const std::string& unit) {
    Impl& m = *impl_;
    if (m.subject_node == nullptr) return false;
    const Ogre::Vector3 me = m.subject_node->getPosition();
    const std::string want = tl::core::upper_ascii(unit);
    int best_machine = Impl::kNoMachine;
    int64_t best_node = 0;
    float best = 0.0f;
    tl::core::Vec3 best_at;
    const auto consider = [&](int machine, const tl::core::LevelScript& owner) {
        for (const tl::core::LevelScript::UnitTriggerView& view : owner.unit_triggers()) {
            if (tl::core::upper_ascii(view.unit) != want) continue;
            // As the pad's pick: a one-shot past its last state is not armed
            // (`+0x1f0`, which `CTriggerUnit::interact`
            // tests).
            if (!view.armed) continue;
            const float dx = view.at.x - me.x, dz = view.at.z - me.z;
            const float d = dx * dx + dz * dz;
            if (best_node != 0 && d >= best) continue;
            best = d;
            best_machine = machine;
            best_node = view.id;
            best_at = view.at;
        }
    };
    consider(Impl::kLevelMachine, m.script);
    for (size_t i = 0; i < m.nested_scripts.size(); ++i) {
        consider(static_cast<int>(i), m.nested_scripts[i].script);
    }
    if (best_node == 0) {
        TL_LOG_WARN("use prop: no enabled '%s' in this level", unit.c_str());
        return false;
    }
    TL_LOG_INFO("use prop: A on '%s' (%lld) at (%.2f, %.2f), %.2f away", unit.c_str(),
                static_cast<long long>(best_node), static_cast<double>(best_at.x),
                static_cast<double>(best_at.z), static_cast<double>(std::sqrt(best)));
    m.trigger_targeted_machine = best_machine;
    m.trigger_targeted_node = best_node;
    m.interact_targeted_trigger();
    return true;
}

bool Renderer::slay_unit(const std::string& unit) {
    Impl& m = *impl_;
    const std::string want = tl::core::upper_ascii(unit);
    const bool quest_unit = want == "@QUEST";
    for (const auto& entry : m.scene_units) {
        if (entry.second.node == 0) continue;
        if (quest_unit ? m.quest_unit_tags.find(entry.second.node) == m.quest_unit_tags.end()
                       : tl::core::upper_ascii(entry.first) != want) {
            continue;
        }
        const int64_t node = entry.second.node;
        tl::core::LevelScript* owner = m.script_owning(node);
        if (owner == nullptr || owner->unit_is_dead(node)) continue;
        TL_LOG_INFO("slay: %s (%lld)", entry.first.c_str(), static_cast<long long>(node));
        const std::string name = entry.first;
        m.hurt_unit(node, name, 1.0e9f, false, 0, owner);
        return true;
    }
    TL_LOG_WARN("slay: no living '%s' in this level", unit.c_str());
    return false;
}

bool Renderer::press_a() {
    Impl& m = *impl_;
    if (!m.update_item_selection(true)) {
        TL_LOG_INFO("press A: nothing in reach");
        return false;
    }
    // `setTargetItem` (or a sleeping mimic as the target), then the frames
    // `getItemAI` / `attackAI` need to walk, turn and use it, rendered, up to
    // ten seconds.
    if (m.mimic_targeted_machine != Impl::kNoMachine) {
        m.player_item = Impl::PlayerItemTarget{};
        m.player_item.kind = Impl::PlayerItemTarget::Kind::Mimic;
        m.player_item.machine = m.mimic_targeted_machine;
        m.player_item.node = m.mimic_targeted_node;
        TL_LOG_INFO("press A: the mimic %lld is the target",
                    static_cast<long long>(m.mimic_targeted_node));
    } else {
        m.target_selected_item();
    }
    constexpr float kStep = 1.0f / 60.0f;
    for (int i = 0; i < 600 && m.player_item.kind != Impl::PlayerItemTarget::Kind::None; ++i) {
        m.step_player(kStep, tl::core::InputState{});
        const char* wanted = m.player.animation();
        if (m.player_animation != wanted) {
            apply_animation(wanted, 0.0f);
            m.player_animation = wanted;
        }
        m.advance(kStep);
        if (!m.root->renderOneFrame(kStep)) break;
    }
    if (m.player_item.kind != Impl::PlayerItemTarget::Kind::None) {
        TL_LOG_INFO("press A: the target item is still being walked to");
    }
    return true;
}

// PORT: a headless verification switch (`--roll-death-drop`): a unit's death loot rolled at the player.
void Renderer::roll_death_drop(const std::string& unit, int count) {
    Impl& m = *impl_;
    if (m.subject_node == nullptr) return;
    for (int i = 0; i < count; ++i) {
        m.drop_death_loot_at(from_ogre(m.subject_node->getPosition()), /*node=*/-1, unit,
                             /*attacker=*/0);
    }
}

bool Renderer::give_player_item(const std::string& item_name, bool enchant) {
    Impl& m = *impl_;
    m.ensure_item_index();
    m.load_unit_types();
    const tl::core::ItemStats* data = m.items.find(item_name);
    if (data == nullptr) data = m.items.find_by_display_name(item_name);
    if (data == nullptr) {
        TL_LOG_WARN("give: nothing in the item index is called '%s'", item_name.c_str());
        return false;
    }
    if (m.player_inventory.effect_context().types == nullptr) m.refresh_inventory_effects();
    const auto figures = [&m](const char* when) {
        const tl::core::CharacterEffects total = m.player_total();
        const tl::core::CharacterProgression& c = m.player_character;
        TL_LOG_INFO(
            "give: %s: str %d dex %d mag %d def %d, armour %d (worn %d), block %d, "
            "crit %d, %zu set bonus(es)",
            when, c.strength(total), c.dexterity(total), c.magic(total), c.defense(total),
            c.armour_class(total), m.player_inventory.armor_bonus(), c.block_chance(total),
            c.crit_chance(total), m.player_inventory.effects().affixes().size());
    };
    figures("before");
    // `CConsole::spawnItem` is the original's
    // own way in: `createItem`.
    const tl::core::ItemStats copy = m.create_item(*data, false, true, enchant);
    std::string affix_names;
    for (const tl::core::ItemStats::RolledAffix& rolled : copy.affixes) {
        affix_names += (affix_names.empty() ? "" : ", ") + rolled.affix.name;
    }
    TL_LOG_INFO(
        "give: '%s' made: armour %d, %zu armour effect(s), damage %d, set '%s', "
        "requires level %d, affixes [%s]",
        copy.display_name.c_str(), copy.graph_ac, copy.combat_effects.size(), copy.damage,
        copy.set.c_str(), copy.min_level_required, affix_names.c_str());
    const int slot = m.player_inventory.pickup(copy, 1, m.unit_types);
    if (slot < 0) {
        TL_LOG_WARN("give: no room for '%s'", copy.display_name.c_str());
        return false;
    }
    // Worn without the requirement gate: a verification run gives what it names.
    if (!m.player_inventory.equip_into_first_free_location(static_cast<size_t>(slot), m.unit_types,
                                                           nullptr)) {
        TL_LOG_INFO("give: '%s' goes in the bag", copy.display_name.c_str());
    }
    m.sync_equipped_weapon();
    m.refresh_skill_effects();
    figures("after");
    return true;
}

bool Renderer::give_pet_item(const std::string& item_name, int count) {
    Impl& m = *impl_;
    if (!m.pet.alive()) return false;
    m.ensure_item_index();
    const tl::core::ItemStats* item = m.items.find(item_name);
    if (item == nullptr) item = m.items.find_by_display_name(item_name);
    if (item == nullptr) {
        TL_LOG_WARN("pet: nothing in the item index is called '%s'", item_name.c_str());
        return false;
    }
    m.load_unit_types();
    if (!m.pet.bag().add_item(m.create_item(*item, false, true), count, m.unit_types)) {
        // `CCharacter::getItem`.
        m.play_voice(tl::core::SoundSlot::VoicePetInventoryFull);
        return false;
    }
    m.load_stat_graphs();
    TL_LOG_INFO("pet bag: %d x %s, worth %d gold", count, item->name.c_str(),
                tl::core::item_sell_price(*item, m.stat_graphs, count));
    return true;
}

// PORT: a headless verification switch (`--pet-learn`): a scroll given to the pet and learned.
bool Renderer::pet_learn(const std::string& scroll_name) {
    Impl& m = *impl_;
    if (!give_pet_item(scroll_name)) return false;
    // The copy just made: the last occupied slot holding that name.
    int slot = -1;
    const std::vector<tl::core::InventorySlot>& slots = m.pet.bag().slots();
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].count > 0 &&
            (slots[i].item.name == scroll_name || slots[i].item.display_name == scroll_name)) {
            slot = static_cast<int>(i);
        }
    }
    if (slot < 0) return false;
    m.load_unit_types();
    m.ensure_effect_data();
    m.refresh_inventory_effects();
    const tl::core::ItemStats item = slots[static_cast<size_t>(slot)].item;
    if (!tl::core::spell_learnable(item, item.uses, m.pet_use_target(), m.unit_types,
                                   m.stat_graphs)) {
        TL_LOG_INFO("pet tray: the pet cannot learn '%s'", item.display_name.c_str());
        return false;
    }
    m.learn_spell_from_tray(/*pet_owner=*/true, static_cast<size_t>(slot));
    return true;
}

// PORT: a headless verification switch (`--pet-use`): an item given to the pet and used.
bool Renderer::pet_use(const std::string& item_name) {
    Impl& m = *impl_;
    if (!give_pet_item(item_name)) return false;
    // The copy just made: the last occupied slot holding that name.
    int slot = -1;
    const std::vector<tl::core::InventorySlot>& slots = m.pet.bag().slots();
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].count > 0 &&
            (slots[i].item.name == item_name || slots[i].item.display_name == item_name)) {
            slot = static_cast<int>(i);
        }
    }
    if (slot < 0) return false;
    m.refresh_inventory_effects();
    const tl::core::ItemUseTarget target = m.pet_use_target();
    const tl::core::ItemStats& item = slots[static_cast<size_t>(slot)].item;
    TL_LOG_INFO(
        "pet tray: '%s' (%s%s) on the pet (health %.0f of %.0f, mana %.1f of %.0f, "
        "portals %s)",
        item_name.c_str(), item.unittype.c_str(),
        m.unit_types.isa(item.unittype, tl::core::unit_type::kNoPets) ? ", NOPETS" : "",
        static_cast<double>(target.hp), static_cast<double>(target.max_hp),
        static_cast<double>(target.mana), static_cast<double>(target.max_mana),
        target.level_allows_portals ? "allowed" : "not allowed");
    return m.use_pet_item(static_cast<size_t>(slot));
}

/// Stand the player's pet where a main-menu backdrop says to: the layout's `Point of
/// Interest` Property Node. The pet is the default Dog.
void Renderer::dress_character() {
    Impl& m = *impl_;
    // The unit's numbers first: equip_starting_weapon() reads `player_stats.right_hand`.
    m.load_player_stats();
    m.equip_starting_weapon();
    m.equip_carried_particles();
}

// PORT: the renderer's interface to `main.cpp`.
const std::vector<tl::core::PlayerClass>& Renderer::player_classes() {
    impl_->load_player_classes();
    return impl_->player_classes;
}

// PORT: the renderer's interface to `main.cpp`.
void Renderer::set_player_class(const std::string& name) { impl_->player_class = name; }

// PORT: the renderer's interface to `main.cpp`.
const std::string& Renderer::player_class() const { return impl_->player_class; }

// PORT: the character creation screen's model: the class's mesh and starting
// outfit stood on the backdrop, the camera kept (presentation).
bool Renderer::show_player_class(const std::string& name) {
    Impl& m = *impl_;
    m.load_player_classes();

    const tl::core::PlayerClass* chosen = nullptr;
    for (const tl::core::PlayerClass& entry : m.player_classes) {
        if (tl::core::AssetIndex::normalise(entry.name) == tl::core::AssetIndex::normalise(name)) {
            chosen = &entry;
            break;
        }
    }
    if (chosen == nullptr) {
        TL_LOG_WARN("no playable class called '%s'", name.c_str());
        return false;
    }
    if (m.player_class == chosen->name && m.subject != nullptr) return true;

    // The camera must survive the swap: show_mesh() reframes (frame_camera_on()) and the
    // creation screen keeps the backdrop's authored shot. Material scripts are reparsed:
    // another class may have taken the `Material_#1/<slot>` names.
    m.force_subject_materials = true;
    struct ForceOff {
        Renderer::Impl& m;
        ~ForceOff() { m.force_subject_materials = false; }
    } force_off{m};

    const bool keep = m.camera_node != nullptr && m.camera != nullptr;
    const Ogre::Vector3 eye = keep ? m.camera_node->getPosition() : Ogre::Vector3::ZERO;
    const Ogre::Quaternion aim =
        keep ? m.camera_node->getOrientation() : Ogre::Quaternion::IDENTITY;
    const Ogre::Real near_clip = keep ? m.camera->getNearClipDistance() : 0.0f;
    const Ogre::Real far_clip = keep ? m.camera->getFarClipDistance() : 0.0f;

    m.clear_subject();
    m.player_class = chosen->name;
    if (!show_mesh(chosen->mesh)) return false;
    if (keep) {
        m.camera_node->setPosition(eye);
        m.camera_node->setOrientation(aim);
        m.camera->setNearClipDistance(near_clip);
        m.camera->setFarClipDistance(far_clip);
    }

    // The class's starting outfit: textures on the base body (Renderer::wear_base_outfit()).
    wear_base_outfit();
    // `Idle` by name: link_animation() appends the extension itself.
    if (link_animation("Idle")) apply_animation("Idle", kMenuIdleTime);
    dress_character();

    report_stale_materials(m.scene, "a class swap");
    TL_LOG_INFO("showing the %s", chosen->display_name.c_str());
    return true;
}

// PORT: the creation screen's pet stood on the backdrop's `Point of Interest`
// marker (presentation).
bool Renderer::Impl::menu_pet_spot(Ogre::Vector3& where, Ogre::Quaternion& facing) {
    const tl::formats::CmpNode* marker = nullptr;
    for (const tl::formats::CmpNode& node : script_layout.nodes) {
        if (node.type != tl::formats::cmp_type::kMarker) continue;
        const tl::formats::CmpField* type =
            node.field(tl::formats::cmp_field::property_node::kType);
        if (type == nullptr) continue;
        if (type->as_text() != "Point of Interest") continue;
        marker = &node;
        break;
    }
    if (marker == nullptr || !marker->has_position()) return false;

    where = Ogre::Vector3(marker->position().x, marker->position().y, marker->position().z);
    // The marker's Y is a marker's Y, not a ground height - the same trap
    // `Player Start` sets, and the same answer.
    float surface = 0.0f;
    if (ground_under(Ogre::Vector3(where.x, where.y + 20.0f, where.z), surface)) {
        where.y = surface;
    }

    // Facing, the same way `stand_subject_at` reads it: the marker's first
    // vector is a direction to turn the mesh's own +Z onto, not a basis to
    // install. Flattened to the ground plane; these markers carry no pitch.
    facing = Ogre::Quaternion::IDENTITY;
    if (marker->vec3s.size() >= 2) {
        Ogre::Vector3 towards(marker->vec3s[1].x, 0.0f, marker->vec3s[1].z);
        if (!towards.isZeroLength()) {
            towards.normalise();
            facing = Ogre::Vector3::UNIT_Z.getRotationTo(towards);
        }
    }
    return true;
}

// PORT: the creation screen's pet shown, one at a time (presentation).
bool Renderer::place_menu_pet(const std::string& unit) {
    Impl& m = *impl_;

    // Only one at a time: the creation screen calls this again every time the
    // choice changes, and two pets standing on one marker is what leaving the
    // old one visible would give.
    for (auto& entry : m.menu_pets) {
        if (entry.second == nullptr) continue;
        if (Ogre::SceneNode* node = entry.second->getParentSceneNode()) {
            node->setVisible(entry.first == unit);
        }
    }
    if (m.menu_pets.count(unit) != 0) {
        TL_LOG_INFO("menu pet: %s (already placed)", unit.c_str());
        return true;
    }

    Ogre::Vector3 where;
    Ogre::Quaternion facing;
    if (!m.menu_pet_spot(where, facing)) {
        TL_LOG_INFO("no Point of Interest in this layout; no pet placed");
        return false;
    }

    Ogre::Entity* pet = m.spawn_unit(unit, where, /*owner=*/0);
    if (pet == nullptr) {
        TL_LOG_WARN("could not place '%s' at the Point of Interest", unit.c_str());
        return false;
    }
    m.menu_pets[unit] = pet;
    if (Ogre::SceneNode* node = pet->getParentSceneNode()) node->setOrientation(facing);
    TL_LOG_INFO("menu pet: %s at (%.2f, %.2f, %.2f)", unit.c_str(), where.x, where.y, where.z);
    return true;
}

// CGameClient::applyCharacterState: the subject is
// already the file's class; the state is the save loader's (`carried_from_file`). It
// stands at the level's start (CLevel +0xc4, facing +0xe8); the first pet at CLevel +0xf4
// facing +0x100 (the menu's pet marker). Not carried: the new pet's +0x280, alignment,
// +0x648 and name; +0xf8c = 3 is the menu's own (ContinueMenu::press).
void Renderer::apply_character_state(const tl::formats::CharacterFile& file) {
    Impl& m = *impl_;
    const CarriedPlayer carried = carried_from_file(file);
    restore_player_after_control(carried);
    for (auto& entry : m.menu_pets) {
        if (entry.second == nullptr) continue;
        if (Ogre::SceneNode* node = entry.second->getParentSceneNode()) node->setVisible(false);
    }
    if (!file.pets.empty() && !file.pets.front().name.empty()) {
        place_menu_pet(file.pets.front().name);
    } else {
        TL_LOG_INFO("menu: the save has no pet");
    }
    TL_LOG_INFO("menu: '%s' stands on the menu, level %d", file.player.name.c_str(),
                m.player_character.level());
}

bool Renderer::restart(const RendererConfig& config) {
    TL_LOG_INFO("restarting the renderer for a new scene");
    log_memory("before the restart");
    // By the steady clock rather than OGRE's timer, which goes with Root half
    // way through. Every level change pays this before its layout starts, and
    // it is the part load_layout()'s own figure does not include.
    using Clock = std::chrono::steady_clock;
    const Clock::time_point started = Clock::now();

    shutdown();
    const Clock::time_point down = Clock::now();

    // A fresh Impl, not the old one reset: no level-state field can be missed.
    impl_ = std::make_unique<Impl>();
    impl_->wire_bag_item_events();

    const bool ok = init(config);
    const auto ms = [](Clock::duration d) {
        return static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
    };
    TL_LOG_INFO("renderer restarted in %.0f ms (shutdown %.0f ms, init %.0f ms)",
                ms(Clock::now() - started), ms(down - started), ms(Clock::now() - down));
    log_memory("after the restart");
    if (!ok) TL_LOG_ERROR("the renderer did not come back up");
    return ok;
}

// PORT: the renderer's interface to `main.cpp`.
void Renderer::set_continue_available(bool available) { impl_->continue_available = available; }

MenuAction Renderer::run_main_menu(const std::string& dump_png, bool auto_new_game) {
    Impl& m = *impl_;
    if (!m.window) return MenuAction::Quit;

    // `CGameClient::updateMenu` ends every
    // front-end frame with `globallyDisableSkills(false)`; nothing in the front
    // end sets it.
    tl::core::globally_disable_skills(false);
    m.loading.hide();
    m.menu.set_continue_available(m.continue_available);
    if (!m.menu.create(kGameGroup, m.config.width, m.config.height)) {
        // A console has no command line and no second chance: a menu that could
        // not be built must not leave the player looking at a backdrop with no
        // way forward. Starting the game is the useful failure.
        TL_LOG_WARN("the main menu could not be built; starting a new game instead");
        return MenuAction::NewGame;
    }

    // The frame first, before anything has been pointed at or pressed, so it
    // shows the menu as it opens and means the same thing on both targets.
    if (!dump_png.empty()) {
        advance_scene(menu_dump_delay);
        dump_frame(dump_png);
    }

    // Asked for, rather than pressed. Before the headless check, so it works
    // in a window too - watching the transition happen is worth something that
    // a pair of PNGs is not.
    if (auto_new_game) {
        m.menu.destroy();
        TL_LOG_INFO("main menu: new game (asked for, not pressed)");
        return MenuAction::NewGame;
    }

    // Headless: there is nobody to press anything, and sitting in the loop
    // below would never finish. The dump above is the whole point of the run.
    if (m.config.headless) {
        m.menu.destroy();
        return MenuAction::Quit;
    }

    // The character stands still on the menu. The turntable is a backdrop's
    // flourish and the original's menu does not use it - the character plays its
    // idle and nothing turns.
    const float turntable = m.config.turntable_degrees_per_second;
    m.config.turntable_degrees_per_second = 0.0f;

    Ogre::Timer* timer = m.root->getTimer();
    unsigned long last = timer->getMilliseconds();
    MenuAction action = MenuAction::None;

    // Which list is currently up, so `Back` knows where to return to: the
    // Choose Level floor list goes back to its own biome list, not straight
    // to the main buttons the way every other `Back` in this menu does.
    enum class Screen { Main, Wardrobe, Bestiary, LevelBiomes, LevelFloors } screen = Screen::Main;

    // Each MainFloor::biome once, in list_main_floors() order, with its display name.
    auto biome_list = [&]() {
        std::vector<std::pair<std::string, std::string>> biomes;
        for (const MainFloor& f : list_main_floors()) {
            if (!biomes.empty() && biomes.back().first == f.biome) continue;
            biomes.emplace_back(f.biome, f.biome_label);
        }
        return biomes;
    };

    m.title_menu_running = true;
    while (!m.window->isClosed() && tl::core::should_keep_running()) {
        tl::core::pump_events();

        const unsigned long now = timer->getMilliseconds();
        const float delta = std::min(static_cast<float>(now - last) * 0.001f, 0.1f);
        last = now;

        const tl::core::InputState input = tl::core::poll_input();
        // Options, from the menu's own button, has the pad while it is up
        // and until it has finished closing (`CMainMenu::update` waits on
        // its +0x140 for that); the menu under it takes nothing.
        m.step_settings_menu();
        if (m.howto_menu.open) {
            // Help, likewise. No flag on the menu for it (function 0x62 sets
            // none): nothing moved the menu's focus.
            const bool start_edge = input.quit && !m.quit_held;
            const bool back_edge = input.inventory && !m.inventory_button_held;
            m.drive_howto_menu(input, delta, start_edge, back_edge);
            m.menu.hold(input);
            action = MenuAction::None;
        } else if (m.settings_menu.open || !m.settings_menu.finished) {
            if (m.settings_menu.open) m.drive_settings_menu(input);
            m.menu.hold(input);
            action = MenuAction::None;
        } else {
            action = m.menu.update(input);
        }
        if (action == MenuAction::Help) {
            TL_LOG_INFO("main menu: help");
            m.toggle_help();
            action = MenuAction::None;
        }
        if (action == MenuAction::Options) {
            // `CMainMenu::onClick` 0x5e: `toggleSettings`,
            // then +0x140 set. The menu stays drawn under it.
            TL_LOG_INFO("main menu: options");
            m.open_settings_menu();
            action = MenuAction::None;
        }
        m.confirm_held = input.confirm;
        m.skip_held = input.skip;
        m.quit_held = input.quit;
        m.inventory_button_held = input.inventory;

        // Wardrobe, Bestiary, ChooseLevel and Back are this engine's own menu screens,
        // handled here.
        if (action == MenuAction::Wardrobe) {
            open_wardrobe();
            screen = Screen::Wardrobe;
            action = MenuAction::None;
        } else if (action == MenuAction::Bestiary) {
            open_bestiary();
            screen = Screen::Bestiary;
            action = MenuAction::None;
        } else if (action == MenuAction::EditorStep) {
            if (screen == Screen::Bestiary) {
                step_bestiary(m.menu.editor_row(), m.menu.editor_step());
            } else {
                step_wardrobe(m.menu.editor_row(), m.menu.editor_step());
            }
            action = MenuAction::None;
        } else if (action == MenuAction::ChooseLevel) {
            m.menu.show_biome_list(biome_list());
            screen = Screen::LevelBiomes;
            action = MenuAction::None;
        } else if (action == MenuAction::ChooseLevelBiome) {
            const std::string biome = m.menu.chosen_biome();
            std::vector<std::pair<std::string, int>> entries;
            for (const MainFloor& f : list_main_floors()) {
                if (f.biome == biome) entries.emplace_back(f.label, f.floor);
            }
            m.menu.show_floor_list(entries);
            screen = Screen::LevelFloors;
            action = MenuAction::None;
        } else if (action == MenuAction::Back) {
            if (screen == Screen::Wardrobe) close_wardrobe();
            if (screen == Screen::Bestiary) close_bestiary();
            if (screen == Screen::LevelFloors) {
                m.menu.show_biome_list(biome_list());
                screen = Screen::LevelBiomes;
            } else {
                m.menu.show_main_buttons();
                screen = Screen::Main;
            }
            action = MenuAction::None;
        } else if (action == MenuAction::LoadDungeon) {
            // The menu is about to be destroyed below, which is where its own
            // copy of this lives.
            m.chosen_dungeon = m.menu.chosen_dungeon();
            m.chosen_dungeon_floor = m.menu.chosen_dungeon_floor();
        }

        // Rendered, not only stepped: OGRE's material controllers advance on frame events.
        if (screen == Screen::Wardrobe) steer_wardrobe(input, delta);
        if (screen == Screen::Bestiary) steer_bestiary(input, delta);
        m.advance(delta);
        if (!m.root->renderOneFrame()) break;
        m.save_scripted_shot();

        if (action != MenuAction::None) break;
    }

    m.title_menu_running = false;
    close_wardrobe();
    close_bestiary();
    m.config.turntable_degrees_per_second = turntable;
    m.menu.destroy();

    // The window closing, or the console's applet being taken away, is a quit.
    if (action == MenuAction::None) action = MenuAction::Quit;
    if (action == MenuAction::LoadDungeon) {
        TL_LOG_INFO("main menu: %s floor %d", m.chosen_dungeon.c_str(), m.chosen_dungeon_floor);
    } else {
        TL_LOG_INFO("main menu: %s", action == MenuAction::NewGame         ? "new game"
                                     : action == MenuAction::Continue      ? "continue"
                                     : action == MenuAction::LoadCharacter ? "load character"
                                                                           : "quit");
    }
    return action;
}

// PORT: the renderer's interface to `main.cpp`.
const std::string& Renderer::dungeon_choice() const { return impl_->chosen_dungeon; }
// PORT: the renderer's interface to `main.cpp`.
int Renderer::dungeon_choice_floor() const { return impl_->chosen_dungeon_floor; }
const std::string& Renderer::menu_biome_choice() const { return impl_->chosen_biome; }

Renderer::DungeonTransition Renderer::dungeon_transition() const {
    const Impl& m = *impl_;
    DungeonTransition t;
    t.pending = m.dungeon_transition_pending;
    if (!t.pending) return t;
    if (m.death_town_pending || m.portal_warp_pending) {
        // `warpLevels("Town", -99)`, from the death screen or a portal, and
        // `performWarp`'s choice of where that leads.
        t.portal_warp = true;
        tl::core::TravelRecord none;
        const tl::core::TownPortalWarp to = tl::core::town_portal_destination(
            m.config.travel != nullptr ? *m.config.travel : none, m.portal_level());
        t.back_to_portal = to.back_to_portal;
        if (to.dungeon == "TOWN") {
            t.return_to_town = true;
        } else {
            t.dungeon = to.dungeon;
            t.floor = to.depth + 1;
        }
        return t;
    }
    if (m.warp_pending) {
        // A Warper's request, from any level, or a waypoint menu row.
        t.waypoint = m.warp_waypoint;
        // `TOWN` names the town dungeon, whose depth 0 this engine loads as the
        // town layout.
        if (tl::core::upper_ascii(m.warp_dungeon) == "TOWN") {
            t.return_to_town = true;
            return t;
        }
        t.dungeon = m.warp_dungeon;
        t.floor = m.warp_floor;
        t.level_delta = m.warp_level_delta;
        t.level_absolute = m.warp_level_absolute;
        return t;
    }
    // The floor's `EXIT_NODE` Warper, walked into: the default delta of 1.
    t.level_delta = 1;
    const int next_floor = m.dungeon_active_floor + 1;
    if (next_floor > m.dungeon_active_floors_total) {
        t.return_to_town = true;
    } else {
        t.dungeon = m.dungeon_active_name;
        t.floor = next_floor;
    }
    return t;
}

// ---------------------------------------------------------------------------
// The level remembered and put back
// ---------------------------------------------------------------------------

// PORT: the automap's revealed cells into a level state, and back out of one.
void Renderer::Impl::view_store_automap(tl::formats::LevelSaveState& state) {
    automap.revealed_grid(state.automap_width, state.automap_height, state.automap);
}

void Renderer::Impl::view_restore_automap(const tl::formats::LevelSaveState& state) {
    automap.restore_revealed(state.automap_width, state.automap_height, state.automap);
}

// PORT: the renderer's interface to `main.cpp`.
const std::string& Renderer::level_dungeon() const { return impl_->level_dungeon; }
int Renderer::level_depth() const { return impl_->level_depth; }

// PORT: the renderer's interface to `main.cpp`.
void Renderer::store_level_state() { impl_->store_level_state(); }

// CGameClient::performWarp, the bookkeeping
// around the unload.
void Renderer::leave_level(const std::string& destination, int depth, bool return_to_town) {
    Impl& m = *impl_;
    m.store_level_state();
    tl::core::TravelRecord* travel = m.config.travel;
    const std::string to = tl::core::upper_ascii(destination);
    // the level left becomes the last dungeon unless the warp stays in
    // its dungeon or is the -99 one, or the template is `DONTSTORE`. The
    // destination is tracked either way.
    const bool leaves_dungeon = to != m.level_dungeon && !return_to_town;
    if (travel != nullptr && !m.level_dungeon.empty() && leaves_dungeon && !m.level_dont_store) {
        travel->last_dungeon = m.level_dungeon;
        travel->last_dungeon_depth = m.level_depth;
    }
    // A warp into another dungeon the player has no history of, and which is
    // `VOLATILE` (`CDungeon +0x88`) or is no dungeon the manager knows, gives
    // it a new salt - after `setCurrentDungeon` and
    // `srand(time(0))`, which every warp does.
    tl::core::Dungeon* target = loaded_dungeon_manager().dungeon_by_name(to);
    const bool new_salt = leaves_dungeon &&
                          !(travel != nullptr && tl::core::has_dungeon_history(*travel, to)) &&
                          (target == nullptr || target->def.is_volatile);
    tl::core::game_seed().current_dungeon = target;
    tl::core::seed_crt_from_time();
    if (new_salt && target != nullptr) {
        target->salt = tl::core::volatile_random().integer_between(0, 0x7fffffff);
    }
    if (travel == nullptr || m.level_dungeon.empty()) return;
    tl::core::update_dungeon_tracking(*travel, to, depth, m.player_character.level(),
                                      target != nullptr ? &target->salt : nullptr);
}

// `CGameClient::loadCharacter`: the game seed
// and the depth it was saved at (`CCharacterSaveState +0x60`, `+0x64`), the
// saved dungeon made current, and each tracked dungeon's salt put back on the
// dungeon.
void Renderer::load_character_seeds(const tl::formats::CharacterFile& file) {
    tl::core::DungeonManager& manager = loaded_dungeon_manager();
    tl::core::GameSeed& seed = tl::core::game_seed();
    seed.seed = file.player.game_seed;
    seed.saved_depth = file.player.depth;
    seed.current_dungeon = manager.dungeon_by_name(file.current_dungeon);
    for (const tl::formats::DungeonTrackerSave& tracker : file.trackers) {
        if (tl::core::Dungeon* dungeon = manager.dungeon_by_name(tracker.name)) {
            dungeon->salt = tracker.salt;
        }
    }
}

// `CGameClient::setGameState`, states 3 and
// 4: a new game takes a new game seed.
void Renderer::begin_new_game() {
    loaded_dungeon_manager();
    tl::core::game_seed().seed =
        tl::core::roll_game_seed(tl::core::kLevelSeedSetting, tl::core::volatile_random());
    // `setGameState`, after the seed: `resetQuestManager(false)`,
    // with no player made yet.
    Impl& m = *impl_;
    m.load_quest_book();
    m.quest_log.reset_quest_manager(true);
}

CarriedPlayer Renderer::carry_player() {
    Impl& m = *impl_;
    CarriedPlayer carried;
    carried.valid = true;
    carried.character = m.player_character;
    carried.inventory = m.player_inventory;
    carried.stash = m.player_stash;
    carried.quests = m.quest_log.carry();
    carried.health = m.player_health.current;
    carried.max_health = m.player_health.max;
    carried.dead = m.player_health.dead;
    carried.pet = m.pet;
    carried.pet_spells = m.pet.skills().spells;
    carried.pet_spell_levels = m.pet.skills().spell_levels;
    for (const Impl::MachineView& view : m.machines()) {
        for (const int64_t id : view.script->player_pets()) {
            if (view.script == &m.script && id == m.pet_node_id) continue;  // carried as `pet`
            CarriedPlayer::CarriedCompanion companion;
            companion.unit = view.script->unit_of(id);
            // `CGameClient::killPets`, from
            // `performWarp`: a pet that `DIES_ON_WARP` (the traps) is killed
            // before the warp, so it does not come along.
            if (!companion.unit.empty() && m.stats_for_unit(companion.unit).dies_on_warp) {
                TL_LOG_INFO("warp: the player's %s dies on the warp", companion.unit.c_str());
                continue;
            }
            float max = 0.0f;
            view.script->unit_health(id, companion.hp, max);
            view.script->unit_level(id, companion.level);
            view.script->unit_experience(id, companion.experience);
            if (!companion.unit.empty()) carried.companions.push_back(companion);
        }
    }
    carried.godspeed = m.god_mode;
    carried.has_skills = true;
    carried.skill_levels = m.player_skills.levels();
    carried.skill_slots.assign(m.player_skills.slots().begin(), m.player_skills.slots().end());
    carried.second_skill_slots.assign(m.player_skills.second_slots().begin(),
                                      m.player_skills.second_slots().end());
    carried.mapped_function_skills = m.player_skills.mapped_pairs();
    carried.skill_set = m.player_skills.skill_set();
    carried.spells = m.player_skills.spells;
    carried.spell_levels = m.player_skills.spell_levels;
    return carried;
}

namespace renderer_detail {
// `CQuestManager::save` and `CQuest::save`
// in this engine's terms: the completed
// names, then each quest in the log.
tl::formats::QuestManagerSave quests_to_save(
    const tl::core::QuestLog& log, const std::function<int64_t(const std::string&)>& unit_guid_of) {
    tl::formats::QuestManagerSave out;
    for (const std::string& name : log.state().names_with(tl::core::QuestStatus::Complete)) {
        out.completed.push_back(name);
    }
    // `CQuestManager::save` writes the held quests in their order.
    for (const tl::core::QuestEntry* held : log.held_entries()) {
        const tl::core::QuestEntry& entry = *held;
        if (entry.definition == nullptr || entry.status != tl::core::QuestStatus::Active) continue;
        tl::formats::QuestSave q;
        q.name = entry.definition->name;
        q.seed = static_cast<int32_t>(entry.seed);  // +0x1c
        q.depth = entry.depth;                      // +0x20
        q.level = entry.level;                      // +0x20c
        // The dialogs' +0x76: this engine keeps one flag for the `[INTRO]` lines.
        // +0x77, `giveOrRemoveItems` having run (`CQuestDialog::save`),
        // per line.
        const auto sections = {&entry.definition->intro, &entry.definition->ret,
                               &entry.definition->complete, &entry.definition->passive};
        size_t section_index = 0;
        for (const std::vector<tl::core::DialogLine>* lines : sections) {
            auto& section = q.dialogs[section_index];
            for (size_t i = 0; i < lines->size(); ++i) {
                const bool interacted = section_index == 0 && entry.dialog_interacted;
                const bool handed = log.items_handed(*entry.definition, (*lines)[i]);
                section.push_back({static_cast<uint8_t>(interacted ? 1 : 0),
                                   static_cast<uint8_t>(handed ? 1 : 0)});
            }
            ++section_index;
        }
        // CQuestUnitData::save: isComplete(false), +0xb9
        // placed, +0x38 rolled count, +0x3c so far, the id, the guid (+0x30, the unit's
        // UNIT_GUID via getBaseUnitDataGroup, -1 for none), the children.
        std::function<tl::formats::QuestUnitSave(const tl::core::QuestObjective&)> record =
            [&record, &unit_guid_of](const tl::core::QuestObjective& o) {
                tl::formats::QuestUnitSave unit;
                // DIVERGENCE (2026-10-11): the unit's own `+0x60`, where
                // the original writes `isComplete(false)`
                // (`quest_unit_done_from_save`).
                unit.complete = o.done ? 1 : 0;
                unit.bb9 = o.populated ? 1 : 0;
                unit.i38 = o.wanted;
                unit.i3c = o.have;
                unit.i10 = o.id;
                unit.i30 = o.resolved.empty() ? -1 : unit_guid_of(o.resolved);
                for (const tl::core::QuestObjective& child : o.children) {
                    unit.children.push_back(record(child));
                }
                return unit;
            };
        auto units = [&record](const std::vector<tl::core::QuestObjective>& objectives,
                               std::vector<std::vector<tl::formats::QuestUnitSave>>& groups) {
            for (const tl::core::QuestObjective& o : objectives) groups.push_back({record(o)});
        };
        units(entry.defeat, q.objectives[0]);
        units(entry.acquire, q.objectives[1]);
        units(entry.populate, q.objectives[2]);
        out.active.push_back(std::move(q));
    }
    return out;
}

}  // namespace renderer_detail
// PORT: the renderer's interface to `main.cpp`.
bool Renderer::save_character(const std::string& path, bool force) {
    return impl_->save_character(path, force);
}

namespace renderer_detail {
// The directory a save file sits in, with its separator.
// PORT: file I/O, the directory of a save file.
std::string save_directory_of(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

}  // namespace renderer_detail

void Renderer::Impl::load_shared_stash(const std::string& directory) {
    // `CSharedStash::loadSharedStash`:
    // nothing while the stash holds anything.
    tl::core::SharedStash* stash = config.shared_stash;
    if (stash == nullptr || !stash->begin_load()) return;
    const std::string path = directory + stash->file_name();
    std::FILE* in = std::fopen(path.c_str(), "rb");
    if (in == nullptr) return;
    std::vector<uint8_t> bytes;
    uint8_t buffer[4096];
    size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), in)) > 0) {
        bytes.insert(bytes.end(), buffer, buffer + got);
    }
    std::fclose(in);
    std::vector<tl::formats::ItemSaveState> saved;
    if (!tl::formats::read_shared_stash_file(bytes.data(), bytes.size(), saved)) {
        TL_LOG_WARN("shared stash: '%s' could not be read", path.c_str());
    }
    // Each item into its saved slot (`pickupEquipment(item, +0x58)`); one that
    // does not fit is destroyed.
    load_unit_types();
    size_t placed = 0;
    for (const tl::formats::ItemSaveState& item : saved) {
        const std::optional<tl::core::ItemStats> copy = item_from_save(item);
        if (!copy) continue;
        if (stash->inventory().pickup_into_slot(*copy, std::max(1, item.value), item.slot))
            ++placed;
    }
    TL_LOG_INFO("shared stash: %zu item(s) from '%s'", placed, path.c_str());
}

void Renderer::Impl::save_shared_stash(const std::string& directory) {
    // `CSharedStash::saveSharedStash`: every
    // item with its slot.
    const tl::core::SharedStash* stash = config.shared_stash;
    if (stash == nullptr) return;
    std::vector<tl::formats::ItemSaveState> saved;
    const std::vector<tl::core::InventorySlot>& slots = stash->inventory().slots();
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].count > 0) saved.push_back(item_save_state(slots[i], static_cast<int32_t>(i)));
    }
    const std::vector<uint8_t> bytes = tl::formats::write_shared_stash_file(saved);
    const std::string path = directory + stash->file_name();
    std::FILE* out = std::fopen(path.c_str(), "wb");
    if (out == nullptr) {
        TL_LOG_WARN("shared stash: could not write '%s'", path.c_str());
        return;
    }
    std::fwrite(bytes.data(), 1, bytes.size(), out);
    std::fclose(out);
}

void Renderer::load_shared_stash(const std::string& directory) {
    impl_->load_shared_stash(directory);
}

// CGameClient::saveCharacter
bool Renderer::Impl::save_character(const std::string& path, bool force) {
    Impl& m = *this;
    // With no player, nothing; unless forced, a dead one is not saved.
    if (!m.player_control || (m.player_health.dead && !force) || m.config.travel == nullptr ||
        path.empty()) {
        return false;
    }
    // `CSharedStash::saveSharedStash` comes
    // first, beside the character file.
    m.save_shared_stash(save_directory_of(path));
    // DIVERGENCE: `settle` runs `CLevel::updateLayouts` eleven times first; here, stored as is.
    m.store_level_state();
    const tl::core::TravelRecord& travel = *m.config.travel;

    tl::formats::CharacterFile file;
    file.class_name = m.player_class;
    file.difficulty = static_cast<uint32_t>(std::max(0, m.config.difficulty));
    file.hardcore = m.config.hardcore ? 1 : 0;
    // `saveCharacter`: `+0xa10`, then `+0xa14`.
    file.generation = static_cast<uint32_t>(m.player_character.generation());
    file.retired = m.player_character.retired() ? 1 : 0;

    // `CPlayer::fillSaveState`.
    tl::formats::CharacterSaveState& p = file.player;
    const tl::core::CharacterProgression::Saved sheet = m.player_character.saved();
    p.name = m.player_character.name();
    p.mods = m.config.mod_names;  // `CPlayer::fillSaveState`
    p.enabled = 1;
    p.alignment = 1;  // `setAlignment(1)`, the player's
    p.level = sheet.level;
    p.id4 = static_cast<int32_t>(sheet.experience);
    p.idc = static_cast<int32_t>(sheet.fame);
    p.id8 = sheet.fame_level;
    p.hp = m.player_health.current;
    // `+0x418`, the curve's maximum; `maxHP()` is worked out from it.
    p.base_max_hp = static_cast<int32_t>(m.player_character.base_max_hp());
    p.permanent_hp = sheet.permanent_hp;                          // +0x41c
    p.permanent_mana = static_cast<int32_t>(sheet.permanent_mp);  // (int)+0x440
    std::memcpy(&p.iec, &sheet.mp, sizeof(float));  // +0x438, a float copied as four bytes
    p.i164[0] = sheet.strength;
    p.i164[1] = sheet.dexterity;
    p.i164[2] = sheet.defense;
    p.i164[3] = sheet.magic;
    p.i164[4] = m.player_inventory.gold();   // +0x444
    p.i108[0] = sheet.unspent_stat_points;   // +0x45c
    p.i108[1] = sheet.unspent_skill_points;  // +0x460
    // `CSkillManager`'s known skills, their levels in the class file's order
    // (+0x1e8), and the four scroll spells (`+1000` -> +0x118 / +0x138).
    p.skill_levels = m.player_skills.levels();
    p.spells = m.player_skills.spells;
    p.spell_levels = m.player_skills.spell_levels;
    // `CPlayer +0x8b0` and `+0x900`: what the pad's function slots hold.
    file.skill_slots.assign(m.player_skills.slots().begin(), m.player_skills.slots().end());
    file.second_slots.assign(m.player_skills.second_slots().begin(),
                             m.player_skills.second_slots().end());
    // `+0x950` and `+0x9b0`, pair by pair.
    file.mapped_skills = m.player_skills.mapped_pairs();
    // CGameClient::saveCharacter: the 18
    // slots of `CPlayer +0x7dc`.
    tl::core::write_journal(m.player_character.journal(), file.journal);
    // The 34 context-tip flags of `CPlayer +0xa17`.
    tl::core::write_tips_seen(m.player_character.tips_seen(), file.flags);
    TL_LOG_INFO(
        "save: journal written (%d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, "
        "%d, %d)",
        static_cast<int32_t>(file.journal[0]), static_cast<int32_t>(file.journal[1]),
        static_cast<int32_t>(file.journal[2]), static_cast<int32_t>(file.journal[3]),
        static_cast<int32_t>(file.journal[4]), static_cast<int32_t>(file.journal[5]),
        static_cast<int32_t>(file.journal[6]), static_cast<int32_t>(file.journal[7]),
        static_cast<int32_t>(file.journal[8]), static_cast<int32_t>(file.journal[9]),
        static_cast<int32_t>(file.journal[10]), static_cast<int32_t>(file.journal[11]),
        static_cast<int32_t>(file.journal[12]), static_cast<int32_t>(file.journal[13]),
        static_cast<int32_t>(file.journal[14]), static_cast<int32_t>(file.journal[15]),
        static_cast<int32_t>(file.journal[16]), static_cast<int32_t>(file.journal[17]));
    p.clock = travel.play_time;
    p.depth = m.level_depth;
    p.deepest_depth = travel.deepest_depth;
    p.game_seed = tl::core::game_seed().seed;  // `CGameClient +0x1090`
    if (m.player_control) {
        const tl::core::Vec3 at = m.player.position();
        p.position = {at.x, at.y, at.z};
        p.orientation = orientation_bytes(m.player.facing());
    }
    // The inventory, one CItemSaveState per occupied slot:
    // +0x238 to +0x68 (the count); +0x58 the CInventory slot,
    // replayed through pickup_into_slot. +0x5c (is an equip location) is not trusted.
    const std::vector<tl::core::InventorySlot>& slots = m.player_inventory.slots();
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].count <= 0) continue;
        p.inventory.push_back(m.item_save_state(slots[i], static_cast<int32_t>(i)));
    }
    // X360 `CPlayer::fillSaveState`: after the character's own,
    // every item of the stash (`+0x9f4`) with its slot plus 1000.
    const std::vector<tl::core::InventorySlot>& stash_slots = m.player_stash.slots();
    for (size_t i = 0; i < stash_slots.size(); ++i) {
        if (stash_slots[i].count <= 0) continue;
        p.inventory.push_back(m.item_save_state(
            stash_slots[i], tl::core::kStashSaveSlotBase + static_cast<int32_t>(i)));
    }

    file.current_dungeon = m.level_dungeon;
    file.last_dungeon = travel.last_dungeon;
    file.last_dungeon_depth = travel.last_dungeon_depth;
    file.portal_open = travel.portal_open ? 1 : 0;
    file.portal_position = travel.portal_position;
    file.portal_dungeon = travel.portal_dungeon;
    file.portal_depth = travel.portal_depth;
    if (m.pet.alive()) {
        tl::formats::CharacterSaveState pet;
        // `CCharacter::fillSaveState`: the
        // unit it is now, and a fish's transformation - `+0x688` to +0x17c,
        // `+0x690` (the GUID of the unit it first was) to +0x28.
        pet.name = m.pet.unit();
        pet.transform_left = m.pet.transform_left();
        pet.transformed_from =
            m.pet.transformed_from().empty() ? -1 : m.unit_guid_of(m.pet.transformed_from());
        pet.hp = m.pet.health();
        // `CCharacter::fillSaveState`: level
        // `+0x100` to +0xd0, experience `+0x448` to +0xd4.
        pet.level = m.pet.level();
        pet.id4 = m.pet.experience();
        pet.i48 = 2;  // `+0x710`, which `loadLevel` sets to 2 on a pet
        pet.alignment = 1;
        pet.enabled = 1;
        // `CCharacter::fillSaveState`: the spell slots, names and levels.
        pet.spells = m.pet.skills().spells;
        pet.spell_levels = m.pet.skills().spell_levels;
        const std::vector<tl::core::InventorySlot>& pet_slots = m.pet.bag().slots();
        for (size_t i = 0; i < pet_slots.size(); ++i) {
            if (pet_slots[i].count <= 0) continue;
            pet.inventory.push_back(m.item_save_state(pet_slots[i], static_cast<int32_t>(i)));
        }
        file.pets.push_back(std::move(pet));
    }
    // The rest of `CPlayer +0x648`: a character `addPet` made one (Brink), which
    // `CCharacter::applySaveState` gives back to the player with `addPet`.
    for (const Impl::MachineView& view : m.machines()) {
        for (const int64_t id : view.script->player_pets()) {
            if (view.script == &m.script && id == m.pet_node_id) continue;  // saved above
            tl::formats::CharacterSaveState companion;
            companion.name = view.script->unit_of(id);
            if (companion.name.empty()) continue;
            float max = 0.0f;
            view.script->unit_health(id, companion.hp, max);
            view.script->unit_level(id, companion.level);
            view.script->unit_experience(id, companion.id4);
            companion.i48 = 2;
            companion.alignment = 1;
            companion.enabled = 1;
            file.pets.push_back(std::move(companion));
        }
    }
    file.levels = travel.levels;
    file.trackers = travel.trackers;
    file.quests =
        quests_to_save(m.quest_log, [&m](const std::string& unit) { return m.unit_guid_of(unit); });

    const std::vector<uint8_t> bytes = tl::formats::write_character_file(file);

    // `save.tmp` and `backup.tmp` beside the save; then remove(backup),
    // rename(save, backup), rename(tmp, save).
    const size_t slash = path.find_last_of("/\\");
    const std::string directory =
        slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
    const std::string temporary = directory + "save.tmp";
    const std::string backup = directory + "backup.tmp";
    std::FILE* out = std::fopen(temporary.c_str(), "wb");
    if (out == nullptr) {
        TL_LOG_WARN("Unable to save character to : %s", path.c_str());
        return false;
    }
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), out) == bytes.size();
    std::fclose(out);
    if (!written) {
        TL_LOG_WARN("Unable to save character to : %s", path.c_str());
        return false;
    }
    std::remove(backup.c_str());
    std::rename(path.c_str(), backup.c_str());
    std::rename(temporary.c_str(), path.c_str());
    TL_LOG_INFO("Saving character to :%s (%zu bytes, %zu level(s) remembered)", path.c_str(),
                bytes.size(), file.levels.size());
    return true;
}

CarriedPlayer Renderer::carried_from_file(const tl::formats::CharacterFile& file) {
    Impl& m = *impl_;
    CarriedPlayer carried;
    carried.valid = true;

    // A fresh sheet and bag for the class, then the saved values over them.
    m.load_stat_graphs();
    m.load_player_stats();
    const tl::formats::CharacterSaveState& p = file.player;
    tl::core::CharacterProgression::Saved sheet;
    sheet.level = p.level;
    sheet.experience = static_cast<uint64_t>(std::max(0, p.id4));
    sheet.fame = static_cast<uint64_t>(std::max(0, p.idc));
    sheet.fame_level = p.id8;
    sheet.hp = p.hp;
    std::memcpy(&sheet.mp, &p.iec, sizeof(float));
    sheet.strength = p.i164[0];
    sheet.dexterity = p.i164[1];
    sheet.defense = p.i164[2];
    sheet.magic = p.i164[3];
    sheet.unspent_stat_points = p.i108[0];
    sheet.unspent_skill_points = p.i108[1];
    sheet.permanent_hp = p.permanent_hp;
    sheet.permanent_mp = static_cast<float>(p.permanent_mana);
    // DIVERGENCE: an older port's file without skill levels is paid, once, the skill
    // points the curves would have given at the saved levels.
    if (p.skill_levels.empty()) {
        const tl::core::ClassProfile& profile = m.class_profile;
        int owed = 0;
        for (int level = 2; level <= p.level; ++level) {
            owed += static_cast<int>(std::ceil(
                m.stat_graphs.value(profile.skill_points_graph, static_cast<float>(level), 0.0f)));
        }
        for (int fame = 2; fame <= p.id8; ++fame) {
            owed += static_cast<int>(std::ceil(m.stat_graphs.value(
                profile.fame_skill_points_graph, static_cast<float>(p.level), 0.0f)));
        }
        sheet.unspent_skill_points = std::max(sheet.unspent_skill_points, owed);
        if (owed > 0) {
            TL_LOG_INFO("save: written before skills; %d skill point(s) paid for its levels", owed);
        }
    }
    m.player_character.apply_saved(sheet, m.stat_graphs);
    if (!p.name.empty()) m.player_character.set_name(p.name);
    // `CGameClient::loadCharacter`: the header's
    // generation and retired byte onto the player.
    m.player_character.set_generation(static_cast<int>(file.generation));
    m.player_character.set_retired(file.retired != 0);
    // CGameClient::loadCharacter: every slot through
    // `CPlayer::setJournalStatistic`.
    tl::core::read_journal(file.journal, m.player_character.journal());
    tl::core::read_tips_seen(file.flags, m.player_character.tips_seen());
    {
        size_t seen = 0;
        for (uint8_t flag : m.player_character.tips_seen()) seen += flag != 0 ? 1 : 0;
        TL_LOG_INFO("save: %zu context tip(s) already seen", seen);
    }
    TL_LOG_INFO(
        "save: journal read (%d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, "
        "%d)",
        static_cast<int32_t>(file.journal[0]), static_cast<int32_t>(file.journal[1]),
        static_cast<int32_t>(file.journal[2]), static_cast<int32_t>(file.journal[3]),
        static_cast<int32_t>(file.journal[4]), static_cast<int32_t>(file.journal[5]),
        static_cast<int32_t>(file.journal[6]), static_cast<int32_t>(file.journal[7]),
        static_cast<int32_t>(file.journal[8]), static_cast<int32_t>(file.journal[9]),
        static_cast<int32_t>(file.journal[10]), static_cast<int32_t>(file.journal[11]),
        static_cast<int32_t>(file.journal[12]), static_cast<int32_t>(file.journal[13]),
        static_cast<int32_t>(file.journal[14]), static_cast<int32_t>(file.journal[15]),
        static_cast<int32_t>(file.journal[16]), static_cast<int32_t>(file.journal[17]));
    carried.character = m.player_character;
    carried.has_skills = !p.skill_levels.empty();
    carried.skill_levels = p.skill_levels;
    carried.skill_slots = file.skill_slots;
    carried.second_skill_slots = file.second_slots;
    carried.mapped_function_skills = file.mapped_skills;
    carried.spells = p.spells;
    carried.spell_levels = p.spell_levels;
    if (!carried.has_skills) {
        // Nothing to put back but the mapping, which is empty too.
        carried.has_skills = true;
        carried.skill_levels.clear();
    }

    m.ensure_item_index();
    m.load_unit_types();
    tl::core::Inventory bag;
    tl::core::Inventory stash = tl::core::make_player_stash();
    for (const tl::formats::ItemSaveState& item : p.inventory) {
        const std::optional<tl::core::ItemStats> copy = m.item_from_save(item);
        if (!copy) continue;
        if (tl::core::is_stash_save_slot(item.slot)) {
            // X360 CPlayer::applySaveState: a slot above 999 is the stash's
            // slot - 1000; a copy that does not fit is destroyed. The
            // character's pass never puts it in the bag.
            stash.pickup_into_slot(*copy, std::max(1, item.value),
                                   item.slot - tl::core::kStashSaveSlotBase);
            continue;
        }
        const tl::core::ItemStats* stats = &*copy;
        // CInventory::pickupEquipment(CEquipment*, int, bool):
        // each item into its saved slot. A slot out of range, or an equip
        // location the item cannot occupy (older port saves' dense indices), falls back
        // to ordinary routing.
        const int count = std::max(1, item.value);
        bool trust_slot = item.slot >= 0 && static_cast<size_t>(item.slot) < bag.capacity();
        if (trust_slot && tl::core::Inventory::is_equip_location(static_cast<size_t>(item.slot))) {
            trust_slot = bag.can_equip_into_location(*stats, m.unit_types, item.slot);
        }
        // An occupied slot refuses a non-merging copy (`pickupEquipment`); a
        // save that aliased two items into one slot routes the second by pane.
        if (!trust_slot || !bag.pickup_into_slot(*stats, count, item.slot)) {
            bag.pickup(*stats, count, m.unit_types);
        }
    }
    bag.set_gold(p.i164[4]);
    carried.inventory = bag;
    carried.stash = stash;
    carried.health = p.hp;
    carried.max_health = static_cast<float>(p.base_max_hp);
    carried.dead = !(p.hp > 0.0f);

    // `CQuestManager::load`: the completed
    // names, then each quest rebuilt (`reinitializeQuest`) and its flags laid
    // back over it.
    m.load_quest_book();
    tl::core::QuestLog log;
    log.set_book(&m.quest_book);
    log.set_unit_namer(m.quest_unit_namer());
    {
        // A save is read before the character stands in a level, so no quest
        // unit is rolled here (`getSpawnClassUnitDataLookingFor`);
        // the record's guid puts it back.
        tl::core::QuestUnitWorld world = m.quest_unit_world();
        world.player_on_level = [] { return false; };
        log.set_unit_world(std::move(world));
    }
    log.set_random_names(&m.random_names);
    // `load` opens with `resetQuestManager(false)`.
    log.set_dungeon_source(m.quest_dungeon_source());
    log.reset_quest_manager(false);
    for (const std::string& name : file.quests.completed) log.state().complete(name);
    for (const tl::formats::QuestSave& q : file.quests.active) {
        const tl::core::QuestDefinition* definition = m.quest_book.find(q.name);
        if (definition == nullptr) continue;
        if (!log.accept(*definition, q.level, static_cast<uint32_t>(q.seed), m.stat_graphs,
                        std::max(0, q.depth))) {
            continue;
        }
        tl::core::QuestEntry* entry = log.find(q.name);
        if (entry == nullptr) continue;
        for (const auto& dialog : q.dialogs[0]) {
            if (dialog[0] != 0) entry->dialog_interacted = true;
        }
        // `CQuestDialog::load`: +0x77 from each line's second byte.
        const std::vector<tl::core::DialogLine>* lines_of[] = {
            &definition->intro, &definition->ret, &definition->complete, &definition->passive};
        for (size_t s = 0; s < 4; ++s) {
            for (size_t i = 0; i < q.dialogs[s].size() && i < lines_of[s]->size(); ++i) {
                if (q.dialogs[s][i][1] != 0) log.set_items_handed(*definition, (*lines_of[s])[i]);
            }
        }
        // `CQuestUnitData::load`: +0x60 from
        // the first byte, +0xb9, the counts, the unit's guid; the children in
        // order when the record holds no more of them than the objective has.
        std::function<void(const tl::formats::QuestUnitSave&, tl::core::QuestObjective&)> lay_unit =
            [&lay_unit, &m](const tl::formats::QuestUnitSave& unit, tl::core::QuestObjective& o) {
                o.populated = unit.bb9 != 0;
                o.wanted = unit.i38;
                o.have = unit.i3c;
                o.done = tl::core::quest_unit_done_from_save(unit.complete != 0, o.wanted, o.have);
                o.id = unit.i10;  // +0x10, what the quest's placed units carry
                // +0x30: the unit `reinitialize` could not roll with no level
                // to roll for is the one the guid names, as the first event to
                // find `+0x20` empty would look it up (`getBaseUnitDataGroup`
                // `getUnitDataByGuid`).
                if (o.resolved.empty() && unit.i30 != -1) o.resolved = m.unit_of_guid(unit.i30);
                if (unit.children.size() > o.children.size()) return;
                for (size_t c = 0; c < unit.children.size(); ++c) {
                    lay_unit(unit.children[c], o.children[c]);
                }
            };
        auto lay = [&lay_unit](const std::vector<std::vector<tl::formats::QuestUnitSave>>& groups,
                               std::vector<tl::core::QuestObjective>& objectives) {
            for (size_t i = 0; i < groups.size() && i < objectives.size(); ++i) {
                if (groups[i].empty()) continue;
                lay_unit(groups[i][0], objectives[i]);
            }
        };
        lay(q.objectives[0], entry->defeat);
        lay(q.objectives[1], entry->acquire);
        lay(q.objectives[2], entry->populate);
        log.name_entry(*entry);
    }
    carried.quests = log.carry();

    // The animal is the first `UNITTYPE:PET` unit; every other pet is a
    // character the player took on (`addPet`), brought back beside the player.
    for (const tl::formats::CharacterSaveState& pet : file.pets) {
        m.index_unit(pet.name);
        const bool animal = tl::core::upper_ascii(m.stats_for_unit(pet.name).unittype) == "PET";
        if (animal && carried.saved_pet_unit.empty()) {
            carried.saved_pet_unit = pet.name;
            // `CCharacter::applySaveState`:
            // `+0x690` from the state's +0x28 and `+0x688` from its
            // +0x17c - the pet goes on being what a fish made it.
            carried.pet.set_transformation(
                pet.transform_left,
                pet.transformed_from != -1 ? m.unit_of_guid(pet.transformed_from) : std::string());
            // CCharacter::applySaveState on the pet: its
            // level and experience. DIVERGENCE: an older file without the pet's level gives
            // it its master's.
            if (0 < pet.level) {
                carried.pet.set_progress(pet.level, std::max(0, pet.id4));
            } else {
                carried.pet.set_progress(p.level, std::max(0, p.id4));
            }
            // Its spells,
            // learned again on arrival, and its bag, each item into its slot.
            carried.pet_spells = pet.spells;
            carried.pet_spell_levels = pet.spell_levels;
            carried.has_pet_bag = true;
            carried.pet_bag.set_players_pet(true);
            for (const tl::formats::ItemSaveState& item : pet.inventory) {
                const std::optional<tl::core::ItemStats> copy = m.item_from_save(item);
                if (!copy) continue;
                const int count = std::max(1, item.value);
                bool trust_slot =
                    item.slot >= 0 && static_cast<size_t>(item.slot) < carried.pet_bag.capacity();
                if (trust_slot &&
                    tl::core::Inventory::is_equip_location(static_cast<size_t>(item.slot))) {
                    trust_slot =
                        carried.pet_bag.can_equip_into_location(*copy, m.unit_types, item.slot);
                }
                if (!trust_slot || !carried.pet_bag.pickup_into_slot(*copy, count, item.slot)) {
                    carried.pet_bag.pickup(*copy, count, m.unit_types);
                }
            }
            TL_LOG_INFO("save: the pet %s, %zu item(s), spells '%s', '%s'", pet.name.c_str(),
                        pet.inventory.size(), pet.spells[0].c_str(), pet.spells[1].c_str());
            continue;
        }
        CarriedPlayer::CarriedCompanion companion;
        companion.unit = pet.name;
        companion.hp = pet.hp;
        companion.level = pet.level;
        companion.experience = std::max(0, pet.id4);
        if (!companion.unit.empty()) carried.companions.push_back(companion);
    }
    TL_LOG_INFO("save: %s, level %d, %zu item(s), %zu quest(s), %zu level(s) remembered",
                file.class_name.c_str(), sheet.level, carried.inventory.size(),
                carried.quests.entries.size(), file.levels.size());
    return carried;
}

// PORT: the port's level change rebuilds the renderer's level and hands the
// player over (`CarriedPlayer`); the console keeps one `CPlayer` across a warp.
void Renderer::restore_player_before_level(const CarriedPlayer& carried) {
    Impl& m = *impl_;
    if (!carried.valid) return;
    m.load_quest_book();
    m.quest_log.restore(carried.quests);
    m.arriving_player_level = static_cast<int32_t>(carried.character.level());
    // `place_pet` keeps the stance, the bag and the health of an animal that is
    // already alive, the unit it is now and what a fish's transformation has
    // left.
    m.pet = carried.pet;
    // The carried book points into the skill files of the renderer that was
    // left, and the manager's effects into its tables: both are rebuilt here
    // (the spells by name once the skill files are read, the effects not kept,
    // as the player's are not).
    m.pet.skills() = tl::core::SkillBook{};
    m.pet.reset_effects();
    if (carried.has_pet_bag) m.pet.bag() = carried.pet_bag;
    m.pet.bag().set_players_pet(true);
    if (carried.pet.alive()) {
        m.config.pet = carried.pet.unit();
    } else if (!carried.saved_pet_unit.empty()) {
        m.config.pet = carried.saved_pet_unit;
    }
    TL_LOG_INFO("player: %zu quest(s) in the log carried across", m.quest_log.entries().size());
}

void Renderer::restore_player_after_control(const CarriedPlayer& carried) {
    Impl& m = *impl_;
    if (!carried.valid) return;
    m.player_character = carried.character;
    // The character that arrived is not a new one: what `firstTimeSetup`
    // queued for the sheet it replaces (the welcome) goes with that sheet.
    if (m.tip_queue.size() != 0) {
        TL_LOG_INFO("tips: %zu queued for the new sheet dropped, a carried character arrived",
                    m.tip_queue.size());
    }
    m.tip_queue.clear();
    m.player_inventory = carried.inventory;
    m.player_stash = carried.stash;
    // The carried bags point at the old renderer's tables; recalculate before anything
    // reads them.
    m.refresh_inventory_effects();
    // What the carried bag holds and wears, on the new level's model: the hands too, not
    // only the wardrobe, or the unit file's starting weapon drawn on arrival stays.
    m.sync_equipped_weapon();
    // The pet's spells, by name, into the book its new renderer's files fill.
    m.relearn_pet_spells(carried.pet_spells, carried.pet_spell_levels);
    if (carried.has_skills) {
        m.player_skills.set_levels(carried.skill_levels);
        // `CCharacter::applySaveState`: each
        // saved spell is learned again (`learnSpell`),
        // which adds its skill to the manager.
        m.player_skills.clear_spells();
        for (size_t i = 0; i < carried.spells.size(); ++i) {
            const std::string& name = carried.spells[i];
            if (name.empty()) continue;
            const auto file = m.player_skill_files.find(tl::core::upper_ascii(name));
            m.player_skills.learn_spell(
                name, carried.spell_levels[i],
                file != m.player_skill_files.end() ? &file->second->def : nullptr);
        }
        m.player_skills.set_slots(carried.skill_slots, carried.second_skill_slots);
        m.player_skills.set_mapped_pairs(carried.mapped_function_skills);
        // DIVERGENCE: the port's saves from before 2026-09-23 wrote the item
        // slots empty, and on the Xbox slots 0 and 1 (the bumpers) can hold
        // nothing else, so an empty pair is given `unitInit`'s defaults back.
        if (m.player_skills.second_slots()[0] == -1 && m.player_skills.slots()[0] == -1 &&
            m.player_skills.second_slots()[1] == -1 && m.player_skills.slots()[1] == -1) {
            m.bind_default_item_slots();
        }
        m.player_skills.set_skill_set(carried.skill_set);
    }
    m.character_level = m.player_character.level();
    m.player_health.max = carried.max_health;
    m.player_health.current = carried.health;
    m.player_health.dead = carried.dead;
    m.refresh_player_maxima();
    if (carried.godspeed) m.set_godspeed(true);
    m.update_menu();
    m.refresh_game_hud();
    TL_LOG_INFO(
        "player: level %d, %.0f of %.0f health (permanent +%d), %zu item(s), %d gold "
        "carried across",
        m.player_character.level(), static_cast<double>(carried.health),
        static_cast<double>(m.player_health.max), m.player_character.permanent_hp(),
        m.player_inventory.size(), m.player_inventory.gold());
}

// CLevel::placePlayerAtWarpToDungeonFloor
bool Renderer::place_player_at_warp(const std::string& previous_dungeon, int previous_depth,
                                    const std::string& warp_name) {
    Impl& m = *impl_;
    if (!m.player_control || m.subject_node == nullptr) return false;
    const std::string previous = tl::core::upper_ascii(previous_dungeon);
    int32_t back = 0;      // 0x2c(%rsp)
    int32_t absolute = 0;  // 0x24(%rsp)
    if (previous == m.level_dungeon) {
        back = previous_depth - m.level_depth;
        absolute = previous_depth;
    }

    struct Candidate {
        Ogre::Vector3 position;
        Ogre::Vector3 forward;
        std::string dungeon;
        std::string warp_name;
        int32_t delta = 1;
        int32_t absolute = 0;
    };
    std::vector<Candidate> candidates;
    for (const Impl::MachineView& view : m.machines()) {
        for (const tl::core::LevelScript::WarperRecord& w : view.script->warpers()) {
            Candidate c;
            c.position = Ogre::Vector3(w.position.x, w.position.y, w.position.z);
            c.forward = Ogre::Vector3(w.forward.x, w.forward.y, w.forward.z);
            c.dungeon = w.dungeon;
            c.warp_name = w.warp_name;
            c.delta = w.level_delta;
            c.absolute = w.level_absolute;
            candidates.push_back(std::move(c));
        }
    }
    // DIVERGENCE: a generated floor's `ENTRANCE_NODE` and `EXIT_NODE` Warpers
    // (`LEVEL DELTA` -1 and the default 1, no name) are walked into rather than
    // run as script nodes; they are read from their layouts for where they
    // stand and which way they face.
    if (m.dungeon_active) {
        Candidate entrance;
        entrance.position = to_ogre(m.dungeon_entrance_warp_at);
        entrance.forward = to_ogre(m.dungeon_entrance_warp_forward);
        entrance.delta = -1;
        candidates.push_back(entrance);
        Candidate exit;
        exit.position = to_ogre(m.dungeon_exit_warp_at);
        exit.forward = to_ogre(m.dungeon_exit_warp_forward);
        exit.delta = 1;
        candidates.push_back(exit);
    }

    for (const Candidate& w : candidates) {
        bool match = false;
        if (!w.warp_name.empty() && !previous.empty() &&
            tl::core::upper_ascii(warp_name) == tl::core::upper_ascii(w.warp_name)) {
            match = true;
        } else if (tl::core::upper_ascii(w.dungeon) == previous ||
                   (previous == "MAIN" && w.dungeon.empty())) {
            if (w.delta == 0) {
                match = w.absolute == absolute || (w.absolute == 0 && back == 0);
            } else {
                match = w.absolute == 0 && back == w.delta;
            }
        }
        if (!match) continue;

        Ogre::Vector3 at = w.position;
        // `CCharacter::dropToGround(4)`.
        float surface = 0.0f;
        if (!m.collision.empty() && m.collision.ground_under(at.x, at.z, at.y + 4.0f, surface)) {
            at.y = surface;
        }
        const float yaw = std::atan2(w.forward.x, w.forward.z);
        m.player.place({at.x, at.y, at.z}, yaw);
        m.step_player(0.0f, tl::core::InputState{});
        for (const Impl::MachineView& view : m.machines()) {
            view.script->arrive_at(tl::core::Vec3{at.x, at.y, at.z});
        }
        TL_LOG_INFO("player: arrived at the warper to '%s' (%.2f, %.2f, %.2f)", w.dungeon.c_str(),
                    at.x, at.y, at.z);
        return true;
    }
    return false;
}

// PORT: the renderer's interface to `main.cpp`.
bool Renderer::quit_to_menu_requested() const { return impl_->quit_requested; }

// PORT: the renderer's interface to `main.cpp`.
bool Renderer::retirement_requested() const { return impl_->retirement_requested; }

bool Renderer::window_open() const {
    return impl_->window != nullptr && !impl_->window->isClosed();
}

tl::core::HeirloomHandOff Renderer::take_heirloom() {
    tl::core::HeirloomHandOff out = std::move(impl_->heirloom_handoff);
    impl_->heirloom_handoff = tl::core::HeirloomHandOff{};
    return out;
}

void Renderer::receive_heirloom(const tl::core::HeirloomHandOff& handoff) {
    Impl& m = *impl_;
    m.load_stat_graphs();
    m.load_unit_types();
    // `+0xa10` = `+0x3908`; the fame figure is taken now and the
    // generation cleared.
    m.player_character.set_generation(handoff.generation);
    const int fame = tl::core::heirloom_fame(handoff.generation, m.stat_graphs);
    // pickupEquipment(+0x3900, true). DIVERGENCE: an item that would not fit
    // goes down at the player's feet.
    if (handoff.item) {
        if (m.player_inventory.pickup(*handoff.item, 1, m.unit_types) < 0) {
            m.drop_item_near_player(*handoff.item, 1);
        }
        m.player_inventory.calculate_effect_values();
        m.update_menu();
    }
    // `awardFame(level, player, fame, false)` once the level is up.
    if (fame != 0 && m.award_player_fame(fame)) {
        m.play_voice(tl::core::SoundSlot::VoiceFameUp);
    }
    TL_LOG_INFO("retire: new character, generation %d, %s, %d fame (fame level %d)",
                handoff.generation,
                handoff.item ? ("heirloom '" + handoff.item->name + "'").c_str() : "no heirloom",
                fame, m.player_character.fame_level());
    m.refresh_game_hud();
}

bool Renderer::dump_level_list(const std::string& png_path) {
    Impl& m = *impl_;
    if (!m.window) return false;
    m.loading.hide();
    if (!m.menu.create(kGameGroup, m.config.width, m.config.height)) {
        TL_LOG_WARN("the main menu could not be built; nothing to dump");
        return false;
    }
    // Each `MainFloor::biome` once, in the order `list_main_floors()` first
    // meets it - the same reduction `run_main_menu()`'s own `biome_list()`
    // lambda does for the live screen.
    std::vector<std::pair<std::string, std::string>> biomes;
    for (const MainFloor& f : list_main_floors()) {
        if (!biomes.empty() && biomes.back().first == f.biome) continue;
        biomes.emplace_back(f.biome, f.biome_label);
    }
    m.menu.show_biome_list(biomes);
    const bool ok = dump_frame(png_path);
    m.menu.destroy();
    return ok;
}

void Renderer::creation_names(std::string& player_name, std::string& pet_name) const {
    player_name = impl_->created_player_name;
    pet_name = impl_->created_pet_name;
}

MenuAction Renderer::run_character_create(const std::string& dump_png, std::string& chosen,
                                          std::string& pet, const std::string& preselect,
                                          const std::vector<std::string>& cycle) {
    Impl& m = *impl_;
    chosen.clear();
    pet.clear();
    if (!m.window) return MenuAction::Quit;

    if (!m.create_screen.create(kGameGroup, m.config.width, m.config.height, player_classes())) {
        // Same failure as the main menu's: a console has no command line, so a
        // screen that could not be built must not leave the player stuck.
        // Starting the game as whatever the subject already is beats stopping.
        TL_LOG_WARN("character creation could not be built; starting with the default class");
        return MenuAction::NewGame;
    }

    // The class the screen opens on, standing on the backdrop's Player Start.
    // Swapped rather than restarted, which is why `clear_subject()` exists.
    if (!preselect.empty()) m.create_screen.select(preselect);
    chosen = m.create_screen.chosen();
    if (!chosen.empty()) show_player_class(chosen);

    // Before anything has been pointed at, so it means the same on both targets.
    if (!dump_png.empty()) dump_frame(dump_png);

    // Headless: nobody can press OK. The screen's default class is the Destroyer, so a
    // run with no `--class` starts as that.
    if (m.config.headless) {
        // Scroll the icons as the interactive loop does - swap, then advance and render -
        // so a fault in the frame after a swap shows.
        for (const std::string& name : cycle) {
            // A pet name in the cycle chooses the pet; anything else is a class.
            if (name == "Dog" || name == "Cat" || name == "Chakawary") {
                m.create_screen.select_pet(name);
                if (m.create_screen.take_pet_changed()) {
                    TL_LOG_INFO("character creation: pet %s", name.c_str());
                    place_menu_pet(name);
                    for (int frame = 0; frame < 3; ++frame) {
                        m.advance(1.0f / 60.0f);
                        if (m.root != nullptr) m.root->renderOneFrame();
                    }
                }
                continue;
            }
            m.create_screen.select(name);
            if (!m.create_screen.take_selection_changed()) continue;
            chosen = m.create_screen.chosen();
            TL_LOG_INFO("character creation: selecting %s", chosen.c_str());
            show_player_class(chosen);
            for (int frame = 0; frame < 3; ++frame) {
                m.advance(1.0f / 60.0f);
                if (m.root != nullptr) m.root->renderOneFrame();
            }
        }
        if (!dump_png.empty() && !cycle.empty()) dump_frame(dump_png);

        pet = m.create_screen.pet();
        m.created_player_name = m.create_screen.player_name();
        m.created_pet_name = m.create_screen.pet_name();
        m.create_screen.destroy();
        TL_LOG_INFO("character creation: headless, taking %s", chosen.c_str());
        return MenuAction::NewGame;
    }

    // The character stands still and plays its idle, the same as on the menu.
    const float turntable = m.config.turntable_degrees_per_second;
    m.config.turntable_degrees_per_second = 0.0f;

    Ogre::Timer* timer = m.root->getTimer();
    unsigned long last = timer->getMilliseconds();
    MenuAction action = MenuAction::None;

    while (!m.window->isClosed() && tl::core::should_keep_running()) {
        tl::core::pump_events();

        const unsigned long now = timer->getMilliseconds();
        const float delta = std::min(static_cast<float>(now - last) * 0.001f, 0.1f);
        last = now;

        const tl::core::InputState input = tl::core::poll_input();
        action = m.create_screen.update(input);

        // A different class means a different body on the backdrop. Done here
        // rather than inside the screen, which knows nothing about the scene.
        if (m.create_screen.take_selection_changed()) {
            chosen = m.create_screen.chosen();
            show_player_class(chosen);
        }
        // The pet is a second model on the same backdrop, and choosing one has
        // to be visible or the buttons do nothing - which is exactly how it
        // read on the console, where they could not even be reached.
        if (m.create_screen.take_pet_changed()) {
            place_menu_pet(m.create_screen.pet());
        }

        m.advance(delta);
        if (!m.root->renderOneFrame()) break;
        m.save_scripted_shot();

        if (action != MenuAction::None) break;
    }

    m.config.turntable_degrees_per_second = turntable;
    chosen = m.create_screen.chosen();
    pet = m.create_screen.pet();
    m.created_player_name = m.create_screen.player_name();
    m.created_pet_name = m.create_screen.pet_name();
    m.create_screen.destroy();

    if (action == MenuAction::None) action = MenuAction::Quit;
    TL_LOG_INFO("character creation: %s (%s '%s', %s '%s')",
                action == MenuAction::NewGame ? "start" : "back", chosen.c_str(),
                m.created_player_name.c_str(), pet.c_str(), m.created_pet_name.c_str());
    return action;
}

MenuAction Renderer::run_difficulty_menu(tl::core::DifficultyMenu& state,
                                         tl::core::DifficultyClick& click,
                                         const std::string& dump_png) {
    Impl& m = *impl_;
    click = tl::core::DifficultyClick{};
    if (!m.window) return MenuAction::Quit;

    m.loading.hide();
    m.menu.set_continue_available(false);
    if (!m.menu.create(kGameGroup, m.config.width, m.config.height)) {
        // As the two screens before it: a screen that cannot be built must not
        // leave a console with no way forward.
        TL_LOG_WARN("the difficulty screen could not be built; starting at the configured one");
        return MenuAction::NewGame;
    }
    m.menu.show_difficulty(state);

    if (!dump_png.empty()) dump_frame(dump_png);
    if (m.config.headless) {
        state.set_open(false);
        m.menu.destroy();
        return MenuAction::NewGame;
    }

    const float turntable = m.config.turntable_degrees_per_second;
    m.config.turntable_degrees_per_second = 0.0f;

    Ogre::Timer* timer = m.root->getTimer();
    unsigned long last = timer->getMilliseconds();
    MenuAction action = MenuAction::None;

    while (!m.window->isClosed() && tl::core::should_keep_running()) {
        tl::core::pump_events();

        const unsigned long now = timer->getMilliseconds();
        const float delta = std::min(static_cast<float>(now - last) * 0.001f, 0.1f);
        last = now;

        action = m.menu.update(tl::core::poll_input());

        m.advance(delta);
        if (!m.root->renderOneFrame()) break;
        m.save_scripted_shot();

        if (action != MenuAction::None) break;
    }

    m.config.turntable_degrees_per_second = turntable;
    click = m.menu.difficulty_click();
    state.set_open(false);
    m.menu.destroy();

    if (action == MenuAction::None) action = MenuAction::Quit;
    if (action == MenuAction::NewGame) {
        TL_LOG_INFO("difficulty screen: difficulty %d%s", click.difficulty,
                    click.hardcore ? ", hardcore" : "");
    } else {
        TL_LOG_INFO("difficulty screen: %s", action == MenuAction::Back ? "back" : "quit");
    }
    return action;
}

// The Load Character screen as CContinueGameMenu leaves it: updateCharacterList,
// for the highlight, CContinueGameMenu::update
// for the name under the list.
void Renderer::Impl::show_load_character(const tl::core::ContinueMenu& state,
                                         const std::string& shown_file) {
    LayoutScreen& screen = character_load_screen;
    if (!screen.ready()) return;
    const auto rows = state.rows();
    const int lit = state.highlight_row();
    for (int row = 0; row < tl::core::kContinueRows; ++row) {
        const std::string number = std::to_string(row + 1);
        const tl::core::ContinueRow& text = rows[static_cast<size_t>(row)];
        screen.set_hidden("Player" + number, !text.shown);
        screen.set_caption("Player" + number + "Name", text.name);
        float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
        tl::formats::parse_colour(text.name_colour, r, g, b, a);
        screen.set_text_colour("Player" + number + "Name", Ogre::ColourValue(r, g, b, a));
        screen.set_caption("Player" + number + "Desc", text.description);
        screen.set_hidden("PlayerHighlight" + number, row != lit);
    }
    screen.set_hidden("ScrollUp", !state.scroll_up_shown());
    screen.set_hidden("ScrollDown", !state.scroll_down_shown());
    screen.set_hidden("highlight", !state.highlight_shown());
    // The highlight's corner less the first row's (`createMenus`)
    // plus the selected row's; left where it was off the five.
    float row_x = 0.0f, row_y = 0.0f, first_x = 0.0f, first_y = 0.0f;
    if (lit >= 0 && screen.offset("Player1", first_x, first_y) &&
        screen.offset("Player" + std::to_string(lit + 1), row_x, row_y)) {
        screen.set_offset("highlight", load_highlight_x - first_x + row_x,
                          load_highlight_y - first_y + row_y);
    }
    // `update`: the name of the player behind the menu, nothing with no file.
    std::string shown_name;
    if (state.count() != 0) {
        for (const tl::core::ContinueEntry& entry : state.entries()) {
            if (entry.path == shown_file) shown_name = entry.name;
        }
    }
    screen.set_caption("CharacterName", shown_name);
    screen.set_hidden("DeleteConfirm", !state.delete_confirm_shown());
    screen.set_hidden("DeadConfirm", !state.dead_confirm_shown());
    screen.set_hidden("CharacterModsWarning", !state.mods_warning());
    screen.refresh();
}

MenuAction Renderer::run_load_character(tl::core::ContinueMenu& state,
                                        const LoadCharacterHost& host,
                                        const std::string& shown_file,
                                        tl::core::ContinuePress& last, const std::string& dump_png,
                                        const std::string& keys) {
    Impl& m = *impl_;
    last = tl::core::ContinuePress{};
    if (!m.window) return MenuAction::Quit;

    m.loading.hide();
    LayoutScreen& screen = m.character_load_screen;
    if (!screen.ready()) {
        // Under the fishing ring (560), over the HUD. DIVERGENCE: with the PC's mods warning.
        screen.create_export(kGameGroup, m.config.width, m.config.height,
                             "media/UI/characterload.uilayout", "tl/characterload", 520,
                             {"CharacterModsWarning"});
        if (screen.ready()) {
            screen.offset("highlight", m.load_highlight_x, m.load_highlight_y);
            // activates the highlight each time it is placed, which
            // brings it in front of the rows the layout lists after it.
            screen.raise("highlight");
        }
    }
    if (!screen.ready()) {
        TL_LOG_WARN("the Load Character screen could not be built; back to the main menu");
        state.close();
        return MenuAction::Back;
    }

    // The game's mods, which `selectCharacter` holds a save's against (none on the console).
    const std::vector<std::string>& mods = m.config.mod_names;
    MenuAction action = MenuAction::None;
    // One button, as `vfunc_12` takes it, and what it led to.
    auto press = [&](tl::core::PadButton button, bool bumpers) {
        tl::core::ContinuePress did = state.press(button, bumpers, /*loading=*/false, mods);
        if (did.delete_file) {
            TL_LOG_INFO("load character: deleting '%s'", did.delete_path.c_str());
            if (host.remove) host.remove(did.delete_path);
            const tl::core::ContinuePress after = state.finish_delete(
                host.list ? host.list() : std::vector<tl::core::ContinueEntry>(), mods);
            did.closed = after.closed;
            did.request = after.request;
            did.apply_character = after.apply_character;
        }
        if (did.play_error) m.play_ui(tl::core::InterfaceSound::Error);
        last = did;
        if (did.closed) {
            action = did.request.state == 2 ? MenuAction::Continue : MenuAction::Back;
        } else if (did.apply_character && state.current_file() != shown_file) {
            action = MenuAction::LoadCharacter;
        }
    };

    for (char key : keys) {
        if (action != MenuAction::None) break;
        switch (key) {
            case 'u': press(tl::core::PadButton::DPadUp, false); break;
            case 'd': press(tl::core::PadButton::DPadDown, false); break;
            case 'a': press(tl::core::PadButton::A, false); break;
            case 'b': press(tl::core::PadButton::B, false); break;
            case 'x': press(tl::core::PadButton::X, false); break;
            case 'y': press(tl::core::PadButton::Y, false); break;
            default: break;
        }
    }

    m.show_load_character(state, shown_file);
    screen.show();
    if (!dump_png.empty()) dump_frame(dump_png);
    if (m.config.headless) {
        screen.hide();
        if (action == MenuAction::None) {
            state.close();
            action = MenuAction::Back;
        }
        return action;
    }

    const float turntable = m.config.turntable_degrees_per_second;
    m.config.turntable_degrees_per_second = 0.0f;
    Ogre::Timer* timer = m.root->getTimer();
    unsigned long last_time = timer->getMilliseconds();
    // The A that opened the screen is not a press on it.
    bool nav_held = true, a_held = true, b_held = true, x_held = true, y_held = true;

    while (action == MenuAction::None && !m.window->isClosed() && tl::core::should_keep_running()) {
        tl::core::pump_events();
        const unsigned long now = timer->getMilliseconds();
        const float delta = std::min(static_cast<float>(now - last_time) * 0.001f, 0.1f);
        last_time = now;

        const tl::core::InputState input = tl::core::poll_input();
        const bool up = input.move_y > 0.5f;
        const bool down = input.move_y < -0.5f;
        if (up || down) {
            if (!nav_held) {
                press(up ? tl::core::PadButton::DPadUp : tl::core::PadButton::DPadDown, false);
            }
            nav_held = true;
        } else {
            nav_held = false;
        }
        const bool bumpers = input.tab_prev && input.tab_next;
        if (action == MenuAction::None && input.confirm && !a_held) {
            press(tl::core::PadButton::A, bumpers);
        }
        if (action == MenuAction::None && input.skip && !b_held) {
            press(tl::core::PadButton::B, false);
        }
        if (action == MenuAction::None && input.attack && !x_held) {
            press(tl::core::PadButton::X, false);
        }
        if (action == MenuAction::None && input.action_y && !y_held) {
            press(tl::core::PadButton::Y, false);
        }
        a_held = input.confirm;
        b_held = input.skip;
        x_held = input.attack;
        y_held = input.action_y;
        m.confirm_held = input.confirm;
        m.skip_held = input.skip;
        m.quit_held = input.quit;

        m.show_load_character(state, shown_file);
        m.advance(delta);
        if (!m.root->renderOneFrame()) break;
        m.save_scripted_shot();
    }

    m.config.turntable_degrees_per_second = turntable;
    screen.hide();
    if (action == MenuAction::None) action = MenuAction::Quit;
    TL_LOG_INFO("load character: %s", action == MenuAction::Continue        ? "continue"
                                      : action == MenuAction::LoadCharacter ? "view"
                                      : action == MenuAction::Back          ? "back"
                                                                            : "quit");
    return action;
}

// `CCharacter::getIntroCinematic`, as
// `CGameClient::setGameState` asks a new character for it.
std::string Renderer::intro_cinematic() const { return impl_->player_stats.intro_cinematic; }

// `CGameClient::performWarp`: the cinematic this arrival opens, or
// none.
std::string Renderer::warp_cinematic() const {
    const Impl& m = *impl_;
    if (m.config.travel == nullptr || m.level_dungeon.empty()) return {};
    const int32_t deepest = tl::core::get_max_depth(*m.config.travel, m.level_dungeon);
    if (!tl::core::warp_plays_cinematic(deepest, m.level_depth, m.level_cinematic,
                                        m.level_restored)) {
        return {};
    }
    return m.level_cinematic;
}

// `CGameUI::setCinematicOpen`:
// `CCinematicMenu::setText` then `setOpen(true)`, and the screen stays until
// its button is taken.
MenuAction Renderer::run_cinematic(const std::string& dump_png, const std::string& name) {
    Impl& m = *impl_;
    if (!m.window) return MenuAction::Quit;

    m.load_cinematics();
    // `setText` does nothing for a name `CCinematics` has not got,
    // but `setOpen(true)` still runs; a screen with nothing on it is not
    // opened here.
    const tl::core::Cinematic* cinematic = m.cinematics.get(name);
    if (cinematic == nullptr) {
        TL_LOG_WARN("cinematic: nothing in media/cinematics/ is called '%s'", name.c_str());
        return MenuAction::NewGame;
    }
    TL_LOG_INFO("cinematic: %s opens (image '%s', sound '%s', %.2f s crawl)",
                cinematic->name.c_str(), cinematic->image.c_str(), cinematic->sound.c_str(),
                static_cast<double>(tl::core::cinematic_crawl_seconds(
                    tl::core::cinematic_display_text(cinematic->text))));

    // A headless run with no picture asked for has nobody to press Continue.
    if (m.config.headless && dump_png.empty()) return MenuAction::NewGame;

    CinematicMenu screen;
    if (!screen.create(kGameGroup, m.config.width, m.config.height, *cinematic)) {
        TL_LOG_WARN("cinematic: '%s' could not be built", name.c_str());
        return MenuAction::NewGame;
    }

    // `CCinematicMenu::setOpen(true)`: `setIngameUIVisible(false)`.
    const bool hud_was_up = m.game_hud.visible();
    if (hud_was_up) m.game_hud.hide();
    const bool automap_was_up = m.automap_initialized && m.automap.is_visible();
    if (automap_was_up) m.automap.hide();
    const auto close = [&]() {
        screen.destroy();
        if (hud_was_up) m.game_hud.show();
        if (automap_was_up) m.automap.show();
    };

    if (!dump_png.empty()) {
        // The crawl at rest, which is the frame that shows whether the text
        // fits its window.
        screen.advance_crawl(screen.crawl().duration);
        screen.advance_crawl(0.0f);
        dump_frame(dump_png);
        close();
        return MenuAction::NewGame;
    }

    // `setText`: the cinematic's `SOUND` in slot 0x26 of a bank of
    // the menu's own, played with no position.
    const bool voice_started = m.play_sound_2d(cinematic->sound, "cinematic");
    m.cinematic_open = true;
    for (const Impl::MachineView& view : m.machines()) view.script->set_modal_open(true);

    Ogre::Timer* timer = m.root->getTimer();
    unsigned long last = timer->getMilliseconds();
    MenuAction action = MenuAction::NewGame;

    while (!m.window->isClosed() && tl::core::should_keep_running()) {
        tl::core::pump_events();

        const unsigned long now = timer->getMilliseconds();
        const float delta = std::min(static_cast<float>(now - last) * 0.001f, 0.1f);
        last = now;

        const tl::core::InputState input = tl::core::poll_input();
        const bool finished = screen.update(delta, input);

        // The level stands still underneath: the cinematic menu is on
        // `CGameUI`'s modal list (`CGameUI::create`), which
        // `CGameClient::getIsPaused` counts.
        m.advance(delta);
        if (!m.root->renderOneFrame()) {
            action = MenuAction::Quit;
            break;
        }
        m.save_scripted_shot();

        if (finished) break;
    }

    // `CCinematicMenu::setOpen(false)`: the bank is stopped and the
    // interface comes back. The music climbs back from `advance()`.
    if (voice_started) tl::core::audio_stop_voice(lowered(cinematic->sound));
    m.cinematic_open = false;
    // The button that closed it is still down: the game's own edges start from the pad as
    // it is now, so the A is not seen again as a fresh press.
    {
        const tl::core::InputState held = tl::core::poll_input();
        m.confirm_held = held.confirm;
        m.skip_held = held.skip;
        m.attack_held = held.attack;
        m.action_x_held = held.attack;
        m.action_y_held = held.action_y;
        m.quit_held = held.quit;
        m.menu_next_held = held.menu_next;
        m.menu_prev_held = held.menu_prev;
        m.tab_next_held = held.tab_next;
        m.tab_prev_held = held.tab_prev;
        m.inventory_button_held = held.inventory;
    }
    for (const Impl::MachineView& view : m.machines()) view.script->set_modal_open(false);
    close();

    TL_LOG_INFO("cinematic: %s closed", name.c_str());
    return action;
}

// PORT: a headless verification switch (`--highlight`): a unit lit as if selected.
bool Renderer::highlight_unit(const std::string& name) {
    Impl& m = *impl_;
    const std::string wanted = lowered(name);
    // "player" lights the character itself: not a game feature, but a model always on
    // screen in a fixed pose, for static frame comparisons.
    if (wanted == "player" || wanted == "subject") {
        if (m.subject == nullptr) return false;
        highlight_entity(m.subject, true, false, m.rim_light_texture);
        TL_LOG_INFO("highlight: the player's own model");
        return true;
    }
    for (const auto& entry : m.scene_units) {
        if (lowered(entry.first).find(wanted) == std::string::npos) continue;
        if (entry.second.entity == nullptr) continue;
        const tl::core::UnitStats* stats = m.unit_book.find(entry.first);
        const bool hostile = stats != nullptr && stats->hostile();
        highlight_entity(entry.second.entity, true, stats != nullptr && m.outline_hostile(*stats),
                         m.rim_light_texture);
        // `--highlight` means "pretend the selector picked this", so the HUD's
        // target panel fills too - otherwise a static `--hud-dump` shows the
        // glow with no name, level or bar over it.
        m.pad_selected = entry.first;
        m.pad_selected_node = entry.second.node;
        m.pad_selected_hostile = hostile;
        m.pad_highlighted = entry.second.entity;
        m.pad_highlighted_hostile = stats != nullptr && m.outline_hostile(*stats);
        const Ogre::SceneNode* at = entry.second.entity->getParentSceneNode();
        const Ogre::Vector3 where = at != nullptr ? at->_getDerivedPosition() : Ogre::Vector3::ZERO;
        TL_LOG_INFO("highlight: '%s'%s, mesh '%s' at (%.1f, %.1f, %.1f)", entry.first.c_str(),
                    hostile ? " (hostile)" : "", entry.second.mesh.c_str(),
                    static_cast<double>(where.x), static_cast<double>(where.y),
                    static_cast<double>(where.z));
        return true;
    }
    TL_LOG_ERROR("--highlight: no placed unit matches '%s'", name.c_str());
    return false;
}

// PORT: a headless verification switch (`--pad`): the target selector run headless.
void Renderer::set_headless_targeting(bool on) {
    impl_->headless_targeting = on;
    TL_LOG_INFO("headless targeting %s", on ? "on: the console's target selector runs" : "off");
}

// PORT: a headless verification switch (`--attack`): swing whenever possible.
void Renderer::set_auto_attack(bool on) {
    impl_->auto_attack = on;
    TL_LOG_INFO("auto-attack %s", on ? "on: the character swings whenever it can" : "off");
}

// PORT: the renderer's interface to `main.cpp` (headless runs).
bool Renderer::attack() { return impl_->begin_swing(0); }

// PORT: a headless verification switch (`--drop-dump`): a carried item dropped.
bool Renderer::drop_carried(const std::string& item) {
    Impl& m = *impl_;
    const std::vector<tl::core::InventorySlot>& slots = m.player_inventory.slots();
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].count <= 0) continue;
        if (lowered(slots[i].item.name) != lowered(item)) continue;
        const tl::core::ItemStats stats = slots[i].item;
        const int count = slots[i].count;
        m.player_inventory.remove_item(i, count);
        if (tl::core::Inventory::is_equip_location(i)) m.sync_equipped_weapon();
        m.drop_item_near_player(stats, count);
        return true;
    }
    TL_LOG_WARN("--drop-dump: nothing carried is called '%s'", item.c_str());
    return false;
}

// PORT: a headless verification switch (`--pu-smoke`): a ParticleUniverse test effect.
bool Renderer::spawn_pu_smoke() {
    Impl& m = *impl_;
    return m.particle_universe.spawn_smoke(m.scene, m.subject_node, m.camera);
}

void Renderer::advance_scene(float seconds) {
    Impl& m = *impl_;
    if (!(seconds > 0.0f) || !m.root) return;

    // The same fixed 60 Hz step as simulate_walk(), so both targets agree.
    constexpr float kStep = 1.0f / 60.0f;
    const int steps = static_cast<int>(seconds / kStep);

    TL_LOG_INFO(
        "advancing the scene %.2fs at 60 Hz (%d step(s)): %zu animated entit%s, "
        "%zu particle system(s)",
        static_cast<double>(seconds), steps, m.scene_animations.size(),
        m.scene_animations.size() == 1 ? "y" : "ies", m.particle_systems.size());

    const size_t gaps_before = m.script.unimplemented().size();

    for (int i = 0; i < steps; ++i) {
        // A scripted walk of the player is part of the scene (the town's cutscene ends with
        // one); no input is polled.
        if (m.player_control && m.player.following()) {
            m.step_player(kStep, tl::core::InputState{});
            const char* wanted = m.player.animation();
            if (m.player_animation != wanted) {
                apply_animation(wanted, 0.0f);
                m.player_animation = wanted;
            }
        } else if (m.player_control && m.subject_node) {
            // The camera's half of `step_player`, which the run loop reaches
            // every frame: without it a run that stands still never eases into
            // a close-up (a scene's line, a vendor) or shakes.
            m.camera_delta = kStep;
            m.follow_subject();
            m.camera_delta = 0.0f;
        }
        // No button is down in a headless run, so the dispatch's release test
        // lets a held skill go as a tapped button would.
        if (m.player_control) {
            m.release_held_skill(std::array<bool, 7>{}, std::array<bool, 7>{}, false);
        }
        m.advance(kStep);
        // `--pad`: the console's target selector, stepped here since a headless run never
        // reaches the run loop.
        if (m.headless_targeting && m.player_control) {
            m.update_item_selection(!m.update_pad_selection(true));
        }
        // Rendered, not just stepped. renderOneFrame() is what fires OGRE's
        // frame events, and the material controllers - the town's scrolling
        // river and its animated foliage - advance on those and on nothing else.
        if (!m.root->renderOneFrame(kStep)) break;
    }

    if (m.particle_universe.smoke_particles() > 0) {
        TL_LOG_INFO("--pu-smoke: %zu ParticleUniverse particle(s) alive",
                    m.particle_universe.smoke_particles());
    }

    // What the level script asked for and did not get, logged: a half-running level looks
    // like a running one.
    const std::vector<std::string> gaps = m.script.unimplemented();
    if (gaps.size() > gaps_before) {
        TL_LOG_WARN("the level script asked for %zu thing(s) this engine does not do yet:",
                    gaps.size());
        for (const std::string& gap : gaps) TL_LOG_WARN("  %s", gap.c_str());
    }
}

namespace renderer_detail {
/// Whether dump_frame() should leave the loading screen up. Only
/// dump_loading_screen() sets it; see the note at the hide below.
bool keep_loading_screen = false;

/// The same for the game screen's HUD, and for the same reason. Only
/// dump_game_hud() sets it.
bool keep_game_hud = false;

/// The same for the console inventory menu. Only dump_inventory() sets it.
bool keep_inventory = false;

}  // namespace renderer_detail
// PORT: a pad script's screenshot (`--pad-script`), with its diagnostics.
void Renderer::Impl::save_scripted_shot() {
    std::string png;
    if (!tl::core::take_scripted_shot(png) || window == nullptr) return;
    window->writeContentsToFile(png);
    TL_LOG_INFO("pad script: saved %s", png.c_str());
    log_dropped_item_materials();
    log_shot_units();
}

/// Beside a pad script's shot: each unit in view, where it stands against
/// the ground under it, and what its model is playing - the console showed a
/// townsman sunk to the ankles that the desktop draws on the ground.
void Renderer::Impl::log_shot_units() {
    if (camera == nullptr) return;
    for (const auto& entry : scene_units) {
        Ogre::Entity* entity = entry.second.entity;
        if (entity == nullptr || !entity->isVisible()) continue;
        const Ogre::SceneNode* node = entity->getParentSceneNode();
        if (node == nullptr || !camera->isVisible(entity->getWorldBoundingBox(true))) continue;
        const Ogre::Vector3 at = node->_getDerivedPosition();
        float ground = 0.0f;
        const bool found = collision.ground_under(at.x, at.z, at.y + 4.0f, ground);
        std::string playing;
        if (Ogre::AnimationStateSet* states = entity->getAllAnimationStates()) {
            for (const auto& state : states->getAnimationStates()) {
                if (!state.second->getEnabled()) continue;
                char part[96];
                std::snprintf(part, sizeof(part), "%s%s %.2f/%.2f w%.2f",
                              playing.empty() ? "" : ", ", state.first.c_str(),
                              static_cast<double>(state.second->getTimePosition()),
                              static_cast<double>(state.second->getLength()),
                              static_cast<double>(state.second->getWeight()));
                playing += part;
            }
        }
        const Ogre::Vector3 scale = node->_getDerivedScale();
        TL_LOG_INFO(
            "shot unit: %s at (%.2f, %.3f, %.2f) scale %.2f, ground %s%.3f (%+.3f), "
            "bounds y %.3f..%.3f, %s, playing [%s]",
            entry.first.c_str(), static_cast<double>(at.x), static_cast<double>(at.y),
            static_cast<double>(at.z), static_cast<double>(scale.y), found ? "" : "none ",
            static_cast<double>(ground), static_cast<double>(found ? at.y - ground : 0.0f),
            static_cast<double>(entity->getWorldBoundingBox(true).getMinimum().y),
            static_cast<double>(entity->getWorldBoundingBox(true).getMaximum().y),
            entity->isHardwareAnimationEnabled() ? "gpu skinned" : "cpu skinned", playing.c_str());
    }
}

/// What each dropped item in view is drawn with, logged beside a pad script's shot.
// PORT: a diagnostic logged beside a pad script's shot.
void Renderer::Impl::log_dropped_item_materials() {
    if (camera == nullptr) return;
    particle_universe.log_materials(scene);
    // Every generated shader is written again when more lights are in view
    // than this (`SGScheme::synchronizeWithLightSettings`).
    if (shader_generator != nullptr) {
        TL_LOG_INFO("shot shaders: generated for %d light(s)",
                    static_cast<int>(
                        shader_generator->getRenderState(Ogre::MSN_SHADERGEN)->getLightCount()));
    }
    for (const DroppedWorldItem& dropped : dropped_items) {
        if (dropped.entity == nullptr || dropped.node == nullptr) continue;
        if (!camera->isVisible(dropped.node->_getWorldAABB())) continue;
        const Ogre::Vector3 at = dropped.node->_getDerivedPosition();
        TL_LOG_INFO(
            "shot item: %s at (%.2f, %.2f, %.2f), visible %d, opacity %.2f, queue %u, "
            "flying %d",
            dropped.item.display_name.c_str(), static_cast<double>(at.x), static_cast<double>(at.y),
            static_cast<double>(at.z), dropped.presence.visible ? 1 : 0,
            static_cast<double>(dropped.presence.opacity),
            static_cast<unsigned>(dropped.entity->getRenderQueueGroup()), dropped.flying ? 1 : 0);
        for (Ogre::SubEntity* sub : dropped.entity->getSubEntities()) {
            const Ogre::MaterialPtr& material = sub->getMaterial();
            if (!material) {
                TL_LOG_INFO("  submesh: no material");
                continue;
            }
            Ogre::Technique* best = sub->getTechnique();
            const std::string drawn_with = best == nullptr
                                               ? std::string("nothing")
                                               : "'" + best->getSchemeName() + "' technique of '" +
                                                     best->getParent()->getName() + "'";
            TL_LOG_INFO(
                "  material '%s' (loaded %d, %zu technique(s), %zu supported), drawn with "
                "%s",
                material->getName().c_str(), material->isLoaded() ? 1 : 0,
                static_cast<size_t>(material->getNumTechniques()),
                material->getSupportedTechniques().size(), drawn_with.c_str());
            for (const Ogre::Technique* technique : material->getTechniques()) {
                for (const Ogre::Pass* pass : technique->getPasses()) {
                    const Ogre::ColourValue d = pass->getDiffuse();
                    const Ogre::ColourValue e = pass->getSelfIllumination();
                    std::string units;
                    for (const Ogre::TextureUnitState* unit : pass->getTextureUnitStates()) {
                        units +=
                            (units.empty() ? "" : ", ") + unit->getTextureName() +
                            (unit->getTextureType() == Ogre::TEX_TYPE_CUBE_MAP ? " (cube)" : "") +
                            (unit->_getTexturePtr() ? "" : " (no texture)");
                    }
                    TL_LOG_INFO(
                        "    '%s' pass %u: programmable %d, lighting %d, blend %d/%d, depth "
                        "write %d, reject %d/%u, diffuse (%.2f, %.2f, %.2f, %.2f), emissive "
                        "(%.2f, %.2f, %.2f), units [%s]",
                        technique->getSchemeName().c_str(), static_cast<unsigned>(pass->getIndex()),
                        pass->isProgrammable() ? 1 : 0, pass->getLightingEnabled() ? 1 : 0,
                        static_cast<int>(pass->getSourceBlendFactor()),
                        static_cast<int>(pass->getDestBlendFactor()),
                        pass->getDepthWriteEnabled() ? 1 : 0,
                        static_cast<int>(pass->getAlphaRejectFunction()),
                        static_cast<unsigned>(pass->getAlphaRejectValue()),
                        static_cast<double>(d.r), static_cast<double>(d.g),
                        static_cast<double>(d.b), static_cast<double>(d.a),
                        static_cast<double>(e.r), static_cast<double>(e.g),
                        static_cast<double>(e.b), units.c_str());
                }
            }
        }
    }
    // ... and the drop effect's particle materials (`GENERIC-ITEM.LAYOUT`'s
    // `star3.dds` and `multi-sparkle clear.dds`, `CParticleTechWrapper`'s
    // `tl/pu/<texture>/<style>`): the white shapes could be either.
    auto& materials = Ogre::MaterialManager::getSingleton();
    for (auto it = materials.getResourceIterator(); it.hasMoreElements();) {
        const Ogre::ResourcePtr resource = it.getNext();
        const std::string& name = resource->getName();
        if (name.rfind("tl/pu/", 0) != 0) continue;
        if (name.find("star3") == std::string::npos && name.find("sparkle") == std::string::npos) {
            continue;
        }
        const auto* material = static_cast<const Ogre::Material*>(resource.get());
        for (const Ogre::Technique* technique : material->getTechniques()) {
            for (const Ogre::Pass* pass : technique->getPasses()) {
                std::string units;
                for (const Ogre::TextureUnitState* unit : pass->getTextureUnitStates()) {
                    const Ogre::TexturePtr& texture = unit->_getTexturePtr();
                    char part[160];
                    std::snprintf(part, sizeof(part), "%s%s %s %ux%u mips %u alpha %d",
                                  units.empty() ? "" : ", ", unit->getTextureName().c_str(),
                                  texture
                                      ? Ogre::PixelUtil::getFormatName(texture->getFormat()).c_str()
                                      : "(no texture)",
                                  texture ? static_cast<unsigned>(texture->getWidth()) : 0u,
                                  texture ? static_cast<unsigned>(texture->getHeight()) : 0u,
                                  texture ? static_cast<unsigned>(texture->getNumMipmaps()) : 0u,
                                  texture && texture->hasAlpha() ? 1 : 0);
                    units += part;
                }
                TL_LOG_INFO(
                    "shot particle material '%s' '%s' pass %u: programmable %d, lighting %d, "
                    "blend %d/%d, colour op %d, units [%s]",
                    name.c_str(), technique->getSchemeName().c_str(),
                    static_cast<unsigned>(pass->getIndex()), pass->isProgrammable() ? 1 : 0,
                    pass->getLightingEnabled() ? 1 : 0,
                    static_cast<int>(pass->getSourceBlendFactor()),
                    static_cast<int>(pass->getDestBlendFactor()),
                    pass->getNumTextureUnitStates() > 0
                        ? static_cast<int>(
                              pass->getTextureUnitState(0)->getColourBlendMode().operation)
                        : -1,
                    units.c_str());
            }
        }
    }
}

// The light map first, as the frame loop's `renderOneFrame` does (a render
// texture's group is updated before the window's): a frame dump updates its own
// target only, and the scenery samples the map. `--light-map-dump` writes both.
void Renderer::Impl::update_light_maps(const std::string& png_path) {
    if (shadow_map.texture) {
        Ogre::RenderTarget* shadow_target = shadow_map.texture->getBuffer()->getRenderTarget();
        shadow_target->update();
        if (config.light_map_dump) shadow_target->writeContentsToFile(png_path + ".shadowmap.png");
    }
    if (!light_map.texture) return;
    Ogre::RenderTarget* light_target = light_map.texture->getBuffer()->getRenderTarget();
    light_target->update();
    if (!config.light_map_dump) return;
    light_target->writeContentsToFile(png_path + ".lightmap.png");
    const Ogre::Vector3 at = light_map.camera_node->getPosition();
    const Ogre::RenderTarget::FrameStats& drawn = light_target->getStatistics();
    TL_LOG_INFO("wrote %s.lightmap.png (%zu batches, %zu triangles; camera at (%.2f, %.2f, %.2f))",
                png_path.c_str(), drawn.batchCount, drawn.triangleCount, static_cast<double>(at.x),
                static_cast<double>(at.y), static_cast<double>(at.z));
}

bool Renderer::dump_frame(const std::string& png_path) {
    Impl& m = *impl_;

    // Never a loading screen over a dumped frame (dump_loading_screen(), a backdrop excepted).
    const bool loading_was_up = m.loading.visible();
    m.loading.hide();
    if (keep_loading_screen && loading_was_up)
        m.loading.show_as_before(kGameGroup, m.config.width, m.config.height);

    // Nor the HUD (only `--hud-dump` asks for it).
    const bool hud_was_up = m.game_hud.visible();
    if (!keep_game_hud) m.game_hud.hide();

    // And the inventory menu.
    const bool inv_was_up = m.inventory_menu.visible();
    if (!keep_inventory) m.inventory_menu.hide();

    // The quest offer window stays, as the dialogue box does: both are transient and only
    // there when asked for.

    // Pin the time-driven controllers (the menu's scrolling billboard) for a reproducible
    // dump; the factor is restored on the way out.
    auto& controllers = Ogre::ControllerManager::getSingleton();
    const Ogre::Real saved_factor = controllers.getTimeFactor();
    controllers.setTimeFactor(0.0f);
    controllers.setElapsedTime(0.0f);

    // Rendered offscreen rather than read back from the window: a hidden or
    // obscured window has no dependable backbuffer to read, and this way the
    // dump is identical whether or not --headless was passed.
    Ogre::TexturePtr target = Ogre::TextureManager::getSingleton().createManual(
        "tl/dump_frame", Ogre::RGN_DEFAULT, Ogre::TEX_TYPE_2D, m.config.width, m.config.height, 0,
        Ogre::PF_BYTE_RGBA, Ogre::TU_RENDERTARGET);

    Ogre::RenderTexture* render_target = target->getBuffer()->getRenderTarget();
    Ogre::Viewport* viewport = render_target->addViewport(m.camera);
    viewport->setVisibilityMask(kVisibilityWorld);
    m.apply_scheme(viewport);
    render_target->setAutoUpdated(false);

    if (keep_game_hud && m.automap.is_visible() && m.automap.camera() != nullptr) {
        if (m.automap_initialized && m.subject_node) {
            m.automap.set_player_position(m.subject_node->getPosition(),
                                          m.subject_node->getOrientation());
            m.automap.update(m.subject_node->getPosition(), 0.0f);
        }
        Ogre::Camera* auto_cam = m.automap.camera();
        Ogre::Viewport* auto_vp = render_target->addViewport(auto_cam, 2);
        // Drawn the way the window draws it (Automap::attach).
        m.automap.attach(render_target);
        auto_vp->setClearEveryFrame(true, Ogre::FBT_DEPTH);
        auto_vp->setBackgroundColour(Ogre::ColourValue(0.0f, 0.0f, 0.0f, 0.0f));
        auto_vp->setVisibilityMask(kVisibilityAutomap);
        auto_vp->setOverlaysEnabled(false);
        auto_vp->setMaterialScheme(Ogre::MSN_SHADERGEN);

        // Bottom-right circular minimap.
        const float aspect =
            static_cast<float>(m.config.width) / static_cast<float>(m.config.height);
        const float h = 0.240f;
        const float w = h / aspect;
        const float top = 0.7896f - (h * 0.5f);
        const float left = 0.8973f - (w * 0.5f);
        auto_vp->setDimensions(left, top, w, h);
    }

    if (m.config.dump_gamma) m.display_gamma.attach(render_target);

    m.camera->setAspectRatio(static_cast<Ogre::Real>(m.config.width) /
                             static_cast<Ogre::Real>(m.config.height));

    if (m.subject && !subject_in_shot(m.camera, m.subject)) {
        TL_LOG_WARN("the subject is outside the camera's frustum; '%s' will not show it",
                    png_path.c_str());
    }

    // Every draw of this frame, when asked (`--draw-dump`): the light map's and
    // the frame's, in the order they are made.
    std::unique_ptr<DrawDump> draws;
    if (!m.config.draw_dump.empty()) {
        draws = std::make_unique<DrawDump>(m.scene, m.config.draw_dump);
        // Where the player stands, against the capture's player position
        // (the console's `CPositionableObject +0x70`).
        if (m.player_control && m.subject_node != nullptr) {
            const tl::core::Vec3 feet = m.player.position();
            const Ogre::Vector3 node = m.subject_node->getPosition();
            TL_LOG_INFO(
                "draw dump: player at (%.4f, %.4f, %.4f), node y %.4f, collision floor %.4f",
                static_cast<double>(feet.x), static_cast<double>(feet.y),
                static_cast<double>(feet.z), static_cast<double>(node.y),
                static_cast<double>(m.collision.floor_height(feet.x, feet.z)));
        }
        if (m.shadow_map.texture) {
            draws->watch(m.shadow_map.texture->getBuffer()->getRenderTarget());
        }
        if (m.light_map.texture) draws->watch(m.light_map.texture->getBuffer()->getRenderTarget());
        draws->watch(render_target);
    }

    try {
        m.update_light_maps(png_path);
        // The timer's queue listener (TL_OVERLAY_DUMP lists the interface's draws).
        m.gpu_timer.frame_started(m.scene, {});
        m.gpu_timer.dump_overlay_next = std::getenv("TL_OVERLAY_DUMP") != nullptr;
        m.touch_overlay.step();  // no frame listener fires for this target
        render_target->update();
        render_target->writeContentsToFile(png_path);
        m.automap.detach(render_target);
    } catch (const Ogre::Exception& e) {
        m.automap.detach(render_target);
        // The full description, not just the message: it carries the source
        // file, line and function, which is the difference between "something
        // in OGRE threw" and knowing what.
        TL_LOG_ERROR("frame dump failed: %s", e.getFullDescription().c_str());
        draws.reset();
        Ogre::TextureManager::getSingleton().remove(target);
        controllers.setTimeFactor(saved_factor);
        if (hud_was_up) m.game_hud.show();
        if (inv_was_up) m.inventory_menu.show();
        return false;
    }

    if (draws) {
        TL_LOG_INFO("draw dump: %d draws into %s", draws->draws(), m.config.draw_dump.c_str());
        draws.reset();
    }
    const Ogre::RenderTarget::FrameStats& stats = render_target->getStatistics();
    TL_LOG_INFO("wrote %s (%ux%u, %d batches, %d triangles)", png_path.c_str(), m.config.width,
                m.config.height, static_cast<int>(stats.batchCount),
                static_cast<int>(stats.triangleCount));
    // The level's own `Particle` nodes that are playing, and what they have
    // alive: a trap's tell is a handful of sparks, which a frame alone does not
    // settle either way.
    for (const ParticleSystem& system : m.particle_systems) {
        if (!system.wrapper || !system.pu.playing) continue;
        TL_LOG_INFO("  particle wrapper %lld playing: %zu particle(s)%s",
                    static_cast<long long>(system.node), torchlight_particle_count(system.pu),
                    system.pu.dying ? ", dying out" : "");
    }
    // Where the pet was when the shutter went: whether it kept station.
    if (m.pet.alive() && m.pet.in_world() && m.subject_node != nullptr) {
        const Ogre::Vector3 owner = m.subject_node->getPosition();
        const tl::core::Vec3 at = m.pet.position();
        const float dx = at.x - owner.x;
        const float dz = at.z - owner.z;
        TL_LOG_INFO("  pet at (%.2f, %.2f), %.2f from its owner, %s%s", static_cast<double>(at.x),
                    static_cast<double>(at.z), static_cast<double>(std::sqrt(dx * dx + dz * dz)),
                    m.pet.running() ? "running" : "walking",
                    m.pet.has_destination() ? ", on its way somewhere" : ", stood still");
    }

    Ogre::TextureManager::getSingleton().remove(target);
    controllers.setTimeFactor(saved_factor);
    if (hud_was_up) m.game_hud.show();
    if (inv_was_up) m.inventory_menu.show();
    return true;
}

}  // namespace tl::render
