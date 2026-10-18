#include <shark/client/bootstrap.hpp>

namespace shark::client {

std::string_view runtime_help() noexcept {
    return "Usage: Runtime [--help | --version | --window [--window-ms 1..60000] | "
           "--graphics [--graphics-ms 1..60000] [--device UUID|auto] "
           "[--validation required|off]]\n"
           "Shark foundation only; --window opens an empty native window.\n"
           "--graphics presents a clear color; there is no game or scene renderer.\n"
           "Validation defaults to off for the sample; required needs the locked validation "
           "tools.\n"
           "Both samples: F6 toggles offline pause; F7 steps once while paused.\n";
}

std::string_view editor_help() noexcept {
    return "Usage: Editor [--help | --version | --window [--window-ms 1..60000] | "
           "--graphics [--graphics-ms 1..60000] [--device UUID|auto] "
           "[--validation required|off]]\n"
           "Shark foundation only; --window opens an empty native window.\n"
           "--graphics presents a clear color; there is no authoring editor UI.\n"
           "Validation defaults to off for the sample; required needs the locked validation "
           "tools.\n"
           "Both samples: F6 toggles offline pause; F7 steps once while paused.\n";
}

} // namespace shark::client
