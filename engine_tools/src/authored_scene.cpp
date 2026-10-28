#include "shark/tools/authored_scene.hpp"

#include "authored_scene_test_access.hpp"
#include "scene_edit_access.hpp"

#include <algorithm>
#include <bit>
#include <exception>
#include <limits>
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
        auto created = scene->create_entity({entity.id, std::nullopt, entity.name, entity.transform,
                                             parent, entity.camera, entity.mesh_renderer});
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
                              std::is_same_v<Command, SetNameCommand> ||
                              std::is_same_v<Command, SetCameraCommand> ||
                              std::is_same_v<Command, SetMeshRendererCommand>) {
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
                        {request.id, std::nullopt, request.name, request.transform, parent,
                         request.camera, request.mesh_renderer})));
                } else if constexpr (std::is_same_v<Command, DeleteEntityCommand>) {
                    checked(scene.destroy_entity(*target));
                } else if constexpr (std::is_same_v<Command, SetNameCommand>) {
                    checked(scene.set_name(*target, request.name));
                } else if constexpr (std::is_same_v<Command, SetTransformCommand>) {
                    checked(scene.set_transform(*target, request.transform));
                } else if constexpr (std::is_same_v<Command, SetCameraCommand>) {
                    checked(scene.set_camera(*target, request.camera));
                } else if constexpr (std::is_same_v<Command, SetMeshRendererCommand>) {
                    checked(scene.set_mesh_renderer(*target, request.mesh_renderer));
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

SceneDocument capture_document(const ProjectDocument& project, const ActiveScene& active,
                               const Scene& scene, bool revalidate_source = true) {
    const auto stats = checked(scene.stats());
    if (stats.closed || stats.pending_mutations != 0) {
        fail(ErrorCode::conflict, "capture requires a live committed scene");
    }
    const SceneRevision revision{stats.instance, stats.revision};
    SceneQueryOptions options;
    options.page_size = scene_query_maximum_page_size;
    // Bound each read to one page of slots, including sparse empty pages.
    options.scan_budget = scene_query_maximum_page_size;
    options.expected_revision = revision;
    options.fields.camera = true;
    options.fields.mesh_renderer = true;
    auto page = checked(scene.query_entities(options));
    std::vector<SourceEntity> observed;
    std::vector<std::optional<EntityHandle>> parents;
    std::map<EntityHandle, std::size_t> indices;
    for (;;) {
        const auto* scope = std::get_if<AuthoredSceneScope>(&page.scope());
        if (!scope || scope->scene != active.id || page.revision() != revision) {
            fail(ErrorCode::conflict, "capture requires the active authored scene incarnation");
        }
        for (const auto& row : page.entities()) {
            if (observed.size() >= source_maximum_entities) {
                fail(ErrorCode::conflict, "scene capture exceeds the source entity limit",
                     "/entities");
            }
            const auto* id = std::get_if<AuthoredEntityId>(&row.reference.identity);
            if (!id || row.reference.authority || !row.name || !row.transform || !row.hierarchy) {
                fail(ErrorCode::invalid_configuration,
                     "capture requires complete authored entity state", "/entities");
            }
            indices.emplace(row.reference.handle, observed.size());
            observed.push_back({*id, std::nullopt, *row.name, row.transform->local, "{}",
                                row.camera, row.mesh_renderer});
            parents.push_back(row.hierarchy->parent);
        }
        if (!page.next_cursor()) {
            break;
        }
        page = checked(scene.continue_query(*page.next_cursor()));
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
    SceneDocument document{project.project, active.id, active.origin, std::move(ordered),
                           active.extras};
    if (revalidate_source) {
        return checked(validate_scene_document(document));
    }
    // Internal history only copies canonical extras already validated at activation
    // (or the fixed empty object for a new entity). Core validates components,
    // identity, hierarchy and depth under source-compatible limits. Recheck the
    // aggregate extras budget because new entities add empty objects. Avoid parsing
    // immutable metadata again on the mutation path: the pinned JSON DOM destructor
    // allocates inside noexcept and cannot provide recoverable allocation failure.
    auto extras_bytes = document.extras_json.size();
    for (const auto& entity : document.entities) {
        if (entity.extras_json.size() > source_maximum_total_extras_bytes - extras_bytes) {
            fail(ErrorCode::invalid_configuration, "total source extras exceeds its byte bound");
        }
        extras_bytes += entity.extras_json.size();
    }
    return document;
}

struct HistoryEntry {
    AuthoringHistoryTicket ticket;
    std::size_t command_count;
    std::shared_ptr<const SceneDocument> before;
    std::shared_ptr<const SceneDocument> after;
    std::vector<AuthoredEntityId> replaced;
    std::size_t charge;
};

std::size_t snapshot_charge(const SceneDocument& document) {
    auto charge = sizeof(SceneDocument) + document.entities.capacity() * sizeof(SourceEntity) +
                  document.extras_json.capacity() + 1;
    for (const auto& entity : document.entities) {
        charge += entity.name.value.capacity() + 1 + entity.extras_json.capacity() + 1;
    }
    return charge;
}

struct HistoryRestoration {
    std::vector<RetainedEntity> metadata;
    AuthoringCommandEffect effect;
};

struct RetainedMutation {
    AuthoringMutationRequest request;
    std::shared_ptr<const AuthoringMutationOutcome> outcome;
    std::size_t charge;
};

// Existing effects hold at most the union of two source scenes; diagnostics are
// fixed engine messages/paths/IDs, never an echo of caller-owned text. Reserve
// conservatively before dispatch, so retaining the decision cannot allocate.
constexpr std::size_t mutation_outcome_reservation =
    2 * source_maximum_entities * sizeof(AuthoredEntityId) + 64 * 1024;

bool same_number(double left, double right) noexcept {
    return std::bit_cast<std::uint64_t>(left) == std::bit_cast<std::uint64_t>(right);
}

bool same_transform_input(const Transform& left, const Transform& right) noexcept {
    return same_number(left.translation.x, right.translation.x) &&
           same_number(left.translation.y, right.translation.y) &&
           same_number(left.translation.z, right.translation.z) &&
           same_number(left.rotation.x, right.rotation.x) &&
           same_number(left.rotation.y, right.rotation.y) &&
           same_number(left.rotation.z, right.rotation.z) &&
           same_number(left.rotation.w, right.rotation.w) &&
           same_number(left.scale.x, right.scale.x) && same_number(left.scale.y, right.scale.y) &&
           same_number(left.scale.z, right.scale.z);
}

bool same_camera_input(const std::optional<Camera>& left,
                       const std::optional<Camera>& right) noexcept {
    if (left.has_value() != right.has_value())
        return false;
    if (!left)
        return true;
    return left->projection == right->projection &&
           same_number(left->vertical_fov_radians, right->vertical_fov_radians) &&
           same_number(left->orthographic_height, right->orthographic_height) &&
           same_number(left->near_clip, right->near_clip) &&
           same_number(left->far_clip, right->far_clip);
}

bool same_command_input(const AuthoringCommand& left, const AuthoringCommand& right) {
    return std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            const auto* other = std::get_if<T>(&right);
            if (!other || value.id != other->id)
                return false;
            if constexpr (std::is_same_v<T, CreateEntityCommand>) {
                return value.name.value == other->name.value && value.parent == other->parent &&
                       same_transform_input(value.transform, other->transform) &&
                       same_camera_input(value.camera, other->camera) &&
                       value.mesh_renderer == other->mesh_renderer;
            } else if constexpr (std::is_same_v<T, SetNameCommand>) {
                return value.name.value == other->name.value;
            } else if constexpr (std::is_same_v<T, SetTransformCommand>) {
                return same_transform_input(value.transform, other->transform);
            } else if constexpr (std::is_same_v<T, SetCameraCommand>) {
                return same_camera_input(value.camera, other->camera);
            } else if constexpr (std::is_same_v<T, SetMeshRendererCommand>) {
                return value.mesh_renderer == other->mesh_renderer;
            } else if constexpr (std::is_same_v<T, ReparentEntityCommand>) {
                return value.parent == other->parent && value.mode == other->mode;
            } else {
                return true;
            }
        },
        left);
}

