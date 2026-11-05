#include <shark/tools/authoring_protocol.hpp>
#include <shark/tools/prefab_document.hpp>
#include <shark/tools/project_files.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

namespace {
using namespace shark;
using namespace shark::tools;
using Json = nlohmann::json;
namespace fs = std::filesystem;

template <typename T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().message);
        REQUIRE(result);
    }
    return std::move(result).value();
}
template <typename T> T id(unsigned number) {
    const auto suffix = std::to_string(number);
    return T{take(StableId::parse("21d00000-0000-4000-8000-" +
                                  std::string(12 - suffix.size(), '0') + suffix))};
}
void write(const fs::path& path, std::string_view text) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    REQUIRE(output);
}
std::string read(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input);
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}
class Fixture final {
  public:
    Fixture() {
        root = fs::temp_directory_path() /
               ("shark-prefab-protocol-" + take(generate_stable_id()).to_string());
        REQUIRE(fs::create_directory(root));
        REQUIRE(fs::create_directory(root / "assets"));
        auto files = take(ProjectFiles::open(root));
        const ProjectDocument project{
            id<ProjectId>(1),
            id<WorldId>(2),
            {"Prefab protocol"},
            id<SceneId>(3),
            {{id<SceneId>(3), take(ProjectPath::parse("Scene.sharkscene"))}}};
        Transform parent_pose;
        parent_pose.translation.x = 100;
        Transform root_pose;
        root_pose.translation.x = 5;
        Transform child_pose;
        child_pose.translation.x = 2;
        const SceneDocument scene{project.project,
                                  project.startup_scene,
                                  {},
                                  {{id<AuthoredEntityId>(90), {}, {"External parent"}, parent_pose},
                                   {id<AuthoredEntityId>(10),
                                    id<AuthoredEntityId>(90),
                                    {"Template"},
                                    root_pose,
                                    "{\"part\":\"root\"}"},
                                   {id<AuthoredEntityId>(11),
                                    id<AuthoredEntityId>(10),
                                    {"Child"},
                                    child_pose,
                                    "{\"part\":\"child\"}"}}};
        take(files->write_scene(project, scene, {}));
        take(files->write_project(take(ProjectPath::parse("Project.sharkproject")), project, {}));
        files.reset();
        owner = take(AuthoringProtocol::create(root));
        result("project.open", {{"path", "Project.sharkproject"}});
    }
    ~Fixture() {
        owner.reset();
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    Json call(std::string_view method, Json parameters = Json::object()) {
        const Json request{{"jsonrpc", "2.0"},
                           {"protocol_version", 1},
                           {"id", sequence++},
                           {"method", method},
                           {"params", std::move(parameters)}};
        const auto reply = take(owner->handle(request.dump()));
        INFO(reply.json);
        return Json::parse(reply.json);
    }
    Json result(std::string_view method, Json parameters = Json::object()) {
        auto reply = call(method, std::move(parameters));
        REQUIRE(reply.contains("result"));
        return reply.at("result");
    }
    Json revision() {
        return result("project.status").at("revision");
    }
    Json captured(unsigned asset = 20) {
        return result("prefab.capture",
                      {{"entity_id", id<AuthoredEntityId>(10).value().to_string()},
                       {"asset_id", id<AssetId>(asset).value().to_string()},
                       {"expected_revision", revision()}});
    }
    Json metadata(std::string_view path, unsigned asset = 20) {
        const Json value{{"format", "shark.asset"},
                         {"schema_version", 1},
                         {"project_id", id<ProjectId>(1).value().to_string()},
                         {"asset_id", id<AssetId>(asset).value().to_string()},
                         {"kind", "prefab"},
                         {"origin", "Shark authored test fixture"},
                         {"license", "MIT"},
                         {"importer", "shark.prefab"},
                         {"audience", "shared"},
                         {"settings", Json::object()},
                         {"dependencies", Json::array()},
                         {"extras", Json::object()}};
        return result("asset.metadata.write", {{"path", std::string{path} + ".sharkasset"},
                                               {"metadata", value},
                                               {"expected_revision", nullptr}});
    }
    Json publish(std::string_view path = "assets/Template.sharkprefab", unsigned asset = 20) {
        metadata(path, asset);
        return result("prefab.write", {{"path", path},
                                       {"document", captured(asset).at("document")},
                                       {"expected_revision", nullptr}});
    }
    Json prepare(const Json& token, Json overrides = Json::array()) {
        return result("prefab.prepare", {{"revision", token},
                                         {"expected_revision", revision()},
                                         {"parent_id", nullptr},
                                         {"overrides", overrides}});
    }
    Json instantiate(const Json& prepared) {
        return result("prefab.instantiate",
                      {{"preparation", prepared.at("preparation")},
                       {"mutation_id", result("scene.mutation_status").at("next")}});
    }
    void success(const Json& reply) {
        REQUIRE(reply.at("outcome").at("ok") == true);
        REQUIRE(reply.at("outcome").at("value").at("result").at("ok") == true);
        CHECK(reply.at("outcome").at("value").at("result").at("value").at("changed") == true);
    }
    fs::path root;
    std::unique_ptr<AuthoringProtocol> owner;
    unsigned sequence{1};
};
} // namespace

