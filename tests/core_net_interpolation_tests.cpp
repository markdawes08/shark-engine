#include <shark/net/replication.hpp>
#include <shark/transform.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <limits>
#include <numbers>
#include <thread>

namespace {
using namespace shark;
using namespace shark::net;
using namespace std::chrono_literals;

template <typename T> T take(Result<T> result) {
    if (!result)
        INFO(result.error().context.operation << ": " << result.error().message);
    REQUIRE(result);
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result)
        INFO(result.error().context.operation << ": " << result.error().message);
    REQUIRE(result);
}
template <typename T> T id(unsigned value) {
    std::string text = "a04b0000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[value & 15U];
        value >>= 4U;
    }
    return T{take(StableId::parse(text))};
}
SessionSnapshot session() {
    return {id<SessionId>(1), id<AccountId>(2), id<CharacterId>(3), 1, 1, true, true};
}
ReplicatedPose pose(double x, double degrees = 0, math::Vec3 scale = {1, 1, 1}) {
    Transform value;
    value.translation.x = x;
    value.rotation = glm::angleAxis(degrees * std::numbers::pi / 180, math::Vec3{0, 1, 0});
    value.scale = scale;
    const auto matrix = take(transform_matrix(value));
    ReplicatedPose result;
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 4; ++column)
            result.coefficients[static_cast<std::size_t>(row * 4 + column)] = matrix[column][row];
    return take(canonicalize_replicated_pose(result));
}
ReplicationUpdate baseline(double x = 0) {
    return {session().session,
            1,
            id<ReplicaSessionId>(4),
            1,
            0,
            true,
            {{ReplicationChange::spawn, {1}, pose(x)}}};
}
ReplicationUpdate delta(std::uint64_t revision, double x) {
    return {session().session,
            1,
            id<ReplicaSessionId>(4),
            revision,
            revision - 1,
            false,
            {{ReplicationChange::update, {1}, pose(x)}}};
}
double x(const ReplicaPresentationSnapshot& value, std::size_t index = 0) {
    return value.entities.at(index).pose.coefficients[3];
}
} // namespace

TEST_CASE("Replica presentation interpolates owned translation and shortest-arc rotation",
          "[net-interpolation]") {
    auto cache = take(ReplicaClient::create(session()));
    REQUIRE(take(cache->apply(baseline(), 0ns)) == ReplicaApplyResult::applied);
    auto moved = delta(2, 3);
    moved.changes.front().pose = pose(3, 90);
    REQUIRE(take(cache->apply(moved, 100ms)) == ReplicaApplyResult::applied);
    const auto middle = take(cache->sample(50ms));
    REQUIRE(middle.interpolated);
    REQUIRE(middle.has_baseline);
    REQUIRE(middle.stream == moved.stream);
    REQUIRE(middle.revision == 2);
    REQUIRE(x(middle) == Catch::Approx(1.5));
    const auto& basis = middle.entities.front().pose.coefficients;
    REQUIRE(basis[0] == Catch::Approx(std::sqrt(0.5)));
    REQUIRE(basis[2] == Catch::Approx(std::sqrt(0.5)));
    REQUIRE(basis[8] == Catch::Approx(-std::sqrt(0.5)));
    REQUIRE(x(take(cache->sample(0ns))) == 0);
    REQUIRE(x(take(cache->sample(1s))) == 3);
    REQUIRE_FALSE(take(cache->sample(1s)).interpolated);
    REQUIRE(take(cache->snapshot()).entities.front().pose == *moved.changes.front().pose);

    auto shortest = take(ReplicaClient::create(session()));
    auto first = baseline();
    first.changes.front().pose = pose(0, 170);
    take(shortest->apply(first, 0ns));
    auto last = delta(2, 0);
    last.changes.front().pose = pose(0, -170);
    take(shortest->apply(last, 100ms));
    const auto rotation = take(shortest->sample(50ms));
    REQUIRE(rotation.interpolated);
    REQUIRE(rotation.entities.front().pose.coefficients[0] == Catch::Approx(-1));
    REQUIRE(rotation.entities.front().pose.coefficients[10] == Catch::Approx(-1));
}

