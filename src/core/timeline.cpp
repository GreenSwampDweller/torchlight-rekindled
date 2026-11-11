#include "core/timeline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace tl::core {

namespace {

constexpr std::array<std::string_view, 8> kInterpolationNames = {
    "Linear", "Linear Round", "Linear Round Down", "Linear Round Up",
    "Spline", "Quaternion",   "No Interpolation",  "Use Timeline Default"};

// PORT: a word's bits as a float.
float word_as_float(uint32_t word) {
    float value = 0.0f;
    std::memcpy(&value, &word, sizeof(value));
    return value;
}

// PORT: a float's bits as a word.
uint32_t float_as_word(float value) {
    uint32_t word = 0;
    std::memcpy(&word, &value, sizeof(word));
    return word;
}

// `Ogre::Vector4 * Ogre::Matrix4`: each column's sum fused,
// the first column in another order than the rest.
std::array<float, 4> row_times_matrix(const std::array<float, 4>& v, const float (&m)[4][4]) {
    return {
        std::fma(v[0], m[0][0], std::fma(m[3][0], v[3], std::fma(m[1][0], v[1], m[2][0] * v[2]))),
        std::fma(m[3][1], v[3], std::fma(m[2][1], v[2], std::fma(m[0][1], v[0], m[1][1] * v[1]))),
        std::fma(m[3][2], v[3], std::fma(m[2][2], v[2], std::fma(m[0][2], v[0], m[1][2] * v[1]))),
        std::fma(m[3][3], v[3], std::fma(m[2][3], v[2], std::fma(m[0][3], v[0], m[1][3] * v[1])))};
}

}  // namespace

// PORT: an accessor of a timeline value.
float TimelineValue::as_float(size_t index) const {
    return index < words.size() ? word_as_float(words[index]) : 0.0f;
}

// PORT: an accessor of a timeline value.
int32_t TimelineValue::as_int(size_t index) const {
    return index < words.size() ? static_cast<int32_t>(words[index]) : 0;
}

// PORT: an accessor of a timeline value.
tl::formats::CmpVec3 TimelineValue::as_vec3() const {
    return {as_float(0), as_float(1), as_float(2)};
}

