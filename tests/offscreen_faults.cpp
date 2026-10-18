#include <shark/client/graphics.hpp>

#include "engine/client/src/graphics_native_test.hpp"
#include "probes/graphics/report.hpp"

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
class TerminationObservation final {
  public:
    explicit TerminationObservation(const std::uint32_t& count) noexcept {
        teardown_count = &count;
    }
    ~TerminationObservation() noexcept {
        expected_termination = false;
        teardown_count = nullptr;
    }
    TerminationObservation(const TerminationObservation&) = delete;
    TerminationObservation& operator=(const TerminationObservation&) = delete;
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
    if (!std::cout) {
        throw std::runtime_error("Cannot write offscreen fault report");
    }
}
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <typename T> T take(shark::Result<T> result) {
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    return std::move(result).value();
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
            {"pending_queue_workers", value.pending_queue_workers}};
}
Object diagnostics_json(GraphicsDevice& owner) {
    const auto value = take(owner.diagnostics());
    Json::Array messages;
    for (const auto& message : value.messages) {
        messages.emplace_back(message);
    }
    return {{"warnings", value.warnings},
            {"errors", value.errors},
            {"dropped", value.dropped},
            {"messages", std::move(messages)}};
}
int run(std::string_view fault, const std::string& uuid, const std::filesystem::path& vertex,
        const std::filesystem::path& pixel) {
    OffscreenRequest request;
    request.extent = {32, 32};
    request.texture_extent = {1, 1};
    request.texture_rgba8 = {64, 128, 192, 255};
    request.vertices = {{{-0.75F, -0.75F, 0.5F}, {0, 1}},
                        {{0.75F, -0.75F, 0.5F}, {1, 1}},
                        {{0.0F, 0.75F, 0.5F}, {0.5F, 0}}};
    request.indices = {0, 1, 2};
    request.vertex_shader = take(load_shader_artifact(vertex, ShaderStage::vertex));
    request.pixel_shader = take(load_shader_artifact(pixel, ShaderStage::pixel));
    require(static_cast<bool>(validate_offscreen_request(request)), "Invalid fault fixture inputs");
    emit({{"kind", "identity"},
          {"schema_version", std::uint32_t{1}},
          {"fault", std::string(fault)},
          {"selection", uuid},
          {"validation", "required"},
          {"device_accessed", true},
          {"dependency_lock_sha256", SHARK_GRAPHICS_LOCK_SHA256},
          {"source_sha256", SHARK_GRAPHICS_SOURCE_SHA256},
          {"compiler", SHARK_GRAPHICS_COMPILER}});
    NativeGraphicsTestControl control;
    control.fault = fault == "partial-creation" ? NativeGraphicsFault::offscreen_partial_creation
                    : fault == "submission-timeout"
                        ? NativeGraphicsFault::offscreen_submission_timeout
                        : NativeGraphicsFault::offscreen_recording_error;
    TerminationObservation observation(control.teardown_calls);
    auto backend = make_fault_graphics_backend(control);
    auto* observed = backend.get();
    GraphicsConfig config;
    config.adapter_uuid = uuid;
    config.window.visible = false;
    auto owner = take(create_graphics(config, std::move(backend)));
    const auto adapter = take(owner->adapter());
    emit({{"kind", "ready"},
          {"adapter", Object{{"uuid", adapter.uuid},
                             {"name", adapter.name},
                             {"driver_name", adapter.driver_name},
                             {"driver_info", adapter.driver_info},
                             {"driver_version", adapter.driver_version}}},
          {"resources", resources_json(observed->presentation_resources())},
          {"diagnostics", diagnostics_json(*owner)}});
    control.armed = true;
    auto failed = owner->render_offscreen(request);
    require(!failed && control.fired, "Offscreen fault was not observed");
    emit({{"kind", "fault"},
          {"fired", control.fired},
          {"error", error_json(failed.error())},
          {"resources", resources_json(observed->presentation_resources())},
          {"diagnostics", diagnostics_json(*owner)},
          {"teardown_calls", control.teardown_calls}});
    if (fault == "recording-error") {
        const auto closed = owner->close();
        require(!closed && control.teardown_calls == 0 && observed->offscreen_pending(),
                "Unresolved recording was released");
        emit({{"kind", "retained"},
              {"error", error_json(closed.error())},
              {"resources", resources_json(observed->presentation_resources())},
              {"diagnostics", diagnostics_json(*owner)},
              {"teardown_calls", control.teardown_calls}});
        expected_termination = true;
        owner.reset();
        expected_termination = false;
        return 88;
    }
    if (fault == "submission-timeout") {
        auto repeated = owner->render_offscreen(request);
        auto presentation = owner->present();
        require(!repeated && repeated.error().code == shark::ErrorCode::conflict && !presentation &&
                    presentation.error().code == shark::ErrorCode::conflict,
                "Pending offscreen ownership allowed new work");
    }
    require(static_cast<bool>(owner->drain()),
            "Offscreen drain did not retire healthy-driver work");
    require(!observed->offscreen_pending(), "Drain retained completed offscreen ownership");
    emit({{"kind", "retired"},
          {"resources", resources_json(observed->presentation_resources())},
          {"diagnostics", diagnostics_json(*owner)},
          {"teardown_calls", control.teardown_calls}});
    const auto recovered = take(owner->render_offscreen(request));
    require(recovered.submitted > 0 && recovered.completed == recovered.submitted &&
                recovered.created_resources == 17 && recovered.retired_resources == 17 &&
                recovered.rgba8.size() == 4096 && recovered.depth.size() == 1024,
            "Recovered draw did not complete and retire its actual GPU resources");
    const auto center = std::size_t{16 * 32 + 16};
    require(recovered.rgba8[center * 4] == 64 && recovered.rgba8[center * 4 + 1] == 128 &&
                recovered.rgba8[center * 4 + 2] == 192 && recovered.rgba8[center * 4 + 3] == 255 &&
                recovered.depth[center] == 0.5F,
            "Recovered draw did not produce the known center color/depth");
    emit({{"kind", "recovered"},
          {"submitted", recovered.submitted},
          {"completed", recovered.completed},
          {"created_resources", recovered.created_resources},
          {"retired_resources", recovered.retired_resources},
          {"center_rgba8", Json::Array{64U, 128U, 192U, 255U}},
          {"center_depth", 0.5},
          {"diagnostics", diagnostics_json(*owner)}});
    require(static_cast<bool>(owner->close()), "Recovered offscreen owner did not close");
    emit({{"kind", "closed"},
          {"resources", resources_json(observed->presentation_resources())},
          {"diagnostics", diagnostics_json(*owner)},
          {"teardown_calls", control.teardown_calls}});
    require(control.teardown_calls == 5 && !observed->offscreen_pending(),
            "Incomplete device teardown");
    owner.reset();
    return 1; // Preserve the observed injected failure even after successful recovery.
}
} // namespace

