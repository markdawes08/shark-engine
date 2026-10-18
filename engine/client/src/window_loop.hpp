#pragma once

#include <shark/client/frame_loop.hpp>
#include <shark/client/window.hpp>
#include <shark/log.hpp>

namespace shark::client::detail {

// Shared by Runtime/Editor and the explicit native smoke. No global state.
// Null report means close requested: no further input tick was dispatched.
[[nodiscard]] Result<std::optional<OfflineFrameReport>>
pump_offline_window(Window& window, OfflineFrameLoop& loop, MonotonicTime now, bool& manual_pause,
                    const OfflineInputCallback& callback, Logger& diagnostics);

} // namespace shark::client::detail
