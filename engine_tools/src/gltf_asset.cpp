#include "shark/tools/gltf_asset.hpp"

#include "asset_cache_format.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <type_traits>

namespace shark::tools {
namespace {
using Bytes = std::span<const std::byte>;
using Json = nlohmann::json;
constexpr std::size_t header_bytes = 44;
constexpr std::uint32_t absent_index = std::numeric_limits<std::uint32_t>::max();
struct Failure {
    Error error;
};
[[noreturn]] void fail(const char* message, ErrorCode code = ErrorCode::invalid_configuration) {
    throw Failure{Error{code, message, {}}};
}
template <typename T> T take(Result<T> result) {
    if (!result)
        throw Failure{std::move(result).error()};
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result)
        throw Failure{std::move(result).error()};
}
template <typename T, typename F> Result<T> boundary(const char* operation, F&& function) {
    try {
        if constexpr (std::is_void_v<T>) {
            function();
            return {};
        } else
            return function();
    } catch (Failure& failure) {
        failure.error.context.operation = operation;
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed, "glTF asset operation failed", {operation}};
    }
}
void poll(std::stop_token stop) {
    if (stop.stop_requested())
        fail("glTF asset operation cancelled", ErrorCode::execution_failed);
}
Bytes bytes(std::string_view value) {
    return std::as_bytes(std::span{value.data(), value.size()});
}
bool text_valid(std::string_view value) {
    // JSON's strict encoder validates UTF-8 without imposing Name's smaller
    // display-text restrictions on source-authored glTF names/diagnostics.
    try {
        static_cast<void>(Json(value).dump());
        return true;
    } catch (...) {
        return false;
    }
}
struct Writer {
    std::vector<std::byte> output;
    std::stop_token stop;
    void raw(Bytes value) {
        poll(stop);
        if (value.size() > gltf_asset_maximum_payload_bytes - output.size())
            fail("glTF payload exceeds the 2 MiB artifact bound");
        output.insert(output.end(), value.begin(), value.end());
    }
    void integer(std::uint64_t value, unsigned width) {
        std::array<std::byte, 8> data{};
        for (unsigned i = 0; i < width; ++i)
            data[i] = static_cast<std::byte>((value >> (8U * i)) & 255U);
        raw(Bytes{data}.first(width));
    }
    template <typename T> void u32(T value) {
        if (value > std::numeric_limits<std::uint32_t>::max())
            fail("glTF payload integer exceeds its representation");
        integer(static_cast<std::uint64_t>(value), 4);
    }
    void flag(bool value) {
        integer(value ? 1 : 0, 1);
    }
    template <typename E> void enumeration(E value, unsigned maximum) {
        const auto number = static_cast<unsigned>(value);
        if (number > maximum)
            fail("Invalid glTF payload enum");
        u32(number);
    }
    void number(float value) {
        if (!std::isfinite(value))
            fail("Nonfinite glTF payload value");
        integer(std::bit_cast<std::uint32_t>(value), 4);
    }
    void number(double value) {
        if (!std::isfinite(value))
            fail("Nonfinite glTF payload value");
        integer(std::bit_cast<std::uint64_t>(value), 8);
    }
    void optional_index(const std::optional<std::size_t>& value) {
        if (value && *value >= absent_index)
            fail("Invalid glTF payload optional index");
        u32(value.value_or(absent_index));
    }
    void text(const std::string& value, std::size_t maximum = 4096) {
        if (value.size() > maximum || !text_valid(value))
            fail("Invalid glTF payload string");
        u32(value.size());
        raw(bytes(value));
    }
    void blob(const std::vector<std::byte>& value) {
        u32(value.size());
        raw(value);
    }
    void path(const std::optional<ProjectPath>& value) {
        flag(value.has_value());
        if (value)
            text(value->text());
    }
    void identity(const std::optional<AssetId>& value) {
        flag(value.has_value());
        if (value)
            text(value->value().to_string(), 36);
    }
    void digest(const ContentDigest& value) {
        raw(value.bytes);
    }
    template <typename T, typename F>
    void items(const std::vector<T>& values, std::size_t maximum, std::size_t, F&& encode) {
        if (values.size() > maximum)
            fail("glTF payload record count exceeds its bound");
        u32(values.size());
        for (const auto& value : values)
            encode(value);
    }
};
struct Reader {
    Bytes input;
    std::stop_token stop;
    std::size_t position{};
    std::size_t allocated{};
    void charge(std::size_t count, std::size_t size = 1) {
        if (size != 0 && count > (gltf_asset_maximum_decoded_bytes - allocated) / size)
            fail("glTF decoded payload exceeds its allocation bound");
        allocated += count * size;
    }
    Bytes raw(std::size_t count) {
        poll(stop);
        if (count > input.size() - position)
            fail("Truncated glTF payload");
        const auto result = input.subspan(position, count);
        position += count;
        return result;
    }
    std::uint64_t integer(unsigned width) {
        const auto data = raw(width);
        std::uint64_t result = 0;
        for (unsigned i = 0; i < width; ++i)
            result |= static_cast<std::uint64_t>(std::to_integer<unsigned>(data[i])) << (8U * i);
        return result;
    }
    template <typename T> void u32(T& value) {
        value = static_cast<T>(integer(4));
    }
    void flag(bool& value) {
        const auto number = integer(1);
        if (number > 1)
            fail("Invalid glTF payload boolean");
        value = number != 0;
    }
    template <typename E> void enumeration(E& value, unsigned maximum) {
        const auto number = integer(4);
        if (number > maximum)
            fail("Invalid glTF payload enum");
        value = static_cast<E>(number);
    }
    void number(float& value) {
        value = std::bit_cast<float>(static_cast<std::uint32_t>(integer(4)));
        if (!std::isfinite(value))
            fail("Nonfinite glTF payload value");
    }
    void number(double& value) {
        value = std::bit_cast<double>(integer(8));
        if (!std::isfinite(value))
            fail("Nonfinite glTF payload value");
    }
    void optional_index(std::optional<std::size_t>& value) {
        const auto number = integer(4);
        value = number == absent_index
                    ? std::nullopt
                    : std::optional<std::size_t>{static_cast<std::size_t>(number)};
    }
    void text(std::string& value, std::size_t maximum = 4096) {
        const auto size = static_cast<std::size_t>(integer(4));
        if (size > maximum)
            fail("glTF payload string exceeds its bound");
        const auto data = raw(size);
        charge(size);
        value.assign(reinterpret_cast<const char*>(data.data()), data.size());
        if (!text_valid(value))
            fail("Invalid UTF-8 glTF payload string");
    }
    void blob(std::vector<std::byte>& value) {
        const auto size = static_cast<std::size_t>(integer(4));
        const auto data = raw(size);
        charge(size);
        value.assign(data.begin(), data.end());
    }
    void path(std::optional<ProjectPath>& value) {
        bool present{};
        flag(present);
        if (present) {
            std::string text_value;
            text(text_value);
            value = take(ProjectPath::parse(text_value));
        }
    }
    void identity(std::optional<AssetId>& value) {
        bool present{};
        flag(present);
        if (present) {
            std::string text_value;
            text(text_value, 36);
            value = AssetId{take(StableId::parse(text_value))};
        }
    }
    void digest(ContentDigest& value) {
        const auto data = raw(value.bytes.size());
        std::copy(data.begin(), data.end(), value.bytes.begin());
    }
    template <typename T, typename F>
    void items(std::vector<T>& values, std::size_t maximum, std::size_t minimum_wire, F&& decode) {
        const auto count = static_cast<std::size_t>(integer(4));
        if (count > maximum || count > (input.size() - position) / minimum_wire)
            fail("glTF payload count exceeds its bound or remaining bytes");
        charge(count, sizeof(T));
        values.resize(count);
        for (auto& value : values)
            decode(value);
    }
};
template <typename A, typename T> void numbers(A& archive, T& values) {
    for (auto& value : values)
        archive.number(value);
}
template <typename A, typename T> void vector(A& archive, T& value) {
    archive.number(value.x);
    archive.number(value.y);
    archive.number(value.z);
}
template <typename A, typename T> void matrix(A& archive, T& value) {
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            archive.number(value[column][row]);
}
template <typename A, typename T> void bounds(A& archive, T& value) {
    vector(archive, value.minimum);
    vector(archive, value.maximum);
}
template <typename A, typename T> void optional_bounds(A& archive, T& value) {
    bool present = value.has_value();
    archive.flag(present);
    if (present) {
        if constexpr (std::is_same_v<A, Reader>)
            value.emplace();
        bounds(archive, *value);
    }
}
template <typename A, typename T> void source_report(A& archive, T& source) {
    archive.enumeration(source.container, 1);
    auto& counts = source.counts;
    archive.u32(counts.scenes);
    archive.u32(counts.nodes);
    archive.u32(counts.meshes);
    archive.u32(counts.primitives);
    archive.u32(counts.accessors);
    archive.u32(counts.materials);
    archive.u32(counts.textures);
    archive.u32(counts.samplers);
    archive.optional_index(source.selected_scene);
    archive.items(source.resources, gltf_source_maximum_resources, 54, [&](auto& value) {
        archive.enumeration(value.kind, 1);
        archive.u32(value.index);
        archive.enumeration(value.storage, 3);
        archive.path(value.path);
        archive.identity(value.asset);
        archive.u32(value.byte_count);
        archive.digest(value.digest);
        archive.text(value.mime_type, 128);
    });
    archive.items(source.diagnostics, gltf_source_maximum_diagnostics, 4,
                  [&](auto& value) { archive.text(value, gltf_source_maximum_diagnostic_bytes); });
}
template <typename A, typename T> void asset_body(A& archive, T& asset) {
    auto& geometry = asset.geometry;
    source_report(archive, geometry.source);
    archive.optional_index(geometry.selected_scene);
    archive.items(geometry.primitives, gltf_source_maximum_total_entries, 78, [&](auto& p) {
        archive.u32(p.source_mesh);
        archive.u32(p.source_primitive);
        archive.optional_index(p.source_material);
        archive.items(p.vertices, gltf_geometry_maximum_vertices, 72, [&](auto& vertex) {
            numbers(archive, vertex.position);
            numbers(archive, vertex.normal);
            numbers(archive, vertex.tangent);
            numbers(archive, vertex.texcoord0);
            numbers(archive, vertex.texcoord1);
            numbers(archive, vertex.color);
        });
        archive.items(p.indices, gltf_geometry_maximum_indices, 4,
                      [&](auto& value) { archive.u32(value); });
        bounds(archive, p.bounds);
        archive.flag(p.has_texcoord0);
        archive.flag(p.has_texcoord1);
        archive.flag(p.has_color);
        archive.flag(p.has_tangents);
        archive.flag(p.generated_normals);
        archive.flag(p.generated_tangents);
    });
    archive.items(geometry.meshes, gltf_source_maximum_table_entries, 60, [&](auto& mesh) {
        archive.u32(mesh.source_mesh);
        archive.text(mesh.name);
        archive.items(mesh.primitives, gltf_source_maximum_total_entries, 4,
                      [&](auto& value) { archive.u32(value); });
        bounds(archive, mesh.bounds);
    });
    archive.items(geometry.nodes, gltf_source_maximum_table_entries, 426, [&](auto& node) {
        archive.u32(node.source_node);
        archive.text(node.name);
        archive.optional_index(node.parent);
        archive.items(node.children, gltf_source_maximum_table_entries, 4,
                      [&](auto& value) { archive.u32(value); });
        archive.optional_index(node.mesh);
        vector(archive, node.local.translation);
        archive.number(node.local.rotation.x);
        archive.number(node.local.rotation.y);
        archive.number(node.local.rotation.z);
        archive.number(node.local.rotation.w);
        vector(archive, node.local.scale);
        matrix(archive, node.local_matrix);
        matrix(archive, node.world_matrix);
        numbers(archive, node.normal_matrix);
        archive.flag(node.mirrored);
        optional_bounds(archive, node.world_bounds);
    });
    archive.items(geometry.roots, gltf_source_maximum_table_entries, 4,
                  [&](auto& value) { archive.u32(value); });
    optional_bounds(archive, geometry.world_bounds);
    auto& textures = asset.textures;
    archive.optional_index(textures.selected_scene);
    archive.items(textures.images, gltf_texture_maximum_variants, 16, [&](auto& image) {
        archive.u32(image.source_image);
        archive.enumeration(image.semantic, 2);
        archive.u32(image.component_bits);
        archive.items(image.mips, 13, 12, [&](auto& mip) {
            archive.u32(mip.width);
            archive.u32(mip.height);
            archive.blob(mip.rgba);
        });
    });
    archive.items(textures.usages, gltf_texture_maximum_usages, 40, [&](auto& usage) {
        archive.u32(usage.source_material);
        archive.enumeration(usage.role, 4);
        archive.u32(usage.source_texture);
        archive.u32(usage.texcoord);
        archive.u32(usage.image);
        archive.enumeration(usage.sampler.magnification, 1);
        archive.enumeration(usage.sampler.minification, 1);
        archive.enumeration(usage.sampler.mip, 2);
        archive.enumeration(usage.sampler.wrap_s, 2);
        archive.enumeration(usage.sampler.wrap_t, 2);
    });
    archive.optional_index(asset.materials.selected_scene);
    archive.items(asset.materials.materials, gltf_source_maximum_table_entries, 66, [&](auto& m) {
        archive.u32(m.source_material);
        archive.text(m.name);
        numbers(archive, m.base_color);
        archive.number(m.metallic);
        archive.number(m.roughness);
        numbers(archive, m.emissive);
        archive.number(m.normal_scale);
        archive.number(m.occlusion_strength);
        archive.enumeration(m.alpha_mode, 2);
        archive.number(m.alpha_cutoff);
        archive.flag(m.double_sided);
        for (auto& link : m.textures) {
            bool present = link.has_value();
            archive.flag(present);
            if (present) {
                if constexpr (std::is_same_v<A, Reader>)
                    link.emplace();
                archive.u32(link->source_texture);
                archive.u32(link->texcoord);
            }
        }
    });
}

