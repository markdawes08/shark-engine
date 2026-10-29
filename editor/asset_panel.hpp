#pragma once

#include "project_session.hpp"

#include <shark/tools/asset_import.hpp>

#include <memory>
#include <span>

namespace shark::editor {

// Eight maximum-size native metadata records fit the protocol response budget.
inline constexpr std::size_t asset_panel_page_size = 8;
inline constexpr std::size_t asset_panel_inventory_bytes = 16384;

struct AssetBrowserRecord {
    tools::AssetMetadata metadata;
    std::string source;
    std::string sidecar;
    std::size_t metadata_bytes;
    bool source_present;
};
struct AssetBrowserPage {
    tools::AssetCatalogRevision revision;
    std::size_t offset;
    std::size_t total;
    std::vector<AssetBrowserRecord> records;
    std::optional<std::size_t> next_offset;
};
struct AssetPublication {
    bool published;
    std::size_t bytes;
    tools::FileDurability durability;
};
struct AssetJobCommit {
    tools::AssetCacheRevision revision;
    bool changed;
    std::optional<AssetPublication> publication;
};
struct AssetJobStatus {
    tools::AssetImportJobId id;
    tools::AssetImportJobState state;
    std::size_t completed_nodes;
    std::size_t total_nodes;
    std::optional<tools::AssetImportSummary> summary;
    std::optional<AssetJobCommit> commit;
    std::optional<Error> error;
};
struct AssetDrainReceipt {
    std::size_t drained;
    std::vector<AssetJobStatus> jobs;
};
struct AssetArtifactInfo {
    AssetId asset;
    tools::AssetCacheKey key;
    std::string kind;
    tools::AssetAudience audience;
    tools::AssetTargetProfile target;
    std::string importer;
    std::uint32_t importer_version;
    std::uint32_t payload_version;
    ContentDigest source_digest;
    ContentDigest settings_digest;
    ContentDigest payload_digest;
    std::vector<tools::AssetArtifactDependency> dependencies;
    std::size_t payload_bytes;
};
struct AssetMetadataObservation {
    std::string path;
    std::string json;
};
struct AssetMetadataReceipt {
    std::string path;
    AssetPublication publication;
};

// Creating-thread adapter over the original ProjectSession. It owns display
// values and one exact metadata token, never another catalog/importer/worker.
// A close/reopen or scene activation invalidates this panel; construct a new one.
class AssetPanel final {
  public:
    explicit AssetPanel(ProjectSession&);
    ~AssetPanel();
    AssetPanel(const AssetPanel&) = delete;
    AssetPanel& operator=(const AssetPanel&) = delete;

    [[nodiscard]] Result<void> refresh_catalog();
    [[nodiscard]] Result<void> refresh_inventory(std::span<const std::string_view> sidecars);
    [[nodiscard]] Result<void> query_page(std::size_t offset);
    [[nodiscard]] const std::optional<AssetBrowserPage>& page() const noexcept;
    [[nodiscard]] Result<void> select(AssetId);
    [[nodiscard]] const std::optional<AssetId>& selection() const noexcept;

    [[nodiscard]] Result<void> refresh_importers();
    [[nodiscard]] std::span<const tools::AssetImporterInfo> importers() const noexcept;
    [[nodiscard]] Result<void> refresh_cache();
    [[nodiscard]] const std::optional<tools::AssetCacheUsage>& cache() const noexcept;
    [[nodiscard]] Result<tools::AssetCacheStatus>
    query_selected_cache(const tools::AssetTargetProfile&);
    [[nodiscard]] Result<AssetArtifactInfo> inspect_artifact(tools::AssetCacheKey);

    [[nodiscard]] Result<tools::AssetImportJobAdmission>
    submit_selected(const tools::AssetTargetProfile&);
    [[nodiscard]] Result<void> refresh_jobs();
    [[nodiscard]] std::span<const AssetJobStatus> jobs() const noexcept;
    // These return the original outcomes without a second fallible observation.
    // Refresh the display list explicitly after consuming the receipt.
    [[nodiscard]] Result<AssetJobStatus> cancel(tools::AssetImportJobId);
    [[nodiscard]] Result<AssetDrainReceipt> drain();
    [[nodiscard]] Result<AssetJobStatus> take_job(tools::AssetImportJobId);

    // Read releases the panel's previous token before acquiring a replacement.
    // Write uses that exact same-path token or explicit create-only null. Neither
    // operation refreshes the catalog, imports, edits the scene or joins history.
    [[nodiscard]] Result<void> read_metadata(std::string_view path);
    [[nodiscard]] const std::optional<AssetMetadataObservation>& metadata() const noexcept;
    [[nodiscard]] Result<AssetMetadataReceipt>
    write_metadata(std::string_view path, std::string_view json, bool create_only);
    [[nodiscard]] Result<void> release_metadata();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::editor
