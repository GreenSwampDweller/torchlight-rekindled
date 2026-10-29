// OGRE-facing rendering. Everything that knows OGRE lives behind this interface: the engine core
// talks to a Renderer and never includes an OGRE header. Desktop and Switch differ only in the
// render system installed and where the media lives.
#pragma once

#include "core/asset_path.h"
#include "core/character.h"
#include "core/continue_menu.h"
#include "core/difficulty_menu.h"
#include "core/game_config.h"
#include "core/inventory.h"
#include "core/level_memory.h"
#include "core/mods.h"
#include "core/pet.h"
#include "core/quest_book.h"
#include "core/retirement.h"
#include "core/settings_menu.h"
#include "core/stash.h"
#include "core/units.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tl::core {
struct InputState;
}  // namespace tl::core

namespace tl::render {

/// What the player asked the main menu for. Declared here rather than in render/menu.h so that
/// main.cpp can drive the menu without an OGRE header.
enum class MenuAction {
    None,
    NewGame,
    Quit,

    /// The main menu's own "Wardrobe" button: the debug wardrobe editor. Handled inside
    /// `run_main_menu()`, never returned to a caller, as `Back` is.
    Wardrobe,

    /// The main menu's own "Bestiary" button: the debug bestiary, every unit
    /// under `media/units/monsters/` standing in turn where the menu's pet
    /// stands. Handled inside `run_main_menu()`, like `Wardrobe`.
    Bestiary,

    /// Left or right on one of a debug editor's rows (the wardrobe's or the
    /// bestiary's): `MainMenu::editor_row()` and `editor_step()` say which row
    /// and by how much. Handled inside `run_main_menu()`.
    EditorStep,

    /// A list's own way out, back to the main buttons.
    Back,

    /// A floor was picked off the Choose Level list. `Renderer::dungeon_choice()` /
    /// `dungeon_choice_floor()` name which; the caller restarts the renderer and loads it as
    /// `NewGame` loads the town, skipping character creation.
    LoadDungeon,

    /// The main menu's own "Choose Level" button: swap its two buttons for a
    /// list of `media/dungeons/MAIN.DAT.adm`'s biomes - the real game's 35
    /// floors, grouped. Handled entirely inside `run_main_menu()`.
    ChooseLevel,

    /// A biome was picked off the Choose Level list. `Renderer::menu_biome_choice()` names which;
    /// the caller swaps in that biome's floor list, whose `Back` returns to the biome list.
    ChooseLevelBiome,

    /// The main menu's `ContinueLast`: `CMainMenu::onClick`
    /// function 3, `requestSetGameState(2)` -
    /// the newest save of a living character.
    Continue,

    /// The main menu's `Options` (`i_415`, `onClick` `GUISETTINGSMENU`):
    /// `CMainMenu::onClick` function 0x5e,
    /// `CGameUI::toggleSettings` - the Options screen over the
    /// menu. Handled inside `run_main_menu()`.
    Options,

    /// The main menu's `Help & Controls` (`Help`, `GUIHELPANDOPTIONSMENU`):
    /// `CMainMenu::onClick` function 0x62, `CGameUI`'s
    /// Help toggle. Handled inside `run_main_menu()`.
    Help,

    /// The main menu's `LoadCharacter`: the list of saved characters
    /// (`CContinueGameMenu`, `Renderer::run_load_character`). From that
    /// screen it means "this character behind the menu, and the list again":
    /// the game client's `applyCharacterState` was asked for.
    LoadCharacter,
};

/// What the Load Character screen needs of the save directory.
struct LoadCharacterHost {
    /// `CContinueGameMenu::reloadFiles`: every save, newest first.
    std::function<std::vector<tl::core::ContinueEntry>()> list;
    /// `deleteCharacter`'s `remove` of the file under the save path.
    std::function<void(const std::string& path)> remove;
};

/// The game's start-up configuration (core/game_config.h); the renderer's name for it.
using RendererConfig = tl::core::GameConfig;

/// Everything about the player that the original keeps across a level change
/// because it is the same `CPlayer` object on both sides of
/// `CGameClient::performWarp`: the sheet, the
/// bag, the quests, the health and the pet. This engine rebuilds its renderer
/// instead, so the player leaves in one of these and comes back out of it.
struct CarriedPlayer {
    bool valid = false;
    tl::core::CharacterProgression character;
    tl::core::Inventory inventory;
    /// The console's stash, `CPlayer +0x9f4` (`core/stash.h`).
    tl::core::Inventory stash = tl::core::make_player_stash();
    tl::core::QuestLog::Carried quests;
    float health = 0.0f;
    float max_health = 0.0f;
    bool dead = false;
    /// The pet as it left: its unit, stance, bag, level and what a fish's
    /// transformation has left (`+0x688`, `+0x690`).
    tl::core::Pet pet;
    /// A saved pet's unit, for a file's pet that `pet` is not yet alive as.
    std::string saved_pet_unit;
    /// The player's other pets (`CPlayer +0x648`): a character made one by
    /// `CCharacter::addPet`, which a warp takes along (`CGameClient::performWarp`
    /// removes them with the player and `loadLevel` adds them back beside them).
    struct CarriedCompanion {
        std::string unit;
        float hp = 0.0f;
        int level = 0;
        int32_t experience = 0;  ///< `+0x448`
    };
    std::vector<CarriedCompanion> companions;
    /// The pet's spells by name and level (its `+0x3c8`/`+1000`, saved at its
    /// state's `+0x118`/`+0x138`), learned again on arrival: the book `pet`
    /// carries points into the skill files of the renderer being left.
    std::array<std::string, 4> pet_spells;
    std::array<int32_t, 4> pet_spell_levels{};
    /// A saved pet's bag, from a save file (a warp carries `pet` whole).
    bool has_pet_bag = false;
    tl::core::Inventory pet_bag;

