// Torchlight: the program's entry point and its command line.
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <cstring>
#if defined(_WIN32)
#include <direct.h>
#endif
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "console_interface.h"
#include "core/asset_path.h"
#include "core/audio.h"
#include "core/input.h"
#include "core/job_pool.h"
#include "core/level_memory.h"
#include "core/log.h"
#include "core/menu_manager.h"
#include "core/pet.h"
#include "core/platform.h"
#include "core/run_config.h"
#include "core/save_directory.h"
#include "core/settings_menu.h"
#include "core/touch_pad.h"
#include "core/waypoints.h"
#include "usage.h"

#if defined(TL_HAVE_OGRE)
#include "render/gpu_skinning.h"
#include "render/renderer.h"
#endif

using tl::core::empty_save_path;
using tl::core::list_continue_entries;
using tl::core::list_saves;
using tl::core::load_game_settings;
using tl::core::make_save_directory;
using tl::core::menu_backdrop_file;
using tl::core::newest_living_save;
using tl::core::save_directory;

namespace {

// The default `--mesh`: the project's reference model.
constexpr const char* kDefaultMesh = "Alchemist.MESH";

// The backdrop pose: idle rather than the bind pose, as the original's menu shows.
constexpr const char* kDefaultAnimation = "Idle";

// The moving pose; players ship a Run but no walk animation.
constexpr const char* kMovingAnimation = "Run";

// Seconds into the animation for a dumped frame: not the bind pose, and the same
// instant on both targets (Idle runs 3.333 s).
constexpr float kDefaultAnimationTime = 1.1f;

// A worn set is one mesh over the body. The full archive path is needed: wardrobe
// basenames repeat across the class directories, and the archive opens none of them.
constexpr const char* kDefaultEquipment = "media/wardrobe/alchemist/plate_set.MESH";

// The main-menu backdrop when no save names one (one ships per tileset).
constexpr const char* kMenuLayout =
    "MEDIA/LAYOUTS/MAINMENUS/1X1SINGLE_ROOM_TOWN/MAINMENU_TOWN.LAYOUT";

// Builds for a device with no terminal (Switch, Android) start at the main menu and
// play; what they read and write beside the game is under the platform's data root.
#if defined(TL_PLATFORM_SWITCH) || defined(TL_PLATFORM_ANDROID) || defined(TL_PLATFORM_IOS)
#define TL_DEVICE_BUILD 1
#endif

// The directory a character file is in, with its separator.
std::string directory_of(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

// `CGameClient::loadCharacter`, its shared stash:
// `CSharedStash::setPlayer`, then `loadSharedStash`.
// DIVERGENCE: read beside the character file, not from `GetSaveDataPath`, so a
// `--save-file` elsewhere leaves the player's own shared stash alone.
void load_character_shared_stash(tl::render::Renderer& renderer, tl::core::SharedStash& stash,
                                 bool hardcore, const std::string& character_path) {
    stash.set_player(hardcore);
    renderer.load_shared_stash(directory_of(character_path));
}

// The menu backdrop: `CGameClient::setGameState` loads the newest save
// (`from` overrides it) and `loadMenuLevel` at its dungeon and depth; with none, the
// town. Reading it loads the shared stash too, for a New Game after it.
std::string menu_backdrop(tl::render::Renderer& renderer, const std::string& from,
                          tl::core::SharedStash& stash) {
    std::string path = from;
    if (path.empty()) {
        const std::vector<std::string> saves = list_saves();
        if (!saves.empty()) path = saves.front();
    }
    tl::formats::CharacterFile file;
    if (path.empty() || !tl::core::read_character_with_backup(path, file)) {
        return kMenuLayout;
    }
    load_character_shared_stash(renderer, stash, file.hardcore != 0, path);
    const std::string layout = renderer.menu_layout_for(file.current_dungeon, file.player.depth);
    return layout.empty() ? std::string(kMenuLayout) : layout;
}

// The playable town, the level `media/dungeons/town.dat` names as the Town dungeon.
constexpr const char* kTownLayout = "MEDIA/LAYOUTS/TOWN/1X1SINGLE_ROOM/TOWN.LAYOUT";

struct Options {
    std::string game_data;
    std::string pak;  ///< `--pak`: RendererConfig::pak
    std::string ogre_media;
    std::string dump_frame;
    std::string mesh = kDefaultMesh;
    std::string layout;

    /// `--dungeon <Name>[,floor]`: generate one floor of a procedural dungeon, by a
    /// `.DAT.adm` file's `NAME`; floor 1 by default (`Renderer::load_dungeon()`).
    std::string dungeon;
    int dungeon_floor = 1;
    /// `--rng-trace <file>`: see `Renderer::set_rng_trace()`.
    std::string rng_trace;

    /// `--logic-trace <file>`: see `Renderer::set_logic_trace()`.
    std::string logic_trace;

    /// `--ground-probe x,z` (repeatable): see `Renderer::probe_ground()`.
    std::vector<std::string> ground_probes;
    /// `--passability-dump <file>`: see `Renderer::set_passability_dump()`.
    std::string passability_dump;

    std::vector<std::string> equipment;
    std::vector<std::string> link_animations;

    /// Quest state set before the level loads, to reach story-gated content: each
    /// entry `NAME`, `NAME:complete` or `NAME:active` (a bare name is complete).
    std::vector<std::string> quests;

    /// `x,z,seconds,png`: after everything else, stand the character there, run the
    /// scene forward and dump - for a trigger no scripted walk reaches.
    std::vector<std::string> visits;

    /// `x,z[,png]`: path the character there through the navigation grid, walk it,
    /// and optionally dump. Repeatable, to walk a route in legs.
    std::vector<std::string> walk_to;

    /// `--pad-script <presses>`: drive the pad from a script
    /// (`tl::core::set_pad_script`), e.g. `down,down,a,text=Lara,b`.
    std::string pad_script;
    /// `--accept-offer "<seconds>[,<DUNGEON:floor>]"`: Accept the offer
    /// `--talk-to` opened, run that long, then (optionally) warp there - what
    /// is carried across, the player's new pet among it, is in the log.
    std::string accept_offer;

    /// `--class <name>`: start as this class, skipping the creation screen. Also
    /// what the creation screen writes its answer into.
    std::string player_class;
    bool class_explicit = false;

    /// `--create-dump <png>`: one frame of the creation screen, before anybody
    /// has pointed at it. The twin of `--menu-dump`.
    std::string create_dump;

    /// Class names to step the creation screen's selection through, headlessly,
    /// before it is taken. `--create-cycle Destroyer,Vanquisher,Alchemist`.
    std::vector<std::string> create_cycle;

    /// `--talk-to <name[,png]>`: walk to a named unit and open what they say, as
    /// pressing A on them does.
    std::vector<std::string> talk_to;
    /// `--vendor-keys`: presses on the vendor screen `--talk-to` opened, before
    /// its frame is dumped. Repeatable: the n-th list is for the n-th
    /// `--talk-to`, and the last one for any after it.
    std::vector<std::string> vendor_keys;
    /// `--inventory-keys`: presses on the player's own inventory screen
    /// (`Renderer::inventory_keys`), after `--give-item`. Repeatable.
    std::vector<std::string> inventory_keys;
    /// `--fishing-keys`: steps through the fishing `--talk-to "Fishing Hole"`
    /// started (`Renderer::fishing_keys`). Repeatable like `--vendor-keys`.
    std::vector<std::string> fishing_keys;

    /// `--travel DEST[:floor],seconds[,png]`: take the player there the way a warp
    /// does (`CGameClient::performWarp`), let the level run, and optionally dump a
    /// frame. `TOWN` or a dungeon.
    std::vector<std::string> travels;
    /// `--portal open|map:ITEM|spell|enter|up|down|waygate|waypoints|choose:N[,seconds[,png]]`:
    /// read a Town Portal Scroll; read map scroll `ITEM`; cast Town Portal; enter the
    /// open portal; take a floor's stairs; use the waygate; open the waypoint menu;
    /// choose its row N. Then seconds of play and a frame. After `--travel`.
    std::vector<std::string> portal_steps;

    /// `--save-file PATH`: save this run's character there, at the points the
    /// original saves (a warp, leaving for the menu). `--continue-file PATH`:
    /// start from that save rather than from a layout, as Continue does.
    std::string save_file;
    std::string continue_file;
    /// `--menu-from-save PATH`: the save the menu's backdrop is chosen from, in
    /// place of the newest in the save directory.
    std::string menu_from_save;
    /// `--menu-dungeon NAME:DEPTH`: the backdrop `CGameClient::loadMenuLevel`
    /// brings up for that dungeon and depth, as a save there would.
    std::string menu_dungeon;

    /// `--die CHOICE[,screen.png[,after.png]]`: take the player's life, run until
    /// the death screen opens, dump it, and choose `here`, `entrance`, `town` or
    /// `title`; `after.png` is a second later. After the `--travel` steps.
    std::vector<std::string> deaths;

    /// `--hardcore`: a hardcore character (`CPlayer +0xa15`), for a run that
    /// does not pass the difficulty screen, whose checkbox is where it is set.
    bool hardcore = false;

    /// `--difficulty` or `--hardcore` was given: New Game skips the difficulty
    /// screen, as `--class` skips character creation, and for the same reason.
    bool difficulty_explicit = false;

    /// `--difficulty-dump <png>`: one frame of the difficulty screen, before
    /// anybody has pressed anything. The twin of `--create-dump`.
    std::string difficulty_dump;

    /// `--load-character-dump <png>`: one frame of the Load Character screen.
    /// `--load-character-keys <keys>` presses first, a letter each (`u` `d` `a` `b`
    /// `x` `y`). **`y` then `a` deletes the selected save**, as in play.
    std::string load_character_dump;
    std::string load_character_keys;

    /// Seconds into a `--visit` at which to fire the skip, or negative for
    /// never. The verification path for `Skip Cutscene`.
    float skip_after = -1.0f;

    std::string animation;

    /// Where to park the animation before the frame is dumped.
    float animation_time = kDefaultAnimationTime;
    float turntable = 9.0f;

    /// Vertical field of view in degrees; 0 keeps OGRE's default of 45.
    float fov = 0.0f;

    /// The animation was asked for: one that cannot be honoured is then a failure.
    bool animation_explicit = false;

    /// Whether the wardrobe was chosen rather than defaulted into, on the same
    /// reasoning as animation_explicit.
    bool equipment_explicit = false;

    /// Separate from animation_explicit: `--animation-time` alone applies to the
    /// default animation, which the defaults must not overwrite.
    bool animation_time_explicit = false;

    /// Keep the character swinging while the scene runs; implies `--play`.
    bool attack = false;

    /// Run the console's target selector headlessly, logging its picks; implies `--play`.
    bool pad = false;

    /// Units to light up as the console's selector would, for a static frame
    /// that can be compared against one without them (`--highlight`).
    std::vector<std::string> highlights;

    /// `--fight <seconds>,<png>`: fight where the character stands, then dump;
    /// `--stand` (not swinging) as `stand:<seconds>,<png>`. Repeatable, in sequence.
    std::vector<std::string> fights;
    unsigned width = 1280;
    unsigned height = 720;
    bool size_given = false;  ///< `--width` or `--height`: config.txt's size stands aside
    bool fullscreen = false;  ///< config.txt's `fullscreen` (the desktop's)
    bool vsync = true;        ///< config.txt's `vsync` (the desktop's)
    /// Dress the subject as an unequipped character: the player unit's
    /// <SLOT>_BASE textures on the body plus HELMET_BASE_MESH. The menu's
    /// default; `--base-outfit` asks for it anywhere.
    bool base_outfit = false;
    bool headless = false;
    bool help = false;

    /// Start at the main menu, the way the game does: the default unless a scene or
    /// a frame is asked for (a menu waiting for a key would hang that run).
    bool start_in_menu = false;

    /// `--no-menu`: go straight into the game. For iterating on the town
    /// without clicking through the menu each time.
    bool no_menu = false;

    /// Whether a scene was named rather than defaulted into.
    bool layout_explicit = false;

    /// Whether a mesh was named: `--mesh X` alone means "look at this model", not
    /// the game's front end.
    bool mesh_explicit = false;

    /// Where to write a frame of the menu before it is handed to the player.
    std::string menu_dump;
    float menu_dump_at = 0.0f;  ///< `--menu-dump-at SECONDS`: Renderer::menu_dump_delay

    /// Where to write a frame of the loading screen, painted once before a
    /// synchronous load.
    std::string loading_dump;

    /// Where to write a frame of the main menu's debug wardrobe editor (the port's
    /// own screen), with `--wardrobe-picks` chosen first (`chest=3,weapon=12`).
    std::string wardrobe_dump;
    std::string wardrobe_picks;

    /// The same for the main menu's debug bestiary, with `--bestiary-picks`
    /// chosen first (`unit=ordrak,animation=2`). `unit=all` writes one frame
    /// per unit instead, `<stem>_<NNN>_<name>.png` beside the path given.
    std::string bestiary_dump;
    std::string bestiary_picks;

    /// Where to write a frame of the main menu's Choose Level biome list (the
    /// port's own screen).
    std::string level_list_dump;

    /// Where to write a frame of the level with the game screen's HUD over it.
    /// Every other dumped frame takes the HUD down on purpose.
    std::string hud_dump;
    float hud_wait = 0.0f;  ///< `--hud-wait SECONDS`, run before `--hud-dump`

    /// `--drop-dump <item>,<png>`: drop the first stack of that item the way Y
    /// on the inventory screen does, let the toss land, and write the frame.
    std::string drop_dump;

    /// Where to write a frame of the level with the console inventory menu over it.
    std::string inventory_dump;

    /// Where to write a frame of the level with the console stats menu over it.
    std::string stats_dump;

    /// Where to write a frame of the level with the console skills tab over it.
    std::string skills_dump;

    /// Where to write a frame of the level with the console quest tab over it.
    std::string quest_dump;

    /// `<npc[,png]>`, repeatable: open the quest offer window on that unit and
    /// optionally dump it. The name is a lower-cased substring, as `--talk-to`.
    std::vector<std::string> quest_offers;

    /// Which of the quest tab's six trays `--quest-dump` lights, 0-based.
    int quest_slot = 0;

    /// Quests to finish before anything is drawn, by `NAME`, after `--accept-quest`;
    /// pays what the quest owes.
    std::vector<std::string> complete_quests;

    /// `--pick-up-quest-items`: take every item a quest put in the level, as the A
    /// press on each would, before `--accept-quest` and `--complete-quest`.
    bool pick_up_quest_items = false;

    /// `--award-xp <amount>`: experience handed over once the level is up, through
    /// the same award a kill or a quest makes.
    uint64_t award_xp = 0;

    /// `--hurt <amount>`: take that much health off once the level is up.
    /// `--item-slot <index>` (repeatable): then press that hotbar slot (0 LB, the
    /// health potion; 1 RB, the mana potion) and run five seconds, logging both.
    float hurt = 0.0f;
    std::vector<int> item_slots;

    /// Quests to take before anything is drawn, by `NAME`, repeatable: Accept, with
    /// a tray, a rolled reward and a journal entry (`--quest` only sets a gate).
    std::vector<std::string> accept_quests;

    /// The level the accepted quests are worth, at which every reward curve is
    /// sampled (`calculate_quest_reward()`).
    int quest_level = 2;

    /// Which inventory filter tab `--inventory-dump` opens on: 0 Weapons, 1 Armor,
    /// 2 Jewelry, 3 Potions, 4 Spells, 5 Fish.
    int inventory_tab = 0;

    /// Whether to start with the inventory menu open.
    bool show_inventory = false;

    /// Where to write a frame of the class intro sequence.
    /// `--unit-heights`: also after every `--travel`.
    bool unit_heights = false;
    std::string intro_dump;
    /// `--cinematic NAME`: which `[CINEMATIC]` `--intro-dump` photographs
    /// (`VIGNETTE1`); the class's own introduction when not given.
    std::string cinematic;

    /// Play the class introduction sequence even when not starting through
    /// the full character creation menu.
    bool intro = false;

    /// Bypass the class introduction sequence.
    bool no_intro = false;

    /// Whether the minimap is disabled.
    bool no_minimap = false;
    int fps_overlay = -1;  ///< `--fps` 1, `--no-fps` 0, -1 the default
    int fps_limit = -1;    ///< `--fps-limit N` (0 none), -1 config.txt's
    int tips = -1;         ///< `--tips` 1, `--no-tips` 0, -1 the default

    /// Whether to start with fullscreen automap overlay.
    bool fullscreen_map = false;

    /// Initial automap zoom level (20..60): 30, as `CAutomap::CAutomap`
    /// sets it.
    float map_zoom = 30.0f;

    /// Levels and loose stat points to grant the new character, so that the
    /// stats screen has something to show. Zero unless asked for.
    int level_ups = 0;
    int stat_points = 0;
    int skill_points = 0;

    /// `--skill-set <0|1>`: which of the two skill sets the skills tab shows.
    int skill_set = 0;

    /// `--skill-select <name>`: put the skills tab's cursor on that skill before the
    /// dump; `--skill-map` opens the map-to-button sheet over it.
    std::string skill_select;
    /// `--learn-skill <name>`: spend a point on that skill before the dump, repeatable.
    std::vector<std::string> learn_skills;
    /// `--cast <skill>`: cast that skill once at the play position, repeatable, through
    /// `CCharacter::castSkill`.
    std::vector<std::string> cast_skills;
    /// `--map-skill <name>:<button>`: put a skill on `y`, `b`, `lt` or `rt`
    /// before the casts, repeatable - the skills tab's map sheet, headless.
    std::vector<std::string> map_skills;
    /// `--hotbar <button>`: press `lb`, `rb`, `y`, `b`, `lt` or `rt` for one
    /// frame through the in-game dispatch, after the casts, repeatable.
    std::vector<std::string> hotbar_presses;
    /// `--hotbar-dump <png>`: the HUD after those presses, so the slots show
    /// what was mapped, cast and drunk (`--hud-dump` is taken before them).
    std::string hotbar_dump;
    /// `--effects-dump <txt>`: the player's live effect list, as text.
    std::string effects_dump;
    bool skill_map = false;
    int foremen_kills = 0;

    /// `--pet <Dog|Cat|none>`: which animal follows the character, as the creation
    /// screen would choose (`Dog` is what `charactercreate.layout` opens on).
    std::string pet = "Dog";

    /// `--audio`: open the audio device even though the run is headless
    /// (`RendererConfig::force_audio`).
    bool force_audio = false;

    /// `--mute`: never open the audio device, windowed or not, and whatever
    /// `--audio` says. See `RendererConfig::mute`.
    bool mute = false;
    /// `--touch-pad`: draw the on-screen pad where no touch screen puts it there.
    bool touch_pad = false;
    /// `--cpu-skinning`: characters skinned on the CPU from the start, as the debug
    /// menu's GPU SKINNING row does.
    bool cpu_skinning = false;
    /// `--workers N`: threads beside the game's for its fork-join work (core/job_pool.h); 0
    /// keeps all of it on the game's thread. -1: the default.
    int workers = -1;
    /// `--bench SECONDS`: RendererConfig::bench_seconds.
    float bench_seconds = 0.0f;
    bool invulnerable = false;  ///< `--invulnerable`: RendererConfig::invulnerable

    /// `--no-starting-fish`: leave the Xbox release's `Improbably Large Fish` out of
    /// a new character's bag.
    bool no_starting_fish = false;

    /// `--pet-stance <aggressive|defensive|passive>`: the pet's stance; defensive
    /// unless asked (`tl::core::kDefaultStance`).
    std::string pet_stance;

    /// `--give-item <name[,enchant]>`: make a copy of an item for the player
    /// and wear it where it fits (the bag otherwise), repeatable; `enchant`
    /// forces the magic roll (`createItem`'s fourth argument).
    std::vector<std::string> give_items;

    /// `--roll-death-drop <unit[,count]>`: what `unit` leaves when the player
    /// kills it, rolled `count` times at the player's feet, repeatable.
    std::vector<std::string> death_drops;

    /// `--pet-item <name[,count]>`: put something in the pet's bag, repeatable.
    std::vector<std::string> pet_items;
    /// `--pet-learn <scroll>`: the scroll into the pet's bag and A on it on
    /// the pet's tray, repeatable (a third on a full pet opens the replace
    /// dialog, which needs a pad).
    std::vector<std::string> pet_learns;

    /// `--pet-use <item>`: the item into the pet's bag and A on it on the
    /// pet's tray - a potion or a scroll used by the pet on itself.
    std::vector<std::string> pet_uses;

    /// `--pet-fish <name>`: feed the animal that fish once the level is up.
    /// Repeatable, so a run can turn a dog into a spider and back.
    std::vector<std::string> pet_fish;

    /// `--send-pet-to-town`: send it shopping once the level is up.
    bool send_pet_to_town = false;

    /// `--pet-dump <png>`: a frame with the pet panel and the animal in it.
    std::string pet_dump;

    /// More archives or directories to mount beside the install
    /// (`RendererConfig::extra_data`).
    std::vector<std::string> extra_data;
    /// `--mod-data <dir>`: a mod root containing MEDIA/, mounted before base data.
    std::vector<std::string> mod_data;
    /// `--console-ui DIR`: the console interface for a PC archive
    /// (`RendererConfig::console_ui`), unused beside the Xbox 360 pak.
    std::string console_ui;

    /// `--no-hud`: leave the game screen's HUD off.
    bool no_hud = false;

    /// `--auto-new-game`: take the New Game branch without anyone pressing it.
    bool auto_new_game = false;

    /// Bisection switches: leave out the particles, or the projected light map,
    /// so a visual fault can be attributed to a layer in one run.
    bool no_particles = false;
    bool no_torchlight = false;
    bool no_shadows = false;
    /// `--light-map-dump`: see RendererConfig::light_map_dump.
    bool light_map_dump = false;
    /// `--draw-dump <dir>`: see RendererConfig::draw_dump.
    std::string draw_dump;
    /// `--dump-gamma`: see RendererConfig::dump_gamma.
    bool dump_gamma = false;
    /// A ParticleUniverse system built in code at the player.
    bool pu_smoke = false;
    /// `--pu-particles`: see RendererConfig::pu_particle_layouts.
    std::vector<std::string> pu_particle_layouts = {"*"};
    bool pu_particle_layouts_given = false;

    /// Hand the character to the player: direct movement on the left stick, or
    /// WASD on the desktop, with the camera following.
    bool play = false;
    int difficulty = 2;

    /// Seconds of scripted walking to run; 0 is off.
    float walk_seconds = 0.0f;

    /// Stand the character at these world coordinates before dumping, instead of at
    /// the level's spawn point. Empty means the spawn.
    std::string at;

    /// Frames taken after running the scene forward, as (seconds, path) pairs,
    /// sorted; `--dump-frame` is taken before anything moves.
    std::vector<std::pair<float, std::string>> dump_at;

    /// Where to write the frame taken *after* the scripted walk (`dump_frame` is
    /// taken before it).
    std::string walk_dump;
};

// The switches that only set one option: `--name VALUE` into a string, and a
// bare `--name` into a flag. Every other switch is parse_args's chain.
bool set_string_option(Options& o, const std::string& arg, const char* value) {
    static const std::pair<const char*, std::string Options::*> kOptions[] = {
        {"--pak", &Options::pak},
        {"--ogre-media", &Options::ogre_media},
        {"--dump-frame", &Options::dump_frame},
        {"--rng-trace", &Options::rng_trace},
        {"--logic-trace", &Options::logic_trace},
        {"--passability-dump", &Options::passability_dump},
        {"--menu-dump", &Options::menu_dump},
        {"--wardrobe-dump", &Options::wardrobe_dump},
        {"--wardrobe-picks", &Options::wardrobe_picks},
        {"--bestiary-dump", &Options::bestiary_dump},
        {"--bestiary-picks", &Options::bestiary_picks},
        {"--level-list-dump", &Options::level_list_dump},
        {"--loading-dump", &Options::loading_dump},
        {"--hud-dump", &Options::hud_dump},
        {"--skill-select", &Options::skill_select},
        {"--pet-stance", &Options::pet_stance},
        {"--pet-dump", &Options::pet_dump},
        {"--console-ui", &Options::console_ui},
    };
    for (const auto& [name, field] : kOptions) {
        if (arg != name) continue;
        o.*field = value;
        return true;
    }
    return false;
}

bool set_flag_option(Options& o, const std::string& arg) {
    static const std::pair<const char*, bool Options::*> kOptions[] = {
        {"--no-menu", &Options::no_menu},
        {"--auto-new-game", &Options::auto_new_game},
        {"--pick-up-quest-items", &Options::pick_up_quest_items},
        {"--inventory", &Options::show_inventory},
        {"--skill-map", &Options::skill_map},
        {"--audio", &Options::force_audio},
        {"--mute", &Options::mute},
        {"--touch-pad", &Options::touch_pad},
        {"--cpu-skinning", &Options::cpu_skinning},
        {"--invulnerable", &Options::invulnerable},
        {"--no-starting-fish", &Options::no_starting_fish},
        {"--send-pet-to-town", &Options::send_pet_to_town},
        {"--no-hud", &Options::no_hud},
        {"--no-minimap", &Options::no_minimap},
        {"--fullscreen-map", &Options::fullscreen_map},
    };
    for (const auto& [name, field] : kOptions) {
        if (arg != name) continue;
        o.*field = true;
        return true;
    }
    return false;
}

// `--name N` into a count, negatives read as 0.
bool set_count_option(Options& o, const std::string& arg, const char* value) {
    static const std::pair<const char*, int Options::*> kOptions[] = {
        {"--quest-slot", &Options::quest_slot},
        {"--inventory-tab", &Options::inventory_tab},
        {"--workers", &Options::workers},
        {"--level-up", &Options::level_ups},
        {"--stat-points", &Options::stat_points},
        {"--skill-points", &Options::skill_points},
        {"--foremen-kills", &Options::foremen_kills},
    };
    for (const auto& [name, field] : kOptions) {
        if (arg != name) continue;
        o.*field = std::max(0, std::atoi(value));
        return true;
    }
    return false;
}

#if defined(TL_DEVICE_BUILD)
// A device build's defaults: it has no command line, so they are its whole
// interface.
void apply_device_defaults(Options& o) {
    const std::string root = tl::core::platform_data_root();
    // The Xbox 360's own arrangement: the archive, `music/` and the shader folder
    // beside the program, at the root itself.
    o.game_data = root;

    // A PC archive's console interface (none beside the Xbox pak); else the built-in pack.
    o.console_ui = root + "console_ui";

    // Starts at the main menu, then plays. One frame each side of the menu is
    // dumped, before anything is handed over.
    o.menu_dump = root + "menu.png";
    o.dump_frame = root + "frame.png";
#if defined(TL_PLATFORM_SWITCH)
    // TEMPORARY: diagnosing the automap's black-square minimap on the console.
    o.hud_dump = root + "hud_debug.png";
#endif
    o.start_in_menu = true;

    // The field of view is left at 0: the settings file's `FOV : 45` (vertical at
    // 4:3) is derived for the viewport's aspect.
}
#endif

bool is_directory(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool is_regular_file(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

// The desktop run from a player's folder rather than from the source tree: a
// pak.zip in the working directory. It then reads the game and OGRE's shader
// library from there and keeps config.txt there, as a device does.
#if !defined(TL_DEVICE_BUILD)
bool player_folder() { return is_regular_file("pak.zip") || is_regular_file("Pak.zip"); }
#endif

#if !defined(TL_DEVICE_BUILD)
// The desktop's defaults: the source tree's `game_data/`, or a player's folder
// (scripts/package_windows.py) - the program beside its own pak.zip, music/
// and RTShaderLib/, laid out as the devices lay theirs.
void apply_desktop_defaults(Options& o) {
    o.game_data = "game_data";
    if (!player_folder()) return;
    o.game_data = "./";
    if (is_directory("RTShaderLib")) o.ogre_media = ".";
}
#endif

Options parse_args(int argc, char** argv) {
    Options o;
#if defined(TL_DEVICE_BUILD)
    apply_device_defaults(o);
#else
    apply_desktop_defaults(o);
#endif

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_next = (i + 1 < argc);

        if (has_next &&
            (set_string_option(o, arg, argv[i + 1]) || set_count_option(o, arg, argv[i + 1]))) {
            ++i;
            continue;
        }
        if (set_flag_option(o, arg)) continue;
        if (arg == "--game-data" && has_next) {
            o.game_data = argv[++i];
        } else if (arg == "--mesh" && has_next) {
            o.mesh = argv[++i];
            o.mesh_explicit = true;
        } else if (arg == "--layout" && has_next) {
            o.layout = argv[++i];
            o.layout_explicit = true;
        } else if (arg == "--dungeon" && has_next) {
            // "<Name>[,floor]" - see the note on Options::dungeon.
            const std::string spec = argv[++i];
            const size_t c1 = spec.find(',');
            o.dungeon = spec.substr(0, c1);
            if (c1 != std::string::npos) {
                const size_t c2 = spec.find(',', c1 + 1);
                o.dungeon_floor = std::max(1, std::atoi(spec.substr(c1 + 1, c2 - c1 - 1).c_str()));
                if (c2 != std::string::npos) {
                    std::fprintf(stderr,
                                 "--dungeon takes no seed any more: a floor is seeded as "
                                 "the original seeds it\n");
                }
            }
            o.play = true;
        } else if (arg == "--ground-probe" && has_next) {
            o.ground_probes.push_back(argv[++i]);
        } else if (arg == "--menu") {
            o.layout = kMenuLayout;
            o.layout_explicit = true;
        } else if (arg == "--menu-dump-at" && has_next) {
            o.menu_dump_at = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--drop-dump" && has_next) {
            o.drop_dump = argv[++i];
            o.play = true;
        } else if (arg == "--inventory-dump" && has_next) {
            o.inventory_dump = argv[++i];
            o.play = true;
        } else if (arg == "--stats-dump" && has_next) {
            o.stats_dump = argv[++i];
            o.play = true;
        } else if (arg == "--skills-dump" && has_next) {
            o.skills_dump = argv[++i];
            o.play = true;
        } else if (arg == "--quest-dump" && has_next) {
            o.quest_dump = argv[++i];
            o.play = true;
        } else if (arg == "--quest-offer" && has_next) {
            o.quest_offers.push_back(argv[++i]);
            o.play = true;
        } else if (arg == "--accept-quest" && has_next) {
            o.accept_quests.push_back(argv[++i]);
        } else if (arg == "--complete-quest" && has_next) {
            o.complete_quests.push_back(argv[++i]);
        } else if (arg == "--award-xp" && has_next) {
            o.award_xp = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--hurt" && has_next) {
            o.hurt = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--item-slot" && has_next) {
            o.item_slots.push_back(std::atoi(argv[++i]));
        } else if (arg == "--quest-level" && has_next) {
            o.quest_level = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--skill-set" && has_next) {
            o.skill_set = std::atoi(argv[++i]) == 0 ? 0 : 1;
        } else if (arg == "--learn-skill" && has_next) {
            o.learn_skills.push_back(argv[++i]);
            o.play = true;
        } else if (arg == "--cast" && has_next) {
            o.cast_skills.push_back(argv[++i]);
            o.play = true;
        } else if (arg == "--map-skill" && has_next) {
            o.map_skills.push_back(argv[++i]);
            o.play = true;
        } else if (arg == "--hotbar" && has_next) {
            o.hotbar_presses.push_back(argv[++i]);
            o.play = true;
        } else if (arg == "--hotbar-dump" && has_next) {
            o.hotbar_dump = argv[++i];
            o.play = true;
        } else if (arg == "--effects-dump" && has_next) {
            o.effects_dump = argv[++i];
            o.play = true;
        } else if (arg == "--bench" && has_next) {
            o.bench_seconds = std::strtof(argv[++i], nullptr);
        } else if (arg == "--tips") {
            o.tips = 1;
        } else if (arg == "--no-tips") {
            o.tips = 0;
        } else if (arg == "--pet" && has_next) {
            o.pet = argv[++i];
            // `--pet none` is how a run asks for no animal at all, which an
            // empty string on a command line cannot say.
            if (o.pet == "none" || o.pet == "None") o.pet.clear();
        } else if (arg == "--give-item" && has_next) {
            o.give_items.push_back(argv[++i]);
        } else if (arg == "--roll-death-drop" && has_next) {
            o.death_drops.push_back(argv[++i]);
        } else if (arg == "--pet-item" && has_next) {
            o.pet_items.push_back(argv[++i]);
        } else if (arg == "--pet-learn" && has_next) {
            o.pet_learns.push_back(argv[++i]);
        } else if (arg == "--pet-use" && has_next) {
            o.pet_uses.push_back(argv[++i]);
        } else if (arg == "--pet-fish" && has_next) {
            o.pet_fish.push_back(argv[++i]);
        } else if (arg == "--extra-data" && has_next) {
            o.extra_data.push_back(argv[++i]);
        } else if (arg == "--mod-data" && has_next) {
            o.mod_data.push_back(argv[++i]);
        } else if (arg == "--fps") {
            o.fps_overlay = 1;
        } else if (arg == "--no-fps") {
            o.fps_overlay = 0;
        } else if (arg == "--fps-limit" && has_next) {
            o.fps_limit = std::max(0, std::atoi(argv[++i]));
        } else if (arg == "--map-zoom" && has_next) {
            o.map_zoom = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--equip" && has_next) {
            o.equipment.push_back(argv[++i]);
            o.equipment_explicit = true;
        } else if (arg == "--base-outfit") {
            o.base_outfit = true;
            o.equipment_explicit = true;
        } else if (arg == "--naked") {
            o.equipment.clear();
            o.equipment_explicit = true;
        } else if (arg == "--visit" && has_next) {
            o.visits.push_back(argv[++i]);
        } else if (arg == "--quest" && has_next) {
            o.quests.push_back(argv[++i]);
        } else if (arg == "--link-animation" && has_next) {
            o.link_animations.push_back(argv[++i]);
            o.animation_explicit = true;
        } else if (arg == "--animation" && has_next) {
            o.animation = argv[++i];
            o.animation_explicit = true;
        } else if (arg == "--animation-time" && has_next) {
            o.animation_time = static_cast<float>(std::atof(argv[++i]));
            o.animation_time_explicit = true;
        } else if (arg == "--fov" && has_next) {
            o.fov = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--turntable" && has_next) {
            o.turntable = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--width" && has_next) {
            o.width = static_cast<unsigned>(std::atoi(argv[++i]));
            o.size_given = true;
        } else if (arg == "--height" && has_next) {
            o.height = static_cast<unsigned>(std::atoi(argv[++i]));
            o.size_given = true;
        } else if (arg == "--highlight" && has_next) {
            o.highlights.push_back(argv[++i]);
        } else if (arg == "--pad") {
            o.pad = true;
            o.play = true;
        } else if (arg == "--attack") {
            o.attack = true;
            o.play = true;
        } else if (arg == "--fight" && has_next) {
            o.fights.push_back(argv[++i]);
            o.play = true;
        } else if (arg == "--cast-at-foe" && has_next) {
            o.fights.push_back(std::string("cast:") + argv[++i]);
            o.play = true;
        } else if (arg == "--cast-here" && has_next) {
            // "<skill>,<seconds>,<png>": `--cast` where it stands in the list,
            // aimed as the pad aims with the stick at rest, then the scene run.
            o.fights.push_back(std::string("here:") + argv[++i]);
            o.play = true;
        } else if (arg == "--cast-at-point" && has_next) {
            // "<skill>,<x>,<z>,<seconds>,<png>": a position skill cast at a
            // point, as the pad's stick would aim it, then the scene run.
            o.fights.push_back(std::string("point:") + argv[++i]);
            o.play = true;
        } else if (arg == "--stand" && has_next) {
            o.fights.push_back(std::string("stand:") + argv[++i]);
            o.play = true;
        } else if (arg == "--talk-to-step" && has_next) {
            // `--talk-to` where it stands in the list: walk up to a named unit
            // or prop and press A on it, then carry on with the next step.
            o.fights.push_back(std::string("talk:") + argv[++i]);
            o.play = true;
        } else if (arg == "--teleport" && has_next) {
            // "<x>,<z>": `--at` where it stands in the list; the pets come along.
            o.fights.push_back(std::string("teleport:") + argv[++i]);
            o.play = true;
        } else if (arg == "--slay" && has_next) {
            // The player's killing blow on a placed unit, in the list's order.
            o.fights.push_back(std::string("slay:") + argv[++i]);
            o.play = true;
        } else if (arg == "--save-now") {
            // The character saved to `--save-file`, where it stands in the list.
            o.fights.push_back("save:");
            o.play = true;
        } else if (arg == "--take-quest-items") {
            // `--pick-up-quest-items` where it stands in the list.
            o.fights.push_back("take:");
            o.play = true;
        } else if (arg == "--take-item" && has_next) {
            // A on every item of that name lying in the level, where it stands in the list.
            o.fights.push_back(std::string("takeitem:") + argv[++i]);
            o.play = true;
        } else if (arg == "--drop-quest-items") {
            // Y in the bag on every item a quest stamped, where it stands in the list.
            o.fights.push_back("drop:");
            o.play = true;
        } else if (arg == "--quest-progress") {
            // Every held quest's objectives to the log, where it stands in the list.
            o.fights.push_back("progress:");
            o.play = true;
        } else if (arg == "--use-prop" && has_next) {
            // A on the nearest prop of that unit, wherever it is, in the list's order.
            o.fights.push_back(std::string("use:") + argv[++i]);
            o.play = true;
        } else if (arg == "--godspeed") {
            // The debug menu's GODSPEED switched on, where it stands in the list.
            o.fights.push_back("godspeed:");
            o.play = true;
        } else if (arg == "--debug-level" && has_next) {
            // The debug menu's LEVEL row, where it stands in the list.
            o.fights.push_back(std::string("level:") + argv[++i]);
            o.play = true;
        } else if (arg == "--press-a") {
            // The pad's A on the prop or item in reach, where it stands in the list.
            o.fights.push_back("press:");
            o.play = true;
        } else if (arg == "--tip-press" && has_next) {
            // "<b|y>": that button on the open context tip, where it stands in the list.
            o.fights.push_back(std::string("tip:") + argv[++i]);
            o.play = true;
        } else if (arg == "--unit-heights") {
            // Every placed unit's height against the ground under it, in the fight list.
            o.fights.push_back("heights:");
            o.unit_heights = true;
            o.play = true;
        } else if (arg == "--hold-hotbar" && has_next) {
            // "<button>,<seconds>[,<png>]": a hotbar button held, in the fight
            // list, and the frame before it is let go.
            o.fights.push_back(std::string("hold:") + argv[++i]);
            o.play = true;
        } else if (arg == "--stick" && has_next) {
            // "<x>,<y>,<seconds>": the left stick held, in the fight list.
            o.fights.push_back(std::string("stick:") + argv[++i]);
            o.play = true;
        } else if (arg == "--hold-x" && has_next) {
            // "<x>,<y>,<seconds>[,<png>]": the attack button held with the
            // left stick at (x, y), in the fight list.
            o.fights.push_back(std::string("holdx:") + argv[++i]);
            o.play = true;
        } else if (arg == "--walk-after" && has_next) {
            // "<x>,<z>": a walk in the fight list, to go over what a kill dropped.
            o.fights.push_back(std::string("walk:") + argv[++i]);
            o.play = true;
        } else if (arg == "--difficulty" && has_next) {
            const std::string level = argv[++i];
            o.difficulty = level == "easy"       ? 0
                           : level == "normal"   ? 1
                           : level == "veryhard" ? 3
                                                 : 2;
            o.difficulty_explicit = true;
        } else if (arg == "--play") {
            o.play = true;
        } else if (arg == "--walk" && has_next) {
            o.walk_seconds = static_cast<float>(std::atof(argv[++i]));
            o.play = true;
        } else if (arg == "--skip-after" && has_next) {
            o.skip_after = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--accept-offer" && has_next) {
            o.accept_offer = argv[++i];
        } else if (arg == "--pad-script" && has_next) {
            o.pad_script = argv[++i];
        } else if (arg == "--talk-to" && has_next) {
            o.talk_to.push_back(argv[++i]);
        } else if (arg == "--vendor-keys" && has_next) {
            o.vendor_keys.push_back(argv[++i]);
        } else if (arg == "--inventory-keys" && has_next) {
            o.inventory_keys.push_back(argv[++i]);
        } else if (arg == "--fishing-keys" && has_next) {
            o.fishing_keys.push_back(argv[++i]);
        } else if (arg == "--travel" && has_next) {
            o.travels.push_back(argv[++i]);
            o.play = true;
        } else if (arg == "--portal" && has_next) {
            o.portal_steps.push_back(argv[++i]);
            o.play = true;
        } else if (arg == "--die" && has_next) {
            o.deaths.push_back(argv[++i]);
            o.play = true;
        } else if (arg == "--hardcore") {
            o.hardcore = true;
            o.difficulty_explicit = true;
        } else if (arg == "--difficulty-dump" && has_next) {
            o.difficulty_dump = argv[++i];
            o.play = true;
        } else if (arg == "--load-character-dump" && has_next) {
            o.load_character_dump = argv[++i];
        } else if (arg == "--load-character-keys" && has_next) {
            o.load_character_keys = argv[++i];
        } else if (arg == "--save-file" && has_next) {
            o.save_file = argv[++i];
        } else if (arg == "--hud-wait" && has_next) {
            o.hud_wait = static_cast<float>(std::atof(argv[++i]));
        } else if (arg == "--menu-from-save" && has_next) {
            o.menu_from_save = argv[++i];
        } else if (arg == "--menu-dungeon" && has_next) {
            o.menu_dungeon = argv[++i];
        } else if (arg == "--continue-file" && has_next) {
            o.continue_file = argv[++i];
            o.play = true;
        } else if (arg == "--class" && has_next) {
            o.player_class = argv[++i];
            o.class_explicit = true;
        } else if (arg == "--intro-dump" && has_next) {
            o.intro_dump = argv[++i];
            o.play = true;
        } else if (arg == "--cinematic" && has_next) {
            o.cinematic = argv[++i];
        } else if (arg == "--intro") {
            o.intro = true;
        } else if (arg == "--no-intro") {
            o.no_intro = true;
        } else if (arg == "--create-dump" && has_next) {
            o.create_dump = argv[++i];
            o.play = true;
        } else if (arg == "--create-cycle" && has_next) {
            const std::string list = argv[++i];
            size_t at = 0;
            while (at <= list.size()) {
                const size_t comma = list.find(',', at);
                const std::string one =
                    list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                if (!one.empty()) o.create_cycle.push_back(one);
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
            o.play = true;
        } else if (arg == "--walk-to" && has_next) {
            o.walk_to.push_back(argv[++i]);
            o.play = true;
        } else if (arg == "--walk-dump" && has_next) {
            o.walk_dump = argv[++i];
        } else if (arg == "--dump-at" && has_next) {
            // "<seconds>,<png>". A comma rather than a colon because a Windows
            // path starts with one.
            const std::string spec = argv[++i];
            const size_t comma = spec.find(',');
            if (comma == std::string::npos) {
                std::fprintf(stderr, "--dump-at wants \"<seconds>,<png>\", got '%s'\n",
                             spec.c_str());
                o.help = true;
            } else {
                o.dump_at.emplace_back(static_cast<float>(std::atof(spec.substr(0, comma).c_str())),
                                       spec.substr(comma + 1));
            }
        } else if (arg == "--at" && has_next) {
            o.at = argv[++i];
            o.play = true;
        } else if (arg == "--no-particles") {
            o.no_particles = true;
        } else if (arg == "--pu-smoke") {
            o.pu_smoke = true;
        } else if (arg == "--pu-particles" && has_next) {
            // The first use replaces the default; "all" is every layout, "none"
            // no layout, anything else an archive-path prefix.
            if (!o.pu_particle_layouts_given) o.pu_particle_layouts.clear();
            o.pu_particle_layouts_given = true;
            std::string value = argv[++i];
            for (char& c : value) {
                c = c == '\\' ? '/'
                              : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            if (value == "ALL") {
                o.pu_particle_layouts.push_back("*");
            } else if (value != "NONE") {
                o.pu_particle_layouts.push_back(value);
            }
        } else if (arg == "--no-torchlight") {
            o.no_torchlight = true;
        } else if (arg == "--no-shadows") {
            o.no_shadows = true;
        } else if (arg == "--light-map-dump") {
            o.light_map_dump = true;
        } else if (arg == "--draw-dump" && has_next) {
            o.draw_dump = argv[++i];
        } else if (arg == "--dump-gamma") {
            o.dump_gamma = true;
        } else if (arg == "--headless") {
            o.headless = true;
        } else if (arg == "--help" || arg == "-h") {
            o.help = true;
        }
    }

    // Start at the main menu unless a scene or a picture was asked for (`--no-menu`
    // says so outright). A picture of a front-end screen asks for the menu, even
    // `--headless`.
    const bool asked_for_something_else =
        o.layout_explicit || !o.dungeon.empty() || o.mesh_explicit || o.play || o.headless ||
        !o.dump_frame.empty() || !o.walk_dump.empty() || !o.dump_at.empty() || !o.visits.empty() ||
        !o.walk_to.empty() || !o.talk_to.empty() || !o.fights.empty() || o.attack ||
        !o.hud_dump.empty() || !o.drop_dump.empty() || !o.inventory_dump.empty() ||
        !o.stats_dump.empty() || !o.skills_dump.empty() || !o.pet_dump.empty() ||
        !o.quest_dump.empty() || !o.quest_offers.empty() || o.walk_seconds > 0.0f ||
        !o.at.empty() || !o.intro_dump.empty() || o.intro;
    if (o.no_menu) {
        o.start_in_menu = false;
    } else if (!asked_for_something_else || !o.menu_dump.empty() || !o.wardrobe_dump.empty() ||
               !o.bestiary_dump.empty() || !o.level_list_dump.empty() || o.auto_new_game ||
               !o.create_dump.empty() || !o.create_cycle.empty() || !o.difficulty_dump.empty() ||
               !o.load_character_dump.empty()) {
        o.start_in_menu = true;
    }
    // A picture of the character creation screen has to get past the main menu
    // to reach it, and a headless run has nobody to press New Character.
    if (!o.create_dump.empty() || !o.create_cycle.empty() || !o.difficulty_dump.empty()) {
        o.auto_new_game = true;
    }

    // New Game leads into the town, playing; set here so what follows from `--play`
    // (run animation, outfit, no turntable) is decided in one place.
    if (o.start_in_menu) {
        o.layout = kMenuLayout;
        o.play = true;
    }

    // By default link the idle animation and play it; anything named wins.
    if (!o.animation_explicit) {
        o.link_animations.assign(1, kDefaultAnimation);
        o.animation = kDefaultAnimation;
    }

    // A controlled character needs its moving pose too: `Run.SKELETON`, the only
    // one a player has (a half-pushed stick changes speed, not gait; tl::core::Player).
    if (o.play) {
        bool linked = false;
        for (const std::string& name : o.link_animations) {
            if (name == kMovingAnimation) linked = true;
        }
        if (!linked) o.link_animations.push_back(kMovingAnimation);

        // A character the player is steering must not also be on a turntable.
        o.turntable = 0.0f;

        // Without a scene there is no ground and nothing to walk into.
        if (o.layout.empty()) o.layout = kTownLayout;
    }
    if (o.start_in_menu) {
        // The menu's character stands still and idles until New Game.
        o.turntable = 0.0f;
    }
    if (!o.animation_time_explicit && o.animation_explicit) {
        // An animation asked for by name starts at its beginning; only the
        // default backdrop pose is parked partway in.
        o.animation_time = 0.0f;
    }
    if (!o.equipment_explicit) {
        // A played, menu or named-class character is newly created: the starting
        // outfit, textures on the base body (Renderer::wear_base_outfit()). A bare
        // `--mesh` wears the default plate, which is one class's wardrobe piece.
        if (o.play || o.class_explicit || o.layout == kMenuLayout) {
            o.base_outfit = true;
        } else {
            o.equipment.assign(1, kDefaultEquipment);
        }
    }
    return o;
}

// `config.txt` (core/run_config.h), read before anything else happens and
// applied to the options. A device makes the file with everything off, beside
// its game files; with `debug` off it writes none of the frame dumps it
// otherwise takes on every start. The desktop makes none and keeps its tools,
// which the verification runs use, but reads one in its working directory.
// `fps` stands where no `--fps` / `--no-fps` was given.
Options with_run_config(Options o) {
#if defined(TL_DEVICE_BUILD)
    std::string folder = o.game_data;
    if (!folder.empty() && folder.back() != '/') folder += '/';
    tl::core::RunConfig device_defaults;
#if defined(TL_PLATFORM_ANDROID)
    // A phone's display runs at up to 120 Hz; the game is held to 60 unless the file says.
    device_defaults.fps_limit = 60;
#endif
    const tl::core::RunConfig config =
        tl::core::load_run_config(folder + "config.txt", device_defaults, /*create=*/true);
    if (!config.debug) {
        o.menu_dump.clear();
        o.dump_frame.clear();
        o.hud_dump.clear();
    }
    if (o.fps_overlay == -1) o.fps_overlay = config.fps ? 1 : 0;
    if (o.fps_limit == -1) o.fps_limit = static_cast<int>(config.fps_limit);
#else
    const bool player = player_folder();
    std::FILE* present = std::fopen("config.txt", "rb");
    if (present != nullptr) std::fclose(present);
    // A player's folder writes the window's size into a new file, so it is there to change.
    const tl::core::RunConfig defaults =
        player ? tl::core::RunConfig{false, false, false, o.width, o.height, false}
               : tl::core::RunConfig{true, true, false, 0, 0, false};
    const tl::core::RunConfig config =
        tl::core::load_run_config("config.txt", defaults, /*create=*/player);
    if (player || present != nullptr) {
        if (o.fps_overlay == -1) o.fps_overlay = config.fps ? 1 : 0;
        if (!o.size_given && config.width > 0 && config.height > 0) {
            o.width = config.width;
            o.height = config.height;
        }
        o.fullscreen = config.fullscreen;
        o.vsync = config.vsync;
        if (o.fps_limit == -1) o.fps_limit = static_cast<int>(config.fps_limit);
    }
#endif
    return o;
}

// The log: torchlight-<tag>.log on the Switch for a build tagged by TL_RUN_TAG,
// so it does not overwrite the ordinary build's. With `debug` and `logs` both
// off (config.txt) nothing is opened, and nothing is even formatted.
void open_log() {
    if (!tl::core::run_config().debug && !tl::core::run_config().logs) {
        tl::core::log_set_min_level(tl::core::LogLevel::Off);
        return;
    }
#if defined(TL_PLATFORM_SWITCH)
    tl::core::log_init(tl::core::platform_data_root() + (sizeof(TL_RUN_TAG) > 1
                                                             ? "torchlight-" TL_RUN_TAG ".log"
                                                             : "torchlight.log"));
#else
    tl::core::log_init(tl::core::platform_data_root() + "torchlight.log");
#endif
}

#if defined(TL_PLATFORM_SWITCH)
constexpr const char* kBuildName = "switch";
#elif defined(TL_PLATFORM_ANDROID)
constexpr const char* kBuildName = "android";
#elif defined(TL_PLATFORM_IOS)
constexpr const char* kBuildName = "ios";
#else
constexpr const char* kBuildName = "desktop";
#endif

// `--pad-script`, `--touch-pad`, `--cpu-skinning` and `--workers`, before anything is drawn.
void apply_input_options(const Options& opts) {
    if (opts.workers >= 0) tl::core::set_job_workers(static_cast<unsigned>(opts.workers));
    if (!opts.pad_script.empty()) tl::core::set_pad_script(opts.pad_script);
    if (opts.touch_pad) {
        tl::core::touch_pad().set_screen(static_cast<float>(opts.width),
                                         static_cast<float>(opts.height));
        tl::core::touch_pad().set_visible(true);
    }
#if defined(TL_HAVE_OGRE)
    if (opts.cpu_skinning) tl::render::GpuSkinning::set_enabled(false);
#endif
}

// What a device needs before anything loads, said to the player in the
// platform's own way (platform_error_message) rather than as a failure deep in
// the load: the pak, which nothing ships but the player's own copy; OGRE's
// shader library, inside the .nro (the APK unpacks its own); and `music/`,
// without which the game runs and plays none. False stops the run.
bool device_files_present(const Options& opts) {
    std::string data = opts.game_data;
    if (!data.empty() && data.back() != '/') data += '/';
    if (!is_regular_file(data + "pak.zip") && !is_regular_file(data + "Pak.zip")) {
        tl::core::platform_error_message(
            "The game's pak.zip is missing.",
            "Put your own copy of the Xbox 360 release's pak.zip here:\n\n  " + data +
                "pak.zip\n\nwith its music/ folder beside it:\n\n  " + data +
                "music/\n\nThe game's files are not included with this program; they come from "
                "your copy of the game.");
        return false;
    }
    std::string media = opts.ogre_media.empty() ? std::string(TL_OGRE_MEDIA_DIR) : opts.ogre_media;
    if (!media.empty() && media.back() != '/') media += '/';
    if (!is_directory(media + "RTShaderLib")) {
        tl::core::platform_error_message(
            "The shader library could not be read.",
            "It comes with the program itself (" + media +
                "RTShaderLib). Copy the program to the device again; if it was copied whole, the "
                "copy on the device may be damaged.");
        return false;
    }
    if (!is_directory(data + "music")) {
        tl::core::platform_error_message(
            "The music/ folder is missing: the game will play without music.",
            "The level themes are not inside pak.zip. Copy the music/ folder from your copy of "
            "the game beside it:\n\n  " +
                data + "music/");
    }
    return true;
}

// What the run needs before OGRE: the pad's switches, the files a device (or
// a player's folder) cannot run without, and the index of the game's assets.
// False ends the run.
bool prepare_run(const Options& opts, tl::core::AssetIndex& assets) {
    apply_input_options(opts);
#if defined(TL_DEVICE_BUILD)
    const bool check_files = true;
#else
    const bool check_files = player_folder();
#endif
    if (check_files && !device_files_present(opts)) return false;
    if (assets.build(opts.game_data)) return true;
    TL_LOG_ERROR("no readable game data at '%s'", opts.game_data.c_str());
    TL_LOG_ERROR("supply your own Torchlight install with --game-data <path>");
    return false;
}

#if defined(TL_HAVE_OGRE)
// The window: its size, and on the desktop whether it fills the screen (config.txt).
void apply_window_options(const Options& opts, tl::render::RendererConfig& rc) {
    rc.width = opts.width;
    rc.height = opts.height;
    rc.fullscreen = opts.fullscreen;
    rc.vsync = opts.vsync;
    rc.fps_limit = opts.fps_limit > 0 ? static_cast<unsigned>(opts.fps_limit) : 0;
}
#endif

}  // namespace

/// How long a `--walk-to` leg may take before it is called a failure, in seconds
/// (corner to corner of the town is about 20).
constexpr float kWalkRouteTimeout = 60.0f;

/// An uncaught exception (an `Ogre::Exception` names what it could not find) is
/// logged and the log flushed: it writes on its own thread, and would lose its tail.
[[noreturn]] static void log_uncaught_exception() {
    if (std::exception_ptr current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& e) {
            TL_LOG_ERROR("uncaught exception: %s", e.what());
        } catch (...) {
            TL_LOG_ERROR("uncaught exception of an unknown type");
        }
    } else {
        TL_LOG_ERROR("std::terminate with no exception in flight");
    }
    tl::core::log_shutdown();
    std::abort();
}

#if defined(TL_PLATFORM_ANDROID)
// Called by android_main(), which turns the activity and its args.txt into a
// command line; a shared library has no main().
#define main tl_main
#endif
#if defined(TL_PLATFORM_IOS)
#define main tl_engine_main
#endif
int main(int argc, char** argv) {
    const Options opts = with_run_config(parse_args(argc, argv));

    if (opts.help) {
        print_usage();
        return 0;
    }

    open_log();
    std::set_terminate(log_uncaught_exception);
    TL_LOG_INFO("torchlight starting (%s build)", kBuildName);
    tl::core::start_profiler();
    tl::core::start_gpu_waits();
    // Everything before the first frame is loading; the renderer's first frame
    // switches the boost off again (Renderer::Impl::FrameClock).
    tl::core::set_loading_boost(true);
    TL_LOG_INFO("game data: %s", opts.game_data.c_str());
    tl::core::AssetIndex assets;
    if (!prepare_run(opts, assets)) {
        tl::core::log_shutdown();
        return 1;
    }

    TL_LOG_INFO("indexed %zu assets", assets.size());
    if (!assets.ambiguous_basenames().empty()) {
        // The same filename in several directories: intra-asset references go
        // through AssetIndex::resolve_near() with the referring asset's path.
        TL_LOG_INFO("%zu ambiguous basenames; use resolve_near() for asset refs",
                    assets.ambiguous_basenames().size());
    }

#if defined(TL_HAVE_OGRE)
    // The levels `CPlayer` remembers, its dungeon records and play time. Here, not
    // in the renderer, which is rebuilt on every level change.
    tl::core::TravelRecord travel;
    // `CDifficultyMenu`, built once by `CMenuManager::create`: the
    // focus it was left on is where it opens next.
    tl::core::DifficultyMenu difficulty_menu;
    // `CSharedStash` (`CMasterResourceManager +0x178`), one for the process.
    tl::core::SharedStash shared_stash;

    // The character's save file (`CGameUI+0x16c0`), empty until one is made.
    std::string save_path;
    make_save_directory();
    if (!opts.save_file.empty()) save_path = opts.save_file;

    // `CGame::setup`: the settings' audio levels,
    // kept by the mixer until it starts.
    tl::core::GameSettings game_settings;
    const std::string settings_path = save_directory() + "settings.txt";
    load_game_settings(settings_path, game_settings);
    {
        const tl::core::AudioGains gains = tl::core::audio_gains(
            tl::core::AudioLevels{game_settings.sound_volume, game_settings.music_volume,
                                  game_settings.sound_mute, game_settings.music_mute});
        tl::core::audio_set_levels(gains.effects, gains.music);
    }

    tl::render::RendererConfig rc;
    rc.travel = &travel;
    rc.settings = &game_settings;
    rc.settings_path = settings_path;
    rc.shared_stash = &shared_stash;
    rc.save_path = &save_path;
    rc.hardcore = opts.hardcore;
    rc.game_data = opts.game_data;
    rc.pak = opts.pak;
    rc.ogre_media = opts.ogre_media;
    rc.shader_cache = tl::core::platform_data_root() + "shadercache.bin";
    // Outlives the renderer; for the files read off the filesystem (the music).
    rc.assets = &assets;
#if defined(TL_DEVICE_BUILD)
    // A device always has a framebuffer, so a frame dump does not imply headless.
    rc.headless = false;
    rc.difficulty = opts.difficulty;
#else
    rc.headless = opts.headless || !opts.dump_frame.empty() || !opts.walk_dump.empty() ||
                  !opts.dump_at.empty() || !opts.visits.empty() || !opts.walk_to.empty() ||
                  !opts.talk_to.empty() || !opts.hud_dump.empty() || !opts.drop_dump.empty() ||
                  !opts.inventory_dump.empty() || !opts.skills_dump.empty() ||
                  !opts.pet_dump.empty() || !opts.intro_dump.empty() || !opts.quest_dump.empty() ||
                  !opts.quest_offers.empty();
#endif
    rc.pad = opts.pad;
    apply_window_options(opts, rc);
    rc.turntable_degrees_per_second = opts.turntable;
    rc.no_particles = opts.no_particles;
    rc.no_torchlight = opts.no_torchlight;
    rc.no_shadows = opts.no_shadows;
    rc.light_map_dump = opts.light_map_dump;
    rc.draw_dump = opts.draw_dump;
    rc.dump_gamma = opts.dump_gamma;
    rc.pu_particle_layouts = opts.pu_particle_layouts;
    rc.fov_degrees = opts.fov;
    rc.extra_data = opts.extra_data;
    rc.console_ui = console_interface(opts.console_ui, opts.extra_data);
    rc.mod_data = opts.mod_data;
    rc.game_hud = !opts.no_hud;
    rc.minimap = !opts.no_minimap;
    rc.stats_overlay = opts.fps_overlay == 1 || (opts.fps_overlay == -1 && !rc.headless);
    // A headless run is a check: a tip opening over it would take the pad from
    // its script and sit in its frames.
    rc.context_tips = opts.tips == 1 || (opts.tips == -1 && !rc.headless);
    rc.fullscreen_map = opts.fullscreen_map;
    rc.map_zoom = opts.map_zoom;
    rc.show_inventory = opts.show_inventory;
    rc.extra_levels = opts.level_ups;
    rc.extra_stat_points = opts.stat_points;
    rc.extra_skill_points = opts.skill_points;
    rc.skill_set = opts.skill_set;
    rc.foremen_kills = opts.foremen_kills;
    rc.pet = opts.pet;
    rc.force_audio = opts.force_audio;
    rc.mute = opts.mute;
    rc.bench_seconds = opts.bench_seconds;
    rc.invulnerable = opts.invulnerable;
    rc.starting_fish = !opts.no_starting_fish;

    tl::render::Renderer renderer;
    if (!opts.logic_trace.empty()) renderer.set_logic_trace(opts.logic_trace);
    if (!renderer.init(rc)) {
        TL_LOG_ERROR("renderer initialisation failed");
        tl::core::log_shutdown();
        return 1;
    }
    if (!opts.passability_dump.empty()) renderer.set_passability_dump(opts.passability_dump);

    // What the subject is built from: copies of `opts`, because the creation screen
    // decides two of them at runtime. The character is built twice when the game
    // starts at its menu (backdrop, then town), hence setup_subject.
    std::string subject_mesh = opts.mesh;
    std::vector<std::string> subject_equipment = opts.equipment;
    std::string chosen_class = opts.player_class;

    /// Point the subject at a class: its mesh and the name that identifies it from
    /// here on (the name, not the mesh's stem: the Destroyer's is `player.MESH`).
    auto apply_class = [&]() {
        if (chosen_class.empty()) return;
        for (const tl::core::PlayerClass& entry : renderer.player_classes()) {
            if (entry.name != chosen_class) continue;
            subject_mesh = entry.mesh;
            break;
        }
        // The armour is not touched: the starting outfit is `base_outfit`.
        renderer.set_player_class(chosen_class);
    };

    auto setup_subject = [&]() -> bool {
        if (!renderer.show_mesh(subject_mesh)) return false;

        // Before any wardrobe piece: a worn set hides the body slots this retextures.
        if (opts.base_outfit) renderer.wear_base_outfit();

        for (const std::string& piece : subject_equipment) {
            if (!renderer.equip(piece)) {
                if (opts.equipment_explicit) return false;
                // The default wardrobe is the Alchemist's; another mesh goes bare.
                TL_LOG_WARN("could not equip '%s'; showing the bare mesh", piece.c_str());
            }
        }

        bool animated = true;
        for (const std::string& name : opts.link_animations) {
            if (!renderer.link_animation(name)) {
                if (opts.animation_explicit) return false;
                // A default animation another mesh lacks: it stands in its bind pose.
                TL_LOG_WARN("no '%s' for this mesh; showing the bind pose", name.c_str());
                animated = false;
            }
        }

        if (animated && !opts.animation.empty() &&
            !renderer.apply_animation(opts.animation, opts.animation_time) &&
            opts.animation_explicit) {
            return false;
        }
        return true;
    };

    // Loading a scene. A device build, which cannot retry, falls back to the small
    // menu backdrop; on the desktop a failure is a failure.
    auto load_scene = [&](const std::string& layout) -> bool {
        if (layout.empty()) return true;
        if (renderer.load_layout(layout)) return true;
        TL_LOG_ERROR("could not load layout '%s'", layout.c_str());
#if defined(TL_DEVICE_BUILD)
        if (layout != kMenuLayout) {
            TL_LOG_WARN(
                "falling back to the main-menu backdrop so the run still "
                "produces something to look at");
            if (renderer.load_layout(kMenuLayout)) return true;
            TL_LOG_ERROR("the fallback layout failed too");
        }
#endif
        return false;
    };

    // `CGameClient::performWarp`: the level left is
    // remembered and the same `CPlayer` carried across. `portal_warp` is the -99
    // branch, entry type 2; `back_to_portal` its way out of a town;
    // `waypoint` is `warpLevels`' `WAYPOINT`, entry type 6.
    auto warp = [&](bool to_town, const std::string& to_dungeon, int to_floor,
                    bool portal_warp = false, bool back_to_portal = false, bool waypoint = false,
                    int level_delta = 0, int level_absolute = 0) -> bool {
        const std::string destination = to_town ? "TOWN" : to_dungeon;
        // What the level is loaded with.
        const int entry_type = tl::core::warp_entry_type(
            level_delta, level_absolute, portal_warp, false, waypoint,
            tl::core::upper_ascii(renderer.level_dungeon()), tl::core::upper_ascii(destination));
        const int destination_depth = to_town ? 0 : to_floor - 1;
        // `saveCharacter(false, true)` before anything is unloaded.
        if (!save_path.empty()) renderer.save_character(save_path);
        renderer.leave_level(destination, destination_depth, portal_warp);
        const std::string previous_dungeon = renderer.level_dungeon();
        const int previous_depth = renderer.level_depth();
        const tl::render::CarriedPlayer carried = renderer.carry_player();

        if (!renderer.restart(rc)) return false;
        apply_class();
        if (!setup_subject()) return false;
        // Quests before the layout: a level's gates read them while it is placed.
        renderer.restore_player_before_level(carried);

        bool loaded = false;
        if (to_town) {
            TL_LOG_INFO("dungeon: returning to town");
            loaded = renderer.load_layout(kTownLayout);
        } else {
            renderer.set_level_entry_type(entry_type);
            loaded = renderer.load_dungeon(to_dungeon, to_floor);
            // A stratum the generator fails to lay out should not end the run
            // with nothing on screen.
            if (!loaded) {
                TL_LOG_ERROR("could not generate dungeon '%s' floor %d", to_dungeon.c_str(),
                             to_floor);
                loaded = renderer.load_layout(kTownLayout);
            }
        }
        if (!loaded || !renderer.enable_player_control()) return false;
        renderer.restore_player_after_control(carried);
        // `CLevel::placePlayerAtWarpToDungeonFloor`.
        // Entry type 2 skips it: a town's `Town Portal`, else the level's start, or
        // back through the portal, beside it.
        if (back_to_portal) {
            renderer.arrive_back_at_portal();
        } else if (portal_warp) {
            renderer.place_player_at_town_portal();
        } else if (entry_type == tl::core::kEntryWaypoint) {
            // Entry type 6: the last `Waypoint` / `Waypoint Start` node, else the
            // level's start; no Warper placement (`performWarp`).
            renderer.place_player_at_waypoint();
        } else {
            renderer.place_player_at_warp(previous_dungeon, previous_depth, std::string());
        }
        // Entry type 5 with a dungeon left:
        // back out of a map scroll's dungeon the player stands by its portal,
        // which closes.
        if (entry_type == tl::core::kEntryOtherDungeon) {
            renderer.arrive_from_temporary_portal(previous_dungeon);
        }
        // `deleteOpenPortals` and `createPortals`, except on the way back to one.
        if (!back_to_portal) renderer.create_portals();
        renderer.bring_player_pets(carried);
        // the level's `CINEMATIC`, on the deepest arrival so far at a
        // level that was not put back from its remembered state.
        const std::string cinematic = renderer.warp_cinematic();
        if (!cinematic.empty() && !opts.no_intro) renderer.run_cinematic(std::string(), cinematic);
        // ... and again once the player stands on the new level.
        if (!save_path.empty()) renderer.save_character(save_path);
        return true;
    };

    // `CPlayer::unitInit` for a new character: `CSharedStash::setPlayer`,
    // which reads the file only for a player of the other kind than the stash's.
    auto meet_shared_stash = [&]() {
        if (!shared_stash.set_player(rc.hardcore)) return;
        renderer.load_shared_stash(directory_of(save_path.empty() ? save_directory() : save_path));
    };

    // `CGameClient::setGameState(2)`: `loadCharacter`,
    // then the saved dungeon and depth with entry type 4 (the remembered level back).
    bool continue_at_start = false;
    auto continue_game = [&](const tl::formats::CharacterFile& file,
                             const std::string& path) -> bool {
        travel = tl::core::TravelRecord{};
        travel.levels = file.levels;
        travel.trackers = file.trackers;
        travel.play_time = file.player.clock;
        travel.deepest_depth = file.player.deepest_depth;
        // `+0x7e4` is journal slot 2, which `loadCharacter` sets with the
        // others (`setJournalStatistic`).
        travel.deepest_depth_max = static_cast<int32_t>(file.journal[2]);
        travel.last_dungeon = file.last_dungeon;
        travel.last_dungeon_depth = file.last_dungeon_depth;
        travel.portal_open = file.portal_open != 0;
        travel.portal_position = file.portal_position;
        travel.portal_dungeon = file.portal_dungeon;
        travel.portal_depth = file.portal_depth;
        save_path = path;
        chosen_class = file.class_name;
        rc.difficulty = static_cast<int>(file.difficulty);
        rc.hardcore = file.hardcore != 0;

        if (!renderer.restart(rc)) return false;
        renderer.load_character_seeds(file);
        apply_class();
        if (!setup_subject()) return false;
        const tl::render::CarriedPlayer carried = renderer.carried_from_file(file);
        load_character_shared_stash(renderer, shared_stash, file.hardcore != 0, path);
        renderer.restore_player_before_level(carried);
        bool loaded = false;
        if (file.current_dungeon.empty() || file.current_dungeon == "TOWN") {
            loaded = renderer.load_layout(kTownLayout);
        } else {
            loaded = renderer.load_dungeon(file.current_dungeon, file.player.depth + 1);
            if (!loaded) loaded = renderer.load_layout(kTownLayout);
        }
        if (!loaded || !renderer.enable_player_control()) return false;
        renderer.restore_player_after_control(carried);
        // `CGameClient +0x146c`, which Load Character sets for an A pressed
        // with both bumpers held (`CContinueGameMenu`'s pad handler):
        // once the level is in, the player and the pet are put
        // on its start point instead of where they were saved.
        if (continue_at_start) {
            continue_at_start = false;
            TL_LOG_INFO("continue: on the level's start, as asked with both bumpers");
        } else {
            renderer.place_player_at(file.player.position.x, file.player.position.z);
        }
        renderer.bring_player_pets(carried);
        // `setGameState`: `deleteOpenPortals` and `createPortals` on the level.
        renderer.create_portals();
        TL_LOG_INFO("continue: %s from %s", file.player.name.c_str(), path.c_str());
        return true;
    };

    // The loading screen, on its own, before anything else has happened.
    if (!opts.loading_dump.empty()) {
        const bool ok = renderer.dump_loading_screen(opts.loading_dump);
        renderer.shutdown();
        tl::core::log_shutdown();
        return ok ? 0 : 1;
    }

    // The title's character, when no `--class` names one: the newest save's, as
    // the backdrop behind it is (`menu_backdrop`), and with none the game
    // client's default class, `Destroyer` (`CGameClient::CGameClient`,
    // `+0x174`, `+0x1a8`). Its pet is the default
    // `Dog` (`+0x190`) or the save's.
    tl::formats::CharacterFile title_save;
    bool title_save_read = false;
    if (opts.start_in_menu && chosen_class.empty()) {
        const std::string newest = menu_backdrop_file(opts.menu_from_save);
        title_save_read = !newest.empty() &&
                          tl::core::read_character_with_backup(newest, title_save);
        chosen_class = title_save_read ? title_save.class_name : std::string("Destroyer");
    }

    // `--class` on the command line, before anything is built.
    apply_class();
    if (!setup_subject()) {
        tl::core::log_shutdown();
        return 1;
    }

    // Before the layout, because a level's quest gates are evaluated while it is
    // being placed and a subtree that was gated shut is never built at all.
    renderer.give_starting_quests();
    for (const std::string& quest : opts.quests) {
        if (!renderer.set_quest(quest)) {
            tl::core::log_shutdown();
            return 1;
        }
    }

    // The scene the run ends up in: `opts.layout`, or with the front end the menu
    // backdrop, replaced by the town on New Game or by `dungeon` from Choose Level.
    std::string scene = opts.layout;
    // The title's backdrop: a save's (`--menu-from-save`, else the newest), or
    // `--menu-dungeon NAME:DEPTH`'s as `loadMenuLevel` picks it for that depth.
    const auto title_backdrop = [&]() -> std::string {
        if (!opts.menu_dungeon.empty()) {
            const size_t colon = opts.menu_dungeon.find(':');
            const std::string backdrop = renderer.menu_layout_for(
                opts.menu_dungeon.substr(0, colon),
                colon == std::string::npos ? 0 : std::atoi(opts.menu_dungeon.c_str() + colon + 1));
            if (!backdrop.empty()) return backdrop;
        }
        return menu_backdrop(renderer, opts.menu_from_save, shared_stash);
    };
    if (scene == kMenuLayout && (!opts.menu_from_save.empty() || !opts.menu_dungeon.empty())) {
        scene = title_backdrop();
    }
    std::string dungeon = opts.dungeon;
    int dungeon_floor = opts.dungeon_floor;
    bool created_new_char = false;

    // Load Character (`CContinueGameMenu`); its state outlives every renderer.
    // `Continue` fills `chosen` and `chosen_path`. `selectCharacter`
    // hands the save to `applyCharacterState`, which remakes the
    // player on the menu's level; here the renderer restarts on the save's class and
    // `Renderer::apply_character_state` applies it.
    tl::core::ContinueMenu continue_menu;
    auto load_character = [&](tl::formats::CharacterFile& chosen, std::string& chosen_path,
                              const std::string& backdrop_file, std::string dump_png,
                              std::string keys) -> tl::render::MenuAction {
        tl::render::LoadCharacterHost host;
        host.list = list_continue_entries;
        host.remove = [](const std::string& path) { std::remove(path.c_str()); };
        const tl::core::ContinuePress opened = continue_menu.open(host.list(), {});
        // Whose character stands behind the menu: the newest save's, as the
        // menu's level is, until another is applied.
        std::string shown_file = backdrop_file;
        bool behind = opened.apply_character && continue_menu.current_file() != shown_file;
        for (;;) {
            if (behind) {
                shown_file = continue_menu.current_file();
                tl::formats::CharacterFile file;
                const bool read = tl::core::read_character_with_backup(shown_file, file);
                if (read) chosen_class = file.class_name;
                if (!renderer.restart(rc)) return tl::render::MenuAction::Quit;
                apply_class();
                if (!setup_subject()) return tl::render::MenuAction::Quit;
                if (!renderer.load_layout(menu_backdrop(renderer, backdrop_file, shared_stash))) {
                    return tl::render::MenuAction::Quit;
                }
                renderer.dress_character();
                if (read) {
                    renderer.apply_character_state(file);
                } else {
                    renderer.place_menu_pet();
                }
            }
            tl::core::ContinuePress last;
            const tl::render::MenuAction action =
                renderer.run_load_character(continue_menu, host, shown_file, last, dump_png, keys);
            keys.clear();
            if (action == tl::render::MenuAction::LoadCharacter) {
                behind = true;
                continue;
            }
            if (action == tl::render::MenuAction::Continue) {
                const std::string path = continue_menu.current_file();
                tl::formats::CharacterFile file;
                if (!tl::core::read_character_with_backup(path, file)) {
                    TL_LOG_WARN("load character: '%s' could not be read", path.c_str());
                    return tl::render::MenuAction::Back;
                }
                chosen = std::move(file);
                chosen_path = path;
                continue_at_start = last.at_level_start;
            }
            return action;
        }
    };
    // The difficulty screen, New Game's last step (`CMenuManager::setActiveMenu`:
    // menu 4, or 5 after a retirement). A pick stores the difficulty
    // and Hardcore (`CDifficultyMenu::onClick`); after a
    // retirement it starts state 4.
    auto choose_difficulty = [&](bool retirement, const std::string& dump_png) {
        if (opts.difficulty_explicit) return tl::render::MenuAction::NewGame;
        difficulty_menu.retirement = retirement;
        tl::core::DifficultyClick click;
        const tl::render::MenuAction action =
            renderer.run_difficulty_menu(difficulty_menu, click, dump_png);
        if (action == tl::render::MenuAction::NewGame && click.chosen) {
            rc.difficulty = click.difficulty;
            rc.hardcore = click.hardcore;
        }
        return action;
    };
    // The front end: the next screen is `setActiveMenu`'s table, asked for as
    // `CGameUI::requestSetGameState(state, menu)` by `CMainMenu::onClick`,
    // `CNewGameMenu::onClick`,
    // `handle_SubmitPet`, `CDifficultyMenu::onClick` and
    // `CContinueGameMenu::onClick`. Leaving for the game
    // closes them (`closeMenus`). Choose Level and `--class` are the port's own.
    struct FrontEnd {
        int32_t first = tl::core::kMenuMain;
        std::string main_dump;
        bool auto_new_game = false;
        std::string create_dump;
        std::vector<std::string> create_cycle;
        std::string difficulty_dump;
        /// `--class` without a picture of the creation screen: no creation
        /// screen, and no difficulty screen unless its picture was asked for.
        bool skip_create = false;
    };
    tl::core::FrontEndMenus front_end_menus;
    auto run_front_end = [&](const FrontEnd& how, tl::formats::CharacterFile& saved,
                             std::string& saved_path, std::string& pet) -> tl::render::MenuAction {
        const std::string backdrop_file = menu_backdrop_file(opts.menu_from_save);
        // A copy: `run_character_create()` clears its `chosen` output on entry,
        // which would wipe the same string passed as `preselect`.
        const std::string wanted_class = chosen_class;
        std::string main_dump = how.main_dump;
        bool auto_new_game = how.auto_new_game;
        const auto leave = [&](tl::render::MenuAction action) {
            tl::core::close_menus(front_end_menus);
            return action;
        };
        int32_t next = how.first;
        for (;;) {
            const tl::core::MenuSwitch change =
                tl::core::set_active_menu(front_end_menus, next, continue_menu.can_continue());
            if (change.select_first_character) {
                tl::core::ContinuePress selected;
                continue_menu.select_character(0, true, {}, selected);
            }
            switch (tl::core::open_menu(front_end_menus)) {
                case tl::core::kMenuMain: {
                    renderer.set_continue_available(newest_living_save(saved, saved_path));
                    const tl::render::MenuAction choice =
                        renderer.run_main_menu(main_dump, auto_new_game);
                    main_dump.clear();
                    auto_new_game = false;
                    if (choice == tl::render::MenuAction::LoadCharacter) {
                        next = tl::core::kMenuLoadCharacter;
                    } else if (choice == tl::render::MenuAction::NewGame && !how.skip_create) {
                        next = tl::core::kMenuNewGame;
                    } else {
                        if (choice == tl::render::MenuAction::NewGame &&
                            !how.difficulty_dump.empty()) {
                            choose_difficulty(/*retirement=*/false, how.difficulty_dump);
                        }
                        return leave(choice);
                    }
                    break;
                }
                case tl::core::kMenuLoadCharacter: {
                    const tl::render::MenuAction picked = load_character(
                        saved, saved_path, backdrop_file, std::string(), std::string());
                    if (picked != tl::render::MenuAction::Back) return leave(picked);
                    next = tl::core::kMenuMain;
                    break;
                }
                case tl::core::kMenuNewGame:
                case tl::core::kMenuNewGameRetirement: {
                    const bool retirement = front_end_menus.new_game_retirement;
                    const tl::render::MenuAction made = renderer.run_character_create(
                        how.create_dump, chosen_class, pet, wanted_class, how.create_cycle);
                    if (made == tl::render::MenuAction::NewGame) {
                        next = retirement ? tl::core::kMenuDifficultyRetirement
                                          : tl::core::kMenuDifficulty;
                    } else if (retirement) {
                        // No `Back` in the retirement mode: the screen again,
                        // unless the window is going.
                        if (!tl::core::should_keep_running() || !renderer.window_open()) {
                            return leave(tl::render::MenuAction::Quit);
                        }
                    } else {
                        next = tl::core::kMenuMain;
                    }
                    break;
                }
                case tl::core::kMenuDifficulty:
                case tl::core::kMenuDifficultyRetirement: {
                    const bool retirement = front_end_menus.difficulty_retirement;
                    const tl::render::MenuAction picked =
                        choose_difficulty(retirement, how.difficulty_dump);
                    if (picked == tl::render::MenuAction::NewGame) return leave(picked);
                    if (picked != tl::render::MenuAction::Back)
                        return leave(tl::render::MenuAction::Quit);
                    next = retirement ? tl::core::kMenuNewGameRetirement : tl::core::kMenuNewGame;
                    break;
                }
                default: return leave(tl::render::MenuAction::Quit);
            }
        }
    };
    // The title menu after the first (that one has its own one-shot dumps): carry out
    // the pick, back to the menu when it fails; false ends the run. After a retirement,
    // New Game in its retirement mode, without `Back` (`CNewGameMenu::setOpen`).
    auto enter_from_menu = [&](const tl::core::HeirloomHandOff* heirloom = nullptr) -> bool {
        tl::formats::CharacterFile saved;
        std::string saved_path;
        for (;;) {
            if (!renderer.restart(rc)) return false;
            apply_class();
            if (!setup_subject()) return false;
            if (!renderer.load_layout(title_backdrop())) {
                return false;
            }
            renderer.dress_character();
            renderer.place_menu_pet();
            // After a retirement, `requestSetGameState(0, 2)`: New Game in its
            // retirement mode, the title skipped.
            FrontEnd how;
            how.first =
                heirloom != nullptr ? tl::core::kMenuNewGameRetirement : tl::core::kMenuMain;
            std::string pet;
            const tl::render::MenuAction choice = run_front_end(how, saved, saved_path, pet);

            if (choice == tl::render::MenuAction::Continue) {
                if (continue_game(saved, saved_path)) return true;
                TL_LOG_WARN("could not continue from '%s'; back to the title menu",
                            saved_path.c_str());
                continue;
            }
            if (choice == tl::render::MenuAction::LoadDungeon) {
                dungeon = renderer.dungeon_choice();
                dungeon_floor = renderer.dungeon_choice_floor();
                scene.clear();
            } else if (choice == tl::render::MenuAction::NewGame) {
                dungeon.clear();
                scene = kTownLayout;
            } else {
                return false;
            }
            // A new character remembers nowhere, and gets a file of its own.
            travel = tl::core::TravelRecord{};
            save_path =
                choice == tl::render::MenuAction::NewGame ? empty_save_path() : std::string();
            const bool skip_create = choice == tl::render::MenuAction::LoadDungeon;
            if (!pet.empty()) rc.pet = pet;
            if (!skip_create) renderer.creation_names(rc.player_name, rc.pet_name);

            if (!renderer.restart(rc)) return false;
            renderer.begin_new_game();
            apply_class();
            if (!setup_subject()) return false;
            renderer.give_starting_quests();
            for (const std::string& quest : opts.quests) {
                if (!renderer.set_quest(quest)) return false;
            }

            bool loaded = false;
            if (!dungeon.empty()) {
                if (!opts.rng_trace.empty()) renderer.set_rng_trace(opts.rng_trace);
                loaded = renderer.load_dungeon(dungeon, dungeon_floor);
                if (!loaded) {
                    TL_LOG_ERROR("could not generate dungeon '%s' floor %d", dungeon.c_str(),
                                 dungeon_floor);
                    TL_LOG_WARN("back to the title menu instead");
                    continue;
                }
            } else {
                loaded = renderer.load_layout(scene);
            }
            if (!loaded) return false;
            if (!renderer.enable_player_control()) return false;
            if (heirloom != nullptr) renderer.receive_heirloom(*heirloom);
            renderer.create_portals();
            meet_shared_stash();
            // `setGameState`: `saveCharacter(false, false)` once the new character stands.
            if (!save_path.empty()) renderer.save_character(save_path);
            // The class introduction, as a launch's first New Game plays it
            // (after the level and control, below): a character made after
            // returning to the menu is just as new.
            if (!skip_create && !opts.no_intro && !rc.headless) {
                const std::string intro = renderer.intro_cinematic();
                if (!intro.empty() &&
                    renderer.run_cinematic(std::string(), intro) == tl::render::MenuAction::Quit) {
                    return false;
                }
            }
            return true;
        }
    };

    // Continue, taken from the first menu a launch shows.
    bool continued_from_save = false;
    if (opts.start_in_menu) {
        // The backdrop, the character on its `Player Start` and the pet on the
        // `Point of Interest` beside it, all out of the layout.
        if (!load_scene(title_backdrop())) {
            tl::core::log_shutdown();
            return 1;
        }
        renderer.dress_character();
        if (title_save_read) {
            renderer.apply_character_state(title_save);
        } else {
            renderer.place_menu_pet();
        }

        // `--wardrobe-dump`: a frame of the wardrobe editor, and stop.
        if (!opts.wardrobe_dump.empty()) {
            const bool ok = renderer.dump_wardrobe(opts.wardrobe_dump, opts.wardrobe_picks);
            renderer.shutdown();
            tl::core::log_shutdown();
            return ok ? 0 : 1;
        }
        // And for the bestiary.
        if (!opts.bestiary_dump.empty()) {
            const bool ok = renderer.dump_bestiary(opts.bestiary_dump, opts.bestiary_picks);
            renderer.shutdown();
            tl::core::log_shutdown();
            return ok ? 0 : 1;
        }
        // And for the Choose Level list.
        if (!opts.level_list_dump.empty()) {
            const bool ok = renderer.dump_level_list(opts.level_list_dump);
            renderer.shutdown();
            tl::core::log_shutdown();
            return ok ? 0 : 1;
        }

        // The menu itself takes the frame, once its overlay is up: a dump from
        // out here would photograph the backdrop with no interface on it.
        tl::formats::CharacterFile startup_save;
        std::string startup_save_path;
        // And for Load Character, with whatever buttons were asked for.
        if (!opts.load_character_dump.empty()) {
            const std::string backdrop_file = menu_backdrop_file(opts.menu_from_save);
            const tl::render::MenuAction got =
                load_character(startup_save, startup_save_path, backdrop_file,
                               opts.load_character_dump, opts.load_character_keys);
            TL_LOG_INFO("load character: the keys led to %s",
                        got == tl::render::MenuAction::Continue ? startup_save_path.c_str()
                        : got == tl::render::MenuAction::Back   ? "back"
                                                                : "quit");
            renderer.shutdown();
            tl::core::log_shutdown();
            return 0;
        }
        // New Game opens character creation over the same backdrop. `--class` skips
        // it, unless a picture of it was asked for: then it opens on that class.
        FrontEnd how;
        how.main_dump = opts.menu_dump;
        renderer.menu_dump_delay = opts.menu_dump_at;
        how.auto_new_game = opts.auto_new_game;
        how.create_dump = opts.create_dump;
        how.create_cycle = opts.create_cycle;
        how.difficulty_dump = opts.difficulty_dump;
        how.skip_create =
            opts.class_explicit && opts.create_dump.empty() && opts.create_cycle.empty();
        std::string pet;
        const tl::render::MenuAction choice =
            run_front_end(how, startup_save, startup_save_path, pet);
        {
            // Choose Level: `dungeon_choice()` is read before the renderer is thrown
            // away below, and character creation is skipped.
            if (choice == tl::render::MenuAction::Continue) {
                continued_from_save = continue_game(startup_save, startup_save_path);
                if (!continued_from_save) {
                    TL_LOG_ERROR("could not continue from '%s'", startup_save_path.c_str());
                    tl::core::log_shutdown();
                    return 1;
                }
            } else if (choice == tl::render::MenuAction::LoadDungeon) {
                dungeon = renderer.dungeon_choice();
                dungeon_floor = renderer.dungeon_choice_floor();
                scene.clear();
            } else if (choice != tl::render::MenuAction::NewGame) {
                renderer.shutdown();
                tl::core::log_shutdown();
                return 0;
            } else {
                // A new character gets a file of its own (`CNewGameMenu::getEmptySave`).
                save_path = empty_save_path();
            }

            if (!continued_from_save) {
                const bool skip_create =
                    choice == tl::render::MenuAction::LoadDungeon || how.skip_create;
                if (choice == tl::render::MenuAction::NewGame && !how.skip_create)
                    created_new_char = true;

                // The pet chosen on the creation screen goes into the config, to
                // survive the restart below.
                if (!pet.empty()) {
                    rc.pet = pet;
                    TL_LOG_INFO("pet chosen: %s", pet.c_str());
                }
                if (!skip_create) renderer.creation_names(rc.player_name, rc.pet_name);

                // The whole renderer is rebuilt (Renderer::restart()), then the
                // character, which was in the scene that went.
                if (!renderer.restart(rc)) {
                    tl::core::log_shutdown();
                    return 1;
                }
                renderer.begin_new_game();
                apply_class();
                if (!setup_subject()) {
                    tl::core::log_shutdown();
                    return 1;
                }
                // The quest state has to be set again, and before the layout: a level's
                // gates are evaluated once, while it is being placed.
                renderer.give_starting_quests();
                for (const std::string& quest : opts.quests) {
                    if (!renderer.set_quest(quest)) {
                        tl::core::log_shutdown();
                        return 1;
                    }
                }
                if (dungeon.empty()) scene = kTownLayout;
            }
        }
    }

    // After the subject, so the layout's camera wins over the automatic framing and
    // Player Start moves the existing character. A generated floor can fail; a
    // device build then goes back to the title menu.
    if (!opts.continue_file.empty() && !continued_from_save) {
        tl::formats::CharacterFile file;
        if (!tl::core::read_character_with_backup(opts.continue_file, file) ||
            !continue_game(file, opts.continue_file)) {
            TL_LOG_ERROR("could not continue from '%s'", opts.continue_file.c_str());
            tl::core::log_shutdown();
            return 1;
        }
        continued_from_save = true;
    }
    bool entered_via_menu = continued_from_save;
    if (continued_from_save) {
        // Already standing in the saved level.
    } else if (!dungeon.empty()) {
        if (!opts.rng_trace.empty()) renderer.set_rng_trace(opts.rng_trace);
        if (!renderer.load_dungeon(dungeon, dungeon_floor)) {
            TL_LOG_ERROR("could not generate dungeon '%s' floor %d", dungeon.c_str(),
                         dungeon_floor);
#if defined(TL_DEVICE_BUILD)
            TL_LOG_WARN("back to the title menu instead");
            if (!enter_from_menu()) {
                tl::core::log_shutdown();
                return 1;
            }
            entered_via_menu = true;
#else
            tl::core::log_shutdown();
            return 1;
#endif
        }
    } else if (!load_scene(scene)) {
        tl::core::log_shutdown();
        return 1;
    }

    // After the layout, which brings the movement speeds, body size and framing
    // (`enter_from_menu()` did this for its own pick).
    if (opts.play && !entered_via_menu && !renderer.enable_player_control()) {
        TL_LOG_ERROR("could not hand the character to the player");
        tl::core::log_shutdown();
        return 1;
    }
    if (opts.play && !entered_via_menu) {
        renderer.create_portals();
        meet_shared_stash();
    }
    // `setGameState`: `saveCharacter(false, false)` once a new character stands.
    if (opts.play && !entered_via_menu && !save_path.empty()) renderer.save_character(save_path);
    if (opts.pad) renderer.set_headless_targeting(true);
    for (const std::string& unit : opts.highlights) renderer.highlight_unit(unit);
    if (opts.attack) renderer.set_auto_attack(true);

    // The animal's switches, in play order: stance, what it carries, then the errand.
    if (!opts.pet_stance.empty() && !renderer.set_pet_stance(opts.pet_stance)) {
        TL_LOG_WARN("--pet-stance wants aggressive, defensive or passive, got '%s'",
                    opts.pet_stance.c_str());
    }
    for (const std::string& entry : opts.give_items) {
        const size_t comma = entry.find(',');
        renderer.give_player_item(entry.substr(0, comma),
                                  comma != std::string::npos &&
                                      entry.compare(comma + 1, std::string::npos, "enchant") == 0);
    }
    for (const std::string& entry : opts.death_drops) {
        const size_t comma = entry.find(',');
        renderer.roll_death_drop(
            entry.substr(0, comma),
            comma == std::string::npos ? 1 : std::max(1, std::atoi(entry.c_str() + comma + 1)));
    }
    for (const std::string& entry : opts.pet_items) {
        const size_t comma = entry.find(',');
        const std::string name = entry.substr(0, comma);
        const int count =
            comma == std::string::npos ? 1 : std::max(1, std::atoi(entry.c_str() + comma + 1));
        renderer.give_pet_item(name, count);
    }
    for (const std::string& scroll : opts.pet_learns) renderer.pet_learn(scroll);
    for (const std::string& item : opts.pet_uses) renderer.pet_use(item);
    if (opts.send_pet_to_town && renderer.send_pet_to_town()) {
        // Being sent, it runs 20 to 30 units off (`returnToTownAI`) and vanishes:
        // run the scene until it is away (ten seconds at most).
        for (int step = 0; step < 100 && renderer.pet_in_world(); ++step) {
            renderer.advance_scene(0.1f);
        }
    }
    // After the send: with `--send-pet-to-town` the fish goes to a pet that is
    // away, which the console's bag allows (asks only for a pet).
    for (const std::string& fish : opts.pet_fish) renderer.feed_pet(fish);

    // `--at`: somewhere other than the spawn.
    if (!opts.at.empty()) {
        float x = 0.0f, z = 0.0f;
        if (std::sscanf(opts.at.c_str(), "%f,%f", &x, &z) == 2) {
            renderer.place_player_at(x, z);
        } else {
            TL_LOG_ERROR("--at wants \"x,z\", got '%s'", opts.at.c_str());
            tl::core::log_shutdown();
            return 1;
        }
    }

    // The quest log and rewards, before any frame is dumped.
    if (opts.hurt > 0.0f) renderer.hurt_player(opts.hurt);
    for (const int slot : opts.item_slots) renderer.press_item_slot(slot, 5.0f);
    if (opts.pick_up_quest_items) {
        TL_LOG_INFO("--pick-up-quest-items: %d item(s) taken", renderer.pick_up_quest_items());
    }
    for (const std::string& quest : opts.accept_quests) {
        renderer.accept_quest(quest, opts.quest_level);
    }
    for (const std::string& quest : opts.complete_quests) {
        renderer.complete_quest(quest);
    }
    if (opts.award_xp > 0) renderer.award_experience(opts.award_xp);
    if (opts.pu_smoke && !renderer.spawn_pu_smoke()) {
        TL_LOG_WARN("--pu-smoke: ParticleUniverse is not running");
    }

    // The static frame first, before anything has moved.
    if (!opts.dump_frame.empty() && !renderer.dump_frame(opts.dump_frame)) {
        tl::core::log_shutdown();
        return 1;
    }

    // Then the HUD over the same scene; `--hud-wait` runs the scene first.
    if (!opts.hud_dump.empty() && opts.hud_wait > 0.0f) renderer.advance_scene(opts.hud_wait);
    if (!opts.hud_dump.empty() && !renderer.dump_game_hud(opts.hud_dump)) {
        tl::core::log_shutdown();
        return 1;
    }

    // The pet's frame (the whole HUD), after `--send-pet-to-town`.
    if (!opts.pet_dump.empty() && !renderer.dump_game_hud(opts.pet_dump)) {
        tl::core::log_shutdown();
        return 1;
    }

    // Points are spent before the dumps: a passive holds an effect only once it has a level.
    for (const std::string& name : opts.learn_skills) renderer.learn_skill(name);
    // And the casts after them, so `--learn-skill X --cast X` works in one run.
    for (const std::string& name : opts.cast_skills) renderer.cast_skill(name);
    // Then the pad's way in: a skill on a button, and the button pressed.
    for (const std::string& entry : opts.map_skills) {
        const size_t colon = entry.rfind(':');
        if (colon == std::string::npos) {
            TL_LOG_WARN("--map-skill wants <name>:<button>, not '%s'", entry.c_str());
            continue;
        }
        renderer.map_skill(entry.substr(0, colon), entry.substr(colon + 1));
    }
    for (const std::string& button : opts.hotbar_presses) renderer.press_hotbar(button);
    if (!opts.hotbar_dump.empty() && !renderer.dump_game_hud(opts.hotbar_dump)) {
        tl::core::log_shutdown();
        return 1;
    }
    if (!opts.effects_dump.empty() && !renderer.dump_effects(opts.effects_dump)) {
        tl::core::log_shutdown();
        return 1;
    }

    for (const std::string& keys : opts.inventory_keys) renderer.inventory_keys(keys);

    // Then the console inventory menu, over the same static scene.
    if (!opts.inventory_dump.empty()) renderer.set_inventory_tab(opts.inventory_tab);
    if (!opts.inventory_dump.empty() && !renderer.dump_inventory(opts.inventory_dump)) {
        tl::core::log_shutdown();
        return 1;
    }

    // Then the console stats menu, over the same static scene.
    if (!opts.stats_dump.empty() && !renderer.dump_stats(opts.stats_dump)) {
        tl::core::log_shutdown();
        return 1;
    }

    // Then the console skills tab.
    if (!opts.skills_dump.empty()) {
        if (!opts.skill_select.empty()) renderer.select_skill(opts.skill_select);
        renderer.set_skill_map_open(opts.skill_map);
    }
    if (!opts.skills_dump.empty() && !renderer.dump_skills(opts.skills_dump)) {
        tl::core::log_shutdown();
        return 1;
    }

    // The offer window, on whoever was named; before the quest tab.
    for (const std::string& who : opts.quest_offers) {
        std::string name = who;
        std::string png;
        const size_t comma = who.rfind(',');
        if (comma != std::string::npos) {
            name = who.substr(0, comma);
            png = who.substr(comma + 1);
        }
        if (png.empty()) {
            renderer.open_quest_offer(name);
        } else if (!renderer.dump_quest_offer(name, png)) {
            tl::core::log_shutdown();
            return 1;
        }
    }

    // Then the quest tab.
    if (!opts.quest_dump.empty()) renderer.set_quest_tab_selection(opts.quest_slot);
    if (!opts.quest_dump.empty() && !renderer.dump_quests(opts.quest_dump)) {
        tl::core::log_shutdown();
        return 1;
    }

    // A cinematic's frame: the class's own, or the one `--cinematic` names.
    if (!opts.intro_dump.empty()) {
        renderer.run_cinematic(
            opts.intro_dump, opts.cinematic.empty() ? renderer.intro_cinematic() : opts.cinematic);
        renderer.shutdown();
        tl::core::log_shutdown();
        return 0;
    }

    // `setGameState`: a new character's `INTRO_CINEMATIC`.
    const bool should_play_intro = !opts.no_intro && (created_new_char || opts.intro) &&
                                   !rc.headless && !renderer.intro_cinematic().empty();
    if (should_play_intro) {
        const tl::render::MenuAction intro_act =
            renderer.run_cinematic(std::string(), renderer.intro_cinematic());
        if (intro_act == tl::render::MenuAction::Quit) {
            renderer.shutdown();
            tl::core::log_shutdown();
            return 0;
        }
    }

    // Then the motion frames, in time order, each carrying on from the last.
    {
        std::vector<std::pair<float, std::string>> at = opts.dump_at;
        std::sort(at.begin(), at.end(),
                  [](const std::pair<float, std::string>& a,
                     const std::pair<float, std::string>& b) { return a.first < b.first; });
        float reached = 0.0f;
        for (const auto& entry : at) {
            renderer.advance_scene(entry.first - reached);
            reached = entry.first;
            if (!renderer.dump_frame(entry.second)) {
                tl::core::log_shutdown();
                return 1;
            }
        }
    }

    if (opts.walk_seconds > 0.0f && !renderer.simulate_walk(opts.walk_seconds)) {
        tl::core::log_shutdown();
        return 1;
    }

    if (!opts.walk_dump.empty() && !renderer.dump_frame(opts.walk_dump)) {
        tl::core::log_shutdown();
        return 1;
    }

    // Pathfinding legs, in order, after the scripted walk.
    for (const std::string& leg : opts.walk_to) {
        float x = 0.0f, z = 0.0f;
        char png[256] = {0};
        const int fields = std::sscanf(leg.c_str(), "%f,%f,%255s", &x, &z, png);
        if (fields < 2) {
            TL_LOG_ERROR("--walk-to wants \"x,z\" or \"x,z,png\", got '%s'", leg.c_str());
            tl::core::log_shutdown();
            return 1;
        }
        // A route that fails is reported and the run carries on, to dump where it stuck.
        renderer.walk_route_to(x, z, kWalkRouteTimeout);
        if (fields == 3 && !renderer.dump_frame(png)) {
            tl::core::log_shutdown();
            return 1;
        }
    }

    // Standing and fighting, and the other in-order steps, after the walks.
    for (const std::string& entry : opts.fights) {
        // `--walk-after <x>,<z>`: walked as `--walk-to` is, where it stands in the list.
        if (entry.rfind("walk:", 0) == 0) {
            float x = 0.0f;
            float z = 0.0f;
            if (std::sscanf(entry.c_str() + 5, "%f,%f", &x, &z) != 2) {
                TL_LOG_ERROR("--walk-after wants \"<x>,<z>\", got '%s'", entry.c_str() + 5);
                tl::core::log_shutdown();
                return 1;
            }
            renderer.walk_route_to(x, z, kWalkRouteTimeout);
            continue;
        }
        if (entry == "press:") {
            renderer.press_a();
            continue;
        }
        if (entry.rfind("tip:", 0) == 0) {
            renderer.press_tip(entry.substr(4));
            continue;
        }
        if (entry.rfind("talk:", 0) == 0) {
            renderer.talk_to_unit(entry.substr(5), kWalkRouteTimeout);
            continue;
        }
        if (entry.rfind("teleport:", 0) == 0) {
            float x = 0.0f;
            float z = 0.0f;
            if (std::sscanf(entry.c_str() + 9, "%f,%f", &x, &z) != 2) {
                TL_LOG_ERROR("--teleport wants \"<x>,<z>\", got '%s'", entry.c_str() + 9);
                tl::core::log_shutdown();
                return 1;
            }
            renderer.place_player_at(x, z);
            continue;
        }
        if (entry.rfind("use:", 0) == 0) {
            renderer.use_prop(entry.substr(4));
            continue;
        }
        if (entry.rfind("slay:", 0) == 0) {
            renderer.slay_unit(entry.substr(5));
            continue;
        }
        if (entry == "save:") {
            if (save_path.empty()) {
                TL_LOG_WARN("--save-now: no --save-file to save to");
            } else {
                renderer.save_character(save_path);
            }
            continue;
        }
        if (entry == "take:") {
            TL_LOG_INFO("--take-quest-items: %d item(s) taken", renderer.pick_up_quest_items());
            continue;
        }
        if (entry.rfind("takeitem:", 0) == 0) {
            TL_LOG_INFO("--take-item: %d item(s) taken", renderer.pick_up_items(entry.substr(9)));
            continue;
        }
        if (entry == "drop:") {
            TL_LOG_INFO("--drop-quest-items: %d item(s) dropped", renderer.drop_quest_items());
            continue;
        }
        if (entry == "progress:") {
            renderer.log_quest_progress();
            continue;
        }
        if (entry == "godspeed:") {
            renderer.set_godspeed(true);
            continue;
        }
        if (entry.rfind("level:", 0) == 0) {
            renderer.level_up_to(std::atoi(entry.c_str() + 6));
            continue;
        }
        if (entry == "heights:") {
            renderer.log_unit_heights();
            continue;
        }
        // `--hold-hotbar <button>,<seconds>`: a hotbar button held, then let go.
        if (entry.rfind("hold:", 0) == 0) {
            const std::string rest = entry.substr(5);
            const size_t comma = rest.find(',');
            float seconds = 0.0f;
            char png[256] = {0};
            const int read = comma == std::string::npos
                                 ? 0
                                 : std::sscanf(rest.c_str() + comma + 1, "%f,%255s", &seconds, png);
            if (read < 1) {
                TL_LOG_ERROR("--hold-hotbar wants \"<button>,<seconds>[,<png>]\", got '%s'",
                             rest.c_str());
                tl::core::log_shutdown();
                return 1;
            }
            renderer.hold_hotbar(rest.substr(0, comma), seconds);
            // The frame with the button still down: the next step lets go.
            if (png[0] != '\0' && !renderer.dump_frame(png)) {
                tl::core::log_shutdown();
                return 1;
            }
            continue;
        }
        // `--hold-x <x>,<y>,<seconds>[,<png>]`: X held with the stick at (x, y).
        if (entry.rfind("holdx:", 0) == 0) {
            float x = 0.0f, y = 0.0f, seconds = 0.0f;
            char png[256] = {0};
            if (std::sscanf(entry.c_str() + 6, "%f,%f,%f,%255s", &x, &y, &seconds, png) < 3) {
                TL_LOG_ERROR("--hold-x wants \"<x>,<y>,<seconds>[,<png>]\", got '%s'",
                             entry.c_str() + 6);
                tl::core::log_shutdown();
                return 1;
            }
            renderer.hold_attack(x, y, seconds);
            if (png[0] != '\0' && !renderer.dump_frame(png)) {
                tl::core::log_shutdown();
                return 1;
            }
            continue;
        }
        // `--stick <x>,<y>,<seconds>`: the left stick held, where it stands in the list.
        if (entry.rfind("stick:", 0) == 0) {
            float x = 0.0f, y = 0.0f, seconds = 0.0f;
            if (std::sscanf(entry.c_str() + 6, "%f,%f,%f", &x, &y, &seconds) != 3) {
                TL_LOG_ERROR("--stick wants \"<x>,<y>,<seconds>\", got '%s'", entry.c_str() + 6);
                tl::core::log_shutdown();
                return 1;
            }
            renderer.hold_stick(x, y, seconds);
            continue;
        }
        // `--stand` rides in the same list, so the two run in the order given.
        const bool stand = entry.rfind("stand:", 0) == 0;
        // `--cast-at-foe <skill>,<seconds>,<png>`: the nearest enemy targeted
        // and faced, the skill cast, then the scene run and dumped.
        const bool cast = entry.rfind("cast:", 0) == 0;
        const bool point = entry.rfind("point:", 0) == 0;
        const bool here = entry.rfind("here:", 0) == 0;
        std::string fight = stand ? entry.substr(6) : entry;
        if (here) {
            const std::string rest = entry.substr(5);
            const size_t comma = rest.find(',');
            renderer.cast_skill(rest.substr(0, comma));
            fight = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
        }
        if (point) {
            const std::string rest = entry.substr(6);
            const size_t comma = rest.find(',');
            float px = 0.0f;
            float pz = 0.0f;
            int used = 0;
            if (comma == std::string::npos ||
                std::sscanf(rest.c_str() + comma + 1, "%f,%f,%n", &px, &pz, &used) != 2) {
                TL_LOG_ERROR("--cast-at-point wants \"<skill>,<x>,<z>,<seconds>,<png>\", got '%s'",
                             rest.c_str());
                tl::core::log_shutdown();
                return 1;
            }
            renderer.cast_skill_at(rest.substr(0, comma), px, pz);
            fight = rest.substr(comma + 1 + static_cast<size_t>(used));
        }
        if (cast) {
            const std::string rest = entry.substr(5);
            const size_t comma = rest.find(',');
            renderer.face_nearest_foe();
            renderer.cast_skill(rest.substr(0, comma));
            fight = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
        }
        float seconds = 0.0f;
        char png[256] = {0};
        if (std::sscanf(fight.c_str(), "%f,%255s", &seconds, png) != 2) {
            TL_LOG_ERROR("--fight wants \"<seconds>,<png>\", got '%s'", fight.c_str());
            tl::core::log_shutdown();
            return 1;
        }
        renderer.set_auto_attack(!stand && !cast && !point && !here);
        renderer.advance_scene(seconds);
        if (!renderer.dump_frame(png)) {
            tl::core::log_shutdown();
            return 1;
        }
    }

    // Journeys, the way a warp takes them: `TOWN` or `DUNGEON:floor`, then that
    // many seconds of the level running.
    for (const std::string& step : opts.travels) {
        const size_t comma = step.find(',');
        std::string where = step.substr(0, comma);
        // `DEST@x;z[;y]`: stood there once arrived (y, the feet's height, where no
        // ground is found); a trailing `!` presses A there.
        float stand_x = 0.0f, stand_z = 0.0f, stand_y = 0.0f;
        bool stand_there = false;
        bool stand_y_given = false;
        const bool press_there = !where.empty() && where.back() == '!';
        if (press_there) where.pop_back();
        const size_t at_sign = where.find('@');
        if (at_sign != std::string::npos) {
            const int read =
                std::sscanf(where.c_str() + at_sign + 1, "%f;%f;%f", &stand_x, &stand_z, &stand_y);
            stand_there = read >= 2;
            stand_y_given = read == 3;
            where.resize(at_sign);
        }
        const float seconds = comma == std::string::npos
                                  ? 0.0f
                                  : static_cast<float>(std::atof(step.c_str() + comma + 1));
        const size_t second_comma = comma == std::string::npos ? comma : step.find(',', comma + 1);
        const std::string png =
            second_comma == std::string::npos ? std::string() : step.substr(second_comma + 1);
        const size_t colon = where.find(':');
        const bool to_town = where == "TOWN" || where == "town";
        const std::string name = where.substr(0, colon);
        const int floor = colon == std::string::npos ? 1 : std::atoi(where.c_str() + colon + 1);
        if (!warp(to_town, name, floor)) {
            tl::core::log_shutdown();
            return 1;
        }
        if (stand_there) {
            renderer.place_player_at(stand_x, stand_z, stand_y_given ? &stand_y : nullptr);
        }
        if (press_there) {
            renderer.advance_scene(0.5f);
            renderer.press_a();
        }
        if (seconds > 0.0f) renderer.advance_scene(seconds);
        if (opts.unit_heights) renderer.log_unit_heights();
        if (!png.empty() && !renderer.dump_frame(png)) {
            tl::core::log_shutdown();
            return 1;
        }
    }

    // The town portal, step by step.
    for (const std::string& step : opts.portal_steps) {
        const size_t comma = step.find(',');
        const std::string what = step.substr(0, comma);
        const float seconds = comma == std::string::npos
                                  ? 0.0f
                                  : static_cast<float>(std::atof(step.c_str() + comma + 1));
        const size_t second_comma = comma == std::string::npos ? comma : step.find(',', comma + 1);
        const std::string png =
            second_comma == std::string::npos ? std::string() : step.substr(second_comma + 1);
        const auto take_warp = [&]() -> bool {
            const auto transition = renderer.dungeon_transition();
            return warp(transition.return_to_town, transition.dungeon, transition.floor,
                        transition.portal_warp, transition.back_to_portal, transition.waypoint,
                        transition.level_delta, transition.level_absolute);
        };
        if (what == "open") {
            // From the bag: the console puts no scroll on a hotbar slot.
            if (!renderer.use_bag_item("Town Portal Scroll"))
                TL_LOG_WARN("--portal open: no Town Portal Scroll was used");
        } else if (what.compare(0, 4, "map:") == 0) {
            // A map scroll: into the bag, then the bag's A on it.
            const std::string item = what.substr(4);
            renderer.give_player_item(item, false);
            if (!renderer.use_bag_item(item)) {
                TL_LOG_WARN("--portal map: '%s' was not used", item.c_str());
            }
        } else if (what == "enter") {
            if (renderer.walk_into_portal() && !take_warp()) {
                tl::core::log_shutdown();
                return 1;
            }
        } else if (what == "spell") {
            // The `Town Portal` spell, learned from its scroll (`--inventory-keys`).
            renderer.cast_skill("Town Portal");
        } else if (what == "up" || what == "down") {
            if (renderer.walk_into_stairs(what == "up") && !take_warp()) {
                tl::core::log_shutdown();
                return 1;
            }
        } else if (what == "waygate") {
            renderer.activate_waypoint();
        } else if (what == "waypoints") {
            if (!renderer.walk_into_waypoint_portal()) {
                TL_LOG_WARN("--portal waypoints: the waypoint menu did not open");
            }
        } else if (what.rfind("choose:", 0) == 0) {
            const int row = std::atoi(what.c_str() + 7);
            if (renderer.choose_waypoint(row) && !take_warp()) {
                tl::core::log_shutdown();
                return 1;
            }
        } else {
            TL_LOG_ERROR(
                "--portal wants open, spell, enter, up, down, waygate, waypoints or choose:N, got "
                "'%s'",
                what.c_str());
            tl::core::log_shutdown();
            return 1;
        }
        if (seconds > 0.0f) renderer.advance_scene(seconds);
        if (!png.empty() && !renderer.dump_frame(png)) {
            tl::core::log_shutdown();
            return 1;
        }
    }

    // Deaths, the way a blow ends the character: the screen, a choice, the result.
    for (const std::string& step : opts.deaths) {
        std::vector<std::string> parts;
        size_t from = 0;
        for (;;) {
            const size_t comma = step.find(',', from);
            parts.push_back(step.substr(from, comma - from));
            if (comma == std::string::npos) break;
            from = comma + 1;
        }
        const std::string& choice = parts[0];
        renderer.kill_player("--die");
        for (int i = 0; i < 20 && !renderer.death_screen_open(); ++i) renderer.advance_scene(0.5f);
        if (!renderer.death_screen_open()) {
            TL_LOG_ERROR("--die: the death screen never opened");
            tl::core::log_shutdown();
            return 1;
        }
        if (parts.size() > 1 && !parts[1].empty() && !renderer.dump_death_screen(parts[1])) {
            tl::core::log_shutdown();
            return 1;
        }
        if (choice == "title") {
            TL_LOG_INFO("--die: exit to title (no save)");
            break;
        }
        const int index = choice == "here"       ? 0
                          : choice == "entrance" ? 1
                          : choice == "town"     ? 2
                                                 : -1;
        if (index < 0 || !renderer.choose_resurrection(index)) {
            TL_LOG_ERROR("--die: cannot choose '%s'", choice.c_str());
            tl::core::log_shutdown();
            return 1;
        }
        const auto transition = renderer.dungeon_transition();
        if (transition.pending &&
            !warp(transition.return_to_town, transition.dungeon, transition.floor,
                  transition.portal_warp, transition.back_to_portal, transition.waypoint,
                  transition.level_delta, transition.level_absolute)) {
            tl::core::log_shutdown();
            return 1;
        }
        renderer.advance_scene(1.0f);
        if (parts.size() > 2 && !parts[2].empty() && !renderer.dump_frame(parts[2])) {
            tl::core::log_shutdown();
            return 1;
        }
    }

    // Ground probes, then the scripted scenes: stand on a trigger, let it run, dump.
    // Last, so none of the frames above changes.
    for (const std::string& probe : opts.ground_probes) {
        float x = 0.0f, z = 0.0f;
        if (std::sscanf(probe.c_str(), "%f,%f", &x, &z) == 2) renderer.probe_ground(x, z);
    }

    for (const std::string& visit : opts.visits) {
        float x = 0.0f, z = 0.0f, seconds = 0.0f;
        char png[256] = {0};
        if (std::sscanf(visit.c_str(), "%f,%f,%f,%255s", &x, &z, &seconds, png) != 4) {
            TL_LOG_ERROR("--visit wants \"x,z,seconds,png\", got '%s'", visit.c_str());
            tl::core::log_shutdown();
            return 1;
        }
        if (!renderer.place_player_at(x, z)) {
            TL_LOG_WARN("--visit: no ground at (%.2f, %.2f); dumping anyway",
                        static_cast<double>(x), static_cast<double>(z));
        }
        // Optionally skipping the scene part-way through (`--skip-after`).
        if (opts.skip_after >= 0.0f && opts.skip_after < seconds) {
            renderer.advance_scene(opts.skip_after);
            renderer.skip_cutscene();
            renderer.advance_scene(seconds - opts.skip_after);
        } else {
            renderer.advance_scene(seconds);
        }
        if (!renderer.dump_frame(png)) {
            tl::core::log_shutdown();
            return 1;
        }

        // And put the level's script back: a visit must not leave it mid-cutscene.
        renderer.reset_script();
    }

    if (!opts.drop_dump.empty()) {
        const size_t comma = opts.drop_dump.rfind(',');
        const std::string item = opts.drop_dump.substr(0, comma);
        const std::string png =
            comma == std::string::npos ? "drop.png" : opts.drop_dump.substr(comma + 1);
        // After the walks. A toss lands well inside the one second run below.
        if (!renderer.drop_carried(item)) {
            tl::core::log_shutdown();
            return 1;
        }
        renderer.advance_scene(1.0f);
        // With the HUD when there is one, for the label on the floor.
        const bool dumped =
            renderer.game_hud_ready() ? renderer.dump_game_hud(png) : renderer.dump_frame(png);
        if (!dumped) {
            tl::core::log_shutdown();
            return 1;
        }
    }

    // Fishing that a `--press-a` on a hole started, with no `--talk-to` to carry
    // the keys.
    if (opts.talk_to.empty()) {
        for (const std::string& keys : opts.fishing_keys) renderer.fishing_keys(keys);
    }

    // Walking up to somebody and hearing what they say.
    for (size_t talk = 0; talk < opts.talk_to.size(); ++talk) {
        const std::string& who = opts.talk_to[talk];
        std::string name = who;
        std::string png;
        const size_t comma = who.rfind(',');
        if (comma != std::string::npos) {
            name = who.substr(0, comma);
            png = who.substr(comma + 1);
        }
        renderer.talk_to_unit(name, kWalkRouteTimeout);
        if (!opts.vendor_keys.empty()) {
            renderer.vendor_keys(opts.vendor_keys[std::min(talk, opts.vendor_keys.size() - 1)]);
        }
        if (!opts.fishing_keys.empty()) {
            renderer.fishing_keys(opts.fishing_keys[std::min(talk, opts.fishing_keys.size() - 1)]);
        }
        if (!opts.accept_offer.empty() && renderer.accept_offer()) {
            const size_t comma = opts.accept_offer.find(',');
            renderer.advance_scene(static_cast<float>(std::atof(opts.accept_offer.c_str())));
            if (comma != std::string::npos) {
                const std::string where = opts.accept_offer.substr(comma + 1);
                const size_t colon = where.find(':');
                const bool to_town = where == "TOWN" || where == "town";
                const int floor =
                    colon == std::string::npos ? 1 : std::atoi(where.c_str() + colon + 1);
                if (!warp(to_town, where.substr(0, colon), floor)) {
                    tl::core::log_shutdown();
                    return 1;
                }
                renderer.advance_scene(3.0f);
            }
        }
        if (!png.empty() && !renderer.dump_talk(png)) {
            tl::core::log_shutdown();
            return 1;
        }
    }

    // A retirement `--vendor-keys` took. INVENTED, a verification path: `enter_from_menu`
    // without the creation screen; the heir is the same class, unnamed (`<file>_heir.svt`).
    if (renderer.retirement_requested()) {
        const tl::core::HeirloomHandOff heirloom = renderer.take_heirloom();
        if (!save_path.empty()) renderer.save_character(save_path);
        travel = tl::core::TravelRecord{};
        if (!save_path.empty()) {
            const size_t dot = save_path.rfind(".svt");
            save_path =
                (dot == std::string::npos ? save_path : save_path.substr(0, dot)) + "_heir.svt";
        }
        if (!renderer.restart(rc)) {
            tl::core::log_shutdown();
            return 1;
        }
        renderer.begin_new_game();
        apply_class();
        if (!setup_subject()) {
            tl::core::log_shutdown();
            return 1;
        }
        renderer.give_starting_quests();
        if (!renderer.load_layout(kTownLayout) || !renderer.enable_player_control()) {
            tl::core::log_shutdown();
            return 1;
        }
        renderer.receive_heirloom(heirloom);
        renderer.create_portals();
        if (!save_path.empty()) renderer.save_character(save_path);
    }

    // run(), then whatever transition it returned for (`Renderer::DungeonTransition`),
    // each through a full `Renderer::restart()` in warp().
    auto play = [&]() {
        for (;;) {
            renderer.run();

            // Quit to the main menu; before the transition check, which a stray
            // exit-reached on the same frame must not trigger.
            if (renderer.quit_to_menu_requested()) {
                // A retirement's hand-off, out of the renderer before the restart.
                const bool retiring = renderer.retirement_requested();
                const tl::core::HeirloomHandOff heirloom =
                    retiring ? renderer.take_heirloom() : tl::core::HeirloomHandOff{};
                // `CGameClient::update`:
                // leaving the game saves it.
                if (!save_path.empty()) renderer.save_character(save_path);
                if (!enter_from_menu(retiring ? &heirloom : nullptr)) return;
                continue;
            }

            const auto transition = renderer.dungeon_transition();
            if (!transition.pending) break;

            if (!warp(transition.return_to_town, transition.dungeon, transition.floor,
                      transition.portal_warp, transition.back_to_portal, transition.waypoint,
                      transition.level_delta, transition.level_absolute)) {
                return;
            }
        }
    };

#if defined(TL_DEVICE_BUILD)
    play();
#else
    // A run that was asked for frames has produced them and is finished.
    if (opts.dump_frame.empty() && opts.walk_dump.empty() && opts.dump_at.empty() &&
        opts.visits.empty() && opts.walk_to.empty() && opts.talk_to.empty() &&
        opts.hud_dump.empty() && opts.inventory_dump.empty() && opts.intro_dump.empty() &&
        opts.stats_dump.empty() && opts.skills_dump.empty() && opts.quest_dump.empty() &&
        opts.quest_offers.empty() && opts.travels.empty() && opts.deaths.empty()) {
        play();
    }
#endif

    renderer.shutdown();
#else
    TL_LOG_WARN("built without OGRE");
    TL_LOG_WARN("run: python scripts/build_ogre.py --platform desktop");
#endif

    tl::core::log_shutdown();
    return 0;
}
