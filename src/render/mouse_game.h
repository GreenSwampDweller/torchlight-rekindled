// The PC's mouse and keyboard in the game (core/mouse_client.h, core/mouse_pick.h): the
// mouse layer's one object. Its members are defined in mouse_game.cpp (the frame, the
// HUD and the keys), mouse_game_pick.cpp (what the pointer is over and where it points)
// and mouse_game_ai.cpp (the player's AI the clicks start, and the host's longer answers).
#pragma once

#include <OgreCamera.h>
#include <OgreEntity.h>
#include <OgreSceneNode.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include "core/combat.h"
#include "core/input.h"
#include "core/log.h"
#include "core/mouse_client.h"
#include "core/mouse_pick.h"
#include "render/mouse_panes.h"
#include "render/pc_foldout.h"
#include "render/pc_hud.h"
#include "render/renderer_impl.h"

namespace tl::render {

using Kind = PlayerItemTarget::Kind;
using Unit = tl::core::MouseUnit;
using tl::core::Vec3;

/// The AI states the clicks reach that the core does not name.
constexpr int kAttackState = 3;
constexpr int kSkillState = 0xd;

inline Vec3 to_core(const Ogre::Vector3& v) { return {v.x, v.y, v.z}; }

/// An entity's local bounds with its node's scale on them.
inline void entity_bounds(const Ogre::Entity* entity, Vec3& low, Vec3& high) {
    low = {-0.5f, 0.0f, -0.5f};
    high = {0.5f, 1.0f, 0.5f};
    if (entity == nullptr) return;
    const Ogre::AxisAlignedBox& box = entity->getBoundingBox();
    if (!box.isFinite()) return;
    Ogre::Vector3 scale = Ogre::Vector3::UNIT_SCALE;
    if (const Ogre::SceneNode* node = entity->getParentSceneNode()) {
        scale = node->_getDerivedScale();
    }
    low = to_core(box.getMinimum() * scale);
    high = to_core(box.getMaximum() * scale);
}

inline float planar(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + 0.0f + dz * dz);
}

/// The mouse layer: its state between frames, the picks, the player's AI the clicks
/// start, and the game behind the clicks (`MouseHost`).
struct Renderer::Impl::MouseGame final : tl::core::MouseHost {
    /// Something the pointer can be over that is not a character: one of the port's item
    /// targets, or a breakable by its id.
    struct Item {
        PlayerItemTarget target;
        int64_t breakable = 0;
    };
    /// A character the pointer could be over this frame.
    struct Character {
        int64_t node = 0;
        std::string name;
        Ogre::Entity* entity = nullptr;
    };
    /// What a pick found.
    struct Over {
        Ogre::Entity* unit_entity = nullptr;
        std::string unit_name;
        bool unit_hostile = false;
        int64_t unit_node = 0;
        Ogre::Entity* item_entity = nullptr;
        Item item;
        bool have_item = false;
    };
    /// The items a pick chooses among.
    struct ItemPicks {
        std::vector<tl::core::PickCandidate> candidates;
        std::vector<Item> items;
        std::vector<Ogre::Entity*> entities;
    };

    Impl& g;
    tl::core::MouseClient client;
    tl::core::InputState input;
    bool left_was = false;
    bool right_was = false;
    bool middle_was = false;
    bool pointer_seen = false;
    /// A console window has the input and both buttons have been up since it opened.
    bool window_armed = false;
    std::uint16_t keys_was = 0;
    std::uint32_t pc_keys_was = 0;
    /// The player's AI state as the clicks set it (`+0x330`): 0, 3, 4, 0xd or 0xf. The
    /// walk to an item (7) and to somebody (9) are `player_item` and `pending_talk`.
    int state = 0;
    /// `+0x4d0` / `+0x4d1`: a swing or a cast went out since the last click.
    bool attack_started = false;
    /// `+0x278`: seconds since the route last began or ended.
    float path_timer = 10.0f;
    /// `+0x350`, as a handle into `items`.
    Unit target_item_handle = 0;
    /// Where the route being walked ends, and the last point none was found to.
    Vec3 route_end;
    Vec3 no_route;
    /// The point `setTargetDirection` was last given this frame.
    std::optional<Vec3> aim;
    std::int64_t approach_skill = -1;
    std::vector<Item> items;
    std::vector<Character> units;
    /// The PC's HUD, when a PC pak is mounted, and the window of it the pointer is over.
    PcHud pc_hud;
    bool pc_hud_tried = false;
    unsigned hud_width = 0;
    unsigned hud_height = 0;
    std::string over_hud;
    /// The pointer is the interface's this frame (`CGameUI::mouseOverPanel`).
    bool hud_blocks = false;

