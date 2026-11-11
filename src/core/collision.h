// Level collision: the triangle soup a level's room pieces are baked into, and
// the sphere and ray casts every unit and the passability grid ask of it -
// `CCollisionList` and the collision half of `CLevel`. `CCollisionModel::loadModel`
// reads a piece's `COLLISIONFILE`, and
// `CLevel::bakeCollision` appends every
// collision-enabled `Room Piece` at its transform into one level list, which
// `CCharacter::updateMotion` and `CLevel::calculatePassability` cast against.
//
// A face's material is 100 for a `nocollide` submesh, else the submesh's index
// (`MATH::getMeshInformation`); a material-100 face is
// never ground. Its normal follows the winding (`CCollisionList::calculateNormals`),
// which makes every cast one-sided.
//
// No OGRE, no file I/O: the renderer hands over world-space triangles.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace tl::core {

/// The minimum this file needs; not a general maths library.
struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// PORT: the port's vector; `Ogre::Vector3`'s operator, as the console inlines it.
inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
// PORT: the port's vector; `Ogre::Vector3`'s operator, as the console inlines it.
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }

/// `MATH::closestPointOnLine`: the point of
/// segment `a`-`b` nearest `p`.
Vec3 closest_point_on_line(const Vec3& a, const Vec3& b, const Vec3& p);
/// `MATH::closestPointOnTriangle`: the
/// nearest of the three edges' closest points, the earlier edge winning a tie.
Vec3 closest_point_on_triangle(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& p);

/// The material a face of a `nocollide` submesh gets
/// (`MATH::getMeshInformation`:
/// `std::string::find("nocollide")`, case-sensitive).
inline constexpr uint32_t kNoCollideMaterial = 100;

/// `CCollisionList+0x100`, set to 100000 by `CCollisionList::CCollisionList`:
/// faces of this material are skipped by both
/// casts. Nothing assigns it; kept so the test reads as the original's.
inline constexpr uint32_t kSkippedMaterial = 100000;

/// A unit's `COLLISION_RADIUS` when its data authors none: 0.3
/// (`CCharacter::unitInit`).
inline constexpr float kDefaultCollisionRadius = 0.3f;

/// The material a submesh's faces take. See `kNoCollideMaterial`.
uint32_t collision_material(const std::string& material_name, uint32_t submesh_index);

/// What a cast met. The fields are the out-parameters of
/// `CLevel::preSortedSphereCollision`, in its order.
struct CollisionHit {
    /// Where the sphere's centre is when it touches (a ray: unused).
    Vec3 centre;
    /// The point of contact.
    Vec3 point;
    /// The face's normal, or for a sphere touching an edge, the direction from
    /// the contact point towards the sphere's end position.
    Vec3 normal;
    uint32_t material = 0;
    /// The port's collision group the face came from; 0 is the level.
    int64_t group = 0;
};

class CollisionWorld {
public:
    /// Add one world-space face, with its vertices in the mesh's index order.
    ///
    /// Stored the way `CCollisionModel::loadModel`
    /// adds a mesh triangle, `addFace(i2, i1,
    /// i0)`, so the normal `calculateNormals` gives is the mesh's own
    /// front-facing normal. Degenerate faces are kept, as the original keeps
    /// them: their zero normal means no cast can meet them.
    void add_triangle(const Vec3& p0, const Vec3& p1, const Vec3& p2, uint32_t material = 0);

    /// Which group the faces added from here on belong to. Zero, the default,
    /// is the level list and cannot be switched off. Anything else is a unit's
    /// own collision model (a door's, a chest's), which the original casts unit
    /// by unit through `CLevel::objectSphereCollision` and
    /// `objectRayCollision`, in the order of each group's
    /// first face.
    void set_group(int64_t group) { group_ = group; }

    /// Switch a group's faces on or off: the unit's `COLLIDEABLE` and its
    /// collision being live. Nothing is removed and no index is rebuilt.
    void set_group_enabled(int64_t group, bool on);

    /// The unit's `BLOCK` (default false, `CBaseUnit::unitInit`):
    /// **a ray meets a unit's collision model only when the unit
    /// authors it**; a chest or a barrel stops a sphere and never a ray.
    void set_group_blocks(int64_t group, bool on);
    /// Where the object ray meets a group through a floor's load: its faces less `shift`
    /// (`Group::load_shift`); zero once the load is over.
    void set_group_load_shift(int64_t group, const Vec3& shift);
    bool group_blocks(int64_t group) const;

    /// Whether that group has any faces at all.
    bool has_group(int64_t group) const;
    /// Whether its faces are switched on (false for no such group).
    bool group_enabled(int64_t group) const;

    /// The box around one group's own faces, world-placed: what
    /// `addToAvoidanceMap` reads before probing it cell by cell
    /// (`PassabilityMap::increment_map_passability_collision`). False when the
    /// group has no faces.
    bool group_bounds(int64_t group, Vec3& low, Vec3& high) const;

