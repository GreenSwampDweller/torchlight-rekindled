// The character save file and the level states inside it.
// `CGameClient::saveCharacter`,
// `CGameClient::loadCharacter`.
// Each member names the original offset it mirrors. Strings: UTF-8 here, UTF-16LE in the file.
#pragma once

#include "formats/byte_reader.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace tl::formats {

/// `m_gFileVersion`, the version every record is written at.
inline constexpr uint32_t kSaveFileVersion = 23;

struct SaveVec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

/// One saved `CEffect`, as `CCharacterSaveState::save`
/// writes it.
struct EffectSaveState {
    std::array<std::string, 4> names;  ///< +0x80, +0x88 and two temporaries
    uint8_t b31 = 0;
    int32_t i1c = 0;
    int32_t i14 = 0;
    uint8_t b33 = 0;
    uint8_t b32 = 0;
    uint8_t b30 = 0;
    uint8_t b37 = 0;
    int32_t i20 = 0;
    int32_t i10 = 0;
    int64_t theme_guid = -1;  ///< the unit theme's (+0x130) +0x30 GUID, or -1
    int32_t i3c = 0;
    int32_t i40 = 0;
    int32_t i44 = 0;
    int32_t i24 = 0;  ///< `(int)` of the float +0x24
    int32_t i18 = 0;
    int32_t i38 = 0;
    int32_t ic0 = 0;
    std::array<int32_t, 4> ic4{};
};

using EffectLists = std::array<std::vector<EffectSaveState>, 3>;

/// `CItemSaveState` (0x180 bytes; `CItemSaveState::CItemSaveState`,
/// `save`).
struct ItemSaveState {
    int64_t unit_guid = -1;                 ///< +0x60; -1 is gold
    std::string name;                       ///< +0x10
    std::string s18;                        ///< +0x18
    std::string s20;                        ///< +0x20
    int64_t object_id = -1;                 ///< +0x30
    int64_t hierarchy = -1;                 ///< +0x40
    uint8_t from_layout = 0;                ///< +0x48 (`CBaseUnit+0x191`)
    int64_t parent_id = -1;                 ///< +0x38
    int64_t spawner_id = 0;                 ///< +0x50
    int32_t i28 = 0;                        ///< +0x28
    int32_t i6c = 0;                        ///< +0x6c
    int32_t slot = 0;                       ///< +0x58
    uint8_t b5c = 0;                        ///< +0x5c
    uint8_t enabled = 0;                    ///< +0x5d
    uint8_t collideable = 0;                ///< +0x5e (`+0x18d`)
    uint8_t snaps_to_ground = 1;            ///< +0x17c (`+0x1f2`), 1 from the constructor
    uint8_t b74 = 0;                        ///< +0x74
    uint8_t b75 = 0;                        ///< +0x75
    SaveVec3 position;                      ///< +0x84
    SaveVec3 centre;                        ///< +0x90
    std::array<uint8_t, 64> orientation{};  ///< +0x9c
    int32_t value = 0;  ///< +0x68: trigger state (`+0x248`), gold amount (`+0x22c`)
    int32_t i70 = 0;    ///< +0x70
    std::vector<ItemSaveState> sockets;  ///< +0x128
    int32_t i78 = -1;                    ///< +0x78
    int32_t i7c = -1;                    ///< +0x7c
    int32_t room = -1;                   ///< +0x80
    int64_t i170 = 0;                    ///< +0x170
    int32_t i178 = 0;                    ///< +0x178
    /// +0x17c, the version: `CItemSaveState::load` sets the file's; a record that never left memory
    /// keeps what its heap held (the constructor leaves it).
    uint8_t version = 0;
    std::vector<std::pair<float, int32_t>> pairs;  ///< (+0x158 as float, +0x140)
    EffectLists effects;                           ///< +0xe0, +0xf8, +0x110
};

