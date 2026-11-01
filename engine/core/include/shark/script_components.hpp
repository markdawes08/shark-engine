#pragma once

#include "shark/error.hpp"

#include <cstddef>
#include <string>

namespace shark {

inline constexpr std::size_t script_minimum_source_path_bytes = 13;
inline constexpr std::size_t script_maximum_source_path_bytes = 4096;

// Authored source reference only. Storing this value never loads or executes Lua.
// The eventual execution owner must select the appropriate gameplay or
// presentation capabilities before any script can run.
struct Script {
    std::string source{"scripts/main.lua"};
    bool enabled{true};
    bool operator==(const Script&) const = default;
};

[[nodiscard]] Result<Script> validate_script(Script script);

} // namespace shark
