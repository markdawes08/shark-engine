#include <shark/client/graphics.hpp>
#include <shark/time.hpp>

#include "probes/graphics/report.hpp"

#include <array>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

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
void require(bool value, const char* message) {
    if (!value)
        throw Failure{{ErrorCode::execution_failed, message, {"gui.smoke"}}};
}

Object stats_json(const PresentationStats& stats) {
    return {{"submitted", stats.submitted},   {"completed", stats.completed},
            {"presented", stats.presented},   {"in_flight", stats.in_flight},
            {"generation", stats.generation}, {"retired_generations", stats.retired_generations},
            {"closed", stats.closed},         {"faulted", stats.faulted}};
}

void add_quad(GuiFrame& frame, std::array<float, 4> bounds, std::array<float, 4> color,
              std::array<std::int32_t, 4> clip, GuiTexture texture = GuiTexture::atlas) {
    const auto vertex = static_cast<std::uint32_t>(frame.vertices.size());
    const auto first = static_cast<std::uint32_t>(frame.indices.size());
    const auto [left, top, right, bottom] = bounds;
    const auto low = texture == GuiTexture::atlas ? 0.25F : 0.0F;
    const auto high = texture == GuiTexture::atlas ? 0.75F : 1.0F;
    frame.vertices.push_back({{left, top}, {low, low}, color});
    frame.vertices.push_back({{right, top}, {high, low}, color});
    frame.vertices.push_back({{right, bottom}, {high, high}, color});
    frame.vertices.push_back({{left, bottom}, {low, high}, color});
    for (const std::uint32_t index : {0U, 1U, 2U, 0U, 2U, 3U})
        frame.indices.push_back(vertex + index);
    frame.draws.push_back({first, 6, clip, texture});
}

GuiFrame fixture(WindowExtent extent, bool changed) {
    GuiFrame frame;
    frame.extent = extent;
    const auto width = static_cast<std::int32_t>(extent.width);
    const auto height = static_cast<std::int32_t>(extent.height);
    add_quad(frame, {20.0F, 20.0F, changed ? 60.0F : 100.0F, 90.0F},
             changed ? std::array{0.0F, 0.0F, 1.0F, 1.0F} : std::array{1.0F, 0.0F, 0.0F, 1.0F},
             {0, 0, width, height});
    add_quad(frame, {60.0F, 40.0F, 140.0F, 110.0F}, {0.0F, 1.0F, 0.0F, 0.5F}, {70, 50, 120, 100});
    return frame;
}

GuiFrame viewport_fixture(WindowExtent extent, const ShaderArtifact& vertex_shader,
                          const ShaderArtifact& pixel_shader, unsigned phase) {
    GuiFrame frame;
    frame.extent = extent;
    if (phase != 5) {
        SceneViewportRequest scene;
        scene.extent = phase >= 3 ? WindowExtent{144, 96} : WindowExtent{96, 64};
        // Every camera/content phase intentionally retains the same metadata.
        // The GUI GPU cache must compare packed content, not revision alone.
        scene.scene_revision = {600, 9};
        scene.vertex_shader = vertex_shader;
        scene.pixel_shader = pixel_shader;
        const auto quad = [&](float left, float right, float depth, std::array<float, 4> color,
                              float camera_offset) {
            const auto first_vertex = static_cast<std::uint32_t>(scene.vertices.size());
            const auto first_index = static_cast<std::uint32_t>(scene.indices.size());
            scene.vertices.push_back({{left, -1, depth}, color});
            scene.vertices.push_back({{right, -1, depth}, color});
            scene.vertices.push_back({{right, 1, depth}, color});
            scene.vertices.push_back({{left, 1, depth}, color});
            for (const std::uint32_t index : {0U, 1U, 2U, 0U, 2U, 3U})
                scene.indices.push_back(first_vertex + index);
            SceneViewportDraw draw;
            draw.first_index = first_index;
            draw.index_count = 6;
            draw.transform = {1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, camera_offset, 0, 0, 1};
            draw.double_sided = true;
            scene.draws.push_back(draw);
        };
        // Near red geometry is submitted before the full far blue/green quad.
        // Retaining red therefore also checks shared-depth rendering in the target.
        // Opaque scene profile ignores source alpha when composited into UI.
        quad(-1, 0, 0.25F, {1, 0, 0, 0}, phase >= 1 ? 0.5F : 0.0F);
        quad(-2, 2, 0.75F,
             phase >= 2 ? std::array{0.0F, 1.0F, 0.0F, 1.0F} : std::array{0.0F, 0.0F, 1.0F, 1.0F},
             phase >= 1 ? 0.5F : 0.0F);
        frame.viewport = std::move(scene);
        add_quad(frame, {20, 20, 140, 100}, {1, 1, 1, 1},
                 {0, 0, static_cast<std::int32_t>(extent.width),
                  static_cast<std::int32_t>(extent.height)},
                 GuiTexture::scene_viewport);
    }
    add_quad(frame, {80, 10, 160, 70}, {1, 1, 1, 0.5F}, {90, 30, 130, 60});
    return frame;
}

