#include "shark/name.hpp"

#include "utf8.hpp"

namespace shark {

Result<void> validate_name(const Name& name) {
    if (name.value.empty() || name.value.size() > maximum_name_bytes) {
        return Error{ErrorCode::invalid_argument,
                     "name must contain 1..256 UTF-8 bytes",
                     {.operation = "name.validate"}};
    }
    if (!detail::valid_utf8(name.value, true)) {
        return Error{ErrorCode::invalid_argument,
                     "name must be valid UTF-8 without C0, C1 or DEL controls",
                     {.operation = "name.validate"}};
    }
    return {};
}

} // namespace shark
