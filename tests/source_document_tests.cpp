#include <shark/tools/source_document.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace shark::tools;

constexpr std::string_view project_id = "00000000-0000-4000-8000-000000000001";
constexpr std::string_view world_id = "00000000-0000-4000-8000-000000000002";
constexpr std::string_view scene_id = "00000000-0000-4000-8000-000000000003";
constexpr std::string_view root_id = "00000000-0000-4000-8000-000000000010";
constexpr std::string_view child_id = "00000000-0000-4000-8000-000000000011";

template <typename T> T id(const std::string_view text) {
    auto parsed = shark::StableId::parse(text);
    REQUIRE(parsed);
    return T{parsed.value()};
}

std::string fixture(const char* name) {
    const auto path = std::filesystem::path{SHARK_SOURCE_FIXTURE_DIRECTORY} / name;
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::string replace_one(std::string text, const std::string_view old_text,
                        const std::string_view new_text) {
    const auto position = text.find(old_text);
    REQUIRE(position != std::string::npos);
    text.replace(position, old_text.size(), new_text);
    return text;
}

std::string entity_text(const std::string_view fields = {}) {
    return "{\"entity_id\":\"" + std::string{root_id} +
           "\",\"components\":{\"shark.name\":{\"schema_version\":1},"
           "\"shark.transform\":{\"schema_version\":1}}" +
           std::string{fields} + "}";
}

std::string scene_text(const std::string& entities = entity_text(),
                       const std::string_view fields = {}) {
    return "{\"format\":\"shark.scene\",\"schema_version\":1,\"project_id\":\"" +
           std::string{project_id} + "\",\"scene_id\":\"" + std::string{scene_id} +
           "\",\"entities\":[" + entities + "]" + std::string{fields} + "}";
}

SceneDocument scene() {
    auto decoded = decode_scene_document(scene_text());
    REQUIRE(decoded);
    return std::move(decoded).value().document;
}

ProjectDocument project() {
    auto decoded = decode_project_document(fixture("project-v1.sharkproject"));
    REQUIRE(decoded);
    return std::move(decoded).value().document;
}

SourceEntity indexed_entity(const std::size_t index) {
    const std::string suffix = std::to_string(index + 100);
    const std::string text =
        "00000000-0000-4000-8000-" + std::string(12 - suffix.size(), '0') + suffix;
    return {id<shark::AuthoredEntityId>(text), std::nullopt, {}, {}, "{}"};
}

template <typename T>
void rejected(const shark::Result<T>& result, const std::string_view operation,
              const std::string_view pointer = {}) {
    REQUIRE_FALSE(result);
    CHECK_FALSE(result.error().message.empty());
    CHECK(result.error().context.operation == operation);
    if (!pointer.empty()) {
        CHECK(result.error().context.path == pointer);
    }
}

void rejected_scene(const std::string& text, const std::string_view pointer = {}) {
    const auto original = text;
    rejected(decode_scene_document(text), "source.scene.decode", pointer);
    CHECK(text == original);
}

std::string extras_with_string(const std::size_t bytes) {
    return "{\"x\":\"" + std::string(bytes, 'a') + "\"}";
}

} // namespace

TEST_CASE("source document identities remain distinct authored value types", "[source-documents]") {
    STATIC_REQUIRE_FALSE(std::is_same_v<ProjectId, shark::SceneId>);
    STATIC_REQUIRE_FALSE(std::is_same_v<ProjectId, shark::WorldId>);
    STATIC_REQUIRE_FALSE(std::is_same_v<shark::AuthoredEntityId, shark::WorldEntityId>);
    STATIC_REQUIRE(source_document_version == 1);
    STATIC_REQUIRE(source_maximum_bytes == 4 * 1024 * 1024);
    STATIC_REQUIRE(source_maximum_entities == 4096);
    STATIC_REQUIRE(source_maximum_scenes == 256);
    STATIC_REQUIRE(source_maximum_json_depth == 32);
    STATIC_REQUIRE(source_maximum_json_nodes == 200000);
    STATIC_REQUIRE(source_maximum_string_bytes == 4096);
    STATIC_REQUIRE(source_maximum_extras_bytes == 4096);
    STATIC_REQUIRE(source_maximum_total_extras_bytes == 65536);
    STATIC_REQUIRE(source_maximum_hierarchy_depth == 256);
}

