#include "../editor/transform_drag.hpp"

#include <shark/tools/project_files.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
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
    return take(StableId::parse("17b00000-0000-4000-8000-" + std::string(12 - suffix.size(), '0') +
                                suffix));
}
AuthoredEntityId entity(std::size_t number) {
    return AuthoredEntityId{stable(number)};
}
Json call(ProjectSession& session, std::string_view method, Json parameters = Json::object()) {
    auto reply = take(session.request(method, parameters.dump()));
    REQUIRE_FALSE(reply.failed);
    auto document = Json::parse(reply.json);
    REQUIRE_FALSE(document.contains("error"));
    return document.at("result");
}
void external_edit(ProjectSession& session, Json command) {
    const auto status = call(session, "scene.mutation_status");
    const auto reply =
        call(session, "scene.mutate",
             {{"mutation_id", status.at("next")},
              {"expected_revision", status.at("revision")},
              {"operation", {{"type", "command"}, {"command", std::move(command)}}}});
    REQUIRE(reply.at("outcome").at("ok").get<bool>());
    REQUIRE(reply.at("outcome").at("value").at("result").at("ok").get<bool>());
}
TransformManipulation movement(double meters) {
    TransformManipulation result;
    result.space = TransformSpace::world;
    result.translation = {0, meters, 0};
    return result;
}
void same_transform(const Transform& left, const Transform& right) {
    CHECK(
        transform_matrices_equivalent(take(transform_matrix(left)), take(transform_matrix(right))));
}

class Fixture final {
  public:
    Fixture() {
        root = fs::temp_directory_path() /
               ("shark-editor-transform-drag-" + take(generate_stable_id()).to_string());
        REQUIRE(fs::create_directory(root));
        REQUIRE(fs::create_directory(root / "scenes"));
        auto files = take(ProjectFiles::open(root));
        ProjectDocument project{
            ProjectId{stable(9001)},
            WorldId{stable(9002)},
            {"Transform drag fixture"},
            SceneId{stable(9003)},
            {{SceneId{stable(9003)}, take(ProjectPath::parse("scenes/Main.sharkscene"))}}};
        SceneDocument scene{project.project, project.startup_scene, {}, {}};
        SourceEntity parent{entity(1), {}, {"Rotated parent"}};
        parent.transform.translation = {10, 0, 0};
        parent.transform.rotation = {std::sqrt(0.5), 0, 0, std::sqrt(0.5)};
        parent.transform.scale = {2, 2, 2};
        scene.entities.push_back(parent);
        SourceEntity child{entity(2), entity(1), {"Selected"}};
        child.transform.translation = {1, 0, 0};
        child.camera = Camera{};
        child.mesh_renderer = MeshRenderer{AssetId{stable(8001)}, 3, false};
        scene.entities.push_back(child);
        SourceEntity descendant{entity(3), entity(2), {"Large descendant"}};
        descendant.transform.scale = {1e200, 1e200, 1e200};
        scene.entities.push_back(descendant);
        scene.entities.push_back(SourceEntity{entity(4), {}, {"Other"}});
        static_cast<void>(take(files->write_scene(project, scene, std::nullopt)));
        static_cast<void>(take(files->write_project(
            take(ProjectPath::parse("Project.sharkproject")), project, std::nullopt)));
    }
    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    Fixture(const Fixture&) = delete;
    std::string bytes() const {
        std::ifstream stream{root / "scenes/Main.sharkscene", std::ios::binary};
        REQUIRE(stream);
        return {std::istreambuf_iterator<char>{stream}, {}};
    }
    fs::path root;
};
struct Workspace {
    Fixture fixture;
    std::unique_ptr<ProjectSession> session{
        take(ProjectSession::create(fixture.root, "Project.sharkproject"))};
    ScenePanel panel{*session};
    TransformDrag drag{panel};
    Workspace() {
        accepted(panel.refresh());
        accepted(panel.select(entity(2)));
    }
};
} // namespace

