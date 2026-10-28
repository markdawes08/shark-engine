// Original Shark-authored offline source and geometry fixtures; project MIT license.
#include "shark/tools/client_export.hpp"
#include "shark/tools/gltf_asset.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <thread>

namespace {
using namespace shark;
using namespace shark::tools;
namespace fs = std::filesystem;

template <typename T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().message);
        INFO(result.error().context.operation);
        INFO(result.error().context.path);
    }
    REQUIRE(result);
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result)
        INFO(result.error().message);
    REQUIRE(result);
}
template <typename T> T id(unsigned number) {
    std::ostringstream text;
    text << "14a00000-0000-4000-8000-" << std::hex << std::setw(12) << std::setfill('0') << number;
    return T{take(StableId::parse(text.str()))};
}
ProjectPath path(std::string_view text) {
    return take(ProjectPath::parse(text));
}
void write(const fs::path& target, std::string_view data) {
    std::ofstream file{target, std::ios::binary | std::ios::trunc};
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    file.close();
    REQUIRE(file.good());
}
std::string read(const fs::path& target) {
    std::ifstream file{target, std::ios::binary};
    REQUIRE(file.good());
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}
std::string triangle(float width = 1) {
    std::string result;
    for (const float value : std::array<float, 9>{0, 0, 0, width, 0, 0, 0, 1, 0}) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned shift = 0; shift < 32; shift += 8)
            result += static_cast<char>((bits >> shift) & 255U);
    }
    return result;
}
constexpr std::string_view model =
    R"({"asset":{"version":"2.0"},"buffers":[{"uri":"triangle.bin","byteLength":36}],"bufferViews":[{"buffer":0,"byteLength":36}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],"materials":[{"pbrMetallicRoughness":{"baseColorFactor":[0.25,0.5,0.75,1]},"doubleSided":true}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";

class CountedGltf final : public AssetImporter {
  public:
    GltfAssetImporter importer;
    unsigned imports{0};
    mutable unsigned normalizations{0};
    const AssetImporterInfo& info() const noexcept override {
        return importer.info();
    }
    Result<std::string> normalize_settings(std::string_view text) const override {
        ++normalizations;
        return importer.normalize_settings(text);
    }
    Result<std::vector<std::byte>> import(const AssetImportInput& input) override {
        ++imports;
        return importer.import(input);
    }
    Result<void> validate_payload(const AssetArtifact& artifact) const override {
        return importer.validate_payload(artifact);
    }
};

std::unique_ptr<AuthoredScene> authored(
    std::vector<MeshRenderer> renderers = {{builtin_cube_asset_id()}, {builtin_plane_asset_id()}},
    unsigned project_number = 1, bool startup = true) {
    ProjectDocument project{
        id<ProjectId>(project_number),
        id<WorldId>(2),
        Name{"Export 雪"},
        id<SceneId>(3),
        {{id<SceneId>(3), path("startup.sharkscene")}, {id<SceneId>(7), path("other.sharkscene")}},
        R"({"private_project_note":"not for runtime"})"};
    Camera camera;
    camera.projection = CameraProjection::orthographic;
    camera.near_clip = 1;
    camera.far_clip = 10;
    Transform eye;
    eye.translation.z = 4;
    SceneDocument scene{id<ProjectId>(project_number),
                        id<SceneId>(startup ? 3U : 7U),
                        WorldPosition{id<WorldId>(2), {123456789, -3, 8}, {0.25, 4, 0}},
                        {{id<AuthoredEntityId>(4), {}, Name{"Camera"}, eye, "{}", camera}},
                        R"({"private_scene_note":true})"};
    for (std::size_t index = 0; index < renderers.size(); ++index)
        scene.entities.push_back({id<AuthoredEntityId>(100U + static_cast<unsigned>(index)),
                                  {},
                                  Name{"Mesh"},
                                  {},
                                  R"({"private_entity_note":true})",
                                  {},
                                  renderers[index]});
    return take(AuthoredScene::create(project, scene));
}
void edit(AuthoredScene& owner, const AuthoringCommand& command) {
    auto reply = take(owner.execute(command, take(owner.status()).revision));
    take(std::move(reply.outcome));
}

