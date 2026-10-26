#include "project_files_native.hpp"

#include "shark/stable_id.hpp"
#include "shark/tools/source_document.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace shark::tools::detail {
namespace {

constexpr std::size_t maximum_components = 64;
constexpr std::size_t maximum_directory_entries = 8192;
constexpr std::string_view lock_name = ".shark-write.lock";
constexpr std::string_view stage_prefix = ".shark-stage-";
thread_local FileTestControl test_control;

struct FileFailure {
    Error error;
};

[[noreturn]] void fail(ErrorCode code, const char* message, std::string_view path = {}) {
    throw FileFailure{Error{code, message, {.path = std::string{path}}}};
}

template <typename T> T checked(Result<T> result) {
    if (!result) {
        throw FileFailure{std::move(result).error()};
    }
    return std::move(result).value();
}

template <typename T, typename Function>
Result<T> boundary(const char* operation, Function&& function) {
    try {
        return function();
    } catch (FileFailure& failure) {
        failure.error.context.operation = operation;
        return std::move(failure.error);
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::execution_failed,
                     "project file allocation failed",
                     {.operation = operation}};
    } catch (...) {
        return Error{
            ErrorCode::execution_failed, "project file operation failed", {.operation = operation}};
    }
}

char folded(char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

bool ascii_equal(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [](char a, char b) { return folded(a) == folded(b); });
}

std::vector<std::string> components(const ProjectPath& path) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    do {
        const auto end = path.text().find('/', begin);
        const auto part = std::string_view{path.text()}.substr(
            begin, end == std::string::npos ? end : end - begin);
        if (ascii_equal(part, lock_name) ||
            (part.size() >= stage_prefix.size() &&
             ascii_equal(part.substr(0, stage_prefix.size()), stage_prefix))) {
            fail(ErrorCode::invalid_path, "project path uses a reserved file-service name",
                 path.text());
        }
        if (result.size() == maximum_components) {
            fail(ErrorCode::invalid_path, "project file paths allow at most 64 components",
                 path.text());
        }
        result.emplace_back(part);
        if (end == std::string::npos) {
            return result;
        }
        begin = end + 1;
    } while (begin < path.text().size());
    return result;
}

void phase(FilePhase value) noexcept {
    if (test_control.hook != nullptr) {
        test_control.hook(value, test_control.context);
    }
}

void fault(FileFault value, std::string_view path) {
    if (test_control.fault == value) {
        fail(ErrorCode::io_failure, "injected project file failure", path);
    }
}

std::filesystem::path native_component(std::string_view text) {
    return std::filesystem::path{std::u8string{text.begin(), text.end()}};
}

#if defined(_WIN32)

class Handle final {
  public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~Handle() {
        if (value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
    }
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value_ != INVALID_HANDLE_VALUE) {
                CloseHandle(value_);
            }
            value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    [[nodiscard]] HANDLE get() const noexcept {
        return value_;
    }
    explicit operator bool() const noexcept {
        return value_ != INVALID_HANDLE_VALUE;
    }

  private:
    HANDLE value_;
};

std::wstring os_path(const std::filesystem::path& path) {
    auto value = path.native();
    if (value.starts_with(L"\\\\?\\")) {
        return value;
    }
    if (value.starts_with(L"\\\\")) {
        return L"\\\\?\\UNC\\" + value.substr(2);
    }
    return L"\\\\?\\" + value;
}

