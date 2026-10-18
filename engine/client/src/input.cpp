#include "shark/client/input.hpp"

#include <cmath>

namespace shark::client {
namespace {

bool valid_action(ButtonAction action) noexcept {
    return action == ButtonAction::press || action == ButtonAction::release ||
           action == ButtonAction::repeat;
}

InputEventResult apply_button(ButtonState& state, ButtonAction action) noexcept {
    switch (action) {
    case ButtonAction::press:
        if (!state.held) {
            state.held = true;
            state.pressed = true;
        }
        return InputEventResult::accepted;
    case ButtonAction::release:
        if (state.held) {
            state.held = false;
            state.released = true;
        }
        return InputEventResult::accepted;
    case ButtonAction::repeat:
        if (!state.held) {
            return InputEventResult::invalid_transition;
        }
        state.repeated = true;
        return InputEventResult::accepted;
    }
    return InputEventResult::invalid_action;
}

void clear_edges(ButtonState& state) noexcept {
    state.pressed = false;
    state.released = false;
    state.repeated = false;
}

void cancel_button(ButtonState& state) noexcept {
    state.released = state.released || state.held;
    state.held = false;
    state.pressed = false;
    state.repeated = false;
}

} // namespace

ButtonState InputSnapshot::key(Key key) const noexcept {
    if (!valid(key)) {
        return {};
    }
    return keys[static_cast<std::size_t>(key) - 1];
}

ButtonState InputSnapshot::mouse_button(MouseButton button) const noexcept {
    if (!valid(button)) {
        return {};
    }
    return mouse_buttons[static_cast<std::size_t>(button) - 1];
}

InputEventResult InputState::on_key(Key key, ButtonAction action) noexcept {
    if (!valid(key)) {
        return InputEventResult::invalid_key;
    }
    if (!valid_action(action)) {
        return InputEventResult::invalid_action;
    }
    if (!state_.focused) {
        return InputEventResult::ignored_unfocused;
    }
    return apply_button(state_.keys[static_cast<std::size_t>(key) - 1], action);
}

InputEventResult InputState::on_mouse_button(MouseButton button, ButtonAction action) noexcept {
    if (!valid(button)) {
        return InputEventResult::invalid_button;
    }
    if (!valid_action(action) || action == ButtonAction::repeat) {
        return InputEventResult::invalid_action;
    }
    if (!state_.focused) {
        return InputEventResult::ignored_unfocused;
    }
    return apply_button(state_.mouse_buttons[static_cast<std::size_t>(button) - 1], action);
}

InputEventResult InputState::on_cursor(double x, double y) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return InputEventResult::invalid_position;
    }
    if (!state_.focused) {
        return InputEventResult::ignored_unfocused;
    }
    state_.cursor = {x, y, true};
    return InputEventResult::accepted;
}

InputEventResult InputState::on_scroll(double x, double y) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return InputEventResult::invalid_scroll;
    }
    if (!state_.focused) {
        return InputEventResult::ignored_unfocused;
    }
    const double next_x = state_.scroll.x + x;
    const double next_y = state_.scroll.y + y;
    if (!std::isfinite(next_x) || !std::isfinite(next_y)) {
        state_.scroll_overflow = true;
        return InputEventResult::overflow;
    }
    state_.scroll = {next_x, next_y};
    return InputEventResult::accepted;
}

InputEventResult InputState::on_text(char32_t scalar) noexcept {
    if (scalar > 0x10FFFF || (scalar >= 0xD800 && scalar <= 0xDFFF)) {
        return InputEventResult::invalid_scalar;
    }
    if (!state_.focused) {
        return InputEventResult::ignored_unfocused;
    }
    if (state_.text_size == input_text_capacity) {
        state_.text_overflow = true;
        return InputEventResult::overflow;
    }
    state_.text[state_.text_size] = scalar;
    ++state_.text_size;
    return InputEventResult::accepted;
}

InputEventResult InputState::on_focus(bool focused) noexcept {
    if (state_.focused == focused) {
        return InputEventResult::accepted;
    }
    state_.focused = focused;
    if (focused) {
        state_.focus_gained = true;
        return InputEventResult::accepted;
    }
    state_.focus_lost = true;
    for (auto& key : state_.keys) {
        cancel_button(key);
    }
    for (auto& button : state_.mouse_buttons) {
        cancel_button(button);
    }
    state_.cursor = {};
    state_.scroll = {};
    state_.text = {};
    state_.text_size = 0;
    return InputEventResult::accepted;
}

InputSnapshot InputState::capture() noexcept {
    const InputSnapshot result = state_;
    for (auto& key : state_.keys) {
        clear_edges(key);
    }
    for (auto& button : state_.mouse_buttons) {
        clear_edges(button);
    }
    state_.scroll = {};
    state_.text = {};
    state_.text_size = 0;
    state_.focus_gained = false;
    state_.focus_lost = false;
    state_.text_overflow = false;
    state_.scroll_overflow = false;
    return result;
}

} // namespace shark::client
