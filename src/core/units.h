// A unit's authored numbers from its `[UNIT]` `.DAT` (speeds, radii, stats, skills, items), kept
// by `NAME`. No OGRE and no file I/O: the archive is read by whoever calls add().
#pragma once

#include "core/ai_stat_watcher.h"
#include "core/character_particles.h"
#include "core/effects.h"
#include "core/sound_bank.h"
#include "formats/adm.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tl::core {

/// `EDAMAGE_TYPES` as the data keys spell them (`gDAMAGE_TYPES`, upper-cased):
/// 0 physical, 1 magical, 2 fire, 3 ice, 4 electric, 5 poison, 6 all.
inline constexpr int kDamageTypeCount = 7;
inline constexpr const char* kDamageTypeKeys[kDamageTypeCount] = {
    "PHYSICAL", "MAGICAL", "FIRE", "ICE", "ELECTRIC", "POISON", "ALL"};

/// The speeds a unit's `.DAT` authors, in level units per second. Defaults: the player base's.
struct MovementStats {
    float walking_speed = 2.5f;
    float running_speed = 6.25f;
};

/// The radius for a person-sized unit that authors no `COLLISION_RADIUS`: the commonest
/// authored value.
inline constexpr float kDefaultUnitRadius = 0.8f;

/// One of a unit's attacks, as its own skill file (`media/skills/`) authors it: a monster's
/// attack animation is the skill's `ANIMATION`.
struct AttackSkill {
    /// `NAME`, as the unit's `[SKILL]` block spells it.
    std::string name;

    /// `ANIMATION` - the skeleton beside the unit's mesh to play.
    std::string animation;

    /// `RANGE` - how far the skill reaches; zero when unauthored (the unit's reach decides).
    float range = 0.0f;
};

/// `CAttackDescription` (0x80 bytes, constructor):
/// one way a character swings.
struct AttackDescription {
    std::string animation = "ATTACK";  ///< +0x10, the animation-name prefix
    int max_damage = 0;                ///< +0x24[0], physical
    int min_damage = 0;                ///< +0x40[0]
    float range = 0.375f;              ///< +0x68
    float strike_range = 0.5f;         ///< +0x6c
    float speed = 1.0f;                ///< +0x70, the swing's speed divisor
    int damage_type = 0;               ///< +0x78
    /// +0x24[1..6], the attack's damage of each other type (`setLevel`'s
    /// `DAMAGE_<TYPE>` loop); index 0 is `max_damage`.
    std::array<int, 7> damage_by_type{};
    /// `MINDAMAGE` and `MAXDAMAGE` off the difficulty's health curve, halved, as `setLevel`
    /// writes them on its attack.
    int health_min_half = 0;
    int health_max_half = 0;
};

/// One key of a character's `[EQUIPMENT]` group, as `CCharacter::createDefaultEquipment`
/// reads them: an item to make and put on (`RIGHTHAND`
/// ... `WEAPON`), or a spawn class to roll for one (`SPAWNRIGHTHAND` ... `SPAWNCLASS`).
struct DefaultEquipment {
    std::string name;
    /// The equip location; 12 (`WEAPON`, `SPAWNCLASS`) is the item's first free one.
    int location = 0;
    bool spawn_class = false;
};

/// One unit's authored numbers.
struct UnitStats {
    /// `DISPLAYNAME` - what a person calls this unit (`NAME` is the data's join key).
    std::string display_name;

    /// `DESCRIPTION` - the line under a target's health bar (`TargetDescription`).
    std::string description;

    /// `PORTRAIT` - the dialogue head, an `itemicons.imageset` name; `CQuestDialogMenu::setNPC`
    /// hides the frame when it is empty.
    std::string portrait;

    MovementStats movement;

    /// `SCALE`, 1 when unauthored.
    float scale = 1.0f;
    /// `SCALE_VARIATION`, 0 when unauthored: `CBaseUnit::unitInit`
    /// makes each unit `SCALE + randomBetweenVolatile(0, SCALE_VARIATION)`.
    float scale_variation = 0.0f;

    /// `COLLISION_RADIUS`, `+0x194`, default 0.3 (`CCharacter::unitInit`): the collision
    /// sphere, and with 0.4 added the object-grid circle while it stands
    /// (`CBaseUnit::addToAvoidanceMap`).
    float radius = 0.3f;

    /// `SIGHT_RADIUS`, `+0x360`, default 7 (`unitInit`): how far the unit looks for
    /// something to fight, inside its `VIEW_ANGLE`.
    float sight_radius = 7.0f;

    /// `MOTION_RADIUS`, `+0x36c`, default 4.5 (`unitInit`): noticed whatever the facing.
    float motion_radius = 4.5f;

    /// `DORMANTNAME`: a waiting mimic's name; when empty `CCharacter::getMimicName`
    /// says `Chest`.
    std::string dormant_name;

    /// `VIEW_ANGLE`, `+0x368`, default 90 (`unitInit`): degrees either side of facing.
    float view_angle = 90.0f;

    /// `FOLLOW_RADIUS`, `+0x370`, default 18 (`unitInit`): how far a held target may
    /// get before the hunter gives it up.
    float follow_radius = 18.0f;

    /// `REACH_BONUS`, `+0x4e0`: added (times the unit's scale) to the attack's
    /// own range and strike range. `CCharacter::unitInit` default 0.75.
    float reach = 0.75f;

    /// `ATTACK_RANGE` (X360 `+0x42c`, default 0.375) and `STRIKE_RANGE` (X360 `+0x428`, default
    /// 0.5). Read after `setLevel` built the unit's own `ATTACK`, so that attack never takes them.
    float attack_range = 0.375f;
    float strike_range = 0.5f;