bool same_mutation_input(const AuthoringMutationRequest& left,
                         const AuthoringMutationRequest& right) {
    if (left.expected_revision != right.expected_revision)
        return false;
    return std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            const auto* other = std::get_if<T>(&right.mutation);
            if (!other)
                return false;
            if constexpr (std::is_same_v<T, AuthoringCommand>) {
                return same_command_input(value, *other);
            } else if constexpr (std::is_same_v<T, AuthoringBatch>) {
                // Batch ID is request correlation, deliberately outside mutation identity.
                if (value.commands.size() != other->commands.size())
                    return false;
                for (std::size_t index = 0; index < value.commands.size(); ++index) {
                    if (!same_command_input(value.commands[index], other->commands[index]))
                        return false;
                }
                return true;
            } else if constexpr (std::is_same_v<T, UndoAuthoringMutation> ||
                                 std::is_same_v<T, RedoAuthoringMutation>) {
                return value.ticket == other->ticket;
            } else {
                return true;
            }
        },
        left.mutation);
}

std::size_t mutation_payload_charge(const AuthoringMutation& mutation, bool capacity) {
    std::size_t charge = sizeof(AuthoringMutationRequest);
    const auto add_name = [&](const Name& name) {
        if (name.value.size() > authoring_maximum_mutation_name_bytes) {
            fail(ErrorCode::invalid_argument, "mutation Name input exceeds its byte bound");
        }
        charge += (capacity ? name.value.capacity() : name.value.size()) + 1;
    };
    const auto command = [&](const AuthoringCommand& value) {
        std::visit(
            [&](const auto& item) {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, CreateEntityCommand> ||
                              std::is_same_v<T, SetNameCommand>) {
                    add_name(item.name);
                }
            },
            value);
    };
    std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, AuthoringCommand>) {
                command(value);
            } else if constexpr (std::is_same_v<T, AuthoringBatch>) {
                if (value.commands.size() > authoring_maximum_batch_commands) {
                    fail(ErrorCode::invalid_argument, "mutation batch command bound exceeded");
                }
                charge += (capacity ? value.commands.capacity() : value.commands.size()) *
                          sizeof(AuthoringCommand);
                for (const auto& item : value.commands)
                    command(item);
            }
        },
        mutation);
    if (charge > authoring_maximum_mutation_payload_bytes) {
        fail(ErrorCode::invalid_argument, "mutation payload exceeds its byte bound");
    }
    return charge;
}