struct Fixture {
    fs::path root = fs::temp_directory_path() /
                    ("shark-client-export-" + take(generate_stable_id()).to_string());
    CountedGltf gltf;
    GltfResourceImporter resource;
    ByteAssetImporter bytes;
    std::unique_ptr<ProjectFiles> files;
    std::unique_ptr<AssetCatalog> catalog;
    std::unique_ptr<AssetImports> imports;
    std::vector<ProjectPath> inventory;
    const AssetTargetProfile target{"windows-x64-export-test", AssetAudience::client};

    Fixture() {
        REQUIRE(fs::create_directory(root));
        files = take(ProjectFiles::open(root));
        catalog = take(AssetCatalog::create(*files, id<ProjectId>(1)));
        const std::array<AssetImporter*, 3> registry{&gltf, &resource, &bytes};
        imports = take(AssetImports::open(*files, *catalog, id<ProjectId>(1), registry));
    }
    ~Fixture() {
        imports.reset();
        catalog.reset();
        files.reset();
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    ProjectPath source(unsigned number) const {
        return path("source" + std::to_string(number));
    }
    ProjectPath sidecar(unsigned number) const {
        return take(asset_sidecar_path(source(number)));
    }
    void seed(unsigned number, std::string_view content, std::string kind, std::string importer,
              std::string settings = "{}", std::vector<AssetId> dependencies = {},
              AssetAudience audience = AssetAudience::shared) {
        write(root / source(number).text(), content);
        AssetMetadata metadata{id<ProjectId>(1),
                               id<AssetId>(number),
                               std::move(kind),
                               "Original Shark source " + std::to_string(number),
                               "MIT",
                               std::move(importer),
                               audience,
                               std::move(settings),
                               std::move(dependencies),
                               R"({"private_metadata_note":true})"};
        take(files->write_asset_metadata(sidecar(number), metadata, {}));
        inventory.push_back(sidecar(number));
    }
    void model_asset(unsigned number = 20, AssetAudience audience = AssetAudience::shared) {
        const auto settings =
            R"({"path":"models/model.gltf","resources":[{"path":"models/triangle.bin","asset":")" +
            id<AssetId>(10).value().to_string() + R"("}]})";
        seed(number, model, "gltf", "shark.gltf", settings, {id<AssetId>(10)}, audience);
    }
    void external() {
        seed(10, triangle(), "gltf-resource", "shark.gltf-resource");
        model_asset();
        refresh();
    }
    AssetCatalogRevision refresh() {
        return take(catalog->refresh(inventory, take(catalog->revision())));
    }
    AssetCacheRevision revision() const {
        return take(imports->revision());
    }
    AssetCacheKey publish(unsigned number = 20, const AssetTargetProfile* profile = nullptr) {
        const auto& selected = profile ? *profile : target;
        auto prepared = take(
            imports->prepare(id<AssetId>(number), selected, take(catalog->revision()), revision()));
        const auto summary = take(prepared.summary());
        take(imports->publish(std::move(prepared), take(catalog->revision()), revision()));
        return summary.root_key;
    }
    void metadata(unsigned number, AssetMetadata value) {
        auto prior = take(files->read_asset_metadata(sidecar(number)));
        take(files->write_asset_metadata(sidecar(number), value, prior.revision));
    }
    AssetMetadata metadata(unsigned number) {
        return take(files->read_asset_metadata(sidecar(number))).source.document;
    }
    ClientExportRequest request(const AuthoredScene& owner) const {
        return {id<AuthoredEntityId>(4), take(owner.status()).revision, take(catalog->revision()),
                revision(), target};
    }
    PreparedClientExport prepare(const AuthoredScene& owner) {
        return take(prepare_client_export(owner, *catalog, *imports, request(owner)));
    }
};

std::unique_ptr<LoadedPreparedScene> load(const PreparedClientExport& candidate) {
    std::vector<PreparedAssetInput> inputs;
    for (const auto& mesh : candidate.meshes())
        inputs.push_back({mesh.asset, mesh.payload});
    return take(
        LoadedPreparedScene::load(candidate.scene_payload(), take(PreparedAssets::create(inputs))));
}
bool contains(std::span<const std::byte> payload, std::string_view text) {
    const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
    return std::search(payload.begin(), payload.end(), bytes.begin(), bytes.end()) != payload.end();
}
} // namespace

