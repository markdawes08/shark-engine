#include "shark/tools/asset_import.hpp"

#include "project_files_native.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace {
using namespace shark;
using namespace shark::tools;
template <typename T> T take(Result<T> result) {
    if (!result)
        throw std::runtime_error{std::string{error_code_name(result.error().code)} + ":" +
                                 result.error().context.operation};
    return std::move(result).value();
}
template <typename T> T id(char digit) {
    std::string text = "50000000-0000-4000-8000-000000000001";
    text.back() = digit;
    return T{take(StableId::parse(text))};
}
void write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (!out)
        throw std::runtime_error{"source fixture write failed"};
}
detail::FilePhase phase(std::string_view value) {
    const std::array phases{std::pair{"staged", detail::FilePhase::staged},
                            std::pair{"written", detail::FilePhase::written},
                            std::pair{"flushed", detail::FilePhase::flushed},
                            std::pair{"before_publish", detail::FilePhase::before_publish},
                            std::pair{"published", detail::FilePhase::published}};
    for (const auto& [name, selected] : phases)
        if (value == name)
            return selected;
    throw std::runtime_error{"unknown phase"};
}

using Clock = std::chrono::steady_clock;
constexpr auto job_deadline = std::chrono::seconds{15};

// The owner writes its observation marker before releasing this gate. Abrupt
// process exit runs on the actual importer worker, without a project writer.
class ExitImporter final : public AssetImporter {
  public:
    explicit ExitImporter(bool exit_running) : exit_running_(exit_running) {}
    const AssetImporterInfo& info() const noexcept override {
        return bytes_.info();
    }
    Result<std::string> normalize_settings(std::string_view text) const override {
        return bytes_.normalize_settings(text);
    }
    Result<void> validate_payload(const AssetArtifact& artifact) const override {
        return bytes_.validate_payload(artifact);
    }
    Result<std::vector<std::byte>> import(const AssetImportInput& input) override {
        if (exit_running_ && input.asset == id<AssetId>('b')) {
            std::unique_lock lock{mutex_};
            entered_ = true;
            changed_.notify_all();
            if (!changed_.wait_until(lock, Clock::now() + job_deadline, [&] { return released_; }))
                return Error{ErrorCode::execution_failed, "Running-exit gate timed out", {}};
            if (exit_)
                std::_Exit(77);
            return Error{ErrorCode::execution_failed, "Running-exit gate cancelled", {}};
        }
        return bytes_.import(input);
    }
    void await_entry() {
        std::unique_lock lock{mutex_};
        if (!changed_.wait_until(lock, Clock::now() + job_deadline, [&] { return entered_; }))
            throw std::runtime_error{"Importer did not enter the running gate"};
    }
    void release(bool exit) noexcept {
        {
            const std::lock_guard lock{mutex_};
            if (!released_) {
                exit_ = exit;
                released_ = true;
            }
        }
        changed_.notify_all();
    }

  private:
    ByteAssetImporter bytes_;
    bool exit_running_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_{false};
    bool released_{false};
    bool exit_{false};
};

