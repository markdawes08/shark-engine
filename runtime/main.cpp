#include <shark/bootstrap.hpp>
#include <shark/client/bootstrap.hpp>
#include <shark/client/window_launch.hpp>
#include <shark/log.hpp>

#include <array>
#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    shark::Logger logger{std::cerr};
    const auto invalid_arguments = [&logger](const shark::Error& error) {
        if (!logger.write(shark::LogLevel::error, error)) {
            return 1;
        }
        std::cerr << "Usage: Runtime [--help | --version | --window [--window-ms 1..60000]]\n"
                  << std::flush;
        return std::cerr ? 2 : 1;
    };
    // Explicit opt-in: informational commands never initialize a display.
    if (argc >= 2 && std::string_view{argv[1]} == "--window") {
        if (argc > 4) {
            return invalid_arguments({shark::ErrorCode::invalid_argument,
                                      "Too many window launch arguments",
                                      {.operation = "client.window.arguments.parse"}});
        }
        std::array<std::string_view, 3> window_arguments;
        for (int index = 1; index < argc; ++index) {
            window_arguments[static_cast<std::size_t>(index - 1)] = argv[index];
        }
        const auto launch = shark::client::parse_window_arguments(
            std::span{window_arguments}.first(static_cast<std::size_t>(argc - 1)));
        if (!launch) {
            return invalid_arguments(launch.error());
        }
        const auto result =
            shark::client::run_window_sample("Shark Runtime - Window", launch.value());
        if (!result) {
            static_cast<void>(logger.write(shark::LogLevel::error, result.error()));
            return 1;
        }
        return 0;
    }
    // Bound argument storage before constructing views of caller text.
    if (argc < 1 || argc > 2) {
        return invalid_arguments({shark::ErrorCode::invalid_argument,
                                  "Unsupported arguments: expected --help or --version",
                                  {.operation = "bootstrap.arguments.parse"}});
    }
    std::array<std::string_view, 1> arguments;
    for (int index = 1; index < argc; ++index) {
        arguments[static_cast<std::size_t>(index - 1)] = argv[index];
    }
    const auto parsed = shark::parse_bootstrap_arguments(
        std::span{arguments}.first(static_cast<std::size_t>(argc - 1)));
    if (!parsed) {
        return invalid_arguments(parsed.error());
    }
    switch (parsed.value()) {
    case shark::BootstrapAction::version:
        std::cout << "Shark Runtime " << shark::version_string() << '\n';
        break;
    case shark::BootstrapAction::help:
        std::cout << shark::client::runtime_help();
        break;
    }
    std::cout << std::flush;
    if (!std::cout) {
        // A failed diagnostic sink cannot change this failure into success.
        const auto diagnostic =
            logger.write(shark::LogLevel::error, {shark::ErrorCode::io_failure,
                                                  "cannot write Runtime output",
                                                  {.operation = "runtime.output"}});
        static_cast<void>(diagnostic);
        return 1;
    }
    return 0;
}
