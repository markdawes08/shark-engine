#include "script_panel.hpp"

#include <nlohmann/json.hpp>
#include <shark/script_components.hpp>
#include <shark/tools/project_files.hpp>

#include <array>
#include <utility>

namespace shark::editor {
namespace {
using Json = nlohmann::json;
struct Failure {
    Error error;
};
template <typename T> T take(Result<T> value) {
    if (!value)
        throw Failure{std::move(value).error()};
    return std::move(value).value();
}
[[noreturn]] void fail(ErrorCode code, const char* message) {
    throw Failure{{code, message, {.operation = "editor.script"}}};
}
template <typename F> Result<void> boundary(F&& operation) {
    try {
        operation();
        return {};
    } catch (Failure& failure) {
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Cannot observe script operation; retain the draft and reconcile",
                     {.operation = "editor.script"}};
    }
}
Error decode_error(const Json& envelope) {
    constexpr std::array codes{
        ErrorCode::invalid_argument,    ErrorCode::invalid_configuration,  ErrorCode::invalid_id,
        ErrorCode::invalid_path,        ErrorCode::path_outside_root,      ErrorCode::io_failure,
        ErrorCode::entropy_unavailable, ErrorCode::capability_unavailable, ErrorCode::conflict,
        ErrorCode::execution_failed};
    const auto& data = envelope.at("data");
    const auto code = data.at("code").get<std::string>();
    for (const auto candidate : codes) {
        if (error_code_name(candidate) == code) {
            const auto& context = data.at("context");
            return {candidate,
                    envelope.at("message").get<std::string>(),
                    {context.value("operation", std::string{}),
                     context.value("request_id", std::string{}),
                     context.value("path", std::string{}),
                     context.value("entity_id", std::string{}),
                     context.value("asset_id", std::string{})}};
        }
    }
    fail(ErrorCode::execution_failed, "Unknown script error response");
}
Json result(ProjectSession& session, std::string_view method, const Json& parameters) {
    const auto reply = take(session.request(method, parameters.dump()));
    const auto envelope = Json::parse(reply.json);
    if (envelope.contains("error"))
        throw Failure{decode_error(envelope.at("error"))};
    if (reply.failed)
        fail(ErrorCode::execution_failed, "Script operation returned an unknown outcome");
    return envelope.at("result");
}
void release(ProjectSession& session, const std::optional<std::string>& token) {
    if (token && session.state_current() && session.state().open)
        static_cast<void>(result(session, "text.release", {{"revision", *token}}));
}
} // namespace

Result<void> ScriptPanel::open(std::string_view path) {
    return boundary([&] {
        if (has_changes())
            fail(ErrorCode::conflict, "Write or discard the current script draft first");
        const auto valid = take(validate_script(Script{std::string(path), true}));
        auto read = result(session_, "text.read", {{"path", valid.source}});
        auto text = read.at("text").get<std::string>();
        auto token = read.at("revision").get<std::string>();
        auto baseline = text;
        release(session_, token_);
        path_ = valid.source;
        text_ = std::move(text);
        baseline_ = std::move(baseline);
        token_ = std::move(token);
        loaded_ = true;
        is_new_ = false;
        uncertain_ = false;
        notice_ = "Loaded exact source revision.";
    });
}
Result<void> ScriptPanel::create(std::string_view path) {
    return boundary([&] {
        if (has_changes())
            fail(ErrorCode::conflict, "Write or discard the current script draft first");
        const auto valid = take(validate_script(Script{std::string(path), true}));
        release(session_, token_);
        path_ = valid.source;
        text_.clear();
        baseline_.clear();
        token_.reset();
        loaded_ = true;
        is_new_ = true;
        uncertain_ = false;
        notice_ = "New draft; writing requires the file to be absent.";
    });
}
Result<void> ScriptPanel::edit(std::string_view text) {
    return boundary([&] {
        if (!loaded_ || uncertain_)
            fail(ErrorCode::conflict, "Open a script and reconcile any uncertain write first");
        if (text.size() > tools::project_text_maximum_bytes)
            fail(ErrorCode::invalid_argument, "Script text exceeds 32768 bytes");
        text_.assign(text);
    });
}
Result<void> ScriptPanel::write() {
    return boundary([&] {
        if (!loaded_ || uncertain_ || !has_changes())
            fail(ErrorCode::conflict, "There is no writable script draft");
        // All input is prepared before publication; uncertain completion remains
        // visible until a read proves the exact bytes or the user discards.
        auto baseline = text_;
        Json parameters{{"path", path_},
                        {"text", text_},
                        {"expected_revision", token_ ? Json(*token_) : Json(nullptr)}};
        uncertain_ = true;
        try {
            auto written = result(session_, "text.write", parameters);
            if (!written.at("publication").at("published").get<bool>())
                fail(ErrorCode::execution_failed, "Script publication outcome is unknown");
            auto token = written.at("revision").get<std::string>();
            auto durability = written.at("publication").at("durability").get<std::string>();
            auto notice =
                "Written (" + durability + "). Scene Save and undo do not affect this file.";
            token_ = std::move(token);
            baseline_ = std::move(baseline);
            notice_ = std::move(notice);
            is_new_ = false;
            uncertain_ = false;
        } catch (const Failure& failure) {
            if (failure.error.code != ErrorCode::execution_failed &&
                failure.error.code != ErrorCode::io_failure)
                uncertain_ = false;
            throw;
        }
    });
}
Result<void> ScriptPanel::reconcile() {
    return boundary([&] {
        if (!loaded_ || !uncertain_)
            fail(ErrorCode::conflict, "No uncertain script write needs reconciliation");
        auto read = result(session_, "text.read", {{"path", path_}});
        auto observed = read.at("text").get<std::string>();
        auto token = read.at("revision").get<std::string>();
        if (observed != text_) {
            try {
                release(session_, token);
            } catch (...) {
            }
            fail(ErrorCode::conflict, "File contents differ from the draft; preserve or discard "
                                      "the draft before reopening");
        }
        auto baseline = text_;
        try {
            release(session_, token_);
        } catch (...) {
        }
        token_ = std::move(token);
        baseline_ = std::move(baseline);
        is_new_ = false;
        uncertain_ = false;
        notice_ = "Exact file contents observed; original write durability is unknown.";
    });
}
Result<void> ScriptPanel::discard() {
    return boundary([&] {
        if (uncertain_) {
            // The original token might have been consumed by a successful write.
            try {
                release(session_, token_);
            } catch (...) {
            }
        } else {
            release(session_, token_);
        }
        path_.clear();
        text_.clear();
        baseline_.clear();
        token_.reset();
        loaded_ = false;
        is_new_ = false;
        uncertain_ = false;
        notice_.clear();
    });
}
} // namespace shark::editor
