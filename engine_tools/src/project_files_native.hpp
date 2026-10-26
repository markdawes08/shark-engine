#pragma once

#include "shark/project_path.hpp"
#include "shark/tools/project_files.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace shark::tools::detail {

enum class FilePhase { staged, written, flushed, before_publish, published };
enum class FileFault {
    none,
    read,
    stage_create,
    stage_write,
    stage_flush,
    publish,
    directory_flush
};

// Private deterministic fixture seam, scoped to the calling thread. Hooks must
// not throw; a child-process fixture may deliberately terminate at a phase.
struct FileTestControl {
    FileFault fault{FileFault::none};
    void (*hook)(FilePhase, void*) noexcept {};
    void* context{};
};

[[nodiscard]] FileTestControl set_file_test_control(FileTestControl control) noexcept;

class NativeProjectFiles final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<NativeProjectFiles>> open(const ProjectRoot& root);
    ~NativeProjectFiles();
    NativeProjectFiles(const NativeProjectFiles&) = delete;
    NativeProjectFiles& operator=(const NativeProjectFiles&) = delete;
    NativeProjectFiles(NativeProjectFiles&&) = delete;
    NativeProjectFiles& operator=(NativeProjectFiles&&) = delete;

    [[nodiscard]] Result<std::string> read(const ProjectPath& path,
                                           std::size_t maximum_bytes = source_maximum_bytes) const;
    [[nodiscard]] Result<bool> exists(const ProjectPath& path) const;
    [[nodiscard]] Result<void> validate_path(const ProjectPath& path,
                                             bool allow_missing = true) const;
    // nullopt requires an absent destination. A failed Result never published;
    // every returned durability value describes a completed publication.
    [[nodiscard]] Result<FileDurability> replace(const ProjectPath& path, std::string_view bytes,
                                                 std::optional<std::string_view> expected,
                                                 std::size_t maximum_bytes = source_maximum_bytes,
                                                 bool development_cache = false);
    [[nodiscard]] Result<std::size_t> cache_stages(bool discard);

  private:
    struct Impl;
    explicit NativeProjectFiles(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::tools::detail
