#include <shark/client/gpu_resources.hpp>
#include <shark/client/graphics.hpp>

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
        throw Failure(
            {shark::ErrorCode::execution_failed, message, {.operation = "offscreen-smoke"}});
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
    bool render{false};
    GraphicsConfig config;
    std::filesystem::path vertex;
    std::filesystem::path pixel;
    std::filesystem::path output;
};

Options parse(int argc, char* argv[]) {
    Options options;
    options.config.window = {
        .title = "Shark 06b offscreen", .content = {160, 120}, .visible = false};
    const auto invalid = [] {
        throw Failure({shark::ErrorCode::invalid_argument,
                       "Use --describe or --render --device UUID --vertex PATH --pixel PATH "
                       "--output FRESH_DIRECTORY --validation required|off",
                       {.operation = "offscreen-smoke.arguments"}});
    };
    if (argc == 2 && std::string_view(argv[1]) == "--describe") {
        return options;
    }
    if (argc != 12 || std::string_view(argv[1]) != "--render") {
        invalid();
    }
    options.render = true;
    std::set<std::string_view> seen;
    for (int index = 2; index < argc; index += 2) {
        const std::string_view key{argv[index]}, value{argv[index + 1]};
        if (value.empty() || !seen.insert(key).second) {
            invalid();
        }
        if (key == "--device" && valid_graphics_uuid(value)) {
            options.config.adapter_uuid = value;
        } else if (key == "--vertex") {
            options.vertex = std::filesystem::path(std::u8string(value.begin(), value.end()));
        } else if (key == "--pixel") {
            options.pixel = std::filesystem::path(std::u8string(value.begin(), value.end()));
        } else if (key == "--output") {
            options.output = std::filesystem::path(std::u8string(value.begin(), value.end()));
        } else if (key == "--validation" && (value == "required" || value == "off")) {
            options.config.validation =
                value == "required" ? GraphicsValidation::required : GraphicsValidation::off;
        } else {
            invalid();
        }
    }
    if (options.config.adapter_uuid.empty() || options.vertex.empty() || options.pixel.empty() ||
        options.output.empty() || !seen.contains("--validation")) {
        invalid();
    }
    return options;
}

OffscreenRequest cube_request(const Options& options, bool second) {
    OffscreenRequest request;
    request.vertex_shader = take(load_shader_artifact(options.vertex, ShaderStage::vertex));
    request.pixel_shader = take(load_shader_artifact(options.pixel, ShaderStage::pixel));
    request.extent = second ? WindowExtent{128, 128} : WindowExtent{96, 80};
    request.texture_extent = {2, 2};
    request.texture_rgba8 = {255, 32, 16,  255, 16,  255, 32, 255,
                             32,  16, 255, 255, 255, 224, 16, 255};
    request.transform = {.56F, .252F,  .12F,  0, 0, .56F, -.15F, 0,
                         .42F, -.336F, -.16F, 0, 0, 0,    .5F,   1};
    constexpr float h = .6F;
    constexpr std::array<std::array<float, 3>, 24> positions{
        {{-h, -h, h},  {h, -h, h},  {h, h, h},    {-h, h, h},  {h, -h, -h}, {-h, -h, -h},
         {-h, h, -h},  {h, h, -h},  {h, -h, h},   {h, -h, -h}, {h, h, -h},  {h, h, h},
         {-h, -h, -h}, {-h, -h, h}, {-h, h, h},   {-h, h, -h}, {-h, h, h},  {h, h, h},
         {h, h, -h},   {-h, h, -h}, {-h, -h, -h}, {h, -h, -h}, {h, -h, h},  {-h, -h, h}}};
    constexpr std::array<std::array<float, 2>, 4> uv{{{0, 1}, {1, 1}, {1, 0}, {0, 0}}};
    for (std::size_t index = 0; index < positions.size(); ++index) {
        request.vertices.push_back({positions[index], uv[index % 4]});
    }
    // A smaller, fully occluded cube is submitted after the near cube. Its solid
    // first texel would overwrite the reference if depth testing were disabled.
    for (const auto& position : positions) {
        request.vertices.push_back(
            {{position[0] * .65F + .384F, position[1] * .65F - .48F, position[2] * .65F - .512F},
             {.25F, .25F}});
    }
    for (std::uint32_t face = 0; face < 12; ++face) {
        for (const auto index : {0U, 1U, 2U, 0U, 2U, 3U}) {
            request.indices.push_back(face * 4 + index);
        }
    }
    if (second) {
        for (std::size_t left = 0, right = request.indices.size() - 3; left < right;
             left += 3, right -= 3) {
            for (std::size_t corner = 0; corner < 3; ++corner) {
                std::swap(request.indices[left + corner], request.indices[right + corner]);
            }
        }
        for (std::size_t channel = 0; channel < 4; ++channel) {
            std::swap(request.texture_rgba8[channel], request.texture_rgba8[12 + channel]);
        }
    }
    return request;
}

