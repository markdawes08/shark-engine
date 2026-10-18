#include <shark/server_application.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <csignal>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

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

class NativeHandlersFixture {
  public:
    NativeHandlersFixture()
        : interrupt_(std::signal(SIGINT, previous_interrupt)),
          terminate_(std::signal(SIGTERM, previous_terminate)) {}
    ~NativeHandlersFixture() {
        if (terminate_ != SIG_ERR) {
            static_cast<void>(std::signal(SIGTERM, terminate_));
        }
        if (interrupt_ != SIG_ERR) {
            static_cast<void>(std::signal(SIGINT, interrupt_));
        }
    }
    NativeHandlersFixture(const NativeHandlersFixture&) = delete;
    NativeHandlersFixture& operator=(const NativeHandlersFixture&) = delete;
    [[nodiscard]] bool installed() const noexcept {
        return interrupt_ != SIG_ERR && terminate_ != SIG_ERR;
    }

  private:
    shark::SignalHandler interrupt_;
    shark::SignalHandler terminate_;
};

struct BackendState {
    std::array<Event, 1024> events{};
    std::size_t event_count{0};
    bool claim_denied{false};
    bool fail_install_interrupt{false};
    bool fail_install_terminate{false};
    bool fail_restore_interrupt{false};
    bool fail_restore_terminate{false};
    int signal{0};
    bool owned{false};

    void record(const Event event) noexcept {
        if (event_count < events.size()) {
            events[event_count++] = event;
        }
    }
    [[nodiscard]] std::vector<Event> recorded() const {
        return {events.begin(), events.begin() + static_cast<std::ptrdiff_t>(event_count)};
    }
};

class TestBackend final : public shark::SignalBackend {
  public:
    explicit TestBackend(BackendState& state) noexcept : state_(state) {}
    ~TestBackend() override {
        state_.record(Event::destroy);
    }
    bool try_claim() noexcept override {
        state_.record(Event::claim);
        if (state_.claim_denied || state_.owned) {
            return false;
        }
        state_.owned = true;
        state_.signal = 0;
        return true;
    }
    shark::SignalHandler exchange(const int signal,
                                  shark::SignalHandler handler) noexcept override {
        if (signal == SIGINT) {
            if (handler == previous_interrupt) {
                state_.record(Event::restore_interrupt);
                return state_.fail_restore_interrupt ? SIG_ERR : handler;
            }
            state_.record(Event::install_interrupt);
            return state_.fail_install_interrupt ? SIG_ERR : previous_interrupt;
        }
        if (handler == previous_terminate) {
            state_.record(Event::restore_terminate);
            return state_.fail_restore_terminate ? SIG_ERR : handler;
        }
        state_.record(Event::install_terminate);
        return state_.fail_install_terminate ? SIG_ERR : previous_terminate;
    }
    int requested_signal() const noexcept override {
        return state_.signal;
    }
    void release(const bool restored) noexcept override {
        state_.record(restored ? Event::release_clean : Event::release_failed);
        state_.owned = !restored;
    }

  private:
    BackendState& state_;
};

class ControlledBuffer final : public std::stringbuf {
  public:
    bool fail_write{false};
    bool fail_flush{false};
    bool throw_write{false};
    bool throw_flush{false};
    std::function<void()> on_write;

  protected:
    std::streamsize xsputn(const char* text, const std::streamsize count) override {
        if (on_write) {
            auto callback = std::exchange(on_write, {});
            callback();
        }
        if (throw_write) {
            throw 7;
        }
        return fail_write ? 0 : std::stringbuf::xsputn(text, count);
    }
    int sync() override {
        if (throw_flush) {
            throw 7;
        }
        return fail_flush ? -1 : std::stringbuf::sync();
    }
};

constexpr auto ready = "Shark Server ready (headless idle)\n";
constexpr auto completed = "Shark Server stopped: run duration elapsed\n";

void require_cleanup(const BackendState& state) {
    REQUIRE(state.recorded() == std::vector<Event>{Event::claim, Event::install_interrupt,
                                                   Event::install_terminate,
                                                   Event::restore_terminate,
                                                   Event::restore_interrupt, Event::release_clean});
    REQUIRE_FALSE(state.owned);
}

} // namespace

