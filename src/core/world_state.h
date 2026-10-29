// The gameplay state the renderer used to hold: what the level, the player and their
// items are, without OGRE (core/world.h).
#pragma once

#include "core/active_range.h"
#include "core/affix_book.h"
#include "core/animation_manifest.h"
#include "core/asset_path.h"
#include "core/attack_button.h"
#include "core/audio.h"
#include "core/automap_grid.h"
#include "core/average_fps.h"
#include "core/breakable.h"
#include "core/camera_shake.h"
#include "core/character.h"
#include "core/character_effects.h"
#include "core/character_sheet.h"
#include "core/cinematics.h"
#include "core/collision.h"
#include "core/combine.h"
#include "core/context_tips.h"
#include "core/continue_menu.h"
#include "core/death.h"
#include "core/difficulty_menu.h"
#include "core/display_gamma.h"
#include "core/dungeon.h"
#include "core/effects.h"
#include "core/enchanting.h"
#include "core/equipment_tooltip.h"
#include "core/fishing.h"
#include "core/game_config.h"
#include "core/game_speed.h"
#include "core/game_ui.h"
#include "core/gamepad.h"
#include "core/hotbar.h"
#include "core/howto_menu.h"
#include "core/input.h"
#include "core/interactive_menu.h"
#include "core/inventory.h"
#include "core/inventory_screen.h"
#include "core/item_drop.h"
#include "core/item_effects.h"
#include "core/item_label.h"
#include "core/item_name.h"
#include "core/item_roll.h"
#include "core/item_save.h"
#include "core/item_sets.h"
#include "core/item_use.h"
#include "core/journal.h"
#include "core/layout_helpers.h"
#include "core/layout_maths.h"
#include "core/level_memory.h"
#include "core/light_override.h"
#include "core/log.h"
#include "core/merchant.h"
#include "core/mesh_placement.h"
#include "core/missile.h"
#include "core/model_animation.h"
#include "core/music_object.h"
#include "core/navigation.h"
#include "core/new_game_menu.h"
#include "core/no_spawn_region.h"
#include "core/ogre_types.h"
#include "core/passability.h"
#include "core/pet.h"
#include "core/pet_commands.h"
#include "core/platform.h"
#include "core/player.h"
#include "core/populate.h"
#include "core/quest.h"
#include "core/quest_book.h"
#include "core/random_groups.h"
#include "core/randomizer.h"
#include "core/retirement.h"
#include "core/script.h"
#include "core/settings_menu.h"
#include "core/skill_animation.h"
#include "core/skill_cast.h"
#include "core/skill_text.h"
#include "core/skills.h"
#include "core/skills_tab.h"
#include "core/sound_data.h"
#include "core/spawn_class.h"
#include "core/spell_remove.h"
#include "core/stash.h"
#include "core/stats.h"
#include "core/stats_page.h"
#include "core/targeting.h"
#include "core/text.h"
#include "core/text_event.h"
#include "core/town_portal.h"
#include "core/tray_moves.h"
#include "core/tray_press.h"
#include "core/tray_row.h"
#include "core/unit_observers.h"
#include "core/unit_resource.h"
#include "core/unit_themes.h"
#include "core/unit_types.h"
#include "core/units.h"
#include "core/view_handles.h"
#include "core/wardrobe.h"
#include "core/warp.h"
#include "core/waypoints.h"
#include "core/weapon_procs.h"
#include "core/weapon_trail.h"
#include "core/weapons.h"
#include "formats/adm.h"
#include "formats/cegui.h"
#include "formats/cmp.h"
#include "formats/ogg.h"
#include "formats/uilayout.h"
#include "formats/wav.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace tl::core {

class World;

/// `CMissile::killMissile`: `+0x1b4`, how long a dead
/// missile's particles keep releasing before `handleDeathOfMissile` stops them.
inline constexpr float kMissileParticleStopSeconds = 1.0f;

// The player as a missile meets it: no character id is negative.
constexpr int64_t kMissilePlayer = -1;

/// The gameplay camera's default distance: `CCameraControl::getDefaultDistanceToDolly()`
/// (.rodata, 28.5) times `CAMERAMULT`; `CCameraControl::processInput()` clamps
/// between 14.0 and that. `CCameraControl::updateGameCamera` offsets
/// `(0.5, 0.75, -0.5)` turned by the camera angle, scaled by the dolly.
inline constexpr float kDefaultCameraDistance = 28.5f;

/// The default when a level authors no `RIMLIGHT` (`CLevel::addCharacter`).
constexpr const char* kDefaultRimLight = "media/sharedtextures/rimlight.dds";

/// What a main-menu level plays (`CGameClient::loadMenuLevel` hardcodes it), spelled as
/// a rules `MUSIC` key.
constexpr const char* kMenuMusic = "../music/Title.ogg";

/// How long a quest's banner stays up, in seconds.
constexpr float kQuestBannerSeconds = 2.0f;

/// Where the ids given to spawned units start: a block no layout uses (`0x5350` is "SP").
constexpr int64_t kSpawnedNodeBase = 0x5350000000000000LL;

/// The animation a unit plays when the layout names nothing else (an engine name, not data).
constexpr const char* kUnitIdleAnimation = "Idle";

/// An item's record as a spawn class reads it (`CUnitResourceList`'s data of the unit).
SpawnUnitData spawn_data_of(const ItemStats& item);

class WorldState {
public:
    /// Every quest under `media/Quests/` and the log the player carries; the log owns the
    /// gate state (`QuestLog::state()` is what a `Group` node's gate tests).
    tl::core::QuestBook quest_book;

    tl::core::QuestLog quest_log;

    /// Which quest the open window is offering, and who is offering it.
    const tl::core::QuestDefinition* offered_quest = nullptr;

    /// The line the window shows (`CQuestDialogMenu +0x120`'s), for
    /// `hand_dialog_items` when it closes.
    const tl::core::DialogLine* offered_line = nullptr;

    /// The offered line's section (`CQuestDialog +0x70`): `OK` on a
    /// `[COMPLETE]` line is what completes its quest.
    tl::core::QuestSection offered_section = tl::core::QuestSection::Intro;

    /// The menu's unit (`CQuestDialogMenu +0xc0`) for a line the player's own
    /// talk opened: the machine that runs it and its node.
    int talk_menu_machine = kNoMachine;

    int64_t talk_menu_node = 0;

    std::set<std::string> reported_gates;

    tl::core::LevelScript script;

    /// Where a character can walk in this level and how to get anywhere (core/navigation.h).
    tl::core::NavGrid nav;

    float level_unit_active_range = 16.0f;

    /// `GLOBALS.DAT` `UNIT_NEAR_RANGE` (`CGameGlobals +0xac`, 24 by default).
    float level_unit_near_range = 24.0f;

    /// `GLOBALS.DAT` `UNIT_SHADOW_RANGE` (`CGameGlobals +0xa8`, 10 by default).
    float level_unit_shadow_range = 10.0f;

    /// The loaded level as `CLevel` keys it: `+0x280` (upper-cased dungeon `NAME`), `+0x1a4`
    /// (depth), `IS_TOWN` (+0x86), `DONTSTORE` (+0x5a). Empty for a plain layout.
    std::string level_dungeon;

    int32_t level_depth = 0;

    /// `CLevel +0x1a8`, the floor's monster level (`place_dungeon_floor`).
    /// INVENTED: 1 for a level no floor roll set.
    int32_t level_monster_level = 1;

    bool level_is_town = false;

    /// The template's `ALLOW_PET_RETURN` (+0x85); false for a plain layout.
    bool level_allows_pet_return = false;

    /// Empty until `setOpen`: `CWaypointMenu::CWaypointMenu`
    /// clears its flags (X360 `+0x108`, `+0x109`) and row lists (`+0x29c`, `+0x2ac`).
    std::vector<tl::core::WaypointRow> waypoint_rows;

    int waypoint_selected = 0;

    bool waypoint_stick_held = false;

    tl::core::OptionsMenu pause_menu;

    tl::core::SettingsMenu settings_menu;

    tl::core::HowToMenu howto_menu;

    bool howto_stick_held = false;

    /// `CGameGlobals +0xf0`: `[CONTEXTTIPS]`, the console's wording when its data is mounted.
    tl::core::ContextTipTexts tip_texts;

    bool tip_texts_loaded = false;

    uint32_t tip_pet_pickups = 0;

    /// `CCinematics` (`media/cinematics/*.dat`), read on first use.
    tl::core::Cinematics cinematics;

    bool cinematics_loaded = false;

    /// `CCinematicMenu +0x30`: the cinematic screen is up.
    bool cinematic_open = false;

    /// `CCinematicMenu +0xf8`: the music's share, a quarter under a cinematic; 1 from
    /// `CCinematicMenu::CCinematicMenu`.
    float cinematic_music = 1.0f;

    /// What every quest has its characters say, and how long for (core/dialog.h): filled
    /// by load_quest_book(), handed to every machine by wire_script().
    tl::core::DialogBook dialog_book;

    /// The units this level places, with what their own `.DAT` authors (core/units.h).
    tl::core::UnitBook unit_book;

    unsigned spawn_serial = 0;

    /// Skill `NAME`, lower-cased, to what it plays and how far it reaches, from every
    /// `.DAT.adm` under `media/skills/` (files are not named after their skills).
    std::map<std::string, tl::core::AttackSkill> skills;

    bool skill_files_loaded = false;

    /// The player's skills: every skill file by upper-cased `NAME` (a `SkillDef` points
    /// into it) and the screen's book. The Xbox copies under `xbox/media/skills/` win.
    struct PlayerSkillFile {
        tl::formats::AdmFile file;
        tl::core::SkillDef def;
    };

    std::map<std::string, std::unique_ptr<PlayerSkillFile>> player_skill_files;

    bool player_skill_files_loaded = false;

    tl::core::SkillBook player_skills;

    /// Where the class's `[SKILL]` blocks were read from, for the log.
    std::string player_skills_source;