    /// `CBaseUnit::rayCollision`: a ray
    /// against exactly one group's own faces. The original casts in the unit's
    /// local space; the faces here are already world-placed, the same cast.
    bool ray_collision_group(int64_t group, const Vec3& start, const Vec3& end,
                             CollisionHit& hit) const;

    /// Index what has been added. Cheap to call again after more faces.
    void build();

    void clear();

    // PORT: an accessor of the port's collision lists.
    size_t triangle_count() const { return tris_.size(); }
    /// Faces of material `kNoCollideMaterial`, for the load log.
    // PORT: an accessor of the port's collision lists.
    size_t nocollide_count() const { return nocollide_count_; }
    bool empty() const { return tris_.empty(); }

    /// The box every face sits inside. False when empty.
    bool bounds(Vec3& low, Vec3& high) const;

    /// The level list's own bounds, over the vertices of group 0:
    /// `CCollisionList+0x1c` / `+0x10` as `calculateFaceBounds`
    /// leaves them, which is what the
    /// passability grid of a level with no rooms is sized from.
    bool level_bounds(Vec3& low, Vec3& high) const;

    /// PORT: whether a box meets, on x and z, the bounds of every face the quadtree holds
    /// together. False means `sort_for_collision` over it collects nothing.
    bool may_sort_any(const Vec3& low, const Vec3& high) const;

    /// `CLevel::sortForCollision`: collect
    /// the level faces whose bounds meet the box on x and z, for the
    /// `pre_sorted_*` casts that follow. The set (`CLevel +0x44` on the
    /// console) is emptied and filled from the level's quadtree, in the
    /// quadtree's order (`quadtree_sort`).
    void sort_for_collision(const Vec3& low, const Vec3& high) const;

    /// `CLevel::sortForCollisionByPoints`,
    /// what `CMissile::checkCollision` sorts with: the box around the two
    /// points, grown by `radius` on every axis.
    void sort_for_collision_by_points(const Vec3& a, const Vec3& b, float radius) const;

    /// The set the last sort gathered, in the quadtree's order.
    const std::vector<uint32_t>& sorted_faces() const { return sorted_; }

    /// `CLevel::preSortedSphereCollision`: sweep a sphere from `start` to `end`
    /// against the faces the last `sort_for_collision()` gathered.
    ///
    /// `objects` false with its skip flag set
    /// (`calculatePassability`); true is the console's
    /// (`CCharacter::updateMotion`) and
    /// (`CCharacter::die`, `CMissile::checkCollision`): every collideable unit,
    /// `BLOCK` or not.
    bool pre_sorted_sphere_collision(const Vec3& start, const Vec3& end, float radius, bool objects,
                                     CollisionHit& hit) const;

    /// `CLevel::preSortedRayCollision`;
    /// objects are the `BLOCK` units alone.
    bool pre_sorted_ray_collision(const Vec3& start, const Vec3& end, bool objects,
                                  CollisionHit& hit) const;

    /// `CLevel::rayCollision`: the same cast,
    /// sorting its own faces out of the quadtree over the ray's bounds and
    /// leaving the pre-sorted set alone. `objects` is the inverse of its skip
    /// flag; objects are the `BLOCK` units alone.
    bool ray_collision(const Vec3& start, const Vec3& end, bool objects, CollisionHit& hit) const;

    /// The height of the ground below (x, z), searching down from `from_y`.
    ///
    /// The test `CCharacter::dropToGround`
    /// makes: a ray straight down, objects included, and a hit only counts when
    /// its face is not material 100. The caller's starting height stands in for
    /// the original's fixed +100.
    bool ground_under(float x, float z, float from_y, float& surface_y) const;
    /// `CLevel::snapToValidGround`: `at` onto the ground under it; false leaves it.
    bool snap_to_valid_ground(Vec3& at) const;

    /// `CLevel::floorHeight`, which
    /// `CEquipment::drop` lands an item on: a ray from (x, 400, z) to (x, -100,
    /// z), objects included, any face; the hit's height, or 0 when nothing is hit.
    float floor_height(float x, float z) const;

private:
    struct Tri {
        /// `CCollisionList+0xa0`, `+0xb8`, `+0xd0`: the face's three vertices in
        /// the list's order (the mesh's i2, i1, i0).
        Vec3 a, b, c;
        /// `+0x88`, from `calculateNormals`.
        Vec3 normal;
        /// `+0x40` / `+0x28`, from `calculateFaceBounds`.
        Vec3 low, high;
        /// `+0xe8`.
        uint32_t material = 0;
        int64_t group = 0;
    };

    /// A list the casts walk: its faces, the list's own faces by position, and its bounds.
    struct FaceList {
        // References, bound where the list is made.
        // cppcheck-suppress uninitMemberVarNoCtor
        const std::vector<uint32_t>& faces;
        // cppcheck-suppress uninitMemberVarNoCtor
        const std::vector<uint32_t>& by_position;
        const Vec3& low;
        const Vec3& high;
    };

