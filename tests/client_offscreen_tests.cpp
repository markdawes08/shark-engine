#include <shark/client/graphics.hpp>

#include "../engine/client/src/graphics_backend.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace shark::client::test {
OffscreenRequest offscreen_request_fixture();
SceneViewportRequest scene_viewport_request_fixture();
} // namespace shark::client::test

namespace {
using namespace shark::client;
using shark::Error;
using shark::ErrorCode;
using shark::Result;

struct OffscreenState {
    std::uint32_t renders{0};
    std::uint32_t drains{0};
    std::uint32_t presentations{0};
    std::uint32_t destroys{0};
    std::uint64_t timeout{0};
    bool pending{false};
    bool complete{false};
    bool fail_render{false};
    bool throw_render{false};
    bool validate_error{false};
    std::uint32_t configurations{0};
    std::uint32_t captures{0};
    bool fail_configuration{false};
    bool throw_configuration{false};
    bool fail_capture{false};
    bool throw_capture{false};
    bool capture_enabled{false};
    std::optional<OffscreenRequest> draw;
    std::optional<SceneViewportRequest> scene;
    WindowExtent framebuffer{640, 480};
    bool wait_present{false};
    std::uint32_t retired_swapchains{0};
    SceneViewportStats viewport;
    detail::GraphicsDiagnosticSink* sink{nullptr};
};

struct CompleteAtExit {
    OffscreenState& state;
    ~CompleteAtExit() {
        state.complete = true;
    }
};

class OffscreenBackend final : public detail::GraphicsBackend {
  public:
    explicit OffscreenBackend(OffscreenState& state) : state_(state) {}
    Result<void> create_window(const WindowConfig&) override {
        return {};
    }
    Result<void> create_instance(GraphicsValidation,
                                 detail::GraphicsDiagnosticSink& sink) override {
        state_.sink = &sink;
        return {};
    }
    Result<void> create_surface() override {
        return {};
    }
    Result<std::vector<GraphicsAdapter>> enumerate_adapters() override {
        GraphicsAdapter adapter;
        adapter.uuid = "0123456789abcdef0123456789abcdef";
        adapter.name = "CPU offscreen owner fixture";
        adapter.driver_name = "No Vulkan initialized";
        adapter.kind = AdapterKind::discrete;
        adapter.capabilities.features.fill(true);
        adapter.capabilities.formats.fill(true);
        adapter.capabilities.limits = {4096, 2048, 12, 4, 4, 16, 16, 16384, 134217728, 128};
        return std::vector{adapter};
    }
    Result<void> create_device(std::size_t) override {
        return {};
    }
    Result<void> create_nvrhi(detail::GraphicsDiagnosticSink&) override {
        return {};
    }
    Result<OffscreenReadback> render_offscreen(const OffscreenRequest& request) override {
        ++state_.renders;
        if (state_.pending) {
            return Error{
                ErrorCode::conflict, "Pending fixture resources", {"test.offscreen.pending"}};
        }
        if (state_.throw_render) {
            state_.pending = true;
            throw std::runtime_error("CPU injected offscreen recording failure");
        }
        if (state_.fail_render) {
            state_.pending = true;
            return Error{ErrorCode::execution_failed,
                         "CPU injected offscreen timeout",
                         {"test.offscreen.wait", "owned-request"}};
        }
        if (state_.validate_error) {
            state_.sink->record(true, "CPU injected offscreen validation error");
        }
        OffscreenReadback result;
        result.extent = request.extent;
        result.rgba8 = request.texture_rgba8;
        result.depth = {0.25F};
        result.submitted = 1;
        result.completed = 1;
        result.created_resources = 12;
        result.retired_resources = 12;
        return result;
    }
    bool offscreen_pending() const noexcept override {
        return state_.pending;
    }
    Result<SceneViewportReadback>
    render_scene_offscreen(const SceneViewportRequest& request) override {
        // Reuse the existing owner fault seam; this fixture initializes no GPU.
        OffscreenRequest upload;
        upload.extent = request.extent;
        upload.texture_rgba8 = {255, 128, 64, 255};
        auto pixels = render_offscreen(upload);
        if (!pixels)
            return pixels.error();
        state_.viewport = {request.extent, 1, 0, 1, 5, false};
        return SceneViewportReadback{std::move(pixels).value(), state_.viewport,
                                     request.scene_revision, request.origin,
                                     static_cast<std::uint32_t>(request.draws.size())};
    }
    SceneViewportStats scene_viewport_stats() const noexcept override {
        auto result = state_.viewport;
        result.pending = state_.pending;
        return result;
    }
    Result<void> set_presentation_draw(const OffscreenRequest& request, bool capture) override {
        ++state_.configurations;
        if (state_.throw_configuration) {
            throw std::runtime_error("CPU injected configuration failure");
        }
        if (state_.fail_configuration) {
            return Error{ErrorCode::execution_failed,
                         "CPU injected configuration failure",
                         {"test.window_draw.configure", "owned-request"}};
        }
        state_.draw = request;
        state_.capture_enabled = capture;
        return {};
    }
    Result<void> set_presentation_scene(const SceneViewportRequest& request,
                                        bool capture) override {
        ++state_.configurations;
        if (state_.throw_configuration)
            throw std::runtime_error("CPU injected scene configuration failure");
        if (state_.fail_configuration)
            return Error{ErrorCode::execution_failed,
                         "CPU injected scene configuration failure",
                         {"test.window_scene.configure", "owned-request"}};
        state_.scene = request;
        state_.capture_enabled = capture;
        return {};
    }
    Result<OffscreenReadback> capture_presented() override {
        ++state_.captures;
        if (state_.throw_capture) {
            throw std::runtime_error("CPU injected capture failure");
        }
        if (state_.fail_capture) {
            return Error{ErrorCode::execution_failed,
                         "CPU injected capture timeout",
                         {"test.window_draw.capture", "owned-request"}};
        }
        if (state_.validate_error) {
            state_.sink->record(true, "CPU injected capture validation error");
        }
        OffscreenReadback result;
        result.color_encoding = ReadbackColorEncoding::srgb;
        result.rgba8 = state_.scene ? std::vector<std::uint8_t>{255, 128, 64, 255}
                                    : state_.draw->texture_rgba8;
        result.depth = {0.25F};
        result.submitted = 2;
        result.completed = 2;
        result.created_resources = 15;
        return result;
    }
    Result<WindowState> window_state() override {
        return WindowState{.content = state_.framebuffer, .framebuffer = state_.framebuffer};
    }
    Result<PresentationSync> create_presentation() override {
        ++state_.presentations;
        return PresentationSync::host_completion;
    }
    Result<std::optional<detail::SwapchainInfo>> create_swapchain(WindowExtent extent) override {
        return std::optional{detail::SwapchainInfo{extent, 2}};
    }
    Result<bool> wait_slot(std::uint32_t, std::uint64_t) override {
        return true;
    }
    Result<detail::AcquiredImage> acquire(std::uint32_t, std::uint64_t) override {
        return detail::AcquiredImage{detail::AcquireStatus::ready, 0};
    }
    Result<void> submit(std::uint32_t, std::uint32_t, ClearColor) override {
        return {};
    }
    Result<detail::PresentStatus> present(std::uint32_t, std::uint32_t, std::uint64_t) override {
        return state_.wait_present ? detail::PresentStatus::waiting
                                   : detail::PresentStatus::presented;
    }
    void destroy_swapchain() noexcept override {
        ++state_.retired_swapchains;
    }
    Result<detail::DrainStatus> drain_presentation(std::uint64_t timeout) override {
        ++state_.drains;
        state_.timeout = timeout;
        if (!state_.complete) {
            return detail::DrainStatus::waiting;
        }
        state_.pending = false;
        return detail::DrainStatus::complete;
    }
    void destroy_nvrhi() noexcept override {
        ++state_.destroys;
    }
    void destroy_device() noexcept override {
        ++state_.destroys;
    }
    void destroy_surface() noexcept override {
        ++state_.destroys;
    }
    void destroy_instance() noexcept override {
        ++state_.destroys;
    }
    void destroy_window() noexcept override {
        ++state_.destroys;
    }

