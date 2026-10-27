#pragma once

#include <cstddef>
#include <optional>

namespace shark::tools::detail {

// Calling-thread decoder fixture only. Counts every private decoder allocation,
// including PNG framing scratch. No controls enter the public authoring API.
void gltf_image_fail_allocation(std::optional<std::size_t> position) noexcept;
[[nodiscard]] std::size_t gltf_image_allocation_attempts() noexcept;
[[nodiscard]] std::size_t gltf_image_live_allocations() noexcept;

} // namespace shark::tools::detail