AuthoringMutationReply mutation_rejection(AuthoringMutationId id, SceneRevision revision,
                                          AuthoringMutationState state, const char* message,
                                          const char* operation,
                                          ErrorCode code = ErrorCode::conflict) {
    return {id, state, revision, Error{code, message, {.operation = operation}}};
}

template <typename Checkpoint>
HistoryRestoration restore_document(Scene& candidate, const SceneDocument& target,
                                    const std::vector<AuthoredEntityId>& replaced,
                                    Checkpoint&& checkpoint) {
    std::vector<SceneEntityState> current;
    checked(candidate.visit_entities([&](const EntityReference& reference) -> Result<void> {
        auto value = candidate.entity_state(reference.handle);
        if (!value) {
            return std::move(value).error();
        }
        current.push_back(std::move(value).value());
        return {};
    }));
    std::sort(current.begin(), current.end(), [](const auto& left, const auto& right) {
        return left.depth != right.depth ? left.depth > right.depth
                                         : left.reference.handle.slot < right.reference.handle.slot;
    });
    checkpoint();
    std::map<AuthoredEntityId, const SourceEntity*> targets;
    std::set<AuthoredEntityId> affected;
    for (const auto& entity : target.entities) {
        targets.emplace(entity.id, &entity);
        affected.insert(entity.id);
    }
    // Detach deepest first: every node is a leaf when moved. This avoids
    // temporary composed-world/depth failures and preserves surviving handles.
    for (const auto& entity : current) {
        affected.insert(std::get<AuthoredEntityId>(entity.reference.identity));
        if (entity.parent) {
            checked(candidate.reparent(entity.reference.handle, std::nullopt,
                                       ReparentMode::keep_local));
        }
    }
    checkpoint();
    for (const auto& entity : current) {
        const auto id = std::get<AuthoredEntityId>(entity.reference.identity);
        if (!targets.contains(id) || std::binary_search(replaced.begin(), replaced.end(), id)) {
            checked(candidate.destroy_entity(entity.reference.handle));
        }
    }
    checkpoint();
    std::map<AuthoredEntityId, EntityHandle> handles;
    for (const auto& entity : target.entities) {
        auto found = candidate.find(entity.id);
        if (found) {
            checked(candidate.set_name(found.value(), entity.name));
            checked(candidate.set_transform(found.value(), entity.transform));
            checked(candidate.set_camera(found.value(), entity.camera));
            checked(candidate.set_mesh_renderer(found.value(), entity.mesh_renderer));
            handles.emplace(entity.id, found.value());
        } else {
            handles.emplace(entity.id, checked(candidate.create_entity(
                                           {entity.id, std::nullopt, entity.name, entity.transform,
                                            std::nullopt, entity.camera, entity.mesh_renderer})));
        }
    }
    checkpoint();
    std::map<AuthoredEntityId, std::vector<AuthoredEntityId>> children;
    std::vector<AuthoredEntityId> ordered;
    ordered.reserve(target.entities.size());
    for (const auto& entity : target.entities) {
        if (entity.parent) {
            children[*entity.parent].push_back(entity.id);
        } else {
            ordered.push_back(entity.id);
        }
    }
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        const auto id = ordered[index];
        const auto& entity = *targets.at(id);
        if (entity.parent) {
            checked(candidate.reparent(handles.at(id), handles.at(*entity.parent),
                                       ReparentMode::keep_local));
        }
        if (const auto found = children.find(id); found != children.end()) {
            ordered.insert(ordered.end(), found->second.begin(), found->second.end());
        }
    }
    if (ordered.size() != target.entities.size()) {
        fail(ErrorCode::conflict, "history hierarchy cannot be resolved");
    }
    checkpoint();
    std::vector<RetainedEntity> metadata;
    metadata.reserve(target.entities.size());
    for (const auto& entity : target.entities) {
        metadata.push_back({handles.at(entity.id), entity.id, entity.extras_json});
    }
    return {std::move(metadata), {true, {affected.begin(), affected.end()}}};
}

} // namespace

