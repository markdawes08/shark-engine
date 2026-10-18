#include <shark/client/graphics.hpp>

#include "../engine/client/src/graphics_backend.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using shark::Error;
using shark::ErrorCode;
using shark::Result;
using namespace shark::client;
using namespace shark::client::detail;

enum class Call {
    poll,
    state,
    command,
    presentation,
    swapchain,
    wait,
    acquire,
    submit,
    present,
    drain,
    destroy_swapchain,
    destroy_presentation,
    destroy_nvrhi,
    destroy_device,
    destroy_surface,
    destroy_instance,
    destroy_window,
};

struct Event {
    Call call{};
    std::uint32_t slot{0};
    std::uint32_t image{0};
    std::uint64_t timeout{0};
};

struct State {
    std::array<Event, 1024> events{};
    std::size_t event_count{0};
    GraphicsWindowFrame frame{
        .state = {
            .content = {640, 480}, .framebuffer = {640, 480}, .focused = true, .visible = true}};
    PresentationSync synchronization{PresentationSync::present_fence};
    std::optional<SwapchainInfo> swapchain{SwapchainInfo{{640, 480}, 3}};
    bool wait_complete{true};
    bool poll_complete{false};
    AcquiredImage acquired{AcquireStatus::ready, 1};
    PresentStatus presented{PresentStatus::presented};
    DrainStatus drained{DrainStatus::complete};
    std::optional<Call> fail;
    bool throw_failure{false};
    bool validation_error_on_submit{false};
    std::optional<Call> validation_stage;
    GraphicsDiagnosticSink* diagnostics{nullptr};
    ClearColor color{};
    WindowExtent requested_extent{};
    WindowCommand command{WindowCommand::show};

    void record(Call call, std::uint32_t slot = 0, std::uint32_t image = 0,
                std::uint64_t timeout = 0) noexcept {
        if (validation_stage == call && diagnostics) {
            diagnostics->record(true, "Injected stage validation error");
        }
        if (event_count < events.size()) {
            events[event_count++] = {call, slot, image, timeout};
        }
    }
    Result<void> attempt(Call call, std::uint32_t slot = 0, std::uint32_t image = 0,
                         std::uint64_t timeout = 0) {
        record(call, slot, image, timeout);
        if (fail != call) {
            return {};
        }
        if (throw_failure) {
            throw std::runtime_error("Injected presentation exception");
        }
        return Error{ErrorCode::execution_failed,
                     "Injected presentation failure",
                     {.operation = "test.presentation.failure", .request_id = "owned-request"}};
    }
    [[nodiscard]] std::vector<Event> calls(Call call) const {
        std::vector<Event> found;
        for (std::size_t index = 0; index < event_count; ++index) {
            if (events[index].call == call) {
                found.push_back(events[index]);
            }
        }
        return found;
    }
    [[nodiscard]] std::size_t count(Call call) const {
        return calls(call).size();
    }
    [[nodiscard]] std::size_t position(Call call) const {
        for (std::size_t index = 0; index < event_count; ++index) {
            if (events[index].call == call) {
                return index;
            }
        }
        return event_count;
    }
    void clear() noexcept {
        event_count = 0;
    }
};

GraphicsAdapter adapter() {
    GraphicsAdapter value;
    value.uuid = "0123456789abcdef0123456789abcdef";
    value.name = "Presentation CPU fixture";
    value.kind = AdapterKind::discrete;
    value.capabilities.features.fill(true);
    value.capabilities.formats.fill(true);
    value.capabilities.limits = {4096, 2048, 12, 4, 4, 16, 16, 16384, 134217728, 128};
    return value;
}

