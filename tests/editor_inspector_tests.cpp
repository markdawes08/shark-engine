#include "../editor/inspector_panel.hpp"

#include <shark/tools/project_files.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>

namespace {
using namespace shark;
using namespace shark::editor;
using namespace shark::tools;
using Json = nlohmann::json;
namespace fs = std::filesystem;

template <typename T> T take(Result<T> result) {
    REQUIRE(result);
    return std::move(result).value();
}
void accepted(Result<void> result) {
    REQUIRE(result);
}
template <typename T> void rejected(const Result<T>& result, ErrorCode code) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == code);
    CHECK_FALSE(result.error().message.empty());
}
StableId stable(std::size_t number) {
    const auto suffix = std::to_string(number);
    return take(StableId::parse("16c00000-0000-4000-8000-" + std::string(12 - suffix.size(), '0') +
                                suffix));
}
AuthoredEntityId entity(std::size_t number) {
    return AuthoredEntityId{stable(number)};
}
Json call(ProjectSession& session, std::string_view method, Json parameters = Json::object()) {
    auto reply = take(session.request(method, parameters.dump()));
    REQUIRE_FALSE(reply.failed);
    auto result = Json::parse(reply.json);
    REQUIRE_FALSE(result.contains("error"));
    return result.at("result");
}

class Fixture final {
  public:
    Fixture() {
        root = fs::temp_directory_path() /
               ("shark-editor-inspector-" + take(generate_stable_id()).to_string());
        REQUIRE(fs::create_directory(root));
        REQUIRE(fs::create_directory(root / "scenes"));
        auto files = take(ProjectFiles::open(root));
        ProjectDocument project{
            ProjectId{stable(9001)},
            WorldId{stable(9002)},
            {"Inspector fixture"},
            SceneId{stable(9003)},
            {{SceneId{stable(9003)}, take(ProjectPath::parse("scenes/Main.sharkscene"))}}};
        SceneDocument scene{project.project, project.startup_scene, {}, {}};
        scene.entities.push_back(SourceEntity{entity(1), {}, {"Original"}});
        scene.entities.push_back(SourceEntity{entity(2), {}, {"Other"}});
        static_cast<void>(take(files->write_scene(project, scene, std::nullopt)));
        static_cast<void>(take(files->write_project(
            take(ProjectPath::parse("Project.sharkproject")), project, std::nullopt)));
    }
    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    Fixture(const Fixture&) = delete;
    fs::path root;
};
struct Workspace {
    Fixture fixture;
    std::unique_ptr<ProjectSession> session{
        take(ProjectSession::create(fixture.root, "Project.sharkproject"))};
    ScenePanel scene{*session};
    InspectorPanel inspector{scene};
    Workspace() {
        accepted(scene.refresh());
        accepted(scene.select(entity(1)));
        accepted(inspector.refresh());
    }
    void select(AuthoredEntityId id) {
        accepted(scene.select(id));
        accepted(inspector.refresh());
    }
};
} // namespace

