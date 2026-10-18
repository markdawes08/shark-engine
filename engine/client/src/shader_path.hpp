#pragma once

#include <shark/error.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace shark::client::detail {

// ProjectPath describes portable relative references. Artifact inputs accept
// native absolute paths; validate those before opening and owning diagnostics.
inline bool valid_native_path_encoding(const std::filesystem::path& path) {
    const auto& native = path.native();
    if (native.empty() || native.size() > 4096 ||
        native.find(std::filesystem::path::value_type{}) !=
            std::filesystem::path::string_type::npos) {
        return false;
    }
    for (std::size_t index = 0; index < native.size(); ++index) {
#if defined(_WIN32)
        const auto code = static_cast<std::uint32_t>(native[index]);
        if (code >= 0xd800 && code <= 0xdbff) {
            if (++index == native.size()) {
                return false;
            }
            const auto low = static_cast<std::uint32_t>(native[index]);
            if (low < 0xdc00 || low > 0xdfff) {
                return false;
            }
        } else if (code >= 0xdc00 && code <= 0xdfff) {
            return false;
        }
#else
        const auto first = static_cast<unsigned char>(native[index]);
        if (first < 0x80) {
            continue;
        }
        std::uint32_t remaining = 0;
        std::uint32_t code = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            remaining = 1;
            code = first & 0x1fU;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            remaining = 2;
            code = first & 0x0fU;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            remaining = 3;
            code = first & 0x07U;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (remaining >= native.size() - index) {
            return false;
        }
        for (std::uint32_t count = 0; count < remaining; ++count) {
            const auto next = static_cast<unsigned char>(native[++index]);
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            code = (code << 6U) | (next & 0x3fU);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
            return false;
        }
#endif
    }
    return true;
}

inline Result<std::string> artifact_path_text(const std::filesystem::path& path) {
    if (valid_native_path_encoding(path)) {
        try {
            const auto utf8 = path.u8string();
            if (utf8.size() <= 4096) {
                return std::string(utf8.begin(), utf8.end());
            }
        } catch (const std::filesystem::filesystem_error&) {
            // Invalid encoding never leaks into an owned error or open call.
        }
    }
    return Error{
        ErrorCode::invalid_path,
        "Shader path must be valid native Unicode without NUL and at most 4096 UTF-8 bytes",
        {"shader.load"}};
}

} // namespace shark::client::detail
