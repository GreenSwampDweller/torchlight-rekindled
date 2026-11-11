#include "core/motion.h"

#include "core/passability.h"

#include <cmath>

namespace tl::core {

Vec3 bound_outside_push(const Vec3& sum, bool player) {
    // the height zeroed, then the length (two `fmadds`).
    Vec3 push = sum;
    push.y = 0.0f;
    const float length =
        std::sqrt(std::fma(push.z, push.z, std::fma(push.x, push.x, push.y * push.y)));
    const float bound = player ? 10.0f : 30.0f;
    if (!(bound < length)) return push;
    // A player's through `Vector3::normalise`, any other's
    // written out; both divide only above 1e-8.
    const float whole =
        std::sqrt(std::fma(push.z, push.z, std::fma(push.x, push.x, push.y * push.y)));
    if (static_cast<double>(whole) > 1e-08) {
        const float inverse = 1.0f / whole;
        push.x = inverse * push.x;
        push.y = inverse * push.y;
        push.z = inverse * push.z;
    }
    push.x = push.x * bound;
    push.y = push.y * bound;
    push.z = push.z * bound;
    return push;
}

namespace {

// `Ogre::Vector3::length`, unfused as on Linux. PORT: the port's vector.
float length(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

}  // namespace

void steer(MotionState& state, float dir_x, float dir_z, float speed) {
    // CCharacter::updatePathing, from the
    // route delta onwards.
    const float old_y = state.velocity.y;
    float vx = dir_x;
    float vy = 0.0f;
    float vz = dir_z;
    const float len = std::sqrt(vx * vx + 0.0f + vz * vz);
    if (1e-08f < len) {
        const float inv = 1.0f / len;
        vx = vx * inv;
        vy = vy * inv;
        vz = inv * vz;
    }

    // Lay it along the ground: the stored ground normal crossed twice, with the
    // live normal only while the unit is on ground (UNIT_Y otherwise).
    const Vec3 ground = state.ground_normal;
    const Vec3 up = state.grounded ? state.ground_normal : Vec3{0.0f, 1.0f, 0.0f};
    const float cz = ground.x * vy - ground.y * vx;
    const float cx = vz * ground.y - vy * ground.z;
    const float cy = vx * ground.z - ground.x * vz;
    const float along_y = cz * up.x - cx * up.z;
    const float along_x = up.z * cy - cz * up.y;
    const float along_z = cx * up.y - cy * up.x;

    state.velocity.x = along_x * speed;
    state.velocity.z = along_z * speed;
    state.velocity.y = (old_y - along_y) + along_y * speed;
}

void drop_to_ground(const CollisionWorld& world, MotionState& state) {
    // CCharacter::dropToGround, the
    // single-ray branch.
    const float was_y = state.position.y;
    state.grounded = false;
    CollisionHit hit;
    const Vec3 from{state.position.x, 100.0f, state.position.z};
    const Vec3 to{state.position.x, -100.0f, state.position.z};
    if (!world.ray_collision(from, to, true, hit) || hit.material == kNoCollideMaterial) {
        state.position.y = was_y;
        return;
    }
    state.position = {hit.point.x, hit.point.y + state.radius, hit.point.z};
}

void drop_to_ground_open(const CollisionWorld& world, MotionState& state, float reach,
                         const std::function<Vec3(const Vec3&, float)>& random_open) {
    // CCharacter::dropToGround, the branch
    // with its flag clear.
    state.position.y = 100.0f;
    state.grounded = false;
    float radius = 0.1f;
    for (int attempt = 0; attempt != 30; ++attempt) {
        CollisionHit hit;
        const Vec3 to{state.position.x, -100.0f, state.position.z};
        // `li r10, 0`: the objects are tested too. DIVERGENCE: the console's
        // characters (`CLevel +0x54`, material 200) are not among them; only a `BLOCK` one would be.
        if (world.ray_collision(state.position, to, /*objects=*/true, hit) &&
            hit.material != kNoCollideMaterial) {
            state.position = {hit.point.x, hit.point.y + state.radius, hit.point.z};
            return;
        }
        const Vec3 open = random_open(state.position, radius);
        const float wider = radius + 0.1f;
        radius = wider <= reach ? wider : reach;
        state.position = {open.x, 100.0f, open.z};
    }
}

void update_motion(float delta, const CollisionWorld& world, MotionState& state,
                   const PassabilityMap* passability) {
    // CCharacter::updateMotion
    const float r = state.radius;
    state.against_wall = false;

    float gravity = -150.0f;
    if (state.off_ground) {
        gravity = 0.0f;
        state.push = state.push * 0.0f;
    }
    if (!state.pathing && length(state.push) < 0.01f && state.grounded) return;

    const bool was_off_ground = state.off_ground;
    state.off_ground = false;

    // `CBaseUnit::hasEffect(0x3f)` scales the move by an effect value; no effect
    // is modelled, so it is the original's own default of 1.
    const float effect = 1.0f;
    const float scale = state.velocity_scale;
    const float no_rise = delta * 0.0f;
    const float half_rise = delta * no_rise * 0.5f;

    Vec3 next;
    next.y = delta * gravity * delta * 0.5f + delta * state.push.y +
             state.velocity.y * effect * scale * delta + state.position.y;
    next.x = delta * state.push.x + state.velocity.x * effect * scale * delta + half_rise +
             state.position.x;
    next.z = delta * state.push.z + effect * state.velocity.z * scale * delta + half_rise +
             state.position.z;
    if (state.grounded) next.y = next.y + 0.01f;

    const Vec3 push_before = state.push;
    const bool was_grounded = state.grounded;
    state.grounded = false;
    state.velocity.x = state.velocity.x + no_rise;
    state.velocity.y = gravity * delta + state.velocity.y;
    state.velocity.z = state.velocity.z + no_rise;

    Vec3 start = state.position;
    Vec3 result = next;

    Vec3 low = start;
    Vec3 high = start;
    if (next.x < low.x) low.x = next.x;
    if (high.x < next.x) high.x = next.x;
    if (next.y < low.y) low.y = next.y;
    if (high.y < next.y) high.y = next.y;
    if (next.z < low.z) low.z = next.z;
    if (high.z < next.z) high.z = next.z;
    low = {low.x + r * -4.0f, low.y - 100.0f, r * -4.0f + low.z};
    high = {high.x + r * 4.0f, high.y + 100.0f, r * 4.0f + high.z};
    world.sort_for_collision(low, high);

    // Onto the floor under where it is going, if that floor is within a step.
    CollisionHit hit;
    int limit = 4;
    int iterations = 0;
    const Vec3 above{next.x + 0.0f, next.y + 100.0f, next.z + 0.0f};
    if (world.pre_sorted_sphere_collision(above, next, r, true, hit) &&
        hit.material != kNoCollideMaterial && 0.2f < hit.normal.y &&
        hit.centre.y <= next.y + 0.3f) {
        limit = 5;
        iterations = 1;
        next = hit.centre;
        start.y = hit.centre.y;
    }

    float dx = next.x - start.x;
    float dy = next.y - start.y;
    float dz = next.z - start.z;

    // Slide along what the move meets. The position kept is where the sphere
    // touched; the velocity, projected off the face, carries the slide into
    // the next frame.
    while (world.pre_sorted_sphere_collision(start, next, r, true, hit) && iterations < limit) {
        state.off_ground = false;
        Vec3 n = hit.normal;

        const float into = state.velocity.x * n.x + state.velocity.y * n.y + state.velocity.z * n.z;
        if (into < 0.0f) {
            state.velocity.x = state.velocity.x - n.x * into;
            state.velocity.y = state.velocity.y - n.y * into;
            state.velocity.z = state.velocity.z - into * n.z;
        }
        if (n.y < 0.6f) {
            float hx = n.x;
            float hy = 0.0f;
            float hz = n.z;
            const float hl = std::sqrt(n.x * n.x + 0.0f + n.z * n.z);
            if (1e-08f < hl) {
                const float inv = 1.0f / hl;
                hx = n.x * inv;
                hy = inv * 0.0f;
                hz = inv * n.z;
            }
            const float against = state.push.x * hx + state.push.y * hy + state.push.z * hz;
            if (against < 0.0f) {
                state.push.x = state.push.x - hx * against;
                state.push.y = state.push.y - hy * against;
                state.push.z = state.push.z - against * hz;
            }
        }
        if (0.2f < n.y && hit.material != kNoCollideMaterial) {
            state.ground_normal = n;
            state.grounded = true;
        }

        const float travel = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float k = dx * n.x + n.y * dy + dz * n.z;
        if (0.0f < k) n = {0.0f, 0.0f, 0.0f};
        if (0.2f <= hit.normal.y) {
            float sx = dx - n.x * k;
            float sy = dy - n.y * k;
            float sz = dz - n.z * k;
            const float sl = std::sqrt(sx * sx + sy * sy + sz * sz);
            if (1e-08f < sl) {
                const float inv = 1.0f / sl;
                sx = sx * inv;
                sy = sy * inv;
                sz = sz * inv;
            }
            next = {sx * travel + start.x, sy * travel + start.y, sz * travel + start.z};
            state.against_wall = false;
        } else {
            state.wall_point = hit.point;
            state.against_wall = true;
            // A wall: the move itself loses the part into it, and up to ten
            // slides are allowed.
            dx = dx - n.x * k;
            dy = dy - n.y * k;
            dz = dz - n.z * k;
            float sx = dx;
            float sy = dy;
            float sz = dz;
            const float sl = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (1e-08f < sl) {
                const float inv = 1.0f / sl;
                sx = dx * inv;
                sy = dy * inv;
                sz = inv * dz;
            }
            limit = 10;
            next = {travel * sx + start.x, sy * travel + start.y, sz * travel + start.z};
        }
        result = hit.centre;
        ++iterations;
    }

    // Is there ground under where it ended up?
    const float rise = 0.9f;
    CollisionHit ground;
    const Vec3 ray_from{result.x + 0.0f, rise + result.y, result.z + 0.0f};
    const Vec3 ray_to{result.x, result.y - 5.0f, result.z};
    if (!world.pre_sorted_ray_collision(ray_from, ray_to, true, ground)) {
        state.off_ground = true;
    } else {
        const float gap = std::fabs(ground.point.y - result.y);
        if (r + 0.1f <= gap || ground.normal.y <= 0.2f || ground.material == kNoCollideMaterial) {
            if (0.7f < ground.normal.y && ground.material == kNoCollideMaterial) {
                state.off_ground = true;
            } else {
                const float floor_y = r + ground.point.y;
                if (result.y < floor_y) result.y = floor_y;
                state.off_ground = false;
            }
        } else {
            if (gap < r + 0.02f) result.y = r + ground.point.y;
            state.ground_normal = ground.normal;
            state.grounded = true;
            state.off_ground = false;
        }
    }

    state.off_ground_time = state.off_ground ? delta + state.off_ground_time : 0.0f;
    if (state.off_ground) {
        if (!was_off_ground) {
            // Refused: a step with no ground under it does not happen.
            result = start;
            state.velocity = state.velocity * 0.0f;
        } else if (0.1f < state.off_ground_time) {
            // INVENTED: `CLevel::randomOpenItemPosition(start, 1.0)`:
            // the nearest open cell within three cells stands in for its
            // pick; the unit is walked to it at 4 units a second as the original walks it.
            Vec3 target = start;
            if (passability != nullptr && passability->ready()) {
                const int sx = passability->cell_x(start.x);
                const int sz = passability->cell_z(start.z);
                bool found = passability->map_passable(sx, sz);
                for (int ring = 1; ring <= 3 && !found; ++ring) {
                    for (int oz = -ring; oz <= ring && !found; ++oz) {
                        for (int ox = -ring; ox <= ring && !found; ++ox) {
                            if (std::abs(ox) != ring && std::abs(oz) != ring) continue;
                            if (!passability->map_passable(sx + ox, sz + oz)) continue;
                            target = {passability->centre_x(sx + ox), start.y,
                                      passability->centre_z(sz + oz)};
                            found = true;
                        }
                    }
                }
            }
            Vec3 toward{target.x - result.x, 0.0f, target.z - result.z};
            const float tl = length(toward);
            if (tl < 1.0f && 1e-08f < tl) toward = toward * (1.0f / tl);
            result = {toward.x * 4.0f * delta + result.x, toward.y * 4.0f * delta + result.y,
                      toward.z * 4.0f * delta + result.z};
        }
    }

    // Standing on ground with nowhere to go: hold still.
    if (state.grounded) {
        bool hold = true;
        if (state.pathing || 0.0f < length(push_before)) {
            if (state.walking_speed != 0.0f || state.running_speed != 0.0f) hold = false;
        }
        if (hold) {
            result.x = start.x;
            result.z = start.z;
            if (was_grounded) result.y = start.y;
        }
    }

    if (iterations == 0) state.grounded = false;
    // The re-plan of a pathing unit against a wall (`CCharacter::setDestination`)
    // is the caller's, through `against_wall` (`Player::follow()` has no route: a DIVERGENCE).

    if (state.off_ground || was_off_ground) {
        result.y = start.y;
        if (state.off_ground && 0.5f < state.off_ground_time) {
            state.position = {result.x, start.y, result.z};
            drop_to_ground(world, state);
            result = state.position;
        }
    }

    state.position = result;
    if (!state.grounded && !state.off_ground) {
        state.velocity.x = 0.0f;
        state.velocity.z = 0.0f;
    } else {
        state.velocity = state.velocity * 0.0f;
    }
}

}  // namespace tl::core
