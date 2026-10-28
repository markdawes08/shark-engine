#include "shark/prepared_assets.hpp"

#include "prepared_codec.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace shark {
namespace {
using namespace prepared_detail;
constexpr std::size_t vertex_wire_bytes = 18 * sizeof(std::uint32_t);
constexpr std::size_t primitive_minimum_wire_bytes = 17 + 4 + vertex_wire_bytes + 4 + 3 * 4;

template <std::size_t N> bool finite(const std::array<float, N>& values) {
    return std::all_of(values.begin(), values.end(),
                       [](float value) { return std::isfinite(value); });
}
template <std::size_t N> bool unit_interval(const std::array<float, N>& values) {
    return std::all_of(values.begin(), values.end(), [](float value) {
        return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
    });
}
std::size_t validate(const PreparedMeshAsset& asset) {
    require(!asset.meshes.empty() && asset.meshes.size() <= prepared_mesh_maximum_meshes,
            "Prepared mesh asset requires a bounded nonempty mesh inventory");
    std::size_t charged = sizeof(PreparedMeshAsset);
    const auto charge = [&](std::size_t count, std::size_t width) {
        require(count <= (prepared_mesh_maximum_decoded_bytes - charged) / width,
                "Prepared mesh exceeds its decoded allocation bound");
        charged += count * width;
    };
    charge(asset.meshes.size(), sizeof(PreparedMesh));
    std::size_t primitive_count{};
    std::size_t vertex_count{};
    std::size_t index_count{};
    for (const auto& mesh : asset.meshes) {
        require(!mesh.primitives.empty() &&
                    mesh.primitives.size() <= prepared_mesh_maximum_primitives - primitive_count,
                "Prepared mesh requires bounded nonempty primitives");
        primitive_count += mesh.primitives.size();
        charge(mesh.primitives.size(), sizeof(PreparedPrimitive));
        for (const auto& primitive : mesh.primitives) {
            require(!primitive.vertices.empty() && !primitive.indices.empty() &&
                        primitive.indices.size() % 3 == 0 &&
                        primitive.vertices.size() <=
                            prepared_mesh_maximum_vertices - vertex_count &&
                        primitive.indices.size() <= prepared_mesh_maximum_indices - index_count,
                    "Invalid prepared triangle inventory");
            vertex_count += primitive.vertices.size();
            index_count += primitive.indices.size();
            charge(primitive.vertices.size(), sizeof(PreparedVertex));
            charge(primitive.indices.size(), sizeof(std::uint32_t));
            require(unit_interval(primitive.material.base_color),
                    "Invalid prepared material color");
            for (const auto& vertex : primitive.vertices) {
                require(finite(vertex.position) && finite(vertex.normal) &&
                            finite(vertex.tangent) && finite(vertex.texcoord0) &&
                            finite(vertex.texcoord1) && unit_interval(vertex.color),
                        "Invalid prepared vertex attributes");
                require(std::abs(std::hypot(vertex.normal[0], vertex.normal[1], vertex.normal[2]) -
                                 1.0F) <= 1e-4F,
                        "Prepared vertex normal must have unit length");
                require(
                    std::abs(std::hypot(vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]) -
                             1.0F) <= 1e-4F &&
                        (vertex.tangent[3] == -1.0F || vertex.tangent[3] == 1.0F),
                    "Invalid prepared vertex tangent");
                // Missing source tangents have the fixed default basis. Do not
                // infer tangent presence or normal-map support from this subset.
            }
            for (const auto index : primitive.indices)
                require(index < primitive.vertices.size(),
                        "Prepared index is outside its primitive");
        }
    }
    return charged;
}
template <std::size_t N> void write_values(Writer& writer, const std::array<float, N>& values) {
    for (const auto value : values)
        writer.f32(value);
}
template <std::size_t N> void read_values(Reader& reader, std::array<float, N>& values) {
    for (auto& value : values)
        value = reader.f32();
}
void write_vertex(Writer& writer, const PreparedVertex& vertex) {
    write_values(writer, vertex.position);
    write_values(writer, vertex.normal);
    write_values(writer, vertex.tangent);
    write_values(writer, vertex.texcoord0);
    write_values(writer, vertex.texcoord1);
    write_values(writer, vertex.color);
}
PreparedVertex read_vertex(Reader& reader) {
    PreparedVertex vertex;
    read_values(reader, vertex.position);
    read_values(reader, vertex.normal);
    read_values(reader, vertex.tangent);
    read_values(reader, vertex.texcoord0);
    read_values(reader, vertex.texcoord1);
    read_values(reader, vertex.color);
    return vertex;
}
std::uint32_t count32(std::size_t size) {
    require(size <= std::numeric_limits<std::uint32_t>::max(),
            "Prepared count cannot be represented");
    return static_cast<std::uint32_t>(size);
}
PreparedPrimitive quad(std::array<float, 3> normal, std::array<float, 3> tangent,
                       const std::array<std::array<float, 3>, 4>& positions) {
    PreparedPrimitive result;
    result.vertices.reserve(4);
    constexpr std::array<std::array<float, 2>, 4> uv{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    for (std::size_t i = 0; i < positions.size(); ++i) {
        PreparedVertex vertex;
        vertex.position = positions[i];
        vertex.normal = normal;
        vertex.tangent = {tangent[0], tangent[1], tangent[2], 1.0F};
        vertex.texcoord0 = uv[i];
        result.vertices.push_back(vertex);
    }
    result.indices = {0, 1, 2, 0, 2, 3};
    return result;
}
PreparedMeshAsset cube() {
    // Face-local CCW quads viewed from outside. Positions span one meter;
    // duplicated face vertices preserve flat normals and independent UVs.
    constexpr float lo = -0.5F;
    constexpr float hi = 0.5F;
    const std::array faces{
        quad({1, 0, 0}, {0, 0, -1}, {{{hi, lo, hi}, {hi, lo, lo}, {hi, hi, lo}, {hi, hi, hi}}}),
        quad({-1, 0, 0}, {0, 0, 1}, {{{lo, lo, lo}, {lo, lo, hi}, {lo, hi, hi}, {lo, hi, lo}}}),
        quad({0, 1, 0}, {1, 0, 0}, {{{lo, hi, hi}, {hi, hi, hi}, {hi, hi, lo}, {lo, hi, lo}}}),
        quad({0, -1, 0}, {1, 0, 0}, {{{lo, lo, lo}, {hi, lo, lo}, {hi, lo, hi}, {lo, lo, hi}}}),
        quad({0, 0, 1}, {1, 0, 0}, {{{lo, lo, hi}, {hi, lo, hi}, {hi, hi, hi}, {lo, hi, hi}}}),
        quad({0, 0, -1}, {-1, 0, 0}, {{{hi, lo, lo}, {lo, lo, lo}, {lo, hi, lo}, {hi, hi, lo}}})};
    PreparedPrimitive primitive;
    primitive.vertices.reserve(24);
    primitive.indices.reserve(36);
    for (const auto& face : faces) {
        const auto base = count32(primitive.vertices.size());
        primitive.vertices.insert(primitive.vertices.end(), face.vertices.begin(),
                                  face.vertices.end());
        for (const auto index : face.indices)
            primitive.indices.push_back(base + index);
    }
    return {builtin_cube_asset_id(), {{{std::move(primitive)}}}};
}
PreparedMeshAsset plane() {
    return {builtin_plane_asset_id(),
            {{{quad({0, 1, 0}, {1, 0, 0},
                    {{{-0.5F, 0, 0.5F}, {0.5F, 0, 0.5F}, {0.5F, 0, -0.5F}, {-0.5F, 0, -0.5F}}})}}}};
}
} // namespace

