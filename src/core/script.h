// Level script: the original's layout logic runtime. The nodes that draw nothing (`Player
// Sphere Trigger`, `Timer`, `Timeline`, `Camera Controller`, `Quest Controller`) and the
// `Logic Group`s wiring outputs to inputs, each node a transcription of its class
// (`CLogicTimer`, `CCounter`, `CTriggerSphere`, `CTimeline`, `CQuestController`,
// `CCameraController`, `CAnimationPlayer`, `CPathController`, `CSkipCutscene`,
// `CGameStateController`) and its descriptor's `InputLogicEvent`.
// The port's own: which renderer object a node becomes, how it is drawn, and the walker
// a path hands over to (`CCharacter::setDestination`).
// No OGRE and no file I/O: the machine decides what happened, the renderer what it looks
// like; they meet at `ScriptEvent`.
#pragma once

#include "core/affix_book.h"
#include "core/ai_flags.h"
#include "core/ai_skills.h"
#include "core/collision.h"
#include "core/combat.h"
#include "core/damage_shape.h"
#include "core/death_flight.h"
#include "core/dialog.h"
#include "core/effect_manager.h"
#include "core/inventory.h"
#include "core/jump_down.h"
#include "core/logic_events.h"
#include "core/machine_walk.h"
#include "core/motion.h"
#include "core/navigation.h"
#include "core/no_spawn_region.h"
#include "core/passability.h"
#include "core/quest.h"
#include "core/quest_book.h"
#include "core/random_names.h"
#include "core/route.h"
#include "core/script_event.h"
#include "core/skill_animation.h"
#include "core/sound_data.h"
#include "core/sound_object.h"
#include "core/text_event.h"
#include "core/timeline.h"
#include "core/unit_spawner.h"
#include "core/units.h"
#include "core/weapon_procs.h"
#include "core/weapons.h"
#include "formats/cmp.h"
#include "formats/save_file.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace tl::core {

class Inventory;
class LevelScript;
struct EffectRolls;
struct SkillConstruction;

/// Where a layout file sits in the level: `origin + x * local.x + y * local.y + z *
/// local.z`. Applied once at `build`; the machine then runs in level coordinates.
struct LayoutFrame {
    Vec3 origin;
    Vec3 x{1.0f, 0.0f, 0.0f};
    Vec3 y{0.0f, 1.0f, 0.0f};
    Vec3 z{0.0f, 0.0f, 1.0f};

    // PORT: a point of a layout file in the level's coordinates.
    Vec3 point(const Vec3& local) const { return origin + direction(local); }
    Vec3 direction(const Vec3& local) const { return x * local.x + y * local.y + z * local.z; }
};

/// `LEVEL DELTA` that sends a warp to town instead of to a depth
/// (`CGameClient::performWarp`).
inline constexpr int32_t kWarpDeltaToTown = -99;

/// Whether a unit at `from`, facing `facing`, notices something at `at`.
/// `CLevel::findCharacterWithinView`.
bool can_see(const Vec3& from, float facing, const Vec3& at, float motion_radius, float view_angle,
             float sight_range);
/// The same with the looker turned along `forward` as the console's skill
/// searches turn it: `Quaternion::FromAxes(UNIT_Y x forward, UNIT_Y, forward)`,
/// the cross product in three `fmsubs`.
bool can_see_along(const Vec3& from, const Vec3& forward, const Vec3& at, float motion_radius,
                   float view_angle, float sight_range);

/// `EAIState` values the script reads or sets (`CCharacter::setAIState`).
///
namespace ai_state {
inline constexpr int32_t kNone = 0;
inline constexpr int32_t kFidget = 1;
inline constexpr int32_t kIdle = 2;
inline constexpr int32_t kAttack = 3;
inline constexpr int32_t kHunt = 4;
inline constexpr int32_t kDying = 5;
inline constexpr int32_t kDead = 6;
inline constexpr int32_t kFlee = 10;
/// `returnToTownAI`
inline constexpr int32_t kGoingToTown = 0x29;
/// `updateInTownTimer`
inline constexpr int32_t kInTown = 0x2a;
/// `CCharacter::skillAI`
inline constexpr int32_t kSkill = 0xd;
/// `performingSkillAI`
inline constexpr int32_t kPerformingSkill = 0xe;
/// `skillApproachAI`
inline constexpr int32_t kSkillApproach = 0xf;
/// `CMonster::jumpDownAI`
inline constexpr int32_t kJumpDown = 0x10;
inline constexpr int32_t kPlayAnimation = 0x23;
inline constexpr int32_t kScripted = 0x2c;
}  // namespace ai_state

/// The running script of one loaded layout.
class LevelScript {
public:
    // PORT: the level machine's wiring, set by the renderer.
    void set_dialog(const DialogBook* book) { dialog_ = book; }
    // PORT: the level machine's wiring, set by the renderer.
    void set_units(const UnitBook* units) { units_ = units; }
    // PORT: the level machine's wiring, set by the renderer.
    void set_navigation(const NavGrid* nav) { nav_ = nav; }
    // PORT: the level machine's wiring, set by the renderer.
    void set_active_range_mod(float mod) { active_range_mod_ = mod; }
    /// The level's unit active range: `INDOOR_UNIT_ACTIVE_RANGE` (16) while the template's
    /// `UNIT_LIGHT_FADE` is set, else `OUTDOOR_UNIT_ACTIVE_RANGE` (24)
    /// (`CCharacter::calculateActiveRange`).
    void set_unit_active_range(float range) { unit_active_range_ = range; }
    /// `GLOBALS.DAT` `UNIT_NEAR_RANGE` (`CGameGlobals +0xac`, 24 by default):
    /// past it a character is hidden and not updated at all.
    // PORT: the level machine's wiring, set by the renderer.
    void set_unit_near_range(float range) { unit_near_range_ = range; }
    /// A character's fade (`+0x528`), 1 for anything else.
    float unit_opacity(int64_t id) const;
    /// `UNIT_SHADOW_RANGE` (`CGameGlobals +0xa8`, 10 by default).
    void set_unit_shadow_range(float range) { unit_shadow_range_ = range; }
    /// The camera test `CCharacter::update`
    /// makes of a character: `Camera::isVisible` on its model's world bounds.
    /// Unset, every character is in view.
    using ViewTest = std::function<bool(const LevelScript& machine, int64_t node)>;
    // PORT: the level machine's wiring, set by the renderer.
    void set_view_test(ViewTest test) { view_test_ = std::move(test); }
    /// `CLevel::updateNearUnitList`'s item
    /// half, which the renderer keeps: run on the same once-a-second tick,
    /// after the characters.
    void set_item_near_list(std::function<void()> hook) { item_near_list_ = std::move(hook); }
    /// `CMoneyTaker::takeMoney`, its two questions of the player:
    /// the gold it holds (`+0x444`), and `giveGold(-amount)`. Unset, there is
    /// no player and a `Money Taker` does nothing.
    struct PlayerGold {
        std::function<int32_t()> held;
        std::function<void(int32_t amount)> give;
    };
    void set_player_gold(PlayerGold hooks) { player_gold_ = std::move(hooks); }
    /// `CLayout::processInputs(unsigned int, CEditorBaseObject*)`
    /// on a `Layout Link Timeline`'s layout: the input to each `LAYOUT CONTROL`
    /// timeline in creation order. True when there was one.
    bool process_inputs(uint32_t input, QuestState& quests, std::vector<ScriptEvent>& out);
    /// The link's layout took nothing: `CPositionableObjectDescriptor`'s `InputLogicEvent`
    /// on the link itself.
    void link_input_declined(int64_t link, uint32_t input, QuestState& quests,
                             std::vector<ScriptEvent>& out);
    /// `CLayout::setDurationModification(float)`: above 0, each `LAYOUT CONTROL`
    /// timeline takes it by its `TYPE` (`setDurationModificationTime`): `Set`,
    /// `Multiply`, `None`.
    void set_duration_modification(float value);
    /// `KSETTINGS_F_AVERAGE_FPS` as `CGame::update`
    /// last wrote it (`core::AverageFps`).
    void set_average_fps(float fps) { average_fps_ = fps; }
    /// `DESTROY DEAD CORPSE` (`KSETTINGS_DESTROY_MONSTERS_AFTER_DEATH`) and
    /// `SHADOWS_ENABLED`: 0 and 1 unless a settings file says otherwise.
    void set_destroy_dead_corpse(int32_t value) { destroy_dead_corpse_ = value; }
    void set_shadows_enabled(int32_t value) { shadows_enabled_ = value; }
    /// `CCharacter::setMaster` with a placed character as
    /// master, as `CCharacter::addPet` makes a `MAKEPET`
    /// summon: the master's side, AI idle, no target. False when either is unknown.
    bool set_unit_master(int64_t id, LevelScript& master_machine, int64_t master_node,
                         QuestState& quests, std::vector<ScriptEvent>& out);
    /// `CLevel::deleteCharacter`: the
    /// character is gone from the level - hidden, off the avoidance map,
    /// forgotten. Its node stays.
    bool delete_character(int64_t id, std::vector<ScriptEvent>& out);
    /// `+0x190` set (`CCharacter::completeDeath` for
    /// a `DESTROY_ON_DEATH` unit): deleted before its next update. True when it
    /// was not marked already.
    bool mark_unit_for_deletion(int64_t id);
    /// `die`'s `DEATH_FLIGHT` branch on a character this machine runs: it flies
    /// `flight` in AI state 5 until `dyingAI`
    /// lands it.
    bool start_death_flight(int64_t id, const DeathFlight& flight);
    /// The same, laid here by `plan_death_flight` over the level's collision, rolls on
    /// `g_RandVolatile`. False with no such character or no collision.
    bool start_death_flight(int64_t id, const Vec3& direction, float force,
                            DeathFlight* laid = nullptr);
    /// The character is flying its death flight.
    bool unit_flying(int64_t id) const;
    /// This machine's living characters whose master is `(master_machine, master_node)`,
    /// oldest first: its share of the master's pet list (`die`).
    std::vector<int64_t> pets_of(const LevelScript* master_machine, int64_t master_node) const;
    // PORT: the level machine's wiring, set by the renderer.
    void set_collision(const CollisionWorld* collision) { collision_ = collision; }
    /// The level's grids, which characters write their avoidance circles to
    /// (`CLevel +0x40`/`+0x48`).
    // PORT: the level machine's wiring, set by the renderer.
    void set_passability(PassabilityMap* grid) { passability_ = grid; }

    /// Where this machine's layout file sits in the level. Set before `build`,
    /// which carries every node into the level's coordinates with it.
    // PORT: the level machine's wiring, set by the renderer.
    void set_frame(const LayoutFrame& frame) { frame_ = frame; }
    const LayoutFrame& frame() const { return frame_; }

    /// Every machine of the level, in the order their characters join the level's one list
    /// (`updateCharacters`, `findCharacterWithinView`),
    /// so characters of different layouts see and strike one another.
    /// Null: this machine alone.
    void set_level_machines(const std::vector<LevelScript*>* machines) { level_ = machines; }
    /// PORT: zeroes the process-wide near-unit list timer (`CLevel::update`'s function static);
    /// only tests call it.
    static void reset_near_list_timer();
    /// `CGameClient +0x10bc`, written only by a `Game State Controller` in `PAUSE GAME`
    /// (`doActualGameState`): the level is dormant (`CLevel::isDormant`) and
    /// the interface in a cinematic (`getUIIsInCinematic`).
    /// Process-wide (`CGame::createGameClient`): only a controller
    /// clears it.
    static bool client_paused();
    /// The constructor's 0: for tests, and for a verification run's
    /// `reset_script`, which must leave the game as it found it.
    static void reset_client_paused();
    /// `media/graphs/stats`, for `CCharacter::setLevel`.
    void set_stat_graphs(const StatBook* graphs) { graphs_ = graphs; }
    /// `CGameClient +0x38ec`, which `CDifficultyMenu::onClick`
    /// stores; the renderer hands over the
    /// one the game plays at.
    void set_difficulty(int difficulty) { difficulty_ = difficulty; }
    int difficulty() const { return difficulty_; }
    /// `CGameGlobals +0x98..+0xa4` for the difficulty (`GLOBALS.DAT`
    /// `*_MONSTER_ATTENTION_BONUS`), added to sight and motion radii.
    // PORT: the level machine's wiring, set by the renderer.
    void set_attention_bonus(float bonus) { attention_bonus_ = bonus; }

    /// What a blow on the player meets: `CCharacter::AC()`, `maxHP()`,
    /// `getBlockChance()` and the player's collision radius.
    void set_player_defense(const Defender& defense, float radius) {
        player_defense_ = defense;
        player_radius_ = radius;
    }

    /// `CCharacter::alive` for the player, who lives in the renderer:
    /// false from `CCharacter::die` until `CPlayer::levelResetting` sets state 0.
    void set_player_alive(bool alive) { player_alive_ = alive; }
    /// The player's `getEffectValue` totals, read by a character whose master is the player:
    /// `PERCENT PET DAMAGE` (0x5b) in `maxDamage` and
    /// `PERCENT PET ARMOR` (0x61) in `armorBonus`.
    void set_player_effects(const CharacterEffects& effects) { player_effects_ = effects; }
    bool player_alive() const { return player_alive_; }

    /// Player HP and max HP, for `CGameStateController::update`.
    void set_player_hp(float hp, float max_hp) {
        player_hp_ = hp;
        player_max_hp_ = max_hp;
    }

