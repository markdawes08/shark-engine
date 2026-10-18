#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace shark::client {

// Engine-owned identifiers, not native key codes, characters or serialized IDs.
// The named keys describe keyboard locations; text arrives separately on_text.
enum class Key : std::uint16_t {
    unknown,
    space,
    apostrophe,
    comma,
    minus,
    period,
    slash,
    digit_0,
    digit_1,
    digit_2,
    digit_3,
    digit_4,
    digit_5,
    digit_6,
    digit_7,
    digit_8,
    digit_9,
    semicolon,
    equal,
    a,
    b,
    c,
    d,
    e,
    f,
    g,
    h,
    i,
    j,
    k,
    l,
    m,
    n,
    o,
    p,
    q,
    r,
    s,
    t,
    u,
    v,
    w,
    x,
    y,
    z,
    left_bracket,
    backslash,
    right_bracket,
    grave_accent,
    world_1,
    world_2,
    escape,
    enter,
    tab,
    backspace,
    insert,
    delete_key,
    right,
    left,
    down,
    up,
    page_up,
    page_down,
    home,
    end,
    caps_lock,
    scroll_lock,
    num_lock,
    print_screen,
    pause,
    f1,
    f2,
    f3,
    f4,
    f5,
    f6,
    f7,
    f8,
    f9,
    f10,
    f11,
    f12,
    f13,
    f14,
    f15,
    f16,
    f17,
    f18,
    f19,
    f20,
    f21,
    f22,
    f23,
    f24,
    f25,
    keypad_0,
    keypad_1,
    keypad_2,
    keypad_3,
    keypad_4,
    keypad_5,
    keypad_6,
    keypad_7,
    keypad_8,
    keypad_9,
    keypad_decimal,
    keypad_divide,
    keypad_multiply,
    keypad_subtract,
    keypad_add,
    keypad_enter,
    keypad_equal,
    left_shift,
    left_control,
    left_alt,
    left_super,
    right_shift,
    right_control,
    right_alt,
    right_super,
    menu,
    count
};

enum class MouseButton : std::uint8_t {
    unknown,
    left,
    right,
    middle,
    button_4,
    button_5,
    button_6,
    button_7,
    button_8,
    count
};

// Counts exclude unknown and count. Array slot zero represents the first valid ID.
inline constexpr std::size_t key_count = static_cast<std::size_t>(Key::count) - 1;
inline constexpr std::size_t mouse_button_count = static_cast<std::size_t>(MouseButton::count) - 1;
inline constexpr std::size_t input_text_capacity = 64;

[[nodiscard]] constexpr bool valid(Key key) noexcept {
    return key > Key::unknown && key < Key::count;
}
[[nodiscard]] constexpr bool valid(MouseButton button) noexcept {
    return button > MouseButton::unknown && button < MouseButton::count;
}

enum class ButtonAction { press, release, repeat };

// Fixed results keep callback ingestion allocation-free. An adapter handles or
// translates them to an owned diagnostic outside the callback boundary.
enum class InputEventResult {
    accepted,
    ignored_unfocused,
    invalid_key,
    invalid_button,
    invalid_action,
    invalid_scalar,
    invalid_position,
    invalid_scroll,
    invalid_transition,
    overflow
};

struct ButtonState {
    bool held{false};
    bool pressed{false};
    bool released{false};
    bool repeated{false};
    friend bool operator==(const ButtonState&, const ButtonState&) = default;
};

// Content-area coordinates, with the origin at top-left, +x right and +y down.
// They are neither framebuffer pixels nor relative/raw mouse motion. A future
// window adapter owns native conversion and high-DPI framebuffer scaling.
struct CursorPosition {
    double x{0.0};
    double y{0.0};
    bool valid{false};
    friend bool operator==(const CursorPosition&, const CursorPosition&) = default;
};

// Fractional native scroll units, independent of cursor/content coordinates.
struct ScrollOffset {
    double x{0.0};
    double y{0.0};
    friend bool operator==(const ScrollOffset&, const ScrollOffset&) = default;
};

// An owned observation of one capture interval, not an ordered event log.
// Edge/repeat flags coalesce multiple occurrences; a quick tap keeps both edges.
// Text preserves accepted scalar order. Mutating a copy cannot affect its source.
struct InputSnapshot {
    std::array<ButtonState, key_count> keys{};
    std::array<ButtonState, mouse_button_count> mouse_buttons{};
    CursorPosition cursor{};
    ScrollOffset scroll{};
    std::array<char32_t, input_text_capacity> text{};
    std::size_t text_size{0};
    bool focused{false};
    bool focus_gained{false};
    bool focus_lost{false};
    bool text_overflow{false};
    bool scroll_overflow{false};

    // Invalid identifiers safely return neutral state; valid() distinguishes
    // an invalid query from a valid, inactive control.
    [[nodiscard]] ButtonState key(Key key) const noexcept;
    [[nodiscard]] ButtonState mouse_button(MouseButton button) const noexcept;
    friend bool operator==(const InputSnapshot&, const InputSnapshot&) = default;
};

// One client application/window owner, serialized on its controlling thread.
// No method allocates, logs, blocks, calls user code, or touches a scheduler.
// There is no internal synchronization: callbacks and captures must not race.
class InputState final {
  public:
    InputState() noexcept = default;
    InputState(const InputState&) = delete;
    InputState& operator=(const InputState&) = delete;
    InputState(InputState&&) = delete;
    InputState& operator=(InputState&&) = delete;

    // Initially unfocused. Validate input before ignoring unfocused callbacks.
    // Duplicate press/release is accepted without creating an extra edge.
    // Keyboard repeat requires held=true; mouse repeat is invalid_action.
    [[nodiscard]] InputEventResult on_key(Key key, ButtonAction action) noexcept;
    [[nodiscard]] InputEventResult on_mouse_button(MouseButton button,
                                                   ButtonAction action) noexcept;
    [[nodiscard]] InputEventResult on_cursor(double x, double y) noexcept;
    [[nodiscard]] InputEventResult on_scroll(double x, double y) noexcept;
    [[nodiscard]] InputEventResult on_text(char32_t scalar) noexcept;

    // Loss cancels pending pressed/repeated/text/scroll, releases every held
    // control, preserves existing release edges, and invalidates the cursor.
    // Overflow flags remain visible until capture. Regain resurrects nothing.
    // Duplicate focus notifications do not reset or cancel additional state.
    [[nodiscard]] InputEventResult on_focus(bool focused) noexcept;

    // Returns a value and resets only interval data. Held, focus and cursor
    // survive captures. Text overflow retains its accepted prefix; scroll
    // overflow rejects both new components atomically. Neither blocks releases.
    [[nodiscard]] InputSnapshot capture() noexcept;

  private:
    InputSnapshot state_{};
};

} // namespace shark::client
