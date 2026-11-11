// How a timeline property reaches its value between keys (`core/timeline.*`).
// Keys are made-up numbers; nothing is read from the game.

#include "core/timeline.h"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "test_support.h"

namespace {

using tl::core::TimelineInterpolation;
using tl::core::TimelineValueType;
using tl::formats::CmpTimelineKey;

CmpTimelineKey key(float t, std::initializer_list<float> values,
                   TimelineInterpolation tag = TimelineInterpolation::Linear) {
    CmpTimelineKey k;
    k.time = t;
    k.interpolation = static_cast<uint32_t>(tag);
    for (float v : values) {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        k.words.push_back(bits);
    }
    return k;
}

bool close(float a, float b) { return std::abs(a - b) < 1e-4f; }

// A one-wide key as a spline reads it: an integer word.
CmpTimelineKey int_key(float t, int32_t value) {
    CmpTimelineKey k;
    k.time = t;
    k.interpolation = static_cast<uint32_t>(TimelineInterpolation::Linear);
    k.words.push_back(static_cast<uint32_t>(value));
    return k;
}

// The value at `percent` played forwards from 0.
float scalar_at(const std::vector<CmpTimelineKey>& keys, TimelineInterpolation property,
                TimelineInterpolation timeline_default, float percent) {
    const auto v = tl::core::timeline_value(keys, property, timeline_default, percent, 0.0f, false,
                                            TimelineValueType::Float);
    return v.has_value() ? v->as_float() : -999.0f;
}

}  // namespace

// (0.3 - 0.2) / 0.4 = 0.25 of the way from (10, 20, 30) to (50, 100, 150).
TL_TEST(timeline_linear_blends_a_vector_between_the_bracketing_keys) {
    const std::vector<CmpTimelineKey> keys = {key(0.2f, {10.0f, 20.0f, 30.0f}),
                                              key(0.6f, {50.0f, 100.0f, 150.0f})};
    const auto v =
        tl::core::timeline_value(keys, TimelineInterpolation::Linear, TimelineInterpolation::Linear,
                                 0.3f, 0.0f, false, TimelineValueType::Vector3);
    TL_CHECK(v.has_value());
    TL_CHECK(close(v->as_vec3().x, 20.0f));
    TL_CHECK(close(v->as_vec3().y, 40.0f));
    TL_CHECK(close(v->as_vec3().z, 60.0f));
}

// `DoLinearInterpolation` rounds by the timeline's default only for a property that defers
// to it (tag 7); a property's own Round tag blends plainly.
TL_TEST(timeline_a_propertys_own_round_tag_blends_plainly) {
    const std::vector<CmpTimelineKey> keys = {key(0.0f, {10.0f}), key(1.0f, {11.0f})};
    for (TimelineInterpolation own :
         {TimelineInterpolation::LinearRound, TimelineInterpolation::LinearRoundDown,
          TimelineInterpolation::LinearRoundUp, TimelineInterpolation::Quaternion}) {
        TL_CHECK(close(scalar_at(keys, own, TimelineInterpolation::Linear, 0.4f), 10.4f));
    }
}

// Through the default: Round is (int)(v + 0.5), Down (int)v, Up (int)(v + 1),
// all truncating toward zero. 10.4 and 10.6 separate every pair.
TL_TEST(timeline_rounding_through_the_timeline_default) {
    const std::vector<CmpTimelineKey> keys = {key(0.0f, {10.0f}), key(1.0f, {11.0f})};
    const auto via = [&](TimelineInterpolation d, float p) {
        return scalar_at(keys, TimelineInterpolation::UseTimelineDefault, d, p);
    };
    TL_CHECK(close(via(TimelineInterpolation::Linear, 0.4f), 10.4f));
    TL_CHECK(close(via(TimelineInterpolation::LinearRound, 0.4f), 10.0f));
    TL_CHECK(close(via(TimelineInterpolation::LinearRound, 0.6f), 11.0f));
    TL_CHECK(close(via(TimelineInterpolation::LinearRoundDown, 0.6f), 10.0f));
    TL_CHECK(close(via(TimelineInterpolation::LinearRoundUp, 0.4f), 11.0f));

    // Truncation, not floor: -0.5 rounds "down" to 0 and "up" to 0.
    const std::vector<CmpTimelineKey> negative = {key(0.0f, {0.0f}), key(1.0f, {-1.0f})};
    TL_CHECK(close(scalar_at(negative, TimelineInterpolation::UseTimelineDefault,
                             TimelineInterpolation::LinearRoundDown, 0.5f),
                   0.0f));
    TL_CHECK(close(scalar_at(negative, TimelineInterpolation::UseTimelineDefault,
                             TimelineInterpolation::LinearRoundUp, 0.5f),
                   0.0f));
}

