#include "shark/tools/client_export.hpp"

#include "asset_cache_format.hpp"

#include <algorithm>
#include <map>
#include <new>
#include <set>

namespace shark::tools {
namespace {
struct Failure {
    Error error;
};
template <typename T> T take(Result<T> result) {
    if (!result)
        throw Failure{std::move(result).error()};
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result)
        throw Failure{std::move(result).error()};
}
[[noreturn]] void fail(ErrorCode code, std::string message, std::optional<AssetId> asset = {},
                       std::optional<AuthoredEntityId> entity = {}) {
    Error error{code, std::move(message), {}};
    if (asset)
        error.context.asset_id = asset->value().to_string();
    if (entity)
        error.context.entity_id = entity->value().to_string();
    throw Failure{std::move(error)};
}
template <typename T, typename F> Result<T> boundary(const char* operation, F&& body) {
    try {
        return body();
    } catch (Failure& failure) {
        failure.error.context.operation = operation;
        return std::move(failure.error);
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::execution_failed,
                     "Cannot allocate bounded client export content",
                     {operation}};
    } catch (...) {
        return Error{
            ErrorCode::execution_failed, "Client export content preparation failed", {operation}};
    }
}

bool builtin(AssetId asset) {
    return asset == builtin_cube_asset_id() || asset == builtin_plane_asset_id();
}
void check_request(const ClientExportRequest& request) {
    take(detail::validate_target(request.target));
    if (request.target.side != AssetAudience::client)
        fail(ErrorCode::invalid_configuration,
             "Client export requires a client-side content target");
}
void check_startup(const CapturedScene& capture, AuthoredEntityId camera) {
    if (capture.project.startup_scene != capture.scene.scene)
        fail(ErrorCode::invalid_configuration,
             "Active scene is not the project's declared startup scene");
    const auto found = std::find_if(capture.scene.entities.begin(), capture.scene.entities.end(),
                                    [&](const auto& entity) { return entity.id == camera; });
    if (found == capture.scene.entities.end() || !found->camera)
        fail(ErrorCode::invalid_id, "Export camera must name an entity with a Camera", {}, camera);
}
std::set<AssetId> roots(const SceneDocument& scene) {
    std::set<AssetId> result;
    for (const auto& entity : scene.entities) {
        if (entity.mesh_renderer && !builtin(entity.mesh_renderer->asset)) {
            result.insert(entity.mesh_renderer->asset);
            if (result.size() > client_export_maximum_meshes)
                fail(ErrorCode::invalid_configuration,
                     "Client export exceeds 126 external mesh assets", entity.mesh_renderer->asset,
                     entity.id);
        }
    }
    return result;
}
using Records = std::map<AssetId, AssetRecord>;
Records preflight(const std::set<AssetId>& root_ids, ProjectId project, AssetCatalog& catalog,
                  const ClientExportRequest& request) {
    if (take(catalog.revision()) != request.catalog_revision)
        fail(ErrorCode::conflict, "Export catalog scope or revision changed");
    Records records;
    std::size_t visits = 0;
    for (const auto root : root_ids) {
        std::set<AssetId> visited;
        std::vector<AssetId> pending{root};
        while (!pending.empty()) {
            const auto asset = pending.back();
            pending.pop_back();
            if (!visited.insert(asset).second)
                continue;
            if (++visits > client_export_maximum_dependency_visits)
                fail(ErrorCode::invalid_configuration,
                     "Export freshness work exceeds 512 dependency visits", asset);
            auto found = records.find(asset);
            if (found == records.end()) {
                if (records.size() == client_export_maximum_sources)
                    fail(ErrorCode::invalid_configuration,
                         "Export import closure exceeds 128 sources", asset);
                auto resolved = take(catalog.resolve(asset, request.catalog_revision));
                if (!resolved.record)
                    fail(ErrorCode::invalid_id, "Export asset is absent from the catalog", asset);
                auto& record = *resolved.record;
                if (record.metadata.project != project)
                    fail(ErrorCode::conflict, "Export asset belongs to another project", asset);
                if (record.metadata.audience != AssetAudience::client &&
                    record.metadata.audience != AssetAudience::shared)
                    fail(ErrorCode::invalid_configuration,
                         "Client export cannot include server-private content", asset);
                found = records.emplace(asset, std::move(record)).first;
            }
            const auto& dependencies = found->second.metadata.dependencies;
            pending.insert(pending.end(), dependencies.begin(), dependencies.end());
        }
    }
    return records;
}
AssetCacheKey fresh(AssetImports& imports, AssetId asset, const ClientExportRequest& request) {
    auto status = take(
        imports.query(asset, request.target, request.catalog_revision, request.cache_revision));
    if (status.blocked_by)
        throw Failure{std::move(*status.blocked_by)};
    if (status.state != AssetCacheState::fresh || !status.required_key ||
        status.required_key != status.last_valid_key)
        fail(ErrorCode::conflict,
             "Export requires a fresh import; explicitly import or reconcile this asset", asset);
    return *status.required_key;
}
void check_artifact(const AssetArtifact& artifact, AssetId expected_asset,
                    AssetCacheKey expected_key, const ClientExportRequest& request) {
    if (artifact.asset != expected_asset || artifact.key != expected_key ||
        artifact.target != request.target ||
        (artifact.audience != AssetAudience::client && artifact.audience != AssetAudience::shared))
        fail(ErrorCode::conflict, "Cached export artifact identity or client target differs",
             expected_asset);
}
} // namespace