// `CTimeline::AddProperty` and
// `CDescriptor::GetPropertyByIndex`; value types as
// `CDescriptorProp::getVariableType` gives them. An unknown index returns
// before allocating: no property.
std::optional<TimelineProperty> resolve_timeline_property(uint32_t descriptor, uint32_t tag) {
    namespace type = tl::formats::cmp_type;
    namespace field = tl::formats::cmp_field;

    switch (descriptor) {
        case type::kModel:
            if (tag == field::model::kVisible) {
                return TimelineProperty{TimelinePropertyKind::Visible, TimelineValueType::Bool,
                                        "VISIBLE"};
            }
            if (tag == field::model::kPosition) {
                return TimelineProperty{TimelinePropertyKind::Position, TimelineValueType::Vector3,
                                        "POSITION"};
            }
            if (tag == field::model::kForward) {
                return TimelineProperty{TimelinePropertyKind::Forward, TimelineValueType::Vector3,
                                        "FORWARD"};
            }
            if (tag == field::model::kRight) {
                return TimelineProperty{TimelinePropertyKind::Right, TimelineValueType::Vector3,
                                        "RIGHT"};
            }
            if (tag == field::model::kUp) {
                return TimelineProperty{TimelinePropertyKind::Up, TimelineValueType::Vector3, "UP"};
            }
            break;

        case type::kMonster:
            if (tag == field::monster::kPosition) {
                return TimelineProperty{TimelinePropertyKind::Position, TimelineValueType::Vector3,
                                        "POSITION"};
            }
            if (tag == field::monster::kForward) {
                return TimelineProperty{TimelinePropertyKind::Forward, TimelineValueType::Vector3,
                                        "FORWARD"};
            }
            if (tag == field::monster::kRight) {
                return TimelineProperty{TimelinePropertyKind::Right, TimelineValueType::Vector3,
                                        "RIGHT"};
            }
            if (tag == field::monster::kUp) {
                return TimelineProperty{TimelinePropertyKind::Up, TimelineValueType::Vector3, "UP"};
            }
            if (tag == field::monster::kScale) {
                return TimelineProperty{TimelinePropertyKind::Other, TimelineValueType::Float,
                                        "SCALE"};
            }
            break;

        case type::kParticleLink:
        case type::kLayoutLink:
            if (tag == field::linked::kVisible) {
                return TimelineProperty{TimelinePropertyKind::Visible, TimelineValueType::Bool,
                                        "VISIBLE"};
            }
            if (tag == field::linked::kPosition) {
                return TimelineProperty{TimelinePropertyKind::Position, TimelineValueType::Vector3,
                                        "POSITION"};
            }
            if (tag == field::linked::kForward) {
                return TimelineProperty{TimelinePropertyKind::Forward, TimelineValueType::Vector3,
                                        "FORWARD"};
            }
            if (tag == field::linked::kRight) {
                return TimelineProperty{TimelinePropertyKind::Right, TimelineValueType::Vector3,
                                        "RIGHT"};
            }
            if (tag == field::linked::kUp) {
                return TimelineProperty{TimelinePropertyKind::Up, TimelineValueType::Vector3, "UP"};
            }
            break;

        case type::kRoomPiece:
            if (tag == field::room_piece::kVisible) {
                return TimelineProperty{TimelinePropertyKind::Visible, TimelineValueType::Bool,
                                        "VISIBLE"};
            }
            if (tag == field::room_piece::kPosition) {
                return TimelineProperty{TimelinePropertyKind::Position, TimelineValueType::Vector3,
                                        "POSITION"};
            }
            if (tag == field::room_piece::kForward) {
                return TimelineProperty{TimelinePropertyKind::Forward, TimelineValueType::Vector3,
                                        "FORWARD"};
            }
            if (tag == field::room_piece::kRight) {
                return TimelineProperty{TimelinePropertyKind::Right, TimelineValueType::Vector3,
                                        "RIGHT"};
            }
            if (tag == field::room_piece::kUp) {
                return TimelineProperty{TimelinePropertyKind::Up, TimelineValueType::Vector3, "UP"};
            }
            if (tag == field::room_piece::kCollisionEnabled) {
                return TimelineProperty{TimelinePropertyKind::Other, TimelineValueType::Bool,
                                        "COLLISION ENABLED"};
            }
            if (tag == field::room_piece::kScaleZ) {
                return TimelineProperty{TimelinePropertyKind::Other, TimelineValueType::Float,
                                        "SCALE Z"};
            }
            break;

        case type::kGroup:
            if (tag == field::group::kPosition) {
                return TimelineProperty{TimelinePropertyKind::Position, TimelineValueType::Vector3,
                                        "POSITION"};
            }
            if (tag == field::group::kVisible) {
                return TimelineProperty{TimelinePropertyKind::Visible, TimelineValueType::Bool,
                                        "VISIBLE"};
            }
            break;

        case type::kPlayerSphereTrigger:
            if (tag == field::player_sphere_trigger::kPosition) {
                return TimelineProperty{TimelinePropertyKind::Position, TimelineValueType::Vector3,
                                        "POSITION"};
            }
            break;

        case type::kPlayerBoxTrigger:
            if (tag == field::player_box_trigger::kPosition) {
                return TimelineProperty{TimelinePropertyKind::Position, TimelineValueType::Vector3,
                                        "POSITION"};
            }
            // `CTriggerBoxDescriptor::Set_setDimensions`.
            if (tag == field::player_box_trigger::kDimensions) {
                return TimelineProperty{TimelinePropertyKind::Other, TimelineValueType::Vector3,
                                        "DIMENSIONS"};
            }
            break;

        case type::kUnitSpawner:
            if (tag == field::unit_spawner::kPosition) {
                return TimelineProperty{TimelinePropertyKind::Position, TimelineValueType::Vector3,
                                        "POSITION"};
            }
            if (tag == field::unit_spawner::kForward) {
                return TimelineProperty{TimelinePropertyKind::Forward, TimelineValueType::Vector3,
                                        "FORWARD"};
            }
            if (tag == field::unit_spawner::kRight) {
                return TimelineProperty{TimelinePropertyKind::Right, TimelineValueType::Vector3,
                                        "RIGHT"};
            }
            if (tag == field::unit_spawner::kUp) {
                return TimelineProperty{TimelinePropertyKind::Up, TimelineValueType::Vector3, "UP"};
            }
            break;

        case type::kUnitTrigger:
            if (tag == field::unit_trigger::kPosition) {
                return TimelineProperty{TimelinePropertyKind::Position, TimelineValueType::Vector3,
                                        "POSITION"};
            }
            if (tag == field::unit_trigger::kForward) {
                return TimelineProperty{TimelinePropertyKind::Forward, TimelineValueType::Vector3,
                                        "FORWARD"};
            }
            if (tag == field::unit_trigger::kRight) {
                return TimelineProperty{TimelinePropertyKind::Right, TimelineValueType::Vector3,
                                        "RIGHT"};
            }
            if (tag == field::unit_trigger::kUp) {
                return TimelineProperty{TimelinePropertyKind::Up, TimelineValueType::Vector3, "UP"};
            }
            break;

        case type::kCameraController:
            if (tag == field::camera_controller::kPosition) {
                return TimelineProperty{TimelinePropertyKind::Position, TimelineValueType::Vector3,
                                        "POSITION"};
            }
            break;

        case type::kEmitter:
            if (tag == field::emitter::kPosition) {
                return TimelineProperty{TimelinePropertyKind::Position, TimelineValueType::Vector3,
                                        "POSITION"};
            }
            break;

        case type::kMarker:
            if (tag == field::property_node::kVisible) {
                return TimelineProperty{TimelinePropertyKind::Visible, TimelineValueType::Bool,
                                        "VISIBLE"};
            }
            if (tag == field::property_node::kPosition) {
                return TimelineProperty{TimelinePropertyKind::Position, TimelineValueType::Vector3,
                                        "POSITION"};
            }
            if (tag == field::property_node::kEnabled) {
                return TimelineProperty{TimelinePropertyKind::Other, TimelineValueType::Bool,
                                        "ENABLED"};
            }
            break;

        case type::kDamageShape:
            if (tag == field::damage_shape::kPosition) {
                return TimelineProperty{TimelinePropertyKind::Position, TimelineValueType::Vector3,
                                        "POSITION"};
            }
            if (tag == field::damage_shape::kEnabled) {
                return TimelineProperty{TimelinePropertyKind::Other, TimelineValueType::Bool,
                                        "ENABLED"};
            }
            if (tag == field::damage_shape::kTimer) {
                return TimelineProperty{TimelinePropertyKind::Other, TimelineValueType::Float,
                                        "TIMER"};
            }
            break;

        // Every affector's second property is the `ENABLE`
        // `CAffectorDescriptor::CAffectorDescriptor`
        // registers.
        case type::kGravityWell:
        case type::kLinearForce:
        case type::kColorOverTime:
        case type::kScaleOverTime:
        case type::kTextureRotate:
        case type::kTextureAnimation:
        case type::kVortex:
        case type::kSineForce:
        case type::kAlign:
        case type::kJet:
        case type::kGeometryRotator:
        case type::kPlaneCollision:
        case type::kSphereCollision:
            if (tag == 1) {
                return TimelineProperty{TimelinePropertyKind::Other, TimelineValueType::Bool,
                                        "ENABLE"};
            }
            break;

        default: break;
    }
    return std::nullopt;
}

