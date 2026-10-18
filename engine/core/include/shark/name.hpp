#pragma once

#include "shark/error.hpp"

#include <cstddef>
#include <string>

namespace shark {

inline constexpr std::size_t maximum_name_bytes = 256;

// Display text, never identity. Nonempty UTF-8, no C0/C1 controls or DEL.
// Names need not be unique. No case folding or Unicode normalization is applied.
struct Name {
    std::string value{"Entity"};
    bool operator==(const Name&) const = default;
};

[[nodiscard]] Result<void> validate_name(const Name& name);

} // namespace shark
