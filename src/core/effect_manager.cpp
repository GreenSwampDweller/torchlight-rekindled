// `CEffectManager`: the effects one unit holds, ticked, and the totals the rest
// of the engine asks it for. See `effect_manager.h`.
#include "core/effect_manager.h"
#include "core/item_use.h"
#include "core/random.h"
#include "core/unit_themes.h"

#include <algorithm>
#include <optional>

namespace tl::core {
namespace {

// PORT: ASCII case folding, the case-blind compare of names in the port's narrow strings.
std::string upper(std::string text) {
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return text;
}

// PORT: ASCII case folding, the case-blind compare of names in the port's narrow strings.
bool same_name(const std::string& a, const std::string& b) {
    // The original compares exactly (`wmemcmp`); folding case changes nothing,
    // as no two effects of one type differ only in case.
    return upper(a) == upper(b);
}

// PORT: whether an effect's duration is one of the two words (`ALWAYS`, `INSTANT`).
bool duration_is_word(float duration) {
    return duration == kDurationAlways || duration == kDurationInstant;
}

// PORT: the list an effect's activation goes in.
int activation_index(EffectActivation activation) {
    const int index = static_cast<int>(activation);
    return index >= 0 && index < 3 ? index : static_cast<int>(EffectActivation::Dynamic);
}

}  // namespace

bool LiveEffect::expired() const {
    // `CCharacter::updateEffects` and
    // `CEffectManager::calculateEffectValues`
    // share this test, and both exclude the two word durations from it: an
    // `ALWAYS` effect dies only when something sets its flag.
    if (dead) return true;
    if (duration_is_word(effect.duration)) return false;
    return effect.duration <= elapsed;
}

// PORT: the manager handed its tables, and its values worked out again.
void EffectManager::set_context(const Context& context) {
    context_ = context;
    calculate_effect_values();
}

// PORT: one of the manager's three lists.
std::vector<LiveEffect>& EffectManager::list_for(EffectActivation activation) {
    return lists_[static_cast<size_t>(activation_index(activation))];
}

// PORT: one of the manager's three lists.
const std::vector<LiveEffect>& EffectManager::list_for(EffectActivation activation) const {
    return lists_[static_cast<size_t>(activation_index(activation))];
}

// PORT: one of the manager's three lists.
const std::vector<LiveEffect>& EffectManager::effects(EffectActivation activation) const {
    return list_for(activation);
}

// PORT: every effect of the three lists.
std::vector<const LiveEffect*> EffectManager::all_effects() const {
    std::vector<const LiveEffect*> out;
    for (const std::vector<LiveEffect>& list : lists_) {
        for (const LiveEffect& live : list) out.push_back(&live);
    }
    return out;
}

void EffectManager::retire_matching(const Effect& effect) {
    // `CEffectManager::addNewEffect`'s first
    // half: an exclusive new effect retires the one it replaces, over all three
    // lists, matching name *and* type, touching only an effect that is
    // wordless-durationed or has ticked. A non-exclusive one stacks.
    if (!effect.exclusive) return;
    for (std::vector<LiveEffect>& list : lists_) {
        for (LiveEffect& live : list) {
            if (live.dead) continue;
            if (live.effect.type != effect.type) continue;
            if (!same_name(live.effect.name, effect.name)) continue;
            const bool word = duration_is_word(live.effect.duration);
            if (!word && live.elapsed >= live.effect.duration) continue;
            if (!word) live.elapsed = live.effect.duration;
            live.dead = true;
        }
    }
}

// PORT: the effects of one type in the three lists.
std::vector<LiveEffect*> EffectManager::live_effects_of_type(int type) {
    std::vector<LiveEffect*> out;
    for (std::vector<LiveEffect>& list : lists_) {
        for (LiveEffect& live : list) {
            if (live.effect.type == type) out.push_back(&live);
        }
    }
    return out;
}

// `CEffectManager::getAffix`, asked only
// whether it finds one.
bool EffectManager::has_affix(const std::string& name) const {
    for (const LiveAffix& affix : affixes_) {
        if (same_name(affix.name, name)) return true;
    }
    return false;
}

// `CEffectManager::getAffix(name)`, `+0xa4`
// written: a held affix's countdown set again.
bool EffectManager::set_affix_duration(const std::string& name, float duration) {
    for (LiveAffix& affix : affixes_) {
        if (!same_name(affix.name, name)) continue;
        affix.duration = duration;
        return true;
    }
    return false;
}

bool EffectManager::offer_effect(const Effect& effect, const std::string& affix,
                                 const std::string& skill) {
    // `applyEffectOnUnit` decides on the effect as offered, then
    // `CBaseUnit::copyEffect` -> `cloneEffect`
    // holds `new CEffect(effect)`, the clone.
    if (!context_.on_unit) {
        add_effect(clone_effect(effect), affix, skill);
        return true;
    }
    Effect copy = effect;
    // An affix of this manager's own is added as it is, unrolled.
    offering_copy_ = affix.empty();
    const bool taken = context_.on_unit(copy);
    offering_copy_ = false;
    if (!taken) return false;
    add_effect(clone_effect(copy), affix, skill);
    return true;
}

const LiveEffect* EffectManager::get_effect(int type) const {
    // `CEffectManager::getEffect`: the lists at +0x18 and
    // +0x28, in that order (`cmplwi r5, 2`).
    for (EffectActivation activation : {EffectActivation::Passive, EffectActivation::Dynamic}) {
        for (const LiveEffect& live : list_for(activation)) {
            if (live.effect.type == type) return &live;
        }
    }
    return nullptr;
}

void EffectManager::transfer_effects(int type, const std::function<bool(const Effect&)>& valid,
                                     const std::function<void(Effect&)>& apply) const {
    // `CEffectManager::transferEffects`. The
    // list is walked by index and re-read each pass, as the original does; a
    // unit applying to itself could grow it.
    const std::vector<LiveEffect>& list = list_for(EffectActivation::Transfer);
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].effect.type != type) continue;
        if (valid && !valid(list[i].effect)) continue;
        // `CEffect(type, +0x1c duration, +0xdc applied value, 0,
        // DYNAMIC, 0.0, 0)`, then `+0x2b` (`SAVE`) 0, `+0x29` (`EXCLUSIVE`) and
        // `+0x124` (the unit theme) from the source.
        Effect made =
            value_effect(type, list[i].effect.duration, list[i].value, EffectActivation::Dynamic);
        made.save = false;
        made.exclusive = list[i].effect.exclusive;
        made.unit_theme = list[i].effect.unit_theme;
        // `applyEffectOnUnit(target, owner, made)`, then `made` is deleted and
        // `clearOutDescriptions` empties the manager's text
        // cache, which the port does not keep.
        if (apply) apply(made);
    }
}