    /// `media/EffectsList.dat` and every affix file (core/affix_book.h); console copies
    /// under `xbox/media/affixes/` win.
    tl::core::EffectTypeTable effect_types;

    tl::core::AffixBook affixes;

    /// `CSets` (`media/sets/`), loaded with the affixes it names.
    tl::core::ItemSetBook item_sets;

    /// `CUnitThemes` (`media/unitthemes/`), read by `load_unit_themes()`.
    tl::core::UnitThemeBook unit_themes;

    bool unit_themes_loaded = false;

    bool effect_data_loaded = false;

    /// `GLOBALS.DAT`'s item-roll figures (`CGameGlobals`, `item_roll_context()`).
    tl::core::ItemRollGlobals item_roll_globals;

    bool item_roll_globals_loaded = false;

    tl::core::EffectManager::Context item_effect_context_;

    /// Rebuilt whenever the player's stats change: what `skill_stats_text` and
    /// `skill_stat_bonuses` read. Holds `std::function`s that capture `this`.
    tl::core::EffectDisplayContext skill_effects;

    /// The player's own `CEffectManager` (`CCharacter +0x1b8`), kept in step by
    /// `refresh_skill_effects`, ticked by `update_player_effects` (core/effect_manager.h).
    tl::core::EffectManager player_effects;

    /// `GLOBALS.DAT`'s regeneration rates; see load_recharge_rates().
    tl::core::RechargeRates recharge_rates;

    bool recharge_rates_loaded = false;

    /// The point stores before it casts (X360 `CCharacter +0x1c0`,
    /// Linux `+0x228`); unset, `--cast` aims as the pad at rest.
    std::optional<tl::core::Vec3> pad_skill_target;

    /// The player's `+0x228` (X360 `+0x1c0`): the last aimed-at point, from
    /// `setTargetDirection`, `castSkill` and `turnTowardPosition`;
    /// read by `step_player_skill_turn`.
    tl::core::Vec3 player_turn_point{};

    /// The player's `+0x234` (X360 `+0x1cc`): its last aim direction, normalised
    /// (`setTargetDirection`, `turnToward`).
    tl::core::Vec3 player_turn_direction{0.0f, 0.0f, 1.0f};

    /// The player's running skills and cooldowns (`CSkillManager::update`).
    ///
    tl::core::SkillRunner player_skill_runner;

    /// The worn items' skill managers by equip location (`CItem +0x1c8`, X360 `+0x184`),
    /// keyed to the item's name and emptied when it changes.
    std::array<tl::core::SkillRunner, 12> worn_skill_runners;

    std::array<std::string, 12> worn_skill_runner_items;

    /// Unit `NAME`, lower-cased, to its `.DAT.adm` (files are not named after units); built
    /// once, on the first level that needs it.
    std::map<std::string, std::string> monster_files;

    bool monster_files_loaded = false;

    /// The player character's progression and attributes (stats, level, exp, journal).
    tl::core::CharacterProgression player_character;

    /// The player character's inventory (equipment, potions, scrolls, items).
    tl::core::Inventory player_inventory;

    /// The level's authored `LEVELNAME`, which the HUD prints in the corner.
    std::string level_display_name;

    /// The level's own `RIMLIGHT`, which every character in it carries
    /// (`CLevel::addCharacter`).
    std::string rim_light_texture = kDefaultRimLight;

    /// The layout this level was loaded from, as asked for (which level this is: the
    /// pet's town trip and the town timer read it).
    std::string level_reference;

    bool dungeon_transition_pending = false;

    bool warp_pending = false;

    std::string warp_dungeon;

    int warp_floor = 1;

    /// `CGameClient +0x10a0` and `+0x10a4` as `warpLevels` left them: level delta and
    /// absolute depth, for `performWarp`'s entry type.
    int32_t warp_level_delta = 0;

    int32_t warp_level_absolute = 0;

    /// The warp's `WAYPOINT` (`CGameClient +0x10ba`): a Warper that authors it,
    /// or a row of the waypoint menu.
    bool warp_waypoint = false;

    /// The two banners: `CGameUI`'s `setBigText` (`+0x1980`)
    /// and `setSmallText` (`+0x1990`). Each holds, then
    /// `CGameUI::updateIngameUI` fades it by 2.0 a second.
    struct Banner {
        std::string text;
        float left = 0.0f;
        float alpha = 0.0f;
    };

    Banner big_banner;

    Banner small_banner;

    /// The level's `LEVELNAME` (template `+0x770`) and `SHOWFLOORNUMBER` (`+0x76c`), what
    /// `CLevel::getName` (`level_display_name`) and the arrival banner are made of.
    std::string level_name_authored;

    bool level_shows_floor = true;

    bool level_has_rules = false;

    std::map<std::string, std::string> preloaded_particles;

    bool preloaded_particles_read = false;

    /// Whether a level's music and ambience start when it loads (off for a frame dump).
    bool audio_enabled = true;

    /// The player's `+0x1ec` / `+0x1f0`: the damage taken and not yet shown.
    tl::core::DamageDisplay player_damage_shown;

    /// The player's target (`CCharacter +0x340`) while it is someone to talk to; the script
    /// reads it through `set_player_target` (`idleAI`).
    int64_t talk_target_node = 0;

    int talk_target_machine = kNoMachine;

    static constexpr int kNoMachine = -2;

    /// The console's X (core/attack_button.h): hold position (`+0x1fe`), "down at the last
    /// dispatch" (`+0x55`) and the 0.1 s repeat clock (`+0x40`).
    tl::core::AttackButton attack_button;

    /// Whether the skip button was already down last frame.
    bool skip_held = false;

    /// DIVERGENCE (2026-09-30): a debug menu, as a pad cannot type into `CConsole`;
    /// drawn on `modalmenu_xenon.layout`, a row of the Pause Menu (`core::options_row`).
    bool debug_menu_open = false;

    /// `CPlayer +0x790` (`CPlayer::getIsInGodMode`): `CCharacter::modifyHP`
    /// changes nothing while set.
    bool god_mode = false;

    /// A sound definition: `CSoundData` (`core/sound_data.h`); layouts and lines name the
    /// definition, not the file.
    using SoundDefinition = tl::core::SoundData;

    /// `CSoundBankDataInformation`, every `media/sounds/` table: a bank reaches a
    /// definition by `NAME`, a layout's `Sound` node or a particle's sound by `GUID`.
    tl::core::SoundDataInformation sound_info;

    bool sound_files_loaded = false;

    /// Decoded effects, by archive path, shared with every voice playing one; dropped with
    /// the archive's caches on remount (`mount_resources()`).
    std::map<std::string, tl::core::SamplePtr> samples;

    std::minstd_rand recording_picker{static_cast<std::minstd_rand::result_type>(
        std::chrono::steady_clock::now().time_since_epoch().count())};

    /// `CSoundManager`'s queue of global samples.
    tl::core::VoiceQueue voice_queue;

    /// Slots already reported as unauthored, one line each.
    std::set<int> reported_voices;

    std::string dialog_sound_key;

    /// The track the level authors, for `Play Level Music` to put back.
    std::string level_music;

    /// The level's `Music` nodes (`CMusicObject`) by node id, made on first input and updated
    /// every frame (`CMusicObjectDescriptor::update`).
    std::map<int64_t, tl::core::MusicObject> music_objects;

    tl::formats::CmpScene script_layout;

    std::set<int64_t> script_gated;

    /// The quest dialog's share of `interactive_menus`.
    bool quest_dialog_interactive = false;

    /// A particle file's `CParticleCache` extras (`LoadParticle`,
    /// copied by `createParticleCache`): one-shot `Sound`s by GUID
    /// with `VOLUME` (+0x40, +0x58) and `Camera Shake`s (+0xc0), in file order, by path.
    /// `CParticleCache::CParticleCache(SceneManager*, CSoundBank*)`.
    ///
    struct ParticleFileExtras {
        struct Sound {
            std::string guid;
            float volume = 1.0f;
        };
        std::vector<Sound> sounds;
        std::vector<tl::core::CameraShake> shakes;
    };

    std::map<std::string, ParticleFileExtras> particle_file_extras;

    /// GUID to mesh paths, from the tileset index: every `FILE` of the record, in order (the
    /// variants one of which each copy rolls, `CRoomPiece::initRoomPiece`).
    std::map<int64_t, std::vector<std::string>> tileset;

    bool tileset_loaded = false;

    /// GUID to collision mesh paths (`COLLISIONFILE`, in order), from the same index;
    /// not drawn (src/core/collision.h).
    std::map<int64_t, std::vector<std::string>> tileset_collision;

    /// Each piece's `PRERENDER` (-1) / `POSTRENDER` (+1), MeshPlacement::render_phase.
    std::map<int64_t, int> tileset_render_phase;

    /// Every `PIECE` GUID the index holds, drawn or not.
    std::set<int64_t> tileset_known;

    /// `CRoomPieceDataInformation +0x10` (console `+8`): the last piece named `ERROR`
    /// (`ParseRoomPieceData`), drawn for an unknown GUID; 0
    /// with none (`CRoomPieceDataInformation::CRoomPieceDataInformation`).
    ///
    int64_t tileset_default_guid = 0;

    /// The level's collision geometry, in world space.
    tl::core::CollisionWorld collision;

    /// The level's pathable grid (`CLevel::bakeCollision`, core/passability.h).
    tl::core::PassabilityMap passability;

    /// The `Unit Trigger` nodes placed with their own collision: candidates for
    /// `CBaseUnit::addToAvoidanceMap` once `passability.bake()` stands; drained after it.
    std::vector<int64_t> door_trigger_nodes;

    /// Which of `door_trigger_nodes` marked the map (for `CBaseUnit+0x19b`), to take back out
    /// on opening (`CBaseUnit::removeFromAvoidanceMap`).
    std::set<int64_t> door_avoidance_added;

    /// The `Unit Trigger`s a `Not Collidable` switched off (`CBaseUnit +0x18d`
    /// clear), which the level state carries in `CItemSaveState +0x5e`.
    std::set<int64_t> trigger_not_collidable;

