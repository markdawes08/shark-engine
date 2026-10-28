#include <shark/client/bootstrap.hpp>

namespace shark::client {

std::string_view runtime_help() noexcept {
    return "Usage: Runtime [--help | --version | --window [--window-ms 1..60000] | "
           "--graphics | --cube [--graphics-ms 1..60000] [--device UUID|auto] "
           "[--validation required|off] [--shader-bundle PATH]]\n"
           "Shark foundation only; --window opens an empty native window.\n"
           "--graphics presents a clear color; --cube presents the fixed textured cube.\n"
           "--cube loads assets/cube.sharkshader beside the executable by default.\n"
           "Scene: --scene PATH --camera UUID --vertex-shader PATH --pixel-shader PATH\n"
           "       [--mesh PATH ...] [--extent WIDTHxHEIGHT] [--frames 1..600]\n"
           "       [--scene-ms 1..60000] [--capture-directory NEW_PATH]\n"
           "       [--device UUID|auto] [--validation required|off]\n"
           "Scene inputs are prepared engine payloads; rendering is temporary opaque unlit.\n"
           "F5 reloads scene files; no gameplay or authoring UI is implemented.\n"
           "Validation defaults to off for the sample; required needs the locked validation "
           "tools.\n"
           "All samples: F6 toggles offline pause; F7 steps once while paused.\n";
}

std::string_view editor_help() noexcept {
    return "Usage: Editor [--help | --version | --window [--window-ms 1..60000] | "
           "--graphics | --cube [--graphics-ms 1..60000] [--device UUID|auto] "
           "[--validation required|off] [--shader-bundle PATH]]\n"
           "Shark foundation only; --window opens an empty native window.\n"
           "--graphics presents a clear color; --cube presents the fixed textured cube.\n"
           "--cube loads assets/cube.sharkshader beside the executable by default.\n"
           "There is no authoring editor UI.\n"
           "Validation defaults to off for the sample; required needs the locked validation "
           "tools.\n"
           "All samples: F6 toggles offline pause; F7 steps once while paused.\n";
}

} // namespace shark::client
