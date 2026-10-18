#include <shark/time.hpp>

#include <algorithm>
#include <limits>
#include <thread>
#include <utility>

namespace shark {
namespace {

constexpr std::uint64_t units_per_tick = 1'000'000'000;

Error timing_error(const ErrorCode code, const char* operation, const char* message) {
    return Error{code, message, ErrorContext{operation}};
}

class PollGuard final {
  public:
    explicit PollGuard(bool& polling) noexcept : polling_(polling) {
        polling_ = true;
    }
    ~PollGuard() {
        polling_ = false;
    }
    PollGuard(const PollGuard&) = delete;
    PollGuard& operator=(const PollGuard&) = delete;

  private:
    bool& polling_;
};

void accrue(const MonotonicTime elapsed, const std::uint32_t rate, std::uint64_t& pending,
            std::uint64_t& remainder) noexcept {
    const auto nanoseconds = static_cast<std::uint64_t>(elapsed.count());
    // Decomposition avoids nanoseconds * rate overflowing for extreme clocks.
    // elapsed <= INT64_MAX, rate <= 1000 and prior pending <= 1,000,000.
    const auto partial = (nanoseconds % units_per_tick) * rate + remainder;
    pending += (nanoseconds / units_per_tick) * rate + partial / units_per_tick;
    remainder = partial % units_per_tick;
}

StepReport snapshot(const std::uint64_t pending, const std::uint64_t remainder,
                    const bool cancelled) noexcept {
    return StepReport{0, 0, pending, remainder, cancelled, false};
}

} // namespace

MonotonicTime SteadyMonotonicClock::now() noexcept {
    return std::chrono::duration_cast<MonotonicTime>(std::chrono::steady_clock::now() - origin_);
}

Result<void> SteadyMonotonicClock::wait_for(const MonotonicTime duration) {
    // A wait is a bounded polling opportunity, never an uninterruptible request
    // for an arbitrary future date. Owners choose shorter stop-check intervals.
    if (duration < MonotonicTime::zero() || duration > std::chrono::seconds{1}) {
        return timing_error(ErrorCode::invalid_argument, "clock.wait",
                            "Wait duration must be between zero and one second");
    }
    try {
        std::this_thread::sleep_for(duration);
    } catch (...) {
        return timing_error(ErrorCode::execution_failed, "clock.wait", "Monotonic wait failed");
    }
    return {};
}

double StepReport::interpolation_alpha() const noexcept {
    return static_cast<double>(remainder_units) / static_cast<double>(units_per_tick);
}

Result<std::unique_ptr<ServerScheduler>> ServerScheduler::create(const ServerTimingConfig config,
                                                                 const MonotonicTime origin,
                                                                 const std::uint64_t first_tick) {
    if (config.tick_rate_hz == 0 || config.tick_rate_hz > 1000 || config.max_ticks_per_pass == 0 ||
        config.max_ticks_per_pass > 1024 || config.max_pending_ticks == 0 ||
        config.max_pending_ticks > 1'000'000) {
        return timing_error(ErrorCode::invalid_configuration, "timing.server.configuration",
                            "Server timing rate, callback budget or debt limit is out of range");
    }
    if (origin < MonotonicTime::zero()) {
        return timing_error(ErrorCode::invalid_argument, "timing.server.origin",
                            "Monotonic origin must be nonnegative");
    }
    return std::unique_ptr<ServerScheduler>{new ServerScheduler{config, origin, first_tick}};
}

ServerScheduler::ServerScheduler(const ServerTimingConfig config, const MonotonicTime origin,
                                 const std::uint64_t first_tick) noexcept
    : config_(config), last_(origin), next_tick_(first_tick) {}

Result<StepReport> ServerScheduler::fail(const ErrorCode code, const char* operation,
                                         const char* message) {
    faulted_ = true;
    failure_ = timing_error(code, operation, message);
    return *failure_;
}

Result<StepReport> ServerScheduler::poll(const MonotonicTime now, const TickCallback& callback,
                                         const std::stop_token stop) {
    if (polling_) {
        return timing_error(ErrorCode::conflict, "timing.server.reentrant",
                            "A scheduling pass is already active");
    }
    if (faulted_) {
        return failure_ ? Result<StepReport>{*failure_}
                        : fail(ErrorCode::execution_failed, "timing.server.faulted",
                               "The server schedule is faulted");
    }
    if (cancelled_ || stop.stop_requested()) {
        cancelled_ = true;
        return backlog();
    }
    if (!callback) {
        return timing_error(ErrorCode::invalid_argument, "timing.server.callback",
                            "A tick callback is required");
    }
    if (now < last_) {
        return fail(ErrorCode::execution_failed, "timing.server.clock_regression",
                    "Monotonic scheduling time moved backwards");
    }
    PollGuard guard{polling_};
    accrue(now - last_, config_.tick_rate_hz, pending_, remainder_);
    last_ = now;
    if (pending_ > config_.max_pending_ticks) {
        // Keep the offending observation inspectable; a fault never admits more
        // time, discards debt or pretends the region has caught up.
        return fail(ErrorCode::execution_failed, "timing.server.overload",
                    "Authoritative tick debt exceeded the configured limit");
    }
    std::uint32_t executed = 0;
    while (pending_ != 0 && executed < config_.max_ticks_per_pass) {
        if (stop.stop_requested()) {
            cancelled_ = true;
            break;
        }
        if (next_tick_ == std::numeric_limits<std::uint64_t>::max()) {
            return fail(ErrorCode::execution_failed, "timing.server.tick_overflow",
                        "Authoritative tick index is exhausted");
        }
        Result<void> result;
        try {
            result = callback(next_tick_);
        } catch (...) {
            return fail(ErrorCode::execution_failed, "timing.server.callback",
                        "The tick callback threw an exception");
        }
        if (!result) {
            faulted_ = true;
            failure_ = std::move(result).error();
            return *failure_;
        }
        ++next_tick_;
        --pending_;
        ++executed;
    }
    cancelled_ = cancelled_ || stop.stop_requested();
    auto report = backlog();
    report.executed = executed;
    return report;
}

std::uint64_t ServerScheduler::next_tick() const noexcept {
    return next_tick_;
}
StepReport ServerScheduler::backlog() const noexcept {
    return snapshot(pending_, remainder_, cancelled_);
}
bool ServerScheduler::faulted() const noexcept {
    return faulted_;
}
const ServerTimingConfig& ServerScheduler::config() const noexcept {
    return config_;
}

Result<std::unique_ptr<OfflineScheduler>> OfflineScheduler::create(const MonotonicTime origin,
                                                                   const std::uint64_t first_tick) {
    if (origin < MonotonicTime::zero()) {
        return timing_error(ErrorCode::invalid_argument, "timing.offline.origin",
                            "Monotonic origin must be nonnegative");
    }
    return std::unique_ptr<OfflineScheduler>{new OfflineScheduler{origin, first_tick}};
}

OfflineScheduler::OfflineScheduler(const MonotonicTime origin,
                                   const std::uint64_t first_tick) noexcept
    : last_(origin), next_tick_(first_tick) {}

Result<StepReport> OfflineScheduler::fail(const ErrorCode code, const char* operation,
                                          const char* message) {
    faulted_ = true;
    failure_ = timing_error(code, operation, message);
    return *failure_;
}

Result<StepReport> OfflineScheduler::poll(const MonotonicTime now, const TickCallback& callback,
                                          const std::stop_token stop) {
    if (polling_) {
        return timing_error(ErrorCode::conflict, "timing.offline.reentrant",
                            "A scheduling pass is already active");
    }
    if (faulted_) {
        return failure_ ? Result<StepReport>{*failure_}
                        : fail(ErrorCode::execution_failed, "timing.offline.faulted",
                               "The offline schedule is faulted");
    }
    if (cancelled_ || stop.stop_requested()) {
        cancelled_ = true;
        return backlog();
    }
    if (!callback) {
        return timing_error(ErrorCode::invalid_argument, "timing.offline.callback",
                            "A tick callback is required");
    }
    PollGuard guard{polling_};
    MonotonicTime elapsed{0};
    bool clamped = now < last_;
    if (now >= last_) {
        elapsed = now - last_;
        last_ = now;
    }
    constexpr auto maximum_delta = std::chrono::milliseconds{250};
    if (elapsed > maximum_delta) {
        elapsed = maximum_delta;
        clamped = true;
    }
    accrue(elapsed, 60, pending_, remainder_);
    std::uint32_t executed = 0;
    while (pending_ != 0 && executed < 8) {
        if (stop.stop_requested()) {
            cancelled_ = true;
            break;
        }
        if (next_tick_ == std::numeric_limits<std::uint64_t>::max()) {
            return fail(ErrorCode::execution_failed, "timing.offline.tick_overflow",
                        "Offline tick index is exhausted");
        }
        Result<void> result;
        try {
            result = callback(next_tick_);
        } catch (...) {
            return fail(ErrorCode::execution_failed, "timing.offline.callback",
                        "The tick callback threw an exception");
        }
        if (!result) {
            faulted_ = true;
            failure_ = std::move(result).error();
            return *failure_;
        }
        ++next_tick_;
        --pending_;
        ++executed;
    }
    cancelled_ = cancelled_ || stop.stop_requested();
    auto report = backlog();
    report.executed = executed;
    report.clamped_delta = clamped;
    if (!cancelled_) {
        report.discarded = pending_;
        pending_ = 0;
        report.pending = 0;
    }
    return report;
}

std::uint64_t OfflineScheduler::next_tick() const noexcept {
    return next_tick_;
}
StepReport OfflineScheduler::backlog() const noexcept {
    return snapshot(pending_, remainder_, cancelled_);
}
bool OfflineScheduler::faulted() const noexcept {
    return faulted_;
}

} // namespace shark
