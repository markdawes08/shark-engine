#include "shark/tools/gltf_source.hpp"
#include "shark/tools/gltf_geometry.hpp"
#include "shark/tools/gltf_textures.hpp"

#include "gltf_image_decoder.hpp"
#include "gltf_source_test_port.hpp"
#include "source_json.hpp"
#include "utf8.hpp"

#include <charconv>
#include <string_view>

namespace {
struct GltfIntegerFailure {};

// cgltf otherwise uses atoi/atoll, whose out-of-range conversions are undefined.
// Attribute suffixes and fields not yet converted by Shark must have the same
// checked boundary as sizes/indices already validated by the source inspector.
template <typename Integer> Integer checked_gltf_integer(const char* text) {
    const std::string_view token{text};
    Integer value{};
    const auto converted = std::from_chars(token.data(), token.data() + token.size(), value);
    if (converted.ec != std::errc{} || converted.ptr != token.data() + token.size() || value < 0)
        throw GltfIntegerFailure{};
    return value;
}
} // namespace

#define CGLTF_ATOI(text) ::checked_gltf_integer<int>(text)
#define CGLTF_ATOLL(text) ::checked_gltf_integer<long long>(text)
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <numeric>
#include <set>
#include <utility>

namespace shark::tools {
namespace {
using Json = nlohmann::json;
using Bytes = std::span<const std::byte>;

struct Failure {
    Error error;
};
[[noreturn]] void fail(std::string message, ErrorCode code = ErrorCode::invalid_configuration) {
    throw Failure{Error{code, std::move(message), {}}};
}
template <typename T> T take(Result<T> result) {
    if (!result)
        throw Failure{std::move(result).error()};
    return std::move(result).value();
}
void cancelled(std::stop_token stop) {
    if (stop.stop_requested())
        fail("glTF source inspection was cancelled", ErrorCode::execution_failed);
}
std::string folded(std::string_view text) {
    std::string result{text};
    for (auto& c : result)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    return result;
}
bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}
std::size_t add(std::size_t a, std::size_t b) {
    if (b > std::numeric_limits<std::size_t>::max() - a)
        fail("glTF size addition overflow");
    return a + b;
}
std::size_t multiply(std::size_t a, std::size_t b) {
    if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a)
        fail("glTF size multiplication overflow");
    return a * b;
}
std::size_t integer(const Json& value,
                    std::size_t maximum = std::numeric_limits<std::size_t>::max()) {
    if ((!value.is_number_unsigned() && !value.is_number_integer()) ||
        (value.is_number_integer() && !value.is_number_unsigned() && value.get<std::int64_t>() < 0))
        fail("glTF requires a nonnegative integer");
    const auto number = value.get<std::uint64_t>();
    if (number > maximum)
        fail("glTF integer exceeds its bound");
    return static_cast<std::size_t>(number);
}
std::size_t required_integer(const Json& object, const char* key,
                             std::size_t maximum = std::numeric_limits<std::size_t>::max()) {
    if (!object.contains(key))
        fail(std::string{"Missing glTF integer: "} + key);
    return integer(object.at(key), maximum);
}
std::size_t optional_integer(const Json& object, const char* key, std::size_t fallback = 0) {
    return object.contains(key) ? integer(object.at(key)) : fallback;
}
std::size_t index(const Json& value, std::size_t count) {
    const auto result = integer(value);
    if (result >= count)
        fail("glTF reference index is out of bounds");
    return result;
}
const Json& array(const Json& object, const char* key) {
    static const Json empty = Json::array();
    if (!object.contains(key))
        return empty;
    const auto& result = object.at(key);
    if (!result.is_array())
        fail(std::string{"glTF table must be an array: "} + key);
    return result;
}
void object(const Json& value) {
    if (!value.is_object())
        fail("glTF table entries must be objects");
}
std::string string(const Json& value, std::size_t maximum = gltf_source_maximum_file_bytes) {
    if (!value.is_string())
        fail("glTF requires a string");
    auto result = value.get<std::string>();
    if (result.size() > maximum || result.find('\0') != std::string::npos)
        fail("glTF string exceeds its bound or contains NUL");
    return result;
}
std::uint32_t little(Bytes bytes, std::size_t offset, std::size_t width) {
    if (width > 4 || offset > bytes.size() || width > bytes.size() - offset)
        fail("Truncated glTF binary value");
    std::uint32_t result = 0;
    for (std::size_t i = 0; i < width; ++i)
        result |= std::to_integer<std::uint32_t>(bytes[offset + i]) << static_cast<unsigned>(8 * i);
    return result;
}
void range(std::size_t offset, std::size_t length, std::size_t available) {
    if (offset > available || length > available - offset)
        fail("glTF byte range exceeds its source");
}

thread_local std::optional<std::size_t> failed_allocation;
thread_local std::size_t allocation_attempts = 0;
thread_local std::size_t live_allocations = 0;
struct Arena;
struct alignas(std::max_align_t) Allocation {
    std::size_t bytes;
    Allocation* previous;
    Allocation* next;
};
struct AllocationFailure {};
struct Arena {
    Allocation* head{};
    std::size_t current{};
    bool exhausted{};
    // cgltf is compiled as private C++ here. An allocation failure unwinds its
    // parser before upstream can walk a partially initialized null array during
    // cleanup. Intrusive tracking needs no second allocation and releases every
    // remaining block without dereferencing those partial parser structures.
    ~Arena() {
        while (head) {
            auto* allocation = head;
            head = allocation->next;
            --live_allocations;
            std::free(allocation);
        }
    }
};
void* allocate(void* user, cgltf_size size) {
    auto& arena = *static_cast<Arena*>(user);
    const auto attempt = allocation_attempts++;
    if (failed_allocation && attempt == *failed_allocation)
        throw AllocationFailure{};
    const auto requested = std::max<std::size_t>(size, 1);
    if (requested > gltf_source_maximum_parser_bytes - sizeof(Allocation) ||
        arena.current > gltf_source_maximum_parser_bytes - sizeof(Allocation) - requested) {
        arena.exhausted = true;
        throw AllocationFailure{};
    }
    const auto charge = requested + sizeof(Allocation);
    auto* memory = static_cast<Allocation*>(std::malloc(charge));
    if (!memory)
        throw AllocationFailure{};
    memory->bytes = charge;
    memory->previous = nullptr;
    memory->next = arena.head;
    if (arena.head)
        arena.head->previous = memory;
    arena.head = memory;
    arena.current += charge;
    ++live_allocations;
    return memory + 1;
}
void deallocate(void* user, void* memory) noexcept {
    if (!memory)
        return;
    auto& arena = *static_cast<Arena*>(user);
    auto* allocation = static_cast<Allocation*>(memory) - 1;
    if (allocation->previous)
        allocation->previous->next = allocation->next;
    else
        arena.head = allocation->next;
    if (allocation->next)
        allocation->next->previous = allocation->previous;
    arena.current -= allocation->bytes;
    --live_allocations;
    std::free(allocation);
}
cgltf_result deny_read(const cgltf_memory_options*, const cgltf_file_options*, const char*,
                       cgltf_size*, void**) {
    return cgltf_result_io_error;
}
void deny_release(const cgltf_memory_options*, const cgltf_file_options*, void*) {}

