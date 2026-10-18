#include "shark/project_path.hpp"

#include "utf8.hpp"

#include <algorithm>
#include <cstdint>
#include <system_error>
#include <utility>

namespace shark {
namespace {
std::string utf8(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

bool reserved_name(std::string_view component) {
    auto base = std::string{component.substr(0, component.find('.'))};
    // Reject spaced device stems as portable aliases as well.
    while (!base.empty() && base.back() == ' ') {
        base.pop_back();
    }
    std::transform(base.begin(), base.end(), base.begin(), [](char character) {
        return character >= 'a' && character <= 'z' ? static_cast<char>(character - 'a' + 'A')
                                                    : character;
    });
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" || base == "CONIN$" ||
        base == "CONOUT$") {
        return true;
    }
    if (base.starts_with("COM") || base.starts_with("LPT")) {
        const auto suffix = std::string_view{base}.substr(3);
        return (suffix.size() == 1 && suffix[0] >= '1' && suffix[0] <= '9') ||
               suffix == "\xc2\xb9" || suffix == "\xc2\xb2" || suffix == "\xc2\xb3";
    }
    return false;
}

Error path_error(ErrorCode code, std::string message, ErrorContext context) {
    return {code, std::move(message), std::move(context)};
}

bool valid_native_encoding(const std::filesystem::path& path) {
    const auto& text = path.native();
    if (text.find(std::filesystem::path::value_type{}) !=
        std::filesystem::path::string_type::npos) {
        return false;
    }
#if defined(_WIN32)
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto code = static_cast<std::uint32_t>(text[index]);
        if (code >= 0xd800 && code <= 0xdbff) {
            if (++index == text.size()) {
                return false;
            }
            const auto low = static_cast<std::uint32_t>(text[index]);
            if (low < 0xdc00 || low > 0xdfff) {
                return false;
            }
        } else if (code >= 0xdc00 && code <= 0xdfff) {
            return false;
        }
    }
    return true;
#else
    return detail::valid_utf8(text);
#endif
}

bool contained(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    auto root_part = root.begin();
    auto candidate_part = candidate.begin();
    while (root_part != root.end()) {
        // Exact canonical spelling deliberately rejects ambiguous case-only
        // aliases, including distinct case-sensitive Windows directories.
        if (candidate_part == candidate.end() || *root_part != *candidate_part) {
            return false;
        }
        ++root_part;
        ++candidate_part;
    }
    return true;
}
} // namespace

ProjectPath::ProjectPath(std::string text) : text_(std::move(text)) {}

Result<ProjectPath> ProjectPath::parse(std::string_view text, ErrorContext context) {
    if (context.operation.empty()) {
        context.operation = "project_path.parse";
    }
    if (text.empty() || text.size() > 4096 || !detail::valid_utf8(text)) {
        return path_error(ErrorCode::invalid_path, "expected 1..4096 bytes of valid UTF-8",
                          std::move(context));
    }
    if (context.path.empty()) {
        context.path = text;
    }
    for (const auto character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20 ||
            std::string_view{"\\<>:\"|?*"}.find(character) != std::string_view::npos) {
            return path_error(ErrorCode::invalid_path,
                              "project paths require portable relative names and forward slashes",
                              std::move(context));
        }
    }
    std::size_t offset = 0;
    do {
        const auto end = text.find('/', offset);
        const auto component =
            text.substr(offset, end == std::string_view::npos ? end : end - offset);
        if (component.empty() || component.size() > 255 || component == "." || component == ".." ||
            component.back() == '.' || component.back() == ' ' || reserved_name(component)) {
            return path_error(ErrorCode::invalid_path, "invalid or reserved project path component",
                              std::move(context));
        }
        if (end == std::string_view::npos) {
            break;
        }
        offset = end + 1;
    } while (offset <= text.size());
    return ProjectPath{std::string{text}};
}

const std::string& ProjectPath::text() const noexcept {
    return text_;
}

ProjectRoot::ProjectRoot(std::filesystem::path path) : path_(std::move(path)) {}

Result<ProjectRoot> ProjectRoot::open(const std::filesystem::path& directory,
                                      ErrorContext context) {
    if (context.operation.empty()) {
        context.operation = "project_root.open";
    }
    if (!valid_native_encoding(directory)) {
        return path_error(ErrorCode::invalid_path,
                          "project root must have valid native Unicode encoding and no NUL bytes",
                          std::move(context));
    }
    if (context.path.empty()) {
        try {
            auto encoded = utf8(directory);
            if (encoded.size() <= 4096) {
                context.path = std::move(encoded);
            }
        } catch (const std::filesystem::filesystem_error&) {
            return path_error(ErrorCode::invalid_path, "cannot encode project root as UTF-8",
                              std::move(context));
        }
    }
    std::error_code error;
    auto root = std::filesystem::canonical(directory, error);
    if (error) {
        return path_error(ErrorCode::io_failure, "cannot resolve project root: " + error.message(),
                          std::move(context));
    }
    if (!std::filesystem::is_directory(root, error)) {
        return path_error(error ? ErrorCode::io_failure : ErrorCode::invalid_path,
                          "project root must be an existing directory", std::move(context));
    }
    return ProjectRoot{std::move(root)};
}

const std::filesystem::path& ProjectRoot::native_path() const noexcept {
    return path_;
}

Result<std::filesystem::path> ProjectRoot::resolve(const ProjectPath& path,
                                                   ErrorContext context) const {
    if (context.operation.empty()) {
        context.operation = "project_root.resolve";
    }
    if (context.path.empty()) {
        context.path = path.text();
    }
    std::error_code error;
    const auto current_root = std::filesystem::canonical(path_, error);
    if (error || !std::filesystem::is_directory(current_root, error)) {
        return path_error(ErrorCode::io_failure,
                          "project root is no longer an accessible directory", std::move(context));
    }
    if (!contained(path_, current_root) || !contained(current_root, path_)) {
        return path_error(ErrorCode::path_outside_root, "project root location changed",
                          std::move(context));
    }

    auto resolved = path_;
    const std::u8string encoded{path.text().begin(), path.text().end()};
    const std::filesystem::path relative{encoded};
    bool missing = false;
    for (auto component = relative.begin(); component != relative.end(); ++component) {
        resolved /= *component;
        if (missing) {
            continue;
        }
        const auto status = std::filesystem::symlink_status(resolved, error);
        if (error == std::errc::no_such_file_or_directory ||
            (!error && status.type() == std::filesystem::file_type::not_found)) {
            error.clear();
            missing = true;
            continue;
        }
        if (error) {
            return path_error(ErrorCode::io_failure,
                              "cannot inspect project path: " + error.message(),
                              std::move(context));
        }
        resolved = std::filesystem::canonical(resolved, error);
        if (error) {
            return path_error(ErrorCode::io_failure,
                              "cannot resolve project path (including dangling links): " +
                                  error.message(),
                              std::move(context));
        }
        if (!contained(path_, resolved)) {
            return path_error(ErrorCode::path_outside_root,
                              "resolved project path escapes its root", std::move(context));
        }
        auto next = component;
        if (++next != relative.end() && !std::filesystem::is_directory(resolved, error)) {
            return path_error(error ? ErrorCode::io_failure : ErrorCode::invalid_path,
                              "project path ancestor must be a directory", std::move(context));
        }
    }
    return resolved;
}

} // namespace shark