class Backend final : public GraphicsBackend {
  public:
    explicit Backend(State& state) noexcept : state_(state) {}
    Result<void> create_window(const WindowConfig&) override {
        return {};
    }
    Result<void> create_instance(GraphicsValidation, GraphicsDiagnosticSink& sink) override {
        state_.diagnostics = &sink;
        return {};
    }
    Result<void> create_surface() override {
        return {};
    }
    Result<std::vector<GraphicsAdapter>> enumerate_adapters() override {
        return std::vector{adapter()};
    }
    Result<void> create_device(std::size_t) override {
        return {};
    }
    Result<void> create_nvrhi(GraphicsDiagnosticSink&) override {
        return {};
    }
    Result<GraphicsWindowFrame> poll_window() override {
        if (auto result = state_.attempt(Call::poll); !result) {
            return result.error();
        }
        return state_.frame;
    }
    Result<WindowState> window_state() override {
        if (auto result = state_.attempt(Call::state); !result) {
            return result.error();
        }
        return state_.frame.state;
    }
    Result<void> window_command(WindowCommand command, WindowExtent extent) override {
        state_.command = command;
        state_.requested_extent = extent;
        return state_.attempt(Call::command);
    }
    Result<PresentationSync> create_presentation() override {
        if (auto result = state_.attempt(Call::presentation); !result) {
            return result.error();
        }
        return state_.synchronization;
    }
    Result<std::optional<SwapchainInfo>> create_swapchain(WindowExtent extent) override {
        state_.requested_extent = extent;
        if (auto result = state_.attempt(Call::swapchain); !result) {
            return result.error();
        }
        return state_.swapchain;
    }
    Result<bool> wait_slot(std::uint32_t slot, std::uint64_t timeout) override {
        if (auto result = state_.attempt(Call::wait, slot, 0, timeout); !result) {
            return result.error();
        }
        return timeout == 0 ? state_.poll_complete : state_.wait_complete;
    }
    Result<AcquiredImage> acquire(std::uint32_t slot, std::uint64_t timeout) override {
        if (auto result = state_.attempt(Call::acquire, slot, 0, timeout); !result) {
            return result.error();
        }
        return state_.acquired;
    }
    Result<void> submit(std::uint32_t slot, std::uint32_t image, ClearColor color) override {
        state_.color = color;
        if (state_.validation_error_on_submit) {
            state_.diagnostics->record(true, "Injected submission validation error");
        }
        return state_.attempt(Call::submit, slot, image);
    }
    Result<PresentStatus> present(std::uint32_t slot, std::uint32_t image,
                                  std::uint64_t timeout) override {
        if (auto result = state_.attempt(Call::present, slot, image, timeout); !result) {
            return result.error();
        }
        return state_.presented;
    }
    Result<DrainStatus> drain_presentation(std::uint64_t timeout) override {
        if (auto result = state_.attempt(Call::drain, 0, 0, timeout); !result) {
            return result.error();
        }
        return state_.drained;
    }
    void destroy_swapchain() noexcept override {
        state_.record(Call::destroy_swapchain);
    }
    void destroy_presentation() noexcept override {
        state_.record(Call::destroy_presentation);
    }
    void destroy_nvrhi() noexcept override {
        state_.record(Call::destroy_nvrhi);
    }
    void destroy_device() noexcept override {
        state_.record(Call::destroy_device);
    }
    void destroy_surface() noexcept override {
        state_.record(Call::destroy_surface);
    }
    void destroy_instance() noexcept override {
        state_.record(Call::destroy_instance);
    }
    void destroy_window() noexcept override {
        state_.record(Call::destroy_window);
    }

  private:
    State& state_;
};

std::unique_ptr<GraphicsDevice> create(State& state) {
    auto result = create_graphics({}, std::make_unique<Backend>(state));
    REQUIRE(result);
    return std::move(result).value();
}

template <class T> void error(const Result<T>& result, ErrorCode code) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == code);
    CHECK_FALSE(result.error().message.empty());
    CHECK_FALSE(result.error().context.operation.empty());
}

PresentationReport present(GraphicsDevice& owner) {
    auto result = owner.present();
    REQUIRE(result);
    return result.value();
}

void no_gpu_work(const State& state) {
    for (const auto call : {Call::presentation, Call::swapchain, Call::wait, Call::acquire,
                            Call::submit, Call::present, Call::drain, Call::destroy_swapchain}) {
        CHECK(state.count(call) == 0);
    }
}

} // namespace