void require(bool condition, const char* message) {
    if (!condition)
        fail(message);
}
bool unit_interval(float value) {
    return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}
void include(std::optional<GltfGeometryBounds>& output, const math::Vec3& point) {
    require(std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z),
            "Nonfinite glTF payload bounds");
    if (!output)
        output = GltfGeometryBounds{point, point};
    else {
        output->minimum = glm::min(output->minimum, point);
        output->maximum = glm::max(output->maximum, point);
    }
}
void include(std::optional<GltfGeometryBounds>& output, const GltfGeometryBounds& value) {
    include(output, value.minimum);
    include(output, value.maximum);
}
bool close(double left, double right) {
    return std::isfinite(left) && std::isfinite(right) &&
           std::abs(left - right) <= 1e-9 + 1e-9 * std::max(std::abs(left), std::abs(right));
}
bool same_bounds(const std::optional<GltfGeometryBounds>& a,
                 const std::optional<GltfGeometryBounds>& b) {
    if (a.has_value() != b.has_value())
        return false;
    if (!a)
        return true;
    for (int i = 0; i < 3; ++i)
        if (!close(a->minimum[i], b->minimum[i]) || !close(a->maximum[i], b->maximum[i]))
            return false;
    return true;
}
GltfGeometryBounds transformed(const GltfGeometryBounds& bounds_value,
                               const math::Mat4& transform) {
    std::optional<GltfGeometryBounds> result;
    for (unsigned corner = 0; corner < 8; ++corner) {
        const math::Vec3 point{(corner & 1U) ? bounds_value.maximum.x : bounds_value.minimum.x,
                               (corner & 2U) ? bounds_value.maximum.y : bounds_value.minimum.y,
                               (corner & 4U) ? bounds_value.maximum.z : bounds_value.minimum.z};
        include(result, math::Vec3{transform * glm::dvec4{point, 1.0}});
    }
    return *result;
}
void validate_asset(const GltfAsset& asset, std::stop_token stop) {
    const auto& geometry = asset.geometry;
    const auto& source = geometry.source;
    const auto& counts = source.counts;
    for (const auto count : {counts.scenes, counts.nodes, counts.meshes, counts.accessors,
                             counts.materials, counts.textures, counts.samplers})
        require(count <= gltf_source_maximum_table_entries, "Invalid glTF source counts");
    require(counts.primitives <= gltf_source_maximum_total_entries,
            "Invalid glTF source primitive count");
    require(!source.selected_scene || *source.selected_scene < counts.scenes,
            "Invalid glTF source default scene");
    require(!geometry.selected_scene || *geometry.selected_scene < counts.scenes,
            "Invalid glTF selected scene");
    require(asset.textures.selected_scene == geometry.selected_scene &&
                asset.materials.selected_scene == geometry.selected_scene,
            "glTF payload views disagree on scene selection");
    require(geometry.primitives.size() <= counts.primitives &&
                geometry.meshes.size() <= counts.meshes && geometry.nodes.size() <= counts.nodes &&
                asset.materials.materials.size() <= counts.materials &&
                asset.textures.images.size() <= gltf_texture_maximum_variants &&
                asset.textures.usages.size() <= gltf_texture_maximum_usages,
            "glTF payload output counts exceed source or conversion bounds");
    require(geometry.selected_scene ||
                (geometry.nodes.empty() && geometry.meshes.empty() && geometry.primitives.empty() &&
                 asset.textures.images.empty() && asset.textures.usages.empty() &&
                 asset.materials.materials.empty()),
            "glTF payload without a scene contains converted records");
    require(source.resources.size() <= gltf_source_maximum_resources &&
                source.diagnostics.size() <= gltf_source_maximum_diagnostics,
            "glTF payload report exceeds its bounds");
    std::set<std::pair<GltfResourceKind, std::size_t>> resources;
    std::map<AssetId, std::tuple<std::string, ContentDigest, std::size_t>> external;
    std::set<std::size_t> source_images;
    std::size_t source_bytes = 0;
    for (const auto& resource : source.resources) {
        require(resource.index < gltf_source_maximum_table_entries &&
                    resources.emplace(resource.kind, resource.index).second &&
                    resource.byte_count <= gltf_source_maximum_file_bytes &&
                    resource.byte_count <= gltf_source_maximum_total_bytes - source_bytes,
                "Invalid glTF resource report");
        source_bytes += resource.byte_count;
        const bool is_external = resource.storage == GltfResourceStorage::external;
        require(resource.path.has_value() == is_external &&
                    resource.asset.has_value() == is_external,
                "Invalid glTF external resource identity");
        if (resource.asset) {
            auto identity = std::tuple{resource.path->text(), resource.digest, resource.byte_count};
            const auto [it, inserted] = external.emplace(*resource.asset, identity);
            require(inserted || it->second == identity,
                    "Inconsistent glTF external source identity");
        }
        if (resource.kind == GltfResourceKind::image) {
            require(resource.mime_type == "image/png" || resource.mime_type == "image/jpeg",
                    "Invalid decoded-image source MIME");
            source_images.insert(resource.index);
        }
    }
    std::map<std::size_t, const GltfMaterial*> materials;
    std::optional<std::size_t> previous_material;
    for (const auto& m : asset.materials.materials) {
        poll(stop);
        require(m.source_material < counts.materials &&
                    (!previous_material || *previous_material < m.source_material),
                "Unordered or invalid glTF material identity");
        previous_material = m.source_material;
        materials.emplace(m.source_material, &m);
        require(std::all_of(m.base_color.begin(), m.base_color.end(), unit_interval) &&
                    std::all_of(m.emissive.begin(), m.emissive.end(), unit_interval) &&
                    unit_interval(m.metallic) && unit_interval(m.roughness) &&
                    unit_interval(m.occlusion_strength) && std::isfinite(m.normal_scale) &&
                    std::isfinite(m.alpha_cutoff) && m.alpha_cutoff >= 0.0F,
                "Invalid glTF material factors");
        for (const auto& link : m.textures)
            require(!link || (link->source_texture < counts.textures && link->texcoord <= 1),
                    "Invalid glTF material texture reference");
    }
    std::set<std::size_t> used_materials;
    std::size_t total_vertices = 0;
    std::size_t total_indices = 0;
    for (const auto& primitive : geometry.primitives) {
        poll(stop);
        require(!primitive.vertices.empty() && !primitive.indices.empty() &&
                    primitive.indices.size() % 3 == 0 &&
                    primitive.vertices.size() <= gltf_geometry_maximum_vertices - total_vertices &&
                    primitive.indices.size() <= gltf_geometry_maximum_indices - total_indices &&
                    primitive.source_mesh < counts.meshes &&
                    primitive.source_primitive < gltf_source_maximum_table_entries,
                "Invalid glTF primitive inventory");
        total_vertices += primitive.vertices.size();
        total_indices += primitive.indices.size();
        require(!primitive.generated_tangents || primitive.has_tangents,
                "Generated glTF tangents are absent");
        if (primitive.source_material) {
            const auto found = materials.find(*primitive.source_material);
            require(found != materials.end(), "Missing glTF primitive material");
            used_materials.insert(*primitive.source_material);
            for (std::size_t role = 0; role < found->second->textures.size(); ++role) {
                const auto& link = found->second->textures[role];
                require(!link || (link->texcoord == 0 ? primitive.has_texcoord0
                                                      : primitive.has_texcoord1),
                        "glTF material references a missing primitive UV set");
                require(!link || role != static_cast<std::size_t>(GltfTextureRole::normal) ||
                            primitive.has_tangents,
                        "glTF normal texture has no tangent basis");
            }
        }
        for (const auto& vertex : primitive.vertices) {
            const auto normal_length =
                std::hypot(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
            require(std::abs(normal_length - 1.0F) <= 1e-4F &&
                        std::all_of(vertex.color.begin(), vertex.color.end(), unit_interval),
                    "Invalid glTF vertex normal or color");
            if (primitive.has_tangents) {
                const auto length =
                    std::hypot(vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]);
                const auto dot = vertex.normal[0] * vertex.tangent[0] +
                                 vertex.normal[1] * vertex.tangent[1] +
                                 vertex.normal[2] * vertex.tangent[2];
                require(std::abs(length - 1.0F) <= 1e-4F && std::abs(dot) <= 1e-4F &&
                            (vertex.tangent[3] == -1.0F || vertex.tangent[3] == 1.0F),
                        "Invalid glTF vertex tangent basis");
            }
        }
        std::optional<GltfGeometryBounds> expected;
        for (const auto index : primitive.indices) {
            require(index < primitive.vertices.size(), "Invalid glTF vertex index");
            const auto& value = primitive.vertices[index].position;
            include(expected, math::Vec3{value[0], value[1], value[2]});
        }
        require(same_bounds(expected, primitive.bounds), "Inconsistent glTF primitive bounds");
    }
    require(used_materials.size() == materials.size(), "Unreferenced glTF material record");
    std::vector<bool> primitive_seen(geometry.primitives.size());
    std::optional<std::size_t> previous_mesh;
    for (const auto& mesh : geometry.meshes) {
        require(!mesh.primitives.empty() && mesh.source_mesh < counts.meshes &&
                    (!previous_mesh || *previous_mesh < mesh.source_mesh),
                "Invalid glTF mesh inventory");
        previous_mesh = mesh.source_mesh;
        std::optional<GltfGeometryBounds> expected;
        for (std::size_t i = 0; i < mesh.primitives.size(); ++i) {
            const auto index = mesh.primitives[i];
            require(index < geometry.primitives.size() && !primitive_seen[index],
                    "Invalid or repeated glTF mesh primitive");
            primitive_seen[index] = true;
            const auto& primitive = geometry.primitives[index];
            require(primitive.source_mesh == mesh.source_mesh && primitive.source_primitive == i,
                    "Inconsistent glTF primitive source correspondence");
            include(expected, primitive.bounds);
        }
        require(same_bounds(expected, mesh.bounds), "Inconsistent glTF mesh bounds");
    }
    require(
        std::all_of(primitive_seen.begin(), primitive_seen.end(), [](bool seen) { return seen; }),
        "Unreferenced glTF primitive");
    std::set<std::size_t> node_sources;
    std::vector<bool> mesh_seen(geometry.meshes.size());
    std::vector<bool> node_seen(geometry.nodes.size());
    std::optional<GltfGeometryBounds> world_bounds;
    for (const auto root : geometry.roots) {
        require(root < geometry.nodes.size() && !node_seen[root] && !geometry.nodes[root].parent,
                "Invalid glTF root node");
        node_seen[root] = true;
    }
    for (std::size_t i = 0; i < geometry.nodes.size(); ++i) {
        poll(stop);
        const auto& node = geometry.nodes[i];
        require(node.source_node < counts.nodes && node_sources.insert(node.source_node).second &&
                    (!node.parent || *node.parent < i),
                "Invalid glTF node identity or parent ordering");
        for (const auto child : node.children) {
            require(child > i && child < geometry.nodes.size() && !node_seen[child] &&
                        geometry.nodes[child].parent == i,
                    "Invalid glTF child relationship");
            node_seen[child] = true;
        }
        const auto local = take(transform_matrix(node.local));
        const auto world = node.parent ? geometry.nodes[*node.parent].world_matrix * local : local;
        require(transform_matrices_equivalent(local, node.local_matrix) &&
                    transform_matrices_equivalent(world, node.world_matrix),
                "Inconsistent glTF hierarchy matrices");
        const auto inverse = take(inverse_transform_matrix(world));
        glm::dmat3 normalized{1.0};
        for (int column = 0; column < 3; ++column) {
            const auto basis = math::Vec3{world[column]};
            normalized[column] = basis / std::hypot(basis.x, basis.y, basis.z);
            for (int row = 0; row < 3; ++row)
                require(close(inverse[row][column],
                              node.normal_matrix[static_cast<std::size_t>(column * 3 + row)]),
                        "Inconsistent glTF normal matrix");
        }
        require(node.mirrored == (glm::determinant(normalized) < 0.0),
                "Inconsistent glTF instance mirror flag");
        std::optional<GltfGeometryBounds> expected;
        if (node.mesh) {
            require(*node.mesh < geometry.meshes.size(), "Invalid glTF node mesh");
            mesh_seen[*node.mesh] = true;
            expected = transformed(geometry.meshes[*node.mesh].bounds, world);
            include(world_bounds, *expected);
        }
        require(same_bounds(expected, node.world_bounds), "Inconsistent glTF node world bounds");
    }
    require(std::all_of(node_seen.begin(), node_seen.end(), [](bool seen) { return seen; }) &&
                std::all_of(mesh_seen.begin(), mesh_seen.end(), [](bool seen) { return seen; }) &&
                same_bounds(world_bounds, geometry.world_bounds),
            "Unreferenced glTF hierarchy records or inconsistent world bounds");
    std::set<std::pair<std::size_t, GltfTextureSemantic>> variants;
    std::map<std::size_t, std::tuple<std::uint32_t, std::uint32_t, unsigned>> image_formats;
    for (const auto& image : asset.textures.images) {
        poll(stop);
        require(source_images.contains(image.source_image) &&
                    variants.emplace(image.source_image, image.semantic).second &&
                    (image.component_bits == 8 || image.component_bits == 16) &&
                    !image.mips.empty() && image.mips.size() <= 13,
                "Invalid glTF image variant");
        auto width = image.mips.front().width;
        auto height = image.mips.front().height;
        const auto format = std::tuple{width, height, image.component_bits};
        const auto [format_it, inserted] = image_formats.emplace(image.source_image, format);
        require(inserted || format_it->second == format,
                "glTF semantic variants disagree on source image layout");
        require(width > 0 && height > 0 && width <= gltf_texture_maximum_dimension &&
                    height <= gltf_texture_maximum_dimension &&
                    static_cast<std::size_t>(width) * height <= gltf_texture_maximum_pixels,
                "Invalid glTF image dimensions");
        for (std::size_t i = 0; i < image.mips.size(); ++i) {
            const auto& mip = image.mips[i];
            require(mip.width == width && mip.height == height &&
                        mip.rgba.size() == static_cast<std::size_t>(width) * height * 4 *
                                               (image.component_bits / 8),
                    "Invalid glTF mip dimensions or storage");
            if (width == 1 && height == 1)
                require(i + 1 == image.mips.size(), "Extra glTF mip levels");
            else
                require(i + 1 < image.mips.size(), "Incomplete glTF mip chain");
            width = std::max(1U, width / 2);
            height = std::max(1U, height / 2);
        }
    }
    std::vector<bool> image_seen(asset.textures.images.size());
    std::set<std::pair<std::size_t, GltfTextureRole>> usages;
    std::optional<std::pair<std::size_t, GltfTextureRole>> previous_usage;
    for (const auto& usage : asset.textures.usages) {
        const auto key = std::pair{usage.source_material, usage.role};
        const auto material = materials.find(usage.source_material);
        require(material != materials.end() && usage.image < asset.textures.images.size() &&
                    usage.texcoord <= 1 && usage.source_texture < counts.textures &&
                    static_cast<unsigned>(usage.role) < 5 && usages.insert(key).second &&
                    (!previous_usage || *previous_usage < key),
                "Invalid or unordered glTF texture usage");
        previous_usage = key;
        image_seen[usage.image] = true;
        const auto& link = material->second->textures[static_cast<std::size_t>(usage.role)];
        require(link && link->source_texture == usage.source_texture &&
                    link->texcoord == usage.texcoord,
                "glTF material/texture links disagree");
        const auto semantic =
            usage.role == GltfTextureRole::normal ? GltfTextureSemantic::normal
            : (usage.role == GltfTextureRole::base_color || usage.role == GltfTextureRole::emissive)
                ? GltfTextureSemantic::srgb_color
                : GltfTextureSemantic::linear_data;
        require(asset.textures.images[usage.image].semantic == semantic,
                "glTF texture semantic disagrees with its material role");
    }
    for (const auto& [id, material] : materials)
        for (std::size_t role = 0; role < material->textures.size(); ++role)
            require(material->textures[role].has_value() ==
                        usages.contains({id, static_cast<GltfTextureRole>(role)}),
                    "Missing glTF material texture usage");
    require(std::all_of(image_seen.begin(), image_seen.end(), [](bool seen) { return seen; }),
            "Unreferenced glTF image variant");
    poll(stop);
}