/// `CCharacterSaveState` (0x228 bytes; `CCharacterSaveState::CCharacterSaveState`,
/// `save`).
/// Defaults are the constructor's.
struct CharacterSaveState {
    uint8_t mesh_hidden = 0;                ///< +0x20c (`+0x70d`)
    int64_t unit_guid = -1;                 ///< +0x20 (`+0x1a0`)
    int64_t transformed_from = -1;          ///< +0x28 (`+0x690`), the GUID of the unit it first was
    int64_t object_id = -1;                 ///< +0x10 (`+0x20`)
    uint8_t from_layout = 0;                ///< +0x18 (`+0x191`)
    uint8_t b30 = 0;                        ///< +0x30
    uint8_t enabled = 0;                    ///< +0x31 (vtable +0x48)
    uint8_t champion = 0;                   ///< +0x32 (`+0x702`)
    uint8_t b33 = 0;                        ///< +0x33 (`+0x70e`)
    uint8_t b34 = 0;                        ///< +0x34 (`CPlayer+0x791`)
    float transform_left = 0.0f;            ///< +0x17c (`+0x688`), a transformation's seconds left
    float f180 = 0.0f;                      ///< +0x180 (`+0x684`)
    int32_t i48 = 0;                        ///< +0x48 (`+0x710`)
    int32_t alignment = 0;                  ///< +0x178 (`+0x338`)
    float scale = 0.0f;                     ///< +0x38 (`+0x98`)
    int64_t master_id = -1;                 ///< +0x50
    int64_t spawner_id = -1;                ///< +0x58 (`+0x180`)
    int32_t game_seed = -1;                 ///< +0x60
    std::string name;                       ///< +0x40
    int32_t depth = 0;                      ///< +0x64
    int32_t deepest_depth = 0;              ///< +0x68 (`CPlayer+0x7d8`)
    SaveVec3 position;                      ///< +0x6c
    std::array<uint8_t, 64> orientation{};  ///< +0x78
    int32_t level = 0;                      ///< +0xd0 (`+0x100`)
    int32_t id4 = 0;                        ///< +0xd4 (`+0x448`)
    int32_t id8 = 0;                        ///< +0xd8 (`+0x450`)
    int32_t idc = 0;                        ///< +0xdc (`+0x44c`)
    float hp = 0.0f;                        ///< +0xe0 (`+0x414`)
    int32_t base_max_hp = 0;                ///< +0xe4 (`+0x418`)
    int32_t permanent_hp = 0;               ///< +0xe8 (`+0x41c`, `PERMANENT HEALTH`)
    int32_t iec = 0;                        ///< +0xec (`+0x438`)
    int32_t if0 = 0;                        ///< +0xf0 (`+0x43c`)
    int32_t permanent_mana = 0;             ///< +0xf4 (`(int)+0x440`, `PERMANENT MANA`)
    int32_t i100 = 0;                       ///< +0x100 (`+0x454`)
    int32_t i104 = 0;                       ///< +0x104 (`+0x458`)
    float clock = 0.0f;                     ///< +0xf8 (`+0x388`, play time for a player)
    int32_t ifc = 0;                        ///< +0xfc (`+0x668`)
    std::array<int32_t, 3> i108{};          ///< +0x108 .. +0x110 (`+0x45c` .. `+0x464`)
    std::array<int64_t, 3> ib8{-1, -1, -1};  ///< +0xb8, +0xc0, +0xc8
    std::vector<int32_t> skill_levels;       ///< +0x1e8
    std::array<std::string, 4> spells;       ///< +0x118
    std::array<int32_t, 4> spell_levels{};   ///< +0x138
    std::array<int32_t, 7> i148{};           ///< +0x148 (`+0x620`)
    std::array<int32_t, 5> i164{};           ///< +0x164 .. +0x174
    int32_t room = -1;                       ///< +0x184
    int64_t i200 = -1;                       ///< +0x200 (`+0x170`)
    int32_t i208 = -1;                       ///< +0x208 (`+0x178`)
    std::vector<std::string> mods;           ///< +0x210
    std::vector<ItemSaveState> inventory;    ///< +0x188
    EffectLists effects;                     ///< +0x1a0, +0x1b8, +0x1d0
};

/// `CLogicNodeState` (0x60 bytes; `CLogicNodeState::save`
/// `CLogicNodeState::load`).
///
struct LogicNodeState {
    std::array<uint8_t, 5> flags{};  ///< +0x10 .. +0x14
    int32_t i18 = 0;                 ///< +0x18
    int32_t i1c = 0;                 ///< +0x1c
    float f20 = 0.0f;                ///< +0x20
    float f24 = 0.0f;                ///< +0x24 (raw four bytes for a spawner)
    std::string descriptor;          ///< +0x28
    uint32_t scene = 0;              ///< +0x30
    int64_t parent_id = -1;          ///< +0x38
    int64_t hierarchy = 0;           ///< +0x40
    int64_t object_id = 0;           ///< +0x48
    SaveVec3 position;               ///< +0x50
};

/// `CFormationNodeSaveAndLoad` (0x38 bytes;
/// `CFormationNodeSaveAndLoad::saveFormationNode`,
/// `CFormationNodeSaveAndLoad::loadFormationNode`).
struct FormationSaveState {
    uint8_t pending = 0;  ///< +0x10, `!CFormationNode+0x108`
    std::string layout;   ///< +0x18
    SaveVec3 position;    ///< +0x2c
};

/// `CLevelState` (0xb8 bytes; `save`, `load`),
/// at `CLevelState::CLevelState(depth)`'s values:
/// the depth given, no dungeon, no time, no
/// lists, no automap, neither a town nor holding a quest item.
struct LevelSaveState {
    int32_t depth = 0;                           ///< +0x10
    float time = 0.0f;                           ///< +0x98
    std::string dungeon;                         ///< +0x18
    uint8_t is_town = 0;                         ///< +0xb0
    uint8_t holds_quest_item = 0;                ///< +0xb1
    int32_t automap_width = 0;                   ///< +0x9c
    int32_t automap_height = 0;                  ///< +0xa0
    std::vector<float> automap;                  ///< +0xa8
    std::vector<CharacterSaveState> characters;  ///< +0x20
    std::vector<ItemSaveState> items;            ///< +0x38
    std::vector<LogicNodeState> logic;           ///< +0x50
    std::vector<std::string> chunks;             ///< +0x68
    std::vector<FormationSaveState> formations;  ///< +0x80
};

