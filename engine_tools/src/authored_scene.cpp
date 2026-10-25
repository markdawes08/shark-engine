#include "shark/tools/authored_scene.hpp"

#include "authored_scene_test_access.hpp"
#include "scene_edit_access.hpp"

#include <algorithm>
#include <exception>
#include <map>
#include <new>
#include <set>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace shark::tools {
namespace {

struct ActivationFailure {
    Error error;
};

[[noreturn]] void fail(ErrorCode code, const char* message, std::string path = {},
                       std::string entity = {}) {
    throw ActivationFailure{
        Error{code, message, {.path = std::move(path), .entity_id = std::move(entity)}}};
}

template <typename T> T checked(Result<T> result) {
    if (!result) {
        throw ActivationFailure{std::move(result).error()};
    }
    return std::move(result).value();
}

void checked(Result<void> result) {
    if (!result) {
        throw ActivationFailure{std::move(result).error()};
    }
}

template <typename T, typename Function>
Result<T> boundary(const char* operation, Function&& function) {
    try {
        return function();
    } catch (ActivationFailure& failure) {
        failure.error.context.operation = operation;
        return std::move(failure.error);
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::execution_failed,
                     "source scene allocation failed",
                     {.operation = operation}};
    } catch (...) {
        return Error{
            ErrorCode::execution_failed, "source scene operation failed", {.operation = operation}};
    }
}

void validate_limits(SceneLimits limits) {
    if (limits.max_entities == 0 || limits.max_entities > source_maximum_entities ||
        limits.max_pending_mutations == 0 || limits.max_pending_mutations > 65536 ||
        limits.max_hierarchy_depth == 0 ||
        limits.max_hierarchy_depth > source_maximum_hierarchy_depth) {
        fail(ErrorCode::invalid_configuration,
             "authored scene limits must fit source and Core bounds");
    }
}

void validate_references(const ProjectDocument& project, const SceneDocument& scene) {
    if (scene.project != project.project) {
        fail(ErrorCode::invalid_configuration, "scene belongs to a different project",
             "/project_id");
    }
    bool catalogued = false;
    for (const auto& entry : project.scenes) {
        if (entry.scene == scene.scene) {
            catalogued = true;
            break;
        }
    }
    if (!catalogued) {
        fail(ErrorCode::invalid_configuration, "scene is missing from the project catalog",
             "/scene_id");
    }
    if (scene.world_origin && scene.world_origin->world != project.world) {
        fail(ErrorCode::invalid_configuration, "scene origin belongs to a different world",
             "/world_origin/world_id");
    }
}

struct RetainedEntity {
    EntityHandle handle;
    AuthoredEntityId id;
    std::string extras;
};

// Only source metadata is retained beside the one active registry. Components
// and hierarchy are always captured from the Scene owner, never this sidecar.
struct ActiveScene {
    std::unique_ptr<Scene> scene;
    SceneId id;
    std::optional<WorldPosition> origin;
    std::string extras;
    std::vector<RetainedEntity> entities;
    SceneRevision baseline;
};

std::unique_ptr<ActiveScene> prepare(const ProjectDocument& project, const SceneDocument& candidate,
                                     SceneLimits limits) {
    auto document = checked(validate_scene_document(candidate));
    validate_references(project, document);
    if (document.entities.size() > limits.max_entities) {
        fail(ErrorCode::conflict, "source entities exceed the authored scene capacity",
             "/entities");
    }
    auto scene = checked(Scene::create(AuthoredSceneScope{document.scene}, limits));
    const auto count = document.entities.size();
    std::map<AuthoredEntityId, std::size_t> indices;
    for (std::size_t index = 0; index < count; ++index) {
        indices.emplace(document.entities[index].id, index);
    }
    std::vector<std::vector<std::size_t>> children(count);
    std::vector<std::optional<std::size_t>> parents(count);
    std::vector<std::size_t> ordered;
    ordered.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& entity = document.entities[index];
        if (entity.parent) {
            const auto parent = indices.at(*entity.parent);
            parents[index] = parent;
            children[parent].push_back(index);
        } else {
            ordered.push_back(index);
        }
    }
    std::vector<std::optional<EntityHandle>> handles(count);
    for (std::size_t position = 0; position < ordered.size(); ++position) {
        const auto index = ordered[position];
        const auto& entity = document.entities[index];
        const auto parent = parents[index] ? handles[*parents[index]] : std::nullopt;
        auto created =
            scene->create_entity({entity.id, std::nullopt, entity.name, entity.transform, parent});
        if (!created) {
            auto error = std::move(created).error();
            error.context.path = "/entities/" + std::to_string(index);
            error.context.entity_id = entity.id.value().to_string();
            throw ActivationFailure{std::move(error)};
        }
        handles[index] = created.value();
        for (const auto child : children[index]) {
            ordered.push_back(child);
        }
    }
    if (ordered.size() != count) {
        fail(ErrorCode::invalid_configuration, "source hierarchy cannot be resolved", "/entities");
    }
    std::vector<RetainedEntity> retained;
    retained.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        retained.push_back({handles[index].value(), document.entities[index].id,
                            std::move(document.entities[index].extras_json)});
    }
    const auto stats = checked(scene->stats());
    return std::make_unique<ActiveScene>(ActiveScene{std::move(scene),
                                                     document.scene,
                                                     std::move(document.world_origin),
                                                     std::move(document.extras_json),
                                                     std::move(retained),
                                                     {stats.instance, stats.revision}});
}

