#include <shark/client/bootstrap.hpp>

namespace shark::client {

std::string_view runtime_help() noexcept {
    return "Usage: Runtime [--help | --version | --window [--window-ms 1..60000]]\n"
           "Shark foundation only; --window opens a native window without rendering or a game.\n"
           "Window sample: F6 toggles offline pause; F7 steps once while paused.\n";
}

std::string_view editor_help() noexcept {
    return "Usage: Editor [--help | --version | --window [--window-ms 1..60000]]\n"
           "Shark foundation only; --window opens a native window, not an authoring editor.\n"
           "Window sample: F6 toggles offline pause; F7 steps once while paused.\n";
}

} // namespace shark::client