TEST_CASE("client export captures builtin startup content without saving authoring state",
          "[client-export]") {
    Fixture fixture;
    auto owner = authored();
    edit(*owner, SetNameCommand{id<AuthoredEntityId>(100), Name{"Unsaved cube"}});
    const auto before = take(owner->capture());
    const auto history = take(owner->history());
    const auto cache = fixture.revision();
    auto candidate = fixture.prepare(*owner);
    CHECK(candidate.project() == id<ProjectId>(1));
    CHECK(candidate.world() == id<WorldId>(2));
    CHECK(candidate.scene() == id<SceneId>(3));
    CHECK(candidate.name().value == "Export 雪");
    CHECK(candidate.request().camera == id<AuthoredEntityId>(4));
    CHECK(candidate.dirty());
    CHECK(candidate.meshes().empty());
    CHECK(candidate.sources().empty());
    CHECK(candidate.payload_bytes() == candidate.scene_payload().size());
    CHECK(candidate.scene_digest() == take(sha256(candidate.scene_payload())));
    CHECK_FALSE(contains(candidate.scene_payload(), "private_"));
    auto loaded = load(candidate);
    CHECK(loaded->assets()->size() == 2);
    CHECK(loaded->world_origin() == before.scene.world_origin);
    const auto cube = take(loaded->scene().find(id<AuthoredEntityId>(100)));
    CHECK(take(loaded->scene().entity_state(cube)).name.value == "Unsaved cube");
    const auto after = take(owner->capture());
    CHECK(after.scene == before.scene);
    CHECK(after.revision == before.revision);
    CHECK(after.dirty == before.dirty);
    const auto retained = take(owner->history());
    CHECK(retained.undo_entries == history.undo_entries);
    CHECK(retained.retained_bytes == history.retained_bytes);
    REQUIRE(retained.next_undo);
    REQUIRE(history.next_undo);
    CHECK(retained.next_undo->ticket == history.next_undo->ticket);
    CHECK(fixture.revision() == cache);
    CHECK_FALSE(fs::exists(fixture.root / std::string{asset_cache_filename}));
    take(validate_client_export(candidate, *owner, *fixture.catalog, *fixture.imports));
}

TEST_CASE("client export requires startup camera and exact project context even for builtins",
          "[client-export]") {
    Fixture fixture;
    auto owner = authored();
    SECTION("explicit camera must exist and carry Camera") {
        auto request = fixture.request(*owner);
        request.camera = id<AuthoredEntityId>(999);
        CHECK_FALSE(prepare_client_export(*owner, *fixture.catalog, *fixture.imports, request));
        request.camera = id<AuthoredEntityId>(100);
        CHECK_FALSE(prepare_client_export(*owner, *fixture.catalog, *fixture.imports, request));
    }
    SECTION("active nonstartup scene is not silently substituted") {
        owner = authored({}, 1, false);
        CHECK_FALSE(prepare_client_export(*owner, *fixture.catalog, *fixture.imports,
                                          fixture.request(*owner)));
    }
    SECTION("wrong project cannot borrow this cache context") {
        owner = authored({}, 90);
        CHECK_FALSE(prepare_client_export(*owner, *fixture.catalog, *fixture.imports,
                                          fixture.request(*owner)));
    }
    SECTION("scope and revision fences apply without mesh queries") {
        Fixture foreign;
        auto request = fixture.request(*owner);
        request.scene_revision.revision += 1;
        CHECK_FALSE(prepare_client_export(*owner, *fixture.catalog, *fixture.imports, request));
        request = fixture.request(*owner);
        request.catalog_revision = take(foreign.catalog->revision());
        CHECK_FALSE(prepare_client_export(*owner, *fixture.catalog, *fixture.imports, request));
        request = fixture.request(*owner);
        request.cache_revision = foreign.revision();
        CHECK_FALSE(prepare_client_export(*owner, *fixture.catalog, *fixture.imports, request));
        request = fixture.request(*owner);
        request.cache_revision.value += 1;
        CHECK_FALSE(prepare_client_export(*owner, *fixture.catalog, *fixture.imports, request));
    }
    SECTION("only valid declared client targets are admitted") {
        auto request = fixture.request(*owner);
        request.target.side = AssetAudience::server;
        CHECK_FALSE(prepare_client_export(*owner, *fixture.catalog, *fixture.imports, request));
        request.target.side = AssetAudience::shared;
        CHECK_FALSE(prepare_client_export(*owner, *fixture.catalog, *fixture.imports, request));
        request.target = {"INVALID TARGET", AssetAudience::client};
        CHECK_FALSE(prepare_client_export(*owner, *fixture.catalog, *fixture.imports, request));
    }
}