    /// The player's target when it is one of this machine's units, else 0: `CMonster::idleAI`
    /// asks whether the unit thinking is the one the player talks to. Set only for an
    /// `INTERACTIVE CHARACTER` (0x83).
    void set_player_target(int64_t node) { player_target_ = node; }
    /// The player's forward (`+0xb4`) and whether they are going anywhere
    /// (`+0x264`) and running (`+0x321`), which a pet's `idleAI` reads.
    // PORT: the level machine's wiring, set by the renderer.
    void set_player_motion(const Vec3& forward, bool moving, bool running) {
        player_forward_ = forward;
        player_moving_ = moving;
        player_running_ = running;
    }
    /// The player's `walkingSpeed()` and `runningSpeed()`,
    /// which a pet's `idleAI` compares and
    /// `CANMATCHSPEED` keeps pace with.
    void set_player_speeds(float walking, float running) {
        player_walking_speed_ = walking;
        player_running_speed_ = running;
    }
    /// The player's own target (`CCharacter +0x340` on the player), which a
    /// pet's `idleAI` takes on when it is within 8 of the player.
    /// `machine` null with node 0 is none.
    void set_player_attack_target(LevelScript* machine, int64_t node) {
        player_attack_target_ = UnitRef{machine, node};
        player_has_attack_target_ = machine != nullptr && node != 0;
    }
    /// `CCharacter::addPet(player, c)` -> `setMaster`:
    /// the player's side, AI idle, target dropped. False when
    /// this machine runs no such character.
    bool make_player_pet(int64_t id, QuestState& quests, std::vector<ScriptEvent>& out);
    /// `CQuestManager`'s walk of a quest's controller listeners (`QuestNotice`)
    /// over this machine's `Quest Controller`s.
    void quest_notice(const QuestNotice& notice, QuestState& quests, std::vector<ScriptEvent>& out);
    /// `CCharacter::removePet(player, c)`:
    /// out of the player's pet list, nothing else. False when it was not in it.
    bool remove_player_pet(int64_t id);
    /// `CCharacter::hasPet` on the player: in the player's pet list.
    bool unit_listed_as_player_pet(int64_t id) const;
    /// `CPlayer::levelResetting` for one of
    /// the player's pets: `teleportToMaster(distance)` from the player standing
    /// at `master`. False when this machine runs no such pet of the player's.
    bool put_pet_by_master(int64_t id, const Vec3& master, float distance);
    /// The machine's characters whose master is the player, alive.
    std::vector<int64_t> player_pets() const;
    /// Every character this machine runs, alive or not, in the order they were
    /// made (the level list's order within this machine).
    std::vector<int64_t> character_ids() const;
    /// `+0x710`, the pet stance: 1 Aggressive,
    /// 2 Defensive (the player's animal: `setCreationPet`,
    /// `CGameClient::loadLevel`), 0 Passive.
    bool set_unit_stance(int64_t id, int32_t stance);
    /// `CCharacter::setTarget(nullptr)` on this machine's character: what a
    /// stance change does after it (`CGameUI::onClick`).
    ///
    bool drop_unit_target(int64_t id, std::vector<ScriptEvent>& out);
    /// `+0x664`, `CANMATCHSPEED` as `unitInit` copies it onto the character.
    bool set_unit_can_match_speed(int64_t id, bool on);
    /// The character's AI state (`+0x330`), or -1.
    int32_t unit_ai_state(int64_t id) const;
    /// The character's `+0x81` as `CCharacter::setVisible(bool, bool)`
    /// last left it: 1 shown, 0 hidden, -1
    /// for no such character.
    int32_t unit_visible(int64_t id) const;
    /// Whether the character runs (`+0x321`) and has somewhere to be (`+0x264`).
    bool unit_running(int64_t id) const;
    bool unit_pathing(int64_t id) const;
    /// `CCharacter::isPetNearDeath()` on this
    /// machine's character.
    bool unit_near_death(int64_t id) const;
    /// `CCharacter::sendToTown(CLevel&)`; false on a town
    /// level or with no master. The returns are the `PET_TOWN_RETURN` (0x60) values.
    bool send_unit_to_town(int64_t id, float own_return, float master_return, bool level_is_town,
                           QuestState& quests, std::vector<ScriptEvent>& out);
    /// `+0x684`, the seconds left of a town trip.
    float unit_town_timer(int64_t id) const;
    /// A town trip carried across a warp: `CGameClient::performWarp`
    /// moves the pets to the new level (`CSkillManager::stopAllSkills(true, false,
    /// false)`, `CLevel::removeCharacter(pet, true)`), so a pet
    /// in town (0x2a) stays, its clock running, and one on its way (0x29) arrives idle. The
    /// port makes the pet again; this restores an away trip, false for any other state.
    bool restore_unit_town_trip(int64_t id, int32_t state, float timer, QuestState& quests,
                                std::vector<ScriptEvent>& out);
    /// Whether the level is a town (`CLevel +0x1d8 -> +0x86`, `IS_TOWN`):
    /// `updateInTownTimer` brings a pet home at once there.
    // PORT: the level machine's wiring, set by the renderer.
    void set_level_is_town(bool town) { level_is_town_ = town; }
    /// `CGameClient +0x38d0 == 1` (`setGameState`): a game is
    /// played, not the front end. Without it `idleAINormal` looks for no fight and
    /// `CCharacter::update` runs no town clock.
    void set_game_playing(bool playing) { game_playing_ = playing; }
    /// `KSETTINGS_PLAYER_UNTARGETABLE` (`PLAYERNOTARGET`, `CConsole::executeCommandLine`);
    /// `CGame::CGame` resets it.
    void set_player_untargetable(bool on) { player_untargetable_ = on; }
    /// The interface's half of `CGameUI::getUIIsInCinematic`:
    /// the cinematic menu (`+0x540`) and the
    /// interactive menus counted in `+0x178c`, which only the scene knows.
    void set_ui_cinematic(bool on) { ui_cinematic_ = on; }
    /// A pet back from town (`updateInTownTimer`
    /// finished this frame), reported once: the scene sells its bag, plays the
    /// voice and opens the dialog.
    std::vector<int64_t> take_units_back_from_town();
    /// `CCharacter +0x490`, the character's inventory: the second half of
    /// `CCharacter::getEffectValue`. The
    /// player's pet has one.
    bool set_unit_inventory(int64_t id, const Inventory* inventory);
    /// `CCharacter::mana()` and `maxMana()`
    /// of this machine's character.
    bool unit_mana(int64_t id, float& mana, float& max);
    /// `CCharacter::performingSkillLoose` on
    /// a placed unit: a skill held and its animation up or not run out.
    bool unit_skill_holds(int64_t id) const;
    /// Gives the character the manager the scene already holds for it
    /// (`CBaseUnit +0x1b8`): the player's pet keeps one across a warp.
    bool set_unit_effects(int64_t id, std::shared_ptr<EffectManager> effects);
    /// A character's side as `CCharacter::alignment()`
    /// answers it (`alignment_of`), or
    /// nothing when this machine runs no such character.
    std::optional<UnitStats::Alignment> unit_side(int64_t id) const;
    /// The character's `+0x640` is the player.
    bool unit_is_player_pet(int64_t id) const;
    /// The character's `+0x640` is set: the player or a character the level
    /// still runs (`has_master`).
    bool unit_has_master(int64_t id) const;
    /// `CCharacter::setVisible` with `+0x70d`, as `applyInstantEffect`'s `FADE OUT` (0x6f) and
    /// `SET VISIBLE` (0x70) and `updateAnimation`'s mesh keys set it; damage shapes pass over
    /// a hidden character (`characterUpdatedInLevel`). `immediate`: at once, or
    /// through the fade. Raises `SetVisible` when it changes.
    bool set_unit_hidden(int64_t id, bool hidden, bool immediate, std::vector<ScriptEvent>& out);
    /// `CCharacter::setAlignment`: `+0x338`.
    /// False when this machine runs no such character.
    bool set_unit_side(int64_t id, UnitStats::Alignment side);
    /// `CCharacter::removePet(std::wstring)` on the player,
    /// this machine's half: every pet whose unit name matches (upper-cased) leaves and dies.
    int desummon_player_pets(const std::string& unit, QuestState& quests,
                             std::vector<ScriptEvent>& out);
    /// `CQuestDialog::cleanUp`, this machine's half: every
    /// player pet whose unit matches leaves the player (`removePet`)
    /// and is returned, alive.
    std::vector<int64_t> release_player_pets(const std::string& unit);
    /// The unit a node of this machine is, or empty.
    std::string unit_of(int64_t id) const;
    /// A character's hit points, as a warp carries them across.
    void set_unit_hp(int64_t id, float hp);

    /// The conversation is over. DIVERGENCE: the unit talked to lets go of the player
    /// (the original keeps it as target), and one that does not walk turns back to its place.
    void release_player_target(std::vector<ScriptEvent>& out);

    /// `CQuestManager::getDialogsForNPC`: the player's quest
    /// line a unit says when a `Quest Controller` names a quest with none for it, with its
    /// quest and section; null for none.
    using NpcDialogLookup = std::function<const DialogLine*(
        const std::string& unit, std::string* quest, QuestSection* section)>;
    void set_npc_dialog(NpcDialogLookup lookup) { npc_dialog_ = std::move(lookup); }

    /// `CQuest::getQuestDialog(CBaseUnit*)` on `quest`: the
    /// line for `unit` now and its section, or null (also for an unknown quest).
    using QuestDialogLookup = std::function<const DialogLine*(
        const std::string& quest, const std::string& unit, QuestSection* section)>;
    // PORT: the level machine's wiring, set by the renderer.
    void set_quest_dialog(QuestDialogLookup lookup) { quest_dialog_ = std::move(lookup); }

    /// `CQuestManager::getQuestForNPC(unit)`, called by
    /// `CGameUI::forceInteractiveDialog` for a line with no
    /// quest; the answer is dropped, only its draws and `initializeQuestWithNPC` remain.
    void set_quest_for_npc(std::function<void(const std::string& unit)> ask) {
        quest_for_npc_ = std::move(ask);
    }

    /// `CQuestManager::getQuestByName` found the quest: needed before a
    /// `Quest Controller` registers with it (`CQuest::addQuestControllerListerner`). Unset,
    /// every quest is found.
    void set_quest_known(std::function<bool(const std::string& quest)> known) {
        quest_known_ = std::move(known);
    }

    /// The mixer a `Sound` node's bank plays on (`core/sound_object.h`), and the definition a
    /// node's `GUID` names (`CSoundObject::setSoundBankGuid`: the bank
    /// cleared, the definition slot 0). Unset, a node plays nothing.
    void set_sound_host(SoundBankHost host) { sound_host_ = std::move(host); }
    using SoundSlotLookup =
        std::function<bool(const std::string& guid, BankSample& slot, bool& loops)>;
    void set_sound_slot(SoundSlotLookup lookup) { sound_slot_ = std::move(lookup); }

    /// `CQuestDialogMenu::setNPC` and `onClick`
    /// on a unit the player talked to (`CGameUI::updateIngameUI`
    /// state 0x1e): a boxed line disables the unit (old value kept) ...
    void menu_opened_on(int64_t node);
    /// Not an `EMENU_EVENT`: the close button on a `FORCEACCEPT` offer
    /// (`CQuestDialogMenu::handle_CloseButton`), which accepts then closes.
    static constexpr int32_t kCloseButtonAccepts = -1;
    /// That close button on the script's own window.
    void close_button_dialog(QuestState& quests, std::vector<ScriptEvent>& out);
    /// The menu letting go of `node` without closing on it (the window put away
    /// by something else): `setEnabled(+0x1e9)` alone.
    void menu_released(int64_t node);
    /// ... and closing puts it back, then broadcasts on it by menu event. `OK`
    /// on a `[COMPLETE]` line (`complete_line`) completes `quest` between the
    /// two halves of its broadcasts (`ScriptAction::CompleteQuest`).
    void menu_closed_on(int64_t node, int32_t menu_event, bool complete_line,
                        const std::string& quest, QuestState& quests,
                        std::vector<ScriptEvent>& out);

    /// The cinematic menu's listeners told of `menu_event`
    /// (`CDropdownMenu::broadcastEvent(1, event)`): each `Cinematic` node that
    /// opened one takes it (`CCinematicObject::menuEventOccured`).
    void cinematic_menu_event(int32_t menu_event);

    /// Whether `id` is one of this machine's characters.
    bool has_character(int64_t id) const { return character(id) != nullptr; }

    /// `CCharacter::approachAI`, the reached unit's half: it
    /// targets the player and stops where it stands (`stopPathing` inlined).
    void approached_by_player(int64_t node, std::vector<ScriptEvent>& out);

    /// `CQuestDialogMenu::setNPC`: a line with `LOOK_AT_PLAYER`
    /// (default true) makes the speaker target the player.
    void target_player(int64_t node, std::vector<ScriptEvent>& out);
    /// `CQuestDialogMenu::updateDialogTags`:
    /// a `<LOOKAT=NAME>` naming a character makes the speaker target it
    /// (`CCharacter::setTarget`); `machine` is the script that character is in.
    void target_unit(int64_t node, LevelScript* machine, int64_t target,
                     std::vector<ScriptEvent>& out);

    /// `CLevel::restartLevel`: `levelResetting` on every character.
    void restart_level(QuestState& quests, std::vector<ScriptEvent>& out);

    /// A unit's animations in its model's order, with lengths: what
    /// `CGenericModel::findRandomAnimation` walks.
    void set_animation_catalog(
        std::function<std::vector<std::pair<std::string, float>>(const std::string& unit)>
            catalog) {
        animation_catalog_ = std::move(catalog);
    }

    /// Where the sound listener stands while the machine is activated: the last frame's, which
    /// the console's `CSoundManager` keeps through a load (`configureVolumeScale`).
    void set_activation_listener(const Vec3& at) { activation_listener_ = at; }

    /// The order `CEditorScene::loadCompressedLayout` makes the file's objects,
    /// by id, for the next `build`: the order of its descriptors and of their objects.
    void set_made_order(std::vector<int64_t> ids) { made_order_ = std::move(ids); }

    /// `CLayoutDescriptor`'s scene activation: each `Layout Link`'s layout
    /// activated in its turn of `level_activated`, handed what the activation fired before it.
    void set_link_activation(std::function<void(int64_t link, std::vector<ScriptEvent>& out)> f) {
        link_activation_ = std::move(f);
    }

    /// How long a unit's animation runs in seconds, negative when it has none by that name:
    /// `CGenericModel::getAnimationLengthSeconds`.
    void set_animation_length(
        std::function<float(const std::string& unit, const std::string& animation)> length) {
        animation_length_ = std::move(length);
    }

    /// The player's skill manager (`CPlayer +0x1c8`) as a `Skill Controller`
    /// reaches it. The renderer owns the player's skills, so it answers.
    struct PlayerSkills {
        /// `CSkillManager::getSkill(name)`
        /// found it: the player has it.
        std::function<bool(const std::string& skill)> knows;
        /// `CSkillManager::getSkillIsUpdatingByName`.
        ///
        std::function<bool(const std::string& skill)> updating;
        /// `CBaseUnit::addSkillByName` and
        /// `setSkillLevel`: false when no skill file answers to the name.
        std::function<bool(const std::string& skill)> learn;
        /// `CSkillManager::deleteSkill`.
        std::function<void(const std::string& skill)> unlearn;
        /// `setSkillLevel` when `level` is above 0, then `CCharacter::castSkill`.
        std::function<void(const std::string& skill, int32_t level)> cast;
        /// `stopSkillsByName` / `attemptStopOfActiveSkill`.
        std::function<void(const std::string& skill, bool force)> stop;
    };
    // PORT: the level machine's wiring, set by the renderer.
    void set_player_skills(PlayerSkills skills) { player_skills_ = std::move(skills); }

    /// A skill a placed unit holds: one of `CBaseUnit::unitInit`
    /// 's `[SKILL]` blocks, `addSkillByName`
    /// and then the block's keys onto the new `CSkill`.
    struct UnitSkill {
        /// The file, `LEVEL` (`setSkillLevel`) and `ENABLED` (+0x6d).
        KnownSkill known;
        int32_t cast_chance = 40;   ///< `CHANCE` (+0x118), `rollCastChance`
        int32_t cancel_chance = 0;  ///< `CANCEL_CHANCE` (+0x11c)
        /// The level property's `+0x50`: 2 from `CSkillProperty`'s constructor
        /// until `startSkill` copies
        /// `TARGET_ALIGNMENT` over it. `selectOffensiveSkill` and `huntAI` ask whether it is 2.
        int32_t property_alignment = 2;
        /// Neither the block nor the unit authors `LEVEL`: the block's level is
        /// the unit's (`+0x100`), put in by `load_unit_skills`.
        bool level_from_unit = false;
    };
    /// A placed unit's casting: `CCharacter +0x1c8` (its skill manager) and the
    /// fields round `+0x398`.
    struct UnitCasting {
        bool loaded = false;
        /// `CSkillManager +0x60`, in `addSkill` order. A deque, not a vector: a skill's event can
        /// teach the unit another skill (`CBaseUnit::activateEffect`)
        /// while a `const KnownSkill&` into this list is held, and appending to a deque moves
        /// nothing (a vector once did, and the event read freed storage: `std::bad_alloc`).
        /// remove_unit_skill() must not run while one of the unit's events fires.
        std::deque<UnitSkill> skills;
        int active = -1;     ///< `+0x398`, an index into `skills`
        ActiveSkill cast;    ///< `+0x267`, `+0x380`, `+0x37d`, `+0x384`
        SkillRunner runner;  ///< the manager's running list and cooldowns
        float mana = 0.0f;   ///< `+0x438`
        /// The held skill's target position (X360 `CSkill +0x28`) as
        /// `castSkill` last set it for a skill aimed at a place.
        Vec3 skill_point{};
        /// `maxMana` without its effects
        int max_mana = 1;
        /// `fireSkillsOnCreate` has run
        bool create_fired = false;
    };
    /// What a placed unit's skills ask of whoever owns the scene.
    struct UnitSkills {
        /// The unit's `[SKILL]` blocks, resolved to their files, in file order.
        std::function<std::vector<UnitSkill>(const std::string& unit)> load;
        /// The gate's view of the caster (`requirementsMet`) and where it and the target
        /// `(machine, node)` stand. Null machine and node 0: no target; null machine otherwise:
        /// the player.
        std::function<SkillCaster(LevelScript& machine, int64_t node, LevelScript* target_machine,
                                  int64_t target_node, bool has_target)>
            caster;
        /// `CCharacter::castSkill` past its
        /// gates: `performSkill`, the mana and the animation, on the target as
        /// above. True when the skill started.
        std::function<bool(LevelScript& machine, int64_t node, LevelScript* target_machine,
                           int64_t target_node, bool has_target)>
            cast;
        /// `CSkillManager::stopAllSkills(force, false, false)`:
        /// `force` stops every running skill, else each is asked to (`CSkill::attemptToStopSkill`).
        ///
        std::function<void(LevelScript& machine, int64_t node, bool force)> stop_all;
        /// `CSkillManager::stopSkillsByName(name, true)`
        /// on the unit's manager.
        std::function<void(LevelScript& machine, int64_t node, const std::string& name)>
            stop_by_name;
        /// `CGameGlobals +0x60`, `MANA_RECHARGE_RATE`: a percent of the pool
        /// per second (`CCharacter::updateMana`).
        float mana_recharge_rate = 0.0f;
        /// `CBaseUnit::reapplyEffects(false)` from `unitInit`:
        /// the unit's own `[EFFECT]`s, ahead of its `[SKILL]`s.
        std::function<void(LevelScript& machine, int64_t node)> own_effects;
        /// The skill files by name (`CSkill::CSkill`'s facts), and a skill's
        /// definition by upper-cased name: what `activateEffect` teaches with.
        SkillFileLookup skill_files;
        std::function<const SkillDef*(const std::string& upper_name)> skill_def;
        /// Whether loading a skill event's layout `FILE` adds a sound sample
        /// (`layout_adds_sound_sample`), which a skill made for a unit does to
        /// `g_Rand` (`roll_skill_construction`).
        std::function<bool(const std::string& file)> layout_adds_sample;
        /// A skill event's `FILE` loaded as the skill is made (`load_event_file`).
        std::function<void(const std::string& file)> load_event_file;
        /// `CCharacter::executeProcs(type, target)`
        /// on the unit: the target as
        /// `caster` takes one.
        std::function<void(LevelScript& machine, int64_t node, int type,
                           LevelScript* target_machine, int64_t target_node, bool has_target)>
            procs;
    };
    void set_unit_skills(UnitSkills hooks) { unit_skills_ = std::move(hooks); }
    /// `CSkillManager::addSkill` on the character's own manager: a spell the
    /// player's pet has learned (`learnSpell`),
    /// after its `[SKILL]`s. Replaces an entry of the same name.
    bool add_unit_skill(int64_t id, const UnitSkill& skill);
    /// `CSkillManager::removeSkill`: a spell the pet unlearned.
    bool remove_unit_skill(int64_t id, const std::string& name);
    /// A placed character's casting, or null.
    UnitCasting* unit_casting(int64_t node);
    /// The living characters holding a skill, in this machine's character order.
    std::vector<int64_t> casting_units() const;
    /// `CSkillEvent::applyWeaponDamage` with `caster` as the
    /// owner: its `rollAttack` at `WEAPONDAMAGEPCT` and `SOAKSCALEPCT` on the victim (null
    /// machine: the player), as a `Strike`. False when the caster cannot strike.
    bool unit_skill_blow(int64_t caster, LevelScript* victim_machine, int64_t victim_node,
                         float damage_scale, float soak_scale, std::vector<ScriptEvent>& out);

