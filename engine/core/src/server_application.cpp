#include <shark/server_application.hpp>
#include <shark/time.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <ostream>
#include <string_view>
#include <utility>

namespace shark {
namespace {

void append_cleanup_failures(LifecycleReport& report, const SignalCleanup cleanup) {
    if (cleanup.terminate_failed) {
        report.cleanup_failures[0] = Error{ErrorCode::execution_failed,
                                           "cannot restore previous SIGTERM handler",
                                           {.operation = "server.shutdown.signals.terminate"}};
    }
    if (cleanup.interrupt_failed) {
        report.cleanup_failures[1] = Error{ErrorCode::execution_failed,
                                           "cannot restore previous SIGINT handler",
                                           {.operation = "server.shutdown.signals.interrupt"}};
    }
}

Error signal_start_error(const SignalStartFailure failure) {
    switch (failure) {
    case SignalStartFailure::invalid_state:
        return {ErrorCode::conflict,
                "shutdown signal ownership cannot be started in its current state",
                {.operation = "server.startup.signals.state"}};
    case SignalStartFailure::missing_backend:
        return {ErrorCode::capability_unavailable,
                "shutdown signal backend is unavailable",
                {.operation = "server.startup.signals.backend"}};
    case SignalStartFailure::already_owned:
        return {ErrorCode::conflict,
                "shutdown signals already have an owner",
                {.operation = "server.startup.signals.claim"}};
    case SignalStartFailure::install_interrupt:
        return {ErrorCode::execution_failed,
                "cannot install SIGINT handler",
                {.operation = "server.startup.signals.interrupt"}};
    case SignalStartFailure::install_terminate:
        return {ErrorCode::execution_failed,
                "cannot install SIGTERM handler",
                {.operation = "server.startup.signals.terminate"}};
    case SignalStartFailure::none:
        break;
    }
    return {ErrorCode::execution_failed,
            "unknown shutdown signal startup failure",
            {.operation = "server.startup.signals"}};
}

bool write_readiness(std::ostream& output) noexcept {
    try {
        output << "Shark Server ready (headless idle)\n" << std::flush;
        return static_cast<bool>(output);
    } catch (...) {
        // User-supplied stream buffers may throw types other than ios::failure.
        return false;
    }
}

bool write_completion(std::ostream& output, const ServerStopReason reason) noexcept {
    const std::string_view text = [reason] {
        switch (reason) {
        case ServerStopReason::duration_elapsed:
            return "run duration elapsed";
        case ServerStopReason::interrupt:
            return "SIGINT";
        case ServerStopReason::terminate:
            return "SIGTERM";
        case ServerStopReason::requested:
            return "shutdown requested";
        }
        return "shutdown requested";
    }();
    try {
        output << "Shark Server stopped: " << text << '\n' << std::flush;
        return static_cast<bool>(output);
    } catch (...) {
        return false;
    }
}

} // namespace

ServerApplication::ServerApplication(std::ostream& output,
                                     std::unique_ptr<SignalBackend> backend) noexcept
    : output_(output), signals_(std::move(backend)) {}

ServerApplication::~ServerApplication() noexcept {
    // Fallback cleanup cannot allocate, log or touch a potentially failed sink.
    // Explicit stop is required when the caller needs cleanup diagnostics.
    static_cast<void>(signals_.stop());
}

LifecycleReport ServerApplication::start(ServerConfig candidate) {
    if (state_ != ServerState::stopped) {
        return {Error{ErrorCode::conflict,
                      "Server application must be stopped before starting",
                      {.operation = "server.startup.state"}},
                {}};
    }
    const auto validated = validate_server_config(candidate);
    if (!validated) {
        return {validated.error(), {}};
    }
    config_ = validated.value();
    state_ = ServerState::starting;
    const auto startup = signals_.start();
    if (startup.failure != SignalStartFailure::none) {
        // The signal owner already unwound its partial acquisition. Commit state
        // before constructing owned error strings, which may allocate.
        state_ = startup.cleanup.ok() ? ServerState::stopped : ServerState::faulted;
        LifecycleReport report{signal_start_error(startup.failure), {}};
        append_cleanup_failures(report, startup.cleanup);
        return report;
    }
    if (!write_readiness(output_)) {
        const auto cleanup = signals_.stop();
        state_ = cleanup.ok() ? ServerState::stopped : ServerState::faulted;
        LifecycleReport report{Error{ErrorCode::io_failure,
                                     "cannot report readiness",
                                     {.operation = "server.startup.readiness"}},
                               {}};
        append_cleanup_failures(report, cleanup);
        return report;
    }
    state_ = ServerState::running;
    return {};
}

LifecycleReport ServerApplication::stop(const ServerStopReason reason) {
    if (polling_) {
        return {Error{ErrorCode::conflict,
                      "Server idle loop must return before releasing its resources",
                      {.operation = "server.shutdown.state"}},
                {}};
    }
    if (state_ == ServerState::stopped) {
        return {};
    }
    if (state_ != ServerState::running) {
        return {Error{ErrorCode::conflict,
                      "Server application cannot stop in its current state",
                      {.operation = "server.shutdown.state"}},
                {}};
    }
    state_ = ServerState::stopping;
    const auto cleanup = signals_.stop();
    if (!cleanup.ok()) {
        state_ = ServerState::faulted;
        LifecycleReport report;
        append_cleanup_failures(report, cleanup);
        return report;
    }
    const bool reported = write_completion(output_, reason);
    state_ = ServerState::stopped;
    if (!reported) {
        return {Error{ErrorCode::io_failure,
                      "cannot report completion",
                      {.operation = "server.shutdown.completion"}},
                {}};
    }
    return {};
}

Result<ServerStopReason> ServerApplication::run_idle(MonotonicClock& clock,
                                                     const std::stop_token stop) {
    if (state_ != ServerState::running || polling_) {
        return Error{ErrorCode::conflict,
                     "Server idle loop requires a running application with no active loop",
                     {.operation = "server.timing.state"}};
    }
    struct PollGuard {
        bool& flag;
        explicit PollGuard(bool& value) noexcept : flag(value) {
            flag = true;
        }
        ~PollGuard() noexcept {
            flag = false;
        }
    } guard{polling_};

    if (stop.stop_requested()) {
        return ServerStopReason::requested;
    }
    const auto started = clock.now();
    if (started < MonotonicTime::zero()) {
        return Error{ErrorCode::execution_failed,
                     "Server monotonic clock epoch must be nonnegative",
                     {.operation = "server.timing.clock"}};
    }
    std::optional<MonotonicTime> deadline;
    if (config_.run_for) {
        const auto duration = std::chrono::duration_cast<MonotonicTime>(*config_.run_for);
        if (started > MonotonicTime::max() - duration) {
            return Error{ErrorCode::execution_failed,
                         "Server deadline exceeds monotonic clock range",
                         {.operation = "server.timing.deadline"}};
        }
        deadline = started + duration;
    }
    auto previous = started;
    bool waited = false;
    constexpr MonotonicTime poll_interval = std::chrono::milliseconds{20};
    while (true) {
        const auto signal = requested_signal();
        if (signal == SIGINT) {
            return ServerStopReason::interrupt;
        }
        if (signal == SIGTERM) {
            return ServerStopReason::terminate;
        }
        if (stop.stop_requested()) {
            return ServerStopReason::requested;
        }
        const auto now = clock.now();
        if (now < previous || (waited && now == previous)) {
            return Error{ErrorCode::execution_failed,
                         "Server monotonic clock regressed or did not advance after a wait",
                         {.operation = "server.timing.clock"}};
        }
        if (deadline && now >= *deadline) {
            return ServerStopReason::duration_elapsed;
        }
        const auto delay = deadline ? std::min(poll_interval, *deadline - now) : poll_interval;
        previous = now;
        try {
            const auto result = clock.wait_for(delay);
            if (!result) {
                return result.error();
            }
        } catch (...) {
            return Error{ErrorCode::execution_failed,
                         "Server clock wait threw an exception",
                         {.operation = "server.timing.wait"}};
        }
        waited = true;
    }
}

ServerState ServerApplication::state() const noexcept {
    return state_;
}

int ServerApplication::requested_signal() const noexcept {
    return signals_.requested_signal();
}

const ServerConfig& ServerApplication::config() const noexcept {
    return config_;
}

} // namespace shark