TEST_CASE("Server application exclusively owns its resources", "[lifecycle][server-application]") {
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<shark::ServerApplication>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<shark::ServerApplication>);
    STATIC_REQUIRE_FALSE(std::is_move_constructible_v<shark::ServerApplication>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<shark::ServerApplication>);
    STATIC_REQUIRE(std::is_nothrow_destructible_v<shark::ServerApplication>);
    BackendState backend;
    std::ostringstream output;
    {
        shark::ServerApplication app{output, std::make_unique<TestBackend>(backend)};
        REQUIRE(app.state() == shark::ServerState::stopped);
        REQUIRE(app.requested_signal() == 0);
        REQUIRE(app.stop());
        REQUIRE(backend.recorded().empty());
    }
    REQUIRE(backend.recorded() == std::vector<Event>{Event::destroy});
    REQUIRE(output.str().empty());
}

TEST_CASE("Server application validates before acquiring signals",
          "[lifecycle][server-application]") {
    BackendState backend;
    std::ostringstream output;
    shark::ServerApplication app{output, std::make_unique<TestBackend>(backend)};
    for (const auto duration : {-1, 0, 600001}) {
        const auto report = app.start({std::chrono::milliseconds{duration}});
        REQUIRE_FALSE(report);
        REQUIRE(report.failure);
        REQUIRE(report.failure->code == shark::ErrorCode::invalid_configuration);
        REQUIRE(report.failure->context.operation == "server.configuration.validate");
        REQUIRE_FALSE(report.cleanup_failures[0]);
        REQUIRE_FALSE(report.cleanup_failures[1]);
        REQUIRE(app.state() == shark::ServerState::stopped);
    }
    REQUIRE(backend.recorded().empty());
    REQUIRE(output.str().empty());
    REQUIRE(app.start({std::chrono::milliseconds{1}}));
    REQUIRE(app.stop());
}

TEST_CASE("Server application rejects an absent signal capability",
          "[lifecycle][server-application]") {
    std::ostringstream output;
    shark::ServerApplication app{output, nullptr};
    const auto report = app.start({});
    REQUIRE_FALSE(report);
    REQUIRE(report.failure);
    REQUIRE(report.failure->code == shark::ErrorCode::capability_unavailable);
    REQUIRE(report.failure->context.operation == "server.startup.signals.backend");
    REQUIRE(app.state() == shark::ServerState::stopped);
    REQUIRE(app.stop());
    REQUIRE(output.str().empty());
}

TEST_CASE("Server application reports each acquisition failure and permits a clean retry",
          "[lifecycle][server-application]") {
    BackendState backend;
    std::ostringstream output;
    shark::ServerApplication app{output, std::make_unique<TestBackend>(backend)};
    std::string expected_operation;
    std::vector<Event> expected_events;
    shark::ErrorCode expected_code{shark::ErrorCode::execution_failed};
    SECTION("an existing process owner denies the lease") {
        backend.claim_denied = true;
        expected_operation = "server.startup.signals.claim";
        expected_code = shark::ErrorCode::conflict;
        expected_events = {Event::claim};
    }
    SECTION("SIGINT registration fails before any handler is acquired") {
        backend.fail_install_interrupt = true;
        expected_operation = "server.startup.signals.interrupt";
        expected_events = {Event::claim, Event::install_interrupt, Event::release_clean};
    }
    SECTION("SIGTERM registration fails after SIGINT acquisition") {
        backend.fail_install_terminate = true;
        expected_operation = "server.startup.signals.terminate";
        expected_events = {Event::claim, Event::install_interrupt, Event::install_terminate,
                           Event::restore_interrupt, Event::release_clean};
    }
    const auto report = app.start({});
    REQUIRE_FALSE(report);
    REQUIRE(report.failure);
    REQUIRE(report.failure->code == expected_code);
    REQUIRE(report.failure->context.operation == expected_operation);
    REQUIRE_FALSE(report.cleanup_failures[0]);
    REQUIRE_FALSE(report.cleanup_failures[1]);
    REQUIRE(backend.recorded() == expected_events);
    REQUIRE(app.state() == shark::ServerState::stopped);
    REQUIRE(output.str().empty());
    backend.claim_denied = false;
    backend.fail_install_interrupt = false;
    backend.fail_install_terminate = false;
    REQUIRE(app.start({}));
    REQUIRE(app.stop());
}