    /// DIVERGENCE: a `Unit Trigger` fires the moment the player walks up to it, for runs with
    /// nobody holding a pad (headless walks). `exempt` names props whose A the original does
    /// not deliver to `interact` (`CCharacter::getItemAI`).
    void enable_proximity_unit_triggers(std::function<bool(const std::string& unit)> exempt = {}) {
        proximity_unit_triggers_ = true;
        proximity_exempt_ = std::move(exempt);
    }

    /// A `Unit Trigger` the pad's second pass may offer: its
    /// node, its unit, and where it stands in this machine's own frame.
    struct UnitTriggerView {
        int64_t id = 0;
        std::string unit;
        Vec3 at;
        /// `LOOP STYLE` (`CTriggerUnit +0x254`): 0 `No Loop`, 1 `Cycle`,
        /// 2 `Back and Forth`.
        int32_t loop_style = 0;
        /// `+0x1f0`: it can be used. `trigger` clears it when a one-shot
        /// reaches its last state; `interact` and the pad's pick test it.
        bool armed = true;
        /// `+0x248`, `+0x24c`, `+0x250`: its state and the first and last.
        int32_t state = 0;
        int32_t first = 0;
        int32_t last = 1;
    };
    /// The enabled ones.
    std::vector<UnitTriggerView> unit_triggers() const;
    /// Every one, enabled or not: `CTriggerUnit::update`
    /// does not ask.
    std::vector<UnitTriggerView> every_unit_trigger() const;
    /// The one at `id`, enabled or not; false when there is none.
    bool unit_trigger(int64_t id, UnitTriggerView& out) const;

    /// The mimics still lying in wait (`MIMICIDLE`), enabled and visible, as
    /// `CLevel::findItemsOnscreen` lists them
    /// beside the items: the node, the unit and where it stands.
    std::vector<UnitTriggerView> idle_mimics() const;
    /// True while the unit holds `MIMICIDLE`.
    bool unit_mimic_idle(int64_t id) const;
    /// `CCharacter::attack` by the player on a mimic lying
    /// in wait: no blow, its `reactToDamage(player, true)`. False when not one.
    bool attack_idle_mimic(int64_t id, QuestState& quests, std::vector<ScriptEvent>& out);

    /// `CTriggerUnit::interact` reaching `trigger`
    /// with the player. Required quest and item are not read.
    void interact_unit_trigger(int64_t id, QuestState& quests, std::vector<ScriptEvent>& out);

    /// The unit half of `CTriggerUnit::unitInit` for a
    /// `Unit Trigger` whose `MODEL` names a unit: `MAXSTATES` (default 2) sets the last state,
    /// `LOOPTYPE` the loop style (-1 unknown) unless the node authors `LOOP STYLE`
    /// (`Set_setLoopType`, stored after), then `resetTrigger`.
    void init_unit_trigger(int64_t id, int32_t max_states, int32_t loop_type);

    /// `CTriggerUnit::update` for a trigger on the near
    /// list: a change that asked to be told broadcasts `Triggered`, then `State One` or `State
    /// Two` for states 0 and 1, on the first update after it: the prop's animation 0 is
    /// `Skeleton::getAnimation(0)`'s `Bind`, never played, so the wait never applies.
    void update_unit_trigger(int64_t id, QuestState& quests, std::vector<ScriptEvent>& out);

    /// `CTriggerUnit::applySaveState`:
    /// `setTriggerState(saved, 0, 1, 0)`, the loop flag per `No Loop`, notify cleared.
    void restore_unit_trigger(int64_t id, int32_t state, std::vector<ScriptEvent>& out);

    /// Record every delivered wire, in the original's trace line format. Off by default.
    // PORT: the logic trace (`--logic-trace`).
    void set_trace(bool on) { trace_on_ = on; }
    /// What a `T` line calls the player (the original writes the class name).
    // PORT: the logic trace (`--logic-trace`).
    void set_trace_player_name(const std::string& name) { trace_player_name_ = name; }
    /// Whether `broadcast` hands each wire on to its input (the default) or only
    /// traces it: a test drives one input and compares the wires it sets off.
    // PORT: for tests.
    void set_wire_delivery(bool on) { deliver_wires_ = on; }
    std::vector<std::string> take_trace() {
        std::vector<std::string> lines;
        lines.swap(trace_);
        return lines;
    }

    /// Build the machine from a parsed layout. `gated` is the set of node ids
    /// held back by a quest gate or a random group.
    void build(const tl::formats::CmpScene& scene, const std::set<int64_t>& gated);

    /// `CEditorScene::setActivated`: each descriptor's scene
    /// activation and every `Logic Group`'s `Level Activated`. Once, after placing.
    void level_activated(QuestState& quests, std::vector<ScriptEvent>& out);

    /// The scene going away: `CSoundObjectDescriptor::descriptorSceneDeactivated`
    /// disables every `Sound` node. Call
    /// before the machine is dropped.
    void level_deactivated(QuestState& quests, std::vector<ScriptEvent>& out);

    /// One frame in `CGameClient::updateIngame`'s order:
    /// level (units, then `iLevelUpdate`), interface (dialogs), then descriptors in
    /// `CLayoutScene::createDescriptors` order. The level runs on `delta` times the game speed.
    void update(float delta, const Vec3& player, QuestState& quests, std::vector<ScriptEvent>& out);

    /// `CGameSpeed::getGameSpeed()` times the client's multiplier: `CLevel::update`
    /// runs the near-unit timer, level-update objects and
    /// characters (`CLevel::updateCharacters`) on it, and
    /// `CLevel::updateLayouts` the descriptors.
    void set_game_speed(float speed) { game_speed_ = speed; }

    /// Only the characters' half of `update`: for a room whose wires wait on a
    /// `Play` but whose units are the level's all the same.
    void update_units(float delta, const Vec3& player, QuestState& quests,
                      std::vector<ScriptEvent>& out);

    /// `CGameClient::updateIngame`'s paused branch: the quest dialog's
    /// NPC (`CGameUI::getQuestDialogNPC`), when a character, still runs
    /// `CCharacter::update` and `updateAnimation`
    /// while the level stands still. Whether this machine had it.
    bool update_paused_speaker(const std::string& unit, float frame_delta, QuestState& quests,
                               std::vector<ScriptEvent>& out);

    /// Whether this machine runs any character (the pet included).
    bool has_characters() const { return !characters_.empty(); }

    /// `Play` on a `Layout Link Timeline`: run the timelines this layout marks
    /// `LAYOUT CONTROL`.
    void play_layout_timelines(QuestState& quests, std::vector<ScriptEvent>& out, bool backwards);
    void stop_layout_timelines(QuestState& quests, std::vector<ScriptEvent>& out);
    /// `CLayout::callFunctionOnObjects(stop)` on a timeline or particle
    /// layout's `Sound` nodes: those whose bank loops are stopped, the rest play out.
    void stop_layout_sounds(QuestState& quests, std::vector<ScriptEvent>& out);
    /// The layout going away: each `Sound` node's bank destroyed (`sound_bank_destroy`).
    void release_sound_nodes();
    /// `CLayout::initOnLoad()`, for a link that does not
    /// start on load: every `Timeline` loses its own `START ON LOAD`.
    void init_on_load();

    /// Push a point out of every unit it is standing inside, by at most `budget`.

    /// `CGameClient::getIsPaused`, the modal-menu half
    /// (the cinematic menu is modal: `CGameUI::create`).
    void set_modal_open(bool open) { modal_open_ = open; }

    /// Set every monster the level placed on its idle AI.
    size_t arm_hostiles();

    /// `CLevel::addCharacter` ends with `dropToGround(4.0)`:
    /// every character placed here stood on the floor once the collision is in.
    void drop_characters_to_ground(std::vector<ScriptEvent>& out);

    bool node_targetable(int64_t id) const;
    /// `CSceneNodeObject::getEnabled` on a node this machine holds; a node it
    /// does not hold is enabled.
    bool node_enabled(int64_t id) const;

    /// CLogicTimer +0x60 remaining time, for test verification.
    float timer_left(int64_t id) const;
    /// Reset a timer node (`CLogicTimer::resetTimer`).
    bool timer_reset(int64_t id);
    /// Trigger a roll on a Random Choice node (`CRandomChoice::roll`).
    ///
    bool roll_random_choice(int64_t id, QuestState& quests, std::vector<ScriptEvent>& out);

    /// CPuzzleRandomizer fields, for test verification.
    uint32_t puzzle_active_inputs(int64_t id) const;
    uint32_t puzzle_order_at(int64_t id, size_t index) const;
    bool puzzle_solved(int64_t id) const;
    bool send_puzzle_input(int64_t id, uint32_t in, QuestState& quests,
                           std::vector<ScriptEvent>& out);

    /// An input (`kDamageShapeIn`: 2 on, 3 off) to the `Damage Shape` `id`.
    bool send_damage_shape_input(int64_t id, uint32_t in, QuestState& quests,
                                 std::vector<ScriptEvent>& out);
    /// `CDamageShape +0x1c8`: a skill's event joins (`TArrayList::add`, at the
    /// end) or leaves the listeners of the `Damage Shape` `id`. False when
    /// there is no such shape.
    bool damage_shape_add_listener(int64_t id, DamageShapeListener* listener);
    /// The level's items a damage shape can reach (`CLevel +0xa0`, the ones
    /// `ISA 0x1d`): the renderer keeps the breakables.
    struct LevelItems {
        /// The breakables still standing, newest first.
        std::function<std::vector<DamageShapeItem>()> list;
        /// `CItem::interact(nullptr)` (vtable +0x280).
        std::function<void(int64_t item)> interact;
    };
    void set_level_items(const LevelItems* items) { level_items_ = items; }
    /// `CLevel::updateItems`: every update listener's
    /// `itemUpdatedInLevel` for a breakable in range. On the level's own machine, per item.
    void item_updated_in_level(const DamageShapeItem& item);
    bool damage_shape_remove_listener(int64_t id, DamageShapeListener* listener);
    /// `CSkillEvent::addListeners`'s two writes into a
    /// shape: `TARGET_CORPSES`, and a `MAX_UNITS_HIT` above 0 replacing the shape's own.
    bool damage_shape_take_event(int64_t id, bool target_corpses, int32_t max_units_hit);
    /// A `Damage Shape`'s state, for tests and the renderer; null when `id` is
    /// not one.
    const DamageShape* damage_shape(int64_t id) const;
    /// `CLevel +0x1a8`, the level's monster level (what `populateSectionOfLevel`
    /// spawns at): where a `Damage Shape` reads its damage curve.
    // PORT: the level machine's wiring, set by the renderer.
    void set_level_monster_level(int32_t level) { level_monster_level_ = level; }
    /// `CRandomNames::getSingleton()`, which names a `CHAMPION` as it is made.
    // PORT: the level machine's wiring, set by the renderer.
    void set_random_names(const RandomNames* names) { random_names_ = names; }
    /// The unit-type hierarchy, for a `Damage Shape`'s `UNITTYPE` and the ISA
    /// it tests. Set before `build`.
    // PORT: the level machine's wiring, set by the renderer.
    void set_unit_types(const UnitTypeHierarchy* types) { unit_types_ = types; }

    /// What making a character draws, in `CResourceManager::createUnit`'s order:
    /// `CBaseUnit::unitInit`'s scale
    /// (`SCALE + randomBetweenVolatile(0, SCALE_VARIATION)`), then `CCharacter::unitInit`'s
    /// `setLevel(LEVEL, true)` and hit points.
    struct UnitCreation {
        float scale = 1.0f;
        UnitCombat combat;
        /// What it holds after `CCharacter::createDefaultEquipment`:
        /// authored hands, then rolled, and the right-hand weapon's attack
        /// (`UnitStats::weapon_attack`).
        std::string right_hand;
        std::string left_hand;
        UnitStats::WeaponAttack weapon_attack;
        UnitStats::WeaponAttack left_weapon_attack;
        /// The name `makeChampion` gave a `CHAMPION` (`CRandomNames::
        /// generateName`); empty for anything else.
        std::string champion_name;
        /// The character's own inventory (`CCharacter::unitInit`) after
        /// `createDefaultEquipment`; null when the machine has no item book.
        std::shared_ptr<Inventory> inventory;
        /// The figures the inventory's `canEquip` asks of a unit with
        /// `USEWEAPONDAMAGE` (`CharacterProgression::init_unit`); null for one
        /// without, which answers no requirements.
        std::shared_ptr<CharacterProgression> sheet;
        /// The unit's own `[EFFECT]`s as `CBaseUnit::reapplyEffects` made them
        /// (each constructor's roll run), for its effect manager.
        std::vector<Effect> own_effects;
        /// The unit's own effect manager (`+0x17c`) as `CBaseUnit::unitInit`
        /// leaves it: its own effects, then its `[AFFIXES]`; null for a unit
        /// with neither, or a machine with no effect tables.
        std::shared_ptr<EffectManager> effects;
    };
    /// What `CCharacter::createDefaultEquipment` makes a
    /// character's equipment with.
    struct UnitEquipment {
        const ItemBook* items = nullptr;
        CreateEquipment create;
        RollSpawnClass roll;
        EnchantEquipment enchant;
        /// The attack of the weapon an item is (`CEquipment::calculateCombatStats`),
        /// for a hand.
        std::function<UnitStats::WeaponAttack(const std::string& item, Hand hand)> weapon_attack;
    };
    // PORT: the level machine's wiring, set by the renderer.
    void set_unit_equipment(UnitEquipment equipment) { unit_equipment_ = std::move(equipment); }
    /// `default_equipment` false: a unit made from a save skips
    /// `createDefaultEquipment`. `level_default` above 0 is what `LEVEL` falls back to; 0 is
    /// the level's monster level.
    UnitCreation roll_unit_creation(const std::string& unit, bool default_equipment = true,
                                    int level_default = 0);
    /// A placed Monster's unit, made as its node was (`CMonsterDescriptor::Set_createNewCharacter`,
    /// in the file's creation pass): `build` makes the node's character from it.
    void preset_unit_creation(int64_t node, UnitCreation made) {
        preset_creations_[node] = std::move(made);
    }

    /// `createDefaultEquipment`'s spawn-class half (`loadAndEquipSpawnClass`),
    /// which needs the spawn classes: called for
    /// every character made, with the authored hands already in `made`.
    using EquipmentRoller = std::function<void(const std::string& unit, UnitCreation& made)>;
    void set_equipment_roller(EquipmentRoller roller) { equipment_roller_ = std::move(roller); }
    /// What a character holds in its right and left hand; false for a node
    /// that is no character.
    bool unit_hands(int64_t id, std::string& right, std::string& left) const;
    /// The left hand's weapon is the one in use (`+0x3ec`, `selectAttack`).
    bool unit_attacks_left(int64_t id) const;

    /// A unit a `Unit Spawner` has just released. `creation`, when given, is
    /// what making it already drew; otherwise it is drawn here.
    void add_spawned_unit(int64_t id, const std::string& unit, const Vec3& at, bool hunts,
                          float facing = 0.0f, const UnitCreation* creation = nullptr);

    /// `CCharacter::makeChampion(name, theme)`, as
    /// `CLevel::populateSectionOfLevel` runs it: the figures
    /// and scale + 0.2; name and theme are the caller's. False for a non-character or champion.
    bool make_unit_champion(int64_t id);

    /// A character's scale (`+0x98`); 0 for a node that is not one.
    float unit_scale(int64_t id) const;

    /// `CFormationNode::updateLevelObject`:
    /// records every formation member on every other member in an all-to-all
    /// peer pattern.
    void link_formation(const std::vector<int64_t>& unit_ids);

    /// `CCharacter::spawn`: AI state 0xc while the entrance
    /// plays (no AI, no turning, no route). Off: `spawningAI`,
    /// state 0, or 4 with a target.
    void set_spawning(int64_t id, bool spawning);

    /// A unit that moved under its own steam (the pet).
    void move_unit(int64_t id, const Vec3& at);

    void remove_spawned_unit(int64_t id);

    /// The `CUnitSpawner` the machine holds for node `id`, or null.
    UnitSpawner* unit_spawner(int64_t id);
    /// Every spawner, in the order `CUnitSpawnerDescriptor::update`
    /// walks its objects: the order the layout made them.
    std::vector<std::pair<int64_t, UnitSpawner*>> unit_spawners();
    /// An output raised by a node from outside the machine - a spawner's
    /// (`CUnitSpawner` vtable +0x30) - and whatever it is wired to.
    void raise_output(int64_t id, uint32_t output, QuestState& quests,
                      std::vector<ScriptEvent>& out);

