// `CGenericModel`'s animation half: which animations a model is playing, how
// they cross-fade, what is queued behind them, and the keys a frame passes.
// The records are kept newest first. Follows the console where it differs from
// Linux: a second set of states (CGenericModel::loadModel,
// CCharacter::loadModel, reInitialize), a
// looping blend carrying on its old record, and
// unreached restoring keys still fired when a record is dropped.
//
// No OGRE: a `ModelTrack` stands for the `Ogre::AnimationState` the original
// writes, and the renderer copies them onto its entity after each call.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tl::core {

/// The parts of an `Ogre::AnimationState` the model's animation code reads
/// and writes (the time through `setTimePosition`).
struct ModelTrack {
    float length = 0.0f;
    float time = 0.0f;
    float weight = 1.0f;
    bool enabled = false;
    bool loop = true;

    /// `Ogre::AnimationState::setTimePosition`: a looping state wraps into
    /// `[0, length)`, any other is held to `[0, length]`.
    void set_time(float t);
};

/// One key of an animation (`CKeyframe`): its `gKEYFRAME_TYPES` index and
/// its frame at 30 a second. `tag` is the index of the `.ANIMATION` `KEY` it was read from.
struct ModelKey {
    int type = 0;
    float frame = 0.0f;
    size_t tag = 0;
};

/// A key a frame passed: the animation and the key's place in that animation's list.
struct FiredModelKey {
    unsigned animation = 0;
    size_t key = 0;
};

/// `CActiveAnimation` (built inline by `playAnimation`; vtable).
struct ActiveAnimation {
    unsigned index = 0;       ///< +0x08: the animation
    float end = 0.0f;         ///< +0x10: when it ends (its length, or a duration)
    float length = 0.0f;      ///< +0x14: the state's length
    float time = 0.0f;        ///< +0x18
    bool loop = false;        ///< +0x1c
    bool active = false;      ///< +0x1d: playing
    bool queued = false;      ///< +0x1e: waiting behind the one in front of it
    bool wrapped = false;     ///< +0x1f: looped this frame
    bool alternate = false;   ///< +0x20: plays from the second set of states
    bool finished = false;    ///< +0x21: to be dropped
    bool faded = false;       ///< +0x22: ran out, or its blend did
    bool superseded = false;  ///< +0x23: a newer record is in front of it
    float blend_left = 0.0f;  ///< +0x24
    float blend_time = 0.0f;  ///< +0x28
    /// +0x2c: how much of the blend is still to come; weight = 1 - fade
    float fade = 0.0f;
    float speed = 1.0f;       ///< +0x30
    bool flush_hits = false;  ///< +0x34: fire unreached `HIT`s when dropped

    /// `CActiveAnimation::update(float)` (inlined in the function's loop).
    void update(float dt);
};

class ModelAnimator {
public:
    /// `alternate`: the model has the second set of states. `CGenericModel::CGenericModel`
    /// leaves the animation half empty.
    explicit ModelAnimator(bool alternate = false) : has_alternate_(alternate) {}

    // ---- the animation set (`CAnimationSet::CAnimationSet`) and its states,
    // made and filled by `CGenericModel::loadModel` ----

    /// One more animation, its name as its skeleton names it, its state's
    /// length. Returns its index.
    unsigned add_animation(const std::string& name, float length);
    size_t animation_count() const { return names_.size(); }
    const std::string& animation_name(unsigned index) const { return names_[index]; }

    /// `CGenericModel::AddKey(unsigned, CDataGroup*)`: the
    /// key list of `animation`, grown to reach it, takes one more key at its end.
    void add_key(unsigned animation, const ModelKey& key);
    /// `CGenericModel::getKeyCount`.
    size_t key_count(unsigned animation) const;
    /// `CGenericModel::getKeyFrame`: null
    /// past either end.
    const ModelKey* key(unsigned animation, size_t key) const;