    /// `RANGE_MULTIPLIER`, `+0x4dc`, default 1.
    float range_multiplier = 1.0f;

    /// `TURN_RATE`, `+0x288`, default 10: how fast `turnToward` closes a
    /// heading, per degree off.
    float turn_rate = 10.0f;

    /// `NEAR_WALK_RADIUS`, `+0x364`, default 0: `huntAI` walks rather than runs
    /// inside it.
    float near_walk_radius = 0.0f;

    /// `DAMAGE_REACT_RADIUS`, `+0x374`, default 2.
    float damage_react_radius = 2.0f;

    /// `BRAVERY`, `+0x668`, default 1.
    float bravery = 1.0f;

    /// `ATTACKANGLE`, `+0x76c`, default 60: `facingTarget` allows half of it either side.
    float attack_angle = 70.0f;

    /// `AI_ATTACKCOOLDOWN`, `CMonster +0x7ec`, default 0.
    float attack_cooldown = 0.0f;

    /// `LEVEL`, `+0x100`, default 1 (`CBaseUnit`'s constructor).
    int level = 1;
    /// Whether the data carries `LEVEL`: `unitInit` defaults it
    /// to the level the unit was made at.
    bool level_authored = false;

    /// `AITYPE`, `+0x334`: `KAITYPE_STRINGS` NORMAL 0, RANGEDDEFENDER 1,
    /// DEFENDER 2, RANGEDCASTER 3, CIRCLER 4, RESURRECTER 5, DUMMY 6.
    int ai_type = 0;

    /// `ARMOR`, a percentage of the armour curve (`setLevel`), default 0; and
    /// `ARMOR_PHYSICAL`, a percentage of that, -1 when not authored.
    int armor = 0;
    int armor_physical = -1;
    /// `ARMOR_MAGICAL` .. `ARMOR_ALL` by `EDAMAGE_TYPES` (index 0 is
    /// `armor_physical`), percentages of the same armour, -1 when not authored.
    std::array<int, kDamageTypeCount> armor_by_type{-1, -1, -1, -1, -1, -1, -1};
    /// `DAMAGE_PHYSICAL` .. `DAMAGE_ALL` by `EDAMAGE_TYPES`: percentages of the
    /// attack's maximum damage (`setLevel`), -1 when not authored.
    std::array<int, kDamageTypeCount> damage_by_type{-1, -1, -1, -1, -1, -1, -1};

    /// `SET_HP_PCT`, default 1: the share of its hit points a unit starts with.
    float set_hp_pct = 1.0f;

    /// `NO_UNARMED_ATTACKS`, `+0x1f5`.
    bool no_unarmed_attacks = false;

    /// `DAMAGEABLE`, `+0x52f`, default true, and `INVINCIBLE`, `+0x52e`,
    /// default false, each as authored (`invincible` below is either).
    bool damageable = true;
    bool invincible_flag = false;

    /// `OCCUPIESNODES`, `+0x18f`, default true.
    bool occupies_nodes = true;

    /// `DISPLAYDMG`, `+0x774` (`CCharacter::unitInit`), default true:
    /// the damage numbers over the unit (`displayDamageAbsorbed`).
    ///
    bool display_dmg = true;

    /// `DIES_ON_WARP` (`CCharacter::diesOnWarp`; inlined in `killPets`):
    /// a pet of the player that does is killed before a warp.
    bool dies_on_warp = false;

    /// `MINION_SPAWNCLASS`, `+0x6b8` (`unitInit`): rolled once into pets of its own as the
    /// level takes the character in (`createPathfinder`).
    std::string minion_spawnclass;

    /// `ATTACHES_TO_MASTER`, read off the data group by
    /// `CCharacter::attachesToMaster`,
    /// default false: a pet that keeps no station of its own and takes the
    /// nearest target it sees.
    bool attaches_to_master = false;

    /// `DESTROY_ON_DEATH`, read off the data group by
    /// `CCharacter::getDestroyOnDeath`, default false: the unit is
    /// deleted once its death is complete and leaves no body.
    bool destroy_on_death = false;
    /// `RENDERALWAYS` (`CCharacter +0x773`, `unitInit`): `update`
    /// neither puts it back in view nor makes it wait unseen.
    bool render_always = false;
    /// `BOSS_CHAMPION` (`CCharacter +0x772`): active and near wherever it stands
    /// (`calculateActiveRange`). A `RENDERALWAYS` unit without it,
    /// once hidden, is never wanted back in view.
    bool boss_champion = false;
    /// `CHAMPION` (`CCharacter::unitInit`): made a champion with
    /// a generated name and the `CHAMPION` unit theme.
    bool champion = false;
    /// The unit's `[EQUIPMENT]` as `createDefaultEquipment` reads it
    /// (`read_default_equipment`).
    std::vector<DefaultEquipment> default_equipment;
    /// The unit's own `[EFFECT]`s (`unit_own_effects`), as authored; each
    /// character rolls its own copies as it is made (`roll_unit_creation`).
    std::vector<Effect> own_effects;
    /// The values of the unit's `[AFFIXES]` (the first group of the name), each
    /// an affix `CBaseUnit::reapplyAffixes` adds to it.
    std::vector<std::string> authored_affixes;
    /// `UNIQUENAME` (`CCharacter::makeChampion`): the champion
    /// keeps its own name.
    bool unique_name = false;
    /// `MINHP` and `MAXHP` as `makeChampion` reads them, 0 when not authored
    /// (`setLevel` reads 1).
    float champion_min_health = 0.0f;
    float champion_max_health = 0.0f;
    /// `SHADOWS` (`CBaseUnit +0x1a9`, `CBaseUnit::unitInit`),
    /// default true: the unit may cast a shadow
    /// (`calculateActiveRange`'s `+0x530`).
    bool shadows = true;

