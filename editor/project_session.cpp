#include "project_session.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <charconv>
#include <limits>
#include <thread>
#include <utility>

namespace shark::editor {
namespace {
using Json = nlohmann::json;

struct Failure {
    Error error;
};

template <typename T> T take(Result<T> result) {
    if (!result)
        throw Failure{std::move(result).error()};
    return std::move(result).value();
}

[[noreturn]] void fail(ErrorCode code, const char* message) {
    throw Failure{{code, message, {.operation = "editor.project.session"}}};
}

template <typename T, typename F> Result<T> boundary(F&& operation) {
    try {
        return operation();
    } catch (Failure& failure) {
        return std::move(failure.error);
    } catch (...) {
        return Error{
            ErrorCode::execution_failed,
            "Cannot complete editor project operation; inspect current state before retrying",
            {.operation = "editor.project.session"}};
    }
}

Error decode_error(const Json& envelope) {
    constexpr std::array codes{
        ErrorCode::invalid_argument,    ErrorCode::invalid_configuration,  ErrorCode::invalid_id,
        ErrorCode::invalid_path,        ErrorCode::path_outside_root,      ErrorCode::io_failure,
        ErrorCode::entropy_unavailable, ErrorCode::capability_unavailable, ErrorCode::conflict,
        ErrorCode::execution_failed};
    const auto& value = envelope.at("data");
    const auto name = value.at("code").get<std::string>();
    Error result;
    bool found = false;
    for (const auto code : codes) {
        if (error_code_name(code) == name) {
            result.code = code;
            found = true;
            break;
        }
    }
    if (!found)
        fail(ErrorCode::execution_failed, "Authoring returned an unknown stable error code");
    result.message = envelope.at("message").get<std::string>();
    const auto& context = value.at("context");
    result.context = {
        context.value("operation", std::string{}), context.value("request_id", std::string{}),
        context.value("path", std::string{}), context.value("entity_id", std::string{}),
        context.value("asset_id", std::string{})};
    return result;
}

Json result_value(const tools::AuthoringProtocolReply& reply) {
    const auto document = Json::parse(reply.json);
    if (document.contains("error"))
        throw Failure{decode_error(document.at("error"))};
    if (reply.failed)
        fail(ErrorCode::execution_failed,
             "Authoring returned a rejected nested result; inspect its retained outcome");
    return document.at("result");
}

std::uint64_t decimal(const Json& value) {
    const auto& text = value.get_ref<const std::string&>();
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        fail(ErrorCode::execution_failed, "Authoring returned an invalid revision");
    return result;
}

SceneRevision revision(const Json& value) {
    return {decimal(value.at("scene_instance")), decimal(value.at("revision"))};
}

Json revision_json(SceneRevision value) {
    return {{"scene_instance", std::to_string(value.scene_instance)},
            {"revision", std::to_string(value.revision)}};
}

ProjectState decode_state(const Json& value) {
    ProjectState state;
    state.open = value.at("open").get<bool>();
    if (state.open) {
        state.dirty = value.at("dirty").get<bool>();
        state.path = value.at("project_path").get<std::string>();
        state.name = value.at("project_name").get<std::string>();
        state.project_id = value.at("project_id").get<std::string>();
        state.world_id = value.at("world_id").get<std::string>();
        state.scene_id = value.at("scene_id").get<std::string>();
        state.revision = revision(value.at("revision"));
        state.pending_mutations = value.at("pending_mutations").get<std::uint64_t>();
    }
    return state;
}

SaveReceipt decode_save(const Json& value) {
    if (!value.at("success").get<bool>())
        fail(ErrorCode::execution_failed, "Authoring did not complete scene publication");
    const auto& publication = value.at("publication");
    SaveReceipt result{
        publication.at("published").get<bool>(), value.at("current_scene_clean").get<bool>(),
        revision(value.at("saved_revision")), publication.at("bytes").get<std::uint64_t>(),
        publication.at("durability").get<std::string>()};
    if (!result.published || result.bytes == 0 ||
        (result.durability != "file_flushed" && result.durability != "file_and_directory_flushed" &&
         result.durability != "directory_flush_failed"))
        fail(ErrorCode::execution_failed,
             "Authoring publication outcome is inconsistent; inspect saved files before retrying");
    return result;
}
} // namespace

struct ProjectSession::Impl {
    const std::thread::id thread{std::this_thread::get_id()};
    std::filesystem::path root;
    std::unique_ptr<tools::AuthoringProtocol> owner;
    ProjectState state;
    bool state_current{false};
    ConsoleLog console;
    std::uint64_t sequence{0};

