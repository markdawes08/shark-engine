#include <shark/bootstrap.hpp>
#include <shark/log.hpp>
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

int invalid_configuration(shark::Logger& logger, const shark::Error& error) {
    if (!logger.write(shark::LogLevel::error, error)) {
        return 1;
    }
    std::cerr << usage << std::flush;
    return std::cerr ? 2 : 1;
}

int report_failure(shark::Logger& logger, const shark::Error& error) {
    // Do not recursively log a failed diagnostic sink; failure still exits nonzero.
    const auto diagnostic = logger.write(shark::LogLevel::error, error);
    static_cast<void>(diagnostic);
    return 1;
}

int run_server(shark::Logger& logger, const shark::ServerConfig& config) {
    const auto validated = shark::validate_server_config(config);
    if (!validated) {
        return invalid_configuration(logger, validated.error());
    }
    SignalHandlers signals;
    if (!signals.install()) {
        return report_failure(logger, {shark::ErrorCode::execution_failed,
                                       "cannot install shutdown signal handlers",
                                       {.operation = "server.startup.signals"}});
    }
    std::cout << "Shark Server ready (headless idle)\n" << std::flush;
    if (!std::cout) {
        return report_failure(logger, {shark::ErrorCode::io_failure,
                                       "cannot report readiness",
                                       {.operation = "server.startup.readiness"}});
    }

    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    const auto duration = validated.value().run_for;
    const auto deadline = duration ? started + *duration : Clock::time_point::max();
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
        return report_failure(logger, {shark::ErrorCode::io_failure,
                                       "cannot report completion",
                                       {.operation = "server.shutdown.completion"}});
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    shark::Logger logger{std::cerr};
    // Bound argument storage before constructing views or parsing token text.
    if (argc < 1) {
        return invalid_configuration(logger, {shark::ErrorCode::invalid_configuration,
                                              "missing executable argument",
                                              {.operation = "server.arguments.parse"}});
    }
    if (argc > 3) {
        return invalid_configuration(logger, {shark::ErrorCode::invalid_configuration,
                                              "too many arguments",
                                              {.operation = "server.arguments.parse"}});
    }
    std::array<std::string_view, 2> arguments;
    for (int index = 1; index < argc; ++index) {
        arguments[static_cast<std::size_t>(index - 1)] = argv[index];
    }
    const auto parsed = shark::parse_server_arguments(
        std::span{arguments}.first(static_cast<std::size_t>(argc - 1)));
    if (!parsed) {
        return invalid_configuration(logger, parsed.error());
    }
    switch (parsed.value().action) {
    case shark::ServerAction::help:
        std::cout << usage
                  << "Shark headless idle process; networking and simulation are not implemented.\n"
                     "With no arguments, wait for SIGINT or SIGTERM.\n"
                     "--run-for-ms N stops after N milliseconds (1..600000; 1-6 decimal digits).\n"
                  << std::flush;
        break;
    case shark::ServerAction::version:
        std::cout << "Shark Server " << shark::version_string() << '\n' << std::flush;
        break;
    case shark::ServerAction::run:
        return run_server(logger, parsed.value().config);
    }
    if (!std::cout) {
        return report_failure(logger, {shark::ErrorCode::io_failure,
                                       "cannot write Server output",
                                       {.operation = "server.output"}});
    }
    return 0;
}
