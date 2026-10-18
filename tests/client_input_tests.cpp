#include <shark/client/input.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <set>
#include <type_traits>
#include <utility>

namespace {

using shark::client::ButtonAction;
using shark::client::ButtonState;
using shark::client::InputEventResult;
using shark::client::InputSnapshot;
using shark::client::InputState;
using shark::client::Key;
using shark::client::MouseButton;

void focus(InputState& input) {
    REQUIRE(input.on_focus(true) == InputEventResult::accepted);
    static_cast<void>(input.capture());
}

void check_button(const ButtonState state, const bool held, const bool pressed, const bool released,
                  const bool repeated = false) {
    CHECK(state.held == held);
    CHECK(state.pressed == pressed);
    CHECK(state.released == released);
    CHECK(state.repeated == repeated);
}

static_assert(!std::is_copy_constructible_v<InputState>);
static_assert(!std::is_move_constructible_v<InputState>);
static_assert(std::is_copy_constructible_v<InputSnapshot>);
static_assert(std::is_nothrow_default_constructible_v<InputState>);
static_assert(noexcept(std::declval<InputState&>().on_key(Key::a, ButtonAction::press)));
static_assert(noexcept(std::declval<InputState&>().on_mouse_button(MouseButton::left,
                                                                   ButtonAction::press)));
static_assert(noexcept(std::declval<InputState&>().on_cursor(0.0, 0.0)));
static_assert(noexcept(std::declval<InputState&>().on_scroll(0.0, 0.0)));
static_assert(noexcept(std::declval<InputState&>().on_text(U'a')));
static_assert(noexcept(std::declval<InputState&>().on_focus(false)));
static_assert(noexcept(std::declval<InputState&>().capture()));

} // namespace

TEST_CASE("input starts unfocused and invalid queries are neutral", "[input]") {
    InputState input;
    const auto initial = input.capture();
    CHECK(initial == InputSnapshot{});
    CHECK(input.capture() == initial);
    for (const auto key :
         std::array{Key::unknown, Key::count, static_cast<Key>(std::uint16_t{65535})}) {
        CHECK_FALSE(shark::client::valid(key));
        CHECK(initial.key(key) == ButtonState{});
        CHECK(input.on_key(key, ButtonAction::press) == InputEventResult::invalid_key);
    }
    for (const auto button : std::array{MouseButton::unknown, MouseButton::count,
                                        static_cast<MouseButton>(std::uint8_t{255})}) {
        CHECK_FALSE(shark::client::valid(button));
        CHECK(initial.mouse_button(button) == ButtonState{});
        CHECK(input.on_mouse_button(button, ButtonAction::press) ==
              InputEventResult::invalid_button);
    }
    CHECK(input.capture() == initial);
}

TEST_CASE("keyboard captures preserve held state and consume edges exactly once", "[input]") {
    InputState input;
    focus(input);
    REQUIRE(input.on_key(Key::w, ButtonAction::press) == InputEventResult::accepted);
    REQUIRE(input.on_key(Key::w, ButtonAction::press) == InputEventResult::accepted);
    check_button(input.capture().key(Key::w), true, true, false);
    check_button(input.capture().key(Key::w), true, false, false);
    REQUIRE(input.on_key(Key::w, ButtonAction::press) == InputEventResult::accepted);
    check_button(input.capture().key(Key::w), true, false, false);
    REQUIRE(input.on_key(Key::w, ButtonAction::release) == InputEventResult::accepted);
    REQUIRE(input.on_key(Key::w, ButtonAction::release) == InputEventResult::accepted);
    check_button(input.capture().key(Key::w), false, false, true);
    check_button(input.capture().key(Key::w), false, false, false);
    REQUIRE(input.on_key(Key::w, ButtonAction::release) == InputEventResult::accepted);
    check_button(input.capture().key(Key::w), false, false, false);
}

