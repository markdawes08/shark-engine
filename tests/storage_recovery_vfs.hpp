#pragma once

#include <cstdint>

namespace shark::storage::test {

enum class IoFault { none, write, sync, full };
using MainWriteHook = void (*)(int amount, std::int64_t position) noexcept;

// Test executable only: every successful operation delegates to SQLite's real
// native VFS. Arming rejects actual writes/syncs until explicitly cleared.
class RecoveryVfs final {
  public:
    RecoveryVfs();
    ~RecoveryVfs();
    RecoveryVfs(const RecoveryVfs&) = delete;
    RecoveryVfs& operator=(const RecoveryVfs&) = delete;
    void arm(IoFault fault) noexcept;
    // Optional process-crash observer after a successful native MAIN_DB write.
    // No production connection uses this fixture. Existing outage tests leave it null.
    void observe_main_write(MainWriteHook hook) noexcept;
    void clear() noexcept;
    [[nodiscard]] std::uint64_t failures() const noexcept;
};

} // namespace shark::storage::test
