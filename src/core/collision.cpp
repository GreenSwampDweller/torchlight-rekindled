#include "core/collision.h"

#include "core/runic_math.h"

#include <algorithm>
#include <cmath>

namespace tl::core {

namespace {

/// The "no hit yet" distance both list casts start from (`99999.0`).
constexpr float kNoHit = 99999.0f;

/// `Ogre::Vector3::length`, unfused.
float length(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

/// The length as `CCollisionList::sphereCollision` inlines it: y squared, z and
/// x fused onto it.
float fused_length(const Vec3& v) {
    return std::sqrt(std::fma(v.x, v.x, std::fma(v.z, v.z, v.y * v.y)));
}

/// The length as `CCollisionList::rayCollision` inlines it: z squared, y and x
/// fused onto it.
float ray_length(const Vec3& v) {
    return std::sqrt(std::fma(v.x, v.x, std::fma(v.y, v.y, v.z * v.z)));
}

/// `Ogre::Vector3::normalise`, as the original inlines it: left alone at or
/// below 1e-8.
void normalise(Vec3& v) {
    // y squared, x and z fused onto it; left alone at or below the double 1e-8
    // (`blelr`, which a NaN passes).
    const float len = std::sqrt(std::fma(v.z, v.z, std::fma(v.x, v.x, v.y * v.y)));
    if (static_cast<double>(len) <= 1e-08) return;
    const float inv = 1.0f / len;
    v.x = v.x * inv;
    v.y = v.y * inv;
    v.z = v.z * inv;
}

/// `MATH::boundsIntersectXY`: despite the name, x and z. The
/// console inlines it wherever it is used (`CQuadtreeNode::sort`),
/// as four refusals in this order.
bool bounds_intersect_xz(const Vec3& a_low, const Vec3& a_high, const Vec3& b_low,
                         const Vec3& b_high) {
    return !(b_high.x < a_low.x || b_high.z < a_low.z || a_high.x < b_low.x || a_high.z < b_low.z);
}

/// `MATH::boundsContainsXY`, inlined on the console the same way:
/// whether the outer box holds the inner one on x and z.
bool bounds_contains_xz(const Vec3& outer_low, const Vec3& outer_high, const Vec3& inner_low,
                        const Vec3& inner_high) {
    return !(outer_high.x < inner_high.x || outer_high.z < inner_high.z ||
             inner_low.x < outer_low.x || inner_low.z < outer_low.z);
}

/// The material `CLevel::objectSphereCollision` and
/// `objectRayCollision` give a hit on an item's collision
/// model, whatever the face's own: 100, the `nocollide` material.
constexpr uint32_t kItemObjectMaterial = 100;

/// `MATH::classifyPoint`: 0 in front of the
/// plane, 1 behind it, 2 on it (within 1e-5).
int classify_point(const Vec3& p, const Vec3& on_plane, const Vec3& n) {
    // y, then z and x fused onto it.
    const float d =
        std::fma(on_plane.x - p.x, n.x, std::fma(on_plane.z - p.z, n.z, (on_plane.y - p.y) * n.y));
    if (-1e-05f <= d) return (d <= 1e-05f) ? 2 : 1;
    return 0;
}

/// `MATH::classifyPointForSphere`: the same,
/// for a sphere of `radius` centred on `p`.
int classify_point_for_sphere(const Vec3& p, const Vec3& on_plane, const Vec3& n, float radius) {
    // classifyPoint's fused sum, then the radius added.
    const float d =
        std::fma(on_plane.x - p.x, n.x, std::fma(on_plane.z - p.z, n.z, (on_plane.y - p.y) * n.y)) +
        radius;
    if (-1e-05f <= d) return (d <= 1e-05f) ? 2 : 1;
    return 0;
}

/// `MATH::getSpherePlaneIntersection`: where
/// the line through `p1` and `p2` crosses the plane, unbounded.
bool sphere_plane_intersection(const Vec3& p1, const Vec3& p2, const Vec3& on_plane, const Vec3& n,
                               Vec3& out) {
    const float dx = p2.x - p1.x;
    const float dy = p2.y - p1.y;
    const float dz = p2.z - p1.z;
    // y, then x and z fused onto it; the point `t * d + p1` fused.
    const float line = std::fma(n.z, dz, std::fma(n.x, dx, n.y * dy));
    if (!(1e-05f <= std::fabs(line))) return false;
    const float tx = on_plane.x - p1.x;
    const float ty = on_plane.y - p1.y;
    const float tz = on_plane.z - p1.z;
    const float distance = std::fma(n.z, tz, std::fma(n.x, tx, n.y * ty));
    const float t = distance / line;
    out.x = std::fma(t, dx, p1.x);
    out.y = std::fma(t, dy, p1.y);
    out.z = std::fma(t, dz, p1.z);
    return true;
}

/// `MATH::getLinePlaneIntersection`: the
/// same, within the segment.
bool line_plane_intersection(const Vec3& p1, const Vec3& p2, const Vec3& on_plane, const Vec3& n,
                             Vec3& out) {
    const float dx = p2.x - p1.x;
    const float dy = p2.y - p1.y;
    const float dz = p2.z - p1.z;
    // the sums fused as in `sphere_plane_intersection`; the tests are
    // `blt`/`bgt`, which a NaN passes.
    const float line = std::fma(n.z, dz, std::fma(n.x, dx, n.y * dy));
    if (std::fabs(line) < 1e-05f) return false;
    const float tx = on_plane.x - p1.x;
    const float ty = on_plane.y - p1.y;
    const float tz = on_plane.z - p1.z;
    const float distance = std::fma(n.z, tz, std::fma(n.x, tx, n.y * ty));
    const float t = distance / line;
    if (t < 0.0f || 1.0f < t) return false;
    out.x = std::fma(t, dx, p1.x);
    out.y = std::fma(t, dy, p1.y);
    out.z = std::fma(t, dz, p1.z);
    return true;
}

}  // namespace

// `MATH::closestPointOnLine`: the
// projection's two sums in two `fmadds` each, the point along the segment in
// one. A NaN projection (a segment of no length) passes both tests (`bge`,
// `ble`) and is carried into the point.
Vec3 closest_point_on_line(const Vec3& a, const Vec3& b, const Vec3& p) {
    const float ex = b.x - a.x;
    const float ey = b.y - a.y;
    const float ez = b.z - a.z;
    const float length_squared = std::fma(ez, ez, std::fma(ex, ex, ey * ey));
    const float dot = std::fma(p.x - a.x, ex, std::fma(p.z - a.z, ez, (p.y - a.y) * ey));
    const float t = dot / length_squared;
    if (t < 0.0f) return a;
    if (t > 1.0f) return b;
    return {std::fma(ex, t, a.x), std::fma(ey, t, a.y), std::fma(ez, t, a.z)};
}

// `MATH::closestPointOnTriangle`: the
// nearest of the three edges' closest points, the earlier edge winning a tie.
// Each squared distance in two `fmadds`, the first edge's summed in another
// order than the other two's.
Vec3 closest_point_on_triangle(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& p) {
    Vec3 best = closest_point_on_line(a, b, p);
    float dx = best.x - p.x, dy = best.y - p.y, dz = best.z - p.z;
    float best_d = std::fma(dy, dy, std::fma(dz, dz, dx * dx));
    const Vec3 on_bc = closest_point_on_line(b, c, p);
    dx = on_bc.x - p.x, dy = on_bc.y - p.y, dz = on_bc.z - p.z;
    const float d_bc = std::fma(dx, dx, std::fma(dz, dz, dy * dy));
    if (d_bc < best_d) {
        best = on_bc;
        best_d = d_bc;
    }
    const Vec3 on_ca = closest_point_on_line(c, a, p);
    dx = on_ca.x - p.x, dy = on_ca.y - p.y, dz = on_ca.z - p.z;
    if (std::fma(dx, dx, std::fma(dz, dz, dy * dy)) < best_d) best = on_ca;
    return best;
}

uint32_t collision_material(const std::string& material_name, uint32_t submesh_index) {
    // MATH::getMeshInformation
    return material_name.find("nocollide") != std::string::npos ? kNoCollideMaterial
                                                                : submesh_index;
}

void CollisionWorld::add_triangle(const Vec3& p0, const Vec3& p1, const Vec3& p2,
                                  uint32_t material) {
    Tri t;
    // CCollisionModel::loadModel:
    // addFace(index[i + 2], index[i + 1], index[i]). The level's faces then come
    // through CCollisionList::addCollisionList
    // and CCollisionList::addFace, each with its own material.
    // The caller hands the vertices over already turned.
    t.a = p2;
    t.b = p1;
    t.c = p0;

    // CCollisionList::calculateNormals
    const float cbx = t.c.x - t.b.x;
    const float bax = t.b.x - t.a.x;
    const float cby = t.c.y - t.b.y;
    const float cbz = t.c.z - t.b.z;
    const float bay = t.b.y - t.a.y;
    const float baz = t.b.z - t.a.z;
    // each component one `fmsubs`, the length y squared with z and x
    // fused onto it, inlined (not `normalise`'s order); left alone at or below the double 1e-8.
    Vec3 n;
    n.y = std::fma(cbz, bax, -(baz * cbx));
    n.z = std::fma(bay, cbx, -(cby * bax));
    n.x = std::fma(baz, cby, -(bay * cbz));
    const float len = std::sqrt(std::fma(n.x, n.x, std::fma(n.z, n.z, n.y * n.y)));
    if (!(static_cast<double>(len) <= 1e-08)) {
        const float inv = 1.0f / len;
        n.x = n.x * inv;
        n.y = n.y * inv;
        n.z = n.z * inv;
    }
    t.normal = n;

    // CCollisionList::calculateFaceBounds
    t.low = t.a;
    t.high = t.a;
    expand_bounds(t.low, t.high, t.b);
    expand_bounds(t.low, t.high, t.c);

    t.material = material;
    t.group = group_;
    if (material == kNoCollideMaterial) ++nocollide_count_;

    Vec3& low = group_ == 0 ? level_low_ : object_low_;
    Vec3& high = group_ == 0 ? level_high_ : object_high_;
    bool& have = group_ == 0 ? have_level_ : have_objects_;
    if (!have) {
        low = t.a;
        high = t.a;
        have = true;
    }
    expand_bounds(low, high, t.a);
    expand_bounds(low, high, t.b);
    expand_bounds(low, high, t.c);

    const uint32_t index = static_cast<uint32_t>(tris_.size());
    if (group_ == 0) {
        level_faces_.push_back(index);
    } else {
        const auto [found, fresh] = groups_.try_emplace(group_);
        Group& g = found->second;
        if (fresh) {
            group_order_.push_back(group_);
            group_walk_.push_back(&g);
            g.low = t.low;
            g.high = t.high;
        } else {
            expand_bounds(g.low, g.high, t.low);
            expand_bounds(g.low, g.high, t.high);
        }
        g.faces.push_back(index);
    }
    tris_.push_back(t);
}

// PORT: a unit's collision list switched on or off (`setEnabled`'s effect on it).
void CollisionWorld::set_group_enabled(int64_t group, bool on) {
    if (group == 0) return;  // the level itself
    const auto found = groups_.find(group);
    if (found == groups_.end()) return;
    found->second.enabled = on;
}

// PORT: whether a unit's collision list blocks movement, set.
void CollisionWorld::set_group_blocks(int64_t group, bool on) {
    if (group == 0) return;
    const auto found = groups_.find(group);
    if (found == groups_.end()) return;
    found->second.blocks = on;
}

// PORT: an accessor of the port's collision lists.
void CollisionWorld::set_group_load_shift(int64_t group, const Vec3& shift) {
    if (group == 0) return;
    const auto found = groups_.find(group);
    if (found == groups_.end()) return;
    found->second.load_shift = shift;
}

// PORT: an accessor of the port's collision lists.
bool CollisionWorld::group_blocks(int64_t group) const {
    const auto found = groups_.find(group);
    return found != groups_.end() && found->second.blocks;
}

// PORT: an accessor of the port's collision lists.
bool CollisionWorld::has_group(int64_t group) const {
    if (group == 0) return false;
    return groups_.count(group) != 0;
}

// PORT: an accessor of the port's collision lists.
bool CollisionWorld::group_enabled(int64_t group) const {
    if (group == 0) return false;
    const auto found = groups_.find(group);
    if (found == groups_.end() || found->second.faces.empty()) return false;
    return found->second.enabled;
}

// PORT: an accessor of the port's collision lists.
bool CollisionWorld::group_bounds(int64_t group, Vec3& low, Vec3& high) const {
    if (group == 0) return false;
    const auto found = groups_.find(group);
    if (found == groups_.end() || found->second.faces.empty()) return false;
    low = found->second.low;
    high = found->second.high;
    return true;
}

bool CollisionWorld::ray_collision_group(int64_t group, const Vec3& start, const Vec3& end,
                                         CollisionHit& hit) const {
    // CBaseUnit::rayCollision with its last
    // parameter set, as `incrementMapPassabilityCollision`
    // (`li r8, 1`) and its decrement call it: the unit's `COLLIDEABLE` and live
    // flags are not asked, so a door whose collision is off is still probed.
    if (group == 0) return false;
    Vec3 low = start;
    Vec3 high = start;
    expand_bounds(low, high, end);

    const auto found = groups_.find(group);
    if (found == groups_.end()) return false;

    // The group's own faces, in the order they were added - the order the
    // whole list was walked in before, so a tie resolves the same way.
    float best = kNoHit;
    for (const uint32_t idx : found->second.faces) {
        const Tri& t = tris_[idx];
        if (t.material == kSkippedMaterial) continue;
        if (!bounds_intersect(t.low, t.high, low, high)) continue;

        const int from = classify_point(start, t.a, t.normal);
        const int to = classify_point(end, t.a, t.normal);
        if (from == 1 || from == to) continue;

        Vec3 p;
        if (!line_plane_intersection(start, end, t.a, t.normal, p)) continue;
        if (!point_in(p, t)) continue;
        // `CCollisionList::rayCollision`: the distance fused (z, y, x);
        // `bge` skips.
        const float d = ray_length(p - start);
        if (!(d >= best)) {
            best = d;
            hit.point = p;
            hit.centre = p;
            hit.normal = t.normal;
            hit.material = t.material;
            hit.group = t.group;
        }
    }
    return best != kNoHit;
}

void CollisionWorld::quadtree_reset(const Vec3& low, const Vec3& high, float smallest) {
    // CQuadtree::CQuadtree (inline in CLevel::bakeCollision
    // on Linux). The root is the box made square on x
    // and z by widening the shorter side about its middle; y is never read.
    quad_.clear();
    quad_smallest_ = smallest;
    float low_x = low.x, low_z = low.z, high_x = high.x, high_z = high.z;
    const float wide_x = std::fabs(high_x - low_x);
    const float wide_z = std::fabs(high_z - low_z);
    const float side = wide_z <= wide_x ? wide_x : wide_z;
    if (wide_x < side) {
        const float grow = (side - wide_x) * 0.5f;
        low_x = low_x - grow;
        high_x = grow + high_x;
    }
    if (wide_z < side) {
        const float grow = (side - wide_z) * 0.5f;
        low_z = low_z - grow;
        high_z = grow + high_z;
    }
    QuadNode root;
    root.high = {high_x, high.y, high_z};
    root.low = {low_x, low.y, low_z};
    quad_.push_back(std::move(root));
}

void CollisionWorld::quadtree_add(uint32_t face) {
    // CQuadtreeNode::addData: down
    // from the root to the smallest node that holds the face's box on x and z.
    // A node wider than the smallest size that holds the box and has no
    // children yet is quartered first; a face no child holds stays with the
    // node. The halves are cut by the node's width on x alone.
    const Vec3& face_low = tris_[face].low;
    const Vec3& face_high = tris_[face].high;
    uint32_t at = 0;
    for (;;) {
        const float wide = quad_[at].high.x - quad_[at].low.x;
        const float half = wide * 0.5f;
        if (!quad_[at].split && quad_smallest_ < wide &&
            bounds_contains_xz(quad_[at].low, quad_[at].high, face_low, face_high)) {
            const Vec3 low = quad_[at].low;
            const Vec3 high = quad_[at].high;
            const uint32_t first = static_cast<uint32_t>(quad_.size());
            quad_.resize(quad_.size() + 4);
            QuadNode& node = quad_[at];
            node.split = true;
            // In the console's order: low x and high z, high x and high z,
            // low x and low z, high x and low z.
            quad_[first + 0].high = {low.x + half, high.y, high.z};
            quad_[first + 0].low = {low.x, low.y, low.z + half};
            quad_[first + 1].high = {high.x, high.y, high.z};
            quad_[first + 1].low = {low.x + half, low.y, low.z + half};
            quad_[first + 2].high = {low.x + half, high.y, low.z + half};
            quad_[first + 2].low = {low.x, low.y, low.z};
            quad_[first + 3].high = {high.x, high.y, low.z + half};
            quad_[first + 3].low = {low.x + half, low.y, low.z};
            for (uint32_t i = 0; i < 4; ++i) node.child[i] = first + i;
        }
        if (!quad_[at].split) break;
        uint32_t into = 4;
        for (uint32_t i = 0; i < 4; ++i) {
            const QuadNode& child = quad_[quad_[at].child[i]];
            if (bounds_contains_xz(child.low, child.high, face_low, face_high)) {
                into = i;
                break;
            }
        }
        if (into == 4) break;
        at = quad_[at].child[into];
    }
    quad_[at].data.push_back(face);
    quad_[at].boxes.push_back({face_low.x, face_low.z, face_high.x, face_high.z});
}

void CollisionWorld::quadtree_sort(uint32_t node, const Vec3& low, const Vec3& high, bool test,
                                   std::vector<uint32_t>& out) const {
    // CQuadtreeNode::sort. Without
    // `test` the node lies inside the box: everything it and its children
    // hold. With it, the node's own faces whose bounds meet the box, then each
    // child the box holds whole (untested from there down) or merely meets
    // (tested again).
    const QuadNode& n = quad_[node];
    if (!test) {
        for (const uint32_t face : n.data) out.push_back(face);
        if (n.split) {
            for (const uint32_t child : n.child) quadtree_sort(child, low, high, false, out);
        }
        return;
    }
    for (size_t i = 0; i < n.data.size(); ++i) {
        // `bounds_intersect_xz(low, high, face low, face high)`, off the copy beside the index.
        const QuadNode::FaceBox& b = n.boxes[i];
        if (!(b.high_x < low.x || b.high_z < low.z || high.x < b.low_x || high.z < b.low_z)) {
            out.push_back(n.data[i]);
        }
    }
    if (!n.split) return;
    for (const uint32_t index : n.child) {
        const QuadNode& child = quad_[index];
        if (bounds_contains_xz(low, high, child.low, child.high)) {
            quadtree_sort(index, low, high, false, out);
        } else if (bounds_intersect_xz(child.low, child.high, low, high)) {
            quadtree_sort(index, low, high, true, out);
        }
    }
}

void CollisionWorld::build() {
    // CLevel::bakeCollision, from the list's
    // `calculateFaceBounds` on: the quadtree, then every face into it in list
    // order. The original bakes once; here more level faces rebuild the tree.
    // Object faces are not in it: a unit's model is cast whole.
    if (indexed_ == level_faces_.size() && (!quad_.empty() || level_faces_.empty())) return;
    if (!have_level_) {
        quad_.clear();
        indexed_ = 0;
        return;
    }
    // The smallest node: a thousandth of the box's diagonal, truncated, at least 1
    // (`fmadds` twice, times 0.001, `fctidz`).
    const float dx = level_high_.x - level_low_.x;
    const float dy = level_high_.y - level_low_.y;
    const float dz = level_high_.z - level_low_.z;
    const float across = std::sqrt(std::fma(dy, dy, std::fma(dx, dx, dz * dz))) * 0.001f;
    float smallest = 1.0f;
    if (1.0f < across) smallest = static_cast<float>(static_cast<int64_t>(across));
    // The root is sized from the list's own bounds; the console's is the union of
    // the room pieces' boxes, looser, so a face on a cell's edge
    // can sit one node off. The set a sort returns is the same.
    quadtree_reset(level_low_, level_high_, smallest);
    for (const uint32_t face : level_faces_) quadtree_add(face);
    indexed_ = level_faces_.size();
    sorted_low_ = sorted_high_ = Vec3{};
    for (size_t i = 0; i < level_faces_.size(); ++i) {
        const Tri& t = tris_[level_faces_[i]];
        if (i == 0) {
            sorted_low_ = t.low;
            sorted_high_ = t.high;
        } else {
            expand_bounds(sorted_low_, sorted_high_, t.low);
            expand_bounds(sorted_low_, sorted_high_, t.high);
        }
    }
}

bool CollisionWorld::may_sort_any(const Vec3& low, const Vec3& high) const {
    if (indexed_ != level_faces_.size() || quad_.empty() || level_faces_.empty()) return false;
    // `quadtree_sort` keeps a face only where this test passes for it, and every face's box
    // lies inside this one.
    return bounds_intersect_xz(low, high, sorted_low_, sorted_high_);
}

// PORT: the port's collision lists emptied.
void CollisionWorld::clear() {
    tris_.clear();
    group_ = 0;
    groups_.clear();
    group_order_.clear();
    group_walk_.clear();
    level_faces_.clear();
    quad_.clear();
    quad_smallest_ = 1.0f;
    sorted_.clear();
    scratch_level_.clear();
    indexed_ = 0;
    nocollide_count_ = 0;
    have_level_ = false;
    have_objects_ = false;
}

// PORT: the box every face sits in, the level's and the objects'.
bool CollisionWorld::bounds(Vec3& low, Vec3& high) const {
    if (!have_level_ && !have_objects_) return false;
    if (have_level_) {
        low = level_low_;
        high = level_high_;
        if (have_objects_) {
            expand_bounds(low, high, object_low_);
            expand_bounds(low, high, object_high_);
        }
    } else {
        low = object_low_;
        high = object_high_;
    }
    return true;
}

// The level list's own bounds, `CCollisionList +0x1c` / `+0x10` as
// `calculateFaceBounds` leaves them.
bool CollisionWorld::level_bounds(Vec3& low, Vec3& high) const {
    if (!have_level_) return false;
    low = level_low_;
    high = level_high_;
    return true;
}

void CollisionWorld::sort_for_collision(const Vec3& low, const Vec3& high) const {
    // CLevel::sortForCollision: the set is
    // emptied, room for 0x80 kept, and the root sorted with its test on.
    sorted_.clear();
    sorted_.reserve(0x80);
    if (indexed_ != level_faces_.size() || quad_.empty()) return;  // not built
    quadtree_sort(0, low, high, true, sorted_);
}

void CollisionWorld::sort_for_collision_by_points(const Vec3& a, const Vec3& b,
                                                  float radius) const {
    // CLevel::sortForCollisionByPoints. It
    // keeps no room in the set, unlike sortForCollision.
    Vec3 low = a;
    Vec3 high = a;
    expand_bounds(low, high, b);
    low = {low.x - radius, low.y - radius, low.z - radius};
    high = {high.x + radius, high.y + radius, high.z + radius};
    sorted_.clear();
    if (indexed_ != level_faces_.size() || quad_.empty()) return;  // not built
    quadtree_sort(0, low, high, true, sorted_);
}

bool CollisionWorld::point_in(const Vec3& p, const Tri& t) {
    // CCollisionList::pointIn
    const Vec3& a = t.a;
    const Vec3& b = t.b;
    const Vec3& c = t.c;
    const Vec3& n = t.normal;
    const float pz = p.z - a.z;
    const float px = p.x - a.x;
    const float py = p.y - a.y;
    const float ey_c = c.y - a.y;
    const float ex_c = c.x - a.x;
    const float ez_c = c.z - a.z;
    const float by = b.y - a.y;
    const float bx = b.x - a.x;
    const float bz = b.z - a.z;
    // each cross term an `fmsubs` (one rounding, the second product
    // rounded first), each sum two `fmadds` onto the first product.
    const float qy = std::fma(n.z, ex_c, -(n.x * ez_c));
    const float qz = std::fma(n.x, ey_c, -(n.y * ex_c));
    const float qx = std::fma(n.y, ez_c, -(n.z * ey_c));
    const float u = std::fma(px, qx, std::fma(pz, qz, py * qy));
    const float whole = std::fma(qx, bx, std::fma(qz, bz, qy * by));
    if (u < 0.0f || whole < u) return false;
    // the second side the same way, then `fadds` and the bound.
    const float ty = std::fma(pz, bx, -(bz * px));
    const float tx = std::fma(py, bz, -(pz * by));
    const float tz = std::fma(px, by, -(py * bx));
    const float v = std::fma(n.x, tx, std::fma(n.z, tz, n.y * ty));
    if (v < 0.0f) return false;
    return v + u <= whole;
}

bool CollisionWorld::list_sphere(const FaceList& list, const Vec3& start, const Vec3& end,
                                 const Vec3& box_low, const Vec3& box_high, float radius,
                                 CollisionHit& hit, float& best) const {
    // CCollisionList::sphereCollision
    const std::vector<uint32_t>& faces = list.faces;
    const std::vector<uint32_t>& by_position = list.by_position;
    if (faces.empty()) return best != kNoHit;
    if (!bounds_intersect(list.low, list.high, box_low, box_high)) return best != kNoHit;

    float best_facing = 1.0f;
    for (size_t position = 0; position < faces.size(); ++position) {
        const Tri& t = tris_[faces[position]];
        // The material (`+0xb0`) and, below, the edge test's normal (`+0x70` ->
        // `+0x60`, `r17` the loop's count) are read by the loop's position in the
        // sorted set: the list's own face at that position (`by_position`), not this face.
        const Tri& at_position = position < by_position.size() ? tris_[by_position[position]] : t;
        if (at_position.material == kSkippedMaterial) continue;
        if (!bounds_intersect(t.low, t.high, box_low, box_high)) continue;

        const int from = classify_point(start, t.a, t.normal);
        const int to = classify_point_for_sphere(end, t.a, t.normal, radius);
        if (to == 0 || to == from) continue;

        const Vec3 offset{end.x - radius * t.normal.x, end.y - radius * t.normal.y,
                          end.z - radius * t.normal.z};
        Vec3 p;
        if (!sphere_plane_intersection(start, offset, t.a, t.normal, p)) continue;

        if (point_in(p, t)) {
            // the distance's squares fused (y, z, x); `bgt` skips.
            const float d = fused_length(p - start);
            if (!(d > best)) {
                best = d;
                hit.point = p;
                hit.normal = t.normal;
                hit.centre = {t.normal.x * radius + p.x, t.normal.y * radius + p.y,
                              t.normal.z * radius + p.z};
                hit.material = t.material;
                hit.group = t.group;
            }
            continue;
        }

        const Vec3 closest = closest_point_on_triangle(t.a, t.b, t.c, start);
        Vec3 away = end - closest;
        // fused the same way; `bgt` against the radius, then the best.
        const float d = fused_length(away);
        if (d > radius) continue;
        if (d > best) continue;
        normalise(away);
        // y, then z and x fused, on the position's normal.
        const Vec3& n = at_position.normal;
        const float facing = std::fma(away.x, n.x, std::fma(n.z, away.z, n.y * away.y));
        if (!(facing < best_facing)) continue;
        if (classify_point(end, t.a, t.normal) != 0) continue;
        best = d;
        hit.normal = away;
        hit.point = closest;
        hit.centre = {radius * away.x + closest.x, radius * away.y + closest.y,
                      radius * away.z + closest.z};
        hit.material = t.material;
        hit.group = t.group;
        best_facing = facing;
    }
    return best != kNoHit;
}

bool CollisionWorld::list_ray(const FaceList& list, const Vec3& start, const Vec3& end,
                              const Vec3& box_low, const Vec3& box_high, CollisionHit& hit,
                              float& best) const {
    // CCollisionList::rayCollision
    const std::vector<uint32_t>& faces = list.faces;
    const std::vector<uint32_t>& by_position = list.by_position;
    if (faces.empty()) return best != kNoHit;
    if (!bounds_intersect(list.low, list.high, box_low, box_high)) return best != kNoHit;

    for (size_t position = 0; position < faces.size(); ++position) {
        const Tri& t = tris_[faces[position]];
        // The material skipped by the loop's position (`+0xb0[r15]`), as
        // in list_sphere(); the hit's material is the face's own.
        const Tri& at_position = position < by_position.size() ? tris_[by_position[position]] : t;
        if (at_position.material == kSkippedMaterial) continue;
        if (!bounds_intersect(t.low, t.high, box_low, box_high)) continue;

        const int from = classify_point(start, t.a, t.normal);
        const int to = classify_point(end, t.a, t.normal);
        if (from == 1 || from == to) continue;

        Vec3 p;
        if (!line_plane_intersection(start, end, t.a, t.normal, p)) continue;
        if (!point_in(p, t)) continue;
        // `CCollisionList::rayCollision`: the distance fused (z, y, x);
        // `bge` skips.
        const float d = ray_length(p - start);
        if (!(d >= best)) {
            best = d;
            hit.point = p;
            hit.centre = p;
            hit.normal = t.normal;
            hit.material = t.material;
            hit.group = t.group;
        }
    }
    return best != kNoHit;
}

bool CollisionWorld::object_sphere_collision(const Vec3& start, const Vec3& end, float radius,
                                             CollisionHit& hit, float& nearest) const {
    // CLevel::objectSphereCollision, the walk
    // of the level's items; its `BLOCK`-only flag is clear in every caller.
    // Each unit answers through
    // CBaseUnit::sphereCollision, the nearest
    // hit point to `start` winning, an earlier unit keeping a tie.
    nearest = kNoHit;
    if (group_order_.empty()) return false;

    // CBaseUnit::sphereCollision: the sweep's bounds, grown by twice the
    // radius, against the unit's.
    Vec3 low = start;
    Vec3 high = start;
    expand_bounds(low, high, end);
    const float grow = radius * 2.0f;
    const Vec3 reach_low{low.x - grow, low.y - grow, low.z - grow};
    const Vec3 reach_high{grow + high.x, grow + high.y, grow + high.z};
    // The box its list's cast tests each face against.
    const Vec3 box_low{low.x - radius, low.y - radius, low.z - radius};
    const Vec3 box_high{high.x + radius, high.y + radius, high.z + radius};

    for (size_t i = 0; i < group_order_.size(); ++i) {
        const int64_t id = group_order_[i];
        const Group& g = *group_walk_[i];
        if (!g.enabled) continue;  // `COLLIDEABLE` (+0x155) and live (+0x164)
        if (!bounds_intersect(g.low, g.high, reach_low, reach_high)) continue;
        // The unit's own list, every face of it (the original turns the sweep
        // into the unit's frame and the hit back; the faces here are already
        // placed in the world).
        CollisionHit unit_hit;
        float unit_best = kNoHit;
        if (!list_sphere({g.faces, g.faces, g.low, g.high}, start, end, box_low, box_high, radius,
                         unit_hit, unit_best)) {
            continue;
        }
        const float dx = unit_hit.point.x - start.x;
        const float dy = unit_hit.point.y - start.y;
        const float dz = unit_hit.point.z - start.z;
        const float d = std::sqrt(std::fma(dx, dx, std::fma(dz, dz, dy * dy)));
        if (d < nearest) {
            hit = unit_hit;
            hit.material = kItemObjectMaterial;
            hit.group = id;
            nearest = d;
        }
    }
    return nearest != kNoHit;
}

bool CollisionWorld::object_ray_collision(const Vec3& start, const Vec3& end, CollisionHit& hit,
                                          float& nearest) const {
    // CLevel::objectRayCollision, the walk of
    // the level's items: a unit is asked only when `COLLIDEABLE`, live **and
    // `BLOCK`**. Then CBaseUnit::rayCollision: the ray's bounds
    // against the unit's, and its list's cast.
    nearest = kNoHit;
    if (group_order_.empty()) return false;

    for (size_t i = 0; i < group_order_.size(); ++i) {
        const int64_t id = group_order_[i];
        const Group& g = *group_walk_[i];
        if (!g.enabled || !g.blocks) continue;
        // The ray's own bounds against the unit's culling bounds (`+0x180`), where it stands.
        Vec3 low = start;
        Vec3 high = start;
        expand_bounds(low, high, end);
        if (!bounds_intersect(g.low, g.high, low, high)) continue;
        // Then its model's cast, at its stored position: the faces moved back by `load_shift`.
        const Vec3& shift = g.load_shift;
        const bool shifted = shift.x != 0.0f || shift.y != 0.0f || shift.z != 0.0f;
        const Vec3 from = shifted ? start + shift : start;
        const Vec3 to = shifted ? end + shift : end;
        Vec3 ray_low = from;
        Vec3 ray_high = from;
        expand_bounds(ray_low, ray_high, to);
        CollisionHit unit_hit;
        float unit_best = kNoHit;
        if (!list_ray({g.faces, g.faces, g.low, g.high}, from, to, ray_low, ray_high, unit_hit,
                      unit_best))
            continue;
        if (shifted) {
            unit_hit.point = unit_hit.point - shift;
            unit_hit.centre = unit_hit.centre - shift;
        }
        // `fmuls` on y, then `fmadds` on z and on x.
        const float dx = unit_hit.point.x - start.x;
        const float dy = unit_hit.point.y - start.y;
        const float dz = unit_hit.point.z - start.z;
        const float d = std::sqrt(std::fma(dx, dx, std::fma(dz, dz, dy * dy)));
        if (d < nearest) {
            hit = unit_hit;
            hit.material = kItemObjectMaterial;
            hit.group = id;
            nearest = d;
        }
    }
    return nearest != kNoHit;
}

bool CollisionWorld::pre_sorted_sphere_collision(const Vec3& start, const Vec3& end, float radius,
                                                 bool objects, CollisionHit& hit) const {
    // CLevel::preSortedSphereCollision: is the
    // level's list alone; ask the
    // objects first.
    Vec3 low = start;
    Vec3 high = start;
    expand_bounds(low, high, end);
    low = {low.x - radius, low.y - radius, low.z - radius};
    high = {high.x + radius, high.y + radius, high.z + radius};

    float nearest = kNoHit;
    if (objects) {
        CollisionHit object_hit;
        float object_nearest = kNoHit;
        if (object_sphere_collision(start, end, radius, object_hit, object_nearest)) {
            hit = object_hit;
            nearest = object_nearest;
        }
    }

    if (have_level_) {
        CollisionHit level_hit;
        float level_best = kNoHit;
        if (list_sphere({sorted_, level_faces_, level_low_, level_high_}, start, end, low, high,
                        radius, level_hit, level_best)) {
            const float d = length(level_hit.point - start);
            if (d < nearest) {
                hit = level_hit;
                nearest = d;
            }
        }
    }
    return nearest != kNoHit;
}

bool CollisionWorld::pre_sorted_ray_collision(const Vec3& start, const Vec3& end, bool objects,
                                              CollisionHit& hit) const {
    // CLevel::preSortedRayCollision
    Vec3 low = start;
    Vec3 high = start;
    expand_bounds(low, high, end);

    float best = kNoHit;
    if (objects) {
        CollisionHit object_hit;
        float object_nearest = kNoHit;
        if (object_ray_collision(start, end, object_hit, object_nearest)) {
            hit = object_hit;
            best = object_nearest;
        }
    }
    float nearest = best;
    if (have_level_) {
        // The level cast starts from the object's distance, so it only answers
        // when it is nearer.
        CollisionHit level_hit;
        if (list_ray({sorted_, level_faces_, level_low_, level_high_}, start, end, low, high,
                     level_hit, best) &&
            best < nearest) {
            hit = level_hit;
            nearest = best;
        }
    }
    return nearest != kNoHit;
}

bool CollisionWorld::ray_collision(const Vec3& start, const Vec3& end, bool objects,
                                   CollisionHit& hit) const {
    // CLevel::rayCollision: the objects, then
    // its own sort of the quadtree over the ray's bounds (room for 500 faces),
    // and the list's cast from the object's distance.
    Vec3 low = start;
    Vec3 high = start;
    expand_bounds(low, high, end);

    float best = kNoHit;
    if (objects) {
        CollisionHit object_hit;
        float object_nearest = kNoHit;
        if (object_ray_collision(start, end, object_hit, object_nearest)) {
            hit = object_hit;
            best = object_nearest;
        }
    }
    float nearest = best;
    if (have_level_) {
        scratch_level_.clear();
        scratch_level_.reserve(500);
        if (indexed_ == level_faces_.size() && !quad_.empty()) {
            quadtree_sort(0, low, high, true, scratch_level_);
        }
        CollisionHit level_hit;
        if (list_ray({scratch_level_, level_faces_, level_low_, level_high_}, start, end, low, high,
                     level_hit, best) &&
            best < nearest) {
            hit = level_hit;
            nearest = best;
        }
    }
    return nearest != kNoHit;
}

bool CollisionWorld::snap_to_valid_ground(Vec3& at) const {
    // CLevel::snapToValidGround: a ray from 10 above
    // to 100 below; a hit on anything but material 100 puts the point 0.01 above it.
    CollisionHit hit;
    if (!ray_collision({at.x, at.y + 10.0f, at.z}, {at.x, at.y - 100.0f, at.z}, true, hit)) {
        return false;
    }
    if (hit.material == kNoCollideMaterial) return false;
    at = {hit.point.x, hit.point.y + 0.01f, hit.point.z};
    return true;
}

bool CollisionWorld::ground_under(float x, float z, float from_y, float& surface_y) const {
    // CCharacter::dropToGround: its ray is
    // +100 to -100.
    CollisionHit hit;
    if (!ray_collision({x, from_y, z}, {x, from_y - 200.0f, z}, true, hit)) return false;
    if (hit.material == kNoCollideMaterial) return false;
    surface_y = hit.point.y;
    return true;
}

float CollisionWorld::floor_height(float x, float z) const {
    // CLevel::floorHeight: +400 to -100,
    // `rayCollision(..., false)`.
    CollisionHit hit;
    if (!ray_collision({x, 400.0f, z}, {x, -100.0f, z}, true, hit)) return 0.0f;
    return hit.point.y;
}

}  // namespace tl::core
