#include "shark/tools/asset_import.hpp"

#include "project_files_native.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {
using namespace shark;
using namespace shark::tools;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr auto deadline = std::chrono::seconds{15};

template <typename T> T take(Result<T> result) {
    INFO((result ? "" : result.error().message));
    REQUIRE(result);
    return std::move(result).value();
}
template <typename T> T id(unsigned number) {
    std::ostringstream text;
    text << "50000000-0000-4000-8000-" << std::hex << std::setw(12) << std::setfill('0') << number;
    return T{take(StableId::parse(text.str()))};
}
ProjectPath path(std::string_view text) {
    return take(ProjectPath::parse(text));
}
void write(const fs::path& target, std::string_view text) {
    std::ofstream file{target, std::ios::binary | std::ios::trunc};
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    REQUIRE(file.good());
}
std::string read(const fs::path& target) {
    std::ifstream file{target, std::ios::binary};
    REQUIRE(file.good());
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}
bool terminal(AssetImportJobState state) {
    return state == AssetImportJobState::succeeded || state == AssetImportJobState::failed ||
           state == AssetImportJobState::cancelled;
}
template <typename T> void rejected(const Result<T>& result, ErrorCode code) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == code);
    CHECK_FALSE(result.error().context.operation.empty());
}

// All synchronization is driven by explicit callback entry/release. Deadlines
// only fail a broken test; they never select the expected interleaving.
class GatedImporter final : public AssetImporter {
  public:
    enum class Output {
        normal,
        failure,
        exception,
        malformed,
        oversized,
        long_error,
        limit_error,
        edge_error,
        reserved_error
    };
    ByteAssetImporter bytes;
    AssetImporterInfo descriptor = bytes.info();
    std::atomic<unsigned> calls{0};
    std::atomic<bool> imported_on_owner{false};
    std::atomic<bool> overlapping_callbacks{false};
    std::atomic<bool> observed_stop{false};
    Output output{Output::normal};
    std::function<void()> stop_hook;

    const AssetImporterInfo& info() const noexcept override {
        return descriptor;
    }
    Result<std::string> normalize_settings(std::string_view json) const override {
        Callback use{*this};
        return bytes.normalize_settings(json);
    }
    Result<void> validate_payload(const AssetArtifact& artifact) const override {
        Callback use{*this};
        return bytes.validate_payload(artifact);
    }
    Result<std::vector<std::byte>> import(const AssetImportInput& input) override {
        Callback use{*this};
        std::stop_callback stopped{input.stop, [this]() noexcept {
                                       if (stop_hook)
                                           stop_hook();
                                   }};
        if (std::this_thread::get_id() == owner_)
            imported_on_owner = true;
        {
            std::unique_lock lock{mutex_};
            ++calls;
            entered_.notify_all();
            if (blocked_ && (!block_asset_ || input.asset == *block_asset_)) {
                bool completed = false;
                if (cooperative_) {
                    completed = release_.wait_until(lock, input.stop, Clock::now() + deadline,
                                                    [&] { return released_; });
                } else {
                    completed = release_.wait_until(lock, Clock::now() + deadline,
                                                    [&] { return released_; });
                }
                observed_stop = input.stop.stop_requested();
                if (!completed && !input.stop.stop_requested())
                    return Error{ErrorCode::execution_failed, "Fixture gate timed out", {}};
            }
        }
        switch (output) {
        case Output::failure:
            return Error{ErrorCode::execution_failed, "Controlled importer failure", {}};
        case Output::exception:
            throw std::runtime_error{"Controlled importer exception"};
        case Output::malformed:
            return std::vector<std::byte>{std::byte{1}};
        case Output::oversized:
            return std::vector<std::byte>(asset_import_maximum_payload_bytes + 1);
        case Output::long_error:
            return Error{ErrorCode::execution_failed,
                         std::string(65536, 'm'),
                         {std::string(65536, 'o'), std::string(65536, 'r'), std::string(65536, 'p'),
                          std::string(65536, 'e'), std::string(65536, 'a')}};
        case Output::limit_error:
        case Output::edge_error: {
            const std::size_t size = output == Output::limit_error ? 1024 : 1025;
            return Error{ErrorCode::execution_failed,
                         std::string(size, 'm'),
                         {std::string(size, 'o'), std::string(size, 'r'), std::string(size, 'p'),
                          std::string(size, 'e'), std::string(size, 'a')}};
        }
        case Output::reserved_error: {
            Error error{ErrorCode::execution_failed, {}, {}};
            for (auto* field :
                 {&error.message, &error.context.operation, &error.context.request_id,
                  &error.context.path, &error.context.entity_id, &error.context.asset_id}) {
                field->reserve(65536);
                *field = "short";
            }
            return error;
        }
        case Output::normal:
            return bytes.import(input);
        }
        return Error{ErrorCode::execution_failed, "Unknown fixture output", {}};
    }
    void block(bool cooperative = false, std::optional<AssetId> asset = {}) {
        const std::lock_guard lock{mutex_};
        blocked_ = true;
        released_ = false;
        cooperative_ = cooperative;
        block_asset_ = asset;
    }
    void release() noexcept {
        {
            const std::lock_guard lock{mutex_};
            released_ = true;
        }
        release_.notify_all();
    }
    bool wait_calls(unsigned minimum) {
        std::unique_lock lock{mutex_};
        return entered_.wait_until(lock, Clock::now() + deadline,
                                   [&] { return calls.load() >= minimum; });
    }

