// Original Shark-authored source, geometry and publication fixtures; MIT license.
#include "../editor/world_cell_panel.hpp"
#include "project_files_native.hpp"
#include "shark/tools/authoring_protocol.hpp"
#include "shark/tools/gltf_asset.hpp"
#include "shark/tools/project_files.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

namespace {
using namespace shark;
using namespace shark::tools;
using Json = nlohmann::json;
namespace fs = std::filesystem;
template <class T> T take(Result<T> value) {
    if (!value)
        INFO(value.error().message);
    REQUIRE(value);
    return std::move(value).value();
}
template <class Id> Id id(char digit) {
    std::string text{"650d0000-0000-4000-8000-000000000001"};
    text.back() = digit;
    return Id{take(StableId::parse(text))};
}
ProjectPath path(std::string_view value) {
    return take(ProjectPath::parse(value));
}
std::string read(const fs::path& value) {
    std::ifstream in(value, std::ios::binary);
    REQUIRE(in);
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}
void write(const fs::path& value, std::string_view data) {
    std::ofstream out(value, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    REQUIRE(out);
}
struct Fault {
    detail::FileTestControl prior;
    explicit Fault(detail::FileFault fault)
        : prior(detail::set_file_test_control({fault, nullptr, nullptr})) {}
    ~Fault() {
        static_cast<void>(detail::set_file_test_control(prior));
    }
};
struct Fixture {
    fs::path root =
        fs::current_path() / ("shark-cell-author-" + take(generate_stable_id()).to_string());
    std::unique_ptr<AuthoringProtocol> owner;
    std::string project_source, scene_source, buffer;
    explicit Fixture(bool imported = false, bool camera = false) {
        REQUIRE(fs::create_directory(root));
        auto files = take(ProjectFiles::open(root));
        const ProjectDocument project{id<ProjectId>('1'),
                                      id<WorldId>('2'),
                                      Name{"Cell authoring"},
                                      id<SceneId>('3'),
                                      {{id<SceneId>('3'), path("Scene.sharkscene")}},
                                      "{\"private_project_metadata\":true}"};
        SourceEntity ground{id<AuthoredEntityId>('4'), {}, Name{"Ground"}};
        ground.transform.translation = {2, 0, 2};
        ground.transform.scale = {4, 4, 4};
        ground.mesh_renderer =
            MeshRenderer{imported ? id<AssetId>('8') : builtin_plane_asset_id(), 0, false};
        ground.extras_json = "{\"private_cell_metadata\":true}";
        if (camera)
            ground.camera = Camera{};
        const SceneDocument scene{project.project,
                                  project.startup_scene,
                                  WorldPosition{project.world, {1'000'000'000, 0, 0}, {0, 0, 0}},
                                  {ground},
                                  "{}"};
        take(files->write_scene(project, scene, {}));
        take(files->write_project(path("Project.sharkproject"), project, {}));
        if (imported) {
            for (float value : std::array<float, 9>{0, 0, 0, 0, 0, 1, 1, 0, 0}) {
                const auto bits = std::bit_cast<std::uint32_t>(value);
                for (unsigned shift = 0; shift < 32; shift += 8)
                    buffer.push_back(static_cast<char>((bits >> shift) & 255U));
            }
            write(root / "triangle.bin", buffer);
            write(
                root / "model.gltf",
                R"({"asset":{"version":"2.0"},"buffers":[{"uri":"triangle.bin","byteLength":36}],"bufferViews":[{"buffer":0,"byteLength":36}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})");
            const AssetMetadata child{project.project,
                                      id<AssetId>('9'),
                                      "gltf-resource",
                                      "Engine fixture",
                                      "MIT",
                                      "shark.gltf-resource",
                                      AssetAudience::shared,
                                      "{}",
                                      {}};
            const auto settings =
                Json{
                    {"path", "model.gltf"},
                    {"resources", Json::array({{{"path", "triangle.bin"},
                                                {"asset", id<AssetId>('9').value().to_string()}}})}}
                    .dump();
            const AssetMetadata model{
                project.project, id<AssetId>('8'),      "gltf",   "Engine fixture",  "MIT",
                "shark.gltf",    AssetAudience::server, settings, {id<AssetId>('9')}};
            take(files->write_asset_metadata(path("triangle.bin.sharkasset"), child, {}));
            take(files->write_asset_metadata(path("model.gltf.sharkasset"), model, {}));
            const auto catalog = take(AssetCatalog::create(*files, project.project));
            const std::array inventory{path("triangle.bin.sharkasset"),
                                       path("model.gltf.sharkasset")};
            take(catalog->refresh(inventory, take(catalog->revision())));
            GltfAssetImporter gltf;
            GltfResourceImporter resource;
            ByteAssetImporter bytes;
            const std::array<AssetImporter*, 3> importers{&gltf, &resource, &bytes};
            const auto imports =
                take(AssetImports::open(*files, *catalog, project.project, importers));
            auto prepared =
                take(imports->prepare(id<AssetId>('8'), {"local-server", AssetAudience::server},
                                      take(catalog->revision()), take(imports->revision())));
            take(imports->publish(std::move(prepared), take(catalog->revision()),
                                  take(imports->revision())));
        }
        files.reset();
        project_source = read(root / "Project.sharkproject");
        scene_source = read(root / "Scene.sharkscene");
        owner = take(AuthoringProtocol::create(root));
        result("project.open", {{"path", "Project.sharkproject"}});
        if (imported) {
            const auto revision = result("asset.catalog.status").at("revision");
            result("asset.catalog.refresh",
                   {{"expected_revision", revision},
                    {"sidecars", {"triangle.bin.sharkasset", "model.gltf.sharkasset"}}});
        }
    }
    ~Fixture() {
        owner.reset();
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    Json call(std::string_view method, Json params = Json::object()) {
        const auto reply = take(owner->handle(Json{
            {"jsonrpc", "2.0"},
            {"protocol_version", 1},
            {"id", "cell-fixture"},
            {"method", method},
            {"params", std::move(params)}}.dump()));
        INFO(reply.json);
        return Json::parse(reply.json);
    }
    Json result(std::string_view method, Json params = Json::object()) {
        auto reply = call(method, std::move(params));
        REQUIRE_FALSE(reply.contains("error"));
        return reply.at("result");
    }
    Json request(std::string output = "World.sharkcell", bool prepare = true) {
        Json parameters{{"expected_revision", result("project.status").at("revision")},
                        {"expected_catalog", result("asset.catalog.status").at("revision")},
                        {"expected_cache", result("asset.cache.status").at("revision")},
                        {"world_id", id<WorldId>('2').value().to_string()},
                        {"cell", {{"x", 1'000'000'000}, {"y", 0}, {"z", 0}}},
                        {"content_revision", "17"},
                        {"target", {{"id", "local-server"}, {"side", "server"}}},
                        {"collision_entities", {id<AuthoredEntityId>('4').value().to_string()}},
                        {"navigation_entities", {id<AuthoredEntityId>('4').value().to_string()}}};
        const auto digest = prepare ? result("scene.cell.prepare", parameters).at("payload_digest")
                                    : Json(std::string(64, '0'));
        parameters["path"] = std::move(output);
        parameters["expected_digest"] = digest;
        return parameters;
    }
    void unchanged() {
        CHECK(read(root / "Project.sharkproject") == project_source);
        CHECK(read(root / "Scene.sharkscene") == scene_source);
    }
};
} // namespace

TEST_CASE("terminal cell export preserves dirty owner history and publishes independently "
          "inspected bytes",
          "[world-cell-authoring]") {
    Fixture f;
    const auto state = f.result("scene.mutation_status");
    const auto edit =
        f.result("scene.mutate", {{"mutation_id", state.at("next")},
                                  {"expected_revision", state.at("revision")},
                                  {"operation",
                                   {{"type", "command"},
                                    {"command",
                                     {{"type", "set_name"},
                                      {"entity_id", id<AuthoredEntityId>('4').value().to_string()},
                                      {"name", "Edited ground"}}}}}});
    REQUIRE(edit.at("outcome").at("ok").get<bool>());
    const auto before = f.result("project.status"), history = f.result("scene.history"),
               mutations = f.result("scene.mutation_status");
    const auto request = f.request();
    CHECK_FALSE(fs::exists(f.root / "World.sharkcell"));
    CHECK(f.result("project.status") == before);
    const auto receipt = f.result("scene.cell.export", request);
    CHECK(receipt.at("payload_digest").get<std::string>() ==
          request.at("expected_digest").get<std::string>());
    CHECK(receipt.at("publication").at("published").get<bool>());
    CHECK(receipt.at("dirty").get<bool>());
    CHECK(receipt.at("vertices").get<std::size_t>() == 4);
    CHECK(receipt.at("collision_triangles").get<std::size_t>() == 2);
    CHECK(receipt.at("navigation_triangles").get<std::size_t>() == 2);
    CHECK_FALSE(receipt.at("runtime_assets_embedded").get<bool>());
    CHECK(receipt.at("mesh_closure_checked").get<bool>());
    const auto raw = read(f.root / "World.sharkcell");
    CHECK(raw.substr(0, 8) == "SHKCELL1");
    CHECK(raw.find("private_cell_metadata") == std::string::npos);
    const auto bytes = std::as_bytes(std::span{raw.data(), raw.size()});
    CHECK(take(sha256(bytes)).to_hex() == receipt.at("payload_digest").get<std::string>());
    const auto loaded = take(LoadedWorldCell::load(bytes, take(PreparedAssets::create())));
    REQUIRE(loaded->geometry());
    CHECK(loaded->geometry()->vertices ==
          std::vector<math::Vec3>{{0, 0, 4}, {4, 0, 4}, {4, 0, 0}, {0, 0, 0}});
    const auto observed = f.result("cell.inspect", {{"path", "World.sharkcell"}});
    CHECK(observed.at("payload_digest").get<std::string>() ==
          receipt.at("payload_digest").get<std::string>());
    CHECK_FALSE(observed.at("mesh_closure_checked").get<bool>());
    CHECK_FALSE(observed.contains("publication"));
    CHECK_FALSE(observed.contains("original_receipt"));
    CHECK(f.call("scene.cell.export", f.request()).contains("error"));
    CHECK(read(f.root / "World.sharkcell") == raw);
    CHECK(f.result("project.status") == before);
    CHECK(f.result("scene.history") == history);
    CHECK(f.result("scene.mutation_status") == mutations);
    f.unchanged();
}

TEST_CASE("cell export fences revisions keys paths static profiles and prepublication failures",
          "[world-cell-authoring]") {
    SECTION("malformed and mismatched requests never publish") {
        Fixture f;
        const auto good = f.request();
        for (const auto field : {"expected_revision", "expected_catalog", "expected_cache"}) {
            auto stale = good;
            const auto key = field == std::string_view{"expected_revision"} ? "revision" : "value";
            stale[field][key] =
                std::to_string(std::stoull(good.at(field).at(key).get<std::string>()) + 1);
            CHECK(f.call("scene.cell.export", stale).contains("error"));
        }
        auto wrong = good;
        wrong["cell"]["x"] = 1'000'000'001;
        CHECK(f.call("scene.cell.export", wrong).contains("error"));
        wrong = good;
        wrong["collision_entities"] = Json::array();
        CHECK(f.call("scene.cell.export", wrong).contains("error"));
        wrong = good;
        wrong["content_revision"] = "01";
        CHECK(f.call("scene.cell.export", wrong).contains("error"));
        for (const auto name : {"../Outside.sharkcell", "Scene.sharkscene"}) {
            wrong = good;
            wrong["path"] = name;
            CHECK(f.call("scene.cell.export", wrong).contains("error"));
        }
        wrong = good;
        wrong["unexpected"] = true;
        CHECK(f.call("scene.cell.export", wrong).contains("error"));
        wrong = good;
        wrong["expected_digest"] = std::string(64, '0');
        CHECK(f.call("scene.cell.export", wrong).contains("error"));
        wrong = good;
        wrong.erase("expected_digest");
        CHECK(f.call("scene.cell.export", wrong).contains("error"));
        CHECK(f.call("text.read", {{"path", "World.sharkcell"}}).contains("error"));
        CHECK_FALSE(fs::exists(f.root / "World.sharkcell"));
        f.unchanged();
    }
    SECTION("camera content remains explicitly unsupported") {
        Fixture f(false, true);
        CHECK(f.call("scene.cell.export", f.request("World.sharkcell", false)).contains("error"));
        CHECK_FALSE(fs::exists(f.root / "World.sharkcell"));
        f.unchanged();
    }
    SECTION("a known prepublication error preserves source and a retry remains create-only") {
        Fixture f;
        const auto request = f.request();
        {
            Fault fault{detail::FileFault::publish};
            CHECK(f.call("scene.cell.export", request).contains("error"));
        }
        CHECK_FALSE(fs::exists(f.root / "World.sharkcell"));
        const auto receipt = f.result("scene.cell.export", request);
        CHECK(receipt.at("publication").at("published").get<bool>());
        f.unchanged();
    }
    SECTION("postpublication durability warnings remain successful publications") {
        Fixture f;
        const auto request = f.request();
        Json receipt;
        {
            Fault fault{detail::FileFault::directory_flush};
            receipt = f.result("scene.cell.export", request);
        }
        CHECK(receipt.at("publication").at("published").get<bool>());
        CHECK(receipt.at("publication").at("durability").get<std::string>() ==
              "directory_flush_failed");
        CHECK(f.result("cell.inspect", {{"path", "World.sharkcell"}})
                  .at("payload_digest")
                  .get<std::string>() == receipt.at("payload_digest").get<std::string>());
        f.unchanged();
    }
    SECTION("inspection rejects corrupted binary without treating it as source text") {
        Fixture f;
        f.result("scene.cell.export", f.request());
        auto bytes = read(f.root / "World.sharkcell");
        bytes.back() ^= 1;
        write(f.root / "World.sharkcell", bytes);
        CHECK(f.call("cell.inspect", {{"path", "World.sharkcell"}}).contains("error"));
        CHECK(f.call("text.read", {{"path", "World.sharkcell"}}).contains("error"));
        f.unchanged();
    }
}

TEST_CASE("cell authoring validates actual imported target closures and rejects stale dependencies",
          "[world-cell-authoring]") {
    Fixture f(true);
    const auto request = f.request();
    const auto exported = f.result("scene.cell.export", request);
    CHECK(exported.at("vertices").get<std::size_t>() == 3);
    CHECK(exported.at("collision_triangles").get<std::size_t>() == 1);
    auto client = request;
    client["path"] = "Client.sharkcell";
    client["target"] = {{"id", "local-client"}, {"side", "client"}};
    const auto forbidden = f.call("scene.cell.export", client);
    REQUIRE(forbidden.contains("error"));
    CHECK(forbidden.at("error").at("data").at("code").get<std::string>() ==
          "invalid_configuration");
    write(f.root / "triangle.bin", f.buffer + "changed");
    auto stale = request;
    stale["path"] = "Stale.sharkcell";
    CHECK(f.call("scene.cell.export", stale).contains("error"));
    CHECK_FALSE(fs::exists(f.root / "Stale.sharkcell"));
    CHECK_FALSE(fs::exists(f.root / "Client.sharkcell"));
    f.unchanged();
}

TEST_CASE("human cell adapter and terminal owner publish identical content without scene edits",
          "[world-cell-authoring]") {
    Fixture f;
    const auto terminal = f.result("scene.cell.export", f.request("Terminal.sharkcell"));
    f.owner.reset();
    const auto session =
        take(shark::editor::ProjectSession::create(f.root, "Project.sharkproject"));
    shark::editor::WorldCellPanel panel{*session};
    const auto before = session->state();
    const shark::editor::WorldCellExportOptions options{{id<WorldId>('2'), {1'000'000'000, 0, 0}},
                                                        17,
                                                        {"local-server", AssetAudience::server},
                                                        {id<AuthoredEntityId>('4')},
                                                        {id<AuthoredEntityId>('4')},
                                                        "Human.sharkcell"};
    const auto receipt = take(panel.export_cell(options, before.revision));
    CHECK(receipt.published);
    CHECK(receipt.durability);
    CHECK(receipt.digest == terminal.at("payload_digest").get<std::string>());
    CHECK(read(f.root / "Human.sharkcell") == read(f.root / "Terminal.sharkcell"));
    const auto observation = take(panel.inspect("Human.sharkcell"));
    CHECK_FALSE(observation.published);
    CHECK_FALSE(observation.durability);
    REQUIRE(observation.matches_prepared);
    CHECK(*observation.matches_prepared);
    CHECK(observation.digest == receipt.digest);
    CHECK_FALSE(panel.export_cell(options, before.revision));
    REQUIRE(panel.prepared());
    CHECK(panel.prepared()->digest == receipt.digest);
    CHECK(session->state() == before);
    CHECK(session->state_current());
    f.unchanged();
    bool wrong_thread_rejected{};
    std::thread other([&] {
        const auto value = panel.inspect("Human.sharkcell");
        wrong_thread_rejected = !value && value.error().code == ErrorCode::conflict;
    });
    other.join();
    CHECK(wrong_thread_rejected);
}
