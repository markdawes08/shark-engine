#include <shark/client/frame_loop.hpp>

#include <cmath>
#include <utility>

namespace shark::client {
namespace {

Error frame_error(ErrorCode code, const char* operation, const char* message) {
    return {code, message, {.operation = operation}};
}

bool valid_snapshot(const InputSnapshot& input) noexcept {
    if (input.text_size > input_text_capacity || !std::isfinite(input.scroll.x) ||
        !std::isfinite(input.scroll.y) ||
        (input.cursor.valid &&
         (!std::isfinite(input.cursor.x) || !std::isfinite(input.cursor.y)))) {
        return false;
    }
    for (std::size_t i = 0; i < input.text_size; ++i) {
        const auto scalar = input.text[i];
        if (scalar > 0x10FFFF || (scalar >= 0xD800 && scalar <= 0xDFFF)) {
            return false;
        }
    }
    return true;
}

void clear_interval(InputSnapshot& input) noexcept {
    for (auto& key : input.keys) {
        key.pressed = key.released = key.repeated = false;
    }
    for (auto& button : input.mouse_buttons) {
        button.pressed = button.released = button.repeated = false;
    }
    input.scroll = {};
    input.text = {};
    input.text_size = 0;
    input.focus_gained = input.focus_lost = false;
    input.text_overflow = input.scroll_overflow = false;
}

void cancel_positive(InputSnapshot& input, bool release_held) noexcept {
    const auto cancel = [release_held](ButtonState& button) {
        button.pressed = button.repeated = false;
        if (release_held) {
            button.released = button.released || button.held;
            button.held = false;
        }
    };
    for (auto& key : input.keys) {
        cancel(key);
    }
    for (auto& button : input.mouse_buttons) {
        cancel(button);
    }
    input.scroll = {};
    input.text = {};
    input.text_size = 0;
    if (release_held) {
        input.cursor = {};
    }
}

void merge(InputSnapshot& pending, const InputSnapshot& input) noexcept {
    if (input.focus_lost || !input.focused) {
        cancel_positive(pending, true);
    }
    const auto merge_button = [](ButtonState& target, const ButtonState& source) {
        target.held = source.held;
        target.pressed = target.pressed || source.pressed;
        target.released = target.released || source.released;
        target.repeated = target.repeated || source.repeated;
    };
    for (std::size_t i = 0; i < key_count; ++i) {
        merge_button(pending.keys[i], input.keys[i]);
    }
    for (std::size_t i = 0; i < mouse_button_count; ++i) {
        merge_button(pending.mouse_buttons[i], input.mouse_buttons[i]);
    }
    pending.cursor = input.cursor;
    pending.focused = input.focused;
    pending.focus_gained = pending.focus_gained || input.focus_gained;
    pending.focus_lost = pending.focus_lost || input.focus_lost;
    pending.text_overflow = pending.text_overflow || input.text_overflow;
    pending.scroll_overflow = pending.scroll_overflow || input.scroll_overflow;
    for (std::size_t i = 0; i < input.text_size; ++i) {
        if (pending.text_size == input_text_capacity) {
            pending.text_overflow = true;
            break;
        }
        pending.text[pending.text_size++] = input.text[i];
    }
    const auto x = pending.scroll.x + input.scroll.x;
    const auto y = pending.scroll.y + input.scroll.y;
    if (std::isfinite(x) && std::isfinite(y)) {
        pending.scroll = {x, y};
    } else {
        pending.scroll_overflow = true;
    }
}

struct AdvanceGuard {
    bool& active;
    explicit AdvanceGuard(bool& value) noexcept : active(value) {
        active = true;
    }
    ~AdvanceGuard() {
        active = false;
    }
};
} // namespace

Result<std::unique_ptr<OfflineFrameLoop>> OfflineFrameLoop::create(MonotonicTime origin,
                                                                   std::uint64_t first_tick) {
    auto scheduler = OfflineScheduler::create(origin, first_tick);
    if (!scheduler) {
        return scheduler.error();
    }
    return std::unique_ptr<OfflineFrameLoop>{new OfflineFrameLoop{std::move(scheduler).value()}};
}

OfflineFrameLoop::OfflineFrameLoop(std::unique_ptr<OfflineScheduler> scheduler) noexcept
    : scheduler_(std::move(scheduler)) {}

Result<OfflineFrameReport> OfflineFrameLoop::advance(MonotonicTime now, const InputSnapshot& input,
                                                     OfflineFrameControl control,
                                                     const OfflineInputCallback& callback,
                                                     std::stop_token stop) {
    if (advancing_) {
        return frame_error(ErrorCode::conflict, "client.frame.reentrant",
                           "An offline frame is already active");
    }
    if (!callback || !valid_snapshot(input)) {
        return frame_error(ErrorCode::invalid_argument, "client.frame.input",
                           "A callback and a bounded finite input snapshot are required");
    }
    const bool inactive = !input.focused || control.iconified;
    if (control.single_step && (!control.paused || inactive)) {
        return frame_error(ErrorCode::invalid_argument, "client.frame.step",
                           "Single step requires manual pause and an active window");
    }
    AdvanceGuard guard{advancing_};
    const bool pause = control.paused || inactive;
    // Do not leak pre-pause actions into a later step. Paused observations still
    // update held/released state; only the explicit step's current positives run.
    if (pause || paused_) {
        cancel_positive(pending_, false);
    }
    merge(pending_, input);
    if (inactive) {
        cancel_positive(pending_, true);
    } else if (pause && !control.single_step) {
        cancel_positive(pending_, false);
    }
    paused_ = pause;
    const TickCallback tick = [&](std::uint64_t index) -> Result<void> {
        const auto consumed = pending_;
        clear_interval(pending_);
        return callback(index, consumed);
    };
    auto result = scheduler_->poll(now, tick, OfflineControl{pause, control.single_step}, stop);
    if (!result) {
        return result.error();
    }
    if (result.value().cancelled) {
        pending_ = {};
    }
    return OfflineFrameReport{result.value(), pause, control.iconified};
}

std::uint64_t OfflineFrameLoop::next_tick() const noexcept {
    return scheduler_->next_tick();
}
bool OfflineFrameLoop::faulted() const noexcept {
    return scheduler_->faulted();
}

} // namespace shark::client
