#pragma once

#include <string>

namespace shark::tools {

// Build identification for the editor foundation; this exposes no authoring API.
[[nodiscard]] std::string editor_version_banner();

} // namespace shark::tools