    // PORT: an accessor of the port's model animator (`CGenericModel`'s animation half).
    std::vector<ModelTrack>& tracks() { return tracks_; }
    const std::vector<ModelTrack>& tracks() const { return tracks_; }
    std::vector<ModelTrack>& alternate_tracks() { return alternate_tracks_; }
    const std::vector<ModelTrack>& alternate_tracks() const { return alternate_tracks_; }
    bool has_alternate() const { return has_alternate_; }

    const std::vector<ActiveAnimation>& active() const { return active_; }

    /// While set, `update` steps nothing.
    bool paused = false;

    // ---- `CGenericModel`'s methods ----

    /// `CGenericModel::getAnimationIndex(const std::string&)`:
    /// the first whose upper-cased name equals `name`, or -1.
    int index_of(const std::string& name) const;

    /// `CGenericModel::getAnimationLengthSeconds(int)`.
    float length_seconds(int index) const;

    // ---- what a layout node's `ANIMATION` group sets ----

    /// The `ANIMATION` property's `to_id`: the first animation
    /// of the set whose name equals `name` exactly, or 0 - an unknown name
    /// plays the model's first animation.
    unsigned descriptor_index(const std::string& name) const;

    /// `CCharacter::setAnimationPlaying(unsigned)` (X360
    /// inline in the `Monster` node's `ANIMATION` setter):
    /// everything stops (`clearAnimations`), `index` is kept, and with a set
    /// it is played again.
    void set_animation_playing(unsigned index);
    /// `CCharacter::setAnimationLoop(bool)` (X360 inline in
    /// the `LOOPS` setter): kept, and the animation played again.
    void set_animation_loop(bool loop);
    /// `CCharacter::setAnimationSpeed(float)` (X360 inline in
    /// the `SPEED` setter): kept as it is, and the animation played
    /// again.
    void set_animation_speed(float speed);
    unsigned played_index() const { return played_index_; }
    bool played_loop() const { return played_loop_; }
    float played_speed() const { return played_speed_; }

    /// `CGenericModel::playAnimation(unsigned, bool, float, float)`:
    /// everything else stops and this
    /// starts at full weight. `duration` -1 is the animation's own length.
    void play(unsigned index, bool loop, float speed, float duration = -1.0f);
    /// `CGenericModel::playAnimation(const std::string&, bool, float)`.
    ///
    void play(const std::string& name, bool loop, float speed);

    /// `CGenericModel::blendAnimation(unsigned, bool, float, float, bool)`:
    /// cross-fade into `index` over
    /// `blend` seconds.
    void blend(unsigned index, bool loop, float blend, float speed, bool flush_hits = false);
    /// `CGenericModel::blendAnimation(const std::string&, ...)`:
    /// every animation of that name.
    void blend(const std::string& name, bool loop, float blend, float speed);

    /// `CGenericModel::queueBlendAnimation(unsigned, bool, float, float)`:
    /// starts when what plays in front of
    /// it has `blend` seconds left.
    void queue_blend(int index, bool loop, float blend, float speed);
    /// `CGenericModel::queueBlendAnimation(const std::string&, ...)`.
    ///
    void queue_blend(const std::string& name, bool loop, float blend, float speed);

    /// `CGenericModel::clearQueuedAnimations()`.
    void clear_queued();
    /// `CGenericModel::clearAnimations()`.
    void clear();

    /// `CGenericModel::animationPlaying() const`.
    bool playing() const;
    /// `CGenericModel::animationPlaying(unsigned) const`.
    ///
    bool playing(unsigned index) const;
    /// `CGenericModel::animationPlaying(const std::string&) const`.
    ///
    bool playing(const std::string& name) const;
    /// The console's own: whether the first record playing `index` plays it
    /// from the second set.
    bool playing_alternate(unsigned index) const;
    /// `CGenericModel::animationQueued(unsigned) const`.
    bool queued(unsigned index) const;
    /// `CGenericModel::animationQueued(const std::string&) const`.
    bool queued(const std::string& name) const;

