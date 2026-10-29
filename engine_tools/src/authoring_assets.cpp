#include "authoring_assets.hpp"

#include "authoring_asset_wire.hpp"
#include "shark/tools/gltf_asset.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <utility>

namespace shark::tools {
namespace {
using Json = nlohmann::json;
namespace wire = authoring_asset_wire;

constexpr std::array methods{
    std::string_view{"asset.importers"},      std::string_view{"asset.metadata.read"},
    std::string_view{"asset.metadata.write"}, std::string_view{"asset.metadata.release"},
    std::string_view{"asset.catalog.status"}, std::string_view{"asset.catalog.refresh"},
    std::string_view{"asset.catalog.query"},  std::string_view{"asset.catalog.resolve"},
    std::string_view{"asset.cache.status"},   std::string_view{"asset.cache.query"},
    std::string_view{"asset.cache.inspect"},  std::string_view{"asset.cache.reload"},
    std::string_view{"asset.cache.prune"},    std::string_view{"asset.cache.clear"},
    std::string_view{"asset.cache.recover"},  std::string_view{"asset.jobs.submit"},
    std::string_view{"asset.jobs.list"},      std::string_view{"asset.jobs.cancel"},
    std::string_view{"asset.jobs.drain"},     std::string_view{"asset.jobs.take"},
    std::string_view{"asset.jobs.close"}};

[[noreturn]] void fail(int code, ErrorCode engine_code, const char* message,
                       std::string path = {}) {
    throw AuthoringAssetFailure{
        code, {engine_code, message, {.operation = "authoring.assets", .path = std::move(path)}}};
}

template <typename T> T take(Result<T> result, int code = -32000) {
    if (!result)
        throw AuthoringAssetFailure{code, std::move(result).error()};
    return std::move(result).value();
}

void accept(Result<void> result, int code = -32000) {
    if (!result)
        throw AuthoringAssetFailure{code, std::move(result).error()};
}

void fields(const Json& parameters, std::initializer_list<std::string_view> allowed) {
    accept(wire::require_fields(parameters, allowed), -32602);
}

const Json& parameter(const Json& parameters, const char* name) {
    if (!parameters.contains(name))
        fail(-32602, ErrorCode::invalid_argument, "Required asset parameter is missing",
             std::string{"/params/"} + name);
    return parameters.at(name);
}

std::string token(const Json& value, const char* path) {
    if (!value.is_string())
        fail(-32602, ErrorCode::invalid_id, "Expected an asset metadata revision UUID", path);
    return take(StableId::parse(value.get_ref<const std::string&>()), -32602).to_string();
}

ProjectPath metadata_path(const Json& parameters) {
    const auto& value = parameter(parameters, "path");
    if (!value.is_string())
        fail(-32602, ErrorCode::invalid_path, "Expected a relative asset metadata path",
             "/params/path");
    return take(ProjectPath::parse(value.get_ref<const std::string&>()), -32602);
}

std::string folded_path(const ProjectPath& path) {
    auto result = path.text();
    for (auto& character : result)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character + ('a' - 'A'));
    return result;
}

bool overlaps(const ProjectPath& left, const ProjectPath& right) {
    const auto a = folded_path(left);
    const auto b = folded_path(right);
    return a == b || a.starts_with(b + '/') || b.starts_with(a + '/');
}

bool terminal(AssetImportJobState state) {
    return state == AssetImportJobState::succeeded || state == AssetImportJobState::failed ||
           state == AssetImportJobState::cancelled;
}
} // namespace

struct AuthoringAssets::Impl {
    ProjectFiles& files;
    const ProjectDocument& project;
    const ProjectPath& project_path;
    ByteAssetImporter bytes;
    GltfResourceImporter resource;
    GltfAssetImporter gltf;
    const std::vector<AssetImporterInfo> descriptors;
    std::unique_ptr<AssetCatalog> catalog;
    // Reverse destruction joins the worker before catalog or importer release.
    std::unique_ptr<AssetImports> imports;
    struct MetadataRevision {
        ProjectPath path;
        std::optional<FileRevision> revision;
    };
    std::map<std::string, MetadataRevision> revisions;
    bool jobs_closed{false};

    Impl(ProjectFiles& owner, const ProjectDocument& document, const ProjectPath& path)
        : files(owner), project(document), project_path(path),
          descriptors{bytes.info(), resource.info(), gltf.info()},
          catalog(take(AssetCatalog::create(files, project.project))) {}