    /// The skill foldout: what it was opened for (a numbered slot from 0, or
    /// `kFoldoutRight`), from which window of the HUD, and the skill of each of its cells.
    static constexpr int kFoldoutRight = 101;
    PcFoldout foldout;
    int foldout_for = -1;
    std::string foldout_window;
    std::vector<std::int64_t> foldout_skills;
    /// Which of its cells are items of the bag (by their unit's GUID) rather than skills.
    std::vector<bool> foldout_items;
    void toggle_weapon_set();
    bool interface_takes(const tl::core::MouseButtons& buttons, bool taken);
    void toggle_foldout(int target, const std::string& window);
    bool click_foldout(const tl::core::MouseButtons& buttons);
    void list_foldout_items(std::vector<PcFoldout::Entry>& entries, int row);
    /// The Pause Menu and Options under the pointer: where it last was, and which of its
    /// buttons were down.
    float menu_x = -1.0f;
    float menu_y = -1.0f;
    bool menu_left_was = false;
    bool menu_right_was = false;
    /// The Help screen's wheel: the frames a tick still scrolls for, and which way.
    int help_scroll_frames = 0;
    bool help_scroll_up = false;
    void help_wheel(tl::core::InputState& in);
    tl::core::InputState master_menu_pointer(tl::core::InputState in);
    /// The skill chosen for the right button in the skills pane, by its GUID.
    std::int64_t active_skill = tl::core::kNoSkill;
    /// The PC's side panes.
    MousePanes panes;

    explicit MouseGame(Impl& game) : g(game), panes(game) {}

    // The frame.
    void frame(float delta, const tl::core::InputState& in);
    void scripted_pointer();
    bool nearest_named_character(const std::string& name, Ogre::Vector3& at) const;
    bool named_thing(const std::string& name, Ogre::Vector3& at) const;
    void step_ai(float delta);
    void sync_hud(float delta);
    const tl::core::KnownSkill* hud_skill_over();
    int hud_item_over();
    void step_hud();
    void step_camera();
    void step_pc_keys();
    void click_hud(const std::string& function);
    void click_slot(int slot, bool right);
    bool click_pet_hud(const std::string& function);
    bool over_hud_takes(const tl::core::MouseButtons& buttons, bool taken);

    // The picks.
    tl::core::ScreenMatrix screen_matrix() const;
    void update_over(bool active);
    void pick_character(const tl::core::ScreenMatrix& matrix, float x, float y, Over& over);
    void pick_item(const tl::core::ScreenMatrix& matrix, float x, float y, Over& over);
    void add_item(const tl::core::ScreenMatrix& matrix, const Vec3& position, Ogre::Entity* entity,
                  const Item& item, ItemPicks& picks) const;
    void add_props(const tl::core::ScreenMatrix& matrix, int machine,
                   const tl::core::LevelScript& owner, const NodeMap& nodes,
                   ItemPicks& picks) const;
    void show_character(const Over& over);
    void show_item(Ogre::Entity* entity);
    void note_item(const Over& over);
    bool world_location(bool raised, Vec3& out);
    bool floor_rise(const Vec3& origin, const Vec3& direction, float feet, float& rise) const;

    // Items by handle.
    Unit item_handle(const Item& item);
    const Item* item_of(Unit handle) const;
    bool item_position(Unit handle, Vec3& at) const;
    bool prop_position(const PlayerItemTarget& target, Vec3& at) const;
    std::string unit_name(int64_t node) const;
    std::string name_of(Unit character) const;

    // The player's AI.
    void reach(const Vec3& at, float target_radius, bool& in_range, bool& sight);
    bool target_point(Vec3& at, float& radius) const;
    void route_to(const Vec3& to);
    void stop();
    void give_up();
    void swing_at_target();
    void attack_ai(float delta);
    void hunt(float delta);
    void skill_approach();
    bool follow_target(const Vec3& at, float moved);
    const tl::core::KnownSkill* skill(std::int64_t guid) const;
    int target_type(const tl::core::KnownSkill& known) const;
    int skill_reach(std::int64_t guid) const;
    bool cast(std::int64_t guid);