TEST_CASE("presentation exposes owned reports without native handles", "[presentation]") {
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<GraphicsDevice&>().present()),
                                  Result<PresentationReport>>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<GraphicsDevice&>().presentation_stats()),
                                  Result<PresentationStats>>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<GraphicsDevice&>().poll_window()),
                                  Result<GraphicsWindowFrame>>);
    State state;
    auto owner = create(state);
    const auto stats = owner->presentation_stats();
    REQUIRE(stats);
    CHECK_FALSE(stats.value().active);
    CHECK_FALSE(stats.value().faulted);
    CHECK_FALSE(stats.value().closed);
    CHECK(stats.value().generation == 0);
    CHECK(stats.value().submitted == 0);
    no_gpu_work(state);
}

TEST_CASE("presentation startup is lazy while minimized zero sized or closing", "[presentation]") {
    for (unsigned int scenario = 0; scenario < 4; ++scenario) {
        State state;
        auto owner = create(state);
        if (scenario == 0) {
            state.frame.state.framebuffer.width = 0;
        } else if (scenario == 1) {
            state.frame.state.framebuffer.height = 0;
        } else if (scenario == 2) {
            state.frame.state.iconified = true;
        } else {
            state.frame.state.close_requested = true;
        }
        CAPTURE(scenario);
        for (unsigned int frame = 0; frame < 3; ++frame) {
            const auto report = present(*owner);
            CHECK(report.status ==
                  (scenario == 3 ? PresentationStatus::closed : PresentationStatus::suspended));
            CHECK(report.stats.submitted == 0);
            CHECK(report.stats.generation == 0);
            CHECK_FALSE(report.recreated);
        }
        no_gpu_work(state);
    }
}

TEST_CASE("presentation rejects invalid clear colors before any backend call", "[presentation]") {
    State state;
    auto owner = create(state);
    for (const float value :
         {-0.001F, 1.001F, std::numeric_limits<float>::quiet_NaN(),
          std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            ClearColor color;
            const std::array channels{&color.red, &color.green, &color.blue, &color.alpha};
            *channels[channel] = value;
            error(owner->present(color), ErrorCode::invalid_argument);
            CHECK(state.event_count == 0);
        }
    }
    const ClearColor bounds{0.0F, 1.0F, 0.0F, 1.0F};
    REQUIRE(owner->present(bounds));
    CHECK(state.color.red == 0.0F);
    CHECK(state.color.green == 1.0F);
    CHECK(state.color.blue == 0.0F);
    CHECK(state.color.alpha == 1.0F);
}

TEST_CASE("presentation limits submitted work and waits before slot reuse", "[presentation]") {
    for (const auto synchronization :
         {PresentationSync::present_fence, PresentationSync::host_completion}) {
        State state;
        state.synchronization = synchronization;
        auto owner = create(state);
        const std::uint32_t limit = synchronization == PresentationSync::present_fence ? 2U : 1U;
        for (std::uint32_t index = 0; index < limit; ++index) {
            const auto report = present(*owner);
            CHECK(report.status == PresentationStatus::presented);
            CHECK(report.stats.frame_limit == limit);
            CHECK(report.stats.synchronization == synchronization);
            CHECK(report.stats.in_flight <= limit);
            CHECK(report.stats.peak_in_flight <= limit);
        }
        const auto submitted_before = state.count(Call::submit);
        const auto acquired_before = state.count(Call::acquire);
        state.wait_complete = false;
        auto waiting = present(*owner);
        CHECK(waiting.status == PresentationStatus::waiting);
        CHECK(state.count(Call::submit) == submitted_before);
        CHECK(state.count(Call::acquire) == acquired_before);
        REQUIRE_FALSE(state.calls(Call::wait).empty());
        CHECK(state.calls(Call::wait).back().slot == 0);
        CHECK(state.calls(Call::wait).back().timeout == 5'000'000);
        state.wait_complete = true;
        state.clear();
        const auto resumed = present(*owner);
        CHECK(resumed.status == PresentationStatus::presented);
        CHECK(resumed.stats.in_flight <= limit);
        CHECK(resumed.stats.peak_in_flight == limit);
        CHECK(resumed.stats.submitted == limit + 1);
        CHECK(resumed.stats.completed >= 1);
        CHECK(state.position(Call::wait) < state.position(Call::submit));
        REQUIRE(state.calls(Call::submit).size() == 1);
        CHECK(state.calls(Call::submit).front().slot == 0);
    }
}

