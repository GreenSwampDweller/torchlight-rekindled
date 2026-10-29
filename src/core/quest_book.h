// A Torchlight quest: what it asks for, what it pays, and what the player has
// done about it (`core/quest.h` holds the level gates). No OGRE and no file
// I/O: the caller of QuestBook::add() reads the archive.
#pragma once

#include "core/collision.h"
#include "core/dialog.h"
#include "core/quest.h"
#include "core/random.h"
#include "core/random_names.h"
#include "core/stats.h"
#include "core/units.h"
#include "formats/adm.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace tl::core {

/// Which of the four spoken sections a line belongs to, or the journal entry:
/// `CQuest`'s section table (Linux .data, read by `loadQuestData`).
///
enum class QuestSection {
    Intro,     ///< the offer, before the player has taken it
    Return,    ///< what the giver says while it is unfinished
    Complete,  ///< what they say when it is done
    Passive,   ///< small talk, and every cutscene line the town's quests carry
    Details,   ///< the journal entry; nobody speaks it
};

const char* quest_section_name(QuestSection section);
/// The section a line was read from, by its tag (`INTRO` ...); `Intro` for none.
QuestSection quest_section_of(const std::string& tag);

/// What a unit placed for a quest carries: `CBaseUnit +0x170` (the quest's
/// GUID, -1 for none) and `+0x178` (the objective's id). This port names the
/// quest instead of its GUID.
struct QuestUnitTag {
    std::string quest;
    int32_t objective = -1;
    bool empty() const { return quest.empty(); }
};

struct QuestObjective;

/// `CQuestUnitData::populate`'s floor gate:
/// whether this objective
/// is placed on the level. A quest that names no dungeon clears `create`,
/// hence not const.
bool objective_on_level(QuestObjective& objective, const std::string& quest_dungeon,
                        int quest_depth, const std::string& level_dungeon, int level_depth);

/// `CQuestUnitData::getDungeonFloorActiveOn`: `RELATIVEFLOOR` added
/// to the quest's floor (`CQuest +0x20`; a negative sum keeps the quest's own),
/// else `SPECIFICFLOOR`, else the quest's floor. 0-based; a tag prints it plus 1.
int objective_floor(const QuestObjective& objective, int quest_depth);

/// `CQuestUnitData::getDeepestFloor`: a
/// `RELATIVEFLOOR` added to the quest's floor, 1 when the sum is negative; with
/// none, `SPECIFICFLOOR`, 0 for its -1; then the deepest of that and every child's.
int deepest_floor(const QuestObjective& objective, int quest_depth);

/// `CQuest::getDungeonMaxFloor`: the quest's
/// floor, or the deepest unit of the `[ACQUIRE]` then `[DEFEAT]` groups it took.
int dungeon_max_floor(const std::vector<QuestObjective>& acquire,
                      const std::vector<QuestObjective>& defeat, int quest_depth);

/// `CQuest::getQuestHasRandomPieces`: any
/// unit of the groups it took is `getIsRandom`.
bool quest_has_random_pieces(const std::vector<QuestObjective>& acquire,
                             const std::vector<QuestObjective>& defeat);

/// One thing a quest wants done - `CQuestUnitData`, one `[UNIT]` of a
/// `[DEFEAT]`, `[ACQUIRE]` or `[POPULATE]` group (`CQuest::calculateUnitsFromTag`),
/// parsed by `CQuestUnitData::parseDataGroup`
/// over the defaults of
/// `CQuestUnitData::CQuestUnitData`.
struct QuestObjective {
    /// `ITEM`, `MONSTER`, `SPAWNCLASS`, `SPAWNFROMUNITTYPE`, `UNITTYPE` and
    /// `UNIQUE_MONSTER_NAME` - the six ways a unit can be named.
    std::string item;
    std::string monster;
    std::string spawn_class;
    std::string spawn_from_unittype;
    std::string unit_type;
    std::string unique_monster_name;

    /// `RELATIVEFLOOR`, default 0, and `SPECIFICFLOOR`, default -1.
    int relative_floor = 0;
    int specific_floor = -1;

    /// `MINCOUNT` / `MAXCOUNT`. How many are wanted; a range means the quest
    /// rolls one when it is given (`getIsRandom()` is exactly `min != max`).
    int min_count = 0;
    int max_count = 0;

    bool remove_from_inventory = true;   ///< `REMOVEFROMINVENTORY`
    bool remove_only_questitems = true;  ///< `REMOVE_ONLY_QUESTITEMS`
    bool never_destroy = true;           ///< `NEVERDESTROY`
    bool create = true;                  ///< `CREATE`
    bool make_champion = false;          ///< `MAKECHAMPION`

    std::vector<QuestObjective> children;

    /// `+0xb4`: 0 for `[DEFEAT]` and `[POPULATE]`, 1 for `[ACQUIRE]`. It picks
    /// the level marker `createUnit` uses: `Quest Boss` for 0, `Quest Item` for 1.
    int kind = 0;

    // ---- the unit it resolved to (`getBaseUnitDataGroup`) --------
    /// `+0x20`: the resolved unit, by `NAME` (`resolve_objective_unit`); empty
    /// for a `UNITTYPE` objective and for a roll that found nothing, which
    /// `spawnUnit` refuses to place.
    std::string resolved;
    /// `+0x44`: the `UNITTYPE`'s id, 0x16 (`NONE`) from the constructor; set
    /// only for a `UNITTYPE` with no `SPAWNCLASS`.
    uint32_t unit_type_id = 22;
    /// `+0xb0`: `LEVEL_QUEST_MONSTERS` (1), `LEVEL_QUEST_CHAMPIONS` (2),
    /// `LEVEL_QUEST_ITEMS` (3), else 0; set by `getSpawnClassUnitDataLookingFor`.
    int predefined_class = 0;

    // ---- per quest instance ---------------------------------------------
    /// `+0x10`, from `m_gQuestUnitDataID`: the number a placed unit carries back
    /// (`+0x178`). Only its uniqueness is used.
    int32_t id = 0;

    /// `+0xb9`: this objective has been placed. Set once by
    /// `CQuestUnitData::populate`, saved with the quest, and
    /// required before anything is credited (`unitDefeated`,
    /// `unitPickedUp`).
    bool populated = false;

    // ---- progress -------------------------------------------------------
    /// How many of `wanted` the player has, and whether this one is settled.
    /// `done` (`+0x60`) starts false whatever the count: a target of zero is
    /// met by the first kill (`unitDefeated` counts, then tests).
    int wanted = 0;
    int have = 0;
    bool done = false;

    /// `+0xc0`: the `CRandomNames::generateName(true)` name of a non-`ITEM`
    /// `MAKECHAMPION` objective, drawn off the quest's seed; empty until the
    /// quest is taken with a name book set (`QuestLog::set_random_names`).
    std::string champion_name;

    /// What `getUnitName` prints otherwise (`QuestLog::set_unit_namer`): the
    /// `ITEM`'s or `MONSTER`'s `DISPLAYNAME`, else its `NAME`; for a
    /// `UNITTYPE`, the type's name (`+0x68`).
    std::string display_name;

