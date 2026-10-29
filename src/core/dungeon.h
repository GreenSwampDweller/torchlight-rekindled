// Procedural dungeons: a dungeon definition, and the level template that
// generates its floors. A `Warper` naming a `DUNGEON NAME` leads to a level
// built at runtime: `media/dungeons/<NAME>.DAT.adm` says which ruleset each
// stratum uses, and a ruleset is a level template - `[CHUNKTYPE]` room shapes
// with their doorways, and a folder of `.LAYOUT` room pieces for each.
//
// `CLevelTemplateData`: `load`,
// `createRandomLayout` and its helpers,
// `deleteLayouts`, and the layout-choice stage
// of `CLevel::loadRoomLayout`.
//
// No OGRE and no file I/O: the caller hands in the parsed AdmFile and answers
// the two file-system questions `load` asks.
#pragma once

#include "core/random.h"
#include "core/randomizer.h"
#include "formats/adm.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tl::core {

/// The eleven spawn slots of `g_LEVELLAYOUT_CREATION_NAMES` (filled in
/// `_GLOBAL__I_CLevelTemplateData`): monsters, champions, props,
/// NPCs, creeps, gold, fish, formations, quest monsters, quest items, quest
/// champions, in that order.
constexpr size_t kSpawnSlotCount = 11;
using SpawnSlots = std::array<std::string, kSpawnSlotCount>;

/// One `[STRATA<n>]` block of a dungeon definition: the ruleset a range of its
/// floors uses, and what populates them. Not every dungeon authors every key.
struct DungeonStrata {
    std::string ruleset;  ///< `RULESET`: e.g. `media/layouts/cave/rules.dat`
    int floors = 1;       ///< `FLOORS`

    /// Each spawn slot's population numbers, as `CDungeon::getStrataTemplate`
    /// reads them from
    /// `g_LEVELLAYOUT_CREATION_PATHNODES` (a density per square metre of
    /// pathable area, `MONSTERS_PER_METER_MIN/MAX` ...
    /// `FORMATIONS_PER_METER_MIN/MAX`) and `g_LEVELLAYOUT_CREATION_COUNTS` (a
    /// flat count, `MONSTER_MIN/MAX` ... `FISH_MIN/MAX`, with the binary's
    /// `PPROPS_MAX`). Only an authored value replaces the template's.
    std::array<float, kSpawnSlotCount> density_min{};
    std::array<float, kSpawnSlotCount> density_max{};
    std::array<float, kSpawnSlotCount> count_min{};
    std::array<float, kSpawnSlotCount> count_max{};
    std::array<bool, kSpawnSlotCount> density_min_authored{};
    std::array<bool, kSpawnSlotCount> density_max_authored{};
    std::array<bool, kSpawnSlotCount> count_min_authored{};
    std::array<bool, kSpawnSlotCount> count_max_authored{};

    /// `MONSTER_LVL_MIN`/`MONSTER_LVL_MAX`: the level range monsters on this
    /// stratum are drawn from, rolled once per spawn. -1 (both) is unauthored,
    /// `CDungeon::getStrataTemplate`'s default
    /// (`), which leaves it to `DungeonDef::monster_level_mult`.
    int monster_lvl_min = -1;
    int monster_lvl_max = -1;

    // Read by `CDungeon::loadDungeon` and `CDungeon::getStrataTemplate`, and
    // laid onto the level template the stratum builds.
    /// `WEIGHT`: each of its floors' odds in `getRandomLevelTemplateData`
    int weight = 1;
    std::string dungeon;            ///< `DUNGEON`: take a random floor of this dungeon instead
    std::string name_override;      ///< `NAME_OVERRIDE`: replaces the template's `LEVELNAME`
    std::string cinematic;          ///< `CINEMATIC`: replaces the template's, even when empty
    bool no_random_quests = false;  ///< `NO_RANDOM_QUESTS`
    bool dont_store = false;        ///< `DONTSTORE`
    bool allow_portals = true;      ///< `ALLOW_PORTALS`
    bool allow_pet_return = true;   ///< `ALLOW_PET_RETURN`
    bool is_town = false;           ///< `IS_TOWN`