TEST_CASE("presentation acquire waiting and out of date never submit or retry internally",
          "[presentation]") {
    for (const auto status : {AcquireStatus::waiting, AcquireStatus::out_of_date}) {
        State state;
        state.acquired.status = status;
        auto owner = create(state);
        const auto report = present(*owner);
        CHECK(report.status == (status == AcquireStatus::waiting
                                    ? PresentationStatus::waiting
                                    : PresentationStatus::recreate_pending));
        CHECK(report.stats.submitted == 0);
        CHECK(state.count(Call::acquire) == 1);
        CHECK(state.calls(Call::acquire).front().timeout == 1'000'000);
        CHECK(state.count(Call::submit) == 0);
        CHECK(state.count(Call::present) == 0);
        CHECK(state.count(Call::swapchain) == 1);
        if (status == AcquireStatus::out_of_date) {
            state.acquired.status = AcquireStatus::ready;
            state.clear();
            const auto recovered = present(*owner);
            CHECK(recovered.status == PresentationStatus::presented);
            CHECK(recovered.stats.generation == 2);
            CHECK(recovered.stats.retired_generations == 1);
            CHECK(state.count(Call::swapchain) == 1);
            CHECK(state.position(Call::drain) < state.position(Call::destroy_swapchain));
            CHECK(state.position(Call::destroy_swapchain) < state.position(Call::swapchain));
        }
    }
}

TEST_CASE("presentation preserves an acquired image across a bounded present wait",
          "[presentation]") {
    State state;
    state.presented = PresentStatus::waiting;
    auto owner = create(state);
    auto report = present(*owner);
    CHECK(report.status == PresentationStatus::waiting);
    CHECK(report.stats.submitted == 1);
    CHECK(report.stats.presented == 0);
    state.clear();
    report = present(*owner);
    CHECK(report.status == PresentationStatus::waiting);
    CHECK(state.count(Call::present) == 1);
    CHECK(state.count(Call::acquire) == 0);
    CHECK(state.count(Call::submit) == 0);
    CHECK(state.calls(Call::present).front().slot == 0);
    CHECK(state.calls(Call::present).front().image == 1);
    state.presented = PresentStatus::presented;
    state.clear();
    report = present(*owner);
    CHECK(report.status == PresentationStatus::presented);
    CHECK(report.stats.submitted == 1);
    CHECK(report.stats.presented == 1);
    CHECK(state.count(Call::present) == 1);
    CHECK(state.count(Call::acquire) == 0);
    CHECK(state.count(Call::submit) == 0);
}

TEST_CASE("out of date presentation retains submitted use until the next drain", "[presentation]") {
    State state;
    state.presented = PresentStatus::out_of_date;
    auto owner = create(state);
    const auto first = present(*owner);
    CHECK(first.status == PresentationStatus::recreate_pending);
    CHECK(first.stats.submitted == 1);
    CHECK(first.stats.presented == 0);
    CHECK(first.stats.completed == 0);
    CHECK(state.count(Call::destroy_swapchain) == 0);
    state.presented = PresentStatus::presented;
    state.drained = DrainStatus::waiting;
    state.clear();
    const auto waiting = present(*owner);
    CHECK(waiting.status == PresentationStatus::waiting);
    CHECK(waiting.stats.completed == 0);
    CHECK(state.count(Call::destroy_swapchain) == 0);
    CHECK(state.count(Call::submit) == 0);
    state.drained = DrainStatus::complete;
    state.clear();
    const auto recovered = present(*owner);
    CHECK(recovered.status == PresentationStatus::presented);
    CHECK(recovered.stats.completed == 1);
    CHECK(recovered.stats.submitted == 2);
    CHECK(recovered.stats.retired_generations == 1);
    CHECK(state.position(Call::drain) < state.position(Call::destroy_swapchain));
}