void append_identity(const Scene& scene, EntityHandle handle,
                     std::vector<AuthoredEntityId>& affected) {
    const auto reference = checked(scene.reference(handle));
    affected.push_back(std::get<AuthoredEntityId>(reference.identity));
}

void append_subtree(const Scene& scene, EntityHandle root,
                    std::vector<AuthoredEntityId>& affected) {
    std::vector<EntityHandle> pending{root};
    for (std::size_t index = 0; index < pending.size(); ++index) {
        const auto state = checked(scene.entity_state(pending[index]));
        affected.push_back(std::get<AuthoredEntityId>(state.reference.identity));
        pending.insert(pending.end(), state.children.begin(), state.children.end());
    }
}

AuthoringCommandReply apply_command(Scene& scene, SceneLimits limits,
                                    const AuthoringCommand& command,
                                    SceneRevision expected_revision) {
    const auto observed = checked(scene.stats());
    auto outcome = boundary<AuthoringCommandEffect>("authoring.execute", [&] {
        if (expected_revision != SceneRevision{observed.instance, observed.revision}) {
            fail(ErrorCode::conflict, "authored scene revision has changed");
        }
        // Prepare the entire receipt before calling the mutation boundary.
        // No allocation or caller callback follows a successful mutation.
        AuthoringCommandEffect effect{false, {}};
        effect.affected_entities.reserve(limits.max_entities + 2);
        std::visit(
            [&](const auto& request) {
                using Command = std::decay_t<decltype(request)>;
                if constexpr (std::is_same_v<Command, CreateEntityCommand> ||
                              std::is_same_v<Command, SetNameCommand>) {
                    // Reject oversized caller-owned text before copying it
                    // into the Core mutation's owned input.
                    checked(validate_name(request.name));
                }
                std::optional<EntityHandle> target;
                std::optional<EntityHandle> parent;
                if constexpr (!std::is_same_v<Command, CreateEntityCommand>) {
                    target = checked(scene.find(request.id));
                }
                if constexpr (std::is_same_v<Command, CreateEntityCommand> ||
                              std::is_same_v<Command, ReparentEntityCommand>) {
                    if (request.parent) {
                        parent = checked(scene.find(*request.parent));
                    }
                }
                if constexpr (std::is_same_v<Command, CreateEntityCommand> ||
                              std::is_same_v<Command, SetNameCommand>) {
                    effect.affected_entities.push_back(request.id);
                } else {
                    append_subtree(scene, *target, effect.affected_entities);
                }
                if constexpr (std::is_same_v<Command, DeleteEntityCommand> ||
                              std::is_same_v<Command, ReparentEntityCommand>) {
                    if (const auto old_parent = checked(scene.entity_state(*target)).parent) {
                        append_identity(scene, *old_parent, effect.affected_entities);
                    }
                }
                if (parent) {
                    append_identity(scene, *parent, effect.affected_entities);
                }
                std::sort(effect.affected_entities.begin(), effect.affected_entities.end());
                effect.affected_entities.erase(
                    std::unique(effect.affected_entities.begin(), effect.affected_entities.end()),
                    effect.affected_entities.end());
                if constexpr (std::is_same_v<Command, CreateEntityCommand>) {
                    static_cast<void>(checked(scene.create_entity(
                        {request.id, std::nullopt, request.name, request.transform, parent})));
                } else if constexpr (std::is_same_v<Command, DeleteEntityCommand>) {
                    checked(scene.destroy_entity(*target));
                } else if constexpr (std::is_same_v<Command, SetNameCommand>) {
                    checked(scene.set_name(*target, request.name));
                } else if constexpr (std::is_same_v<Command, SetTransformCommand>) {
                    checked(scene.set_transform(*target, request.transform));
                } else {
                    checked(scene.reparent(*target, parent, request.mode));
                }
            },
            command);
        // The owner thread and live Scene cannot change during an immediate
        // Core mutation: no callback or borrowed mutable Scene is exposed.
        // stats() is allocation-free on this already-checked owner thread.
        effect.changed = scene.stats().value().revision != observed.revision;
        if (!effect.changed) {
            effect.affected_entities.clear();
        }
        return effect;
    });
    if (!outcome && outcome.error().context.entity_id.empty()) {
        outcome.error().context.entity_id =
            std::visit([](const auto& request) { return request.id.value().to_string(); }, command);
    }
    const auto stats = scene.stats().value();
    return AuthoringCommandReply{{stats.instance, stats.revision}, std::move(outcome)};
}

} // namespace

