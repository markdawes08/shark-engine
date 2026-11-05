#include <shark/tools/authored_scene.hpp>
#include <shark/tools/prefab_instance.hpp>

#include "authored_scene_test_access.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace shark;
using namespace shark::tools;

template <typename T> T take(Result<T> result) {
    REQUIRE(result);
    return std::move(result).value();
}

template <typename T> T id(std::uint32_t number) {
    std::string text = "21000000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[number & 0xfU];
        number >>= 4U;
    }
    return T{take(StableId::parse(text))};
}

class SequenceEntropy final : public IdEntropy {
  public:
    Result<void> fill(std::span<std::byte, 16> bytes) override {
        std::fill(bytes.begin(), bytes.end(), std::byte{0});
        bytes[0] = std::byte{0x55};
        auto value = next_++;
        for (std::size_t index = 0; index < 8; ++index) {
            bytes[15 - index] = static_cast<std::byte>(value & 0xffU);
            value >>= 8U;
        }
        return {};
    }

  private:
    std::uint64_t next_{100};
};

ProjectDocument project() {
    return {id<ProjectId>(1),
            id<WorldId>(2),
            {"Prefab project"},
            id<SceneId>(3),
            {{id<SceneId>(3), take(ProjectPath::parse("scenes/start.sharkscene"))}}};
}

SceneDocument source(bool populated = true) {
    SceneDocument result{id<ProjectId>(1), id<SceneId>(3), {}, {}};
    if (populated)
        result.entities.push_back(
            {id<AuthoredEntityId>(90), {}, {"Existing"}, {}, R"({"existing":true})"});
    return result;
}

PrefabDocument prefab() {
    SourceEntity child{id<AuthoredEntityId>(11), id<AuthoredEntityId>(10), {"Lens"}};
    child.transform.translation = {0.0, 2.0, 0.0};
    child.extras_json = R"({"local_reference":"21000000-0000-4000-8000-00000000000a"})";
    child.mesh_renderer = MeshRenderer{id<AssetId>(30), 2, true};
    child.camera = Camera{};
    SourceEntity root{id<AuthoredEntityId>(10), {}, {"Beacon"}};
    root.extras_json = R"({"part":"root"})";
    root.rigid_body = RigidBody{};
    root.script = Script{"scripts/beacon.lua", false};
    // Forward references must survive source order, preparation and history.
    return {id<ProjectId>(1),
            id<AssetId>(20),
            root.id,
            {child, root},
            R"({"asset_note":"keep source"})"};
}

std::unique_ptr<AuthoredScene> owner(SceneLimits limits = {}, bool populated = true) {
    return take(AuthoredScene::create(project(), source(populated), limits));
}

const Scene& live(const AuthoredScene& value) {
    return take(value.scene()).get();
}

SceneRevision revision(const AuthoredScene& value) {
    return take(value.status()).revision;
}

CapturedScene capture(const AuthoredScene& value) {
    return take(value.capture());
}

AuthoredEntityId mapped(const InstantiatePrefabCommand& command, AuthoredEntityId local) {
    const auto found = std::find_if(command.remap.begin(), command.remap.end(),
                                    [&](const auto& entry) { return entry.source == local; });
    REQUIRE(found != command.remap.end());
    return found->instance;
}

const SourceEntity& entity(const CapturedScene& value, AuthoredEntityId wanted) {
    const auto found = std::find_if(value.scene.entities.begin(), value.scene.entities.end(),
                                    [&](const auto& item) { return item.id == wanted; });
    REQUIRE(found != value.scene.entities.end());
    return *found;
}

void same_capture(const CapturedScene& actual, const CapturedScene& expected) {
    CHECK(actual.project == expected.project);
    CHECK(actual.scene == expected.scene);
    CHECK(actual.revision == expected.revision);
    CHECK(actual.dirty == expected.dirty);
}