    /// `MUST_BE_COMPLETED` and `MUST_BE_COMPLETED_OR_ACTIVE`, each a list of
    /// `QUEST` names (`CDungeon +0x28` / `+0x40`, one entry per depth).
    /// `CGameClient::loadLevel` completes the first and gives the second.
    std::vector<std::string> must_be_completed;
    std::vector<std::string> must_be_completed_or_active;

    /// Each slot's key (`MONSTERSPAWNCLASS` ... `QUESTCHAMPIONSPAWNCLASS`) and
    /// its `...RANDOMIZED` flag, each with whether it was authored: only an
    /// authored value replaces the template's (`CDataGroup::GetDataValue` with
    /// the template's value as the default).
    SpawnSlots spawn_slot_names;
    std::array<bool, kSpawnSlotCount> spawn_slot_authored{};
    std::array<bool, kSpawnSlotCount> spawn_slot_randomized{};
    std::array<bool, kSpawnSlotCount> spawn_slot_randomized_authored{};
};

/// A dungeon, at `CDungeon::CDungeon`'s values
/// until `loadDungeon` reads its file: no salt
/// (`+0x58` -1), a monster level multiplier of 1 (`+0x70`), a player level
/// match of 1 to 1 offset 0 (`+0x74..+0x7c`), volatile (`+0x88`), not
/// bottomless (`+0x89`), its lists empty and its depth randomizer seeded 0
/// (`+0x90`).
struct DungeonDef {
    std::string name;            ///< `NAME`
    std::string parent_dungeon;  ///< `PARENT_DUNGEON` - where its entrance sits
    std::string display_name;    ///< `DISPLAYNAME`
    std::vector<DungeonStrata> strata;
    bool ok = false;

    /// `MONSTER_LVL_MULT`, the fallback when a stratum leaves its level range
    /// unauthored: `CGameClient::getRankOfMonstersOnFloor`
    /// (`) computes `max(1, round((floor + 1) * MONSTER_LVL_MULT))`, the
    /// floor counting from 0.
    float monster_level_mult = 1.0f;

    /// +0x60 `NAME` uppercased; `CDungeonManager::getDungeonByName` compares it
    std::string upper_name;
    bool is_volatile = true;            ///< +0x88 `VOLATILE`
    bool bottomless = false;            ///< +0x89 `BOTTOMLESS`
    int player_level_match_min = 1;     ///< +0x74 `PLAYER_LVL_MATCH_MIN`
    int player_level_match_max = 1;     ///< +0x78 `PLAYER_LVL_MATCH_MAX`
    int player_level_match_offset = 0;  ///< +0x7c `PLAYER_LVL_MATCH_OFFSET`

    /// +0x100: the stratum each depth uses, one entry per `FLOORS` of each
    /// stratum in order. The first floor of a dungeon is depth 0.
    std::vector<uint32_t> depth_strata;

    /// +0x90: one choice per depth, valued by the depth, with odds of its
    /// stratum's `WEIGHT`.
    Randomizer depth_weights;
};

/// `CDungeon::loadDungeon(wchar_t const*)`: a
/// `[DUNGEON]` file, `media/dungeons/<NAME>.DAT.adm`. Strata are read by name,
/// `STRATA0`, `STRATA1`, ..., stopping at the first name that is missing.
DungeonDef parse_dungeon_def(const tl::formats::AdmFile& file);

struct TravelRecord;
class LevelTemplate;

/// `CGameClient::getRankOfMonstersOnFloor`.
/// Computes the monster rank for a floor based on depth, the dungeon's
/// `MONSTER_LVL_MULT` and player tracking (`PLAYER_LVL_MATCH_*`).
/// `dungeon == nullptr` returns `max(1, depth)`. `player == nullptr` or
/// `use_player == false` skips the player matching term.
int32_t rank_of_monsters_on_floor(int32_t depth, const DungeonDef* dungeon,
                                  const TravelRecord* player, int32_t player_level,
                                  bool use_player);

/// `CGameClient::loadLevel`.
/// Monster level for the floor: rolls within the template's authored range
/// [`MONSTER_LVL_MIN`, `MONSTER_LVL_MAX`] on `volatile_rng` bounded by
/// `player_level`, or falls back to `std::max(1, rank)` when the template
/// authors no range.
int32_t level_monster_level(const LevelTemplate* level_template, int32_t rank, bool has_player,
                            int32_t player_level, RunicRandom& volatile_rng);

