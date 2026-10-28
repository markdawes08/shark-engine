#pragma once

#include <shark/client/gpu_resources.hpp>
#include <shark/prepared_startup.hpp>

namespace shark::client {

struct LoadedPreparedPackage {
    PreparedStartup startup;
    ContentDigest startup_digest;
    std::unique_ptr<LoadedPreparedScene> content;
    ShaderArtifact vertex_shader;
    ShaderArtifact pixel_shader;
};

// CPU-only validation of one complete package generation. All file references
// are fixed, contained paths relative to the descriptor's parent directory.
// Decoding consumes the same owned bytes whose lengths and hashes were checked.
// No source readers, authoring services or graphics device are used.
[[nodiscard]] Result<LoadedPreparedPackage>
load_prepared_package(const std::filesystem::path& startup_file);
[[nodiscard]] Result<std::filesystem::path> default_prepared_startup_path();

} // namespace shark::client