TEST_CASE("Replica duplicates preserve frame times and equal receive times coalesce",
          "[net-interpolation]") {
    auto cache = take(ReplicaClient::create(session()));
    take(cache->apply(baseline(), 0ns));
    const auto moved = delta(2, 2);
    take(cache->apply(moved, 100ms));
    REQUIRE(take(cache->apply(moved, 200ms)) == ReplicaApplyResult::duplicate);
    REQUIRE(take(cache->stats()).presentation_frames == 2);
    REQUIRE(x(take(cache->sample(50ms))) == Catch::Approx(1));
    REQUIRE_FALSE(cache->apply(delta(3, 4), 150ms));
    REQUIRE(take(cache->snapshot()).revision == 2);
    take(cache->apply(delta(3, 4), 200ms));
    take(cache->apply(delta(4, 6), 200ms));
    REQUIRE(take(cache->stats()).presentation_frames == 3);
    REQUIRE(x(take(cache->sample(150ms))) == Catch::Approx(4));
    REQUIRE(x(take(cache->sample(200ms))) == 6);
}

TEST_CASE("Replica receive history remains bounded and clamps without extrapolation",
          "[net-interpolation]") {
    auto cache = take(ReplicaClient::create(session()));
    take(cache->apply(baseline(), 0ns));
    for (std::uint64_t index = 1; index <= 12; ++index) {
        take(cache->apply(delta(index + 1, static_cast<double>(index)),
                          std::chrono::milliseconds{10 * static_cast<std::int64_t>(index)}));
        REQUIRE(take(cache->stats()).presentation_frames <= replica_presentation_maximum_frames);
    }
    REQUIRE(take(cache->stats()).presentation_frames == 8);
    REQUIRE(take(cache->stats()).presentation_entities == 8);
    REQUIRE(x(take(cache->sample(0ns))) == 5);
    REQUIRE(x(take(cache->sample(10s))) == 12);
    REQUIRE_FALSE(take(cache->sample(10s)).interpolated);
    REQUIRE(x(take(cache->sample(105ms))) == Catch::Approx(10.5));
}

TEST_CASE("Delayed replica presentation preserves current topology and baseline fences",
          "[net-interpolation]") {
    auto cache = take(ReplicaClient::create(session()));
    auto first = baseline();
    first.changes.push_back({ReplicationChange::spawn, {2}, pose(10)});
    take(cache->apply(first, 0ns));
    auto changed = delta(2, 2);
    changed.changes.push_back({ReplicationChange::despawn, {2}, std::nullopt});
    changed.changes.push_back({ReplicationChange::spawn, {3}, pose(30)});
    take(cache->apply(changed, 100ms));
    const auto delayed = take(cache->sample(50ms));
    REQUIRE(delayed.entities.size() == 2);
    REQUIRE(delayed.entities[0].id.value == 1);
    REQUIRE(delayed.entities[1].id.value == 3);
    REQUIRE(x(delayed) == Catch::Approx(1));
    REQUIRE(x(delayed, 1) == 30);
    const auto clamped = take(cache->sample(0ns));
    REQUIRE(clamped.entities[1].id.value == 3);
    REQUIRE(x(clamped, 1) == 30);

    auto replacement = baseline(10);
    replacement.revision = 3;
    replacement.changes.push_back({ReplicationChange::spawn, {3}, pose(40)});
    take(cache->apply(replacement, 200ms));
    REQUIRE(take(cache->stats()).presentation_frames == 1);
    REQUIRE(x(take(cache->sample(0ns))) == 10);
    take(cache->discard_baseline());
    REQUIRE(take(cache->stats()).presentation_frames == 0);
    REQUIRE_FALSE(take(cache->sample(0ns)).has_baseline);
    REQUIRE(take(cache->sample(0ns)).entities.empty());
    REQUIRE(take(cache->apply(replacement, 300ms)) == ReplicaApplyResult::resync_required);
    REQUIRE(take(cache->stats()).presentation_frames == 0);
    replacement.revision = 4;
    replacement.changes.front().pose = pose(100);
    take(cache->apply(replacement, 300ms));
    REQUIRE(x(take(cache->sample(0ns))) == 100);
    auto retired = delta(5, 101);
    retired.changes.push_back({ReplicationChange::spawn, {2}, pose(20)});
    REQUIRE_FALSE(cache->apply(retired, 400ms));
    REQUIRE(take(cache->snapshot()).revision == 4);
}

