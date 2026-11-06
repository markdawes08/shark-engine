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
}

struct Candidate {
    PreparedWorldCell value;
    std::span<const std::byte> scene_payload;
};

Candidate read(std::span<const std::byte> payload) {
    const auto body =
        unwrap(payload, cell_magic, world_cell_maximum_payload_bytes, world_cell_payload_version);
    Reader reader{body, 0};
    WorldCellKey key{WorldId{reader.id()}, {}};
    key.cell = {reader.i32(), reader.i32(), reader.i32()};
    const auto revision = reader.integer(8);
    const auto size = static_cast<std::size_t>(reader.u32());
    require(size <= prepared_scene_maximum_payload_bytes,
            "World cell scene exceeds its encoded byte bound");
    const auto scene_payload = reader.raw(size);
    reader.finish();
    auto scene = take(decode_prepared_scene(scene_payload));
    PreparedWorldCell value{key, revision, std::move(scene)};
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
        return wrap(cell_magic, writer.output, world_cell_maximum_payload_bytes,
                    world_cell_payload_version);
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
        return std::unique_ptr<LoadedWorldCell>(new LoadedWorldCell{
            candidate.value.key, candidate.value.content_revision, std::move(scene)});
    });
}

} // namespace shark
