#include <shark/client/frame_loop.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using shark::ErrorCode;
using shark::MonotonicTime;
using shark::client::ButtonAction;
using shark::client::InputEventResult;
using shark::client::InputSnapshot;
using shark::client::InputState;
using shark::client::Key;
using shark::client::MouseButton;
using shark::client::OfflineFrameControl;
using shark::client::OfflineFrameLoop;
using shark::client::OfflineInputCallback;

struct TickCapture {
    std::uint64_t index;
    InputSnapshot input;
};

struct FrameFixture {
    FrameFixture() {
        auto created = OfflineFrameLoop::create(0ns);
        REQUIRE(created);
        loop = std::move(created).value();
        REQUIRE(input.on_focus(true) == InputEventResult::accepted);
        static_cast<void>(input.capture());
    }

    std::unique_ptr<OfflineFrameLoop> loop;
    InputState input;
    std::vector<TickCapture> ticks;
    OfflineInputCallback callback{[this](std::uint64_t index, const InputSnapshot& snapshot) {
        ticks.push_back({index, snapshot});
        return shark::Result<void>{};
    }};

    shark::client::OfflineFrameReport advance(MonotonicTime now, OfflineFrameControl control = {}) {
        auto result = loop->advance(now, input.capture(), control, callback);
        REQUIRE(result);
        return result.value();
    }
};

void check_no_interval(const InputSnapshot& input) {
    for (const auto& key : input.keys) {
        CHECK_FALSE(key.pressed);
        CHECK_FALSE(key.released);
        CHECK_FALSE(key.repeated);
    }
    for (const auto& button : input.mouse_buttons) {
        CHECK_FALSE(button.pressed);
        CHECK_FALSE(button.released);
        CHECK_FALSE(button.repeated);
    }
    CHECK(input.text_size == 0);
    CHECK(input.scroll.x == 0.0);
    CHECK(input.scroll.y == 0.0);
    CHECK_FALSE(input.focus_gained);
    CHECK_FALSE(input.focus_lost);
    CHECK_FALSE(input.text_overflow);
    CHECK_FALSE(input.scroll_overflow);
}

static_assert(!std::is_copy_constructible_v<OfflineFrameLoop>);
static_assert(!std::is_move_constructible_v<OfflineFrameLoop>);

} // namespace

TEST_CASE("frame input survives zero ticks and is consumed only by the first catch-up tick",
          "[frame]") {
    FrameFixture fixture;
    REQUIRE(fixture.input.on_key(Key::w, ButtonAction::press) == InputEventResult::accepted);
    REQUIRE(fixture.input.on_key(Key::w, ButtonAction::repeat) == InputEventResult::accepted);
    REQUIRE(fixture.input.on_mouse_button(MouseButton::left, ButtonAction::press) ==
            InputEventResult::accepted);
    REQUIRE(fixture.input.on_cursor(12.5, -3.0) == InputEventResult::accepted);
    REQUIRE(fixture.input.on_text(U'a') == InputEventResult::accepted);
    REQUIRE(fixture.input.on_scroll(0.25, 1.5) == InputEventResult::accepted);
    const auto frame = fixture.input.capture();
    const auto retained = frame;
    auto first = fixture.loop->advance(1ms, frame, {}, fixture.callback);
    REQUIRE(first);
    CHECK(first.value().timing.executed == 0);
    CHECK(fixture.advance(8ms).timing.executed == 0);
    REQUIRE(fixture.input.on_text(U'\U0001F988') == InputEventResult::accepted);
    REQUIRE(fixture.input.on_scroll(-0.5, 0.25) == InputEventResult::accepted);
    auto catch_up = fixture.advance(50ms);
    CHECK(catch_up.timing.executed == 3);
    REQUIRE(fixture.ticks.size() == 3);
    const auto& first_input = fixture.ticks[0].input;
    CHECK(first_input.key(Key::w).held);
    CHECK(first_input.key(Key::w).pressed);
    CHECK(first_input.key(Key::w).repeated);
    CHECK(first_input.mouse_button(MouseButton::left).pressed);
    REQUIRE(first_input.text_size == 2);
    CHECK(first_input.text[0] == U'a');
    CHECK(first_input.text[1] == U'\U0001F988');
    CHECK(first_input.scroll.x == -0.25);
    CHECK(first_input.scroll.y == 1.75);
    for (std::size_t i = 1; i < fixture.ticks.size(); ++i) {
        CHECK(fixture.ticks[i].index == i);
        CHECK(fixture.ticks[i].input.key(Key::w).held);
        CHECK(fixture.ticks[i].input.mouse_button(MouseButton::left).held);
        CHECK(fixture.ticks[i].input.cursor == frame.cursor);
        check_no_interval(fixture.ticks[i].input);
    }
    CHECK(frame == retained);
    CHECK(fixture.loop->next_tick() == 3);
}