  private:
    OffscreenState& state_;
};

std::unique_ptr<GraphicsDevice> create(OffscreenState& state) {
    auto result = detail::create_graphics({}, std::make_unique<OffscreenBackend>(state));
    if (!result) {
        throw std::runtime_error(result.error().message);
    }
    return std::move(result).value();
}
} // namespace

TEST_CASE("Offscreen owner rejects invalid requests and thread access before backend calls",
          "[offscreen]") {
    OffscreenState state;
    auto device = create(state);
    auto request = test::offscreen_request_fixture();
    request.indices.back() = 100;
    const auto invalid = device->render_offscreen(request);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == ErrorCode::invalid_argument);
    CHECK(state.renders == 0);
    request = test::offscreen_request_fixture();
    std::optional<Result<OffscreenReadback>> wrong_thread;
    std::thread worker([&] { wrong_thread = device->render_offscreen(request); });
    worker.join();
    REQUIRE(wrong_thread.has_value());
    REQUIRE_FALSE(*wrong_thread);
    CHECK(wrong_thread->error().code == ErrorCode::conflict);
    CHECK(state.renders == 0);
    REQUIRE(device->close());
    const auto closed = device->render_offscreen(request);
    REQUIRE_FALSE(closed);
    CHECK(closed.error().context.operation == "graphics.closed");
    CHECK(state.renders == 0);
    CHECK(state.destroys == 5);
    CHECK(state.presentations == 0);
}

