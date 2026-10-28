#include <shark/bootstrap.hpp>
#include <shark/client/bootstrap.hpp>
#include <shark/client/prepared_package.hpp>
#include <shark/client/presentation_launch.hpp>
#include <shark/client/scene_launch.hpp>
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
        std::cerr << shark::client::runtime_help() << std::flush;
        return std::cerr ? 2 : 1;
    };
    const auto launch_scene = [&logger](const shark::client::SceneLaunchOptions& options) {
        auto result = shark::client::run_scene("Shark Runtime - Scene", options);
        if (!result) {
            static_cast<void>(logger.write(shark::LogLevel::error, result.error()));
            return 1;
        }
        return 0;
    };
    if (argc == 1) {
        auto path = shark::client::default_prepared_startup_path();
        if (!path) {
            static_cast<void>(logger.write(shark::LogLevel::error, path.error()));
            return 1;
        }
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path.value(), error);
        if (error && error != std::errc::no_such_file_or_directory) {
            static_cast<void>(
                logger.write(shark::LogLevel::error, {shark::ErrorCode::io_failure,
                                                      "Cannot inspect adjacent package startup",
                                                      {.operation = "runtime.package.locate"}}));
            return 1;
        }
        if (status.type() != std::filesystem::file_type::not_found && !error) {
            shark::client::SceneLaunchOptions options;
            options.package = path.value();
            return launch_scene(options);
        }
    }
    if (argc >= 2 && std::string_view{argv[1]} == "--check-package") {
        if (argc != 3)
            return invalid_arguments({shark::ErrorCode::invalid_argument,
                                      "Expected --check-package PATH",
                                      {.operation = "client.package.arguments.parse"}});
        const std::array arguments{std::string_view{"--package"}, std::string_view{argv[2]}};
        auto parsed = shark::client::parse_scene_arguments(arguments);
        if (!parsed)
            return invalid_arguments(parsed.error());
        auto loaded = shark::client::load_prepared_package(parsed.value().package);
        if (!loaded) {
            static_cast<void>(logger.write(shark::LogLevel::error, loaded.error()));
            return 1;
        }
        const auto& value = loaded.value();
        const auto& startup = value.startup;
        std::cout
            << "{\"format\":\"shark.package-inspection\",\"schema_version\":1,\"status\":\"PASS\","
               "\"renderer_initialized\":false,\"target\":\""
            << startup.target << "\",\"project\":\"" << startup.project.to_string()
            << "\",\"world\":\"" << startup.world.value().to_string() << "\",\"scene\":\""
            << startup.scene.value().to_string() << "\",\"camera\":\""
            << startup.camera.value().to_string() << "\",\"startup_sha256\":\""
            << value.startup_digest.to_hex() << "\",\"mesh_count\":" << startup.meshes.size()
            << ",\"qualification\":\"NOT_RUN\"}\n"
            << std::flush;
        if (!std::cout) {
            static_cast<void>(
                logger.write(shark::LogLevel::error, {shark::ErrorCode::io_failure,
                                                      "Cannot write package inspection",
                                                      {.operation = "runtime.package.output"}}));
            return 1;
        }
        return 0;
    }
    if (argc >= 2 &&
        (std::string_view{argv[1]} == "--scene" || std::string_view{argv[1]} == "--package")) {
        if (static_cast<std::size_t>(argc - 1) > shark::client::scene_launch_maximum_arguments) {
            return invalid_arguments({shark::ErrorCode::invalid_argument,
                                      "Too many scene launch arguments",
                                      {.operation = "client.scene.arguments.parse"}});
        }
        std::array<std::string_view, shark::client::scene_launch_maximum_arguments> arguments;
        for (int index = 1; index < argc; ++index)
            arguments[static_cast<std::size_t>(index - 1)] = argv[index];
        auto parsed = shark::client::parse_scene_arguments(
            std::span{arguments}.first(static_cast<std::size_t>(argc - 1)));
        if (!parsed)
            return invalid_arguments(parsed.error());
        return launch_scene(parsed.value());
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
            shark::client::run_presentation_sample("Shark Runtime - Presentation", launch.value());
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
