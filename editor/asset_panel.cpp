#include "asset_panel.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <thread>

namespace shark::editor {
namespace {
using Json = nlohmann::json;
using namespace tools;

struct Failure {
    Error error;
};
template <typename T> T take(Result<T> result) {
    if (!result)
        throw Failure{std::move(result).error()};
    return std::move(result).value();
}
[[noreturn]] void fail(ErrorCode code, const char* message) {
    throw Failure{{code, message, {.operation = "editor.assets"}}};
}
template <typename T, typename F> Result<T> boundary(F&& function) {
    try {
        return function();
    } catch (Failure& failure) {
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Asset operation could not complete; reconcile any uncertain publication or "
                     "acknowledgement",
                     {.operation = "editor.assets"}};
    }
}
Error error_value(const Json& value) {
    constexpr std::array codes{
        ErrorCode::invalid_argument,    ErrorCode::invalid_configuration,  ErrorCode::invalid_id,
        ErrorCode::invalid_path,        ErrorCode::path_outside_root,      ErrorCode::io_failure,
        ErrorCode::entropy_unavailable, ErrorCode::capability_unavailable, ErrorCode::conflict,
        ErrorCode::execution_failed};
    const auto& name = value.at("code").get_ref<const std::string&>();
    const auto found = std::find_if(codes.begin(), codes.end(),
                                    [&](auto code) { return error_code_name(code) == name; });
    if (found == codes.end())
        fail(ErrorCode::execution_failed, "Asset reply contains an unknown error code");
    const auto& context = value.at("context");
    return {*found,
            value.at("message").get<std::string>(),
            {context.value("operation", std::string{}), context.value("request_id", std::string{}),
             context.value("path", std::string{}), context.value("entity_id", std::string{}),
             context.value("asset_id", std::string{})}};
}
std::uint64_t decimal(const Json& value) {
    const auto& text = value.get_ref<const std::string&>();
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        fail(ErrorCode::execution_failed, "Asset reply contains an invalid counter");
    return result;
}
template <typename T> T revision_value(const Json& value) {
    return {take(StableId::parse(value.at("scope").get_ref<const std::string&>())),
            decimal(value.at("value"))};
}
template <typename T> Json revision_json(const T& value) {
    return {{"scope", value.scope.to_string()}, {"value", std::to_string(value.value)}};
}
AssetId asset_value(const Json& value) {
    return AssetId{take(StableId::parse(value.get_ref<const std::string&>()))};
}
AssetCacheKey key_value(const Json& value) {
    return take(ContentDigest::parse(value.get_ref<const std::string&>()));
}
std::optional<AssetCacheKey> optional_key(const Json& value) {
    return value.is_null() ? std::optional<AssetCacheKey>{} : key_value(value);
}
AssetAudience audience(const Json& value) {
    if (value == "client")
        return AssetAudience::client;
    if (value == "server")
        return AssetAudience::server;
    if (value == "shared")
        return AssetAudience::shared;
    fail(ErrorCode::execution_failed, "Asset reply contains an unknown audience");
}
AssetTargetProfile target_value(const Json& value) {
    return {value.at("id").get<std::string>(), audience(value.at("side"))};
}
Json target_json(const AssetTargetProfile& target) {
    if (target.id.empty() || target.id.size() > 64 ||
        (target.side != AssetAudience::client && target.side != AssetAudience::server))
        fail(ErrorCode::invalid_argument,
             "Import target requires a bounded ID and client/server side");
    return {{"id", target.id},
            {"side", target.side == AssetAudience::client ? "client" : "server"}};
}
AssetImportJobId job_id(const Json& value) {
    return {take(StableId::parse(value.at("scope").get_ref<const std::string&>())),
            decimal(value.at("sequence"))};
}
Json job_json(AssetImportJobId id) {
    return {{"scope", id.scope.to_string()}, {"sequence", std::to_string(id.sequence)}};
}
AssetPublication publication(const Json& value) {
    const auto name = value.at("durability").get<std::string>();
    FileDurability durability;
    if (name == "file_flushed")
        durability = FileDurability::file_flushed;
    else if (name == "file_and_directory_flushed")
        durability = FileDurability::file_and_directory_flushed;
    else if (name == "directory_flush_failed")
        durability = FileDurability::directory_flush_failed;
    else
        fail(ErrorCode::execution_failed, "Asset reply contains an unknown publication outcome");
    if (!value.at("published").get<bool>())
        fail(ErrorCode::execution_failed, "Asset publication reply did not report publication");
    return {true, value.at("bytes").get<std::size_t>(), durability};
}
AssetImportSummary summary_value(const Json& value) {
    return {revision_value<AssetCacheRevision>(value.at("cache_revision")),
            revision_value<AssetCatalogRevision>(value.at("catalog_revision")),
            asset_value(value.at("root")),
            target_value(value.at("target")),
            key_value(value.at("root_key")),
            value.at("nodes").get<std::size_t>(),
            value.at("imported").get<std::size_t>(),
            value.at("reused").get<std::size_t>()};
}
AssetJobStatus job_value(const Json& value) {
    AssetImportJobState state;
    const auto name = value.at("state").get<std::string>();
    if (name == "queued")
        state = AssetImportJobState::queued;
    else if (name == "running")
        state = AssetImportJobState::running;
    else if (name == "ready")
        state = AssetImportJobState::ready;
    else if (name == "succeeded")
        state = AssetImportJobState::succeeded;
    else if (name == "failed")
        state = AssetImportJobState::failed;
    else if (name == "cancelled")
        state = AssetImportJobState::cancelled;
    else
        fail(ErrorCode::execution_failed, "Asset reply contains an unknown job state");
    AssetJobStatus result{job_id(value.at("job_id")),
                          state,
                          value.at("completed_nodes").get<std::size_t>(),
                          value.at("total_nodes").get<std::size_t>(),
                          {},
                          {},
                          {}};
    if (result.completed_nodes > result.total_nodes ||
        result.total_nodes > asset_import_maximum_nodes)
        fail(ErrorCode::execution_failed, "Asset reply exceeds job progress bounds");
    if (!value.at("summary").is_null())
        result.summary = summary_value(value.at("summary"));
    if (!value.at("commit").is_null()) {
        const auto& commit = value.at("commit");
        result.commit = AssetJobCommit{revision_value<AssetCacheRevision>(commit.at("revision")),
                                       commit.at("changed").get<bool>(),
                                       {}};
        if (!commit.at("publication").is_null())
            result.commit->publication = publication(commit.at("publication"));
    }
    if (!value.at("error").is_null())
        result.error = error_value(value.at("error"));
    if ((state == AssetImportJobState::succeeded &&
         (!result.summary || !result.commit || result.error)) ||
        (state == AssetImportJobState::failed && (!result.error || result.commit)))
        fail(ErrorCode::execution_failed, "Asset reply contains an inconsistent terminal outcome");
    return result;
}
std::vector<AssetJobStatus> jobs_value(const Json& values) {
    if (!values.is_array() || values.size() > asset_import_maximum_jobs)
        fail(ErrorCode::execution_failed, "Asset reply exceeds four retained jobs");
    std::vector<AssetJobStatus> result;
    result.reserve(values.size());
    for (const auto& value : values)
        result.push_back(job_value(value));
    return result;
}
AssetBrowserPage page_value(const Json& value, AssetCatalogRevision expected, std::size_t offset) {
    AssetBrowserPage page{revision_value<AssetCatalogRevision>(value.at("revision")),
                          offset,
                          value.at("total").get<std::size_t>(),
                          {},
                          {}};
    const auto& records = value.at("records");
    if (page.revision != expected || page.total > asset_maximum_catalog_entries ||
        offset > page.total || !records.is_array() || records.size() > asset_panel_page_size ||
        records.size() > page.total - offset)
        fail(ErrorCode::execution_failed, "Asset reply contains an inconsistent catalog page");
    for (const auto& record : records) {
        auto metadata = take(decode_asset_metadata(record.at("metadata").dump()));
        auto source = record.at("source").get<std::string>();
        auto sidecar = record.at("sidecar").get<std::string>();
        const auto bytes = record.at("metadata_bytes").get<std::size_t>();
        if (source.size() > 4096 || sidecar.size() > 4096 || bytes > asset_metadata_maximum_bytes ||
            (!page.records.empty() && !(page.records.back().metadata.asset < metadata.asset)))
            fail(ErrorCode::execution_failed, "Asset reply exceeds catalog record bounds");
        page.records.push_back({std::move(metadata), std::move(source), std::move(sidecar), bytes,
                                record.at("source_present").get<bool>()});
    }
    if (!value.at("next_offset").is_null()) {
        page.next_offset = value.at("next_offset").get<std::size_t>();
        if (*page.next_offset != offset + records.size() || *page.next_offset <= offset ||
            *page.next_offset >= page.total)
            fail(ErrorCode::execution_failed, "Asset reply contains an invalid next page");
    } else if (offset + records.size() != page.total) {
        fail(ErrorCode::execution_failed, "Asset reply omitted remaining catalog records");
    }
    return page;
}
} // namespace