Result<PreparedClientExport> prepare_client_export(const AuthoredScene& owner,
                                                   AssetCatalog& catalog, AssetImports& imports,
                                                   const ClientExportRequest& request) {
    return boundary<PreparedClientExport>("content.client-export.prepare", [&] {
        check_request(request);
        const auto capture = take(owner.capture(request.scene_revision));
        check_startup(capture, request.camera);
        take(imports.check_context(capture.project.project, request.catalog_revision,
                                   request.cache_revision));
        const auto root_ids = roots(capture.scene);
        const auto records = preflight(root_ids, capture.project.project, catalog, request);
        PreparedClientExport result{capture.project.project,
                                    capture.project.world,
                                    capture.scene.scene,
                                    capture.project.name,
                                    request,
                                    capture.dirty};
        result.scene_payload_ = take(prepare_scene(owner, request.scene_revision));
        result.scene_digest_ = take(sha256(result.scene_payload_));
        result.payload_bytes_ = result.scene_payload_.size();
        result.meshes_.reserve(root_ids.size());
        std::map<AssetId, AssetCacheKey> selected;
        std::size_t imported_bytes = 0;
        auto charge_source = [&](const AssetArtifact& artifact) {
            const auto record = records.find(artifact.asset);
            if (record == records.end())
                fail(ErrorCode::conflict,
                     "Cached dependency is absent from the captured metadata closure",
                     artifact.asset);
            if (selected.contains(artifact.asset)) {
                if (selected.at(artifact.asset) != artifact.key)
                    fail(ErrorCode::conflict, "Export roots disagree on a dependency generation",
                         artifact.asset);
                return false;
            }
            if (artifact.payload.size() > asset_import_maximum_total_bytes - imported_bytes)
                fail(ErrorCode::invalid_configuration, "Export cache closure exceeds 16 MiB",
                     artifact.asset);
            imported_bytes += artifact.payload.size();
            selected.emplace(artifact.asset, artifact.key);
            result.sources_.push_back({artifact.asset, artifact.key, record->second.metadata.origin,
                                       record->second.metadata.license});
            return true;
        };
        for (const auto root : root_ids) {
            const auto key = fresh(imports, root, request);
            auto artifact = take(imports.read(key, request.cache_revision));
            check_artifact(artifact, root, key, request);
            if (artifact.importer != "shark.gltf" || artifact.importer_version != 1 ||
                artifact.kind != "gltf" || artifact.payload_version != 1)
                fail(ErrorCode::capability_unavailable,
                     "Only typed glTF mesh artifacts can enter this export profile", root);
            auto payload = take(prepare_mesh_asset(root, artifact.payload));
            if (payload.size() > client_export_maximum_bytes - result.payload_bytes_)
                fail(ErrorCode::invalid_configuration,
                     "Prepared export exceeds 32 MiB of scene and mesh bytes", root);
            result.payload_bytes_ += payload.size();
            result.meshes_.push_back({root, key, take(sha256(payload)), std::move(payload)});
            std::vector<AssetArtifactDependency> pending;
            if (charge_source(artifact))
                pending = artifact.dependencies;
            while (!pending.empty()) {
                const auto dependency = pending.back();
                pending.pop_back();
                const auto known = selected.find(dependency.asset);
                if (known != selected.end()) {
                    if (known->second != dependency.key)
                        fail(ErrorCode::conflict,
                             "Export roots disagree on a dependency generation", dependency.asset);
                    continue;
                }
                auto child = take(imports.read(dependency.key, request.cache_revision));
                check_artifact(child, dependency.asset, dependency.key, request);
                if (charge_source(child))
                    pending.insert(pending.end(), child.dependencies.begin(),
                                   child.dependencies.end());
            }
        }
        if (selected.size() != records.size())
            fail(ErrorCode::conflict, "Cached and metadata export closures differ");
        std::sort(result.sources_.begin(), result.sources_.end(),
                  [](const auto& a, const auto& b) { return a.asset < b.asset; });
        std::vector<PreparedAssetInput> mesh_inputs;
        mesh_inputs.reserve(result.meshes_.size());
        for (const auto& mesh : result.meshes_)
            mesh_inputs.push_back({mesh.asset, mesh.payload});
        auto assets = take(PreparedAssets::create(mesh_inputs));
        // Checks every referenced mesh index, even when no draw would be visible.
        auto loaded = take(LoadedPreparedScene::load(result.scene_payload_, std::move(assets)));
        take(loaded->scene().find(request.camera));
        take(validate_client_export(result, owner, catalog, imports));
        return result;
    });
}