    /// A unit's animation passed a `HIT` key: `CCharacter::updateAttack`
    /// strikes if a swing is running.
    void attack_hit(int64_t id, QuestState& quests, std::vector<ScriptEvent>& out);

    /// No attacker at all, as a `Damage Shape` passes:
    /// `applyDamage` then skips `reactToDamage`
    /// (the area flag clear).
    static constexpr int64_t kNoAttacker = -1;

    /// Damage dealt to a script-run unit by anything: `CCharacter::applyDamage`,
    /// then `CLevel::reactToAreaDamage(attacker, victim)`:
    /// every living friend (`CCharacter::isFriend`) within its own
    /// `DAMAGE_REACT_RADIUS`, edge to edge (`CCharacter::inDamageReactRange`),
    /// reacts to the attacker. `victim` null is the player.
    void react_to_area_damage(int64_t attacker, LevelScript* attacker_machine,
                              LevelScript* victim_machine, int64_t victim, const Vec3& victim_at,
                              float victim_radius, QuestState& quests,
                              std::vector<ScriptEvent>& out);
    /// `CMonster::reactToDamage` against `attacker` (0 = the player). True when the blow
    /// killed it. `struck` (`applyDamage`'s 8th argument: blows, missiles, instant `DAMAGE`)
    /// fires `CAST SKILL ON STRUCK` procs first (`executeProcs(0x51, attacker)`).
    /// `shown`: add to the number over the unit, as `applyDamage` does; else only `modifyHP`.
    bool damage_unit(int64_t id, float amount, int64_t attacker, QuestState& quests,
                     std::vector<ScriptEvent>& out, LevelScript* attacker_machine = nullptr,
                     const KillingBlow* blow = nullptr, bool struck = false, bool shown = true);
    /// `CCharacter::updateHP`: the damage of the unit's effects
    /// this frame, added to the number over it.
    void add_unit_damage_shown(int64_t id, float amount);

    /// What `CPropertyNode::killOrMoveBaseUnits` reads of a `Path Node Occupation Box` or
    /// `Circle`: position, bounds (`updateBoundingBox`), whether a
    /// circle (`TYPE` 8), and scale X.
    struct OccupationArea {
        Vec3 centre;
        Vec3 min;
        Vec3 max;
        bool circle = false;
        float scale_x = 0.0f;
    };
    /// `CPropertyNode::killOrMoveBaseUnits()` as the node
    /// goes into the map (`updatePathNodes`): a player's pet in the way is put back by
    /// its master; anything else inside loses all its hit points (`killed` lists them).
    void kill_or_move_base_units(const OccupationArea& area, QuestState& quests,
                                 std::vector<ScriptEvent>& out, std::vector<int64_t>& killed);
    /// `CCharacter +0x708 = 1.0`, set when one of the unit's own procs casts
    /// (`executeProcs`).
    void lock_unit_procs(int64_t id);

    /// `CCharacter::transformCharacter(unit, duration)`, from
    /// `TRANSFORM` (0x39) or `TRANSFORM PERMANENT` (0x49, -999): the character becomes `unit`,
    /// keeping node, place, side and effects; a `Transform` event. False when not possible.
    bool transform_unit(int64_t id, const std::string& unit, float duration, QuestState& quests,
                        std::vector<ScriptEvent>& out);
    /// The unit a character was before it was transformed; empty for none.
    std::string unit_transformed_from(int64_t id) const;
    /// `+0x688`: the seconds its transformation has left, -999 for one that
    /// lasts, 0 for none.
    float unit_transform_left(int64_t id) const;
    /// Both fields put back on a character made again: what
    /// `CCharacter::applySaveState` does with
    /// a saved pet's, and what the original's pet simply keeps across a warp.
    bool restore_unit_transformation(int64_t id, float left, const std::string& from);

    /// A unit's hit points (`+0x414`) and `maxHP()`; false for a node that is
    /// not a character.
    bool unit_health(int64_t id, float& hp, float& max) const;

    /// What a script-run unit brings to a blow it takes (`rollAttack`'s
    /// `AC()`, `maxHP()`, `damageDefense(type)`); false for a node that is
    /// not a character.
    bool unit_defender(int64_t id, Defender& out) const;
    /// A unit's own `+0x620`, by `EDAMAGE_TYPES`, as `applyDamageEffects`
    /// reads it - without the effects
    /// `damageDefense` adds.
    bool unit_raw_defense(int64_t id, std::array<int, kDamageTypeCount>& out) const;

    /// A unit's level (`+0x100`); false for a node that is not a character.
    bool unit_level(int64_t id, int& level) const;

    /// `CCharacter::setLevel(level, true)` again, for a unit
    /// given its own level (one more hit-point draw).
    void set_unit_level(int64_t id, int level);

    /// A unit's experience (`+0x448`); false for a node that is not a character.
    bool unit_experience(int64_t id, int32_t& experience) const;
    void set_unit_experience(int64_t id, int32_t experience);

    /// `CCharacter::awardExperience` on every pet as the
    /// player's award runs it, and `CCharacter::levelUp` at
    /// the gate, one level an award. Those that rose join `levelled`.
    void award_pet_experience(int amount, std::vector<int64_t>& levelled);

    /// Where a script-run unit stands, feet on the ground; false when it is not
    /// one.
    bool unit_position(int64_t id, Vec3& at) const;
    /// A unit's final target position (X360 +0x1c0): where `castSkill` aimed
    /// a skill at a place.
    bool unit_final_target(int64_t id, Vec3& at) const;
    /// A script-run unit's collision radius (`+0x194`), 0 when it is not one.
    float unit_radius(int64_t id) const;
    /// A script-run unit's target (`CCharacter +0x340`): false when it holds
    /// none. A null `machine` is the player.
    bool unit_target(int64_t id, LevelScript*& machine, int64_t& node) const;
    /// A script-run unit's turn (the yaw of `+0xb4`); false when it is not one.
    bool unit_facing(int64_t id, float& yaw) const;
    /// Where the node `id` stands (`CPositionableObject` position); false when
    /// there is no such node.
    bool node_position(int64_t id, Vec3& at) const;
    /// Where the node stands in its own file (`getPosition(false)`); false
    /// when there is no such node.
    bool node_local_position(int64_t id, Vec3& at) const;

    /// A character's `CEffectManager` (`+0x1b8`), null when it holds none or
    /// is not a character.
    EffectManager* unit_effects(int64_t id);
    /// The same, made with `context` when it has none; null for a node that is
    /// not a character (a corpse still has one).
    EffectManager* ensure_unit_effects(int64_t id, const EffectManager::Context& context);
    /// What a unit's own effect manager is made with (`CEffectManager`'s
    /// tables: effect types, curves, unit themes); its owner and its activation
    /// are the unit's (`load_own_effects`).
    // PORT: the level machine's wiring, set by the renderer.
    void set_unit_effect_tables(const EffectManager::Context& tables) {
        unit_effect_tables_ = tables;
        has_unit_effect_tables_ = true;
    }
    /// The affixes a unit's `[AFFIXES]` name (`CEffectGroupManager`).
    // PORT: the level machine's wiring, set by the renderer.
    void set_affix_book(const AffixBook* affixes) { affix_book_ = affixes; }
    /// The sound definitions (`CMasterResourceManager +0x80`), for what filling
    /// a character's sound banks does to `g_Rand` as it is made.
    // PORT: the level machine's wiring, set by the renderer.
    void set_sound_data(const SoundDataInformation* sounds) { sound_data_ = sounds; }
    /// Whether the sound system starts a sound it is asked to play; past that
    /// `CSoundManager::` `playSound` draws the volume variation. Unset: never.
    // PORT: the level machine's wiring, set by the renderer.
    void set_audio_plays(std::function<bool()> plays) { audio_plays_ = std::move(plays); }
    /// Whether a unit's model, as `CCharacter::loadModel`
    /// loads it, adds a sound sample from its animations' keys
    /// (`manifest_adds_sound_sample`), by unit name.
    // PORT: the level machine's wiring, set by the renderer.
    void set_model_sounds(std::function<bool(const std::string& unit)> adds) {
        model_sounds_ = std::move(adds);
    }
    /// `CBaseUnit::activateEffect(CEffect*)`
    /// on a level unit: a cast-skill effect teaches it its skill.
    void activate_unit_effect(int64_t node, Effect& effect);
    /// A character's own `[EFFECT]`s as it was made (rolled), or null.
    // PORT: an accessor of the level machine's unit records.
    const std::vector<Effect>* unit_own_effects_made(int64_t id) const;
    /// The characters of this machine holding an effect manager, in order.
    std::vector<int64_t> units_with_effects() const;
    /// `CCharacter::interrupt(true)`.
    void interrupt_unit(int64_t id, QuestState& quests, std::vector<ScriptEvent>& out);
    /// `CCharacter::updateEffects`, knock-back's end: `add`
    /// joins the outside push, height zeroed, length held to 30 (a player's: 10).
    void knock_back_unit(int64_t id, const Vec3& add);
    /// `CCharacter::updateEffects`' `STUN` half: AI state
    /// 0x28 and still while stunned (not when dying, dead or in 0xc, 0x10, 0x29), state 0 after.
    /// A `DAMPVELOCITY` (`damp`) or `UNDAMPVELOCITY` key: `CCharacter::updateAnimation`
    /// (key types 0x12, 0x13) zeroes the velocity or restores it.
    void damp_unit_velocity(int64_t id, bool damp);
    /// `+0x258`, the unit's velocity scale; -1 for no character.
    float unit_velocity_scale(int64_t id) const;
    bool set_unit_stunned(int64_t id, bool stunned, QuestState& quests,
                          std::vector<ScriptEvent>& out);
    /// `modifyHP` with a positive amount: up to `maxHP()`, an integer.
    void heal_unit(int64_t id, float amount);
    /// `CCharacter::modifyHP` with a heal on any character
    /// here; a body takes only a heal of at least `maxHP()`. True when it filled a body from
    /// exactly 0, which clears `GIVE XP`.
    bool modify_unit_hp(int64_t id, float amount);
    /// `CSkillEvent::applyEffects`, its end:
    /// a character that was not alive before the effects (`was_alive`) and has
    /// hit points after them gets up - `setAIState(2)`. True when it did.
    bool unit_gets_up(int64_t id, bool was_alive, QuestState& quests,
                      std::vector<ScriptEvent>& out);
    /// `reactToDamage(attacker, true)` (vtable +0x350) with the player as the
    /// attacker, as an effect the player applied provokes it.
    void provoke_unit(int64_t id, QuestState& quests, std::vector<ScriptEvent>& out);
    /// The unit's AI state (`+0x330`), -1 when it is not one.
    int32_t unit_ai(int64_t id) const;

    /// A character a missile can find and strike: `CLevel::findCharacterWithinView`
    /// 's walk, and `CLevel::preSortedSphereCollision`'s unit half.
    struct Reachable {
        int64_t id = 0;
        std::string unit;     ///< its unit's `NAME`
        Vec3 center;          ///< `getPosition(true)`: the body sphere's centre
        float radius = 0.0f;  ///< `+0x194`, the body radius (feet are `center.y - radius`)
        UnitStats::Alignment alignment = UnitStats::Alignment::Hostile;
        /// It holds the `MIMICIDLE` theme: `CLevel::findCharacterWithinView`
        /// passes it over, though a missile
        /// can still meet it.
        bool mimic_idle = false;
    };
    /// This machine's living, visible, targetable characters, in its character
    /// order, less the unit types 0x29/0x2a the level's search skips.
    void reachable_characters(std::vector<Reachable>& out) const;

    /// A placed unit has been killed: `Monster Killed`.
    void unit_killed(int64_t id, QuestState& quests, std::vector<ScriptEvent>& out);
    /// `CCharacter::die`'s `playSample(12)`, after its procs: the unit's
    /// `DEATH_SOUND`, unless its death already played it.
    void play_death_sample(int64_t id, std::vector<ScriptEvent>& out);
    /// While set, a death leaves its sample to `play_death_sample` (the caller runs the
    /// death's procs first).
    void set_defer_death_sample(bool defer) { defer_death_sample_ = defer; }
    /// `CCharacter::die`'s `findRandomAnimation("DIE")` for a death that
    /// neither flies nor bursts: the name, empty when the model has none.
    std::string death_animation(int64_t id) const;
    bool unit_is_dead(int64_t id) const;
    /// Whether any unit of this machine is dead (`unit_is_dead` is false for every id if not).
    bool has_dead_units() const { return !dead_.empty(); }

    /// The player pressed the skip key: `CSkipCutscene::update`
    /// sees Escape on an enabled node and
    /// marks it; the skip itself runs on the node's next update.
    void skip_cutscene(QuestState& quests, std::vector<ScriptEvent>& out);

    /// The player clicked through the open dialog (`OK`, menu event 2).
    void dismiss_dialog(QuestState& quests, std::vector<ScriptEvent>& out);
    /// The player answered an offer: `Accept` (3) or `Decline` (4).
    void answer_dialog(bool accept, QuestState& quests, std::vector<ScriptEvent>& out);
    /// Whether a boxed line waits for the player. Off (a headless run), it
    /// closes itself after its reading time, accepting an offer.
    // PORT: the level machine's wiring, set by the renderer.
    void set_dialogs_wait_for_player(bool wait) { dialogs_wait_ = wait; }

    const std::string& dialog_text() const;
    bool dialog_open() const { return !dialogs_.empty() && showing_; }
    bool floaty_open() const { return floaty_.showing; }

    /// Does nothing: the path tracks the player itself (`CPlayer::update`).
    ///
    void player_arrived(int64_t, QuestState&, std::vector<ScriptEvent>&) {}

    /// Does nothing: the controller times its own animation (`CAnimationPlayer::update`).
    ///
    void animation_finished(int64_t, QuestState&, std::vector<ScriptEvent>&) {}

    size_t object_count() const { return objects_.size(); }
    /// Whether any `Sound` node stands in this layout: its bank is stepped
    /// every frame (`CSoundObjectDescriptor::update`), wired or not.
    bool has_sound_nodes() const;
    size_t wire_count() const;
    /// Whether any object has a wire: `wire_count() != 0` without the count.
    bool has_wires() const;
    size_t trigger_count() const;
    size_t timeline_count() const;
    bool timelines_playing() const;

    /// Whether anything is running: a timeline playing, a camera taken or a
    /// dialog up.
    bool busy() const;

    /// Every distinct wire this engine did not implement, in name order.
    std::vector<std::string> unimplemented() const;

    // Level state (`CLevelState`).

    /// `CLevel+600`: the level has a remembered state. Set before `level_activated`, so
    /// spawn-on-create spawners hold back (`CUnitSpawnerDescriptor::descriptorSceneActivated`).
    ///
    void set_has_saved_state(bool on) { has_saved_state_ = on; }
    /// `CGameClient::storedUnitExists(long long)` (inlined in
    /// `convertCharacter`): the saved state of this level lists
    /// a character with object id `id`. Unset: none.
    void set_stored_unit_exists(std::function<bool(int64_t id)> exists) {
        stored_unit_exists_ = std::move(exists);
    }

    /// `CPlayer::storeLevelSavedState`, one room layout's
    /// loop: this machine's logic nodes as `CLogicNodeState`s; `hierarchy` is the link
    /// chain's hash.
    void store_logic_states(uint32_t scene, int64_t hierarchy,
                            std::vector<tl::formats::LogicNodeState>& out) const;

    /// `CGameClient::restoreLevelState`, the
    /// same loop the other way.
    void restore_logic_states(uint32_t scene, int64_t hierarchy,
                              const std::vector<tl::formats::LogicNodeState>& states,
                              QuestState& quests, std::vector<ScriptEvent>& out);

    /// One character as `CCharacter::fillSaveState`
    /// sees it, in the fields this engine
    /// models.
    struct CharacterRecord {
        int64_t node = 0;
        std::string unit;
        bool from_layout = false;  ///< `+0x191`: a `Monster` the layout placed
        Vec3 position;             ///< feet (`getPosition(true)` less the radius)
        float facing = 0.0f;
        float hp = 0.0f;  ///< `+0x414`
        int level = 0;    ///< `+0x100`
        /// What it holds in equip locations 0 and 1 (`CCharacter::fillSaveState`
        /// writes its inventory). `equipment` says
        /// the record carries them, for `apply_character_record`.
        bool equipment = false;
        std::string right_hand;
        std::string left_hand;
        UnitStats::WeaponAttack weapon_attack;
        UnitStats::WeaponAttack left_weapon_attack;
    };

    /// The characters `storeLevelSavedState` keeps: alive, not deleted, not
    /// moved from outside (the pet). The original's second pass (units with a
    /// master) finds nothing here: no monster in this engine has one.
    std::vector<CharacterRecord> living_characters() const;

    /// `CCharacter::applySaveState` on a
    /// character this machine runs: position, facing, level, hit points. False
    /// when there is no such one.
    bool apply_character_record(const CharacterRecord& record, std::vector<ScriptEvent>& out);

    /// The layout monsters of a restored dungeon level the state does not hold:
    /// `CMonsterDescriptor::descriptorSceneActivated` marks
    /// each, `restoreLevelState` clears those it finds, and `CLevel::updateCharacters`
    /// deletes the rest. Returns the ids deleted.
    std::vector<int64_t> delete_placed_monsters_except(const std::set<int64_t>& kept,
                                                       std::vector<ScriptEvent>& out);

