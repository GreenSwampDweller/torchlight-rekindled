// What the game is started with: the data to mount, the class and pet, the switches a
// headless run and the debug menu set. Read by the renderer and by the game (core/world.h).
#pragma once

#include "core/asset_path.h"
#include "core/level_memory.h"
#include "core/settings_menu.h"
#include "core/stash.h"

#include <string>
#include <vector>

namespace tl::core {

struct GameConfig {
    /// Directory holding the user's Torchlight install: either the install
    /// itself (containing Pak.zip) or a directory of extracted media.
    std::string game_data;

    /// `--pak PATH`: the archive mounted in place of `game_data`'s Pak.zip (for instance a PC
    /// archive with the Xbox's render data put in). Empty: `game_data`'s own.
    std::string pak;

    /// OGRE's own media - RTShaderLib, needed by RTShaderSystem to generate
    /// shaders for Torchlight's fixed-function materials. Defaults to the
    /// compiled-in TL_OGRE_MEDIA_DIR. The directory that holds `RTShaderLib/`;
    /// OGRE's `Main/` is looked for beside it (OGRE's own tree, the desktop)
    /// and then inside it (a device's single shader folder).
    std::string ogre_media;

    /// The console interface's data for a PC archive (`console_ui/`): mounted after the archive
    /// when it is not the Xbox 360's, which carries its interface itself. Empty or absent: nothing.
    std::string console_ui;

    /// Where linked shader programs are kept between runs, or empty for none: the driver's program
    /// binaries are saved at shutdown and handed back at the next start, since `restart()` (every
    /// level change) throws OGRE away. A no-op (said once in the log) where the driver offers none.
    std::string shader_cache;

    /// More archives or directories to mount into the game's resource group, after `game_data`
    /// and `console_ui`, in order: this adds, it does not replace.
    std::vector<std::string> extra_data;

    /// Extracted mod roots containing MEDIA/ (or media/). Same-path files override the game.
    std::vector<std::string> mod_data;
    /// The active mods' names (`CModFileFilter::getEnabledModNames`), as a save records them.
    std::vector<std::string> mod_names;

    /// The install's own files, indexed case-insensitively. Not owned, must outlive the renderer;
    /// null means raw paths. Needed for the music, which is not in the archive and is cased
    /// inconsistently (`../MUSIC/TOWNFIGHT.OGG` ships as `music/TownFight.ogg`).
    const AssetIndex* assets = nullptr;

    /// No visible window. A hidden one is still created: GL needs a context,
    /// and there is no offscreen path that does not have one.
    bool headless = false;
    /// `--pad`: a headless run standing in for somebody holding a pad, so a
    /// `Unit Trigger` waits for A rather than firing on the walk up to it.
    bool pad = false;

    /// The game's difficulty, `CGameClient +0x38ec`: 0 easy, 1 normal, 2 hard, 3
    /// very hard. New Game's difficulty screen sets it (`CDifficultyMenu::
    /// onClick`, `Renderer::run_difficulty_menu`); a
    /// run that never passes that screen plays at `--difficulty`, Hard unless
    /// told otherwise.
    int difficulty = 2;

    unsigned width = 1280;
    unsigned height = 720;
    bool fullscreen = false;  ///< the desktop's config.txt: fullscreen at width x height
    bool vsync = true;        ///< the desktop's config.txt: the window waits for the refresh
    unsigned fps_limit = 0;   ///< config.txt's: the most frames a second, 0 for no limit

    /// Vertical field of view in degrees, or 0 to derive it from the authored value and the
    /// viewport's aspect. The game's settings file says `FOV : 45.000000`, the vertical angle at
    /// 4:3 with the horizontal held as the display widens (34.52 degrees at 16:9). See
    /// setup_scene().
    float fov_degrees = 0.0f;

    /// Degrees per second the subject turns about its vertical axis while the
    /// frame loop runs. A slow turn is what separates a backdrop from a
    /// screenshot; 0 holds it still. It never affects a dumped frame, which is
    /// rendered before the loop starts and so always at rotation zero.
    float turntable_degrees_per_second = 9.0f;

    /// Debug switches for bisecting a visual fault (scenery, light map or particles).
    bool no_particles = false;

    /// The particle layouts drawn, as upper-case archive-path prefixes; "*" is every layout, the
    /// default. A bisection switch (`--pu-particles`).
    std::vector<std::string> pu_particle_layouts = {"*"};
    bool no_torchlight = false;
    /// Leave the shadow map out (`--no-shadows`): SHADOWS_ENABLED off.
    bool no_shadows = false;
    /// Write the light map (`ProjectorRTTLight`, the render texture the
    /// scenery's `LIGHTPASS` projects) beside every frame dump, as
    /// `<png>.lightmap.png`: what the lanterns and the TORCHLIGHT plate put
    /// on the ground, seen from its camera above the focus, -Z at the top.
    bool light_map_dump = false;
    /// With every frame dump, a record of the frame draw by draw (`render/draw_dump.h`) into this
    /// directory, to compare with the Xbox build's capture. Empty: none.
    std::string draw_dump;
    /// Draw brightness and contrast (`render/display_gamma.h`) into frame
    /// dumps too. Off by default: the window always has it, and the dumps'
    /// calibrated baselines were taken without it.
    bool dump_gamma = false;