AuthoringCommandReply execute(AuthoredScene& value, const AuthoringCommand& command) {
    const auto before = revision(value);
    auto reply = take(value.execute(command, before));
    REQUIRE(reply.outcome);
    CHECK(reply.outcome.value().changed);
    CHECK(reply.observed_revision == SceneRevision{before.scene_instance, before.revision + 1});
    return reply;
}

void rejected(AuthoredScene& value, const AuthoringCommand& command) {
    const auto before = capture(value);
    const auto old_history = take(value.history());
    const auto result = take(value.execute(command, before.revision));
    REQUIRE_FALSE(result.outcome);
    CHECK_FALSE(result.outcome.error().context.operation.empty());
    CHECK_FALSE(result.outcome.error().message.empty());
    CHECK(result.observed_revision == before.revision);
    same_capture(capture(value), before);
    const auto after = take(value.history());
    CHECK(after.undo_entries == old_history.undo_entries);
    CHECK(after.redo_entries == old_history.redo_entries);
    CHECK(after.retained_bytes == old_history.retained_bytes);
}

AuthoringMutationRequest request(AuthoredScene& value, const AuthoringCommand& command) {
    const auto status = take(value.mutation_status());
    REQUIRE(status.next);
    return {*status.next, status.revision, command};
}

} // namespace

TEST_CASE("prefab instances receive independent IDs and preserve local content",
          "[prefab-instances]") {
    auto value = owner();
    const auto original = prefab();
    const auto saved = original;
    const auto existing = take(live(*value).find(id<AuthoredEntityId>(90)));
    SequenceEntropy entropy;
    std::set<AuthoredEntityId> identities;
    std::vector<InstantiatePrefabCommand> instances;
    for (std::size_t index = 0; index < 12; ++index) {
        auto command = take(prepare_prefab_instance(original, {}, entropy));
        CHECK(command.id == mapped(command, original.root));
        REQUIRE(command.remap.size() == original.entities.size());
        for (const auto& item : command.remap) {
            CHECK(item.instance != original.root);
            CHECK(item.instance != original.entities.front().id);
            CHECK(identities.insert(item.instance).second);
        }
        execute(*value, command);
        instances.push_back(std::move(command));
    }
    const auto result = capture(*value);
    REQUIRE(result.scene.entities.size() == 25);
    CHECK(take(live(*value).find(id<AuthoredEntityId>(90))) == existing);
    for (const auto& command : instances) {
        const auto child_id = mapped(command, original.entities.front().id);
        const auto& root = entity(result, command.id);
        const auto& child = entity(result, child_id);
        CHECK_FALSE(root.parent);
        CHECK(root.name.value == "Beacon");
        CHECK(root.rigid_body == original.entities.back().rigid_body);
        CHECK(root.script == original.entities.back().script);
        CHECK(root.extras_json == original.entities.back().extras_json);
        CHECK(child.parent == command.id);
        CHECK(child.mesh_renderer == original.entities.front().mesh_renderer);
        CHECK(child.camera == original.entities.front().camera);
        CHECK(child.extras_json == original.entities.front().extras_json);
        const auto root_position =
            std::find_if(result.scene.entities.begin(), result.scene.entities.end(),
                         [&](const auto& e) { return e.id == command.id; });
        REQUIRE(root_position != result.scene.entities.begin());
        CHECK((root_position - 1)->id == child_id);
    }
    execute(*value, SetNameCommand{instances.front().id, {"Changed instance"}});
    const auto changed = capture(*value);
    CHECK(entity(changed, instances.front().id).name.value == "Changed instance");
    CHECK(entity(changed, instances.back().id).name.value == "Beacon");
    CHECK(original == saved);
}

