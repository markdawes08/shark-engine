#include "engine/client/src/graphics_native_test.hpp"
#include <shark/client/graphics.hpp>
#include <shark/client/scene_viewport.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <set>

#include "probes/graphics/report.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace shark;
using namespace shark::client;
using namespace shark::client::detail;
using shark::probe::Json;
using Array = Json::Array;
using Object = Json::Object;

class Failure final : public std::exception {
  public:
    explicit Failure(shark::Error error) : error_(std::move(error)) {}
    const char* what() const noexcept override {
        return error_.message.c_str();
    }
    const shark::Error& error() const noexcept {
        return error_;
    }

  private:
    shark::Error error_;
};

void require(bool condition, const char* message) {
    if (!condition) {
        throw Failure(
            {shark::ErrorCode::execution_failed, message, {.operation = "scene-render-smoke"}});
    }
}

template <typename T> T take(shark::Result<T> result) {
    if (!result) {
        throw Failure(std::move(result).error());
    }
    return std::move(result).value();
}

Object error_json(const shark::Error& error) {
    return {{"code", std::string(shark::error_code_name(error.code))},
            {"message", error.message},
            {"operation", error.context.operation}};
}

void emit(Object record) {
    Json{std::move(record)}.write(std::cout);
    std::cout << '\n' << std::flush;
    if (!std::cout) {
        throw std::runtime_error("Cannot write graphics-device smoke report");
    }
}

Array strings(const std::vector<std::string>& values) {
    Array result;
    for (const auto& value : values) {
        result.emplace_back(value);
    }
    return result;
}

template <typename T, std::size_t Size>
Object named_values(const std::array<const char*, Size>& names, const std::array<T, Size>& values) {
    Object result;
    for (std::size_t index = 0; index < Size; ++index) {
        result.emplace(names[index], values[index]);
    }
    return result;
}

const char* adapter_kind(AdapterKind kind) {
    switch (kind) {
    case AdapterKind::other:
        return "other";
    case AdapterKind::integrated:
        return "integrated";
    case AdapterKind::discrete:
        return "discrete";
    case AdapterKind::software:
        return "software";
    }
    return "unknown";
}

Object adapter_json(const GraphicsAdapter& adapter) {
    constexpr std::array feature_names{"vulkan_1_3",          "timeline_semaphore",
                                       "synchronization2",    "dynamic_rendering",
                                       "swapchain_extension", "graphics_compute_present_queue",
                                       "srgb_surface",        "fifo_present"};
    constexpr std::array format_names{"rgba8_srgb", "rgba8_unorm", "rgba16_float",
                                      "d32_float_depth_array"};
    constexpr std::array limit_names{
        "maxImageDimension2D",           "maxImageDimensionCube",
        "maxImageArrayLayers",           "maxColorAttachments",
        "maxBoundDescriptorSets",        "maxPerStageDescriptorSampledImages",
        "maxPerStageDescriptorSamplers", "maxUniformBufferRange",
        "maxStorageBufferRange",         "maxComputeWorkGroupInvocations"};
    return {{"uuid", adapter.uuid},
            {"name", adapter.name},
            {"driver_name", adapter.driver_name},
            {"driver_info", adapter.driver_info},
            {"vendor_id", adapter.vendor_id},
            {"device_id", adapter.device_id},
            {"driver_version", adapter.driver_version},
            {"api_version", adapter.api_version},
            {"queue_family", adapter.queue_family},
            {"kind", adapter_kind(adapter.kind)},
            {"features", named_values(feature_names, adapter.capabilities.features)},
            {"formats", named_values(format_names, adapter.capabilities.formats)},
            {"limits", named_values(limit_names, adapter.capabilities.limits)},
            {"missing_requirements", strings(missing_graphics_requirements(adapter))}};
}