    /// The skill book (`tl::core::SkillBook`) as plain values. The book itself
    /// points into skill files the renderer being left owns; the one arrived in
    /// rebuilds it from the class and puts these back. `skill_levels` is in the
    /// class file's order, as the save file's `+0x1e8` is.
    bool has_skills = false;
    std::vector<int32_t> skill_levels;
    std::vector<int64_t> skill_slots;
    std::vector<int64_t> second_skill_slots;
    /// The other two lists, as the file's pairs (`SkillBook::mapped_pairs`).
    std::vector<std::pair<int64_t, int64_t>> mapped_function_skills;
    int skill_set = 0;
    std::array<std::string, 4> spells;
    std::array<int32_t, 4> spell_levels{};

    /// The debug menu's GODSPEED (`CPlayer +0x790` and the `PLAYER_SPEED_UP`
    /// affix). The original keeps both on its one `CPlayer` across
    /// `CGameClient::performWarp`; the renderer that held them is thrown away,
    /// so the switch crosses the rebuild here. Not saved: a test switch.
    bool godspeed = false;
};

class Renderer {
public:
    Renderer();
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// Bring up OGRE and mount the game's data. False on any failure, with the
    /// reason already logged.
    bool init(const RendererConfig& requested);

    /// Load `mesh_name` and place it in the scene with the camera framed on it. Matched
    /// case-insensitively by OGRE (OGRE_RESOURCEMANAGER_STRICT=0), which also covers the mesh's
    /// own reference to its skeleton ("alchemist.skeleton" for "Alchemist.SKELETON").
    bool show_mesh(const std::string& mesh_name);

    /// Link an animation-only skeleton beside the subject's mesh onto the subject's skeleton:
    /// Torchlight keeps one .skeleton per animation (Idle.SKELETON holds only the idle tracks),
    /// and OGRE links by bone name. `name` is the file stem ("Idle"), resolved beside the
    /// subject's own mesh because a bare basename is ambiguous.
    bool link_animation(const std::string& name);

    /// Put a wardrobe mesh on the subject: a second mesh drawn over the body, rigged to the same
    /// Biped skeleton. The piece gets its own skeleton instance, since the body and the set name
    /// different skeleton resources (`alchemist.skeleton`, `plate_set.skeleton`) with the same
    /// bones.
    ///
    /// `mesh_name` may be a full archive path, and should be when the basename is ambiguous.
    /// `only_slot`, when given, draws just that slot's submesh: wardrobe meshes are whole bodies
    /// even for one piece (`alchemist_hair.MESH` carries boots, chest, gloves and a face with
    /// empty texture units).
    bool equip(const std::string& mesh_name, const std::string& only_slot = {});

    /// Dress the subject as the game dresses a character wearing nothing: the player unit's
    /// `.DAT` textures per slot (`CHEST_BASE`, `BOOTS_BASE`, `GLOVES_BASE`) retexture the base
    /// body's own submeshes; `HELMET_BASE_MESH`, the hair, is the one real mesh.
    bool wear_base_outfit();

    /// Enable one of the animations reachable from the loaded skeleton, parked
    /// at `time`. Applies to the subject and everything equipped on it, so they
    /// stay in step. Animations from a linked skeleton count, so link first.
    bool apply_animation(const std::string& name, float time);

    /// Put the player somewhere in the story before a level is loaded. `specification` is `NAME`,
    /// `NAME:complete` or `NAME:active` (bare: complete); false for another suffix. Before
    /// load_layout(): a level's quest gates are evaluated once, while it is placed.
    bool set_quest(const std::string& specification);

    /// A new character's own quests: `CPlayer::setQuestManager`
    /// gives (`giveQuest(quest, nullptr, false)`, no banner) and accepts every quest the class's
    /// unit file lists under `[QUESTS]`. Before load_layout(), like set_quest(). A save's log
    /// replaces it.
    void give_starting_quests();

    /// Put the level's script back the way it loaded, after a scripted verification scene and
    /// before anyone is given the controller.
    void reset_script();

    /// Load a `.cmp` layout and place its contents in the scene. `reference` is an archive path
    /// as the layout data spells it (uppercase, e.g.
    /// `MEDIA/LAYOUTS/MAINMENUS/.../MAINMENU_TOWN.LAYOUT`), resolved case-insensitively.
    bool load_layout(const std::string& reference);

    /// The main menu's backdrop for a saved character last seen on floor
    /// `depth` of `dungeon` - `CGameClient::loadMenuLevel`:
    /// that floor's template's
    /// `MAINMENURULES` (the mines' say `mainmenu_minerules.dat`), or the town's
    /// rules when there is no template, laid out as `CLevel::loadRoomLayout`
    /// would. The layout path to hand to `load_layout`; empty when nothing
    /// could be resolved.
    std::string menu_layout_for(const std::string& dungeon, int32_t depth);

    /// Build and place one procedurally generated dungeon floor. `name` is a dungeon definition's
    /// own `NAME` (`media/dungeons/<NAME>.DAT.adm`: `Test_Cave`, `Test_Mine`, ...), matched
    /// case-insensitively; `floor_number` is the floor generated. Only the first `[STRATA]` block
    /// is used for every floor so far.
    ///
    /// Seeded as the original seeds it: the level seed is the character's game seed, the
    /// dungeon's salt and the depth (`CGameClient::resetGameSeed`); its population comes off the
    /// clock. See `core/dungeon.h` and `Renderer::Impl::place_dungeon_floor()`.
    bool load_dungeon(const std::string& name, int floor_number = 1);
    /// The entry type the next level is loaded with (`performWarp`'s; see
    /// `core::warp_entry_type`). 3, `setGameState`'s, until a warp says.
    void set_level_entry_type(int entry_type);

    /// `CGameClient::loadCharacter`'s seeds: the save's game seed and depth,
    /// its dungeon made current, and every tracked dungeon's salt put back.
    void load_character_seeds(const tl::formats::CharacterFile& file);
    /// A new game's game seed (`CGameClient::setGameState`, states 3 and 4).
    void begin_new_game();

    /// Append every seeding and draw of a dungeon floor's generator (`g_Rand` in the original) to
    /// `path`, for comparison with a trace of the original. Empty stops it.
    void set_rng_trace(const std::string& path);

    /// Write every wire the level scripts deliver to `path`, one line each
    /// (`<seconds> W <descriptor>:<name> .<output> -> <descriptor>:<name> .<input>`), with a
    /// `# level load <n>` line before each level. Empty stops it.
    void set_logic_trace(const std::string& path);