AssetId builtin_cube_asset_id() {
    return AssetId{StableId::parse("00000000-0000-4000-8000-000000000013").value()};
}
AssetId builtin_plane_asset_id() {
    return AssetId{StableId::parse("00000000-0000-4000-8000-000000000014").value()};
}
Result<std::vector<std::byte>> encode_prepared_mesh(const PreparedMeshAsset& asset) {
    return boundary<std::vector<std::byte>>("prepared.mesh.encode", [&] {
        static_cast<void>(validate(asset));
        Writer writer{prepared_mesh_maximum_payload_bytes - envelope_bytes};
        writer.id(asset.asset.value());
        writer.u32(count32(asset.meshes.size()));
        for (const auto& mesh : asset.meshes) {
            writer.u32(count32(mesh.primitives.size()));
            for (const auto& primitive : mesh.primitives) {
                write_values(writer, primitive.material.base_color);
                writer.u8(primitive.material.double_sided ? 1 : 0);
                writer.u32(count32(primitive.vertices.size()));
                for (const auto& vertex : primitive.vertices)
                    write_vertex(writer, vertex);
                writer.u32(count32(primitive.indices.size()));
                for (const auto index : primitive.indices)
                    writer.u32(index);
            }
        }
        return wrap("SHKMESH1", writer.output, prepared_mesh_maximum_payload_bytes);
    });
}
namespace {
PreparedMeshAsset decode_mesh(std::span<const std::byte> payload, std::size_t decoded_limit) {
    Reader reader{unwrap(payload, "SHKMESH1", prepared_mesh_maximum_payload_bytes), decoded_limit};
    reader.charge(1, sizeof(PreparedMeshAsset));
    PreparedMeshAsset result{AssetId{reader.id()}, {}};
    const auto mesh_count = reader.count(prepared_mesh_maximum_meshes,
                                         4 + primitive_minimum_wire_bytes, sizeof(PreparedMesh));
    result.meshes.reserve(mesh_count);
    std::size_t primitives{};
    std::size_t vertices{};
    std::size_t indices{};
    for (std::size_t mesh_index = 0; mesh_index < mesh_count; ++mesh_index) {
        PreparedMesh mesh;
        const auto primitive_count =
            reader.count(prepared_mesh_maximum_primitives - primitives,
                         primitive_minimum_wire_bytes, sizeof(PreparedPrimitive));
        primitives += primitive_count;
        mesh.primitives.reserve(primitive_count);
        for (std::size_t primitive_index = 0; primitive_index < primitive_count;
             ++primitive_index) {
            PreparedPrimitive primitive;
            read_values(reader, primitive.material.base_color);
            const auto double_sided = reader.u8();
            require(double_sided <= 1, "Invalid prepared material boolean");
            primitive.material.double_sided = double_sided != 0;
            const auto vertex_count = reader.count(prepared_mesh_maximum_vertices - vertices,
                                                   vertex_wire_bytes, sizeof(PreparedVertex));
            vertices += vertex_count;
            primitive.vertices.reserve(vertex_count);
            for (std::size_t vertex_index = 0; vertex_index < vertex_count; ++vertex_index)
                primitive.vertices.push_back(read_vertex(reader));
            const auto index_count = reader.count(prepared_mesh_maximum_indices - indices,
                                                  sizeof(std::uint32_t), sizeof(std::uint32_t));
            indices += index_count;
            primitive.indices.reserve(index_count);
            for (std::size_t index = 0; index < index_count; ++index)
                primitive.indices.push_back(reader.u32());
            mesh.primitives.push_back(std::move(primitive));
        }
        result.meshes.push_back(std::move(mesh));
    }
    reader.finish();
    static_cast<void>(validate(result));
    return result;
}
} // namespace

