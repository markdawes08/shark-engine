#include "authoring_asset_wire.hpp"

#include <algorithm>
#include <charconv>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace shark::tools::authoring_asset_wire {
namespace {
using Json = nlohmann::json;

struct WireFailure {
    Error error;
};

[[noreturn]] void fail(std::string_view path, const char* message) {
    throw WireFailure{Error{ErrorCode::invalid_argument, message, {.path = std::string(path)}}};
}

template <typename T> T take(Result<T> value, std::string_view path) {
    if (!value) {
        auto error = std::move(value).error();
        error.context.path = std::string(path) + error.context.path;
        throw WireFailure{std::move(error)};
    }
    return std::move(value).value();
}

template <typename T, typename Function> Result<T> boundary(Function&& function) {
    try {
        return function();
    } catch (WireFailure& failure) {
        failure.error.context.operation = "authoring.asset.decode";
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Cannot decode asset protocol value",
                     {.operation = "authoring.asset.decode"}};
    }
}

void object(const Json& value, std::initializer_list<std::string_view> keys,
            std::string_view path) {
    if (!value.is_object())
        fail(path, "Expected an object");
    for (const auto& item : value.items()) {
        if (std::find(keys.begin(), keys.end(), item.key()) == keys.end())
            fail(path, "Unknown parameter field");
    }
}

const Json& required(const Json& value, const char* key, std::string_view path) {
    const auto found = value.find(key);
    if (found == value.end())
        fail(std::string(path) + '/' + key, "Required field is missing");
    return *found;
}

const std::string& text(const Json& value, std::string_view path) {
    if (!value.is_string())
        fail(path, "Expected a string");
    return value.get_ref<const std::string&>();
}

std::uint64_t decimal(const Json& value, std::string_view path) {
    const auto& input = text(value, path);
    if (input.empty() || input.size() > 20 || (input.size() > 1 && input.front() == '0') ||
        !std::all_of(input.begin(), input.end(), [](char c) { return c >= '0' && c <= '9'; }))
        fail(path, "Expected a canonical unsigned 64-bit decimal string");
    std::uint64_t result{};
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size())
        fail(path, "Decimal string exceeds the unsigned 64-bit range");
    return result;
}

StableId identity(const Json& value, std::string_view path) {
    return take(StableId::parse(text(value, path)), path);
}

template <typename Revision> Revision revision(const Json& value, std::string_view path) {
    object(value, {"scope", "value"}, path);
    return {identity(required(value, "scope", path), std::string(path) + "/scope"),
            decimal(required(value, "value", path), std::string(path) + "/value")};
}

const char* audience(AssetAudience value) {
    switch (value) {
    case AssetAudience::client:
        return "client";
    case AssetAudience::server:
        return "server";
    case AssetAudience::shared:
        return "shared";
    }
    throw std::logic_error("Unknown native asset audience");
}

const char* job_state(AssetImportJobState value) {
    switch (value) {
    case AssetImportJobState::queued:
        return "queued";
    case AssetImportJobState::running:
        return "running";
    case AssetImportJobState::ready:
        return "ready";
    case AssetImportJobState::succeeded:
        return "succeeded";
    case AssetImportJobState::failed:
        return "failed";
    case AssetImportJobState::cancelled:
        return "cancelled";
    }
    throw std::logic_error("Unknown native import job state");
}

Json target_json(const AssetTargetProfile& value) {
    return {{"id", value.id}, {"side", audience(value.side)}};
}

Json summary_json(const AssetImportSummary& value) {
    return {{"cache_revision", revision_json(value.cache_revision)},
            {"catalog_revision", revision_json(value.catalog_revision)},
            {"root", value.root.value().to_string()},
            {"target", target_json(value.target)},
            {"root_key", value.root_key.to_hex()},
            {"nodes", value.nodes},
            {"imported", value.imported},
            {"reused", value.reused}};
}

Json optional_key(const std::optional<AssetCacheKey>& value) {
    return value ? Json(value->to_hex()) : Json(nullptr);
}

