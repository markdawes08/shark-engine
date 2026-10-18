#pragma once

#include <shark/client/shader_bundle.hpp>

namespace shark::client {

// Fixed diagnostic geometry shared by the interactive sample and executable
// readback qualification. Includes an occluded farther cube to exercise depth.
[[nodiscard]] Result<OffscreenRequest> make_cube_sample(const ShaderBundle& bundle,
                                                        WindowExtent extent = {128, 128},
                                                        bool alternate = false);

} // namespace shark::client