TEST_CASE("legacy project fixture migrates to independently authored version one values",
          "[source-documents]") {
    const auto old_text = fixture("project-v0.sharkproject");
    const auto saved_text = old_text;
    auto legacy = decode_project_document(old_text);
    auto current = decode_project_document(fixture("project-v1.sharkproject"));
    REQUIRE(legacy);
    REQUIRE(current);
    CHECK(legacy.value().source_schema_version == 0);
    CHECK(current.value().source_schema_version == 1);
    CHECK(old_text == saved_text);
    const auto& value = legacy.value().document;
    CHECK(value == current.value().document);
    CHECK(value.project == id<ProjectId>(project_id));
    CHECK(value.world == id<shark::WorldId>(world_id));
    CHECK(value.startup_scene == id<shark::SceneId>(scene_id));
    CHECK(value.name.value == "Shark 海岸");
    REQUIRE(value.scenes.size() == 2);
    CHECK(value.scenes[0].scene == value.startup_scene);
    CHECK(value.scenes[0].path.text() == "scenes/海岸.sharkscene");
    CHECK(value.scenes[1].path.text() == "scenes/interior.sharkscene");
    CHECK(value.extras_json ==
          "{\"editor\":{\"grid\":true},\"nullable\":null,\"tags\":[\"coast\",\"海\"]}");
    auto encoded = encode_project_document(value);
    REQUIRE(encoded);
    auto reread = decode_project_document(encoded.value());
    REQUIRE(reread);
    CHECK(reread.value().source_schema_version == 1);
    CHECK(reread.value().document == value);
    CHECK(fixture("project-v0.sharkproject") == saved_text);
}

TEST_CASE("legacy flat scene migration preserves order hierarchy identity and world coordinates",
          "[source-documents]") {
    const auto old_text = fixture("scene-v0.sharkscene");
    const auto saved_text = old_text;
    auto legacy = decode_scene_document(old_text);
    auto current = decode_scene_document(fixture("scene-v1.sharkscene"));
    REQUIRE(legacy);
    REQUIRE(current);
    CHECK(legacy.value().source_schema_version == 0);
    CHECK(current.value().source_schema_version == 1);
    CHECK(old_text == saved_text);
    const auto& value = legacy.value().document;
    CHECK(value == current.value().document);
    CHECK(value.project == id<ProjectId>(project_id));
    CHECK(value.scene == id<shark::SceneId>(scene_id));
    REQUIRE(value.world_origin);
    CHECK(value.world_origin->world == id<shark::WorldId>(world_id));
    CHECK(value.world_origin->cell == shark::WorldCell{-2147483648LL, 2147483647LL, 1234567890LL});
    CHECK(value.world_origin->offset == std::array<double, 3>{0.125, 1023.875, 0.0});
    REQUIRE(value.entities.size() == 2);
    const auto& child = value.entities[0];
    CHECK(child.id == id<shark::AuthoredEntityId>(child_id));
    CHECK(child.parent == id<shark::AuthoredEntityId>(root_id));
    CHECK(child.name.value == "灯台 🦈");
    CHECK(child.transform.translation == shark::math::Vec3{1.25, -2.5, 0.0});
    CHECK(child.transform.rotation.x == 0.0);
    CHECK(child.transform.rotation.y == 0.0);
    CHECK(child.transform.rotation.z == 1.0);
    CHECK(child.transform.rotation.w == 0.0);
    CHECK(child.transform.scale == shark::math::Vec3{-2.0, 3.0, 4.0});
    CHECK(child.extras_json == "{\"editor\":{\"locked\":false},\"note\":\"preserve me\"}");
    const auto& root = value.entities[1];
    CHECK(root.id == id<shark::AuthoredEntityId>(root_id));
    CHECK_FALSE(root.parent);
    CHECK(root.name == shark::Name{});
    CHECK(root.transform == shark::Transform{});
    CHECK(root.extras_json == "{\"unknown_extension\":[7,{\"text\":\"海\"}]}");
    CHECK(value.extras_json == "{\"layer\":\"coast\",\"revision_note\":3}");
    auto encoded = encode_scene_document(value);
    REQUIRE(encoded);
    auto reread = decode_scene_document(encoded.value());
    REQUIRE(reread);
    CHECK(reread.value().source_schema_version == 1);
    CHECK(reread.value().document == value);
    CHECK(fixture("scene-v0.sharkscene") == saved_text);
}

TEST_CASE("source encoding is canonical readable and stable without input mutation",
          "[source-documents]") {
    auto original = scene();
    original.extras_json = " { \"z\" : [null, true], \"a\" : { \"b\" : 7 } } ";
    const auto saved = original;
    const auto validated = validate_scene_document(original);
    REQUIRE(validated);
    CHECK(validated.value().extras_json == "{\"a\":{\"b\":7},\"z\":[null,true]}");
    auto first = encode_scene_document(original);
    auto second = encode_scene_document(original);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first.value() == second.value());
    CHECK(original == saved);
    CHECK(first.value().ends_with("\n"));
    CHECK(first.value().find("\n  \"") != std::string::npos);
    CHECK(first.value().find('\t') == std::string::npos);
    auto decoded = decode_scene_document(first.value());
    REQUIRE(decoded);
    auto third = encode_scene_document(decoded.value().document);
    REQUIRE(third);
    CHECK(third.value() == first.value());
    CHECK(decoded.value().document == validated.value());

    const auto project_value = project();
    const auto project_saved = project_value;
    auto project_first = encode_project_document(project_value);
    auto project_second = encode_project_document(project_value);
    REQUIRE(project_first);
    REQUIRE(project_second);
    CHECK(project_first.value() == project_second.value());
    CHECK(project_first.value().ends_with("\n"));
    CHECK(project_value == project_saved);
}

