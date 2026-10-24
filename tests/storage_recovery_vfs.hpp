#pragma once

#include <cstdint>

namespace shark::storage::test {

enum class IoFault { none, write, sync, full };

// Test executable only: every successful operation delegates to SQLite's real
// native VFS. Arming rejects actual writes/syncs until explicitly cleared.
class RecoveryVfs final {
  public:
    RecoveryVfs();
    ~RecoveryVfs();
    RecoveryVfs(const RecoveryVfs&) = delete;
    RecoveryVfs& operator=(const RecoveryVfs&) = delete;
    void arm(IoFault fault) noexcept;
    void clear() noexcept;
    [[nodiscard]] std::uint64_t failures() const noexcept;
};

} // namespace shark::storage::test