    /// Whether the game screen's HUD is drawn at all. Off is a bisection
    /// switch and what a run comparing scenery frames wants.
    bool game_hud = true;

    /// Whether the automap/minimap is enabled during gameplay.
    bool minimap = true;

    /// Whether the frame-rate and resolution line starts on: the FPS Overlay mod's
    /// (core/mods.h). Headless runs leave it off unless `--fps` asks.
    bool stats_overlay = true;
    /// Whether `stats_overlay` follows the mod, which the mods page may switch (no `--fps` or
    /// `--no-fps`, and not headless).
    bool stats_overlay_from_mods = false;

    /// Whether to start with fullscreen automap overlay.
    bool fullscreen_map = false;

    /// Initial automap zoom (20..60): 30, as `CAutomap::CAutomap`
    /// sets it.
    float map_zoom = 30.0f;

    /// Whether to start with the inventory menu open.
    bool show_inventory = false;

    /// Levels to grant the new character at spawn, and stat points on top. Zero by default so
    /// verification runs see the same character; `--level-up` and `--stat-points` ask. A level goes
    /// through `CharacterProgression::level_up()`, with the authored curves.
    int extra_levels = 0;
    int extra_stat_points = 0;

    /// `--skill-points <n>`: points to spend on the skills tab, as `--stat-points`.
    int extra_skill_points = 0;

    /// `--skill-set <0|1>`: which of the two skill sets the skills tab shows when a headless dump
    /// opens it (the D-pad picks it in play).
    int skill_set = 0;

    /// Number of Foremen to simulate killing on spawn, testing experience and level progression.
    int foremen_kills = 0;

    /// Whether a new character is given the Xbox release's starting fish
    /// (`xbox_starting_fish()`); `--no-starting-fish` for the four PC defaults only.
    bool starting_fish = true;

    /// Open the audio device even on a headless run, which is otherwise silent: `--audio` with
    /// `--dump-at` plays the scene's sounds so a person can check the mixer.
    bool force_audio = false;

    /// Never open the audio device, even on a windowed run; wins over `force_audio`. Every sound
    /// and music call then finds no device and returns.
    bool mute = false;
    /// `--bench SECONDS`: the windowed game loop ends by itself after this
    /// long (0: it does not). For timing a change on the desktop with the same
    /// five-second fps lines the console logs, without anybody closing the
    /// window; run it with `--mute`.
    float bench_seconds = 0.0f;
    /// `--invulnerable`: the player takes no damage (the debug menu's god mode
    /// without GODSPEED's speed affix) - for frame comparisons against the
    /// Xbox capture, whose player is held the same way.
    bool invulnerable = false;

    /// Whether `CGameUI::queueTip` takes anything. DIVERGENCE (verification):
    /// off for a headless run, where an open tip would hold the scripted pad
    /// and stand in the dumped frames; `--tips` puts them back. The Show Tips
    /// setting is not touched by it.
    bool context_tips = true;

    /// Which animal follows the character: `Dog` or `Cat`, as `media/units/monsters/` names them,
    /// or empty for none. Set by `--pet` or the creation screen and carried across New Game's
    /// restart; `Dog` because `charactercreate.layout` opens on it.
    std::string pet = "Dog";

    /// The names typed on the console's creation screen (`CNewGameMenu`, stored at `CGameClient
    /// +0x14D4` and `+0x14F0`). Empty for a character that skipped the screen (named after its
    /// class). The pet's name is carried but nothing shows it yet.
    std::string player_name;
    std::string pet_name;

    /// Where the player has been: `CPlayer`'s remembered levels, dungeon records
    /// and play time. Owned by the caller so that it outlives `restart()`, as
    /// the original's `CPlayer` outlives a level change. Null: nothing is
    /// remembered.
    TravelRecord* travel = nullptr;

    /// `CSettings`, as far as the Options screen reaches into it
    /// (`core/settings_menu.h`). Owned by the caller so that it outlives
    /// `restart()`; null, and the renderer keeps a set of its own.
    GameSettings* settings = nullptr;
    /// Where Done on the Options screen writes them. Empty: nowhere.
    std::string settings_path;

    /// `CSharedStash`, one for every character, owned by the caller so that it
    /// outlives `restart()` as the original's (`CMasterResourceManager +0x178`)
    /// outlives a level. Null: no shared stash.
    SharedStash* shared_stash = nullptr;

    /// `CPlayer +0xa15`, kept in the character file. The difficulty screen's
    /// checkbox sets it for a new character (`CGameClient +0x1b8`,
    /// `CDifficultyMenu::onClick`); `--hardcore` and
    /// a loaded file too.
    bool hardcore = false;

    /// The character file `saveCharacter` writes, owned by the caller. Null or
    /// empty: none. `CPlayer::die` saves a hardcore character's death itself.
    const std::string* save_path = nullptr;
};

}  // namespace tl::core
