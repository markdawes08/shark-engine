#pragma once

#include "shark/tools/asset_import.hpp"

#include <nlohmann/json.hpp>

#include <initializer_list>
#include <string_view>

namespace shark::tools::authoring_asset_wire {

// Private protocol conversion only. Native metadata/catalog/import validators
// remain authoritative; no file or mutable service is accessed here.
[[nodiscard]] Result<void> require_fields(const nlohmann::json& value,
                                          std::initializer_list<std::string_view> allowed);
[[nodiscard]] Result<std::size_t> parse_index(const nlohmann::json& value, std::size_t maximum,
                                              std::string_view path);
[[nodiscard]] Result<AssetId> parse_asset_id(const nlohmann::json& value,
                                             std::string_view path = "/params/asset_id");
[[nodiscard]] Result<AssetMetadata> parse_metadata(const nlohmann::json& value);
[[nodiscard]] Result<AssetCatalogRevision>
parse_catalog_revision(const nlohmann::json& value,
                       std::string_view path = "/params/expected_revision");
[[nodiscard]] Result<AssetCacheRevision>
parse_cache_revision(const nlohmann::json& value,
                     std::string_view path = "/params/expected_revision");
[[nodiscard]] Result<AssetImportJobId> parse_job_id(const nlohmann::json& value);
[[nodiscard]] Result<AssetTargetProfile> parse_target(const nlohmann::json& value);
[[nodiscard]] Result<AssetCacheKey> parse_key(const nlohmann::json& value);
[[nodiscard]] Result<std::vector<ProjectPath>> parse_sidecars(const nlohmann::json& value);

[[nodiscard]] nlohmann::json revision_json(AssetCatalogRevision value);
[[nodiscard]] nlohmann::json revision_json(AssetCacheRevision value);
[[nodiscard]] nlohmann::json job_id_json(AssetImportJobId value);
[[nodiscard]] nlohmann::json error_json(const Error& value);
[[nodiscard]] nlohmann::json publication_json(const FileCommit& value);
[[nodiscard]] nlohmann::json metadata_json(const AssetMetadata& value);
[[nodiscard]] nlohmann::json record_json(const AssetRecord& value);
[[nodiscard]] nlohmann::json page_json(const AssetCatalogPage& value);
[[nodiscard]] nlohmann::json resolution_json(const AssetResolution& value);
[[nodiscard]] nlohmann::json importers_json(const std::vector<AssetImporterInfo>& value);
[[nodiscard]] nlohmann::json cache_status_json(const AssetCacheStatus& value);
[[nodiscard]] nlohmann::json usage_json(const AssetCacheUsage& value);
[[nodiscard]] nlohmann::json commit_json(const AssetCacheCommit& value);
// Descriptor and independently hashed byte count, never an inline binary payload.
[[nodiscard]] nlohmann::json artifact_json(const AssetArtifact& value);
[[nodiscard]] nlohmann::json job_json(const AssetImportJobStatus& value);
[[nodiscard]] nlohmann::json jobs_json(const std::vector<AssetImportJobStatus>& value);
[[nodiscard]] nlohmann::json admission_json(const AssetImportJobAdmission& value);
[[nodiscard]] nlohmann::json methods_json();

} // namespace shark::tools::authoring_asset_wire
