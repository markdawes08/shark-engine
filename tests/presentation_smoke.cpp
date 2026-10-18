#include <shark/client/graphics.hpp>
#include <shark/time.hpp>

#include "probes/graphics/report.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace shark::client;
using shark::probe::Json;
using Array = Json::Array;
using Object = Json::Object;
constexpr std::uint32_t cycle_count = 4;
constexpr std::uint32_t frames_per_cycle = 96;
constexpr std::uint32_t attempt_limit = 16;
constexpr std::array sizes{WindowExtent{700, 420}, WindowExtent{530, 350}, WindowExtent{800, 450},
                           WindowExtent{640, 400}};

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
            {shark::ErrorCode::execution_failed, message, {.operation = "presentation-smoke"}});
    }
}

template <typename T> T take(shark::Result<T> result) {
    if (!result) {
        throw Failure(std::move(result).error());
    }
    return std::move(result).value();
}

void check(shark::Result<void> result) {
    if (!result) {
        throw Failure(std::move(result).error());
    }
}

void emit(Object record) {
    Json{std::move(record)}.write(std::cout);
    std::cout << '\n' << std::flush;
    if (!std::cout) {
        throw std::runtime_error("Cannot write presentation smoke report");
    }
}

Object error_json(const shark::Error& error) {
    return {{"code", std::string(shark::error_code_name(error.code))},
            {"message", error.message},
            {"operation", error.context.operation}};
}

Object extent_json(WindowExtent extent) {
    return {{"width", extent.width}, {"height", extent.height}};
}

Object window_json(const WindowState& state) {
    return {{"content", extent_json(state.content)},
            {"framebuffer", extent_json(state.framebuffer)},
            {"visible", state.visible},
            {"iconified", state.iconified},
            {"close_requested", state.close_requested}};
}

Object stats_json(const PresentationStats& stats) {
    return {{"synchronization", stats.synchronization == PresentationSync::present_fence
                                    ? "present_fence"
                                    : "host_completion"},
            {"extent", extent_json(stats.extent)},
            {"image_count", stats.image_count},
            {"frame_limit", stats.frame_limit},
            {"in_flight", stats.in_flight},
            {"peak_in_flight", stats.peak_in_flight},
            {"submitted", stats.submitted},
            {"completed", stats.completed},
            {"presented", stats.presented},
            {"generation", stats.generation},
            {"retired_generations", stats.retired_generations},
            {"suspended_frames", stats.suspended_frames},
            {"waiting_frames", stats.waiting_frames},
            {"active", stats.active},
            {"faulted", stats.faulted},
            {"closed", stats.closed}};
}

std::string serialized_stats(const PresentationStats& stats) {
    std::ostringstream stream;
    Json{stats_json(stats)}.write(stream);
    return stream.str();
}

Array strings(const std::vector<std::string>& values) {
    Array result;
    for (const auto& value : values)
        result.emplace_back(value);
    return result;
}

Object diagnostics_json(const GraphicsDiagnostics& diagnostics) {
    return {{"warnings", diagnostics.warnings},
            {"errors", diagnostics.errors},
            {"dropped", diagnostics.dropped},
            {"messages", strings(diagnostics.messages)}};
}

bool clean(const GraphicsDiagnostics& diagnostics) {
    return diagnostics.warnings == 0 && diagnostics.errors == 0 && diagnostics.dropped == 0 &&
           diagnostics.messages.empty();
}

template <typename T, std::size_t Size>
Object named_values(const std::array<const char*, Size>& names, const std::array<T, Size>& values) {
    Object result;
    for (std::size_t index = 0; index < Size; ++index)
        result.emplace(names[index], values[index]);
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
    constexpr std::array features{"vulkan_1_3",          "timeline_semaphore",
                                  "synchronization2",    "dynamic_rendering",
                                  "swapchain_extension", "graphics_compute_present_queue",
                                  "srgb_surface",        "fifo_present"};
    constexpr std::array formats{"rgba8_srgb", "rgba8_unorm", "rgba16_float",
                                 "d32_float_depth_array"};
    constexpr std::array limits{
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
            {"features", named_values(features, adapter.capabilities.features)},
            {"formats", named_values(formats, adapter.capabilities.formats)},
            {"limits", named_values(limits, adapter.capabilities.limits)},
            {"missing_requirements", strings(missing_graphics_requirements(adapter))}};
}