  private:
    struct Callback {
        const GatedImporter& importer;
        explicit Callback(const GatedImporter& value) : importer(value) {
            if (importer.active_.fetch_add(1) != 0)
                const_cast<GatedImporter&>(importer).overlapping_callbacks = true;
        }
        ~Callback() {
            --importer.active_;
        }
    };
    const std::thread::id owner_ = std::this_thread::get_id();
    mutable std::atomic<unsigned> active_{0};
    std::mutex mutex_;
    std::condition_variable entered_;
    std::condition_variable_any release_;
    bool blocked_{false};
    bool released_{false};
    bool cooperative_{false};
    std::optional<AssetId> block_asset_;
};

struct Fixture {
    fs::path root =
        fs::temp_directory_path() / ("shark-import-jobs-" + take(generate_stable_id()).to_string());
    std::unique_ptr<ProjectFiles> files;
    std::unique_ptr<AssetCatalog> catalog;
    GatedImporter importer;
    std::unique_ptr<AssetImports> imports;
    std::vector<ProjectPath> inventory;
    const AssetTargetProfile target{"jobs-x64", AssetAudience::client};
    Fixture() {
        REQUIRE(fs::create_directory(root));
        files = take(ProjectFiles::open(root));
        catalog = take(AssetCatalog::create(*files, id<ProjectId>(1)));
        reopen();
    }
    ~Fixture() {
        importer.release();
        imports.reset();
        catalog.reset();
        files.reset();
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    void reopen() {
        imports.reset();
        const std::array<AssetImporter*, 1> registry{&importer};
        imports = take(AssetImports::open(*files, *catalog, id<ProjectId>(1), registry));
    }
    ProjectPath source(unsigned number) const {
        return path("asset" + std::to_string(number) + ".bin");
    }
    ProjectPath sidecar(unsigned number) const {
        return take(asset_sidecar_path(source(number)));
    }
    void seed(unsigned number, std::string_view data = "hello",
              std::vector<AssetId> dependencies = {}) {
        write(root / source(number).text(), data);
        AssetMetadata metadata{id<ProjectId>(1),
                               id<AssetId>(number),
                               "bytes",
                               "Shark authored offline fixture",
                               "MIT",
                               "shark.bytes",
                               AssetAudience::shared,
                               "{}",
                               std::move(dependencies)};
        take(files->write_asset_metadata(sidecar(number), metadata, {}));
        inventory.push_back(sidecar(number));
    }
    AssetCatalogRevision refresh() {
        return take(catalog->refresh(inventory, take(catalog->revision())));
    }
    AssetCacheRevision revision() const {
        return take(imports->revision());
    }
    AssetImportJobAdmission submit(unsigned number, const AssetTargetProfile* profile = nullptr) {
        return take(imports->submit(id<AssetId>(number), profile ? *profile : target,
                                    take(catalog->revision()), revision()));
    }
    AssetImportJobStatus status(AssetImportJobId job) const {
        auto values = imports->jobs();
        if (!values) {
            INFO(values.error().message);
            REQUIRE(values);
        }
        for (const auto& value : values.value())
            if (value.id == job)
                return value;
        FAIL("Expected a retained job");
    }
    AssetImportJobStatus wait(AssetImportJobId job, AssetImportJobState state) const {
        const auto end = Clock::now() + deadline;
        for (;;) {
            auto value = status(job);
            if (value.state == state)
                return value;
            if (terminal(value.state))
                FAIL("Job became terminal before the expected phase");
            if (Clock::now() >= end)
                FAIL("Timed out waiting for the expected job phase");
            std::this_thread::yield();
        }
    }
    AssetImportJobStatus finish(AssetImportJobId job) {
        const auto end = Clock::now() + deadline;
        for (;;) {
            auto drained = imports->drain_jobs();
            if (!drained) {
                INFO(drained.error().message);
                REQUIRE(drained);
            }
            auto value = status(job);
            if (terminal(value.state))
                return value;
            if (Clock::now() >= end)
                FAIL("Timed out waiting for a terminal job");
            std::this_thread::yield();
        }
    }
    AssetImportSummary commit(unsigned number) {
        auto prepared = take(
            imports->prepare(id<AssetId>(number), target, take(catalog->revision()), revision()));
        auto summary = take(prepared.summary());
        take(imports->publish(std::move(prepared), take(catalog->revision()), revision()));
        return summary;
    }
    fs::path cache_file() const {
        return root / std::string{asset_cache_filename};
    }
};
struct Fault {
    detail::FileTestControl previous;
    explicit Fault(detail::FileFault value)
        : previous(detail::set_file_test_control({value, {}, {}})) {}
    ~Fault() {
        static_cast<void>(detail::set_file_test_control(previous));
    }
};

void stop_callback_reentry(bool close) {
    // Callback captures outlive the service even if an assertion aborts the case.
    std::optional<AssetImportJobId> stopped_job;
    std::atomic<unsigned> callbacks{0};
    std::atomic<bool> ran_on_owner{false};
    std::atomic<bool> rejected_reentry{false};
    const auto owner = std::this_thread::get_id();
    Fixture f;
    f.seed(10);
    f.refresh();
    f.importer.block(true);
    f.importer.stop_hook = [&, service = f.imports.get()]() noexcept {
        ++callbacks;
        ran_on_owner = std::this_thread::get_id() == owner;
        if (!stopped_job)
            return;
        const auto queried = service->jobs();
        const auto cancelled = service->cancel_job(*stopped_job);
        const auto drained = service->drain_jobs();
        rejected_reentry = !queried && queried.error().code == ErrorCode::conflict && !cancelled &&
                           cancelled.error().code == ErrorCode::conflict && !drained &&
                           drained.error().code == ErrorCode::conflict;
    };
    stopped_job = f.submit(10).id;
    REQUIRE(f.importer.wait_calls(1));
    if (close)
        take(f.imports->close_jobs());
    else
        take(f.imports->cancel_job(*stopped_job));
    CHECK(callbacks == 1);
    CHECK(ran_on_owner);
    CHECK(rejected_reentry);
    CHECK(f.finish(*stopped_job).state == AssetImportJobState::cancelled);
    CHECK_FALSE(fs::exists(f.cache_file()));
}
} // namespace

