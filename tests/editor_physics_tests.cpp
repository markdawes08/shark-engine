#include "../editor/physics_panel.hpp"
#include "../editor/scene_panel.hpp"
#include "../editor/script_panel.hpp"

#include <shark/prepared_assets.hpp>
#include <shark/tools/project_files.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {
using namespace shark;
using namespace shark::editor;
using namespace shark::tools;
using namespace std::chrono_literals;
using Json = nlohmann::json;

template <typename T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().message);
        INFO(result.error().context.operation);
    }
    REQUIRE(result);
    return std::move(result).value();
}
void accepted(Result<void> result) {
    if (!result)
        INFO(result.error().message);
    REQUIRE(result);
}
StableId stable(std::uint32_t number) {
    const auto suffix = std::to_string(number);
    return take(StableId::parse("18e00000-0000-4000-8000-" + std::string(12 - suffix.size(), '0') +
                                suffix));
}
AuthoredEntityId entity(std::uint32_t number) {
    return AuthoredEntityId{stable(number)};
}
Json call(ProjectSession& owner, std::string_view method, Json params = Json::object()) {
    auto reply = take(owner.request(method, params.dump()));
    INFO(reply.json);
    REQUIRE_FALSE(reply.failed);
    return Json::parse(reply.json).at("result");
}
std::string read(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input);
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}
class Fixture final {
  public:
    explicit Fixture(std::string_view source = {}) {
        root = std::filesystem::temp_directory_path() /
               ("shark-editor-physics-" + take(generate_stable_id()).to_string());
        REQUIRE(std::filesystem::create_directory(root));
        REQUIRE(std::filesystem::create_directory(root / "scripts"));
        auto files = take(ProjectFiles::open(root));
        ProjectDocument project{
            ProjectId{stable(9001)},
            WorldId{stable(9002)},
            {"Physics preview"},
            SceneId{stable(9003)},
            {{SceneId{stable(9003)}, take(ProjectPath::parse("Scene.sharkscene"))}}};
        SceneDocument scene{project.project, project.startup_scene, {}, {}};
        SourceEntity camera{entity(1), {}, {"Camera"}};
        camera.transform.translation = {0, 2, 10};
        camera.camera = Camera{CameraProjection::orthographic, 1, 20, 0.1, 100};
        scene.entities.push_back(camera);
        SourceEntity actor{entity(2), {}, {"Actor"}};
        actor.transform.translation = {0, 2, 0};
        actor.mesh_renderer = MeshRenderer{builtin_cube_asset_id()};
        actor.rigid_body = RigidBody{};
        actor.rigid_body->motion = BodyMotion::dynamic_body;
        if (!source.empty()) {
            actor.script = Script{"scripts/main.lua", true};
            static_cast<void>(
                take(files->write_text(take(ProjectPath::parse("scripts/main.lua")), source, {})));
        }
        scene.entities.push_back(actor);
        SourceEntity sensor{entity(3), {}, {"Sensor"}};
        sensor.transform.translation = {0, 2, 0};
        sensor.rigid_body = RigidBody{};
        sensor.rigid_body->half_extents = {4, 4, 4};
        sensor.rigid_body->is_trigger = true;
        scene.entities.push_back(sensor);
        static_cast<void>(take(files->write_scene(project, scene, {})));
        static_cast<void>(take(
            files->write_project(take(ProjectPath::parse("Project.sharkproject")), project, {})));
        files.reset();
        session = take(ProjectSession::create(root, "Project.sharkproject"));
    }
    ~Fixture() {
        session.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path root;
    std::unique_ptr<ProjectSession> session;
};
} // namespace