TEST_CASE("client export deduplicates meshes and retains only used licensed import closures",
          "[client-export]") {
    Fixture fixture;
    fixture.external();
    fixture.model_asset(30);
    fixture.seed(99, "PRIVATE_SERVER_SECRET", "bytes", "shark.bytes", "{}", {},
                 AssetAudience::server);
    fixture.refresh();
    const auto key20 = fixture.publish();
    const auto key30 = fixture.publish(30);
    auto owner = authored({{id<AssetId>(30)},
                           {id<AssetId>(20)},
                           {id<AssetId>(30), 0, false},
                           {builtin_plane_asset_id()}});
    const auto imports_before = fixture.gltf.imports;
    const auto cache_bytes = read(fixture.root / std::string{asset_cache_filename});
    auto candidate = fixture.prepare(*owner);
    REQUIRE(candidate.meshes().size() == 2);
    CHECK(candidate.meshes()[0].asset == id<AssetId>(20));
    CHECK(candidate.meshes()[1].asset == id<AssetId>(30));
    CHECK(candidate.meshes()[0].source_key == key20);
    CHECK(candidate.meshes()[1].source_key == key30);
    REQUIRE(candidate.sources().size() == 3);
    CHECK(candidate.sources()[0].asset == id<AssetId>(10));
    CHECK(candidate.sources()[1].asset == id<AssetId>(20));
    CHECK(candidate.sources()[2].asset == id<AssetId>(30));
    CHECK(candidate.sources()[1].key == key20);
    CHECK(candidate.sources()[2].key == key30);
    for (const auto& source : candidate.sources()) {
        CHECK(source.license == "MIT");
        CHECK(source.origin.starts_with("Original Shark source "));
    }
    std::size_t total = candidate.scene_payload().size();
    for (const auto& mesh : candidate.meshes()) {
        total += mesh.payload.size();
        CHECK(mesh.digest == take(sha256(mesh.payload)));
        auto decoded = take(decode_prepared_mesh(mesh.payload));
        REQUIRE(decoded.meshes.size() == 1);
        REQUIRE(decoded.meshes[0].primitives.size() == 1);
        const auto& primitive = decoded.meshes[0].primitives[0];
        CHECK(primitive.vertices[1].position == std::array<float, 3>{1, 0, 0});
        CHECK(primitive.indices == std::vector<std::uint32_t>{0, 1, 2});
        CHECK(primitive.material.base_color == std::array<float, 4>{0.25F, 0.5F, 0.75F, 1});
        CHECK(primitive.material.double_sided);
        CHECK_FALSE(contains(mesh.payload, "private_"));
        CHECK_FALSE(contains(mesh.payload, "PRIVATE_SERVER_SECRET"));
    }
    CHECK(candidate.payload_bytes() == total);
    CHECK(load(candidate)->assets()->size() == 4);
    auto repeated = fixture.prepare(*owner);
    CHECK(std::ranges::equal(candidate.scene_payload(), repeated.scene_payload()));
    for (std::size_t index = 0; index < candidate.meshes().size(); ++index)
        CHECK(candidate.meshes()[index].payload == repeated.meshes()[index].payload);
    CHECK(fixture.gltf.imports == imports_before);
    CHECK(read(fixture.root / std::string{asset_cache_filename}) == cache_bytes);
}

