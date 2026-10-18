#pragma once

#include <shark/error.hpp>

#include <cstddef>
#include <iosfwd>

namespace shark {

enum class LogLevel { debug, info, warning, error };

// The root owns the sink and must keep it alive until after this logger. Calls
// must be serialized by that owner, including calls through other loggers that
// share the sink. This synchronous logger allocates, writes and flushes: never
// use it in signal handlers, audio/real-time callbacks or allocation-free code.
// A successful write is one UTF-8 JSON object and newline. Oversize or invalid
// UTF-8 fields fail before writing; no silent truncation. Paths allow the full
// 4096-byte ProjectPath limit; other caller fields allow 1024 bytes each. Sink failure can leave
// a partial record, returns io_failure, and never recursively logs or retries.
class Logger {
  public:
    static constexpr std::size_t max_field_bytes = 1024;
    static constexpr std::size_t max_path_bytes = 4096;
    static constexpr std::size_t max_record_bytes = 65536;

    explicit Logger(std::ostream& sink) noexcept : sink_(sink) {}

    [[nodiscard]] Result<void> write(LogLevel level, const Error& error);

  private:
    std::ostream& sink_;
};

} // namespace shark