TEST_CASE("quick taps and represses retain both edges without counting events", "[input]") {
    InputState input;
    focus(input);
    REQUIRE(input.on_key(Key::space, ButtonAction::press) == InputEventResult::accepted);
    REQUIRE(input.on_key(Key::space, ButtonAction::release) == InputEventResult::accepted);
    check_button(input.capture().key(Key::space), false, true, true);
    REQUIRE(input.on_key(Key::space, ButtonAction::press) == InputEventResult::accepted);
    REQUIRE(input.on_key(Key::space, ButtonAction::release) == InputEventResult::accepted);
    REQUIRE(input.on_key(Key::space, ButtonAction::press) == InputEventResult::accepted);
    check_button(input.capture().key(Key::space), true, true, true);
    check_button(input.capture().key(Key::space), true, false, false);

    for (int tap = 0; tap < 4; ++tap) {
        REQUIRE(input.on_mouse_button(MouseButton::left, ButtonAction::press) ==
                InputEventResult::accepted);
        REQUIRE(input.on_mouse_button(MouseButton::left, ButtonAction::release) ==
                InputEventResult::accepted);
    }
    check_button(input.capture().mouse_button(MouseButton::left), false, true, true);
    check_button(input.capture().mouse_button(MouseButton::left), false, false, false);
}

TEST_CASE("keyboard autorepeat cannot create a press or resurrect released input", "[input]") {
    InputState input;
    focus(input);
    CHECK(input.on_key(Key::a, ButtonAction::repeat) == InputEventResult::invalid_transition);
    check_button(input.capture().key(Key::a), false, false, false);
    REQUIRE(input.on_key(Key::a, ButtonAction::press) == InputEventResult::accepted);
    static_cast<void>(input.capture());
    REQUIRE(input.on_key(Key::a, ButtonAction::repeat) == InputEventResult::accepted);
    REQUIRE(input.on_key(Key::a, ButtonAction::repeat) == InputEventResult::accepted);
    check_button(input.capture().key(Key::a), true, false, false, true);
    check_button(input.capture().key(Key::a), true, false, false);
    REQUIRE(input.on_key(Key::a, ButtonAction::release) == InputEventResult::accepted);
    CHECK(input.on_key(Key::a, ButtonAction::repeat) == InputEventResult::invalid_transition);
    check_button(input.capture().key(Key::a), false, false, true);
    REQUIRE(input.on_mouse_button(MouseButton::left, ButtonAction::press) ==
            InputEventResult::accepted);
    CHECK(input.on_mouse_button(MouseButton::left, ButtonAction::repeat) ==
          InputEventResult::invalid_action);
    check_button(input.capture().mouse_button(MouseButton::left), true, true, false);
}

TEST_CASE("every supported key and mouse button has independent state", "[input]") {
    InputState input;
    focus(input);
    for (std::size_t value = 1; value <= shark::client::key_count; ++value) {
        const auto key = static_cast<Key>(value);
        REQUIRE(shark::client::valid(key));
        REQUIRE(input.on_key(key, ButtonAction::press) == InputEventResult::accepted);
    }
    for (std::size_t value = 1; value <= shark::client::mouse_button_count; ++value) {
        const auto button = static_cast<MouseButton>(value);
        REQUIRE(shark::client::valid(button));
        REQUIRE(input.on_mouse_button(button, ButtonAction::press) == InputEventResult::accepted);
    }
    const auto held = input.capture();
    for (std::size_t value = 1; value <= shark::client::key_count; ++value) {
        check_button(held.key(static_cast<Key>(value)), true, true, false);
        if (value % 2 == 0) {
            REQUIRE(input.on_key(static_cast<Key>(value), ButtonAction::release) ==
                    InputEventResult::accepted);
        }
    }
    for (std::size_t value = 1; value <= shark::client::mouse_button_count; ++value) {
        check_button(held.mouse_button(static_cast<MouseButton>(value)), true, true, false);
        if (value % 2 == 0) {
            REQUIRE(input.on_mouse_button(static_cast<MouseButton>(value), ButtonAction::release) ==
                    InputEventResult::accepted);
        }
    }
    const auto alternating = input.capture();
    for (std::size_t value = 1; value <= shark::client::key_count; ++value) {
        check_button(alternating.key(static_cast<Key>(value)), value % 2 != 0, false,
                     value % 2 == 0);
    }
    for (std::size_t value = 1; value <= shark::client::mouse_button_count; ++value) {
        check_button(alternating.mouse_button(static_cast<MouseButton>(value)), value % 2 != 0,
                     false, value % 2 == 0);
    }
    REQUIRE(input.on_focus(false) == InputEventResult::accepted);
    const auto lost = input.capture();
    for (std::size_t value = 1; value <= shark::client::key_count; ++value) {
        check_button(lost.key(static_cast<Key>(value)), false, false, value % 2 != 0);
    }
    for (std::size_t value = 1; value <= shark::client::mouse_button_count; ++value) {
        check_button(lost.mouse_button(static_cast<MouseButton>(value)), false, false,
                     value % 2 != 0);
    }
}