    /// `CQuestUnitData::getUnitName`: the
    /// champion name, else `display_name` with a `{...}` span taken out.
    /// INVENTED: with nothing resolved the authored key is printed, where the
    /// original prints an empty string.
    std::string unit_name() const;

    /// Whether a unit called `name` is the unit this objective resolved to
    /// (`unitDefeated`, `unitPickedUp`); before anything
    /// resolved it, the authored `ITEM` or `MONSTER`. Not `UNIQUE_MONSTER_NAME`,
    /// which is only printed.
    bool matches(const std::string& name) const;

    /// `done` and every child complete. `CQuestUnitData::isComplete(false)`;
    /// `isComplete(true)` is `done`
    /// (`+0x60`) alone.
    bool complete() const;

    /// `CQuestUnitData::getIsRandom`: a count
    /// range, a `SPAWNCLASS`, or a child that is.
    bool is_random() const;
};

/// Resolves `QuestObjective::display_name` (see there); empty when the
/// objective names nothing it can find.
using QuestUnitNamer = std::function<std::string(const QuestObjective&)>;

struct QuestEntry;

/// What `CQuestUnitData` reaches outside the quest for
/// (`QuestLog::set_unit_world`); left empty, only an `ITEM` or a `MONSTER`
/// resolves.
struct QuestUnitWorld {
    /// `CHierarchy::getTypeIDByName`.
    std::function<uint32_t(const std::string& type_name)> type_id;
    /// `getSpawnClassUnitDataLookingFor`
    /// before its roll: `CGameClient::resetGameSeed(seed_depth)`,
    /// then
    /// `CLevelTemplateData::chooseUnitSpawners`
    /// on the quest's level template, into `chosen`. False, the generator
    /// untouched, when the quest has no template.
    std::function<bool(const QuestEntry& entry, int32_t seed_depth,
                       std::vector<std::string>& chosen)>
        choose_spawners;
    /// `CQuest::getPlayer` stands on a level, without which
    /// a spawn class is not rolled; true when not set. A loaded quest's unit
    /// comes back by its saved guid (`CQuestUnitData::load`),
    /// not by a roll.
    std::function<bool()> player_on_level;
    /// `CSpawnClassParser::getSpawnClass(name)` found a class.
    std::function<bool(const std::string& name)> has_spawn_class;
    /// `CSpawnClass::rollSpawnClass(out, null, null, null, level, 0xffffffff,
    /// level_bonus, rarity_override, 0, 0)` on `g_Rand`: the first unit drawn,
    /// empty for none.
    std::function<std::string(const std::string& name, int32_t level, int32_t level_bonus,
                              int32_t rarity_override)>
        roll_spawn_class;
    /// `CLevel::randomOpenPosition(at, radius, false)`, which
    /// `spawnChildrenUnits` draws for every child, placed or not.
    std::function<Vec3(const Vec3& at, float radius)> random_open_position;
    /// `CQuestUnitData::spawnUnit` past its
    /// own checks: the objective's resolved unit made at the level's level,
    /// added at `at`, stamped with `tag`, a champion with `MAKECHAMPION`.
    std::function<void(const QuestEntry& entry, const QuestObjective& objective,
                       const QuestUnitTag& tag, const Vec3& at)>
        spawn_unit;

    /// A unit of the level `units_of_type` found: `handle` is the caller's
    /// own, `item` says which of the level's lists it is on, `tag` what it
    /// carries now (`+0x170` / `+0x178`; empty for a quest GUID of -1).
    struct LevelUnit {
        int64_t handle = 0;
        bool item = false;
        QuestUnitTag tag;
    };
    /// `CLevel::getUnitsByUnitType(type, list)`:
    /// the level's characters, then its items, each in list order,
    /// that `CBaseUnit::ISA(type)`. Empty when not set.
    std::function<std::vector<LevelUnit>(uint32_t type)> units_of_type;
    /// What `CQuestUnitData::populate` does to the unit it picked:
    /// tags it; a character's NPC icon is worked
    /// out again (`CQuestManager::calculateNPCIcon`), an equipment identified and
    /// its text dropped (`CItem::destroyItemText` twice).
    std::function<void(const LevelUnit& unit, const QuestUnitTag& tag)> tag_unit;
};

/// The two lines a counted kill or a newly found item puts up for a second
/// (`CGameUI::setBigText` / `setSmallText` with 1.0): the quest's name over
/// "<unit> - <have>/<wanted> Defeated" (`unitDefeated`) or
/// "<item> - <have>/<wanted> Found" (`unitPickedUp`).
struct QuestProgressText {
    std::string big;
    std::string small;
};

/// The unit a quest event is about (`questEventFire`'s third argument).
struct QuestEventUnit {
    /// Its `NAME`: the data group the events compare (`+0x1b0`).
    std::string name;
    /// What the progress line calls it: a character's name (`+0x4c0`), an
    /// item's `getItemName()`. Empty for a unit that is neither,
    /// which gets no line.
    std::string shown_name;
    /// `+0x170` / `+0x178`. A pickup that counts an untagged item stamps it
    /// with its quest (`unitPickedUp`), which the caller keeps.
    QuestUnitTag tag;
    /// `CBaseUnit::ISA(type)`; false for every type when not set.
    std::function<bool(uint32_t type)> isa;
    /// `getPosition(true)`, where `spawnChildrenUnits` puts a child.
    Vec3 position{};
    /// For `unitInteracted`: the unit has a level (`+0x68 -> +0x18`).
    bool on_level = true;

    // PORT: the unit's `ISA`, through the renderer's hook.
    bool is_a(uint32_t type) const { return isa && isa(type); }
};

/// What `CQuest::reinitializeQuest`'s floor roll
/// asks of the quest's dungeon and of the
/// player.
struct QuestDungeon {
    /// `CDungeonManager::getDungeonByName(DUNGEON)` found one.
    bool found = false;
    /// Its `DISPLAYNAME` (X360 `CDungeon +0x58`): what `[DUNEGONNAME]` becomes.
    std::string display_name;
    /// `CPlayer::getMaxDepth(DUNGEON)`: the deepest floor of it the player has
    /// been on, -1 for none.
    int max_depth = -1;
    /// `CDungeon +0x18`, how many depths it has, and `+0x89`, `BOTTOMLESS`.
    int depth_count = 0;
    bool bottomless = false;
    /// Whether the level template of a floor says `NO_RANDOM_QUESTS` (`+0x87`),
    /// after `resetGameSeed(floor)` and `getLevelTemplateDataForDepth(floor)`.
    std::function<bool(int floor)> no_random_quests;
    /// `CGameClient::getRankOfMonstersOnFloor(floor, dungeon)`,
    /// with the player as it stands. Null: no client, and
    /// `initializeQuestWithNPC` leaves the level alone.
    std::function<int32_t(int floor)> rank_on_floor;
    /// for a `CLEAR_DUNGEON_HISTORY` quest: the player's
    /// levels of the dungeon expired (`CPlayer::clearDungeonHistory`) and the
    /// kept levels gone over (`updateStoredLevels` on the player's level).
    std::function<void()> clear_history;
};
using QuestDungeonSource = std::function<QuestDungeon(const std::string& dungeon)>;