void mark_job(const std::filesystem::path& root, AssetImportJobId job) {
    const auto marker = root / "job-marker";
    if (!std::filesystem::create_directory(marker))
        throw std::runtime_error{"Job observation marker already exists"};
    const auto text = job.scope.to_string() + "\n" + std::to_string(job.sequence) + "\n";
    if (text.size() > 128)
        throw std::runtime_error{"Job marker exceeds its bound"};
    write(marker / "identity.txt", text);
}
std::optional<AssetImportJobId> marked_job(const std::filesystem::path& root) {
    const auto directory = root / "job-marker";
    if (!std::filesystem::exists(directory))
        return {};
    const auto marker = directory / "identity.txt";
    if (std::filesystem::is_symlink(directory) || std::filesystem::is_symlink(marker) ||
        !std::filesystem::is_regular_file(marker) || std::filesystem::file_size(marker) > 128)
        throw std::runtime_error{"Invalid bounded job observation marker"};
    std::ifstream input{marker, std::ios::binary};
    std::string scope, sequence, extra;
    if (!std::getline(input, scope) || !std::getline(input, sequence) || std::getline(input, extra))
        throw std::runtime_error{"Malformed job observation marker"};
    const auto parsed_scope = take(StableId::parse(scope));
    std::uint64_t number = 0;
    const auto parsed = std::from_chars(sequence.data(), sequence.data() + sequence.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != sequence.data() + sequence.size() ||
        number == 0 || parsed_scope.to_string() != scope || std::to_string(number) != sequence)
        throw std::runtime_error{"Noncanonical job observation marker"};
    return AssetImportJobId{parsed_scope, number};
}
AssetImportJobStatus job_status(AssetImports& imports, AssetImportJobId job) {
    const auto values = take(imports.jobs());
    const auto found = std::find_if(values.begin(), values.end(),
                                    [&](const auto& value) { return value.id == job; });
    if (found == values.end())
        throw std::runtime_error{"Lost retained job status"};
    return *found;
}
void await_ready(AssetImports& imports, AssetImportJobId job) {
    const auto until = Clock::now() + job_deadline;
    for (;;) {
        const auto value = job_status(imports, job);
        if (value.state == AssetImportJobState::ready)
            return;
        if (value.state != AssetImportJobState::queued &&
            value.state != AssetImportJobState::running)
            throw std::runtime_error{"Job reached an unexpected terminal phase"};
        if (Clock::now() >= until)
            throw std::runtime_error{"Job did not become ready"};
        std::this_thread::yield();
    }
}
AssetImportJobStatus finish_job(AssetImports& imports, AssetImportJobId job) {
    await_ready(imports, job);
    if (take(imports.drain_jobs()) != 1)
        throw std::runtime_error{"Ready job was not drained"};
    const auto result = take(imports.take_job(job));
    if (!take(imports.jobs()).empty())
        throw std::runtime_error{"Job outcome was not acknowledged"};
    return result;
}
std::string payload_body(const AssetArtifact& artifact) {
    if (artifact.payload.size() < 36)
        throw std::runtime_error{"Invalid byte payload"};
    return {reinterpret_cast<const char*>(artifact.payload.data() + 36),
            artifact.payload.size() - 36};
}