    /// The level's own file's `Path Node Occupation` nodes whose cells are in
    /// the map (`CPropertyNode +0x150`); `apply_path_blockers` starts it over.
    std::set<int64_t> level_path_blockers_on;

    /// A path blocker node's box as `updatePathNodes` hands it to the level, for the linked
    /// layouts' and rooms' `Path Node Occupation` regions.
    struct PathBlocker {
        bool circle = false;
        tl::core::Vec3 centre;
        /// `+0x120`, `updateBoundingBox`'s bounds of the turned box.
        tl::core::Vec3 low;
        tl::core::Vec3 high;
        /// The node's own scale X (`BOX WIDTH`, else `RADIUS`): a circle's
        /// radius, and what `killOrMoveBaseUnits` measures with.
        float scale_x = 0.0f;
    };

    struct MachinePathBlocker {
        PathBlocker box;
        /// `CPropertyNode +0x150`: its cells are in the map; `updatePathNodes`
        /// adds only while clear and removes only while set.
        bool applied = false;
    };

    std::map<int64_t, MachinePathBlocker> machine_path_blockers;
    /// The enabled `Path Node Occupation` nodes of the layouts placed, in the level's frame, in
    /// the order `CLevel::loadRoomLayout` (cases 8 and 9) walks them; a floor
    /// lays them after its bake.
    std::vector<PathBlocker> loaded_path_blockers;
    /// They were laid as the floor loaded: the room machines' copies find their cells in.
    bool path_blockers_laid = false;

    /// The `No Spawn Region` `Property Node`s as boxes, in the order `CLevel::loadRoomLayout`
    /// (case 3) adds them to `CLevel +0xd8`.
    std::vector<tl::core::NoSpawnRegion> no_spawn_regions;

    /// The player character, when --play is on (off: a frame dump wants a fixed pose).
    tl::core::Player player;

    bool player_control = false;

    /// The camera's compass heading (radians about +Y) and subject-to-camera vector, from
    /// the layout's authored camera (follow_subject()).
    float camera_yaw = 0.0f;

    /// The animation the player controller last asked for, applied once per change.
    std::string player_animation;

    /// Everything the player character's own `.DAT` chain authors (`[EQUIPMENT]`,
    /// `WEAPON_SCALE`, speeds), from load_player_stats().
    tl::core::UnitStats player_stats;

    /// The weapon whose attack is in use (`CCharacter +0x3ec`, Linux `+0x498`), and the
    /// entity drawing the right hand's (the left's when the right is empty).
    tl::core::ItemStats weapon;

    bool has_weapon = false;

    /// Where `weapon` is equipped: 0 the right hand, 1 the left, -1 bare hands.
    int attack_location = -1;

    /// `CCharacter +0x2b2` (Linux `+0x37e`): flipped by every `performAttack`;
    /// with both hands armed the hands swing in turn.
    bool alternate_hand = false;

    /// The animation class that weapon swings on ("Polearm", "Rslash", "Attack"; core/weapons.h).
    std::string attack_class = "Attack";

    /// A swing in progress; the hit lands on the manifest's `HIT` frame
    /// (core/animation_manifest.h).
    struct Swing {
        bool active = false;
        std::string animation;  ///< the `.SKELETON` stem being played
        float elapsed = 0.0f;   ///< seconds into it, at the played rate
        float hit_at = 0.0f;    ///< when the blow lands
        float length = 0.0f;    ///< how long the whole thing runs
        bool landed = false;
        int64_t target = 0;  ///< the node aimed at, 0 for "whatever is in reach"
    };

    Swing swing;

    /// The animation a cast queued: `CCharacter::castSkill`
    /// blends it in for `duration`, then `IDLE` or the loop; its keys fire `EVENT_TRIGGER`
    /// (`CCharacter::updateSkillKeys`).
    struct SkillPlayback {
        bool active = false;
        std::string animation;  ///< the `.SKELETON` stem playing
        int index = -1;         ///< its place in the model's list
        float time = 0.0f;      ///< the animation's own clock, in seconds
        float length = 0.0f;
        float speed = 1.0f;
        float held = -1.0f;  ///< frame time left on the cue; -1 loops
        int then_loop = -1;  ///< queued after it, looping
    };

    SkillPlayback skill_playback;

    /// `CCharacter +0x398` and the cast flags beside it.
    tl::core::ActiveSkill player_active_skill;

    /// The character's animations in manifest order with lengths (`CGenericModel +0x1e0 ->
    /// +0x28`), built once per mesh.
    tl::core::AnimationCatalog player_catalog;

    /// The node the character is fighting (`CCharacter::setTarget`, `+0x340`); outlives a
    /// swing, read by `CMonster::idleAI` for the pet.
    int64_t player_target_node = 0;

    /// The target item (`CCharacter +0x350`) as a skill sees it: a breakable `castSkill`
    /// found and aimed at; 0 for none. Kept until `setTarget`,
    /// a position aim, or the breakable leaving (`notifyOfDeletion`).
    int64_t player_skill_item = 0;

    /// How much life something has left, by layout node id (one unit name covers several);
    /// seeded on the first hit.
    struct Health {
        float current = 0.0f;
        float max = 0.0f;
        bool dead = false;
    };

    std::map<int64_t, Health> unit_health;

    Health player_health;

    /// The player's own death: `CCharacter::alive` false (AI state 5, then 6) and
    /// `CPlayer +0x884`, the delay before the death screen.
    struct PlayerDeath {
        bool dying = false;
        std::string animation;  ///< the `DIE` animation `findRandomAnimation` chose
        float timer = tl::core::kDeathTimerStart;
    };

    PlayerDeath player_death;

    /// A spawned instance's own level and champion-ness, by node, set at `spawn_unit()`;
    /// `level == 0`: none (readers use `effective_monster_level()`).
    struct SpawnLevel {
        int level = 0;
        bool champion = false;
    };

    std::map<int64_t, SpawnLevel> unit_spawn_level;

    /// The authored stat curves under `media/graphs/stats/`; see `core/stats.h`.
    tl::core::StatBook stat_graphs;

    bool stat_graphs_loaded = false;

    /// `media/graphs/camerashakes/`: the curves a `CCameraShake` names.
    tl::core::StatBook camera_shake_graphs;

    bool camera_shake_graphs_loaded = false;

    /// `CGameSpeed`, from `CMasterResourceManager::CMasterResourceManager`
    /// with its own `DecelerateGameSpeed.dat`
    /// (`CGraph::loadGraph`); stepped by advance() (load_game_speed()).
    tl::core::StatGraph game_speed_curve;

    tl::core::GameSpeed game_speed{&game_speed_curve, &game_speed_curve};

    bool game_speed_loaded = false;

    /// Every item in the game, by name, from `media/masterresourceunits.dat`.
    tl::core::ItemBook items;

    /// The master table's `UNIT_GUID`s (`CResourceManager::getUnitDataByGuid`,
    /// `getUnitGuidByDataGroup`), by `NAME`; filled by `ensure_item_index`.
    std::map<int64_t, std::string> master_unit_by_guid;

    std::map<std::string, int64_t> master_guid_by_unit;  ///< by upper-cased `NAME`

    /// The `.ANIMATION` manifest beside each character mesh, by mesh directory.
    std::map<std::string, tl::core::AnimationManifest> manifests;
    /// `manifests`' entries by the mesh path asked with (its directory picks the entry).
    std::unordered_map<std::string, const tl::core::AnimationManifest*> manifests_by_mesh;

    /// The sleeping mimic the same pass points at, by machine and node. DIVERGENCE: the PC
    /// targets it under the pointer (`CGameClient::updateIngame`); a pad cannot.
    int mimic_targeted_machine = -2;

    int64_t mimic_targeted_node = 0;

    /// Fishing (`core/fishing.h`): the player's AI states 0x20..0x22 and the
    /// bobber (`CPlayer +0x850..+0x860`).
    tl::core::FishingState fishing_state = tl::core::FishingState::Off;

    tl::core::FishingBobber bobber;

    /// The fishing animation playing, and its speed (`blendAnimation`'s).
    std::string fishing_animation;

    /// A one-shot fishing animation, and whether it has run out.
    bool fishing_animation_once = false;

    /// What `queueBlendAnimation` put behind the one-shot fishing animation.
    std::string fishing_queued;

    /// The player's own level (`award_experience()`), and what a placed monster with no
    /// `unit_spawn_level` scales its curves at.
    int character_level = 1;

    /// What a `PLAYER` `Animation Controller` has the character play
    /// (`CAnimationPlayer::playAnimation`), and whether it loops.
    std::string scripted_player_animation;

    bool quest_book_loaded = false;

    /// A quest no NPC initialised is worth 1 level (`CQuest +0x1ec` from the constructor);
    /// `--quest-level` moves it for `--accept-quest`.
    int quest_level_for_offer = 1;

    /// The three classes a new character can be, in the order the creation
    /// screen shows them. Empty until load_player_classes() has run.
    std::vector<tl::core::PlayerClass> player_classes;

    /// The character's class by `NAME`, or empty (a `--mesh` run): this, not the mesh's
    /// filename, identifies the class.
    std::string player_class;

    /// `GLOBALS.DAT`'s label colours (`core/item_label.h`), read with the item
    /// figures.
    tl::core::ItemLabelColours item_label_colours;

    std::map<std::string, bool> breakable_units;

    /// `NONSELECTABLE` by prop name, once each (`non_selectable_prop()`).
    std::map<std::string, bool> non_selectable_units;

    /// The collision groups the range took out (`CTriggerUnit::update`), so it puts back
    /// only what it took.
    std::set<int64_t> trigger_range_off;

    /// A merchant: its inventory (`CCharacter +0x490`) and clock (`+0x388`,
    /// `CCharacter::update`), by node (core/merchant.h).
    struct MerchantState {
        std::string unit;
        tl::core::Inventory inventory = tl::core::make_merchant_inventory();
        bool gambler = false;
        float clock = 0.0f;
    };

    std::map<int64_t, MerchantState> merchants;

