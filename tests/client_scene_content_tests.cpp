#include <shark/client/scene_content.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <string>

namespace {
using namespace shark;
using namespace shark::client;

template <typename T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().message);
        INFO(result.error().context.path);
    }
    REQUIRE(result);
    return std::move(result).value();
}

template <typename Id> Id id(char digit) {
    std::string value = "10000000-0000-4000-8000-000000000000";
    value.back() = digit;
    return Id{take(StableId::parse(value))};
}

struct Files {
    std::filesystem::path root;
    SceneContentFiles content;
    Files() {
        root = std::filesystem::temp_directory_path() /
               ("shark-scene-content-" + take(generate_stable_id()).to_string());
        REQUIRE(std::filesystem::create_directory(root));
        content.scene = root / std::filesystem::path{u8"Scene 雪.sharkprepared"};
    }
    ~Files() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    Files(const Files&) = delete;
    Files& operator=(const Files&) = delete;
};

void write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.close();
    REQUIRE(stream);
}

std::string text(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}

PreparedScene scene(AssetId asset = builtin_cube_asset_id()) {
    PreparedEntity camera{id<AuthoredEntityId>('1')};
    camera.name.value = "Camera";
    camera.camera = Camera{};
    camera.transform.translation = math::Vec3{0, 0, 5};
    PreparedEntity mesh{id<AuthoredEntityId>('2')};
    mesh.name.value = "Mesh";
    mesh.mesh_renderer = MeshRenderer{asset};
    return {id<SceneId>('3'), {}, {camera, mesh}};
}

std::vector<std::byte> mesh_bytes(AssetId asset = id<AssetId>('4')) {
    const auto builtins = take(PreparedAssets::create());
    auto mesh = *take(builtins->find(builtin_cube_asset_id()));
    mesh.asset = asset;
    return take(encode_prepared_mesh(mesh));
}
} // namespace

TEST_CASE("prepared file loading owns builtins and preserves old scene across reload",
          "[scene-content]") {
    Files files;
    write(files.content.scene, take(encode_prepared_scene(scene())));
    auto loaded = take(load_scene_content(files.content));
    const auto old = take(loaded->scene().find(id<AuthoredEntityId>('2')));
    const auto revision = take(loaded->scene().stats());
    CHECK(loaded->assets()->size() == 2);
    CHECK(take(loaded->scene().entity_state(old)).mesh_renderer ==
          MeshRenderer{builtin_cube_asset_id()});
    auto replacement = take(load_scene_content(files.content));
    CHECK(take(replacement->scene().stats()).instance != revision.instance);
    CHECK_FALSE(replacement->scene().entity_state(old));
    CHECK(loaded->scene().entity_state(old));
    write(files.content.scene, {});
    CHECK_FALSE(load_scene_content(files.content));
    CHECK(take(loaded->scene().stats()).revision == revision.revision);
    CHECK(loaded->scene().entity_state(old));
}

TEST_CASE("prepared mesh files resolve references and leases outlive files and scene",
          "[scene-content]") {
    Files files;
    files.content.meshes.push_back(files.root / "mesh.sharkmesh");
    write(files.content.meshes[0], mesh_bytes());
    write(files.content.scene, take(encode_prepared_scene(scene(id<AssetId>('4')))));
    auto loaded = take(load_scene_content(files.content));
    CHECK(loaded->assets()->size() == 3);
    const auto lease = take(loaded->assets()->find(id<AssetId>('4')));
    const auto handle = take(loaded->scene().find(id<AuthoredEntityId>('2')));
    REQUIRE(loaded->scene().destroy_entity(handle));
    loaded.reset();
    REQUIRE(std::filesystem::remove(files.content.meshes[0]));
    CHECK(lease->meshes[0].primitives[0].vertices.size() == 24);
    CHECK(lease->meshes[0].primitives[0].indices.size() == 36);
    auto missing = load_scene_content(files.content);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::io_failure);
    CHECK(missing.error().context.path == text(files.content.meshes[0]));
}

TEST_CASE("prepared file snapshots reject corrupt truncated and extra payload bytes",
          "[scene-content]") {
    Files files;
    const auto original = take(encode_prepared_scene(scene()));
    auto invalid = original;
    invalid.back() ^= std::byte{1};
    write(files.content.scene, invalid);
    auto corrupt = load_scene_content(files.content);
    REQUIRE_FALSE(corrupt);
    CHECK(corrupt.error().context.path == text(files.content.scene));
    write(files.content.scene, std::span{original}.first(original.size() - 1));
    CHECK_FALSE(load_scene_content(files.content));
    invalid = original;
    invalid.push_back(std::byte{});
    write(files.content.scene, invalid);
    CHECK_FALSE(load_scene_content(files.content));
    write(files.content.scene, original);
    files.content.meshes.push_back(files.root / "bad.sharkmesh");
    auto mesh = mesh_bytes();
    mesh.resize(mesh.size() - 1);
    write(files.content.meshes[0], mesh);
    auto truncated_mesh = load_scene_content(files.content);
    REQUIRE_FALSE(truncated_mesh);
    CHECK(truncated_mesh.error().context.path == text(files.content.meshes[0]));
}

