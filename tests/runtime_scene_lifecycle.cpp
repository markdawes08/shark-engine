#include <shark/client/scene_launch.hpp>
#include <shark/time.hpp>

#include "probes/graphics/report.hpp"
#if defined(SHARK_RUNTIME_SCENE_NATIVE)
#include "graphics_native_test.hpp"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace shark;
using namespace shark::client;
using shark::probe::Json;
using Object = Json::Object;
using Array = Json::Array;

class Failure final : public std::exception {
  public:
    explicit Failure(Error error) : error_(std::move(error)) {}
    const char* what() const noexcept override {
        return error_.message.c_str();
    }
    const Error& error() const noexcept {
        return error_;
    }

  private:
    Error error_;
};

template <typename T> T take(Result<T> result) {
    if (!result)
        throw Failure{std::move(result).error()};
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result)
        throw Failure{std::move(result).error()};
}

std::uint32_t checks = 0;
void require(bool value, const char* message) {
    ++checks;
    if (!value)
        throw Failure{{ErrorCode::execution_failed, message, {"runtime.scene.lifecycle"}}};
}

void emit(Object value) {
    Json{std::move(value)}.write(std::cout);
    std::cout << '\n' << std::flush;
    if (!std::cout)
        throw std::runtime_error{"Cannot write Runtime scene lifetime result"};
}

RenderScene extract(const LoadedPreparedScene& loaded, AuthoredEntityId camera,
                    WindowExtent extent) {
    const auto stats = take(loaded.scene().stats());
    return take(extract_render_scene(loaded.scene(), *loaded.assets(),
                                     {take(loaded.scene().find(camera)),
                                      {stats.instance, stats.revision},
                                      {extent.width, extent.height},
                                      {{0.0, 0.0, 0.0}, 1, loaded.world_origin()}}));
}

OffscreenReadback present(GraphicsDevice& graphics, const SceneViewportRequest& request,
                          bool install = true) {
    bool installed = !install;
    SteadyMonotonicClock clock;
    for (std::uint32_t attempt = 0; attempt < 120; ++attempt) {
        const auto frame = take(graphics.poll_window());
        require(!frame.state.close_requested && !frame.diagnostics.window_error &&
                    !frame.diagnostics.input_error &&
                    frame.diagnostics.invalid_window_events == 0 &&
                    frame.diagnostics.rejected_input_events == 0,
                "Unexpected window event or close during scene lifecycle");
        if (frame.state.framebuffer == request.extent && !frame.state.iconified) {
            if (!installed) {
                take(graphics.set_presentation_scene(request, true));
                installed = true;
            }
            const auto result = take(graphics.present());
            require(result.status != PresentationStatus::closed,
                    "Presentation closed before the lifecycle phase completed");
            if (result.status == PresentationStatus::presented) {
                auto readback = take(graphics.capture_presented());
                require(readback.extent == request.extent &&
                            readback.color_encoding == ReadbackColorEncoding::srgb &&
                            readback.submitted != 0 && readback.completed >= readback.submitted,
                        "Lifecycle capture did not come from the completed sRGB presentation");
                require(readback.depth.size() == static_cast<std::size_t>(request.extent.width) *
                                                     request.extent.height &&
                            readback.rgba8.size() == readback.depth.size() * 4,
                        "Lifecycle capture has incomplete color or depth bytes");
                return readback;
            }
        }
        // Return to native events between bounded presentation attempts, using
        // the existing monotonic wait rather than a separate fixture scheduler.
        take(clock.wait_for(std::chrono::milliseconds{10}));
    }
    throw Failure{{ErrorCode::execution_failed,
                   "Scene lifecycle presentation did not complete within 120 event passes",
                   {"runtime.scene.lifecycle.present"}}};
}

std::uint64_t foreground(const OffscreenReadback& pixels) {
    require(
        std::all_of(pixels.depth.begin(), pixels.depth.end(),
                    [](float value) { return std::isfinite(value) && value >= 0 && value <= 1; }),
        "Scene lifecycle captured invalid depth");
    return static_cast<std::uint64_t>(std::count_if(pixels.depth.begin(), pixels.depth.end(),
                                                    [](float value) { return value < 1; }));
}

void same_image(const OffscreenReadback& first, const OffscreenReadback& next) {
    require(first.extent == next.extent && first.rgba8 == next.rgba8 && first.depth == next.depth,
            "Same-device reload or restoration changed the completed scene image");
}

