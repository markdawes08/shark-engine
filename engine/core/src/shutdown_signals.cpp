#include <shark/shutdown_signals.hpp>

#include <atomic>
#include <csignal>
#include <utility>

namespace shark {
namespace {

// This is the sole asynchronous bridge. No logging, allocation, backend dispatch
// or atomics are used by the handler. The atomic lease is ordinary-thread only.
volatile std::sig_atomic_t shutdown_signal = 0;
std::atomic_flag native_claimed = ATOMIC_FLAG_INIT;

extern "C" void shark_shutdown_signal_bridge(int signal) {
    shutdown_signal = signal;
}

class NativeSignalBackend final : public SignalBackend {
  public:
    bool try_claim() noexcept override {
        if (claimed_ || native_claimed.test_and_set(std::memory_order_acquire)) {
            return false;
        }
        claimed_ = true;
        shutdown_signal = 0;
        return true;
    }

    SignalHandler exchange(int signal, SignalHandler handler) noexcept override {
        return std::signal(signal, handler);
    }

    int requested_signal() const noexcept override {
        return shutdown_signal;
    }

    void release(bool restored) noexcept override {
        if (claimed_) {
            claimed_ = false;
            if (restored) {
                native_claimed.clear(std::memory_order_release);
            }
        }
    }

  private:
    bool claimed_{false};
};

} // namespace

std::unique_ptr<SignalBackend> make_native_signal_backend() {
    return std::make_unique<NativeSignalBackend>();
}

bool SignalCleanup::ok() const noexcept {
    return !terminate_failed && !interrupt_failed;
}

bool SignalStartResult::ok() const noexcept {
    return failure == SignalStartFailure::none && cleanup.ok();
}

ShutdownSignals::ShutdownSignals(std::unique_ptr<SignalBackend> backend) noexcept
    : backend_(std::move(backend)) {}

ShutdownSignals::~ShutdownSignals() noexcept {
    static_cast<void>(stop());
}

SignalStartResult ShutdownSignals::start() noexcept {
    if (active_ || cleanup_failed_) {
        return {SignalStartFailure::invalid_state};
    }
    if (!backend_) {
        return {SignalStartFailure::missing_backend};
    }
    if (!backend_->try_claim()) {
        return {SignalStartFailure::already_owned};
    }
    claimed_ = true;
    previous_interrupt_ = backend_->exchange(SIGINT, shark_shutdown_signal_bridge);
    if (previous_interrupt_ == SIG_ERR) {
        return {SignalStartFailure::install_interrupt, stop()};
    }
    interrupt_acquired_ = true;
    previous_terminate_ = backend_->exchange(SIGTERM, shark_shutdown_signal_bridge);
    if (previous_terminate_ == SIG_ERR) {
        return {SignalStartFailure::install_terminate, stop()};
    }
    terminate_acquired_ = true;
    active_ = true;
    return {};
}

SignalCleanup ShutdownSignals::stop() noexcept {
    SignalCleanup result;
    active_ = false;
    if (terminate_acquired_) {
        terminate_acquired_ = false;
        result.terminate_failed = backend_->exchange(SIGTERM, previous_terminate_) == SIG_ERR;
    }
    if (interrupt_acquired_) {
        interrupt_acquired_ = false;
        result.interrupt_failed = backend_->exchange(SIGINT, previous_interrupt_) == SIG_ERR;
    }
    if (claimed_) {
        claimed_ = false;
        backend_->release(result.ok());
    }
    cleanup_failed_ = cleanup_failed_ || !result.ok();
    return result;
}

int ShutdownSignals::requested_signal() const noexcept {
    return active_ ? backend_->requested_signal() : 0;
}

bool ShutdownSignals::active() const noexcept {
    return active_;
}

} // namespace shark