// PORT: asset parsing, the fields a node kind's orientation is authored in.
std::optional<OrientationFields> orientation_fields(uint32_t descriptor) {
    namespace type = tl::formats::cmp_type;
    namespace field = tl::formats::cmp_field;
    switch (descriptor) {
        case type::kModel:
            return OrientationFields{field::model::kRight, field::model::kUp,
                                     field::model::kForward};
        case type::kMonster:
            return OrientationFields{field::monster::kRight, field::monster::kUp,
                                     field::monster::kForward};
        case type::kParticleLink:
        case type::kLayoutLink:
            return OrientationFields{field::linked::kRight, field::linked::kUp,
                                     field::linked::kForward};
        case type::kRoomPiece:
            return OrientationFields{field::room_piece::kRight, field::room_piece::kUp,
                                     field::room_piece::kForward};
        case type::kUnitSpawner:
            return OrientationFields{field::unit_spawner::kRight, field::unit_spawner::kUp,
                                     field::unit_spawner::kForward};
        case type::kUnitTrigger:
            return OrientationFields{field::unit_trigger::kRight, field::unit_trigger::kUp,
                                     field::unit_trigger::kForward};
        default: return std::nullopt;
    }
}

// `CTimelineDescriptor::GetTimelineInterpolationIDByString`. The
// search gives up after index 6, so the last name never matches.
TimelineInterpolation parse_timeline_interpolation(std::string_view name) {
    for (uint32_t i = 0; i < 7; ++i) {
        if (name == kInterpolationNames[i]) return static_cast<TimelineInterpolation>(i);
    }
    return TimelineInterpolation::Linear;
}