TEST_CASE("focus loss neutralizes held input and cancels pending actions", "[input]") {
    InputState input;
    focus(input);
    REQUIRE(input.on_key(Key::w, ButtonAction::press) == InputEventResult::accepted);
    REQUIRE(input.on_key(Key::w, ButtonAction::repeat) == InputEventResult::accepted);
    REQUIRE(input.on_key(Key::a, ButtonAction::press) == InputEventResult::accepted);
    REQUIRE(input.on_key(Key::a, ButtonAction::release) == InputEventResult::accepted);
    REQUIRE(input.on_mouse_button(MouseButton::left, ButtonAction::press) ==
            InputEventResult::accepted);
    REQUIRE(input.on_mouse_button(MouseButton::right, ButtonAction::press) ==
            InputEventResult::accepted);
    REQUIRE(input.on_mouse_button(MouseButton::right, ButtonAction::release) ==
            InputEventResult::accepted);
    REQUIRE(input.on_cursor(30.25, 40.5) == InputEventResult::accepted);
    REQUIRE(input.on_scroll(1.0, -2.0) == InputEventResult::accepted);
    REQUIRE(input.on_text(U'x') == InputEventResult::accepted);
    REQUIRE(input.on_focus(false) == InputEventResult::accepted);
    const auto lost = input.capture();
    CHECK_FALSE(lost.focused);
    CHECK(lost.focus_lost);
    CHECK_FALSE(lost.focus_gained);
    check_button(lost.key(Key::w), false, false, true);
    check_button(lost.key(Key::a), false, false, true);
    check_button(lost.mouse_button(MouseButton::left), false, false, true);
    check_button(lost.mouse_button(MouseButton::right), false, false, true);
    CHECK_FALSE(lost.cursor.valid);
    CHECK(lost.scroll == shark::client::ScrollOffset{});
    CHECK(lost.text_size == 0);
    CHECK(input.capture() == InputSnapshot{});
}

TEST_CASE("unfocused events are ignored and focus regain never restores held input", "[input]") {
    InputState input;
    for (const auto action : {ButtonAction::press, ButtonAction::release, ButtonAction::repeat}) {
        CHECK(input.on_key(Key::w, action) == InputEventResult::ignored_unfocused);
    }
    for (const auto action : {ButtonAction::press, ButtonAction::release}) {
        CHECK(input.on_mouse_button(MouseButton::left, action) ==
              InputEventResult::ignored_unfocused);
    }
    CHECK(input.on_cursor(10.0, 20.0) == InputEventResult::ignored_unfocused);
    CHECK(input.on_scroll(1.0, 2.0) == InputEventResult::ignored_unfocused);
    CHECK(input.on_text(U'a') == InputEventResult::ignored_unfocused);
    CHECK(input.capture() == InputSnapshot{});

    focus(input);
    REQUIRE(input.on_key(Key::w, ButtonAction::press) == InputEventResult::accepted);
    REQUIRE(input.on_mouse_button(MouseButton::left, ButtonAction::press) ==
            InputEventResult::accepted);
    REQUIRE(input.on_focus(false) == InputEventResult::accepted);
    CHECK(input.on_key(Key::w, ButtonAction::press) == InputEventResult::ignored_unfocused);
    CHECK(input.on_key(Key::w, ButtonAction::release) == InputEventResult::ignored_unfocused);
    CHECK(input.on_mouse_button(MouseButton::left, ButtonAction::release) ==
          InputEventResult::ignored_unfocused);
    REQUIRE(input.on_focus(true) == InputEventResult::accepted);
    CHECK(input.on_key(Key::w, ButtonAction::repeat) == InputEventResult::invalid_transition);
    const auto regained = input.capture();
    CHECK(regained.focused);
    CHECK(regained.focus_lost);
    CHECK(regained.focus_gained);
    check_button(regained.key(Key::w), false, false, true);
    check_button(regained.mouse_button(MouseButton::left), false, false, true);
    CHECK_FALSE(regained.cursor.valid);
    REQUIRE(input.on_key(Key::w, ButtonAction::press) == InputEventResult::accepted);
    check_button(input.capture().key(Key::w), true, true, false);
}