/// What `questRequirmentsHaveBeenMet` reads of
/// the player, through the quest manager (X360 `+0x1c8 -> +0x8`).
struct QuestPlayer {
    bool present = false;
    uint32_t level = 0;  ///< X360 `CPlayer +0xf0`
    /// The player stands on a level (X360 `+0x60`, and that level's `+0xc`).
    bool on_level = false;
    /// `CPlayer::getMaxDepth(dungeon)`: -1 for none.
    std::function<int32_t(const std::string& dungeon)> max_depth;
    /// The `NAME` of `dungeon`'s template for `depth` (`CDungeonManager::getDungeonByName`,
    /// `getLevelTemplateDataForDepth`), with no `resetGameSeed`
    /// first; false when there is none.
    std::function<bool(const std::string& dungeon, uint32_t depth, std::string& name)>
        template_name;
};
using QuestPlayerSource = std::function<QuestPlayer()>;

/// The floor `reinitializeQuest` binds a
/// quest to (`CQuest +0x20`), and whether it ran the quest out (`+0x210`, which
/// `getQuestIsValidForNPC` refuses on).
struct QuestFloor {
    int depth = 0;
    bool exhausted = false;
};

/// `reinitializeQuest`'s floor loop: up to ten tries, `k` from 1, each the
/// player's deepest floor plus `k` (0 when negative), taken when its template
/// allows random quests. A `REPEATABLE` quest (`+0x20b`) in a dungeon that is
/// not `BOTTOMLESS` stops, exhausted, as soon as a try reaches the dungeon's
/// depth count. After ten refusals the last floor tried stands.
QuestFloor roll_quest_floor(bool repeatable, const QuestDungeon& dungeon);

struct QuestDefinition;

/// `CQuest::reinitializeQuest`, after the
/// floor: how many `[DEFEAT]` groups the quest takes, the first so many of
/// them - `randomIntegerBetween(defeat_create, the group count)` on `g_Rand`,
/// which draws nothing when the two meet.
uint32_t take_defeat_groups(const QuestDefinition& quest, RunicRandom& g_rand);

/// What a quest pays - `CQuestRewards`, parsed by `parseRewardTag`.
/// The percentages are of an authored curve
/// at the quest's level. All zero until a `[REWARD]` block is read
/// (`CQuestRewards::CQuestRewards`); XP's 500/800 are that block's.
struct QuestRewardSpec {
    float gold_min_pct = 0.0f;  ///< `GOLDMINPCT`, default 0
    float gold_max_pct = 0.0f;  ///< `GOLDMAXPCT`, default 0
    float xp_min_pct = 0.0f;    ///< `XPMINPCT`, 500 (0x1f4) inside a `[REWARD]`
    float xp_max_pct = 0.0f;    ///< `XPMAXPCT`, 800 (0x320) inside a `[REWARD]`
    float fame_min_pct = 0.0f;  ///< `FAMEMINPCT`, default 0
    float fame_max_pct = 0.0f;  ///< `FAMEMAXPCT`, default 0

    /// `TREASURECLASS` - a spawn class rolled for item rewards.
    std::string treasure_class;

    /// One `[UNITREWARD]` group (`CUnitCreateRef`, X360 +0x1c list).
    struct UnitReward {
        std::string unit;      ///< `UNITNAME`
        bool identify = true;  ///< `IDENTIFY`, default true (`CUnitCreateRef +0xc`)
        /// The resource list the name was looked up in: the group's own index
        /// in the `[REWARD]`, not a search of the four (see `parse_quest_definition`).
        uint32_t resource_group = 0;
    };
    std::vector<UnitReward> unit_rewards;

    // PORT: whether a quest authors any reward.
    bool any() const {
        return gold_max_pct > 0.0f || xp_max_pct > 0.0f || fame_max_pct > 0.0f ||
               !treasure_class.empty() || !unit_rewards.empty();
    }
};

/// The three numbers the offer window prints, once they have been rolled.
struct QuestReward {
    int gold = 0;
    int xp = 0;
    int fame = 0;
};

/// One unit `CQuestRewards::calculateRewards` made and keeps
/// for the payout (its unit rewards, X360 `CQuestRewards +0x44`).
struct QuestRewardUnit {
    std::string unit;      ///< its `NAME`
    bool is_item = false;  ///< a `CEquipment` (the `__dynamic_cast`)
    ItemStats item;        ///< the copy as it was made, for an item
};

/// What `CQuestRewards::calculateRewards` reaches outside the
/// quest for when it makes the reward units.
struct QuestRewardWorld {
    /// The quest's resource manager has a level and the quest manager a
    /// player: without both nothing is worked out, figures or units.
    /// Unset is "no".
    std::function<bool()> ready;
    /// `CResourceManager::createUnit(rm, data group, 0, false)`
    /// of a unit by name, looked up in the resource list
    /// `resource_group` names (0 items, 1 monsters, 2 players, 3 props): its own
    /// draws, and the copy into `out`. False when nothing was made.
    std::function<bool(const std::string& unit, uint32_t resource_group, QuestRewardUnit& out)>
        create_unit;
    /// `CSpawnClass::rollSpawnClass(class, units, enchant flags, player, player,
    /// level, seed, 0)` on `g_Rand`: the units it picked, in
    /// order, and an enchant flag for each.
    std::function<void(const std::string& spawn_class, int32_t level, uint32_t seed,
                       std::vector<std::string>& units, std::vector<bool>& enchant)>
        roll_spawn_class;
    /// `CEquipment::enchant(item, true)`.
    std::function<void(ItemStats& item)> enchant;
    /// The level's difficulty (`CLevel +0x141c`), which picks the gold and
    /// fame curves.
    std::function<int()> difficulty;
    const StatBook* graphs = nullptr;
};

/// Whether a quest may be offered at all: `CQuestRequirements`, as
/// `CQuestRequirements::parseRequirementTag`
/// leaves it; defaults from `CQuestRequirements::CQuestRequirements`.
/// Inside a `[REQUIREMENTS]` group an unauthored
/// number is 0xffffffff.
struct QuestRequirementSpec {
    /// X360 `+0x8`: the `[REQUIREMENTS]` group itself; a quest without one
    /// passes at once.
    bool present = false;
    uint32_t min_player_level = 0;       ///< X360 +0x10 `MINPLAYERLEVEL`
    uint32_t max_player_level = 999999;  ///< X360 +0x14 `MAXPLAYERLEVEL`
    int32_t min_dungeon_depth = 0;       ///< X360 +0x18 `MINDUNGEONDEPTH`
    int32_t max_dungeon_depth = 999999;  ///< X360 +0x1c `MAXDUNGEONDEPTH`
    /// X360 +0x20 `RULESET`, upper-cased (`STRINGS::StringUpper`): the `NAME`
    /// of the level template the player's next floor uses.
    std::string ruleset;

    /// `[QUESTSCOMPLETE]`, `[QUESTSNOTCOMPLETE]` and `[QUESTSACTIVE]`: every
    /// `STRING` or `TRANSLATE` value, whatever its key, is a quest name,
    /// upper-cased.
    std::vector<std::string> quests_complete;
    std::vector<std::string> quests_not_complete;
    std::vector<std::string> quests_active;
};