    // MouseHost: the client and the interface.
    bool paused() override { return g.game_paused(); }
    bool find_world_location(Vec3& out, bool raised) override {
        return world_location(raised, out);
    }
    bool hold_key() override { return input.hold_position; }
    bool right_button() override { return input.pointer_right; }
    std::int64_t slot_skill(int slot) override {
        const std::int64_t guid = g.player_skills.slots()[static_cast<size_t>(slot)];
        return guid != -1 && g.player_skills.find_by_guid(guid) != nullptr ? guid
                                                                           : tl::core::kNoSkill;
    }
    bool slot_has_item(int slot) override {
        return g.player_skills.second_slots()[static_cast<size_t>(slot)] != -1;
    }
    static bool key(int slot, std::uint16_t keys) { return ((keys >> slot) & 1U) != 0; }
    bool slot_key_held(int slot) override { return key(slot, input.slot_keys); }
    bool slot_key_pressed(int slot) override {
        return key(slot, input.slot_keys) && !key(slot, keys_was);
    }
    bool slot_key_released(int slot) override {
        return !key(slot, input.slot_keys) && key(slot, keys_was);
    }
    void activate_item_slot(int slot) override { g.activate_item_slot(slot); }
    // The cursor that arms a skill for an item of the player's own is the PC
    // interface's; the console's screens have none.
    int cursor_state() override { return 0; }
    void set_cursor_state(int /*state*/) override {}
    void set_pending_skill(std::int64_t /*guid*/) override {}
    void flush_interface_input() override {}
    void set_right_button_pressed() override {}
    bool equipment_tooltip_visible() override { return false; }

    // MouseHost: the player's character.
    bool player_alive() override { return !g.player_death.dying && !g.player_health.dead; }
    bool player_enabled() override { return !g.script_player_disabled; }
    Vec3 player_position() override { return g.player.position(); }
    int ai_state() override;
    void set_ai_state(int wanted) override;
    bool hold_position() override { return g.attack_button.hold; }
    void set_hold_position(bool hold) override { g.attack_button.hold = hold; }
    bool pathing() override { return g.player.following(); }
    void set_path_timer(float seconds) override { path_timer = seconds; }
    bool take_attack_started() override {
        const bool was = attack_started;
        attack_started = false;
        return was;
    }
    bool performing_skill() override { return g.player_performing_skill(); }
    // CCharacter::performingSkillLoose
    bool performing_skill_loose() override {
        return tl::core::skill_holds_character(g.player_active_skill);
    }
    // CCharacter::performingAttackLoose: more than 0.2 s of the swing left.
    bool performing_attack_loose() override {
        return g.swing.active && 0.2f < (g.swing.length - g.swing.elapsed) / g.swing_rate();
    }
    Unit target() override { return static_cast<Unit>(g.player_target_node); }
    Unit target_item() override { return target_item_handle; }
    // CCharacter::setTarget: the target, and the target item always let go.
    void set_target(Unit character) override {
        g.set_player_target(static_cast<int64_t>(character));
        target_item_handle = 0;
        g.player_item = PlayerItemTarget{};
    }
    // CCharacter::setTargetItem: an item lets the target go; none only
    // empties the slot.
    void set_target_item(Unit item) override {
        target_item_handle = item;
        if (item != 0) g.set_player_target(0);
        const Item* found = item_of(item);
        // A breakable is the skills' target item too.
        g.player_skill_item = found != nullptr ? found->breakable : 0;
    }
    void set_target_direction(float x, float z) override {
        aim = Vec3{x, 0.0f, z};
        g.set_player_target_direction({x, 0.0f, z});
    }
    void stop_pathing() override { stop(); }
    void set_destination(float x, float z) override;
    bool in_attack_range() override {
        Vec3 at;
        float radius = 0.0f;
        if (!target_point(at, radius)) return false;
        bool in_range = false;
        bool sight = false;
        reach(at, radius, in_range, sight);
        return in_range;
    }
    void attack() override { swing_at_target(); }
    void hunt_ai() override { hunt(0.0f); }
    void fetch_item(Unit item, bool by_pet, Unit previous) override;
    // INVENTED: the pet is never sent for an item (its own getItemAI is not carried).
    bool pet_can_fetch() override { return false; }

