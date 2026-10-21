#include "shark/tools/authored_scene.hpp"

#include <exception>
#include <map>
#include <new>
#include <thread>
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

} // namespace

struct AuthoredScene::Impl {
    // Check this immutable field before touching any owner-managed mutable state.
    const std::thread::id owner{std::this_thread::get_id()};
    const ProjectDocument project;
    const SceneLimits limits;
    std::unique_ptr<ActiveScene> active;

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

Result<std::reference_wrapper<Scene>> AuthoredScene::scene() {
    return boundary<std::reference_wrapper<Scene>>("source.scene.access", [&] {
        static_cast<void>(impl_->read_status());
        return std::ref(*impl_->active->scene);
    });
}

Result<std::reference_wrapper<const Scene>> AuthoredScene::scene() const {
    return boundary<std::reference_wrapper<const Scene>>("source.scene.access", [&] {
        static_cast<void>(impl_->read_status());
        return std::cref(*impl_->active->scene);
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

} // namespace shark::tools