TEST_CASE("Legal affine shear and reflection changes retain complete committed fallback poses",
          "[net-interpolation]") {
    SECTION("hierarchy-induced shear remains legal cache content") {
        auto cache = take(ReplicaClient::create(session()));
        auto first = baseline();
        first.changes.front().pose->coefficients[1] = 0.5;
        take(cache->apply(first, 0ns));
        auto last = delta(2, 4);
        last.changes.front().pose->coefficients[1] = 0.75;
        take(cache->apply(last, 100ms));
        const auto display = take(cache->sample(50ms));
        REQUIRE_FALSE(display.interpolated);
        REQUIRE(display.entities.front().pose == *last.changes.front().pose);
        REQUIRE(take(cache->snapshot()).entities.front().pose == *last.changes.front().pose);
    }
    SECTION("crossing a reflection never generates a singular interpolated basis") {
        auto cache = take(ReplicaClient::create(session()));
        take(cache->apply(baseline(), 0ns));
        auto reflected = delta(2, 2);
        reflected.changes.front().pose = pose(2, 0, {-1, 1, 1});
        take(cache->apply(reflected, 100ms));
        const auto display = take(cache->sample(50ms));
        REQUIRE_FALSE(display.interpolated);
        REQUIRE(display.entities.front().pose == *reflected.changes.front().pose);
    }
    SECTION("matching reflected scales interpolate without crossing zero") {
        auto cache = take(ReplicaClient::create(session()));
        auto first = baseline();
        first.changes.front().pose = pose(0, 0, {-1, 1, 1});
        take(cache->apply(first, 0ns));
        auto last = delta(2, 2);
        last.changes.front().pose = pose(2, 0, {-2, 1, 1});
        take(cache->apply(last, 100ms));
        const auto display = take(cache->sample(50ms));
        REQUIRE(display.interpolated);
        REQUIRE(display.entities.front().pose.coefficients[0] == Catch::Approx(-1.5));
    }
}

TEST_CASE("Replica history rejects invalid observations atomically and retains untimed behavior",
          "[net-interpolation]") {
    auto cache = take(ReplicaClient::create(session()));
    REQUIRE_FALSE(cache->apply(baseline(), -1ns));
    REQUIRE_FALSE(cache->sample(-1ns));
    take(cache->apply(baseline(), 10ms));
    const auto expected = take(cache->snapshot());
    auto invalid = delta(2, 2);
    invalid.changes.front().pose->coefficients[0] = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_FALSE(cache->apply(invalid, 20ms));
    auto foreign = delta(2, 2);
    foreign.session = id<SessionId>(90);
    REQUIRE_FALSE(cache->apply(foreign, 20ms));
    REQUIRE(take(cache->stats()).presentation_frames == 1);
    REQUIRE(take(cache->snapshot()).revision == expected.revision);
    REQUIRE(take(cache->snapshot()).entities == expected.entities);
    auto skipped = delta(3, 3);
    REQUIRE(take(cache->apply(skipped, 20ms)) == ReplicaApplyResult::resync_required);
    REQUIRE(take(cache->stats()).presentation_frames == 1);
    REQUIRE(take(cache->snapshot()).entities == expected.entities);
    bool rejected_thread = false;
    std::thread other{[&] { rejected_thread = !cache->sample(20ms) && !cache->stats(); }};
    other.join();
    REQUIRE(rejected_thread);
    take(cache->apply(delta(2, 2)));
    REQUIRE(take(cache->stats()).presentation_frames == 0);
    REQUIRE(x(take(cache->sample(0ns))) == 2);
    REQUIRE_FALSE(take(cache->sample(0ns)).interpolated);
}