    /// A `Warper` of this machine, for `CLevel::placePlayerAtWarpToDungeonFloor`.
    struct WarperRecord {
        int64_t node = 0;
        Vec3 position;
        Vec3 forward;
        std::string dungeon;         ///< +0x110
        std::string warp_name;       ///< +0x40
        int32_t level_delta = 1;     ///< +0x100
        int32_t level_absolute = 0;  ///< +0x104
    };
    std::vector<WarperRecord> warpers() const;

    /// A node of one descriptor type: its id, unit, name and where it is.
    struct NodeRecord {
        int64_t node = 0;
        std::string unit;
        std::string name;
        Vec3 position;
        bool enabled = true;
        /// A `Property Node`: its `TYPE`, `RADIUS`, `BOX WIDTH` and `BOX LENGTH`.
        std::string kind;
        float radius = 0.0f;
        float box_width = 0.0f;
        float box_length = 0.0f;
        /// Its world axes (after the machine's frame): what
        /// `CPropertyNode::updateBoundingBox` turns its box by.
        Vec3 right{1.0f, 0.0f, 0.0f};
        Vec3 up{0.0f, 1.0f, 0.0f};
        Vec3 forward{0.0f, 0.0f, 1.0f};
    };
    std::vector<NodeRecord> nodes_of_type(uint32_t type) const;

    /// The player has just been stood at `player`. DIVERGENCE, with
    /// `enable_proximity_unit_triggers()`: a `Unit Trigger` within reach counts as already
    /// entered, so arriving on the mine entrance does not send the player straight back.
    void arrive_at(const Vec3& player);

    /// The machine's clock: the sum of every `update` delta, which is what the
    /// original's `CLevel::update` accumulates for its logic.
    float clock() const { return clock_; }

    // PORT: a test of `build` reads the objects it made.
    friend struct LevelScriptProbe;
    friend struct LevelScriptMirror;
    friend struct LevelScriptSides;

private:
    /// A `CLogicLink`: one output of the owning object wired to one input of
    /// another. Resolved to ids at build, against each descriptor's vocabulary.
    struct Link {
        uint32_t output = logic::kNone;
        int64_t target = 0;
        uint32_t input = logic::kNone;
    };

    /// A `CLogicObject`: the object's membership of one logic group, with its
    /// links in authored order.
    struct Membership {
        std::vector<Link> links;
    };

    /// One track of a timeline on one target, as `CTimelineProperty::CTimelineProperty`
    /// starts it: no point (-1), last applied at -0.01,
    /// interpolating as the timeline (7) until its keys say otherwise.
    struct Track {
        int64_t target = 0;
        const tl::formats::CmpTimelineTrack* data = nullptr;
        std::optional<TimelineProperty> property;
        float last = -0.01f;  ///< +0x24, the percent last applied
        TimelineInterpolation interpolation = TimelineInterpolation::Linear;  ///< +0x48
    };

    /// A character of the level's list: the machine that runs it and its node,
    /// or the player (no machine, node 0). A node id alone is not enough - two
    /// copies of one room file carry the same ids.
    struct UnitRef {
        LevelScript* machine = nullptr;
        int64_t node = 0;
        // PORT: a reference to a unit of a machine, or the player.
        bool player() const { return machine == nullptr; }
        bool operator==(const UnitRef& other) const {
            return machine == other.machine && node == other.node;
        }
        // PORT: a reference to a unit of a machine, or the player.
        bool operator!=(const UnitRef& other) const { return !(*this == other); }
    };

    struct Object {
        int64_t id = 0;
        uint32_t type = 0;
        std::string name;
        int64_t parent = 0;
        Vec3 position;
        /// Where it stands in its own file (`getPosition(false)`): the build's
        /// value before the room's frame, and a `POSITION` key's since.
        Vec3 local_position;
        Vec3 forward;
        Vec3 right;
        Vec3 up;
        /// `CPositionableObject +0xc0`'s columns (right, up, forward) in its own
        /// file, for a node a timeline turns: read from the node when its first
        /// `RIGHT`/`UP`/`FORWARD` track is made, each key replacing one column.
        bool has_local_axes = false;
        Vec3 local_right{1.0f, 0.0f, 0.0f};
        Vec3 local_up{0.0f, 1.0f, 0.0f};
        Vec3 local_forward{0.0f, 0.0f, 1.0f};
        std::vector<Membership> logic;

        /// `CSceneNodeObject` +0x82 / the descriptor's own enabled flag.
        bool enabled = true;
        /// The node it was built from, for a `Camera Shake` (null otherwise).
        const tl::formats::CmpNode* source = nullptr;
        bool visible = true;

        // Monster (`CMonster`).
        std::string unit;
        bool spawned = false;  ///< not placed by the layout (`+0x191` clear)
        /// The port's targetable-and-alive: `unit_killed` clears it, which is
        /// how the renderer's selectors, fidgets and reach pass a corpse by.
        bool targetable = true;
        /// `+0x531` `TARGETABLE`: the layout's value and the two targeting inputs only;
        /// `CCharacter::die` leaves it, so a corpse is still
        /// offered to a `TARGET CORPSE` shape (`characterUpdatedInLevel`).
        bool targetable_flag = true;
        bool invulnerable = false;
        int32_t ai = ai_state::kIdle;  ///< +0x330
        int64_t path = 0;              ///< +0x750, the Pathing node followed
        /// ... and the machine that runs that node: a `Pathing` node drives
        /// units of the whole level (`CLevel::getBaseUnitsByDataGroup`), so the
        /// path may sit in another room's machine. Null with `path` 0.
        LevelScript* path_machine = nullptr;
        /// A Monster's `PATH` (+0x760), resolved at scene activation; a
        /// Pathing node's `PATH NAME` (`CPathController +0x188`).
        std::string path_name;
        float path_pct = -1.0f;           ///< +0x1e8
        bool path_forward = true;         ///< +0x770
        bool path_loops = false;          ///< +0x771
        bool cannot_wander = false;       ///< +0x7f0 CANNOT WANDER
        bool fire_all_hp_events = false;  ///< CMonsterDescriptor index 8 / +0x1d1
        // The player's path stand-in only (`path_follow` without a character).
        bool has_destination = false;
        Vec3 destination;
        bool running = false;
        bool no_target = false;  ///< +0x70c NO TARGET
        std::string animation;   ///< the AI's play-animation (+0x2a8)
        float animation_left = 0.0f;
        bool animation_loops = false;

        // Timer (`CLogicTimer`).
        float timer_time = 1.0f;       ///< +0x58 TIME
        float timer_max = -1.0f;       ///< +0x5c MAXTIME
        float timer_left = 1.0f;       ///< +0x60
        int32_t timer_loops = 1;       ///< +0x64 LOOP COUNT
        int32_t timer_loops_left = 1;  ///< +0x68
        bool timer_forever = false;    ///< +0x6d LOOPS FOREVER

        // Output Incrementor (`COutputIncrementor`, its constructor's
        // defaults; the count is `counter_value`, `+0x5c`).
        int32_t incr_max = 5;         ///< +0x58 MAX OUTPUT
        int32_t incr_loop_count = 1;  ///< +0x60 LOOP COUNT
        int32_t incr_loops_left = 1;  ///< +0x64
        bool incr_forever = false;    ///< +0x69 LOOPS FOREVER

        // Counter (`CCounter`).
        int32_t counter_equals = 10;  ///< +0x58 EQUALS VALUE
        int32_t counter_start = 0;    ///< +0x5c STARTING VALUE
        int32_t counter_value = 0;    ///< +0x60
        int32_t counter_type = 0;     ///< +0x64

        // Player Sphere / Box Trigger (`CLogicTrigger`).
        float radius = 0.0f;  ///< +0x104 RADIUS
        Vec3 half_extent;
        bool inside = false;          ///< +0x100
        bool triggered_once = false;  ///< +0x101
        /// Unit Trigger (`CTriggerUnit`), at `CTriggerUnit::CTriggerUnit`'s values:
        /// state and first 0, last 1, `No Loop`, forwards, not
        /// notifying. `LOOP STYLE`: 0 `No Loop`, 1 `Cycle`, 2 `Back and Forth`.
        int32_t trigger_loop_style = 0;
        bool trigger_loop_authored = false;
        bool trigger_armed = true;      ///< +0x1f0
        int32_t trigger_state = 0;      ///< +0x248
        int32_t trigger_first = 0;      ///< +0x24c
        int32_t trigger_last = 1;       ///< +0x250 (`MAXSTATES` - 1)
        int32_t trigger_direction = 0;  ///< +0x258, `Back and Forth`: 1 going back
        bool trigger_notify = false;    ///< +0x25d, `update` broadcasts the state
        bool deactivated_once = false;  ///< +0x102

        // Timeline (`CTimeline`).
        const tl::formats::CmpTimeline* timeline = nullptr;
        std::vector<Track> tracks;
        bool tl_loops = false;           ///< +0x89
        bool tl_start_on_load = false;   ///< +0x8a
        bool tl_layout_control = false;  ///< +0x8b
        /// `+0xa8`, the timeline's `TYPE` (`gTIMELINE_MODIFICATION_TYPE_NAMES`:
        /// `None` 0, `Set` 1, `Multiply` 2; any other name is 0).
        int32_t tl_modification = 0;
        /// A `Money Taker`'s `AMOUNT` (`CMoneyTaker +0x58`, 0 from the constructor).
        int32_t money_amount = 0;
        float tl_step = 0.0f;                   ///< +0x90
        float tl_duration = 0.0f;               ///< +0x94
        float tl_time = 0.0f;                   ///< +0x98
        float tl_accum = 0.0f;                  ///< +0x9c
        bool tl_backwards = false;              ///< +0xa0
        bool tl_playing = false;                ///< +0xa1
        bool tl_set_properties_on_load = true;  ///< +0xa3 SET PROPERTIES ON LOAD
        /// +0xa4 DEFAULT INTERPOLATION TYPE
        TimelineInterpolation tl_default_interpolation = TimelineInterpolation::Linear;

        // Quest Controller (`CQuestController`).
        std::string quest;
        bool qc_load_pending = true;        ///< +0xb0
        bool qc_broadcast_on_load = false;  ///< +0xb1
        bool qc_player_was_enabled = true;  ///< +0xb2
        bool qc_player_disabled = false;    ///< +0xb3 PLAYER GETS DISABLED
        int32_t qc_menu_event = 5;          ///< +0xb4
        bool qc_listening = false;
        /// `Cinematic`: the `[CINEMATIC]` (`+0x68`), whether it listens to the
        /// cinematic menu, and the menu event it was told (`+0x78`).
        std::string cinematic;
        bool cine_listening = false;
        int32_t cine_menu_event = 0;

        /// A unit the dialog menu holds (`CQuestDialogMenu +0xc0`) with a boxed
        /// line up: its own enabled flag, kept in `+0x1e9` while `setNPC` has it
        /// off.
        bool menu_held = false;
        bool menu_was_enabled = true;

        // Camera Controller (`CCameraController`).
        float cam_pan = 1.5f;         ///< +0x148 PAN TIME
        float cam_remaining = 0.0f;   ///< +0x14c
        float cam_ease_x = 0.8f;      ///< +0x150
        float cam_ease_y = 0.9f;      ///< +0x154
        float cam_pause = 0.0f;       ///< +0x158 TIME
        float cam_pause_left = 0.0f;  ///< +0x15c
        bool cam_pausing = false;     ///< +0x160
        bool cam_resets = true;       ///< +0x161 RESETS AT END
        bool cam_follow = false;      ///< +0x162 FOLLOW UNIT
        bool cam_stopped = false;     ///< +0x163
        /// +0x1c0 TYPE, an index into `gCAMERA_TYPE_NAMES` {"Position",
        /// "Unit"}; the constructor's 1.
        int cam_type = 1;
        UnitRef cam_unit;  ///< +0x120, anywhere in the level
        Vec3 cam_start;    ///< +0x130
        Vec3 cam_delta;    ///< +0x13c

        // Game State Controller (`CGameStateController`).
        std::string gs_state;
        int32_t gs_type = 1;           ///< +0x64
        bool gs_initialized = false;   ///< +0x61
        bool gs_invulnerable = false;  ///< +0x68
        int32_t gs_tip = -1;           ///< +0x70, the `EContextTip` of `TIP`
        /// +0x6c, the player's HP last frame. DIVERGENCE: the original never initialises it,
        /// so its first frame reads leftover heap; 0 here fires nothing.
        float gs_prev_hp = 0.0f;

        // Skill Controller (`CSkillController`).
        std::string sk_skill;             ///< +0x130 SKILL
        std::string sk_group;             ///< +0x120 GROUP
        std::string sk_unit;              ///< UNIT, whose data group is +0x108
        int32_t sk_level = 0;             ///< +0x150 SKILL LEVEL
        bool sk_learn_on_start = false;   ///< +0x148
        bool sk_unlearn_on_stop = false;  ///< +0x149
        bool sk_starting = false;         ///< +0x14a, a start waiting for `update`
        bool sk_stopping = false;         ///< +0x14b, a stop waiting for `update`
        bool sk_force_stop = false;       ///< +0x14c
        bool sk_target_player = false;    ///< +0x14d
        bool sk_use_unit_target = false;  ///< +0x14e
        /// +0x110, the unit `configureUnit` settled on: the player, or a unit
        /// of the level.
        bool sk_has_unit = false;
        bool sk_player = false;
        UnitRef sk_ref;

        // Skip Cutscene (`CSkipCutscene`).
        bool skip_pending = false;  ///< +0x59
        bool skip_skills = false;   ///< +0x5a

        // Dungeon Object (`CDungeonObject`): `DUNGEON NAME` (+0x100, X360 +0xf0).
        std::string dungeon_name;

        // Animation Controller (`CAnimationPlayer`).
        std::string anim_name;            ///< +0x60 ANIMATION
        std::string anim_group;           ///< +0x80 GROUP
        std::string anim_unit;            ///< +0x88 UNIT
        std::vector<UnitRef> anim_units;  ///< +0x68, from the whole level
        bool anim_player = false;         ///< the list holds the level's player
        bool anim_start_on_load = false;  ///< +0x90
        bool anim_playing = false;        ///< +0x91
        bool anim_loops = false;          ///< +0x92
        bool anim_play_idle = false;      ///< +0x93 PLAY IDLE
        bool anim_timed = false;          ///< +0x94
        float anim_remaining = 0.0f;      ///< +0x98
        float anim_blend = 0.2f;          ///< +0x9c BLEND TIME
        float anim_blend_out = 0.2f;      ///< +0xa0 BLEND OUT TIME
        float anim_force = 0.0f;          ///< +0xa4 FORCE DURATION

        // Pathing (`CPathController`).
        std::vector<Vec3> points;         ///< +0x150, relative to the layout
        std::vector<float> cumulative;    ///< +0x168
        float path_total = 0.0f;          ///< +0x180
        std::string path_group;           ///< GROUP
        std::vector<UnitRef> path_units;  ///< +0x108, from the whole level (the player: none)
        bool path_collected = false;
        bool path_runs = true;           ///< +0x134 RUN
        bool path_to_player = false;     ///< +0x135 WALK TO PLAYER
        bool path_ended = false;         ///< +0x136
        bool path_enable_units = false;  ///< +0x194 ENABLE UNIT ON START

        // Unit Spawner: `CUnitSpawner` itself (shared so an `Object` stays
        // copyable; its address is what the renderer is handed).
        std::shared_ptr<UnitSpawner> spawner;

        // Music, Sound, Warper, Random Choice, Unit Trigger.
        std::string resource;
        bool loops = false;
        // Sound: `VOLUME`, `RADIUS` (5.0 from `CSoundObject`'s constructor
        // when unauthored), `ENVIRONMENTAL`. Property Node: `RADIUS`, `BOX WIDTH`,
        // `BOX LENGTH` (`TYPE` is `resource`).
        float marker_radius = 0.0f;
        float marker_width = 0.0f;
        float marker_length = 0.0f;
        /// A `Jump Down Area` (`CPropertyNode +0x10c` 6): its box, in the
        /// level's coordinates (`updateBoundingBox`).
        ///
        bool jump_down_area = false;
        NoSpawnRegion jump_down_box;
        /// A `Sound` node's `CSoundObject` (`core/sound_object.h`) as `CSoundObject::CSoundObject`
        /// leaves it (visible, enabled, starting on activation,
        /// positional, radius 5), with the node's `START ON ACTIVATE`, `ENVIRONMENTAL`, `VOLUME`
        /// and `RADIUS` (`setRadius`) over it. The `GUID` is `resource`.
        SoundObject sound;
        bool sound_slot_resolved = false;
        std::string dungeon;
        std::string warp_name;
        int32_t level_delta = 1;
        int32_t level_absolute = 0;
        bool waypoint = false;
        std::vector<int32_t> weights;
        int32_t choice_mode = 0;
        int32_t choice_count = 1;

        // Puzzle Input (`CPuzzleRandomizer`).
        bool puzzle_solved = false;                  ///< +0x60
        uint32_t puzzle_active_inputs = 3;           ///< +0x64 (+0x68 is `enabled`)
        uint32_t puzzle_order[6]{0, 1, 2, 3, 4, 5};  ///< +0x6c..+0x80
        bool puzzle_pulled[6]{false};                ///< +0x84..+0x89
        uint32_t puzzle_state = 0;                   ///< +0x8c

