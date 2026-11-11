#include "formats/save_file.h"

#include <algorithm>

namespace tl::formats {

namespace {

// `WriteUTF32ToUTF16` after a u16 length in UTF-16 units.
void write_string16(ByteWriter& out, const std::string& text) {
    const std::vector<uint16_t> units = utf8_to_utf16(text);
    out.u16(static_cast<uint16_t>(units.size()));
    for (uint16_t unit : units) out.u16(unit);
}

std::string read_string16(ByteReader& in) {
    const uint16_t length = in.u16();
    return in.utf16le(length);
}

void write_vec3(ByteWriter& out, const SaveVec3& v) {
    out.f32(v.x);
    out.f32(v.y);
    out.f32(v.z);
}

SaveVec3 read_vec3(ByteReader& in) {
    SaveVec3 v;
    v.x = in.f32();
    v.y = in.f32();
    v.z = in.f32();
    return v;
}

void write_effect(ByteWriter& out, const EffectSaveState& e) {
    for (const std::string& name : e.names) write_string16(out, name);
    out.u8(e.b31);
    out.i32(e.i1c);
    out.i32(e.i14);
    out.u8(e.b33);
    out.u8(e.b32);
    out.u8(e.b30);
    out.u8(e.b37);
    out.i32(e.i20);
    out.i32(e.i10);
    out.i64(e.theme_guid);
    out.i32(e.i3c);
    out.i32(e.i40);
    out.i32(e.i44);
    out.i32(e.i24);
    out.i32(e.i18);
    out.i32(e.i38);
    out.i32(e.ic0);
    for (int32_t v : e.ic4) out.i32(v);
}

void read_effect(ByteReader& in, EffectSaveState& e) {
    for (std::string& name : e.names) name = read_string16(in);
    e.b31 = in.u8();
    e.i1c = in.i32();
    e.i14 = in.i32();
    e.b33 = in.u8();
    e.b32 = in.u8();
    e.b30 = in.u8();
    e.b37 = in.u8();
    e.i20 = in.i32();
    e.i10 = in.i32();
    e.theme_guid = in.i64();
    e.i3c = in.i32();
    e.i40 = in.i32();
    e.i44 = in.i32();
    e.i24 = in.i32();
    e.i18 = in.i32();
    e.i38 = in.i32();
    e.ic0 = in.i32();
    for (int32_t& v : e.ic4) v = in.i32();
}

void write_effects(ByteWriter& out, const EffectLists& lists) {
    for (const std::vector<EffectSaveState>& list : lists) {
        out.u32(static_cast<uint32_t>(list.size()));
        for (const EffectSaveState& e : list) write_effect(out, e);
    }
}

bool read_effects(ByteReader& in, EffectLists& lists) {
    for (std::vector<EffectSaveState>& list : lists) {
        const uint32_t count = in.u32();
        if (!in.ok() || count > in.remaining()) return false;
        list.resize(count);
        for (EffectSaveState& e : list) read_effect(in, e);
    }
    return in.ok();
}

// `CDungeonTracker::save` / `CDungeonTracker::load`.
//
void write_tracker(ByteWriter& out, const DungeonTrackerSave& t) {
    write_string16(out, t.name);
    out.i32(t.player_level);
    out.i32(t.deepest);
    out.i32(t.salt);
    out.u32(static_cast<uint32_t>(t.ints.size()));
    for (int32_t v : t.ints) out.i32(v);
}

bool read_tracker(ByteReader& in, DungeonTrackerSave& t) {
    t.name = read_string16(in);
    t.player_level = in.i32();
    t.deepest = in.i32();
    t.salt = in.i32();
    const uint32_t count = in.u32();
    if (!in.ok() || count > in.remaining()) return false;
    t.ints.resize(count);
    for (int32_t& v : t.ints) v = in.i32();
    return in.ok();
}

// `CQuestUnitData::save` / `load`.
//
void write_quest_unit(ByteWriter& out, const QuestUnitSave& u) {
    out.u8(u.complete);
    out.u8(u.bb9);
    out.i32(u.i38);
    out.i32(u.i3c);
    out.i32(u.i10);
    out.i64(u.i30);
    out.u32(static_cast<uint32_t>(u.children.size()));
    for (const QuestUnitSave& child : u.children) write_quest_unit(out, child);
}

bool read_quest_unit(ByteReader& in, QuestUnitSave& u, int depth) {
    if (depth > 16) return false;
    u.complete = in.u8();
    u.bb9 = in.u8();
    u.i38 = in.i32();
    u.i3c = in.i32();
    u.i10 = in.i32();
    u.i30 = in.i64();
    const uint32_t count = in.u32();
    if (!in.ok() || count > in.remaining()) return false;
    u.children.resize(count);
    for (QuestUnitSave& child : u.children) {
        if (!read_quest_unit(in, child, depth + 1)) return false;
    }
    return in.ok();
}

// `CQuest::save`.
void write_quest(ByteWriter& out, const QuestSave& q) {
    out.i64(q.giver_guid);
    out.i32(q.seed);
    out.i32(q.depth);
    out.i32(q.level);
    for (const auto& section : q.dialogs) {
        out.u32(static_cast<uint32_t>(section.size()));
        for (const auto& dialog : section) {
            out.u8(dialog[0]);
            out.u8(dialog[1]);
        }
    }
    for (const auto& list : q.objectives) {
        out.u32(static_cast<uint32_t>(list.size()));
        for (const std::vector<QuestUnitSave>& group : list) {
            out.u32(static_cast<uint32_t>(group.size()));
            for (const QuestUnitSave& unit : group) write_quest_unit(out, unit);
        }
    }
}

bool read_quest(ByteReader& in, QuestSave& q) {
    q.giver_guid = in.i64();
    q.seed = in.i32();
    q.depth = in.i32();
    q.level = in.i32();
    for (auto& section : q.dialogs) {
        const uint32_t count = in.u32();
        if (!in.ok() || count > in.remaining()) return false;
        section.resize(count);
        for (auto& dialog : section) {
            dialog[0] = in.u8();
            dialog[1] = in.u8();
        }
    }
    for (auto& list : q.objectives) {
        const uint32_t groups = in.u32();
        if (!in.ok() || groups > in.remaining()) return false;
        list.resize(groups);
        for (std::vector<QuestUnitSave>& group : list) {
            const uint32_t units = in.u32();
            if (!in.ok() || units > in.remaining()) return false;
            group.resize(units);
            for (QuestUnitSave& unit : group) {
                if (!read_quest_unit(in, unit, 0)) return false;
            }
        }
    }
    return in.ok();
}

// `CQuestManager::save`: the completed names,
// then each active quest behind an offset placeholder that is patched with the
// offset after it.
void write_quests(ByteWriter& out, const QuestManagerSave& quests) {
    out.u32(static_cast<uint32_t>(quests.completed.size()));
    for (const std::string& name : quests.completed) write_string16(out, name);
    out.u32(static_cast<uint32_t>(quests.active.size()));
    for (const QuestSave& quest : quests.active) {
        const size_t placeholder = out.size();
        out.u32(static_cast<uint32_t>(placeholder));
        write_string16(out, quest.name);
        write_quest(out, quest);
        out.patch_u32(placeholder, static_cast<uint32_t>(out.size()));
    }
}

// `CQuestManager::load`.
bool read_quests(ByteReader& in, QuestManagerSave& quests) {
    const uint32_t completed = in.u32();
    if (!in.ok() || completed > in.remaining()) return false;
    quests.completed.resize(completed);
    for (std::string& name : quests.completed) name = read_string16(in);
    const int32_t active = in.i32();
    if (!in.ok() || active < 0 || static_cast<uint32_t>(active) > in.remaining()) return false;
    quests.active.resize(static_cast<size_t>(active));
    for (QuestSave& quest : quests.active) {
        const uint32_t end = in.u32();
        quest.name = read_string16(in);
        if (!read_quest(in, quest)) return false;
        if (in.offset() != end) return false;
    }
    return in.ok();
}

}  // namespace

// `CItemSaveState::save`.
void write_item_state(ByteWriter& out, const ItemSaveState& item) {
    out.i64(item.unit_guid);
    write_string16(out, item.name);
    write_string16(out, item.s18);
    write_string16(out, item.s20);
    out.i64(item.object_id);
    out.i64(item.hierarchy);
    out.u8(item.from_layout);
    out.i64(item.parent_id);
    out.i64(item.spawner_id);
    out.i32(item.i28);
    out.i32(item.i6c);
    out.i32(item.slot);
    out.u8(item.b5c);
    out.u8(item.enabled);
    out.u8(item.collideable);
    out.u8(item.snaps_to_ground);
    out.u8(item.b74);
    out.u8(item.b75);
    write_vec3(out, item.position);
    write_vec3(out, item.centre);
    out.raw(item.orientation.data(), item.orientation.size());
    out.i32(item.value);
    out.i32(item.i70);
    out.u32(static_cast<uint32_t>(item.sockets.size()));
    for (const ItemSaveState& socket : item.sockets) write_item_state(out, socket);
    out.i32(item.i78);
    out.i32(item.i7c);
    out.i32(item.room);
    out.i64(item.i170);
    out.i32(item.i178);
    out.u16(static_cast<uint16_t>(item.pairs.size()));
    for (const auto& pair : item.pairs) {
        out.f32(pair.first);
        out.i32(pair.second);
    }
    write_effects(out, item.effects);
}

bool read_item_state(ByteReader& in, ItemSaveState& item) {
    // `CItemSaveState::load` first keeps the file's version (+0x17c).
    item.version = static_cast<uint8_t>(kSaveFileVersion);
    item.unit_guid = in.i64();
    item.name = read_string16(in);
    item.s18 = read_string16(in);
    item.s20 = read_string16(in);
    item.object_id = in.i64();
    item.hierarchy = in.i64();
    item.from_layout = in.u8();
    item.parent_id = in.i64();
    item.spawner_id = in.i64();
    item.i28 = in.i32();
    item.i6c = in.i32();
    item.slot = in.i32();
    item.b5c = in.u8();
    item.enabled = in.u8();
    item.collideable = in.u8();
    item.snaps_to_ground = in.u8();
    item.b74 = in.u8();
    item.b75 = in.u8();
    item.position = read_vec3(in);
    item.centre = read_vec3(in);
    in.bytes(item.orientation.data(), item.orientation.size());
    item.value = in.i32();
    item.i70 = in.i32();
    const uint32_t sockets = in.u32();
    if (!in.ok() || sockets > in.remaining()) return false;
    item.sockets.resize(sockets);
    for (ItemSaveState& socket : item.sockets) {
        if (!read_item_state(in, socket)) return false;
    }
    item.i78 = in.i32();
    item.i7c = in.i32();
    item.room = in.i32();
    item.i170 = in.i64();
    item.i178 = in.i32();
    const uint16_t pairs = in.u16();
    item.pairs.resize(pairs);
    for (auto& pair : item.pairs) {
        pair.first = in.f32();
        pair.second = in.i32();
    }
    return read_effects(in, item.effects);
}

// `CCharacterSaveState::save`.
void write_character_state(ByteWriter& out, const CharacterSaveState& c) {
    out.u8(c.mesh_hidden);
    out.i64(c.unit_guid);
    out.i64(c.transformed_from);
    out.i64(c.object_id);
    out.u8(c.from_layout);
    out.u8(c.b30);
    out.u8(c.enabled);
    out.u8(c.champion);
    out.u8(c.b33);
    out.u8(c.b34);
    out.f32(c.transform_left);
    out.f32(c.f180);
    out.i32(c.i48);
    out.i32(c.alignment);
    out.f32(c.scale);
    out.i64(c.master_id);
    out.i64(c.spawner_id);
    out.i32(c.game_seed);
    write_string16(out, c.name);
    out.i32(c.depth);
    out.i32(c.deepest_depth);
    write_vec3(out, c.position);
    out.raw(c.orientation.data(), c.orientation.size());
    out.i32(c.level);
    out.i32(c.id4);
    out.i32(c.id8);
    out.i32(c.idc);
    out.f32(c.hp);
    out.i32(c.base_max_hp);
    out.i32(c.permanent_hp);
    out.i32(c.iec);
    out.i32(c.if0);
    out.i32(c.permanent_mana);
    out.i32(c.i100);
    out.i32(c.i104);
    out.f32(c.clock);
    out.i32(c.ifc);
    for (int32_t v : c.i108) out.i32(v);
    for (int64_t v : c.ib8) out.i64(v);
    out.u32(static_cast<uint32_t>(c.skill_levels.size()));
    for (int32_t v : c.skill_levels) out.i32(v);
    for (size_t i = 0; i < c.spells.size(); ++i) {
        write_string16(out, c.spells[i]);
        out.i32(c.spell_levels[i]);
    }
    for (int32_t v : c.i148) out.i32(v);
    for (int32_t v : c.i164) out.i32(v);
    out.i32(c.room);
    out.i64(c.i200);
    out.i32(c.i208);
    out.u32(static_cast<uint32_t>(c.mods.size()));
    for (const std::string& mod : c.mods) write_string16(out, mod);
    out.u32(static_cast<uint32_t>(c.inventory.size()));
    for (const ItemSaveState& item : c.inventory) write_item_state(out, item);
    write_effects(out, c.effects);
}

// `CCharacterSaveState::load` at version 23 only;
// its gates for older files are not taken.
bool read_character_state(ByteReader& in, CharacterSaveState& c) {
    c.mesh_hidden = in.u8();
    c.unit_guid = in.i64();
    c.transformed_from = in.i64();
    c.object_id = in.i64();
    c.from_layout = in.u8();
    c.b30 = in.u8();
    c.enabled = in.u8();
    c.champion = in.u8();
    c.b33 = in.u8();
    c.b34 = in.u8();
    c.transform_left = in.f32();
    c.f180 = in.f32();
    c.i48 = in.i32();
    c.alignment = in.i32();
    c.scale = in.f32();
    c.master_id = in.i64();
    c.spawner_id = in.i64();
    c.game_seed = in.i32();
    c.name = read_string16(in);
    c.depth = in.i32();
    c.deepest_depth = in.i32();
    c.position = read_vec3(in);
    in.bytes(c.orientation.data(), c.orientation.size());
    c.level = in.i32();
    c.id4 = in.i32();
    c.id8 = in.i32();
    c.idc = in.i32();
    c.hp = in.f32();
    c.base_max_hp = in.i32();
    c.permanent_hp = in.i32();
    c.iec = in.i32();
    c.if0 = in.i32();
    c.permanent_mana = in.i32();
    c.i100 = in.i32();
    c.i104 = in.i32();
    c.clock = in.f32();
    c.ifc = in.i32();
    for (int32_t& v : c.i108) v = in.i32();
    for (int64_t& v : c.ib8) v = in.i64();
    const uint32_t skills = in.u32();
    if (!in.ok() || skills > in.remaining()) return false;
    c.skill_levels.resize(skills);
    for (int32_t& v : c.skill_levels) v = in.i32();
    for (size_t i = 0; i < c.spells.size(); ++i) {
        c.spells[i] = read_string16(in);
        c.spell_levels[i] = in.i32();
    }
    for (int32_t& v : c.i148) v = in.i32();
    for (int32_t& v : c.i164) v = in.i32();
    c.room = in.i32();
    c.i200 = in.i64();
    c.i208 = in.i32();
    const uint32_t mods = in.u32();
    if (!in.ok() || mods > in.remaining()) return false;
    c.mods.resize(mods);
    for (std::string& mod : c.mods) mod = read_string16(in);
    const uint32_t items = in.u32();
    if (!in.ok() || items > in.remaining()) return false;
    c.inventory.resize(items);
    for (ItemSaveState& item : c.inventory) {
        if (!read_item_state(in, item)) return false;
    }
    return read_effects(in, c.effects);
}

// `CLogicNodeState::save`. The flag bytes go
// out as +0x10, +0x11, **+0x13, +0x10** - and `load`
// reads them into +0x10 .. +0x13.
void write_logic_state(ByteWriter& out, const LogicNodeState& s) {
    write_string16(out, s.descriptor);
    out.u8(s.flags[0]);
    out.u8(s.flags[1]);
    out.u8(s.flags[3]);
    out.u8(s.flags[0]);
    out.i32(s.i1c);
    out.f32(s.f20);
    out.f32(s.f24);
    out.u32(s.scene);
    out.i64(s.object_id);
    out.i64(s.parent_id);
    out.i64(s.hierarchy);
    write_vec3(out, s.position);
    out.u8(s.flags[4]);
    out.i32(s.i18);
}

bool read_logic_state(ByteReader& in, LogicNodeState& s) {
    s.descriptor = read_string16(in);
    for (int i = 0; i < 4; ++i) s.flags[i] = in.u8() != 0 ? 1 : 0;
    s.i1c = in.i32();
    s.f20 = in.f32();
    s.f24 = in.f32();
    s.scene = in.u32();
    s.object_id = in.i64();
    s.parent_id = in.i64();
    s.hierarchy = in.i64();
    s.position = read_vec3(in);
    s.flags[4] = in.u8() != 0 ? 1 : 0;
    s.i18 = in.i32();
    return in.ok();
}

// `CLevelState::save`.
void write_level_state(ByteWriter& out, const LevelSaveState& level) {
    out.i32(level.depth);
    out.f32(level.time);
    write_string16(out, level.dungeon);
    out.u8(level.is_town);
    out.u8(level.holds_quest_item);
    out.i32(level.automap_width);
    out.i32(level.automap_height);
    if (level.automap_width > 0) {
        const size_t cells = static_cast<size_t>(level.automap_width) *
                             static_cast<size_t>(std::max(level.automap_height, 0));
        for (size_t i = 0; i < cells; ++i)
            out.f32(i < level.automap.size() ? level.automap[i] : 0.0f);
    }
    out.u32(static_cast<uint32_t>(level.characters.size()));
    for (const CharacterSaveState& c : level.characters) write_character_state(out, c);
    out.u32(static_cast<uint32_t>(level.items.size()));
    for (const ItemSaveState& item : level.items) write_item_state(out, item);
    out.u32(static_cast<uint32_t>(level.logic.size()));
    for (const LogicNodeState& state : level.logic) write_logic_state(out, state);
    out.u32(static_cast<uint32_t>(level.chunks.size()));
    for (const std::string& chunk : level.chunks) write_string16(out, chunk);
    uint32_t pending = 0;
    for (const FormationSaveState& f : level.formations) pending += f.pending != 0 ? 1 : 0;
    out.u32(pending);
    for (const FormationSaveState& f : level.formations) {
        if (f.pending == 0) continue;
        // `saveFormationNode`: a u32 length,
        // not a u16.
        const std::vector<uint16_t> units = utf8_to_utf16(f.layout);
        out.u32(static_cast<uint32_t>(units.size()));
        for (uint16_t unit : units) out.u16(unit);
        write_vec3(out, f.position);
    }
}

// `CLevelState::load`.
bool read_level_state(ByteReader& in, LevelSaveState& level) {
    level.depth = in.i32();
    level.time = in.f32();
    level.dungeon = read_string16(in);
    level.is_town = in.u8() != 0 ? 1 : 0;
    level.holds_quest_item = in.u8();
    level.automap_width = in.i32();
    level.automap_height = in.i32();
    level.automap.clear();
    if (level.automap_width > 0) {
        const uint64_t cells = static_cast<uint64_t>(level.automap_width) *
                               static_cast<uint64_t>(std::max(level.automap_height, 0));
        if (cells * 4 > in.remaining()) return false;
        level.automap.resize(static_cast<size_t>(cells));
        for (float& cell : level.automap) cell = in.f32();
    }
    const uint32_t characters = in.u32();
    if (!in.ok() || characters > in.remaining()) return false;
    level.characters.resize(characters);
    for (CharacterSaveState& c : level.characters) {
        if (!read_character_state(in, c)) return false;
    }
    const uint32_t items = in.u32();
    if (!in.ok() || items > in.remaining()) return false;
    level.items.resize(items);
    for (ItemSaveState& item : level.items) {
        if (!read_item_state(in, item)) return false;
    }
    const uint32_t logic = in.u32();
    if (!in.ok() || logic > in.remaining()) return false;
    level.logic.resize(logic);
    for (LogicNodeState& state : level.logic) {
        if (!read_logic_state(in, state)) return false;
    }
    const uint32_t chunks = in.u32();
    if (!in.ok() || chunks > in.remaining()) return false;
    level.chunks.resize(chunks);
    for (std::string& chunk : level.chunks) chunk = read_string16(in);
    const uint32_t formations = in.u32();
    if (!in.ok() || formations > in.remaining()) return false;
    level.formations.resize(formations);
    for (FormationSaveState& f : level.formations) {
        f.pending = 1;
        const uint32_t length = in.u32();
        f.layout = in.utf16le(length);
        f.position = read_vec3(in);
    }
    return in.ok();
}

std::vector<uint8_t> write_character_file(const CharacterFile& file) {
    ByteWriter out;
    out.u32(file.version);
    write_string16(out, file.class_name);
    out.u32(file.difficulty);
    out.u8(file.hardcore);
    out.u32(file.generation);
    out.u8(file.retired);
    write_character_state(out, file.player);
    out.u16(static_cast<uint16_t>(file.skill_slots.size()));
    for (int64_t v : file.skill_slots) out.i64(v);
    out.u16(static_cast<uint16_t>(file.mapped_skills.size()));
    for (const auto& pair : file.mapped_skills) {
        out.i64(pair.first);
        out.i64(pair.second);
    }
    out.u16(static_cast<uint16_t>(file.second_slots.size()));
    for (int64_t v : file.second_slots) out.i64(v);
    for (uint32_t v : file.journal) out.u32(v);
    write_string16(out, file.current_dungeon);
    write_string16(out, file.last_dungeon);
    out.i32(file.last_dungeon_depth);
    out.u8(file.portal_open);
    write_vec3(out, file.portal_position);
    write_string16(out, file.portal_dungeon);
    out.i32(file.portal_depth);
    out.u32(static_cast<uint32_t>(file.pets.size()));
    for (const CharacterSaveState& pet : file.pets) write_character_state(out, pet);
    out.raw(file.flags.data(), file.flags.size());
    out.i32(static_cast<int32_t>(file.levels.size()));
    for (const LevelSaveState& level : file.levels) write_level_state(out, level);
    out.i32(static_cast<int32_t>(file.trackers.size()));
    for (const DungeonTrackerSave& tracker : file.trackers) write_tracker(out, tracker);
    if (file.has_quests) {
        write_quests(out, file.quests);
    } else {
        out.u32(0);
    }
    out.u32(static_cast<uint32_t>(out.size() + 4));
    return out.bytes();
}

bool read_character_file(const uint8_t* data, size_t size, CharacterFile& file) {
    if (data == nullptr || size < 8) return false;
    ByteReader tail(data + size - 4, 4);
    if (tail.u32() != size) return false;
    ByteReader in(data, size - 4);

    file.version = in.u32();
    if (file.version != kSaveFileVersion) return false;
    file.class_name = read_string16(in);
    file.difficulty = in.u32();
    file.hardcore = in.u8() != 0 ? 1 : 0;
    file.generation = in.u32();
    file.retired = in.u8();
    if (!read_character_state(in, file.player)) return false;
    uint16_t count = in.u16();
    file.skill_slots.resize(count);
    for (int64_t& v : file.skill_slots) v = in.i64();
    count = in.u16();
    file.mapped_skills.resize(count);
    for (auto& pair : file.mapped_skills) {
        pair.first = in.i64();
        pair.second = in.i64();
    }
    count = in.u16();
    file.second_slots.resize(count);
    for (int64_t& v : file.second_slots) v = in.i64();
    for (uint32_t& v : file.journal) v = in.u32();
    file.current_dungeon = read_string16(in);
    file.last_dungeon = read_string16(in);
    file.last_dungeon_depth = in.i32();
    file.portal_open = in.u8() != 0 ? 1 : 0;
    file.portal_position = read_vec3(in);
    file.portal_dungeon = read_string16(in);
    file.portal_depth = in.i32();
    const uint32_t pets = in.u32();
    if (!in.ok() || pets > in.remaining()) return false;
    file.pets.resize(pets);
    for (CharacterSaveState& pet : file.pets) {
        if (!read_character_state(in, pet)) return false;
    }
    for (uint8_t& flag : file.flags) flag = in.u8() != 0 ? 1 : 0;
    const int32_t levels = in.i32();
    if (!in.ok() || levels < 0 || static_cast<uint32_t>(levels) > in.remaining()) return false;
    file.levels.resize(static_cast<size_t>(levels));
    for (LevelSaveState& level : file.levels) {
        if (!read_level_state(in, level)) return false;
    }
    const int32_t trackers = in.i32();
    if (!in.ok() || trackers < 0 || static_cast<uint32_t>(trackers) > in.remaining()) return false;
    file.trackers.resize(static_cast<size_t>(trackers));
    for (DungeonTrackerSave& tracker : file.trackers) {
        if (!read_tracker(in, tracker)) return false;
    }
    file.has_quests = true;
    if (!read_quests(in, file.quests)) return false;
    return in.ok() && in.remaining() == 0;
}

std::vector<uint8_t> write_shared_stash_file(const std::vector<ItemSaveState>& items) {
    ByteWriter out;
    out.u32(kSaveFileVersion);
    out.u32(static_cast<uint32_t>(items.size()));
    for (const ItemSaveState& item : items) write_item_state(out, item);
    return out.bytes();
}

bool read_shared_stash_file(const uint8_t* data, size_t size, std::vector<ItemSaveState>& items) {
    items.clear();
    if (data == nullptr) return false;
    ByteReader in(data, size);
    const uint32_t version = in.u32();
    if (!in.ok() || version != kSaveFileVersion) return false;
    const uint32_t count = in.u32();
    if (!in.ok() || count > in.remaining()) return false;
    items.resize(count);
    for (ItemSaveState& item : items) {
        if (!read_item_state(in, item)) return false;
    }
    return in.ok();
}

}  // namespace tl::formats
