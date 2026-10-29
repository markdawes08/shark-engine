#pragma once

#include <shark/client/input.hpp>
#include <shark/error.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace shark::client {

struct WindowExtent {
    std::uint32_t width{0};
    std::uint32_t height{0};
    friend bool operator==(const WindowExtent&, const WindowExtent&) = default;
};

struct ContentScale {
    float x{1.0F};
    float y{1.0F};
    friend bool operator==(const ContentScale&, const ContentScale&) = default;
};

struct WindowConfig {
    std::string title{"Shark"};
    WindowExtent content{1280, 720};
    bool visible{true};
    bool resizable{true};
};

struct WindowState {
    WindowExtent content{};
    WindowExtent framebuffer{};
    ContentScale scale{};
    bool focused{false};
    bool iconified{false};
    bool visible{false};
    bool close_requested{false};
    friend bool operator==(const WindowState&, const WindowState&) = default;
};

// Owned diagnostic values are constructed only outside native callbacks. Counts
// saturate rather than wrap; input overflow never blocks release/focus events.
struct WindowDiagnostics {
    std::uint32_t rejected_input_events{0};
    std::uint32_t invalid_window_events{0};
    std::optional<Error> input_error{};
    std::optional<Error> window_error{};
};

class Window;
namespace detail {
class WindowBackend;
[[nodiscard]] Result<std::unique_ptr<Window>> create_window(const WindowConfig& config,
                                                            std::unique_ptr<WindowBackend> backend);
[[nodiscard]] Result<void*> native_test_handle(Window& window);
} // namespace detail

// One owner of one native window and the process GLFW lease. The application
// must create its native owner on the process main thread; this precondition is
// not inferred from the first caller. Nonmovable. All methods and destruction
// require its creating thread. Wrong-
// thread method calls return conflict without touching the backend; destroying a
// live owner on another thread is a programmer error and terminates the process.
class Window final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<Window>> create(const WindowConfig& config = {});
    ~Window() noexcept;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    [[nodiscard]] Result<void> poll_events();
    [[nodiscard]] Result<WindowState> state() const;
    [[nodiscard]] Result<InputSnapshot> capture_input();
    [[nodiscard]] Result<WindowDiagnostics> take_diagnostics();
    [[nodiscard]] Result<void> resize(WindowExtent content);
    [[nodiscard]] Result<void> iconify();
    [[nodiscard]] Result<void> restore();
    [[nodiscard]] Result<void> show();
    [[nodiscard]] Result<void> focus();
    [[nodiscard]] Result<void> request_close();
    // Explicit owner decision before teardown, for unsaved-work confirmation.
    [[nodiscard]] Result<void> cancel_close_request();
    [[nodiscard]] Result<void> close();

  private:
    struct Impl;
    explicit Window(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
    friend Result<std::unique_ptr<Window>>
    detail::create_window(const WindowConfig&, std::unique_ptr<detail::WindowBackend>);
    friend Result<void*> detail::native_test_handle(Window&);
};

// Convert content coordinates using actual framebuffer/content ratios, not the
// monitor content-scale hint. Zero extents, invalid cursor or nonfinite values
// reject. This neither clamps the cursor nor manufactures a rendering surface.
[[nodiscard]] Result<CursorPosition> cursor_to_framebuffer(const WindowState& window,
                                                           CursorPosition cursor);
[[nodiscard]] bool native_window_available() noexcept;

} // namespace shark::client
