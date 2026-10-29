#include "authoring_capture.hpp"

#include "shark/client/graphics.hpp"
#include "shark/content_digest.hpp"
#include "shark/prepared_scene.hpp"

#include <nlohmann/json.hpp>

#include <bit>
#include <cmath>
#include <fstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace shark::editor {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace client;
using namespace tools;

struct Failure {
    Error error;
};

std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

[[noreturn]] void fail(ErrorCode code, const char* message, const fs::path& path = {}) {
    throw Failure{{code, message, {.operation = "authoring.capture", .path = utf8(path)}}};
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

// As for ProjectFiles, uncooperative topology edits must not race inspection,
// writing and rename. Reject links/reparse points before opening any artifact.
void no_links(const fs::path& absolute) {
    fs::path current;
    for (const auto& part : absolute) {
        current /= part;
        std::error_code error;
        const auto status = fs::symlink_status(current, error);
        if (error == std::errc::no_such_file_or_directory)
            continue;
        if (error)
            fail(ErrorCode::io_failure, "Cannot inspect capture path", current);
        if (fs::is_symlink(status))
            fail(ErrorCode::invalid_path, "Capture paths cannot contain links", current);
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            fail(ErrorCode::invalid_path, "Capture paths cannot contain reparse points", current);
#endif
    }
}

fs::path shader_path(const fs::path& input) {
    const auto absolute = fs::absolute(input);
    no_links(absolute);
    if (!fs::is_regular_file(absolute) || fs::hard_link_count(absolute) != 1)
        fail(ErrorCode::invalid_path, "Capture shader must be a single-link regular file",
             absolute);
    return fs::canonical(absolute);
}

struct Stage {
    fs::path path;
    bool published{false};
    ~Stage() {
        if (!published) {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    }
};

Json revision(SceneRevision value) {
    return {{"scene_instance", std::to_string(value.scene_instance)},
            {"revision", std::to_string(value.revision)}};
}
Json origin_json(const RenderOrigin& value) {
    Json anchor = nullptr;
    if (value.world_anchor) {
        const auto& position = *value.world_anchor;
        anchor = {{"world_id", position.world.value().to_string()},
                  {"cell", {position.cell.x, position.cell.y, position.cell.z}},
                  {"offset", position.offset}};
    }
    return {{"scene_local_offset",
             {value.scene_local_offset.x, value.scene_local_offset.y, value.scene_local_offset.z}},
            {"generation", std::to_string(value.generation)},
            {"world_anchor", std::move(anchor)}};
}
Json write_bytes(const fs::path& path, std::span<const std::byte> bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream)
        fail(ErrorCode::io_failure, "Cannot write capture artifact", path);
    return {{"bytes", bytes.size()}, {"sha256", take(sha256(bytes)).to_hex()}};
}

class Capture final : public AuthoringCaptureProvider {
  public:
    Capture(ShaderArtifact vertex, ShaderArtifact pixel, std::string adapter,
            GraphicsValidation validation)
        : vertex_(std::move(vertex)), pixel_(std::move(pixel)), adapter_(std::move(adapter)),
          validation_(validation) {}

    Result<AuthoringArtifactResult>
    capture(const PreparedClientExport& content, const AuthoringCaptureRequest& request,
            const std::function<Result<void>()>& validate) override {
        try {
            if (!validate || request.viewport.width == 0 || request.viewport.height == 0 ||
                request.viewport.width > scene_viewport_maximum_extent ||
                request.viewport.height > scene_viewport_maximum_extent ||
                request.viewport.width > authoring_capture_maximum_extent ||
                request.viewport.height > authoring_capture_maximum_extent ||
                std::uint64_t{request.viewport.width} * request.viewport.height >
                    authoring_capture_maximum_pixels)
                fail(ErrorCode::invalid_argument,
                     "Capture needs a validation callback and bounded extent");
            const auto absolute = fs::absolute(request.output).lexically_normal();
            no_links(absolute);
            if (absolute.filename().empty() || fs::exists(absolute))
                fail(ErrorCode::conflict, "Capture output must be a fresh named directory",
                     absolute);
            static_cast<void>(take(ProjectPath::parse(utf8(absolute.filename()))));
            const auto parent = fs::canonical(absolute.parent_path());
            if (!fs::is_directory(parent))
                fail(ErrorCode::invalid_path, "Capture parent must already exist", parent);
            const auto output = parent / absolute.filename();

            std::vector<PreparedAssetInput> inputs;
            inputs.reserve(content.meshes().size());
            for (const auto& mesh : content.meshes())
                inputs.push_back({mesh.asset, mesh.payload});
            auto assets = take(PreparedAssets::create(inputs));
            auto loaded = take(LoadedPreparedScene::load(content.scene_payload(), assets));
            const auto& scene = loaded->scene();
            const auto stats = take(scene.stats());
            const auto camera = take(scene.find(content.request().camera));
            const auto rendered_revision = SceneRevision{stats.instance, stats.revision};
            auto origin = request.origin;
            if (!origin.world_anchor)
                origin.world_anchor = loaded->world_origin();
            auto snapshot = take(extract_render_scene(
                scene, *assets, {camera, rendered_revision, request.viewport, origin}));
            const auto extraction = snapshot.stats();
            auto packed = take(make_scene_viewport_request(snapshot, vertex_, pixel_));
            // This is a private read-only Core reconstruction of captured bytes,
            // not a second authoring owner. Both original and rendered identities
            // are reported; the fresh Core incarnation never impersonates source.
            take(validate());
            GraphicsConfig config;
            config.adapter_uuid = adapter_;
            config.validation = validation_;
            config.window = {.title = "Shark terminal capture",
                             .content = {request.viewport.width, request.viewport.height},
                             .visible = false};
            auto graphics = take(GraphicsDevice::create(config));
            auto rendered = graphics->render_scene_offscreen(packed);
            auto closed = graphics->close();
            // Even failed rendering passes through bounded close; unresolved GPU
            // work retains the existing device's fail-fast destruction policy.
            take(std::move(closed));
            auto frame = take(std::move(rendered));
            const auto viewport = take(graphics->scene_viewport_stats());
            const auto adapter = take(graphics->adapter());
            const auto diagnostics = take(graphics->diagnostics());
            const auto& pixels = frame.pixels;
            const auto pixel_count = static_cast<std::size_t>(request.viewport.width) *
                                     static_cast<std::size_t>(request.viewport.height);
            if (frame.scene_revision != rendered_revision || frame.origin != origin ||
                pixels.extent != WindowExtent{request.viewport.width, request.viewport.height} ||
                pixels.color_encoding != ReadbackColorEncoding::linear ||
                pixels.rgba8.size() != pixel_count * 4 || pixels.depth.size() != pixel_count ||
                pixels.submitted == 0 || pixels.completed != pixels.submitted ||
                pixels.created_resources != pixels.retired_resources || viewport.pending ||
                viewport.live_target_resources != 0 || viewport.generation == 0 ||
                viewport.retired_generations != viewport.generation ||
                viewport.rendered_frames != 1 || adapter.uuid != adapter_ || diagnostics.warnings ||
                diagnostics.errors || diagnostics.dropped)
                fail(ErrorCode::execution_failed,
                     "Capture readback or closed GPU state is inconsistent");
            std::vector<std::byte> depth;
            depth.reserve(pixel_count * 4);
            for (const float value : pixels.depth) {
                if (!std::isfinite(value) || value < 0.0F || value > 1.0F)
                    fail(ErrorCode::execution_failed, "Capture depth is not normalized and finite");
                const auto bits = std::bit_cast<std::uint32_t>(value);
                for (unsigned shift = 0; shift < 32; shift += 8)
                    depth.push_back(static_cast<std::byte>((bits >> shift) & 255U));
            }
            auto stage_path =
                parent / (".shark-capture-stage-" + take(generate_stable_id()).to_string());
            if (!fs::create_directory(stage_path))
                fail(ErrorCode::conflict, "Cannot create unique capture staging directory",
                     stage_path);
            Stage stage{std::move(stage_path)};
            Json artifacts;
            artifacts["color.rgba"] =
                write_bytes(stage.path / "color.rgba", std::as_bytes(std::span{pixels.rgba8}));
            artifacts["depth.f32"] = write_bytes(stage.path / "depth.f32", depth);
            Json report{
                {"format", "shark.authoring-capture"},
                {"schema_version", 1},
                {"published", true},
                {"succeeded", true},
                {"output", utf8(output)},
                {"durability", "not_confirmed"},
                {"project_id", content.project().value().to_string()},
                {"scene_id", content.scene().value().to_string()},
                {"camera_id", content.request().camera.value().to_string()},
                {"source_revision", revision(content.request().scene_revision)},
                {"rendered_revision", revision(rendered_revision)},
                {"source_dirty", content.dirty()},
                {"scene_digest", content.scene_digest().to_hex()},
                {"origin", origin_json(frame.origin)},
                {"width", request.viewport.width},
                {"height", request.viewport.height},
                {"draw_count", frame.draw_count},
                {"hidden", extraction.hidden},
                {"culled", extraction.culled},
                {"mesh_instances", extraction.mesh_instances},
                {"color_space", "linear"},
                {"color", "color.rgba"},
                {"depth", "depth.f32"},
                {"subpixel_precision_bits", pixels.subpixel_precision_bits},
                {"artifacts", std::move(artifacts)},
                {"adapter",
                 {{"uuid", adapter.uuid},
                  {"name", adapter.name},
                  {"driver_name", adapter.driver_name},
                  {"driver_info", adapter.driver_info},
                  {"driver_version", adapter.driver_version}}},
                {"validation", validation_ == GraphicsValidation::required ? "required" : "off"},
                {"diagnostics",
                 {{"warnings", diagnostics.warnings},
                  {"errors", diagnostics.errors},
                  {"dropped", diagnostics.dropped}}},
                {"closed", true},
                {"viewport",
                 {{"generation", viewport.generation},
                  {"retired_generations", viewport.retired_generations},
                  {"rendered_frames", viewport.rendered_frames},
                  {"live_target_resources", viewport.live_target_resources},
                  {"pending", viewport.pending}}},
                {"gpu",
                 {{"submitted", pixels.submitted},
                  {"completed", pixels.completed},
                  {"created_resources", pixels.created_resources},
                  {"retired_resources", pixels.retired_resources}}}};
            auto json = report.dump();
            if (json.size() > 64 * 1024)
                fail(ErrorCode::execution_failed, "Capture artifact description exceeds its bound");
            static_cast<void>(
                write_bytes(stage.path / "report.json", std::as_bytes(std::span{json})));
            AuthoringArtifactResult result{std::move(json), true, true};
            no_links(output);
            if (fs::exists(output))
                fail(ErrorCode::conflict, "Capture output appeared before publication", output);
            take(validate());
            fs::rename(stage.path, output);
            stage.published = true;
            return result;
        } catch (Failure& failure) {
            return std::move(failure.error);
        } catch (...) {
            return Error{ErrorCode::execution_failed,
                         "Cannot prepare or publish terminal capture",
                         {.operation = "authoring.capture"}};
        }
    }

  private:
    ShaderArtifact vertex_;
    ShaderArtifact pixel_;
    std::string adapter_;
    GraphicsValidation validation_;
};
} // namespace