struct Container {
    GltfContainer kind;
    std::string_view json;
    Bytes binary;
    bool has_binary{};
    std::size_t unknown_chunks{};
};
Container container(const GltfSourceInput& input) {
    const auto name = folded(input.path.text());
    const bool binary_name = ends_with(name, ".glb");
    if (!binary_name && !ends_with(name, ".gltf"))
        fail("glTF source path must end in .gltf or .glb", ErrorCode::invalid_path);
    const bool binary_magic = input.bytes.size() >= 4 && little(input.bytes, 0, 4) == 0x46546c67U;
    if (binary_name != binary_magic)
        fail("glTF container does not match its source suffix");
    if (!binary_name)
        return {GltfContainer::json,
                {reinterpret_cast<const char*>(input.bytes.data()), input.bytes.size()},
                {},
                false,
                0};
    if (input.bytes.size() < 20 || little(input.bytes, 4, 4) != 2 ||
        little(input.bytes, 8, 4) != input.bytes.size())
        fail("Invalid GLB header/version/length");
    Container result{GltfContainer::binary, {}, {}, false, 0};
    std::size_t offset = 12;
    std::size_t chunks = 0;
    while (offset < input.bytes.size()) {
        range(offset, 8, input.bytes.size());
        const auto length = static_cast<std::size_t>(little(input.bytes, offset, 4));
        const auto type = little(input.bytes, offset + 4, 4);
        if (length % 4 != 0)
            fail("GLB chunk lengths must be multiples of four");
        offset += 8;
        range(offset, length, input.bytes.size());
        const auto bytes = input.bytes.subspan(offset, length);
        if (chunks == 0) {
            if (type != 0x4e4f534aU || length == 0)
                fail("GLB requires its nonempty JSON chunk first");
            result.json = {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        } else if (type == 0x004e4942U) {
            if (chunks != 1 || result.has_binary)
                fail("GLB BIN chunk must occur once, immediately after JSON");
            result.has_binary = true;
            result.binary = bytes;
        } else {
            if (type == 0x4e4f534aU)
                fail("GLB cannot contain a second JSON chunk");
            ++result.unknown_chunks;
            if (result.unknown_chunks > gltf_source_maximum_diagnostics)
                fail("Too many unknown GLB chunks");
        }
        offset += length;
        ++chunks;
    }
    return result;
}
Json parse_json(std::string_view text) {
    if (text.find('\0') != std::string_view::npos)
        fail("glTF JSON cannot contain raw NUL bytes");
    return take(detail::parse_source_json(text, gltf_source_maximum_file_bytes,
                                          gltf_source_maximum_json_nodes));
}

struct Resolved {
    GltfResourceStorage storage;
    std::optional<ProjectPath> path;
    std::optional<AssetId> asset;
    Bytes bytes;
    std::vector<std::byte> decoded;
    std::string mime;
};
struct View {
    std::size_t buffer;
    std::size_t offset;
    std::size_t length;
    std::size_t stride;
    bool has_target;
    bool has_stride;
};
struct Sparse {
    std::vector<std::uint32_t> indices;
    std::size_t values_view{};
    std::size_t values_offset{};
};
struct Accessor {
    std::optional<std::size_t> view;
    std::size_t offset;
    std::size_t count;
    std::size_t component;
    std::size_t component_bytes;
    std::size_t element_bytes;
    std::size_t stride;
    std::string type;
    Sparse sparse;
};

struct Inspection {
    const GltfSourceInput& input;
    Container source;
    Json document;
    GltfSourceInspection result;
    std::map<std::string, std::size_t> external;
    std::vector<bool> used;
    std::vector<Resolved> buffers;
    std::vector<Resolved> images;
    std::vector<View> views;
    std::vector<Accessor> accessors;
    std::vector<std::optional<std::uint32_t>> index_bounds;
    std::set<std::string> extensions;
    std::size_t entries{};
    std::size_t decoded_bytes{};
    std::size_t resolved_bytes{};
    std::size_t accessor_elements{};

    explicit Inspection(const GltfSourceInput& value)
        : input(value), source(container(value)), document(parse_json(source.json)),
          result{source.kind, {}, {}, {}, {}}, used(value.external_sources.size(), false) {}

    void charge(std::size_t count) {
        if (count > gltf_source_maximum_table_entries ||
            count > gltf_source_maximum_total_entries - entries)
            fail("glTF table inventory exceeds its bound");
        entries += count;
    }
    void diagnostic(std::string message) {
        if (result.diagnostics.size() >= gltf_source_maximum_diagnostics ||
            message.size() > gltf_source_maximum_diagnostic_bytes)
            fail("glTF diagnostics exceed their bound");
        result.diagnostics.push_back(std::move(message));
    }
    void decode_charge(std::size_t count) {
        if (count > gltf_source_maximum_file_bytes ||
            count > gltf_source_maximum_total_bytes - decoded_bytes)
            fail("glTF decoded source bytes exceed their bound");
        decoded_bytes += count;
    }
    ProjectPath resolve_path(std::string_view uri) const {
        if (uri.empty() || uri.size() > gltf_source_maximum_uri_bytes || uri.front() == '/' ||
            uri.find_first_of("\\?#:") != std::string_view::npos)
            fail("glTF URI must be a contained local relative reference", ErrorCode::invalid_path);
        std::string decoded;
        decoded.reserve(uri.size());
        auto hex = [](char c) -> unsigned {
            if (c >= '0' && c <= '9')
                return static_cast<unsigned>(c - '0');
            if (c >= 'a' && c <= 'f')
                return static_cast<unsigned>(c - 'a') + 10U;
            if (c >= 'A' && c <= 'F')
                return static_cast<unsigned>(c - 'A') + 10U;
            fail("Malformed glTF URI percent escape", ErrorCode::invalid_path);
        };
        for (std::size_t i = 0; i < uri.size(); ++i) {
            if (uri[i] == '%') {
                if (uri.size() - i < 3)
                    fail("Truncated glTF URI percent escape", ErrorCode::invalid_path);
                decoded.push_back(static_cast<char>((hex(uri[i + 1]) << 4U) | hex(uri[i + 2])));
                i += 2;
            } else {
                decoded.push_back(uri[i]);
            }
        }
        if (decoded.empty() || !shark::detail::valid_utf8(decoded, true) ||
            decoded.front() == '/' || decoded.find_first_of("\\:\0", 0, 3) != std::string::npos)
            fail("Invalid decoded glTF URI", ErrorCode::invalid_path);
        std::vector<std::string_view> components;
        const auto& root = input.path.text();
        std::size_t start = 0;
        for (auto end = root.find('/'); end != std::string::npos; end = root.find('/', start)) {
            components.push_back(std::string_view{root}.substr(start, end - start));
            start = end + 1;
        }
        start = 0;
        while (start <= decoded.size()) {
            const auto end = decoded.find('/', start);
            const auto part = std::string_view{decoded}.substr(
                start, end == std::string::npos ? end : end - start);
            if (part.empty())
                fail("Empty glTF URI path component", ErrorCode::invalid_path);
            if (part == "..") {
                if (components.empty())
                    fail("glTF URI escapes its project", ErrorCode::path_outside_root);
                components.pop_back();
            } else if (part != ".") {
                static_cast<void>(take(ProjectPath::parse(part)));
                components.push_back(part);
            }
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        std::string joined;
        for (const auto part : components) {
            if (!joined.empty())
                joined += '/';
            joined += part;
        }
        return take(ProjectPath::parse(joined));
    }
    Resolved data_uri(std::string_view uri, GltfResourceKind kind) {
        const auto comma = uri.find(',');
        if (comma == std::string_view::npos)
            fail("Malformed glTF data URI");
        const auto header = folded(uri.substr(5, comma - 5));
        constexpr std::string_view base64_suffix = ";base64";
        if (!ends_with(header, base64_suffix))
            fail("glTF data URIs require bounded base64 content");
        const auto mime = header.substr(0, header.size() - base64_suffix.size());
        if ((kind == GltfResourceKind::buffer && mime != "application/octet-stream" &&
             mime != "application/gltf-buffer") ||
            (kind == GltfResourceKind::image && mime != "image/png" && mime != "image/jpeg"))
            fail("Unsupported glTF data URI media type");
        const auto body = uri.substr(comma + 1);
        if (body.empty() || body.size() % 4 != 0)
            fail("Invalid glTF base64 length");
        const auto padding =
            static_cast<std::size_t>(body.back() == '=') +
            static_cast<std::size_t>(body.size() >= 2 && body[body.size() - 2] == '=');
        const auto length = multiply(body.size() / 4, 3) - padding;
        decode_charge(length);
        Resolved result_value{GltfResourceStorage::data_uri, {}, {}, {}, {}, mime};
        result_value.decoded.reserve(length);
        auto digit = [](char c) -> unsigned {
            if (c >= 'A' && c <= 'Z')
                return static_cast<unsigned>(c - 'A');
            if (c >= 'a' && c <= 'z')
                return static_cast<unsigned>(c - 'a') + 26U;
            if (c >= '0' && c <= '9')
                return static_cast<unsigned>(c - '0') + 52U;
            if (c == '+')
                return 62;
            if (c == '/')
                return 63;
            fail("Invalid glTF base64 character");
        };
        for (std::size_t i = 0; i < body.size(); i += 4) {
            const auto a = digit(body[i]);
            const auto b = digit(body[i + 1]);
            const bool pad_c = body[i + 2] == '=';
            const bool pad_d = body[i + 3] == '=';
            const auto c = pad_c ? 0U : digit(body[i + 2]);
            const auto d = pad_d ? 0U : digit(body[i + 3]);
            if ((pad_c && !pad_d) || ((pad_c || pad_d) && i + 4 != body.size()) ||
                (pad_c && (b & 15U) != 0) || (pad_d && !pad_c && (c & 3U) != 0))
                fail("Noncanonical glTF base64 padding");
            result_value.decoded.push_back(static_cast<std::byte>((a << 2U) | (b >> 4U)));
            if (!pad_c)
                result_value.decoded.push_back(static_cast<std::byte>((b << 4U) | (c >> 2U)));
            if (!pad_d)
                result_value.decoded.push_back(static_cast<std::byte>((c << 6U) | d));
        }
        result_value.bytes = result_value.decoded;
        return result_value;
    }
    Resolved uri(std::string_view value, GltfResourceKind kind) {
        if (value.size() >= 5 && folded(value.substr(0, 5)) == "data:")
            return data_uri(value, kind);
        auto path = resolve_path(value);
        const auto found = external.find(folded(path.text()));
        if (found == external.end())
            fail("glTF external reference has no supplied source snapshot");
        const auto position = found->second;
        const auto& supplied = input.external_sources[position];
        if (supplied.path != path)
            fail("glTF reference spelling differs from its canonical source mapping",
                 ErrorCode::invalid_path);
        used[position] = true;
        return {
            GltfResourceStorage::external, std::move(path), supplied.asset, supplied.bytes, {}, {}};
    }
    void reference(GltfResourceKind kind, std::size_t position, const Resolved& value) {
        if (value.bytes.size() > gltf_source_maximum_total_bytes - resolved_bytes)
            fail("glTF resolved resource bytes exceed their aggregate bound");
        resolved_bytes += value.bytes.size();
        result.resources.push_back({kind, position, value.storage, value.path, value.asset,
                                    value.bytes.size(), take(sha256(value.bytes)), value.mime});
    }
    void strip_extensions(Json& value) {
        if (value.is_object()) {
            if (value.contains("extensions")) {
                const auto& extension_object = value.at("extensions");
                object(extension_object);
                for (const auto& [name, unused] : extension_object.items()) {
                    static_cast<void>(unused);
                    if (!extensions.contains(name))
                        fail("glTF extension use is missing from extensionsUsed");
                }
                value.erase("extensions");
            }
            for (auto& [name, child] : value.items())
                if (name != "extras")
                    strip_extensions(child);
        } else if (value.is_array()) {
            for (auto& child : value)
                strip_extensions(child);
        } else if (value.is_number_float()) {
            const auto number = value.get<double>();
            if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max())
                fail("glTF numeric value cannot be represented as finite engine data");
        }
    }
    void tables() {
        object(document);
        if (!document.contains("asset"))
            fail("glTF asset descriptor is required");
        const auto& asset = document.at("asset");
        object(asset);
        if (!asset.contains("version") || string(asset.at("version")) != "2.0" ||
            (asset.contains("minVersion") && string(asset.at("minVersion")) != "2.0"))
            fail("Only the declared glTF 2.0 source version is supported");
        constexpr std::array names{"accessors", "animations", "buffers", "bufferViews", "cameras",
                                   "images",    "materials",  "meshes",  "nodes",       "samplers",
                                   "scenes",    "skins",      "textures"};
        for (const auto* name : names) {
            const auto& values = array(document, name);
            charge(values.size());
            for (const auto& value : values)
                object(value);
        }
        if (array(document, "buffers").size() + array(document, "images").size() >
            gltf_source_maximum_resources)
            fail("glTF resource references exceed their bound");
        if (!array(document, "skins").empty() || !array(document, "animations").empty())
            fail("Skins and animations are outside the static glTF profile");
        const auto& required = array(document, "extensionsRequired");
        if (!required.empty())
            fail("Unsupported required glTF extension: " + string(required.front(), 128));
        for (const auto& value : array(document, "extensionsUsed")) {
            const auto name = string(value, 128);
            if (name.empty() || !extensions.insert(name).second)
                fail("Invalid or duplicate glTF extension declaration");
            diagnostic("Ignored optional glTF extension: " + name);
        }
        for (std::size_t i = 0; i < source.unknown_chunks; ++i)
            diagnostic("Ignored unknown GLB chunk");
        if (!array(document, "cameras").empty())
            diagnostic("Static glTF camera definitions are not imported");
        for (std::size_t i = 0; i < input.external_sources.size(); ++i)
            external.emplace(folded(input.external_sources[i].path.text()), i);
        result.counts.scenes = array(document, "scenes").size();
        result.counts.nodes = array(document, "nodes").size();
        result.counts.meshes = array(document, "meshes").size();
        result.counts.accessors = array(document, "accessors").size();
        result.counts.materials = array(document, "materials").size();
        result.counts.textures = array(document, "textures").size();
        result.counts.samplers = array(document, "samplers").size();
        if (document.contains("scene"))
            result.selected_scene = index(document.at("scene"), result.counts.scenes);
        else if (result.counts.scenes != 0)
            result.selected_scene = 0;
        // Unsupported optional extensions use the validated core fallback. Do not
        // let parser recognition silently activate extension behavior.
        strip_extensions(document);
    }
    void resources() {
        const auto& source_buffers = array(document, "buffers");
        buffers.reserve(source_buffers.size());
        bool binary_used = false;
        for (std::size_t i = 0; i < source_buffers.size(); ++i) {
            cancelled(input.stop);
            const auto& value = source_buffers[i];
            const auto length =
                required_integer(value, "byteLength", gltf_source_maximum_file_bytes);
            if (length == 0)
                fail("glTF buffer byteLength must be positive");
            Resolved resolved{GltfResourceStorage::binary_chunk, {}, {}, {}, {}, {}};
            if (value.contains("uri")) {
                resolved = uri(string(value.at("uri")), GltfResourceKind::buffer);
                if (resolved.bytes.size() < length)
                    fail("glTF buffer source is shorter than byteLength");
            } else {
                if (i != 0 || !source.has_binary || source.binary.size() < length ||
                    source.binary.size() - length > 3)
                    fail("glTF buffer has no matching bounded GLB BIN chunk");
                for (const auto byte : source.binary.subspan(length))
                    if (byte != std::byte{0})
                        fail("GLB BIN padding must contain zero bytes");
                resolved.bytes = source.binary.first(length);
                binary_used = true;
            }
            reference(GltfResourceKind::buffer, i, resolved);
            buffers.push_back(std::move(resolved));
        }
        if (source.has_binary && !binary_used)
            fail("GLB contains an unreferenced BIN chunk");
        const auto& source_views = array(document, "bufferViews");
        views.reserve(source_views.size());
        for (const auto& value : source_views) {
            const auto buffer = index(value.at("buffer"), buffers.size());
            const auto offset = optional_integer(value, "byteOffset");
            const auto length = required_integer(value, "byteLength");
            const auto stride = optional_integer(value, "byteStride");
            if (length == 0 ||
                (value.contains("byteStride") && (stride < 4 || stride > 252 || stride % 4 != 0)))
                fail("Invalid glTF bufferView length or stride");
            const auto declared = required_integer(source_buffers[buffer], "byteLength");
            range(offset, length, declared);
            if (value.contains("target")) {
                const auto target = integer(value.at("target"));
                if (target != 34962 && target != 34963)
                    fail("Invalid glTF bufferView target");
            }
            views.push_back({buffer, offset, length, stride, value.contains("target"),
                             value.contains("byteStride")});
        }
        const auto& source_images = array(document, "images");
        images.reserve(source_images.size());
        for (std::size_t i = 0; i < source_images.size(); ++i) {
            cancelled(input.stop);
            const auto& image = source_images[i];
            if (image.contains("uri") == image.contains("bufferView"))
                fail("glTF image requires exactly one URI or bufferView");
            Resolved resolved{GltfResourceStorage::buffer_view, {}, {}, {}, {}, {}};
            if (image.contains("uri")) {
                resolved = uri(string(image.at("uri")), GltfResourceKind::image);
            } else {
                const auto& view = views.at(index(image.at("bufferView"), views.size()));
                if (!image.contains("mimeType"))
                    fail("Embedded glTF image requires mimeType");
                resolved.bytes = buffers[view.buffer].bytes.subspan(view.offset, view.length);
            }
            if (image.contains("mimeType")) {
                const auto mime = string(image.at("mimeType"), 128);
                if (!resolved.mime.empty() && resolved.mime != mime)
                    fail("glTF image media types disagree");
                resolved.mime = mime;
            }
            constexpr std::array<std::byte, 8> png{std::byte{0x89}, std::byte{'P'}, std::byte{'N'},
                                                   std::byte{'G'},  std::byte{13},  std::byte{10},
                                                   std::byte{26},   std::byte{10}};
            const bool is_png = resolved.bytes.size() >= png.size() &&
                                std::equal(png.begin(), png.end(), resolved.bytes.begin());
            const bool is_jpeg =
                resolved.bytes.size() >= 3 && resolved.bytes[0] == std::byte{0xff} &&
                resolved.bytes[1] == std::byte{0xd8} && resolved.bytes[2] == std::byte{0xff};
            const auto detected = is_png ? "image/png" : is_jpeg ? "image/jpeg" : "";
            if (resolved.mime.empty())
                resolved.mime = detected;
            if ((resolved.mime != "image/png" && resolved.mime != "image/jpeg") ||
                resolved.mime != detected)
                fail("glTF image requires matching PNG/JPEG encoded content");
            reference(GltfResourceKind::image, i, resolved);
            images.push_back(std::move(resolved));
        }
        if (std::find(used.begin(), used.end(), false) != used.end())
            fail("Supplied glTF external source is not referenced");
    }
    Bytes view_bytes(std::size_t position) const {
        const auto& view = views[position];
        return buffers[view.buffer].bytes.subspan(view.offset, view.length);
    }
    void accessor_tables() {
        const auto& source_accessors = array(document, "accessors");
        accessors.reserve(source_accessors.size());
        index_bounds.resize(source_accessors.size());
        for (const auto& value : source_accessors) {
            cancelled(input.stop);
            const auto count =
                required_integer(value, "count", gltf_source_maximum_accessor_elements);
            if (count == 0)
                fail("glTF accessor count must be positive");
            if (count > gltf_source_maximum_total_accessor_elements - accessor_elements)
                fail("glTF aggregate accessor elements exceed their storage/work bound");
            accessor_elements += count;
            const auto component = required_integer(value, "componentType");
            const auto width = component == 5120 || component == 5121   ? 1U
                               : component == 5122 || component == 5123 ? 2U
                               : component == 5125 || component == 5126 ? 4U
                                                                        : 0U;
            if (width == 0 || !value.contains("type"))
                fail("Invalid glTF accessor component or type");
            const auto type = string(value.at("type"), 16);
            const auto dimensions = type == "SCALAR" ? 1U
                                    : type == "VEC2" ? 2U
                                    : type == "VEC3" ? 3U
                                    : type == "VEC4" ? 4U
                                    : type == "MAT2" ? 2U
                                    : type == "MAT3" ? 3U
                                    : type == "MAT4" ? 4U
                                                     : 0U;
            if (dimensions == 0)
                fail("Invalid glTF accessor type");
            const bool matrix = type.starts_with("MAT");
            const auto column = multiply(dimensions, width);
            const auto padded_column = matrix ? (add(column, 3) / 4) * 4 : column;
            const auto element = matrix ? multiply(padded_column, dimensions) : column;
            Accessor accessor{{},      optional_integer(value, "byteOffset"),
                              count,   component,
                              width,   element,
                              element, type,
                              {}};
            if (value.contains("normalized")) {
                if (!value.at("normalized").is_boolean() ||
                    (value.at("normalized").get<bool>() &&
                     (component == 5125 || component == 5126)))
                    fail("Invalid glTF accessor normalization");
            }
            if (value.contains("bufferView")) {
                const auto position = index(value.at("bufferView"), views.size());
                const auto& view = views[position];
                accessor.view = position;
                accessor.stride = view.stride == 0 ? element : view.stride;
                if (accessor.stride < element || accessor.stride % width != 0 ||
                    accessor.offset % width != 0 ||
                    add(view.offset, accessor.offset) % width != 0 ||
                    (matrix && add(view.offset, accessor.offset) % 4 != 0))
                    fail("Misaligned glTF accessor or undersized stride");
                range(accessor.offset, add(multiply(count - 1, accessor.stride), element),
                      view.length);
            } else if (value.contains("byteOffset")) {
                fail("glTF accessor without bufferView cannot specify byteOffset");
            }
            if (value.contains("sparse")) {
                const auto& sparse = value.at("sparse");
                object(sparse);
                const auto sparse_count = required_integer(sparse, "count", count);
                if (sparse_count == 0 || !sparse.contains("indices") || !sparse.contains("values"))
                    fail("Invalid glTF sparse accessor descriptor");
                const auto& indices = sparse.at("indices");
                const auto& values = sparse.at("values");
                object(indices);
                object(values);
                const auto indices_view = index(indices.at("bufferView"), views.size());
                const auto values_view = index(values.at("bufferView"), views.size());
                const auto indices_component = required_integer(indices, "componentType");
                const auto indices_width = indices_component == 5121   ? 1U
                                           : indices_component == 5123 ? 2U
                                           : indices_component == 5125 ? 4U
                                                                       : 0U;
                if (indices_width == 0)
                    fail("glTF sparse indices require unsigned integer components");
                const auto indices_offset = optional_integer(indices, "byteOffset");
                const auto values_offset = optional_integer(values, "byteOffset");
                if (views[indices_view].has_stride || views[values_view].has_stride ||
                    views[indices_view].has_target || views[values_view].has_target ||
                    indices_offset % indices_width != 0 || values_offset % width != 0 ||
                    add(views[indices_view].offset, indices_offset) % indices_width != 0 ||
                    add(views[values_view].offset, values_offset) % width != 0 ||
                    (matrix && add(views[values_view].offset, values_offset) % 4 != 0))
                    fail("Invalid sparse glTF alignment or stride");
                range(indices_offset, multiply(sparse_count, indices_width),
                      views[indices_view].length);
                range(values_offset, multiply(sparse_count, element), views[values_view].length);
                accessor.sparse.values_view = values_view;
                accessor.sparse.values_offset = values_offset;
                accessor.sparse.indices.reserve(sparse_count);
                const auto bytes = view_bytes(indices_view);
                for (std::size_t i = 0; i < sparse_count; ++i) {
                    const auto sparse_index =
                        little(bytes, indices_offset + i * indices_width, indices_width);
                    if (sparse_index >= count || (!accessor.sparse.indices.empty() &&
                                                  sparse_index <= accessor.sparse.indices.back()))
                        fail("glTF sparse indices must be strictly increasing and in range");
                    accessor.sparse.indices.push_back(sparse_index);
                }
            }
            accessors.push_back(std::move(accessor));
        }
    }
    std::uint32_t scalar(const Accessor& accessor, std::size_t element) const {
        const auto found =
            std::lower_bound(accessor.sparse.indices.begin(), accessor.sparse.indices.end(),
                             static_cast<std::uint32_t>(element));
        if (found != accessor.sparse.indices.end() && *found == element) {
            const auto sparse_element =
                static_cast<std::size_t>(found - accessor.sparse.indices.begin());
            return little(view_bytes(accessor.sparse.values_view),
                          accessor.sparse.values_offset + sparse_element * accessor.element_bytes,
                          accessor.component_bytes);
        }
        return accessor.view
                   ? little(view_bytes(*accessor.view), accessor.offset + element * accessor.stride,
                            accessor.component_bytes)
                   : 0;
    }
    void meshes() {
        for (const auto& mesh : array(document, "meshes")) {
            if (mesh.contains("weights"))
                fail("Morph weights are outside the static glTF profile");
            const auto& primitives = array(mesh, "primitives");
            if (primitives.empty())
                fail("glTF mesh must contain primitives");
            charge(primitives.size());
            result.counts.primitives += primitives.size();
            for (const auto& primitive : primitives) {
                cancelled(input.stop);
                object(primitive);
                if (!array(primitive, "targets").empty())
                    fail("Morph targets are outside the static glTF profile");
                const auto mode = optional_integer(primitive, "mode", 4);
                if (mode < 4 || mode > 6)
                    fail("Only triangle glTF primitive modes are supported");
                if (!primitive.contains("attributes"))
                    fail("glTF primitive requires core vertex attributes");
                const auto& attributes = primitive.at("attributes");
                object(attributes);
                charge(attributes.size());
                if (!attributes.contains("POSITION"))
                    fail("glTF primitive requires a core POSITION fallback");
                const auto& position =
                    accessors[index(attributes.at("POSITION"), accessors.size())];
                if (position.type != "VEC3" || position.component != 5126)
                    fail("glTF POSITION requires floating point VEC3 data");
                for (const auto& [name, attribute] : attributes.items()) {
                    if (name == "JOINTS_0" || name == "WEIGHTS_0" || name == "JOINTS_1" ||
                        name == "WEIGHTS_1")
                        fail("Skin vertex attributes are outside the static glTF profile");
                    const auto& accessor = accessors[index(attribute, accessors.size())];
                    if (accessor.count != position.count)
                        fail("glTF primitive attribute counts disagree");
                    if (name.starts_with("TEXCOORD_") && name != "TEXCOORD_0" &&
                        name != "TEXCOORD_1")
                        fail("glTF texture coordinate sets above one are unsupported");
                }
                if (primitive.contains("indices")) {
                    const auto indices_position = index(primitive.at("indices"), accessors.size());
                    const auto& indices = accessors[indices_position];
                    if (indices.type != "SCALAR" ||
                        (indices.component != 5121 && indices.component != 5123 &&
                         indices.component != 5125) ||
                        indices.stride != indices.component_bytes)
                        fail("glTF primitive indices require tightly packed unsigned scalar data");
                    auto& bound = index_bounds[indices_position];
                    if (!bound) {
                        std::uint32_t maximum = 0;
                        for (std::size_t i = 0; i < indices.count; ++i)
                            maximum = std::max(maximum, scalar(indices, i));
                        bound = maximum;
                    }
                    if (*bound >= position.count)
                        fail("glTF primitive vertex index is out of range");
                }
                if (primitive.contains("material"))
                    static_cast<void>(index(primitive.at("material"), result.counts.materials));
            }
        }
    }
    void graph() {
        const auto& nodes = array(document, "nodes");
        std::vector<std::optional<std::size_t>> parents(nodes.size());
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const auto& node = nodes[i];
            if (node.contains("skin") || node.contains("weights"))
                fail("Skin or morph node data is outside the static glTF profile");
            if (node.contains("matrix") && (node.contains("translation") ||
                                            node.contains("rotation") || node.contains("scale")))
                fail("glTF node cannot combine matrix and TRS transforms");
            for (const auto& [name, size] : std::array<std::pair<const char*, std::size_t>, 4>{
                     {{"matrix", 16}, {"translation", 3}, {"rotation", 4}, {"scale", 3}}}) {
                if (node.contains(name)) {
                    const auto& transform = node.at(name);
                    if (!transform.is_array() || transform.size() != size)
                        fail("Invalid glTF node transform shape");
                    for (const auto& value : transform)
                        if (!value.is_number())
                            fail("glTF transform requires numeric values");
                }
            }
            if (node.contains("mesh"))
                static_cast<void>(index(node.at("mesh"), result.counts.meshes));
            if (node.contains("camera"))
                static_cast<void>(index(node.at("camera"), array(document, "cameras").size()));
            const auto& children = array(node, "children");
            charge(children.size());
            for (const auto& child : children) {
                const auto position = index(child, nodes.size());
                if (position == i || parents[position])
                    fail("glTF node hierarchy has a cycle or multiple parents");
                parents[position] = i;
            }
        }
        // Parent links have at most one predecessor. This iterative color walk
        // bounds graph work without recursion on a hostile node chain.
        std::vector<unsigned char> colors(nodes.size(), 0);
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            std::optional<std::size_t> current{i};
            while (current && colors[*current] == 0) {
                colors[*current] = 1;
                current = parents[*current];
            }
            if (current && colors[*current] == 1)
                fail("glTF node hierarchy contains a cycle");
            current = i;
            while (current && colors[*current] == 1) {
                colors[*current] = 2;
                current = parents[*current];
            }
        }
        for (const auto& scene : array(document, "scenes")) {
            const auto& roots = array(scene, "nodes");
            charge(roots.size());
            std::set<std::size_t> unique;
            for (const auto& root : roots) {
                const auto position = index(root, nodes.size());
                if (parents[position] || !unique.insert(position).second)
                    fail("glTF scene root is duplicated or has a parent");
            }
        }
        // These fields are converted directly to C++ enum types by cgltf.
        // Even an int-representable value must be rejected before that cast if
        // it is outside the declared glTF sampler domain.
        for (const auto& sampler : array(document, "samplers")) {
            const auto allowed = [&](const char* key, std::initializer_list<std::size_t> values) {
                if (!sampler.contains(key))
                    return;
                const auto value = integer(sampler.at(key));
                if (std::find(values.begin(), values.end(), value) == values.end())
                    fail("Invalid glTF sampler enum value");
            };
            allowed("wrapS", {33071, 33648, 10497});
            allowed("wrapT", {33071, 33648, 10497});
            allowed("magFilter", {9728, 9729});
            allowed("minFilter", {9728, 9729, 9984, 9985, 9986, 9987});
        }
        for (const auto& texture : array(document, "textures")) {
            if (!texture.contains("source"))
                fail("glTF texture requires a core image fallback");
            static_cast<void>(index(texture.at("source"), array(document, "images").size()));
            if (texture.contains("sampler"))
                static_cast<void>(index(texture.at("sampler"), result.counts.samplers));
        }
    }
    void parser() {
        cancelled(input.stop);
        // Parsed core JSON has already passed all native index/range checks.
        // Optional extension objects are intentionally omitted from this view.
        const auto canonical = document.dump();
        Arena arena;
        cgltf_options options{};
        options.type = cgltf_file_type_gltf;
        options.memory = {allocate, deallocate, &arena};
        options.file = {deny_read, deny_release, nullptr};
        cgltf_data* raw = nullptr;
        cgltf_result parsed;
        try {
            parsed = cgltf_parse(&options, canonical.data(), canonical.size(), &raw);
        } catch (const GltfIntegerFailure&) {
            fail("glTF parser integer is malformed or outside its supported range");
        } catch (const AllocationFailure&) {
            fail(arena.exhausted ? "glTF parser exceeded its allocation budget"
                                 : "glTF parser allocation failed",
                 ErrorCode::execution_failed);
        }
        std::unique_ptr<cgltf_data, decltype(&cgltf_free)> owner{raw, cgltf_free};
        if (parsed != cgltf_result_success) {
            if (parsed == cgltf_result_out_of_memory)
                fail(arena.exhausted ? "glTF parser exceeded its allocation budget"
                                     : "glTF parser allocation failed",
                     ErrorCode::execution_failed);
            fail("glTF parser rejected the bounded core source");
        }
        // cgltf_validate's raw index scan assumes native alignment and ignores
        // sparse overrides. The bytewise checks above validate effective indices
        // without either assumption; metadata validation leaves data unbound.
        if (cgltf_validate(owner.get()) != cgltf_result_success)
            fail("glTF parser metadata validation failed");
        cancelled(input.stop);
    }
    void prepare() {
        tables();
        resources();
        accessor_tables();
        meshes();
        graph();
        parser();
    }
    GltfSourceInspection run() {
        prepare();
        return std::move(result);
    }
};