LiveEffect& EffectManager::add_effect(const Effect& effect, const std::string& affix,
                                      const std::string& skill) {
    // `CEffectManager::addNewEffect(CEffect*)`,
    // with the value `CEffect::calculateBaseValue(Roll)`
    // stores computed here.
    made_ = true;
    retire_matching(effect);

    LiveEffect live;
    live.effect = effect;
    live.affix = affix;
    live.skill = skill;
    live.source = applying_source_;
    if (context_.on_activate) context_.on_activate(live.effect);
    // `CEffect::CEffect` resolves `UNITTHEME`
    // (+0x130) against the theme table when the effect is built. DIVERGENCE:
    // the port's `Effect` is parsed without the table, so the name is resolved
    // here, when the effect is added; the table is loaded once, before any
    // effect, so the pointer is the same.
    if (context_.theme_book != nullptr && !effect.unit_theme.empty()) {
        live.theme = context_.theme_book->get_unit_theme(effect.unit_theme);
    }
    if (effect.rolled_value) {
        // The console's manager holds the effect itself
        // (`CEffectManager::cloneEffect` makes no copy of an
        // affix's own), with the value its last roll left: `EffectRolls` ran them.
        live.value = *effect.rolled_value;
    } else if (context_.graphs != nullptr) {
        // `Roll` is the original's argument. INVENTED: an effect whose rolls
        // the port does not run yet (`Effect::rolled_value` empty) takes its
        // low end; the console draws `randomBetweenVolatile(smaller, larger)`
        // on every `calculateBaseValue(0)` (the constructors,
        // `setOwner`, `CAffix::setPercent`, ...) and keeps the last.
        live.value =
            effect_base_value(live.effect, EffectCalculate::Roll, *context_.graphs, context_.owner);
    }
    std::vector<LiveEffect>& list = list_for(effect.activation);
    list.push_back(live);
    // The totals are left alone: `addNewEffect` does not recalculate them, the
    // unit's update does (`CCharacter::update`).
    return list.back();
}

