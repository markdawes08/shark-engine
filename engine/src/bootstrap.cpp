#include <shark/bootstrap.hpp>

#include "version_config.hpp"

namespace shark {

BootstrapAction parse_bootstrap_arguments(std::span<const std::string_view> arguments) noexcept {
    if (arguments.empty()) {
        return BootstrapAction::help;
    }
    if (arguments.size() != 1) {
        return BootstrapAction::invalid;
    }
    if (arguments.front() == "--help") {
        return BootstrapAction::help;
    }
    if (arguments.front() == "--version") {
        return BootstrapAction::version;
    }
    return BootstrapAction::invalid;
}

std::string_view version_string() noexcept {
    return SHARK_VERSION_STRING;
}

} // namespace shark
