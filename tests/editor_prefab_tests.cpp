#include "../editor/physics_panel.hpp"
#include "../editor/prefab_panel.hpp"
#include "../editor/scene_panel.hpp"

#include <shark/tools/prefab_document.hpp>
#include <shark/tools/project_files.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

namespace {
using namespace shark;
using namespace shark::tools;
using namespace shark::editor;
using Json = nlohmann::json;
namespace fs = std::filesystem;

template <typename T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().message);
        REQUIRE(result);
    }
    return std::move(result).value();
}
void accepted(Result<void> result) {
    if (!result) {
        INFO(result.error().message);
        REQUIRE(result);
    }
}
template <typename T> T id(unsigned number) {
    const auto suffix = std::to_string(number);
    return T{take(StableId::parse("21d00000-0000-4000-8000-" +
                                  std::string(12 - suffix.size(), '0') + suffix))};
}
std::string read(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input);
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}
void write(const fs::path& path, std::string_view contents) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    REQUIRE(output);
}
class Fixture final {
  public:
    Fixture() {
        root = fs::temp_directory_path() /
               ("shark-editor-prefabs-" + take(generate_stable_id()).to_string());
        REQUIRE(fs::create_directory(root));
        REQUIRE(fs::create_directory(root / "assets"));
        auto files = take(ProjectFiles::open(root));
        const ProjectDocument project{
            id<ProjectId>(1),
            id<WorldId>(2),
            {"Prefab panel"},
            id<SceneId>(3),
            {{id<SceneId>(3), take(ProjectPath::parse("Scene.sharkscene"))}}};
        Transform parent_pose;
        parent_pose.translation.x = 100;
        Transform root_pose;
        root_pose.translation.x = 5;
        Transform child_pose;
        child_pose.translation.x = 2;
        SourceEntity camera{id<AuthoredEntityId>(30), {}, {"Camera"}};
        camera.camera = Camera{};
        camera.transform.translation = {0, 2, 10};
        const SourceEntity captured_root{id<AuthoredEntityId>(10),
                                         id<AuthoredEntityId>(90),
                                         {"Template"},
                                         root_pose,
                                         "{\"root\":true}"};
        const SourceEntity captured_child{id<AuthoredEntityId>(11),
                                          id<AuthoredEntityId>(10),
                                          {"Child"},
                                          child_pose,
                                          "{\"child\":true}"};
        const SceneDocument scene{project.project,
                                  project.startup_scene,
                                  {},
                                  {{id<AuthoredEntityId>(90), {}, {"Parent"}, parent_pose},
                                   captured_root,
                                   captured_child,
                                   camera}};
        take(files->write_scene(project, scene, {}));
        take(files->write_project(take(ProjectPath::parse("Project.sharkproject")), project, {}));
        for (const auto& [path, asset] : {std::pair{"assets/Template.sharkprefab", 20U},
                                          std::pair{"assets/Captured.sharkprefab", 21U}}) {
            const AssetMetadata metadata{project.project, id<AssetId>(asset),
                                         "prefab",        "Shark authored offline fixture",
                                         "MIT",           "shark.prefab"};
            take(files->write_asset_metadata(
                take(ProjectPath::parse(std::string{path} + ".sharkasset")), metadata, {}));
        }
        auto entity_root = captured_root;
        entity_root.parent.reset();
        const PrefabDocument prefab{
            project.project, id<AssetId>(20), entity_root.id, {captured_child, entity_root}};
        write(root / "assets/Template.sharkprefab", take(encode_prefab_document(prefab)));
        files.reset();
        session = take(ProjectSession::create(root, "Project.sharkproject"));
    }
    ~Fixture() {
        session.reset();
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    fs::path root;
    std::unique_ptr<ProjectSession> session;
};
} // namespace

