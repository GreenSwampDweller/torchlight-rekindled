// The level's characters: `CCharacter::update`, `CMonster::updateAI` and its states,
// `setDestination`/`updatePathing`, the avoidance map, the swing and the damage it does.
#include "core/jump_down.h"
#include "core/ogre_math.h"
#include "core/script.h"

#include "core/ai_clock.h"
#include "core/attack_button.h"
#include "core/character.h"
#include "core/character_effects.h"
#include "core/death.h"
#include "core/hp_events.h"
#include "core/item_drop.h"
#include "core/log.h"
#include "core/randomizer.h"
#include "core/section_profile.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include <limits>

namespace tl::core {
namespace {

namespace type = tl::formats::cmp_type;
using namespace tl::core::logic;

// The distance across the ground, as the console's `sqrt(dx*dx + 0 + dz*dz)`.
float distance_xz(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + 0.0f + dz * dz);
}

// The console's ground distance where it fuses the second square into the
// first: `fmuls dx*dx`, `fmadds dz*dz + that`, `fsqrts`.
float distance_xz_fused(const Vec3& a, const Vec3& b) {
    const float dx = b.x - a.x;
    const float dz = b.z - a.z;
    return std::sqrt(std::fma(dz, dz, dx * dx));
}

std::string upper(std::string text) {
    for (char& ch : text) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return text;
}

/// `CLevel::findCharacterWithinView`'s sight gate, in the looker's frame; see
/// `can_see()` in script.cpp.
bool view_test(const Vec3& from, float facing, const Vec3& at, float motion, float view,
               float sight) {
    return can_see(from, facing, at, motion, view, sight);
}

}  // namespace

