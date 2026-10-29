#include <shark/client/window.hpp>

#include "window_backend.hpp"

#include <cmath>
#include <exception>
#include <limits>
#include <string_view>
#include <thread>
#include <utility>

namespace shark::client {
namespace {

Error failure(ErrorCode code, std::string_view operation, std::string_view message) {
    return {code, std::string(message), {.operation = std::string(operation)}};
}

bool valid_title(std::string_view text) noexcept {
    if (text.empty() || text.size() > 256) {
        return false;
    }
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<std::uint8_t>(text[index]);
        if (first == 0) {
            return false;
        }
        if (first < 0x80U) {
            ++index;
            continue;
        }
        std::size_t count = 0;
        std::uint32_t scalar = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xC2U && first <= 0xDFU) {
            count = 2;
            scalar = first & 0x1FU;
            minimum = 0x80U;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            count = 3;
            scalar = first & 0x0FU;
            minimum = 0x800U;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            count = 4;
            scalar = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (count > text.size() - index) {
            return false;
        }
        for (std::size_t offset = 1; offset < count; ++offset) {
            const auto next = static_cast<std::uint8_t>(text[index + offset]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            scalar = (scalar << 6U) | (next & 0x3FU);
        }
        if (scalar < minimum || scalar > 0x10FFFFU || (scalar >= 0xD800U && scalar <= 0xDFFFU)) {
            return false;
        }
        index += count;
    }
    return true;
}

bool valid_requested_extent(WindowExtent size) noexcept {
    return size.width > 0 && size.width <= 16384 && size.height > 0 && size.height <= 16384;
}

bool valid_observed_extent(WindowExtent size) noexcept {
    constexpr auto maximum = static_cast<std::uint32_t>(std::numeric_limits<int>::max());
    return size.width <= maximum && size.height <= maximum;
}

bool valid_scale(ContentScale scale) noexcept {
    return std::isfinite(scale.x) && std::isfinite(scale.y) && scale.x > 0.0F && scale.y > 0.0F;
}

void increment(std::uint32_t& count) noexcept {
    if (count != std::numeric_limits<std::uint32_t>::max()) {
        ++count;
    }
}

} // namespace

struct Window::Impl final : detail::WindowEventSink {
    explicit Impl(std::unique_ptr<detail::WindowBackend> implementation) noexcept
        : backend(std::move(implementation)), owner(std::this_thread::get_id()) {}

    ~Impl() noexcept override {
        if (claimed && owner != std::this_thread::get_id()) {
            std::terminate();
        }
        shutdown();
    }

    std::unique_ptr<detail::WindowBackend> backend;
    const std::thread::id owner;
    InputState input{};
    WindowState observed{};
    std::uint32_t rejected_input{0};
    std::uint32_t invalid_window{0};
    bool claimed{false};
    bool initialized{false};
    bool created{false};
    bool installed{false};
    bool active{false};

    [[nodiscard]] Result<void> check(std::string_view operation, bool require_active = true) const {
        if (owner != std::this_thread::get_id()) {
            return failure(ErrorCode::conflict, operation,
                           "Window requires its controlling thread");
        }
        if (require_active && !active) {
            return failure(ErrorCode::conflict, operation, "Window is closed");
        }
        return {};
    }

    void shutdown() noexcept {
        active = false;
        if (installed) {
            installed = false;
            backend->uninstall_callbacks();
        }
        if (created) {
            created = false;
            backend->destroy();
        }
        if (initialized) {
            initialized = false;
            backend->terminate();
        }
        if (claimed) {
            claimed = false;
            backend->release();
        }
    }

    void record(InputEventResult result) noexcept {
        if (result != InputEventResult::accepted && result != InputEventResult::ignored_unfocused) {
            increment(rejected_input);
        }
    }

    void update_focus() noexcept {
        record(input.on_focus(observed.focused && !observed.iconified));
    }

    [[nodiscard]] Error backend_failure(std::string_view operation,
                                        std::string_view message) const {
        auto error = failure(ErrorCode::execution_failed, operation, message);
        if (const int code = backend->last_error_code(); code != 0) {
            error.message += " (native error code " + std::to_string(code) + ")";
        }
        return error;
    }

