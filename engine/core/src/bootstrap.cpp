#include <shark/bootstrap.hpp>

#include "version_config.hpp"

namespace shark {

Result<BootstrapAction> parse_bootstrap_arguments(std::span<const std::string_view> arguments) {
    if (arguments.empty()) {
        return BootstrapAction::help;
    }
    if (arguments.size() == 1 && arguments.front() == "--help") {
        return BootstrapAction::help;
    }
    if (arguments.size() == 1 && arguments.front() == "--version") {
        return BootstrapAction::version;
    }
    return Error{ErrorCode::invalid_argument,
                 "Unsupported arguments: expected --help or --version",
                 {.operation = "bootstrap.arguments.parse"}};
}

std::string_view version_string() noexcept {
    return SHARK_VERSION_STRING;
}

} // namespace shark