int run_jobs(std::string_view mode, const std::filesystem::path& root,
             std::string_view selected_phase) {
    const bool seed = mode == "seed-jobs";
    const bool interrupt = mode == "interrupt-jobs";
    const bool recover = mode == "recover-jobs";
    const bool blocked = mode == "blocked-jobs";
    if (!seed && !interrupt && !recover && !blocked && mode != "inspect-before-jobs" &&
        mode != "inspect-after-jobs")
        throw std::runtime_error{"Unknown async process mode"};
    auto files = take(ProjectFiles::open(root));
    const auto root_source = take(ProjectPath::parse("root.bin"));
    const auto leaf_source = take(ProjectPath::parse("leaf.bin"));
    const auto root_sidecar = take(asset_sidecar_path(root_source));
    const auto leaf_sidecar = take(asset_sidecar_path(leaf_source));
    if (seed) {
        write(root / "root.bin", "Root");
        write(root / "leaf.bin", "Before");
        take(files->write_asset_metadata(root_sidecar,
                                         {id<ProjectId>('1'),
                                          id<AssetId>('a'),
                                          "bytes",
                                          "Shark authored async process fixture",
                                          "MIT",
                                          "shark.bytes",
                                          AssetAudience::client,
                                          "{}",
                                          {id<AssetId>('b')}},
                                         {}));
        take(files->write_asset_metadata(leaf_sidecar,
                                         {id<ProjectId>('1'), id<AssetId>('b'), "bytes",
                                          "Shark authored async process fixture", "MIT",
                                          "shark.bytes", AssetAudience::shared},
                                         {}));
    }
    auto catalog = take(AssetCatalog::create(*files, id<ProjectId>('1')));
    const std::array inventory{root_sidecar, leaf_sidecar};
    const auto cr = take(catalog->refresh(inventory, take(catalog->revision())));
    for (const auto asset : {id<AssetId>('a'), id<AssetId>('b')}) {
        const auto resolved = take(catalog->resolve(asset, cr));
        if (!resolved.record || !resolved.record->source_present)
            throw std::runtime_error{"Lost persistent process asset identity"};
        const auto metadata = take(files->read_asset_metadata(resolved.record->sidecar));
        if (metadata.revision != resolved.record->metadata_revision ||
            metadata.source.document != resolved.record->metadata)
            throw std::runtime_error{"Catalog and source metadata disagree"};
    }
    ExitImporter importer{interrupt && selected_phase == "running"};
    const std::array<AssetImporter*, 1> registry{&importer};
    auto cache = take(AssetImports::open(*files, *catalog, id<ProjectId>('1'), registry));
    // This guard is destroyed before cache: every failure releases the callback
    // gate without exiting, allowing the service to join its worker safely.
    struct GateRelease {
        ExitImporter& importer;
        ~GateRelease() {
            importer.release(false);
        }
    } release{importer};
    const AssetTargetProfile target{"jobs-process-x64", AssetAudience::client};
    const auto discovered = take(cache->importers());
    if (discovered.size() != 1 || discovered[0] != importer.info())
        throw std::runtime_error{"Async importer discovery changed"};
    const auto old_job = marked_job(root);
    bool rejected_old = false;
    if (old_job) {
        const auto cancel = cache->cancel_job(*old_job);
        const auto take_old = cache->take_job(*old_job);
        rejected_old = old_job->scope != take(cache->revision()).scope && !cancel && !take_old &&
                       cancel.error().code == ErrorCode::conflict &&
                       take_old.error().code == ErrorCode::conflict && take(cache->jobs()).empty();
        if (!rejected_old)
            throw std::runtime_error{"Restart accepted a foreign in-memory job identity"};
    }
    if (interrupt)
        write(root / "leaf.bin", "After");
    const auto recovered = recover ? take(AssetImports::recover(*files)) : 0;
    std::optional<AssetImportJobStatus> completed;
    if (seed || interrupt || recover || blocked) {
        const auto admission =
            take(cache->submit(id<AssetId>('a'), target, cr, take(cache->revision())));
        if (admission.deduplicated)
            throw std::runtime_error{"Fresh owner unexpectedly deduplicated work"};
        if (interrupt) {
            if (selected_phase == "running")
                importer.await_entry();
            mark_job(root, admission.id);
            if (selected_phase == "running")
                importer.release(true);
            await_ready(*cache, admission.id);
            if (selected_phase == "running")
                throw std::runtime_error{"Worker exit did not occur"};
            if (selected_phase == "ready")
                std::_Exit(77);
            auto selected = phase(selected_phase);
            const auto previous = detail::set_file_test_control(
                {detail::FileFault::none,
                 [](detail::FilePhase reached, void* context) noexcept {
                     if (reached == *static_cast<detail::FilePhase*>(context))
                         std::_Exit(77);
                 },
                 &selected});
            completed = finish_job(*cache, admission.id);
            static_cast<void>(detail::set_file_test_control(previous));
            throw std::runtime_error{"Native async publication exit did not occur"};
        }
        completed = finish_job(*cache, admission.id);
        if (blocked) {
            if (completed->state != AssetImportJobState::failed || !completed->error ||
                completed->error->code != ErrorCode::conflict || completed->commit)
                throw std::runtime_error{"Orphan stage did not block async publication"};
        } else if (completed->state != AssetImportJobState::succeeded || !completed->summary ||
                   !completed->commit || completed->error || completed->completed_nodes != 2 ||
                   completed->total_nodes != 2 || completed->summary->nodes != 2)
            throw std::runtime_error{"Async closure did not complete through owner publication"};
    }
    take(cache->close_jobs());
    const auto revision = take(cache->revision());
    const auto usage = take(cache->usage(revision));
    if (usage.bindings != 2 || usage.artifacts < 2 || usage.artifacts > 4 ||
        usage.payload_bytes == 0 || usage.payload_bytes > asset_import_maximum_total_bytes ||
        usage.file_bytes == 0 || usage.file_bytes > asset_cache_maximum_file_bytes)
        throw std::runtime_error{"Lost bounded cache closure after process restart"};
    const auto status = take(cache->query(id<AssetId>('a'), target, cr, revision));
    if (!status.last_valid_key)
        throw std::runtime_error{"Lost last-valid async root binding"};
    const auto artifact = take(cache->read(*status.last_valid_key, revision));
    if (artifact.asset != id<AssetId>('a') || artifact.audience != AssetAudience::client ||
        artifact.dependencies.size() != 1 || artifact.dependencies[0].asset != id<AssetId>('b'))
        throw std::runtime_error{"Lost client/shared dependency closure"};
    const auto dependency = take(cache->read(artifact.dependencies[0].key, revision));
    const bool after = recover || mode == "inspect-after-jobs";
    const bool fresh = seed || after;
    if (payload_body(artifact) != "Root" ||
        payload_body(dependency) != (after ? "After" : "Before") ||
        dependency.audience != AssetAudience::shared ||
        status.state != (fresh ? AssetCacheState::fresh : AssetCacheState::stale))
        throw std::runtime_error{"Wrong last-valid dependency bytes or freshness after restart"};
    std::cout
        << "{\"format\":\"shark.asset-jobs-process\",\"schema_version\":1,\"status\":\"PASS\","
           "\"state\":\""
        << (blocked ? "blocked"
            : fresh ? "fresh"
                    : "stale")
        << "\",\"root_payload\":\"Root\",\"leaf_payload\":\"" << payload_body(dependency)
        << "\",\"fresh\":" << (fresh ? "true" : "false") << ",\"recovered\":" << recovered
        << ",\"key\":\"" << status.last_valid_key->to_hex() << "\",\"dependency_key\":\""
        << dependency.key.to_hex() << "\",\"owner_scope\":\"" << revision.scope.to_string()
        << "\",\"old_job_rejected\":" << (rejected_old ? "true" : "false")
        << ",\"jobs_retained\":" << take(cache->jobs()).size()
        << ",\"artifacts\":" << usage.artifacts
        << ",\"unpublished_stage\":" << (usage.unpublished_stage ? "true" : "false")
        << ",\"nodes\":" << (completed ? completed->total_nodes : 0)
        << ",\"imported\":" << (completed && completed->summary ? completed->summary->imported : 0)
        << ",\"reused\":" << (completed && completed->summary ? completed->summary->reused : 0)
        << ",\"changed\":"
        << (completed && completed->commit && completed->commit->changed ? "true" : "false")
        << ",\"job_sequence\":" << (completed ? completed->id.sequence : 0) << "}\n";
    return 0;
}
} // namespace
int run(int argc, char** argv) {
    try {
        if (argc < 3 || argc > 4)
            throw std::runtime_error{"expected mode root [phase]"};
        const std::string_view mode{argv[1]};
        const std::string root_text{argv[2]};
        const auto root = std::filesystem::path{std::u8string{root_text.begin(), root_text.end()}};
        if (mode.ends_with("-jobs")) {
            if (argc != (mode == "interrupt-jobs" ? 4 : 3))
                throw std::runtime_error{"Expected async mode root [phase]"};
            return run_jobs(mode, root, argc == 4 ? std::string_view{argv[3]} : std::string_view{});
        }
        auto files = take(ProjectFiles::open(root));
        const auto source = take(ProjectPath::parse("asset.bin"));
        const auto sidecar = take(asset_sidecar_path(source));
        if (mode == "seed") {
            write(root / "asset.bin", "Before");
            take(files->write_asset_metadata(sidecar,
                                             {id<ProjectId>('1'), id<AssetId>('a'), "bytes",
                                              "Shark authored process fixture", "MIT",
                                              "shark.bytes"},
                                             {}));
        }
        auto catalog = take(AssetCatalog::create(*files, id<ProjectId>('1')));
        const std::array inventory{sidecar};
        const auto catalog_revision = take(catalog->refresh(inventory, take(catalog->revision())));
        ByteAssetImporter bytes;
        const std::array<AssetImporter*, 1> importers{&bytes};
        auto cache = take(AssetImports::open(*files, *catalog, id<ProjectId>('1'), importers));
        const AssetTargetProfile target{"process-x64", AssetAudience::client};
        std::size_t recovered = 0;
        if (mode == "interrupt")
            write(root / "asset.bin", "After");
        if (mode == "recover")
            recovered = take(AssetImports::recover(*files));
        if (mode == "seed" || mode == "interrupt" || mode == "recover" || mode == "blocked") {
            auto prepared = take(cache->prepare(id<AssetId>('a'), target, catalog_revision,
                                                take(cache->revision())));
            auto selected = detail::FilePhase::staged;
            if (mode == "interrupt") {
                if (argc != 4)
                    throw std::runtime_error{"missing phase"};
                selected = phase(argv[3]);
                static_cast<void>(detail::set_file_test_control(
                    {detail::FileFault::none,
                     [](detail::FilePhase reached, void* context) noexcept {
                         if (reached == *static_cast<detail::FilePhase*>(context))
                             std::_Exit(77);
                     },
                     &selected}));
            }
            auto commit =
                cache->publish(std::move(prepared), catalog_revision, take(cache->revision()));
            if (mode == "blocked") {
                if (commit || commit.error().code != ErrorCode::conflict)
                    throw std::runtime_error{"orphan stage did not block publication"};
                std::cout << "{\"format\":\"shark.asset-import-process\",\"schema_version\":1,"
                             "\"status\":\"PASS\",\"state\":\"blocked\"}\n";
                return 0;
            }
            take(std::move(commit));
        } else if (mode != "inspect-before" && mode != "inspect-after")
            throw std::runtime_error{"unknown mode"};
        const auto status =
            take(cache->query(id<AssetId>('a'), target, catalog_revision, take(cache->revision())));
        if (!status.last_valid_key)
            throw std::runtime_error{"lost last valid binding"};
        const auto artifact = take(cache->read(*status.last_valid_key, take(cache->revision())));
        const std::string body{reinterpret_cast<const char*>(artifact.payload.data() + 36),
                               artifact.payload.size() - 36};
        const bool after = mode == "recover" || mode == "inspect-after";
        if (body != (after ? "After" : "Before") ||
            (after && status.state != AssetCacheState::fresh))
            throw std::runtime_error{"wrong retained payload/state"};
        std::cout << "{\"format\":\"shark.asset-import-process\",\"schema_version\":1,\"status\":"
                     "\"PASS\",\"payload\":\""
                  << body
                  << "\",\"fresh\":" << (status.state == AssetCacheState::fresh ? "true" : "false")
                  << ",\"recovered\":" << recovered << ",\"key\":\""
                  << status.last_valid_key->to_hex() << "\"}\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        std::cout << "{\"status\":\"FAIL\"}\n";
        return 1;
    }
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    try {
        std::vector<std::string> values;
        values.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) {
            const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1,
                                                  nullptr, 0, nullptr, nullptr);
            if (size <= 0)
                return 1;
            std::string text(static_cast<std::size_t>(size), '\0');
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, text.data(), size,
                                    nullptr, nullptr) != size)
                return 1;
            text.pop_back();
            values.push_back(std::move(text));
        }
        std::vector<char*> pointers;
        for (auto& value : values)
            pointers.push_back(value.data());
        return run(argc, pointers.data());
    } catch (...) {
        return 1;
    }
}
#else
int main(int argc, char** argv) {
    return run(argc, argv);
}
#endif