/// `CDungeonTracker` (0x30 bytes; `save`).
struct DungeonTrackerSave {
    std::string name;           ///< +0x00
    int32_t player_level = 0;   ///< +0x0c
    int32_t deepest = 0;        ///< +0x08
    int32_t salt = 0;           ///< +0x10
    std::vector<int32_t> ints;  ///< +0x18
};

/// `CQuestUnitData::save`.
struct QuestUnitSave {
    uint8_t complete = 0;
    uint8_t bb9 = 0;
    int32_t i38 = 0;
    int32_t i3c = 0;
    int32_t i10 = 0;
    int64_t i30 = 0;
    std::vector<QuestUnitSave> children;
};

/// `CQuest::save`.
struct QuestSave {
    std::string name;  ///< written by the manager before the record
    int64_t giver_guid = -1;
    int32_t seed = 0;   ///< +0x1c
    int32_t depth = 0;  ///< +0x20
    int32_t level = 0;  ///< +0x20c
    /// Four dialog sections, two bytes per dialog: +0x76 interacted, +0x77.
    std::array<std::vector<std::array<uint8_t, 2>>, 4> dialogs;
    /// Three objective lists, each a list of groups of unit records.
    std::array<std::vector<std::vector<QuestUnitSave>>, 3> objectives;
};

/// `CQuestManager::save`.
struct QuestManagerSave {
    std::vector<std::string> completed;
    std::vector<QuestSave> active;
};

/// The whole file, `saveCharacter`'s order.
struct CharacterFile {
    uint32_t version = kSaveFileVersion;
    std::string class_name;   ///< `CPlayer+0x40`
    uint32_t difficulty = 0;  ///< `CGameClient+0x38ec`
    uint8_t hardcore = 0;     ///< `CPlayer+0xa15`
    uint32_t generation = 0;  ///< `CPlayer+0xa10`, retirements before this character
    uint8_t retired = 0;      ///< `CPlayer+0xa14`
    CharacterSaveState player;
    std::vector<int64_t> skill_slots = std::vector<int64_t>(10, -1);  ///< `+0x8b0`
    /// `+0x950`, `+0x9b0`
    std::vector<std::pair<int64_t, int64_t>> mapped_skills =
        std::vector<std::pair<int64_t, int64_t>>(12, {-1, -1});
    std::vector<int64_t> second_slots = std::vector<int64_t>(10, -1);  ///< `+0x900`
    std::array<uint32_t, 18> journal{};                                ///< `+0x7dc`
    std::string current_dungeon;                                       ///< `CLevel+0x280`
    std::string last_dungeon;                                          ///< `CPlayer+2000`
    int32_t last_dungeon_depth = 0;                                    ///< `CPlayer+0x7cc`
    uint8_t portal_open = 0;                                           ///< `CPlayer+0x7b0`
    SaveVec3 portal_position;                                          ///< `CPlayer+0x7c0`
    std::string portal_dungeon;                                        ///< `CPlayer+0x7b8`
    int32_t portal_depth = 0;                                          ///< `CPlayer+0x7b4`
    std::vector<CharacterSaveState> pets;                              ///< `CPlayer+0x648`
    std::array<uint8_t, 34> flags{};                                   ///< `CPlayer+0xa17`
    std::vector<LevelSaveState> levels;                                ///< `CPlayer+0x798`
    std::vector<DungeonTrackerSave> trackers;                          ///< `CPlayer+0xa40`
    bool has_quests = true;
    QuestManagerSave quests;
};

void write_item_state(ByteWriter& out, const ItemSaveState& item);
void write_character_state(ByteWriter& out, const CharacterSaveState& character);
void write_logic_state(ByteWriter& out, const LogicNodeState& state);
void write_level_state(ByteWriter& out, const LevelSaveState& level);

/// Read back what the writers above produce. Version 23 only: the gates for
/// older files are not transcribed, since this engine never wrote one.
bool read_item_state(ByteReader& in, ItemSaveState& item);
bool read_character_state(ByteReader& in, CharacterSaveState& character);
bool read_logic_state(ByteReader& in, LogicNodeState& state);
bool read_level_state(ByteReader& in, LevelSaveState& level);

/// `saveCharacter`'s body, ending with the file size (`ftell + 4`).
std::vector<uint8_t> write_character_file(const CharacterFile& file);

/// `loadCharacter`: false when the last four bytes are not the file's size, the
/// version is not 23, or the bytes run out.
bool read_character_file(const uint8_t* data, size_t size, CharacterFile& file);

/// `CSharedStash::saveSharedStash`:
/// `m_gFileVersion`, the number of items (`CInventory +0x38`), then each item's
/// `CItemSaveState` (its `fillSaveState` with the item's slot, `+0x58`). No
/// size trailer.
std::vector<uint8_t> write_shared_stash_file(const std::vector<ItemSaveState>& items);

/// `CSharedStash::loadSharedStash`'s reading
/// half: false when the version is not 23 or the bytes run out.
bool read_shared_stash_file(const uint8_t* data, size_t size, std::vector<ItemSaveState>& items);

}  // namespace tl::formats
