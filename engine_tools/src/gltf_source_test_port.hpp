#pragma once

#include <cstddef>
#include <optional>

namespace shark::tools::detail {

// Calling-thread fixture only. Fail the selected cgltf allocation (zero based),
// or disable the sweep. Never exposed through the public authoring API.
void gltf_fail_allocation(std::optional<std::size_t> index) noexcept;
[[nodiscard]] std::size_t gltf_allocation_attempts() noexcept;
[[nodiscard]] std::size_t gltf_live_allocations() noexcept;

} // namespace shark::tools::detail