// Geometry stays on the validated source owner. No parser/data pointer crosses
// this boundary, and neither cgltf's aligned reads nor its file helpers are used.
struct GeometryConversion {
    Inspection& source;
    const GltfGeometrySettings& settings;
    std::string& field;
    GltfGeometry result{};
    std::size_t output_bytes{};
    std::size_t work_items{};
    std::size_t vertices{};
    std::size_t indices{};

    void charge(std::size_t bytes) {
        if (bytes > gltf_geometry_maximum_output_bytes - output_bytes)
            fail("glTF geometry output exceeds its byte bound");
        output_bytes += bytes;
    }
    void work(std::size_t count) {
        if (count > gltf_geometry_maximum_work - work_items)
            fail("glTF geometry conversion exceeds its work bound");
        work_items += count;
    }
    void poll(std::size_t iteration = 0) const {
        if ((iteration & 255U) == 0)
            cancelled(source.input.stop);
    }
    std::string name(const Json& value) {
        if (!value.contains("name"))
            return {};
        auto text = string(value.at("name"), gltf_source_maximum_uri_bytes);
        charge(text.size());
        return text;
    }
    static math::Vec3 vector(const std::array<float, 3>& value) {
        return {value[0], value[1], value[2]};
    }
    static std::array<float, 3> packed(const math::Vec3& value) {
        return {static_cast<float>(value.x), static_cast<float>(value.y),
                static_cast<float>(value.z)};
    }
    static math::Vec3 unit(const math::Vec3& value) {
        const auto length = std::hypot(value.x, value.y, value.z);
        if (!std::isfinite(length) || length == 0.0)
            fail("glTF direction must be finite and nonzero");
        return value / length;
    }
    static void include(std::optional<GltfGeometryBounds>& bounds, const math::Vec3& point) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
            fail("glTF transformed geometry bound is nonfinite");
        if (!bounds)
            bounds = GltfGeometryBounds{point, point};
        else {
            bounds->minimum = glm::min(bounds->minimum, point);
            bounds->maximum = glm::max(bounds->maximum, point);
        }
    }
    static void include(std::optional<GltfGeometryBounds>& bounds,
                        const GltfGeometryBounds& other) {
        include(bounds, other.minimum);
        include(bounds, other.maximum);
    }
    static GltfGeometryBounds transformed(const GltfGeometryBounds& bounds,
                                          const math::Mat4& matrix) {
        std::optional<GltfGeometryBounds> result;
        for (unsigned corner = 0; corner != 8; ++corner) {
            const math::Vec3 point{(corner & 1U) ? bounds.maximum.x : bounds.minimum.x,
                                   (corner & 2U) ? bounds.maximum.y : bounds.minimum.y,
                                   (corner & 4U) ? bounds.maximum.z : bounds.minimum.z};
            include(result, math::Vec3{matrix * glm::dvec4{point, 1.0}});
        }
        return *result;
    }
    // Return the complete effective element, including sparse replacement or
    // implicit zero base. All ranges/packing have already passed 12a preflight.
    Bytes element(const Accessor& accessor, std::size_t position) const {
        const auto found =
            std::lower_bound(accessor.sparse.indices.begin(), accessor.sparse.indices.end(),
                             static_cast<std::uint32_t>(position));
        if (found != accessor.sparse.indices.end() && *found == position) {
            const auto sparse = static_cast<std::size_t>(found - accessor.sparse.indices.begin());
            return source.view_bytes(accessor.sparse.values_view)
                .subspan(accessor.sparse.values_offset + sparse * accessor.element_bytes,
                         accessor.element_bytes);
        }
        if (!accessor.view)
            return {};
        return source.view_bytes(*accessor.view)
            .subspan(accessor.offset + position * accessor.stride, accessor.element_bytes);
    }
    std::array<float, 4> values(std::size_t accessor_index, std::size_t position,
                                std::size_t components) const {
        const auto& accessor = source.accessors[accessor_index];
        const auto bytes = element(accessor, position);
        std::array<float, 4> decoded{};
        if (bytes.empty())
            return decoded;
        for (std::size_t component = 0; component < components; ++component) {
            const auto bits =
                little(bytes, component * accessor.component_bytes, accessor.component_bytes);
            if (accessor.component == 5126)
                decoded[component] = std::bit_cast<float>(bits);
            else if (accessor.component == 5121)
                decoded[component] = static_cast<float>(bits) / 255.0F;
            else if (accessor.component == 5123)
                decoded[component] = static_cast<float>(bits) / 65535.0F;
            else
                fail("Unsupported glTF vertex component representation");
            if (!std::isfinite(decoded[component]))
                fail("glTF vertex component is not finite");
            if (decoded[component] == 0.0F)
                decoded[component] = 0.0F;
        }
        return decoded;
    }
    std::size_t attribute(const Json& attributes, const char* semantic, const char* type,
                          bool normalized_integer = false, bool color = false) {
        const auto position = index(attributes.at(semantic), source.accessors.size());
        const auto& accessor = source.accessors[position];
        const auto& json = array(source.document, "accessors")[position];
        const bool normalized = json.value("normalized", false);
        if ((accessor.type != type && !(color && accessor.type == "VEC4")) ||
            (accessor.component != 5126 &&
             !(normalized_integer && normalized &&
               (accessor.component == 5121 || accessor.component == 5123))))
            fail(std::string{"Unsupported glTF vertex representation: "} + semantic);
        if (accessor.view &&
            (accessor.offset % 4 != 0 ||
             add(source.views[*accessor.view].offset, accessor.offset) % 4 != 0 ||
             (accessor.component_bytes != 4 && !source.views[*accessor.view].has_stride)))
            fail(std::string{"glTF vertex attribute requires four-byte alignment: "} + semantic);
        if (accessor.view && source.views[*accessor.view].has_target &&
            integer(array(source.document, "bufferViews")[*accessor.view].at("target")) != 34962)
            fail(std::string{"glTF vertex bufferView has an index target: "} + semantic);
        return position;
    }
    std::optional<std::size_t> optional_attribute(const Json& attributes, const char* semantic,
                                                  const char* type, bool normalized_integer = false,
                                                  bool color = false) {
        if (!attributes.contains(semantic))
            return {};
        return attribute(attributes, semantic, type, normalized_integer, color);
    }
    // Only the core texture-coordinate links required by geometry are consumed
    // here; complete material conversion and fallback remain the 12d owner.
    std::optional<std::size_t> material_uvs(const Json& primitive, const Json& attributes) {
        if (!primitive.contains("material"))
            return {};
        const auto& material =
            array(source.document,
                  "materials")[index(primitive.at("material"), source.result.counts.materials)];
        std::optional<std::size_t> normal_uv;
        auto texture = [&](const Json& parent, const char* key, bool normal = false) {
            if (!parent.contains(key))
                return;
            const auto& info = parent.at(key);
            object(info);
            static_cast<void>(index(info.at("index"), source.result.counts.textures));
            const auto uv = optional_integer(info, "texCoord");
            if (uv > 1 || !attributes.contains(uv == 0 ? "TEXCOORD_0" : "TEXCOORD_1"))
                fail("glTF material references an unavailable supported UV set");
            if (normal)
                normal_uv = uv;
        };
        if (material.contains("pbrMetallicRoughness")) {
            const auto& pbr = material.at("pbrMetallicRoughness");
            object(pbr);
            texture(pbr, "baseColorTexture");
            texture(pbr, "metallicRoughnessTexture");
        }
        texture(material, "normalTexture", true);
        texture(material, "occlusionTexture");
        texture(material, "emissiveTexture");
        return normal_uv;
    }
    GltfGeometryPrimitive primitive(const Json& value, std::size_t mesh_index,
                                    std::size_t primitive_index) {
        field = "meshes[" + std::to_string(mesh_index) + "].primitives[" +
                std::to_string(primitive_index) + "]";
        poll();
        const auto& attributes = value.at("attributes");
        const auto position = attribute(attributes, "POSITION", "VEC3");
        const auto normal = optional_attribute(attributes, "NORMAL", "VEC3");
        const auto tangent = optional_attribute(attributes, "TANGENT", "VEC4");
        const auto uv0 = optional_attribute(attributes, "TEXCOORD_0", "VEC2", true);
        const auto uv1 = optional_attribute(attributes, "TEXCOORD_1", "VEC2", true);
        const auto color = optional_attribute(attributes, "COLOR_0", "VEC3", true, true);
        for (const auto& [semantic, ignored] : attributes.items()) {
            static_cast<void>(ignored);
            if (semantic == "POSITION" || semantic == "NORMAL" || semantic == "TANGENT" ||
                semantic == "TEXCOORD_0" || semantic == "TEXCOORD_1" || semantic == "COLOR_0")
                continue;
            if (!semantic.starts_with('_'))
                fail("Unsupported glTF vertex semantic: " + semantic);
            source.diagnostic("Ignored custom glTF vertex attribute: " + semantic);
        }
        const auto normal_uv = material_uvs(value, attributes);
        const bool generate_normals = !normal;
        const bool generate_tangents = normal_uv && (!normal || !tangent);
        const bool split = generate_normals || generate_tangents;
        const auto count = source.accessors[position].count;
        const auto mode = optional_integer(value, "mode", 4);
        const auto index_accessor =
            value.contains("indices")
                ? std::optional<std::size_t>{index(value.at("indices"), source.accessors.size())}
                : std::nullopt;
        const auto source_count = index_accessor ? source.accessors[*index_accessor].count : count;
        if (index_accessor) {
            const auto& accessor = source.accessors[*index_accessor];
            const auto& descriptor = array(source.document, "accessors")[*index_accessor];
            if (descriptor.value("normalized", false))
                fail("glTF primitive indices cannot be normalized");
            if (accessor.view && source.views[*accessor.view].has_target &&
                integer(array(source.document, "bufferViews")[*accessor.view].at("target")) !=
                    34963)
                fail("glTF index bufferView has a vertex target");
        }
        if (source_count < 3 || (mode == 4 && source_count % 3 != 0))
            fail("glTF triangle primitive has an invalid element count");
        const auto triangle_indices = mode == 4 ? source_count : multiply(source_count - 2, 3);
        const auto output_vertices = split ? triangle_indices : count;
        if (output_vertices > gltf_geometry_maximum_vertices - vertices ||
            triangle_indices > gltf_geometry_maximum_indices - indices)
            fail("glTF geometry vertex/index inventory exceeds its bound");
        vertices += output_vertices;
        indices += triangle_indices;
        charge(add(sizeof(GltfGeometryPrimitive),
                   add(multiply(output_vertices, sizeof(GltfGeometryVertex)),
                       multiply(triangle_indices, sizeof(std::uint32_t)))));
        std::size_t components = 3;
        if (normal)
            components += 3;
        if (normal && tangent)
            components += 4;
        if (uv0)
            components += 2;
        if (uv1)
            components += 2;
        if (color)
            components += source.accessors[*color].type == "VEC3" ? 3U : 4U;
        work(add(multiply(count, components), add(source_count, triangle_indices)));

        GltfGeometryPrimitive converted;
        converted.source_mesh = mesh_index;
        converted.source_primitive = primitive_index;
        if (value.contains("material"))
            converted.source_material = integer(value.at("material"));
        converted.has_texcoord0 = uv0.has_value();
        converted.has_texcoord1 = uv1.has_value();
        converted.has_color = color.has_value();
        converted.has_tangents = (normal && tangent) || generate_tangents;
        converted.generated_normals = generate_normals;
        converted.generated_tangents = generate_tangents;
        std::vector<GltfGeometryVertex> base(count);
        for (std::size_t i = 0; i < count; ++i) {
            poll(i);
            auto& vertex = base[i];
            const auto point = values(position, i, 3);
            std::copy_n(point.begin(), 3, vertex.position.begin());
            if (normal) {
                const auto direction = values(*normal, i, 3);
                const math::Vec3 raw{direction[0], direction[1], direction[2]};
                if (std::abs(std::hypot(raw.x, raw.y, raw.z) - 1.0) > 1e-4)
                    fail("glTF NORMAL must have unit length");
                vertex.normal = packed(unit(raw));
            }
            if (normal && tangent) {
                vertex.tangent = values(*tangent, i, 4);
                const math::Vec3 raw{vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]};
                if (std::abs(std::hypot(raw.x, raw.y, raw.z) - 1.0) > 1e-4 ||
                    std::abs(glm::dot(vector(vertex.normal), raw)) > 1e-4 ||
                    (vertex.tangent[3] != 1.0F && vertex.tangent[3] != -1.0F))
                    fail("glTF TANGENT requires an orthogonal unit direction and sign");
                const auto direction = packed(unit(raw));
                std::copy(direction.begin(), direction.end(), vertex.tangent.begin());
            }
            if (uv0) {
                const auto uv = values(*uv0, i, 2);
                std::copy_n(uv.begin(), 2, vertex.texcoord0.begin());
            }
            if (uv1) {
                const auto uv = values(*uv1, i, 2);
                std::copy_n(uv.begin(), 2, vertex.texcoord1.begin());
            }
            if (color) {
                const auto n = source.accessors[*color].type == "VEC3" ? 3U : 4U;
                const auto rgba = values(*color, i, n);
                for (std::size_t component = 0; component < n; ++component) {
                    if (rgba[component] < 0.0F || rgba[component] > 1.0F)
                        fail("glTF COLOR_0 must stay in the unit interval");
                    vertex.color[component] = rgba[component];
                }
            }
        }
        auto source_index = [&](std::size_t i) {
            if (!index_accessor)
                return static_cast<std::uint32_t>(i);
            const auto& accessor = source.accessors[*index_accessor];
            const auto value = source.scalar(accessor, i);
            const auto reserved = accessor.component == 5121   ? 255U
                                  : accessor.component == 5123 ? 65535U
                                                               : 0xffffffffU;
            if (value == reserved)
                fail("glTF primitive uses a reserved maximum index value");
            return value;
        };
        std::vector<std::uint32_t> triangles;
        triangles.reserve(triangle_indices);
        bool discarded = false;
        auto triangle = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
            const auto cross = glm::cross(vector(base[b].position) - vector(base[a].position),
                                          vector(base[c].position) - vector(base[a].position));
            if (std::hypot(cross.x, cross.y, cross.z) == 0.0) {
                discarded = true;
                return;
            }
            triangles.push_back(a);
            triangles.push_back(b);
            triangles.push_back(c);
        };
        if (mode == 4) {
            for (std::size_t i = 0; i < source_count; i += 3) {
                poll(i);
                triangle(source_index(i), source_index(i + 1), source_index(i + 2));
            }
        } else {
            const auto first = source_index(0);
            auto a = first;
            auto b = source_index(1);
            for (std::size_t i = 2; i < source_count; ++i) {
                poll(i);
                const auto c = source_index(i);
                if (mode == 6)
                    triangle(first, b, c);
                else if (i % 2 == 0)
                    triangle(a, b, c);
                else
                    triangle(b, a, c);
                a = b;
                b = c;
            }
        }
        if (triangles.empty())
            fail("glTF primitive has no nondegenerate triangles");
        if (discarded)
            source.diagnostic("Discarded zero-area glTF triangles");
        converted.indices.reserve(triangle_indices);
        if (split)
            converted.vertices.reserve(output_vertices);
        else
            converted.vertices = std::move(base);
        bool degenerate_uv = false;
        std::optional<GltfGeometryBounds> bounds;
        for (std::size_t i = 0; i < triangles.size(); i += 3) {
            poll(i);
            if (!split) {
                if (converted.has_tangents &&
                    (converted.vertices[triangles[i]].tangent[3] !=
                         converted.vertices[triangles[i + 1]].tangent[3] ||
                     converted.vertices[triangles[i]].tangent[3] !=
                         converted.vertices[triangles[i + 2]].tangent[3]))
                    fail("glTF triangle tangent handedness is inconsistent");
                for (std::size_t corner = 0; corner < 3; ++corner) {
                    converted.indices.push_back(triangles[i + corner]);
                    include(bounds, vector(converted.vertices[triangles[i + corner]].position));
                }
                continue;
            }
            std::array<GltfGeometryVertex, 3> face{base[triangles[i]], base[triangles[i + 1]],
                                                   base[triangles[i + 2]]};
            const auto edge1 = vector(face[1].position) - vector(face[0].position);
            const auto edge2 = vector(face[2].position) - vector(face[0].position);
            if (generate_normals) {
                const auto direction = packed(unit(glm::cross(edge1, edge2)));
                for (auto& vertex : face)
                    vertex.normal = direction;
            }
            if (generate_tangents) {
                auto uv = [&](std::size_t corner) -> const std::array<float, 2>& {
                    return *normal_uv == 0 ? face[corner].texcoord0 : face[corner].texcoord1;
                };
                const double u1 = static_cast<double>(uv(1)[0]) - uv(0)[0];
                const double v1 = static_cast<double>(uv(1)[1]) - uv(0)[1];
                const double u2 = static_cast<double>(uv(2)[0]) - uv(0)[0];
                const double v2 = static_cast<double>(uv(2)[1]) - uv(0)[1];
                const auto determinant = u1 * v2 - u2 * v1;
                for (auto& vertex : face) {
                    const auto n = vector(vertex.normal);
                    math::Vec3 t{0.0};
                    double sign = 1.0;
                    if (determinant != 0.0) {
                        const auto derivative = (edge1 * v2 - edge2 * v1) / determinant;
                        const auto projected = derivative - n * glm::dot(n, derivative);
                        if (std::hypot(projected.x, projected.y, projected.z) != 0.0) {
                            t = unit(projected);
                            const auto bitangent = (edge2 * u1 - edge1 * u2) / determinant;
                            sign = glm::dot(glm::cross(n, t), bitangent) < 0.0 ? -1.0 : 1.0;
                        } else {
                            degenerate_uv = true;
                        }
                    } else {
                        degenerate_uv = true;
                    }
                    if (determinant == 0.0 || t == math::Vec3{0.0}) {
                        const auto absolute = glm::abs(n);
                        const math::Vec3 axis = absolute.x <= absolute.y && absolute.x <= absolute.z
                                                    ? math::Vec3{1.0, 0.0, 0.0}
                                                : absolute.y <= absolute.z
                                                    ? math::Vec3{0.0, 1.0, 0.0}
                                                    : math::Vec3{0.0, 0.0, 1.0};
                        t = unit(glm::cross(axis, n));
                    }
                    const auto direction = packed(t);
                    std::copy(direction.begin(), direction.end(), vertex.tangent.begin());
                    vertex.tangent[3] = static_cast<float>(sign);
                }
            }
            for (const auto& vertex : face) {
                converted.indices.push_back(static_cast<std::uint32_t>(converted.vertices.size()));
                converted.vertices.push_back(vertex);
                include(bounds, vector(vertex.position));
            }
        }
        if (degenerate_uv)
            source.diagnostic("Used orthogonal fallback for degenerate glTF tangent UVs");
        converted.bounds = *bounds;
        return converted;
    }
    Transform local_transform(const Json& value) {
        if (value.contains("matrix")) {
            const auto& raw = value.at("matrix");
            math::Mat4 matrix{1.0};
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row)
                    matrix[column][row] =
                        raw[static_cast<std::size_t>(column * 4 + row)].get<double>();
            return take(decompose_transform(matrix));
        }
        Transform local;
        for (const auto& [key, target] : std::array<std::pair<const char*, math::Vec3*>, 2>{
                 {{"translation", &local.translation}, {"scale", &local.scale}}}) {
            if (value.contains(key))
                for (int axis = 0; axis < 3; ++axis)
                    (*target)[axis] = value.at(key)[static_cast<std::size_t>(axis)].get<double>();
        }
        if (value.contains("rotation")) {
            const auto& rotation = value.at("rotation");
            local.rotation = math::Quat{rotation[3].get<double>(), rotation[0].get<double>(),
                                        rotation[1].get<double>(), rotation[2].get<double>()};
        }
        return take(canonicalize_transform(local));
    }
    GltfGeometry run() {
        poll();
        result.selected_scene = source.result.selected_scene;
        if (settings.scene)
            result.selected_scene = index(Json(*settings.scene), source.result.counts.scenes);
        charge(sizeof(GltfGeometry));
        charge(multiply(gltf_source_maximum_diagnostics,
                        sizeof(std::string) + gltf_source_maximum_diagnostic_bytes));
        for (const auto& resource : source.result.resources) {
            charge(sizeof(GltfResourceReference) + resource.mime_type.size() +
                   (resource.path ? resource.path->text().size() : 0));
        }
        if (!result.selected_scene) {
            result.source = std::move(source.result);
            return std::move(result);
        }
        struct Pending {
            std::size_t source;
            std::optional<std::size_t> parent;
        };
        const auto& nodes = array(source.document, "nodes");
        const auto& roots = array(source.document, "scenes")[*result.selected_scene];
        std::vector<Pending> stack;
        for (auto iterator = array(roots, "nodes").rbegin();
             iterator != array(roots, "nodes").rend(); ++iterator)
            stack.push_back({integer(*iterator), {}});
        std::set<std::size_t> meshes;
        while (!stack.empty()) {
            poll(result.nodes.size());
            const auto pending = stack.back();
            stack.pop_back();
            field = "nodes[" + std::to_string(pending.source) + "]";
            const auto& value = nodes[pending.source];
            charge(sizeof(GltfGeometryNode) + sizeof(std::size_t));
            work(1);
            GltfGeometryNode node;
            node.source_node = pending.source;
            node.name = name(value);
            node.parent = pending.parent;
            node.local = local_transform(value);
            node.local_matrix = take(transform_matrix(node.local));
            node.world_matrix = pending.parent
                                    ? result.nodes[*pending.parent].world_matrix * node.local_matrix
                                    : node.local_matrix;
            take(validate_transform_matrix(node.world_matrix));
            const auto inverse = take(inverse_transform_matrix(node.world_matrix));
            for (int column = 0; column < 3; ++column)
                for (int row = 0; row < 3; ++row)
                    node.normal_matrix[static_cast<std::size_t>(column * 3 + row)] =
                        inverse[row][column];
            glm::dmat3 normalized{1.0};
            for (int column = 0; column < 3; ++column)
                normalized[column] = unit(math::Vec3{node.world_matrix[column]});
            node.mirrored = glm::determinant(normalized) < 0.0;
            if (value.contains("mesh")) {
                node.mesh = integer(value.at("mesh"));
                meshes.insert(*node.mesh);
            }
            const auto output = result.nodes.size();
            if (pending.parent)
                result.nodes[*pending.parent].children.push_back(output);
            else
                result.roots.push_back(output);
            result.nodes.push_back(std::move(node));
            const auto& children = array(value, "children");
            for (auto iterator = children.rbegin(); iterator != children.rend(); ++iterator)
                stack.push_back({integer(*iterator), output});
        }
        std::map<std::size_t, std::size_t> mesh_indices;
        const auto& source_meshes = array(source.document, "meshes");
        for (const auto mesh_index : meshes) {
            poll();
            field = "meshes[" + std::to_string(mesh_index) + "]";
            const auto& value = source_meshes[mesh_index];
            charge(sizeof(GltfGeometryMesh));
            GltfGeometryMesh mesh;
            mesh.source_mesh = mesh_index;
            mesh.name = name(value);
            std::optional<GltfGeometryBounds> bounds;
            const auto& primitives = array(value, "primitives");
            charge(multiply(primitives.size(), sizeof(std::size_t)));
            for (std::size_t i = 0; i < primitives.size(); ++i) {
                auto converted = primitive(primitives[i], mesh_index, i);
                include(bounds, converted.bounds);
                mesh.primitives.push_back(result.primitives.size());
                result.primitives.push_back(std::move(converted));
            }
            mesh.bounds = *bounds;
            mesh_indices.emplace(mesh_index, result.meshes.size());
            result.meshes.push_back(std::move(mesh));
        }
        for (auto& node : result.nodes) {
            poll();
            if (node.mesh) {
                field = "nodes[" + std::to_string(node.source_node) + "]";
                node.mesh = mesh_indices.at(*node.mesh);
                node.world_bounds =
                    transformed(result.meshes[*node.mesh].bounds, node.world_matrix);
                include(result.world_bounds, *node.world_bounds);
            }
        }
        poll();
        // Diagnostics added during conversion belong to this same owned report.
        // Their fixed inherited limits also bound their output storage.
        result.source = std::move(source.result);
        return std::move(result);
    }
};