void EffectManager::add_affix(const Affix& affix, uint32_t level, float duration,
                              const std::string& skill, std::optional<int32_t> owner_level,
                              const EffectRolls& rolls) {
    // `CEffectManager::addAffix(CAffix*, unsigned int, CBaseUnit*, float)`:
    // a positive `duration` is the affix's
    // countdown (`+0xc4`), then `setLevel` unless the level is -1,
    // the affix onto the list, `setOwner`, `addEffectsToEffectManager` and
    // `calculateEffectValues`.
    made_ = true;
    LiveAffix live;
    live.name = affix.name;
    live.skill = skill;
    live.duration = duration > 0.0f ? duration : affix.duration;
    live.id = ++last_affix_id_;
    live.rolls = rolls;
    live.attached = true;

    Affix owned = affix;
    if (rolls.on()) {
        // `setLevel` takes the affix's template (+0x08); the affix here is that
        // template's copy at the same level (`getNewAffixByName`, an item's
        // enchant), which stands in for it as `add_affix_to_item` says.
        if (level != 0xffffffffu) affix_set_level(owned, owned, level, rolls);
        std::optional<uint32_t> owner;
        if (owner_level) owner = static_cast<uint32_t>(*owner_level);
        affix_set_owner(owned, owner, rolls);
        affix_add_effects_rolls(owned, rolls);
    }
    // `CAffix::setSkillOwner` -> `CAffix::setOwner` -> `CEffect::setOwner(owner,
    // true)` on each effect, before the level.
    for (Effect& effect : owned.effects) set_owner_level(effect, owner_level);
    // `CAffix::addEffectsToEffectManager`:
    // the affix's effects, at the affix's level, each onto the manager.
    const Affix scaled = affix_at_level(owned, level);
    live.effects = scaled.effects;
    for (size_t slot = 0; slot < live.effects.size(); ++slot) {
        live.held.push_back(static_cast<uint32_t>(slot));
    }
    const uint32_t id = live.id;
    affixes_.push_back(std::move(live));
    for (size_t slot = 0; slot < scaled.effects.size(); ++slot) {
        const Effect& effect = scaled.effects[slot];
        // `addEffectsToEffectManager` clones each (`cloneEffect`), sets its affix
        // owner and activates it (`CBaseUnit::activateEffect`) - never through `applyEffectOnUnit`,
        // whose value re-roll and provocation are a skill event's (`offer_effect`): a `MONSTER_HUNT`
        // added to a unit whose manager the World hooks provoked it (MAIN:19, 2026-11-10).
        add_effect(clone_effect(effect), affix.name, skill);
        LiveEffect& held = list_for(effect.activation).back();
        held.affix_id = id;
        held.affix_slot = static_cast<uint32_t>(slot);
    }
    calculate_effect_values();
}