bool absent_error(DWORD error) noexcept {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

[[noreturn]] void os_failure(const char* message, std::string_view path = {}) {
    const auto error = GetLastError();
    fail(error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION ? ErrorCode::conflict
                                                                           : ErrorCode::io_failure,
         message, path);
}

using Metadata = BY_HANDLE_FILE_INFORMATION;

Metadata metadata(const Handle& handle, std::string_view path) {
    Metadata value{};
    if (!GetFileInformationByHandle(handle.get(), &value)) {
        os_failure("cannot inspect project filesystem object", path);
    }
    return value;
}

bool same_object(const Metadata& left, const Metadata& right) noexcept {
    return left.dwVolumeSerialNumber == right.dwVolumeSerialNumber &&
           left.nFileIndexHigh == right.nFileIndexHigh && left.nFileIndexLow == right.nFileIndexLow;
}

bool unchanged(const Metadata& left, const Metadata& right) noexcept {
    return same_object(left, right) && left.nFileSizeHigh == right.nFileSizeHigh &&
           left.nFileSizeLow == right.nFileSizeLow &&
           left.ftLastWriteTime.dwHighDateTime == right.ftLastWriteTime.dwHighDateTime &&
           left.ftLastWriteTime.dwLowDateTime == right.ftLastWriteTime.dwLowDateTime &&
           left.nNumberOfLinks == right.nNumberOfLinks;
}

void validate_object(const Metadata& value, bool directory, std::string_view path) {
    if ((value.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        fail(ErrorCode::invalid_path, "project file paths cannot contain reparse points", path);
    }
    if (((value.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory) {
        fail(ErrorCode::invalid_path,
             directory ? "project file ancestor must be a directory"
                       : "project source must be a regular file",
             path);
    }
    if (!directory && value.nNumberOfLinks != 1) {
        fail(ErrorCode::invalid_path, "project source cannot be a hard link", path);
    }
}

Handle open_directory(const std::filesystem::path& path, std::string_view context) {
    Handle handle{CreateFileW(os_path(path).c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (!handle) {
        os_failure("cannot open project directory", context);
    }
    validate_object(metadata(handle, context), true, context);
    return handle;
}

void check_spelling(const std::filesystem::path& parent, std::string_view name,
                    std::string_view context) {
    const auto expected = native_component(name).native();
    const auto pattern = os_path(parent / L"*");
    WIN32_FIND_DATAW data{};
    const auto search = FindFirstFileW(pattern.c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) {
            return;
        }
        os_failure("cannot inspect project directory names", context);
    }
    struct FindCloser {
        HANDLE handle;
        ~FindCloser() {
            FindClose(handle);
        }
    } closer{search};
    std::size_t count = 0;
    do {
        const std::wstring_view actual{data.cFileName};
        if (actual == L"." || actual == L"..") {
            continue;
        }
        if (++count > maximum_directory_entries) {
            fail(ErrorCode::invalid_path, "project directory exceeds the 8192-entry scan bound",
                 context);
        }
        if (CompareStringOrdinal(actual.data(), static_cast<int>(actual.size()), expected.data(),
                                 static_cast<int>(expected.size()), TRUE) == CSTR_EQUAL &&
            actual != expected) {
            fail(ErrorCode::invalid_path, "project path has an ambiguous case alias", context);
        }
        if (data.cAlternateFileName[0] != L'\0' &&
            CompareStringOrdinal(data.cAlternateFileName, -1, expected.c_str(), -1, TRUE) ==
                CSTR_EQUAL &&
            actual != expected) {
            fail(ErrorCode::invalid_path, "project path uses an alternate short filename", context);
        }
    } while (FindNextFileW(search, &data));
    if (GetLastError() != ERROR_NO_MORE_FILES) {
        os_failure("cannot complete project directory inspection", context);
    }
}

std::optional<Handle> open_source(const std::filesystem::path& path, std::string_view context) {
    Handle handle{CreateFileW(os_path(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (!handle) {
        if (absent_error(GetLastError())) {
            return std::nullopt;
        }
        os_failure("cannot open project source", context);
    }
    if (GetFileType(handle.get()) != FILE_TYPE_DISK) {
        fail(ErrorCode::invalid_path, "project source must be a disk file", context);
    }
    validate_object(metadata(handle, context), false, context);
    return handle;
}

std::string read_file(const Handle& handle, std::string_view context,
                      std::size_t maximum_bytes = source_maximum_bytes) {
    fault(FileFault::read, context);
    const auto before = metadata(handle, context);
    validate_object(before, false, context);
    if (before.nFileSizeHigh != 0 || before.nFileSizeLow > maximum_bytes) {
        fail(ErrorCode::invalid_configuration, "project source exceeds its byte limit", context);
    }
    std::string result;
    result.reserve(static_cast<std::size_t>(before.nFileSizeLow));
    std::array<char, 65536> buffer{};
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(handle.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count,
                      nullptr)) {
            os_failure("cannot read project source", context);
        }
        if (count == 0) {
            break;
        }
        if (count > maximum_bytes - result.size()) {
            fail(ErrorCode::invalid_configuration, "project source exceeds its byte limit",
                 context);
        }
        result.append(buffer.data(), count);
    }
    const auto after = metadata(handle, context);
    if (!unchanged(before, after) || result.size() != before.nFileSizeLow) {
        fail(ErrorCode::conflict, "project source changed while being read", context);
    }
    return result;
}

#else

class Handle final {
  public:
    explicit Handle(int value = -1) noexcept : value_(value) {}
    ~Handle() {
        if (value_ >= 0) {
            close(value_);
        }
    }
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) {
                close(value_);
            }
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    [[nodiscard]] int get() const noexcept {
        return value_;
    }
    explicit operator bool() const noexcept {
        return value_ >= 0;
    }

  private:
    int value_;
};

using Metadata = struct stat;

[[noreturn]] void os_failure(const char* message, std::string_view path = {}) {
    fail(errno == ELOOP || errno == ENOTDIR ? ErrorCode::invalid_path : ErrorCode::io_failure,
         message, path);
}

Metadata metadata(const Handle& handle, std::string_view path) {
    Metadata value{};
    if (fstat(handle.get(), &value) != 0) {
        os_failure("cannot inspect project filesystem object", path);
    }
    return value;
}

bool same_object(const Metadata& left, const Metadata& right) noexcept {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

bool unchanged(const Metadata& left, const Metadata& right) noexcept {
    return same_object(left, right) && left.st_size == right.st_size &&
           left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
           left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
           left.st_ctim.tv_sec == right.st_ctim.tv_sec &&
           left.st_ctim.tv_nsec == right.st_ctim.tv_nsec && left.st_nlink == right.st_nlink;
}

void validate_object(const Metadata& value, bool directory, std::string_view path) {
    if (!(directory ? S_ISDIR(value.st_mode) : S_ISREG(value.st_mode))) {
        fail(ErrorCode::invalid_path,
             directory ? "project file ancestor must be an ordinary directory"
                       : "project source must be a regular file",
             path);
    }
    if (!directory && value.st_nlink != 1) {
        fail(ErrorCode::invalid_path, "project source cannot be a hard link", path);
    }
}

Handle open_directory_at(int parent, const char* name, std::string_view context) {
    Handle handle{openat(parent, name, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW)};
    if (!handle) {
        os_failure("cannot open project directory", context);
    }
    validate_object(metadata(handle, context), true, context);
    return handle;
}

void check_spelling(int parent, std::string_view name, std::string_view context) {
    const auto descriptor = openat(parent, ".", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (descriptor < 0) {
        os_failure("cannot inspect project directory names", context);
    }
    auto* directory = fdopendir(descriptor);
    if (directory == nullptr) {
        close(descriptor);
        os_failure("cannot inspect project directory names", context);
    }
    struct DirectoryCloser {
        DIR* directory;
        ~DirectoryCloser() {
            closedir(directory);
        }
    } closer{directory};
    std::size_t count = 0;
    for (;;) {
        errno = 0;
        const auto* entry = readdir(directory);
        if (entry == nullptr) {
            if (errno != 0) {
                os_failure("cannot complete project directory inspection", context);
            }
            break;
        }
        const std::string_view actual{entry->d_name};
        if (actual == "." || actual == "..") {
            continue;
        }
        if (++count > maximum_directory_entries) {
            fail(ErrorCode::invalid_path, "project directory exceeds the 8192-entry scan bound",
                 context);
        }
        if (ascii_equal(actual, name) && actual != name) {
            fail(ErrorCode::invalid_path, "project path has an ambiguous ASCII case alias",
                 context);
        }
    }
}

std::optional<Handle> open_source(int parent, const char* name, std::string_view context) {
    Handle handle{openat(parent, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (!handle) {
        if (errno == ENOENT) {
            return std::nullopt;
        }
        os_failure("cannot open project source", context);
    }
    validate_object(metadata(handle, context), false, context);
    return handle;
}

std::string read_file(const Handle& handle, std::string_view context,
                      std::size_t maximum_bytes = source_maximum_bytes) {
    fault(FileFault::read, context);
    const auto before = metadata(handle, context);
    validate_object(before, false, context);
    if (before.st_size < 0 || static_cast<std::uint64_t>(before.st_size) > maximum_bytes) {
        fail(ErrorCode::invalid_configuration, "project source exceeds its byte limit", context);
    }
    std::string result;
    result.reserve(static_cast<std::size_t>(before.st_size));
    std::array<char, 65536> buffer{};
    for (;;) {
        const auto count = ::read(handle.get(), buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            os_failure("cannot read project source", context);
        }
        if (count == 0) {
            break;
        }
        const auto size = static_cast<std::size_t>(count);
        if (size > maximum_bytes - result.size()) {
            fail(ErrorCode::invalid_configuration, "project source exceeds its byte limit",
                 context);
        }
        result.append(buffer.data(), size);
    }
    const auto after = metadata(handle, context);
    if (!unchanged(before, after) || result.size() != static_cast<std::size_t>(before.st_size)) {
        fail(ErrorCode::conflict, "project source changed while being read", context);
    }
    return result;
}

#endif

struct Location {
    std::vector<Handle> directories;
    std::vector<std::string> names;
    std::filesystem::path parent_path;
    bool missing_parent{false};
};

struct Stage {
    Handle handle;
    std::string name;
    bool published{false};
#if !defined(_WIN32)
    int parent{-1};
#endif
    ~Stage() {
        if (!published && handle) {
#if defined(_WIN32)
            FILE_DISPOSITION_INFO disposition{TRUE};
            SetFileInformationByHandle(handle.get(), FileDispositionInfo, &disposition,
                                       sizeof(disposition));
#else
            Metadata held{};
            Metadata named{};
            if (fstat(handle.get(), &held) == 0 &&
                fstatat(parent, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 &&
                same_object(held, named)) {
                unlinkat(parent, name.c_str(), 0);
            }
#endif
        }
    }
};

} // namespace

struct NativeProjectFiles::Impl {
    ProjectRoot root;
    Handle root_handle;
    Handle lock_handle;
    Metadata root_identity;
    Metadata lock_identity;

    explicit Impl(const ProjectRoot& value) : root(value) {}

    void validate_root(std::string_view context) const {
#if defined(_WIN32)
        const auto current = open_directory(root.native_path(), context);
        if (!same_object(metadata(current, context), root_identity)) {
            fail(ErrorCode::conflict, "project root was replaced", context);
        }
#else
        const auto current = open_directory_at(AT_FDCWD, root.native_path().c_str(), context);
        if (!same_object(metadata(current, context), root_identity)) {
            fail(ErrorCode::conflict, "project root was replaced", context);
        }
        Metadata lock{};
        if (fstatat(root_handle.get(), lock_name.data(), &lock, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(lock.st_mode) || lock.st_nlink != 1 || !same_object(lock, lock_identity)) {
            fail(ErrorCode::conflict, "project ownership lock was replaced or removed", context);
        }
#endif
    }

    Location locate(const ProjectPath& path, bool allow_missing_parent) const {
        auto names = components(path);
        validate_root(path.text());
        static_cast<void>(checked(root.resolve(path)));
        Location location{{}, std::move(names), root.native_path(), false};
        location.directories.reserve(location.names.size() - 1);
        for (std::size_t index = 0; index + 1 < location.names.size(); ++index) {
            const auto& name = location.names[index];
            if (location.missing_parent) {
                location.parent_path /= native_component(name);
                continue;
            }
#if defined(_WIN32)
            check_spelling(location.parent_path, name, path.text());
            const auto candidate = location.parent_path / native_component(name);
            const auto attributes = GetFileAttributesW(os_path(candidate).c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES && absent_error(GetLastError()) &&
                allow_missing_parent) {
                location.missing_parent = true;
            } else {
                location.directories.push_back(open_directory(candidate, path.text()));
            }
#else
            const auto parent = location.directories.empty() ? root_handle.get()
                                                             : location.directories.back().get();
            check_spelling(parent, name, path.text());
            Metadata info{};
            if (fstatat(parent, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT &&
                allow_missing_parent) {
                location.missing_parent = true;
            } else {
                location.directories.push_back(
                    open_directory_at(parent, name.c_str(), path.text()));
            }
#endif
            location.parent_path /= native_component(name);
        }
        return location;
    }

    void validate_chain(const Location& location, std::string_view context) const {
        validate_root(context);
        auto parent_path = root.native_path();
#if !defined(_WIN32)
        auto parent = root_handle.get();
#endif
        for (std::size_t index = 0; index < location.directories.size(); ++index) {
            parent_path /= native_component(location.names[index]);
#if defined(_WIN32)
            const auto current = open_directory(parent_path, context);
            if (!same_object(metadata(current, context),
                             metadata(location.directories[index], context))) {
                fail(ErrorCode::conflict, "project directory changed during file operation",
                     context);
            }
#else
            Metadata current{};
            if (fstatat(parent, location.names[index].c_str(), &current, AT_SYMLINK_NOFOLLOW) !=
                    0 ||
                !S_ISDIR(current.st_mode) ||
                !same_object(current, metadata(location.directories[index], context))) {
                fail(ErrorCode::conflict, "project directory changed during file operation",
                     context);
            }
            parent = location.directories[index].get();
#endif
        }
    }

    std::optional<Handle> source(const Location& location, std::string_view context) const {
        if (location.missing_parent) {
            return std::nullopt;
        }
#if defined(_WIN32)
        check_spelling(location.parent_path, location.names.back(), context);
        return open_source(location.parent_path / native_component(location.names.back()), context);
#else
        const auto parent =
            location.directories.empty() ? root_handle.get() : location.directories.back().get();
        check_spelling(parent, location.names.back(), context);
        return open_source(parent, location.names.back().c_str(), context);
#endif
    }
};

FileTestControl set_file_test_control(FileTestControl control) noexcept {
    return std::exchange(test_control, control);
}

NativeProjectFiles::NativeProjectFiles(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
NativeProjectFiles::~NativeProjectFiles() = default;

Result<std::unique_ptr<NativeProjectFiles>> NativeProjectFiles::open(const ProjectRoot& root) {
    return boundary<std::unique_ptr<NativeProjectFiles>>("source.files.open", [&] {
        auto impl = std::make_unique<Impl>(root);
#if defined(_WIN32)
        impl->root_handle = open_directory(root.native_path(), {});
        check_spelling(root.native_path(), lock_name, {});
        impl->lock_handle =
            Handle{CreateFileW(os_path(root.native_path() / native_component(lock_name)).c_str(),
                               GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                               FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (!impl->lock_handle) {
            os_failure("project already has an owner or its lock cannot be opened");
        }
#else
        impl->root_handle = open_directory_at(AT_FDCWD, root.native_path().c_str(), {});
        check_spelling(impl->root_handle.get(), lock_name, {});
        impl->lock_handle = Handle{openat(impl->root_handle.get(), lock_name.data(),
                                         O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600)};
        if (!impl->lock_handle) {
            os_failure("cannot open project ownership lock");
        }
        if (flock(impl->lock_handle.get(), LOCK_EX | LOCK_NB) != 0) {
            fail(errno == EWOULDBLOCK ? ErrorCode::conflict : ErrorCode::io_failure,
                 "project already has an owner or its lock cannot be acquired");
        }
#endif
        impl->root_identity = metadata(impl->root_handle, {});
        impl->lock_identity = metadata(impl->lock_handle, {});
        validate_object(impl->lock_identity, false, lock_name);
        impl->validate_root({});
        return std::unique_ptr<NativeProjectFiles>{new NativeProjectFiles{std::move(impl)}};
    });
}

Result<void> NativeProjectFiles::validate_path(const ProjectPath& path, bool allow_missing) const {
    return boundary<void>("source.files.validate_path", [&]() -> Result<void> {
        const auto location = impl_->locate(path, allow_missing);
        const auto file = impl_->source(location, path.text());
        if (!file && !allow_missing) {
            fail(ErrorCode::io_failure, "project source does not exist", path.text());
        }
        impl_->validate_chain(location, path.text());
        return {};
    });
}

Result<bool> NativeProjectFiles::exists(const ProjectPath& path) const {
    return boundary<bool>("source.files.exists", [&] {
        const auto location = impl_->locate(path, true);
        const auto file = impl_->source(location, path.text());
        impl_->validate_chain(location, path.text());
        return file.has_value();
    });
}

Result<std::string> NativeProjectFiles::read(const ProjectPath& path,
                                             std::size_t maximum_bytes) const {
    return boundary<std::string>("source.files.read", [&] {
        const auto location = impl_->locate(path, false);
        const auto file = impl_->source(location, path.text());
        if (!file) {
            fail(ErrorCode::io_failure, "project source does not exist", path.text());
        }
        auto result = read_file(*file, path.text(), maximum_bytes);
        impl_->validate_chain(location, path.text());
        return result;
    });
}

Result<FileDurability> NativeProjectFiles::replace(const ProjectPath& path, std::string_view bytes,
                                                   std::optional<std::string_view> expected,
                                                   std::size_t maximum_bytes) {
    return boundary<FileDurability>("source.files.replace", [&] {
        if (bytes.size() > maximum_bytes || (expected && expected->size() > maximum_bytes)) {
            fail(ErrorCode::invalid_configuration, "project source exceeds its byte limit",
                 path.text());
        }
        const auto location = impl_->locate(path, false);
        // Reject an invalid or stale destination before creating a stage; repeat
        // this comparison immediately before publication as well.
        const auto check_expected = [&] {
            const auto file = impl_->source(location, path.text());
            if (file.has_value() != expected.has_value() ||
                (file && read_file(*file, path.text(), maximum_bytes) != *expected)) {
                fail(ErrorCode::conflict, "project source changed; reload before retrying",
                     path.text());
            }
        };
        check_expected();
        fault(FileFault::stage_create, path.text());
        Stage stage;
#if !defined(_WIN32)
        stage.parent = location.directories.empty() ? impl_->root_handle.get()
                                                    : location.directories.back().get();
#endif
        for (std::size_t attempt = 0; attempt < 16; ++attempt) {
            stage.name = std::string{stage_prefix} + checked(generate_stable_id()).to_string();
#if defined(_WIN32)
            stage.handle = Handle{CreateFileW(
                os_path(location.parent_path / native_component(stage.name)).c_str(),
                GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH,
                nullptr)};
            if (!stage.handle && GetLastError() != ERROR_FILE_EXISTS &&
                GetLastError() != ERROR_ALREADY_EXISTS) {
                os_failure("cannot create sibling project stage", path.text());
            }
#else
            stage.handle = Handle{openat(stage.parent, stage.name.c_str(),
                                         O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600)};
            if (!stage.handle && errno != EEXIST) {
                os_failure("cannot create sibling project stage", path.text());
            }
#endif
            if (stage.handle) {
                break;
            }
        }
        if (!stage.handle) {
            fail(ErrorCode::io_failure, "cannot reserve a unique sibling project stage",
                 path.text());
        }
        phase(FilePhase::staged);
        const auto write_limit =
            test_control.fault == FileFault::stage_write ? bytes.size() / 2 : bytes.size();
        std::size_t offset = 0;
        while (offset < write_limit) {
            const auto remaining = std::min<std::size_t>(65536, write_limit - offset);
#if defined(_WIN32)
            DWORD count = 0;
            if (!WriteFile(stage.handle.get(), bytes.data() + offset, static_cast<DWORD>(remaining),
                           &count, nullptr) ||
                count == 0) {
                os_failure("cannot write sibling project stage", path.text());
            }
            offset += count;
#else
            const auto count = ::write(stage.handle.get(), bytes.data() + offset, remaining);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                os_failure("cannot write sibling project stage", path.text());
            }
            offset += static_cast<std::size_t>(count);
#endif
        }
        fault(FileFault::stage_write, path.text());
        phase(FilePhase::written);
        fault(FileFault::stage_flush, path.text());
#if defined(_WIN32)
        if (!FlushFileBuffers(stage.handle.get())) {
            os_failure("cannot flush sibling project stage", path.text());
        }
#else
        if (fsync(stage.handle.get()) != 0) {
            os_failure("cannot flush sibling project stage", path.text());
        }
#endif
        phase(FilePhase::flushed);
        // Every allocation needed by the native publication is prepared before
        // the final expected-state check. After native success only noexcept
        // hooks, native calls, destructors and enum construction remain.
#if defined(_WIN32)
        const auto destination =
            os_path(location.parent_path / native_component(location.names.back()));
        const auto rename_bytes =
            offsetof(FILE_RENAME_INFO, FileName) + (destination.size() + 1) * sizeof(wchar_t);
        const auto words = (rename_bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t);
        std::vector<std::max_align_t> rename_storage(words);
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(rename_storage.data());
        rename->ReplaceIfExists = expected ? TRUE : FALSE;
        rename->RootDirectory = nullptr;
        rename->FileNameLength = static_cast<DWORD>(destination.size() * sizeof(wchar_t));
        std::memcpy(rename->FileName, destination.c_str(),
                    (destination.size() + 1) * sizeof(wchar_t));
#endif
        phase(FilePhase::before_publish);
        impl_->validate_chain(location, path.text());
        check_expected();
#if !defined(_WIN32)
        Metadata named_stage{};
        const auto held_stage = metadata(stage.handle, path.text());
        if (fstatat(stage.parent, stage.name.c_str(), &named_stage, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(named_stage.st_mode) || named_stage.st_nlink != 1 ||
            !same_object(named_stage, held_stage)) {
            fail(ErrorCode::conflict, "sibling project stage was replaced or linked", path.text());
        }
#endif
        fault(FileFault::publish, path.text());
#if defined(_WIN32)
        if (!SetFileInformationByHandle(stage.handle.get(), FileRenameInfo, rename,
                                        static_cast<DWORD>(rename_bytes))) {
            if (!expected &&
                (GetLastError() == ERROR_ALREADY_EXISTS || GetLastError() == ERROR_FILE_EXISTS)) {
                fail(ErrorCode::conflict, "project source was created before publication",
                     path.text());
            }
            os_failure("cannot publish sibling project stage", path.text());
        }
#else
        const auto renamed = expected ? renameat(stage.parent, stage.name.c_str(), stage.parent,
                                                 location.names.back().c_str())
                                      : syscall(SYS_renameat2, stage.parent, stage.name.c_str(), stage.parent,
                                                location.names.back().c_str(), RENAME_NOREPLACE);
        if (renamed != 0) {
            if (!expected && errno == EEXIST) {
                fail(ErrorCode::conflict, "project source was created before publication", path.text());
            }
            if (!expected && (errno == ENOSYS || errno == EINVAL)) {
                fail(ErrorCode::capability_unavailable, "filesystem does not support exclusive atomic publication",
                     path.text());
            }
            os_failure("cannot publish sibling project stage", path.text());
        }
#endif
        stage.published = true;
        phase(FilePhase::published);
        if (test_control.fault == FileFault::directory_flush) {
            return FileDurability::directory_flush_failed;
        }
#if defined(_WIN32)
        return FlushFileBuffers(stage.handle.get()) ? FileDurability::file_flushed
                                                    : FileDurability::directory_flush_failed;
#else
        return fsync(stage.parent) == 0 ? FileDurability::file_and_directory_flushed
                                       : FileDurability::directory_flush_failed;
#endif
    });
}

} // namespace shark::tools::detail