// `CTimelineProperty::SetInterpolationType`, as
// `CTimeline::loadTimelineFromBinaryFile`
// leaves it: called once per key, in key order, so the last key's tag is the
// one kept. A bool is always No Interpolation; Quaternion survives only on a
// vector3.
TimelineInterpolation timeline_property_interpolation(uint32_t last_key_tag,
                                                      TimelineValueType type) {
    if (type == TimelineValueType::Bool) return TimelineInterpolation::NoInterpolation;
    if (last_key_tag == static_cast<uint32_t>(TimelineInterpolation::Quaternion) &&
        type != TimelineValueType::Vector3) {
        return TimelineInterpolation::Linear;
    }
    return static_cast<TimelineInterpolation>(last_key_tag);
}

// `CTimelineProperty::getValueOfLinearPropertyByPercent`. Blended
// in double; the rounding truncates toward zero, so Round Down of -0.5 is 0.
double timeline_component(TimelineInterpolation rounding, double u,
                          const tl::formats::CmpTimelineKey& a,
                          const tl::formats::CmpTimelineKey& b, size_t index,
                          TimelineValueType type) {
    if (index >= a.words.size() || index >= b.words.size()) return 0.0;
    double value = u;  // a string or bool carries the fraction itself
    switch (type) {
        case TimelineValueType::Float:
        case TimelineValueType::Vector2:
        case TimelineValueType::Vector3:
        case TimelineValueType::Vector4: {
            const float from = word_as_float(a.words[index]);
            const float to = word_as_float(b.words[index]);
            value = static_cast<double>(from) + static_cast<double>(to - from) * u;
            break;
        }
        case TimelineValueType::Integer:
        case TimelineValueType::Unsigned: {
            const auto from = static_cast<int32_t>(a.words[index]);
            const auto to = static_cast<int32_t>(b.words[index]);
            value = static_cast<double>(from) + static_cast<double>(to - from) * u;
            break;
        }
        case TimelineValueType::String:
        case TimelineValueType::Bool: break;
        default: return 0.0;
    }
    switch (rounding) {
        case TimelineInterpolation::LinearRound:
            return static_cast<double>(static_cast<int32_t>(value + 0.5));
        case TimelineInterpolation::LinearRoundDown:
            return static_cast<double>(static_cast<int32_t>(value));
        case TimelineInterpolation::LinearRoundUp:
            return static_cast<double>(static_cast<int32_t>(value + 1.0));
        default: return value;
    }
}

