#include <shark/client/prepared_package.hpp>

#include <shark/project_path.hpp>

#include "executable_path.hpp"
#include "shader_path.hpp"

#include <fstream>

namespace shark::client {
namespace {
constexpr std::string_view operation = "client.package.load";
struct Failure {
    Error error;
};
template <typename T> T take(Result<T> result) {
    if (!result)
        throw Failure{std::move(result).error()};
    return std::move(result).value();
}
[[noreturn]] void fail(ErrorCode code, const char* message) {
    throw Failure{{code, message, {std::string{operation}}}};
}

std::filesystem::path resolve(const ProjectRoot& root, std::string_view relative) {
    return take(root.resolve(take(ProjectPath::parse(relative))));
}

std::vector<std::byte> read(const std::filesystem::path& path, std::size_t limit,
                            std::optional<PreparedFileIdentity> expected = {}) {
    std::error_code status;
    if (!std::filesystem::is_regular_file(path, status))
        fail(ErrorCode::io_failure, "Package payload must be an accessible regular file");
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        fail(ErrorCode::io_failure, "Cannot open package payload");
    const auto size = stream.tellg();
    if (size < 0 || size > static_cast<std::streamoff>(limit) ||
        (expected && size != static_cast<std::streamoff>(expected->bytes)))
        fail(ErrorCode::invalid_configuration,
             "Package payload length differs from its bounded inventory");
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    if (!stream ||
        (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()),
                                        static_cast<std::streamsize>(bytes.size()))) ||
        stream.peek() != std::char_traits<char>::eof() || stream.bad())
        fail(ErrorCode::io_failure, "Cannot read a complete package payload snapshot");
    if (expected && take(sha256(bytes)) != expected->digest)
        fail(ErrorCode::invalid_configuration,
             "Package payload SHA-256 differs from its inventory");
    return bytes;
}
} // namespace

Result<LoadedPreparedPackage> load_prepared_package(const std::filesystem::path& startup_file) {
    std::string current_path;
    try {
        current_path = take(detail::artifact_path_text(startup_file));
        const auto parent = startup_file.parent_path();
        const auto root =
            take(ProjectRoot::open(parent.empty() ? std::filesystem::path{"."} : parent));
        const auto name = take(detail::artifact_path_text(startup_file.filename()));
        const auto descriptor_path = resolve(root, name);
        const auto descriptor = read(descriptor_path, prepared_startup_maximum_bytes);
        auto startup = take(decode_prepared_startup(descriptor));
        const auto descriptor_digest = take(sha256(descriptor));
        auto read_relative = [&](std::string_view relative, std::size_t limit,
                                 PreparedFileIdentity expected) {
            current_path = std::string{relative};
            const auto path = resolve(root, relative);
            current_path = take(detail::artifact_path_text(path));
            return read(path, limit, expected);
        };
        const auto scene = read_relative(prepared_startup_scene_path,
                                         prepared_scene_maximum_payload_bytes, startup.scene_file);
        std::vector<std::vector<std::byte>> payloads;
        std::vector<PreparedAssetInput> inputs;
        payloads.reserve(startup.meshes.size());
        inputs.reserve(startup.meshes.size());
        for (const auto& mesh : startup.meshes) {
            payloads.push_back(read_relative(prepared_startup_mesh_path(mesh.asset),
                                             prepared_mesh_maximum_payload_bytes, mesh.file));
            inputs.push_back({mesh.asset, payloads.back()});
        }
        auto assets = take(PreparedAssets::create(inputs));
        current_path = std::string{prepared_startup_scene_path};
        auto loaded = take(LoadedPreparedScene::load(scene, std::move(assets)));
        const auto camera = take(loaded->scene().find(startup.camera));
        const auto state = take(loaded->scene().entity_state(camera));
        const auto* scope = std::get_if<AuthoredSceneScope>(&state.reference.scope);
        if (!scope || scope->scene != startup.scene || !state.camera ||
            (loaded->world_origin() && loaded->world_origin()->world != startup.world))
            fail(ErrorCode::invalid_configuration,
                 "Package scene, world or camera differs from startup identity");
        const auto vertex_bytes =
            read_relative(prepared_startup_vertex_path, prepared_startup_shader_maximum_bytes,
                          startup.vertex_shader);
        auto vertex = take(decode_shader_artifact(vertex_bytes, ShaderStage::vertex,
                                                  ShaderInterface::scene_unlit));
        const auto pixel_bytes =
            read_relative(prepared_startup_pixel_path, prepared_startup_shader_maximum_bytes,
                          startup.pixel_shader);
        auto pixel = take(
            decode_shader_artifact(pixel_bytes, ShaderStage::pixel, ShaderInterface::scene_unlit));
        return LoadedPreparedPackage{std::move(startup), descriptor_digest, std::move(loaded),
                                     std::move(vertex), std::move(pixel)};
    } catch (Failure& failure) {
        failure.error.context.operation = operation;
        if (failure.error.context.path.empty())
            failure.error.context.path = current_path;
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Package candidate loading failed",
                     {std::string{operation}, {}, current_path}};
    }
}

Result<std::filesystem::path> default_prepared_startup_path() {
    auto directory = detail::executable_directory();
    if (!directory)
        return directory.error();
    return directory.value() / prepared_startup_filename;
}
} // namespace shark::client