struct TextureConversion {
    Inspection& source;
    const GltfTextureSettings& settings;
    std::string& field;
    GltfTextures output{};
    std::size_t output_bytes{};
    std::size_t mip_bytes{};
    std::size_t decoded_bytes{};
    std::size_t work_items{};
    std::map<std::size_t, detail::GltfImageInfo> headers{};
    std::map<std::pair<std::size_t, GltfTextureSemantic>, std::size_t> variants{};

    struct Slot {
        const char* name;
        GltfTextureRole role;
        GltfTextureSemantic semantic;
        bool pbr;
    };
    static constexpr std::array slots{
        Slot{"baseColorTexture", GltfTextureRole::base_color, GltfTextureSemantic::srgb_color,
             true},
        Slot{"metallicRoughnessTexture", GltfTextureRole::metallic_roughness,
             GltfTextureSemantic::linear_data, true},
        Slot{"normalTexture", GltfTextureRole::normal, GltfTextureSemantic::normal, false},
        Slot{"occlusionTexture", GltfTextureRole::occlusion, GltfTextureSemantic::linear_data,
             false},
        Slot{"emissiveTexture", GltfTextureRole::emissive, GltfTextureSemantic::srgb_color, false}};

    void poll() const {
        cancelled(source.input.stop);
    }
    void charge(std::size_t bytes) {
        if (bytes > gltf_texture_maximum_output_bytes - output_bytes)
            fail("glTF texture output exceeds its metadata/byte bound");
        output_bytes += bytes;
    }
    void work(std::size_t count) {
        if (count > gltf_texture_maximum_work - work_items)
            fail("glTF texture conversion exceeds its work bound");
        work_items += count;
    }
    const Json* info(const Json& material, const Slot& slot) const {
        const Json* parent = &material;
        if (slot.pbr) {
            if (!material.contains("pbrMetallicRoughness"))
                return nullptr;
            parent = &material.at("pbrMetallicRoughness");
            object(*parent);
        }
        if (!parent->contains(slot.name))
            return nullptr;
        const auto& value = parent->at(slot.name);
        object(value);
        return &value;
    }
    std::set<std::size_t> material_inventory() {
        std::set<std::size_t> materials;
        if (!output.selected_scene)
            return materials;
        const auto& scene = array(source.document, "scenes")[*output.selected_scene];
        std::vector<std::size_t> pending;
        for (const auto& root : array(scene, "nodes"))
            pending.push_back(integer(root));
        const auto& nodes = array(source.document, "nodes");
        std::set<std::size_t> meshes;
        while (!pending.empty()) {
            poll();
            const auto position = pending.back();
            pending.pop_back();
            const auto& node = nodes[position];
            if (node.contains("mesh"))
                meshes.insert(integer(node.at("mesh")));
            for (const auto& child : array(node, "children"))
                pending.push_back(integer(child));
        }
        for (const auto mesh : meshes) {
            const auto& primitives = array(array(source.document, "meshes")[mesh], "primitives");
            for (const auto& primitive : primitives) {
                poll();
                if (!primitive.contains("material"))
                    continue;
                const auto position =
                    index(primitive.at("material"), source.result.counts.materials);
                const auto& material = array(source.document, "materials")[position];
                field = "materials[" + std::to_string(position) + "]";
                for (const auto& slot : slots) {
                    const auto* reference = info(material, slot);
                    if (!reference)
                        continue;
                    const auto uv = optional_integer(*reference, "texCoord");
                    if (uv > 1 ||
                        !primitive.at("attributes").contains(uv == 0 ? "TEXCOORD_0" : "TEXCOORD_1"))
                        fail("glTF texture use references an unavailable supported UV set");
                }
                materials.insert(position);
            }
        }
        return materials;
    }
    static GltfTextureWrap wrap(std::size_t value) {
        switch (value) {
        case 33071:
            return GltfTextureWrap::clamp_to_edge;
        case 33648:
            return GltfTextureWrap::mirrored_repeat;
        case 10497:
            return GltfTextureWrap::repeat;
        default:
            fail("Invalid glTF sampler wrap value");
        }
    }
    GltfTextureSampler sampler(const Json& texture) const {
        GltfTextureSampler converted;
        if (!texture.contains("sampler"))
            return converted;
        const auto& value =
            array(source.document,
                  "samplers")[index(texture.at("sampler"), source.result.counts.samplers)];
        converted.wrap_s = wrap(optional_integer(value, "wrapS", 10497));
        converted.wrap_t = wrap(optional_integer(value, "wrapT", 10497));
        const auto mag = optional_integer(value, "magFilter", 9729);
        if (mag != 9728 && mag != 9729)
            fail("Invalid glTF magnification filter");
        converted.magnification =
            mag == 9728 ? GltfTextureFilter::nearest : GltfTextureFilter::linear;
        switch (optional_integer(value, "minFilter", 9987)) {
        case 9728:
            converted.minification = GltfTextureFilter::nearest;
            converted.mip = GltfTextureMipFilter::none;
            break;
        case 9729:
            converted.minification = GltfTextureFilter::linear;
            converted.mip = GltfTextureMipFilter::none;
            break;
        case 9984:
            converted.minification = GltfTextureFilter::nearest;
            converted.mip = GltfTextureMipFilter::nearest;
            break;
        case 9985:
            converted.minification = GltfTextureFilter::linear;
            converted.mip = GltfTextureMipFilter::nearest;
            break;
        case 9986:
            converted.minification = GltfTextureFilter::nearest;
            converted.mip = GltfTextureMipFilter::linear;
            break;
        case 9987:
            converted.minification = GltfTextureFilter::linear;
            converted.mip = GltfTextureMipFilter::linear;
            break;
        default:
            fail("Invalid glTF minification filter");
        }
        return converted;
    }
    std::size_t image_variant(std::size_t image, GltfTextureSemantic semantic) {
        const auto key = std::pair{image, semantic};
        if (const auto found = variants.find(key); found != variants.end())
            return found->second;
        field = "images[" + std::to_string(image) + "]";
        poll();
        if (variants.size() >= gltf_texture_maximum_variants)
            fail("glTF texture semantic variants exceed their bound");
        auto found = headers.find(image);
        if (found == headers.end()) {
            const auto& encoded = source.images[image];
            const auto header =
                take(detail::inspect_gltf_image(encoded.bytes, encoded.mime, source.input.stop));
            if (header.decoded_bytes > gltf_texture_maximum_decoded_bytes - decoded_bytes)
                fail("glTF decoded image inventory exceeds its aggregate byte bound");
            decoded_bytes += header.decoded_bytes;
            work(header.work_items);
            found = headers.emplace(image, header).first;
        }
        const auto& header = found->second;
        auto width = static_cast<std::size_t>(header.width);
        auto height = static_cast<std::size_t>(header.height);
        std::size_t levels = 0;
        std::size_t bytes = 0;
        while (true) {
            ++levels;
            bytes = add(bytes, multiply(multiply(width, height), 4 * (header.component_bits / 8)));
            if (width == 1 && height == 1)
                break;
            const auto next_width = std::max<std::size_t>(1, width / 2);
            const auto next_height = std::max<std::size_t>(1, height / 2);
            work(multiply(add(width, next_width) - std::gcd(width, next_width),
                          add(height, next_height) - std::gcd(height, next_height)));
            width = next_width;
            height = next_height;
        }
        if (bytes > gltf_texture_maximum_mip_bytes - mip_bytes)
            fail("glTF texture mip inventory exceeds its aggregate byte bound");
        mip_bytes += bytes;
        charge(add(sizeof(GltfTextureImage), add(bytes, multiply(levels, sizeof(GltfTextureMip)))));
        const auto dense = output.images.size();
        variants.emplace(key, dense);
        GltfTextureImage converted;
        converted.source_image = image;
        converted.semantic = semantic;
        converted.component_bits = header.component_bits;
        converted.mips.reserve(levels);
        output.images.push_back(std::move(converted));
        return dense;
    }
    void plan(const std::set<std::size_t>& materials) {
        for (const auto material_index : materials) {
            const auto& material = array(source.document, "materials")[material_index];
            for (const auto& slot : slots) {
                poll();
                const auto* reference = info(material, slot);
                if (!reference)
                    continue;
                field = "materials[" + std::to_string(material_index) + "]." + slot.name;
                if (output.usages.size() >= gltf_texture_maximum_usages)
                    fail("glTF texture uses exceed their bound");
                charge(sizeof(GltfTextureUsage));
                const auto texture_index =
                    index(reference->at("index"), source.result.counts.textures);
                const auto& texture = array(source.document, "textures")[texture_index];
                const auto image = index(texture.at("source"), source.images.size());
                const auto dense = image_variant(image, slot.semantic);
                output.usages.push_back({material_index, slot.role, texture_index,
                                         optional_integer(*reference, "texCoord"), dense,
                                         sampler(texture)});
            }
        }
    }
    static double to_linear(double encoded) {
        return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
    }
    static double to_srgb(double linear) {
        return linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    }
    static double sample(const GltfTextureMip& mip, unsigned bits, std::size_t pixel,
                         std::size_t channel) {
        const auto offset = (pixel * 4 + channel) * (bits / 8);
        const auto value = little(mip.rgba, offset, bits / 8);
        return static_cast<double>(value) / (bits == 16 ? 65535.0 : 255.0);
    }
    static void store(GltfTextureMip& mip, unsigned bits, std::size_t pixel, std::size_t channel,
                      double value) {
        const auto maximum = bits == 16 ? 65535U : 255U;
        const auto integer_value =
            static_cast<std::uint32_t>(std::floor(std::clamp(value, 0.0, 1.0) * maximum + 0.5));
        const auto offset = (pixel * 4 + channel) * (bits / 8);
        mip.rgba[offset] = static_cast<std::byte>(integer_value & 255U);
        if (bits == 16)
            mip.rgba[offset + 1] = static_cast<std::byte>(integer_value >> 8U);
    }
    GltfTextureMip next_mip(const GltfTextureMip& previous, unsigned bits,
                            GltfTextureSemantic semantic) const {
        GltfTextureMip next;
        const auto width = static_cast<std::size_t>(previous.width);
        const auto height = static_cast<std::size_t>(previous.height);
        const auto target_width = std::max<std::size_t>(1, width / 2);
        const auto target_height = std::max<std::size_t>(1, height / 2);
        next.width = static_cast<std::uint32_t>(target_width);
        next.height = static_cast<std::uint32_t>(target_height);
        next.rgba.resize(target_width * target_height * 4 * (bits / 8));
        for (std::size_t y = 0; y < target_height; ++y) {
            poll();
            const auto y0 = y * height;
            const auto y1 = (y + 1) * height;
            for (std::size_t x = 0; x < target_width; ++x) {
                const auto x0 = x * width;
                const auto x1 = (x + 1) * width;
                std::array<double, 4> sum{};
                std::array<std::uint64_t, 4> exact_sum{};
                for (std::size_t sy = y0 / target_height;
                     sy < (y1 + target_height - 1) / target_height; ++sy) {
                    const auto wy =
                        std::min(y1, (sy + 1) * target_height) - std::max(y0, sy * target_height);
                    for (std::size_t sx = x0 / target_width;
                         sx < (x1 + target_width - 1) / target_width; ++sx) {
                        const auto wx =
                            std::min(x1, (sx + 1) * target_width) - std::max(x0, sx * target_width);
                        const auto weight = static_cast<double>(wx * wy);
                        for (std::size_t channel = 0; channel < 4; ++channel) {
                            if (channel == 3 || semantic == GltfTextureSemantic::linear_data) {
                                const auto offset = ((sy * width + sx) * 4 + channel) * (bits / 8);
                                exact_sum[channel] += static_cast<std::uint64_t>(
                                                          little(previous.rgba, offset, bits / 8)) *
                                                      (wx * wy);
                                continue;
                            }
                            auto value = sample(previous, bits, sy * width + sx, channel);
                            if (channel != 3 && semantic == GltfTextureSemantic::srgb_color)
                                value = to_linear(value);
                            else if (channel != 3 && semantic == GltfTextureSemantic::normal)
                                value = value * 2.0 - 1.0;
                            sum[channel] += value * weight;
                        }
                    }
                }
                for (auto& value : sum)
                    value /= static_cast<double>(width * height);
                if (semantic == GltfTextureSemantic::normal) {
                    const auto length = std::hypot(sum[0], sum[1], sum[2]);
                    if (length <= 1e-12) {
                        sum[0] = 0.0;
                        sum[1] = 0.0;
                        sum[2] = 1.0;
                    } else {
                        for (std::size_t channel = 0; channel < 3; ++channel)
                            sum[channel] /= length;
                    }
                    for (std::size_t channel = 0; channel < 3; ++channel)
                        sum[channel] = sum[channel] * 0.5 + 0.5;
                } else if (semantic == GltfTextureSemantic::srgb_color) {
                    for (std::size_t channel = 0; channel < 3; ++channel)
                        sum[channel] = to_srgb(sum[channel]);
                }
                for (std::size_t channel = 0; channel < 4; ++channel) {
                    if (channel == 3 || semantic == GltfTextureSemantic::linear_data) {
                        const auto denominator = static_cast<std::uint64_t>(width * height);
                        const auto rounded = (exact_sum[channel] + denominator / 2) / denominator;
                        const auto offset = ((y * target_width + x) * 4 + channel) * (bits / 8);
                        next.rgba[offset] = static_cast<std::byte>(rounded & 255U);
                        if (bits == 16)
                            next.rgba[offset + 1] = static_cast<std::byte>(rounded >> 8U);
                    } else {
                        store(next, bits, y * target_width + x, channel, sum[channel]);
                    }
                }
            }
        }
        return next;
    }
    GltfTextures run() {
        poll();
        output.selected_scene = source.result.selected_scene;
        if (settings.scene)
            output.selected_scene = index(Json(*settings.scene), source.result.counts.scenes);
        charge(sizeof(GltfTextures));
        charge(multiply(gltf_source_maximum_diagnostics,
                        sizeof(std::string) + gltf_source_maximum_diagnostic_bytes));
        for (const auto& resource : source.result.resources)
            charge(sizeof(GltfResourceReference) + resource.mime_type.size() +
                   (resource.path ? resource.path->text().size() : 0));
        plan(material_inventory());
        // Every header, semantic variant and mip/work budget is accepted before
        // the first decoder allocation. Reused source images decode only once.
        detail::GltfImageDecodeBudget budget;
        std::map<std::size_t, detail::GltfDecodedImage> decoded;
        for (const auto& [image, header] : headers) {
            field = "images[" + std::to_string(image) + "]";
            poll();
            const auto& encoded = source.images[image];
            decoded.emplace(image,
                            take(detail::decode_gltf_image(encoded.bytes, encoded.mime, header,
                                                           budget, source.input.stop)));
        }
        for (auto& image : output.images) {
            field = "images[" + std::to_string(image.source_image) + "]";
            poll();
            const auto& original = decoded.at(image.source_image);
            image.mips.push_back({original.info.width, original.info.height, original.rgba});
            while (image.mips.back().width != 1 || image.mips.back().height != 1)
                image.mips.push_back(
                    next_mip(image.mips.back(), image.component_bits, image.semantic));
        }
        poll();
        output.source = std::move(source.result);
        return std::move(output);
    }
};

