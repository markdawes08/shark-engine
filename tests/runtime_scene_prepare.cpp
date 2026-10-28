// Original Shark fixture composition; reused glTF fixture has its adjacent MIT license.
// This executable links CPU Tools only. Runtime consumes only the prepared outputs.
#include <shark/tools/gltf_asset.hpp>
#include <shark/tools/prepared_content.hpp>
#include <shark/tools/project_files.hpp>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace shark;
using namespace shark::tools;

template <typename T> T take(Result<T> result) {
    if (!result) {
        throw std::runtime_error(result.error().context.operation + ": " + result.error().message);
    }
    return std::move(result).value();
}

template <typename T> T id(char digit) {
    std::string text = "13e00000-0000-4000-8000-000000000001";
    text.back() = digit;
    return T{take(StableId::parse(text))};
}

std::string read(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    if (size > source_maximum_bytes)
        throw std::runtime_error("Fixture input exceeds source bound");
    std::string result(static_cast<std::size_t>(size), '\0');
    std::ifstream stream{path, std::ios::binary};
    if (!stream.read(result.data(), static_cast<std::streamsize>(result.size())))
        throw std::runtime_error("Cannot read fixture input");
    return result;
}

void write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream stream{path, std::ios::binary};
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream)
        throw std::runtime_error("Cannot write prepared fixture");
}

void edit(AuthoredScene& owner, const AuthoringCommand& command) {
    const auto reply = take(owner.execute(command, take(owner.status()).revision));
    static_cast<void>(take(reply.outcome));
}

Camera camera(double height) {
    Camera value;
    value.projection = CameraProjection::orthographic;
    value.orthographic_height = height;
    value.near_clip = 1;
    value.far_clip = 9;
    return value;
}

ProjectDocument project() {
    return {id<ProjectId>('1'),
            id<WorldId>('2'),
            Name{"Runtime acceptance"},
            id<SceneId>('3'),
            {{id<SceneId>('3'), take(ProjectPath::parse("scene.sharkscene"))}},
            "{}"};
}

void save(const std::filesystem::path& root, const AuthoredScene& owner) {
    const auto captured = take(owner.capture());
    // A saved source project proves this input can continue through normal Tools.
    // The Runtime process receives only scene.bin and optional mesh.bin below.
    for (const auto& entry : captured.project.scenes)
        std::filesystem::create_directories(root /
                                            std::filesystem::path{entry.path.text()}.parent_path());
    auto files = take(ProjectFiles::open(root));
    static_cast<void>(take(files->write_project(take(ProjectPath::parse("project.sharkproject")),
                                                captured.project, std::nullopt)));
    static_cast<void>(take(files->write_scene(captured.project, captured.scene, std::nullopt)));
    write(root / "scene.bin", take(prepare_scene(owner, captured.revision)));
}

void featureworld(const std::filesystem::path& source, const std::filesystem::path& output) {
    auto files = take(ProjectFiles::open(source / "assets/FeatureWorld"));
    const auto source_project =
        take(files->read_project(take(ProjectPath::parse("FeatureWorld.sharkproject"))));
    auto loaded = take(files->load_scene(source_project.source.document,
                                         source_project.source.document.startup_scene));
    auto& owner = *loaded.owner;
    const auto camera_id =
        AuthoredEntityId{take(StableId::parse("20000000-0000-4000-8000-000000000004"))};
    Transform pose;
    pose.translation = {-4, 0, 3};
    edit(owner, SetTransformCommand{camera_id, pose});
    edit(owner, SetCameraCommand{camera_id, camera(3)});
    save(output, owner);
}

void fresh(const std::filesystem::path& output) {
    const auto document = project();
    auto owner = take(
        AuthoredScene::create(document, {document.project, document.startup_scene, {}, {}, "{}"}));
    Transform eye;
    eye.translation.z = 4;
    edit(*owner,
         CreateEntityCommand{id<AuthoredEntityId>('4'), Name{"Camera"}, eye, {}, camera(4), {}});
    Transform cube;
    cube.translation.x = -1;
    edit(*owner, CreateEntityCommand{id<AuthoredEntityId>('5'),
                                     Name{"Cube"},
                                     cube,
                                     {},
                                     {},
                                     MeshRenderer{builtin_cube_asset_id(), 0, true}});
    Transform plane;
    plane.translation.x = 1;
    plane.scale = math::Vec3{1.5};
    const auto half = std::sqrt(0.5);
    plane.rotation = math::Quat{half, half, 0, 0};
    edit(*owner, CreateEntityCommand{id<AuthoredEntityId>('6'),
                                     Name{"Plane"},
                                     plane,
                                     {},
                                     {},
                                     MeshRenderer{builtin_plane_asset_id(), 0, true}});
    save(output, *owner);
}

