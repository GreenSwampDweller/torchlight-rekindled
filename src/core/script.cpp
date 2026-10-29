#include "core/script.h"

#include "core/item_drop.h"

#include "core/active_range.h"
#include "core/ai_clock.h"
#include "core/context_tips.h"
#include "core/hp_events.h"
#include "core/log.h"
#include "core/logic_names.h"
#include "core/ogre_math.h"
#include "core/quest_book.h"
#include "core/randomizer.h"
#include "core/runic_math.h"
#include "core/skill_cast.h"
#include "core/unit_types.h"
#include "formats/float_convert.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <limits>

namespace tl::core {
namespace {

namespace type = tl::formats::cmp_type;
using namespace tl::core::logic;

/// A quest's name as `CQuest::loadQuestData` stores it (`+0x50`,
/// `STRINGS::StringUpper`).
std::string quest_upper(std::string text) {
    for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

/// A field's text, or empty when the node is shorter than the schema.
// PORT: asset parsing, a layout node's field read with its fallback.
std::string text_at(const tl::formats::CmpNode& node, size_t index) {
    const tl::formats::CmpField* f = node.field(index);
    return f != nullptr ? f->as_text() : std::string();
}

// PORT: asset parsing, a layout node's field read with its fallback.
float float_at(const tl::formats::CmpNode& node, size_t index, float fallback) {
    const tl::formats::CmpField* f = node.field(index);
    return f != nullptr && f->count() >= 1 ? f->as_float() : fallback;
}

/// A boolean field, defaulting to `fallback` when the node does not carry it.
// PORT: asset parsing, a layout node's field read with its fallback.
bool bool_at(const tl::formats::CmpNode& node, size_t index, bool fallback) {
    const tl::formats::CmpField* f = node.field(index);
    return f != nullptr && f->count() == 1 ? f->as_bool() : fallback;
}

/// An integer field (`COUNT`, `EQUALS VALUE`): one dword read as a signed int.
// PORT: asset parsing, a layout node's field read with its fallback.
int32_t int_at(const tl::formats::CmpNode& node, size_t index, int32_t fallback) {
    const tl::formats::CmpField* f = node.field(index);
    return f != nullptr && f->count() == 1 ? static_cast<int32_t>(f->as_uint()) : fallback;
}

// PORT: asset parsing, a layout node's field read with its fallback.
Vec3 vec3_at(const tl::formats::CmpNode& node, size_t index) {
    const tl::formats::CmpField* f = node.field(index);
    if (f == nullptr || f->count() != 3) return {};
    return {f->as_float(0), f->as_float(1), f->as_float(2)};
}

// PORT: the parser's vector as the machine's.
Vec3 to_vec3(const tl::formats::CmpVec3& v) { return {v.x, v.y, v.z}; }

/// A node made under `CAffectorDescriptor`: a `CAffectorWrapper`
/// (`CAffectorDescriptor::InputLogicEvent`), whose `ENABLE`
/// `CAffectorDescriptor::CAffectorDescriptor` registers.
bool is_affector(uint32_t node_type) {
    switch (node_type) {
        case type::kColorOverTime:
        case type::kScaleOverTime:
        case type::kTextureRotate:
        case type::kTextureAnimation:
        case type::kGravityWell:
        case type::kVortex:
        case type::kSineForce:
        case type::kLinearForce:
        case type::kAlign:
        case type::kJet:
        case type::kGeometryRotator:
        case type::kPlaneCollision:
        case type::kSphereCollision: return true;
        default: return false;
    }
}

/// A layout `FORWARD` as a yaw about +Y from +Z. A node that carries none faces +Z.
// PORT: a layout `FORWARD` as the port's yaw about +Y from +Z.
float yaw_of(const Vec3& forward) {
    if (forward.x * forward.x + forward.z * forward.z < 1e-8f) return 0.0f;
    return std::atan2(forward.x, forward.z);
}

// INVENTED: the ground distance the port's walk measures to say the player's
// destination is reached (a stand-in for `setDestination`'s search, below).
float distance_xz(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

float distance_squared(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

}  // namespace

// `CLevel::findCharacterWithinView`, the test
// it puts each candidate through, in the looker's own frame.
namespace {
bool can_see_turned(const OgreQuaternion& turn, const Vec3& from, const Vec3& at,
                    float motion_radius, float view_angle, float sight_range);
}  // namespace

bool can_see(const Vec3& from, float facing, const Vec3& at, float motion_radius, float view_angle,
             float sight_range) {
    // PORT: the looker's turn from the port's facing, a yaw about +Y.
    const OgreQuaternion turn{std::cos(facing * 0.5f), 0.0f, std::sin(facing * 0.5f), 0.0f};
    return can_see_turned(turn, from, at, motion_radius, view_angle, sight_range);
}

bool can_see_along(const Vec3& from, const Vec3& forward, const Vec3& at, float motion_radius,
                   float view_angle, float sight_range) {
    // UNIT_Y x forward, `fmsubs` each.
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const Vec3 right{std::fma(forward.z, up.y, -(forward.y * up.z)),
                     std::fma(forward.x, up.z, -(forward.z * up.x)),
                     std::fma(forward.y, up.x, -(forward.x * up.y))};
    return can_see_turned(quaternion_from_axes(right, up, forward), from, at, motion_radius,
                          view_angle, sight_range);
}

namespace {
bool can_see_turned(const OgreQuaternion& turn, const Vec3& from, const Vec3& at,
                    float motion_radius, float view_angle, float sight_range) {
    // The candidate through the inverse of the looker's transform
    // (`Matrix4::inverse`); the distance in the x-z plane in
    // one `fmadds`, the console's `atan2(x, z)` rounded and in degrees.
    const Vec3 local = matrix_transform(matrix_inverse(object_transform(turn, from)), at);
    const float d = std::sqrt(std::fma(local.x, local.x, local.z * local.z));
    // Each test as its branch goes (`ble`, `bge` are taken on a NaN).
    if (!(d > motion_radius)) return true;
    const float angle =
        static_cast<float>(crt_atan2(static_cast<double>(local.x), static_cast<double>(local.z))) *
        57.29582595825195f;
    // within the view angle, or (a view at all) close and
    // within 30 degrees.
    if (std::fabs(angle) > view_angle) {
        if (!(view_angle > 0.0f)) return false;
        if (!(d < 2.0f)) return false;
        if (!(std::fabs(angle) < 30.0f)) return false;
    }
    return !(d > sight_range);
}
}  // namespace

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

void LevelScript::build(const tl::formats::CmpScene& scene, const std::set<int64_t>& gated) {
    objects_.clear();
    index_.clear();
    update_order_.clear();
    frame_order_.clear();
    logic_groups_.clear();
    timelines_ = scene.timelines;
    unimplemented_.clear();
    // The machine's own off the map before it is rebuilt; their pets elsewhere
    // stay where they are.
    for (auto& entry : characters_) avoidance_remove_self(entry.second);
    characters_.clear();
    character_order_.clear();
    dead_.clear();
    inactive_.clear();
    near_list_built_ = false;
    player_joined_ = static_cast<size_t>(-1);
    // No reseed here: a nested layout's machine (a room, a skill's or a death's
    // layout) is built mid-play, and the generators run on. The level load
    // seeds them itself, before its own `build` (`reseed_level_generators`).
    dialogs_.clear();
    level_updates_.clear();
    character_listeners_.clear();
    character_listener_removals_.clear();
    outbox_.clear();
    floaty_ = FloatyLine{};
    showing_ = false;
    camera_holder_ = 0;
    logic_live_ = true;
    clock_ = 0.0f;
    activated_ = false;
    player_path_ = 0;

    for (const tl::formats::CmpNode& node : scene.nodes) {
        if (gated.count(node.id) != 0) continue;

        // The node's descriptor makes its object with the class's constructor
        // (`CDescriptor::loadObject` -> `CreateObject`), then sets it from the node. Nothing more
        // is read for `COutputIncrementorDescriptor::CreateObject`,
        // `CCameraShakeDescriptor::CreateObject` (node
        // kept as `source`), `CLogicGroupDescriptor::CreateObject`
        // and `CCinematicDescriptor::CreateObject`
        // (`CCinematicObject::CCinematicObject`).
        Object o;
        o.id = node.id;
        o.type = node.type;
        o.name = node.name;
        o.parent = node.parent_id;
        if (node.has_position()) o.position = to_vec3(node.position());
        if (node.type == type::kCameraShake) o.source = &node;

        switch (node.type) {
            case type::kPlayerSphereTrigger: {
                // `CTriggerSphereDescriptor::CreateObject`: a `CTriggerSphere`
                // with its constructor's defaults, then the node's properties.
                namespace f = tl::formats::cmp_field::player_sphere_trigger;
                // `CTriggerSphere::CTriggerSphere`: radius 1,
                // scale 1, on `CLogicTrigger::CLogicTrigger`:
                // nothing inside or fired, enabled, visible.
                o.position = vec3_at(node, f::kPosition);
                o.enabled = bool_at(node, f::kEnabled, true);
                o.radius = float_at(node, f::kRadius, 1.0f);
                break;
            }
            case type::kPlayerBoxTrigger: {
                // `CTriggerBoxDescriptor::CreateObject`: a `CTriggerBox`
                // with its constructor's defaults, then the node's properties.
                namespace f = tl::formats::cmp_field::player_box_trigger;
                o.position = vec3_at(node, f::kPosition);
                o.enabled = bool_at(node, f::kEnabled, true);
                // `CTriggerBox::setDimensions`: dimensions,
                // halves, and scale set to them (`CTriggerBox::scaleUpdated`,
                // never differs here). `CTriggerBox::CTriggerBox`:
                // `setDimensions(4, 4, 4)`.
                const tl::formats::CmpField* dims = node.field(f::kDimensions);
                o.half_extent =
                    (dims != nullptr && dims->count() == 3 ? vec3_at(node, f::kDimensions)
                                                           : Vec3{4.0f, 4.0f, 4.0f}) *
                    0.5f;
                break;
            }
            case type::kWarper: {
                // `CWarperDescriptor::CreateObject`;
                // `CWarper::CWarper`: level delta 1, no absolute level, enabled,
                // not a waypoint.
                namespace f = tl::formats::cmp_field::warper;
                o.position = vec3_at(node, f::kPosition);
                o.forward = vec3_at(node, f::kForward);
                o.enabled = bool_at(node, f::kEnabled, true);
                o.dungeon = text_at(node, f::kDungeonName);
                o.warp_name = text_at(node, f::kWarpName);
                o.level_delta = int_at(node, f::kLevelDelta, 1);
                o.level_absolute = int_at(node, f::kLevelAbsolute, 0);
                o.waypoint = bool_at(node, f::kWaypoint, false);
                break;
            }
            case type::kTimer: {
                // `CLogicTimerDescriptor::CreateObject`;
                // `CLogicTimer::CLogicTimer` defaults, the node's through
                // `CLogicTimerDescriptor::Set_setTimer` and its neighbours.
                namespace f = tl::formats::cmp_field::timer;
                o.enabled = bool_at(node, f::kEnabled, true);
                o.timer_time = float_at(node, f::kTime, 1.0f);
                o.timer_max = float_at(node, f::kMaxTime, -1.0f);
                o.timer_loops = int_at(node, f::kLoopCount, 1);
                o.timer_loops_left = o.timer_loops;
                o.timer_forever = bool_at(node, f::kLoopsForever, false);
                timer_reset(o);
                break;
            }
            case type::kTimeline: {
                // `CTimelineDescriptor::CreateObject`: a `CTimeline`
                // with its constructor's defaults, then the node's properties.
                namespace f = tl::formats::cmp_field::timeline;
                // `CTimeline::CTimeline`: a step of 0 (+0x70), a duration of 1
                // (+0x74), not looping (+0x69), started on load (+0x6A), under the layout's control (+0x6B) and
                // setting its properties on load (+0x83) - the defaults a text layout's unset properties keep.
                o.tl_duration = float_at(node, f::kDuration, 1.0f);
                o.tl_step = float_at(node, f::kStep, 0.0f);
                o.tl_loops = bool_at(node, f::kLoops, false);
                o.tl_start_on_load = bool_at(node, f::kStartOnLoad, true);
                o.tl_set_properties_on_load = bool_at(node, f::kResetProperties, true);
                o.tl_layout_control = bool_at(node, f::kLayoutControl, true);
                o.tl_modification = timeline_mod_id_by_string(text_at(node, f::kType));
                // `CTimelineDescriptor::Set_setDefaultInterpolationType`.
                o.tl_default_interpolation =
                    parse_timeline_interpolation(text_at(node, f::kDefaultInterpolation));
                break;
            }
            case type::kCameraController: {
                // `CCameraControllerDescriptor::CreateObject`;
                // `CCameraController::CCameraController`
                // defaults.
                namespace f = tl::formats::cmp_field::camera_controller;
                o.position = vec3_at(node, f::kPosition);
                o.cam_pan = float_at(node, f::kPanTime, 1.5f);
                // `Set_setCameraEaseInPCT` writes the graph point's x (+0x150),
                // `Set_setCameraEaseInDistancePCT` its y (+0x154).
                // Both clamp to [0, 1] (`Set_setCameraEaseInPCT`).
                o.cam_ease_x = std::clamp(float_at(node, f::kEaseInPct, 0.8f), 0.0f, 1.0f);
                o.cam_ease_y = std::clamp(float_at(node, f::kEaseInDistancePct, 0.9f), 0.0f, 1.0f);
                o.cam_pause = float_at(node, f::kTime, 0.0f);
                o.cam_resets = bool_at(node, f::kResetsAtEnd, true);
                o.cam_follow = bool_at(node, f::kFollowUnit, false);
                // `Set_setCameraType`, through `getTypeIDByString`:
                // the index in {"Position", "Unit"}.
                std::string kind = text_at(node, f::kType);
                for (char& c : kind)
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (kind == "POSITION") o.cam_type = 0;
                if (kind == "UNIT") o.cam_type = 1;
                // `CCameraController::setUnitInteractWith`: the name
                // kept (`+0x110`), its data group looked up, the unit found
                // before dropped.
                o.unit = text_at(node, f::kUnit);
                // +0x82, `setEnabled`'s flag, cleared by
                // `CSceneNodeObject::CSceneNodeObject`; nothing
                // of the camera controller's sets it.
                o.enabled = false;
                break;
            }
            case type::kGameStateController: {
                // `CGameStateControllerDescriptor::CreateObject`;
                // `CGameStateController::CGameStateController`: disabled, not
                // invulnerable, state 1, tip -1.
                namespace f = tl::formats::cmp_field::game_state_controller;
                o.enabled = bool_at(node, f::kEnabled, false);
                o.gs_invulnerable = bool_at(node, f::kInvulnerable, false);
                o.gs_state = text_at(node, f::kState);
                // `STATE` through `getGameStateIDByString`, 0
                // when it names nothing. An unauthored one keeps the constructor's 1,
                // `PAUSE PLAYER`.
                if (!o.gs_state.empty()) o.gs_type = game_state_id_by_string(o.gs_state);
                // `TIP` through `getTipIDByString`: 0 for a name it
                // does not know, the constructor's -1 without the field. The
                // compiler writes every unauthored one as `TIP_INVENTORY`.
                const std::string tip = text_at(node, f::kTip);
                if (!tip.empty()) o.gs_tip = std::max(0, context_tip_by_name(tip));
                break;
            }
            case type::kQuestController: {
                // `CQuestControllerDescriptor::CreateObject`;
                // `CQuestController::CQuestController`: no quest
                // or unit, group `MONSTERS`, neither broadcasting on load nor disabling the player.
                namespace f = tl::formats::cmp_field::quest_controller;
                o.quest = text_at(node, f::kQuest);
                // `CQuestController::setUnitInteractWith`: the
                // name kept (`+0x90`), its data group looked up (`+0x70`), the
                // unit found before dropped (`+0x78`).
                o.unit = text_at(node, f::kUnit);
                o.qc_broadcast_on_load = bool_at(node, f::kBroadcastOnLoad, false);
                o.qc_player_disabled = bool_at(node, f::kDisablePlayer, false);
                break;
            }
            case type::kMusic: {
                namespace f = tl::formats::cmp_field::music;
                o.resource = text_at(node, f::kFileName);
                // `CMusicObject::CMusicObject` loops (`+0x72`)
                // until `LOOPS` says otherwise.
                o.loops = bool_at(node, f::kLoops, true);
                break;
            }
            case type::kSound: {
                // `CSoundObjectDescriptor::CreateObject`: a `CSoundObject`
                // with its constructor's defaults, then the node's properties.
                namespace f = tl::formats::cmp_field::sound;
                o.position = vec3_at(node, f::kPosition);
                o.resource = text_at(node, f::kGuid);
                o.sound.bank.radius = 5.0f;
                o.sound.start_on_activate = bool_at(node, f::kStartOnActivate, true);
                o.sound.environmental = bool_at(node, f::kEnvironmental, true);
                if (const tl::formats::CmpField* v = node.field(f::kVolume)) {
                    if (v->count() == 1) o.sound.bank.volume_scale = v->as_float();
                }
                if (const tl::formats::CmpField* r = node.field(f::kRadius)) {
                    if (r->count() == 1) o.sound.bank.radius = r->as_float();
                }
                break;
            }
            case type::kTeleport: {
                // `CTeleportDescriptor::CreateObject`:
                // `CTeleport::CTeleport` enabled, then the node's `ENABLED`.
                o.enabled = bool_at(node, tl::formats::cmp_field::teleport::kEnabled, true);
                break;
            }
            case type::kDungeonObject: {
                // `CDungeonObjectDescriptor::CreateObject`:
                // `CDungeonObject::CDungeonObject` with an empty name, no entity;
                // `DUNGEON NAME` through `Set_setDungeon`.
                //
                o.dungeon_name =
                    text_at(node, tl::formats::cmp_field::dungeon_object::kDungeonName);
                break;
            }
            case type::kSkipCutscene: {
                // `CSkipCutsceneDescriptor::CreateObject`;
                // `CSkipCutscene::CSkipCutscene`: disabled, nothing pending,
                // skills left.
                namespace f = tl::formats::cmp_field::skip_cutscene;
                o.enabled = bool_at(node, f::kEnabled, false);
                o.skip_skills = bool_at(node, f::kDisableSkills, false);
                break;
            }
            case type::kCounter: {
                // `CCounterDescriptor::CreateObject`: a `CCounter`
                // with its constructor's defaults, then the node's properties.
                // `CCounter::CCounter` defaults.
                namespace f = tl::formats::cmp_field::counter;
                o.enabled = bool_at(node, f::kEnabled, true);
                o.counter_equals = int_at(node, f::kEqualsValue, 10);
                o.counter_start = int_at(node, f::kStartingValue, 0);
                o.counter_value = o.counter_start;
                // `LOGIC`, authored as its name (`Set_setLogicType` stores the
                // index).
                o.counter_type = counter_type_id_by_string(text_at(node, f::kLogic));
                break;
            }
            case 43: {  // Output Incrementor
                // `COutputIncrementor::COutputIncrementor`: maximum 5, count 0,
                // one loop, enabled. `Set_setMaxIncrement` keeps a maximum under 6
                // (else 5) and zeroes the count; `Set_setLoopCount`;
                // `Set_setLoopForever`.
                namespace f = tl::formats::cmp_field::output_incrementor;
                o.enabled = bool_at(node, f::kEnabled, true);
                const int32_t max = int_at(node, f::kMaxOutput, 5);
                o.incr_max = max < 6 ? max : 5;
                o.counter_value = 0;
                o.incr_loop_count = int_at(node, f::kLoopCount, 1);
                o.incr_loops_left = o.incr_loop_count;
                o.incr_forever = bool_at(node, f::kLoopsForever, false);
                break;
            }
            case type::kMoneyTaker:
                // `CMoneyTakerDescriptor::CreateObject`: a `CMoneyTaker`
                // with its constructor's defaults, then the node's properties.
                // `CMoneyTaker::CMoneyTaker`: `+0x58 = 0`.
                o.money_amount = int_at(node, tl::formats::cmp_field::money_taker::kAmount, 0);
                break;
            case type::kPathing: {
                // `CPathControllerDescriptor::CreateObject`;
                // `CPathController::CPathController` defaults.
                namespace f = tl::formats::cmp_field::pathing;
                o.position = vec3_at(node, f::kPosition);
                o.enabled = bool_at(node, f::kEnabled, true);
                o.path_enable_units = bool_at(node, f::kEnableOnStart, false);
                o.path_runs = bool_at(node, f::kRun, true);
                o.path_to_player = bool_at(node, f::kPathToPlayer, false);
                // `CPathController::setUnitInteractWith`: the
                // name kept, `PLAYER` marked, the units collected before forgotten.
                o.unit = text_at(node, f::kUnit);
                // `CPathController::CPathController` starts `+0x104` as the second of the
                // resource categories (+0x1C), `MONSTERS`; a `GROUP` the node sets replaces it.
                o.path_group = node.field(f::kGroup) != nullptr ? text_at(node, f::kGroup)
                                                                : std::string("MONSTERS");
                o.path_name = text_at(node, f::kPathName);
                // `CPathController::setArrayOfVectors`: the
                // floats copied flat, three to a waypoint.
                if (const tl::formats::CmpField* points = node.field(f::kPoints)) {
                    const size_t count = points->count() / 3;
                    o.points.reserve(count);
                    for (size_t i = 0; i < count; ++i) {
                        o.points.push_back({points->as_float(3 * i), points->as_float(3 * i + 1),
                                            points->as_float(3 * i + 2)});
                    }
                }
                // `CPathController::caculateSpline`: the running
                // length along the waypoints, and the total.
                o.cumulative.clear();
                o.path_total = 0.0f;
                for (size_t i = 0; i < o.points.size(); ++i) {
                    if (i > 0) {
                        o.path_total += std::sqrt(distance_squared(o.points[i], o.points[i - 1]));
                    }
                    o.cumulative.push_back(o.path_total);
                }
                break;
            }
            case type::kAnimationController: {
                // `CAnimationPlayerDescriptor::CreateObject`;
                // `CAnimationPlayer::CAnimationPlayer` defaults.
                namespace f = tl::formats::cmp_field::animation_controller;
                o.anim_start_on_load = bool_at(node, f::kStartOnLoad, false);
                o.anim_blend = float_at(node, f::kBlendTime, 0.2f);
                o.anim_blend_out = float_at(node, f::kBlendOutTime, 0.2f);
                o.anim_force = float_at(node, f::kDurationOverride, 0.0f);
                o.anim_play_idle = bool_at(node, f::kPlayIdle, false);
                o.anim_name = text_at(node, f::kAnimation);
                o.anim_group = text_at(node, f::kGroup);
                // `CAnimationPlayer::CAnimationPlayer` starts `UNIT` (+0xA4) as `PLAYER`.
                o.anim_unit = node.field(f::kUnit) != nullptr ? text_at(node, f::kUnit)
                                                              : std::string("PLAYER");
                // The constructor's `GROUP` and `UNIT` `PLAYER`:
                // a controller that authors neither is the player's.
                if (o.anim_group.empty()) o.anim_group = "PLAYER";
                break;
            }
            case type::kSkillController: {
                // `CSkillControllerDescriptor::CreateObject`;
                // `CSkillController::CSkillController`: flags
                // clear, level 0.
                namespace f = tl::formats::cmp_field::skill_controller;
                // `CSkillController::CSkillController`: learn on start (+0x15C), unlearn on stop
                // (+0x15D) and the unit's target (+0x162) set; no forced stop (+0x160), not the player (+0x161).
                o.sk_learn_on_start = bool_at(node, f::kLearnOnStart, true);
                o.sk_unlearn_on_stop = bool_at(node, f::kUnlearnOnStop, true);
                o.sk_force_stop = bool_at(node, f::kForceStop, false);
                o.sk_target_player = bool_at(node, f::kTargetPlayer, false);
                o.sk_use_unit_target = bool_at(node, f::kUseUnitTarget, true);
                // Quirk kept: `SKILL LEVEL` is a flag handed to a setter
                // that stores a whole word: an authored level is 0x01000000 (the last
                // level).
                o.sk_level = int_at(node, f::kSkillLevel, 0) != 0 ? 0x01000000 : 0;
                o.sk_group = text_at(node, f::kGroup);
                // `CSkillController::setUnitInteractWith`
                // (inlined in its descriptor's setter): the
                // name, its data group, the unit found before dropped.
                o.sk_unit = text_at(node, f::kUnit);
                o.sk_skill = text_at(node, f::kSkill);
                break;
            }
            case 39: {  // Random Choice
                // `CRandomChoiceDescriptor::CreateObject`;
                // `CRandomChoice::CRandomChoice`: first type, enabled, count 1,
                // weights 0.
                namespace f = tl::formats::cmp_field::random_choice;
                o.enabled = bool_at(node, f::kEnabled, true);
                // `TYPE`, authored as its name (`Set_setRandomType`).
                o.choice_mode = choice_type_id_by_string(text_at(node, f::kType));
                o.choice_count = int_at(node, f::kCount, 1);
                for (size_t i = f::kOne; i <= f::kFive; ++i)
                    o.weights.push_back(int_at(node, i, 0));
                break;
            }
            case 46: {  // Puzzle Input (`CPuzzleRandomizer`)
                // `CPuzzleRandomizerDescriptor::CreateObject`: a `CPuzzleRandomizer`
                // with its constructor's defaults, then the node's properties.
                namespace f = tl::formats::cmp_field::puzzle_input;
                // `CPuzzleRandomizer::CPuzzleRandomizer`: enabled, unsolved, state 0,
                // and setActiveInputs(3) takes 15 draws from `volatile_random()` as the layout is
                // built.
                o.enabled = bool_at(node, f::kEnabled, true);  // +0x68
                puzzle_set_active_inputs(o, 3);
                // `CPuzzleRandomizerDescriptor::Set_setActiveInputs` is called
                // when NUMBER OF INPUTS is authored, running setActiveInputs(n) and taking
                // another 5 * n draws from g_RandVolatile.
                if (node.field(f::kNumberOfInputs) != nullptr) {
                    puzzle_set_active_inputs(
                        o, static_cast<uint32_t>(int_at(node, f::kNumberOfInputs, 3)));
                }
                break;
            }
            case type::kDamageShape: {
                // `CDamageShapeDescriptor::CreateObject` -> `CDamageShape`.
                // No `FORWARD`/`RIGHT`/`UP`
                // of its own: the node's axes are the room's.
                o.damage_shape = read_damage_shape(node, unit_types_);
                o.position = vec3_at(node, tl::formats::cmp_field::damage_shape::kPosition);
                o.forward = {0.0f, 0.0f, 1.0f};
                o.right = {1.0f, 0.0f, 0.0f};
                o.up = {0.0f, 1.0f, 0.0f};
                o.enabled = o.damage_shape->enabled;
                break;
            }
            case type::kUnitSpawner: {
                // `CUnitSpawnerDescriptor::CreateObject` ->
                // `CUnitSpawner`, then
                // `DescriptorObjectHasBeenInited` -> `reset`.
                namespace f = tl::formats::cmp_field::unit_spawner;
                o.position = vec3_at(node, f::kPosition);
                o.forward = vec3_at(node, f::kForward);
                o.right = vec3_at(node, f::kRight);
                o.up = vec3_at(node, f::kUp);
                ShapeFrame frame;
                frame.center = o.position;
                frame.forward = o.forward;
                frame.right = o.right;
                frame.up = o.up;
                o.spawner = std::make_shared<UnitSpawner>(read_unit_spawner(node), frame);
                break;
            }
            case type::kGroup:
                // `CRandomGroupDescriptor::CreateObject`; `+0x81`
                // is what `CRandomGroup::setVisible` compares
                // against.
                o.visible = bool_at(node, tl::formats::cmp_field::group::kVisible, true);
                break;
            case type::kParticle:
                // `CParticleTechWrapperDescriptor::CreateObject`:
                // `VISIBLE` is the wrapper's enabled flag
                // (`CParticleTechWrapperDescriptor::Set_setEnabled`).
                o.enabled = bool_at(node, tl::formats::cmp_field::particle::kVisible, true);
                break;
            case type::kColorOverTime:
            case type::kScaleOverTime:
            case type::kTextureRotate:
            case type::kTextureAnimation:
            case type::kGravityWell:
            case type::kVortex:
            case type::kSineForce:
            case type::kLinearForce:
            case type::kAlign:
            case type::kJet:
            case type::kGeometryRotator:
            case type::kPlaneCollision:
            case type::kSphereCollision:
                // `ENABLE` (`CAffectorWrapper +0x119`, set by the constructor):
                // the property after POSITION on
                // every affector.
                o.enabled = bool_at(node, 1, true);
                break;
            case type::kUnitTrigger: {
                // `CUnitTriggerDescriptor::CreateObject`;
                // `CUnitTriggerDescriptor::descriptorSceneActivated`
                // marks each as the layout's: a trigger built here is never `spawned`.
                namespace f = tl::formats::cmp_field::unit_trigger;
                o.position = vec3_at(node, f::kPosition);
                o.enabled = bool_at(node, f::kEnabled, true);
                o.unit = text_at(node, f::kModel);
                // `LOOP STYLE` is authored as its name: `Set_setLoopType` stores
                // the index `GetLoopTypeIDByString` finds for it.
                o.trigger_loop_style = loop_type_id_by_string(text_at(node, f::kLoopStyle));
                o.trigger_loop_authored = node.field(f::kLoopStyle) != nullptr;
                break;
            }
            case type::kMarker: {
                // `CPropertyNodeDescriptor::CreateObject`: a `CPropertyNode`
                // with its constructor's defaults, then the node's properties.
                namespace f = tl::formats::cmp_field::property_node;
                o.position = vec3_at(node, f::kPosition);
                o.enabled = bool_at(node, f::kEnabled, true);
                o.resource = text_at(node, f::kType);
                const auto number = [&node](size_t index) {
                    const tl::formats::CmpField* field = node.field(index);
                    return field != nullptr && field->count() == 1 ? field->as_float() : 0.0f;
                };
                o.marker_radius = number(f::kRadius);
                o.marker_width = number(f::kBoxWidth);
                o.marker_length = number(f::kBoxLength);
                // `TYPE` `Jump Down Area` (6) and the path blockers (`Path Node Occupation Circle`
                // 8, `... Box` 9). Their box is the node's world orientation's columns (identity
                // when not a right-handed unit triple), turned with the room
                // (`CPropertyNode::updateBoundingBox`).
                o.jump_down_area = o.resource == "Jump Down Area";
                if (o.jump_down_area || o.resource == "Path Node Occupation Circle" ||
                    o.resource == "Path Node Occupation Box") {
                    o.forward = vec3_at(node, f::kForward);
                    o.right = vec3_at(node, f::kRight);
                    o.up = vec3_at(node, f::kUp);
                    const auto unit_length = [](const Vec3& v) {
                        const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
                        return 0.99f <= l && l <= 1.01f;
                    };
                    const Vec3& r = o.right;
                    const Vec3& u = o.up;
                    const Vec3 cross{r.y * u.z - r.z * u.y, r.z * u.x - r.x * u.z,
                                     r.x * u.y - r.y * u.x};
                    const float handed =
                        cross.x * o.forward.x + cross.y * o.forward.y + cross.z * o.forward.z;
                    if (!unit_length(o.forward) || !unit_length(o.right) || !unit_length(o.up) ||
                        handed < 0.9f) {
                        o.forward = {0.0f, 0.0f, 1.0f};
                        o.right = {1.0f, 0.0f, 0.0f};
                        o.up = {0.0f, 1.0f, 0.0f};
                    }
                }
                break;
            }
            case type::kMonster: {
                namespace f = tl::formats::cmp_field::monster;
                o.position = vec3_at(node, f::kPosition);
                o.forward = vec3_at(node, f::kForward);
                o.unit = text_at(node, f::kMonster);
                o.targetable = bool_at(node, f::kTargetable, true);
                o.targetable_flag = o.targetable;
                o.invulnerable = bool_at(node, f::kInvincible, false);
                // `CMonsterDescriptor`'s constructor registers
                // the fields; against the compiled indices (FILE 5, INVINCIBLE 9, PATH 10,
                // TARGETABLE 13), ENABLED is 6, PATH LOOPS 11, VISIBLE 12.
                o.enabled = bool_at(node, 6, true);
                // NO TARGET (+0x70c) is registered between ENABLED and FIRE ALL HP
                // EVENTS, which puts it at 7.
                o.no_target = bool_at(node, 7, false);
                o.fire_all_hp_events = bool_at(node, 8, false);
                o.path_loops = bool_at(node, 11, false);
                o.path_name = text_at(node, f::kPath);
                o.visible = bool_at(node, f::kVisible, true);
                o.cannot_wander = bool_at(node, f::kCannotWander, false);
                break;
            }
            case type::kCinematic:
                o.cinematic = text_at(node, tl::formats::cmp_field::cinematic::kCinematic);
                break;
            case type::kWaypointActivator:
                // `CWaypointActivatorDescriptor::CreateObject` with
                // `CWaypointActivator::CWaypointActivator` inline: no property of
                // its own.
                break;
            default: break;
        }

        // Into the level's coordinates (`set_frame`). A path's points are offsets
        // `CPathController::getNextPointAtPercent` adds to its
        // scene's position: moved with the room, not turned. A box trigger's extent stays on the
        // world axes (`CTriggerBox::updateTrigger`).
        if (o.type == type::kMonster &&
            o.forward.x * o.forward.x + o.forward.z * o.forward.z < 1e-8f) {
            o.forward = {0.0f, 0.0f, 1.0f};
        }
        o.local_position = o.position;
        o.position = frame_.point(o.position);
        o.forward = frame_.direction(o.forward);
        o.right = frame_.direction(o.right);
        o.up = frame_.direction(o.up);
        for (Vec3& point : o.points) point = point + frame_.origin;
        // The spawner took its node's frame in file coordinates; it gets the frame just computed,
        // so it spawns and fires from where the room (or skill layout) stands.
        if (o.spawner) {
            ShapeFrame placed = o.spawner->frame();
            placed.center = o.position;
            placed.forward = o.forward;
            placed.right = o.right;
            placed.up = o.up;
            o.spawner->set_frame(placed);
        }

        index_.emplace(o.id, objects_.size());
        objects_.push_back(std::move(o));
    }

    // Every placed monster is a character, created in node order
    // (`CDescriptorManager::loadDescriptorObjects`); its level and hit points are
    // set as it is created.
    for (const Object& o : objects_) {
        if (o.type != type::kMonster || o.unit.empty()) continue;
        const auto preset = preset_creations_.find(o.id);
        make_character(o, o.position, yaw_of(o.forward),
                       preset != preset_creations_.end() ? &preset->second : nullptr);
    }
    preset_creations_.clear();

    // `CTimeline::loadTimelineFromDataGroup`: one
    // `CTimelineProperty` per authored property of each existing target, keyed by object id
    // and property, so `UpdateTimeline` walks id order, then property order.
    for (const tl::formats::CmpTimeline& t : timelines_) {
        const auto it = index_.find(t.node_id);
        if (it == index_.end()) continue;
        Object& o = objects_[it->second];
        o.timeline = &t;
        std::vector<const tl::formats::CmpTimelineTarget*> targets;
        for (const tl::formats::CmpTimelineTarget& target : t.targets) {
            if (index_.count(target.object_id) != 0) targets.push_back(&target);
        }
        std::stable_sort(targets.begin(), targets.end(),
                         [](const auto* a, const auto* b) { return a->object_id < b->object_id; });
        for (const tl::formats::CmpTimelineTarget* target : targets) {
            std::vector<const tl::formats::CmpTimelineTrack*> tracks;
            for (const tl::formats::CmpTimelineTrack& track : target->tracks)
                tracks.push_back(&track);
            std::stable_sort(tracks.begin(), tracks.end(), [](const auto* a, const auto* b) {
                if (a->is_event != b->is_event) return !a->is_event;
                return a->tag < b->tag;
            });
            for (const tl::formats::CmpTimelineTrack* track : tracks) {
                Track added;
                added.target = target->object_id;
                added.data = track;
                // `CTimeline::AddProperty`:
                // the tag indexes the target descriptor's property list.
                Object& driven = objects_[index_.at(target->object_id)];
                added.property = resolve_timeline_property(driven.type, track->tag);
                // A track that turns its target starts from the orientation the
                // node was placed with (absent columns are the constructor's
                // identity).
                const bool turns = added.property.has_value() &&
                                   (added.property->kind == TimelinePropertyKind::Forward ||
                                    added.property->kind == TimelinePropertyKind::Right ||
                                    added.property->kind == TimelinePropertyKind::Up);
                if (turns && !driven.has_local_axes) {
                    driven.has_local_axes = true;
                    const tl::formats::CmpNode* node = scene.node_by_id(driven.id);
                    const std::optional<OrientationFields> fields = orientation_fields(driven.type);
                    if (node != nullptr && fields.has_value()) {
                        const auto column = [node](size_t index, Vec3& into) {
                            const tl::formats::CmpField* f = node->field(index);
                            if (f != nullptr && f->count() == 3) {
                                into = {f->as_float(0), f->as_float(1), f->as_float(2)};
                            }
                        };
                        column(fields->right, driven.local_right);
                        column(fields->up, driven.local_up);
                        column(fields->forward, driven.local_forward);
                    }
                }
                // `CTimeline::loadTimelineFromBinaryFile` hands
                // every key's tag to the property in turn; the last one stays.
                added.interpolation = timeline_property_interpolation(
                    track->keys.empty() ? 0u : track->keys.back().interpolation,
                    added.property ? added.property->value_type : TimelineValueType::Vector3);
                o.tracks.push_back(added);
            }
        }
        // `CTimeline::CTimeline`: time -0.01 (not a percent), remainder 0;
        // `Reset_To_Beginning` then does nothing.
        // `CTimelineDescriptor::loadAdditionalInfo` starts a
        // `START ON LOAD` timeline.
        o.tl_time = -0.01f;
        o.tl_accum = 0.0f;
    }
    for (Object& o : objects_) {
        if (o.type == type::kTimeline) o.tl_playing = o.tl_start_on_load;
    }

    // The wires: `CLogicLink::CLogicLink(scene, from, output, to, input)`
    // through the descriptors' `CLogicWrapper`s (`CLogicWrapper::CLogicWrapper`).
    // `CLogicGroup::loadLogicGroupToDataGroup` makes one `CLogicObject` per member,
    // registered by `CLogicObject::setObjectID`, so an object
    // in two groups is invoked once per group, in group order. `CLogicObject::AddLogicLink`
    // appends links in file order, dropping one the
    // descriptor does not know.
    for (const tl::formats::CmpLogicGroup& group : scene.logic_groups) {
        if (gated.count(group.node_id) != 0) continue;
        if (index_.count(group.node_id) != 0) logic_groups_.push_back(group.node_id);
        for (const tl::formats::CmpLogicObject& member : group.objects) {
            const auto source = index_.find(member.object_id);
            if (source == index_.end()) continue;
            Object& from = objects_[source->second];
            const Vocabulary& from_words = vocabulary(from.type);
            Membership membership;
            for (const tl::formats::CmpLogicLink& link : member.links) {
                const int64_t target = group.resolve(link.to);
                const auto to = index_.find(target);
                if (target == 0 || to == index_.end()) continue;
                const Object& into = objects_[to->second];
                Link resolved;
                resolved.output = from_words.output_by_name(link.output);
                resolved.target = target;
                resolved.input = vocabulary(into.type).input_by_name(link.input);
                if (resolved.output == kNone) {
                    unimplemented_.insert("output '" + link.output + "' on " +
                                          from_words.descriptor);
                    continue;
                }
                if (resolved.input == kNone) {
                    unimplemented_.insert("input '" + link.input + "' on " +
                                          vocabulary(into.type).descriptor);
                    continue;
                }
                membership.links.push_back(resolved);
            }
            from.logic.push_back(std::move(membership));
        }
    }

    build_update_order();
}

// `CDescriptorManager` (adds, `update` and `CEditorScene::setActivated`
// walk): a scene's descriptors in the order its load first asked for each - the game
// never runs `CLayout::createDescriptors` (skips it once the game's flag is set) -
// each one's objects in the order made. The made order is `set_made_order`'s, else the file's.
void LevelScript::build_update_order() {
    std::vector<const Object*> made;
    made.reserve(objects_.size());
    std::set<int64_t> placed;
    for (int64_t id : made_order_) {
        const Object* o = find(id);
        if (o != nullptr && placed.insert(id).second) made.push_back(o);
    }
    for (const Object& o : objects_) {
        if (placed.insert(o.id).second) made.push_back(&o);
    }
    std::vector<uint32_t> order;
    for (const Object* o : made) {
        if (std::find(order.begin(), order.end(), o->type) == order.end()) order.push_back(o->type);
    }
    for (uint32_t t : order) {
        for (const Object* o : made) {
            if (o->type == t) update_order_.push_back(o->id);
        }
    }
    for (int64_t id : update_order_) {
        const Object* o = find(id);
        if (o != nullptr && updates_each_frame(o->type)) frame_order_.push_back(id);
    }
}

// PORT: the level machine's own bookkeeping: an accessor.
LevelScript::Object* LevelScript::find(int64_t id) {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : &objects_[it->second];
}

// PORT: the level machine's own bookkeeping: an accessor.
const LevelScript::Object* LevelScript::find(int64_t id) const {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : &objects_[it->second];
}

// PORT: the descriptors `update`'s switch acts on; the rest it passes over.
bool LevelScript::updates_each_frame(uint32_t type) {
    switch (type) {
        case type::kTimeline:
        case type::kSound:
        case type::kPlayerSphereTrigger:
        case type::kPlayerBoxTrigger:
        case type::kUnitTrigger:
        case type::kTimer:
        case type::kCameraController:
        case type::kAnimationController:
        case type::kSkillController:
        case type::kSkipCutscene:
        case type::kGameStateController:
        case type::kDamageShape: return true;
        default: return false;
    }
}

// PORT: the level machine's own bookkeeping: an accessor.
bool LevelScript::has_wires() const {
    for (const Object& o : objects_) {
        for (const Membership& m : o.logic) {
            if (!m.links.empty()) return true;
        }
    }
    return false;
}

// PORT: the level machine's own bookkeeping: an accessor.
size_t LevelScript::wire_count() const {
    size_t n = 0;
    for (const Object& o : objects_) {
        for (const Membership& m : o.logic) n += m.links.size();
    }
    return n;
}

// PORT: the level's `Unit Trigger` nodes, for the renderer.
std::vector<LevelScript::UnitTriggerView> LevelScript::every_unit_trigger() const {
    std::vector<UnitTriggerView> out;
    for (const Object& o : objects_) {
        if (o.type != type::kUnitTrigger) continue;
        UnitTriggerView view;
        view.id = o.id;
        view.unit = o.unit;
        view.at = o.position;
        view.loop_style = o.trigger_loop_style;
        view.armed = o.trigger_armed;
        view.state = o.trigger_state;
        view.first = o.trigger_first;
        view.last = o.trigger_last;
        out.push_back(view);
    }
    return out;
}

// PORT: one `Unit Trigger` node, for the renderer.
bool LevelScript::unit_trigger(int64_t id, UnitTriggerView& out) const {
    for (const UnitTriggerView& view : every_unit_trigger()) {
        if (view.id != id) continue;
        out = view;
        return true;
    }
    return false;
}

// PORT: this machine's `Unit Trigger` nodes, for the renderer.
std::vector<LevelScript::UnitTriggerView> LevelScript::unit_triggers() const {
    std::vector<UnitTriggerView> out;
    for (const Object& o : objects_) {
        if (o.type != type::kUnitTrigger || !o.enabled) continue;
        UnitTriggerView view;
        view.id = o.id;
        view.unit = o.unit;
        view.at = o.position;
        view.loop_style = o.trigger_loop_style;
        view.armed = o.trigger_armed;
        view.state = o.trigger_state;
        view.first = o.trigger_first;
        view.last = o.trigger_last;
        out.push_back(view);
    }
    return out;
}

void LevelScript::interact_unit_trigger(int64_t id, QuestState& quests,
                                        std::vector<ScriptEvent>& out) {
    (void)quests;
    // `CTriggerUnit::interact`: enabled
    // (vtable +0x48), armed (`+0x1f0`), handed a player; then
    // `trigger(player)`.
    Object* o = find(id);
    if (o == nullptr || o->type != type::kUnitTrigger || !o->enabled) return;
    if (!o->trigger_armed) return;
    trigger_unit(*o, /*by_player=*/true, out);
}

// `CTriggerUnit::trigger(CCharacter*)`. `CItem::interact`'s
// quest event is the renderer's (`ItemInteracted`), as are the portal and waypoint
// branches (ISA 0x2b, 0xad, 0xab).
bool LevelScript::trigger_unit(Object& o, bool by_player, std::vector<ScriptEvent>& out) {
    if (!o.trigger_armed) return false;
    ScriptEvent fired;
    fired.by_player = by_player;
    // `+0x258`: 0 forward; going back (`Back and Forth`) one state down.
    int32_t next = o.trigger_direction == 0 ? o.trigger_state + 1
                                            : o.trigger_state - (o.trigger_direction == 1 ? 1 : 0);
    switch (o.trigger_loop_style) {
        case 1:
            // `Cycle`: past the last state, the first; `selectRandomSkill`.
            if (o.trigger_last < next) next = o.trigger_first;
            fired.trigger_skill = true;
            break;
        case 2:
            // `Back and Forth`: turns at either end.
            if (next < o.trigger_last) {
                if (next <= o.trigger_first) o.trigger_direction = 0;
            } else {
                o.trigger_direction = 1;
            }
            break;
        case 0:
            // `No Loop`: reaching the last state spends it - `+0x1f0` cleared,
            // the `[TREASURE]` and `selectRandomSkill`.
            if (o.trigger_last <= next) {
                o.trigger_armed = false;
                fired.trigger_spent = true;
                fired.trigger_skill = true;
            }
            break;
        default: return false;
    }
    set_trigger_state(o, next, /*notify=*/true, /*force=*/false, /*animate=*/true, out, &fired);
    // `CItem::interact(character)`, whether or not the state moved.
    ScriptEvent interacted;
    interacted.action = ScriptAction::ItemInteracted;
    interacted.node_id = o.id;
    interacted.name = o.name;
    interacted.unit = o.unit;
    interacted.by_player = by_player;
    out.push_back(interacted);
    return true;
}

// `CTriggerUnit::setTriggerState(ETRIGGER_STATES, bool, bool, bool)`.
// The model's animation, the state layouts
// and the state's sound are the renderer's, from the event.
void LevelScript::set_trigger_state(Object& o, int32_t state, bool notify, bool force, bool animate,
                                    std::vector<ScriptEvent>& out, const ScriptEvent* fired) {
    // Unforced, a prop already in that state is left alone - unless it is in
    // its last, which is gone through again.
    if (!force && o.trigger_state != o.trigger_last && o.trigger_state == state) return;
    int32_t clamped = o.trigger_first <= state ? state : o.trigger_first;
    if (o.trigger_last < clamped) clamped = o.trigger_last;
    o.trigger_state = clamped;
    // `+0x25d` = the level's `+0x1a0`.
    if (notify) o.trigger_notify = activated_;
    ScriptEvent e = fired != nullptr ? *fired : ScriptEvent{};
    e.action = ScriptAction::TriggerState;
    e.node_id = o.id;
    e.name = o.name;
    e.unit = o.unit;
    e.on = fired != nullptr;
    e.trigger_state = clamped;
    e.trigger_first = o.trigger_first;
    e.trigger_last = o.trigger_last;
    e.trigger_forced = force;
    e.trigger_animate = animate;
    e.trigger_sound = activated_;
    out.push_back(e);
}

void LevelScript::init_unit_trigger(int64_t id, int32_t max_states, int32_t loop_type) {
    Object* o = find(id);
    if (o == nullptr || o->type != type::kUnitTrigger) return;
    // `unitInit`: `+0x24c` 0, `+0x250 =
    // MAXSTATES - 1`, `+0x254` from `LOOPTYPE` when it names one.
    o->trigger_first = 0;
    o->trigger_last = max_states - 1;
    if (loop_type >= 0 && !o->trigger_loop_authored) o->trigger_loop_style = loop_type;
    // `resetTrigger`, the level not yet
    // active: armed, the first state, nothing broadcast. The renderer puts the
    // prop in it as it places it.
    o->trigger_armed = true;
    int32_t first = o->trigger_first;
    if (o->trigger_last < first) first = o->trigger_last;
    o->trigger_state = first;
    o->trigger_direction = 0;
    o->trigger_notify = false;
}

// PORT: the renderer's view of a `Unit Trigger` written back.
void LevelScript::update_unit_trigger(int64_t id, QuestState& quests,
                                      std::vector<ScriptEvent>& out) {
    Object* o = find(id);
    if (o == nullptr || o->type != type::kUnitTrigger || !o->trigger_notify) return;
    const int32_t state = o->trigger_state;
    broadcast(id, kTriggered, quests, out);
    if (state == 0) {
        broadcast(id, kStateOne, quests, out);
    } else if (state == 1) {
        broadcast(id, kStateTwo, quests, out);
    }
    o = find(id);
    if (o != nullptr) o->trigger_notify = false;
}

// PORT: a `Unit Trigger`'s remembered state put back.
void LevelScript::restore_unit_trigger(int64_t id, int32_t state, std::vector<ScriptEvent>& out) {
    Object* o = find(id);
    if (o == nullptr || o->type != type::kUnitTrigger) return;
    // `setTriggerState(saved, 0, 1, 0)`: forced, not animated (`applySaveState`
    // then sets the state's animation to its last frame).
    set_trigger_state(*o, state, /*notify=*/false, /*force=*/true, /*animate=*/false, out);
    // `restoreLevelState` runs before the level is active (`CLevel +0x1a0`),
    // so the state's sound is not played.
    out.back().trigger_sound = false;
    o->trigger_armed = !(o->trigger_loop_style == 0 && o->trigger_last <= o->trigger_state);
    o->trigger_notify = false;
}

// PORT: a diagnostic count.
size_t LevelScript::trigger_count() const {
    size_t n = 0;
    for (const Object& o : objects_) {
        if (o.type == type::kPlayerSphereTrigger || o.type == type::kPlayerBoxTrigger) ++n;
        // A `Unit Trigger` is something the player acts on, by pad or by
        // walking up to it, so a machine that holds one has work to do.
        if (o.type == type::kUnitTrigger) ++n;
    }
    return n;
}

// PORT: a diagnostic count.
size_t LevelScript::timeline_count() const {
    size_t n = 0;
    for (const Object& o : objects_) {
        if (o.timeline != nullptr) ++n;
    }
    return n;
}

// PORT: the level machine's own bookkeeping: an accessor.
bool LevelScript::timelines_playing() const {
    for (const Object& o : objects_) {
        if (o.timeline != nullptr && o.tl_playing) return true;
    }
    return false;
}

// PORT: the level machine's own bookkeeping: an accessor.
bool LevelScript::busy() const {
    if (!dialogs_.empty() || camera_holder_ != 0) return true;
    return timelines_playing();
}

// PORT: a diagnostic, the node kinds this machine met and does not run.
std::vector<std::string> LevelScript::unimplemented() const {
    return {unimplemented_.begin(), unimplemented_.end()};
}

void LevelScript::note_unhandled(const Object& target, const std::string& what) {
    unimplemented_.insert(what + " on " + vocabulary(target.type).descriptor);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

// `CEditorBaseObject::BroadcastEvent` -> `CDescriptor::` `BroadcastEventFromObject`
// -> `CLogicObject::Invoke`
// -> `CLogicLink::InitiateInputEvent`.
void LevelScript::broadcast(int64_t from, uint32_t output, QuestState& quests,
                            std::vector<ScriptEvent>& out) {
    if (!logic_live_) return;
    // DIVERGENCE: a logic graph is authored by hand and nothing stops one
    // wiring a cycle; the original recurses until the stack gives out. This
    // does not.
    if (depth_ > kMaxLogicDepth) return;

    Object* source = find(from);
    if (source == nullptr) return;

    // The links are copied before any is delivered: a delivery can reach back
    // into this object (a `Timeline` whose `Stopped` stops itself).
    std::vector<Link> fire;
    for (const Membership& m : source->logic) {
        for (const Link& link : m.links) {
            if (link.output == output) fire.push_back(link);
        }
    }
    if (fire.empty()) return;

    ++depth_;
    for (const Link& link : fire) {
        const Object* target = find(link.target);
        if (target == nullptr) continue;
        if (trace_on_) {
            const Object* s = find(from);
            char buffer[512];
            std::snprintf(buffer, sizeof(buffer), "%.3f W %s:%s .%s -> %s:%s .%s",
                          static_cast<double>(clock_), vocabulary(s->type).descriptor,
                          s->name.c_str(), output_name(output), vocabulary(target->type).descriptor,
                          target->name.c_str(), input_name(link.input));
            trace_.push_back(buffer);
        }
        if (!deliver_wires_) continue;
        input(link.target, link.input, quests, out);
    }
    --depth_;
}

void LevelScript::input(int64_t to, uint32_t in, QuestState& quests,
                        std::vector<ScriptEvent>& out) {
    Object* object = find(to);
    if (object == nullptr) return;
    // `objects_` does not grow during a delivery (spawned units join through
    // `add_spawned_unit`, which the renderer calls after `update` returns), so
    // the reference stays good.
    Object& o = *object;

    auto emit_visible = [&](bool on) {
        o.visible = on;
        ScriptEvent e;
        e.action = ScriptAction::SetVisible;
        e.node_id = o.id;
        e.name = o.name;
        e.on = on;
        out.push_back(e);
    };
    auto emit_enabled = [&](bool on) {
        o.enabled = on;
        ScriptEvent e;
        e.action = ScriptAction::SetEnabled;
        e.node_id = o.id;
        e.name = o.name;
        e.on = on;
        out.push_back(e);
    };

    switch (o.type) {
        // `CLogicGroupDescriptor::InputLogicEvent`.
        //
        case type::kLogicGroup:
            if (in >= kGroupStop && in <= kGroupStop + 7) {
                broadcast(o.id, kGroupStopOut + (in - kGroupStop), quests, out);
                return;
            }
            break;

        // `CLogicTimerDescriptor::InputLogicEvent`.
        case type::kTimer:
            if (in == kDisable) return timer_set_enabled(o, false, quests, out);
            if (in == kReset) {
                timer_reset(o);
                return timer_set_enabled(o, true, quests, out);
            }
            if (in == kEnable) return timer_set_enabled(o, true, quests, out);
            break;

        // `CCounterDescriptor::InputLogicEvent`.
        case type::kCounter:
            switch (in) {
                case kEnable: o.enabled = true; return;
                case kDisable: o.enabled = false; return;
                case kReset:
                    // `CCounter::reset`
                    o.enabled = true;
                    o.counter_value = o.counter_start;
                    return;
                case kAdd: return counter_add(o, 1, quests, out);
                case kSubtract: return counter_subtract(o, 1, quests, out);
                default: break;
            }
            break;

        // `COutputIncrementorDescriptor::InputLogicEvent`.
        case 43:
            if (in == kDisable || in == kEnable) {
                // `COutputIncrementor::setEnabled`: the flag,
                // then `Enabled` or `Disabled`.
                o.enabled = in == kEnable;
                broadcast(o.id, o.enabled ? kEnabled : kDisabled, quests, out);
                return;
            }
            if (in == kIncrement) {
                // `COutputIncrementor::increment`: only while
                // looping and enabled. `Incremented`, then the count's own output (the sixth past
                // five); at the maximum the count restarts and a loop is spent.
                if (!(o.incr_forever || 0 < o.incr_loops_left) || !o.enabled) return;
                broadcast(o.id, kIncremented, quests, out);
                ++o.counter_value;
                const int32_t v = o.counter_value;
                broadcast(o.id,
                          1 <= v && v <= 5 ? kFirstIncrement + static_cast<uint32_t>(v - 1)
                                           : kFirstIncrement + 5,
                          quests, out);
                if (o.counter_value == o.incr_max) {
                    if (!o.incr_forever && o.incr_loops_left < 1) return;
                    o.counter_value = 0;
                    --o.incr_loops_left;
                }
                return;
            }
            break;

        // `CTriggerDescriptor::InputLogicEvent`.
        case type::kPlayerSphereTrigger:
        case type::kPlayerBoxTrigger:
            if (in == kDisable) {
                o.enabled = false;
                return;
            }
            if (in == kEnable) {
                o.enabled = true;
                return;
            }
            if (in == kReset) {
                // `CLogicTrigger::TriggerReset`
                //
                o.inside = false;
                o.triggered_once = false;
                o.deactivated_once = false;
                broadcast(o.id, kWasReset, quests, out);
                return;
            }
            break;

        // `CTimelineDescriptor::InputLogicEvent`.
        //
        case type::kTimeline:
            switch (in) {
                case kEnable: o.enabled = true; return;    // +0xa2 cleared
                case kDisable: o.enabled = false; return;  // +0xa2 set; unread on play
                case kTimelinePlay: return timeline_play(o, quests, out);
                case kTimelinePlayBackwards: return timeline_play_backwards(o, quests, out);
                case kTimelineStop: return timeline_stop_to(o, false, quests, out);
                case kTimelineStopToEnd: return timeline_stop_to(o, true, quests, out);
                case kTimelinePause:
                    // `CTimeline::Pause`
                    if (!o.tl_playing) return;
                    o.tl_playing = false;
                    broadcast(o.id, kPaused, quests, out);
                    return;
                case kTimelineReset:
                    // `CTimeline::Reset_To_Beginning`: nothing
                    // when the time is already -0.01 itself.
                    if (o.tl_time == -0.01f) return;
                    timeline_reset_properties(o, -0.01f);
                    timeline_set_percent(o, -0.01f, quests, out);
                    broadcast(o.id, kResetToBeginning, quests, out);
                    return;
                case kTimelineResetToEnd:
                    // `CTimeline::Reset_To_End()`: nothing when the
                    // timeline is at 1.01 already (`+0x98`).
                    if (o.tl_time == 1.01f) return;
                    timeline_reset_properties(o, 1.01f);
                    timeline_set_percent(o, 1.01f, quests, out);
                    broadcast(o.id, kResetToEnd, quests, out);
                    return;
                case kTimelineFastForwardToEnd:
                case kTimelineRewindToStart: {
                    // `CTimeline::fastFowardToEnd` /
                    // `CTimeline::rewindToStart`
                    const bool end = in == kTimelineFastForwardToEnd;
                    o.tl_playing = true;
                    timeline_reset_properties(o, end ? 1.01f : -0.01f);
                    timeline_set_percent(o, end ? 1.01f : -0.01f, quests, out);
                    broadcast(o.id, kStopped, quests, out);
                    broadcast(o.id, end ? kStoppedForwards : kStoppedBackwards, quests, out);
                    o.tl_playing = false;
                    return;
                }
                default: break;
            }
            break;

        // `CLayoutDescriptor::InputLogicEvent`,
        // the timeline link.
        case type::kTimelineLink:
            if (in == kTimelinePlay || in == kTimelinePlayBackwards || in == kTimelineStop ||
                in == kTimelineStopToEnd) {
                ScriptEvent e;
                e.action = ScriptAction::NestedTimeline;
                e.node_id = o.id;
                e.name = o.name;
                e.on = in == kTimelinePlay || in == kTimelinePlayBackwards;
                e.backwards = in == kTimelinePlayBackwards;
                out.push_back(e);
                return;
            }
            // Its `default:` (every input but the link's own cases, `Show` and `Hide` included) is
            // `CLayout::processInputs` first; only an input the layout does not take reaches the
            // link. Plain and particle links always fall through (`CLayout::editorObjectCreated`).
            //
            if (!link_input_declined_ && in != kEnable && in != kDisable && in != kEnableAndShow &&
                in != kDisableAndHide && in != kStartParticle && in != kStopParticle &&
                in != kForceStopParticle && in != kTimelinePause && in != kTimelineResetToEnd) {
                ScriptEvent e;
                e.action = ScriptAction::LinkInput;
                e.node_id = o.id;
                e.name = o.name;
                e.input = in;
                out.push_back(e);
                return;
            }
            [[fallthrough]];
        // `CLayoutDescriptor::InputLogicEvent`:
        // the shared half, and
        // `CPositionableObjectDescriptor::InputLogicEvent`
        // under it.
        case type::kLayoutLink:
        case type::kParticleLink:
            switch (in) {
                case kEnable: emit_enabled(true); return;
                case kDisable: emit_enabled(false); return;
                case kEnableAndShow:
                    emit_enabled(true);
                    emit_visible(true);
                    return;
                case kDisableAndHide:
                    emit_enabled(false);
                    emit_visible(false);
                    return;
                case kStartParticle:
                    // `CLayout::start`, then `Particle Started`.
                    emit_visible(true);
                    broadcast(o.id, kParticleStarted, quests, out);
                    return;
                case kStopParticle:
                case kForceStopParticle:
                    emit_visible(false);
                    broadcast(o.id, kParticleStopped, quests, out);
                    return;
                case kShow: emit_visible(true); return;
                case kHide: emit_visible(false); return;
                case kTimelinePause:
                    // `CLayout::pause`: `callFunctionOnObjects(2)`,
                    // whose particle half is `CParticle::Pause`,
                    // then `Particle Paused`. A plain link's timelines are not paused
                    // here.
                    if (o.type == type::kParticleLink) {
                        ScriptEvent e;
                        e.action = ScriptAction::ParticleLinkPause;
                        e.node_id = o.id;
                        e.name = o.name;
                        out.push_back(e);
                        broadcast(o.id, kParticlePaused, quests, out);
                    }
                    return;
                default: break;
            }
            break;

        // `CParticleTechWrapperDescriptor::InputLogicEvent`: the node's own system,
        // where its file is part of the level; a particle link's nodes are not reachable.
        case type::kParticle: {
            const int64_t id = o.id;
            const auto run = [&](bool on) {
                ScriptEvent e;
                e.action = ScriptAction::ParticleNode;
                e.node_id = id;
                e.name = o.name;
                e.on = on;
                out.push_back(e);
            };
            // `CParticleTechWrapper::start`:
            // a wrapper that is not enabled is stopped instead;
            // `CParticleWrapper::start` under
            // it raises `Particle Started` itself.
            const auto start = [&]() {
                if (!o.enabled) return run(false);
                run(true);
                broadcast(id, kParticleStarted, quests, out);
            };
            if (in == kStartParticle) {
                if (!o.enabled) {
                    // `CParticleTechWrapper::setActivated(true)`:
                    // shown, enabled, started.
                    emit_visible(true);
                    o.enabled = true;
                    start();
                }
                start();
                broadcast(id, kParticleStarted, quests, out);
                return;
            }
            if (in == kStopParticle) {
                // `CParticleTechWrapper::stop`. The final
                // `Particle Stopped` (`CParticleWrapper::stop`) is not carried.
                run(false);
                broadcast(id, kParticleStopped, quests, out);
                return;
            }
            if (in == kShow || in == kHide) return emit_visible(in == kShow);
            break;
        }

        // `CAffectorDescriptor::InputLogicEvent`: `Enable` and
        // `Disable` are `CAffectorWrapper::setEnabled`.
        case type::kColorOverTime:
        case type::kScaleOverTime:
        case type::kTextureRotate:
        case type::kTextureAnimation:
        case type::kGravityWell:
        case type::kVortex:
        case type::kSineForce:
        case type::kLinearForce:
        case type::kAlign:
        case type::kJet:
        case type::kGeometryRotator:
        case type::kPlaneCollision:
        case type::kSphereCollision:
            if (in == kEnable || in == kDisable) return affector_set_enabled(o, in == kEnable, out);
            if (in == kShow || in == kHide) return emit_visible(in == kShow);
            break;

        // `CParticleEmitterWrapperDescriptor::InputLogicEvent`:
        // `Enable` shows and enables the wrapper, `Disable` disables it
        // (`CParticleEmitterWrapper::setEnabled`); every input then goes on to
        // `CPositionableObjectDescriptor::InputLogicEvent` (whose `Show`/`Hide` draw nothing).
        case type::kEmitter:
            if (in == kEnable || in == kDisable) {
                if (in == kEnable) o.visible = true;
                o.enabled = in == kEnable;
                ScriptEvent e;
                e.action = ScriptAction::EmitterEnabled;
                e.node_id = o.id;
                e.name = o.name;
                e.on = in == kEnable;
                out.push_back(e);
                return;
            }
            if (in == kShow || in == kHide) {
                o.visible = in == kShow;
                return;
            }
            break;

        // `CPositionableObjectDescriptor::InputLogicEvent` and
        // the descriptors that only forward to it. `CRandomGroupDescriptor::InputLogicEvent`:
        // `Show`/`Hide` are `CRandomGroup::setVisible`.
        case type::kGroup:
        case type::kModel:
        case type::kRoomPiece:
        case type::kLight:
            if (in == kShow || in == kHide) return emit_visible(in == kShow);
            break;

        // `CPropertyNodeDescriptor::InputLogicEvent` ->
        // `CPropertyNode::setEnabled`.
        case type::kMarker:
            if (in == kEnable || in == kDisable) {
                property_node_set_enabled(o, in == kEnable, quests, out);
                return;
            }
            if (in == kShow || in == kHide) return emit_visible(in == kShow);
            break;

        // `CQuestControllerDescriptor::InputLogicEvent`.
        //
        case type::kQuestController:
            switch (in) {
                case kInteract: return quest_interact(o, quests, out);
                case kForceAccept: {
                    // `CQuestController::setQuestAccepted(true)`
                    //
                    if (o.quest.empty()) return;
                    const bool was_active = quests.status(o.quest) == QuestStatus::Active ||
                                            quests.status(o.quest) == QuestStatus::Complete;
                    if (was_active) return;
                    quests.activate(o.quest);
                    ScriptEvent e;
                    e.action = ScriptAction::QuestChanged;
                    e.node_id = o.id;
                    e.name = o.name;
                    e.text = o.quest;
                    e.status = QuestStatus::Active;
                    out.push_back(e);
                    // `giveQuest`'s walk of the quest's controllers, every
                    // machine of the level's (`questHasBeenAccepted(true)`).
                    const QuestNotice given{quest_upper(o.quest), {kQuestActive}};
                    for (LevelScript* machine : level_machines()) {
                        machine->quest_notice(given, quests, sink_for(machine, out));
                    }
                    return;
                }
                case kForceComplete: {
                    // `CQuestController::setQuestComplete(true)`
                    //
                    if (o.quest.empty()) return;
                    if (quests.status(o.quest) == QuestStatus::Complete) return;
                    // A quest not accepted is given first (`giveQuest(quest,
                    // null, false)`), and its controllers hear that before the
                    // completion.
                    const bool accepted = quests.status(o.quest) == QuestStatus::Active;
                    quests.complete(o.quest);
                    ScriptEvent e;
                    e.action = ScriptAction::QuestChanged;
                    e.node_id = o.id;
                    e.name = o.name;
                    e.text = o.quest;
                    e.status = QuestStatus::Complete;
                    out.push_back(e);
                    if (!accepted) {
                        const QuestNotice given{quest_upper(o.quest), {kQuestActive}};
                        for (LevelScript* machine : level_machines()) {
                            machine->quest_notice(given, quests, sink_for(machine, out));
                        }
                    }
                    // `completeQuest`'s walk (`questHasBeenCompleted(true)`).
                    const QuestNotice done{quest_upper(o.quest), {kQuestComplete}};
                    for (LevelScript* machine : level_machines()) {
                        machine->quest_notice(done, quests, sink_for(machine, out));
                    }
                    return;
                }
                case kForceNotAccepted: {
                    // `CQuestController::setQuestAccepted(false)`:
                    // an accepted quest only, `CQuestManager::removeQuest(quest, false)`:
                    // off the held list, no `cleanUp`, Quest Not
                    // Active to every controller.
                    if (o.quest.empty() || quests.status(o.quest) != QuestStatus::Active) return;
                    quests.set(o.quest, QuestStatus::Unknown);
                    ScriptEvent e;
                    e.action = ScriptAction::QuestChanged;
                    e.node_id = o.id;
                    e.name = o.name;
                    e.text = o.quest;
                    e.status = QuestStatus::Unknown;
                    out.push_back(e);
                    const QuestNotice removed{quest_upper(o.quest), {kQuestNotActive}};
                    for (LevelScript* machine : level_machines()) {
                        machine->quest_notice(removed, quests, sink_for(machine, out));
                    }
                    return;
                }
                case kForceNotComplete: {
                    // `CQuestController::setQuestComplete(false)`:
                    // `setIsComplete(false)`, then `CQuestManager::resetQuest`
                    // whatever the state: off the list, not complete,
                    // Quest Not Complete to all.
                    if (o.quest.empty()) return;
                    quests.set(o.quest, QuestStatus::Unknown);
                    ScriptEvent e;
                    e.action = ScriptAction::QuestChanged;
                    e.node_id = o.id;
                    e.name = o.name;
                    e.text = o.quest;
                    e.status = QuestStatus::Unknown;
                    out.push_back(e);
                    const QuestNotice reset{quest_upper(o.quest), {kQuestNotComplete}};
                    for (LevelScript* machine : level_machines()) {
                        machine->quest_notice(reset, quests, sink_for(machine, out));
                    }
                    return;
                }
                default: break;
            }
            break;

        // `CCameraControllerDescriptor::InputLogicEvent`.
        //
        case type::kCameraController:
            if (in == kStartCamera) {
                camera_start(o, quests, out);
                camera_update(o, 0.0f, quests, out);
                return;
            }
            if (in == kCameraOff) return camera_stop(o, quests, out);
            break;

        // `CGameStateControllerDescriptor::InputLogicEvent`.
        //
        case type::kGameStateController:
            if (in == kEnable || in == kDisable) {
                const bool on = in == kEnable;
                if (o.enabled == on) return;
                o.enabled = on;
                return game_state_perform(o, quests, out);
            }
            if (in == kWarpPetsToPlayer) return warp_pets_to_player();
            if (in == kHealPlayer) {
                // `CGameStateController::healPlayer`.
                //
                ScriptEvent e;
                e.action = ScriptAction::HealPlayer;
                e.node_id = o.id;
                e.name = o.name;
                out.push_back(e);
                return;
            }
            if (in == kShowTip) {
                // `CGameStateController::showHelp`: nothing without
                // a `TIP`.
                if (o.gs_tip == -1) return;
                ScriptEvent e;
                e.action = ScriptAction::ShowTip;
                e.node_id = o.id;
                e.name = o.name;
                e.input = static_cast<uint32_t>(o.gs_tip);
                out.push_back(e);
                return;
            }
            break;

        // `CSkipCutsceneDescriptor::InputLogicEvent` ->
        // `CSkipCutscene::setEnabled`.
        case type::kSkipCutscene:
            if (in == kEnable || in == kDisable) {
                o.enabled = in == kEnable;
                broadcast(o.id, o.enabled ? kEnabled : kDisabled, quests, out);
                if (o.enabled) o.skip_pending = false;
                return;
            }
            break;

        // `CAnimationPlayerDescriptor::InputLogicEvent`.
        //
        case type::kAnimationController:
            switch (in) {
                case kAnimationPlay:
                case kAnimationPlayLooping:
                    animation_play(o, in == kAnimationPlayLooping, quests, out);
                    broadcast(o.id, kAnimationPlaying, quests, out);
                    return;
                case kAnimationStop: return animation_stop(o, false, quests, out);
                case kAnimationStopAndIdle: return animation_stop(o, true, quests, out);
                default: break;
            }
            break;

        // `CSkillControllerDescriptor::InputLogicEvent`.
        //
        case type::kSkillController:
            switch (in) {
                case kStartSkill:
                    // `CSkillController::startSkill`.
                    if (skill_configure(o) && !o.sk_skill.empty()) {
                        o.sk_starting = true;
                        o.sk_stopping = false;
                    }
                    return;
                case kStopSkill:
                    // `CSkillController::stopSkill`.
                    if (skill_configure(o) && !o.sk_skill.empty()) {
                        o.sk_starting = false;
                        o.sk_stopping = true;
                    }
                    return;
                case kLearnSkill: return skill_learn(o, quests, out);
                case kUnlearnSkill: return skill_unlearn(o, quests, out);
                default: break;
            }
            break;

        // `CPathControllerDescriptor::InputLogicEvent`
        // -> `CPathController::setEnabled`.
        //
        case type::kPathing:
            if (in == kEnable || in == kDisable)
                return path_set_enabled(o, in == kEnable, quests, out);
            break;

        // `CMusicObjectDescriptor::InputLogicEvent`: `Play` (9,
        // `CMusicObject::play`), `Stop` (10), `Play Level Music` (13), on the renderer's
        // `MusicObject`.
        case type::kMusic:
            if (in == kPlay || in == kPlayLevelMusic || in == kStop) {
                ScriptEvent e;
                e.action = ScriptAction::Music;
                e.node_id = o.id;
                e.name = o.name;
                e.input = in;
                e.on = in != kStop;
                e.text = o.resource;
                e.loops = o.loops;
                out.push_back(e);
                return;
            }
            break;

        // `CTeleportDescriptor::InputLogicEvent`: `Activate
        // Teleport` (0x26) is `CTeleport::activate(getPosition(true), getForward())`:
        // moves the player, output 8. `Enable`/`Disable`
        // are `CTeleport::setEnabled`, which `activate` does not read.
        // `CDungeonObjectDescriptor::InputLogicEvent`: `Clear
        // History` (0x51), `CDungeonObject::clearHistory`:
        // `CPlayer::clearDungeonHistory(+0x100)`, then `updateStoredLevels`.
        case type::kDungeonObject:
            if (in == kClearHistory) {
                ScriptEvent e;
                e.action = ScriptAction::ClearDungeonHistory;
                e.node_id = o.id;
                e.name = o.dungeon_name;
                out.push_back(e);
            }
            return;

        case type::kTeleport:
            if (in == kActivateTeleport) {
                ScriptEvent e;
                e.action = ScriptAction::TeleportPlayer;
                e.node_id = o.id;
                e.name = o.name;
                e.position = o.position;
                e.facing = std::atan2(o.forward.x, o.forward.z);
                out.push_back(e);
                broadcast(o.id, kActivated, quests, out);
                return;
            }
            if (in == kEnable || in == kDisable) {
                o.enabled = in == kEnable;
                return;
            }
            break;

        // `CCameraShakeDescriptor::InputLogicEvent`: `Activate`
        // (0xf) is `startCameraShake(getPosition(true), false)` about its
        // layout; nothing else.
        case type::kCameraShake:
            if (in == kActivate && o.source != nullptr) {
                ScriptEvent e;
                e.action = ScriptAction::CameraShake;
                e.node_id = o.id;
                e.name = o.name;
                e.node = o.source;
                e.position = frame_.origin;
                out.push_back(e);
                return;
            }
            break;

        // `CSoundObjectDescriptor::InputLogicEvent`.
        //
        case type::kSound:
            if (in == kPlay || in == kStop || in == kPause || in == kResume) {
                resolve_sound_slot(o);
                o.sound.at = o.position;
                sound_object_input(o.sound, in, player_at_, sound_host_, volatile_random(),
                                   sound_output(o, quests, out));
                return;
            }
            break;

        // `CUnitSpawnerDescriptor::InputLogicEvent`.
        //
        case type::kUnitSpawner:
            if (in == kSpawnUnits || in == kStop || in == kDestroySpawnedUnits ||
                in == kHideAndDisableSpawnedUnits || in == kIncrementLevelDelta ||
                in == kDecrementLevelDelta) {
                spawner_input(o, in, out);
                return;
            }
            break;

        // `CUnitTriggerDescriptor::InputLogicEvent`.
        //
        case type::kUnitTrigger:
            switch (in) {
                case kEnable:
                case kDisable:
                    o.enabled = in == kEnable;
                    broadcast(o.id, o.enabled ? kEnabled : kDisabled, quests, out);
                    return;
                // 0x2e: `trigger(nullptr)` - a wire hands it no character. A
                // spent one-shot (`+0x1f0` clear) does nothing.
                case kTrigger: trigger_unit(o, /*by_player=*/false, out); return;
                // 0x1e / 0x1f: `setTriggerState(0 or 1, 1, 0, 1)`, without `trigger`'s treasure or
                // trap; `update` then broadcasts `Triggered`.
                case kSetStateOne:
                case kSetStateTwo:
                    set_trigger_state(o, in == kSetStateOne ? 0 : 1, /*notify=*/true,
                                      /*force=*/false, /*animate=*/true, out);
                    return;
                // 6: `CTriggerUnit::resetTrigger`: armed again,
                // first state forced and animated, `Was Reset` while the level is active.
                case kReset:
                    o.trigger_armed = true;
                    set_trigger_state(o, o.trigger_first, /*notify=*/false, /*force=*/true,
                                      /*animate=*/true, out);
                    if (activated_) broadcast(o.id, kWasReset, quests, out);
                    return;
                // 0x4d / 0x4e: the unit's `+0x18d` and `addToAvoidanceMap` /
                // `removeFromAvoidanceMap` (vtable +0x270 / +0x268).
                case kCollidable:
                case kNotCollidable: {
                    ScriptEvent e;
                    e.action = ScriptAction::TriggerCollidable;
                    e.node_id = o.id;
                    e.name = o.name;
                    e.unit = o.unit;
                    e.on = in == kCollidable;
                    out.push_back(e);
                    return;
                }
                default: break;
            }
            break;

        // `CWarperDescriptor::InputLogicEvent`:
        // `CWarper::activate`, which does not test enabled.
        case type::kWarper:
            if (in == kActivateWarper) {
                ScriptEvent e;
                e.action = ScriptAction::Warp;
                e.node_id = o.id;
                e.name = o.name;
                e.text = o.dungeon;
                e.level_delta = o.level_delta;
                e.level_absolute = o.level_absolute;
                e.waypoint = o.waypoint;
                e.warp_name = o.warp_name;
                out.push_back(e);
                return;
            }
            if (in == kShow || in == kHide) return emit_visible(in == kShow);
            if (in == kEnable || in == kDisable) {
                o.enabled = in == kEnable;
                return;
            }
            break;

        // `CCinematicDescriptor::InputLogicEvent`:
        // `Play` (9) is `CCinematicObject::play`, inline there.
        case type::kCinematic:
            if (in == kPlay) return cinematic_play(o, out);
            break;

        // `CWaypointActivatorDescriptor::InputLogicEvent`: 0xf
        // calls `CWaypointActivator::activate`: every player on
        // the collision list adds its level's depth as a waypoint.
        case type::kWaypointActivator:
            if (in == kActivate) {
                ScriptEvent e;
                e.action = ScriptAction::WaypointActivated;
                e.node_id = o.id;
                e.name = o.name;
                out.push_back(e);
                return;
            }
            break;

        // `CMoneyTakerDescriptor::InputLogicEvent`: `Take Money` only,
        // `CMoneyTaker::takeMoney()`: `Insufficient funds`, or pay and `Money Taken`.
        case type::kMoneyTaker:
            if (in == kTakeMoney && player_gold_.held && player_gold_.give) {
                if (player_gold_.held() < o.money_amount) {
                    broadcast(o.id, 0x74, quests, out);
                    return;
                }
                player_gold_.give(-o.money_amount);
                broadcast(o.id, 0x75, quests, out);
            }
            return;

        // `CRandomChoiceDescriptor::InputLogicEvent`
        // -> `CRandomChoice::roll`.
        //
        case 39:
            if (in == kRoll) {
                if (!o.enabled) return;
                if (o.weights.size() != 5) return;
                // `TYPE` 1 `RANDOM`: each positive weight a percent chance on `g_RandVolatile`, in
                // order. Otherwise `WEIGHT`: `COUNT` picks with replacement through a `CRandomizer`
                // on `g_Rand`.
                if (o.choice_mode == 1) {
                    for (size_t i = 0; i < 5; ++i) {
                        if (o.weights[i] > 0) {
                            const int32_t roll = volatile_random().integer_between(1, 100);
                            if (roll <= o.weights[i]) {
                                broadcast(o.id, kOne + static_cast<uint32_t>(i), quests, out);
                            }
                        }
                    }
                    return;
                }
                Randomizer randomizer(RandomizerType::kNormal);
                for (size_t i = 0; i < 5; ++i) {
                    if (o.weights[i] > 0) {
                        randomizer.add_choice(static_cast<int32_t>(i), o.weights[i]);
                    }
                }
                if (randomizer.choice_count() > 0) {
                    // DIVERGENCE: the original compares the loop against `COUNT`
                    // unsigned, so a negative `COUNT` would fire ~4e9
                    // times; here it fires none. `COUNT` defaults to 1.
                    for (int32_t drawn = 0; drawn < o.choice_count; ++drawn) {
                        const int32_t pick = randomizer.get_random(game_random());
                        broadcast(o.id, kOne + static_cast<uint32_t>(pick), quests, out);
                    }
                }
                return;
            }
            break;

        // `CPuzzleRandomizerDescriptor::InputLogicEvent`.
        case 46:
            switch (in) {
                case kEnable:
                case kDisable: o.enabled = in == kEnable; return;
                case kReset:
                    // `reset()` alone keeps the order (a failure does not reshuffle);
                    // the `Reset` input then reshuffles: 5 * n volatile draws.
                    puzzle_reset(o);
                    puzzle_set_active_inputs(o, o.puzzle_active_inputs);
                    return;
                case kPuzzleInput1:
                case kPuzzleInput1 + 1:
                case kPuzzleInput1 + 2:
                case kPuzzleInput1 + 3:
                case kPuzzleInput1 + 4:
                case kPuzzleInput1 + 5: puzzle_trigger(o, in - kPuzzleInput1); return;
                default: break;
            }
            break;

        // `CMonsterDescriptor::InputLogicEvent`.
        //
        case type::kMonster:
            switch (in) {
                case kShow:
                case kHide:
                    // `CCharacter::setMeshVisible(v, false)` through the fade, then `On Visible` /
                    // `On Invisible`; with no character yet, only marked. `Hide` first runs
                    // `CCharacter::stopParticles(true)`.
                    //
                    if (in == kHide && character(o.id) != nullptr) {
                        ScriptEvent stop;
                        stop.action = ScriptAction::StopUnitParticles;
                        stop.node_id = o.id;
                        stop.name = o.name;
                        stop.on = true;
                        out.push_back(stop);
                    }
                    if (!set_unit_hidden(o.id, in == kHide, false, out)) emit_visible(in == kShow);
                    broadcast(o.id, in == kShow ? kOnVisible : kOnInvisible, quests, out);
                    return;
                case kEnable:
                case kDisable:
                    // `CSceneNodeObject::setEnabled`.
                    emit_enabled(in == kEnable);
                    broadcast(o.id, in == kEnable ? kEnabled : kDisabled, quests, out);
                    return;
                case kMakeInvulnerable:
                case kMakeVulnerable:
                    o.invulnerable = in == kMakeInvulnerable;
                    broadcast(o.id, o.invulnerable ? kInvulnerable : kVulnerable, quests, out);
                    return;
                case kAlertMonster:
                case kEnableTargetingAndAlert: {
                    // `CMonsterDescriptor::InputLogicEvent`: 0x40
                    // clears +0x70c first; both add `MONSTER_HUNT`, `NO LINE OF SIGHT` 20 s,
                    // `setTarget(player)`.
                    o.no_target = false;
                    if (Character* c = character(o.id)) {
                        c->hunt_affix = true;
                        c->ai_flags.add(AIFlag::NoLineOfSight, 20.0f);
                        set_target(o, *c, true, UnitRef{}, out);
                    }
                    return;
                }
                case kDisableTargeting:
                    if (Character* c = character(o.id)) {
                        set_target(o, *c, false, UnitRef{}, out);
                        c->hunt_affix = false;
                    }
                    o.no_target = true;
                    return;
                case kEnableTargeting: o.no_target = false; return;
                case kHunt:
                    // `setAIState(4)` and nothing else.
                    set_ai_state(o, ai_state::kHunt, quests, out);
                    return;
                case kCannotBeTargeted:
                    o.targetable = false;
                    o.targetable_flag = false;
                    return;
                case kCanBeTargeted:
                    o.targetable = true;
                    o.targetable_flag = true;
                    return;
                case kKillMonster: {
                    // `modifyHP(-HPFloat())` (`CMonsterDescriptor::InputLogicEvent`,
                    // call), with `modifyHP`'s HP-threshold broadcast
                    // under the same gates as in `damage_unit`.
                    if (dead_.count(o.id) != 0) return;
                    if (Character* c = character(o.id)) {
                        die_now(o, *c, quests, out);
                        return;
                    }
                    ScriptEvent e;
                    e.action = ScriptAction::Kill;
                    e.node_id = o.id;
                    e.name = o.name;
                    e.unit = o.unit;
                    out.push_back(e);
                    unit_killed(o.id, quests, out);
                    return;
                }
                case kInteract: monster_interact(o, quests, out); return;
                case kAddAsPet:
                case kRemoveAsPet: {
                    // `addPet(player, this)` / `removePet(player, this)`: the
                    // player is the client's first (`+0x68` -> `+0x28`, `+0x58`).
                    // The scene keeps the player's pet list and its limit.
                    if (character(o.id) == nullptr) return;
                    ScriptEvent e;
                    e.action = ScriptAction::PlayerPet;
                    e.node_id = o.id;
                    e.name = o.name;
                    e.unit = o.unit;
                    e.on = in == kAddAsPet;
                    out.push_back(e);
                    return;
                }
                case kStopSkills:
                    // `CSkillManager::stopAllSkills(true, false, false)`.
                    //
                    if (character(o.id) != nullptr && unit_skills_.stop_all) {
                        unit_skills_.stop_all(*this, o.id, /*force=*/true);
                    }
                    return;
                case kKillPets: kill_pets(o, quests, out); return;
                default: break;
            }
            break;

        // `CDamageShapeDescriptor::InputLogicEvent`: 2 on, 3 off.
        case type::kDamageShape:
            if (in == 0x02 || in == 0x03) {
                damage_shape_set_enabled(o, in == 0x02, quests, out);
                return;
            }
            break;

        default: break;
    }

    note_unhandled(o, input_name(in));
}

// ---------------------------------------------------------------------------
// Timer (`CLogicTimer`)
// ---------------------------------------------------------------------------

// `CLogicTimer::resetTimer` (on the console inlined in
// `setEnabled`, and in the descriptor's `Reset`).
//
void LevelScript::timer_reset(Object& o) {
    if (o.timer_max <= o.timer_time) {
        o.timer_left = o.timer_time;
        return;
    }
    o.timer_left = volatile_random().between(o.timer_time, o.timer_max);
}

// `CLogicTimer::setEnabled`.
void LevelScript::timer_set_enabled(Object& o, bool on, QuestState& quests,
                                    std::vector<ScriptEvent>& out) {
    o.enabled = on;
    timer_reset(o);
    if (!o.enabled) {
        broadcast(o.id, kDisabled, quests, out);
        return;
    }
    broadcast(o.id, kEnabled, quests, out);
    if (o.timer_loops_left < 1) o.timer_loops_left = 1;
}

// `CLogicTimer::update`.
void LevelScript::timer_update(Object& o, float delta, QuestState& quests,
                               std::vector<ScriptEvent>& out) {
    if (!(o.timer_forever || o.timer_loops_left > 0) || !o.enabled) return;
    o.timer_left -= delta;
    if (!(o.timer_left <= 0.0f)) return;
    broadcast(o.id, kActivated, quests, out);
    if (o.timer_forever) {
        timer_reset(o);
        return;
    }
    --o.timer_loops_left;
    if (o.timer_loops_left > 0) timer_reset(o);
}

// ---------------------------------------------------------------------------
// Counter (`CCounter`)
// ---------------------------------------------------------------------------

// `CCounter::add` (inlined into
// `CCounterDescriptor::InputLogicEvent`).
void LevelScript::counter_add(Object& o, int32_t n, QuestState& quests,
                              std::vector<ScriptEvent>& out) {
    if (!o.enabled) return;
    o.counter_value += n;
    if (o.counter_type == 2 || o.counter_type == 3) broadcast(o.id, kActivated, quests, out);
    counter_update(o, quests, out);
}

// `CCounter::subtract` (inlined into
// `CCounterDescriptor::InputLogicEvent`).
void LevelScript::counter_subtract(Object& o, int32_t n, QuestState& quests,
                                   std::vector<ScriptEvent>& out) {
    if (!o.enabled) return;
    o.counter_value -= n;
    if (o.counter_type == 4 || o.counter_type == 5) broadcast(o.id, kActivated, quests, out);
    counter_update(o, quests, out);
}

// `CCounter::updateCounter`.
void LevelScript::counter_update(Object& o, QuestState& quests, std::vector<ScriptEvent>& out) {
    if (!o.enabled || o.counter_value != o.counter_equals) return;
    switch (o.counter_type) {
        case 0:
            broadcast(o.id, kActivated, quests, out);
            o.enabled = false;
            break;
        case 1:
            broadcast(o.id, kActivated, quests, out);
            o.counter_value = o.counter_start;
            break;
        case 2:
        case 4: o.enabled = false; break;
        case 3:
        case 5: o.counter_value = o.counter_start; break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// Triggers (`CLogicTrigger`)
// ---------------------------------------------------------------------------

// `CTriggerSphere::updateTrigger`,
// `CTriggerBox::updateTrigger`, `CLogicTrigger::TriggerActivated`
// and `CLogicTrigger::TriggerDeactivated`.
void LevelScript::trigger_update(Object& o, const Vec3& player, QuestState& quests,
                                 std::vector<ScriptEvent>& out) {
    if (!o.enabled) return;
    bool inside = false;
    const Vec3 d = player - o.position;
    if (o.type == type::kPlayerSphereTrigger) {
        inside = !(o.radius < std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z));
    } else if (o.type == type::kPlayerBoxTrigger) {
        inside = std::fabs(d.x) <= o.half_extent.x && std::fabs(d.y) <= o.half_extent.y &&
                 std::fabs(d.z) <= o.half_extent.z;
    } else {
        // DIVERGENCE: a `Unit Trigger` has no reach of its own; see
        // enable_proximity_unit_triggers().
        constexpr float kUnitTriggerReach = 2.0f;
        inside = d.x * d.x + d.y * d.y + d.z * d.z <= kUnitTriggerReach * kUnitTriggerReach;
    }

    if (inside) {
        if (o.inside) return;
        o.inside = true;
        if (o.type == type::kUnitTrigger) {
            // The player walking up stands in for the player's `interact`.
            interact_unit_trigger(o.id, quests, out);
            return;
        }
        if (o.triggered_once) {
            broadcast(o.id, kTriggered, quests, out);
            return;
        }
        broadcast(o.id, kTriggeredFirstTime, quests, out);
        o.triggered_once = true;
        broadcast(o.id, kTriggered, quests, out);
        return;
    }
    if (!o.inside) return;
    o.inside = false;
    if (o.type == type::kUnitTrigger) return;
    if (!o.deactivated_once) {
        broadcast(o.id, kDeactivatedFirstTime, quests, out);
        o.deactivated_once = true;
    }
    broadcast(o.id, kDeactivated, quests, out);
}

// ---------------------------------------------------------------------------
// Timeline (`CTimeline`, `CTimelineProperty`)
// ---------------------------------------------------------------------------

// `CTimeline::resetPropertiesToPercent`.
void LevelScript::timeline_reset_properties(Object& o, float percent) {
    for (Track& track : o.tracks) track.last = percent;
}

// `CTimeline::SetTimelinePercentDone`.
void LevelScript::timeline_set_percent(Object& o, float percent, QuestState& quests,
                                       std::vector<ScriptEvent>& out) {
    percent = percent < 1.0f ? percent : 1.0f;  // `minss`: a NaN is 1
    o.tl_time = o.tl_duration * percent;
    o.tl_accum = 0.0f;
    if (0.0f < o.tl_step) {
        const float steps = o.tl_duration * percent / o.tl_step;
        const float whole = static_cast<float>(tl::formats::x86_float_to_int32(steps));
        o.tl_time = o.tl_step * whole;
        o.tl_accum = steps - whole;
    }
    if (!o.tl_playing) return;
    for (Track& track : o.tracks) track_update(o, track, percent, o.tl_backwards, quests, out);
}

// `CTimeline::Play`.
void LevelScript::timeline_play(Object& o, QuestState& quests, std::vector<ScriptEvent>& out) {
    if (o.tl_playing && !o.tl_backwards) return;
    o.tl_backwards = false;
    o.tl_playing = true;
    timeline_reset_properties(o, -0.01f);
    timeline_set_percent(o, -0.01f, quests, out);
    broadcast(o.id, kStarted, quests, out);
    broadcast(o.id, kStartedForwards, quests, out);
}

// `CTimeline::Play_Backwards`.
void LevelScript::timeline_play_backwards(Object& o, QuestState& quests,
                                          std::vector<ScriptEvent>& out) {
    if (o.tl_playing && o.tl_backwards) return;
    o.tl_backwards = true;
    o.tl_playing = true;
    timeline_reset_properties(o, 1.01f);
    timeline_set_percent(o, 1.01f, quests, out);
    broadcast(o.id, kStarted, quests, out);
    broadcast(o.id, kStartedBackwards, quests, out);
}

// `CTimeline::StopToBeginning` and
// `CTimeline::StopToEnd`. The
// properties are set while still marked playing, which is what applies them.
void LevelScript::timeline_stop_to(Object& o, bool end, QuestState& quests,
                                   std::vector<ScriptEvent>& out) {
    if (!o.tl_playing) return;
    const float percent = end ? 1.01f : -0.01f;
    timeline_reset_properties(o, percent);
    timeline_set_percent(o, percent, quests, out);
    o.tl_playing = false;
    broadcast(o.id, kStopped, quests, out);
    broadcast(o.id, o.tl_backwards ? kStoppedBackwards : kStoppedForwards, quests, out);
}

// `CTimeline::UpdateTimeline`.
void LevelScript::timeline_update(Object& o, float delta, QuestState& quests,
                                  std::vector<ScriptEvent>& out) {
    if (!o.tl_loops) {
        if (!o.tl_backwards ? o.tl_duration <= o.tl_time : o.tl_time <= 0.0f) {
            o.tl_playing = false;
            return;
        }
    }
    if (!o.tl_playing) return;

    float step = o.tl_backwards ? -delta : delta;
    if (0.0f < o.tl_step) {
        o.tl_accum += step;
        if (!(o.tl_accum < o.tl_step)) {
            o.tl_time += o.tl_step;
            o.tl_accum -= o.tl_step;
        }
    } else {
        o.tl_time += step;
    }

    bool keep_playing = o.tl_playing;
    bool wrapped = false;
    std::vector<uint32_t> after;
    if (!o.tl_backwards) {
        if (!(o.tl_time < o.tl_duration)) {
            if (!o.tl_loops) {
                o.tl_time = o.tl_duration;
                after = {kFinished, kStopped, kStoppedForwards};
                keep_playing = false;
            } else {
                o.tl_time -= o.tl_duration;
                wrapped = true;
            }
        }
    } else if (o.tl_time < 0.0f) {
        if (!o.tl_loops) {
            o.tl_time = 0.0f;
            after = {kFinished, kStopped, kStoppedBackwards};
            keep_playing = false;
        } else {
            o.tl_time += o.tl_duration;
            wrapped = true;
        }
    }

    // The original broadcasts the stop events before it updates the properties,
    // with the playing flag it read at the top still standing; the flag is only
    // written back at the end.
    for (uint32_t e : after) broadcast(o.id, e, quests, out);
    if (wrapped) broadcast(o.id, kLooped, quests, out);

    const float percent = 0.0f < o.tl_duration ? o.tl_time / o.tl_duration : 1.0f;
    for (Track& track : o.tracks) track_update(o, track, percent, o.tl_backwards, quests, out);
    o.tl_playing = keep_playing && o.tl_playing;
}

// `CTimelineProperty::Update`, `triggerEvent`
// and `DoLinearInterpolation`.
void LevelScript::track_update(Object& o, Track& track, float percent, bool backwards,
                               QuestState& quests, std::vector<ScriptEvent>& out) {
    const tl::formats::CmpTimelineTrack* data = track.data;
    if (data == nullptr) return;
    percent = percent <= 0.0f ? 0.0f : std::min(percent, 1.0f);

    float last = track.last;
    if (percent == last) return;
    if (!backwards && 1.0f <= last && percent == 0.0f) {
        // Wrapped round a loop: start again from before the first key.
        last = -0.01f;
        track.last = -0.01f;
        return;
    }
    if (backwards) {
        if (last < percent && 0.0f < last) {
            track_update(o, track, 0.0f, true, quests, out);
            last = 1.01f;
        }
    } else if (percent < last && last < 1.0f) {
        track_update(o, track, 1.0f, false, quests, out);
        last = -0.01f;
    }

    if (data->keys.empty()) return;

    if (data->is_event) {
        for (const tl::formats::CmpTimelineKey& key : data->keys) {
            const bool crossed = backwards ? (key.time < last && percent <= key.time)
                                           : (last < key.time && key.time <= percent);
            if (!crossed) continue;
            Object* target = find(track.target);
            if (target != nullptr) {
                const uint32_t id = vocabulary(target->type).input_at(data->tag);
                if (id == kNone) {
                    unimplemented_.insert("timeline event " + std::to_string(data->tag) + " on " +
                                          vocabulary(target->type).descriptor);
                } else {
                    if (trace_on_) {
                        char buffer[512];
                        std::snprintf(buffer, sizeof(buffer), "%.3f K %s:%s -> %s:%s .%s",
                                      static_cast<double>(clock_), "Timeline", o.name.c_str(),
                                      vocabulary(target->type).descriptor, target->name.c_str(),
                                      input_name(id));
                        trace_.push_back(buffer);
                    }
                    // DIVERGENCE: the same cap as `broadcast`'s. An event track can reach its own
                    // timeline (a key at 0 firing `Reset`), and the original recurses until the
                    // stack gives out (`Reset_To_Beginning`'s guard, holds only at
                    // exactly -0.01).
                    if (depth_ > kMaxLogicDepth) break;
                    ++depth_;
                    input(track.target, id, quests, out);
                    --depth_;
                }
            }
            break;  // one key per update
        }
        track.last = percent;
        return;
    }

    // A value track, applied only when it drives the target's `POSITION`.
    if (!track.property.has_value()) {
        // No property at this index: `CTimeline::AddProperty`
        // never made one, so the original
        // sets nothing either. Noted for the log.
        note_unhandled(o, "timeline property " + std::to_string(data->tag));
        track.last = percent;
        return;
    }
    if (track.property->kind != TimelinePropertyKind::Position) {
        // A Damage Shape's `ENABLED` (`Set_setEnabled`: `setEnabled`)
        // and `TIMER` (`Set_setTimer`: the countdown with it).
        Object* shape = find(track.target);
        if (shape != nullptr && shape->damage_shape &&
            (track.property->name == "ENABLED" || track.property->name == "TIMER")) {
            const std::optional<TimelineValue> value =
                timeline_value(data->keys, track.interpolation, o.tl_default_interpolation, percent,
                               last, backwards, track.property->value_type);
            if (value.has_value()) {
                if (track.property->name == "ENABLED") {
                    damage_shape_set_enabled(*shape, value->as_int() != 0, quests, out);
                } else if (Object* again = find(track.target);
                           again != nullptr && again->damage_shape) {
                    again->damage_shape->timer = value->as_float();
                    again->damage_shape->time_left = value->as_float();
                }
            }
            track.last = percent;
            return;
        }
        // A Player Box Trigger's `DIMENSIONS`: `CTriggerBoxDescriptor::Set_setDimensions`
        // -> `CTriggerBox::setDimensions`.
        if (track.property->name == "DIMENSIONS") {
            Object* box = find(track.target);
            if (box != nullptr && box->type == type::kPlayerBoxTrigger) {
                const std::optional<TimelineValue> value =
                    timeline_value(data->keys, track.interpolation, o.tl_default_interpolation,
                                   percent, last, backwards, track.property->value_type);
                if (value.has_value()) box->half_extent = to_vec3(value->as_vec3()) * 0.5f;
            }
            track.last = percent;
            return;
        }
        // A Monster's `SCALE`: `CMonsterDescriptor::Set_setScale` ->
        // `CPositionableObject::setScale(float)` on all axes, then
        // `CCharacter::scaleUpdated` moves the model by -radius / scale (feet kept).
        if (track.property->name == "SCALE" && character(track.target) != nullptr) {
            const std::optional<TimelineValue> value =
                timeline_value(data->keys, track.interpolation, o.tl_default_interpolation, percent,
                               last, backwards, track.property->value_type);
            if (value.has_value()) {
                ScriptEvent e;
                e.action = ScriptAction::ScaleNode;
                e.node_id = track.target;
                e.amount = value->as_float();
                out.push_back(e);
            }
            track.last = percent;
            return;
        }
        // `FORWARD`, `RIGHT`, `UP`: `CPositionableObjectDescriptor::Set_setForward`,
        // `Set_setRight`, `Set_setUp`.
        const TimelinePropertyKind kind = track.property->kind;
        if (kind == TimelinePropertyKind::Forward || kind == TimelinePropertyKind::Right ||
            kind == TimelinePropertyKind::Up) {
            const std::optional<TimelineValue> value =
                timeline_value(data->keys, track.interpolation, o.tl_default_interpolation, percent,
                               last, backwards, track.property->value_type);
            Object* turned = find(track.target);
            if (value.has_value() && turned != nullptr) {
                set_orientation_column(*turned, kind, to_vec3(value->as_vec3()), out);
            }
            track.last = percent;
            return;
        }
        Object* target = find(track.target);
        if (target == nullptr) {
            track.last = percent;
            return;
        }
        const std::optional<TimelineValue> value =
            timeline_value(data->keys, track.interpolation, o.tl_default_interpolation, percent,
                           last, backwards, track.property->value_type);
        // `VISIBLE`: `CPositionableObjectDescriptor::Set_setVisible`, the object's
        // `setVisible`. A Monster's would be `CCharacter::setVisible`; not driven here.
        if (kind == TimelinePropertyKind::Visible && character(track.target) == nullptr) {
            if (value.has_value()) {
                target->visible = value->as_int() != 0;
                ScriptEvent e;
                e.action = ScriptAction::SetVisible;
                e.node_id = target->id;
                e.name = target->name;
                e.on = target->visible;
                out.push_back(e);
            }
            track.last = percent;
            return;
        }
        // A Property Node's `ENABLED`: `CPropertyNodeDescriptor::Set_setEnabled` ->
        // `setEnabled`, the input's own call.
        if (target->type == tl::formats::cmp_type::kMarker && track.property->name == "ENABLED") {
            if (value.has_value())
                property_node_set_enabled(*target, value->as_int() != 0, quests, out);
            track.last = percent;
            return;
        }
        // A Room Piece's `COLLISION ENABLED`: `CRoomPieceDescriptor::` `Set_setCollisionEnabled`
        // is read only as the piece loads (`initRoomPiece`;
        // `setCollisionMeshFilePath`): a key during
        // play changes nothing.
        if (target->type == tl::formats::cmp_type::kRoomPiece &&
            track.property->name == "COLLISION ENABLED") {
            track.last = percent;
            return;
        }
        // A particle affector's `ENABLE`: `CAffectorDescriptor::Set_setEnabled` ->
        // `CAffectorWrapper`'s `setEnabled`.
        if (is_affector(target->type) && track.property->name == "ENABLE") {
            if (value.has_value()) affector_set_enabled(*target, value->as_int() != 0, out);
            track.last = percent;
            return;
        }
        // INVENTED: the remaining setters (a Room Piece's `SCALE Z`, a Monster's
        // `VISIBLE`) are not transcribed and set nothing; noted for the log.
        note_unhandled(o, "timeline property " + std::string(track.property->name));
        track.last = percent;
        return;
    }
    // A key is where the target stands in its own file; into the level, through
    // `CTimelineProperty::SetValueOnObject`
    // and the property's setter.
    const std::optional<TimelineValue> val =
        timeline_value(data->keys, track.interpolation, o.tl_default_interpolation, percent, last,
                       backwards, track.property->value_type);
    if (val.has_value()) {
        const Vec3 at = frame_.point(to_vec3(val->as_vec3()));
        // The setter moves the object itself: what a skill event reads off a
        // moving damage shape (`CSkillEvent::updateEvent`).
        //
        if (Object* moved = find(track.target)) {
            moved->local_position = to_vec3(val->as_vec3());
            if (character(track.target) == nullptr) moved->position = at;
        }
        ScriptEvent e;
        e.action = ScriptAction::MoveNode;
        e.node_id = track.target;
        e.position = at;
        out.push_back(e);
        if (Character* c = character(track.target)) {
            c->motion.position = {at.x, at.y + c->motion.radius, at.z};
        }
    }
    track.last = percent;
}

void LevelScript::play_layout_timelines(QuestState& quests, std::vector<ScriptEvent>& out,
                                        bool backwards) {
    std::vector<int64_t> driven;
    for (const Object& o : objects_) {
        if (o.timeline != nullptr && o.tl_layout_control) driven.push_back(o.id);
    }
    for (int64_t id : driven) {
        Object* o = find(id);
        if (o == nullptr) continue;
        if (backwards) timeline_play_backwards(*o, quests, out);
        else timeline_play(*o, quests, out);
    }
}

// `CLayout::processInputs(unsigned int, CEditorBaseObject*)`,
// layout kind 2: of its kept objects (`editorObjectCreated`)
// the `LAYOUT CONTROL` timelines take the input through their descriptor.
bool LevelScript::process_inputs(uint32_t input, QuestState& quests,
                                 std::vector<ScriptEvent>& out) {
    std::vector<int64_t> driven;
    for (const Object& o : objects_) {
        if (o.type == tl::formats::cmp_type::kTimeline && o.tl_layout_control) {
            driven.push_back(o.id);
        }
    }
    for (const int64_t id : driven) this->input(id, input, quests, out);
    return !driven.empty();
}

// PORT: an input sent on a link with the decline flag up for its length.
void LevelScript::link_input_declined(int64_t link, uint32_t input, QuestState& quests,
                                      std::vector<ScriptEvent>& out) {
    link_input_declined_ = true;
    this->input(link, input, quests, out);
    link_input_declined_ = false;
}

// `CLayout::setDurationModification(float)` with
// `CTimeline::setDurationModificationTime(float)` under it.
void LevelScript::set_duration_modification(float value) {
    if (!(0.0f < value)) return;
    for (Object& o : objects_) {
        if (o.type != tl::formats::cmp_type::kTimeline || !o.tl_layout_control) continue;
        if (o.tl_modification == 1) {
            o.tl_duration = value;
        } else if (o.tl_modification == 2) {
            o.tl_duration = value * o.tl_duration;
        }
    }
}

// `CLayout::initOnLoad()`, called by
// `CLayoutDescriptor::descriptorSceneLoaded` on each linked layout: with the
// link's `START ON LOAD` clear, every `Timeline` of it loses its own (and `tl_playing`).
void LevelScript::init_on_load() {
    for (Object& o : objects_) {
        if (o.type != tl::formats::cmp_type::kTimeline) continue;
        o.tl_start_on_load = false;
        o.tl_playing = false;
    }
}

// PORT: every timeline the layout controls stopped (`timeline_stop_to`, which holds the rule).
void LevelScript::stop_layout_timelines(QuestState& quests, std::vector<ScriptEvent>& out) {
    std::vector<int64_t> driven;
    for (const Object& o : objects_) {
        if (o.timeline != nullptr && o.tl_layout_control && o.tl_playing) driven.push_back(o.id);
    }
    for (int64_t id : driven) {
        if (Object* o = find(id)) timeline_stop_to(*o, false, quests, out);
    }
}

// `CLayout::callFunctionOnObjects`, state 1 on each
// `CSoundObject` child: `CSoundObject::stop` only when its bank loops.
void LevelScript::stop_layout_sounds(QuestState& quests, std::vector<ScriptEvent>& out) {
    for (Object& o : objects_) {
        if (o.type != type::kSound || !o.sound.bank.looping) continue;
        sound_object_stop(o.sound, sound_host_, sound_output(o, quests, out));
    }
}

// `CSoundObject::~CSoundObject`: its bank deleted.
void LevelScript::release_sound_nodes() {
    for (Object& o : objects_) {
        if (o.type == type::kSound) sound_bank_destroy(o.sound.bank, sound_host_);
    }
}

// ---------------------------------------------------------------------------
// Quest Controller (`CQuestController`)
// ---------------------------------------------------------------------------

// `CQuestController::interactWithUnit`.
void LevelScript::quest_interact(Object& o, QuestState& quests, std::vector<ScriptEvent>& out) {
    // The unit, by name. None in the level: nothing happens at all.
    UnitRef unit;
    if (!base_unit_named(o.unit, unit)) return;

    // Listening for the menu (`CGameUI::addMenuListener(0, this)`), then
    // `CGameUI::closeAll`; the player's enabled flag kept and cleared when `PLAYER GETS
    // DISABLED`.
    o.qc_listening = true;
    o.qc_player_was_enabled = player_enabled_;
    if (o.qc_player_disabled) set_player_enabled(false, out);

    // `CGameUI::forceInteractiveDialog(player, unit, quest)`.
    force_interactive_dialog(o, unit, quests, out);
    broadcast(o.id, kInteracting, quests, out);
}

// `CMonsterDescriptor::InputLogicEvent`, case 0x30: the
// player's `stopPathing` and AI state 0x2c, then `CGameUI::forceInteractiveDialog(player,
// this, nullptr)`: `CQuestDialogMenu::` `setNPC` with no
// quest and the player's AI state back to 2.
void LevelScript::monster_interact(Object& o, QuestState& quests, std::vector<ScriptEvent>& out) {
    force_interactive_dialog(o, UnitRef{this, o.id}, quests, out);
}

// `CGameUI::forceInteractiveDialog(player, unit, quest)`
// and `CQuestDialogMenu::setNPC(unit, floaty, quest)`.
// With a quest, `CQuest::getQuestDialog(quest, unit)`
// picks the line; with none, `CQuestManager::getQuestForNPC(unit)` is asked and ignored.
// No line: the first of `getDialogsForNPC(list, unit, true)`.
//
bool LevelScript::force_interactive_dialog(Object& opener, const UnitRef& unit, QuestState& quests,
                                           std::vector<ScriptEvent>& out) {
    const Object* speaker = unit.machine != nullptr ? unit.machine->find(unit.node) : nullptr;
    const std::string& unit_name = speaker != nullptr ? speaker->unit : opener.unit;
    std::string quest = opener.type == type::kQuestController ? opener.quest : std::string();
    QuestSection section = QuestSection::Intro;
    const DialogLine* line = nullptr;
    if (quest.empty()) {
        if (quest_for_npc_) quest_for_npc_(unit_name);
    } else if (quest_dialog_) {
        line = quest_dialog_(quest, unit_name, &section);
    } else if (dialog_ != nullptr) {
        // INVENTED: a machine with no quest log to ask (the tests) takes the
        // book's line by a section order of its own.
        line = dialog_->find(quest, unit_name, quests.status(quest));
        if (line != nullptr) section = quest_section_of(line->section);
    }
    if (line == nullptr) {
        if (npc_dialog_) {
            line = npc_dialog_(unit_name, &quest, &section);
        } else if (dialog_ != nullptr) {
            // INVENTED: as above, the book in quest-name order.
            line = dialog_->find_for_unit(unit_name, quests, &quest);
            if (line != nullptr) section = quest_section_of(line->section);
        }
    }
    if (line == nullptr) {
        // `setNPC`'s error path: at once `Interacted Closed`, `Interacted Accepted`,
        // `Interacted` on the unit, menu event 1 to the listener, and the unit let go.
        if (opener.type == type::kQuestController && opener.qc_listening) {
            controller_menu_event(opener, 1, out);
        }
        if (unit.machine != nullptr && unit.machine->find(unit.node) != nullptr) {
            std::vector<ScriptEvent>& sink = sink_for(unit.machine, out);
            for (uint32_t said : {kInteractedClosed, kInteractedAccepted, kInteracted}) {
                unit.machine->broadcast(unit.node, said, quests, sink);
            }
        }
        return false;
    }
    if (line->floaty) {
        // `FLOATYTEXT`: a text event over the unit, and `onClick(10)` once the
        // menu next updates (`CQuestDialogMenu::update`).
        //
        show_floaty(opener, *line, out);
        floaty_.menu_event_pending = true;
        floaty_.node = opener.id;
        floaty_.menu_unit = unit;
        floaty_.section = section;
        floaty_.quest = quest;
        return false;
    }
    open_dialog(opener, *line, quest, section, unit, out);
    return true;
}

// `CQuestDialogMenu::onClick`, its broadcasts on the
// menu's unit (`CEditorBaseObject`). `OK` (10): `Interacted Closed`,
// `Interacted`; on a `[COMPLETE]` line `Interacted Closed`, `Interacted Accepted`,
// `Interacted`, then `CQuestManager::completeQuest`. `Accept` (9): `acceptQuest`
// -> `broadcastAcceptedOrDeclined(true)`,
// then `Interacted Closed`. `Decline` (8): the same with false.
void LevelScript::unit_menu_closed(const UnitRef& unit, int32_t menu_event, bool complete_line,
                                   const std::string& quest, QuestState& quests,
                                   std::vector<ScriptEvent>& out) {
    LevelScript* machine = unit.machine;
    if (machine == nullptr || machine->find(unit.node) == nullptr) return;
    std::vector<ScriptEvent>& sink = sink_for(machine, out);
    const auto say = [&](uint32_t said) { machine->broadcast(unit.node, said, quests, sink); };
    if (menu_event == 3 || menu_event == 4) {
        say(menu_event == 3 ? kInteractedAccepted : kInteractedDeclined);
        say(kInteracted);
        say(kInteractedClosed);
        return;
    }
    if (menu_event == kCloseButtonAccepts) {
        // `handle_CloseButton`: `Interacted Closed`; accept's `Interacted Accepted`,
        // `Interacted`; then decline's, whose `Interacted Declined` is skipped, and `Interacted`.
        say(kInteractedClosed);
        say(kInteractedAccepted);
        say(kInteracted);
        say(kInteracted);
        return;
    }
    if (menu_event == 2 && complete_line) {
        say(kInteractedClosed);
        say(kInteractedAccepted);
        say(kInteracted);
        ScriptEvent e;
        e.action = ScriptAction::CompleteQuest;
        e.node_id = unit.node;
        e.quest = quest;
        e.text = quest;
        out.push_back(e);
    }
    say(kInteractedClosed);
    say(kInteracted);
}

// `CQuestController::menuEventOccured`: the menu event kept
// (`+0xb4`), the controller on the level's update list, and the player's
// enabled flag put back when `PLAYER GETS DISABLED`.
void LevelScript::controller_menu_event(Object& o, int32_t menu_event,
                                        std::vector<ScriptEvent>& out) {
    o.qc_menu_event = menu_event;
    add_level_update(o.id);
    if (o.qc_player_disabled) set_player_enabled(o.qc_player_was_enabled, out);
}

// `CCinematicObject::play`: an empty
// `CINEMATIC` does nothing; else `CGameUI::setCinematicOpen(name)` and
// the node among the cinematic menu's listeners.
void LevelScript::cinematic_play(Object& o, std::vector<ScriptEvent>& out) {
    if (o.cinematic.empty()) return;
    ScriptEvent e;
    e.action = ScriptAction::OpenCinematic;
    e.node_id = o.id;
    e.name = o.name;
    e.text = o.cinematic;
    out.push_back(e);
    o.cine_listening = true;
}

// `CCinematicObject::menuEventOccured`: the event kept and the node on
// the update list. The menu broadcasts 2 as it closes (`CCinematicMenu::setOpen(false)`,
// `CDropdownMenu::broadcastEvent(1, 2)`).
void LevelScript::cinematic_menu_event(int32_t menu_event) {
    for (Object& o : objects_) {
        if (o.type != type::kCinematic || !o.cine_listening) continue;
        o.cine_menu_event = menu_event;
        add_level_update(o.id);
    }
}

// `CCinematicObject::updateLevelObject`: events 2, 3 and 4
// fire output 0x5c; then the node leaves the cinematic menu's listeners
// (`CGameUI::removeMenuListener(1, this)`) and the update list.
void LevelScript::cinematic_level_update(Object& o, QuestState& quests,
                                         std::vector<ScriptEvent>& out) {
    const int32_t event = o.cine_menu_event;
    if (event == 3 || event == 4 || event == 2) broadcast(o.id, 0x5c, quests, out);
    o.cine_listening = false;
}

// `CQuestDialogMenu::setNPC`: a boxed line turns its unit off
// (`setEnabled(false)`, vtable +0x40 - `CSceneNodeObject::setEnabled`
// only stores `+0x82`), its old flag kept in `+0x1e9` (`getEnabled`, +0x48).
void LevelScript::hold_menu_unit(const UnitRef& unit) {
    Object* u = unit.machine != nullptr ? unit.machine->find(unit.node) : nullptr;
    if (u == nullptr || u->menu_held) return;
    u->menu_held = true;
    u->menu_was_enabled = u->enabled;
    u->enabled = false;
}

// `setEnabled(+0x1e9)`, which `onClick`, `update`'s close and the next `setNPC`
// each do first.
// PORT: the unit a menu held let go.
void LevelScript::release_menu_unit(const UnitRef& unit) {
    Object* u = unit.machine != nullptr ? unit.machine->find(unit.node) : nullptr;
    if (u == nullptr || !u->menu_held) return;
    u->menu_held = false;
    u->enabled = u->menu_was_enabled;
}

// `CQuestDialogMenu::setNPC` on a unit the
// player talked to.
void LevelScript::menu_opened_on(int64_t node) { hold_menu_unit(UnitRef{this, node}); }

// `CQuestDialogMenu::setOpen(false)` without
// a click: `setEnabled(+0x1e9)`.
void LevelScript::menu_released(int64_t node) { release_menu_unit(UnitRef{this, node}); }

// `CQuestDialogMenu::onClick` on a unit the
// player talked to.
void LevelScript::menu_closed_on(int64_t node, int32_t menu_event, bool complete_line,
                                 const std::string& quest, QuestState& quests,
                                 std::vector<ScriptEvent>& out) {
    const UnitRef unit{this, node};
    release_menu_unit(unit);
    unit_menu_closed(unit, menu_event, complete_line, quest, quests, out);
}

// `CQuestController::questHasBeenAccepted`, `questHasBeenCompleted`
// and `questHasBeenAbandoned` on every controller of the quest, in turn.
void LevelScript::quest_notice(const QuestNotice& notice, QuestState& quests,
                               std::vector<ScriptEvent>& out) {
    std::vector<int64_t> heard;
    for (const Object& o : objects_) {
        if (o.type == type::kQuestController && o.quest == notice.quest) heard.push_back(o.id);
    }
    for (int64_t id : heard) {
        for (uint32_t output : notice.outputs) broadcast(id, output, quests, out);
    }
}

// `CQuestController::updateLevelObject`,
// after `menuEventOccured` put the controller on the level's update
// list.
void LevelScript::quest_level_update(Object& o, QuestState& quests, std::vector<ScriptEvent>& out) {
    if (o.qc_load_pending && o.qc_broadcast_on_load) {
        o.qc_load_pending = false;
        const QuestStatus status = quests.status(o.quest);
        const bool active = status == QuestStatus::Active || status == QuestStatus::Complete;
        broadcast(o.id, active ? kQuestActive : kQuestNotActive, quests, out);
        broadcast(o.id, status == QuestStatus::Complete ? kQuestComplete : kQuestNotComplete,
                  quests, out);
        return;
    }
    broadcast(o.id, kInteracted, quests, out);
    if (o.qc_menu_event == 3) broadcast(o.id, kInteractedAccepted, quests, out);
    if (o.qc_menu_event == 4) broadcast(o.id, kInteractedDeclined, quests, out);
    // `CGameUI::removeMenuListener(0, this)` (X360 inline in
    // `updateLevelObject` and the destructor): off the quest dialog menu's list.
    o.qc_listening = false;
}

// ---------------------------------------------------------------------------
// Damage Shape (`CDamageShape`)
// ---------------------------------------------------------------------------

// PORT: a damage shape's listeners (`DamageShapeListener`): one added.
bool LevelScript::damage_shape_add_listener(int64_t id, DamageShapeListener* listener) {
    Object* o = find(id);
    if (o == nullptr || !o->damage_shape || listener == nullptr) return false;
    o->damage_shape->listeners.push_back(listener);
    return true;
}

// PORT: a damage shape's listeners (`DamageShapeListener`): one removed.
bool LevelScript::damage_shape_remove_listener(int64_t id, DamageShapeListener* listener) {
    Object* o = find(id);
    if (o == nullptr || !o->damage_shape) return false;
    std::vector<DamageShapeListener*>& list = o->damage_shape->listeners;
    const auto it = std::find(list.begin(), list.end(), listener);
    if (it != list.end()) list.erase(it);
    return true;
}

// PORT: a damage shape's pending event taken by the renderer.
bool LevelScript::damage_shape_take_event(int64_t id, bool target_corpses, int32_t max_units_hit) {
    Object* o = find(id);
    if (o == nullptr || !o->damage_shape) return false;
    if (target_corpses) o->damage_shape->target_corpse = true;
    if (0 < max_units_hit) o->damage_shape->max_units_to_hit = max_units_hit;
    return true;
}

// PORT: the machine's damage shape nodes.
std::vector<int64_t> LevelScript::damage_shape_ids() const {
    std::vector<int64_t> ids;
    for (const Object& o : objects_) {
        if (o.damage_shape) ids.push_back(o.id);
    }
    return ids;
}

// PORT: every damage shape of the machine stopped.
void LevelScript::disable_damage_shapes(QuestState& quests, std::vector<ScriptEvent>& out) {
    for (int64_t id : damage_shape_ids()) {
        Object* o = find(id);
        if (o != nullptr && o->damage_shape && o->damage_shape->enabled) {
            damage_shape_set_enabled(*o, false, quests, out);
        }
    }
}

// PORT: a damage shape node by id.
const DamageShape* LevelScript::damage_shape(int64_t id) const {
    const Object* o = find(id);
    return o != nullptr && o->damage_shape ? &*o->damage_shape : nullptr;
}

// `CLevel::addCharacterUpdateListener`: a
// removal still waiting for the flush is called off (swapped with the last),
// and a listener not on the list yet goes to its front.
void LevelScript::add_character_update_listener(int64_t id) {
    std::vector<int64_t>& waiting = character_listener_removals_;
    const auto queued = std::find(waiting.begin(), waiting.end(), id);
    if (queued != waiting.end()) {
        *queued = waiting.back();
        waiting.pop_back();
    }
    if (std::find(character_listeners_.begin(), character_listeners_.end(), id) !=
        character_listeners_.end()) {
        return;
    }
    character_listeners_.insert(character_listeners_.begin(), id);
}

// `CLevel::removeCharacterUpdateListener`: a
// listener on the list is queued, once, for `flushCharacterUpdateListeners`.
void LevelScript::remove_character_update_listener(int64_t id) {
    if (std::find(character_listeners_.begin(), character_listeners_.end(), id) ==
        character_listeners_.end()) {
        return;
    }
    std::vector<int64_t>& waiting = character_listener_removals_;
    if (std::find(waiting.begin(), waiting.end(), id) != waiting.end()) return;
    waiting.push_back(id);
}

// `CLevel::flushCharacterUpdateListeners`.
void LevelScript::flush_character_update_listeners() {
    for (int64_t id : character_listener_removals_) {
        character_listeners_.erase(
            std::remove(character_listeners_.begin(), character_listeners_.end(), id),
            character_listeners_.end());
    }
    character_listener_removals_.clear();
}

// `CLevel::updateCharacters`, after a character's
// update: removals flushed, then every listener called, the list walked as it stood.
// INVENTED: the order across machines (the port walks them in `set_level_machines` order).
void LevelScript::character_updated_in_level(const UpdatedCharacter& who, float delta,
                                             QuestState& quests, std::vector<ScriptEvent>& out) {
    for (LevelScript* machine : level_machines()) {
        machine->flush_character_update_listeners();
        const std::vector<int64_t> listeners = machine->character_listeners_;
        for (int64_t id : listeners) {
            const Object* area = machine->find(id);
            if (area != nullptr && area->jump_down_area) {
                machine->jump_down_area_character_updated(*area, who, quests, out);
                continue;
            }
            machine->damage_shape_character_updated(id, who, delta, quests, out);
        }
    }
}

// `CPropertyNode::setEnabled`: the flag, `Enabled` /
// `Disabled` on every call, then a `Jump Down Area`'s `CPropertyNode::jumpDownLogic`
// listing, or a path blocker's `updatePathNodes`.
void LevelScript::property_node_set_enabled(Object& o, bool on, QuestState& quests,
                                            std::vector<ScriptEvent>& out) {
    o.enabled = on;
    broadcast(o.id, on ? kEnabled : kDisabled, quests, out);
    ScriptEvent e;
    e.action = ScriptAction::SetEnabled;
    e.node_id = o.id;
    e.name = o.name;
    e.on = on;
    out.push_back(e);
    if (!o.jump_down_area) return;
    if (on) {
        add_character_update_listener(o.id);
    } else {
        remove_character_update_listener(o.id);
    }
}

// `CAffectorWrapper::setEnabled`: `+0x119`, and the affector's own
// switch (`ParticleUniverse::ParticleAffector +0x110`) set to its negation,
// which the renderer does from the event. Nothing is broadcast.
void LevelScript::affector_set_enabled(Object& o, bool on, std::vector<ScriptEvent>& out) {
    o.enabled = on;
    ScriptEvent e;
    e.action = ScriptAction::AffectorEnabled;
    e.node_id = o.id;
    e.name = o.name;
    e.on = on;
    out.push_back(e);
}

// `CPositionableObject::setForward`, `CPositionableObject::setRight`,
// `CPositionableObject::setUp`, via their `Vector3` overloads:
// one column replaced, not normalised, then
// `setOrientation` from `Quaternion::FromRotationMatrix`.
void LevelScript::set_orientation_column(Object& o, TimelinePropertyKind column, const Vec3& local,
                                         std::vector<ScriptEvent>& out) {
    o.has_local_axes = true;
    switch (column) {
        case TimelinePropertyKind::Forward: o.local_forward = local; break;
        case TimelinePropertyKind::Right: o.local_right = local; break;
        case TimelinePropertyKind::Up: o.local_up = local; break;
        default: return;
    }
    o.forward = frame_.direction(o.local_forward);
    o.right = frame_.direction(o.local_right);
    o.up = frame_.direction(o.local_up);
    if (Character* c = character(o.id)) {
        // A character's facing is the yaw of the forward column, which the port draws it by;
        // its `RIGHT` and `UP` columns are kept but not drawn.
        if (column == TimelinePropertyKind::Forward) c->facing = yaw_of(o.forward);
        return;
    }
    ScriptEvent e;
    e.action = ScriptAction::OrientNode;
    e.node_id = o.id;
    e.name = o.name;
    e.axis_x = o.right;
    e.axis_y = o.up;
    e.axis_z = o.forward;
    out.push_back(e);
}

// `CPropertyNode::characterUpdatedInLevel`: an enabled `Jump Down Area` lets a
// character whose centre is in its box jump; an infinite box everywhere, a null one nowhere.
void LevelScript::jump_down_area_character_updated(const Object& area, const UpdatedCharacter& who,
                                                   QuestState& quests,
                                                   std::vector<ScriptEvent>& out) {
    if (!area.enabled || who.machine == nullptr) return;
    LevelScript& machine = *who.machine;
    Object* o = machine.find(who.unit.node);
    Character* c = machine.character(who.unit.node);
    if (o == nullptr || c == nullptr) return;
    const NoSpawnRegion& box = area.jump_down_box;
    if (box.extent == NoSpawnExtent::Null) return;
    const Vec3& p = c->motion.position;
    if (box.extent == NoSpawnExtent::Infinite ||
        (box.min.x <= p.x && p.x <= box.max.x && box.min.y <= p.y && p.y <= box.max.y &&
         box.min.z <= p.z && p.z <= box.max.z)) {
        machine.set_allow_jump_down(*o, *c, true, quests, out);
    }
}

LevelScript::UpdatedCharacter LevelScript::updated_character(int64_t id) {
    UpdatedCharacter who;
    who.unit = {this, id};
    who.machine = this;
    const Object* o = find(id);
    const Character* c = character(id);
    if (o == nullptr || c == nullptr) {
        who.targetable = false;
        return who;
    }
    const UnitStats& stats = stats_for(o->unit);
    who.at = feet_of(*c);
    who.radius = c->motion.radius;
    // INVENTED: `+0x70d`, the mesh hidden, is the node's `visible`, which the
    // `Show`/`Hide` inputs (`setMeshVisible`) set; the gibbing death that also
    // sets it (`die`) is not modelled.
    who.hidden = !o->visible;
    who.targetable = o->targetable_flag;
    who.alive = dead_.count(id) == 0;
    const UnitStats::Alignment side = alignment_of(*o);
    who.alignment = side == UnitStats::Alignment::Hostile    ? 2
                    : side == UnitStats::Alignment::Friendly ? 1
                                                             : 0;
    if (unit_types_ != nullptr && !stats.unittype.empty()) {
        who.unit_type = unit_types_->id_of(stats.unittype);
        who.known_type = true;
    }
    return who;
}

LevelScript::UpdatedCharacter LevelScript::updated_player() const {
    UpdatedCharacter who;
    who.at = player_at_;
    who.radius = player_radius_;
    who.alive = player_alive_;
    who.alignment = 1;  // `GOOD`
    // INVENTED: the player's own type is its class, which the port's script
    // does not know; `Player` (0x1c), which every class ISAs, stands in.
    who.unit_type = unit_type::kPlayer;
    who.known_type = true;
    return who;
}

// `CBaseUnit::ISA` -> `CResourceManager::ISA`
// -> `CHierarchy::ISA`; false with no hierarchy.
bool LevelScript::unit_isa(const UpdatedCharacter& who, uint32_t want) const {
    if (unit_types_ == nullptr) return false;
    if (who.known_type) return unit_types_->isa(who.unit_type, want);
    // INVENTED: a character whose type the port did not record passes only
    // `any` (0), which every type in `UNITTYPES.HIE` ISAs.
    return want == 0;
}

// `CDamageShape::setEnabled` with the level's side: the
// update list, or for a shape with no time and loops the instant pass, then disabled.
void LevelScript::damage_shape_set_enabled(Object& o, bool on, QuestState& quests,
                                           std::vector<ScriptEvent>& out) {
    if (!o.damage_shape) return;
    const int64_t id = o.id;
    const bool was = o.damage_shape->enabled;
    const DamageShapeSwitch next = tl::core::damage_shape_set_enabled(*o.damage_shape, on);
    o.enabled = on;
    if (was != on) {
        TL_LOG_INFO("script: damage shape %s %s", o.name.c_str(), on ? "enabled" : "disabled");
    }
    switch (next) {
        case DamageShapeSwitch::Stop: remove_character_update_listener(id); break;
        case DamageShapeSwitch::Listen: add_character_update_listener(id); break;
        case DamageShapeSwitch::Instant:
            damage_shape_instant(o, quests, out);
            if (Object* again = find(id)) damage_shape_set_enabled(*again, false, quests, out);
            break;
    }
}

// `CDamageShapeDescriptor::update` ->
// `CDamageShape::updateLevelObject`.
// INVENTED: `CLevel::snapToValidGround` is not
// transcribed; a shape stays at the height it was placed or moved to.
void LevelScript::damage_shape_update(Object& o, float delta, QuestState& quests,
                                      std::vector<ScriptEvent>& out) {
    if (!o.damage_shape) return;
    if (damage_shape_finished(*o.damage_shape)) {
        damage_shape_set_enabled(o, false, quests, out);  // `Stop`: raises nothing
        o.damage_shape->done = false;
    }
    DamageShape& s = *o.damage_shape;
    s.ticked = true;
    if (!s.enabled) return;
    damage_shape_step(s, delta);
}

// `CDamageShape::doInstantDamage`: one pass with no time over every character
// (the player, then each machine's in creation order), then every item
// (`itemUpdatedInLevel`).
void LevelScript::damage_shape_instant(Object& o, QuestState& quests,
                                       std::vector<ScriptEvent>& out) {
    const int64_t id = o.id;
    damage_shape_character_updated(id, updated_player(), 0.0f, quests, out);
    for (LevelScript* machine : level_machines()) {
        std::vector<ScriptEvent>& sink = machine == this ? out : machine->outbox_;
        const std::vector<int64_t> order = machine->character_order_;
        for (int64_t cid : order) {
            damage_shape_character_updated(id, machine->updated_character(cid), 0.0f, quests, sink);
        }
    }
    if (level_items_ != nullptr && level_items_->list) {
        for (const DamageShapeItem& item : level_items_->list())
            damage_shape_item_updated(id, item);
    }
}

// `CLevel::updateItems`: the listeners of
// every machine, as `character_updated_in_level` walks them.
void LevelScript::item_updated_in_level(const DamageShapeItem& item) {
    for (LevelScript* machine : level_machines()) {
        machine->flush_character_update_listeners();
        const std::vector<int64_t> listeners = machine->character_listeners_;
        for (int64_t id : listeners) machine->damage_shape_item_updated(id, item);
    }
}

// `CDamageShape::itemUpdatedInLevel` -> `doDamageToItem`
// (vtable `+0x200`), then the item's
// `interact(nullptr)` (vtable +0x280).
void LevelScript::damage_shape_item_updated(int64_t id, const DamageShapeItem& item) {
    Object* o = find(id);
    if (o == nullptr || !o->damage_shape) return;
    const ShapeFrame frame{o->position, o->forward, o->right, o->up};
    // INVENTED: with no hierarchy handed over only `any` (0) passes, as for a
    // character (`unit_isa`).
    const uint32_t want = o->damage_shape->unit_type;
    const bool isa = unit_types_ != nullptr ? unit_types_->isa(item.unit_type, want) : want == 0;
    if (!damage_shape_do_damage_to_item(*o->damage_shape, frame, item, isa)) return;
    TL_LOG_INFO("script: damage shape %s reaches item %lld", o->name.c_str(),
                static_cast<long long>(item.id));
    if (level_items_ != nullptr && level_items_->interact) level_items_->interact(item.id);
}

// `CDamageShape::characterUpdatedInLevel` -> `doDamageToCharacter`
// (vtable `+0x1f8`). `out` takes the events
// of `who`'s machine.
void LevelScript::damage_shape_character_updated(int64_t id, const UpdatedCharacter& who,
                                                 float delta, QuestState& quests,
                                                 std::vector<ScriptEvent>& out) {
    Object* o = find(id);
    if (o == nullptr || !o->damage_shape) return;
    DamageShape& s = *o->damage_shape;
    const ShapeFrame frame{o->position, o->forward, o->right, o->up};
    if (!damage_shape_reaches(s, frame, who.hidden, who.targetable, who.alive, who.at,
                              who.radius)) {
        return;
    }
    DamageShapeVictim victim;
    victim.unit = who.unit;
    victim.isa = unit_isa(who, s.unit_type);
    victim.alignment = who.alignment;
    victim.alive = who.alive;
    // `CDamageShape +0x178`: the constructor's `getGraph(L"DAMAGE_MONSTER")`.
    const StatGraph* graph = graphs_ != nullptr ? graphs_->find(kMonsterDamageGraph) : nullptr;
    const DamageShapeHit hit = damage_shape_do_damage_to_character(
        s, victim, graph, level_monster_level_, delta, volatile_random());
    if (!hit.deal) return;
    const std::string name = o->name;
    // Nothing lands at 0: the type-0x34 effect reaches `modifyHP(0)` and returns
    // (`applyInstantEffect`), and `applyDamage`'s 0 changes nothing. A
    // shape left for a skill's listener (`MIN` = `MAX` = 0) is all of these.
    if (hit.amount != 0.0f) {
        if (!s.dot) {
            const Object* victim =
                who.machine != nullptr ? who.machine->find(who.unit.node) : nullptr;
            TL_LOG_INFO("script: damage shape %s hits %s for %.2f", name.c_str(),
                        victim != nullptr ? victim->unit.c_str() : "the player",
                        static_cast<double>(hit.amount));
        }
        damage_shape_land(id, name, who, hit.amount, s.dot, quests, out);
    }
    // Landing can run wires that add objects; find the shape again.
    if (hit.record) {
        if (Object* again = find(id); again != nullptr && again->damage_shape) {
            again->damage_shape->hit.push_back(who.unit);
        }
    }
}

// A DOT shape calls `CCharacter::applyDamage` with no
// attacker; any other hands an instant 0x34 effect (`applyEffectOnUnit`
// -> `applyInstantEffect`), passing the struck
// flag. INVENTED: that effect's block roll, resistances and knock-back are not
// transcribed.
void LevelScript::damage_shape_land(int64_t id, const std::string& name,
                                    const UpdatedCharacter& who, float amount, bool dot,
                                    QuestState& quests, std::vector<ScriptEvent>& out) {
    LevelScript* machine = who.machine;
    const Character* c = machine != nullptr ? machine->character(who.unit.node) : nullptr;
    if (machine == nullptr) {
        // The player lives in the renderer: a `Strike` on them, as a monster's
        // blow on them is.
        ScriptEvent e;
        e.action = ScriptAction::Strike;
        e.node_id = id;
        e.name = name;
        e.target_node = who.unit.node;
        e.target_machine = machine;
        e.text = machine == nullptr ? std::string("the player") : c->unit;
        e.amount = amount;
        e.over_time = dot;
        out.push_back(e);
        return;
    }
    // `CDamageShape::doDamageToCharacter`: `applyDamage` with the
    // direction from the shape to the character on the ground, normalised,
    // and a force of 1.
    KillingBlow blow;
    blow.has_direction = true;
    blow.force = 1.0f;
    if (const Object* shape = find(id))
        blow.direction = blow_direction(shape->position, who.at, 0.0f);
    machine->damage_unit(who.unit.node, amount, kNoAttacker, quests, out, nullptr, &blow, !dot);
}

// ---------------------------------------------------------------------------
// Camera Controller (`CCameraController`)
// ---------------------------------------------------------------------------

namespace {

/// The controller's ease graph, `CGraph(1, ...)` from `startCamera`,
/// read by `CGraph::getValue`: a
/// `ParticleUniverse::DynamicAttributeCurved` spline over an `Ogre::SimpleSpline`
/// (`processControlPoints`; `getValue`). Quirk kept:
/// points evenly spaced by index and Catmull-Rom overshoot, so `Camera Stopped` comes at about 69%
/// of the pan.
float camera_ease(float t, float x, float y) {
    std::array<std::array<float, 2>, 3> p{{{0.0f, 0.0f}, {x, y}, {1.0f, 1.0f}}};
    for (size_t i = 1; i < p.size(); ++i) {
        for (size_t j = i; 0 < j && p[j][0] < p[j - 1][0]; --j) std::swap(p[j], p[j - 1]);
    }
    const float range = p[2][0] - p[0][0];
    float fraction = 1.0f;
    if (range != 0.0f) fraction = std::min(t / range, 1.0f);

    // `Ogre::SimpleSpline::interpolate(Real)` and `interpolate(unsigned, Real)`.
    const float segment = fraction * 2.0f;
    const auto index = static_cast<unsigned>(segment);
    const float s = segment - static_cast<float>(index);
    if (index + 1 == p.size()) return p[index][1];
    if (s == 0.0f) return p[index][1];
    if (s == 1.0f) return p[index + 1][1];
    // `recalcTangents`: an open spline (first point differs from the last).
    const auto tangent = [&p](size_t i) {
        if (i == 0) return 0.5f * (p[1][1] - p[0][1]);
        if (i == p.size() - 1) return 0.5f * (p[i][1] - p[i - 1][1]);
        return 0.5f * (p[i + 1][1] - p[i - 1][1]);
    };
    const float s2 = s * s;
    const float s3 = s2 * s;
    // powers (s^3, s^2, s, 1) times the Hermite matrix, then times the points.
    const float w0 = s3 * 2.0f + s2 * -3.0f + s * 0.0f + 1.0f;
    const float w1 = s3 * -2.0f + s2 * 3.0f + s * 0.0f + 0.0f;
    const float w2 = s3 * 1.0f + s2 * -2.0f + s * 1.0f + 0.0f;
    const float w3 = s3 * 1.0f + s2 * -1.0f + s * 0.0f + 0.0f;
    return w0 * p[index][1] + w1 * p[index + 1][1] + w2 * tangent(index) + w3 * tangent(index + 1);
}

}  // namespace

// `CCameraController::startCamera`.
void LevelScript::camera_start(Object& o, QuestState& quests, std::vector<ScriptEvent>& out) {
    // `configureUnitsInLevel` binds a named unit whatever the type;
    // only a `Unit` camera that still has none gives up.
    if (o.cam_unit.machine == nullptr) {
        // `CCameraController::configureUnit`: `getBaseUnitByDataGroup`.
        UnitRef unit;
        if (base_unit_named(o.unit, unit)) o.cam_unit = unit;
    }
    const Object* unit =
        o.cam_unit.machine != nullptr ? o.cam_unit.machine->find(o.cam_unit.node) : nullptr;
    if (o.cam_type == 1 && unit == nullptr) return;

    o.cam_stopped = false;
    o.cam_start = camera_holder_ != 0 ? camera_look_at_ : player_at_;
    // With no unit, the controller's own place in the level (its position
    // plus its scene's, `getPosition(true)`).
    const Vec3 target = unit != nullptr ? o.cam_unit.machine->position_of(*unit) : o.position;
    o.cam_delta = target - o.cam_start;
    o.cam_remaining = o.cam_pan;

    // The player: disabled, pathing stopped, AI state 0x2c.
    set_player_enabled(false, out);
    camera_holder_ = o.id;

    ScriptEvent e;
    e.action = ScriptAction::CameraOn;
    e.node_id = o.id;
    e.name = o.name;
    e.position = o.cam_start;
    e.follow_id = o.cam_unit.node;
    e.pan_time = o.cam_pan;
    e.ease_at = o.cam_ease_x;
    out.push_back(e);

    broadcast(o.id, kCameraMoving, quests, out);
    o.cam_pause_left = 0.0f;
    o.cam_pausing = false;
    o.enabled = true;
}

// `CCameraController::stopCamera`.
void LevelScript::camera_stop(Object& o, QuestState& quests, std::vector<ScriptEvent>& out) {
    o.cam_pause_left = 0.0f;
    if (!o.cam_stopped) {
        if (o.enabled) broadcast(o.id, kCameraStopped, quests, out);
        o.cam_stopped = true;
    }
    set_player_enabled(true, out);
    camera_look_at_ = player_at_;
    if (camera_holder_ == o.id || camera_holder_ != 0) {
        camera_holder_ = 0;
        ScriptEvent e;
        e.action = ScriptAction::CameraOff;
        e.node_id = o.id;
        e.name = o.name;
        out.push_back(e);
    }
    broadcast(o.id, kCameraControlRestored, quests, out);
    o.enabled = false;
}

// `CCameraController::update`.
void LevelScript::camera_update(Object& o, float delta, QuestState& quests,
                                std::vector<ScriptEvent>& out) {
    if (!o.enabled) return;
    const Object* unit =
        o.cam_unit.machine != nullptr ? o.cam_unit.machine->find(o.cam_unit.node) : nullptr;
    Vec3 look = camera_look_at_;
    if (0.0f < o.cam_remaining || o.cam_pausing) {
        if (o.cam_follow && unit != nullptr) {
            o.cam_delta = o.cam_unit.machine->position_of(*unit) - o.cam_start;
        }
        if (o.cam_pausing) o.cam_pause_left -= delta;
        o.cam_remaining -= delta;
        float t = 1.0f - std::max(o.cam_remaining, 0.0f) / o.cam_pan;
        t = t <= 0.0f ? 0.0f : std::min(t, 1.0f);
        const float g = camera_ease(t, o.cam_ease_x, o.cam_ease_y);
        look = o.cam_start + o.cam_delta * g;
        camera_look_at_ = look;
        ScriptEvent e;
        e.action = ScriptAction::CameraLookAt;
        e.node_id = o.id;
        e.position = look;
        out.push_back(e);
        if (1.0f <= g) {
            if (0.0f < o.cam_pause && !o.cam_pausing) {
                o.cam_pausing = true;
                o.cam_pause_left = o.cam_pause;
                broadcast(o.id, kCameraPausing, quests, out);
                return;
            }
            if (o.cam_pause_left <= 0.0f && !o.cam_stopped) {
                o.cam_stopped = true;
                broadcast(o.id, kCameraStopped, quests, out);
                o.cam_pausing = false;
                if (o.cam_resets) camera_stop(o, quests, out);
            }
        }
        return;
    }
    look = o.cam_follow && unit != nullptr ? position_of(*unit) : o.position;
    camera_look_at_ = look;
    ScriptEvent e;
    e.action = ScriptAction::CameraLookAt;
    e.node_id = o.id;
    e.position = look;
    out.push_back(e);
}

// The player's `setEnabled` (`CSceneNodeObject::setEnabled`, vtable +0x40):
// one character, whichever room's node asks.
void LevelScript::set_player_enabled(bool on, std::vector<ScriptEvent>& out) {
    for (LevelScript* machine : level_machines()) machine->player_enabled_ = on;
    player_enabled_ = on;
    ScriptEvent e;
    e.action = ScriptAction::PlayerEnabled;
    e.on = on;
    out.push_back(e);
}

// ---------------------------------------------------------------------------
// Game State Controller (`CGameStateController`)
// ---------------------------------------------------------------------------

namespace {
// `CGameClient +0x10bc`.
bool g_client_paused = false;
}  // namespace

// PORT: the level machine's own bookkeeping: an accessor.
bool LevelScript::client_paused() { return g_client_paused; }

void LevelScript::reset_client_paused() { g_client_paused = false; }

// `CGameStateController::performStateControl`: nothing
// before scene activation; then `INVULNERABLE`, the state, and `Enabled` / `Disabled`.
void LevelScript::game_state_perform(Object& o, QuestState& quests, std::vector<ScriptEvent>& out) {
    if (!o.gs_initialized) return;
    if (o.gs_invulnerable) {
        ScriptEvent e;
        e.action = ScriptAction::PlayerInvincible;
        e.node_id = o.id;
        e.name = o.name;
        e.on = o.enabled;
        out.push_back(e);
    }
    game_state_do(o, o.enabled, out);
    broadcast(o.id, o.enabled ? kEnabled : kDisabled, quests, out);
}

// `CGameStateController::doActualGameState(bool)`, jump
// table. The level's halves here; the interface's go out as `GameState`.
//   1 `PAUSE PLAYER`: player disabled, `stopPathing`. 2 `PAUSE GAME`: `+0x10bc`, disabled.
//   3 `INTERACTIVE UI`, 4 `HIDE PLAYER`, 5 `DISABLE SKILLS`, 6 `CLOSE INGAME UI`.
void LevelScript::game_state_do(const Object& o, bool on, std::vector<ScriptEvent>& out) {
    switch (o.gs_type) {
        case 1: set_player_enabled(!on, out); break;
        case 2:
            g_client_paused = on;
            set_player_enabled(!on, out);
            break;
        case 3:
            if (!on) set_player_enabled(true, out);
            break;
        case 4: {
            ScriptEvent shown;
            shown.action = ScriptAction::PlayerMeshVisible;
            shown.node_id = o.id;
            shown.name = o.name;
            shown.on = !on;
            out.push_back(shown);
            break;
        }
        case 5: globally_disable_skills(on); break;
        case 6: close_all_dialogs(out); break;
        default: break;
    }
    ScriptEvent e;
    e.action = ScriptAction::GameState;
    e.node_id = o.id;
    e.name = o.name;
    e.on = on;
    e.text = o.gs_state;
    e.input = static_cast<uint32_t>(o.gs_type);
    out.push_back(e);
}

// `CGameStateControllerDescriptor::InputLogicEvent`,
// `Warp Pets to Player` (0x4b): `teleportToMaster(1.0)`
// on every pet.
void LevelScript::warp_pets_to_player() {
    for (LevelScript* machine : level_machines()) {
        for (const int64_t id : machine->player_pets()) {
            Object* pet = machine->find(id);
            Character* c = machine->character(id);
            if (pet != nullptr && c != nullptr) machine->teleport_to_master(*pet, *c, 1.0f);
        }
    }
}

// ---------------------------------------------------------------------------
// Skip Cutscene (`CSkipCutscene`)
// ---------------------------------------------------------------------------

void LevelScript::skip_cutscene(QuestState&, std::vector<ScriptEvent>&) {
    // `CSkipCutscene::update`: Escape on an
    // enabled node marks it.
    for (Object& o : objects_) {
        if (o.type == type::kSkipCutscene && o.enabled) o.skip_pending = true;
    }
}

// `CGameUI::closeAll`, as far as the
// machine's lines go: the dialog menus are hidden, so the line on screen and
// those queued go.
void LevelScript::close_all_dialogs(std::vector<ScriptEvent>& out) {
    if (showing_ && !dialogs_.empty()) {
        ScriptEvent e;
        e.action = ScriptAction::Dialog;
        e.node_id = dialogs_.front().node;
        e.name = dialogs_.front().name;
        e.unit = dialogs_.front().unit;
        e.on = false;
        out.push_back(e);
    }
    dialogs_.clear();
    showing_ = false;
    clear_floaty(out);
}

// `CSkipCutscene::update`, the pending half.
void LevelScript::skip_update(Object& o, QuestState& quests, std::vector<ScriptEvent>& out) {
    if (!o.enabled || !o.skip_pending) return;

    // `+0x5a`, `DISABLE SKILLS`: no skill starts from anything the skip sets
    // off, until its own broadcasts are out.
    if (o.skip_skills) globally_disable_skills(true);
    // Logic muted: none of these stops fire anything.
    logic_live_ = false;
    close_all_dialogs(out);

    auto under_parent = [&](const Object& other) {
        // `CEditorBaseObject::isChildOfObject(parent id)`: the node or any
        // ancestor is the skip node's parent.
        int64_t at = other.parent;
        for (int guard = 0; guard < 64 && at != -1 && at != 0; ++guard) {
            if (at == o.parent) return true;
            const Object* up = find(at);
            if (up == nullptr) break;
            at = up->parent;
        }
        return false;
    };
    for (Object& other : objects_) {
        if (!under_parent(other)) continue;
        if (other.type == type::kAnimationController && other.anim_playing &&
            !other.anim_start_on_load) {
            animation_stop(other, true, quests, out);
        } else if (other.type == type::kCameraController && other.enabled) {
            camera_stop(other, quests, out);
        } else if (other.type == type::kTimer && other.enabled) {
            timer_set_enabled(other, false, quests, out);
        }
    }
    logic_live_ = true;

    // The player: `setEnabled(true)`, `setPathToFollow(nullptr)` (vtable
    // +0x310, which drops the path and keeps the destination), `setVisible(true)`
    // and `setMeshVisible(true, true)`.
    set_player_enabled(true, out);
    for (LevelScript* machine : level_machines()) machine->player_path_ = 0;
    player_path_ = 0;
    ScriptEvent shown;
    shown.action = ScriptAction::PlayerMeshVisible;
    shown.node_id = o.id;
    shown.name = o.name;
    shown.on = true;
    out.push_back(shown);
    o.enabled = false;
    broadcast(o.id, kDisabled, quests, out);
    o.skip_pending = false;
    broadcast(o.id, kSkipCutscene, quests, out);
    if (o.skip_skills) globally_disable_skills(false);
}

// ---------------------------------------------------------------------------
// Skill Controller (`CSkillController`)
// ---------------------------------------------------------------------------

// `CSkillController::configureUnit`, settled once. No
// `UNIT`: the player when `GROUP` is `PLAYERS`. A `[Player]` (0x1c) `UNIT`: the player;
// any other: the level's first (`CLevel::getBaseUnitByDataGroup`).
//
bool LevelScript::skill_configure(Object& o) {
    if (o.sk_has_unit) return true;
    if (o.sk_unit.empty()) {
        // Compared exactly (`wmemcmp`).
        if (o.sk_group == "PLAYERS") {
            o.sk_has_unit = true;
            o.sk_player = true;
        }
        return o.sk_has_unit;
    }
    const std::string& type_name = stats_for(o.sk_unit).unittype;
    if (unit_types_ != nullptr &&
        unit_types_->isa(unit_types_->id_of(type_name), unit_type::kPlayer)) {
        o.sk_has_unit = true;
        o.sk_player = true;
        return true;
    }
    const std::vector<UnitRef> units = units_named(o.sk_unit);
    if (units.empty()) return false;
    o.sk_has_unit = true;
    o.sk_ref = units.front();
    return true;
}

// `CSkillController::learnSkill`: a skill the
// unit already knows is left alone (`CSkillManager::knowsSkill`);
// otherwise `addSkillByName`, its level, and `Skill Learned`.
void LevelScript::skill_learn(Object& o, QuestState& quests, std::vector<ScriptEvent>& out) {
    if (!skill_configure(o) || o.sk_skill.empty()) return;
    if (!o.sk_player) {
        // INVENTED: a monster's skill manager is not reachable from here. No
        // shipped layout aims a Skill Controller at one (the game's only one is
        // the mines boss room's `KillPet`, on `PLAYERS`).
        unimplemented_.insert("Skill Controller on a monster");
        return;
    }
    if (player_skills_.knows && player_skills_.knows(o.sk_skill)) return;
    if (player_skills_.learn && player_skills_.learn(o.sk_skill)) {
        broadcast(o.id, kSkillLearned, quests, out);
    }
}

// INVENTED: a `Skill` node's unlearn, read from the node's inputs and outputs
// and not from its code: the player's skill unlearned, then `SKILL UNLEARNED`
// broadcast.
void LevelScript::skill_unlearn(Object& o, QuestState& quests, std::vector<ScriptEvent>& out) {
    if (!skill_configure(o) || o.sk_skill.empty() || !o.sk_has_unit) return;
    if (o.sk_player && player_skills_.unlearn) player_skills_.unlearn(o.sk_skill);
    broadcast(o.id, kSkillUnlearned, quests, out);
}

// `CSkillController::update`, for a character
// (`+0x188` 0, which every character is: `CCharacter::CCharacter` passes 0).
//
void LevelScript::skill_update(Object& o, QuestState& quests, std::vector<ScriptEvent>& out) {
    if (o.sk_skill.empty()) return;
    if (!o.sk_stopping) {
        if (!o.sk_starting) return;
        if (skill_configure(o)) {
            if (!o.sk_player) {
                // INVENTED, as in `skill_learn`.
                unimplemented_.insert("Skill Controller on a monster");
                o.sk_starting = false;
                return;
            }
            // Already running: `Skill Started` and nothing cast.
            if (player_skills_.updating && player_skills_.updating(o.sk_skill)) {
                o.sk_starting = false;
                broadcast(o.id, kSkillStarted, quests, out);
                return;
            }
            if (o.sk_learn_on_start) skill_learn(o, quests, out);
            // `CSkillController::getSkillTarget`: `USE UNIT TARGET` needs a target the
            // player does not carry here, so the start waits.
            if (o.sk_use_unit_target && !o.sk_target_player && player_target_ == 0) return;
            // `getSkill` by the upper-cased name: a skill the player
            // does not have is not cast, and nothing is said.
            if (player_skills_.knows && player_skills_.knows(o.sk_skill)) {
                if (player_skills_.cast) player_skills_.cast(o.sk_skill, o.sk_level);
            } else {
                TL_LOG_INFO("script: skill controller '%s': the player has no '%s'; nothing cast",
                            o.name.c_str(), o.sk_skill.c_str());
            }
        }
        o.sk_starting = false;
        return;
    }
    if (!o.sk_has_unit) return;
    if (!skill_configure(o) || !o.sk_player) {
        o.sk_stopping = false;
        broadcast(o.id, kSkillStopped, quests, out);
        return;
    }
    if (player_skills_.stop) player_skills_.stop(o.sk_skill, o.sk_force_stop);
    o.sk_stopping = player_skills_.updating && player_skills_.updating(o.sk_skill);
    if (!o.sk_stopping) {
        broadcast(o.id, kSkillStopped, quests, out);
        if (o.sk_unlearn_on_stop) skill_unlearn(o, quests, out);
    }
}

// ---------------------------------------------------------------------------
// Animation Controller (`CAnimationPlayer`)
// ---------------------------------------------------------------------------

// `CLevel::getBaseUnitsByDataGroup`: every unit of the
// level's lists with the data group, over all the level's machines (a room's node can
// reach a unit another machine runs).
std::vector<LevelScript::UnitRef> LevelScript::units_named(const std::string& unit) const {
    std::vector<UnitRef> found;
    if (unit.empty()) return found;
    for (LevelScript* machine : level_machines()) {
        for (const Object& o : machine->objects_) {
            if (o.type == type::kMonster && o.unit == unit && machine->dead_.count(o.id) == 0) {
                found.push_back({machine, o.id});
            }
        }
    }
    return found;
}

// `CLevel::getBaseUnitByDataGroup`: the first of the
// first list (`Monster`s), else the first of the second (`Unit Trigger`s).
bool LevelScript::base_unit_named(const std::string& unit, UnitRef& found) const {
    if (unit.empty()) return false;
    const std::vector<UnitRef> units = units_named(unit);
    if (!units.empty()) {
        found = units.front();
        return true;
    }
    for (LevelScript* machine : level_machines()) {
        for (const Object& o : machine->objects_) {
            if (o.type == type::kUnitTrigger && o.unit == unit) {
                found = {machine, o.id};
                return true;
            }
        }
    }
    return false;
}

// PORT: where the events of another machine's unit go.
std::vector<ScriptEvent>& LevelScript::sink_for(LevelScript* machine,
                                                std::vector<ScriptEvent>& out) {
    return machine == nullptr || machine == this ? out : machine->outbox_;
}

// PORT: a path node by id.
LevelScript::Object* LevelScript::path_of(const Object& unit) {
    if (unit.path == 0) return nullptr;
    return (unit.path_machine != nullptr ? unit.path_machine : this)->find(unit.path);
}

// PORT: a unit's path reached its end: `END OF PATH REACHED` broadcast once.
void LevelScript::path_reached(LevelScript* machine, Object& path, QuestState& quests,
                               std::vector<ScriptEvent>& out) {
    if (path.path_ended) return;
    path.path_ended = true;
    LevelScript* owner = machine != nullptr ? machine : this;
    owner->broadcast(path.id, kEndOfPathReached, quests, sink_for(owner, out));
}

// The unit list `CAnimationPlayer::playAnimation(bool)`
// rebuilds.
void LevelScript::animation_collect(Object& o) {
    o.anim_units.clear();
    // The first group is the level's player (`CLevel +0x28`, its first
    // entry's `+0x58`), which the renderer holds.
    o.anim_player = o.anim_group == "PLAYER";
    if (o.anim_player) return;
    o.anim_units = units_named(o.anim_unit);
}

// `CAnimationPlayer::playAnimation(bool)`.
void LevelScript::animation_play(Object& o, bool loops, QuestState&,
                                 std::vector<ScriptEvent>& out) {
    animation_collect(o);
    o.anim_remaining = 0.0f;
    if (o.anim_player && animation_length_) {
        const float length = animation_length_("PLAYER", o.anim_name);
        o.anim_remaining = length > 0.0f ? length : 0.0f;
    }
    if (!o.anim_units.empty() && animation_length_) {
        const UnitRef& ref = o.anim_units.front();
        const Object* first = ref.machine->find(ref.node);
        if (first != nullptr) {
            const float length = animation_length_(first->unit, o.anim_name);
            o.anim_remaining = length > 0.0f ? length : 0.0f;
        }
    }
    if (o.anim_force <= 0.0f) {
        if (loops) {
            o.anim_timed = false;
        } else {
            o.anim_remaining -= o.anim_blend_out;
            o.anim_timed = true;
        }
    } else {
        if (!loops && 0.0f < o.anim_remaining) {
            o.anim_remaining = std::min(o.anim_remaining, o.anim_force);
        }
        o.anim_remaining -= o.anim_blend_out;
        o.anim_timed = true;
    }
    o.anim_loops = loops;
    if (!o.anim_units.empty()) animation_apply(o, out);
    if (o.anim_player && !o.anim_name.empty()) {
        // `setAIPlayAnimation` on the player; the renderer plays it.
        ScriptEvent e;
        e.action = ScriptAction::Animation;
        e.name = o.name;
        e.unit = "PLAYER";
        e.to_player = true;
        e.text = o.anim_name;
        e.on = true;
        e.loops = loops;
        e.blend = o.anim_blend;
        out.push_back(e);
    }
    o.anim_playing = true;
}

// `CAnimationPlayer::playAnimation()`: each
// unit not already on the animation gets `setAIPlayAnimation`.
void LevelScript::animation_apply(Object& o, std::vector<ScriptEvent>& out) {
    for (const UnitRef& ref : o.anim_units) {
        Object* unit = ref.machine->find(ref.node);
        if (unit == nullptr) continue;
        if (unit->ai == ai_state::kPlayAnimation && unit->animation == o.anim_name) continue;
        ref.machine->set_ai_play_animation(*unit, o.anim_name, o.anim_loops, o.anim_blend,
                                           sink_for(ref.machine, out));
    }
}

// `CAnimationPlayer::stopAnimation(bool)`.
void LevelScript::animation_stop(Object& o, bool idle, QuestState& quests,
                                 std::vector<ScriptEvent>& out) {
    if (!o.anim_playing) return;
    o.anim_playing = false;
    if (o.anim_player && !o.anim_name.empty() && (o.anim_play_idle || idle)) {
        ScriptEvent e;
        e.action = ScriptAction::Animation;
        e.name = o.name;
        e.unit = "PLAYER";
        e.to_player = true;
        e.text = o.anim_name;
        e.on = false;
        out.push_back(e);
    }
    if (o.anim_play_idle || idle) {
        for (const UnitRef& ref : o.anim_units) {
            Object* unit = ref.machine->find(ref.node);
            if (unit == nullptr) continue;
            std::vector<ScriptEvent>& sink = sink_for(ref.machine, out);
            if (!(unit->ai == ai_state::kPlayAnimation && unit->animation == "IDLE")) {
                ref.machine->set_ai_play_animation(*unit, "IDLE", true, 0.1f, sink);
            }
            ref.machine->set_ai_state(*unit, ai_state::kNone, quests, sink);
        }
    }
    broadcast(o.id, kAnimationStopped, quests, out);
}

// `CAnimationPlayer::update`.
void LevelScript::animation_update(Object& o, float delta, QuestState& quests,
                                   std::vector<ScriptEvent>& out) {
    if (!o.anim_playing) return;
    if (o.anim_timed) {
        if (o.anim_remaining <= o.anim_blend_out) return;
        o.anim_remaining -= delta;
        if (o.anim_remaining <= o.anim_blend_out) animation_stop(o, false, quests, out);
        return;
    }
    animation_apply(o, out);
}

// `CCharacter::setAIPlayAnimation`.
void LevelScript::set_ai_play_animation(Object& unit, const std::string& animation, bool loops,
                                        float blend, std::vector<ScriptEvent>& out) {
    if (animation_length_ && animation_length_(unit.unit, animation) < 0.0f) {
        unit.animation.clear();
        return;
    }
    unit.ai = ai_state::kPlayAnimation;
    unit.animation = animation;
    unit.animation_loops = loops;
    unit.animation_left = animation_length_ ? animation_length_(unit.unit, animation) : 0.0f;

    if (Character* c = character(unit.id)) c->gait.clear();

    ScriptEvent e;
    e.action = ScriptAction::Animation;
    e.node_id = unit.id;
    e.name = unit.name;
    e.unit = unit.unit;
    e.text = animation;
    e.on = true;
    e.loops = loops;
    e.blend = blend;
    std::string upper = animation;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    e.then_idle = !loops && upper != "IDLE";
    out.push_back(e);
}

// `CCharacter::setAIState`.
void LevelScript::set_ai_state(Object& unit, int32_t state, QuestState& quests,
                               std::vector<ScriptEvent>& out) {
    unit.ai = state;
    if (state == ai_state::kIdle || unit.path == 0) return;
    Object* path = path_of(unit);
    if (path == nullptr) {
        set_path_to_follow(unit, 0, nullptr);
        return;
    }
    // A monster that is not looping its path, or any unit on a walk-to-player
    // path, ends it here (`setPathToFollow(0)`, vtable +0x310).
    if (path->path_to_player || !unit.path_loops) {
        path_reached(unit.path_machine, *path, quests, out);
        set_path_to_follow(unit, 0, nullptr);
    }
}

// ---------------------------------------------------------------------------
// Pathing (`CPathController`, `CCharacter::updatePathFollowing`)
// ---------------------------------------------------------------------------

// `CPathController::setEnabled`.
void LevelScript::path_set_enabled(Object& o, bool on, QuestState& quests,
                                   std::vector<ScriptEvent>& out) {
    if (o.enabled == on) return;
    o.enabled = on;
    broadcast(o.id, on ? kEnabled : kDisabled, quests, out);

    const bool player = o.path_group == "PLAYER" || o.unit == "PLAYER";
    if (!o.path_collected) {
        o.path_units = player ? std::vector<UnitRef>{UnitRef{}} : units_named(o.unit);
        o.path_collected = !o.path_units.empty();
    }
    for (const UnitRef& ref : o.path_units) {
        if (ref.player()) {
            // The player: `setPathToFollow`, `setAIState(2)`.
            if (on) {
                o.path_ended = false;
                if (o.path_to_player) o.position = frame_.point(player_at_);
                player_path_ = o.id;
                player_path_pct_ = -1.0f;
                player_path_forward_ = true;
                player_has_destination_ = false;
            } else if (player_path_ == o.id) {
                player_path_ = 0;
            }
            continue;
        }
        // The unit's own machine runs it, and its events are that machine's.
        LevelScript& runs = *ref.machine;
        std::vector<ScriptEvent>& sink = sink_for(ref.machine, out);
        Object* unit = runs.find(ref.node);
        if (unit == nullptr) continue;
        if (on) {
            o.path_ended = false;
            // A walk-to-player path moves itself onto the unit
            // (`setPosition(unit->getPosition(true))`), so it stands at the room's frame applied to
            // the unit's position.
            if (o.path_to_player) o.position = frame_.point(runs.position_of(*unit));
            // `setTarget` (none, or the player on a walk-to-player path),
            // `stopPathing`, `setTargetItem(0)` (a port monster holds no target
            // item), `setPathToFollow(this)`, `setAIState(2)`.
            if (Character* c = runs.character(ref.node)) {
                runs.set_target(*unit, *c, o.path_to_player, UnitRef{}, sink);
                runs.stop_pathing(*c);
            }
            runs.set_path_to_follow(*unit, o.id, this);
            runs.set_ai_state(*unit, ai_state::kIdle, quests, sink);
        } else if (unit->path == o.id && unit->path_machine == this) {
            runs.set_path_to_follow(*unit, 0, nullptr);
        }
        // `ENABLE ON START`: `CSceneNodeObject::setEnabled(true)` on the unit,
        // which only stores the flag (+0x82). It runs when the path is switched
        // off as well as on: the original's quirk, kept.
        if (o.path_enable_units && !unit->enabled) {
            unit->enabled = true;
            ScriptEvent e;
            e.action = ScriptAction::SetEnabled;
            e.node_id = unit->id;
            e.name = unit->name;
            e.on = true;
            sink.push_back(e);
        }
    }
}

// `CCharacter::setPathToFollowByName`: the first
// `Pathing` object in creation order whose `PATH NAME` equals it exactly; none clears.
void LevelScript::set_path_to_follow_by_name(Object& unit) {
    for (const Object& o : objects_) {
        if (o.type != type::kPathing || o.path_name != unit.path_name) continue;
        unit.path = o.id;
        unit.path_machine = this;
        return;
    }
    unit.path = 0;
    unit.path_machine = nullptr;
}

// `CMonster::setPathToFollow`, the same body as
// `CCharacter::setPathToFollow`: the cap restored, a
// path raising it to 800; route clock 10, percent -1.
void LevelScript::set_path_to_follow(Object& unit, int64_t path, LevelScript* machine) {
    Character* c = character(unit.id);
    if (c != nullptr) c->search_cap = c->saved_search_cap;
    unit.path = machine != nullptr ? path : 0;
    unit.path_machine = machine != nullptr && path != 0 ? machine : nullptr;
    if (unit.path_machine != nullptr && c != nullptr) {
        c->saved_search_cap = c->search_cap;
        c->search_cap = 800;
    }
    if (c != nullptr) c->since_route = 10.0f;
    unit.path_pct = -1.0f;
}

int32_t LevelScript::unit_search_cap(int64_t node) const {
    const Character* c = character(node);
    return c != nullptr ? c->search_cap : -1;
}

// `CPathController::getNextPointAtPercent`.
Vec3 LevelScript::path_point_at(const Object& path, float pct, bool forward,
                                float unit_radius) const {
    // `PATH TO PLAYER`: `CPathController::caculatePointInFrontOfPlayer`:
    // from the player towards the controller, twice the two radii out, at y 0.
    if (path.path_to_player) {
        float dx = path.position.x - player_at_.x;
        float dz = path.position.z - player_at_.z;
        const float length = std::sqrt(dx * dx + 0.0f + dz * dz);
        if (1e-08 < static_cast<double>(length)) {
            const float inv = 1.0f / length;
            dx = dx * inv;
            dz = dz * inv;
        }
        const float reach = player_radius_ + unit_radius;
        const float step = reach + reach;
        return Vec3{dx * step + player_at_.x, 0.0f, dz * step + player_at_.z};
    }
    if (path.points.empty()) return path.position;
    pct = pct <= 0.0f ? 0.0f : std::min(pct, 1.0f);
    auto fraction = [&](size_t i) {
        return path.path_total == 0.0f ? path.cumulative[i] : path.cumulative[i] / path.path_total;
    };
    if (!forward) {
        size_t i = path.points.size();
        do {
            --i;
        } while (i > 0 && pct < fraction(i));
        return path.points[i];
    }
    for (size_t i = 0; i < path.points.size(); ++i) {
        if (pct <= fraction(i)) return path.points[i];
    }
    return path.points.front();
}

// `CPathController::getClosestPctOfPath`. It
// measures the distance to every waypoint and then returns the first waypoint's
// fraction regardless, which is 0: the original's quirk, kept.
float LevelScript::path_closest_pct(const Object& path) const {
    if (path.path_to_player || path.points.size() == 1 || path.path_total == 0.0f) return 1.0f;
    return path.cumulative.front() / path.path_total;
}

// `CCharacter::updatePathFollowing(bool)`.
void LevelScript::path_follow(Object& unit, bool force, const Vec3& at, QuestState& quests,
                              std::vector<ScriptEvent>& out) {
    Character* c = character(unit.id);
    if (!force && !unit.enabled) {
        if (c != nullptr) stop_pathing(*c);
        unit.has_destination = false;
        return;
    }
    // The player's stand-in carries no machine: its path is this machine's.
    Object* path = path_of(unit);
    if (path == nullptr) return;

    bool set = false;
    if (unit.path_pct <= 0.0f) {
        unit.path_pct = unit.path_forward ? path_closest_pct(*path) : 0.95f;
        set = true;
    }
    // On the ground plane: both heights are zeroed before the test
    // (`movss %xmm2,0x54(%rsp)` with xmm2 = 0, and again).
    const float radius = c != nullptr ? c->motion.radius : player_radius_;
    const Vec3 point = path_point_at(*path, unit.path_pct, unit.path_forward, radius);
    const float dx = point.x - at.x;
    const float dz = point.z - at.z;
    if (!(2.0f < dx * dx + dz * dz)) {
        const float closest = path_closest_pct(*path);
        if (!unit.path_forward) {
            const float pct = std::min(closest, unit.path_pct);
            unit.path_pct = pct;
            if (0.0f <= pct) {
                unit.path_pct = pct - 0.05f;
                if (pct - 0.05f <= 0.05f) {
                    unit.path_pct = 0.0f;
                    if (unit.path_loops) unit.path_forward = true;
                }
                set = true;
            }
        } else {
            const float pct = std::max(closest, unit.path_pct);
            unit.path_pct = pct;
            if (!(1.0f < pct) || set) {
                unit.path_pct = pct + 0.05f;
                if (pct + 0.05f < 0.95f) {
                    set = true;
                } else {
                    unit.path_pct = 1.0f;
                    if (!unit.path_loops) {
                        set = true;
                        if (!path->path_ended) {
                            set = false;
                            path_reached(unit.path_machine, *path, quests, out);
                        }
                    } else {
                        unit.path_forward = false;
                        set = true;
                    }
                }
            }
        }
    }

    // on: a unit not moving takes a destination while its path runs;
    // otherwise only a step along the path gives it a new one.
    if (c != nullptr) {
        c->run = path->path_runs;
        if (!(!c->pathing && !path->path_ended) && !set) return;
        const Vec3 to = path_point_at(*path, unit.path_pct, unit.path_forward, radius);
        set_destination(*c, stats_for(unit.unit), to.x, to.z);
        if (!c->pathing && !path->path_ended) path_reached(unit.path_machine, *path, quests, out);
        return;
    }

    // The player (`CPlayer::update` runs this every frame): the port's walk
    // stands in for the player's `setDestination`.
    unit.running = path->path_runs;
    if (!(!unit.has_destination && !path->path_ended) && !set) return;

    // `setDestination` to the waypoint the percent names;
    // already in the goal's cell, the unit is not moving.
    unit.destination = path_point_at(*path, unit.path_pct, unit.path_forward, radius);
    unit.has_destination = distance_xz(unit.destination, at) > kNavCell * 0.5f;
    if (!unit.has_destination && !path->path_ended) {
        path->path_ended = true;
        broadcast(path->id, kEndOfPathReached, quests, out);
    }
}

// ---------------------------------------------------------------------------
// Level activation and the frame
// ---------------------------------------------------------------------------

// `CSoundObject::setSoundBankGuid`: the bank emptied and
// the definition added as slot 0 (`addSample`). Built on first use.
void LevelScript::resolve_sound_slot(Object& o) {
    if (o.sound_slot_resolved) return;
    o.sound_slot_resolved = true;
    sound_bank_clear(o.sound.bank, sound_host_);
    if (o.resource.empty() || !sound_slot_) return;
    BankSample slot;
    bool loops = false;
    if (!sound_slot_(o.resource, slot, loops)) return;
    if (slot.radius == 0.0f) slot.radius = o.sound.bank.radius;
    if (slot.volume == 0.0f) slot.volume = o.sound.bank.volume_scale;
    o.sound.bank.looping = loops;
    // A node that is not positional has its slot at radius 0 already when it
    // plays (`play` zeroes them all).
    o.sound.bank.slots.push_back(std::move(slot));
}

// PORT: a sound node's output raised for the renderer.
SoundOutput LevelScript::sound_output(Object& o, QuestState& quests,
                                      std::vector<ScriptEvent>& out) {
    const int64_t id = o.id;
    return [this, id, &quests, &out](uint32_t event) { broadcast(id, event, quests, out); };
}

// PORT: whether the machine holds sound nodes.
bool LevelScript::has_sound_nodes() const {
    for (const Object& o : objects_) {
        if (o.type == type::kSound) return true;
    }
    return false;
}

void LevelScript::level_deactivated(QuestState& quests, std::vector<ScriptEvent>& out) {
    for (Object& o : objects_) {
        // `CTimelineDescriptor::descriptorSceneDeactivated`:
        // every timeline
        // `CTimeline::SetEnabled(false)` (`+0xa2` set).
        if (o.type == type::kTimeline) o.enabled = false;
        if (o.type != type::kSound) continue;
        sound_object_scene_deactivated(o.sound, player_at_, sound_host_, volatile_random(),
                                       sound_output(o, quests, out));
    }
}

// `CUnitSpawnerDescriptor::descriptorSceneActivated`: the
// spawner works out its total; `SPAWN ON CREATE` releases at once, but with a remembered state
// (`CLevel+600`) only a `Particle` or `Missiles` spawner does.
void LevelScript::activate_spawner(Object& o, std::vector<ScriptEvent>& out) {
    if (!o.spawner) return;
    o.spawner->create_units_in_memory();
    const UnitSpawnerSpec& spec = o.spawner->spec();
    if (!spec.spawn_on_create) return;
    if (has_saved_state_ && spec.group != "Particle" && spec.group != "Missiles") return;
    spawner_input(o, kSpawnUnits, out);
}

// `CMonsterDescriptor::descriptorSceneActivated`: the character
// to `CLevel::addCharacter`, which drops it, then an authored `PATH` is handed over by
// name (`CCharacter::setPathToFollowByName`), no AI state change;
// `CMonster::idleAI` walks it.
void LevelScript::activate_monster(Object& o, std::vector<ScriptEvent>& out) {
    const int64_t id = o.id;
    drop_character_to_ground(o, out);
    if (Object* again = find(id); again != nullptr && !again->path_name.empty())
        set_path_to_follow_by_name(*again);
}

void LevelScript::level_activated(QuestState& quests, std::vector<ScriptEvent>& out) {
    // `CEditorScene::setActivated` runs once per scene.
    if (activated_) return;
    activated_ = true;
    // `CEditorScene::setActivated` runs each
    // descriptor's scene activation in the scene's descriptor order (`build_update_order`).
    for (int64_t id : update_order_) {
        Object* o = find(id);
        if (o == nullptr) continue;
        switch (o->type) {
            case type::kTimeline:
                // `CTimelineDescriptor::descriptorSceneActivated`
                o->tl_playing = o->tl_start_on_load;
                break;
            case type::kUnitSpawner: activate_spawner(*o, out); break;
            case type::kLayoutLink:
                // `CLayoutDescriptor::descriptorSceneActivated`: the linked
                // layout's `setActivated(true)` (vtable +0x108) before the later descriptors.
                if (link_activation_) link_activation_(id, out);
                break;
            case type::kSound:
                // The `Sound` descriptor's: a node that
                // starts on activation shown, enabled - which plays it - and
                // played.
                resolve_sound_slot(*o);
                o->sound.at = o->position;
                sound_object_scene_activated(o->sound, activation_listener_, sound_host_,
                                             volatile_random(), sound_output(*o, quests, out));
                break;
            case type::kGroup:
                // `CRandomGroupDescriptor::descriptorSceneActivated`:
                // an invisible group hides what is under it
                // (`CEditorScene::setChildrenVisible(id, false)`);
                // the group's own event reaches its descendants.
                if (!o->visible) {
                    ScriptEvent e;
                    e.action = ScriptAction::SetVisible;
                    e.node_id = o->id;
                    e.name = o->name;
                    e.on = false;
                    out.push_back(e);
                }
                break;
            case type::kMarker:
                // `CPropertyNodeDescriptor::descriptorSceneActivated` ->
                // `CPropertyNode::activate`: a `Jump Down Area`
                // is enabled whatever it authors.
                if (o->jump_down_area) {
                    o->jump_down_box = property_node_bounds(
                        o->position, o->right, o->up, o->forward,
                        o->marker_width != 0.0f ? o->marker_width : o->marker_radius,
                        o->marker_length != 0.0f ? o->marker_length : 1.0f);
                    property_node_set_enabled(*o, true, quests, out);
                }
                break;
            case type::kDamageShape:
                // `CDamageShapeDescriptor::descriptorSceneActivated`:
                // an enabled shape is switched off and on again.
                if (o->damage_shape && o->damage_shape->enabled) {
                    const int64_t id = o->id;
                    damage_shape_set_enabled(*o, false, quests, out);
                    if (Object* again = find(id))
                        damage_shape_set_enabled(*again, true, quests, out);
                }
                break;
            case type::kGameStateController:
                // `CGameStateControllerDescriptor::descriptorSceneActivated`
                // -> `CGameStateController::setInitialized`
                if (!o->gs_initialized) {
                    o->gs_initialized = true;
                    if (o->enabled) game_state_perform(*o, quests, out);
                }
                break;
            case type::kAnimationController:
                // `CAnimationPlayerDescriptor::descriptorSceneActivated`
                //
                if (o->anim_start_on_load) animation_play(*o, true, quests, out);
                break;
            case type::kMonster: activate_monster(*o, out); break;
            case type::kQuestController:
                // `CQuestControllerDescriptor::descriptorSceneActivated`
                // -> `CQuestController::initQuestController`: only for a
                // known quest, the controller registered (`CQuest::addQuestControllerListerner`)
                // and a `BROADCAST ON LOAD` one put on the update
                // list.
                if (!quest_known_ || quest_known_(o->quest)) {
                    if (o->qc_broadcast_on_load) add_level_update(o->id);
                }
                break;
            case type::kSkillController:
                // `CSkillControllerDescriptor::descriptorSceneActivated`:
                // outside the editor (bit 1) each controller's unit is
                // settled now.
                skill_configure(*o);
                break;
            case type::kPathing:
                // `CPathControllerDescriptor::descriptorSceneActivated` ->
                // `CPathController::initPathController`: a path starting enabled is
                // handed to its units (`setPathToFollow`, `setAIState(2)`) whatever `ENABLE ON
                // START` says.
                if (o->enabled) {
                    const int64_t path_id = o->id;
                    o->enabled = false;
                    if (Object* path = find(path_id)) path_set_enabled(*path, true, quests, out);
                }
                break;
            default: break;
        }
    }
    // `CLogicGroupDescriptor::levelActivated`.
    std::vector<int64_t> groups = logic_groups_;
    for (int64_t id : groups) broadcast(id, kLevelActivated, quests, out);
}

void LevelScript::step_units(float delta, const Vec3& player, QuestState& quests,
                             std::vector<ScriptEvent>& out) {
    // `CLevel::update` -> `updateCharacters`:
    // every character's `CCharacter::update`,
    // in the level's list order.
    update_characters(delta, quests, out);

    // The player's own path: `CPlayer::update`
    // runs `updatePathFollowing(true)` every
    // frame.
    if (player_path_ != 0) {
        Object* path = find(player_path_);
        if (path == nullptr) {
            player_path_ = 0;
        } else {
            Object stand_in;
            stand_in.path = player_path_;
            stand_in.path_pct = player_path_pct_;
            stand_in.path_forward = player_path_forward_;
            stand_in.has_destination = player_has_destination_;
            stand_in.destination = player_destination_;
            stand_in.enabled = true;
            const Vec3 was = player_destination_;
            const bool had = player_has_destination_;
            path_follow(stand_in, true, player, quests, out);
            player_path_pct_ = stand_in.path_pct;
            player_path_forward_ = stand_in.path_forward;
            player_has_destination_ = stand_in.has_destination;
            player_destination_ = stand_in.destination;
            if (player_has_destination_ &&
                (!had || distance_squared(was, player_destination_) > 1e-6f)) {
                ScriptEvent e;
                e.action = ScriptAction::WalkPlayer;
                e.node_id = path->id;
                e.name = path->name;
                e.position = player_destination_;
                e.running = path->path_runs;
                out.push_back(e);
            }
            if (path->path_ended) player_path_ = 0;
        }
    }

    (void)player;
}

// `CLevel::updateNearUnitList`: the near list rebuilt,
// every character measured again (`calculateActiveRange`); one
// past `UNIT_NEAR_RANGE` hidden at once (`setVisible(false, true)`)
// and left off.
void LevelScript::update_near_unit_list(const Vec3& view, std::vector<ScriptEvent>& out) {
    near_list_built_ = true;
    // PORT: the player is added to the level's list when it enters, after the
    // load made what it made (`CLevel::addCharacter`, at the
    // head): taken as the machine's character count at its first rebuild.
    if (player_joined_ == static_cast<size_t>(-1)) player_joined_ = character_order_.size();
    size_t listed = 0;
    size_t active = 0;
    for (int64_t id : character_order_) {
        Object* o = find(id);
        Character* c = character(id);
        if (o == nullptr || c == nullptr) continue;
        calculate_active_range(*o, *c, view, out);
        c->listed = c->in_near_range;
        if (!c->in_near_range) set_character_visible(*o, *c, false, true, out);
        listed += c->listed ? 1 : 0;
        active += c->active ? 1 : 0;
    }
    near_counts_ = {listed, active};
    near_total_ = character_order_.size();
}

// `CCharacter::calculateActiveRange` on a
// character this machine runs; `core::calculate_active_range` is the
// arithmetic.
void LevelScript::calculate_active_range(const Object& o, Character& c, const Vec3& view,
                                         std::vector<ScriptEvent>& out) {
    ActiveRangeInput in;
    const UnitStats& stats = stats_for(o.unit);
    const Vec3 at = position_of(o);
    in.distance =
        std::sqrt((view.x - at.x) * (view.x - at.x) + 0.0f + (view.z - at.z) * (view.z - at.z));
    // Active wherever it stands: a `BOSS_CHAMPION` (`+0x772`), a character
    // whose master is active - the player always is - or one dying (state 5).
    // `ISA 0x1c` is the player, which no machine runs.
    bool master_active = c.master_player;
    if (!master_active && c.has_master && c.master.machine != nullptr) {
        const Character* master = c.master.machine->character(c.master.node);
        master_active = master != nullptr && master->active;
    }
    in.forced = stats.boss_champion || master_active || o.ai == ai_state::kDying;
    // The current skill (`+0x398`) still casting (`core::still_casting`), or
    // any skill on the manager's running list (`+0x1c8`, `+0x50`).
    if (const UnitSkill* held = active_unit_skill(c)) {
        const SkillRunState* state = c.casting.runner.state(held->known.entry.name);
        in.casting =
            still_casting(c.casting.cast, state != nullptr ? state->animations : SkillAnimations{},
                          c.casting.runner.is_running(held->known.entry.name));
    }
    in.skills_running = c.casting.runner.any_running();
    in.unit_active_range = unit_active_range_;
    in.unit_near_range = unit_near_range_;
    // Effect 0x2c, a unit's own passive `PERCENT ACTIVE DISTANCE BONUS`, and
    // the template's `ACTIVERANGEMOD`, which a unit in AI state 5 or 6 does
    // not get.
    in.active_distance_bonus = stats.active_distance_bonus;
    if (o.ai != 5 && o.ai != 6) in.active_range_mod = active_range_mod_;
    in.master_is_player = c.master_player;
    in.think_index = c.think_index;
    in.unit_shadows = stats.shadows;
    in.shadows_enabled = shadows_enabled_;
    in.unit_shadow_range = unit_shadow_range_;
    in.height_difference = view.y - c.motion.position.y;
    in.ai_state = o.ai;
    const ActiveRange r = tl::core::calculate_active_range(in);
    c.active = r.active;
    c.in_near_range = r.in_near_range;
    c.think_index = r.think_index;
    if (c.casts_shadow != r.casts_shadow) {
        // What `updateAnimation` hands the
        // model and the worn pieces (`CGenericModel::setCastsShadows`).
        c.casts_shadow = r.casts_shadow;
        ScriptEvent e;
        e.action = ScriptAction::CastShadows;
        e.node_id = o.id;
        e.name = o.name;
        e.on = r.casts_shadow;
        out.push_back(e);
    }
    if (c.active) {
        inactive_.erase(o.id);
    } else {
        inactive_.insert(o.id);
    }
}

// `CCharacter::setVisible(bool, bool)`. Its
// carried particles start and stop with the node's `SetVisible` event.
void LevelScript::set_character_visible(Object& o, Character& c, bool on, bool immediate,
                                        std::vector<ScriptEvent>& out) {
    const bool show = on && !c.mesh_hidden;
    if (show == o.visible) return;
    if (!immediate) {
        // vtable +0x50, `setVisible(bool)`: only the fade's goal.
        want_visible(c, show);
        return;
    }
    c.want_visible = show;
    if (!show && c.opacity != 0.0f) {
        c.opacity = 0.0f;
        step_opacity(o, c, 0.0f, true, out);
        if (o.visible == show) return;
    }
    // `CSceneNodeObject::setVisible`, `+0x81`.
    o.visible = show;
    ScriptEvent e;
    e.action = ScriptAction::SetVisible;
    e.node_id = o.id;
    e.name = o.name;
    e.on = show;
    out.push_back(e);
}

// `CCharacter::updateOpacity`;
// `core::update_opacity` is the arithmetic. A body flung over an abyss
// (`+0x681`) fades four times as fast once 0.6 of its flight is behind it.
void LevelScript::step_opacity(Object& o, Character& c, float delta, bool force,
                               std::vector<ScriptEvent>& out) {
    const bool falls = c.flown && c.flight.falls;
    const float length = c.flown ? c.flight.path.length() : 0.0f;
    const float progress = 0.0f < length ? c.flight_distance / length : 0.0f;
    const OpacityStep step = update_opacity(c.opacity, c.want_visible, c.in_near_range, o.visible,
                                            falls, progress, delta, force);
    c.opacity = step.opacity;
    if (step.written) {
        ScriptEvent e;
        e.action = ScriptAction::Opacity;
        e.node_id = o.id;
        e.name = o.name;
        e.amount = c.opacity;
        out.push_back(e);
    }
    if (step.show == OpacityStep::Show::Hide) {
        set_character_visible(o, c, false, true, out);
    } else if (step.show == OpacityStep::Show::Show) {
        set_character_visible(o, c, true, true, out);
        // Shown at the opacity the step left: the event above came first.
        if (!step.written) {
            ScriptEvent e;
            e.action = ScriptAction::Opacity;
            e.node_id = o.id;
            e.name = o.name;
            e.amount = c.opacity;
            out.push_back(e);
        }
    }
}

float LevelScript::unit_opacity(int64_t id) const {
    const Character* c = character(id);
    return c != nullptr ? c->opacity : 1.0f;
}

namespace {
/// `CLevel::update(...)::UpdateTimer`.
float g_near_list_timer = 0.0f;
}  // namespace

// PORT: the near list's timer cleared (a new run of the program).
void LevelScript::reset_near_list_timer() { g_near_list_timer = 0.0f; }

// `CLevel::update`: past one second of `UpdateTimer`,
// `CLevel::updateNearUnitList`, then the items' half.
// The timer is a function static, carried over between levels; only the level's first
// machine advances it. The vector is the camera's aim point.
void LevelScript::step_near_unit_lists(float delta, std::vector<ScriptEvent>& out) {
    if (level_ != nullptr && !level_->empty() && level_->front() != this) return;
    g_near_list_timer += delta;
    if (!(1.0f < g_near_list_timer)) return;
    std::pair<size_t, size_t> counts{0, 0};
    size_t total = 0;
    const Vec3 look = level_camera_look_at();
    for (LevelScript* machine : level_machines()) {
        machine->update_near_unit_list(look, sink_for(machine, out));
        counts.first += machine->near_counts_.first;
        counts.second += machine->near_counts_.second;
        total += machine->near_total_;
    }
    if (item_near_list_) item_near_list_();
    g_near_list_timer = 0.0f;
    if (counts != near_logged_) {
        near_logged_ = counts;
        TL_LOG_INFO("script: near list %zu of %zu character(s), %zu active", counts.first, total,
                    counts.second);
    }
}

// PORT: whether any machine of the level holds a dialog or a modal open.
bool LevelScript::level_paused() const {
    for (const LevelScript* machine : level_machines()) {
        if (machine->dialog_open() || machine->modal_open_) return true;
    }
    return modal_open_;
}

// PORT: whether every machine of the level lets the player act.
bool LevelScript::level_player_enabled() const {
    for (const LevelScript* machine : level_machines()) {
        if (!machine->player_enabled_) return false;
    }
    return true;
}

// `CGameUI::getUIIsInCinematic` over the whole level: a
// controller holding the camera, the player disabled, or the player on a scripted path.
// `+0x10bc` is `client_paused()`; the menus are the scene's (`set_ui_cinematic`).
bool LevelScript::ui_in_cinematic() const {
    if (g_client_paused || ui_cinematic_ || level_camera_taken() || !level_player_enabled()) {
        return true;
    }
    for (const LevelScript* machine : level_machines()) {
        if (machine->player_path_ != 0 && player_moving_) return true;
    }
    return false;
}

// PORT: whether any machine of the level holds the camera.
bool LevelScript::level_camera_taken() const {
    for (const LevelScript* machine : level_machines()) {
        if (machine->camera_holder_ != 0) return true;
    }
    return false;
}

// PORT: where the level's camera looks, for the near list.
Vec3 LevelScript::level_camera_look_at() const {
    for (const LevelScript* machine : level_machines()) {
        if (machine->camera_holder_ != 0) return machine->camera_look_at_;
    }
    return camera_look_at_;
}

bool LevelScript::update_paused_speaker(const std::string& unit, float frame_delta,
                                        QuestState& quests, std::vector<ScriptEvent>& out) {
    if (unit.empty()) return false;
    const std::string wanted = quest_upper(unit);
    for (Object& o : objects_) {
        if (o.type != type::kMonster) continue;
        if (quest_upper(o.unit) != wanted && quest_upper(o.name) != wanted) continue;
        Character* c = character(o.id);
        if (c == nullptr || dead_.count(o.id) != 0) continue;
        // The frame's own time (0x58e7xx), not the level's: the
        // game speed is `CLevel::update`'s, which does not run.
        character_update(o, *c, frame_delta, quests, out);
        return true;
    }
    return false;
}

void LevelScript::update_units(float frame_delta, const Vec3& player, QuestState& quests,
                               std::vector<ScriptEvent>& out) {
    if (level_paused()) return;
    // `CLevel::update`: elapsed times the
    // game speed.
    const float delta = frame_delta * game_speed_;
    player_at_ = player;
    if (camera_holder_ == 0) camera_look_at_ = player;
    step_near_unit_lists(delta, out);
    update_characters(delta, quests, out);
}

void LevelScript::update(float frame_delta, const Vec3& player, QuestState& quests,
                         std::vector<ScriptEvent>& out) {
    // The level's time: `CLevel::update` scales by the game
    // speed, and `updateIngame` hands `CLevel::updateLayouts` the same. The dialog
    // window runs on the frame's own time.
    const float delta = frame_delta * game_speed_;
    // What another machine raised for this one's nodes (a Damage Shape's
    // instant pass reaching a character here).
    if (!outbox_.empty()) {
        out.insert(out.end(), outbox_.begin(), outbox_.end());
        outbox_.clear();
    }
    // `CGameClient::getIsPaused`: while the quest dialog is
    // up, `updateIngame` runs neither `CLevel::update` nor
    // `updateLayouts`. Only the interface updates.
    if (dialog_open()) {
        update_dialog_ui(frame_delta, quests, out);
        return;
    }
    if (level_paused()) return;
    clock_ += delta;
    player_at_ = player;
    if (camera_holder_ == 0) camera_look_at_ = player;

    // `CLevel::update`: the near-unit list
    // once its timer passes a second, characters, then the `iLevelUpdate` list.
    step_near_unit_lists(delta, out);
    step_units(delta, player, quests, out);
    // `CLevel::updateLevelUpdateObjects`: the count is
    // re-read every step; one returning false leaves by swap-with-last, its slot revisited.
    for (size_t i = 0; i < level_updates_.size();) {
        const int64_t id = level_updates_[i];
        bool stays = false;
        if (Object* o = find(id)) {
            if (o->type == type::kQuestController) {
                quest_level_update(*o, quests, out);  // always returns 0
            } else if (o->type == 46) {
                stays = puzzle_level_update(*o, quests, out);
            } else if (o->type == type::kCinematic) {
                cinematic_level_update(*o, quests, out);  // returns 0
            }
        }
        if (!stays && i < level_updates_.size() && level_updates_[i] == id) {
            level_updates_[i] = level_updates_.back();
            level_updates_.pop_back();
            continue;
        }
        ++i;
    }

    update_dialog_ui(frame_delta, quests, out);

    // `CLevel::updateLayouts`: the
    // descriptors in order. Every type this switch acts on is in `updates_each_frame`.
    for (size_t i = 0; i < frame_order_.size(); ++i) {
        Object* o = find(frame_order_[i]);
        if (o == nullptr) continue;
        switch (o->type) {
            case type::kTimeline:
                // `CTimelineDescriptor::update`:
                // `CTimeline::UpdateTimeline` on each timeline.
                timeline_update(*o, delta, quests, out);
                break;
            case type::kSound:
                // `CSoundObjectDescriptor::update`:
                // `updateSounds` on each sound object.
                o->sound.at = o->position;
                sound_object_update(o->sound, delta, player_at_, sound_host_, volatile_random(),
                                    sound_output(*o, quests, out));
                break;
            case type::kPlayerSphereTrigger:
            case type::kPlayerBoxTrigger:
                // `CTriggerDescriptor::update`:
                // each `CLogicTrigger`'s own update (vtable
                // `+0x1d8`) with the frame time and the scene.
                trigger_update(*o, player, quests, out);
                break;
            case type::kUnitTrigger:
                // `CUnitTriggerDescriptor::update` does nothing; this is the
                // headless walks' DIVERGENCE (`enable_proximity_unit_triggers`).
                if (proximity_unit_triggers_ &&
                    !(proximity_exempt_ && proximity_exempt_(o->unit))) {
                    trigger_update(*o, player, quests, out);
                }
                break;
            case type::kTimer:
                // `CLogicTimerDescriptor::update`:
                // `CLogicTimer::update` on each
                // timer.
                timer_update(*o, delta, quests, out);
                break;
            case type::kCameraController:
                // `CCameraControllerDescriptor::update`:
                //
                // `CCameraController::update` on each.
                camera_update(*o, delta, quests, out);
                break;
            case type::kAnimationController:
                // `CAnimationPlayerDescriptor::update`:
                // `CAnimationPlayer::update` on each.
                animation_update(*o, delta, quests, out);
                break;
            case type::kSkillController:
                // `CSkillControllerDescriptor::update`.
                //
                skill_update(*o, quests, out);
                break;
            case type::kSkipCutscene:
                // `CSkipCutsceneDescriptor::update`:
                // `CSkipCutscene::update` on each.
                skip_update(*o, quests, out);
                break;
            case type::kGameStateController: {
                // `CGameStateControllerDescriptor::update`
                // -> `CGameStateController::update`
                const float curr_hp = static_cast<float>(static_cast<uint32_t>(player_hp_));
                if (curr_hp != o->gs_prev_hp) {
                    const float max_hp = static_cast<float>(static_cast<uint32_t>(player_max_hp_));
                    const float delta = curr_hp - o->gs_prev_hp;
                    std::vector<uint32_t> fired;
                    broadcast_player_hp_events(max_hp, o->gs_prev_hp, delta, fired);
                    for (uint32_t ev : fired) {
                        broadcast(o->id, ev, quests, out);
                    }
                    o->gs_prev_hp = curr_hp;
                }
                break;
            }
            case type::kDamageShape: damage_shape_update(*o, delta, quests, out); break;
            default: break;
        }
    }
}

// ---------------------------------------------------------------------------
// Spawners
// ---------------------------------------------------------------------------

// PORT: a layout input to a `Unit Spawner` node handed to the renderer, which runs `UnitSpawner::input`.
void LevelScript::spawner_input(Object& o, uint32_t in, std::vector<ScriptEvent>& out) {
    if (!o.spawner) return;
    ScriptEvent e;
    e.action = ScriptAction::SpawnerInput;
    e.node_id = o.id;
    e.name = o.name;
    e.position = o.position;
    e.spawner = o.spawner.get();
    e.input = in;
    e.machine = this;
    out.push_back(e);
}

// PORT: a spawner node by id.
UnitSpawner* LevelScript::unit_spawner(int64_t id) {
    Object* o = find(id);
    return o != nullptr && o->type == type::kUnitSpawner ? o->spawner.get() : nullptr;
}

// PORT: the machine's spawner nodes.
std::vector<std::pair<int64_t, UnitSpawner*>> LevelScript::unit_spawners() {
    std::vector<std::pair<int64_t, UnitSpawner*>> out;
    for (Object& o : objects_) {
        if (o.type == type::kUnitSpawner && o.spawner) out.emplace_back(o.id, o.spawner.get());
    }
    return out;
}

// PORT: an output raised on a node from outside (the renderer).
void LevelScript::raise_output(int64_t id, uint32_t output, QuestState& quests,
                               std::vector<ScriptEvent>& out) {
    if (find(id) == nullptr) return;
    broadcast(id, output, quests, out);
}

// ---------------------------------------------------------------------------
// Dialog presentation
// ---------------------------------------------------------------------------

const std::string& LevelScript::dialog_text() const {
    static const std::string kNone;
    if (dialogs_.empty() || !showing_) return kNone;
    return dialogs_.front().text;
}

// `CQuestDialogMenu::setNPC(unit, false, quest)` for a
// boxed line; with no line for the unit, `getDialogsForNPC`
// takes the first of the player's quests with one.
void LevelScript::open_dialog(Object& o, const DialogLine& line_ref, const std::string& quest,
                              QuestSection section, const UnitRef& unit,
                              std::vector<ScriptEvent>& out) {
    const DialogLine* line = &line_ref;
    PendingDialog pending;
    pending.node = o.id;
    pending.name = o.name;
    pending.unit = o.unit.empty() ? line->unit : o.unit;
    // INVENTED: QuestEntry is not reachable from LevelScript, so pass nullptr.
    const std::string picked = get_dialog(*line, nullptr, game_random());
    pending.text = picked;
    pending.sound = line->sound;
    pending.quest = quest;
    pending.menu_unit = unit;
    pending.section = section;
    // `Accept` and `Decline` for an `[INTRO]` (`CQuestDialog +0x70` 1), a single
    // `OK` otherwise.
    pending.offer = section == QuestSection::Intro;
    pending.camera_mod = Vec3{line->camera_mod_x, line->camera_mod_y, line->camera_mod_z};
    // `CGameUI::forceInteractiveDialog`: `setTarget` for a
    // character; a prop goes to `setTargetItem`.
    if (o.type == type::kMonster) {
        pending.speaker_node = o.id;
    } else if (UnitRef speaker; base_unit_named(o.unit, speaker) && !speaker.player()) {
        const Object* unit = speaker.machine->find(speaker.node);
        if (unit != nullptr && unit->type == type::kMonster) pending.speaker_node = speaker.node;
    }
    // DIVERGENCE: the original waits for the player; a player who is not
    // watching (a headless run, the console) gets the recording's length.
    pending.remaining = DialogBook::box_time(picked, line->seconds);
    dialogs_.push_back(std::move(pending));
    show_front_dialog(out);
}

// PORT: the first queued dialog handed to the renderer, its menu unit held.
void LevelScript::show_front_dialog(std::vector<ScriptEvent>& out) {
    if (showing_ || dialogs_.empty()) return;
    showing_ = true;
    const PendingDialog& pending = dialogs_.front();
    hold_menu_unit(pending.menu_unit);
    ScriptEvent e;
    e.action = ScriptAction::Dialog;
    e.node_id = pending.node;
    e.name = pending.name;
    e.unit = pending.unit;
    e.text = pending.text;
    e.sound = pending.sound;
    e.quest = pending.quest;
    e.offer = pending.offer;
    e.camera_mod = pending.camera_mod;
    e.speaker_node = pending.speaker_node;
    e.seconds = pending.remaining;
    e.on = true;
    out.push_back(e);
}

void LevelScript::show_floaty(const Object& o, const DialogLine& line,
                              std::vector<ScriptEvent>& out) {
    clear_floaty(out);
    floaty_.node = o.id;
    floaty_.name = o.name;
    floaty_.unit = o.unit.empty() ? line.unit : o.unit;
    floaty_.remaining = kFloatySeconds;
    floaty_.showing = true;

    ScriptEvent e;
    e.action = ScriptAction::Dialog;
    e.node_id = floaty_.node;
    e.name = floaty_.name;
    e.unit = floaty_.unit;
    // INVENTED: QuestEntry is not reachable from LevelScript, so pass nullptr.
    const std::string picked = get_dialog(line, nullptr, game_random());
    e.text = picked;
    e.sound = line.sound;
    e.seconds = floaty_.remaining;
    e.floaty = true;
    e.on = true;
    out.push_back(e);
}

// `CGameUI::update`: the dialog on screen; a floaty line's menu closes the frame after it
// opened (`CQuestDialogMenu::update`, `onClick(10)`).
void LevelScript::update_dialog_ui(float delta, QuestState& quests, std::vector<ScriptEvent>& out) {
    if (floaty_.menu_event_pending) {
        floaty_.menu_event_pending = false;
        if (Object* o = find(floaty_.node); o != nullptr && o->qc_listening) {
            controller_menu_event(*o, 2, out);
        }
        unit_menu_closed(floaty_.menu_unit, 2, floaty_.section == QuestSection::Complete,
                         floaty_.quest, quests, out);
    }
    show_front_dialog(out);
    // DIVERGENCE: with nobody to read it (a headless run) a box closes itself
    // after its reading time, and an offer is accepted.
    if (!dialogs_wait_ && showing_ && !dialogs_.empty()) {
        dialogs_.front().remaining -= delta;
        if (dialogs_.front().remaining <= 0.0f)
            close_dialog(dialogs_.front().offer ? 3 : 2, quests, out);
    }
    if (floaty_.showing) {
        floaty_.remaining -= delta;
        if (floaty_.remaining <= 0.0f) clear_floaty(out);
    }
}

void LevelScript::clear_floaty(std::vector<ScriptEvent>& out) {
    if (!floaty_.showing) return;
    floaty_.showing = false;
    ScriptEvent e;
    e.action = ScriptAction::Dialog;
    e.node_id = floaty_.node;
    e.name = floaty_.name;
    e.unit = floaty_.unit;
    e.floaty = true;
    e.on = false;
    out.push_back(e);
}

// The dialog closing is `CQuestController::menuEventOccured`: the
// controller joins the level's update list, and `Interacted` fires on the next
// level update.
void LevelScript::close_dialog(int32_t menu_event, QuestState& quests,
                               std::vector<ScriptEvent>& out) {
    if (dialogs_.empty() || !showing_) return;
    const PendingDialog done = dialogs_.front();
    dialogs_.erase(dialogs_.begin());
    showing_ = false;

    ScriptEvent e;
    e.action = ScriptAction::Dialog;
    e.node_id = done.node;
    e.name = done.name;
    e.unit = done.unit;
    e.quest = done.quest;
    e.offer = done.offer;
    e.menu_event = menu_event;
    e.on = false;
    out.push_back(e);

    // `onClick`: the unit's flag back first; the menu event
    // to the listening controller (accepting sends 3 twice, `onClick` and `acceptQuest`);
    // then the broadcasts on the unit.
    if (menu_event == kCloseButtonAccepts) {
        // `handle_CloseButton`: the unit's broadcasts first, the listener hearing 3 then
        // 1 (kept, so plain `Interacted`), and the unit let go last
        // (`CDropdownMenu::handle_CloseButton` -> `setOpen(false)`).
        unit_menu_closed(done.menu_unit, menu_event, false, done.quest, quests, out);
        if (Object* o = find(done.node); o != nullptr && o->qc_listening) {
            controller_menu_event(*o, 3, out);
            controller_menu_event(*o, 1, out);
        }
        release_menu_unit(done.menu_unit);
        show_front_dialog(out);
        return;
    }
    release_menu_unit(done.menu_unit);
    if (Object* o = find(done.node); o != nullptr && o->qc_listening) {
        controller_menu_event(*o, menu_event, out);
    }
    unit_menu_closed(done.menu_unit, menu_event, done.section == QuestSection::Complete, done.quest,
                     quests, out);
    show_front_dialog(out);
}

// PORT: the renderer's interface to the dialog.
void LevelScript::close_button_dialog(QuestState& quests, std::vector<ScriptEvent>& out) {
    close_dialog(kCloseButtonAccepts, quests, out);
}

// PORT: the renderer's interface to the dialog.
void LevelScript::dismiss_dialog(QuestState& quests, std::vector<ScriptEvent>& out) {
    close_dialog(2, quests, out);
}

// PORT: the renderer's interface to the dialog.
void LevelScript::answer_dialog(bool accept, QuestState& quests, std::vector<ScriptEvent>& out) {
    close_dialog(accept ? 3 : 4, quests, out);
}

// ---------------------------------------------------------------------------
// Units placed and removed from outside
// ---------------------------------------------------------------------------

void LevelScript::add_spawned_unit(int64_t id, const std::string& unit, const Vec3& at, bool hunts,
                                   float facing, const UnitCreation* creation) {
    if (id == 0 || unit.empty()) return;
    if (find(id) != nullptr) return;

    Object o;
    o.id = id;
    o.type = type::kMonster;
    o.name = unit;
    o.unit = unit;
    o.spawned = true;
    o.position = at;
    index_.emplace(id, objects_.size());
    objects_.push_back(std::move(o));
    Character& c = make_character(objects_.back(), at, facing, creation);
    if (hunts) {
        // `TARGET PLAYER`: CUnitSpawner::spawnUnitByIndex
        // adds MONSTER_HUNT,
        // AI flag 4 (NO LINE OF SIGHT, 20.0s) and setTarget(player).
        c.hunt_affix = true;
        c.ai_flags.add(AIFlag::NoLineOfSight, 20.0f);
        std::vector<ScriptEvent> ignored;
        set_target(objects_.back(), c, true, UnitRef{}, ignored);
    }
}

bool LevelScript::make_unit_champion(int64_t id) {
    Character* c = character(id);
    if (c == nullptr) return false;
    static const StatBook kNoGraphs;
    if (!make_champion(c->combat, stats_for(c->unit), difficulty_,
                       graphs_ != nullptr ? *graphs_ : kNoGraphs, volatile_random())) {
        return false;
    }
    // vtable +0x48 with `+0x84 + 0.2`.
    c->scale = c->scale + 0.2f;
    return true;
}

// `CFormationNode::updateLevelObject`: each
// member gets every other member in its formation list (all-to-all peer
// topology).
void LevelScript::link_formation(const std::vector<int64_t>& unit_ids) {
    for (size_t i = 0; i < unit_ids.size(); ++i) {
        Character* c = character(unit_ids[i]);
        if (c == nullptr) continue;
        for (size_t j = 0; j < unit_ids.size(); ++j) {
            if (i != j) {
                c->ai_flags.add_formation_unit(unit_ids[j]);
            }
        }
    }
}

// PORT: a unit put at a point by the renderer.
void LevelScript::move_unit(int64_t id, const Vec3& at) {
    Character* c = character(id);
    if (c == nullptr) return;
    const bool was = c->on_grid;
    avoidance_remove(*c);
    c->motion.position = {at.x, at.y + c->motion.radius, at.z};
    if (was) avoidance_add(*c);
    if (Object* o = find(id)) o->position = at;
}

// PORT: the level machine's own bookkeeping: an accessor.
bool LevelScript::node_enabled(int64_t id) const {
    const Object* o = find(id);
    return o == nullptr || o->enabled;
}

// PORT: the level machine's own bookkeeping: an accessor.
bool LevelScript::node_targetable(int64_t id) const {
    const Object* o = find(id);
    return o == nullptr || o->targetable;
}

// PORT: the level machine's own bookkeeping: an accessor.
float LevelScript::timer_left(int64_t id) const {
    const Object* o = find(id);
    return (o != nullptr && o->type == type::kTimer) ? o->timer_left : -1.0f;
}

// PORT: the level machine's own bookkeeping: an accessor.
bool LevelScript::timer_reset(int64_t id) {
    Object* o = find(id);
    if (o == nullptr || o->type != type::kTimer) return false;
    timer_reset(*o);
    return true;
}

// PORT: a headless verification step: a `Random Choice` node sent `ROLL`.
bool LevelScript::roll_random_choice(int64_t id, QuestState& quests,
                                     std::vector<ScriptEvent>& out) {
    Object* o = find(id);
    if (o == nullptr || o->type != 39) return false;
    input(id, logic::kRoll, quests, out);
    return true;
}

// PORT: a diagnostic of a puzzle node.
uint32_t LevelScript::puzzle_active_inputs(int64_t id) const {
    const Object* o = find(id);
    return (o != nullptr && o->type == 46) ? o->puzzle_active_inputs : 0;
}

// PORT: a diagnostic of a puzzle node.
uint32_t LevelScript::puzzle_order_at(int64_t id, size_t index) const {
    const Object* o = find(id);
    return (o != nullptr && o->type == 46 && index < 6) ? o->puzzle_order[index] : 0;
}

// PORT: a diagnostic of a puzzle node.
bool LevelScript::puzzle_solved(int64_t id) const {
    const Object* o = find(id);
    return (o != nullptr && o->type == 46) ? o->puzzle_solved : false;
}

// PORT: an input sent to a puzzle node from outside.
bool LevelScript::send_puzzle_input(int64_t id, uint32_t in, QuestState& quests,
                                    std::vector<ScriptEvent>& out) {
    Object* o = find(id);
    if (o == nullptr || o->type != 46) return false;
    input(id, in, quests, out);
    return true;
}

// PORT: an input sent to a damage shape node from outside.
bool LevelScript::send_damage_shape_input(int64_t id, uint32_t in, QuestState& quests,
                                          std::vector<ScriptEvent>& out) {
    Object* o = find(id);
    if (o == nullptr || o->type != type::kDamageShape) return false;
    input(id, in, quests, out);
    return true;
}

// `CLevel::addCharacter`, its last call:
// `CCharacter::dropToGround (4.0, false, level)`.
// (`CMonster::updateAI`: every monster is a character with an AI.)
void LevelScript::drop_characters_to_ground(std::vector<ScriptEvent>& out) {
    for (Object& o : objects_) {
        if (o.type == type::kMonster) drop_character_to_ground(o, out);
    }
}

void LevelScript::drop_character_to_ground(Object& o, std::vector<ScriptEvent>& out) {
    if (collision_ == nullptr || collision_->empty()) return;
    Character* c = character(o.id);
    if (c == nullptr || dead_.count(o.id) != 0) return;
    const bool was = c->on_grid;
    avoidance_remove(*c);
    drop_to_ground_open(*collision_, c->motion, 4.0f, [this](const Vec3& at, float radius) {
        return random_open_position(
            at, 0.0f, radius,
            [this](const Vec3& p) {
                return passability_ == nullptr || !passability_->ready() ||
                       (passability_->map_passable(p) && passability_->position_passable(p));
            },
            volatile_random());
    });
    if (was) avoidance_add(*c);
    const Vec3 at = feet_of(*c);
    o.position = at;
    if (std::fabs(at.x - c->sent_at.x) > 1e-5f || std::fabs(at.y - c->sent_at.y) > 1e-5f ||
        std::fabs(at.z - c->sent_at.z) > 1e-5f) {
        c->sent_at = at;
        ScriptEvent e;
        e.action = ScriptAction::MoveUnit;
        e.node_id = o.id;
        e.unit = o.unit;
        e.position = at;
        e.facing = c->facing;
        out.push_back(e);
    }
}

// PORT: a diagnostic count of the placed units that take a side.
size_t LevelScript::arm_hostiles() {
    size_t armed = 0;
    for (const Object& o : objects_) {
        if (o.type != type::kMonster || o.unit.empty() || !o.visible || !o.targetable) continue;
        const Character* c = character(o.id);
        if (c == nullptr) continue;
        if (stats_for(o.unit).alignment != UnitStats::Alignment::Neutral) ++armed;
    }
    return armed;
}

void LevelScript::unit_killed(int64_t id, QuestState& quests, std::vector<ScriptEvent>& out) {
    if (dead_.count(id) != 0) return;
    dead_.insert(id);
    if (Object* o = find(id)) {
        // `CCharacter::die`: state 5, then 6 after the fall.
        // `setAIState(5)` ends a walk-to-player or non-looping path; a looping one stays.
        set_ai_state(*o, ai_state::kDying, quests, out);
        o->ai = ai_state::kDead;
        o->targetable = false;
    }
    if (Character* c = character(id)) {
        stop_pathing(*c);
        avoidance_remove(*c);
        c->has_target = false;
    }
    broadcast(id, kMonsterKilled, quests, out);
}

// PORT: the level machine's own bookkeeping: an accessor.
bool LevelScript::unit_is_dead(int64_t id) const { return dead_.count(id) != 0; }

// PORT: a spawned unit's node and record removed from the machine.
void LevelScript::remove_spawned_unit(int64_t id) {
    if (Character* c = character(id)) avoidance_remove(*c);
    forget_character(id);
    const auto it = index_.find(id);
    if (it == index_.end()) return;
    objects_.erase(objects_.begin() + static_cast<std::ptrdiff_t>(it->second));
    index_.clear();
    for (size_t i = 0; i < objects_.size(); ++i) index_.emplace(objects_[i].id, i);
}

}  // namespace tl::core