TEST_CASE("Background imports execute off owner and publish only during drain",
          "[asset-import-jobs]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    const auto revision = f.revision();
    const auto admitted = f.submit(10);
    CHECK_FALSE(admitted.deduplicated);
    const auto ready = f.wait(admitted.id, AssetImportJobState::ready);
    CHECK(ready.completed_nodes == 1);
    CHECK(ready.total_nodes == 1);
    CHECK_FALSE(ready.commit);
    CHECK(f.revision() == revision);
    CHECK_FALSE(fs::exists(f.cache_file()));
    CHECK(take(f.imports->drain_jobs()) == 1);
    const auto done = f.status(admitted.id);
    CHECK(done.state == AssetImportJobState::succeeded);
    REQUIRE(done.summary);
    REQUIRE(done.commit);
    CHECK(done.commit->changed);
    CHECK(done.summary->root == id<AssetId>(10));
    CHECK(done.summary->cache_revision == revision);
    CHECK(done.commit->revision.value == revision.value + 1);
    CHECK_FALSE(done.error);
    CHECK_FALSE(f.importer.imported_on_owner);
    CHECK_FALSE(f.importer.overlapping_callbacks);
    CHECK(take(f.imports->drain_jobs()) == 0);
    const auto artifact = take(f.imports->read(done.summary->root_key, done.commit->revision));
    CHECK(artifact.asset == id<AssetId>(10));
    CHECK(artifact.payload.size() == 41);
}

TEST_CASE("Matching retained pending work shares one stable job and one import",
          "[asset-import-jobs]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    f.importer.block();
    const auto first = f.submit(10);
    REQUIRE(f.importer.wait_calls(1));
    const auto duplicate = f.submit(10);
    CHECK(duplicate.deduplicated);
    CHECK(duplicate.id == first.id);
    CHECK(take(f.imports->jobs()).size() == 1);
    CHECK(f.importer.calls == 1);
    CHECK(take(f.imports->drain_jobs()) == 0);
    f.importer.release();
    f.wait(first.id, AssetImportJobState::ready);
    const auto ready_duplicate = f.submit(10);
    CHECK(ready_duplicate.deduplicated);
    CHECK(ready_duplicate.id == first.id);
    CHECK(f.finish(first.id).state == AssetImportJobState::succeeded);
    const auto consumed = take(f.imports->take_job(first.id));
    CHECK(consumed.state == AssetImportJobState::succeeded);
    const auto retry = f.submit(10);
    CHECK_FALSE(retry.deduplicated);
    CHECK(retry.id.scope == first.id.scope);
    CHECK(retry.id.sequence > first.id.sequence);
    const auto result = f.finish(retry.id);
    REQUIRE(result.summary);
    REQUIRE(result.commit);
    CHECK(result.summary->reused == 1);
    CHECK_FALSE(result.commit->changed);
    CHECK(f.importer.calls == 1);
}

