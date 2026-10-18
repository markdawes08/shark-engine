#include <shark/client/window.hpp>

#include "../engine/client/src/window_backend.hpp"
#include "../engine/client/src/window_loop.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using shark::ErrorCode;
using shark::client::ButtonAction;
using shark::client::ButtonState;
using shark::client::ContentScale;
using shark::client::CursorPosition;
using shark::client::Key;
using shark::client::MouseButton;
using shark::client::OfflineFrameLoop;
using shark::client::Window;
using shark::client::WindowConfig;
using shark::client::WindowExtent;
using shark::client::WindowState;
using shark::client::detail::BackendClaim;
using shark::client::detail::pump_offline_window;
using shark::client::detail::WindowBackend;
using shark::client::detail::WindowCommand;
using shark::client::detail::WindowEventSink;
using namespace std::chrono_literals;

enum class Step {
    claim,
    initialize,
    create,
    install,
    query,
    poll,
    command,
    uninstall,
    destroy,
    terminate,
    release,
    delete_backend
};

struct BackendState {
    std::array<Step, 512> steps{};
    std::size_t step_count{0};
    BackendClaim claim_result{BackendClaim::acquired};
    bool fail_initialize{false};
    bool fail_create{false};
    bool fail_install{false};
    bool fail_query{false};
    bool fail_poll{false};
    bool fail_command{false};
    int native_error_code{0};
    bool lease{false};
    bool* shared_lease{&lease};
    bool focus_loss_on_poll{false};
    WindowEventSink* sink{nullptr};
    WindowState queried{{800, 600}, {1600, 1200}, {1.5F, 1.25F}, true, false, true, false};
    WindowExtent created_content{};
    bool created_visible{false};
    bool created_resizable{false};
    WindowCommand last_command{WindowCommand::resize};
    WindowExtent commanded_size{};

    void record(const Step step) noexcept {
        if (step_count < steps.size()) {
            steps[step_count++] = step;
        }
    }
    [[nodiscard]] std::vector<Step> recorded() const {
        return {steps.begin(), steps.begin() + static_cast<std::ptrdiff_t>(step_count)};
    }
};

class TestBackend final : public WindowBackend {
  public:
    explicit TestBackend(BackendState& state) noexcept : state_(state) {}
    ~TestBackend() override {
        state_.record(Step::delete_backend);
    }
    BackendClaim claim() noexcept override {
        state_.record(Step::claim);
        if (state_.claim_result != BackendClaim::acquired) {
            return state_.claim_result;
        }
        if (*state_.shared_lease) {
            return BackendClaim::conflict;
        }
        *state_.shared_lease = true;
        return BackendClaim::acquired;
    }
    bool initialize() noexcept override {
        state_.record(Step::initialize);
        return !state_.fail_initialize;
    }
    bool create(const WindowConfig& config) noexcept override {
        state_.record(Step::create);
        state_.created_content = config.content;
        state_.created_visible = config.visible;
        state_.created_resizable = config.resizable;
        return !state_.fail_create;
    }
    bool install_callbacks(WindowEventSink& sink) noexcept override {
        state_.record(Step::install);
        state_.sink = &sink;
        return !state_.fail_install;
    }
    bool query(WindowState& state) noexcept override {
        state_.record(Step::query);
        if (state_.fail_query) {
            return false;
        }
        state = state_.queried;
        return true;
    }
    bool poll() noexcept override {
        state_.record(Step::poll);
        if (state_.focus_loss_on_poll && state_.sink != nullptr) {
            state_.queried.focused = false;
            state_.sink->focus(false);
        }
        return !state_.fail_poll;
    }
    bool command(const WindowCommand command, const WindowExtent size) noexcept override {
        state_.record(Step::command);
        state_.last_command = command;
        state_.commanded_size = size;
        if (state_.fail_command) {
            return false;
        }
        switch (command) {
        case WindowCommand::resize:
            state_.queried.content = size;
            break;
        case WindowCommand::iconify:
            state_.queried.iconified = true;
            break;
        case WindowCommand::restore:
            state_.queried.iconified = false;
            break;
        case WindowCommand::show:
            state_.queried.visible = true;
            break;
        case WindowCommand::focus:
            state_.queried.focused = true;
            break;
        case WindowCommand::request_close:
            state_.queried.close_requested = true;
            break;
        }
        return true;
    }
    void uninstall_callbacks() noexcept override {
        state_.record(Step::uninstall);
        state_.sink = nullptr;
    }
    void destroy() noexcept override {
        state_.record(Step::destroy);
    }
    void terminate() noexcept override {
        state_.record(Step::terminate);
    }
    void release() noexcept override {
        state_.record(Step::release);
        *state_.shared_lease = false;
        state_.native_error_code = 0;
    }
    int last_error_code() const noexcept override {
        return state_.native_error_code;
    }
    void* native_handle() const noexcept override {
        return nullptr;
    }

  private:
    BackendState& state_;
};

[[nodiscard]] auto create(BackendState& backend, const WindowConfig& config = {}) {
    return shark::client::detail::create_window(config, std::make_unique<TestBackend>(backend));
}

[[nodiscard]] std::unique_ptr<Window> open(BackendState& backend, const WindowConfig& config = {}) {
    auto result = create(backend, config);
    REQUIRE(result);
    REQUIRE(backend.sink != nullptr);
    return std::move(result).value();
}

template <typename T> void check_error(const shark::Result<T>& result, const ErrorCode code) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == code);
    CHECK_FALSE(result.error().message.empty());
    CHECK_FALSE(result.error().context.operation.empty());
}

[[nodiscard]] WindowState state_of(const Window& window) {
    const auto result = window.state();
    REQUIRE(result);
    return result.value();
}

[[nodiscard]] shark::client::InputSnapshot capture(Window& window) {
    const auto result = window.capture_input();
    REQUIRE(result);
    return result.value();
}

[[nodiscard]] std::unique_ptr<OfflineFrameLoop> frame_loop() {
    auto result = OfflineFrameLoop::create(0ns);
    REQUIRE(result);
    return std::move(result).value();
}

