#include <shark/bootstrap.hpp>
#include <shark/client/bootstrap.hpp>
#include <shark/client/presentation_launch.hpp>
#include <shark/client/window_launch.hpp>
#include <shark/log.hpp>
#include <shark/tools/build_info.hpp>

#ifdef SHARK_EDITOR_GUI
#include "editor_shell.hpp"
#endif

#include <array>
#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    shark::Logger logger{std::cerr};
    const auto invalid_arguments = [&logger](const shark::Error& error) {
        if (!logger.write(shark::LogLevel::error, error)) {
            return 1;
        }
        std::cerr
            << "Usage: Editor [--help | --version | --editor [--root PATH] [--project RELATIVE] "
               "[--device UUID|auto] [--validation required|off] [--gui-vertex SPV --gui-pixel "
               "SPV] [--scene-vertex SPV --scene-pixel SPV] | "
               "--window [--window-ms 1..60000] | "
               "--graphics | --cube [--graphics-ms 1..60000] [--device UUID|auto] "
               "[--validation required|off] [--shader-bundle PATH]]\n"
            << std::flush;
        return std::cerr ? 2 : 1;
    };
    if (argc >= 2 && std::string_view{argv[1]} == "--editor") {
#ifdef SHARK_EDITOR_GUI
        if (argc > 18)
            return invalid_arguments({shark::ErrorCode::invalid_argument,
                                      "Too many editor arguments",
                                      {.operation = "editor.arguments"}});
        std::array<std::string_view, 17> arguments;
        for (int index = 1; index < argc; ++index)
            arguments[static_cast<std::size_t>(index - 1)] = argv[index];
        auto launch = shark::editor::parse_editor_arguments(
            std::span{arguments}.first(static_cast<std::size_t>(argc - 1)));
        if (!launch)
            return invalid_arguments(launch.error());
        const auto result = shark::editor::run_editor(launch.value());
        if (!result) {
            static_cast<void>(logger.write(shark::LogLevel::error, result.error()));
            return 1;
        }
        return 0;
#else
        static_cast<void>(
            logger.write(shark::LogLevel::error, {shark::ErrorCode::capability_unavailable,
                                                  "This build has no native graphical editor",
                                                  {.operation = "editor.run"}}));
        return 1;
#endif
    }
    // Explicit opt-in: informational commands never initialize a display.
    if (argc >= 2 &&
        (std::string_view{argv[1]} == "--graphics" || std::string_view{argv[1]} == "--cube")) {
        if (argc > 10) {
            return invalid_arguments({shark::ErrorCode::invalid_argument,
                                      "Too many presentation launch arguments",
                                      {.operation = "client.graphics.arguments.parse"}});
        }
        std::array<std::string_view, 9> graphics_arguments;
        for (int index = 1; index < argc; ++index) {
            graphics_arguments[static_cast<std::size_t>(index - 1)] = argv[index];
        }
        const auto launch = shark::client::parse_presentation_arguments(
            std::span{graphics_arguments}.first(static_cast<std::size_t>(argc - 1)));
        if (!launch) {
            return invalid_arguments(launch.error());
        }
        const auto result =
            shark::client::run_presentation_sample("Shark Editor - Presentation", launch.value());
        if (!result) {
            static_cast<void>(logger.write(shark::LogLevel::error, result.error()));
            return 1;
        }
        return 0;
    }
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
            shark::client::run_window_sample("Shark Editor - Window", launch.value());
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
        std::cout << shark::tools::editor_version_banner() << '\n';
        break;
    case shark::BootstrapAction::help:
        std::cout << shark::client::editor_help();
#ifdef SHARK_EDITOR_GUI
        std::cout << "  --editor [--root PATH] [--project RELATIVE] [--device UUID|auto]\n"
                     "           [--validation required|off] [--gui-vertex SPV --gui-pixel SPV]\n"
                     "           [--scene-vertex SPV --scene-pixel SPV]\n"
                     "      Open the project editor workspace (validation off by default).\n";
#endif
        break;
    }
    std::cout << std::flush;
    if (!std::cout) {
        const auto diagnostic =
            logger.write(shark::LogLevel::error, {shark::ErrorCode::io_failure,
                                                  "cannot write Editor output",
                                                  {.operation = "editor.output"}});
        static_cast<void>(diagnostic);
        return 1;
    }
    return 0;
}
