#pragma once

#include "shark/content_digest.hpp"
#include "shark/tools/asset_catalog.hpp"

namespace shark::tools {

inline constexpr std::size_t asset_import_maximum_nodes = 128;
inline constexpr std::size_t asset_import_maximum_source_bytes = 1024 * 1024;
inline constexpr std::size_t asset_import_maximum_payload_bytes = 2 * 1024 * 1024;
inline constexpr std::size_t asset_import_maximum_total_bytes = 16 * 1024 * 1024;
inline constexpr std::size_t asset_cache_maximum_entries = 128;
inline constexpr std::size_t asset_cache_maximum_index_bytes = 4 * 1024 * 1024;
inline constexpr std::size_t asset_cache_maximum_file_bytes = 24 * 1024 * 1024;
inline constexpr std::string_view asset_cache_filename = asset_development_cache_name;

using AssetCacheKey = ContentDigest;
struct AssetTargetProfile {
    std::string id;
    AssetAudience side;
    bool operator==(const AssetTargetProfile&) const = default;
};
struct AssetImporterInfo {
    std::string id;
    std::uint32_t version;
    std::string kind;
    std::uint32_t payload_version;
    std::string settings_schema_json;
    bool operator==(const AssetImporterInfo&) const = default;
};
struct AssetArtifactDependency {
    AssetId asset;
    AssetCacheKey key;
    bool operator==(const AssetArtifactDependency&) const = default;
};
struct AssetArtifact {
    AssetId asset;
    AssetCacheKey key;
    std::string kind;
    AssetAudience audience;
    AssetTargetProfile target;
    std::string importer;
    std::uint32_t importer_version;
    std::uint32_t payload_version;
    ContentDigest source_digest;
    ContentDigest settings_digest;
    std::vector<AssetArtifactDependency> dependencies;
    ContentDigest payload_digest;
    std::vector<std::byte> payload;
    bool operator==(const AssetArtifact&) const = default;
};
struct AssetImportDependency {
    AssetId asset;
    AssetCacheKey key;
    std::span<const std::byte> payload;
};
struct AssetImportInput {
    AssetId asset;
    std::string_view kind;
    std::span<const std::byte> source;
    std::string_view settings_json;
    const AssetTargetProfile& target;
    std::span<const AssetImportDependency> dependencies;
};

// Trusted native CPU importer, not a dynamically loaded plugin. References must
// outlive the service. Callbacks are bounded/deterministic and publish nothing;
// their descriptor/version is immutable for a service's lifetime. No path, scene
// or project writer is passed to callbacks. Exceptions become owned failures.
class AssetImporter {
  public:
    virtual ~AssetImporter() = default;
    [[nodiscard]] virtual const AssetImporterInfo& info() const noexcept = 0;
    [[nodiscard]] virtual Result<std::string> normalize_settings(std::string_view json) const = 0;
    [[nodiscard]] virtual Result<std::vector<std::byte>> import(const AssetImportInput& input) = 0;
    [[nodiscard]] virtual Result<void> validate_payload(const AssetArtifact& artifact) const = 0;
};

// A real bounded engine-owned byte payload spine. It supports identity and
// uppercase-ascii transforms; no glTF/image/audio decoding is implied.
class ByteAssetImporter final : public AssetImporter {
  public:
    [[nodiscard]] const AssetImporterInfo& info() const noexcept override;
    [[nodiscard]] Result<std::string> normalize_settings(std::string_view json) const override;
    [[nodiscard]] Result<std::vector<std::byte>> import(const AssetImportInput& input) override;
    [[nodiscard]] Result<void> validate_payload(const AssetArtifact& artifact) const override;
};

struct AssetCacheRevision {
    StableId scope;
    std::uint64_t value;
    bool operator==(const AssetCacheRevision&) const = default;
};
enum class AssetCacheState { missing, fresh, stale };
struct AssetCacheStatus {
    AssetCacheRevision revision;
    AssetId asset;
    AssetTargetProfile target;
    AssetCacheState state;
    std::optional<AssetCacheKey> required_key;
    std::optional<AssetCacheKey> last_valid_key;
    // A blocked current input does not erase a retained last-valid artifact.
    std::optional<Error> blocked_by;
};
struct AssetCacheUsage {
    AssetCacheRevision revision;
    std::size_t artifacts;
    std::size_t bindings;
    std::size_t payload_bytes;
    std::size_t file_bytes;
    bool unpublished_stage;
};
struct AssetImportSummary {
    AssetCacheRevision cache_revision;
    AssetCatalogRevision catalog_revision;
    AssetId root;
    AssetTargetProfile target;
    AssetCacheKey root_key;
    std::size_t nodes;
    std::size_t imported;
    std::size_t reused;
};
struct AssetCacheCommit {
    AssetCacheRevision revision;
    bool changed;
    std::optional<FileCommit> file;
};

class PreparedAssetImport final {
  public:
    ~PreparedAssetImport();
    PreparedAssetImport(PreparedAssetImport&&) noexcept;
    PreparedAssetImport& operator=(PreparedAssetImport&&) noexcept;
    PreparedAssetImport(const PreparedAssetImport&) = delete;
    PreparedAssetImport& operator=(const PreparedAssetImport&) = delete;
    [[nodiscard]] Result<AssetImportSummary> summary() const;

  private:
    struct Impl;
    explicit PreparedAssetImport(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
    friend class AssetImports;
};

// Creating-thread offline authoring service. ProjectFiles, catalog and importers
// must outlive it. Preparation is synchronous/pure with respect to publication;
// 11c supplies bounded workers before later UI/CLI adapters. Runtime/Server never
// link this service. Cache snapshots/retries use explicit revisions.
class AssetImports final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<AssetImports>>
    open(ProjectFiles& files, AssetCatalog& catalog, ProjectId project,
         std::span<AssetImporter* const> importers);
    ~AssetImports();
    AssetImports(const AssetImports&) = delete;
    AssetImports& operator=(const AssetImports&) = delete;
    [[nodiscard]] Result<std::vector<AssetImporterInfo>> importers() const;
    [[nodiscard]] Result<AssetCacheRevision> revision() const;
    [[nodiscard]] Result<PreparedAssetImport> prepare(AssetId root,
                                                      const AssetTargetProfile& target,
                                                      AssetCatalogRevision expected_catalog,
                                                      AssetCacheRevision expected_cache);
    [[nodiscard]] Result<AssetCacheCommit> publish(PreparedAssetImport&& prepared,
                                                   AssetCatalogRevision expected_catalog,
                                                   AssetCacheRevision expected_cache);
    [[nodiscard]] Result<AssetCacheStatus> query(AssetId asset, const AssetTargetProfile& target,
                                                 AssetCatalogRevision expected_catalog,
                                                 AssetCacheRevision expected_cache);
    [[nodiscard]] Result<AssetArtifact> read(AssetCacheKey key, AssetCacheRevision expected) const;
    [[nodiscard]] Result<AssetCacheUsage> usage(AssetCacheRevision expected) const;
    [[nodiscard]] Result<AssetCacheRevision> reload(AssetCacheRevision expected);
    // Prune retains all last-valid binding closures. Clear explicitly discards
    // the disposable cache; neither operation changes authored source/metadata.
    [[nodiscard]] Result<AssetCacheCommit> prune(AssetCacheRevision expected);
    [[nodiscard]] Result<AssetCacheCommit> clear(AssetCacheRevision expected);
    // Exactly the reserved unpublished cache stage; never source-save stages.
    [[nodiscard]] static Result<std::size_t> recover(ProjectFiles& files);

  private:
    struct Impl;
    explicit AssetImports(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::tools
