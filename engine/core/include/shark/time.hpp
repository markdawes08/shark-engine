#pragma once

#include <shark/error.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

namespace shark {

using MonotonicTime = std::chrono::nanoseconds;

// A nonnegative monotonic epoch, unrelated to durable or civil time. The owner
// supplies this interface so tests need no wall-clock sleeps.
class MonotonicClock {
  public:
    virtual ~MonotonicClock() = default;
    [[nodiscard]] virtual MonotonicTime now() noexcept = 0;
    [[nodiscard]] virtual Result<void> wait_for(MonotonicTime duration) = 0;
};

class SteadyMonotonicClock final : public MonotonicClock {
  public:
    [[nodiscard]] MonotonicTime now() noexcept override;
    [[nodiscard]] Result<void> wait_for(MonotonicTime duration) override;

  private:
    std::chrono::steady_clock::time_point origin_{std::chrono::steady_clock::now()};
};

// Construct callbacks outside poll. They run synchronously on the serialized
// simulation owner, not a worker, signal handler or real-time audio callback.
// Success acknowledges exactly one completed tick. Failure or an exception
// terminally faults the schedule; already-mutated simulation is never retried.
using TickCallback = std::function<Result<void>(std::uint64_t tick)>;

struct StepReport {
    std::uint32_t executed{0};
    std::uint64_t discarded{0};
    std::uint64_t pending{0};
    // Exact rational fraction of one tick: remainder_units / 1,000,000,000.
    std::uint64_t remainder_units{0};
    bool cancelled{false};
    bool clamped_delta{false};

    [[nodiscard]] double interpolation_alpha() const noexcept;
};

struct ServerTimingConfig {
    // Provisional tuning, not a capacity claim. Rates are integral 1..1000 Hz,
    // budgets 1..1024 callbacks/pass and debt limits 1..1,000,000 whole ticks.
    std::uint32_t tick_rate_hz{20};
    std::uint32_t max_ticks_per_pass{8};
    std::uint32_t max_pending_ticks{200};
};

struct OfflineControl {
    bool paused{false};
    // A request for this pass, valid only while paused. A successful step advances
    // exactly one tick without creating elapsed-time debt or leaving pause.
    bool single_step{false};
};

// Both schedules are single-owner objects. Never destroy one inside its callback.
// Calls from a callback are rejected without changing the outer pass. Successful
// poll paths allocate no storage except whatever the user callback does.
// Cancellation is terminal: remaining debt stays inspectable and later polls
// return a cancelled snapshot. Restart requires a deliberately new schedule.
class ServerScheduler final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<ServerScheduler>>
    create(ServerTimingConfig config, MonotonicTime origin, std::uint64_t first_tick = 0);

    ServerScheduler(const ServerScheduler&) = delete;
    ServerScheduler& operator=(const ServerScheduler&) = delete;
    ServerScheduler(ServerScheduler&&) = delete;
    ServerScheduler& operator=(ServerScheduler&&) = delete;

    [[nodiscard]] Result<StepReport> poll(MonotonicTime now, const TickCallback& callback,
                                          std::stop_token stop = {});
    [[nodiscard]] std::uint64_t next_tick() const noexcept;
    [[nodiscard]] StepReport backlog() const noexcept;
    [[nodiscard]] bool faulted() const noexcept;
    [[nodiscard]] const ServerTimingConfig& config() const noexcept;

  private:
    ServerScheduler(ServerTimingConfig config, MonotonicTime origin,
                    std::uint64_t first_tick) noexcept;
    [[nodiscard]] Result<StepReport> fail(ErrorCode code, const char* operation,
                                          const char* message);

    ServerTimingConfig config_;
    MonotonicTime last_;
    std::uint64_t next_tick_;
    std::uint64_t pending_{0};
    std::uint64_t remainder_{0};
    bool polling_{false};
    bool cancelled_{false};
    bool faulted_{false};
    std::optional<Error> failure_;
};

class OfflineScheduler final {
  public:
    // Fixed 60 Hz, 250 ms maximum admitted delta, 8 callbacks/pass. Clock
    // regression clamps to zero without lowering the previous high-water time.
    [[nodiscard]] static Result<std::unique_ptr<OfflineScheduler>>
    create(MonotonicTime origin, std::uint64_t first_tick = 0);

    OfflineScheduler(const OfflineScheduler&) = delete;
    OfflineScheduler& operator=(const OfflineScheduler&) = delete;
    OfflineScheduler(OfflineScheduler&&) = delete;
    OfflineScheduler& operator=(OfflineScheduler&&) = delete;

    [[nodiscard]] Result<StepReport> poll(MonotonicTime now, const TickCallback& callback,
                                          std::stop_token stop = {});
    // Pause clears accumulated time and admits none while paused. The first
    // resumed pass resets the clock anchor and executes no ticks; later passes
    // admit only fresh elapsed time. Regressions never lower the high-water
    // clock, including paused and resumed passes. A single step has zero alpha.
    // Invalid running single-step requests reject before consuming elapsed time.
    // Fault/cancellation remain terminal and preserve their existing snapshots.
    [[nodiscard]] Result<StepReport> poll(MonotonicTime now, const TickCallback& callback,
                                          OfflineControl control, std::stop_token stop = {});
    [[nodiscard]] std::uint64_t next_tick() const noexcept;
    [[nodiscard]] StepReport backlog() const noexcept;
    [[nodiscard]] bool faulted() const noexcept;

  private:
    OfflineScheduler(MonotonicTime origin, std::uint64_t first_tick) noexcept;
    [[nodiscard]] Result<StepReport> fail(ErrorCode code, const char* operation,
                                          const char* message);

    MonotonicTime last_;
    std::uint64_t next_tick_;
    std::uint64_t pending_{0};
    std::uint64_t remainder_{0};
    bool polling_{false};
    bool cancelled_{false};
    bool faulted_{false};
    bool paused_{false};
    std::optional<Error> failure_;
};

} // namespace shark
