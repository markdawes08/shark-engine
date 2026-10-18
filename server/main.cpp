#include <shark/bootstrap.hpp>
#include <shark/server_config.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string_view>
#include <thread>

namespace {

volatile std::sig_atomic_t shutdown_signal = 0;

extern "C" void request_shutdown(int signal) {
    // No allocation, I/O, clock or other library calls are safe here.
    shutdown_signal = signal;
}

class SignalHandlers {
  public:
    SignalHandlers() = default;
    SignalHandlers(const SignalHandlers&) = delete;
    SignalHandlers& operator=(const SignalHandlers&) = delete;

    ~SignalHandlers() {
        if (term_installed_) {
            std::signal(SIGTERM, previous_term_);
        }
        if (int_installed_) {
            std::signal(SIGINT, previous_int_);
        }
    }

    [[nodiscard]] bool install() noexcept {
        previous_int_ = std::signal(SIGINT, request_shutdown);
        if (previous_int_ == SIG_ERR) {
            return false;
        }
        int_installed_ = true;
        previous_term_ = std::signal(SIGTERM, request_shutdown);
        if (previous_term_ == SIG_ERR) {
            return false;
        }
        term_installed_ = true;
        return true;
    }

  private:
    using Handler = void (*)(int);
    Handler previous_int_ = SIG_DFL;
    Handler previous_term_ = SIG_DFL;
    bool int_installed_ = false;
    bool term_installed_ = false;
};

constexpr std::string_view usage = "Usage: Server [--help | --version | --run-for-ms N]\n";

int invalid_configuration(std::string_view error) {
    std::cerr << "Shark Server: invalid configuration: " << error << '\n' << usage;
    return 2;
}

int run_server(const shark::ServerConfig& config) {
    SignalHandlers signals;
    if (!signals.install()) {
        std::cerr << "Shark Server: startup failed: cannot install shutdown signal handlers\n";
        return 1;
    }
    std::cout << "Shark Server ready (headless idle)\n" << std::flush;
    if (!std::cout) {
        std::cerr << "Shark Server: startup failed: cannot report readiness\n";
        return 1;
    }

    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    const auto deadline = config.run_for ? started + *config.run_for : Clock::time_point::max();
    constexpr auto poll_interval = std::chrono::milliseconds{20};
    while (shutdown_signal == 0) {
        const auto now = Clock::now();
        if (now >= deadline) {
            break;
        }
        std::this_thread::sleep_until(std::min(now + poll_interval, deadline));
    }

    const auto signal = shutdown_signal;
    const auto reason = signal == SIGINT    ? "SIGINT"
                        : signal == SIGTERM ? "SIGTERM"
                                            : "run duration elapsed";
    std::cout << "Shark Server stopped: " << reason << '\n' << std::flush;
    if (!std::cout) {
        std::cerr << "Shark Server: shutdown failed: cannot report completion\n";
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    // Bound argument storage before constructing views or parsing token text.
    if (argc < 1) {
        return invalid_configuration("missing executable argument");
    }
    if (argc > 3) {
        return invalid_configuration("too many arguments");
    }
    std::array<std::string_view, 2> arguments;
    for (int index = 1; index < argc; ++index) {
        arguments[static_cast<std::size_t>(index - 1)] = argv[index];
    }
    const auto parsed = shark::parse_server_arguments(
        std::span{arguments}.first(static_cast<std::size_t>(argc - 1)));
    switch (parsed.action) {
    case shark::ServerAction::help:
        std::cout << usage
                  << "Shark headless idle process; networking and simulation are not implemented.\n"
                     "With no arguments, wait for SIGINT or SIGTERM.\n"
                     "--run-for-ms N stops after N milliseconds (1..600000; 1-6 decimal digits).\n"
                  << std::flush;
        return std::cout ? 0 : 1;
    case shark::ServerAction::version:
        std::cout << "Shark Server " << shark::version_string() << '\n' << std::flush;
        return std::cout ? 0 : 1;
    case shark::ServerAction::invalid:
        return invalid_configuration(parsed.error);
    case shark::ServerAction::run:
        return run_server(parsed.config);
    }
    return 2;
}