struct Settings {
    ProjectPath path;
    std::optional<std::size_t> scene;
    std::vector<std::pair<ProjectPath, AssetId>> resources;
};
Settings settings(std::string_view text) {
    const auto canonical = take(detail::canonical_import_object(text));
    const auto object = Json::parse(canonical);
    require(object.contains("path") && object.at("path").is_string(),
            "glTF settings require a logical path");
    for (const auto& [key, value] : object.items()) {
        static_cast<void>(value);
        require(key == "path" || key == "scene" || key == "resources", "Unknown glTF setting");
    }
    Settings output{take(ProjectPath::parse(object.at("path").get<std::string>())), {}, {}};
    require(output.path.text().ends_with(".gltf") || output.path.text().ends_with(".glb"),
            "glTF logical path requires .gltf or .glb extension");
    if (object.contains("scene")) {
        const auto& value = object.at("scene");
        require(value.is_number_unsigned() ||
                    (value.is_number_integer() && value.get<std::int64_t>() >= 0),
                "glTF scene setting must be a nonnegative integer");
        const auto number = value.get<std::uint64_t>();
        require(number < gltf_source_maximum_table_entries, "glTF scene setting exceeds its bound");
        output.scene = static_cast<std::size_t>(number);
    }
    if (object.contains("resources")) {
        const auto& resources = object.at("resources");
        require(resources.is_array() && resources.size() <= gltf_source_maximum_resources,
                "Invalid glTF resource settings");
        std::set<AssetId> ids;
        std::set<std::string> paths;
        for (const auto& value : resources) {
            require(value.is_object() && value.size() == 2 && value.contains("path") &&
                        value.contains("asset") && value.at("path").is_string() &&
                        value.at("asset").is_string(),
                    "glTF resource requires exactly path and asset strings");
            auto path = take(ProjectPath::parse(value.at("path").get<std::string>()));
            const auto id = AssetId{take(StableId::parse(value.at("asset").get<std::string>()))};
            auto folded = path.text();
            for (auto& c : folded)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            require(paths.insert(folded).second && ids.insert(id).second,
                    "Duplicate glTF resource alias or identity");
            output.resources.emplace_back(std::move(path), id);
        }
        std::sort(output.resources.begin(), output.resources.end(),
                  [](const auto& left, const auto& right) {
                      return left.first.text() < right.first.text();
                  });
    }
    return output;
}
std::string normalized(const Settings& value) {
    Json object{{"path", value.path.text()}, {"resources", Json::array()}};
    if (value.scene)
        object["scene"] = *value.scene;
    for (const auto& [path, id] : value.resources)
        object["resources"].push_back({{"path", path.text()}, {"asset", id.value().to_string()}});
    auto result = object.dump();
    require(result.size() <= 4096, "Normalized glTF settings exceed 4096 bytes");
    return result;
}
Bytes resource_body(Bytes payload, AssetId expected, const ContentDigest& digest) {
    require(payload.size() >= 84 && payload.size() <= asset_import_maximum_source_bytes + 84,
            "Invalid glTF resource payload length");
    Reader reader{payload, {}};
    require(std::ranges::equal(reader.raw(8), bytes("SHKRES01")) && reader.integer(4) == 1,
            "Invalid glTF resource payload version");
    require(std::ranges::equal(reader.raw(36), bytes(expected.value().to_string())),
            "glTF resource payload identity mismatch");
    ContentDigest stored;
    reader.digest(stored);
    const auto size = static_cast<std::size_t>(reader.integer(4));
    const auto body = reader.raw(size);
    require(reader.position == payload.size() && stored == digest && take(sha256(body)) == digest,
            "glTF identity resource digest or length mismatch");
    return body;
}
void descriptor(const AssetArtifact& artifact, const AssetImporterInfo& info) {
    require(artifact.kind == info.kind && artifact.importer == info.id &&
                artifact.importer_version == info.version &&
                artifact.payload_version == info.payload_version,
            "Invalid glTF artifact descriptor");
    take(detail::validate_target(artifact.target));
}
} // namespace

