// An effect held on a unit, ticked, and the totals the rest of the engine asks
// it for: the apply side of `core/effects.h`. `CEffectManager`:
// three lists of effects, one per
// `EEFFECT_ACTIVATION`, the affixes that own them, and a float per effect type
// caching the total every query reads. The cache is kept because its staleness
// is observable: `getEffectValue` returns 0 without scanning when a type's
// cached total is 0.
#pragma once

#include "core/affix_book.h"
#include "core/effects.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tl::core {

class UnitThemeBook;
struct UnitTheme;

/// `EDAMAGE_TYPES` one past `All`: no damage-type filter.
/// `CEffectManager::getEffectValue` answers it from the cache.
inline constexpr int kDamageTypeAny = 7;

/// The cache at `CEffectManager +0x80` runs to `+0x2c8`: 146 floats.
inline constexpr int kEffectTypeCacheCount = 146;

/// The effect types read by index, as the engine's own code reads them (rows of
/// `media/EffectsList.dat`).
inline constexpr int kEffectTypeMeleeDamageBonus = 0x00;        ///< `MELEEDAMAGEBONUS`
inline constexpr int kEffectTypeRangedDamageBonus = 0x01;       ///< `RANGEDDAMAGEBONUS`
inline constexpr int kEffectTypeDefense = 0x02;                 ///< `DEFENSE`
inline constexpr int kEffectTypeMagic = 0x03;                   ///< `MAGIC`
inline constexpr int kEffectTypeMaxMana = 0x04;                 ///< `MAX MANA`
inline constexpr int kEffectTypeMaxHp = 0x05;                   ///< `MAX HP`
inline constexpr int kEffectTypeArmorBonus = 0x08;              ///< `ARMOR BONUS`
inline constexpr int kEffectTypePercentMana = 0x13;             ///< `PERCENT MANA`
inline constexpr int kEffectTypePercentHp = 0x14;               ///< `PERCENT HP`
inline constexpr int kEffectTypePercentPetHealth = 0x68;        ///< `PERCENT PET HEALTH`
inline constexpr int kEffectTypeDamageBonus = 0x0a;             ///< `DAMAGE BONUS`
inline constexpr int kEffectTypePercentMeleeDamage = 0x0f;      ///< `PERCENT MELEE DAMAGE`
inline constexpr int kEffectTypePercentRangedDamage = 0x10;     ///< `PERCENT RANGED DAMAGE`
inline constexpr int kEffectTypePercentDefense = 0x11;          ///< `PERCENT DEFENSE`
inline constexpr int kEffectTypePercentMagic = 0x12;            ///< `PERCENT MAGIC`
inline constexpr int kEffectTypePercentArmorBonus = 0x17;       ///< `PERCENT ARMOR BONUS`
inline constexpr int kEffectTypePercentDamageBonus = 0x19;      ///< `PERCENT DAMAGE BONUS`
inline constexpr int kEffectTypePercentDamageTaken = 0x1a;      ///< `PERCENT DAMAGE TAKEN`
inline constexpr int kEffectTypeShieldBuffer = 0x7a;            ///< `SHIELD BUFFER`
inline constexpr int kEffectTypePercentBlockChance = 0x21;      ///< `PERCENT BLOCK CHANCE`
inline constexpr int kEffectTypeCriticalChance = 0x37;          ///< `CRITICAL CHANCE`
inline constexpr int kEffectTypePercentBlockChanceBase = 0x3e;  ///< `PERCENT BLOCK CHANCE BASE`
inline constexpr int kEffectTypeDegradeArmorEffect = 0x42;      ///< `DEGRADE ARMOR EFFECT`
inline constexpr int kEffectTypeXpGainBonus = 0x44;             ///< `XP GAIN BONUS`
inline constexpr int kEffectTypeStrengthBonus = 0x45;           ///< `STRENGTH BONUS`
inline constexpr int kEffectTypeDexterityBonus = 0x46;          ///< `DEXTERITY BONUS`
inline constexpr int kEffectTypePercentStrengthBonus = 0x47;    ///< `PERCENT STRENGTH BONUS`
inline constexpr int kEffectTypePercentDexterityBonus = 0x48;   ///< `PERCENT DEXTERITY BONUS`
inline constexpr int kEffectTypeFameGainBonus = 0x4d;           ///< `FAME GAIN BONUS`
inline constexpr int kEffectTypeSkillBonus = 0x52;              ///< `SKILL BONUS`
inline constexpr int kEffectTypeAttackSpellBonus = 0x55;        ///< `ATTACK SPELL BONUS`
inline constexpr int kEffectTypeDefenseSpellBonus = 0x56;       ///< `DEFENSE SPELL BONUS`
inline constexpr int kEffectTypeCharmSpellBonus = 0x57;         ///< `CHARM SPELL BONUS`
inline constexpr int kEffectTypeDualWieldingBonus = 0x58;       ///< `DUAL WIELDING BONUS`
inline constexpr int kEffectTypePercentPetDamage = 0x5b;        ///< `PERCENT PET DAMAGE`
inline constexpr int kEffectTypePercentPetArmor = 0x61;         ///< `PERCENT PET ARMOR`
/// `PERCENT MARTIAL ITEM DAMAGE BONUS`
inline constexpr int kEffectTypePercentMartialItemDamageBonus = 0x63;
/// `PERCENT RANGED ITEM DAMAGE BONUS`
inline constexpr int kEffectTypePercentRangedItemDamageBonus = 0x66;
/// `PERCENT MAGIC ITEM DAMAGE BONUS`
inline constexpr int kEffectTypePercentMagicItemDamageBonus = 0x67;
/// `SILENCE`, row 128 of `media/EffectsList.dat`. `CSkill::requirementsMet`
/// refuses a skill outright when the caster
/// holds one, gated on the skill's own `CSkill +0x70` flag.
inline constexpr int kEffectTypeSilence = 0x80;
/// `DESUMMON ON DEATH`, row 129: a unit spawner's `DESTROY BODY` puts it on what it releases.
inline constexpr int kEffectTypeDesummonOnDeath = 0x81;

