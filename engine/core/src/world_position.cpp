#include <shark/world_position.hpp>

#include <cfenv>
#include <cmath>
#include <limits>

namespace shark {
namespace {

static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::radix == 2 &&
              std::numeric_limits<double>::digits == 53);

Error position_error(const char* operation, const char* message,
                     const ErrorCode code = ErrorCode::invalid_argument) {
    return Error{code, message, ErrorContext{operation}};
}

bool valid_cell(const std::int64_t value) noexcept {
    return value >= world_cell_min && value <= world_cell_max;
}

bool valid_cells(const WorldCell& cell) noexcept {
    return valid_cell(cell.x) && valid_cell(cell.y) && valid_cell(cell.z);
}

Result<void> validate(const WorldPosition& position, const char* operation) {
    if (!valid_cells(position.cell)) {
        return position_error(operation,
                              "World cell is outside the version 1 representation bounds");
    }
    for (const auto offset : position.offset) {
        if (!std::isfinite(offset) || offset < 0.0 || offset >= world_cell_size_meters ||
            (offset == 0.0 && std::signbit(offset))) {
            return position_error(operation, "World position offset is not canonical");
        }
    }
    return {};
}

struct SplitOffset {
    std::int64_t cells;
    double remainder;
};

Result<SplitOffset> split_offset(const double offset, const char* operation) {
    if (!std::isfinite(offset)) {
        return position_error(operation, "World position offset or delta must be finite");
    }
    const auto cells = std::trunc(offset / world_cell_size_meters);
    // Any larger carry cannot yield an in-range cell even with cancellation by
    // the original cell. This check also makes the double-to-integer cast safe.
    constexpr double maximum_carry = 4'294'967'296.0;
    if (cells < -maximum_carry || cells > maximum_carry) {
        return position_error(operation, "World position carry exceeds the representation bounds");
    }
    return SplitOffset{static_cast<std::int64_t>(cells), std::fmod(offset, world_cell_size_meters)};
}

Result<void> normalize_axis(std::int64_t& cell, double& offset, const char* operation) {
    auto split = split_offset(offset, operation);
    if (!split) {
        return split.error();
    }
    auto carry = split.value().cells;
    auto remainder = split.value().remainder;
    if (remainder < 0.0) {
        --carry;
        remainder += world_cell_size_meters;
        // Round-to-nearest can reach the upper endpoint for a tiny negative
        // remainder. Carry that rounded boundary rather than clamp or use epsilon.
        if (remainder == world_cell_size_meters) {
            ++carry;
            remainder = 0.0;
        }
    }
    const auto result_cell = cell + carry;
    if (!valid_cell(result_cell)) {
        return position_error(operation,
                              "Normalized world cell is outside the representation bounds");
    }
    cell = result_cell;
    offset = remainder == 0.0 ? 0.0 : remainder;
    return {};
}

} // namespace

Result<void> validate_world_position(const WorldPosition& position) {
    return validate(position, "world-position.validate");
}

Result<WorldPosition> normalize_world_position(WorldPosition position) {
    constexpr auto operation = "world-position.normalize";
    if (!valid_cells(position.cell)) {
        return position_error(operation, "Input world cell is outside the representation bounds");
    }
    if (std::fegetround() != FE_TONEAREST) {
        return position_error(operation, "World position arithmetic requires round-to-nearest mode",
                              ErrorCode::invalid_configuration);
    }
    const std::array cells{&position.cell.x, &position.cell.y, &position.cell.z};
    for (std::size_t axis = 0; axis < cells.size(); ++axis) {
        auto normalized = normalize_axis(*cells[axis], position.offset[axis], operation);
        if (!normalized) {
            return normalized.error();
        }
    }
    return position;
}

Result<WorldPosition> translate_world_position(const WorldPosition& position,
                                               const std::array<double, 3>& delta_meters) {
    constexpr auto operation = "world-position.translate";
    auto canonical = validate(position, operation);
    if (!canonical) {
        return canonical.error();
    }
    if (std::fegetround() != FE_TONEAREST) {
        return position_error(operation, "World position arithmetic requires round-to-nearest mode",
                              ErrorCode::invalid_configuration);
    }
    auto translated = position;
    const std::array cells{&translated.cell.x, &translated.cell.y, &translated.cell.z};
    for (std::size_t axis = 0; axis < cells.size(); ++axis) {
        auto split = split_offset(delta_meters[axis], operation);
        if (!split) {
            return split.error();
        }
        // Keep the remainder signed until after adding the local offset, so a
        // tiny negative delta can cancel a tiny positive offset without loss.
        *cells[axis] += split.value().cells;
        translated.offset[axis] += split.value().remainder;
        auto normalized = normalize_axis(*cells[axis], translated.offset[axis], operation);
        if (!normalized) {
            return normalized.error();
        }
    }
    return translated;
}

} // namespace shark