TEST_CASE("a completed later queue submission proves earlier use complete", "[presentation]") {
    State state;
    auto owner = create(state);
    CHECK(present(*owner).stats.completed == 0);
    state.poll_complete = true;
    const auto second = present(*owner);
    CHECK(second.stats.submitted == 2);
    CHECK(second.stats.completed == 2);
    CHECK(second.stats.in_flight == 0);
    CHECK(second.stats.peak_in_flight == 2);
    REQUIRE(owner->drain());
    const auto drained = owner->presentation_stats();
    REQUIRE(drained);
    CHECK(drained.value().completed == 2);
    CHECK(drained.value().in_flight == 0);
    CHECK(state.count(Call::destroy_swapchain) == 0);
}

TEST_CASE("suboptimal presentation defers exactly one retirement to a later call",
          "[presentation]") {
    for (const bool at_acquire : {false, true}) {
        State state;
        if (at_acquire) {
            state.acquired.status = AcquireStatus::suboptimal;
        } else {
            state.presented = PresentStatus::suboptimal;
        }
        auto owner = create(state);
        const auto first = present(*owner);
        CHECK(first.stats.submitted == 1);
        CHECK(first.stats.presented == 1);
        CHECK(first.stats.generation == 1);
        CHECK(state.count(Call::swapchain) == 1);
        CHECK(state.count(Call::destroy_swapchain) == 0);
        state.acquired.status = AcquireStatus::ready;
        state.presented = PresentStatus::presented;
        state.clear();
        const auto next = present(*owner);
        CHECK(next.status == PresentationStatus::presented);
        CHECK(next.stats.generation == 2);
        CHECK(next.stats.retired_generations == 1);
        CHECK(next.recreated);
        CHECK(state.count(Call::drain) == 1);
        CHECK(state.count(Call::destroy_swapchain) == 1);
        CHECK(state.count(Call::swapchain) == 1);
        CHECK(state.position(Call::drain) < state.position(Call::destroy_swapchain));
        CHECK(state.position(Call::destroy_swapchain) < state.position(Call::swapchain));
    }
}

TEST_CASE("resize retains old resources until both GPU and presentation use drain",
          "[presentation]") {
    State state;
    auto owner = create(state);
    REQUIRE(present(*owner).stats.generation == 1);
    state.frame.state.framebuffer = {800, 600};
    state.swapchain = SwapchainInfo{{800, 600}, 3};
    state.drained = DrainStatus::waiting;
    state.clear();
    const auto waiting = present(*owner);
    CHECK(waiting.status == PresentationStatus::waiting);
    CHECK(waiting.stats.generation == 1);
    CHECK(waiting.stats.retired_generations == 0);
    CHECK(state.count(Call::drain) == 1);
    CHECK(state.calls(Call::drain).front().timeout == 5'000'000);
    CHECK(state.count(Call::destroy_swapchain) == 0);
    CHECK(state.count(Call::swapchain) == 0);
    CHECK(state.count(Call::acquire) == 0);
    CHECK(state.count(Call::submit) == 0);
    state.drained = DrainStatus::complete;
    state.clear();
    const auto resized = present(*owner);
    CHECK(resized.status == PresentationStatus::presented);
    CHECK(resized.stats.generation == 2);
    CHECK(resized.stats.retired_generations == 1);
    CHECK(resized.stats.extent == WindowExtent{800, 600});
    CHECK(state.requested_extent == WindowExtent{800, 600});
    CHECK(state.position(Call::drain) < state.position(Call::destroy_swapchain));
    CHECK(state.position(Call::destroy_swapchain) < state.position(Call::swapchain));
    CHECK(state.position(Call::swapchain) < state.position(Call::submit));
}

TEST_CASE("minimizing active presentation suspends all GPU work until restore", "[presentation]") {
    State state;
    auto owner = create(state);
    REQUIRE(present(*owner).stats.submitted == 1);
    for (const bool iconified : {true, false}) {
        state.frame.state.iconified = iconified;
        state.frame.state.framebuffer = iconified ? WindowExtent{640, 480} : WindowExtent{};
        state.clear();
        for (unsigned int frame = 0; frame < 4; ++frame) {
            const auto report = present(*owner);
            CHECK(report.status == PresentationStatus::suspended);
            CHECK(report.stats.submitted == 1);
        }
        no_gpu_work(state);
    }
    state.frame.state.framebuffer = {640, 480};
    state.clear();
    const auto resumed = present(*owner);
    CHECK(resumed.status == PresentationStatus::presented);
    CHECK(resumed.stats.submitted == 2);
    CHECK(state.count(Call::submit) == 1);
}