TEST_CASE("Offscreen readback owns returned storage and does not initialize presentation",
          "[offscreen]") {
    OffscreenState state;
    auto device = create(state);
    auto request = test::offscreen_request_fixture();
    auto result = device->render_offscreen(request);
    REQUIRE(result);
    request.texture_rgba8[0] = 0;
    REQUIRE(device->close());
    CHECK(result.value().rgba8.front() == 255);
    CHECK(result.value().depth.front() == 0.25F);
    CHECK(result.value().submitted == result.value().completed);
    CHECK(result.value().created_resources == result.value().retired_resources);
    CHECK(state.renders == 1);
    CHECK(state.drains == 0);
    CHECK(state.presentations == 0);
}

TEST_CASE("Offscreen pending resources survive close until bounded completion retry",
          "[offscreen]") {
    OffscreenState state;
    state.fail_render = true;
    auto device = create(state);
    const CompleteAtExit completion_guard{state};
    const auto request = test::offscreen_request_fixture();
    const auto result = device->render_offscreen(request);
    REQUIRE_FALSE(result);
    CHECK(result.error().context.request_id == "owned-request");
    CHECK(state.pending);
    CHECK_FALSE(device->present());
    CHECK(state.presentations == 0);
    CHECK_FALSE(device->close());
    CHECK(state.pending);
    CHECK(state.destroys == 0);
    CHECK(state.drains == 1);
    CHECK(state.timeout == 1'000'000'000);
    CHECK_FALSE(device->render_offscreen(request));
    state.complete = true;
    REQUIRE(device->drain());
    CHECK_FALSE(state.pending);
    REQUIRE(device->close());
    CHECK(state.destroys == 5);
    CHECK(state.presentations == 0);
}

TEST_CASE("Offscreen exceptions retain pending owners and become owned failures", "[offscreen]") {
    OffscreenState state;
    state.throw_render = true;
    auto device = create(state);
    const CompleteAtExit completion_guard{state};
    auto result = device->render_offscreen(test::offscreen_request_fixture());
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::execution_failed);
    CHECK(result.error().context.operation == "graphics.offscreen.render");
    CHECK(state.pending);
    CHECK(state.destroys == 0);
    // Release the controlled fixture before an assertion can leave live owners.
    state.complete = true;
    REQUIRE(device->close());
    CHECK(state.destroys == 5);
}

