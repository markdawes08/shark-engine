#include "asset_cache_format.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <tuple>
#include <type_traits>

namespace shark::tools {
namespace {
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
template <typename T, typename Function>
Result<T> boundary(const char* operation, Function&& function) {
    try {
        if constexpr (std::is_void_v<std::invoke_result_t<Function>>) {
            function();
            return {};
        } else
            return function();
    } catch (Failure& error) {
        error.error.context.operation = operation;
        return std::move(error.error);
    } catch (...) {
        return Error{
            ErrorCode::execution_failed, "Development cache operation failed", {operation}};
    }
}
std::span<const std::byte> bytes(std::string_view value) {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}
void raw(std::string& output, std::span<const std::byte> value) {
    output.append(reinterpret_cast<const char*>(value.data()), value.size());
}
void integer(std::string& output, std::uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i)
        output.push_back(static_cast<char>((value >> (8U * i)) & 255U));
}
std::uint64_t read_integer(std::string_view input, std::size_t offset, unsigned count) {
    if (offset > input.size() || count > input.size() - offset)
        fail("Truncated development payload");
    std::uint64_t value = 0;
    for (unsigned i = 0; i < count; ++i)
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(input[offset + i]))
                 << (8U * i);
    return value;
}
void text(std::string& output, std::string_view value) {
    integer(output, value.size(), 8);
    output.append(value);
}
bool token(std::string_view value) {
    return !value.empty() && value.size() <= 64 && value.front() >= 'a' && value.front() <= 'z' &&
           std::all_of(value.begin(), value.end(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                      c == '-';
           });
}
bool compatible(AssetAudience asset, AssetAudience side) {
    return asset == AssetAudience::shared || asset == side;
}
bool binding_less(const detail::CacheBinding& a, const detail::CacheBinding& b) {
    return std::tie(a.asset, a.target.id, a.target.side) <
           std::tie(b.asset, b.target.id, b.target.side);
}

void validate(const detail::CacheState& state) {
    if (state.artifacts.size() > asset_cache_maximum_entries ||
        state.bindings.size() > asset_cache_maximum_entries)
        fail("Cache entry bound exceeded");
    std::size_t payload_bytes = 0;
    std::map<AssetCacheKey, const AssetArtifact*> artifacts;
    for (const auto& a : state.artifacts) {
        take(detail::validate_target(a.target));
        if (!token(a.kind) || !token(a.importer) || a.importer_version == 0 ||
            a.payload_version == 0 || !compatible(a.audience, a.target.side) || a.payload.empty() ||
            a.payload.size() > asset_import_maximum_payload_bytes ||
            a.payload.size() > asset_import_maximum_total_bytes - payload_bytes ||
            a.dependencies.size() > asset_maximum_dependencies)
            fail("Invalid cached artifact metadata or byte bound");
        payload_bytes += a.payload.size();
        if (!artifacts.emplace(a.key, &a).second)
            fail("Duplicate cached artifact key");
        if (take(sha256(a.payload)) != a.payload_digest ||
            take(detail::import_key(state.project, a)) != a.key)
            fail("Cached artifact digest/key mismatch");
        std::optional<AssetId> previous;
        for (const auto& d : a.dependencies) {
            if (d.asset == a.asset || (previous && d.asset <= *previous))
                fail("Unordered, duplicate or self cache dependency");
            previous = d.asset;
        }
    }
    std::map<AssetCacheKey, unsigned> colors;
    auto visit = [&](auto&& self, const AssetArtifact& a, std::size_t depth) -> void {
        if (depth > asset_cache_maximum_entries || colors[a.key] == 1)
            fail("Cache dependency cycle");
        if (colors[a.key] == 2)
            return;
        colors[a.key] = 1;
        for (const auto& d : a.dependencies) {
            const auto found = artifacts.find(d.key);
            if (found == artifacts.end() || found->second->asset != d.asset ||
                found->second->target != a.target ||
                (a.audience == AssetAudience::shared &&
                 found->second->audience != AssetAudience::shared))
                fail("Missing or cross-side cached dependency");
            self(self, *found->second, depth + 1);
        }
        colors[a.key] = 2;
    };
    for (const auto& a : state.artifacts)
        visit(visit, a, 0);
    std::set<std::tuple<AssetId, std::string, AssetAudience>> bindings;
    for (const auto& b : state.bindings) {
        take(detail::validate_target(b.target));
        const auto found = artifacts.find(b.key);
        if (!bindings.emplace(b.asset, b.target.id, b.target.side).second ||
            found == artifacts.end() || found->second->asset != b.asset ||
            found->second->target != b.target)
            fail("Invalid cache binding");
    }
}
} // namespace