TEST_CASE("transient unavailable swapchain yields without submitting", "[presentation]") {
    State state;
    state.swapchain.reset();
    auto owner = create(state);
    const auto first = present(*owner);
    CHECK(first.status == PresentationStatus::recreate_pending);
    CHECK(first.stats.generation == 0);
    CHECK_FALSE(first.stats.active);
    CHECK(state.count(Call::swapchain) == 1);
    CHECK(state.count(Call::acquire) == 0);
    CHECK(state.count(Call::submit) == 0);
    state.swapchain = SwapchainInfo{{640, 480}, 3};
    const auto recovered = present(*owner);
    CHECK(recovered.status == PresentationStatus::presented);
    CHECK(recovered.stats.generation == 1);
    CHECK(state.count(Call::presentation) == 1);
    CHECK(state.count(Call::swapchain) == 2);
}

TEST_CASE("presentation rejects malformed backend image and swapchain reports", "[presentation]") {
    for (unsigned int scenario = 0; scenario < 6; ++scenario) {
        State state;
        if (scenario == 0) {
            state.swapchain->extent.width = 0;
        } else if (scenario == 1) {
            state.swapchain->extent.height = 0;
        } else if (scenario == 2) {
            state.swapchain->image_count = 0;
        } else if (scenario == 3) {
            state.acquired.image_index = 3;
        } else if (scenario == 4) {
            state.acquired.status = static_cast<AcquireStatus>(99);
        } else {
            state.swapchain->image_count = 17;
        }
        CAPTURE(scenario);
        auto owner = create(state);
        error(owner->present(), ErrorCode::execution_failed);
        CHECK(state.count(Call::submit) == 0);
        const auto stats = owner->presentation_stats();
        REQUIRE(stats);
        CHECK(stats.value().faulted);
    }
}

TEST_CASE("backend presentation failures latch without resubmission and retain primary context",
          "[presentation]") {
    for (const auto stage : {Call::presentation, Call::swapchain, Call::wait, Call::acquire,
                             Call::submit, Call::present}) {
        for (const bool throwing : {false, true}) {
            State state;
            auto owner = create(state);
            if (stage == Call::wait) {
                REQUIRE(owner->present());
                REQUIRE(owner->present());
            }
            state.fail = stage;
            state.throw_failure = throwing;
            auto failed = owner->present();
            CAPTURE(stage, throwing);
            error(failed, ErrorCode::execution_failed);
            if (!throwing) {
                CHECK(failed.error().context.operation == "test.presentation.failure");
                CHECK(failed.error().context.request_id == "owned-request");
            }
            const auto count = state.event_count;
            auto repeated = owner->present();
            error(repeated, ErrorCode::execution_failed);
            CHECK(state.event_count == count);
            const auto stats = owner->presentation_stats();
            REQUIRE(stats);
            CHECK(stats.value().faulted);
            state.fail.reset();
            REQUIRE(owner->poll_window());
            error(owner->close(), ErrorCode::execution_failed);
            const auto closed = owner->presentation_stats();
            REQUIRE(closed);
            CHECK(closed.value().closed);
            CHECK(state.count(Call::destroy_device) == 1);
            const auto after_close = state.event_count;
            error(owner->close(), ErrorCode::execution_failed);
            CHECK(state.event_count == after_close);
        }
    }
}

TEST_CASE("submission validation errors stop further presentation and survive close",
          "[presentation]") {
    State state;
    state.validation_error_on_submit = true;
    auto owner = create(state);
    error(owner->present(), ErrorCode::execution_failed);
    CHECK(state.count(Call::submit) == 1);
    CHECK(state.count(Call::present) == 0);
    state.clear();
    error(owner->present(), ErrorCode::execution_failed);
    CHECK(state.event_count == 0);
    error(owner->close(), ErrorCode::execution_failed);
    const auto diagnostics = owner->diagnostics();
    REQUIRE(diagnostics);
    CHECK(diagnostics.value().errors == 1);
    CHECK(state.position(Call::drain) < state.position(Call::destroy_swapchain));
}