    AssetImports& cache() {
        if (!imports) {
            const std::array<AssetImporter*, 3> registry{&bytes, &resource, &gltf};
            auto candidate = take(AssetImports::open(files, *catalog, project.project, registry));
            if (jobs_closed)
                accept(candidate->close_jobs());
            imports = std::move(candidate);
        }
        return *imports;
    }

    void check_path(const ProjectPath& sidecar) const {
        const auto source = take(asset_source_path(sidecar), -32602);
        for (const auto& candidate : {sidecar, source}) {
            if (overlaps(candidate, project_path))
                fail(-32000, ErrorCode::conflict,
                     "Asset access overlaps an owned project source path", candidate.text());
            for (const auto& scene : project.scenes)
                if (overlaps(candidate, scene.path))
                    fail(-32000, ErrorCode::conflict,
                         "Asset access overlaps an owned scene source path", candidate.text());
        }
    }

    void check_project(const AssetMetadata& metadata) const {
        if (metadata.project != project.project)
            fail(-32000, ErrorCode::conflict, "Asset metadata belongs to another project");
    }

    void check_token_capacity(bool replacing = false) const {
        if (!replacing && revisions.size() >= authoring_asset_maximum_metadata_revisions)
            fail(-32000, ErrorCode::conflict,
                 "Asset metadata revision limit reached; release an unused token or reuse a "
                 "retained token");
    }

    std::string fresh_token() const {
        auto identity = take(generate_stable_id()).to_string();
        if (revisions.contains(identity))
            fail(-32000, ErrorCode::conflict, "Generated asset metadata revision already exists");
        return identity;
    }

    std::vector<AssetImportJobStatus> jobs() const {
        return imports ? take(imports->jobs()) : std::vector<AssetImportJobStatus>{};
    }

    AssetImports& existing_jobs() {
        if (!imports)
            fail(-32000, ErrorCode::conflict,
                 "Import job was taken, closed or belongs to another owner");
        return *imports;
    }
};

AuthoringAssets::AuthoringAssets(ProjectFiles& files, const ProjectDocument& project,
                                 const ProjectPath& path)
    : impl_(std::make_unique<Impl>(files, project, path)) {}
AuthoringAssets::~AuthoringAssets() = default;

PreparedClientExport AuthoringAssets::prepare_export(const AuthoredScene& owner,
                                                     const ClientExportRequest& request) {
    return take(prepare_client_export(owner, *impl_->catalog, impl_->cache(), request));
}

PreparedClientExport AuthoringAssets::prepare_current_export(const AuthoredScene& owner,
                                                             AuthoredEntityId camera,
                                                             SceneRevision expected,
                                                             const AssetTargetProfile& target) {
    auto& cache = impl_->cache();
    return take(prepare_client_export(
        owner, *impl_->catalog, cache,
        {camera, expected, take(impl_->catalog->revision()), take(cache.revision()), target}));
}

Result<void> AuthoringAssets::validate_export(const PreparedClientExport& candidate,
                                              const AuthoredScene& owner) {
    return validate_client_export(candidate, owner, *impl_->catalog, impl_->cache());
}

bool AuthoringAssets::supports(std::string_view method) noexcept {
    return std::find(methods.begin(), methods.end(), method) != methods.end();
}

bool AuthoringAssets::requires_project_fence(std::string_view method) noexcept {
    return method != "asset.jobs.list" && method != "asset.jobs.cancel" &&
           method != "asset.jobs.take" && method != "asset.jobs.close" &&
           method != "asset.metadata.release";
}

