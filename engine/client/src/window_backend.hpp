#pragma once

// Private implementation/test seam. Never included by installed public headers.
#include <shark/client/window.hpp>

#include <memory>

namespace shark::client::detail {

enum class BackendClaim { acquired, unavailable, conflict, failed };
enum class WindowCommand { resize, iconify, restore, show, focus, request_close };

class WindowEventSink {
  public:
    virtual ~WindowEventSink() = default;
    virtual void key(int key, int action) noexcept = 0;
    virtual void mouse_button(int button, int action) noexcept = 0;
    virtual void cursor(double x, double y) noexcept = 0;
    virtual void scroll(double x, double y) noexcept = 0;
    virtual void text(char32_t scalar) noexcept = 0;
    virtual void content_size(int width, int height) noexcept = 0;
    virtual void framebuffer_size(int width, int height) noexcept = 0;
    virtual void content_scale(float x, float y) noexcept = 0;
    virtual void focus(bool focused) noexcept = 0;
    virtual void iconify(bool iconified) noexcept = 0;
    virtual void close_requested() noexcept = 0;
};

// Every operation is nonthrowing. A failed initialize/create/install may have
// partially acquired resources: terminate/destroy/uninstall are called once in
// reverse order after EACH attempted phase, including the failed phase. Cleanup
// must be idempotent, allocation-free and detach callbacks before owner teardown.
class WindowBackend {
  public:
    virtual ~WindowBackend() = default;
    [[nodiscard]] virtual BackendClaim claim() noexcept = 0;
    [[nodiscard]] virtual bool initialize() noexcept = 0;
    [[nodiscard]] virtual bool create(const WindowConfig& config) noexcept = 0;
    [[nodiscard]] virtual bool install_callbacks(WindowEventSink& sink) noexcept = 0;
    [[nodiscard]] virtual bool query(WindowState& state) noexcept = 0;
    [[nodiscard]] virtual bool poll() noexcept = 0;
    [[nodiscard]] virtual bool command(WindowCommand command, WindowExtent size) noexcept = 0;
    virtual void uninstall_callbacks() noexcept = 0;
    virtual void destroy() noexcept = 0;
    virtual void terminate() noexcept = 0;
    virtual void release() noexcept = 0;
    [[nodiscard]] virtual void* native_handle() const noexcept = 0;
    // Native adapters retain a fixed numeric error. The owner copies it into an
    // owned diagnostic outside callbacks; borrowed native messages never escape.
    [[nodiscard]] virtual int last_error_code() const noexcept {
        return 0;
    }
};

[[nodiscard]] std::unique_ptr<WindowBackend> make_native_window_backend();
// GLFW's fixed key/button/action catalog, translated without including GLFW or
// depending on its enum layout matching engine enums. Unknown values reject.
[[nodiscard]] Key translate_glfw_key(int key) noexcept;
[[nodiscard]] MouseButton translate_glfw_mouse_button(int button) noexcept;
[[nodiscard]] std::optional<ButtonAction> translate_glfw_action(int action) noexcept;

} // namespace shark::client::detail
