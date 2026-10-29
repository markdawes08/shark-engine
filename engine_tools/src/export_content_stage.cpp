#include "export_content_stage.hpp"
#include "shark/prepared_startup.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace shark::tools {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
struct Failure {
    Error error;
};
std::string utf8(const fs::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
fs::path native_path(std::string_view value) {
    return fs::path{std::u8string{reinterpret_cast<const char8_t*>(value.data()), value.size()}};
}
[[noreturn]] void fail(const char* message, ErrorCode code = ErrorCode::invalid_argument,
                       const fs::path& path = {}) {
    throw Failure{{code, message, {.operation = "content.export.stage", .path = utf8(path)}}};
}
template <typename T> T take(Result<T> value) {
    if (!value)
        throw Failure{std::move(value).error()};
    return std::move(value).value();
}
template <typename T, typename F> Result<T> boundary(F&& function) {
    try {
        return function();
    } catch (Failure& error) {
        return std::move(error.error);
    } catch (const fs::filesystem_error&) {
        return Error{ErrorCode::io_failure,
                     "Cannot access export staging paths",
                     {.operation = "content.export.stage"}};
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Cannot stage export content",
                     {.operation = "content.export.stage"}};
    }
}
// These are private staging inputs, not a hostile filesystem sandbox. As with
// ProjectFiles, uncooperative topology changes must not race inspection/open.
void no_links(const fs::path& absolute) {
    fs::path current;
    for (const auto& part : absolute) {
        current /= part;
        std::error_code error;
        const auto status = fs::symlink_status(current, error);
        if (error == std::errc::no_such_file_or_directory)
            continue;
        if (error)
            fail("Cannot inspect export path", ErrorCode::io_failure, current);
        if (fs::is_symlink(status))
            fail("Export paths must not contain links", ErrorCode::invalid_path, current);
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            fail("Export paths must not contain reparse points", ErrorCode::invalid_path, current);
#endif
    }
}

fs::path existing_regular(const fs::path& input) {
    const auto absolute = fs::absolute(input);
    no_links(absolute);
    if (!fs::is_regular_file(absolute) || fs::hard_link_count(absolute) != 1)
        fail("Export input must be a single-link regular file", ErrorCode::invalid_path, absolute);
    return fs::canonical(absolute);
}

std::vector<std::byte> read_shader(const fs::path& file) {
    const auto checked = existing_regular(file);
    const auto size = fs::file_size(checked);
    if (size < 20 || size > prepared_startup_shader_maximum_bytes || size % 4 != 0)
        fail("Shader exceeds the prepared shader size contract", ErrorCode::invalid_argument, file);
    std::vector<std::byte> result(static_cast<std::size_t>(size));
    std::ifstream stream{checked, std::ios::binary};
    if (!stream.read(reinterpret_cast<char*>(result.data()),
                     static_cast<std::streamsize>(result.size())) ||
        stream.peek() != std::char_traits<char>::eof())
        fail("Cannot read an unchanged complete shader", ErrorCode::io_failure, file);
    constexpr std::array magic{std::byte{0x03}, std::byte{0x02}, std::byte{0x23}, std::byte{0x07}};
    if (!std::equal(magic.begin(), magic.end(), result.begin()))
        fail("Shader is not little-endian SPIR-V", ErrorCode::invalid_argument, file);
    return result;
}

fs::path fresh_output(const fs::path& input, const fs::path& project_root) {
    const auto absolute = fs::absolute(input);
    no_links(absolute);
    if (absolute.filename().empty())
        fail("Export staging output needs a directory name", ErrorCode::invalid_path, absolute);
    static_cast<void>(take(ProjectPath::parse(utf8(absolute.filename()))));
    if (fs::exists(absolute))
        fail("Export staging output already exists", ErrorCode::conflict, absolute);
    const auto parent = fs::canonical(absolute.parent_path());
    if (!fs::is_directory(parent))
        fail("Export staging parent must exist", ErrorCode::invalid_path, parent);
    for (auto ancestor = parent; !ancestor.empty();) {
        if (fs::equivalent(ancestor, project_root))
            fail("Export staging must be outside the source project root",
                 ErrorCode::path_outside_root, absolute);
        const auto next = ancestor.parent_path();
        if (next == ancestor)
            break;
        ancestor = next;
    }
    const auto result = parent / absolute.filename();
    if (!fs::create_directory(result))
        fail("Cannot create fresh export staging directory", ErrorCode::conflict, result);
    return result;
}

