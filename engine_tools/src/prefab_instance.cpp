#include "shark/tools/prefab_instance.hpp"
#include "prefab_instance_internal.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace shark::tools {
namespace {

struct Failure {
    Error error;
};

[[noreturn]] void fail(const char* message, std::string path = {}, std::string entity = {}) {
    throw Failure{Error{ErrorCode::invalid_argument,
                        message,
                        {.path = std::move(path), .entity_id = std::move(entity)}}};
}

template <typename T> T take(Result<T> value) {
    if (!value)
        throw Failure{std::move(value).error()};
    return std::move(value).value();
}

template <typename T, typename Function>
Result<T> boundary(const char* operation, Function&& function) {
    try {
        return function();
    } catch (Failure& failure) {
        failure.error.context.operation = operation;
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Prefab instance preparation failed",
                     {.operation = operation}};
    }
}

void apply_overrides(PrefabDocument& prefab, const std::vector<PrefabEntityOverride>& overrides) {
    if (overrides.size() > prefab.entities.size())
        fail("Prefab override count exceeds its entity count", "/overrides");
    std::map<AuthoredEntityId, std::size_t> indices;
    for (std::size_t index = 0; index < prefab.entities.size(); ++index)
        indices.emplace(prefab.entities[index].id, index);
    std::set<AuthoredEntityId> seen;
    for (std::size_t index = 0; index < overrides.size(); ++index) {
        const auto& value = overrides[index];
        const auto path = "/overrides/" + std::to_string(index);
        const auto found = indices.find(value.entity);
        if (found == indices.end())
            fail("Prefab override target is not in this asset", path + "/entity",
                 value.entity.value().to_string());
        if (!seen.insert(value.entity).second)
            fail("Prefab override target is duplicated", path + "/entity",
                 value.entity.value().to_string());
        if (!value.name && !value.transform)
            fail("Prefab override must replace Name or local Transform", path,
                 value.entity.value().to_string());
        auto& target = prefab.entities[found->second];
        if (value.name) {
            if (value.name->value.size() > source_maximum_string_bytes)
                fail("Prefab override Name exceeds its byte bound", path + "/name");
            auto validated = validate_name(*value.name);
            if (!validated) {
                auto error = std::move(validated).error();
                error.context.path = path + "/name";
                error.context.entity_id = value.entity.value().to_string();
                throw Failure{std::move(error)};
            }
            target.name = *value.name;
        }
        if (value.transform)
            target.transform = *value.transform;
    }
}

template <typename Generate>
InstantiatePrefabCommand prepare(const PrefabDocument& source, const PrefabInstanceOptions& options,
                                 Generate&& generate) {
    if (options.overrides.size() > source_maximum_entities)
        fail("Prefab override count exceeds its bound", "/overrides");
    auto prefab = take(validate_prefab_document(source));
    auto overridden = prefab;
    apply_overrides(overridden, options.overrides);
    static_cast<void>(take(validate_prefab_document(overridden)));
    if (options.parent &&
        std::any_of(prefab.entities.begin(), prefab.entities.end(),
                    [](const auto& entity) { return entity.rigid_body.has_value(); }))
        fail("Rigid body prefab roots cannot have an external parent", "/parent");
    std::set<AuthoredEntityId> used;
    for (const auto& entity : prefab.entities)
        used.insert(entity.id);
    if (options.parent)
        used.insert(*options.parent);
    std::vector<PrefabEntityRemap> remap;
    remap.reserve(prefab.entities.size());
    std::optional<AuthoredEntityId> root;
    for (const auto& entity : prefab.entities) {
        std::optional<AuthoredEntityId> fresh;
        for (std::size_t attempt = 0; attempt < 8; ++attempt) {
            const AuthoredEntityId candidate{take(generate())};
            if (used.insert(candidate).second) {
                fresh = candidate;
                break;
            }
        }
        if (!fresh)
            fail("Prefab entity ID generation exhausted collision attempts", "/remap");
        remap.push_back({entity.id, *fresh});
        if (entity.id == prefab.root)
            root = *fresh;
    }
    return {*root, std::move(prefab), std::move(remap), options.parent, options.overrides};
}

} // namespace

Result<InstantiatePrefabCommand> prepare_prefab_instance(const PrefabDocument& prefab,
                                                         const PrefabInstanceOptions& options) {
    return boundary<InstantiatePrefabCommand>("prefab.instance.prepare", [&] {
        return prepare(prefab, options, [] { return generate_stable_id(); });
    });
}

Result<InstantiatePrefabCommand> prepare_prefab_instance(const PrefabDocument& prefab,
                                                         const PrefabInstanceOptions& options,
                                                         IdEntropy& entropy) {
    return boundary<InstantiatePrefabCommand>("prefab.instance.prepare", [&] {
        return prepare(prefab, options, [&] { return generate_stable_id(entropy); });
    });
}

Result<detail::ExpandedPrefabInstance>
detail::expand_prefab_instance(const InstantiatePrefabCommand& command) {
    return boundary<ExpandedPrefabInstance>("prefab.instance.validate", [&] {
        if (command.remap.size() > source_maximum_entities ||
            command.overrides.size() > source_maximum_entities)
            fail("Prefab instance input exceeds its entity bound");
        auto prefab = take(validate_prefab_document(command.prefab));
        if (command.remap.size() != prefab.entities.size())
            fail("Prefab entity mapping must cover the complete asset", "/remap");
        apply_overrides(prefab, command.overrides);
        prefab = take(validate_prefab_document(prefab));
        std::set<AuthoredEntityId> sources;
        for (const auto& entity : prefab.entities)
            sources.insert(entity.id);
        std::map<AuthoredEntityId, AuthoredEntityId> mapping;
        std::set<AuthoredEntityId> destinations;
        for (const auto& pair : command.remap) {
            if (!sources.contains(pair.source) ||
                !mapping.emplace(pair.source, pair.instance).second ||
                sources.contains(pair.instance) || !destinations.insert(pair.instance).second)
                fail("Prefab mapping must have unique local sources and fresh destinations",
                     "/remap");
        }
        if (mapping.at(prefab.root) != command.id)
            fail("Prefab root does not match its mapped destination", "/id");
        if (command.parent && destinations.contains(*command.parent))
            fail("An external parent cannot be an instance entity", "/parent");
        ExpandedPrefabInstance result;
        result.entities = std::move(prefab.entities);
        std::map<AuthoredEntityId, std::size_t> indices;
        for (std::size_t index = 0; index < result.entities.size(); ++index) {
            auto& entity = result.entities[index];
            const auto local = entity.id;
            entity.id = mapping.at(local);
            if (entity.parent)
                entity.parent = mapping.at(*entity.parent);
            else
                entity.parent = command.parent;
            if (entity.rigid_body && entity.parent)
                fail("Rigid bodies must remain instance roots", "/parent",
                     entity.id.value().to_string());
            indices.emplace(entity.id, index);
        }
        std::vector<std::vector<std::size_t>> children(result.entities.size());
        result.ordered.reserve(result.entities.size());
        for (std::size_t index = 0; index < result.entities.size(); ++index) {
            const auto& entity = result.entities[index];
            if (entity.id == command.id)
                result.ordered.push_back(index);
            else
                children[indices.at(*entity.parent)].push_back(index);
        }
        for (std::size_t position = 0; position < result.ordered.size(); ++position) {
            const auto& found = children[result.ordered[position]];
            result.ordered.insert(result.ordered.end(), found.begin(), found.end());
        }
        return result;
    });
}

} // namespace shark::tools
