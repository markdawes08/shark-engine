#pragma once

#include "shark/render_components.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace shark {

inline constexpr std::uint32_t prepared_mesh_version = 1;
inline constexpr std::size_t prepared_mesh_maximum_payload_bytes = 2 * 1024 * 1024;
inline constexpr std::size_t prepared_mesh_maximum_decoded_bytes = 16 * 1024 * 1024;
inline constexpr std::size_t prepared_mesh_maximum_meshes = 4096;
inline constexpr std::size_t prepared_mesh_maximum_primitives = 16384;
inline constexpr std::size_t prepared_mesh_maximum_vertices = 262144;
inline constexpr std::size_t prepared_mesh_maximum_indices = 786432;
inline constexpr std::size_t prepared_assets_maximum_count = 128;
inline constexpr std::size_t prepared_assets_maximum_bytes = 32 * 1024 * 1024;

// Owned CPU values, not packed GPU layouts. No source document, importer, path,
// GPU handle or runtime entity is embedded in a prepared mesh payload.
struct PreparedVertex {
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 4> tangent{1.0F, 0.0F, 0.0F, 1.0F};
    std::array<float, 2> texcoord0{};
    std::array<float, 2> texcoord1{};
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    bool operator==(const PreparedVertex&) const = default;
};
struct PreparedMaterial {
    std::array<float, 4> base_color{1.0F, 1.0F, 1.0F, 1.0F};
    bool double_sided{false};
    bool operator==(const PreparedMaterial&) const = default;
};
struct PreparedPrimitive {
    std::vector<PreparedVertex> vertices;
    std::vector<std::uint32_t> indices;
    PreparedMaterial material;
    bool operator==(const PreparedPrimitive&) const = default;
};
struct PreparedMesh {
    std::vector<PreparedPrimitive> primitives;
    bool operator==(const PreparedMesh&) const = default;
};
struct PreparedMeshAsset {
    AssetId asset;
    std::vector<PreparedMesh> meshes;
    bool operator==(const PreparedMeshAsset&) const = default;
};

[[nodiscard]] Result<std::vector<std::byte>> encode_prepared_mesh(const PreparedMeshAsset& asset);
[[nodiscard]] Result<PreparedMeshAsset> decode_prepared_mesh(std::span<const std::byte> payload);

// Engine-reserved stable identities. Built-ins need no files or downloads.
[[nodiscard]] AssetId builtin_cube_asset_id();
[[nodiscard]] AssetId builtin_plane_asset_id();

struct PreparedAssetInput {
    AssetId asset;
    std::span<const std::byte> payload;
};

// Complete immutable resource snapshot. Create a replacement privately and swap
// it only after all scene references validate. Existing shared leases survive
// snapshot replacement/destruction; the caller owns their separate lifetime.
class PreparedAssets final {
  public:
    [[nodiscard]] static Result<std::shared_ptr<const PreparedAssets>>
    create(std::span<const PreparedAssetInput> inputs = {});
    [[nodiscard]] Result<std::shared_ptr<const PreparedMeshAsset>> find(AssetId asset) const;
    [[nodiscard]] Result<std::shared_ptr<const PreparedMeshAsset>>
    resolve(const MeshRenderer& renderer) const;
    [[nodiscard]] std::size_t size() const noexcept;
    // Logical owned payload/vector charge, excluding allocator overhead and
    // caller-retained snapshots. Not a process RSS or GPU memory measurement.
    [[nodiscard]] std::size_t retained_bytes() const noexcept;

  private:
    PreparedAssets(std::vector<std::shared_ptr<const PreparedMeshAsset>> assets,
                   std::size_t retained_bytes);
    std::vector<std::shared_ptr<const PreparedMeshAsset>> assets_;
    std::size_t retained_bytes_;
};

} // namespace shark