/// `CEffect +0x48`: the unit that put the effect on - a skill's caster
/// (`CSkillEffectAndAffixes::applyAffixesAndEffects`), a blow's
/// attacker (`CCharacter::applyDamageEffects`) - which
/// `CCharacter::updateHP` credits with a death by it. The machine holding the
/// unit and its node; a null machine with `set` is the player.
struct EffectSource {
    bool set = false;
    const void* machine = nullptr;
    int64_t node = 0;
    // PORT: an accessor of an effect's source.
    bool player() const { return set && machine == nullptr; }
};

/// One effect live on a unit: the authored block, already scaled to the affix's
/// level by `affix_at_level()`, plus the three fields that applying it settles.
struct LiveEffect {
    Effect effect;
    /// `CEffect +0xc0`: what `CEffect::calculateBaseValue(Roll)`
    /// stored. Every `getEffectValue` sums this, so
    /// the contribution is fixed when the effect is applied.
    float value = 0.0f;
    /// `CEffect +0x18`: time since it was applied, counting up. An `ALWAYS` or
    /// `INSTANT` duration never advances it (`CCharacter::updateEffects`).
    ///
    float elapsed = 0.0f;
    /// `CEffect +0x34`: set when a newer copy of the same effect supersedes this
    /// one, or when another effect's `LINK_NAME` names it. A set flag is death
    /// on its own, whatever the duration says.
    bool dead = false;
    /// The affix that owns the effect, by `NAME`, empty for a free-standing one.
    std::string affix;
    /// Which held affix (`LiveAffix::id`, 0 for none) and which of its effects
    /// this is: `CAffix::clearEffectsFromOwner` removes an
    /// affix's own effect objects, one by one in the affix's order.
    uint32_t affix_id = 0;
    uint32_t affix_slot = 0;
    /// `CEffect::setSkillOwner`: the skill whose event applied it,
    /// which `hasEffectFromSkill` asks about.
    std::string skill;
    /// `CEffect +0x130`: theme pointer resolved from `effect.unit_theme`.
    const UnitTheme* theme = nullptr;
    /// `CEffect +0x48`: whose effect it is (`EffectSource`); unset for one
    /// nobody put on (a unit's own, an item's).
    EffectSource source;
    /// `CEffect +0xa8` is not null: the effect's `PARTICLE_FX` has been made
    /// (`CEffect::playFX`), which it is once.
    bool fx_started = false;

