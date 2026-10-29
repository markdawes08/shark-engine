#include <shark/tools/authoring_protocol.hpp>
#include <shark/tools/gltf_asset.hpp>
#include <shark/tools/project_files.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

namespace {
using namespace shark;
using namespace shark::tools;
using Json = nlohmann::json;
namespace fs = std::filesystem;

constexpr auto project_text = "16b00000-0000-4000-8000-000000000001";
constexpr auto world_text = "16b00000-0000-4000-8000-000000000002";
constexpr auto scene_text = "16b00000-0000-4000-8000-000000000003";
constexpr auto camera_text = "16b00000-0000-4000-8000-000000000004";
constexpr auto mesh_text = "16b00000-0000-4000-8000-000000000005";
constexpr auto asset_text = "16b00000-0000-4000-8000-000000000006";

template <typename T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().message);
        INFO(result.error().context.operation);
    }
    REQUIRE(result);
    return std::move(result).value();
}
template <typename T> T id(std::string_view text) {
    return T{take(StableId::parse(text))};
}
template <typename T> void rejected(const Result<T>& result, ErrorCode code) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == code);
    CHECK_FALSE(result.error().message.empty());
    CHECK_FALSE(result.error().context.operation.empty());
}
std::string read(const fs::path& path) {
    std::ifstream stream{path, std::ios::binary};
    REQUIRE(stream);
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}
void write(const fs::path& path, std::string_view bytes) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    REQUIRE(stream);
}
Json call(AuthoringProtocol& owner, std::string_view method, Json parameters = Json::object()) {
    const auto reply = take(owner.handle(Json{
        {"jsonrpc", "2.0"},
        {"protocol_version", 1},
        {"id", "viewport-fixture"},
        {"method", method},
        {"params", std::move(parameters)}}.dump()));
    INFO(reply.json);
    REQUIRE_FALSE(reply.failed);
    return Json::parse(reply.json).at("result");
}
SceneRevision revision(AuthoringProtocol& owner) {
    const auto value = call(owner, "project.status").at("revision");
    return {std::stoull(value.at("scene_instance").get<std::string>()),
            std::stoull(value.at("revision").get<std::string>())};
}
void move_mesh(AuthoringProtocol& owner) {
    const auto status = call(owner, "scene.mutation_status");
    const auto changed = call(owner, "scene.mutate",
                              {{"mutation_id", status.at("next")},
                               {"expected_revision", status.at("revision")},
                               {"operation",
                                {{"type", "command"},
                                 {"command",
                                  {{"type", "set_transform"},
                                   {"entity_id", mesh_text},
                                   {"transform",
                                    {{"translation", {1, 0, -4}},
                                     {"rotation", {0, 0, 0, 1}},
                                     {"scale", {1, 1, 1}}}}}}}}});
    REQUIRE(changed.at("outcome").at("ok").get<bool>());
    REQUIRE(changed.at("outcome").at("value").at("result").at("ok").get<bool>());
}