TEST_CASE("Editor prefab preparation and instantiation share source isolation and history",
          "[editor-prefabs]") {
    Fixture fixture;
    auto& session = *fixture.session;
    PrefabPanel panel{session};
    ScenePanel scene{session};
    accepted(scene.refresh());
    const auto before = session.state();
    const auto bytes = read(fixture.root / "assets/Template.sharkprefab");
    accepted(panel.open("assets/Template.sharkprefab"));
    CHECK(panel.loaded());
    CHECK_FALSE(panel.has_changes());
    accepted(panel.prepare());
    REQUIRE(panel.prepared());
    REQUIRE(panel.prepared_root());
    const auto root = *panel.prepared_root();
    CHECK(session.state() == before);
    const auto effect = take(panel.instantiate());
    CHECK(effect.changed);
    CHECK(effect.revision.revision == before.revision.revision + 1);
    accepted(scene.refresh());
    const auto root_values = take(scene.inspect(root, scene.revision()));
    CHECK(root_values.name == Name{"Template"});
    CHECK(root_values.transform.translation.x == 5);
    auto history = take(scene.history(scene.revision()));
    REQUIRE(history.next_undo);
    CHECK(history.undo_entries == 1);
    CHECK(history.next_undo->command_count == 1);
    CHECK(take(scene.undo(history.next_undo->ticket, history.revision)).changed);
    accepted(scene.refresh());
    CHECK_FALSE(scene.inspect(root, scene.revision()));
    history = take(scene.history(scene.revision()));
    REQUIRE(history.next_redo);
    CHECK(take(scene.redo(history.next_redo->ticket, history.revision)).changed);
    CHECK(read(fixture.root / "assets/Template.sharkprefab") == bytes);
    accepted(panel.discard());
}

TEST_CASE("Editor prefab capture and explicit source publication preserve the authored scene",
          "[editor-prefabs]") {
    Fixture fixture;
    auto& session = *fixture.session;
    PrefabPanel panel{session};
    const auto before = session.state();
    const auto scene_bytes = read(fixture.root / "Scene.sharkscene");
    accepted(panel.capture("assets/Captured.sharkprefab", id<AuthoredEntityId>(10), id<AssetId>(21),
                           before.revision));
    CHECK(panel.loaded());
    CHECK(panel.has_changes());
    CHECK_FALSE(fs::exists(fixture.root / "assets/Captured.sharkprefab"));
    const auto captured = take(decode_prefab_document(panel.text())).document;
    const auto root = std::find_if(captured.entities.begin(), captured.entities.end(),
                                   [&](const auto& entity) { return entity.id == captured.root; });
    REQUIRE(root != captured.entities.end());
    CHECK_FALSE(root->parent);
    CHECK(root->transform.translation.x == 5);
    accepted(panel.write());
    CHECK_FALSE(panel.has_changes());
    CHECK(fs::exists(fixture.root / "assets/Captured.sharkprefab"));
    CHECK(session.state() == before);
    CHECK(read(fixture.root / "Scene.sharkscene") == scene_bytes);
    accepted(panel.discard());
}

TEST_CASE("Editor prefab conflicts preserve drafts and prevent stale scene publication",
          "[editor-prefabs]") {
    Fixture fixture;
    auto& session = *fixture.session;
    PrefabPanel panel{session};
    accepted(panel.open("assets/Template.sharkprefab"));
    SECTION("external source conflict") {
        const auto original = panel.text();
        accepted(panel.edit(original + "\n"));
        write(fixture.root / "assets/Template.sharkprefab", original + "\n\n");
        const auto before = session.state();
        CHECK_FALSE(panel.write());
        CHECK(panel.has_changes());
        CHECK(panel.text() == original + "\n");
        CHECK(read(fixture.root / "assets/Template.sharkprefab") == original + "\n\n");
        CHECK(session.state() == before);
    }
    SECTION("stale prepared scene revision") {
        accepted(panel.prepare());
        ScenePanel scene{session};
        accepted(scene.refresh());
        CHECK(take(scene.create("Unrelated", {}, scene.revision())).changed);
        const auto before = session.state();
        const auto rejected = panel.instantiate();
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().code == ErrorCode::conflict);
        CHECK(session.state() == before);
        CHECK_FALSE(panel.instantiation_uncertain());
    }
    accepted(panel.discard());
}

