#pragma once

#include <shark/log.hpp>
#include <shark/tools/authoring_protocol.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace shark::editor {

inline constexpr std::size_t console_maximum_entries = 128;

struct ConsoleEntry {
    std::uint64_t sequence{0};
    LogLevel level{LogLevel::info};
    std::string method;
    std::string request_id;
    std::string message;
    std::string job_id;
    std::optional<Error> error;
    bool published{false};
    std::string durability;
    bool truncated{false};
    std::uint64_t repeats{1};
};

// Application-private, owner-thread display journal. No raw requests, source
// documents or binary payloads are retained. Recording never changes a reply.
class ConsoleLog final {
  public:
    void observe(std::string_view method, const tools::AuthoringProtocolReply&) noexcept;
    void record(std::string_view method, const Error&) noexcept;
    [[nodiscard]] std::span<const ConsoleEntry> entries() const noexcept;
    [[nodiscard]] std::uint64_t dropped() const noexcept;
    void clear() noexcept;

  private:
    struct JobObservation {
        std::string id;
        std::string state;
    };
    void append(ConsoleEntry);
    std::vector<ConsoleEntry> entries_;
    std::vector<JobObservation> jobs_;
    std::uint64_t sequence_{0};
    std::uint64_t dropped_{0};
};

} // namespace shark::editor