TEST_CASE("Offscreen validation failures do not return successful readback", "[offscreen]") {
    OffscreenState state;
    state.validate_error = true;
    auto device = create(state);
    auto result = device->render_offscreen(test::offscreen_request_fixture());
    REQUIRE_FALSE(result);
    CHECK(result.error().context.operation == "graphics.validation");
    const auto second = device->render_offscreen(test::offscreen_request_fixture());
    REQUIRE_FALSE(second);
    CHECK(state.renders == 1);
    CHECK_FALSE(device->close());
    CHECK(state.destroys == 5);
}

TEST_CASE("Window draw validates owned inputs and lifecycle before backend configuration",
          "[offscreen]") {
    OffscreenState state;
    auto device = create(state);
    auto request = test::offscreen_request_fixture();
    request.indices.back() = 100;
    CHECK_FALSE(device->set_presentation_draw(request));
    CHECK(state.configurations == 0);
    request = test::offscreen_request_fixture();
    std::optional<Result<void>> wrong_thread;
    std::thread worker([&] { wrong_thread = device->set_presentation_draw(request); });
    worker.join();
    REQUIRE(wrong_thread);
    REQUIRE_FALSE(*wrong_thread);
    CHECK(wrong_thread->error().code == ErrorCode::conflict);
    CHECK(state.configurations == 0);
    REQUIRE(device->set_presentation_draw(request, true));
    request.texture_rgba8.front() = 0;
    REQUIRE(state.draw);
    CHECK(state.draw->texture_rgba8.front() == 255);
    CHECK(state.capture_enabled);
    CHECK_FALSE(device->set_presentation_draw(request));
    CHECK(state.configurations == 1);
    CHECK_FALSE(device->capture_presented());
    CHECK(state.captures == 0);
    REQUIRE(device->close());
    CHECK_FALSE(device->set_presentation_draw(request));
    CHECK_FALSE(device->capture_presented());
    CHECK(state.configurations == 1);
    CHECK(state.captures == 0);
}

TEST_CASE("Window draw is immutable once presentation is attempted and pending work conflicts",
          "[offscreen]") {
    OffscreenState state;
    state.complete = true;
    auto device = create(state);
    const auto request = test::offscreen_request_fixture();
    state.pending = true;
    CHECK_FALSE(device->set_presentation_draw(request, true));
    CHECK(state.configurations == 0);
    REQUIRE(device->drain());
    REQUIRE(device->present());
    CHECK_FALSE(device->set_presentation_draw(request, true));
    CHECK_FALSE(device->capture_presented());
    CHECK(state.configurations == 0);
    CHECK(state.captures == 0);
    REQUIRE(device->close());
}

TEST_CASE("Window draw configuration failures preserve errors and permit explicit retry",
          "[offscreen]") {
    OffscreenState state;
    auto device = create(state);
    const auto request = test::offscreen_request_fixture();
    state.fail_configuration = true;
    const auto failed = device->set_presentation_draw(request);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().context.request_id == "owned-request");
    state.fail_configuration = false;
    state.throw_configuration = true;
    const auto threw = device->set_presentation_draw(request);
    REQUIRE_FALSE(threw);
    CHECK(threw.error().context.operation == "graphics.window_draw.configure");
    state.throw_configuration = false;
    REQUIRE(device->set_presentation_draw(request));
    CHECK(state.configurations == 3);
    REQUIRE(device->close());
}

