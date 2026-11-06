#pragma once

#include "shark/world_cell.hpp"

#include <filesystem>
#include <stop_token>

namespace shark {

// Bounded local file input for a trusted content host. The path is not a player
// capability or a source importer. Returns owned bytes only; the cell loader
// still validates their envelope, key, revision and asset closure. Reads check
// cooperative cancellation between chunks, but cannot preempt a native IO call
// or provide a transactional snapshot of concurrent in-place file writes.
[[nodiscard]] Result<std::vector<std::byte>>
read_world_cell_file(const std::filesystem::path& path,
                     std::size_t maximum_bytes = world_cell_maximum_payload_bytes,
                     std::stop_token stop = {});

} // namespace shark