void EffectManager::clear_effects_from_owner(LiveAffix& affix) {
    // `CAffix::clearEffectsFromOwner()`: only
    // while the affix is on a manager; each effect loses its affix, rolls again
    // (`calculateBaseValue(0)`), and the first entry of its list
    // holding it is replaced by the list's last; then
    // the affix leaves the manager. Its call on `+0x40` has nothing
    // to reach in the port.
    if (!affix.attached) return;
    for (const uint32_t slot : affix.held) {
        Effect& effect = affix.effects[slot];
        affix.rolls.roll(effect);
        std::vector<LiveEffect>& list = list_for(effect.activation);
        for (size_t index = 0; index < list.size(); ++index) {
            if (list[index].affix_id != affix.id || list[index].affix_slot != slot) continue;
            list[index] = list.back();
            list.pop_back();
            break;
        }
    }
    affix.attached = false;
}

void EffectManager::delete_effect(const LiveEffect& live) {
    // `CEffect::~CEffect`: an effect an affix owns (`+0xd8`)
    // leaves that affix's list (+0xb0), the list's last moved into its place.
    // Its `setOwner(null, false)` draws nothing.
    if (live.affix_id == 0) return;
    for (LiveAffix& affix : affixes_) {
        if (affix.id != live.affix_id) continue;
        for (size_t k = 0; k < affix.held.size(); ++k) {
            if (affix.held[k] != live.affix_slot) continue;
            affix.held[k] = affix.held.back();
            affix.held.pop_back();
            break;
        }
        return;
    }
}

void EffectManager::update_affixes(float dt) {
    // `CEffectManager::updateAffixes(float)`.
    // An expired affix is removed by swapping the last into its place and
    // re-reading that index; kept, since `getEffectValue` sums in list order and
    // float addition is not associative.
    bool removed = false;
    size_t index = 0;
    while (index < affixes_.size()) {
        LiveAffix& affix = affixes_[index];
        bool expired = false;
        if (affix.duration >= 0.0f) {
            // `CAffix::updateAffixDuration`: counting down begins at
            // zero, so an affix handed a duration of exactly 0 expires now.
            affix.duration -= dt;
            if (affix.duration <= 0.0f) {
                affix.duration = 0.0f;
                expired = true;
            }
        }
        if (!expired) {
            ++index;
            continue;
        }
        // The affix is deleted (`~CAffix` runs
        // `clearEffectsFromOwner`), then the list's last takes its place.
        clear_effects_from_owner(affix);
        affixes_[index] = std::move(affixes_.back());
        affixes_.pop_back();
        removed = true;
    }
    if (removed) calculate_effect_values();
}

void EffectManager::update_effects(float dt) {
    // The effect half of `CCharacter::updateEffects`:
    // only a finite duration advances, so
    // `ALWAYS` and `INSTANT` never age.
    for (std::vector<LiveEffect>& list : lists_) {
        for (LiveEffect& live : list) {
            if (duration_is_word(live.effect.duration)) continue;
            live.elapsed += dt;
        }
    }
}

std::vector<std::string> EffectManager::take_effect_fx() {
    // `CCharacter::updateEffects` walks lists
    // 0 and 1, not `TRANSFER`. The call is on every
    // effect's path, one that has just run out included.
    std::vector<std::string> names;
    for (EffectActivation activation : {EffectActivation::Passive, EffectActivation::Dynamic}) {
        for (LiveEffect& live : list_for(activation)) {
            // `CEffect::fxShouldPlay`: no particle yet, and a name.
            if (live.fx_started || live.effect.particle_fx.empty()) continue;
            live.fx_started = true;
            names.push_back(live.effect.particle_fx);
        }
    }
    return names;
}

void EffectManager::apply_link(const std::string& link_name) {
    // `CEffectManager::deleteDeadEffects`: an
    // effect on its way out takes with it every effect whose own `NAME` matches
    // its `LINK_NAME`, by forcing a timed effect's elapsed time up to its
    // duration (`+0x20` set: `+0x10 = +0x1c`) and setting its flag (`+0x2c`).
    // The scan covers all three lists, including the one the dying effect came
    // from, and the dying effect itself.
    if (link_name.empty()) return;
    for (std::vector<LiveEffect>& list : lists_) {
        for (LiveEffect& live : list) {
            if (!same_name(live.effect.name, link_name)) continue;
            if (!duration_is_word(live.effect.duration)) live.elapsed = live.effect.duration;
            live.dead = true;
        }
    }
}