    /// `PARTICLE_GETHIT`, `PARTICLE_DEATH`, `PARTICLE_EXPLODE`, `PARTICLE_SPAWN`
    /// and the three `_CENSOR` twins, read off the data group by
    /// `CCharacter::loadModel` into the list
    /// at `+0x4a8`. Names of `media/particles/preload.dat`, upper-cased.
    UnitParticles particles;

    /// `WALK_ANIM_MULT` (`+0x328`) and `RUN_ANIM_MULT` (`+0x32c`), read by
    /// `unitInit` with a default of 1: what the gait animation's
    /// speed is multiplied by (`updateAnimation`).
    float walk_anim_mult = 1.0f;
    float run_anim_mult = 1.0f;

    /// `JUMP_SPEED`, read off the data group by `CCharacter::attemptJumpDown`
    /// with a default of 1: the jump down's speed is multiplied by it.
    float jump_speed = 1.0f;

    /// `MAXIMUM_PET_INSTANCES`, read off the unit's data group by
    /// `CCharacter::getMaximumPetInstances`
    /// with a default of 5: how many pets of this one name a master keeps
    /// (`addPet`).
    int maximum_pet_instances = 5;

    /// `COLLISIONFILE` is authored: the unit has a collision model.
    bool has_collision_model = false;

    /// `PERCENT ACTIVE DISTANCE BONUS` (effect 0x2c) from the unit's own passive
    /// `[EFFECT]`s, summed.
    float active_distance_bonus = 0.0f;

    /// The attack of the weapon in the unit's right hand, as
    /// `CEquipment::calculateCombatStats`
    /// builds it (`RSLASH`, `POLEARM`, ...: the item's `RANGE`, `STRIKERANGE`
    /// and `SPEED` / 100); `present` is false for a unit that fights with its
    /// own `ATTACK`.
    struct WeaponAttack {
        bool present = false;
        AttackDescription attack;
        /// `AI_ATTACKCOOLDOWN` on the item (`+0x408`).
        float cooldown = 0.0f;
        /// `MISSILE` (`CEquipment +0x400`): what `CEquipment::fireMissiles`
        /// looses; empty for a weapon that
        /// strikes.
        std::string missile;
        /// The item `ISA` `Ranged Weapon` (0x23) / `Melee` (0x3c).
        bool ranged = false;
        bool melee = false;
        /// of the item: the arc `attack` searches a mixed
        /// pair of hands with (`console_weapon_arc`), 0 for none.
        float arc = 0.0f;
    };
    /// The right hand's weapon (`RIGHTHAND`), its attack for that hand
    /// (`CEquipment +0x238`).
    WeaponAttack weapon_attack;
    /// The left hand's (`LEFTHAND`), its attack for the left hand (`+0x23c`):
    /// an archer's bow, the second of two weapons.
    WeaponAttack left_weapon_attack;

    /// Data-driven AI stat watchers authored in [AI] [STAT] groups.
    /// CAIManager::initalize.
    std::vector<AIStatWatcherDef> ai_watchers;

    /// `ALIGNMENT`: three sides, and no `ALIGNMENT` is hostile. `NEUTRAL` is not friendly: it
    /// fights nobody and nobody fights it; `fights()` below is what asks.
    enum class Alignment : uint8_t {
        Hostile,   ///< `EVIL`, or - far more often - no `ALIGNMENT` at all.
        Friendly,  ///< `GOOD`, and `UNITTYPE:PET`, which authors no alignment.
        Neutral,   ///< `NEUTRAL`: ambient wildlife, on no side at all.
    };
    Alignment alignment = Alignment::Hostile;

    /// Neither is true of a NEUTRAL unit.
    // PORT: an accessor of the port's own bookkeeping.
    bool hostile() const { return alignment == Alignment::Hostile; }
    bool friendly() const { return alignment == Alignment::Friendly; }

    /// `TARGETABLE` on the unit's own `.DAT`, not a layout node's
    /// (`LevelScript::node_targetable()`).
    bool targetable = true;

    /// `INVINCIBLE`, or `DAMAGEABLE:False`: nothing may take this unit's health down.
    bool invincible = false;

    /// `ATTACKSPEED`, a percentage of the ordinary swing rate (50 attacks half as often).
    float attack_speed = 100.0f;

    /// `MINHP` and `MAXHP`: percentages of the level health curve, not hit points.
    float min_health = 100.0f;
    float max_health = 100.0f;

    /// `MINDAMAGE` and `MAXDAMAGE`, percentages of `DAMAGE_MONSTER` the same
    /// way - unless `USEWEAPONDAMAGE`, in which case what is in the unit's hand
    /// decides and these scale that instead.
    float min_damage = 100.0f;
    float max_damage = 100.0f;

    /// `XP` on monster units, a percentage of `EXPERIENCE_MONSTER` (or
    /// `EXPERIENCE_CHAMPIONMONSTER`).
    int xp = 100;

    /// What the unit leaves when it dies (`CCharacter::unitInit`):
    /// the `[TREASURE]` group's `SPAWNCLASS`
    /// (+0x6c0, empty for none; `NONE` names no class and so is none too) and
    /// its `MIN`/`MAX`, each default 1 (+0x6c8, +0x6cc); `DONTDROPGOLD`
    /// (+0x703), default false.
    std::string treasure_class;
    int treasure_min = 1;
    int treasure_max = 1;
    bool dont_drop_gold = false;
    /// A merchant's four keys over the `BASEFILE` chain (`read_merchant_data`), original defaults.
    float merchant_level_factor = 0.0f;
    bool allows_merch_uniques = false;
    bool allows_merch_magic = true;
    int merchant_tab = 0;
    /// `SPAWNER`: the editor model the unit is released from. Named, a spawner puts the unit on its
    /// own point
    /// (`CUnitSpawner +0x1c3`, `setResourceSpawn`).
    std::string spawner_model;