    void check_thread() const {
        if (thread != std::this_thread::get_id())
            fail(ErrorCode::conflict, "Editor project operations require the creating thread");
    }

    tools::AuthoringProtocolReply dispatch(std::string_view method, std::string_view parameters) {
        check_thread();
        if (sequence == std::numeric_limits<std::uint64_t>::max())
            fail(ErrorCode::conflict,
                 "Editor request identities are exhausted; close this session");
        if (method.empty() || method.size() > 128 ||
            parameters.size() > tools::authoring_protocol_maximum_request_bytes)
            fail(ErrorCode::invalid_argument, "Editor request exceeds authoring bounds");
        // Keep raw params intact so the existing protocol detects duplicate keys,
        // depth/node limits and malformed UTF-8 rather than normalizing them away.
        auto encoded = Json{
            {"jsonrpc", "2.0"},
            {"protocol_version", tools::authoring_protocol_version},
            {"id", std::to_string(++sequence)},
            {"method",
             method}}.dump();
        encoded.pop_back();
        encoded += ",\"params\":";
        encoded += parameters;
        encoded += '}';
        if (encoded.size() > tools::authoring_protocol_maximum_request_bytes)
            fail(ErrorCode::invalid_argument, "Editor request exceeds authoring bounds");
        auto result = owner->handle(encoded);
        if (!result) {
            console.record(method, result.error());
            throw Failure{std::move(result).error()};
        }
        auto reply = std::move(result).value();
        console.observe(method, reply);
        return reply;
    }

    void refresh() {
        check_thread();
        state_current = false;
        auto candidate = decode_state(result_value(dispatch("project.status", "{}")));
        state = std::move(candidate);
        state_current = true;
    }