TEST_CASE("Window capture requires opt-in presentation and returns owned encoded pixels",
          "[offscreen]") {
    OffscreenState state;
    state.complete = true;
    auto device = create(state);
    auto request = test::offscreen_request_fixture();
    REQUIRE(device->set_presentation_draw(request, true));
    REQUIRE(device->present());
    std::optional<Result<OffscreenReadback>> wrong_thread;
    std::thread worker([&] { wrong_thread = device->capture_presented(); });
    worker.join();
    REQUIRE(wrong_thread);
    CHECK_FALSE(*wrong_thread);
    CHECK(state.captures == 0);
    state.pending = true;
    CHECK_FALSE(device->capture_presented());
    CHECK(state.captures == 0);
    REQUIRE(device->drain());
    auto result = device->capture_presented();
    REQUIRE(result);
    state.draw->texture_rgba8.front() = 0;
    CHECK(result.value().rgba8.front() == 255);
    CHECK(result.value().color_encoding == ReadbackColorEncoding::srgb);
    CHECK(result.value().submitted == result.value().completed);
    CHECK(result.value().created_resources == 15);
    CHECK(result.value().retired_resources == 0);
    REQUIRE(device->close());
    CHECK(result.value().rgba8.front() == 255);
    CHECK_FALSE(device->capture_presented());
    CHECK(state.captures == 1);
}

TEST_CASE("Window capture failure retains its owner for explicit retry and validation is fatal",
          "[offscreen]") {
    OffscreenState state;
    state.complete = true;
    auto device = create(state);
    REQUIRE(device->set_presentation_draw(test::offscreen_request_fixture(), true));
    REQUIRE(device->present());
    state.fail_capture = true;
    auto result = device->capture_presented();
    REQUIRE_FALSE(result);
    CHECK(result.error().context.request_id == "owned-request");
    CHECK(state.destroys == 0);
    state.fail_capture = false;
    state.throw_capture = true;
    result = device->capture_presented();
    REQUIRE_FALSE(result);
    CHECK(result.error().context.operation == "graphics.capture.readback");
    CHECK(state.destroys == 0);
    state.throw_capture = false;
    REQUIRE(device->capture_presented());
    state.validate_error = true;
    result = device->capture_presented();
    REQUIRE_FALSE(result);
    CHECK(result.error().context.operation == "graphics.validation");
    CHECK_FALSE(device->capture_presented());
    CHECK(state.captures == 4);
    CHECK_FALSE(device->close());
    CHECK(state.destroys == 5);
}

TEST_CASE("Scene viewport owner validates inputs thread and closed access before backend calls",
          "[scene-viewport]") {
    OffscreenState state;
    auto device = create(state);
    auto request = test::scene_viewport_request_fixture();
    request.indices.front() = 100;
    const auto invalid = device->render_scene_offscreen(request);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == ErrorCode::invalid_argument);
    CHECK(state.renders == 0);
    request = test::scene_viewport_request_fixture();
    std::optional<Result<SceneViewportReadback>> wrong_thread;
    std::optional<Result<SceneViewportStats>> wrong_stats;
    std::thread worker([&] {
        wrong_thread = device->render_scene_offscreen(request);
        wrong_stats = device->scene_viewport_stats();
    });
    worker.join();
    REQUIRE(wrong_thread);
    REQUIRE(wrong_stats);
    CHECK_FALSE(*wrong_thread);
    CHECK_FALSE(*wrong_stats);
    CHECK(state.renders == 0);
    const auto result = device->render_scene_offscreen(request);
    REQUIRE(result);
    CHECK(result.value().scene_revision == request.scene_revision);
    CHECK(result.value().origin == request.origin);
    CHECK(result.value().draw_count == 1);
    CHECK(result.value().pixels.extent == request.extent);
    REQUIRE(device->scene_viewport_stats());
    CHECK(device->scene_viewport_stats().value().generation == 1);
    request.scene_revision.revision = 99;
    request.origin.generation = 99;
    CHECK(result.value().scene_revision.revision == 4);
    CHECK(result.value().origin.generation == 7);
    REQUIRE(device->close());
    CHECK_FALSE(device->render_scene_offscreen(request));
    REQUIRE(device->scene_viewport_stats());
    CHECK(state.renders == 1);
    CHECK(result.value().pixels.rgba8.front() == 255);
}

