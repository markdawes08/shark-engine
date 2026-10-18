#include "window_backend.hpp"

namespace shark::client {

bool native_window_available() noexcept {
    return false;
}

std::unique_ptr<detail::WindowBackend> detail::make_native_window_backend() {
    return {};
}

} // namespace shark::client
