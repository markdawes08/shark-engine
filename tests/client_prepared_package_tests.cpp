#include <shark/client/prepared_package.hpp>
#include <shark/client/scene_launch.hpp>

#include "gpu_shader_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>

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
std::vector<std::byte> shader_bytes(std::span<const std::uint32_t> words) {
    std::vector<std::byte> result;
    for (const auto word : words)
        for (unsigned shift = 0; shift < 32; shift += 8)
            result.push_back(static_cast<std::byte>((word >> shift) & 255U));
    return result;
}
PreparedFileIdentity identity(std::span<const std::byte> bytes) {
    return {static_cast<std::uint32_t>(bytes.size()), take(sha256(bytes))};
}
void write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.close();
    REQUIRE(stream);
}
struct Package {
    std::filesystem::path root;
    std::filesystem::path path;
    PreparedStartup startup{id<StableId>('1'), id<WorldId>('2'), id<SceneId>('3'),
                            id<AuthoredEntityId>('4'), Name{"Package"}};
    PreparedScene scene{startup.scene, WorldPosition{startup.world, {}, {}}, {}};
    std::vector<std::byte> scene_bytes;
    std::vector<std::byte> vertex = shader_bytes(test::shaders::scene_vs_words);
    std::vector<std::byte> pixel = shader_bytes(test::shaders::scene_ps_words);
    Package() {
        root = std::filesystem::temp_directory_path() /
               ("shark-package-" + take(generate_stable_id()).to_string()) /
               std::filesystem::path{u8"relocated space \u96ea"};
        REQUIRE(std::filesystem::create_directories(root / "content"));
        REQUIRE(std::filesystem::create_directory(root / "assets"));
        path = root / prepared_startup_filename;
        PreparedEntity camera{startup.camera};
        camera.camera = Camera{};
        camera.transform.translation = math::Vec3{0, 0, 5};
        PreparedEntity mesh{id<AuthoredEntityId>('5')};
        mesh.mesh_renderer = MeshRenderer{builtin_cube_asset_id()};
        scene.entities = {camera, mesh};
        publish_scene();
        startup.vertex_shader = identity(vertex);
        startup.pixel_shader = identity(pixel);
        write(root / prepared_startup_vertex_path, vertex);
        write(root / prepared_startup_pixel_path, pixel);
        publish_startup();
    }
    ~Package() {
        std::error_code ignored;
        std::filesystem::remove_all(root.parent_path(), ignored);
    }
    Package(const Package&) = delete;
    Package& operator=(const Package&) = delete;
    void publish_scene() {
        scene_bytes = take(encode_prepared_scene(scene));
        startup.scene_file = identity(scene_bytes);
        write(root / prepared_startup_scene_path, scene_bytes);
    }
    void publish_startup() {
        write(path, take(encode_prepared_startup(startup)));
    }
};
} // namespace

TEST_CASE("Package loader owns checked scene shader and identity snapshots without graphics",
          "[prepared-package]") {
    Package package;
    auto first = take(load_prepared_package(package.path));
    CHECK(first.startup == package.startup);
    CHECK(first.content->assets()->size() == 2);
    CHECK(first.vertex_shader.words ==
          std::vector(test::shaders::scene_vs_words.begin(), test::shaders::scene_vs_words.end()));
    CHECK(first.pixel_shader.words ==
          std::vector(test::shaders::scene_ps_words.begin(), test::shaders::scene_ps_words.end()));
    const auto handle = take(first.content->scene().find(package.startup.camera));
    CHECK(take(first.content->scene().entity_state(handle)).camera.has_value());
    const auto digest = first.startup_digest;
    auto second = take(load_prepared_package(package.path));
    CHECK(second.startup_digest == digest);
    CHECK(take(second.content->scene().stats()).instance !=
          take(first.content->scene().stats()).instance);
    package.scene_bytes.back() ^= std::byte{1};
    write(package.root / prepared_startup_scene_path, package.scene_bytes);
    CHECK_FALSE(load_prepared_package(package.path));
    CHECK(take(first.content->scene().entity_state(handle)).camera.has_value());
    CHECK(first.vertex_shader.words.front() == 0x07230203U);
    const auto located = take(default_prepared_startup_path());
    CHECK(located.is_absolute());
    CHECK(located.filename() == prepared_startup_filename);
}

