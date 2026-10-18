#include <shark/shutdown_signals.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <csignal>
#include <initializer_list>
#include <memory>
#include <type_traits>
#include <utility>

namespace {

enum class Event {
    claim,
    install_interrupt,
    install_terminate,
    restore_terminate,
    restore_interrupt,
    release_clean,
    release_failed,
    destroy
};

void previous_interrupt(int) {}
void previous_terminate(int) {}

struct FakeState {
    std::array<Event, 32> events{};
    std::size_t count{0};
    bool overflow{false};
    bool claim_available{true};
    bool fail_install_interrupt{false};
    bool fail_install_terminate{false};
    bool fail_restore_interrupt{false};
    bool fail_restore_terminate{false};
    int requested{0};
    shark::SignalHandler interrupt{previous_interrupt};
    shark::SignalHandler terminate{previous_terminate};

    void record(Event event) noexcept {
        if (count == events.size()) {
            overflow = true;
            return;
        }
        events[count++] = event;
    }
};

class FakeBackend final : public shark::SignalBackend {
  public:
    explicit FakeBackend(FakeState& state) noexcept : state_(state) {}
    ~FakeBackend() override {
        state_.record(Event::destroy);
    }

    bool try_claim() noexcept override {
        state_.record(Event::claim);
        if (!state_.claim_available) {
            return false;
        }
        state_.requested = 0;
        return true;
    }

    shark::SignalHandler exchange(int signal, shark::SignalHandler handler) noexcept override {
        if (signal == SIGINT) {
            const bool restoring = handler == previous_interrupt;
            state_.record(restoring ? Event::restore_interrupt : Event::install_interrupt);
            if (restoring ? state_.fail_restore_interrupt : state_.fail_install_interrupt) {
                return SIG_ERR;
            }
            return std::exchange(state_.interrupt, handler);
        }
        const bool restoring = handler == previous_terminate;
        state_.record(restoring ? Event::restore_terminate : Event::install_terminate);
        if (restoring ? state_.fail_restore_terminate : state_.fail_install_terminate) {
            return SIG_ERR;
        }
        return std::exchange(state_.terminate, handler);
    }

    int requested_signal() const noexcept override {
        return state_.requested;
    }

    void release(bool restored) noexcept override {
        state_.record(restored ? Event::release_clean : Event::release_failed);
    }

  private:
    FakeState& state_;
};

void check_events(const FakeState& state, std::initializer_list<Event> expected) {
    REQUIRE_FALSE(state.overflow);
    REQUIRE(state.count == expected.size());
    std::size_t index = 0;
    for (const auto event : expected) {
        CHECK(state.events[index++] == event);
    }
}

volatile std::sig_atomic_t sentinel_signal = 0;

extern "C" void signal_sentinel(int signal) {
    sentinel_signal = signal;
}

// The tests never raise a default signal: both sentinels are installed before any
// native owner is started, and the test process's original handlers are restored.
class NativeSentinels final {
  public:
    NativeSentinels() noexcept
        : interrupt_(std::signal(SIGINT, signal_sentinel)),
          terminate_(std::signal(SIGTERM, signal_sentinel)) {}

    ~NativeSentinels() noexcept {
        if (terminate_ != SIG_ERR) {
            static_cast<void>(std::signal(SIGTERM, terminate_));
        }
        if (interrupt_ != SIG_ERR) {
            static_cast<void>(std::signal(SIGINT, interrupt_));
        }
    }

    NativeSentinels(const NativeSentinels&) = delete;
    NativeSentinels& operator=(const NativeSentinels&) = delete;

    [[nodiscard]] bool installed() const noexcept {
        return interrupt_ != SIG_ERR && terminate_ != SIG_ERR;
    }

  private:
    shark::SignalHandler interrupt_;
    shark::SignalHandler terminate_;
};

static_assert(!std::is_copy_constructible_v<shark::ShutdownSignals>);
static_assert(!std::is_copy_assignable_v<shark::ShutdownSignals>);
static_assert(!std::is_move_constructible_v<shark::ShutdownSignals>);
static_assert(!std::is_move_assignable_v<shark::ShutdownSignals>);
static_assert(std::is_nothrow_destructible_v<shark::ShutdownSignals>);
static_assert(
    std::is_nothrow_constructible_v<shark::ShutdownSignals, std::unique_ptr<shark::SignalBackend>>);
static_assert(noexcept(std::declval<shark::ShutdownSignals&>().start()));
static_assert(noexcept(std::declval<shark::ShutdownSignals&>().stop()));
static_assert(noexcept(std::declval<const shark::ShutdownSignals&>().requested_signal()));

} // namespace