        // Damage Shape (`CDamageShape`).
        std::optional<DamageShape> damage_shape;
    };

    Object* find(int64_t id);
    const Object* find(int64_t id) const;

    /// `CEditorBaseObject::BroadcastEvent` and
    /// `CDescriptor::BroadcastEventFromObject`.
    ///
    void broadcast(int64_t from, uint32_t output, QuestState& quests,
                   std::vector<ScriptEvent>& out);

    /// The target descriptor's `InputLogicEvent`.
    void input(int64_t to, uint32_t input, QuestState& quests, std::vector<ScriptEvent>& out);

    // Transcribed classes, one family per node type.

    /// `CTriggerUnit::trigger`: false when
    /// the unit is spent.
    bool trigger_unit(Object& o, bool by_player, std::vector<ScriptEvent>& out);
    /// `CTriggerUnit::setTriggerState(state, notify, force, animate)`;
    /// `fired` marks the event as
    /// `trigger`'s, with what it did.
    void set_trigger_state(Object& o, int32_t state, bool notify, bool force, bool animate,
                           std::vector<ScriptEvent>& out, const ScriptEvent* fired = nullptr);
    void timer_set_enabled(Object& o, bool on, QuestState& quests, std::vector<ScriptEvent>& out);
    void timer_reset(Object& o);
    void timer_update(Object& o, float delta, QuestState& quests, std::vector<ScriptEvent>& out);

    void counter_add(Object& o, int32_t n, QuestState& quests, std::vector<ScriptEvent>& out);
    void counter_subtract(Object& o, int32_t n, QuestState& quests, std::vector<ScriptEvent>& out);
    void counter_update(Object& o, QuestState& quests, std::vector<ScriptEvent>& out);

    void trigger_update(Object& o, const Vec3& player, QuestState& quests,
                        std::vector<ScriptEvent>& out);

    void timeline_play(Object& o, QuestState& quests, std::vector<ScriptEvent>& out);
    void timeline_play_backwards(Object& o, QuestState& quests, std::vector<ScriptEvent>& out);
    void timeline_stop_to(Object& o, bool end, QuestState& quests, std::vector<ScriptEvent>& out);
    void timeline_set_percent(Object& o, float percent, QuestState& quests,
                              std::vector<ScriptEvent>& out);
    void timeline_reset_properties(Object& o, float percent);
    void timeline_update(Object& o, float delta, QuestState& quests, std::vector<ScriptEvent>& out);
    void track_update(Object& o, Track& track, float percent, bool backwards, QuestState& quests,
                      std::vector<ScriptEvent>& out);

    void quest_interact(Object& o, QuestState& quests, std::vector<ScriptEvent>& out);
    /// A `Monster`'s `Interact` (`CMonsterDescriptor::InputLogicEvent`
    /// case 0x30): the unit's own line, with no
    /// quest named.
    void monster_interact(Object& o, QuestState& quests, std::vector<ScriptEvent>& out);
    /// `CGameUI::forceInteractiveDialog(player, unit, quest)`
    /// for a line opened by `opener` on `unit`. False when no box is left open.
    bool force_interactive_dialog(Object& opener, const UnitRef& unit, QuestState& quests,
                                  std::vector<ScriptEvent>& out);
    /// The menu's broadcasts on its unit (`+0xc0`) as it closes
    /// (`CQuestDialogMenu::onClick`), by menu event;
    /// `OK` on a `[COMPLETE]` line completes `quest` between them.
    void unit_menu_closed(const UnitRef& unit, int32_t menu_event, bool complete_line,
                          const std::string& quest, QuestState& quests,
                          std::vector<ScriptEvent>& out);
    /// The menu's event reaching the controller that opened it
    /// (`CQuestController::menuEventOccured`).
    void controller_menu_event(Object& o, int32_t menu_event, std::vector<ScriptEvent>& out);
    void cinematic_play(Object& o, std::vector<ScriptEvent>& out);
    void cinematic_level_update(Object& o, QuestState& quests, std::vector<ScriptEvent>& out);
    /// `setEnabled(+0x1e9)` on the menu's unit as the menu lets go of it.
    void release_menu_unit(const UnitRef& unit);
    void quest_level_update(Object& o, QuestState& quests, std::vector<ScriptEvent>& out);

    void camera_start(Object& o, QuestState& quests, std::vector<ScriptEvent>& out);
    void camera_stop(Object& o, QuestState& quests, std::vector<ScriptEvent>& out);
    void camera_update(Object& o, float delta, QuestState& quests, std::vector<ScriptEvent>& out);

    void game_state_perform(Object& o, QuestState& quests, std::vector<ScriptEvent>& out);
    void game_state_do(const Object& o, bool on, std::vector<ScriptEvent>& out);
    void warp_pets_to_player();

    void skip_update(Object& o, QuestState& quests, std::vector<ScriptEvent>& out);
    void close_all_dialogs(std::vector<ScriptEvent>& out);

    void puzzle_set_active_inputs(Object& o, uint32_t count);
    void puzzle_trigger(Object& o, uint32_t input_index);
    void puzzle_reset(Object& o);
    bool puzzle_level_update(Object& o, QuestState& quests, std::vector<ScriptEvent>& out);
    void puzzle_add_level_listener(Object& o);
    void add_level_update(int64_t id);

    /// A character as the level's character-update listeners see it.
    struct UpdatedCharacter {
        DamageShapeUnit unit;
        LevelScript* machine = nullptr;  ///< who runs it; null for the player
        Vec3 at;                         ///< `getPosition(true)`
        float radius = 0.0f;             ///< +0x194
        bool hidden = false;             ///< +0x70d
        bool targetable = true;          ///< +0x531
        bool alive = true;
        int32_t alignment = 0;   ///< `CCharacter::alignment()`
        uint32_t unit_type = 0;  ///< +0x1ac
        bool known_type = false;
    };
    UpdatedCharacter updated_character(int64_t id);
    UpdatedCharacter updated_player() const;
    bool unit_isa(const UpdatedCharacter& who, uint32_t want) const;

    void add_character_update_listener(int64_t id);
    void remove_character_update_listener(int64_t id);
    void flush_character_update_listeners();
    void character_updated_in_level(const UpdatedCharacter& who, float delta, QuestState& quests,
                                    std::vector<ScriptEvent>& out);

    void damage_shape_set_enabled(Object& o, bool on, QuestState& quests,
                                  std::vector<ScriptEvent>& out);
    void damage_shape_update(Object& o, float delta, QuestState& quests,
                             std::vector<ScriptEvent>& out);
    void damage_shape_instant(Object& o, QuestState& quests, std::vector<ScriptEvent>& out);
    void damage_shape_item_updated(int64_t id, const DamageShapeItem& item);
    const LevelItems* level_items_ = nullptr;
    void damage_shape_character_updated(int64_t id, const UpdatedCharacter& who, float delta,
                                        QuestState& quests, std::vector<ScriptEvent>& out);
    void damage_shape_land(int64_t id, const std::string& name, const UpdatedCharacter& who,
                           float amount, bool dot, QuestState& quests,
                           std::vector<ScriptEvent>& out);

    void animation_collect(Object& o);
    void animation_play(Object& o, bool loops, QuestState& quests, std::vector<ScriptEvent>& out);
    void animation_apply(Object& o, std::vector<ScriptEvent>& out);
    void animation_stop(Object& o, bool idle, QuestState& quests, std::vector<ScriptEvent>& out);
    void animation_update(Object& o, float delta, QuestState& quests,
                          std::vector<ScriptEvent>& out);

    void path_set_enabled(Object& o, bool on, QuestState& quests, std::vector<ScriptEvent>& out);
    /// `unit_radius` is the walker's (`+0x194`), which a walk-to-player path
    /// keeps clear of the player.
    Vec3 path_point_at(const Object& path, float pct, bool forward, float unit_radius) const;
    float path_closest_pct(const Object& path) const;
    void set_path_to_follow_by_name(Object& unit);
    /// `CMonster::setPathToFollow`, the same
    /// as `CCharacter`'s: `machine` null
    /// clears the path.
    void set_path_to_follow(Object& unit, int64_t path, LevelScript* machine);
    void path_follow(Object& unit, bool force, const Vec3& at, QuestState& quests,
                     std::vector<ScriptEvent>& out);

    /// `CCharacter::setAIState`, with its
    /// end-of-path side effect.
    void set_ai_state(Object& unit, int32_t state, QuestState& quests,
                      std::vector<ScriptEvent>& out);
    /// `CCharacter::setAIPlayAnimation`.
    void set_ai_play_animation(Object& unit, const std::string& animation, bool loops, float blend,
                               std::vector<ScriptEvent>& out);

    void set_player_enabled(bool on, std::vector<ScriptEvent>& out);

    /// `CSkillController::configureUnit`.
    bool skill_configure(Object& o);
    /// `CSkillController::learnSkill` /
    /// `CSkillController::unlearnSkill`.
    void skill_learn(Object& o, QuestState& quests, std::vector<ScriptEvent>& out);
    void skill_unlearn(Object& o, QuestState& quests, std::vector<ScriptEvent>& out);
    /// `CSkillController::update`.
    void skill_update(Object& o, QuestState& quests, std::vector<ScriptEvent>& out);
    PlayerSkills player_skills_;
    UnitSkills unit_skills_;

    /// Every living unit of the level whose `MONSTER` is `unit` (`CLevel::getBaseUnitsByDataGroup`):
    /// machines in list order, units in creation order.
    std::vector<UnitRef> units_named(const std::string& unit) const;
    /// The one unit of the level that is a `unit`, a `Monster` before a
    /// `Unit Trigger` (`CLevel::getBaseUnitByDataGroup`).
    ///
    bool base_unit_named(const std::string& unit, UnitRef& found) const;
    /// Where events about `machine`'s nodes go while this machine runs: `out`
    /// for this one, the other machine's outbox otherwise.
    std::vector<ScriptEvent>& sink_for(LevelScript* machine, std::vector<ScriptEvent>& out);
    /// The `Pathing` node a unit follows, in whichever machine runs it.
    Object* path_of(const Object& unit);
    /// `End of Path Reached` on a path node of any machine (`broadcast` there).
    void path_reached(LevelScript* machine, Object& path, QuestState& quests,
                      std::vector<ScriptEvent>& out);

    /// Hand an input to the node's spawner, through the renderer.
    void spawner_input(Object& o, uint32_t in, std::vector<ScriptEvent>& out);

    void note_unhandled(const Object& target, const std::string& what);

    // Dialog presentation (the port's own; the menu is `CQuestDialogMenu`).
    void open_dialog(Object& o, const DialogLine& line, const std::string& quest,
                     QuestSection section, const UnitRef& unit, std::vector<ScriptEvent>& out);
    void hold_menu_unit(const UnitRef& unit);
    void show_floaty(const Object& o, const DialogLine& line, std::vector<ScriptEvent>& out);
    void clear_floaty(std::vector<ScriptEvent>& out);
    void show_front_dialog(std::vector<ScriptEvent>& out);
    void close_dialog(int32_t menu_event, QuestState& quests, std::vector<ScriptEvent>& out);
    void update_dialog_ui(float delta, QuestState& quests, std::vector<ScriptEvent>& out);

    // Characters (`CCharacter`/`CMonster`): the frame, the AI states, movement and the swing
    // (`script_units.cpp`).

    UnitRef self(int64_t node) { return {this, node}; }
    /// A node by id: the player for 0, this machine's character, else the first
    /// machine of the level's list that runs one.
    UnitRef resolve(int64_t node);
    /// The level's list (`set_level_machines`), or this machine alone.
    MachineWalk level_machines() const;
    /// Stands in for the list when there is none; refreshed on every call, as a
    /// machine is moved into place after it is made.
    mutable std::vector<LevelScript*> alone_;

    struct Character {
        /// `+0x1b8`, the unit's `CEffectManager`: made the first time an
        /// effect reaches it (`ensure_unit_effects`). Shared so a copied
        /// `Character` keeps the one manager.
        std::shared_ptr<EffectManager> effects;
        /// `+0x490`, the inventory `getEffectValue`
        /// adds; the scene's
        /// (`set_unit_inventory`), null for a unit with none.
        const Inventory* inventory = nullptr;
        /// The character's own inventory, when it was made with one
        /// (`UnitCreation::inventory`); `inventory` points at it.
        std::shared_ptr<Inventory> own_inventory;
        /// What `own_inventory` asks requirements of (`UnitCreation::sheet`).
        std::shared_ptr<CharacterProgression> sheet;
        /// Its own `[EFFECT]`s, rolled as it was made (`UnitCreation::own_effects`).
        std::vector<Effect> own_effects;
        int64_t node = 0;
        std::string unit;
        /// `+0x640` is the player (`CCharacter::setMaster`
        /// from `CCharacter::addPet`):
        /// the character follows them.
        bool master_player = false;
        /// In its master's pet list (`+0x648`): `addPet` puts it there,
        /// `removePet` takes it out - and
        /// leaves `+0x640`, so a pet removed that way still follows its master.
        bool listed_pet = false;
        /// Its bit in the master's `+0x538` (X360 `+0x514`), one per pet: whether the pet was on
        /// the avoidance map when the master's `removeFromAvoidanceMap` took it off.
        bool grid_with_master = false;
        /// `+0x448`: what `awardExperience`
        /// has added up.
        int32_t experience = 0;
        /// `+0x338`, `CCharacter::setAlignment`: the master's
        /// side via `setMaster`, the caster's via a summon
        /// (`CSkillEvent::unitSpawnedFromUnitSpawner`). Unset: the
        /// unit's `UnitStats` alignment.
        std::optional<UnitStats::Alignment> alignment;
        MotionState motion;        ///< +0x84 (the sphere's centre), velocity, ground
        float facing = 0.0f;       ///< the yaw of +0xb4
        float home_facing = 0.0f;  ///< the yaw it was placed with
        bool face_home = false;    ///< turning back to it after a conversation
        float scale = 1.0f;        ///< +0x98, the unit's scale
        /// The items in equip locations 0 and 1, and each one's attack.
        std::string right_hand;
        std::string left_hand;
        UnitStats::WeaponAttack weapon_attack;
        UnitStats::WeaponAttack left_weapon_attack;
        /// `+0x2b2` (Linux `+0x37e`), 0 from the constructor: flipped by every
        /// `performAttack`, read by `selectAttack(0)`.
        bool alternate_hand = false;
        /// `+0x3ec` is the left hand's weapon: `selectAttack` took it.
        bool attack_left = false;
        int32_t think_index = 0;  ///< +0x520
        float think = 100.0f;     ///< +0x524, 100 from the constructor
        /// `+0x199` and `+0x19a`: active (thinks and moves) and near (updated at all), by
        /// `calculateActiveRange`; both set as the level adds it
        /// (`CBaseUnit::activateUnitInLevel`).
        bool active = true;
        bool in_near_range = true;
        /// `+0x528`, the fade: 1 as the level adds it (`CLevel::addCharacter`
        /// -> `CCharacter::setActiveInLevel(true)`),
        /// stepped by `updateOpacity`.
        float opacity = 1.0f;
        /// `+0x52c`: the fade's goal, `setVisible(bool)`.
        bool want_visible = true;
        /// `+0x70d`: the mesh hidden on purpose (`setMeshVisible`,
        /// mesh keys, `FADE OUT`/`SET VISIBLE`); `setVisible(true)` does not show it.
        bool mesh_hidden = false;
        /// On the level's near list (`CLevel +0x88`), which `updateCharacters`
        /// walks: rebuilt once a second from `in_near_range`, joined as the
        /// level adds the character.
        bool listed = true;
        /// `+0x640` when the master is a character the level runs (`setMaster`);
        /// the player is `master_player`.
        bool has_master = false;
        UnitRef master;
        /// `+0x530`, the shadow flag `calculateActiveRange` keeps.
        bool casts_shadow = false;
        /// `+0x190`: to be deleted before its next update (`updateCharacters`).
        ///
        bool pending_delete = false;
        /// The death flight (`die`): while it flies, the path, speed,
        /// whether it falls and the distance flown. `flown` once one was laid (`+0x681` outlives
        /// the flight).
        bool flying = false;
        bool flown = false;
        DeathFlight flight;
        float flight_distance = 0.0f;
        /// `+0x682`: a `Jump Down Area` let it jump (`setAllowJumpDown`)
        /// - set by the area after the unit's update, cleared at
        /// the end of the next.
        bool allow_jump_down = false;
        /// The jump down (`attemptJumpDown`):
        /// `+0x680` while it flies, the path and speed (`+0x670`, `+0x67c`),
        /// the distance flown (`+0x678`).
        bool jumping = false;
        JumpDown jump;
        float jump_distance = 0.0f;
        /// The level clock at which the queued `JUMP_LAND` has run and `IDLE`
        /// plays (`jumpDownAI`'s `animationPlayingSubstring("IDLE")`).
        float jump_idle_at = 0.0f;
        bool has_target = false;  ///< +0x340 set
        UnitRef target;           ///< +0x340
        bool hunt_affix = false;  ///< `MONSTER_HUNT` added
        /// `+0x708`: 1.0 when an own proc casts, run down by `update`;
        /// `applyDamage`'s struck procs wait
        /// for it.
        float proc_lockout = 0.0f;
        /// `reapplyEffects` has run for it (`load_own_effects`).
        bool own_effects_loaded = false;
        /// `+0x688`: a transformation's time left (`transformCharacter`), -999
        /// for one that lasts (`TRANSFORM PERMANENT`), 0 for none.
        float transform_left = 0.0f;
        /// `+0x690`: the unit it was before its first transformation (the
        /// original keeps its GUID; -1 when not transformed). Empty for none.
        std::string transformed_from;
        /// The `MIMICIDLE` theme, which `CCharacter::unitInit`
        /// gives every `ISA 0xa7` (`MIMIC`): asleep as a chest (unhurt, harmless, unfound, seeing
        /// only its motion radius) until its AI leaves idle or something strikes it.
        bool mimic_idle = false;
        /// `+0x710`, the pet stance (`set_unit_stance`): 1 from the constructor.
        int32_t stance = 1;
        /// `+0x664`, `CANMATCHSPEED` (`unitInit`).
        bool can_match_speed = false;
        /// `+0x684`, the town trip's seconds left (`sendToTown`).
        ///
        float town_timer = 0.0f;
        /// `+0x1ec` / `+0x1f0`: the damage not yet shown over the character.
        DamageDisplay damage_shown;
        bool pathing = false;  ///< +0x264
        Vec3 destination;      ///< +0x228 (X360 +0x1c0, the final target position)
        /// X360 +0x1cc, the final target direction: the ground direction from the
        /// unit to `destination`, normalised, as `setFinalTargetPosition`
        /// leaves it.
        Vec3 destination_direction{0.0f, 0.0f, 1.0f};
        Vec3 next;                   ///< +0x21c
        Route route;                 ///< +0x270
        float since_route = 100.0f;  ///< +0x278
        /// The pathfinder's node-expansion cap (`CAstarPathfinder +0x6c`, 150) and the copy
        /// `CCharacter::createPathfinder` keeps:
        /// `setPathToFollow` raises it to 800 while a path is held.
        int32_t search_cap = 150;
        int32_t saved_search_cap = 150;
        /// A held `FLEE EFFECT` (0x4f), its seconds left, as `applyEffectOnUnit`
        /// gives it and `CMonster::fleeAI` asks
        /// about. The port's monsters hold no effect manager, so only this is kept.
        float flee_effect_left = 0.0f;
        float since_replan = 10.0f;  ///< +0x27c, 10 from the constructor
        float fidget_timer = 0.0f;   ///< +0x280
        float fidget_wait = 0.0f;    ///< +0x284
        /// The fidget animation's time left: the port's clock for
        /// `CMonster::fidgetAI`'s `animationPlayingSubstring("FIDGET")`.
        float fidget_left = 0.0f;
        bool run = false;         ///< +0x321
        bool on_grid = false;     ///< +0x19b
        float swing_left = 0.0f;  ///< +0x378
        /// The `HIT` reaction `interrupt` blended, its time left at rate 1:
        /// the port's clock for `animationPlayingSubstring("HIT")`, which
        /// `interruptAI` (state 0x1d) waits on.
        float hit_left = 0.0f;
        bool struck = false;    ///< +0x37c
        std::string swing;      ///< the attack animation playing
        float cooldown = 0.0f;  ///< CMonster +0x7e8
        /// The animation `selectAttack` chose for the attack
        /// in use (its +0x70): the index in the model's list, -1 none, with its
        /// name and length.
        int attack_animation = -1;
        /// The current attack (+0x2c0) set: `selectAttack` has run on a unit
        /// with a model (every character has its one unarmed attack).
        bool has_attack = false;
        std::string attack_animation_name;
        float attack_animation_length = 0.0f;
        UnitCombat combat;
        float min_hp = 9999999.0f;  ///< +0x420 minimum HP watermark
        std::string gait;           ///< the locomotion animation last sent
        float gait_rate = 1.0f;     ///< and its speed (+0x260)
        Vec3 sent_at;
        float sent_facing = 0.0f;
        bool sent = false;
        Vec3 traced_at;
        bool traced = false;
        AIFlags ai_flags;          ///< +0x718 (CAIManager's flag manager and formation list)
        AISkillManager ai_skills;  ///< +0x718 (CAIManager +0x20, the skill manager)
        std::vector<AIStatWatcher> ai_watchers;  ///< +0x718 (CAIManager +0x28, stat watchers)
        UnitCasting casting;                     ///< +0x1c8 and +0x398
    };
    Character* character(int64_t id);
    const Character* character(int64_t id) const;
    /// `carried`: a character being re-made as another unit
    /// (`convert_character`), whose effects, side, place in its master's list
    /// and the like the new one keeps; null for a new character.
    Character& make_character(const Object& o, const Vec3& at, float facing,
                              const UnitCreation* creation = nullptr,
                              const Character* carried = nullptr);
    const UnitStats& stats_for(const std::string& unit) const;
    Vec3 position_of(const Object& o) const;
    std::string target_unit(const UnitRef& target) const;

