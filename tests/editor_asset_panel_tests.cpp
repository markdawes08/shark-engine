#include "../editor/asset_panel.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {
using namespace shark;
using namespace shark::tools;
using namespace shark::editor;
using Json = nlohmann::json;
namespace fs = std::filesystem;

template <typename T> T take(Result<T> result) {
    REQUIRE(result);
    return std::move(result).value();
}
void accepted(Result<void> result) {
    REQUIRE(result);
}
template <typename T> void conflict(const Result<T>& result) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::conflict);
    CHECK_FALSE(result.error().message.empty());
}
StableId stable(std::size_t number) {
    const auto text = std::to_string(number);
    return take(
        StableId::parse("16d00000-0000-4000-8000-" + std::string(12 - text.size(), '0') + text));
}
AssetId asset(std::size_t number) {
    return AssetId{stable(number)};
}
void write(const fs::path& path, std::string_view value) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(value.data(), static_cast<std::streamsize>(value.size()));
    file.close();
    REQUIRE(file);
}
std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file);
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}
Json call(ProjectSession& session, std::string_view method, Json params = Json::object()) {
    const auto reply = take(session.request(method, params.dump()));
    REQUIRE_FALSE(reply.failed);
    const auto document = Json::parse(reply.json);
    REQUIRE_FALSE(document.contains("error"));
    return document.at("result");
}
class Fixture final {
  public:
    Fixture() {
        root = fs::temp_directory_path() /
               ("shark-editor-assets-" + take(generate_stable_id()).to_string());
        REQUIRE(fs::create_directory(root));
        REQUIRE(fs::create_directory(root / "scenes"));
        REQUIRE(fs::create_directory(root / "assets"));
        auto files = take(ProjectFiles::open(root));
        ProjectDocument project{
            ProjectId{stable(9001)},
            WorldId{stable(9002)},
            {"Asset panel fixture"},
            SceneId{stable(9003)},
            {{SceneId{stable(9003)}, take(ProjectPath::parse("scenes/Main.sharkscene"))}}};
        SceneDocument scene{project.project,
                            project.startup_scene,
                            {},
                            {{AuthoredEntityId{stable(9004)}, {}, {"Original"}}}};
        static_cast<void>(take(files->write_scene(project, scene, std::nullopt)));
        static_cast<void>(take(files->write_project(
            take(ProjectPath::parse("Project.sharkproject")), project, std::nullopt)));
        source_scene = read(root / "scenes/Main.sharkscene");
    }
    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    Fixture(const Fixture&) = delete;
    std::string metadata(std::size_t number) {
        return take(
            encode_asset_metadata({ProjectId{stable(9001)}, asset(number), "bytes",
                                   "Shark authored offline fixture", "MIT", "shark.bytes"}));
    }
    std::string add(std::size_t number, bool present = true) {
        const auto source = "assets/item-" + std::to_string(number) + ".txt";
        if (present)
            write(root / source, "independent source " + std::to_string(number));
        const auto sidecar = source + ".sharkasset";
        write(root / sidecar, metadata(number));
        sidecars.push_back(sidecar);
        return sidecar;
    }
    std::vector<std::string_view> paths() const {
        std::vector<std::string_view> result;
        for (const auto& path : sidecars)
            result.push_back(path);
        return result;
    }
    fs::path root;
    std::string source_scene;
    std::vector<std::string> sidecars;
};
struct Workspace {
    Fixture fixture;
    std::unique_ptr<ProjectSession> session{
        take(ProjectSession::create(fixture.root, "Project.sharkproject"))};
    AssetPanel panel{*session};
    void inventory() {
        accepted(panel.refresh_catalog());
        accepted(panel.refresh_inventory(fixture.paths()));
        CHECK_FALSE(panel.page());
        accepted(panel.refresh_catalog());
        accepted(panel.refresh_cache());
    }
};
constexpr AssetAudience client_side = AssetAudience::client;
const AssetTargetProfile target{"windows-client", client_side};
AssetJobStatus ready(AssetPanel& panel, AssetImportJobId id) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (std::chrono::steady_clock::now() < deadline) {
        accepted(panel.refresh_jobs());
        const auto jobs = panel.jobs();
        const auto found =
            std::find_if(jobs.begin(), jobs.end(), [&](const auto& job) { return job.id == id; });
        REQUIRE(found != jobs.end());
        if (found->state == AssetImportJobState::ready)
            return *found;
        REQUIRE((found->state == AssetImportJobState::queued ||
                 found->state == AssetImportJobState::running));
        std::this_thread::yield();
    }
    FAIL("Import did not reach ready within its bounded deadline");
}
AssetJobStatus drain_one(AssetPanel& panel, AssetImportJobId id) {
    const auto receipt = take(panel.drain());
    CHECK(receipt.drained == 1);
    const auto found = std::find_if(receipt.jobs.begin(), receipt.jobs.end(),
                                    [&](const auto& job) { return job.id == id; });
    REQUIRE(found != receipt.jobs.end());
    return *found;
}
} // namespace

