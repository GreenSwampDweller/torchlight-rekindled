// The quest manager's event updates: a unit defeated, picked up, interacted with or dropped,
// credited to the objectives that wait for it.
#include "core/logic_events.h"
#include "core/quest_book.h"
#include "core/quest_names.h"
#include "core/runic_strings.h"
#include "core/unit_types.h"

#include <algorithm>
#include <string>
#include <vector>

namespace tl::core {
using quest_names::same_name;

namespace {

// Whether a unit carries this objective's tag. PORT: the tag as the port keeps it.
bool tagged_for(const QuestUnitTag& tag, const QuestEntry& entry, const QuestObjective& o) {
    return !tag.empty() && same_name(tag.quest, entry.name()) && tag.objective == o.id;
}

// Whether a unit is tagged for this quest. PORT: the tag as the port keeps it.
bool tagged_for_quest(const QuestUnitTag& tag, const QuestEntry& entry) {
    return !tag.empty() && same_name(tag.quest, entry.name());
}

/// What one event walks the quest's objectives with.
struct QuestEvent {
    QuestEntry& entry;
    QuestEventUnit& unit;
    const QuestUnitWorld& world;
    std::vector<QuestProgressText>& texts;
};

/// The pair of lines both counting events put up
/// (`STRINGS::GetValueAsWString`).
void progress_text(const QuestObjective& o, QuestEvent& e, const std::string& word) {
    QuestProgressText text;
    if (e.entry.definition != nullptr) {
        text.big = replace_quest_tags(e.entry.definition->display_name, e.entry);
    }
    text.small = e.unit.shown_name + " - " + value_as_string(static_cast<uint32_t>(o.have)) + "/" +
                 value_as_string(static_cast<uint32_t>(o.wanted)) + word;
    e.texts.push_back(std::move(text));
}

// `CQuestUnitData::getBaseUnitDataGroup` in the events: an objective left with no unit
// resolves again, each time an event reaches it.
void ensure_resolved(QuestObjective& o, QuestEvent& e) {
    if (o.resolved.empty()) resolve_objective_unit(o, e.entry, e.world);
}

// The `tagged` half every quest event computes: the unit is of the objective's
// `UNITTYPE`, or carries the objective's own tag. PORT: a helper of `quest_event`.
bool tagged_for_objective(const QuestObjective& o, const QuestEvent& e) {
    return (o.unit_type_id != unit_type::kNone && e.unit.is_a(o.unit_type_id)) ||
           tagged_for(e.unit.tag, e.entry, o);
}

/// `CQuestUnitData::spawnChildrenUnits`, through
/// `spawnUnit`: a child counts as placed even when nothing
/// was.
void spawn_children(QuestObjective& o, QuestEvent& e) {
    for (size_t i = 0; i < o.children.size(); ++i) {
        QuestObjective& child = o.children[i];
        float radius = static_cast<float>(i) * 0.5f;
        if (radius <= 1.0f) radius = 1.0f;
        Vec3 at = e.world.random_open_position
                      ? e.world.random_open_position(e.unit.position, radius)
                      : e.unit.position;
        at.y += 1.5f;
        if (!child.resolved.empty() && !child.done && e.world.spawn_unit) {
            e.world.spawn_unit(e.entry, child, QuestUnitTag{e.entry.name(), child.id}, at);
        }
        child.populated = true;
    }
}

/// `CQuestUnitData::unitDefeated(CBaseUnit*)`.
/// An objective that creates its units from no
/// `SPAWNFROMUNITTYPE` counts only what it tagged. The count goes up before it is compared,
/// so a target of 0 needs one.
bool credit_defeat(QuestObjective& o, QuestEvent& e) {
    if (!o.populated) return false;
    ensure_resolved(o, e);
    bool matched = o.matches(e.unit.name) || e.unit.is_a(o.unit_type_id);
    const bool tagged = tagged_for_objective(o, e);
    if (o.spawn_from_unittype.empty() && o.create) matched = tagged;
    if (!o.done) {
        if (!matched) return false;
        o.have += 1;
        if (e.unit.on_level && e.unit.is_a(unit_type::kCharacter) && !e.unit.shown_name.empty()) {
            progress_text(o, e, " Defeated");
        }
        if (static_cast<uint32_t>(o.wanted) > static_cast<uint32_t>(o.have)) return false;
        spawn_children(o, e);
        o.done = true;
        return true;
    }
    if (matched) return false;
    for (QuestObjective& child : o.children) {
        if (credit_defeat(child, e)) return true;
    }
    return false;
}

/// `CQuestUnitData::unitPickedUp(CBaseUnit*)`.
/// A `[DEFEAT]`/`[POPULATE]` objective
/// takes nothing itself, but once done hands its quest's items to its children (the
/// champion's dropped stone).
bool credit_pickup(QuestObjective& o, QuestEvent& e) {
    if (!o.populated) return false;
    if (o.kind == 0) {
        if (o.done && tagged_for_quest(e.unit.tag, e.entry)) {
            for (QuestObjective& child : o.children) {
                if (credit_pickup(child, e)) return true;
            }
        }
        return false;
    }
    ensure_resolved(o, e);
    if (!e.unit.tag.empty() && !tagged_for_quest(e.unit.tag, e.entry)) return false;
    const bool matched = o.matches(e.unit.name);
    const bool tagged = tagged_for_objective(o, e);
    const bool was_done = o.done;
    bool counted = false;
    if (!o.create) {
        if (matched) {
            e.unit.tag.quest = e.entry.name();
            o.have += 1;
        }
        counted = matched;
        if (!was_done && static_cast<uint32_t>(o.wanted) <= static_cast<uint32_t>(o.have)) {
            o.done = true;
        }
    } else if (!was_done) {
        if (tagged && matched) {
            e.unit.tag.quest = e.entry.name();
            o.have += 1;
            counted = true;
        }
        if (static_cast<uint32_t>(o.wanted) <= static_cast<uint32_t>(o.have)) o.done = true;
    }
    if (o.done && static_cast<uint32_t>(o.wanted) < static_cast<uint32_t>(o.have)) {
        o.have = o.wanted;
    }
    if (!was_done && counted && e.unit.on_level && e.unit.is_a(unit_type::kItem) &&
        !e.unit.shown_name.empty() && !e.entry.complete_announced) {
        progress_text(o, e, " Found");
    }
    if (o.done) {
        if (!counted && tagged_for_quest(e.unit.tag, e.entry) && !o.children.empty()) {
            for (QuestObjective& child : o.children) {
                if (credit_pickup(child, e)) return true;
            }
            return false;
        }
        if (!was_done) spawn_children(o, e);
    }
    return counted;
}

/// `CQuestUnitData::unitInteracted(CBaseUnit*)`:
/// a unit with a level only, placed or not.
/// Matched as a kill is (data group or `ISA`), with no `CREATE` rule;
/// counted until done, the children spawned when it finishes. A finished
/// objective hands a unit that is not its own to its children.
bool credit_interact(QuestObjective& o, QuestEvent& e) {
    if (!e.unit.on_level) return false;
    ensure_resolved(o, e);
    const bool matched = o.matches(e.unit.name) || e.unit.is_a(o.unit_type_id);
    const bool tagged = tagged_for_objective(o, e);
    if (!o.done && matched) {
        o.have += 1;
        if (static_cast<uint32_t>(o.have) < static_cast<uint32_t>(o.wanted)) return false;
        spawn_children(o, e);
        o.done = true;
        return true;
    }
    if (tagged || !o.done) return false;
    for (QuestObjective& child : o.children) {
        if (credit_interact(child, e)) return true;
    }
    return false;
}

/// `CQuestUnitData::unitDropped(CBaseUnit*)`:
/// placed objectives only, the children first; then a unit of the objective's
/// `UNITTYPE` or carrying its own tag takes one off the count (not below 0) and
/// undoes it. An item the pickup only stamped with the quest (not the objective) does not
/// match, so dropping it leaves the count alone.
bool credit_drop(QuestObjective& o, QuestEvent& e) {
    if (!o.populated) return false;
    for (QuestObjective& child : o.children) {
        if (credit_drop(child, e)) return true;
    }
    ensure_resolved(o, e);
    if (!tagged_for_objective(o, e)) return false;
    if (o.have != 0) o.have -= 1;
    o.done = false;
    return true;
}

/// `CQuest::questEventUpdate`'s walk: the `[ACQUIRE]`
/// lists, then the `[DEFEAT]` lists. A list is one `[UNIT]` here (`calculateUnitsFromTag`),
/// so every objective is offered the event.
template <typename Credit>
void credit_lists(QuestEvent& e, Credit credit) {
    for (QuestObjective& o : e.entry.acquire) credit(o, e);
    for (QuestObjective& o : e.entry.defeat) credit(o, e);
    // The first group again, as many times as the list holds it again.
    for (uint32_t i = 0; i < e.entry.defeat_repeats; ++i) credit(e.entry.defeat.front(), e);
}

}  // namespace

std::vector<const QuestDefinition*> QuestLog::quest_event(int event, QuestEventUnit& unit,
                                                          bool by_player) {
    // `CQuestManager::questEventUpdate`:
    // every quest it holds that is not complete - every one for a drop (event
    // 5) - through `CQuest::questEventUpdate`; the banner for one this event completed,
    // only the first time.
    std::vector<const QuestDefinition*> finished;
    for (QuestEntry* held : held_entries_mutable()) {
        QuestEntry& entry = *held;
        if (entry.status != QuestStatus::Active) continue;
        const bool was_complete = entry.objectives_complete();
        if (was_complete && event != 5) continue;
        QuestEvent e{entry, unit, unit_world_, progress_texts_};
        switch (event) {
            case 0: credit_lists(e, credit_pickup); break;
            case 1: credit_lists(e, credit_defeat); break;
            // Only the character the manager belongs.
            case 2:
                if (by_player) credit_lists(e, credit_interact);
                break;
            case 5: credit_lists(e, credit_drop); break;
            default: break;
        }
        // The refresh `refreshQuestMenu` is asked for:
        // set by a quest this event completed
        // and had not yet announced, and by one whose completion this event
        // left as it was - not by one completed again after its banner, nor by
        // one a drop took out of completion.
        const bool now_complete = entry.objectives_complete();
        if (!now_complete || was_complete || entry.complete_announced) {
            if (now_complete == was_complete) quest_menu_refresh_ = true;
            continue;
        }
        quest_menu_refresh_ = true;
        entry.complete_announced = true;
        if (entry.definition != nullptr) finished.push_back(entry.definition);
    }
    return finished;
}

// `CQuestManager::questEventUpdate(3 or 4, 0, 0)` (`CQuest`'s):
// `setQuestAccepted(true)` sends 3,
// `completeQuest` 4. Each held quest not complete flags the names of its
// lists now active (`CQuestManager::getQuestIsActive`) or done; the ones this completes are
// queued for the banner.
void QuestLog::broadcast_quest_event(int event, const std::string& completing) {
    for (QuestEntry* held : held_entries_mutable()) {
        QuestEntry& entry = *held;
        if (entry.status != QuestStatus::Active || entry.definition == nullptr) continue;
        if (entry.objectives_complete()) continue;
        const bool accepted = event == 3;
        const std::vector<std::string>& names = accepted
                                                    ? entry.definition->complete_on_quest_accept
                                                    : entry.definition->complete_on_quest_complete;
        std::vector<uint8_t>& flags = accepted ? entry.accepted_named : entry.completed_named;
        for (size_t i = 0; i < names.size() && i < flags.size(); ++i) {
            if (flags[i] != 0) continue;
            const bool met = accepted
                                 ? quest_is_active(names[i])
                                 : quest_complete(names[i]) && !same_name(names[i], completing);
            if (met) flags[i] = 1;
        }
        if (!entry.objectives_complete() || entry.complete_announced) continue;
        entry.complete_announced = true;
        finished_.push_back(entry.definition);
    }
}

// `CQuestManager::getQuestIsActive`: a quest
// of that name on the player's list - here the held
// entries and the quests given into the state alike.
bool QuestLog::quest_is_active(const std::string& name) const {
    return state_.status(name) == QuestStatus::Active;
}

// `CQuestManager::getQuestComplete`: the
// completed map's flag, which `completeQuest` writes as "not
// `REPEATABLE`" - a repeatable quest done is not complete to it.
bool QuestLog::quest_complete(const std::string& name) const {
    if (state_.status(name) != QuestStatus::Complete) return false;
    const QuestDefinition* quest = book_ != nullptr ? book_->find(name) : nullptr;
    return quest == nullptr || !quest->repeatable;
}

// PORT: what the log has queued for the renderer, taken.
std::vector<const QuestDefinition*> QuestLog::take_finished() {
    std::vector<const QuestDefinition*> out;
    out.swap(finished_);
    return out;
}

// PORT: what the log has queued for the renderer, taken.
std::vector<QuestProgressText> QuestLog::take_progress_texts() {
    std::vector<QuestProgressText> out;
    out.swap(progress_texts_);
    return out;
}

// PORT: a quest event of this kind handed to `quest_event`, which holds the rule.
std::vector<const QuestDefinition*> QuestLog::unit_defeated(QuestEventUnit& unit) {
    return quest_event(1, unit, false);
}

// PORT: a quest event of this kind handed to `quest_event`, which holds the rule.
std::vector<const QuestDefinition*> QuestLog::item_picked_up(QuestEventUnit& unit) {
    return quest_event(0, unit, false);
}

// PORT: a quest event of this kind handed to `quest_event`, which holds the rule.
std::vector<const QuestDefinition*> QuestLog::unit_interacted(QuestEventUnit& unit,
                                                              bool by_player) {
    return quest_event(2, unit, by_player);
}

// PORT: a quest event of this kind handed to `quest_event`, which holds the rule.
std::vector<const QuestDefinition*> QuestLog::item_dropped(QuestEventUnit& unit) {
    return quest_event(5, unit, false);
}

// PORT: a quest event of this kind handed to `quest_event`, which holds the rule.
std::vector<const QuestDefinition*> QuestLog::unit_defeated(const std::string& unit_name,
                                                            const QuestUnitTag& tag) {
    QuestEventUnit unit;
    unit.name = unit_name;
    unit.tag = tag;
    return unit_defeated(unit);
}

// PORT: a quest event of this kind handed to `quest_event`, which holds the rule.
std::vector<const QuestDefinition*> QuestLog::item_picked_up(const std::string& item_name,
                                                             const QuestUnitTag& tag) {
    QuestEventUnit unit;
    unit.name = item_name;
    unit.tag = tag;
    return item_picked_up(unit);
}

}  // namespace tl::core