// --------------------------------------------------------------------------
// The level template (CLevelTemplateData) and the objects it builds.

/// One `[EXIT]` of a chunk type: a doorway, in the chunk's own space. 12 bytes
/// in `CChunkType`'s exit list.
struct ChunkExit {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

/// `CChunkType` (size 0x60): one `[CHUNKTYPE]` group, a room shape.
///
/// A chunk spans x in [origin - span/2, origin + (WIDTH - 1) * span + span/2]
/// and z in [origin, origin + ...] with span = `CHUNKWIDTHBASIS * TILEBASIS`;
/// see `LevelTemplate::intersects_chunks()` for the exact bounds.
struct ChunkType {
    bool entrance = false;        ///< +0x10 `ENTRANCE_CHUNK`
    bool exit = false;            ///< +0x11 `EXIT_CHUNK`
    bool registered = false;      ///< +0x12 set by `load` when its first room piece is listed
    bool must_place = false;      ///< +0x13 `MUST_PLACE`
    int32_t width = 1;            ///< +0x14 `WIDTH`
    int32_t height = 1;           ///< +0x18 `HEIGHT`
    int32_t max_appearance = 10;  ///< +0x1c `MAX_APPEARANCE`, per room piece, not per type
    std::string folder;           ///< +0x20 rules directory + `FOLDER` (default `NAME`) + "/"
    std::vector<std::string> inclusive_files;  ///< +0x28 `INCLUSIVE_FILES` that exist, uppercase
    std::string name;                          ///< +0x40 `NAME`, as authored
    std::vector<ChunkExit> exits;              ///< +0x48 `[EXIT]` groups, in order
};

/// `CChunk` (size 0x28): one room-piece file a chunk type can be filled with.
struct Chunk {
    uint32_t type_index = 0;      ///< +0x10
    int32_t odds = 1;             ///< +0x14, always 1; `resetOdds` restores it
    uint32_t max_appearance = 0;  ///< +0x18, the type's `MAX_APPEARANCE`
    /// +0x20: uppercase archive path with the compiled extension stripped,
    /// `MEDIA/LAYOUTS/CAVE/1X1NS/1X1NS_A.LAYOUT`.
    std::string path;
};

/// `CChunkInstance` (size 0x48): one placed chunk. Its position is the world
/// position its room piece is placed at, with an identity rotation.
struct ChunkInstance {
    uint32_t type_index = 0;  ///< +0x10 (0xffffffff for a `CHUNK_RANDOM` naming no type)
    float x = 0.0f;           ///< +0x20
    float y = 0.0f;           ///< +0x24
    float z = 0.0f;           ///< +0x28
    /// +0x30 `std::vector<int>`: indices into its type's exits that `matchExits`
    /// joined to a neighbour, one entry per join.
    std::vector<int32_t> connected_exits;
};

/// `CLevelLayout` (size 0x48): one complete floor, instances and their pieces
/// in parallel.
struct LevelLayout {
    std::vector<uint32_t> pieces;          ///< +0x10, indices into `LevelTemplate::chunks`
    std::vector<ChunkInstance> instances;  ///< +0x28
    int32_t seed = 0;                      ///< +0x40, `createRandomLayout`'s argument
};

/// The file-system questions `CLevelTemplateData::load` asks, answered by the
/// caller.
struct LevelTemplateFiles {
    /// `CFileSystem::getFileInfo`: does this layout exist? The argument is an
    /// uppercase path ending `.LAYOUT`, with no compiled extension.
    std::function<bool(const std::string& layout)> exists;

