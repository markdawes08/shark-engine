#include "console_log.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace shark::editor {
namespace {
using Json = nlohmann::json;

void increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max())
        ++value;
}

std::string bounded(std::string_view text, std::size_t limit, bool& truncated) {
    auto size = std::min(text.size(), limit);
    if (size < text.size()) {
        while (size && (static_cast<unsigned char>(text[size]) & 0xc0U) == 0x80U)
            --size;
        truncated = true;
    }
    // This is display sanitation only. The authoring owner remains the input
    // validator. Invalid UTF-8 is replaced within the bounded copied prefix.
    auto clean = Json::parse(Json(std::string{text.substr(0, size)})
                                 .dump(-1, ' ', false, Json::error_handler_t::replace))
                     .get<std::string>();
    if (clean != text.substr(0, size))
        truncated = true;
    size = std::min(clean.size(), limit);
    if (size < clean.size()) {
        while (size && (static_cast<unsigned char>(clean[size]) & 0xc0U) == 0x80U)
            --size;
        truncated = true;
    }
    // ImGui text is NUL-terminated. Show controls as spaces instead of silently
    // losing the suffix or letting diagnostics introduce invisible UI content.
    clean.resize(size);
    for (char& value : clean)
        if (static_cast<unsigned char>(value) < 0x20U || value == '\x7f') {
            value = ' ';
            truncated = true;
        }
    return clean;
}

Error native_error(const Json& value) {
    constexpr std::array codes{
        ErrorCode::invalid_argument,    ErrorCode::invalid_configuration,  ErrorCode::invalid_id,
        ErrorCode::invalid_path,        ErrorCode::path_outside_root,      ErrorCode::io_failure,
        ErrorCode::entropy_unavailable, ErrorCode::capability_unavailable, ErrorCode::conflict,
        ErrorCode::execution_failed};
    Error result;
    const auto name = value.at("code").get<std::string>();
    for (const auto code : codes)
        if (error_code_name(code) == name)
            result.code = code;
    result.message = value.at("message").get<std::string>();
    const auto& context = value.at("context");
    result.context = {
        context.value("operation", std::string{}), context.value("request_id", std::string{}),
        context.value("path", std::string{}), context.value("entity_id", std::string{}),
        context.value("asset_id", std::string{})};
    return result;
}

void publication(ConsoleEntry& entry, const Json& value) {
    if (!value.is_object() || !value.contains("publication") || value.at("publication").is_null())
        return;
    const auto& published = value.at("publication");
    entry.published = published.at("published").get<bool>();
    entry.durability = published.at("durability").get<std::string>();
    if (entry.published && entry.durability == "directory_flush_failed")
        entry.level = LogLevel::warning;
}

bool actionable(std::string_view method) {
    constexpr std::array<std::string_view, 16> actions{
        "project.open",          "project.create",    "project.close",     "scene.save",
        "scene.reload",          "scene.mutate",      "text.write",        "asset.metadata.write",
        "asset.catalog.refresh", "asset.jobs.submit", "asset.jobs.take",   "asset.jobs.close",
        "asset.cache.reload",    "asset.cache.prune", "asset.cache.clear", "asset.cache.recover"};
    return std::find(actions.begin(), actions.end(), method) != actions.end();
}

bool same_error(const std::optional<Error>& left, const std::optional<Error>& right) {
    if (left.has_value() != right.has_value())
        return false;
    return !left || (left->code == right->code && left->message == right->message &&
                     left->context.operation == right->context.operation &&
                     left->context.path == right->context.path &&
                     left->context.entity_id == right->context.entity_id &&
                     left->context.asset_id == right->context.asset_id);
}
} // namespace

void ConsoleLog::append(ConsoleEntry entry) {
    entry.method = bounded(entry.method, 128, entry.truncated);
    entry.request_id = bounded(entry.request_id, 64, entry.truncated);
    entry.message = bounded(entry.message, 1024, entry.truncated);
    entry.job_id = bounded(entry.job_id, 64, entry.truncated);
    entry.durability = bounded(entry.durability, 64, entry.truncated);
    if (entry.error) {
        auto& error = *entry.error;
        error.message = bounded(error.message, 1024, entry.truncated);
        error.context.operation = bounded(error.context.operation, 128, entry.truncated);
        error.context.request_id = bounded(error.context.request_id, 128, entry.truncated);
        error.context.path = bounded(error.context.path, 4096, entry.truncated);
        error.context.entity_id = bounded(error.context.entity_id, 64, entry.truncated);
        error.context.asset_id = bounded(error.context.asset_id, 64, entry.truncated);
    }
    if (!entries_.empty()) {
        auto& previous = entries_.back();
        if (!actionable(entry.method) && previous.level == entry.level &&
            previous.method == entry.method && previous.message == entry.message &&
            previous.job_id == entry.job_id && previous.published == entry.published &&
            previous.durability == entry.durability && same_error(previous.error, entry.error)) {
            increment(previous.repeats);
            previous.request_id = std::move(entry.request_id);
            return;
        }
    }
    increment(sequence_);
    entry.sequence = sequence_;
    if (entries_.size() == console_maximum_entries) {
        entries_.erase(entries_.begin());
        increment(dropped_);
    }
    entries_.push_back(std::move(entry));
}