TEST_CASE("prefab capture observes a subtree without authoring or file effects",
          "[authoring-prefabs]") {
    Fixture f;
    const auto status = f.result("project.status");
    const auto bytes = read(f.root / "Scene.sharkscene");
    const auto captured = f.captured();
    const auto prefab =
        take(decode_prefab_document(captured.at("document").get<std::string>())).document;
    CHECK(prefab.project == id<ProjectId>(1));
    CHECK(prefab.asset == id<AssetId>(20));
    CHECK(prefab.root == id<AuthoredEntityId>(10));
    REQUIRE(prefab.entities.size() == 2);
    const auto root = std::find_if(prefab.entities.begin(), prefab.entities.end(),
                                   [&](const auto& entity) { return entity.id == prefab.root; });
    REQUIRE(root != prefab.entities.end());
    CHECK_FALSE(root->parent);
    CHECK(root->transform.translation.x == 5);
    CHECK(root->extras_json == "{\"part\":\"root\"}");
    CHECK(captured.at("entity_count") == 2);
    CHECK(f.result("project.status") == status);
    CHECK(read(f.root / "Scene.sharkscene") == bytes);
    CHECK_FALSE(fs::exists(f.root / "assets/Template.sharkprefab"));
}

TEST_CASE("prefab preparation preserves an immutable exact command across file edits and retries",
          "[authoring-prefabs]") {
    Fixture f;
    const auto source = f.publish();
    const auto baseline = f.result("project.status");
    const auto prepared =
        f.prepare(source.at("revision"),
                  Json::array({{{"entity_id", id<AuthoredEntityId>(10).value().to_string()},
                                {"name", "Instance override"}}}));
    CHECK(f.result("project.status") == baseline);
    const auto mutation = f.result("scene.mutation_status").at("next");
    const Json request{{"preparation", prepared.at("preparation")}, {"mutation_id", mutation}};
    const auto original_source = read(f.root / "assets/Template.sharkprefab");
    write(f.root / "assets/Template.sharkprefab", original_source + "\n");
    const auto committed = f.result("prefab.instantiate", request);
    f.success(committed);
    const auto first_revision = f.revision();
    const auto replay = f.result("prefab.instantiate", request);
    CHECK(replay.at("state") == "replayed");
    CHECK(replay.at("outcome") == committed.at("outcome"));
    CHECK(f.revision() == first_revision);
    const auto inspected =
        f.result("scene.inspect", {{"entity_id", prepared.at("root_entity_id")},
                                   {"fields", {"name", "transform", "hierarchy"}}});
    CHECK(inspected.at("entities").at(0).at("name") == "Instance override");
    CHECK(inspected.at("entities").at(0).at("hierarchy").at("parent_id").is_null());
    auto history = f.result("scene.history");
    CHECK(history.at("undo_entries") == 1);
    CHECK(history.at("next_undo").at("command_count") == 1);
    const auto ledger = f.result("scene.mutation_status");
    f.success(f.result(
        "scene.mutate",
        {{"mutation_id", ledger.at("next")},
         {"expected_revision", ledger.at("revision")},
         {"operation", {{"type", "undo"}, {"ticket", history.at("next_undo").at("ticket")}}}}));
    CHECK(
        f.call("scene.inspect", {{"entity_id", prepared.at("root_entity_id")}}).contains("error"));
}