Json object_schema(Json properties, Json required_fields = Json::array()) {
    return {{"type", "object"},
            {"additionalProperties", false},
            {"properties", std::move(properties)},
            {"required", std::move(required_fields)}};
}
Json id_schema() {
    return {{"type", "string"},
            {"format", "uuid"},
            {"pattern", "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"}};
}
Json decimal_schema() {
    return {{"type", "string"},
            {"pattern", "^(0|[1-9][0-9]{0,19})$"},
            {"maximum_decimal", "18446744073709551615"}};
}
Json revision_schema() {
    return object_schema({{"scope", id_schema()}, {"value", decimal_schema()}}, {"scope", "value"});
}
Json job_id_schema() {
    return object_schema({{"scope", id_schema()}, {"sequence", decimal_schema()}},
                         {"scope", "sequence"});
}
Json target_schema() {
    return object_schema({{"id",
                           {{"type", "string"},
                            {"minLength", 1},
                            {"maxLength", 64},
                            {"pattern", "^[a-z][a-z0-9._-]*$"}}},
                          {"side", {{"enum", {"client", "server"}}}}},
                         {"id", "side"});
}
Json digest_schema() {
    return {{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}};
}
Json metadata_schema() {
    const Json identifier{{"type", "string"},
                          {"minLength", 1},
                          {"maxLength", 64},
                          {"pattern", "^[a-z][a-z0-9._-]*$"}};
    const Json bounded_object{{"type", "object"}, {"maximum_encoded_bytes", 4096}};
    auto schema = object_schema(
        {{"format", {{"const", "shark.asset"}}},
         {"schema_version", {{"type", "integer"}, {"const", asset_metadata_version}}},
         {"project_id", id_schema()},
         {"asset_id", id_schema()},
         {"kind", identifier},
         {"origin", {{"type", "string"}, {"minimum_utf8_bytes", 1}, {"maximum_utf8_bytes", 1024}}},
         {"license", {{"type", "string"}, {"minimum_utf8_bytes", 1}, {"maximum_utf8_bytes", 128}}},
         {"importer", identifier},
         {"audience", {{"enum", {"client", "server", "shared"}}}},
         {"settings", bounded_object},
         {"dependencies",
          {{"type", "array"},
           {"maxItems", asset_maximum_dependencies},
           {"uniqueItems", true},
           {"items", id_schema()}}},
         {"extras", bounded_object}},
        {"format", "schema_version", "project_id", "asset_id", "kind", "origin", "license",
         "importer", "audience", "settings", "dependencies"});
    schema["description"] = "Full shark.asset schema 1; native metadata codec validates the "
                            "envelope and bounded objects. Importer settings validation occurs "
                            "through the existing import service.";
    schema["maximum_encoded_bytes"] = asset_metadata_maximum_bytes;
    return schema;
}
Json path_schema() {
    return {{"type", "string"},
            {"minimum_utf8_bytes", 1},
            {"maximum_utf8_bytes", 4096},
            {"format", "contained-project-path"}};
}
Json result_schema(std::initializer_list<const char*> fields, const char* description) {
    Json required_fields = Json::array();
    for (const auto* field : fields)
        required_fields.push_back(field);
    return {
        {"type", "object"}, {"required", std::move(required_fields)}, {"description", description}};
}
} // namespace

Result<void> require_fields(const Json& value, std::initializer_list<std::string_view> allowed) {
    return boundary<void>([&]() -> Result<void> {
        object(value, allowed, "/params");
        return {};
    });
}

Result<std::size_t> parse_index(const Json& value, std::size_t maximum, std::string_view path) {
    return boundary<std::size_t>([&] {
        std::uint64_t result{};
        if (value.is_number_unsigned())
            result = value.get<std::uint64_t>();
        else if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
            result = static_cast<std::uint64_t>(value.get<std::int64_t>());
        else
            fail(path, "Expected a nonnegative integer, not a boolean or floating-point value");
        if (result > maximum)
            fail(path, "Integer exceeds the method bound");
        return static_cast<std::size_t>(result);
    });
}