TEST_CASE("Server application unwinds failed readiness before returning owned diagnostics",
          "[lifecycle][server-application]") {
    BackendState backend;
    ControlledBuffer buffer;
    std::ostream output{&buffer};
    SECTION("write fails") {
        buffer.fail_write = true;
    }
    SECTION("flush fails") {
        buffer.fail_flush = true;
    }
    SECTION("write throws a nonstandard exception") {
        buffer.throw_write = true;
        output.exceptions(std::ios::badbit | std::ios::failbit);
    }
    SECTION("flush throws a nonstandard exception") {
        buffer.throw_flush = true;
        output.exceptions(std::ios::badbit | std::ios::failbit);
    }
    shark::LifecycleReport report;
    {
        shark::ServerApplication app{output, std::make_unique<TestBackend>(backend)};
        report = app.start({});
        REQUIRE_FALSE(report);
        REQUIRE(app.state() == shark::ServerState::stopped);
        REQUIRE(app.requested_signal() == 0);
        require_cleanup(backend);
        REQUIRE(app.stop());
    }
    REQUIRE(report.failure);
    REQUIRE(report.failure->code == shark::ErrorCode::io_failure);
    REQUIRE(report.failure->context.operation == "server.startup.readiness");
    REQUIRE_FALSE(report.cleanup_failures[0]);
    REQUIRE_FALSE(report.cleanup_failures[1]);
    REQUIRE(backend.events[backend.event_count - 1] == Event::destroy);
    REQUIRE(backend.event_count == 7);
}

TEST_CASE("Server startup retains primary and both reverse cleanup failures",
          "[lifecycle][server-application]") {
    BackendState backend;
    backend.fail_restore_terminate = true;
    backend.fail_restore_interrupt = true;
    ControlledBuffer buffer;
    buffer.fail_write = true;
    std::ostream output{&buffer};
    {
        shark::ServerApplication app{output, std::make_unique<TestBackend>(backend)};
        const auto report = app.start({});
        REQUIRE_FALSE(report);
        REQUIRE(report.failure);
        REQUIRE(report.failure->context.operation == "server.startup.readiness");
        REQUIRE(report.cleanup_failures[0]);
        REQUIRE(report.cleanup_failures[1]);
        REQUIRE(report.cleanup_failures[0]->context.operation ==
                "server.shutdown.signals.terminate");
        REQUIRE(report.cleanup_failures[1]->context.operation ==
                "server.shutdown.signals.interrupt");
        REQUIRE(app.state() == shark::ServerState::faulted);
        REQUIRE_FALSE(app.start({}));
        REQUIRE_FALSE(app.stop());
        REQUIRE(backend.recorded() ==
                std::vector<Event>{Event::claim, Event::install_interrupt, Event::install_terminate,
                                   Event::restore_terminate, Event::restore_interrupt,
                                   Event::release_failed});
    }
    REQUIRE(backend.event_count == 7);
    REQUIRE(backend.events[6] == Event::destroy);
    REQUIRE(buffer.str().empty());
}

TEST_CASE("Server partial acquisition retains failed SIGINT rollback",
          "[lifecycle][server-application]") {
    BackendState backend;
    backend.fail_install_terminate = true;
    backend.fail_restore_interrupt = true;
    std::ostringstream output;
    shark::ServerApplication app{output, std::make_unique<TestBackend>(backend)};
    const auto report = app.start({});
    REQUIRE_FALSE(report);
    REQUIRE(report.failure);
    REQUIRE(report.failure->context.operation == "server.startup.signals.terminate");
    REQUIRE_FALSE(report.cleanup_failures[0]);
    REQUIRE(report.cleanup_failures[1]);
    REQUIRE(report.cleanup_failures[1]->context.operation == "server.shutdown.signals.interrupt");
    REQUIRE(app.state() == shark::ServerState::faulted);
    REQUIRE(output.str().empty());
    REQUIRE(backend.recorded() ==
            std::vector<Event>{Event::claim, Event::install_interrupt, Event::install_terminate,
                               Event::restore_interrupt, Event::release_failed});
}

TEST_CASE("Server application restarts with fresh configuration and signal state",
          "[lifecycle][server-application]") {
    BackendState backend;
    std::ostringstream output;
    shark::ServerApplication app{output, std::make_unique<TestBackend>(backend)};
    for (int cycle = 1; cycle <= 100; ++cycle) {
        CAPTURE(cycle);
        REQUIRE(app.start({std::chrono::milliseconds{cycle}}));
        REQUIRE(app.state() == shark::ServerState::running);
        REQUIRE(app.config().run_for == std::chrono::milliseconds{cycle});
        REQUIRE(app.requested_signal() == 0);
        backend.signal = SIGTERM;
        REQUIRE(app.requested_signal() == SIGTERM);
        const auto count = backend.event_count;
        const auto duplicate_start = app.start({std::chrono::milliseconds{600000}});
        REQUIRE_FALSE(duplicate_start);
        REQUIRE(duplicate_start.failure);
        REQUIRE(duplicate_start.failure->code == shark::ErrorCode::conflict);
        REQUIRE(app.config().run_for == std::chrono::milliseconds{cycle});
        REQUIRE(backend.event_count == count);
        REQUIRE(app.stop(shark::ServerStopReason::duration_elapsed));
        REQUIRE(app.state() == shark::ServerState::stopped);
        REQUIRE(app.requested_signal() == 0);
        REQUIRE_FALSE(backend.owned);
        const auto completed_output = output.str();
        REQUIRE(app.stop());
        REQUIRE(output.str() == completed_output);
        REQUIRE(backend.event_count == count + 3);
    }
    REQUIRE(backend.event_count == 600);
    std::string expected;
    for (int cycle = 0; cycle < 100; ++cycle) {
        expected += ready;
        expected += completed;
    }
    REQUIRE(output.str() == expected);
}

