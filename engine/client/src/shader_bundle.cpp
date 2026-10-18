#include <shark/client/shader_bundle.hpp>

#include "shader_digest.hpp"
#include "shader_path.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace shark::client {
namespace {
constexpr std::size_t header_size = 88;
constexpr std::size_t shader_limit = 4U * 1024U * 1024U;
constexpr std::size_t bundle_limit = header_size + shader_limit * 2;
constexpr std::array<std::byte, 8> magic{std::byte{'S'}, std::byte{'H'}, std::byte{'R'},
                                         std::byte{'K'}, std::byte{'S'}, std::byte{'H'},
                                         std::byte{'D'}, std::byte{0}};

Error invalid_bundle(std::string message) {
    return {ErrorCode::invalid_argument, std::move(message), {"shader.bundle.decode"}};
}

std::uint32_t little_u32(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    std::uint32_t result = 0;
    for (std::uint32_t index = 0; index < 4; ++index) {
        result |= std::to_integer<std::uint32_t>(bytes[offset + index]) << (index * 8U);
    }
    return result;
}
} // namespace

Result<ShaderBundle> decode_shader_bundle(std::span<const std::byte> bytes) {
    if (bytes.size() < header_size || bytes.size() > bundle_limit ||
        !std::equal(magic.begin(), magic.end(), bytes.begin())) {
        return invalid_bundle("Shader bundle magic or byte length is invalid");
    }
    if (little_u32(bytes, 8) != 1 || little_u32(bytes, 20) != 0) {
        return invalid_bundle("Unsupported shader bundle version or reserved fields");
    }
    const auto vertex_size = little_u32(bytes, 12);
    const auto pixel_size = little_u32(bytes, 16);
    for (const auto size : {vertex_size, pixel_size}) {
        if (size < 20 || size > shader_limit || size % 4 != 0) {
            return invalid_bundle("Shader bundle payload length is outside supported bounds");
        }
    }
    if (bytes.size() != header_size + static_cast<std::size_t>(vertex_size) + pixel_size) {
        return invalid_bundle("Shader bundle must contain exactly its two declared payloads");
    }
    const auto vertex_bytes = bytes.subspan(header_size, vertex_size);
    const auto pixel_bytes = bytes.subspan(header_size + vertex_size, pixel_size);
    const auto vertex_hash = detail::shader_sha256(vertex_bytes);
    const auto pixel_hash = detail::shader_sha256(pixel_bytes);
    if (!std::equal(vertex_hash.begin(), vertex_hash.end(), bytes.begin() + 24) ||
        !std::equal(pixel_hash.begin(), pixel_hash.end(), bytes.begin() + 56)) {
        return invalid_bundle("Shader bundle payload SHA-256 does not match its header");
    }
    auto vertex = decode_shader_artifact(vertex_bytes, ShaderStage::vertex);
    if (!vertex) {
        return vertex.error();
    }
    auto pixel = decode_shader_artifact(pixel_bytes, ShaderStage::pixel);
    if (!pixel) {
        return pixel.error();
    }
    return ShaderBundle{std::move(vertex).value(), std::move(pixel).value(),
                        detail::shader_digest_text(detail::shader_sha256(bytes))};
}

Result<ShaderBundle> load_shader_bundle(const std::filesystem::path& path) {
    auto path_text = detail::artifact_path_text(path);
    if (!path_text) {
        auto error = path_text.error();
        error.context.operation = "shader.bundle.load";
        return error;
    }
    const auto fail = [&](ErrorCode code, std::string message) -> Result<ShaderBundle> {
        return Error{code, std::move(message), {"shader.bundle.load", {}, path_text.value()}};
    };
    std::error_code status;
    if (!std::filesystem::is_regular_file(path, status)) {
        return fail(ErrorCode::io_failure, "Shader bundle must be an accessible regular file");
    }
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return fail(ErrorCode::io_failure, "Cannot open shader bundle");
    }
    const auto size = stream.tellg();
    if (size < static_cast<std::streamoff>(header_size) ||
        size > static_cast<std::streamoff>(bundle_limit)) {
        return fail(ErrorCode::invalid_argument, "Shader bundle byte length is outside bounds");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)) ||
        stream.peek() != std::char_traits<char>::eof()) {
        return fail(ErrorCode::io_failure, "Cannot read a stable complete shader bundle");
    }
    auto bundle = decode_shader_bundle(bytes);
    if (!bundle) {
        auto error = bundle.error();
        error.context.path = path_text.value();
        return error;
    }
    return bundle;
}

Result<std::filesystem::path> default_shader_bundle_path() {
    const auto fail = []() -> Result<std::filesystem::path> {
        return Error{ErrorCode::io_failure,
                     "Cannot resolve a bounded executable path for the shader bundle",
                     {"shader.bundle.locate"}};
    };
    std::filesystem::path executable;
#if defined(_WIN32)
    std::array<wchar_t, 4097> buffer{};
    const auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0 || size >= buffer.size()) {
        return fail();
    }
    executable = std::wstring(buffer.data(), size);
#else
    std::error_code error;
    executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error || !executable.is_absolute()) {
        return fail();
    }
#endif
    auto result = executable.parent_path() / "assets" / "cube.sharkshader";
    if (!detail::artifact_path_text(result)) {
        return fail();
    }
    return result;
}

} // namespace shark::client