struct AuthoredScene::Impl {
    // Check this immutable field before touching any owner-managed mutable state.
    const std::thread::id owner{std::this_thread::get_id()};
    const ProjectDocument project;
    const SceneLimits limits;
    std::unique_ptr<ActiveScene> active;
    std::optional<std::size_t> batch_failure_after;
    std::optional<std::size_t> history_failure_after;
    const AuthoringHistoryLimits history_limits;
    std::vector<std::unique_ptr<HistoryEntry>> entries;
    std::size_t cursor{0};
    std::size_t history_bytes{0};
    std::uint64_t next_history_ticket{1};
    SceneRevision history_revision;
    std::shared_ptr<const SceneDocument> cached_snapshot;
    SceneRevision cached_revision{};
    const AuthoringMutationScope mutation_scope;
    const AuthoringMutationLimits mutation_limits;
    std::uint64_t mutation_watermark{0};
    std::vector<std::unique_ptr<RetainedMutation>> mutations;
    std::size_t mutation_bytes{0};
    std::optional<std::size_t> mutation_failure_after;

    Impl(ProjectDocument catalog, SceneLimits bounds, std::unique_ptr<ActiveScene> value,
         AuthoringHistoryLimits history_bounds, AuthoringMutationScope retry_scope,
         AuthoringMutationLimits retry_bounds)
        : project(std::move(catalog)), limits(bounds), active(std::move(value)),
          history_limits(history_bounds), history_revision(active->baseline),
          mutation_scope(retry_scope), mutation_limits(retry_bounds) {
        entries.reserve(history_limits.max_entries);
        mutations.reserve(mutation_limits.max_entries);
    }

    const RetainedMutation* retained_mutation(AuthoringMutationId id) const noexcept {
        for (const auto& value : mutations) {
            if (value->request.id == id)
                return value.get();
        }
        return nullptr;
    }

    void retain_mutation(std::unique_ptr<RetainedMutation> entry) noexcept {
        while (!mutations.empty() &&
               (mutations.size() >= mutation_limits.max_entries ||
                mutation_bytes > mutation_limits.max_retained_bytes - entry->charge)) {
            mutation_bytes -= mutations.front()->charge;
            mutations.erase(mutations.begin());
        }
        mutation_bytes += entry->charge;
        mutation_watermark = entry->request.id.sequence;
        mutations.push_back(std::move(entry));
    }

    void check_history(SceneRevision revision) const {
        if (!entries.empty() && revision != history_revision) {
            fail(ErrorCode::conflict, "authoring history is out of sync; clear it explicitly");
        }
    }

    AuthoringHistoryStatus read_history() const {
        const auto state = read_status();
        std::optional<AuthoringHistoryGroup> undo;
        std::optional<AuthoringHistoryGroup> redo;
        if (cursor != 0) {
            const auto& entry = *entries[cursor - 1];
            undo = AuthoringHistoryGroup{entry.ticket, entry.command_count};
        }
        if (cursor < entries.size()) {
            const auto& entry = *entries[cursor];
            redo = AuthoringHistoryGroup{entry.ticket, entry.command_count};
        }
        return {state.revision, state.revision == history_revision,
                cursor,         entries.size() - cursor,
                history_bytes,  history_limits,
                undo,           redo};
    }

    std::shared_ptr<const SceneDocument> before_snapshot() const {
        const auto state = read_status();
        if (cached_snapshot && cached_revision == state.revision) {
            return cached_snapshot;
        }
        return std::make_shared<const SceneDocument>(
            capture_document(project, *active, *active->scene, false));
    }

    std::unique_ptr<HistoryEntry> prepare_pair(std::shared_ptr<const SceneDocument> before,
                                               std::shared_ptr<const SceneDocument> after,
                                               std::size_t command_count,
                                               std::vector<AuthoredEntityId> replaced = {}) const {
        if (next_history_ticket == 0) {
            fail(ErrorCode::conflict, "authoring history ticket capacity exhausted");
        }
        const auto charge = sizeof(HistoryEntry) + snapshot_charge(*before) +
                            snapshot_charge(*after) +
                            replaced.capacity() * sizeof(AuthoredEntityId);
        if (charge > history_limits.max_retained_bytes) {
            fail(ErrorCode::conflict, "authoring history snapshot exceeds its byte limit");
        }
        return std::make_unique<HistoryEntry>(HistoryEntry{{next_history_ticket},
                                                           command_count,
                                                           std::move(before),
                                                           std::move(after),
                                                           std::move(replaced),
                                                           charge});
    }

    std::unique_ptr<HistoryEntry> prepare_history(const Scene& candidate,
                                                  std::size_t command_count) const {
        auto before = before_snapshot();
        auto after = std::make_shared<const SceneDocument>(
            capture_document(project, *active, candidate, false));
        std::vector<AuthoredEntityId> replaced;
        for (const auto& entity : after->entities) {
            const auto old = active->scene->find(entity.id);
            if (old && old.value() != checked(candidate.find(entity.id))) {
                replaced.push_back(entity.id);
            }
        }
        std::sort(replaced.begin(), replaced.end());
        return prepare_pair(std::move(before), std::move(after), command_count,
                            std::move(replaced));
    }

