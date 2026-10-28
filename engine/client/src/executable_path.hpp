#pragma once

#include <shark/error.hpp>

#include <filesystem>

namespace shark::client::detail {
[[nodiscard]] Result<std::filesystem::path> executable_directory();
}