TEST_CASE("Server stop releases handlers before reporting completion",
          "[lifecycle][server-application]") {
    BackendState backend;
    ControlledBuffer buffer;
    std::ostream output{&buffer};
    shark::ServerApplication app{output, std::make_unique<TestBackend>(backend)};
    REQUIRE(app.start({}));
    bool observed_completion = false;
    buffer.on_write = [&] {
        observed_completion = true;
        REQUIRE(app.state() == shark::ServerState::stopping);
        require_cleanup(backend);
    };
    REQUIRE(app.stop(shark::ServerStopReason::interrupt));
    REQUIRE(observed_completion);
    REQUIRE(buffer.str() == std::string{ready} + "Shark Server stopped: SIGINT\n");
}

TEST_CASE("Server startup preserves a shutdown request received during readiness",
          "[lifecycle][server-application]") {
    BackendState backend;
    ControlledBuffer buffer;
    std::ostream output{&buffer};
    shark::ServerApplication app{output, std::make_unique<TestBackend>(backend)};
    buffer.on_write = [&] {
        REQUIRE(app.state() == shark::ServerState::starting);
        REQUIRE(backend.owned);
        backend.signal = SIGINT;
    };
    REQUIRE(app.start({}));
    REQUIRE(app.requested_signal() == SIGINT);
    REQUIRE(app.stop(shark::ServerStopReason::interrupt));
    REQUIRE(buffer.str() == std::string{ready} + "Shark Server stopped: SIGINT\n");
}

TEST_CASE("Server stop surfaces either or both restoration failures without success output",
          "[lifecycle][server-application]") {
    BackendState backend;
    SECTION("SIGTERM fails") {
        backend.fail_restore_terminate = true;
    }
    SECTION("SIGINT fails") {
        backend.fail_restore_interrupt = true;
    }
    SECTION("both fail") {
        backend.fail_restore_terminate = true;
        backend.fail_restore_interrupt = true;
    }
    std::ostringstream output;
    shark::ServerApplication app{output, std::make_unique<TestBackend>(backend)};
    REQUIRE(app.start({}));
    const auto report = app.stop();
    REQUIRE_FALSE(report);
    REQUIRE_FALSE(report.failure);
    REQUIRE(report.cleanup_failures[0].has_value() == backend.fail_restore_terminate);
    REQUIRE(report.cleanup_failures[1].has_value() == backend.fail_restore_interrupt);
    REQUIRE(app.state() == shark::ServerState::faulted);
    REQUIRE(backend.recorded() ==
            std::vector<Event>{Event::claim, Event::install_interrupt, Event::install_terminate,
                               Event::restore_terminate, Event::restore_interrupt,
                               Event::release_failed});
    REQUIRE(output.str() == ready);
    const auto count = backend.event_count;
    REQUIRE_FALSE(app.stop());
    REQUIRE_FALSE(app.start({}));
    REQUIRE(backend.event_count == count);
}

TEST_CASE("Server completion output failure leaves released resources restartable",
          "[lifecycle][server-application]") {
    BackendState backend;
    ControlledBuffer buffer;
    std::ostream output{&buffer};
    shark::ServerApplication app{output, std::make_unique<TestBackend>(backend)};
    REQUIRE(app.start({}));
    SECTION("write fails") {
        buffer.fail_write = true;
    }
    SECTION("flush fails") {
        buffer.fail_flush = true;
    }
    SECTION("write throws a nonstandard exception") {
        buffer.throw_write = true;
        output.exceptions(std::ios::badbit | std::ios::failbit);
    }
    SECTION("flush throws a nonstandard exception") {
        buffer.throw_flush = true;
        output.exceptions(std::ios::badbit | std::ios::failbit);
    }
    const auto report = app.stop(shark::ServerStopReason::terminate);
    REQUIRE_FALSE(report);
    REQUIRE(report.failure);
    REQUIRE(report.failure->code == shark::ErrorCode::io_failure);
    REQUIRE(report.failure->context.operation == "server.shutdown.completion");
    REQUIRE_FALSE(report.cleanup_failures[0]);
    REQUIRE_FALSE(report.cleanup_failures[1]);
    REQUIRE(app.state() == shark::ServerState::stopped);
    require_cleanup(backend);
    buffer.fail_write = false;
    buffer.fail_flush = false;
    buffer.throw_write = false;
    buffer.throw_flush = false;
    output.clear();
    REQUIRE(app.start({}));
    REQUIRE(app.stop());
}

