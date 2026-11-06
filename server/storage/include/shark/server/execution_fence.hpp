#pragma once

#include "shark/error.hpp"

#include <cstdint>
#include <memory>
#include <thread>

namespace shark::server {

// Process-local execution authority, separate from session and durable epochs.
struct ExecutionPermit {
    std::uint64_t scope{0};
    std::uint64_t generation{0};
    bool operator==(const ExecutionPermit&) const = default;
};

// One creating-thread simulation island. Frozen permits authorize observations
// only; rotation never refreshes an adapter's captured permit implicitly.
class ExecutionFence final {
  public:
    ExecutionFence(const ExecutionFence&) = delete;
    ExecutionFence& operator=(const ExecutionFence&) = delete;
    ExecutionFence(ExecutionFence&&) = delete;
    ExecutionFence& operator=(ExecutionFence&&) = delete;
    [[nodiscard]] static Result<std::shared_ptr<ExecutionFence>> create();
    [[nodiscard]] Result<ExecutionPermit> permit() const;
    [[nodiscard]] Result<void> check(ExecutionPermit permit, bool allow_frozen = false) const;
    [[nodiscard]] Result<void> freeze(ExecutionPermit permit);
    [[nodiscard]] Result<void> resume(ExecutionPermit permit);
    [[nodiscard]] Result<ExecutionPermit> rotate(ExecutionPermit permit);

  private:
    explicit ExecutionFence(std::uint64_t scope) noexcept;
    std::thread::id owner_;
    ExecutionPermit permit_;
    bool frozen_{false};
};

} // namespace shark::server
