// The game the renderer draws: the gameplay half of what `Renderer::Impl` held, moved here so
// it runs without OGRE and can be checked against the console's own functions. The renderer
// derives from it and answers the hooks below (render/renderer_impl.h).
#pragma once

#include "core/ascii_case.h"
#include "core/world_state.h"

namespace tl::core {

/// What `World::view_settle_link_particles` does with a link's particle systems: learn their link,
/// become one `CParticle`, stop.
struct LinkParticleSettle {
    bool particle = false;
    bool cparticle = false;
    bool stopped = false;
};

class World : public WorldState {
public:
    World() = default;
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    virtual ~World() = default;

    // PORT: the archive, as the renderer mounts it.
    virtual std::vector<std::string> archive_paths_in_listing_order(const std::string& dir,
                                                                    const char* suffix) const = 0;
    virtual std::string resolve_archive_path_quiet(const std::string& reference) const = 0;
    virtual std::string class_unit_path(const std::string& class_name,
                                        bool* console = nullptr) const = 0;
    virtual std::string archive_animation_in_dir(const std::string& dir, const std::string& state,
                                                 bool armed, int variation) const = 0;
    virtual std::string archive_path_in_dir(const std::string& basename,
                                            const std::string& dir) const = 0;
    virtual std::string archive_path_unique(const std::string& basename) const = 0;
    virtual std::vector<std::string> archive_paths_under(const std::string& dir,
                                                         const char* suffix) const = 0;
    /// The data files under `dir`: the archive's `.dat.adm`, as `archive_paths_under` orders
    /// them, then a mod's own `.dat` and `.dat.adm` the archive does not have.
    virtual std::vector<std::string> data_paths_under(const std::string& dir) const = 0;
    /// The same, the archive's in its listing order (`archive_paths_in_listing_order`).
    virtual std::vector<std::string> data_paths_in_listing_order(const std::string& dir) const = 0;
    /// A data file as either form: compiled `.adm`, or a mod's text `.dat`.
    static tl::formats::AdmFile parse_data_group(const uint8_t* data, size_t size);
    virtual bool console_game_data() const = 0;
    virtual std::string dir_of_asset(const std::string& name) const = 0;
    virtual tl::core::DungeonManager& loaded_dungeon_manager() const = 0;
    virtual std::vector<uint8_t> read_resource(const std::string& path) const = 0;
    virtual std::string resolve_archive_path(const std::string& reference) const = 0;
    /// Every mod's own copy of `path` (or, for a compiled name, of its text source), in the mods'
    /// order: what one mod's file would hide. Set by the renderer once the mods are mounted.
    std::function<std::vector<std::vector<uint8_t>>(const std::string&)> read_mod_copies;
    bool mod_has_file(const std::string& path) const {
        return read_mod_copies && !read_mod_copies(path).empty();
    }

    // PORT: the renderer's scene nodes, read and moved for the game (core/view_handles.h).
    virtual ogre::Vector3 view_position(const Ogre::SceneNode* node) const = 0;
    virtual ogre::Quaternion view_orientation(const Ogre::SceneNode* node) const = 0;
    virtual ogre::Vector3 view_derived_position(const Ogre::SceneNode* node) const = 0;
    virtual ogre::Quaternion view_derived_orientation(const Ogre::SceneNode* node) const = 0;
    virtual void view_set_position(Ogre::SceneNode* node, const ogre::Vector3& at) = 0;
    virtual void view_set_orientation(Ogre::SceneNode* node, const ogre::Quaternion& turn) = 0;
    virtual Ogre::SceneNode* view_parent_node(const Ogre::Entity* entity) const = 0;
    virtual tl::core::DungeonFiles archive_dungeon_files() const = 0;

    // PORT: what the game asks of the renderer's half.
    virtual bool add_map_portal(const ogre::Vector3& at, const std::string& warp, bool enabled) = 0;
    virtual void view_restore_automap(const tl::formats::LevelSaveState& state) = 0;
    virtual void view_store_automap(tl::formats::LevelSaveState& state) = 0;
    virtual void choose_player_attack_hand() = 0;
    virtual void fire_particle_trigger(const std::string& animation, size_t index,
                                       const tl::core::AnimationTag& tag) = 0;
    virtual void player_hide_hand_trails() = 0;
    virtual void player_shoot_tracer(const tl::core::Vec3& from, const tl::core::Vec3& to) = 0;
    virtual bool player_weapon_skill(const ProcTarget& target) = 0;
    virtual void player_weapon_trail_key(const tl::core::AnimationTag& tag) = 0;
    virtual void view_add_node_collision(Ogre::SceneNode* node, const std::string& path,
                                         int64_t group) = 0;
    virtual float view_animation_length(Ogre::Entity* entity, const std::string& name) const = 0;
    virtual void view_cast_shadows(Ogre::Entity* entity, bool on) = 0;
    virtual bool view_ready() const = 0;
    /// `animation`: the `DIE` animation the death chose, empty when the machine has none.
    virtual bool view_unit_falls(int64_t node, const std::string& animation) = 0;
    virtual Ogre::Entity* create_item_entity(const tl::core::ItemStats& item) = 0;
    virtual void draw_item_presence(Ogre::SceneNode* node, const tl::core::ItemPresence& p,
                                    const tl::core::ItemPresenceChange& change) = 0;
    virtual bool item_in_camera(Ogre::SceneNode* node, bool from_model = false) const = 0;
    virtual ogre::Vector3 item_view_point() const = 0;
    virtual void place_pet_beside_player() = 0;
    virtual tl::core::QuestEventUnit quest_event_unit(const std::string& name,
                                                      const std::string& unittype,
                                                      const ogre::Vector3& at,
                                                      const tl::core::QuestUnitTag& tag) = 0;
    virtual bool save_character(const std::string& path, bool force) = 0;
    virtual Ogre::Entity* view_spawn_unit_model(const std::string& unit,
                                                const std::string& mesh_path, int64_t id,
                                                const ogre::Vector3& at,
                                                const ogre::Quaternion& facing, float placed_scale,
                                                bool spawn_anim) = 0;
    virtual void spawned_item_state(int64_t id, int state) = 0;
    virtual void step_player(float delta, const tl::core::InputState& in) = 0;
    virtual void view_destroy_entity(Ogre::Entity* entity) = 0;
    virtual std::string view_entity_mesh(const Ogre::Entity* entity) const = 0;
    virtual void view_hide_death_screen() = 0;
    virtual ogre::Vector3 view_landed_size(Ogre::SceneNode* node, Ogre::Entity* entity) = 0;
    virtual Ogre::SceneNode* view_place_entity(Ogre::Entity* entity, const ogre::Vector3& at,
                                               const ogre::Quaternion& turn) = 0;
    virtual void view_restart_trigger_layout(int64_t node, const NodeMap* nodes) = 0;
    virtual void view_set_scale(Ogre::SceneNode* node, float scale) = 0;
    virtual void view_set_visible(const std::vector<Ogre::SceneNode*>& holders, bool on) = 0;
    virtual void view_show_minimap() = 0;
    /// `CBaseUnit::sphereCollision` for each of `ids`: `t[i]` the first `t` in [0, 1] unit `i`'s
    /// collision model is touched, above 1 if not; `drawn[i]` 0 where it is not drawn.
    virtual void unit_collider_sweeps(const std::vector<int64_t>& ids, const tl::core::Vec3& from,
                                      const tl::core::Vec3& to, float radius, std::vector<float>& t,
                                      std::vector<char>& drawn) = 0;
    virtual std::vector<Ogre::SceneNode*> place_particle_layout(
        const std::string& reference, const ogre::Vector3& at, const ogre::Quaternion& facing,
        Ogre::SceneNode* parent, ParticleStart start = ParticleStart::Now) = 0;
    virtual void view_aim_node(Ogre::SceneNode* node, const ogre::Vector3& at,
                               const ogre::Vector3& forward) = 0;
    virtual Ogre::SceneNode* view_create_node(const ogre::Vector3& at) = 0;
    virtual void view_destroy_node(Ogre::SceneNode* node) = 0;
    virtual bool view_held_item(const Ogre::Entity* held, ogre::Vector3& hand,
                                float& height) const = 0;
    /// For each list of holders: whether a system on one of them has not finished - or, where
    /// `by_updating` is set, is still updating (`CParticle::getNumberOfParticlesUpdating(true)`)
    /// - the lists whose `stop` is set stopping their systems' emission first.
    virtual void view_transients_alive(
        const std::vector<const std::vector<Ogre::SceneNode*>*>& lists,
        const std::vector<char>& stop, const std::vector<char>& by_updating,
        std::vector<char>& alive) = 0;
    virtual void add_text_event(const tl::core::TextEvent& event) = 0;
    virtual int breakable_within_view(float max_angle, float max_distance) const = 0;
    virtual bool skill_bone_world(const ViewUnit& unit, int attachment, ogre::Vector3& out) = 0;
    virtual bool skill_unit_forward(const ViewUnit& unit, ogre::Vector3& out) = 0;
    virtual void start_character_particle(const tl::core::UnitStats& unit, int slot,
                                          const ogre::Vector3& at, CharacterRef who) = 0;
    virtual void view_claim_particles(size_t mark, std::vector<Ogre::SceneNode*>& holders,
                                      std::vector<Ogre::SceneNode*>& line_holders) = 0;
    virtual size_t view_particle_mark() const = 0;
    virtual bool view_particles_alive(const std::vector<Ogre::SceneNode*>& holders) const = 0;
    virtual void view_ride_particles(size_t mark, Ogre::SceneNode* ride) = 0;
    virtual void view_stop_emission(const std::vector<Ogre::SceneNode*>& holders) = 0;
    virtual void view_stop_particles(const std::vector<Ogre::SceneNode*>& holders, bool loop) = 0;
    virtual void view_stretch_lines(const std::vector<Ogre::SceneNode*>& line_holders,
                                    const ogre::Vector3& start, const ogre::Vector3& end) = 0;
    virtual void view_translate(const std::vector<Ogre::SceneNode*>& holders,
                                const ogre::Vector3& by) = 0;
    virtual void play_effect_fx(tl::core::EffectManager& effects, const ogre::Vector3& centre,
                                const std::string& who) = 0;
    virtual bool play_sound(const std::string& name, const ogre::Vector3& at) = 0;
    virtual bool play_sound(const std::string& name, const ogre::Vector3& at, float volume_scale,
                            float radius_if_none, const std::string& voice_key,
                            bool volume_given = false) = 0;
    virtual void apply_script_events(const std::vector<tl::core::ScriptEvent>& events,
                                     NodeMap& nodes) = 0;
    virtual void close_ingame_ui() = 0;
    virtual void create_portals() = 0;
    virtual void delete_open_portals() = 0;
    virtual void destroy_particle_holders(std::vector<Ogre::SceneNode*>& holders) = 0;
    virtual void destroy_spawned_unit(Ogre::Entity* entity, int64_t id) = 0;
    virtual void log_scene_census(const char* when) const = 0;
    virtual void open_map_portal(const std::string& dungeon) = 0;
    virtual void open_modal(const std::string& title, const std::string& text) = 0;
    virtual bool play_scene_animation(Ogre::Entity* entity, const std::string& mesh_path,
                                      const std::string& animation, bool loops, float rate,
                                      bool optional = false,
                                      Ogre::AnimationState** started = nullptr, bool armed = true,
                                      std::string* played = nullptr, float blend = 0.0f,
                                      bool queue = false) = 0;
    virtual void quest_event(const std::vector<const tl::core::QuestDefinition*>& finished) = 0;
    virtual SceneUnit* scene_unit_of(tl::core::LevelScript* machine, int64_t node) = 0;
    virtual void write_entity_animation(EntityAnimation& animation) = 0;
    virtual void activate_item_slot(int index) = 0;
    virtual void apply_quest_item_removals() = 0;
    virtual void bag_item_added(tl::core::ItemStats& item) = 0;
    virtual void bag_item_removed(const tl::core::ItemStats& item) = 0;
    virtual bool cast_player_skill(const tl::core::KnownSkill& skill, bool report) = 0;
    virtual void catch_fish() = 0;
    virtual void close_pause_menu() = 0;
    virtual void close_quest_offer() = 0;
    virtual void close_waypoint_menu() = 0;
    virtual void dress_player(bool force = false) = 0;
    virtual bool equip_weapon_item(const tl::core::ItemStats& item, int location = -1) = 0;
    virtual bool fishing_animation_playing(const std::string& name) const = 0;
    virtual void follow_player_animation() = 0;
    virtual bool full_screen_menu_up() const = 0;
    virtual bool hand_dialog_items(const tl::core::QuestDefinition& quest,
                                   const tl::core::DialogLine& line) = 0;
    virtual bool identify_item_in(tl::core::Inventory& bag, size_t slot) = 0;
    virtual bool link_subject_animation(const std::string& name) = 0;
    virtual bool modal_dialog_open() const = 0;
    virtual tl::core::AnimationCatalog model_catalog(const std::string& mesh,
                                                     const std::string& dir,
                                                     size_t* missing = nullptr) = 0;
    virtual tl::core::MusicHost music_host(int64_t node) = 0;
    virtual bool open_gem_socketing() = 0;
    virtual bool open_spell_remove(bool pet_owner, size_t slot) = 0;
    virtual bool path_blocker_of(const tl::core::LevelScript::NodeRecord& n,
                                 PathBlocker& b) const = 0;
    virtual bool path_blocker_of(const tl::formats::CmpNode& node, PathBlocker& b) const = 0;
    virtual void play_fishing_animation(const std::string& name, float rate, bool once,
                                        const std::string& then) = 0;
    virtual bool play_music(const std::string& relative, bool loop, const char* because) = 0;
    virtual bool play_sound_2d(const std::string& name, const char* label = "voice") = 0;
    virtual void player_levelled_up() = 0;
    virtual tl::core::PortalLevel portal_level() const = 0;
    virtual void queue_tip(tl::core::ContextTip tip) = 0;
    virtual void refresh_game_hud() = 0;
    virtual void view_skill_effects_changed() = 0;
    virtual void refresh_waypoint_menu() = 0;
    virtual void release_quest_pets(const tl::core::QuestCompletion& done) = 0;
    virtual void reward_player(const tl::core::QuestCompletion& done) = 0;
    virtual void set_help_open(bool open) = 0;
    virtual void set_interactive_menu_visible(bool on) = 0;
    virtual void stop_fishing() = 0;
    virtual void sync_worn_attachments(int weapon_location) = 0;
    virtual bool ui_in_cinematic() const = 0;
    virtual void unequip_weapon() = 0;
    virtual bool unit_in_view(const tl::core::LevelScript& machine, int64_t node) = 0;
    virtual int unit_machine(const std::string& unit, int64_t& node) const = 0;
    virtual void update_automap_icons() = 0;
    virtual void update_character_preview(float delta) = 0;
    virtual void update_menu() = 0;
    virtual void update_npc_icons() = 0;
    virtual bool use_item_on_pet(const tl::core::ItemStats& item, int stack, bool by_player,
                                 tl::core::ItemUseResult& used) = 0;
    virtual bool use_player_item(size_t slot, bool from_tray = false) = 0;
    virtual bool wear_player_item(size_t slot) = 0;
    virtual bool logic_trace_on() const = 0;
    virtual bool place_particle(const tl::formats::CmpScene& layout,
                                const tl::formats::CmpNode& node, const ogre::Vector3& position,
                                const ogre::Quaternion& orientation) = 0;
    virtual void view_automap_add_tile(int type, const ogre::Vector3& at, const ogre::Vector3& dir,
                                       bool a, bool b) = 0;
    virtual void view_automap_player(const ogre::Vector3& at) = 0;
    virtual void view_place_viewer(const tl::formats::CmpScene& layout,
                                   const ogre::Vector3& origin) = 0;
    virtual void view_place_meshes(const std::vector<MeshPlacement>& placements,
                                   size_t& animated_props, size_t& idled) = 0;
    virtual void view_own_trigger_particles(size_t before, int64_t node, const NodeMap& sink) = 0;
    virtual void view_settle_link_particles(size_t systems_before, int64_t owner,
                                            const std::string& reference,
                                            const ogre::Vector3& origin,
                                            const ogre::Quaternion& basis,
                                            LinkParticleSettle what) = 0;
    virtual bool view_caching_particles() const = 0;
    virtual unsigned long view_milliseconds() const = 0;
    /// `CGameUI::updateLoading`: the loading screen refreshed between
    /// `CLevel::populate`'s phases, ten times a floor, at the console's own points. Nothing by default.
    virtual void view_update_loading() {}
    virtual void view_automap_create() = 0;
    virtual size_t view_level_particle_node(size_t systems_before) = 0;
    virtual void view_stand_subject(const ogre::Vector3& at, const ogre::Vector3& facing) = 0;
    virtual void bake_static_scenery() = 0;
    virtual void view_prepare_mesh(const std::string& mesh_path) = 0;
    virtual bool ground_under(const ogre::Vector3& from, float& surface_y) const = 0;
    virtual void logic_trace_text(const char* text) = 0;
    virtual void flush_logic_trace(tl::core::LevelScript& machine) = 0;
    virtual void apply_environment(const std::string& layout_path) = 0;
    virtual tl::core::SoundBankHost node_sound_host() = 0;
    virtual std::vector<std::pair<std::string, float>> animation_catalog(
        const std::string& unit) = 0;
    virtual float scene_animation_length(const std::string& unit, const std::string& animation) = 0;