AuthoringAssetReply AuthoringAssets::dispatch(std::string_view method, const Json& parameters,
                                              bool& outcome_may_have_changed) {
    if (method == "asset.importers") {
        fields(parameters, {});
        const auto descriptors =
            impl_->imports ? take(impl_->imports->importers()) : impl_->descriptors;
        return {Json{{"importers", wire::importers_json(descriptors)}}};
    }
    if (method == "asset.metadata.read") {
        fields(parameters, {"path"});
        auto path = metadata_path(parameters);
        impl_->check_path(path);
        impl_->check_token_capacity();
        auto value = take(impl_->files.read_asset_metadata(path));
        impl_->check_project(value.source.document);
        auto identity = impl_->fresh_token();
        AuthoringAssetReply reply{Json{{"path", path.text()},
                                       {"metadata", wire::metadata_json(value.source.document)},
                                       {"revision", identity}}};
        reply.observation.emplace(AuthoringAssetObservation{std::move(identity), std::move(path),
                                                            std::move(value.revision)});
        return reply;
    }
    if (method == "asset.metadata.release") {
        fields(parameters, {"revision"});
        auto identity = token(parameter(parameters, "revision"), "/params/revision");
        if (!impl_->revisions.contains(identity))
            fail(-32000, ErrorCode::conflict,
                 "Asset metadata revision was consumed, closed or belongs to another owner");
        AuthoringAssetReply reply{Json{{"released", true}}};
        reply.released_token = std::move(identity);
        return reply;
    }
    if (method == "asset.metadata.write") {
        fields(parameters, {"path", "metadata", "expected_revision"});
        auto path = metadata_path(parameters);
        auto metadata = take(wire::parse_metadata(parameter(parameters, "metadata")), -32602);
        impl_->check_path(path);
        impl_->check_project(metadata);
        const auto& revision = parameter(parameters, "expected_revision");
        std::optional<std::string> consumed;
        std::optional<FileRevision> expected;
        if (!revision.is_null()) {
            consumed = token(revision, "/params/expected_revision");
            const auto found = impl_->revisions.find(*consumed);
            if (found == impl_->revisions.end() || found->second.path != path ||
                !found->second.revision)
                fail(-32000, ErrorCode::conflict,
                     "Asset metadata revision was consumed, closed or belongs to another "
                     "path/owner",
                     path.text());
            expected = found->second.revision;
        }
        impl_->check_token_capacity(consumed.has_value());
        auto identity = impl_->fresh_token();
        // Allocate the retained revision slot before any file can be published.
        const auto inserted = impl_->revisions.emplace(identity, Impl::MetadataRevision{path, {}});
        auto committed = [&] {
            try {
                return impl_->files.write_asset_metadata(path, metadata, expected);
            } catch (...) {
                impl_->revisions.erase(inserted.first);
                throw;
            }
        }();
        if (!committed) {
            impl_->revisions.erase(inserted.first);
            static_cast<void>(take(std::move(committed)));
        }
        outcome_may_have_changed = true;
        inserted.first->second.revision = committed.value().revision;
        if (consumed)
            impl_->revisions.erase(*consumed);
        return {Json{{"path", path.text()},
                     {"revision", identity},
                     {"publication", wire::publication_json(committed.value())}}};
    }
    if (method == "asset.catalog.status") {
        fields(parameters, {});
        return {Json{{"revision", wire::revision_json(take(impl_->catalog->revision()))}}};
    }
    if (method == "asset.catalog.refresh") {
        fields(parameters, {"sidecars", "expected_revision"});
        const auto sidecars = take(wire::parse_sidecars(parameter(parameters, "sidecars")), -32602);
        const auto expected =
            take(wire::parse_catalog_revision(parameter(parameters, "expected_revision")), -32602);
        for (const auto& sidecar : sidecars)
            impl_->check_path(sidecar);
        outcome_may_have_changed = true;
        return {Json{
            {"revision", wire::revision_json(take(impl_->catalog->refresh(sidecars, expected)))}}};
    }
    if (method == "asset.catalog.query") {
        fields(parameters, {"expected_revision", "offset", "page_size"});
        const auto expected =
            take(wire::parse_catalog_revision(parameter(parameters, "expected_revision")), -32602);
        const auto offset =
            parameters.contains("offset")
                ? take(wire::parse_index(parameters.at("offset"), asset_maximum_catalog_entries,
                                         "/params/offset"),
                       -32602)
                : std::size_t{0};
        const auto size =
            parameters.contains("page_size")
                ? take(wire::parse_index(parameters.at("page_size"), asset_maximum_page_size,
                                         "/params/page_size"),
                       -32602)
                : std::size_t{64};
        if (size == 0)
            fail(-32602, ErrorCode::invalid_argument, "Catalog page_size must be between 1 and 128",
                 "/params/page_size");
        return {wire::page_json(take(impl_->catalog->query(expected, offset, size)))};
    }
    if (method == "asset.catalog.resolve") {
        fields(parameters, {"asset_id", "expected_revision"});
        const auto asset = take(wire::parse_asset_id(parameter(parameters, "asset_id")), -32602);
        const auto expected =
            take(wire::parse_catalog_revision(parameter(parameters, "expected_revision")), -32602);
        return {wire::resolution_json(take(impl_->catalog->resolve(asset, expected)))};
    }
    if (method == "asset.cache.status") {
        fields(parameters, {});
        auto& cache = impl_->cache();
        return {wire::usage_json(take(cache.usage(take(cache.revision()))))};
    }
    if (method == "asset.cache.query" || method == "asset.jobs.submit") {
        fields(parameters, {"asset_id", "target", "expected_catalog", "expected_cache"});
        const auto asset = take(wire::parse_asset_id(parameter(parameters, "asset_id")), -32602);
        const auto target = take(wire::parse_target(parameter(parameters, "target")), -32602);
        const auto catalog =
            take(wire::parse_catalog_revision(parameter(parameters, "expected_catalog"),
                                              "/params/expected_catalog"),
                 -32602);
        const auto cache = take(wire::parse_cache_revision(parameter(parameters, "expected_cache"),
                                                           "/params/expected_cache"),
                                -32602);
        if (method == "asset.cache.query")
            return {
                wire::cache_status_json(take(impl_->cache().query(asset, target, catalog, cache)))};
        if (impl_->jobs_closed)
            fail(-32000, ErrorCode::conflict,
                 "Import job admission has been closed for this owner");
        outcome_may_have_changed = true;
        return {wire::admission_json(take(impl_->cache().submit(asset, target, catalog, cache)))};
    }
    if (method == "asset.cache.inspect") {
        fields(parameters, {"key", "expected_revision"});
        const auto key = take(wire::parse_key(parameter(parameters, "key")), -32602);
        const auto expected =
            take(wire::parse_cache_revision(parameter(parameters, "expected_revision")), -32602);
        return {wire::artifact_json(take(impl_->cache().read(key, expected)))};
    }
    if (method == "asset.cache.reload" || method == "asset.cache.prune" ||
        method == "asset.cache.clear") {
        fields(parameters, {"expected_revision"});
        const auto expected =
            take(wire::parse_cache_revision(parameter(parameters, "expected_revision")), -32602);
        auto& cache = impl_->cache();
        outcome_may_have_changed = true;
        if (method == "asset.cache.reload")
            return {Json{{"revision", wire::revision_json(take(cache.reload(expected)))}}};
        return {wire::commit_json(
            take(method == "asset.cache.prune" ? cache.prune(expected) : cache.clear(expected)))};
    }
    if (method == "asset.cache.recover") {
        fields(parameters, {});
        outcome_may_have_changed = true;
        return {Json{{"removed", take(AssetImports::recover(impl_->files))}}};
    }
    if (method == "asset.jobs.list") {
        fields(parameters, {});
        return {Json{{"jobs", wire::jobs_json(impl_->jobs())}}};
    }
    if (method == "asset.jobs.cancel") {
        fields(parameters, {"job_id"});
        const auto id = take(wire::parse_job_id(parameter(parameters, "job_id")), -32602);
        outcome_may_have_changed = true;
        return {wire::job_json(take(impl_->existing_jobs().cancel_job(id)))};
    }
    if (method == "asset.jobs.drain") {
        fields(parameters, {});
        outcome_may_have_changed = true;
        const auto drained = impl_->imports ? take(impl_->imports->drain_jobs()) : std::size_t{0};
        return {Json{{"drained", drained}, {"jobs", wire::jobs_json(impl_->jobs())}}};
    }
    if (method == "asset.jobs.take") {
        fields(parameters, {"job_id"});
        const auto id = take(wire::parse_job_id(parameter(parameters, "job_id")), -32602);
        const auto jobs = take(impl_->existing_jobs().jobs());
        const auto found =
            std::find_if(jobs.begin(), jobs.end(), [&](const auto& job) { return job.id == id; });
        if (found == jobs.end())
            fail(-32000, ErrorCode::conflict,
                 "Import job was taken, closed or belongs to another owner");
        if (!terminal(found->state))
            fail(-32000, ErrorCode::conflict,
                 "Job is not terminal; drain its completion before taking");
        AuthoringAssetReply reply{wire::job_json(*found)};
        reply.taken_job = id;
        return reply;
    }
    if (method == "asset.jobs.close") {
        fields(parameters, {});
        outcome_may_have_changed = true;
        if (impl_->imports)
            accept(impl_->imports->close_jobs());
        impl_->jobs_closed = true;
        return {Json{{"closed", true}, {"jobs", wire::jobs_json(impl_->jobs())}}};
    }
    fail(-32601, ErrorCode::invalid_argument, "Unknown asset authoring method");
}

void AuthoringAssets::admit(AuthoringAssetReply& reply) {
    if (reply.observation) {
        auto& observation = *reply.observation;
        const auto inserted = impl_->revisions.emplace(
            std::move(observation.token),
            Impl::MetadataRevision{std::move(observation.path), std::move(observation.revision)});
        if (!inserted.second)
            fail(-32000, ErrorCode::conflict,
                 "Asset metadata revision publication identity already exists");
    }
    if (reply.released_token)
        impl_->revisions.erase(*reply.released_token);
    if (reply.taken_job)
        static_cast<void>(take(impl_->existing_jobs().take_job(*reply.taken_job)));
}

} // namespace shark::tools