    void record_history(std::unique_ptr<HistoryEntry> entry, SceneRevision revision) noexcept {
        // Capacity was reserved at owner creation; entry allocation and all
        // rejection decisions precede Core publication. Only moves/cleanup here.
        for (std::size_t index = cursor; index < entries.size(); ++index) {
            history_bytes -= entries[index]->charge;
        }
        entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(cursor), entries.end());
        while (!entries.empty() &&
               (entries.size() >= history_limits.max_entries ||
                history_bytes > history_limits.max_retained_bytes - entry->charge)) {
            history_bytes -= entries.front()->charge;
            entries.erase(entries.begin());
        }
        history_bytes += entry->charge;
        cached_snapshot = entry->after;
        cached_revision = revision;
        entries.push_back(std::move(entry));
        cursor = entries.size();
        next_history_ticket = next_history_ticket == std::numeric_limits<std::uint64_t>::max()
                                  ? 0
                                  : next_history_ticket + 1;
        history_revision = revision;
    }

    void reset_history(SceneRevision revision) noexcept {
        entries.clear();
        cached_snapshot.reset();
        cached_revision = {};
        cursor = 0;
        history_bytes = 0;
        history_revision = revision;
    }

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

Result<std::unique_ptr<AuthoredScene>>
AuthoredScene::create(const ProjectDocument& project, const SceneDocument& scene,
                      SceneLimits limits, AuthoringHistoryLimits history_limits,
                      AuthoringMutationLimits mutation_limits) {
    return boundary<std::unique_ptr<AuthoredScene>>("source.scene.create", [&] {
        validate_limits(limits);
        if (history_limits.max_entries == 0 ||
            history_limits.max_entries > authoring_maximum_history_entries ||
            history_limits.max_retained_bytes == 0 ||
            history_limits.max_retained_bytes > authoring_maximum_history_bytes) {
            fail(ErrorCode::invalid_configuration,
                 "authoring history limits exceed supported bounds");
        }
        if (mutation_limits.max_entries == 0 ||
            mutation_limits.max_entries > authoring_maximum_mutation_entries ||
            mutation_limits.max_retained_bytes == 0 ||
            mutation_limits.max_retained_bytes > authoring_maximum_mutation_bytes) {
            fail(ErrorCode::invalid_configuration,
                 "mutation retention limits exceed supported bounds");
        }
        const AuthoringMutationScope scope{checked(generate_stable_id())};
        auto catalog = checked(validate_project_document(project));
        auto active = prepare(catalog, scene, limits);
        return std::unique_ptr<AuthoredScene>(
            new AuthoredScene(std::make_unique<Impl>(std::move(catalog), limits, std::move(active),
                                                     history_limits, scope, mutation_limits)));
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
    if (std::holds_alternative<DeleteEntityCommand>(command)) {
        return execute_delete(command, expected_revision);
    }
    return evaluate_commands(std::span<const AuthoringCommand>{&command, 1}, expected_revision,
                             true, "authoring.execute");
}

Result<AuthoringCommandReply> AuthoredScene::execute_delete(const AuthoringCommand& command,
                                                            SceneRevision expected_revision) {
    return boundary<AuthoringCommandReply>("authoring.execute", [&] {
        static_cast<void>(impl_->read_status());
        auto& live = *impl_->active->scene;
        auto outcome = boundary<AuthoringCommandEffect>("authoring.execute", [&] {
            static_cast<void>(impl_->read_status(expected_revision));
            impl_->check_history(expected_revision);
            checked(shark::scene_detail::SceneEditAccess::check(live, expected_revision));
            const auto& request = std::get<DeleteEntityCommand>(command);
            const auto handle = checked(live.find(request.id));
            std::vector<AuthoredEntityId> removed;
            append_subtree(live, handle, removed);
            const std::set<AuthoredEntityId> ids(removed.begin(), removed.end());
            auto before = impl_->before_snapshot();
            auto after = std::make_shared<SceneDocument>(*before);
            std::erase_if(after->entities,
                          [&](const auto& entity) { return ids.contains(entity.id); });
            // Subtree removal only reduces a previously validated source value.
            // The immutable projection is history data; Core still owns validation
            // and the immediate atomic deletion, as in the original single API.
            auto entry = impl_->prepare_pair(std::move(before), std::move(after), 1);
            auto reply = apply_command(live, impl_->limits, command, expected_revision);
            if (!reply.outcome) {
                throw ActivationFailure{std::move(reply.outcome).error()};
            }
            impl_->record_history(std::move(entry), reply.observed_revision);
            return std::move(reply.outcome).value();
        });
        if (!outcome && outcome.error().context.entity_id.empty()) {
            outcome.error().context.entity_id =
                std::get<DeleteEntityCommand>(command).id.value().to_string();
        }
        const auto stats = live.stats().value();
        return AuthoringCommandReply{{stats.instance, stats.revision}, std::move(outcome)};
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
    auto result =
        evaluate_commands(batch.commands, expected_revision, publish,
                          publish ? "authoring.batch.execute" : "authoring.batch.preview");
    if (!result) {
        result.error().context.request_id = batch.id.value().to_string();
        return std::move(result).error();
    }
    auto reply = std::move(result).value();
    if (!reply.outcome) {
        reply.outcome.error().context.request_id = batch.id.value().to_string();
    }
    return AuthoringBatchReply{batch.id, reply.observed_revision, std::move(reply.outcome)};
}

Result<AuthoringCommandReply>
AuthoredScene::evaluate_commands(std::span<const AuthoringCommand> commands,
                                 SceneRevision expected_revision, bool publish,
                                 const char* operation) const {
    return boundary<AuthoringCommandReply>(operation, [&] {
        static_cast<void>(impl_->read_status());
        auto& live = *impl_->active->scene;
        auto outcome = boundary<AuthoringCommandEffect>(operation, [&] {
            static_cast<void>(impl_->read_status(expected_revision));
            impl_->check_history(expected_revision);
            if (commands.size() > authoring_maximum_batch_commands) {
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
            for (std::size_t index = 0; index < commands.size(); ++index) {
                const auto stats = checked(candidate->stats());
                auto reply = apply_command(*candidate, impl_->limits, commands[index],
                                           {stats.instance, stats.revision});
                if (!reply.outcome) {
                    auto error = std::move(reply.outcome).error();
                    if (commands.size() != 1 ||
                        std::string_view{operation} != "authoring.execute") {
                        error.context.path = "/commands/" + std::to_string(index);
                    }
                    throw ActivationFailure{std::move(error)};
                }
                changed = changed || reply.outcome.value().changed;
                const auto& ids = reply.outcome.value().affected_entities;
                affected.insert(ids.begin(), ids.end());
                allocation_boundary();
            }
            AuthoringCommandEffect effect{changed, {affected.begin(), affected.end()}};
            allocation_boundary();
            std::unique_ptr<HistoryEntry> entry;
            if (changed) {
                entry = impl_->prepare_history(*candidate, commands.size());
                allocation_boundary();
            }
            if (publish) {
                allocation_boundary();
                const auto revision = checked(shark::scene_detail::SceneEditAccess::commit(
                    live, *candidate, expected_revision));
                if (entry) {
                    impl_->record_history(std::move(entry), revision);
                }
            }
            return effect;
        });
        const auto stats = live.stats().value();
        return AuthoringCommandReply{{stats.instance, stats.revision}, std::move(outcome)};
    });
}

Result<AuthoringHistoryStatus>
AuthoredScene::history(std::optional<SceneRevision> expected_revision) const {
    return boundary<AuthoringHistoryStatus>("authoring.history", [&] {
        static_cast<void>(impl_->read_status(expected_revision));
        return impl_->read_history();
    });
}

Result<AuthoringHistoryStatus> AuthoredScene::clear_history(SceneRevision expected_revision) {
    return boundary<AuthoringHistoryStatus>("authoring.history.clear", [&] {
        static_cast<void>(impl_->read_status(expected_revision));
        // Reuse the complete owner/visit/pending guard before changing history.
        checked(
            shark::scene_detail::SceneEditAccess::check(*impl_->active->scene, expected_revision));
        impl_->reset_history(expected_revision);
        return impl_->read_history();
    });
}

Result<AuthoringMutationStatus>
AuthoredScene::mutation_status(std::optional<SceneRevision> expected_revision) const {
    return boundary<AuthoringMutationStatus>("authoring.mutation.status", [&] {
        const auto state = impl_->read_status(expected_revision);
        std::optional<AuthoringMutationId> next;
        if (impl_->mutation_watermark != std::numeric_limits<std::uint64_t>::max()) {
            next = AuthoringMutationId{impl_->mutation_scope, impl_->mutation_watermark + 1};
        }
        std::optional<AuthoringMutationId> oldest;
        if (!impl_->mutations.empty())
            oldest = impl_->mutations.front()->request.id;
        return AuthoringMutationStatus{
            impl_->mutation_scope,   state.revision,        impl_->mutation_watermark, next, oldest,
            impl_->mutations.size(), impl_->mutation_bytes, impl_->mutation_limits};
    });
}

Result<AuthoringMutationReply> AuthoredScene::mutation_outcome(AuthoringMutationId id) const {
    constexpr auto operation = "authoring.mutation.query";
    return boundary<AuthoringMutationReply>(operation, [&] {
        const auto revision = impl_->read_status().revision;
        if (id.sequence == 0) {
            return mutation_rejection(id, revision, AuthoringMutationState::not_admitted,
                                      "mutation sequence must be nonzero", operation,
                                      ErrorCode::invalid_argument);
        }
        if (const auto* entry = impl_->retained_mutation(id)) {
            return AuthoringMutationReply{id, AuthoringMutationState::retained, revision,
                                          entry->outcome};
        }
        if (id.scope != impl_->mutation_scope || id.sequence <= impl_->mutation_watermark) {
            return mutation_rejection(id, revision, AuthoringMutationState::unknown_outcome,
                                      "mutation outcome is unavailable; reconcile live state",
                                      operation);
        }
        return mutation_rejection(id, revision, AuthoringMutationState::not_recorded,
                                  "mutation was not admitted by this owner", operation);
    });
}

Result<AuthoringMutationReply> AuthoredScene::mutate(const AuthoringMutationRequest& request) {
    constexpr auto operation = "authoring.mutation.execute";
    return boundary<AuthoringMutationReply>(operation, [&] {
        const auto revision = impl_->read_status().revision;
        if (request.id.sequence == 0) {
            return mutation_rejection(request.id, revision, AuthoringMutationState::not_admitted,
                                      "mutation sequence must be nonzero", operation,
                                      ErrorCode::invalid_argument);
        }
        if (request.id.scope != impl_->mutation_scope) {
            return mutation_rejection(request.id, revision, AuthoringMutationState::unknown_outcome,
                                      "mutation belongs to another owner; reconcile live state",
                                      operation);
        }
        const auto* retained = impl_->retained_mutation(request.id);
        if (!retained && request.id.sequence <= impl_->mutation_watermark) {
            return mutation_rejection(request.id, revision, AuthoringMutationState::unknown_outcome,
                                      "mutation outcome expired; reconcile live state", operation);
        }
        if (!retained && (impl_->mutation_watermark == std::numeric_limits<std::uint64_t>::max() ||
                          request.id.sequence != impl_->mutation_watermark + 1)) {
            return mutation_rejection(request.id, revision, AuthoringMutationState::out_of_order,
                                      "mutation sequence must be the next unused value", operation);
        }
        // Check the bounded envelope before comparing or copying caller input.
        auto charge = boundary<std::size_t>(
            operation, [&] { return mutation_payload_charge(request.mutation, false); });
        if (!charge) {
            return AuthoringMutationReply{request.id, AuthoringMutationState::not_admitted,
                                          revision, std::move(charge).error()};
        }
        if (const auto* entry = retained) {
            if (!same_mutation_input(entry->request, request)) {
                return mutation_rejection(request.id, revision,
                                          AuthoringMutationState::payload_mismatch,
                                          "mutation identity has a different payload", operation);
            }
            return AuthoringMutationReply{request.id, AuthoringMutationState::replayed, revision,
                                          entry->outcome};
        }
        if (charge.value() + sizeof(RetainedMutation) + mutation_outcome_reservation >
            impl_->mutation_limits.max_retained_bytes) {
            return mutation_rejection(request.id, revision, AuthoringMutationState::not_admitted,
                                      "mutation cannot fit the retention byte limit", operation);
        }
        const auto failure_after = std::exchange(impl_->mutation_failure_after, std::nullopt);
        std::size_t step = 0;
        const auto allocation_boundary = [&] {
            if (failure_after && step++ == *failure_after)
                throw std::bad_alloc{};
        };
        allocation_boundary();
        auto entry = std::make_unique<RetainedMutation>(RetainedMutation{request, {}, 0});
        allocation_boundary();
        entry->charge = mutation_payload_charge(entry->request.mutation, true) +
                        sizeof(RetainedMutation) + mutation_outcome_reservation;
        if (entry->charge > impl_->mutation_limits.max_retained_bytes) {
            return mutation_rejection(request.id, revision, AuthoringMutationState::not_admitted,
                                      "owned mutation cannot fit the retention byte limit",
                                      operation);
        }
        allocation_boundary();
        auto outcome = std::make_shared<AuthoringMutationOutcome>(AuthoringMutationOutcome{
            request.id, revision, AuthoringCommandEffect{false, {}}, std::nullopt});
        entry->outcome = outcome;
        allocation_boundary();
        // All ledger storage exists before dispatch. Successful publication is
        // followed only by nonthrowing moves, bounded eviction and pointer copies.
        static_assert(std::is_nothrow_move_assignable_v<Result<AuthoringCommandEffect>>);
        allocation_boundary();
        auto dispatched = boundary<void>(operation, [&] {
            allocation_boundary();
            std::visit(
                [&](const auto& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, ClearAuthoringHistoryMutation>) {
                        const bool changed = impl_->cursor != 0 || !impl_->entries.empty();
                        auto result = clear_history(request.expected_revision);
                        if (result) {
                            outcome->history_after = result.value();
                            outcome->result = AuthoringCommandEffect{changed, {}};
                        } else {
                            outcome->result = std::move(result).error();
                        }
                    } else {
                        auto result = [&] {
                            if constexpr (std::is_same_v<T, UndoAuthoringMutation>) {
                                return undo(value.ticket, request.expected_revision);
                            } else if constexpr (std::is_same_v<T, RedoAuthoringMutation>) {
                                return redo(value.ticket, request.expected_revision);
                            } else {
                                return execute(value, request.expected_revision);
                            }
                        }();
                        if (result) {
                            outcome->observed_revision = result.value().observed_revision;
                            outcome->result = std::move(result).value().outcome;
                        } else {
                            outcome->result = std::move(result).error();
                        }
                    }
                },
                entry->request.mutation);
            return Result<void>{};
        });
        // Failure while constructing an operation's owned diagnostic is itself
        // a terminal dispatch failure. Keep it under the admitted identity too.
        if (!dispatched)
            outcome->result = std::move(dispatched).error();
        impl_->retain_mutation(std::move(entry));
        return AuthoringMutationReply{
            request.id, AuthoringMutationState::recorded, outcome->observed_revision,
            std::shared_ptr<const AuthoringMutationOutcome>{std::move(outcome)}};
    });
}

Result<AuthoringHistoryReply> AuthoredScene::undo(AuthoringHistoryTicket ticket,
                                                  SceneRevision expected_revision) {
    return move_history(ticket, expected_revision, false);
}

Result<AuthoringHistoryReply> AuthoredScene::redo(AuthoringHistoryTicket ticket,
                                                  SceneRevision expected_revision) {
    return move_history(ticket, expected_revision, true);
}

Result<AuthoringHistoryReply> AuthoredScene::move_history(AuthoringHistoryTicket ticket,
                                                          SceneRevision expected_revision,
                                                          bool forward) {
    const auto* operation = forward ? "authoring.redo" : "authoring.undo";
    return boundary<AuthoringHistoryReply>(operation, [&] {
        static_cast<void>(impl_->read_status());
        auto& live = *impl_->active->scene;
        auto outcome = boundary<AuthoringCommandEffect>(operation, [&] {
            static_cast<void>(impl_->read_status(expected_revision));
            impl_->check_history(expected_revision);
            if ((!forward && impl_->cursor == 0) ||
                (forward && impl_->cursor == impl_->entries.size())) {
                fail(ErrorCode::conflict, "no history entry is available in this direction");
            }
            const auto index = forward ? impl_->cursor : impl_->cursor - 1;
            const auto& entry = *impl_->entries[index];
            if (ticket != entry.ticket) {
                fail(ErrorCode::conflict, "history ticket does not match the next entry");
            }
            const auto& target_snapshot = forward ? entry.after : entry.before;
            const auto& target = *target_snapshot;
            validate_references(impl_->project, target);
            if (target.scene != impl_->active->id || target.world_origin != impl_->active->origin ||
                target.extras_json != impl_->active->extras) {
                fail(ErrorCode::conflict, "history metadata does not match the active scene");
            }
            const auto failure_after = std::exchange(impl_->history_failure_after, std::nullopt);
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
            auto restored =
                restore_document(*candidate, target, entry.replaced, allocation_boundary);
            allocation_boundary();
            // Prebuild metadata and the complete result, then force one revision
            // for the history movement even when the source values are equal.
            const auto revision = checked(shark::scene_detail::SceneEditAccess::commit(
                live, *candidate, expected_revision, true));
            impl_->active->entities.swap(restored.metadata);
            impl_->cursor = forward ? impl_->cursor + 1 : impl_->cursor - 1;
            impl_->history_revision = revision;
            impl_->cached_snapshot = target_snapshot;
            impl_->cached_revision = revision;
            return std::move(restored.effect);
        });
        const auto stats = live.stats().value();
        return AuthoringHistoryReply{ticket, {stats.instance, stats.revision}, std::move(outcome)};
    });
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
        auto document = capture_document(impl_->project, active, *active.scene);
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
        impl_->reset_history(result);
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

Result<void> detail::AuthoredSceneTestAccess::fail_history_after(AuthoredScene& owner,
                                                                 std::size_t step) {
    return boundary<void>("source.scene.test-history-fault", [&] {
        static_cast<void>(owner.impl_->read_status());
        owner.impl_->history_failure_after = step;
        return Result<void>{};
    });
}

Result<void> detail::AuthoredSceneTestAccess::fail_mutation_after(AuthoredScene& owner,
                                                                  std::size_t step) {
    return boundary<void>("source.scene.test-mutation-fault", [&] {
        static_cast<void>(owner.impl_->read_status());
        owner.impl_->mutation_failure_after = step;
        return Result<void>{};
    });
}

Result<void> detail::AuthoredSceneTestAccess::mutation_watermark(AuthoredScene& owner,
                                                                 std::uint64_t value) {
    return boundary<void>("source.scene.test-mutation-watermark", [&] {
        static_cast<void>(owner.impl_->read_status());
        if (!owner.impl_->mutations.empty() || value < owner.impl_->mutation_watermark) {
            fail(ErrorCode::conflict, "watermark fixture requires an empty ledger and no rewind");
        }
        owner.impl_->mutation_watermark = value;
        return Result<void>{};
    });
}

} // namespace shark::tools