namespace detail {
Result<ContentDigest> hash_text(std::string_view text_value) {
    return sha256(bytes(text_value));
}
Result<void> validate_target(const AssetTargetProfile& profile) {
    if (!token(profile.id) ||
        (profile.side != AssetAudience::client && profile.side != AssetAudience::server)) {
        return Error{ErrorCode::invalid_argument,
                     "Target requires a lowercase ID and client/server side",
                     {"asset.target.validate"}};
    }
    return {};
}
Result<void> validate_importer_info(const AssetImporterInfo& info) {
    return boundary<void>("asset.importer.validate", [&] {
        if (!token(info.id) || !token(info.kind) || info.version == 0 ||
            info.payload_version == 0 || info.settings_schema_json.size() > 4096)
            fail("Invalid importer descriptor");
        static_cast<void>(take(canonical_import_object(info.settings_schema_json)));
    });
}
Result<AssetCacheKey> import_key(ProjectId project, const AssetArtifact& a) {
    return boundary<AssetCacheKey>("asset.cache.key", [&] {
        if (!token(a.kind) || !token(a.importer) || a.importer_version == 0 ||
            a.payload_version == 0 || a.dependencies.size() > asset_maximum_dependencies ||
            !compatible(a.audience, a.target.side))
            fail("Invalid cache key inputs");
        take(validate_target(a.target));
        std::string encoded{"shark.asset.import-key.v1"};
        integer(encoded, 1, 4);
        raw(encoded, project.value().bytes());
        raw(encoded, a.asset.value().bytes());
        text(encoded, a.kind);
        integer(encoded, static_cast<std::uint64_t>(a.audience), 4);
        raw(encoded, a.source_digest.bytes);
        raw(encoded, a.settings_digest.bytes);
        text(encoded, a.importer);
        integer(encoded, a.importer_version, 4);
        integer(encoded, a.payload_version, 4);
        text(encoded, a.target.id);
        integer(encoded, static_cast<std::uint64_t>(a.target.side), 4);
        integer(encoded, a.dependencies.size(), 4);
        std::optional<AssetId> previous;
        for (const auto& d : a.dependencies) {
            if (previous && d.asset <= *previous)
                fail("Unordered cache key dependencies");
            previous = d.asset;
            raw(encoded, d.asset.value().bytes());
            raw(encoded, d.key.bytes);
        }
        return take(hash_text(encoded));
    });
}
Result<std::string> encode_cache(const CacheState& input) {
    return boundary<std::string>("asset.cache.encode", [&] {
        validate(input);
        auto state = input;
        std::sort(state.artifacts.begin(), state.artifacts.end(),
                  [](const auto& a, const auto& b) { return a.key < b.key; });
        std::sort(state.bindings.begin(), state.bindings.end(), binding_less);
        CacheIndex index{state.project, {}, state.bindings};
        std::string payload;
        for (auto& a : state.artifacts) {
            const auto offset = payload.size();
            const auto length = a.payload.size();
            raw(payload, a.payload);
            a.payload.clear();
            index.entries.push_back({std::move(a), offset, length});
        }
        const auto json = take(encode_cache_index(index));
        if (json.size() > asset_cache_maximum_index_bytes)
            fail("Cache index byte bound exceeded");
        std::string body = json;
        body += payload;
        const auto digest = take(hash_text(body));
        std::string encoded{"SHKDEV1\0", 8};
        integer(encoded, 1, 4);
        integer(encoded, json.size(), 4);
        integer(encoded, payload.size(), 8);
        raw(encoded, digest.bytes);
        encoded += body;
        if (encoded.size() > asset_cache_maximum_file_bytes)
            fail("Cache snapshot byte bound exceeded");
        return encoded;
    });
}
Result<CacheState> decode_cache(std::string_view encoded, ProjectId project) {
    return boundary<CacheState>("asset.cache.decode", [&] {
        if (encoded.size() < 56 || encoded.size() > asset_cache_maximum_file_bytes ||
            encoded.substr(0, 8) != std::string_view{"SHKDEV1\0", 8} ||
            read_integer(encoded, 8, 4) != 1)
            fail("Unsupported or invalid development cache container");
        const auto index_bytes = read_integer(encoded, 12, 4);
        const auto payload_bytes = read_integer(encoded, 16, 8);
        if (index_bytes > asset_cache_maximum_index_bytes ||
            payload_bytes > asset_import_maximum_total_bytes ||
            index_bytes + payload_bytes != encoded.size() - 56)
            fail("Invalid cache container lengths");
        const auto digest = take(hash_text(encoded.substr(56)));
        if (!std::equal(digest.bytes.begin(), digest.bytes.end(),
                        bytes(encoded.substr(24, 32)).begin()))
            fail("Cache container digest mismatch");
        auto index =
            take(decode_cache_index(encoded.substr(56, static_cast<std::size_t>(index_bytes))));
        if (index.project != project)
            fail("Development cache belongs to a different project", ErrorCode::conflict);
        CacheState state{project, {}, std::move(index.bindings)};
        std::size_t offset = 0;
        std::optional<AssetCacheKey> previous;
        for (auto& entry : index.entries) {
            if ((previous && entry.artifact.key <= *previous) || entry.offset != offset ||
                entry.length == 0 || entry.length > asset_import_maximum_payload_bytes ||
                entry.length > payload_bytes - offset)
                fail("Invalid cache payload range/order");
            previous = entry.artifact.key;
            const auto source = bytes(
                encoded.substr(56 + static_cast<std::size_t>(index_bytes) + offset, entry.length));
            entry.artifact.payload.assign(source.begin(), source.end());
            state.artifacts.push_back(std::move(entry.artifact));
            offset += entry.length;
        }
        if (offset != payload_bytes ||
            !std::is_sorted(state.bindings.begin(), state.bindings.end(), binding_less))
            fail("Unreferenced cache bytes or unordered bindings");
        validate(state);
        return state;
    });
}
} // namespace detail

