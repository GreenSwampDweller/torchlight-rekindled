// The AI states a level script reads or sets (core/script.h's `LevelScript`).
#pragma once
#include <cstdint>

namespace tl::core {

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

}  // namespace tl::core