// `CEffectManager::removeNonSavedEffects()` (inlined in
// `CPlayer::levelResetting` on the console).
void EffectManager::remove_non_saved_effects() {
    for (std::vector<LiveEffect>& list : lists_) {
        for (LiveEffect& live : list) {
            // `+0x33` (X360 `+0x2b`), `SAVE`.
            if (live.effect.save) continue;
            // The console tests the timed flag (`+0x20`) for the copy
            // (`+0x10 = +0x1c`).
            const float duration = live.effect.duration;
            if (duration != kDurationInstant && duration != kDurationAlways) {
                live.elapsed = duration;
            }
            live.dead = true;  // `+0x34` (X360 `+0x2c`)
        }
    }
    delete_dead_effects();
}

// PORT: the port's rebuilt list put back in the order the console's swap removals
// left it (`ItemStats::passive_order`); the console never rebuilds its list.
void EffectManager::reorder(EffectActivation activation, const std::vector<uint32_t>& order) {
    std::vector<LiveEffect>& list = list_for(activation);
    if (order.size() > list.size()) return;
    std::vector<LiveEffect> head;
    head.reserve(order.size());
    std::vector<bool> taken(order.size(), false);
    for (const uint32_t from : order) {
        if (from >= order.size() || taken[from]) return;
        taken[from] = true;
    }
    for (const uint32_t from : order) head.push_back(list[from]);
    for (size_t i = 0; i < head.size(); ++i) list[i] = std::move(head[i]);
}

void EffectManager::delete_dead_effects() {
    // `CEffectManager::deleteDeadEffects()`:
    // in each list by index, an effect goes when its duration is `INSTANT`
    // (`+0x1c == -900`), or it is timed (`+0x20`) and its time has run out, or
    // its flag (`+0x2c`) is set. Its `LINK_NAME`, when it has one, kills what it
    // names first; then it is deleted unless an affix owns it (`+0xd8`), and
    // the list's last takes its place and is read next. Nothing is
    // recalculated (`clearOutDescriptions` only): `CCharacter::update` runs
    // `calculateEffectValues` before this call, so a removed
    // effect still counts until the next.
    for (std::vector<LiveEffect>& list : lists_) {
        size_t index = 0;
        while (index < list.size()) {
            if (list[index].effect.duration != kDurationInstant && !list[index].expired()) {
                ++index;
                continue;
            }
            apply_link(list[index].effect.link_name);
            list[index] = std::move(list.back());
            list.pop_back();
        }
    }
}

bool EffectManager::add_affix_by_name(const std::string& name, uint32_t level,
                                      const AffixBook* book, float duration,
                                      const EffectRolls& rolls) {
    // `CEffectManager::addAffix(const std::wstring&, unsigned, CBaseUnit*,
    // CResourceManager*, float)`:
    // `getNewAffixByName(name, level)`, then `addAffix(affix, level, unit,
    // duration)`.
    if (book == nullptr) return false;
    const std::optional<Affix> affix = new_affix_by_name(*book, name, level, rolls);
    if (!affix) return false;
    add_affix(*affix, level, duration, std::string(), std::nullopt, rolls);
    return true;
}

void EffectManager::clear_out_affix_effects() {
    // `CEffectManager::clearOutAffixEffects()`:
    // `CAffix::clearEffectsFromOwner` on each affix in turn.
    for (LiveAffix& affix : affixes_) clear_effects_from_owner(affix);
}

