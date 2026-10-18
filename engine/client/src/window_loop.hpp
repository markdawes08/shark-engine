#pragma once

#include <shark/client/frame_loop.hpp>
#include <shark/client/graphics.hpp>
#include <shark/log.hpp>

namespace shark::client::detail {

// Both window-only and graphics samples consume the same owned snapshot and
// offline pause/step policy. This never touches a live server scheduler.
[[nodiscard]] Result<std::optional<OfflineFrameReport>>
advance_offline_window(GraphicsWindowFrame frame, OfflineFrameLoop& loop, MonotonicTime now,
                       bool& manual_pause, const OfflineInputCallback& callback,
                       Logger& diagnostics);

[[nodiscard]] Result<void> write_offline_control(bool prior_pause, bool manual_pause,
                                                 const OfflineFrameReport& frame,
                                                 std::uint64_t next_tick, std::ostream& output);

// One final sample summary after successful close. NOT_RUN is deliberate:
// interactive samples do not execute the hardware qualification workload.
[[nodiscard]] Result<void>
write_presentation_completion(std::string_view adapter_uuid, GraphicsValidation validation,
                              const PresentationStats& stats, std::ostream& output,
                              std::string_view shader_bundle_sha256 = {});

// Shared by Runtime/Editor and the explicit native smoke. No global state.
// Null report means close requested: no further input tick was dispatched.
[[nodiscard]] Result<std::optional<OfflineFrameReport>>
pump_offline_window(Window& window, OfflineFrameLoop& loop, MonotonicTime now, bool& manual_pause,
                    const OfflineInputCallback& callback, Logger& diagnostics);

} // namespace shark::client::detail
