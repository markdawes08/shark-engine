#pragma once

#include <shark/error.hpp>

#include <span>
#include <string_view>

namespace shark {

enum class BootstrapAction { help, version };

// Arguments exclude argv[0]. No arguments show foundation help, not a game loop.
[[nodiscard]] Result<BootstrapAction>
parse_bootstrap_arguments(std::span<const std::string_view> arguments);

[[nodiscard]] std::string_view version_string() noexcept;

} // namespace shark