TEST_CASE("prepared file loading rejects invalid inventory paths and file bounds",
          "[scene-content]") {
    Files files;
    write(files.content.scene, take(encode_prepared_scene(scene())));
    const auto valid = files.content;
    files.content.meshes.resize(prepared_assets_maximum_count - 1, files.content.scene);
    auto count = load_scene_content(files.content);
    REQUIRE_FALSE(count);
    CHECK(count.error().code == ErrorCode::invalid_argument);
    files.content = valid;
    files.content.meshes.push_back({});
    auto empty = load_scene_content(files.content);
    REQUIRE_FALSE(empty);
    CHECK(empty.error().code == ErrorCode::invalid_path);
    files.content = valid;
    files.content.scene = std::filesystem::path{std::string{"bad\0path", 8}};
    auto nul = load_scene_content(files.content);
    REQUIRE_FALSE(nul);
    CHECK(nul.error().code == ErrorCode::invalid_path);
    files.content.scene = files.root;
    auto directory = load_scene_content(files.content);
    REQUIRE_FALSE(directory);
    CHECK(directory.error().code == ErrorCode::io_failure);
    files.content = valid;
    std::filesystem::resize_file(files.content.scene, prepared_scene_maximum_payload_bytes + 1);
    auto oversized = load_scene_content(files.content);
    REQUIRE_FALSE(oversized);
    CHECK(oversized.error().code == ErrorCode::invalid_argument);
    CHECK(oversized.error().context.path == text(files.content.scene));
    write(files.content.scene, take(encode_prepared_scene(scene())));
    files.content.meshes.push_back(files.root / "large.sharkmesh");
    write(files.content.meshes[0], {});
    std::filesystem::resize_file(files.content.meshes[0], prepared_mesh_maximum_payload_bytes + 1);
    auto mesh_size = load_scene_content(files.content);
    REQUIRE_FALSE(mesh_size);
    CHECK(mesh_size.error().code == ErrorCode::invalid_argument);
    CHECK(mesh_size.error().context.path == text(files.content.meshes[0]));
}

TEST_CASE("prepared files precharge aggregate input before any mesh decoding", "[scene-content]") {
    Files files;
    write(files.content.scene, take(encode_prepared_scene(scene())));
    // Sixteen maximum-sized mesh declarations already fill 32 MiB; the scene
    // makes that inventory too large. One repeated regular file keeps this
    // admission control cheap without creating a large retained test fixture.
    const auto path = files.root / "sized.sharkmesh";
    write(path, {});
    std::filesystem::resize_file(path, prepared_mesh_maximum_payload_bytes);
    files.content.meshes.resize(16, path);
    const auto result = load_scene_content(files.content);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::invalid_argument);
    CHECK(result.error().context.operation == "client.scene.load");
    CHECK(result.error().context.path == text(path));
    CHECK(result.error().message.find("aggregate") != std::string::npos);
}

TEST_CASE("prepared file candidates reject duplicate reserved and unresolved identities",
          "[scene-content]") {
    Files files;
    const auto path = files.root / "mesh.sharkmesh";
    write(path, mesh_bytes());
    write(files.content.scene, take(encode_prepared_scene(scene())));
    files.content.meshes = {path, path};
    auto duplicate = load_scene_content(files.content);
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().context.path == text(path));
    CHECK(duplicate.error().context.asset_id == id<AssetId>('4').value().to_string());
    files.content.meshes = {path};
    write(path, mesh_bytes(builtin_cube_asset_id()));
    auto reserved = load_scene_content(files.content);
    REQUIRE_FALSE(reserved);
    CHECK(reserved.error().context.path == text(path));
    files.content.meshes.clear();
    auto source = scene(id<AssetId>('4'));
    source.entities[1].mesh_renderer->visible = false;
    write(files.content.scene, take(encode_prepared_scene(source)));
    auto unresolved = load_scene_content(files.content);
    REQUIRE_FALSE(unresolved);
    CHECK(unresolved.error().context.path == text(files.content.scene));
    CHECK(unresolved.error().context.asset_id == id<AssetId>('4').value().to_string());
}
