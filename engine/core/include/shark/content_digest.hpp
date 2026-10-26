#pragma once

#include "shark/error.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace shark {

// Content identity, not persistent asset identity or an authenticity signature.
struct ContentDigest {
    std::array<std::byte, 32> bytes{};
    auto operator<=>(const ContentDigest&) const = default;
    [[nodiscard]] std::string to_hex() const;
    [[nodiscard]] static Result<ContentDigest> parse(std::string_view hexadecimal);
};

// Uses Core's already pinned private cryptographic implementation/runtime.
[[nodiscard]] Result<ContentDigest> sha256(std::span<const std::byte> bytes);

} // namespace shark
