#include <shark/error.hpp>

namespace shark {

std::string_view error_code_name(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::invalid_argument:
        return "invalid_argument";
    case ErrorCode::invalid_configuration:
        return "invalid_configuration";
    case ErrorCode::invalid_id:
        return "invalid_id";
    case ErrorCode::invalid_path:
        return "invalid_path";
    case ErrorCode::path_outside_root:
        return "path_outside_root";
    case ErrorCode::io_failure:
        return "io_failure";
    case ErrorCode::entropy_unavailable:
        return "entropy_unavailable";
    case ErrorCode::capability_unavailable:
        return "capability_unavailable";
    case ErrorCode::conflict:
        return "conflict";
    case ErrorCode::execution_failed:
        return "execution_failed";
    }
    return "unknown_error";
}

std::string_view error_category_name(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::invalid_argument:
    case ErrorCode::invalid_configuration:
    case ErrorCode::invalid_id:
    case ErrorCode::invalid_path:
    case ErrorCode::path_outside_root:
        return "validation";
    case ErrorCode::entropy_unavailable:
    case ErrorCode::capability_unavailable:
        return "unavailable";
    case ErrorCode::conflict:
        return "conflict";
    case ErrorCode::io_failure:
    case ErrorCode::execution_failed:
        return "execution";
    }
    return "execution";
}

} // namespace shark