Result<AssetId> parse_asset_id(const Json& value, std::string_view path) {
    return boundary<AssetId>([&] { return AssetId{identity(value, path)}; });
}
Result<AssetMetadata> parse_metadata(const Json& value) {
    return boundary<AssetMetadata>([&] {
        if (!value.is_object())
            fail("/params/metadata", "Expected the complete asset metadata object");
        return take(decode_asset_metadata(value.dump()), "/params/metadata");
    });
}
Result<AssetCatalogRevision> parse_catalog_revision(const Json& value, std::string_view path) {
    return boundary<AssetCatalogRevision>(
        [&] { return revision<AssetCatalogRevision>(value, path); });
}
Result<AssetCacheRevision> parse_cache_revision(const Json& value, std::string_view path) {
    return boundary<AssetCacheRevision>([&] { return revision<AssetCacheRevision>(value, path); });
}
Result<AssetImportJobId> parse_job_id(const Json& value) {
    return boundary<AssetImportJobId>([&] {
        object(value, {"scope", "sequence"}, "/params/job_id");
        return AssetImportJobId{
            identity(required(value, "scope", "/params/job_id"), "/params/job_id/scope"),
            decimal(required(value, "sequence", "/params/job_id"), "/params/job_id/sequence")};
    });
}
Result<AssetTargetProfile> parse_target(const Json& value) {
    return boundary<AssetTargetProfile>([&] {
        object(value, {"id", "side"}, "/params/target");
        auto id = text(required(value, "id", "/params/target"), "/params/target/id");
        if (id.empty() || id.size() > 64)
            fail("/params/target/id", "Target ID must contain 1..64 bytes");
        const auto& side = required(value, "side", "/params/target");
        if (side != "client" && side != "server")
            fail("/params/target/side", "Target side must be client or server");
        return AssetTargetProfile{std::move(id),
                                  side == "client" ? AssetAudience::client : AssetAudience::server};
    });
}
Result<AssetCacheKey> parse_key(const Json& value) {
    return boundary<AssetCacheKey>(
        [&] { return take(ContentDigest::parse(text(value, "/params/key")), "/params/key"); });
}
Result<std::vector<ProjectPath>> parse_sidecars(const Json& value) {
    return boundary<std::vector<ProjectPath>>([&] {
        if (!value.is_array() || value.size() > asset_maximum_catalog_entries)
            fail("/params/sidecars", "Expected at most 4096 sidecar paths");
        std::vector<ProjectPath> result;
        result.reserve(value.size());
        for (std::size_t index = 0; index < value.size(); ++index) {
            const auto path = "/params/sidecars/" + std::to_string(index);
            result.push_back(take(ProjectPath::parse(text(value[index], path)), path));
        }
        return result;
    });
}

