#pragma once

#include <shark/error.hpp>
#include <shark/server_config.hpp>
#include <shark/shutdown_signals.hpp>

#include <array>
#include <iosfwd>
#include <memory>
#include <optional>
#include <stop_token>

namespace shark {

class MonotonicClock;

enum class ServerState { stopped, starting, running, stopping, faulted };
enum class ServerStopReason { duration_elapsed, interrupt, terminate, requested };

struct LifecycleReport {
    std::optional<Error> failure;
    // Restoration order: SIGTERM, then SIGINT. An empty slot means no failure.
    std::array<std::optional<Error>, 2> cleanup_failures;

    [[nodiscard]] bool ok() const noexcept {
        return !failure && !cleanup_failures[0] && !cleanup_failures[1];
    }
    explicit operator bool() const noexcept {
        return ok();
    }
};

// The application exclusively owns shutdown-signal registration. The root owns
// output and must keep it alive until after this application. All lifecycle calls
// belong to one ordinary thread; concurrent use is not supported. Reentrant
// start/stop calls are rejected without altering the operation in progress.
// Expected failures own diagnostics. Failed acquisition/readiness is rolled back
// before allocating those diagnostics; allocation failure itself is not an
// expected recoverable outcome. Rejected transitions preserve existing ownership.
class ServerApplication final {
  public:
    explicit ServerApplication(std::ostream& output, std::unique_ptr<SignalBackend> backend =
                                                         make_native_signal_backend()) noexcept;
    ~ServerApplication() noexcept;

    ServerApplication(const ServerApplication&) = delete;
    ServerApplication& operator=(const ServerApplication&) = delete;
    ServerApplication(ServerApplication&&) = delete;
    ServerApplication& operator=(ServerApplication&&) = delete;

    [[nodiscard]] LifecycleReport start(ServerConfig candidate);
    [[nodiscard]] LifecycleReport stop(ServerStopReason reason = ServerStopReason::requested);
    // Runs only the bootstrap idle deadline; it does not execute simulation ticks.
    // Expected wait/clock failures leave ownership running so the caller can stop
    // explicitly and retain cleanup diagnostics. The clock is borrowed for this call.
    [[nodiscard]] Result<ServerStopReason> run_idle(MonotonicClock& clock,
                                                    std::stop_token stop = {});
    [[nodiscard]] ServerState state() const noexcept;
    [[nodiscard]] int requested_signal() const noexcept;
    // Last validated candidate; only running state authorizes using its duration.
    [[nodiscard]] const ServerConfig& config() const noexcept;

  private:
    std::ostream& output_;
    ShutdownSignals signals_;
    ServerConfig config_;
    ServerState state_{ServerState::stopped};
    bool polling_{false};
};

} // namespace shark