void run(const SceneLaunchOptions& options) {
    require(options.camera.has_value() && !options.adapter_uuid.empty() &&
                options.validation == GraphicsValidation::required,
            "Scene lifecycle requires an explicit camera, device and required validation");
    require(!options.frames && !options.duration && options.capture_directory.empty(),
            "Scene lifecycle owns its bounded phases; launch capture/time options do not apply");
    auto loaded = take(load_scene_content(options.content));
    const auto vertex = take(load_shader_artifact(options.vertex_shader, ShaderStage::vertex,
                                                  ShaderInterface::scene_unlit));
    const auto pixel = take(load_shader_artifact(options.pixel_shader, ShaderStage::pixel,
                                                 ShaderInterface::scene_unlit));
    constexpr WindowExtent original_extent{160, 120};
    constexpr WindowExtent resized_extent{200, 144};
    auto retained = extract(*loaded, *options.camera, original_extent);
    require(retained.records().size() == 1,
            "Lifecycle input must have exactly one visible mesh entity and a separate camera");
    const auto entity = take(loaded->scene().resolve(retained.records()[0].entity));
    const auto camera = take(loaded->scene().find(*options.camera));
    require(entity != camera, "Lifecycle fixture camera must survive the visible mesh deletion");
    const auto incarnation = take(loaded->scene().stats()).instance;

    GraphicsConfig config;
    config.adapter_uuid = options.adapter_uuid;
    config.validation = GraphicsValidation::required;
    config.window = {"Shark Runtime scene lifetime", original_extent, true, true};
#if defined(SHARK_RUNTIME_SCENE_NATIVE)
    detail::NativeGraphicsTestControl control;
    auto graphics =
        take(detail::create_graphics(config, detail::make_fault_graphics_backend(control)));
#else
    auto graphics = take(GraphicsDevice::create(config));
#endif
    const auto adapter = take(graphics->adapter());
    require(adapter.uuid == options.adapter_uuid, "Lifecycle selected a different graphics device");
    Array phases;
    const auto render = [&](const char* phase, const RenderScene& scene) {
        const auto request = take(make_scene_viewport_request(scene, vertex, pixel));
        auto pixels = present(*graphics, request);
        const auto occupied = foreground(pixels);
        phases.emplace_back(Object{{"phase", phase},
                                   {"width", pixels.extent.width},
                                   {"height", pixels.extent.height},
                                   {"draws", static_cast<std::uint64_t>(request.draws.size())},
                                   {"foreground_pixels", occupied}});
        return pixels;
    };
    const auto baseline = render("baseline", retained);
    require(foreground(baseline) > 32, "Lifecycle baseline did not show its mesh");

#if defined(SHARK_RUNTIME_SCENE_NATIVE)
    // The empty candidate would visibly clear the old mesh if it were adopted.
    // Fail after a real candidate depth attachment is acquired, then present the
    // old owned request without reinstalling it; recovery cannot mask adoption.
    const auto prior_request = take(make_scene_viewport_request(retained, vertex, pixel));
    auto candidate = prior_request;
    candidate.draws.clear();
    candidate.indices.clear();
    candidate.vertices.clear();
    control.fault = detail::NativeGraphicsFault::window_draw_partial_creation;
    control.armed = true;
    const auto rejected = graphics->set_presentation_scene(candidate, true);
    require(!rejected && rejected.error().code == ErrorCode::execution_failed && control.fired,
            "Partial scene candidate creation did not reject through the native fault control");
    const auto recovered = present(*graphics, prior_request, false);
    same_image(baseline, recovered);
    phases.emplace_back(Object{{"phase", "replacement-rejected"},
                               {"width", recovered.extent.width},
                               {"height", recovered.extent.height},
                               {"draws", static_cast<std::uint64_t>(prior_request.draws.size())},
                               {"foreground_pixels", foreground(recovered)}});
#endif

    take(loaded->scene().destroy_entity(entity));
    require(!loaded->scene().entity_state(entity), "Destroyed scene entity handle remained valid");
    const auto deleted_scene = extract(*loaded, *options.camera, original_extent);
    require(deleted_scene.records().empty(), "Deleted visible entity remained in extraction");
    const auto empty = render("destroyed", deleted_scene);
    require(foreground(empty) == 0, "Destroyed entity left visible depth in the presentation");
    bool clear = true;
    for (std::size_t index = 0; index < empty.depth.size(); ++index)
        clear = clear && empty.rgba8[index * 4] == 0 && empty.rgba8[index * 4 + 1] == 0 &&
                empty.rgba8[index * 4 + 2] == 0 && empty.rgba8[index * 4 + 3] == 255;
    require(clear, "Destroyed entity left color in the presentation");

    auto missing = options.content;
    missing.scene += ".missing-lifecycle-candidate";
    require(!std::filesystem::exists(missing.scene), "Missing-candidate path unexpectedly exists");
    const auto before_failed = take(loaded->scene().stats());
    require(!load_scene_content(missing), "Missing scene candidate unexpectedly loaded");
    const auto after_failed = take(loaded->scene().stats());
    require(after_failed.instance == before_failed.instance &&
                after_failed.revision == before_failed.revision &&
                after_failed.live_entities == before_failed.live_entities &&
                loaded->scene().entity_state(camera).has_value(),
            "Failed scene candidate altered its live predecessor");

    auto replacement = take(load_scene_content(options.content));
    require(take(replacement->scene().stats()).instance != incarnation &&
                !replacement->scene().entity_state(entity) &&
                !replacement->scene().entity_state(camera),
            "Reload reused old scene incarnation or handles");
    loaded = std::move(replacement);
    const auto reloaded = render("reloaded", extract(*loaded, *options.camera, original_extent));
    same_image(baseline, reloaded);

    take(graphics->resize(resized_extent));
    const auto resized = render("resized", extract(*loaded, *options.camera, resized_extent));
    require(foreground(resized) > 32, "Resized scene lost its visible geometry");
    take(graphics->resize(original_extent));
    const auto restored = render("restored", extract(*loaded, *options.camera, original_extent));
    same_image(baseline, restored);

    // The original Scene is gone. Rendering this explicitly retained historical
    // value must still own its immutable asset; it is not a claim of currentness.
    loaded.reset();
    const auto historical = render("retained-snapshot", retained);
    same_image(baseline, historical);
    take(graphics->close());
    const auto stats = take(graphics->presentation_stats());
    const auto diagnostics = take(graphics->diagnostics());
    const auto viewport = take(graphics->scene_viewport_stats());
    require(stats.closed && !stats.active && !stats.faulted && stats.in_flight == 0 &&
                stats.completed == stats.submitted && stats.presented == 7 &&
                stats.generation == stats.retired_generations && stats.generation >= 3,
            "Scene lifecycle did not retire every presentation owner");
    require(diagnostics.warnings == 0 && diagnostics.errors == 0 && diagnostics.dropped == 0,
            "Scene lifecycle reported native validation diagnostics");
    require(!viewport.pending && viewport.live_target_resources == 0,
            "Scene lifecycle retained an unexpected offscreen target");
    emit({{"event", "runtime-scene-lifecycle-complete"},
          {"status", "PASS"},
          {"adapter_uuid", adapter.uuid},
          {"adapter_name", adapter.name},
          {"driver_name", adapter.driver_name},
          {"driver_info", adapter.driver_info},
          {"driver_version", adapter.driver_version},
          {"validation", "required"},
          {"replacement_rejection", Object{{"fault", "window_draw_partial_creation"},
                                           {"fired", true},
                                           {"old_frame_preserved", true}}},
          {"checks", checks},
          {"phases", std::move(phases)},
          {"submitted", stats.submitted},
          {"completed", stats.completed},
          {"presented", stats.presented},
          {"generations", stats.generation},
          {"retired_generations", stats.retired_generations},
          {"in_flight", stats.in_flight},
          {"closed", stats.closed},
          {"warnings", diagnostics.warnings},
          {"errors", diagnostics.errors},
          {"dropped", diagnostics.dropped}});
}
} // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc == 2 && std::string_view{argv[1]} == "--describe") {
            emit({{"fixture", "runtime-scene-lifecycle"},
                  {"phases", std::uint32_t{7}},
                  {"validation", "required"},
                  {"gpu_initialized", false}});
            return 0;
        }
        require(argc > 1 && static_cast<std::size_t>(argc - 1) <= scene_launch_maximum_arguments,
                "Expected bounded Runtime scene launch arguments");
        std::array<std::string_view, scene_launch_maximum_arguments> arguments{};
        for (int index = 1; index < argc; ++index)
            arguments[static_cast<std::size_t>(index - 1)] = argv[index];
        run(take(
            parse_scene_arguments(std::span{arguments}.first(static_cast<std::size_t>(argc - 1)))));
        return 0;
    } catch (const Failure& failure) {
        emit({{"event", "runtime-scene-lifecycle-failed"},
              {"status", "FAIL"},
              {"checks", checks},
              {"code", std::string{error_code_name(failure.error().code)}},
              {"operation", failure.error().context.operation},
              {"message", failure.error().message}});
    } catch (const std::exception& error) {
        emit({{"event", "runtime-scene-lifecycle-failed"},
              {"status", "FAIL"},
              {"checks", checks},
              {"message", error.what()}});
    }
    return 1;
}