    [[nodiscard]] Result<void> refresh() {
        WindowState candidate{};
        if (!backend->query(candidate)) {
            return backend_failure("window.query", "Native window state query failed");
        }
        if (!valid_observed_extent(candidate.content) ||
            !valid_observed_extent(candidate.framebuffer) || !valid_scale(candidate.scale)) {
            increment(invalid_window);
            return failure(ErrorCode::execution_failed, "window.query",
                           "Native window state was invalid");
        }
        // A close request is sticky until explicitly cancelled or destroyed, even if a
        // backend observation races the operating system's close notification.
        candidate.close_requested = candidate.close_requested || observed.close_requested;
        observed = candidate;
        update_focus();
        return {};
    }

    [[nodiscard]] Result<void> issue(detail::WindowCommand command, WindowExtent extent,
                                     std::string_view operation) {
        auto valid = check(operation);
        if (!valid) {
            return std::move(valid).error();
        }
        if (!backend->command(command, extent)) {
            return backend_failure(operation, "Native window operation failed");
        }
        if (command == detail::WindowCommand::request_close) {
            observed.close_requested = true;
        } else if (command == detail::WindowCommand::cancel_close) {
            observed.close_requested = false;
        }
        return refresh();
    }

    void key(int native_key, int native_action) noexcept override {
        const auto action = detail::translate_glfw_action(native_action);
        if (!action) {
            record(InputEventResult::invalid_action);
            return;
        }
        record(input.on_key(detail::translate_glfw_key(native_key), *action));
    }
    void mouse_button(int native_button, int native_action) noexcept override {
        const auto action = detail::translate_glfw_action(native_action);
        if (!action) {
            record(InputEventResult::invalid_action);
            return;
        }
        record(input.on_mouse_button(detail::translate_glfw_mouse_button(native_button), *action));
    }
    void cursor(double x, double y) noexcept override {
        record(input.on_cursor(x, y));
    }
    void scroll(double x, double y) noexcept override {
        record(input.on_scroll(x, y));
    }
    void text(char32_t scalar) noexcept override {
        record(input.on_text(scalar));
    }
    void content_size(int width, int height) noexcept override {
        if (width < 0 || height < 0) {
            increment(invalid_window);
            return;
        }
        observed.content = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
    }
    void framebuffer_size(int width, int height) noexcept override {
        if (width < 0 || height < 0) {
            increment(invalid_window);
            return;
        }
        observed.framebuffer = {static_cast<std::uint32_t>(width),
                                static_cast<std::uint32_t>(height)};
    }
    void content_scale(float x, float y) noexcept override {
        if (!valid_scale({x, y})) {
            increment(invalid_window);
            return;
        }
        observed.scale = {x, y};
    }
    void focus(bool focused) noexcept override {
        observed.focused = focused;
        update_focus();
    }
    void iconify(bool iconified) noexcept override {
        observed.iconified = iconified;
        update_focus();
    }
    void close_requested() noexcept override {
        observed.close_requested = true;
    }
};

Window::Window(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Window::~Window() noexcept = default;

Result<std::unique_ptr<Window>> Window::create(const WindowConfig& config) {
    return detail::create_window(config, detail::make_native_window_backend());
}

Result<std::unique_ptr<Window>> detail::create_window(const WindowConfig& config,
                                                      std::unique_ptr<WindowBackend> backend) {
    if (!valid_requested_extent(config.content) || !valid_title(config.title)) {
        return failure(ErrorCode::invalid_configuration, "window.configuration.validate",
                       "Window requires 1..16384 content dimensions and 1..256 UTF-8 title bytes "
                       "without NUL");
    }
    if (!backend) {
        return failure(ErrorCode::capability_unavailable, "window.claim",
                       "Window backend is absent");
    }
    auto impl = std::make_unique<Window::Impl>(std::move(backend));
    const auto claim = impl->backend->claim();
    if (claim != BackendClaim::acquired) {
        const auto code = claim == BackendClaim::unavailable ? ErrorCode::capability_unavailable
                          : claim == BackendClaim::conflict  ? ErrorCode::conflict
                                                             : ErrorCode::execution_failed;
        return failure(code, "window.claim", "Native window backend could not be claimed");
    }
    impl->claimed = true;
    impl->initialized = true;
    if (!impl->backend->initialize()) {
        return impl->backend_failure("window.initialize", "Native window initialization failed");
    }
    impl->created = true;
    if (!impl->backend->create(config)) {
        return impl->backend_failure("window.create", "Native window creation failed");
    }
    impl->installed = true;
    if (!impl->backend->install_callbacks(*impl)) {
        return impl->backend_failure("window.callbacks", "Native window callbacks failed");
    }
    auto refreshed = impl->refresh();
    if (!refreshed) {
        return std::move(refreshed).error();
    }
    impl->active = true;
    return std::unique_ptr<Window>(new Window(std::move(impl)));
}

Result<void*> detail::native_test_handle(Window& window) {
    const auto valid = window.impl_->check("window.native_test_handle");
    if (!valid) {
        return valid.error();
    }
    auto* handle = window.impl_->backend->native_handle();
    if (handle == nullptr) {
        return failure(ErrorCode::capability_unavailable, "window.native_test_handle",
                       "Native test handle is unavailable");
    }
    return handle;
}

Result<void> Window::poll_events() {
    auto valid = impl_->check("window.poll");
    if (!valid) {
        return std::move(valid).error();
    }
    if (!impl_->backend->poll()) {
        return impl_->backend_failure("window.poll", "Native event polling failed");
    }
    return impl_->refresh();
}

Result<WindowState> Window::state() const {
    auto valid = impl_->check("window.state");
    if (!valid) {
        return std::move(valid).error();
    }
    return impl_->observed;
}

Result<InputSnapshot> Window::capture_input() {
    auto valid = impl_->check("window.capture_input");
    if (!valid) {
        return std::move(valid).error();
    }
    return impl_->input.capture();
}

Result<WindowDiagnostics> Window::take_diagnostics() {
    auto valid = impl_->check("window.diagnostics");
    if (!valid) {
        return std::move(valid).error();
    }
    WindowDiagnostics result{impl_->rejected_input, impl_->invalid_window};
    if (result.rejected_input_events != 0) {
        result.input_error = failure(ErrorCode::invalid_argument, "window.input",
                                     "Input events were rejected as malformed or overflowing");
    }
    if (result.invalid_window_events != 0) {
        result.window_error = failure(ErrorCode::execution_failed, "window.events",
                                      "Invalid native window observations were rejected");
    }
    impl_->rejected_input = 0;
    impl_->invalid_window = 0;
    return result;
}

Result<void> Window::resize(WindowExtent content) {
    auto valid = impl_->check("window.resize");
    if (!valid) {
        return std::move(valid).error();
    }
    if (!valid_requested_extent(content)) {
        return failure(ErrorCode::invalid_argument, "window.resize",
                       "Content dimensions must be between 1 and 16384");
    }
    return impl_->issue(detail::WindowCommand::resize, content, "window.resize");
}
Result<void> Window::iconify() {
    return impl_->issue(detail::WindowCommand::iconify, {}, "window.iconify");
}
Result<void> Window::restore() {
    return impl_->issue(detail::WindowCommand::restore, {}, "window.restore");
}
Result<void> Window::show() {
    return impl_->issue(detail::WindowCommand::show, {}, "window.show");
}
Result<void> Window::focus() {
    return impl_->issue(detail::WindowCommand::focus, {}, "window.focus");
}
Result<void> Window::request_close() {
    return impl_->issue(detail::WindowCommand::request_close, {}, "window.request_close");
}
Result<void> Window::cancel_close_request() {
    return impl_->issue(detail::WindowCommand::cancel_close, {}, "window.cancel_close");
}
Result<void> Window::close() {
    auto valid = impl_->check("window.close", false);
    if (!valid) {
        return std::move(valid).error();
    }
    impl_->shutdown();
    return {};
}

Result<CursorPosition> cursor_to_framebuffer(const WindowState& window, CursorPosition cursor) {
    if (!cursor.valid || !std::isfinite(cursor.x) || !std::isfinite(cursor.y) ||
        window.content.width == 0 || window.content.height == 0 || window.framebuffer.width == 0 ||
        window.framebuffer.height == 0) {
        return failure(ErrorCode::invalid_argument, "window.cursor_to_framebuffer",
                       "Cursor and nonzero content/framebuffer dimensions are required");
    }
    const double x = cursor.x * (static_cast<double>(window.framebuffer.width) /
                                 static_cast<double>(window.content.width));
    const double y = cursor.y * (static_cast<double>(window.framebuffer.height) /
                                 static_cast<double>(window.content.height));
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return failure(ErrorCode::invalid_argument, "window.cursor_to_framebuffer",
                       "Framebuffer cursor conversion overflowed");
    }
    return CursorPosition{x, y, true};
}

} // namespace shark::client