TEST_CASE("optional source fields use schema defaults and empty scenes remain valid values",
          "[source-documents]") {
    auto minimal = decode_scene_document(scene_text());
    REQUIRE(minimal);
    CHECK_FALSE(minimal.value().document.world_origin);
    REQUIRE(minimal.value().document.entities.size() == 1);
    CHECK_FALSE(minimal.value().document.entities[0].parent);
    CHECK(minimal.value().document.entities[0].name == shark::Name{});
    CHECK(minimal.value().document.entities[0].transform == shark::Transform{});
    CHECK(minimal.value().document.entities[0].extras_json == "{}");
    CHECK(minimal.value().document.extras_json == "{}");
    auto empty = decode_scene_document(scene_text("", ",\"world_origin\":null"));
    REQUIRE(empty);
    CHECK(empty.value().document.entities.empty());
    CHECK_FALSE(empty.value().document.world_origin);
}

TEST_CASE("source versions and format discriminators reject unknown or incorrectly typed values",
          "[source-documents]") {
    for (const auto version : {"2", "-1", "1.0", "true", "null", "\"1\"", "4294967296"}) {
        DYNAMIC_SECTION("version " << version) {
            rejected_scene(replace_one(scene_text(), "\"schema_version\":1",
                                       "\"schema_version\":" + std::string{version}),
                           "/schema_version");
            const auto input =
                replace_one(fixture("project-v1.sharkproject"), "\"schema_version\": 1",
                            "\"schema_version\": " + std::string{version});
            rejected(decode_project_document(input), "source.project.decode", "/schema_version");
        }
    }
    rejected_scene(replace_one(scene_text(), "shark.scene", "shark.project"), "/format");
    rejected_scene(replace_one(scene_text(), "\"format\":\"shark.scene\",", ""), "/format");
    rejected(decode_project_document(fixture("scene-v1.sharkscene")), "source.project.decode");
    rejected(decode_scene_document(fixture("project-v1.sharkproject")), "source.scene.decode");
}

TEST_CASE("components require known identities versions and field types", "[source-documents]") {
    const auto original = scene_text();
    const std::vector<std::pair<std::string, std::string>> changes{
        {"\"shark.name\":{\"schema_version\":1},", ""},
        {",\"shark.transform\":{\"schema_version\":1}", ""},
        {"shark.name", "shark.unknown"},
        {"\"shark.name\":{\"schema_version\":1}", "\"shark.name\":{\"schema_version\":2}"},
        {"\"shark.transform\":{\"schema_version\":1}",
         "\"shark.transform\":{\"schema_version\":0}"},
        {"\"shark.name\":{\"schema_version\":1}", "\"shark.name\":{\"value\":\"x\"}"},
        {"\"shark.name\":{\"schema_version\":1}",
         "\"shark.name\":{\"schema_version\":1,\"value\":12}"},
        {"\"shark.transform\":{\"schema_version\":1}",
         "\"shark.transform\":{\"schema_version\":1,\"translation\":null}"},
        {"\"shark.transform\":{\"schema_version\":1}",
         "\"shark.transform\":{\"schema_version\":1,\"rotation\":[0,0,1]}"},
        {"\"shark.transform\":{\"schema_version\":1}",
         "\"shark.transform\":{\"schema_version\":1,\"scale\":[1,1,1,1]}"}};
    for (const auto& [from, to] : changes) {
        DYNAMIC_SECTION(from << " -> " << to) {
            rejected_scene(replace_one(original, from, to));
        }
    }
}

TEST_CASE("unknown fields cannot smuggle authority or future component data",
          "[source-documents]") {
    const std::vector<std::string> inputs{
        scene_text(entity_text(), ",\"authority\":{}"),
        scene_text(entity_text(",\"handle\":7")),
        scene_text(entity_text(",\"name\":\"legacy\"")),
        replace_one(scene_text(), "\"shark.name\":{\"schema_version\":1}",
                    "\"shark.name\":{\"schema_version\":1,\"extras\":{}}"),
        replace_one(scene_text(), "\"shark.transform\":{\"schema_version\":1}",
                    "\"shark.transform\":{\"schema_version\":1,\"position\":[0,0,0]}"),
        replace_one(scene_text(), "\"components\":{", "\"components\":{\"extra\":{},"),
        replace_one(fixture("scene-v0.sharkscene"), "\"name\": \"灯台 🦈\",",
                    "\"components\": {},"),
        replace_one(fixture("scene-v0.sharkscene"), "\"position\":", "\"translation\":")};
    for (const auto& input : inputs) {
        DYNAMIC_SECTION(input) {
            rejected_scene(input);
        }
    }
    const auto input = replace_one(fixture("project-v1.sharkproject"),
                                   "\"name\":", "\"region_authority\": 7, \"name\":");
    rejected(decode_project_document(input), "source.project.decode", "/region_authority");
}

