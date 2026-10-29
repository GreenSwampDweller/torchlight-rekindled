// What a level script hands the renderer each frame: `ScriptEvent`s. Split from core/script.h.
#pragma once

#include "core/collision.h"
#include "core/quest.h"
#include "core/units.h"
#include "formats/cmp.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace tl::core {

class LevelScript;
class UnitSpawner;

/// What the script is asking the renderer to do.
enum class ScriptAction {
    SetVisible,      ///< `Show` / `Hide` on something the layout placed
    SetEnabled,      ///< `Enable` / `Disable`; a particle link starts or stops
    MoveNode,        ///< a timeline property track driving a target's position
    CameraOn,        ///< a `Camera Controller` taking the camera
    CameraOff,       ///< ... and handing it back
    CameraLookAt,    ///< where a camera controller has the camera looking this frame
    GameState,       ///< the interface state during a scripted scene
    QuestChanged,    ///< a `Quest Controller` fired
    Animation,       ///< a unit starts or stops an animation
    MoveUnit,        ///< a unit walked by the script moved
    Dialog,          ///< a `Quest Controller` opened or closed a line of dialogue
    Hunt,            ///< a `Monster` started or stopped going after something
    Kill,            ///< a `Monster` died: fall over, do not vanish; `on`: the player struck it
    WalkPlayer,      ///< a `Pathing` node whose `GROUP` is PLAYER: walk the character
    NestedTimeline,  ///< `Play` / `Play Backwards` / `Stop` on a `Layout Link Timeline`
    Music,           ///< `Play` or `Play Level Music` on a `Music` node
    SpawnerInput,    ///< an input reached a `Unit Spawner`: `spawner` takes `input`
    /// `CTriggerUnit::setTriggerState` put a `Unit Trigger`
    /// in `trigger_state`. `on`: `trigger` did it.
    TriggerState,
    /// `CItem::interact`, called by `trigger`
    /// after its `setTriggerState`: the prop's quest event.
    ItemInteracted,
    TriggerCollidable,  ///< `Collidable` / `Not Collidable` on a `Unit Trigger` (`on`)
    Warp,               ///< a `Warper` was activated: go to the dungeon named in `text`
    /// a `Waypoint Activator` was activated: this level's depth becomes a waypoint
    WaypointActivated,
    OpenCinematic,  ///< a `Cinematic` node opens the `[CINEMATIC]` named in `text`
    PlayerEnabled,  ///< the player character was enabled or disabled (`on`)
    Strike,         ///< a character's blow landed: `target_node` takes `amount` (0 = the player)
    /// `CEquipment::fireMissiles` for a character's weapon:
    /// `node_id` looses `text` from its left hand (`on`) or right, at the target when `aimed`.
    WeaponMissile,
    Miss,            ///< a swing met nothing, or a wall (`CSoundBank` slot 0xf)
    Unhandled,       ///< a wire this engine does not implement yet, named in `text`
    ScaleNode,       ///< a timeline track driving a unit's `SCALE`: uniform `amount`
    HideHandTrails,  ///< a unit's two hand weapon trails switched off and hidden
    OrientNode,      ///< a timeline track turning a node: its world `axis_x`/`axis_y`/`axis_z`
    Opacity,         ///< a character's fade (`+0x528`, `CGenericModel::setOpacity`): `amount`
    /// `CCharacter::stopParticles(on)` on the unit placed by `node_id`: its
    /// following and `BIND` particle triggers stopped (`ThemeParticleTriggers`)
    StopUnitParticles,
    CastShadows,  ///< a character casts a shadow or stops (`+0x530`): `on`
    PlayerPet,    ///< `Add as Pet` (`on`) / `Remove as Pet`: `addPet` / `removePet` on the player
    JumpLanded,   ///< a jump down reached the ground (`jumpDownAI`): `LAND_SOUND` at `position`
    DeathLanded,  ///< a flung body reached the end of its flight (`completeDeath`); `on`: on ground
    CompleteQuest,     ///< `OK` on a `[COMPLETE]` line: `CQuestManager::completeQuest` on `quest`
    PlayerInvincible,  ///< a `Game State Controller`'s `INVULNERABLE`: the player's `+0x52e` (`on`)
    HealPlayer,        ///< `Heal Player`: `modifyHP(maxHP())` on the player
    /// a `Teleport` node's `Activate Teleport`: the player to `position`, facing `facing`
    TeleportPlayer,
    /// a `Dungeon Object`'s `Clear History`: `CPlayer::clearDungeonHistory(name)`
    /// on every player, then `updateStoredLevels` on the level it stands in
    ClearDungeonHistory,
    ShowTip,            ///< `Show Tip`: `CGameUI::queueTip` with the tip in `input`
    PlayerMeshVisible,  ///< the player's `setMeshVisible(on, true)` (`HIDE PLAYER`, a skip)
    ParticleNode,  ///< a `Particle` node's own system: `CParticleTechWrapper::start` (`on`) / `stop`
    ParticleLinkPause,  ///< a particle link's `CParticle::Pause`: `CLayout::pause` on its layout
    AffectorEnabled,    ///< a particle affector's `ENABLE` (`CAffectorWrapper::setEnabled`): `on`
    EmitterEnabled,  ///< an `Emitter` node's emission (`CParticleEmitterWrapper::setEnabled`): `on`
    CameraShake,  ///< `Activate` on a `Camera Shake`: its `node`, about `position` (the layout's)
    /// `CCharacter::convertCharacter` re-made `node_id` as
    /// `unit` (it was `text`); the scene drops the old model, makes the new, plays `TRANSFORM`.
    Transform,
    /// `CSoundBank::playSample(slot, node, 0, 0, false)` on a character's own
    /// bank (`+0x298`), at its node: `input` is the slot (`SoundSlot`).
    UnitSample,
    /// `CLayoutDescriptor::InputLogicEvent`: an input a
    /// `Layout Link Timeline` has no case for goes to the link's layout
    /// (`CLayout::processInputs`), and back to the link if that does not take it.
    LinkInput,
    /// `CCharacter::displayDamageAbsorbed`
    /// has a number to show over a character: `amount`, and `on` when it is one
    /// of the player's side (red, not orange).
    DamageText,
};

