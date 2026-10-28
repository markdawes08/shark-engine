#pragma once

#include "shark/stable_id.hpp"

namespace shark {

// Stable content identity shared by authoring and runtime values. A reference
// does not load content, identify a path, or prove an asset is available.
using AssetId = TypedId<struct AssetIdDomain>;

} // namespace shark