TEST_CASE("duplicate focus notifications do not cancel or invent interval state", "[input]") {
    InputState input;
    REQUIRE(input.on_focus(false) == InputEventResult::accepted);
    CHECK(input.capture() == InputSnapshot{});
    REQUIRE(input.on_focus(true) == InputEventResult::accepted);
    REQUIRE(input.on_key(Key::w, ButtonAction::press) == InputEventResult::accepted);
    REQUIRE(input.on_text(U'a') == InputEventResult::accepted);
    REQUIRE(input.on_focus(true) == InputEventResult::accepted);
    const auto first = input.capture();
    CHECK(first.focus_gained);
    CHECK_FALSE(first.focus_lost);
    CHECK(first.text_size == 1);
    check_button(first.key(Key::w), true, true, false);
    REQUIRE(input.on_focus(true) == InputEventResult::accepted);
    CHECK_FALSE(input.capture().focus_gained);
    REQUIRE(input.on_focus(false) == InputEventResult::accepted);
    REQUIRE(input.on_focus(false) == InputEventResult::accepted);
    const auto lost = input.capture();
    CHECK(lost.focus_lost);
    check_button(lost.key(Key::w), false, false, true);
    REQUIRE(input.on_focus(false) == InputEventResult::accepted);
    CHECK(input.capture() == InputSnapshot{});
}

TEST_CASE("cursor positions retain fractional content coordinates across captures", "[input]") {
    InputState input;
    focus(input);
    REQUIRE(input.on_cursor(100.25, 200.5) == InputEventResult::accepted);
    const auto first = input.capture();
    CHECK(first.cursor == shark::client::CursorPosition{100.25, 200.5, true});
    CHECK(input.capture().cursor == first.cursor);
    REQUIRE(input.on_cursor(-5.25, -9.75) == InputEventResult::accepted);
    CHECK(input.capture().cursor == shark::client::CursorPosition{-5.25, -9.75, true});
    const auto maximum = std::numeric_limits<double>::max();
    REQUIRE(input.on_cursor(maximum, -maximum) == InputEventResult::accepted);
    CHECK(input.capture().cursor == shark::client::CursorPosition{maximum, -maximum, true});
    REQUIRE(input.on_focus(false) == InputEventResult::accepted);
    REQUIRE(input.on_focus(true) == InputEventResult::accepted);
    CHECK_FALSE(input.capture().cursor.valid);
}

TEST_CASE("nonfinite coordinates reject both components without changing valid input", "[input]") {
    const std::array bad_values{std::numeric_limits<double>::quiet_NaN(),
                                std::numeric_limits<double>::infinity(),
                                -std::numeric_limits<double>::infinity()};
    for (const auto bad : bad_values) {
        for (const bool bad_x : {false, true}) {
            InputState input;
            focus(input);
            REQUIRE(input.on_cursor(5.5, -7.25) == InputEventResult::accepted);
            REQUIRE(input.on_scroll(3.25, -2.5) == InputEventResult::accepted);
            REQUIRE(input.on_key(Key::w, ButtonAction::press) == InputEventResult::accepted);
            const double x = bad_x ? bad : 30.0;
            const double y = bad_x ? 40.0 : bad;
            CHECK(input.on_cursor(x, y) == InputEventResult::invalid_position);
            CHECK(input.on_scroll(x, y) == InputEventResult::invalid_scroll);
            const auto snapshot = input.capture();
            CHECK(snapshot.cursor == shark::client::CursorPosition{5.5, -7.25, true});
            CHECK(snapshot.scroll == shark::client::ScrollOffset{3.25, -2.5});
            CHECK_FALSE(snapshot.scroll_overflow);
            check_button(snapshot.key(Key::w), true, true, false);
        }
    }
}

