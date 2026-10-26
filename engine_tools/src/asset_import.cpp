#include "shark/tools/asset_import.hpp"

#include "asset_cache_format.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

namespace shark::tools {
namespace {
struct Failure {
    Error error;
};
[[noreturn]] void fail(ErrorCode code, const char* message, std::string path = {},
                       std::string asset = {}) {
    throw Failure{Error{code, message, {.path = std::move(path), .asset_id = std::move(asset)}}};
}
template <typename T> T take(Result<T> result) {
    if (!result)
        throw Failure{std::move(result).error()};
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result)
        throw Failure{std::move(result).error()};
}
template <typename T, typename Function>
Result<T> boundary(const char* operation, Function&& function) {
    try {
        if constexpr (std::is_void_v<std::invoke_result_t<Function>>) {
            function();
            return {};
        } else
            return function();
    } catch (Failure& failure) {
        failure.error.context.operation = operation;
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed, "Asset import operation failed", {operation}};
    }
}
bool same_binding(const detail::CacheBinding& b, AssetId asset, const AssetTargetProfile& target) {
    return b.asset == asset && b.target == target;
}
void sort_state(detail::CacheState& state) {
    std::sort(state.artifacts.begin(), state.artifacts.end(),
              [](const auto& a, const auto& b) { return a.key < b.key; });
    std::sort(state.bindings.begin(), state.bindings.end(), [](const auto& a, const auto& b) {
        return std::tie(a.asset, a.target.id, a.target.side) <
               std::tie(b.asset, b.target.id, b.target.side);
    });
}
struct CapturedInput {
    AssetRecord record;
    FileRevision source;
    std::string settings;
    AssetImporterInfo importer;
    AssetArtifact artifact;
};
struct Inputs {
    std::map<AssetId, CapturedInput> nodes;
    std::vector<AssetId> order;
};
struct RegisteredImporter {
    AssetImporter* importer;
    AssetImporterInfo info;
};
} // namespace

