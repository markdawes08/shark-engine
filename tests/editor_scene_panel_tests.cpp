#include "../editor/scene_panel.hpp"

#include <shark/tools/project_files.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
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
    CHECK_FALSE(result.error().context.operation.empty());
}

StableId stable(std::size_t number) {
    const auto suffix = std::to_string(number);
    return take(StableId::parse("16b00000-0000-4000-8000-" + std::string(12 - suffix.size(), '0') +
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
void external_edit(ProjectSession& session, Json operation) {
    const auto status = call(session, "scene.mutation_status");
    const auto result = call(session, "scene.mutate",
                             {{"mutation_id", status.at("next")},
                              {"expected_revision", status.at("revision")},
                              {"operation", std::move(operation)}});
    REQUIRE(result.at("outcome").at("ok").get<bool>());
    REQUIRE(result.at("outcome").at("value").at("result").at("ok").get<bool>());
}
const HierarchyRow& row(const ScenePanel& panel, AuthoredEntityId id) {
    const auto found = std::find_if(panel.rows().begin(), panel.rows().end(),
                                    [&](const auto& value) { return value.id == id; });
    REQUIRE(found != panel.rows().end());
    return *found;
}

class Fixture final {
  public:
    explicit Fixture(std::size_t count = 4) {
        root = fs::temp_directory_path() /
               ("shark-editor-panel-" + take(generate_stable_id()).to_string());
        REQUIRE(fs::create_directory(root));
        REQUIRE(fs::create_directory(root / "scenes"));
        auto files = take(ProjectFiles::open(root));
        ProjectDocument project{
            ProjectId{stable(9001)},
            WorldId{stable(9002)},
            {"Panel fixture"},
            SceneId{stable(9003)},
            {{SceneId{stable(9003)}, take(ProjectPath::parse("scenes/Main.sharkscene"))}}};
        SceneDocument scene{project.project, project.startup_scene, {}, {}};
        for (std::size_t index = 1; index <= count; ++index) {
            SourceEntity value{entity(index), {}, {"Entity " + std::to_string(index)}};
            if (index == 1)
                value.transform.translation = {10, 0, 0};
            if (index == 2) {
                value.parent = entity(1);
                value.transform.translation = {2, 0, 0};
            }
            if (index == 3)
                value.camera = Camera{};
            if (index == 4) {
                value.mesh_renderer = MeshRenderer{AssetId{stable(8001)}, 0, true};
                value.transform.translation = {20, 0, 0};
            }
            scene.entities.push_back(std::move(value));
        }
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
    SceneDocument saved() const {
        std::ifstream input{root / "scenes/Main.sharkscene", std::ios::binary};
        REQUIRE(input);
        return take(decode_scene_document(std::string{std::istreambuf_iterator<char>{input}, {}}))
            .document;
    }
};
} // namespace

TEST_CASE("Editor hierarchy consumes bounded protocol pages and displays a stable tree",
          "[editor-scene-panel]") {
    Fixture fixture{270};
    auto session = take(ProjectSession::create(fixture.root, "Project.sharkproject"));
    ScenePanel panel{*session};
    accepted(panel.refresh());
    REQUIRE(panel.current());
    REQUIRE(panel.rows().size() == 270);
    CHECK(panel.page_count() == 5);
    CHECK(panel.page().size() == 64);
    CHECK(row(panel, entity(2)).parent == entity(1));
    CHECK(row(panel, entity(2)).depth == 2);
    CHECK(row(panel, entity(1)).child_count == 1);
    CHECK(row(panel, entity(3)).has_camera);
    CHECK(row(panel, entity(4)).has_mesh_renderer);
    accepted(panel.select(entity(2)));
    for (std::size_t index = 0; index < 4; ++index)
        REQUIRE(panel.next_page());
    CHECK(panel.page().size() == 14);
    CHECK_FALSE(panel.next_page());
    CHECK(panel.selection()->id == entity(2));
    CHECK(panel.previous_page());
    for (int index = 0; index < 3; ++index) {
        accepted(panel.refresh());
        CHECK(call(*session, "project.status").at("cursors").at("retained") == 0);
    }
    const auto moved =
        take(panel.reparent(entity(1), entity(270), ReparentMode::keep_local, panel.revision()));
    REQUIRE(moved.changed);
    REQUIRE_FALSE(moved.refresh_error);
    const auto parent = std::find_if(panel.rows().begin(), panel.rows().end(),
                                     [](const auto& value) { return value.id == entity(270); });
    REQUIRE(parent != panel.rows().end());
    REQUIRE(std::distance(parent, panel.rows().end()) >= 3);
    CHECK((parent + 1)->id == entity(1));
    CHECK((parent + 2)->id == entity(2));
    CHECK((parent + 2)->depth == 3);
    CHECK(call(*session, "project.status").at("cursors").at("retained") == 0);
}

TEST_CASE("Editor entity controls share retained validation history and saved source",
          "[editor-scene-panel]") {
    Fixture fixture;
    auto session = take(ProjectSession::create(fixture.root, "Project.sharkproject"));
    ScenePanel panel{*session};
    accepted(panel.refresh());
    const auto initial = panel.revision();
    const auto created = take(panel.create("New child", entity(1), initial));
    REQUIRE(created.changed);
    REQUIRE_FALSE(created.refresh_error);
    CHECK(session->state().dirty);
    CHECK(panel.selection()->id == created.entity);
    CHECK(row(panel, created.entity).parent == entity(1));
    CHECK(row(panel, entity(1)).child_count == 2);
    CHECK(call(*session, "scene.history").at("undo_entries") == 1);

    const auto before_rejection = call(*session, "scene.mutation_status");
    rejected(panel.create(std::string(257, 'x'), {}, panel.revision()),
             ErrorCode::invalid_argument);
    CHECK_FALSE(panel.needs_reconciliation());
    CHECK(panel.current());
    CHECK(panel.rows().size() == 5);
    CHECK(call(*session, "scene.history").at("undo_entries") == 1);
    CHECK(call(*session, "scene.mutation_status").at("high_watermark") !=
          before_rejection.at("high_watermark"));
    rejected(panel.erase(created.entity, initial), ErrorCode::conflict);
    CHECK(panel.rows().size() == 5);
    const auto history = call(*session, "scene.history");
    const auto saved = take(session->save(panel.revision()));
    CHECK(saved.published);
    CHECK_FALSE(session->state().dirty);
    CHECK(call(*session, "scene.history") == history);
    const auto source = fixture.saved();
    const auto stored = std::find_if(source.entities.begin(), source.entities.end(),
                                     [&](const auto& value) { return value.id == created.entity; });
    REQUIRE(stored != source.entities.end());
    CHECK(stored->name.value == "New child");
    CHECK(stored->parent == entity(1));

    const auto deleted = take(panel.erase(entity(1), panel.revision()));
    CHECK(deleted.changed);
    CHECK_FALSE(panel.selection());
    REQUIRE(panel.rows().size() == 2);
    CHECK(panel.rows()[0].id == entity(3));
    CHECK(panel.rows()[1].id == entity(4));
    CHECK(session->state().dirty);
}

TEST_CASE("Editor reparent modes retain shared transform and cycle rejection rules",
          "[editor-scene-panel]") {
    Fixture fixture;
    auto session = take(ProjectSession::create(fixture.root, "Project.sharkproject"));
    ScenePanel panel{*session};
    accepted(panel.refresh());
    accepted(panel.select(entity(2)));
    const auto initial = panel.revision();
    rejected(panel.reparent(entity(1), entity(2), ReparentMode::keep_local, initial),
             ErrorCode::conflict);
    CHECK(panel.revision() == initial);
    CHECK_FALSE(panel.needs_reconciliation());
    CHECK(call(*session, "scene.history").at("undo_entries") == 0);
    const auto moved =
        take(panel.reparent(entity(2), entity(4), ReparentMode::keep_world, initial));
    CHECK(moved.changed);
    CHECK(row(panel, entity(2)).parent == entity(4));
    const auto inspect =
        call(*session, "scene.inspect",
             {{"entity_id", entity(2).value().to_string()}, {"fields", {"transform"}}});
    const auto& transform = inspect.at("entities").at(0).at("transform");
    CHECK(transform.at("local").at("translation") == Json::array({-8.0, 0.0, 0.0}));
    CHECK(transform.at("world_matrix").at("values").at(12) == 12.0);
    const auto detached =
        take(panel.reparent(entity(2), {}, ReparentMode::keep_local, panel.revision()));
    CHECK(detached.changed);
    CHECK_FALSE(row(panel, entity(2)).parent);
    const auto detached_state =
        call(*session, "scene.inspect",
             {{"entity_id", entity(2).value().to_string()}, {"fields", {"transform"}}});
    CHECK(detached_state.at("entities")
              .at(0)
              .at("transform")
              .at("world_matrix")
              .at("values")
              .at(12) == -8.0);
}

TEST_CASE("Editor selection cannot silently rebind across external edits undo or reopen",
          "[editor-scene-panel]") {
    Fixture fixture;
    auto session = take(ProjectSession::create(fixture.root, "Project.sharkproject"));
    ScenePanel panel{*session};
    accepted(panel.refresh());
    accepted(panel.select(entity(2)));
    const auto old_revision = panel.revision();
    external_edit(
        *session,
        {{"type", "command"},
         {"command", {{"type", "delete_entity"}, {"entity_id", entity(2).value().to_string()}}}});
    const auto history = call(*session, "scene.history");
    external_edit(*session, {{"type", "undo"}, {"ticket", history.at("next_undo").at("ticket")}});
    CHECK_FALSE(panel.current());
    rejected(panel.erase(entity(2), old_revision), ErrorCode::conflict);
    accepted(panel.refresh());
    CHECK_FALSE(panel.selection());
    CHECK(row(panel, entity(2)).name == "Entity 2");
    accepted(panel.select(entity(2)));
    const auto before_close = panel.revision();
    const auto closed = take(session->close(CloseChoice::discard, before_close));
    REQUIRE(closed.closed);
    accepted(session->open("Project.sharkproject"));
    accepted(panel.refresh());
    CHECK(panel.revision().scene_instance != before_close.scene_instance);
    CHECK_FALSE(panel.selection());
    rejected(panel.erase(entity(2), before_close), ErrorCode::conflict);
    rejected(panel.select(entity(9000)), ErrorCode::conflict);
    rejected(panel.reconcile(), ErrorCode::conflict);
    Result<void> foreign{Error{}};
    std::thread worker([&] { foreign = panel.refresh(); });
    worker.join();
    rejected(foreign, ErrorCode::conflict);
    CHECK(panel.current());
}