    /// `CFileSystem::getFileList(folder, list, L"*.layout", false, true, false,
    /// false)`: every layout at or below `folder`, as
    /// `CFileSystem::getFilesInDirectory`
    /// names them - the archive path uppercased, with its last four characters
    /// (`.CMP`) cut off. Order does not matter; `load` sorts.
    std::function<std::vector<std::string>(const std::string& folder)> list_layouts;
};

/// `CLevelTemplateData`: a parsed ruleset and the layouts generated from it.
///
/// Only the fields floor generation reads are carried; the lighting, fog,
/// water and camera keys `load` also stores are `apply_environment()`'s job in
/// the renderer.
class LevelTemplate {
public:
    // Scalars read by `load`, with the constructor's defaults.
    //
    bool randomized = true;            ///< +0x58 `RANDOMIZED`
    bool requires_exit = true;         ///< +0x59 `REQUIRESEXIT`
    bool dont_store = false;           ///< +0x5a `DONTSTORE`
    std::string name;                  ///< +0x68 `NAME`, uppercased
    bool has_pathing = true;           ///< +0x70 `HASPATHING`
    bool has_automap = true;           ///< +0x71 `HASAUTOMAP`
    bool populate = true;              ///< +0x72 `POPULATE`
    int32_t min_chunks = 2;            ///< +0x74 `MINCHUNKS`
    int32_t max_chunks = 3;            ///< +0x78 `MAXCHUNKS`
    int32_t monster_level_min = -1;    ///< +0x7c `MONSTER_LVL_MIN`
    int32_t monster_level_max = -1;    ///< +0x80 `MONSTER_LVL_MAX`
    bool no_random_quests = false;     ///< +0x87 `NO_RANDOM_QUESTS`
    float tile_basis = 4.0f;           ///< +0x88 `TILEBASIS`
    float chunk_width_basis = 25.0f;   ///< +0x8c `CHUNKWIDTHBASIS`
    float chunk_height_basis = 25.0f;  ///< +0x90 `CHUNKHEIGHTBASIS`
    float camera_mult = 1.0f;          ///< +0x94 `CAMERAMULT`
    float active_range_mod = 0.0f;     ///< +0x98 `ACTIVERANGEMOD`
    std::string redirect_dungeon;      ///< +0x60 the stratum's `DUNGEON`
    bool allow_portals = true;         ///< +0x84 the stratum's `ALLOW_PORTALS`
    bool allow_pet_return = true;      ///< +0x85 the stratum's `ALLOW_PET_RETURN`
    bool is_town = false;              ///< +0x86 the stratum's `IS_TOWN`
    std::string cinematic;             ///< +0x698 `CINEMATIC`, then the stratum's
    std::string level_particle;        ///< +0x6a8 `LEVELPARTICLE`
    /// +0x6f0 `MAINMENURULES`: the backdrop `CGameClient::loadMenuLevel`
    /// loads when a saved character last
    /// stood on this template.
    std::string main_menu_rules;
    /// +0x76c `SHOWFLOORNUMBER`, 1 from the constructor: whether the level's
    /// name carries " - Floor N" (`CLevel::getName`).
    ///
    bool show_floor_number = true;
    std::string level_name = "Dungeon";  ///< +0x770 `LEVELNAME`, or the stratum's `NAME_OVERRIDE`

    /// +0x520: each spawn slot's class, the constructor's default
    /// (`CLevelTemplateData::CLevelTemplateData`)
    /// until a stratum authors one. Slots 5, 7 and 9 are assigned from a string
    /// object rather than a literal, taken as empty.
    SpawnSlots spawn_classes = {"MONSTERSET",
                                "MONSTERSETCHAMPION",
                                "PROPS",
                                "NPCS",
                                "CREEPS",
                                "",
                                "FISH_SPAWN",
                                "",
                                "MONSTERSET",
                                "",
                                "MONSTERSETCHAMPION"};
    std::array<bool, kSpawnSlotCount> spawn_classes_randomized{};  ///< +0x5d0
    SpawnSlots chosen_spawn_classes;  ///< +0x578, written by `chooseUnitSpawners`

    /// X360 +0x634 / +0x638 + slot*8: density per square metre of pathable area, with
    /// the constructor's defaults: monsters,
    /// champions, props, NPCs, creeps, gold, fish, formations, then the quest
    /// slots. The console's champions top out at 0.0003 (the Linux build's 0.0004).
    std::array<float, kSpawnSlotCount> density_min = {0.01f, 0.00025f, 0.005f, 0.0f, 0.0f, 0.0012f,
                                                      0.0f,  0.001f,   0.0f,   0.0f, 0.0f};
    std::array<float, kSpawnSlotCount> density_max = {
        0.0125f, 0.0003f, 0.0075f, 0.0f, 0.0f, 0.0017f, 0.0f, 0.002f, 0.0f, 0.0f, 0.0f};
    std::array<float, kSpawnSlotCount> count_min{};  ///< X360 +0x68c + slot*8, 0 in the constructor
    std::array<float, kSpawnSlotCount> count_max{};  ///< X360 +0x690 + slot*8, 0 in the constructor
    bool layout_created = false;                     ///< +0x762, set by `createRandomLayout`