Json revision_json(AssetCatalogRevision value) {
    return {{"scope", value.scope.to_string()}, {"value", std::to_string(value.value)}};
}
Json revision_json(AssetCacheRevision value) {
    return {{"scope", value.scope.to_string()}, {"value", std::to_string(value.value)}};
}
Json job_id_json(AssetImportJobId value) {
    return {{"scope", value.scope.to_string()}, {"sequence", std::to_string(value.sequence)}};
}
Json error_json(const Error& value) {
    return {{"code", error_code_name(value.code)},
            {"message", value.message},
            {"context",
             {{"operation", value.context.operation},
              {"request_id", value.context.request_id},
              {"path", value.context.path},
              {"entity_id", value.context.entity_id},
              {"asset_id", value.context.asset_id}}}};
}
Json publication_json(const FileCommit& value) {
    const char* durability = "file_flushed";
    if (value.durability == FileDurability::file_and_directory_flushed)
        durability = "file_and_directory_flushed";
    else if (value.durability == FileDurability::directory_flush_failed)
        durability = "directory_flush_failed";
    return {
        {"published", true}, {"bytes", value.revision.byte_count()}, {"durability", durability}};
}
Json metadata_json(const AssetMetadata& value) {
    auto encoded = encode_asset_metadata(value);
    if (!encoded)
        throw std::logic_error("Cannot encode validated asset metadata");
    return Json::parse(encoded.value());
}
Json record_json(const AssetRecord& value) {
    return {{"metadata", metadata_json(value.metadata)},
            {"source", value.source.text()},
            {"sidecar", value.sidecar.text()},
            {"metadata_bytes", value.metadata_revision.byte_count()},
            {"source_present", value.source_present}};
}
Json page_json(const AssetCatalogPage& value) {
    Json records = Json::array();
    for (const auto& record : value.records)
        records.push_back(record_json(record));
    return {{"revision", revision_json(value.revision)},
            {"total", value.total},
            {"records", std::move(records)},
            {"next_offset", value.next_offset ? Json(*value.next_offset) : Json(nullptr)}};
}
Json resolution_json(const AssetResolution& value) {
    const char* availability = "unknown_id";
    if (value.availability == AssetAvailability::source_missing)
        availability = "source_missing";
    else if (value.availability == AssetAvailability::source_present)
        availability = "source_present";
    return {{"revision", revision_json(value.revision)},
            {"asset_id", value.asset.value().to_string()},
            {"availability", availability},
            {"record", value.record ? record_json(*value.record) : Json(nullptr)}};
}
Json importers_json(const std::vector<AssetImporterInfo>& value) {
    Json result = Json::array();
    for (const auto& importer : value)
        result.push_back({{"id", importer.id},
                          {"version", importer.version},
                          {"kind", importer.kind},
                          {"payload_version", importer.payload_version},
                          {"settings_schema", Json::parse(importer.settings_schema_json)}});
    return result;
}
Json cache_status_json(const AssetCacheStatus& value) {
    const char* state = "missing";
    if (value.state == AssetCacheState::fresh)
        state = "fresh";
    else if (value.state == AssetCacheState::stale)
        state = "stale";
    return {{"revision", revision_json(value.revision)},
            {"asset_id", value.asset.value().to_string()},
            {"target", target_json(value.target)},
            {"state", state},
            {"required_key", optional_key(value.required_key)},
            {"last_valid_key", optional_key(value.last_valid_key)},
            {"blocked_by", value.blocked_by ? error_json(*value.blocked_by) : Json(nullptr)}};
}
Json usage_json(const AssetCacheUsage& value) {
    return {{"revision", revision_json(value.revision)},
            {"artifacts", value.artifacts},
            {"bindings", value.bindings},
            {"payload_bytes", value.payload_bytes},
            {"file_bytes", value.file_bytes},
            {"unpublished_stage", value.unpublished_stage}};
}
Json commit_json(const AssetCacheCommit& value) {
    return {{"revision", revision_json(value.revision)},
            {"changed", value.changed},
            {"publication", value.file ? publication_json(*value.file) : Json(nullptr)}};
}
Json artifact_json(const AssetArtifact& value) {
    Json dependencies = Json::array();
    for (const auto& dependency : value.dependencies)
        dependencies.push_back(
            {{"asset_id", dependency.asset.value().to_string()}, {"key", dependency.key.to_hex()}});
    return {{"asset_id", value.asset.value().to_string()},
            {"key", value.key.to_hex()},
            {"kind", value.kind},
            {"audience", audience(value.audience)},
            {"target", target_json(value.target)},
            {"importer", value.importer},
            {"importer_version", value.importer_version},
            {"payload_version", value.payload_version},
            {"source_digest", value.source_digest.to_hex()},
            {"settings_digest", value.settings_digest.to_hex()},
            {"dependencies", std::move(dependencies)},
            {"payload_digest", value.payload_digest.to_hex()},
            {"payload_bytes", value.payload.size()}};
}
Json job_json(const AssetImportJobStatus& value) {
    return {{"job_id", job_id_json(value.id)},
            {"state", job_state(value.state)},
            {"completed_nodes", value.completed_nodes},
            {"total_nodes", value.total_nodes},
            {"summary", value.summary ? summary_json(*value.summary) : Json(nullptr)},
            {"commit", value.commit ? commit_json(*value.commit) : Json(nullptr)},
            {"error", value.error ? error_json(*value.error) : Json(nullptr)}};
}
Json jobs_json(const std::vector<AssetImportJobStatus>& value) {
    Json result = Json::array();
    for (const auto& job : value)
        result.push_back(job_json(job));
    return result;
}
Json admission_json(const AssetImportJobAdmission& value) {
    return {{"job_id", job_id_json(value.id)}, {"deduplicated", value.deduplicated}};
}