OffscreenReadback present(GraphicsDevice& graphics, const GuiFrame& request) {
    bool installed = false;
    SteadyMonotonicClock clock;
    for (std::uint32_t attempt = 0; attempt < 120; ++attempt) {
        const auto window = take(graphics.poll_window());
        require(!window.state.close_requested && !window.diagnostics.window_error &&
                    !window.diagnostics.input_error &&
                    window.diagnostics.invalid_window_events == 0 &&
                    window.diagnostics.rejected_input_events == 0,
                "Unexpected window close or diagnostics during GUI presentation");
        if (window.state.framebuffer == request.extent && !window.state.iconified) {
            if (!installed) {
                take(graphics.set_presentation_gui(request));
                installed = true;
            }
            const auto result = take(graphics.present({0, 0, 0, 1}));
            require(result.status != PresentationStatus::closed,
                    "GUI presentation closed before its phase completed");
            if (result.status == PresentationStatus::presented) {
                auto pixels = take(graphics.capture_presented());
                require(pixels.extent == request.extent &&
                            pixels.color_encoding == ReadbackColorEncoding::srgb &&
                            pixels.submitted != 0 && pixels.completed >= pixels.submitted &&
                            pixels.depth.size() == static_cast<std::size_t>(request.extent.width) *
                                                       request.extent.height &&
                            pixels.rgba8.size() == pixels.depth.size() * 4,
                        "GUI capture is not the completed presented sRGB attachment");
                return pixels;
            }
        }
        take(clock.wait_for(std::chrono::milliseconds{10}));
    }
    throw Failure{{ErrorCode::execution_failed,
                   "GUI presentation exceeded 120 event passes",
                   {"gui.smoke.present"}}};
}

template <typename T>
void write_bytes(const std::filesystem::path& path, const std::vector<T>& bytes) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size() * sizeof(T)));
    stream.close();
    require(static_cast<bool>(stream), "Cannot write GUI capture bytes");
}

