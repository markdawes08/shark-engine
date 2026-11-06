#pragma once

#include "shark/prepared_assets.hpp"
#include "shark/prepared_scene.hpp"
#include "shark/runtime_prefab.hpp"
#include "shark/tools/authored_scene.hpp"
#include "shark/tools/prefab_document.hpp"
#include "shark/world_cell.hpp"

namespace shark::tools {

// Pure source-to-Core value bridge for a trusted runtime host. Retains prefab
// local IDs, hierarchy and current component/content references; source project
// and extras are not runtime state. Validates composed transforms before return.
// Performs no file read, asset closure/cook, Scene mutation or authority grant.
[[nodiscard]] Result<RuntimePrefab> prepare_runtime_prefab(const PrefabDocument& prefab);

// Pure bridge from an existing validated SHKGLT01 cache artifact. Keeps dense
// mesh indices, all vertex attributes and temporary unlit base-color/culling
// values. The original artifact retains textures, PBR and alpha-mode data;
// those are deliberately absent from this version of the prepared profile.
// No source file, importer, cache binding or runtime owner is mutated.
// The caller selects a fresh, permitted cache artifact; supplied bytes are not
// proof of project/audience authorization. Reserved built-in IDs reject.
[[nodiscard]] Result<std::vector<std::byte>>
prepare_mesh_asset(AssetId asset, std::span<const std::byte> cached_gltf_payload);

// Capture one committed authoring revision through the existing owner. Preserve
// entity IDs, hierarchy, names, TRS, Camera/MeshRenderer and the world anchor;
// project paths, source extras, history and retry state are not runtime data.
// This does not save source, clear dirty state or acknowledge cache freshness.
[[nodiscard]] Result<std::vector<std::byte>>
prepare_scene(const AuthoredScene& owner,
              std::optional<SceneRevision> expected_revision = std::nullopt);

// Capture one exact committed revision as static world-cell development content.
// The source anchor must already equal the supplied world/cell at zero offset;
// preparation never relocates content or grants runtime/region authority.
// Retains the existing capture guards, source dirty state and authoring history.
[[nodiscard]] Result<std::vector<std::byte>> prepare_world_cell(const AuthoredScene& owner,
                                                                SceneRevision expected_revision,
                                                                WorldCellKey key,
                                                                std::uint64_t content_revision);

// Build a prepared scene from the same cached glTF artifact used for its mesh
// payload. Callers supply one stable entity ID per dense converted node. Names
// must fit Core Name rules; an unnamed node receives the name Node.
// No source camera/light, random identity, global anchor or file path is invented.
[[nodiscard]] Result<std::vector<std::byte>>
prepare_gltf_scene(AssetId asset, SceneId scene, std::span<const std::byte> cached_gltf_payload,
                   std::span<const AuthoredEntityId> node_ids);

} // namespace shark::tools