    /// The alive test `CCharacter::updateEffects`
    /// and
    /// `CEffectManager::calculateEffectValues`
    /// share: a finite duration that has run
    /// out, or the flag.
    bool expired() const;
};

/// One affix live on a unit. `CAffix +0xa4` is the countdown
/// `CAffix::updateAffixDuration` runs; a negative value never
/// expires, and **zero expires on the first tick**, because the test that skips
/// the countdown is `duration < 0` and not `duration <= 0`.
struct LiveAffix {
    std::string name;
    float duration = -1.0f;
    std::string skill;
    /// PORT: the instance, which the effects it put on the lists name
    /// (`LiveEffect::affix_id`); the console's are pointers.
    uint32_t id = 0;
    /// `CAffix +0xb0`: its effects as they were added (the objects the lists
    /// hold, on the console), and the rolls they were added with, which
    /// `clearEffectsFromOwner` runs again (`calculateBaseValue(0)`).
    std::vector<Effect> effects;
    EffectRolls rolls;
    /// `CAffix +0xb0` as it stands: the slots of `effects` still held, in the
    /// list's order. Deleting an effect (`~CEffect`) takes it
    /// off its affix's list, the last moved into its place.
    std::vector<uint32_t> held;
    /// `CAffix +0x1c` is set: the manager it is on, which
    /// `clearEffectsFromOwner` clears - a second call does nothing.
    bool attached = false;
};

/// `CEffectManager`: the effects one unit holds.
class EffectManager {
public:
    /// What an effect needs to work out its own number when it is applied - the
    /// same three things `effect_base_value()` takes. The manager holds them
    /// because the original's effects reach them through their owner unit.
    struct Context {
        const EffectTypeTable* types = nullptr;
        const StatBook* graphs = nullptr;
        EffectOwner owner;
        const UnitThemeBook* theme_book = nullptr;
        /// The owner unit's `isEffectValidForUnit` and `applyEffectOnUnit`, which
        /// `CAffix::addEffectsToEffectManager`
        /// hands every effect an affix brings: true copies it in
        /// (`CBaseUnit::copyEffect`), false leaves it
        /// out. Unset, every effect is copied (the player's manager). The effect
        /// is the copy about to be held.
        std::function<bool(Effect&)> on_unit;
        /// The owner unit's `CBaseUnit::activateEffect`,
        /// run on every effect as it comes to be held (`createEffect`,
        /// `cloneEffect`, `CAffix::addEffectsToEffectManager`, `applySaveState`),
        /// before its value is worked out: a cast-skill effect teaches its unit
        /// the skill it names (`core/weapon_procs.h`).
        std::function<void(Effect&)> on_activate;
    };

    void set_context(const Context& context);
    const Context& context() const { return context_; }

    /// `CEffectManager::addAffix(CAffix*, unsigned int, CBaseUnit*, float)`,
    /// with
    /// `CAffix::addEffectsToEffectManager`
    /// folded in: a positive `duration` overrides the affix's own, `level`
    /// scales it, each effect is applied, and the cache is recalculated.
    /// `owner_level` is a skill's caster's level, which `CAffix::setSkillOwner`
    /// and `CAffix::setOwner`
    /// put on every effect first (`CEffect::setOwner(owner,
    /// true)`, for `USEOWNERLEVEL`). With `rolls` on, the console's
    /// `g_RandVolatile` draws run too (`CAffix::setLevel`, `setOwner`,
    /// `addEffectsToEffectManager`; `Effect::rolled_value`); the character
    /// branch is `Context::on_unit`.
    void add_affix(const Affix& affix, uint32_t level, float duration = -1.0f,
                   const std::string& skill = std::string(),
                   std::optional<int32_t> owner_level = std::nullopt,
                   const EffectRolls& rolls = EffectRolls{});