Result<void> validate_client_export(const PreparedClientExport& candidate,
                                    const AuthoredScene& owner, AssetCatalog& catalog,
                                    AssetImports& imports) {
    return boundary<void>("content.client-export.validate", [&]() -> Result<void> {
        if (candidate.scene_payload().empty())
            fail(ErrorCode::invalid_argument, "Export candidate is empty or has been moved from");
        const auto& request = candidate.request();
        check_request(request);
        const auto capture = take(owner.capture(request.scene_revision));
        check_startup(capture, request.camera);
        if (capture.project.project != candidate.project() ||
            capture.scene.scene != candidate.scene())
            fail(ErrorCode::conflict, "Export candidate belongs to a different project or scene");
        take(imports.check_context(candidate.project(), request.catalog_revision,
                                   request.cache_revision));
        const auto root_ids = roots(capture.scene);
        const auto records = preflight(root_ids, candidate.project(), catalog, request);
        if (root_ids.size() != candidate.meshes().size() ||
            records.size() != candidate.sources().size())
            fail(ErrorCode::conflict, "Export input inventory changed");
        for (const auto& mesh : candidate.meshes()) {
            if (!root_ids.contains(mesh.asset) ||
                fresh(imports, mesh.asset, request) != mesh.source_key)
                fail(ErrorCode::conflict, "Export mesh inputs changed since preparation",
                     mesh.asset);
        }
        for (const auto& source : candidate.sources()) {
            const auto record = records.find(source.asset);
            if (record == records.end() || record->second.metadata.origin != source.origin ||
                record->second.metadata.license != source.license)
                fail(ErrorCode::conflict, "Export attribution changed since preparation",
                     source.asset);
        }
        // No callback/source capture may leave a changed owner/revision behind.
        take(imports.check_context(candidate.project(), request.catalog_revision,
                                   request.cache_revision));
        take(owner.capture(request.scene_revision));
        return {};
    });
}
} // namespace shark::tools