TEST_CASE("Editor prefab reconciliation compares canonical content after an IO failure",
          "[editor-prefabs]") {
    Fixture fixture;
    auto& session = *fixture.session;
    PrefabPanel panel{session};
    const auto before = session.state();
    accepted(panel.capture("assets/Captured.sharkprefab", id<AuthoredEntityId>(10), id<AssetId>(21),
                           before.revision));
    const auto captured = Json::parse(panel.text());
    nlohmann::ordered_json reordered;
    for (const auto* key : {"entities", "root_entity", "asset_id", "project_id", "schema_version",
                            "format", "extras"})
        reordered[key] = captured.at(key);
    const auto draft = "\n" + reordered.dump() + "\n\n";
    const auto canonical =
        take(encode_prefab_document(take(decode_prefab_document(draft)).document));
    REQUIRE(draft != canonical);
    accepted(panel.edit(draft));
    const auto sidecar = fixture.root / "assets/Captured.sharkprefab.sharkasset";
    const auto metadata = read(sidecar);
    REQUIRE(fs::remove(sidecar));
    const auto failed = panel.write();
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == ErrorCode::io_failure);
    REQUIRE(panel.uncertain());
    CHECK(panel.text() == draft);
    // This is an observed IO failure followed by independent publication, not
    // a claim that the test dropped an actual successful completion.
    write(sidecar, metadata);
    write(fixture.root / "assets/Captured.sharkprefab", canonical);
    accepted(panel.reconcile());
    CHECK_FALSE(panel.uncertain());
    CHECK_FALSE(panel.has_changes());
    CHECK(panel.text() == canonical);
    CHECK(panel.notice().find("durability is unknown") != std::string::npos);
    CHECK(session.state() == before);
    accepted(panel.discard());
}

TEST_CASE("Editor clean prefab panels release expired observations across project reopen",
          "[editor-prefabs]") {
    Fixture fixture;
    auto& session = *fixture.session;
    PrefabPanel panel{session};
    accepted(panel.open("assets/Template.sharkprefab"));
    REQUIRE_FALSE(panel.has_changes());
    const auto original = session.state().revision;
    CHECK(take(session.close(CloseChoice::reject, original)).closed);
    accepted(session.open("Project.sharkproject"));
    CHECK(session.state().revision.scene_instance != original.scene_instance);
    SECTION("explicit discard then open") {
        accepted(panel.discard());
        CHECK_FALSE(panel.loaded());
        accepted(panel.open("assets/Template.sharkprefab"));
    }
    SECTION("replace clean observation directly") {
        accepted(panel.open("assets/Template.sharkprefab"));
    }
    CHECK(panel.loaded());
    CHECK_FALSE(panel.has_changes());
    CHECK_FALSE(panel.instantiation_uncertain());
    accepted(panel.prepare());
    CHECK(panel.prepared());
    accepted(panel.release_preparation());
    accepted(panel.discard());
}

TEST_CASE("Editor prefab publication and instantiation reject while preview is active",
          "[editor-prefabs]") {
    Fixture fixture;
    auto& session = *fixture.session;
    PrefabPanel panel{session};
    PhysicsPanel preview{session};
    accepted(panel.open("assets/Template.sharkprefab"));
    accepted(panel.prepare());
    const auto before = session.state();
    accepted(preview.start(id<AuthoredEntityId>(30), before.revision, "prefab-test",
                           std::chrono::nanoseconds{0}));
    CHECK_FALSE(panel.instantiate());
    CHECK_FALSE(panel.instantiation_uncertain());
    accepted(panel.release_preparation());
    CHECK_FALSE(panel.prepare());
    const auto original = panel.text();
    accepted(panel.edit(original + "\n"));
    CHECK_FALSE(panel.write());
    CHECK(panel.text() == original + "\n");
    CHECK(read(fixture.root / "assets/Template.sharkprefab") == original);
    CHECK(session.state() == before);
    accepted(preview.stop());
    accepted(panel.discard());
}