TEST_CASE("Four retained slots bound queued running and terminal jobs until taken",
          "[asset-import-jobs]") {
    Fixture f;
    for (unsigned number = 10; number < 15; ++number)
        f.seed(number);
    f.refresh();
    f.importer.block();
    std::vector<AssetImportJobId> jobs{f.submit(10).id};
    REQUIRE(f.importer.wait_calls(1));
    for (unsigned number = 11; number < 14; ++number)
        jobs.push_back(f.submit(number).id);
    CHECK(take(f.imports->jobs()).size() == asset_import_maximum_jobs);
    CHECK(f.importer.calls == 1);
    for (std::size_t index = 1; index < jobs.size(); ++index) {
        CHECK(f.status(jobs[index]).state == AssetImportJobState::queued);
        CHECK(take(f.imports->cancel_job(jobs[index])).state == AssetImportJobState::cancelled);
    }
    CHECK_FALSE(
        f.imports->submit(id<AssetId>(14), f.target, take(f.catalog->revision()), f.revision()));
    rejected(f.imports->take_job(jobs[0]), ErrorCode::conflict);
    take(f.imports->take_job(jobs[1]));
    const auto replacement = f.submit(14);
    CHECK(replacement.id.sequence > jobs.back().sequence);
    CHECK_FALSE(replacement.deduplicated);
    CHECK(take(f.imports->jobs()).size() == asset_import_maximum_jobs);
    take(f.imports->cancel_job(replacement.id));
    take(f.imports->cancel_job(jobs[0]));
    f.importer.release();
    CHECK(f.finish(jobs[0]).state == AssetImportJobState::cancelled);
    CHECK_FALSE(fs::exists(f.cache_file()));
}

TEST_CASE("Queued cancellation never executes the cancelled importer", "[asset-import-jobs]") {
    Fixture f;
    f.seed(10);
    f.seed(11);
    f.refresh();
    f.importer.block();
    const auto running = f.submit(10).id;
    REQUIRE(f.importer.wait_calls(1));
    const auto queued = f.submit(11).id;
    CHECK(f.status(queued).state == AssetImportJobState::queued);
    const auto cancelled = take(f.imports->cancel_job(queued));
    CHECK(cancelled.state == AssetImportJobState::cancelled);
    CHECK_FALSE(cancelled.commit);
    CHECK(take(f.imports->cancel_job(queued)).state == AssetImportJobState::cancelled);
    f.importer.release();
    CHECK(f.finish(running).state == AssetImportJobState::succeeded);
    CHECK(f.importer.calls == 1);
    CHECK(f.status(queued).state == AssetImportJobState::cancelled);
    CHECK_FALSE(f.importer.overlapping_callbacks);
}

TEST_CASE("Running cancellation fences publication even when callback ignores stop",
          "[asset-import-jobs]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    f.importer.block();
    const auto job = f.submit(10).id;
    REQUIRE(f.importer.wait_calls(1));
    const auto before = f.revision();
    const auto cancelling = take(f.imports->cancel_job(job));
    CHECK_FALSE(cancelling.commit);
    f.importer.release();
    const auto outcome = f.finish(job);
    CHECK(outcome.state == AssetImportJobState::cancelled);
    CHECK_FALSE(outcome.commit);
    CHECK(f.revision() == before);
    CHECK_FALSE(fs::exists(f.cache_file()));
    CHECK(f.importer.observed_stop);
    take(f.imports->take_job(job));
    const auto retry = f.submit(10).id;
    CHECK(retry.sequence > job.sequence);
    CHECK(f.finish(retry).state == AssetImportJobState::succeeded);
}

TEST_CASE("Ready cancellation discards complete output before owner publication",
          "[asset-import-jobs]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    const auto job = f.submit(10).id;
    f.wait(job, AssetImportJobState::ready);
    const auto before = f.revision();
    take(f.imports->cancel_job(job));
    CHECK(f.finish(job).state == AssetImportJobState::cancelled);
    CHECK(f.revision() == before);
    CHECK_FALSE(fs::exists(f.cache_file()));
}