TEST_CASE("Editor transform drag previews preserve owner and commit one undo group",
          "[editor-transform-drag]") {
    Workspace work;
    const auto initial = work.panel.revision();
    const auto source = work.fixture.bytes();
    const auto before = take(work.panel.inspect(entity(2), initial));
    accepted(work.drag.begin());
    accepted(work.drag.update(movement(2)));
    accepted(work.drag.update(movement(4)));
    REQUIRE(work.drag.candidate());
    Transform expected;
    expected.translation = {3, 0, 0};
    same_transform(*work.drag.candidate(), expected);
    CHECK(work.panel.revision() == initial);
    CHECK_FALSE(work.session->state().dirty);
    CHECK(take(work.panel.history(initial)).undo_entries == 0);
    CHECK(call(*work.session, "scene.mutation_status").at("high_watermark") == "0");
    CHECK(work.fixture.bytes() == source);
    CHECK(take(work.panel.inspect(entity(2), initial)).transform == before.transform);
    const auto committed = take(work.drag.commit());
    CHECK(committed.changed);
    CHECK_FALSE(committed.refresh_error);
    CHECK_FALSE(work.drag.active());
    CHECK(committed.revision.revision == initial.revision + 1);
    const auto edited = take(work.panel.inspect(entity(2), committed.revision));
    same_transform(edited.transform, expected);
    CHECK(edited.name == before.name);
    CHECK(edited.camera == before.camera);
    CHECK(edited.mesh_renderer == before.mesh_renderer);
    auto history = take(work.panel.history(committed.revision));
    REQUIRE(history.next_undo);
    CHECK(history.undo_entries == 1);
    CHECK(history.next_undo->command_count == 1);
    const auto undo = take(work.panel.undo(history.next_undo->ticket, history.revision));
    same_transform(take(work.panel.inspect(entity(2), undo.revision)).transform, before.transform);
    history = take(work.panel.history(undo.revision));
    REQUIRE(history.next_redo);
    const auto redo = take(work.panel.redo(history.next_redo->ticket, history.revision));
    same_transform(take(work.panel.inspect(entity(2), redo.revision)).transform, expected);
    accepted(work.panel.select(entity(2)));
    accepted(work.drag.begin());
    accepted(work.drag.update(TransformManipulation{}));
    const auto noop = take(work.drag.commit());
    CHECK_FALSE(noop.changed);
    CHECK(noop.revision == redo.revision);
    CHECK(take(work.panel.history(noop.revision)).undo_entries == 1);
}

TEST_CASE("Editor transform drag cancellation and rejected updates never commit an older pose",
          "[editor-transform-drag]") {
    Workspace work;
    const auto revision = work.panel.revision();
    accepted(work.drag.begin());
    accepted(work.drag.update(movement(4)));
    accepted(work.drag.cancel());
    CHECK_FALSE(work.drag.active());
    CHECK_FALSE(work.drag.candidate());
    rejected(work.drag.commit(), ErrorCode::conflict);
    accepted(work.drag.begin());
    accepted(work.drag.update(movement(4)));
    TransformManipulation invalid;
    invalid.operation = TransformOperation::scale;
    invalid.scale_factors = {0, 1, 1};
    rejected(work.drag.update(invalid), ErrorCode::invalid_argument);
    CHECK_FALSE(work.drag.candidate());
    SECTION("Releasing a rejected update leaves the owner unchanged") {
        rejected(work.drag.commit(), ErrorCode::conflict);
        CHECK_FALSE(work.drag.active());
        CHECK(work.panel.revision() == revision);
        CHECK_FALSE(work.session->state().dirty);
        CHECK(take(work.panel.history(revision)).undo_entries == 0);
        CHECK(call(*work.session, "scene.mutation_status").at("high_watermark") == "0");
    }
    SECTION("A corrected cumulative update can be committed") {
        accepted(work.drag.update(movement(2)));
        const auto receipt = take(work.drag.commit());
        Transform expected;
        expected.translation = {2, 0, 0};
        same_transform(take(work.panel.inspect(entity(2), receipt.revision)).transform, expected);
    }
}

TEST_CASE("Editor transform previews reject invalid descendants through the shared owner",
          "[editor-transform-drag]") {
    Workspace work;
    const auto revision = work.panel.revision();
    accepted(work.drag.begin());
    accepted(work.drag.update(movement(4)));
    TransformManipulation too_large;
    too_large.operation = TransformOperation::scale;
    too_large.scale_factors = {1e200, 1e200, 1e200};
    const auto observed = take(work.panel.observe_transform(*work.panel.selection()));
    REQUIRE(calculate_manipulated_transform(observed.local, observed.parent_world, too_large));
    const auto rejected_preview = work.drag.update(too_large);
    rejected(rejected_preview, ErrorCode::invalid_argument);
    CHECK(rejected_preview.error().context.operation == "authoring.batch.preview");
    CHECK_FALSE(work.drag.candidate());
    rejected(work.drag.commit(), ErrorCode::conflict);
    CHECK(work.panel.revision() == revision);
    CHECK_FALSE(work.session->state().dirty);
    CHECK(take(work.panel.history(revision)).undo_entries == 0);
    CHECK(call(*work.session, "scene.mutation_status").at("high_watermark") == "0");
}