struct AssetPanel::Impl {
    ProjectSession& session;
    const std::thread::id thread{std::this_thread::get_id()};
    const std::uint64_t incarnation;
    std::optional<AssetCatalogRevision> catalog;
    std::optional<AssetBrowserPage> page;
    std::optional<AssetId> selected;
    std::optional<AssetCacheUsage> cache;
    std::vector<AssetImporterInfo> importers;
    std::vector<AssetJobStatus> jobs;
    std::optional<AssetMetadataObservation> metadata;
    std::optional<std::string> metadata_token;

    explicit Impl(ProjectSession& value)
        : session(value), incarnation(value.state().revision.scene_instance) {}
    void check(bool fresh = true) const {
        if (thread != std::this_thread::get_id())
            fail(ErrorCode::conflict, "Asset panel requires its creating thread");
        if (!session.state().open || session.state().revision.scene_instance != incarnation ||
            (fresh && !session.state_current()))
            fail(ErrorCode::conflict,
                 "Asset panel belongs to an old or unobserved project session");
    }
    Json call(std::string_view method, const Json& parameters = Json::object()) {
        auto reply = take(session.request(method, parameters.dump()));
        auto document = Json::parse(reply.json);
        if (document.contains("error")) {
            auto value = document.at("error").at("data");
            value["message"] = document.at("error").at("message");
            throw Failure{error_value(value)};
        }
        // A failed job can be carried by a successful method response. Decode
        // the job's terminal state rather than treating reply.failed as rollback.
        return std::move(document.at("result"));
    }
    void require_revisions(bool selected_required = false) const {
        check();
        if (!catalog || !cache || (selected_required && !selected))
            fail(ErrorCode::conflict, "Observe the asset catalog/cache and select an asset first");
    }
    void release_metadata() {
        check(false);
        if (metadata_token) {
            const auto result = call("asset.metadata.release", {{"revision", *metadata_token}});
            if (!result.at("released").get<bool>())
                fail(ErrorCode::execution_failed, "Asset metadata token was not released");
        }
        metadata_token.reset();
        metadata.reset();
    }
};