void precharge(const GltfSourceInput& input) {
    cancelled(input.stop);
    if (input.bytes.empty() || input.bytes.size() > gltf_source_maximum_file_bytes ||
        input.external_sources.size() > gltf_source_maximum_resources)
        fail("glTF source inventory exceeds its bound");
    std::size_t bytes = input.bytes.size();
    std::set<std::string> paths{folded(input.path.text())};
    std::set<AssetId> identities;
    for (const auto& external : input.external_sources) {
        if (external.bytes.empty() || external.bytes.size() > gltf_source_maximum_file_bytes ||
            external.bytes.size() > gltf_source_maximum_total_bytes - bytes)
            fail("glTF external source inventory exceeds its byte bound");
        bytes += external.bytes.size();
        if (!paths.insert(folded(external.path.text())).second ||
            !identities.insert(external.asset).second)
            fail("Duplicate or case-colliding glTF external path/identity", ErrorCode::conflict);
    }
}
} // namespace

namespace detail {
void gltf_fail_allocation(std::optional<std::size_t> position) noexcept {
    failed_allocation = position;
    allocation_attempts = 0;
}
std::size_t gltf_allocation_attempts() noexcept {
    return allocation_attempts;
}
std::size_t gltf_live_allocations() noexcept {
    return live_allocations;
}
} // namespace detail