TEST_CASE("Editor transform drag fences selection history and project incarnations",
          "[editor-transform-drag]") {
    Workspace work;
    const auto initial = work.panel.revision();
    accepted(work.drag.begin());
    accepted(work.drag.update(movement(4)));
    SECTION("Selection away and back cannot revive a gesture") {
        const auto epoch = work.panel.selection_epoch();
        accepted(work.panel.select(entity(4)));
        accepted(work.panel.select(entity(2)));
        CHECK(work.panel.selection_epoch() > epoch);
    }
    SECTION("A terminal edit invalidates the captured revision") {
        external_edit(*work.session, {{"type", "set_name"},
                                      {"entity_id", entity(2).value().to_string()},
                                      {"name", "Terminal edit"}});
    }
    SECTION("Reopen cannot revive the same durable selected entity") {
        take(work.session->close(CloseChoice::reject, initial));
        accepted(work.session->open("Project.sharkproject"));
        accepted(work.panel.refresh());
        accepted(work.panel.select(entity(2)));
        CHECK(work.panel.revision().scene_instance != initial.scene_instance);
    }
    SECTION("Undo cannot revive the original revision") {
        const auto receipt = take(work.panel.apply_transform(*work.panel.selection(), Transform{}));
        const auto history = take(work.panel.history(receipt.revision));
        REQUIRE(history.next_undo);
        take(work.panel.undo(history.next_undo->ticket, history.revision));
        accepted(work.panel.select(entity(2)));
    }
    CHECK(work.drag.stale());
    rejected(work.drag.commit(), ErrorCode::conflict);
    CHECK_FALSE(work.drag.active());
    accepted(work.panel.refresh());
    const auto value = take(work.panel.inspect(entity(2), work.panel.revision()));
    Transform original;
    original.translation = {1, 0, 0};
    same_transform(value.transform, original);
}

TEST_CASE("Editor transform drags preserve owner thread and terminal save reopen parity",
          "[editor-transform-drag]") {
    Workspace editor;
    Workspace terminal;
    accepted(editor.drag.begin());
    accepted(editor.drag.update(movement(4)));
    bool foreign_rejected = false;
    std::thread worker([&] {
        const auto began = editor.drag.begin();
        const auto updated = editor.drag.update(movement(8));
        const auto committed = editor.drag.commit();
        const auto cancelled = editor.drag.cancel();
        foreign_rejected = !began && !updated && !committed && !cancelled &&
                           began.error().code == ErrorCode::conflict &&
                           updated.error().code == ErrorCode::conflict &&
                           committed.error().code == ErrorCode::conflict &&
                           cancelled.error().code == ErrorCode::conflict;
    });
    worker.join();
    CHECK(foreign_rejected);
    REQUIRE(editor.drag.candidate());
    const auto receipt = take(editor.drag.commit());
    external_edit(
        *terminal.session,
        {{"type", "set_transform"},
         {"entity_id", entity(2).value().to_string()},
         {"transform",
          {{"translation", {3, 0, 0}}, {"rotation", {0, 0, 0, 1}}, {"scale", {1, 1, 1}}}}});
    const auto terminal_revision = terminal.session->state().revision;
    take(editor.session->save(receipt.revision));
    take(terminal.session->save(terminal_revision));
    take(editor.session->close(CloseChoice::reject, receipt.revision));
    take(terminal.session->close(CloseChoice::reject, terminal_revision));
    accepted(editor.session->open("Project.sharkproject"));
    accepted(terminal.session->open("Project.sharkproject"));
    accepted(editor.panel.refresh());
    accepted(terminal.panel.refresh());
    const auto human = take(editor.panel.inspect(entity(2), editor.panel.revision()));
    const auto agent = take(terminal.panel.inspect(entity(2), terminal.panel.revision()));
    Transform expected;
    expected.translation = {3, 0, 0};
    same_transform(human.transform, expected);
    same_transform(agent.transform, expected);
    CHECK(human.name == agent.name);
    CHECK(human.camera == agent.camera);
    CHECK(human.mesh_renderer == agent.mesh_renderer);
    CHECK_FALSE(editor.session->state().dirty);
    CHECK_FALSE(terminal.session->state().dirty);
    CHECK(take(editor.panel.history(editor.panel.revision())).undo_entries == 0);
}
