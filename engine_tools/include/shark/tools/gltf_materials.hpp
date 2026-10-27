#pragma once

#include "shark/tools/gltf_textures.hpp"

#include <array>

namespace shark::tools {

inline constexpr std::size_t gltf_material_maximum_output_bytes = 2 * 1024 * 1024;

enum class GltfAlphaMode { opaque, mask, blend };

struct GltfMaterialTexture {
    std::size_t source_texture{};
    std::size_t texcoord{};
    bool operator==(const GltfMaterialTexture&) const = default;
};
struct GltfMaterial {
    std::size_t source_material{};
    std::string name;
    std::array<float, 4> base_color{1.0F, 1.0F, 1.0F, 1.0F};
    float metallic{1.0F};
    float roughness{1.0F};
    std::array<float, 3> emissive{};
    float normal_scale{1.0F};
    float occlusion_strength{1.0F};
    GltfAlphaMode alpha_mode{GltfAlphaMode::opaque};
    float alpha_cutoff{0.5F};
    bool double_sided{};
    // Indexed by GltfTextureRole. Resolve to a GltfTextures usage by this
    // material's source_material and role, never by an invented persistent ID.
    std::array<std::optional<GltfMaterialTexture>, 5> textures{};
    bool operator==(const GltfMaterial&) const = default;
};
struct GltfMaterialSettings {
    // Same selected/default/scene-zero policy as geometry and textures.
    std::optional<std::size_t> scene;
};
struct GltfMaterials {
    GltfSourceInspection source;
    std::optional<std::size_t> selected_scene;
    // Reachable explicit materials, ordered by source index. A geometry
    // primitive with no source_material uses the default factors above and no
    // textures; no synthetic source index or output record is assigned to it.
    std::vector<GltfMaterial> materials;
};

// Pure CPU core metallic-roughness material conversion. Uses the same immutable
// source owner and UV validation as textures, without decoding images, opening
// files or publishing to a cache/Scene/device.
[[nodiscard]] Result<GltfMaterials>
convert_gltf_materials(const GltfSourceInput& input, const GltfMaterialSettings& settings = {});

} // namespace shark::tools