Object run(int argc, char** argv) {
    require(argc == 9 || argc == 13,
            "Expected --device UUID --vertex FILE --pixel FILE --output DIRECTORY "
            "[--scene-vertex FILE --scene-pixel FILE]");
    std::string device;
    std::filesystem::path vertex, pixel, output, scene_vertex, scene_pixel;
    std::set<std::string> seen;
    for (int index = 1; index < argc; index += 2) {
        const std::string option{argv[index]};
        require(seen.insert(option).second, "Duplicate GUI fixture option");
        if (option == "--device")
            device = argv[index + 1];
        else if (option == "--vertex")
            vertex = argv[index + 1];
        else if (option == "--pixel")
            pixel = argv[index + 1];
        else if (option == "--output")
            output = argv[index + 1];
        else if (option == "--scene-vertex")
            scene_vertex = argv[index + 1];
        else if (option == "--scene-pixel")
            scene_pixel = argv[index + 1];
        else
            require(false, "Unknown GUI fixture option");
    }
    require(valid_graphics_uuid(device) && !vertex.empty() && !pixel.empty() && !output.empty(),
            "GUI fixture needs an explicit GPU and complete paths");
    require(scene_vertex.empty() == scene_pixel.empty(),
            "GUI viewport requires both scene shaders");
    const bool viewport_mode = !scene_vertex.empty();
    ShaderArtifact scene_vs, scene_ps;
    if (viewport_mode) {
        scene_vs = take(
            load_shader_artifact(scene_vertex, ShaderStage::vertex, ShaderInterface::scene_unlit));
        scene_ps = take(
            load_shader_artifact(scene_pixel, ShaderStage::pixel, ShaderInterface::scene_unlit));
    }
    GuiAtlas atlas;
    atlas.extent = {2, 2};
    atlas.rgba8.assign(16, 255);
    atlas.vertex_shader =
        take(load_shader_artifact(vertex, ShaderStage::vertex, ShaderInterface::gui));
    atlas.pixel_shader =
        take(load_shader_artifact(pixel, ShaderStage::pixel, ShaderInterface::gui));
    require(std::filesystem::create_directory(output), "GUI capture directory must be fresh");

    constexpr WindowExtent original{160, 120};
    constexpr WindowExtent resized{200, 144};
    GraphicsConfig config;
    config.adapter_uuid = device;
    config.validation = GraphicsValidation::required;
    config.window = {"Shark GUI render qualification", original, true, true};
    auto graphics = take(GraphicsDevice::create(config));
    const auto adapter = take(graphics->adapter());
    require(adapter.uuid == device, "GUI fixture selected a different GPU");
    take(graphics->configure_gui(atlas, true));
    Array phases;
    const auto capture = [&](const char* name, const GuiFrame& request) {
        auto pixels = present(*graphics, request);
        const auto image_count = take(graphics->presentation_stats()).image_count;
        require(image_count > 0 && image_count <= 16, "Unexpected GUI image count");
        const auto iterations = viewport_mode ? 2 * image_count + 1 : 1U;
        // Exercise acquired-slot reuse before the next camera/content change.
        for (std::uint32_t repeat = 1; repeat < iterations; ++repeat)
            pixels = present(*graphics, request);
        const auto color_file = std::string{name} + ".rgba8";
        const auto depth_file = std::string{name} + ".d32f";
        write_bytes(output / color_file, pixels.rgba8);
        write_bytes(output / depth_file, pixels.depth);
        Object row{{"name", name},
                   {"width", request.extent.width},
                   {"height", request.extent.height},
                   {"color_file", color_file},
                   {"depth_file", depth_file},
                   {"presentation", stats_json(take(graphics->presentation_stats()))}};
        if (viewport_mode) {
            row.emplace("iterations", iterations);
            row.emplace("image_count", image_count);
            row.emplace("viewport",
                        request.viewport
                            ? Json{Object{{"width", request.viewport->extent.width},
                                          {"height", request.viewport->extent.height},
                                          {"scene_instance",
                                           request.viewport->scene_revision.scene_instance},
                                          {"revision", request.viewport->scene_revision.revision}}}
                            : Json{});
        }
        phases.emplace_back(std::move(row));
    };
    if (viewport_mode) {
        for (unsigned phase = 0; phase < 4; ++phase) {
            constexpr std::array names{"scene-baseline", "scene-camera", "scene-content",
                                       "scene-target-resized"};
            capture(names[phase], viewport_fixture(original, scene_vs, scene_ps, phase));
        }
        take(graphics->resize(resized));
        for (unsigned phase = 4; phase < 7; ++phase) {
            constexpr std::array names{"scene-window-resized", "scene-removed", "scene-restored"};
            capture(names[phase - 4], viewport_fixture(resized, scene_vs, scene_ps, phase));
        }
    } else {
        capture("baseline", fixture(original, false));
        capture("changed", fixture(original, true));
        take(graphics->resize(resized));
        capture("resized", fixture(resized, true));
    }
    take(graphics->request_close());
    require(take(graphics->window_state()).close_requested,
            "Native close request was not observed");
    take(graphics->cancel_close_request());
    require(!take(graphics->window_state()).close_requested,
            "Native close cancellation was not observed");
    capture("close-cancelled", viewport_mode ? viewport_fixture(resized, scene_vs, scene_ps, 7)
                                             : fixture(resized, true));
    take(graphics->drain());
    take(graphics->close());
    const auto stats = take(graphics->presentation_stats());
    const auto diagnostics = take(graphics->diagnostics());
    require(stats.closed && !stats.faulted && stats.in_flight == 0 &&
                stats.submitted == stats.completed && stats.generation == stats.retired_generations,
            "GUI presentation owners did not retire after bounded close");
    Array messages;
    for (const auto& message : diagnostics.messages)
        messages.emplace_back(message);
    return {{"schema_version", std::uint32_t{1}},
            {"event", viewport_mode ? "gui-viewport-complete" : "gui-render-complete"},
            {"status", "PASS"},
            {"adapter_uuid", adapter.uuid},
            {"adapter_name", adapter.name},
            {"driver_name", adapter.driver_name},
            {"driver_info", adapter.driver_info},
            {"driver_version", adapter.driver_version},
            {"validation", "required"},
            {"phases", std::move(phases)},
            {"presentation", stats_json(stats)},
            {"diagnostics", Object{{"warnings", diagnostics.warnings},
                                   {"errors", diagnostics.errors},
                                   {"dropped", diagnostics.dropped},
                                   {"messages", std::move(messages)}}}};
}
} // namespace

int main(int argc, char** argv) {
    try {
        const auto result = run(argc, argv);
        Json{result}.write(std::cout);
        std::cout << '\n';
        return std::cout ? 0 : 1;
    } catch (const Failure& failure) {
        Json{Object{{"status", "FAIL"},
                    {"error", Object{{"code", std::string(error_code_name(failure.error().code))},
                                     {"operation", failure.error().context.operation},
                                     {"message", failure.what()}}}}}
            .write(std::cout);
    } catch (const std::exception& error) {
        Json{Object{{"status", "FAIL"}, {"message", error.what()}}}.write(std::cout);
    }
    std::cout << '\n';
    return 1;
}