TEST_CASE("duplicate decoded JSON keys reject at every object layer including extras",
          "[source-documents]") {
    const std::vector<std::string> inputs{
        replace_one(scene_text(), "\"format\":", "\"format\":\"shark.scene\",\"format\":"),
        replace_one(scene_text(), "\"entity_id\":",
                    "\"entity_id\":\"" + std::string{root_id} + "\",\"entity\\u005fid\":"),
        replace_one(scene_text(), "\"schema_version\":1}",
                    "\"schema_version\":1,\"schema_version\":1}"),
        scene_text(entity_text(), ",\"extras\":{\"a\":1,\"\\u0061\":2}"),
        scene_text(entity_text(), ",\"extras\":{\"nested\":{\"x\":0,\"x\":0}}")};
    for (const auto& input : inputs) {
        DYNAMIC_SECTION(input) {
            rejected_scene(input);
        }
    }
}

TEST_CASE("source parser rejects malformed truncated and trailing input without modifying buffers",
          "[source-documents]") {
    const std::vector<std::string> inputs{
        "",
        " ",
        "null",
        "[]",
        "{",
        "{}",
        scene_text().substr(0, scene_text().size() - 1),
        scene_text() + "false",
        scene_text() + " {}",
        scene_text() + std::string(1, '\0'),
        replace_one(scene_text(), "\"schema_version\":1", "\"schema_version\":01"),
        scene_text(entity_text(), ",\"extras\":{\"x\":NaN}"),
        scene_text(entity_text(), ",\"extras\":{\"x\":Infinity}"),
        scene_text(entity_text(), ",\"extras\":{\"x\":1e9999}"),
        scene_text(entity_text(), ",\"extras\":{\"x\":true,}"),
        scene_text(entity_text(), ",\"extras\":{/*comment*/}")};
    for (const auto& input : inputs) {
        DYNAMIC_SECTION(input) {
            rejected_scene(input);
        }
    }
}

TEST_CASE("UTF8 and Unicode escapes are validated in strings and object keys",
          "[source-documents]") {
    const std::vector<std::string> bad_strings{std::string{"\xC0\xAF", 2},
                                               std::string{"\xED\xA0\x80", 3},
                                               std::string{"\xF4\x90\x80\x80", 4},
                                               std::string{"\xE2\x82", 2},
                                               "\\uD800",
                                               "\\uDC00",
                                               "\\uD800x",
                                               "\\uD800\\u0041",
                                               std::string(1, '\n'),
                                               std::string(1, '\0')};
    for (const auto& invalid : bad_strings) {
        rejected_scene(scene_text(entity_text(), ",\"extras\":{\"x\":\"" + invalid + "\"}"));
        rejected_scene(scene_text(entity_text(), ",\"extras\":{\"" + invalid + "\":1}"));
    }
    auto escaped = decode_scene_document(scene_text(
        entity_text(), ",\"extras\":{\"emoji\":\"\\uD83E\\uDD88\",\"nul\":\"\\u0000\"}"));
    REQUIRE(escaped);
    CHECK(escaped.value().document.extras_json.find("🦈") != std::string::npos);
    CHECK(escaped.value().document.extras_json.find("\\u0000") != std::string::npos);
}

TEST_CASE("source UUID references are canonical required and namespace checked",
          "[source-documents]") {
    for (const auto invalid :
         {"", "00000000-0000-0000-0000-000000000000", "00000000-0000-1000-8000-000000000010",
          "00000000-0000-4000-7000-000000000010", "AAAAAAAA-0000-4000-8000-000000000010",
          "00000000000040008000000000000010"}) {
        DYNAMIC_SECTION(invalid) {
            rejected_scene(replace_one(scene_text(), root_id, invalid));
            rejected_scene(replace_one(scene_text(), project_id, invalid));
            rejected_scene(replace_one(scene_text(), scene_id, invalid));
        }
    }
    rejected_scene(scene_text(entity_text() + "," + entity_text()));
    rejected_scene(scene_text(entity_text(",\"parent\":\"" + std::string{child_id} + "\"")));
    rejected_scene(scene_text(entity_text(",\"parent\":\"" + std::string{root_id} + "\"")));
    rejected_scene(scene_text(entity_text(",\"parent\":42")));
}