template <typename T> Array numbers(const T& values) {
    Array result;
    for (const auto value : values) {
        result.emplace_back(static_cast<double>(value));
    }
    return result;
}
Object extent_json(WindowExtent extent) {
    return {{"width", extent.width}, {"height", extent.height}};
}
void write_file(const std::filesystem::path& path, const void* data, std::size_t size) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    stream.close();
    require(static_cast<bool>(stream), "Cannot write readback artifact");
}

void run(const Options& options) {
    const bool validated = options.config.validation == GraphicsValidation::required;
    emit({{"kind", "identity"},
          {"schema_version", 1U},
          {"assignment_06", "NOT_RUN"},
          {"mode", options.render ? "render" : "describe"},
          {"dependency_lock_sha256", SHARK_GRAPHICS_LOCK_SHA256},
          {"source_sha256", SHARK_GRAPHICS_SOURCE_SHA256},
          {"compiler", SHARK_GRAPHICS_COMPILER},
          {"selection", options.config.adapter_uuid},
          {"validation", validated ? "required" : "off"}});
    if (!options.render) {
        emit({{"kind", "description"},
              {"status", "PASS"},
              {"qualification", "NOT_RUN"},
              {"device_accessed", false},
              {"cases", 2U},
              {"native_graphics_available", native_graphics_available()},
              {"scope", "Bounded offscreen textured cube and independent readback; no window draw "
                        "or authoring UI"}});
        return;
    }
    require(!std::filesystem::exists(options.output), "Readback directory must be new");
    std::filesystem::create_directories(options.output);
    // Validate artifacts and request before opening a native device.
    auto first = cube_request(options, false);
    auto second = cube_request(options, true);
    require(static_cast<bool>(validate_offscreen_request(first)), "Invalid first fixture request");
    require(static_cast<bool>(validate_offscreen_request(second)),
            "Invalid second fixture request");
    auto device = take(GraphicsDevice::create(options.config));
    emit({{"kind", "adapter"}, {"adapter", adapter_json(take(device->adapter()))}});
    std::uint64_t previous = 0;
    for (auto* request : {&first, &second}) {
        const std::string name = request == &first ? "cube-96x80" : "cube-128x128";
        const auto result = take(device->render_offscreen(*request));
        require(result.extent == request->extent, "Readback extent mismatch");
        require(result.submitted > previous && result.completed == result.submitted,
                "Readback requires a new completed queue submission");
        require(result.created_resources == 17 && result.retired_resources == 17,
                "Offscreen resources did not retire after completion");
        previous = result.submitted;
        write_file(options.output / (name + ".rgba8"), result.rgba8.data(), result.rgba8.size());
        write_file(options.output / (name + ".d32f"), result.depth.data(),
                   result.depth.size() * sizeof(float));
        Array vertices;
        for (const auto& vertex : request->vertices) {
            vertices.emplace_back(Array{vertex.position[0], vertex.position[1], vertex.position[2],
                                        vertex.uv[0], vertex.uv[1]});
        }
        emit({{"kind", "case"},
              {"name", name},
              {"extent", extent_json(result.extent)},
              {"vertices", std::move(vertices)},
              {"indices", numbers(request->indices)},
              {"transform", numbers(request->transform)},
              {"texture_extent", extent_json(request->texture_extent)},
              {"texture_rgba8", numbers(request->texture_rgba8)},
              {"submitted", result.submitted},
              {"completed", result.completed},
              {"subpixel_precision_bits", result.subpixel_precision_bits},
              {"created_resources", result.created_resources},
              {"retired_resources", result.retired_resources},
              {"color_file", name + ".rgba8"},
              {"depth_file", name + ".d32f"}});
    }
    take(device->drain());
    take(device->close());
    const auto diagnostics = take(device->diagnostics());
    require(clean(diagnostics), "Graphics validation reported diagnostics");
    emit({{"kind", "complete"},
          {"status", validated ? "PASS" : "NOT_RUN"},
          {"execution_status", "PASS"},
          {"cases", 2U},
          {"assignment_06", "NOT_RUN"},
          {"qualification", validated ? "PASS" : "NOT_RUN"},
          {"diagnostics", diagnostics_json(diagnostics)}});
}
} // namespace

int main(int argc, char* argv[]) {
    try {
        run(parse(argc, argv));
        return 0;
    } catch (const Failure& failure) {
        emit({{"kind", "failure"}, {"status", "FAIL"}, {"error", error_json(failure.error())}});
    } catch (const std::exception& error) {
        emit({{"kind", "failure"}, {"status", "FAIL"}, {"message", error.what()}});
    }
    return 1;
}
