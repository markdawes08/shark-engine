#include <shark/server_application.hpp>
#include <shark/time.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <csignal>
#include <functional>
#include <memory>
#include <sstream>
#include <stop_token>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;

class IdleSignals final : public shark::SignalBackend {
  public:
    bool try_claim() noexcept override {
        owned = true;
        return true;
    }
    shark::SignalHandler exchange(int, shark::SignalHandler) noexcept override {
        return fail_restore ? SIG_ERR : SIG_DFL;
    }
    int requested_signal() const noexcept override {
        return request;
    }
    void release(bool restored) noexcept override {
        owned = false;
        clean = restored;
    }
    int request{};
    bool owned{};
    bool clean{};
    bool fail_restore{};
};

class TestClock final : public shark::MonotonicClock {
  public:
    shark::MonotonicTime now() noexcept override {
        ++observations;
        return time;
    }
    shark::Result<void> wait_for(shark::MonotonicTime delay) override {
        waits.push_back(delay);
        if (on_wait) {
            return on_wait(delay);
        }
        time += delay;
        return {};
    }
    shark::MonotonicTime time{};
    int observations{};
    std::vector<shark::MonotonicTime> waits;
    std::function<shark::Result<void>(shark::MonotonicTime)> on_wait;
};

struct IdleFixture {
    std::ostringstream output;
    IdleSignals* signals{};
    shark::ServerApplication application;
    TestClock clock;

    IdleFixture() : application(output, make_backend()) {}
    std::unique_ptr<IdleSignals> make_backend() {
        auto backend = std::make_unique<IdleSignals>();
        signals = backend.get();
        return backend;
    }
};
} // namespace

TEST_CASE("Idle server waits only to its injected deadline", "[timing][server-idle]") {
    IdleFixture fixture;
    REQUIRE(fixture.application.start({45ms}));
    const auto result = fixture.application.run_idle(fixture.clock);
    REQUIRE(result);
    CHECK(result.value() == shark::ServerStopReason::duration_elapsed);
    CHECK(fixture.clock.waits == std::vector<shark::MonotonicTime>{20ms, 20ms, 5ms});
    CHECK(fixture.clock.time == 45ms);
    CHECK(fixture.application.state() == shark::ServerState::running);
    CHECK(fixture.signals->owned);
    REQUIRE(fixture.application.stop(result.value()));
    CHECK_FALSE(fixture.signals->owned);
    CHECK(fixture.signals->clean);
}

TEST_CASE("Idle deadline tolerates an oversleep", "[timing][server-idle]") {
    IdleFixture fixture;
    fixture.clock.on_wait = [&](shark::MonotonicTime) -> shark::Result<void> {
        fixture.clock.time += 1h;
        return {};
    };
    REQUIRE(fixture.application.start({1ms}));
    const auto result = fixture.application.run_idle(fixture.clock);
    REQUIRE(result);
    CHECK(result.value() == shark::ServerStopReason::duration_elapsed);
    CHECK(fixture.clock.waits.size() == 1);
    CHECK(fixture.clock.waits.front() == 1ms);
    CHECK(fixture.application.stop(result.value()));
}

TEST_CASE("Idle loop rejects a negative clock epoch", "[timing][server-idle]") {
    IdleFixture fixture;
    fixture.clock.time = shark::MonotonicTime::min();
    REQUIRE(fixture.application.start({1ms}));
    const auto result = fixture.application.run_idle(fixture.clock);
    REQUIRE_FALSE(result);
    CHECK(result.error().context.operation == "server.timing.clock");
    CHECK(fixture.clock.waits.empty());
    CHECK(fixture.application.stop());
}

TEST_CASE("Idle server responds to signals without a duration", "[timing][server-idle]") {
    IdleFixture fixture;
    int signal = SIGINT;
    auto expected = shark::ServerStopReason::interrupt;
    SECTION("interrupt") {}
    SECTION("terminate") {
        signal = SIGTERM;
        expected = shark::ServerStopReason::terminate;
    }
    fixture.clock.on_wait = [&](shark::MonotonicTime delay) -> shark::Result<void> {
        fixture.clock.time += delay;
        if (fixture.clock.waits.size() == 3) {
            fixture.signals->request = signal;
        }
        return {};
    };
    REQUIRE(fixture.application.start({}));
    const auto result = fixture.application.run_idle(fixture.clock);
    REQUIRE(result);
    CHECK(result.value() == expected);
    CHECK(fixture.clock.waits.size() == 3);
    CHECK(fixture.clock.time == 60ms);
    CHECK(fixture.application.stop(result.value()));
}

