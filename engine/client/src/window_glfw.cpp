#include "window_backend.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <GLFW/glfw3native.h>

#include <atomic>
#include <memory>

namespace shark::client {
namespace {

// GLFW is process-global. Only the successful claimant may initialize, install
// the error bridge, terminate or restore its predecessor's error callback.
std::atomic_flag glfw_claimed = ATOMIC_FLAG_INIT;
std::atomic<int> pending_error{GLFW_NO_ERROR};
static_assert(std::atomic<int>::is_always_lock_free);

void error_callback(int code, const char*) noexcept {
    // The native description is borrowed and potentially unbounded. Retain only
    // a fixed error flag here; the owner constructs bounded diagnostics later.
    pending_error.store(code, std::memory_order_relaxed);
}

class GlfwWindowBackend final : public detail::WindowBackend {
  public:
    detail::BackendClaim claim() noexcept override {
        if (claimed_ || glfw_claimed.test_and_set(std::memory_order_acquire)) {
            return detail::BackendClaim::conflict;
        }
        claimed_ = true;
        pending_error.store(GLFW_NO_ERROR, std::memory_order_relaxed);
        previous_error_ = glfwSetErrorCallback(error_callback);
        return detail::BackendClaim::acquired;
    }

    bool initialize() noexcept override {
        const bool initialized = glfwInit() == GLFW_TRUE;
        return native_ok() && initialized;
    }

    bool create(const WindowConfig& config) noexcept override {
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_VISIBLE, config.visible ? GLFW_TRUE : GLFW_FALSE);
        glfwWindowHint(GLFW_RESIZABLE, config.resizable ? GLFW_TRUE : GLFW_FALSE);
        glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
        window_ = glfwCreateWindow(static_cast<int>(config.content.width),
                                   static_cast<int>(config.content.height), config.title.c_str(),
                                   nullptr, nullptr);
        return native_ok() && window_ != nullptr;
    }

    bool install_callbacks(detail::WindowEventSink& sink) noexcept override {
        sink_ = &sink;
        glfwSetWindowUserPointer(window_, this);
        glfwSetKeyCallback(window_, [](GLFWwindow* window, int key, int, int action, int) noexcept {
            if (auto* target = callback_sink(window)) {
                target->key(key, action);
            }
        });
        glfwSetMouseButtonCallback(window_,
                                   [](GLFWwindow* window, int button, int action, int) noexcept {
                                       if (auto* target = callback_sink(window)) {
                                           target->mouse_button(button, action);
                                       }
                                   });
        glfwSetCursorPosCallback(window_, [](GLFWwindow* window, double x, double y) noexcept {
            if (auto* target = callback_sink(window)) {
                target->cursor(x, y);
            }
        });
        glfwSetScrollCallback(window_, [](GLFWwindow* window, double x, double y) noexcept {
            if (auto* target = callback_sink(window)) {
                target->scroll(x, y);
            }
        });
        glfwSetCharCallback(window_, [](GLFWwindow* window, unsigned int scalar) noexcept {
            if (auto* target = callback_sink(window)) {
                target->text(static_cast<char32_t>(scalar));
            }
        });
        glfwSetWindowSizeCallback(window_, [](GLFWwindow* window, int width, int height) noexcept {
            if (auto* target = callback_sink(window)) {
                target->content_size(width, height);
            }
        });
        glfwSetFramebufferSizeCallback(window_,
                                       [](GLFWwindow* window, int width, int height) noexcept {
                                           if (auto* target = callback_sink(window)) {
                                               target->framebuffer_size(width, height);
                                           }
                                       });
        glfwSetWindowContentScaleCallback(window_,
                                          [](GLFWwindow* window, float x, float y) noexcept {
                                              if (auto* target = callback_sink(window)) {
                                                  target->content_scale(x, y);
                                              }
                                          });
        glfwSetWindowFocusCallback(window_, [](GLFWwindow* window, int focused) noexcept {
            if (auto* target = callback_sink(window)) {
                target->focus(focused == GLFW_TRUE);
            }
        });
        glfwSetWindowIconifyCallback(window_, [](GLFWwindow* window, int iconified) noexcept {
            if (auto* target = callback_sink(window)) {
                target->iconify(iconified == GLFW_TRUE);
            }
        });
        glfwSetWindowCloseCallback(window_, [](GLFWwindow* window) noexcept {
            if (auto* target = callback_sink(window)) {
                target->close_requested();
            }
        });
        return native_ok();
    }