    /// `CEffectManager::addNewEffect(CEffect*)`
    /// for an effect offered to the unit (`Context::on_unit`): copied in when the
    /// unit takes it (`CBaseUnit::copyEffect`, -> `cloneEffect`).
    /// Whether it was.
    bool offer_effect(const Effect& effect, const std::string& affix = std::string(),
                      const std::string& skill = std::string());
    bool has_unit_hook() const { return static_cast<bool>(context_.on_unit); }
    /// Inside `Context::on_unit`: whether a take is `cloneEffect`'s new copy (true) or an
    /// effect of this manager's own affix (false).
    bool offering_copy() const { return offering_copy_; }
    /// Whether the character's manager (`+0x17c`) exists: made by its first affix or effect.
    bool made() const { return made_; }

    LiveEffect& add_effect(const Effect& effect, const std::string& affix = std::string(),
                           const std::string& skill = std::string());

    /// `CEffectManager::updateAffixes(float)`:
    /// every affix's duration runs down, and
    /// an expired one takes its effects with it. The cache is recalculated only
    /// when something went.
    void update_affixes(float dt);

    /// The effect half of `CCharacter::updateEffects`:
    /// a finite duration advances. `ALWAYS`
    /// and `INSTANT` do not.
    void update_effects(float dt);

    /// The particles `CCharacter::updateEffects`
    /// starts in the same walk: every
    /// `PASSIVE` or `DYNAMIC` effect whose `CEffect::fxShouldPlay`
    /// says so - a `PARTICLE_FX` named and not yet made. Their names, in list
    /// order, each marked made; the caller puts one at its unit
    /// (`CEffect::playFX`).
    std::vector<std::string> take_effect_fx();

    /// `CEffectManager::deleteDeadEffects()`:
    /// an `INSTANT` effect, a timed one whose time has run out and one whose
    /// flag is set go, each first killing every effect its `LINK_NAME` names.
    /// The cache is not touched (`clearOutDescriptions` only): the unit's next
    /// update recalculates.
    void delete_dead_effects();

    /// `CEffectManager::removeNonSavedEffects()` (inlined in
    /// `CPlayer::levelResetting` on the console): in each of
    /// the three lists, every effect that is not `SAVE`d is marked dead - a
    /// timed one with its time run out first - and then `deleteDeadEffects`.
    void remove_non_saved_effects();

    /// `CEffectManager::addAffix(const std::wstring&, unsigned int, CBaseUnit*,
    /// CResourceManager*, float)`: nothing
    /// without a resource manager (`book` null); otherwise the book's affix by
    /// that name cloned at `level` (`CResourceManager::getNewAffixByName`)
    /// and added as `add_affix` adds one. An
    /// unknown name adds nothing (`addAffix` on a null affix returns at once).
    /// Returns whether an affix was added.
    bool add_affix_by_name(const std::string& name, uint32_t level, const AffixBook* book,
                           float duration = -1.0f, const EffectRolls& rolls = EffectRolls{});

    /// `CEffectManager::clearOutAffixEffects()`:
    /// every affix takes its effects off the
    /// lists (`CAffix::clearEffectsFromOwner`, a
    /// swap-remove each through `removeEffect(CEffect*, false)`). The
    /// affixes stay, and the cache is not touched.
    void clear_out_affix_effects();

    /// `CEffectManager::clearEffects(bool)`.
    /// `remove_effects` true (`CEnchantMenu::performInteraction`) swap-removes
    /// from all three lists every effect whose type is not 0x3e (`PERCENT BLOCK
    /// CHANCE BASE`, an item's own `BLOCK_CHANCE`); false (the destructor, and
    /// `CInventory::calculateEffectValues`) leaves the lists alone. Either way
    /// every affix is deleted, which takes its own effects off the lists
    /// (`~CAffix` -> `clearEffectsFromOwner`). The cache is not
    /// touched: a caller recalculates.
    void clear_effects(bool remove_effects);

