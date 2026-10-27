#pragma once

#include "shark/tools/gltf_source.hpp"

namespace shark::tools {

inline constexpr std::size_t gltf_texture_maximum_dimension = 4096;
inline constexpr std::size_t gltf_texture_maximum_pixels = 4 * 1024 * 1024;
inline constexpr std::size_t gltf_texture_maximum_decoded_bytes = 32 * 1024 * 1024;
inline constexpr std::size_t gltf_texture_maximum_mip_bytes = 32 * 1024 * 1024;
inline constexpr std::size_t gltf_texture_maximum_output_bytes = 36 * 1024 * 1024;
inline constexpr std::size_t gltf_texture_maximum_decoder_live_bytes = 64 * 1024 * 1024;
inline constexpr std::size_t gltf_texture_maximum_decoder_cumulative_bytes = 256 * 1024 * 1024;
inline constexpr std::size_t gltf_texture_maximum_work = 32 * 1024 * 1024;
inline constexpr std::size_t gltf_texture_maximum_usages = 4096;
inline constexpr std::size_t gltf_texture_maximum_variants = 3 * gltf_source_maximum_resources;

enum class GltfTextureRole { base_color, metallic_roughness, normal, occlusion, emissive };
enum class GltfTextureSemantic { srgb_color, linear_data, normal };
enum class GltfTextureFilter { nearest, linear };
enum class GltfTextureMipFilter { none, nearest, linear };
enum class GltfTextureWrap { clamp_to_edge, mirrored_repeat, repeat };

struct GltfTextureSampler {
    GltfTextureFilter magnification{GltfTextureFilter::linear};
    GltfTextureFilter minification{GltfTextureFilter::linear};
    GltfTextureMipFilter mip{GltfTextureMipFilter::linear};
    GltfTextureWrap wrap_s{GltfTextureWrap::repeat};
    GltfTextureWrap wrap_t{GltfTextureWrap::repeat};
    bool operator==(const GltfTextureSampler&) const = default;
};
struct GltfTextureMip {
    std::uint32_t width{};
    std::uint32_t height{};
    // Tightly packed, top-left RGBA. Eight-bit components use one byte;
    // sixteen-bit components use two bytes, least-significant byte first.
    std::vector<std::byte> rgba;
    bool operator==(const GltfTextureMip&) const = default;
};
struct GltfTextureImage {
    std::size_t source_image{};
    GltfTextureSemantic semantic{GltfTextureSemantic::linear_data};
    // 8 or 16. Source PNG16 precision is retained; other PNG and JPEG expand
    // to RGBA8. RGB remains sRGB encoded for srgb_color and linear otherwise.
    unsigned component_bits{8};
    std::vector<GltfTextureMip> mips;
    bool operator==(const GltfTextureImage&) const = default;
};
struct GltfTextureUsage {
    std::size_t source_material{};
    GltfTextureRole role{GltfTextureRole::base_color};
    std::size_t source_texture{};
    std::size_t texcoord{};
    std::size_t image{};
    GltfTextureSampler sampler;
    bool operator==(const GltfTextureUsage&) const = default;
};
struct GltfTextureSettings {
    // Same selected/default/scene-zero policy as geometry. No scenes is empty.
    std::optional<std::size_t> scene;
};
struct GltfTextures {
    GltfSourceInspection source;
    std::optional<std::size_t> selected_scene;
    // First usage order: ascending source material, then the five role enums.
    // Variants share only the same source image and processing semantic.
    std::vector<GltfTextureImage> images;
    std::vector<GltfTextureUsage> usages;
};

// Pure CPU selected-scene core texture conversion over immutable snapshots.
// Preserves the existing source boundary; no file, cache, Scene or device I/O.
[[nodiscard]] Result<GltfTextures> convert_gltf_textures(const GltfSourceInput& input,
                                                         const GltfTextureSettings& settings = {});

} // namespace shark::tools