TEST_CASE("Server lifecycle rejects reentrant transitions without disturbing outer ownership",
          "[lifecycle][server-application]") {
    BackendState backend;
    ControlledBuffer buffer;
    std::ostream output{&buffer};
    shark::ServerApplication app{output, std::make_unique<TestBackend>(backend)};
    shark::ServerState expected_state{shark::ServerState::starting};
    int callbacks = 0;
    const auto callback = [&] {
        ++callbacks;
        REQUIRE(app.state() == expected_state);
        const auto count = backend.event_count;
        const auto nested_start = app.start({});
        const auto nested_stop = app.stop();
        REQUIRE_FALSE(nested_start);
        REQUIRE_FALSE(nested_stop);
        REQUIRE(nested_start.failure);
        REQUIRE(nested_stop.failure);
        REQUIRE(nested_start.failure->code == shark::ErrorCode::conflict);
        REQUIRE(nested_stop.failure->code == shark::ErrorCode::conflict);
        REQUIRE(app.state() == expected_state);
        REQUIRE(backend.event_count == count);
    };
    buffer.on_write = callback;
    REQUIRE(app.start({}));
    REQUIRE(app.state() == shark::ServerState::running);
    expected_state = shark::ServerState::stopping;
    buffer.on_write = callback;
    REQUIRE(app.stop());
    REQUIRE(app.state() == shark::ServerState::stopped);
    REQUIRE(callbacks == 2);
    require_cleanup(backend);
}

TEST_CASE("Server destruction performs silent bounded fallback cleanup",
          "[lifecycle][server-application]") {
    BackendState backend;
    ControlledBuffer buffer;
    std::ostream output{&buffer};
    SECTION("ordinary cleanup") {}
    SECTION("both restorations fail") {
        backend.fail_restore_terminate = true;
        backend.fail_restore_interrupt = true;
    }
    {
        shark::ServerApplication app{output, std::make_unique<TestBackend>(backend)};
        REQUIRE(app.start({}));
        // Destruction must not touch even an exception-enabled broken output.
        buffer.throw_write = true;
        buffer.throw_flush = true;
        output.exceptions(std::ios::badbit | std::ios::failbit);
    }
    REQUIRE(buffer.str() == ready);
    REQUIRE(backend.event_count == 7);
    REQUIRE(backend.events[3] == Event::restore_terminate);
    REQUIRE(backend.events[4] == Event::restore_interrupt);
    REQUIRE(backend.events[5] ==
            (backend.fail_restore_terminate ? Event::release_failed : Event::release_clean));
    REQUIRE(backend.events[6] == Event::destroy);
}

TEST_CASE("Failed native readiness restores process handlers and permits a new application",
          "[lifecycle][server-application]") {
    NativeHandlersFixture handlers;
    REQUIRE(handlers.installed());
    ControlledBuffer buffer;
    buffer.fail_write = true;
    std::ostream output{&buffer};
    shark::ServerApplication app{output};
    const auto failed = app.start({});
    REQUIRE_FALSE(failed);
    REQUIRE(failed.failure);
    REQUIRE(failed.failure->context.operation == "server.startup.readiness");
    REQUIRE_FALSE(failed.cleanup_failures[0]);
    REQUIRE_FALSE(failed.cleanup_failures[1]);
    REQUIRE(app.state() == shark::ServerState::stopped);
    REQUIRE(std::signal(SIGTERM, previous_terminate) == previous_terminate);
    REQUIRE(std::signal(SIGINT, previous_interrupt) == previous_interrupt);
    buffer.fail_write = false;
    output.clear();
    REQUIRE(app.start({}));
    REQUIRE(app.stop());
    std::ostringstream next_output;
    shark::ServerApplication next{next_output};
    REQUIRE(next.start({}));
    REQUIRE(next.stop());
}
