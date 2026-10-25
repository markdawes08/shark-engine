#pragma once

namespace shark::storage::detail {

// Private creating-thread fault/crash seams for explicit offline maintenance.
// Return true to fail the operation; callbacks must not throw or reenter Store.
// Copy steps follow real SQLite backup steps, never simulated successful writes.
enum class MaintenancePhase {
    backup_step,
    backup_complete,
    backup_validated,
    after_v1_to_v2,
    after_v2_to_v3,
    before_migration_commit,
    after_migration_commit,
    restore_step,
    restore_complete,
    restore_validated
};
using MaintenanceFaultHook = bool (*)(MaintenancePhase) noexcept;
void set_maintenance_fault_hook(MaintenanceFaultHook hook) noexcept;

} // namespace shark::storage::detail
