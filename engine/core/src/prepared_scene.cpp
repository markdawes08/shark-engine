#include "shark/prepared_scene.hpp"

#include "prepared_codec.hpp"

#include <map>
#include <utility>

namespace shark {
namespace {
using namespace prepared_detail;
constexpr std::string_view scene_magic = "SHKSCNE1";
constexpr std::size_t minimum_entity_bytes = 104;

struct Candidate {
    PreparedScene value;
    std::vector<std::size_t> order;
    std::vector<std::optional<std::size_t>> parents;
};

// Bounded, iterative hierarchy validation. Core owns all component and matrix
// rules; no runtime handles/incarnations are allocated merely to encode values.
Candidate validate(const PreparedScene& input) {
    require(input.entities.size() <= prepared_scene_maximum_entities,
            "Prepared scene exceeds its entity bound");
    if (input.world_origin)
        take(validate_world_position(*input.world_origin));
    Candidate result{{input.scene, input.world_origin, {}}, {}, {}};
    auto& entities = result.value.entities;
    entities.reserve(input.entities.size());
    std::map<AuthoredEntityId, std::size_t> indices;
    for (const auto& entity : input.entities) {
        try {
            require(indices.emplace(entity.id, entities.size()).second,
                    "Duplicate prepared entity identity");
            take(validate_name(entity.name));
            const auto transform = take(canonicalize_transform(entity.transform));
            if (entity.camera)
                take(validate_camera(*entity.camera));
            if (entity.mesh_renderer)
                take(validate_mesh_renderer(*entity.mesh_renderer));
            entities.push_back({entity.id, entity.parent, entity.name, transform, entity.camera,
                                entity.mesh_renderer});
        } catch (Failure& failure) {
            failure.error.context.entity_id = entity.id.value().to_string();
            throw;
        }
    }
    result.parents.resize(entities.size());
    result.order.reserve(entities.size());
    std::vector<std::vector<std::size_t>> children(entities.size());
    for (std::size_t index = 0; index < entities.size(); ++index) {
        if (!entities[index].parent) {
            result.order.push_back(index);
            continue;
        }
        const auto parent = indices.find(*entities[index].parent);
        if (parent == indices.end() || parent->second == index) {
            throw Failure{Error{ErrorCode::invalid_id,
                                "Prepared entity has a missing or self parent",
                                {.entity_id = entities[index].id.value().to_string()}}};
        }
        result.parents[index] = parent->second;
        children[parent->second].push_back(index);
    }
    std::vector<math::Mat4> worlds(entities.size(), math::Mat4{1.0});
    std::vector<std::size_t> depths(entities.size());
    for (std::size_t cursor = 0; cursor < result.order.size(); ++cursor) {
        const auto index = result.order[cursor];
        const auto parent = result.parents[index];
        try {
            depths[index] = parent ? depths[*parent] + 1 : 1;
            require(depths[index] <= prepared_scene_maximum_hierarchy_depth,
                    "Prepared scene exceeds its hierarchy depth bound");
            const auto local = take(transform_matrix(entities[index].transform));
            worlds[index] = parent ? worlds[*parent] * local : local;
            take(validate_transform_matrix(worlds[index]));
            result.order.insert(result.order.end(), children[index].begin(), children[index].end());
        } catch (Failure& failure) {
            failure.error.context.entity_id = entities[index].id.value().to_string();
            throw;
        }
    }
    require(result.order.size() == entities.size(), "Prepared scene contains a parent cycle");
    return result;
}

bool flag(Reader& reader) {
    const auto value = reader.u8();
    require(value <= 1, "Prepared scene flag must be zero or one");
    return value != 0;
}

void write_transform(Writer& writer, const Transform& transform) {
    for (const auto value :
         {transform.translation.x, transform.translation.y, transform.translation.z,
          transform.rotation.x, transform.rotation.y, transform.rotation.z, transform.rotation.w,
          transform.scale.x, transform.scale.y, transform.scale.z})
        writer.f64(value);
}

Transform read_transform(Reader& reader) {
    Transform result;
    for (std::size_t index = 0; index < 3; ++index)
        result.translation[static_cast<math::Vec3::length_type>(index)] = reader.f64();
    const auto x = reader.f64();
    const auto y = reader.f64();
    const auto z = reader.f64();
    const auto w = reader.f64();
    result.rotation = math::Quat{w, x, y, z};
    for (std::size_t index = 0; index < 3; ++index)
        result.scale[static_cast<math::Vec3::length_type>(index)] = reader.f64();
    return result;
}

Candidate read(std::span<const std::byte> payload) {
    const auto body = unwrap(payload, scene_magic, prepared_scene_maximum_payload_bytes);
    Reader reader{body, prepared_scene_maximum_decoded_bytes};
    reader.charge(1, sizeof(PreparedScene));
    PreparedScene result{SceneId{reader.id()}, std::nullopt, {}};
    if (flag(reader)) {
        WorldPosition origin{WorldId{reader.id()}, {}, {}};
        origin.cell.x = reader.i32();
        origin.cell.y = reader.i32();
        origin.cell.z = reader.i32();
        for (auto& offset : origin.offset)
            offset = reader.f64();
        result.world_origin = origin;
    }
    const auto count =
        reader.count(prepared_scene_maximum_entities, minimum_entity_bytes, sizeof(PreparedEntity));
    result.entities.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        PreparedEntity entity{AuthoredEntityId{reader.id()}};
        try {
            if (flag(reader))
                entity.parent = AuthoredEntityId{reader.id()};
            entity.name.value = reader.text(maximum_name_bytes);
            entity.transform = read_transform(reader);
            if (flag(reader)) {
                Camera camera;
                camera.projection = static_cast<CameraProjection>(reader.u32());
                camera.vertical_fov_radians = reader.f64();
                camera.orthographic_height = reader.f64();
                camera.near_clip = reader.f64();
                camera.far_clip = reader.f64();
                entity.camera = camera;
            }
            if (flag(reader)) {
                const auto asset = AssetId{reader.id()};
                const auto mesh_index = reader.u32();
                entity.mesh_renderer = MeshRenderer{asset, mesh_index, flag(reader)};
            }
            result.entities.push_back(std::move(entity));
        } catch (Failure& failure) {
            failure.error.context.entity_id = entity.id.value().to_string();
            throw;
        }
    }
    reader.finish();
    return validate(result);
}

} // namespace