TEST_CASE("Shared physics preview keeps source history and immutable views separate",
          "[editor-physics]") {
    Fixture fixture;
    auto& session = *fixture.session;
    ScenePanel source{session};
    accepted(source.refresh());
    accepted(source.select(entity(2)));
    auto draft = take(source.inspect(entity(2), source.revision()));
    draft.name.value = "Unsaved actor";
    static_cast<void>(take(source.apply_components(draft)));
    const auto before = session.state();
    const auto history = call(session, "scene.history");
    const auto ledger = call(session, "scene.mutation_status");
    const auto bytes = read(fixture.root / "Scene.sharkscene");
    PhysicsPanel preview{session};
    accepted(preview.start(entity(1), before.revision, "physics-test", 0ns));
    REQUIRE(preview.state().active);
    REQUIRE(preview.state().session_id);
    const auto first_id = *preview.state().session_id;
    preview.pause(true);
    accepted(preview.step(1ns));
    CHECK(preview.state().tick == 1);
    CHECK(preview.state().events == 1);
    AuthoringViewportRequest request{
        entity(1), before.revision, {160, 120}, {}, 1, {"physics-test", AssetAudience::client}};
    request.physics_session = first_id;
    auto snapshot = take(session.viewport_snapshot(request));
    REQUIRE(snapshot.physics_tick());
    CHECK(*snapshot.physics_tick() == 1);
    CHECK(snapshot.revision().scene_instance != before.revision.scene_instance);
    REQUIRE(snapshot.records().size() == 1);
    CHECK(snapshot.records()[0].world_matrix[3].y < 2);
    CHECK(session.state() == before);
    CHECK(call(session, "scene.history") == history);
    CHECK(call(session, "scene.mutation_status") == ledger);
    CHECK(read(fixture.root / "Scene.sharkscene") == bytes);
    CHECK(take(source.inspect(entity(2), source.revision())).transform.translation.y == 2);
    // Runtime detach preserves visual geometry and emits its final exit, while
    // the source body remains available when editing resumes.
    accepted(preview.remove(entity(2)));
    CHECK(preview.state().events == 1);
    CHECK(take(source.inspect(entity(2), source.revision())).rigid_body.has_value());
    accepted(preview.stop());
    CHECK_FALSE(preview.state().active);
    CHECK_FALSE(session.viewport_snapshot(request));
    CHECK(snapshot.records()[0].asset);
    CHECK(session.state() == before);
    CHECK(call(session, "scene.history") == history);
    accepted(preview.start(entity(1), before.revision, "physics-test", 2ns));
    REQUIRE(preview.state().session_id);
    CHECK(*preview.state().session_id != first_id);
    CHECK(preview.state().tick == 0);
    accepted(preview.stop());
    draft = take(source.inspect(entity(2), source.revision()));
    draft.name.value = "Editing resumed";
    CHECK(take(source.apply_components(draft)).changed);
}

TEST_CASE("Physics controls reuse offline pause bounded catch-up and project teardown",
          "[editor-physics]") {
    Fixture fixture;
    auto& session = *fixture.session;
    PhysicsPanel preview{session};
    accepted(preview.start(entity(1), session.state().revision, "physics-test", 0ns));
    accepted(preview.poll(20ms, false));
    CHECK(preview.state().tick == 1);
    accepted(preview.poll(40ms, true));
    const auto paused_tick = preview.state().tick;
    accepted(preview.poll(10s, true));
    CHECK(preview.state().tick == paused_tick);
    accepted(preview.poll(11s, false));
    CHECK(preview.state().tick == paused_tick);
    accepted(preview.poll(11020ms, false));
    CHECK(preview.state().tick == paused_tick + 1);
    accepted(preview.poll(12s, false));
    CHECK(preview.state().tick <= paused_tick + 9);
    preview.pause(true);
    accepted(preview.step(13s));
    const auto stepped = preview.state().tick;
    accepted(preview.poll(14s, false));
    CHECK(preview.state().tick == stepped);
    CHECK(preview.state().alpha == 1);
    const auto closed = take(session.close(CloseChoice::reject, session.state().revision));
    CHECK(closed.closed);
    // Scoped cleanup remains safe after the owning project was closed.
    accepted(preview.stop());
    REQUIRE(session.open("Project.sharkproject"));
    CHECK_FALSE(session.state().dirty);
    accepted(preview.start(entity(1), session.state().revision, "physics-test", 15s));
    CHECK(preview.state().tick == 0);
    accepted(preview.stop());
}