TEST_CASE("frame quick taps across captures retain both edges and latest held state", "[frame]") {
    FrameFixture fixture;
    REQUIRE(fixture.input.on_key(Key::space, ButtonAction::press) == InputEventResult::accepted);
    CHECK(fixture.advance(1ms).timing.executed == 0);
    REQUIRE(fixture.input.on_key(Key::space, ButtonAction::release) == InputEventResult::accepted);
    REQUIRE(fixture.input.on_mouse_button(MouseButton::right, ButtonAction::press) ==
            InputEventResult::accepted);
    REQUIRE(fixture.input.on_mouse_button(MouseButton::right, ButtonAction::release) ==
            InputEventResult::accepted);
    CHECK(fixture.advance(10ms).timing.executed == 0);
    CHECK(fixture.advance(34ms).timing.executed == 2);
    REQUIRE(fixture.ticks.size() == 2);
    const auto key = fixture.ticks[0].input.key(Key::space);
    CHECK_FALSE(key.held);
    CHECK(key.pressed);
    CHECK(key.released);
    const auto mouse = fixture.ticks[0].input.mouse_button(MouseButton::right);
    CHECK_FALSE(mouse.held);
    CHECK(mouse.pressed);
    CHECK(mouse.released);
    check_no_interval(fixture.ticks[1].input);
}

TEST_CASE("frame focus loss cancels old pending actions while post-regain input survives",
          "[frame]") {
    FrameFixture fixture;
    REQUIRE(fixture.input.on_key(Key::a, ButtonAction::press) == InputEventResult::accepted);
    REQUIRE(fixture.input.on_text(U'x') == InputEventResult::accepted);
    REQUIRE(fixture.input.on_scroll(5.0, 6.0) == InputEventResult::accepted);
    CHECK(fixture.advance(1ms).timing.executed == 0);
    REQUIRE(fixture.input.on_focus(false) == InputEventResult::accepted);
    REQUIRE(fixture.input.on_focus(true) == InputEventResult::accepted);
    REQUIRE(fixture.input.on_key(Key::b, ButtonAction::press) == InputEventResult::accepted);
    REQUIRE(fixture.input.on_text(U'y') == InputEventResult::accepted);
    REQUIRE(fixture.input.on_scroll(0.5, -0.25) == InputEventResult::accepted);
    REQUIRE(fixture.input.on_cursor(20.0, 30.0) == InputEventResult::accepted);
    CHECK(fixture.advance(34ms).timing.executed == 2);
    REQUIRE(fixture.ticks.size() == 2);
    const auto& input = fixture.ticks[0].input;
    CHECK_FALSE(input.key(Key::a).held);
    CHECK_FALSE(input.key(Key::a).pressed);
    CHECK(input.key(Key::a).released);
    CHECK(input.key(Key::b).held);
    CHECK(input.key(Key::b).pressed);
    CHECK(input.focus_gained);
    CHECK(input.focus_lost);
    REQUIRE(input.text_size == 1);
    CHECK(input.text[0] == U'y');
    CHECK(input.scroll.x == 0.5);
    CHECK(input.scroll.y == -0.25);
    CHECK(input.cursor.valid);
    check_no_interval(fixture.ticks[1].input);
}

