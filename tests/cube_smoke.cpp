#include "engine/client/src/graphics_backend.hpp"
#include <shark/client/cube_sample.hpp>
#include <shark/client/gpu_resources.hpp>
#include <shark/client/graphics.hpp>
#include <shark/client/shader_bundle.hpp>
#include <shark/time.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
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
using namespace shark::client;
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
        throw Failure({shark::ErrorCode::execution_failed, message, {.operation = "cube-smoke"}});
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
    bool smoke{false};
    GraphicsConfig config;
    std::filesystem::path bundle;
    std::filesystem::path output;
};
Options parse(int argc, char* argv[]) {
    Options options;
    options.config.window = {.title = "Shark 06c cube", .content = {192, 128}, .visible = true};
    const auto invalid = [] {
        throw Failure({shark::ErrorCode::invalid_argument,
                       "Use --describe or --smoke --device UUID --bundle PATH --output "
                       "FRESH_DIRECTORY --validation required|off",
                       {.operation = "cube-smoke.arguments"}});
    };
    if (argc == 2 && std::string_view(argv[1]) == "--describe")
        return options;
    if (argc != 10 || std::string_view(argv[1]) != "--smoke")
        invalid();
    options.smoke = true;
    std::set<std::string_view> seen;
    for (int i = 2; i < argc; i += 2) {
        const std::string_view key{argv[i]}, value{argv[i + 1]};
        if (value.empty() || !seen.insert(key).second)
            invalid();
        if (key == "--device" && valid_graphics_uuid(value))
            options.config.adapter_uuid = value;
        else if (key == "--bundle")
            options.bundle = std::filesystem::path(std::u8string(value.begin(), value.end()));
        else if (key == "--output")
            options.output = std::filesystem::path(std::u8string(value.begin(), value.end()));
        else if (key == "--validation" && (value == "required" || value == "off"))
            options.config.validation =
                value == "required" ? GraphicsValidation::required : GraphicsValidation::off;
        else
            invalid();
    }
    if (options.config.adapter_uuid.empty() || options.bundle.empty() || options.output.empty() ||
        !seen.contains("--validation"))
        invalid();
    return options;
}
Object extent_json(WindowExtent extent) {
    return {{"width", extent.width}, {"height", extent.height}};
}
Object stats_json(const PresentationStats& s) {
    return {{"submitted", s.submitted},
            {"completed", s.completed},
            {"presented", s.presented},
            {"generation", s.generation},
            {"retired_generations", s.retired_generations},
            {"suspended_frames", s.suspended_frames},
            {"in_flight", s.in_flight},
            {"frame_limit", s.frame_limit},
            {"active", s.active},
            {"closed", s.closed},
            {"faulted", s.faulted}};
}
Object resources_json(const detail::GraphicsResourceSnapshot& s) {
    return {{"window_draw_resources", s.window_draw_resources},
            {"window_draw_submissions", s.window_draw_submissions},
            {"live_swapchains", s.live_swapchains},
            {"offscreen_resources", s.offscreen_resources},
            {"offscreen_submissions", s.offscreen_submissions},
            {"offscreen_recordings", s.offscreen_recordings},
            {"pending_queue_workers", s.pending_queue_workers}};
}
WindowState poll(GraphicsDevice& device) {
    const auto frame = take(device.poll_window());
    require(!frame.state.close_requested && frame.diagnostics.rejected_input_events == 0 &&
                frame.diagnostics.invalid_window_events == 0 && !frame.diagnostics.input_error &&
                !frame.diagnostics.window_error,
            "Window closed or reported unexpected callback diagnostics");
    return frame.state;
}
template <typename Predicate> WindowState await_state(GraphicsDevice& device, Predicate predicate) {
    shark::SteadyMonotonicClock clock;
    const auto deadline = clock.now() + std::chrono::seconds{2};
    WindowState last;
    while (clock.now() < deadline) {
        const auto state = poll(device);
        last = state;
        if (predicate(state))
            return state;
        take(clock.wait_for(std::chrono::milliseconds{5}));
    }
    throw std::runtime_error(
        "Window state did not reach the requested workload within two seconds; content=" +
        std::to_string(last.content.width) + "x" + std::to_string(last.content.height) +
        " framebuffer=" + std::to_string(last.framebuffer.width) + "x" +
        std::to_string(last.framebuffer.height) + " iconified=" + std::to_string(last.iconified));
}
void frames(GraphicsDevice& device) {
    shark::SteadyMonotonicClock clock;
    for (std::uint32_t frame = 0; frame < 3; ++frame) {
        bool done = false;
        for (std::uint32_t attempt = 0; attempt < 32; ++attempt) {
            const auto state = poll(device);
            require(!state.iconified, "Unexpected minimize during draw");
            const auto result = take(device.present({0, 0, 0, 1}));
            if (result.status == PresentationStatus::presented) {
                done = true;
                break;
            }
            require(result.status == PresentationStatus::waiting ||
                        result.status == PresentationStatus::recreate_pending,
                    "Unexpected draw presentation outcome");
            take(clock.wait_for(std::chrono::milliseconds{5}));
        }
        require(done, "Draw exhausted bounded presentation attempts");
    }
}
void write_file(const std::filesystem::path& path, const void* data, std::size_t size) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    stream.close();
    require(static_cast<bool>(stream), "Cannot write cube readback");
}
Object save_readback(const std::filesystem::path& output, const std::string& name,
                     const OffscreenReadback& readback) {
    write_file(output / (name + ".rgba8"), readback.rgba8.data(), readback.rgba8.size());
    write_file(output / (name + ".d32f"), readback.depth.data(),
               readback.depth.size() * sizeof(float));
    return {{"extent", extent_json(readback.extent)},
            {"submitted", readback.submitted},
            {"completed", readback.completed},
            {"subpixel_precision_bits", readback.subpixel_precision_bits},
            {"color_encoding",
             readback.color_encoding == ReadbackColorEncoding::srgb ? "srgb" : "linear"},
            {"created_resources", readback.created_resources},
            {"retired_resources", readback.retired_resources},
            {"color_file", name + ".rgba8"},
            {"depth_file", name + ".d32f"}};
}
void run(const Options& options) {
    const bool validated = options.config.validation == GraphicsValidation::required;
    emit({{"kind", "identity"},
          {"schema_version", 1U},
          {"mode", options.smoke ? "smoke" : "describe"},
          {"dependency_lock_sha256", SHARK_GRAPHICS_LOCK_SHA256},
          {"source_sha256", SHARK_GRAPHICS_SOURCE_SHA256},
          {"compiler", SHARK_GRAPHICS_COMPILER},
          {"selection", options.config.adapter_uuid},
          {"validation", validated ? "required" : "off"}});
    if (!options.smoke) {
        emit({{"kind", "description"},
              {"device_accessed", false},
              {"cases", 3U},
              {"qualification", "NOT_RUN"}});
        return;
    }
    require(!std::filesystem::exists(options.output), "Readback output directory must be fresh");
    std::filesystem::create_directories(options.output);
    const auto bundle = take(load_shader_bundle(options.bundle));
    auto request = take(make_cube_sample(bundle, {192, 128}));
    auto backend = detail::make_graphics_backend();
    auto* observer = backend.get();
    auto device = take(detail::create_graphics(options.config, std::move(backend)));
    take(device->set_presentation_draw(request, true));
    emit({{"kind", "adapter"},
          {"adapter", adapter_json(take(device->adapter()))},
          {"bundle_sha256", bundle.sha256}});
    constexpr std::array<WindowExtent, 3> sizes{{{192, 128}, {256, 192}, {192, 192}}};
    for (std::uint32_t index = 0; index < sizes.size(); ++index) {
        // Win32 applies initial monitor DPI scaling. Normalize the exact
        // pixel workload before first acquisition, as in PresentationSmoke.
        take(device->resize(sizes[index]));
        const auto state = await_state(*device, [&](const WindowState& s) {
            return !s.iconified && s.content == sizes[index];
        });
        require(state.framebuffer == sizes[index],
                "Fixture requires its exact current-PC pixel extent");
        frames(*device);
        const auto window = take(device->capture_presented());
        require(window.extent == sizes[index] &&
                    window.color_encoding == ReadbackColorEncoding::srgb,
                "Window capture extent or color encoding mismatch");
        request.extent = sizes[index];
        const auto offscreen = take(device->render_offscreen(request));
        require(offscreen.extent == sizes[index] &&
                    offscreen.color_encoding == ReadbackColorEncoding::linear,
                "Offscreen capture extent or color encoding mismatch");
        emit({{"kind", "case"},
              {"index", index},
              {"window", save_readback(options.output, "window-" + std::to_string(index), window)},
              {"offscreen",
               save_readback(options.output, "offscreen-" + std::to_string(index), offscreen)},
              {"stats", stats_json(take(device->presentation_stats()))},
              {"resources", resources_json(observer->presentation_resources())}});
        if (index == 0) {
            require(!device->set_presentation_draw(request, true),
                    "Draw configuration changed after presentation began");
            take(device->iconify());
            static_cast<void>(
                await_state(*device, [](const WindowState& s) { return s.iconified; }));
            const auto before = take(device->presentation_stats());
            const auto suspended = take(device->present());
            require(suspended.status == PresentationStatus::suspended &&
                        suspended.stats.submitted == before.submitted,
                    "Minimized cube must not submit GPU work");
            emit({{"kind", "minimized"},
                  {"before", stats_json(before)},
                  {"after", stats_json(suspended.stats)}});
            take(device->restore());
        }
    }
    take(device->drain());
    take(device->close());
    const auto diagnostics = take(device->diagnostics());
    require(clean(diagnostics), "Unexpected graphics validation diagnostics");
    emit({{"kind", "complete"},
          {"status", validated ? "PASS" : "NOT_RUN"},
          {"stats", stats_json(take(device->presentation_stats()))},
          {"resources", resources_json(observer->presentation_resources())},
          {"diagnostics", diagnostics_json(diagnostics)}});
}
} // namespace
int main(int argc, char* argv[]) {
    try {
        run(parse(argc, argv));
        return 0;
    } catch (const Failure& failure) {
        emit({{"kind", "failure"}, {"error", error_json(failure.error())}});
    } catch (const std::exception& error) {
        emit({{"kind", "failure"}, {"message", error.what()}});
    }
    return 1;
}