TEST_CASE("prefab source tokens reject stale bytes foreign paths and mismatched identity",
          "[authoring-prefabs]") {
    Fixture f;
    const auto first = f.publish();
    const auto bytes = read(f.root / "assets/Template.sharkprefab");
    SECTION("stale content") {
        write(f.root / "assets/Template.sharkprefab", bytes + "\n");
        CHECK(f.call("prefab.prepare", {{"revision", first.at("revision")},
                                        {"expected_revision", f.revision()},
                                        {"parent_id", nullptr},
                                        {"overrides", Json::array()}})
                  .contains("error"));
        CHECK(f.call("prefab.write", {{"path", "assets/Template.sharkprefab"},
                                      {"document", bytes},
                                      {"expected_revision", first.at("revision")}})
                  .contains("error"));
        CHECK(read(f.root / "assets/Template.sharkprefab") == bytes + "\n");
    }
    SECTION("cross path token") {
        f.metadata("assets/Other.sharkprefab");
        CHECK(f.call("prefab.write", {{"path", "assets/Other.sharkprefab"},
                                      {"document", bytes},
                                      {"expected_revision", first.at("revision")}})
                  .contains("error"));
        CHECK_FALSE(fs::exists(f.root / "assets/Other.sharkprefab"));
    }
    SECTION("sidecar is a separate publication") {
        f.metadata("assets/Other.sharkprefab", 21);
        CHECK(f.call("prefab.write", {{"path", "assets/Other.sharkprefab"},
                                      {"document", bytes},
                                      {"expected_revision", nullptr}})
                  .contains("error"));
        CHECK(fs::exists(f.root / "assets/Other.sharkprefab.sharkasset"));
        CHECK_FALSE(fs::exists(f.root / "assets/Other.sharkprefab"));
    }
    SECTION("released token") {
        f.result("prefab.release", {{"revision", first.at("revision")}});
        CHECK(f.call("prefab.prepare", {{"revision", first.at("revision")},
                                        {"expected_revision", f.revision()},
                                        {"parent_id", nullptr},
                                        {"overrides", Json::array()}})
                  .contains("error"));
    }
    CHECK(f.result("project.status").at("dirty") == false);
}

TEST_CASE("prefab preparations retain payload identity and expire with project ownership",
          "[authoring-prefabs]") {
    Fixture f;
    const auto source = f.publish();
    const auto first = f.prepare(source.at("revision"));
    const auto second = f.prepare(source.at("revision"));
    CHECK(first.at("root_entity_id") != second.at("root_entity_id"));
    const auto mutation = f.result("scene.mutation_status").at("next");
    const auto committed = f.result("prefab.instantiate", {{"preparation", first.at("preparation")},
                                                           {"mutation_id", mutation}});
    f.success(committed);
    const auto mismatch = f.result("prefab.instantiate", {{"preparation", second.at("preparation")},
                                                          {"mutation_id", mutation}});
    CHECK(mismatch.at("state") == "payload_mismatch");
    const auto current = f.revision();
    f.result("prefab.release_preparation", {{"preparation", first.at("preparation")}});
    CHECK(f.call("prefab.instantiate",
                 {{"preparation", first.at("preparation")}, {"mutation_id", mutation}})
              .contains("error"));
    f.result("project.close", {{"unsaved", "discard"}, {"expected_revision", current}});
    f.result("project.open", {{"path", "Project.sharkproject"}});
    CHECK(f.call("prefab.prepare", {{"revision", source.at("revision")},
                                    {"expected_revision", f.revision()},
                                    {"parent_id", nullptr},
                                    {"overrides", Json::array()}})
              .contains("error"));
}