// Values are the locked GLFW 3.4 key catalog. The fixture deliberately does not
// include GLFW headers or initialize a native library/display.
constexpr std::array native_keys{std::pair{32, Key::space},
                                 std::pair{39, Key::apostrophe},
                                 std::pair{44, Key::comma},
                                 std::pair{45, Key::minus},
                                 std::pair{46, Key::period},
                                 std::pair{47, Key::slash},
                                 std::pair{48, Key::digit_0},
                                 std::pair{49, Key::digit_1},
                                 std::pair{50, Key::digit_2},
                                 std::pair{51, Key::digit_3},
                                 std::pair{52, Key::digit_4},
                                 std::pair{53, Key::digit_5},
                                 std::pair{54, Key::digit_6},
                                 std::pair{55, Key::digit_7},
                                 std::pair{56, Key::digit_8},
                                 std::pair{57, Key::digit_9},
                                 std::pair{59, Key::semicolon},
                                 std::pair{61, Key::equal},
                                 std::pair{65, Key::a},
                                 std::pair{66, Key::b},
                                 std::pair{67, Key::c},
                                 std::pair{68, Key::d},
                                 std::pair{69, Key::e},
                                 std::pair{70, Key::f},
                                 std::pair{71, Key::g},
                                 std::pair{72, Key::h},
                                 std::pair{73, Key::i},
                                 std::pair{74, Key::j},
                                 std::pair{75, Key::k},
                                 std::pair{76, Key::l},
                                 std::pair{77, Key::m},
                                 std::pair{78, Key::n},
                                 std::pair{79, Key::o},
                                 std::pair{80, Key::p},
                                 std::pair{81, Key::q},
                                 std::pair{82, Key::r},
                                 std::pair{83, Key::s},
                                 std::pair{84, Key::t},
                                 std::pair{85, Key::u},
                                 std::pair{86, Key::v},
                                 std::pair{87, Key::w},
                                 std::pair{88, Key::x},
                                 std::pair{89, Key::y},
                                 std::pair{90, Key::z},
                                 std::pair{91, Key::left_bracket},
                                 std::pair{92, Key::backslash},
                                 std::pair{93, Key::right_bracket},
                                 std::pair{96, Key::grave_accent},
                                 std::pair{161, Key::world_1},
                                 std::pair{162, Key::world_2},
                                 std::pair{256, Key::escape},
                                 std::pair{257, Key::enter},
                                 std::pair{258, Key::tab},
                                 std::pair{259, Key::backspace},
                                 std::pair{260, Key::insert},
                                 std::pair{261, Key::delete_key},
                                 std::pair{262, Key::right},
                                 std::pair{263, Key::left},
                                 std::pair{264, Key::down},
                                 std::pair{265, Key::up},
                                 std::pair{266, Key::page_up},
                                 std::pair{267, Key::page_down},
                                 std::pair{268, Key::home},
                                 std::pair{269, Key::end},
                                 std::pair{280, Key::caps_lock},
                                 std::pair{281, Key::scroll_lock},
                                 std::pair{282, Key::num_lock},
                                 std::pair{283, Key::print_screen},
                                 std::pair{284, Key::pause},
                                 std::pair{290, Key::f1},
                                 std::pair{291, Key::f2},
                                 std::pair{292, Key::f3},
                                 std::pair{293, Key::f4},
                                 std::pair{294, Key::f5},
                                 std::pair{295, Key::f6},
                                 std::pair{296, Key::f7},
                                 std::pair{297, Key::f8},
                                 std::pair{298, Key::f9},
                                 std::pair{299, Key::f10},
                                 std::pair{300, Key::f11},
                                 std::pair{301, Key::f12},
                                 std::pair{302, Key::f13},
                                 std::pair{303, Key::f14},
                                 std::pair{304, Key::f15},
                                 std::pair{305, Key::f16},
                                 std::pair{306, Key::f17},
                                 std::pair{307, Key::f18},
                                 std::pair{308, Key::f19},
                                 std::pair{309, Key::f20},
                                 std::pair{310, Key::f21},
                                 std::pair{311, Key::f22},
                                 std::pair{312, Key::f23},
                                 std::pair{313, Key::f24},
                                 std::pair{314, Key::f25},
                                 std::pair{320, Key::keypad_0},
                                 std::pair{321, Key::keypad_1},
                                 std::pair{322, Key::keypad_2},
                                 std::pair{323, Key::keypad_3},
                                 std::pair{324, Key::keypad_4},
                                 std::pair{325, Key::keypad_5},
                                 std::pair{326, Key::keypad_6},
                                 std::pair{327, Key::keypad_7},
                                 std::pair{328, Key::keypad_8},
                                 std::pair{329, Key::keypad_9},
                                 std::pair{330, Key::keypad_decimal},
                                 std::pair{331, Key::keypad_divide},
                                 std::pair{332, Key::keypad_multiply},
                                 std::pair{333, Key::keypad_subtract},
                                 std::pair{334, Key::keypad_add},
                                 std::pair{335, Key::keypad_enter},
                                 std::pair{336, Key::keypad_equal},
                                 std::pair{340, Key::left_shift},
                                 std::pair{341, Key::left_control},
                                 std::pair{342, Key::left_alt},
                                 std::pair{343, Key::left_super},
                                 std::pair{344, Key::right_shift},
                                 std::pair{345, Key::right_control},
                                 std::pair{346, Key::right_alt},
                                 std::pair{347, Key::right_super},
                                 std::pair{348, Key::menu}};

static_assert(native_keys.size() == shark::client::key_count);
static_assert(!std::is_copy_constructible_v<Window>);
static_assert(!std::is_move_constructible_v<Window>);
static_assert(std::is_nothrow_destructible_v<Window>);
static_assert(noexcept(std::declval<WindowEventSink&>().key(65, 1)));
static_assert(noexcept(std::declval<WindowEventSink&>().mouse_button(0, 0)));
static_assert(noexcept(std::declval<WindowEventSink&>().cursor(0.0, 0.0)));
static_assert(noexcept(std::declval<WindowEventSink&>().scroll(0.0, 0.0)));
static_assert(noexcept(std::declval<WindowEventSink&>().text(U'a')));
static_assert(noexcept(std::declval<WindowEventSink&>().content_size(0, 0)));
static_assert(noexcept(std::declval<WindowEventSink&>().framebuffer_size(0, 0)));
static_assert(noexcept(std::declval<WindowEventSink&>().content_scale(1.0F, 1.0F)));
static_assert(noexcept(std::declval<WindowEventSink&>().focus(false)));
static_assert(noexcept(std::declval<WindowEventSink&>().iconify(false)));
static_assert(noexcept(std::declval<WindowEventSink&>().close_requested()));

} // namespace