TEST_CASE("Idle cancellation is bounded by one poll and preserves ownership",
          "[timing][server-idle]") {
    IdleFixture fixture;
    std::stop_source source;
    SECTION("already requested") {
        source.request_stop();
    }
    SECTION("during wait") {
        fixture.clock.on_wait = [&](shark::MonotonicTime delay) -> shark::Result<void> {
            fixture.clock.time += delay;
            source.request_stop();
            return {};
        };
    }
    REQUIRE(fixture.application.start({}));
    const auto result = fixture.application.run_idle(fixture.clock, source.get_token());
    REQUIRE(result);
    CHECK(result.value() == shark::ServerStopReason::requested);
    CHECK(fixture.clock.waits.size() <= 1);
    CHECK(fixture.clock.time <= 20ms);
    CHECK(fixture.signals->owned);
    CHECK(fixture.application.stop(result.value()));
}

TEST_CASE("Idle clock failures retain context and allow explicit cleanup",
          "[timing][server-idle]") {
    IdleFixture fixture;
    fixture.clock.on_wait = [&](shark::MonotonicTime) -> shark::Result<void> {
        fixture.signals->fail_restore = true;
        return shark::Error{shark::ErrorCode::io_failure,
                            "injected clock wait failure",
                            {.operation = "fixture.wait", .request_id = "wait-7"}};
    };
    REQUIRE(fixture.application.start({}));
    const auto result = fixture.application.run_idle(fixture.clock);
    REQUIRE_FALSE(result);
    CHECK(result.error().context.operation == "fixture.wait");
    CHECK(result.error().context.request_id == "wait-7");
    CHECK(result.error().code == shark::ErrorCode::io_failure);
    CHECK(fixture.application.state() == shark::ServerState::running);
    const auto cleanup = fixture.application.stop();
    CHECK_FALSE(cleanup);
    CHECK(cleanup.cleanup_failures[0]);
    CHECK(cleanup.cleanup_failures[1]);
    CHECK(fixture.application.state() == shark::ServerState::faulted);
    CHECK(result.error().message == "injected clock wait failure");
}

TEST_CASE("Idle wait exceptions stop at the subsystem boundary", "[timing][server-idle]") {
    IdleFixture fixture;
    fixture.clock.on_wait = [](shark::MonotonicTime) -> shark::Result<void> { throw 7; };
    REQUIRE(fixture.application.start({}));
    const auto result = fixture.application.run_idle(fixture.clock);
    REQUIRE_FALSE(result);
    CHECK(result.error().context.operation == "server.timing.wait");
    CHECK(fixture.application.stop());
}

TEST_CASE("Idle loop rejects a stalled or regressing clock", "[timing][server-idle]") {
    IdleFixture fixture;
    bool regress = false;
    SECTION("stalled") {}
    SECTION("regression") {
        regress = true;
    }
    fixture.clock.on_wait = [&](shark::MonotonicTime) -> shark::Result<void> {
        if (regress) {
            fixture.clock.time -= 1ns;
        }
        return {};
    };
    REQUIRE(fixture.application.start({}));
    const auto result = fixture.application.run_idle(fixture.clock);
    REQUIRE_FALSE(result);
    CHECK(result.error().context.operation == "server.timing.clock");
    CHECK(fixture.clock.waits.size() == 1);
    CHECK(fixture.application.stop());
}

TEST_CASE("Idle deadline rejects overflow before waiting", "[timing][server-idle]") {
    IdleFixture fixture;
    fixture.clock.time = shark::MonotonicTime::max() - 1ms;
    REQUIRE(fixture.application.start({2ms}));
    const auto result = fixture.application.run_idle(fixture.clock);
    REQUIRE_FALSE(result);
    CHECK(result.error().context.operation == "server.timing.deadline");
    CHECK(fixture.clock.waits.empty());
    CHECK(fixture.application.stop());
}

TEST_CASE("Idle transitions reject reentrancy and retain the outer lifetime",
          "[timing][server-idle]") {
    IdleFixture fixture;
    CHECK_FALSE(fixture.application.run_idle(fixture.clock));
    bool rejected = false;
    fixture.clock.on_wait = [&](shark::MonotonicTime delay) -> shark::Result<void> {
        const auto nested = fixture.application.run_idle(fixture.clock);
        const auto stop = fixture.application.stop();
        const auto start = fixture.application.start({});
        rejected = !nested && !stop && !start && fixture.signals->owned;
        fixture.clock.time += delay;
        return {};
    };
    REQUIRE(fixture.application.start({1ms}));
    const auto result = fixture.application.run_idle(fixture.clock);
    REQUIRE(result);
    CHECK(rejected);
    CHECK(fixture.application.stop(result.value()));
    CHECK_FALSE(fixture.application.run_idle(fixture.clock));
}

TEST_CASE("Idle deadlines reset across repeated application lifetimes", "[timing][server-idle]") {
    IdleFixture fixture;
    for (int cycle = 0; cycle < 100; ++cycle) {
        REQUIRE(fixture.application.start({2ms}));
        const auto result = fixture.application.run_idle(fixture.clock);
        REQUIRE(result);
        REQUIRE(result.value() == shark::ServerStopReason::duration_elapsed);
        REQUIRE(fixture.application.stop(result.value()));
    }
    CHECK(fixture.clock.time == 200ms);
    CHECK(fixture.clock.waits.size() == 100);
}
