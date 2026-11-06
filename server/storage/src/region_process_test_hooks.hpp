#pragma once

namespace shark::server::detail {
// Deterministic closed-link adversity, consumed once at the specified actual
// child exchange. Dropping a reply happens after the child processed the bytes.
enum class RegionControlPoint { freeze, activate, movement_commit, combat_commit, step };
enum class RegionControlFault { drop_request, drop_reply };
void set_region_control_fault(RegionControlPoint point, RegionControlFault fault) noexcept;
void clear_region_control_fault() noexcept;
} // namespace shark::server::detail