void ConsoleLog::record(std::string_view method, const Error& error) noexcept {
    try {
        ConsoleEntry entry;
        entry.method = method;
        entry.level = LogLevel::error;
        entry.message = error.message;
        entry.error = error;
        append(std::move(entry));
    } catch (...) {
        increment(dropped_);
    }
}

void ConsoleLog::observe(std::string_view method,
                         const tools::AuthoringProtocolReply& reply) noexcept {
    try {
        if (reply.json.empty())
            return;
        const auto document = Json::parse(reply.json);
        ConsoleEntry entry;
        entry.method = method;
        if (document.contains("id") && document.at("id").is_string())
            entry.request_id = document.at("id").get<std::string>();
        entry.message = "Completed";
        if (document.contains("error")) {
            const auto& error = document.at("error");
            auto fields = error.at("data");
            fields["message"] = error.at("message");
            entry.error = native_error(fields);
        } else {
            const auto& value = document.at("result");
            if ((method == "scene.mutate" || method == "scene.mutation_outcome" ||
                 method == "scene.preview") &&
                value.contains("outcome")) {
                const auto& outcome = value.at("outcome");
                if (!outcome.at("ok").get<bool>())
                    entry.error = native_error(outcome.at("error"));
                else if (outcome.at("value").contains("result")) {
                    const auto& original = outcome.at("value").at("result");
                    if (!original.at("ok").get<bool>())
                        entry.error = native_error(original.at("error"));
                    else
                        entry.message = original.at("value").at("changed").get<bool>()
                                            ? "Scene changed"
                                            : "Scene unchanged";
                }
            }
            if (method == "asset.cache.query" && !value.at("blocked_by").is_null()) {
                entry.error = native_error(value.at("blocked_by"));
                entry.message = "Import inputs blocked; inspect last-valid cache status";
            }
            if (method.starts_with("asset.jobs.")) {
                const auto observe_job = [&](const Json& job) {
                    const auto& identity = job.at("job_id");
                    const auto id = identity.at("scope").get<std::string>() + ":" +
                                    identity.at("sequence").get<std::string>();
                    const auto state = job.at("state").get<std::string>();
                    const auto previous =
                        std::find_if(jobs_.begin(), jobs_.end(),
                                     [&](const auto& seen) { return seen.id == id; });
                    if (previous != jobs_.end() && previous->state == state &&
                        method != "asset.jobs.take" && method != "asset.jobs.cancel")
                        return;
                    ConsoleEntry status = entry;
                    status.job_id = id;
                    status.message = method == "asset.jobs.take" ? "Acknowledged " + state : state;
                    if (state == "failed") {
                        status.level = LogLevel::error;
                        status.error = native_error(job.at("error"));
                    } else if (state == "cancelled") {
                        status.level = LogLevel::warning;
                    }
                    if (!job.at("commit").is_null())
                        publication(status, job.at("commit"));
                    append(std::move(status));
                    if (previous != jobs_.end())
                        previous->state = state;
                    else {
                        if (jobs_.size() == 4)
                            jobs_.erase(jobs_.begin());
                        jobs_.push_back({id, state});
                    }
                };
                if (value.contains("jobs")) {
                    if (value.at("jobs").size() > 4)
                        throw std::out_of_range("job count");
                    for (const auto& job : value.at("jobs"))
                        observe_job(job);
                } else if (value.contains("state")) {
                    observe_job(value);
                    return;
                }
            }
            publication(entry, value);
            if (!entry.error && !actionable(method) && !reply.failed)
                return;
        }
        if (entry.error) {
            entry.level = LogLevel::error;
            entry.message = entry.error->message;
        } else if (reply.failed) {
            entry.level = LogLevel::error;
            entry.message = "Operation rejected; inspect its retained outcome";
        }
        append(std::move(entry));
    } catch (...) {
        increment(dropped_);
    }
}

std::span<const ConsoleEntry> ConsoleLog::entries() const noexcept {
    return entries_;
}
std::uint64_t ConsoleLog::dropped() const noexcept {
    return dropped_;
}
void ConsoleLog::clear() noexcept {
    entries_.clear();
    dropped_ = 0;
    // Keep observed job transitions: clearing display is not taking outcomes.
}
} // namespace shark::editor