/// One `media/Quests/**/<name>.DAT.adm`, whole, as `CQuest::loadQuestData`
/// reads it.
struct QuestDefinition {
    std::string name;          ///< `NAME`, the key everything else refers to
    std::string display_name;  ///< `DISPLAYNAME`, with its `|c` markup intact
    std::string dungeon;       ///< `DUNGEON`
    std::string quest_type;    ///< `QUESTTYPE` - only `FIND` and `KILL BOSS` ship
    std::string alternative_complete_text;
    std::string alternative_complete_text_small;
    int64_t guid = 0;  ///< `QUEST_GUID`

    bool completes_on_accept = false;         ///< `COMPLETESONACCEPT`
    bool force_accept = false;                ///< `FORCEACCEPT`
    bool show_quest_complete = true;          ///< `SHOWQUESTCOMPLETE`, default **true**
    bool quest_controller_completes = false;  ///< `QUESTCONTROLLERCOMPLETES`
    bool no_notification = false;             ///< `NO_NOTIFICATION`
    bool clear_dungeon_history = false;       ///< `CLEAR_DUNGEON_HISTORY`
    bool repeatable = false;                  ///< `REPEATABLE`
    bool can_abandon = false;                 ///< `CANABANDON`
    bool show_tips = false;                   ///< `SHOWTIPS`

    /// `[GIVE_QUESTS_ON_COMPLETE]`, then the quests whose taking or completing
    /// this one waits for (Linux .data; `QuestEntry::accepted_named`).
    std::vector<std::string> give_quests_on_complete;
    std::vector<std::string> complete_on_quest_accept;
    std::vector<std::string> complete_on_quest_complete;

    std::vector<QuestObjective> defeat;
    std::vector<QuestObjective> acquire;
    std::vector<QuestObjective> populate;

    /// The group's `MINCREATE`, at least 1, or 0 with no `[UNIT]`
    /// (`calculateUnitsFromTag`): the fewest `[DEFEAT]` groups `reinitializeQuest`
    /// takes; `[ACQUIRE]`'s is never read. 1 from `CQuest::CQuest`.
    uint32_t defeat_create = 1;
    uint32_t acquire_create = 1;

    QuestRequirementSpec requirements;
    QuestRewardSpec reward_spec;

    /// The spoken lines, by section, plus the journal entry.
    std::vector<DialogLine> intro;
    std::vector<DialogLine> ret;
    std::vector<DialogLine> complete;
    std::vector<DialogLine> passive;
    DialogLine details;
    bool has_details = false;

    /// Every distinct `UNITNAME` this quest's dialogue names, upper-cased.
    std::vector<std::string> speakers() const;
};

/// Parse one dialogue line from an `[INTRO]`, `[RETURN]`, `[COMPLETE]`,
/// `[PASSIVE]` or `[DETAILS]` section: every `DIALOG` string in file order, `\n`
/// made a newline (`CQuestDialog::parseDialogTag`).
DialogLine parse_dialog_line(const tl::formats::AdmSection& entry);

/// Build a definition from a quest file's `[QUEST]` section. False without a
/// `NAME`, the one thing `loadQuestData` insists on. `DialogLine::seconds` is
/// left at zero for the loader (`Renderer::Impl::load_quest_book`).
bool parse_quest_definition(const tl::formats::AdmSection& root, QuestDefinition& out);

/// Every quest under `media/Quests/`, by name.
class QuestBook {
public:
    void add(QuestDefinition quest);

    /// Case-insensitive, like every other name in this engine.
    const QuestDefinition* find(const std::string& name) const;
    /// By `QUEST_GUID`, which is what a unit's `+0x170` holds; null for none.
    const QuestDefinition* find_by_guid(int64_t guid) const;

    /// Every quest whose dialogue names `unit` as a speaker, in the order added:
    /// `loadQuests` in `CFileSystem::getFileList` order, which
    /// matters because `quest_for_npc` draws among them by index.
    std::vector<const QuestDefinition*> for_unit(const std::string& unit) const;

    /// `CQuestManager`'s random people's quest (X360 `+0x54`): the quest whose
    /// `NAME` is `RANDOMPEOPLE` (`loadQuests`); null for none.
    const QuestDefinition* random_people() const;

    /// Every quest in the order of `CQuestManager`'s own map: by `NAME` as
    /// written, case-sensitive (`getQuestByName`).
    std::vector<const QuestDefinition*> in_manager_order() const;

    size_t size() const { return by_name_.size(); }

private:
    std::map<std::string, QuestDefinition> by_name_;  ///< keyed upper-cased
    std::vector<std::string> order_;                  ///< the keys, as added
};

/// The player's own copy of one quest: the rolled counts, the rolled reward and
/// how far along it is.
struct QuestEntry {
    const QuestDefinition* definition = nullptr;
    QuestStatus status = QuestStatus::Unknown;

    /// What `[DUNEGONNAME]` becomes: the `DISPLAYNAME` of the quest's dungeon
    /// (`CDungeonManager::getDungeonByName`), resolved once
    /// (`QuestLog::dungeon_display_name`; `CQuest::replaceStringTags`
    /// asks each time). None leaves the tag standing.
    std::optional<std::string> dungeon_display_name;

    /// `CQuest +0x20c`, from `CGameClient::getRankOfMonstersOnFloor`
    /// (`initializeQuestWithNPC`). The reward curves are sampled at
    /// `level + 1`.
    int level = 1;

    /// `CQuest +0x1c`, -1 from `CQuest::CQuest`;
    /// an NPC giving the quest draws it with
    /// `randomIntegerBetweenVolatile(0, INT_MAX)`.
    int32_t seed = -1;

    QuestReward reward;

    /// The item the `TREASURECLASS` rolled, as an icon name and what it is
    /// called; resolved once, when the quest is taken. Empty when the class
    /// picks by `UNITTYPE`.
    std::string reward_icon;
    std::string reward_item;

    /// `CQuestDialog +0x76` (`CQuest::setQuestAcceptDialogInteracted`): the
    /// offer acknowledged, which completes a quest that asks for nothing
    /// (`CQuest::isComplete`).
    bool dialog_interacted = false;

    /// The rewards' items have been handed over (`CQuestRewards::rewardPlayer`);
    /// only `reInitializeRewards`
    /// (here `accept`'s re-roll) makes them again.
    bool reward_items_given = false;
    /// The units `calculateRewards` made for the quest when an NPC offered it
    /// (`Offered::reward_units`), handed over at the payout.
    std::vector<QuestRewardUnit> reward_units;

    /// `CQuest +0x209`: the "Quest Complete" banner has been put up, which
    /// `CQuestManager::questEventUpdate` does
    /// only once; cleared by `reinitializeQuest`, not saved.
    bool complete_announced = false;

