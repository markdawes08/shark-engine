#include <shark/bootstrap.hpp>
#include <shark/log.hpp>
#include <shark/server_application.hpp>
#include <shark/server_config.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string_view>
#include <thread>

namespace {

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

int report_lifecycle_failure(shark::Logger& logger, const shark::LifecycleReport& report) {
    if (report.failure) {
        static_cast<void>(report_failure(logger, *report.failure));
    }
    for (const auto& cleanup_failure : report.cleanup_failures) {
        if (cleanup_failure) {
            static_cast<void>(report_failure(logger, *cleanup_failure));
        }
    }
    return 1;
}

int run_server(shark::Logger& logger, const shark::ServerConfig& config) {
    shark::ServerApplication application{std::cout};
    const auto startup = application.start(config);
    if (!startup) {
        if (startup.failure && startup.failure->code == shark::ErrorCode::invalid_configuration) {
            return invalid_configuration(logger, *startup.failure);
        }
        return report_lifecycle_failure(logger, startup);
    }

    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    const auto duration = application.config().run_for;
    const auto deadline = duration ? started + *duration : Clock::time_point::max();
    constexpr auto poll_interval = std::chrono::milliseconds{20};
    while (application.requested_signal() == 0) {
        const auto now = Clock::now();
        if (now >= deadline) {
            break;
        }
        std::this_thread::sleep_until(std::min(now + poll_interval, deadline));
    }

    const auto signal = application.requested_signal();
    const auto reason = signal == SIGINT    ? shark::ServerStopReason::interrupt
                        : signal == SIGTERM ? shark::ServerStopReason::terminate
                                            : shark::ServerStopReason::duration_elapsed;
    const auto shutdown = application.stop(reason);
    return shutdown ? 0 : report_lifecycle_failure(logger, shutdown);
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
