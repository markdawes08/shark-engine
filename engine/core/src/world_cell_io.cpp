#include "shark/world_cell_io.hpp"

#include "utf8.hpp"

#include <algorithm>
#include <fstream>

namespace shark {
namespace {
constexpr auto operation = "world.cell.read";
constexpr std::size_t maximum_path_bytes = 4096;
constexpr std::size_t read_chunk_bytes = 64 * 1024;

Error failure(ErrorCode code, const char* message, const std::string& path = {}) {
    return {code, message, {operation, {}, path}};
}

Result<std::string> path_text(const std::filesystem::path& path) {
    const auto& native = path.native();
    if (native.empty() || native.size() > maximum_path_bytes ||
        native.find(std::filesystem::path::value_type{}) !=
            std::filesystem::path::string_type::npos) {
        return failure(ErrorCode::invalid_path,
                       "World cell path must be bounded and contain no NUL");
    }
    const auto encoded = path.u8string();
    const std::string text(encoded.begin(), encoded.end());
    if (text.size() > maximum_path_bytes || !detail::valid_utf8(text)) {
        return failure(ErrorCode::invalid_path, "World cell path must be bounded valid Unicode");
    }
    return text;
}
} // namespace

Result<std::vector<std::byte>> read_world_cell_file(const std::filesystem::path& path,
                                                    const std::size_t maximum_bytes,
                                                    const std::stop_token stop) {
    std::string text;
    try {
        if (maximum_bytes == 0 || maximum_bytes > world_cell_maximum_payload_bytes) {
            return failure(ErrorCode::invalid_argument,
                           "World cell read bound is outside the profile");
        }
        if (stop.stop_requested())
            return failure(ErrorCode::conflict, "World cell read was cancelled");
        auto checked_path = path_text(path);
        if (!checked_path)
            return std::move(checked_path).error();
        text = std::move(checked_path).value();
        std::error_code status;
        if (!std::filesystem::is_regular_file(path, status)) {
            return failure(ErrorCode::io_failure,
                           "World cell input must be an accessible regular file", text);
        }
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
            return failure(ErrorCode::io_failure, "Cannot open world cell input", text);
        const auto end = stream.tellg();
        if (end < 0)
            return failure(ErrorCode::io_failure, "Cannot size world cell input", text);
        if (end > static_cast<std::streamoff>(maximum_bytes)) {
            return failure(ErrorCode::invalid_argument, "World cell input exceeds its byte bound",
                           text);
        }
        if (stop.stop_requested())
            return failure(ErrorCode::conflict, "World cell read was cancelled", text);
        const auto size = static_cast<std::size_t>(end);
        std::vector<std::byte> bytes(size);
        stream.seekg(0);
        if (!stream)
            return failure(ErrorCode::io_failure, "Cannot seek world cell input", text);
        std::size_t offset{};
        while (offset < size) {
            if (stop.stop_requested())
                return failure(ErrorCode::conflict, "World cell read was cancelled", text);
            const auto count = std::min(read_chunk_bytes, size - offset);
            if (!stream.read(reinterpret_cast<char*>(bytes.data() + offset),
                             static_cast<std::streamsize>(count))) {
                return failure(ErrorCode::io_failure, "Cannot read complete world cell input",
                               text);
            }
            offset += count;
        }
        if (stop.stop_requested())
            return failure(ErrorCode::conflict, "World cell read was cancelled", text);
        if (stream.peek() != std::char_traits<char>::eof() || stream.bad()) {
            return failure(ErrorCode::io_failure,
                           "World cell input changed or could not be read completely", text);
        }
        return bytes;
    } catch (const std::filesystem::filesystem_error&) {
        return failure(ErrorCode::invalid_path, "World cell path conversion failed");
    } catch (...) {
        return failure(ErrorCode::execution_failed, "World cell file read failed", text);
    }
}

} // namespace shark
