#include <shark/tools/build_info.hpp>

#include <shark/bootstrap.hpp>

namespace shark::tools {

std::string editor_version_banner() {
    return "Shark Editor " + std::string(version_string());
}

} // namespace shark::tools