void EffectManager::clear_effects(bool remove_effects) {
    // `CEffectManager::clearEffects(bool)`.
    if (remove_effects) {
        for (std::vector<LiveEffect>& list : lists_) {
            size_t index = 0;
            while (index < list.size()) {
                if (list[index].effect.type == kEffectTypePercentBlockChanceBase) {
                    ++index;
                    continue;
                }
                delete_effect(list[index]);
                list[index] = std::move(list.back());
                list.pop_back();
            }
        }
    }
    // on the affix list (+0x08): each affix deleted in order, its
    // destructor running `clearEffectsFromOwner`.
    for (LiveAffix& affix : affixes_) clear_effects_from_owner(affix);
    affixes_.clear();
    // Then the theme list (+0x298).
    themes_.clear();
}

void EffectManager::destroy(const EffectRolls& rolls) {
    // `CEffectManager::~CEffectManager()`: `clearEffects(false)` first.
    for (LiveAffix& affix : affixes_) affix.rolls = rolls;
    clear_effects(false);
}

void EffectManager::calculate_effect_values() {
    // `CEffectManager::calculateEffectValues()`:
    // one float per effect type, summed over
    // the `Passive` and `Dynamic` lists only - `Transfer` is the proc path and
    // is not part of a unit's own totals. Dead effects are left out, and the
    // four recharge types are scaled to the original's 62.5 Hz tick on the way
    // in.
    cache_.fill(0.0f);
    for (int activation = 0; activation < 2; ++activation) {
        for (const LiveEffect& live : lists_[static_cast<size_t>(activation)]) {
            const int type = live.effect.type;
            if (type < 0 || type >= kEffectTypeCacheCount) continue;
            if (live.expired()) continue;
            float value = live.value;
            if (!duration_is_word(live.effect.duration) && context_.types != nullptr &&
                effect_is_per_tick(*context_.types, type)) {
                value *= 0.016f;
            }
            cache_[static_cast<size_t>(type)] += value;
        }
    }

    // The same pass rebuilds the theme list (+0x2c8). Its loop runs type by type
    // (0 to 0x90), then list by list, then over the list's live effects of that
    // type, and appends each effect's theme (+0x130) once. So the list is in type
    // order, and an effect of a type outside that range gives no theme.
    // Taken here list by list and sorted by type, stably: the same order, one walk.
    themes_.clear();
    constexpr int kThemePassTypes = 0x91;
    std::vector<std::pair<int, const UnitTheme*>>& held = theme_scratch_;
    held.clear();
    for (int activation = 0; activation < 2; ++activation) {
        for (const LiveEffect& live : lists_[static_cast<size_t>(activation)]) {
            const int type = live.effect.type;
            if (type < 0 || type >= kThemePassTypes || live.expired() || live.theme == nullptr) {
                continue;
            }
            held.emplace_back(type, live.theme);
        }
    }
    std::stable_sort(held.begin(), held.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& entry : held) {
        if (std::find(themes_.begin(), themes_.end(), entry.second) == themes_.end()) {
            themes_.push_back(entry.second);
        }
    }
}

float EffectManager::effect_value(int type, int damage_type) const {
    // `CEffectManager::getEffectValue(EEFFECT_TYPE, EDAMAGE_TYPES)`.
    if (type < 0 || type >= kEffectTypeCacheCount) return 0.0f;
    if (damage_type == kDamageTypeAny) return cache_[static_cast<size_t>(type)];
    if (cache_[static_cast<size_t>(type)] == 0.0f) return 0.0f;

    float total = 0.0f;
    for (int activation = 0; activation < 2; ++activation) {
        for (const LiveEffect& live : lists_[static_cast<size_t>(activation)]) {
            if (live.effect.type != type) continue;
            if (static_cast<int>(live.effect.damage_type) != damage_type) continue;
            // The original does not re-test for death here: only the cache being
            // zero keeps a dead effect out of this sum. Kept.
            float value = live.value;
            if (!duration_is_word(live.effect.duration) && context_.types != nullptr &&
                effect_is_per_tick(*context_.types, type)) {
                value *= 0.016f;
            }
            total += value;
        }
    }
    return total;
}

