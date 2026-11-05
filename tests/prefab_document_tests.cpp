#include <shark/tools/prefab_document.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace shark::tools;

constexpr std::string_view project_id = "21000000-0000-4000-8000-000000000001";
constexpr std::string_view asset_id = "21000000-0000-4000-8000-000000000002";
constexpr std::string_view root_id = "21000000-0000-4000-8000-000000000010";
constexpr std::string_view child_id = "21000000-0000-4000-8000-000000000011";

template <typename T> T id(std::string_view text) {
    const auto parsed = shark::StableId::parse(text);
    REQUIRE(parsed);
    return T{parsed.value()};
}

std::string fixture() {
    std::ifstream input{std::filesystem::path{SHARK_PREFAB_FIXTURE_DIRECTORY} /
                            "beacon-v1.sharkprefab",
                        std::ios::binary};
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::string replace_one(std::string text, std::string_view before, std::string_view after) {
    const auto position = text.find(before);
    REQUIRE(position != std::string::npos);
    text.replace(position, before.size(), after);
    return text;
}

std::string prefab_text(std::string_view fields = {}) {
    return "{\"format\":\"shark.prefab\",\"schema_version\":1,\"project_id\":\"" +
           std::string{project_id} + "\",\"asset_id\":\"" + std::string{asset_id} +
           "\",\"root_entity\":\"" + std::string{root_id} + "\",\"entities\":[{\"entity_id\":\"" +
           std::string{root_id} +
           "\",\"components\":{\"shark.name\":{\"schema_version\":1},"
           "\"shark.transform\":{\"schema_version\":1}}}]" +
           std::string{fields} + "}";
}

PrefabDocument document() {
    auto decoded = decode_prefab_document(prefab_text());
    REQUIRE(decoded);
    return std::move(decoded).value().document;
}

SourceEntity indexed_entity(std::size_t index) {
    const auto suffix = std::to_string(index + 100);
    return {id<shark::AuthoredEntityId>("21000000-0000-4000-8000-" +
                                        std::string(12 - suffix.size(), '0') + suffix),
            id<shark::AuthoredEntityId>(root_id)};
}

template <typename T>
void rejected(const shark::Result<T>& result, std::string_view operation,
              std::string_view pointer = {}) {
    REQUIRE_FALSE(result);
    CHECK_FALSE(result.error().message.empty());
    CHECK(result.error().context.operation == operation);
    if (!pointer.empty())
        CHECK(result.error().context.path == pointer);
}

void rejected_document(const PrefabDocument& value) {
    rejected(validate_prefab_document(value), "source.prefab.validate");
    rejected(encode_prefab_document(value), "source.prefab.encode");
}

} // namespace

TEST_CASE("prefab fixture preserves local identity references and component values",
          "[prefab-documents]") {
    const auto text = fixture();
    const auto original = text;
    const auto decoded = decode_prefab_document(text);
    REQUIRE(decoded);
    CHECK(decoded.value().source_schema_version == prefab_document_version);
    const auto& value = decoded.value().document;
    CHECK(value.project == id<ProjectId>(project_id));
    CHECK(value.asset == id<shark::AssetId>(asset_id));
    CHECK(value.root == id<shark::AuthoredEntityId>(root_id));
    REQUIRE(value.entities.size() == 2);
    const auto& child = value.entities[0];
    CHECK(child.id == id<shark::AuthoredEntityId>(child_id));
    CHECK(child.parent == value.root);
    CHECK(child.name.value == "Beacon lens");
    CHECK(child.transform.translation == shark::math::Vec3{0.0, 2.0, 0.0});
    REQUIRE(child.mesh_renderer);
    CHECK(child.mesh_renderer->asset == id<shark::AssetId>("21000000-0000-4000-8000-000000000020"));
    CHECK(child.mesh_renderer->mesh_index == 2);
    CHECK(child.mesh_renderer->visible);
    REQUIRE(child.camera);
    CHECK(*child.camera == shark::Camera{});
    CHECK(child.extras_json == R"({"label":"lens"})");
    const auto& root = value.entities[1];
    CHECK_FALSE(root.parent);
    CHECK(root.transform == shark::Transform{});
    REQUIRE(root.rigid_body);
    CHECK(root.rigid_body->motion == shark::BodyMotion::dynamic_body);
    CHECK(root.rigid_body->mass == 12.0);
    REQUIRE(root.script);
    CHECK(root.script->source == "scripts/beacon.lua");
    CHECK_FALSE(root.script->enabled);
    CHECK(text == original);

    const auto saved = value;
    const auto encoded = encode_prefab_document(value);
    REQUIRE(encoded);
    CHECK(encoded.value().ends_with('\n'));
    CHECK(encoded.value().find("\n  \"asset_id\"") != std::string::npos);
    const auto reread = decode_prefab_document(encoded.value());
    REQUIRE(reread);
    CHECK(reread.value().document == value);
    CHECK(value == saved);
    const auto encoded_again = encode_prefab_document(reread.value().document);
    REQUIRE(encoded_again);
    CHECK(encoded_again.value() == encoded.value());
}