TEST_CASE("Editor inspector commits canonical component drafts as one shared history group",
          "[editor-inspector]") {
    Workspace work;
    auto& draft = *work.inspector.draft();
    CHECK_FALSE(work.inspector.dirty());
    CHECK_FALSE(work.session->state().dirty);
    const auto initial = work.scene.revision();
    auto schemas = component_schemas();
    REQUIRE(schemas.size() == 4);
    CHECK_FALSE(schemas[0].removable);
    CHECK(schemas[2].removable);
    draft.name.value = "Renamed";
    draft.transform.translation = {3, 4, 5};
    draft.transform.rotation = math::Quat{-2, 0, 0, 0};
    draft.transform.scale = {-2, 3, 4};
    draft.camera = Camera{CameraProjection::orthographic, 0.7, 12.0, 0.0, 200.0};
    draft.mesh_renderer =
        MeshRenderer{AssetId{stable(8001)}, std::numeric_limits<std::uint32_t>::max(), false};
    CHECK(work.inspector.dirty());
    auto receipt = take(work.inspector.apply());
    CHECK(receipt.changed);
    CHECK_FALSE(receipt.refresh_error);
    CHECK(receipt.entity == entity(1));
    CHECK(receipt.revision.revision == initial.revision + 1);
    CHECK(work.session->state().dirty);
    CHECK_FALSE(work.inspector.dirty());
    CHECK_FALSE(work.inspector.stale());
    const auto observed = take(work.scene.inspect(entity(1), receipt.revision));
    CHECK(observed.name.value == "Renamed");
    CHECK(observed.transform.translation == math::Vec3{3, 4, 5});
    CHECK(observed.transform.rotation == math::Quat{1, 0, 0, 0});
    CHECK(observed.transform.scale == math::Vec3{-2, 3, 4});
    REQUIRE(observed.camera);
    CHECK(observed.camera->projection == CameraProjection::orthographic);
    CHECK(observed.camera->near_clip == 0.0);
    REQUIRE(observed.mesh_renderer);
    CHECK(observed.mesh_renderer->mesh_index == std::numeric_limits<std::uint32_t>::max());
    CHECK_FALSE(observed.mesh_renderer->visible);
    auto history = take(work.scene.history(receipt.revision));
    CHECK(history.undo_entries == 1);
    REQUIRE(history.next_undo);
    CHECK(history.next_undo->command_count == 4);
    const auto no_op = take(work.inspector.apply());
    CHECK_FALSE(no_op.changed);
    CHECK(no_op.revision == receipt.revision);
    CHECK(take(work.scene.history(receipt.revision)).undo_entries == 1);
    take(work.session->save(receipt.revision));
    CHECK_FALSE(work.session->state().dirty);
    history = take(work.scene.history(receipt.revision));
    REQUIRE(history.next_undo);
    const auto undo = take(work.scene.undo(history.next_undo->ticket, history.revision));
    CHECK_FALSE(undo.entity);
    CHECK(work.session->state().dirty);
    CHECK_FALSE(work.scene.selection());
    CHECK(work.inspector.stale());
    const auto restored = take(work.scene.inspect(entity(1), undo.revision));
    CHECK(restored.name.value == "Original");
    CHECK(restored.transform == Transform{});
    CHECK_FALSE(restored.camera);
    CHECK_FALSE(restored.mesh_renderer);
    history = take(work.scene.history(undo.revision));
    REQUIRE(history.next_redo);
    const auto redo = take(work.scene.redo(history.next_redo->ticket, history.revision));
    CHECK(take(work.scene.inspect(entity(1), redo.revision)).name.value == "Renamed");
    work.select(entity(1));
    work.inspector.draft()->camera.reset();
    work.inspector.draft()->mesh_renderer.reset();
    const auto removed = take(work.inspector.apply());
    CHECK(removed.changed);
    const auto absent = take(work.scene.inspect(entity(1), removed.revision));
    CHECK_FALSE(absent.camera);
    CHECK_FALSE(absent.mesh_renderer);
}

TEST_CASE("Editor inspector rejection preserves original scene draft and shared history",
          "[editor-inspector]") {
    Workspace work;
    const auto revision = work.scene.revision();
    work.inspector.draft()->name.value = "Draft name must not partially publish";
    work.inspector.draft()->transform.scale.x = 0;
    rejected(work.inspector.apply(), ErrorCode::invalid_argument);
    CHECK(work.scene.revision() == revision);
    CHECK_FALSE(work.session->state().dirty);
    CHECK(work.inspector.dirty());
    CHECK(take(work.scene.inspect(entity(1), revision)).name.value == "Original");
    CHECK(take(work.scene.history(revision)).undo_entries == 0);
    const auto first_status = call(*work.session, "scene.mutation_status");
    CHECK(first_status.at("high_watermark") == "1");
    accepted(work.inspector.refresh());
    work.inspector.draft()->camera = Camera{};
    work.inspector.draft()->camera->far_clip = 0.05;
    rejected(work.inspector.apply(), ErrorCode::invalid_argument);
    CHECK_FALSE(take(work.scene.inspect(entity(1), revision)).camera);
    CHECK(take(work.scene.history(revision)).undo_entries == 0);
    accepted(work.inspector.refresh());
    work.inspector.draft()->transform.translation.x = std::numeric_limits<double>::infinity();
    rejected(work.inspector.apply(), ErrorCode::invalid_argument);
    accepted(work.inspector.refresh());
    work.inspector.draft()->name.value.assign(4097, 'x');
    rejected(work.inspector.apply(), ErrorCode::invalid_argument);
    CHECK(call(*work.session, "scene.mutation_status").at("high_watermark") == "2");
    CHECK_FALSE(work.scene.needs_reconciliation());
    CHECK_FALSE(work.session->state().dirty);
}