TEST_CASE("window translates every supported native key and button independently", "[window]") {
    std::array<bool, shark::client::key_count> seen{};
    for (const auto& [native, key] : native_keys) {
        CAPTURE(native);
        REQUIRE(shark::client::detail::translate_glfw_key(native) == key);
        const auto index = static_cast<std::size_t>(key) - 1;
        REQUIRE_FALSE(seen[index]);
        seen[index] = true;
    }
    for (const auto mapped : seen) {
        CHECK(mapped);
    }
    for (int native = 0; native < 8; ++native) {
        CHECK(shark::client::detail::translate_glfw_mouse_button(native) ==
              static_cast<MouseButton>(native + 1));
    }
    CHECK(shark::client::detail::translate_glfw_action(0) == ButtonAction::release);
    CHECK(shark::client::detail::translate_glfw_action(1) == ButtonAction::press);
    CHECK(shark::client::detail::translate_glfw_action(2) == ButtonAction::repeat);
    for (const auto native : {-1,
                              0,
                              31,
                              33,
                              38,
                              58,
                              60,
                              62,
                              64,
                              97,
                              160,
                              163,
                              255,
                              270,
                              279,
                              285,
                              289,
                              315,
                              319,
                              337,
                              339,
                              349,
                              std::numeric_limits<int>::min(),
                              std::numeric_limits<int>::max()}) {
        CHECK(shark::client::detail::translate_glfw_key(native) == Key::unknown);
    }
    for (const auto native :
         {-1, 8, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
        CHECK(shark::client::detail::translate_glfw_mouse_button(native) == MouseButton::unknown);
    }
    for (const auto native :
         {-1, 3, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
        CHECK_FALSE(shark::client::detail::translate_glfw_action(native));
    }
}

TEST_CASE("window owns startup and detaches callbacks before reverse teardown", "[window]") {
    BackendState backend;
    const WindowConfig config{"Shark window fixture", {1024, 768}, false, false};
    {
        auto window = open(backend, config);
        CHECK(backend.recorded() == std::vector<Step>{Step::claim, Step::initialize, Step::create,
                                                      Step::install, Step::query});
        CHECK(backend.lease);
        CHECK(backend.created_content == config.content);
        CHECK_FALSE(backend.created_visible);
        CHECK_FALSE(backend.created_resizable);
        CHECK(state_of(*window) == backend.queried);
    }
    CHECK(backend.recorded() == std::vector<Step>{Step::claim, Step::initialize, Step::create,
                                                  Step::install, Step::query, Step::uninstall,
                                                  Step::destroy, Step::terminate, Step::release,
                                                  Step::delete_backend});
    CHECK(backend.sink == nullptr);
    CHECK_FALSE(backend.lease);
}

TEST_CASE("window claim failures acquire nothing and preserve the existing lease", "[window]") {
    for (const auto claim :
         {BackendClaim::unavailable, BackendClaim::conflict, BackendClaim::failed}) {
        BackendState backend;
        backend.claim_result = claim;
        const auto result = create(backend);
        REQUIRE_FALSE(result);
        CHECK_FALSE(result.error().message.empty());
        CHECK_FALSE(result.error().context.operation.empty());
        CHECK(backend.recorded() == std::vector<Step>{Step::claim, Step::delete_backend});
        CHECK_FALSE(backend.lease);
    }
    check_error(shark::client::detail::create_window({}, nullptr),
                ErrorCode::capability_unavailable);
}

TEST_CASE("window validates configuration before claiming platform resources", "[window]") {
    const std::array invalid_titles{std::string{},
                                    std::string(257, 'x'),
                                    std::string{"bad\0title", 9},
                                    std::string{"\xC0\xAF", 2},
                                    std::string{"\xED\xA0\x80", 3},
                                    std::string{"\xF4\x90\x80\x80", 4},
                                    std::string{"\xE2\x82", 2}};
    for (const auto& title : invalid_titles) {
        BackendState backend;
        WindowConfig config;
        config.title = title;
        check_error(create(backend, config), ErrorCode::invalid_configuration);
        CHECK(backend.recorded() == std::vector<Step>{Step::delete_backend});
    }
    for (const auto content :
         std::array{WindowExtent{0, 720}, WindowExtent{1280, 0}, WindowExtent{16385, 720},
                    WindowExtent{1280, 16385},
                    WindowExtent{std::numeric_limits<std::uint32_t>::max(), 1}}) {
        BackendState backend;
        WindowConfig config;
        config.content = content;
        check_error(create(backend, config), ErrorCode::invalid_configuration);
        CHECK(backend.recorded() == std::vector<Step>{Step::delete_backend});
    }
    for (const auto& config :
         std::array{WindowConfig{"Shark \xF0\x9F\xA6\x88 \xCE\xA9", {1, 16384}},
                    WindowConfig{std::string(256, 'x'), {16384, 1}}}) {
        BackendState backend;
        auto window = open(backend, config);
        CHECK(backend.created_content == config.content);
    }
}

TEST_CASE("window startup failures unwind every attempted phase exactly once", "[window]") {
    for (int failure = 0; failure < 4; ++failure) {
        CAPTURE(failure);
        BackendState backend;
        backend.fail_initialize = failure == 0;
        backend.fail_create = failure == 1;
        backend.fail_install = failure == 2;
        backend.fail_query = failure == 3;
        const auto result = create(backend);
        REQUIRE_FALSE(result);
        CHECK_FALSE(result.error().message.empty());
        CHECK_FALSE(result.error().context.operation.empty());
        std::vector<Step> expected{Step::claim, Step::initialize};
        if (failure >= 1) {
            expected.push_back(Step::create);
        }
        if (failure >= 2) {
            expected.push_back(Step::install);
        }
        if (failure == 3) {
            expected.push_back(Step::query);
        }
        if (failure >= 2) {
            expected.push_back(Step::uninstall);
        }
        if (failure >= 1) {
            expected.push_back(Step::destroy);
        }
        expected.insert(expected.end(), {Step::terminate, Step::release, Step::delete_backend});
        CHECK(backend.recorded() == expected);
        CHECK_FALSE(backend.lease);
        CHECK(backend.sink == nullptr);

        backend.fail_initialize = false;
        backend.fail_create = false;
        backend.fail_install = false;
        backend.fail_query = false;
        auto retry = open(backend);
        CHECK(state_of(*retry) == backend.queried);
    }
}

TEST_CASE("window rejects invalid initial observations with complete rollback", "[window]") {
    for (int invalid = 0; invalid < 5; ++invalid) {
        BackendState backend;
        if (invalid == 0) {
            backend.queried.content.width = std::numeric_limits<std::uint32_t>::max();
        } else if (invalid == 1) {
            backend.queried.framebuffer.height = std::numeric_limits<std::uint32_t>::max();
        } else if (invalid == 2) {
            backend.queried.scale.x = 0.0F;
        } else if (invalid == 3) {
            backend.queried.scale.y = -1.0F;
        } else {
            backend.queried.scale.x = std::numeric_limits<float>::infinity();
        }
        check_error(create(backend), ErrorCode::execution_failed);
        CHECK(backend.recorded() == std::vector<Step>{Step::claim, Step::initialize, Step::create,
                                                      Step::install, Step::query, Step::uninstall,
                                                      Step::destroy, Step::terminate, Step::release,
                                                      Step::delete_backend});
        CHECK_FALSE(backend.lease);
        CHECK(backend.sink == nullptr);
    }
    BackendState minimized;
    minimized.queried.content = {};
    minimized.queried.framebuffer = {};
    minimized.queried.iconified = true;
    auto window = open(minimized);
    CHECK(state_of(*window) == minimized.queried);
    CHECK_FALSE(capture(*window).focused);
}

TEST_CASE("window retains native failure codes in owned errors outside callback lifetime",
          "[window]") {
    constexpr auto native_detail = " (native error code 65544)";
    for (int failure = 0; failure < 4; ++failure) {
        BackendState backend;
        backend.native_error_code = 65544;
        backend.fail_initialize = failure == 0;
        backend.fail_create = failure == 1;
        backend.fail_install = failure == 2;
        backend.fail_query = failure == 3;
        const auto result = create(backend);
        check_error(result, ErrorCode::execution_failed);
        CHECK(result.error().message.ends_with(native_detail));
        CHECK(backend.native_error_code == 0);
        CHECK(backend.recorded().back() == Step::delete_backend);
    }
    BackendState backend;
    auto window = open(backend);
    backend.native_error_code = 65544;
    backend.fail_poll = true;
    const auto poll = window->poll_events();
    check_error(poll, ErrorCode::execution_failed);
    CHECK(poll.error().message.ends_with(native_detail));
    backend.fail_poll = false;
    backend.fail_query = true;
    const auto query = window->poll_events();
    check_error(query, ErrorCode::execution_failed);
    CHECK(query.error().message.ends_with(native_detail));
    backend.fail_query = false;
    backend.fail_command = true;
    const auto command = window->resize({900, 700});
    check_error(command, ErrorCode::execution_failed);
    CHECK(command.error().message.ends_with(native_detail));
    REQUIRE(window->close());
    window.reset();
    CHECK(backend.native_error_code == 0);
    CHECK(poll.error().message.ends_with(native_detail));
    CHECK(query.error().message.ends_with(native_detail));
    CHECK(command.error().message.ends_with(native_detail));
}

TEST_CASE("window rejects a second owner while allowing clean repeated lifetimes", "[window]") {
    bool lease = false;
    for (int cycle = 0; cycle < 32; ++cycle) {
        CAPTURE(cycle);
        BackendState first;
        BackendState second;
        first.shared_lease = &lease;
        second.shared_lease = &lease;
        auto window = open(first);
        check_error(create(second), ErrorCode::conflict);
        CHECK(second.recorded() == std::vector<Step>{Step::claim, Step::delete_backend});
        CHECK(lease);
        REQUIRE(window->close());
        CHECK_FALSE(lease);
        auto replacement = open(second);
        CHECK(lease);
        CHECK(capture(*replacement).key(Key::a) == ButtonState{});
    }
    CHECK_FALSE(lease);
}

TEST_CASE("closed window operations reject without reacquiring or touching backend", "[window]") {
    BackendState backend;
    auto window = open(backend);
    REQUIRE(window->close());
    const auto after_close = backend.recorded();
    REQUIRE(window->close());
    check_error(window->poll_events(), ErrorCode::conflict);
    check_error(window->state(), ErrorCode::conflict);
    check_error(window->capture_input(), ErrorCode::conflict);
    check_error(window->take_diagnostics(), ErrorCode::conflict);
    check_error(window->resize({640, 480}), ErrorCode::conflict);
    check_error(window->iconify(), ErrorCode::conflict);
    check_error(window->restore(), ErrorCode::conflict);
    check_error(window->show(), ErrorCode::conflict);
    check_error(window->focus(), ErrorCode::conflict);
    check_error(window->request_close(), ErrorCode::conflict);
    CHECK(backend.recorded() == after_close);
}

TEST_CASE("window methods reject the wrong thread without backend work", "[window]") {
    BackendState backend;
    auto window = open(backend);
    const auto before = backend.recorded();
    std::array<bool, 11> rejected{};
    // Joining provides the handoff; neither thread accesses mutable owner state
    // concurrently, and Catch assertions stay on the controlling thread.
    std::thread worker{[&] {
        const auto conflict = [](const auto& result) {
            return !result && result.error().code == ErrorCode::conflict;
        };
        rejected = {conflict(window->poll_events()),
                    conflict(window->state()),
                    conflict(window->capture_input()),
                    conflict(window->take_diagnostics()),
                    conflict(window->resize({640, 480})),
                    conflict(window->iconify()),
                    conflict(window->restore()),
                    conflict(window->show()),
                    conflict(window->focus()),
                    conflict(window->request_close()),
                    conflict(window->close())};
    }};
    worker.join();
    for (const auto result : rejected) {
        CHECK(result);
    }
    CHECK(backend.recorded() == before);
    REQUIRE(window->poll_events());
    REQUIRE(window->close());
}

TEST_CASE("window callbacks translate full keyboard and mouse catalogs into owned input",
          "[window]") {
    BackendState backend;
    auto window = open(backend);
    static_cast<void>(capture(*window));
    for (const auto& [native, key] : native_keys) {
        static_cast<void>(key);
        backend.sink->key(native, 1);
    }
    for (int native = 0; native < 8; ++native) {
        backend.sink->mouse_button(native, 1);
    }
    const auto pressed = capture(*window);
    for (const auto& [native, key] : native_keys) {
        CHECK(pressed.key(key) == ButtonState{true, true, false, false});
        backend.sink->key(native, 0);
    }
    for (int native = 0; native < 8; ++native) {
        CHECK(pressed.mouse_button(static_cast<MouseButton>(native + 1)) ==
              ButtonState{true, true, false, false});
        backend.sink->mouse_button(native, 0);
    }
    const auto released = capture(*window);
    for (const auto& [native, key] : native_keys) {
        static_cast<void>(native);
        CHECK(released.key(key) == ButtonState{false, false, true, false});
    }
    for (const auto button : released.mouse_buttons) {
        CHECK(button == ButtonState{false, false, true, false});
    }
    CHECK(pressed.key(Key::a).held);
    const auto diagnostics = window->take_diagnostics();
    REQUIRE(diagnostics);
    CHECK(diagnostics.value().rejected_input_events == 0);
}

TEST_CASE("window callback focus loss cancels pending input and preserves release edges",
          "[window]") {
    BackendState backend;
    auto window = open(backend);
    static_cast<void>(capture(*window));
    backend.sink->key(65, 1);
    backend.sink->key(65, 2);
    backend.sink->mouse_button(0, 1);
    backend.sink->cursor(10.0, 20.0);
    backend.sink->scroll(0.5, 2.0);
    backend.sink->text(U'\u03A9');
    backend.sink->focus(false);
    backend.sink->key(66, 1);
    backend.sink->text(U'x');
    const auto lost = capture(*window);
    CHECK_FALSE(state_of(*window).focused);
    CHECK_FALSE(lost.focused);
    CHECK(lost.focus_lost);
    CHECK(lost.key(Key::a) == ButtonState{false, false, true, false});
    CHECK(lost.key(Key::b) == ButtonState{});
    CHECK(lost.mouse_button(MouseButton::left) == ButtonState{false, false, true, false});
    CHECK_FALSE(lost.cursor.valid);
    CHECK(lost.text_size == 0);
    CHECK(lost.scroll == shark::client::ScrollOffset{});
    backend.sink->focus(true);
    const auto regained = capture(*window);
    CHECK(regained.focus_gained);
    CHECK(regained.key(Key::a) == ButtonState{});
    const auto diagnostics = window->take_diagnostics();
    REQUIRE(diagnostics);
    CHECK(diagnostics.value().rejected_input_events == 0);
}

TEST_CASE("window cursor scroll text and repeat callbacks preserve the input contract",
          "[window]") {
    BackendState backend;
    auto window = open(backend);
    static_cast<void>(capture(*window));
    backend.sink->key(65, 1);
    backend.sink->key(65, 2);
    backend.sink->cursor(-10.5, 20.25);
    backend.sink->scroll(0.25, -1.5);
    backend.sink->scroll(0.5, 0.25);
    backend.sink->text(U'\0');
    backend.sink->text(U'\u03A9');
    backend.sink->text(U'\U0001F988');
    const auto observed = capture(*window);
    CHECK(observed.key(Key::a) == ButtonState{true, true, false, true});
    CHECK(observed.cursor == CursorPosition{-10.5, 20.25, true});
    CHECK(observed.scroll == shark::client::ScrollOffset{0.75, -1.25});
    REQUIRE(observed.text_size == 3);
    CHECK(observed.text[0] == U'\0');
    CHECK(observed.text[1] == U'\u03A9');
    CHECK(observed.text[2] == U'\U0001F988');
    const auto following = capture(*window);
    CHECK(following.key(Key::a) == ButtonState{true, false, false, false});
    CHECK(following.cursor == observed.cursor);
    CHECK(following.text_size == 0);
    CHECK(following.scroll == shark::client::ScrollOffset{});
}

TEST_CASE("window minimization cancels input despite native focus callback ordering", "[window]") {
    BackendState backend;
    auto window = open(backend);
    static_cast<void>(capture(*window));
    backend.sink->key(65, 1);
    backend.sink->mouse_button(0, 1);
    backend.sink->iconify(true);
    const auto minimized = capture(*window);
    CHECK(state_of(*window).focused);
    CHECK(state_of(*window).iconified);
    CHECK_FALSE(minimized.focused);
    CHECK(minimized.focus_lost);
    CHECK(minimized.key(Key::a) == ButtonState{false, false, true, false});
    CHECK(minimized.mouse_button(MouseButton::left).released);
    backend.sink->focus(true);
    backend.sink->key(65, 1);
    CHECK_FALSE(capture(*window).key(Key::a).held);
    backend.sink->iconify(false);
    const auto restored = capture(*window);
    CHECK(restored.focused);
    CHECK(restored.focus_gained);
    CHECK(restored.key(Key::a) == ButtonState{});
    backend.sink->focus(false);
    backend.sink->iconify(true);
    backend.sink->iconify(false);
    CHECK_FALSE(capture(*window).focused);
    backend.sink->focus(true);
    backend.sink->key(65, 1);
    CHECK(capture(*window).key(Key::a).pressed);
}

TEST_CASE("window polling preserves focus cancellation and refreshes complete native state",
          "[window]") {
    BackendState backend;
    auto window = open(backend);
    static_cast<void>(capture(*window));
    backend.sink->key(87, 1);
    backend.focus_loss_on_poll = true;
    backend.queried.content = {900, 700};
    backend.queried.framebuffer = {1800, 1400};
    backend.queried.scale = {2.0F, 2.0F};
    backend.queried.iconified = true;
    backend.queried.visible = false;
    backend.queried.close_requested = true;
    REQUIRE(window->poll_events());
    CHECK(state_of(*window) == backend.queried);
    const auto observed = capture(*window);
    CHECK(observed.key(Key::w) == ButtonState{false, false, true, false});
    CHECK(observed.focus_lost);
    const auto steps = backend.recorded();
    REQUIRE(steps.size() >= 2);
    CHECK(steps[steps.size() - 2] == Step::poll);
    CHECK(steps.back() == Step::query);
}

TEST_CASE("window records separate content framebuffer and monitor scale observations",
          "[window]") {
    BackendState backend;
    auto window = open(backend);
    backend.sink->content_size(1000, 500);
    backend.sink->framebuffer_size(1500, 1000);
    backend.sink->content_scale(1.25F, 1.5F);
    backend.sink->cursor(250.5, 100.25);
    const auto observed = state_of(*window);
    CHECK(observed.content == WindowExtent{1000, 500});
    CHECK(observed.framebuffer == WindowExtent{1500, 1000});
    CHECK(observed.scale == ContentScale{1.25F, 1.5F});
    const auto input = capture(*window);
    CHECK(input.cursor == CursorPosition{250.5, 100.25, true});
    const auto pixels = shark::client::cursor_to_framebuffer(observed, input.cursor);
    REQUIRE(pixels);
    CHECK(pixels.value() == CursorPosition{375.75, 200.5, true});
    backend.sink->iconify(true);
    backend.sink->framebuffer_size(0, 0);
    CHECK(state_of(*window).iconified);
    CHECK(state_of(*window).framebuffer == WindowExtent{});
    CHECK_FALSE(shark::client::cursor_to_framebuffer(state_of(*window), input.cursor));
    backend.sink->iconify(false);
    backend.sink->framebuffer_size(2000, 1000);
    backend.sink->close_requested();
    CHECK_FALSE(state_of(*window).iconified);
    CHECK(state_of(*window).close_requested);
    CHECK(observed.framebuffer == WindowExtent{1500, 1000});
}

TEST_CASE("window commands report actual state instead of inferring framebuffer scale",
          "[window]") {
    BackendState backend;
    backend.queried.focused = false;
    backend.queried.visible = false;
    auto window = open(backend);
    REQUIRE(window->resize({900, 700}));
    CHECK(backend.last_command == WindowCommand::resize);
    CHECK(backend.commanded_size == WindowExtent{900, 700});
    CHECK(state_of(*window).content == WindowExtent{900, 700});
    CHECK(state_of(*window).framebuffer == WindowExtent{1600, 1200});
    REQUIRE(window->iconify());
    CHECK(backend.last_command == WindowCommand::iconify);
    CHECK(state_of(*window).iconified);
    REQUIRE(window->restore());
    CHECK(backend.last_command == WindowCommand::restore);
    CHECK_FALSE(state_of(*window).iconified);
    REQUIRE(window->show());
    CHECK(backend.last_command == WindowCommand::show);
    CHECK(state_of(*window).visible);
    REQUIRE(window->focus());
    CHECK(backend.last_command == WindowCommand::focus);
    CHECK(state_of(*window).focused);
    CHECK(capture(*window).focus_gained);
    REQUIRE(window->request_close());
    CHECK(backend.last_command == WindowCommand::request_close);
    CHECK(state_of(*window).close_requested);
}

TEST_CASE("window close observations stay latched across stale native state queries", "[window]") {
    BackendState backend;
    auto window = open(backend);
    backend.sink->close_requested();
    CHECK(state_of(*window).close_requested);
    CHECK_FALSE(backend.queried.close_requested);
    REQUIRE(window->poll_events());
    CHECK(state_of(*window).close_requested);
    REQUIRE(window->resize({900, 700}));
    CHECK(state_of(*window).close_requested);
}

TEST_CASE("window rejects invalid resize requests before calling native code", "[window]") {
    BackendState backend;
    auto window = open(backend);
    const auto before = backend.recorded();
    for (const auto content :
         std::array{WindowExtent{0, 720}, WindowExtent{1280, 0}, WindowExtent{16385, 720},
                    WindowExtent{1280, 16385},
                    WindowExtent{1, std::numeric_limits<std::uint32_t>::max()}}) {
        check_error(window->resize(content), ErrorCode::invalid_argument);
        CHECK(backend.recorded() == before);
    }
    REQUIRE(window->resize({1, 16384}));
    CHECK(backend.commanded_size == WindowExtent{1, 16384});
    REQUIRE(window->resize({16384, 1}));
    CHECK(backend.commanded_size == WindowExtent{16384, 1});
}

TEST_CASE("window reports command and polling failures without losing teardown ownership",
          "[window]") {
    BackendState backend;
    auto window = open(backend);
    const auto original = state_of(*window);
    backend.fail_command = true;
    CHECK_FALSE(window->resize({900, 700}));
    CHECK(state_of(*window) == original);
    backend.fail_command = false;
    backend.fail_poll = true;
    CHECK_FALSE(window->poll_events());
    CHECK(state_of(*window) == original);
    backend.fail_poll = false;
    backend.fail_query = true;
    CHECK_FALSE(window->poll_events());
    CHECK(state_of(*window) == original);
    CHECK_FALSE(window->resize({900, 700}));
    CHECK(state_of(*window) == original);
    backend.fail_query = false;
    REQUIRE(window->poll_events());
    REQUIRE(window->close());
    CHECK_FALSE(backend.lease);
    CHECK(backend.sink == nullptr);
}

TEST_CASE("window rejects malformed queried state without publishing partial observations",
          "[window]") {
    BackendState backend;
    auto window = open(backend);
    const auto original = state_of(*window);
    backend.queried.content = {1024, 768};
    backend.queried.framebuffer = {2048, 1536};
    backend.queried.focused = false;
    backend.queried.scale.y = std::numeric_limits<float>::quiet_NaN();
    check_error(window->poll_events(), ErrorCode::execution_failed);
    CHECK(state_of(*window) == original);
    CHECK(capture(*window).focused);
    const auto diagnostics = window->take_diagnostics();
    REQUIRE(diagnostics);
    CHECK(diagnostics.value().invalid_window_events == 1);
    REQUIRE(diagnostics.value().window_error);
    backend.queried.scale = {2.0F, 2.0F};
    REQUIRE(window->poll_events());
    CHECK(state_of(*window) == backend.queried);
    CHECK_FALSE(capture(*window).focused);
}

TEST_CASE("window defers bounded input diagnostics without blocking releases or focus",
          "[window]") {
    BackendState backend;
    auto window = open(backend);
    static_cast<void>(capture(*window));
    backend.sink->key(65, 1);
    backend.sink->mouse_button(0, 1);
    for (std::size_t index = 0; index < shark::client::input_text_capacity; ++index) {
        backend.sink->text(U'a');
    }
    backend.sink->text(U'b');
    backend.sink->text(static_cast<char32_t>(0xD800));
    backend.sink->key(-1, 1);
    backend.sink->key(65, 99);
    backend.sink->mouse_button(8, 1);
    backend.sink->mouse_button(0, 2);
    backend.sink->cursor(std::numeric_limits<double>::quiet_NaN(), 0.0);
    backend.sink->scroll(0.0, std::numeric_limits<double>::infinity());
    backend.sink->key(65, 0);
    backend.sink->focus(false);
    const auto snapshot = capture(*window);
    CHECK(snapshot.text_overflow);
    CHECK(snapshot.key(Key::a).released);
    CHECK(snapshot.mouse_button(MouseButton::left).released);
    CHECK_FALSE(snapshot.key(Key::a).held);
    const auto diagnostics = window->take_diagnostics();
    REQUIRE(diagnostics);
    CHECK(diagnostics.value().rejected_input_events == 8);
    CHECK(diagnostics.value().invalid_window_events == 0);
    REQUIRE(diagnostics.value().input_error);
    CHECK_FALSE(diagnostics.value().input_error->message.empty());
    CHECK_FALSE(diagnostics.value().input_error->context.operation.empty());
    CHECK_FALSE(diagnostics.value().window_error);
    const auto cleared = window->take_diagnostics();
    REQUIRE(cleared);
    CHECK(cleared.value().rejected_input_events == 0);
    CHECK_FALSE(cleared.value().input_error);
    CHECK(diagnostics.value().rejected_input_events == 8);
}

TEST_CASE("window rejects invalid metric callbacks atomically with deferred diagnostics",
          "[window]") {
    BackendState backend;
    auto window = open(backend);
    const auto original = state_of(*window);
    backend.sink->content_size(-1, 500);
    backend.sink->content_size(500, -1);
    backend.sink->framebuffer_size(-1, 500);
    backend.sink->framebuffer_size(500, -1);
    backend.sink->content_scale(0.0F, 1.0F);
    backend.sink->content_scale(1.0F, -1.0F);
    backend.sink->content_scale(std::numeric_limits<float>::quiet_NaN(), 1.0F);
    backend.sink->content_scale(1.0F, std::numeric_limits<float>::infinity());
    CHECK(state_of(*window) == original);
    const auto diagnostics = window->take_diagnostics();
    REQUIRE(diagnostics);
    CHECK(diagnostics.value().rejected_input_events == 0);
    CHECK(diagnostics.value().invalid_window_events == 8);
    CHECK_FALSE(diagnostics.value().input_error);
    REQUIRE(diagnostics.value().window_error);
    CHECK_FALSE(diagnostics.value().window_error->message.empty());
    CHECK_FALSE(diagnostics.value().window_error->context.operation.empty());
    const auto cleared = window->take_diagnostics();
    REQUIRE(cleared);
    CHECK(cleared.value().invalid_window_events == 0);
    CHECK_FALSE(cleared.value().window_error);
    backend.sink->content_size(0, 0);
    backend.sink->framebuffer_size(0, 0);
    CHECK(state_of(*window).content == WindowExtent{});
    CHECK(state_of(*window).framebuffer == WindowExtent{});
}

TEST_CASE("window aggregates callback rejection bursts into owned diagnostics", "[window]") {
    BackendState backend;
    auto window = open(backend);
    backend.sink->key(65, 1);
    for (int index = 0; index < 4096; ++index) {
        backend.sink->key(65, 99);
        backend.sink->framebuffer_size(-1, 0);
    }
    backend.sink->key(65, 0);
    const auto input = capture(*window);
    CHECK(input.key(Key::a).released);
    CHECK_FALSE(input.key(Key::a).held);
    const auto diagnostics = window->take_diagnostics();
    REQUIRE(diagnostics);
    CHECK(diagnostics.value().rejected_input_events == 4096);
    CHECK(diagnostics.value().invalid_window_events == 4096);
    REQUIRE(diagnostics.value().input_error);
    REQUIRE(diagnostics.value().window_error);
    CHECK(diagnostics.value().input_error->message.size() <= 256);
    CHECK(diagnostics.value().window_error->message.size() <= 256);
    REQUIRE(window->close());
    window.reset();
    CHECK(diagnostics.value().rejected_input_events == 4096);
    CHECK_FALSE(diagnostics.value().input_error->message.empty());
}

TEST_CASE("framebuffer conversion uses each actual dimension and permits outside coordinates",
          "[window]") {
    WindowState window{{800, 600}, {1600, 900}, {3.0F, 4.0F}, true, false, true, false};
    const auto converted = shark::client::cursor_to_framebuffer(window, {-10.5, 650.0, true});
    REQUIRE(converted);
    CHECK(converted.value() == CursorPosition{-21.0, 975.0, true});
    window.scale = {std::numeric_limits<float>::quiet_NaN(), -1.0F};
    const auto independent = shark::client::cursor_to_framebuffer(window, {1.25, 2.5, true});
    REQUIRE(independent);
    CHECK(independent.value() == CursorPosition{2.5, 3.75, true});
    window.content = {2, 2};
    window.framebuffer = {2, 2};
    const auto extreme = shark::client::cursor_to_framebuffer(
        window, {std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(), true});
    REQUIRE(extreme);
    CHECK(extreme.value() == CursorPosition{std::numeric_limits<double>::max(),
                                            -std::numeric_limits<double>::max(), true});
}

TEST_CASE("framebuffer conversion rejects zero dimensions invalid cursors and overflow",
          "[window]") {
    const WindowState valid{{800, 600}, {1600, 1200}, {2.0F, 2.0F}, true, false, true, false};
    for (int dimension = 0; dimension < 4; ++dimension) {
        auto window = valid;
        if (dimension == 0) {
            window.content.width = 0;
        } else if (dimension == 1) {
            window.content.height = 0;
        } else if (dimension == 2) {
            window.framebuffer.width = 0;
        } else {
            window.framebuffer.height = 0;
        }
        CHECK_FALSE(shark::client::cursor_to_framebuffer(window, {10.0, 20.0, true}));
    }
    for (const auto cursor :
         std::array{CursorPosition{10.0, 20.0, false},
                    CursorPosition{std::numeric_limits<double>::quiet_NaN(), 20.0, true},
                    CursorPosition{10.0, std::numeric_limits<double>::infinity(), true},
                    CursorPosition{std::numeric_limits<double>::max(), 20.0, true},
                    CursorPosition{10.0, -std::numeric_limits<double>::max(), true}}) {
        const auto result = shark::client::cursor_to_framebuffer(valid, cursor);
        REQUIRE_FALSE(result);
        CHECK_FALSE(result.error().message.empty());
        CHECK_FALSE(result.error().context.operation.empty());
    }
}

TEST_CASE("window frame pump propagates polling and query failures before dispatch",
          "[window][frame]") {
    for (const bool query_failure : std::array{false, true}) {
        BackendState backend;
        auto window = open(backend);
        auto loop = frame_loop();
        backend.fail_poll = !query_failure;
        backend.fail_query = query_failure;
        backend.native_error_code = 713;
        std::ostringstream output;
        shark::Logger logger{output};
        bool paused = false;
        std::uint64_t calls = 0;
        const auto result = pump_offline_window(
            *window, *loop, 50ms, paused,
            [&](std::uint64_t, const shark::client::InputSnapshot&) -> shark::Result<void> {
                ++calls;
                return {};
            },
            logger);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::execution_failed);
        CHECK(result.error().context.operation == (query_failure ? "window.query" : "window.poll"));
        CHECK(result.error().message.find("713") != std::string::npos);
        CHECK(calls == 0);
        CHECK(loop->next_tick() == 0);
        CHECK_FALSE(paused);
        CHECK(output.str().empty());
        REQUIRE(window->close());
        CHECK_FALSE(backend.lease);
    }
}

TEST_CASE("window frame pump propagates diagnostic sink failure before dispatch",
          "[window][frame]") {
    for (const bool window_diagnostic : std::array{false, true}) {
        BackendState backend;
        auto window = open(backend);
        auto loop = frame_loop();
        if (window_diagnostic) {
            backend.sink->content_scale(0.0F, 1.0F);
        } else {
            backend.sink->key(-1, 1);
        }
        std::ostringstream output;
        output.setstate(std::ios::badbit);
        shark::Logger logger{output};
        bool paused = false;
        std::uint64_t calls = 0;
        const auto result = pump_offline_window(
            *window, *loop, 50ms, paused,
            [&](std::uint64_t, const shark::client::InputSnapshot&) -> shark::Result<void> {
                ++calls;
                return {};
            },
            logger);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::io_failure);
        CHECK(result.error().context.operation == "log.write");
        CHECK(calls == 0);
        CHECK(loop->next_tick() == 0);
        REQUIRE(window->close());
        CHECK_FALSE(backend.lease);
    }
}