TEST_CASE("prefab envelope rejects unsupported schemas nesting and foreign state",
          "[prefab-documents]") {
    for (const auto version : {"0", "2", "1.0", "true", "null"}) {
        rejected(decode_prefab_document(replace_one(prefab_text(), "\"schema_version\":1",
                                                    "\"schema_version\":" + std::string{version})),
                 "source.prefab.decode", "/schema_version");
    }
    for (const auto field :
         {R"(,"prefab":"other.sharkprefab")", R"(,"nested_prefabs":[])", R"(,"overrides":{})",
          R"(,"world_origin":null)", R"(,"scene_id":"unrelated")", R"(,"ownership_epoch":1)"}) {
        rejected(decode_prefab_document(prefab_text(field)), "source.prefab.decode");
    }
    rejected(decode_prefab_document(replace_one(prefab_text(), "shark.prefab", "shark.scene")),
             "source.prefab.decode", "/format");
    rejected(decode_prefab_document(
                 replace_one(prefab_text(), asset_id, "00000000-0000-0000-0000-000000000000")),
             "source.prefab.decode", "/asset_id");
    const auto nested_component =
        replace_one(prefab_text(), "\"components\":{", "\"components\":{\"shark.prefab\":{},");
    rejected(decode_prefab_document(nested_component), "source.prefab.decode");
    const auto missing_root =
        replace_one(prefab_text(), "\"root_entity\":\"" + std::string{root_id} + "\",", "");
    rejected(decode_prefab_document(missing_root), "source.prefab.decode", "/root_entity");
}

TEST_CASE("decoded prefab graph and body failures identify their entity and field",
          "[prefab-documents]") {
    auto text = fixture();
    SECTION("parent outside the prefab") {
        text = replace_one(text, "\"parent\": \"" + std::string{root_id} + "\"",
                           R"("parent": "21000000-0000-4000-8000-000000000099")");
    }
    SECTION("child rigid body") {
        text = replace_one(
            text, R"("shark.camera": {"schema_version": 1})",
            R"("shark.camera": {"schema_version": 1}, "shark.rigid_body": {"schema_version": 1})");
    }
    const auto original = text;
    const auto decoded = decode_prefab_document(text);
    rejected(decoded, "source.prefab.decode", "/entities/0/parent");
    CHECK(decoded.error().context.entity_id == child_id);
    CHECK(text == original);
}

TEST_CASE("prefab graph validation requires a complete single rooted hierarchy",
          "[prefab-documents]") {
    auto value = document();
    SECTION("empty") {
        value.entities.clear();
    }
    SECTION("missing declared root") {
        value.root = id<shark::AuthoredEntityId>(child_id);
    }
    SECTION("multiple roots") {
        value.entities.push_back(indexed_entity(0));
        value.entities.back().parent.reset();
    }
    SECTION("duplicate identity") {
        value.entities.push_back(value.entities.front());
    }
    SECTION("external parent") {
        value.entities.push_back(indexed_entity(0));
        value.entities.back().parent = id<shark::AuthoredEntityId>(child_id);
    }
    SECTION("self parent") {
        value.entities.front().parent = value.root;
    }
    SECTION("disconnected cycle") {
        value.entities.push_back(indexed_entity(0));
        value.entities.push_back(indexed_entity(1));
        value.entities[1].parent = value.entities[2].id;
        value.entities[2].parent = value.entities[1].id;
    }
    SECTION("declared root is a child") {
        value.entities.push_back(indexed_entity(0));
        value.root = value.entities.back().id;
    }
    rejected_document(value);
}