TEST_CASE("Editor inspector never retargets stale drafts or replaced entity identities",
          "[editor-inspector]") {
    Workspace work;
    work.inspector.draft()->name.value = "Retained unsent draft";
    accepted(work.scene.select(entity(2)));
    CHECK(work.inspector.stale());
    rejected(work.inspector.apply(), ErrorCode::conflict);
    CHECK(work.inspector.draft()->selection.id == entity(1));
    accepted(work.scene.select(entity(1)));
    auto external = take(work.scene.inspect(entity(1), work.scene.revision()));
    external.name.value = "Intervening edit";
    take(work.scene.apply_components(external));
    CHECK(work.inspector.stale());
    rejected(work.inspector.apply(), ErrorCode::conflict);
    CHECK(work.inspector.draft()->name.value == "Retained unsent draft");
    accepted(work.inspector.refresh());
    CHECK(work.inspector.draft()->name.value == "Intervening edit");
    const auto old = *work.inspector.draft();
    take(work.scene.erase(entity(1), work.scene.revision()));
    const auto history = take(work.scene.history(work.scene.revision()));
    REQUIRE(history.next_undo);
    take(work.scene.undo(history.next_undo->ticket, history.revision));
    accepted(work.scene.select(entity(1)));
    CHECK(work.inspector.stale());
    rejected(work.inspector.apply(), ErrorCode::conflict);
    rejected(work.scene.apply_components(old), ErrorCode::conflict);
    accepted(work.inspector.refresh());
    work.inspector.draft()->selection.id = entity(2);
    rejected(work.inspector.apply(), ErrorCode::conflict);
    work.inspector.discard();
    CHECK_FALSE(work.inspector.draft());
    CHECK_FALSE(work.inspector.dirty());
    CHECK_FALSE(work.inspector.stale());
}

TEST_CASE("Editor history requires current tickets revisions and owner thread",
          "[editor-inspector]") {
    Workspace work;
    work.inspector.draft()->name.value = "First";
    take(work.inspector.apply());
    const auto first = take(work.scene.history(work.scene.revision()));
    REQUIRE(first.next_undo);
    work.inspector.draft()->name.value = "Second";
    take(work.inspector.apply());
    const auto current = take(work.scene.history(work.scene.revision()));
    REQUIRE(current.next_undo);
    rejected(work.scene.undo(first.next_undo->ticket, first.revision), ErrorCode::conflict);
    rejected(work.scene.undo(first.next_undo->ticket, current.revision), ErrorCode::conflict);
    rejected(work.scene.redo(current.next_undo->ticket, current.revision), ErrorCode::conflict);
    CHECK(work.scene.revision() == current.revision);
    CHECK(take(work.scene.history(current.revision)).undo_entries == 2);
    CHECK(take(work.scene.inspect(entity(1), current.revision)).name.value == "Second");
    bool inspect_failed = false;
    bool history_failed = false;
    bool apply_failed = false;
    std::thread worker([&] {
        auto inspect = work.scene.inspect(entity(1), current.revision);
        inspect_failed = !inspect && inspect.error().code == ErrorCode::conflict;
        auto history = work.scene.history(current.revision);
        history_failed = !history && history.error().code == ErrorCode::conflict;
        auto apply = work.inspector.apply();
        apply_failed = !apply && apply.error().code == ErrorCode::conflict;
    });
    worker.join();
    CHECK(inspect_failed);
    CHECK(history_failed);
    CHECK(apply_failed);
    take(work.session->save(current.revision));
    CHECK(take(work.scene.history(current.revision)).undo_entries == 2);
    take(work.session->close(CloseChoice::reject, current.revision));
    accepted(work.session->open("Project.sharkproject"));
    accepted(work.scene.refresh());
    CHECK(work.scene.revision().scene_instance != current.revision.scene_instance);
    CHECK(take(work.scene.history(work.scene.revision())).undo_entries == 0);
    rejected(work.inspector.apply(), ErrorCode::conflict);
}
