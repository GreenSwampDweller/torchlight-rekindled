#include "core/model_animation.h"

#include "formats/ogre_mesh.h"
#include "formats/ogre_skeleton.h"

#include <cctype>
#include <cmath>

namespace tl::core {
namespace {

// PORT: ASCII upper-casing, the case-blind compare of animation names.
std::string upper(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

/// `gKEYFRAME_TYPES` indices the drop of a record still fires:
/// `REMOVEANIMATIONPARTICLES`, `REMOVEPARTICLES`,
/// `SHOWMESH`, `FADEINMESH`, `TARGETABLE`, `UNDAMPVELOCITY`, `SHOWWEAPONS`,
/// `HIDEWEAPONTRAIL`, `ENABLECOLLISION` - every key that puts back what an
/// earlier key of the animation took away.
bool restores(int type) {
    return type == 0xd || type == 0xc || type == 0x17 || type == 0x19 || type == 0x11 ||
           type == 0x13 || type == 0x14 || type == 8 || type == 10;
}

/// A key's time: its frame times 1/30 as the console holds it, 0.033333335f
/// (`fmuls`), not a division.
float key_seconds(const ModelKey& key) { return key.frame * 0.033333335f; }

}  // namespace

void ModelTrack::set_time(float t) {
    // `Ogre::AnimationState::setTimePosition`.
    if (t == time) return;
    time = t;
    if (loop) {
        time = std::fmod(time, length);
        if (time < 0.0f) time += length;
    } else {
        if (time < 0.0f) time = 0.0f;
        else if (time > length) time = length;
    }
}

void ActiveAnimation::update(float dt) {
    // `CActiveAnimation::update(float)`.
    wrapped = false;
    if (!active) return;
    if (blend_left > 0.0f) {
        blend_left = blend_left - dt;
        fade = 1.0f;
        if (blend_time != 0.0f && blend_left > 0.0f) {
            fade = blend_left / blend_time;
            if (fade > 1.0f) fade = 1.0f;
        }
        if (!(blend_left > 0.0f)) {
            fade = 0.0f;
            faded = true;
            blend_left = 0.0f;
        }
    }
    // One `fmadds`: speed * dt + time, one rounding.
    time = std::fma(speed, dt, time);
    if (end == 0.0f) {
        time = 0.0f;
        if (loop) {
            wrapped = true;
            return;
        }
        active = false;
        faded = true;
        finished = true;
        return;
    }
    if (!(time > end)) return;
    time = time - end;
    if (!loop) {
        time = end;
        active = false;
        faded = true;
        finished = true;
        return;
    }
    wrapped = true;
    while (time > end) time = time - end;
}

// PORT: an animation added to the model's set (`CGenericModel +0x1cc`), with a
// track in each state list.
unsigned ModelAnimator::add_animation(const std::string& name, float length) {
    names_.push_back(name);
    ModelTrack track;
    track.length = length;
    tracks_.push_back(track);
    if (has_alternate_) alternate_tracks_.push_back(track);
    if (keys_.size() < names_.size()) keys_.resize(names_.size());
    return static_cast<unsigned>(names_.size() - 1);
}

void ModelAnimator::add_key(unsigned animation, const ModelKey& key) {
    // `CGenericModel::AddKey`: the lists grown to
    // `animation + 1` when they do not reach, then the key appended.
    if (keys_.size() <= animation) keys_.resize(animation + 1);
    keys_[animation].push_back(key);
}

// PORT: an accessor of the port's model animator (`CGenericModel`'s animation half).
size_t ModelAnimator::key_count(unsigned animation) const {
    return animation < keys_.size() ? keys_[animation].size() : 0;
}

const ModelKey* ModelAnimator::key(unsigned animation, size_t index) const {
    // `CGenericModel::getKeyFrame`: 0 past the end of either
    // list.
    if (animation >= keys_.size()) return nullptr;
    if (index >= keys_[animation].size()) return nullptr;
    return &keys_[animation][index];
}

// PORT: an accessor of the port's model animator (`CGenericModel`'s animation half).
ModelTrack* ModelAnimator::state(bool alternate, unsigned index) {
    // The first state when the index is past the end: the console's `bge` to the
    // vector's own start.
    std::vector<ModelTrack>& set = alternate ? alternate_tracks_ : tracks_;
    if (set.empty()) return nullptr;
    return index < set.size() ? &set[index] : &set[0];
}

// PORT: an accessor of the port's model animator (`CGenericModel`'s animation half).
ModelTrack* ModelAnimator::track_of(const ActiveAnimation& record) {
    return state(record.alternate, record.index);
}

// PORT: a track of the model's state list switched on.
void ModelAnimator::enable(ModelTrack* track) {
    if (track != nullptr) track->enabled = true;
}

// PORT: a track of the model's state list switched off.
void ModelAnimator::disable(ModelTrack* track) {
    if (track != nullptr) track->enabled = false;
}

unsigned ModelAnimator::descriptor_index(const std::string& name) const {
    // the narrow name compared whole with each of the set's,
    // no case folding; none matching is 0.
    for (size_t i = 0; i < names_.size(); ++i) {
        if (names_[i] == name) return static_cast<unsigned>(i);
    }
    return 0;
}

void ModelAnimator::replay_played() {
    // (Linux, the same lines in each setter): nothing without
    // animations; an index past the last is cleared back to 0 first; then
    // `playAnimation(index, loop, speed, -1)`.
    const size_t count = names_.size();
    if (count == 0) return;
    if (count <= played_index_) {
        clear();
        played_index_ = 0;
    }
    play(played_index_, played_loop_, played_speed_, -1.0f);
}

void ModelAnimator::set_animation_playing(unsigned index) {
    // `clearAnimations` before the index is stored, whether or
    // not the model has a set.
    clear();
    played_index_ = index;
    replay_played();
}

// PORT: the played animation's loop flag set, the play run again.
void ModelAnimator::set_animation_loop(bool loop) {
    played_loop_ = loop;
    replay_played();
}

// PORT: the played animation's speed set, the play run again.
void ModelAnimator::set_animation_speed(float speed) {
    played_speed_ = speed;
    replay_played();
}

// PORT: an accessor of the port's model animator (`CGenericModel`'s animation half).
int ModelAnimator::index_of(const std::string& name) const {
    for (size_t i = 0; i < names_.size(); ++i) {
        if (upper(names_[i]) == upper(name)) return static_cast<int>(i);
    }
    return -1;
}

float ModelAnimator::length_seconds(int index) const {
    // `CGenericModel::getAnimationLengthSeconds(int)`: 0 out
    // of the set's range; the state's length (the first state past the states'
    // end) otherwise.
    if (index < 0 || static_cast<size_t>(index) >= names_.size()) return 0.0f;
    if (tracks_.empty()) return 0.0f;
    const size_t at = static_cast<size_t>(index) < tracks_.size() ? static_cast<size_t>(index) : 0;
    return tracks_[at].length;
}

void ModelAnimator::play(unsigned index, bool loop, float speed, float duration) {
    // `CGenericModel::playAnimation(unsigned, bool, float, float)`.
    //
    if (names_.empty()) return;
    clear();
    ModelTrack* track = state(false, index);
    if (track == nullptr) return;
    track->loop = loop;
    ActiveAnimation record;
    record.index = index;
    record.end = track->length;
    record.length = track->length;
    record.time = 0.0f;
    record.speed = speed;
    record.loop = loop;
    if (duration != -1.0f) record.end = duration;
    record.fade = 0.0f;
    record.queued = false;
    record.active = true;
    enable(track);
    active_.insert(active_.begin(), record);
}

void ModelAnimator::play(const std::string& name, bool loop, float speed) {
    // `CGenericModel::playAnimation(const std::string&, bool, float)`:
    // the first animation whose upper-cased name is
    // `name` as passed.
    for (size_t i = 0; i < names_.size(); ++i) {
        if (upper(names_[i]) == name) {
            play(static_cast<unsigned>(i), loop, speed, -1.0f);
            return;
        }
    }
}

void ModelAnimator::blend(unsigned index, bool loop, float blend, float speed, bool flush_hits) {
    // `CGenericModel::blendAnimation(unsigned, bool, float, float, bool)`.
    //
    if (names_.empty()) return;
    const float length = length_seconds(static_cast<int>(index));
    if (length == 0.0f || blend == 0.0f) {
        play(index, loop, speed, -1.0f);
        return;
    }
    ModelTrack* first = state(false, index);
    const float state_length = first != nullptr ? first->length : 0.0f;
    // `state_length < blend ? state_length : blend` (the `blt`).
    const float blend_used = state_length < blend ? state_length : blend;
    clear_queued();

    bool alternate = false;
    if (playing(index)) alternate = !playing_alternate(index);
    alternate = alternate && has_alternate_;

    ModelTrack* track = state(alternate, index);
    if (track != nullptr) track->loop = loop;

    ActiveAnimation record;
    record.index = index;
    record.end = state_length;
    record.length = state_length;
    record.time = 0.0f;
    record.speed = speed;
    record.loop = loop;
    record.alternate = alternate;
    record.flush_hits = flush_hits;
    record.fade = 0.0f;
    record.active = true;
    record.queued = false;
    if (record.end != 0.0f) {
        record.blend_left = blend_used;
        record.blend_time = blend_used;
        record.fade = blend_used != 0.0f ? 1.0f : 0.0f;
    }
    enable(track);
    active_.insert(active_.begin(), record);

    // An older record of the same animation: a looping blend carries on from
    // its time and takes over its share of the weight.
    for (size_t i = 1; i < active_.size(); ++i) {
        if (active_[i].index != index) continue;
        if (loop) {
            ActiveAnimation& now = active_[0];
            ActiveAnimation& old = active_[i];
            float t = old.time;
            now.time = t;
            if (!(t > 0.0f)) t = 0.0f;
            now.time = t;
            if (!(t < now.end)) t = now.end;
            now.time = t;
            now.fade = 1.0f - old.fade;
            old.fade = 0.0f;
        }
        return;
    }
    // Otherwise the new state starts at no weight.
    if (track != nullptr) track->weight = 0.0f;
}

void ModelAnimator::blend(const std::string& name, bool loop, float blend_seconds, float speed) {
    // `CGenericModel::blendAnimation(const std::string&, ...)`:
    // every animation whose upper-cased name is `name` as passed, not just the
    // first.
    for (size_t i = 0; i < names_.size(); ++i) {
        if (upper(names_[i]) == name)
            blend(static_cast<unsigned>(i), loop, blend_seconds, speed, false);
    }
}

void ModelAnimator::queue_blend(int index, bool loop, float blend, float speed) {
    // `CGenericModel::queueBlendAnimation(unsigned, bool, float, float)`.
    //
    if (index == -1 || names_.empty()) return;
    const unsigned at = static_cast<unsigned>(index);
    if (length_seconds(index) == 0.0f) blend = 0.0f;
    ModelTrack* first = state(false, at);
    const float state_length = first != nullptr ? first->length : 0.0f;
    const float blend_used = state_length < blend ? state_length : blend;

    bool alternate = false;
    if (playing(at)) alternate = !playing_alternate(at);
    alternate = alternate && has_alternate_;

    ActiveAnimation record;
    record.index = at;
    record.end = state_length;
    record.length = state_length;
    record.speed = speed;
    record.loop = loop;
    record.alternate = alternate;
    ModelTrack* track = state(alternate, at);
    if (track != nullptr) track->loop = loop;
    record.active = false;
    record.queued = true;
    // No blend at all is a blend of 0.0001 s, so the record still
    // waits for the one in front of it to be that close to its end.
    const float wait = blend_used > 0.0f ? blend_used : 9.999999747378752e-05f;
    record.blend_left = wait;
    record.blend_time = wait;
    record.fade = 1.0f;
    active_.insert(active_.begin(), record);
}

void ModelAnimator::queue_blend(const std::string& name, bool loop, float blend, float speed) {
    // `CGenericModel::queueBlendAnimation(const std::string&, ...)`:
    // through `getAnimationIndex`.
    queue_blend(index_of(name), loop, blend, speed);
}

void ModelAnimator::clear_queued() {
    // `CGenericModel::clearQueuedAnimations()`: the queued
    // records out, the rest kept in their order.
    for (size_t i = 0; i < active_.size();) {
        if (active_[i].queued) {
            active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void ModelAnimator::clear() {
    // `CGenericModel::clearAnimations()`: each record's state
    // switched off, when its index is inside the set.
    for (const ActiveAnimation& record : active_) {
        if (record.index < names_.size()) disable(state(record.alternate, record.index));
    }
    active_.clear();
}

// PORT: an accessor of the port's model animator (`CGenericModel`'s animation half).
bool ModelAnimator::playing() const {
    for (const ActiveAnimation& record : active_) {
        if (!record.queued && record.active) return true;
    }
    return false;
}

// PORT: an accessor of the port's model animator (`CGenericModel`'s animation half).
bool ModelAnimator::playing(unsigned index) const {
    for (const ActiveAnimation& record : active_) {
        if (!record.queued && record.index == index && record.active) return true;
    }
    return false;
}

bool ModelAnimator::playing(const std::string& name) const {
    // `CGenericModel::animationPlaying(const std::string&)`:
    // through `getAnimationIndex`, -1 included.
    return playing(static_cast<unsigned>(index_of(name)));
}

// PORT: an accessor of the port's model animator (`CGenericModel`'s animation half).
bool ModelAnimator::playing_alternate(unsigned index) const {
    for (const ActiveAnimation& record : active_) {
        if (!record.queued && record.index == index && record.active) return record.alternate;
    }
    return false;
}

// PORT: an accessor of the port's model animator (`CGenericModel`'s animation half).
bool ModelAnimator::queued(unsigned index) const {
    for (const ActiveAnimation& record : active_) {
        if (record.queued && record.index == index) return true;
    }
    return false;
}

// PORT: an accessor of the port's model animator (`CGenericModel`'s animation half).
bool ModelAnimator::queued(const std::string& name) const {
    return queued(static_cast<unsigned>(index_of(name)));
}

// PORT: an accessor of the port's model animator (`CGenericModel`'s animation half).
void ModelAnimator::set_speed(float speed) {
    for (ActiveAnimation& record : active_) {
        if (!record.queued && record.active) record.speed = speed;
    }
}

// PORT: every record not queued put at `t`, held to its length (a headless frame dump).
void ModelAnimator::set_time(float t) {
    // Every record not queued, to `t` held to `[0, end]`.
    for (ActiveAnimation& record : active_) {
        if (record.queued) continue;
        float at = t > 0.0f ? t : 0.0f;
        if (!(at < record.end)) at = record.end;
        record.time = at;
    }
}

// PORT: an accessor of the port's model animator (`CGenericModel`'s animation half).
float ModelAnimator::time() const {
    for (const ActiveAnimation& record : active_) {
        if (!record.queued && record.active) return record.time;
    }
    return 0.0f;
}

const std::vector<FiredModelKey>& ModelAnimator::update(float dt, bool force, bool visible) {
    // `CGenericModel::updateAnimation(float, bool)`.
    if (!visible && dt != 1000.0f) {
        fired_.clear();
        return fired_;
    }
    if (paused) dt = 0.0f;
    if (active_.empty()) {
        fired_.clear();
        return fired_;
    }
    if (!force && active_.size() == 1 && active_[0].finished) {
        fired_.clear();
        return fired_;
    }
    fired_.clear();

    float total = 0.0f;
    bool full = false;    // a superseded record reached full weight
    bool behind = false;  // a record in front has run out
    struct Pending {
        float weight;
        unsigned index;
        bool alternate;
    };
    std::vector<Pending> pending;

    for (size_t i = 0; i < active_.size(); ++i) {
        const unsigned index = active_[i].index;
        const size_t last = active_.size() - 1;
        bool waiting = false;

        // A queued record starts once the one behind it is within its blend
        // of the end.
        if (active_[i].queued && dt != 0.0f && !(i < last && active_[i + 1].queued)) {
            bool start = true;
            if (i != last && active_[i + 1].active) {
                const ActiveAnimation& ahead = active_[i + 1];
                const float left = ahead.wrapped ? 0.0f : ahead.end - ahead.time;
                const float until = left / ahead.speed - dt;
                if (!(until <= active_[i].blend_time)) {
                    start = false;
                    waiting = true;
                }
            }
            if (start) {
                ActiveAnimation& now = active_[i];
                now.fade = 0.0f;
                now.active = true;
                now.queued = false;
                ModelTrack* track = state(now.alternate, index);
                enable(track);
                if (track != nullptr) track->set_time(now.time);
            }
        }
        if (i > 0 && !waiting && dt != 0.0f) active_[i].superseded = true;
        if (waiting && dt != 0.0f) continue;

        ActiveAnimation& record = active_[i];
        const float from = record.time;
        const bool was_active = record.active;
        record.update(dt);
        const float to = record.time;
        const float weight = 1.0f - record.fade;
        ModelTrack* track = state(record.alternate, index);
        if (weight != 0.0f && (record.active || was_active || force || from != to)) {
            if (track != nullptr) track->set_time(to);
        }
        const float share = weight - total;
        total = share + total;
        bool direct = full;
        if (!full) {
            if (share != 0.0f) {
                if (!force && from == to) {
                    direct = true;
                } else {
                    pending.push_back(Pending{share, index, record.alternate});
                }
            }
        }
        if (direct && share != 0.0f && record.active && (force || from != to)) {
            if (track != nullptr) track->weight = share;
        }
        if (share == 1.0f && record.superseded) full = true;

        if (was_active && dt != 0.0f && index != 0xffffffffu) {
            for (size_t k = 0; k < key_count(index); ++k) {
                const float at = key_seconds(keys_[index][k]);
                // branch for branch (a NaN passes
                // where the console's `bgt`/`bge`/`blt` would let it).
                const bool passed = (!(from > at) && to >= at) || (!(from > at) && to < from) ||
                                    (!(to < at) && !(to >= from));
                if (passed) fired_.push_back(FiredModelKey{index, k});
            }
        }
        if (behind && dt != 0.0f) record.finished = true;
        if (record.faded && dt != 0.0f) behind = true;
    }

    // The shares pushed are scaled together: by 1 - total while the total is
    // under 1 and not 0, and by 2 otherwise.
    float scale = 2.0f;
    if (!(total >= 1.0f) && total != 0.0f) scale = 1.0f - total;
    for (const Pending& p : pending) {
        ModelTrack* track = state(p.alternate, p.index);
        if (track != nullptr) track->weight = p.weight * scale;
    }

    if (dt != 0.0f) {
        for (size_t i = 1; i < active_.size(); ++i) {
            if (!active_[i].finished) continue;
            const ActiveAnimation record = active_[i];
            bool shared = false;
            for (size_t j = 0; j < i; ++j) {
                if (active_[j].index == record.index && active_[j].alternate == record.alternate) {
                    shared = true;
                    break;
                }
            }
            if (!shared) disable(state(record.alternate, record.index));
            // The keys it did not reach that undo an earlier key.
            if (!(record.time >= record.end)) {
                for (size_t k = 0; k < key_count(record.index); ++k) {
                    const ModelKey& key = keys_[record.index][k];
                    if (record.time >= key_seconds(key)) continue;
                    if (restores(key.type) ||
                        (record.flush_hits && (key.type == 0 || key.type == 0x1c))) {
                        fired_.push_back(FiredModelKey{record.index, k});
                    }
                }
            }
            // Out by the last record taking its place.
            active_[i] = active_.back();
            active_.pop_back();
            --i;
        }
    }
    return fired_;
}

namespace {

// The entity's own animations as `CGenericModel::loadModel` files them, one
// named `*` left out.
std::vector<std::string> own_animation_names(const std::vector<std::string>& entity_has) {
    std::vector<std::string> names;
    for (const std::string& own : entity_has) {
        if (own != "*") names.push_back(own);
    }
    return names;
}

}  // namespace

std::vector<LoadedAnimation> load_animation_list(const std::vector<AnimationFileEntry>& entries,
                                                 const std::string& model_stem,
                                                 const std::vector<std::string>& entity_has) {
    // `CGenericModel::loadAnimations`.
    std::vector<LoadedAnimation> out(entries.size());
    // `CAnimationSet +0x2c` and `+0x28` as `CGenericModel::loadModel` made
    // them: the entity's own animations, one named `*` left out.
    std::vector<std::string> names = own_animation_names(entity_has);
    unsigned count = static_cast<unsigned>(names.size());
    std::vector<std::string> seen;  // the names met so far
    bool linking = true;            // cleared for good by a name the entity has

    const auto has = [&entity_has](const std::string& name) {
        for (const std::string& n : entity_has) {
            if (n == name) return true;
        }
        return false;
    };

    for (size_t b = 0; b < entries.size(); ++b) {
        const AnimationFileEntry& entry = entries[b];
        if (!entry.exists || entry.skeleton_names.empty()) continue;
        const unsigned append = static_cast<unsigned>(names.size());
        size_t chosen = 0;
        std::string name = entry.skeleton_names[0];
        // The file named as the mesh: its `BIND` animation, when it has one.
        // `FILE` less its last nine characters (`.skeleton`), upper-cased,
        // against the mesh's stem as the model holds it.
        if (entry.file.size() >= 9 &&
            upper(entry.file.substr(0, entry.file.size() - 9)) == model_stem) {
            for (size_t i = 0; i < entry.skeleton_names.size(); ++i) {
                if (upper(entry.skeleton_names[i]) == "BIND") {
                    name = entry.skeleton_names[i];
                    chosen = i;
                }
            }
        }
        bool duplicate = false;
        if (has(name)) {
            for (const std::string& s : seen) {
                if (s == name) duplicate = true;
            }
        }
        seen.push_back(name);

        unsigned index = append;
        bool bind = false;
        if (upper(name) == "BIND") {
            bind = true;
            for (unsigned i = 0; i < count && i < names.size(); ++i) {
                if (names[i] == name) {
                    index = i;
                    break;
                }
            }
        } else if (has(name) && !duplicate) {
            linking = false;
            for (unsigned i = 0; i < count && i < names.size(); ++i) {
                if (names[i] == name) {
                    index = i;
                    break;
                }
            }
        }
        if (duplicate) {
            out[b].duplicate = true;
            out[b].name = name;
            continue;
        }
        const std::string filed = entry.skeleton_names[chosen];
        if (index == names.size()) names.push_back(filed);
        out[b].index = static_cast<int>(index);
        out[b].name = filed;
        out[b].counted = linking && !bind;
        if (linking && !bind) ++count;
    }
    return out;
}

AnimationSet load_animation_set(
    const std::string& mesh_file, const std::vector<std::string>& block_stems,
    const std::function<std::vector<uint8_t>(const std::string&)>& read_beside) {
    // `CGenericModel::loadAnimations`: each block's skeleton merged into the
    // model's and its animation's name pushed unless the set holds it.
    AnimationSet set;
    const auto add = [&set](const tl::formats::OgreSkeletonAnimation& a, const std::string& file) {
        for (const AnimationSetEntry& e : set) {
            if (e.name == a.name) return;
        }
        set.push_back({a.name, file, a.length});
    };
    const auto skeleton = [&read_beside](const std::string& file) {
        const std::vector<uint8_t> bytes = read_beside(file);
        return tl::formats::parse_ogre_skeleton(bytes.data(), bytes.size());
    };
    std::string base = mesh_file;
    if (const size_t slash = base.find_last_of("/\\"); slash != std::string::npos)
        base = base.substr(slash + 1);
    std::string stem = base;
    if (const size_t dot = stem.find_last_of('.'); dot != std::string::npos) stem.resize(dot);
    const std::vector<uint8_t> mesh_bytes = read_beside(base);
    const tl::formats::OgreMesh mesh =
        tl::formats::parse_ogre_mesh(mesh_bytes.data(), mesh_bytes.size());
    if (!mesh.skeleton.empty()) {
        std::string own = mesh.skeleton;
        if (const size_t slash = own.find_last_of("/\\"); slash != std::string::npos)
            own = own.substr(slash + 1);
        for (const tl::formats::OgreSkeletonAnimation& a : skeleton(own).animations) add(a, own);
    }
    for (const std::string& block : block_stems) {
        const std::string file = block + ".skeleton";
        const tl::formats::OgreSkeleton read = skeleton(file);
        if (!read.ok || read.animations.empty()) continue;
        size_t chosen = 0;
        if (upper(block) == upper(stem)) {
            for (size_t i = 0; i < read.animations.size(); ++i) {
                if (upper(read.animations[i].name) == "BIND") chosen = i;
            }
        }
        add(read.animations[chosen], file);
    }
    return set;
}

}  // namespace tl::core