// ---------------------------------------------------------------------------
// Bookkeeping
// ---------------------------------------------------------------------------

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
LevelScript::Character* LevelScript::character(int64_t id) {
    const auto it = characters_.find(id);
    return it == characters_.end() ? nullptr : &it->second;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
const LevelScript::Character* LevelScript::character(int64_t id) const {
    const auto it = characters_.find(id);
    return it == characters_.end() ? nullptr : &it->second;
}

// PORT: a unit's data from the port's unit book, the defaults when it has none.
const UnitStats& LevelScript::stats_for(const std::string& unit) const {
    if (units_ != nullptr) {
        if (const UnitStats* found = units_->find(unit)) return *found;
    }
    static const UnitStats kDefaults;
    return kDefaults;
}

// PORT: where a unit's feet are: its motion centre less its radius.
Vec3 LevelScript::feet_of(const Character& c) const {
    return {c.motion.position.x, c.motion.position.y - c.motion.radius, c.motion.position.z};
}

// PORT: a node's position: a character's feet, else the node's own.
Vec3 LevelScript::position_of(const Object& o) const {
    if (const Character* c = character(o.id)) return feet_of(*c);
    return o.position;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
std::string LevelScript::target_unit(const UnitRef& target) const {
    const Object* o = target_object(target);
    return o != nullptr ? o->unit : std::string();
}

// PORT: the machines of the level (the port's layouts), or this one alone.
MachineWalk LevelScript::level_machines() const {
    if (level_ != nullptr && !level_->empty()) return MachineWalk(*level_);
    alone_.assign(1, const_cast<LevelScript*>(this));
    return MachineWalk(alone_);
}

LevelScript::UnitRef LevelScript::resolve(int64_t node) {
    if (node == 0) return {};
    if (character(node) != nullptr) return {this, node};
    for (LevelScript* m : level_machines()) {
        if (m->character(node) != nullptr) return {m, node};
    }
    return {this, node};
}

const SkillDef* LevelScript::effect_skill(const Effect& effect) const {
    if (!effect_teaches_skill(effect.type) || !unit_skills_.skill_files ||
        !unit_skills_.skill_def || !unit_skills_.skill_files(effect.name)) {
        return nullptr;
    }
    return unit_skills_.skill_def(upper(effect.name));
}

void LevelScript::roll_own_effects(std::vector<Effect>& effects, EffectRolls& rolls,
                                   SkillConstruction& with) const {
    const bool constructs = has_unit_effect_tables_ && unit_effect_tables_.types != nullptr;
    for (Effect& effect : effects) {
        rolls.roll(effect);
        // `CEffectManager::createEffect` activates each as it is made: one that
        // teaches a skill makes it there (`CBaseUnit::activateEffect` ->
        // `addSkillByName` -> `CSkill::CSkill`), its effects rolled and event files loaded.
        if (!constructs) continue;
        if (const SkillDef* def = effect_skill(effect)) roll_skill_construction(*def, with);
    }
}

// `CBaseUnit::unitInit`, the scale, then
// `CCharacter::unitInit` and its
// `setLevel(LEVEL, true)`.
LevelScript::UnitCreation LevelScript::roll_unit_creation(const std::string& unit,
                                                          bool default_equipment,
                                                          int level_default) {
    UnitCreation made;
    const UnitStats& stats = stats_for(unit);
    made.scale = stats.scale + volatile_random().between(0.0f, stats.scale_variation);
    // `CMonsterDescriptor::CreateObject` makes the monster at
    // the level's monster level; `CMonster::CMonster` keeps
    // at least 1, and `unitInit` reads `LEVEL` with that as default.
    const int fallback =
        0 < level_default ? level_default : std::max(1, static_cast<int>(level_monster_level_));
    // `CBaseUnit::unitInit` then `reapplyEffects(false)`:
    // `CEffectManager::createEffects` rolls each own `[EFFECT]` on `g_RandVolatile`
    // at the level it was made with (`LEVEL` is read after).
    made.own_effects = stats.own_effects;
    EffectRolls rolls;
    rolls.graphs = graphs_;
    rolls.rng = &volatile_random();
    rolls.owner.present = true;
    rolls.owner.level = static_cast<uint32_t>(fallback);
    const bool constructs =
        has_unit_effect_tables_ && graphs_ != nullptr && unit_effect_tables_.types != nullptr;
    SkillConstruction with;
    with.types = unit_effect_tables_.types;
    with.unit_types = unit_types_;
    with.affixes = affix_book_;
    with.rolls.graphs = graphs_;
    with.rolls.rng = &volatile_random();
    with.game_rng = &game_random();
    with.layout_adds_sample = unit_skills_.layout_adds_sample;
    with.load_event_file = unit_skills_.load_event_file;
    if (graphs_ != nullptr) roll_own_effects(made.own_effects, rolls, with);
    // `unitInit` then gives its `[SKILL]`s, one `CSkillManager::addSkill`
    // each, whose effects roll as made (`roll_skill_construction`); only
    // while the game runs (`* +0x1404`).
    if (unit_skills_.load && constructs) {
        for (const UnitSkill& block : unit_skills_.load(unit)) {
            if (block.known.def != nullptr) roll_skill_construction(*block.known.def, with);
        }
    }
    // The unit's own manager: own effects (the values just rolled), then
    // `reapplyAffixes(false)` (after the `[SKILL]`s): each
    // affix through `CBaseUnit::addAffix(name, LEVEL, this, -1)`. Activations wait for the
    // character (`load_own_effects`, `load_unit_skills`).
    if (has_unit_effect_tables_ && graphs_ != nullptr &&
        (!made.own_effects.empty() ||
         (!stats.authored_affixes.empty() && affix_book_ != nullptr))) {
        made.effects = std::make_shared<EffectManager>();
        EffectManager::Context context = unit_effect_tables_;
        context.owner = rolls.owner;
        made.effects->set_context(context);
        for (const Effect& effect : made.own_effects) made.effects->add_effect(effect);
        if (affix_book_ != nullptr) {
            const uint32_t at =
                static_cast<uint32_t>(stats.level_authored ? stats.level : fallback);
            for (const std::string& name : stats.authored_affixes) {
                std::optional<Affix> affix =
                    new_affix_by_name(*affix_book_, upper(name), at, rolls);
                if (!affix) continue;
                made.effects->add_affix(*affix, at, -1.0f, std::string(), fallback, rolls);
            }
        }
    }
    const int level = stats.level_authored ? stats.level : fallback;
    static const StatBook kNoGraphs;
    made.combat = set_level(stats, level, difficulty_, graphs_ != nullptr ? *graphs_ : kNoGraphs,
                            volatile_random());
    // `CCharacter::createDefaultEquipment`: authored hands,
    // then the `SPAWN...` classes rolled on `g_Rand`.
    // `CCharacter::loadModel` (from `unitInit`): each sound
    // bank slot the GUID finds is a `CSoundBank::addSample`, which keeps
    // only `g_Rand`'s low word (`setSeed(low word)`), as is each sound key of the model.
    // INVENTED: particles a key or theme spawns are not followed into the preloader
    // (`CParticlePreloader::LoadParticle`).
    if (sound_data_ != nullptr) {
        for (size_t slot = 0; slot < kSoundSlots; ++slot) {
            const std::string& name = stats.sounds[static_cast<SoundSlot>(slot)];
            if (name.empty()) continue;
            const SoundData* named = sound_data_->find(upper(name));
            if (named == nullptr || sound_data_->find_by_guid(named->guid) == nullptr) continue;
            game_random().set_seed(
                static_cast<int32_t>(static_cast<uint32_t>(game_random().state())));
        }
    }
    if (model_sounds_ && model_sounds_(unit))
        game_random().set_seed(static_cast<int32_t>(static_cast<uint32_t>(game_random().state())));
    // Then the sounds every character's bank holds by name, each a
    // sample when the definitions have it.
    if (sound_data_ != nullptr) {
        static const char* const kBankSounds[] = {
            "MISSILEREFLECT",
            "METALBLOCK",
            "ELEMENTALFIREGETHIT",
            "ELEMENTALICEGETHIT",
            "ELEMENTALELECTRICGETHIT",
            "ELEMENTALPOISONGETHIT",
            "SWORDFLESH",
            "BLOCK",
            "CRITICALSTRIKE",
        };
        for (const char* name : kBankSounds) {
            if (sound_data_->find(name) == nullptr) continue;
            game_random().set_seed(
                static_cast<int32_t>(static_cast<uint32_t>(game_random().state())));
        }
    }
    if (default_equipment && unit_equipment_.items != nullptr && unit_types_ != nullptr) {
        // `CCharacter::unitInit`: `CInventory(this, 0x32)`
        // with no pane, then `createDefaultEquipment`
        // into it; the hands are what went on at locations 0 and 1.
        made.inventory = std::make_shared<Inventory>(0x32, /*bare=*/true);
        // Its owner the character: every change verified (`verifyEquipment`).
        // `CEquipment::canEquip` asks a `USEWEAPONDAMAGE` unit for an
        // identified item and requirements; others for neither.
        if (stats.use_weapon_damage) {
            made.sheet = std::make_shared<CharacterProgression>();
            made.sheet->init_unit(level);
        }
        made.inventory->set_owner(unit_types_, made.sheet.get(), made.effects.get());
        made.inventory->create_default_equipment(stats.default_equipment, *unit_equipment_.items,
                                                 *unit_types_, unit_equipment_.create,
                                                 unit_equipment_.roll, unit_equipment_.enchant);
        const auto hand = [&](int location, Hand which, std::string& name,
                              UnitStats::WeaponAttack& attack) {
            if (!made.inventory->equip_location_occupied(location)) return;
            name = made.inventory->slots()[static_cast<size_t>(location)].item.name;
            if (unit_equipment_.weapon_attack) attack = unit_equipment_.weapon_attack(name, which);
        };
        hand(0, Hand::Right, made.right_hand, made.weapon_attack);
        hand(1, Hand::Left, made.left_hand, made.left_weapon_attack);
    } else if (default_equipment) {
        made.right_hand = stats.right_hand;
        made.left_hand = stats.left_hand;
        made.weapon_attack = stats.weapon_attack;
        made.left_weapon_attack = stats.left_weapon_attack;
        if (equipment_roller_) equipment_roller_(unit, made);
    }
    // `CCharacter::unitInit`: `CHAMPION` makes a champion named
    // by `CRandomNames::generateName` (on `g_Rand`) with the `CHAMPION` theme; else
    // `BOSS_CHAMPION` keeps its name, no theme. `makeChampion` adds 0.2 to
    // the scale.
    if (stats.champion || stats.boss_champion) {
        if (stats.champion && random_names_ != nullptr) {
            made.champion_name = random_names_->generate_name(true, game_random());
        }
        if (make_champion(made.combat, stats, difficulty_,
                          graphs_ != nullptr ? *graphs_ : kNoGraphs, volatile_random())) {
            made.scale = made.scale + 0.2f;
        }
    }
    return made;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::unit_hands(int64_t id, std::string& right, std::string& left) const {
    const Character* c = character(id);
    if (c == nullptr) return false;
    right = c->right_hand;
    left = c->left_hand;
    return true;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::unit_attacks_left(int64_t id) const {
    const Character* c = character(id);
    return c != nullptr && c->attack_left && c->left_weapon_attack.present;
}

float LevelScript::unit_scale(int64_t id) const {
    const Character* c = character(id);
    return c != nullptr ? c->scale : 0.0f;
}

// `CCharacter::CCharacter` and `unitInit`:
// a unit placed in the level, made as
// `roll_unit_creation` says.
LevelScript::Character& LevelScript::make_character(const Object& o, const Vec3& at, float facing,
                                                    const UnitCreation* creation,
                                                    const Character* carried) {
    Character fresh;
    fresh.node = o.id;
    fresh.unit = o.unit;
    const UnitStats& stats = stats_for(o.unit);
    fresh.motion.radius = stats.radius;
    fresh.motion.position = {at.x, at.y + stats.radius, at.z};
    fresh.motion.walking_speed = stats.movement.walking_speed;
    fresh.motion.running_speed = stats.movement.running_speed;
    fresh.facing = facing;
    fresh.home_facing = facing;
    const UnitCreation made = creation != nullptr ? *creation : roll_unit_creation(o.unit);
    fresh.scale = made.scale;
    // The renderer placed it here already: nothing to send until it moves.
    fresh.sent = true;
    fresh.sent_at = at;
    fresh.sent_facing = facing;
    // ... and standing on whatever the layout started it on: a gait is sent only
    // once the unit moves.
    fresh.gait = "Idle";
    // A monster node's `VISIBLE` goes through `setMeshVisible`
    // (from `CMonsterDescriptor::descriptorSceneActivated`):
    // hidden, no fade shows it. `CLevel::addCharacter`
    // starts it active, near, at opacity 1.
    fresh.mesh_hidden = !o.visible;
    fresh.want_visible = o.visible;
    // `unitInit`: `ISA 0xa7` adds the
    // `MIMICIDLE` unit theme.
    fresh.mimic_idle =
        unit_types_ != nullptr && unit_types_->isa(stats.unittype, unit_type::kMimic);
    fresh.can_match_speed = stats.can_match_speed;
    fresh.combat = made.combat;
    fresh.right_hand = made.right_hand;
    fresh.left_hand = made.left_hand;
    fresh.weapon_attack = made.weapon_attack;
    fresh.left_weapon_attack = made.left_weapon_attack;
    if (made.inventory) {
        fresh.own_inventory = made.inventory;
        fresh.inventory = made.inventory.get();
    }
    fresh.sheet = made.sheet;
    fresh.own_effects = made.own_effects;
    if (made.effects) fresh.effects = made.effects;

    AIFlagsHooks hooks;
    // CAIFlagManager::flagSet type 0 (AWARE): spread AWARE with duration
    // to each formation peer.
    hooks.on_spread_aware = [this](int64_t member_id, float duration) {
        for (LevelScript* m : level_machines()) {
            if (Character* other = m->character(member_id)) {
                other->ai_flags.add(AIFlag::Aware, duration);
                return;
            }
        }
    };
    // CAIFlagManager::flagSet type 1 (BERSERK): dormant in the port today (nothing sets
    // it).
    // INVENTED: CCharacter::interrupt, setAIState(2)
    // and CMonster::updateAI(0.0f, true)
    // are omitted on this dormant path; target clearing and path stopping are wired.
    const int64_t my_id = o.id;
    hooks.on_berserk_start = [this, my_id]() {
        if (Character* self = character(my_id)) {
            stop_pathing(*self);
            self->has_target = false;
            self->target = UnitRef{};
        }
    };
    // CAIFlagManager::flagRemoved type 1 (BERSERK): dormant in the port today.
    // INVENTED: CCharacter::setAlignment by ISA(0x1b)
    // is omitted: runtime alignment mutation is not modelled.
    hooks.on_berserk_end = []() {};
    fresh.ai_flags.set_hooks(std::move(hooks));

    AISkillsHooks skill_hooks;
    // INVENTED: CAISkillManager::skillAdded/skillRemoved find the character's CSkill through CSkillManager (+0x1c8); a LevelScript character holds none, so +0x6d is not switched.
    skill_hooks.on_skill_enabled = [](const std::string&, bool) {};
    fresh.ai_skills.set_hooks(std::move(skill_hooks));

    // CAIManager::initalize: build watchers
    // from parsed [AI] [STAT] groups.
    for (const auto& def : stats.ai_watchers) {
        AIStatWatcherHooks watcher_hooks;
        watcher_hooks.owner_id = [my_id]() { return my_id; };
        watcher_hooks.hp = [this, my_id]() -> int32_t {
            if (const Character* self = character(my_id)) {
                return static_cast<int32_t>(std::floor(self->combat.hp));
            }
            return 0;
        };
        watcher_hooks.max_hp = [this, my_id]() -> int32_t {
            if (const Character* self = character(my_id)) {
                return max_hp(self->combat);
            }
            return 1;
        };
        // INVENTED: monsters have no mana model in the port today.
        //
        watcher_hooks.mana = []() -> int32_t { return 0; };
        watcher_hooks.max_mana = []() -> int32_t { return 1; };
        watcher_hooks.alive = [this, my_id]() -> bool {
            if (const Character* self = character(my_id)) {
                return self->combat.hp > 0.0f && dead_.count(my_id) == 0;
            }
            return false;
        };
        watcher_hooks.has_level = [this]() -> bool { return !level_machines().empty(); };
        watcher_hooks.formation_units = [this, my_id]() -> std::vector<int64_t> {
            if (const Character* self = character(my_id)) {
                return self->ai_flags.formation_units();
            }
            return {};
        };
        watcher_hooks.find_flags_by_id = [this](int64_t id) -> AIFlags* {
            for (LevelScript* m : level_machines()) {
                if (Character* other = m->character(id)) {
                    return &other->ai_flags;
                }
            }
            return nullptr;
        };
        watcher_hooks.active_units_count = [this, my_id](uint32_t unittype, uint32_t alignment,
                                                         float radius, bool include_living,
                                                         bool include_dead) -> float {
            if (const Character* self = character(my_id)) {
                std::vector<int64_t> out;
                get_active_characters_at_position(self->motion.position, unittype, alignment,
                                                  radius, include_living, include_dead, out);
                return static_cast<float>(out.size());
            }
            return 0.0f;
        };
        // `getTargets`' area cases walk the level's character list (`level_machines()` in order)
        // with the distance and, for `AREAUNITTYPES`, `ISA` only.
        watcher_hooks.area_targets = [this, my_id](float radius,
                                                   uint32_t unittype) -> std::vector<int64_t> {
            if (const Character* self = character(my_id)) {
                std::vector<int64_t> out;
                get_active_characters_at_position(self->motion.position, unittype, 3, radius, true,
                                                  true, out);
                return out;
            }
            return {};
        };
        watcher_hooks.area_formation_targets = [this, my_id](float radius) -> std::vector<int64_t> {
            std::vector<int64_t> out;
            if (const Character* self = character(my_id)) {
                const Vec3& my_pos = self->motion.position;
                for (int64_t peer_id : self->ai_flags.formation_units()) {
                    for (LevelScript* m : level_machines()) {
                        if (const Character* peer = m->character(peer_id)) {
                            const float dx = my_pos.x - peer->motion.position.x;
                            const float dy = my_pos.y - peer->motion.position.y;
                            const float dz = my_pos.z - peer->motion.position.z;
                            if (std::sqrt(dx * dx + dy * dy + dz * dz) <= radius) {
                                out.push_back(peer_id);
                            }
                            break;
                        }
                    }
                }
            }
            return out;
        };
        // CAIManager::addAISkill
        watcher_hooks.add_ai_skill = [this, my_id](AISkill* skill, float duration) {
            if (Character* self = character(my_id)) self->ai_skills.add(skill, duration);
        };
        // CAIManager::hasAISkill
        watcher_hooks.has_ai_skill = [this, my_id](AISkill* skill) -> bool {
            if (Character* self = character(my_id)) return self->ai_skills.has(skill);
            return false;
        };
        // CAIManager::removeAISkill
        watcher_hooks.remove_ai_skill = [this, my_id](AISkill* skill) {
            if (Character* self = character(my_id)) self->ai_skills.remove(skill);
        };
        // INVENTED: CCharacter::setAIPlayAnimation
        // is not transcribed; animation hook
        // is empty.
        watcher_hooks.play_animation = [](int64_t, const std::string&) {};

        fresh.ai_watchers.emplace_back(def, std::move(watcher_hooks));
    }

    if (carried != nullptr) {
        // What a `unitInit` over a living `CCharacter` leaves as it was: effect manager (new own
        // effects on top), inventory, master and list place, earnings, proc lockout, hidden
        // state, taught skills, transformation, a pet's stance and town clock.
        fresh.effects = carried->effects;
        fresh.inventory = carried->inventory;
        fresh.master_player = carried->master_player;
        fresh.listed_pet = carried->listed_pet;
        fresh.experience = carried->experience;
        fresh.alignment = carried->alignment;
        fresh.proc_lockout = carried->proc_lockout;
        fresh.mesh_hidden = carried->mesh_hidden;
        fresh.want_visible = carried->want_visible;
        fresh.casting.skills = carried->casting.skills;
        fresh.transform_left = carried->transform_left;
        fresh.transformed_from = carried->transformed_from;
        fresh.stance = carried->stance;
        fresh.town_timer = carried->town_timer;
    }
    if (characters_.count(o.id) == 0) character_order_.push_back(o.id);
    Character& character = characters_[o.id] = std::move(fresh);
    // `CBaseUnit::unitInit`: `reapplyEffects(false)` as the
    // unit is made, when the hooks are in (else on its first update).
    load_own_effects(character);
    return character;
}

// PORT: a unit's record dropped from the machine.
void LevelScript::forget_character(int64_t id) {
    characters_.erase(id);
    character_order_.erase(std::remove(character_order_.begin(), character_order_.end(), id),
                           character_order_.end());
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
std::string LevelScript::unit_of(int64_t id) const {
    const Object* o = find(id);
    return o != nullptr ? o->unit : std::string();
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
void LevelScript::set_unit_hp(int64_t id, float hp) {
    if (Character* c = character(id)) c->combat.hp = hp;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::unit_health(int64_t id, float& hp, float& max) const {
    const Character* c = character(id);
    if (c == nullptr) return false;
    hp = c->combat.hp;
    max = static_cast<float>(max_hp(c->combat));
    return true;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::unit_raw_defense(int64_t id, std::array<int, kDamageTypeCount>& out) const {
    const Character* c = character(id);
    if (c == nullptr) return false;
    out = c->combat.damage_defense;
    return true;
}

bool LevelScript::unit_defender(int64_t id, Defender& out) const {
    const Character* c = character(id);
    if (c == nullptr) return false;
    out = Defender{};
    // `CCharacter::AC()` and `damageDefense`,
    // each over `defense()`.
    //
    const int defense = unit_defense(c->effects.get());
    // INVENTED: what a unit wears (`createDefaultEquipment`'s armour: Brink's,
    // a skeleton's shield) is not carried by the level machine, so its
    // inventory's armour (`+0x3e8 -> +0x08`) is 0 here.
    out.armor = armor_class(c->combat, defense, c->effects.get(), master_effects(*c), 0);
    out.max_hp = max_hp(c->combat);
    // `getBlockChance()` and `INTERRUPT RESISTANCE` (0x88), off its
    // own effects.
    out.block_chance = block_chance(CharacterEffects(c->effects.get()));
    out.interrupt_resistance = c->effects != nullptr ? c->effects->effect_value(0x88, 7) : 0.0f;
    for (int type = 0; type < kDamageTypeCount; ++type) {
        out.damage_defense[static_cast<size_t>(type)] =
            damage_defense(c->combat, type, defense, c->effects.get());
    }
    // `modifyDamage`'s `PERCENT DAMAGE TAKEN` and `SHIELD BUFFER`s: its own
    // effects (a placed unit carries no inventory here).
    out.effects = CharacterEffects(c->effects.get());
    out.shields = c->effects.get();
    return true;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::unit_level(int64_t id, int& level) const {
    const Character* c = character(id);
    if (c == nullptr) return false;
    level = c->combat.level;
    return true;
}

// `CCharacter::setLevel` on a level unit: its
// stats made again at the new level (`set_level`, which holds the rule).
void LevelScript::set_unit_level(int64_t id, int level) {
    Character* c = character(id);
    const Object* o = find(id);
    if (c == nullptr || o == nullptr || c->combat.level == level) return;
    static const StatBook kNoGraphs;
    c->combat = set_level(stats_for(o->unit), level, difficulty_,
                          graphs_ != nullptr ? *graphs_ : kNoGraphs, volatile_random());
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::unit_experience(int64_t id, int32_t& experience) const {
    const Character* c = character(id);
    if (c == nullptr) return false;
    experience = c->experience;
    return true;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
void LevelScript::set_unit_experience(int64_t id, int32_t experience) {
    if (Character* c = character(id)) c->experience = experience;
}

// `CCharacter::awardExperience(level, killed, amount, true)`
// as the player's award calls it on each pet: shared, so no pet
// `XP GAIN BONUS` and no forward to its master.
void LevelScript::award_pet_experience(int amount, std::vector<int64_t>& levelled) {
    if (graphs_ == nullptr) return;
    for (const Object& o : objects_) {
        if (o.type != type::kMonster || dead_.count(o.id) != 0) continue;
        Character* c = character(o.id);
        if (c == nullptr || !c->master_player) continue;
        // Dying or dead (`+0x330` 5 or 6) takes no gain; a loss it does.
        if ((o.ai == ai_state::kDying || o.ai == ai_state::kDead) && 0 < amount) continue;
        c->experience = experience_after_award(c->experience, amount);
        if (experience_gate(*graphs_, c->combat.level) <= c->experience) {
            if (c->combat.level < max_experience_level(*graphs_)) {
                // `CCharacter::levelUp`
                // (vtable +0x320 of a `CMonster`): `setLevel(level + 1, false)`,
                // then the hit points and the mana at their new maxima.
                c->combat = set_level(stats_for(o.unit), c->combat.level + 1, difficulty_, *graphs_,
                                      volatile_random());
                c->combat.hp = static_cast<float>(max_hp(c->combat));
                if (c->casting.loaded) {
                    // `CCharacter::calculateMaxMana`:
                    // the level times 5,
                    // plus 20.
                    c->casting.max_mana = std::max(1, c->combat.level * 5 + 20);
                    c->casting.mana = static_cast<float>(c->casting.max_mana);
                }
                levelled.push_back(o.id);
                continue;
            }
            // At the cap the total stays at the gate.
            c->experience = experience_gate(*graphs_, c->combat.level);
        }
    }
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::unit_position(int64_t id, Vec3& at) const {
    const Character* c = character(id);
    if (c == nullptr) return false;
    at = feet_of(*c);
    return true;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::unit_target(int64_t id, LevelScript*& machine, int64_t& node) const {
    const Character* c = character(id);
    if (c == nullptr || !c->has_target) return false;
    machine = c->target.machine;
    node = c->target.node;
    return true;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
float LevelScript::unit_radius(int64_t id) const {
    const Character* c = character(id);
    return c != nullptr ? c->motion.radius : 0.0f;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::unit_facing(int64_t id, float& yaw) const {
    const Character* c = character(id);
    if (c == nullptr) return false;
    yaw = c->facing;
    return true;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
EffectManager* LevelScript::unit_effects(int64_t id) {
    Character* c = character(id);
    return c != nullptr ? c->effects.get() : nullptr;
}

const std::vector<Effect>* LevelScript::unit_own_effects_made(int64_t id) const {
    const Character* c = character(id);
    return c != nullptr ? &c->own_effects : nullptr;
}

// PORT: a unit's effect manager made on first use.
EffectManager* LevelScript::ensure_unit_effects(int64_t id, const EffectManager::Context& context) {
    Character* c = character(id);
    if (c == nullptr) return nullptr;
    if (!c->effects) {
        c->effects = std::make_shared<EffectManager>();
        c->effects->set_context(context);
    }
    return c->effects.get();
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
std::vector<int64_t> LevelScript::units_with_effects() const {
    std::vector<int64_t> out;
    for (int64_t id : character_order_) {
        const Character* c = character(id);
        if (c != nullptr && c->effects) out.push_back(id);
    }
    return out;
}

// `CCharacter::interrupt` on a level unit, by id.
void LevelScript::interrupt_unit(int64_t id, QuestState& quests, std::vector<ScriptEvent>& out) {
    Object* o = find(id);
    Character* c = character(id);
    if (o == nullptr || c == nullptr) return;
    interrupt(*o, *c, true, quests, out);
}

// `CGenericModel::animationExistsSubstring`: an animation
// whose upper-cased name begins with `prefix`, as passed.
bool LevelScript::unit_animation_prefix_exists(const std::string& unit,
                                               const std::string& prefix) const {
    if (!animation_catalog_) return false;
    for (const auto& entry : animation_catalog_(unit)) {
        const std::string name = upper(entry.first);
        if (name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0)
            return true;
    }
    return false;
}

bool LevelScript::unit_animation_exists(const std::string& unit, const std::string& name) const {
    if (!animation_catalog_) return false;
    const std::string wanted = upper(name);
    for (const auto& entry : animation_catalog_(unit)) {
        if (upper(entry.first) == wanted) return true;
    }
    return false;
}

// `CCharacter::interrupt(bool hit)`, read
// from the console's, which differs from the Linux build's: it tests AI flag 2
// inline, and leaves the destination where it was.
void LevelScript::interrupt(Object& o, Character& c, bool hit, QuestState& quests,
                            std::vector<ScriptEvent>& out) {
    // `CANNOT INTERRUPT` held (`+0x64c`, flag 2 with time on it).
    if (c.ai_flags.has(AIFlag::CannotInterrupt)) return;
    if (o.ai == 0x10 || o.ai == ai_state::kGoingToTown || o.ai == 0x28 ||
        o.ai == ai_state::kPlayAnimation) {
        return;
    }
    // a skill is active (`+0x2c4`) and its animation is up
    // (`+0x1ff`), or has time (`+0x2b4`) with the swing not over (`+0x2b1`).
    const UnitSkill* active = active_unit_skill(c);
    const bool performing =
        active != nullptr && (c.casting.cast.casting || (0.0f < c.casting.cast.animation_left &&
                                                         !c.casting.cast.attack_ended));
    if (performing) {
        // `+0x2cc`: `CSkill::getCanBeInterrupted`, the root
        // property's `INTERRUPTABLE` (`+0x90 -> +0xb1`), as `castSkill` copied it.
        if (active->known.def == nullptr || !active->known.def->base.interruptable) return;
        if (!unit_animation_exists(o.unit, "HIT")) {
            // No `HIT` to play: an `IDLE` blended (looping) and state 0, and
            // the rest as for no hit.
            hit = false;
            std::string idle;
            float length = 0.0f;
            if (find_random_animation(o.unit, "IDLE", idle, length) >= 0) {
                blend_reset(c);
                ScriptEvent e;
                e.action = ScriptAction::Animation;
                e.node_id = o.id;
                e.name = o.name;
                e.unit = o.unit;
                e.text = idle;
                e.on = true;
                e.loops = true;
                e.blend = 0.2f;
                out.push_back(e);
            }
            set_ai_state(o, ai_state::kNone, quests, out);
        }
    }
    if (hit && !unit_animation_exists(o.unit, "HIT")) return;
    if (o.ai == 0x10 || o.ai == 0x28 || o.ai == ai_state::kGoingToTown ||
        o.ai == ai_state::kDying || o.ai == ai_state::kDead) {
        return;
    }
    // The swing (`+0x2ac`) and the cast (`+0x2b4`, `+0x1ff`) dropped, every
    // skill stopped (`stopAllSkills(false, false, false)`).
    c.swing_left = 0.0f;
    c.swing.clear();
    c.casting.cast.animation_left = 0.0f;
    c.casting.cast.casting = false;
    if (unit_skills_.stop_all) unit_skills_.stop_all(*this, o.id, /*force=*/false);
    if (hit) {
        // A random `HIT` blended without a loop, `IDLE` queued behind it
        // (`queueBlendAnimation("IDLE", true, 0.2, 1.0)`), and state 0x1d.
        std::string reaction;
        float length = 0.0f;
        if (find_random_animation(o.unit, "HIT", reaction, length) >= 0) {
            blend_reset(c);
            c.hit_left = length;
            ScriptEvent e;
            e.action = ScriptAction::Animation;
            e.node_id = o.id;
            e.name = o.name;
            e.unit = o.unit;
            e.text = reaction;
            e.on = true;
            e.loops = false;
            e.blend = 0.2f;
            e.queue.push_back("IDLE");
            out.push_back(e);
        }
        c.gait.clear();
        set_ai_state(o, 0x1d, quests, out);
    }
    // The route (`+0x1fc`) ends with its timer at 10 (`+0x208`); the console
    // does not move the destination.
    if (c.pathing) c.since_route = 10.0f;
    c.pathing = false;
    // The hands' weapon trails (`+0x5f8`, `+0x5fc`) off and hidden.
    ScriptEvent trails;
    trails.action = ScriptAction::HideHandTrails;
    trails.node_id = o.id;
    trails.name = o.name;
    out.push_back(trails);
}

// `CCharacter::updateEffects`: the outside push is
// zeroed on entry and sums this frame's knock-backs; then cut to 30 (10 for
// an ISA 0x1c).
void LevelScript::knock_back_unit(int64_t id, const Vec3& add) {
    Character* c = character(id);
    if (c == nullptr) return;
    c->motion.push = bound_outside_push(add, false);
}

// `CCharacter::updateAnimation`, key
// types 0x12 `DAMPVELOCITY` (`+0x258 = 0`, the velocity times 0) and 0x13
// `UNDAMPVELOCITY` (`+0x258 = 1`).
void LevelScript::damp_unit_velocity(int64_t id, bool damp) {
    Character* c = character(id);
    if (c == nullptr) return;
    if (damp) {
        c->motion.velocity_scale = 0.0f;
        c->motion.velocity = c->motion.velocity * 0.0f;
    } else {
        c->motion.velocity_scale = 1.0f;
    }
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
float LevelScript::unit_velocity_scale(int64_t id) const {
    const Character* c = character(id);
    return c != nullptr ? c->motion.velocity_scale : -1.0f;
}

// `CCharacter::blendAnimation(int/string, bool, float, float, float)`:
// `+0x258 = 1`, so a damped unit moves again, before the
// model's blend (`CGenericModel::blendAnimation`). Callers here: `attack`, `interrupt`,
// the jump down.
void LevelScript::blend_reset(Character& c) { c.motion.velocity_scale = 1.0f; }

// `CCharacter::updateEffects`, the `STUN` (0x40) half: a
// live stun stops a unit (not attacking, dying, dead, 0x29 or 0x10) in state 0x28; once
// it runs out, a unit still in 0x28 goes. Theme and animation are the scene's.
bool LevelScript::set_unit_stunned(int64_t id, bool stunned, QuestState& quests,
                                   std::vector<ScriptEvent>& out) {
    Object* o = find(id);
    Character* c = character(id);
    if (o == nullptr || c == nullptr) return false;
    constexpr int32_t kStunned = 0x28;
    if (!stunned) {
        // `+0x258 = 1`, the theme off, and a unit still in 0x28 let go.
        c->motion.velocity_scale = 1.0f;
        if (o->ai != kStunned) return false;
        set_ai_state(*o, ai_state::kNone, quests, out);
        return true;
    }
    if (o->ai == kStunned || o->ai == 0xc || o->ai == ai_state::kDying ||
        o->ai == ai_state::kDead || o->ai == 0x29 || o->ai == 0x10) {
        return false;
    }
    // `blendAnimation("STUNNED")` (the scale to 1), the
    // theme, then `+0x258 = 0` and the velocity zeroed.
    c->motion.velocity_scale = 0.0f;
    c->motion.velocity = {0.0f, 0.0f, 0.0f};
    stop_pathing(*c);
    set_ai_state(*o, kStunned, quests, out);
    // `blendAnimation("STUNNED" or "IDLE", true, 0.2, 1.0, -1.0)`:
    // the gait is chosen again.
    c->gait.clear();
    return true;
}

// PORT: a level unit told it was hurt with no attacker (`react_to_damage`, which holds the rule).
void LevelScript::provoke_unit(int64_t id, QuestState& quests, std::vector<ScriptEvent>& out) {
    Object* o = find(id);
    Character* c = character(id);
    if (o == nullptr || c == nullptr || dead_.count(id) != 0) return;
    react_to_damage(*o, *c, UnitRef{}, true, quests, out);
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
int32_t LevelScript::unit_ai(int64_t id) const {
    const Object* o = find(id);
    return o != nullptr && character(id) != nullptr ? o->ai : -1;
}

// `CCharacter::modifyHP` by a positive amount on a
// level unit: held to its maximum, truncated to an integer.
void LevelScript::heal_unit(int64_t id, float amount) {
    Character* c = character(id);
    if (c == nullptr || !(amount > 0.0f) || dead_.count(id) != 0) return;
    float hp = 0.0f;
    float max = 0.0f;
    if (!unit_health(id, hp, max)) return;
    const float ceiling = static_cast<float>(static_cast<int>(max));
    c->combat.hp = std::min(ceiling, hp + amount);
}

// `CCharacter::modifyHP`, a heal. The damage
// half is `damage_unit`'s.
bool LevelScript::modify_unit_hp(int64_t id, float amount) {
    Character* c = character(id);
    if (c == nullptr || !(0.0f <= amount)) return false;
    // A mimic holding `MIMICIDLE` is not touched.
    if (c->mimic_idle) return false;
    const float max = static_cast<float>(max_hp(c->combat));
    const float before = c->combat.hp;
    // A body takes only the whole of `maxHP()`.
    if (before <= 0.0f && 0.0f < amount && amount < max) return false;
    // From exactly 0 with at least `maxHP()`: `+0x454` cleared.
    const bool cleared = before == 0.0f && max <= amount;
    // `getIsInGodMode` is false for a monster, and `broadcastHPThreshholdEvents`
    // fires nothing for a heal (`broadcast_unit_hp_events`). `INVINCIBLE`'s
    // floor at 1 and the floor at 0 hold for damage only.
    float hp = before + amount;
    if (max < hp) hp = max;
    c->combat.hp = hp;
    if (hp < c->min_hp) c->min_hp = hp;
    return cleared;
}

// `CSkillEvent::applyEffects`: `alive()` before the
// effects, `updateEffects(0)`, and for one not alive with
// `HP()` above 0, `setAIState(2)`.
bool LevelScript::unit_gets_up(int64_t id, bool was_alive, QuestState& quests,
                               std::vector<ScriptEvent>& out) {
    if (was_alive) return false;
    Object* o = find(id);
    Character* c = character(id);
    if (o == nullptr || c == nullptr || dead_.count(id) == 0) return false;
    // `HP()` (inlined on the console, around its `floor`):
    // the hit points as an integer.
    if (!(0 < static_cast<int>(c->combat.hp))) return false;
    // The port's own marks of a body (`unit_killed`, `kill_character`) undone:
    // off the dead, targetable again as `+0x531` says, back on the avoidance
    // map, and its gait sent again so the model leaves its death pose.
    dead_.erase(id);
    o->targetable = o->targetable_flag;
    avoidance_add(*c);
    c->gait.clear();
    set_ai_state(*o, ai_state::kIdle, quests, out);
    return true;
}

float LevelScript::character_speed(const Character& c, float base) const {
    // The arithmetic is `running_speed` (core/character_effects.h); the second effect is asked
    // only for a slowdown. Both via `CCharacter::getEffectValue`.
    //
    const float percent = character_effect(c, 0x15);
    const float kept = !(percent >= 0.0f) ? character_effect(c, 0x8c) : 0.0f;
    return running_speed(base, percent, kept);
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::node_local_position(int64_t id, Vec3& at) const {
    const Object* o = find(id);
    if (o == nullptr) return false;
    at = o->local_position;
    return true;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::node_position(int64_t id, Vec3& at) const {
    const Object* o = find(id);
    if (o == nullptr) return false;
    at = o->position;
    return true;
}

std::vector<LevelScript::UnitTriggerView> LevelScript::idle_mimics() const {
    // `CLevel::findItemsOnscreen`: alive, `ISA 0xa7`, `MIMICIDLE`,
    // visible (`+0x81`) and enabled (vtable +0x48).
    std::vector<UnitTriggerView> out;
    for (int64_t id : character_order_) {
        const Object* o = find(id);
        const Character* c = character(id);
        if (o == nullptr || c == nullptr || !c->mimic_idle) continue;
        if (!o->visible || !o->enabled || dead_.count(id) != 0 || inactive_.count(id) != 0)
            continue;
        out.push_back(UnitTriggerView{id, o->unit, feet_of(*c)});
    }
    return out;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::unit_mimic_idle(int64_t id) const {
    const Character* c = character(id);
    return c != nullptr && c->mimic_idle;
}

// PORT: a mimic holding `MIMICIDLE` woken as if hurt (`react_to_damage`).
bool LevelScript::attack_idle_mimic(int64_t id, QuestState& quests, std::vector<ScriptEvent>& out) {
    Object* o = find(id);
    Character* c = character(id);
    if (o == nullptr || c == nullptr || !c->mimic_idle) return false;
    react_to_damage(*o, *c, UnitRef{}, true, quests, out);
    return true;
}

// PORT: the level's characters that `find_character_within_view` would consider, for the target selector.
void LevelScript::reachable_characters(std::vector<Reachable>& out) const {
    // The same survivors `find_character_within_view` considers: the dead, the
    // hidden, the disabled and the untargetable are skipped (`candidate`), and
    // so are unit types 0x29/0x2a.
    for (int64_t id : character_order_) {
        const Object* o = find(id);
        if (o == nullptr) continue;
        const Character* c = candidate(*o);
        if (c == nullptr) continue;
        if (o->ai == 0x29 || o->ai == 0x2a) continue;
        Reachable r;
        r.id = id;
        r.unit = o->unit;
        r.center = c->motion.position;
        r.radius = c->motion.radius;
        r.alignment = alignment_of(*o);
        r.mimic_idle = c->mimic_idle;
        out.push_back(r);
    }
}

// ---------------------------------------------------------------------------
// The avoidance map (`CBaseUnit::addToAvoidanceMap` / `removeFromAvoidanceMap`)
// ---------------------------------------------------------------------------

// `CCharacter::addToAvoidanceMap`: a character neither
// shown nor enabled (`CSceneNodeObject::getEnabled`) stays off. One not on the map first
// puts back each pet whose bit is set, then itself (`CBaseUnit::addToAvoidanceMap`):
// a circle of radius + 0.4 on the object grid.
void LevelScript::avoidance_add(Character& c) {
    if (passability_ == nullptr || !passability_->ready()) return;
    const Object* self = find(c.node);
    if (self != nullptr && !self->visible && !self->enabled) return;
    if (!c.on_grid) {
        // The console's list (`+0x5ac`) with its dead dropped.
        for (LevelScript* machine : level_machines()) {
            for (int64_t id : machine->pets_of(this, c.node)) {
                Character* pet = machine->character(id);
                if (pet != nullptr && pet->grid_with_master) machine->avoidance_add(*pet);
            }
        }
    }
    if (c.on_grid) return;
    const UnitStats& stats = stats_for(c.unit);
    if (!stats.occupies_nodes || !stats.collideable) return;
    const float r = stats.radius + 0.4f;
    const Vec3& at = c.motion.position;
    passability_->increment_object_passability({at.x + -r, at.y + 0.0f, at.z + -r},
                                               {at.x + r, at.y + 0.0f, at.z + r}, r);
    c.on_grid = true;
}

// `CCharacter::removeFromAvoidanceMap`: first each pet
// off (its bit set to whether it was on), then itself at its position now.
void LevelScript::avoidance_remove(Character& c) {
    if (passability_ == nullptr || !passability_->ready()) return;
    if (c.on_grid) {
        for (LevelScript* machine : level_machines()) {
            for (int64_t id : machine->pets_of(this, c.node)) {
                Character* pet = machine->character(id);
                if (pet == nullptr) continue;
                pet->grid_with_master = pet->on_grid;
                machine->avoidance_remove(*pet);
            }
        }
    }
    avoidance_remove_self(c);
}

// `CBaseUnit::removeFromAvoidanceMap` alone.
void LevelScript::avoidance_remove_self(Character& c) {
    if (passability_ == nullptr || !passability_->ready() || !c.on_grid) return;
    const UnitStats& stats = stats_for(c.unit);
    const float r = stats.radius + 0.4f;
    const Vec3& at = c.motion.position;
    passability_->decrement_object_passability({at.x + -r, at.y + 0.0f, at.z + -r},
                                               {at.x + r, at.y + 0.0f, at.z + r}, r);
    c.on_grid = false;
}

// ---------------------------------------------------------------------------
// Targets
// ---------------------------------------------------------------------------

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
const LevelScript::Object* LevelScript::target_object(const UnitRef& target) const {
    return target.machine != nullptr ? target.machine->find(target.node) : nullptr;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
const LevelScript::Character* LevelScript::target_character(const UnitRef& target) const {
    return target.machine != nullptr ? target.machine->character(target.node) : nullptr;
}

// PORT: where a target stands: the player's, or a unit's feet.
bool LevelScript::target_at(const UnitRef& target, Vec3& at) const {
    if (target.player()) {
        at = player_at_;
        return true;
    }
    if (const Character* c = target_character(target)) {
        at = feet_of(*c);
        return true;
    }
    return false;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::unit_final_target(int64_t id, Vec3& at) const {
    const Character* c = character(id);
    if (c == nullptr) return false;
    at = c->destination;
    return true;
}

// The frame `canCastCurrentSkill` and `castSkill` look for a skill's target
// in: the unit's position (+0x70) turned along the final target direction.
LevelScript::ViewFrame LevelScript::skill_frame(const Character& c) const {
    ViewFrame frame;
    frame.at = c.motion.position;
    frame.facing = c.facing;
    frame.along = true;
    frame.forward = c.destination_direction;
    return frame;
}

// `CCharacter::setFinalTargetPosition(float x, float z)`: the point at
// height 0, and the ground direction to it from the centre, normalised when longer than
// the minimum.
void LevelScript::set_final_target_position(Character& c, float x, float z) {
    c.destination = {x, 0.0f, z};
    const Vec3& at = c.motion.position;
    float dx = x - at.x;
    float dz = z - at.z;
    // `fmuls 0*0`, `fmadds dx*dx + that`, `fmadds
    // dz*dz + that`.
    const float length = std::sqrt(std::fma(dz, dz, std::fma(dx, dx, 0.0f * 0.0f)));
    if (1e-08 < static_cast<double>(length)) {
        const float inverse = 1.0f / length;
        dx = dx * inverse;
        dz = dz * inverse;
    }
    c.destination_direction = {dx, 0.0f, dz};
}

// `CCharacter::turnToward`, its speed factor:
// movement speed (0x15), a slow damped by 0x8c's share; a pet adds its master's 0x5c
// times 0.01; held to 10; then `fma(p, 0.01, 1)`, at least 0.5.
float LevelScript::turn_factor(const Character& c) const {
    float p = character_effect(c, 0x15);
    if (!(p >= 0.0f)) {
        float k = character_effect(c, 0x8c) * 0.01f;
        k = k < 1.0f ? k : 1.0f;
        k = k > 0.0f ? k : 0.0f;
        p = (1.0f - k) * p;
    }
    if (has_master(c)) {
        float master = 0.0f;
        if (c.master_player) {
            master = master_effects(c).effect_value(0x5c, kDamageTypeAny);
        } else if (const Character* m = c.master.machine->character(c.master.node)) {
            master = c.master.machine->character_effect(*m, 0x5c);
        }
        p = std::fma(master, 0.01f, p);
    }
    if (!(p <= 10.0f)) p = 10.0f;
    float factor = std::fma(p, 0.01f, 1.0f);
    if (!(factor >= 0.5f)) factor = 0.5f;
    return factor;
}

// PORT: a target's `getPosition(true)`: a character's centre, the player's
// feet raised by its radius.
bool LevelScript::target_centre_at(const UnitRef& target, Vec3& at) const {
    if (target.player()) {
        at = player_at_;
        at.y += player_radius_;
        return true;
    }
    if (const Character* c = target_character(target)) {
        at = c->motion.position;
        return true;
    }
    return false;
}

// PORT: a target's radius: the player's, or its unit data's.
float LevelScript::target_radius(const UnitRef& target) const {
    if (target.player()) return player_radius_;
    const Object* o = target_object(target);
    return o != nullptr ? stats_for(o->unit).radius : kDefaultCollisionRadius;
}

// `CCharacter::alive`: not dying or dead.
bool LevelScript::target_alive(const UnitRef& target) const {
    if (target.player()) return player_alive_;
    if (target_object(target) == nullptr || target_character(target) == nullptr) return false;
    return target.machine->dead_.count(target.node) == 0;
}

// Off the level's updated list (`CLevel::updateNearUnitList`).
//
bool LevelScript::target_inactive(const UnitRef& target) const {
    return !target.player() && target.machine->inactive_.count(target.node) != 0;
}

// PORT: a target's AI state: the player's alive or dead, or the unit's.
int32_t LevelScript::target_state(const UnitRef& target) const {
    if (target.player()) return player_alive_ ? ai_state::kIdle : ai_state::kDead;
    const Object* o = target_object(target);
    return o != nullptr ? o->ai : ai_state::kDead;
}

// `CCharacter::alignment()`: `+0x338`, which
// `unitInit` fills from the unit's data and `setAlignment`
// overrides.
namespace {

// `EAlignment` (0 `NEUTRAL`, 1 `GOOD`, 2 `EVIL`) for the port's three sides.
// PORT: the port's enum turned into the console's.
int32_t e_alignment(UnitStats::Alignment side) {
    switch (side) {
        case UnitStats::Alignment::Friendly: return 1;
        case UnitStats::Alignment::Hostile: return 2;
        case UnitStats::Alignment::Neutral: return 0;
    }
    return 0;
}

}  // namespace

UnitStats::Alignment LevelScript::alignment_of(const Object& o) const {
    if (const Character* c = character(o.id); c != nullptr && c->alignment.has_value()) {
        return *c->alignment;
    }
    return stats_for(o.unit).alignment;
}

// `CCharacter::alignment()`: `+0x280`, with
// `GOOD` and `EVIL` swapped while the unit holds a `TURN ALIGNMENT` effect
// (0x50). The player is `GOOD`.
int32_t LevelScript::console_alignment(const Object& o) const {
    const int32_t side = e_alignment(alignment_of(o));
    const Character* c = character(o.id);
    if (c != nullptr && c->effects && c->effects->has_effect(0x50)) {
        if (side == 1) return 2;
        if (side == 2) return 1;
    }
    return side;
}

// PORT: a target's `alignment()`, the player's `GOOD` (1); -1 for a target that
// is not there.
int32_t LevelScript::target_console_alignment(const UnitRef& target) const {
    if (target.player()) return 1;
    const Object* other = target_object(target);
    return other != nullptr ? target.machine->console_alignment(*other) : -1;
}

// `CCharacter::isEnemy(CCharacter*)`: no target or a
// `NEUTRAL` side is no enemy; `BERSERK` takes anybody; side `4` is hostile to all; else
// the sides differ.
bool LevelScript::is_enemy(const Object& o, const UnitRef& target) const {
    const int32_t theirs = target_console_alignment(target);
    if (theirs < 0 || theirs == 0) return false;
    const int32_t mine = console_alignment(o);
    if (mine == 0) return false;
    const Character* c = character(o.id);
    if (c != nullptr && c->ai_flags.has(AIFlag::Berserk)) return true;
    if (theirs == 4 || mine == 4) return true;
    return theirs != mine;
}

// PORT: `is_enemy` asked of a unit by id.
bool LevelScript::unit_is_enemy_of(int64_t node, LevelScript* other, int64_t other_node) const {
    const Object* o = find(node);
    if (o == nullptr) return false;
    if (other == nullptr) return is_enemy(*o, UnitRef{});  // the player
    if (other->find(other_node) == nullptr) return false;
    return is_enemy(*o, UnitRef{other, other_node});
}

// `CCharacter::isFriend(CCharacter*)`: a `NEUTRAL` target
// is a friend; `BERSERK` or side `4` has none; else the sides match. PORT: a missing
// target answers as a neutral one.
bool LevelScript::is_friend(const Object& o, const UnitRef& target) const {
    const int32_t theirs = target_console_alignment(target);
    if (theirs <= 0) return true;
    const Character* c = character(o.id);
    if (c != nullptr && c->ai_flags.has(AIFlag::Berserk)) return false;
    const int32_t mine = console_alignment(o);
    if (theirs == 4 || mine == 4) return false;
    return theirs == mine;
}

// `CCharacter::setTarget`.
void LevelScript::set_target(Object& o, Character& c, bool has, const UnitRef& target,
                             std::vector<ScriptEvent>& out) {
    if (o.ai == ai_state::kDead) {
        if (has) return;
    } else if (o.no_target) {
        return;
    }
    const bool changed = c.has_target != has || (has && c.target != target);
    c.has_target = has;
    c.target = has ? target : UnitRef{};
    if (trace_on_) {
        // As the original's trace writes every `setTarget` call.
        const std::string whom = !has              ? std::string("none")
                                 : target.player() ? trace_player_name_
                                                   : target_unit(target);
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%.3f T %s -> %s", static_cast<double>(clock_),
                      o.unit.c_str(), whom.c_str());
        trace_.push_back(buffer);
    }
    if (!changed) return;
    Vec3 at = player_at_;
    if (has) target_at(target, at);
    ScriptEvent e;
    e.action = ScriptAction::Hunt;
    e.node_id = o.id;
    e.name = o.name;
    e.unit = o.unit;
    e.on = has;
    e.text =
        has ? (target.player() ? std::string("the player") : target_unit(target)) : std::string();
    e.position = at;
    e.target_node = target.node;
    e.target_machine = target.machine;
    out.push_back(e);
}

// PORT: a unit the player walks up to takes the player as its target and stops (`set_target`).
void LevelScript::approached_by_player(int64_t node, std::vector<ScriptEvent>& out) {
    Object* o = find(node);
    Character* c = character(node);
    if (o == nullptr || c == nullptr) return;
    set_target(*o, *c, true, UnitRef{}, out);
    stop_pathing(*c);
}

// PORT: the talk target lets the player go (`set_target`); a unit that cannot walk faces home.
void LevelScript::release_player_target(std::vector<ScriptEvent>& out) {
    const int64_t node = player_target_;
    player_target_ = 0;
    if (node == 0) return;
    Object* o = find(node);
    Character* c = character(node);
    if (o == nullptr || c == nullptr) return;
    if (!c->has_target || !c->target.player()) return;
    set_target(*o, *c, false, UnitRef{}, out);
    c->face_home = stats_for(o->unit).movement.walking_speed == 0.0f;
}

// PORT: a unit made to target the player (`set_target`).
void LevelScript::target_player(int64_t node, std::vector<ScriptEvent>& out) {
    Object* o = find(node);
    Character* c = character(node);
    if (o == nullptr || c == nullptr) return;
    set_target(*o, *c, true, UnitRef{}, out);
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
void LevelScript::target_unit(int64_t node, LevelScript* machine, int64_t target,
                              std::vector<ScriptEvent>& out) {
    Object* o = find(node);
    Character* c = character(node);
    if (o == nullptr || c == nullptr || machine == nullptr) return;
    set_target(*o, *c, false, UnitRef{machine, target}, out);
}

// CLevel::restartLevel: vtable +0x1f8 on every character from the list's head;
// `CLevel::addCharacter` adds at the head, so newest
// first. The order is observable (the cave boss room's Alric and Colossus).
void LevelScript::restart_level(QuestState& quests, std::vector<ScriptEvent>& out) {
    const std::vector<int64_t> order(character_order_.rbegin(), character_order_.rend());
    for (int64_t id : order) {
        Object* o = find(id);
        Character* found = character(id);
        if (o == nullptr || found == nullptr) continue;
        Character& c = *found;
        // CCharacter::levelResetting.
        // `CBaseUnit::levelResetting` stops skills, which no character here
        // runs.
        if (static_cast<float>(static_cast<int>(c.combat.hp)) > 0.0f) {
            set_ai_state(*o, ai_state::kNone, quests, out);
        }
        set_target(*o, c, false, UnitRef{}, out);
    }
}

// `CLevel::sightUnobstructed(from, to, false)`, as `findCharacterWithinView`
// and `validLineOfSight` ask it: units' own models are cast too,
// so a shut door blocks the view.
bool LevelScript::sight_unobstructed(const Vec3& from, const Vec3& to) const {
    if (collision_ == nullptr || collision_->empty()) return true;
    CollisionHit hit;
    return !collision_->ray_collision(from, to, /*objects=*/true, hit);
}

// `CLevel::findCharacterWithinView`'s first
// gates on a unit: a monster, visible, enabled, targetable, alive, active.
const LevelScript::Character* LevelScript::candidate(const Object& other) const {
    if (other.type != type::kMonster) return nullptr;
    const Character* them = character(other.id);
    if (them == nullptr) return nullptr;
    if (!other.visible || !other.enabled || !other.targetable || other.unit.empty()) return nullptr;
    if (dead_.count(other.id) != 0 || inactive_.count(other.id) != 0) return nullptr;
    return them;
}

// `CCharacter::getEffectValue(type, 7)`: own manager and inventory.
// `MONSTER_HUNT` is a flag here (`hunt_affix`), its passive effects added while held:
// 0x29 100, 0xe 360, 0x2c 200. INVENTED: its `DEFENSE` effect is not carried.
float LevelScript::character_effect(const Character& c, int type) const {
    float value = CharacterEffects(c.effects.get(), c.inventory).effect_value(type, kDamageTypeAny);
    if (c.hunt_affix) {
        if (type == 0x29) value += 100.0f;
        if (type == 0xe) value += 360.0f;
        if (type == 0x2c) value += 200.0f;
    }
    return value;
}

// `CCharacter::sightRange`: `SIGHT_RADIUS` (with the attention bonus,
// not for `ISA 0xa7`), 35 for a walking, running pet; plus `SIGHT
// BONUS` (0xd), times 1 + 0x29; plus `ACTIVERANGEMOD`; doubled under `AWARE`.
float LevelScript::sight_range(const Object& o, const Character& c) const {
    const UnitStats& mine = stats_for(o.unit);
    const bool mimic = unit_types_ != nullptr && unit_types_->isa(mine.unittype, unit_type::kMimic);
    float sight = mine.sight_radius;
    if (0.0f < sight && !mimic) sight = std::max(0.0f, sight + attention_bonus_);
    if ((c.master_player || has_master(c)) && 0.0f < character_speed(c, c.motion.walking_speed) &&
        0.0f < character_speed(c, c.motion.running_speed)) {
        sight = 35.0f;
    }
    sight = character_effect(c, 0xd) + sight;
    sight = (character_effect(c, 0x29) + 1.0f) * sight;
    sight = active_range_mod_ + sight;
    if (c.ai_flags.has(AIFlag::Aware)) sight = sight * 2.0f;
    return sight;
}

// `CCharacter::calculateTargetInSight(alignment, nearest)`:
// the cone's three numbers worked out and handed to `findCharacterWithinView`
// from the unit's own frame, then twice more when that finds no one.
bool LevelScript::find_character_within_view(const Object& o, const Character& c, bool nearest,
                                             UnitRef& found, int32_t alignment) {
    const UnitStats& mine = stats_for(o.unit);
    // 7 asks `getTargetAlignment`.
    if (alignment == 7) alignment = target_alignment_of(o, c);
    // `MOTION_RADIUS` (+0x2a0) with the attention bonus `unitInit` adds when it
    // is above 0 (`*_MONSTER_ATTENTION_BONUS`) - not for a unit that `ISA 0xa7`.
    const bool mimic = unit_types_ != nullptr && unit_types_->isa(mine.unittype, unit_type::kMimic);
    float motion = mine.motion_radius;
    if (0.0f < motion && !mimic) motion = std::max(0.0f, motion + attention_bonus_);
    // `VIEW_ANGLE` (+0x29c) plus `VIEW ANGLE BONUS`
    // (0xe), times one plus `PERCENT VIEW ANGLE BONUS` (0x2a); 360 under `AWARE`.
    float view = (character_effect(c, 0x2a) + 1.0f) * (character_effect(c, 0xe) + mine.view_angle);
    if (c.ai_flags.has(AIFlag::Aware)) view = 360.0f;
    float sight = sight_range(o, c);
    // `PERCENT BLIND` (0x90) takes its share off all
    // three, held to [0, 1] (a NaN is 0).
    float factor = 1.0f - character_effect(c, 0x90);
    factor = factor > 0.0f ? factor : 0.0f;
    factor = factor < 1.0f ? factor : 1.0f;
    // a mimic holding `MIMICIDLE` looks no further than
    // its motion radius.
    if (mimic && c.mimic_idle) sight = motion;
    // The looker's frame at its position (+0x70, the centre).
    ViewFrame frame;
    frame.at = c.motion.position;
    frame.facing = c.facing;
    if (find_within_view(frame, o, alignment, motion * factor, view * factor, sight * factor,
                         nearest, nullptr, found)) {
        return true;
    }
    // with a skill held (+0x2c4), again with its `FINDTARGETANGLE`
    // (the property in use, +0x5c, its +0x64; 0 with none) and its
    // `TARGET_ALIGNMENT` (+0xe8), the skill vetting each.
    if (const UnitSkill* held = active_unit_skill(const_cast<Character&>(c))) {
        const SkillProperty* property = held->known.property(held->known.effective);
        const float angle = property != nullptr ? property->find_target_angle : 0.0f;
        const SkillDef* def = held->known.def;
        const int32_t skill_alignment = def != nullptr ? def->target_alignment : 2;
        if (find_within_view(frame, o, skill_alignment, motion * factor, angle * factor,
                             sight * factor, nearest, held, found)) {
            return true;
        }
    }
    // a character with a master (+0x5a4) that is not `ISA 0xa9`
    // looks again from its master's frame, the cone without the blindness.
    const bool no_master_view =
        unit_types_ != nullptr && unit_types_->isa(mine.unittype, unit_type::kPetNoFollow);
    MasterView master;
    if (!no_master_view && master_view(c, master)) {
        // the master's orientation matrix (vtable
        // +0x6c), its translation the master's `getPosition(true)`.
        ViewFrame from_master;
        from_master.at = master.centre;
        from_master.facing = std::atan2(master.forward.x, master.forward.z);
        return find_within_view(from_master, o, alignment, motion, view, sight, nearest, nullptr,
                                found);
    }
    return false;
}

// The `CCharacter::isOfAlignment`-like test of `findCharacterWithinView`: an
// untargetable one never; `ALL` (3) always; 1 and 6 `GOOD`, 2 and 5 `EVIL`
// (`CCharacter::alignment`); any other number anyone.
bool LevelScript::of_alignment(int32_t wanted, int32_t side, bool targetable) {
    if (!targetable) return false;
    if (wanted == 3) return true;
    if (wanted == 1 || wanted == 6) return side == 1;
    if (wanted == 2 || wanted == 5) return side == 2;
    return true;
}

// `CLevel::+0x54`, the near list, newest first (`CLList::add`); the player
// after those made before it (`player_joined_`).
// INVENTED: across layouts, machines in `set_level_machines` order, each newest first.
std::vector<LevelScript::UnitRef> LevelScript::near_list_order() {
    std::vector<UnitRef> out;
    bool player_placed = false;
    bool first = true;
    for (LevelScript* machine : level_machines()) {
        const std::vector<int64_t>& order = machine->character_order_;
        const size_t joined = std::min(machine->player_joined_, order.size());
        for (size_t k = order.size(); k-- > 0;) {
            if (first && !player_placed && k < joined) {
                out.push_back(UnitRef{});
                player_placed = true;
            }
            out.push_back(UnitRef{machine, order[k]});
        }
        if (first && !player_placed) {
            out.push_back(UnitRef{});
            player_placed = true;
        }
        first = false;
    }
    if (!player_placed) out.push_back(UnitRef{});
    return out;
}

// `CLevel::findCharacterWithinView(frame, alignment, motion, view, sight, nearest, exclude,
// skill)`: the near list in order; a candidate alive (or dead for a
// corpse skill), enabled, visible, not the looker, in range, of the alignment, a valid
// target, not an idle mimic, passing the skill's test; then in the cone
// and in clear sight five units up (`CLevel::rayCollision`). The first, or the nearest.
bool LevelScript::find_within_view(const ViewFrame& frame, const Object& o, int32_t alignment,
                                   float motion, float view, float sight, bool nearest,
                                   const UnitSkill* skill, UnitRef& found) {
    const bool corpses =
        skill != nullptr && skill->known.def != nullptr && skill->known.def->target_corpses;
    float best = 9999.0f;
    bool any = false;
    const Character* self = character(o.id);
    for (const UnitRef& who : near_list_order()) {
        Vec3 at;
        if (who.machine == nullptr) {
            // The player: alive, enabled, `GOOD`.
            if (!player_alive_ || !level_player_enabled()) continue;
            if (!of_alignment(alignment, 1, true)) continue;
            if (skill != nullptr && self != nullptr &&
                !unit_skill_target_valid(o, *self, *skill, who))
                continue;
            // `getPosition(true)`: the player's centre.
            at = {player_at_.x, player_at_.y + player_radius_, player_at_.z};
        } else {
            LevelScript& m = *who.machine;
            const Object* other = m.find(who.node);
            const Character* them = m.character(who.node);
            if (other == nullptr || them == nullptr) continue;
            const bool dead = other->ai == ai_state::kDying || other->ai == ai_state::kDead ||
                              m.dead_.count(other->id) != 0;
            if (dead && !corpses) continue;
            if (!other->enabled || !other->visible) continue;
            if (&m == this && other->id == o.id) continue;
            if (!them->active) continue;
            // The can-be-targeted flag (+0x511) as it is: the port's `targetable_flag`
            // (`targetable` is targetable-and-alive, which a death clears).
            if (!of_alignment(alignment, m.console_alignment(*other),
                              other->targetable_flag && m.stats_for(other->unit).targetable)) {
                continue;
            }
            if (!corpses) {
                if (m.pet_near_death(*other, *them)) continue;
                if (dead || other->ai == 0x29 || other->ai == 0x2a) continue;
            }
            if (them->mimic_idle && m.unit_types_ != nullptr &&
                m.unit_types_->isa(m.stats_for(other->unit).unittype, unit_type::kMimic)) {
                continue;
            }
            if (skill != nullptr && self != nullptr &&
                !unit_skill_target_valid(o, *self, *skill, who))
                continue;
            // `getPosition(true)`: the unit's centre (+0x70).
            at = them->motion.position;
        }
        if (frame.along ? !can_see_along(frame.at, frame.forward, at, motion, view, sight)
                        : !can_see(frame.at, frame.facing, at, motion, view, sight)) {
            continue;
        }
        if (!sight_unobstructed({frame.at.x, frame.at.y + 5.0f, frame.at.z},
                                {at.x, at.y + 5.0f, at.z}))
            continue;
        const float d = distance_xz(frame.at, at);
        if (!nearest) {
            found = who;
            return true;
        }
        if (!(best <= d)) {
            best = d;
            found = who;
            any = true;
        }
    }
    return any;
}

namespace {}  // namespace

// `CCharacter::getTargetAlignment`.
int32_t LevelScript::target_alignment_of(const Object& o, const Character& c) const {
    if (c.ai_flags.has(AIFlag::Berserk)) return 3;
    return e_alignment(alignment_of(o)) != 1 ? 6 : 5;
}

// `CSkill::getTargetIsValid(who, owner, at, false)`,
// `o` the owner.
bool LevelScript::unit_skill_target_valid(const Object& o, const Character& c, const UnitSkill& s,
                                          const UnitRef& who) const {
    SkillTargetQuery query;
    const SkillProperty* property = s.known.property(s.known.effective);
    query.has_property = property != nullptr;
    query.target_type = property != nullptr ? property->target_type : 0;
    const SkillDef* def = s.known.def;
    query.target_alignment = def != nullptr ? def->target_alignment : 2;
    query.dead_wanted = def != nullptr && def->target_corpses;
    query.owner_alignment = e_alignment(alignment_of(o));
    // `+0x1b0` among `TARGET_SPECIFIC_UNITS`, when it lists any. No list
    // names a player class, so the player is in none.
    const auto specific = [def](const std::string& unit, bool player) {
        if (def == nullptr || def->target_specific_units.empty()) return true;
        if (player) return false;
        const std::string wanted = upper(unit);
        for (const std::string& name : def->target_specific_units) {
            if (name == wanted) return true;
        }
        return false;
    };
    // `TARGET_UNITTYPE` is unauthored, so every unit matches.
    // alive is `fctiwz(floor(hp)) > 0` (`floor`): under
    // one hit point counts as dead; NaN is not alive.
    const auto whole_hp = [](float hp) { return std::floor(hp) >= 1.0f; };
    SkillTargetFacts& owner = query.owner;
    owner.alive = whole_hp(c.combat.hp);
    owner.is_owner = true;
    owner.specific_unit = specific(o.unit, false);
    SkillTargetFacts& t = query.candidate;
    if (who.player()) {
        t.alive = whole_hp(player_hp_);
    } else {
        const Character* them = target_character(who);
        t.alive = them != nullptr && whole_hp(them->combat.hp);
    }
    t.enemy = is_enemy(o, who);
    if (who.player()) {
        t.is_owners_master = c.master_player;
        t.specific_unit = specific(std::string(), true);
    } else {
        t.is_owner = who.machine == this && who.node == o.id;
        const Character* them = target_character(who);
        // `+0x640`: its master is the owner, or the owner's master is it.
        t.master_is_owner = them != nullptr && them->has_master && !them->master_player &&
                            them->master == UnitRef{const_cast<LevelScript*>(this), o.id};
        t.is_owners_master = c.has_master && !c.master_player && c.master == who;
        t.specific_unit = specific(target_unit(who), false);
    }
    return skill_target_is_valid(query);
}

// ---------------------------------------------------------------------------
// The swing
// ---------------------------------------------------------------------------

// `CGenericModel::findRandomAnimation(prefix)`: the first
// upper-cased name with the prefix, each later one replacing it on
// `randomBetweenVolatile(0, 1000) < 500`.
int LevelScript::find_random_animation(const std::string& unit, const std::string& prefix,
                                       std::string& name, float& length) const {
    if (!animation_catalog_) return -1;
    const std::vector<std::pair<std::string, float>> catalog = animation_catalog_(unit);
    const std::string wanted = upper(prefix);
    int chosen = -1;
    for (size_t i = 0; i < catalog.size(); ++i) {
        const std::string candidate = upper(catalog[i].first);
        if (candidate.size() < wanted.size() || candidate.compare(0, wanted.size(), wanted) != 0) {
            continue;
        }
        if (chosen == -1 || volatile_random().between(0.0f, 1000.0f) < 500.0f) {
            chosen = static_cast<int>(i);
        }
    }
    if (chosen >= 0) {
        name = catalog[static_cast<size_t>(chosen)].first;
        length = catalog[static_cast<size_t>(chosen)].second;
    }
    return chosen;
}

// `CCharacter +0x2c0`, the attack in use: the weapon's that `selectAttack`
// took, or the unit's own (a list of one,
// `randomIntegerBetweenVolatile(0, 0)` draws nothing).
const AttackDescription& LevelScript::current_attack(const Character& c,
                                                     const UnitStats& stats) const {
    (void)stats;
    if (const UnitStats::WeaponAttack* weapon = weapon_in_use(c)) return weapon->attack;
    return c.combat.attack;
}

// of the weapon in use (`+0x3ec`), 0 with none.
float LevelScript::weapon_arc(const Character& c) const {
    const UnitStats::WeaponAttack* weapon = weapon_in_use(c);
    return weapon != nullptr ? weapon->arc : 0.0f;
}

// PORT: the weapon of the hand a unit attacks with (`CCharacter::selectAttack`'s choice).
const UnitStats::WeaponAttack* LevelScript::weapon_in_use(const Character& c) const {
    if (c.attack_left) return c.left_weapon_attack.present ? &c.left_weapon_attack : nullptr;
    return c.weapon_attack.present ? &c.weapon_attack : nullptr;
}

// PORT: a level unit's hands as `HeldHands` reads them (`calculateCombatStats`'s inputs).
HeldHands LevelScript::held_hands(const Character& c) const {
    HeldHands hands;
    hands.using_weapon = weapon_in_use(c) != nullptr;
    const auto fill = [](HeldHand& hand, const std::string& item,
                         const UnitStats::WeaponAttack& weapon) {
        hand.held = !item.empty();
        hand.weapon = weapon.present;
        hand.ranged = weapon.ranged;
        hand.melee = weapon.melee;
        hand.range = weapon.attack.range;
    };
    fill(hands.right, c.right_hand, c.weapon_attack);
    fill(hands.left, c.left_hand, c.left_weapon_attack);
    return hands;
}

// `CCharacter::selectAttack(EATTACK_RANGE_TYPE)`.
//
void LevelScript::select_attack(Character& c, int mode) {
    // an attack with swing time left is kept.
    if (0.0f < c.swing_left) return;
    // No model (+0x1ac), nothing chosen.
    if (stats_for(c.unit).mesh.empty()) return;
    c.has_attack = true;
    c.attack_left = select_attack_hand(held_hands(c), mode, c.alternate_hand) == AttackHand::Left;
    // The attack in use: the hand's weapon's, else an unarmed one drawn
    // (`randomIntegerBetweenVolatile(0, count - 1)`; the port keeps one). Its animation via
    // `CGenericModel::findRandomAnimation`, kept for `attack`.
    std::string prefix;
    if (const UnitStats::WeaponAttack* weapon = weapon_in_use(c)) {
        prefix = weapon->attack.animation;
    } else {
        (void)volatile_random().integer_between(0, 0);
        prefix = c.combat.attack.animation;
    }
    c.attack_animation =
        find_random_animation(c.unit, prefix, c.attack_animation_name, c.attack_animation_length);
}

// `CCharacter::attack`.
void LevelScript::choose_attack_hand(const Object& o, Character& c, const UnitStats& stats) {
    const HeldHands hands = held_hands(c);
    switch (attack_hand_step(hands)) {
        case AttackHandStep::SelectAny: select_attack(c, 0); return;
        case AttackHandStep::Keep: return;
        case AttackHandStep::Mixed: break;
    }
    // `rangedRange`, then `meleeRange`: each,
    // with no attack in use (+0x2c0), runs `selectAttack(0)` and gives 0.
    float ranged = 0.0f;
    if (!c.has_attack) {
        select_attack(c, 0);
    } else {
        ranged = ranged_range(stats, c.scale, current_attack(c, stats), hands);
    }
    float melee = 0.0f;
    if (!c.has_attack) {
        select_attack(c, 0);
    } else {
        melee = melee_range(stats, c.scale, current_attack(c, stats), hands);
    }
    if (melee == ranged) return;
    // The melee hand's arc; a foe within its reach and a metre more, 1.5 out,
    // takes the melee hand (`selectAttack(1)`), else the ranged (2).
    const UnitStats::WeaponAttack& melee_hand =
        attack_melee_hand_is_right(hands) ? c.weapon_attack : c.left_weapon_attack;
    UnitRef found;
    const int mode =
        melee_hand.arc != 0.0f && find_foe_in_view(o, c, 1.5f, melee_hand.arc, melee + 1.0f, found)
            ? 1
            : 2;
    select_attack(c, mode);
}

// `CLevel::findCharacterWithinView(frame, getTargetAlignment(), near, angle, range, true,...)
// `, as `CCharacter::attack` and
// `performAttack` call it: the nearest foe.
bool LevelScript::find_foe_in_view(const Object& o, const Character& c, float near, float angle,
                                   float range, UnitRef& found) {
    ViewFrame frame;
    frame.at = c.motion.position;
    frame.facing = c.facing;
    return find_within_view(frame, o, target_alignment_of(o, c), near, angle, range,
                            /*nearest=*/true, nullptr, found);
}

bool LevelScript::in_attack_range(Character& c, const UnitStats& stats) {
    Vec3 at;
    if (!c.has_target || !target_at(c.target, at)) return false;
    // no attack in use (+0x2c0), `selectAttack(0)`; still none, out
    // of range.
    if (!c.has_attack) {
        select_attack(c, 0);
        if (!c.has_attack) return false;
    }
    // `inAttackRange(0)`: a `Ranged Weapon` in either hand
    // lets the height by and adds `getEffectValue(0x4e, 7)`; `attackRange`
    // reads both hands.
    const HeldHands hands = held_hands(c);
    const bool ranged =
        (hands.left.held && hands.left.ranged) || (hands.right.held && hands.right.ranged);
    const float bonus = ranged ? character_effect(c, 0x4e) : 0.0f;
    // both `getPosition(true)`, the centres.
    Vec3 centre;
    if (!target_centre_at(c.target, centre)) return false;
    return tl::core::in_attack_range(stats, c.scale, current_attack(c, stats), c.motion.position,
                                     centre, target_radius(c.target), ranged, bonus, &hands);
}

// `CCharacter::validLineOfSight(CLevel&, flags)`: true
// under `NO LINE OF SIGHT` or with no target. The ray goes from the centre
// 1.5 up to the target's centre, the rise held each way to a fifth of the world
// coordinates' length (`fmuls`/`fmadds`); a NaN takes the limit.
bool LevelScript::valid_line_of_sight(const Character& c) const {
    if (c.ai_flags.has(AIFlag::NoLineOfSight)) return true;
    Vec3 centre;
    if (!c.has_target || !target_centre_at(c.target, centre)) return true;
    const Vec3& self = c.motion.position;
    const Vec3 from{self.x + 0.0f, (0.0f + self.y) + 1.5f, self.z + 0.0f};
    Vec3 to{centre.x, (centre.y - self.y) + from.y, centre.z};
    const float rise = to.y - from.y;
    const float limit =
        std::sqrt(std::fma(from.y, from.y, std::fma(to.z, to.z, to.x * to.x))) * 0.2f;
    const float held = rise < limit ? rise : limit;
    to.y = (-limit < held ? held : -limit) + from.y;
    return sight_unobstructed(from, to);
}

// `CLevel::getActiveCharactersAtPosition`: every
// near-list character of the type and alignment (3 any) within `radius` (3D, inclusive)
// passing `alive == include_living || !alive == include_dead` (both off: the living pass).
// INVENTED: the player first, then the machines' characters.
void LevelScript::get_active_characters_at_position(const Vec3& pos, uint32_t unittype,
                                                    uint32_t alignment, float radius,
                                                    bool include_living, bool include_dead,
                                                    std::vector<int64_t>& out) const {
    const auto passes_life = [&](bool alive) {
        return alive == include_living || !alive == include_dead;
    };
    if (level_player_enabled()) {
        const bool alive = player_alive_;
        if (passes_life(alive)) {
            if (alignment == 3 || alignment == 1) {
                if (unittype == 0) {
                    const float dx = pos.x - player_at_.x;
                    const float dy = pos.y - player_at_.y;
                    const float dz = pos.z - player_at_.z;
                    if (std::sqrt(dx * dx + dy * dy + dz * dz) <= radius) {
                        out.push_back(0);
                    }
                }
            }
        }
    }

    for (LevelScript* machine : level_machines()) {
        for (int64_t id : machine->character_order_) {
            const Character* c = machine->character(id);
            if (c == nullptr) continue;
            const Object* obj = machine->find(id);
            if (obj == nullptr) continue;

            const bool alive = machine->dead_.count(id) == 0 && c->combat.hp > 0.0f;
            if (!passes_life(alive)) continue;

            if (alignment != 3) {
                const UnitStats::Alignment side = machine->alignment_of(*obj);
                uint32_t unit_align = 0;
                if (side == UnitStats::Alignment::Hostile) unit_align = 2;
                else if (side == UnitStats::Alignment::Friendly) unit_align = 1;
                if (unit_align != alignment) continue;
            }

            // INVENTED: unittype hierarchy check omitted as live units carry no hierarchy in LevelScript;
            // all shipped watchers author ANY.
            (void)unittype;

            const float dx = pos.x - c->motion.position.x;
            const float dy = pos.y - c->motion.position.y;
            const float dz = pos.z - c->motion.position.z;
            if (std::sqrt(dx * dx + dy * dy + dz * dz) <= radius) {
                out.push_back(id);
            }
        }
    }
}

// `CCharacter::attack()`.
bool LevelScript::attack(Object& o, Character& c, QuestState& quests,
                         std::vector<ScriptEvent>& out) {
    const UnitStats& stats = stats_for(o.unit);
    // no swing without a target, with `NO_UNARMED_ATTACKS`, while a
    // skill holds the unit (`performingSkillLoose`) or a swing runs.
    //
    if (!c.has_target) return false;
    if (stats.no_unarmed_attacks) return false;
    if (c.casting.active >= 0 && skill_holds_character(c.casting.cast)) return false;
    if (c.has_attack && 0.0f < c.swing_left) return false;
    // a mimic (0xa7) still `MIMICIDLE` is not swung at: a ranged
    // weapon walks to it, anything else wakes it (`reactToDamage(this, true)`).
    if (!c.target.player() && c.target.machine != nullptr && unit_types_ != nullptr) {
        Object* to = c.target.machine->find(c.target.node);
        Character* them = c.target.machine->character(c.target.node);
        if (to != nullptr && them != nullptr && them->mimic_idle &&
            unit_types_->isa(c.target.machine->stats_for(to->unit).unittype, unit_type::kMimic)) {
            const UnitStats::WeaponAttack* weapon = weapon_in_use(c);
            if (weapon != nullptr && weapon->ranged) {
                set_destination(c, stats, them->motion.position.x, them->motion.position.z);
            } else {
                c.target.machine->react_to_damage(*to, *them, UnitRef{this, o.id}, true, quests,
                                                  out);
            }
            return false;
        }
    }
    choose_attack_hand(o, c, stats);
    const AttackDescription& chosen = current_attack(c, stats);
    // The animation `selectAttack` chose (+0x2c0's +0x70) and its length
    // (`getAnimationLengthSeconds`).
    if (c.attack_animation < 0) return false;
    const std::string animation = c.attack_animation_name;
    const float length = c.attack_animation_length;

    c.struck = false;
    // a slow (or NaN) of PERCENT ATTACK SPEED (0x16) damped by
    // 0x8c's share; speed = fma(percent, 0.01f, 1) / the attack's speed, at least 0.2 (NaN
    // 0.2); 1.5x under BERSERK; at most 1.75, a console-only cap.
    // `CCharacter::getEffectValue(type, 7)`: own manager and inventory.
    float percent = character_effect(c, 0x16);
    if (!(percent >= 0.0f)) {
        float kept = character_effect(c, 0x8c) * 0.01f;
        kept = kept < 1.0f ? kept : 1.0f;
        kept = kept > 0.0f ? kept : 0.0f;
        percent = (1.0f - kept) * percent;
    }
    float speed = std::fma(percent, 0.01f, 1.0f) / chosen.speed;
    if (!(speed >= 0.2f)) speed = 0.2f;
    if (c.ai_flags.has(AIFlag::Berserk)) speed = speed * 1.5f;
    if (speed > 1.75f) speed = 1.75f;
    c.swing_left = length / speed;
    c.swing = animation;
    c.gait.clear();

    // `blendAnimation(index, false, 0.2, speed, -1)`.
    blend_reset(c);
    ScriptEvent e;
    e.action = ScriptAction::Animation;
    e.node_id = o.id;
    e.name = o.name;
    e.unit = o.unit;
    e.text = animation;
    e.on = true;
    e.loops = false;
    e.then_idle = true;
    e.blend = 0.2f;
    e.rate = speed;
    out.push_back(e);
    if (trace_on_) {
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%.3f A %s blend %s loop=0",
                      static_cast<double>(clock_), o.unit.c_str(), animation.c_str());
        trace_.push_back(buffer);
    }
    // an armed attack (the armed flag, +0x24 - every
    // weapon's reads 1 on the console) swung with a weapon in use (+0x3ec): the
    // unit's bank plays `EFFORT` (slot 0x1a).
    if (weapon_in_use(c) != nullptr) play_unit_sample(o, static_cast<int>(SoundSlot::Effort), out);
    // the fidget clock (+0x214) held to 5.
    if (c.fidget_wait > 5.0f) c.fidget_wait = 5.0f;
    return true;
}

// `CCharacter::performAttack(0, 0, 1, 1, 7)`.
void LevelScript::perform_attack(Object& o, Character& c, QuestState& quests,
                                 std::vector<ScriptEvent>& out) {
    // `+0x2b2` flipped first; the next
    // `selectAttack(0)` takes the other hand.
    c.alternate_hand = !c.alternate_hand;
    const UnitStats& stats = stats_for(o.unit);
    const AttackDescription& chosen = current_attack(c, stats);
    const Vec3 here = feet_of(c);
    UnitRef victim;
    bool have = false;
    if (c.has_target) {
        if (!is_enemy(o, c.target)) return;
        victim = c.target;
        have = true;
    }
    // (flag 0x10 clear): the target, or the nearest foe 1.5 to 21 out
    // within 30 degrees; then the weapon's `MISSILE` (`CEquipment::fireMissiles`), ending the
    // blow. INVENTED: `executeProcs(0x4a)`, the wand's item search and `doWeaponSkill` are
    // the player's here.
    if (const UnitStats::WeaponAttack* weapon = weapon_in_use(c);
        weapon != nullptr && !weapon->missile.empty()) {
        UnitRef aim = victim;
        bool aimed = have;
        if (!aimed) aimed = find_foe_in_view(o, c, 1.5f, 30.0f, 21.0f, aim);
        ScriptEvent e;
        e.action = ScriptAction::WeaponMissile;
        e.node_id = o.id;
        e.name = o.name;
        e.unit = o.unit;
        e.text = weapon->missile;
        e.on = c.attack_left;
        e.aimed = aimed;
        e.target_node = aimed ? aim.node : 0;
        e.target_machine = aimed ? aim.machine : nullptr;
        out.push_back(e);
        return;
    }
    Vec3 at;
    // `inStrikeRange`: both `getPosition(true)`, the centres.
    Vec3 victim_centre;
    const bool in_range = have && target_at(victim, at) &&
                          target_centre_at(victim, victim_centre) &&
                          in_strike_range(stats, c.scale, chosen, c.motion.position, victim_centre,
                                          target_radius(victim));
    if (!in_range) {
        // Whoever stands in front within the strike range and a metre more.
        const float reach =
            (c.scale * stats.reach + 0.2f + chosen.strike_range + 0.0f) * stats.range_multiplier +
            1.0f;
        UnitRef other;
        bool got = false;
        float best = std::numeric_limits<float>::max();
        auto consider = [&](const UnitRef& who, const Vec3& there) {
            if (!view_test(here, c.facing, there, 0.0f, 60.0f, reach)) return;
            if (!sight_unobstructed({here.x, here.y + 5.0f, here.z},
                                    {there.x, there.y + 5.0f, there.z})) {
                return;
            }
            const float d = distance_xz(here, there);
            if (d < best) {
                best = d;
                other = who;
                got = true;
            }
        };
        const UnitStats::Alignment side = alignment_of(o);
        if (level_player_enabled() && player_alive_ && side == UnitStats::Alignment::Hostile) {
            consider(UnitRef{}, player_at_);
        }
        for (LevelScript* machine : level_machines()) {
            for (int64_t id : machine->character_order_) {
                const Object* found_object = machine->find(id);
                if (found_object == nullptr) continue;
                const Object& them = *found_object;
                const Character* tc = machine->candidate(them);
                if (tc == nullptr) continue;
                if (!fights(side, machine->alignment_of(them),
                            machine->stats_for(them.unit).targetable)) {
                    continue;
                }
                consider(UnitRef{machine, them.id}, feet_of(*tc));
            }
        }
        if (got) {
            victim = other;
            have = true;
        }
    }
    if (!have || !target_at(victim, at)) return;

    // A wall between (`melee_attack_walled`): three rays at 5 above, the
    // second and third moved back along the facing and sideways; all three
    // must meet something.
    const bool walled = collision_ != nullptr && !collision_->empty() &&
                        melee_attack_walled(*collision_, here, c.facing, at);
    // `inStrikeRange`: a `Ranged Weapon` in use lets the
    // height by and adds `getEffectValue(0x4e, 7)`.
    const UnitStats::WeaponAttack* in_use = weapon_in_use(c);
    const bool ranged = in_use != nullptr && in_use->ranged;
    // Both `getPosition(true)`, the centres.
    Vec3 struck_centre = at;
    target_centre_at(victim, struck_centre);
    if (!tl::core::in_strike_range(stats, c.scale, chosen, c.motion.position, struck_centre,
                                   target_radius(victim), ranged,
                                   ranged ? character_effect(c, 0x4e) : 0.0f)) {
        return;
    }
    if (!facing_target(c.facing, here, at, stats.attack_angle, weapon_arc(c))) return;
    if (walled) {
        ScriptEvent miss;
        miss.action = ScriptAction::Miss;
        miss.node_id = o.id;
        miss.unit = o.unit;
        out.push_back(miss);
        return;
    }

    // `rollAttack`.
    Defender defender;
    if (victim.player()) {
        defender = player_defense_;
    } else if (target_character(victim) != nullptr) {
        victim.machine->unit_defender(victim.node, defender);
    }
    if (!victim.player() &&
        (target_state(victim) == ai_state::kDying || target_state(victim) == ai_state::kDead)) {
        return;
    }
    // `rollAttack`: 0 from a mimic that holds
    // `MIMICIDLE`.
    if (c.mimic_idle) return;
    int absorbed = 0;
    defender.on_absorbed = [&absorbed] { ++absorbed; };
    const Blow blow = roll_attack(c.combat, defender, volatile_random(), c.effects.get(), 1.0f,
                                  1.0f, master_effects(c));
    ScriptEvent e;
    e.action = ScriptAction::Strike;
    e.roll_attack = true;
    e.node_id = o.id;
    e.name = o.name;
    e.unit = o.unit;
    e.target_node = victim.node;
    e.target_machine = victim.machine;
    e.text = victim.player() ? std::string("the player") : target_unit(victim);
    e.amount = static_cast<float>(blow.damage);
    e.critical = blow.critical;
    e.blocked = blow.blocked;
    e.absorbed = absorbed;
    e.by_type = blow.by_type;
    e.parts = blow.parts;
    // What `rollAttack` hands `applyDamage`: the attacker-to-target direction,
    // turned by the jitter, and the knock-back force.
    e.has_direction = true;
    e.direction = blow_direction(here, at, blow.direction_jitter);
    e.force = blow.knockback;
    out.push_back(e);
    (void)quests;
}

// `CCharacter::updateAttack(CLevel&)`.
void LevelScript::attack_hit(int64_t id, QuestState& quests, std::vector<ScriptEvent>& out) {
    Object* o = find(id);
    Character* c = character(id);
    if (o == nullptr || c == nullptr) return;
    if (!(0.0f < c->swing_left) || o->ai == ai_state::kDying || o->ai == ai_state::kDead) return;
    c->struck = true;
    perform_attack(*o, *c, quests, out);
}

// ---------------------------------------------------------------------------
// Damage (`CCharacter::applyDamage`, `modifyHP`, `CMonster::reactToDamage`)
// ---------------------------------------------------------------------------

bool LevelScript::damage_unit(int64_t id, float amount, int64_t attacker, QuestState& quests,
                              std::vector<ScriptEvent>& out, LevelScript* attacker_machine,
                              const KillingBlow* blow, bool struck, bool shown) {
    Object* o = find(id);
    Character* c = character(id);
    if (o == nullptr || c == nullptr) return false;
    if (amount == 0.0f || o->ai == ai_state::kDying || o->ai == ai_state::kDead) return false;
    if (dead_.count(id) != 0) return false;
    const UnitStats& stats = stats_for(o->unit);
    // `applyDamage`: nothing happens to a
    // unit that is not damageable.
    if (!stats.damageable) return false;
    load_own_effects(*c);
    // a glancing blow says so; any other, not the unit's own, goes
    // onto the number over it (`+0x1ec`).
    if (shown && !(blow != nullptr && blow->glancing) &&
        !(attacker == id && (attacker_machine == nullptr || attacker_machine == this))) {
        c->damage_shown.pending = amount + c->damage_shown.pending;
    }

    // a struck unit whose lockout ran out casts `CAST SKILL ON STRUCK` procs at the
    // attacker (`executeProcs(0x51, attacker)`) before `modifyHP`; the cast can add
    // to this machine, so the unit is looked up again.
    if (struck && !(0.0f < c->proc_lockout) && unit_skills_.procs) {
        LevelScript* target_machine = nullptr;
        int64_t target_node = 0;
        bool has_target = false;
        if (attacker != kNoAttacker) {
            const UnitRef by = attacker == 0                 ? UnitRef{}
                               : attacker_machine != nullptr ? UnitRef{attacker_machine, attacker}
                                                             : resolve(attacker);
            if (attacker == 0 || by.machine != nullptr) {
                target_machine = attacker == 0 ? nullptr : by.machine;
                target_node = attacker == 0 ? 0 : by.node;
                has_target = true;
            }
        }
        unit_skills_.procs(*this, id, 0x51, target_machine, target_node, has_target);
        o = find(id);
        c = character(id);
        if (o == nullptr || c == nullptr) return false;
        if (o->ai == ai_state::kDying || o->ai == ai_state::kDead || dead_.count(id) != 0) {
            return false;
        }
    }

    // `modifyHP(-amount)`. A mimic holding
    // `MIMICIDLE` is not touched by it: the blow costs it nothing,
    // and what follows - `reactToDamage` - is what wakes it.
    if (c->mimic_idle) {
        // with an attacker and a blow that struck, the unit's friends
        // near it, then the unit itself (its HP untouched).
        if (attacker != kNoAttacker && struck) {
            react_to_area_damage(attacker, attacker_machine, this, id, feet_of(*c),
                                 c->motion.radius, quests, out);
            o = find(id);
            c = character(id);
            if (o == nullptr || c == nullptr) return false;
            const UnitRef by = attacker == 0                 ? UnitRef{}
                               : attacker_machine != nullptr ? UnitRef{attacker_machine, attacker}
                                                             : resolve(attacker);
            react_to_damage(*o, *c, by, true, quests, out);
        }
        if (!c->has_target) c->ai_flags.add(AIFlag::Aware, 3.0f);
        return false;
    }
    const float before = c->combat.hp;
    const float delta = -amount;
    // `modifyHP` broadcasts unless in god mode (`CCharacter::getIsInGodMode`
    // false for a monster) or the player; `broadcastHPThreshholdEvents` needs a flag
    // only `CMonsterDescriptor::CreateObject` and `CItemDescriptor::CreateObject`
    // set: placed, never spawned.
    if (delta < 0.0f && !o->spawned) {
        std::vector<uint32_t> fired;
        broadcast_unit_hp_events(static_cast<float>(c->combat.base_max_hp), before, c->min_hp,
                                 delta, o->fire_all_hp_events, fired);
        for (uint32_t ev : fired) {
            broadcast(o->id, ev, quests, out);
        }
    }
    float hp = before - amount;
    const float max = static_cast<float>(max_hp(c->combat));
    if (max < hp) hp = max;
    if ((stats.invincible_flag || o->invulnerable) && hp < 1.0f) hp = 1.0f;
    if (hp < 0.0f) {
        hp = 0.0f;
        // `modifyHP`: under 0, `forceDisplayOfDamageAbsorbed`.
        display_unit_damage(*o, *c, kForcedDamageDisplay, out);
    }
    c->combat.hp = hp;
    if (hp < c->min_hp) c->min_hp = hp;

    if (c->combat.hp <= 0.0f) {
        // `die(attacker, direction, force, critical || damage >=
        // maxHP() * 1.75)`.
        const bool burst = (blow != nullptr && blow->critical) || max * 1.75f <= amount;
        kill_character(*o, *c, quests, out, blow, burst, attacker == 0);
        // then, with an attacker and a blow that struck, its friends
        // near it react (`CLevel::reactToAreaDamage`); itself, with no HP, not.
        if (attacker != kNoAttacker && struck) {
            react_to_area_damage(attacker, attacker_machine, this, id, feet_of(*c),
                                 c->motion.radius, quests, out);
            o = find(id);
            c = character(id);
            if (o == nullptr || c == nullptr) return true;
        }
        // The death branch (`die`, then the awards) rejoins at the
        // `AWARE` add below, so a unit killed while untargeted still
        // spreads `AWARE` to its formation.
        if (!c->has_target) c->ai_flags.add(AIFlag::Aware, 3.0f);
        return true;
    }
    // No attacker (a Damage Shape's): skips `reactToDamage`; nor is
    // it made for a blow that did not strike (the 8th argument): its
    // friends near it first (`CLevel::reactToAreaDamage`), then the unit.
    if (attacker != kNoAttacker && struck) {
        react_to_area_damage(attacker, attacker_machine, this, id, feet_of(*c), c->motion.radius,
                             quests, out);
        o = find(id);
        c = character(id);
        if (o == nullptr || c == nullptr) return false;
        const UnitRef by = attacker == 0                 ? UnitRef{}
                           : attacker_machine != nullptr ? UnitRef{attacker_machine, attacker}
                                                         : resolve(attacker);
        react_to_damage(*o, *c, by, true, quests, out);
    }
    // `CCharacter::applyDamage`,
    // after `reactToDamage`: an untargeted unit (+0x340
    // == 0) gets `AWARE` for 3.0 seconds.
    if (!c->has_target) {
        c->ai_flags.add(AIFlag::Aware, 3.0f);
    }
    return false;
}

// `CCharacter::getBravery`: `+0x5c4`, which `makeChampion`
// sets to 1.
static float character_bravery(const UnitStats& stats, const UnitCombat& combat,
                               const AIFlags& flags) {
    float bravery = combat.champion ? 1.0f : stats.bravery;
    if (flags.has(AIFlag::Frighten)) {
        bravery *= 0.5f;
    }
    return bravery;
}

// `CLevel::reactToAreaDamage(CCharacter*, CCharacter*)`,
// `CCharacter::inDamageReactRange` inlined; the player is passed over
// (empty `reactToDamage`). DIVERGENCE: the machines' lists in turn, so the
// friends' order (and volatile rolls) can differ.
void LevelScript::react_to_area_damage(int64_t attacker, LevelScript* attacker_machine,
                                       LevelScript* victim_machine, int64_t victim,
                                       const Vec3& victim_at, float victim_radius,
                                       QuestState& quests, std::vector<ScriptEvent>& out) {
    const UnitRef struck = victim_machine == nullptr ? UnitRef{} : UnitRef{victim_machine, victim};
    const UnitRef by = attacker == 0                 ? UnitRef{}
                       : attacker_machine != nullptr ? UnitRef{attacker_machine, attacker}
                                                     : resolve(attacker);
    for (LevelScript* machine : level_machines()) {
        std::vector<int64_t> ids;
        for (const Object& o : machine->objects_) {
            if (machine->character(o.id) != nullptr) ids.push_back(o.id);
        }
        for (int64_t id : ids) {
            if (machine == victim_machine && id == victim) continue;
            Object* o = machine->find(id);
            Character* c = machine->character(id);
            if (o == nullptr || c == nullptr) continue;
            // `+0x278` 5 and 6: dying and dead.
            if (o->ai == ai_state::kDying || o->ai == ai_state::kDead) continue;
            if (machine->dead_.count(id) != 0) continue;
            if (!machine->is_friend(*o, struck)) continue;
            // across the ground, `fmadds` for the sum of the
            // squares, less both radii (`+0x15c`), against `+0x2a8`.
            const Vec3 here = machine->feet_of(*c);
            const float dx = victim_at.x - here.x;
            const float dz = victim_at.z - here.z;
            const float apart =
                std::sqrt(std::fma(dz, dz, dx * dx)) - c->motion.radius - victim_radius;
            if (machine->stats_for(o->unit).damage_react_radius < apart) continue;
            machine->react_to_damage(*o, *c, by, false, quests, sink_for(machine, out));
        }
    }
}

// `CMonster::reactToDamage(CCharacter*, bool)`.
//
void LevelScript::react_to_damage(Object& o, Character& c, const UnitRef& attacker, bool direct,
                                  QuestState& quests, std::vector<ScriptEvent>& out) {
    if (o.ai == ai_state::kDying || o.ai == ai_state::kDead) return;
    // The player's camera mode (`+0x390c`) - a camera controller holding it.
    if (level_camera_taken()) return;
    const int32_t state = o.ai;
    if (state == 0x10 || state == 0x29 || state == 0x28 || state == 0x23 || state == 0x2a) return;
    if (attacker == UnitRef{this, o.id}) return;
    if (state == ai_state::kFlee) return;
    if (pet_near_death(o, c)) return;
    // `KSETTINGS_PLAYER_UNTARGETABLE` 1 ends it here.
    if (player_untargetable_) return;
    c.think = kAiIntervals[c.think_index];
    // a direct blow on a unit that `ISA 0xa7` removes `MIMICIDLE`.
    if (direct && c.mimic_idle) {
        c.mimic_idle = false;
        TL_LOG_INFO("script: %s is disturbed and wakes", o.unit.c_str());
    }

    const UnitStats& stats = stats_for(o.unit);
    const float bravery = character_bravery(stats, c.combat, c.ai_flags);
    const float max = static_cast<float>(max_hp(c.combat));
    const float hp = std::min(std::floor(c.combat.hp), max);
    // Stance +0x710 (1 from the constructor): a Passive pet struck directly
    // always runs.
    const bool brave = !(0.0f < max) || !direct ||
                       (c.stance != 0 && (1.0f - bravery <= hp / max ||
                                          0x31 < volatile_random().integer_between(0, 100)));
    if (brave) {
        if (!target_alive(attacker) || !is_enemy(o, attacker)) return;
        if (!attacker.player()) {
            const Object* them = target_object(attacker);
            if (them == nullptr || !them->targetable) return;
        }
        const int roll = volatile_random().integer_between(0, 100);
        if (roll < 10 && (!c.has_target || attacker != c.target) && o.ai != ai_state::kFlee) {
            set_target(o, c, false, UnitRef{}, out);
            set_ai_state(o, ai_state::kIdle, quests, out);
        }
        if (c.stance != 0 &&
            (o.ai == ai_state::kIdle || o.ai == ai_state::kNone || o.ai == ai_state::kFidget)) {
            stop_pathing(c);
            set_ai_state(o, ai_state::kHunt, quests, out);
            set_target(o, c, true, attacker, out);
        }
        return;
    }
    // Afraid: run 3 to 5 away from the attacker.
    set_ai_state(o, ai_state::kFlee, quests, out);
    Vec3 them;
    if (!target_at(attacker, them)) return;
    const Vec3 here = c.motion.position;
    float dx = here.x - them.x;
    float dy = here.y - them.y;
    float dz = here.z - them.z;
    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (1e-08f < length) {
        dx = dx / length;
        dz = dz / length;
    }
    const float away = volatile_random().between(3.0f, 5.0f);
    set_destination(c, stats, away * dx + here.x, away * dz + here.z);
}

std::string LevelScript::living_unit(int64_t node) const {
    const Character* c = character(node);
    if (c == nullptr || dead_.count(node) != 0) return std::string();
    return c->unit;
}

// `CCharacter::applyEffectOnUnit(CCharacter*, CBaseUnit*, CEffect*)`,
// case 0x4f (`FLEE EFFECT`): not the player, nor a unit dying, dead or
// in state 0x28.
bool LevelScript::apply_flee_effect(int64_t node, float chance, float duration, const Vec3* source,
                                    QuestState& quests, std::vector<ScriptEvent>& out) {
    Object* o = find(node);
    Character* found = character(node);
    if (o == nullptr || found == nullptr || dead_.count(node) != 0) return false;
    Character& c = *found;
    if (o->ai == ai_state::kDying || o->ai == ai_state::kDead || o->ai == 0x28) return false;
    if (chance < static_cast<float>(volatile_random().integer_between(0, 100))) return false;
    const float roll = static_cast<float>(volatile_random().integer_between(0, 100));
    // `getEffectValue(0x79 FLEE RESIST, 7)`, off the unit's own manager.
    const float resist = c.effects ? c.effects->effect_value(0x79) : 0.0f;
    if (!(resist < roll)) return false;
    bool turned = false;
    // Already fleeing and only just set on its way (`+0x278 <= 0.5`): the copy
    // is taken and nothing else happens.
    if (!(o->ai == ai_state::kFlee && c.since_route <= 0.5f)) {
        turned = o->ai != ai_state::kFlee;
        interrupt(*o, c, true, quests, out);
        set_ai_state(*o, ai_state::kFlee, quests, out);
        if (source != nullptr) {
            // 3 to 5 away from where the effect came from, on the ground plane.
            const Vec3 here = c.motion.position;
            float dx = here.x - source->x;
            float dz = here.z - source->z;
            const float length = std::sqrt(dx * dx + 0.0f + dz * dz);
            if (1e-08f < length) {
                dx = dx / length;
                dz = dz / length;
            }
            const float away = volatile_random().between(3.0f, 5.0f);
            set_destination(c, stats_for(o->unit), away * dx + here.x, away * dz + here.z);
        }
    }
    // `copyEffect` (the shared tail).
    c.flee_effect_left = std::max(c.flee_effect_left, duration);
    return turned;
}

// ---------------------------------------------------------------------------
// The AI states
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Skills (`CMonster::selectOffensiveSkill` and its neighbours, the three skill
// states of `CCharacter`, `castSkill`)
// ---------------------------------------------------------------------------

namespace {

// PORT: a unit skill's property at its effective level.
const SkillProperty* unit_skill_property(const LevelScript::UnitSkill& s) {
    return s.known.property(s.known.effective);
}

// `CSkill::getManaCost` and `getManaCostOT`: the level's
// float, truncated to a `long`.
int64_t unit_skill_mana_cost(const LevelScript::UnitSkill& s) {
    const SkillProperty* p = unit_skill_property(s);
    return p != nullptr ? static_cast<int64_t>(p->mana_cost) : 0;
}
// PORT: a unit skill's property field.
int64_t unit_skill_mana_cost_ot(const LevelScript::UnitSkill& s) {
    const SkillProperty* p = unit_skill_property(s);
    return p != nullptr ? static_cast<int64_t>(p->mana_cost_ot) : 0;
}

// The mana alone: `floor(mana)` (`CCharacter::mana`) against both
// costs.
bool unit_skill_mana_covers(float pool, const LevelScript::UnitSkill& s) {
    const int64_t mana = static_cast<int64_t>(std::floor(pool));
    return mana >= unit_skill_mana_cost(s) && mana >= unit_skill_mana_cost_ot(s);
}

// A skill with no charges left (the charge count +0xec at 0).
bool unit_skill_spent(const LevelScript::UnitSkill* s) {
    return s != nullptr && s->known.charges == 0;
}

// The first of a unit's `known` skills, -1 with none.
int first_unit_skill(size_t known) { return known == 0 ? -1 : 0; }

// `CSkill::getTargetType`: the `TARGET_TYPE` of the property at +0x90.
int32_t unit_skill_target_type(const LevelScript::UnitSkill& s) {
    const SkillProperty* p = unit_skill_property(s);
    return p != nullptr ? p->target_type : 0;
}

// `CSkill::getRange` / `getRangeMin`.
float unit_skill_range(const LevelScript::UnitSkill& s) {
    // PORT: a unit skill's property at its effective level.
    const SkillProperty* p = unit_skill_property(s);
    return p != nullptr ? p->range : 1.0f;
}
// PORT: a unit skill's property field.
float unit_skill_range_min(const LevelScript::UnitSkill& s) {
    const SkillProperty* p = unit_skill_property(s);
    return p != nullptr ? p->range_min : 0.0f;
}

// `CSkill +0x6d` and `+0x6b`: enabled for use, and not `HIDDEN`.
// PORT: a unit skill's flags, `CSkill +0x6d` and `+0x6b`: enabled for use, and not `HIDDEN`.
bool unit_skill_enabled(const LevelScript::UnitSkill& s) {
    return s.known.enabled && !(s.known.def != nullptr && s.known.def->hidden);
}

// PORT: a unit skill's type.
bool unit_skill_type_is(const LevelScript::UnitSkill& s, SkillType type) {
    return s.known.def != nullptr && s.known.def->type == type;
}

// `CSkill::rollCastChance`: no draw at 100 or above.
bool roll_cast_chance(const LevelScript::UnitSkill& s) {
    if (99 < s.cast_chance) return true;
    return volatile_random().integer_between(0, 100) <= s.cast_chance;
}

}  // namespace

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
LevelScript::UnitCasting* LevelScript::unit_casting(int64_t node) {
    Character* c = character(node);
    return c != nullptr ? &c->casting : nullptr;
}

// PORT: the level units with skills that are alive.
std::vector<int64_t> LevelScript::casting_units() const {
    std::vector<int64_t> out;
    for (int64_t id : character_order_) {
        const Character* c = character(id);
        if (c == nullptr || c->casting.skills.empty() || dead_.count(id) != 0) continue;
        out.push_back(id);
    }
    return out;
}

bool LevelScript::unit_skill_blow(int64_t caster, LevelScript* victim_machine, int64_t victim_node,
                                  float damage_scale, float soak_scale,
                                  std::vector<ScriptEvent>& out) {
    Object* o = find(caster);
    Character* c = character(caster);
    if (o == nullptr || c == nullptr) return false;
    if (o->ai == ai_state::kDying || o->ai == ai_state::kDead) return false;
    // `rollAttack`, the defender as
    // `perform_attack` describes it.
    Defender defender;
    std::string victim = "the player";
    Vec3 at = player_at_;
    if (victim_machine == nullptr) {
        defender = player_defense_;
    } else {
        const Character* tc = victim_machine->character(victim_node);
        const Object* to = victim_machine->find(victim_node);
        if (tc == nullptr || to == nullptr || victim_machine->unit_is_dead(victim_node))
            return false;
        victim_machine->unit_defender(victim_node, defender);
        victim = to->unit;
        at = victim_machine->feet_of(*tc);
    }
    // `rollAttack`: 0 from a mimic that holds
    // `MIMICIDLE`.
    if (c->mimic_idle) return false;
    int absorbed = 0;
    defender.on_absorbed = [&absorbed] { ++absorbed; };
    const Blow blow = roll_attack(c->combat, defender, volatile_random(), c->effects.get(),
                                  damage_scale, soak_scale, master_effects(*c));
    ScriptEvent e;
    e.action = ScriptAction::Strike;
    e.roll_attack = true;
    e.node_id = o->id;
    e.name = o->name;
    e.unit = o->unit;
    e.target_node = victim_machine != nullptr ? victim_node : 0;
    e.target_machine = victim_machine;
    e.text = victim;
    e.amount = static_cast<float>(blow.damage);
    e.critical = blow.critical;
    e.blocked = blow.blocked;
    e.by_type = blow.by_type;
    e.parts = blow.parts;
    // `rollAttack`'s direction and force for `applyDamage`, as a swing's.
    e.has_direction = true;
    e.direction = blow_direction(feet_of(*c), at, blow.direction_jitter);
    e.force = blow.knockback;
    out.push_back(e);
    return true;
}

// `CBaseUnit::unitInit`: the `[SKILL]`
// blocks, read once the scene has handed the machine a way to resolve them.
void LevelScript::load_unit_skills(Character& c) {
    UnitCasting& k = c.casting;
    if (k.loaded || !unit_skills_.load) return;
    // `CBaseUnit::unitInit`: `reapplyEffects(false)` first
    // (a cast-skill effect puts its skill at the head: `activateEffect`),
    // then the `[SKILL]` blocks. INVENTED: read on the first update.
    load_own_effects(c);
    k.loaded = true;
    std::vector<UnitSkill> blocks = unit_skills_.load(c.unit);
    for (UnitSkill& block : blocks) {
        if (block.level_from_unit) {
            const uint32_t level = static_cast<uint32_t>(c.combat.level);
            block.known.invested = level;
            block.known.effective = level;
        }
        // `addSkillByName` adds a copy per block, and
        // `CSkillManager::setSkillLevel` re-levels every copy:
        // the last block of a name decides the level of all.
        const std::string key = upper(block.known.entry.name);
        for (UnitSkill& held : k.skills) {
            if (upper(held.known.entry.name) != key) continue;
            held.known.invested = block.known.invested;
            held.known.effective = block.known.effective;
        }
        k.skills.push_back(std::move(block));
    }
    activate_unit_affixes(c);
    // `CCharacter::calculateMaxMana`: the
    // level times 5, plus 20.
    k.max_mana = std::max(1, c.combat.level * 5 + 20);
    // INVENTED: the pool starts full; the store `unitInit` makes is not read.
    k.mana = static_cast<float>(k.max_mana);
}

// PORT: a unit's active skill (`CCharacter +0x2c0`'s), or null.
LevelScript::UnitSkill* LevelScript::active_unit_skill(Character& c) {
    UnitCasting& k = c.casting;
    if (k.active < 0 || k.active >= static_cast<int>(k.skills.size())) return nullptr;
    return &k.skills[static_cast<size_t>(k.active)];
}

// The tests `selectOffensiveSkill` and its neighbours open with: mana for `getManaCost`
// and `getManaCostOT` (`CCharacter::mana` floors to an integer) and
// `!getSkillIsCooling`.
bool LevelScript::unit_skill_affordable(const Character& c, const UnitSkill& s) const {
    return unit_skill_mana_covers(c.casting.mana, s) &&
           !c.casting.runner.is_cooling(s.known.entry.name);
}

// `CSkillManager::getSkillByGuid(long long)`:
// the manager's list (`+0x60`, X360 `+0x2c`) in
// order, the first skill whose GUID (`+0x150`, X360 `+0x180`) matches.
int LevelScript::unit_skill_by_guid(const Character& c, int64_t guid) const {
    for (size_t i = 0; i < c.casting.skills.size(); ++i) {
        if (c.casting.skills[i].known.guid() == guid) return static_cast<int>(i);
    }
    return -1;
}

// `CCharacter::castSkill(-1)`, `inSkillRange`,
// `skillApproachAI`: the active skill re-picked by
// GUID (`getSkillByGuid`), so a unit holding one skill twice casts its first copy.
void LevelScript::reselect_active_by_guid(Character& c) {
    const int active = c.casting.active;
    if (active < 0 || active >= static_cast<int>(c.casting.skills.size())) return;
    const int first =
        unit_skill_by_guid(c, c.casting.skills[static_cast<size_t>(active)].known.guid());
    if (first != active) c.casting.active = first;
}

// `CCharacter::setActiveSkill(CSkill*, false)`:
// `+0x398` is cleared first, so the skill
// named (when the unit holds it) is always taken.
void LevelScript::set_active_skill(Character& c, int index) {
    c.casting.active = -1;
    if (index < 0 || index >= static_cast<int>(c.casting.skills.size())) return;
    c.casting.active = index;
}

// `CMonster::selectOffensiveSkill(bool)`.
void LevelScript::select_offensive_skill(Object& o, Character& c, bool check, QuestState&,
                                         std::vector<ScriptEvent>& out) {
    UnitCasting& k = c.casting;
    const int count = static_cast<int>(k.skills.size());
    if (count <= 0) return;
    // The skill held on the way in (`pCVar11`), put back when nothing qualifies.
    const int previous = k.active;
    // `+0x780`, every choice at odds 1.
    Randomizer choices;
    set_active_skill(c, -1);
    for (int i = 0; i < count; ++i) {
        const UnitSkill& s = k.skills[static_cast<size_t>(i)];
        if (!unit_skill_affordable(c, s)) continue;
        if (!(s.property_alignment == 2 || unit_skill_type_is(s, SkillType::Offensive))) continue;
        if (!unit_skill_enabled(s)) continue;
        if (check) {
            set_active_skill(c, i);
            if (!can_cast_current_skill(o, c, false, false, out)) continue;
        }
        choices.add_choice(i, 1);
    }
    int chosen = previous;
    if (choices.choice_count() != 0) {
        // `CRandomizer::getRandom`:
        // `randomBetween`, so `g_Rand`.
        int32_t index = -1;
        const int32_t value = choices.get_random(game_random(), &index);
        chosen = index >= 0 && value < count ? value : -1;
    }
    set_active_skill(c, chosen);
}

// `CMonster::selectDefensiveSkill()`: a self,
// pets or self-and-pets target, or `SKILL_TYPE` `DEFENSIVE`, and not
// `TARGET_ALIGNMENT` `EVIL`.
void LevelScript::select_defensive_skill(Character& c) {
    UnitCasting& k = c.casting;
    const int count = static_cast<int>(k.skills.size());
    if (count <= 0) return;
    Randomizer choices;
    set_active_skill(c, -1);
    for (int i = 0; i < count; ++i) {
        const UnitSkill& s = k.skills[static_cast<size_t>(i)];
        if (!unit_skill_affordable(c, s)) continue;
        const int32_t type = unit_skill_target_type(s);
        if (type != 3 && type != 10 && type != 9 && !unit_skill_type_is(s, SkillType::Defensive)) {
            continue;
        }
        const int32_t alignment = s.known.def != nullptr ? s.known.def->target_alignment : 2;
        if (alignment == 2 || !unit_skill_enabled(s)) continue;
        choices.add_choice(i, 1);
    }
    // Nothing qualifying leaves `+0x398` cleared.
    if (choices.choice_count() == 0) return;
    int32_t index = -1;
    const int32_t value = choices.get_random(game_random(), &index);
    set_active_skill(c, index >= 0 && value < count ? value : -1);
}

// `CMonster::hasOffensiveSkill()`: `selectOffensiveSkill`'s first
// tests without the randomizer, and on the property's +0x50 alone.
bool LevelScript::has_offensive_skill(const Character& c) const {
    for (const UnitSkill& s : c.casting.skills) {
        if (!unit_skill_affordable(c, s)) continue;
        if (s.property_alignment == 2 && unit_skill_enabled(s)) return true;
    }
    return false;
}

// `CMonster::hasDefensiveSkill()`:
// `SKILL_TYPE` `DEFENSIVE` or `CHARM` counts here, and the alignment is not
// tested.
bool LevelScript::has_defensive_skill(const Character& c) const {
    for (const UnitSkill& s : c.casting.skills) {
        if (!unit_skill_affordable(c, s)) continue;
        const int32_t type = unit_skill_target_type(s);
        if (type != 3 && type != 10 && type != 9 && !unit_skill_type_is(s, SkillType::Defensive) &&
            !unit_skill_type_is(s, SkillType::Charm)) {
            continue;
        }
        if (unit_skill_enabled(s)) return true;
    }
    return false;
}

// `CCharacter::inSkillRange(long long)`: 0 in range, 1
// nearer than `RANGEMIN` (always tested on the console), 2 out of range or nothing.
int LevelScript::in_skill_range(Character& c) {
    if (!c.has_target) return 2;
    reselect_active_by_guid(c);
    const int active = c.casting.active;
    if (active < 0 || active >= static_cast<int>(c.casting.skills.size())) return 2;
    const UnitSkill& s = c.casting.skills[static_cast<size_t>(active)];
    const int32_t type = unit_skill_target_type(s);
    if (type == 3 || type == 10 || type == 9) return 0;
    Vec3 at;
    if (!target_at(c.target, at)) return 2;
    // the ground distance (`fmuls`, `fmadds`), less
    // both radii (`+0x15c`), each floored at 0.
    const float them = std::max(0.0f, target_radius(c.target));
    const float me = std::max(0.0f, c.motion.radius);
    const float gap = (distance_xz_fused(feet_of(c), at) - them) - me;
    if (unit_skill_range(s) < gap) return 2;
    return gap < unit_skill_range_min(s) ? 1 : 0;
}

// `CCharacter::canCastCurrentSkill(CBaseUnit*, bool, bool)`
// on the current target. `skip_range` reaches `requirementsMet`; `skip_chance`
// skips `rollCastChance`.
bool LevelScript::can_cast_current_skill(Object& o, Character& c, bool skip_range, bool skip_chance,
                                         std::vector<ScriptEvent>& out) {
    UnitSkill* s = active_unit_skill(c);
    // The skill manager (`+0x1c8`), vtable +0x48 (`CSceneNodeObject::getEnabled`)
    // and the active skill (`+0x398`).
    if (s == nullptr || !c.casting.loaded) return false;
    if (!o.enabled) return false;
    if (!skip_chance && !roll_cast_chance(*s)) return false;
    if (!unit_skill_affordable(c, *s)) return false;
    // `getSkillIsCooling`, before any target
    // is looked for.
    if (c.casting.runner.is_cooling(s->known.entry.name)) return false;
    const int32_t type = unit_skill_target_type(*s);
    UnitRef aim = c.target;
    if (c.has_target && type != 3 && type != 10 && type != 1 && type != 0xb && type != 6 &&
        type != 5) {
        int32_t alignment = target_alignment_of(o, c);
        // A `TARGET` skill aimed at `GOOD` looks among the `EVIL`.
        const int32_t aimed = s->known.def != nullptr ? s->known.def->target_alignment : 2;
        if (type == 2 && aimed == 1 && alignment == 6) alignment = 2;
        if (!unit_skill_target_valid(o, c, *s, c.target)) {
            // `findCharacterWithinView(frame, alignment, 0, FINDTARGETANGLE, (fma(REACH_BONUS,
            // scale, RANGE) + 0.2) * RANGE_MULTIPLIER + 1, nearest, this, skill)`,
            // turned along the final target direction; then `setTarget`.
            const SkillProperty* property = unit_skill_property(*s);
            const float angle = property != nullptr ? property->find_target_angle : 0.0f;
            const UnitStats& stats = stats_for(o.unit);
            const float reach = (std::fma(stats.reach, c.scale, unit_skill_range(*s)) + 0.2f) *
                                    stats.range_multiplier +
                                1.0f;
            UnitRef found;
            if (!find_within_view(skill_frame(c), o, alignment, 0.0f, angle, reach,
                                  /*nearest=*/true, s, found)) {
                return false;
            }
            set_target(o, c, true, found, out);
            // What is asked below is the unit found (`r12`), whether or not
            // `setTarget` took it.
            aim = found;
        }
    }
    // `CSkillManager::getSkillCanBeExecuted(skill, this, 3, +0x1c0, target, skip_range)`:
    // not cooling, and `requirementsMet`. A position skill is
    // measured to the final target position, not the target.
    if (!unit_skills_.caster) return false;
    LevelScript* machine = c.has_target ? aim.machine : nullptr;
    const int64_t node = c.has_target ? (aim.player() ? -1 : aim.node) : 0;
    SkillCaster caster = unit_skills_.caster(*this, o.id, machine, node, c.has_target);
    caster.target_position = c.destination;
    caster.skip_range = skip_range;
    return c.casting.runner.can_be_executed(s->known, s->known.effective, caster);
}

// `CCharacter::castSkill(long long)`, the
// parts before `performSkill`; `performSkill` itself, the mana and the
// animation are the scene's (`UnitSkills::cast`).
bool LevelScript::cast_skill(Object& o, Character& c, QuestState& quests,
                             std::vector<ScriptEvent>& out) {
    // Attacking (an attack in use and its timer above 0 - above
    // 0.2 only for the player), or a cast still holding the unit: true, and
    // nothing done.
    if (c.has_attack && 0.0f < c.swing_left) return true;
    UnitSkill* s = active_unit_skill(c);
    if (s != nullptr) {
        const SkillRunState* state = c.casting.runner.state(s->known.entry.name);
        const SkillAnimations animations = state != nullptr ? state->animations : SkillAnimations{};
        if (still_casting(c.casting.cast, animations,
                          c.casting.runner.is_running(s->known.entry.name))) {
            return true;
        }
    }
    reselect_active_by_guid(c);
    s = active_unit_skill(c);
    if (s == nullptr) return false;
    // the mana alone; a skill cooling goes on to
    // `getSkillCanBeExecuted`, past the position block that lets the target go.
    // INVENTED: out of mana, sample 0x21 is not played for a monster.
    if (!unit_skill_mana_covers(c.casting.mana, *s)) return false;
    // A unit-targeted skill: `TARGET_ALIGNMENT` `EVIL` lets a friend go, `GOOD`
    // an enemy - and looks among the `EVIL` (`getTargetAlignment`'s 6 made 2).
    const int32_t alignment = s->known.def != nullptr ? s->known.def->target_alignment : 2;
    int32_t look_among = target_alignment_of(o, c);
    bool search = !c.has_target;
    if (unit_skill_target_type(*s) == 2 && c.has_target) {
        if (alignment == 2) {
            if (is_friend(o, c.target)) set_target(o, c, false, UnitRef{}, out);
            search = !c.has_target;
        } else if (alignment == 1) {
            if (look_among == 6) look_among = 2;
            if (is_enemy(o, c.target)) set_target(o, c, false, UnitRef{}, out);
            search = !c.has_target;
        }
    }
    // No target, no target item, `FINDTARGETANGLE` above 0: `findCharacterWithinView`
    // with the reach-scaled range, and `setTarget` on what it found.
    // INVENTED: with nothing found, `findItemWithinView` is not asked for a
    // monster, and a position skill's walk to a pathable point is not run.
    if (search) {
        const SkillProperty* property = unit_skill_property(*s);
        const float angle = property != nullptr ? property->find_target_angle : 0.0f;
        if (0.0f < angle) {
            // the same search as
            // `canCastCurrentSkill`'s, along the final target direction.
            const UnitStats& stats = stats_for(o.unit);
            const float reach = (std::fma(stats.reach, c.scale, unit_skill_range(*s)) + 0.2f) *
                                    stats.range_multiplier +
                                1.0f;
            UnitRef found;
            if (find_within_view(skill_frame(c), o, look_among, 0.0f, angle, reach,
                                 /*nearest=*/true, s, found)) {
                set_target(o, c, true, found, out);
                TL_LOG_INFO("script: %s's '%s' finds %s in view%s", o.unit.c_str(),
                            s->known.entry.name.c_str(),
                            found.player() ? "the player" : target_unit(found).c_str(),
                            target_alive(found) ? "" : " (a body)");
            }
        }
    }
    // a skill aimed at a place (`POSITION` 1, 0xb,
    // `POSITIONRANDOM` 5, `POSITIONFLEE` 6), not for the player. Its point: the target's feet,
    // else `RANGE` ahead; `getPositionForSkill` makes the final target
    // position; the target is let go; the point is brought within `RANGE` at the unit's height.
    {
        const int32_t type = unit_skill_target_type(*s);
        const SkillProperty* property = unit_skill_property(*s);
        if (property != nullptr && (type == 1 || type == 0xb || type == 5 || type == 6)) {
            const float range = property->range;
            SkillPositionUnit owner;
            owner.centre = c.motion.position;
            owner.radius = c.motion.radius;
            owner.forward = {std::sin(c.facing), 0.0f, std::cos(c.facing)};
            SkillPositionUnit aim;
            bool aimed = false;
            Vec3 point;
            if (c.has_target && target_centre_at(c.target, aim.centre)) {
                // the centre, less the radius (+0x15c)
                // times -1 added to the height.
                aimed = true;
                aim.radius = target_radius(c.target);
                if (const Character* them = target_character(c.target)) {
                    aim.forward = {std::sin(them->facing), 0.0f, std::cos(them->facing)};
                }
                point = {aim.centre.x, aim.centre.y + aim.radius * -1.0f, aim.centre.z};
            } else {
                // the forward (vtable +0x98) times
                // `RANGE`, added to the centre, the height less the radius.
                const float fx = owner.forward.x * range;
                const float fy = owner.forward.y * range;
                const float fz = owner.forward.z * range;
                point = {owner.centre.x + fx, (owner.centre.y + owner.radius * -1.0f) + fy,
                         owner.centre.z + fz};
            }
            c.casting.skill_point = point;
            if (const std::optional<Vec3> at = position_for_skill(
                    *property, point, &owner, aimed ? &aim : nullptr, volatile_random())) {
                c.destination = *at;
            }
            // the target let go (and the target item, which a
            // level unit never has).
            set_target(o, c, false, UnitRef{}, out);
            // on the ground from the centre, and no
            // further than `RANGE` (`fmuls z*z`, `fmadds 0*0 + that`, `fmadds
            // x*x + that`).
            Vec3 off{c.destination.x - owner.centre.x, 0.0f, c.destination.z - owner.centre.z};
            const float length =
                std::sqrt(std::fma(off.x, off.x, std::fma(0.0f, 0.0f, off.z * off.z)));
            if (range < length) {
                off = ogre_normalised(off);
                off = {off.x * range, range * off.y, range * off.z};
            }
            // back to the world from the centre.
            c.destination = {off.x + owner.centre.x, owner.centre.y + off.y,
                             off.z + owner.centre.z};
            // INVENTED: with `REQUIRES_PATHABLE` the console steps the point back until `canPathTo`
            // takes it and snaps it to the grid; the
            // port leaves it where the limit put it.
        }
    }
    // `FIREATTARGET` with a target:
    // `setFinalTargetPosition` at the target's centre.
    if (const SkillProperty* property = unit_skill_property(*s)) {
        Vec3 there;
        if (property->fire_at_target && c.has_target && target_centre_at(c.target, there)) {
            set_final_target_position(c, there.x, there.z);
        }
    }
    if (!unit_skills_.caster || !unit_skills_.cast) return false;
    LevelScript* machine = c.has_target ? c.target.machine : nullptr;
    const int64_t node = c.has_target ? (c.target.player() ? -1 : c.target.node) : 0;
    SkillCaster caster = unit_skills_.caster(*this, o.id, machine, node, c.has_target);
    // `getSkillCanBeExecuted(skill, this, 3, +0x1c0, target, 0)`.
    caster.target_position = c.destination;
    if (!c.casting.runner.can_be_executed(s->known, s->known.effective, caster)) return false;
    const std::string name = s->known.entry.name;
    if (!unit_skills_.cast(*this, o.id, machine, node, c.has_target)) return false;
    // `startSkill`: the level property's
    // +0x50 takes `TARGET_ALIGNMENT`.
    if (UnitSkill* again = active_unit_skill(c)) {
        if (again->known.entry.name == name) again->property_alignment = alignment;
    }
    // An animation was queued: `+0x267`, and `setAIState(0xe)`. The gait is
    // sent again once it has run.
    if (c.casting.cast.casting) {
        c.gait.clear();
        set_ai_state(o, ai_state::kPerformingSkill, quests, out);
    }
    // the fidget clock (+0x214) held to 5, an
    // animation or none (comes here without one).
    if (c.fidget_wait > 5.0f) c.fidget_wait = 5.0f;
    // a skill with no charges left hands the unit its
    // first known skill. INVENTED: the skill set aside to become current
    // (+0x2c8) is not kept by the port.
    if (unit_skill_spent(active_unit_skill(c)))
        set_active_skill(c, first_unit_skill(c.casting.skills.size()));
    return true;
}

// `CCharacter::availableForCommand`: not a pet near death,
// not dying or dead, not on its way to town or in it (0x29, 0x2a).
bool LevelScript::available_for_command(const UnitRef& who) const {
    if (!who.player()) {
        const Object* t = target_object(who);
        const Character* tc = target_character(who);
        if (t == nullptr || tc == nullptr) return false;
        if (who.machine->pet_near_death(*t, *tc)) return false;
    }
    const int32_t state = target_state(who);
    return state != ai_state::kDying && state != ai_state::kDead && state != ai_state::kInTown &&
           state != ai_state::kGoingToTown;
}

// `CCharacter::turnTowardPosition` toward the target:
// its ground point the final target position, the turn along the normalised direction to
// its centre (the products fused as the console does).
void LevelScript::turn_toward_target(Object& o, Character& c, const UnitStats& stats,
                                     const Vec3& at, float delta) {
    c.destination = {at.x, 0.0f, at.z};
    Vec3 centre = at;
    target_centre_at(c.target, centre);
    float dx = centre.x - c.motion.position.x;
    float dy = centre.y - c.motion.position.y;
    float dz = centre.z - c.motion.position.z;
    const float length = std::sqrt(std::fma(dy, dy, std::fma(dz, dz, dx * dx)));
    if (1e-08 < static_cast<double>(length)) {
        const float inverse = 1.0f / length;
        dx = dx * inverse;
        dy = inverse * dy;
        dz = dz * inverse;
    }
    // `turnToward`: not dying, dead, in 0xc or 0x28, and a
    // turn rate (+0x218): the final target direction (+0x1cc) takes
    // the direction, and the unit turns.
    if (o.ai != ai_state::kDying && o.ai != ai_state::kDead && o.ai != 0xc && o.ai != 0x28 &&
        stats.turn_rate != 0.0f) {
        c.destination_direction = {dx, dy, dz};
        c.facing = turn_toward(c.facing, dx, dz, stats.turn_rate, delta, nullptr, turn_factor(c));
    }
}

// `CCharacter::skillAI(float, CLevel&)`,
// state 0xd. The target item (+0x28c) and the hold position (+0x1fe) the
// port's level units never have.
void LevelScript::skill_ai(Object& o, Character& c, float delta, QuestState& quests,
                           std::vector<ScriptEvent>& out) {
    const auto drop = [&](int32_t state) {
        set_ai_state(o, state, quests, out);
        set_target(o, c, false, UnitRef{}, out);
    };
    // no target, or one dying or dead, either of
    // the two a pet near death, or a target not `availableForCommand`: state 0.
    if (!c.has_target || !target_alive(c.target) || pet_near_death(o, c) ||
        !available_for_command(c.target)) {
        drop(ai_state::kNone);
        return;
    }
    // no skill held, state 0 too.
    if (active_unit_skill(c) == nullptr) {
        drop(ai_state::kNone);
        return;
    }
    Vec3 at;
    target_at(c.target, at);
    const UnitStats& stats = stats_for(o.unit);
    // out of the follow radius (+0x2a4), centre to
    // centre on the ground (`fmuls dx*dx`, `fmadds dz*dz + that`), a unit with
    // no master (+0x5a4) gives up: stopped, idle, no target.
    if (!(distance_xz_fused(feet_of(c), at) < stats.follow_radius) && !has_master(c)) {
        stop_pathing(c);
        drop(ai_state::kIdle);
        return;
    }
    // too far (`inSkillRange` 1) gives up as well.
    const int range = in_skill_range(c);
    if (range == 1) {
        stop_pathing(c);
        drop(ai_state::kIdle);
        return;
    }
    // a `SELF` (3), `SELFANDPETS` (10) or `PETS` (9)
    // skill needs no line of sight.
    const int32_t type = unit_skill_target_type(*active_unit_skill(c));
    const bool wants_sight = type != 3 && type != 10 && type != 9;
    if (range != 0 || (wants_sight && !valid_line_of_sight(c))) {
        // a cast still holding the unit (`performingSkillLoose`)
        // waits.
        if (c.casting.active >= 0 && skill_holds_character(c.casting.cast)) return;
        // one that can still be cast is walked into
        // range (0xf); else: let go of, idle, and `updateAI(0, true)`.
        if (can_cast_current_skill(o, c, true, true, out)) {
            set_ai_state(o, ai_state::kSkillApproach, quests, out);
            return;
        }
        c.casting.active = -1;
        set_ai_state(o, ai_state::kIdle, quests, out);
        monster_update_ai(o, c, 0.0f, quests, out, true);
        return;
    }
    // stopped, turned toward the target, and cast once facing it;
    // a cast that does not start ends in idle.
    stop_pathing(c);
    turn_toward_target(o, c, stats, at, delta);
    if (facing_target(c.facing, feet_of(c), at, stats.attack_angle, weapon_arc(c)) &&
        !cast_skill(o, c, quests, out)) {
        set_ai_state(o, ai_state::kIdle, quests, out);
    }
    // a unit with a master, unless `ISA 0xa9`, more
    // than 35 from it (centre to centre, `fmuls dx*dx`, `fmadds dz*dz + that`;
    // a NaN too) lets the target go and idles.
    const bool no_follow =
        unit_types_ != nullptr && unit_types_->isa(stats.unittype, unit_type::kPetNoFollow);
    MasterView master;
    if (!no_follow && master_view(c, master)) {
        const float dx = master.centre.x - c.motion.position.x;
        const float dz = master.centre.z - c.motion.position.z;
        if (!(std::sqrt(std::fma(dz, dz, dx * dx)) <= 35.0f)) {
            set_target(o, c, false, UnitRef{}, out);
            set_ai_state(o, ai_state::kIdle, quests, out);
        }
    }
}

// `CCharacter::performingSkillAI(float)`,
// state 0xe.
void LevelScript::performing_skill_ai(Object& o, Character& c, float delta, QuestState& quests,
                                      std::vector<ScriptEvent>& out) {
    // `attemptStopOfActiveSkill`; when it lets go, `notifyAISkillComplete`
    // (vtable +0x408, `selectOffensiveSkill(false)` for a monster),
    // `setAIState(2)` and `updateAI(dt, true)`.
    const auto release = [&]() {
        UnitSkill* held = active_unit_skill(c);
        const SkillRunState* state =
            held != nullptr ? c.casting.runner.state(held->known.entry.name) : nullptr;
        const SkillProperty* property = held != nullptr ? unit_skill_property(*held) : nullptr;
        // `CSkill::getCanStop`: the minimum time has run.
        const bool can_stop =
            state == nullptr || property == nullptr || property->minimum_time <= state->elapsed;
        const SkillStop stop = attempt_stop_of_active_skill(
            c.casting.cast, state != nullptr ? &state->animations : nullptr, can_stop);
        if (stop == SkillStop::None) return false;
        if (stop == SkillStop::StopByName && held != nullptr && unit_skills_.stop_by_name) {
            unit_skills_.stop_by_name(*this, o.id, held->known.entry.name);
        }
        // `stopAllSkills(false, false, false)`.
        if (stop == SkillStop::StopAll && unit_skills_.stop_all) {
            unit_skills_.stop_all(*this, o.id, /*force=*/false);
        }
        select_offensive_skill(o, c, false, quests, out);
        set_ai_state(o, ai_state::kIdle, quests, out);
        monster_update_ai(o, c, delta, quests, out, true);
        return true;
    };
    UnitSkill* s = active_unit_skill(c);
    bool holding = false;
    if (s != nullptr) {
        const SkillRunState* state = c.casting.runner.state(s->known.entry.name);
        const bool one_shot = state == nullptr || state->animations.loop_into == -1;
        holding = (one_shot || c.casting.runner.is_running(s->known.entry.name)) &&
                  (c.casting.cast.casting || 0.0f < c.casting.cast.animation_left) &&
                  0.0f < c.combat.hp;
    }
    if (!holding && release()) return;
    s = active_unit_skill(c);
    if (s == nullptr) return;
    if (0.0f < c.casting.cast.animation_left && !c.casting.cast.attack_ended) return;
    if (unit_skill_target_type(*s) != 2) return;
    // A unit-targeted skill held past its swing keeps going while its target is
    // in range and in sight and alive; otherwise it is let go.
    if (c.has_target && valid_line_of_sight(c)) {
        Vec3 at;
        if (target_at(c.target, at)) {
            const float gap = distance_xz(feet_of(c), at);
            const SkillRunState* state = c.casting.runner.state(s->known.entry.name);
            const bool one_shot = state == nullptr || state->animations.loop_into == -1;
            if (unit_skill_range_min(*s) <= gap && gap <= unit_skill_range(*s) &&
                (one_shot || c.casting.runner.is_running(s->known.entry.name)) &&
                target_alive(c.target)) {
                return;
            }
        }
    }
    release();
}

// `CCharacter::skillApproachAI(float, CLevel&, long long)`,
// state 0xf: walk into range, then 0xd. The held skill is its own guid's copy.
void LevelScript::skill_approach_ai(Object& o, Character& c, float delta, QuestState& quests,
                                    std::vector<ScriptEvent>& out) {
    const auto drop = [&]() {
        set_ai_state(o, ai_state::kNone, quests, out);
        set_target(o, c, false, UnitRef{}, out);
    };
    // no target, one dying or dead, or either a
    // pet near death: a friend. State 0, no target.
    if (!c.has_target || !target_alive(c.target) || pet_near_death(o, c) ||
        (!c.target.player() && c.target.machine != nullptr &&
         c.target.machine->unit_near_death(c.target.node)) ||
        is_friend(o, c.target)) {
        drop();
        return;
    }
    if (active_unit_skill(c) == nullptr) return;
    // one that cannot be cast idles and thinks again
    // at once, `updateAI(0, true)` - the skill still held.
    if (!can_cast_current_skill(o, c, true, true, out)) {
        set_ai_state(o, ai_state::kIdle, quests, out);
        monster_update_ai(o, c, 0.0f, quests, out, true);
        return;
    }
    Vec3 at;
    target_at(c.target, at);
    const UnitStats& stats = stats_for(o.unit);
    // centre to centre on the ground (`fmuls dx*dx`,
    // `fmadds dz*dz + that`); running from the near-walk radius (+0x298) on.
    const float d = distance_xz_fused(feet_of(c), at);
    c.run = d >= stats.near_walk_radius;
    // out of the follow radius (+0x2a4) a unit whose
    // master is not the player gives up: stopped, state 0, no target.
    if (!c.master_player && !(d < stats.follow_radius)) {
        stop_pathing(c);
        drop();
        return;
    }
    // a `SELF` (3), `SELFANDPETS` (10) or `PETS` (9)
    // skill needs no line of sight.
    const int32_t type = unit_skill_target_type(*active_unit_skill(c));
    const bool sight = (type == 3 || type == 10 || type == 9) || valid_line_of_sight(c);
    // too far lets the skill go: stopped, idle, no
    // target.
    const int range = in_skill_range(c);
    if (range == 1) {
        c.casting.active = -1;
        stop_pathing(c);
        set_ai_state(o, ai_state::kIdle, quests, out);
        set_target(o, c, false, UnitRef{}, out);
        return;
    }
    // out of range or sight, a second since the last path: the
    // target is followed again (from a standstill, or once 4 from the final target position)
    // unless the `CANCEL_CHANCE` roll lets the skill go. Not on a path after it: state 0.
    if (!(range == 0 && sight) && 1.0f < c.since_route) {
        const auto cancelled = [&]() {
            const UnitSkill* held = active_unit_skill(c);
            if (held == nullptr) return false;
            const bool passed = !(held->cancel_chance < 100) ||
                                !(held->cancel_chance < volatile_random().integer_between(0, 100));
            return passed && !(delta == 0.0f);
        };
        bool follow = false;
        if (!c.pathing) {
            if (cancelled()) {
                set_ai_state(o, ai_state::kIdle, quests, out);
                monster_update_ai(o, c, 0.0f, quests, out, true);
                return;
            }
            follow = true;
        } else {
            const float fx = at.x - c.destination.x;
            const float fz = at.z - c.destination.z;
            if (!(std::sqrt(std::fma(fz, fz, fx * fx)) < 4.0f)) {
                if (cancelled()) {
                    set_ai_state(o, ai_state::kIdle, quests, out);
                    monster_update_ai(o, c, 0.0f, quests, out, true);
                    return;
                }
                follow = true;
            }
        }
        if (follow) follow_character(o, c, c.target, out);
        if (!c.pathing) {
            set_target(o, c, false, UnitRef{}, out);
            set_ai_state(o, ai_state::kNone, quests, out);
            return;
        }
    }
    // in range and in sight, the skill state, stopped,
    // and `updateAI(delta, true)` again.
    if (in_skill_range(c) == 0 && sight) {
        set_ai_state(o, ai_state::kSkill, quests, out);
        stop_pathing(c);
        monster_update_ai(o, c, delta, quests, out, true);
    }
}

// `CMonster::idleAINormal(float, CLevel&, bool)`.
//
bool LevelScript::idle_ai_normal(Object& o, Character& c, float delta, QuestState& quests,
                                 std::vector<ScriptEvent>& out) {
    c.run = false;
    // An alignment, not near death, stance 1, a game being played
    // (`(level +0x220)->+0x38d0 == 1`) and `KSETTINGS_PLAYER_UNTARGETABLE` 0.
    const bool sided = alignment_of(o) != UnitStats::Alignment::Neutral;
    const bool open = game_playing_ && !player_untargetable_;
    if (!sided || pet_near_death(o, c) || c.stance != 1 || !open) {
        // Any other stance, with a side and not near death, under the same two
        // gates: a defensive skill on the target held, or nothing.
        if (sided && !pet_near_death(o, c) && c.stance != 1 && open) {
            set_active_skill(c, -1);
            select_defensive_skill(c);
            if (active_unit_skill(c) != nullptr &&
                can_cast_current_skill(o, c, false, false, out)) {
                cast_skill(o, c, quests, out);
                return true;
            }
        }
        return false;
    }
    UnitRef found;
    // `calculateTargetInSight(7, nearest)`: the nearest for a pet that
    // attaches to its master, the first found otherwise.
    const bool nearest = has_master(c) && stats_for(o.unit).attaches_to_master;
    const bool sighted = find_character_within_view(o, c, nearest, found);
    // A skill first: defensive on a 35% draw when one is held, offensive
    // otherwise - with no one in sight only when none is held yet.
    const auto pick = [&]() {
        if (!has_defensive_skill(c) || 0x22 < volatile_random().integer_between(0, 100)) {
            select_offensive_skill(o, c, false, quests, out);
        } else {
            select_defensive_skill(c);
        }
    };
    if (!sighted) {
        if (c.casting.active < 0) pick();
        return false;
    }
    pick();
    // `canCastCurrentSkill(found, false, false)`: asked of the unit just found,
    // which becomes the target only when the answer is yes.
    if (can_cast_current_skill_on(o, c, found, out)) {
        set_target(o, c, true, found, out);
        stop_pathing(c);
        set_ai_state(o, ai_state::kSkill, quests, out);
        monster_update_ai(o, c, delta, quests, out, true);
        return true;
    }
    // A pet leaves alone what is more than 35 from its master.
    MasterView master;
    if (master_view(c, master) &&
        !(unit_types_ != nullptr &&
          unit_types_->isa(stats_for(o.unit).unittype, unit_type::kPetNoFollow))) {
        Vec3 there;
        if (target_at(found, there) && 35.0f < distance_xz(master.at, there)) return false;
    }
    if (!is_enemy(o, found)) return false;
    if (!can_attack_with_current_weapon(c)) return false;
    stop_pathing(c);
    set_ai_state(o, ai_state::kHunt, quests, out);
    set_target(o, c, true, found, out);
    // `ROAR_SOUND`.
    play_unit_sample(o, static_cast<int>(SoundSlot::Roar), out);
    return true;
}

// `CPropertyNode::killOrMoveBaseUnits()`. The
// walk is the level's character list (`CLevel +0x98`), active characters only
// (`+0x199`); the player itself, inside or out, is left alone.
void LevelScript::kill_or_move_base_units(const OccupationArea& area, QuestState& quests,
                                          std::vector<ScriptEvent>& out,
                                          std::vector<int64_t>& killed) {
    const auto distance = [](const Vec3& a, const Vec3& b) {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        const float dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    // A copy: a death can take a character off the list.
    const std::vector<int64_t> order = character_order_;
    for (const int64_t id : order) {
        Object* o = find(id);
        Character* c = character(id);
        if (o == nullptr || c == nullptr || !c->active) continue;
        const Vec3 at = feet_of(*c);
        // `AxisAlignedBox::contains`, both ends in.
        const bool inside = !(at.x < area.min.x) && !(area.max.x < at.x) && !(at.y < area.min.y) &&
                            !(area.max.y < at.y) && !(at.z < area.min.z) && !(area.max.z < at.z);
        if (!inside) {
            // a pet left no nearer its master than the node's scale.
            if (c->master_player && !(distance(at, player_at_) < area.scale_x)) {
                teleport_to_master(*o, *c, 1.0f);
            }
            continue;
        }
        if (c->master_player) {
            teleport_to_master(*o, *c, 1.0f);
            continue;
        }
        // `TYPE` 8, the circle: only within scale X of the node.
        if (area.circle && area.scale_x < distance(at, area.centre)) continue;
        // `modifyHP(-(float)HP())`, and `HP()` (inlined on
        // the console around `floor`) is the floor: a character with a
        // fraction of a point over is left with it, and lives (`update` asks
        // `+0x414 <= 0` before `die`).
        const float whole = std::floor(c->combat.hp);
        if (!(0.0f < whole)) continue;
        if (damage_unit(id, whole, kNoAttacker, quests, out, nullptr, nullptr, false,
                        /*shown=*/false)) {
            killed.push_back(id);
        }
    }
}

void LevelScript::add_unit_damage_shown(int64_t id, float amount) {
    Character* c = character(id);
    if (c == nullptr) return;
    c->damage_shown.pending = c->damage_shown.pending + amount;
}

// `CCharacter::displayDamageAbsorbed(float)`:
// which colour is its test of the character - a player (`ISA 0x1c`, never one
// of these), a unit whose master is one, or a unit of alignment 1.
void LevelScript::display_unit_damage(const Object& o, Character& c, float delta,
                                      std::vector<ScriptEvent>& out) {
    int amount = 0;
    if (!display_damage(c.damage_shown, stats_for(o.unit).display_dmg, delta, amount)) return;
    ScriptEvent e;
    e.action = ScriptAction::DamageText;
    e.node_id = o.id;
    e.name = o.name;
    e.unit = o.unit;
    e.amount = static_cast<float>(amount);
    e.on = c.master_player || alignment_of(o) == UnitStats::Alignment::Friendly;
    out.push_back(e);
}

// `CSoundBank::playSample(int, Ogre::SceneNode*, float, float, bool)`
// as the AI calls it: the unit's own bank and node.
// The samples a unit's bank holds at `slot`, in `CCharacter::loadModel`'s
// order: its own sound, then the common one at that slot.
// Each `CSoundBank::addSample` appends an instance per kept
// recording and leaves the slot its definition's chance and volume.
std::vector<LevelScript::BankInstance> LevelScript::unit_bank_samples(const Object& o,
                                                                      int slot) const {
    std::vector<BankInstance> out;
    // Each definition added as the recordings `addSample` keeps of it.
    const auto add = [&out](const SoundData* data) {
        for (const size_t file : bank_recordings(*data)) out.push_back(BankInstance{data, file});
    };
    if (sound_data_ == nullptr || slot < 0 || slot >= static_cast<int>(kSoundSlots)) return out;
    const std::string& name = stats_for(o.unit).sounds[static_cast<SoundSlot>(slot)];
    if (!name.empty()) {
        const SoundData* named = sound_data_->find(upper(name));
        if (named != nullptr && sound_data_->find_by_guid(named->guid) != nullptr) add(named);
    }
    struct Fixed {
        int slot;
        const char* name;
    };
    static const Fixed kFixed[] = {
        {static_cast<int>(SoundSlot::MissileReflect), "MISSILEREFLECT"},
        {static_cast<int>(SoundSlot::ShieldBlock), "METALBLOCK"},
        {static_cast<int>(SoundSlot::FireHit), "ELEMENTALFIREGETHIT"},
        {static_cast<int>(SoundSlot::IceHit), "ELEMENTALICEGETHIT"},
        {static_cast<int>(SoundSlot::ElectricHit), "ELEMENTALELECTRICGETHIT"},
        {static_cast<int>(SoundSlot::PoisonHit), "ELEMENTALPOISONGETHIT"},
        {static_cast<int>(SoundSlot::PhysicalHit), "SWORDFLESH"},
        {static_cast<int>(SoundSlot::Block), "BLOCK"},
        {static_cast<int>(SoundSlot::Critical), "CRITICALSTRIKE"},
    };
    for (const Fixed& fixed : kFixed) {
        if (fixed.slot != slot) continue;
        if (const SoundData* named = sound_data_->find(fixed.name)) add(named);
    }
    return out;
}

// `CSoundBank::playSample(slot, ...)`, its draws in order: a slot whose
// chance is not below 0 rolls `randomIntegerBetweenVolatile(1, 100)`, playing
// when the chance is above it; then the instance, `randomIntegerBetweenVolatile(0, count -
// 1)`; then `CSoundManager::playSound` draws the volume
// variation only once the sound started (`audio_plays`). Played as drawn
// (`ScriptEvent::on`).
void LevelScript::play_unit_sample(const Object& o, int slot, std::vector<ScriptEvent>& out) const {
    bool drawn = false;
    std::string sample;
    std::string recording;
    float variation = 0.0f;
    if (sound_data_ != nullptr) {
        if (slot < 0 || slot >= static_cast<int>(kSoundSlots)) return;
        // The bank's lists start empty (`CSoundBank::CSoundBank`): past the
        // highest slot filled nothing is drawn; an unfilled lower slot has chance 0,
        // so it rolls, then finds no sample.
        int filled = -1;
        for (int k = 0; k < static_cast<int>(kSoundSlots); ++k) {
            if (!unit_bank_samples(o, k).empty()) filled = k;
        }
        if (slot > filled) return;
        const std::vector<BankInstance> samples = unit_bank_samples(o, slot);
        const int32_t chance = samples.empty() ? 0 : samples.back().data->play_chance;
        RunicRandom& rolls = volatile_random();
        if (chance >= 0 && chance <= rolls.integer_between(1, 100)) return;
        if (samples.empty()) return;
        // Which instance - which recording - of the slot.
        const BankInstance& chosen = samples[static_cast<size_t>(
            rolls.integer_between(0, static_cast<int>(samples.size()) - 1))];
        // The slot's volume (+0x6c) is the last added's.
        if (samples.back().data->volume > 0.0f && audio_plays_ && audio_plays_()) {
            variation = rolls.between(0.0f, chosen.data->volume_variation);
        }
        sample = chosen.data->name;
        recording = chosen.data->files[chosen.file];
        drawn = true;
    }
    ScriptEvent e;
    e.on = drawn;
    e.sound = sample;
    e.text = recording;
    e.amount = variation;
    e.action = ScriptAction::UnitSample;
    e.node_id = o.id;
    e.name = o.name;
    e.unit = o.unit;
    e.input = static_cast<uint32_t>(slot);
    out.push_back(e);
}

bool LevelScript::can_cast_current_skill_on(Object& o, Character& c, const UnitRef& unit,
                                            std::vector<ScriptEvent>& out) {
    const bool had = c.has_target;
    const UnitRef was = c.target;
    c.has_target = true;
    c.target = unit;
    std::vector<ScriptEvent> probe;
    const bool castable = can_cast_current_skill(o, c, false, false, probe);
    c.has_target = had;
    c.target = was;
    if (castable) out.insert(out.end(), probe.begin(), probe.end());
    return castable;
}

// The head the three ranged idle handlers share:
// `gated` when the handler returns
// at once (no alignment, a pet near death, the player untargetable), else whether there
// is a unit. A pet ignores what is over 35 from its master; `no_follow_exempt` is the
// defender's `ISA(0xa9)`.
bool LevelScript::idle_ai_sight(Object& o, Character& c, bool no_follow_exempt, bool& gated,
                                UnitRef& found) {
    gated = alignment_of(o) == UnitStats::Alignment::Neutral || pet_near_death(o, c) ||
            player_untargetable_;
    if (gated) return false;
    // `calculateTargetInSight(7, nearest)`: the nearest for a pet that
    // attaches to its master, the first found otherwise.
    const bool nearest = has_master(c) && stats_for(o.unit).attaches_to_master;
    if (!find_character_within_view(o, c, nearest, found)) return false;
    MasterView master;
    if (!master_view(c, master)) return true;
    if (no_follow_exempt && unit_types_ != nullptr &&
        unit_types_->isa(stats_for(o.unit).unittype, unit_type::kPetNoFollow)) {
        return true;
    }
    Vec3 there;
    return !(target_at(found, there) && 35.0f < distance_xz(master.at, there));
}

bool LevelScript::can_attack_with_current_weapon(const Character& c) const {
    // `CMonster::canAttackWithCurrentWeapon`.
    if (0.0f < c.cooldown) return false;
    // `CCharacter::canAttackWithCurrentWeapon`: true with no
    // inventory, no weapon in use, or no known skill (`CSkillManager::knownSkills`);
    // else false while a `WEAPON` skill cools
    // (`CSkillManager::getAnySkillsCoolingByActivationType`).
    // INVENTED: a placed unit's weapon knows no skills, so this half is true.
    return true;
}

// `canAttackWithCurrentWeapon` (vtable +0x3f8,
// `CMonster`'s) and `inAttackRange(0)` on the unit just made the
// target - the hunt at once, the target set again, and `ROAR_SOUND`.
bool LevelScript::idle_ai_open_attack(Object& o, Character& c, const UnitRef& found,
                                      QuestState& quests, std::vector<ScriptEvent>& out) {
    if (!can_attack_with_current_weapon(c) || !in_attack_range(c, stats_for(o.unit))) return false;
    stop_pathing(c);
    set_ai_state(o, ai_state::kHunt, quests, out);
    set_target(o, c, true, found, out);
    play_unit_sample(o, static_cast<int>(SoundSlot::Roar), out);
    return true;
}

// `CMonster::idleAIRangedDefender(float, CLevel&, bool)`.
//
bool LevelScript::idle_ai_ranged_defender(Object& o, Character& c, QuestState& quests,
                                          std::vector<ScriptEvent>& out) {
    c.run = false;
    bool gated = false;
    UnitRef found;
    if (!idle_ai_sight(o, c, true, gated, found)) {
        // No one to act on: a defensive skill made ready, if none is.
        if (!gated && c.casting.active < 0) select_defensive_skill(c);
        return false;
    }
    set_target(o, c, true, found, out);
    if (idle_ai_open_attack(o, c, found, quests, out)) return true;
    select_defensive_skill(c);
    // `canCastCurrentSkill(null, false, false)`: of the target, and
    // `castSkill(-1)`.
    if (active_unit_skill(c) != nullptr && can_cast_current_skill(o, c, false, false, out)) {
        cast_skill(o, c, quests, out);
        return true;
    }
    if (9 < volatile_random().integer_between(0, 100)) return false;
    set_ai_state(o, ai_state::kFlee, quests, out);
    return true;
}

// `CMonster::idleAIRangedCaster(float, CLevel&, bool)`.
//
bool LevelScript::idle_ai_ranged_caster(Object& o, Character& c, float delta, QuestState& quests,
                                        std::vector<ScriptEvent>& out) {
    c.run = false;
    // Defensive on a 35% draw when one is held, offensive otherwise.
    const auto pick = [&]() {
        if (!has_defensive_skill(c) || 0x22 < volatile_random().integer_between(0, 100)) {
            select_offensive_skill(o, c, false, quests, out);
        } else {
            select_defensive_skill(c);
        }
    };
    bool gated = false;
    UnitRef found;
    if (!idle_ai_sight(o, c, false, gated, found)) {
        if (!gated && c.casting.active < 0) pick();
        return false;
    }
    set_target(o, c, true, found, out);
    if (idle_ai_open_attack(o, c, found, quests, out)) return true;
    pick();
    if (active_unit_skill(c) != nullptr && can_cast_current_skill_on(o, c, found, out)) {
        set_target(o, c, true, found, out);
        set_ai_state(o, ai_state::kSkill, quests, out);
        stop_pathing(c);
        monster_update_ai(o, c, delta, quests, out, true);
        return true;
    }
    if (9 < volatile_random().integer_between(0, 100)) return false;
    set_ai_state(o, ai_state::kFlee, quests, out);
    return true;
}

// `CMonster::idleAICircler(float, CLevel&, bool)`.
//
bool LevelScript::idle_ai_circler(Object& o, Character& c, float delta, QuestState& quests,
                                  std::vector<ScriptEvent>& out) {
    c.run = false;
    // Defensive on a 10% draw when one is held, offensive otherwise.
    const auto pick = [&]() {
        if (!has_defensive_skill(c) || 9 < volatile_random().integer_between(0, 100)) {
            select_offensive_skill(o, c, false, quests, out);
        } else {
            select_defensive_skill(c);
        }
    };
    bool gated = false;
    UnitRef found;
    if (!idle_ai_sight(o, c, false, gated, found)) {
        if (!gated && c.casting.active < 0) pick();
        return false;
    }
    set_target(o, c, true, found, out);
    if (idle_ai_open_attack(o, c, found, quests, out)) return true;
    pick();
    if (active_unit_skill(c) != nullptr && can_cast_current_skill_on(o, c, found, out)) {
        set_target(o, c, true, found, out);
        set_ai_state(o, ai_state::kSkill, quests, out);
        stop_pathing(c);
        monster_update_ai(o, c, delta, quests, out, true);
        return true;
    }
    // The hunt either way - and it stays the state when the draw
    // below fails and the handler answers that it did nothing.
    set_ai_state(o, ai_state::kHunt, quests, out);
    if (0x13 < volatile_random().integer_between(0, 100)) return false;
    // State 0xb (`getRangeAI`), to a point within 3 of the unit found: z drawn
    // first, then x.
    set_ai_state(o, 0xb, quests, out);
    Vec3 at;
    target_at(found, at);
    const float z = volatile_random().between(at.z - 3.0f, at.z + 3.0f);
    const float x = volatile_random().between(at.x - 3.0f, at.x + 3.0f);
    set_destination(c, stats_for(o.unit), x, z);
    return true;
}

// `CCharacter::addPet(player, c)` ending in `setMaster`:
// master the player, side `GOOD`, AI reset unless dying
// or dead, `setTarget(null)`.
bool LevelScript::make_player_pet(int64_t id, QuestState& quests, std::vector<ScriptEvent>& out) {
    Object* o = find(id);
    Character* c = character(id);
    if (o == nullptr || c == nullptr) return false;
    c->master_player = true;
    c->listed_pet = true;
    c->alignment = UnitStats::Alignment::Friendly;
    if (o->ai != ai_state::kDying && o->ai != ai_state::kDead) {
        stop_pathing(*c);
        set_ai_state(*o, ai_state::kIdle, quests, out);
    }
    set_target(*o, *c, false, UnitRef{}, out);
    return true;
}

bool LevelScript::remove_player_pet(int64_t id) {
    Character* c = character(id);
    if (c == nullptr || !c->master_player || !c->listed_pet) return false;
    // `removePet`, via `CCharacter::petIndex`:
    // closes the list over it; the pet's master and side stay.
    c->listed_pet = false;
    return true;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::unit_listed_as_player_pet(int64_t id) const {
    const Character* c = character(id);
    return c != nullptr && c->master_player && c->listed_pet;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
std::optional<UnitStats::Alignment> LevelScript::unit_side(int64_t id) const {
    const Object* o = find(id);
    if (o == nullptr || character(id) == nullptr) return std::nullopt;
    return alignment_of(*o);
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::unit_is_player_pet(int64_t id) const {
    const Character* c = character(id);
    return c != nullptr && c->master_player;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::unit_has_master(int64_t id) const {
    const Character* c = character(id);
    return c != nullptr && has_master(*c);
}

// `CCharacter::update` on a body: only the fade, the
// range, `CCharacter::characterShouldFadeOut`, the deletion and the camera test.
void LevelScript::body_update(Object& o, Character& c, float delta, std::vector<ScriptEvent>& out) {
    step_opacity(o, c, delta, false, out);
    calculate_active_range(o, c, level_camera_look_at(), out);
    // `dyingAI`: the body along its flight; at the end
    // `completeDeath`, state 6, `LAND_SOUND` unless it fell.
    if (c.flying && 0.0f < delta) {
        bool landed = false;
        const Vec3 at =
            step_death_flight(c.flight, c.flight_distance, c.motion.position.y, delta, landed);
        c.motion.position = at;
        o.position = feet_of(c);
        if (landed) {
            c.flying = false;
            o.ai = ai_state::kDead;
            ScriptEvent e;
            e.action = ScriptAction::DeathLanded;
            e.node_id = o.id;
            e.name = o.name;
            e.unit = o.unit;
            e.position = feet_of(c);
            e.on = !c.flight.falls;
            out.push_back(e);
        }
    }
    // `characterShouldFadeOut`: a body that is not a player's, under `DESTROY
    // DEAD CORPSE`.
    const bool fade_out = o.ai == ai_state::kDead && 0 < destroy_dead_corpse_;
    if (fade_out) want_visible(c, false);
    bool fade = fade_out;
    if (!c.active) {
        // and a body faded to nothing is marked for deletion
        // (`+0x190`) under the setting, or while the game runs at 30 frames a
        // second or fewer (`KSETTINGS_F_AVERAGE_FPS`).
        want_visible(c, false);
        fade = true;
        if (o.ai == ai_state::kDead && c.opacity <= 0.0f &&
            (destroy_dead_corpse_ != 0 || !(30.0f < average_fps_))) {
            c.pending_delete = true;
        }
        if (!o.visible) return;
    }
    const UnitStats& stats = stats_for(o.unit);
    if (!stats.render_always) {
        if (in_view(o)) {
            if (!c.mesh_hidden) want_visible(c, !fade);
        } else {
            set_character_visible(o, c, false, true, out);
        }
    }
    emit_character(o, c, out);
}

bool LevelScript::delete_character(int64_t id, std::vector<ScriptEvent>& out) {
    Object* o = find(id);
    Character* c = character(id);
    if (o == nullptr || c == nullptr) return false;
    // `CLevel::deleteCharacter`: off the
    // level's lists and gone.
    if (o->visible) {
        o->visible = false;
        ScriptEvent e;
        e.action = ScriptAction::SetVisible;
        e.node_id = o->id;
        e.name = o->name;
        e.on = false;
        out.push_back(e);
    }
    o->targetable = false;
    avoidance_remove(*c);
    inactive_.erase(id);
    dead_.insert(id);
    forget_character(id);
    return true;
}

// `CCharacter +0x648`, a master's pets, oldest first (`CCharacter::addPet`,
// appends): the living units listed as its pets.
std::vector<int64_t> LevelScript::pets_of(const LevelScript* master_machine,
                                          int64_t master_node) const {
    std::vector<int64_t> out;
    for (int64_t id : mastered_) {
        const Character* c = character(id);
        if (c == nullptr || !c->has_master || !c->listed_pet) continue;
        if (c->master.machine != master_machine || c->master.node != master_node) continue;
        const Object* o = find(id);
        if (o == nullptr) continue;
        if (dead_.count(id) != 0 || o->ai == ai_state::kDying || o->ai == ai_state::kDead) continue;
        out.push_back(id);
    }
    if (out.empty()) return out;
    // In the characters' order, as the list is kept.
    std::vector<int64_t> ordered;
    ordered.reserve(out.size());
    for (int64_t id : character_order_) {
        if (std::find(out.begin(), out.end(), id) != out.end()) ordered.push_back(id);
    }
    return ordered;
}

bool LevelScript::start_death_flight(int64_t id, const DeathFlight& flight) {
    Object* o = find(id);
    Character* c = character(id);
    if (o == nullptr || c == nullptr || flight.path.size() < 3) return false;
    c->flying = true;
    c->flown = true;
    c->flight = flight;
    c->flight_distance = 0.0f;
    // `die` set state 5; `completeDeath` sets 6 when the flight lands.
    o->ai = ai_state::kDying;
    return true;
}

// PORT: a dead unit put on its death flight (`plan_death_flight`, core/death_flight); `die`
// set state 5, `completeDeath` sets 6 when the flight lands.
bool LevelScript::start_death_flight(int64_t id, const Vec3& direction, float force,
                                     DeathFlight* laid) {
    const Character* c = character(id);
    if (c == nullptr || collision_ == nullptr) return false;
    const DeathFlight flight = plan_death_flight(*collision_, c->motion.position, c->motion.radius,
                                                 direction, force, volatile_random());
    if (!start_death_flight(id, flight)) return false;
    if (laid != nullptr) *laid = flight;
    return true;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::unit_flying(int64_t id) const {
    const Character* c = character(id);
    return c != nullptr && c->flying;
}

// PORT: a unit marked to be removed at the end of the frame.
bool LevelScript::mark_unit_for_deletion(int64_t id) {
    Character* c = character(id);
    if (c == nullptr || c->pending_delete) return false;
    c->pending_delete = true;
    return true;
}

bool LevelScript::set_unit_master(int64_t id, LevelScript& master_machine, int64_t master_node,
                                  QuestState& quests, std::vector<ScriptEvent>& out) {
    Object* o = find(id);
    Character* c = character(id);
    if (o == nullptr || c == nullptr || master_machine.character(master_node) == nullptr)
        return false;
    // `CCharacter::setMaster`: the master, its side (set by
    // the caller), then unless dying, dead, in town or near death, AI idle and no target.
    c->has_master = true;
    if (std::find(mastered_.begin(), mastered_.end(), id) == mastered_.end())
        mastered_.push_back(id);
    c->listed_pet = true;
    // `addPet`'s `+0x538` bit: whether the pet is on the avoidance map now.
    c->grid_with_master = c->on_grid;
    c->master = UnitRef{&master_machine, master_node};
    if (o->ai != ai_state::kDying && o->ai != ai_state::kDead && o->ai != ai_state::kInTown &&
        o->ai != 0x29) {
        set_ai_state(*o, ai_state::kIdle, quests, out);
    }
    set_target(*o, *c, false, UnitRef{}, out);
    return true;
}

bool LevelScript::set_unit_hidden(int64_t id, bool hidden, bool immediate,
                                  std::vector<ScriptEvent>& out) {
    Object* o = find(id);
    Character* c = character(id);
    if (o == nullptr || c == nullptr) return false;
    // Hiding marks `+0x70d` after `setVisible(false, ..)`, showing clears it before
    // (`updateAnimation` keys 0x16..0x19; cases 0x6f/0x70).
    if (hidden) {
        set_character_visible(*o, *c, false, immediate, out);
        c->mesh_hidden = true;
    } else {
        c->mesh_hidden = false;
        set_character_visible(*o, *c, true, immediate, out);
    }
    return true;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::set_unit_side(int64_t id, UnitStats::Alignment side) {
    Character* c = character(id);
    if (c == nullptr) return false;
    c->alignment = side;
    return true;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
std::vector<int64_t> LevelScript::character_ids() const { return character_order_; }

// PORT: the player's living pets in this machine.
std::vector<int64_t> LevelScript::player_pets() const {
    std::vector<int64_t> out;
    for (const Object& o : objects_) {
        if (o.type != type::kMonster) continue;
        const Character* c = character(o.id);
        if (c == nullptr || !c->master_player || !c->listed_pet) continue;
        if (dead_.count(o.id) != 0 || o.ai == ai_state::kDying || o.ai == ai_state::kDead) continue;
        out.push_back(o.id);
    }
    return out;
}

int LevelScript::desummon_player_pets(const std::string& unit, QuestState& quests,
                                      std::vector<ScriptEvent>& out) {
    const auto upper = [](std::string text) {
        for (char& ch : text) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return text;
    };
    const std::string wanted = upper(unit);
    int removed = 0;
    for (int64_t id : player_pets()) {
        Object* o = find(id);
        Character* c = character(id);
        if (o == nullptr || c == nullptr || upper(o->unit) != wanted) continue;
        // `removePet(CCharacter*)`, then
        // `die`.
        c->master_player = false;
        kill_character(*o, *c, quests, out);
        ++removed;
    }
    return removed;
}

// PORT: the player's pets of this unit let go and put back near the player (the quest dialog's `RELEASEPET`).
std::vector<int64_t> LevelScript::release_player_pets(const std::string& unit) {
    const auto upper = [](std::string text) {
        for (char& ch : text) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return text;
    };
    const std::string wanted = upper(unit);
    std::vector<int64_t> released;
    for (int64_t id : player_pets()) {
        const Object* o = find(id);
        Character* c = character(id);
        if (o == nullptr || c == nullptr || upper(o->unit) != wanted) continue;
        c->master_player = false;
        released.push_back(id);
    }
    return released;
}

// `CPlayer::levelResetting` puts each of the
// player's pets by its master with `teleportToMaster(count * 0.25)`,
// the count of every pet the player has.
bool LevelScript::put_pet_by_master(int64_t id, const Vec3& master, float distance) {
    Object* o = find(id);
    Character* c = character(id);
    if (o == nullptr || c == nullptr || !c->master_player) return false;
    player_at_ = master;
    teleport_to_master(*o, *c, distance);
    return true;
}

// `CCharacter::teleportToMaster(float)`: `distance` from
// the master back towards the unit, then `CLevel::randomOpenPosition(p, 8)` and
// `dropToGround(4.0)`, off the avoidance map while it moves.
void LevelScript::teleport_to_master(Object& o, Character& c, float distance) {
    MasterView master;
    if (!master_view(c, master)) return;
    const Vec3& player_at = master.at;
    const Vec3 at = feet_of(c);
    float dx = player_at.x - at.x;
    float dz = player_at.z - at.z;
    const float away = std::sqrt(dx * dx + 0.0f + dz * dz);
    if (1e-08f < away) {
        dx = dx / away;
        dz = dz / away;
    }
    const Vec3 wanted{player_at.x - dx * distance, player_at.y, player_at.z - dz * distance};
    const bool was = c.on_grid;
    avoidance_remove(c);
    const Vec3 landed = open_position_near(wanted, 0.0f, 8.0f);
    c.motion.position = {landed.x, landed.y + c.motion.radius, landed.z};
    drop_unit_to_ground(c);
    o.position = feet_of(c);
    if (was) avoidance_add(c);
    TL_LOG_INFO("script: %s is put back by its master at (%.1f, %.1f)", o.unit.c_str(),
                static_cast<double>(o.position.x), static_cast<double>(o.position.z));
}

// `+0x25c` as `CCharacter::updateAnimation` works it out for
// `updatePathing`: the speed effect again as a factor, 1.5x under `BERSERK`. A
// running player pet with `CANMATCHSPEED` slower than its master's run + 2 moves at that.
// Worked out where used, which comes to the same.
float LevelScript::speed_multiplier(const Character& c, const UnitStats& stats) const {
    // `CCharacter::updateAnimation`: above 100 is
    // 100; a slow (or NaN) damped by 0x8c's share and held at -100; factor
    // `fma(percent, 0.01f, 1)`. `CCharacter::getEffectValue`.
    float percent = character_effect(c, 0x15);
    if (percent > 100.0f) {
        percent = 100.0f;
    } else {
        if (!(percent >= 0.0f)) {
            float kept = character_effect(c, 0x8c) * 0.01f;
            kept = kept < 1.0f ? kept : 1.0f;
            kept = kept > 0.0f ? kept : 0.0f;
            percent = (1.0f - kept) * percent;
        }
        if (!(percent >= -100.0f)) percent = -100.0f;
    }
    float factor = std::fma(percent, 0.01f, 1.0f);
    if (c.ai_flags.has(AIFlag::Berserk)) factor = factor * 1.5f;
    const bool no_follow =
        unit_types_ != nullptr && unit_types_->isa(stats.unittype, unit_type::kPetNoFollow);
    MasterView master;
    if (!no_follow && c.run && master_view(c, master) &&
        !(stats.movement.running_speed <= stats.movement.walking_speed)) {
        const float mine = character_speed(c, stats.movement.running_speed);
        if (0.0f < mine && master.running_speed + 2.0f > mine && !(percent < 0.0f) &&
            c.can_match_speed) {
            factor = (master.running_speed + 2.0f) / mine;
        }
    }
    return factor < 0.1f ? 0.1f : factor;
}

// `CCharacter::sendToTown(CLevel&)`.
bool LevelScript::send_unit_to_town(int64_t id, float own_return, float master_return,
                                    bool level_is_town, QuestState& quests,
                                    std::vector<ScriptEvent>& out) {
    Object* o = find(id);
    Character* c = character(id);
    if (o == nullptr || c == nullptr || level_is_town || !c->master_player) return false;
    if (dead_.count(id) != 0 || o->ai == ai_state::kDying || o->ai == ai_state::kDead) return false;
    // `min(CLevel +0x1a8 * 10, 120)`, less the `PET_TOWN_RETURN` share, at least 1.
    float timer = static_cast<float>(level_monster_level_) * 10.0f;
    if (120.0f < timer) timer = 120.0f;
    // the unit's share times 0.01f, the master's
    // fused on (`fmadds`), `fnmsubs` off the timer; at least 1 (a NaN is 1).
    const float share = std::fma(master_return, 0.01f, own_return * 0.01f);
    timer = std::fma(-timer, share, timer);
    if (!(timer > 1.0f)) timer = 1.0f;
    c->town_timer = timer;
    set_target(*o, *c, false, UnitRef{}, out);
    set_ai_state(*o, ai_state::kGoingToTown, quests, out);
    // `CLevel::randomOpenPositionRange(master, 20, 30)`.
    const Vec3 to = open_position_near(player_at_, 20.0f, 30.0f);
    set_destination(*c, stats_for(o->unit), to.x, to.z);
    return true;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
float LevelScript::unit_town_timer(int64_t id) const {
    const Character* c = character(id);
    return c != nullptr ? c->town_timer : 0.0f;
}

// `CCharacter::spawn` and `CCharacter::spawningAI`;
// the renderer plays the entrance and reports its end.
// `setAIState` without its path half.
void LevelScript::set_spawning(int64_t id, bool spawning) {
    Object* o = find(id);
    Character* c = character(id);
    if (spawning) {
        spawning_.insert(id);
        if (o != nullptr && c != nullptr) {
            o->ai = 0xc;
            // `getSpawnInvisible()`: the fade (`+0x528`) to 0, which the next
            // `updateOpacity` writes and raises.
            if (stats_for(o->unit).spawn_invisible) c->opacity = 0.0f;
        }
        return;
    }
    if (spawning_.erase(id) == 0) return;
    if (o == nullptr || c == nullptr || o->ai != 0xc) return;
    o->ai = c->has_target ? ai_state::kHunt : ai_state::kNone;
}

bool LevelScript::restore_unit_town_trip(int64_t id, int32_t state, float timer, QuestState& quests,
                                         std::vector<ScriptEvent>& out) {
    Object* o = find(id);
    Character* c = character(id);
    if (o == nullptr || c == nullptr || !c->master_player) return false;
    // `CLevel::removeCharacter`'s `state != 0x2a`: every
    // other state, 0x29 among them, is idle on arrival.
    if (state != ai_state::kInTown) return false;
    if (!(0.0f < timer)) return false;
    c->town_timer = timer;
    set_target(*o, *c, false, UnitRef{}, out);
    stop_pathing(*c);
    set_ai_state(*o, state, quests, out);
    return true;
}

std::vector<int64_t> LevelScript::take_units_back_from_town() {
    std::vector<int64_t> out;
    out.swap(back_from_town_);
    return out;
}

// `CCharacter::returnToTownAI(float, CLevel&)`,
// AI state 0x29: running to the point
// `sendToTown` chose; there, every skill stops (`stopAllSkills(true, false,
// false)`) and it is in town (0x2a).
void LevelScript::return_to_town_ai(Object& o, Character& c, QuestState& quests,
                                    std::vector<ScriptEvent>& out) {
    c.run = true;
    if (c.pathing) return;
    if (unit_skills_.stop_all) unit_skills_.stop_all(*this, o.id, /*force=*/true);
    set_ai_state(o, ai_state::kInTown, quests, out);
}

// `CCharacter::updateInTownTimer(float, CLevel&)`, from
// `CCharacter::update` in state 0x2a: when the clock is
// out (at once in town) the unit is healed, idle, put 18 to 25 from its master and
// dropped; the scene does the rest. During a cinematic an expired clock is set to 0.1 s.
void LevelScript::update_in_town_timer(Object& o, Character& c, float delta, QuestState& quests,
                                       std::vector<ScriptEvent>& out) {
    const bool cinematic = ui_in_cinematic();
    if (c.town_timer <= 0.0f) return;
    c.town_timer = c.town_timer - delta;
    if (0.0f < c.town_timer && !level_is_town_) return;
    if (cinematic) {
        if (!(0.0f < c.town_timer)) c.town_timer = 0.1f;
        return;
    }
    c.combat.hp = static_cast<float>(max_hp(c.combat));
    c.town_timer = 0.0f;
    set_ai_state(o, ai_state::kNone, quests, out);
    if (c.master_player) {
        const Vec3 to = open_position_near(player_at_, 18.0f, 25.0f);
        stop_pathing(c);
        c.motion.position = {to.x, to.y + c.motion.radius, to.z};
        drop_unit_to_ground(c);
        o.position = feet_of(c);
    }
    back_from_town_.push_back(o.id);
}

// `CLevel::randomOpenPositionRange(at, lo, hi, false)` on this
// level's grids, drawn from the volatile stream as the port's level does.
Vec3 LevelScript::open_position_near(const Vec3& at, float lo, float hi) {
    return random_open_position(
        at, lo, hi,
        [this](const Vec3& p) {
            return passability_ == nullptr || !passability_->ready() ||
                   (passability_->map_passable(p) && passability_->position_passable(p));
        },
        volatile_random());
}

// `CCharacter::dropToGround(4.0, level)`: on the ground under the sphere, an
// open spot near it when it is blocked.
void LevelScript::drop_unit_to_ground(Character& c) {
    if (collision_ == nullptr || collision_->empty()) return;
    drop_to_ground_open(*collision_, c.motion, 4.0f, [this](const Vec3& at, float radius) {
        return open_position_near(at, 0.0f, radius);
    });
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::set_unit_stance(int64_t id, int32_t stance) {
    Character* c = character(id);
    if (c == nullptr) return false;
    c->stance = stance;
    return true;
}

// PORT: a unit's target cleared (`set_target`).
bool LevelScript::drop_unit_target(int64_t id, std::vector<ScriptEvent>& out) {
    Object* o = find(id);
    Character* c = character(id);
    if (o == nullptr || c == nullptr) return false;
    set_target(*o, *c, false, UnitRef{}, out);
    return true;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::set_unit_can_match_speed(int64_t id, bool on) {
    Character* c = character(id);
    if (c == nullptr) return false;
    c->can_match_speed = on;
    return true;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
int32_t LevelScript::unit_ai_state(int64_t id) const {
    const Object* o = find(id);
    return o != nullptr && character(id) != nullptr ? o->ai : -1;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
int32_t LevelScript::unit_visible(int64_t id) const {
    const Object* o = find(id);
    if (o == nullptr || character(id) == nullptr) return -1;
    return o->visible ? 1 : 0;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::unit_running(int64_t id) const {
    const Character* c = character(id);
    return c != nullptr && c->run;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::unit_pathing(int64_t id) const {
    const Character* c = character(id);
    return c != nullptr && c->pathing;
}

// PORT: `pet_near_death` asked of a unit by id.
bool LevelScript::unit_near_death(int64_t id) const {
    const Object* o = find(id);
    const Character* c = character(id);
    return o != nullptr && c != nullptr && pet_near_death(*o, *c);
}

// PORT: a unit's own effects loaded on first use, through the renderer's hook.
// `CBaseUnit::reapplyEffects(false)`: the rolled own
// `[EFFECT]`s into its manager, each through `activateEffect` (which may teach a skill).
void LevelScript::load_own_effects(Character& c) {
    if (c.own_effects_loaded) return;
    if (unit_skills_.own_effects) {
        c.own_effects_loaded = true;
        unit_skills_.own_effects(*this, c.node);
        return;
    }
    if (!c.effects || c.own_effects.empty()) return;
    c.own_effects_loaded = true;
    // The manager `roll_unit_creation` made holds them already; each one's
    // `activateEffect` now that the character is there to learn from it.
    for (Effect effect : c.own_effects) activate_unit_effect(c.node, effect);
    c.effects->calculate_effect_values();
}

// The activations `reapplyAffixes` runs as it adds each
// affix's effects (`addNewEffect` -> `activateEffect`), after the `[SKILL]`s.
void LevelScript::activate_unit_affixes(Character& c) {
    if (!c.effects) return;
    for (const LiveAffix& affix : c.effects->affixes()) {
        for (Effect effect : affix.effects) activate_unit_effect(c.node, effect);
    }
}

namespace {

/// A placed unit's known list (`CSkillManager +0x60`) as `activateEffect` asks of it.
// PORT: the unit's list turned into the copies `activate_effect` works on.
SkillCopies unit_known_list(const LevelScript::UnitCasting& casting, bool one_per_name) {
    SkillCopies known(one_per_name);
    for (const LevelScript::UnitSkill& held : casting.skills) {
        SkillCopy copy;
        copy.name = held.known.entry.name;
        copy.level = held.known.invested;
        copy.enabled = held.known.enabled;
        copy.hidden = held.known.def != nullptr && held.known.def->hidden;
        copy.charges = held.known.charges;
        known.push_back(copy);
    }
    return known;
}

}  // namespace

// `CBaseUnit::activateEffect(CEffect*)` on a level unit.
void LevelScript::activate_unit_effect(int64_t node, Effect& effect) {
    if (!effect_teaches_skill(effect.type)) return;
    UnitCasting* k = unit_casting(node);
    if (k == nullptr || graphs_ == nullptr || !unit_skills_.skill_files || !unit_skills_.skill_def)
        return;
    if (!unit_skills_.skill_files(effect.name)) {
        TL_LOG_WARN("skills: Skill not found : %s (%s)", effect.name.c_str(),
                    unit_of(node).c_str());
        return;
    }
    // `addSkillByName(NAME, true)` appends a copy; `CCharacter::deactivateEffect`
    // deletes it. INVENTED: the delete is not carried, so one copy
    // per name is kept and re-levelled (`player_proc_skills`).
    SkillCopies known = unit_known_list(*k, /*one_per_name=*/true);
    const size_t before = known.copies().size();
    activate_effect(effect, known, *graphs_, unit_skills_.skill_files);
    const std::string key = upper(effect.name);
    const SkillCopy* made = nullptr;
    for (const SkillCopy& copy : known.copies()) {
        if (upper(copy.name) == key) made = &copy;
    }
    if (made == nullptr) return;
    // `CSkillManager::setSkillLevel`
    // re-levels every copy of the name.
    for (UnitSkill& held : k->skills) {
        if (upper(held.known.entry.name) != key) continue;
        held.known.invested = made->level;
        held.known.effective = made->level;
    }
    if (known.copies().size() == before) return;
    // The new copy is `CSkill::CSkill` of the
    // template: enabled, `+0x118` 100, `+0x11c` 0, its charges as
    // `activateEffect` set them.
    const SkillDef* def = unit_skills_.skill_def(key);
    if (def == nullptr) return;
    UnitSkill skill;
    skill.known.def = def;
    skill.known.entry.name = def->name;
    skill.known.invested = made->level;
    skill.known.effective = made->level;
    skill.cast_chance = 100;
    skill.cancel_chance = 0;
    skill.known.charges = made->charges;
    k->skills.push_back(std::move(skill));
    TL_LOG_INFO("skills: %s learns '%s' at level %u from its effect '%s'", unit_of(node).c_str(),
                def->name.c_str(), made->level, effect.name.c_str());
}

namespace {

// Whether a known skill came from the unit's own `[SKILL]` blocks: what
// `CCharacter::convertCharacter` takes off before the
// unit is re-made.
bool from_skill_block(const std::vector<LevelScript::UnitSkill>& blocks,
                      const LevelScript::UnitSkill& held) {
    for (const LevelScript::UnitSkill& block : blocks) {
        if (upper(block.known.entry.name) == upper(held.known.entry.name) &&
            block.known.invested == held.known.invested) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool LevelScript::convert_character(int64_t id, const std::string& unit, QuestState& quests,
                                    std::vector<ScriptEvent>& out) {
    // CCharacter::convertCharacter
    Object* o = find(id);
    Character* c = character(id);
    if (o == nullptr || c == nullptr || unit.empty()) return false;
    // `getDataGroupByObjectName("MONSTERS", unit)`, then `"PLAYERS"`: a unit
    // with no record is logged and nothing changes.
    if (units_ == nullptr || units_->find(unit) == nullptr) {
        TL_LOG_WARN("script: Missing datagroup for character %s", unit.c_str());
        return false;
    }
    const std::string was = o->unit;
    // The weapons in its hands are deleted (`removeEquipment`). Each
    // `[SKILL]` block's skill leaves the manager (with the active skill if one); skills its
    // effects taught stay. `setAlignment(alignment())` round `unitInit` keeps its side.
    const UnitStats::Alignment side = alignment_of(*o);
    const Vec3 feet = feet_of(*c);
    const float facing = c->facing;
    const int level = c->combat.level;
    stop_pathing(*c);
    avoidance_remove(*c);
    Character carried = std::move(*c);
    if (unit_skills_.load) {
        const std::vector<UnitSkill> blocks = unit_skills_.load(was);
        std::deque<UnitSkill> kept;
        for (const UnitSkill& held : carried.casting.skills) {
            if (!from_skill_block(blocks, held)) kept.push_back(held);
        }
        carried.casting.skills = std::move(kept);
    }
    carried.alignment = side;

    // `deleteUnitSpecificData` and `unitInit`
    // (vtable +0x1f0) on the new unit's record: re-made at its own `LEVEL`,
    // else the level it was.
    o->unit = unit;
    // `unitInit`'s `from_save`: the old record was there, or the remembered level lists it
    // (`CGameClient::storedUnitExists`). So a converted unit keeps empty hands, gold and
    // stock unrolled, except a stranger (a pet) on a remembered level.
    const bool from_save =
        has_saved_state_ ? (stored_unit_exists_ && stored_unit_exists_(id)) : true;
    const UnitCreation made = roll_unit_creation(unit, !from_save, level);
    Character& fresh = make_character(*o, feet, facing, &made, &carried);
    load_unit_skills(fresh);

    ScriptEvent e;
    e.action = ScriptAction::Transform;
    e.node_id = id;
    e.name = o->name;
    e.unit = unit;
    e.text = was;
    out.push_back(e);
    (void)quests;
    TL_LOG_INFO("script: %s becomes %s", was.c_str(), unit.c_str());
    return true;
}

bool LevelScript::transform_unit(int64_t id, const std::string& unit, float duration,
                                 QuestState& quests, std::vector<ScriptEvent>& out) {
    // CCharacter::transformCharacter
    Object* o = find(id);
    Character* c = character(id);
    if (o == nullptr || c == nullptr || dead_.count(id) != 0) return false;
    if (units_ == nullptr || units_->find(unit) == nullptr) {
        TL_LOG_WARN("script: Missing datagroup for character %s", unit.c_str());
        return false;
    }
    // `setAIState(0)` (vtable +0x348) unless in town (0x2a).
    if (o->ai != ai_state::kInTown) set_ai_state(*o, ai_state::kNone, quests, out);
    c = character(id);
    if (c == nullptr) return false;
    c->transform_left = duration;                                    // +0x688
    if (c->transformed_from.empty()) c->transformed_from = o->unit;  // +0x690
    // INVENTED: `setVisible(false, true)` then `setVisible(true, true)`
    // - shown again at once - is left to the scene's swap.
    return convert_character(id, unit, quests, out);
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
std::string LevelScript::unit_transformed_from(int64_t id) const {
    const Character* c = character(id);
    return c != nullptr ? c->transformed_from : std::string();
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
float LevelScript::unit_transform_left(int64_t id) const {
    const Character* c = character(id);
    return c != nullptr ? c->transform_left : 0.0f;
}

bool LevelScript::restore_unit_transformation(int64_t id, float left, const std::string& from) {
    // CCharacter::applySaveState: `+0x690`
    // from the state's +0x28, `+0x688` from its +0x17c.
    Character* c = character(id);
    if (c == nullptr) return false;
    c->transform_left = left;
    c->transformed_from = from;
    return true;
}

void LevelScript::lock_unit_procs(int64_t id) {
    // `CCharacter::executeProcs`: `+0x708 = 1.0`.
    if (Character* c = character(id)) c->proc_lockout = 1.0f;
}

// PORT: a skill a unit learns put in its list, replacing one of the same name.
bool LevelScript::add_unit_skill(int64_t id, const UnitSkill& skill) {
    Character* c = character(id);
    if (c == nullptr) return false;
    load_unit_skills(*c);
    std::deque<UnitSkill>& skills = c->casting.skills;
    for (UnitSkill& held : skills) {
        if (upper(held.known.entry.name) == upper(skill.known.entry.name)) {
            held = skill;
            return true;
        }
    }
    skills.push_back(skill);
    return true;
}

// PORT: a unit's skill of this name removed, the active index kept on the same skill.
bool LevelScript::remove_unit_skill(int64_t id, const std::string& name) {
    Character* c = character(id);
    if (c == nullptr) return false;
    std::deque<UnitSkill>& skills = c->casting.skills;
    for (size_t i = 0; i < skills.size(); ++i) {
        if (upper(skills[i].known.entry.name) != upper(name)) continue;
        const int index = static_cast<int>(i);
        if (c->casting.active == index) {
            c->casting.active = -1;
        } else if (index < c->casting.active) {
            --c->casting.active;
        }
        skills.erase(skills.begin() + static_cast<long>(i));
        return true;
    }
    return false;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::set_unit_effects(int64_t id, std::shared_ptr<EffectManager> effects) {
    Character* c = character(id);
    if (c == nullptr) return false;
    c->effects = std::move(effects);
    return true;
}

// PORT: the level machine's own bookkeeping: an accessor of its unit records.
bool LevelScript::set_unit_inventory(int64_t id, const Inventory* inventory) {
    Character* c = character(id);
    if (c == nullptr) return false;
    c->inventory = inventory;
    return true;
}

// PORT: a unit's mana, its skills loaded first.
bool LevelScript::unit_mana(int64_t id, float& mana, float& max) {
    Character* c = character(id);
    if (c == nullptr) return false;
    load_unit_skills(*c);
    if (!c->casting.loaded) return false;
    mana = c->casting.mana;
    max = static_cast<float>(c->casting.max_mana);
    return true;
}

// PORT: `skill_holds_character` asked of a unit's cast.
bool LevelScript::unit_skill_holds(int64_t id) const {
    const Character* c = character(id);
    if (c == nullptr || c->casting.active < 0) return false;
    return skill_holds_character(c->casting.cast);
}

// `CCharacter +0x640`, the master (`CCharacter::addPet`). DIVERGENCE: the original keeps it pointing at
// a master that has been deleted; here a pet whose master is gone has none.
bool LevelScript::has_master(const Character& c) const {
    if (c.master_player) return true;
    // DIVERGENCE: the original keeps `+0x640` pointing at a master that has
    // been deleted (neither `die` nor `~CCharacter` lets its pets go); here a
    // pet whose master is gone has none.
    return c.has_master && c.master.machine != nullptr &&
           c.master.machine->character(c.master.node) != nullptr;
}

// PORT: what a pet reads of its master: the player's, or a level unit's.
bool LevelScript::master_view(const Character& c, MasterView& out) const {
    if (c.master_player) {
        out.at = player_at_;
        out.centre = {player_at_.x, player_at_.y + player_radius_, player_at_.z};
        out.forward = player_forward_;
        out.has_target = player_has_attack_target_;
        out.target = player_attack_target_;
        out.running = player_running_;
        out.walking_speed = player_walking_speed_;
        out.running_speed = player_running_speed_;
        out.moving = player_moving_;
        return true;
    }
    if (!has_master(c)) return false;
    const LevelScript& runs = *c.master.machine;
    const Character& m = *runs.character(c.master.node);
    const Object* mo = runs.find(c.master.node);
    const UnitStats& stats = runs.stats_for(mo != nullptr ? mo->unit : std::string());
    out.at = runs.feet_of(m);
    out.centre = m.motion.position;
    out.forward = {std::sin(m.facing), 0.0f, std::cos(m.facing)};
    out.has_target = m.has_target;
    out.target = m.target;
    out.running = m.run;
    out.walking_speed = runs.character_speed(m, stats.movement.walking_speed);
    out.running_speed = runs.character_speed(m, stats.movement.running_speed);
    out.moving = m.pathing;
    return true;
}

// `CCharacter::isPetNearDeath()`: a character with a
// master and `INVINCIBLE` at its last hit point, or under 15% while already fleeing.
bool LevelScript::pet_near_death(const Object& o, const Character& c) const {
    if (!has_master(c) || !stats_for(o.unit).invincible_flag) return false;
    const int hp = static_cast<int>(c.combat.hp);
    if (hp < 2) return true;
    const int max = max_hp(c.combat);
    if (static_cast<float>(hp) <= static_cast<float>(max) * 0.15f) return o.ai == ai_state::kFlee;
    return false;
}

// The point a pet keeps station: in the
// box from three units ahead of the master less 7.2 to the master plus 7.2,
// z drawn first.
void LevelScript::pet_wander_point(Character& c, const UnitStats& stats) {
    MasterView master;
    if (!master_view(c, master)) return;
    const Vec3 ahead{master.at.x + master.forward.x * 3.0f, master.at.y,
                     master.at.z + master.forward.z * 3.0f};
    const float z = volatile_random().between(ahead.z - 7.2f, master.at.z + 7.2f);
    const float x = volatile_random().between(ahead.x - 7.2f, master.at.x + 7.2f);
    set_destination(c, stats, x, z);
}

// `CMonster::idleAI`, its tail for a
// character with a master - the player or a monster that summoned it
// - when it does not attach to the master, follows no path, is enabled and is not `PETNOFOLLOW`.
void LevelScript::idle_ai_pet(Object& o, Character& c, QuestState& quests,
                              std::vector<ScriptEvent>& out) {
    const UnitStats& stats = stats_for(o.unit);
    MasterView master;
    if (!master_view(c, master)) return;
    // The master's `+0x321`, while its walking speed is under its running speed.
    c.run = master.running && master.walking_speed < master.running_speed;
    // Any stance but Passive (`+0x710`, 1 from the constructor) takes on the
    // master's own target when it is an enemy within 8 of the master, unless
    // near death.
    if (c.stance != 0 && master.has_target && is_enemy(o, master.target) && !pet_near_death(o, c)) {
        Vec3 there;
        if (target_at(master.target, there) && distance_xz(master.at, there) <= 8.0f) {
            set_target(o, c, true, master.target, out);
            set_ai_state(o, ai_state::kHunt, quests, out);
            return;
        }
    }
    const float away = distance_xz(feet_of(c), master.at);
    if (away <= 6.0f) {
        if (away <= 8.0f && !master.moving) c.run = false;
    } else {
        c.run = true;
    }
    // Past 25 it is put 20 from the master - and carries on.
    if (25.0f < away && 0.0f < character_speed(c, stats.movement.walking_speed)) {
        teleport_to_master(o, c, 20.0f);
    }
    const bool near_death = pet_near_death(o, c);
    if (near_death) c.run = true;
    const float drift = distance_xz(master.at, c.pathing ? c.destination : feet_of(c));
    // The C runtime's `rand()`, not either of Runic's generators.
    //
    const bool roll = crt_random().next() % 1000 < 100;
    if (!(roll || 8.0f < drift || near_death)) return;
    if (near_death) set_ai_state(o, ai_state::kFlee, quests, out);
    if ((near_death && !c.pathing) || (8.0f < drift && 0.25f < c.since_route)) {
        c.run = true;
        pet_wander_point(c, stats);
    }
}

// `CMonster::idleAI(float, CLevel&, bool)`.
void LevelScript::idle_ai(Object& o, Character& c, float delta, QuestState& quests,
                          std::vector<ScriptEvent>& out) {
    const UnitStats& stats = stats_for(o.unit);
    bool found = false;
    if (o.enabled && o.path == 0) {
        // `AITYPE` (`+0x334`, the switch): NORMAL, DEFENDER and
        // RESURRECTER share a handler; DUMMY (6) has none.
        switch (stats.ai_type) {
            case 0:
            case 2:
            case 5: found = idle_ai_normal(o, c, delta, quests, out); break;
            case 1: found = idle_ai_ranged_defender(o, c, quests, out); break;
            case 3: found = idle_ai_ranged_caster(o, c, delta, quests, out); break;
            case 4: found = idle_ai_circler(o, c, delta, quests, out); break;
            default: break;
        }
    }
    // `CMonster::idleAI`, after the type's
    // handler: a mimic still in the idle state with `MIMICIDLE` does nothing
    // more - no fidget, no wander; in any other state the theme comes off.
    if (unit_types_ != nullptr && unit_types_->isa(stats.unittype, unit_type::kMimic)) {
        if (o.ai == ai_state::kIdle) {
            if (c.mimic_idle) return;
        } else if (c.mimic_idle) {
            c.mimic_idle = false;
            TL_LOG_INFO("script: %s wakes on what it saw", o.unit.c_str());
        }
    }
    if (found) return;
    // an idle, enabled unit with a model and no route rolls
    // `randomIntegerBetweenVolatile(0, 1000)` each think; under 10, after over ten idle seconds
    // and with a `FIDGET` animation, it plays one (0.2 s blend, `IDLE` queued), takes state 1,
    // plays its `IDLE` sample and restarts both clocks.
    if (!stats.mesh.empty() && o.enabled && !c.pathing && o.path == 0 && o.ai == ai_state::kIdle) {
        if (volatile_random().integer_between(0, 1000) < 10) {
            if (c.fidget_wait > 10.0f && unit_animation_prefix_exists(o.unit, "FIDGET")) {
                std::string animation;
                float length = 0.0f;
                find_random_animation(o.unit, "FIDGET", animation, length);
                ScriptEvent e;
                e.action = ScriptAction::Animation;
                e.node_id = o.id;
                e.name = o.name;
                e.unit = o.unit;
                e.text = animation;
                e.on = true;
                e.loops = false;
                e.then_idle = true;
                e.blend = 0.2f;
                out.push_back(e);
                c.fidget_left = length;
                set_ai_state(o, ai_state::kFidget, quests, out);
                play_unit_sample(o, static_cast<int>(SoundSlot::Idle), out);
                c.fidget_timer = 0.0f;
                c.fidget_wait = 0.0f;
            }
            return;
        }
    }
    if (character_speed(c, stats.movement.walking_speed) == 0.0f) return;
    if (o.cannot_wander) return;
    const bool no_follow =
        unit_types_ != nullptr && unit_types_->isa(stats.unittype, unit_type::kPetNoFollow);
    // an interactive character talking with the player stands still.
    // DIVERGENCE: the original follows its path here (`updatePathFollowing(false)`);
    // on the console they stop and resume after.
    if (c.has_target && c.target.player() && player_target_ != 0 && player_target_ == o.id) {
        stop_pathing(c);
    } else if (o.path != 0) {
        // A path is followed: a `Pathing` node walks the player's
        // pet like any unit - the mines' fourth floor sends Brink to his mark.
        path_follow(o, false, feet_of(c), quests, out);
    } else if (!c.pathing && volatile_random().between(0.0f, 1000.0f) < 100.0f) {
        if (has_master(c) && o.enabled && !no_follow) {
            pet_wander_point(c, stats);
        } else if (o.enabled) {
            const float z =
                volatile_random().between(c.motion.position.z - 8.0f, c.motion.position.z + 8.0f);
            const float x =
                volatile_random().between(c.motion.position.x - 8.0f, c.motion.position.x + 8.0f);
            set_destination(c, stats, x, z);
        } else {
            stop_pathing(c);
        }
    }
    if (has_master(c) && !stats.attaches_to_master && o.path == 0 && o.enabled && !no_follow) {
        idle_ai_pet(o, c, quests, out);
    }
}

// `CMonster::fidgetAI`: back to idle once no `FIDGET` plays (the port's
// clock is `fidget_left`); a pet over 8 from its master (not `ISA 0xa9`) at once.
void LevelScript::fidget_ai(Object& o, Character& c, QuestState& quests,
                            std::vector<ScriptEvent>& out) {
    if (o.ai == ai_state::kDying || o.ai == ai_state::kDead) return;
    if (!(c.fidget_left > 0.0f)) set_ai_state(o, ai_state::kIdle, quests, out);
    const UnitStats& stats = stats_for(o.unit);
    const bool no_follow =
        unit_types_ != nullptr && unit_types_->isa(stats.unittype, unit_type::kPetNoFollow);
    MasterView master;
    if (no_follow || !master_view(c, master)) return;
    const Vec3 here = feet_of(c);
    const float dx = master.at.x - here.x;
    const float dz = master.at.z - here.z;
    if (!(std::sqrt(std::fma(dz, dz, dx * dx)) > 8.0f)) return;
    set_ai_state(o, ai_state::kIdle, quests, out);
    c.fidget_left = 0.0f;
    ScriptEvent e;
    e.action = ScriptAction::Animation;
    e.node_id = o.id;
    e.name = o.name;
    e.unit = o.unit;
    e.text = "IDLE";
    e.on = true;
    e.loops = true;
    e.blend = 0.1f;
    out.push_back(e);
}

// `CMonster::huntAI(float, CLevel&)`.
void LevelScript::hunt_ai(Object& o, Character& c, QuestState& quests,
                          std::vector<ScriptEvent>& out) {
    const UnitStats& stats = stats_for(o.unit);
    auto to_idle = [&]() {
        set_ai_state(o, ai_state::kIdle, quests, out);
        set_target(o, c, false, UnitRef{}, out);
    };
    // A near-death pet on either side ends the hunt as a gone target does.
    const auto target_near_death = [&]() {
        if (c.target.player() || c.target.machine == nullptr) return false;
        return c.target.machine->unit_near_death(c.target.node);
    };
    // `canAttackWithCurrentWeapon` through the vtable.
    if (!c.has_target || !target_alive(c.target) || pet_near_death(o, c) || target_near_death() ||
        !can_attack_with_current_weapon(c)) {
        to_idle();
        return;
    }
    // a friend for a target ends the hunt in state
    // 0, not idle (`li r4, 0` into the same `setAIState` and `setTarget(null)`).
    if (is_friend(o, c.target)) {
        set_ai_state(o, ai_state::kNone, quests, out);
        set_target(o, c, false, UnitRef{}, out);
        return;
    }
    // a unit a `Jump Down Area` allows tries it when its target's centre is a unit
    // below its own; the hunt goes on either way.
    if (c.allow_jump_down) {
        Vec3 target_centre;
        if (!target_at(c.target, target_centre)) return;
        target_centre.y += target_radius(c.target);
        if (target_centre.y < c.motion.position.y - 1.0f) attempt_jump_down(o, c, quests, out);
    }
    // under `BERSERK`, `calculateTargetInSight(3, true)`, and a
    // unit nearer than the target by over 0.1 (3D, fused) becomes the target.
    if (c.ai_flags.has(AIFlag::Berserk)) {
        UnitRef nearer;
        if (find_character_within_view(o, c, /*nearest=*/true, nearer, 3) &&
            !(nearer == c.target)) {
            Vec3 now, then;
            if (target_centre_at(c.target, now) && target_centre_at(nearer, then)) {
                const Vec3& self = c.motion.position;
                const float dx = now.x - self.x, dy = now.y - self.y, dz = now.z - self.z;
                const float nx = then.x - self.x, ny = then.y - self.y, nz = then.z - self.z;
                const float current = std::sqrt(std::fma(dx, dx, std::fma(dz, dz, dy * dy)));
                const float other = std::sqrt(std::fma(nx, nx, std::fma(nz, nz, ny * ny)));
                if (other < current - 0.1f) set_target(o, c, true, nearer, out);
            }
        }
    }
    Vec3 at;
    target_at(c.target, at);
    const float d = distance_xz_fused(feet_of(c), at);
    c.run = stats.near_walk_radius <= d;
    // Giving up: `FOLLOW_RADIUS` widened by `getEffectValue(0x2c, 7)`
    // (`PERCENT ACTIVE DISTANCE BONUS` - `MONSTER_HUNT`'s 200 and the
    // unit's own passive effect), plus the level's `ACTIVERANGEMOD`.
    const float follow = stats.follow_radius;
    const float percent = character_effect(c, 0x2c);
    // only for a unit with no master.
    // `fma(percent, 0.01f, 1) * follow`, plus
    // the level's `ACTIVERANGEMOD`.
    if (std::fma(percent, 0.01f, 1.0f) * follow + active_range_mod_ <= d && !has_master(c)) {
        stop_pathing(c);
        to_idle();
        return;
    }
    const bool sight = valid_line_of_sight(c);
    bool in_range = in_attack_range(c, stats);
    if ((!in_range || !sight) && 0.5f < c.since_route) {
        if (stats.movement.walking_speed == 0.0f && stats.movement.running_speed == 0.0f) {
            set_ai_state(o, ai_state::kIdle, quests, out);
            return;
        }
        if ((stats.ai_type == 3 || stats.ai_type == 1) &&
            volatile_random().integer_between(0, 100) < 10) {
            set_ai_state(o, ai_state::kFlee, quests, out);
            return;
        }
        if (stats.ai_type == 4 && volatile_random().integer_between(0, 100) < 10) {
            set_ai_state(o, 0xb, quests, out);
            const float z = volatile_random().between(at.z - 3.0f, at.z + 3.0f);
            const float x = volatile_random().between(at.x - 3.0f, at.x + 3.0f);
            set_destination(c, stats, x, z);
            return;
        }
        if (!c.pathing || 1.0f <= distance_xz(c.destination, at)) {
            follow_character(o, c, c.target, out);
        }
        if (!c.pathing) {
            set_ai_state(o, ai_state::kIdle, quests, out);
            return;
        }
    }
    in_range = in_attack_range(c, stats);
    if (in_range && sight) {
        set_ai_state(o, ai_state::kAttack, quests, out);
        attack_ai(o, c, 0.0f, quests, out);
        stop_pathing(c);
        return;
    }
    // The skill held, when its property's +0x50 is 2 and it can be cast now:
    // state 0xd and `skillAI` at once.
    if (const UnitSkill* held = active_unit_skill(c)) {
        if (held->property_alignment == 2 && can_cast_current_skill(o, c, false, false, out)) {
            set_ai_state(o, ai_state::kSkill, quests, out);
            skill_ai(o, c, 0.0f, quests, out);
            return;
        }
    }
    if (volatile_random().integer_between(0, 100) < 10) {
        select_offensive_skill(o, c, false, quests, out);
    }
    // A unit with a master gives up a target more than 35 from the master.
    //
    MasterView master;
    if (!master_view(c, master) || !c.has_target) return;
    Vec3 there;
    if (!target_at(c.target, there) || distance_xz(master.at, there) <= 35.0f) return;
    set_target(o, c, false, UnitRef{}, out);
    set_ai_state(o, ai_state::kIdle, quests, out);
}

// `CMonster::attackAI(float, CLevel&)`.
void LevelScript::attack_ai(Object& o, Character& c, float delta, QuestState& quests,
                            std::vector<ScriptEvent>& out) {
    const UnitStats& stats = stats_for(o.unit);
    if (0.0f < c.cooldown) {
        set_ai_state(o, ai_state::kNone, quests, out);
        set_target(o, c, false, UnitRef{}, out);
        return;
    }
    const int32_t their_state = c.has_target ? target_state(c.target) : ai_state::kDead;
    // `CMonster::attackAI`: a near-death pet
    // on either side ends it as a gone target does.
    const bool near_death = pet_near_death(o, c) ||
                            (c.has_target && !c.target.player() && c.target.machine != nullptr &&
                             c.target.machine->unit_near_death(c.target.node));
    if (!c.has_target || !target_alive(c.target) || near_death || their_state == 0x2a ||
        their_state == 0x29) {
        set_ai_state(o, ai_state::kIdle, quests, out);
        set_target(o, c, false, UnitRef{}, out);
        return;
    }
    if (is_friend(o, c.target)) {
        set_ai_state(o, ai_state::kNone, quests, out);
        set_target(o, c, false, UnitRef{}, out);
        return;
    }
    // No attack in use (+0x2c0) or `NO_UNARMED_ATTACKS` (+0x1a5, the
    // no-unarmed-attacks flag):
    // `selectAttack(0)`, and when that leaves it so, a skill or
    // nothing - the held skill and its id (+0x2c4, +0x2d0) cleared first.
    if (!c.has_attack || stats.no_unarmed_attacks) {
        select_attack(c, 0);
        if (!c.has_attack || stats.no_unarmed_attacks) {
            set_active_skill(c, -1);
            select_offensive_skill(o, c, true, quests, out);
            if (active_unit_skill(c) == nullptr) return;
            if (in_skill_range(c) != 0) return;
            cast_skill(o, c, quests, out);
            return;
        }
    }
    Vec3 at;
    target_at(c.target, at);
    const Vec3 here = feet_of(c);
    // `FOLLOW_RADIUS` (+0x2a4) on the ground, the second square fused;
    // out of it the chase is given up.
    if (!(distance_xz_fused(here, at) < stats.follow_radius)) {
        stop_pathing(c);
        set_ai_state(o, ai_state::kIdle, quests, out);
        set_target(o, c, false, UnitRef{}, out);
        return;
    }
    // `turnTowardPosition`, as `turn_toward_target`.
    turn_toward_target(o, c, stats, at, delta);
    // Facing it: a skill before the swing, when one can be cast and the target
    // stands in its range.
    if (facing_target(c.facing, here, at, stats.attack_angle, weapon_arc(c))) {
        set_active_skill(c, -1);
        select_offensive_skill(o, c, true, quests, out);
        if (active_unit_skill(c) != nullptr && in_skill_range(c) == 0) {
            cast_skill(o, c, quests, out);
            return;
        }
    }
    // Attacking: an attack in use and its timer (+0x2ac) running.
    const bool attacking = c.has_attack && 0.0f < c.swing_left;
    if (!in_attack_range(c, stats) || !valid_line_of_sight(c)) {
        if (!attacking) set_ai_state(o, ai_state::kHunt, quests, out);
        return;
    }
    if (!attacking) {
        if (stats.ai_type == 4 && volatile_random().integer_between(0, 100) < 25) {
            set_ai_state(o, 0xb, quests, out);
            const float z = volatile_random().between(at.z - 3.0f, at.z + 3.0f);
            const float x = volatile_random().between(at.x - 3.0f, at.x + 3.0f);
            set_destination(c, stats, x, z);
            return;
        }
        // Facing it: `selectAttack(0)` afresh, then the swing.
        if (facing_target(c.facing, here, at, stats.attack_angle, weapon_arc(c)) &&
            (select_attack(c, 0), attack(o, c, quests, out))) {
            if (const UnitStats::WeaponAttack* weapon = weapon_in_use(c)) {
                c.cooldown = std::max(c.cooldown, 0.0f) + weapon->cooldown;
            }
            c.cooldown = std::max(c.cooldown, 0.0f) + stats.attack_cooldown;
        }
    }
    // `canAttackWithCurrentWeapon` through the vtable.
    if (!can_attack_with_current_weapon(c)) set_ai_state(o, ai_state::kFlee, quests, out);
}

// `CMonster::fleeAI(float, CLevel&)`.
void LevelScript::flee_ai(Object& o, Character& c, QuestState& quests,
                          std::vector<ScriptEvent>& out) {
    set_target(o, c, false, UnitRef{}, out);
    c.run = true;
    if (c.pathing) return;
    // A defensive skill on a 75% draw, when one can be cast.
    if (volatile_random().integer_between(0, 100) < 0x4b) {
        select_defensive_skill(c);
        if (active_unit_skill(c) != nullptr && can_cast_current_skill(o, c, false, false, out)) {
            cast_skill(o, c, quests, out);
            return;
        }
    }
    // Back to idle on the second roll, unless near death or a `FLEE EFFECT` is
    // still held (`hasEffect(0x4f)`): a ratlin made to flee by a
    // death keeps running for the effect's time.
    if (volatile_random().integer_between(0, 100) < 0x4b && !pet_near_death(o, c) &&
        !(0.0f < c.flee_effect_left)) {
        set_ai_state(o, ai_state::kIdle, quests, out);
    }
    // A unit with a master (not `PETNOFOLLOW`) runs to within 16 of it; any
    // other 5 round itself. z is drawn first.
    const UnitStats& stats = stats_for(o.unit);
    MasterView master;
    if (master_view(c, master) &&
        !(unit_types_ != nullptr && unit_types_->isa(stats.unittype, unit_type::kPetNoFollow))) {
        const float z = volatile_random().between(master.at.z - 16.0f, master.at.z + 16.0f);
        const float x = volatile_random().between(master.at.x - 16.0f, master.at.x + 16.0f);
        set_destination(c, stats, x, z);
        return;
    }
    const float z =
        volatile_random().between(c.motion.position.z - 5.0f, c.motion.position.z + 5.0f);
    const float x =
        volatile_random().between(c.motion.position.x - 5.0f, c.motion.position.x + 5.0f);
    set_destination(c, stats, x, z);
}

// `CCharacter::setAllowJumpDown`: `+0x682`; a change runs the AI
// at once (`updateAI(0, true)`, vtable +0x358).
void LevelScript::set_allow_jump_down(Object& o, Character& c, bool allow, QuestState& quests,
                                      std::vector<ScriptEvent>& out) {
    if (c.allow_jump_down == allow) return;
    c.allow_jump_down = allow;
    // The port holds a body's and a spawning unit's AI, as `character_update`.
    if (dead_.count(o.id) != 0 || spawning_.count(o.id) != 0) return;
    monster_update_ai(o, c, 0.0f, quests, out, true);
}

// `CCharacter::attemptJumpDown`, from
// `huntAI`.
bool LevelScript::attempt_jump_down(Object& o, Character& c, QuestState& quests,
                                    std::vector<ScriptEvent>& out) {
    if (!c.has_target || collision_ == nullptr || collision_->empty()) return false;
    // A unit whose model has no `JUMP_DOWN` never jumps (`animationExists`).
    // The renderer answers for the class (`Jump_Down_Armed` or `_Unarmed`),
    // below 0 when the model has none.
    const float down_length = animation_length_ ? animation_length_(o.unit, "JUMP_DOWN") : -1.0f;
    if (!(0.0f <= down_length)) return false;
    const float land_length = animation_length_(o.unit, "JUMP_LAND");
    const bool has_land = 0.0f <= land_length;
    Vec3 target;
    if (!target_at(c.target, target)) return false;
    target.y += target_radius(c.target);
    const float jump_speed = stats_for(o.unit).jump_speed;
    JumpDown jump;
    const bool taken = plan_jump_down(
        *collision_, c.motion.position, c.motion.radius, target, sight_range(o, c),
        [this](const Vec3& p) { return open_position_near(p, 0.0f, 1.0f); }, jump_speed,
        volatile_random(), jump);
    if (!taken) return false;

    // the route stops, and the leave to jump is spent (which runs
    // the AI once more, still hunting, before the jump is laid).
    stop_pathing(c);
    set_allow_jump_down(o, c, false, quests, out);
    c.jumping = true;
    c.jump = jump;
    c.jump_distance = 0.0f;

    // `blendAnimation("JUMP_DOWN", false, 0.05, length / (path length /
    // speed), -1)`: the animation lasts as long as the flight; unless the unit
    // was flung and fell (`+0x681`), `JUMP_LAND` and `IDLE` queued behind it.
    const float flight_time = jump.path.length() / jump.speed;
    blend_reset(c);
    ScriptEvent e;
    e.action = ScriptAction::Animation;
    e.node_id = o.id;
    e.name = o.name;
    e.unit = o.unit;
    e.on = true;
    e.loops = false;
    e.text = "JUMP_DOWN";
    e.blend = 0.05f;
    e.rate = down_length / flight_time;
    const bool fell = c.flown && c.flight.falls;
    if (!fell) {
        e.queue.push_back("JUMP_LAND");
        e.then_idle = true;
    }
    out.push_back(e);
    // DIVERGENCE: the script does not see the model, so `IDLE` is taken to
    // play once the flight and the queued `JUMP_LAND` have run.
    c.jump_idle_at = clock_ + flight_time + (has_land && !fell ? land_length : 0.0f);
    c.gait.clear();
    TL_LOG_INFO("script: %s jumps down from (%.2f, %.2f, %.2f) to (%.2f, %.2f, %.2f)",
                o.name.empty() ? o.unit.c_str() : o.name.c_str(),
                static_cast<double>(c.motion.position.x), static_cast<double>(c.motion.position.y),
                static_cast<double>(c.motion.position.z), static_cast<double>(jump.path.point(2).x),
                static_cast<double>(jump.path.point(2).y),
                static_cast<double>(jump.path.point(2).z));
    set_ai_state(o, ai_state::kJumpDown, quests, out);
    return true;
}

// `CMonster::jumpDownAI`, AI state 0x10: along the path,
// turned toward the landing over the first half; `LAND_SOUND`, then the hunt once `IDLE`
// plays. DIVERGENCE: `setAnimationTime` is not sent; the animation's rate matches the
// flight.
void LevelScript::jump_down_ai(Object& o, Character& c, float delta, QuestState& quests,
                               std::vector<ScriptEvent>& out) {
    if (c.jump.path.size() < 3) return;
    const float before = c.jump_distance;
    const float length = c.jump.path.length();
    const UnitStats& stats = stats_for(o.unit);
    if (c.jump_distance < length * 0.5f) {
        // `turnTowardPosition` toward the
        // landing.
        const Vec3 landing = c.jump.path.point(2);
        float dx = landing.x - c.motion.position.x;
        float dz = landing.z - c.motion.position.z;
        const float dy = landing.y - c.motion.position.y;
        const float l = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (1e-08f < l) {
            dx = dx / l;
            dz = dz / l;
        }
        c.facing = turn_toward(c.facing, dx, dz, stats.turn_rate, delta, nullptr, turn_factor(c));
    }
    c.jump_distance = delta * c.jump.speed + c.jump_distance;
    if (length <= c.jump_distance) {
        if (length <= before) {
            c.jumping = false;
            if (clock_ < c.jump_idle_at) return;
            set_ai_state(o, ai_state::kHunt, quests, out);
            return;
        }
        c.jump_distance = length;
        // `LAND_SOUND` (slot 0x11), then `queueBlendAnimation("IDLE", true,
        // 0.2, 1)`, which the jump's queue already ends with.
        ScriptEvent e;
        e.action = ScriptAction::JumpLanded;
        e.node_id = o.id;
        e.name = o.name;
        e.unit = o.unit;
        e.position = feet_of(c);
        out.push_back(e);
    }
    c.motion.position = c.jump.path.spline_position_at_distance(c.jump_distance);
    o.position = feet_of(c);
}

// `CMonster::updateAI(float, bool)`.
void LevelScript::monster_update_ai(Object& o, Character& c, float delta, QuestState& quests,
                                    std::vector<ScriptEvent>& out, bool force) {
    // The AI's own time, once a character: the forced calls are its own re-entries.
    const std::optional<SectionScope> timed =
        force ? std::nullopt : std::optional<SectionScope>(std::in_place, Section::Ai);
    c.cooldown -= delta;
    // `performingSkill`: a cast still holding the unit outside the skill states
    // is stopped (`stopAllSkills(false, false, false)`).
    if (const UnitSkill* held = active_unit_skill(c)) {
        const SkillRunState* state = c.casting.runner.state(held->known.entry.name);
        const bool casting =
            still_casting(c.casting.cast, state != nullptr ? state->animations : SkillAnimations{},
                          c.casting.runner.is_running(held->known.entry.name));
        if (casting && o.ai != ai_state::kPerformingSkill && o.ai != ai_state::kSkill &&
            unit_skills_.stop_all) {
            unit_skills_.stop_all(*this, o.id, /*force=*/false);
        }
    }
    if (o.ai != 0x2c && o.ai != ai_state::kPlayAnimation && o.path == 0) {
        const float was = c.think;
        c.think = delta + was;
        // not while it jumps (`+0x680`).
        if (!force && delta + was < kAiIntervals[c.think_index] && !c.jumping) return;
    } else {
        c.think = delta + c.think;
    }
    c.think = 0.0f;
    if (c.has_target && (!target_alive(c.target) || target_inactive(c.target))) {
        set_target(o, c, false, UnitRef{}, out);
    }
    // Alive and the level dormant (`CLevel::isDormant`, the client's
    // `+0x10bc`) or the unit disabled.
    if (o.ai != ai_state::kDying && o.ai != ai_state::kDead && (client_paused() || !o.enabled)) {
        const bool attacking = 0.0f < c.swing_left;
        if (!attacking && o.ai != ai_state::kIdle && o.ai != ai_state::kPlayAnimation &&
            o.ai != 0x10 && o.ai != 0x29 && o.ai != 0x2a && o.ai != 0x28) {
            set_ai_state(o, ai_state::kIdle, quests, out);
        }
    }
    switch (o.ai) {
        case ai_state::kNone:
            if (!(0.0f < c.swing_left)) set_ai_state(o, ai_state::kIdle, quests, out);
            return;
        case ai_state::kFidget: fidget_ai(o, c, quests, out); return;
        case ai_state::kIdle: idle_ai(o, c, delta, quests, out); return;
        case ai_state::kAttack: attack_ai(o, c, delta, quests, out); return;
        case ai_state::kHunt: hunt_ai(o, c, quests, out); return;
        case ai_state::kFlee: flee_ai(o, c, quests, out); return;
        case ai_state::kGoingToTown: return_to_town_ai(o, c, quests, out); return;
        case ai_state::kPlayAnimation:
            // `CCharacter::animationPlayAI`: once neither playing
            // (`CGenericModel::animationPlaying`) nor queued (`animationQueued`),
            // state 0 and `updateAI(delta, true)` again at once.
            if (!o.animation_loops) {
                o.animation_left -= delta;
                if (o.animation_left <= 0.0f) {
                    o.animation.clear();
                    set_ai_state(o, ai_state::kNone, quests, out);
                    monster_update_ai(o, c, delta, quests, out, /*force=*/true);
                }
            }
            return;
        case 0xb:
            // `CMonster::getRangeAI(float, CLevel&)`: run to the
            // point; there, the hunt with a target and the idle without.
            c.run = true;
            if (!c.pathing) {
                set_ai_state(o, c.has_target ? ai_state::kHunt : ai_state::kIdle, quests, out);
            }
            return;
        case ai_state::kSkill: skill_ai(o, c, delta, quests, out); return;
        case ai_state::kPerformingSkill: performing_skill_ai(o, c, delta, quests, out); return;
        case ai_state::kSkillApproach: skill_approach_ai(o, c, delta, quests, out); return;
        case ai_state::kJumpDown: jump_down_ai(o, c, delta, quests, out); return;
        case 0x1d:
            // `CCharacter::interruptAI(float, CLevel&)`
            // (vtable +0x390): back to state 0
            // unless an animation whose name holds `HIT` is still playing.
            if (!(0.0f < c.hit_left)) set_ai_state(o, ai_state::kNone, quests, out);
            return;
        case 0x28:
            // `CCharacter::immobileAI(float)`: held where it is
            // while `IMMOBILIZE` (0x3f) or the velocity scale is 0; otherwise state 0.
            if ((c.effects && c.effects->has_effect(0x3f)) || c.motion.velocity_scale == 0.0f) {
                c.motion.velocity_scale = 0.0f;
                c.motion.velocity = {0.0f, 0.0f, 0.0f};
            } else {
                set_ai_state(o, ai_state::kNone, quests, out);
            }
            return;
        default: return;
    }
}

// ---------------------------------------------------------------------------
// The frame
// ---------------------------------------------------------------------------

// `CCharacter::update(Camera*, Vector3 const&, float)`.
//
void LevelScript::character_update(Object& o, Character& c, float delta, QuestState& quests,
                                   std::vector<ScriptEvent>& out) {
    const UnitStats& stats = stats_for(o.unit);
    // The fade steps first, before the town test.
    step_opacity(o, c, delta, false, out);
    // In town (0x2a): hidden (the scene's part), off the avoidance map, and
    // nothing else but the town clock, while a game is being played
    // (`(level +0x220)->+0x38d0 == 1`).
    if (o.ai == ai_state::kInTown && game_playing_) {
        avoidance_remove(c);
        update_in_town_timer(o, c, delta, quests, out);
        if (o.ai != ai_state::kInTown) emit_character(o, c, out);
        return;
    }
    // `calculateActiveRange` against the
    // point the camera looks at: active, near, and the think interval's band.
    calculate_active_range(o, c, level_camera_look_at(), out);
    // `characterShouldFadeOut` is a body's; `fade` is what the camera test turns
    // off, and an inactive character's path sets it.
    bool fade = false;
    if (!c.active) {
        // an inactive character fades out, and once it is unseen it
        // leaves the avoidance map, drops its target and goes no further.
        want_visible(c, false);
        fade = true;
        if (!o.visible) {
            avoidance_remove(c);
            set_target(o, c, false, UnitRef{}, out);
            return;
        }
    } else if (!stats.render_always && !o.visible && !o.enabled) {
        // an active character unseen and disabled is put back in view
        // when the camera sees it, hidden at once when it does not, and waits.
        if (in_view(o)) {
            if (!c.mesh_hidden) want_visible(c, !fade);
        } else {
            set_character_visible(o, c, false, true, out);
        }
        avoidance_remove(c);
        set_target(o, c, false, UnitRef{}, out);
        return;
    }
    c.since_route = std::min(100.0f, c.since_route + delta);
    if (0.0f < c.flee_effect_left) c.flee_effect_left = std::max(0.0f, c.flee_effect_left - delta);
    c.fidget_timer = std::min(100.0f, c.fidget_timer + delta);
    c.fidget_wait = std::min(100.0f, c.fidget_wait + delta);
    if (0.0f < c.fidget_left) c.fidget_left = std::max(0.0f, c.fidget_left - delta);
    c.since_replan = std::min(100.0f, c.since_replan + delta);

    if (0.0f < c.hit_left) c.hit_left = std::max(0.0f, c.hit_left - delta);
    if (0.0f < c.swing_left) {
        c.swing_left -= delta;
        if (c.swing_left <= 0.0f) {
            if (!c.struck && o.ai != ai_state::kDying && o.ai != ai_state::kDead) {
                c.struck = true;
                perform_attack(o, c, quests, out);
            }
            c.swing_left = 0.0f;
            c.swing.clear();
        }
    }

    avoidance_remove(c);
    load_unit_skills(c);
    // `CCharacter::update`: the
    // procs' lockout runs down.
    if (0.0f < c.proc_lockout) c.proc_lockout -= delta;
    // and a transformation's time: at 0 `CCharacter::revertTransformation`.
    // A permanent one (-999) never runs down.
    if (0.0f < c.transform_left && c.transform_left != -999.0f) {
        c.transform_left -= delta;
        if (c.transform_left <= 0.0f) {
            const int64_t id = o.id;
            const std::string back = c.transformed_from;
            c.transform_left = 0.0f;
            if (!back.empty() && convert_character(id, back, quests, out)) {
                if (Character* reverted = character(id)) reverted->transformed_from.clear();
            }
            return;
        }
    }
    // `CCharacter::updateMana`: max / 100 times the global
    // rate, then `MANA RECHARGE` (6) and `MANA RECHARGE PLAYER` (0x7b), two `modifyMana`
    // calls each held to [0, max].
    if (c.casting.loaded && o.ai != ai_state::kDying && o.ai != ai_state::kDead) {
        const float ceiling = static_cast<float>(c.casting.max_mana);
        const auto modify_mana = [&](float change) {
            c.casting.mana = c.casting.mana + change;
            if (ceiling < c.casting.mana) c.casting.mana = ceiling;
            if (c.casting.mana < 0.0f) c.casting.mana = 0.0f;
        };
        // ((max * rate) * dt) * 0.01f.
        modify_mana(((ceiling * unit_skills_.mana_recharge_rate) * delta) * 0.01f);
        const CharacterEffects total(c.effects.get(), c.inventory);
        const float recharge = total.effect_value(6);
        const float player = total.effect_value(0x7b);
        modify_mana((player + recharge) * delta);
    }
    // `displayDamageAbsorbed(delta)` (the call, after `updateMana`).
    display_unit_damage(o, c, delta, out);
    // `CAIManager::update`: flag, skill, and watcher steps.
    c.ai_flags.update(delta);
    c.ai_skills.update(delta);
    for (auto& watcher : c.ai_watchers) {
        watcher.update(delta);
    }
    if (spawning_.count(o.id) == 0) monster_update_ai(o, c, delta, quests, out);
    if (dead_.count(o.id) != 0) return;
    if (delta == 0.0f) {
        avoidance_add(c);
        return;
    }

    // Turning toward the target when not on a route.
    if (c.has_target && !c.pathing && o.ai != 0xc && o.ai != ai_state::kDying &&
        o.ai != ai_state::kDead && o.ai != 0x29 && o.ai != 0x10) {
        Vec3 at;
        if (target_at(c.target, at)) {
            c.destination = {at.x, 0.0f, at.z};
            float dx = at.x - c.motion.position.x;
            float dz = at.z - c.motion.position.z;
            const float dy = at.y + c.motion.radius - c.motion.position.y;
            const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (1e-08f < length) {
                dx = dx / length;
                dz = dz / length;
            }
            c.facing =
                turn_toward(c.facing, dx, dz, stats.turn_rate, delta, nullptr, turn_factor(c));
        }
    }
    // Back to the way it was placed, once a conversation has let it go
    // (release_player_target()'s DIVERGENCE).
    if (c.face_home) {
        if (c.has_target || c.pathing) {
            c.face_home = false;
        } else {
            bool turned = false;
            c.facing = turn_toward(c.facing, std::sin(c.home_facing), std::cos(c.home_facing),
                                   stats.turn_rate, delta, &turned, turn_factor(c));
            if (!turned) {
                c.facing = c.home_facing;
                c.face_home = false;
            }
        }
    }

    update_pathing(c, stats, delta);
    if (collision_ != nullptr && !collision_->empty()) {
        c.motion.pathing = c.pathing;
        const Vec3 was = c.motion.position;
        timed(Section::Motion, [&] { update_motion(delta, *collision_, c.motion, passability_); });
        // `updateMotion`: a move ended against a wall on a route. Far from the
        // destination, after a re-plan interval (1), the contact point goes on the object grid
        // (radius + 0.4) while `setDestination` routes again; near it, the route is dropped.
        if (c.motion.against_wall && c.pathing) {
            const float dx = was.x - c.destination.x;
            const float dz = was.z - c.destination.z;
            const float r = c.motion.radius;
            if (r + r < std::sqrt(dx * dx + 0.0f + dz * dz)) {
                constexpr float kReplanInterval = 1.0f;
                if (kReplanInterval <= c.since_replan && c.since_replan != kReplanInterval) {
                    const float around = r + 0.4f;
                    const Vec3& p = c.motion.wall_point;
                    const bool grid = passability_ != nullptr && passability_->ready();
                    if (grid) {
                        passability_->increment_object_passability(
                            {p.x - around, p.y + 0.0f, p.z - around},
                            {p.x + around, p.y + 0.0f, p.z + around}, around);
                    }
                    const Vec3 moved = c.motion.position;
                    c.motion.position = was;
                    set_destination(c, stats, c.destination.x, c.destination.z);
                    c.motion.position = moved;
                    if (grid) {
                        passability_->decrement_object_passability(
                            {p.x - around, p.y + 0.0f, p.z - around},
                            {p.x + around, p.y + 0.0f, p.z + around}, around);
                    }
                    c.since_replan = 0.0f;
                }
            } else {
                if (c.pathing) c.since_route = 10.0f;
                c.pathing = false;
                c.next = was;
            }
        }
    } else {
        // INVENTED: a level with no collision (a unit test) moves the sphere
        // straight along its velocity.
        c.motion.position.x += c.motion.velocity.x * c.motion.velocity_scale * delta;
        c.motion.position.z += c.motion.velocity.z * c.motion.velocity_scale * delta;
        c.motion.velocity = {0.0f, 0.0f, 0.0f};
    }
    avoidance_add(c);
    // unless `RENDERALWAYS`, the camera test (`Camera::isVisible` on
    // the model's world bounds): in view, the fade's goal follows
    // `fade`; out of it, the character is hidden at once.
    if (!stats.render_always) {
        if (in_view(o)) {
            if (!c.mesh_hidden) want_visible(c, !fade);
        } else {
            set_character_visible(o, c, false, true, out);
        }
    }
    // the update's last call: the leave to jump is the area's to give
    // again after this update.
    set_allow_jump_down(o, c, false, quests, out);
    emit_character(o, c, out);
}

}  // namespace tl::core
