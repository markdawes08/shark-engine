#include "prefab_panel.hpp"

#include <nlohmann/json.hpp>
#include <shark/project_path.hpp>
#include <shark/tools/prefab_document.hpp>

#include <array>
#include <charconv>
#include <thread>
#include <type_traits>
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
    throw Failure{{code, message, {.operation = "editor.prefab"}}};
}
template <typename T, typename F> Result<T> boundary(F&& operation) {
    try {
        if constexpr (std::is_void_v<T>) {
            operation();
            return {};
        } else {
            return operation();
        }
    } catch (Failure& failure) {
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Cannot observe prefab operation; keep the draft and reconcile",
                     {.operation = "editor.prefab"}};
    }
}
Error decode_error(const Json& envelope) {
    constexpr std::array codes{
        ErrorCode::invalid_argument,    ErrorCode::invalid_configuration,  ErrorCode::invalid_id,
        ErrorCode::invalid_path,        ErrorCode::path_outside_root,      ErrorCode::io_failure,
        ErrorCode::entropy_unavailable, ErrorCode::capability_unavailable, ErrorCode::conflict,
        ErrorCode::execution_failed};
    const auto& data = envelope.contains("data") ? envelope.at("data") : envelope;
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
    fail(ErrorCode::execution_failed, "Unknown prefab error response");
}
Json result(ProjectSession& session, std::string_view method, const Json& parameters,
            bool allow_nested_failure = false) {
    const auto reply = take(session.request(method, parameters.dump()));
    const auto envelope = Json::parse(reply.json);
    if (envelope.contains("error"))
        throw Failure{decode_error(envelope.at("error"))};
    if (reply.failed && !allow_nested_failure)
        fail(ErrorCode::execution_failed, "Prefab operation returned an unknown outcome");
    return envelope.at("result");
}
Json revision_json(SceneRevision value) {
    return {{"scene_instance", std::to_string(value.scene_instance)},
            {"revision", std::to_string(value.revision)}};
}
std::uint64_t decimal(const Json& value) {
    const auto& text = value.get_ref<const std::string&>();
    std::uint64_t number = 0;
    const auto converted = std::from_chars(text.data(), text.data() + text.size(), number);
    if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size())
        fail(ErrorCode::execution_failed, "Prefab reply returned an invalid revision");
    return number;
}
SceneRevision revision_value(const Json& value) {
    return {decimal(value.at("scene_instance")), decimal(value.at("revision"))};
}
AuthoredEntityId entity_id(const Json& value) {
    return AuthoredEntityId{take(StableId::parse(value.get_ref<const std::string&>()))};
}
std::string checked_path(std::string_view path) {
    if (path.size() > 4096)
        fail(ErrorCode::invalid_path, "Prefab path exceeds its byte bound");
    auto parsed = take(ProjectPath::parse(path));
    if (!parsed.text().ends_with(".sharkprefab"))
        fail(ErrorCode::invalid_path, "Prefab sources require the .sharkprefab suffix");
    return parsed.text();
}
std::string canonical_source(std::string_view text) {
    const auto decoded = take(tools::decode_prefab_document(text));
    return take(tools::encode_prefab_document(decoded.document));
}
} // namespace

struct PrefabPanel::Impl {
    struct Pending {
        Json identity;
        std::string parameters;
        AuthoredEntityId root;
    };
    ProjectSession& session;
    const std::thread::id thread{std::this_thread::get_id()};
    std::string path;
    std::string text;
    std::string baseline;
    std::string notice;
    std::optional<std::string> token;
    std::optional<std::string> preparation;
    std::optional<AuthoredEntityId> root;
    SceneRevision prepared_revision{};
    std::optional<Pending> pending;
    bool loaded{false};
    bool is_new{false};
    bool uncertain{false};