TEST_CASE("Signal resource rejects a missing backend without acquiring anything",
          "[lifecycle][signals]") {
    shark::ShutdownSignals signals{nullptr};
    const auto started = signals.start();
    CHECK(started.failure == shark::SignalStartFailure::missing_backend);
    CHECK_FALSE(started.ok());
    CHECK(started.cleanup.ok());
    CHECK_FALSE(signals.active());
    CHECK(signals.requested_signal() == 0);
    CHECK(signals.stop().ok());
}

TEST_CASE("Signal resource acquires once and restores previous handlers in reverse order",
          "[lifecycle][signals]") {
    FakeState state;
    shark::ShutdownSignals signals{std::make_unique<FakeBackend>(state)};
    REQUIRE(signals.start().ok());
    CHECK(signals.active());
    CHECK(state.interrupt != previous_interrupt);
    CHECK(state.terminate != previous_terminate);
    state.requested = SIGTERM;
    CHECK(signals.requested_signal() == SIGTERM);
    CHECK(signals.start().failure == shark::SignalStartFailure::invalid_state);
    check_events(state, {Event::claim, Event::install_interrupt, Event::install_terminate});
    REQUIRE(signals.stop().ok());
    CHECK_FALSE(signals.active());
    CHECK(signals.requested_signal() == 0);
    CHECK(state.interrupt == previous_interrupt);
    CHECK(state.terminate == previous_terminate);
    CHECK(signals.stop().ok());
    check_events(state, {Event::claim, Event::install_interrupt, Event::install_terminate,
                         Event::restore_terminate, Event::restore_interrupt, Event::release_clean});
}

TEST_CASE("A failed signal lease does not touch registrations or pending shutdown",
          "[lifecycle][signals]") {
    FakeState state;
    state.claim_available = false;
    state.requested = SIGINT;
    shark::ShutdownSignals signals{std::make_unique<FakeBackend>(state)};
    const auto result = signals.start();
    CHECK(result.failure == shark::SignalStartFailure::already_owned);
    CHECK(result.cleanup.ok());
    CHECK_FALSE(signals.active());
    CHECK(state.requested == SIGINT);
    CHECK(signals.stop().ok());
    check_events(state, {Event::claim});

    state.claim_available = true;
    REQUIRE(signals.start().ok());
    CHECK(signals.requested_signal() == 0);
    REQUIRE(signals.stop().ok());
}

TEST_CASE("Failed interrupt installation releases the lease without restoring unacquired stages",
          "[lifecycle][signals]") {
    FakeState state;
    state.fail_install_interrupt = true;
    shark::ShutdownSignals signals{std::make_unique<FakeBackend>(state)};
    const auto result = signals.start();
    CHECK(result.failure == shark::SignalStartFailure::install_interrupt);
    CHECK(result.cleanup.ok());
    CHECK_FALSE(signals.active());
    CHECK(signals.stop().ok());
    check_events(state, {Event::claim, Event::install_interrupt, Event::release_clean});
    state.fail_install_interrupt = false;
    REQUIRE(signals.start().ok());
    REQUIRE(signals.stop().ok());
}

TEST_CASE("Failed terminate installation rolls back only the acquired interrupt handler",
          "[lifecycle][signals]") {
    FakeState state;
    state.fail_install_terminate = true;
    shark::ShutdownSignals signals{std::make_unique<FakeBackend>(state)};
    const auto result = signals.start();
    CHECK(result.failure == shark::SignalStartFailure::install_terminate);
    CHECK(result.cleanup.ok());
    CHECK_FALSE(signals.active());
    CHECK(state.interrupt == previous_interrupt);
    CHECK(state.terminate == previous_terminate);
    CHECK(signals.stop().ok());
    check_events(state, {Event::claim, Event::install_interrupt, Event::install_terminate,
                         Event::restore_interrupt, Event::release_clean});
    state.fail_install_terminate = false;
    REQUIRE(signals.start().ok());
    REQUIRE(signals.stop().ok());
}

TEST_CASE("Failed rollback reports cleanup failure and prevents a later lifecycle",
          "[lifecycle][signals]") {
    FakeState state;
    state.fail_install_terminate = true;
    state.fail_restore_interrupt = true;
    {
        shark::ShutdownSignals signals{std::make_unique<FakeBackend>(state)};
        const auto result = signals.start();
        CHECK(result.failure == shark::SignalStartFailure::install_terminate);
        CHECK_FALSE(result.ok());
        CHECK_FALSE(result.cleanup.ok());
        CHECK(result.cleanup.interrupt_failed);
        CHECK_FALSE(result.cleanup.terminate_failed);
        CHECK_FALSE(signals.active());
        CHECK(signals.start().failure == shark::SignalStartFailure::invalid_state);
        CHECK(signals.stop().ok());
    }
    check_events(state, {Event::claim, Event::install_interrupt, Event::install_terminate,
                         Event::restore_interrupt, Event::release_failed, Event::destroy});
}