struct PreparedAssetImport::Impl {
    AssetImportSummary summary;
    Inputs inputs;
};
PreparedAssetImport::PreparedAssetImport(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
PreparedAssetImport::~PreparedAssetImport() = default;
PreparedAssetImport::PreparedAssetImport(PreparedAssetImport&&) noexcept = default;
PreparedAssetImport& PreparedAssetImport::operator=(PreparedAssetImport&&) noexcept = default;
Result<AssetImportSummary> PreparedAssetImport::summary() const {
    return boundary<AssetImportSummary>("asset.import.summary", [&] {
        if (!impl_)
            fail(ErrorCode::invalid_argument, "Prepared import is empty or consumed");
        return impl_->summary;
    });
}

struct AssetImports::Impl {
    ProjectFiles& files;
    AssetCatalog& catalog;
    ProjectId project;
    std::thread::id owner{std::this_thread::get_id()};
    AssetCacheRevision revision;
    std::optional<FileRevision> file;
    detail::CacheState cache;
    std::vector<RegisteredImporter> registry;

    Impl(ProjectFiles& source, AssetCatalog& scene_catalog, ProjectId id, StableId scope)
        : files(source), catalog(scene_catalog), project(id), revision{scope, 0},
          cache{id, {}, {}} {}
    void check_owner() const {
        if (owner != std::this_thread::get_id())
            fail(ErrorCode::conflict, "Asset imports require their owner thread");
    }
    void check(AssetCacheRevision expected, bool disk = true) const {
        check_owner();
        if (revision != expected)
            fail(ErrorCode::conflict, "Cache revision or owner scope changed");
        if (disk && take(files.read_development_cache()) != file) {
            fail(ErrorCode::conflict, "Cache file changed; reload or reconcile before retrying",
                 std::string{asset_cache_filename});
        }
    }
    void check_catalog(AssetCatalogRevision expected) const {
        if (take(catalog.revision()) != expected)
            fail(ErrorCode::conflict, "Asset catalog revision or owner changed");
    }
    void can_advance() const {
        if (revision.value == std::numeric_limits<std::uint64_t>::max())
            fail(ErrorCode::conflict, "Cache revision exhausted; reopen explicitly");
    }
    const RegisteredImporter& importer(const AssetMetadata& metadata) const {
        for (const auto& item : registry) {
            if (item.info.id == metadata.importer) {
                if (item.importer->info() != item.info)
                    fail(ErrorCode::conflict, "Importer descriptor changed; create a new service");
                if (item.info.kind != metadata.kind)
                    fail(ErrorCode::invalid_configuration, "Importer does not support this kind",
                         {}, metadata.asset.value().to_string());
                return item;
            }
        }
        fail(ErrorCode::capability_unavailable, "No registered importer for the asset", {},
             metadata.asset.value().to_string());
    }
    const AssetArtifact* artifact(AssetCacheKey key) const {
        const auto found = std::find_if(cache.artifacts.begin(), cache.artifacts.end(),
                                        [&](const auto& a) { return a.key == key; });
        return found == cache.artifacts.end() ? nullptr : &*found;
    }
    std::optional<AssetCacheKey> binding(AssetId asset, const AssetTargetProfile& target) const {
        for (const auto& b : cache.bindings)
            if (same_binding(b, asset, target))
                return b.key;
        return {};
    }
    void validate_payload(const AssetArtifact& artifact_value) const {
        for (const auto& item : registry) {
            if (item.info.id == artifact_value.importer &&
                item.info.version == artifact_value.importer_version &&
                item.info.payload_version == artifact_value.payload_version) {
                if (item.importer->info() != item.info)
                    fail(ErrorCode::conflict, "Importer descriptor changed");
                take(item.importer->validate_payload(artifact_value));
            }
        }
    }
    Inputs collect(AssetId root, const AssetTargetProfile& target, AssetCatalogRevision expected) {
        check_catalog(expected);
        take(detail::validate_target(target));
        Inputs inputs;
        std::map<AssetId, unsigned> colors;
        std::size_t source_bytes = 0;
        auto capture = [&](AssetId id) {
            if (inputs.nodes.size() >= asset_import_maximum_nodes) {
                fail(ErrorCode::invalid_configuration,
                     "Import dependency closure exceeds 128 nodes");
            }
            auto resolved = take(catalog.resolve(id, expected));
            if (!resolved.record)
                fail(ErrorCode::invalid_id, "Asset dependency is missing from the catalog", {},
                     id.value().to_string());
            auto record = std::move(*resolved.record);
            if (record.metadata.project != project)
                fail(ErrorCode::conflict, "Asset belongs to another project");
            auto meta = take(files.read_asset_metadata(record.sidecar));
            if (meta.revision != record.metadata_revision)
                fail(ErrorCode::conflict, "Asset metadata changed; refresh the catalog",
                     record.sidecar.text(), id.value().to_string());
            if (record.metadata.audience != AssetAudience::shared &&
                record.metadata.audience != target.side) {
                fail(ErrorCode::invalid_configuration, "Asset is private to the other content side",
                     record.sidecar.text(), id.value().to_string());
            }
            const auto& registered = importer(record.metadata);
            auto settings =
                take(registered.importer->normalize_settings(record.metadata.settings_json));
            if (settings.size() > 4096)
                fail(ErrorCode::invalid_configuration, "Importer settings exceed 4096 bytes");
            settings = take(detail::canonical_import_object(settings));
            auto source = take(files.read_asset_bytes(record.source));
            if (source.byte_count() > asset_import_maximum_total_bytes - source_bytes)
                fail(ErrorCode::invalid_configuration, "Import source closure exceeds 16 MiB");
            source_bytes += source.byte_count();
            AssetArtifact output{id,
                                 {},
                                 record.metadata.kind,
                                 record.metadata.audience,
                                 target,
                                 registered.info.id,
                                 registered.info.version,
                                 registered.info.payload_version,
                                 take(detail::hash_text(files.revision_bytes(source))),
                                 take(detail::hash_text(settings)),
                                 {},
                                 {},
                                 {}};
            inputs.nodes.emplace(id, CapturedInput{std::move(record), std::move(source),
                                                   std::move(settings), registered.info,
                                                   std::move(output)});
            colors[id] = 1;
        };
        struct Frame {
            AssetId id;
            std::size_t next;
        };
        std::vector<Frame> stack;
        stack.reserve(asset_import_maximum_nodes);
        capture(root);
        stack.push_back({root, 0});
        while (!stack.empty()) {
            auto& frame = stack.back();
            const auto id = frame.id;
            auto& node = inputs.nodes.at(id);
            if (frame.next < node.record.metadata.dependencies.size()) {
                const auto dependency = node.record.metadata.dependencies[frame.next++];
                if (colors[dependency] == 1)
                    fail(ErrorCode::invalid_configuration, "Asset dependency cycle", {},
                         dependency.value().to_string());
                if (colors[dependency] == 0) {
                    capture(dependency);
                    stack.push_back({dependency, 0});
                }
                continue;
            }
            for (const auto& dependency : node.record.metadata.dependencies) {
                const auto& child = inputs.nodes.at(dependency);
                if (node.record.metadata.audience == AssetAudience::shared &&
                    child.record.metadata.audience != AssetAudience::shared) {
                    fail(ErrorCode::invalid_configuration,
                         "Shared content cannot depend on side-private content",
                         node.record.sidecar.text(), id.value().to_string());
                }
                node.artifact.dependencies.push_back({dependency, child.artifact.key});
            }
            node.artifact.key = take(detail::import_key(project, node.artifact));
            colors[id] = 2;
            inputs.order.push_back(id);
            stack.pop_back();
        }
        check_catalog(expected);
        return inputs;
    }
    AssetCacheCommit commit(detail::CacheState candidate) {
        sort_state(candidate);
        if (candidate == cache)
            return {revision, false, {}};
        can_advance();
        auto encoded = take(detail::encode_cache(candidate));
        // All allocations/validation precede native publication.
        auto committed = take(files.write_development_cache(std::move(encoded), file));
        file = committed.revision;
        cache.artifacts.swap(candidate.artifacts);
        cache.bindings.swap(candidate.bindings);
        ++revision.value;
        return {revision, true, std::move(committed)};
    }
};

AssetImports::AssetImports(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
AssetImports::~AssetImports() = default;
Result<std::unique_ptr<AssetImports>>
AssetImports::open(ProjectFiles& files, AssetCatalog& catalog, ProjectId project,
                   std::span<AssetImporter* const> importers_value) {
    return boundary<std::unique_ptr<AssetImports>>("asset.cache.open", [&] {
        if (importers_value.size() > 16)
            fail(ErrorCode::invalid_argument, "Importer registry limit is sixteen");
        auto impl = std::make_unique<Impl>(files, catalog, project, take(generate_stable_id()));
        static_cast<void>(take(catalog.revision()));
        std::set<std::string> ids;
        for (auto* importer : importers_value) {
            if (!importer)
                fail(ErrorCode::invalid_argument, "Null importer");
            const auto& info = importer->info();
            take(detail::validate_importer_info(info));
            if (!ids.insert(info.id).second)
                fail(ErrorCode::invalid_argument, "Duplicate importer identifier");
            impl->registry.push_back({importer, info});
        }
        impl->file = take(files.read_development_cache());
        if (impl->file)
            impl->cache = take(detail::decode_cache(files.revision_bytes(*impl->file), project));
        return std::unique_ptr<AssetImports>(new AssetImports(std::move(impl)));
    });
}
Result<std::vector<AssetImporterInfo>> AssetImports::importers() const {
    return boundary<std::vector<AssetImporterInfo>>("asset.importers", [&] {
        impl_->check_owner();
        std::vector<AssetImporterInfo> result;
        for (const auto& item : impl_->registry) {
            if (item.importer->info() != item.info)
                fail(ErrorCode::conflict, "Importer descriptor changed");
            result.push_back(item.info);
        }
        return result;
    });
}
Result<AssetCacheRevision> AssetImports::revision() const {
    return boundary<AssetCacheRevision>("asset.cache.revision", [&] {
        impl_->check_owner();
        return impl_->revision;
    });
}
Result<PreparedAssetImport> AssetImports::prepare(AssetId root, const AssetTargetProfile& target,
                                                  AssetCatalogRevision expected_catalog,
                                                  AssetCacheRevision expected_cache) {
    return boundary<PreparedAssetImport>("asset.import.prepare", [&] {
        impl_->check(expected_cache);
        auto inputs = impl_->collect(root, target, expected_catalog);
        std::size_t imported = 0;
        std::size_t reused = 0;
        std::size_t payload_bytes = 0;
        for (const auto id : inputs.order) {
            auto& node = inputs.nodes.at(id);
            if (const auto* existing = impl_->artifact(node.artifact.key)) {
                impl_->validate_payload(*existing);
                node.artifact = *existing;
                ++reused;
            } else {
                std::vector<AssetImportDependency> dependencies;
                for (const auto& dependency : node.artifact.dependencies) {
                    const auto& child = inputs.nodes.at(dependency.asset).artifact;
                    dependencies.push_back({child.asset, child.key, child.payload});
                }
                const auto& registered = impl_->importer(node.record.metadata);
                const auto source = impl_->files.revision_bytes(node.source);
                AssetImportInput input{
                    id,
                    node.artifact.kind,
                    {reinterpret_cast<const std::byte*>(source.data()), source.size()},
                    node.settings,
                    target,
                    dependencies};
                auto payload = take(registered.importer->import(input));
                if (payload.empty() || payload.size() > asset_import_maximum_payload_bytes ||
                    payload.size() > asset_import_maximum_total_bytes - payload_bytes)
                    fail(ErrorCode::invalid_configuration, "Import payload byte bound exceeded");
                node.artifact.payload_digest = take(sha256(payload));
                node.artifact.payload = std::move(payload);
                take(registered.importer->validate_payload(node.artifact));
                ++imported;
            }
            if (node.artifact.payload.size() > asset_import_maximum_total_bytes - payload_bytes)
                fail(ErrorCode::invalid_configuration, "Import payload closure exceeds 16 MiB");
            payload_bytes += node.artifact.payload.size();
        }
        const auto key = inputs.nodes.at(root).artifact.key;
        AssetImportSummary summary{expected_cache,      expected_catalog, root,  target, key,
                                   inputs.nodes.size(), imported,         reused};
        return PreparedAssetImport(std::make_unique<PreparedAssetImport::Impl>(
            PreparedAssetImport::Impl{std::move(summary), std::move(inputs)}));
    });
}
Result<AssetCacheCommit> AssetImports::publish(PreparedAssetImport&& prepared,
                                               AssetCatalogRevision expected_catalog,
                                               AssetCacheRevision expected_cache) {
    return boundary<AssetCacheCommit>("asset.import.publish", [&] {
        impl_->check(expected_cache);
        impl_->check_catalog(expected_catalog);
        if (!prepared.impl_ || prepared.impl_->summary.cache_revision != expected_cache ||
            prepared.impl_->summary.catalog_revision != expected_catalog)
            fail(ErrorCode::conflict, "Prepared import belongs to another scope/revision");
        auto plan = std::move(prepared.impl_);
        auto candidate = impl_->cache;
        for (const auto id : plan->inputs.order) {
            const auto& node = plan->inputs.nodes.at(id);
            if (take(impl_->files.read_asset_metadata(node.record.sidecar)).revision !=
                    node.record.metadata_revision ||
                take(impl_->files.read_asset_bytes(node.record.source)) != node.source) {
                fail(ErrorCode::conflict, "Source/metadata changed before import publication",
                     node.record.source.text(), id.value().to_string());
            }
            const auto& registered = impl_->importer(node.record.metadata);
            if (registered.info != node.importer ||
                take(detail::canonical_import_object(take(registered.importer->normalize_settings(
                    node.record.metadata.settings_json)))) != node.settings) {
                fail(ErrorCode::conflict, "Importer/settings changed before publication");
            }
            const auto existing =
                std::find_if(candidate.artifacts.begin(), candidate.artifacts.end(),
                             [&](const auto& a) { return a.key == node.artifact.key; });
            if (existing != candidate.artifacts.end()) {
                if (*existing != node.artifact)
                    fail(ErrorCode::conflict, "Matching cache key produced different output");
            } else {
                if (candidate.artifacts.size() == asset_cache_maximum_entries)
                    fail(ErrorCode::invalid_configuration,
                         "Cache is full; explicitly prune or clear");
                candidate.artifacts.push_back(node.artifact);
            }
            auto binding = std::find_if(
                candidate.bindings.begin(), candidate.bindings.end(),
                [&](const auto& b) { return same_binding(b, id, plan->summary.target); });
            if (binding == candidate.bindings.end()) {
                if (candidate.bindings.size() == asset_cache_maximum_entries)
                    fail(ErrorCode::invalid_configuration,
                         "Cache bindings are full; explicitly clear");
                candidate.bindings.push_back({id, plan->summary.target, node.artifact.key});
            } else
                binding->key = node.artifact.key;
        }
        impl_->check_catalog(expected_catalog);
        return impl_->commit(std::move(candidate));
    });
}
Result<AssetCacheStatus> AssetImports::query(AssetId asset, const AssetTargetProfile& target,
                                             AssetCatalogRevision expected_catalog,
                                             AssetCacheRevision expected_cache) {
    return boundary<AssetCacheStatus>("asset.cache.query", [&] {
        impl_->check(expected_cache);
        impl_->check_catalog(expected_catalog);
        take(detail::validate_target(target));
        const auto last = impl_->binding(asset, target);
        AssetCacheStatus status{expected_cache,
                                asset,
                                target,
                                last ? AssetCacheState::stale : AssetCacheState::missing,
                                {},
                                last,
                                {}};
        try {
            const auto inputs = impl_->collect(asset, target, expected_catalog);
            status.required_key = inputs.nodes.at(asset).artifact.key;
            if (last && *last == *status.required_key) {
                impl_->validate_payload(*impl_->artifact(*last));
                status.state = AssetCacheState::fresh;
            }
        } catch (Failure& failure) {
            failure.error.context.operation = "asset.cache.query.inputs";
            status.blocked_by = std::move(failure.error);
        }
        return status;
    });
}
Result<AssetArtifact> AssetImports::read(AssetCacheKey key, AssetCacheRevision expected) const {
    return boundary<AssetArtifact>("asset.cache.read", [&] {
        impl_->check(expected);
        const auto* artifact = impl_->artifact(key);
        if (!artifact)
            fail(ErrorCode::invalid_argument, "Cached artifact key is missing");
        impl_->validate_payload(*artifact);
        return *artifact;
    });
}
Result<AssetCacheUsage> AssetImports::usage(AssetCacheRevision expected) const {
    return boundary<AssetCacheUsage>("asset.cache.usage", [&] {
        impl_->check(expected);
        std::size_t bytes = 0;
        for (const auto& artifact : impl_->cache.artifacts)
            bytes += artifact.payload.size();
        return AssetCacheUsage{expected,
                               impl_->cache.artifacts.size(),
                               impl_->cache.bindings.size(),
                               bytes,
                               impl_->file ? impl_->file->byte_count() : 0,
                               take(impl_->files.cache_stages(false)) != 0};
    });
}
Result<AssetCacheRevision> AssetImports::reload(AssetCacheRevision expected) {
    return boundary<AssetCacheRevision>("asset.cache.reload", [&] {
        impl_->check(expected, false);
        auto file = take(impl_->files.read_development_cache());
        if (file == impl_->file)
            return impl_->revision;
        auto state =
            file ? take(detail::decode_cache(impl_->files.revision_bytes(*file), impl_->project))
                 : detail::CacheState{impl_->project, {}, {}};
        impl_->can_advance();
        impl_->file = std::move(file);
        impl_->cache.artifacts.swap(state.artifacts);
        impl_->cache.bindings.swap(state.bindings);
        ++impl_->revision.value;
        return impl_->revision;
    });
}
Result<AssetCacheCommit> AssetImports::prune(AssetCacheRevision expected) {
    return boundary<AssetCacheCommit>("asset.cache.prune", [&] {
        impl_->check(expected);
        std::set<AssetCacheKey> retained;
        auto visit = [&](auto&& self, AssetCacheKey key) -> void {
            if (!retained.insert(key).second)
                return;
            const auto* artifact = impl_->artifact(key);
            if (!artifact)
                fail(ErrorCode::invalid_configuration, "Missing retained cache dependency");
            for (const auto& dependency : artifact->dependencies)
                self(self, dependency.key);
        };
        for (const auto& binding : impl_->cache.bindings)
            visit(visit, binding.key);
        auto candidate = impl_->cache;
        std::erase_if(candidate.artifacts,
                      [&](const auto& a) { return !retained.contains(a.key); });
        return impl_->commit(std::move(candidate));
    });
}
Result<AssetCacheCommit> AssetImports::clear(AssetCacheRevision expected) {
    return boundary<AssetCacheCommit>("asset.cache.clear", [&] {
        impl_->check(expected);
        return impl_->commit({impl_->project, {}, {}});
    });
}
Result<std::size_t> AssetImports::recover(ProjectFiles& files) {
    return boundary<std::size_t>("asset.cache.recover",
                                 [&] { return take(files.cache_stages(true)); });
}

} // namespace shark::tools