TEST_CASE("prefab overrides preview atomically and become one undo redo group",
          "[prefab-instances]") {
    auto value = owner();
    const auto original = prefab();
    auto moved = Transform{};
    moved.translation = {4.0, 5.0, 6.0};
    PrefabInstanceOptions options;
    options.overrides = {{original.root, Name{"Override root"}, {}},
                         {original.entities.front().id, {}, moved}};
    SequenceEntropy entropy;
    const auto command = take(prepare_prefab_instance(original, options, entropy));
    const auto before = capture(*value);
    const AuthoringBatch batch{id<AuthoringBatchId>(200), {command}};
    const auto preview = take(value->preview(batch, before.revision));
    REQUIRE(preview.outcome);
    CHECK(preview.outcome.value().changed);
    same_capture(capture(*value), before);
    CHECK(take(value->history()).undo_entries == 0);
    const auto committed = execute(*value, command);
    CHECK(committed.outcome.value().affected_entities == preview.outcome.value().affected_entities);
    const auto after = capture(*value);
    CHECK(entity(after, command.id).name.value == "Override root");
    CHECK(entity(after, mapped(command, original.entities.front().id)).transform == moved);
    CHECK(entity(after, command.id).script == original.entities.back().script);
    CHECK(original.entities.back().name.value == "Beacon");
    auto history = take(value->history());
    REQUIRE(history.next_undo);
    CHECK(history.undo_entries == 1);
    CHECK(history.next_undo->command_count == 1);
    const auto old_handle = take(live(*value).find(command.id));
    const auto old_reference = take(live(*value).reference(old_handle));
    auto undo = take(value->undo(history.next_undo->ticket, history.revision));
    REQUIRE(undo.outcome);
    CHECK(capture(*value).scene == before.scene);
    CHECK_FALSE(live(*value).resolve(old_reference));
    history = take(value->history());
    REQUIRE(history.next_redo);
    auto redo = take(value->redo(history.next_redo->ticket, history.revision));
    REQUIRE(redo.outcome);
    CHECK(capture(*value).scene == after.scene);
    CHECK(take(live(*value).find(command.id)) != old_handle);
    CHECK_FALSE(live(*value).resolve(old_reference));

    const auto encoded = take(encode_scene_document(after.scene));
    const auto decoded = take(decode_scene_document(encoded));
    auto reopened = take(AuthoredScene::create(after.project, decoded.document));
    CHECK(capture(*reopened).scene == after.scene);
    CHECK(revision(*reopened).scene_instance != after.revision.scene_instance);
}

TEST_CASE("prefab preparation rejects invalid overrides and entropy failure",
          "[prefab-instances]") {
    const auto original = prefab();
    PrefabInstanceOptions options;
    SECTION("unknown target") {
        options.overrides.push_back({id<AuthoredEntityId>(999), Name{"Unknown"}, {}});
    }
    SECTION("duplicate target") {
        options.overrides = {{original.root, Name{"First"}, {}},
                             {original.root, Name{"Second"}, {}}};
    }
    SECTION("empty override") {
        options.overrides.push_back({original.root, {}, {}});
    }
    SECTION("invalid name") {
        options.overrides.push_back({original.root, Name{"bad\nname"}, {}});
    }
    SECTION("invalid root body scale") {
        auto invalid = Transform{};
        invalid.scale = {1.0, 2.0, 1.0};
        options.overrides.push_back({original.root, {}, invalid});
    }
    SECTION("entropy failure") {
        class FailedEntropy final : public IdEntropy {
          public:
            Result<void> fill(std::span<std::byte, 16>) override {
                return Error{ErrorCode::entropy_unavailable, "Injected entropy failure"};
            }
        } failed;
        const auto prepared = prepare_prefab_instance(original, {}, failed);
        REQUIRE_FALSE(prepared);
        CHECK(prepared.error().code == ErrorCode::entropy_unavailable);
        return;
    }
    SequenceEntropy entropy;
    const auto prepared = prepare_prefab_instance(original, options, entropy);
    REQUIRE_FALSE(prepared);
    CHECK_FALSE(prepared.error().context.operation.empty());
}