TEST_CASE("Cancellation after publication returns the retained committed result",
          "[asset-import-jobs]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    const auto job = f.submit(10).id;
    const auto committed = f.finish(job);
    REQUIRE(committed.commit);
    const auto bytes = read(f.cache_file());
    const auto cancelled = take(f.imports->cancel_job(job));
    CHECK(cancelled.state == AssetImportJobState::succeeded);
    REQUIRE(cancelled.commit);
    CHECK(cancelled.commit->revision == committed.commit->revision);
    CHECK(read(f.cache_file()) == bytes);
    CHECK(f.revision() == committed.commit->revision);
}

TEST_CASE("Worker progress advances in dependency order without owner callbacks racing",
          "[asset-import-jobs]") {
    Fixture f;
    f.seed(11, "dependency");
    f.seed(10, "root", {id<AssetId>(11)});
    const auto catalog = f.refresh();
    const auto cache = f.revision();
    f.importer.block(false, id<AssetId>(10));
    const auto job = f.submit(10).id;
    REQUIRE(f.importer.wait_calls(2));
    const auto running = f.status(job);
    CHECK(running.state == AssetImportJobState::running);
    CHECK(running.completed_nodes == 1);
    CHECK(running.total_nodes == 2);
    rejected(f.imports->prepare(id<AssetId>(10), f.target, catalog, cache), ErrorCode::conflict);
    CHECK_FALSE(f.imports->query(id<AssetId>(10), f.target, catalog, cache));
    CHECK_FALSE(f.importer.overlapping_callbacks);
    f.importer.release();
    const auto outcome = f.finish(job);
    CHECK(outcome.state == AssetImportJobState::succeeded);
    CHECK(outcome.completed_nodes == 2);
    REQUIRE(outcome.summary);
    CHECK(outcome.summary->nodes == 2);
    CHECK(outcome.summary->imported == 2);
    CHECK_FALSE(f.importer.overlapping_callbacks);
}

TEST_CASE("Late source or settings changes preserve the previous cache on completion",
          "[asset-import-jobs]") {
    for (const bool change_metadata : {false, true}) {
        CAPTURE(change_metadata);
        Fixture f;
        f.seed(10, "old");
        f.refresh();
        const auto original = f.commit(10);
        const auto old_bytes = read(f.cache_file());
        write(f.root / f.source(10).text(), "captured update");
        f.importer.block();
        const auto job = f.submit(10).id;
        REQUIRE(f.importer.wait_calls(2));
        if (change_metadata) {
            auto metadata = take(f.files->read_asset_metadata(f.sidecar(10)));
            metadata.source.document.settings_json = "{\"transform\":\"uppercase-ascii\"}";
            take(f.files->write_asset_metadata(f.sidecar(10), metadata.source.document,
                                               metadata.revision));
        } else {
            write(f.root / f.source(10).text(), "newer human edit");
        }
        f.importer.release();
        const auto outcome = f.finish(job);
        CHECK(outcome.state == AssetImportJobState::failed);
        REQUIRE(outcome.error);
        CHECK(outcome.error->code == ErrorCode::conflict);
        CHECK_FALSE(outcome.commit);
        CHECK(read(f.cache_file()) == old_bytes);
        CHECK(take(f.imports->read(original.root_key, f.revision())).key == original.root_key);
        take(f.imports->take_job(job));
        if (change_metadata)
            f.refresh();
        const auto retry = f.submit(10).id;
        CHECK(f.finish(retry).state == AssetImportJobState::succeeded);
    }
}

TEST_CASE("Catalog revision changes reject a completed job until explicit resubmission",
          "[asset-import-jobs]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    const auto job = f.submit(10).id;
    f.wait(job, AssetImportJobState::ready);
    f.seed(11);
    f.refresh();
    const auto outcome = f.finish(job);
    CHECK(outcome.state == AssetImportJobState::failed);
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::conflict);
    CHECK_FALSE(fs::exists(f.cache_file()));
    take(f.imports->take_job(job));
    CHECK(f.finish(f.submit(10).id).state == AssetImportJobState::succeeded);
}