const AssetImporterInfo& ByteAssetImporter::info() const noexcept {
    static const AssetImporterInfo descriptor{
        "shark.bytes", 1, "bytes", 1,
        R"({"type":"object","properties":{"transform":{"type":"string","enum":["identity","uppercase-ascii"],"default":"identity"}},"additionalProperties":false})"};
    return descriptor;
}
Result<std::string> ByteAssetImporter::normalize_settings(std::string_view json) const {
    return detail::normalize_byte_settings(json);
}
Result<std::vector<std::byte>> ByteAssetImporter::import(const AssetImportInput& input) {
    return boundary<std::vector<std::byte>>("asset.bytes.import", [&] {
        if (input.kind != "bytes" || input.source.size() > asset_import_maximum_source_bytes)
            fail("Unsupported or oversized byte source");
        take(detail::validate_target(input.target));
        const auto settings = take(normalize_settings(input.settings_json));
        std::string payload{"SHKBYT01"};
        integer(payload, 1, 4);
        raw(payload, input.asset.value().bytes());
        integer(payload, input.source.size(), 8);
        const auto uppercase = settings == R"({"transform":"uppercase-ascii"})";
        for (auto b : input.source) {
            auto c = std::to_integer<unsigned>(b);
            if (uppercase && c >= 'a' && c <= 'z')
                c -= static_cast<unsigned>('a' - 'A');
            payload.push_back(static_cast<char>(c));
        }
        const auto view = bytes(payload);
        return std::vector<std::byte>{view.begin(), view.end()};
    });
}
Result<void> ByteAssetImporter::validate_payload(const AssetArtifact& artifact) const {
    return boundary<void>("asset.bytes.validate", [&] {
        if (artifact.kind != "bytes" || artifact.importer != "shark.bytes" ||
            artifact.importer_version != 1 || artifact.payload_version != 1 ||
            artifact.payload.size() < 36 ||
            artifact.payload.size() > asset_import_maximum_payload_bytes)
            fail("Invalid byte artifact descriptor");
        const std::string_view data{reinterpret_cast<const char*>(artifact.payload.data()),
                                    artifact.payload.size()};
        if (data.substr(0, 8) != "SHKBYT01" || read_integer(data, 8, 4) != 1 ||
            read_integer(data, 28, 8) != data.size() - 36 ||
            data.size() - 36 > asset_import_maximum_source_bytes ||
            !std::equal(artifact.asset.value().bytes().begin(),
                        artifact.asset.value().bytes().end(), artifact.payload.begin() + 12))
            fail("Malformed byte artifact payload");
    });
}

} // namespace shark::tools