    /// INVENTED: a `CItemSaveState` here has no affixes, sockets or rolled armour, so the
    /// stock copies are kept by level (`dungeon:depth`) and node.
    std::map<std::string, std::map<int64_t, MerchantState>> remembered_merchants;

    /// Placed characters already asked whether they are merchants.
    std::set<int64_t> merchant_checked;

    /// A Transmuter's own inventory (`CCharacter +0x490`), by node: the combine's slots 15-18
    /// are its, and stay when the screen closes.
    struct CombinerState {
        std::string unit;
        tl::core::Inventory inventory;
    };

    std::map<int64_t, CombinerState> combiners;

    /// INVENTED, as `remembered_merchants`: the copies the level state cannot
    /// carry, by level and node.
    std::map<std::string, std::map<int64_t, CombinerState>> remembered_combiners;

    /// The combiner whose screen is open.
    int64_t combining_with = 0;

    /// `CRecipes` (`media/recipes/`), read by `load_recipes()`.
    tl::core::RecipeBook recipes;

    bool recipes_loaded = false;

    /// `CRandomNames` (`media/RANDOMNAMES.DAT`), read with the quests.
    tl::core::RandomNames random_names;

    /// What the bags tell the quest log: `CEquipment::addedToInventory`
    /// raises event 0 for the player's and the pet's bags,
    /// `removedFromInventory` event 5 from any (`housing_item_events`).
    tl::core::InventoryItemEvents bag_item_events;

    tl::core::InventoryItemEvents housing_item_events;

    int reflect_depth = 0;

    /// The player's animal (core/pet.h); its behaviour is the level script's (a character of
    /// the level's machine). This side owns the model, the voice lines and the map icon.
    tl::core::Pet pet;

    /// The pet's id in the level script (`kSpawnedNodeBase`), new with each model.
    int64_t pet_node_id = 0;

    /// A layout file as parse_cmp() reads it, once per level. Null when it does not read; a
    /// parse failure is kept (`ok` false). By pointer, so a nested placement cannot move a
    /// layout its parent is walking.
    std::unordered_map<std::string, std::unique_ptr<const tl::formats::CmpScene>> layout_files;

    /// The spawn tables a roll reads and its context, read on first use.
    struct SpawnTables {
        std::map<std::string, tl::core::SpawnClass> classes;
        std::map<std::string, tl::core::SpawnUnitData> units;
        tl::core::SpawnContext context;
    };

    /// The spawn tables the drops roll (`drop_death_loot`), kept for the level.
    SpawnTables loot_tables;

    bool loot_tables_built = false;

    std::map<std::string, std::string> prop_files;

    bool prop_files_loaded = false;

    /// What a `Unit Trigger`'s `MODEL` (a unit, `cmp_field::unit_trigger`) is made of.
    /// `ANIMATION_STATE_n` is played once and held, not looped (`CTriggerUnit::setTriggerState`
    /// `blendAnimation(idx, false, 0.1, 1.0, -1.0)`); `_LOOP_` loops; a transition
    /// runs first.
    struct TriggerProp {
        std::string mesh;            ///< `RESOURCEDIRECTORY` + `MESHFILE`
        std::string collision;       ///< + `COLLISIONFILE`, when it has one
        std::string resting;         ///< `ANIMATION_STATE_1`, played once
        std::string resting_loop;    ///< `ANIMATION_STATE_LOOP_1`, looped
        std::string activated;       ///< `ANIMATION_STATE_2`, played once
        std::string activated_loop;  ///< `ANIMATION_STATE_LOOP_2`, looped
        std::string opening;         ///< `ANIMATION_TRANSITION_STATE_2`
        std::string closing;         ///< `ANIMATION_TRANSITION_STATE_1`
        /// `CTriggerUnit::unitInit`: `MAXSTATES` (2 unless
        /// authored), and `LOOPTYPE`'s index in `gTRIGGER_LOOP_TYPE_NAMES`, -1 for none.
        int32_t max_states = 2;
        int32_t loop_type = -1;
        /// `addSoundsToState`: `SOUND_STATE_ONE` / `_TWO`,
        /// else `INTERACT_SOUND`.
        std::string state_sound[2];
        /// A props unit file names it, so `setUnitModelName`
        /// runs `unitInit`; it may draw nothing.
        bool found = false;
        bool collideable = true;  ///< `COLLIDEABLE`
        /// `BLOCK` (`CBaseUnit +0x156`, `unitInit`): whether a ray meets the
        /// collision model (`CLevel::objectRayCollision`).
        bool block = false;
        /// `LAYOUT_STATE_1` and `LAYOUT_STATE_2` (`CTriggerUnit +0x2b0`).
        std::string state_layout[2];
        /// `OCCUPIESNODES`, `CBaseUnit +0x18f`: without it `addToAvoidanceMap`
        /// leaves the grid alone.
        bool occupies_nodes = true;
        /// `SHADOWS` (`CBaseUnit +0x1a9`, true by default).
        bool shadows = true;
        /// `CItem +0x210`, the floor label: `DISPLAYNAME`, `NAME` or `ITEM`
        /// (`CItem::unitInit`).
        std::string display_name;
        std::string unittype;  ///< `UNITTYPE`, for the label's two type tests
        /// `CTriggerUnit::unitInit`: `[TREASURE]`'s
        /// `SPAWNCLASS` (`+0x2d0`), `MINCOUNT`, `MAXCOUNT` (1 each; constructor).
        ///
        std::string treasure_class;
        int32_t treasure_min = 1;
        int32_t treasure_max = 1;
        /// `CBaseUnit::unitInit`: `[SKILL]` blocks in order.
        std::vector<std::string> skills;
        std::vector<uint32_t> skill_levels;
        /// `+0x194`, `COLLISION_RADIUS` (`CTriggerUnit::unitInit`), 0.3 by default
        /// (`CBaseUnit::CBaseUnit`): the trap is cast that far below.
        float collision_radius = 0.3f;
        /// `SCALE` (1) + `randomBetweenVolatile(0, SCALE_VARIATION)` (`CBaseUnit::unitInit`).
        ///
        float scale = 1.0f;
        float scale_variation = 0.0f;
    };

    std::map<std::string, TriggerProp> trigger_props;

    /// A player skill's `[EVENT_*]` block as its layouts remember it: the `CSkillEvent` a
    /// `Missiles` spawner fires for (`CSkillEvent::missileApplyingEffects`).
    struct SkillEventSource {
        std::string skill;
        uint32_t level = 0;
        const tl::formats::AdmSection* event = nullptr;     ///< the block (`+0xa0`)
        const tl::formats::AdmSection* property = nullptr;  ///< its level (`+0x90`)
        /// That level's `DURATIONOVERRIDEMS` in seconds (`+0xa4`, inherited).
        float duration_override = 0.0f;
        tl::core::SkillTargetFilter unauthored = tl::core::SkillTargetFilter::Enemy;
        int32_t target_alignment = 2;  ///< the skill's `TARGET_ALIGNMENT`
        tl::core::SkillEventSpec spec;
        int group = 0;            ///< `+0x48`, the event's `EVENT_*` group
        int32_t target_type = 0;  ///< the level property's `TARGET_TYPE` (`+0x70`)
        /// The skill's owner when a placed unit; null is the player.
        tl::core::LevelScript* caster_machine = nullptr;
        int64_t caster_node = 0;
        /// The copy of the skill that fired the event (`SkillRunState::copy`).
        uint32_t run = 0;
    };

    /// Each unit model's animations in manifest order, by mesh.
    std::map<std::string, tl::core::AnimationCatalog> unit_catalogs;

    /// The caster a unit's `SkillCastContext` points at, kept alive here.
    tl::core::SkillCaster unit_cast_caster;

    /// A character `findCharacterWithinView` can answer: the player, or a
    /// machine's node.
    struct ViewUnit {
        bool player = false;
        tl::core::LevelScript* machine = nullptr;
        int64_t node = 0;
        /// A breakable by id when nonzero (`SkillEventTarget::item`).
        int64_t item = 0;
    };

    /// Skills a player skill's events executed (`EXECUTE_SKILL`), added hidden by
    /// `CSkillManager::addSkill`, by upper-cased name.
    std::map<std::string, tl::core::KnownSkill> executed_skills;

    /// `CLevel +0x200`: who watches which unit (core/unit_observers.h); per level.
    tl::core::UnitObservers unit_observers;

    /// Set while the player's `updateHP` takes effect damage: death broadcasts 4, then 1.
    bool player_hurt_by_effects = false;

    /// Who the possibly-lethal effects credit, while `player_hurt_by_effects` is set.
    int64_t player_effect_killer = tl::core::LevelScript::kNoAttacker;

    std::map<std::string, const tl::formats::CmpScene*> cached_layouts;

    // ---- procs and a weapon's own skills (`core/weapon_procs.h`) ----------
    /// The player's known list as procs ask of it (`CSkillManager +0x60`): what
    /// `CBaseUnit::activateEffect` taught; one copy per
    /// name. `CCharacter::deactivateEffect`'s delete is
    /// not carried.
    tl::core::SkillCopies player_proc_skills{true};

    /// `CCharacter +0x708`: 1.0 when an own proc other than `CAST SKILL AT TARGET` casts
    /// (`executeProcs`), run down by `CCharacter::update`,
    /// waited out by `CAST SKILL ON STRUCK`.
    float player_proc_lockout = 0.0f;

    /// What a proc or weapon skill is cast at: a character, a breakable (by id), or nothing,
    /// with its centre and radius taken when made (a breakable's procs come after it broke).
    struct ProcTarget {
        tl::core::LevelScript* machine = nullptr;
        int64_t node = 0;
        int64_t breakable_id = 0;
        bool placed = false;
        tl::core::Vec3 centre{};
        float radius = 0.0f;
        bool any() const { return placed; }
    };

    /// `CMissilePreloader`'s missiles by their upper-cased name, built once.
    std::map<std::string, const tl::formats::CmpNode*> missiles_by_name;

    bool missiles_indexed = false;