    /// Whether this unit is made a champion as it is made: its data says
    /// `CHAMPION` or `BOSS_CHAMPION`, on which `CCharacter::unitInit`
    /// runs `makeChampion`. A champion a spawn class or a
    /// floor's population makes of an ordinary monster is not one of these
    /// (`spawn_marked_champion`).
    bool is_champion = false;

    /// `USEWEAPONDAMAGE`. False for a plain monster, true for every player.
    bool use_weapon_damage = false;

    /// `INTRO_CINEMATIC` (`CCharacter::getIntroCinematic`):
    /// the cinematic a new character of this class
    /// opens with.
    std::string intro_cinematic;

    /// `[EQUIPMENT]`: what the unit is holding when it is created.
    std::string right_hand;
    std::string left_hand;
    /// `[EQUIPMENT]`'s `SPAWNRIGHTHAND` and `SPAWNLEFTHAND`: a spawn class
    /// `CCharacter::createDefaultEquipment`
    /// rolls for each character as it is made, the first unit of the roll going
    /// in that hand.
    std::string spawn_right_hand;
    std::string spawn_left_hand;

    /// `WEAPON_SCALE` - how big what it holds is drawn; 1 when unauthored.
    float weapon_scale = 1.0f;
    /// `SHIELD_SCALE`, the same for a shield (`attachToGivenLocation`).
    float shield_scale = 1.0f;

    /// `COLLIDEABLE`, default true: false makes the unit walkable (posed corpses); a unit dying in
    /// play leaves the grid by `CCharacter::removeFromAvoidanceMap()`.
    bool collideable = true;

    /// `ARMED`: picks the `_Armed` or `_Unarmed` animation set (resolve_unit_animation()).
    bool armed = true;

    /// The `[SKILL]` blocks the unit's `.DAT` lists, resolved through the skill files; empty for
    /// a unit with none, which keeps a peaceable NPC from swinging.
    std::vector<AttackSkill> attacks;

    /// `UNITTYPE` - what kind of thing this is. `PET` is what makes a pet, which authors no
    /// `ALIGNMENT`, friendly.
    std::string unittype;

    /// `[QUESTS]`: the quests the unit offers when no dialogue names it
    /// (`CQuestManager::getQuestForNPC`'s last resort).
    std::vector<std::string> quests;

    /// The unit file's `[THEMES]` blocks, the base's first, under an otherwise empty root, as
    /// `CBaseUnit::unitInitThemes` reads them.
    tl::formats::AdmSection themes_root;

    /// Whether this unit is the player's animal (`UNITTYPE:PET`).
    bool is_pet() const;

    /// `RESOURCEDIRECTORY` + `MESHFILE`, joined, without an extension - the model this unit is
    /// drawn with. Both are inherited through `BASEFILE`.
    std::string mesh;

    /// `HEALTH_GRAPH`, `DAMAGE_GRAPH`, `ARMOR_GRAPH` - the curves this unit is a percentage of, by
    /// name; empty falls back to the monster curves.
    std::string health_graph;
    std::string damage_graph;
    std::string armor_graph;

    /// The sounds the unit's `.DAT` names, by sound definition name, in the 67 slots
    /// `CCharacter::loadModel` fills from `KSOUNDBANK_STRINGS`. An `ATTACKSOUND`/`FOOTSTEP`
    /// animation tag usually means "whatever this unit authors" here.
    SoundBank sounds;

    /// Named slots; everything else goes through `sounds[SoundSlot::...]`.
    // PORT: an accessor of the port's own bookkeeping.
    const std::string& attack_sound() const { return sounds[SoundSlot::Attack]; }
    const std::string& strike_sound() const { return sounds[SoundSlot::Strike]; }
    const std::string& idle_sound() const { return sounds[SoundSlot::Idle]; }
    const std::string& roar_sound() const { return sounds[SoundSlot::Roar]; }
    // PORT: an accessor of the port's own bookkeeping.
    const std::string& step_sound() const { return sounds[SoundSlot::Step]; }
    const std::string& miss_sound() const { return sounds[SoundSlot::Miss]; }

    /// `CANMATCHSPEED`: the unit moves at whoever it follows rather than at its own rate.
    bool can_match_speed = false;

    /// `SPAWN_ANIM`, default `SPAWN` (`CCharacter::spawn()`):
    /// the entrance a spawner releases the unit with. An animation class,
    /// matched as a substring and rolled among its matches
    /// (`CGenericModel::animationExistsSubstring()`), as `FIDGET` is.
    std::string spawn_anim;
    /// `SPAWN_INVISIBLE`: `spawn` starts the entrance at opacity 0 (`+0x528 = 0`,
    /// `updateOpacity(0, true)`), and the unit fades in from there.
    bool spawn_invisible = false;
    /// `RENDERBEHIND` (`CCharacter +0x4a1`, X360 `+0x3f1`): the model is drawn
    /// through what stands in front of it as an outline (`CCharacter::
    /// setRenderBehind`), true unless the data says otherwise.
    bool render_behind = true;
};

/// Whether `self` would pick `other` to fight: opposite sides, neither `Neutral`, and
/// `other` targetable (`UnitStats::targetable`). Invincibility is deliberately not a test
/// (`CCharacter::isPetNearDeath()` expects a pet worn down).
inline bool fights(UnitStats::Alignment self, UnitStats::Alignment other, bool other_targetable) {
    if (self == UnitStats::Alignment::Neutral) return false;
    if (other == UnitStats::Alignment::Neutral) return false;
    if (self == other) return false;
    return other_targetable;
}
inline bool fights(const UnitStats& self, const UnitStats& other) {
    return fights(self.alignment, other.alignment, other.targetable);
}