Json methods_json() {
    const auto empty = object_schema(Json::object());
    const auto revision = revision_schema();
    const auto observed = object_schema({{"expected_revision", revision}}, {"expected_revision"});
    const auto identity = object_schema({{"job_id", job_id_schema()}}, {"job_id"});
    const auto import_request =
        object_schema({{"asset_id", id_schema()},
                       {"target", target_schema()},
                       {"expected_catalog", revision},
                       {"expected_cache", revision}},
                      {"asset_id", "target", "expected_catalog", "expected_cache"});
    const Json sample_revision{{"scope", "30000000-0000-4000-8000-000000000001"}, {"value", "1"}};
    const Json sample_job{{"scope", "30000000-0000-4000-8000-000000000002"}, {"sequence", "1"}};
    const Json sample_import{{"asset_id", "30000000-0000-4000-8000-000000000003"},
                             {"target", {{"id", "windows-client"}, {"side", "client"}}},
                             {"expected_catalog", sample_revision},
                             {"expected_cache", sample_revision}};
    Json methods = Json::array();
    const auto add = [&](const char* name, Json params, Json result, Json example) {
        methods.push_back({{"name", name},
                           {"execution_context", "cpu_owner_thread"},
                           {"requires_open_project", true},
                           {"requires_project_root", false},
                           {"params", std::move(params)},
                           {"result", std::move(result)},
                           {"example",
                            {{"jsonrpc", "2.0"},
                             {"protocol_version", 1},
                             {"id", "example"},
                             {"method", name},
                             {"params", std::move(example)}}}});
    };
    add("asset.importers", empty,
        result_schema({"importers"},
                      "Actual registered native importer descriptors and settings schemas"),
        Json::object());
    add("asset.metadata.read", object_schema({{"path", path_schema()}}, {"path"}),
        result_schema({"path", "metadata", "revision"},
                      "Exact native metadata and an opaque retained file revision"),
        {{"path", "assets/mesh.gltf.sharkasset"}});
    const auto metadata = metadata_schema();
    const Json sample_metadata{{"format", "shark.asset"},
                               {"schema_version", 1},
                               {"project_id", "30000000-0000-4000-8000-000000000004"},
                               {"asset_id", "30000000-0000-4000-8000-000000000003"},
                               {"kind", "bytes"},
                               {"origin", "Project-owned source"},
                               {"license", "MIT"},
                               {"importer", "shark.bytes"},
                               {"audience", "shared"},
                               {"settings", Json::object()},
                               {"dependencies", Json::array()}};
    add("asset.metadata.write",
        object_schema({{"path", path_schema()},
                       {"metadata", metadata},
                       {"expected_revision", {{"oneOf", {id_schema(), Json{{"type", "null"}}}}}}},
                      {"path", "metadata", "expected_revision"}),
        result_schema({"path", "revision", "publication"},
                      "Published sidecar, new token and explicit durability"),
        {{"path", "assets/data.bin.sharkasset"},
         {"metadata", sample_metadata},
         {"expected_revision", nullptr}});
    add("asset.metadata.release", object_schema({{"revision", id_schema()}}, {"revision"}),
        object_schema({{"released", {{"const", true}}}}, {"released"}),
        {{"revision", "30000000-0000-4000-8000-000000000005"}});
    add("asset.catalog.status", empty,
        result_schema({"revision"}, "Current catalog scope and revision"), Json::object());
    add("asset.catalog.refresh",
        object_schema({{"sidecars",
                        {{"type", "array"},
                         {"maxItems", asset_maximum_catalog_entries},
                         {"items", path_schema()}}},
                       {"expected_revision", revision}},
                      {"sidecars", "expected_revision"}),
        result_schema({"revision"}, "Complete explicit inventory validated and atomically adopted"),
        {{"sidecars", Json::array()}, {"expected_revision", sample_revision}});
    add("asset.catalog.query",
        object_schema({{"expected_revision", revision},
                       {"offset",
                        {{"type", "integer"},
                         {"minimum", 0},
                         {"maximum", asset_maximum_catalog_entries},
                         {"default", 0}}},
                       {"page_size",
                        {{"type", "integer"},
                         {"minimum", 1},
                         {"maximum", asset_maximum_page_size},
                         {"default", 64}}}},
                      {"expected_revision"}),
        result_schema({"revision", "total", "records", "next_offset"},
                      "Owned metadata records ordered by asset ID; each continuation requires the "
                      "original revision"),
        {{"expected_revision", sample_revision}, {"page_size", 16}});
    add("asset.catalog.resolve",
        object_schema({{"asset_id", id_schema()}, {"expected_revision", revision}},
                      {"asset_id", "expected_revision"}),
        result_schema(
            {"revision", "asset_id", "availability", "record"},
            "Availability is unknown_id, source_missing or source_present; record is nullable"),
        {{"asset_id", sample_import["asset_id"]}, {"expected_revision", sample_revision}});
    add("asset.cache.status", empty,
        result_schema({"revision", "artifacts", "bindings", "payload_bytes", "file_bytes",
                       "unpublished_stage"},
                      "Current development-cache usage and publication-stage observation"),
        Json::object());
    add("asset.cache.query", import_request,
        result_schema({"revision", "asset_id", "target", "state", "required_key", "last_valid_key",
                       "blocked_by"},
                      "Fresh/stale/missing with retained last-valid output and nullable input "
                      "failure; never imports"),
        sample_import);
    add("asset.cache.inspect",
        object_schema({{"key", digest_schema()}, {"expected_revision", revision}},
                      {"key", "expected_revision"}),
        result_schema({"asset_id", "key", "kind", "audience", "target", "importer",
                       "importer_version", "payload_version", "source_digest", "settings_digest",
                       "dependencies", "payload_digest", "payload_bytes"},
                      "Validated artifact descriptor and digests; binary payload is not returned"),
        {{"key", std::string(64, '0')}, {"expected_revision", sample_revision}});
    add("asset.cache.reload", observed,
        result_schema({"revision"},
                      "Explicitly adopt validated on-disk cache, invalidating stale jobs"),
        {{"expected_revision", sample_revision}});
    for (const auto* name : {"asset.cache.prune", "asset.cache.clear"})
        add(name, observed,
            result_schema(
                {"revision", "changed", "publication"},
                "Cache decision with nullable publication receipt; authored sources stay intact"),
            {{"expected_revision", sample_revision}});
    add("asset.cache.recover", empty,
        result_schema({"removed"}, "Remove only the reserved bounded unpublished cache stage"),
        Json::object());
    add("asset.jobs.submit", import_request,
        result_schema({"job_id", "deduplicated"},
                      "Capture current inputs and admit or deduplicate bounded pending work"),
        sample_import);
    add("asset.jobs.list", empty,
        result_schema({"jobs"}, "At most four owned statuses; observing jobs never publishes"),
        Json::object());
    for (const auto* name : {"asset.jobs.cancel", "asset.jobs.take"})
        add(name, identity,
            result_schema(
                {"job_id", "state", "completed_nodes", "total_nodes", "summary", "commit", "error"},
                "Native job outcome; cancellation does not roll back success and take requires a "
                "terminal state"),
            {{"job_id", sample_job}});
    add("asset.jobs.drain", empty,
        result_schema({"drained", "jobs"}, "Nonblocking publication of at most one ready "
                                           "completion; inspect each terminal outcome"),
        Json::object());
    add("asset.jobs.close", empty,
        result_schema({"closed", "jobs"}, "Permanently close admission, cancel uncommitted work, "
                                          "join and preserve terminal outcomes"),
        Json::object());
    return methods;
}

} // namespace shark::tools::authoring_asset_wire
