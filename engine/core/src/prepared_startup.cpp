#include "shark/prepared_startup.hpp"

#include "prepared_codec.hpp"

namespace shark {
namespace {
using namespace prepared_detail;
constexpr std::string_view magic = "SHKSTRT1";

void validate(const PreparedStartup& value) {
    take(validate_name(value.name));
    require(value.target == prepared_startup_target, "Unsupported prepared startup target");
    require(value.meshes.size() <= prepared_assets_maximum_count - 2,
            "Prepared startup exceeds its mesh count bound");
    require(value.scene_file.bytes >= envelope_bytes &&
                value.scene_file.bytes <= prepared_scene_maximum_payload_bytes,
            "Prepared startup scene length is outside bounds");
    for (const auto& shader : {value.vertex_shader, value.pixel_shader})
        require(shader.bytes >= 20 && shader.bytes <= prepared_startup_shader_maximum_bytes &&
                    shader.bytes % 4 == 0,
                "Prepared startup shader length is outside bounds");
    std::size_t remaining = prepared_assets_maximum_bytes - value.scene_file.bytes;
    std::optional<AssetId> previous;
    for (const auto& mesh : value.meshes) {
        require(mesh.asset != builtin_cube_asset_id() && mesh.asset != builtin_plane_asset_id() &&
                    (!previous || *previous < mesh.asset),
                "Prepared startup mesh IDs must be sorted, unique and not reserved");
        require(mesh.file.bytes >= envelope_bytes &&
                    mesh.file.bytes <= prepared_mesh_maximum_payload_bytes &&
                    mesh.file.bytes <= remaining,
                "Prepared startup mesh length exceeds its file or aggregate bound");
        remaining -= mesh.file.bytes;
        previous = mesh.asset;
    }
}
void write_file(Writer& writer, const PreparedFileIdentity& value) {
    writer.u32(value.bytes);
    writer.raw(value.digest.bytes);
}
PreparedFileIdentity read_file(Reader& reader) {
    PreparedFileIdentity value;
    value.bytes = reader.u32();
    const auto digest = reader.raw(value.digest.bytes.size());
    std::copy(digest.begin(), digest.end(), value.digest.bytes.begin());
    return value;
}
} // namespace

std::string prepared_startup_mesh_path(AssetId asset) {
    return "content/" + asset.value().to_string() + ".sharkmesh";
}

Result<std::vector<std::byte>> encode_prepared_startup(const PreparedStartup& value) {
    return boundary<std::vector<std::byte>>("prepared.startup.encode", [&] {
        validate(value);
        Writer writer{prepared_startup_maximum_bytes - envelope_bytes};
        writer.id(value.project);
        writer.id(value.world.value());
        writer.id(value.scene.value());
        writer.id(value.camera.value());
        writer.text(value.name.value, maximum_name_bytes);
        writer.text(value.target, 64);
        write_file(writer, value.scene_file);
        write_file(writer, value.vertex_shader);
        write_file(writer, value.pixel_shader);
        writer.u32(static_cast<std::uint32_t>(value.meshes.size()));
        for (const auto& mesh : value.meshes) {
            writer.id(mesh.asset.value());
            write_file(writer, mesh.file);
        }
        return wrap(magic, writer.output, prepared_startup_maximum_bytes);
    });
}

Result<PreparedStartup> decode_prepared_startup(std::span<const std::byte> payload) {
    return boundary<PreparedStartup>("prepared.startup.decode", [&] {
        Reader reader{unwrap(payload, magic, prepared_startup_maximum_bytes),
                      prepared_startup_maximum_bytes};
        PreparedStartup result{reader.id(),
                               WorldId{reader.id()},
                               SceneId{reader.id()},
                               AuthoredEntityId{reader.id()},
                               Name{reader.text(maximum_name_bytes)},
                               reader.text(64),
                               read_file(reader),
                               read_file(reader),
                               read_file(reader),
                               {}};
        const auto count =
            reader.count(prepared_assets_maximum_count - 2, 52, sizeof(PreparedStartupMesh));
        result.meshes.reserve(count);
        for (std::size_t i = 0; i < count; ++i)
            result.meshes.push_back({AssetId{reader.id()}, read_file(reader)});
        reader.finish();
        validate(result);
        return result;
    });
}
} // namespace shark