    /// `CGenericModel::setAnimationSpeed(float)`.
    void set_speed(float speed);
    /// `CGenericModel::setAnimationTime(float)`.
    void set_time(float time);
    /// `CGenericModel::getAnimationTime()`.
    float time() const;

    /// `CGenericModel::updateAnimation(float, bool)`. A step
    /// of exactly 1000 runs without the `visible` test. Returns the keys passed.
    const std::vector<FiredModelKey>& update(float dt, bool force, bool visible = true);
    const std::vector<FiredModelKey>& fired() const { return fired_; }

    /// The state a record plays on, in the first or the second set.
    ModelTrack* track_of(const ActiveAnimation& record);

private:
    ModelTrack* state(bool alternate, unsigned index);
    void enable(ModelTrack* track);
    void disable(ModelTrack* track);
    /// the three setters' shared tail.
    void replay_played();

    bool has_alternate_ = false;
    /// The constructors' 0, on and 1.
    unsigned played_index_ = 0;
    bool played_loop_ = true;
    float played_speed_ = 1.0f;
    std::vector<std::string> names_;
    std::vector<std::vector<ModelKey>> keys_;
    std::vector<ModelTrack> tracks_;
    std::vector<ModelTrack> alternate_tracks_;
    /// Newest first.
    std::vector<ActiveAnimation> active_;
    std::vector<FiredModelKey> fired_;
};

/// One `[ANIMATION]` block of a model's `.ANIMATION` file, as
/// `CGenericModel::loadAnimations` reads it.
struct AnimationFileEntry {
    std::string file;                         ///< `FILE`
    bool exists = false;                      ///< the file is in the archive
    std::vector<std::string> skeleton_names;  ///< its skeleton's animations, in order
    std::vector<ModelKey> keys;               ///< its `KEY` blocks
};

/// What `loadAnimations` makes of the file, on a first load: for each block in
/// order, the animation index its keys go to and the name it files under;
/// skipped blocks have `index == -1`.
struct LoadedAnimation {
    int index = -1;
    std::string name;
    bool counted = false;    ///< it adds to the set's count (`+0x28`); a `BIND` does not
    bool duplicate = false;  ///< logged as "Animation duplicate named: "
};

/// `CGenericModel::loadAnimations`, the
/// part that decides the animation list: a block whose `FILE` is missing is
/// passed over; the animation is the skeleton's first, or, in the file named
/// as the mesh (`model_stem`), the one called `BIND`; a name the entity
/// already has and an earlier block took is a duplicate and skipped; a `BIND`
/// animation is filed but not counted. `entity_has` is the entity's
/// `hasAnimationState` before the load. The list starts as `loadModel`
/// leaves the set: the entity's own animations but one named
/// `*`, so an index counts them first.
std::vector<LoadedAnimation> load_animation_list(const std::vector<AnimationFileEntry>& entries,
                                                 const std::string& model_stem,
                                                 const std::vector<std::string>& entity_has);

/// One name of a model's animation set (`CAnimationSet +0x2c`).
struct AnimationSetEntry {
    std::string name;
    std::string file;  ///< the skeleton file it came from
    float length = 0.0f;
};
using AnimationSet = std::vector<AnimationSetEntry>;

/// The set `loadAnimations` leaves: the mesh skeleton's own animations first,
/// then each block's skeleton's animation under its own name (the one called `BIND` in the file
/// named as the mesh), in block order, a name already there passed over. `block_stems` are the
/// blocks' `FILE` stems; `read_beside` reads a file beside the mesh, empty when there is none.
AnimationSet load_animation_set(
    const std::string& mesh_file, const std::vector<std::string>& block_stems,
    const std::function<std::vector<uint8_t>(const std::string&)>& read_beside);

}  // namespace tl::core