TEST_CASE("project references require unique IDs paths and a declared startup scene",
          "[source-documents]") {
    auto value = project();
    SECTION("duplicate scene ID") {
        value.scenes[1].scene = value.scenes[0].scene;
    }
    SECTION("duplicate path") {
        value.scenes[1].path = value.scenes[0].path;
    }
    SECTION("missing startup") {
        value.startup_scene = id<shark::SceneId>(root_id);
    }
    SECTION("empty declaration") {
        value.scenes.clear();
    }
    const auto saved = value;
    rejected(validate_project_document(value), "source.project.validate");
    rejected(encode_project_document(value), "source.project.encode");
    CHECK(value == saved);
}

TEST_CASE("project source paths apply portable lexical containment before filesystem work",
          "[source-documents]") {
    for (const auto invalid :
         {"../escape.sharkscene", "/absolute.sharkscene", "C:/drive.sharkscene",
          "//server/share/scene", "scenes/../other", "scenes//other", "scenes/CON",
          "https://host/scene", "scenes/file:stream"}) {
        const auto text =
            replace_one(fixture("project-v1.sharkproject"), "scenes/海岸.sharkscene", invalid);
        rejected(decode_project_document(text), "source.project.decode", "/scenes/0/path");
    }
}

TEST_CASE("project validation rejects a Core-invalid path left by moving from its public entry",
          "[source-documents]") {
    auto value = project();
    const auto expected_path = value.scenes[0].path.text();
    const auto transferred_path = std::move(value.scenes[0].path);
    CHECK(transferred_path.text() == expected_path);
    // The qualified standard libraries leave this moved-from string empty.
    // A valid original construction cannot stand in for current-value validation.
    REQUIRE(value.scenes[0].path.text().empty());
    const auto core = shark::ProjectPath::parse(value.scenes[0].path.text());
    REQUIRE_FALSE(core);
    REQUIRE(core.error().code == shark::ErrorCode::invalid_path);
    const auto saved = value;
    const auto validated = validate_project_document(value);
    rejected(validated, "source.project.validate", "/scenes/0/path");
    CHECK(validated.error().code == shark::ErrorCode::invalid_path);
    const auto encoded = encode_project_document(value);
    rejected(encoded, "source.project.encode", "/scenes/0/path");
    CHECK(encoded.error().code == shark::ErrorCode::invalid_path);
    CHECK(value == saved);
}

TEST_CASE("typed scene validation and encoding revalidate hierarchy atomically",
          "[source-documents]") {
    auto value = scene();
    value.entities.push_back(indexed_entity(0));
    SECTION("duplicate ID") {
        value.entities[1].id = value.entities[0].id;
    }
    SECTION("unresolved parent") {
        value.entities[0].parent = id<shark::AuthoredEntityId>(child_id);
    }
    SECTION("self parent") {
        value.entities[0].parent = value.entities[0].id;
    }
    SECTION("cycle") {
        value.entities[0].parent = value.entities[1].id;
        value.entities[1].parent = value.entities[0].id;
    }
    const auto saved = value;
    rejected(validate_scene_document(value), "source.scene.validate");
    rejected(encode_scene_document(value), "source.scene.encode");
    CHECK(value == saved);
}

TEST_CASE("hierarchy depth includes the root and accepts the exact bounded chain",
          "[source-documents]") {
    auto value = scene();
    value.entities.clear();
    for (std::size_t index = 0; index < source_maximum_hierarchy_depth; ++index) {
        auto entity = indexed_entity(index);
        if (!value.entities.empty()) {
            entity.parent = value.entities.back().id;
        }
        value.entities.push_back(std::move(entity));
    }
    auto valid = validate_scene_document(value);
    REQUIRE(valid);
    auto encoded = encode_scene_document(value);
    REQUIRE(encoded);
    auto decoded = decode_scene_document(encoded.value());
    REQUIRE(decoded);
    CHECK(decoded.value().document == value);
    auto overflow = indexed_entity(source_maximum_hierarchy_depth);
    overflow.parent = value.entities.back().id;
    value.entities.push_back(std::move(overflow));
    rejected(validate_scene_document(value), "source.scene.validate");
    rejected(encode_scene_document(value), "source.scene.encode");
}