AssetPanel::AssetPanel(ProjectSession& session) : impl_(std::make_unique<Impl>(session)) {}
AssetPanel::~AssetPanel() {
    try {
        if (impl_->metadata_token)
            impl_->release_metadata();
    } catch (...) {
        // ProjectSession destruction also expires all outstanding metadata tokens.
    }
}
Result<void> AssetPanel::refresh_catalog() {
    return boundary<void>([&]() -> Result<void> {
        impl_->check();
        const auto status = impl_->call("asset.catalog.status");
        const auto revision = revision_value<AssetCatalogRevision>(status.at("revision"));
        auto page = page_value(
            impl_->call("asset.catalog.query", {{"expected_revision", revision_json(revision)},
                                                {"offset", 0},
                                                {"page_size", asset_panel_page_size}}),
            revision, 0);
        impl_->catalog = revision;
        impl_->page = std::move(page);
        impl_->selected.reset();
        return {};
    });
}
Result<void> AssetPanel::refresh_inventory(std::span<const std::string_view> paths) {
    return boundary<void>([&]() -> Result<void> {
        impl_->check();
        if (!impl_->catalog)
            fail(ErrorCode::conflict, "Observe the catalog before replacing its inventory");
        if (paths.size() > asset_maximum_catalog_entries)
            fail(ErrorCode::invalid_argument, "Asset inventory exceeds 4096 paths");
        std::size_t bytes = 0;
        for (const auto path : paths) {
            if (path.size() > 4096 || path.size() > asset_panel_inventory_bytes - bytes)
                fail(ErrorCode::invalid_argument,
                     "Asset inventory exceeds the editor's bounded input");
            bytes += path.size();
        }
        Json paths_json = Json::array();
        for (const auto path : paths)
            paths_json.push_back(path);
        auto result = impl_->call("asset.catalog.refresh",
                                  {{"expected_revision", revision_json(*impl_->catalog)},
                                   {"sidecars", std::move(paths_json)}});
        impl_->catalog = revision_value<AssetCatalogRevision>(result.at("revision"));
        impl_->page.reset();
        impl_->selected.reset();
        return {};
    });
}
Result<void> AssetPanel::query_page(std::size_t offset) {
    return boundary<void>([&]() -> Result<void> {
        impl_->check();
        if (!impl_->catalog || offset > asset_maximum_catalog_entries)
            fail(ErrorCode::conflict, "Observe the catalog before paging");
        auto page = page_value(impl_->call("asset.catalog.query",
                                           {{"expected_revision", revision_json(*impl_->catalog)},
                                            {"offset", offset},
                                            {"page_size", asset_panel_page_size}}),
                               *impl_->catalog, offset);
        impl_->page = std::move(page);
        impl_->selected.reset();
        return {};
    });
}
const std::optional<AssetBrowserPage>& AssetPanel::page() const noexcept {
    return impl_->page;
}
Result<void> AssetPanel::select(AssetId id) {
    return boundary<void>([&]() -> Result<void> {
        impl_->check();
        if (!impl_->page ||
            std::none_of(impl_->page->records.begin(), impl_->page->records.end(),
                         [&](const auto& record) { return record.metadata.asset == id; }))
            fail(ErrorCode::conflict, "Asset is absent from the observed page");
        impl_->selected = id;
        return {};
    });
}
const std::optional<AssetId>& AssetPanel::selection() const noexcept {
    return impl_->selected;
}
Result<void> AssetPanel::refresh_importers() {
    return boundary<void>([&]() -> Result<void> {
        impl_->check();
        const auto result = impl_->call("asset.importers");
        const auto& values = result.at("importers");
        if (!values.is_array() || values.size() > 32)
            fail(ErrorCode::execution_failed, "Importer discovery exceeds editor bounds");
        std::vector<AssetImporterInfo> candidate;
        for (const auto& value : values)
            candidate.push_back({value.at("id").get<std::string>(),
                                 value.at("version").get<std::uint32_t>(),
                                 value.at("kind").get<std::string>(),
                                 value.at("payload_version").get<std::uint32_t>(),
                                 value.at("settings_schema").dump()});
        impl_->importers = std::move(candidate);
        return {};
    });
}
std::span<const AssetImporterInfo> AssetPanel::importers() const noexcept {
    return impl_->importers;
}
Result<void> AssetPanel::refresh_cache() {
    return boundary<void>([&]() -> Result<void> {
        impl_->check();
        const auto value = impl_->call("asset.cache.status");
        impl_->cache = AssetCacheUsage{revision_value<AssetCacheRevision>(value.at("revision")),
                                       value.at("artifacts").get<std::size_t>(),
                                       value.at("bindings").get<std::size_t>(),
                                       value.at("payload_bytes").get<std::size_t>(),
                                       value.at("file_bytes").get<std::size_t>(),
                                       value.at("unpublished_stage").get<bool>()};
        return {};
    });
}
const std::optional<AssetCacheUsage>& AssetPanel::cache() const noexcept {
    return impl_->cache;
}
Result<AssetCacheStatus> AssetPanel::query_selected_cache(const AssetTargetProfile& target) {
    return boundary<AssetCacheStatus>([&] {
        impl_->require_revisions(true);
        const auto value = impl_->call("asset.cache.query",
                                       {{"asset_id", impl_->selected->value().to_string()},
                                        {"target", target_json(target)},
                                        {"expected_catalog", revision_json(*impl_->catalog)},
                                        {"expected_cache", revision_json(impl_->cache->revision)}});
        AssetCacheState state;
        if (value.at("state") == "fresh")
            state = AssetCacheState::fresh;
        else if (value.at("state") == "missing")
            state = AssetCacheState::missing;
        else if (value.at("state") == "stale")
            state = AssetCacheState::stale;
        else
            fail(ErrorCode::execution_failed, "Asset reply contains an unknown cache state");
        AssetCacheStatus result{revision_value<AssetCacheRevision>(value.at("revision")),
                                asset_value(value.at("asset_id")),
                                target_value(value.at("target")),
                                state,
                                optional_key(value.at("required_key")),
                                optional_key(value.at("last_valid_key")),
                                {}};
        if (!value.at("blocked_by").is_null())
            result.blocked_by = error_value(value.at("blocked_by"));
        return result;
    });
}
Result<AssetArtifactInfo> AssetPanel::inspect_artifact(AssetCacheKey key) {
    return boundary<AssetArtifactInfo>([&] {
        impl_->require_revisions();
        const auto value = impl_->call(
            "asset.cache.inspect",
            {{"key", key.to_hex()}, {"expected_revision", revision_json(impl_->cache->revision)}});
        AssetArtifactInfo result{asset_value(value.at("asset_id")),
                                 key_value(value.at("key")),
                                 value.at("kind").get<std::string>(),
                                 audience(value.at("audience")),
                                 target_value(value.at("target")),
                                 value.at("importer").get<std::string>(),
                                 value.at("importer_version").get<std::uint32_t>(),
                                 value.at("payload_version").get<std::uint32_t>(),
                                 key_value(value.at("source_digest")),
                                 key_value(value.at("settings_digest")),
                                 key_value(value.at("payload_digest")),
                                 {},
                                 value.at("payload_bytes").get<std::size_t>()};
        const auto& dependencies = value.at("dependencies");
        if (dependencies.size() > asset_import_maximum_nodes ||
            result.payload_bytes > asset_import_maximum_payload_bytes)
            fail(ErrorCode::execution_failed, "Artifact inspection exceeds native bounds");
        for (const auto& dependency : dependencies)
            result.dependencies.push_back(
                {asset_value(dependency.at("asset_id")), key_value(dependency.at("key"))});
        return result;
    });
}
Result<AssetImportJobAdmission> AssetPanel::submit_selected(const AssetTargetProfile& target) {
    return boundary<AssetImportJobAdmission>([&] {
        impl_->require_revisions(true);
        const auto value = impl_->call("asset.jobs.submit",
                                       {{"asset_id", impl_->selected->value().to_string()},
                                        {"target", target_json(target)},
                                        {"expected_catalog", revision_json(*impl_->catalog)},
                                        {"expected_cache", revision_json(impl_->cache->revision)}});
        return AssetImportJobAdmission{job_id(value.at("job_id")),
                                       value.at("deduplicated").get<bool>()};
    });
}
Result<void> AssetPanel::refresh_jobs() {
    return boundary<void>([&]() -> Result<void> {
        // Listing is read-only reconciliation, like cancel/take, and remains
        // available while a project-status observation needs recovery.
        impl_->check(false);
        auto candidate = jobs_value(impl_->call("asset.jobs.list").at("jobs"));
        impl_->jobs = std::move(candidate);
        return {};
    });
}
std::span<const AssetJobStatus> AssetPanel::jobs() const noexcept {
    return impl_->jobs;
}
Result<AssetJobStatus> AssetPanel::cancel(AssetImportJobId id) {
    return boundary<AssetJobStatus>([&] {
        impl_->check(false);
        auto result = job_value(impl_->call("asset.jobs.cancel", {{"job_id", job_json(id)}}));
        return result;
    });
}
Result<AssetDrainReceipt> AssetPanel::drain() {
    return boundary<AssetDrainReceipt>([&] {
        impl_->check();
        const auto value = impl_->call("asset.jobs.drain");
        AssetDrainReceipt receipt{value.at("drained").get<std::size_t>(),
                                  jobs_value(value.at("jobs"))};
        if (receipt.drained > 1)
            fail(ErrorCode::execution_failed, "Job drain exceeded one completion");
        return receipt;
    });
}
Result<AssetJobStatus> AssetPanel::take_job(AssetImportJobId id) {
    return boundary<AssetJobStatus>([&] {
        impl_->check(false);
        auto result = job_value(impl_->call("asset.jobs.take", {{"job_id", job_json(id)}}));
        std::erase_if(impl_->jobs, [&](const auto& job) { return job.id == id; });
        return result;
    });
}
Result<void> AssetPanel::read_metadata(std::string_view path) {
    return boundary<void>([&]() -> Result<void> {
        impl_->check();
        if (path.empty() || path.size() > 4096)
            fail(ErrorCode::invalid_argument, "Metadata path must contain 1..4096 bytes");
        impl_->release_metadata();
        const auto value = impl_->call("asset.metadata.read", {{"path", path}});
        auto token = value.at("revision").get<std::string>();
        AssetMetadataObservation observation{value.at("path").get<std::string>(),
                                             value.at("metadata").dump(2)};
        impl_->metadata_token = std::move(token);
        impl_->metadata = std::move(observation);
        return {};
    });
}
const std::optional<AssetMetadataObservation>& AssetPanel::metadata() const noexcept {
    return impl_->metadata;
}
Result<AssetMetadataReceipt> AssetPanel::write_metadata(std::string_view path,
                                                        std::string_view json, bool create_only) {
    return boundary<AssetMetadataReceipt>([&] {
        impl_->check();
        if (path.empty() || path.size() > 4096 || json.size() > asset_metadata_maximum_bytes)
            fail(ErrorCode::invalid_argument, "Metadata edit exceeds its bounded input");
        if (create_only && impl_->metadata_token)
            fail(ErrorCode::conflict, "Release the observed metadata before a create-only write");
        if (!create_only &&
            (!impl_->metadata_token || !impl_->metadata || impl_->metadata->path != path))
            fail(ErrorCode::conflict, "Read this exact metadata path before replacement");
        auto encoded = take(encode_asset_metadata(take(decode_asset_metadata(json))));
        AssetMetadataObservation observation{std::string{path}, encoded};
        const auto value = impl_->call(
            "asset.metadata.write",
            {{"path", path},
             {"metadata", Json::parse(encoded)},
             {"expected_revision", create_only ? Json(nullptr) : Json(*impl_->metadata_token)}});
        auto token = value.at("revision").get<std::string>();
        AssetMetadataReceipt receipt{value.at("path").get<std::string>(),
                                     publication(value.at("publication"))};
        impl_->metadata_token = std::move(token);
        impl_->metadata = std::move(observation);
        return receipt;
    });
}
Result<void> AssetPanel::release_metadata() {
    return boundary<void>([&]() -> Result<void> {
        impl_->release_metadata();
        return {};
    });
}

} // namespace shark::editor
