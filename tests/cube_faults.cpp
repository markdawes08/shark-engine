#include <shark/client/cube_sample.hpp>
#include <shark/client/graphics.hpp>
#include <shark/time.hpp>

#include "engine/client/src/graphics_native_test.hpp"
#include "probes/graphics/report.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace shark::client;
using namespace shark::client::detail;
using shark::probe::Json;
using Object = Json::Object;
bool expected_termination = false;
const std::uint32_t* teardown_count = nullptr;
struct TerminationObservation final {
    explicit TerminationObservation(const std::uint32_t& count) noexcept {
        teardown_count = &count;
    }
    ~TerminationObservation() noexcept {
        expected_termination = false;
        teardown_count = nullptr;
    }
};
[[noreturn]] void observe_termination() noexcept {
    std::printf("{\"kind\":\"terminated\",\"expected\":%s,\"teardown_calls\":%u}\n",
                expected_termination ? "true" : "false",
                expected_termination && teardown_count ? *teardown_count : 999U);
    std::fflush(stdout);
    std::_Exit(expected_termination ? 86 : 87);
}
void emit(Object value) {
    Json{std::move(value)}.write(std::cout);
    std::cout << '\n' << std::flush;
    if (!std::cout)
        throw std::runtime_error("Cannot write cube fault report");
}
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <typename T> T take(shark::Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    return std::move(result).value();
}
void take(shark::Result<void> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
}
Object error_json(const shark::Error& error) {
    return {{"code", std::string(shark::error_code_name(error.code))},
            {"message", error.message},
            {"operation", error.context.operation}};
}
Object resources_json(const GraphicsResourceSnapshot& value) {
    return {{"offscreen_resources", value.offscreen_resources},
            {"offscreen_submissions", value.offscreen_submissions},
            {"offscreen_recordings", value.offscreen_recordings},
            {"live_swapchains", value.live_swapchains},
            {"presentation_command_lists", value.command_lists},
            {"pending_queue_workers", value.pending_queue_workers},
            {"window_draw_resources", value.window_draw_resources},
            {"window_draw_submissions", value.window_draw_submissions},
            {"retained_recordings", value.retained_recordings}};
}
Object stats_json(GraphicsDevice& owner) {
    const auto value = take(owner.presentation_stats());
    return {{"submitted", value.submitted},
            {"completed", value.completed},
            {"presented", value.presented},
            {"generation", value.generation},
            {"retired_generations", value.retired_generations},
            {"active", value.active},
            {"closed", value.closed},
            {"faulted", value.faulted}};
}
Object diagnostics_json(GraphicsDevice& owner) {
    const auto value = take(owner.diagnostics());
    Json::Array messages;
    for (const auto& message : value.messages)
        messages.emplace_back(message);
    return {{"warnings", value.warnings},
            {"errors", value.errors},
            {"dropped", value.dropped},
            {"messages", std::move(messages)}};
}
shark::Result<PresentationReport> present_bounded(GraphicsDevice& owner) {
    shark::SteadyMonotonicClock clock;
    for (std::uint32_t attempt = 0; attempt < 64; ++attempt) {
        const auto window = take(owner.poll_window());
        require(!window.state.close_requested && !window.state.iconified,
                "Window fault fixture was externally interrupted");
        auto result = owner.present();
        if (!result || result.value().status == PresentationStatus::presented)
            return result;
        require(result.value().status == PresentationStatus::waiting ||
                    result.value().status == PresentationStatus::recreate_pending,
                "Window fault fixture received an unexpected presentation status");
        take(clock.wait_for(std::chrono::milliseconds{5}));
    }
    throw std::runtime_error("Window fault fixture exceeded its bounded presentation attempts");
}
void normalize_window(GraphicsDevice& owner) {
    constexpr WindowExtent extent{192, 192};
    take(owner.resize(extent));
    shark::SteadyMonotonicClock clock;
    for (std::uint32_t attempt = 0; attempt < 64; ++attempt) {
        const auto window = take(owner.poll_window());
        require(!window.state.close_requested && !window.state.iconified,
                "Window fault fixture was externally interrupted");
        if (window.state.content == extent && window.state.framebuffer == extent)
            return;
        take(clock.wait_for(std::chrono::milliseconds{5}));
    }
    throw std::runtime_error("Window fault fixture could not normalize its exact pixel extent");
}
int run(std::string_view fault, const std::string& uuid, const std::filesystem::path& path) {
    const auto bundle = take(load_shader_bundle(path));
    const auto request = take(make_cube_sample(bundle));
    emit({{"kind", "identity"},
          {"schema_version", 1U},
          {"fault", std::string(fault)},
          {"selection", uuid},
          {"validation", "required"},
          {"device_accessed", true},
          {"bundle_sha256", bundle.sha256},
          {"dependency_lock_sha256", SHARK_GRAPHICS_LOCK_SHA256},
          {"source_sha256", SHARK_GRAPHICS_SOURCE_SHA256},
          {"compiler", SHARK_GRAPHICS_COMPILER}});
    NativeGraphicsTestControl control;
    control.fault = fault == "partial-creation"  ? NativeGraphicsFault::window_draw_partial_creation
                    : fault == "capture-timeout" ? NativeGraphicsFault::window_capture_timeout
                                                 : NativeGraphicsFault::recording_error;
    TerminationObservation observation(control.teardown_calls);
    auto backend = make_fault_graphics_backend(control);
    auto* observed = backend.get();
    GraphicsConfig config;
    config.adapter_uuid = uuid;
    config.window.visible = false;
    config.window.content = {192, 192};
    auto owner = take(create_graphics(config, std::move(backend)));
    normalize_window(*owner);
    take(owner->set_presentation_draw(request, true));
    const auto adapter = take(owner->adapter());
    emit({{"kind", "ready"},
          {"adapter", Object{{"uuid", adapter.uuid},
                             {"name", adapter.name},
                             {"driver_name", adapter.driver_name},
                             {"driver_info", adapter.driver_info},
                             {"driver_version", adapter.driver_version}}},
          {"resources", resources_json(observed->presentation_resources())},
          {"stats", stats_json(*owner)},
          {"diagnostics", diagnostics_json(*owner)}});
    if (fault == "capture-timeout")
        take(present_bounded(*owner));
    control.armed = true;
    const auto failed = [&]() -> shark::Result<void> {
        if (fault == "capture-timeout") {
            auto result = owner->capture_presented();
            return result ? shark::Result<void>{} : shark::Result<void>{result.error()};
        }
        auto result = present_bounded(*owner);
        return result ? shark::Result<void>{} : shark::Result<void>{result.error()};
    }();
    require(!failed && control.fired, "Configured window fault was not observed");
    emit({{"kind", "fault"},
          {"fired", control.fired},
          {"error", error_json(failed.error())},
          {"resources", resources_json(observed->presentation_resources())},
          {"stats", stats_json(*owner)},
          {"diagnostics", diagnostics_json(*owner)},
          {"teardown_calls", control.teardown_calls}});
    if (fault == "recording-error") {
        const auto closed = owner->close();
        require(!closed && control.teardown_calls == 0 &&
                    observed->presentation_resources().retained_recordings == 1,
                "Unresolved window recording was released");
        require(closed.error().context.operation == failed.error().context.operation &&
                    closed.error().message == failed.error().message,
                "Cleanup replaced the original recording failure");
        emit({{"kind", "retained"},
              {"error", error_json(closed.error())},
              {"resources", resources_json(observed->presentation_resources())},
              {"stats", stats_json(*owner)},
              {"diagnostics", diagnostics_json(*owner)},
              {"teardown_calls", control.teardown_calls}});
        expected_termination = true;
        owner.reset();
        expected_termination = false;
        return 88;
    }
    if (fault == "capture-timeout") {
        take(owner->drain());
        emit({{"kind", "drained"},
              {"resources", resources_json(observed->presentation_resources())},
              {"stats", stats_json(*owner)},
              {"diagnostics", diagnostics_json(*owner)},
              {"teardown_calls", control.teardown_calls}});
        const auto recovered = take(owner->capture_presented());
        require(recovered.extent == WindowExtent{192, 192} &&
                    recovered.color_encoding == ReadbackColorEncoding::srgb &&
                    recovered.submitted > 0 && recovered.completed == recovered.submitted &&
                    recovered.created_resources == 15 && recovered.retired_resources == 0 &&
                    recovered.rgba8.size() == 192 * 192 * 4 && recovered.depth.size() == 192 * 192,
                "Capture recovery did not retain and read actual presentation resources");
        constexpr auto center = std::size_t{96 * 192 + 96};
        Json::Array color;
        for (std::size_t channel = 0; channel < 4; ++channel)
            color.emplace_back(static_cast<std::uint32_t>(recovered.rgba8[center * 4 + channel]));
        require(recovered.rgba8[center * 4] == 255 &&
                    std::abs(static_cast<int>(recovered.rgba8[center * 4 + 1]) - 99) <= 1 &&
                    std::abs(static_cast<int>(recovered.rgba8[center * 4 + 2]) - 71) <= 1 &&
                    recovered.rgba8[center * 4 + 3] == 255 &&
                    std::abs(recovered.depth[center] - 0.2687578548F) <= 0.00001F,
                "Recovered window readback failed fixed center color/depth expectations");
        emit({{"kind", "recovered"},
              {"submitted", recovered.submitted},
              {"completed", recovered.completed},
              {"created_resources", recovered.created_resources},
              {"retired_resources", recovered.retired_resources},
              {"center_rgba8", std::move(color)},
              {"center_depth", static_cast<double>(recovered.depth[center])},
              {"diagnostics", diagnostics_json(*owner)}});
    }
    const auto closed = owner->close();
    require(static_cast<bool>(closed) == (fault == "capture-timeout"),
            "Close did not preserve the original presentation failure state");
    auto record = Object{{"kind", "closed"},
                         {"resources", resources_json(observed->presentation_resources())},
                         {"stats", stats_json(*owner)},
                         {"diagnostics", diagnostics_json(*owner)},
                         {"teardown_calls", control.teardown_calls}};
    if (!closed)
        record.emplace("error", error_json(closed.error()));
    emit(std::move(record));
    require(control.teardown_calls == 7, "Incomplete window/device teardown");
    owner.reset();
    return 1;
}
} // namespace
int main(int argc, char** argv) {
    std::set_terminate(observe_termination);
    try {
        if (argc == 2 &&
            (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "--describe")) {
            emit({{"kind", "description"},
                  {"schema_version", 1U},
                  {"device_accessed", false},
                  {"qualification", "NOT_RUN"},
                  {"faults",
                   Json::Array{"partial-creation", "capture-timeout", "recording-error"}}});
            return 0;
        }
        if (argc == 7 && std::string_view(argv[1]) == "--native-fault" &&
            std::string_view(argv[3]) == "--device" && std::string_view(argv[5]) == "--bundle") {
            const std::string_view fault(argv[2]), path(argv[6]);
            const std::string uuid(argv[4]);
            if ((fault == "partial-creation" || fault == "capture-timeout" ||
                 fault == "recording-error") &&
                valid_graphics_uuid(uuid) && !path.empty())
                return run(fault, uuid,
                           std::filesystem::path(std::u8string(path.begin(), path.end())));
        }
        std::cerr
            << "Expected --describe, --help, or --native-fault "
               "partial-creation|capture-timeout|recording-error --device UUID --bundle PATH\n";
        return 2;
    } catch (const std::exception& error) {
        emit({{"kind", "unexpected_failure"}, {"message", std::string(error.what())}});
        return 4;
    }
}