    // MouseHost: characters and items by handle.
    bool unit_alive(Unit character) override {
        tl::core::LevelScript* owner = g.script_owning(static_cast<int64_t>(character));
        return owner != nullptr && !owner->unit_is_dead(static_cast<int64_t>(character));
    }
    bool unit_present(Unit character) override {
        const std::string name = name_of(character);
        Ogre::Vector3 at = Ogre::Vector3::ZERO;
        std::string found;
        return !name.empty() && g.unit_position(name, at, found);
    }
    bool is_enemy(Unit character) override {
        const tl::core::UnitStats* stats = g.unit_book.find(name_of(character));
        return stats != nullptr && g.unit_hostile(static_cast<int64_t>(character), *stats);
    }
    bool unit_interactive(Unit character) override {
        g.load_unit_types();
        const tl::core::UnitStats* stats = g.unit_book.find(name_of(character));
        return stats != nullptr && g.unit_types.isa(g.unit_types.id_of(stats->unittype),
                                                    tl::core::unit_type::kInteractiveCharacter);
    }
    Vec3 unit_position(Unit character) override {
        Vec3 at = g.player.position();
        if (tl::core::LevelScript* owner = g.script_owning(static_cast<int64_t>(character))) {
            owner->unit_position(static_cast<int64_t>(character), at);
        }
        return at;
    }
    bool item_breakable(Unit item) override {
        const Item* found = item_of(item);
        return found != nullptr && found->breakable != 0;
    }
    bool item_takeable(Unit item) override {
        const Item* found = item_of(item);
        return found != nullptr && found->target.kind == Kind::Dropped;
    }
    bool item_usable(Unit item) override {
        Vec3 at;
        return item_position(item, at);
    }
    bool item_selectable(Unit item) override {
        Vec3 at;
        return item_position(item, at);
    }

    // MouseHost: skills.
    bool has_skills() override { return true; }
    bool skill_known(std::int64_t guid) override { return skill(guid) != nullptr; }
    // The right button's skill (the PC's `+0x3b0`, CCharacter::setActiveSkillByName): the
    // one chosen in the skills pane.
    // DIVERGENCE: the console's character keeps none, so it is not saved, and until one is
    // chosen the right button takes the first skill on the hotbar.
    std::int64_t right_skill() override {
        if (active_skill != tl::core::kNoSkill && skill(active_skill) != nullptr) {
            return active_skill;
        }
        for (int slot = 0; slot < tl::core::kMouseSlots; ++slot) {
            const std::int64_t guid = slot_skill(slot);
            if (guid != tl::core::kNoSkill) return guid;
        }
        return tl::core::kNoSkill;
    }
    // The console's character keeps none for the left button (the PC's `+0x3c0`).
    std::int64_t left_skill() override { return tl::core::kNoSkill; }
    bool skill_ready(std::int64_t /*guid*/) override { return false; }
    int skill_target_type(std::int64_t guid) override {
        const tl::core::KnownSkill* known = skill(guid);
        return known != nullptr ? target_type(*known) : 0;
    }
    int skill_animation_index(std::int64_t /*guid*/) override { return 0; }
    bool has_current_skill() override { return !g.player_active_skill.empty(); }
    std::int64_t current_skill() override {
        const tl::core::KnownSkill* known =
            g.player_skills.find_by_name(g.player_active_skill.skill);
        return known != nullptr ? known->guid() : tl::core::kNoSkill;
    }
    bool current_skill_moves() override {
        const tl::core::KnownSkill* known =
            g.player_skills.find_by_name(g.player_active_skill.skill);
        return known != nullptr && known->def != nullptr && known->def->moves_to_target;
    }
    bool cast_skill(std::int64_t guid) override { return cast(guid); }
    void attempt_to_stop_skill(bool now) override { g.attempt_to_stop_player_skill(now); }
    int in_skill_range(std::int64_t guid) override { return skill_reach(guid); }
    void skill_approach_ai(std::int64_t guid) override {
        approach_skill = guid;
        skill_approach();
    }
};

}  // namespace tl::render