    std::vector<ChunkType> chunk_types;  ///< +0x10 TArrayList<CChunkType*>
    std::vector<Chunk> chunks;           ///< +0x28 TArrayList<CChunk*>, sorted per type
    std::vector<uint32_t> must_place;    ///< +0x40 TArrayList<CChunkType*>, as type indices

    /// +0xb8: the `[LAYOUT]` groups' fixed layouts first (+0xd0 holds the same
    /// ones), then every layout `createRandomLayout` has added.
    std::vector<LevelLayout> layouts;
    size_t fixed_layout_count = 0;  ///< +0xd8, the count of +0xd0

    Randomizer layout_randomizer;        ///< *+0xe8, picks from `layouts`
    bool has_layout_randomizer = false;  ///< +0xe8 is null until `load` or `deleteLayouts`

    /// +0xf0: one per `[CHUNKTYPE]` group, holding that type's chunk indices.
    std::vector<Randomizer> piece_randomizers;

    // +0x108 .. +0x4b0: chunk types with at least one room piece, sorted into
    // pools. A single-doorway type goes into a pool by the side of its own
    // chunk that doorway faces.
    Randomizer ordinary_types;          ///< +0x108 neither entrance nor exit
    Randomizer entrance_types;          ///< +0x170
    Randomizer exits_facing_pos_z;      ///< +0x1d8
    Randomizer exits_facing_neg_z;      ///< +0x240
    Randomizer exits_facing_neg_x;      ///< +0x2a8
    Randomizer exits_facing_pos_x;      ///< +0x310
    Randomizer dead_ends_facing_pos_z;  ///< +0x378
    Randomizer dead_ends_facing_neg_z;  ///< +0x3e0
    Randomizer dead_ends_facing_neg_x;  ///< +0x448
    Randomizer dead_ends_facing_pos_x;  ///< +0x4b0

    /// `CLevelTemplateData::load(wchar_t const*)`,
    /// the parts floor generation reads.
    /// `path` is the ruleset path as the dungeon names it
    /// (`media/layouts/cave/rules.dat`); its directory prefixes every folder.
    /// `rng` is `g_Rand` as `load` finds it: a `[LAYOUT]` group's
    /// `CHUNK_RANDOM` entries draw their room pieces here, at load time.
    bool load(const std::string& path, const tl::formats::AdmFile& file,
              const LevelTemplateFiles& files, RunicRandom& rng);

    /// `CLevelTemplateData::resetOdds()`:
    /// every piece's odds back to 1.
    void reset_odds();

    /// `CLevelTemplateData::getRandomChunk(unsigned int)`: a chunk index.
    uint32_t get_random_chunk(uint32_t type_index, RunicRandom& rng);

    /// `CLevelTemplateData::intersectsChunks`.
    bool intersects_chunks(const ChunkInstance& chunk,
                           const std::vector<ChunkInstance>& placed) const;

    /// `CLevelTemplateData::exitsMatch`.
    bool exits_match(const ChunkInstance& chunk, const std::vector<ChunkInstance>& placed) const;

    /// `CLevelTemplateData::matchExits`.
    void match_exits(ChunkInstance& chunk, std::vector<ChunkInstance>& placed) const;

    /// `CLevelTemplateData::createRandomLayout(int)`:
    /// generate one layout into `layouts`.
    /// Returns `seed` (0 when the template has no entrance type), whether or
    /// not a layout was added.
    int32_t create_random_layout(RunicRandom& rng, int32_t seed);

    /// `CLevelTemplateData::getRandomLayout()`. Null when there is
    /// nothing to pick.
    const LevelLayout* get_random_layout(RunicRandom& rng);

    /// `CLevelTemplateData::deleteLayouts()`:
    /// back to the fixed layouts only.
    void delete_layouts();