    /// `onClick` on the menu's unit for the
    /// player's talk: its flag back and the broadcasts on it; `OK` on a
    /// `[COMPLETE]` line completes the quest between them.
    void talk_menu_closed(int machine, int64_t node, int32_t menu_event, bool complete_line,
                          const std::string& quest);
    /// The player's talk window closing with `menu_event` (2 `OK`, 3 `Accept`, 4 `Decline`):
    /// put away, `onClick`'s broadcasts, and `OK` on `[COMPLETE]` completes the quest.
    void finish_talk_window(int32_t menu_event);
    /// The player's forward, gait and whether they are going anywhere, for a
    /// machine whose characters include the player's pets.
    void tell_player_motion(tl::core::LevelScript& machine);
    /// Finish a quest the player is carrying and pay what it owes.
    bool complete_quest(const std::string& name);
    /// `completeQuest` as the dialogue's `OK` on a `[COMPLETE]` line runs it:
    /// once more on a quest already complete (`QuestLog::complete_by_dialog`).
    bool complete_quest_by_dialog(const std::string& name);
    /// What follows any completion here: the payout, the gates, the icons.
    void quest_completed(const std::string& name);
    /// Carry out every queued `CQuestManager::completeQuest`:
    /// journal, `[REWARD]` (`rewardPlayer`), "Quest Complete", take-backs.
    void pay_quest_completions();
    /// A quest's reward in `calculateRewards`' order:
    /// `[UNITREWARD]` units, then `TREASURECLASS` (`tl::core::quest_treasure_roll`).
    std::vector<std::string> quest_reward_items(const tl::core::QuestDefinition& quest, int level,
                                                int32_t seed);
    /// Raise the two-line banner and play the narrator, on accept or on completion.
    void announce_quest(const tl::core::QuestDefinition& quest, bool complete);
    /// The first item `quest_reward_items` hands over (what the windows draw); empty: none.
    std::string quest_reward_item(const tl::core::QuestDefinition& quest, int level, int32_t seed);
    /// A quest's seed from an NPC (`CQuest +0x1c`, drawn by `initializeQuestWithNPC`
    /// on the volatile stream, `QuestLog::offered`).
    /// INVENTED: a quest no NPC initialised gets a seed from its GUID.
    int32_t offer_seed(const tl::core::QuestDefinition& quest) const {
        const tl::core::QuestLog::Offered* rolled = quest_log.offered(quest);
        if (rolled != nullptr && rolled->seed != -1) return rolled->seed;
        return static_cast<int32_t>(static_cast<uint32_t>(quest.guid) ^ 0x9e3779b9u);
    }
    /// The level a quest's rewards are worked at (`CQuest +0x1ec`, `QuestLog::offer_level`),
    /// else `quest_level_for_offer`.
    int quest_level_of(const tl::core::QuestDefinition& quest) const {
        return quest_log.offer_level(quest, quest_level_for_offer);
    }
    /// The quest given to the player unless its log is full for it.
    bool take_quest(const tl::core::QuestDefinition& quest, int level, uint32_t seed) {
        return !quest_log.log_full_for(quest) && quest_log.accept(quest, level, seed, stat_graphs);
    }
    /// Fill an accepted quest's `reward_item` / `reward_icon`.
    void resolve_reward_item(tl::core::QuestEntry& entry);
    /// Say so when a quest that a *gate in this level* was waiting on changes.
    void report_gates_waiting_on(const std::string& quest);
    /// The `PORTRAIT` a unit authors (a name in `media/UI/itemicons/`); empty for none.
    std::string portrait_of(const std::string& unit);
    /// What every machine of the level is told about its characters' ranges
    /// and the camera (`set_unit_*_range`, `set_view_test`).
    void configure_unit_ranges(tl::core::LevelScript& machine);
    /// The level a warp leaves (`core/warp.h`): its dungeon, that dungeon's
    /// `PARENT_DUNGEON` and the depth.
    tl::core::WarpOrigin warp_origin() const;
    /// The pad on the open menu: the stick moves the highlight, A is
    /// `onClick` on the row, B closes it.
    void drive_waypoint_menu(const tl::core::InputState& in, bool confirm_edge, bool skip_edge);
    /// `CGameUI`'s Help toggle.
    void toggle_help();
    /// The pad on Help and its `vfunc_9`.
    void drive_howto_menu(const tl::core::InputState& in, float dt, bool start_edge,
                          bool back_edge);
    /// Any of the screens Start leads to is up - the Pause Menu, Options or the
    /// debug menu - and the level stands still under it.
    bool start_menu_up() const {
        return debug_menu_open || pause_menu.open || settings_menu.open || !settings_menu.finished;
    }
    /// `CGameClient::getIsPaused` on the console: a full-screen menu
    /// covers a side (`rightCovered`, `leftCovered`) or a modal menu
    /// is up. The PC's needs both sides covered.
    bool game_paused() const { return full_screen_menu_up() || modal_dialog_open(); }
    void load_context_tips();
    /// `CCharacter::equipmentPickedUp` for the pet: the
    /// tip when its bag is full.
    void queue_pet_bag_tip();
    /// A row's caption: the depth's template's `LEVELNAME`, with ` - Floor N`
    /// when it shows floor numbers (`setOpen`).
    std::string waypoint_label(const tl::core::WaypointRow& row,
                               const tl::core::DungeonFiles& files);
    /// `warpLevels(dungeon, 0, depth, true, L"", false)`.
    void warp_to_waypoint(const tl::core::WaypointRow& row);
    void load_cinematics();
    /// `CCinematicMenu::update`, the music half, every frame.
    void step_cinematic_music(float delta);
    void ensure_skill_index();
    void ensure_player_skill_files();
    void ensure_effect_data();
    /// What `CEquipment::unitInit`'s rolls read (`core/item_roll.h`).
    tl::core::ItemRollContext item_roll_context();
    /// What an item's own effect manager is built with: the effect and graph
    /// tables and no owner (an item is not a character).
    const tl::core::EffectManager::Context& item_effect_context();
    /// Hands the player's inventory (and the pet's bag) the tables its effect
    /// half reads, and recalculates (`Inventory::set_effect_context`).
    void refresh_inventory_effects();
    /// `CCharacter::getEffectValue`'s two halves for the player: its own
    /// manager and its inventory's totals (`core/character_effects.h`).
    tl::core::CharacterEffects player_total() const {
        return tl::core::CharacterEffects(&player_effects, &player_inventory);
    }
    void load_recharge_rates();
    /// `CCharacter::updateHP` and
    /// `updateMana` for the player.
    void update_player_meters(float dt);
    /// `CCharacter::modifyMana` on the player: god mode
    /// sets `maxMana()`; else added, capped at `maxMana()`, floored at 0.
    void modify_player_mana(float delta);
    /// `player_health.max` and the sheet's mana maximum from `maxHP()` and
    /// `maxMana()` (`CharacterProgression::max_hp_of`/`max_mp_of`).
    void refresh_player_maxima();
    /// An item's `RESPEC`: `CPlayer::resetSkills`.
    void respec_player();
    /// A on the bag's own tray (past the vendor pages): gem, fish, useable
    /// item, else `wear_player_item`.
    void bag_default_action(int slot);
    /// the first Identify Scroll of the player's
    /// bag, then of a pet's that is not in town, used on the item at `slot`.
    bool identify_player_item(size_t slot);
    /// `CCharacter::getEffectValue`'s two halves for the pet.
    tl::core::CharacterEffects pet_total() const {
        return tl::core::CharacterEffects(&pet.effects(), &pet.bag());
    }
    /// The pet's manager's tables, and its spell book's effect totals.
    void refresh_pet_effects();
    /// The pet as the item use path reads it.
    tl::core::ItemUseTarget pet_use_target();
    /// What every tray's row fill asks of the player.
    tl::core::TrayRowContext tray_row_context();
    /// `canUse` and `useOnTarget` of the pet's copy at `slot`
    /// on the pet, then (the copy away when spent).
    bool use_pet_item(size_t slot);
    /// the function's ISA 0x7c branch for the fish at `slot` of `holder`: with a pet
    /// (`+0x5ac`), used on it, then.
    bool feed_pet_from(tl::core::Inventory& holder, size_t slot);
    /// for the spell scroll at `slot`: the learn, or with the slots full
    /// (two for a pet, four else) the replace dialog.
    void learn_spell_from_tray(bool pet_owner, size_t slot);
    /// The pet's spells relearned into an emptied book (`learnSpell`,
    /// as `applySaveState` does).
    void relearn_pet_spells(const std::array<std::string, 4>& names,
                            const std::array<int32_t, 4>& levels);
    /// The Xbox in-game dispatch's slot loop over the six hotbar
    /// buttons, each given as pressed this frame and held since the last.
    void step_hotbar(const std::array<bool, 7>& pressed, const std::array<bool, 7>& held,
                     float stick_x = 0.0f, float stick_y = 0.0f);
    /// The dispatch's release test: with no face button pressed and a skill performed, it
    /// is let go when its button is up (`core::hotbar_lets_go`).
    void release_held_skill(const std::array<bool, 7>& pressed, const std::array<bool, 7>& held,
                            bool face_pressed);
    /// `CPlayer::attemptToStopPlayerSkill(bool)`.
    void attempt_to_stop_player_skill(bool stop_path);
    /// `CCharacter::performingSkill` for the player.
    bool player_performing_skill();
    /// where the pad aims a skill (stick direction or
    /// facing; distance by `RANGEMIN`..`RANGE`, or a trigger's pull).
    tl::core::Vec3 pad_skill_point(const tl::core::KnownSkill& skill, float stick_x, float stick_y,
                                   float left_trigger, float right_trigger) const;
    /// `CCharacter::setTargetDirection`: the point
    /// on the ground (height 0) and the level direction to it.
    void set_player_target_direction(const tl::core::Vec3& point);
    /// `CCharacter::turnTowardPosition` for the player,
    /// with `turnToward`.
    void turn_player_toward(const tl::core::Vec3& at, float dt);
    /// `CCharacter::castSkill` for a position skill (types 1, 5,
    /// 6, 11): grounded, target let go, within `RANGE`, walked back for `REQUIRES_PATHABLE`.
    /// Writes `caster.target_position`.
    void aim_position_skill(const tl::core::KnownSkill& skill, tl::core::SkillCaster& caster);
    /// `CPlayer::unitInit`'s default item slots.
    void bind_default_item_slots();
    /// `CSkillManager::startPassiveSkills` for the player,
    /// when the book is built and when a skill's level changes.
    void start_player_passives();
    /// `CCharacter::updateSkill(float)` -> `CInventory::updateSkillManagers`,
    /// inline in `CCharacter::updateAnimation`:
    /// each location's item runs `CSkillManager::update`.
    ///
    void update_worn_skill_managers(float delta);
    /// The runner a skill of that name runs on (the player's, else a worn item's), where its
    /// later events go.
    tl::core::SkillRunner& skill_runner_running(const std::string& name);
    /// Rebuild `player_skills` for `player_class` at the class's starting
    /// levels, points and mapping cleared.
    void build_player_skills();
    /// A saved item made again (`createUnit` with `from_save`, then its
    /// `applySaveState`); nothing for a name the item index does not know.
    std::optional<tl::core::ItemStats> item_from_save(const tl::formats::ItemSaveState& item);
    /// One held item's `CItemSaveState` (`CEquipment::fillSaveState`,
    /// core/item_save.h); `+0x5c`: the slot is an equip location.
    tl::formats::ItemSaveState item_save_state(const tl::core::InventorySlot& held, int32_t slot);
    void set_big_text(const std::string& text, float seconds);
    void set_small_text(const std::string& text, float seconds);
    /// `CGameClient::loadLevel`: the level's name big and "Floor N" small, for 1.0
    /// second.
    void raise_arrival_banner();
    /// `CParticlePreloader::GetParticle` by name: its `FILE` in
    /// `media/particles/preload.dat`, empty for none.
    std::string preloaded_particle_file(const std::string& name);
    void set_talk_target(const std::string& unit);
    void clear_talk_target();
    bool script_dialog_open() const { return dialog_machine() != kNoMachine; }
    /// `LOOK_AT_PLAYER` (`CQuestDialogMenu::setNPC`).
    void speaker_looks_at_player(const std::string& unit);
    /// `CCharacter::setDestination` for the held player:
    /// stopped, `attack()`, the point kept (`+0x1c0`). (0, 0) is the point already there.
    void set_player_destination_held(float x, float z);
    /// `CConsole::executeCommandLine`'s `GODSPEED`: `GOD`
    /// flips `+0x790`, `SPEED` (`addSpeedToPlayer`) toggles
    /// `PLAYER_SPEED_UP`. DIVERGENCE: both set to `on`, and no `CPlayer::setCheater`.
    ///
    void set_godspeed(bool on);
    /// DIVERGENCE (2026-10-03, for testing skills): the LEVEL 100 row:
    /// `CConsole::executeCommandLine`'s `LEVELUP` per level
    /// (`CCharacter::awardExperience`) up to `level`.
    void level_up_to(int level);
    /// DIVERGENCE (2026-10-11, for testing skills): the port's own rest of the row:
    /// the best droppable staff (ISA 0x3d) for the level, its requirements met from the
    /// unspent points, worn as A wears it (`wear_item`).
    void outfit_debug_staff();
    /// The LEVEL row whole: `level_up_to`, then `outfit_debug_staff`.
    void debug_level_row(int level);
    void ensure_sound_index();
    const SoundDefinition* sound_by_name(const std::string& name);
    const SoundDefinition* sound_by_guid(const std::string& guid);
    /// Which recording of a definition sounds: the sound system's choice, not a game draw.
    // PORT: the port's own generator, apart from the game's two; seeded from the clock.
    const std::string& pick_recording(const SoundDefinition& definition);
    /// Say one of the character's voice lines (`CSoundBank::queueGlobalSample`). The slot is
    /// the player's bank's even for the pet's lines, as `modifyHP` reads it.
    /// `delay` in seconds; `volume` 0: the sound's own. False when the class authors nothing
    /// for the slot or the line is not queued.
    bool play_voice(tl::core::SoundSlot slot, float delay = 0.0f, float volume = 0.0f);
    /// `CSoundBank::queueGlobalSample(slot, volume, delay)`
    /// on a bank whose `slot` names `name`.
    bool queue_global_sample(tl::core::SoundSlot slot, const std::string& name, float volume,
                             float delay);
    /// Play one of the interface's own sounds (banks the menus build, named by definition;
    /// core/sound_bank.h).
    bool play_ui(tl::core::InterfaceSound sound);
    /// The definition name in a slot, or "-", for the log.
    const char* voice_or_none(tl::core::SoundSlot slot) const;
    /// Advance `play_voice()`'s queue (`CSoundManager::update`).
    void step_voices(float delta);
    /// `CQuestDialog::playDialogSound` / `stopDialogSound`: the one dialogue
    /// recording that is sounding, by its voice key.
    void play_dialog_sound(const std::string& name);
    void stop_dialog_sound();
    /// The decoded sample behind one archive path, decoding it the first time.
    tl::core::SamplePtr sample_for(const std::string& archive_path);
    /// Start the music (the rules' `MUSIC`) and the ambience (a layout `Sound` node) this
    /// level authors, after it is placed.
    void start_level_audio(const std::string& layout_path);
    void music_node_input(const tl::core::ScriptEvent& event);
    void step_music_objects(float delta);
    /// The quest dialog's share, taken or given back.
    void set_quest_dialog_interactive(bool on);
    const ParticleFileExtras* particle_extras(const std::string& reference);
    /// Log each `Puzzle Input`'s order as its machine is built (`CPuzzleRandomizer` +0x6c,
    /// shuffled on the volatile generator).
    void log_puzzles(const tl::core::LevelScript& script);
    /// The two hands as `attackRange`, `selectAttack` and `attack` read them.
    tl::core::HeldHands player_held_hands();
    /// `CCharacter::selectAttack(mode)` for the player;
    /// nothing while a swing has more than 0.2 s left.
    void select_player_attack(int mode);
    /// Put a cast's cue on the character.
    void play_skill_cue(const tl::core::SkillAnimationCue& cue);
    /// `CCharacter::setTarget` for the living
    /// player: the target, and the target item let go.
    void set_player_target(int64_t node) {
        player_target_node = node;
        player_skill_item = 0;
    }
    // PORT: the level a placed unit was made at (`CLevel +0x1a8` for a populated floor's,
    // `spawn_unit()`), else the player's `character_level`.
    int effective_monster_level(int64_t node) const {
        const auto found = unit_spawn_level.find(node);
        if (found != unit_spawn_level.end() && found->second.level > 0) return found->second.level;
        return character_level;
    }
    /// Whether this node was promoted at spawn through `CHAMPIONSPAWNCLASS`
    /// (`apply_champion_curves()`; apart from `UnitStats::is_champion`).
    bool spawn_marked_champion(int64_t node) const {
        const auto found = unit_spawn_level.find(node);
        return found != unit_spawn_level.end() && found->second.champion;
    }
    void load_game_speed();
    /// `CCharacter::rollAttack`: a player blow leaving the target dying or dead
    /// slows the game for half a second.
    void slow_on_player_kill(bool target_dead);
    /// -1 for a unit the table has not got.
    int64_t unit_guid_of(const std::string& name);
    /// Empty for a guid the table has not got.
    std::string unit_of_guid(int64_t guid);
    /// A on it: `CCharacter::attack` on a `MIMICIDLE`
    /// target calls `reactToDamage(player, true)`. DIVERGENCE: ranged, the original walks to
    /// it instead.
    void disturb_targeted_mimic();
    /// Whether a `Unit Trigger`'s prop is an `ENCHANTSHRINE` (ISA 0x9d).
    bool is_enchant_shrine(const std::string& unit);
    /// Whether a `Unit Trigger`'s prop is a `FISHING HOLE` (ISA 0x7a).
    bool is_fishing_hole(const std::string& unit);
    /// One frame of AI states 0x20..0x22, and the two buttons in 0x21.
    void step_fishing(float delta, bool catch_press, bool stop_press);
    /// The name a quest objective prints (`CQuestUnitData::getUnitName`),
    /// off the item and unit tables.
    tl::core::QuestUnitNamer quest_unit_namer();
    /// How fast a swing plays, as a multiple of real time (the blow's frame sets the cadence).
    // INVENTED: `ATTACKSPEED` over 100; the console's attack animation speed is not
    // transcribed here.
    float swing_rate() const {
        return player_stats.attack_speed > 0.0f ? player_stats.attack_speed / 100.0f : 1.0f;
    }
    /// What the character should be playing this frame: its swing while one is
    /// running, and otherwise whatever its movement says.
    std::string wanted_player_animation() const {
        if (player_death.dying) {
            return player_death.animation.empty() ? player_animation : player_death.animation;
        }
        if (fishing_state != tl::core::FishingState::Off && !fishing_animation.empty()) {
            return fishing_animation;
        }
        // An `Animation Controller` holding the player (`setAIPlayAnimation`,
        // state 0x10): the scene's animation, over whatever it was doing.
        if (!scripted_player_animation.empty()) return scripted_player_animation;
        if (skill_playback.active) return skill_playback.animation;
        if (swing.active) return swing.animation;
        return player.animation();
    }
    void load_stat_graphs();
    void load_camera_shake_graphs();
    size_t load_graph_directory(const std::string& directory, tl::core::StatBook& book) const;
    /// Read every quest under `media/Quests/` into `quest_book`, once.
    void load_quest_book();
    /// A unit's data group: the file over its `BASEFILE` chain
    /// (`CUnitResourceList::parseChildFile`, core/unit_resource.h); a missing base is empty.
    /// `first_root` is tried before each `BASEFILE` (`xbox/`). With a `group`, also the four
    /// values `CUnitResourceList::loadFile` appends.
    tl::formats::AdmFile read_unit_file(const std::string& path,
                                        const std::string& first_root = std::string(),
                                        const tl::core::ResourceGroup* group = nullptr) const;
    /// The file a unit's link names (read_unit_file), empty for none.
    std::string resolve_unit_link(const std::string& wanted, const std::string& first_root,
                                  bool is_first) const;
    const tl::core::AnimationManifest& manifest_for(const std::string& mesh_path);
    /// Read the playable classes, once.
    void load_player_classes();
    /// A mod's class's portrait, from its mod's `charactercreate.layout`.
    void find_mod_class_portrait(tl::core::PlayerClass& entry) const;
    /// `CEquipment::calculateCombatStats` for the weapon in
    /// a hand: its attack (`+0x238` right, `+0x23c` left), or none.
    tl::core::UnitStats::WeaponAttack weapon_attack_of(const std::string& item_name,
                                                       tl::core::Hand hand = tl::core::Hand::Right);
    /// `CCharacter::createDefaultEquipment`, spawn-class
    /// half: `SPAWNRIGHTHAND` then `SPAWNLEFTHAND`, one `g_Rand` roll each.
    void roll_unit_equipment(const std::string& unit, tl::core::LevelScript::UnitCreation& made);
    /// What `CCharacter::createDefaultEquipment` makes a machine's
    /// characters' equipment with.
    tl::core::LevelScript::UnitEquipment unit_equipment_hooks();
    void sync_equipped_weapon();
    /// `CUnitThemes::reload`, once
    /// (`CUnitThemes::CUnitThemes(folder)`).
    void load_unit_themes();
    /// Whether `unit` is made as a `CBreakable` (`CREATEAS: BREAKABLE` along `BASEFILE`;
    /// `CResourceManager::createNewItem`).
    bool is_breakable_unit(const std::string& unit);
    bool non_selectable_prop(const std::string& unit);
    /// `CTriggerUnit::update`'s collision rule on one: in while active, out of
    /// range out (only while `COLLIDEABLE`, i.e. not made `Not Collidable`).
    void trigger_range_collision(int64_t key, bool active);
    /// `generateMerchantInventory` into a
    /// fresh merchant-shaped inventory for `node`.
    void stock_merchant(MerchantState& state, int64_t node, const tl::core::UnitStats& stats);
    /// The merchant as `core/merchant.h`'s stock roll reads it.
    tl::core::MerchantCharacter merchant_character(MerchantState& state, int64_t node,
                                                   const tl::core::UnitStats& stats);
    /// Run `fn` with the stock roll's context (its pointers live only as long).
    void with_merchant_stock_context(
        const std::function<void(const tl::core::MerchantStockContext&)>& fn);
    /// `CLevel::populateNewMerchantsInventory`, as
    /// `CGameClient::restoreLevelState` ends.
    void populate_new_merchants();
    /// `CCharacter::applySaveState`'s
    /// merchant half, for a merchant the level state brings back.
    void restore_merchant(const tl::formats::CharacterSaveState& saved);
    void load_recipes();
    /// Y on a Transmuter's screen:.
    void transmute_selected();
    /// What reads off the player's level.
    tl::core::PetDepartureLevel pet_departure_level() const;
    /// `CCharacter::applySaveState`'s inventory half for a
    /// restored Transmuter.
    void restore_combiner(const tl::formats::CharacterSaveState& saved);
    tl::core::CreateLootUnit loot_unit_maker();
    /// `CQuestManager::populate` for the level just built.
    void populate_quests();
    /// An item's `+0x170` / `+0x178` as a tag; empty for -1.
    tl::core::QuestUnitTag quest_tag_of_item(const tl::core::ItemStats& item);
    /// The `QUEST_GUID` of the quest a tag names, -1 for none.
    int64_t quest_guid_of(const tl::core::QuestUnitTag& tag);
    void wire_bag_item_events();
    /// The player's blow on `target`: `rollAttack`, held weapon or bare-handed; sets
    /// `last_blow_critical`. `damage_scale` and `soak_scale` are `rollAttack`'s two floats.
    float player_blow(int64_t target, float damage_scale = 1.0f, float soak_scale = 1.0f);
    /// What `CCharacter::rollAttack` puts on the one struck
    /// after `applyDamageEffects`: `KNOCKBACK`, `DEGRADEARMOR` and the attacker's
    /// `TRANSFER` effects. Null `target_machine` and `target_node` 0: the player. `from`/`to`:
    /// the knock-back's direction.
    void roll_attack_effects(const tl::core::AttackEffectSources& sources, float knock_back,
                             float degrade, const tl::core::Vec3& from, const tl::core::Vec3& to,
                             tl::core::LevelScript* target_machine, int64_t target_node,
                             const tl::core::EffectSource& attacker);
    /// The same for a placed character's blow: its manager, the pet's bag for the player's
    /// pet, and its `KNOCK BACK` and `DEGRADE ARMOR`.
    void unit_attack_effects(int64_t attacker_node, tl::core::LevelScript* target_machine,
                             int64_t target_node);
    /// `CCharacter::getDmgToReflectFromMissile` of the player (0), the pet,
    /// or a placed character: the share sent back.
    float missile_reflect_share(int64_t unit);
    /// A skill event's caster's level (X360 `+0xf0`) for `CEffect::setOwner`'s
    /// `USEOWNERLEVEL`: the placed character's, or the player's for null; empty if unrun.
    std::optional<int32_t> event_caster_level(tl::core::LevelScript* machine, int64_t node) const {
        if (machine == nullptr) return player_character.level();
        int level = 0;
        if (!machine->unit_level(node, level)) return std::nullopt;
        return level;
    }
    /// How much life a placed unit has, creating the entry the first time.
    Health& health_of(int64_t node, const std::string& unit);
    /// A floor-rolled level (`unit_spawn_level`) handed to the machine's `setLevel`.
    void apply_spawn_level(int64_t node) {
        const auto found = unit_spawn_level.find(node);
        if (found != unit_spawn_level.end() && found->second.level > 0) {
            script.set_unit_level(node, found->second.level);
        }
    }
    /// `CCharacter::applyDamage`, its tail: a
    /// living victim answers with `DAMAGE REFLECTION` (0x30) plus `PERCENT DAMAGE REFLECTED`
    /// (0x20), through the attacker's armour. Node 0 is the player.
    void reflect_damage(const tl::core::CharacterEffects& victim, int64_t victim_node,
                        const std::string& victim_name, float amount, int64_t attacker);
    /// `CCharacter::applyDamageEffects` on a placed unit or
    /// the player (null): the elemental part may leave a `DAMAGEEFFECT*` affix.
    void apply_damage_status(tl::core::LevelScript* machine, int64_t node, float amount,
                             const std::array<int, tl::core::kDamageTypeCount>& by_type, int parts,
                             int first_type, bool forced, int attacker_level,
                             const tl::core::EffectSource& attacker);
    /// The learned spells onto the script's pet (`learnSpell`).
    void give_pet_spells_to_script();
    /// Copy what the level script says about the pet into `pet`.
    void sync_pet_mirror();
    /// Whether this level is the town, where the pet has nowhere to go.
    bool in_town() const;
    tl::core::Vec3 populate_open_position(const tl::core::Vec3& at, float radius);
    /// A unit's `MINION_SPAWNCLASS` rolled on `g_Rand` as the level takes it in.
    std::vector<std::string> roll_unit_minions(const std::string& unit, int level);
    /// Both, for a character already in the level.
    void spawn_unit_minions(int64_t master, int64_t owner);
    void make_populated_champion(int64_t id, const std::string& unit);
    std::vector<tl::core::FormationChoice> formation_choices(const std::string& table) const;
    /// `CGameClient::loadLevel`'s quest pass for a dungeon depth.
    void apply_floor_quests(const tl::core::DungeonDef& def, uint32_t depth);
    void load_tileset();
    const tl::formats::CmpScene* layout_file(const std::string& path);
    /// How long a `SOUND` definition plays, in seconds; 0 when it names nothing.
    float sound_length(const std::string& name);
    /// `CCharacter::castSkill`: the
    /// point a unit's position skill is cast at, into `unit_cast_caster`.
    void aim_unit_position_skill(tl::core::LevelScript& machine, int64_t node,
                                 const tl::core::KnownSkill& skill);
    size_t set_path_blocker_cells(const PathBlocker& b, bool on);
    /// Every blocker that is on taken off the map (a level reset).
    void lift_path_blockers();
    /// `CBaseUnit::addToAvoidanceMap` for every
    /// `ISA(INTERACTABLE)` `Unit Trigger` placed (`door_trigger_nodes`, drained): its collision
    /// model into the map grid; recorded in `door_avoidance_added` for `TriggerState`.
    void register_door_avoidance();
    /// Load the authored numbers of every unit this layout places.
    void load_unit_stats(const tl::formats::CmpScene& layout, const std::set<int64_t>& gated);
    /// Read one unit's authored numbers into the book; false when nothing is called that.
    bool index_unit(const std::string& name);
    /// This unit's authored numbers, or the defaults when nothing loaded them. A reference
    /// into the book: good until the book is cleared.
    const tl::core::UnitStats& stats_for_unit(const std::string& unit) const {
        static const tl::core::UnitStats kDefaults{};
        if (const tl::core::UnitStats* found = unit_book.find(unit)) return *found;
        return kDefaults;
    }
    /// `CCharacter::setAlignment`: red outline and highlight for a hostile
    /// (`+0x280` 2 or 4) unless a mimic (`ISA 0xa7`).
    bool outline_hostile(const tl::core::UnitStats& stats) const {
        return stats.hostile() && !unit_types.isa(stats.unittype, tl::core::unit_type::kMimic);
    }
    // PORT: whether a unit is on the hostile side (`unit_side`).
    bool unit_hostile(int64_t node, const tl::core::UnitStats& stats) const {
        return unit_side(node, stats) == tl::core::UnitStats::Alignment::Hostile;
    }
    /// Index every monster unit by its `NAME`, once.
    void ensure_monster_index();
    /// Resolve a `Unit Spawner`'s `RESOURCE` to a unit name (a unit, or a `Spawn Class` table
    /// rolled on `g_Rand`); empty when it cannot.
    std::string resolve_spawn_resource(const std::string& group, const std::string& resource,
                                       int depth = 0, int32_t level = 0, uint32_t flags = 0);
    /// The spawn class whose `NAME` is `name` (`spawn_class_paths`), parsed
    /// against `context`; empty when there is none.
    std::optional<tl::core::SpawnClass> load_spawn_class(
        const std::string& name, const tl::core::SpawnContext& context) const;
    /// Ids for released items, from the same range as spawned characters.
    int64_t next_spawned_id();
    /// Index every prop unit by its `NAME`, once (`media/units/props/`, apart from monsters).
    void ensure_prop_index();
    /// A prop's `RESOURCEDIRECTORY` + `LAYOUTFILE` + `.LAYOUT`; empty for a
    /// prop with no layout body.
    std::string prop_layout_path(const std::string& unit);
    /// Resolve a `Unit Trigger`'s `MODEL` to its geometry, or false when
    /// nothing under `media/units/props/` is called that.
    bool trigger_prop(const std::string& unit, TriggerProp& out);
    /// `unitInit`'s states for a new machine's `Unit
    /// Trigger`s (`LevelScript::init_unit_trigger`).
    void init_unit_triggers(tl::core::LevelScript& machine);
    /// What `CTriggerUnit::unitInit` loads as the unit is made: its
    /// skills' event files, its `LAYOUTFILE`, its state layouts.
    /// `skills_rolled`: the population made it and rolled its `[SKILL]`s already.
    /// What `CBreakable::unitInit` loads after its skills: its interact
    /// particle's file and its interact sound.
    void breakable_unit_loads(const tl::core::BreakableData& data);
    void trigger_unit_loads(const std::string& unit, const TriggerProp& prop,
                            bool skills_rolled = false);
    /// `CBaseUnit::unitInit`'s `[SKILL]`s for a prop (a breakable's or a chest's traps).
    void roll_prop_skills(const std::string& unit);
    /// A merchant the population makes, stocked as it is made (`made_merchants`).
    void stock_made_merchant(int64_t handle, const std::string& unit);
    void place_made_merchant(int64_t handle, int64_t node);
    /// A placed layout's enabled `Path Node Occupation` nodes into `loaded_path_blockers`.
    void collect_path_blockers(const tl::formats::CmpScene& layout, const std::set<int64_t>& gated,
                               const ogre::Vector3& origin, const ogre::Quaternion& basis);
    /// A floor's rooms activated after its population, then its triggers' marks.
    void activate_deferred_rooms();
    /// `loaded_path_blockers` onto the map after the floor's bake.
    void lay_loaded_path_blockers();
    /// The floor's LEVELPARTICLE layout, after its population.
    void place_level_particle(const std::string& level_particle_ref,
                              std::vector<std::pair<std::string, ogre::Vector3>>& placed_pieces);
    /// `CLayout::loadLayoutFile` of a file not placed as it is loaded:
    /// counted and seeded, its groups rolled, its links loaded. True when it made a `Timeline`.
    bool count_layout_load(const std::string& reference);
    /// `rollRandomGroups` over a file's Groups: the nodes not created; `created`, when given, the
    /// groups made, in the order the load makes their objects.
    std::set<int64_t> roll_layout_groups(const tl::formats::CmpScene& layout,
                                         std::vector<int64_t>* created = nullptr);
    const std::set<int64_t>* load_particle_file(const std::string& path,
                                                bool* added_sample = nullptr);
    std::string layout_archive_path(const std::string& reference) const;
    /// `DAMAGE_REQUIRES_LOS` (`CSkillEvent +0x14a`): the level stands between caster and
    /// target (`machine` null: the player).
    bool skill_event_sight_blocked(const SkillEventSource& src, tl::core::LevelScript* machine,
                                   int64_t node);
    /// `CBaseUnit::unitInit`: a unit's
    /// `[SKILL]` blocks.
    std::vector<tl::core::LevelScript::UnitSkill> unit_skill_list(const std::string& unit);
    const tl::core::AnimationCatalog* unit_catalog(const std::string& mesh);
    /// A placed unit's skill by name, for its runner's lookups.
    const tl::core::KnownSkill* unit_skill_named(tl::core::LevelScript& machine, int64_t node,
                                                 const std::string& name);
    /// A placed character's `CEffectManager` (`+0x1b8`), made on first use (owner the player,
    /// gate `unit_effect_on_unit`).
    tl::core::EffectManager* unit_effect_manager(tl::core::LevelScript* machine, int64_t node);
    /// `updateEffects`' `KNOCK BACK EFFECT` case (`CCharacter::updateEffects`;
    /// `updateHP`): this frame's push, forces
    /// decayed; `resistance` is `PERCENT KNOCK BACK RESISTANCE` (0x35).
    tl::core::Vec3 knock_back_push(tl::core::EffectManager& effects, float resistance, float dt,
                                   const std::string& who);
    /// A placed item's collision group and avoidance entry switched (`TriggerCollidable`,
    /// `ENABLECOLLISION` / `DISABLECOLLISION`).
    void set_trigger_collidable(int64_t key, bool on);
    /// A skill event striking one character (`characterEffectedByDamageShape`,
    /// `startEvent`'s `APPLYEFFECTS`).
    bool skill_event_strike(const SkillEventSource& source, tl::core::LevelScript* machine,
                            int64_t node);
    /// `CSkill::getTargetIsValid` for a player skill.
    bool player_skill_target_valid(const tl::core::KnownSkill& skill, const ViewUnit& unit);
    const tl::core::KnownSkill* add_executed_skill(const std::string& name, uint32_t level);
    /// The player's skill of that name: the class's, else an executed one.
    const tl::core::KnownSkill* player_skill(const std::string& name);
    /// `CSkillEvent::unitSpawnedFromUnitSpawner` for a
    /// character a player skill's spawner released.
    void skill_unit_spawned(const SkillEventSource& source, tl::core::LevelScript& machine,
                            int64_t node);
    /// The compiled layout at `reference`, parsed once (via layout_file()).
    const tl::formats::CmpScene* cached_layout(const std::string& reference);
    /// The skill files by name, for `SkillCopies::add`.
    tl::core::SkillFileLookup skill_file_lookup();
    ProcTarget proc_target_unit(int64_t node);
    /// `CCharacter::executeProcs(type, target)` for the
    /// player: its own effects, then its worn items'.
    void player_execute_procs(int type, const ProcTarget& target);
    /// `CCharacter::doWeaponProcs(weapon, struck)`.
    void player_weapon_procs(const ProcTarget& struck);
    /// The procs' target for an attacker id: a placed unit, or none.
    ProcTarget proc_target_attacker(int64_t attacker);
    /// `CBaseUnit::activateEffect` on a placed unit: a
    /// cast-skill effect adds its skill to the unit's list.
    void unit_activate_effect(tl::core::LevelScript* machine, int64_t node,
                              tl::core::Effect& effect);
    const tl::formats::CmpNode* missile_named(const std::string& name);
    /// The cast or the swing holds the player where they stand: see step_player().
    bool player_motion_held();
    /// `CGameGlobals +0x2c0 .. +0x2c8` (`reload`).
    tl::core::LevelMemoryRules level_memory_rules() const;
    /// Increment a journal statistic on the player and log it (except steps).
    void count_journal(tl::core::JournalStatistic stat, int32_t amount = 1) {
        if (amount == 0) return;
        player_character.journal().increment(stat, amount);
        if (stat != tl::core::JournalStatistic::StepsTaken) {
            const int32_t total = player_character.journal().value(stat);
            TL_LOG_INFO("journal: %s +%d = %d", tl::core::journal_statistic_name(stat), amount,
                        total);
        }
    }
    std::string rules_path_for(const std::string& layout_path);
    static std::string backdrop_key(const std::string& layout_path);
    bool read_rules(const std::string& layout_path, tl::formats::AdmFile& out, bool quiet);
    void read_rim_light(const std::string& rules_reference);
    /// A frame over 50 ms, split into drive, advance and render, for the log.
    void log_long_frame(unsigned long long drive_us, unsigned long long advance_us,
                        unsigned long long render_us, unsigned long long total_us);
    /// `CQuestManager::getNPCIcon`.
    int npc_quest_icon(const std::string& unit, uint32_t type);
    /// `media/UNITTYPES.HIE`, read once (`CResourceManager`'s hierarchy).
    void load_unit_types();
    /// `CGenericModel::updateAnimation` for an entity with an animator:
    /// false when it has none, and the caller steps its states itself.
    bool step_entity_animation(Ogre::Entity* entity, float delta);
    /// Whether `state` is one of the entity's animator's tracks.
    bool animator_owns(Ogre::Entity* entity, const Ogre::AnimationState* state) const;
    /// False when the log is full and the quest was not taken.
    bool accept_offered_quest(const tl::core::QuestDefinition& quest);
    /// `QuestLog::take_notices` handed to every machine of the level
    /// (`LevelScript::quest_notice`), their events applied.
    void deliver_quest_notices();
    /// What `CQuestRewards::calculateRewards` makes reward units with.
    tl::core::QuestRewardWorld quest_reward_world();
    /// `level_activated` on a placed room's machine, and its events applied.
    void activate_room_machine(size_t index, const std::string& path);
    /// The rest of a level's own layout once its whole tree is placed (`place_layout`, depth 0).
    void finish_level_layout(const std::string& path, const tl::formats::CmpScene& layout,
                             const std::set<int64_t>& gated, const ogre::Vector3& origin);
    /// `CCharacter::applyInstantEffect`'s `OPEN TOWN PORTAL` on the player.
    void open_town_portal();
    // Waypoints (`core/waypoints.h`, renderer_portal.cpp).
    /// `CWaypointActivator::activate`: this
    /// level's depth a waypoint, and "Waypoint Activated" when it is new.
    void activate_waypoint();
    tl::core::GameSettings& game_settings();
    /// `CPlayer::update`'s three tips, once a frame.
    void queue_player_update_tips();
    // PORT: the OGRE animation state a unit's animation was last given, kept.
    void remember_state(Ogre::Entity* entity, const std::string& animation,
                        Ogre::AnimationState* state) {
        if (entity == nullptr || state == nullptr) return;
        std::string key = animation;
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        unit_states[{entity, key}] = state;
    }
    // PORT: the OGRE animation state a unit's animation was last given.
    Ogre::AnimationState* remembered_state(Ogre::Entity* entity,
                                           const std::string& animation) const {
        std::string key = animation;
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const auto it = unit_states.find({entity, key});
        return it == unit_states.end() ? nullptr : it->second;
    }
    /// `CResourceManager::createItem`:
    /// a copy through `unitInit`'s rolls (`init_item_copy`). `from_save`
    /// rolls combat stats and requirements but does not enchant.
    tl::core::ItemStats create_item(const tl::core::ItemStats& data, bool from_save,
                                    bool random_enchant_allowed, bool force_enchant = false);
    /// The player as the item use path reads it.
    tl::core::ItemUseTarget player_use_target();
    tl::core::SkillCastContext player_cast_context();
    /// What the gate asks about the caster: mana off the sheet, the two
    /// equipped weapons' unit types, and the effects that can silence them.
    tl::core::SkillCaster player_caster() const;
    /// `CCharacter::awardExperience` on the player: pets
    /// first (`LevelScript::award_pet_experience`); true when the player rose a level.
    bool award_player_experience(int amount, bool dying);
    void clear_level_up_particle();
    /// Whether the machine that runs node `node`, drawn at `where`, lets it be targeted
    /// (found through the scene node, as in unit_machine()).
    bool node_targetable(int64_t node, const Ogre::SceneNode* where) const;
    tl::core::LevelScript* machine_script(int machine);
    void apply_machine_events(int machine, const std::vector<tl::core::ScriptEvent>& events);
    /// The machine whose `Quest Controller` has a line up, or `kNoMachine` (rooms' too).
    int dialog_machine() const;
    /// `CGameStateController::doActualGameState`, the
    /// interface's half (the level's is `LevelScript::game_state_do`).
    void game_state_interface(uint32_t type, bool on);
    /// `CGameStateController::healPlayer`:
    /// `modifyHP(maxHP())`.
    void heal_player();
    /// Put every enabled one on the navigation grid, once the grid stands.
    void apply_machine_path_blockers();
    void clear_weapon_element();
    /// The shrine's `+0x19c` (Linux `CTriggerUnit +0x1f0`) cleared: a one-shot
    /// trigger at its last state (the machine's `UnitTriggerView::armed`).
    bool enchant_shrine_spent() const;
    /// The renderer's handle on one placed copy of a node (a `Unit Trigger`'s presence and
    /// collision group, a pathing blocker): copies share node ids, so the key mixes in the
    /// room and link hierarchy `CLogicNodeState` saves (+0x30, +0x40). Level nodes keep ids.
    int64_t instance_key(int64_t node, const NodeMap& nodes) const;
    int64_t instance_key(int64_t node, int machine) const;
    static int64_t instance_key(int64_t node, uint32_t scene, int64_t hierarchy);
    /// `CPlayer::fishingAICatch` once
    /// `FISHING_CATCH` has run out.
    void land_catch();
    /// `CBaseUnit::unitInitThemes` the first
    /// time a unit's themes are asked for.
    void init_unit_themes(UnitThemeState& state, const tl::core::UnitStats& stats);
    void flush_item_interacts();
    /// Queue `item` for `flush_item_interacts`: true for a standing, unqueued breakable.
    bool queue_item_interact(int64_t item, bool by_player = false);
    void step_breakable_shapes();
    /// The standing breakable with this id, or null.
    const BreakableWorld* standing_breakable(int64_t id) const;
    /// `CCharacter::unitInit`'s Merchant branch for any
    /// unstocked merchant, then every merchant's clock.
    void step_merchants(float delta);
    /// The merchant a vendor screen was opened on, by its unit name; null for
    /// none.
    MerchantState* merchant_named(const std::string& unit);
    /// The Transmuter placed as `unit` (ISA 0x86); null for none.
    CombinerState* combiner_named(const std::string& unit);
    /// The killer or breaker as the loot roll reads it: `attacker` 0 is the
    /// player, `LevelScript::kNoAttacker` nobody (null).
    const tl::core::LootCharacter* loot_character(int64_t attacker, tl::core::LootCharacter& out);
    /// `player_blow` with no weapon in use: the character's own attack.
    float player_unarmed_blow(int64_t target, float damage_scale, float soak_scale);
    /// A placed character as `CCharacter::isEffectValidForUnit`
    /// reads it.
    tl::core::ItemUseTarget unit_use_target(tl::core::LevelScript* machine, int64_t node);
    /// `CCharacter::performUnknownSkill(CSkill*)` on the
    /// player, for a used item's skill.
    void perform_unknown_skill(const std::string& name, uint32_t level);
    /// `applyInstantEffect`'s `WARP` on the player: `warpLevels(dungeon, 0,
    /// absolute)`, carried out as a Warper's is.
    void warp_by_effect(const std::string& dungeon, int32_t absolute);
    /// The machine whose characters include `node` (the level's own first).
    tl::core::LevelScript* script_owning(int64_t node);
    /// The subject and everything worn on it (subject may be null before show_mesh()).
    std::vector<Ogre::Entity*> animated() const {
        std::vector<Ogre::Entity*> all;
        if (subject) all.push_back(subject);
        all.insert(all.end(), equipment.begin(), equipment.end());
        return all;
    }
    /// Destroy the model and the script's character, for another animal or a level left.
    void destroy_pet_entity();
    /// Send the pet shopping (`CCharacter::sendToTown`); false when away or not allowed.
    bool send_pet_to_town();
    /// `pet`'s stance onto the script's character, and its target dropped.
    void apply_pet_stance();
    /// Whether this entity is still a placed or spawned unit (the highlight's raw pointer).
    bool entity_still_placed(const Ogre::Entity* entity) const;
    /// The unit itself, by the same match.
    Ogre::Entity* find_unit_entity(const std::string& name, std::string& found) const;
    /// Block or unblock one `Path Node Occupation` region on the nav grid (nothing for
    /// another node). `nodes`: the switching machine's map. `live`: also clear characters off
    /// the cells (`clear_path_blocker_area`).
    void apply_path_blocker(int64_t node_id, bool on, const NodeMap* nodes = nullptr,
                            bool live = false);
    /// `CPropertyNode::killOrMoveBaseUnits()` over every
    /// machine: a box `width` by `length` about `centre`.
    void clear_path_blocker_area(const PathBlocker& b);
    /// Apply every enabled pathing blocker the layout places.
    void apply_path_blockers(const tl::formats::CmpScene& layout, const std::set<int64_t>& gated);
    /// `CCharacter::alignment()` for a placed character:
    /// its machine's `+0x338` (`setAlignment`), else the data's.
    tl::core::UnitStats::Alignment unit_side(int64_t node, const tl::core::UnitStats& stats) const;
    /// A character whose `+0x640` master is the player, in any machine.
    bool node_is_player_pet(int64_t node) const;
    /// The nodes a machine's events act on.
    NodeMap* nodes_of(tl::core::LevelScript* machine);
    void fire_unit_skills_on_create(tl::core::LevelScript& machine, int64_t node);
    UnitEventCaster unit_event_caster_for(tl::core::LevelScript* machine, int64_t node);
    /// The hooks every machine's characters cast through (`LevelScript::UnitSkills`).
    tl::core::LevelScript::UnitSkills unit_skill_hooks();
    tl::core::SkillCaster unit_caster(tl::core::LevelScript& machine, int64_t node,
                                      tl::core::LevelScript* target_machine, int64_t target_node,
                                      bool has_target);
    tl::core::SkillCastContext unit_cast_context(tl::core::LevelScript& machine, int64_t node,
                                                 tl::core::LevelScript* target_machine,
                                                 int64_t target_node, bool has_target);
    /// What unit_cast_context() leaves behind besides the context: the unit's effect manager
    /// made and handed out, the tables loaded, its model's animations read.
    void prepare_unit_cast(tl::core::LevelScript& machine, int64_t node);
    /// `CCharacter::castSkill`'s `performSkill`, mana and animation for a unit.
    bool cast_unit_skill(tl::core::LevelScript& machine, int64_t node,
                         tl::core::LevelScript* target_machine, int64_t target_node,
                         bool has_target);
    /// `CSkill::stopSkill`'s `resetEvent` over
    /// the copy's events: `CSkillEvent::stopEvent`, and
    /// `CSkillEvent::reset` detaching flying missiles
    /// (`FlyingMissile::detached`).
    void detach_event_missiles(const std::string& skill, uint32_t run,
                               const tl::formats::AdmSection& block,
                               tl::core::LevelScript* caster_machine, int64_t caster_node);
    bool level_stores_unit(int64_t id);
    /// `CSkillEvent::applyWeaponDamage` on a placed
    /// character: `WEAPONDAMAGEPCT` of the player's blow; the damage dealt.
    float skill_event_weapon_blow(const SkillEventSource& source, tl::core::LevelScript* machine,
                                  int64_t node, float damage_scale, float soak_scale);
    /// The standing breakable with this id, by index; -1 for none.
    int breakable_index(int64_t id) const;
    void invoke_hit_skills(const SkillEventSource& source, int group,
                           tl::core::LevelScript* machine, int64_t node);
    /// `CCharacter::addPet` on the player: past
    /// `MAXIMUM_PET_INSTANCES` the oldest of the name die, then `setMaster`.
    void add_player_pet(tl::core::LevelScript& machine, int64_t node);
    /// The same with a placed master (a monster's `MAKEPET` summon).
    void add_monster_pet(tl::core::LevelScript& machine, int64_t node,
                         tl::core::LevelScript& master_machine, int64_t master_node);
    std::vector<MachineView> machines();
    /// The level being built's dungeon and depth, and whether it is remembered (`CLevel+600`).
    void set_level_identity(const std::string& dungeon, int32_t depth, bool is_town,
                            bool dont_store, bool allows_portals);
    /// Whether this layout's particles are drawn (`RendererConfig::pu_particle_layouts`).
    bool particle_universe_takes(const std::string& layout_path) const;
    /// What `reinitializeQuest`'s floor roll reads.
    tl::core::QuestDungeonSource quest_dungeon_source();
    tl::core::QuestPlayerSource quest_player_source();
    void start_critical_shake();
    /// A shake `CCharacter::die` starts at a body, along
    /// the up vector (`+0x9c`), from `centre`.
    void start_death_shake(const char* name, float duration, float magnitude,
                           const ogre::Vector3& centre);
    void ensure_item_index();
    /// `CBaseUnit::addToAvoidanceMap` / `removeFromAvoidanceMap`
    /// for a breakable: radius + 0.4 on the grid.
    void breakable_avoidance(BreakableWorld& b, bool add);
    /// The treasure at the end of `CCharacter::die`;
    /// `attacker` as `hurt_unit` takes it.
    void drop_death_loot(int64_t node, const std::string& unit, int64_t attacker);
    void drop_death_loot_at(const ogre::Vector3& at, int64_t node, const std::string& unit,
                            int64_t attacker);
    /// What the loot rolls hand core/item_roll.h: placement tests, `addUnit`, `createUnit`.
    tl::core::LootPlacement loot_placement_at(const ogre::Vector3& at);
    /// The two blows' shared half: the defender, the roll, the interrupt and
    /// `applyDamageEffects`.
    float player_strike(int64_t target, const tl::core::Strike& strike);
    /// Take `amount` off a placed unit, killing it if that empties it. `owner`: its machine
    /// if known. `blow`: what `applyDamage` hands `die`. `struck`: `applyDamage`'s 8th
    /// argument, rolling `CAST SKILL ON STRUCK` (`LevelScript::damage_unit`).
    void hurt_unit(int64_t node, const std::string& unit, float amount, bool is_crit = false,
                   int64_t attacker = 0, tl::core::LevelScript* owner = nullptr,
                   const tl::core::KillingBlow* blow = nullptr, bool struck = false);
    /// What a blow meets on the player: `AC()`, `maxHP()`, `getBlockChance()`
    /// and `damageDefense(type)`.
    tl::core::Defender player_defender();
    /// The nearest hostile placed unit within `reach` of the character, or 0.
    int64_t hostile_in_reach(float reach) const;
    /// A unit as `CSkillManager::getPositionForSkill` reads
    /// one: centre, radius, facing. A null machine is the player.
    bool skill_position_unit(tl::core::LevelScript* machine, int64_t node,
                             tl::core::SkillPositionUnit& out);
    /// `CCharacter::die`'s skill half for a placed unit: `stopAllSkills(true, true, false)` and
    /// `CSkillManager::fireSkillsOnDeath`.
    void fire_unit_skills_on_death(tl::core::LevelScript& machine, int64_t node);
    /// `CCharacter::applyEffectOnUnit` for a placed
    /// character; true copies the effect in.
    bool unit_effect_on_unit(tl::core::LevelScript* machine, int64_t node,
                             tl::core::Effect& effect);
    /// `CCharacter::applyInstantEffect` for a
    /// placed character.
    void unit_instant_effect(tl::core::LevelScript* machine, int64_t node,
                             const tl::core::Effect& effect, float value);
    void update_unit_effects(float dt);
    ProcTarget proc_target_breakable(int index);
    /// `CCharacter::executeProcs(type, target)`, placed unit.
    void unit_execute_procs(tl::core::LevelScript& machine, int64_t node, int type,
                            tl::core::LevelScript* target_machine, int64_t target_node,
                            bool has_target);
    /// `CSkillManager::executeSkill(skill, player, ...)` on
    /// the holder's manager: the player's (`location` -1) or the worn item's
    /// (`worn_skill_runners`, `+0x1c8`; `doWeaponSkill`, `doWeaponProcs`,
    /// `CInventory::executeProcs`). INVENTED: a bag item runs on the player's.
    bool run_proc_skill(const tl::core::SkillCopy& skill, const tl::core::Vec3& start,
                        const tl::core::Vec3& aim, const ProcTarget& target, const char* why,
                        int location = -1);
    /// The console's turn in a skill or holding attack (X360 `CCharacter::update`
    /// `core::player_skill_turn`), before the motion.
    void step_player_skill_turn(float dt);
    /// `displayDamageAbsorbed(delta)` on the player: the red number, when due.
    void display_player_damage(float delta);
    /// `CCharacter::applyInstantEffect`, case
    /// `DAMAGE`, on the player.
    void player_instant_damage(const tl::core::Effect& effect);
    /// `CCharacter::modifyDamage` when a `SHIELD BUFFER` took the hit:
    /// `Absorbed!` over the character and `SHIELDBLOCK` (slot 7).
    void show_absorbed(const ogre::Vector3& at, const tl::core::UnitStats& stats);
    /// The same over a placed unit, by node.
    void show_unit_absorbed(tl::core::LevelScript* machine, int64_t node);
    /// One of the combat and loot lines over a unit standing at `unit_at`.
    void add_combat_text(tl::core::CombatText what, const ogre::Vector3& unit_at, int amount = 0);
    /// Where a unit of the level stands; false when nothing draws it.
    bool unit_node_position(int64_t node, ogre::Vector3& at);
    /// `CCharacter::playHitParticle` on the
    /// unit struck.
    void play_hit_particle(const tl::core::UnitStats& victim, const ogre::Vector3& from,
                           const ogre::Vector3& at, CharacterRef who);
    /// `CCharacter::playStrikeParticle` of
    /// the attacker; `radius` is the struck unit's.
    void play_strike_particle(const tl::core::UnitStats& attacker, const ogre::Vector3& from,
                              const ogre::Vector3& at, float radius, bool critical, bool blocked,
                              CharacterRef who);
    /// `CCharacter::die`: the particle at the body.
    void play_death_particle(const tl::core::UnitStats& unit, const ogre::Vector3& at,
                             CharacterRef who);
    /// The slot key of a level character: the machine that runs `node`, or the player for 0.
    CharacterRef character_ref(int64_t node, tl::core::LevelScript* machine = nullptr);
    /// What `rollAttack` shows of the player's blow besides
    /// the damage. `damage` is what `applyDamage` was handed.
    void show_player_blow(int64_t target, const std::string& unit, float damage);
    /// `CCharacter::awardFame` for the
    /// player, with its text.
    bool award_player_fame(int amount);
    /// `CSkillEvent::startEvent`, the layout half: `FILE`
    /// on the caster (or target, `PLACEONTARGET`) facing its way, timelines played.
    void start_skill_event(const tl::core::KnownSkill& skill, uint32_t level, int group,
                           const tl::formats::AdmSection& event,
                           const tl::core::SkillEventTarget& target);
    /// `CSkillEvent::updateEvent`, the `ATTACHOWNER` half,
    /// once a frame.
    void update_live_skill_events(float delta);
    /// `updateEvent`, the `ATTACHTOTARGET` and line halves.
    void update_skill_event_lines(LiveSkillEvent& event);
    /// `CCharacter::moveToPosition` for the player.
    bool move_player_to(const ogre::Vector3& to, bool ignore_units, bool ignore_obstructions);
    bool move_unit_to(tl::core::LevelScript* machine, int64_t node, const ogre::Vector3& to,
                      bool ignore_units, bool ignore_obstructions);
    /// `CSkillEvent::shouldStop`, negated, over one copy's
    /// live events (`SkillRunState::copy`).
    bool skill_events_alive(const std::string& skill, uint32_t run,
                            tl::core::LevelScript* caster_machine, int64_t caster_node);
    /// `CSkillEvent::canStop` over them, for
    /// `CSkill::attemptToStopSkill`.
    bool skill_events_can_stop(const std::string& skill, uint32_t run,
                               tl::core::LevelScript* caster_machine, int64_t caster_node);
    bool skill_event_particles_alive(const LiveSkillEvent& event) const;
    void reset_skill_events(const std::string& skill, uint32_t run,
                            tl::core::LevelScript* caster_machine, int64_t caster_node);
    /// `CSkillEvent::stopEvent`; the event then leaves the
    /// list, its layout released by `release_ended_skill_events`.
    void stop_skill_event(LiveSkillEvent& event);
    /// The machines of an ended event's layout, [first, end) of `nested_scripts`, retired; one
    /// that holds characters stays a machine of the level.
    void retire_skill_layout(size_t first, size_t end);
    /// Every event that left its skill's list, or was reset, has its layout retired and its
    /// particles handed to `transient_particles`. Only between walks of the level's machines.
    void release_ended_skill_events();
    /// `CSkillEvent::addListeners`, the damage-shape half:
    /// shapes of machines `first`.. take `TARGET_CORPSES` and `MAX_UNITS_HIT` and report.
    void hang_player_skill_event(const SkillEventSource& source, size_t first);
    /// `CSkillEvent::applyEffects` on one character (null
    /// machine: the player); whether anything was applied. (`CSkillEvent::invokeHitSkills`,
    /// is apart.)
    bool skill_event_effects_on(const SkillEventSource& source, tl::core::LevelScript* machine,
                                int64_t node);
    /// `CLevel::findCharacterWithinView` from `at` turned by
    /// `facing`, optionally for a skill (`CSkill::getTargetIsValid`); true with a unit.
    bool find_character_within_view(const tl::core::Vec3& at, const ogre::Quaternion& facing,
                                    float near, float angle, float range, int32_t alignment,
                                    bool nearest, const ViewUnit& ignore, ViewUnit& found,
                                    const std::function<bool(const ViewUnit&)>& valid = nullptr);
    /// `CCharacter::castSkill`: a
    /// targetless cast with `FINDTARGETANGLE` takes the nearest valid character in the cone,
    /// else the first breakable (`findItemWithinView`).
    void find_player_skill_target(const tl::core::KnownSkill& skill);
    /// `CSkillEvent::startEvent`'s tail: each `EXECUTE_SKILL`
    /// via `CExecuteSkillProps::executeSkill`.
    void execute_event_skills(const SkillEventSource& source, const tl::core::SkillEventSpec& spec,
                              const ViewUnit* unit, const tl::core::Vec3& origin,
                              const ogre::Quaternion& facing, const tl::core::Vec3& point);
    /// The observer for one copy of a caster's skill, made on first asking.
    SkillObserver* skill_observer_for(tl::core::LevelScript* caster_machine, int64_t caster_node,
                                      const std::string& skill, uint32_t run);
    /// `CSkill::unitStateChange`.
    void skill_unit_state_change(SkillObserver& observer, tl::core::LevelScript* machine,
                                 int64_t node, int state);
    /// The level's items as `CLevel::getActiveUnitsAtPosition`
    /// walks them (`+0x90`): the breakables still standing, at their nodes.
    std::vector<tl::core::ActiveUnitCandidate> breakable_item_candidates() const;
    /// `CUnitSpawner::createAndFireMissile` /
    /// `CMissile::fireMissile`: launched from `at` along
    /// `forward`, drawn, kept in `missiles`. `who` names the firer in the log.
    void launch_missile(FlyingMissile flying, const ogre::Vector3& at, const ogre::Vector3& forward,
                        const std::string& who);
    /// `CEquipment::fireMissiles(owner, target)` for the
    /// player's weapon (needs a `MISSILE`); `target` a character, `breakable` an index (-1).
    bool fire_weapon_missiles(int64_t target, int breakable);
    /// The same for a placed character's weapon, from its hand (`left`), at `target_node` of
    /// `target_machine` when `aimed`, else along its facing.
    void fire_unit_weapon_missile(tl::core::LevelScript* machine, int64_t node,
                                  const std::string& missile, bool left, bool aimed,
                                  tl::core::LevelScript* target_machine, int64_t target_node);
    /// `CEquipment::missileApplyingEffects`: the wielder's
    /// `rollAttack` on what its missile met.
    bool weapon_missile_harm(int64_t unit, const ogre::Vector3& at, float damage_scale,
                             float soak_scale);
    /// The same for a placed character's weapon: `getCharacterCanBeHarmedByMissile`
    /// (`isEnemy(wielder, unit)`), then `rollAttack`.
    bool unit_weapon_missile_harm(tl::core::LevelScript* wielder, int64_t wielder_node,
                                  int64_t unit, const ogre::Vector3& at, float damage_scale,
                                  float soak_scale);
    void fire_spawner_missile(int64_t node, const std::string& resource, const ogre::Vector3& at,
                              const ogre::Vector3& forward);
    /// `transient_particles` whose time is up: stopped, and once nothing on them lives,
    /// destroyed; every list due asked in one walk of the systems.
    void dispose_transient_particles(float delta);
    void step_missiles(float delta);
    void clear_missiles();
    bool level_missile_harm(const tl::core::MissileSpec& spec, int64_t unit,
                            const ogre::Vector3& at, float damage_scale,
                            const tl::core::Vec3& push);
    /// `CSkillEvent::missileApplyingEffects` behind
    /// `getCharacterCanBeHarmedByMissile`: true when the missile stops on `unit`.
    bool missile_harm(const SkillEventSource& source, int64_t unit, float soak_scale,
                      const ogre::Vector3& at, float damage_scale, float yaw);
    /// `CSkillEvent::canEffectPosObject(at, unit)`: centre
    /// within 3.0 of `at` in height; null machine is the player.
    bool skill_event_reaches_height(const ogre::Vector3& at, tl::core::LevelScript* machine,
                                    int64_t node);
    /// `missileApplyingEffects`' `CSkill::triggerEvent(6, ...)`: `EVENT_MISSILEHIT`
    /// on the firing copy, the struck unit its target.
    void fire_missile_hit_event(const SkillEventSource& source, tl::core::LevelScript* machine,
                                int64_t node, const ogre::Vector3& at, float yaw);
    /// What `CQuestUnitData` reaches outside the quest for (`tl::core::QuestUnitWorld`).
    tl::core::QuestUnitWorld quest_unit_world();
    /// `CEquipment::update` and `CItemGold::update`
    /// after `CItem::update`: the toss, gold's sparkle and growth.
    void step_dropped_item(DroppedWorldItem& dropped, float delta);
    /// `CEquipment::setActiveInLevel(true)` ->
    /// `createParticles`.
    void start_item_particles(DroppedWorldItem& dropped);
    /// A ground item leaving the level: its particles, then its model.
    void remove_ground_item(DroppedWorldItem& dropped);
    /// `CBreakable::interact(CCharacter*)` on
    /// `breakables[index]`, by the player (`by_player`) or by nobody.
    bool break_breakable(size_t index, bool by_player);
    void cast_prop_trap(const std::string& unit, const std::string& name, uint32_t level,
                        const ogre::Vector3& skill_at, const ogre::Quaternion& facing,
                        bool by_player);
    tl::core::ItemActiveRangeInput item_range_input(const ogre::Vector3& at, bool interactable,
                                                    bool casts_shadows) const;
    /// `CLevel::updateNearUnitList`'s item
    /// half, on the characters' tick (`LevelScript::set_item_near_list`).
    void item_near_list();
    void step_level_items(float delta);
    /// Every `Unit Trigger` of every machine of the level.
    std::vector<TriggerItem> level_trigger_items();
    /// A breakable taken out of the level: its model, its collision, its place
    /// on the map grid.
    void remove_breakable(BreakableWorld& b);
    void drop_item_near_player(const tl::core::ItemStats& item, int count);
    /// `CLevel::addItem(item, at, true)` for a `CEquipment`:
    /// grounded, then tossed (`CEquipment::drop`).
    void drop_item(const tl::core::ItemStats& item, int count, const ogre::Vector3& at);
    /// `CLevel::addItem(gold, at, true)` for a `CItemGold`: `setActiveInLevel`
    /// grounds it and its sparkle; the drop sound when `drop_sound`.
    void place_gold(const tl::core::GoldPile& pile, const ogre::Vector3& at,
                    const tl::core::ItemStats* item = nullptr, bool drop_sound = true);
    /// `CItem::snapToGround`: down from 100 to -100; a miss
    /// retries at `randomOpenPosition(p, 4)`, 30 times.
    ogre::Vector3 snap_item_to_ground(const ogre::Vector3& at);
    /// `CCharacter::getItem`, the gold half,
    /// for `dropped_items[index]`.
    void take_gold(size_t index);
    /// `CGameClient::autoPickupGold`.
    void auto_pickup_gold();
    /// An item at rest on the ground (`CLevel::addItem`, `CItem::snapToGround`): no toss.
    void place_ground_item(const tl::core::ItemStats& item, int count, const ogre::Vector3& at,
                           const ogre::Quaternion& orientation, const tl::core::QuestUnitTag& tag);
    /// `addCharacter`'s `dropToGround(4.0)` (`CQuestUnitData::createUnit`;
    /// `spawnUnit`).
    ogre::Vector3 dropped_to_ground(const ogre::Vector3& at) const;
    /// `CLevel::addCharacter`'s `CCharacter::dropToGround(level, 4.0, ..., false)`: the search
    /// (core/item_drop.h, `drop_character_to_ground`).
    ogre::Vector3 character_to_ground(const ogre::Vector3& at);
    /// The sound tables a gold pile's bank reads and `g_Rand` (`make_gold_pile`).
    tl::core::GoldPileSounds gold_sounds();
    void create_quest_unit(const tl::core::QuestObjective& objective,
                           const tl::core::QuestUnitTag& tag, int64_t quest_guid);
    /// `CQuestUnitData::spawnUnit` past its checks; the
    /// character's node, or 0.
    int64_t spawn_quest_unit(const tl::core::QuestObjective& objective,
                             const tl::core::QuestUnitTag& tag, int64_t quest_guid,
                             const ogre::Vector3& at, const ogre::Quaternion& facing);
    /// Take `amount` off the player. `attacker` (`applyDamage`'s 6th argument, `kNoAttacker`
    /// none) is answered by reflection; `struck` rolls `CAST SKILL ON STRUCK` first.
    /// `shown`: add to the number over the character.
    void hurt_player(float amount, const std::string& from,
                     int64_t attacker = tl::core::LevelScript::kNoAttacker, bool struck = false,
                     bool shown = true);
    /// `attacker`: whoever dealt the last blow (`die`'s 2nd argument), for the
    /// `CAST SKILL ON DEATH` procs; `kNoAttacker` for none.
    void kill_player(int64_t attacker = tl::core::LevelScript::kNoAttacker);
    void choose_resurrection(tl::core::ResurrectChoice choice);
    void level_resetting_player(tl::core::ResurrectChoice choice);
    void build_player_catalog();
    void step_skill_animation(float delta);
    /// The skill animation's own keys, before the skill's: trails, sounds, particles.
    void fire_model_keys(const std::vector<tl::core::FiredKey>& keys,
                         const tl::core::AnimationManifest& manifest);
    /// `CCharacter::updateActiveSkill` for
    /// the player.
    void step_active_skill(float dt);
    void load_player_stats();
    /// `createUnit` for a breakable, then `CLevel::addItem(b, at, false)`:
    /// `unitInit`, `setActiveInLevel(true)`,
    /// then `settle`. False with no file or model.
    bool place_breakable(const std::string& unit, const ogre::Vector3& at,
                         const ogre::Quaternion& facing, int64_t id, const ogre::Vector3* settle,
                         const float* drawn_scale = nullptr);
    /// Start a swing, if the character is not already in one.
    bool begin_swing(int64_t target);
    /// Advance the player's swing and land it when the authored frame passes.
    void step_swing(float delta);
    /// A placed unit's life has run out: fall over, stop hunting, stop being hunted.
    void kill_unit(int64_t node, const std::string& unit, bool fall = true, int64_t attacker = 0);
    /// `CPlayer::storeLevelSavedState` over this level.
    void store_level_state();
    /// `CGameClient::restoreLevelState`, if remembered.
    void restore_level_state();
    /// The skill texts' context (`skill_effects`) and the player's applied manager from the
    /// player's stats as they stand.
    void refresh_skill_effects();
    /// Hand a level script everything it asks the renderer for.
    void wire_script(tl::core::LevelScript& machine);
    /// One layout file placed: its gates and random groups rolled, its nodes made into
    /// placements, machines, particles and units, nested links placed in turn; at depth 0
    /// the level's script, grids and viewer too.
    bool place_layout(const std::string& reference, const ogre::Vector3& origin,
                      const ogre::Quaternion& basis, int depth, uint32_t load_seed_arg = 0);
    /// A mod's `Merge/` layouts beside `path`, placed with it (place_layout).
    void place_merge_layouts(const std::string& path, const ogre::Vector3& origin,
                             const ogre::Quaternion& basis, int depth);
    /// A layout's `Item` node: `CItemDescriptor::CreateObject`
    /// (`CEquipment::createNewEquipment`), then
    /// `CItemDescriptor::descriptorSceneActivated`.
    void place_layout_item(const tl::formats::CmpNode& node, const ogre::Vector3& at);
    /// Find a `Property Node` of `wanted_type` (`"exit"` or `"entrance"`, without case) in a
    /// placed chunk's `.cmp` (`EXIT_NODE.LAYOUT` / `ENTRANCE_NODE.LAYOUT` via a `Layout Link`).
    /// What else the marker's layout says: its flattened forward (`CLevel +0x164`) and the
    /// `Warper` beside it.
    bool find_chunk_marker(const std::string& chunk_cmp_path, const ogre::Vector3& chunk_origin,
                           const std::string& wanted_type, ogre::Vector3& world_position,
                           float& radius, ChunkMarkerExtra* extra = nullptr);
    /// Build and place one procedurally-generated dungeon floor.
    bool place_dungeon_floor(const std::string& dungeon_name, int floor_number);
    /// A `CREATEAS: TRIGGER` prop made outside a layout (`populate`'s chests:
    /// `CResourceManager::createUnit`, `CLevel::addItem`),
    /// as a one-node layout so it acts as a placed one. False for a unit
    /// that is not a trigger prop. `scale`: what making it drew (`CBaseUnit::unitInit`).
    bool place_populated_trigger(const std::string& unit, const ogre::Vector3& at,
                                 const ogre::Quaternion& facing, int64_t id, float scale = 1.0f);
    /// ... and each of them made beside `master`, added to the level under
    /// `owner` and made its pet.
    void place_unit_minions(int64_t master, int64_t owner, const std::vector<std::string>& minions);
    int populate_units(const std::string& spawn_class, uint32_t wanted, int level,
                       tl::core::Randomizer& sections, const PopulatePhase& phase);
    /// A `CFormationNode` over `reference` at `at` (`setLayoutFile`);
    /// false when the layout is not in the archive.
    bool add_pending_formation(const std::string& reference, const ogre::Vector3& at,
                               int monster_level);
    /// `CLevel::populate(true)`: formations, counts, the five
    /// unit phases and gold. `placed_pieces` takes the formations placed.
    void populate_level(const tl::core::LevelTemplate& level_template, int level,
                        std::vector<std::pair<std::string, ogre::Vector3>>& placed_pieces);
    /// `group` tags the triangles so they can be switched off (`CollisionWorld::set_group()`);
    /// zero is the level's own. `piece_at`: the owning `Room Piece`'s position, making it a
    /// `CLevel::populate` section (`population_sections`).
    /// `recalculate`: the piece's box is cast again after the bake (`passability_recalculate`).
    void add_collision_mesh(const std::string& path, const ogre::Matrix4& to_world,
                            int64_t group = 0, const ogre::Vector3* piece_at = nullptr,
                            bool recalculate = false);

