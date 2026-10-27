#include "shark/tools/gltf_source.hpp"

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
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <new>
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
        const auto& images = array(document, "images");
        for (std::size_t i = 0; i < images.size(); ++i) {
            cancelled(input.stop);
            const auto& image = images[i];
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
    GltfSourceInspection run() {
        tables();
        resources();
        accessor_tables();
        meshes();
        graph();
        parser();
        return std::move(result);
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
} // namespace shark::tools
