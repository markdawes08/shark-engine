#include "shark/tools/asset_import.hpp"

#include "asset_cache_format.hpp"
#include "shark/completion_queue.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
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
        failure.error.context.operation = std::string{operation};
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
    // The owning immutable FileRevision remains in this node across handoff.
    // Workers must never call ProjectFiles::revision_bytes (owner-thread only).
    std::string_view bytes;
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
bool terminal(AssetImportJobState state) {
    return state == AssetImportJobState::succeeded || state == AssetImportJobState::failed ||
           state == AssetImportJobState::cancelled;
}
struct JobControlGuard {
    bool& flag;
    bool previous;
    explicit JobControlGuard(bool& value) noexcept : flag(value), previous(value) {
        flag = true;
    }
    ~JobControlGuard() {
        flag = previous;
    }
};
void bound_error(Error& error) noexcept {
    // Only fixed known ASCII fallbacks survive an oversized callback diagnostic.
    // Avoid truncating an arbitrary UTF-8 sequence in the middle of a codepoint.
    auto bound = [](std::string& text) {
        // Standard strings may round a 1024-byte value's capacity upward.
        if (text.size() > 1024 || text.capacity() > 2048)
            std::string{}.swap(text);
    };
    bound(error.message);
    bound(error.context.operation);
    bound(error.context.request_id);
    bound(error.context.path);
    bound(error.context.entity_id);
    bound(error.context.asset_id);
}
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
    // All importer callbacks are serialized, including the legacy offline API.
    // Owner calls try-lock: cancellation/status must not wait behind a callback.
    mutable std::mutex callbacks;
    struct Token {
        std::size_t slot;
        std::uint64_t sequence;
    };
    struct Job {
        AssetImportJobStatus status;
        ContentDigest work_key;
        std::unique_ptr<PreparedAssetImport::Impl> plan;
        detail::CacheState cached;
        std::stop_source stop;
        std::atomic<std::size_t> completed{0};
        std::size_t source_bytes;
        std::size_t cached_bytes;
        Error fallback{ErrorCode::execution_failed,
                       "Asset import worker failed",
                       {.operation = "asset.jobs.worker"}};
        Job(AssetImportJobStatus value, ProjectId project)
            : status(std::move(value)), cached{project, {}, {}}, source_bytes(0), cached_bytes(0) {}
    };
    std::array<std::unique_ptr<Job>, asset_import_maximum_jobs> job_slots;
    mutable std::mutex jobs_mutex;
    std::condition_variable wake;
    CompletionQueue<Token, 1> requests;
    CompletionQueue<Token, 1> completions;
    std::optional<Token> active;
    std::uint64_t next_sequence{1};
    bool closing{false};
    bool publishing_job{false};
    std::jthread worker;

    Impl(ProjectFiles& source, AssetCatalog& scene_catalog, ProjectId id, StableId scope)
        : files(source), catalog(scene_catalog), project(id), revision{scope, 0},
          cache{id, {}, {}} {}
    ~Impl() {
        shutdown();
    }
    std::unique_lock<std::mutex> callback_access() const {
        std::unique_lock lock{callbacks, std::try_to_lock};
        if (!lock.owns_lock())
            fail(ErrorCode::conflict, "Importer worker is busy; drain or cancel its job first");
        return lock;
    }
    void check_owner() const {
        if (owner != std::this_thread::get_id())
            fail(ErrorCode::conflict, "Asset imports require their owner thread");
    }
    void check_job_access() const {
        check_owner();
        if (publishing_job)
            fail(ErrorCode::conflict, "Job control cannot be reentered from a callback");
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
    const RegisteredImporter& importer(const AssetMetadata& metadata, bool inspect = true) const {
        for (const auto& item : registry) {
            if (item.info.id == metadata.importer) {
                if (inspect && item.importer->info() != item.info)
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
    Inputs collect(AssetId root, const AssetTargetProfile& target, AssetCatalogRevision expected,
                   bool normalize = true) {
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
            const auto& registered = importer(record.metadata, false);
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
                                 {},
                                 {},
                                 {},
                                 {}};
            const auto bytes = files.revision_bytes(source);
            inputs.nodes.emplace(id, CapturedInput{std::move(record),
                                                   std::move(source),
                                                   bytes,
                                                   {},
                                                   registered.info,
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
            }
            colors[id] = 2;
            inputs.order.push_back(id);
            stack.pop_back();
        }
        check_catalog(expected);
        if (normalize)
            normalize_inputs(inputs);
        return inputs;
    }
    void normalize_inputs(Inputs& inputs) const {
        for (const auto id : inputs.order) {
            auto& node = inputs.nodes.at(id);
            const auto& registered = importer(node.record.metadata);
            auto settings =
                take(registered.importer->normalize_settings(node.record.metadata.settings_json));
            if (settings.size() > 4096)
                fail(ErrorCode::invalid_configuration, "Importer settings exceed 4096 bytes");
            node.settings = take(detail::canonical_import_object(settings));
            node.artifact.settings_digest = take(detail::hash_text(node.settings));
            node.artifact.dependencies.clear();
            for (const auto dependency : node.record.metadata.dependencies)
                node.artifact.dependencies.push_back(
                    {dependency, inputs.nodes.at(dependency).artifact.key});
            node.artifact.key = take(detail::import_key(project, node.artifact));
        }
    }
    void prepare_inputs(PreparedAssetImport::Impl& plan, const detail::CacheState& retained,
                        std::stop_token stop = {}, std::atomic<std::size_t>* progress = nullptr) {
        normalize_inputs(plan.inputs);
        std::size_t payload_bytes = 0;
        for (const auto id : plan.inputs.order) {
            if (stop.stop_requested())
                return;
            auto& node = plan.inputs.nodes.at(id);
            const auto existing =
                std::find_if(retained.artifacts.begin(), retained.artifacts.end(),
                             [&](const auto& a) { return a.key == node.artifact.key; });
            if (existing != retained.artifacts.end()) {
                validate_payload(*existing);
                node.artifact = *existing;
                ++plan.summary.reused;
            } else {
                std::vector<AssetImportDependency> dependencies;
                for (const auto& dependency : node.artifact.dependencies) {
                    const auto& child = plan.inputs.nodes.at(dependency.asset).artifact;
                    dependencies.push_back({child.asset, child.key, child.payload, child.kind,
                                            child.importer, child.importer_version,
                                            child.payload_version, child.source_digest});
                }
                const auto& registered = importer(node.record.metadata);
                AssetImportInput input{
                    id,
                    node.artifact.kind,
                    {reinterpret_cast<const std::byte*>(node.bytes.data()), node.bytes.size()},
                    node.settings,
                    plan.summary.target,
                    dependencies,
                    stop};
                auto payload = take(registered.importer->import(input));
                if (stop.stop_requested())
                    return;
                if (payload.empty() || payload.size() > asset_import_maximum_payload_bytes ||
                    payload.size() > asset_import_maximum_total_bytes - payload_bytes)
                    fail(ErrorCode::invalid_configuration, "Import payload byte bound exceeded");
                node.artifact.payload_digest = take(sha256(payload));
                node.artifact.payload = std::move(payload);
                take(registered.importer->validate_payload(node.artifact));
                ++plan.summary.imported;
            }
            if (node.artifact.payload.size() > asset_import_maximum_total_bytes - payload_bytes)
                fail(ErrorCode::invalid_configuration, "Import payload closure exceeds 16 MiB");
            payload_bytes += node.artifact.payload.size();
            if (progress)
                progress->fetch_add(1);
        }
        plan.summary.root_key = plan.inputs.nodes.at(plan.summary.root).artifact.key;
    }
    Job& find_job(AssetImportJobId id) const {
        if (id.scope != revision.scope)
            fail(ErrorCode::conflict, "Job belongs to another import owner");
        for (const auto& job : job_slots)
            if (job && job->status.id == id)
                return *job;
        fail(ErrorCode::invalid_id, "Job is unknown or already taken; reconcile before retrying");
    }
    AssetImportJobStatus status(const Job& job) const {
        auto result = job.status;
        result.completed_nodes = job.completed.load();
        return result;
    }
    void dispatch() {
        // Called on the owner with jobs_mutex held. Only one job is in flight;
        // its completion is consumed/published before another callback can start.
        if (closing || active)
            return;
        std::optional<Token> next;
        for (std::size_t index = 0; index < job_slots.size(); ++index) {
            const auto& job = job_slots[index];
            if (job && job->status.state == AssetImportJobState::queued &&
                (!next || job->status.id.sequence < next->sequence))
                next = Token{index, job->status.id.sequence};
        }
        if (!next)
            return;
        active = next;
        Token token = *next;
        if (requests.try_push(std::move(token)) != CompletionPush::accepted)
            std::terminate();
        wake.notify_one();
    }
    void run() noexcept {
        try {
            for (;;) {
                Token token{};
                Job* job = nullptr;
                {
                    std::unique_lock lock{jobs_mutex};
                    wake.wait(lock, [&] { return closing || requests.size() != 0; });
                    const auto next = requests.try_pop();
                    if (!next)
                        return;
                    token = *next;
                    job = job_slots[token.slot].get();
                    job->status.state = AssetImportJobState::running;
                }
                auto result = boundary<void>("asset.jobs.worker", [&] {
                    const std::lock_guard lock{callbacks};
                    if (!job->stop.stop_requested())
                        prepare_inputs(*job->plan, job->cached, job->stop.get_token(),
                                       &job->completed);
                });
                {
                    const std::lock_guard lock{jobs_mutex};
                    if (!result) {
                        auto error = std::move(result).error();
                        bound_error(error);
                        job->status.error = std::move(error);
                    }
                    job->status.state = AssetImportJobState::ready;
                    if (completions.try_push(std::move(token)) != CompletionPush::accepted)
                        std::terminate();
                }
            }
        } catch (...) {
            // Synchronization failures are exceptional; preserve a preallocated
            // diagnostic rather than allowing an exception out of thread entry.
            try {
                const std::lock_guard lock{jobs_mutex};
                if (active) {
                    auto& job = *job_slots[active->slot];
                    job.status.error = std::move(job.fallback);
                    job.status.state = AssetImportJobState::ready;
                    Token token = *active;
                    if (completions.size() == 0 &&
                        completions.try_push(std::move(token)) != CompletionPush::accepted)
                        std::terminate();
                }
                closing = true;
            } catch (...) {
                std::terminate();
            }
        }
    }
    void shutdown() noexcept {
        try {
            JobControlGuard guard{publishing_job};
            std::array<std::optional<std::stop_source>, asset_import_maximum_jobs> stops;
            {
                const std::lock_guard lock{jobs_mutex};
                closing = true;
                requests.close();
                for (std::size_t index = 0; index < job_slots.size(); ++index)
                    if (job_slots[index] && !terminal(job_slots[index]->status.state))
                        stops[index] = job_slots[index]->stop;
            }
            for (auto& stop : stops)
                if (stop)
                    stop->request_stop();
            wake.notify_all();
            if (worker.joinable()) {
                worker.request_stop();
                worker.join();
            }
            completions.close();
            while (completions.try_pop()) {
            }
            const std::lock_guard lock{jobs_mutex};
            active.reset();
            for (auto& job : job_slots)
                if (job && !terminal(job->status.state)) {
                    job->status.state = AssetImportJobState::cancelled;
                    job->status.error.reset();
                    job->plan.reset();
                    job->cached.artifacts.clear();
                    job->source_bytes = 0;
                    job->cached_bytes = 0;
                }
        } catch (...) {
            std::terminate();
        }
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
        const auto access = impl_->callback_access();
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
Result<void> AssetImports::check_context(ProjectId expected_project,
                                         AssetCatalogRevision expected_catalog,
                                         AssetCacheRevision expected_cache) const {
    return boundary<void>("asset.cache.context", [&] {
        impl_->check(expected_cache);
        impl_->check_catalog(expected_catalog);
        const auto access = impl_->callback_access();
        if (impl_->project != expected_project)
            fail(ErrorCode::conflict, "Asset cache belongs to another project");
    });
}
Result<PreparedAssetImport> AssetImports::prepare(AssetId root, const AssetTargetProfile& target,
                                                  AssetCatalogRevision expected_catalog,
                                                  AssetCacheRevision expected_cache) {
    return boundary<PreparedAssetImport>("asset.import.prepare", [&] {
        impl_->check(expected_cache);
        const auto access = impl_->callback_access();
        auto inputs = impl_->collect(root, target, expected_catalog, false);
        auto plan = std::make_unique<PreparedAssetImport::Impl>(PreparedAssetImport::Impl{
            {expected_cache, expected_catalog, root, target, {}, inputs.nodes.size(), 0, 0},
            std::move(inputs)});
        impl_->prepare_inputs(*plan, impl_->cache);
        return PreparedAssetImport(std::move(plan));
    });
}
Result<AssetCacheCommit> AssetImports::publish(PreparedAssetImport&& prepared,
                                               AssetCatalogRevision expected_catalog,
                                               AssetCacheRevision expected_cache) {
    return boundary<AssetCacheCommit>("asset.import.publish", [&] {
        impl_->check(expected_cache);
        impl_->check_catalog(expected_catalog);
        const auto access = impl_->callback_access();
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
        const auto access = impl_->callback_access();
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
        const auto access = impl_->callback_access();
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
Result<AssetImportJobAdmission> AssetImports::submit(AssetId root, const AssetTargetProfile& target,
                                                     AssetCatalogRevision expected_catalog,
                                                     AssetCacheRevision expected_cache) {
    return boundary<AssetImportJobAdmission>("asset.jobs.submit", [&] {
        impl_->check_job_access();
        impl_->check(expected_cache);
        {
            const std::lock_guard lock{impl_->jobs_mutex};
            if (impl_->closing)
                fail(ErrorCode::conflict, "Job admission is closed; create a new import owner");
        }
        // This path invokes no importer callbacks and is safe while the private
        // worker processes an earlier immutable snapshot.
        auto inputs = impl_->collect(root, target, expected_catalog, false);
        std::string identity = "shark.asset.job.v1";
        auto append = [&](std::string_view text) {
            identity += std::to_string(text.size());
            identity += ':';
            identity += text;
        };
        append(root.value().to_string());
        append(target.id);
        append(std::to_string(static_cast<int>(target.side)));
        append(expected_catalog.scope.to_string());
        append(std::to_string(expected_catalog.value));
        append(expected_cache.scope.to_string());
        append(std::to_string(expected_cache.value));
        std::size_t source_bytes = 0;
        for (const auto& [id, node] : inputs.nodes) {
            append(id.value().to_string());
            append(node.record.source.text());
            append(node.record.sidecar.text());
            append(node.artifact.source_digest.to_hex());
            append(
                take(detail::hash_text(impl_->files.revision_bytes(node.record.metadata_revision)))
                    .to_hex());
            source_bytes += node.source.byte_count();
        }
        const auto work_key = take(detail::hash_text(identity));
        std::size_t cached_bytes = 0;
        for (const auto& artifact : impl_->cache.artifacts)
            cached_bytes += artifact.payload.size();
        const std::lock_guard lock{impl_->jobs_mutex};
        std::size_t retained_source = 0;
        std::size_t retained_cached = 0;
        std::optional<std::size_t> free;
        for (std::size_t index = 0; index < impl_->job_slots.size(); ++index) {
            const auto& job = impl_->job_slots[index];
            if (!job) {
                if (!free)
                    free = index;
                continue;
            }
            if (!terminal(job->status.state) && !job->stop.stop_requested() &&
                job->work_key == work_key)
                return AssetImportJobAdmission{job->status.id, true};
            retained_source += job->source_bytes;
            retained_cached += job->cached_bytes;
        }
        if (!free || source_bytes > asset_import_jobs_maximum_source_bytes - retained_source ||
            cached_bytes > asset_import_jobs_maximum_cached_bytes - retained_cached)
            fail(ErrorCode::conflict, "Import job capacity is full; drain and take retained jobs");
        if (impl_->next_sequence == std::numeric_limits<std::uint64_t>::max())
            fail(ErrorCode::conflict, "Job sequence exhausted; create a new import owner");
        const AssetImportJobId id{impl_->revision.scope, impl_->next_sequence};
        auto job = std::make_unique<Impl::Job>(
            AssetImportJobStatus{
                id, AssetImportJobState::queued, 0, inputs.nodes.size(), {}, {}, {}},
            impl_->project);
        job->work_key = work_key;
        job->source_bytes = source_bytes;
        job->cached_bytes = cached_bytes;
        job->cached = impl_->cache;
        job->plan = std::make_unique<PreparedAssetImport::Impl>(PreparedAssetImport::Impl{
            {expected_cache, expected_catalog, root, target, {}, inputs.nodes.size(), 0, 0},
            std::move(inputs)});
        if (!impl_->worker.joinable())
            impl_->worker = std::jthread{[state = impl_.get()] { state->run(); }};
        impl_->job_slots[*free] = std::move(job);
        ++impl_->next_sequence;
        impl_->dispatch();
        return AssetImportJobAdmission{id, false};
    });
}
Result<std::vector<AssetImportJobStatus>> AssetImports::jobs() const {
    return boundary<std::vector<AssetImportJobStatus>>("asset.jobs.query", [&] {
        impl_->check_job_access();
        const std::lock_guard lock{impl_->jobs_mutex};
        std::vector<AssetImportJobStatus> result;
        for (const auto& job : impl_->job_slots)
            if (job)
                result.push_back(impl_->status(*job));
        std::sort(result.begin(), result.end(),
                  [](const auto& a, const auto& b) { return a.id.sequence < b.id.sequence; });
        return result;
    });
}
Result<AssetImportJobStatus> AssetImports::cancel_job(AssetImportJobId id) {
    return boundary<AssetImportJobStatus>("asset.jobs.cancel", [&] {
        impl_->check_job_access();
        JobControlGuard guard{impl_->publishing_job};
        std::unique_lock lock{impl_->jobs_mutex};
        auto& job = impl_->find_job(id);
        if (!terminal(job.status.state)) {
            auto stop = job.stop;
            lock.unlock();
            stop.request_stop();
            lock.lock();
            if (job.status.state == AssetImportJobState::queued &&
                (!impl_->active || impl_->active->sequence != id.sequence)) {
                job.status.state = AssetImportJobState::cancelled;
                job.plan.reset();
                job.cached.artifacts.clear();
                job.source_bytes = 0;
                job.cached_bytes = 0;
            }
        }
        return impl_->status(job);
    });
}
Result<std::size_t> AssetImports::drain_jobs() {
    return boundary<std::size_t>("asset.jobs.drain", [&] {
        impl_->check_job_access();
        const std::lock_guard lock{impl_->jobs_mutex};
        const auto token = impl_->completions.try_pop();
        if (!token)
            return std::size_t{0};
        auto& job = *impl_->job_slots[token->slot];
        if (job.stop.stop_requested()) {
            job.status.state = AssetImportJobState::cancelled;
            job.status.error.reset();
        } else if (job.status.error) {
            job.status.state = AssetImportJobState::failed;
        } else {
            // Failure to allocate a reply must also consume this completion.
            auto committed = boundary<AssetCacheCommit>("asset.jobs.publish", [&] {
                job.status.summary = job.plan->summary;
                const auto catalog = job.plan->summary.catalog_revision;
                const auto cache = job.plan->summary.cache_revision;
                PreparedAssetImport prepared{std::move(job.plan)};
                JobControlGuard guard{impl_->publishing_job};
                return take(publish(std::move(prepared), catalog, cache));
            });
            if (committed) {
                job.status.commit = std::move(committed).value();
                job.status.state = AssetImportJobState::succeeded;
            } else {
                auto error = std::move(committed).error();
                bound_error(error);
                job.status.error = std::move(error);
                job.status.state = AssetImportJobState::failed;
            }
        }
        job.plan.reset();
        job.cached.artifacts.clear();
        job.source_bytes = 0;
        job.cached_bytes = 0;
        impl_->active.reset();
        impl_->dispatch();
        return std::size_t{1};
    });
}
Result<AssetImportJobStatus> AssetImports::take_job(AssetImportJobId id) {
    return boundary<AssetImportJobStatus>("asset.jobs.take", [&] {
        impl_->check_job_access();
        const std::lock_guard lock{impl_->jobs_mutex};
        auto& job = impl_->find_job(id);
        if (!terminal(job.status.state))
            fail(ErrorCode::conflict, "Job is not terminal; drain its completion before taking");
        auto result = impl_->status(job);
        for (auto& slot : impl_->job_slots)
            if (slot.get() == &job) {
                slot.reset();
                break;
            }
        return result;
    });
}
Result<void> AssetImports::close_jobs() {
    return boundary<void>("asset.jobs.close", [&] {
        impl_->check_job_access();
        impl_->shutdown();
    });
}
Result<std::size_t> AssetImports::recover(ProjectFiles& files) {
    return boundary<std::size_t>("asset.cache.recover",
                                 [&] { return take(files.cache_stages(true)); });
}

} // namespace shark::tools
