#pragma once

#include "shark/asset_id.hpp"
#include "shark/tools/source_document.hpp"

namespace shark::tools {

inline constexpr std::uint32_t prefab_document_version = 1;

// Source-only reusable hierarchy. Entity IDs and parent references are local to
// this asset; instantiation must assign fresh authored entity IDs. This value neither
// creates entities nor loads scripts, physics bodies or referenced mesh assets.
struct PrefabDocument {
    ProjectId project;
    AssetId asset;
    AuthoredEntityId root;
    std::vector<SourceEntity> entities;
    std::string extras_json{"{}"};
    bool operator==(const PrefabDocument&) const = default;
};

// Exact schema 1 only. The nonempty, bounded hierarchy has one explicit root;
// parent references must remain inside it. Nested prefabs are unsupported.
// Existing source/component limits apply, including root-only rigid bodies.
// Extras remain opaque metadata and are never interpreted as entity references.
[[nodiscard]] Result<DecodedSource<PrefabDocument>> decode_prefab_document(std::string_view text);
[[nodiscard]] Result<PrefabDocument> validate_prefab_document(const PrefabDocument& document);
[[nodiscard]] Result<std::string> encode_prefab_document(const PrefabDocument& document);

} // namespace shark::tools