TEST_CASE("source values reuse Core Name and Transform validation and canonicalization",
          "[source-documents]") {
    auto value = scene();
    value.entities[0].name.value = std::string(shark::maximum_name_bytes, 'n');
    value.entities[0].transform.rotation = {2.0, 0.0, 0.0, 0.0};
    value.entities[0].transform.scale = {-shark::transform_minimum_scale, 1.0, 2.0};
    auto canonical = validate_scene_document(value);
    REQUIRE(canonical);
    CHECK(canonical.value().entities[0].transform.rotation ==
          shark::math::Quat{1.0, 0.0, 0.0, 0.0});
    CHECK(value.entities[0].transform.rotation.w == 2.0);

    SECTION("name byte overflow") {
        value.entities[0].name.value.push_back('x');
    }
    SECTION("empty name") {
        value.entities[0].name.value.clear();
    }
    SECTION("control in name") {
        value.entities[0].name.value = "tab\tname";
    }
    SECTION("zero quaternion") {
        value.entities[0].transform.rotation = {0.0, 0.0, 0.0, 0.0};
    }
    SECTION("singular scale") {
        value.entities[0].transform.scale.y = 0.0;
    }
    SECTION("subminimum scale") {
        value.entities[0].transform.scale.z = shark::transform_minimum_scale / 2.0;
    }
    SECTION("infinite translation") {
        value.entities[0].transform.translation.x = std::numeric_limits<double>::infinity();
    }
    SECTION("NaN rotation") {
        value.entities[0].transform.rotation.y = std::numeric_limits<double>::quiet_NaN();
    }
    rejected(validate_scene_document(value), "source.scene.validate");
    rejected(encode_scene_document(value), "source.scene.encode");
}

TEST_CASE("scene world origins require exact integral cells and canonical finite offsets",
          "[source-documents]") {
    const auto input = fixture("scene-v1.sharkscene");
    const std::vector<std::pair<std::string, std::string>> changes{
        {"[-2147483648, 2147483647, 1234567890]", "[-2147483649, 0, 0]"},
        {"[-2147483648, 2147483647, 1234567890]", "[0, 2147483648, 0]"},
        {"[-2147483648, 2147483647, 1234567890]", "[0.0, 0, 0]"},
        {"[-2147483648, 2147483647, 1234567890]", "[true, 0, 0]"},
        {"[0.125, 1023.875, 0]", "[-0.0, 0, 0]"},
        {"[0.125, 1023.875, 0]", "[1024, 0, 0]"},
        {"[0.125, 1023.875, 0]", "[-0.1, 0, 0]"},
        {"[0.125, 1023.875, 0]", "[0, 0]"},
        {"\"cell\":", "\"region_epoch\": 1, \"cell\":"}};
    for (const auto& [from, to] : changes) {
        DYNAMIC_SECTION(to) {
            rejected_scene(replace_one(input, from, to));
        }
    }
    auto integer_negative_zero =
        decode_scene_document(replace_one(input, "[0.125, 1023.875, 0]", "[-0, 0, 0]"));
    REQUIRE(integer_negative_zero);
    CHECK_FALSE(std::signbit(integer_negative_zero.value().document.world_origin->offset[0]));
    auto value = scene();
    value.world_origin = shark::WorldPosition{id<shark::WorldId>(world_id), {}, {1024.0, 0.0, 0.0}};
    rejected(validate_scene_document(value), "source.scene.validate");
    rejected(encode_scene_document(value), "source.scene.encode");
}

TEST_CASE("entity and project scene count bounds accept endpoints and reject overflow",
          "[source-documents]") {
    auto value = scene();
    value.entities.clear();
    for (std::size_t index = 0; index < source_maximum_entities; ++index) {
        value.entities.push_back(indexed_entity(index));
    }
    auto valid = validate_scene_document(value);
    REQUIRE(valid);
    auto encoded = encode_scene_document(value);
    REQUIRE(encoded);
    auto decoded = decode_scene_document(encoded.value());
    REQUIRE(decoded);
    CHECK(decoded.value().document == value);
    value.entities.push_back(indexed_entity(source_maximum_entities));
    rejected(validate_scene_document(value), "source.scene.validate");
    rejected(encode_scene_document(value), "source.scene.encode");

    auto project_value = project();
    project_value.scenes.clear();
    for (std::size_t index = 0; index < source_maximum_scenes; ++index) {
        auto path = shark::ProjectPath::parse("scenes/" + std::to_string(index) + ".sharkscene");
        REQUIRE(path);
        project_value.scenes.push_back(
            {shark::SceneId{indexed_entity(index).id.value()}, std::move(path).value()});
    }
    project_value.startup_scene = project_value.scenes[0].scene;
    REQUIRE(validate_project_document(project_value));
    auto project_encoded = encode_project_document(project_value);
    REQUIRE(project_encoded);
    auto project_decoded = decode_project_document(project_encoded.value());
    REQUIRE(project_decoded);
    CHECK(project_decoded.value().document == project_value);
    auto extra_path = shark::ProjectPath::parse("scenes/overflow.sharkscene");
    REQUIRE(extra_path);
    project_value.scenes.push_back(
        {shark::SceneId{indexed_entity(source_maximum_scenes).id.value()},
         std::move(extra_path).value()});
    rejected(validate_project_document(project_value), "source.project.validate");
    rejected(encode_project_document(project_value), "source.project.encode");
}

