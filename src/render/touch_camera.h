// The touch camera: a turn, a tilt and a pinch zoom over the console's camera.
#pragma once

#include "core/input.h"

#include <OgreVector.h>

namespace tl::render {

/// DIVERGENCE (platform): the free camera. The right stick turns the camera about the player
/// (the heading the left stick walks by turns with it), a pinch zooms on a touch screen, iOS's
/// right stick tilts it, and R3 puts it back to the console's view. The Free Camera mod
/// (core/mods.h) switches it, on any platform; off, it changes nothing: the camera is the
/// console's.
struct TouchCamera {
    static bool enabled();

    float orbit_input = 0.0f;   ///< this frame's turn, -1..1
    float pitch_input = 0.0f;   ///< this frame's tilt, -1..1
    float zoom_delta = 0.0f;    ///< this frame's pinch, in dolly units, closer negative
    float orbit = 0.0f;         ///< the turn so far, radians about +Y
    float pitch_adjust = 0.0f;  ///< the tilt so far, radians
    float base_yaw = 0.0f;      ///< the heading before the turn
    float heading = 0.0f;       ///< the heading step() left; another is a new level's
    bool tracking = false;
    bool zoomed = false;  ///< the dolly is off its far end (step)
    bool r3_held = false;
    /// This frame's R3 resets the view, so it does not toggle the map too.
    bool took_r3 = false;

    /// The frame's input, none while `blocked` (a menu, a dialog, the death screen) or off;
    /// R3 is taken while the view is away from the console's.
    void take(const tl::core::InputState& in, bool blocked);
    /// Advance the turn and tilt into `camera_yaw`, and the pinch into `dolly`.
    void step(float dt, bool cinematic, float& camera_yaw, float& dolly, float camera_mult);
    /// The console's eye `offset` tilted, then turned; itself while both are zero.
    Ogre::Vector3 turned(Ogre::Vector3 offset, float dolly) const;
};

}  // namespace tl::render