class Fixture final {
  public:
    Fixture(bool imported = false) {
        root = fs::temp_directory_path() /
               ("shark-authoring-viewport-" + take(generate_stable_id()).to_string());
        REQUIRE(fs::create_directory(root));
        auto files = take(ProjectFiles::open(root));
        ProjectDocument project{
            id<ProjectId>(project_text),
            id<WorldId>(world_text),
            {"Viewport project"},
            id<SceneId>(scene_text),
            {{id<SceneId>(scene_text), take(ProjectPath::parse("scene.sharkscene"))}}};
        Camera camera;
        camera.projection = CameraProjection::orthographic;
        camera.orthographic_height = 8;
        camera.near_clip = 1;
        camera.far_clip = 10;
        Transform model;
        model.translation.z = -4;
        SceneDocument scene{
            project.project,
            project.startup_scene,
            WorldPosition{project.world, {123456789, -3, 8}, {0.25, 4, 0}},
            {{id<AuthoredEntityId>(camera_text), {}, {"Camera"}, {}, "{}", camera},
             {id<AuthoredEntityId>(mesh_text),
              {},
              {"Mesh"},
              model,
              "{}",
              {},
              MeshRenderer{imported ? id<AssetId>(asset_text) : builtin_cube_asset_id()}}}};
        static_cast<void>(take(files->write_scene(project, scene, {})));
        static_cast<void>(take(
            files->write_project(take(ProjectPath::parse("project.sharkproject")), project, {})));
        if (imported) {
            write(root / "mesh.gltf", read("tests/fixtures/gltf_geometry/embedded-strip.gltf"));
            const auto source = take(ProjectPath::parse("mesh.gltf"));
            const auto sidecar = take(asset_sidecar_path(source));
            sidecar_name = sidecar.text();
            const AssetMetadata metadata{project.project,
                                         id<AssetId>(asset_text),
                                         "gltf",
                                         "Original Shark geometry fixture",
                                         "MIT",
                                         "shark.gltf",
                                         AssetAudience::shared,
                                         R"({"path":"mesh.gltf","resources":[]})",
                                         {},
                                         "{}"};
            static_cast<void>(take(files->write_asset_metadata(sidecar, metadata, {})));
            auto catalog = take(AssetCatalog::create(*files, project.project));
            const auto catalog_revision =
                take(catalog->refresh(std::array{sidecar}, take(catalog->revision())));
            GltfAssetImporter importer;
            const std::array<AssetImporter*, 1> registry{&importer};
            auto imports = take(AssetImports::open(*files, *catalog, project.project, registry));
            auto candidate = take(imports->prepare(id<AssetId>(asset_text), target,
                                                   catalog_revision, take(imports->revision())));
            static_cast<void>(take(imports->publish(std::move(candidate), catalog_revision,
                                                    take(imports->revision()))));
        }
    }
    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    std::unique_ptr<AuthoringProtocol> open() const {
        auto owner = take(AuthoringProtocol::create(root));
        call(*owner, "project.open", {{"path", "project.sharkproject"}});
        return owner;
    }
    AuthoringViewportRequest request(AuthoringProtocol& owner) const {
        return {id<AuthoredEntityId>(camera_text), revision(owner), {160, 120}, {}, 1, target};
    }
    void inventory(AuthoringProtocol& owner) const {
        const auto status = call(owner, "asset.catalog.status");
        call(owner, "asset.catalog.refresh",
             {{"sidecars", {sidecar_name}}, {"expected_revision", status.at("revision")}});
    }
    fs::path root;
    std::string sidecar_name;
    AssetTargetProfile target{"viewport-test", AssetAudience::client};
};
} // namespace

TEST_CASE("Retained viewport captures unsaved source identities and immutable geometry",
          "[authoring-viewport]") {
    Fixture fixture;
    auto owner = fixture.open();
    auto request = fixture.request(*owner);
    request.local_origin.x = 0.5;
    request.origin_generation = 7;
    auto before = take(owner->viewport_snapshot(request));
    REQUIRE(before.records().size() == 1);
    CHECK(before.revision() == request.expected_revision);
    CHECK(before.camera_entity().handle.scene_instance == request.expected_revision.scene_instance);
    CHECK(std::get<AuthoredEntityId>(before.camera_entity().identity) == request.camera);
    CHECK(before.records()[0].entity.handle.scene_instance ==
          request.expected_revision.scene_instance);
    CHECK(std::get<AuthoredEntityId>(before.records()[0].entity.identity) ==
          id<AuthoredEntityId>(mesh_text));
    CHECK(before.records()[0].world_matrix[3].x == -0.5);
    CHECK(before.records()[0].world_matrix[3].z == -4);
    CHECK(before.origin().generation == 7);
    REQUIRE(before.origin().world_anchor);
    CHECK(before.origin().world_anchor->cell.x == 123456789);
    CHECK(before.origin().world_anchor->offset[0] == 0.25);
    const auto saved = read(fixture.root / "scene.sharkscene");
    move_mesh(*owner);
    const auto state = call(*owner, "project.status");
    const auto history = call(*owner, "scene.history");
    const auto mutations = call(*owner, "scene.mutation_status");
    REQUIRE(state.at("dirty").get<bool>());
    rejected(owner->viewport_snapshot(request), ErrorCode::conflict);
    request.expected_revision = revision(*owner);
    const auto changed = take(owner->viewport_snapshot(request));
    REQUIRE(changed.records().size() == 1);
    CHECK(changed.records()[0].world_matrix[3].x == 0.5);
    CHECK(changed.records()[0].entity == before.records()[0].entity);
    CHECK(before.records()[0].world_matrix[3].x == -0.5);
    CHECK(call(*owner, "project.status") == state);
    CHECK(call(*owner, "scene.history") == history);
    CHECK(call(*owner, "scene.mutation_status") == mutations);
    CHECK(read(fixture.root / "scene.sharkscene") == saved);
    CHECK_FALSE(fs::exists(fixture.root / std::string{asset_cache_filename}));
    call(*owner, "project.close",
         {{"unsaved", "discard"}, {"expected_revision", state.at("revision")}});
    rejected(owner->viewport_snapshot(request), ErrorCode::conflict);
    owner.reset();
    CHECK(before.records()[0].asset->asset == builtin_cube_asset_id());
    CHECK_FALSE(before.records()[0].asset->meshes.empty());
    auto reopened = fixture.open();
    CHECK(fixture.request(*reopened).expected_revision.scene_instance !=
          request.expected_revision.scene_instance);
    rejected(reopened->viewport_snapshot(request), ErrorCode::conflict);
}