TEST_CASE("Queued jobs retain their original cache revision and never silently rebase",
          "[asset-import-jobs]") {
    Fixture f;
    f.seed(10);
    f.seed(11);
    f.refresh();
    f.importer.block();
    const auto first = f.submit(10).id;
    REQUIRE(f.importer.wait_calls(1));
    const auto second = f.submit(11).id;
    const auto original = f.revision();
    f.importer.release();
    CHECK(f.finish(first).state == AssetImportJobState::succeeded);
    const auto bytes = read(f.cache_file());
    const auto stale = f.finish(second);
    CHECK(stale.state == AssetImportJobState::failed);
    REQUIRE(stale.error);
    CHECK(stale.error->code == ErrorCode::conflict);
    CHECK(read(f.cache_file()) == bytes);
    CHECK(f.revision().value == original.value + 1);
    take(f.imports->take_job(second));
    CHECK(f.finish(f.submit(11).id).state == AssetImportJobState::succeeded);
}

TEST_CASE("Different targets or changed captured bytes are not deduplicated",
          "[asset-import-jobs]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    f.importer.block();
    const auto first = f.submit(10).id;
    REQUIRE(f.importer.wait_calls(1));
    const AssetTargetProfile other{"jobs-other", AssetAudience::client};
    const auto different_target = f.submit(10, &other);
    CHECK_FALSE(different_target.deduplicated);
    CHECK(different_target.id != first);
    write(f.root / f.source(10).text(), "edited");
    const auto different_source = f.submit(10);
    CHECK_FALSE(different_source.deduplicated);
    CHECK(different_source.id != first);
    take(f.imports->cancel_job(different_target.id));
    take(f.imports->cancel_job(different_source.id));
    take(f.imports->cancel_job(first));
    f.importer.release();
    CHECK(f.finish(first).state == AssetImportJobState::cancelled);
}

TEST_CASE("Worker failures exceptions and invalid payloads retain last valid output",
          "[asset-import-jobs]") {
    for (const auto output : {GatedImporter::Output::failure, GatedImporter::Output::exception,
                              GatedImporter::Output::malformed, GatedImporter::Output::oversized}) {
        CAPTURE(static_cast<int>(output));
        Fixture f;
        f.seed(10, "original");
        f.refresh();
        const auto original = f.commit(10);
        const auto old_bytes = read(f.cache_file());
        const auto revision = f.revision();
        write(f.root / f.source(10).text(), "changed");
        f.importer.output = output;
        const auto job = f.submit(10).id;
        const auto result = f.finish(job);
        CHECK(result.state == AssetImportJobState::failed);
        REQUIRE(result.error);
        CHECK_FALSE(result.error->context.operation.empty());
        CHECK_FALSE(result.commit);
        CHECK(f.revision() == revision);
        CHECK(read(f.cache_file()) == old_bytes);
        CHECK(take(f.imports->read(original.root_key, revision)).key == original.root_key);
        take(f.imports->take_job(job));
        f.importer.output = GatedImporter::Output::normal;
        CHECK(f.finish(f.submit(10).id).state == AssetImportJobState::succeeded);
    }
}

TEST_CASE("Worker diagnostics are bounded before retention and status copies",
          "[asset-import-jobs]") {
    for (const auto output :
         {GatedImporter::Output::long_error, GatedImporter::Output::limit_error,
          GatedImporter::Output::edge_error, GatedImporter::Output::reserved_error}) {
        Fixture f;
        f.seed(10);
        f.refresh();
        f.importer.output = output;
        const auto result = f.finish(f.submit(10).id);
        CHECK(result.state == AssetImportJobState::failed);
        REQUIRE(result.error);
        CHECK(result.error->code == ErrorCode::execution_failed);
        CHECK(result.error->context.operation == "asset.jobs.worker");
        for (const auto* field :
             {&result.error->message, &result.error->context.operation,
              &result.error->context.request_id, &result.error->context.path,
              &result.error->context.entity_id, &result.error->context.asset_id})
            CHECK(field->size() <= 1024);
        if (output == GatedImporter::Output::limit_error) {
            CHECK(result.error->message == std::string(1024, 'm'));
            CHECK(result.error->context.path == std::string(1024, 'p'));
        } else {
            CHECK(result.error->message.empty());
            CHECK(result.error->context.path.empty());
        }
    }
}

TEST_CASE("Foreign expired and nonowner job operations reject without mutation",
          "[asset-import-jobs]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    const auto job = f.submit(10).id;
    f.finish(job);
    const auto revision = f.revision();
    AssetImportJobId foreign{id<StableId>(999), job.sequence};
    CHECK_FALSE(f.imports->cancel_job(foreign));
    CHECK_FALSE(f.imports->take_job(foreign));
    const AssetImportJobId unknown{job.scope, job.sequence + 10};
    CHECK_FALSE(f.imports->cancel_job(unknown));
    CHECK_FALSE(f.imports->take_job(unknown));
    std::atomic<bool> all_rejected{false};
    std::jthread other{[&] {
        all_rejected = !f.imports->jobs() && !f.imports->cancel_job(job) &&
                       !f.imports->take_job(job) && !f.imports->drain_jobs() &&
                       !f.imports->close_jobs();
    }};
    other.join();
    CHECK(all_rejected);
    CHECK(f.revision() == revision);
    CHECK(take(f.imports->jobs()).size() == 1);
    take(f.imports->take_job(job));
    CHECK_FALSE(f.imports->cancel_job(job));
    CHECK_FALSE(f.imports->take_job(job));
    CHECK(take(f.imports->jobs()).empty());
    const auto newer = f.submit(10).id;
    CHECK(newer.sequence > job.sequence);
    f.finish(newer);
    f.reopen();
    CHECK(take(f.imports->jobs()).empty());
    CHECK_FALSE(f.imports->cancel_job(newer));
}