Result<std::vector<std::byte>> encode_prepared_scene(const PreparedScene& scene) {
    return boundary<std::vector<std::byte>>("prepared.scene.encode", [&] {
        const auto candidate = validate(scene);
        const auto& value = candidate.value;
        Writer writer{prepared_scene_maximum_payload_bytes - envelope_bytes};
        writer.id(value.scene.value());
        writer.u8(value.world_origin ? 1 : 0);
        if (value.world_origin) {
            const auto& origin = *value.world_origin;
            writer.id(origin.world.value());
            writer.i32(static_cast<std::int32_t>(origin.cell.x));
            writer.i32(static_cast<std::int32_t>(origin.cell.y));
            writer.i32(static_cast<std::int32_t>(origin.cell.z));
            for (const auto offset : origin.offset)
                writer.f64(offset);
        }
        writer.u32(static_cast<std::uint32_t>(value.entities.size()));
        for (const auto& entity : value.entities) {
            writer.id(entity.id.value());
            writer.u8(entity.parent ? 1 : 0);
            if (entity.parent)
                writer.id(entity.parent->value());
            writer.text(entity.name.value, maximum_name_bytes);
            write_transform(writer, entity.transform);
            writer.u8(entity.camera ? 1 : 0);
            if (entity.camera) {
                const auto& camera = *entity.camera;
                writer.u32(static_cast<std::uint32_t>(camera.projection));
                writer.f64(camera.vertical_fov_radians);
                writer.f64(camera.orthographic_height);
                writer.f64(camera.near_clip);
                writer.f64(camera.far_clip);
            }
            writer.u8(entity.mesh_renderer ? 1 : 0);
            if (entity.mesh_renderer) {
                writer.id(entity.mesh_renderer->asset.value());
                writer.u32(entity.mesh_renderer->mesh_index);
                writer.u8(entity.mesh_renderer->visible ? 1 : 0);
            }
        }
        return wrap(scene_magic, writer.output, prepared_scene_maximum_payload_bytes);
    });
}

Result<PreparedScene> decode_prepared_scene(std::span<const std::byte> payload) {
    return boundary<PreparedScene>("prepared.scene.decode", [&] { return read(payload).value; });
}

Result<std::unique_ptr<LoadedPreparedScene>>
LoadedPreparedScene::load(std::span<const std::byte> payload,
                          std::shared_ptr<const PreparedAssets> assets) {
    return boundary<std::unique_ptr<LoadedPreparedScene>>("prepared.scene.load", [&] {
        require(assets != nullptr, "Prepared scene loading requires an asset snapshot");
        auto candidate = read(payload);
        for (const auto& entity : candidate.value.entities) {
            if (entity.mesh_renderer) {
                auto resolved = assets->resolve(*entity.mesh_renderer);
                if (!resolved) {
                    auto error = std::move(resolved).error();
                    error.context.entity_id = entity.id.value().to_string();
                    throw Failure{std::move(error)};
                }
            }
        }
        auto scene =
            take(Scene::create(AuthoredSceneScope{candidate.value.scene},
                               {.max_entities = prepared_scene_maximum_entities,
                                .max_hierarchy_depth = prepared_scene_maximum_hierarchy_depth}));
        std::vector<std::optional<EntityHandle>> handles(candidate.value.entities.size());
        for (const auto index : candidate.order) {
            const auto& entity = candidate.value.entities[index];
            const auto parent = candidate.parents[index];
            handles[index] = take(scene->create_entity(
                {entity.id, std::nullopt, entity.name, entity.transform,
                 parent ? handles[*parent] : std::nullopt, entity.camera, entity.mesh_renderer}));
        }
        return std::unique_ptr<LoadedPreparedScene>{new LoadedPreparedScene{
            std::move(scene), std::move(candidate.value.world_origin), std::move(assets)}};
    });
}

} // namespace shark