int main(int argc, char** argv) {
    std::set_terminate(observe_termination);
    try {
        if (argc == 2 &&
            (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "--describe")) {
            emit({{"kind", "description"},
                  {"schema_version", std::uint32_t{1}},
                  {"device_accessed", false},
                  {"qualification", "NOT_RUN"},
                  {"faults",
                   Json::Array{"partial-creation", "submission-timeout", "recording-error"}}});
            return 0;
        }
        if (argc == 9 && std::string_view(argv[1]) == "--native-fault" &&
            std::string_view(argv[3]) == "--device" && std::string_view(argv[5]) == "--vertex" &&
            std::string_view(argv[7]) == "--pixel") {
            const std::string_view fault(argv[2]);
            const std::string uuid(argv[4]);
            if ((fault == "partial-creation" || fault == "submission-timeout" ||
                 fault == "recording-error") &&
                valid_graphics_uuid(uuid) && std::string_view(argv[6]).size() > 0 &&
                std::string_view(argv[8]).size() > 0) {
                const std::string_view vertex(argv[6]), pixel(argv[8]);
                return run(fault, uuid,
                           std::filesystem::path(std::u8string(vertex.begin(), vertex.end())),
                           std::filesystem::path(std::u8string(pixel.begin(), pixel.end())));
            }
        }
        std::cerr << "Expected --describe, --help, or --native-fault "
                     "partial-creation|submission-timeout|recording-error --device UUID --vertex "
                     "PATH --pixel PATH\n";
        return 2;
    } catch (const std::exception& error) {
        emit({{"kind", "unexpected_failure"}, {"message", std::string(error.what())}});
        return 4;
    }
}