Object diagnostics_json(const GraphicsDiagnostics& diagnostics) {
    return {{"warnings", diagnostics.warnings},
            {"errors", diagnostics.errors},
            {"dropped", diagnostics.dropped},
            {"messages", strings(diagnostics.messages)}};
}

bool clean(const GraphicsDiagnostics& diagnostics) {
    return diagnostics.warnings == 0 && diagnostics.errors == 0 && diagnostics.dropped == 0;
}

struct Options {
    std::string mode{"describe"};
    std::string fault;
    GraphicsConfig config;
    std::filesystem::path vertex, pixel, output;
};
Options parse(int argc, char* argv[]) {
    Options options;
    options.config.window = {
        .title = "Shark scene viewport", .content = {160, 120}, .visible = false};
    const auto path = [](std::string_view value) {
        return std::filesystem::path(std::u8string(value.begin(), value.end()));
    };
    const auto invalid = [] {
        throw Failure({ErrorCode::invalid_argument,
                       "Use --describe, --render --device UUID --vertex PATH --pixel PATH --output "
                       "FRESH_DIRECTORY --validation required|off, or --native-fault NAME --device "
                       "UUID --vertex PATH --pixel PATH",
                       {.operation = "scene-render-smoke.arguments"}});
    };
    if (argc == 2 &&
        (std::string_view(argv[1]) == "--describe" || std::string_view(argv[1]) == "--help"))
        return options;
    if (argc == 9 && std::string_view(argv[1]) == "--native-fault" &&
        std::string_view(argv[3]) == "--device" && std::string_view(argv[5]) == "--vertex" &&
        std::string_view(argv[7]) == "--pixel") {
        options.mode = "fault";
        options.fault = argv[2];
        options.config.adapter_uuid = argv[4];
        options.vertex = path(argv[6]);
        options.pixel = path(argv[8]);
        if ((options.fault != "partial-creation" && options.fault != "submission-timeout" &&
             options.fault != "recording-error") ||
            !valid_graphics_uuid(options.config.adapter_uuid) || options.vertex.empty() ||
            options.pixel.empty())
            invalid();
        return options;
    }
    if (argc != 12 || std::string_view(argv[1]) != "--render")
        invalid();
    options.mode = "render";
    std::set<std::string_view> seen;
    for (int index = 2; index < argc; index += 2) {
        const std::string_view key{argv[index]}, value{argv[index + 1]};
        if (value.empty() || !seen.insert(key).second)
            invalid();
        if (key == "--device" && valid_graphics_uuid(value))
            options.config.adapter_uuid = value;
        else if (key == "--vertex")
            options.vertex = path(value);
        else if (key == "--pixel")
            options.pixel = path(value);
        else if (key == "--output")
            options.output = path(value);
        else if (key == "--validation" && (value == "required" || value == "off"))
            options.config.validation =
                value == "required" ? GraphicsValidation::required : GraphicsValidation::off;
        else
            invalid();
    }
    if (options.config.adapter_uuid.empty() || options.vertex.empty() || options.pixel.empty() ||
        options.output.empty() || !seen.contains("--validation"))
        invalid();
    return options;
}
template <typename Id> Id id(std::uint32_t value) {
    std::string text = "00000000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[value & 15U];
        value >>= 4U;
    }
    return Id{take(StableId::parse(text))};
}
struct Fixture {
    std::shared_ptr<const PreparedAssets> assets;
    std::unique_ptr<Scene> scene;
    EntityHandle camera;
    std::vector<EntityHandle> meshes;
};
PreparedPrimitive quad(std::array<float, 4> color, bool reversed = false, bool double_sided = false,
                       bool gradient = false) {
    PreparedPrimitive primitive;
    constexpr std::array<std::array<float, 3>, 4> positions{
        {{-.5F, -.5F, 0}, {.5F, -.5F, 0}, {.5F, .5F, 0}, {-.5F, .5F, 0}}};
    constexpr std::array<std::array<float, 4>, 4> colors{
        {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}}};
    for (std::size_t index = 0; index < 4; ++index) {
        PreparedVertex vertex;
        vertex.position = positions[index];
        vertex.normal = {0, 0, 1};
        vertex.color = gradient ? colors[index] : color;
        primitive.vertices.push_back(vertex);
    }
    primitive.indices = reversed ? std::vector<std::uint32_t>{0, 2, 1, 0, 3, 2}
                                 : std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3};
    primitive.material.double_sided = double_sided;
    if (gradient)
        primitive.material.base_color = {.5F, 1, .25F, 1};
    return primitive;
}
Fixture fixture() {
    const auto asset_id = id<AssetId>(100);
    PreparedMeshAsset asset{asset_id,
                            {{{quad({1, 1, 1, 1}, false, false, true)}},
                             {{quad({.125F, .25F, 1, 1})}},
                             {{quad({1, 1, 0, 1}, true)}},
                             {{quad({0, 1, 1, .5F}, true, true)}}}};
    const auto bytes = take(encode_prepared_mesh(asset));
    const std::array inputs{PreparedAssetInput{asset_id, bytes}};
    Fixture output{take(PreparedAssets::create(inputs)),
                   take(Scene::create(AuthoredSceneScope{id<SceneId>(1)})),
                   {},
                   {}};
    EntitySeed camera{id<AuthoredEntityId>(1)};
    camera.camera = Camera{};
    output.camera = take(output.scene->create_entity(camera));
    const auto add = [&](std::uint32_t identity, std::uint32_t mesh, math::Vec3 position,
                         math::Vec3 scale, bool visible = true) {
        EntitySeed seed{id<AuthoredEntityId>(identity)};
        seed.transform.translation = position;
        seed.transform.scale = scale;
        seed.mesh_renderer = MeshRenderer{asset_id, mesh, visible};
        output.meshes.push_back(take(output.scene->create_entity(seed)));
    };
    add(2, 0, {-.85, 0, -2}, {1.2, 1.2, 1}); // Near gradient, submitted before the blue occluder.
    add(3, 1, {-.85, 0, -4}, {1.2, 1.2, 1}); // Later far draw must not overwrite nearer depth.
    add(4, 0, {.85, 0, -2},
        {-1.2, 1.2, 1});                    // Shared mesh, odd reflection must remain front-facing.
    add(5, 2, {0, 1.25, -3}, {.8, .5, 1});  // Reversed, single-sided: reject.
    add(6, 3, {0, -1.25, -3}, {.8, .5, 1}); // Reversed, double-sided: admit without blending.
    add(7, 1, {0, 0, -1.5}, {3, 3, 1}, false); // Hidden, otherwise obscures the fixture.
    add(8, 1, {100, 0, -3}, {1, 1, 1});        // CPU frustum culling, no draw upload.
    return output;
}
SceneViewportRequest request(Fixture& fixture, std::size_t ordinal, const ShaderArtifact& vertex,
                             const ShaderArtifact& pixel) {
    Camera camera;
    camera.projection =
        ordinal == 1 ? CameraProjection::perspective : CameraProjection::orthographic;
    camera.vertical_fov_radians = std::numbers::pi / 2;
    camera.orthographic_height = 4;
    camera.near_clip = 1;
    camera.far_clip = 9;
    take(fixture.scene->set_camera(fixture.camera, camera));
    if (ordinal == 4)
        for (const auto handle : fixture.meshes)
            take(fixture.scene->set_mesh_renderer(handle, std::nullopt));
    const auto stats = take(fixture.scene->stats());
    RenderExtractionOptions options{fixture.camera, {stats.instance, stats.revision}};
    options.viewport = ordinal == 2 ? RenderViewport{128, 96} : RenderViewport{96, 80};
    const auto extracted = take(extract_render_scene(*fixture.scene, *fixture.assets, options));
    require(extracted.stats().visible == (ordinal == 4 ? 0U : 5U) &&
                extracted.stats().hidden == (ordinal == 4 ? 0U : 1U) &&
                extracted.stats().culled == (ordinal == 4 ? 0U : 1U),
            "Unexpected extraction visibility accounting");
    return take(make_scene_viewport_request(extracted, vertex, pixel));
}
Object extent_json(WindowExtent extent) {
    return {{"width", extent.width}, {"height", extent.height}};
}
Object viewport_json(const SceneViewportStats& value) {
    return {{"extent", extent_json(value.extent)},
            {"generation", value.generation},
            {"retired_generations", value.retired_generations},
            {"rendered_frames", value.rendered_frames},
            {"live_target_resources", value.live_target_resources},
            {"pending", value.pending}};
}
void write_file(const std::filesystem::path& path, const void* data, std::size_t size) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    stream.close();
    require(static_cast<bool>(stream), "Cannot write scene readback artifact");
}
Object identity(const Options& options) {
    return {{"kind", "identity"},
            {"schema_version", 1U},
            {"assignment_13", "NOT_RUN"},
            {"mode", options.mode},
            {"dependency_lock_sha256", SHARK_GRAPHICS_LOCK_SHA256},
            {"source_sha256", SHARK_GRAPHICS_SOURCE_SHA256},
            {"compiler", SHARK_GRAPHICS_COMPILER},
            {"selection", options.config.adapter_uuid},
            {"validation",
             options.config.validation == GraphicsValidation::required ? "required" : "off"},
            {"fault", options.fault}};
}
bool expected_termination = false;
const std::uint32_t* teardown_count = nullptr;
[[noreturn]] void observe_termination() noexcept {
    std::printf("{\"kind\":\"terminated\",\"expected\":%s,\"teardown_calls\":%u}\n",
                expected_termination ? "true" : "false",
                expected_termination && teardown_count ? *teardown_count : 999U);
    std::fflush(stdout);
    std::_Exit(expected_termination ? 86 : 87);
}
Object resources_json(const GraphicsResourceSnapshot& value) {
    return {{"target", value.scene_target_resources},      {"frame", value.scene_frame_resources},
            {"submitted", value.scene_submissions},        {"recording", value.scene_recordings},
            {"swapchains", value.live_swapchains},         {"offscreen", value.offscreen_resources},
            {"queue_workers", value.pending_queue_workers}};
}
int fault_run(const Options& options, const SceneViewportRequest& request) {
    NativeGraphicsTestControl control;
    control.fault =
        options.fault == "partial-creation"     ? NativeGraphicsFault::scene_partial_creation
        : options.fault == "submission-timeout" ? NativeGraphicsFault::scene_submission_timeout
                                                : NativeGraphicsFault::scene_recording_error;
    auto backend = make_fault_graphics_backend(control);
    auto* observed = backend.get();
    auto owner = take(create_graphics(options.config, std::move(backend)));
    auto attempted = request;
    if (options.fault == "partial-creation") {
        const auto warmup = take(owner->render_scene_offscreen(request));
        require(warmup.pixels.submitted == warmup.pixels.completed &&
                    warmup.viewport.generation == 1,
                "Resize fault warmup did not complete");
        auto invalid = request;
        invalid.extent.width = 0;
        require(!owner->render_scene_offscreen(invalid) &&
                    take(owner->scene_viewport_stats()).generation == 1,
                "Invalid extent changed the live viewport");
        attempted.extent = {128, 96};
    }
    emit({{"kind", "ready"},
          {"adapter", adapter_json(take(owner->adapter()))},
          {"resources", resources_json(observed->presentation_resources())},
          {"viewport", viewport_json(take(owner->scene_viewport_stats()))},
          {"diagnostics", diagnostics_json(take(owner->diagnostics()))}});
    control.armed = true;
    const auto failed = owner->render_scene_offscreen(attempted);
    require(!failed && control.fired, "Scene fault did not fire");
    if (options.fault == "partial-creation") {
        const auto retained = take(owner->scene_viewport_stats());
        require(retained.extent == request.extent && retained.generation == 1 &&
                    retained.retired_generations == 0 && retained.live_target_resources == 5 &&
                    !retained.pending,
                "Failed viewport resize replaced the last-valid target");
    }
    emit({{"kind", "fault"},
          {"fired", control.fired},
          {"error", error_json(failed.error())},
          {"resources", resources_json(observed->presentation_resources())},
          {"diagnostics", diagnostics_json(take(owner->diagnostics()))},
          {"teardown_calls", control.teardown_calls}});
    if (options.fault == "recording-error") {
        const auto closed = owner->close();
        require(!closed && control.teardown_calls == 0, "Unresolved scene recording was released");
        emit({{"kind", "retained"},
              {"error", error_json(closed.error())},
              {"resources", resources_json(observed->presentation_resources())},
              {"diagnostics", diagnostics_json(take(owner->diagnostics()))},
              {"teardown_calls", control.teardown_calls}});
        expected_termination = true;
        teardown_count = &control.teardown_calls;
        owner.reset();
        return 88;
    }
    if (options.fault == "submission-timeout") {
        const auto repeated = owner->render_scene_offscreen(request);
        const auto presented = owner->present();
        require(!repeated && repeated.error().code == ErrorCode::conflict && !presented &&
                    presented.error().code == ErrorCode::conflict,
                "Pending scene admitted new GPU work");
    }
    take(owner->drain());
    require(!take(owner->scene_viewport_stats()).pending, "Drain retained scene submission");
    emit({{"kind", "retired"},
          {"resources", resources_json(observed->presentation_resources())},
          {"diagnostics", diagnostics_json(take(owner->diagnostics()))},
          {"teardown_calls", control.teardown_calls}});
    const auto recovered = take(owner->render_scene_offscreen(request));
    const auto offset = std::size_t{65 * 96 + 48};
    const auto& color = recovered.pixels.rgba8;
    require(color[offset * 4] == 0 && color[offset * 4 + 1] == 255 &&
                color[offset * 4 + 2] == 255 &&
                std::abs(static_cast<int>(color[offset * 4 + 3]) - 128) <= 1 &&
                std::abs(recovered.pixels.depth[offset] - .25F) <= 1e-5F,
            "Recovered scene draw lacks known cyan color/depth");
    emit(
        {{"kind", "recovered"},
         {"submitted", recovered.pixels.submitted},
         {"completed", recovered.pixels.completed},
         {"created_resources", recovered.pixels.created_resources},
         {"retired_resources", recovered.pixels.retired_resources},
         {"viewport", viewport_json(recovered.viewport)},
         {"center_rgba8", Array{0U, 255U, 255U, static_cast<std::uint32_t>(color[offset * 4 + 3])}},
         {"center_depth", static_cast<double>(recovered.pixels.depth[offset])},
         {"diagnostics", diagnostics_json(take(owner->diagnostics()))}});
    take(owner->close());
    emit({{"kind", "closed"},
          {"resources", resources_json(observed->presentation_resources())},
          {"viewport", viewport_json(take(owner->scene_viewport_stats()))},
          {"diagnostics", diagnostics_json(take(owner->diagnostics()))},
          {"teardown_calls", control.teardown_calls}});
    require(control.teardown_calls == 5, "Scene-only device did not close all owners");
    return 1;
}
int run(const Options& options) {
    emit(identity(options));
    if (options.mode == "describe") {
        emit({{"kind", "description"},
              {"status", "PASS"},
              {"qualification", "NOT_RUN"},
              {"device_accessed", false},
              {"cases", 5U},
              {"faults", Array{"partial-creation", "submission-timeout", "recording-error"}},
              {"native_graphics_available", native_graphics_available()}});
        return 0;
    }
    auto source = fixture();
    const auto vertex = take(
        load_shader_artifact(options.vertex, ShaderStage::vertex, ShaderInterface::scene_unlit));
    const auto pixel =
        take(load_shader_artifact(options.pixel, ShaderStage::pixel, ShaderInterface::scene_unlit));
    if (options.mode == "fault")
        return fault_run(options, request(source, 0, vertex, pixel));
    require(!std::filesystem::exists(options.output), "Readback directory must be fresh");
    std::filesystem::create_directories(options.output);
    auto device = take(GraphicsDevice::create(options.config));
    emit({{"kind", "adapter"}, {"adapter", adapter_json(take(device->adapter()))}});
    std::uint64_t previous = 0;
    constexpr std::array names{"ortho", "perspective", "resized", "restored", "empty"};
    constexpr std::array<std::uint64_t, 5> generations{1, 1, 2, 3, 3};
    for (std::size_t ordinal = 0; ordinal < names.size(); ++ordinal) {
        const auto packed = request(source, ordinal, vertex, pixel);
        const auto result = take(device->render_scene_offscreen(packed));
        const auto& pixels = result.pixels;
        require(pixels.extent == packed.extent && pixels.submitted > previous &&
                    pixels.completed == pixels.submitted &&
                    pixels.created_resources == pixels.retired_resources &&
                    result.scene_revision == packed.scene_revision &&
                    result.origin == packed.origin,
                "Readback lost its frame identity or completed resource ownership");
        require(result.viewport.generation == generations[ordinal] &&
                    result.viewport.retired_generations == generations[ordinal] - 1 &&
                    result.viewport.rendered_frames == ordinal + 1 &&
                    result.viewport.live_target_resources == 5 && !result.viewport.pending,
                "Persistent viewport did not reuse or retire extent generations");
        previous = pixels.submitted;
        const std::string name = names[ordinal];
        write_file(options.output / (name + ".rgba8"), pixels.rgba8.data(), pixels.rgba8.size());
        write_file(options.output / (name + ".d32f"), pixels.depth.data(),
                   pixels.depth.size() * sizeof(float));
        emit({{"kind", "case"},
              {"name", name},
              {"extent", extent_json(pixels.extent)},
              {"draw_count", result.draw_count},
              {"submitted", pixels.submitted},
              {"completed", pixels.completed},
              {"created_resources", pixels.created_resources},
              {"retired_resources", pixels.retired_resources},
              {"subpixel_precision_bits", pixels.subpixel_precision_bits},
              {"viewport", viewport_json(result.viewport)},
              {"color_file", name + ".rgba8"},
              {"depth_file", name + ".d32f"}});
    }
    take(device->drain());
    take(device->close());
    const auto diagnostics = take(device->diagnostics());
    require(clean(diagnostics), "Graphics validation reported diagnostics");
    const auto closed = take(device->scene_viewport_stats());
    require(closed.live_target_resources == 0 && closed.retired_generations == 3 && !closed.pending,
            "Final viewport owner did not retire");
    const bool validated = options.config.validation == GraphicsValidation::required;
    emit({{"kind", "complete"},
          {"status", validated ? "PASS" : "NOT_RUN"},
          {"qualification", validated ? "PASS" : "NOT_RUN"},
          {"assignment_13", "NOT_RUN"},
          {"cases", 5U},
          {"diagnostics", diagnostics_json(diagnostics)},
          {"viewport", viewport_json(closed)}});
    return 0;
}
} // namespace
int main(int argc, char* argv[]) {
    std::set_terminate(observe_termination);
    try {
        return run(parse(argc, argv));
    } catch (const Failure& failure) {
        emit({{"kind", "failure"}, {"status", "FAIL"}, {"error", error_json(failure.error())}});
    } catch (const std::exception& error) {
        emit({{"kind", "failure"}, {"status", "FAIL"}, {"message", error.what()}});
    }
    return 1;
}