    /// The definition's objectives, counts rolled, with the progress on them.
    /// `defeat` is the first `take_defeat_groups` of the groups.
    std::vector<QuestObjective> defeat;
    std::vector<QuestObjective> acquire;
    std::vector<QuestObjective> populate;
    /// One flag per `[COMPLETE_ON_QUEST_ACCEPT]` / `[COMPLETE_ON_QUEST_COMPLETE]`
    /// name, set by `CQuest::questEventUpdate`'s event 3 / 4;
    /// each is one more thing `objectives_complete` waits for.
    std::vector<uint8_t> accepted_named;
    std::vector<uint8_t> completed_named;
    /// How many times past the list's end the first `[DEFEAT]` group is taken
    /// again: a `MINCREATE` above the group count reads element 0 for each
    /// extra index (`TArrayList` past its end).
    uint32_t defeat_repeats = 0;
    /// The `[DEFEAT]` group the console's list holds at `index`, and how many
    /// it holds (`defeat` and the repeats of its first).
    // PORT: an accessor of a held quest.
    size_t defeat_group_count() const { return defeat.size() + defeat_repeats; }
    const QuestObjective& defeat_group(size_t index) const {
        return index < defeat.size() ? defeat[index] : defeat.front();
    }

    /// `CQuest +0x20`, the floor the quest is bound to: 0 from the constructor,
    /// rolled by `reinitializeQuest`
    /// when an NPC takes the quest up.
    int depth = 0;

    /// `CQuest::isComplete(false)`: never for a `QUESTCONTROLLERCOMPLETES`
    /// quest; with any `[DEFEAT]` or `[ACQUIRE]` objective or
    /// `COMPLETE_ON_QUEST_*` name, all of them; with none, `dialog_interacted`.
    bool objectives_complete() const;

    /// Whether this quest asks for anything to be killed or fetched.
    bool has_objectives() const { return !defeat.empty() || !acquire.empty(); }

    /// The journal entry with its `[TAG]`s substituted. Empty when the quest
    /// authors no `[DETAILS]`.
    std::string details() const;

    const std::string& name() const;
    const std::string& display_name() const;
};

/// A unit's done flag (`+0x60`) as a load takes it from its record.
///
/// DIVERGENCE (2026-10-11): `CQuestUnitData::save`
/// saves `isComplete(false)`, which left a champion killed
/// before its item was picked up unfinishable. The port saves the unit's own
/// flag, and reads an old record as done when its count shows it.
bool quest_unit_done_from_save(bool saved_done, int32_t wanted, int32_t have);

/// `CQuestUnitData::getBaseUnitDataGroup`,
/// the unit half: fills `objective.resolved`, rolling a spawn class where it
/// must (which reseeds `g_Rand`). Returns whether it went through the
/// champion-name tail: every path but an `ITEM` and an `[ACQUIRE]`
/// unit that names nothing.
bool resolve_objective_unit(QuestObjective& objective, const QuestEntry& entry,
                            const QuestUnitWorld& world);

/// How many journal-visible quests the player may carry at once:
/// `CQuestDialogMenu::onClick` refuses past five.
inline constexpr int kQuestLogSize = 6;

/// One `CPlayer::removeQuestItemFromInventoryOrPetsInventoryByGuid`
/// that completing a quest makes, for the
/// renderer to carry out on the player's bag and then the pet's
/// (`Inventory::remove_quest_items`).
struct QuestItemRemoval {
    int64_t quest_guid = -1;
    std::string unit;  ///< empty: any unit of the quest (the original's -1)
    bool whole_stack = false;
    uint32_t isa = 0;
    /// `CLevel::makeQuestUnitsUncool(quest guid)` (inlined into
    /// `CQuest::cleanUp`) instead: every item of the player's
    /// level with the quest's GUID that `ISA(isa)` is marked for deletion.
    bool level_items = false;
};

/// One `CQuestManager::completeQuest`, for
/// the renderer to carry out: `giveRewardForQuest` (->
/// `CQuest::giveRewardForQuest`) counts it and pays
/// `CQuestRewards::rewardPlayer` whenever the quest has a `[REWARD]`, on every
/// path that completes a quest.
struct QuestCompletion {
    const QuestDefinition* quest = nullptr;
    /// Whether the player held it. Only a held quest has had
    /// `initializeQuestWithNPC` roll its rewards (`reInitializeRewards`);
    /// one completed without being held pays nothing.
    bool held = false;
    QuestReward reward;
    int level = 1;      ///< `CQuest +0x20c`
    int32_t seed = -1;  ///< `CQuest +0x1c`
    /// `CQuestDialog +0x76` as `cleanUp` finds it (`QuestEntry::dialog_interacted`).
    bool dialog_interacted = false;
    /// Whether the rewards still hold their items (`CQuestRewards +0x60`): only
    /// until the first payout.
    bool reward_items = true;
    /// The units `calculateRewards` made when the quest was offered
    /// (`QuestEntry::reward_units`); empty for a quest taken without an NPC.
    std::vector<QuestRewardUnit> reward_units;
};

/// `CQuestRewards::calculateRewards`, the
/// arguments of its `TREASURECLASS` roll: `setSeed(quest
/// seed)`, then a level offset `randomIntegerBetween(-1, 2)` added to the
/// quest's level, and `quest seed + 0x272b` for `rollSpawnClass`'s own seed.
struct TreasureRoll {
    int32_t level = 0;
    uint32_t seed = 0;
};
TreasureRoll quest_treasure_roll(int quest_level, int32_t quest_seed, RunicRandom& rng);

/// The player's quest log, and the state machine over it.
class UnitThemeBook;
class UnitThemeSet;
class UnitTypeHierarchy;

/// One change `CQuestManager` passes to each `CQuestController` whose quest name
/// equals the quest's upper-cased `NAME` (`STRINGS::StringUpper`):
/// `outputs` in order. `giveQuest`: Quest
/// Active (0x62); `removeQuest`: Quest Not Active (0x63), then Quest Abandoned
/// (0x66) for an abandon; `completeQuest`: Quest Complete (0x64); `resetQuest`:
/// Quest Not Complete (0x65).
struct QuestNotice {
    std::string quest;  ///< upper-cased
    std::vector<uint32_t> outputs;
};

class QuestLog {
public:
    /// The gate state the level placer reads. Kept in step with the log, so a
    /// quest completed here opens the `Group`s that were waiting on it.
    // PORT: an accessor of the log.
    QuestState& state() { return state_; }
    // PORT: an accessor of the log.
    const QuestState& state() const { return state_; }

    // PORT: the log's wiring, set by the renderer.
    void set_book(const QuestBook* book) { book_ = book; }
    /// Who resolves an objective's printed name when a quest is taken.
    // PORT: the log's wiring, set by the renderer.
    void set_unit_namer(QuestUnitNamer namer) { unit_namer_ = std::move(namer); }
    /// Who answers for a quest's dungeon when its floor is rolled.
    // PORT: the log's wiring, set by the renderer.
    void set_dungeon_source(QuestDungeonSource source) { dungeon_source_ = std::move(source); }
    /// `CRandomNames::getSingleton()`, for a `MAKECHAMPION` objective's name.
    void set_random_names(const RandomNames* names) { random_names_ = names; }
    /// What `CQuestUnitData` reaches outside the quest for (see there).
    void set_unit_world(QuestUnitWorld world) { unit_world_ = std::move(world); }
    /// What `CQuestRewards::calculateRewards` makes reward units with.
    // PORT: the log's wiring, set by the renderer.
    void set_reward_world(QuestRewardWorld world) { reward_world_ = std::move(world); }
    const QuestUnitWorld& unit_world() const { return unit_world_; }
    /// The objectives' printed names again (`getUnitName`
    /// reads the unit as it is asked), for an entry
    /// whose units a save put back.
    void name_entry(QuestEntry& entry) const;