    /// Write every passability grid the next levels bake to `path` (each bake replaces the file),
    /// for comparison with a dump of the original.
    void set_passability_dump(const std::string& path);

    /// One floor of `media/dungeons/MAIN.DAT.adm` - the real game's 35-strata progression - for
    /// the front end's own "Choose Level" screen (the port's own; the original has none).
    /// `biome` and `label` come from each stratum's `RULESET` path; `within_biome` restarts at 1
    /// whenever the biome changes ("floor 5" is "Crypt 1").
    struct MainFloor {
        int floor = 0;            ///< 1-based, global - what `load_dungeon()` takes
        std::string biome;        ///< ruleset directory segment, lowercased: "mine", "crypt", ...
        std::string biome_label;  ///< display name: "Mine", "Sunken Temple", ...
        int within_biome = 0;     ///< 1-based count within this biome's run of strata
        std::string label;        ///< "Mine 1", "Palace 6 (Ordrak)", ...
    };
    std::vector<MainFloor> list_main_floors();

    /// What reaching a generated floor's exit asks for, checked once a frame by `run()` against
    /// the player's distance from the exit chunk's ground. `pending` is false on every other
    /// frame. No room piece authors a hook for this: "reaching the exit ends the floor" is the
    /// port's own reading of `REQUIRESEXIT:True` and an `EXIT_CHUNK` room shape.
    struct DungeonTransition {
        bool pending = false;
        /// True past the dungeon's own `FLOORS` count: back to town. False for a next-floor step.
        bool return_to_town = false;
        std::string dungeon;  ///< empty when `return_to_town`
        int floor = 0;
        /// The warp's level delta and absolute depth (`CGameClient +0x10a0`,
        /// `+0x10a4`), for the entry type.
        int level_delta = 0;
        int level_absolute = 0;
        /// `warpLevels("Town", -99)`: a town portal used, or the death screen's
        /// resurrection in town (`CGameClient::setGameState` state 5). No
        /// last-dungeon record and no Warper placement: entry type 2, which in a
        /// town is its `Town Portal` node.
        bool portal_warp = false;
        /// The -99 warp taken from a town (`performWarp`'s `bVar8`): back to the
        /// level the portal stands on, beside it, and the portal closes.
        bool back_to_portal = false;
        /// `warpLevels`' `WAYPOINT` (`+0x10ba`): a waypoint menu row, or a
        /// Warper that authors it. Into another dungeon it is entry type 6.
        bool waypoint = false;
    };
    DungeonTransition dungeon_transition() const;

    // The level left behind and the player carried across (`CLevelState`).

    /// The loaded level as the original keys it: `CLevel+0x280` (the dungeon's
    /// upper-cased `NAME`, `TOWN` for the town) and `+0x1a4` (the depth, the
    /// first floor 0). Empty for a layout that is no dungeon's level.
    const std::string& level_dungeon() const;
    int level_depth() const;

    /// `CPlayer::storeLevelSavedState` on the
    /// level being played, into `RendererConfig::travel`. What `performWarp`
    /// does before it unloads.
    void store_level_state();

    /// `CGameClient::performWarp` leaving
    /// this level for `destination` at `depth`: the level is remembered, and
    /// when the destination is another dungeon reached some other way than the
    /// -99 return to town, the level left becomes the last dungeon
    /// (`CPlayer+2000`/`+0x7cc`, unless its template is `DONTSTORE`) and the
    /// destination is tracked (`updateDungeonTracking`).
    void leave_level(const std::string& destination, int depth, bool return_to_town);

    /// The town portal's half of a level entered (`core/town_portal.h`):
    /// `CLevel::deleteOpenPortals`, then
    /// `CPlayer::createPortals`. What
    /// `performWarp` and `setGameState` do once the player stands on the new
    /// level.
    void create_portals();

    /// `loadRoomLayout`'s entry type 2 (case
    /// 7): a -99 warp into a town stands the player at its `Town Portal` node.
    /// False when the level has none, and the player stays at its start.
    bool place_player_at_town_portal();

    /// `performWarp`'s arrival from a town through the portal: the
    /// open portals go, the player stands at a random open point within 1 of
    /// where the portal was, and the portal is closed.
    void arrive_back_at_portal();

    /// `--portal map:ITEM`: the bag's press on the first copy of `item_name`
    /// the player carries (`use_player_item`, as the tray's A). True when it
    /// was used.
    bool use_bag_item(const std::string& item_name);

    /// `performWarp`'s arrival of entry type 5 out of `left_dungeon`:
    /// the player stands beside the level's map portal to that
    /// dungeon, when it holds one, and every such portal goes, with the
    /// player's history of the dungeon.
    void arrive_from_temporary_portal(const std::string& left_dungeon);

    /// `--portal enter`: stand at the level's first open portal and press A on
    /// it. True when that left a warp pending.
    bool walk_into_portal();
    /// `--portal up|down`: stand at a generated floor's entrance (its `Stairs
    /// Up`) or exit marker and press A. True when that left a warp pending.
    bool walk_into_stairs(bool up);

    /// `loadRoomLayout`'s entry type 6 (cases
    /// 14 and 15): a waypoint warp into another dungeon starts the player at
    /// the last `Waypoint` or `Waypoint Start` node. False when the level has
    /// none, and the player stays at its start.
    bool place_player_at_waypoint();

    /// `--portal waygate`: what the level's `Waypoint Activator` does when its
    /// `Activate` input arrives, without the boss fight that opens the waygate.
    void activate_waypoint();
    /// `--portal waypoints`: stand at the level's waypoint portal and press A
    /// on it. True when the menu opened.
    bool walk_into_waypoint_portal();
    /// `--portal choose:N`: the stick down N rows on the open menu, then A.
    /// True when that left a warp pending.
    bool choose_waypoint(int row);
    bool waypoint_menu_open() const;
    /// The Pause Menu (`COptionsMenu`, Start) and Options (`CSettingsMenuXenon`).
    bool pause_menu_open() const;
    bool options_menu_open() const;

    /// The player, to take across `restart()`.
    CarriedPlayer carry_player();