    explicit Impl(ProjectSession& owner) : session(owner) {}
    void check_thread() const {
        if (thread != std::this_thread::get_id())
            fail(ErrorCode::conflict, "Prefab panel operations require the creating thread");
    }
    void require_open() const {
        check_thread();
        if (!session.state_current() || !session.state().open)
            fail(ErrorCode::conflict, "Refresh and open a project before editing prefabs");
    }
    bool file_changes() const noexcept {
        return loaded && (is_new || text != baseline || uncertain);
    }
    bool has_changes() const noexcept {
        return file_changes() || preparation.has_value() || pending.has_value();
    }
    void release(std::string_view method, const std::optional<std::string>& value) {
        if (value && session.state().open) {
            const auto field = method == "prefab.release" ? "revision" : "preparation";
            try {
                static_cast<void>(result(session, method, {{field, *value}}));
            } catch (const Failure& failure) {
                // Tokens expire when their project owner closes. A known stale
                // release must not strand a portable panel after reopen.
                if (failure.error.code != ErrorCode::conflict)
                    throw;
            }
        }
    }
    void release_preparation() {
        check_thread();
        if (pending)
            fail(ErrorCode::conflict, "Reconcile the original instance mutation first");
        release("prefab.release_preparation", preparation);
        preparation.reset();
        root.reset();
    }
    void adopt_read(Json read, std::string next_path) {
        auto next_text = read.at("document").get<std::string>();
        auto next_token = read.at("revision").get<std::string>();
        if (next_text.size() > tools::source_maximum_bytes)
            fail(ErrorCode::execution_failed, "Prefab observation exceeds the source byte bound");
        auto next_baseline = next_text;
        try {
            release("prefab.release", token);
        } catch (...) {
            try {
                release("prefab.release", next_token);
            } catch (...) {
            }
            throw;
        }
        path = std::move(next_path);
        text = std::move(next_text);
        baseline = std::move(next_baseline);
        token = std::move(next_token);
        loaded = true;
        is_new = false;
        uncertain = false;
        notice = "Loaded exact prefab source revision.";
    }
    SceneEditReceipt decode_mutation(const Json& value) {
        if (!pending || value.at("mutation_id") != pending->identity)
            fail(ErrorCode::execution_failed, "Prefab reply returned another mutation identity");
        const auto state = value.at("state").get<std::string>();
        const auto& outcome = value.at("outcome");
        if (!outcome.at("ok").get<bool>()) {
            auto error = decode_error(outcome.at("error"));
            if (state == "not_admitted" || state == "not_recorded")
                pending.reset();
            throw Failure{std::move(error)};
        }
        const auto& retained = outcome.at("value");
        if ((state != "recorded" && state != "replayed" && state != "retained") ||
            retained.at("mutation_id") != pending->identity)
            fail(ErrorCode::execution_failed, "Prefab returned an inconsistent retained outcome");
        const auto& operation = retained.at("result");
        if (!operation.at("ok").get<bool>()) {
            auto error = decode_error(operation.at("error"));
            pending.reset();
            throw Failure{std::move(error)};
        }
        SceneEditReceipt receipt{pending->root,
                                 operation.at("value").at("changed").get<bool>(),
                                 revision_value(retained.at("observed_revision")),
                                 {}};
        std::string next_notice = "Instance created. One scene undo removes the complete instance.";
        pending.reset();
        notice.swap(next_notice);
        try {
            release_preparation();
        } catch (...) {
            // The original edit is known. Keep the unused token for explicit
            // cleanup, but never replace success with a release failure.
        }
        try {
            take(session.refresh());
        } catch (Failure& failure) {
            receipt.refresh_error = std::move(failure.error);
        } catch (...) {
            receipt.refresh_error = Error{ErrorCode::execution_failed,
                                          "Instance created; project status refresh failed",
                                          {.operation = "editor.prefab.refresh"}};
        }
        return receipt;
    }
};

PrefabPanel::PrefabPanel(ProjectSession& session) : impl_(std::make_unique<Impl>(session)) {}
PrefabPanel::~PrefabPanel() = default;
bool PrefabPanel::loaded() const noexcept {
    return impl_->loaded;
}
bool PrefabPanel::has_changes() const noexcept {
    return impl_->has_changes();
}
bool PrefabPanel::uncertain() const noexcept {
    return impl_->uncertain;
}
bool PrefabPanel::instantiation_uncertain() const noexcept {
    return impl_->pending.has_value();
}
bool PrefabPanel::prepared() const noexcept {
    return impl_->preparation.has_value();
}
const std::string& PrefabPanel::path() const noexcept {
    return impl_->path;
}
const std::string& PrefabPanel::text() const noexcept {
    return impl_->text;
}
const std::string& PrefabPanel::notice() const noexcept {
    return impl_->notice;
}
const std::optional<AuthoredEntityId>& PrefabPanel::prepared_root() const noexcept {
    return impl_->root;
}