    void update_characters(float delta, QuestState& quests, std::vector<ScriptEvent>& out);
    void character_update(Object& o, Character& c, float delta, QuestState& quests,
                          std::vector<ScriptEvent>& out);
    /// `force` is `updateAI`'s second argument: the think timer is not waited
    /// for (the skill states re-enter the AI at once with it set).
    void monster_update_ai(Object& o, Character& c, float delta, QuestState& quests,
                           std::vector<ScriptEvent>& out, bool force = false);
    void idle_ai(Object& o, Character& c, float delta, QuestState& quests,
                 std::vector<ScriptEvent>& out);
    bool idle_ai_normal(Object& o, Character& c, float delta, QuestState& quests,
                        std::vector<ScriptEvent>& out);
    bool idle_ai_ranged_defender(Object& o, Character& c, QuestState& quests,
                                 std::vector<ScriptEvent>& out);
    bool idle_ai_ranged_caster(Object& o, Character& c, float delta, QuestState& quests,
                               std::vector<ScriptEvent>& out);
    bool idle_ai_circler(Object& o, Character& c, float delta, QuestState& quests,
                         std::vector<ScriptEvent>& out);
    /// What the three idle handlers of a ranged `AITYPE` open with: the gates,
    /// the unit in sight, and a pet's 35 from its master.
    bool idle_ai_sight(Object& o, Character& c, bool no_follow_exempt, bool& gated, UnitRef& found);
    /// The blow an idle handler opens with when its weapon is ready and the
    /// unit found is already in reach: the hunt, and `ROAR_SOUND`.
    bool idle_ai_open_attack(Object& o, Character& c, const UnitRef& found, QuestState& quests,
                             std::vector<ScriptEvent>& out);
    /// `canCastCurrentSkill(unit, false, false)` asked of a unit that is not
    /// (yet) the target.
    bool can_cast_current_skill_on(Object& o, Character& c, const UnitRef& unit,
                                   std::vector<ScriptEvent>& out);
    /// `displayDamageAbsorbed(delta)`: the number, when it is due.
    void display_unit_damage(const Object& o, Character& c, float delta,
                             std::vector<ScriptEvent>& out);
    /// `CSoundBank::playSample(slot, node)` on the character's bank.
    void play_unit_sample(const Object& o, int slot, std::vector<ScriptEvent>& out) const;
    /// The samples a unit's sound bank holds at `slot`, in `loadModel`'s order.
    /// One `CSoundInstance` of a bank slot: a definition and which of its recordings.
    struct BankInstance {
        const SoundData* data = nullptr;
        size_t file = 0;
    };
    std::vector<BankInstance> unit_bank_samples(const Object& o, int slot) const;
    /// `CMonster::idleAI`'s tail for a character whose master is the player.
    void idle_ai_pet(Object& o, Character& c, QuestState& quests, std::vector<ScriptEvent>& out);
    /// `idleAI`'s station point by the master.
    void pet_wander_point(Character& c, const UnitStats& stats);
    /// `CCharacter::teleportToMaster(distance)`.
    void teleport_to_master(Object& o, Character& c, float distance);
    /// `CCharacter::isPetNearDeath()`.
    bool pet_near_death(const Object& o, const Character& c) const;
    /// `CCharacter::returnToTownAI`, AI state
    /// 0x29.
    void return_to_town_ai(Object& o, Character& c, QuestState& quests,
                           std::vector<ScriptEvent>& out);
    /// `CCharacter::updateInTownTimer`, while
    /// in AI state 0x2a.
    void update_in_town_timer(Object& o, Character& c, float delta, QuestState& quests,
                              std::vector<ScriptEvent>& out);
    /// `+0x25c`: what `updatePathing` multiplies the speed,
    /// as `updateAnimation` works it out.
    float speed_multiplier(const Character& c, const UnitStats& stats) const;
    /// `CLevel::randomOpenPositionRange(at, lo, hi, false)`.
    Vec3 open_position_near(const Vec3& at, float lo, float hi);
    /// `CCharacter::dropToGround(4.0, level)`.
    void drop_unit_to_ground(Character& c);
    void hunt_ai(Object& o, Character& c, QuestState& quests, std::vector<ScriptEvent>& out);
    bool attempt_jump_down(Object& o, Character& c, QuestState& quests,
                           std::vector<ScriptEvent>& out);
    void jump_down_ai(Object& o, Character& c, float delta, QuestState& quests,
                      std::vector<ScriptEvent>& out);
    void set_allow_jump_down(Object& o, Character& c, bool allow, QuestState& quests,
                             std::vector<ScriptEvent>& out);
    void property_node_set_enabled(Object& o, bool on, QuestState& quests,
                                   std::vector<ScriptEvent>& out);
    void affector_set_enabled(Object& o, bool on, std::vector<ScriptEvent>& out);
    void set_orientation_column(Object& o, TimelinePropertyKind column, const Vec3& local,
                                std::vector<ScriptEvent>& out);
    void jump_down_area_character_updated(const Object& area, const UpdatedCharacter& who,
                                          QuestState& quests, std::vector<ScriptEvent>& out);
    void attack_ai(Object& o, Character& c, float delta, QuestState& quests,
                   std::vector<ScriptEvent>& out);
    void flee_ai(Object& o, Character& c, QuestState& quests, std::vector<ScriptEvent>& out);

    // Skills: `CMonster::selectOffensiveSkill` and its neighbours, and the
    // three skill states of `CCharacter`.
    void load_unit_skills(Character& c);
    void load_own_effects(Character& c);
    void activate_unit_affixes(Character& c);
    /// `CCharacter::convertCharacter(unit, transform)`
    /// and the like half of
    /// `CCharacter::revertTransformation`.
    bool convert_character(int64_t id, const std::string& unit, QuestState& quests,
                           std::vector<ScriptEvent>& out);
    UnitSkill* active_unit_skill(Character& c);
    bool unit_skill_affordable(const Character& c, const UnitSkill& skill) const;
    void set_active_skill(Character& c, int index);
    void select_offensive_skill(Object& o, Character& c, bool check, QuestState& quests,
                                std::vector<ScriptEvent>& out);
    void select_defensive_skill(Character& c);
    bool has_offensive_skill(const Character& c) const;
    bool has_defensive_skill(const Character& c) const;
    bool can_cast_current_skill(Object& o, Character& c, bool skip_range, bool skip_chance,
                                std::vector<ScriptEvent>& out);
    int in_skill_range(Character& c);
    /// `CSkillManager::getSkillByGuid(long long)`:
    /// the first of the unit's skills whose
    /// `UNIQUE_GUID` is `guid`, or -1.
    int unit_skill_by_guid(const Character& c, int64_t guid) const;
    /// `castSkill(-1)` and `inSkillRange(-1, ..)`: the active skill (`+0x398`)
    /// becomes the first skill of its own GUID.
    void reselect_active_by_guid(Character& c);
    bool cast_skill(Object& o, Character& c, QuestState& quests, std::vector<ScriptEvent>& out);
    /// `CCharacter::availableForCommand`.
    bool available_for_command(const UnitRef& who) const;
    /// `turnTowardPosition` toward the target at `at` (its feet; the turn is to
    /// its centre).
    void turn_toward_target(Object& o, Character& c, const UnitStats& stats, const Vec3& at,
                            float delta);
    void skill_ai(Object& o, Character& c, float delta, QuestState& quests,
                  std::vector<ScriptEvent>& out);
    void performing_skill_ai(Object& o, Character& c, float delta, QuestState& quests,
                             std::vector<ScriptEvent>& out);
    void skill_approach_ai(Object& o, Character& c, float delta, QuestState& quests,
                           std::vector<ScriptEvent>& out);
    void react_to_damage(Object& o, Character& c, const UnitRef& attacker, bool direct,
                         QuestState& quests, std::vector<ScriptEvent>& out);

    // Targets: `findCharacterWithinView`, and the questions asked of a target.
    bool find_character_within_view(const Object& o, const Character& c, bool nearest,
                                    UnitRef& found, int32_t alignment = 7);
    /// Where a search looks from: a point and a yaw about +Y (the frame
    /// `getOrientationMatrix`, vtable +0x6c, gives with the position put in).
    // PORT: the port keeps a unit's turn as a yaw.
    struct ViewFrame {
        Vec3 at;
        float facing = 0.0f;
        /// Turned along `forward` (`FromAxes(UNIT_Y x forward, UNIT_Y,
        /// forward)`) rather than by `facing` about +Y: a skill's search.
        bool along = false;
        Vec3 forward{0.0f, 0.0f, 1.0f};
    };
    ViewFrame skill_frame(const Character& c) const;
    /// `CLevel::findCharacterWithinView` with the cone given.
    bool find_within_view(const ViewFrame& frame, const Object& o, int32_t alignment, float motion,
                          float view, float sight, bool nearest, const UnitSkill* skill,
                          UnitRef& found);
    /// The test `findCharacterWithinView` puts each candidate's side through.
    ///
    static bool of_alignment(int32_t wanted, int32_t side, bool targetable);
    /// The near list (`CLevel +0x54`) in the order the level keeps it.
    std::vector<UnitRef> near_list_order();
    /// `CCharacter::getEffectValue(type, 7)` on a level unit.
    float character_effect(const Character& c, int type) const;
    /// `CCharacter::sightRange`.
    float sight_range(const Object& o, const Character& c) const;
    /// `CCharacter::getTargetAlignment`: 3
    /// under `BERSERK`, else 5 for a `GOOD` character and 6 for any other.
    int32_t target_alignment_of(const Object& o, const Character& c) const;
    /// `CSkill::getTargetIsValid(who, owner, at, false)` for
    /// skill `s` of `o` (owner: `getMasterOwnerCharacter`).
    bool unit_skill_target_valid(const Object& o, const Character& c, const UnitSkill& s,
                                 const UnitRef& who) const;
    /// `CLevel::findCharacterWithinView(frame, alignment, 0, angle, range, true, o, skill)`
    /// as a skill asks it: the nearest target of `s`.
    /// A candidate for sight or a stray blow: visible, enabled, targetable, alive and near.
    const Character* candidate(const Object& other) const;
    const Object* target_object(const UnitRef& target) const;
    const Character* target_character(const UnitRef& target) const;
    bool target_at(const UnitRef& target, Vec3& at) const;
    bool target_centre_at(const UnitRef& target, Vec3& at) const;
    void set_final_target_position(Character& c, float x, float z);
    float turn_factor(const Character& c) const;
    float target_radius(const UnitRef& target) const;
    bool target_alive(const UnitRef& target) const;
    bool target_inactive(const UnitRef& target) const;
    int32_t target_state(const UnitRef& target) const;
    bool is_enemy(const Object& o, const UnitRef& target) const;
    /// `CCharacter::alignment()` without the
    /// effect-0x50 swap (no effects on a placed unit): `+0x338`.
    UnitStats::Alignment alignment_of(const Object& o) const;
    int32_t console_alignment(const Object& o) const;
    int32_t target_console_alignment(const UnitRef& target) const;
    /// `CCharacter::walkingSpeed` /
    /// `runningSpeed`: `PERCENT SPEED`
    /// (0x15), a slow damped by 0x8c, never below 0.
    float character_speed(const Character& c, float base) const;
    /// What a character's master (`+0x640`) shows its pets, whether it is the
    /// player or a character of the level.
    struct MasterView {
        Vec3 at;  ///< the master's feet
        /// `getPosition(true)`: the master's centre (+0x70), where a view from
        /// the master's frame starts.
        Vec3 centre;
        Vec3 forward;  ///< vtable +0x130
        bool has_target = false;
        UnitRef target;              ///< `+0x340`
        bool running = false;        ///< `+0x321`
        float walking_speed = 0.0f;  ///< `+0x28c`
        float running_speed = 0.0f;  ///< `+0x290`
        bool moving = false;         ///< `+0x264`
    };
    /// `+0x640` is set: the player (`master_player`) or a character the level
    /// still runs (`has_master`).
    bool has_master(const Character& c) const;
    /// The master's view; false with no master.
    bool master_view(const Character& c, MasterView& out) const;
    bool is_friend(const Object& o, const UnitRef& target) const;
    void set_target(Object& o, Character& c, bool has, const UnitRef& target,
                    std::vector<ScriptEvent>& out);
    bool sight_unobstructed(const Vec3& from, const Vec3& to) const;
    /// CLevel::getActiveCharactersAtPosition.
    void get_active_characters_at_position(const Vec3& pos, uint32_t unittype, uint32_t alignment,
                                           float radius, bool include_living, bool include_dead,
                                           std::vector<int64_t>& out) const;