/// One of the classes a new character can be, read from
/// `media/units/players/<Class>/<CLASS>.DAT` (the directory is the list).
struct PlayerClass {
    /// `NAME`, also the `.DAT`'s file name.
    std::string name;

    /// `DISPLAYNAME` - what the screen calls them.
    std::string display_name;

    /// `DESCRIPTION` - the sentence under the name.
    std::string description;

    /// The mesh, a full archive path from `RESOURCEDIRECTORY` and `MESHFILE` (not a basename).
    std::string mesh;

    /// The `logo.imageset` icon name (with `lit` and `selected` twins).
    std::string icon;

    /// A mod's class: the imageset its portrait is in and the radio button's three images
    /// (`UnselectedImage`, `HoverImage`, `SelectedImage`) from the window its mod's
    /// `charactercreate.layout` names after it. Empty for the game's own classes.
    std::string portrait_imageset;
    std::string portrait[3];
};

/// A `CItemGold`'s own figures (`CItemGold::CItemGold`).
///
struct GoldPile {
    int32_t amount = 0;    ///< +0x22c
    float heading = 0.0f;  ///< the constructor's turn about Y, in degrees
    float scale = 1.0f;    ///< +0x230, the size the pile grows to
};

/// One item's authored numbers (`media/masterresourceunits.dat`) and a copy's rolled ones.
/// The damage figures are percentages of `BASE_WEAPON_DAMAGE` at the item's level.
struct ItemStats {
    std::string name;          ///< `NAME`, the key everything joins on
    std::string display_name;  ///< `DISPLAYNAME`, what a person calls it
    /// `+0x2d0`: `UNIDENTIFIED_NAME` with its first `{...}` span removed
    /// (`CEquipment::unitInit`), returned by
    /// `getItemName` while unidentified.
    std::string unidentified_name;
    /// `+0x2d8`: `NAME` / `DISPLAYNAME` with its first `{...}` span removed
    /// (`CEquipment::unitInit`). Returned by
    /// `getItemName`.
    std::string base_name;

    /// `UNITTYPE` - `NORMAL STAFF`, `MAGIC SWORD`, `POLEARM`: what decides the swing.
    std::string unittype;

    /// `RESOURCEDIRECTORY` and `MESHFILE`, joined: the mesh to put in the hand,
    /// without an extension.
    std::string mesh;

    int level = 1;  ///< `LEVEL`, which indexes the weapon-damage curve

    /// `MINDAMAGE`/`MAXDAMAGE`, percentages of the curve; 0 when not authored
    /// (`CEquipment::calculateCombatStats`).
    float min_damage = 0.0f;
    float max_damage = 0.0f;

    /// `RARITY_DMG_MOD` and `SPEED_DMG_MOD`, default 100: percentages the
    /// rolled `MINDAMAGE..MAXDAMAGE` is scaled by before the curve.
    int rarity_damage_mod = 100;
    int speed_damage_mod = 100;

    /// How a weapon's damage is split, by `EDAMAGE_TYPES`: `DAMAGE_PHYSICAL`
    /// (default -1, all of it), `DAMAGE_ELECTRIC`/`_FIRE`/`_ICE`/`_POISON`
    /// (default 0), percentages of the rolled figure.
    std::array<int, kDamageTypeCount> damage_split{-1, 0, 0, 0, 0, 0, 0};

    // ---- per copy --------------------------------------------------------
    /// What `calculateCombatStats` rolled for this copy of the item: `+0x334`,
    /// the physical damage its attack carries (`CAttackDescription +0x24`), and
    /// `+0x380` by type, the inherent damage `getDamageBonus` returns. Unset
    /// until `roll_weapon_damage` has run on it.
    bool combat_rolled = false;
    int damage = 0;
    std::array<int, kDamageTypeCount> inherent_damage{};
    /// `+0x350`: the damage types the copy carries, in the order
    /// `addInherentDamage` and
    /// `addDamageBonus` first met them - the
    /// order a save writes them in.
    std::vector<int> damage_types;
    /// `+0x368` by type: what `createElementalDamages`
    /// folded out of a weapon's own passive
    /// `DAMAGE` (0x34) and `DAMAGE BONUS` (0xa) effects - an affix's, mostly.
    /// `getDamageBonus` adds it to `inherent_damage`.
    std::array<int, kDamageTypeCount> damage_bonus{};
    /// `+0x314` and `+0x310`: the damage and armour a save keeps (`CEquipment::fillSaveState`
    /// `applySaveState`); -1 from the constructor.
    /// Set by `setGraphDamage` and a weapon's split,
    /// and by `setGraphAC` and `ARMOR_PHYSICAL`.
    int kept_damage = -1;
    int kept_armour = -1;
    /// The quest this copy belongs to (`CBaseUnit +0x170`, its `QUEST_GUID`)
    /// and the objective that made it (`+0x178`); -1 for neither
    /// (`CBaseUnit::CBaseUnit`). Stamped by
    /// `CQuestUnitData::spawnUnit` and saved
    /// as they are (`CItem::fillSaveState`, `applySaveState`).
    ///
    int64_t quest_guid = -1;
    int32_t quest_unit_id = -1;