TEST_CASE("window frame pump logs deferred diagnostics and preserves callback failures",
          "[window][frame]") {
    BackendState backend;
    auto window = open(backend);
    auto loop = frame_loop();
    backend.sink->key(-1, 1);
    backend.sink->content_scale(0.0F, 1.0F);
    std::ostringstream output;
    shark::Logger logger{output};
    bool paused = false;
    std::uint64_t calls = 0;
    const auto result = pump_offline_window(
        *window, *loop, 50ms, paused,
        [&](std::uint64_t, const shark::client::InputSnapshot&) -> shark::Result<void> {
            ++calls;
            return shark::Error{ErrorCode::conflict,
                                "play tick rejected",
                                {.operation = "test.window.tick", .request_id = "tick-0"}};
        },
        logger);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::conflict);
    CHECK(result.error().context.operation == "test.window.tick");
    CHECK(result.error().context.request_id == "tick-0");
    CHECK(calls == 1);
    CHECK(loop->next_tick() == 0);
    CHECK(loop->faulted());
    const auto records = output.str();
    CHECK(records.find("\"level\":\"warning\"") != std::string::npos);
    const auto first_newline = records.find('\n');
    REQUIRE(first_newline != std::string::npos);
    CHECK(records.find('\n', first_newline + 1) == records.size() - 1);
}

