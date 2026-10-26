#pragma once

#include "shark/tools/project_files.hpp"

#include <span>

namespace shark::tools {

struct AssetCatalogRevision {
    StableId scope;
    std::uint64_t value;
    bool operator==(const AssetCatalogRevision&) const = default;
};

struct AssetRecord {
    AssetMetadata metadata;
    ProjectPath source;
    ProjectPath sidecar;
    FileRevision metadata_revision;
    bool source_present;
    bool operator==(const AssetRecord&) const = default;
};

enum class AssetAvailability { unknown_id, source_missing, source_present };
struct AssetResolution {
    AssetCatalogRevision revision;
    AssetId asset;
    AssetAvailability availability;
    std::optional<AssetRecord> record;
};

struct AssetCatalogPage {
    AssetCatalogRevision revision;
    std::size_t total;
    std::vector<AssetRecord> records;
    std::optional<std::size_t> next_offset;
};

// Creating-thread, derived catalog. ProjectFiles must outlive this owner. The
// caller supplies the complete bounded sidecar inventory, not an implicit scan
// or watcher. Refresh validates a whole candidate before one in-memory swap.
class AssetCatalog final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<AssetCatalog>> create(ProjectFiles& files,
                                                                      ProjectId project);
    ~AssetCatalog();
    AssetCatalog(const AssetCatalog&) = delete;
    AssetCatalog& operator=(const AssetCatalog&) = delete;
    [[nodiscard]] Result<AssetCatalogRevision> revision() const;
    [[nodiscard]] Result<AssetCatalogRevision> refresh(std::span<const ProjectPath> sidecars,
                                                       AssetCatalogRevision expected);
    [[nodiscard]] Result<AssetResolution> resolve(AssetId asset,
                                                  AssetCatalogRevision expected) const;
    [[nodiscard]] Result<AssetCatalogPage>
    query(AssetCatalogRevision expected, std::size_t offset = 0, std::size_t page_size = 64) const;

  private:
    struct Impl;
    explicit AssetCatalog(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::tools