Result<GltfSourceInspection> inspect_gltf_source(const GltfSourceInput& input) {
    try {
        precharge(input);
        return Inspection{input}.run();
    } catch (Failure& failure) {
        failure.error.context.operation = "asset.gltf.inspect";
        if (failure.error.context.path.empty())
            failure.error.context.path = input.path.text();
        return std::move(failure.error);
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::execution_failed,
                     "glTF inspection allocation failed",
                     {.operation = "asset.gltf.inspect", .path = input.path.text()}};
    } catch (const Json::exception&) {
        return Error{ErrorCode::invalid_configuration,
                     "Malformed glTF source fields",
                     {.operation = "asset.gltf.inspect", .path = input.path.text()}};
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "glTF source inspection failed",
                     {.operation = "asset.gltf.inspect", .path = input.path.text()}};
    }
}

Result<GltfGeometry> convert_gltf_geometry(const GltfSourceInput& input,
                                           const GltfGeometrySettings& settings) {
    std::string field;
    try {
        precharge(input);
        Inspection source{input};
        source.prepare();
        return GeometryConversion{source, settings, field}.run();
    } catch (Failure& failure) {
        failure.error.context.operation = "asset.gltf.convert";
        if (failure.error.context.path.empty())
            failure.error.context.path = input.path.text();
        if (!field.empty())
            failure.error.message = field + ": " + failure.error.message;
        return std::move(failure.error);
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::execution_failed,
                     "glTF geometry allocation failed",
                     {.operation = "asset.gltf.convert", .path = input.path.text()}};
    } catch (const Json::exception&) {
        return Error{ErrorCode::invalid_configuration,
                     "Malformed glTF geometry fields: " + field,
                     {.operation = "asset.gltf.convert", .path = input.path.text()}};
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "glTF geometry conversion failed: " + field,
                     {.operation = "asset.gltf.convert", .path = input.path.text()}};
    }
}

Result<GltfTextures> convert_gltf_textures(const GltfSourceInput& input,
                                           const GltfTextureSettings& settings) {
    std::string field;
    try {
        precharge(input);
        Inspection source{input};
        source.prepare();
        return TextureConversion{source, settings, field}.run();
    } catch (Failure& failure) {
        failure.error.context.operation = "asset.gltf.textures";
        if (failure.error.context.path.empty())
            failure.error.context.path = input.path.text();
        if (!field.empty())
            failure.error.message = field + ": " + failure.error.message;
        return std::move(failure.error);
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::execution_failed,
                     "glTF texture allocation failed",
                     {.operation = "asset.gltf.textures", .path = input.path.text()}};
    } catch (const Json::exception&) {
        return Error{ErrorCode::invalid_configuration,
                     "Malformed glTF texture fields: " + field,
                     {.operation = "asset.gltf.textures", .path = input.path.text()}};
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "glTF texture conversion failed: " + field,
                     {.operation = "asset.gltf.textures", .path = input.path.text()}};
    }
}
} // namespace shark::tools
