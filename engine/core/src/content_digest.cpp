#include "shark/content_digest.hpp"

#include "crypto_runtime.hpp"

#include <psa/crypto.h>

#include <mutex>

namespace shark {

std::string ContentDigest::to_hex() const {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto value : bytes) {
        const auto number = std::to_integer<unsigned>(value);
        result.push_back(digits[number >> 4U]);
        result.push_back(digits[number & 15U]);
    }
    return result;
}
Result<ContentDigest> ContentDigest::parse(std::string_view text) {
    constexpr std::string_view digits = "0123456789abcdef";
    if (text.size() == 64) {
        ContentDigest result;
        for (std::size_t i = 0; i < result.bytes.size(); ++i) {
            const auto high = digits.find(text[2 * i]);
            const auto low = digits.find(text[2 * i + 1]);
            if (high == std::string_view::npos || low == std::string_view::npos) {
                return Error{ErrorCode::invalid_argument,
                             "Digest requires lowercase SHA-256 hex",
                             {.operation = "content.digest.parse"}};
            }
            result.bytes[i] = static_cast<std::byte>((high << 4U) | low);
        }
        return result;
    }
    return Error{ErrorCode::invalid_argument,
                 "Digest requires 64 hexadecimal bytes",
                 {.operation = "content.digest.parse"}};
}
Result<ContentDigest> sha256(std::span<const std::byte> bytes) {
    std::lock_guard lock(detail::crypto_runtime().mutex);
    if (auto initialized = detail::initialize_crypto(); !initialized) {
        auto error = initialized.error();
        error.context.operation = "content.sha256";
        return error;
    }
    ContentDigest digest;
    std::size_t written = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const unsigned char*>(bytes.data()),
                         bytes.size(), reinterpret_cast<unsigned char*>(digest.bytes.data()),
                         digest.bytes.size(), &written) != PSA_SUCCESS ||
        written != digest.bytes.size()) {
        return Error{ErrorCode::execution_failed,
                     "Content digest computation failed",
                     {.operation = "content.sha256"}};
    }
    return digest;
}

} // namespace shark