Result<void> PrefabPanel::open(std::string_view path) {
    return boundary<void>([&] {
        impl_->require_open();
        if (impl_->has_changes())
            fail(ErrorCode::conflict, "Write, release or discard the current prefab draft first");
        auto next_path = checked_path(path);
        auto read = result(impl_->session, "prefab.read", {{"path", next_path}});
        impl_->adopt_read(std::move(read), std::move(next_path));
    });
}
Result<void> PrefabPanel::capture(std::string_view path, AuthoredEntityId selected, AssetId asset,
                                  SceneRevision expected_revision) {
    return boundary<void>([&] {
        impl_->require_open();
        if (impl_->has_changes())
            fail(ErrorCode::conflict, "Write, release or discard the current prefab draft first");
        auto next_path = checked_path(path);
        auto capture = result(impl_->session, "prefab.capture",
                              {{"entity_id", selected.value().to_string()},
                               {"asset_id", asset.value().to_string()},
                               {"expected_revision", revision_json(expected_revision)}});
        auto next_text = capture.at("document").get<std::string>();
        if (next_text.size() > tools::source_maximum_bytes)
            fail(ErrorCode::execution_failed, "Captured prefab exceeds the source byte bound");
        std::string next_notice =
            "Captured source draft. Write its matching asset sidecar in Assets "
            "before publishing this file; these are separate publications.";
        impl_->release("prefab.release", impl_->token);
        impl_->path = std::move(next_path);
        impl_->text = std::move(next_text);
        impl_->baseline.clear();
        impl_->token.reset();
        impl_->loaded = true;
        impl_->is_new = true;
        impl_->uncertain = false;
        impl_->notice = std::move(next_notice);
    });
}
Result<void> PrefabPanel::edit(std::string_view text) {
    return boundary<void>([&] {
        impl_->check_thread();
        if (!impl_->loaded || impl_->uncertain || impl_->pending)
            fail(ErrorCode::conflict, "Open a prefab and reconcile uncertain operations first");
        if (text.size() > tools::authoring_protocol_maximum_request_bytes)
            fail(ErrorCode::invalid_argument,
                 "Prefab draft exceeds the Editor's 64 KiB input bound");
        std::string next(text);
        impl_->release_preparation();
        impl_->text = std::move(next);
    });
}
Result<void> PrefabPanel::write() {
    return boundary<void>([&] {
        impl_->require_open();
        if (!impl_->file_changes() || impl_->uncertain || impl_->pending)
            fail(ErrorCode::conflict, "There is no writable prefab source draft");
        impl_->release_preparation();
        auto next_text = canonical_source(impl_->text);
        auto next_baseline = next_text;
        Json parameters{{"path", impl_->path},
                        {"document", impl_->text},
                        {"expected_revision", impl_->token ? Json(*impl_->token) : Json(nullptr)}};
        impl_->uncertain = true;
        try {
            auto written = result(impl_->session, "prefab.write", parameters);
            if (!written.at("publication").at("published").get<bool>())
                fail(ErrorCode::execution_failed, "Prefab publication outcome is unknown");
            auto next_token = written.at("revision").get<std::string>();
            auto next_notice = "Prefab written (" +
                               written.at("publication").at("durability").get<std::string>() +
                               "). Scene Save and undo do not affect this file.";
            impl_->token = std::move(next_token);
            impl_->text = std::move(next_text);
            impl_->baseline = std::move(next_baseline);
            impl_->notice = std::move(next_notice);
            impl_->is_new = false;
            impl_->uncertain = false;
        } catch (const Failure& failure) {
            if (failure.error.code != ErrorCode::execution_failed &&
                failure.error.code != ErrorCode::io_failure)
                impl_->uncertain = false;
            throw;
        }
    });
}
Result<void> PrefabPanel::reconcile() {
    return boundary<void>([&] {
        impl_->check_thread();
        if (!impl_->loaded || !impl_->uncertain)
            fail(ErrorCode::conflict, "No uncertain prefab write needs reconciliation");
        auto next_text = canonical_source(impl_->text);
        auto next_baseline = next_text;
        auto read = result(impl_->session, "prefab.read", {{"path", impl_->path}});
        auto observed = read.at("document").get<std::string>();
        auto token = read.at("revision").get<std::string>();
        if (observed != next_text) {
            try {
                impl_->release("prefab.release", token);
            } catch (...) {
            }
            fail(ErrorCode::conflict,
                 "Published prefab differs from the draft; preserve or discard "
                 "the draft before reopening");
        }
        std::string next_notice =
            "Exact prefab contents observed; original write durability is unknown.";
        try {
            impl_->release("prefab.release", impl_->token);
        } catch (...) {
        }
        impl_->token = std::move(token);
        impl_->text = std::move(next_text);
        impl_->baseline = std::move(next_baseline);
        impl_->is_new = false;
        impl_->uncertain = false;
        impl_->notice = std::move(next_notice);
    });
}
Result<void> PrefabPanel::prepare() {
    return boundary<void>([&] {
        impl_->require_open();
        if (!impl_->loaded || !impl_->token || impl_->has_changes())
            fail(ErrorCode::conflict, "Open a clean source revision before preparing an instance");
        const auto expected = impl_->session.state().revision;
        auto prepared = result(impl_->session, "prefab.prepare",
                               {{"revision", *impl_->token},
                                {"expected_revision", revision_json(expected)},
                                {"parent_id", nullptr},
                                {"overrides", Json::array()}});
        auto token = prepared.at("preparation").get<std::string>();
        auto root = entity_id(prepared.at("root_entity_id"));
        if (revision_value(prepared.at("revision")) != expected)
            fail(ErrorCode::execution_failed,
                 "Prefab preparation returned a different Scene revision");
        std::string next_notice =
            "Instance prepared with fresh IDs. Instantiate once, or release this draft.";
        impl_->preparation = std::move(token);
        impl_->root = root;
        impl_->prepared_revision = expected;
        impl_->notice = std::move(next_notice);
    });
}
Result<void> PrefabPanel::release_preparation() {
    return boundary<void>([&] { impl_->release_preparation(); });
}
Result<SceneEditReceipt> PrefabPanel::instantiate() {
    return boundary<SceneEditReceipt>([&] {
        impl_->require_open();
        if (!impl_->preparation || !impl_->root || impl_->pending || impl_->file_changes())
            fail(ErrorCode::conflict,
                 "Prepare one clean prefab instance and reconcile any prior mutation");
        const auto status =
            result(impl_->session, "scene.mutation_status",
                   {{"expected_revision", revision_json(impl_->prepared_revision)}});
        if (status.at("next").is_null())
            fail(ErrorCode::conflict, "Authoring mutation identities are exhausted");
        Json parameters{{"preparation", *impl_->preparation}, {"mutation_id", status.at("next")}};
        impl_->pending.emplace(Impl::Pending{status.at("next"), parameters.dump(), *impl_->root});
        try {
            return impl_->decode_mutation(
                result(impl_->session, "prefab.instantiate", parameters, true));
        } catch (const Failure& failure) {
            if (failure.error.code != ErrorCode::execution_failed &&
                failure.error.code != ErrorCode::io_failure &&
                failure.error.context.operation != "authoring.protocol.reconcile")
                impl_->pending.reset();
            throw;
        }
    });
}
Result<SceneEditReceipt> PrefabPanel::reconcile_instantiation() {
    return boundary<SceneEditReceipt>([&] {
        impl_->check_thread();
        if (!impl_->pending)
            fail(ErrorCode::conflict, "There is no original prefab mutation to reconcile");
        return impl_->decode_mutation(result(impl_->session, "scene.mutation_outcome",
                                             {{"mutation_id", impl_->pending->identity}}, true));
    });
}
Result<void> PrefabPanel::discard() {
    return boundary<void>([&] {
        impl_->check_thread();
        if (impl_->pending)
            fail(ErrorCode::conflict,
                 "Reconcile the original prefab mutation before discarding drafts");
        impl_->release_preparation();
        if (impl_->uncertain) {
            try {
                impl_->release("prefab.release", impl_->token);
            } catch (...) {
            }
        } else {
            impl_->release("prefab.release", impl_->token);
        }
        impl_->path.clear();
        impl_->text.clear();
        impl_->baseline.clear();
        impl_->notice.clear();
        impl_->token.reset();
        impl_->loaded = false;
        impl_->is_new = false;
        impl_->uncertain = false;
    });
}
} // namespace shark::editor