TEST_CASE("Signal cleanup attempts both handlers exactly once even when restoration fails",
          "[lifecycle][signals]") {
    for (const auto& [fail_terminate, fail_interrupt] :
         {std::pair{true, false}, std::pair{false, true}, std::pair{true, true}}) {
        INFO("terminate failure: " << fail_terminate << ", interrupt failure: " << fail_interrupt);
        FakeState state;
        state.fail_restore_terminate = fail_terminate;
        state.fail_restore_interrupt = fail_interrupt;
        {
            shark::ShutdownSignals signals{std::make_unique<FakeBackend>(state)};
            REQUIRE(signals.start().ok());
            const auto stopped = signals.stop();
            CHECK_FALSE(stopped.ok());
            CHECK(stopped.terminate_failed == fail_terminate);
            CHECK(stopped.interrupt_failed == fail_interrupt);
            CHECK_FALSE(signals.active());
            CHECK(signals.start().failure == shark::SignalStartFailure::invalid_state);
            CHECK(signals.stop().ok());
        }
        check_events(state, {Event::claim, Event::install_interrupt, Event::install_terminate,
                             Event::restore_terminate, Event::restore_interrupt,
                             Event::release_failed, Event::destroy});
    }
}

TEST_CASE("Clean signal cycles reset pending requests and retain unique backend ownership",
          "[lifecycle][signals]") {
    FakeState state;
    {
        shark::ShutdownSignals signals{std::make_unique<FakeBackend>(state)};
        for (int cycle = 0; cycle < 32; ++cycle) {
            state.count = 0;
            REQUIRE(signals.start().ok());
            CHECK(signals.requested_signal() == 0);
            state.requested = SIGINT;
            CHECK(signals.requested_signal() == SIGINT);
            REQUIRE(signals.stop().ok());
            check_events(state, {Event::claim, Event::install_interrupt, Event::install_terminate,
                                 Event::restore_terminate, Event::restore_interrupt,
                                 Event::release_clean});
        }
        state.count = 0;
    }
    check_events(state, {Event::destroy});
}

TEST_CASE("Signal destructor restores resources before destroying its owned backend",
          "[lifecycle][signals]") {
    for (const bool fail_restore : {false, true}) {
        FakeState state;
        state.fail_restore_terminate = fail_restore;
        state.fail_restore_interrupt = fail_restore;
        {
            shark::ShutdownSignals signals{std::make_unique<FakeBackend>(state)};
            REQUIRE(signals.start().ok());
        }
        check_events(state,
                     {Event::claim, Event::install_interrupt, Event::install_terminate,
                      Event::restore_terminate, Event::restore_interrupt,
                      fail_restore ? Event::release_failed : Event::release_clean, Event::destroy});
    }
}

TEST_CASE("Native signal ownership is exclusive without resetting the active owner's request",
          "[lifecycle][signals]") {
    NativeSentinels sentinels;
    REQUIRE(sentinels.installed());
    shark::ShutdownSignals first;
    shark::ShutdownSignals second;
    REQUIRE(first.start().ok());
    REQUIRE(std::raise(SIGINT) == 0);
    CHECK(first.requested_signal() == SIGINT);
    CHECK(second.start().failure == shark::SignalStartFailure::already_owned);
    CHECK(first.requested_signal() == SIGINT);
    CHECK_FALSE(second.active());
    REQUIRE(first.stop().ok());
    REQUIRE(second.start().ok());
    CHECK(second.requested_signal() == 0);
    REQUIRE(std::raise(SIGTERM) == 0);
    CHECK(second.requested_signal() == SIGTERM);
    REQUIRE(second.stop().ok());
}

TEST_CASE("Native signal cycles restore real previous handlers and reset stale requests",
          "[lifecycle][signals]") {
    NativeSentinels sentinels;
    REQUIRE(sentinels.installed());
    shark::ShutdownSignals signals;
    for (const int signal : {SIGINT, SIGTERM}) {
        REQUIRE(signals.start().ok());
        CHECK(signals.requested_signal() == 0);
        REQUIRE(std::raise(signal) == 0);
        CHECK(signals.requested_signal() == signal);
        REQUIRE(signals.stop().ok());
        CHECK(signals.requested_signal() == 0);
    }
    // Each is raised only once after restoration: some C runtimes reset handlers
    // to SIG_DFL when delivered. The outer guard still restores pre-test handlers.
    sentinel_signal = 0;
    REQUIRE(std::raise(SIGINT) == 0);
    CHECK(sentinel_signal == SIGINT);
    sentinel_signal = 0;
    REQUIRE(std::raise(SIGTERM) == 0);
    CHECK(sentinel_signal == SIGTERM);
}