// (5, 10, 0) halfway, normalised by 1/sqrt(125) - only through the default.
TL_TEST(timeline_quaternion_default_normalises_a_vector3) {
    const std::vector<CmpTimelineKey> keys = {key(0.0f, {10.0f, 0.0f, 0.0f}),
                                              key(1.0f, {0.0f, 20.0f, 0.0f})};
    const auto via_default = tl::core::timeline_value(
        keys, TimelineInterpolation::UseTimelineDefault, TimelineInterpolation::Quaternion, 0.5f,
        0.0f, false, TimelineValueType::Vector3);
    TL_CHECK(via_default.has_value());
    TL_CHECK(close(via_default->as_vec3().x, 5.0f / std::sqrt(125.0f)));
    TL_CHECK(close(via_default->as_vec3().y, 10.0f / std::sqrt(125.0f)));

    const auto own = tl::core::timeline_value(keys, TimelineInterpolation::Quaternion,
                                              TimelineInterpolation::Linear, 0.5f, 0.0f, false,
                                              TimelineValueType::Vector3);
    TL_CHECK(own.has_value());
    TL_CHECK(close(own->as_vec3().x, 5.0f));
    TL_CHECK(close(own->as_vec3().y, 10.0f));
}

// Points evenly spaced by index whatever the key times: 0.5 of four points is segment 1 at
// s = 0.5.
TL_TEST(timeline_spline_spaces_its_points_by_index) {
    const std::vector<CmpTimelineKey> keys = {int_key(0.0f, 0), int_key(0.2f, 10),
                                              int_key(0.4f, 40), int_key(1.0f, 60)};
    TL_CHECK(
        close(scalar_at(keys, TimelineInterpolation::Spline, TimelineInterpolation::Linear, 0.5f),
              24.375f));
}

// First point equal to the last closes the spline: T0 = 0.5 (p1 - p[n-2]).
TL_TEST(timeline_spline_closes_when_it_ends_where_it_began) {
    const std::vector<CmpTimelineKey> keys = {int_key(0.0f, 0), int_key(0.3f, 10),
                                              int_key(0.6f, 20), int_key(1.0f, 0)};
    TL_CHECK(
        close(scalar_at(keys, TimelineInterpolation::Spline, TimelineInterpolation::Linear, 0.1f),
              0.795f));
}

// A key of another width ends the fill; the points before it are still the spline.
TL_TEST(timeline_spline_keeps_the_points_before_a_key_of_another_width) {
    std::vector<CmpTimelineKey> keys = {int_key(0.0f, 0), int_key(0.5f, 10), int_key(1.0f, 30)};
    keys.push_back(key(1.0f, {1.0f, 2.0f}));
    TL_CHECK(
        close(scalar_at(keys, TimelineInterpolation::Spline, TimelineInterpolation::Linear, 1.0f),
              30.0f));
}

// The spline is built only when the property itself is Spline; one reached
// through the timeline's default was never filled and sets nothing.
TL_TEST(timeline_spline_through_the_default_sets_nothing) {
    const std::vector<CmpTimelineKey> keys = {key(0.0f, {0.0f}), key(1.0f, {10.0f})};
    const auto v = tl::core::timeline_value(keys, TimelineInterpolation::UseTimelineDefault,
                                            TimelineInterpolation::Spline, 0.5f, 0.0f, false,
                                            TimelineValueType::Float);
    TL_CHECK(!v.has_value());
}