TEST_CASE("window frame pump closes without ticking or handling simultaneous controls",
          "[window][frame]") {
    BackendState backend;
    auto window = open(backend);
    auto loop = frame_loop();
    backend.sink->key(295, 1);
    backend.sink->key(296, 1);
    backend.sink->close_requested();
    std::ostringstream output;
    shark::Logger logger{output};
    bool paused = false;
    std::uint64_t calls = 0;
    const auto result = pump_offline_window(
        *window, *loop, 1s, paused,
        [&](std::uint64_t, const shark::client::InputSnapshot&) -> shark::Result<void> {
            ++calls;
            return {};
        },
        logger);
    REQUIRE(result);
    CHECK_FALSE(result.value());
    CHECK(calls == 0);
    CHECK(loop->next_tick() == 0);
    CHECK_FALSE(paused);
    const auto steps = backend.recorded();
    REQUIRE(steps.size() >= 2);
    CHECK(steps[steps.size() - 2] == Step::poll);
    CHECK(steps.back() == Step::query);
}

TEST_CASE("window frame pump handles F6 F7 edges and continues polling while paused",
          "[window][frame]") {
    BackendState backend;
    auto window = open(backend);
    auto loop = frame_loop();
    std::ostringstream output;
    shark::Logger logger{output};
    bool paused = false;
    std::vector<shark::client::InputSnapshot> ticks;
    const shark::client::OfflineInputCallback callback =
        [&](std::uint64_t, const shark::client::InputSnapshot& input) -> shark::Result<void> {
        ticks.push_back(input);
        CHECK(input.key(Key::f6) == ButtonState{});
        CHECK(input.key(Key::f7) == ButtonState{});
        return {};
    };
    const auto pump = [&](shark::MonotonicTime now) {
        const auto previous_steps = backend.step_count;
        const auto result = pump_offline_window(*window, *loop, now, paused, callback, logger);
        REQUIRE(result);
        REQUIRE(result.value());
        REQUIRE(backend.step_count == previous_steps + 2);
        CHECK(backend.steps[previous_steps] == Step::poll);
        CHECK(backend.steps[previous_steps + 1] == Step::query);
        return *result.value();
    };
    backend.sink->key(295, 1);
    CHECK(pump(50ms).timing.executed == 0);
    CHECK(paused);
    backend.sink->key(295, 2);
    backend.sink->key(296, 1);
    backend.sink->key(65, 1);
    CHECK(pump(100ms).timing.executed == 1);
    CHECK(paused);
    REQUIRE(ticks.size() == 1);
    CHECK(ticks.front().key(Key::a).pressed);
    backend.sink->key(296, 2);
    CHECK(pump(1s).timing.executed == 0);
    CHECK(pump(2s).timing.executed == 0);
    backend.sink->key(296, 0);
    backend.sink->key(296, 1);
    CHECK(pump(3s).timing.executed == 1);
    REQUIRE(ticks.size() == 2);
    CHECK(ticks.back().key(Key::a) == ButtonState{true, false, false, false});
    backend.sink->key(295, 0);
    backend.sink->key(295, 1);
    CHECK(pump(4s).timing.executed == 0);
    CHECK_FALSE(paused);
    backend.sink->key(296, 2);
    CHECK(pump(4s + 50ms).timing.executed == 3);
    CHECK(loop->next_tick() == 5);
    CHECK(output.str().empty());
}