TEST_CASE("mutable prefab commands reject invalid maps and preserve redo on failure",
          "[prefab-instances]") {
    auto value = owner();
    SequenceEntropy entropy;
    execute(*value, SetNameCommand{id<AuthoredEntityId>(90), {"Undo me"}});
    const auto history = take(value->history());
    REQUIRE(history.next_undo);
    REQUIRE(take(value->undo(history.next_undo->ticket, history.revision)).outcome);
    auto command = take(prepare_prefab_instance(prefab(), {}, entropy));
    SECTION("missing mapping") {
        command.remap.pop_back();
    }
    SECTION("duplicate destination") {
        command.remap[0].instance = command.remap[1].instance;
    }
    SECTION("duplicate source") {
        command.remap[0].source = command.remap[1].source;
    }
    SECTION("foreign source") {
        command.remap[0].source = id<AuthoredEntityId>(999);
    }
    SECTION("source ID reused") {
        command.remap[0].instance = command.prefab.root;
    }
    SECTION("wrong root ID") {
        command.id = mapped(command, command.prefab.entities.front().id);
    }
    SECTION("existing destination") {
        command.remap[0].instance = id<AuthoredEntityId>(90);
    }
    SECTION("wrong project") {
        command.prefab.project = id<ProjectId>(999);
    }
    SECTION("body attached to parent") {
        command.parent = id<AuthoredEntityId>(90);
    }
    SECTION("invalid override added after preparation") {
        command.overrides.push_back({id<AuthoredEntityId>(999), Name{"unknown"}, {}});
    }
    rejected(*value, command);
}

TEST_CASE("prefab candidate failures leave metadata history and surviving handles intact",
          "[prefab-instances]") {
    SequenceEntropy entropy;
    const auto command = take(prepare_prefab_instance(prefab(), {}, entropy));
    SECTION("capacity") {
        auto value = owner({2, 1024, 256});
        rejected(*value, command);
    }
    SECTION("late batch rejection") {
        auto value = owner();
        const auto before = capture(*value);
        const AuthoringBatch batch{
            id<AuthoringBatchId>(200),
            {command, SetNameCommand{id<AuthoredEntityId>(999), {"Absent"}}}};
        const auto result = take(value->execute(batch, before.revision));
        REQUIRE_FALSE(result.outcome);
        CHECK(result.outcome.error().context.path == "/commands/1");
        same_capture(capture(*value), before);
        CHECK(take(value->history()).undo_entries == 0);
    }
    SECTION("injected candidate allocation boundary") {
        auto value = owner();
        const auto survivor = take(live(*value).find(id<AuthoredEntityId>(90)));
        REQUIRE(detail::AuthoredSceneTestAccess::fail_batch_after(*value, 3));
        rejected(*value, command);
        CHECK(take(live(*value).find(id<AuthoredEntityId>(90))) == survivor);
    }
    SECTION("root under external parent without a body") {
        auto value = owner();
        auto attached = command;
        attached.prefab.entities.back().rigid_body.reset();
        attached.parent = id<AuthoredEntityId>(90);
        execute(*value, attached);
        CHECK(entity(capture(*value), attached.id).parent == attached.parent);
    }
}

TEST_CASE("prefab retries retain the exact mapping and reject altered bounded payloads",
          "[prefab-instances]") {
    auto value = owner();
    SequenceEntropy entropy;
    const auto command = take(prepare_prefab_instance(prefab(), {}, entropy));
    const auto input = request(*value, command);
    const auto first = take(value->mutate(input));
    REQUIRE(first.outcome);
    const auto original = first.outcome.value();
    REQUIRE(original->result);
    CHECK(first.state == AuthoringMutationState::recorded);
    execute(*value, SetNameCommand{id<AuthoredEntityId>(90), {"Unrelated"}});
    const auto before = capture(*value);
    auto replay = take(value->mutate(input));
    REQUIRE(replay.outcome);
    CHECK(replay.state == AuthoringMutationState::replayed);
    CHECK(replay.outcome.value() == original);
    CHECK(replay.observed_revision == before.revision);
    CHECK(take(value->mutation_outcome(input.id)).outcome.value() == original);
    same_capture(capture(*value), before);

    auto changed = input;
    auto& payload =
        std::get<InstantiatePrefabCommand>(std::get<AuthoringCommand>(changed.mutation));
    SECTION("different mapping") {
        payload.remap[0].instance = id<AuthoredEntityId>(999);
    }
    SECTION("opaque extras bytes") {
        payload.prefab.entities.front().extras_json += " ";
    }
    SECTION("signed zero in source transform") {
        payload.prefab.entities.back().transform.translation.x = -0.0;
    }
    SECTION("new override") {
        payload.overrides.push_back({payload.prefab.root, Name{"Override"}, {}});
    }
    SECTION("different parent") {
        payload.parent = id<AuthoredEntityId>(90);
    }
    SECTION("source array order") {
        std::reverse(payload.prefab.entities.begin(), payload.prefab.entities.end());
    }
    const auto mismatch = take(value->mutate(changed));
    CHECK(mismatch.state == AuthoringMutationState::payload_mismatch);
    REQUIRE_FALSE(mismatch.outcome);
    same_capture(capture(*value), before);
}