    /// `CGameClient::saveCharacter(force, true)`
    /// into `path`: the level being played is
    /// remembered first (`CPlayer::storeLevelSavedState`), then the whole
    /// character is written to a temporary file and rotated in. False when
    /// there is no player, a dead one and no `force`, or the file cannot be
    /// written.
    bool save_character(const std::string& path, bool force = false);

    /// `CSharedStash::loadSharedStash` from
    /// `directory`, into `RendererConfig::shared_stash`: nothing while it holds
    /// anything or has no file there. The caller decides when (`loadCharacter`,
    /// and `setPlayer` for a player of the other kind).
    void load_shared_stash(const std::string& directory);

    /// The death screen (`CDieMenu`), for runs with nobody at the pad: whether it
    /// is up, a choice (0 here, 1 the level's start, 2 town) as `onClick` takes
    /// it, and its frame. `choose_resurrection` returns false when it is not up.
    bool death_screen_open() const;
    bool choose_resurrection(int choice);
    bool dump_death_screen(const std::string& png_path);
    /// Take all of the player's life, as a blow would (`--die`).
    void kill_player(const std::string& from);

    /// The player out of a save file: `CGameClient::loadCharacter`
    /// and `CCharacter::applySaveState`
    /// in this engine's terms. Call once the
    /// class's subject is up; hand the result to `restore_player_before_level`
    /// and `restore_player_after_control` as a warp would.
    CarriedPlayer carried_from_file(const tl::formats::CharacterFile& file);

    /// Put the carried quests and pet back. **Before the next layout loads**: a
    /// level's `Group` gates read the quest state while it is placed, and the pet
    /// is placed by `enable_player_control()`.
    void restore_player_before_level(const CarriedPlayer& carried);

    /// Put the carried sheet, bag and health back. **After
    /// `enable_player_control()`**, which builds a fresh sheet for the class.
    void restore_player_after_control(const CarriedPlayer& carried);

    /// `CLevel::placePlayerAtWarpToDungeonFloor`:
    /// stand the player at the `Warper` that
    /// leads back where they came from. False when none matches, and the player
    /// stays at the level's start.
    bool place_player_at_warp(const std::string& previous_dungeon, int previous_depth,
                              const std::string& warp_name);

    /// Hand the subject over to the player, and the camera over to the subject. Call after the
    /// layout is loaded and the character stands where it belongs (speeds, body size and camera
    /// framing come from what is loaded). Control is the console's: the left stick puts a
    /// destination two units ahead in the camera's frame, the target selector
    /// picks what the buttons act on, and the right stick zooms. Never on for a frame dump.
    bool enable_player_control();

    /// Stand the character somewhere else, on the ground under (x, z): a survey tool for frame
    /// dumps. Call after enable_player_control(); the camera follows. False when there is no
    /// ground in that column.
    bool place_player_at(float x, float z, const float* feet_if_no_ground = nullptr);

    /// Log every collision surface a vertical ray at (x, z) crosses between
    /// +100 and -100, with its material and owning group. `--ground-probe`.
    void probe_ground(float x, float z);

    /// Drive the player through a fixed square (forward, right, back, left) at 60 Hz from the same
    /// start, so both targets' logs and dumped frames are comparable. Needs
    /// enable_player_control() first; run before the frame is dumped.
    bool simulate_walk(float seconds);

    /// Walk to a named unit and open what they have to say, as pressing A on them would. False
    /// when the level places no such unit or they had nothing to say.
    bool talk_to_unit(const std::string& name, float timeout);
    /// Presses on an open vendor screen, comma-separated: `x` (buy or sell;
    /// on a Transmuter's, swap), `y` (on a Transmuter's, transmute), `rb`/`lb`
    /// (the focus), `left`/`right` (the tab), `up`/`down` (the selection), and
    /// `gold=N` (set the player's gold, for a test).
    void vendor_keys(const std::string& keys);
    /// Presses on the player's own inventory screen, which it opens, comma-
    /// separated: `bag=<item>` (a copy made by `createItem` into the bag, not
    /// worn), `magic=<item>` (the same with the magic roll forced, so it comes
    /// unidentified), `sel=<item>` (the selection onto the first copy of that
    /// name, worn or not), `a` (the tray's A: use, identify, equip, unequip),
    /// `fill` (the backpack filled with Health Potions one to a slot) and
    /// `shot=PNG`.
    void inventory_keys(const std::string& keys);
    /// Steps through the fishing `--talk-to "Fishing Hole"` started, comma-separated, each at a
    /// fixed 60 Hz: `wait=S` (S seconds), `bite` (until a bite starts, at most 120 s), `a` (catch),
    /// `b` (stop), `land` (until the catch has landed), `ok` (close the modal) and `shot=PNG`.
    void fishing_keys(const std::string& keys);

    /// Path the character to (x, z) and walk it there at a fixed 60 Hz, through the same movement
    /// code the stick drives. Gives up after `timeout` seconds; false when no route exists.
    bool walk_route_to(float x, float z, float timeout);

    /// Fire every enabled `Skip Cutscene` node, as the button does, so a headless run can verify
    /// the skip path's cleanup.
    void skip_cutscene();

    /// Run the whole scene forward by `seconds` at a fixed 60 Hz timestep, rendering each step:
    /// OGRE's material and texture controllers advance only on rendered frames. The player is not
    /// driven (no input, no script), so this shows the scene animating on its own; two dumps at
    /// different times stay comparable between targets.
    void advance_scene(float seconds);

    /// `--pu-smoke`: a ParticleUniverse system built in code at the player,
    /// to show the library running on this target. advance_scene() logs how
    /// many of its particles are alive. False when ParticleUniverse is not up.
    bool spawn_pu_smoke();

    /// Drop the first stack of `item`, worn or carried, the way Y on the
    /// inventory screen does. For `--drop-dump`.
    bool drop_carried(const std::string& item);

    /// Swing once, if the character is not already swinging.
    bool attack();

    /// Keep swinging for as long as the scene runs (for `--visit` and `--dump-at`).
    void set_auto_attack(bool on);