TEST_CASE("client export never imports missing stale blocked or unsupported mesh inputs",
          "[client-export]") {
    Fixture fixture;
    auto owner = authored({{id<AssetId>(20)}});
    SECTION("uncached source requires explicit import") {
        fixture.external();
        CHECK_FALSE(prepare_client_export(*owner, *fixture.catalog, *fixture.imports,
                                          fixture.request(*owner)));
        CHECK(fixture.gltf.imports == 0);
        CHECK(take(fixture.imports->usage(fixture.revision())).artifacts == 0);
    }
    SECTION("changed dependency rejects retained last-valid bytes") {
        fixture.external();
        fixture.publish();
        const auto calls = fixture.gltf.imports;
        write(fixture.root / fixture.source(10).text(), triangle(2));
        CHECK_FALSE(prepare_client_export(*owner, *fixture.catalog, *fixture.imports,
                                          fixture.request(*owner)));
        CHECK(fixture.gltf.imports == calls);
    }
    SECTION("missing source blocks export without erasing cache") {
        fixture.external();
        fixture.publish();
        const auto usage = take(fixture.imports->usage(fixture.revision()));
        REQUIRE(fs::remove(fixture.root / fixture.source(10).text()));
        CHECK_FALSE(prepare_client_export(*owner, *fixture.catalog, *fixture.imports,
                                          fixture.request(*owner)));
        CHECK(take(fixture.imports->usage(fixture.revision())).artifacts == usage.artifacts);
    }
    SECTION("arbitrary byte importer is not accepted as a prepared glTF mesh") {
        fixture.seed(20, "not a mesh", "bytes", "shark.bytes");
        fixture.refresh();
        fixture.publish();
        CHECK_FALSE(prepare_client_export(*owner, *fixture.catalog, *fixture.imports,
                                          fixture.request(*owner)));
    }
    SECTION("corrupted cache file cannot be bypassed with retained memory") {
        fixture.external();
        fixture.publish();
        write(fixture.root / std::string{asset_cache_filename}, "corrupt");
        CHECK_FALSE(prepare_client_export(*owner, *fixture.catalog, *fixture.imports,
                                          fixture.request(*owner)));
    }
}

TEST_CASE("client export resolves hidden references mesh indices and private dependencies",
          "[client-export]") {
    Fixture fixture;
    fixture.external();
    fixture.publish();
    SECTION("hidden missing references still reject") {
        auto owner = authored({{id<AssetId>(999), 0, false}});
        CHECK_FALSE(prepare_client_export(*owner, *fixture.catalog, *fixture.imports,
                                          fixture.request(*owner)));
    }
    SECTION("dense mesh index is checked for both builtin and imported assets") {
        for (const auto asset : {builtin_cube_asset_id(), id<AssetId>(20)}) {
            auto owner = authored({{asset, 1, false}});
            CHECK_FALSE(prepare_client_export(*owner, *fixture.catalog, *fixture.imports,
                                              fixture.request(*owner)));
        }
    }
    SECTION("server-private dependency cannot escape through an old client root") {
        auto owner = authored({{id<AssetId>(20)}});
        auto metadata = fixture.metadata(10);
        metadata.audience = AssetAudience::server;
        fixture.metadata(10, metadata);
        fixture.refresh();
        CHECK_FALSE(prepare_client_export(*owner, *fixture.catalog, *fixture.imports,
                                          fixture.request(*owner)));
    }
    SECTION("server root cannot be exported through a matching server cache") {
        auto owner = authored({{id<AssetId>(20)}});
        auto metadata = fixture.metadata(20);
        metadata.audience = AssetAudience::server;
        fixture.metadata(20, metadata);
        fixture.refresh();
        const AssetTargetProfile server{"windows-x64-export-test", AssetAudience::server};
        fixture.publish(20, &server);
        CHECK_FALSE(prepare_client_export(*owner, *fixture.catalog, *fixture.imports,
                                          fixture.request(*owner)));
    }
}

TEST_CASE("client export revalidation fences changes without modifying owned candidate bytes",
          "[client-export]") {
    Fixture fixture;
    fixture.external();
    fixture.publish();
    auto owner = authored({{id<AssetId>(20)}});
    auto candidate = fixture.prepare(*owner);
    const auto digest = candidate.scene_digest();
    const auto payload = candidate.meshes()[0].payload;
    SECTION("scene edit") {
        edit(*owner, SetNameCommand{id<AuthoredEntityId>(100), Name{"Changed"}});
    }
    SECTION("source edit with unchanged catalog revision") {
        write(fixture.root / fixture.source(10).text(), triangle(2));
    }
    SECTION("metadata edit before catalog refresh") {
        auto metadata = fixture.metadata(20);
        metadata.license = "MIT; attribution changed";
        fixture.metadata(20, metadata);
    }
    SECTION("catalog revision") {
        fixture.seed(90, "unrelated", "bytes", "shark.bytes");
        fixture.refresh();
    }
    SECTION("cache publication") {
        take(fixture.imports->clear(fixture.revision()));
    }
    CHECK_FALSE(validate_client_export(candidate, *owner, *fixture.catalog, *fixture.imports));
    CHECK(candidate.scene_digest() == digest);
    CHECK(candidate.meshes()[0].payload == payload);
    CHECK(load(candidate)->assets()->size() == 3);
}

