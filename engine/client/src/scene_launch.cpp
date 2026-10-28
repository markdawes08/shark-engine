#include <shark/client/prepared_package.hpp>
#include <shark/client/scene_launch.hpp>

#include "shader_path.hpp"
#include "window_loop.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

namespace shark::client {
namespace {
std::string json_string(std::string_view value) {
    std::string result{"\""};
    constexpr char hex[] = "0123456789abcdef";
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '"' || character == '\\') {
            result += '\\';
            result += character;
        } else if (byte < 32) {
            result += "\\u00";
            result += hex[byte >> 4];
            result += hex[byte & 15];
        } else
            result += character;
    }
    result += '"';
    return result;
}
Error invalid() {
    return {ErrorCode::invalid_argument,
            "Expected --package PATH or --scene PATH --camera UUID --vertex-shader PATH "
            "--pixel-shader PATH; "
            "optional --mesh PATH, --extent WxH, --scene-ms, --frames, --capture-directory, "
            "--device and --validation; only --mesh may repeat",
            {.operation = "client.scene.arguments.parse"}};
}
bool number(std::string_view value, std::uint32_t maximum, std::uint32_t& result) {
    if (value.empty() || value.size() > 5)
        return false;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    return error == std::errc{} && end == value.data() + value.size() && result > 0 &&
           result <= maximum;
}
Result<std::filesystem::path> path_argument(std::string_view value) {
    if (value.empty() || value.size() > 4096 || value.find('\0') != value.npos)
        return invalid();
    auto path = std::filesystem::path{
        std::u8string{reinterpret_cast<const char8_t*>(value.data()), value.size()}};
    if (!detail::artifact_path_text(path))
        return invalid();
    return path;
}
Result<SceneViewportRequest> frame_request(const LoadedPreparedScene& content,
                                           AuthoredEntityId camera, WindowExtent extent,
                                           const ShaderArtifact& vertex,
                                           const ShaderArtifact& pixel,
                                           RenderExtractionStats& extraction) {
    const auto& scene = content.scene();
    auto found = scene.find(camera);
    if (!found)
        return found.error();
    auto stats = scene.stats();
    if (!stats)
        return stats.error();
    auto rendered = extract_render_scene(scene, *content.assets(),
                                         {found.value(),
                                          {stats.value().instance, stats.value().revision},
                                          {extent.width, extent.height},
                                          {{0.0, 0.0, 0.0}, 1, content.world_origin()}});
    if (!rendered)
        return rendered.error();
    extraction = rendered.value().stats();
    return make_scene_viewport_request(rendered.value(), vertex, pixel);
}
Result<void> write_bytes(const std::filesystem::path& path, const char* data, std::size_t size) {
    auto text = detail::artifact_path_text(path);
    if (!text)
        return text.error();
    std::ofstream stream(path, std::ios::binary);
    stream.write(data, static_cast<std::streamsize>(size));
    stream.close();
    if (!stream) {
        return Error{ErrorCode::io_failure,
                     "Cannot write Runtime capture",
                     {.operation = "client.scene.capture", .path = text.value()}};
    }
    return {};
}
struct SceneCandidate {
    std::unique_ptr<LoadedPreparedScene> content;
    AuthoredEntityId camera;
    ShaderArtifact vertex;
    ShaderArtifact pixel;
    std::optional<PreparedStartup> startup;
};
Result<SceneCandidate> load_candidate(const SceneLaunchOptions& options,
                                      const SceneCandidate* current = nullptr) {
    if (!options.package.empty()) {
        auto package = load_prepared_package(options.package);
        if (!package)
            return package.error();
        auto value = std::move(package).value();
        return SceneCandidate{std::move(value.content), value.startup.camera,
                              std::move(value.vertex_shader), std::move(value.pixel_shader),
                              std::move(value.startup)};
    }
    auto loaded = load_scene_content(options.content);
    if (!loaded)
        return loaded.error();
    if (current)
        return SceneCandidate{
            std::move(loaded).value(), current->camera, current->vertex, current->pixel, {}};
    auto vertex = load_shader_artifact(options.vertex_shader, ShaderStage::vertex,
                                       ShaderInterface::scene_unlit);
    if (!vertex)
        return vertex.error();
    auto pixel = load_shader_artifact(options.pixel_shader, ShaderStage::pixel,
                                      ShaderInterface::scene_unlit);
    if (!pixel)
        return pixel.error();
    return SceneCandidate{std::move(loaded).value(),
                          *options.camera,
                          std::move(vertex).value(),
                          std::move(pixel).value(),
                          {}};
}
} // namespace

