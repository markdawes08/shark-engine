#pragma once

#include <cstdint>

namespace shark::storage::detail {

// Private deterministic durable worker barriers. Installed on the creating thread before
// open_runtime; each worker copies the hook. shutdown_requested runs on the owner
// thread; all other phases run on the worker. Callbacks run outside channel locks,
// must not throw/reenter storage, and must eventually return.
// Tests may coordinate bounded condition-variable barriers; production is null.
// closing precedes Store destruction; closed follows it on normal worker shutdown.
enum class RuntimeWorkerPhase {
    opened,
    before_execute,
    after_execute,
    before_publish,
    shutdown_requested,
    closing,
    closed
};
using RuntimeWorkerHook = void (*)(RuntimeWorkerPhase, std::uint64_t sequence) noexcept;
void set_runtime_worker_hook(RuntimeWorkerHook hook) noexcept;
// Throws internally for the first asynchronous command; verifies the worker's
// exception boundary without permitting an exception to cross a test callback.
void set_runtime_worker_exception(bool enabled) noexcept;

} // namespace shark::storage::detail
