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