TEST_CASE("Retained viewport rejects invalid camera bounds profile and foreign thread",
          "[authoring-viewport]") {
    Fixture fixture;
    auto owner = fixture.open();
    auto request = fixture.request(*owner);
    SECTION("entity without Camera") {
        request.camera = id<AuthoredEntityId>(mesh_text);
        rejected(owner->viewport_snapshot(request), ErrorCode::invalid_id);
    }
    SECTION("unknown camera") {
        request.camera = id<AuthoredEntityId>(asset_text);
        rejected(owner->viewport_snapshot(request), ErrorCode::invalid_id);
    }
    SECTION("zero viewport") {
        request.viewport.width = 0;
        rejected(owner->viewport_snapshot(request), ErrorCode::invalid_argument);
    }
    SECTION("oversized viewport") {
        request.viewport.height = 2049;
        rejected(owner->viewport_snapshot(request), ErrorCode::invalid_argument);
    }
    SECTION("nonfinite local origin") {
        request.local_origin.x = std::numeric_limits<double>::infinity();
        rejected(owner->viewport_snapshot(request), ErrorCode::invalid_argument);
    }
    SECTION("zero origin generation") {
        request.origin_generation = 0;
        rejected(owner->viewport_snapshot(request), ErrorCode::invalid_argument);
    }
    SECTION("server target") {
        request.target.side = AssetAudience::server;
        rejected(owner->viewport_snapshot(request), ErrorCode::invalid_configuration);
    }
    SECTION("wrong thread") {
        std::optional<Result<RenderScene>> result;
        std::thread other([&] { result.emplace(owner->viewport_snapshot(request)); });
        other.join();
        REQUIRE(result);
        rejected(*result, ErrorCode::conflict);
    }
    CHECK_FALSE(call(*owner, "project.status").at("dirty").get<bool>());
}

TEST_CASE("Retained viewport fences external source changes without replacing dirty state",
          "[authoring-viewport]") {
    Fixture fixture;
    auto owner = fixture.open();
    move_mesh(*owner);
    const auto state = call(*owner, "project.status");
    const auto history = call(*owner, "scene.history");
    const auto request = fixture.request(*owner);
    fs::path edited;
    SECTION("scene") {
        edited = fixture.root / "scene.sharkscene";
    }
    SECTION("project") {
        edited = fixture.root / "project.sharkproject";
    }
    const auto original = read(edited);
    write(edited, original + "\n");
    rejected(owner->viewport_snapshot(request), ErrorCode::conflict);
    CHECK(read(edited) == original + "\n");
    CHECK(call(*owner, "project.status") == state);
    CHECK(call(*owner, "scene.history") == history);
}

TEST_CASE("Retained viewport uses explicit fresh imported closure without importing or writing",
          "[authoring-viewport]") {
    Fixture fixture{true};
    const auto cache_file = fixture.root / std::string{asset_cache_filename};
    const auto cache_before = read(cache_file);
    auto owner = fixture.open();
    auto request = fixture.request(*owner);
    // A cache artifact never authorizes an implicit sidecar scan or catalog row.
    REQUIRE_FALSE(owner->viewport_snapshot(request));
    fixture.inventory(*owner);
    const auto snapshot = take(owner->viewport_snapshot(request));
    REQUIRE(snapshot.records().size() == 1);
    const auto& mesh = snapshot.records()[0].asset->meshes.at(0);
    REQUIRE(mesh.primitives.size() == 1);
    CHECK(mesh.primitives[0].vertices.size() == 4);
    CHECK(mesh.primitives[0].indices == std::vector<std::uint32_t>{0, 1, 2, 2, 1, 3});
    CHECK(mesh.primitives[0].vertices[3].position == std::array<float, 3>{1, 1, 0});
    CHECK(snapshot.records()[0].asset->asset == id<AssetId>(asset_text));
    CHECK(snapshot.revision() == request.expected_revision);
    auto other = request;
    other.target.id = "unimported-target";
    REQUIRE_FALSE(owner->viewport_snapshot(other));
    const auto source = read(fixture.root / "mesh.gltf");
    write(fixture.root / "mesh.gltf", source + "\n");
    REQUIRE_FALSE(owner->viewport_snapshot(request));
    CHECK(read(cache_file) == cache_before);
    CHECK_FALSE(call(*owner, "project.status").at("dirty").get<bool>());
    CHECK(call(*owner, "scene.history").at("undo_entries") == 0);
}