TEST_CASE("frame merged text retains a bounded prefix and releases despite overflow", "[frame]") {
    FrameFixture fixture;
    REQUIRE(fixture.input.on_key(Key::a, ButtonAction::press) == InputEventResult::accepted);
    for (std::size_t i = 0; i < shark::client::input_text_capacity; ++i) {
        REQUIRE(fixture.input.on_text(U'a') == InputEventResult::accepted);
    }
    CHECK(fixture.advance(1ms).timing.executed == 0);
    REQUIRE(fixture.input.on_text(U'b') == InputEventResult::accepted);
    REQUIRE(fixture.input.on_key(Key::a, ButtonAction::release) == InputEventResult::accepted);
    CHECK(fixture.advance(34ms).timing.executed == 2);
    REQUIRE(fixture.ticks.size() == 2);
    const auto& input = fixture.ticks[0].input;
    CHECK(input.text_overflow);
    REQUIRE(input.text_size == shark::client::input_text_capacity);
    for (const auto scalar : input.text) {
        CHECK(scalar == U'a');
    }
    CHECK(input.key(Key::a).released);
    check_no_interval(fixture.ticks[1].input);
}

TEST_CASE("frame merged scroll overflow rejects both components and later finite deltas work",
          "[frame]") {
    FrameFixture fixture;
    const auto maximum = std::numeric_limits<double>::max();
    REQUIRE(fixture.input.on_scroll(maximum, 1.0) == InputEventResult::accepted);
    CHECK(fixture.advance(1ms).timing.executed == 0);
    REQUIRE(fixture.input.on_scroll(maximum, 99.0) == InputEventResult::accepted);
    CHECK(fixture.advance(2ms).timing.executed == 0);
    REQUIRE(fixture.input.on_scroll(-maximum, 2.0) == InputEventResult::accepted);
    CHECK(fixture.advance(34ms).timing.executed == 2);
    REQUIRE(fixture.ticks.size() == 2);
    CHECK(fixture.ticks[0].input.scroll_overflow);
    CHECK(fixture.ticks[0].input.scroll.x == 0.0);
    CHECK(fixture.ticks[0].input.scroll.y == 3.0);
    check_no_interval(fixture.ticks[1].input);
}

TEST_CASE("frame pending overflow diagnostics survive focus cancellation", "[frame]") {
    FrameFixture fixture;
    for (std::size_t i = 0; i < shark::client::input_text_capacity; ++i) {
        REQUIRE(fixture.input.on_text(U'a') == InputEventResult::accepted);
    }
    REQUIRE(fixture.input.on_text(U'b') == InputEventResult::overflow);
    const auto maximum = std::numeric_limits<double>::max();
    REQUIRE(fixture.input.on_scroll(maximum, 0.0) == InputEventResult::accepted);
    REQUIRE(fixture.input.on_scroll(maximum, 0.0) == InputEventResult::overflow);
    CHECK(fixture.advance(1ms).timing.executed == 0);
    REQUIRE(fixture.input.on_focus(false) == InputEventResult::accepted);
    REQUIRE(fixture.input.on_focus(true) == InputEventResult::accepted);
    CHECK(fixture.advance(17ms).timing.executed == 1);
    REQUIRE(fixture.ticks.size() == 1);
    CHECK(fixture.ticks[0].input.text_size == 0);
    CHECK(fixture.ticks[0].input.text_overflow);
    CHECK(fixture.ticks[0].input.scroll.x == 0.0);
    CHECK(fixture.ticks[0].input.scroll_overflow);
}