Result<std::unique_ptr<tools::AuthoringCaptureProvider>>
make_authoring_capture(AuthoringCaptureConfig config) {
    try {
        if (!valid_graphics_uuid(config.adapter_uuid) ||
            (config.validation != "required" && config.validation != "off") ||
            config.vertex_shader.empty() || config.pixel_shader.empty())
            fail(ErrorCode::invalid_argument, "Capture configuration requires two shaders, an "
                                              "exact GPU UUID and required|off validation");
        if (!native_graphics_available())
            fail(ErrorCode::capability_unavailable,
                 "Native terminal capture is unavailable in this build");
        auto vertex = take(load_shader_artifact(shader_path(config.vertex_shader),
                                                ShaderStage::vertex, ShaderInterface::scene_unlit));
        auto pixel = take(load_shader_artifact(shader_path(config.pixel_shader), ShaderStage::pixel,
                                               ShaderInterface::scene_unlit));
        return std::unique_ptr<AuthoringCaptureProvider>{std::make_unique<Capture>(
            std::move(vertex), std::move(pixel), std::move(config.adapter_uuid),
            config.validation == "required" ? GraphicsValidation::required
                                            : GraphicsValidation::off)};
    } catch (Failure& failure) {
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Cannot configure terminal capture",
                     {.operation = "authoring.capture.configure"}};
    }
}

} // namespace shark::editor