    /// Run the console's target selector while a headless run advances the scene (`--pad`;
    /// `core/targeting.h`), logging what it picked.
    void set_headless_targeting(bool on);

    /// Light up a placed unit by name, as the console's selector would (`--highlight`): a
    /// lower-cased substring. False when nothing is called that. For a static frame's baseline.
    bool highlight_unit(const std::string& name);

    /// Whether the main menu offers Continue: `CMainMenu::update`
    /// shows `ContinueLast` only while `CContinueGameMenu::canContinue` (a save whose
    /// character has hit points left) holds. Read by the next `run_main_menu`.
    void set_continue_available(bool available);

    /// Put the game's own main menu over the loaded scene (`MAINMENU_TOWN.LAYOUT`; see
    /// render/menu.h) and run it until the player chooses something. `dump_png`, when given, gets
    /// one frame of the untouched menu. Returns `Quit` when the window closes or the applet shuts
    /// down, and at once when headless (after the dump). `auto_new_game` presses New Game as soon
    /// as the frame is taken.
    MenuAction run_main_menu(const std::string& dump_png = {}, bool auto_new_game = false);
    /// `--menu-dump-at`: how long the title's backdrop runs (60 Hz steps) before
    /// run_main_menu() takes its dump; 0 takes it on the first frame.
    float menu_dump_delay = 0.0f;

    /// Which dungeon the Choose Level list's own selection loop returned
    /// `MenuAction::LoadDungeon` for. Empty when nothing was chosen.
    const std::string& dungeon_choice() const;

    /// The floor that goes with `dungeon_choice()`: the picked global floor
    /// number.
    int dungeon_choice_floor() const;

    /// Which biome key (`list_main_floors()`'s `MainFloor::biome`) the
    /// Choose Level list's own selection loop returned
    /// `MenuAction::ChooseLevelBiome` for. Empty when nothing was chosen.
    const std::string& menu_biome_choice() const;

    /// Build the main menu, switch to the Choose Level list's biome tier as if Choose Level had
    /// been pressed, and write one frame of it. False when the menu could not be built.
    bool dump_level_list(const std::string& png_path);

    /// The same trade for the debug wardrobe editor, as if Wardrobe had just
    /// been pressed. `picks` sets rows first: `<row>=<index>` pairs, comma
    /// separated, the row by its caption's word (`chest=3,weapon=12`), the
    /// index as the screen counts (0 is the class's own outfit or an empty
    /// hand). `class=<NAME>` dresses that class (default the Alchemist),
    /// `class=all` each in turn; `zoom=<x>` brings the camera in (1 = the
    /// backdrop's). The sweep: `all` (the five armour rows) or `<row>=all`
    /// wears every look of the row alone over the class's own, a frame each,
    /// `<stem>_<class>_<row>_<n>_<item>.png` beside `png_path`.
    bool dump_wardrobe(const std::string& png_path, const std::string& picks);

    /// The same trade for the debug bestiary, as if Bestiary had just been
    /// pressed. `picks` chooses first, comma separated: `unit=<n>` (the
    /// screen's count, from 1) or `unit=<NAME>` (a unit's `NAME`, without
    /// case), `animation=<n>` (0 is the unit's own idle), `zoom=<x>` (1 frames
    /// the whole unit) and `turn=<degrees>`.
    bool dump_bestiary(const std::string& png_path, const std::string& picks);

    /// Give the character what its own unit file says it carries: the weapon in its
    /// `[EQUIPMENT] RIGHTHAND`, and the particle systems its animation manifest attaches to its
    /// bones. `enable_player_control()` does this for a level; the main menu needs it too.
    void dress_character();

    /// The three classes a new character can be, read out of
    /// `media/units/players/`. Empty only when the archive has none.
    const std::vector<tl::core::PlayerClass>& player_classes();

    /// Show a class: its body, its armour, its weapon, replacing the subject (for the creation
    /// screen). False when the class is not one of the three or its mesh will not load.
    bool show_player_class(const std::string& name);

    /// Which class the subject is, or empty. Set rather than inferred from the mesh (the
    /// Destroyer's is `warrior/player.MESH`); read by `load_player_stats()`.
    void set_player_class(const std::string& name);
    const std::string& player_class() const;

    /// The two names the last creation screen ended with.
    void creation_names(std::string& player_name, std::string& pet_name) const;

    /// Put the character creation screen up over the loaded backdrop and run it until the player
    /// picks a class (`NewGame`, `chosen` set) or goes back (`Quit`). `dump_png` photographs it
    /// untouched; `preselect` opens it on that class instead of the layout's default; `cycle`
    /// steps the selection through those classes, rendering between each, before taking OK.
    MenuAction run_character_create(const std::string& dump_png, std::string& chosen,
                                    std::string& pet, const std::string& preselect = {},
                                    const std::vector<std::string>& cycle = {});

    /// The difficulty screen New Game opens once the character is made
    /// (`CMenuManager::setActiveMenu`, menus 4 and 5), over the menu's
    /// backdrop. `state` is kept by the caller across showings (`core/difficulty_menu.h`).
    /// `MenuAction::NewGame` when a difficulty was picked (`click` says which, and Hardcore);
    /// `MenuAction::Back` for B; `MenuAction::Quit` when the window went. Headless: dumps
    /// `dump_png` and returns `NewGame` with `click.chosen` false.
    MenuAction run_difficulty_menu(tl::core::DifficultyMenu& state,
                                   tl::core::DifficultyClick& click,
                                   const std::string& dump_png = {});

    /// The mods page before the title (`run_start_mods_menu`): a scrolling column of a row per
    /// mod, switched by A, and under it the "Don't show this menu again" checkbox.
    /// `MenuAction::Back` for Start (or B), `MenuAction::Quit` when the window went; `list` left as
    /// the player switched it, `dont_show_again` as the checkbox was left. Headless, or with
    /// `dump_png`: dumps it and returns `Back`.
    MenuAction run_mods_menu(tl::core::ModList& list, bool& dont_show_again,
                             const std::string& dump_png = {});

    /// The frame-rate overlay on or off, as the mods page left its mod.
    void set_stats_overlay(bool on);

