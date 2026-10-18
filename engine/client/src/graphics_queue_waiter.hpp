#pragma once

#include <shark/completion_queue.hpp>
#include <shark/error.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace shark::client::detail {

// One owned blocking legacy Vulkan queue-idle operation, never a scheduler.
// The device owner excludes concurrent queue access while pending() is true.
// The worker publishes only an owned integer result through the existing Core
// completion primitive. A timeout does not detach, destroy or cancel the worker.
class GraphicsQueueWaiter final {
  public:
    GraphicsQueueWaiter() = default;
    ~GraphicsQueueWaiter() noexcept {
        if (worker_.joinable()) {
            std::terminate();
        }
    }
    GraphicsQueueWaiter(const GraphicsQueueWaiter&) = delete;
    GraphicsQueueWaiter& operator=(const GraphicsQueueWaiter&) = delete;

    [[nodiscard]] bool pending() const noexcept {
        return worker_.joinable();
    }

    [[nodiscard]] Result<void> start(std::function<int()> action) {
        if (pending()) {
            return error(ErrorCode::conflict, "A graphics queue wait is already owned");
        }
        if (!action) {
            return error(ErrorCode::invalid_argument, "A graphics queue wait action is required");
        }
        ready_ = false;
        failed_ = false;
        try {
            worker_ = std::thread([this, action = std::move(action)]() noexcept {
                bool failed = false;
                try {
                    int result = action();
                    failed = completion_.try_push(std::move(result)) != CompletionPush::accepted;
                } catch (...) {
                    failed = true;
                }
                {
                    const std::lock_guard lock{mutex_};
                    failed_ = failed;
                    ready_ = true;
                }
                ready_changed_.notify_one();
            });
        } catch (...) {
            return error(ErrorCode::execution_failed, "Could not start owned graphics queue wait");
        }
        return {};
    }

    // A value means the operation's thread exited and was joined. Empty means
    // it remains owned. Windows waits for the OS thread to finish, including
    // TLS and DLL detach callbacks, before joining. The non-Windows CPU fixture
    // path bounds the action wait only; standard C++ provides no timed join.
    [[nodiscard]] Result<std::optional<int>> wait(std::uint64_t timeout_ns) {
        if (!pending()) {
            return error(ErrorCode::conflict, "No graphics queue wait is pending");
        }
#if defined(_WIN32)
        auto exited = wait_thread_exit(timeout_ns);
        if (!exited) {
            return exited.error();
        }
        if (!exited.value()) {
            return std::optional<int>{};
        }
#else
        const auto timeout = std::chrono::nanoseconds{
            static_cast<std::int64_t>(std::min<std::uint64_t>(timeout_ns, 1'000'000'000))};
        {
            std::unique_lock lock{mutex_};
            if (!ready_changed_.wait_for(lock, timeout, [this] { return ready_; })) {
                return std::optional<int>{};
            }
        }
#endif
        worker_.join();
        auto result = completion_.try_pop();
        if (failed_ || !result) {
            return error(ErrorCode::execution_failed, "Owned graphics queue wait failed");
        }
        return result;
    }

  private:
#if defined(_WIN32)
    [[nodiscard]] Result<bool> wait_thread_exit(std::uint64_t timeout_ns);
#endif
    static Error error(ErrorCode code, const char* message) {
        return {code, message, {.operation = "graphics.presentation.queue_wait"}};
    }
    CompletionQueue<int, 1> completion_;
    std::mutex mutex_;
    std::condition_variable ready_changed_;
    bool ready_{false};
    bool failed_{false};
    std::thread worker_;
};

} // namespace shark::client::detail
