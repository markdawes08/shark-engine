#pragma once

#include "shark/tools/prefab_document.hpp"

namespace shark::tools {

struct PrefabEntityRemap {
    AuthoredEntityId source;
    AuthoredEntityId instance;
    bool operator==(const PrefabEntityRemap&) const = default;
};

struct PrefabEntityOverride {
    AuthoredEntityId entity;
    std::optional<Name> name{};
    std::optional<Transform> transform{};
};

struct PrefabInstanceOptions {
    std::optional<AuthoredEntityId> parent{};
    std::vector<PrefabEntityOverride> overrides{};
};

// Owned command data, not a live Scene or a publication token. Keep the exact
// prepared command for retained retries; preparing again generates new IDs.
struct InstantiatePrefabCommand {
    AuthoredEntityId id;
    PrefabDocument prefab;
    std::vector<PrefabEntityRemap> remap;
    std::optional<AuthoredEntityId> parent{};
    std::vector<PrefabEntityOverride> overrides{};
};

[[nodiscard]] Result<InstantiatePrefabCommand>
prepare_prefab_instance(const PrefabDocument& prefab, const PrefabInstanceOptions& options = {});
[[nodiscard]] Result<InstantiatePrefabCommand>
prepare_prefab_instance(const PrefabDocument& prefab, const PrefabInstanceOptions& options,
                        IdEntropy& entropy);

} // namespace shark::tools