    /// `CCollisionList::sphereCollision` over
    /// `faces`; `by_position` is the list's own faces in its order, which the original
    /// reads by the loop's position.
    bool list_sphere(const FaceList& list, const Vec3& start, const Vec3& end, const Vec3& box_low,
                     const Vec3& box_high, float radius, CollisionHit& hit, float& best) const;

    /// `CCollisionList::rayCollision` over
    /// `faces`; `by_position` as in list_sphere().
    bool list_ray(const FaceList& list, const Vec3& start, const Vec3& end, const Vec3& box_low,
                  const Vec3& box_high, CollisionHit& hit, float& best) const;

    /// `CCollisionList::pointIn`.
    static bool point_in(const Vec3& p, const Tri& t);

    /// `CQuadtreeCollisionNode` (X360 vtable, 0x48 bytes); it reads
    /// the face bounds off the list.
    struct QuadNode {
        Vec3 high;                         ///< X360 +0x08
        Vec3 low;                          ///< X360 +0x14
        bool split = false;                ///< X360 +0x24
        uint32_t child[4] = {0, 0, 0, 0};  ///< X360 +0x28, indices into `quad_`
        std::vector<uint32_t> data;        ///< X360 +0x38, face indices
        /// PORT: each face's x and z bounds beside `data`, for `quadtree_sort` to read in a row.
        struct FaceBox {
            float low_x, low_z, high_x, high_z;
        };
        std::vector<FaceBox> boxes;
    };

    /// `CQuadtree`'s constructor: the root over the box,
    /// widened on its shorter side to a square, and the smallest cell.
    void quadtree_reset(const Vec3& low, const Vec3& high, float smallest);
    /// `CQuadtreeNode::addData`.
    void quadtree_add(uint32_t face);
    /// `CQuadtreeNode::sort`.
    void quadtree_sort(uint32_t node, const Vec3& low, const Vec3& high, bool test,
                       std::vector<uint32_t>& out) const;

    /// One unit's own collision model: `CBaseUnit`'s `CCollisionModel` and the
    /// flags the object casts read.
    struct Group {
        std::vector<uint32_t> faces;  ///< indices into `tris_`, as added
        Vec3 low, high;               ///< the model's bounds, world-placed
        bool enabled = true;          ///< `COLLIDEABLE` and live
        bool blocks = false;          ///< `BLOCK`
        /// A room's trigger through the load: `CBaseUnit::rayCollision` tests
        /// the ray against its culling bounds where it stands, then casts its model at
        /// `getPosition(true)`, which for a unit with no parent object (`+0x50`)
        /// is its stored, room-relative position: the faces moved back by the room's origin.
        Vec3 load_shift{};
    };

    /// `CLevel::objectSphereCollision` with
    /// its `BLOCK`-only flag clear. `nearest` is the distance from `start` to
    /// the hit's point.
    bool object_sphere_collision(const Vec3& start, const Vec3& end, float radius,
                                 CollisionHit& hit, float& nearest) const;
    /// `CLevel::objectRayCollision`.
    bool object_ray_collision(const Vec3& start, const Vec3& end, CollisionHit& hit,
                              float& nearest) const;

    std::vector<Tri> tris_;
    int64_t group_ = 0;
    /// Each non-zero group, and the groups in the order they first got a face.
    std::unordered_map<int64_t, Group> groups_;
    std::vector<int64_t> group_order_;
    /// PORT: `group_order_`'s groups themselves, in its order (`groups_` never moves them).
    std::vector<const Group*> group_walk_;
    /// The level list's faces (group 0), as indices into `tris_`: the
    /// console's face `i` is `level_faces_[i]`.
    std::vector<uint32_t> level_faces_;
    /// `CLevel +0x3c`: the quadtree over the level list. `quad_[0]` is the
    /// root (`CQuadtree +0x28`) once built.
    std::vector<QuadNode> quad_;
    /// `CQuadtree +0x08`: no node narrower than this is split.
    float quad_smallest_ = 1.0f;
    /// How many of `level_faces_` the quadtree holds.
    size_t indexed_ = 0;
    size_t nocollide_count_ = 0;

    /// Bounds of the level faces and of the object faces.
    Vec3 level_low_, level_high_;
    /// Every face in the quadtree, its bounds together (`may_sort_any`).
    Vec3 sorted_low_, sorted_high_;
    bool have_level_ = false;
    Vec3 object_low_, object_high_;
    bool have_objects_ = false;

    /// `CLevel +0x44` on the console: the set the last sort gathered.
    mutable std::vector<uint32_t> sorted_;

    /// `CLevel::rayCollision`'s own set, kept so a per-frame query allocates
    /// nothing after the first.
    mutable std::vector<uint32_t> scratch_level_;
};

}  // namespace tl::core
