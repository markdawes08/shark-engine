#pragma once

#include <shark/client/window.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace shark::client {

enum class ShaderStage { vertex, pixel };
enum class ShaderInterface { textured_cube, scene_unlit };

// Owned precompiled SPIR-V. Callers explicitly select the bounded interface;
// the default preserves the textured-cube contract. This reader is not a full
// SPIR-V validator or an authentication boundary.
// Offline publication must first pass the shader pipeline's independent
// validation and artifact integrity checks. Requests revalidate mutable values.
struct ShaderArtifact {
    ShaderStage stage{ShaderStage::vertex};
    std::string entry_point;
    std::vector<std::uint32_t> words;
};

[[nodiscard]] Result<ShaderArtifact>
load_shader_artifact(const std::filesystem::path& path, ShaderStage stage,
                     ShaderInterface shader_interface = ShaderInterface::textured_cube);
[[nodiscard]] Result<ShaderArtifact>
decode_shader_artifact(std::span<const std::byte> bytes, ShaderStage stage,
                       ShaderInterface shader_interface = ShaderInterface::textured_cube);
[[nodiscard]] Result<void>
validate_shader_artifact(const ShaderArtifact& artifact,
                         ShaderInterface shader_interface = ShaderInterface::textured_cube);

struct ResourceVertex {
    std::array<float, 3> position{};
    std::array<float, 2> uv{};
};

// Bounded synchronous diagnostic draw, not a scene/material/streaming API.
// Triangles use counterclockwise front faces and backface culling. The shader
// performs the only projection Y inversion. Matrices use column-major storage.
// Texture and output RGB are linear UNORM; alpha is UNORM. Texture origin and
// tightly packed readback origin are top-left, with nearest/clamp sampling.
struct OffscreenRequest {
    WindowExtent extent{128, 128};
    std::vector<ResourceVertex> vertices;
    std::vector<std::uint32_t> indices;
    WindowExtent texture_extent{};
    std::vector<std::uint8_t> texture_rgba8;
    std::array<float, 16> transform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    ShaderArtifact vertex_shader;
    ShaderArtifact pixel_shader;
};

// Both extents are 1..1024, vertices 3..65536, indices 3..196608 and a
// multiple of three. Texture storage is exactly width * height * 4 bytes.
[[nodiscard]] Result<void> validate_offscreen_request(const OffscreenRequest& request);

enum class ReadbackColorEncoding { linear, srgb };

struct OffscreenReadback {
    WindowExtent extent{};
    // RGB transfer only; alpha is always linear UNORM. Window captures use
    // sRGB attachment encoding, offscreen diagnostic draws use linear UNORM.
    ReadbackColorEncoding color_encoding{ReadbackColorEncoding::linear};
    // Native rasterizer framebuffer-coordinate precision, queried from the
    // selected device. Required by an independent reference rasterizer.
    std::uint32_t subpixel_precision_bits{0};
    std::vector<std::uint8_t> rgba8;
    std::vector<float> depth;
    std::uint64_t submitted{0};
    std::uint64_t completed{0};
    std::uint32_t created_resources{0};
    std::uint32_t retired_resources{0};
};

} // namespace shark::client