TEST_CASE("Editor asset catalog is bounded revision paged and source presence is not freshness",
          "[editor-assets]") {
    Workspace work;
    for (std::size_t index = 1; index <= 11; ++index)
        work.fixture.add(index, index != 11);
    work.inventory();
    accepted(work.panel.refresh_importers());
    CHECK(work.panel.importers().size() == 3);
    CHECK(std::any_of(work.panel.importers().begin(), work.panel.importers().end(),
                      [](const auto& importer) { return importer.id == "shark.bytes"; }));
    REQUIRE(work.panel.page());
    CHECK(work.panel.page()->records.size() == asset_panel_page_size);
    CHECK(work.panel.page()->total == 11);
    CHECK(work.panel.page()->records.front().metadata.asset == asset(1));
    REQUIRE(work.panel.page()->next_offset);
    accepted(work.panel.query_page(*work.panel.page()->next_offset));
    CHECK(work.panel.page()->records.size() == 3);
    CHECK_FALSE(work.panel.page()->next_offset);
    CHECK_FALSE(work.panel.page()->records.back().source_present);
    accepted(work.panel.select(asset(11)));
    const auto missing = take(work.panel.query_selected_cache(target));
    CHECK(missing.state == AssetCacheState::missing);
    CHECK(missing.blocked_by);
    CHECK_FALSE(missing.last_valid_key);
    const auto stale_offset = work.panel.page()->offset;
    call(*work.session, "asset.catalog.refresh",
         {{"expected_revision", call(*work.session, "asset.catalog.status").at("revision")},
          {"sidecars", Json::array()}});
    conflict(work.panel.query_page(0));
    CHECK(work.panel.page()->offset == stale_offset);
    conflict(work.panel.submit_selected(target));
    accepted(work.panel.refresh_catalog());
    CHECK(work.panel.page()->total == 0);
    CHECK_FALSE(work.panel.selection());
    const std::string oversized(asset_panel_inventory_bytes + 1, 'x');
    const std::array<std::string_view, 1> paths{oversized};
    CHECK_FALSE(work.panel.refresh_inventory(paths));
    CHECK(read(work.fixture.root / "scenes/Main.sharkscene") == work.fixture.source_scene);
}

TEST_CASE("Editor metadata uses create-only and exact replacement tokens without implicit imports",
          "[editor-assets]") {
    Workspace work;
    const std::string path = "assets/new.txt.sharkasset";
    write(work.fixture.root / "assets/new.txt", "metadata fixture source");
    accepted(work.panel.refresh_catalog());
    const auto created = take(work.panel.write_metadata(path, work.fixture.metadata(1), true));
    CHECK(created.publication.published);
    CHECK(created.publication.bytes > 0);
    REQUIRE(work.panel.metadata());
    CHECK(work.panel.metadata()->path == path);
    CHECK(work.panel.page()->total == 0);
    CHECK_FALSE(fs::exists(work.fixture.root / asset_cache_filename));
    accepted(work.panel.release_metadata());
    conflict(work.panel.write_metadata(path, work.fixture.metadata(1), true));
    accepted(work.panel.read_metadata(path));
    auto edit = Json::parse(work.panel.metadata()->json);
    edit["license"] = "MIT updated attribution";
    const auto exact = read(work.fixture.root / path);
    write(work.fixture.root / path, exact + " ");
    conflict(work.panel.write_metadata(path, edit.dump(), false));
    CHECK(read(work.fixture.root / path) == exact + " ");
    accepted(work.panel.read_metadata(path));
    const auto replaced = take(work.panel.write_metadata(path, edit.dump(), false));
    CHECK(replaced.publication.published);
    CHECK(Json::parse(read(work.fixture.root / path)).at("license") == "MIT updated attribution");
    conflict(work.panel.write_metadata("assets/another.txt.sharkasset", edit.dump(), false));
    accepted(work.panel.release_metadata());
    CHECK_FALSE(work.panel.metadata());
    CHECK_FALSE(work.panel.read_metadata("../outside.txt.sharkasset"));
    CHECK_FALSE(work.session->state().dirty);
    CHECK(call(*work.session, "scene.history").at("undo_entries") == 0);
    // Repeated explicit replacement observations release each predecessor token.
    for (std::size_t index = 0; index < 35; ++index)
        accepted(work.panel.read_metadata(path));
    accepted(work.panel.release_metadata());
    CHECK(read(work.fixture.root / "assets/new.txt") == "metadata fixture source");
}