struct AuthoredScene::Impl {
    // Check this immutable field before touching any owner-managed mutable state.
    const std::thread::id owner{std::this_thread::get_id()};
    const ProjectDocument project;
    const SceneLimits limits;
    std::unique_ptr<ActiveScene> active;
    std::optional<std::size_t> batch_failure_after;

    Impl(ProjectDocument catalog, SceneLimits bounds, std::unique_ptr<ActiveScene> value)
        : project(std::move(catalog)), limits(bounds), active(std::move(value)) {}

    void check_owner() const {
        if (owner != std::this_thread::get_id()) {
            fail(ErrorCode::conflict, "authored scene access requires its owner thread");
        }
    }

    AuthoredSceneStatus read_status(std::optional<SceneRevision> expected = std::nullopt) const {
        check_owner();
        const auto stats = checked(active->scene->stats());
        if (stats.closed) {
            fail(ErrorCode::conflict, "authored scene is closed");
        }
        const SceneRevision revision{stats.instance, stats.revision};
        if (expected && *expected != revision) {
            fail(ErrorCode::conflict, "authored scene revision has changed");
        }
        return {revision, revision != active->baseline, stats.pending_mutations};
    }

    void check_replacement(SceneRevision expected, UnsavedScenePolicy unsaved) const {
        const auto state = read_status(expected);
        if (state.pending_mutations != 0) {
            fail(ErrorCode::conflict, "scene replacement requires no pending mutations");
        }
        if (state.dirty && unsaved == UnsavedScenePolicy::reject) {
            fail(ErrorCode::conflict, "scene replacement would discard unsaved changes");
        }
    }
};

