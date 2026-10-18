#include <shark/client/graphics.hpp>

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
constexpr std::uint32_t cycle_count = 16;

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
            {shark::ErrorCode::execution_failed, message, {.operation = "graphics-device-smoke"}});
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
    std::string_view mode{"describe"};
    GraphicsConfig config;
};

Options parse(int argc, char* argv[]) {
    Options options;
    options.config.window.title = "Shark 05a device foundation - \u6d4b\u8bd5";
    options.config.window.content = {320, 240};
    options.config.window.visible = false;
    const auto invalid = [] {
        throw Failure(
            {shark::ErrorCode::invalid_argument,
             "Use --help, --describe, --inventory [--validation required|off], or "
             "--smoke [--device auto|<32 lowercase hex UUID>] [--validation required|off]",
             {.operation = "graphics-device-smoke.arguments"}});
    };
    if (argc > 6) {
        invalid();
    }
    if (argc > 1) {
        const std::string_view mode{argv[1]};
        if (mode == "--help" || mode == "--describe" || mode == "--inventory" ||
            mode == "--smoke") {
            options.mode = mode.substr(2);
        } else {
            invalid();
        }
    }
    bool device_seen = false;
    bool validation_seen = false;
    for (int index = 2; index < argc; index += 2) {
        if (index + 1 >= argc || options.mode == "help" || options.mode == "describe") {
            invalid();
        }
        const std::string_view option{argv[index]};
        const std::string_view value{argv[index + 1]};
        if (option == "--device" && !device_seen && options.mode == "smoke") {
            device_seen = true;
            if (value != "auto") {
                if (!valid_graphics_uuid(value)) {
                    invalid();
                }
                options.config.adapter_uuid = value;
            }
        } else if (option == "--validation" && !validation_seen &&
                   (value == "required" || value == "off")) {
            validation_seen = true;
            options.config.validation =
                value == "required" ? GraphicsValidation::required : GraphicsValidation::off;
        } else {
            invalid();
        }
    }
    return options;
}

void run(const Options& options) {
    const bool validated = options.config.validation == GraphicsValidation::required;
    emit({{"kind", "identity"},
          {"schema_version", 1U},
          {"mode", std::string(options.mode)},
          {"dependency_lock_sha256", SHARK_GRAPHICS_LOCK_SHA256},
          {"source_sha256", SHARK_GRAPHICS_SOURCE_SHA256},
          {"compiler", SHARK_GRAPHICS_COMPILER},
          {"validation", validated ? "required" : "off"},
          {"selection", options.config.adapter_uuid.empty() ? "auto" : options.config.adapter_uuid},
          {"assignment_05", "NOT_RUN"}});
    if (options.mode == "help" || options.mode == "describe") {
        emit({{"kind", "description"},
              {"status", "PASS"},
              {"qualification", "NOT_RUN"},
              {"native_graphics_available", native_graphics_available()},
              {"device_accessed", false},
              {"cycles", cycle_count},
              {"usage", "--help | --describe | --inventory [--validation required|off] | "
                        "--smoke [--device auto|UUID] [--validation required|off]"},
              {"scope", "Owned graphics device foundation; no submission, presentation, rendering "
                        "or authoring"}});
        return;
    }
    if (options.mode == "inventory") {
        const auto adapters = take(GraphicsDevice::inventory(options.config));
        Array inventory;
        for (const auto& adapter : adapters) {
            inventory.emplace_back(adapter_json(adapter));
        }
        emit({{"kind", "inventory"},
              {"status", "PASS"},
              {"qualification", "NOT_RUN"},
              {"adapters", std::move(inventory)}});
        return;
    }

    for (std::uint32_t cycle = 0; cycle < cycle_count; ++cycle) {
        auto device = take(GraphicsDevice::create(options.config));
        const auto adapter = take(device->adapter());
        const auto before_close = take(device->diagnostics());
        const auto second = GraphicsDevice::create(options.config);
        const bool second_rejected = !second && second.error().code == shark::ErrorCode::conflict;
        const auto first_close = device->close();
        const auto after_close = take(device->diagnostics());
        const auto second_close = device->close();
        const auto after_second_close = take(device->diagnostics());
        const bool idempotent = after_second_close.warnings == after_close.warnings &&
                                after_second_close.errors == after_close.errors &&
                                after_second_close.dropped == after_close.dropped &&
                                after_second_close.messages == after_close.messages;
        const bool selection_matches =
            options.config.adapter_uuid.empty() || adapter.uuid == options.config.adapter_uuid;
        const bool supported = missing_graphics_requirements(adapter).empty();
        const bool diagnostics_clean =
            clean(before_close) && clean(after_close) && clean(after_second_close);
        const bool passed = first_close && second_close && second_rejected && idempotent &&
                            selection_matches && supported && (!validated || diagnostics_clean);
        Object row{{"kind", "cycle"},
                   {"cycle", cycle + 1},
                   {"status", passed ? (validated ? "PASS" : "DIAGNOSTIC") : "FAIL"},
                   {"adapter", adapter_json(adapter)},
                   {"second_owner_rejected", second_rejected},
                   {"idempotent_close", idempotent},
                   {"selection_matches", selection_matches},
                   {"before_close", diagnostics_json(before_close)},
                   {"after_close", diagnostics_json(after_close)},
                   {"after_second_close", diagnostics_json(after_second_close)}};
        if (!second) {
            row.emplace("second_owner_error", error_json(second.error()));
        }
        if (!first_close) {
            row.emplace("close_error", error_json(first_close.error()));
        }
        if (!second_close) {
            row.emplace("second_close_error", error_json(second_close.error()));
        }
        emit(std::move(row));
        require(passed, "Graphics lifecycle, selection or validation diagnostics did not satisfy "
                        "the smoke contract");
    }
    emit({{"kind", "complete"},
          {"status", validated ? "PASS" : "NOT_RUN"},
          {"execution_status", "PASS"},
          {"qualification", validated ? "PASS" : "NOT_RUN"},
          {"cycles", cycle_count},
          {"assignment_05", "NOT_RUN"}});
}
} // namespace

int main(int argc, char* argv[]) {
    try {
        run(parse(argc, argv));
        return 0;
    } catch (const Failure& error) {
        emit({{"kind", "failure"}, {"status", "FAIL"}, {"error", error_json(error.error())}});
    } catch (const std::exception& error) {
        emit({{"kind", "failure"}, {"status", "FAIL"}, {"message", error.what()}});
    }
    return 1;
}