    /// The rules file each main-menu backdrop was chosen from (`Renderer::menu_layout_for`),
    /// by `backdrop_key` of its layout (file names do not lead back; `CLevel::loadRoomLayout`).
    std::map<std::string, std::string> backdrop_rules;

    tl::core::UnitTypeHierarchy unit_types;

    bool unit_types_loaded = false;

    GameConfig config;

    Ogre::SceneNode* subject_node = nullptr;

    Ogre::Entity* subject = nullptr;

    /// `CGenericModel`'s animation half (core/model_animation.h) per entity
    /// `play_scene_animation()` drives; a track's index is this entity's own.
    struct EntityAnimation {
        tl::core::ModelAnimator animator;
        std::vector<Ogre::AnimationState*> states;
        /// What was last written to each state: only changes are written, so a state
        /// another path switched off stays off.
        std::vector<tl::core::ModelTrack> written;
    };

    std::map<Ogre::Entity*, EntityAnimation> entity_animations;

    std::string offered_by;

    /// The speaker's node, and whether the offered line is
    /// `MAKE_PET_ON_ACCEPT` (see accept_offered_quest()).
    int64_t offered_by_node = 0;

    bool offered_makes_pet = false;

    /// Every scene node a layout node produced, by its 64-bit id: what `Hide` and
    /// timelines act on.
    using NodeMap = std::multimap<int64_t, Ogre::SceneNode*>;

    NodeMap scripted_nodes;

    /// One machine per placed nested layout with a script of its own (a wire never crosses
    /// a file); each has its own node map, as copies share their layout's node ids.
    struct NestedScript {
        std::string reference;
        int64_t owner = 0;  ///< the `Layout Link` node in the parent
        /// The `Layout Link` of the parent machine's own file it was reached through (its
        /// `owner`, or a link above it when the files between own no machine): whose turn of
        /// the parent's activation activates it.
        int64_t turn_link = 0;
        std::string owner_name;
        /// The node map of the file holding that link (`Play` on the link is
        /// `CLayout::start` on its layout).
        const NodeMap* owner_nodes = nullptr;
        /// In the level's coordinates (`LevelScript::set_frame` before it is built); its
        /// characters join the level's list (`set_level_machines`).
        tl::core::LevelScript script;
        NodeMap nodes;

        bool running = false;
        bool retired = false;  ///< an ended skill event's: not stepped, off `level_machines`

        /// Where its nodes sit as `CLogicNodeState` records them: the room layout's index in
        /// `CLevel+0x10` (+0x30) and the parent hierarchy hash (+0x40) (`placing_scene`).
        uint32_t scene = 0;
        int64_t hierarchy = 0;

        /// Whether its first action has been logged (once, not every frame).
        bool reported = false;
    };

    /// A deque: `node_sink` points into one while its layout is placed, and a push_back
    /// part-way must not move it.
    std::deque<NestedScript> nested_scripts;

    bool level_dont_store = false;

    /// The template's `ALLOW_PORTALS` (+0x84); false for a plain layout.
    bool level_allows_portals = false;

    /// The settings when the caller keeps none (`GameConfig::settings`).
    tl::core::GameSettings own_settings;

    /// `CLevel+600` (a remembered state exists while the level is built) and
    /// `+0x1a1` (the level was put back from it).
    bool level_has_saved_state = false;

    bool level_restored = false;

    /// A unit's themes (`CBaseUnit +0x108..+0x150`) and the particles they hang on it
    /// (`CCharacter +0x738`), with what this engine drew for each trigger's particle.
    struct UnitThemeState {
        bool initialised = false;
        tl::core::UnitThemeSet themes;
        tl::core::ThemeParticleTriggers triggers;
        struct Drawn {
            std::vector<Ogre::SceneNode*> holders;
            /// `CParticleAnimationTrigger +0x38`: the node under the unit's for a particle
            /// attached on a bone, or the world node of one that does not attach.
            Ogre::SceneNode* tag = nullptr;
            bool configured = false;
        };
        std::vector<Drawn> drawn;
        /// `CCharacter::die` has stopped its particles.
        bool stopped_by_death = false;
        /// `loadModel`'s triggers (`load_model_triggers`) have been made.
        bool model_loaded = false;
        /// `+0x81` as the trigger list last saw it; false from `CSceneNodeObject`'s
        /// constructor, so the first show starts what the unit wears.
        bool shown = false;
        /// Whether `makeChampion`'s theme has been looked at for this unit.
        bool champion_checked = false;
    };

    struct SceneUnit {
        Ogre::Entity* entity = nullptr;
        std::string mesh;
        float rate = 1.0f;

        /// The layout node this unit was placed from (a unit name covers several).
        int64_t node = 0;

        /// What the layout put this unit on, so a reset can put it back.
        std::string animation;
        bool loops = true;

        /// What it is playing now (the script's last start, e.g. a swing), and the state:
        /// the manifest times `HIT` by animation name.
        std::string playing;
        Ogre::AnimationState* playing_state = nullptr;

        /// How far into `playing` the last frame left it, to spot the `HIT` frame once.
        float last_time = 0.0f;

        /// The unit's `ARMED` flag, which picks between two sets of animation files.
        bool armed = true;

        /// The rolled hands (`sync_unit_equipment`) are on it, or it has none.
        bool equipment_synced = false;

        /// The sound cursor, apart from `last_time` (which only `step_monster_swings()`
        /// advances, for hunting units).
        float last_sound_time = 0.0f;
        /// The animation `last_sound_time` was read in: a new one starts before
        /// its first key, not as a wrap of the last one's loop.
        std::string sound_animation{};

        /// Its themes and their particles; filled on first use.
        UnitThemeState themes{};

        /// The animation and time the particle keys were last looked at
        /// (`step_unit_particle_keys`).
        std::string key_animation{};
        float key_time = -1.0f;
    };

    std::multimap<std::string, SceneUnit> scene_units;
    /// `scene_units` by node, each list in the multimap's order; rebuilt on the next lookup
    /// after an entry is added or erased (`scene_units_changed`).
    std::unordered_map<int64_t, std::vector<SceneUnit*>> scene_units_by_node;
    bool scene_units_changed = true;

    /// The state each animation this engine started runs on, by entity and lower-cased
    /// `.SKELETON` stem, so `Stop` can switch off one animation (a skeleton's internal
    /// animation name need not match its file).
    std::map<std::pair<Ogre::Entity*, std::string>, Ogre::AnimationState*> unit_states;

    /// What each `Unit Spawner` has released and not taken back, by the spawner's node id
    /// (`Destroy Spawned Units` and `MAX ALLOWED` are per spawner).
    struct SpawnedUnit {
        Ogre::SceneNode* node = nullptr;
        Ogre::Entity* entity = nullptr;
        std::string unit;

        /// The unit's own id, from a range no layout uses (kSpawnedNodeBase).
        int64_t id = 0;
    };

    std::map<int64_t, std::vector<SpawnedUnit>> spawned;

    /// The player's themes and their particles.
    UnitThemeState player_themes;

    /// Outlives `player_cast_context`, which hands out a pointer to it.
    tl::core::SkillCaster cast_caster;

    /// The level-up particle's nodes under the player's (`CPlayer +0x778`,
    /// `CPlayer::unitInit`), replaced each level-up.
    std::vector<Ogre::SceneNode*> level_up_particle_nodes;

    /// Which script machine drives a unit: `kLevelMachine`, an index into `nested_scripts`,
    /// or `kNoMachine`; found through the scene node, as rooms share node ids.
    static constexpr int kLevelMachine = -1;

    bool script_ready = false;

    /// `CCameraControl +0x98` and `+0x94` (`setCinematicMode`).
    tl::core::CinematicCamera camera_cinematic;

    float camera_dolly = 0.0f;

    /// CAMERAMULT from the level rules; see apply_environment().
    float camera_mult = 1.0f;

    /// The held weapon's element particle (`CEquipment +0x3c8`), under its hand
    /// node, and the hand it rides.
    std::vector<Ogre::SceneNode*> weapon_element_particles;

    /// The enchant shrine a shrine's page works on (`CEnchantMenuXenon +0xf4`, set
    /// from the target item): its machine and `Unit Trigger` node.
    int enchant_shrine_machine = -2;

    int64_t enchant_shrine_node = 0;

    /// The level's fish slot class (`CLevelTemplateData +0x5a8`, slot 6 of
    /// `chooseUnitSpawners`); empty with no template.
    std::string level_fish_spawn_class;

    /// A `CBreakable` in the level: a barrel, an urn, a basket (core/breakable.h).
    struct BreakableWorld {
        std::string unit;  ///< its `NAME`
        tl::core::BreakableData data;
        tl::core::Breakable state;
        Ogre::SceneNode* node = nullptr;
        Ogre::Entity* entity = nullptr;
        /// Its collision group, and the id the `Unit Spawner` that released it
        /// knows it by.
        int64_t id = 0;
        bool has_collision = false;  ///< its collision model loaded (`+0x238`)
        /// Its range and fade, and whether it is on the near item list.
        tl::core::ItemPresence presence;
        bool listed = true;
    };

    /// In `CLevel::addItem` order, newest first. Level-
    /// local. INVENTED: no `CLevelState` field, so a restored level has none.
    std::vector<BreakableWorld> breakables;

    /// Items a missile's area reached (`CMissile::doAOEDamage`,
    /// `CItem::interact(nullptr)`), broken after the missiles step.
    struct PendingItemInteract {
        int64_t item = 0;
        bool by_player = false;  ///< the breaker `interact` is handed
    };

    std::vector<PendingItemInteract> pending_item_interacts;

    /// The level's items as the machines get them (`set_level_items`), offered to the damage
    /// shapes each frame (`CLevel::updateItems`).
    tl::core::LevelScript::LevelItems level_items;

    bool last_blow_critical = false;

    /// The last blow's direction (from the player to the target, turned by
    /// `rollAttack`'s jitter), knock-back force and critical, for `die`.
    tl::core::KillingBlow last_killing_blow;

    /// Wardrobe pieces worn on the subject, animated with the body (Renderer::equip()).
    std::vector<Ogre::Entity*> equipment;