TEST_CASE("validation failures remain immediate across waits cleanup and successful presentation",
          "[presentation]") {
    for (const auto stage : {Call::drain, Call::destroy_swapchain, Call::present}) {
        State state;
        auto owner = create(state);
        if (stage == Call::drain) {
            REQUIRE(owner->present());
            state.frame.state.framebuffer = {800, 600};
            state.drained = DrainStatus::waiting;
        } else if (stage == Call::destroy_swapchain) {
            state.swapchain.reset();
        }
        state.validation_stage = stage;
        state.clear();
        error(owner->present(), ErrorCode::execution_failed);
        const auto stats = owner->presentation_stats();
        REQUIRE(stats);
        CHECK(stats.value().faulted);
        if (stage == Call::present) {
            CHECK(stats.value().presented == 1);
            CHECK(stats.value().submitted == 1);
        }
        const auto count = state.event_count;
        error(owner->present(), ErrorCode::execution_failed);
        CHECK(state.event_count == count);
        state.validation_stage.reset();
        state.drained = DrainStatus::complete;
        error(owner->close(), ErrorCode::execution_failed);
        CHECK(state.count(Call::destroy_device) == 1);
    }
}

TEST_CASE("close timeout retains all owners and explicit retry retires safely", "[presentation]") {
    State state;
    auto owner = create(state);
    REQUIRE(present(*owner).stats.submitted == 1);
    state.drained = DrainStatus::waiting;
    state.clear();
    REQUIRE_FALSE(owner->close());
    CHECK(state.count(Call::drain) == 1);
    CHECK(state.calls(Call::drain).front().timeout == 1'000'000'000);
    CHECK(state.count(Call::destroy_swapchain) == 0);
    CHECK(state.count(Call::destroy_device) == 0);
    auto retained = owner->presentation_stats();
    REQUIRE(retained);
    CHECK_FALSE(retained.value().closed);
    CHECK(retained.value().active);
    state.drained = DrainStatus::complete;
    state.clear();
    REQUIRE(owner->close());
    CHECK(state.position(Call::drain) < state.position(Call::destroy_swapchain));
    CHECK(state.position(Call::destroy_swapchain) < state.position(Call::destroy_presentation));
    CHECK(state.position(Call::destroy_presentation) < state.position(Call::destroy_nvrhi));
    CHECK(state.position(Call::destroy_nvrhi) < state.position(Call::destroy_device));
    const auto closed = owner->presentation_stats();
    REQUIRE(closed);
    CHECK(closed.value().closed);
    CHECK_FALSE(closed.value().active);
    CHECK(closed.value().in_flight == 0);
    CHECK(closed.value().completed == closed.value().submitted);
    CHECK(closed.value().retired_generations == closed.value().generation);
    const auto calls = state.event_count;
    REQUIRE(owner->close());
    CHECK(state.event_count == calls);
}

TEST_CASE("failed drain preserves ownership until a later explicit successful drain",
          "[presentation]") {
    State state;
    auto owner = create(state);
    REQUIRE(owner->present());
    state.fail = Call::drain;
    state.clear();
    error(owner->drain(), ErrorCode::execution_failed);
    CHECK(state.count(Call::destroy_swapchain) == 0);
    CHECK(state.count(Call::destroy_device) == 0);
    state.fail.reset();
    // A known successful drain makes destruction safe even when the earlier
    // failure remains latched and is correctly returned by close.
    static_cast<void>(owner->drain());
    static_cast<void>(owner->close());
    CHECK(state.count(Call::destroy_device) == 1);
}

TEST_CASE("backend outcome enums are validated before further presentation work",
          "[presentation]") {
    for (unsigned int scenario = 0; scenario < 3; ++scenario) {
        State state;
        if (scenario == 0) {
            state.synchronization = static_cast<PresentationSync>(99);
        } else if (scenario == 1) {
            state.presented = static_cast<PresentStatus>(99);
        } else {
            state.drained = static_cast<DrainStatus>(99);
        }
        auto owner = create(state);
        error(owner->present(), ErrorCode::execution_failed);
        const auto count = state.event_count;
        error(owner->present(), ErrorCode::execution_failed);
        CHECK(state.event_count == count);
        if (scenario != 1) {
            CHECK(state.count(Call::submit) == 0);
        }
        state.drained = DrainStatus::complete;
        error(owner->close(), ErrorCode::execution_failed);
        CHECK(state.count(Call::destroy_device) == 1);
    }
}

TEST_CASE("device loss permits safe terminal cleanup but never a successful close",
          "[presentation]") {
    State state;
    auto owner = create(state);
    REQUIRE(owner->present());
    state.drained = DrainStatus::device_lost;
    state.clear();
    error(owner->close(), ErrorCode::execution_failed);
    CHECK(state.count(Call::drain) == 1);
    CHECK(state.count(Call::destroy_swapchain) == 1);
    CHECK(state.count(Call::destroy_device) == 1);
    const auto stats = owner->presentation_stats();
    REQUIRE(stats);
    CHECK(stats.value().faulted);
    CHECK(stats.value().closed);
    const auto calls = state.event_count;
    error(owner->close(), ErrorCode::execution_failed);
    CHECK(state.event_count == calls);
}

TEST_CASE("window forwarding shares owned state input and diagnostic values", "[presentation]") {
    State state;
    state.frame.input.text[0] = U'\u03bb';
    state.frame.input.text_size = 1;
    state.frame.diagnostics.invalid_window_events = 2;
    auto owner = create(state);
    auto first = owner->poll_window();
    REQUIRE(first);
    CHECK(first.value().state.framebuffer == WindowExtent{640, 480});
    CHECK(first.value().input.text[0] == U'\u03bb');
    CHECK(first.value().diagnostics.invalid_window_events == 2);
    first.value().state.framebuffer = {};
    const auto next = owner->window_state();
    REQUIRE(next);
    CHECK(next.value().framebuffer == WindowExtent{640, 480});
    REQUIRE(owner->resize({800, 600}));
    CHECK(state.command == WindowCommand::resize);
    CHECK(state.requested_extent == WindowExtent{800, 600});
    REQUIRE(owner->iconify());
    CHECK(state.command == WindowCommand::iconify);
    REQUIRE(owner->restore());
    CHECK(state.command == WindowCommand::restore);
    REQUIRE(owner->show());
    CHECK(state.command == WindowCommand::show);
    REQUIRE(owner->focus());
    CHECK(state.command == WindowCommand::focus);
    REQUIRE(owner->request_close());
    CHECK(state.command == WindowCommand::request_close);
    no_gpu_work(state);
}

TEST_CASE("presentation and window methods reject foreign thread and closed owner",
          "[presentation]") {
    State state;
    auto owner = create(state);
    std::array<bool, 12> rejected{};
    std::thread worker([&] {
        const auto record = [&](std::size_t index, const auto& result) {
            rejected[index] = !result && result.error().code == ErrorCode::conflict;
        };
        record(0, owner->present());
        record(1, owner->presentation_stats());
        record(2, owner->poll_window());
        record(3, owner->window_state());
        record(4, owner->resize({800, 600}));
        record(5, owner->iconify());
        record(6, owner->restore());
        record(7, owner->show());
        record(8, owner->focus());
        record(9, owner->request_close());
        record(10, owner->drain());
        record(11, owner->close());
    });
    worker.join();
    CHECK(std::all_of(rejected.begin(), rejected.end(), [](bool value) { return value; }));
    CHECK(state.event_count == 0);
    REQUIRE(owner->close());
    state.clear();
    REQUIRE_FALSE(owner->poll_window());
    REQUIRE_FALSE(owner->window_state());
    REQUIRE_FALSE(owner->resize({800, 600}));
    REQUIRE_FALSE(owner->iconify());
    REQUIRE_FALSE(owner->restore());
    REQUIRE_FALSE(owner->show());
    REQUIRE_FALSE(owner->focus());
    REQUIRE_FALSE(owner->request_close());
    error(owner->present(), ErrorCode::conflict);
    REQUIRE(owner->presentation_stats());
    CHECK(state.event_count == 0);
}
