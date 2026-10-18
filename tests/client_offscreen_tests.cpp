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
}

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
    Result<PresentationSync> create_presentation() override {
        ++state_.presentations;
        return PresentationSync::host_completion;
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