AuthoredScene::AuthoredScene(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
AuthoredScene::~AuthoredScene() = default;

Result<std::unique_ptr<AuthoredScene>> AuthoredScene::create(const ProjectDocument& project,
                                                             const SceneDocument& scene,
                                                             SceneLimits limits) {
    return boundary<std::unique_ptr<AuthoredScene>>("source.scene.create", [&] {
        validate_limits(limits);
        auto catalog = checked(validate_project_document(project));
        auto active = prepare(catalog, scene, limits);
        return std::unique_ptr<AuthoredScene>(new AuthoredScene(
            std::make_unique<Impl>(std::move(catalog), limits, std::move(active))));
    });
}

Result<AuthoredSceneStatus> AuthoredScene::status() const {
    return boundary<AuthoredSceneStatus>("source.scene.status",
                                         [&] { return impl_->read_status(); });
}

Result<std::reference_wrapper<const Scene>> AuthoredScene::scene() const {
    return boundary<std::reference_wrapper<const Scene>>("source.scene.access", [&] {
        static_cast<void>(impl_->read_status());
        return std::cref(*impl_->active->scene);
    });
}

Result<AuthoringCommandReply> AuthoredScene::execute(const AuthoringCommand& command,
                                                     SceneRevision expected_revision) {
    return boundary<AuthoringCommandReply>("authoring.execute", [&] {
        static_cast<void>(impl_->read_status());
        return apply_command(*impl_->active->scene, impl_->limits, command, expected_revision);
    });
}

Result<AuthoringBatchReply> AuthoredScene::preview(const AuthoringBatch& batch,
                                                   SceneRevision expected_revision) const {
    return evaluate_batch(batch, expected_revision, false);
}

Result<AuthoringBatchReply> AuthoredScene::execute(const AuthoringBatch& batch,
                                                   SceneRevision expected_revision) {
    return evaluate_batch(batch, expected_revision, true);
}

Result<AuthoringBatchReply> AuthoredScene::evaluate_batch(const AuthoringBatch& batch,
                                                          SceneRevision expected_revision,
                                                          bool publish) const {
    const auto* operation = publish ? "authoring.batch.execute" : "authoring.batch.preview";
    auto result = boundary<AuthoringBatchReply>(operation, [&] {
        static_cast<void>(impl_->read_status());
        auto& live = *impl_->active->scene;
        auto outcome = boundary<AuthoringCommandEffect>(operation, [&] {
            static_cast<void>(impl_->read_status(expected_revision));
            if (batch.commands.size() > authoring_maximum_batch_commands) {
                fail(ErrorCode::invalid_argument, "authoring batch command bound exceeded");
            }
            const auto failure_after = std::exchange(impl_->batch_failure_after, std::nullopt);
            std::size_t step = 0;
            const auto allocation_boundary = [&] {
                if (failure_after && step++ == *failure_after) {
                    throw std::bad_alloc{};
                }
            };
            allocation_boundary();
            auto candidate =
                checked(shark::scene_detail::SceneEditAccess::prepare(live, expected_revision));
            allocation_boundary();
            std::set<AuthoredEntityId> affected;
            bool changed = false;
            for (std::size_t index = 0; index < batch.commands.size(); ++index) {
                const auto stats = checked(candidate->stats());
                auto reply = apply_command(*candidate, impl_->limits, batch.commands[index],
                                           {stats.instance, stats.revision});
                if (!reply.outcome) {
                    auto error = std::move(reply.outcome).error();
                    error.context.path = "/commands/" + std::to_string(index);
                    throw ActivationFailure{std::move(error)};
                }
                changed = changed || reply.outcome.value().changed;
                const auto& ids = reply.outcome.value().affected_entities;
                affected.insert(ids.begin(), ids.end());
                allocation_boundary();
            }
            AuthoringCommandEffect effect{changed, {affected.begin(), affected.end()}};
            allocation_boundary();
            if (publish) {
                allocation_boundary();
                // The complete reply payload exists before the allocation-free
                // commit. Commit rechecks live owner, revision and mutation phase.
                static_cast<void>(checked(shark::scene_detail::SceneEditAccess::commit(
                    live, *candidate, expected_revision)));
            }
            return effect;
        });
        if (!outcome) {
            outcome.error().context.request_id = batch.id.value().to_string();
        }
        const auto stats = live.stats().value();
        return AuthoringBatchReply{batch.id, {stats.instance, stats.revision}, std::move(outcome)};
    });
    if (!result) {
        result.error().context.request_id = batch.id.value().to_string();
    }
    return result;
}

Result<SceneQueryPage> AuthoredScene::query_entities(const SceneQueryOptions& options) const {
    return boundary<SceneQueryPage>("authoring.query", [&] {
        static_cast<void>(impl_->read_status());
        return checked(impl_->active->scene->query_entities(options));
    });
}

Result<SceneQueryPage> AuthoredScene::continue_query(const SceneQueryCursor& cursor) const {
    return boundary<SceneQueryPage>("authoring.continue-query", [&] {
        static_cast<void>(impl_->read_status());
        return checked(impl_->active->scene->continue_query(cursor));
    });
}

Result<SceneQueryPage>
AuthoredScene::inspect_entity(AuthoredEntityId id, SceneQueryFields fields,
                              std::optional<SceneRevision> expected_revision) const {
    return boundary<SceneQueryPage>("authoring.inspect", [&] {
        const auto observed = impl_->read_status(expected_revision);
        const auto handle = checked(impl_->active->scene->find(id));
        return checked(impl_->active->scene->inspect_entity(handle, fields, observed.revision));
    });
}

Result<CapturedScene> AuthoredScene::capture(std::optional<SceneRevision> expected_revision) const {
    return boundary<CapturedScene>("source.scene.capture", [&] {
        const auto status = impl_->read_status(expected_revision);
        if (status.pending_mutations != 0) {
            fail(ErrorCode::conflict, "scene capture requires no pending mutations");
        }
        const auto& active = *impl_->active;
        SceneQueryOptions options;
        options.page_size = scene_query_maximum_page_size;
        // Bound each read to one page of slots, including sparse empty pages.
        options.scan_budget = scene_query_maximum_page_size;
        options.expected_revision = status.revision;
        auto page = checked(active.scene->query_entities(options));
        std::vector<SourceEntity> observed;
        std::vector<std::optional<EntityHandle>> parents;
        std::map<EntityHandle, std::size_t> indices;
        for (;;) {
            const auto* scope = std::get_if<AuthoredSceneScope>(&page.scope());
            if (!scope || scope->scene != active.id || page.revision() != status.revision) {
                fail(ErrorCode::conflict, "capture requires the active authored scene incarnation");
            }
            for (const auto& row : page.entities()) {
                if (observed.size() >= source_maximum_entities) {
                    fail(ErrorCode::conflict, "scene capture exceeds the source entity limit",
                         "/entities");
                }
                const auto* id = std::get_if<AuthoredEntityId>(&row.reference.identity);
                if (!id || row.reference.authority || !row.name || !row.transform ||
                    !row.hierarchy) {
                    fail(ErrorCode::invalid_configuration,
                         "capture requires complete authored entity state", "/entities");
                }
                indices.emplace(row.reference.handle, observed.size());
                observed.push_back({*id, std::nullopt, *row.name, row.transform->local, "{}"});
                parents.push_back(row.hierarchy->parent);
            }
            if (!page.next_cursor()) {
                break;
            }
            page = checked(active.scene->continue_query(*page.next_cursor()));
        }
        for (std::size_t index = 0; index < observed.size(); ++index) {
            if (parents[index]) {
                const auto parent = indices.find(*parents[index]);
                if (parent == indices.end()) {
                    fail(ErrorCode::invalid_configuration, "captured parent is missing",
                         "/entities/" + std::to_string(index) + "/parent",
                         observed[index].id.value().to_string());
                }
                observed[index].parent = observed[parent->second].id;
            }
        }
        std::vector<SourceEntity> ordered;
        ordered.reserve(observed.size());
        std::vector<bool> retained(observed.size(), false);
        for (const auto& metadata : active.entities) {
            const auto found = indices.find(metadata.handle);
            if (found == indices.end()) {
                continue;
            }
            const auto index = found->second;
            auto& entity = observed[index];
            if (entity.id != metadata.id) {
                fail(ErrorCode::invalid_id, "retained source identity has changed");
            }
            entity.extras_json = metadata.extras;
            ordered.push_back(std::move(entity));
            retained[index] = true;
        }
        for (std::size_t index = 0; index < observed.size(); ++index) {
            if (!retained[index]) {
                ordered.push_back(std::move(observed[index]));
            }
        }
        auto document = checked(validate_scene_document(
            {impl_->project.project, active.id, active.origin, std::move(ordered), active.extras}));
        CapturedScene result{status.revision, impl_->project, std::move(document), status.dirty};
        const auto final_status = impl_->read_status(status.revision);
        if (final_status.pending_mutations != 0) {
            fail(ErrorCode::conflict, "scene capture requires no pending mutations");
        }
        return result;
    });
}

Result<SceneRevision> AuthoredScene::activate(const SceneDocument& candidate,
                                              SceneRevision expected_revision,
                                              UnsavedScenePolicy unsaved) {
    return boundary<SceneRevision>("source.scene.activate", [&] {
        impl_->check_owner();
        if (unsaved != UnsavedScenePolicy::reject && unsaved != UnsavedScenePolicy::discard) {
            fail(ErrorCode::invalid_argument, "unsupported unsaved scene policy");
        }
        impl_->check_replacement(expected_revision, unsaved);
        auto prepared = prepare(impl_->project, candidate, impl_->limits);
        const auto result = prepared->baseline;
        impl_->check_replacement(expected_revision, unsaved);
        checked(impl_->active->scene->close());
        // Publication cannot allocate or fail after the previous scene closes.
        impl_->active.swap(prepared);
        return result;
    });
}

bool AuthoredScene::acknowledge_saved(SceneRevision revision) noexcept {
    try {
        const auto state = impl_->read_status(revision);
        if (state.pending_mutations != 0) {
            return false;
        }
        impl_->active->baseline = revision;
        return true;
    } catch (...) {
        return false;
    }
}

Result<std::reference_wrapper<Scene>> detail::AuthoredSceneTestAccess::scene(AuthoredScene& owner) {
    return boundary<std::reference_wrapper<Scene>>("source.scene.access", [&] {
        static_cast<void>(owner.impl_->read_status());
        return std::ref(*owner.impl_->active->scene);
    });
}

Result<void> detail::AuthoredSceneTestAccess::fail_batch_after(AuthoredScene& owner,
                                                               std::size_t step) {
    return boundary<void>("source.scene.test-fault", [&] {
        static_cast<void>(owner.impl_->read_status());
        owner.impl_->batch_failure_after = step;
        return Result<void>{};
    });
}

} // namespace shark::tools
