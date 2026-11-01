#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>

namespace shark::editor {

// Optional application-private observation of captured input and viewport
// routing. This cannot inject input or authorize authoring. One application
// owner bounds output across project close/reopen; repeated neutral frames do
// not consume the transition inventory. Recording never throws into the UI.
class EditorInputTrace final {
  public:
    static constexpr std::size_t maximum_transitions = 256;
    static constexpr std::size_t maximum_payload_bytes = 16 * 1024;

    EditorInputTrace(bool enabled, std::ostream& output) noexcept
        : enabled_(enabled), output_(output) {}
    EditorInputTrace(const EditorInputTrace&) = delete;
    EditorInputTrace& operator=(const EditorInputTrace&) = delete;

    void record(std::uint64_t frame, nlohmann::json observation) noexcept {
        if (!enabled_ || failed_ || emitted_ == maximum_transitions)
            return;
        try {
            const auto payload = observation.dump();
            if (payload.size() > maximum_payload_bytes) {
                failed_ = true;
                return;
            }
            if (payload == previous_)
                return;
            observation["event"] = "editor_input_trace";
            observation["frame"] = std::to_string(frame);
            observation["transition"] = emitted_ + 1;
            observation["limit_reached"] = emitted_ + 1 == maximum_transitions;
            output_ << observation.dump() << '\n' << std::flush;
            if (!output_) {
                failed_ = true;
                return;
            }
            previous_ = payload;
            ++emitted_;
        } catch (...) {
            failed_ = true;
        }
    }

    [[nodiscard]] bool enabled() const noexcept {
        return enabled_ && !failed_ && emitted_ < maximum_transitions;
    }
    [[nodiscard]] std::size_t emitted() const noexcept {
        return emitted_;
    }
    [[nodiscard]] bool failed() const noexcept {
        return failed_;
    }

  private:
    bool enabled_;
    std::ostream& output_;
    std::size_t emitted_{0};
    bool failed_{false};
    std::string previous_;
};

} // namespace shark::editor