    /// `CEffectManager::~CEffectManager()`: `clearEffects(false)`, every
    /// affix's effects rolled again as `~CAffix` takes them off (`calculateBaseValue(0)`, on
    /// `rolls`) - the draws an item's deletion makes.
    void destroy(const EffectRolls& rolls);

    /// `CEffectManager::calculateEffectValues()`:
    /// the per-type cache every query reads.
    /// Dead effects are left out of it.
    void calculate_effect_values();

    /// `CEffectManager::getEffectValue(EEFFECT_TYPE, EDAMAGE_TYPES)`.
    /// `kDamageTypeAny` returns the cached total outright.
    float effect_value(int type, int damage_type = kDamageTypeAny) const;

    /// PORT: the per-type totals set from outside (for tests):
    /// what `calculate_effect_values` leaves in the cache (the effect values,
    /// X360 +0x50), until the next recalculation.
    void set_cached_values(const std::array<float, kEffectTypeCacheCount>& values) {
        cache_ = values;
    }

    /// `CEffectManager::getEffectValue(EEFFECT_ACTIVATION, EEFFECT_TYPE,
    /// EDAMAGE_TYPES)`: the same, narrowed to
    /// one activation's list.
    float effect_value(EffectActivation activation, int type, int damage_type) const;

    /// `CEffectManager::getEffectValue(EEFFECT_TYPE, const std::wstring&)`:
    /// the by-name form, which is how `+N to
    /// <skill>` finds the bonuses that name it.
    float effect_value(int type, const std::string& name) const;

    /// `CEffectManager::hasEffect(EEFFECT_TYPE)`:
    /// an effect of that type on the passive or dynamic list, dead or not.
    bool has_effect(int type) const;

    /// The `SHIELD BUFFER` (0x7a) walk of `CCharacter::modifyDamage`:
    /// the first two lists in order, each live
    /// shield of the hit's damage type or of all (6) soaking what it can. One
    /// that holds the rest keeps the difference as its number (`+0xe0`, then
    /// `CEffect::calculateBaseValue(0)`) and the damage is 0;
    /// one that does not is spent - its number 0, its time run out, its flag
    /// set - and the damage goes on less its value, truncated. `absorbed` is
    /// set once the damage is under 1 after any effect of either list, a
    /// shield or not (the console tests after every one). The damage left.
    int spend_shield_buffers(int damage, int damage_type, bool& absorbed);
    /// `CEffectManager::hasEffectFromSkill(CSkill*)`.
    bool has_effect_from_skill(const std::string& skill) const;

    /// The unit every effect held from now on is put on by
    /// (`CEffect::setOwner`), until it is set again; unset,
    /// nobody's. The callers that apply a skill's or a blow's effects set it
    /// around the apply (`EffectSourceScope`).
    void set_applying_source(const EffectSource& source) { applying_source_ = source; }
    const EffectSource& applying_source() const { return applying_source_; }

    /// `CCharacter::updateHP`, its walk once
    /// the effects have taken the HP under 1: the first two lists
    /// in order, in each the first effect that is a `DAMAGE` (0x34), or an `HP
    /// RECHARGE` / `HP RECHARGE PLAYER` (7, 0x7c) whose `value(0)` is below 0,
    /// and that has an owner. That owner is credited (the experience and fame,
    /// `awardExperience` / `awardFame`, and `die`'s attacker) and the effect
    /// loses it (`setOwner(0, true)`). A second list can credit again; the last
    /// credit is what `updateHP` returns. Unset: nobody is credited.
    EffectSource take_death_credit();
    /// Who `take_death_credit` would credit, nothing taken.
    EffectSource death_credit() const;
    /// `CEffectManager::hasEffect(const std::wstring&)`:
    /// an effect of that exact `NAME` on the
    /// passive or dynamic list, dead or not.
    bool has_effect_named(const std::string& name) const;

    /// `CEffectManager::removeEffect(const std::wstring&, bool)`
    /// and `deleteAffix(const std::wstring&)`.
    ///
    bool remove_effect(const std::string& name);
    bool delete_affix(const std::string& name);