TEST_CASE("prefab hierarchy and count limits accept their boundary and reject excess",
          "[prefab-documents]") {
    auto value = document();
    for (std::size_t index = 0; index < source_maximum_hierarchy_depth - 1; ++index) {
        auto child = indexed_entity(index);
        child.parent = value.entities.back().id;
        value.entities.push_back(std::move(child));
    }
    REQUIRE(validate_prefab_document(value));
    auto child = indexed_entity(source_maximum_hierarchy_depth);
    child.parent = value.entities.back().id;
    value.entities.push_back(std::move(child));
    rejected_document(value);

    value = document();
    for (std::size_t index = 0; index < source_maximum_entities - 1; ++index)
        value.entities.push_back(indexed_entity(index));
    REQUIRE(validate_prefab_document(value));
    value.entities.push_back(indexed_entity(source_maximum_entities));
    rejected_document(value);
}

TEST_CASE("prefabs share component validation and preserve root body constraints",
          "[prefab-documents]") {
    auto value = document();
    SECTION("invalid name") {
        value.entities.front().name.value = "bad\nname";
    }
    SECTION("nonfinite transform") {
        value.entities.front().transform.translation.x = std::numeric_limits<double>::infinity();
    }
    SECTION("invalid camera") {
        value.entities.front().camera = shark::Camera{};
        value.entities.front().camera->near_clip = -1.0;
    }
    SECTION("script path escape") {
        value.entities.front().script = shark::Script{"../beacon.lua", true};
    }
    SECTION("invalid body settings") {
        value.entities.front().rigid_body = shark::RigidBody{};
        value.entities.front().rigid_body->mass = 0.0;
    }
    SECTION("body child") {
        value.entities.push_back(indexed_entity(0));
        value.entities.back().rigid_body = shark::RigidBody{};
    }
    SECTION("body nonuniform scale") {
        value.entities.front().rigid_body = shark::RigidBody{};
        value.entities.front().transform.scale = {1.0, 2.0, 1.0};
    }
    rejected_document(value);
}

TEST_CASE("prefab lexical parsing and extras retain source resource bounds", "[prefab-documents]") {
    for (const auto& text : {replace_one(prefab_text(), "\"schema_version\":1",
                                         "\"schema_version\":1,\"schema_versi\\u006fn\":1"),
                             prefab_text(R"(,"extras":{"x":1e999})"),
                             prefab_text(",\"extras\":{\"x\":\"" +
                                         std::string(source_maximum_string_bytes + 1, 'x') + "\"}"),
                             std::string(source_maximum_bytes + 1, ' '), prefab_text() + "{}"}) {
        rejected(decode_prefab_document(text), "source.prefab.decode");
    }
    std::string nested{"0"};
    for (std::size_t depth = 0; depth < source_maximum_json_depth; ++depth)
        nested = "[" + nested + "]";
    rejected(decode_prefab_document(prefab_text(",\"extras\":{\"x\":" + nested + "}")),
             "source.prefab.decode");

    auto value = document();
    value.extras_json = " { \"z\": 2, \"a\": true } ";
    const auto validated = validate_prefab_document(value);
    REQUIRE(validated);
    CHECK(validated.value().extras_json == R"({"a":true,"z":2})");
    CHECK(value.extras_json == " { \"z\": 2, \"a\": true } ");
    value.extras_json = "{\"x\":\"" + std::string(source_maximum_extras_bytes, 'x') + "\"}";
    rejected_document(value);
    value = document();
    for (std::size_t index = 0; index < 17; ++index) {
        auto entity = indexed_entity(index);
        entity.extras_json = "{\"x\":\"" + std::string(4000, 'x') + "\"}";
        value.entities.push_back(std::move(entity));
    }
    rejected_document(value);
}