TEST_CASE("bounded extras retain opaque values and reject nonobjects duplicates and overflow",
          "[source-documents]") {
    auto value = scene();
    value.extras_json = extras_with_string(source_maximum_extras_bytes - 8);
    REQUIRE(value.extras_json.size() == source_maximum_extras_bytes);
    REQUIRE(validate_scene_document(value));
    auto encoded = encode_scene_document(value);
    REQUIRE(encoded);
    auto decoded = decode_scene_document(encoded.value());
    REQUIRE(decoded);
    CHECK(decoded.value().document.extras_json == value.extras_json);
    for (const auto& invalid :
         std::vector<std::string>{extras_with_string(source_maximum_extras_bytes - 7), "[]", "null",
                                  "true", "", "{\"x\":1,\"\\u0078\":2}", "{\"x\":1e9999}"}) {
        DYNAMIC_SECTION(invalid) {
            value.extras_json = invalid;
            rejected(validate_scene_document(value), "source.scene.validate");
            rejected(encode_scene_document(value), "source.scene.encode");
            rejected_scene(scene_text(entity_text(), ",\"extras\":" + invalid));
        }
    }
    auto project_value = project();
    project_value.extras_json = "[]";
    rejected(validate_project_document(project_value), "source.project.validate");
    rejected(encode_project_document(project_value), "source.project.encode");
}

TEST_CASE("aggregate extras storage includes scene and entity objects", "[source-documents]") {
    auto value = scene();
    value.entities.clear();
    value.extras_json = "{}";
    for (std::size_t index = 0; index < 16; ++index) {
        auto entity = indexed_entity(index);
        entity.extras_json = extras_with_string(source_maximum_extras_bytes - 8);
        value.entities.push_back(std::move(entity));
    }
    // Sixteen 4096-byte objects and the scene's two-byte object exceed the total.
    rejected(validate_scene_document(value), "source.scene.validate");
    value.entities.back().extras_json = extras_with_string(source_maximum_extras_bytes - 10);
    REQUIRE(validate_scene_document(value));
    auto encoded = encode_scene_document(value);
    REQUIRE(encoded);
    auto decoded = decode_scene_document(encoded.value());
    REQUIRE(decoded);
    CHECK(decoded.value().document == value);
    value.entities.back().extras_json = extras_with_string(source_maximum_extras_bytes - 9);
    rejected(validate_scene_document(value), "source.scene.validate");
    rejected(encode_scene_document(value), "source.scene.encode");
}

TEST_CASE("source parser bounds raw bytes nesting nodes and decoded strings",
          "[source-documents]") {
    std::string exact_bytes = scene_text();
    exact_bytes.resize(source_maximum_bytes, ' ');
    REQUIRE(decode_scene_document(exact_bytes));
    exact_bytes.push_back(' ');
    rejected_scene(exact_bytes);

    // Root scene + extras object + 30 nested arrays reaches container depth 32.
    const auto nested_extras = [](const std::size_t count) {
        return ",\"extras\":{\"x\":" + std::string(count, '[') + "0" + std::string(count, ']') +
               "}";
    };
    REQUIRE(decode_scene_document(scene_text(entity_text(), nested_extras(30))));
    rejected_scene(scene_text(entity_text(), nested_extras(31)));

    std::string many_nodes = ",\"extras\":{\"x\":[";
    for (std::size_t index = 0; index < source_maximum_json_nodes; ++index) {
        if (index != 0) {
            many_nodes += ',';
        }
        many_nodes += '0';
    }
    many_nodes += "]}";
    rejected_scene(scene_text(entity_text(), many_nodes));
    rejected_scene(scene_text(entity_text(), ",\"extras\":{\"x\":\"" +
                                                 std::string(source_maximum_string_bytes + 1, 'a') +
                                                 "\"}"));
    auto raw_extras = scene();
    raw_extras.extras_json = "{}";
    raw_extras.extras_json.resize(source_maximum_bytes + 1, ' ');
    rejected(validate_scene_document(raw_extras), "source.scene.validate");
}