    Ogre::Entity* pet_entity = nullptr;

    Ogre::SceneNode* pet_node = nullptr;

    std::string pet_mesh;

    /// The placed unit whose skill is running, set around its `SkillRunner` calls; null
    /// machine: the player.
    struct UnitEventCaster {
        tl::core::LevelScript* machine = nullptr;
        int64_t node = 0;
        Ogre::SceneNode* scene = nullptr;
        float radius = tl::core::kDefaultCollisionRadius;
    };

    UnitEventCaster unit_event_caster;

    /// A placed unit's cast animation, stepped for its keys as the player's is
    /// (`step_skill_animation`).
    struct UnitSkillPlayback {
        tl::core::LevelScript* machine = nullptr;
        int64_t node = 0;
        std::string animation;
        int index = -1;
        float time = 0.0f;
        float length = 0.0f;
        float speed = 1.0f;
        float held = -1.0f;
        int then_loop = -1;
    };

    std::vector<UnitSkillPlayback> unit_skill_playbacks;

    /// One missile in flight (`core/missile.h`) and what draws it.
    struct FlyingMissile {
        tl::core::Missile missile;
        SkillEventSource source;
        /// Fired by a spawner of the level's own layout (a trap): no owner or listener.
        bool level_placed = false;
        /// Fired by the player's weapon (`CEquipment::fireMissiles`): the weapon listens.
        bool by_weapon = false;
        /// Its event restarted (`CSkillEvent::reset`): no listener left, so
        /// `doDamageToCharacter`'s own blow and no death event.
        bool detached = false;
        Ogre::SceneNode* node = nullptr;  ///< carries `ACTIVE`
        std::vector<Ogre::SceneNode*> active;
    };

    /// A deque: a missile's blow can fire more (`step_missiles` holds one by reference).
    std::deque<FlyingMissile> missiles;

    /// A machine and its node map: the level's own, then every nested one in build order.
    struct MachineView {
        tl::core::LevelScript* script = nullptr;
        NodeMap* nodes = nullptr;
        uint32_t scene = 0;
        int64_t hierarchy = 0;
    };

    /// `CCameraControl`'s two shake lists (`+0x58`, `+0x70`), walked by
    /// update_game_camera() and cleared with the level.
    tl::core::CameraShakes camera_shakes;

    bool items_loaded = false;

    /// Units a `Unit Spawner` released with `GIVE LOOT` clear (+0x703, +0x704).
    std::set<int64_t> no_loot_units;

    /// `rollAttack`'s glancing flag (`applyDamage`'s 7th argument) and `blocked()`.
    bool last_blow_glancing = false;

    bool last_blow_blocked = false;

    /// `rollAttack` interrupted the target of the last blow.
    bool last_blow_interrupts = false;

    /// The character whose skill event is applying effects right now (an
    /// effect's `+0x48`): a placed unit, or with a null machine the player.
    tl::core::LevelScript* instant_caster_machine = nullptr;

    int64_t instant_caster_node = 0;

    /// The direction that event hands `applyAffixesAndEffects`
    /// for `CAffix::setDirection`; unset is `UNIT_Z`.
    bool effect_direction_set = false;

    std::array<float, 3> effect_direction{};

    /// Units released with `GIVE XP` clear (`+0x454` = 0).
    std::set<int64_t> no_xp_units;

    /// The level's list of characters (`CLevel +0x98`) as their machines: the level's own,
    /// then each room's in placing order (`LevelScript::set_level_machines`).
    std::vector<tl::core::LevelScript*> level_machines{&script};

    /// A `Particle` spawner's layouts (`CUnitSpawner +0x1d0`), by spawner and index.
    std::map<int64_t, std::map<uint32_t, std::vector<Ogre::SceneNode*>>> spawner_particles;

    /// A `CSkillEvent` hung on a skill event's damage shapes (`CSkillEvent::addListeners`),
    /// its hooks as callbacks.
    struct SkillEventListener final : tl::core::DamageShapeListener {
        std::function<bool(const tl::core::DamageShape&, const tl::core::DamageShapeUnit&)> can;
        std::function<bool(const tl::core::DamageShape&, const tl::core::DamageShapeUnit&)> act;
        /// `itemEffectedByDamageShape`; unset: nothing
        /// against the shape's own `interact`.
        std::function<bool(const tl::core::DamageShape&, const tl::core::DamageShapeItem&)> item;
        // `itemEffectedByDamageShape`,
        // through the listener's hook; unset, nothing against it.
        bool affect_item(const tl::core::DamageShape& shape,
                         const tl::core::DamageShapeItem& which) override {
            return !item || item(shape, which);
        }
        bool can_affect(const tl::core::DamageShape& shape,
                        const tl::core::DamageShapeUnit& who) override {
            return can && can(shape, who);
        }
        bool affect(const tl::core::DamageShape& shape,
                    const tl::core::DamageShapeUnit& who) override {
            return act && act(shape, who);
        }
    };

    std::vector<std::unique_ptr<SkillEventListener>> skill_event_listeners;

    /// Every `Unit Spawner` of a placed skill layout by node id (the file's own, so the latest
    /// cast's is kept), with its event and shape.
    struct SkillSpawner {
        SkillEventSource source;
        tl::core::SpawnerShape shape;
    };

    std::map<int64_t, SkillSpawner> skill_spawners;

    /// A player skill's `CSkillEvent` while it lives (`CSkill +0xb8`): its layout's nested
    /// machines, `first`..`end`.
    struct LiveSkillEvent {
        std::string skill;
        /// The copy of the skill that started it (`SkillRunState::copy`).
        uint32_t run = 0;
        size_t first = 0;
        size_t end = 0;
        /// `ATTACHOWNER`: the caster follows `shape` (`+0x40`, the first damage shape) of the
        /// layout placed at `at`, `facing`.
        bool attach_owner = false;
        bool ignores_units = false;
        bool ignores_obstructions = false;
        bool scale_distance = false;
        tl::core::LevelScript* shape_machine = nullptr;
        int64_t shape = 0;
        ogre::Vector3 at = ogre::Vector3::ZERO;
        ogre::Quaternion facing = ogre::Quaternion::IDENTITY;
        /// `+0x1fc`: origin to target on the ground, and `+0x160`, its length (within range).
        tl::core::Vec3 to_target{};
        float distance = 0.0f;
        int32_t skill_target_type = 0;
        /// Who cast it (`+0x50`): a placed unit, or the player (null machine).
        tl::core::LevelScript* caster_machine = nullptr;
        int64_t caster_node = 0;
        /// The skill loops (`CSkill +0xec` not -1): `stopAllParticles`' flag.
        bool loop = false;
        /// The particle systems its layout placed, by the nodes they hang from.
        std::vector<Ogre::SceneNode*> holders;
        /// Still in the skill's list (`CSkill +0xc0`); `stopSkill` resets it either way.
        bool listed = true;
        /// `updateEvent`'s line half: the systems with a `Line` emitter (`+0xa8`), driven with
        /// `ATTACH_LINE_EMITTER` (+0x147) or `..._TO_MAX_DISTANCE` (+0x148).
        std::vector<Ogre::SceneNode*> line_holders;
        bool attach_line_emitter = false;
        bool to_max_distance = false;
        bool collides = false;  ///< +0x149 `LINE_EMITTER_COLLIDES`
        /// `ATTACHTOTARGET` with no target or +0x148: the layout follows `far_point`.
        bool far_target = false;
        ogre::Vector3 far_point = ogre::Vector3::ZERO;
        int fire_from = 0;          ///< +0x108 `FIREFROMBONE`
        int fire_at = 0;            ///< +0x10c `FIREATBONE`
        float range = 0.0f;         ///< `CSkill::getRange` at the run's level
        float target_ahead = 0.0f;  ///< +0x164 `TARGET_AHEAD_OF_UNIT`
        /// The owner whose bones the line leaves from (`getOwnerCharacter`), and the target
        /// (`+0x80`).
        ViewUnit owner;
        bool has_target = false;
        ViewUnit target;
        bool line_reported = false;
    };

    std::vector<LiveSkillEvent> live_skill_events;

    /// `CSkillEvent +0x15c`: what is left of `DURATIONOVERRIDEMS` (`+0x158`), set by
    /// `startEvent`, run down by `updateEvent`;
    /// `canStop` waits on it.
    struct SkillEventHold {
        std::string skill;
        uint32_t run = 0;
        tl::core::LevelScript* caster_machine = nullptr;
        int64_t caster_node = 0;
        float left = 0.0f;
    };

    std::vector<SkillEventHold> skill_event_holds;

    /// While another unit's executed skill starts: its node, which the layouts ride.
    Ogre::SceneNode* event_caster_override = nullptr;

    /// The same unit by machine and node.
    tl::core::LevelScript* event_caster_override_machine = nullptr;

    int64_t event_caster_override_node = 0;

    /// `CSkill`'s `iUnitObserver` half: one copy of a caster's skill (null machine: player's)
    /// listening for a death its effect may cause.
    struct SkillObserver final : tl::core::UnitObserver {
        World* impl = nullptr;
        tl::core::LevelScript* caster_machine = nullptr;
        int64_t caster_node = 0;
        std::string skill;
        uint32_t run = 0;
        void unit_state_change(const void* machine, int64_t unit, int state) override;
    };

    std::map<std::tuple<tl::core::LevelScript*, int64_t, std::string, uint32_t>,
             std::unique_ptr<SkillObserver>>
        skill_observers;

    /// Particle layouts placed for a moment (a missile's `RELEASE`, `HIT`, `DIE`):
    /// `CMissile::killMissile` gives them 1.0 s (`+0x1b4`),
    /// then `handleDeathOfMissile` stops each.
    struct TransientParticles {
        std::vector<Ogre::SceneNode*> holders;
        float left = 0.0f;
        bool stopped = false;
        /// A character's own particle: taken down once its layout's timer stopped it.
        bool self_timed = false;
        /// A missile's own (`CMissile +0x100`): gone once none of its systems is still
        /// updating, whatever its layout's timer (`CMissile::handleDeathOfMissile`).
        bool missile = false;
    };