    /// What `initializeQuestWithNPC` left on
    /// a quest an NPC took up: its seed, floor, exhausted flag, reward level and
    /// `[DEFEAT]` group count (-1 before any roll).
    struct Offered {
        int32_t seed = -1;
        int depth = 0;
        bool exhausted = false;
        int32_t level = 1;
        int32_t defeat_groups = -1;
        /// X360 `CQuest +0x21`, which `CQuest::CQuest` leaves
        /// unwritten (the console reads 1). A payout sets it
        /// (`CQuest::giveRewardForQuest`) and the next offer
        /// with an NPC clears it and makes the rewards again.
        bool can_reroll_rewards = true;
        /// What `calculateRewards` made and keeps (`CQuestRewards +0x44`), in
        /// its order: the `[UNITREWARD]`s, then the `TREASURECLASS` roll.
        std::vector<QuestRewardUnit> reward_units;
    };
    const Offered* offered(const QuestDefinition& quest) const;
    /// `CQuestRewards::calculateRewards` for
    /// an offered quest: the three figures (returned), then each `[UNITREWARD]`,
    /// then the `TREASURECLASS` units, kept on the offer.
    QuestReward calculate_rewards(const QuestDefinition& quest, Offered& rolled);
    /// An offer as some other record holds it - its rolled state and the unit
    /// that gave it (`CQuest +0x1d4`; empty for none) - put on the log in place
    /// of whatever it had.
    // PORT: the log's state set from outside (a test hands it the console's).
    void restore_offer(const QuestDefinition& quest, const Offered& rolled,
                       const std::string& giver);
    /// A quest not held, its seed (`CQuest +0x10`), floor (`+0x14`) and rank (`+0x1ec`) as
    /// the manager's reset rolled them.
    // PORT: the log's state set from outside (the oracle hands it the console's).
    void seed_dormant(const QuestDefinition& quest, uint32_t seed, int depth, int level);
    /// The level an offered quest is worth (`Offered::level`), or `fallback`
    /// for one no NPC has initialised.
    int offer_level(const QuestDefinition& quest, int fallback) const;
    const QuestBook* book() const { return book_; }
    /// `CDungeonManager::getDungeonByName(quest's DUNGEON)`'s `DISPLAYNAME`, as
    /// `replaceStringTags` asks it (`QuestEntry::dungeon_display_name`); none
    /// without a dungeon source, a `DUNGEON`, or a dungeon of that name.
    std::optional<std::string> dungeon_display_name(const QuestDefinition& quest) const;

    /// What the quests completed since the last call take back, in order
    /// (`CQuest::cleanUp`), for the renderer.
    std::vector<QuestItemRemoval> take_item_removals();

    /// Every completion since the last call, in order, for the renderer to pay.
    std::vector<QuestCompletion> take_completions();

    /// The progress lines the events since the last call put up, in order; a
    /// "Quest Complete" the same event raises comes after and replaces them.
    std::vector<QuestProgressText> take_progress_texts();

    /// The quests an accept or a completion made complete since the last call:
    /// what `CQuestManager::questEventUpdate` puts the banner up for.
    std::vector<const QuestDefinition*> take_finished();

    /// What `CQuestManager` has told the `CQuestController`s since the last
    /// call, in order.
    std::vector<QuestNotice> take_notices();

    /// Who answers for the player when a quest's requirements are tested.
    void set_player_source(QuestPlayerSource source) { player_source_ = std::move(source); }

    /// `CQuestRequirements::questRequirmentsHaveBeenMet`:
    /// whether `quest` may be offered now.
    bool requirements_met(const QuestDefinition& quest) const;

    /// `CQuestManager::getQuestForNPC`: which
    /// quest `unit` talks about. Candidates are the book's quests naming the unit
    /// that `valid_for_npc` accepts; one this unit gave the player wins outright;
    /// `QuestBook::random_people` is dropped from two or more, and set aside when
    /// alone with a `[QUESTS]` list; with none, `unit_quests`, then the one set
    /// aside. One of two or more is drawn with `roll` and initialised.
    const QuestDefinition* quest_for_npc(const std::string& unit,
                                         const std::vector<std::string>& unit_quests,
                                         RunicRandom& roll);

    /// `CQuestManager::getDialogsForNPC(list, unit, true)`,
    /// the first of the list, which is what
    /// `CQuestDialogMenu::setNPC` shows: a line from the player's own quests
    /// first, in the order they were taken, then from `quest_for_npc` (asked a
    /// second time when the first answer had no line).
    const DialogLine* dialog_for_npc(const std::string& unit,
                                     const std::vector<std::string>& unit_quests, RunicRandom& roll,
                                     const QuestDefinition** quest, QuestSection* section);

    /// `CQuest::getQuestDialog(CBaseUnit*)`:
    /// the line this quest has for the unit now, and which section it came
    /// from.
    const DialogLine* dialog_for(const QuestDefinition& quest, const std::string& unit,
                                 QuestSection* section = nullptr) const;

    /// `CQuestManager::calculateNPCIcon`: the
    /// quest marker over `unit`, as a unit theme. The four marker themes come
    /// off first; then, outside a cinematic, the line `quest_for_npc` gives
    /// picks one (`THEMEOVERRIDE`, else `QUEST PASSIVE` / `COMPLETE` /
    /// `INCOMPLETE` / `GIVING`), none with `ICONABOVEHEAD` off. False where the
    /// original returns 0, when `CLevel::updateNPCIcons` uses the vendors' themes.
    bool calculate_npc_icon(const std::string& unit, const std::vector<std::string>& unit_quests,
                            RunicRandom& roll, bool in_cinematic, bool shown, UnitThemeSet& themes,
                            const UnitThemeBook* book);

    /// `CQuest::getQuestIsValidForNPC`.
    bool valid_for_npc(const QuestDefinition& quest, const std::string& unit) const;

    /// `CQuest +0x1e8`: the unit the quest was last initialised with.
    std::string giver_of(const QuestDefinition& quest) const;

    /// Take the quest (`CQuestManager::giveQuest`); the dialog asks
    /// `log_full_for` first. `level` and `seed` are what `initializeQuestWithNPC`
    /// would have set. `depth`, when not negative, is the floor as a save holds
    /// it (`CQuest::load`); otherwise the offer's.
    /// `resolve_units` false: a quest given with no NPC keeps its units unresolved, as the
    /// manager's reset left them (its lookup had no level template); `populate` resolves them.
    bool accept(const QuestDefinition& quest, int level, uint32_t seed, const StatBook& graphs,
                int depth = -1, bool resolve_units = true);
    /// The quest dialog's refusal, before it accepts: a quest that takes a
    /// journal row while the six are taken (`CQuestDialogMenu::onClick`).
    bool log_full_for(const QuestDefinition& quest) const;

    /// Give up on it. Only a `CANABANDON` quest can be, which is what the tab's
    /// `Y` button is gated on.
    bool abandon(const std::string& name);