    /// `UNIT_GUID` (`CBaseUnit +0x1a0`): what a hotbar slot holds and
    /// `CInventory::getEquipmentOfGuid` looks up. -1 when unread.
    int64_t unit_guid = -1;
    /// `USES` (`CEquipment +0x248`, `unitInit`): read as a string, default "0", through
    /// `GetInt`; -9999 is unlimited.
    int uses = 0;
    /// `TARGET_TYPE` (`CEquipment +0x260`): 0 `USER` (the default, used at
    /// once), 1 picks a target first.
    int target_type = 0;
    /// `DONT_USE_ON_FULL` (`CBaseUnit::dontUseOnFull`
    /// default false).
    bool dont_use_on_full = false;
    /// `USE_SOUND`, the item bank's slot 0x14.
    std::string use_sound;
    /// The item's own `[EFFECT]` blocks (`CEquipment +0x1b8`'s list), what
    /// `CEquipment::useOnTarget` applies.
    std::vector<Effect> effects;

    // ---- the item roll, per copy ------------------------------------------
    /// The affixes on this copy, in the order they were added: `+0x1b8`'s
    /// affix list (`CEffectManager +0x10`, count `+0x18`). Each is the pool's
    /// affix cloned at the level it was rolled at (`CAffix::CAffix(CAffix*,
    /// unsigned)`), so its effects already carry
    /// that level; `level` beside it is what `CBaseUnit::addAffix`
    /// was handed.
    struct RolledAffix {
        Affix affix;
        uint32_t level = 0;
    };
    std::vector<RolledAffix> affixes;
    /// The affixes' elemental effects `createElementalDamages` killed: off the
    /// manager, but still in their affix's own list (`deleteDeadEffects`
    /// frees only an effect no affix owns), where
    /// `CAffix::clearEffectsFromOwner` still rolls them. Kept
    /// together: only how many there are, and their ranges, are read.
    std::vector<Effect> dead_affix_effects;
    /// `+0x3e0`: sockets. `SOCKETS` (capped at 2) from `unitInit`,
    /// then the random roll in `enchant`
    /// or the enchanter's `addSockets`.
    /// 0 from the constructor.
    ///
    int sockets = 0;
    /// `+0x3e8` (count `+0x3f0`, capacity `+0x3f4`): the items set into those
    /// sockets, in the order they went in. `CEquipment::addContainerItem`
    /// (`add_container_item`) is the one writer;
    /// `CInventory::calculateEffectValue`
    /// adds each one's effects to its host's. The save nests each one's record
    /// in its host's (`CItemSaveState +0x128`).
    std::vector<ItemStats> socketed;
    /// Made from a save (`createUnit(..., from_save = true)`): `unitInit` put
    /// none of the template's `[EFFECT]` blocks or `BLOCK_CHANCE` on it
    /// (`CBaseUnit::unitInit` skips
    /// `reapplyEffects`), and its effect lists are the saved ones,
    /// `saved_effects`.
    bool from_save = false;
    /// One effect `CEquipment::applySaveState`
    /// put back with `addNewEffect`: the
    /// record rebuilt by `CItemSaveState::load`,
    /// with the saved `+0xc0` and `+0x18`
    /// laid over it. The saved `+0xc0` is what every query sums, so the value
    /// stays what it was.
    struct SavedEffect {
        Effect effect;
        float value = 0.0f;    ///< +0xc0
        float elapsed = 0.0f;  ///< +0x18
    };
    /// A loaded copy's effect lists, in list order (passive, dynamic, ...).
    std::vector<SavedEffect> saved_effects;
    /// `+0x2e0` / `+0x2e8`: the two name parts `getFullItemName`
    /// caches, saved at `CItemSaveState
    /// +0x18` / `+0x20`. Computed by `item_full_name`.
    std::string name_part_2e0;
    std::string name_part_2e8;
    /// `+0x348`: identified. 1 from the constructor, cleared whenever
    /// `enchant` adds affixes, set again by `ALWAYS_IDENTIFIED`. An
    /// unidentified copy is priced as a normal one (`sellPrice`).
    ///
    bool identified = true;
    /// `CBaseUnit +0x155`: `COLLIDEABLE`'s first byte, true when not there
    /// (`CBaseUnit::unitInit`). An item's save
    /// record carries it (`CItem::fillSaveState`).
    bool collideable = true;
    /// `+0x20a`: may roll random affixes and sockets. 0 from `CItem::CItem`;
    /// `CResourceManager::createItem`
    /// stores its
    /// third argument here before `unitInit`.
    bool random_enchant_allowed = false;
    /// `+0x344`: the enchanter's count of paid enchants, 0 from the
    /// constructor; `enchantPrice` scales by
    /// it.
    uint32_t enchant_count = 0;
    /// `+0x1b8` exists though nothing is on it: the manager is made by the
    /// first `addAffix`/`addNewEffect` and never deleted, so an item the
    /// enchanter has stripped keeps an empty one (`item_has_effect_manager`).
    bool emptied_effect_manager = false;
    /// `+0x28c`: heirloom level, 0 from the constructor.
    int heirloom_level = 0;
    /// `+0x264`/`+0x268`: buy and sell price for one, by quality;
    /// `+0x26c`/`+0x270`: the same from the `NORMAL` curves, which is what an
    /// unidentified copy trades at. Set by `recalculatePrice`.
    /// The constructor leaves the second
    /// pair unwritten; 0 here.
    int buy_price = 1;
    int sell_price = 1;
    int buy_price_normal = 0;
    int sell_price_normal = 0;