    std::vector<TransientParticles> transient_particles;

    /// A character a particle slot belongs to: the machine that runs it and its node; the
    /// player is both null.
    struct CharacterRef {
        const void* machine = nullptr;
        int64_t node = 0;
        bool operator<(const CharacterRef& o) const {
            return machine != o.machine ? std::less<const void*>()(machine, o.machine)
                                        : node < o.node;
        }
    };
    /// A character's slot `CParticle` (`CCharacter +0x4a8`): its placed layout, until the layout's
    /// timer has stopped it and its holders are destroyed (`CParticle::Stop(true)` handing the
    /// cache back). Where it was put and which file it is.
    struct SlotParticle {
        std::vector<Ogre::SceneNode*> holders;
        ogre::Vector3 at = ogre::Vector3::ZERO;
        std::string file;
    };
    std::map<std::pair<CharacterRef, int>, SlotParticle> slot_particles;

    Ogre::Entity* weapon_entity = nullptr;

    /// What `equip_unit_items` hung in a placed unit's hands, slots 0 and 1:
    /// the item's entity and its `UNITTYPE`.
    struct UnitHeldItems {
        Ogre::Entity* held[2] = {nullptr, nullptr};
        std::string unittype[2];
    };

    std::map<Ogre::Entity*, UnitHeldItems> unit_held_items;

    /// What an item's range is chosen from (`CItem::calculateActiveRange`):
    /// `UNIT_LIGHT_FADE`, the active ranges, `TRIGGER_NEAR_RANGE` (40).
    bool level_light_fade = true;

    float level_indoor_range = 16.0f;

    float level_outdoor_range = 24.0f;

    float level_trigger_near_range = 40.0f;

    /// The player's `+0x52e`, `INVINCIBLE` (only a `Game State Controller`'s `INVULNERABLE`,
    /// `performStateControl`): `modifyHP` holds the health at 1.
    bool player_invincible = false;

    /// Player Start's world position, and whether the layout had one.
    ogre::Vector3 player_start = ogre::Vector3::ZERO;

    /// The facing the level stands the player in (`CLevel +0x164`), as the player's yaw.
    float player_start_yaw = 0.0f;

    bool have_player_start = false;

    bool automap_initialized = false;

    /// `CDieMenu +0x30` (open), and the pad's highlighted choice. `CDieMenu::CDieMenu`
    /// starts closed, nothing chosen.
    bool death_screen_up = false;

    /// `setGameState(5)` with choice 2 away from a town: `warpLevels("Town", -99)`.
    bool death_town_pending = false;

    /// The `dropped_items` entry targeted for pickup (`CCharacter::setTargetItem`,
    /// `+0x350`), sought only with no unit target (`+0x340`).
    int item_targeted = -1;

    Ogre::Entity* item_highlighted = nullptr;

    /// An item on the floor (dropped, a monster's, gold, a quest's). Picked up by
    /// `CCharacter::getItemAI`/`getItem`
    /// via `setTargetItem`, chosen by
    /// core/targeting.h. Drawn as the console's (`step_ground_labels`,
    /// core/item_label.h); not transcribed: `CItem::update`'s light override.
    ///
    struct DroppedWorldItem {
        tl::core::ItemStats item;
        int count = 0;
        Ogre::SceneNode* node = nullptr;
        Ogre::Entity* entity = nullptr;

        /// The toss (`CEquipment +0x250` path, `+0x258` distance along it,
        /// `+0x25c` still flying) and the orientation it lands in (`+0x2f0`).
        tl::core::DropArc arc;
        float travelled = 0.0f;
        bool flying = false;
        bool spins = false;
        ogre::Quaternion rest = ogre::Quaternion::IDENTITY;

        /// `CBaseUnit +0x170/+0x178` for an item a quest placed; empty otherwise.
        tl::core::QuestUnitTag quest_tag;

        /// The id a `Unit Spawner` knows it by, or 0 for an item no spawner released.
        int64_t unit_id = 0;

        /// A `CItemGold`, not a `CEquipment`: amount, turn and size (`pile`), its growing in
        /// (`+0x234`, from 0.35) and `GOLD_SPARKLE`.
        bool is_gold = false;
        tl::core::GoldPile pile;
        float grow_left = 0.0f;
        std::vector<Ogre::SceneNode*> sparkle;

        /// `CEquipment +0x3d0` and `+0x3c8`: the drop particle and a weapon's element
        /// (`createParticles`, core/item_label.h).
        std::vector<Ogre::SceneNode*> particles;

        /// Its range and fade (`CItem +0x199`, `+0x204`, ...), and whether it is on the near
        /// item list (`CLevel +0x90`).
        tl::core::ItemPresence presence;
        bool listed = true;
    };

    /// Level-local: `restart()` replaces `Impl`, so a dropped item does not survive leaving.
    /// INVENTED: the original keeps its items in `CLevelState`; player drops have no field.
    std::vector<DroppedWorldItem> dropped_items;

    /// What the last item near list logged: listed, active, of all.
    std::array<size_t, 3> item_near_logged{static_cast<size_t>(-1), 0, 0};

    /// A `Unit Trigger`'s range and fade (a `CItem`), by instance key, made as `addItem`
    /// makes one the first time the walk meets it.
    struct TriggerPresence {
        tl::core::ItemPresence presence;
        bool listed = true;
        bool interactable = false;  ///< `ISA 0x20`
        bool shadows = true;        ///< `SHADOWS`
    };

    std::map<int64_t, TriggerPresence> trigger_presence;

    /// One `Unit Trigger` as the item walk sees it.
    struct TriggerItem {
        int64_t key = 0;
        int machine = kLevelMachine;  ///< `kLevelMachine` or a `nested_scripts` index
        int64_t node = 0;             ///< its `Unit Trigger` node in that machine
        /// its prop's nodes (none for a bare collision model)
        std::vector<Ogre::SceneNode*> nodes;
        const NodeMap* node_map = nullptr;  ///< its machine's nodes
        ogre::Vector3 at = ogre::Vector3::ZERO;
        TriggerPresence* state = nullptr;
    };

    /// The `Quest Item` and `Quest Boss` `Property Node`s, in the order
    /// `CLevel::loadRoomLayout` (cases 12 and 13) adds them
    /// to `CLevel +0x108` and `+0xf0`.
    struct QuestMarker {
        ogre::Vector3 position;
        ogre::Quaternion orientation;
        /// `CPropertyNode +0x114`: how many of `populate`'s NPCs stand on it.
        int32_t taken = 0;
    };

    std::vector<QuestMarker> quest_item_markers;

    std::vector<QuestMarker> quest_boss_markers;

    /// What each monster placed for a quest carries (`CBaseUnit +0x170/+0x178`), by node.
    std::map<int64_t, tl::core::QuestUnitTag> quest_unit_tags;

    /// A character's own name (`CCharacter +0x4c0`) by node: a quest champion's
    /// (`CQuestUnitData::spawnUnit`) or `UNIQUE_MONSTER_NAME`
    /// (`createUnit`); shown over `DISPLAYNAME`.
    std::map<int64_t, std::string> unit_names;

    /// The archive directory the subject's mesh came from, with a trailing slash; the
    /// mesh's references are qualified against it.
    std::string subject_dir;

    /// What the chosen class authors about itself, read once out of its `.DAT`
    /// chain by `load_player_stats()` and handed to `player_character`.
    tl::core::ClassProfile class_profile;

    std::string player_catalog_mesh;

    /// The portal props `CPlayer::createPortals` added to the level - the items
    /// `CLevel::deleteOpenPortals` takes away (ISA `TOWNPORTAL` or `WAYPOINTNODE`).
    struct OpenPortal {
        std::string unit;          ///< the `PROPS` unit's `NAME`
        std::string display_name;  ///< its floor label
        std::string unittype;
        ogre::Vector3 at = ogre::Vector3::ZERO;
        /// A `MAP PORTAL`'s `WARP` (0x71) effect: the dungeon it leads to,
        /// upper-cased (`CPlayer::openMapPortal`). Empty for the others.
        std::string warp;
        /// `CSceneNodeObject::setEnabled` (vtable +0x40): `CTriggerUnit::trigger` turns a
        /// used map portal off; off draws nothing and is not offered to the pad.
        bool enabled = true;
        std::vector<Ogre::SceneNode*> holders;  ///< its `LAYOUTFILE`'s particles
        /// Its layout's `Sound` nodes that start on activation and loop, held
        /// as voices under `voice` and re-mixed every frame.
        struct HeldSound {
            std::string voice;
            std::string definition;
            ogre::Vector3 at = ogre::Vector3::ZERO;
            float volume = 1.0f;
            float radius = 0.0f;
        };
        std::vector<HeldSound> sounds;
    };

    std::vector<OpenPortal> open_portals;

    /// A `CFormationNode` waiting for the player: `CFormationNode::setLayoutFile`
    /// loads its monsters out of the level, `updateLevelObject`
    /// brings them in within `kFormationWakeDistance`.
    struct PendingFormation {
        std::string path;
        ogre::Vector3 at;
        int monster_level = 0;
        bool woken = false;
        /// Its layout holds `Unit Spawner`s: they spawn as it wakes.
        bool spawners = false;
        /// The formations file's name for the layout (`CFormationNodeSaveAndLoad +0x18`).
        std::string reference;
        /// Each `Monster` node's unit as `setLayoutFile` made it, in node order.
        std::vector<tl::core::LevelScript::UnitCreation> made;
    };

    std::vector<PendingFormation> pending_formations;
    /// Merchants the population made and stocked, by its handle, until they are placed.
    std::map<int64_t, MerchantState> made_merchants;

    /// `CLevel +0x28`, `populate`'s room records: one per placed room layout, the union of
    /// its sections (`getRoomIndexThatPositionIsIn`).
    std::vector<tl::core::LevelRoomBox> level_rooms;

    struct PopulatedTrigger {
        std::string unit;
        ogre::Quaternion facing = ogre::Quaternion::IDENTITY;
    };

