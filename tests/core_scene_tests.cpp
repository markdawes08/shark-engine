#include <shark/scene.hpp>

#include "scene_generation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

template <typename Id> Id id(std::uint32_t value) {
    std::string text = "00000000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[value & 0xfU];
        value >>= 4U;
    }
    auto parsed = shark::StableId::parse(text);
    REQUIRE(parsed);
    return Id{parsed.value()};
}

shark::AuthorityStamp authority(const std::uint64_t epoch = 1) {
    return {id<shark::RegionId>(10), id<shark::RegionInstanceId>(11), epoch};
}

shark::EntitySeed authored(const std::uint32_t value) {
    return {id<shark::AuthoredEntityId>(value)};
}

shark::EntitySeed gameplay(const std::uint32_t value) {
    return {id<shark::WorldEntityId>(value)};
}

shark::EntitySeed replica(const std::uint32_t value, const std::uint64_t epoch = 1) {
    return {id<shark::WorldEntityId>(value), authority(epoch)};
}

std::unique_ptr<shark::Scene>
scene(shark::SceneScope scope = shark::AuthoredSceneScope{id<shark::SceneId>(1)},
      const shark::SceneLimits limits = {}) {
    auto result = shark::Scene::create(std::move(scope), limits);
    REQUIRE(result);
    return std::move(result).value();
}

shark::EntityHandle create(shark::Scene& owner, shark::EntitySeed seed) {
    auto result = owner.create_entity(std::move(seed));
    REQUIRE(result);
    return result.value();
}

shark::EntityReference reference(const shark::Scene& owner, const shark::EntityHandle handle) {
    auto result = owner.reference(handle);
    REQUIRE(result);
    return result.value();
}

shark::SceneStats stats(const shark::Scene& owner) {
    auto result = owner.stats();
    REQUIRE(result);
    return result.value();
}

template <typename T> void rejected(const shark::Result<T>& result) {
    REQUIRE_FALSE(result);
    CHECK_FALSE(result.error().context.operation.empty());
}

static_assert(!std::is_copy_constructible_v<shark::Scene>);
static_assert(!std::is_move_constructible_v<shark::Scene>);
static_assert(!std::is_same_v<shark::AuthoredEntityId, shark::WorldEntityId>);
static_assert(!std::is_same_v<shark::RegionId, shark::RegionInstanceId>);

} // namespace