    /// The Load Character screen (`CContinueGameMenu`, `media/UI/characterload.uilayout`) over the
    /// menu's backdrop. `state` is the caller's (`ContinueMenu::open`).
    ///
    /// `MenuAction::Continue` when A started a living character (`state.current_file()`;
    /// `last.at_level_start` when both bumpers were held); `MenuAction::LoadCharacter` when X (or a
    /// delete) asked for the selected character behind the menu, which the caller loads before
    /// calling again; `MenuAction::Back` for B or when the last file was deleted;
    /// `MenuAction::Quit` when the window went. `shown_file` is the save behind the menu. `keys`
    /// presses buttons before the first frame (`u d a b x y`); headless, it dumps `dump_png`.
    MenuAction run_load_character(tl::core::ContinueMenu& state, const LoadCharacterHost& host,
                                  const std::string& shown_file, tl::core::ContinuePress& last,
                                  const std::string& dump_png = {}, const std::string& keys = {});

    /// Open a `[CINEMATIC]` by name (`CGameUI::setCinematicOpen`):
    /// its art, its crawling text, its recording,
    /// and Continue.
    ///
    /// When dump_png is given, dumps one frame with the crawl at rest and
    /// returns. A headless run without one only logs the name. Otherwise it
    /// returns once Continue is taken.
    MenuAction run_cinematic(const std::string& dump_png, const std::string& name);

    /// Hold the left stick at (`move_x`, `move_y`) for `seconds` at a fixed 60 Hz,
    /// through the code the pad drives, and log where the character ended up.
    /// For checking what a wall does to somebody walking into it.
    bool hold_stick(float move_x, float move_y, float seconds);

    /// Hold the attack button (X) for `seconds` at a fixed 60 Hz with the left
    /// stick at (`move_x`, `move_y`), through the pad's own code: the hold
    /// position flag, the aim and the swings in place. Logs where the
    /// character faced and stood before and after, and how many swings began.
    bool hold_attack(float move_x, float move_y, float seconds);

    /// Hold a hotbar button (`lb`, `rb`, `y`, `b`, `lt`, `rt`) for `seconds` at
    /// a fixed 60 Hz through the pad's dispatch: pressed on the first step, held
    /// after it, then let go. The nearest foe is faced first, as `--cast-at-foe`
    /// does. For a skill that runs while its button is down (Ember Lance).
    bool hold_hotbar(const std::string& button, float seconds);

    /// Log every placed unit's height and the ground under it (`--unit-heights`).
    void log_unit_heights();

    /// The player class's `INTRO_CINEMATIC` (`CCharacter::getIntroCinematic`),
    /// which a new character opens with. Empty
    /// when it authors none.
    std::string intro_cinematic() const;

    /// The cinematic the arrival on the level just loaded opens
    /// (`CGameClient::performWarp`), or an empty string: the level
    /// template's `CINEMATIC`, when the player has been no deeper in this
    /// dungeon and the level was not put back from a remembered state.
    std::string warp_cinematic() const;

    /// Stand the player's pet where a main-menu backdrop's `Point of Interest` marker says. False
    /// when the layout has none (every playable level). `unit` is `Dog` or `Cat`
    /// (`media/units/monsters/`); calling again replaces the pet standing there.
    bool place_menu_pet(const std::string& unit = "Dog");

    /// `CGameClient::applyCharacterState`:
    /// a save's character put on the menu's level in place of the one standing
    /// there - its sheet, bag, skills and name (`CResourceManager::createPlayer`
    /// of the file's class and the saved state, vtable `+0x148`), and the file's
    /// first pet, or none, on the pet's marker. The level is left alone. Call
    /// with the save's class already the subject (`createPlayer(class)`).
    void apply_character_state(const tl::formats::CharacterFile& file);

    /// Which animal walks with the character in a level, by unit name. Set before
    /// `enable_player_control()`, which puts it in the world; afterwards it replaces the animal
    /// where it stands.
    void set_pet(const std::string& unit);
    const std::string& pet_unit() const;

    /// Send it shopping with whatever is in its bag, and take the gold when it
    /// comes back. False when there is no pet, when it has already gone, or
    /// when this level is the town - which is where the original refuses too.
    bool send_pet_to_town();

    /// Step the stance the pet panel's three buttons show: Aggressive ->
    /// Defensive -> Passive.
    void cycle_pet_stance();

    /// ... or name one outright: `aggressive`, `defensive`, `passive`, matched
    /// without case. False for anything else, which is a typed argument rather
    /// than a state the engine could reach.
    bool set_pet_stance(const std::string& name);

    /// Whether there is an animal standing in the level right now. False while
    /// it is away in town, which is what the inventory's "Your pet has gone to
    /// town." panel is driven by.
    bool pet_in_world() const;

    /// Make a copy of an item for the player (`create_item`, a forced enchant
    /// when `enchant`) and wear it in its first free location, or put it in
    /// the bag. Logs the copy's roll and the player's figures before and after.
    /// What a verification run uses to check that worn items take effect.
    bool give_player_item(const std::string& item_name, bool enchant);

    /// The treasure `CCharacter::die` leaves (`drop_death_loot`), `count`
    /// times, as though `unit` had died at the player's feet to the player's
    /// blow. What a verification run uses to see drops land.
    void roll_death_drop(const std::string& unit, int count);
    /// `--press-a`: the pad's A on whatever the prop-and-item pass picks from
    /// where the player stands now - a Unit Trigger interacted with, else the
    /// item picked up. False when nothing is in reach.
    bool press_a();
    /// `--tip-press <b|y>`: that button on the open context tip. False when no
    /// tip is open or the window does not answer the button.
    bool press_tip(const std::string& button);
    /// The debug menu's GODSPEED row, for a run with no pad (`--godspeed`).
    void set_godspeed(bool on);
    /// The debug menu's LEVEL 100 row, for a run with no pad (`level:<n>`):
    /// the levels, then a staff made, the stat points spent on it, and worn.
    void level_up_to(int level);
    /// A on the nearest enabled `Unit Trigger` prop of `unit`, wherever it is
    /// (`--use-prop`). A verification shortcut, the port's own: it stands in
    /// for walking up to a prop the run's route cannot reach.
    bool use_prop(const std::string& unit);
    /// The player's killing blow on the first living placed unit of that name
    /// (`--slay`); `@quest` is the first living unit a quest placed, whatever
    /// its objective resolved to. A verification shortcut, the port's own: a
    /// scene that hangs off a boss dying is reached without the fight.
    bool slay_unit(const std::string& unit);
    /// `--accept-offer`: the quest window's Accept on whatever offer is open.
    bool accept_offer();
    /// The carried pets put back beside the player on the level arrived at,
    /// each the player's pet again.
    void bring_player_pets(const CarriedPlayer& carried);