void imported(const std::filesystem::path& source, const std::filesystem::path& output) {
    const auto input = read(source / "tests/fixtures/gltf_source/embedded.gltf");
    const auto imported_asset =
        take(convert_gltf_asset({take(ProjectPath::parse("embedded.gltf")),
                                 std::as_bytes(std::span{input.data(), input.size()})}));
    const auto cached = take(encode_gltf_asset(imported_asset));
    const auto asset = id<AssetId>('a');
    const std::array nodes{id<AuthoredEntityId>('5')};
    const auto prepared = take(
        decode_prepared_scene(take(prepare_gltf_scene(asset, id<SceneId>('3'), cached, nodes))));
    const auto project_document = project();
    SceneDocument scene{project_document.project, prepared.scene, {}, {}, "{}"};
    for (const auto& node : prepared.entities)
        scene.entities.push_back({node.id, node.parent, node.name, node.transform, "{}",
                                  node.camera, node.mesh_renderer});
    auto owner = take(AuthoredScene::create(project_document, scene));
    Transform triangle;
    triangle.translation = {-1, -1, 0};
    triangle.scale = math::Vec3{2};
    edit(*owner, SetTransformCommand{nodes[0], triangle});
    Transform eye;
    eye.translation.z = 3;
    edit(*owner,
         CreateEntityCommand{id<AuthoredEntityId>('4'), Name{"Camera"}, eye, {}, camera(3), {}});
    save(output, *owner);
    write(output / "mesh.bin", take(prepare_mesh_asset(asset, cached)));
    const auto license = read(source / "tests/fixtures/gltf_source/LICENSE");
    write(output / "LICENSE", std::as_bytes(std::span{license.data(), license.size()}));

    // The standalone export command consumes the actual saved project and fresh
    // import cache, not this fixture's already prepared mesh.bin shortcut.
    write(output / "embedded.gltf", std::as_bytes(std::span{input.data(), input.size()}));
    auto files = take(ProjectFiles::open(output));
    const auto sidecar = take(ProjectPath::parse("embedded.gltf.sharkasset"));
    const auto metadata = AssetMetadata{project_document.project,
                                        asset,
                                        "gltf",
                                        "Shark original static glTF fixture",
                                        "MIT",
                                        "shark.gltf",
                                        AssetAudience::client,
                                        R"({"path":"embedded.gltf"})",
                                        {},
                                        "{}"};
    static_cast<void>(take(files->write_asset_metadata(sidecar, metadata, std::nullopt)));
    auto catalog = take(AssetCatalog::create(*files, project_document.project));
    const std::array sidecars{sidecar};
    const auto catalog_revision = take(catalog->refresh(sidecars, take(catalog->revision())));
    GltfAssetImporter gltf;
    GltfResourceImporter resource;
    const std::array<AssetImporter*, 2> importers{&gltf, &resource};
    auto imports = take(AssetImports::open(*files, *catalog, project_document.project, importers));
    const AssetTargetProfile target{"windows-x64-export-test", AssetAudience::client};
    const auto cache_revision = take(imports->revision());
    auto prepared_import = take(imports->prepare(asset, target, catalog_revision, cache_revision));
    const auto summary = take(prepared_import.summary());
    const auto committed =
        take(imports->publish(std::move(prepared_import), catalog_revision, cache_revision));
    if (take(imports->read(summary.root_key, committed.revision)).payload != cached)
        throw std::runtime_error("Cached fixture changed the independently prepared geometry");
}

std::filesystem::path path(std::string_view value) {
    return std::filesystem::path{
        std::u8string{reinterpret_cast<const char8_t*>(value.data()), value.size()}};
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc != 5 || std::string_view{argv[1]} != "--source-root" ||
        std::string_view{argv[3]} != "--output") {
        std::cerr << "Usage: RuntimeScenePrepare --source-root PATH --output NEW_DIRECTORY\n";
        return 2;
    }
    try {
        const auto source = std::filesystem::absolute(path(argv[2]));
        const auto output = std::filesystem::absolute(path(argv[4]));
        if (!std::filesystem::create_directory(output))
            throw std::runtime_error("Fixture output must be a new directory");
        for (const auto* name : {"featureworld", "fresh", "imported"})
            std::filesystem::create_directory(output / name);
        featureworld(source, output / "featureworld");
        fresh(output / "fresh");
        imported(source, output / "imported");
        std::cout << "{\"status\":\"PASS\",\"cases\":3,\"device_accessed\":false}\n";
        return std::cout ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
