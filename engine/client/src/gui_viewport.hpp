#pragma once

#include <shark/client/scene_viewport.hpp>

#include <optional>

namespace shark::client::detail {

// Scene revision alone does not identify a camera, extent or packed request.
// Reuse GPU resources only when every supplied viewport value is unchanged.
[[nodiscard]] bool same_gui_viewport(const std::optional<SceneViewportRequest>&,
                                     const std::optional<SceneViewportRequest>&) noexcept;

} // namespace shark::client::detail
