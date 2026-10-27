#pragma once

#include "shark/tools/gltf_geometry.hpp"
#include "shark/tools/gltf_materials.hpp"

namespace shark::tools {

inline constexpr std::uint32_t gltf_asset_payload_version = 1;
inline constexpr std::size_t gltf_asset_maximum_payload_bytes = asset_import_maximum_payload_bytes;
inline constexpr std::size_t gltf_asset_maximum_decoded_bytes = 16 * 1024 * 1024;

struct GltfAsset {
    GltfGeometry geometry;
    GltfTextures textures;
    GltfMaterials materials;
};

// Pure CPU assembly and separately versioned, little-endian engine payload.
// The geometry inspection report is serialized once and restored to all three
// views; geometry's conversion diagnostics form the combined report. Source
// formats, encoded images and parser objects are never embedded in the payload.
// Payloads retain the existing 2 MiB per-artifact limit; conversion bounds are
// independent and a valid larger CPU conversion may fail payload encoding.
[[nodiscard]] Result<GltfAsset> convert_gltf_asset(const GltfSourceInput& input,
                                                   const GltfGeometrySettings& settings = {});
[[nodiscard]] Result<std::vector<std::byte>> encode_gltf_asset(const GltfAsset& asset,
                                                               std::stop_token stop = {});
[[nodiscard]] Result<GltfAsset> decode_gltf_asset(std::span<const std::byte> payload,
                                                  std::stop_token stop = {});

// Identity-only external resource payload. Settings are exactly an empty
// object; dependencies are forbidden. This is distinct from shark.bytes, whose
// transformed bodies cannot establish original glTF source truth.
class GltfResourceImporter final : public AssetImporter {
  public:
    [[nodiscard]] const AssetImporterInfo& info() const noexcept override;
    [[nodiscard]] Result<std::string> normalize_settings(std::string_view json) const override;
    [[nodiscard]] Result<std::vector<std::byte>> import(const AssetImportInput& input) override;
    [[nodiscard]] Result<void> validate_payload(const AssetArtifact& artifact) const override;
};

// Settings: required logical `path`, optional `scene`, and optional `resources`
// array of {path, asset} URI aliases to direct dependency AssetIds. Alias paths
// are part of settings/cache identity, never filesystem requests. ProjectFiles
// and AssetImports capture native source bytes and retain publication ownership.
class GltfAssetImporter final : public AssetImporter {
  public:
    [[nodiscard]] const AssetImporterInfo& info() const noexcept override;
    [[nodiscard]] Result<std::string> normalize_settings(std::string_view json) const override;
    [[nodiscard]] Result<std::vector<std::byte>> import(const AssetImportInput& input) override;
    [[nodiscard]] Result<void> validate_payload(const AssetArtifact& artifact) const override;
};

} // namespace shark::tools