    /// The trigger props `place_populated_trigger` made, by node (stored as created).
    std::map<int64_t, PopulatedTrigger> populated_triggers;

    /// A unit a spawner released: which spawner, for its `unitStateChange`.
    struct SpawnerUnit {
        tl::core::LevelScript* machine = nullptr;
        int64_t spawner = 0;
    };

    std::map<int64_t, SpawnerUnit> spawner_units;

    /// The level template's `ACTIVERANGEMOD` and unit active range, which every
    /// machine's characters use (see apply_environment()).
    float level_active_range_mod = 0.0f;
    /// Where place_mesh() registers what it places while a scripted nested layout is
    /// placed; null: the level's own map.
    NodeMap* node_sink = nullptr;
    std::vector<std::pair<Ogre::Entity*, std::string>> pending_trigger_layouts;
    /// Placed trigger props whose unit authors `LAYOUT_STATE_1`, waiting: a layout cannot
    /// be placed inside `place_layout`, so the script's step hangs them.
    std::vector<std::pair<Ogre::Entity*, std::string>> trigger_layout_props;
    /// `--rng-trace`: where every seeding and draw of `g_Rand` (`core::game_random()`) goes.
    std::FILE* rng_trace_file = nullptr;
    /// The scene nodes a wire or a timeline can address; see place_layout().
    std::set<const Ogre::SceneNode*> scripted_scene_nodes;
    /// `CLevel +0x14c` and `+0x158`: the last `Property Node` typed `Town
    /// Portal` (7) and `Waypoint Start` (15) the layout walk met.
    ogre::Vector3 town_portal_spot = ogre::Vector3::ZERO;
    /// Entry type 6's start (`loadRoomLayout`, cases 14 and 15):
    /// the last `Waypoint` (14) or `Waypoint Start` (15) `Property Node`.
    ogre::Vector3 waypoint_entry_spot = ogre::Vector3::ZERO;
    ogre::Vector3 waypoint_spot{-1.0f, -1.0f, -1.0f};
    /// Layout files being loaded, by path, so a self-reference cannot recurse forever.
    std::set<std::string> layouts_in_progress;
    /// The `Layout Link` node whose file is currently being placed, and its
    /// name. Saved and restored around the recursive call, like `particle_owner`.
    int64_t nested_link = 0;
    /// What a machine made now takes as its `NestedScript::turn_link`.
    int64_t turn_link = 0;
    /// Where the last frame's sound listener stood (`CSoundManager +0x14`..`+0x1C`): a level
    /// loaded since hears its activation's sounds from there.
    tl::core::Vec3 sound_listener;
    /// `CResourceManager+0x42` (X360 `+0x26`): set by `CLevel::loadRoomLayout` around each
    /// room piece, making `CLayout::loadLayoutFile` reseed; clear at first.
    ///
    bool layout_seeding = false;
    uint32_t layout_child_count = 0;
    int64_t placing_hierarchy = 0;
    /// `loadLayoutFile`'s static `mRecursiveFix` count (layouts being loaded)
    /// and `iChildCount`, reset whenever an outermost load starts.
    uint32_t layout_stack_depth = 0;
    size_t collision_skipped = 0;
    /// What kind of file is being placed (ParticleSystem::wrapper): under a layout link,
    /// and a particle file. Saved and restored around the recursion.
    bool placing_under_link = false;
    /// The room layout being placed: its index (`CLevel+0x10`) and the hash of the
    /// `Layout Link` chain above. INVENTED, the hash:
    /// `CEditorBaseObject::calculateParentHierarchyHashCode`
    /// ORs only links with +0x30 set; +0x30 is unread, so all are ORed.
    uint32_t placing_scene = 0;
    bool placing_particle_file = false;
    /// The archive path of the layout whose `Particle` nodes are being placed,
    /// for `RendererConfig::pu_particle_layouts` (`--pu-particles`).
    std::string placing_layout_path;
    /// `Renderer::set_passability_dump()`; empty writes nothing (Renderer::dungeon_transition).
    std::string passability_dump_path;
    /// The `Layout Link Particle` whose subtree is being placed, or 0
    /// (ParticleSystem::owner).
    int64_t particle_owner = 0;
    /// `CLevel::updateNPCIcons` is owed once the player is in control.
    bool npc_icons_dirty = true;
    std::string nested_link_name;
    /// Set by a layout link just before it places its file, taken by
    /// place_layout() as it starts.
    bool link_places_next_layout = false;
    int32_t layout_level_seed = 0;  ///< `CResourceManager::getCurrentLevelSeed()`
    tl::core::QuestState initial_quests;
    bool has_waypoint_entry_spot = false;
    bool has_town_portal_spot = false;
    std::vector<std::pair<size_t, std::string>> deferred_room_machines;
    /// Set by whatever places a file as a `CParticle`, taken by place_layout(): loaded as
    /// `CParticlePreloader::LoadParticle` does, only its
    /// `Particle`, `Sound` and `Camera Shake` objects; no script runs.
    bool cparticle_places_next_layout = false;
    size_t collision_meshes = 0;
    /// While a generated floor's room pieces are placed, their machines wait unactivated:
    /// `CLevel::populate` activates each room scene
    /// (`CEditorScene::setActivated`, vtable +0x218) only after population and
    /// `CQuestManager::populate`.
    bool defer_room_activation = false;
    /// The scale `populate` drew for a trigger prop, by its coming node; `place_layout`
    /// draws any other.
    std::map<int64_t, float> made_trigger_scale;
    /// What `find_chunk_marker` reads besides the marker's place: its facing, and the warper beside it.
    struct ChunkMarkerExtra {
        ogre::Vector3 forward = ogre::Vector3::UNIT_Z;
        bool has_warper = false;
        ogre::Vector3 warper_position = ogre::Vector3::ZERO;
        ogre::Vector3 warper_forward = ogre::Vector3::UNIT_Z;
    };
    ogre::Vector3 dungeon_exit_point = ogre::Vector3::ZERO;
    /// The floor's `ENTRANCE_NODE`: its `Stairs Up` Unit Trigger activates a
    /// Warper with `LEVEL DELTA -1` and no dungeon name.
    ogre::Vector3 dungeon_entrance_point = ogre::Vector3::ZERO;
    /// The `Warper`s of the floor's `ENTRANCE_NODE` and `EXIT_NODE` layouts:
    /// where each stands and its `getForwardAbsolute()`.
    ogre::Vector3 dungeon_entrance_warp_at = ogre::Vector3::ZERO;
    ogre::Vector3 dungeon_entrance_warp_forward = ogre::Vector3::UNIT_Z;
    ogre::Vector3 dungeon_exit_warp_at = ogre::Vector3::ZERO;
    ogre::Vector3 dungeon_exit_warp_forward = ogre::Vector3::UNIT_Z;
    float dungeon_entrance_radius = 3.0f;
    /// One box per placed room layout, grown by its pieces' collision bounds,
    /// and which of them the room piece being placed belongs to (-1: none).
    std::vector<tl::core::PassabilityMap::Box> collision_sections;
    /// `CLevel +0xB0`'s piece boxes: the `recalculate` pieces', in the order made.
    std::vector<tl::core::PassabilityMap::Box> passability_recalculate;
    /// `CLevel::bakeCollision` on a level with pathing where a room has no
    /// `.pth`: the `recalculate` pieces are kept out of the collision until the rooms' passes
    /// are done, then added, their boxes cast again after the rooms' edge strips.
    bool hold_random_pieces = false;
    struct HeldCollision {
        std::string path;
        ogre::Matrix4 to_world;
    };
    std::vector<HeldCollision> held_random_collision;
    std::vector<tl::core::PassabilityMap::Box> held_random_boxes;
    /// The rooms' piece collision, added once every room is placed in the order the pieces were
    /// made (`MeshPlacement::made`), each with its room (`collision_section`): `bakeCollision`
    /// walks each room's `Room Piece` objects so.
    struct RoomPieceCollision {
        int section = -1;
        MeshPlacement placement;
    };
    std::vector<RoomPieceCollision> room_piece_collision;
    uint64_t room_pieces_made = 0;
    /// `ELevelEntryType`, which `CLevel::loadRoomLayout`
    /// picks the start by: 3 from `setGameState`, else `core::warp_entry_type`.
    int level_entry_type = 3;
    /// The level template's `CINEMATIC` (`CLevelTemplateData +0x698`), the
    /// stratum's laid over it; empty for a level loaded as a plain layout.
    std::string level_cinematic;
    float dungeon_exit_radius = 4.0f;
    std::string dungeon_active_name;
    int dungeon_active_floors_total = 1;
    int dungeon_active_floor = 0;
    /// The floor `place_dungeon_floor()` last built and where its exit is, polled by `run()`
    /// every frame; cleared by `load_layout()` (`Renderer::DungeonTransition`).
    bool dungeon_active = false;
    int collision_section = -1;
    /// The level of the player a warp brings in, 0 for none: `CGameClient::loadLevel`
    /// reads `CPlayer +0x100`, but the sheet arrives later here.
    int32_t arriving_player_level = 0;
    /// One unit phase of `CLevel::populate`: monsters..
    /// props, sections picked on `g_Rand`, each a
    /// `populateSectionOfLevel`. `owner`: the characters' list; `label`: the log's word.
    struct PopulatePhase {
        int64_t owner = 0;
        const char* label = "";
        bool champion = false;      ///< `populateSectionOfLevel`'s fourth argument
        bool markers = false;       ///< its fifth: the NPCs stand on `CLevel +0xf0`
        bool empty_counts = false;  ///< an empty roll counts as one: all but the monsters
        bool props = false;         ///< the props phase, for the log
    };
    /// `CLevel::populate`'s sections: one box
    /// per collision-enabled room piece, in the order the pieces were placed.
    std::vector<tl::core::PopulateSection> population_sections;
    /// The floor's chunks where its layout put them, in the layout's order (`CLevel +0x16c`'s
    /// instances, `CChunkInstance +0x28`).
    std::vector<ogre::Vector3> level_chunk_origins;
};

}  // namespace tl::core
