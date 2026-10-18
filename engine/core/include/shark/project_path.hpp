#pragma once

#include "shark/error.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace shark {

// Portable UTF-8 reference, distinct from a native OS path. Components use '/'.
// Empty/dot components, traversal, malformed UTF-8, Windows device/ADS names,
// control characters and trailing dots/spaces are rejected on every platform.
// Bounds: 4096 UTF-8 bytes total, 255 bytes per component.
class ProjectPath final {
  public:
    [[nodiscard]] static Result<ProjectPath> parse(std::string_view text,
                                                   ErrorContext context = {});
    [[nodiscard]] const std::string& text() const noexcept;
    bool operator==(const ProjectPath&) const = default;

  private:
    explicit ProjectPath(std::string text);
    std::string text_;
};

class ProjectRoot final {
  public:
    // The root must already exist and be a directory; its canonical absolute
    // location is retained independently of subsequent process working directories.
    [[nodiscard]] static Result<ProjectRoot> open(const std::filesystem::path& directory,
                                                  ErrorContext context = {});
    [[nodiscard]] const std::filesystem::path& native_path() const noexcept;

    // Resolves existing ancestors, rejecting symlink/junction escapes and dangling
    // links. Missing descendants are allowed for future creation. Does not create
    // anything, reserve a filename or provide an OS sandbox: concurrent filesystem
    // replacement between validation and use remains a caller coordination concern.
    [[nodiscard]] Result<std::filesystem::path> resolve(const ProjectPath& path,
                                                        ErrorContext context = {}) const;

  private:
    explicit ProjectRoot(std::filesystem::path path);
    std::filesystem::path path_;
};

} // namespace shark
