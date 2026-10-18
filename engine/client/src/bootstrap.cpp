#include <shark/client/bootstrap.hpp>

namespace shark::client {

std::string_view runtime_help() noexcept {
    return "Usage: Runtime [--help | --version]\n"
           "Shark build foundation only; game loading and rendering are not implemented.\n";
}

std::string_view editor_help() noexcept {
    return "Usage: Editor [--help | --version]\n"
           "Shark build foundation only; the graphical editor is not implemented.\n";
}

} // namespace shark::client