TEST_CASE(
    "Editor jobs separate ready observation publication failure cancellation and acknowledgement",
    "[editor-assets]") {
    Workspace work;
    work.fixture.add(1);
    work.inventory();
    accepted(work.panel.select(asset(1)));
    auto initial = take(work.panel.query_selected_cache(target));
    CHECK(initial.state == AssetCacheState::missing);
    const auto admission = take(work.panel.submit_selected(target));
    CHECK_FALSE(admission.deduplicated);
    CHECK(take(work.panel.submit_selected(target)).id == admission.id);
    static_cast<void>(ready(work.panel, admission.id));
    CHECK_FALSE(fs::exists(work.fixture.root / asset_cache_filename));
    const auto published = drain_one(work.panel, admission.id);
    CHECK(published.state == AssetImportJobState::succeeded);
    REQUIRE(published.commit);
    REQUIRE(published.commit->publication);
    CHECK(published.commit->publication->published);
    REQUIRE(published.summary);
    CHECK(published.summary->root == asset(1));
    CHECK(published.summary->nodes == 1);
    CHECK(take(work.panel.cancel(admission.id)).state == AssetImportJobState::succeeded);
    accepted(work.panel.refresh_cache());
    const auto fresh = take(work.panel.query_selected_cache(target));
    CHECK(fresh.state == AssetCacheState::fresh);
    REQUIRE(fresh.last_valid_key);
    const auto artifact = take(work.panel.inspect_artifact(*fresh.last_valid_key));
    CHECK(artifact.asset == asset(1));
    CHECK(artifact.kind == "bytes");
    CHECK(artifact.importer == "shark.bytes");
    CHECK(artifact.payload_bytes > 0);
    CHECK(take(work.panel.take_job(admission.id)).state == AssetImportJobState::succeeded);
    CHECK_FALSE(work.panel.take_job(admission.id));
    write(work.fixture.root / "assets/item-1.txt", "second source");
    const auto retry = take(work.panel.submit_selected(target));
    static_cast<void>(ready(work.panel, retry.id));
    write(work.fixture.root / "assets/item-1.txt", "intervening third source");
    const auto rejected = drain_one(work.panel, retry.id);
    CHECK(rejected.state == AssetImportJobState::failed);
    REQUIRE(rejected.error);
    CHECK(rejected.error->code == ErrorCode::conflict);
    CHECK_FALSE(rejected.commit);
    CHECK(take(work.panel.take_job(retry.id)).error);
    accepted(work.panel.refresh_cache());
    const auto stale = take(work.panel.query_selected_cache(target));
    CHECK(stale.state == AssetCacheState::stale);
    CHECK(stale.last_valid_key == fresh.last_valid_key);
    const auto cancelled = take(work.panel.submit_selected(target));
    static_cast<void>(ready(work.panel, cancelled.id));
    static_cast<void>(take(work.panel.cancel(cancelled.id)));
    CHECK(drain_one(work.panel, cancelled.id).state == AssetImportJobState::cancelled);
    CHECK(take(work.panel.take_job(cancelled.id)).state == AssetImportJobState::cancelled);
    accepted(work.panel.refresh_jobs());
    CHECK(work.panel.jobs().empty());
    CHECK_FALSE(work.session->state().dirty);
    CHECK(call(*work.session, "scene.history").at("undo_entries") == 0);
    CHECK(read(work.fixture.root / "scenes/Main.sharkscene") == work.fixture.source_scene);
}

TEST_CASE(
    "Editor asset panels fence session replacement and release a running owner without publication",
    "[editor-assets]") {
    Workspace work;
    work.fixture.add(1);
    work.inventory();
    accepted(work.panel.select(asset(1)));
    const auto job = take(work.panel.submit_selected(target));
    static_cast<void>(ready(work.panel, job.id));
    bool foreign_rejected = false;
    std::thread worker([&] { foreign_rejected = !work.panel.refresh_jobs(); });
    worker.join();
    CHECK(foreign_rejected);
    const auto revision = work.session->state().revision;
    auto status = call(*work.session, "scene.mutation_status");
    auto edited = call(*work.session, "scene.mutate",
                       {{"mutation_id", status.at("next")},
                        {"expected_revision", status.at("revision")},
                        {"operation",
                         {{"type", "command"},
                          {"command",
                           {{"type", "set_name"},
                            {"entity_id", stable(9004).to_string()},
                            {"name", "Unsaved"}}}}}});
    CHECK(edited.at("outcome").at("value").at("result").at("value").at("changed") == true);
    accepted(work.panel.refresh_jobs());
    CHECK(work.session->state().dirty);
    CHECK(call(*work.session, "scene.history").at("undo_entries") == 1);
    const auto closed =
        take(work.session->close(CloseChoice::discard, work.session->state().revision));
    CHECK(closed.closed);
    CHECK_FALSE(fs::exists(work.fixture.root / asset_cache_filename));
    accepted(work.session->open("Project.sharkproject"));
    CHECK(work.session->state().revision.scene_instance != revision.scene_instance);
    conflict(work.panel.refresh_catalog());
    conflict(work.panel.cancel(job.id));
    AssetPanel next{*work.session};
    accepted(next.refresh_jobs());
    CHECK(next.jobs().empty());
    CHECK(call(*work.session, "scene.history").at("undo_entries") == 0);
    CHECK(read(work.fixture.root / "scenes/Main.sharkscene") == work.fixture.source_scene);
}