TEST_CASE("invalid identifiers actions and scalars are rejected even while unfocused", "[input]") {
    for (const bool focused : {false, true}) {
        InputState input;
        if (focused) {
            focus(input);
            REQUIRE(input.on_key(Key::a, ButtonAction::press) == InputEventResult::accepted);
            REQUIRE(input.on_mouse_button(MouseButton::left, ButtonAction::press) ==
                    InputEventResult::accepted);
        }
        const auto bad_action = static_cast<ButtonAction>(std::numeric_limits<int>::max());
        CHECK(input.on_key(Key::unknown, bad_action) == InputEventResult::invalid_key);
        CHECK(input.on_key(Key::a, bad_action) == InputEventResult::invalid_action);
        CHECK(input.on_mouse_button(MouseButton::unknown, bad_action) ==
              InputEventResult::invalid_button);
        CHECK(input.on_mouse_button(MouseButton::left, bad_action) ==
              InputEventResult::invalid_action);
        CHECK(input.on_mouse_button(MouseButton::left, ButtonAction::repeat) ==
              InputEventResult::invalid_action);
        for (const auto scalar : std::array<char32_t, 5>{0xD800, 0xDBFF, 0xDFFF, 0x110000,
                                                         std::numeric_limits<char32_t>::max()}) {
            CHECK(input.on_text(scalar) == InputEventResult::invalid_scalar);
        }
        CHECK(input.on_cursor(0.0, std::numeric_limits<double>::infinity()) ==
              InputEventResult::invalid_position);
        CHECK(input.on_scroll(std::numeric_limits<double>::quiet_NaN(), 0.0) ==
              InputEventResult::invalid_scroll);
        const auto snapshot = input.capture();
        CHECK(snapshot.focused == focused);
        check_button(snapshot.key(Key::a), focused, focused, false);
        check_button(snapshot.mouse_button(MouseButton::left), focused, focused, false);
        CHECK(snapshot.text_size == 0);
        CHECK_FALSE(snapshot.text_overflow);
        CHECK_FALSE(snapshot.scroll_overflow);
        CHECK_FALSE(snapshot.cursor.valid);
        CHECK(snapshot.scroll == shark::client::ScrollOffset{});
    }
}

TEST_CASE("text accepts Unicode scalars in order independently of keyboard state", "[input]") {
    constexpr std::array<char32_t, 8> scalars{0,      U'a',   U'\u00E9', 0xD7FF,
                                              0xE000, 0xFFFF, 0x10000,   0x10FFFF};
    InputState input;
    focus(input);
    REQUIRE(input.on_key(Key::a, ButtonAction::press) == InputEventResult::accepted);
    for (const auto scalar : scalars) {
        REQUIRE(input.on_text(scalar) == InputEventResult::accepted);
    }
    const auto snapshot = input.capture();
    REQUIRE(snapshot.text_size == scalars.size());
    for (std::size_t index = 0; index < scalars.size(); ++index) {
        CHECK(snapshot.text[index] == scalars[index]);
    }
    check_button(snapshot.key(Key::a), true, true, false);
    CHECK_FALSE(snapshot.text_overflow);
    const auto next = input.capture();
    CHECK(next.text_size == 0);
    check_button(next.key(Key::a), true, false, false);
}

TEST_CASE("text overflow preserves the accepted prefix and cannot block releases", "[input]") {
    InputState input;
    focus(input);
    REQUIRE(input.on_key(Key::a, ButtonAction::press) == InputEventResult::accepted);
    for (std::size_t index = 0; index < shark::client::input_text_capacity; ++index) {
        REQUIRE(input.on_text(static_cast<char32_t>(0x10000 + index)) ==
                InputEventResult::accepted);
    }
    CHECK(input.on_text(U'x') == InputEventResult::overflow);
    CHECK(input.on_text(U'y') == InputEventResult::overflow);
    CHECK(input.on_text(0xD800) == InputEventResult::invalid_scalar);
    REQUIRE(input.on_key(Key::a, ButtonAction::release) == InputEventResult::accepted);
    const auto full = input.capture();
    REQUIRE(full.text_size == shark::client::input_text_capacity);
    CHECK(full.text_overflow);
    for (std::size_t index = 0; index < full.text_size; ++index) {
        CHECK(full.text[index] == static_cast<char32_t>(0x10000 + index));
    }
    check_button(full.key(Key::a), false, true, true);
    REQUIRE(input.on_text(U'z') == InputEventResult::accepted);
    const auto next = input.capture();
    REQUIRE(next.text_size == 1);
    CHECK(next.text[0] == U'z');
    CHECK_FALSE(next.text_overflow);
}