TEST_CASE("prefab preparation excludes an external parent and bounds repeated ID collisions",
          "[prefab-instances]") {
    auto document = prefab();
    document.entities.back().rigid_body.reset();
    SECTION("external parent overlaps the first entropy result") {
        SequenceEntropy first;
        const auto parent = AuthoredEntityId{take(generate_stable_id(first))};
        auto value = owner();
        execute(*value, CreateEntityCommand{parent, {"Attachment parent"}});
        PrefabInstanceOptions options;
        options.parent = parent;
        SequenceEntropy restarted;
        const auto command = take(prepare_prefab_instance(document, options, restarted));
        for (const auto& remap : command.remap)
            CHECK(remap.instance != parent);
        execute(*value, command);
        const auto captured = capture(*value);
        CHECK(entity(captured, command.id).parent == parent);
    }
    SECTION("entropy repeats a generated ID") {
        class RepeatedEntropy final : public IdEntropy {
          public:
            Result<void> fill(std::span<std::byte, 16> bytes) override {
                ++calls;
                std::fill(bytes.begin(), bytes.end(), std::byte{0});
                bytes[0] = std::byte{0x66};
                return {};
            }
            std::size_t calls{0};
        } repeated;
        const auto command = prepare_prefab_instance(document, {}, repeated);
        REQUIRE_FALSE(command);
        CHECK(repeated.calls > 1);
        CHECK(repeated.calls <= 16);
    }
}

TEST_CASE("mixed prefab batches preserve source order and generation bound metadata",
          "[prefab-instances]") {
    auto value = owner();
    SequenceEntropy entropy;
    const auto command = take(prepare_prefab_instance(prefab(), {}, entropy));
    const auto before = capture(*value);
    const auto old_handle = take(live(*value).find(id<AuthoredEntityId>(90)));
    const auto ordinary = id<AuthoredEntityId>(91);
    AuthoringBatch batch{id<AuthoringBatchId>(200), {}};
    std::vector<AuthoredEntityId> expected_order;
    SECTION("ordinary creation precedes prefab source order") {
        batch.commands = {CreateEntityCommand{ordinary, {"Created first"}}, command};
        expected_order = {id<AuthoredEntityId>(90), ordinary,
                          mapped(command, command.prefab.entities.front().id), command.id};
    }
    SECTION("delete recreate does not retarget old extras") {
        batch.commands = {DeleteEntityCommand{id<AuthoredEntityId>(90)},
                          CreateEntityCommand{id<AuthoredEntityId>(90), {"Replacement"}}, command};
        expected_order = {id<AuthoredEntityId>(90),
                          mapped(command, command.prefab.entities.front().id), command.id};
    }
    const auto result = take(value->execute(batch, before.revision));
    REQUIRE(result.outcome);
    const auto after = capture(*value);
    std::vector<AuthoredEntityId> actual_order;
    for (const auto& current : after.scene.entities)
        actual_order.push_back(current.id);
    CHECK(actual_order == expected_order);
    CHECK(entity(after, command.id).extras_json == command.prefab.entities.back().extras_json);
    if (batch.commands.size() == 3) {
        CHECK(take(live(*value).find(id<AuthoredEntityId>(90))) != old_handle);
        CHECK(entity(after, id<AuthoredEntityId>(90)).name.value == "Replacement");
        CHECK(entity(after, id<AuthoredEntityId>(90)).extras_json == "{}");
    } else {
        CHECK(take(live(*value).find(id<AuthoredEntityId>(90))) == old_handle);
        CHECK(entity(after, ordinary).extras_json == "{}");
    }
    const auto history = take(value->history());
    REQUIRE(history.next_undo);
    CHECK(history.undo_entries == 1);
    CHECK(history.next_undo->command_count == batch.commands.size());
    REQUIRE(take(value->undo(history.next_undo->ticket, history.revision)).outcome);
    CHECK(capture(*value).scene == before.scene);
    const auto undone = take(value->history());
    REQUIRE(undone.next_redo);
    REQUIRE(take(value->redo(undone.next_redo->ticket, undone.revision)).outcome);
    CHECK(capture(*value).scene == after.scene);
}