const char* status_name(PresentationStatus status) {
    switch (status) {
    case PresentationStatus::presented:
        return "presented";
    case PresentationStatus::suspended:
        return "suspended";
    case PresentationStatus::waiting:
        return "waiting";
    case PresentationStatus::recreate_pending:
        return "recreate_pending";
    case PresentationStatus::closed:
        return "closed";
    }
    return "unknown";
}

WindowState poll(GraphicsDevice& device) {
    const auto frame = take(device.poll_window());
    require(frame.diagnostics.rejected_input_events == 0 &&
                frame.diagnostics.invalid_window_events == 0 && !frame.diagnostics.input_error &&
                !frame.diagnostics.window_error,
            "Native window callback diagnostics must remain clean");
    require(!frame.state.close_requested, "A close request leaves the workload incomplete");
    return frame.state;
}

template <typename Predicate> WindowState await_state(GraphicsDevice& device, Predicate predicate) {
    shark::SteadyMonotonicClock clock;
    const auto deadline = clock.now() + std::chrono::seconds{2};
    while (clock.now() < deadline) {
        const auto state = poll(device);
        if (predicate(state))
            return state;
        check(clock.wait_for(std::chrono::milliseconds{5}));
    }
    throw Failure({shark::ErrorCode::execution_failed,
                   "Native window state deadline expired",
                   {.operation = "presentation-smoke.window"}});
}

void check_stats(const PresentationStats& stats) {
    require(!stats.faulted && stats.completed <= stats.submitted &&
                stats.presented <= stats.submitted && stats.frame_limit >= 1 &&
                stats.frame_limit <= 2 && stats.in_flight <= stats.frame_limit &&
                stats.peak_in_flight <= stats.frame_limit &&
                stats.retired_generations <= stats.generation,
            "Presentation counters or frame bounds are inconsistent");
}

void frames(GraphicsDevice& device, std::uint32_t cycle, std::string stage, std::uint32_t count,
            std::uint32_t& presented) {
    shark::SteadyMonotonicClock clock;
    for (std::uint32_t frame = 1; frame <= count; ++frame) {
        bool complete = false;
        for (std::uint32_t attempt = 1; attempt <= attempt_limit; ++attempt) {
            const auto window = poll(device);
            require(!window.iconified, "Window was minimized outside the declared workload");
            const auto before = take(device.presentation_stats());
            const auto report = take(device.present());
            emit({{"kind", "frame"},
                  {"cycle", cycle},
                  {"stage", stage},
                  {"frame", frame},
                  {"attempt", attempt},
                  {"status", status_name(report.status)},
                  {"recreated", report.recreated},
                  {"window", window_json(window)},
                  {"stats", stats_json(report.stats)}});
            check_stats(report.stats);
            require(report.stats.submitted >= before.submitted &&
                        report.stats.completed >= before.completed &&
                        report.stats.generation >= before.generation,
                    "Presentation counters must not regress");
            if (report.status == PresentationStatus::presented) {
                require(report.stats.presented == before.presented + 1 &&
                            report.stats.extent == window.framebuffer,
                        "A presented frame must advance the count and match the real framebuffer");
                ++presented;
                complete = true;
                break;
            }
            require(report.status == PresentationStatus::waiting ||
                        report.status == PresentationStatus::recreate_pending,
                    "Visible presentation unexpectedly stopped or suspended");
            check(clock.wait_for(std::chrono::milliseconds{5}));
        }
        require(complete, "Presentation exhausted the declared 16-attempt frame budget");
    }
}

struct Options {
    std::string_view mode{"describe"};
    GraphicsConfig config;
};

