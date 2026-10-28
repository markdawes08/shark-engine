#include "executable_path.hpp"

#include "shader_path.hpp"

#include <array>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace shark::client::detail {
Result<std::filesystem::path> executable_directory() {
    const auto fail = []() -> Result<std::filesystem::path> {
        return Error{ErrorCode::io_failure,
                     "Cannot resolve a bounded executable directory",
                     {"client.executable.locate"}};
    };
    try {
        std::filesystem::path executable;
#if defined(_WIN32)
        std::array<wchar_t, 4097> buffer{};
        const auto size =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0 || size >= buffer.size())
            return fail();
        executable = std::wstring(buffer.data(), size);
#else
        std::error_code error;
        executable = std::filesystem::read_symlink("/proc/self/exe", error);
        if (error || !executable.is_absolute())
            return fail();
#endif
        if (!artifact_path_text(executable))
            return fail();
        return executable.parent_path();
    } catch (...) {
        return fail();
    }
}
} // namespace shark::client::detail
