#pragma once

#include <memory>

namespace shark {

using SignalHandler = void (*)(int);

// Calls are serialized by the application owner, never made from a signal handler.
// Backends report failure without throwing or allocating during start/stop.
class SignalBackend {
  public:
    virtual ~SignalBackend() = default;
    [[nodiscard]] virtual bool try_claim() noexcept = 0;
    [[nodiscard]] virtual SignalHandler exchange(int signal, SignalHandler handler) noexcept = 0;
    [[nodiscard]] virtual int requested_signal() const noexcept = 0;
    virtual void release(bool restored) noexcept = 0;
};

// Native signals are process-global. One lease owns both handlers. Failed restoration
// poisons that lease until process exit, rather than losing a previous handler on retry.
[[nodiscard]] std::unique_ptr<SignalBackend> make_native_signal_backend();

enum class SignalStartFailure {
    none,
    invalid_state,
    missing_backend,
    already_owned,
    install_interrupt,
    install_terminate
};

struct SignalCleanup {
    bool terminate_failed{false};
    bool interrupt_failed{false};

    [[nodiscard]] bool ok() const noexcept;
};

struct SignalStartResult {
    SignalStartFailure failure{SignalStartFailure::none};
    SignalCleanup cleanup{};

    [[nodiscard]] bool ok() const noexcept;
};

// Owns one injectable backend and the handlers acquired through it. This object is
// main-thread/owner-thread only. Clean cycles may repeat; cleanup failure is terminal.
class ShutdownSignals final {
  public:
    explicit ShutdownSignals(
        std::unique_ptr<SignalBackend> backend = make_native_signal_backend()) noexcept;
    ~ShutdownSignals() noexcept;
    ShutdownSignals(const ShutdownSignals&) = delete;
    ShutdownSignals& operator=(const ShutdownSignals&) = delete;
    ShutdownSignals(ShutdownSignals&&) = delete;
    ShutdownSignals& operator=(ShutdownSignals&&) = delete;

    [[nodiscard]] SignalStartResult start() noexcept;
    [[nodiscard]] SignalCleanup stop() noexcept;
    [[nodiscard]] int requested_signal() const noexcept;
    [[nodiscard]] bool active() const noexcept;

  private:
    std::unique_ptr<SignalBackend> backend_;
    SignalHandler previous_interrupt_{};
    SignalHandler previous_terminate_{};
    bool claimed_{false};
    bool interrupt_acquired_{false};
    bool terminate_acquired_{false};
    bool active_{false};
    bool cleanup_failed_{false};
};

} // namespace shark