    /// Chunk type `index`, with TArrayList's out-of-range read of element 0.
    const ChunkType& type_at(uint32_t index) const { return tarray_at(chunk_types, index); }
};

/// The layout stage of `CLevel::loadRoomLayout(CLevelTemplateData*, ...)`:
/// seed `g_Rand` with the level seed,
/// generate ten layouts if the template is `RANDOMIZED` and has none yet, seed
/// again with the same value, and pick one. `rng` stands for `g_Rand`.
const LevelLayout* choose_room_layout(LevelTemplate& level_template, RunicRandom& rng,
                                      int32_t level_seed);

/// One `[OBJECT]` of a spawn class, as `chooseUnitSpawners` sees it: the
/// `SPAWNCLASS` it refers to (empty for a `UNIT` or `UNITTYPE` entry), and its
/// `WEIGHT` as `CSpawnClass::addSpawnClassData`
/// stores it: -1 when unauthored or authored
/// 0.
struct SpawnClassRef {
    std::string spawn_class;
    int32_t weight = -1;
};

/// `CResourceManager::getSpawnClassByName`: false when there is no such class.
using SpawnClassLookup =
    std::function<bool(const std::string& name, std::vector<SpawnClassRef>& entries)>;

/// `CLevelTemplateData::chooseUnitSpawners(CResourceManager*)`:
/// fills `chosen_spawn_classes`. A slot that
/// is not `RANDOMIZED`, or whose class does not exist, keeps its class; a
/// `RANDOMIZED` one is replaced by one of the classes its entries refer to,
/// drawn by weight (100 for a weight of 0 or less).
void choose_unit_spawners(LevelTemplate& level_template, const SpawnClassLookup& lookup,
                          RunicRandom& rng);

/// `CLevelTemplateData::getNumberOfUnitsToCreate(ELEVELLAYOUT_CREATION, unsigned int)`:
/// how many of a slot a level wants. A flat
/// count wins when either end is above 0; otherwise both densities must be, and
/// they scale by the pathable area, `pathable_cells / 6.5` square metres
/// (`CLevel+0x58`, passable 0.4-unit cells). Draws once from `rng` (`g_Rand`)
/// unless the answer is 0.
int64_t units_to_create(const LevelTemplate& level_template, size_t slot, uint32_t pathable_cells,
                        RunicRandom& rng);

// --------------------------------------------------------------------------
// CDungeon: which level template a depth gets.

class Dungeon;

/// What `CDungeon` needs from outside: the ruleset files its templates are
/// built from, and `CDungeonManager::getDungeonByName` for a stratum that
/// names another dungeon.
struct DungeonFiles {
    LevelTemplateFiles layouts;
    /// The parsed ruleset a stratum's `RULESET` names; an empty AdmFile when
    /// it does not exist.
    std::function<tl::formats::AdmFile(const std::string& ruleset)> read_ruleset;
    /// The dungeon whose uppercased `NAME` equals `upper_name`, or null.
    std::function<Dungeon*(const std::string& upper_name)> dungeon_by_name;
};

/// `CLevel::getName`: what the HUD's
/// `LevelName` window shows, every frame (`CGameUI::updateIngameUI`).
/// The template's `LEVELNAME` alone when it does not show floor numbers, else
/// `LEVELNAME - Floor N` with N the depth plus one. A level with no template
/// has an empty name. "Floor" goes through `CStringTranslate`, which for
/// English is itself.
std::string level_display_name(const std::string& level_name, bool show_floor_number,
                               int32_t depth);

/// `CGameClient::loadLevel`: the small banner raised under
/// the level's name on arrival - "Floor N" when the template shows floor
/// numbers, else empty.
std::string level_floor_caption(bool show_floor_number, int32_t depth);

/// `CDungeon`: a dungeon definition and the level templates its depths have
/// built so far. A template is built the first time its depth is asked for,
/// from whatever the generator holds then, and kept.
class Dungeon {
public:
    explicit Dungeon(DungeonDef definition);

    DungeonDef def;
    /// +0x10: one slot per depth, null until built.
    std::vector<std::unique_ptr<LevelTemplate>> templates;
    /// +0x58: the salt every level seed of this dungeon adds. -1 from the
    /// constructor; `CDungeonManager::reload` rolls it,
    /// `CGameClient::loadCharacter` puts a saved one back and
    /// `CGameClient::performWarp` rolls it again on entering it afresh.
    int32_t salt = -1;