TEST_CASE("Package loader validates external mesh identities and every hidden reference",
          "[prepared-package]") {
    Package package;
    const auto assets = take(PreparedAssets::create());
    auto mesh = *take(assets->find(builtin_cube_asset_id()));
    mesh.asset = id<AssetId>('6');
    const auto bytes = take(encode_prepared_mesh(mesh));
    package.startup.meshes.push_back({mesh.asset, identity(bytes)});
    package.scene.entities[1].mesh_renderer = MeshRenderer{mesh.asset};
    package.scene.entities[1].mesh_renderer->visible = false;
    package.publish_scene();
    write(package.root / prepared_startup_mesh_path(mesh.asset), bytes);
    package.publish_startup();
    auto loaded = take(load_prepared_package(package.path));
    CHECK(loaded.content->assets()->size() == 3);
    package.scene.entities[1].mesh_renderer->mesh_index = 99;
    package.publish_scene();
    package.publish_startup();
    CHECK_FALSE(load_prepared_package(package.path));
    package.scene.entities[1].mesh_renderer->mesh_index = 0;
    package.publish_scene();
    package.startup.meshes[0].asset = id<AssetId>('7');
    write(package.root / prepared_startup_mesh_path(id<AssetId>('7')), bytes);
    package.publish_startup();
    CHECK_FALSE(load_prepared_package(package.path));
}

TEST_CASE("Package loader rejects descriptor identity mismatches missing and corrupt typed inputs",
          "[prepared-package]") {
    Package package;
    auto original = package.startup;
    package.startup.scene = id<SceneId>('8');
    package.publish_startup();
    CHECK_FALSE(load_prepared_package(package.path));
    package.startup = original;
    package.startup.world = id<WorldId>('8');
    package.publish_startup();
    CHECK_FALSE(load_prepared_package(package.path));
    package.startup = original;
    package.startup.camera = id<AuthoredEntityId>('5');
    package.publish_startup();
    CHECK_FALSE(load_prepared_package(package.path));
    package.startup = original;
    package.publish_startup();
    auto damaged = package.vertex;
    damaged[8] ^= std::byte{1};
    write(package.root / prepared_startup_vertex_path, damaged);
    auto rejected = load_prepared_package(package.path);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().message.find("SHA-256") != std::string::npos);
    package.startup.vertex_shader = identity(package.pixel);
    write(package.root / prepared_startup_vertex_path, package.pixel);
    package.publish_startup();
    CHECK_FALSE(load_prepared_package(package.path));
    write(package.root / prepared_startup_vertex_path, package.vertex);
    package.startup = original;
    package.publish_startup();
    REQUIRE(std::filesystem::remove(package.root / prepared_startup_pixel_path));
    CHECK_FALSE(load_prepared_package(package.path));
    write(package.root / prepared_startup_pixel_path, package.pixel);
    std::filesystem::resize_file(package.path, prepared_startup_maximum_bytes + 1);
    CHECK_FALSE(load_prepared_package(package.path));
    CHECK_FALSE(load_prepared_package({}));
    CHECK_FALSE(load_prepared_package(package.root));
}

TEST_CASE("Package launch arguments select complete inventory without overrides",
          "[prepared-package]") {
    std::vector<std::string_view> args{
        "--package", "startup.sharkstart", "--frames", "2", "--validation",
        "off",       "--extent",           "96x80"};
    auto parsed = take(parse_scene_arguments(args));
    CHECK(parsed.package == "startup.sharkstart");
    CHECK_FALSE(parsed.camera);
    CHECK(parsed.content.scene.empty());
    CHECK(parsed.frames == 2);
    for (const auto& [key, value] : std::array<std::pair<std::string_view, std::string_view>, 5>{
             {{"--scene", "scene"},
              {"--mesh", "mesh"},
              {"--vertex-shader", "v"},
              {"--pixel-shader", "p"},
              {"--camera", "10000000-0000-4000-8000-000000000004"}}}) {
        auto conflicting = args;
        conflicting.insert(conflicting.end(), {key, value});
        CHECK_FALSE(parse_scene_arguments(conflicting));
    }
    args.insert(args.end(), {"--package", "other"});
    CHECK_FALSE(parse_scene_arguments(args));
}