// The first key crossed since the last update, either way; nothing otherwise.
TL_TEST(timeline_no_interpolation_sets_the_key_crossed) {
    const std::vector<CmpTimelineKey> keys = {key(0.2f, {10.0f}), key(0.7f, {20.0f})};
    const auto at = [&](float now, float last, bool backwards) {
        return tl::core::timeline_value(keys, TimelineInterpolation::NoInterpolation,
                                        TimelineInterpolation::Linear, now, last, backwards,
                                        TimelineValueType::Float);
    };
    TL_CHECK(close(TL_VALUE(at(0.3f, 0.1f, false)).as_float(), 10.0f));
    TL_CHECK(close(TL_VALUE(at(0.8f, 0.1f, false)).as_float(), 10.0f));  // first only
    TL_CHECK(!at(0.5f, 0.3f, false).has_value());
    TL_CHECK(close(TL_VALUE(at(0.6f, 0.8f, true)).as_float(), 20.0f));
    TL_CHECK(!at(0.4f, 0.5f, true).has_value());
}

// Before the first key, the first; past the last, the last.
TL_TEST(timeline_holds_the_end_keys_outside_their_range) {
    const std::vector<CmpTimelineKey> keys = {key(0.3f, {10.0f}), key(0.7f, {20.0f})};
    TL_CHECK(
        close(scalar_at(keys, TimelineInterpolation::Linear, TimelineInterpolation::Linear, 0.1f),
              10.0f));
    TL_CHECK(
        close(scalar_at(keys, TimelineInterpolation::Linear, TimelineInterpolation::Linear, 0.9f),
              20.0f));
}

// Played backwards the bracket is found from the last percent, so a step that crosses a key
// extrapolates the span it left.
TL_TEST(timeline_backwards_brackets_from_the_last_percent) {
    const std::vector<CmpTimelineKey> keys = {key(0.0f, {0.0f}), key(0.5f, {10.0f}),
                                              key(1.0f, {30.0f})};
    const auto v =
        tl::core::timeline_value(keys, TimelineInterpolation::Linear, TimelineInterpolation::Linear,
                                 0.4f, 0.8f, true, TimelineValueType::Float);
    TL_CHECK(v.has_value());
    TL_CHECK(close(v->as_float(), 6.0f));
}

// The last key's tag is the property's; a bool is always No Interpolation and
// Quaternion stands only on a vector3.
TL_TEST(timeline_property_type_from_the_last_key) {
    using tl::core::timeline_property_interpolation;
    TL_CHECK_EQ(timeline_property_interpolation(4, TimelineValueType::Vector3),
                TimelineInterpolation::Spline);
    TL_CHECK_EQ(timeline_property_interpolation(0, TimelineValueType::Bool),
                TimelineInterpolation::NoInterpolation);
    TL_CHECK_EQ(timeline_property_interpolation(5, TimelineValueType::Float),
                TimelineInterpolation::Linear);
    TL_CHECK_EQ(timeline_property_interpolation(5, TimelineValueType::Vector3),
                TimelineInterpolation::Quaternion);
}

// Exact names, and the search stops before the eighth.
TL_TEST(timeline_default_interpolation_names) {
    using tl::core::parse_timeline_interpolation;
    TL_CHECK_EQ(parse_timeline_interpolation("No Interpolation"),
                TimelineInterpolation::NoInterpolation);
    TL_CHECK_EQ(parse_timeline_interpolation("Linear Round Down"),
                TimelineInterpolation::LinearRoundDown);
    TL_CHECK_EQ(parse_timeline_interpolation("spline"), TimelineInterpolation::Linear);
    TL_CHECK_EQ(parse_timeline_interpolation("Use Timeline Default"),
                TimelineInterpolation::Linear);
    TL_CHECK_EQ(parse_timeline_interpolation(""), TimelineInterpolation::Linear);
}