/// One thing that happened this frame.
struct ScriptEvent {
    ScriptAction action = ScriptAction::SetVisible;

    /// The node acted on, and its name for the log. Zero for events that are
    /// about the level rather than about a node.
    int64_t node_id = 0;
    std::string name;

    bool on = false;  ///< SetVisible / SetEnabled / GameState / PlayerEnabled
    Vec3 position;    ///< MoveNode, CameraLookAt, MoveUnit, WalkPlayer, Spawn

    /// CameraOn: the node the camera follows, or 0.
    int64_t follow_id = 0;

    /// CameraOn: `PAN TIME` and the ease point, for the log.
    float pan_time = 0.0f;
    float ease_at = 1.0f;

    /// GameState: the `STATE` string (type in `input`). QuestChanged: the quest. Animation:
    /// the animation. Dialog: the line, on the opening event. Hunt: what is hunted. Music: the
    /// `FILE NAME` (`input` says which input). Unhandled: the wire.
    std::string text;

    /// Animation, MoveUnit, Dialog: the unit's `MONSTER` name.
    /// Dialog: the line's `SOUND` definition (`CQuestDialog +0xa0`).
    std::string sound;
    std::string unit;
    bool loops = false;
    /// Animation: the unit is the level's player (an `Animation Controller`
    /// whose `GROUP` is `PLAYER`), not a placed one.
    bool to_player = false;

    /// Animation: the blend in, from the controller's `BLEND TIME`.
    float blend = 0.0f;

    /// How fast to play it, as a multiple of the unit's own authored rate.
    float rate = 1.0f;
    /// Animation: seconds the units' models are stepped on at once after the
    /// start (`CAnimationPlayer::setAnimationDuration`'s
    /// `CGenericModel::updateAnimation(duration, true)`); 0 for none.
    float advance = 0.0f;

    /// Animation, on a play: back to `IDLE` once a one-shot has run, as
    /// `CCharacter::setAIPlayAnimation` queues.
    bool then_idle = false;
    /// Animation, on a play: one-shots queued behind it, in order, before the
    /// idle `then_idle` asks for (`queueBlendAnimation(name, false, ..)`).
    std::vector<std::string> queue;

    /// Hunt, on a target chosen: the node being hunted, or 0 for the player.
    /// Strike: the node struck.
    int64_t target_node = 0;
    /// WeaponMissile: it has a target (`target_node`, `target_machine`).
    bool aimed = false;
    /// ... and the machine that runs it, which need not be the one that sent
    /// the event: a room's units and the level's share one list. Null for the
    /// player.
    LevelScript* target_machine = nullptr;