    // ---- the item roll's base data (`read_item_roll_data`) ----------------
    /// The authored `*_REQUIRED` keys, which `CEquipment::setRequirements`
    /// turns into `min_level_required`..
    /// below; all default 0.
    struct AuthoredRequirements {
        int level = 0;      ///< `LEVEL_REQUIRED`
        int strength = 0;   ///< `STRENGTH_REQUIRED`
        int dexterity = 0;  ///< `DEXTERITY_REQUIRED`
        int magic = 0;      ///< `MAGIC_REQUIRED`
        int defense = 0;    ///< `DEFENSE_REQUIRED`
    };
    AuthoredRequirements authored_requirements;
    /// `MAX_SOCKETS`, default 2 (`CEquipment::getMaxSockets`).
    ///
    int max_sockets = 2;
    /// `SOCKETS`, default 0 (`unitInit`), before the
    /// cap of 2.
    int authored_sockets = 0;
    /// `ALWAYS_IDENTIFIED`, default false (`enchant`, `unitInit`).
    bool always_identified = false;
    /// `MERCHANTINFINITE` (`CEquipment +0x25f`, `unitInit`; false
    /// from the constructor): a merchant's single copy is never sold
    /// out - buying it takes a fresh copy (`buy_from_merchant`).
    bool merchant_infinite = false;
    /// `BLOCK_CHANCE`, default 0 (`unitInit`): a
    /// non-zero value becomes a passive effect of type 0x3e on the copy, which
    /// `enchant`'s unique second pass counts (`item_passive_effect_count`).
    int block_chance = 0;
    /// `SET`, default empty (`CEquipment::getSet`,
    /// read from the item's data group each time it is asked): the `NAME` of
    /// the item set this item is a piece of.
    std::string set;
    /// `DROPPARTICLE`, default empty (`CEquipment::unitInit` into
    /// `+0x3d8`): the particle `createParticles` hangs on the item in place of
    /// its kind's.
    std::string drop_particle;
    /// `SHADOWS` (`CBaseUnit +0x1a9`, true by default): `CItem::calculateActiveRange`
    /// sets the model's shadow flag only for
    /// a unit that has it.
    bool shadows = true;
    /// The values of the item's `[AFFIXES]` group, in order (each an `AFFIX`
    /// line naming an affix file): what `CBaseUnit::reapplyAffixes`
    /// puts on a fresh copy.
    std::vector<std::string> authored_affixes;

    /// `RANGE` and `STRIKERANGE`.
    float range = 0.0f;
    float strike_range = 0.0f;

    /// `SPEED`, a percentage where 100 is the ordinary rate.
    float speed = 100.0f;

    std::string attack_sound;  ///< `ATTACK_SOUND` - the swing
    std::string strike_sound;  ///< `STRIKE_SOUND` - the landing

    /// `MISSILE`, upper-cased (`CEquipment +0x400`, `calculateCombatStats`):
    /// the missile a blow with this weapon fires instead of
    /// landing (`CEquipment::fireMissiles`).
    std::string missile;

    /// The item's `[SKILL]` blocks, in file order: what `CBaseUnit::unitInit`
    /// hands `addSkillByName(NAME, true)` and
    /// `setSkillLevel` for each, between the item's own `[EFFECT]`s
    /// (`reapplyEffects`) and its affixes (`reapplyAffixes`).
    struct Skill {
        std::string name;     ///< `NAME`
        uint32_t level = 1;   ///< `LEVEL`, default the root's `LEVEL` (`+0x100`)
        bool enabled = true;  ///< `ENABLED` (`CSkill +0x6d`), default the root's, default true
    };
    std::vector<Skill> skills;

    /// One `[SKILL_TO_GIVE]` group, as `CEquipment::skillDescription`
    /// reads it from the item's data: what
    /// the item's text says it grants.
    struct SkillToGive {
        std::string name;  ///< `DISPLAYNAME`, else `NAME`, else empty
        int level = 1;     ///< `LEVEL`, else the root's `LEVEL`, else 1
    };
    std::vector<SkillToGive> skills_to_give;

    /// The item's own bank, slots 0x10-0x12 (`CEquipment::drop`
    /// `updateDrop`,
    /// `playTakeSound`): leaving the hand, hitting the floor, and
    /// being picked up.
    std::string fall_sound;  ///< `FALL_SOUND`
    std::string land_sound;  ///< `LAND_SOUND`
    std::string take_sound;  ///< `TAKE_SOUND`
    /// One `[WARDROBE]` group: what the item dresses a character of `CLASS`
    /// in (`CWardrobe::update`). The master unit
    /// table merges the base file's groups ahead of the item's own; the last
    /// match wins.
    struct Wardrobe {
        std::string character_class;  ///< `CLASS`
        std::string mesh;             ///< `MESH`
        std::string texture;          ///< `TEXTURE`
        bool has_mesh = false;
        bool has_texture = false;
    };
    std::vector<Wardrobe> wardrobe;

    std::string icon;  ///< `ICON` - item icon sprite name
    /// `VALUE`: the price as a percentage of the `PRICE_*` curves, **default
    /// 100** (`recalculatePrice`); 0 leaves the
    /// prices at 1. The renderer's loader reads default 0, which `item_sell_price` maps to 100.
    int value = 100;
    int rarity = 0;           ///< `RARITY` - rarity weighting / quality
    int max_stack = 1;        ///< `MAXSTACKSIZE` - maximum stack size
    std::string description;  ///< `DESCRIPTION` - item description text
    /// The armour keys `calculateCombatStats`
    /// reads, with its defaults: `ARMOR` 0 (a fixed figure, which wins over the
    /// roll), `ARMORMIN`/`ARMORMAX` 0 (the roll, taken only when both are
    /// non-zero), `RARITY_AMR_MOD`/`SPECIAL_AMR_MOD` 100.
    int armor = 0;
    int armor_min = 0;
    int armor_max = 0;
    int rarity_armor_mod = 100;
    int special_armor_mod = 100;

