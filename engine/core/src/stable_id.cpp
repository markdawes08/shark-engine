#include "shark/stable_id.hpp"

#include <algorithm>
#include <cstdint>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <bcrypt.h>
#elif defined(__linux__)
#include <cerrno>
#include <sys/random.h>
#else
#error Shark stable IDs require an implemented OS entropy source.
#endif

namespace shark {
namespace {
constexpr std::string_view digits = "0123456789abcdef";

Error invalid_id(ErrorContext context) {
    if (context.operation.empty()) {
        context.operation = "stable_id.parse";
    }
    return {ErrorCode::invalid_id, "expected a lowercase canonical, non-nil UUIDv4",
            std::move(context)};
}

class OsEntropy final : public IdEntropy {
  public:
    Result<void> fill(std::span<std::byte, 16> bytes) override {
#if defined(_WIN32)
        const auto status =
            BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(bytes.data()),
                            static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0) {
            return Error{ErrorCode::entropy_unavailable,
                         "OS entropy generation failed",
                         {.operation = "stable_id.generate"}};
        }
#elif defined(__linux__)
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto count = getrandom(bytes.data() + offset, bytes.size() - offset, 0);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                return Error{ErrorCode::entropy_unavailable,
                             "OS entropy generation failed",
                             {.operation = "stable_id.generate"}};
            }
            offset += static_cast<std::size_t>(count);
        }
#endif
        return {};
    }
};
} // namespace

StableId::StableId(std::array<std::byte, 16> bytes) noexcept : bytes_(bytes) {}

Result<StableId> StableId::parse(std::string_view text, ErrorContext context) {
    if (text.size() != 36) {
        return invalid_id(std::move(context));
    }
    std::array<std::byte, 16> bytes{};
    std::size_t source = 0;
    for (std::size_t destination = 0; destination < bytes.size(); ++destination) {
        if (source == 8 || source == 13 || source == 18 || source == 23) {
            if (text[source++] != '-') {
                return invalid_id(std::move(context));
            }
        }
        const auto high = digits.find(text[source++]);
        const auto low = digits.find(text[source++]);
        if (high == std::string_view::npos || low == std::string_view::npos) {
            return invalid_id(std::move(context));
        }
        bytes[destination] = static_cast<std::byte>((high << 4U) | low);
    }
    if ((bytes[6] & std::byte{0xf0}) != std::byte{0x40} ||
        (bytes[8] & std::byte{0xc0}) != std::byte{0x80}) {
        return invalid_id(std::move(context));
    }
    return StableId{bytes};
}

std::string StableId::to_string() const {
    std::string text;
    text.reserve(36);
    for (std::size_t index = 0; index < bytes_.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            text.push_back('-');
        }
        const auto value = std::to_integer<unsigned int>(bytes_[index]);
        text.push_back(digits[value >> 4U]);
        text.push_back(digits[value & 0x0fU]);
    }
    return text;
}

const std::array<std::byte, 16>& StableId::bytes() const noexcept {
    return bytes_;
}

Result<StableId> generate_stable_id() {
    OsEntropy entropy;
    return generate_stable_id(entropy);
}

Result<StableId> generate_stable_id(IdEntropy& entropy) {
    std::array<std::byte, 16> bytes{};
    auto result = entropy.fill(bytes);
    if (!result) {
        auto error = result.error();
        error.code = ErrorCode::entropy_unavailable;
        if (error.context.operation.empty()) {
            error.context.operation = "stable_id.generate";
        }
        return error;
    }
    bytes[6] = (bytes[6] & std::byte{0x0f}) | std::byte{0x40};
    bytes[8] = (bytes[8] & std::byte{0x3f}) | std::byte{0x80};
    // Encoding through the single validated constructor keeps injected and OS
    // generation subject to exactly the same public identity invariant.
    std::string text;
    text.reserve(36);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            text.push_back('-');
        }
        const auto value = std::to_integer<unsigned int>(bytes[index]);
        text.push_back(digits[value >> 4U]);
        text.push_back(digits[value & 0x0fU]);
    }
    return StableId::parse(text, {.operation = "stable_id.generate"});
}

} // namespace shark