Options parse(int argc, char* argv[]) {
    Options options;
    options.config.window.title = "Shark 05b presentation - \u6d4b\u8bd5";
    options.config.window.content = {640, 400};
    const auto invalid = [] {
        throw Failure({shark::ErrorCode::invalid_argument,
                       "Use --help, --describe, or --smoke [--device auto|<32 lowercase hex UUID>] "
                       "[--validation required|off]",
                       {.operation = "presentation-smoke.arguments"}});
    };
    if (argc > 6)
        invalid();
    if (argc > 1) {
        const std::string_view mode{argv[1]};
        if (mode == "--help" || mode == "--describe" || mode == "--smoke")
            options.mode = mode.substr(2);
        else
            invalid();
    }
    bool device_seen = false;
    bool validation_seen = false;
    for (int index = 2; index < argc; index += 2) {
        if (index + 1 >= argc || options.mode != "smoke")
            invalid();
        const std::string_view option{argv[index]};
        const std::string_view value{argv[index + 1]};
        if (option == "--device" && !device_seen) {
            device_seen = true;
            if (value != "auto") {
                if (!valid_graphics_uuid(value))
                    invalid();
                options.config.adapter_uuid = value;
            }
        } else if (option == "--validation" && !validation_seen &&
                   (value == "required" || value == "off")) {
            validation_seen = true;
            options.config.validation =
                value == "required" ? GraphicsValidation::required : GraphicsValidation::off;
        } else
            invalid();
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
    if (options.mode != "smoke") {
        emit({{"kind", "description"},
              {"status", "PASS"},
              {"qualification", "NOT_RUN"},
              {"native_graphics_available", native_graphics_available()},
              {"device_accessed", false},
              {"cycles", cycle_count},
              {"frames_per_cycle", frames_per_cycle},
              {"resizes_per_cycle", 4U},
              {"minimizes_per_cycle", 2U},
              {"attempt_limit", attempt_limit},
              {"usage",
               "--help | --describe | --smoke [--device auto|UUID] [--validation required|off]"},
              {"scope", "Presentation clears, synchronization and lifetime; no shaders, scene, "
                        "readback or authoring"}});
        return;
    }
    for (std::uint32_t cycle = 1; cycle <= cycle_count; ++cycle) {
        auto device = take(GraphicsDevice::create(options.config));
        try {
            const auto adapter = take(device->adapter());
            // Win32 applies initial monitor DPI scaling. Normalize the native
            // client extent before the first swapchain exists; this is setup,
            // not one of the four measured generation transitions below.
            check(device->resize(options.config.window.content));
            const auto initial = await_state(*device, [&](const auto& state) {
                return state.content == options.config.window.content && state.visible &&
                       !state.iconified && state.framebuffer.width && state.framebuffer.height;
            });
            require(options.config.adapter_uuid.empty() ||
                        adapter.uuid == options.config.adapter_uuid,
                    "Explicit adapter selection must match");
            emit({{"kind", "cycle_begin"},
                  {"cycle", cycle},
                  {"adapter", adapter_json(adapter)},
                  {"window", window_json(initial)},
                  {"stats", stats_json(take(device->presentation_stats()))}});
            std::uint32_t presented = 0;
            frames(*device, cycle, "initial", 16, presented);
            const auto second_owner = GraphicsDevice::create(options.config);
            const bool second_rejected =
                !second_owner && second_owner.error().code == shark::ErrorCode::conflict;
            Object conflict{{"kind", "owner_conflict"},
                            {"cycle", cycle},
                            {"second_owner_rejected", second_rejected},
                            {"stats", stats_json(take(device->presentation_stats()))}};
            if (!second_owner) {
                conflict.emplace("error", error_json(second_owner.error()));
            }
            emit(std::move(conflict));
            require(second_rejected,
                    "A second graphics owner must not replace active presentation");
            for (std::uint32_t index = 0; index < sizes.size(); ++index) {
                const auto before = take(device->presentation_stats());
                check(device->resize(sizes[index]));
                const auto window = await_state(*device, [size = sizes[index]](const auto& state) {
                    return state.content == size && !state.iconified && state.framebuffer.width &&
                           state.framebuffer.height;
                });
                emit({{"kind", "resize"},
                      {"cycle", cycle},
                      {"index", index + 1},
                      {"requested", extent_json(sizes[index])},
                      {"window", window_json(window)},
                      {"stats", stats_json(before)}});
                frames(*device, cycle, "resize" + std::to_string(index + 1), 16, presented);
                require(take(device->presentation_stats()).generation > before.generation,
                        "A framebuffer extent change must replace the swapchain generation");
                if (index == 1 || index == 3) {
                    const auto minimize = (index + 1) / 2;
                    const auto before_minimize = take(device->presentation_stats());
                    check(device->iconify());
                    const auto iconified =
                        await_state(*device, [](const auto& state) { return state.iconified; });
                    const auto suspended = take(device->present());
                    emit({{"kind", "minimize"},
                          {"cycle", cycle},
                          {"index", minimize},
                          {"window", window_json(iconified)},
                          {"status", status_name(suspended.status)},
                          {"before", stats_json(before_minimize)},
                          {"stats", stats_json(suspended.stats)}});
                    require(suspended.status == PresentationStatus::suspended &&
                                suspended.stats.submitted == before_minimize.submitted &&
                                suspended.stats.presented == before_minimize.presented &&
                                suspended.stats.suspended_frames ==
                                    before_minimize.suspended_frames + 1,
                            "Minimize must suspend without submitting or presenting");
                    check(device->restore());
                    const auto restored = await_state(*device, [](const auto& state) {
                        return !state.iconified && state.framebuffer.width &&
                               state.framebuffer.height;
                    });
                    emit({{"kind", "restore"},
                          {"cycle", cycle},
                          {"index", minimize},
                          {"window", window_json(restored)},
                          {"stats", stats_json(take(device->presentation_stats()))}});
                    frames(*device, cycle, "restore" + std::to_string(minimize), 8, presented);
                }
            }
            require(presented == frames_per_cycle, "All declared presented frames must finish");
            const auto before_drain = take(device->presentation_stats());
            check(device->drain());
            const auto drained = take(device->presentation_stats());
            require(drained.completed == drained.submitted && drained.in_flight == 0,
                    "Explicit drain must complete all submitted GPU uses");
            const auto before_close = take(device->diagnostics());
            check(device->close());
            const auto closed = take(device->presentation_stats());
            const auto after_close = take(device->diagnostics());
            check(device->close());
            const auto after_second_close = take(device->diagnostics());
            const auto second_closed = take(device->presentation_stats());
            const bool idempotent = serialized_stats(closed) == serialized_stats(second_closed) &&
                                    after_close.warnings == after_second_close.warnings &&
                                    after_close.errors == after_second_close.errors &&
                                    after_close.dropped == after_second_close.dropped &&
                                    after_close.messages == after_second_close.messages;
            const bool passed =
                closed.closed && !closed.active && !closed.faulted &&
                closed.completed == closed.submitted && closed.in_flight == 0 &&
                closed.retired_generations == closed.generation && closed.generation >= 5 &&
                closed.presented == frames_per_cycle && idempotent &&
                (!validated ||
                 (clean(before_close) && clean(after_close) && clean(after_second_close)));
            emit({{"kind", "cycle_end"},
                  {"cycle", cycle},
                  {"status", passed ? (validated ? "PASS" : "DIAGNOSTIC") : "FAIL"},
                  {"presented_frames", presented},
                  {"idempotent_close", idempotent},
                  {"before_drain", stats_json(before_drain)},
                  {"drained", stats_json(drained)},
                  {"closed", stats_json(closed)},
                  {"second_closed", stats_json(second_closed)},
                  {"before_close", diagnostics_json(before_close)},
                  {"after_close", diagnostics_json(after_close)},
                  {"after_second_close", diagnostics_json(after_second_close)}});
            require(passed,
                    "Presentation teardown or validation did not satisfy the smoke contract");
        } catch (...) {
            const auto primary = std::current_exception();
            try {
                const auto failed_stats = take(device->presentation_stats());
                const auto before = take(device->diagnostics());
                Object failure{{"kind", "cycle_failure"},
                               {"cycle", cycle},
                               {"status", "FAIL"},
                               {"failed_stats", stats_json(failed_stats)},
                               {"before_close", diagnostics_json(before)}};
                try {
                    std::rethrow_exception(primary);
                } catch (const Failure& error) {
                    failure.emplace("error", error_json(error.error()));
                } catch (const std::exception& error) {
                    failure.emplace("message", error.what());
                } catch (...) {
                    failure.emplace("message", "Unknown presentation exception");
                }
                // Flush primary diagnostics before any potentially stalled native teardown.
                emit(std::move(failure));
                const auto closed = device->close();
                Object cleanup{{"kind", "cycle_failure_cleanup"},
                               {"cycle", cycle},
                               {"status", "FAIL"},
                               {"close_succeeded", static_cast<bool>(closed)},
                               {"after_close", diagnostics_json(take(device->diagnostics()))},
                               {"closed_stats", stats_json(take(device->presentation_stats()))}};
                if (!closed) {
                    cleanup.emplace("close_error", error_json(closed.error()));
                }
                emit(std::move(cleanup));
            } catch (...) {
                // Preserve the primary failure even if reporting itself fails.
            }
            std::rethrow_exception(primary);
        }
    }
    emit({{"kind", "complete"},
          {"status", validated ? "PASS" : "NOT_RUN"},
          {"qualification", validated ? "PASS" : "NOT_RUN"},
          {"execution_status", "PASS"},
          {"cycles", cycle_count},
          {"presented_frames", cycle_count * frames_per_cycle},
          {"resize_transitions", cycle_count * 4},
          {"minimize_restore_cycles", cycle_count * 2},
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