TEST_CASE("frame paused step uses current positives and held state but no prior actions",
          "[frame]") {
    FrameFixture fixture;
    REQUIRE(fixture.input.on_key(Key::w, ButtonAction::press) == InputEventResult::accepted);
    REQUIRE(fixture.input.on_text(U'a') == InputEventResult::accepted);
    CHECK(fixture.advance(1ms).timing.executed == 0);
    REQUIRE(fixture.input.on_key(Key::x, ButtonAction::press) == InputEventResult::accepted);
    REQUIRE(fixture.input.on_key(Key::x, ButtonAction::release) == InputEventResult::accepted);
    REQUIRE(fixture.input.on_text(U'b') == InputEventResult::accepted);
    auto paused = fixture.advance(10ms, {.paused = true});
    CHECK(paused.paused);
    CHECK_FALSE(paused.presentation_suspended);
    CHECK(paused.timing.executed == 0);
    REQUIRE(fixture.input.on_key(Key::w, ButtonAction::repeat) == InputEventResult::accepted);
    REQUIRE(fixture.input.on_key(Key::space, ButtonAction::press) == InputEventResult::accepted);
    REQUIRE(fixture.input.on_text(U'c') == InputEventResult::accepted);
    REQUIRE(fixture.input.on_scroll(0.5, 1.0) == InputEventResult::accepted);
    auto stepped = fixture.advance(30s, {.paused = true, .single_step = true});
    CHECK(stepped.paused);
    CHECK(stepped.timing.executed == 1);
    CHECK(stepped.timing.interpolation_alpha() == 0.0);
    REQUIRE(fixture.ticks.size() == 1);
    const auto& input = fixture.ticks[0].input;
    CHECK(input.key(Key::w).held);
    CHECK_FALSE(input.key(Key::w).pressed);
    CHECK(input.key(Key::w).repeated);
    CHECK_FALSE(input.key(Key::x).pressed);
    CHECK(input.key(Key::x).released);
    CHECK(input.key(Key::space).pressed);
    REQUIRE(input.text_size == 1);
    CHECK(input.text[0] == U'c');
    CHECK(input.scroll.y == 1.0);
    CHECK(fixture.advance(60s, {.paused = true}).timing.executed == 0);
    CHECK(fixture.advance(90s, {.paused = true, .single_step = true}).timing.executed == 1);
    REQUIRE(fixture.ticks.size() == 2);
    CHECK(fixture.ticks[1].input.key(Key::w).held);
    check_no_interval(fixture.ticks[1].input);
}

TEST_CASE("frame resume discards paused actions and elapsed time including fractional debt",
          "[frame]") {
    FrameFixture fixture;
    CHECK(fixture.advance(16ms).timing.executed == 0);
    CHECK(fixture.advance(16ms + 1ns, {.paused = true}).timing.remainder_units == 0);
    REQUIRE(fixture.input.on_key(Key::a, ButtonAction::press) == InputEventResult::accepted);
    REQUIRE(fixture.input.on_text(U'a') == InputEventResult::accepted);
    CHECK(fixture.advance(1h, {.paused = true}).timing.executed == 0);
    REQUIRE(fixture.input.on_key(Key::a, ButtonAction::release) == InputEventResult::accepted);
    auto resumed = fixture.advance(2h);
    CHECK_FALSE(resumed.paused);
    CHECK(resumed.timing.executed == 0);
    CHECK(resumed.timing.remainder_units == 0);
    CHECK(fixture.advance(2h + 1ms).timing.executed == 0);
    REQUIRE(fixture.input.on_key(Key::b, ButtonAction::press) == InputEventResult::accepted);
    CHECK(fixture.advance(2h + 17ms).timing.executed == 1);
    REQUIRE(fixture.ticks.size() == 1);
    CHECK_FALSE(fixture.ticks[0].input.key(Key::a).pressed);
    CHECK(fixture.ticks[0].input.key(Key::a).released);
    CHECK(fixture.ticks[0].input.key(Key::b).pressed);
    CHECK(fixture.ticks[0].input.text_size == 0);
}