    /// The held-list half of `CQuestManager::removeQuest(quest, false)`
    /// and `resetQuest`:
    /// the entry dropped, nothing taken
    /// back, nobody told (the level script tells the controllers itself).
    void drop(const std::string& name);

    /// Mark it complete, run `[GIVE_QUESTS_ON_COMPLETE]` and the two
    /// `COMPLETE_ON_QUEST_*` lists, and report the reward to pay out.
    ///
    /// `CQuestManager::completeQuest`.
    /// Returns false for a quest the player has not got.
    bool complete(const std::string& name, QuestReward* paid = nullptr);

    /// `CQuestManager::completeQuest` on any
    /// quest, held or not: a held one through `complete`, one the player never
    /// took marked complete with the same `[COMPLETE_ON_QUEST_COMPLETE]` and
    /// `[GIVE_QUESTS_ON_COMPLETE]` consequences. False when already complete.
    bool complete_quest(const QuestDefinition& quest);

    /// `CQuestManager::completeQuest` as
    /// `CQuestDialogMenu::onClick` calls it for `OK` on a
    /// `[COMPLETE]` line: with no test of the quest's state, so a quest already
    /// complete is completed once more.
    bool complete_by_dialog(const QuestDefinition& quest);

    /// The consequences alone, for a quest a level script has already marked
    /// complete (`CQuestController::setQuestComplete`, which still pays, counts
    /// and cleans up through `completeQuest`).
    void apply_completion_consequences(const QuestDefinition& quest) {
        QuestEntry* entry = find(quest.name);
        const bool held = entry != nullptr && entry->status != QuestStatus::Complete;
        if (entry != nullptr) entry->status = QuestStatus::Complete;
        unhold(quest.name);
        queue_completion(quest, held ? entry : nullptr);
        queue_item_removals(quest, entry);
        completion_consequences(quest);
    }

    /// `CQuestManager::populate` for a level being built:
    /// `CQuest::populate` on every held quest, which runs
    /// `CQuestUnitData::populate` on its
    /// `[ACQUIRE]`, then `[DEFEAT]`, then `[POPULATE]` objectives.
    /// `create_unit` is `createUnit`, once per
    /// unit wanted. A `SPAWNFROMUNITTYPE` objective tags a unit already in the
    /// level instead (`QuestUnitWorld::units_of_type`), drawing on `g_Rand`.
    void populate_level(const std::string& level_dungeon, int level_depth,
                        const std::function<void(const QuestEntry&, const QuestObjective&,
                                                 const QuestUnitTag&)>& create_unit);

    /// The events the original raises on the quest manager
    /// (`CBaseUnit::questEventFire` -> `CQuestManager::questEventUpdate`
    /// > `CQuest::questEventUpdate`),
    /// by `EQUEST_EVENTS`: 0 picked up
    /// (`CEquipment::addedToInventory`), 1 defeated
    /// (`CCharacter::die`), 2 interacted
    /// (`CItem::interact`), 5 dropped
    /// (`CEquipment::removedFromInventory`). Returns the quests this
    /// event completed that have not had their banner yet.
    std::vector<const QuestDefinition*> unit_defeated(QuestEventUnit& unit);
    std::vector<const QuestDefinition*> item_picked_up(QuestEventUnit& unit);
    /// `by_player`: the character that interacted is the quest manager's player
    /// (`CQuest::questEventUpdate` case 2); a wire's `Trigger` has none.
    std::vector<const QuestDefinition*> unit_interacted(QuestEventUnit& unit, bool by_player);
    std::vector<const QuestDefinition*> item_dropped(QuestEventUnit& unit);
    /// The same with a bare name and tag, for a unit with no type or position.
    std::vector<const QuestDefinition*> unit_defeated(const std::string& unit_name,
                                                      const QuestUnitTag& tag = {});
    std::vector<const QuestDefinition*> item_picked_up(const std::string& item_name,
                                                       const QuestUnitTag& tag = {});

    /// The log, in the order quests were taken - which is the order the tab's
    /// six trays are filled in.
    // PORT: an accessor of the log.
    const std::vector<QuestEntry>& entries() const { return entries_; }
    // PORT: an accessor of the log.
    std::vector<QuestEntry>& entries() { return entries_; }

    /// Only the ones that occupy a tray: active, and with a journal entry.
    std::vector<const QuestEntry*> journal_entries() const;
    /// Whether a quest event since the last call asked for
    /// `CGameUI::refreshQuestMenu` (X360 inline,;
    /// the menu's rebuild).
    bool take_quest_menu_refresh() {
        const bool asked = quest_menu_refresh_;
        quest_menu_refresh_ = false;
        return asked;
    }

    const QuestEntry* find(const std::string& name) const;
    QuestEntry* find(const std::string& name);

    bool full() const { return static_cast<int>(journal_entries().size()) >= kQuestLogSize; }

    /// Whether an accepted `quest` takes one of the tab's six trays: a
    /// non-empty `[DETAILS]`, as `CQuestDialogMenu::onClick`'s count tests.
    ///
    static bool shows_in_journal(const QuestDefinition& quest);

    /// Whether taking `quest` requires a free tray. `FORCEACCEPT` waives the
    /// check, not the tray: a forced quest is never refused and still shows.
    ///
    static bool needs_a_free_tray(const QuestDefinition& quest);

    /// Throw the log away and put the gate state back to `state`.
    void reset(const QuestState& state);

    /// `CQuestManager::resetQuestManager(false)`,
    /// at a new game (`CGameClient::setGameState`), a load
    /// (`CQuestManager::load`, first) and a level loaded with no player yet
    /// (`CGameClient::loadLevel`): the held list emptied, completed flags
    /// cleared (names kept), and every quest re-rolled
    /// (`CQuest::reinitializeQuest(false, false)`) in the manager's map order,
    /// off its own seed or else `g_Rand`. `without_player`: floor `k` of the
    /// tries instead of `getMaxDepth + k`.
    void reset_quest_manager(bool without_player);

    /// `CQuestDialog +0x77`: whether `giveOrRemoveItems`
    /// has run for this line of `quest`; it runs once
    /// (`CQuestDialog::save` keeps the flag).
    bool items_handed(const QuestDefinition& quest, const DialogLine& line) const;
    void set_items_handed(const QuestDefinition& quest, const DialogLine& line);

    /// The log with its quests named rather than pointed at, to survive a warp
    /// that rebuilds the book (`restore`).
    struct Carried {
        QuestState state;
        std::set<std::string> items_handed;
        std::vector<std::pair<std::string, QuestEntry>> entries;
        /// The held quests in the manager's order (`held_`).
        std::vector<std::string> held;
        std::map<std::string, std::string> givers;
        std::map<std::string, Offered> offers;
        /// The quests not held, as the manager keeps them (`dormant_`).
        std::vector<std::pair<std::string, QuestEntry>> dormant;
    };
    Carried carry() const;

    /// Take a carried log back against the current book. An entry whose quest
    /// the book no longer has is dropped.
    void restore(const Carried& carried);

