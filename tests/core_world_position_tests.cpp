#include <shark/world_position.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <type_traits>

namespace {

shark::WorldId world(const bool second = false) {
    const auto id = shark::StableId::parse(second ? "12345678-1234-4234-8234-123456789abc"
                                                  : "12345678-1234-4234-8234-123456789abd");
    REQUIRE(id);
    return shark::WorldId{id.value()};
}

shark::WorldPosition position(const shark::WorldCell cell = {},
                              const std::array<double, 3> offset = {}) {
    return {world(), cell, offset};
}

std::array<std::int64_t, 3> axes(const shark::WorldCell cell) {
    return {cell.x, cell.y, cell.z};
}

void require_canonical(const shark::WorldPosition& value) {
    REQUIRE(shark::validate_world_position(value));
    for (const auto offset : value.offset) {
        CHECK(std::isfinite(offset));
        CHECK(offset >= 0.0);
        CHECK(offset < 1024.0);
        CHECK_FALSE(std::signbit(offset));
    }
}

class RoundingRestore final {
  public:
    RoundingRestore() : original_(std::fegetround()) {}
    ~RoundingRestore() {
        static_cast<void>(std::fesetround(original_));
    }
    RoundingRestore(const RoundingRestore&) = delete;
    RoundingRestore& operator=(const RoundingRestore&) = delete;

  private:
    int original_;
};

static_assert(!std::is_same_v<shark::WorldId, shark::SceneId>);

} // namespace

