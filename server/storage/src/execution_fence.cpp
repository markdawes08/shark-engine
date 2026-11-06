#include "shark/server/execution_fence.hpp"

#include <atomic>
#include <limits>

namespace shark::server {
namespace {
std::atomic<std::uint64_t> next_scope{1};
Error rejected(const char* message) {
    return {ErrorCode::conflict, message, {.operation = "server.execution-fence"}};
}
} // namespace

ExecutionFence::ExecutionFence(std::uint64_t scope) noexcept
    : owner_(std::this_thread::get_id()), permit_{scope, 1} {}

Result<std::shared_ptr<ExecutionFence>> ExecutionFence::create() {
    auto scope = next_scope.load(std::memory_order_relaxed);
    for (;;) {
        if (scope == std::numeric_limits<std::uint64_t>::max())
            return rejected("Execution scope is exhausted");
        if (next_scope.compare_exchange_weak(scope, scope + 1, std::memory_order_relaxed))
            break;
    }
    try {
        return std::shared_ptr<ExecutionFence>{new ExecutionFence{scope}};
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Cannot allocate execution fence",
                     {.operation = "server.execution-fence.create"}};
    }
}

Result<ExecutionPermit> ExecutionFence::permit() const {
    if (owner_ != std::this_thread::get_id())
        return rejected("Execution fence requires its creating thread");
    return permit_;
}

Result<void> ExecutionFence::check(ExecutionPermit permit, bool allow_frozen) const {
    if (owner_ != std::this_thread::get_id())
        return rejected("Execution fence requires its creating thread");
    if (permit.scope == 0 || permit.generation == 0 || permit != permit_)
        return rejected("Execution permit is stale or foreign");
    if (frozen_ && !allow_frozen)
        return rejected("Execution is frozen for a consistent transfer cut");
    return {};
}

Result<void> ExecutionFence::freeze(ExecutionPermit permit) {
    if (auto valid = check(permit); !valid)
        return valid;
    frozen_ = true;
    return {};
}

Result<void> ExecutionFence::resume(ExecutionPermit permit) {
    if (auto valid = check(permit, true); !valid)
        return valid;
    if (!frozen_)
        return rejected("Execution is not frozen");
    frozen_ = false;
    return {};
}

Result<ExecutionPermit> ExecutionFence::rotate(ExecutionPermit permit) {
    if (auto valid = check(permit, true); !valid)
        return valid.error();
    if (!frozen_ || permit_.generation == std::numeric_limits<std::uint64_t>::max())
        return rejected("Execution rotation requires a frozen unexhausted permit");
    ++permit_.generation;
    return permit_;
}

} // namespace shark::server