    /// Put something in the pet's bag, by item name. What a verification run
    /// uses to give the animal something to sell; false when the item index has
    /// no such name or the bag is full.
    bool give_pet_item(const std::string& item_name, int count = 1);
    /// `--pet-learn`: the scroll into the pet's bag, then A on it on the pet's
    /// tray, as the pad would.
    bool pet_learn(const std::string& scroll_name);
    /// `--pet-use`: the item into the pet's bag, then the tray's use of it on
    /// its owner ('s `canUse` and `useOnTarget`).
    bool pet_use(const std::string& item_name);

    /// Pick up every item lying in the level for a quest, as the A press on
    /// each would. What a verification run uses to carry a quest's own copy
    /// (its `QUEST_GUID` stamped) to the turn-in; returns how many it took.
    int pick_up_quest_items();
    /// The same for every item of that `NAME` lying in the level, a quest's or
    /// not (`--take-item`).
    int pick_up_items(const std::string& name);
    /// Drop every item in the player's bag that a quest stamped, as Y on each
    /// would (`--drop-quest-items`); returns how many went.
    int drop_quest_items();
    /// Every held quest's objectives to the log, children indented: the unit
    /// each resolved to, its count and whether it is placed and done
    /// (`--quest-progress`).
    void log_quest_progress();

    /// Take `amount` off the character's health, as a blow would. For a
    /// verification run that wants something for a potion to put back.
    void hurt_player(float amount);

    /// Press hotbar slot `index` - LB is 0, RB is 1 - as the pad would, then
    /// let `seconds` of play pass, logging health and mana either side.
    void press_item_slot(int index, float seconds);

    /// Feed the animal a fish, by item name: a transforming fish's `[EFFECT]` names a
    /// `UNITTYPE:PET` unit and a duration; the others put their effects on the pet. False when
    /// the item index has no such name, the use is refused, or there is no pet here.
    bool feed_pet(const std::string& fish_name);

    /// Throw the whole renderer away and bring an identical one back up (New Game, level
    /// changes). Deliberately blunt: `SceneManager::clearScene()` left state behind (the camera,
    /// RTSS's per-material table, the overlay system, the light map, the level's fields) that
    /// broke drawing. After this the caller sets the subject up again - show_mesh(),
    /// wear_base_outfit(), link_animation(), set_quest() - as at startup. False is fatal.
    bool restart(const RendererConfig& config);

    /// Play the music and ambience the current level authors. Called by load_layout(); exposed so
    /// a caller can silence a verification run. See Impl::start_level_audio().
    void set_audio_enabled(bool on);

    /// Render one frame into `png_path`. Renders to an offscreen target rather
    /// than reading back the window, so the result does not depend on the
    /// window being visible, mapped, or unobscured.
    bool dump_frame(const std::string& png_path);

    /// Render the loading screen to a PNG and take it down again. False when the interface data
    /// cannot be read.
    bool dump_loading_screen(const std::string& png_path);

    /// Render the game screen's HUD over whatever is loaded, to a PNG (every other dump takes the
    /// HUD down).
    bool dump_game_hud(const std::string& png_path);
    /// Whether there is a game HUD to put up (the console data is mounted).
    bool game_hud_ready() const;

    /// Render the game screen with the inventory menu open, to a PNG.
    bool dump_inventory(const std::string& png_path);
    /// A frame after `talk_to_unit`: the HUD and whatever the talk opened (the
    /// quest window, or a vendor's screen in the inventory menu) stay up.
    bool dump_talk(const std::string& png_path);

    /// Render the game screen with the stats menu open, to a PNG.
    bool dump_stats(const std::string& png_path);

    /// A frame with the console skills tab over it, on whichever cell the
    /// cursor is on (a `--pad-script` can walk it first).
    bool dump_skills(const std::string& png_path);
    /// Put the skills tab's cursor on the named skill, for a headless dump:
    /// a lower-cased substring of its display or internal name, the way
    /// `--talk-to` takes one. Returns whether anything matched.
    bool select_skill(const std::string& name);
    /// Open (or close) the map-to-button sheet over the skills tab.
    void set_skill_map_open(bool open);
    /// Spend one skill point on the named skill, matched the way
    /// `select_skill` matches, and restart the passives as
    /// `CSkillManager::setSkillLevel`
    /// does. This is the headless stand-in for `A` on the skills tab,
    /// which only a pad reaches.
    bool learn_skill(const std::string& name);
    /// `--cast <skill>`: `CCharacter::castSkill`
    /// on the named skill, at the play
    /// position. Matches on the class-entry name or the display name, the way
    /// `--learn-skill` does. Returns whether it was cast.
    bool cast_skill(const std::string& name);

    /// Make the nearest hostile unit the player's target and turn the player
    /// to face it, as a pad user lining up a cast would. For `--cast-at-foe`.
    bool face_nearest_foe();
    /// `--cast-at-point`: the point the pad would have aimed
    /// set to `x`, `z`, then `--cast <name>`.
    bool cast_skill_at(const std::string& name, float x, float z);
    /// `--map-skill <name>:<button>`: put the named skill on `y`, `b`, `lt` or
    /// `rt` in the skill set that is up, as the skills tab's map sheet does.
    /// The sheet answers only a pad.
    bool map_skill(const std::string& name, const std::string& button);
    /// `--hotbar <button>`: one frame with `lb`, `rb`, `y`, `b`, `lt` or `rt`
    /// pressed, through the in-game dispatch's slot loop.
    bool press_hotbar(const std::string& button);
    /// Write the player's live effect list as text: one line per effect with
    /// its name, type, activation, value and remaining duration, and one per
    /// affix. Machine-comparable, unlike a rendered screen.
    bool dump_effects(const std::string& txt_path);

