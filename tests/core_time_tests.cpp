#include <shark/time.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

std::unique_ptr<shark::ServerScheduler> server(const shark::ServerTimingConfig config = {},
                                               const shark::MonotonicTime origin = 0ns,
                                               const std::uint64_t first_tick = 0) {
    auto result = shark::ServerScheduler::create(config, origin, first_tick);
    REQUIRE(result);
    return std::move(result).value();
}

std::unique_ptr<shark::OfflineScheduler> offline(const shark::MonotonicTime origin = 0ns,
                                                 const std::uint64_t first_tick = 0) {
    auto result = shark::OfflineScheduler::create(origin, first_tick);
    REQUIRE(result);
    return std::move(result).value();
}

shark::Result<void> complete(std::uint64_t) {
    return {};
}

class ManualClock final : public shark::MonotonicClock {
  public:
    shark::MonotonicTime time{0ns};
    shark::MonotonicTime now() noexcept override {
        return time;
    }
    shark::Result<void> wait_for(const shark::MonotonicTime duration) override {
        time += duration;
        return {};
    }
};

static_assert(!std::is_copy_constructible_v<shark::ServerScheduler>);
static_assert(!std::is_move_constructible_v<shark::ServerScheduler>);
static_assert(!std::is_copy_constructible_v<shark::OfflineScheduler>);
static_assert(!std::is_move_constructible_v<shark::OfflineScheduler>);

} // namespace

TEST_CASE("monotonic clocks are injectable and native waits are bounded", "[timing]") {
    ManualClock manual;
    shark::MonotonicClock& clock = manual;
    REQUIRE(clock.now() == 0ns);
    REQUIRE(clock.wait_for(17ms));
    REQUIRE(clock.now() == 17ms);
    auto schedule = server({}, clock.now());
    REQUIRE(clock.wait_for(50ms));
    const auto pass = schedule->poll(clock.now(), complete);
    REQUIRE(pass);
    CHECK(pass.value().executed == 1);

    shark::SteadyMonotonicClock native;
    const auto before = native.now();
    REQUIRE(native.wait_for(0ns));
    CHECK(native.now() >= before);
    CHECK(before >= 0ns);
    const auto negative = native.wait_for(-1ns);
    REQUIRE_FALSE(negative);
    CHECK(negative.error().code == shark::ErrorCode::invalid_argument);
    CHECK(negative.error().context.operation == "clock.wait");
    CHECK_FALSE(native.wait_for(1s + 1ns));
    CHECK_FALSE(native.wait_for(shark::MonotonicTime::max()));
}

TEST_CASE("server timing validates all configuration boundaries", "[timing]") {
    const std::array invalid{
        shark::ServerTimingConfig{0, 8, 200},  shark::ServerTimingConfig{1001, 8, 200},
        shark::ServerTimingConfig{20, 0, 200}, shark::ServerTimingConfig{20, 1025, 200},
        shark::ServerTimingConfig{20, 8, 0},   shark::ServerTimingConfig{20, 8, 1'000'001},
    };
    for (const auto config : invalid) {
        const auto result = shark::ServerScheduler::create(config, 0ns);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == shark::ErrorCode::invalid_configuration);
        CHECK(result.error().context.operation == "timing.server.configuration");
    }
    CHECK(shark::ServerScheduler::create({1, 1, 1}, 0ns));
    CHECK(shark::ServerScheduler::create({1000, 1024, 1'000'000}, 0ns));
    CHECK_FALSE(shark::ServerScheduler::create({}, -1ns));
    CHECK_FALSE(shark::OfflineScheduler::create(-1ns));
}

