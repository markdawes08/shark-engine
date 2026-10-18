#pragma once

#include "graphics_backend.hpp"

namespace shark::client::detail {

// Private, test-build-only injection. Public factories never install a control,
// no environment/CLI option enables it in Editor or Runtime, and runtime-only
// builds do not compile its implementation. The caller outlives the backend.
enum class NativeGraphicsFault {
    recording_error,
    recording_exception,
    surface_error,
    device_error
};
struct NativeGraphicsTestControl {
    NativeGraphicsFault fault{NativeGraphicsFault::recording_error};
    bool armed{false};
    bool fired{false};
    std::uint32_t teardown_calls{0};
};

[[nodiscard]] std::unique_ptr<GraphicsBackend>
make_fault_graphics_backend(NativeGraphicsTestControl&);

} // namespace shark::client::detail