TEST_CASE("Scene viewport submission failures retain owners and require explicit drain",
          "[scene-viewport]") {
    OffscreenState state;
    auto device = create(state);
    const CompleteAtExit completion_guard{state};
    const auto request = test::scene_viewport_request_fixture();
    SECTION("timeout preserves backend context") {
        state.fail_render = true;
    }
    SECTION("throw becomes owned failure") {
        state.throw_render = true;
    }
    const auto result = device->render_scene_offscreen(request);
    REQUIRE_FALSE(result);
    if (state.fail_render)
        CHECK(result.error().context.request_id == "owned-request");
    else
        CHECK(result.error().context.operation == "graphics.scene.render");
    CHECK(state.pending);
    CHECK(state.destroys == 0);
    const auto blocked = device->render_scene_offscreen(request);
    REQUIRE_FALSE(blocked);
    CHECK(blocked.error().code == ErrorCode::conflict);
    CHECK(state.renders == 1);
    CHECK_FALSE(device->close());
    CHECK(state.timeout == 1'000'000'000);
    CHECK(state.destroys == 0);
    state.complete = true;
    REQUIRE(device->drain());
    CHECK_FALSE(state.pending);
    REQUIRE(device->close());
    CHECK(state.destroys == 5);
}

TEST_CASE("Scene viewport diagnostics cannot be returned as successful pixels",
          "[scene-viewport]") {
    OffscreenState state;
    state.validate_error = true;
    auto device = create(state);
    const auto request = test::scene_viewport_request_fixture();
    const auto result = device->render_scene_offscreen(request);
    REQUIRE_FALSE(result);
    CHECK(result.error().context.operation == "graphics.validation");
    CHECK_FALSE(device->render_scene_offscreen(request));
    CHECK(state.renders == 1);
    CHECK_FALSE(device->close());
    CHECK(state.destroys == 5);
}

TEST_CASE("Scene presentation owns replacement snapshots and waits for a repacked extent",
          "[scene-viewport]") {
    OffscreenState state;
    state.complete = true;
    auto device = create(state);
    auto request = test::scene_viewport_request_fixture();
    request.extent = state.framebuffer;
    auto invalid = request;
    invalid.indices.back() = 999;
    CHECK_FALSE(device->set_presentation_scene(invalid));
    CHECK(state.configurations == 0);
    CHECK(state.drains == 0);
    std::optional<Result<void>> wrong_thread;
    std::thread worker([&] { wrong_thread = device->set_presentation_scene(request); });
    worker.join();
    REQUIRE(wrong_thread.has_value());
    CHECK_FALSE(*wrong_thread);
    REQUIRE(device->set_presentation_scene(request, true));
    CHECK_FALSE(device->set_presentation_draw(test::offscreen_request_fixture()));
    request.scene_revision.revision = 99;
    REQUIRE(state.scene);
    CHECK(state.scene->scene_revision.revision == 4);
    REQUIRE(device->present());
    REQUIRE(device->capture_presented());
    REQUIRE(device->set_presentation_scene(request, true));
    CHECK(state.scene->scene_revision.revision == 99);
    CHECK(state.drains > 0);
    CHECK(state.retired_swapchains == 0);
    state.framebuffer = {800, 600};
    const auto mismatch = device->present();
    REQUIRE(mismatch);
    CHECK(mismatch.value().status == PresentationStatus::recreate_pending);
    CHECK_FALSE(mismatch.value().stats.faulted);
    request.extent = state.framebuffer;
    REQUIRE(device->set_presentation_scene(request, true));
    const auto resized = device->present();
    REQUIRE(resized);
    CHECK(resized.value().status == PresentationStatus::presented);
    CHECK(resized.value().stats.extent == state.framebuffer);
    CHECK(state.retired_swapchains == 1);
    REQUIRE(device->close());
    CHECK_FALSE(device->set_presentation_scene(request, true));
}

TEST_CASE("Scene replacement preserves the previous snapshot across failures and GPU waits",
          "[scene-viewport]") {
    OffscreenState state;
    auto device = create(state);
    CompleteAtExit completion{state};
    auto request = test::scene_viewport_request_fixture();
    request.extent = state.framebuffer;
    REQUIRE(device->set_presentation_scene(request, true));
    REQUIRE(device->present());
    CHECK_FALSE(device->set_presentation_scene(request, false));
    CHECK(state.configurations == 1);
    request.scene_revision.revision = 5;
    const auto timeout = device->set_presentation_scene(request, true);
    REQUIRE_FALSE(timeout);
    CHECK(timeout.error().context.operation == "graphics.presentation.drain_timeout");
    CHECK(state.scene->scene_revision.revision == 4);
    CHECK(state.configurations == 1);
    state.complete = true;
    state.fail_configuration = true;
    const auto failed = device->set_presentation_scene(request, true);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().context.operation == "test.window_scene.configure");
    CHECK(state.scene->scene_revision.revision == 4);
    state.fail_configuration = false;
    state.throw_configuration = true;
    const auto threw = device->set_presentation_scene(request, true);
    REQUIRE_FALSE(threw);
    CHECK(threw.error().context.operation == "graphics.window_scene.configure");
    CHECK(state.scene->scene_revision.revision == 4);
    state.throw_configuration = false;
    state.pending = true;
    CHECK_FALSE(device->set_presentation_scene(request, true));
    state.pending = false;
    REQUIRE(device->set_presentation_scene(request, true));
    CHECK(state.scene->scene_revision.revision == 5);
    REQUIRE(device->close());
}