TEST_CASE("frame focus and iconify pause offline time without overriding manual pause", "[frame]") {
    FrameFixture fixture;
    REQUIRE(fixture.input.on_key(Key::a, ButtonAction::press) == InputEventResult::accepted);
    CHECK(fixture.advance(1ms).timing.executed == 0);
    REQUIRE(fixture.input.on_focus(false) == InputEventResult::accepted);
    auto unfocused = fixture.advance(1s);
    CHECK(unfocused.paused);
    CHECK_FALSE(unfocused.presentation_suspended);
    CHECK(unfocused.timing.executed == 0);
    REQUIRE(fixture.input.on_focus(true) == InputEventResult::accepted);
    auto iconified = fixture.advance(2s, {.paused = true, .iconified = true});
    CHECK(iconified.paused);
    CHECK(iconified.presentation_suspended);
    auto restored = fixture.advance(3s, {.paused = true});
    CHECK(restored.paused);
    CHECK_FALSE(restored.presentation_suspended);
    CHECK(restored.timing.executed == 0);
    CHECK(fixture.advance(4s, {.paused = true, .single_step = true}).timing.executed == 1);
    REQUIRE(fixture.ticks.size() == 1);
    CHECK_FALSE(fixture.ticks[0].input.key(Key::a).held);
    CHECK_FALSE(fixture.ticks[0].input.key(Key::a).pressed);
    CHECK(fixture.ticks[0].input.key(Key::a).released);
    CHECK(fixture.advance(5s).timing.executed == 0);
    CHECK(fixture.advance(5s + 17ms).timing.executed == 1);
}

TEST_CASE("frame iconification independently cancels pending held input", "[frame]") {
    FrameFixture fixture;
    REQUIRE(fixture.input.on_key(Key::a, ButtonAction::press) == InputEventResult::accepted);
    CHECK(fixture.advance(1ms).timing.executed == 0);
    auto report = fixture.advance(20ms, {.iconified = true});
    CHECK(report.paused);
    CHECK(report.presentation_suspended);
    CHECK(report.timing.executed == 0);
    REQUIRE(fixture.input.on_focus(false) == InputEventResult::accepted);
    REQUIRE(fixture.input.on_focus(true) == InputEventResult::accepted);
    CHECK(fixture.advance(1s).timing.executed == 0);
    CHECK(fixture.advance(1s + 17ms).timing.executed == 1);
    REQUIRE(fixture.ticks.size() == 1);
    CHECK_FALSE(fixture.ticks[0].input.key(Key::a).held);
    CHECK_FALSE(fixture.ticks[0].input.key(Key::a).pressed);
    CHECK(fixture.ticks[0].input.key(Key::a).released);
}

TEST_CASE("frame long delta uses existing eight-tick clamp and reports discarded debt", "[frame]") {
    FrameFixture fixture;
    REQUIRE(fixture.input.on_key(Key::a, ButtonAction::press) == InputEventResult::accepted);
    auto report = fixture.advance(2s);
    CHECK(report.timing.executed == 8);
    CHECK(report.timing.discarded == 7);
    CHECK(report.timing.pending == 0);
    CHECK(report.timing.clamped_delta);
    CHECK(report.timing.interpolation_alpha() == 0.0);
    REQUIRE(fixture.ticks.size() == 8);
    CHECK(fixture.ticks[0].input.key(Key::a).pressed);
    for (std::size_t i = 1; i < fixture.ticks.size(); ++i) {
        CHECK(fixture.ticks[i].input.key(Key::a).held);
        check_no_interval(fixture.ticks[i].input);
    }
    CHECK(fixture.loop->next_tick() == 8);
    CHECK(fixture.advance(2s).timing.executed == 0);
}

TEST_CASE("frame invalid fabricated snapshots reject before consuming pending input or time",
          "[frame]") {
    FrameFixture fixture;
    REQUIRE(fixture.input.on_key(Key::a, ButtonAction::press) == InputEventResult::accepted);
    CHECK(fixture.advance(1ms).timing.executed == 0);
    const auto valid = fixture.input.capture();
    std::array<InputSnapshot, 5> invalid{valid, valid, valid, valid, valid};
    invalid[0].text_size = shark::client::input_text_capacity + 1;
    invalid[1].scroll.x = std::numeric_limits<double>::infinity();
    invalid[2].cursor = {std::numeric_limits<double>::quiet_NaN(), 0.0, true};
    invalid[3].text_size = 1;
    invalid[3].text[0] = static_cast<char32_t>(0xD800);
    invalid[4].text_size = 1;
    invalid[4].text[0] = static_cast<char32_t>(0x110000);
    for (const auto& snapshot : invalid) {
        auto rejected = fixture.loop->advance(1h, snapshot, {}, fixture.callback);
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().code == ErrorCode::invalid_argument);
        CHECK(fixture.loop->next_tick() == 0);
        CHECK_FALSE(fixture.loop->faulted());
    }
    auto empty_callback = fixture.loop->advance(1h, valid, {}, OfflineInputCallback{});
    REQUIRE_FALSE(empty_callback);
    CHECK(empty_callback.error().code == ErrorCode::invalid_argument);
    CHECK(fixture.advance(17ms).timing.executed == 1);
    REQUIRE(fixture.ticks.size() == 1);
    CHECK(fixture.ticks[0].input.key(Key::a).pressed);
}

