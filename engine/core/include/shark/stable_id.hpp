#pragma once

#include "shark/error.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace shark {

// Persistent identity primitive, not a runtime handle, credential or content hash.
// Construction validates the UUIDv4 version and RFC variant; nil is never valid.
class StableId final {
  public:
    [[nodiscard]] static Result<StableId> parse(std::string_view text, ErrorContext context = {});
    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] const std::array<std::byte, 16>& bytes() const noexcept;
    auto operator<=>(const StableId&) const = default;

  private:
    explicit StableId(std::array<std::byte, 16> bytes) noexcept;
    std::array<std::byte, 16> bytes_;
};

// Domain tags keep, for example, authored and durable gameplay identities distinct.
// Owning subsystems declare their tags when implemented, rather than sharing aliases.
template <typename Domain> class TypedId final {
  public:
    explicit TypedId(StableId value) noexcept : value_(value) {}
    [[nodiscard]] const StableId& value() const noexcept {
        return value_;
    }
    auto operator<=>(const TypedId&) const = default;

  private:
    StableId value_;
};

// Explicit injection for deterministic tests. Success must initialize every byte.
// Implementations return an owned error on failure and do not retain the span.
class IdEntropy {
  public:
    virtual ~IdEntropy() = default;
    [[nodiscard]] virtual Result<void> fill(std::span<std::byte, 16> bytes) = 0;
};

// Production always uses OS entropy. Neither overload maintains a global seed.
[[nodiscard]] Result<StableId> generate_stable_id();
[[nodiscard]] Result<StableId> generate_stable_id(IdEntropy& entropy);

} // namespace shark
