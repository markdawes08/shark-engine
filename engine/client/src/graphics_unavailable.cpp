#include "graphics_backend.hpp"

namespace shark::client {
bool native_graphics_available() noexcept {
    return false;
}
namespace detail {
std::unique_ptr<GraphicsBackend> make_graphics_backend() {
    return {};
}
} // namespace detail
} // namespace shark::client
