#include "shark/tools/prepared_content.hpp"

#include "shark/tools/gltf_asset.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace shark::tools {
namespace {

Error failure(const char* operation, const char* message,
              ErrorCode code = ErrorCode::invalid_argument) {
    return Error{code, message, {.operation = operation}};
}

Error contextualize(Error error, const char* operation, std::optional<AssetId> asset = {}) {
    error.context.operation = operation;
    if (asset)
        error.context.asset_id = asset->value().to_string();
    return error;
}

bool charge(std::size_t& used, std::size_t count, std::size_t element_size) {
    if (count > (prepared_mesh_maximum_decoded_bytes - used) / element_size)
        return false;
    used += count * element_size;
    return true;
}

bool reserved(AssetId asset) {
    return asset == builtin_cube_asset_id() || asset == builtin_plane_asset_id();
}

} // namespace

Result<std::vector<std::byte>> prepare_mesh_asset(AssetId asset,
                                                  std::span<const std::byte> cached_gltf_payload) {
    constexpr auto operation = "content.mesh.prepare";
    try {
        if (reserved(asset))
            return contextualize(failure(operation, "Imported content cannot use a built-in ID"),
                                 operation, asset);
        auto decoded = decode_gltf_asset(cached_gltf_payload);
        if (!decoded)
            return contextualize(std::move(decoded).error(), operation, asset);
        const auto& source = decoded.value();
        std::size_t bytes = sizeof(PreparedMeshAsset);
        if (!charge(bytes, source.geometry.meshes.size(), sizeof(PreparedMesh)))
            return contextualize(failure(operation, "Prepared mesh allocation bound exceeded"),
                                 operation, asset);
        // Charge the entire converted value before allocating its nested vectors.
        // The existing cache decoder has already validated every dense reference.
        for (const auto& mesh : source.geometry.meshes) {
            if (!charge(bytes, mesh.primitives.size(), sizeof(PreparedPrimitive)))
                return contextualize(failure(operation, "Prepared primitive bound exceeded"),
                                     operation, asset);
            for (const auto primitive_index : mesh.primitives) {
                const auto& primitive = source.geometry.primitives.at(primitive_index);
                if (!charge(bytes, primitive.vertices.size(), sizeof(PreparedVertex)) ||
                    !charge(bytes, primitive.indices.size(), sizeof(std::uint32_t)))
                    return contextualize(failure(operation, "Prepared geometry bound exceeded"),
                                         operation, asset);
            }
        }
        PreparedMeshAsset prepared{asset, {}};
        prepared.meshes.reserve(source.geometry.meshes.size());
        for (const auto& mesh : source.geometry.meshes) {
            PreparedMesh output;
            output.primitives.reserve(mesh.primitives.size());
            for (const auto primitive_index : mesh.primitives) {
                const auto& primitive = source.geometry.primitives.at(primitive_index);
                PreparedPrimitive converted;
                converted.vertices.reserve(primitive.vertices.size());
                for (const auto& vertex : primitive.vertices) {
                    converted.vertices.push_back({vertex.position, vertex.normal, vertex.tangent,
                                                  vertex.texcoord0, vertex.texcoord1,
                                                  vertex.color});
                }
                converted.indices = primitive.indices;
                if (primitive.source_material) {
                    const auto material = std::lower_bound(
                        source.materials.materials.begin(), source.materials.materials.end(),
                        *primitive.source_material,
                        [](const GltfMaterial& candidate, std::size_t index) {
                            return candidate.source_material < index;
                        });
                    if (material == source.materials.materials.end() ||
                        material->source_material != *primitive.source_material)
                        return contextualize(failure(operation, "Missing primitive material"),
                                             operation, asset);
                    converted.material = {material->base_color, material->double_sided};
                }
                output.primitives.push_back(std::move(converted));
            }
            prepared.meshes.push_back(std::move(output));
        }
        auto encoded = encode_prepared_mesh(prepared);
        if (!encoded)
            return contextualize(std::move(encoded).error(), operation, asset);
        return encoded;
    } catch (...) {
        return contextualize(failure(operation, "Prepared mesh allocation or conversion failed",
                                     ErrorCode::execution_failed),
                             operation, asset);
    }
}

Result<std::vector<std::byte>> prepare_scene(const AuthoredScene& owner,
                                             std::optional<SceneRevision> expected_revision) {
    constexpr auto operation = "content.scene.prepare";
    try {
        auto captured = owner.capture(expected_revision);
        if (!captured)
            return contextualize(std::move(captured).error(), operation);
        const auto& source = captured.value().scene;
        PreparedScene prepared{source.scene, source.world_origin, {}};
        prepared.entities.reserve(source.entities.size());
        for (const auto& entity : source.entities) {
            prepared.entities.push_back({entity.id, entity.parent, entity.name, entity.transform,
                                         entity.camera, entity.mesh_renderer});
        }
        auto encoded = encode_prepared_scene(prepared);
        if (!encoded)
            return contextualize(std::move(encoded).error(), operation);
        return encoded;
    } catch (...) {
        return failure(operation, "Prepared scene allocation or conversion failed",
                       ErrorCode::execution_failed);
    }
}

Result<std::vector<std::byte>> prepare_gltf_scene(AssetId asset, SceneId scene,
                                                  std::span<const std::byte> cached_gltf_payload,
                                                  std::span<const AuthoredEntityId> node_ids) {
    constexpr auto operation = "content.gltf-scene.prepare";
    try {
        if (reserved(asset))
            return contextualize(failure(operation, "Imported content cannot use a built-in ID"),
                                 operation, asset);
        if (node_ids.size() > prepared_scene_maximum_entities)
            return contextualize(failure(operation, "Imported node identity bound exceeded"),
                                 operation, asset);
        auto decoded = decode_gltf_asset(cached_gltf_payload);
        if (!decoded)
            return contextualize(std::move(decoded).error(), operation, asset);
        const auto& nodes = decoded.value().geometry.nodes;
        if (nodes.size() != node_ids.size())
            return contextualize(failure(operation, "One identity is required per imported node"),
                                 operation, asset);
        PreparedScene prepared{scene, std::nullopt, {}};
        prepared.entities.reserve(nodes.size());
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            const auto& node = nodes[index];
            std::optional<MeshRenderer> renderer;
            if (node.mesh) {
                if (*node.mesh > std::numeric_limits<std::uint32_t>::max())
                    return contextualize(failure(operation, "Imported mesh index is too large"),
                                         operation, asset);
                renderer = MeshRenderer{asset, static_cast<std::uint32_t>(*node.mesh), true};
            }
            prepared.entities.push_back(
                {node_ids[index],
                 node.parent ? std::optional{node_ids[*node.parent]} : std::nullopt,
                 Name{node.name.empty() ? "Node" : node.name}, node.local, std::nullopt, renderer});
        }
        auto encoded = encode_prepared_scene(prepared);
        if (!encoded)
            return contextualize(std::move(encoded).error(), operation, asset);
        return encoded;
    } catch (...) {
        return contextualize(failure(operation, "Imported scene allocation or conversion failed",
                                     ErrorCode::execution_failed),
                             operation, asset);
    }
}

} // namespace shark::tools
