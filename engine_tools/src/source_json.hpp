#pragma once

#include "shark/error.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string_view>

namespace shark::tools::detail {

// Reuses the source codec's bounded SAX preflight, including duplicate decoded
// keys, finite numbers, depth/node/string limits, before constructing a DOM.
[[nodiscard]] Result<nlohmann::json> parse_source_json(std::string_view text,
                                                       std::size_t maximum_string_bytes,
                                                       std::size_t maximum_nodes);

} // namespace shark::tools::detail
