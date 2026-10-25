#pragma once

#include "shark/tools/scene_runner.hpp"

#include <filesystem>
#include <span>
#include <string_view>

namespace shark::tools {

// Trusted native regression cases for the versioned assets/FeatureWorld source.
// The caller loads source through ProjectFiles and supplies an existing writable
// scratch parent. Only the persistence case creates an owned temporary child;
// it never saves to the input project. These are tests, not authoring adapters.
[[nodiscard]] std::span<const std::string_view> feature_world_case_ids() noexcept;
[[nodiscard]] std::vector<SceneRunCase>
feature_world_cases(const std::filesystem::path& scratch_parent);

} // namespace shark::tools