TEST_CASE("fractional scroll accumulates and overflow rejects the whole new pair", "[input]") {
    InputState input;
    focus(input);
    REQUIRE(input.on_scroll(0.25, -1.5) == InputEventResult::accepted);
    REQUIRE(input.on_scroll(-0.75, 0.25) == InputEventResult::accepted);
    CHECK(input.capture().scroll == shark::client::ScrollOffset{-0.5, -1.25});
    CHECK(input.capture().scroll == shark::client::ScrollOffset{});

    const auto maximum = std::numeric_limits<double>::max();
    for (const double sign : {-1.0, 1.0}) {
        for (const bool overflow_x : {false, true}) {
            const double initial_x = overflow_x ? sign * maximum : 3.0;
            const double initial_y = overflow_x ? -4.0 : sign * maximum;
            REQUIRE(input.on_scroll(initial_x, initial_y) == InputEventResult::accepted);
            CHECK(input.on_scroll(overflow_x ? sign * maximum : 7.0,
                                  overflow_x ? 8.0 : sign * maximum) == InputEventResult::overflow);
            const auto overflow = input.capture();
            CHECK(overflow.scroll == shark::client::ScrollOffset{initial_x, initial_y});
            CHECK(overflow.scroll_overflow);
            const auto cleared = input.capture();
            CHECK(cleared.scroll == shark::client::ScrollOffset{});
            CHECK_FALSE(cleared.scroll_overflow);
        }
    }
}

TEST_CASE("focus cancellation preserves overflow evidence until the next capture", "[input]") {
    InputState input;
    focus(input);
    for (std::size_t index = 0; index < shark::client::input_text_capacity; ++index) {
        REQUIRE(input.on_text(U'a') == InputEventResult::accepted);
    }
    REQUIRE(input.on_text(U'b') == InputEventResult::overflow);
    const auto maximum = std::numeric_limits<double>::max();
    REQUIRE(input.on_scroll(maximum, maximum) == InputEventResult::accepted);
    REQUIRE(input.on_scroll(maximum, maximum) == InputEventResult::overflow);
    REQUIRE(input.on_key(Key::w, ButtonAction::press) == InputEventResult::accepted);
    REQUIRE(input.on_mouse_button(MouseButton::left, ButtonAction::press) ==
            InputEventResult::accepted);
    REQUIRE(input.on_focus(false) == InputEventResult::accepted);
    REQUIRE(input.on_focus(true) == InputEventResult::accepted);
    REQUIRE(input.on_text(U'c') == InputEventResult::accepted);
    REQUIRE(input.on_scroll(0.5, -0.25) == InputEventResult::accepted);
    const auto snapshot = input.capture();
    CHECK(snapshot.text_overflow);
    CHECK(snapshot.scroll_overflow);
    REQUIRE(snapshot.text_size == 1);
    CHECK(snapshot.text[0] == U'c');
    CHECK(snapshot.scroll == shark::client::ScrollOffset{0.5, -0.25});
    check_button(snapshot.key(Key::w), false, false, true);
    check_button(snapshot.mouse_button(MouseButton::left), false, false, true);
    const auto cleared = input.capture();
    CHECK_FALSE(cleared.text_overflow);
    CHECK_FALSE(cleared.scroll_overflow);
}