    /// Render the game screen with the quest tab open, to a PNG.
    bool dump_quests(const std::string& png_path);

    /// Open the quest offer window on whatever `unit` has to say: `CGameUI::updateIngameUI`'s NPC
    /// path, the quest chosen by `CQuestManager::getQuestForNPC` and its state choosing which of
    /// its four spoken sections shows. False when the unit has nothing, or without the console
    /// interface data.
    bool open_quest_offer(const std::string& unit);
    bool quest_offer_visible() const;

    /// Answer the open offer: `A` accepts, `B` declines; a line only to be read has a single `OK`.
    /// False when nothing is open, or the quest log is full (`CQuestDialogMenu::onClick`'s one
    /// refusal).
    bool answer_quest_offer(bool accept);

    /// Show or hide the reward row, which is what the left stick does while the
    /// offer window is open.
    void toggle_quest_rewards();

    /// Open the offer window on `unit` and write a frame of it.
    bool dump_quest_offer(const std::string& unit, const std::string& png_path);

    /// Finish a quest the player is carrying and pay what it owes: gold, then
    /// experience, then fame, which is the order `CQuestRewards::rewardPlayer`
    /// hands them over in. False for a quest the log has not got.
    bool complete_quest(const std::string& name);

    /// Hand the player `amount` experience the way a kill or a quest does
    /// (`CCharacter::awardExperience`), with `CPlayer::levelUp` on each level
    /// it gains. For `--award-xp`.
    void award_experience(uint64_t amount);

    /// Take a quest by `NAME`, as though the player had pressed Accept. `level` is what
    /// `initializeQuestWithNPC` would have worked out (every reward curve is sampled at it).
    /// False when there is no such quest, or the log is full.
    bool accept_quest(const std::string& name, int level);

    /// Which of the quest tab's six trays `dump_quests()` opens on. A headless
    /// run cannot press down, so without it only the first quest's description
    /// is photographable.
    void set_quest_tab_selection(int index);

    /// Which of the inventory's six filter tabs `dump_inventory()` opens on.
    /// Weapons, Armor, Jewelry, Potions, Spells, Fish - and a dump is the only
    /// way to see a tab that the character has no way to reach headlessly.
    void set_inventory_tab(int tab);

    void toggle_inventory();
    void open_inventory();
    void close_inventory();
    bool inventory_visible() const;

    void toggle_automap();
    void automap_zoom_in();
    void automap_zoom_out();

    /// Present frames until the window is closed, advancing every enabled animation and the
    /// turntable. No-op when headless. Also returns when the quit input was pressed
    /// (`quit_to_menu_requested()`) or a dungeon's exit was reached.
    void run();

    /// Whether `run()` just returned because the quit input (`+` on the console, ESC on desktop)
    /// was pressed. Not reset by reading it; the next `restart()` makes a fresh `Impl`.
    bool quit_to_menu_requested() const;

    /// The quit above came from a retirement (`requestSetGameState(0, 2)`):
    /// the caller saves, then goes straight to New Game in its retirement mode
    /// with what `take_heirloom` hands it.
    bool retirement_requested() const;
    /// The window exists and has not been closed.
    bool window_open() const;
    /// The heirloom and generation the retirement left in the client
    /// (`CGameClient +0x3900` / `+0x3908`), moved out so they outlive the
    /// next `restart()`.
    tl::core::HeirloomHandOff take_heirloom();
    /// `CGameClient::loadLevel` for a player it
    /// creates in state 4: the generation, the heirloom picked up (forced),
    /// and, once the level is in, `awardFame(fameGate(min(generation, 5)))`.
    /// Called after `enable_player_control()`.
    void receive_heirloom(const tl::core::HeirloomHandOff& handoff);

    void shutdown();

private:
    /// The debug wardrobe editor (`render/renderer_wardrobe.cpp`), run from
    /// inside `run_main_menu()`: a class on the backdrop (the Alchemist unless
    /// a dump asks), every look each slot can give it, worn through the
    /// inventory the game's own wardrobe reads.
    void open_wardrobe();
    void close_wardrobe();
    void step_wardrobe(int row, int step);
    void steer_wardrobe(const tl::core::InputState& input, float delta);
    /// dump_wardrobe()'s sweep over the open editor's `rows` (row captions, lowered).
    bool sweep_wardrobe(const std::string& stem, const std::vector<std::string>& rows);
    /// dump_wardrobe()'s row picks in order: `all` (the armour rows) and `<row>=all` are swept
    /// (returned), `<row>=<n>` is worn now.
    std::vector<std::string> pick_wardrobe_rows(
        const std::vector<std::pair<std::string, std::string>>& rows);

    /// The debug bestiary (`render/renderer_bestiary.cpp`), run from inside
    /// `run_main_menu()`: the menu's pet put away and every monster, boss,
    /// townsperson and pet the archive defines placed on its spot in turn.
    void open_bestiary();
    void close_bestiary();
    void step_bestiary(int row, int step);
    void steer_bestiary(const tl::core::InputState& input, float delta);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// How `main` brings the renderer up (start_renderer).
struct RendererStart {
    std::string logic_trace;       ///< `--logic-trace`, set before `init`
    std::string passability_dump;  ///< `--passability-dump`, set after it
    bool start_in_menu = false;    ///< the game starts at its menu: the mods page first
    int argc = 0;                  ///< the command line, for the mods' switches
    char** argv = nullptr;
};

/// `init` with the run's traces, then, when the game starts at its menu, the mods page: shown
/// when there are mods and `config.txt` does not say `mods_menu=0`; a change written to
/// `mods.dat`, `rc`'s mods read again and the renderer restarted on them. `--mods-menu-dump
/// <png>` takes a frame of the page, headless too, and goes on. The exit code when the run ends
/// here (1: `init` failed; 0: the window closed on the page), else nothing.
std::optional<int> start_renderer(Renderer& renderer, RendererConfig& rc,
                                  const RendererStart& start);

}  // namespace tl::render