TEST_CASE("Scene replacement retires an acquired pending image only after a completed drain",
          "[scene-viewport]") {
    OffscreenState state;
    state.complete = true;
    state.wait_present = true;
    auto device = create(state);
    auto request = test::scene_viewport_request_fixture();
    request.extent = state.framebuffer;
    REQUIRE(device->set_presentation_scene(request));
    auto first = device->present();
    REQUIRE(first);
    CHECK(first.value().status == PresentationStatus::waiting);
    request.scene_revision.revision = 5;
    REQUIRE(device->set_presentation_scene(request));
    CHECK(state.retired_swapchains == 1);
    state.wait_present = false;
    auto next = device->present();
    REQUIRE(next);
    CHECK(next.value().status == PresentationStatus::presented);
    CHECK(next.value().stats.generation == 2);
    CHECK(next.value().stats.completed == next.value().stats.submitted);
    REQUIRE(device->close());
}

TEST_CASE("Scene configuration cannot replace a fixed presentation draw", "[scene-viewport]") {
    OffscreenState state;
    auto device = create(state);
    REQUIRE(device->set_presentation_draw(test::offscreen_request_fixture()));
    CHECK_FALSE(device->set_presentation_scene(test::scene_viewport_request_fixture()));
    CHECK(state.configurations == 1);
    CHECK_FALSE(state.scene);
    REQUIRE(device->close());
}

TEST_CASE("Scene resize waiting does not hide asynchronous validation errors", "[scene-viewport]") {
    OffscreenState state;
    auto device = create(state);
    auto request = test::scene_viewport_request_fixture();
    request.extent = state.framebuffer;
    REQUIRE(device->set_presentation_scene(request));
    state.framebuffer = {800, 600};
    state.sink->record(true, "CPU injected asynchronous validation error");
    const auto result = device->present();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::execution_failed);
    CHECK(state.presentations == 0);
    // Diagnostic failure remains observable, while no GPU work was started.
    CHECK_FALSE(device->close());
}