Result<PreparedMeshAsset> decode_prepared_mesh(std::span<const std::byte> payload) {
    return boundary<PreparedMeshAsset>("prepared.mesh.decode", [&] {
        return decode_mesh(payload, prepared_mesh_maximum_decoded_bytes);
    });
}

PreparedAssets::PreparedAssets(std::vector<std::shared_ptr<const PreparedMeshAsset>> assets,
                               std::size_t retained_bytes)
    : assets_(std::move(assets)), retained_bytes_(retained_bytes) {}

Result<std::shared_ptr<const PreparedAssets>>
PreparedAssets::create(std::span<const PreparedAssetInput> inputs) {
    return boundary<std::shared_ptr<const PreparedAssets>>("prepared.assets.create", [&] {
        require(inputs.size() <= prepared_assets_maximum_count - 2,
                "Prepared asset snapshot exceeds its count bound");
        std::size_t input_bytes{};
        std::set<AssetId> identities{builtin_cube_asset_id(), builtin_plane_asset_id()};
        for (const auto& input : inputs) {
            require(input.payload.size() <= prepared_mesh_maximum_payload_bytes &&
                        input.payload.size() <= prepared_assets_maximum_bytes - input_bytes,
                    "Prepared asset snapshot exceeds its input byte bound");
            input_bytes += input.payload.size();
            require(identities.insert(input.asset).second,
                    "Duplicate or engine-reserved prepared asset identity");
        }
        std::vector<std::shared_ptr<const PreparedMeshAsset>> assets;
        assets.reserve(inputs.size() + 2);
        std::size_t retained = sizeof(PreparedAssets) + assets.capacity() * sizeof(assets.front());
        const auto append = [&](PreparedMeshAsset value) {
            const auto size = validate(value);
            require(size <= prepared_assets_maximum_bytes - retained,
                    "Prepared asset snapshot exceeds its retained byte bound");
            retained += size;
            assets.push_back(std::make_shared<const PreparedMeshAsset>(std::move(value)));
        };
        append(cube());
        append(plane());
        for (const auto& input : inputs) {
            auto value =
                decode_mesh(input.payload, std::min(prepared_mesh_maximum_decoded_bytes,
                                                    prepared_assets_maximum_bytes - retained));
            require(value.asset == input.asset,
                    "Prepared payload identity does not match its reference");
            append(std::move(value));
        }
        std::sort(assets.begin(), assets.end(),
                  [](const auto& left, const auto& right) { return left->asset < right->asset; });
        return std::shared_ptr<const PreparedAssets>{
            new PreparedAssets{std::move(assets), retained}};
    });
}
Result<std::shared_ptr<const PreparedMeshAsset>> PreparedAssets::find(AssetId asset) const {
    return boundary<std::shared_ptr<const PreparedMeshAsset>>("prepared.assets.find", [&] {
        const auto found =
            std::lower_bound(assets_.begin(), assets_.end(), asset,
                             [](const auto& value, AssetId id) { return value->asset < id; });
        if (found == assets_.end() || (*found)->asset != asset) {
            Error error{ErrorCode::invalid_argument, "Prepared asset is not in this snapshot", {}};
            error.context.asset_id = asset.value().to_string();
            throw Failure{std::move(error)};
        }
        return *found;
    });
}
Result<std::shared_ptr<const PreparedMeshAsset>>
PreparedAssets::resolve(const MeshRenderer& renderer) const {
    return boundary<std::shared_ptr<const PreparedMeshAsset>>("prepared.assets.resolve", [&] {
        const auto asset = take(find(renderer.asset));
        if (renderer.mesh_index >= asset->meshes.size()) {
            Error error{
                ErrorCode::invalid_argument, "Mesh index is outside its prepared asset", {}};
            error.context.asset_id = renderer.asset.value().to_string();
            throw Failure{std::move(error)};
        }
        return asset;
    });
}
std::size_t PreparedAssets::size() const noexcept {
    return assets_.size();
}
std::size_t PreparedAssets::retained_bytes() const noexcept {
    return retained_bytes_;
}

} // namespace shark
