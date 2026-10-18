#include <shark/bootstrap.hpp>
#include <shark/client/bootstrap.hpp>
#include <shark/log.hpp>
#include <shark/tools/build_info.hpp>

#include <array>
#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    shark::Logger logger{std::cerr};
    const auto invalid_arguments = [&logger](const shark::Error& error) {
        if (!logger.write(shark::LogLevel::error, error)) {
            return 1;
        }
        std::cerr << "Usage: Editor [--help | --version]\n" << std::flush;
        return std::cerr ? 2 : 1;
    };
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