TEST_CASE("scene token and slot generation exhaustion never wrap into old handles", "[scene]") {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t generation = maximum - 1;
    CHECK(shark::detail::advance_entity_generation(generation));
    CHECK(generation == maximum);
    CHECK_FALSE(shark::detail::advance_entity_generation(generation));
    CHECK(generation == maximum);
    CHECK_FALSE(shark::detail::advance_entity_generation(generation));

    std::atomic<std::uint64_t> next{maximum - 6};
    std::array<std::array<std::uint64_t, 2>, 4> issued{};
    std::array<std::thread, 4> workers;
    for (std::size_t index = 0; index < workers.size(); ++index) {
        workers[index] = std::thread([&, index] {
            for (auto& token : issued[index]) {
                token = shark::detail::claim_scene_instance(next);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    std::vector<std::uint64_t> flattened;
    for (const auto& row : issued) {
        flattened.insert(flattened.end(), row.begin(), row.end());
    }
    std::sort(flattened.begin(), flattened.end());
    REQUIRE(flattened.size() == 8);
    CHECK(flattened.front() == 0);
    for (std::size_t index = 1; index < flattened.size(); ++index) {
        CHECK(flattened[index] == maximum - 7 + index);
    }
    CHECK(next.load() == 0);
    CHECK(shark::detail::claim_scene_instance(next) == 0);
    CHECK(shark::detail::claim_scene_instance(next) == 0);
}

TEST_CASE("scene configuration validates storage bounds and authority incarnation", "[scene]") {
    const auto scope = shark::AuthoredSceneScope{id<shark::SceneId>(1)};
    const std::array invalid{
        shark::SceneLimits{0, 1},
        shark::SceneLimits{1, 0},
        shark::SceneLimits{1'000'001, 1},
        shark::SceneLimits{1, 65'537},
        shark::SceneLimits{std::numeric_limits<std::size_t>::max(), 1},
        shark::SceneLimits{1, std::numeric_limits<std::size_t>::max()},
    };
    for (const auto limits : invalid) {
        const auto result = shark::Scene::create(scope, limits);
        rejected(result);
        CHECK(result.error().code == shark::ErrorCode::invalid_configuration);
    }
    const auto invalid_region =
        shark::Scene::create(shark::RegionSceneScope{id<shark::WorldId>(2), authority(0)});
    rejected(invalid_region);
    auto minimal = scene(scope, {1, 1});
    const auto initial = stats(*minimal);
    CHECK(initial.instance != 0);
    CHECK(initial.revision == 0);
    CHECK(initial.live_entities == 0);
    CHECK(initial.pending_mutations == 0);
    CHECK(initial.retired_slots == 0);
    CHECK_FALSE(initial.closed);
}

TEST_CASE("authored scene identity rejects gameplay and authority metadata", "[scene]") {
    auto owner = scene();
    const auto handle = create(*owner, authored(20));
    const auto saved = reference(*owner, handle);
    CHECK(saved.identity == authored(20).identity);
    CHECK(saved.scope == shark::SceneScope{shark::AuthoredSceneScope{id<shark::SceneId>(1)}});
    CHECK_FALSE(saved.authority);
    REQUIRE(owner->resolve(saved));
    CHECK(owner->resolve(saved).value() == handle);
    REQUIRE(owner->find(authored(20).identity));
    CHECK(owner->find(authored(20).identity).value() == handle);

    rejected(owner->create_entity(gameplay(20)));
    rejected(owner->defer_create(gameplay(21)));
    auto invalid = authored(21);
    invalid.replica_authority = authority();
    rejected(owner->create_entity(invalid));
    rejected(owner->defer_create(invalid));
    rejected(owner->find(gameplay(20).identity));
    rejected(owner->find(authored(99).identity));
    CHECK(stats(*owner).revision == 1);
    CHECK(stats(*owner).pending_mutations == 0);

    auto altered = saved;
    altered.identity = authored(21).identity;
    rejected(owner->resolve(altered));
    altered = saved;
    altered.scope = shark::AuthoredSceneScope{id<shark::SceneId>(2)};
    rejected(owner->resolve(altered));
    altered = saved;
    altered.authority = authority();
    rejected(owner->resolve(altered));
    CHECK(reference(*owner, handle) == saved);
}

TEST_CASE("region entities derive immutable authority from their scene scope", "[scene]") {
    const shark::RegionSceneScope scope{id<shark::WorldId>(2), authority(7)};
    auto owner = scene(scope);
    const auto handle = create(*owner, gameplay(20));
    const auto saved = reference(*owner, handle);
    CHECK(saved.scope == shark::SceneScope{scope});
    REQUIRE(saved.authority);
    CHECK(*saved.authority == scope.authority);
    REQUIRE(owner->resolve(saved));
    CHECK(owner->resolve(saved).value() == handle);
    rejected(owner->create_entity(authored(21)));
    rejected(owner->defer_create(authored(21)));
    rejected(owner->create_entity(replica(21, 7)));
    rejected(owner->defer_create(replica(21, 7)));
    rejected(owner->find(authored(20).identity));

    auto altered = saved;
    altered.authority->epoch = 8;
    rejected(owner->resolve(altered));
    altered = saved;
    altered.authority.reset();
    rejected(owner->resolve(altered));
    altered = saved;
    std::get<shark::RegionSceneScope>(altered.scope).authority.epoch = 8;
    rejected(owner->resolve(altered));
    altered = saved;
    std::get<shark::RegionSceneScope>(altered.scope).authority.instance =
        id<shark::RegionInstanceId>(12);
    rejected(owner->resolve(altered));
    altered = saved;
    std::get<shark::RegionSceneScope>(altered.scope).authority.region = id<shark::RegionId>(12);
    rejected(owner->resolve(altered));
    altered = saved;
    std::get<shark::RegionSceneScope>(altered.scope).world = id<shark::WorldId>(3);
    rejected(owner->resolve(altered));
    CHECK(reference(*owner, handle) == saved);

    auto successor = scene(shark::RegionSceneScope{id<shark::WorldId>(2), authority(8)});
    const auto successor_handle = create(*successor, gameplay(20));
    CHECK(successor_handle.scene_instance != handle.scene_instance);
    rejected(successor->resolve(saved));
    rejected(successor->reference(handle));
}

TEST_CASE("replicas require positive source authority and reject reconnect references", "[scene]") {
    const shark::ReplicaSceneScope scope{id<shark::WorldId>(2), id<shark::ReplicaSessionId>(3)};
    auto owner = scene(scope);
    rejected(owner->create_entity(gameplay(20)));
    rejected(owner->defer_create(gameplay(20)));
    rejected(owner->create_entity(authored(20)));
    rejected(owner->create_entity(replica(20, 0)));
    rejected(owner->defer_create(replica(20, 0)));
    const auto handle = create(*owner, replica(20, 7));
    const auto saved = reference(*owner, handle);
    CHECK(saved.scope == shark::SceneScope{scope});
    REQUIRE(saved.authority);
    CHECK(*saved.authority == authority(7));
    REQUIRE(owner->resolve(saved));
    CHECK(owner->resolve(saved).value() == handle);

    auto altered = saved;
    std::get<shark::ReplicaSceneScope>(altered.scope).session = id<shark::ReplicaSessionId>(4);
    rejected(owner->resolve(altered));
    altered = saved;
    std::get<shark::ReplicaSceneScope>(altered.scope).world = id<shark::WorldId>(4);
    rejected(owner->resolve(altered));
    altered = saved;
    altered.authority->instance = id<shark::RegionInstanceId>(12);
    rejected(owner->resolve(altered));
    altered = saved;
    altered.authority->region = id<shark::RegionId>(12);
    rejected(owner->resolve(altered));
    altered = saved;
    altered.authority->epoch = 8;
    rejected(owner->resolve(altered));
    altered = saved;
    altered.authority.reset();
    rejected(owner->resolve(altered));

    REQUIRE(owner->destroy_entity(handle));
    const auto replacement = create(*owner, replica(20, 8));
    CHECK(replacement != handle);
    rejected(owner->resolve(saved));
    CHECK(reference(*owner, replacement).authority == std::optional{authority(8)});

    REQUIRE(owner->close());
    auto reconnected =
        scene(shark::ReplicaSceneScope{id<shark::WorldId>(2), id<shark::ReplicaSessionId>(4)});
    const auto new_handle = create(*reconnected, replica(20, 8));
    CHECK(new_handle.scene_instance != handle.scene_instance);
    rejected(reconnected->reference(handle));
    rejected(reconnected->resolve(saved));
    REQUIRE(reconnected->find(gameplay(20).identity));
    CHECK(reconnected->find(gameplay(20).identity).value() == new_handle);
}

TEST_CASE("scene handles check instance slot generation and active lifetime", "[scene]") {
    auto owner = scene();
    auto other = scene();
    const auto original = create(*owner, authored(20));
    const auto original_reference = reference(*owner, original);
    const auto foreign = create(*other, authored(20));
    CHECK(original.scene_instance != foreign.scene_instance);
    rejected(owner->reference(foreign));
    rejected(owner->destroy_entity(foreign));
    rejected(owner->defer_destroy(foreign));
    rejected(owner->resolve(reference(*other, foreign)));

    const std::array malformed{
        shark::EntityHandle{},
        shark::EntityHandle{0, original.slot, original.generation},
        shark::EntityHandle{original.scene_instance, std::numeric_limits<std::uint32_t>::max(),
                            original.generation},
        shark::EntityHandle{original.scene_instance, original.slot, 0},
        shark::EntityHandle{original.scene_instance, original.slot, original.generation + 1},
    };
    for (const auto handle : malformed) {
        rejected(owner->reference(handle));
        rejected(owner->destroy_entity(handle));
        rejected(owner->defer_destroy(handle));
    }
    CHECK(stats(*owner).revision == 1);
    REQUIRE(owner->destroy_entity(original));
    rejected(owner->reference(original));
    rejected(owner->resolve(original_reference));
    rejected(owner->find(authored(20).identity));
    const auto replacement = create(*owner, authored(20));
    CHECK(replacement.scene_instance == original.scene_instance);
    CHECK(replacement.slot == original.slot);
    CHECK(replacement.generation > original.generation);
    rejected(owner->reference(original));
    rejected(owner->destroy_entity(original));
    rejected(owner->defer_destroy(original));
    rejected(owner->resolve(original_reference));
    CHECK(stats(*owner).revision == 3);

    REQUIRE(owner->close());
    owner.reset();
    auto reloaded = scene();
    const auto reloaded_handle = create(*reloaded, authored(20));
    CHECK(reloaded_handle.scene_instance != original.scene_instance);
    rejected(reloaded->reference(original));
    rejected(reloaded->reference(replacement));
    rejected(reloaded->resolve(original_reference));
}

TEST_CASE("scene entity capacity and duplicate rejection preserve state", "[scene]") {
    auto owner = scene(shark::AuthoredSceneScope{id<shark::SceneId>(1)}, {2, 4});
    const auto first = create(*owner, authored(20));
    rejected(owner->create_entity(authored(20)));
    const auto second = create(*owner, authored(21));
    rejected(owner->create_entity(authored(22)));
    CHECK(stats(*owner).revision == 2);
    CHECK(stats(*owner).live_entities == 2);
    CHECK(reference(*owner, first).identity == authored(20).identity);
    CHECK(reference(*owner, second).identity == authored(21).identity);
    REQUIRE(owner->destroy_entity(first));
    const auto third = create(*owner, authored(22));
    CHECK(third.slot == first.slot);
    CHECK(third.generation != first.generation);
    CHECK(stats(*owner).live_entities == 2);
}

TEST_CASE("scene rejection diagnostics identify the operation and supplied persistent entity",
          "[scene]") {
    auto owner = scene(shark::AuthoredSceneScope{id<shark::SceneId>(1)}, {2, 1});
    const auto first = create(*owner, authored(20));
    const auto saved = reference(*owner, first);
    const auto authored_id = id<shark::AuthoredEntityId>(20).value().to_string();
    const auto gameplay_id = id<shark::WorldEntityId>(21).value().to_string();
    const auto duplicate = owner->create_entity(authored(20));
    rejected(duplicate);
    CHECK(duplicate.error().context.operation == "scene.create-entity");
    CHECK(duplicate.error().context.entity_id == authored_id);
    const auto domain = owner->create_entity(gameplay(21));
    rejected(domain);
    CHECK(domain.error().context.operation == "scene.create-entity");
    CHECK(domain.error().context.entity_id == gameplay_id);
    const auto deferred_domain = owner->defer_create(gameplay(21));
    rejected(deferred_domain);
    CHECK(deferred_domain.error().context.operation == "scene.defer-create");
    CHECK(deferred_domain.error().context.entity_id == gameplay_id);
    auto invalid_authority = authored(20);
    invalid_authority.replica_authority = authority();
    const auto authority_error = owner->defer_create(invalid_authority);
    rejected(authority_error);
    CHECK(authority_error.error().context.operation == "scene.defer-create");
    CHECK(authority_error.error().context.entity_id == authored_id);
    const auto missing = owner->find(gameplay(21).identity);
    rejected(missing);
    CHECK(missing.error().context.operation == "scene.find");
    CHECK(missing.error().context.entity_id == gameplay_id);
    auto altered = saved;
    altered.authority = authority();
    const auto scoped = owner->resolve(altered);
    rejected(scoped);
    CHECK(scoped.error().context.operation == "scene.resolve");
    CHECK(scoped.error().context.entity_id == authored_id);

    REQUIRE(owner->defer_create(authored(20)));
    const auto full = owner->defer_create(authored(21));
    rejected(full);
    CHECK(full.error().context.operation == "scene.defer-create");
    CHECK(full.error().context.entity_id == id<shark::AuthoredEntityId>(21).value().to_string());
    const auto report = owner->apply_deferred();
    REQUIRE(report);
    REQUIRE(report.value().outcomes.size() == 1);
    const auto& deferred_duplicate = report.value().outcomes.front().result;
    rejected(deferred_duplicate);
    CHECK(deferred_duplicate.error().context.operation == "scene.apply-deferred.create");
    CHECK(deferred_duplicate.error().context.entity_id == authored_id);
    REQUIRE(owner->destroy_entity(first));
    const auto stale = owner->resolve(saved);
    rejected(stale);
    CHECK(stale.error().context.operation == "scene.resolve");
    CHECK(stale.error().context.entity_id == authored_id);
    REQUIRE(owner->close());
    const auto closed = owner->create_entity(authored(20));
    rejected(closed);
    CHECK(closed.error().context.operation == "scene.create-entity");
    CHECK(closed.error().context.entity_id == authored_id);
}

TEST_CASE("scene visits own copied values in stable slot order and defer structural writes",
          "[scene]") {
    auto owner = scene();
    const auto first = create(*owner, authored(30));
    const auto middle = create(*owner, authored(20));
    const auto last = create(*owner, authored(10));
    REQUIRE(owner->destroy_entity(middle));
    const auto reused = create(*owner, authored(40));
    CHECK(reused.slot == middle.slot);
    const auto before = stats(*owner);
    std::vector<shark::EntityReference> visited;
    const auto visit =
        owner->visit_entities([&](const shark::EntityReference& item) -> shark::Result<void> {
            visited.push_back(item);
            REQUIRE(owner->reference(item.handle));
            REQUIRE(owner->resolve(item));
            REQUIRE(owner->find(item.identity));
            REQUIRE(owner->stats());
            rejected(owner->create_entity(authored(50)));
            rejected(owner->destroy_entity(item.handle));
            rejected(owner->apply_deferred());
            rejected(owner->close());
            rejected(owner->visit_entities(
                [](const shark::EntityReference&) -> shark::Result<void> { return {}; }));
            REQUIRE(owner->defer_destroy(item.handle));
            if (item.handle == first) {
                REQUIRE(owner->defer_create(authored(50)));
            }
            return {};
        });
    REQUIRE(visit);
    REQUIRE(visited.size() == 3);
    CHECK(visited[0].handle == first);
    CHECK(visited[1].handle == reused);
    CHECK(visited[2].handle == last);
    CHECK(stats(*owner).revision == before.revision);
    CHECK(stats(*owner).live_entities == 3);
    CHECK(stats(*owner).pending_mutations == 4);
    const auto applied = owner->apply_deferred();
    REQUIRE(applied);
    REQUIRE(applied.value().outcomes.size() == 4);
    for (const auto& outcome : applied.value().outcomes) {
        REQUIRE(outcome.result);
    }
    CHECK(stats(*owner).live_entities == 1);
    REQUIRE(owner->find(authored(50).identity));
    for (const auto& item : visited) {
        rejected(owner->resolve(item));
    }
    // Copies remain readable after mutation; no borrowed registry storage escaped.
    CHECK(visited[0].identity == authored(30).identity);
    CHECK(visited[1].identity == authored(40).identity);
    CHECK(visited[2].identity == authored(10).identity);
    CHECK_FALSE(owner->visit_entities({}));
}

TEST_CASE("deferred scene mutations report ordered individual outcomes and preserve valid work",
          "[scene]") {
    auto owner = scene(shark::AuthoredSceneScope{id<shark::SceneId>(1)}, {2, 8});
    const auto first = create(*owner, authored(20));
    const auto old = reference(*owner, first);
    const std::array tickets{
        owner->defer_create(authored(20)), owner->defer_destroy(first),
        owner->defer_destroy(first),       owner->defer_create(authored(20)),
        owner->defer_create(authored(21)), owner->defer_create(authored(22)),
    };
    for (std::size_t index = 0; index < tickets.size(); ++index) {
        REQUIRE(tickets[index]);
        CHECK(tickets[index].value() != 0);
        if (index != 0) {
            CHECK(tickets[index].value() > tickets[index - 1].value());
        }
    }
    rejected(owner->create_entity(authored(23)));
    rejected(owner->destroy_entity(first));
    CHECK(stats(*owner).revision == 1);
    const auto report = owner->apply_deferred();
    REQUIRE(report);
    CHECK(report.value().revision_before == 1);
    CHECK(report.value().revision_after == 4);
    const auto& outcomes = report.value().outcomes;
    REQUIRE(outcomes.size() == tickets.size());
    for (std::size_t index = 0; index < tickets.size(); ++index) {
        CHECK(outcomes[index].ticket == tickets[index].value());
    }
    CHECK(outcomes[0].kind == shark::SceneMutationKind::create);
    rejected(outcomes[0].result);
    CHECK(outcomes[1].kind == shark::SceneMutationKind::destroy);
    REQUIRE(outcomes[1].result);
    CHECK(outcomes[1].result.value() == first);
    CHECK(outcomes[2].kind == shark::SceneMutationKind::destroy);
    rejected(outcomes[2].result);
    REQUIRE(outcomes[3].result);
    CHECK(outcomes[3].result.value().slot == first.slot);
    CHECK(outcomes[3].result.value().generation > first.generation);
    REQUIRE(outcomes[4].result);
    rejected(outcomes[5].result);
    rejected(owner->resolve(old));
    CHECK(stats(*owner).live_entities == 2);
    CHECK(stats(*owner).pending_mutations == 0);
    const auto empty = owner->apply_deferred();
    REQUIRE(empty);
    CHECK(empty.value().outcomes.empty());
    CHECK(empty.value().revision_before == 4);
    CHECK(empty.value().revision_after == 4);
}

TEST_CASE("scene mutation queues enforce limits without consuming failed admission", "[scene]") {
    auto owner = scene(shark::AuthoredSceneScope{id<shark::SceneId>(1)}, {2, 1});
    const auto first = create(*owner, authored(20));
    const auto admitted = owner->defer_destroy(first);
    REQUIRE(admitted);
    rejected(owner->defer_create(authored(21)));
    rejected(owner->defer_destroy(first));
    CHECK(stats(*owner).pending_mutations == 1);
    CHECK(stats(*owner).live_entities == 1);
    CHECK(stats(*owner).revision == 1);
    const auto report = owner->apply_deferred();
    REQUIRE(report);
    REQUIRE(report.value().outcomes.size() == 1);
    CHECK(report.value().outcomes.front().ticket == admitted.value());
    REQUIRE(report.value().outcomes.front().result);
    const auto next = owner->defer_create(authored(21));
    REQUIRE(next);
    CHECK(next.value() > admitted.value());
    REQUIRE(owner->apply_deferred());
    CHECK(stats(*owner).live_entities == 1);
}

TEST_CASE("failed scene visitors cancel only their new deferred requests and restore access",
          "[scene]") {
    auto owner = scene();
    const auto first = create(*owner, authored(20));
    create(*owner, authored(21));
    const auto existing = owner->defer_create(authored(22));
    REQUIRE(existing);
    shark::SceneMutationTicket canceled = 0;
    const shark::Error error{shark::ErrorCode::invalid_argument,
                             "fixture visitor failure",
                             {.operation = "scene.test.visitor"}};
    const auto failed =
        owner->visit_entities([&](const shark::EntityReference& item) -> shark::Result<void> {
            const auto queued = owner->defer_destroy(item.handle);
            REQUIRE(queued);
            canceled = queued.value();
            return error;
        });
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == error.code);
    CHECK(failed.error().context.operation == error.context.operation);
    CHECK(stats(*owner).pending_mutations == 1);
    CHECK(stats(*owner).live_entities == 2);
    CHECK(stats(*owner).revision == 2);
    REQUIRE(owner->reference(first));
    const auto applied = owner->apply_deferred();
    REQUIRE(applied);
    REQUIRE(applied.value().outcomes.size() == 1);
    CHECK(applied.value().outcomes.front().ticket == existing.value());
    REQUIRE(applied.value().outcomes.front().result);
    const auto later = owner->defer_destroy(first);
    REQUIRE(later);
    CHECK(later.value() > canceled);
    REQUIRE(owner->apply_deferred());
    REQUIRE(owner->visit_entities(
        [](const shark::EntityReference&) -> shark::Result<void> { return {}; }));
    REQUIRE(owner->create_entity(authored(23)));
}

TEST_CASE("throwing scene visitors restore owner phase and leave earlier work intact", "[scene]") {
    auto owner = scene();
    const auto first = create(*owner, authored(20));
    const auto existing = owner->defer_create(authored(21));
    REQUIRE(existing);
    shark::SceneMutationTicket canceled = 0;
    const auto result =
        owner->visit_entities([&](const shark::EntityReference&) -> shark::Result<void> {
            const auto queued = owner->defer_destroy(first);
            REQUIRE(queued);
            canceled = queued.value();
            throw std::runtime_error("fixture visitor exception");
        });
    rejected(result);
    CHECK(result.error().code == shark::ErrorCode::execution_failed);
    CHECK(stats(*owner).pending_mutations == 1);
    CHECK(stats(*owner).revision == 1);
    REQUIRE(owner->reference(first));
    const auto applied = owner->apply_deferred();
    REQUIRE(applied);
    REQUIRE(applied.value().outcomes.size() == 1);
    CHECK(applied.value().outcomes.front().ticket == existing.value());
    const auto later = owner->defer_destroy(first);
    REQUIRE(later);
    CHECK(later.value() > canceled);
    REQUIRE(owner->apply_deferred());
    REQUIRE(owner->create_entity(authored(22)));

    const auto nonstandard = owner->visit_entities(
        [](const shark::EntityReference&) -> shark::Result<void> { throw 7; });
    rejected(nonstandard);
    REQUIRE(owner->close());
}

TEST_CASE("every scene operation rejects another thread before touching owner state", "[scene]") {
    auto owner = scene();
    const auto first = create(*owner, authored(20));
    const auto saved = reference(*owner, first);
    const auto next_seed = authored(21);
    std::array<bool, 10> rejected_calls{};
    std::thread worker([&] {
        rejected_calls[0] = !owner->create_entity(next_seed);
        rejected_calls[1] = !owner->destroy_entity(first);
        rejected_calls[2] = !owner->reference(first);
        rejected_calls[3] = !owner->resolve(saved);
        rejected_calls[4] = !owner->find(saved.identity);
        rejected_calls[5] = !owner->stats();
        rejected_calls[6] = !owner->defer_create(next_seed);
        rejected_calls[7] = !owner->defer_destroy(first);
        rejected_calls[8] = !owner->apply_deferred();
        rejected_calls[9] = !owner->close();
    });
    worker.join();
    for (const bool was_rejected : rejected_calls) {
        CHECK(was_rejected);
    }
    bool visitor_ran = false;
    bool visit_rejected = false;
    std::thread visiting_worker([&] {
        visit_rejected =
            !owner->visit_entities([&](const shark::EntityReference&) -> shark::Result<void> {
                visitor_ran = true;
                return {};
            });
    });
    visiting_worker.join();
    CHECK(visit_rejected);
    CHECK_FALSE(visitor_ran);
    CHECK(stats(*owner).revision == 1);
    CHECK(stats(*owner).live_entities == 1);
    CHECK(stats(*owner).pending_mutations == 0);
    CHECK_FALSE(stats(*owner).closed);
    REQUIRE(owner->resolve(saved));
    REQUIRE(owner->create_entity(next_seed));
}

TEST_CASE("closing a scene cancels queued work invalidates all references and is idempotent",
          "[scene]") {
    auto owner = scene();
    const auto first = create(*owner, authored(20));
    const auto saved = reference(*owner, first);
    REQUIRE(owner->defer_create(authored(21)));
    REQUIRE(owner->defer_destroy(first));
    REQUIRE(owner->close());
    const auto closed = stats(*owner);
    CHECK(closed.closed);
    CHECK(closed.live_entities == 0);
    CHECK(closed.pending_mutations == 0);
    rejected(owner->reference(first));
    rejected(owner->resolve(saved));
    rejected(owner->find(authored(20).identity));
    rejected(owner->create_entity(authored(20)));
    rejected(owner->destroy_entity(first));
    rejected(owner->defer_create(authored(20)));
    rejected(owner->defer_destroy(first));
    rejected(owner->apply_deferred());
    rejected(owner->visit_entities(
        [](const shark::EntityReference&) -> shark::Result<void> { return {}; }));
    REQUIRE(owner->close());
    CHECK(stats(*owner).instance == closed.instance);
    CHECK(stats(*owner).revision == closed.revision);
}

TEST_CASE("fixed seed scene churn never revives destroyed identities or stale references",
          "[scene]") {
    constexpr std::uint32_t seed = 0x07a51dU;
    INFO("seed=" << seed);
    std::mt19937 random(seed);
    auto owner = scene(shark::AuthoredSceneScope{id<shark::SceneId>(1)}, {32, 32});
    std::vector<shark::EntityReference> live;
    std::vector<shark::EntityReference> dead;
    std::uint64_t revision = 0;
    std::uint32_t next_identity = 100;
    for (std::size_t step = 0; step < 600; ++step) {
        if (live.empty() || (live.size() < 32 && random() % 2U == 0)) {
            const auto handle = create(*owner, authored(next_identity++));
            live.push_back(reference(*owner, handle));
        } else {
            const auto index = static_cast<std::size_t>(random()) % live.size();
            REQUIRE(owner->destroy_entity(live[index].handle));
            dead.push_back(live[index]);
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
        }
        ++revision;
        CHECK(stats(*owner).revision == revision);
        CHECK(stats(*owner).live_entities == live.size());
        for (const auto& item : live) {
            REQUIRE(owner->resolve(item));
            CHECK(owner->resolve(item).value() == item.handle);
        }
        if (!dead.empty()) {
            const auto& stale = dead[static_cast<std::size_t>(random()) % dead.size()];
            rejected(owner->resolve(stale));
            rejected(owner->reference(stale.handle));
            rejected(owner->find(stale.identity));
        }
    }
    for (const auto& stale : dead) {
        rejected(owner->resolve(stale));
    }
}