PreparedFileIdentity identity(std::span<const std::byte> payload) {
    return {static_cast<std::uint32_t>(payload.size()), take(sha256(payload))};
}

void write_file(const fs::path& root, std::string_view relative, std::span<const std::byte> bytes,
                Json& files) {
    const auto relative_path = take(ProjectPath::parse(relative));
    const auto destination = root / native_path(relative_path.text());
    no_links(destination.parent_path());
    fs::create_directories(destination.parent_path());
    if (fs::exists(destination))
        fail("Export staging file unexpectedly exists", ErrorCode::conflict, destination);
    std::ofstream stream{destination, std::ios::binary};
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream)
        fail("Cannot write complete export staging file", ErrorCode::io_failure, destination);
    files[std::string{relative}] = {{"sha256", take(sha256(bytes)).to_hex()},
                                    {"bytes", bytes.size()}};
}

} // namespace

Result<fs::path> checked_export_file(const fs::path& input) {
    return boundary<fs::path>([&] { return existing_regular(input); });
}
Result<ExportContentStage> stage_client_export(const PreparedClientExport& candidate,
                                               const fs::path& project_root,
                                               const fs::path& vertex_shader,
                                               const fs::path& pixel_shader,
                                               const fs::path& output_path) {
    return boundary<ExportContentStage>([&] {
        const auto vertex_file = existing_regular(vertex_shader);
        const auto pixel_file = existing_regular(pixel_shader);
        auto vertex = read_shader(vertex_file);
        auto pixel = read_shader(pixel_file);
        auto startup = PreparedStartup{candidate.project().value(),
                                       candidate.world(),
                                       candidate.scene(),
                                       candidate.request().camera,
                                       candidate.name(),
                                       std::string{prepared_startup_target},
                                       identity(candidate.scene_payload()),
                                       identity(vertex),
                                       identity(pixel),
                                       {}};
        for (const auto& mesh : candidate.meshes())
            startup.meshes.push_back({mesh.asset, identity(mesh.payload)});
        const auto startup_bytes = take(encode_prepared_startup(startup));
        const auto output = fresh_output(output_path, project_root);
        Json inventory = Json::object();
        write_file(output, "bin/" + std::string{prepared_startup_filename}, startup_bytes,
                   inventory);
        write_file(output, "bin/" + std::string{prepared_startup_scene_path},
                   candidate.scene_payload(), inventory);
        write_file(output, "bin/" + std::string{prepared_startup_vertex_path}, vertex, inventory);
        write_file(output, "bin/" + std::string{prepared_startup_pixel_path}, pixel, inventory);
        for (const auto& mesh : candidate.meshes())
            write_file(output, "bin/" + prepared_startup_mesh_path(mesh.asset), mesh.payload,
                       inventory);
        Json sources = Json::array();
        for (const auto& item : candidate.sources())
            sources.push_back({{"asset", item.asset.value().to_string()},
                               {"key", item.key.to_hex()},
                               {"origin", item.origin},
                               {"license", item.license}});
        Json content = {{"format", "shark.client-export-content"},
                        {"schema_version", 1},
                        {"project", candidate.project().value().to_string()},
                        {"world", candidate.world().value().to_string()},
                        {"scene", candidate.scene().value().to_string()},
                        {"camera", candidate.request().camera.value().to_string()},
                        {"name", candidate.name().value},
                        {"target", candidate.request().target.id},
                        {"dirty", candidate.dirty()},
                        {"files", std::move(inventory)},
                        {"sources", std::move(sources)}};

        return ExportContentStage{std::move(content), vertex_file, pixel_file, std::move(vertex),
                                  std::move(pixel)};
    });
}
Result<void> validate_export_content_stage(const ExportContentStage& stage) {
    return boundary<void>([&]() -> Result<void> {
        if (read_shader(stage.vertex_path) != stage.vertex ||
            read_shader(stage.pixel_path) != stage.pixel)
            fail("Shader inputs changed during export", ErrorCode::conflict);
        return {};
    });
}
} // namespace shark::tools