TEST_CASE("frame invalid single-step requests do not mutate pending actions or pause state",
          "[frame]") {
    FrameFixture fixture;
    REQUIRE(fixture.input.on_key(Key::a, ButtonAction::press) == InputEventResult::accepted);
    CHECK(fixture.advance(1ms).timing.executed == 0);
    const auto snapshot = fixture.input.capture();
    for (const auto control :
         std::array{OfflineFrameControl{.single_step = true},
                    OfflineFrameControl{.paused = true, .single_step = true, .iconified = true}}) {
        auto rejected = fixture.loop->advance(1h, snapshot, control, fixture.callback);
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().code == ErrorCode::invalid_argument);
    }
    auto unfocused = snapshot;
    unfocused.focused = false;
    auto rejected = fixture.loop->advance(1h, unfocused, {.paused = true, .single_step = true},
                                          fixture.callback);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == ErrorCode::invalid_argument);
    CHECK(fixture.advance(17ms).timing.executed == 1);
    REQUIRE(fixture.ticks.size() == 1);
    CHECK(fixture.ticks[0].input.key(Key::a).pressed);
}

TEST_CASE("frame callback failure preserves completed progress and never replays consumed input",
          "[frame]") {
    FrameFixture fixture;
    REQUIRE(fixture.input.on_key(Key::a, ButtonAction::press) == InputEventResult::accepted);
    std::uint32_t calls = 0;
    const OfflineInputCallback callback = [&](std::uint64_t index,
                                              const InputSnapshot& input) -> shark::Result<void> {
        ++calls;
        if (index == 0) {
            CHECK(input.key(Key::a).pressed);
            return {};
        }
        CHECK_FALSE(input.key(Key::a).pressed);
        return shark::Error{ErrorCode::execution_failed, "tick refused", {.operation = "fixture"}};
    };
    auto failed = fixture.loop->advance(50ms, fixture.input.capture(), {}, callback);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().context.operation == "fixture");
    CHECK(calls == 2);
    CHECK(fixture.loop->next_tick() == 1);
    CHECK(fixture.loop->faulted());
    auto repeated = fixture.loop->advance(1s, fixture.input.capture(), {}, callback);
    REQUIRE_FALSE(repeated);
    CHECK(repeated.error().code == failed.error().code);
    CHECK(repeated.error().message == failed.error().message);
    CHECK(repeated.error().context.operation == failed.error().context.operation);
    CHECK(calls == 2);
    CHECK(fixture.loop->next_tick() == 1);
}

TEST_CASE("frame thrown single-step callback faults without advancing or retrying", "[frame]") {
    FrameFixture fixture;
    std::uint32_t calls = 0;
    const OfflineInputCallback callback = [&](std::uint64_t,
                                              const InputSnapshot&) -> shark::Result<void> {
        ++calls;
        throw std::runtime_error{"fixture"};
    };
    auto failed = fixture.loop->advance(1s, fixture.input.capture(),
                                        {.paused = true, .single_step = true}, callback);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == ErrorCode::execution_failed);
    CHECK(fixture.loop->faulted());
    CHECK(fixture.loop->next_tick() == 0);
    auto repeated = fixture.loop->advance(2s, fixture.input.capture(),
                                          {.paused = true, .single_step = true}, callback);
    REQUIRE_FALSE(repeated);
    CHECK(calls == 1);
}