TEST_CASE("Close cancels running and queued work joins and keeps terminal outcomes",
          "[asset-import-jobs]") {
    Fixture f;
    f.seed(10);
    f.seed(11);
    f.refresh();
    f.importer.block(true);
    const auto running = f.submit(10).id;
    REQUIRE(f.importer.wait_calls(1));
    const auto queued = f.submit(11).id;
    take(f.imports->close_jobs());
    CHECK(f.importer.observed_stop);
    CHECK(f.importer.calls == 1);
    CHECK(f.status(running).state == AssetImportJobState::cancelled);
    CHECK(f.status(queued).state == AssetImportJobState::cancelled);
    CHECK_FALSE(fs::exists(f.cache_file()));
    CHECK_FALSE(
        f.imports->submit(id<AssetId>(10), f.target, take(f.catalog->revision()), f.revision()));
    CHECK(take(f.imports->drain_jobs()) == 0);
    take(f.imports->close_jobs());
    take(f.imports->take_job(running));
    take(f.imports->take_job(queued));
    CHECK(take(f.imports->jobs()).empty());
    f.importer.release();
    f.reopen();
    CHECK(f.finish(f.submit(10).id).state == AssetImportJobState::succeeded);
}

TEST_CASE("Destroying the job service cooperatively joins before borrowed importers die",
          "[asset-import-jobs]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    f.importer.block(true);
    f.submit(10);
    REQUIRE(f.importer.wait_calls(1));
    f.imports.reset();
    CHECK(f.importer.observed_stop);
    CHECK_FALSE(fs::exists(f.cache_file()));
}

TEST_CASE("Cancellation rejects job control reentry from synchronous stop callbacks",
          "[asset-import-jobs]") {
    stop_callback_reentry(false);
}

TEST_CASE("Shutdown rejects job control reentry from synchronous stop callbacks",
          "[asset-import-jobs]") {
    stop_callback_reentry(true);
}

TEST_CASE("Native job publication failures preserve bytes and explicit retry succeeds",
          "[asset-import-jobs]") {
    Fixture f;
    f.seed(10, "old");
    f.refresh();
    f.commit(10);
    const auto bytes = read(f.cache_file());
    write(f.root / f.source(10).text(), "new");
    const auto job = f.submit(10).id;
    f.wait(job, AssetImportJobState::ready);
    {
        Fault fault{detail::FileFault::publish};
        CHECK(take(f.imports->drain_jobs()) == 1);
    }
    const auto outcome = f.status(job);
    CHECK(outcome.state == AssetImportJobState::failed);
    REQUIRE(outcome.error);
    CHECK(outcome.error->code == ErrorCode::io_failure);
    CHECK(read(f.cache_file()) == bytes);
    take(f.imports->take_job(job));
    CHECK(f.finish(f.submit(10).id).state == AssetImportJobState::succeeded);
}

TEST_CASE("Unpublished stages require explicit recovery before a new job can commit",
          "[asset-import-jobs]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    const auto stage = f.root / ".shark-cache-stage-50000000-0000-4000-8000-000000000001";
    write(stage, "unpublished");
    const auto job = f.submit(10).id;
    CHECK(f.finish(job).state == AssetImportJobState::failed);
    CHECK(fs::exists(stage));
    CHECK_FALSE(fs::exists(f.cache_file()));
    take(f.imports->take_job(job));
    CHECK(take(AssetImports::recover(*f.files)) == 1);
    CHECK(take(AssetImports::recover(*f.files)) == 0);
    CHECK(f.finish(f.submit(10).id).state == AssetImportJobState::succeeded);
}