    /// A collision mesh as add_collision_mesh() folds it in, in its own space, read once per
    /// level; each placement transforms its points with the same `Matrix4 * Vector3`.
    /// Defaults: `CCollisionModel::CCollisionModel`.
    struct CollisionModel {
        enum class State { Loaded, Threw, Missing } state = State::Missing;
        /// The load's exception, repeated for every placement as it was before.
        std::string error;
        struct Submesh {
            /// `tl::core::collision_material` of the submesh's name and index.
            uint32_t material = 0;
            std::vector<ogre::Vector3> points;
            /// Three per triangle, each already checked against `points`.
            std::vector<uint32_t> corners;
        };
        std::vector<Submesh> submeshes;
        bool has_bounds = false;
        tl::core::Vec3 low, high;
    };
    /// By the path add_collision_mesh() was handed; per level.
    std::unordered_map<std::string, CollisionModel> collision_models;
    const CollisionModel& collision_model(const std::string& path);
    /// A collision placement's triangles into the level's collision world, at its pose.
    void add_collision_placement(const MeshPlacement& placement);
    /// A spawn class's file by its upper-cased `NAME`, as `CSpawnClassParser` files them; empty for none.
    std::string spawn_class_path(const std::string& name) const;
    /// `CSpawnClassParser::getSpawnClass`'s entries for a class, read from its file.
    tl::core::SpawnClassLookup archive_spawn_class_lookup() const;
    mutable std::unordered_map<std::string, std::string> spawn_class_paths;
    mutable bool spawn_classes_indexed = false;
    /// A unit made and placed: its numbers indexed, an id of its own, its model (the renderer's), the
    /// spawned list and its spawn level. Null when it has no file or no model.
    Ogre::Entity* spawn_unit(const std::string& unit, const ogre::Vector3& at, int64_t owner,
                             int level = 0, bool champion = false, bool spawn_anim = false,
                             const ogre::Quaternion& facing = ogre::Quaternion::IDENTITY,
                             float scale = 0.0f);
    /// The archive path of a unit's mesh (`UnitStats::mesh`); empty for none.
    std::string unit_mesh_path(const std::string& unit);
    /// Fill `tables.context`. `items_by_type` backs a `UNITTYPE` pick with the item index
    /// (`CUnitResourceList::getDataGroupsByUnitType`).
    void build_spawn_context(SpawnTables& tables, bool items_by_type);
};

}  // namespace tl::core