// `CTimelineProperty::DoLinearInterpolation`.
std::optional<TimelineValue> timeline_linear(const tl::formats::CmpTimelineKey& a,
                                             const tl::formats::CmpTimelineKey& b, float percent,
                                             TimelineInterpolation property,
                                             TimelineInterpolation timeline_default,
                                             TimelineValueType type) {
    // Two keys at one time blend nothing: the earlier key.
    const float u = b.time != a.time ? (percent - a.time) / (b.time - a.time) : 0.0f;

    // The rounding is the *timeline's* default, and only for a property that
    // defers to it. A property's own Round or Quaternion tag blends plainly.
    const TimelineInterpolation rounding = property == TimelineInterpolation::UseTimelineDefault
                                               ? timeline_default
                                               : TimelineInterpolation::Linear;

    if (type == TimelineValueType::String) {
        const double v = timeline_component(rounding, u, a, b, 0, type);
        return TimelineValue{v < 1.0 ? a.words : b.words};
    }

    const size_t count = std::min(a.words.size(), b.words.size());
    if (count == 0) return std::nullopt;
    TimelineValue out;
    out.words.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const double v = timeline_component(rounding, u, a, b, i, type);
        switch (type) {
            case TimelineValueType::Integer:
            case TimelineValueType::Unsigned:
                out.words[i] = static_cast<uint32_t>(static_cast<int32_t>(v));
                break;
            case TimelineValueType::Float:
            case TimelineValueType::Vector2:
            case TimelineValueType::Vector3:
            case TimelineValueType::Vector4:
                out.words[i] = float_as_word(static_cast<float>(v));
                break;
            case TimelineValueType::Bool: out.words[i] = 1.0 <= v ? 1u : 0u; break;
            default: return std::nullopt;
        }
    }

    // A timeline defaulting to Quaternion normalises a direction as it blends.
    // (`CTimelineProperty::DoQuaternion` is empty; this is all there is.)
    if (rounding == TimelineInterpolation::Quaternion && type == TimelineValueType::Vector3 &&
        count >= 3) {
        float x = out.as_float(0), y = out.as_float(1), z = out.as_float(2);
        const float length = std::sqrt(x * x + y * y + z * z);
        if (1e-08f < length) {
            const float inverse = 1.0f / length;
            y *= inverse;
            x *= inverse;
            z *= inverse;
        }
        out.words[0] = float_as_word(x);
        out.words[1] = float_as_word(y);
        out.words[2] = float_as_word(z);
    }
    return out;
}