TEST_CASE("A postpublication durability warning remains a committed job after cancellation",
          "[asset-import-jobs]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    const auto job = f.submit(10).id;
    f.wait(job, AssetImportJobState::ready);
    {
        Fault fault{detail::FileFault::directory_flush};
        CHECK(take(f.imports->drain_jobs()) == 1);
    }
    const auto outcome = take(f.imports->cancel_job(job));
    CHECK(outcome.state == AssetImportJobState::succeeded);
    REQUIRE(outcome.commit);
    REQUIRE(outcome.commit->file);
    CHECK(outcome.commit->file->durability == FileDurability::directory_flush_failed);
    CHECK(fs::exists(f.cache_file()));
    CHECK_FALSE(outcome.error);
}

TEST_CASE("Background import retains the complete positive 128 node dependency closure",
          "[asset-import-jobs]") {
    Fixture f;
    for (unsigned number = 1; number <= asset_import_maximum_nodes; ++number) {
        std::vector<AssetId> dependencies;
        if (number != asset_import_maximum_nodes)
            dependencies.push_back(id<AssetId>(number + 1));
        f.seed(number, "node", std::move(dependencies));
    }
    f.refresh();
    const auto result = f.finish(f.submit(1).id);
    CHECK(result.state == AssetImportJobState::succeeded);
    REQUIRE(result.summary);
    REQUIRE(result.commit);
    CHECK(result.summary->nodes == asset_import_maximum_nodes);
    CHECK(result.summary->imported == asset_import_maximum_nodes);
    CHECK(result.completed_nodes == asset_import_maximum_nodes);
    CHECK(f.importer.calls == asset_import_maximum_nodes);
    CHECK(take(f.imports->usage(result.commit->revision)).artifacts == asset_import_maximum_nodes);
    CHECK_FALSE(f.importer.imported_on_owner);
    CHECK_FALSE(f.importer.overlapping_callbacks);
}

TEST_CASE("Aggregate captured source bytes reject admission before the four slot limit",
          "[asset-import-jobs]") {
    Fixture f;
    std::vector<AssetId> dependencies;
    const std::string source(asset_import_maximum_source_bytes, 's');
    for (unsigned number = 2; number <= 16; ++number) {
        f.seed(number, source);
        dependencies.push_back(id<AssetId>(number));
    }
    f.seed(1, source, std::move(dependencies));
    const auto catalog = f.refresh();
    const auto cache = f.revision();
    f.importer.block();
    const auto first = f.submit(1).id;
    REQUIRE(f.importer.wait_calls(1));
    const AssetTargetProfile second_target{"source-two", AssetAudience::client};
    const auto second = f.submit(1, &second_target).id;
    CHECK(take(f.imports->jobs()).size() == 2);
    const AssetTargetProfile third_target{"source-three", AssetAudience::client};
    CHECK_FALSE(f.imports->submit(id<AssetId>(1), third_target, catalog, cache));
    CHECK(take(f.imports->jobs()).size() == 2);
    take(f.imports->cancel_job(second));
    take(f.imports->take_job(second));
    const auto replacement = f.submit(1, &third_target).id;
    CHECK(replacement.sequence > second.sequence);
    take(f.imports->cancel_job(replacement));
    take(f.imports->cancel_job(first));
    f.importer.release();
    CHECK(f.finish(first).state == AssetImportJobState::cancelled);
    CHECK_FALSE(fs::exists(f.cache_file()));
}

TEST_CASE("Aggregate reused cache payload reservations are independent of source bytes",
          "[asset-import-jobs]") {
    Fixture f;
    std::vector<AssetId> dependencies;
    const std::string source(asset_import_maximum_source_bytes, 'c');
    for (unsigned number = 2; number <= 12; ++number) {
        f.seed(number, source);
        dependencies.push_back(id<AssetId>(number));
    }
    f.seed(1, source, std::move(dependencies));
    f.seed(30, "tiny");
    const auto catalog = f.refresh();
    f.commit(1);
    const auto cache = f.revision();
    CHECK(take(f.imports->usage(cache)).payload_bytes > 12 * 1024 * 1024);
    f.importer.block();
    const auto first = f.submit(30).id;
    REQUIRE(f.importer.wait_calls(13));
    const AssetTargetProfile second_target{"cache-two", AssetAudience::client};
    const auto second = f.submit(30, &second_target).id;
    const AssetTargetProfile third_target{"cache-three", AssetAudience::client};
    CHECK_FALSE(f.imports->submit(id<AssetId>(30), third_target, catalog, cache));
    CHECK(take(f.imports->jobs()).size() == 2);
    take(f.imports->cancel_job(second));
    take(f.imports->take_job(second));
    const auto replacement = f.submit(30, &third_target).id;
    take(f.imports->cancel_job(replacement));
    take(f.imports->cancel_job(first));
    f.importer.release();
    CHECK(f.finish(first).state == AssetImportJobState::cancelled);
    CHECK(f.revision() == cache);
}