TEST_CASE("Script drafts use exact file revisions and preserve external conflicts",
          "[editor-physics]") {
    Fixture fixture;
    auto& session = *fixture.session;
    const auto before = session.state();
    ScriptPanel script{session};
    accepted(script.create("scripts/main.lua"));
    CHECK(script.has_changes());
    accepted(script.edit("return 1\n"));
    accepted(script.write());
    CHECK_FALSE(script.has_changes());
    CHECK(read(fixture.root / "scripts/main.lua") == "return 1\n");
    CHECK(session.state() == before);
    accepted(script.edit("return 2\n"));
    {
        std::ofstream out{fixture.root / "scripts/main.lua", std::ios::binary | std::ios::trunc};
        out << "-- external\n";
    }
    auto rejected = script.write();
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == ErrorCode::conflict);
    CHECK(script.has_changes());
    CHECK_FALSE(script.uncertain());
    CHECK(script.text() == "return 2\n");
    CHECK(read(fixture.root / "scripts/main.lua") == "-- external\n");
    CHECK_FALSE(script.open("scripts/main.lua"));
    accepted(script.discard());
    accepted(script.open("scripts/main.lua"));
    CHECK(script.text() == "-- external\n");
    accepted(script.discard());
}

TEST_CASE("Preview exposes script output and recovers from failed source", "[editor-physics]") {
    Fixture fixture{"function on_init( invalid"};
    auto& session = *fixture.session;
    const auto before = session.state();
    PhysicsPanel preview{session};
    const auto failed = preview.start(entity(1), before.revision, "physics-test", 0ns);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().context.path == "scripts/main.lua");
    CHECK_FALSE(preview.state().uncertain);
    CHECK_FALSE(preview.state().active);
    ScriptPanel script{session};
    accepted(script.open("scripts/main.lua"));
    accepted(script.edit(R"lua(
        function on_init() shark.log.info('hello') end
        function on_fixed_update() shark.log.info('tick') end
        function on_destroy() shark.log.info('goodbye') end
    )lua"));
    accepted(script.write());
    accepted(preview.start(entity(1), before.revision, "physics-test", 1ns));
    REQUIRE(preview.state().active);
    REQUIRE(preview.script_details().size() == 1);
    CHECK(preview.script_details()[0].find("hello") != std::string::npos);
    preview.pause(true);
    accepted(preview.step(2ns));
    REQUIRE(preview.script_details().size() == 2);
    CHECK(preview.script_details()[1].find("tick") != std::string::npos);
    accepted(preview.stop());
    REQUIRE(preview.script_details().size() == 3);
    CHECK(preview.script_details()[2].find("goodbye") != std::string::npos);
    CHECK(session.state() == before);
    accepted(script.discard());
}

TEST_CASE("Faulted script preview stays quiescent after its contextual failure",
          "[editor-physics]") {
    Fixture fixture{R"lua(
        function on_fixed_update() while true do end end
    )lua"};
    auto& session = *fixture.session;
    PhysicsPanel preview{session};
    accepted(preview.start(entity(1), session.state().revision, "physics-test", 0ns));
    const auto failure = preview.poll(20ms, false);
    REQUIRE_FALSE(failure);
    CHECK(failure.error().context.path == "scripts/main.lua");
    CHECK(failure.error().context.entity_id == entity(2).value().to_string());
    CHECK(preview.state().faulted);
    CHECK_FALSE(preview.state().running);
    const auto tick = preview.state().tick;
    // Repeated display frames must not produce a generic replacement error,
    // advance the faulted owner, or invoke the callback again.
    accepted(preview.poll(40ms, false));
    accepted(preview.poll(60ms, true));
    CHECK(preview.state().faulted);
    CHECK(preview.state().tick == tick);
    accepted(preview.stop());
}
