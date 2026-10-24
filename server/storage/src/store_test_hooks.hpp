#pragma once

namespace shark::storage::detail {

// Private deterministic process-fixture seam, never exported in public headers.
// The creating thread invokes the callback synchronously. It must not throw or
// reenter storage; production leaves it null. No simulated durability success.
enum class CreatePhase { schema_written, before_commit, after_commit };
using CreateHook = void (*)(CreatePhase) noexcept;
void set_create_hook(CreateHook hook) noexcept;

} // namespace shark::storage::detail