    /// `CEffectManager::transferEffects(CCharacter*, EEFFECT_TYPE)`:
    /// each effect of `type` on the `TRANSFER` list,
    /// in order, that the struck unit's `isEffectValidForUnit` passes is made
    /// again as a new `DYNAMIC` effect (its duration and applied value, its
    /// `EXCLUSIVE` and theme, no `SAVE`, no name) and handed to the unit's
    /// `applyEffectOnUnit`.
    void transfer_effects(int type, const std::function<bool(const Effect&)>& valid,
                          const std::function<void(Effect&)>& apply) const;

    /// `CEffectManager::getEffect(EEFFECT_TYPE)`:
    /// the first held effect of `type` on the
    /// `PASSIVE` list, then the `DYNAMIC` one - never the `TRANSFER` list; null
    /// for none.
    const LiveEffect* get_effect(int type) const;

    /// The three lists, in `EEFFECT_ACTIVATION` order, and the affixes.
    const std::vector<LiveEffect>& effects(EffectActivation activation) const;
    /// PORT: the first `order.size()` entries of a list put in the order given
    /// (`new[i] = old[order[i]]`): a list the port builds again from an item's
    /// data takes back the order the console's swap removals left it in
    /// (`ItemStats::passive_order`). An order that does not fit is ignored.
    void reorder(EffectActivation activation, const std::vector<uint32_t>& order);
    const std::vector<LiveAffix>& affixes() const { return affixes_; }
    /// Every held effect of `type`, in list order, for a unit's
    /// `updateEffects` to act on (and mark spent).
    std::vector<LiveEffect*> live_effects_of_type(int type);
    /// `CEffectManager::getAffix(name)` is
    /// not null: an affix of that name is held.
    bool has_affix(const std::string& name) const;
    /// `CEffectManager::getAffix(name)` (+0xa4 written): a held affix's
    /// countdown set again. False when no affix has that name.
    bool set_affix_duration(const std::string& name, float duration);
    /// `CEffectManager +0x2c8`: live unit themes from active effects.
    const std::vector<const UnitTheme*>& themes() const { return themes_; }
    /// Every effect of every list, for a dump. Not an order the original walks.
    std::vector<const LiveEffect*> all_effects() const;

private:
    std::vector<LiveEffect>& list_for(EffectActivation activation);
    const std::vector<LiveEffect>& list_for(EffectActivation activation) const;
    void retire_matching(const Effect& effect);
    void apply_link(const std::string& link_name);
    void clear_effects_from_owner(LiveAffix& affix);
    void delete_effect(const LiveEffect& live);

    Context context_;
    EffectSource applying_source_;
    /// `+0x28`, `+0x40`, `+0x58`: one list per activation, indexed by it.
    std::array<std::vector<LiveEffect>, 3> lists_;
    /// `+0x10`: the affixes.
    std::vector<LiveAffix> affixes_;
    /// PORT: the last `LiveAffix::id` given.
    uint32_t last_affix_id_ = 0;
    /// PORT: `offering_copy()`.
    bool offering_copy_ = false;
    bool made_ = false;
    /// `+0x80`: the total per effect type.
    std::array<float, kEffectTypeCacheCount> cache_{};
    /// `+0x2c8`: live unit themes.
    std::vector<const UnitTheme*> themes_;
    /// `calculate_effect_values`' working list, kept for its storage.
    std::vector<std::pair<int, const UnitTheme*>> theme_scratch_;
};

/// `CEffect::setOwner` for every effect held while it lives: sets the manager's
/// applying source and puts the previous one back.
class EffectSourceScope {
public:
    EffectSourceScope(EffectManager* manager, const EffectSource& source) : manager_(manager) {
        if (manager_ == nullptr) return;
        saved_ = manager_->applying_source();
        manager_->set_applying_source(source);
    }
    ~EffectSourceScope() {
        if (manager_ != nullptr) manager_->set_applying_source(saved_);
    }
    EffectSourceScope(const EffectSourceScope&) = delete;
    EffectSourceScope& operator=(const EffectSourceScope&) = delete;

private:
    EffectManager* manager_;
    EffectSource saved_;
};

}  // namespace tl::core
