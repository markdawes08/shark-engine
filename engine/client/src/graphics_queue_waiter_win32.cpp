#include "graphics_queue_waiter.hpp"

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace shark::client::detail {

Result<bool> GraphicsQueueWaiter::wait_thread_exit(std::uint64_t timeout_ns) {
    // Floor to the native timer unit: sub-millisecond waits remain polls and
    // cannot gain a second timeout budget after the worker publishes its result.
    const auto timeout_ms =
        static_cast<DWORD>(std::min<std::uint64_t>(timeout_ns, 1'000'000'000) / 1'000'000);
    const auto result = WaitForSingleObject(worker_.native_handle(), timeout_ms);
    if (result == WAIT_OBJECT_0) {
        return true;
    }
    if (result == WAIT_TIMEOUT) {
        return false;
    }
    // The std::thread retains its handle and completion on every failed wait.
    // Only join(), after observed thread exit, consumes that ownership.
    const auto details = result == WAIT_FAILED ? "Win32 error " + std::to_string(GetLastError())
                                               : "unexpected wait status " + std::to_string(result);
    return Error{ErrorCode::execution_failed,
                 "Could not observe graphics queue thread exit: " + details,
                 {.operation = "graphics.presentation.queue_wait"}};
}

} // namespace shark::client::detail