    /// Warp: the Warper's `LEVEL DELTA`, `LEVEL ABSOLUTE`, `WAYPOINT` and
    /// `WARP NAME`. The dungeon is `text`.
    int32_t level_delta = 1;
    int32_t level_absolute = 0;
    bool waypoint = false;
    std::string warp_name;

    /// MoveUnit: which way the unit is now facing, as a yaw in radians about +Y.
    float facing = 0.0f;

    /// MoveUnit: whether `facing` means anything this frame.
    bool turned = false;

    /// OrientNode: the node's orientation columns in the level (`RIGHT`, `UP`,
    /// `FORWARD`), as `CPositionableObject +0xc0` holds them: not normalised.
    Vec3 axis_x;
    Vec3 axis_y;
    Vec3 axis_z;

    /// MoveUnit: walking (false) or running.
    bool running = false;

    /// Dialog: how long the line is up for, and whether it floats.
    float seconds = 0.0f;
    bool floaty = false;
    /// Dialog (boxed): the controller's quest, whether the line is an `[INTRO]` offer, and on
    /// closing the menu event (2 OK, 3 accepted, 4 declined; `CQuestDialogMenu::onClick`).
    ///
    std::string quest;
    bool offer = false;
    int32_t menu_event = 0;
    /// Dialog (boxed): the line's `CAMERAMODX/Y/Z`, and the menu's unit when it is a character
    /// (the player's target after `CGameUI::forceInteractiveDialog`),
    /// which the close-up aims at; 0 for a prop's line.
    Vec3 camera_mod;
    int64_t speaker_node = 0;

    /// NestedTimeline: which way round to play it.
    bool backwards = false;

    /// TriggerState: the state (0 the first), the prop's first and last, whether forced (a
    /// prop already there plays it again) and animated, and whether its sound plays (only
    /// while the level is active).
    int32_t trigger_state = 0;
    int32_t trigger_first = 0;
    int32_t trigger_last = 1;
    bool trigger_forced = false;
    bool trigger_animate = true;
    bool trigger_sound = false;
    /// TriggerState from `trigger`: a one-shot reached its last state (its
    /// `[TREASURE]` is spawned), and the prop's skill was rolled
    /// (`selectRandomSkill`: a one-shot's last state, or any `Cycle` firing).
    bool trigger_spent = false;
    bool trigger_skill = false;
    /// TriggerState from `trigger`: the character it was handed - the player's
    /// `interact`, or none for a wire's `Trigger`.
    bool by_player = false;

    /// CameraShake: the node, whose fields the renderer reads with the shake
    /// curves it holds (`camera_shake_from_node`).
    const tl::formats::CmpNode* node = nullptr;

    /// SpawnerInput: the spawner the machine holds for the node, the input
    /// (`EINPUT_EVENTS`), and the machine, which takes the spawner's outputs.
    UnitSpawner* spawner = nullptr;
    uint32_t input = 0;
    LevelScript* machine = nullptr;

    QuestStatus status = QuestStatus::Unknown;

    /// Strike: the damage after armour, and how it was dealt.
    float amount = 0.0f;
    bool critical = false;
    bool blocked = false;
    /// Strike: how many of the blow's `modifyDamage` calls the struck one's
    /// `SHIELD BUFFER`s took whole, each an `Absorbed!` and
    /// a slot 7 sound over it.
    int absorbed = 0;
    /// Strike: a damage-over-time shape's (`applyDamage` without its struck
    /// flag, so no `CAST SKILL ON STRUCK`).
    bool over_time = false;
    /// Strike: `CCharacter::rollAttack` decided it, so its tail (knock-back, degraded armour,
    /// `TRANSFER` effects) follows. A damage shape's strike is not one.
    bool roll_attack = false;
    /// Strike and Kill: the blow's direction and force (`applyDamage`'s, for
    /// `die`), and on a Kill whether the body bursts (a critical, or a blow of
    /// at least 1.75 times the maximum).
    bool has_direction = false;
    Vec3 direction;
    float force = 0.0f;
    bool burst = false;
    /// Strike: `Blow::by_type` and `Blow::parts`, and the attack's own damage
    /// type - what `rollAttack` hands `applyDamageEffects`.
    std::array<int, kDamageTypeCount> by_type{};
    int parts = 0;
    int first_type = 0;
};

}  // namespace tl::core
