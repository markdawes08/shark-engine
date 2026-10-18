#pragma once

#include <shark/client/gpu_resources.hpp>

namespace shark::client {

// Versioned, bounded distribution payload exported from a verified offline
// generation. Hashes detect damaged payloads; this is not a signature or a
// sandbox for untrusted shader programs. No source/compiler discovery occurs.
struct ShaderBundle {
    ShaderArtifact vertex_shader;
    ShaderArtifact pixel_shader;
    std::string sha256;
};

[[nodiscard]] Result<ShaderBundle> decode_shader_bundle(std::span<const std::byte> bytes);
[[nodiscard]] Result<ShaderBundle> load_shader_bundle(const std::filesystem::path& path);
// Resolves the running executable, independently of the working directory and
// argv[0]. Distribution puts the bundle at assets/cube.sharkshader beside it.
[[nodiscard]] Result<std::filesystem::path> default_shader_bundle_path();

} // namespace shark::client