Result<GltfAsset> convert_gltf_asset(const GltfSourceInput& input,
                                     const GltfGeometrySettings& selection) {
    return boundary<GltfAsset>("asset.gltf.asset.convert", [&] {
        poll(input.stop);
        GltfAsset asset{take(convert_gltf_geometry(input, selection)),
                        take(convert_gltf_textures(input, {selection.scene})),
                        take(convert_gltf_materials(input, {selection.scene}))};
        validate_asset(asset, input.stop);
        return asset;
    });
}
Result<std::vector<std::byte>> encode_gltf_asset(const GltfAsset& asset, std::stop_token stop) {
    return boundary<std::vector<std::byte>>("asset.gltf.asset.encode", [&] {
        poll(stop);
        validate_asset(asset, stop);
        Writer writer{{}, stop};
        writer.raw(bytes("SHKGLT01"));
        writer.u32(gltf_asset_payload_version);
        writer.raw(ContentDigest{}.bytes);
        asset_body(writer, asset);
        const auto digest = take(sha256(Bytes{writer.output}.subspan(header_bytes)));
        std::copy(digest.bytes.begin(), digest.bytes.end(), writer.output.begin() + 12);
        return std::move(writer.output);
    });
}
Result<GltfAsset> decode_gltf_asset(Bytes payload, std::stop_token stop) {
    return boundary<GltfAsset>("asset.gltf.asset.decode", [&] {
        poll(stop);
        require(payload.size() >= header_bytes &&
                    payload.size() <= gltf_asset_maximum_payload_bytes,
                "Invalid glTF payload length");
        Reader reader{payload, stop};
        require(std::ranges::equal(reader.raw(8), bytes("SHKGLT01")) &&
                    reader.integer(4) == gltf_asset_payload_version,
                "Unsupported glTF asset payload version");
        ContentDigest stored;
        reader.digest(stored);
        require(stored == take(sha256(payload.subspan(header_bytes))),
                "glTF payload digest mismatch");
        GltfAsset asset{};
        asset_body(reader, asset);
        require(reader.position == payload.size(), "Unreferenced trailing glTF payload bytes");
        validate_asset(asset, stop);
        // Source reports are bounded; charge both retained copies before making
        // them. Capacity of transient validator containers is separate.
        std::size_t report_bytes =
            asset.geometry.source.resources.size() * sizeof(GltfResourceReference);
        for (const auto& resource : asset.geometry.source.resources)
            report_bytes +=
                resource.mime_type.size() + (resource.path ? resource.path->text().size() : 0);
        report_bytes += asset.geometry.source.diagnostics.size() * sizeof(std::string);
        for (const auto& diagnostic : asset.geometry.source.diagnostics)
            report_bytes += diagnostic.size();
        reader.charge(report_bytes, 2);
        asset.textures.source = asset.geometry.source;
        asset.materials.source = asset.geometry.source;
        return asset;
    });
}
const AssetImporterInfo& GltfResourceImporter::info() const noexcept {
    static const AssetImporterInfo value{"shark.gltf-resource", 1, "gltf-resource", 1,
                                         R"({"type":"object","additionalProperties":false})"};
    return value;
}
Result<std::string> GltfResourceImporter::normalize_settings(std::string_view json) const {
    return boundary<std::string>("asset.gltf.resource.settings", [&] {
        const auto value = take(detail::canonical_import_object(json));
        require(value == "{}", "glTF resource settings must be empty");
        return value;
    });
}
Result<std::vector<std::byte>> GltfResourceImporter::import(const AssetImportInput& input) {
    return boundary<std::vector<std::byte>>("asset.gltf.resource.import", [&] {
        poll(input.stop);
        require(input.kind == info().kind &&
                    input.source.size() <= asset_import_maximum_source_bytes &&
                    input.dependencies.empty(),
                "Invalid glTF resource input");
        take(detail::validate_target(input.target));
        static_cast<void>(take(normalize_settings(input.settings_json)));
        Writer writer{{}, input.stop};
        writer.raw(bytes("SHKRES01"));
        writer.u32(1U);
        writer.raw(bytes(input.asset.value().to_string()));
        writer.digest(take(sha256(input.source)));
        writer.u32(input.source.size());
        writer.raw(input.source);
        return std::move(writer.output);
    });
}
Result<void> GltfResourceImporter::validate_payload(const AssetArtifact& artifact) const {
    return boundary<void>("asset.gltf.resource.validate", [&] {
        descriptor(artifact, info());
        require(artifact.dependencies.empty() &&
                    artifact.settings_digest == take(sha256(bytes("{}"))),
                "Invalid glTF resource settings or dependencies");
        static_cast<void>(resource_body(artifact.payload, artifact.asset, artifact.source_digest));
    });
}
const AssetImporterInfo& GltfAssetImporter::info() const noexcept {
    static const AssetImporterInfo value{
        "shark.gltf", 1, "gltf", gltf_asset_payload_version,
        R"({"type":"object","required":["path"],"properties":{"path":{"type":"string"},"scene":{"type":"integer","minimum":0},"resources":{"type":"array","items":{"type":"object","required":["path","asset"],"properties":{"path":{"type":"string"},"asset":{"type":"string"}},"additionalProperties":false}}},"additionalProperties":false})"};
    return value;
}
Result<std::string> GltfAssetImporter::normalize_settings(std::string_view json) const {
    return boundary<std::string>("asset.gltf.settings", [&] { return normalized(settings(json)); });
}
Result<std::vector<std::byte>> GltfAssetImporter::import(const AssetImportInput& input) {
    return boundary<std::vector<std::byte>>("asset.gltf.import", [&] {
        poll(input.stop);
        require(input.kind == info().kind &&
                    input.source.size() <= asset_import_maximum_source_bytes,
                "Invalid glTF importer source");
        take(detail::validate_target(input.target));
        const auto selected = settings(input.settings_json);
        require(input.dependencies.size() == selected.resources.size(),
                "glTF resource aliases must match direct dependencies exactly");
        std::map<AssetId, const AssetImportDependency*> dependencies;
        for (const auto& dependency : input.dependencies) {
            require(dependency.kind == "gltf-resource" &&
                        dependency.importer == "shark.gltf-resource" &&
                        dependency.importer_version == 1 && dependency.payload_version == 1 &&
                        dependencies.emplace(dependency.asset, &dependency).second,
                    "glTF requires typed identity-only resource dependencies");
        }
        std::vector<GltfExternalSource> sources;
        sources.reserve(selected.resources.size());
        for (const auto& [path, id] : selected.resources) {
            poll(input.stop);
            const auto dependency = dependencies.find(id);
            require(dependency != dependencies.end(),
                    "glTF resource alias has no direct dependency");
            const auto& value = *dependency->second;
            sources.push_back({path, id, resource_body(value.payload, id, value.source_digest)});
        }
        const auto asset = take(convert_gltf_asset(
            {selected.path, input.source, sources, input.stop}, {selected.scene}));
        return take(encode_gltf_asset(asset, input.stop));
    });
}
Result<void> GltfAssetImporter::validate_payload(const AssetArtifact& artifact) const {
    return boundary<void>("asset.gltf.validate", [&] {
        descriptor(artifact, info());
        const auto asset = take(decode_gltf_asset(artifact.payload));
        std::set<AssetId> dependencies;
        for (const auto& dependency : artifact.dependencies)
            require(dependencies.insert(dependency.asset).second,
                    "Duplicate glTF artifact dependency");
        for (const auto& resource : asset.geometry.source.resources)
            require(!resource.asset || dependencies.contains(*resource.asset),
                    "glTF payload refers to an undeclared dependency");
        // Extra declared dependencies may be supplied ignored optional-extension
        // resources. They still participate in the owner's keys and byte fences.
    });
}
} // namespace shark::tools
