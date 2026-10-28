#pragma once

#include "shark/tools/asset_import.hpp"
#include "shark/tools/prepared_content.hpp"

namespace shark::tools {

inline constexpr std::size_t client_export_maximum_meshes = prepared_assets_maximum_count - 2;
inline constexpr std::size_t client_export_maximum_sources = asset_import_maximum_nodes;
inline constexpr std::size_t client_export_maximum_bytes = prepared_assets_maximum_bytes;
// Sum of unique dependency nodes visited per root, including repeated shared
// closures. Bounds the existing freshness queries' source recapture work.
inline constexpr std::size_t client_export_maximum_dependency_visits = 512;

struct ClientExportRequest {
    AuthoredEntityId camera;
    SceneRevision scene_revision;
    AssetCatalogRevision catalog_revision;
    AssetCacheRevision cache_revision;
    // Declared content profile, not a binary-platform compatibility claim.
    AssetTargetProfile target;
};

struct ClientExportMesh {
    AssetId asset;
    AssetCacheKey source_key;
    ContentDigest digest;
    std::vector<std::byte> payload;
};
struct ClientExportSource {
    AssetId asset;
    AssetCacheKey key;
    // Attribution for the used import closure, without source paths/settings/extras.
    std::string origin;
    std::string license;
};

// Immutable owned content candidate; no source, cache or export directory is
// written. Dirty authoring state is captured explicitly and never acknowledged
// as saved. Packaging must revalidate immediately before publishing its files.
class PreparedClientExport final {
  public:
    PreparedClientExport(PreparedClientExport&&) noexcept = default;
    PreparedClientExport& operator=(PreparedClientExport&&) noexcept = default;
    PreparedClientExport(const PreparedClientExport&) = delete;
    PreparedClientExport& operator=(const PreparedClientExport&) = delete;

    [[nodiscard]] ProjectId project() const noexcept {
        return project_;
    }
    [[nodiscard]] WorldId world() const noexcept {
        return world_;
    }
    [[nodiscard]] SceneId scene() const noexcept {
        return scene_;
    }
    [[nodiscard]] const Name& name() const& noexcept {
        return name_;
    }
    const Name& name() const&& = delete;
    [[nodiscard]] const ClientExportRequest& request() const& noexcept {
        return request_;
    }
    const ClientExportRequest& request() const&& = delete;
    [[nodiscard]] bool dirty() const noexcept {
        return dirty_;
    }
    [[nodiscard]] std::span<const std::byte> scene_payload() const& noexcept {
        return scene_payload_;
    }
    std::span<const std::byte> scene_payload() const&& = delete;
    [[nodiscard]] ContentDigest scene_digest() const noexcept {
        return scene_digest_;
    }
    [[nodiscard]] std::span<const ClientExportMesh> meshes() const& noexcept {
        return meshes_;
    }
    std::span<const ClientExportMesh> meshes() const&& = delete;
    [[nodiscard]] std::span<const ClientExportSource> sources() const& noexcept {
        return sources_;
    }
    std::span<const ClientExportSource> sources() const&& = delete;
    [[nodiscard]] std::size_t payload_bytes() const noexcept {
        return payload_bytes_;
    }

  private:
    PreparedClientExport(ProjectId project, WorldId world, SceneId scene, Name name,
                         ClientExportRequest request, bool dirty)
        : project_(project), world_(world), scene_(scene), name_(std::move(name)),
          request_(std::move(request)), dirty_(dirty) {}
    ProjectId project_;
    WorldId world_;
    SceneId scene_;
    Name name_;
    ClientExportRequest request_;
    bool dirty_;
    std::vector<std::byte> scene_payload_;
    ContentDigest scene_digest_{};
    std::vector<ClientExportMesh> meshes_;
    std::vector<ClientExportSource> sources_;
    std::size_t payload_bytes_{0};
    friend Result<PreparedClientExport> prepare_client_export(const AuthoredScene&, AssetCatalog&,
                                                              AssetImports&,
                                                              const ClientExportRequest&);
};

// The active scene must be this project's declared startup scene. Every renderer
// reference, including hidden entities, is resolved; only builtin geometry and
// fresh shark.gltf artifacts are accepted. Imports must be performed explicitly
// beforehand. Returned meshes and source attributions are sorted by AssetId.
[[nodiscard]] Result<PreparedClientExport>
prepare_client_export(const AuthoredScene& owner, AssetCatalog& catalog, AssetImports& imports,
                      const ClientExportRequest& request);

// Rechecks original owner scopes/revisions, cache file, source/metadata closure
// and exact selected keys. This is an observation fence, not a filesystem lock:
// cooperating source writers must not race the final check/publication interval.
[[nodiscard]] Result<void> validate_client_export(const PreparedClientExport& candidate,
                                                  const AuthoredScene& owner, AssetCatalog& catalog,
                                                  AssetImports& imports);

} // namespace shark::tools