    bool query(WindowState& state) noexcept override {
        int width = 0;
        int height = 0;
        int framebuffer_width = 0;
        int framebuffer_height = 0;
        glfwGetWindowSize(window_, &width, &height);
        glfwGetFramebufferSize(window_, &framebuffer_width, &framebuffer_height);
        glfwGetWindowContentScale(window_, &state.scale.x, &state.scale.y);
        state.focused = glfwGetWindowAttrib(window_, GLFW_FOCUSED) == GLFW_TRUE;
        state.iconified = glfwGetWindowAttrib(window_, GLFW_ICONIFIED) == GLFW_TRUE;
        state.visible = glfwGetWindowAttrib(window_, GLFW_VISIBLE) == GLFW_TRUE;
        state.close_requested = glfwWindowShouldClose(window_) == GLFW_TRUE;
        if (!native_ok() || width < 0 || height < 0 || framebuffer_width < 0 ||
            framebuffer_height < 0) {
            return false;
        }
        state.content = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
        state.framebuffer = {static_cast<std::uint32_t>(framebuffer_width),
                             static_cast<std::uint32_t>(framebuffer_height)};
        return true;
    }

    bool poll() noexcept override {
        glfwPollEvents();
        return native_ok();
    }

    bool command(detail::WindowCommand command, WindowExtent size) noexcept override {
        switch (command) {
        case detail::WindowCommand::resize:
            glfwSetWindowSize(window_, static_cast<int>(size.width), static_cast<int>(size.height));
            break;
        case detail::WindowCommand::iconify:
            glfwIconifyWindow(window_);
            break;
        case detail::WindowCommand::restore:
            glfwRestoreWindow(window_);
            break;
        case detail::WindowCommand::show:
            glfwShowWindow(window_);
            break;
        case detail::WindowCommand::focus:
            glfwFocusWindow(window_);
            break;
        case detail::WindowCommand::request_close:
            glfwSetWindowShouldClose(window_, GLFW_TRUE);
            break;
        }
        return native_ok();
    }

    void uninstall_callbacks() noexcept override {
        sink_ = nullptr;
        if (window_ == nullptr) {
            return;
        }
        glfwSetKeyCallback(window_, nullptr);
        glfwSetMouseButtonCallback(window_, nullptr);
        glfwSetCursorPosCallback(window_, nullptr);
        glfwSetScrollCallback(window_, nullptr);
        glfwSetCharCallback(window_, nullptr);
        glfwSetWindowSizeCallback(window_, nullptr);
        glfwSetFramebufferSizeCallback(window_, nullptr);
        glfwSetWindowContentScaleCallback(window_, nullptr);
        glfwSetWindowFocusCallback(window_, nullptr);
        glfwSetWindowIconifyCallback(window_, nullptr);
        glfwSetWindowCloseCallback(window_, nullptr);
        glfwSetWindowUserPointer(window_, nullptr);
    }

    void destroy() noexcept override {
        if (window_ != nullptr) {
            glfwDestroyWindow(window_);
            window_ = nullptr;
        }
    }

    void terminate() noexcept override {
        glfwTerminate();
    }

    void release() noexcept override {
        if (claimed_) {
            glfwSetErrorCallback(previous_error_);
            claimed_ = false;
            pending_error.store(GLFW_NO_ERROR, std::memory_order_relaxed);
            glfw_claimed.clear(std::memory_order_release);
        }
    }

    void* native_handle() const noexcept override {
        return window_ == nullptr ? nullptr : glfwGetWin32Window(window_);
    }

    int last_error_code() const noexcept override {
        return last_error_;
    }

  private:
    [[nodiscard]] bool native_ok() noexcept {
        last_error_ = pending_error.exchange(GLFW_NO_ERROR, std::memory_order_relaxed);
        return last_error_ == GLFW_NO_ERROR;
    }

    [[nodiscard]] static detail::WindowEventSink* callback_sink(GLFWwindow* window) noexcept {
        auto* backend = static_cast<GlfwWindowBackend*>(glfwGetWindowUserPointer(window));
        return backend == nullptr ? nullptr : backend->sink_;
    }

    GLFWwindow* window_{nullptr};
    detail::WindowEventSink* sink_{nullptr};
    GLFWerrorfun previous_error_{nullptr};
    bool claimed_{false};
    int last_error_{GLFW_NO_ERROR};
};

} // namespace

bool native_window_available() noexcept {
    return true;
}

std::unique_ptr<detail::WindowBackend> detail::make_native_window_backend() {
    return std::make_unique<GlfwWindowBackend>();
}

} // namespace shark::client
