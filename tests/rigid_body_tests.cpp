#include <shark/physics.hpp>
#include <shark/prepared_scene.hpp>
#include <shark/tools/prepared_content.hpp>
#include <shark/tools/project_files.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <filesystem>
#include <limits>

namespace {
using namespace shark;
using namespace shark::tools;
template <typename T> T take(Result<T> result) {
    if (!result)
        INFO(result.error().message);
    REQUIRE(result);
    return std::move(result).value();
}
template <typename Id> Id id(char digit) {
    std::string text = "40000000-0000-4000-8000-000000000001";
    text.back() = digit;
    return Id{take(StableId::parse(text))};
}
struct Fixture {
    ProjectDocument project{id<ProjectId>('1'),
                            id<WorldId>('2'),
                            Name{"Physics"},
                            id<SceneId>('3'),
                            {{id<SceneId>('3'), take(ProjectPath::parse("world.sharkscene"))}}};
    SceneDocument source{id<ProjectId>('1'),
                         id<SceneId>('3'),
                         std::nullopt,
                         {{id<AuthoredEntityId>('4'), std::nullopt, Name{"Body"}},
                          {id<AuthoredEntityId>('5'), id<AuthoredEntityId>('4'), Name{"Visual"}}}};
    std::unique_ptr<AuthoredScene> owner{take(AuthoredScene::create(project, source))};
    SceneRevision revision() {
        return take(owner->status()).revision;
    }
    void set(std::optional<RigidBody> body) {
        take(take(owner->execute(SetRigidBodyCommand{id<AuthoredEntityId>('4'), body}, revision()))
                 .outcome);
    }
};
struct Scratch {
    std::filesystem::path path;
    Scratch()
        : path(std::filesystem::path{SHARK_RIGID_BODY_SCRATCH} /
               take(generate_stable_id()).to_string()) {
        REQUIRE(std::filesystem::create_directories(path));
    }
    ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
} // namespace

TEST_CASE("Rigid-body authoring persists every field and history restores constrained transforms",
          "[rigid-body]") {
    Fixture f;
    const auto original = take(f.owner->capture());
    RigidBody body{
        BodyMotion::dynamic_body, ColliderShape::capsule, {0.25, 0.75, 1}, 0.2, 0.8, 3, 0.4, 0.6};
    body.collision_layer = 3;
    body.collision_mask = 18;
    body.is_trigger = true;
    f.set(body);
    auto expected = f.source;
    expected.entities[0].rigid_body = body;
    CHECK(take(f.owner->capture()).scene == expected);
    const auto encoded = take(encode_scene_document(expected));
    CHECK(take(decode_scene_document(encoded)).document == expected);
    auto history = take(f.owner->history());
    take(take(f.owner->undo(history.next_undo->ticket, f.revision())).outcome);
    CHECK(take(f.owner->capture()).scene == original.scene);
    history = take(f.owner->history());
    take(take(f.owner->redo(history.next_redo->ticket, f.revision())).outcome);
    CHECK(take(f.owner->capture()).scene == expected);
    const auto before = f.revision();
    auto bad = Transform{};
    bad.scale = {-1, -1, -1};
    AuthoringBatch invalid{id<AuthoringBatchId>('6'),
                           {SetNameCommand{id<AuthoredEntityId>('4'), Name{"Not published"}},
                            SetTransformCommand{id<AuthoredEntityId>('4'), bad}}};
    CHECK_FALSE(take(f.owner->execute(invalid, before)).outcome);
    CHECK(f.revision() == before);
    CHECK(take(f.owner->capture()).scene == expected);
    AuthoringBatch remove{id<AuthoringBatchId>('7'),
                          {SetRigidBodyCommand{id<AuthoredEntityId>('4'), std::nullopt},
                           SetTransformCommand{id<AuthoredEntityId>('4'), bad}}};
    take(take(f.owner->execute(remove, before)).outcome);
    history = take(f.owner->history());
    take(take(f.owner->undo(history.next_undo->ticket, f.revision())).outcome);
    CHECK(take(f.owner->capture()).scene == expected);
    history = take(f.owner->history());
    take(take(f.owner->redo(history.next_redo->ticket, f.revision())).outcome);
    CHECK_FALSE(take(f.owner->capture()).scene.entities[0].rigid_body);
    CHECK(take(f.owner->capture()).scene.entities[0].transform.scale == math::Vec3{-1});
}

TEST_CASE(
    "Rigid-body retries compare every field exactly including inactive fields and signed zero",
    "[rigid-body]") {
    Fixture f;
    RigidBody body;
    const auto status = take(f.owner->mutation_status());
    AuthoringMutationRequest request{
        *status.next, status.revision,
        AuthoringCommand{SetRigidBodyCommand{id<AuthoredEntityId>('4'), body}}};
    const auto first = take(f.owner->mutate(request));
    CHECK(take(take(first.outcome)->result).changed);
    const auto rev = f.revision();
    CHECK(take(f.owner->mutate(request)).state == AuthoringMutationState::replayed);
    CHECK(take(f.owner->mutation_outcome(request.id)).outcome.value() == first.outcome.value());
    for (int field = 0; field < 3; ++field) {
        auto changed = body;
        if (field == 0)
            changed.collision_layer = 1;
        if (field == 1)
            changed.collision_mask = 0;
        if (field == 2)
            changed.is_trigger = true;
        auto mismatch = request;
        mismatch.mutation =
            AuthoringCommand{SetRigidBodyCommand{id<AuthoredEntityId>('4'), changed}};
        CHECK(take(f.owner->mutate(mismatch)).state == AuthoringMutationState::payload_mismatch);
    }
    for (auto field : {&RigidBody::radius, &RigidBody::half_height, &RigidBody::mass,
                       &RigidBody::friction, &RigidBody::restitution}) {
        auto changed = body;
        changed.*field += 0.01;
        auto mismatch = request;
        mismatch.mutation =
            AuthoringCommand{SetRigidBodyCommand{id<AuthoredEntityId>('4'), changed}};
        CHECK(take(f.owner->mutate(mismatch)).state == AuthoringMutationState::payload_mismatch);
    }
    for (int axis = 0; axis < 3; ++axis) {
        auto changed = body;
        changed.half_extents[axis] += 0.1;
        auto mismatch = request;
        mismatch.mutation =
            AuthoringCommand{SetRigidBodyCommand{id<AuthoredEntityId>('4'), changed}};
        CHECK(take(f.owner->mutate(mismatch)).state == AuthoringMutationState::payload_mismatch);
    }
    body.restitution = -0.0;
    auto mismatch = request;
    mismatch.mutation = AuthoringCommand{SetRigidBodyCommand{id<AuthoredEntityId>('4'), body}};
    CHECK(take(f.owner->mutate(mismatch)).state == AuthoringMutationState::payload_mismatch);
    body.radius = std::numeric_limits<double>::quiet_NaN();
    auto next = take(f.owner->mutation_status());
    auto invalid = AuthoringMutationRequest{
        *next.next, next.revision,
        AuthoringCommand{SetRigidBodyCommand{id<AuthoredEntityId>('4'), body}}};
    auto failed = take(f.owner->mutate(invalid));
    CHECK_FALSE(take(failed.outcome)->result);
    CHECK(take(f.owner->mutate(invalid)).state == AuthoringMutationState::replayed);
    body.radius = std::bit_cast<double>(std::uint64_t{0x7ff8000000000042});
    invalid.mutation = AuthoringCommand{SetRigidBodyCommand{id<AuthoredEntityId>('4'), body}};
    CHECK(take(f.owner->mutate(invalid)).state == AuthoringMutationState::payload_mismatch);
    CHECK(f.revision() == rev);
}

TEST_CASE("Prepared v2 and native source save reopen retain rigid bodies without runtime importers",
          "[rigid-body]") {
    Fixture f;
    auto legacy = take(decode_prepared_scene(take(prepare_scene(*f.owner))));
    // Version 1 output without bodies is retained byte-for-byte on re-encode.
    auto bytes1 = take(encode_prepared_scene(legacy));
    CHECK(std::to_integer<unsigned>(bytes1[8]) == 1);
    CHECK(take(encode_prepared_scene(take(decode_prepared_scene(bytes1)))) == bytes1);
    RigidBody body{
        BodyMotion::kinematic_body, ColliderShape::sphere, {1, 2, 3}, 0.75, 0.5, 4, 0.25, 0.125};
    f.set(body);
    auto captured = take(f.owner->capture());
    auto prepared = take(decode_prepared_scene(take(prepare_scene(*f.owner))));
    auto bytes = take(encode_prepared_scene(prepared));
    CHECK(std::to_integer<unsigned>(bytes[8]) == 2);
    CHECK(take(decode_prepared_scene(bytes)) == prepared);
    auto filtered = prepared;
    filtered.entities.front().rigid_body->collision_layer = 15;
    filtered.entities.front().rigid_body->collision_mask = 0;
    filtered.entities.front().rigid_body->is_trigger = true;
    const auto bytes3 = take(encode_prepared_scene(filtered));
    CHECK(std::to_integer<unsigned>(bytes3[8]) == 3);
    CHECK(take(decode_prepared_scene(bytes3)) == filtered);
    auto filtered_runtime = take(LoadedPreparedScene::load(bytes3, take(PreparedAssets::create())));
    const auto filtered_handle = take(filtered_runtime->scene().find(id<AuthoredEntityId>('4')));
    CHECK(take(filtered_runtime->scene().entity_state(filtered_handle)).rigid_body ==
          filtered.entities.front().rigid_body);
    auto loaded = take(LoadedPreparedScene::load(bytes, take(PreparedAssets::create({}))));
    auto handle = take(loaded->scene().find(id<AuthoredEntityId>('4')));
    CHECK(take(loaded->scene().entity_state(handle)).rigid_body == body);
    bytes[8] = std::byte{5};
    CHECK_FALSE(decode_prepared_scene(bytes));
    Scratch scratch;
    auto files = take(ProjectFiles::open(scratch.path));
    take(files->write_project(take(ProjectPath::parse("game.sharkproject")), f.project,
                              std::nullopt));
    const auto saved = take(files->save_scene(*f.owner, f.revision(), std::nullopt));
    CHECK(saved.current_scene_clean);
    const auto history = take(f.owner->history());
    CHECK(history.undo_entries == 1);
    auto reopened = take(files->load_scene(f.project, f.project.startup_scene));
    CHECK(take(reopened.owner->capture()).scene == captured.scene);
    CHECK(take(reopened.owner->status()).revision.scene_instance !=
          captured.revision.scene_instance);
    CHECK(take(f.owner->history()).undo_entries == history.undo_entries);
}

TEST_CASE("Rigid-body source rejects unknown fields versions duplicate keys and invalid hierarchy",
          "[rigid-body]") {
    Fixture f;
    f.set(RigidBody{});
    const auto before = take(f.owner->capture());
    const auto valid = take(encode_scene_document(before.scene));
    auto legacy = valid;
    for (const auto* field :
         {"\"collision_layer\": 0,", "\"collision_mask\": 65535,", "\"is_trigger\": false,"}) {
        const auto position = legacy.find(field);
        REQUIRE(position != std::string::npos);
        legacy.erase(position, std::string{field}.size());
    }
    CHECK(take(decode_scene_document(legacy)).document == before.scene);
    for (const auto* injection :
         {"\"collision_layer\":16,", "\"collision_mask\":-1,", "\"is_trigger\":\"yes\","}) {
        auto malformed = legacy;
        const auto position = malformed.find("\"shark.rigid_body\": {");
        malformed.insert(position + std::string{"\"shark.rigid_body\": {"}.size(), injection);
        CHECK_FALSE(decode_scene_document(malformed));
    }
    const std::string marker = "\"shark.rigid_body\": {";
    const auto start = valid.find(marker);
    REQUIRE(start != std::string::npos);
    for (const auto& injection : {std::string{"\"unknown\": true,"}, std::string{"\"mass\": 7,"}}) {
        auto text = valid;
        text.insert(start + marker.size(), injection);
        CHECK_FALSE(decode_scene_document(text));
    }
    auto wrong_version = valid;
    const auto version = wrong_version.find("\"schema_version\": 1", start);
    REQUIRE(version != std::string::npos);
    wrong_version.replace(version, std::string{"\"schema_version\": 1"}.size(),
                          "\"schema_version\": 2");
    CHECK_FALSE(decode_scene_document(wrong_version));
    auto bad_motion = valid;
    const auto mode = bad_motion.find("\"motion\": \"static\"", start);
    REQUIRE(mode != std::string::npos);
    bad_motion.replace(mode, std::string{"\"motion\": \"static\""}.size(), "\"motion\": \"hover\"");
    CHECK_FALSE(decode_scene_document(bad_motion));
    auto invalid = before.scene;
    invalid.entities[1].rigid_body.emplace();
    CHECK_FALSE(encode_scene_document(invalid));
    CHECK_FALSE(f.owner->activate(invalid, before.revision, UnsavedScenePolicy::discard));
    CHECK(take(f.owner->capture()).scene == before.scene);
    CHECK(f.revision() == before.revision);
}