float EffectManager::effect_value(EffectActivation activation, int type, int damage_type) const {
    // `CEffectManager::getEffectValue(EEFFECT_ACTIVATION, EEFFECT_TYPE,
    // EDAMAGE_TYPES)`: the same sum over one
    // list. Note that unlike the two-argument form it has no `kDamageTypeAny`
    // shortcut - the cache still gates it, but the scan always runs.
    if (type < 0 || type >= kEffectTypeCacheCount) return 0.0f;
    if (cache_[static_cast<size_t>(type)] == 0.0f) return 0.0f;

    float total = 0.0f;
    for (const LiveEffect& live : list_for(activation)) {
        if (live.effect.type != type) continue;
        if (damage_type != kDamageTypeAny &&
            static_cast<int>(live.effect.damage_type) != damage_type) {
            continue;
        }
        float value = live.value;
        if (!duration_is_word(live.effect.duration) && context_.types != nullptr &&
            effect_is_per_tick(*context_.types, type)) {
            value *= 0.016f;
        }
        total += value;
    }
    return total;
}

float EffectManager::effect_value(int type, const std::string& name) const {
    // `CEffectManager::getEffectValue(EEFFECT_TYPE, const std::wstring&)`:
    // the by-name form. `+N to <skill>` is
    // exactly this - a `SKILL BONUS` effect whose `NAME` is the skill it
    // raises.
    if (type < 0 || type >= kEffectTypeCacheCount) return 0.0f;
    if (cache_[static_cast<size_t>(type)] == 0.0f) return 0.0f;

    float total = 0.0f;
    for (int activation = 0; activation < 2; ++activation) {
        for (const LiveEffect& live : lists_[static_cast<size_t>(activation)]) {
            if (live.effect.type != type) continue;
            if (!same_name(live.effect.name, name)) continue;
            float value = live.value;
            if (!duration_is_word(live.effect.duration) && context_.types != nullptr &&
                effect_is_per_tick(*context_.types, type)) {
                value *= 0.016f;
            }
            total += value;
        }
    }
    return total;
}

bool EffectManager::has_effect(int type) const {
    // `CEffectManager::hasEffect(EEFFECT_TYPE)`: the passive and
    // dynamic lists, the type alone; a dead or run-out effect still counts until it is removed.
    for (size_t index = 0; index < 2; ++index) {
        for (const LiveEffect& live : lists_[index]) {
            if (live.effect.type == type) return true;
        }
    }
    return false;
}

int EffectManager::spend_shield_buffers(int damage, int damage_type, bool& absorbed) {
    // `CCharacter::modifyDamage`: `+0x18` and `+0x28` of the
    // unit's manager, in that order.
    for (size_t index = 0; index < 2; ++index) {
        std::vector<LiveEffect>& list = lists_[index];
        for (LiveEffect& live : list) {
            const int type = static_cast<int>(live.effect.damage_type);
            if (live.effect.type == kEffectTypeShieldBuffer && !live.expired() &&
                (type == damage_type || type == static_cast<int>(DamageType::All))) {
                // `CEffect::value(0)`.
                const float held = context_.graphs != nullptr
                                       ? effect_column_value(live.effect, 0, *context_.graphs)
                                       : live.effect.values[0];
                const float hit = static_cast<float>(damage);
                // `+0xe0` then `CEffect::calculateBaseValue(0)`.
                const auto set_first_value = [this, &live](float value) {
                    live.effect.values[0] = value;
                    if (context_.graphs != nullptr) {
                        live.value =
                            effect_base_value(live.effect, EffectCalculate::Roll, *context_.graphs,
                                              context_.owner, &volatile_random());
                    }
                };
                if (hit <= held) {
                    set_first_value(held - hit);
                    absorbed = true;
                    return 0;
                }
                // `fctiwz`, then the shield spent.
                damage -= static_cast<int>(held);
                set_first_value(0.0f);
                if (!duration_is_word(live.effect.duration)) live.elapsed = live.effect.duration;
                live.dead = true;
            }
            if (damage < 1) {
                absorbed = true;
                return damage;
            }
        }
        if (damage < 1) {
            absorbed = true;
            return damage;
        }
    }
    return damage;
}