Result<SceneLaunchOptions> parse_scene_arguments(std::span<const std::string_view> arguments) {
    if (arguments.empty() || (arguments.front() != "--scene" && arguments.front() != "--package") ||
        arguments.size() % 2 != 0 || arguments.size() > scene_launch_maximum_arguments)
        return invalid();
    try {
        SceneLaunchOptions result;
        std::set<std::string_view> seen;
        for (std::size_t index = 0; index < arguments.size(); index += 2) {
            const auto key = arguments[index];
            const auto value = arguments[index + 1];
            if (key != "--mesh" && !seen.insert(key).second)
                return invalid();
            if (key == "--package" || key == "--scene" || key == "--mesh" ||
                key == "--vertex-shader" || key == "--pixel-shader" ||
                key == "--capture-directory") {
                auto path = path_argument(value);
                if (!path)
                    return path.error();
                if (key == "--package")
                    result.package = std::move(path).value();
                else if (key == "--scene")
                    result.content.scene = std::move(path).value();
                else if (key == "--mesh") {
                    if (result.content.meshes.size() == 126)
                        return invalid();
                    result.content.meshes.push_back(std::move(path).value());
                } else if (key == "--vertex-shader")
                    result.vertex_shader = std::move(path).value();
                else if (key == "--pixel-shader")
                    result.pixel_shader = std::move(path).value();
                else
                    result.capture_directory = std::move(path).value();
            } else if (key == "--camera") {
                auto id = StableId::parse(value);
                if (!id)
                    return invalid();
                result.camera = AuthoredEntityId{id.value()};
            } else if (key == "--scene-ms" || key == "--frames") {
                std::uint32_t parsed = 0;
                if (!number(value, key == "--frames" ? 600U : 60000U, parsed))
                    return invalid();
                if (key == "--frames")
                    result.frames = parsed;
                else
                    result.duration = std::chrono::milliseconds{parsed};
            } else if (key == "--extent") {
                if (value.size() > 11)
                    return invalid();
                const auto split = value.find('x');
                if (split == value.npos ||
                    !number(value.substr(0, split), 2048, result.extent.width) ||
                    !number(value.substr(split + 1), 2048, result.extent.height))
                    return invalid();
            } else if (key == "--device") {
                if (value != "auto" && !valid_graphics_uuid(value))
                    return invalid();
                if (value != "auto")
                    result.adapter_uuid = value;
            } else if (key == "--validation") {
                if (value != "required" && value != "off")
                    return invalid();
                result.validation =
                    value == "required" ? GraphicsValidation::required : GraphicsValidation::off;
            } else
                return invalid();
        }
        if (result.package.empty()
                ? (!result.camera || result.vertex_shader.empty() || result.pixel_shader.empty())
                : (!result.content.scene.empty() || !result.content.meshes.empty() ||
                   result.camera || !result.vertex_shader.empty() || !result.pixel_shader.empty()))
            return invalid();
        return result;
    } catch (...) {
        return invalid();
    }
}

