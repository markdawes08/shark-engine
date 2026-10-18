#pragma once

#include <string_view>

namespace shark::client {

// Actual client bootstrap presentation; Core and Server do not depend on it.
[[nodiscard]] std::string_view runtime_help() noexcept;
[[nodiscard]] std::string_view editor_help() noexcept;

} // namespace shark::client