bool EffectManager::has_effect_named(const std::string& name) const {
    // `CEffectManager::hasEffect(const std::wstring&)`:
    // the first two lists only (`+0x28`,
    // `+0x40`), a `wmemcmp` of the whole name, and no test of the dead flag.
    for (int activation = 0; activation < 2; ++activation) {
        for (const LiveEffect& live : lists_[static_cast<size_t>(activation)]) {
            if (live.effect.name == name) return true;
        }
    }
    return false;
}

bool EffectManager::has_effect_from_skill(const std::string& skill) const {
    // `CEffectManager::hasEffectFromSkill(CSkill*)`.
    for (const std::vector<LiveEffect>& list : lists_) {
        for (const LiveEffect& live : list) {
            if (!live.skill.empty() && same_name(live.skill, skill)) return true;
        }
    }
    return false;
}

namespace {

/// The test `updateHP` puts each effect.
bool credits_a_death(const LiveEffect& live, const StatBook* graphs) {
    const int type = live.effect.type;
    if (!live.source.set) return false;
    if (type == kEffectTypeDamage) return true;
    if (type != kEffectTypeHpRecharge && type != kEffectTypeHpRechargePlayer) return false;
    // `CEffect::value(0)`.
    const float value =
        graphs != nullptr ? effect_column_value(live.effect, 0, *graphs) : live.effect.values[0];
    return value < 0.0f;
}

}  // namespace

// PORT: who `take_death_credit` would credit, nothing taken.
EffectSource EffectManager::death_credit() const {
    EffectSource credited;
    for (size_t list = 0; list < 2; ++list) {
        for (const LiveEffect& live : lists_[list]) {
            if (!credits_a_death(live, context_.graphs)) continue;
            credited = live.source;
            break;
        }
    }
    return credited;
}

EffectSource EffectManager::take_death_credit() {
    // `CCharacter::updateHP`:
    // the lists at manager +0x18 and +0x28, each walked until an
    // effect credits.
    EffectSource credited;
    for (size_t list = 0; list < 2; ++list) {
        for (LiveEffect& live : lists_[list]) {
            if (!credits_a_death(live, context_.graphs)) continue;
            credited = live.source;
            live.source = EffectSource{};
            break;
        }
    }
    return credited;
}

bool EffectManager::remove_effect(const std::string& name) {
    // `CEffectManager::removeEffect(const std::wstring&, bool)`:
    // every effect of that name, in all
    // three lists, deleted and its place taken by the list's last (read again);
    // the `bool` is not read. The totals are left alone (`clearOutDescriptions`
    // only).
    bool removed = false;
    for (std::vector<LiveEffect>& list : lists_) {
        size_t index = 0;
        while (index < list.size()) {
            if (!same_name(list[index].effect.name, name)) {
                ++index;
                continue;
            }
            // Deleted, whoever owns it (`~CEffect`).
            delete_effect(list[index]);
            list[index] = list.back();
            list.pop_back();
            removed = true;
        }
    }
    return removed;
}

bool EffectManager::delete_affix(const std::string& name) {
    // `CEffectManager::deleteAffix(const std::wstring&)`:
    // each affix of that name in turn
    // `clearEffectsFromOwner`, deleted, and the list's last put in its place
    // and read next; when one went, `calculateEffectValues`.
    bool removed = false;
    size_t index = 0;
    while (index < affixes_.size()) {
        if (!same_name(affixes_[index].name, name)) {
            ++index;
            continue;
        }
        clear_effects_from_owner(affixes_[index]);
        affixes_[index] = std::move(affixes_.back());
        affixes_.pop_back();
        removed = true;
    }
    if (removed) calculate_effect_values();
    return removed;
}

}  // namespace tl::core