// `CTimelineProperty::fillSplineWithData` and
// `CTimelineProperty::DoSplineInterpolation`:
// every key a point of one `Ogre::SimpleSpline` of vectors, in key order, so
// the keys' times play no part - the spline spaces its points evenly by index.
std::optional<TimelineValue> timeline_spline(const std::vector<tl::formats::CmpTimelineKey>& keys,
                                             float percent, TimelineValueType type) {
    size_t width = 0;
    switch (type) {
        case TimelineValueType::Integer:
        case TimelineValueType::Float:
        case TimelineValueType::Unsigned:
        case TimelineValueType::Bool: width = 1; break;
        case TimelineValueType::Vector2: width = 2; break;
        case TimelineValueType::Vector3: width = 3; break;
        default: return std::nullopt;  // `fillSplineWithData` resets the type to Linear
    }
    // a one-wide key's word is an integer, converted; a wider
    // key's words are a vector's, raw.
    // INVENTED: components past a key's own width are 0; the original keeps
    // whatever its stack holds, the same for every point, so only a NaN there
    // would change the closed test.
    std::vector<std::array<float, 3>> points;
    for (const tl::formats::CmpTimelineKey& key : keys) {
        // A key of the wrong width ends the fill: the points before it stay,
        // their tangents made as each was added (`mAutoCalc`).
        if (key.words.size() != width) break;
        std::array<float, 3> p{0.0f, 0.0f, 0.0f};
        if (width == 1) {
            p[0] = static_cast<float>(static_cast<int32_t>(key.words[0]));
        } else {
            for (size_t c = 0; c < width; ++c) p[c] = word_as_float(key.words[c]);
        }
        points.push_back(p);
    }
    // INVENTED: an empty spline. The original would index an empty point list.
    if (points.empty()) return std::nullopt;

    // `Ogre::SimpleSpline::interpolate(Real)` and
    // `interpolate(unsigned, Real)`.
    const size_t n = points.size();
    const float segment = percent * static_cast<float>(n - 1);
    const auto index = static_cast<unsigned>(segment);
    const float s = segment - static_cast<float>(index);
    std::array<float, 3> result{};
    if (index + 1 == n || s == 0.0f) {
        result = points[index];
    } else if (s == 1.0f) {
        result = points[index + 1];
    } else {
        // `recalcTangents`: closed when the first point equals the last.
        const bool closed = points.front() == points.back();
        const auto tangent = [&](size_t i, size_t c) {
            if (i == 0) {
                return closed ? 0.5f * (points[1][c] - points[n - 2][c])
                              : 0.5f * (points[1][c] - points[0][c]);
            }
            if (i == n - 1) {
                return closed ? 0.5f * (points[1][c] - points[n - 2][c])
                              : 0.5f * (points[i][c] - points[i - 1][c]);
            }
            return 0.5f * (points[i + 1][c] - points[i - 1][c]);
        };
        const float s2 = s * s;
        const float s3 = s2 * s;
        // powers (s^3, s^2, s, 1) times the Hermite matrix, then times the
        // rows (point, next point, their tangents, each with a 1).
        const std::array<float, 4> powers{s3, s2, s, 1.0f};
        const float hermite[4][4] = {{2.0f, -2.0f, 1.0f, 1.0f},
                                     {-3.0f, 3.0f, -2.0f, -1.0f},
                                     {0.0f, 0.0f, 1.0f, 0.0f},
                                     {1.0f, 0.0f, 0.0f, 0.0f}};
        const std::array<float, 4> weights = row_times_matrix(powers, hermite);
        float rows[4][4];
        for (size_t c = 0; c < 3; ++c) {
            rows[0][c] = points[index][c];
            rows[1][c] = points[index + 1][c];
            rows[2][c] = tangent(index, c);
            rows[3][c] = tangent(index + 1, c);
        }
        for (float(&row)[4] : rows) row[3] = 1.0f;
        const std::array<float, 4> mixed = row_times_matrix(weights, rows);
        result = {mixed[0], mixed[1], mixed[2]};
    }

    // The first `width` components, as raw words.
    TimelineValue out;
    for (size_t c = 0; c < width; ++c) out.words.push_back(float_as_word(result[c]));
    return out;
}

// `CTimelineProperty::Update`, its value
// half.
std::optional<TimelineValue> timeline_value(const std::vector<tl::formats::CmpTimelineKey>& keys,
                                            TimelineInterpolation property,
                                            TimelineInterpolation timeline_default, float percent,
                                            float last, bool backwards, TimelineValueType type) {
    if (keys.empty()) return std::nullopt;
    const TimelineInterpolation mode =
        property == TimelineInterpolation::UseTimelineDefault ? timeline_default : property;

    if (mode == TimelineInterpolation::Spline) {
        // The spline is built only by `SetInterpolationType(Spline)` on the
        // property itself; one reached through the timeline's default was never
        // filled, and `DoSplineInterpolation` sets nothing.
        if (property != TimelineInterpolation::Spline) return std::nullopt;
        return timeline_spline(keys, percent, type);
    }

    if (mode == TimelineInterpolation::NoInterpolation) {
        // The first key crossed since the last update, if any.
        for (const tl::formats::CmpTimelineKey& key : keys) {
            const bool crossed = backwards ? (key.time < last && percent <= key.time)
                                           : (last < key.time && key.time <= percent);
            if (crossed) return TimelineValue{key.words};
        }
        return std::nullopt;
    }

    // The first key at or past `percent` - or, played backwards, at or past
    // `last`, so a backwards step blends across the span it left, and can
    // extrapolate outside it.
    const float bound = backwards ? last : percent;
    size_t at = 0;
    while (at < keys.size() && keys[at].time < bound) ++at;
    if (at == keys.size()) return TimelineValue{keys.back().words};
    if (at == 0 || percent == keys[at].time) return TimelineValue{keys[at].words};
    return timeline_linear(keys[at - 1], keys[at], percent, property, timeline_default, type);
}

}  // namespace tl::core
