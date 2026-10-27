#pragma once

#include "shark/tools/gltf_source.hpp"
#include "shark/transform.hpp"

#include <array>

namespace shark::tools {

inline constexpr std::size_t gltf_geometry_maximum_vertices = 262144;
inline constexpr std::size_t gltf_geometry_maximum_indices = 786432;
inline constexpr std::size_t gltf_geometry_maximum_output_bytes = 32 * 1024 * 1024;
inline constexpr std::size_t gltf_geometry_maximum_work = 8 * 1024 * 1024;

// These are owned CPU values, not a packed GPU layout or a serialized payload.
struct GltfGeometryVertex {
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 4> tangent{1.0F, 0.0F, 0.0F, 1.0F};
    std::array<float, 2> texcoord0{};
    std::array<float, 2> texcoord1{};
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    bool operator==(const GltfGeometryVertex&) const = default;
};
struct GltfGeometryBounds {
    math::Vec3 minimum{0.0};
    math::Vec3 maximum{0.0};
    bool operator==(const GltfGeometryBounds&) const = default;
};
struct GltfGeometryPrimitive {
    std::size_t source_mesh{};
    std::size_t source_primitive{};
    std::optional<std::size_t> source_material;
    std::vector<GltfGeometryVertex> vertices;
    std::vector<std::uint32_t> indices;
    GltfGeometryBounds bounds;
    bool has_texcoord0{};
    bool has_texcoord1{};
    bool has_color{};
    bool has_tangents{};
    bool generated_normals{};
    bool generated_tangents{};
};
struct GltfGeometryMesh {
    std::size_t source_mesh{};
    std::string name;
    std::vector<std::size_t> primitives;
    GltfGeometryBounds bounds;
};
struct GltfGeometryNode {
    std::size_t source_node{};
    std::string name;
    std::optional<std::size_t> parent;
    std::vector<std::size_t> children;
    std::optional<std::size_t> mesh;
    Transform local;
    math::Mat4 local_matrix{1.0};
    math::Mat4 world_matrix{1.0};
    // Column-major inverse-transpose linear matrix, valid for world shear.
    std::array<double, 9> normal_matrix{};
    bool mirrored{};
    std::optional<GltfGeometryBounds> world_bounds;
};
struct GltfGeometrySettings {
    // Absent selects the source default, then scene zero. No source scenes
    // produces an empty conversion; this never implicitly imports loose nodes.
    std::optional<std::size_t> scene;
};
struct GltfGeometry {
    GltfSourceInspection source;
    std::optional<std::size_t> selected_scene;
    std::vector<GltfGeometryPrimitive> primitives;
    std::vector<GltfGeometryMesh> meshes;
    // Root/child source order, parents before children. Meshes are shared and
    // ordered by source index; all references above are dense output indices.
    std::vector<GltfGeometryNode> nodes;
    std::vector<std::size_t> roots;
    std::optional<GltfGeometryBounds> world_bounds;
};

// Pure CPU conversion over the same immutable snapshots and validation as
// inspect_gltf_source. Local triangle winding/tangent signs are retained;
// instance reflection is reported on the node, never baked into shared meshes.
[[nodiscard]] Result<GltfGeometry> convert_gltf_geometry(const GltfSourceInput& input,
                                                         const GltfGeometrySettings& settings = {});

} // namespace shark::tools