    /// `ARMOR_PHYSICAL` (default -1, keep the rolled figure) and the elemental
    /// `ARMOR_ELECTRIC`/`_FIRE`/`_ICE`/`_POISON` (default 0), by
    /// `EDAMAGE_TYPES`: percentages of the rolled armour points, read by
    /// `calculateCombatStats` for an `Armor` only. Physical replaces
    /// `graph_ac`; each elemental one becomes an effect in `combat_effects`.
    std::array<int, kDamageTypeCount> armor_split{-1, 0, 0, 0, 0, 0, 0};

    /// `+0x338`, per copy: the armour points `calculateCombatStats`' armour
    /// half writes (`roll_item_armour`). `CInventory::updateBonuses`
    /// sums it into the inventory's `+0x10`.
    int graph_ac = 0;
    /// The effects `calculateCombatStats` adds to a fresh copy of an `Armor`
    /// (`CBaseUnit::addNewEffect`): one passive per
    /// elemental `ARMOR_*` above 0, its percentage of `graph_ac`. Kept apart
    /// from `effects`, the item's authored `[EFFECT]` blocks, because a copy is
    /// rolled once and the item data is shared.
    std::vector<Effect> combat_effects;
    /// How many of `affixes` the copy held when `calculateCombatStats` ran: the
    /// ones `CBaseUnit::unitInit`'s `reapplyAffixes` put on, ahead of the
    /// `BLOCK_CHANCE` effect and `combat_effects` in the manager's lists; the
    /// rest (`enchant`'s, and any added later) come after them. Set by
    /// `init_item_copy`.
    size_t affixes_before_combat = 0;
    /// The order `CEffectManager::deleteDeadEffects` left
    /// the `PASSIVE` list in when `createElementalDamages` took effects off it:
    /// the console removes an entry by moving the last into its place, so the
    /// list the port builds again from the item's data (in the order the effects
    /// were added) is rearranged by this (`EffectManager::reorder`). Empty: the
    /// order they were added in.
    std::vector<uint32_t> passive_order;

    /// `AI_ATTACKCOOLDOWN` (`CEquipment +0x408`), default 0.
    float attack_cooldown = 0.0f;

    /// Per copy: the requirements `CEquipment::setRequirements`
    /// works out from
    /// `authored_requirements`, the item level and the `ITEM_*_REQUIREMENTS`
    /// curves (`set_item_requirements`), before any
    /// reduction a character's effects give (`item_level_requirement` & co.).
    int min_level_required = 0;  ///< `+0x278`
    int strength_required = 0;   ///< `+0x27c`
    int dexterity_required = 0;  ///< `+0x280`
    int magic_required = 0;      ///< `+0x284`
    int defense_required = 0;    ///< `+0x288`

    /// What a spawn class reads of the item's data group
    /// (`CSpawnClassData::fillRandomizer`, as
    /// `tl::core::SpawnUnitData::from_adm` reads them): `MINLEVEL` default 0,
    /// `MAXLEVEL` default 0 (99999 with a `CREATEAS`), `CREATEAS`,
    /// `RARITY` default 1 and `RARITY_HARDCORE`, -1 when not authored. Unlike
    /// `min_level_required`, the minimum does not fall back to `LEVEL`.
    int spawn_rarity = 1;
    int spawn_min_level = 0;
    int spawn_max_level = 0;
    std::string create_as;
    int rarity_hardcore = -1;

    /// `SCALE` default 1 and `SCALE_VARIATION` default 0, which
    /// `CBaseUnit::unitInit` reads for every
    /// unit, items included.
    float scale = 1.0f;
    float scale_variation = 0.0f;

    /// `CREATEAS:GOLD` makes a `CItemGold`, not a `CEquipment`
    /// (`CResourceManager::createNewItem`);
    /// `MINVALUE`/`MAXVALUE`, each default 100, are its data
    /// (`CItemGold::unitInit`) and `gold` the copy's
    /// figures (`init_gold_copy`).
    float min_value = 100.0f;
    float max_value = 100.0f;
    GoldPile gold;
    /// `createNewItem`'s test: `CREATEAS`, upper-cased, is `GOLD`.
    bool is_gold() const;
};

/// Every item, by name. Names are matched without case.
class ItemBook {
public:
    void add(const ItemStats& stats);
    const ItemStats* find(const std::string& name) const;

    /// ... or by `DISPLAYNAME` (for command-line options); the first match, null for none.
    const ItemStats* find_by_display_name(const std::string& display_name) const;

    /// ... or by `UNIT_GUID`, which is what a hotbar slot holds
    /// (`CResourceManager::getUnitDataByGuid`, as `CGameUI::updateSlots`
    /// looks a slot's item up). Null for -1 or an
    /// unknown GUID.
    const ItemStats* find_by_guid(int64_t guid) const;
    /// Every item, keyed by its lower-cased `NAME`.
    // PORT: an accessor of the port's own bookkeeping.
    const std::map<std::string, ItemStats>& all() const { return by_name_; }
    // PORT: an accessor of the port's own bookkeeping.
    size_t size() const { return by_name_.size(); }
    void clear() {
        by_name_.clear();
        by_guid_.clear();
    }

private:
    std::map<std::string, ItemStats> by_name_;
    std::map<int64_t, std::string> by_guid_;  ///< `UNIT_GUID` -> key in `by_name_`
};

/// Every unit a level needs, by name. Names are matched without case.
class UnitBook {
public:
    void add(const std::string& name, const UnitStats& stats);
    const UnitStats* find(const std::string& name) const;
    size_t size() const { return by_name_.size(); }
    // PORT: an accessor of the port's own bookkeeping.
    void clear() { by_name_.clear(); }

private:
    /// Names compared without case and without a lower-cased copy per lookup.
    struct NameLess {
        using is_transparent = void;
        bool operator()(const std::string& a, const std::string& b) const;
    };
    std::map<std::string, UnitStats, NameLess> by_name_;
};

}  // namespace tl::core