TEST_CASE("world position contract normalizes integer cells and local offsets",
          "[world-position]") {
    CHECK(shark::world_position_version == 1);
    CHECK(shark::world_cell_size_meters == 1024.0);
    CHECK(shark::world_cell_min == -2'147'483'648LL);
    CHECK(shark::world_cell_max == 2'147'483'647LL);

    const auto input = position({10, -10, 2'000'000'000}, {2048.25, -1024.5, 0.0000001});
    const auto normalized = shark::normalize_world_position(input);
    REQUIRE(normalized);
    CHECK(normalized.value().world == input.world);
    CHECK(normalized.value().cell == shark::WorldCell{12, -12, 2'000'000'000});
    CHECK(normalized.value().offset == std::array{0.25, 1023.5, 0.0000001});
    require_canonical(normalized.value());

    const auto repeated = shark::normalize_world_position(normalized.value());
    REQUIRE(repeated);
    CHECK(repeated.value() == normalized.value());
    CHECK(input.cell == shark::WorldCell{10, -10, 2'000'000'000});
    CHECK(input.offset == std::array{2048.25, -1024.5, 0.0000001});

    auto other_world = input;
    other_world.world = world(true);
    const auto other_normalized = shark::normalize_world_position(other_world);
    REQUIRE(other_normalized);
    CHECK(other_normalized.value().world == other_world.world);
    CHECK(other_normalized.value().world != normalized.value().world);
    CHECK(other_normalized.value().cell == normalized.value().cell);
    CHECK(other_normalized.value().offset == normalized.value().offset);
}

TEST_CASE("world position boundaries have a canonical carry and positive zero",
          "[world-position]") {
    const auto exact = shark::normalize_world_position(position({}, {1024.0, -1024.0, -0.0}));
    REQUIRE(exact);
    CHECK(exact.value().cell == shark::WorldCell{1, -1, 0});
    CHECK(exact.value().offset == std::array{0.0, 0.0, 0.0});
    require_canonical(exact.value());

    const auto below = std::nextafter(1024.0, 0.0);
    const auto above = std::nextafter(1024.0, std::numeric_limits<double>::infinity());
    const auto adjacent = shark::normalize_world_position(position({}, {below, above, -above}));
    REQUIRE(adjacent);
    CHECK(adjacent.value().cell == shark::WorldCell{0, 1, -2});
    CHECK(adjacent.value().offset == std::array{below, above - 1024.0, 1024.0 - (above - 1024.0)});
    require_canonical(adjacent.value());

    // Half of the gap immediately below 1024 ties to the even upper endpoint.
    const auto half_gap = (1024.0 - below) / 2.0;
    const auto beyond_half_gap = std::nextafter(half_gap, std::numeric_limits<double>::infinity());
    const auto negative = shark::normalize_world_position(
        position({}, {-std::numeric_limits<double>::denorm_min(), -half_gap, -beyond_half_gap}));
    REQUIRE(negative);
    CHECK(negative.value().cell == shark::WorldCell{0, 0, -1});
    CHECK(negative.value().offset == std::array{0.0, 0.0, below});
    require_canonical(negative.value());
}

TEST_CASE("world position validation rejects noncanonical and invalid values", "[world-position]") {
    for (const auto invalid : std::array{
             -0.0, -1.0, 1024.0, std::numeric_limits<double>::infinity(),
             -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            auto candidate = position();
            candidate.offset[axis] = invalid;
            const auto result = shark::validate_world_position(candidate);
            REQUIRE_FALSE(result);
            CHECK(result.error().code == shark::ErrorCode::invalid_argument);
            CHECK(result.error().context.operation == "world-position.validate");
        }
    }
    for (const auto invalid : std::array{shark::world_cell_min - 1, shark::world_cell_max + 1,
                                         std::numeric_limits<std::int64_t>::min(),
                                         std::numeric_limits<std::int64_t>::max()}) {
        for (const auto cell :
             std::array{shark::WorldCell{invalid, 0, 0}, shark::WorldCell{0, invalid, 0},
                        shark::WorldCell{0, 0, invalid}}) {
            CHECK_FALSE(shark::validate_world_position(position(cell)));
            CHECK_FALSE(shark::normalize_world_position(position(cell)));
        }
    }
    CHECK(shark::validate_world_position(
        position({shark::world_cell_min, shark::world_cell_max, 0},
                 {0.0, std::nextafter(1024.0, 0.0), std::numeric_limits<double>::denorm_min()})));
}

TEST_CASE("world position normalization rejects overflow without changing the candidate",
          "[world-position]") {
    for (const auto invalid : std::array{
             std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::max(),
             -std::numeric_limits<double>::max(), 4'398'046'511'104.0, -4'398'046'511'104.0}) {
        const auto result = shark::normalize_world_position(position({}, {0.0, 0.0, invalid}));
        REQUIRE_FALSE(result);
        CHECK(result.error().code == shark::ErrorCode::invalid_argument);
        CHECK(result.error().context.operation == "world-position.normalize");
    }

    CHECK_FALSE(
        shark::normalize_world_position(position({shark::world_cell_max, 0, 0}, {1024, 0, 0})));
    CHECK_FALSE(
        shark::normalize_world_position(position({0, shark::world_cell_min, 0}, {0, -1, 0})));
    CHECK_FALSE(
        shark::normalize_world_position(position({shark::world_cell_min - 1, 0, 0}, {1024, 0, 0})));
    const auto at_lower_boundary = shark::normalize_world_position(position(
        {shark::world_cell_min, 0, 0}, {-std::numeric_limits<double>::denorm_min(), 0, 0}));
    REQUIRE(at_lower_boundary);
    CHECK(at_lower_boundary.value().cell.x == shark::world_cell_min);
    CHECK(at_lower_boundary.value().offset[0] == 0.0);

    const auto input = position({0, shark::world_cell_max, 0}, {2048.0, 1024.0, 0.0});
    CHECK_FALSE(shark::normalize_world_position(input));
    CHECK(input.cell == shark::WorldCell{0, shark::world_cell_max, 0});
    CHECK(input.offset == std::array{2048.0, 1024.0, 0.0});

    // The complete representable cell range is a valid integer carry.
    const auto entire_range =
        static_cast<double>(shark::world_cell_max - shark::world_cell_min) * 1024.0;
    const auto upper = shark::normalize_world_position(
        position({shark::world_cell_min, 0, 0}, {entire_range, 0, 0}));
    REQUIRE(upper);
    CHECK(upper.value().cell.x == shark::world_cell_max);
    const auto lower = shark::normalize_world_position(
        position({shark::world_cell_max, 0, 0}, {-entire_range, 0, 0}));
    REQUIRE(lower);
    CHECK(lower.value().cell.x == shark::world_cell_min);
}

TEST_CASE("world translation preserves local detail and crosses cell boundaries",
          "[world-position]") {
    const auto input = position({2'000'000'000, -2'000'000'000, 0}, {0.0000001, 1023.75, 0.25});
    const auto translated = shark::translate_world_position(input, {1024.0, 0.5, -0.5});
    REQUIRE(translated);
    CHECK(translated.value().world == input.world);
    CHECK(translated.value().cell == shark::WorldCell{2'000'000'001, -1'999'999'999, -1});
    CHECK(translated.value().offset == std::array{0.0000001, 0.25, 1023.75});
    require_canonical(translated.value());
    const auto reverse = shark::translate_world_position(translated.value(), {-1024.0, -0.5, 0.5});
    REQUIRE(reverse);
    CHECK(reverse.value() == input);

    const auto huge_delta = shark::translate_world_position(
        position({shark::world_cell_min, 0, 0}, {0.0000001, 0, 0}),
        {static_cast<double>(shark::world_cell_max - shark::world_cell_min) * 1024.0, 0, 0});
    REQUIRE(huge_delta);
    CHECK(huge_delta.value().cell.x == shark::world_cell_max);
    CHECK(huge_delta.value().offset[0] == 0.0000001);

    const auto tiny = std::numeric_limits<double>::denorm_min();
    const auto cancel = shark::translate_world_position(position({}, {tiny, 0, 0}), {-tiny, 0, 0});
    REQUIRE(cancel);
    CHECK(cancel.value().cell == shark::WorldCell{});
    CHECK(cancel.value().offset == std::array{0.0, 0.0, 0.0});
    require_canonical(cancel.value());
}

TEST_CASE("world translation validates input and rejects atomic out-of-range results",
          "[world-position]") {
    const auto input = position({0, shark::world_cell_max, 0}, {1.0, 1023.0, 1.0});
    const auto failed = shark::translate_world_position(input, {1024.0, 1.0, 1024.0});
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == shark::ErrorCode::invalid_argument);
    CHECK(failed.error().context.operation == "world-position.translate");
    CHECK(input.cell == shark::WorldCell{0, shark::world_cell_max, 0});
    CHECK(input.offset == std::array{1.0, 1023.0, 1.0});

    CHECK_FALSE(shark::translate_world_position(position({}, {1024.0, 0, 0}), {}));
    CHECK_FALSE(shark::translate_world_position(position({}, {-0.0, 0, 0}), {}));
    CHECK_FALSE(shark::translate_world_position(position({0, 0, shark::world_cell_min - 1}), {}));
    for (const auto invalid : std::array{
             std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::max(),
             -std::numeric_limits<double>::max()}) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            std::array<double, 3> delta{};
            delta[axis] = invalid;
            const auto result = shark::translate_world_position(position(), delta);
            REQUIRE_FALSE(result);
            CHECK(result.error().code == shark::ErrorCode::invalid_argument);
            CHECK(result.error().context.operation == "world-position.translate");
        }
    }
}

TEST_CASE("world position arithmetic rejects a changed rounding mode", "[world-position]") {
    const RoundingRestore restore;
    const auto input = position();
    REQUIRE(std::fesetround(FE_DOWNWARD) == 0);
    CHECK(shark::validate_world_position(input));
    const auto normalized = shark::normalize_world_position(input);
    REQUIRE_FALSE(normalized);
    CHECK(normalized.error().code == shark::ErrorCode::invalid_configuration);
    CHECK(normalized.error().context.operation == "world-position.normalize");
    const auto translated = shark::translate_world_position(input, {});
    REQUIRE_FALSE(translated);
    CHECK(translated.error().code == shark::ErrorCode::invalid_configuration);
    CHECK(translated.error().context.operation == "world-position.translate");
}

TEST_CASE("world position fixed-seed grid agrees with integer reference and round trips",
          "[world-position]") {
    std::mt19937 random{1337};
    constexpr std::int64_t units_per_cell = 16 * 1024;
    for (int sample = 0; sample < 512; ++sample) {
        std::array<std::int64_t, 3> cells{};
        std::array<std::int64_t, 3> offsets{};
        std::array<double, 3> offset_meters{};
        std::array<double, 3> delta{};
        std::array<double, 3> reverse_delta{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            cells[axis] = static_cast<std::int64_t>(random()) % 4'000'000'001LL - 2'000'000'000LL;
            offsets[axis] = static_cast<std::int64_t>(random()) % 2'000'001LL - 1'000'000LL;
            offset_meters[axis] = static_cast<double>(offsets[axis]) / 16.0;
            delta[axis] = static_cast<double>(static_cast<std::int64_t>(random()) % 2'000'001LL -
                                              1'000'000LL) /
                          16.0;
            reverse_delta[axis] = -delta[axis];
        }
        const auto input = position({cells[0], cells[1], cells[2]}, offset_meters);
        const auto normalized = shark::normalize_world_position(input);
        REQUIRE(normalized);
        require_canonical(normalized.value());
        const auto normalized_cells = axes(normalized.value().cell);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto total_units = cells[axis] * units_per_cell + offsets[axis];
            auto expected_cell = total_units / units_per_cell;
            auto expected_offset = total_units % units_per_cell;
            if (expected_offset < 0) {
                --expected_cell;
                expected_offset += units_per_cell;
            }
            CHECK(normalized_cells[axis] == expected_cell);
            CHECK(normalized.value().offset[axis] == static_cast<double>(expected_offset) / 16.0);
        }
        const auto repeated = shark::normalize_world_position(normalized.value());
        REQUIRE(repeated);
        CHECK(repeated.value() == normalized.value());
        const auto moved = shark::translate_world_position(normalized.value(), delta);
        REQUIRE(moved);
        require_canonical(moved.value());
        const auto returned = shark::translate_world_position(moved.value(), reverse_delta);
        REQUIRE(returned);
        CHECK(returned.value() == normalized.value());
    }
}

TEST_CASE("bounded local coordinates preserve nearby detail at distant world cells",
          "[world-position]") {
    const auto origin = position({2'000'000'000, -2'000'000'000, 7}, {1023.75, 12.5, 0.125});
    const std::array local{0.5, -1024.25, 2048.125};
    const auto global = shark::world_position_from_local(origin, local, 4096.0);
    REQUIRE(global);
    CHECK(global.value().cell == shark::WorldCell{2'000'000'001, -2'000'000'001, 9});
    CHECK(global.value().offset == std::array{0.25, 12.25, 0.25});
    const auto relative = shark::world_position_relative_to(global.value(), origin, 4096.0);
    REQUIRE(relative);
    CHECK(relative.value() == local);

    const auto distant = position({2'000'000'000, -2'000'000'000, 0});
    const auto nearby = position(distant.cell, {0.0000001, 0.25, 0});
    const auto precise = shark::world_position_relative_to(nearby, distant, 1.0);
    REQUIRE(precise);
    CHECK(precise.value() == std::array{0.0000001, 0.25, 0.0});
    const auto repeated = shark::world_position_from_local(distant, precise.value(), 1.0);
    REQUIRE(repeated);
    CHECK(repeated.value() == nearby);
    const auto zero = shark::world_position_relative_to(origin, origin, 1.0);
    REQUIRE(zero);
    for (const auto value : zero.value()) {
        CHECK(value == 0.0);
        CHECK_FALSE(std::signbit(value));
    }
}

TEST_CASE("bounded local coordinates admit exact endpoints and reject excess", "[world-position]") {
    const auto origin = position();
    constexpr auto bound = shark::world_local_coordinate_limit;
    const auto global = shark::world_position_from_local(origin, {bound, -bound, -0.0}, bound);
    REQUIRE(global);
    const auto relative = shark::world_position_relative_to(global.value(), origin, bound);
    REQUIRE(relative);
    CHECK(relative.value() == std::array{bound, -bound, 0.0});
    CHECK_FALSE(std::signbit(relative.value()[2]));
    const auto beyond = std::nextafter(bound, std::numeric_limits<double>::infinity());
    CHECK_FALSE(shark::world_position_from_local(origin, {beyond, 0, 0}, bound));
    CHECK_FALSE(shark::world_position_from_local(origin, {0, -beyond, 0}, bound));
    const auto too_distant = shark::translate_world_position(origin, {beyond, 0, 0});
    REQUIRE(too_distant);
    CHECK_FALSE(shark::world_position_relative_to(too_distant.value(), origin, bound));
    CHECK_FALSE(shark::world_position_relative_to(global.value(), origin, 1.0));

    for (const auto invalid :
         std::array{0.0, -0.0, -1.0, beyond, std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::quiet_NaN()}) {
        CHECK_FALSE(shark::world_position_relative_to(origin, origin, invalid));
        CHECK_FALSE(shark::world_position_from_local(origin, {}, invalid));
    }
}

TEST_CASE("local coordinate conversion checks world identity canonical inputs and overflow",
          "[world-position]") {
    const auto origin = position();
    auto foreign = origin;
    foreign.world = world(true);
    CHECK_FALSE(shark::world_position_relative_to(foreign, origin, 1024));
    auto invalid = origin;
    invalid.offset[1] = -0.0;
    CHECK_FALSE(shark::world_position_relative_to(invalid, origin, 1024));
    CHECK_FALSE(shark::world_position_relative_to(origin, invalid, 1024));
    CHECK_FALSE(shark::world_position_from_local(invalid, {}, 1024));
    invalid.cell.x = std::numeric_limits<std::int64_t>::max();
    CHECK_FALSE(shark::world_position_relative_to(invalid, origin, 1024));

    const auto lower = position({shark::world_cell_min, 0, 0});
    const auto upper = position({shark::world_cell_max, 0, 0});
    CHECK_FALSE(shark::world_position_relative_to(upper, lower, 65536));
    CHECK_FALSE(shark::world_position_relative_to(lower, upper, 65536));
    const auto overflow = shark::world_position_from_local(upper, {1024, 0, 0}, 1024);
    REQUIRE_FALSE(overflow);
    CHECK(overflow.error().context.operation == "world-position.from-local");
    CHECK_FALSE(shark::world_position_from_local(lower, {-1, 0, 0}, 1024));
    for (const auto value : std::array{std::numeric_limits<double>::infinity(),
                                       -std::numeric_limits<double>::infinity(),
                                       std::numeric_limits<double>::quiet_NaN()}) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            std::array<double, 3> local{};
            local[axis] = value;
            CHECK_FALSE(shark::world_position_from_local(origin, local, 1024));
        }
    }
}

TEST_CASE("packed local positions retain frame generations and declare float loss",
          "[world-position]") {
    const shark::WorldLocalFrame frame{position({2'000'000'000, -2'000'000'000, 7}), 19};
    const auto source = position(frame.origin.cell, {0.1, 0.25, 0.0});
    const auto packed = shark::pack_world_position(source, frame);
    REQUIRE(packed);
    CHECK(packed.value().origin_generation == 19);
    CHECK(packed.value().meters == std::array{0.1F, 0.25F, 0.0F});
    CHECK_FALSE(std::signbit(packed.value().meters[2]));
    const auto unpacked = shark::unpack_world_position(packed.value(), frame);
    REQUIRE(unpacked);
    CHECK(unpacked.value().world == source.world);
    CHECK(unpacked.value().cell == source.cell);
    CHECK(unpacked.value().offset[0] == static_cast<double>(0.1F));
    CHECK(unpacked.value().offset[0] != source.offset[0]);
    CHECK(std::abs(unpacked.value().offset[0] - source.offset[0]) < 0.00000001);
    CHECK(unpacked.value().offset[1] == 0.25);

    auto changed = frame;
    changed.origin = position(frame.origin.cell, {1, 0, 0});
    ++changed.generation;
    const auto stale = shark::unpack_world_position(packed.value(), changed);
    REQUIRE_FALSE(stale);
    CHECK(stale.error().code == shark::ErrorCode::conflict);
    changed.generation = 0;
    CHECK_FALSE(shark::pack_world_position(source, changed));
    CHECK_FALSE(shark::unpack_world_position(packed.value(), changed));
    auto missing = packed.value();
    missing.origin_generation = 0;
    CHECK_FALSE(shark::unpack_world_position(missing, frame));
}

TEST_CASE("packed local positions validate float bounds canonical values and rounding",
          "[world-position]") {
    const shark::WorldLocalFrame frame{position(), 1};
    constexpr auto bound = shark::world_local_coordinate_limit;
    const auto endpoint = shark::world_position_from_local(frame.origin, {bound, -bound, 0}, bound);
    REQUIRE(endpoint);
    const auto packed = shark::pack_world_position(endpoint.value(), frame);
    REQUIRE(packed);
    const auto unpacked = shark::unpack_world_position(packed.value(), frame);
    REQUIRE(unpacked);
    CHECK(unpacked.value() == endpoint.value());
    for (const auto invalid : std::array{
             -0.0F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(),
             std::nextafter(65536.0F, std::numeric_limits<float>::infinity())}) {
        auto candidate = packed.value();
        candidate.meters[1] = invalid;
        CHECK_FALSE(shark::unpack_world_position(candidate, frame));
    }
    const auto too_distant = shark::translate_world_position(frame.origin, {bound + 1, 0, 0});
    REQUIRE(too_distant);
    CHECK_FALSE(shark::pack_world_position(too_distant.value(), frame));
    auto foreign = frame.origin;
    foreign.world = world(true);
    CHECK_FALSE(shark::pack_world_position(foreign, frame));

    const RoundingRestore restore;
    REQUIRE(std::fesetround(FE_DOWNWARD) == 0);
    const auto relative = shark::world_position_relative_to(frame.origin, frame.origin, 1024);
    REQUIRE_FALSE(relative);
    CHECK(relative.error().code == shark::ErrorCode::invalid_configuration);
    const auto global = shark::world_position_from_local(frame.origin, {}, 1024);
    REQUIRE_FALSE(global);
    CHECK(global.error().code == shark::ErrorCode::invalid_configuration);
    const auto pack = shark::pack_world_position(frame.origin, frame);
    REQUIRE_FALSE(pack);
    CHECK(pack.error().code == shark::ErrorCode::invalid_configuration);
    const auto unpack = shark::unpack_world_position(packed.value(), frame);
    REQUIRE_FALSE(unpack);
    CHECK(unpack.error().code == shark::ErrorCode::invalid_configuration);
}

TEST_CASE("packed local rounding preserves global representability at world edges",
          "[world-position]") {
    const shark::WorldLocalFrame upper_frame{position({shark::world_cell_max, 0, 0}), 1};
    const auto rounded_outside = position(upper_frame.origin.cell, {1023.99999, 0, 0});
    const auto rejected = shark::pack_world_position(rounded_outside, upper_frame);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().context.operation == "world-position.pack");

    const auto inside = position(upper_frame.origin.cell, {1023.5, 0, 0});
    const auto packed_inside = shark::pack_world_position(inside, upper_frame);
    REQUIRE(packed_inside);
    const auto unpacked_inside = shark::unpack_world_position(packed_inside.value(), upper_frame);
    REQUIRE(unpacked_inside);
    CHECK(unpacked_inside.value() == inside);

    const shark::WorldLocalFrame lower_frame{
        position({shark::world_cell_min, 0, 0}, {std::numeric_limits<double>::denorm_min(), 0, 0}),
        2};
    const auto lower_edge = position(lower_frame.origin.cell);
    const auto packed_zero = shark::pack_world_position(lower_edge, lower_frame);
    REQUIRE(packed_zero);
    CHECK(packed_zero.value().meters[0] == 0.0F);
    CHECK_FALSE(std::signbit(packed_zero.value().meters[0]));
    const auto unpacked_zero = shark::unpack_world_position(packed_zero.value(), lower_frame);
    REQUIRE(unpacked_zero);
    CHECK(unpacked_zero.value() == lower_frame.origin);
}