    tools::AuthoringProtocolReply request(std::string_view method, std::string_view parameters) {
        auto reply = dispatch(method, parameters);
        try {
            refresh();
        } catch (const Failure& failure) {
            // A failed display observation cannot erase a publication, retained
            // mutation or taken job outcome already returned by the owner.
            console.record("editor.project.refresh", failure.error);
        } catch (...) {
            // Keep the original reply even if no diagnostic can be allocated.
            // state_current was cleared before observation began.
        }
        return reply;
    }
};

ProjectSession::ProjectSession(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ProjectSession::~ProjectSession() = default;

Result<std::unique_ptr<ProjectSession>> ProjectSession::create(std::filesystem::path root,
                                                               std::string_view initial_project) {
    return boundary<std::unique_ptr<ProjectSession>>([&] {
        auto owner = take(tools::AuthoringProtocol::create(root));
        if (root.empty())
            fail(ErrorCode::invalid_path, "Editor project root must be an existing directory");
        auto impl = std::make_unique<Impl>();
        impl->root = std::filesystem::canonical(root);
        impl->owner = std::move(owner);
        impl->refresh();
        auto session = std::unique_ptr<ProjectSession>{new ProjectSession{std::move(impl)}};
        if (!initial_project.empty()) {
            auto opened = session->open(initial_project);
            if (!opened)
                throw Failure{std::move(opened).error()};
        }
        return session;
    });
}

const std::filesystem::path& ProjectSession::root() const noexcept {
    return impl_->root;
}

const ProjectState& ProjectSession::state() const noexcept {
    return impl_->state;
}

bool ProjectSession::state_current() const noexcept {
    return impl_->state_current;
}

const ConsoleLog& ProjectSession::console() const noexcept {
    return impl_->console;
}

Result<void> ProjectSession::clear_console() {
    return boundary<void>([&] {
        impl_->check_thread();
        impl_->console.clear();
        return Result<void>{};
    });
}

Result<void> ProjectSession::refresh() {
    return boundary<void>([&] {
        impl_->refresh();
        return Result<void>{};
    });
}

Result<void> ProjectSession::open(std::string_view relative_project) {
    return boundary<void>([&] {
        // Open already returns the complete authoritative project observation.
        // Use it directly so a redundant status read cannot leave newly-created
        // panels bound to the previous (possibly empty) scene incarnation.
        auto candidate = decode_state(
            result_value(impl_->dispatch("project.open", Json{{"path", relative_project}}.dump())));
        impl_->state = std::move(candidate);
        impl_->state_current = true;
        return Result<void>{};
    });
}

Result<SaveReceipt> ProjectSession::save(SceneRevision expected_revision) {
    return boundary<SaveReceipt>([&] {
        return decode_save(result_value(impl_->request(
            "scene.save", Json{{"expected_revision", revision_json(expected_revision)}}.dump())));
    });
}

Result<CloseReceipt> ProjectSession::close(CloseChoice choice, SceneRevision expected_revision) {
    return boundary<CloseReceipt>([&] {
        impl_->check_thread();
        if (choice == CloseChoice::cancel)
            return CloseReceipt{false, true, {}, {}};
        if (choice != CloseChoice::reject && choice != CloseChoice::save &&
            choice != CloseChoice::discard)
            fail(ErrorCode::invalid_argument, "Unknown editor close choice");
        CloseReceipt result;
        if (choice == CloseChoice::save) {
            result.saved = take(save(expected_revision));
            if (!result.saved->current_scene_clean) {
                result.error =
                    Error{ErrorCode::conflict,
                          "Scene was published but current changes still need resolution",
                          {.operation = "editor.project.close"}};
                return result;
            }
        }
        try {
            Json parameters = Json::object();
            if (impl_->state.open || expected_revision != SceneRevision{})
                parameters["expected_revision"] = revision_json(expected_revision);
            if (choice == CloseChoice::discard)
                parameters["unsaved"] = "discard";
            static_cast<void>(result_value(impl_->request("project.close", parameters.dump())));
            impl_->state = {};
            impl_->state_current = true;
            result.closed = true;
        } catch (Failure& failure) {
            if (!result.saved)
                throw;
            result.error = std::move(failure.error);
        } catch (...) {
            if (!result.saved)
                throw;
            result.error =
                Error{ErrorCode::execution_failed,
                      "Scene was published; inspect project status before retrying close",
                      {.operation = "editor.project.close"}};
        }
        return result;
    });
}

Result<tools::AuthoringProtocolReply> ProjectSession::request(std::string_view method,
                                                              std::string_view parameters_json) {
    return boundary<tools::AuthoringProtocolReply>(
        [&] { return impl_->request(method, parameters_json); });
}

Result<RenderScene>
ProjectSession::viewport_snapshot(const tools::AuthoringViewportRequest& request) {
    return boundary<RenderScene>([&]() -> Result<RenderScene> {
        impl_->check_thread();
        auto result = impl_->owner->viewport_snapshot(request);
        if (!result)
            impl_->console.record("editor.viewport", result.error());
        return result;
    });
}

Result<void> ProjectSession::refresh_asset_inventory(std::span<const std::string_view> sidecars) {
    return boundary<void>([&] {
        impl_->check_thread();
        if (sidecars.size() > tools::asset_maximum_catalog_entries)
            fail(ErrorCode::invalid_argument, "Asset inventory exceeds its entry bound");
        std::size_t bytes = 0;
        for (const auto path : sidecars) {
            if (path.size() > 4096 || path.size() > 16384 - bytes)
                fail(ErrorCode::invalid_argument, "Asset inventory exceeds its text bound");
            bytes += path.size();
        }
        const auto observed = result_value(impl_->request("asset.catalog.status", "{}"));
        Json paths = Json::array();
        for (const auto path : sidecars)
            paths.push_back(path);
        static_cast<void>(result_value(impl_->request(
            "asset.catalog.refresh",
            Json{{"sidecars", std::move(paths)}, {"expected_revision", observed.at("revision")}}
                .dump())));
        return Result<void>{};
    });
}

} // namespace shark::editor
