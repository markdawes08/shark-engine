#include "graphics_queue_waiter.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace {
using shark::ErrorCode;
using shark::client::detail::GraphicsQueueWaiter;

static_assert(!std::is_copy_constructible_v<GraphicsQueueWaiter>);
static_assert(!std::is_move_constructible_v<GraphicsQueueWaiter>);

// The test gates the action itself instead of guessing when a worker starts.
// Cleanup releases the gate even if a REQUIRE aborts the test, then consumes
// and joins the owned operation before the waiter's strict destructor runs.
struct QueueFixture {
    GraphicsQueueWaiter waiter;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{false};
    bool released{false};
    std::thread::id worker_thread{};
    unsigned int calls{0};

    ~QueueFixture() {
        release();
        if (waiter.pending()) {
            static_cast<void>(waiter.wait(1'000'000'000));
        }
    }

    int action(int value) {
        std::unique_lock lock{mutex};
        ++calls;
        worker_thread = std::this_thread::get_id();
        entered = true;
        changed.notify_one();
        changed.wait(lock, [this] { return released; });
        return value;
    }

    bool await_entry() {
        std::unique_lock lock{mutex};
        return changed.wait_for(lock, std::chrono::seconds{1}, [this] { return entered; });
    }

    void release() {
        {
            const std::lock_guard lock{mutex};
            released = true;
        }
        changed.notify_one();
    }
};

#if defined(_WIN32)
struct ThreadExitFixture {
    GraphicsQueueWaiter waiter;
    std::mutex mutex;
    std::condition_variable changed;
    bool observer_ready{false};
    bool armed{false};
    bool stop_observer{false};
    bool destructor_entered{false};
    bool released{false};
    bool watchdog_released{false};
    std::thread observer;

    struct ExitBlock {
        ThreadExitFixture* fixture{nullptr};
        ~ExitBlock() {
            std::unique_lock lock{fixture->mutex};
            fixture->destructor_entered = true;
            fixture->changed.notify_all();
            fixture->changed.wait(lock, [this] { return fixture->released; });
        }
    };

    ~ThreadExitFixture() {
        release();
        stop_watchdog();
        join_watchdog();
        if (waiter.pending()) {
            static_cast<void>(waiter.wait(1'000'000'000));
        }
    }

    bool start_watchdog() {
        // Start before blocking TLS teardown: Windows serializes DLL thread
        // attach/detach, so a newly created watchdog might not start afterward.
        observer = std::thread([this] {
            std::unique_lock lock{mutex};
            observer_ready = true;
            changed.notify_all();
            changed.wait(lock, [this] { return armed || stop_observer; });
            if (!stop_observer && !changed.wait_for(lock, std::chrono::seconds{1},
                                                    [this] { return stop_observer; })) {
                // Let the broken implementation finish and fail an assertion
                // instead of leaving a blocked worker for CTest to terminate.
                watchdog_released = true;
                released = true;
                changed.notify_all();
            }
        });
        std::unique_lock lock{mutex};
        return changed.wait_for(lock, std::chrono::seconds{1}, [this] { return observer_ready; });
    }

    int action() {
        thread_local ExitBlock block;
        block.fixture = this;
        return 73;
    }

    bool await_destructor() {
        std::unique_lock lock{mutex};
        return changed.wait_for(lock, std::chrono::seconds{1},
                                [this] { return destructor_entered; });
    }

    void arm_watchdog() {
        const std::lock_guard lock{mutex};
        armed = true;
        changed.notify_all();
    }

    void stop_watchdog() {
        {
            const std::lock_guard lock{mutex};
            stop_observer = true;
        }
        changed.notify_all();
    }

    void join_watchdog() {
        // Release the TLS gate first: Windows serializes thread detach, so
        // joining the observer while that gate is blocked can itself deadlock.
        if (observer.joinable()) {
            observer.join();
        }
    }

    void release() {
        {
            const std::lock_guard lock{mutex};
            released = true;
        }
        changed.notify_all();
    }
};
#endif
} // namespace

TEST_CASE("Graphics queue waiter rejects idle waits and empty actions", "[presentation]") {
    QueueFixture fixture;
    CHECK_FALSE(fixture.waiter.pending());
    const auto empty = fixture.waiter.start({});
    REQUIRE_FALSE(empty);
    CHECK(empty.error().code == ErrorCode::invalid_argument);
    CHECK(empty.error().context.operation == "graphics.presentation.queue_wait");
    CHECK_FALSE(fixture.waiter.pending());
    const auto idle = fixture.waiter.wait(0);
    REQUIRE_FALSE(idle);
    CHECK(idle.error().code == ErrorCode::conflict);
    CHECK_FALSE(fixture.waiter.pending());
    REQUIRE(fixture.waiter.start([] { return 7; }));
    const auto done = fixture.waiter.wait(1'000'000'000);
    REQUIRE(done);
    REQUIRE(done.value());
    CHECK(*done.value() == 7);
    CHECK_FALSE(fixture.waiter.pending());
}

TEST_CASE("Graphics queue timeout retains ownership and rejects overlapping waits",
          "[presentation]") {
    QueueFixture fixture;
    const auto owner_thread = std::this_thread::get_id();
    REQUIRE(fixture.waiter.start([&] { return fixture.action(-4); }));
    REQUIRE(fixture.await_entry());
    CHECK(fixture.waiter.pending());
    const auto zero = fixture.waiter.wait(0);
    REQUIRE(zero);
    CHECK_FALSE(zero.value());
    CHECK(fixture.waiter.pending());
    const auto bounded = fixture.waiter.wait(1'000'000);
    REQUIRE(bounded);
    CHECK_FALSE(bounded.value());
    CHECK(fixture.waiter.pending());
    const auto duplicate = fixture.waiter.start([] { return 99; });
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().code == ErrorCode::conflict);
    CHECK(duplicate.error().context.operation == "graphics.presentation.queue_wait");
    fixture.release();
    const auto done = fixture.waiter.wait(1'000'000'000);
    REQUIRE(done);
    REQUIRE(done.value());
    CHECK(*done.value() == -4);
    CHECK_FALSE(fixture.waiter.pending());
    CHECK(fixture.calls == 1);
    CHECK(fixture.worker_thread != owner_thread);
    const auto consumed = fixture.waiter.wait(0);
    REQUIRE_FALSE(consumed);
    CHECK(consumed.error().code == ErrorCode::conflict);
}

TEST_CASE("Graphics queue wait clamps oversized deadlines while preserving the pending action",
          "[presentation]") {
    QueueFixture fixture;
    REQUIRE(fixture.waiter.start([&] { return fixture.action(23); }));
    REQUIRE(fixture.await_entry());
    const auto started = std::chrono::steady_clock::now();
    const auto pending = fixture.waiter.wait(std::numeric_limits<std::uint64_t>::max());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    REQUIRE(pending);
    CHECK_FALSE(pending.value());
    CHECK(fixture.waiter.pending());
    // A watchdog allowance, not a throughput or exact scheduling assertion.
    CHECK(elapsed < std::chrono::seconds{3});
    fixture.release();
    const auto done = fixture.waiter.wait(1'000'000'000);
    REQUIRE(done);
    REQUIRE(done.value());
    CHECK(*done.value() == 23);
    CHECK_FALSE(fixture.waiter.pending());
}

TEST_CASE("Graphics queue waiter joins every generation before accepting the next",
          "[presentation]") {
    QueueFixture fixture;
    for (int generation = 0; generation < 32; ++generation) {
        REQUIRE(fixture.waiter.start([generation] { return generation - 16; }));
        CHECK(fixture.waiter.pending());
        const auto done = fixture.waiter.wait(1'000'000'000);
        REQUIRE(done);
        REQUIRE(done.value());
        CHECK(*done.value() == generation - 16);
        CHECK_FALSE(fixture.waiter.pending());
    }
}

TEST_CASE("Graphics queue waiter contains action exceptions and remains reusable",
          "[presentation]") {
    QueueFixture fixture;
    std::function<int()> action;
    SECTION("standard exception") {
        action = []() -> int { throw std::runtime_error("Injected queue wait exception"); };
    }
    SECTION("nonstandard exception") {
        action = []() -> int { throw 7; };
    }
    REQUIRE(fixture.waiter.start(std::move(action)));
    const auto failed = fixture.waiter.wait(1'000'000'000);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == ErrorCode::execution_failed);
    CHECK(failed.error().context.operation == "graphics.presentation.queue_wait");
    CHECK_FALSE(fixture.waiter.pending());
    REQUIRE(fixture.waiter.start([] { return 42; }));
    const auto recovered = fixture.waiter.wait(1'000'000'000);
    REQUIRE(recovered);
    REQUIRE(recovered.value());
    CHECK(*recovered.value() == 42);
    CHECK_FALSE(fixture.waiter.pending());
}

#if defined(_WIN32)
TEST_CASE("Graphics queue deadline includes thread-local destruction", "[presentation]") {
    ThreadExitFixture fixture;
    std::uint64_t timeout_ns = 0;
    SECTION("zero timeout remains a poll") {}
    SECTION("sub-millisecond timeout remains a poll") {
        timeout_ns = 999'999;
    }
    SECTION("finite timeout retains the exiting worker") {
        timeout_ns = 1'000'000;
    }
    REQUIRE(fixture.start_watchdog());
    REQUIRE(fixture.waiter.start([&] { return fixture.action(); }));
    REQUIRE(fixture.await_destructor());
    // Entering TLS destruction proves the action returned and the waiter
    // published its result; the OS thread is still alive behind this gate.
    fixture.arm_watchdog();
    const auto pending = fixture.waiter.wait(timeout_ns);
    fixture.stop_watchdog();
    REQUIRE(pending);
    CHECK_FALSE(pending.value());
    CHECK_FALSE(fixture.watchdog_released);
    CHECK(fixture.waiter.pending());
    const auto duplicate = fixture.waiter.start([] { return 99; });
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().code == ErrorCode::conflict);
    fixture.release();
    fixture.join_watchdog();
    const auto done = fixture.waiter.wait(1'000'000'000);
    REQUIRE(done);
    REQUIRE(done.value());
    CHECK(*done.value() == 73);
    CHECK_FALSE(fixture.waiter.pending());
    const auto consumed = fixture.waiter.wait(0);
    REQUIRE_FALSE(consumed);
    CHECK(consumed.error().code == ErrorCode::conflict);
    REQUIRE(fixture.waiter.start([] { return 42; }));
    const auto reused = fixture.waiter.wait(1'000'000'000);
    REQUIRE(reused);
    REQUIRE(reused.value());
    CHECK(*reused.value() == 42);
    CHECK_FALSE(fixture.waiter.pending());
}
#endif
