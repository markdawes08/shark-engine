#include "shark/prepared_startup.hpp"
#include "shark/tools/client_export.hpp"
#include "shark/tools/gltf_asset.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
using namespace shark;
using namespace shark::tools;
namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr std::size_t maximum_line_bytes = 2 * 1024 * 1024;
constexpr std::size_t maximum_argument_bytes = 4096;
constexpr std::size_t maximum_arguments = 2 * asset_maximum_catalog_entries + 12;

struct Failure {
    Error error;
};

std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

[[noreturn]] void fail(const char* message, ErrorCode code = ErrorCode::invalid_argument,
                       const fs::path& path = {}) {
    throw Failure{{code, message, {.operation = "content.export.worker", .path = utf8(path)}}};
}

template <typename T> T take(Result<T> result) {
    if (!result)
        throw Failure{std::move(result).error()};
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result)
        throw Failure{std::move(result).error()};
}

fs::path native_path(std::string_view value) {
    return fs::path{std::u8string{reinterpret_cast<const char8_t*>(value.data()), value.size()}};
}

struct Options {
    fs::path project;
    std::string camera;
    std::string target;
    std::vector<ProjectPath> sidecars;
    fs::path vertex_shader;
    fs::path pixel_shader;
    fs::path output;
};

Options options(std::span<const std::string> args) {
    if (args.size() > maximum_arguments || args.size() % 2 != 0)
        fail("Expected bounded option/value pairs");
    Options result;
    std::set<std::string> seen;
    for (std::size_t index = 0; index < args.size(); index += 2) {
        const auto& name = args[index];
        const auto& value = args[index + 1];
        if (name != "--sidecar" && !seen.insert(name).second)
            fail("Duplicate export option");
        if (value.empty() || value.size() > maximum_argument_bytes)
            fail("Export option value is empty or exceeds its byte bound");
        if (name == "--project")
            result.project = native_path(value);
        else if (name == "--camera")
            result.camera = value;
        else if (name == "--content-target")
            result.target = value;
        else if (name == "--sidecar") {
            if (result.sidecars.size() == asset_maximum_catalog_entries)
                fail("Export sidecar inventory exceeds the catalog bound");
            result.sidecars.push_back(take(ProjectPath::parse(value)));
        } else if (name == "--vertex-shader")
            result.vertex_shader = native_path(value);
        else if (name == "--pixel-shader")
            result.pixel_shader = native_path(value);
        else if (name == "--output")
            result.output = native_path(value);
        else
            fail("Unknown export option");
    }
    for (const auto* required : {"--project", "--camera", "--content-target", "--vertex-shader",
                                 "--pixel-shader", "--output"}) {
        if (!seen.contains(required))
            fail("Export requires project, camera, content target, two shaders and fresh output");
    }
    return result;
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

Json envelope(std::string_view status) {
    return {{"format", "shark.export-content-worker"},
            {"schema_version", 1},
            {"status", status},
            {"renderer_initialized", false}};
}

void emit(const Json& value) {
    const auto text = value.dump();
    if (text.size() + 1 > maximum_line_bytes)
        fail("Export worker response exceeds its byte bound", ErrorCode::execution_failed);
    std::cout << text << '\n';
    std::cout.flush();
    if (!std::cout)
        fail("Cannot write export worker response", ErrorCode::io_failure);
}

void expect_command(std::string_view expected) {
    std::array<char, 17> line{};
    std::size_t size = 0;
    while (true) {
        const auto value = std::cin.get();
        if (value == std::char_traits<char>::eof())
            fail("Export parent closed before the required command", ErrorCode::conflict);
        if (value == '\n')
            break;
        if (size == 16)
            fail("Export worker command exceeds its byte bound");
        line[size++] = static_cast<char>(value);
    }
    if (std::string_view{line.data(), size} != expected)
        fail("Unexpected export worker command");
}

int run(std::span<const std::string> args) {
    if (args.size() == 1 && args.front() == "--help") {
        std::cout
            << "Shark ExportContent: private CPU worker for tools/package_runtime.py export\n"
               "--project FILE --camera UUID --content-target TOKEN --sidecar RELATIVE_PATH\n"
               "--vertex-shader FILE --pixel-shader FILE --output FRESH_DIRECTORY\n"
               "Repeat --sidecar for the explicit inventory. Imports are never implicit.\n"
               "The parent must supervise the worker with a deadline. READY waits for\n"
               "VALIDATE, VALIDATED waits for RELEASE; each command is newline terminated.\n"
               "Staging is outside the project root. No package is published by this worker.\n";
        return std::cout ? 0 : 2;
    }
    const auto opt = options(args);
    const auto camera = AuthoredEntityId{take(StableId::parse(opt.camera))};
    const auto source = existing_regular(opt.project);
    const auto vertex_file = existing_regular(opt.vertex_shader);
    const auto pixel_file = existing_regular(opt.pixel_shader);
    const auto vertex = read_shader(vertex_file);
    const auto pixel = read_shader(pixel_file);
    auto files = take(ProjectFiles::open(source.parent_path()));
    const auto project_path = take(ProjectPath::parse(utf8(source.filename())));
    const auto project = take(files->read_project(project_path));
    const auto& document = project.source.document;
    auto loaded = take(files->load_scene(document, document.startup_scene));
    auto catalog = take(AssetCatalog::create(*files, document.project));
    const auto catalog_revision = take(catalog->refresh(opt.sidecars, take(catalog->revision())));
    GltfAssetImporter gltf;
    GltfResourceImporter resource;
    std::array<AssetImporter*, 2> importers{&gltf, &resource};
    auto imports = take(AssetImports::open(*files, *catalog, document.project, importers));
    const auto request = ClientExportRequest{camera,
                                             take(loaded.owner->status()).revision,
                                             catalog_revision,
                                             take(imports->revision()),
                                             {opt.target, AssetAudience::client}};
    const auto candidate = take(prepare_client_export(*loaded.owner, *catalog, *imports, request));
    auto startup = PreparedStartup{candidate.project().value(),
                                   candidate.world(),
                                   candidate.scene(),
                                   camera,
                                   candidate.name(),
                                   std::string{prepared_startup_target},
                                   identity(candidate.scene_payload()),
                                   identity(vertex),
                                   identity(pixel),
                                   {}};
    for (const auto& mesh : candidate.meshes())
        startup.meshes.push_back({mesh.asset, identity(mesh.payload)});
    const auto startup_bytes = take(encode_prepared_startup(startup));
    const auto output = fresh_output(opt.output, source.parent_path());
    Json inventory = Json::object();
    write_file(output, "bin/" + std::string{prepared_startup_filename}, startup_bytes, inventory);
    write_file(output, "bin/" + std::string{prepared_startup_scene_path}, candidate.scene_payload(),
               inventory);
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
    auto ready = envelope("READY");
    ready["content"] = {{"format", "shark.client-export-content"},
                        {"schema_version", 1},
                        {"project", candidate.project().value().to_string()},
                        {"world", candidate.world().value().to_string()},
                        {"scene", candidate.scene().value().to_string()},
                        {"camera", camera.value().to_string()},
                        {"name", candidate.name().value},
                        {"target", candidate.request().target.id},
                        {"dirty", candidate.dirty()},
                        {"files", std::move(inventory)},
                        {"sources", std::move(sources)}};
    emit(ready);
    expect_command("VALIDATE");
    if (take(files->read_project(project_path)).revision != project.revision ||
        take(files->read_scene(document, document.startup_scene)).revision != loaded.file_revision)
        fail("Project or startup scene changed during export", ErrorCode::conflict, source);
    take(validate_client_export(candidate, *loaded.owner, *catalog, *imports));
    if (read_shader(vertex_file) != vertex || read_shader(pixel_file) != pixel)
        fail("Shader inputs changed during export", ErrorCode::conflict);
    emit(envelope("VALIDATED"));
    // The owning thread and ProjectFiles lock remain alive across the parent's
    // final rename. EOF and invalid commands reject; the parent owns the deadline.
    expect_command("RELEASE");
    emit(envelope("RELEASED"));
    return 0;
}

int emit_error(const Error& error) noexcept {
    try {
        auto reply = envelope("ERROR");
        reply["error"] = {
            {"code", error_code_name(error.code)},   {"message", error.message},
            {"operation", error.context.operation},  {"path", error.context.path},
            {"entity_id", error.context.entity_id},  {"asset_id", error.context.asset_id},
            {"request_id", error.context.request_id}};
        emit(reply);
    } catch (...) {
        std::cout << "{\"format\":\"shark.export-content-worker\",\"schema_version\":1,"
                     "\"status\":\"ERROR\",\"renderer_initialized\":false,"
                     "\"error\":{\"code\":\"execution_failed\","
                     "\"message\":\"Cannot report export worker failure\"}}\n";
        std::cout.flush();
    }
    return 2;
}

int guarded_run(std::span<const std::string> args) noexcept {
    try {
        return run(args);
    } catch (const Failure& error) {
        return emit_error(error.error);
    } catch (const fs::filesystem_error&) {
        return emit_error({ErrorCode::io_failure,
                           "Export filesystem operation failed",
                           {.operation = "content.export.worker"}});
    } catch (...) {
        return emit_error({ErrorCode::execution_failed,
                           "Export worker failed",
                           {.operation = "content.export.worker"}});
    }
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc < 1 || static_cast<std::size_t>(argc - 1) > maximum_arguments)
            fail("Too many export arguments");
        std::vector<std::string> args;
        for (int index = 1; index < argc; ++index) {
            const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1,
                                                  nullptr, 0, nullptr, nullptr);
            if (size <= 0 || static_cast<std::size_t>(size) > maximum_argument_bytes + 1)
                fail("Export argument is invalid Unicode or exceeds its byte bound");
            std::string text(static_cast<std::size_t>(size), '\0');
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1, text.data(),
                                    size, nullptr, nullptr) != size)
                fail("Cannot convert Unicode export argument");
            text.pop_back();
            args.push_back(std::move(text));
        }
        return guarded_run(args);
    } catch (const Failure& error) {
        return emit_error(error.error);
    } catch (...) {
        return emit_error({ErrorCode::execution_failed, "Cannot decode export arguments", {}});
    }
}
#else
int main(int argc, char** argv) {
    try {
        if (argc < 1 || static_cast<std::size_t>(argc - 1) > maximum_arguments)
            fail("Too many export arguments");
        std::vector<std::string> args;
        for (int index = 1; index < argc; ++index) {
            const std::string_view value{argv[index]};
            if (value.size() > maximum_argument_bytes)
                fail("Export argument exceeds its byte bound");
            args.emplace_back(value);
        }
        return guarded_run(args);
    } catch (const Failure& error) {
        return emit_error(error.error);
    } catch (...) {
        return emit_error({ErrorCode::execution_failed, "Cannot decode export arguments", {}});
    }
}
#endif