TEST_CASE("window frame pump polls minimized windows and excludes inactive control presses",
          "[window][frame]") {
    BackendState backend;
    auto window = open(backend);
    auto loop = frame_loop();
    std::ostringstream output;
    shark::Logger logger{output};
    bool paused = false;
    std::uint64_t calls = 0;
    const shark::client::OfflineInputCallback callback =
        [&](std::uint64_t, const shark::client::InputSnapshot&) -> shark::Result<void> {
        ++calls;
        return {};
    };
    REQUIRE(window->iconify());
    for (const auto now : std::array{1s, 2s, 3s}) {
        backend.sink->key(295, 1);
        backend.sink->key(296, 1);
        const auto previous_steps = backend.step_count;
        const auto result = pump_offline_window(*window, *loop, now, paused, callback, logger);
        REQUIRE(result);
        REQUIRE(result.value());
        CHECK(result.value()->paused);
        CHECK(result.value()->presentation_suspended);
        CHECK(result.value()->timing.executed == 0);
        REQUIRE(backend.step_count == previous_steps + 2);
        CHECK(backend.steps[previous_steps] == Step::poll);
        CHECK(backend.steps[previous_steps + 1] == Step::query);
        CHECK_FALSE(paused);
    }
    REQUIRE(window->restore());
    const auto resumed = pump_offline_window(*window, *loop, 4s, paused, callback, logger);
    REQUIRE(resumed);
    REQUIRE(resumed.value());
    CHECK_FALSE(resumed.value()->paused);
    CHECK_FALSE(resumed.value()->presentation_suspended);
    CHECK(resumed.value()->timing.executed == 0);
    const auto fresh = pump_offline_window(*window, *loop, 4s + 50ms, paused, callback, logger);
    REQUIRE(fresh);
    REQUIRE(fresh.value());
    CHECK(fresh.value()->timing.executed == 3);
    CHECK(calls == 3);
    CHECK(output.str().empty());
}
