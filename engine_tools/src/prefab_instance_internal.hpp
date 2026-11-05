#pragma once

#include "shark/tools/prefab_instance.hpp"

namespace shark::tools::detail {

struct ExpandedPrefabInstance {
    // Preserves source array order; parent-first indices are separate.
    std::vector<SourceEntity> entities;
    std::vector<std::size_t> ordered;
};

[[nodiscard]] Result<ExpandedPrefabInstance>
expand_prefab_instance(const InstantiatePrefabCommand& command);

} // namespace shark::tools::detail
