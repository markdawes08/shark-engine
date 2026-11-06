#include "shark/world_cell.hpp"

#include "prepared_codec.hpp"

namespace shark {
namespace {
using namespace prepared_detail;
constexpr std::string_view cell_magic = "SHKCELL1";

void validate(const PreparedWorldCell& cell) {
    const WorldPosition origin{cell.key.world, cell.key.cell, {0.0, 0.0, 0.0}};
    take(validate_world_position(origin));
    require(cell.content_revision != 0, "World cell content revision must be positive");
    require(cell.scene.world_origin && *cell.scene.world_origin == origin,
            "World cell scene anchor must equal its zero-offset cell corner");
    require(cell.scene.entities.size() <= prepared_scene_maximum_entities,
            "World cell exceeds its entity bound");
    for (const auto& entity : cell.scene.entities) {
        if (entity.camera || entity.script ||
            (entity.rigid_body && (entity.rigid_body->motion != BodyMotion::static_body ||
                                   entity.rigid_body->is_trigger))) {
            throw Failure{
                Error{ErrorCode::invalid_configuration,
                      "World cell profile accepts static content without cameras or scripts",
                      {.entity_id = entity.id.value().to_string()}}};
        }
    }
    if (cell.geometry)
        take(validate_world_cell_geometry(*cell.geometry));
}

struct Candidate {
    PreparedWorldCell value;
    std::span<const std::byte> scene_payload;
};

Candidate read(std::span<const std::byte> payload) {
    require(payload.size() >= envelope_bytes, "Truncated world cell envelope");
    Reader header{payload, 0};
    static_cast<void>(header.raw(8));
    const auto version = header.u32();
    require(version >= 1 && version <= world_cell_payload_version,
            "Unsupported world cell payload version");
    const auto body = unwrap(payload, cell_magic, world_cell_maximum_payload_bytes, version);
    Reader reader{body, world_geometry_maximum_decoded_bytes};
    WorldCellKey key{WorldId{reader.id()}, {}};
    key.cell = {reader.i32(), reader.i32(), reader.i32()};
    const auto revision = reader.integer(8);
    const auto size = static_cast<std::size_t>(reader.u32());
    require(size <= prepared_scene_maximum_payload_bytes,
            "World cell scene exceeds its encoded byte bound");
    const auto scene_payload = reader.raw(size);
    auto scene = take(decode_prepared_scene(scene_payload));
    PreparedWorldCell value{key, revision, std::move(scene)};
    if (version >= 2) {
        WorldCellGeometry geometry;
        const auto vertices = reader.count(world_geometry_maximum_vertices, 24, sizeof(math::Vec3));
        geometry.vertices.reserve(vertices);
        for (std::size_t index = 0; index < vertices; ++index) {
            const auto x = reader.f64();
            const auto y = reader.f64();
            const auto z = reader.f64();
            geometry.vertices.emplace_back(x, y, z);
        }
        const auto triangles = [&](std::size_t maximum) {
            const auto count = reader.count(maximum, 12, sizeof(std::array<std::uint32_t, 3>));
            std::vector<std::array<std::uint32_t, 3>> result;
            result.reserve(count);
            for (std::size_t index = 0; index < count; ++index)
                result.push_back({reader.u32(), reader.u32(), reader.u32()});
            return result;
        };
        geometry.collision_triangles = triangles(world_geometry_maximum_collision_triangles);
        geometry.navigation_triangles = triangles(world_geometry_maximum_navigation_triangles);
        value.geometry = std::move(geometry);
    }
    reader.finish();
    validate(value);
    return {std::move(value), scene_payload};
}
} // namespace

Result<std::vector<std::byte>> encode_world_cell(const PreparedWorldCell& cell) {
    return boundary<std::vector<std::byte>>("world.cell.encode", [&] {
        validate(cell);
        const auto scene = take(encode_prepared_scene(cell.scene));
        Writer writer{world_cell_maximum_payload_bytes - envelope_bytes};
        writer.id(cell.key.world.value());
        writer.i32(static_cast<std::int32_t>(cell.key.cell.x));
        writer.i32(static_cast<std::int32_t>(cell.key.cell.y));
        writer.i32(static_cast<std::int32_t>(cell.key.cell.z));
        writer.integer(cell.content_revision, 8);
        writer.u32(static_cast<std::uint32_t>(scene.size()));
        writer.raw(scene);
        if (cell.geometry) {
            const auto& geometry = *cell.geometry;
            writer.u32(static_cast<std::uint32_t>(geometry.vertices.size()));
            for (const auto& vertex : geometry.vertices)
                for (int axis = 0; axis < 3; ++axis)
                    writer.f64(vertex[axis]);
            for (const auto* triangles :
                 {&geometry.collision_triangles, &geometry.navigation_triangles}) {
                writer.u32(static_cast<std::uint32_t>(triangles->size()));
                for (const auto& triangle : *triangles)
                    for (const auto index : triangle)
                        writer.u32(index);
            }
        }
        return wrap(cell_magic, writer.output, world_cell_maximum_payload_bytes,
                    cell.geometry ? world_cell_payload_version : 1);
    });
}

Result<PreparedWorldCell> decode_world_cell(std::span<const std::byte> payload) {
    return boundary<PreparedWorldCell>("world.cell.decode", [&] { return read(payload).value; });
}

Result<std::unique_ptr<LoadedWorldCell>>
LoadedWorldCell::load(std::span<const std::byte> payload,
                      std::shared_ptr<const PreparedAssets> assets) {
    return boundary<std::unique_ptr<LoadedWorldCell>>("world.cell.load", [&] {
        auto candidate = read(payload);
        // Keep the original validated bytes and use the existing complete mesh
        // closure/load boundary, including hidden mesh references.
        auto scene = take(LoadedPreparedScene::load(candidate.scene_payload, std::move(assets)));
        return std::unique_ptr<LoadedWorldCell>(
            new LoadedWorldCell{candidate.value.key, candidate.value.content_revision,
                                std::move(scene), std::move(candidate.value.geometry)});
    });
}

} // namespace shark
