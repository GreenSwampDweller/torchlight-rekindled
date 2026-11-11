// Constants and small types the gameplay state shares (core/world_state.h).
#pragma once

#include "core/ogre_types.h"

#include <cstdint>

namespace tl::core {

/// What `World::view_settle_link_particles` does with a link's particle systems: learn their link,
/// become one `CParticle`, stop.
struct LinkParticleSettle {
    bool particle = false;
    bool cparticle = false;
    bool stopped = false;
};

/// `CMissile::killMissile`: `+0x1b4`, how long a dead
/// missile's particles keep releasing before `handleDeathOfMissile` stops them.
inline constexpr float kMissileParticleStopSeconds = 1.0f;

// The player as a missile meets it: no character id is negative.
constexpr int64_t kMissilePlayer = -1;

/// The gameplay camera's default distance: `CCameraControl::getDefaultDistanceToDolly()`
/// (.rodata, 28.5) times `CAMERAMULT`; `CCameraControl::processInput()` clamps
/// between 14.0 and that. `CCameraControl::updateGameCamera` offsets
/// `(0.5, 0.75, -0.5)` turned by the camera angle, scaled by the dolly.
inline constexpr float kDefaultCameraDistance = 28.5f;

/// `PlayerCam`'s clip planes: `CGame::createCamera`.
inline constexpr float kPlayerCamNear = 1.0f;
inline constexpr float kPlayerCamFar = 90.0f;

/// The default when a level authors no `RIMLIGHT` (`CLevel::addCharacter`).
constexpr const char* kDefaultRimLight = "media/sharedtextures/rimlight.dds";

/// What a main-menu level plays (`CGameClient::loadMenuLevel` hardcodes it), spelled as
/// a rules `MUSIC` key.
constexpr const char* kMenuMusic = "../music/Title.ogg";

/// How long a quest's banner stays up, in seconds.
constexpr float kQuestBannerSeconds = 2.0f;

/// Where the ids given to spawned units start: a block no layout uses (`0x5350` is "SP").
constexpr int64_t kSpawnedNodeBase = 0x5350000000000000LL;

/// The animation a unit plays when the layout names nothing else (an engine name, not data).
constexpr const char* kUnitIdleAnimation = "Idle";

/// What `find_chunk_marker` reads besides the marker's place: its facing, and the warper beside it.
struct ChunkMarkerExtra {
    ogre::Vector3 forward = ogre::Vector3::UNIT_Z;
    bool has_warper = false;
    ogre::Vector3 warper_position = ogre::Vector3::ZERO;
    ogre::Vector3 warper_forward = ogre::Vector3::UNIT_Z;
};

/// One unit phase of `CLevel::populate`: monsters..
/// props, sections picked on `g_Rand`, each a
/// `populateSectionOfLevel`. `owner`: the characters' list; `label`: the log's word.
struct PopulatePhase {
    int64_t owner = 0;
    const char* label = "";
    bool champion = false;      ///< `populateSectionOfLevel`'s fourth argument
    bool markers = false;       ///< its fifth: the NPCs stand on `CLevel +0xf0`
    bool empty_counts = false;  ///< an empty roll counts as one: all but the monsters
    bool props = false;         ///< the props phase, for the log
};

/// The steps of `CPlayer::storeLevelSavedState` as `World::store_level_state`
/// takes them, in order: what `WorldState::store_step_listener` is told as each begins.
enum class StoreStep {
    Header,      ///< the level's record (steps 1-4) and the journal's depth
    Prepare,     ///< the machines walked, the merchants stocked
    Formations,  ///< step 5
    Items,       ///< step 6
    Characters,  ///< step 7
    Chunks,      ///< step 8
    Automap,     ///< step 9
    Logic,       ///< step 10
    Kept,        ///< step 11, the levels kept
    Done,
};

}  // namespace tl::core