Result<void> run_scene(std::string_view title, const SceneLaunchOptions& options) {
    if ((options.package.empty()
             ? !options.camera
             : (options.camera || !options.content.scene.empty() ||
                !options.content.meshes.empty() || !options.vertex_shader.empty() ||
                !options.pixel_shader.empty())) ||
        options.extent.width == 0 || options.extent.height == 0 || options.extent.width > 2048 ||
        options.extent.height > 2048 ||
        (options.duration && (*options.duration < std::chrono::milliseconds{1} ||
                              *options.duration > std::chrono::milliseconds{60000})) ||
        (options.frames && (*options.frames == 0 || *options.frames > 600)) ||
        (!options.adapter_uuid.empty() && !valid_graphics_uuid(options.adapter_uuid)) ||
        (options.validation != GraphicsValidation::required &&
         options.validation != GraphicsValidation::off))
        return invalid();
    try {
#if !defined(_WIN32) || (!defined(_M_X64) && !defined(__x86_64__))
        if (!options.package.empty())
            return Error{ErrorCode::capability_unavailable,
                         "Prepared package launch currently requires Windows x86-64; CPU "
                         "inspection is portable",
                         {.operation = "client.package.run"}};
#endif
        auto loaded = load_candidate(options);
        if (!loaded)
            return loaded.error();
        auto active = std::move(loaded).value();
        RenderExtractionStats extraction;
        auto initial = frame_request(*active.content, active.camera, options.extent, active.vertex,
                                     active.pixel, extraction);
        if (!initial)
            return initial.error();
        const bool capture = !options.capture_directory.empty();
        if (capture) {
            auto valid = detail::artifact_path_text(options.capture_directory);
            if (!valid)
                return valid.error();
            std::error_code error;
            if (!std::filesystem::create_directory(options.capture_directory, error) || error)
                return Error{ErrorCode::io_failure,
                             "Capture directory must be new with an existing parent",
                             {.operation = "client.scene.capture", .path = valid.value()}};
        }
        GraphicsConfig config;
        config.window.title = active.startup ? active.startup->name.value : std::string{title};
        config.window.content = options.extent;
        config.adapter_uuid = options.adapter_uuid;
        config.validation = options.validation;
        auto created = GraphicsDevice::create(config);
        if (!created)
            return created.error();
        auto graphics = std::move(created).value();
        auto current = std::move(initial).value();
        if (auto installed = graphics->set_presentation_scene(current, capture); !installed)
            return installed.error();
        SteadyMonotonicClock clock;
        auto made_loop = OfflineFrameLoop::create(clock.now());
        if (!made_loop)
            return made_loop.error();
        auto loop = std::move(made_loop).value();
        Logger diagnostics{std::cerr};
        bool manual_pause = false;
        std::uint64_t frames = 0;
        const OfflineInputCallback tick = [](std::uint64_t, const InputSnapshot&) -> Result<void> {
            return {};
        };
        const auto deadline = options.duration.value_or(std::chrono::milliseconds{60000});
        while (true) {
            const auto now = clock.now();
            if ((options.frames && frames >= *options.frames) ||
                ((options.duration || options.frames) && now >= deadline))
                break;
            auto snapshot = graphics->poll_window();
            if (!snapshot)
                return snapshot.error();
            const auto state = snapshot.value().state;
            const bool reload = snapshot.value().input.key(Key::f5).pressed;
            const bool prior_pause = manual_pause;
            auto frame = detail::advance_offline_window(std::move(snapshot).value(), *loop, now,
                                                        manual_pause, tick, diagnostics);
            if (!frame)
                return frame.error();
            if (!frame.value())
                break;
            if (auto written = detail::write_offline_control(
                    prior_pause, manual_pause, *frame.value(), loop->next_tick(), std::cout);
                !written)
                return written.error();
            if (!state.iconified && state.framebuffer.width != 0 && state.framebuffer.height != 0) {
                if (reload || state.framebuffer != current.extent) {
                    std::optional<SceneCandidate> candidate;
                    if (reload) {
                        auto replacement = load_candidate(options, &active);
                        if (replacement)
                            candidate = std::move(replacement).value();
                        else if (!diagnostics.write(LogLevel::error, replacement.error()))
                            return replacement.error();
                    }
                    RenderExtractionStats next_extraction;
                    const auto& source = candidate ? *candidate : active;
                    auto next = frame_request(*source.content, source.camera, state.framebuffer,
                                              source.vertex, source.pixel, next_extraction);
                    if (!next && candidate) {
                        if (!diagnostics.write(LogLevel::error, next.error()))
                            return next.error();
                        candidate.reset();
                        next = frame_request(*active.content, active.camera, state.framebuffer,
                                             active.vertex, active.pixel, next_extraction);
                    }
                    if (!next)
                        return next.error();
                    if (auto installed = graphics->set_presentation_scene(next.value(), capture);
                        !installed)
                        return installed.error();
                    current = std::move(next).value();
                    extraction = next_extraction;
                    if (candidate)
                        active = std::move(*candidate);
                }
            }
            auto presented = graphics->present();
            if (!presented)
                return presented.error();
            if (presented.value().status == PresentationStatus::closed)
                break;
            if (presented.value().status == PresentationStatus::presented)
                ++frames;
            if (auto waited = clock.wait_for(std::chrono::milliseconds{10}); !waited)
                return waited.error();
        }
        if ((options.frames && frames < *options.frames) || (capture && frames == 0))
            return Error{ErrorCode::execution_failed,
                         "Scene launch ended before its requested frames were presented",
                         {.operation = "client.scene.run"}};
        std::optional<OffscreenReadback> readback;
        if (capture) {
            auto pixels = graphics->capture_presented();
            if (!pixels)
                return pixels.error();
            readback = std::move(pixels).value();
        }
        if (auto closed = graphics->close(); !closed)
            return closed.error();
        auto stats = graphics->presentation_stats();
        auto adapter = graphics->adapter();
        auto messages = graphics->diagnostics();
        if (!stats)
            return stats.error();
        if (!adapter)
            return adapter.error();
        if (!messages)
            return messages.error();
        const auto& s = stats.value();
        if (!s.closed || s.faulted || s.in_flight != 0 || s.submitted != s.completed ||
            s.generation != s.retired_generations || messages.value().errors ||
            messages.value().dropped)
            return Error{ErrorCode::execution_failed,
                         "Scene presentation did not close cleanly",
                         {.operation = "client.scene.close"}};
        std::ostringstream report;
        report << "{\"event\":\"runtime-scene-complete\",\"adapter_name\":"
               << json_string(adapter.value().name)
               << ",\"driver_name\":" << json_string(adapter.value().driver_name)
               << ",\"driver_info\":" << json_string(adapter.value().driver_info)
               << ",\"driver_version\":" << adapter.value().driver_version << ",\"adapter_uuid\":\""
               << adapter.value().uuid << "\",\"validation\":\""
               << (options.validation == GraphicsValidation::required ? "required" : "off")
               << "\",\"width\":" << current.extent.width << ",\"height\":" << current.extent.height
               << ",\"rendered_frames\":" << frames << ",\"draw_count\":" << current.draws.size()
               << ",\"scene_instance\":" << current.scene_revision.scene_instance
               << ",\"scene_revision\":" << current.scene_revision.revision
               << ",\"hidden\":" << extraction.hidden << ",\"culled\":" << extraction.culled
               << ",\"mesh_instances\":" << extraction.mesh_instances
               << ",\"warnings\":" << messages.value().warnings
               << ",\"errors\":" << messages.value().errors
               << ",\"dropped\":" << messages.value().dropped
               << ",\"presentation\":{\"submitted\":" << s.submitted
               << ",\"completed\":" << s.completed << ",\"presented\":" << s.presented
               << ",\"generations\":" << s.generation
               << ",\"retired_generations\":" << s.retired_generations
               << ",\"in_flight\":" << s.in_flight
               << ",\"closed\":true,\"faulted\":false},\"qualification\":\"NOT_RUN\"";
        if (readback) {
            if (readback->extent != current.extent ||
                readback->color_encoding != ReadbackColorEncoding::srgb)
                return Error{ErrorCode::execution_failed,
                             "Scene capture differs from the presented target",
                             {.operation = "client.scene.capture"}};
            auto written = write_bytes(options.capture_directory / "color.rgba",
                                       reinterpret_cast<const char*>(readback->rgba8.data()),
                                       readback->rgba8.size());
            if (!written)
                return written.error();
            std::vector<char> depth;
            depth.reserve(readback->depth.size() * 8);
            for (float value : readback->depth) {
                const auto bits = std::bit_cast<std::uint64_t>(static_cast<double>(value));
                for (unsigned shift = 0; shift < 64; shift += 8)
                    depth.push_back(static_cast<char>((bits >> shift) & 255));
            }
            written =
                write_bytes(options.capture_directory / "depth.f64", depth.data(), depth.size());
            if (!written)
                return written.error();
            report << ",\"capture\":{\"color\":\"color.rgba\",\"depth\":\"depth.f64\",\"color_"
                      "space\":\"srgb\",\"subpixel_precision_bits\":"
                   << readback->subpixel_precision_bits << "}";
        }
        report << "}\n";
        const auto text = report.str();
        if (capture) {
            auto written =
                write_bytes(options.capture_directory / "report.json", text.data(), text.size());
            if (!written)
                return written.error();
        }
        std::cout << text << std::flush;
        if (!std::cout)
            return Error{ErrorCode::io_failure,
                         "Cannot write scene completion",
                         {.operation = "client.scene.output"}};
        return {};
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Runtime scene launch failed",
                     {.operation = "client.scene.run"}};
    }
}
} // namespace shark::client