TEST_CASE("frame nested advance rejects without disturbing the outer pass", "[frame]") {
    FrameFixture fixture;
    REQUIRE(fixture.input.on_key(Key::a, ButtonAction::press) == InputEventResult::accepted);
    const auto snapshot = fixture.input.capture();
    std::uint32_t calls = 0;
    const OfflineInputCallback callback = [&](std::uint64_t index,
                                              const InputSnapshot& input) -> shark::Result<void> {
        ++calls;
        CHECK(input.key(Key::a).pressed == (index == 0));
        auto nested = fixture.loop->advance(2s, snapshot, {.paused = true}, fixture.callback);
        REQUIRE_FALSE(nested);
        CHECK(nested.error().code == ErrorCode::conflict);
        return {};
    };
    auto result = fixture.loop->advance(34ms, snapshot, {}, callback);
    REQUIRE(result);
    CHECK(result.value().timing.executed == 2);
    CHECK_FALSE(result.value().paused);
    CHECK(calls == 2);
    CHECK(fixture.ticks.empty());
    CHECK_FALSE(fixture.loop->faulted());
}

TEST_CASE("frame cancellation stops before another tick and remains terminal", "[frame]") {
    FrameFixture fixture;
    std::stop_source cancellation;
    std::uint32_t calls = 0;
    const OfflineInputCallback callback = [&](std::uint64_t,
                                              const InputSnapshot&) -> shark::Result<void> {
        ++calls;
        cancellation.request_stop();
        return {};
    };
    auto result = fixture.loop->advance(50ms, fixture.input.capture(), {}, callback,
                                        cancellation.get_token());
    REQUIRE(result);
    CHECK(result.value().timing.cancelled);
    CHECK(result.value().timing.executed == 1);
    CHECK(result.value().timing.pending == 2);
    CHECK(fixture.loop->next_tick() == 1);
    auto later = fixture.loop->advance(1s, fixture.input.capture(),
                                       {.paused = true, .single_step = true}, callback);
    REQUIRE(later);
    CHECK(later.value().timing.cancelled);
    CHECK(later.value().timing.executed == 0);
    CHECK(calls == 1);
}

TEST_CASE("frame cancellation before entry never invokes the callback", "[frame]") {
    FrameFixture fixture;
    std::stop_source cancellation;
    cancellation.request_stop();
    auto result = fixture.loop->advance(1s, fixture.input.capture(), {}, fixture.callback,
                                        cancellation.get_token());
    REQUIRE(result);
    CHECK(result.value().timing.cancelled);
    CHECK(result.value().timing.executed == 0);
    CHECK(fixture.ticks.empty());
    CHECK(fixture.loop->next_tick() == 0);
}

TEST_CASE("frame origin and exhausted tick indices use existing scheduler errors", "[frame]") {
    auto invalid = OfflineFrameLoop::create(-1ns);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == ErrorCode::invalid_argument);
    auto created = OfflineFrameLoop::create(0ns, std::numeric_limits<std::uint64_t>::max());
    REQUIRE(created);
    auto loop = std::move(created).value();
    InputSnapshot input;
    input.focused = true;
    std::uint32_t calls = 0;
    const OfflineInputCallback callback = [&](std::uint64_t,
                                              const InputSnapshot&) -> shark::Result<void> {
        ++calls;
        return {};
    };
    auto result = loop->advance(17ms, input, {}, callback);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::execution_failed);
    CHECK(loop->faulted());
    CHECK(calls == 0);
}

TEST_CASE("frame pause does not stop an independent authoritative schedule", "[frame]") {
    FrameFixture fixture;
    auto created = shark::ServerScheduler::create({}, 0ns);
    REQUIRE(created);
    auto server = std::move(created).value();
    std::uint32_t server_ticks = 0;
    const shark::TickCallback tick = [&](std::uint64_t) {
        ++server_ticks;
        return shark::Result<void>{};
    };
    REQUIRE(fixture.input.on_focus(false) == InputEventResult::accepted);
    CHECK(fixture.advance(250ms, {.paused = true, .iconified = true}).timing.executed == 0);
    auto report = server->poll(250ms, tick);
    REQUIRE(report);
    CHECK(report.value().executed == 5);
    CHECK(server_ticks == 5);
    CHECK(server->next_tick() == 5);
    CHECK(fixture.loop->next_tick() == 0);
}
