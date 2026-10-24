#pragma once

namespace shark::storage::detail {

// Private deterministic process-fixture seam, never exported in public headers.
// The creating thread invokes the callback synchronously. It must not throw or
// reenter storage; production leaves it null. No simulated durability success.
enum class CreatePhase { schema_written, before_commit, after_commit };
using CreateHook = void (*)(CreatePhase) noexcept;
void set_create_hook(CreateHook hook) noexcept;

// A true result injects an I/O error at this boundary. after_commit represents
// an uncertain acknowledgement after a real commit, never simulated success.
enum class WritePhase {
    after_source_debit,
    after_write,
    before_commit,
    after_commit,
    before_rollback
};
using WriteFaultHook = bool (*)(WritePhase) noexcept;
void set_write_fault_hook(WriteFaultHook hook) noexcept;

} // namespace shark::storage::detail
