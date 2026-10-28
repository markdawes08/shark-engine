#pragma once

#include <shark/prepared_scene.hpp>

#include <filesystem>

namespace shark::client {

// Explicit prepared development files. Paths are native artifact inputs, not
// project/source references. Built-in cube/plane assets require no mesh files.
struct SceneContentFiles {
    std::filesystem::path scene;
    std::vector<std::filesystem::path> meshes;
};

// CPU-only candidate loading; no source import, graphics or current-owner write.
// At most 126 external meshes and 32 MiB combined encoded scene/mesh bytes.
// Each file also retains its Core codec bound. A successful replacement has a
// fresh Scene incarnation; existing owners and immutable leases remain valid.
[[nodiscard]] Result<std::unique_ptr<LoadedPreparedScene>>
load_scene_content(const SceneContentFiles& files);

} // namespace shark::client