    // Movement.
    bool set_destination(Character& c, const UnitStats& stats, float x, float z);

public:
    /// `CCharacter::applyEffectOnUnit`, `FLEE EFFECT`
    /// (0x4f): `chance` is the effect's value, `duration` how long it is held, `source` where
    /// it came from. True when the character turned to flee now (the caller plays
    /// `FLEE_SOUND`, sample 0xe).
    bool apply_flee_effect(int64_t node, float chance, float duration, const Vec3* source,
                           QuestState& quests, std::vector<ScriptEvent>& out);
    /// A living character this machine runs, by node, and its unit name
    /// (empty when there is none).
    std::string living_unit(int64_t node) const;
    /// `CCharacter::isEnemy` from this
    /// machine's unit `node` (dead or alive) to `other`'s unit `other_node`; a
    /// null `other` is the player. False when either is missing.
    bool unit_is_enemy_of(int64_t node, LevelScript* other, int64_t other_node) const;
    /// Every `Damage Shape` node of this machine, in creation order.
    std::vector<int64_t> damage_shape_ids() const;
    /// The node-expansion cap of character `node`'s pathfinder (`+0x6c`), or
    /// -1 when this machine runs no such character.
    int32_t unit_search_cap(int64_t node) const;
    /// `CSkillEvent::disableAllDamageShapes`:
    /// `CDamageShape::setEnabled(false)` on
    /// every damage shape this machine holds.
    void disable_damage_shapes(QuestState& quests, std::vector<ScriptEvent>& out);

private:
    void stop_pathing(Character& c);
    void interrupt(Object& o, Character& c, bool hit, QuestState& quests,
                   std::vector<ScriptEvent>& out);
    /// `CGenericModel::animationExists(const std::string&)`:
    /// an animation of exactly that name, case
    /// aside.
    bool unit_animation_exists(const std::string& unit, const std::string& name) const;
    /// `CGenericModel::animationExistsSubstring`: a name
    /// that begins with `prefix`, case aside on the model's side.
    bool unit_animation_prefix_exists(const std::string& unit, const std::string& prefix) const;
    /// `CMonster::fidgetAI`.
    void fidget_ai(Object& o, Character& c, QuestState& quests, std::vector<ScriptEvent>& out);
    void blend_reset(Character& c);
    void update_pathing(Character& c, const UnitStats& stats, float delta);
    void follow_character(Object& o, Character& c, const UnitRef& target,
                          std::vector<ScriptEvent>& out);
    void avoidance_add(Character& c);
    void avoidance_remove(Character& c);
    void avoidance_remove_self(Character& c);
    Vec3 feet_of(const Character& c) const;

    // The swing.
    int find_random_animation(const std::string& unit, const std::string& prefix, std::string& name,
                              float& length) const;
    const AttackDescription& current_attack(const Character& c, const UnitStats& stats) const;
    float weapon_arc(const Character& c) const;
    /// `+0x3ec`: the weapon whose attack is in use, null for the character's
    /// own.
    const UnitStats::WeaponAttack* weapon_in_use(const Character& c) const;
    /// The two hands as `attackRange`, `selectAttack` and `attack` read them.
    HeldHands held_hands(const Character& c) const;
    /// `CCharacter::selectAttack(mode)`:
    /// which hand's attack is in use. Nothing while a swing runs.
    void select_attack(Character& c, int mode);
    /// `CCharacter::attack`: the
    /// hand a swing is taken with.
    void choose_attack_hand(const Object& o, Character& c, const UnitStats& stats);
    /// `CLevel::findCharacterWithinView(frame, getTargetAlignment(), near, angle, range, true,...)
    /// `: the nearest foe of `o` beyond `near`, within
    /// `angle` degrees and `range`, in clear sight.
    bool find_foe_in_view(const Object& o, const Character& c, float near, float angle, float range,
                          UnitRef& found);
    bool attack(Object& o, Character& c, QuestState& quests, std::vector<ScriptEvent>& out);
    void perform_attack(Object& o, Character& c, QuestState& quests, std::vector<ScriptEvent>& out);
    bool in_attack_range(Character& c, const UnitStats& stats);
    /// `CMonster::canAttackWithCurrentWeapon()`: false while
    /// the monster's own cooldown is above 0, else `CCharacter::canAttackWithCurrentWeapon()`.
    ///
    bool can_attack_with_current_weapon(const Character& c) const;
    bool valid_line_of_sight(const Character& c) const;
    /// `die` on a character with no attacker, after `modifyHP(-HP)`'s
    /// threshold broadcast (a layout-placed unit's only, as in `damage_unit`).
    void die_now(Object& o, Character& c, QuestState& quests, std::vector<ScriptEvent>& out);
    /// `CMonsterDescriptor::InputLogicEvent`
    /// case 0x53, `Kill Pets`.
    void kill_pets(Object& o, QuestState& quests, std::vector<ScriptEvent>& out);
    void kill_character(Object& o, Character& c, QuestState& quests, std::vector<ScriptEvent>& out,
                        const KillingBlow* blow = nullptr, bool burst = false,
                        bool by_player = false);

    void emit_character(const Object& o, Character& c, std::vector<ScriptEvent>& out);
    void update_near_unit_list(const Vec3& view, std::vector<ScriptEvent>& out);
    /// `CCharacter::calculateActiveRange`
    /// against `view`: `active`, `in_near_range`, the think band, and
    /// `inactive_`.
    void calculate_active_range(const Object& o, Character& c, const Vec3& view,
                                std::vector<ScriptEvent>& out);
    /// `CCharacter::setVisible(bool, bool)`.
    void set_character_visible(Object& o, Character& c, bool on, bool immediate,
                               std::vector<ScriptEvent>& out);
    /// `CCharacter::setVisible(bool)`: the fade's goal.
    static void want_visible(Character& c, bool on) { c.want_visible = on && !c.mesh_hidden; }
    /// `CCharacter::updateOpacity`.
    void step_opacity(Object& o, Character& c, float delta, bool force,
                      std::vector<ScriptEvent>& out);
    /// The camera test (`view_test_`).
    bool in_view(const Object& o) const { return !view_test_ || view_test_(*this, o.id); }
    /// `CCharacter::update` on a body
    /// (`dead_`, state 6): the fade, the range, the camera test and the
    /// deletion that are left to one.
    void body_update(Object& o, Character& c, float delta, std::vector<ScriptEvent>& out);
    /// `CLevel::update`'s near-list timer, once per level: advanced by the
    /// first machine of the level's list, which rebuilds every machine's list.
    void step_near_unit_lists(float delta, std::vector<ScriptEvent>& out);
    /// `CGameClient::getIsPaused`: a quest box up in any of the level's
    /// machines stops the whole level.
    bool level_paused() const;
    /// A modal menu of the interface is open (`CGameUI::modalDialogOpenPartial`
    /// which `getIsPaused` counts): set by whoever
    /// draws it.
    bool modal_open_ = false;
    /// The player as every machine sees it: one enabled flag (`set_player_enabled` writes all
    /// machines), and a camera controller anywhere holding the camera.
    bool level_player_enabled() const;
    bool level_camera_taken() const;
    /// The point the level's camera is aimed at (`CCameraControl +0x4c`): the holding
    /// controller's machine's look-at, else this machine's. It moves the near list and active
    /// range with it.
    Vec3 level_camera_look_at() const;
    /// `CGameUI::getUIIsInCinematic`, as far
    /// as the level's machines know it.
    bool ui_in_cinematic() const;
    void step_units(float delta, const Vec3& player, QuestState& quests,
                    std::vector<ScriptEvent>& out);

    LayoutFrame frame_;
    const std::vector<LevelScript*>* level_ = nullptr;
    std::map<int64_t, Character> characters_;
    /// The same characters in the order they were made - the level list's
    /// order within this machine - so a search of the whole level walks
    /// characters rather than every node of every room.
    std::vector<int64_t> character_order_;
    /// This machine's characters ever given a master (`set_unit_master`), the only ones
    /// `pets_of` can find: it looks at these, not at every character.
    std::vector<int64_t> mastered_;
    void forget_character(int64_t id);
    std::set<int64_t> dead_;
    bool defer_death_sample_ = false;
    std::set<int64_t> death_sampled_;
    PassabilityMap* passability_ = nullptr;
    const StatBook* graphs_ = nullptr;
    int difficulty_ = difficulty::kHard;
    float attention_bonus_ = 0.0f;
    float trace_next_sample_ = 0.0f;
    std::string trace_player_name_ = "PLAYER";
    Defender player_defense_;
    CharacterEffects player_effects_;
    /// The effects of a character's master (`+0x640`), empty for one with none.
    CharacterEffects master_effects(const Character& c) const {
        return c.master_player ? player_effects_ : CharacterEffects{};
    }
    float player_radius_ = kDefaultCollisionRadius;
    std::function<std::vector<std::pair<std::string, float>>(const std::string&)>
        animation_catalog_;
    std::function<void(int64_t, std::vector<ScriptEvent>&)> link_activation_;
    /// `CLevel +0xd0`: this machine's character-update listeners (its Damage
    /// Shapes), the newest first, and `+0x2d8`, the removals waiting for
    /// `flushCharacterUpdateListeners`.
    std::vector<int64_t> character_listeners_;
    std::vector<int64_t> character_listener_removals_;
    /// Events for this machine's nodes raised while another machine ran,
    /// handed out at the start of this one's next `update`.
    std::vector<ScriptEvent> outbox_;
    /// INVENTED: 1 until the renderer hands over the floor's monster level.
    int32_t level_monster_level_ = 1;
    const RandomNames* random_names_ = nullptr;
    const UnitTypeHierarchy* unit_types_ = nullptr;
    EquipmentRoller equipment_roller_;
    std::map<int64_t, UnitCreation> preset_creations_;  ///< by node, until `build` takes them
    UnitEquipment unit_equipment_;
    EffectManager::Context unit_effect_tables_;
    bool has_unit_effect_tables_ = false;
    const AffixBook* affix_book_ = nullptr;
    const SoundDataInformation* sound_data_ = nullptr;
    std::function<bool(const std::string& unit)> model_sounds_;
    std::function<bool()> audio_plays_;

    std::vector<Object> objects_;
    /// Object id to its place in `objects_`; looked up, never walked.
    std::unordered_map<int64_t, size_t> index_;

    /// Objects by descriptor, in `CLayoutScene::createDescriptors`
    /// order, then node order within a
    /// descriptor - `CDescriptorManager::update`.
    std::vector<int64_t> update_order_;
    /// PORT: `update_order_` without the descriptors whose per-frame update does nothing here
    /// (`updates_each_frame`), for `update` to walk.
    std::vector<int64_t> frame_order_;
    static bool updates_each_frame(uint32_t type);
    void build_update_order();
    /// `level_activated`'s `Unit Spawner` and `Monster` turns.
    void activate_spawner(Object& o, std::vector<ScriptEvent>& out);
    void activate_monster(Object& o, std::vector<ScriptEvent>& out);
    /// One character of `drop_characters_to_ground`.
    void drop_character_to_ground(Object& o, std::vector<ScriptEvent>& out);
    /// The skill an own effect teaches (`CBaseUnit::activateEffect`), or none.
    const SkillDef* effect_skill(const Effect& effect) const;
    /// A unit's own effects rolled as made (`graphs_` set), a skill one teaches constructed with `with`.
    void roll_own_effects(std::vector<Effect>& effects, EffectRolls& rolls,
                          SkillConstruction& with) const;
    std::vector<int64_t> made_order_;
    Vec3 activation_listener_;
    std::vector<int64_t> logic_groups_;
    std::vector<tl::formats::CmpTimeline> timelines_;

    /// `CLevel::updateLevelUpdateObjects`:
    /// quest controllers waiting to report the dialog's menu event.
    std::vector<int64_t> level_updates_;

    struct PendingDialog {
        int64_t node = 0;
        std::string name;
        std::string unit;
        std::string text;
        std::string sound;
        std::string quest;
        bool offer = false;
        float remaining = 0.0f;
        Vec3 camera_mod;
        int64_t speaker_node = 0;
        /// `CQuestDialogMenu +0xc0`, the unit the menu was opened on, and the
        /// section of its line (`CQuestDialog +0x70`).
        UnitRef menu_unit;
        QuestSection section = QuestSection::Intro;
    };
    std::vector<PendingDialog> dialogs_;
    bool showing_ = false;
    bool dialogs_wait_ = false;
    /// `CCharacter +0x199`, which only `calculateActiveRange` sets: until the
    /// level's first rebuild no character is a target (every unit's first
    /// `setTarget` in the original's trace is `none`).
    bool near_list_built_ = false;
    /// How many of `character_order_` the level held when the player joined
    /// (`CLevel::addCharacter` puts it at the head). Past the end: last.
    size_t player_joined_ = static_cast<size_t>(-1);

    struct FloatyLine {
        int64_t node = 0;
        std::string name;
        std::string unit;
        float remaining = 0.0f;
        bool showing = false;
        bool menu_event_pending = false;
        /// The menu's unit and the line's section, for `onClick(10)` on the
        /// next update.
        UnitRef menu_unit;
        QuestSection section = QuestSection::Intro;
        std::string quest;
    };
    FloatyLine floaty_;

    /// The camera controller holding the camera (`+0x390c` camera mode), or 0.
    int64_t camera_holder_ = 0;
    Vec3 camera_look_at_;
    Vec3 player_at_;
    Vec3 player_forward_{0.0f, 0.0f, 1.0f};
    bool player_moving_ = false;
    bool player_running_ = false;
    float player_walking_speed_ = 0.0f;
    float player_running_speed_ = 0.0f;
    UnitRef player_attack_target_;
    bool player_has_attack_target_ = false;
    bool level_is_town_ = false;
    bool game_playing_ = true;
    bool player_untargetable_ = false;
    bool ui_cinematic_ = false;
    std::vector<int64_t> back_from_town_;
    bool player_enabled_ = true;
    int64_t player_target_ = 0;
    NpcDialogLookup npc_dialog_;
    QuestDialogLookup quest_dialog_;
    std::function<void(const std::string& unit)> quest_for_npc_;
    std::function<bool(const std::string& quest)> quest_known_;
    SoundBankHost sound_host_;
    SoundSlotLookup sound_slot_;
    /// A `Sound` node's bank given its slot 0 from the `GUID`, once.
    void resolve_sound_slot(Object& o);
    /// The broadcast a `Sound` node's outputs go through.
    SoundOutput sound_output(Object& o, QuestState& quests, std::vector<ScriptEvent>& out);
    bool player_alive_ = true;
    float player_hp_ = 0.0f;
    float player_max_hp_ = 0.0f;

    /// The player's own path (`CCharacter+0x750` on the player) and its
    /// `updatePathFollowing` state.
    int64_t player_path_ = 0;
    float player_path_pct_ = -1.0f;
    bool player_path_forward_ = true;
    bool player_has_destination_ = false;
    Vec3 player_destination_;

    /// `CResourceManager+0x41` (X360 `+0x25`): nothing broadcasts while clear; set by
    /// `CResourceManager::CResourceManager`. `+0x40` and
    /// `+0x42` start clear (the layout reseed, `Renderer::Impl::layout_seeding`).
    bool logic_live_ = true;

    const DialogBook* dialog_ = nullptr;
    const UnitBook* units_ = nullptr;
    const NavGrid* nav_ = nullptr;
    float active_range_mod_ = 0.0f;
    float unit_active_range_ = 16.0f;
    float unit_near_range_ = 24.0f;
    float unit_shadow_range_ = 10.0f;
    ViewTest view_test_;
    std::function<void()> item_near_list_;
    PlayerGold player_gold_;
    /// Set while `link_input_declined` re-delivers an input to a link.
    bool link_input_declined_ = false;
    float average_fps_ = 0.0f;
    int32_t destroy_dead_corpse_ = 0;
    int32_t shadows_enabled_ = 1;
    /// The near list's counts (listed, active): this machine's last rebuild,
    /// and the level's last logged.
    std::pair<size_t, size_t> near_counts_{0, 0};
    size_t near_total_ = 0;
    std::pair<size_t, size_t> near_logged_{static_cast<size_t>(-1), 0};
    /// Monsters `CLevel::updateNearUnitList`
    /// left off the level's updated list (+0x88): they neither update nor can
    /// be found as targets.
    std::set<int64_t> inactive_;
    std::set<int64_t> spawning_;
    const CollisionWorld* collision_ = nullptr;
    bool proximity_unit_triggers_ = false;
    std::function<bool(const std::string&)> proximity_exempt_;
    std::function<float(const std::string&, const std::string&)> animation_length_;

    std::set<std::string> unimplemented_;
    bool has_saved_state_ = false;
    std::function<bool(int64_t)> stored_unit_exists_;
    int depth_ = 0;
    /// How deep logic may nest (a wire's input raising outputs, a timeline
    /// event's input moving timelines) before a cycle is cut.
    static constexpr int kMaxLogicDepth = 64;
    float clock_ = 0.0f;
    float game_speed_ = 1.0f;
    bool activated_ = false;
    bool trace_on_ = false;
    bool deliver_wires_ = true;
    std::vector<std::string> trace_;
};

}  // namespace tl::core