    /// The held quests, `CQuestManager +0x0c` in its order.
    std::vector<const QuestEntry*> held_entries() const;
    /// `CQuest::isComplete(false)`.
    bool is_complete(const QuestDefinition& quest) const;

private:
    /// The held quests' order (`CQuestManager +0x0c`), by upper-cased name: a
    /// quest given goes on the end, one completed or removed has the last put
    /// in its place (`completeQuest`, `removeQuest`).
    std::vector<std::string> held_;
    void hold(const std::string& name);
    void unhold(const std::string& name);
    std::vector<QuestEntry*> held_entries_mutable();
    /// `CQuestManager::questEventUpdate(3 or 4, 0, 0)`: the
    /// held quests' lists of names flagged, the ones that completes queued for
    /// `take_finished`. `completing` is a quest event 4 is sent for that the
    /// completed map does not hold yet.
    void broadcast_quest_event(int event, const std::string& completing = std::string());
    /// `CQuestManager::getQuestIsActive`.
    bool quest_is_active(const std::string& name) const;
    /// `CQuestManager::getQuestComplete`.
    bool quest_complete(const std::string& name) const;
    std::vector<const QuestDefinition*> finished_;
    /// What completing `quest` sets off in other quests.
    void completion_consequences(const QuestDefinition& quest);
    /// `CQuest::cleanUp(true)`, the inventory
    /// half, queued.
    void queue_item_removals(const QuestDefinition& quest, const QuestEntry* entry,
                             bool completed = true);
    std::vector<QuestItemRemoval> item_removals_;
    std::vector<QuestNotice> notices_;
    std::vector<QuestProgressText> progress_texts_;
    /// Queue a notice for `quest`'s controllers.
    void notify(const std::string& quest, std::vector<uint32_t> outputs);
    /// `completeQuest` for `quest`: queued for `take_completions`.
    void queue_completion(const QuestDefinition& quest, const QuestEntry* entry);
    std::vector<QuestCompletion> completions_;
    /// `CQuest +0x26`, the quest accepted.
    bool accepted(const QuestDefinition& quest) const;
    /// `CQuestDialog::dialogIsForNPC`: the line's `UNITNAME`, or
    /// with none the quest's giver.
    bool line_is_for(const QuestDefinition& quest, const DialogLine& line,
                     const std::string& unit) const;
    /// `CQuest::initializeQuestWithNPC` with
    /// a unit.
    void initialize_with_npc(const QuestDefinition& quest, const std::string& unit,
                             RunicRandom& roll);
    /// `CQuestManager::getPlayerHasQuest` (the console inlines
    /// it): the quest is on the player's list.
    bool player_has_quest(const QuestDefinition& quest) const;
    /// `CQuest::reinitializeQuest(.., false)`, what an offer
    /// keeps of it: the floor, the exhausted flag and the `[DEFEAT]` groups.
    void reinitialize_offer(const QuestDefinition& quest, Offered& rolled);

    /// Quest name (upper-cased) to giver (upper-cased).
    std::map<std::string, std::string> givers_;

    /// `CQuestManager::questEventUpdate` for
    /// one event.
    std::vector<const QuestDefinition*> quest_event(int event, QuestEventUnit& unit,
                                                    bool by_player);
    bool quest_menu_refresh_ = false;

    const QuestBook* book_ = nullptr;
    QuestUnitWorld unit_world_;
    QuestRewardWorld reward_world_;
    void rewards_paid(const QuestDefinition& quest);
    /// `CQuestUnitData::populate`'s `SPAWNFROMUNITTYPE` branch: one unit of the
    /// level of `type` tagged with `tag` (see `populate_level`).
    void tag_unit_of_type(uint32_t type, const QuestUnitTag& tag);
    QuestUnitNamer unit_namer_;
    QuestDungeonSource dungeon_source_;
    const RandomNames* random_names_ = nullptr;
    std::map<std::string, Offered> offers_;  ///< by upper-cased quest name
    /// The quests the player does not hold, as the manager keeps each (`CQuest` with its
    /// `CQuestUnitData`): made the first time a level is populated, by upper-cased name.
    std::map<std::string, QuestEntry> dormant_;
    /// A dormant quest's state, made once (`dormant_`).
    QuestEntry& dormant_entry(const QuestDefinition& quest);
    using CreateQuestUnit =
        std::function<void(const QuestEntry&, const QuestObjective&, const QuestUnitTag&)>;
    /// One objective of `populate_level` that is on the level.
    void populate_objective(QuestEntry& entry, QuestObjective& objective, bool active,
                            const CreateQuestUnit& create_unit);
    struct DormantSeed {
        uint32_t seed = 0;
        int depth = 0;
        int level = 1;
    };
    /// Seeds handed in for dormant quests (`seed_dormant`), by upper-cased name.
    std::map<std::string, DormantSeed> dormant_seeds_;
    QuestState state_;
    std::vector<QuestEntry> entries_;
    QuestPlayerSource player_source_;
    std::set<std::string> items_handed_;  ///< `line_key`s

    static std::string line_key(const QuestDefinition& quest, const DialogLine& line);
};

/// `CLevel::updateNPCIcons`, for one unit of
/// the level's list: a unit that is `shown` (`getEnabled() || !+0x70d`) and not
/// pending deletion (`+0x190`) gets its quest marker from `calculate_npc_icon`,
/// and when that finds none, the first vendor theme its type earns - `GAMBLER`,
/// `ENCHANTER`, `TRANSMUTE`, `GEMRETRIEVE`, `GEMRECOVER`, `MERCHANT`, in that
/// order. A vendor theme is never taken off.
void update_npc_icon(QuestLog& log, const std::string& unit,
                     const std::vector<std::string>& unit_quests, uint32_t type,
                     const UnitTypeHierarchy& types, RunicRandom& roll, bool in_cinematic,
                     bool shown, bool pending_delete, UnitThemeSet& themes,
                     const UnitThemeBook* book);

/// Roll a quest's three reward figures: `CQuestRewards::calculateRewards`.
/// `g_Rand` (`rng`, the game's when null) is
/// seeded with `seed + 0x272b`; the gold, fame, then XP
/// percentages are drawn, each figure `ceil(curve(level + 1) * pct / 100)`. The
/// experience curve takes no difficulty suffix.
QuestReward calculate_quest_reward(const QuestRewardSpec& spec, int level, uint32_t seed,
                                   const StatBook& graphs, int difficulty = 1,
                                   RunicRandom* rng = nullptr);

/// Substitute the `[TAG]`s a journal entry carries: `CQuest::replaceStringTags`
/// with `getAcquireTags`.
/// `<i>` and `<j>` count from 1; `[DUNEGONNAME]` (their
/// spelling) is `entry.dungeon_display_name`.
std::string replace_quest_tags(const std::string& text, const QuestEntry& entry);

/// Pick one of a dialogue line's alternative strings: `CQuestDialog::getDialog(bool)`.
/// With fewer than two or no quest, the
/// first, with no draw; else `rng` is reseeded with the quest's seed unless it
/// is -1, `integer_between(1, n) - 1` drawn, and the quest's tags replaced.
std::string get_dialog(const DialogLine& line, const QuestEntry* quest, RunicRandom& rng);

}  // namespace tl::core