TEST_CASE("server time begins at the explicit origin and commits contiguous ticks", "[timing]") {
    auto schedule = server({}, 9s, 42);
    std::vector<std::uint64_t> ticks;
    const auto record = [&](const std::uint64_t tick) -> shark::Result<void> {
        ticks.push_back(tick);
        return {};
    };
    const auto initial = schedule->poll(9s, record);
    REQUIRE(initial);
    CHECK(initial.value().executed == 0);
    const auto fractional = schedule->poll(9s + 49ms, record);
    REQUIRE(fractional);
    CHECK(fractional.value().executed == 0);
    CHECK(fractional.value().remainder_units == 980'000'000);
    const auto first = schedule->poll(9s + 50ms, record);
    REQUIRE(first);
    CHECK(first.value().executed == 1);
    CHECK(first.value().remainder_units == 0);
    const auto second = schedule->poll(9s + 125ms, record);
    REQUIRE(second);
    CHECK(second.value().executed == 1);
    CHECK(second.value().interpolation_alpha() == 0.5);
    CHECK(ticks == std::vector<std::uint64_t>{42, 43});
    CHECK(schedule->next_tick() == 44);
    CHECK(schedule->config().tick_rate_hz == 20);
}

TEST_CASE("server catchup retains every pending tick across bounded passes", "[timing]") {
    auto schedule = server({20, 3, 20});
    std::vector<std::uint64_t> ticks;
    const auto record = [&](const std::uint64_t tick) -> shark::Result<void> {
        ticks.push_back(tick);
        return {};
    };
    for (const auto expected : std::array<std::uint64_t, 4>{7, 4, 1, 0}) {
        const auto pass = schedule->poll(525ms, record);
        REQUIRE(pass);
        CHECK(pass.value().executed <= 3);
        CHECK(pass.value().pending == expected);
        CHECK(pass.value().discarded == 0);
        CHECK(pass.value().remainder_units == 500'000'000);
    }
    REQUIRE(ticks.size() == 10);
    for (std::size_t index = 0; index < ticks.size(); ++index) {
        CHECK(ticks[index] == index);
    }
    CHECK(schedule->next_tick() == 10);
}

TEST_CASE("server debt limit is inclusive and excess debt faults without discarding", "[timing]") {
    auto schedule = server({20, 1, 3});
    const auto at_limit = schedule->poll(150ms, complete);
    REQUIRE(at_limit);
    CHECK(at_limit.value().pending == 2);
    const auto excessive = schedule->poll(250ms, complete);
    REQUIRE_FALSE(excessive);
    CHECK(excessive.error().context.operation == "timing.server.overload");
    CHECK(schedule->faulted());
    CHECK(schedule->next_tick() == 1);
    CHECK(schedule->backlog().pending == 4);
    const auto again = schedule->poll(20s, complete);
    REQUIRE_FALSE(again);
    CHECK(again.error().context.operation == "timing.server.overload");
    CHECK(schedule->backlog().pending == 4);
    CHECK(schedule->next_tick() == 1);
}

TEST_CASE("server extreme elapsed time cannot overflow rational debt", "[timing]") {
    auto schedule = server({1000, 1, 1'000'000});
    const auto result = schedule->poll(shark::MonotonicTime::max(), complete);
    REQUIRE_FALSE(result);
    CHECK(result.error().context.operation == "timing.server.overload");
    CHECK(schedule->backlog().pending == 9'223'372'036'854);
    CHECK(schedule->backlog().remainder_units == 775'807'000);
    CHECK(schedule->next_tick() == 0);
}

TEST_CASE("server clock regression faults before changing accounted debt", "[timing]") {
    auto schedule = server({20, 1, 200}, 100ms);
    REQUIRE(schedule->poll(225ms, complete));
    const auto previous = schedule->backlog();
    const auto regressed = schedule->poll(shark::MonotonicTime::min(), complete);
    REQUIRE_FALSE(regressed);
    CHECK(regressed.error().context.operation == "timing.server.clock_regression");
    CHECK(schedule->next_tick() == 1);
    CHECK(schedule->backlog().pending == previous.pending);
    CHECK(schedule->backlog().remainder_units == previous.remainder_units);
    CHECK_FALSE(schedule->poll(300ms, complete));
}