TEST_CASE("snapshots own their values after captures mutations and owner destruction", "[input]") {
    const auto saved = [] {
        InputState input;
        focus(input);
        REQUIRE(input.on_key(Key::w, ButtonAction::press) == InputEventResult::accepted);
        REQUIRE(input.on_text(U'\u00E9') == InputEventResult::accepted);
        REQUIRE(input.on_cursor(5.25, -9.5) == InputEventResult::accepted);
        const auto snapshot = input.capture();
        REQUIRE(input.on_focus(false) == InputEventResult::accepted);
        static_cast<void>(input.capture());
        return snapshot;
    }();
    auto changed_copy = saved;
    changed_copy.keys.fill({});
    changed_copy.text.fill(U'x');
    changed_copy.cursor = {};
    check_button(saved.key(Key::w), true, true, false);
    REQUIRE(saved.text_size == 1);
    CHECK(saved.text[0] == U'\u00E9');
    CHECK(saved.cursor == shark::client::CursorPosition{5.25, -9.5, true});
    CHECK(changed_copy != saved);

    InputState first;
    InputState second;
    focus(first);
    focus(second);
    REQUIRE(first.on_key(Key::a, ButtonAction::press) == InputEventResult::accepted);
    REQUIRE(first.on_mouse_button(MouseButton::left, ButtonAction::press) ==
            InputEventResult::accepted);
    REQUIRE(second.on_key(Key::w, ButtonAction::press) == InputEventResult::accepted);
    REQUIRE(first.on_focus(false) == InputEventResult::accepted);
    const auto other = second.capture();
    CHECK(other.focused);
    CHECK_FALSE(other.focus_lost);
    check_button(other.key(Key::w), true, true, false);
    check_button(other.key(Key::a), false, false, false);
    check_button(other.mouse_button(MouseButton::left), false, false, false);
}

TEST_CASE("seeded input traces agree with an independent set based transition model", "[input]") {
    constexpr std::array keys{Key::a, Key::w, Key::space, Key::enter, Key::left_shift, Key::menu};
    constexpr std::array buttons{MouseButton::left, MouseButton::button_8};
    constexpr std::size_t control_count = keys.size() + buttons.size();
    constexpr std::array actions{ButtonAction::press, ButtonAction::release, ButtonAction::repeat};
    std::mt19937 random{1337};
    std::set<std::size_t> held;
    std::set<std::size_t> pressed;
    std::set<std::size_t> released;
    std::set<std::size_t> repeated;
    bool focused = false;
    bool gained = false;
    bool lost = false;
    InputState input;
    const auto check_capture = [&] {
        const auto snapshot = input.capture();
        CHECK(snapshot.focused == focused);
        CHECK(snapshot.focus_gained == gained);
        CHECK(snapshot.focus_lost == lost);
        for (std::size_t index = 0; index < control_count; ++index) {
            const auto state = index < keys.size()
                                   ? snapshot.key(keys[index])
                                   : snapshot.mouse_button(buttons[index - keys.size()]);
            check_button(state, held.contains(index), pressed.contains(index),
                         released.contains(index), repeated.contains(index));
        }
        pressed.clear();
        released.clear();
        repeated.clear();
        gained = false;
        lost = false;
    };
    for (std::size_t event = 0; event < 4096; ++event) {
        const auto kind = random() % 10U;
        if (kind < 6U) {
            const std::size_t control = static_cast<std::size_t>(random()) % control_count;
            const auto action = actions[static_cast<std::size_t>(random() % 3U)];
            auto expected = InputEventResult::accepted;
            if (control >= keys.size() && action == ButtonAction::repeat) {
                expected = InputEventResult::invalid_action;
            } else if (!focused) {
                expected = InputEventResult::ignored_unfocused;
            } else if (action == ButtonAction::repeat) {
                if (held.contains(control)) {
                    repeated.insert(control);
                } else {
                    expected = InputEventResult::invalid_transition;
                }
            } else if (action == ButtonAction::press) {
                if (held.insert(control).second) {
                    pressed.insert(control);
                }
            } else if (held.erase(control) != 0) {
                released.insert(control);
            }
            const auto result = control < keys.size()
                                    ? input.on_key(keys[control], action)
                                    : input.on_mouse_button(buttons[control - keys.size()], action);
            CHECK(result == expected);
        } else if (kind < 8U) {
            const bool next_focus = kind == 7U;
            REQUIRE(input.on_focus(next_focus) == InputEventResult::accepted);
            if (focused != next_focus) {
                focused = next_focus;
                if (focused) {
                    gained = true;
                } else {
                    lost = true;
                    released.insert(held.begin(), held.end());
                    held.clear();
                    pressed.clear();
                    repeated.clear();
                }
            }
        } else {
            check_capture();
        }
    }
    check_capture();
}