TEST_CASE("client export bounds mesh roots before importer work and enforces owner threads",
          "[client-export]") {
    Fixture fixture;
    fixture.seed(10, triangle(), "gltf-resource", "shark.gltf-resource");
    fixture.model_asset(100);
    fixture.refresh();
    fixture.publish(100);
    std::vector<MeshRenderer> renderers;
    for (std::size_t index = 0; index <= client_export_maximum_meshes; ++index)
        renderers.push_back({id<AssetId>(100U + static_cast<unsigned>(index)), 0, false});
    auto owner = authored(renderers);
    const auto prior = fixture.gltf.normalizations;
    CHECK_FALSE(
        prepare_client_export(*owner, *fixture.catalog, *fixture.imports, fixture.request(*owner)));
    CHECK(fixture.gltf.normalizations == prior);
    owner = authored();
    auto candidate = fixture.prepare(*owner);
    const auto request = fixture.request(*owner);
    bool prepare_failed = false;
    bool validate_failed = false;
    std::thread other([&] {
        prepare_failed =
            !prepare_client_export(*owner, *fixture.catalog, *fixture.imports, request);
        validate_failed =
            !validate_client_export(candidate, *owner, *fixture.catalog, *fixture.imports);
    });
    other.join();
    CHECK(prepare_failed);
    CHECK(validate_failed);
    take(validate_client_export(candidate, *owner, *fixture.catalog, *fixture.imports));
    auto moved = std::move(candidate);
    CHECK_FALSE(validate_client_export(candidate, *owner, *fixture.catalog, *fixture.imports));
    take(validate_client_export(moved, *owner, *fixture.catalog, *fixture.imports));
}

TEST_CASE("client export payloads remain usable after project scene and cache owners disappear",
          "[client-export]") {
    auto candidate = [] {
        Fixture fixture;
        fixture.external();
        fixture.publish();
        auto owner = authored({{id<AssetId>(20)}});
        return fixture.prepare(*owner);
    }();
    auto loaded = load(candidate);
    REQUIRE(candidate.meshes().size() == 1);
    REQUIRE(candidate.sources().size() == 2);
    CHECK(take(loaded->scene().stats()).live_entities == 2);
    const auto entity = take(loaded->scene().find(id<AuthoredEntityId>(100)));
    const auto mesh =
        take(loaded->assets()->resolve(*take(loaded->scene().entity_state(entity)).mesh_renderer));
    REQUIRE(mesh->meshes.size() == 1);
    CHECK(mesh->meshes[0].primitives[0].vertices[2].position == std::array<float, 3>{0, 1, 0});
}

TEST_CASE("client export precharges repeated shared dependency visits before source recapture",
          "[client-export]") {
    Fixture fixture;
    std::vector<AssetId> dependencies;
    for (unsigned index = 0; index < 64; ++index) {
        const auto number = 10U + index;
        fixture.seed(number, "", "gltf-resource", "shark.gltf-resource");
        dependencies.push_back(id<AssetId>(number));
    }
    std::vector<MeshRenderer> renderers;
    for (unsigned index = 0; index < 8; ++index) {
        const auto number = 2000U + index;
        fixture.model_asset(number);
        auto metadata = fixture.metadata(number);
        metadata.dependencies = dependencies;
        fixture.metadata(number, metadata);
        renderers.push_back({id<AssetId>(number)});
    }
    fixture.refresh();
    // 72 unique sources fit the union bound, while each of eight roots needs
    // its own 65-node freshness capture: 520 visits exceed the 512 work bound.
    auto owner = authored(renderers);
    const auto rejected =
        prepare_client_export(*owner, *fixture.catalog, *fixture.imports, fixture.request(*owner));
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == ErrorCode::invalid_configuration);
    CHECK(rejected.error().message.find("dependency visits") != std::string::npos);
    CHECK(rejected.error().message.find(std::to_string(client_export_maximum_dependency_visits)) !=
          std::string::npos);
    CHECK(fixture.gltf.normalizations == 0);
    CHECK(fixture.gltf.imports == 0);
    CHECK(take(fixture.imports->usage(fixture.revision())).artifacts == 0);
    CHECK_FALSE(fs::exists(fixture.root / std::string{asset_cache_filename}));
}
