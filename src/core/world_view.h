// The hooks the game calls on the renderer's half: what `Renderer::Impl` answers for the
// World (core/world.h), apart from the gameplay it runs itself.
#pragma once

#include "core/world_state.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tl::core {

class WorldView : public WorldState {
public:
    WorldView() = default;
    WorldView(const WorldView&) = delete;
    WorldView& operator=(const WorldView&) = delete;
    virtual ~WorldView() = default;

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
    virtual bool console_game_data() const = 0;
    virtual std::string dir_of_asset(const std::string& name) const = 0;
    virtual tl::core::DungeonManager& loaded_dungeon_manager() const = 0;
    virtual std::vector<uint8_t> read_resource(const std::string& path) const = 0;
    virtual std::string resolve_archive_path(const std::string& reference) const = 0;

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
    /// `CUnitSpawner::createParticleSystemAt`: spawner
    /// `spawner`'s layout `index`, placed again.
    virtual void view_spawner_particles(int64_t spawner, const std::string& file,
                                        const ogre::Vector3& at, const ogre::Quaternion& facing,
                                        uint32_t index) = 0;
    /// A unit whose body is a particle layout (`LAYOUTFILE`, no `MESHFILE`), which
    /// spawn_unit() cannot place.
    virtual bool view_place_layout_bodied_unit(const std::string& unit,
                                               const ogre::Vector3& at) = 0;
    /// A released unit's entrance (`CCharacter::spawn`); whether one started.
    virtual bool view_begin_spawn_entrance(int64_t id, Ogre::Entity* entity) = 0;
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
    /// The view's start of a character's own particle; its sounds are `start_slot_particle`'s.
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
    /// A recording a bank drew, with its volume variation: the mixer's.
    virtual bool play_recording(const std::string& name, const std::string& recording,
                                float variation, const ogre::Vector3& at) = 0;
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
};

}  // namespace tl::core