    /// `CDungeon::getStrataTemplate(unsigned int)`.
    /// Null past the last depth. `source`,
    /// when given, receives the stratum the template comes from.
    LevelTemplate* get_strata_template(uint32_t depth, const DungeonFiles& files, RunicRandom& rng,
                                       const DungeonStrata** source = nullptr);

    /// `CDungeon::getRandomLevelTemplateData()`: a depth drawn by
    /// its stratum's `WEIGHT`.
    LevelTemplate* get_random_level_template(const DungeonFiles& files, RunicRandom& rng,
                                             const DungeonStrata** source = nullptr);

    /// `CDungeon::getLevelTemplateDataForDepth(CGameClient*, unsigned int)`:
    /// the depth's template, the last
    /// depth's past the end, or a random template of the dungeon its stratum's
    /// `DUNGEON` names.
    LevelTemplate* get_level_template_for_depth(uint32_t depth, const DungeonFiles& files,
                                                RunicRandom& rng,
                                                const DungeonStrata** source = nullptr);
};

/// `CDungeonManager`, the process's one (`g_pDungeonManager`, built by
/// `CMasterResourceManager`'s constructor over `media/dungeons/`): every
/// dungeon, its salt and the templates its depths have built, for as long as
/// the game runs.
class DungeonManager {
public:
    /// `CDungeonManager::CDungeonManager(path)` (on the console
    /// made by `CMasterResourceManager::CMasterResourceManager`):
    /// the path upper-cased and cleaned (`+0x10`), the list empty
    /// (growing by 10), `g_pDungeonManager` set, and `reload` at once - here
    /// the first time the manager is asked for (`loaded`).
    /// +0x18: in the order `CFileSystem::getFileList` listed their files.
    std::vector<std::unique_ptr<Dungeon>> dungeons;
    bool loaded = false;

    /// `CDungeonManager::reload()`:
    /// `setSeed(0)`, then each definition in list order, its salt
    /// `randomIntegerBetweenVolatile(0, 0x7fffffff)` as it is added.
    void reload(std::vector<DungeonDef> definitions, RunicRandom& g_rand,
                RunicRandom& volatile_rng);

    /// `CDungeonManager::getDungeonByName(std::wstring)`:
    /// the first dungeon whose `NAME`
    /// (+0x60) equals the uppercased name; null for none.
    Dungeon* dungeon_by_name(const std::string& name) const;
};

/// The process's `CDungeonManager`.
DungeonManager& dungeon_manager();

/// `CGameClient +0x1090`, `+0x1094` and `+0x38d8`: the character's game seed,
/// the depth it was saved at and the dungeon the player is in. They live as
/// long as the client, the whole process.
struct GameSeed {
    int32_t seed = 0;         ///< +0x1090, 0 in the constructor
    int32_t saved_depth = 0;  ///< +0x1094, `CCharacterSaveState +0x64` on a load
    /// +0x38d8, `CGameClient::setCurrentDungeon`:
    /// null in no dungeon.
    Dungeon* current_dungeon = nullptr;
};

/// The process's `CGameClient` seed fields.
GameSeed& game_seed();

/// `KSETTINGS_LEVEL_SEED`. `local_settings.txt` writes 0, which is the value
/// every reader below sees: the console port has no settings file to change it.
inline constexpr int32_t kLevelSeedSetting = 0;

/// The game seed a new character gets: `LEVEL_SEED` when it is not 0, else
/// `randomIntegerBetweenVolatile(0, 0x7fffffff)`. `CGameClient::CGameClient`
/// and `setGameState` for states 3 and 4 both.
int32_t roll_game_seed(int32_t level_seed_setting, RunicRandom& volatile_rng);

/// `CGameClient::resetGameSeed(int)`:
/// `setSeed(game seed + the current dungeon's salt + depth)`, with the saved
/// depth for -1 and no salt without a current dungeon. Returns what was seeded.
int32_t reset_game_seed(const GameSeed& seed, int32_t depth, RunicRandom& g_rand);

/// The level seed `CLevel::loadRoomLayout` seeds and
/// prints as `Seeding: n`: `LEVEL_SEED` in place of the game seed when it is
/// set, plus the depth (`CLevel +0x1a4`) and the current dungeon's salt.
int32_t level_seed(int32_t level_seed_setting, const GameSeed& seed, int32_t depth);

}  // namespace tl::core