TEST_CASE("typed extras depth includes the containing source document structure",
          "[source-documents]") {
    const auto nested = [](const std::size_t arrays) {
        return "{\"x\":" + std::string(arrays, '[') + "0" + std::string(arrays, ']') + "}";
    };
    auto scene_value = scene();
    scene_value.extras_json = nested(30);
    REQUIRE(validate_scene_document(scene_value));
    REQUIRE(encode_scene_document(scene_value));
    scene_value.extras_json = nested(31);
    rejected(validate_scene_document(scene_value), "source.scene.validate");
    rejected(encode_scene_document(scene_value), "source.scene.encode");
    scene_value.extras_json = "{}";
    scene_value.entities[0].extras_json = nested(28);
    REQUIRE(validate_scene_document(scene_value));
    REQUIRE(encode_scene_document(scene_value));
    scene_value.entities[0].extras_json = nested(29);
    rejected(validate_scene_document(scene_value), "source.scene.validate");
    rejected(encode_scene_document(scene_value), "source.scene.encode");

    auto project_value = project();
    project_value.extras_json = nested(30);
    REQUIRE(validate_project_document(project_value));
    REQUIRE(encode_project_document(project_value));
    project_value.extras_json = nested(31);
    rejected(validate_project_document(project_value), "source.project.validate");
    rejected(encode_project_document(project_value), "source.project.encode");
}

TEST_CASE("typed extras raw parse work is bounded across all supplied strings",
          "[source-documents]") {
    auto scene_value = scene();
    REQUIRE(scene_value.entities[0].extras_json == "{}");
    scene_value.extras_json = "{}";
    scene_value.extras_json.resize(source_maximum_bytes - 2, ' ');
    REQUIRE(validate_scene_document(scene_value));
    REQUIRE(encode_scene_document(scene_value));
    scene_value.extras_json.push_back(' ');
    rejected(validate_scene_document(scene_value), "source.scene.validate", "/entities/0/extras");
    rejected(encode_scene_document(scene_value), "source.scene.encode", "/entities/0/extras");

    auto project_value = project();
    project_value.extras_json = "{}";
    project_value.extras_json.resize(source_maximum_bytes, ' ');
    REQUIRE(validate_project_document(project_value));
    REQUIRE(encode_project_document(project_value));
    project_value.extras_json.push_back(' ');
    rejected(validate_project_document(project_value), "source.project.validate", "/extras");
    rejected(encode_project_document(project_value), "source.project.encode", "/extras");
}

TEST_CASE("failed legacy migration is explicit and preserves the original bytes",
          "[source-documents]") {
    const auto valid = fixture("scene-v0.sharkscene");
    for (const auto& input : std::vector<std::string>{
             replace_one(valid, "[0, 0, 0, 2]", "[0, 0, 0, 0]"),
             replace_one(valid, "\"name\": \"灯台 🦈\"", "\"name\": 12"),
             replace_one(valid, "\"parent\": \"" + std::string{root_id} + "\"",
                         "\"parent\": \"00000000-0000-4000-8000-000000000099\""),
             replace_one(valid, "\"id\": \"" + std::string{root_id} + "\"",
                         "\"id\": \"" + std::string{child_id} + "\"")}) {
        rejected_scene(input);
    }
    CHECK(fixture("scene-v0.sharkscene") == valid);
}

TEST_CASE("encoding stops at the output budget even when typed component values are valid",
          "[source-documents]") {
    auto value = scene();
    value.entities.clear();
    for (std::size_t index = 0; index < source_maximum_entities; ++index) {
        auto entity = indexed_entity(index);
        // Escaped quotes expand on serialization; large finite values retain their
        // full decimal spelling. No individual typed field exceeds its own bound.
        entity.name.value = std::string(shark::maximum_name_bytes, '"');
        entity.transform.translation = shark::math::Vec3{std::numeric_limits<double>::max()};
        entity.transform.scale = shark::math::Vec3{std::numeric_limits<double>::max()};
        entity.transform.rotation = {0.1, 0.2, 0.3, 0.4};
        value.entities.push_back(std::move(entity));
    }
    const auto saved = value;
    REQUIRE(validate_scene_document(value));
    rejected(encode_scene_document(value), "source.scene.encode");
    CHECK(value == saved);
}

TEST_CASE("validation diagnostics identify the operation JSON pointer and relevant entity",
          "[source-documents]") {
    auto value = scene();
    value.entities[0].name.value.clear();
    const auto result = validate_scene_document(value);
    rejected(result, "source.scene.validate", "/entities/0/components/shark.name/value");
    CHECK(result.error().context.entity_id == root_id);
    CHECK(result.error().code == shark::ErrorCode::invalid_argument);
    rejected_scene(replace_one(scene_text(), "\"shark.name\":{\"schema_version\":1}",
                               "\"shark.name\":{\"schema_version\":1,\"value\":\"\"}"),
                   "/entities/0/components/shark.name/value");
    const auto invalid_path =
        replace_one(fixture("project-v1.sharkproject"), "scenes/海岸.sharkscene", "../escape");
    const auto project_error = decode_project_document(invalid_path);
    rejected(project_error, "source.project.decode", "/scenes/0/path");
    CHECK(project_error.error().code == shark::ErrorCode::invalid_path);
}
