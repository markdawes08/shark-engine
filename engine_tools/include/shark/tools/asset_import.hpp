#pragma once

#include "shark/content_digest.hpp"
#include "shark/tools/asset_catalog.hpp"

#include <stop_token>

namespace shark::tools {

inline constexpr std::size_t asset_import_maximum_nodes = 128;
inline constexpr std::size_t asset_import_maximum_source_bytes = 1024 * 1024;
inline constexpr std::size_t asset_import_maximum_payload_bytes = 2 * 1024 * 1024;
inline constexpr std::size_t asset_import_maximum_total_bytes = 16 * 1024 * 1024;
inline constexpr std::size_t asset_cache_maximum_entries = 128;
inline constexpr std::size_t asset_cache_maximum_index_bytes = 4 * 1024 * 1024;
inline constexpr std::size_t asset_cache_maximum_file_bytes = 24 * 1024 * 1024;
inline constexpr std::string_view asset_cache_filename = asset_development_cache_name;
inline constexpr std::size_t asset_import_maximum_jobs = 4;
inline constexpr std::size_t asset_import_jobs_maximum_source_bytes = 32 * 1024 * 1024;
inline constexpr std::size_t asset_import_jobs_maximum_cached_bytes = 32 * 1024 * 1024;

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
    // Descriptor and source digest from the validated captured child artifact.
    // Consumers must not interpret an arbitrary transformed payload as raw source.
    std::string_view kind{};
    std::string_view importer{};
    std::uint32_t importer_version{};
    std::uint32_t payload_version{};
    ContentDigest source_digest{};
};
struct AssetImportInput {
    AssetId asset;
    std::string_view kind;
    std::span<const std::byte> source;
    std::string_view settings_json;
    const AssetTargetProfile& target;
    std::span<const AssetImportDependency> dependencies;
    // Cooperative only. The owner also fences publication after cancellation.
    std::stop_token stop{};
};

// Trusted native CPU importer, not a dynamically loaded plugin. References must
// outlive the service and be exclusively borrowed by it. Callbacks cannot reenter
// owner services, are bounded/deterministic and publish nothing;
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

struct AssetImportJobId {
    StableId scope;
    std::uint64_t sequence;
    bool operator==(const AssetImportJobId&) const = default;
};
enum class AssetImportJobState { queued, running, ready, succeeded, failed, cancelled };
struct AssetImportJobStatus {
    AssetImportJobId id;
    AssetImportJobState state;
    std::size_t completed_nodes;
    std::size_t total_nodes;
    std::optional<AssetImportSummary> summary;
    std::optional<AssetCacheCommit> commit;
    std::optional<Error> error;
};
struct AssetImportJobAdmission {
    AssetImportJobId id;
    bool deduplicated;
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
// Bounded jobs use one private worker before later UI/CLI adapters. Runtime/Server never
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
    // Check the borrowed project/catalog/cache owner and on-disk cache fence,
    // including for consumers with no asset roots. Does not mutate or import;
    // per-root query remains required to establish current input freshness.
    [[nodiscard]] Result<void> check_context(ProjectId expected_project,
                                             AssetCatalogRevision expected_catalog,
                                             AssetCacheRevision expected_cache) const;
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
    // Captures inputs on the owner; callbacks execute serially on one worker.
    // Retained terminal outcomes count against admission until explicitly taken.
    [[nodiscard]] Result<AssetImportJobAdmission> submit(AssetId root,
                                                         const AssetTargetProfile& target,
                                                         AssetCatalogRevision expected_catalog,
                                                         AssetCacheRevision expected_cache);
    [[nodiscard]] Result<std::vector<AssetImportJobStatus>> jobs() const;
    [[nodiscard]] Result<AssetImportJobStatus> cancel_job(AssetImportJobId id);
    // Publishes at most one ready completion through the existing revision fence,
    // then dispatches the next queued job. Never waits for a running importer.
    [[nodiscard]] Result<std::size_t> drain_jobs();
    [[nodiscard]] Result<AssetImportJobStatus> take_job(AssetImportJobId id);
    // Closes admission permanently, cancels uncommitted work and joins the worker.
    // Trusted callbacks must return/cooperate; cancellation cannot preempt C++.
    [[nodiscard]] Result<void> close_jobs();
    // Exactly the reserved unpublished cache stage; never source-save stages.
    [[nodiscard]] static Result<std::size_t> recover(ProjectFiles& files);

  private:
    struct Impl;
    explicit AssetImports(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::tools