// `CTimeline::AddProperty` and `CDescriptor::GetPropertyByIndex`.
TL_TEST(timeline_resolve_property_by_descriptor_and_tag) {
    using tl::core::resolve_timeline_property;
    using tl::core::TimelinePropertyKind;
    using tl::core::TimelineValueType;
    namespace cmp_type = tl::formats::cmp_type;

    // Monster tag 0 is POSITION
    const auto monster_pos = resolve_timeline_property(cmp_type::kMonster, 0);
    TL_CHECK(monster_pos.has_value());
    TL_CHECK(monster_pos->kind == TimelinePropertyKind::Position);
    TL_CHECK(monster_pos->value_type == TimelineValueType::Vector3);
    TL_CHECK_EQ(std::string(monster_pos->name), "POSITION");

    // Generic Model tag 1 is POSITION
    const auto model_pos = resolve_timeline_property(cmp_type::kModel, 1);
    TL_CHECK(model_pos.has_value());
    TL_CHECK(model_pos->kind == TimelinePropertyKind::Position);
    TL_CHECK(model_pos->value_type == TimelineValueType::Vector3);
    TL_CHECK_EQ(std::string(model_pos->name), "POSITION");

    // Same tag on different descriptors resolves differently:
    // tag 1 on Generic Model -> POSITION
    TL_CHECK(model_pos->kind == TimelinePropertyKind::Position);
    // tag 1 on Monster -> FORWARD
    const auto monster_fwd = resolve_timeline_property(cmp_type::kMonster, 1);
    TL_CHECK(monster_fwd.has_value());
    TL_CHECK(monster_fwd->kind == TimelinePropertyKind::Forward);
    TL_CHECK_EQ(std::string(monster_fwd->name), "FORWARD");

    // tag 0 on Monster -> POSITION
    TL_CHECK(monster_pos->kind == TimelinePropertyKind::Position);
    // tag 0 on Generic Model -> VISIBLE
    const auto model_vis = resolve_timeline_property(cmp_type::kModel, 0);
    TL_CHECK(model_vis.has_value());
    TL_CHECK(model_vis->kind == TimelinePropertyKind::Visible);
    TL_CHECK(model_vis->value_type == TimelineValueType::Bool);
    TL_CHECK_EQ(std::string(model_vis->name), "VISIBLE");

    // Unknown tag on known descriptor returns std::nullopt
    TL_CHECK(!resolve_timeline_property(cmp_type::kModel, 999).has_value());
    TL_CHECK(!resolve_timeline_property(cmp_type::kMonster, 999).has_value());

    // Unknown descriptor returns std::nullopt
    TL_CHECK(!resolve_timeline_property(9999, 0).has_value());

    // The census-only descriptors, by their `cmp_type` ids (21, 24, 55).
    TL_CHECK_EQ(std::string(TL_VALUE(resolve_timeline_property(cmp_type::kLinearForce, 1)).name),
                "ENABLE");
    TL_CHECK_EQ(std::string(TL_VALUE(resolve_timeline_property(cmp_type::kGravityWell, 1)).name),
                "ENABLE");
    TL_CHECK(TL_VALUE(resolve_timeline_property(cmp_type::kDamageShape, 0)).kind ==
             TimelinePropertyKind::Position);
    // Room Piece 13 is `SCALE Z`; Property Node 9 is `ENABLED`, a bool.
    TL_CHECK_EQ(std::string(TL_VALUE(resolve_timeline_property(cmp_type::kRoomPiece, 13)).name),
                "SCALE Z");
    TL_CHECK(TL_VALUE(resolve_timeline_property(cmp_type::kMarker, 9)).value_type ==
             TimelineValueType::Bool);

    // Boolean property (e.g. VISIBLE) gets NoInterpolation
    TL_CHECK_EQ(tl::core::timeline_property_interpolation(0, model_vis->value_type),
                TimelineInterpolation::NoInterpolation);
}
