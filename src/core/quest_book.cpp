#include "core/quest_book.h"
#include "core/log.h"
#include "core/logic_events.h"
#include "core/quest_names.h"
#include "core/runic_strings.h"
#include "core/unit_themes.h"
#include "core/unit_types.h"

#include "core/random.h"
#include "core/text.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>

namespace tl::core {
using quest_names::same_name;
using quest_names::upper;

namespace {

// PORT: `ceilf` then a truncation to int (`fctiwz`), the reward arithmetic's last step.
int ceil_to_int(float value) { return static_cast<int>(std::ceil(value)); }

}  // namespace

// ---------------------------------------------------------------------------
// QuestObjective
// ---------------------------------------------------------------------------

// `CQuestUnitData::getUnitName`.
std::string QuestObjective::unit_name() const {
    if (!champion_name.empty()) return champion_name;
    if (!display_name.empty()) {
        // An objective that names a `UNITTYPE` and no `SPAWNCLASS` holds the
        // type's name, printed as it is.
        if (unit_type_id != unit_type::kNone ||
            (spawn_class.empty() && !unit_type.empty() && resolved.empty())) {
            return display_name;
        }
        // A unit's `DISPLAYNAME`: from the first `{` to the first `}` after
        // it, when there is something between them, taken out.
        std::string name = display_name;
        const size_t open = name.find('{');
        const size_t close = open == std::string::npos ? std::string::npos : name.find('}', open);
        if (close != std::string::npos && open != std::string::npos && open + 1 < close) {
            name.replace(open, close - open + 1, "");
        }
        return name;
    }
    // INVENTED: nothing resolved - the authored key (see the header).
    if (!item.empty()) return item;
    if (!monster.empty()) return monster;
    if (!spawn_class.empty()) return spawn_class;
    if (!spawn_from_unittype.empty()) return spawn_from_unittype;
    return unit_type;
}

// The events' test: the unit's data group against the one the objective resolved, held here
// by name.
bool QuestObjective::matches(const std::string& name) const {
    if (name.empty()) return false;
    if (!resolved.empty()) return same_name(resolved, name);
    // INVENTED: nothing resolved the objective (a log with no unit world, as in
    // a unit test): its authored unit stands in.
    return same_name(item, name) || same_name(monster, name);
}

// `CQuestUnitData::isComplete(false)`.
bool QuestObjective::complete() const {
    if (!done) return false;
    for (const QuestObjective& child : children) {
        if (!child.complete()) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// QuestDefinition
// ---------------------------------------------------------------------------

// PORT: a dialog section's tag in the quest file.
const char* quest_section_name(QuestSection section) {
    switch (section) {
        case QuestSection::Intro: return "INTRO";
        case QuestSection::Return: return "RETURN";
        case QuestSection::Complete: return "COMPLETE";
        case QuestSection::Passive: return "PASSIVE";
        case QuestSection::Details: return "DETAILS";
    }
    return "";
}

// PORT: a dialog section from its tag in the quest file.
QuestSection quest_section_of(const std::string& tag) {
    for (QuestSection section : {QuestSection::Return, QuestSection::Complete,
                                 QuestSection::Passive, QuestSection::Details}) {
        if (tag == quest_section_name(section)) return section;
    }
    return QuestSection::Intro;
}

// `CQuest::getAllNPCUnitDataInvolvedInQuest`,
// by name: the units its dialogue lines name (`UNITNAME`), each once.
std::vector<std::string> QuestDefinition::speakers() const {
    std::vector<std::string> out;
    const auto collect = [&out](const std::vector<DialogLine>& lines) {
        for (const DialogLine& line : lines) {
            if (line.unit.empty()) continue;
            const std::string name = upper(line.unit);
            if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
        }
    };
    collect(intro);
    collect(ret);
    collect(complete);
    collect(passive);
    if (has_details && !details.unit.empty()) {
        const std::string name = upper(details.unit);
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    }
    return out;
}

// ---------------------------------------------------------------------------
// QuestBook
// ---------------------------------------------------------------------------

// `CQuestManager::loadQuests`, `getQuestByName`:
// a map by the upper-cased name (`CQuest::loadQuestData` upper-cases it).
void QuestBook::add(QuestDefinition quest) {
    // `CQuestManager::loadQuests`: a second quest of a name
    // already loaded is refused ("There appears to be two quests named the
    // samething."); the first stays.
    if (quest.name.empty()) return;
    const std::string key = upper(quest.name);
    if (by_name_.count(key) != 0) return;
    by_name_.emplace(key, std::move(quest));
    order_.push_back(key);
}

const QuestDefinition* QuestBook::random_people() const { return find("RANDOMPEOPLE"); }

// PORT: a quest definition by name, from the port's book.
const QuestDefinition* QuestBook::find(const std::string& name) const {
    if (name.empty()) return nullptr;
    const auto it = by_name_.find(upper(name));
    return it == by_name_.end() ? nullptr : &it->second;
}

// PORT: a quest definition by GUID, from the port's book.
const QuestDefinition* QuestBook::find_by_guid(int64_t guid) const {
    if (guid == -1) return nullptr;
    for (const auto& entry : by_name_) {
        if (entry.second.guid == guid) return &entry.second;
    }
    return nullptr;
}

// The quests an NPC's dialogue involves, read by `getQuestForNPC`.
// INVENTED: the per-unit lists are not kept; every quest is asked for its
// speakers, in the book's order.
std::vector<const QuestDefinition*> QuestBook::in_manager_order() const {
    std::vector<const QuestDefinition*> out;
    out.reserve(by_name_.size());
    for (const auto& entry : by_name_) out.push_back(&entry.second);
    // `std::map<std::wstring, CQuest*>`: `wmemcmp` over the shorter length,
    // then the shorter first. The names are ASCII, so bytes order the same.
    std::stable_sort(
        out.begin(), out.end(),
        [](const QuestDefinition* a, const QuestDefinition* b) { return a->name < b->name; });
    return out;
}

std::vector<const QuestDefinition*> QuestBook::for_unit(const std::string& unit) const {
    std::vector<const QuestDefinition*> out;
    if (unit.empty()) return out;
    const std::string wanted = upper(unit);
    for (const std::string& key : order_) {
        const QuestDefinition& quest = by_name_.at(key);
        for (const std::string& speaker : quest.speakers()) {
            if (speaker == wanted) {
                out.push_back(&quest);
                break;
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Rewards
// ---------------------------------------------------------------------------

// `CQuestRewards::calculateRewards`: each percent times
// 0.01f, then times its curve.
QuestReward calculate_quest_reward(const QuestRewardSpec& spec, int level, uint32_t seed,
                                   const StatBook& graphs, int difficulty, RunicRandom* rolls) {
    QuestReward reward;

    // no figure and no draw unless a gold or fame percentage is
    // above 0, or there are unit rewards or a treasure class.
    if (!(spec.gold_min_pct > 0.0f) && !(spec.gold_max_pct > 0.0f) && !(spec.fame_min_pct > 0.0f) &&
        !(spec.fame_max_pct > 0.0f) && spec.unit_rewards.empty() && spec.treasure_class.empty()) {
        return reward;
    }
    // `g_Rand` itself is seeded, with the quest's seed plus
    // 0x272b kept to 32 bits.
    RunicRandom& rng = rolls != nullptr ? *rolls : game_random();
    rng.set_state(static_cast<uint32_t>(seed + 0x272bu));
    const float gold_pct = rng.between(spec.gold_min_pct, spec.gold_max_pct);
    const float fame_pct = rng.between(spec.fame_min_pct, spec.fame_max_pct);

    const float at = static_cast<float>(level + 1);
    const std::string suffix = difficulty_suffix(difficulty);

    const float gold_curve = graphs.value(std::string(kGoldDropGraph) + suffix, at, 0.0f);
    reward.gold = ceil_to_int(gold_curve * (gold_pct * 0.01f));  // times 0.01f

    const float fame_curve =
        graphs.value(std::string(kFameChampionMonsterGraph) + suffix, at, 0.0f);
    reward.fame = ceil_to_int(fame_curve * (fame_pct * 0.01f));

    // The experience curve has no difficulty variant, and its roll comes after the
    // other two.
    const float xp_pct = rng.between(spec.xp_min_pct, spec.xp_max_pct);
    const float xp_curve = graphs.value(kExperienceMonsterGraph, at, 0.0f);
    reward.xp = ceil_to_int(xp_curve * (xp_pct * 0.01f));

    return reward;
}

// ---------------------------------------------------------------------------
// QuestEntry
// ---------------------------------------------------------------------------

namespace {

const std::string kEmpty;

// PORT: whether every objective of a list is complete (`QuestObjective::complete`).
bool all_complete(const std::vector<QuestObjective>& list) {
    for (const QuestObjective& objective : list) {
        if (!objective.complete()) return false;
    }
    return true;
}

/// `replaceStringTags`' opening loops: the sum of each unit's count so
/// far. INVENTED: a group is one unit here; the original's can hold several.
int progress_count(const std::vector<QuestObjective>& list) {
    int count = 0;
    for (const QuestObjective& objective : list) count += objective.have;
    return count;
}

/// The four tags of one unit under `prefix` + its number, which counts from 1.
/// `getAcquireTags` asks `isComplete(false)`,
/// the unit with its children; `replaceStringTags`' own DEFEAT loop
/// asks `isComplete(true)`, the unit's flag alone.
void substitute_unit(std::string& text, const std::string& prefix, size_t index,
                     const QuestObjective& objective, int quest_depth, bool own_flag_only) {
    const std::string tag = prefix + value_as_string(static_cast<int>(index) + 1);
    // The order is the engine's, and it matters: the longer keys go first, or
    // `[ACQUIRE1]` would eat the head of `[ACQUIRE1COUNT]`.
    text = replace_wstring(text, tag + "COUNT]", value_as_string(objective.wanted));
    text = replace_wstring(text, tag + "DEPTH]",
                           value_as_string(objective_floor(objective, quest_depth) + 1));
    const bool complete = own_flag_only ? objective.done : objective.complete();
    text = replace_wstring(text, tag + "COMPLETE]", complete ? "Complete" : "Incomplete");
    text = replace_wstring(text, tag + "]", objective.unit_name());
}

}  // namespace

// `CQuestUnitData::getDungeonFloorActiveOn`.
int objective_floor(const QuestObjective& objective, int quest_depth) {
    if (objective.relative_floor != 0) {
        const int sum = objective.relative_floor + quest_depth;
        return -1 < sum ? sum : quest_depth;
    }
    if (objective.specific_floor != -1) return objective.specific_floor;
    return quest_depth;
}

// `CQuestUnitData::getDeepestFloor`.
int deepest_floor(const QuestObjective& objective, int quest_depth) {
    int deepest = 0;
    if (objective.relative_floor != 0) {
        const int sum = objective.relative_floor + quest_depth;
        deepest = sum < 0 ? 1 : sum;
    } else if (objective.specific_floor != -1) {
        deepest = objective.specific_floor;
    }
    for (const QuestObjective& child : objective.children) {
        deepest = std::max(deepest, deepest_floor(child, quest_depth));
    }
    return deepest;
}

// `CQuest::getDungeonMaxFloor`. Each group
// is one unit here (`parse_objectives`).
int dungeon_max_floor(const std::vector<QuestObjective>& acquire,
                      const std::vector<QuestObjective>& defeat, int quest_depth) {
    int deepest = quest_depth;
    for (const QuestObjective& o : acquire)
        deepest = std::max(deepest, deepest_floor(o, quest_depth));
    for (const QuestObjective& o : defeat)
        deepest = std::max(deepest, deepest_floor(o, quest_depth));
    return deepest;
}

// `CQuestUnitData::getIsRandom`.
bool QuestObjective::is_random() const {
    if (min_count != max_count) return true;
    if (!spawn_class.empty()) return true;
    for (const QuestObjective& child : children) {
        if (child.is_random()) return true;
    }
    return false;
}

// `CQuest::getQuestHasRandomPieces`.
bool quest_has_random_pieces(const std::vector<QuestObjective>& acquire,
                             const std::vector<QuestObjective>& defeat) {
    for (const QuestObjective& o : acquire) {
        if (o.is_random()) return true;
    }
    for (const QuestObjective& o : defeat) {
        if (o.is_random()) return true;
    }
    return false;
}

// `CQuest::reinitializeQuest`.
uint32_t take_defeat_groups(const QuestDefinition& quest, RunicRandom& g_rand) {
    return static_cast<uint32_t>(g_rand.integer_between(static_cast<int>(quest.defeat_create),
                                                        static_cast<int>(quest.defeat.size())));
}

// `CQuest::isComplete(false)` past the complete
// flag: never for a quest a controller completes; else the units of both lists and the two
// lists of names, each a reason to be complete once all of it is; else an intro line shown.
bool QuestEntry::objectives_complete() const {
    if (definition != nullptr && definition->quest_controller_completes) return false;
    bool any = false;
    if (has_objectives()) {
        if (!all_complete(defeat) || !all_complete(acquire)) return false;
        any = true;
    }
    for (const std::vector<uint8_t>* flags : {&accepted_named, &completed_named}) {
        if (flags->empty()) continue;
        for (uint8_t flag : *flags) {
            if (flag == 0) return false;
        }
        any = true;
    }
    if (any) return true;
    // `CQuest::isComplete(false)`: with nothing
    // to do, complete once an `[INTRO]` line was spoken - or at once
    // when there is none.
    if (definition != nullptr && definition->intro.empty()) return true;
    return dialog_interacted;
}

// PORT: an accessor of a held quest.
const std::string& QuestEntry::name() const {
    return definition != nullptr ? definition->name : kEmpty;
}

// PORT: an accessor of a held quest.
const std::string& QuestEntry::display_name() const {
    return definition != nullptr ? definition->display_name : kEmpty;
}

// CQuest::getQuestDetails
std::string QuestEntry::details() const {
    if (definition == nullptr || !definition->has_details || definition->details.texts.empty()) {
        return std::string();
    }
    return get_dialog(definition->details, this, game_random());
}

// `CQuest::replaceStringTags`.
std::string replace_quest_tags(const std::string& text, const QuestEntry& entry) {
    std::string out = text;

    // The quest's floor, shown from 1.
    out = replace_wstring(out, "[DEPTH]", value_as_string(entry.depth + 1));
    out = replace_wstring(
        out, "[DEFEATCOMPLETECOUNT]",
        value_as_string(progress_count(entry.defeat) +
                        (entry.defeat.empty() ? 0
                                              : static_cast<int>(entry.defeat_repeats) *
                                                    entry.defeat.front().have)));
    out = replace_wstring(out, "[ACQUIRECOMPLETECOUNT]",
                          value_as_string(progress_count(entry.acquire)));

    // The ACQUIRE list through `getAcquireTags`.
    //
    for (size_t i = 0; i < entry.acquire.size(); ++i) {
        substitute_unit(out, "[ACQUIRE", i, entry.acquire[i], entry.depth, false);
    }

    // The original's misspelling, kept. Replaced whenever the dungeon is found,
    // even by an empty name.
    if (entry.dungeon_display_name)
        out = replace_wstring(out, "[DUNEGONNAME]", *entry.dungeon_display_name);

    // The DEFEAT list, and under each of its units the unit's
    // children as `[DEFEAT<i>ACQUIRE<j>...]`.
    for (size_t i = 0; i < entry.defeat_group_count(); ++i) {
        const QuestObjective& objective = entry.defeat_group(i);
        substitute_unit(out, "[DEFEAT", i, objective, entry.depth, true);
        const std::string nested = "[DEFEAT" + value_as_string(static_cast<int>(i) + 1) + "ACQUIRE";
        for (size_t j = 0; j < objective.children.size(); ++j) {
            substitute_unit(out, nested, j, objective.children[j], entry.depth, false);
        }
    }

    return out;
}

// CQuestDialog::getDialog
std::string get_dialog(const DialogLine& line, const QuestEntry* quest, RunicRandom& rng) {
    if (line.texts.empty()) {
        // DIVERGENCE: original attempts to copy from null data pointer (undefined behaviour).
        return std::string();
    }
    const size_t n = line.texts.size();
    unsigned index = 0;
    if (n >= 2 && quest != nullptr) {
        if (quest->seed != -1) {
            rng.set_seed(quest->seed);
        }
        const int draw = rng.integer_between(1, static_cast<int>(n));
        index = static_cast<unsigned>(draw - 1);
    }
    if (index >= n) index = 0;
    const std::string& chosen = line.texts[index];
    if (quest != nullptr) {
        return replace_quest_tags(chosen, *quest);
    }
    return chosen;
}

// ---------------------------------------------------------------------------
// QuestLog
// ---------------------------------------------------------------------------

// `CQuestMenu::abandonQuest` walks the menu's rows as the held
// quests whose `CQuest::getQuestDetails` is
// not empty.
bool QuestLog::shows_in_journal(const QuestDefinition& quest) {
    return quest.has_details && !quest.details.texts.empty();
}

// PORT: whether a quest takes one of the journal's rows when accepted.
bool QuestLog::needs_a_free_tray(const QuestDefinition& quest) {
    return shows_in_journal(quest) && !quest.force_accept;
}

// The three below are the port's own: what a level change keeps of the quest manager
// (`CQuestManager::save`; `load`).
//
void QuestLog::reset_quest_manager(bool without_player) {
    // `CQuestManager::resetQuestManager`.
    entries_.clear();
    held_.clear();
    for (const std::string& name : state_.names_with(QuestStatus::Complete)) {
        state_.set(name, QuestStatus::Unknown);
    }
    // An active quest is one on the player's list, which is gone.
    for (const std::string& name : state_.names_with(QuestStatus::Active)) {
        state_.set(name, QuestStatus::Unknown);
    }
    if (book_ == nullptr) return;
    QuestDungeonSource source = dungeon_source_;
    if (without_player && source) {
        dungeon_source_ = [source](const std::string& name) {
            QuestDungeon dungeon = source(name);
            dungeon.max_depth = 0;
            return dungeon;
        };
    }
    // DIVERGENCE: a new game rebuilds the renderer, so every quest's seed is -1 again; the
    // console's carries into the next new game of the run.
    for (const QuestDefinition* quest : book_->in_manager_order()) {
        reinitialize_offer(*quest, offers_[upper(quest->name)]);
    }
    dungeon_source_ = std::move(source);
}

// PORT: the log emptied for a new game, with the quest states given.
void QuestLog::reset(const QuestState& state) {
    entries_.clear();
    held_.clear();
    givers_.clear();
    offers_.clear();
    dormant_.clear();
    dormant_seeds_.clear();
    notices_.clear();
    state_ = state;
}

// The line's items-handed flag (`CQuestDialog`), kept per quest, section and line. PORT: the
// port's key for it.
std::string QuestLog::line_key(const QuestDefinition& quest, const DialogLine& line) {
    const std::vector<DialogLine>* sections[] = {&quest.intro, &quest.ret, &quest.complete,
                                                 &quest.passive};
    for (size_t s = 0; s < 4; ++s) {
        const std::vector<DialogLine>& lines = *sections[s];
        if (lines.empty() || &line < lines.data() || &line >= lines.data() + lines.size()) continue;
        return upper(quest.name) + "|" + std::to_string(s) + "|" +
               std::to_string(static_cast<size_t>(&line - lines.data()));
    }
    return std::string();
}

// PORT: the line's items-handed flag (`CQuestDialog +0x77`), asked.
bool QuestLog::items_handed(const QuestDefinition& quest, const DialogLine& line) const {
    return items_handed_.count(line_key(quest, line)) != 0;
}

// PORT: the line's items-handed flag (`CQuestDialog +0x77`), set.
void QuestLog::set_items_handed(const QuestDefinition& quest, const DialogLine& line) {
    const std::string key = line_key(quest, line);
    if (!key.empty()) items_handed_.insert(key);
}

// PORT: the log handed across the port's level change (`Renderer::restart`); the
// console keeps one quest manager for the whole game.
QuestLog::Carried QuestLog::carry() const {
    Carried carried;
    carried.state = state_;
    carried.items_handed = items_handed_;
    carried.givers = givers_;
    carried.offers = offers_;
    for (const QuestEntry& entry : entries_) {
        if (entry.definition == nullptr) continue;
        QuestEntry copy = entry;
        copy.definition = nullptr;
        carried.entries.emplace_back(entry.definition->name, std::move(copy));
    }
    carried.held = held_;
    for (const auto& kept : dormant_) {
        if (kept.second.definition == nullptr) continue;
        QuestEntry copy = kept.second;
        copy.definition = nullptr;
        carried.dormant.emplace_back(kept.second.definition->name, std::move(copy));
    }
    return carried;
}

// PORT: a name held, on the end (`giveQuest` pushes onto the held quests).
void QuestLog::hold(const std::string& name) {
    const std::string key = upper(name);
    for (const std::string& h : held_)
        if (h == key) return;
    held_.push_back(key);
}

// PORT: a name let go the console's way: the last held put in its place.
void QuestLog::unhold(const std::string& name) {
    const std::string key = upper(name);
    for (size_t i = 0; i < held_.size(); ++i) {
        if (held_[i] != key) continue;
        held_[i] = held_.back();
        held_.pop_back();
        return;
    }
}

std::vector<const QuestEntry*> QuestLog::held_entries() const {
    std::vector<const QuestEntry*> out;
    for (const std::string& name : held_)
        if (const QuestEntry* entry = find(name)) out.push_back(entry);
    return out;
}

std::vector<QuestEntry*> QuestLog::held_entries_mutable() {
    std::vector<QuestEntry*> out;
    for (const std::string& name : held_)
        if (QuestEntry* entry = find(name)) out.push_back(entry);
    return out;
}

// PORT: the log taken back after the port's level change, its definitions found
// again by name.
void QuestLog::restore(const Carried& carried) {
    state_ = carried.state;
    items_handed_ = carried.items_handed;
    givers_ = carried.givers;
    offers_ = carried.offers;
    dormant_.clear();
    for (const auto& named : carried.dormant) {
        const QuestDefinition* definition = book_ != nullptr ? book_->find(named.first) : nullptr;
        if (definition == nullptr) continue;
        QuestEntry entry = named.second;
        entry.definition = definition;
        dormant_[upper(definition->name)] = std::move(entry);
    }
    entries_.clear();
    for (const auto& named : carried.entries) {
        const QuestDefinition* definition = book_ != nullptr ? book_->find(named.first) : nullptr;
        if (definition == nullptr) continue;
        QuestEntry entry = named.second;
        entry.definition = definition;
        entry.dungeon_display_name = dungeon_display_name(*definition);
        entries_.push_back(std::move(entry));
    }
    // The order the manager held them in; a log carried without one, in the
    // order its active entries came.
    held_.clear();
    if (!carried.held.empty()) {
        for (const std::string& name : carried.held) {
            const QuestEntry* entry = find(name);
            if (entry != nullptr && entry->status == QuestStatus::Active) hold(name);
        }
    } else {
        for (const QuestEntry& entry : entries_)
            if (entry.status == QuestStatus::Active) hold(entry.name());
    }
}

std::optional<std::string> QuestLog::dungeon_display_name(const QuestDefinition& quest) const {
    // `CQuest::replaceStringTags`: the quest's `DUNGEON` through
    // `CDungeonManager::getDungeonByName`, its display name.
    if (!dungeon_source_ || quest.dungeon.empty()) return std::nullopt;
    const QuestDungeon dungeon = dungeon_source_(upper(quest.dungeon));
    if (!dungeon.found) return std::nullopt;
    return dungeon.display_name;
}

// `CQuestRequirements::questRequirmentsHaveBeenMet`.
bool QuestLog::requirements_met(const QuestDefinition& quest) const {
    const QuestRequirementSpec& need = quest.requirements;
    const std::string own = upper(quest.name);

    // A quest that is complete is never offered again (`getQuestComplete` on
    // its own name).
    if (quest_complete(quest.name)) return false;
    if (!need.present) return true;

    const QuestPlayer player = player_source_ ? player_source_() : QuestPlayer{};
    if (!player.present) return false;
    // Unsigned, and only when the minimum was authored.
    if (need.min_player_level != 0xffffffffu &&
        (player.level < need.min_player_level || player.level > need.max_player_level)) {
        return false;
    }
    // A quest named in its own list passes it: the name test runs only on a
    // quest that fails.
    for (const std::string& name : need.quests_complete) {
        if (!quest_complete(name) && name != own) return false;
    }
    for (const std::string& name : need.quests_not_complete) {
        if (quest_complete(name) && name != own) return false;
    }
    for (const std::string& name : need.quests_active) {
        if (!state_.is_active(name)) return false;
    }

    if (!player.on_level) return false;
    // The quest's `DUNGEON`, `MAIN` when it has none: the
    // template of the floor past the deepest the player has reached in it.
    const std::string dungeon = quest.dungeon.empty() ? std::string("MAIN") : upper(quest.dungeon);
    std::string template_name;
    const int32_t deepest = player.max_depth ? player.max_depth(dungeon) : -1;
    // `(x >> 31) - 1 & x`: below 0 is 0.
    const int32_t next = deepest + 1 < 0 ? 0 : deepest + 1;
    if (!player.template_name ||
        !player.template_name(dungeon, static_cast<uint32_t>(next), template_name)) {
        return false;
    }
    if (!need.ruleset.empty() && template_name != need.ruleset) return false;
    // Signed, and only when the minimum was authored - an authored minimum
    // with no maximum has -1 for one, which nothing passes.
    return need.min_dungeon_depth == -1 ||
           (need.min_dungeon_depth <= next && next <= need.max_dungeon_depth);
}

// PORT: whether a quest is active in the port's record.
bool QuestLog::accepted(const QuestDefinition& quest) const {
    return state_.status(quest.name) == QuestStatus::Active;
}

// `CQuest::isComplete(false)`, held or not. One never held
// has the lists its last offer left, each unit fresh (`loadQuestData`,
// `reinitializeQuest`,
// `CQuestUnitData::isComplete`, no `[INTRO]`). The flag the call
// caches is not kept: every answer is worked out.
bool QuestLog::is_complete(const QuestDefinition& quest) const {
    if (state_.status(quest.name) == QuestStatus::Complete) return true;
    if (const QuestEntry* entry = find(quest.name)) return entry->objectives_complete();
    if (quest.quest_controller_completes) return false;
    if (const Offered* rolled = offered(quest)) {
        if (!quest.acquire.empty()) return false;
        if (rolled->defeat_groups > 0 && !quest.defeat.empty()) return false;
    }
    if (!quest.complete_on_quest_accept.empty() || !quest.complete_on_quest_complete.empty()) {
        return false;
    }
    return quest.intro.empty();
}

std::string QuestLog::giver_of(const QuestDefinition& quest) const {
    const auto it = givers_.find(upper(quest.name));
    return it == givers_.end() ? std::string() : it->second;
}

// `CQuestDialog::dialogIsForNPC`.
bool QuestLog::line_is_for(const QuestDefinition& quest, const DialogLine& line,
                           const std::string& unit) const {
    if (!line.unit.empty()) return same_name(line.unit, unit);
    const std::string giver = giver_of(quest);
    return !giver.empty() && same_name(giver, unit);
}

// `CQuest::getQuestDialog(CBaseUnit*)`.
const DialogLine* QuestLog::dialog_for(const QuestDefinition& quest, const std::string& unit,
                                       QuestSection* section) const {
    // `dialogIsForNPC(CBaseUnit*, TArrayList&)`:
    // whether any line of the list is for the unit; the dialog
    // returned is the first that is.
    const auto first_for = [&](const std::vector<DialogLine>& lines) -> const DialogLine* {
        for (const DialogLine& line : lines) {
            if (line_is_for(quest, line, unit)) return &line;
        }
        return nullptr;
    };
    struct Pick {
        const std::vector<DialogLine>* lines;
        QuestSection section;
    };
    const Pick none{nullptr, QuestSection::Intro};
    const Pick ret =
        first_for(quest.ret) != nullptr ? Pick{&quest.ret, QuestSection::Return} : none;

    const bool held = accepted(quest);
    Pick pick = none;
    if (is_complete(quest)) {
        pick = first_for(quest.complete) != nullptr ? Pick{&quest.complete, QuestSection::Complete}
                                                    : ret;
    } else if (!held) {
        pick = first_for(quest.intro) != nullptr ? Pick{&quest.intro, QuestSection::Intro} : ret;
    } else {
        // Accepted: the `[INTRO]` line again only while none of the unit's intro
        // lines has been interacted with. DIVERGENCE: the flag is kept
        // per quest (`QuestEntry::dialog_interacted`), not per line.
        const QuestEntry* entry = find(quest.name);
        const bool interacted = entry != nullptr && entry->dialog_interacted;
        pick = first_for(quest.intro) != nullptr && !interacted
                   ? Pick{&quest.intro, QuestSection::Intro}
                   : ret;
    }
    if (pick.lines == nullptr && held && first_for(quest.passive) != nullptr) {
        pick = Pick{&quest.passive, QuestSection::Passive};
    }
    if (pick.lines == nullptr) return nullptr;
    if (section != nullptr) *section = pick.section;
    return first_for(*pick.lines);
}

// `CQuest::getQuestIsValidForNPC`: not run out, its
// requirements met, and either a line for the unit or `REPEATABLE` (`loadQuestData`).
//
bool QuestLog::valid_for_npc(const QuestDefinition& quest, const std::string& unit) const {
    if (unit.empty()) return false;
    const Offered* rolled = offered(quest);
    if (rolled != nullptr && rolled->exhausted) return false;
    if (!requirements_met(quest)) return false;
    return dialog_for(quest, unit) != nullptr || quest.repeatable;
}

// PORT: the offer the log holds for a quest.
const QuestLog::Offered* QuestLog::offered(const QuestDefinition& quest) const {
    const auto it = offers_.find(upper(quest.name));
    return it == offers_.end() ? nullptr : &it->second;
}

// `CQuest::reinitializeQuest`, the floor.
QuestFloor roll_quest_floor(bool repeatable, const QuestDungeon& dungeon) {
    QuestFloor out;
    bool taken = false;
    int k = 1;
    for (uint32_t tries = 0; !(9 < tries) && !taken; ++tries, ++k) {
        int floor = dungeon.max_depth + k;
        out.depth = floor;
        if (!dungeon.bottomless && repeatable && dungeon.depth_count <= floor) {
            out.exhausted = true;
            return out;
        }
        if (floor < 0) {
            floor = 0;
            out.depth = 0;
        }
        taken = !(dungeon.no_random_quests && dungeon.no_random_quests(floor));
    }
    return out;
}

// `CQuest::reinitializeQuest(.., false)`, the quest's half
// (`UTILITIES::setSeed`). The units are rolled again at `accept`, each off the
// same seed (`CQuestUnitData::reinitialize` seeds `g_Rand`), so nothing between changes them.
void QuestLog::reinitialize_offer(const QuestDefinition& quest, Offered& rolled) {
    RunicRandom& g_rand = game_random();
    rolled.exhausted = false;
    if (rolled.seed != -1) g_rand.set_seed(rolled.seed);
    if (dungeon_source_ && !quest.dungeon.empty()) {
        const QuestDungeon dungeon = dungeon_source_(upper(quest.dungeon));
        if (dungeon.found) {
            const QuestFloor floor = roll_quest_floor(quest.repeatable, dungeon);
            rolled.depth = floor.depth;
            rolled.exhausted = floor.exhausted;
        }
    }
    rolled.defeat_groups = static_cast<int32_t>(take_defeat_groups(quest, g_rand));
}

// `CQuest::initializeQuestWithNPC(unit)`
// (`CLEAR_DUNGEON_HISTORY`, `CPlayer::clearDungeonHistory`). The reward level
// is the monsters' rank on the deepest floor of the roll before this one. A quest no reset
// has rolled (a log put back by `reset`) is given that roll here.
void QuestLog::initialize_with_npc(const QuestDefinition& quest, const std::string& unit,
                                   RunicRandom& roll) {
    if (same_name(giver_of(quest), unit)) return;
    if (player_has_quest(quest)) return;
    const std::string key = upper(quest.name);
    auto it = offers_.find(key);
    if (it == offers_.end()) {
        it = offers_.emplace(key, Offered{}).first;
        reinitialize_offer(quest, it->second);
    }
    Offered& rolled = it->second;
    if (dungeon_source_ && !quest.dungeon.empty()) {
        const QuestDungeon dungeon = dungeon_source_(upper(quest.dungeon));
        if (dungeon.found && dungeon.rank_on_floor) {
            if (quest.clear_dungeon_history && dungeon.clear_history) dungeon.clear_history();
            const size_t taken = std::min(quest.defeat.size(),
                                          static_cast<size_t>(std::max(0, rolled.defeat_groups)));
            const std::vector<QuestObjective> defeat(quest.defeat.begin(),
                                                     quest.defeat.begin() + taken);
            const int deepest = dungeon_max_floor(quest.acquire, defeat, rolled.depth);
            rolled.level = dungeon.rank_on_floor(std::max(deepest, 1));
        }
    }
    rolled.seed = roll.integer_between(0, 0x7fffffff);
    reinitialize_offer(quest, rolled);
    givers_[key] = upper(unit);
    // With the can-reroll-rewards flag set: the made units deleted and
    // `calculateRewards` run.
    if (rolled.can_reroll_rewards) {
        rolled.can_reroll_rewards = false;
        rolled.reward_units.clear();
        calculate_rewards(quest, rolled);
    }
}

// PORT: the log's state set from outside.
void QuestLog::restore_offer(const QuestDefinition& quest, const Offered& rolled,
                             const std::string& giver) {
    const std::string key = upper(quest.name);
    offers_[key] = rolled;
    if (giver.empty()) {
        givers_.erase(key);
    } else {
        givers_[key] = upper(giver);
    }
}

// `CQuestRewards::calculateRewards`.
QuestReward QuestLog::calculate_rewards(const QuestDefinition& quest, Offered& rolled) {
    QuestReward figures;
    const QuestRewardSpec& spec = quest.reward_spec;
    // a level, a player, and no units made yet.
    if (!reward_world_.ready || !reward_world_.ready() || !rolled.reward_units.empty())
        return figures;
    if (!(spec.gold_min_pct > 0.0f) && !(spec.gold_max_pct > 0.0f) && !(spec.fame_min_pct > 0.0f) &&
        !(spec.fame_max_pct > 0.0f) && spec.unit_rewards.empty() && spec.treasure_class.empty()) {
        return figures;
    }
    RunicRandom& rng = game_random();
    // The level is the quest's rank, the seed its own.
    const int32_t level = rolled.level;
    const uint32_t seed = static_cast<uint32_t>(rolled.seed);
    if (reward_world_.graphs != nullptr) {
        figures =
            calculate_quest_reward(spec, level, seed, *reward_world_.graphs,
                                   reward_world_.difficulty ? reward_world_.difficulty() : 1, &rng);
    }
    // each `[UNITREWARD]` made, an item with `IDENTIFY` identified.
    for (const QuestRewardSpec::UnitReward& reward : spec.unit_rewards) {
        QuestRewardUnit made;
        if (!reward_world_.create_unit ||
            !reward_world_.create_unit(reward.unit, reward.resource_group, made)) {
            continue;
        }
        if (reward.identify && made.is_item) made.item.identified = true;
        rolled.reward_units.push_back(std::move(made));
    }
    // the `TREASURECLASS` rolled for the player (as character
    // and finder) at the rank plus -1..2, seeded with the quest's seed and the
    // class's own seed `seed + 0x272b`; each unit made, enchanted (`enchant(true)`)
    // when its flag says so, and identified.
    if (!spec.treasure_class.empty()) {
        const TreasureRoll roll = quest_treasure_roll(level, static_cast<int32_t>(seed), rng);
        std::vector<std::string> units;
        std::vector<bool> enchant;
        if (reward_world_.roll_spawn_class) {
            reward_world_.roll_spawn_class(spec.treasure_class, roll.level, roll.seed, units,
                                           enchant);
        }
        for (size_t i = 0; i < units.size(); ++i) {
            QuestRewardUnit made;
            if (!reward_world_.create_unit || !reward_world_.create_unit(units[i], 0, made))
                continue;
            // The flag list read past its end reads its first (`TArrayList`).
            const bool flag = i < enchant.size() ? enchant[i] : (!enchant.empty() && enchant[0]);
            if (made.is_item) {
                if (flag && reward_world_.enchant) reward_world_.enchant(made.item);
                made.item.identified = true;
            }
            rolled.reward_units.push_back(std::move(made));
        }
    }
    return figures;
}

// `CQuestManager::getPlayerHasQuest`, inlined by the console.
//
bool QuestLog::player_has_quest(const QuestDefinition& quest) const {
    const QuestEntry* entry = find(quest.name);
    return entry != nullptr && entry->status == QuestStatus::Active;
}

int QuestLog::offer_level(const QuestDefinition& quest, int fallback) const {
    const Offered* rolled = offered(quest);
    return rolled != nullptr ? rolled->level : fallback;
}

// `CQuestManager::getDialogsForNPC`, first
// entry.
const DialogLine* QuestLog::dialog_for_npc(const std::string& unit,
                                           const std::vector<std::string>& unit_quests,
                                           RunicRandom& roll, const QuestDefinition** quest,
                                           QuestSection* section) {
    if (quest != nullptr) *quest = nullptr;
    if (unit.empty()) return nullptr;
    for (const QuestEntry* held : held_entries()) {
        const QuestEntry& entry = *held;
        if (entry.definition == nullptr || entry.status != QuestStatus::Active) continue;
        if (const DialogLine* line = dialog_for(*entry.definition, unit, section)) {
            if (quest != nullptr) *quest = entry.definition;
            return line;
        }
    }
    const QuestDefinition* found = quest_for_npc(unit, unit_quests, roll);
    if (found == nullptr) return nullptr;
    const DialogLine* line = dialog_for(*found, unit, section);
    if (line == nullptr) {
        found = quest_for_npc(unit, unit_quests, roll);
        line = found != nullptr ? dialog_for(*found, unit, section) : nullptr;
    }
    if (line != nullptr && quest != nullptr) *quest = found;
    return line;
}

// `CQuestManager::getQuestForNPC`.
const QuestDefinition* QuestLog::quest_for_npc(const std::string& unit,
                                               const std::vector<std::string>& unit_quests,
                                               RunicRandom& roll) {
    // `CQuestManager::getQuestForNPC`.
    if (book_ == nullptr || unit.empty()) return nullptr;

    // The quests kept for the unit's data group, in order, those
    // `getQuestIsValidForNPC` takes.
    std::vector<const QuestDefinition*> candidates;
    for (const QuestDefinition* quest : book_->for_unit(unit)) {
        if (valid_for_npc(*quest, unit)) candidates.push_back(quest);
    }

    // The player's own quests: one this unit's data gave is returned.
    for (const QuestEntry* held : held_entries()) {
        const QuestEntry& entry = *held;
        if (entry.definition == nullptr || entry.status != QuestStatus::Active) continue;
        if (same_name(giver_of(*entry.definition), unit)) return entry.definition;
    }

    // With more than one, the random people's quest is taken out as a
    // `TArrayList` removes (the last moved into its place).
    const QuestDefinition* random_people = book_->random_people();
    if (candidates.size() > 1) {
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (candidates[i] != random_people) continue;
            candidates[i] = candidates.back();
            candidates.pop_back();
            break;
        }
    }
    // With it alone and a `QUESTS` group on the unit, it is set aside for the
    // group's quests.
    std::vector<const QuestDefinition*> set_aside;
    if (candidates.size() == 1 && candidates[0] == random_people && !unit_quests.empty()) {
        set_aside = candidates;
        candidates.clear();
    }
    if (candidates.empty()) {
        // The group's quests, by upper-cased name: one not accepted that is valid for the
        // unit. One this unit's data gave whose floor is deeper than the player has been in
        // its dungeon (`CPlayer::getMaxDepth`) is returned uninitialised.
        for (const std::string& name : unit_quests) {
            const QuestDefinition* quest = book_->find(name);
            if (quest == nullptr || accepted(*quest)) continue;
            if (!valid_for_npc(*quest, unit)) continue;
            if (same_name(giver_of(*quest), unit)) {
                const Offered* rolled = offered(*quest);
                const int32_t floor = rolled != nullptr ? rolled->depth : 0;
                const QuestPlayer player = player_source_ ? player_source_() : QuestPlayer{};
                const int32_t deepest = player.max_depth ? player.max_depth(quest->dungeon) : -1;
                if (deepest < floor) return quest;
            }
            candidates.push_back(quest);
        }
        if (candidates.empty()) candidates = set_aside;
    }
    if (candidates.empty()) return nullptr;

    // One draw among two or more; one alone is taken.
    const int32_t last = static_cast<int32_t>(candidates.size()) - 1;
    const int32_t pick = last < 1 ? 0 : roll.integer_between(0, last);
    const QuestDefinition* quest = candidates[pick < 0 || pick > last ? 0 : pick];
    initialize_with_npc(*quest, unit, roll);
    return quest;
}

namespace {

/// `g_QUESTUNIT_SPAWNCLASS_PREDEFINE_NAMES`: the classes
/// `getBaseUnitDataGroup` gives an objective that names nothing.
const char* const kPredefinedClasses[4] = {"", "LEVEL_QUEST_MONSTERS", "LEVEL_QUEST_CHAMPIONS",
                                           "LEVEL_QUEST_ITEMS"};

/// `CQuestUnitData::getSpawnClassUnitDataLookingFor`:
/// the unit a spawn class rolls for the
/// objective, on the quest's floor's level template.
std::string spawn_class_unit_looking_for(QuestObjective& objective, const QuestEntry& entry,
                                         const QuestUnitWorld& world) {
    if (objective.predefined_class == 0) {
        // `wmemcmp`: the names exactly, case and all.
        for (int i = 1; i != 4; ++i) {
            if (objective.spawn_class == kPredefinedClasses[i]) objective.predefined_class = i;
        }
        if (objective.predefined_class == 0 && objective.spawn_class.empty()) return {};
    }
    // No player in a level, no roll. The quest's template
    // is looked for by `choose_spawners`.
    if (world.player_on_level && !world.player_on_level()) return {};

    // `resetGameSeed(floor + the quest's seed)`, the floor being
    // `getDungeonFloorActiveOn`'s, then `chooseUnitSpawners` on the quest's
    // template - the slots its quest classes read come from there.
    const int32_t floor = objective_floor(objective, entry.depth);
    const int32_t seed_depth =
        static_cast<int32_t>(static_cast<uint32_t>(floor) + static_cast<uint32_t>(entry.seed));
    std::vector<std::string> chosen;
    if (!world.choose_spawners || !world.choose_spawners(entry, seed_depth, chosen)) return {};
    auto slot = [&chosen](size_t i) { return i < chosen.size() ? chosen[i] : std::string(); };

    std::string name = objective.spawn_class;
    bool found = false;
    if (!name.empty()) {
        found = world.has_spawn_class && world.has_spawn_class(name);
        if (!found) name.clear();
    }
    // Chosen slots 8, 9 and 10 are the quest monster, item and champion classes.
    bool items = false;
    if (name.empty()) {
        switch (objective.predefined_class) {
            case 2:
                name = slot(10);
                if (name == "MONSTERSETCHAMPION") name = slot(1);
                if (name == "NONE" || name.empty()) name = "MONSTERSETCHAMPION";
                break;
            case 3:
                name = slot(9);
                items = true;
                break;
            case 1:
                name = slot(8);
                if (name == "MONSTERSET") name = slot(0);
                if (name == "NONE" || name.empty()) name = "MONSTERSET";
                break;
            default: break;
        }
        if (name.empty()) return {};
    }
    if (!found && !(world.has_spawn_class && world.has_spawn_class(name))) return {};

    // `rollSpawnClass(.., level + floor - the quest's floor, 0xffffffff, ..)`: an item class
    // with level bonus -1 and rarity 1, any other with 0 and -1.
    const int32_t level = entry.level + floor - entry.depth;
    if (!world.roll_spawn_class) return {};
    return world.roll_spawn_class(name, level, items ? -1 : 0, items ? 1 : -1);
}

/// `CQuestUnitData::reinitialize(true)`
/// (`getBaseUnitDataGroup`): the count is drawn on what the
/// generator holds after the unit is resolved - the first draw off the quest's seed unless a
/// spawn class was rolled.
void reinitialize_objectives(std::vector<QuestObjective>& list, const QuestEntry& entry,
                             const QuestUnitWorld& world, const RandomNames* names,
                             bool resolve = true, RunicRandom* rng = nullptr) {
    const int32_t seed = static_cast<int32_t>(entry.seed);
    RunicRandom& g_rand = rng != nullptr ? *rng : game_random();
    for (QuestObjective& objective : list) {
        objective.done = false;
        objective.populated = false;
        reinitialize_objectives(objective.children, entry, world, names, resolve, rng);
        g_rand.set_seed(seed);
        if (!resolve) objective.resolved.clear();
        const bool named = resolve && resolve_objective_unit(objective, entry, world);
        // The champion's name off the quest's seed, the generator restored:
        // a generator of its own here.
        if (objective.make_champion && named && names != nullptr && !names->empty()) {
            RunicRandom name_rng(0u);
            name_rng.set_seed(seed);
            objective.champion_name = names->generate_name(true, name_rng);
        }
        objective.wanted = g_rand.integer_between(objective.min_count, objective.max_count);
        objective.have = 0;
    }
}

/// `CQuestUnitData::getBaseUnitDataGroup` of an objective with no unit, as
/// `populate` asks it: the unit, then a champion's name off the quest's seed, `g_Rand` put back
/// to its low word.
void resolve_unresolved_objective(QuestObjective& objective, const QuestEntry& entry,
                                  const QuestUnitWorld& world, const RandomNames* names) {
    const bool named = resolve_objective_unit(objective, entry, world);
    if (!named || !objective.make_champion || names == nullptr || names->empty()) return;
    RunicRandom& g_rand = game_random();
    const int32_t saved = static_cast<int32_t>(static_cast<uint32_t>(g_rand.state()));
    g_rand.set_seed(static_cast<int32_t>(entry.seed));
    objective.champion_name = names->generate_name(true, g_rand);
    g_rand.set_seed(saved);
}

/// `m_gQuestUnitDataID`, which every `CQuestUnitData` takes a number from.
int32_t g_quest_unit_id = 0;

// `m_gQuestUnitDataID`: each objective takes the next number. PORT: the port
// numbers the held copies, where a placed unit's tag points.
void number_objectives(std::vector<QuestObjective>& list) {
    for (QuestObjective& objective : list) {
        objective.id = ++g_quest_unit_id;
        number_objectives(objective.children);
    }
}

// PORT: each objective named for the journal through the renderer's namer.
void name_objectives(std::vector<QuestObjective>& list, const QuestUnitNamer& namer) {
    for (QuestObjective& objective : list) {
        objective.display_name = namer(objective);
        name_objectives(objective.children, namer);
    }
}

}  // namespace

// `CQuestUnitData::load`'s done flag; see the
// header for the `DIVERGENCE:`.
bool quest_unit_done_from_save(bool saved_done, int32_t wanted, int32_t have) {
    if (saved_done) return true;
    return have > 0 && static_cast<uint32_t>(wanted) <= static_cast<uint32_t>(have);
}

bool resolve_objective_unit(QuestObjective& objective, const QuestEntry& entry,
                            const QuestUnitWorld& world) {
    // The two lookups before these never apply after `reinitialize`, so they are skipped.
    objective.resolved.clear();
    if (!objective.spawn_class.empty()) {
        //
        objective.resolved = spawn_class_unit_looking_for(objective, entry, world);
        return true;
    }
    if (!objective.unit_type.empty()) {
        // The type's id; no group.
        objective.unit_type_id =
            world.type_id ? world.type_id(objective.unit_type) : unit_type::kNone;
        return true;
    }
    if (!objective.item.empty()) {
        // `getDataGroupByObjectName(gRESOURCE_GROUP_NAMES[0], ITEM)`, and out
        // before the name.
        objective.resolved = objective.item;
        return false;
    }
    if (!objective.monster.empty()) {
        // The monsters' group, and `REMOVEFROMINVENTORY` cleared.
        objective.resolved = objective.monster;
        objective.remove_from_inventory = false;
        return true;
    }
    // Nothing named: a predefined class. An `[ACQUIRE]`
    // unit's leaves before the name.
    if (objective.kind == 1) {
        objective.spawn_class = kPredefinedClasses[3];
        objective.resolved = spawn_class_unit_looking_for(objective, entry, world);
        return false;
    }
    objective.spawn_class = objective.make_champion ? kPredefinedClasses[2] : kPredefinedClasses[1];
    objective.resolved = spawn_class_unit_looking_for(objective, entry, world);
    return true;
}

// PORT: a held quest's objectives named for the journal.
void QuestLog::name_entry(QuestEntry& entry) const {
    if (!unit_namer_) return;
    name_objectives(entry.defeat, unit_namer_);
    name_objectives(entry.acquire, unit_namer_);
    name_objectives(entry.populate, unit_namer_);
}

// `CQuestDialogMenu::onClick`: a quest with
// journal details that is not `FORCEACCEPT` is refused while six or
// more held quests have details ("Your quest log is full!"). The dialog's test
// alone: `giveQuest`, which scripts and loads call too, has no such limit.
bool QuestLog::log_full_for(const QuestDefinition& quest) const {
    return needs_a_free_tray(quest) && full();
}

bool QuestLog::accept(const QuestDefinition& quest, int level, uint32_t seed,
                      const StatBook& graphs, int depth, bool resolve_units) {
    QuestEntry* existing = find(quest.name);
    QuestEntry fresh;
    QuestEntry& entry = existing != nullptr ? *existing : fresh;

    entry.definition = &quest;
    entry.dungeon_display_name = dungeon_display_name(quest);
    entry.status = QuestStatus::Active;
    entry.level = level;
    entry.seed = seed;
    // The floor the quest was rolled onto when its giver took it up.
    if (depth >= 0) {
        entry.depth = depth;
    } else if (const Offered* rolled = offered(quest)) {
        entry.depth = rolled->depth;
    }
    // The `[DEFEAT]` groups the quest's last roll took (`reinitializeQuest`,
    // `take_defeat_groups`). INVENTED: one no NPC has initialised takes them
    // here (see `initialize_with_npc`).
    uint32_t groups = 0;
    if (const Offered* rolled = offered(quest); rolled != nullptr && 0 <= rolled->defeat_groups) {
        groups = static_cast<uint32_t>(rolled->defeat_groups);
    } else {
        groups = take_defeat_groups(quest, game_random());
    }
    entry.defeat.assign(quest.defeat.begin(),
                        quest.defeat.begin() + std::min<size_t>(groups, quest.defeat.size()));
    entry.defeat_repeats = quest.defeat.empty() || groups <= quest.defeat.size()
                               ? 0u
                               : groups - static_cast<uint32_t>(quest.defeat.size());
    entry.acquire = quest.acquire;
    entry.populate = quest.populate;
    // `reinitializeQuest` clears the two lists' flags.
    entry.accepted_named.assign(quest.complete_on_quest_accept.size(), 0);
    entry.completed_named.assign(quest.complete_on_quest_complete.size(), 0);
    number_objectives(entry.defeat);
    number_objectives(entry.acquire);
    number_objectives(entry.populate);
    entry.complete_announced = false;  // `reinitializeQuest`

    // `CQuest::reinitializeQuest` calls each
    // unit's `reinitialize(true)`, which resolves its unit.
    reinitialize_objectives(entry.defeat, entry, unit_world_, random_names_, resolve_units);
    reinitialize_objectives(entry.acquire, entry, unit_world_, random_names_, resolve_units);
    reinitialize_objectives(entry.populate, entry, unit_world_, random_names_, resolve_units);
    // The printed name is the resolved unit's (`getUnitName`).
    //
    if (unit_namer_) {
        name_objectives(entry.defeat, unit_namer_);
        name_objectives(entry.acquire, unit_namer_);
        name_objectives(entry.populate, unit_namer_);
    }

    entry.reward = calculate_quest_reward(quest.reward_spec, level, seed, graphs);
    // The units the offer made stay on the quest.
    if (const auto it = offers_.find(upper(quest.name)); it != offers_.end()) {
        entry.reward_units = it->second.reward_units;
    }
    // `CQuestRewards::reInitializeRewards` (inlined
    // - the made items deleted, `calculateRewards` again) makes the
    // items again.
    entry.reward_items_given = false;

    state_.activate(quest.name);
    if (existing == nullptr) entries_.push_back(entry);
    hold(quest.name);

    // `CQuest::setQuestAccepted(true)`, from
    // `giveQuest`: event 3 to every quest held, which flags the ones
    // whose `[COMPLETE_ON_QUEST_ACCEPT]` names this one.
    broadcast_quest_event(3);

    // `COMPLETESONACCEPT` finishes the quest at once (`giveQuest`):
    // the controllers hear Complete, Not Active, then Active.
    if (quest.completes_on_accept && is_complete(quest) && complete(quest.name)) {
        notify(quest.name, {logic::kQuestNotActive});
    }
    notify(quest.name, {logic::kQuestActive});
    return true;
}

// `CQuestMenu::abandonQuest`: only a quest that authors `CANABANDON`; then
// `CQuestManager::removeQuest(quest, true)` and
// `CQuest::cleanUp(false)`.
// INVENTED: `cleanUp`'s `reinitializeQuest`, and `removeQuest`'s walk of the
// quest's placed units, are not modelled.
bool QuestLog::abandon(const std::string& name) {
    QuestEntry* entry = find(name);
    if (entry == nullptr || entry->definition == nullptr) return false;
    if (!entry->definition->can_abandon) return false;
    if (entry->status != QuestStatus::Active) return false;

    const QuestDefinition& quest = *entry->definition;
    queue_item_removals(quest, entry, /*completed=*/false);
    givers_.erase(upper(quest.name));
    offers_.erase(upper(quest.name));
    unhold(quest.name);
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(entry - entries_.data()));
    state_.set(quest.name, QuestStatus::Unknown);
    // `removeQuest(quest, true)`: not accepted, then abandoned, per controller.
    notify(quest.name, {logic::kQuestNotActive, logic::kQuestAbandoned});
    return true;
}

// PORT: a quest forgotten by the log: its giver, offer, entry and state.
void QuestLog::drop(const std::string& name) {
    givers_.erase(upper(name));
    offers_.erase(upper(name));
    unhold(name);
    if (QuestEntry* entry = find(name)) {
        entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(entry - entries_.data()));
    }
    state_.set(name, QuestStatus::Unknown);
}

bool QuestLog::complete(const std::string& name, QuestReward* paid) {
    QuestEntry* entry = find(name);
    if (entry == nullptr) return false;
    // The callers of this (an objective done, `COMPLETESONACCEPT`, the
    // script) each complete a quest once;
    // the dialogue's unconditional `completeQuest` is `complete_by_dialog`.
    if (entry->status == QuestStatus::Complete) return false;

    entry->status = QuestStatus::Complete;
    state_.complete(entry->name());
    const std::string held_name = entry->name();
    // `completeQuest`'s walk of the controllers,
    // before the reward and the quests it
    // completes or gives in turn.
    notify(entry->name(), {logic::kQuestComplete});
    if (paid != nullptr) *paid = entry->reward;

    // `CQuestManager::completeQuest` ->
    // `giveRewardForQuest` -> `CQuest::giveRewardForQuest`:
    // the payout, then `cleanUp(true)`.
    // Before the consequences: they can complete other quests, each paid and
    // cleaned up in its own turn, and can reallocate `entries_`.
    if (entry->definition != nullptr) {
        const QuestDefinition& quest = *entry->definition;
        queue_completion(quest, entry);
        entry->reward_items_given = true;
        rewards_paid(quest);
        queue_item_removals(quest, entry);
        completion_consequences(quest);
    }
    // `completeQuest` takes it off the held quests
    // after `giveRewardForQuest` gave the quests that follow it,
    // so the last of those can take its place.
    unhold(held_name);
    return true;
}

// `CQuest::giveRewardForQuest` after
// `CQuestRewards::rewardPlayer`: the made units are gone (handed over), and
// the can-reroll-rewards flag is set, so the next offer
// makes them again.
void QuestLog::rewards_paid(const QuestDefinition& quest) {
    Offered& offer = offers_[upper(quest.name)];
    offer.reward_units.clear();
    offer.can_reroll_rewards = true;
}

// `CQuestManager::completeQuest` from
// `CQuestDialogMenu::onClick`. On a quest already complete it runs whole again
// (`giveRewardForQuest`; `CQuestRewards::rewardPlayer`;
// `cleanUp`): the same gold,
// experience and fame paid again, but not the items. Kept.
bool QuestLog::complete_by_dialog(const QuestDefinition& quest) {
    QuestEntry* entry = find(quest.name);
    const bool already = entry != nullptr ? entry->status == QuestStatus::Complete
                                          : state_.status(quest.name) == QuestStatus::Complete;
    if (!already) return complete_quest(quest);
    state_.complete(quest.name);
    notify(quest.name, {logic::kQuestComplete});
    queue_completion(quest, entry);
    if (entry != nullptr) entry->reward_items_given = true;
    rewards_paid(quest);
    queue_item_removals(quest, entry);
    completion_consequences(quest);
    return true;
}

bool QuestLog::complete_quest(const QuestDefinition& quest) {
    if (state_.status(quest.name) == QuestStatus::Complete) return false;
    if (find(quest.name) != nullptr) return complete(quest.name);
    state_.complete(quest.name);
    notify(quest.name, {logic::kQuestComplete});
    queue_completion(quest, nullptr);
    // INVENTED: a quest the player never held has no objectives here, so only
    // `cleanUp`'s last, quest-wide removal is made for it.
    queue_item_removals(quest, nullptr);
    completion_consequences(quest);
    return true;
}

namespace {

/// `CQuestUnitData::cleanUpForQuestRemoveall(bool)`:
/// children first. Only an `[ACQUIRE]` objective whose unit resolves and that says
/// `REMOVEFROMINVENTORY` takes anything back (`getBaseUnitDataGroup`;
/// `reinitialize`).
/// Without `REMOVE_ONLY_QUESTITEMS`, `CPlayer::removeItemFromInventoryOrPetsInventoryByGuid`
/// looks the items up as quest 0xffffffff
/// and takes nothing. Kept.
void queue_objective_removals(const std::vector<QuestObjective>& list, int64_t quest_guid,
                              uint32_t isa, std::vector<QuestItemRemoval>& out) {
    for (const QuestObjective& objective : list) {
        queue_objective_removals(objective.children, quest_guid, isa, out);
        if (objective.kind != 1 || !objective.remove_from_inventory) continue;
        if (objective.resolved.empty()) continue;
        if (!objective.remove_only_questitems) continue;
        out.push_back(QuestItemRemoval{quest_guid, objective.resolved, true, isa});
    }
}

}  // namespace

void QuestLog::queue_item_removals(const QuestDefinition& quest, const QuestEntry* entry,
                                   bool completed) {
    // `CQuest::cleanUp(bool)`: the acquire
    // groups, the defeat groups, every copy of the quest's own `QuestItem`s, then
    // `CLevel::makeQuestUnitsUncool` on the player's level. INVENTED: the rest
    // of `cleanUp` (the dungeon history, `reinitializeQuest`) is not modelled here.
    if (entry != nullptr) {
        const uint32_t isa = completed ? unit_type::kAny : unit_type::kQuestItem;
        queue_objective_removals(entry->acquire, quest.guid, isa, item_removals_);
        queue_objective_removals(entry->defeat, quest.guid, isa, item_removals_);
    }
    item_removals_.push_back(
        QuestItemRemoval{quest.guid, std::string(), false, unit_type::kQuestItem, false});
    item_removals_.push_back(
        QuestItemRemoval{quest.guid, std::string(), false, unit_type::kQuestItem, true});
}

// What `CQuest::giveRewardForQuest` pays and
// announces, kept for whoever owns the player (`take_completions`): this layer
// holds no inventory to pay into.
void QuestLog::queue_completion(const QuestDefinition& quest, const QuestEntry* entry) {
    QuestCompletion done;
    done.quest = &quest;
    done.held = entry != nullptr;
    if (entry != nullptr) {
        done.reward = entry->reward;
        done.level = entry->level;
        done.seed = entry->seed;
        done.dialog_interacted = entry->dialog_interacted;
        done.reward_items = !entry->reward_items_given;
        if (done.reward_items) done.reward_units = entry->reward_units;
    }
    completions_.push_back(done);
}

// PORT: what the log has queued for the renderer, taken.
std::vector<QuestCompletion> QuestLog::take_completions() {
    std::vector<QuestCompletion> taken;
    taken.swap(completions_);
    return taken;
}

TreasureRoll quest_treasure_roll(int quest_level, int32_t quest_seed, RunicRandom& rng) {
    // `calculateRewards`' treasure roll.
    rng.set_seed(quest_seed);
    TreasureRoll roll;
    roll.level = rng.integer_between(-1, 2) + quest_level;
    roll.seed = static_cast<uint32_t>(quest_seed) + 0x272bu;
    return roll;
}

// PORT: a quest's controller outputs queued for the renderer to deliver.
void QuestLog::notify(const std::string& quest, std::vector<uint32_t> outputs) {
    if (quest.empty()) return;
    notices_.push_back(QuestNotice{upper(quest), std::move(outputs)});
}

// PORT: what the log has queued for the renderer, taken.
std::vector<QuestNotice> QuestLog::take_notices() {
    std::vector<QuestNotice> taken;
    taken.swap(notices_);
    return taken;
}

// PORT: what the log has queued for the renderer, taken.
std::vector<QuestItemRemoval> QuestLog::take_item_removals() {
    std::vector<QuestItemRemoval> taken;
    taken.swap(item_removals_);
    return taken;
}

void QuestLog::completion_consequences(const QuestDefinition& quest) {
    // Copied before recursing: completing another quest can reallocate
    // `entries_`, and with it the definition's owner is untouched but the
    // caller's entry is not.
    const std::vector<std::string> give = quest.give_quests_on_complete;

    // `[GIVE_QUESTS_ON_COMPLETE]` (`CQuest::giveRewardForQuest`):
    // each quest of the manager by the
    // upper-cased name, `giveQuest` (onto the end of the held quests, nothing for
    // one held or accepted already) and `setQuestAccepted(true)`, an event 3.
    // With the seed and floor its last offer left; none, the constructor's.
    for (const std::string& next : give) {
        const QuestDefinition* def = book_ != nullptr ? book_->find(next) : nullptr;
        if (def == nullptr) continue;
        if (state_.status(def->name) == QuestStatus::Active) continue;
        const Offered* rolled = offered(*def);
        static const StatBook kNoGraphs;
        accept(*def, rolled != nullptr ? rolled->level : 1,
               static_cast<uint32_t>(rolled != nullptr ? rolled->seed : -1),
               reward_world_.graphs != nullptr ? *reward_world_.graphs : kNoGraphs,
               rolled != nullptr ? rolled->depth : -1);
    }
    // Then `completeQuest`'s own: the complete flag is `!REPEATABLE`,
    // its event 4 only when that newly sets it - sent
    // before the quest is written into the completed map. A repeatable quest
    // done is free to be offered again.
    if (quest.repeatable) {
        state_.set(quest.name, QuestStatus::Unknown);
        return;
    }
    broadcast_quest_event(4, quest.name);
}

bool objective_on_level(QuestObjective& objective, const std::string& quest_dungeon,
                        int quest_depth, const std::string& level_dungeon, int level_depth) {
    // `CQuestUnitData::populate`. A quest
    // bound to no dungeon cannot create anything.
    if (quest_dungeon.empty()) objective.create = false;
    // `RELATIVEFLOOR` added to the quest's floor (a negative sum keeps the
    // quest's own), else `SPECIFICFLOOR`, else the quest's floor.
    const int floor = objective_floor(objective, quest_depth);
    if (level_depth == floor && same_name(level_dungeon, quest_dungeon)) return true;
    // Off its floor: only an objective pinned to no floor still places, in
    // its quest's dungeon or anywhere when the quest names none.
    if (objective.create) return false;
    if (objective.relative_floor != 0) return false;
    if (objective.specific_floor > 0) return false;
    return quest_dungeon.empty() || same_name(level_dungeon, quest_dungeon);
}

void QuestLog::tag_unit_of_type(uint32_t type, const QuestUnitTag& tag) {
    // `CQuestUnitData::populate`
    // (`CBaseUnit::CBaseUnit`): units drawn until one whose quest
    // GUID is not -1, which is tagged. Every unit's is -1 from its constructor, so only one a
    // quest already stamped is taken. Kept as the console has it.
    if (!unit_world_.units_of_type) return;
    std::vector<QuestUnitWorld::LevelUnit> units = unit_world_.units_of_type(type);
    uint32_t count = static_cast<uint32_t>(units.size());
    while (count != 0) {
        const int pick = game_random().integer_between(0, static_cast<int>(count) - 1);
        const QuestUnitWorld::LevelUnit unit = units[static_cast<size_t>(pick)];
        --count;
        units[static_cast<size_t>(pick)] = units[count];
        if (unit.tag.empty()) continue;
        if (unit_world_.tag_unit) unit_world_.tag_unit(unit, tag);
        return;
    }
}

// A quest the player does not hold, as `CQuest::CQuest` and the manager's
// `reinitializeQuest` left it: seed -1 (+0x10), floor 0 (+0x14), rank 1 (+0x1ec) unless an
// NPC's offer rolled them, its units unresolved (no level template then) and their counts
// drawn off the seed. `g_Rand` is left as it was.
QuestEntry& QuestLog::dormant_entry(const QuestDefinition& quest) {
    const std::string key = upper(quest.name);
    const auto found = dormant_.find(key);
    if (found != dormant_.end()) return found->second;
    QuestEntry& entry = dormant_[key];
    entry.definition = &quest;
    const Offered* rolled = offered(quest);
    entry.seed =
        rolled != nullptr ? static_cast<uint32_t>(rolled->seed) : static_cast<uint32_t>(-1);
    entry.depth = rolled != nullptr ? rolled->depth : 0;
    entry.level = rolled != nullptr ? rolled->level : 1;
    if (const auto given = dormant_seeds_.find(key); given != dormant_seeds_.end()) {
        entry.seed = given->second.seed;
        entry.depth = given->second.depth;
        entry.level = given->second.level;
    }
    // The counts were drawn at the reset, long before: on a generator of their own here.
    RunicRandom counts(0u);
    uint32_t groups = 0;
    if (rolled != nullptr && 0 <= rolled->defeat_groups) {
        groups = static_cast<uint32_t>(rolled->defeat_groups);
    } else {
        // INVENTED: the reset's `[DEFEAT]` group draw is not followed; one off the quest's seed
        // stands in (the shipped random quests take one group, 1..1).
        RunicRandom groups_rng(entry.seed);
        groups = take_defeat_groups(quest, groups_rng);
    }
    entry.defeat.assign(quest.defeat.begin(),
                        quest.defeat.begin() + std::min<size_t>(groups, quest.defeat.size()));
    entry.acquire = quest.acquire;
    entry.populate = quest.populate;
    number_objectives(entry.defeat);
    number_objectives(entry.acquire);
    number_objectives(entry.populate);
    reinitialize_objectives(entry.defeat, entry, unit_world_, random_names_, false, &counts);
    reinitialize_objectives(entry.acquire, entry, unit_world_, random_names_, false, &counts);
    reinitialize_objectives(entry.populate, entry, unit_world_, random_names_, false, &counts);
    return entry;
}

// PORT: a dormant quest's seed, floor and rank set from outside (the oracle hands the console's).
void QuestLog::seed_dormant(const QuestDefinition& quest, uint32_t seed, int depth, int level) {
    const std::string key = upper(quest.name);
    dormant_seeds_[key] = DormantSeed{seed, depth, level};
    dormant_.erase(key);
}

// `CQuestUnitData::populate` of an objective on the level.
void QuestLog::populate_objective(QuestEntry& entry, QuestObjective& objective, bool active,
                                  const CreateQuestUnit& create_unit) {
    // The unit first: `getBaseUnitDataGroup` while it has none, five times at
    // most; still none marks it complete and populated.
    for (int tries = 0; objective.resolved.empty() && tries < 5; ++tries)
        resolve_unresolved_objective(objective, entry, unit_world_, random_names_);
    if (objective.resolved.empty()) {
        TL_LOG_INFO("Unable to spawn unit for quest %s", entry.name().c_str());
        objective.done = true;
        objective.populated = true;
        return;
    }
    // INVENTED: a completed quest's own unit still standing in the level is removed here
    // (`getBaseUnitByDataGroup` and `removeUnit` unless `NEVERDESTROY`); not modelled.
    if (objective.done || !active || objective.populated) return;
    objective.populated = true;
    // A `UNITTYPE` objective places nothing.
    if (objective.unit_type_id != unit_type::kNone) return;
    const QuestUnitTag tag{entry.name(), objective.id};
    // `SPAWNFROMUNITTYPE`'s id, which `CQuestUnitData::reinitialize`
    // looks up by name, `NONE` when there is none.
    const uint32_t from_type = objective.spawn_from_unittype.empty() || !unit_world_.type_id
                                   ? unit_type::kNone
                                   : unit_world_.type_id(objective.spawn_from_unittype);
    if (from_type == unit_type::kNone) {
        // `createUnit` returns at once without `CREATE`.
        if (!objective.create) return;
        for (int i = 0; i < objective.wanted; ++i) create_unit(entry, objective, tag);
        return;
    }
    tag_unit_of_type(from_type, tag);
}

void QuestLog::populate_level(const std::string& level_dungeon, int level_depth,
                              const std::function<void(const QuestEntry&, const QuestObjective&,
                                                       const QuestUnitTag&)>& create_unit) {
    // `CLevel::populate` walks the manager's map, every quest
    // the game knows in `NAME` order, held or not: `CQuest::populate`.
    std::vector<QuestEntry*> walk;
    if (book_ != nullptr) {
        for (const QuestDefinition* quest : book_->in_manager_order()) {
            QuestEntry* held = find(quest->name);
            walk.push_back(held != nullptr ? held : &dormant_entry(*quest));
        }
    } else {
        for (QuestEntry& entry : entries_) walk.push_back(&entry);
    }
    for (QuestEntry* walked : walk) {
        QuestEntry& entry = *walked;
        if (entry.definition == nullptr) continue;
        std::string dungeon = entry.definition->dungeon;
        for (char& c : dungeon) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const bool active = entry.status == QuestStatus::Active;
        // `CQuest::populate`: the acquire groups, the defeat groups, then the
        // populate groups.
        for (std::vector<QuestObjective>* list : {&entry.acquire, &entry.defeat, &entry.populate}) {
            for (QuestObjective& objective : *list) {
                if (!objective_on_level(objective, dungeon, entry.depth, level_dungeon,
                                        level_depth)) {
                    continue;
                }
                populate_objective(entry, objective, active, create_unit);
            }
        }
    }
}

// PORT: the active quests the journal lists (`shows_in_journal`).
// `CQuestMenuXenon`'s rows and `CQuestDialogMenu::onClick`'s count: the held
// quests in their held order that have journal details.
std::vector<const QuestEntry*> QuestLog::journal_entries() const {
    std::vector<const QuestEntry*> out;
    for (const QuestEntry* entry : held_entries()) {
        if (entry->definition == nullptr) continue;
        if (entry->status != QuestStatus::Active) continue;
        if (!shows_in_journal(*entry->definition)) continue;
        out.push_back(entry);
    }
    return out;
}

// PORT: a held quest by name.
const QuestEntry* QuestLog::find(const std::string& name) const {
    for (const QuestEntry& entry : entries_) {
        if (same_name(entry.name(), name)) return &entry;
    }
    return nullptr;
}

// PORT: a held quest by name.
QuestEntry* QuestLog::find(const std::string& name) {
    for (QuestEntry& entry : entries_) {
        if (same_name(entry.name(), name)) return &entry;
    }
    return nullptr;
}

bool QuestLog::calculate_npc_icon(const std::string& unit,
                                  const std::vector<std::string>& unit_quests, RunicRandom& roll,
                                  bool in_cinematic, bool shown, UnitThemeSet& themes,
                                  const UnitThemeBook* book) {
    // `CQuestManager::calculateNPCIcon`. The quest is
    // drawn before anything else, cinematic or not.
    const QuestDefinition* quest = quest_for_npc(unit, unit_quests, roll);
    static const char* const kComplete = "QUEST COMPLETE";
    static const char* const kIncomplete = "QUEST INCOMPLETE";
    static const char* const kGiving = "QUEST GIVING";
    static const char* const kPassive = "QUEST PASSIVE";
    themes.remove_unit_theme(kComplete, book);
    themes.remove_unit_theme(kIncomplete, book);
    themes.remove_unit_theme(kGiving, book);
    themes.remove_unit_theme(kPassive, book);
    if (in_cinematic) return true;
    if (quest == nullptr || !shown) return false;

    // The quests the player holds.
    const bool held = find(quest->name) != nullptr;
    QuestSection section = QuestSection::Intro;
    const DialogLine* line = dialog_for(*quest, unit, &section);
    if (line == nullptr) return false;
    if (!line->icon_above_head) return true;
    if (!line->theme_override.empty()) {
        themes.add_unit_theme(line->theme_override, book);
        return true;
    }
    if (section == QuestSection::Passive) {
        // `CQuest::setQuestAcceptDialogInteracted` marks only the `[INTRO]` lines,
        // so a passive line's marker always goes on.
        themes.add_unit_theme(kPassive, book);
        themes.remove_unit_theme(kComplete, book);
        themes.remove_unit_theme(kIncomplete, book);
        themes.remove_unit_theme(kGiving, book);
        return true;
    }
    if (held) {
        if (is_complete(*quest)) {
            themes.add_unit_theme(kComplete, book);
            themes.remove_unit_theme(kIncomplete, book);
            themes.remove_unit_theme(kGiving, book);
            themes.remove_unit_theme(kPassive, book);
            return true;
        }
        themes.remove_unit_theme(kComplete, book);
        themes.remove_unit_theme(kGiving, book);
        themes.remove_unit_theme(kPassive, book);
        themes.add_unit_theme(kIncomplete, book);
        return true;
    }
    themes.add_unit_theme(kGiving, book);
    themes.remove_unit_theme(kComplete, book);
    themes.remove_unit_theme(kIncomplete, book);
    themes.remove_unit_theme(kPassive, book);
    return true;
}

void update_npc_icon(QuestLog& log, const std::string& unit,
                     const std::vector<std::string>& unit_quests, uint32_t type,
                     const UnitTypeHierarchy& types, RunicRandom& roll, bool in_cinematic,
                     bool shown, bool pending_delete, UnitThemeSet& themes,
                     const UnitThemeBook* book) {
    // `CLevel::updateNPCIcons`, the body of
    // its loop over the level's units.
    if (pending_delete || !shown) return;
    if (log.calculate_npc_icon(unit, unit_quests, roll, in_cinematic, shown, themes, book)) return;
    namespace ut = unit_type;
    const char* theme = nullptr;
    if (types.isa(type, ut::kGambler)) {
        theme = "GAMBLER";
    } else if (types.isa(type, ut::kEnchanter)) {
        theme = "ENCHANTER";
    } else if (types.isa(type, ut::kCombiner)) {
        theme = "TRANSMUTE";
    } else if (types.isa(type, ut::kSocketableDestroyer)) {
        theme = "GEMRETRIEVE";
    } else if (types.isa(type, ut::kSocketableRecoverer)) {
        theme = "GEMRECOVER";
    } else if (types.isa(type, ut::kMerchant)) {
        theme = "MERCHANT";
    }
    if (theme != nullptr) themes.add_unit_theme(theme, book);
}

}  // namespace tl::core