TEST_CASE("server rational ticks do not drift with irregular seeded deltas", "[timing]") {
    auto schedule = server({59, 8, 200});
    std::mt19937 random{1337};
    shark::MonotonicTime now{0};
    std::uint64_t count = 0;
    bool contiguous = true;
    const auto record = [&](const std::uint64_t tick) -> shark::Result<void> {
        contiguous = contiguous && tick == count;
        ++count;
        return {};
    };
    for (int iteration = 0; iteration < 10'000; ++iteration) {
        now += shark::MonotonicTime{static_cast<std::int64_t>(random() % 2'000'000U)};
        REQUIRE(schedule->poll(now, record));
    }
    const auto scaled = static_cast<std::uint64_t>(now.count()) * 59;
    CHECK(count == scaled / 1'000'000'000);
    CHECK(schedule->backlog().remainder_units == scaled % 1'000'000'000);
    CHECK(contiguous);
}

TEST_CASE("server failure preserves the failed tick and original owned error", "[timing]") {
    auto schedule = server();
    const auto pass = schedule->poll(200ms, [](const std::uint64_t tick) -> shark::Result<void> {
        if (tick == 2) {
            return shark::Error{shark::ErrorCode::conflict, "commit refused",
                                shark::ErrorContext{"test.tick.commit", "request-42"}};
        }
        return {};
    });
    REQUIRE_FALSE(pass);
    CHECK(pass.error().code == shark::ErrorCode::conflict);
    CHECK(pass.error().context.operation == "test.tick.commit");
    CHECK(pass.error().context.request_id == "request-42");
    CHECK(schedule->next_tick() == 2);
    CHECK(schedule->backlog().pending == 2);
    CHECK(schedule->faulted());
    const auto repeated = schedule->poll(1s, complete);
    REQUIRE_FALSE(repeated);
    CHECK(repeated.error().message == "commit refused");
    CHECK(schedule->next_tick() == 2);
}

TEST_CASE("server throwing callbacks cannot escape or retry partial simulation", "[timing]") {
    auto schedule = server();
    std::uint64_t calls = 0;
    const auto callback = [&](std::uint64_t) -> shark::Result<void> {
        ++calls;
        throw 42;
    };
    const auto pass = schedule->poll(100ms, callback);
    REQUIRE_FALSE(pass);
    CHECK(pass.error().context.operation == "timing.server.callback");
    CHECK(schedule->next_tick() == 0);
    CHECK(schedule->backlog().pending == 2);
    CHECK_FALSE(schedule->poll(200ms, callback));
    CHECK(calls == 1);
}

TEST_CASE("server cancellation is terminal and retains unexecuted tick debt", "[timing]") {
    auto schedule = server();
    std::stop_source stop;
    const auto pass = schedule->poll(
        200ms,
        [&](std::uint64_t) -> shark::Result<void> {
            static_cast<void>(stop.request_stop());
            return {};
        },
        stop.get_token());
    REQUIRE(pass);
    CHECK(pass.value().executed == 1);
    CHECK(pass.value().cancelled);
    CHECK(pass.value().pending == 3);
    CHECK(schedule->next_tick() == 1);
    const auto later = schedule->poll(20s, complete);
    REQUIRE(later);
    CHECK(later.value().cancelled);
    CHECK(later.value().pending == 3);
    CHECK(later.value().executed == 0);
    CHECK_FALSE(schedule->faulted());

    auto stopped = server();
    const auto before_first = stopped->poll(20s, complete, stop.get_token());
    REQUIRE(before_first);
    CHECK(before_first.value().pending == 0);
    CHECK(stopped->next_tick() == 0);
}

TEST_CASE("server rejects empty and reentrant callbacks without admitting duplicate time",
          "[timing]") {
    auto schedule = server();
    CHECK_FALSE(schedule->poll(100ms, {}));
    CHECK_FALSE(schedule->faulted());
    CHECK(schedule->backlog().pending == 0);
    const auto pass = schedule->poll(100ms, [&](std::uint64_t) -> shark::Result<void> {
        const auto nested = schedule->poll(1s, complete);
        REQUIRE_FALSE(nested);
        CHECK(nested.error().code == shark::ErrorCode::conflict);
        return {};
    });
    REQUIRE(pass);
    CHECK(pass.value().executed == 2);
    CHECK(schedule->next_tick() == 2);
    CHECK(schedule->backlog().pending == 0);
}

TEST_CASE("server tick exhaustion rejects before an unrepresentable acknowledgement", "[timing]") {
    auto schedule = server({}, 0ns, std::numeric_limits<std::uint64_t>::max() - 1);
    std::uint64_t calls = 0;
    const auto pass = schedule->poll(100ms, [&](std::uint64_t) -> shark::Result<void> {
        ++calls;
        return {};
    });
    REQUIRE_FALSE(pass);
    CHECK(pass.error().context.operation == "timing.server.tick_overflow");
    CHECK(calls == 1);
    CHECK(schedule->next_tick() == std::numeric_limits<std::uint64_t>::max());
    CHECK(schedule->backlog().pending == 1);
}

TEST_CASE("offline rational sixty hertz retains exact substep time", "[timing]") {
    auto schedule = offline();
    const auto near = schedule->poll(16'666'666ns, complete);
    REQUIRE(near);
    CHECK(near.value().executed == 0);
    CHECK(near.value().remainder_units == 999'999'960);
    CHECK(near.value().interpolation_alpha() < 1.0);
    const auto crossed = schedule->poll(16'666'667ns, complete);
    REQUIRE(crossed);
    CHECK(crossed.value().executed == 1);
    CHECK(crossed.value().remainder_units == 20);
    const auto exact = schedule->poll(50ms, complete);
    REQUIRE(exact);
    CHECK(exact.value().executed == 2);
    CHECK(exact.value().remainder_units == 0);
    CHECK(schedule->next_tick() == 3);
}

TEST_CASE("offline long exact sequence has no timestep rounding drift", "[timing]") {
    auto schedule = offline();
    for (std::int64_t millisecond = 1; millisecond <= 10'000; ++millisecond) {
        const auto pass = schedule->poll(std::chrono::milliseconds{millisecond}, complete);
        REQUIRE(pass);
        REQUIRE(pass.value().discarded == 0);
    }
    CHECK(schedule->next_tick() == 600);
    CHECK(schedule->backlog().remainder_units == 0);
}

TEST_CASE("offline clamp and callback cap report discarded whole ticks", "[timing]") {
    auto schedule = offline();
    REQUIRE(schedule->poll(1ms, complete));
    const auto clamped = schedule->poll(1s, complete);
    REQUIRE(clamped);
    CHECK(clamped.value().clamped_delta);
    CHECK(clamped.value().executed == 8);
    CHECK(clamped.value().discarded == 7);
    CHECK(clamped.value().pending == 0);
    CHECK(clamped.value().remainder_units == 60'000'000);
    CHECK(schedule->next_tick() == 8);
    const auto same_time = schedule->poll(1s, complete);
    REQUIRE(same_time);
    CHECK(same_time.value().executed == 0);
    CHECK(same_time.value().discarded == 0);
    const auto next = schedule->poll(1s + 16ms, complete);
    REQUIRE(next);
    CHECK(next.value().executed == 1);
    CHECK(schedule->next_tick() == 9);
}

TEST_CASE("offline regression clamps without counting recovery time twice", "[timing]") {
    auto schedule = offline();
    REQUIRE(schedule->poll(100ms, complete));
    const auto backwards = schedule->poll(50ms, complete);
    REQUIRE(backwards);
    CHECK(backwards.value().clamped_delta);
    CHECK(backwards.value().executed == 0);
    const auto recovered = schedule->poll(100ms, complete);
    REQUIRE(recovered);
    CHECK(recovered.value().executed == 0);
    CHECK(schedule->next_tick() == 6);
    const auto minimum = schedule->poll(shark::MonotonicTime::min(), complete);
    REQUIRE(minimum);
    CHECK(minimum.value().clamped_delta);
    const auto maximum = schedule->poll(shark::MonotonicTime::max(), complete);
    REQUIRE(maximum);
    CHECK(maximum.value().executed == 8);
    CHECK(maximum.value().discarded == 7);
    CHECK(maximum.value().clamped_delta);
}

TEST_CASE("offline callback failures preserve debt without invoking drop policy", "[timing]") {
    auto schedule = offline();
    const auto result = schedule->poll(250ms, [](const std::uint64_t tick) -> shark::Result<void> {
        if (tick == 2) {
            return shark::Error{shark::ErrorCode::io_failure, "fixture failure",
                                shark::ErrorContext{"test.offline", "request-9"}};
        }
        return {};
    });
    REQUIRE_FALSE(result);
    CHECK(result.error().context.request_id == "request-9");
    CHECK(schedule->next_tick() == 2);
    CHECK(schedule->backlog().pending == 13);
    CHECK(schedule->faulted());
    const auto again = schedule->poll(1s, complete);
    REQUIRE_FALSE(again);
    CHECK(again.error().context.operation == "test.offline");
    CHECK(schedule->backlog().pending == 13);
}

TEST_CASE("offline arbitrary callback exceptions fault before any retry", "[timing]") {
    auto schedule = offline();
    const auto result = schedule->poll(
        250ms, [](std::uint64_t) -> shark::Result<void> { throw std::runtime_error{"fixture"}; });
    REQUIRE_FALSE(result);
    CHECK(result.error().context.operation == "timing.offline.callback");
    CHECK(schedule->next_tick() == 0);
    CHECK(schedule->backlog().pending == 15);
    CHECK_FALSE(schedule->poll(1s, complete));
}

TEST_CASE("offline stop preserves pending work even at the callback budget edge", "[timing]") {
    auto schedule = offline();
    std::stop_source stop;
    const auto result = schedule->poll(
        250ms,
        [&](const std::uint64_t tick) -> shark::Result<void> {
            if (tick == 7) {
                static_cast<void>(stop.request_stop());
            }
            return {};
        },
        stop.get_token());
    REQUIRE(result);
    CHECK(result.value().executed == 8);
    CHECK(result.value().cancelled);
    CHECK(result.value().discarded == 0);
    CHECK(result.value().pending == 7);
    const auto again = schedule->poll(1s, complete);
    REQUIRE(again);
    CHECK(again.value().cancelled);
    CHECK(again.value().pending == 7);
    CHECK(again.value().executed == 0);

    auto before_first = offline();
    const auto stopped = before_first->poll(1s, complete, stop.get_token());
    REQUIRE(stopped);
    CHECK(stopped.value().cancelled);
    CHECK(stopped.value().pending == 0);
    CHECK(before_first->next_tick() == 0);
}

TEST_CASE("offline reentrant and empty calls cannot consume clock deltas", "[timing]") {
    auto schedule = offline();
    CHECK_FALSE(schedule->poll(50ms, {}));
    const auto result = schedule->poll(50ms, [&](std::uint64_t) -> shark::Result<void> {
        const auto nested = schedule->poll(1s, complete);
        REQUIRE_FALSE(nested);
        CHECK(nested.error().context.operation == "timing.offline.reentrant");
        return {};
    });
    REQUIRE(result);
    CHECK(result.value().executed == 3);
    CHECK(result.value().discarded == 0);
    CHECK(schedule->next_tick() == 3);
}

TEST_CASE("offline tick exhaustion cannot overflow a committed index", "[timing]") {
    auto schedule = offline(0ns, std::numeric_limits<std::uint64_t>::max());
    std::uint64_t calls = 0;
    const auto result = schedule->poll(50ms, [&](std::uint64_t) -> shark::Result<void> {
        ++calls;
        return {};
    });
    REQUIRE_FALSE(result);
    CHECK(result.error().context.operation == "timing.offline.tick_overflow");
    CHECK(calls == 0);
    CHECK(schedule->backlog().pending == 3);
    CHECK(schedule->faulted());
}