TEST_CASE("prefabs preserve source capacity independently of the command batch bound",
          "[prefab-instances]") {
    auto large = prefab();
    large.entities.clear();
    large.entities.push_back({large.root, {}, {"Root"}});
    for (std::uint32_t index = 1; index < 300; ++index)
        large.entities.push_back({id<AuthoredEntityId>(1000 + index), large.root});
    auto value = owner({}, false);
    SequenceEntropy entropy;
    const auto command = take(prepare_prefab_instance(large, {}, entropy));
    execute(*value, command);
    CHECK(capture(*value).scene.entities.size() == 300);
    const auto history = take(value->history());
    REQUIRE(history.next_undo);
    CHECK(history.next_undo->command_count == 1);

    // The retained-request byte bound remains independent of source capacity.
    const auto count = authoring_maximum_mutation_payload_bytes /
                           (sizeof(SourceEntity) + sizeof(PrefabEntityRemap)) +
                       1;
    REQUIRE(count <= source_maximum_entities);
    for (std::size_t index = large.entities.size(); index < count; ++index)
        large.entities.push_back(
            {id<AuthoredEntityId>(static_cast<std::uint32_t>(1000 + index)), large.root});
    auto fresh = owner({}, false);
    const auto large_command = take(prepare_prefab_instance(large, {}, entropy));
    const auto before = capture(*fresh);
    const auto result = take(fresh->mutate(request(*fresh, large_command)));
    CHECK(result.state == AuthoringMutationState::not_admitted);
    CHECK_FALSE(result.outcome);
    same_capture(capture(*fresh), before);
    CHECK(take(fresh->mutation_status()).high_watermark == 0);
}

TEST_CASE("prefab authoring preserves visit pending thread and closed owner fences",
          "[prefab-instances]") {
    auto value = owner();
    SequenceEntropy entropy;
    const auto command = take(prepare_prefab_instance(prefab(), {}, entropy));
    auto& raw = take(detail::AuthoredSceneTestAccess::scene(*value)).get();
    const auto before = revision(*value);
    REQUIRE(raw.visit_entities([&](const EntityReference&) -> Result<void> {
        const auto result = take(value->execute(command, before));
        CHECK_FALSE(result.outcome);
        return {};
    }));
    CHECK(revision(*value) == before);
    REQUIRE(raw.defer_set_name(take(raw.find(id<AuthoredEntityId>(90))), {"Queued"}));
    const auto pending = take(value->execute(command, before));
    CHECK_FALSE(pending.outcome);
    CHECK(revision(*value) == before);
    REQUIRE(raw.apply_deferred());
    bool wrong_thread_rejected = false;
    const auto current = revision(*value);
    std::thread worker([&] { wrong_thread_rejected = !value->execute(command, current); });
    worker.join();
    CHECK(wrong_thread_rejected);
    REQUIRE(raw.close());
    CHECK_FALSE(value->execute(command, current));
}
