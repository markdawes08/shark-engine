#include "shark/tools/gltf_source.hpp"

#include "gltf_source_test_port.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace shark;
using namespace shark::tools;

template <typename T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().message);
        INFO(result.error().context.path);
    }
    REQUIRE(result);
    return std::move(result).value();
}

ProjectPath path(std::string_view text) {
    return take(ProjectPath::parse(text));
}
AssetId asset(unsigned number) {
    std::ostringstream text;
    text << "12000000-0000-4000-8000-" << std::hex << std::setw(12) << std::setfill('0') << number;
    return AssetId{take(StableId::parse(text.str()))};
}
std::span<const std::byte> bytes(std::string_view text) {
    return std::as_bytes(std::span<const char>{text.data(), text.size()});
}
std::string fixture(const char* name) {
    const auto file = std::filesystem::path{SHARK_GLTF_FIXTURE_DIRECTORY} / name;
    std::ifstream input{file, std::ios::binary};
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}
std::string replace(std::string text, std::string_view before, std::string_view after) {
    const auto position = text.find(before);
    REQUIRE(position != std::string::npos);
    text.replace(position, before.size(), after);
    return text;
}
Result<GltfSourceInspection> inspect(std::string_view text,
                                     std::span<const GltfExternalSource> external = {},
                                     std::string_view source = "model.gltf") {
    return inspect_gltf_source({path(source), bytes(text), external, {}});
}
void rejected(const Result<GltfSourceInspection>& result) {
    REQUIRE_FALSE(result);
    CHECK_FALSE(result.error().message.empty());
    CHECK(result.error().message.size() <= 4096);
    CHECK(result.error().context.operation == "asset.gltf.inspect");
    CHECK(detail::gltf_live_allocations() == 0);
}
void write_u32(std::string& text, std::size_t offset, std::uint32_t value) {
    REQUIRE(offset + 4 <= text.size());
    for (std::size_t index = 0; index < 4; ++index) {
        text[offset + index] = static_cast<char>((value >> (index * 8)) & 255U);
    }
}
std::uint32_t read_u32(const std::string& text, std::size_t offset) {
    REQUIRE(offset + 4 <= text.size());
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(text[offset + index]))
                 << (index * 8);
    }
    return value;
}
std::string make_glb(std::string json, std::string binary) {
    json.append((4 - json.size() % 4) % 4, ' ');
    REQUIRE(binary.size() % 4 == 0);
    std::string result(12, '\0');
    result.replace(0, 4, "glTF");
    write_u32(result, 4, 2);
    result.resize(20, '\0');
    write_u32(result, 12, static_cast<std::uint32_t>(json.size()));
    write_u32(result, 16, 0x4E4F534AU);
    result += json;
    const auto bin_header = result.size();
    result.resize(result.size() + 8, '\0');
    write_u32(result, bin_header, static_cast<std::uint32_t>(binary.size()));
    write_u32(result, bin_header + 4, 0x004E4942U);
    result += binary;
    write_u32(result, 8, static_cast<std::uint32_t>(result.size()));
    return result;
}
std::string buffer_document(std::string_view uri, std::size_t length) {
    return "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"" + std::string{uri} +
           "\",\"byteLength\":" + std::to_string(length) + "}]}";
}
std::string top_level(std::string_view fields) {
    return "{\"asset\":{\"version\":\"2.0\"}," + std::string{fields} + "}";
}
std::string objects(std::size_t count) {
    std::string result;
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0)
            result.push_back(',');
        result += "{}";
    }
    return result;
}
std::string regular_accessor(std::string_view accessor, std::size_t length = 36,
                             std::string_view view_fields = {}) {
    return top_level(
        "\"buffers\":[{\"uri\":\"values.bin\",\"byteLength\":" + std::to_string(length) +
        "}],\"bufferViews\":[{\"buffer\":0,\"byteLength\":" + std::to_string(length) +
        std::string{view_fields} + "}],\"accessors\":[{" + std::string{accessor} + "}]");
}
struct AllocationReset {
    ~AllocationReset() {
        detail::gltf_fail_allocation(std::nullopt);
    }
};
} // namespace

TEST_CASE("glTF source inspection accepts the authored empty static document", "[gltf-source]") {
    const auto report = take(inspect(fixture("minimal.gltf")));
    CHECK(report.container == GltfContainer::json);
    CHECK(report.counts.scenes == 0);
    CHECK(report.counts.nodes == 0);
    CHECK(report.counts.meshes == 0);
    CHECK(report.counts.primitives == 0);
    CHECK(report.counts.accessors == 0);
    CHECK(report.counts.materials == 0);
    CHECK(report.counts.textures == 0);
    CHECK(report.counts.samplers == 0);
    CHECK_FALSE(report.selected_scene);
    CHECK(report.resources.empty());
    CHECK(report.diagnostics.empty());
}

TEST_CASE("glTF external triangle has independently known counts identity and source digest",
          "[gltf-source]") {
    const auto data = fixture("triangle.bin");
    REQUIRE(data.size() == 36);
    const std::array external{
        GltfExternalSource{path("models/triangle.bin"), asset(1), bytes(data)}};
    const auto report = take(inspect(fixture("external.gltf"), external, "models/triangle.gltf"));
    CHECK(report.container == GltfContainer::json);
    CHECK(report.counts.scenes == 1);
    CHECK(report.counts.nodes == 1);
    CHECK(report.counts.meshes == 1);
    CHECK(report.counts.primitives == 1);
    CHECK(report.counts.accessors == 1);
    REQUIRE(report.selected_scene);
    CHECK(*report.selected_scene == 0);
    REQUIRE(report.resources.size() == 1);
    const auto& resource = report.resources.front();
    CHECK(resource.kind == GltfResourceKind::buffer);
    CHECK(resource.index == 0);
    CHECK(resource.storage == GltfResourceStorage::external);
    REQUIRE(resource.path);
    CHECK(resource.path->text() == "models/triangle.bin");
    CHECK(resource.asset == asset(1));
    CHECK(resource.byte_count == 36);
    CHECK(resource.digest.to_hex() ==
          "d8b87038ed2ace74ed183166f6b0a541eab77f7184d50e65edc4bca5ccb5445a");
    CHECK(report.diagnostics.empty());
}

TEST_CASE("glTF data URI and GLB resources preserve the same authored triangle bytes",
          "[gltf-source]") {
    const auto embedded = take(inspect(fixture("embedded.gltf")));
    const auto binary = take(inspect(fixture("triangle.glb"), {}, "model.glb"));
    CHECK(embedded.container == GltfContainer::json);
    CHECK(binary.container == GltfContainer::binary);
    REQUIRE(embedded.resources.size() == 1);
    REQUIRE(binary.resources.size() == 1);
    CHECK(embedded.resources[0].storage == GltfResourceStorage::data_uri);
    CHECK(binary.resources[0].storage == GltfResourceStorage::binary_chunk);
    CHECK(embedded.resources[0].digest == binary.resources[0].digest);
    CHECK(binary.resources[0].digest.to_hex() ==
          "d8b87038ed2ace74ed183166f6b0a541eab77f7184d50e65edc4bca5ccb5445a");
    CHECK(embedded.resources[0].byte_count == 36);
    CHECK(binary.resources[0].byte_count == 36);
    CHECK_FALSE(binary.resources[0].path);
    CHECK_FALSE(binary.resources[0].asset);
    CHECK(binary.counts.primitives == 1);
    CHECK(binary.counts.accessors == 1);
}

TEST_CASE("glTF image references report source bytes without claiming decoded pixels",
          "[gltf-source]") {
    const auto image = fixture("pixel.png");
    const std::array external{
        GltfExternalSource{path("textures/pixel.png"), asset(2), bytes(image)}};
    const auto report = take(inspect(top_level(R"("images":[{"uri":"../textures/pixel.png"}])"),
                                     external, "models/model.gltf"));
    REQUIRE(report.resources.size() == 1);
    const auto& resource = report.resources.front();
    CHECK(resource.kind == GltfResourceKind::image);
    CHECK(resource.storage == GltfResourceStorage::external);
    CHECK(resource.path == path("textures/pixel.png"));
    CHECK(resource.asset == asset(2));
    CHECK(resource.byte_count == 68);
    CHECK(resource.mime_type == "image/png");
    CHECK(resource.digest.to_hex() ==
          "7e37c3392d585d2d06e4aa3b63b52a4f4babd914538918dd8868e2527070d344");
}

TEST_CASE("glTF static negative nonuniform scale and matrix nodes remain inspectable source data",
          "[gltf-source]") {
    const auto data = fixture("triangle.bin");
    const std::array external{GltfExternalSource{path("triangle.bin"), asset(1), bytes(data)}};
    for (const auto node : {R"({"mesh":0,"translation":[3,4,5],"scale":[-1,2,3]})",
                            R"({"mesh":0,"rotation":[0,0,0,1],"scale":[1,2,1]})",
                            R"({"mesh":0,"matrix":[-1,0,0,0,0,2,0,0,0,0,3,0,3,4,5,1]})"}) {
        const auto source = replace(fixture("external.gltf"), R"({"mesh":0})", node);
        const auto report = take(inspect(source, external));
        CHECK(report.counts.nodes == 1);
        CHECK(report.counts.meshes == 1);
        REQUIRE(report.resources.size() == 1);
        CHECK(report.resources[0].digest.to_hex() ==
              "d8b87038ed2ace74ed183166f6b0a541eab77f7184d50e65edc4bca5ccb5445a");
    }
}

TEST_CASE("glTF selected scene and default scene policy are explicit", "[gltf-source]") {
    const auto selected = take(inspect(top_level(R"("scenes":[{},{}],"scene":1)")));
    REQUIRE(selected.selected_scene);
    CHECK(*selected.selected_scene == 1);
    const auto first = take(inspect(top_level(R"("scenes":[{},{}])")));
    REQUIRE(first.selected_scene);
    CHECK(*first.selected_scene == 0);
    rejected(inspect(top_level(R"("scenes":[{}],"scene":1)")));
    rejected(inspect(top_level(R"("scene":0)")));
    rejected(inspect(top_level(R"("scenes":[{}],"scene":-1)")));
    rejected(inspect(top_level(R"("nodes":[{}],"scenes":[{"nodes":[1]}])")));
}

TEST_CASE("glTF containers require the matching supported filename and version", "[gltf-source]") {
    const auto minimal = fixture("minimal.gltf");
    rejected(inspect(minimal, {}, "model.obj"));
    rejected(inspect(minimal, {}, "model.glb"));
    rejected(inspect(fixture("triangle.glb"), {}, "model.gltf"));
    for (const auto version : {"1.0", "2.1", "3.0"}) {
        INFO(version);
        rejected(inspect(replace(minimal, "2.0", version)));
    }
    rejected(inspect(R"({"asset":{"version":2}})"));
    rejected(inspect(R"({"asset":{"version":"2.0","minVersion":"2.1"}})"));
    rejected(inspect(R"({"asset":{}})"));
    rejected(inspect("[]"));
}

TEST_CASE("glTF GLB header truncation version and declared length reject", "[gltf-source]") {
    const auto valid = fixture("triangle.glb");
    for (const std::size_t length :
         {std::size_t{0}, std::size_t{4}, std::size_t{11}, valid.size() - 1}) {
        INFO(length);
        rejected(inspect(valid.substr(0, length), {}, "model.glb"));
    }
    auto malformed = valid;
    malformed[0] = 'x';
    rejected(inspect(malformed, {}, "model.glb"));
    malformed = valid;
    write_u32(malformed, 4, 1);
    rejected(inspect(malformed, {}, "model.glb"));
    malformed = valid;
    write_u32(malformed, 8, static_cast<std::uint32_t>(valid.size() + 4));
    rejected(inspect(malformed, {}, "model.glb"));
    malformed = valid + "abcd";
    rejected(inspect(malformed, {}, "model.glb"));
}

TEST_CASE("glTF GLB chunk lengths order duplicate BIN and NUL padding reject", "[gltf-source]") {
    const auto valid = fixture("triangle.glb");
    const auto json_length = read_u32(valid, 12);
    const auto bin_header = std::size_t{20} + json_length;
    auto malformed = valid;
    write_u32(malformed, 16, 0x004E4942U);
    rejected(inspect(malformed, {}, "model.glb"));
    malformed = valid;
    write_u32(malformed, 12, json_length - 1);
    rejected(inspect(malformed, {}, "model.glb"));
    malformed = valid;
    write_u32(malformed, bin_header, 40);
    rejected(inspect(malformed, {}, "model.glb"));
    malformed = valid;
    malformed[bin_header - 1] = '\0';
    rejected(inspect(malformed, {}, "model.glb"));
    malformed = valid + valid.substr(bin_header);
    write_u32(malformed, 8, static_cast<std::uint32_t>(malformed.size()));
    rejected(inspect(malformed, {}, "model.glb"));
}

TEST_CASE("glTF JSON preflight rejects duplicate decoded keys malformed UTF8 and nonfinite input",
          "[gltf-source]") {
    for (const auto malformed :
         {R"({"asset":{"version":"2.0","version":"2.0"}})",
          R"({"asset":{"version":"2.0","vers\u0069on":"2.0"}})",
          R"({"asset":{"version":"2.0"},"extras":1e999})",
          R"({"asset":{"version":"2.0"},"extras":NaN})", R"({"asset":{"version":"2.0"}} trailing)",
          R"({"asset":{"version":"2.0"},})"}) {
        INFO(malformed);
        rejected(inspect(malformed));
    }
    auto malformed = fixture("minimal.gltf");
    malformed.push_back('\0');
    rejected(inspect(malformed));
    malformed = top_level("\"extras\":\"" + std::string(1, static_cast<char>(0xFF)) + "\"");
    rejected(inspect(malformed));
}

TEST_CASE(
    "glTF private parser integer conversions reject overflowing fields and attribute suffixes",
    "[gltf-source]") {
    const auto sampler = take(inspect(top_level(
        R"("samplers":[{"wrapS":33071,"wrapT":10497,"minFilter":9987,"magFilter":9729}])")));
    CHECK(sampler.counts.samplers == 1);
    CHECK(detail::gltf_live_allocations() == 0);
    for (const auto value :
         {"2147483648", "18446744073709551615", "2147483647", "65536", "-1", "1.5", "true"}) {
        INFO(value);
        rejected(inspect(top_level(std::string{"\"samplers\":[{\"wrapS\":"} + value + "}]")));
        CHECK(detail::gltf_live_allocations() == 0);
    }
    const auto data = fixture("triangle.bin");
    const std::array external{GltfExternalSource{path("triangle.bin"), asset(1), bytes(data)}};
    const auto source = fixture("external.gltf");
    CHECK(inspect(replace(source, R"("POSITION":0)", R"("POSITION":0,"COLOR_0":0)"), external));
    for (const auto attribute :
         {"COLOR_2147483648", "JOINTS_18446744073709551615", "WEIGHTS_99999999999999999999999",
          "NORMAL_2147483648", "TANGENT_2147483648", "POSITION_2147483648"}) {
        INFO(attribute);
        rejected(inspect(replace(source, R"("POSITION":0)",
                                 std::string{R"("POSITION":0,")"} + attribute + "\":0"),
                         external));
        CHECK(detail::gltf_live_allocations() == 0);
    }
}

TEST_CASE("glTF GLB permits only the bounded zero padding after its declared buffer bytes",
          "[gltf-source]") {
    const auto json = top_level(R"("buffers":[{"byteLength":1}])");
    std::string binary(4, '\0');
    binary[0] = 'x';
    const auto report = take(inspect(make_glb(json, binary), {}, "model.glb"));
    REQUIRE(report.resources.size() == 1);
    CHECK(report.resources[0].byte_count == 1);
    binary[3] = 'x';
    rejected(inspect(make_glb(json, binary), {}, "model.glb"));
    binary.assign(8, '\0');
    binary[0] = 'x';
    rejected(inspect(make_glb(json, binary), {}, "model.glb"));
}

TEST_CASE("glTF required extensions reject and optional core fallback emits a bounded diagnostic",
          "[gltf-source]") {
    const auto optional =
        take(inspect(top_level(R"("extensionsUsed":["SHARK_optional_fixture"])")));
    REQUIRE_FALSE(optional.diagnostics.empty());
    CHECK(optional.diagnostics.size() <= gltf_source_maximum_diagnostics);
    CHECK(optional.diagnostics.front().find("SHARK_optional_fixture") != std::string::npos);
    for (const auto extension : {"KHR_draco_mesh_compression", "EXT_meshopt_compression",
                                 "KHR_texture_basisu", "SHARK_unknown"}) {
        INFO(extension);
        auto document = top_level("\"extensionsUsed\":[\"" + std::string{extension} +
                                  "\"],\"extensionsRequired\":[\"" + extension + "\"]");
        const auto result = inspect(document);
        rejected(result);
        CHECK(result.error().message.find(extension) != std::string::npos);
    }
    std::string extensions;
    for (std::size_t i = 0; i <= gltf_source_maximum_diagnostics; ++i) {
        if (i != 0)
            extensions.push_back(',');
        extensions += "\"SHARK_optional_" + std::to_string(i) + "\"";
    }
    rejected(inspect(top_level("\"extensionsUsed\":[" + extensions + "]")));
}

TEST_CASE("glTF skeletal animation and morph input remains explicitly unsupported",
          "[gltf-source]") {
    for (const auto fields : {R"("skins":[{}])", R"("animations":[{}])",
                              R"("meshes":[{"weights":[1],"primitives":[]}])",
                              R"("meshes":[{"primitives":[{"attributes":{},"targets":[{}]}]}])",
                              R"("nodes":[{"skin":0}],"skins":[{}])"}) {
        INFO(fields);
        rejected(inspect(top_level(fields)));
    }
}

TEST_CASE("glTF static camera nodes produce diagnostics instead of engine camera components",
          "[gltf-source]") {
    const auto report = take(inspect(
        top_level(R"("cameras":[{"type":"perspective","perspective":{"yfov":1,"znear":0.1}}],)"
                  R"("nodes":[{"camera":0}],"scenes":[{"nodes":[0]}])")));
    CHECK(report.counts.nodes == 1);
    CHECK_FALSE(report.diagnostics.empty());
}

TEST_CASE("glTF source node cycles and multiple parents reject before any recursive traversal",
          "[gltf-source]") {
    rejected(inspect(top_level(R"("nodes":[{"children":[1]},{"children":[0]}])")));
    rejected(inspect(top_level(R"("nodes":[{"children":[0]}])")));
    rejected(inspect(top_level(R"("nodes":[{"children":[2]},{"children":[2]},{}])")));
    const auto report =
        take(inspect(top_level(R"("nodes":[{"children":[1]},{}],"scenes":[{"nodes":[0]}])")));
    CHECK(report.counts.nodes == 2);
}

TEST_CASE(
    "glTF URI decoding and parent normalization resolve within the explicit project snapshots",
    "[gltf-source]") {
    const std::string data = "data";
    for (const auto uri : {"../shared/data.bin", "../shared/./data.bin", "../shared/%64ata.bin",
                           "../shared/folder/../data.bin"}) {
        INFO(uri);
        const std::array external{
            GltfExternalSource{path("shared/data.bin"), asset(1), bytes(data)}};
        const auto report = take(inspect(buffer_document(uri, 4), external, "models/model.gltf"));
        REQUIRE(report.resources.size() == 1);
        CHECK(report.resources[0].path == path("shared/data.bin"));
        CHECK(report.resources[0].asset == asset(1));
        CHECK(report.resources[0].byte_count == 4);
    }
    const std::array external{
        GltfExternalSource{path("shared/caf\xC3\xA9.bin"), asset(2), bytes(data)}};
    const auto unicode =
        take(inspect(buffer_document("../shared/caf%C3%A9.bin", 4), external, "models/model.gltf"));
    REQUIRE(unicode.resources.size() == 1);
    CHECK(unicode.resources[0].path == path("shared/caf\xC3\xA9.bin"));
}

TEST_CASE("glTF URI paths reject escapes schemes aliases and malformed percent encoding",
          "[gltf-source]") {
    for (const auto uri :
         {"../../escape.bin", "%2e%2e/%2e%2e/escape.bin", "/absolute.bin", "//server/share.bin",
          "https://example.test/data.bin", "file:data.bin", "C:/data.bin", "data.bin?version=1",
          "data.bin#part", "bad%00.bin", "bad%0a.bin", "bad%.bin", "bad%0.bin", "bad%GG.bin",
          "%FF.bin", "bad%5cpath.bin", "CON.bin", "file:stream", "trailing.%20"}) {
        INFO(uri);
        rejected(inspect(buffer_document(uri, 4), {}, "models/model.gltf"));
    }
    rejected(inspect(buffer_document("data.bin", 4), {}, "models/model.gltf"));
}

TEST_CASE("glTF URI normalization cannot hide malformed portable components that are later removed",
          "[gltf-source]") {
    const std::string data = "data";
    const std::array external{GltfExternalSource{path("models/mesh.bin"), asset(1), bytes(data)}};
    CHECK(inspect(buffer_document("safe/../mesh.bin", 4), external, "models/model.gltf"));
    for (const auto uri :
         {"%ff/../mesh.bin", "bad%0a/../mesh.bin", "bad%7f/../mesh.bin", "CON/../mesh.bin"}) {
        INFO(uri);
        rejected(inspect(buffer_document(uri, 4), external, "models/model.gltf"));
    }
}

TEST_CASE("glTF external source mappings reject duplicates aliases unused values and wrong sizes",
          "[gltf-source]") {
    const std::string data = "data";
    const auto source = buffer_document("data.bin", 4);
    const GltfExternalSource original{path("data.bin"), asset(1), bytes(data)};
    const std::array duplicate_path{original,
                                    GltfExternalSource{path("data.bin"), asset(2), bytes(data)}};
    rejected(inspect(source, duplicate_path));
    const std::array duplicate_id{original,
                                  GltfExternalSource{path("other.bin"), asset(1), bytes(data)}};
    rejected(inspect(source, duplicate_id));
    const std::array case_alias{original,
                                GltfExternalSource{path("DATA.bin"), asset(2), bytes(data)}};
    rejected(inspect(source, case_alias));
    const std::array unused{original,
                            GltfExternalSource{path("unused.bin"), asset(2), bytes(data)}};
    rejected(inspect(source, unused));
    const std::array one{original};
    rejected(inspect(buffer_document("data.bin", 5), one));
    const auto trailing = take(inspect(buffer_document("data.bin", 3), one));
    REQUIRE(trailing.resources.size() == 1);
    CHECK(trailing.resources[0].byte_count == 4);
    CHECK(trailing.resources[0].digest == take(sha256(bytes(data))));
    const std::array wrong_case{GltfExternalSource{path("DATA.bin"), asset(1), bytes(data)}};
    rejected(inspect(source, wrong_case));
    rejected(inspect(fixture("external.gltf")));
}

TEST_CASE("glTF source and external per-file budgets are charged before parsing", "[gltf-source]") {
    const std::string oversized(gltf_source_maximum_file_bytes + 1, ' ');
    rejected(inspect(oversized));
    const std::array external{GltfExternalSource{path("large.bin"), asset(1), bytes(oversized)}};
    rejected(inspect(buffer_document("large.bin", oversized.size()), external));
    std::string at_limit = fixture("minimal.gltf");
    at_limit.append(gltf_source_maximum_file_bytes - at_limit.size(), ' ');
    CHECK(inspect(at_limit));
}

TEST_CASE("glTF repeated external references share one exact source and AssetId mapping",
          "[gltf-source]") {
    const std::string data = "data";
    const std::array external{GltfExternalSource{path("data.bin"), asset(1), bytes(data)}};
    const auto report = take(inspect(
        top_level(
            R"("buffers":[{"uri":"data.bin","byteLength":4},{"uri":"data.bin","byteLength":4}])"),
        external));
    REQUIRE(report.resources.size() == 2);
    CHECK(report.resources[0].asset == asset(1));
    CHECK(report.resources[1].asset == asset(1));
    CHECK(report.resources[0].digest == report.resources[1].digest);
    CHECK(report.resources[0].path == report.resources[1].path);
}

TEST_CASE("glTF external closure has a positive 127-resource boundary and rejects the next mapping",
          "[gltf-source]") {
    const std::string data = "x";
    std::vector<GltfExternalSource> external;
    std::string buffers;
    for (std::size_t i = 0; i < gltf_source_maximum_resources; ++i) {
        const auto name = "resource" + std::to_string(i) + ".bin";
        external.push_back({path(name), asset(static_cast<unsigned>(i + 1)), bytes(data)});
        if (i != 0)
            buffers.push_back(',');
        buffers += "{\"uri\":\"" + name + "\",\"byteLength\":1}";
    }
    const auto report = take(inspect(top_level("\"buffers\":[" + buffers + "]"), external));
    CHECK(report.resources.size() == gltf_source_maximum_resources);
    external.push_back({path("extra.bin"), asset(200), bytes(data)});
    rejected(inspect(top_level("\"buffers\":[" + buffers + "]"), external));
}

TEST_CASE("glTF independent aggregate input budget counts every declared external snapshot",
          "[gltf-source]") {
    const std::string data(gltf_source_maximum_file_bytes, '\0');
    std::vector<GltfExternalSource> external;
    std::string buffers;
    for (unsigned i = 0; i < 15; ++i) {
        const auto name = "resource" + std::to_string(i) + ".bin";
        external.push_back({path(name), asset(i + 1), bytes(data)});
        if (i != 0)
            buffers.push_back(',');
        buffers += "{\"uri\":\"" + name + "\",\"byteLength\":" + std::to_string(data.size()) + "}";
    }
    CHECK(inspect(top_level("\"buffers\":[" + buffers + "]"), external));
    external.push_back({path("resource15.bin"), asset(16), bytes(data)});
    buffers += ",{\"uri\":\"resource15.bin\",\"byteLength\":" + std::to_string(data.size()) + "}";
    rejected(inspect(top_level("\"buffers\":[" + buffers + "]"), external));
}

TEST_CASE("glTF resolved resource budget charges repeated external aliases independently",
          "[gltf-source]") {
    const std::string data(gltf_source_maximum_file_bytes, '\0');
    const std::array external{GltfExternalSource{path("shared.bin"), asset(1), bytes(data)}};
    const auto one = "{\"uri\":\"shared.bin\",\"byteLength\":" + std::to_string(data.size()) + "}";
    const auto count = gltf_source_maximum_total_bytes / data.size();
    REQUIRE(count == 16);
    std::string buffers;
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0)
            buffers.push_back(',');
        buffers += one;
    }
    const auto report = take(inspect(top_level("\"buffers\":[" + buffers + "]"), external));
    REQUIRE(report.resources.size() == count);
    std::size_t resolved_bytes = 0;
    for (const auto& resource : report.resources) {
        CHECK(resource.byte_count == data.size());
        CHECK(resource.asset == asset(1));
        resolved_bytes += resource.byte_count;
    }
    CHECK(resolved_bytes == gltf_source_maximum_total_bytes);
    buffers += ',' + one;
    rejected(inspect(top_level("\"buffers\":[" + buffers + "]"), external));
}

TEST_CASE("glTF resolved resource budget also charges every repeated image buffer view",
          "[gltf-source]") {
    auto data = fixture("pixel.png");
    data.resize(gltf_source_maximum_file_bytes, '\0');
    const std::array external{GltfExternalSource{path("image.bin"), asset(1), bytes(data)}};
    const auto head =
        "\"buffers\":[{\"uri\":\"image.bin\",\"byteLength\":" + std::to_string(data.size()) +
        "}],\"bufferViews\":[{\"buffer\":0,\"byteLength\":" + std::to_string(data.size()) +
        "}],\"images\":[";
    const auto count = gltf_source_maximum_total_bytes / data.size() - 1;
    REQUIRE(count == 15);
    std::string images;
    constexpr std::string_view one = R"({"bufferView":0,"mimeType":"image/png"})";
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0)
            images.push_back(',');
        images += one;
    }
    const auto report = take(inspect(top_level(head + images + ']'), external));
    REQUIRE(report.resources.size() == count + 1);
    std::size_t resolved_bytes = 0;
    for (const auto& resource : report.resources)
        resolved_bytes += resource.byte_count;
    CHECK(resolved_bytes == gltf_source_maximum_total_bytes);
    CHECK(report.resources.back().kind == GltfResourceKind::image);
    CHECK(report.resources.back().storage == GltfResourceStorage::buffer_view);
    images += ',';
    images += one;
    rejected(inspect(top_level(head + images + ']'), external));
}

TEST_CASE("glTF total accessor elements are bounded independently of table count and source bytes",
          "[gltf-source]") {
    const std::string data(gltf_source_maximum_accessor_elements, '\0');
    const std::array external{GltfExternalSource{path("values.bin"), asset(1), bytes(data)}};
    const auto count =
        gltf_source_maximum_total_accessor_elements / gltf_source_maximum_accessor_elements;
    REQUIRE(count == 16);
    const auto one = "{\"bufferView\":0,\"componentType\":5121,\"count\":" +
                     std::to_string(gltf_source_maximum_accessor_elements) +
                     ",\"type\":\"SCALAR\"}";
    std::string accessors;
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0)
            accessors.push_back(',');
        accessors += one;
    }
    const auto head =
        "\"buffers\":[{\"uri\":\"values.bin\",\"byteLength\":" + std::to_string(data.size()) +
        "}],\"bufferViews\":[{\"buffer\":0,\"byteLength\":" + std::to_string(data.size()) +
        "}],\"accessors\":[";
    const auto report = take(inspect(top_level(head + accessors + ']'), external));
    CHECK(report.counts.accessors == count);
    REQUIRE(report.resources.size() == 1);
    CHECK(report.resources[0].byte_count == gltf_source_maximum_accessor_elements);
    CHECK(report.counts.accessors * gltf_source_maximum_accessor_elements ==
          gltf_source_maximum_total_accessor_elements);
    accessors += R"(,{"bufferView":0,"componentType":5121,"count":1,"type":"SCALAR"})";
    rejected(inspect(top_level(head + accessors + ']'), external));
}

TEST_CASE("glTF table and recursive JSON bounds have accepted and rejected neighbors",
          "[gltf-source]") {
    const auto nodes = "\"nodes\":[" + objects(gltf_source_maximum_table_entries) + "]";
    const auto report = take(inspect(top_level(nodes)));
    CHECK(report.counts.nodes == gltf_source_maximum_table_entries);
    rejected(
        inspect(top_level("\"nodes\":[" + objects(gltf_source_maximum_table_entries + 1) + "]")));
    CHECK(inspect(top_level("\"extras\":" + std::string(31, '[') + "0" + std::string(31, ']'))));
    rejected(inspect(top_level("\"extras\":" + std::string(32, '[') + "0" + std::string(32, ']'))));
    std::string scalars;
    for (std::size_t i = 0; i < 65536; ++i) {
        if (i != 0)
            scalars.push_back(',');
        scalars += "null";
    }
    rejected(inspect(top_level("\"extras\":[" + scalars + "]")));
}

TEST_CASE("glTF total table count is bounded independently from each table", "[gltf-source]") {
    const auto entries = objects(gltf_source_maximum_table_entries);
    const auto fields = "\"nodes\":[" + entries + "],\"scenes\":[" + entries + "],\"materials\":[" +
                        entries + "],\"samplers\":[" + entries + "]";
    const auto report = take(inspect(top_level(fields)));
    CHECK(report.counts.nodes + report.counts.scenes + report.counts.materials +
              report.counts.samplers ==
          gltf_source_maximum_total_entries);
    rejected(inspect(top_level(fields + ",\"meshes\":[{\"primitives\":[]}]")));
}

TEST_CASE("glTF buffer views reject invalid references arithmetic and unsupported strides",
          "[gltf-source]") {
    const auto data = fixture("triangle.bin");
    const std::array external{GltfExternalSource{path("triangle.bin"), asset(1), bytes(data)}};
    const auto source = fixture("external.gltf");
    for (const auto replacement :
         {R"("buffer":1,"byteLength":36)", R"("buffer":0,"byteLength":37)",
          R"("buffer":0,"byteOffset":1,"byteLength":36)",
          R"("buffer":0,"byteOffset":18446744073709551615,"byteLength":36)",
          R"("buffer":0,"byteLength":36,"byteStride":0)",
          R"("buffer":0,"byteLength":36,"byteStride":3)",
          R"("buffer":0,"byteLength":36,"byteStride":256)"}) {
        INFO(replacement);
        rejected(inspect(replace(source, R"("buffer":0,"byteLength":36)", replacement), external));
    }
}

TEST_CASE("glTF accessors validate offsets count component sizes and sparse-free bounds",
          "[gltf-source]") {
    const auto data = fixture("triangle.bin");
    const std::array external{GltfExternalSource{path("values.bin"), asset(1), bytes(data)}};
    const std::string base = R"("bufferView":0,"componentType":5126,"count":3,"type":"VEC3")";
    CHECK(inspect(regular_accessor(base), external));
    for (const auto& accessor :
         {replace(base, "\"count\":3", "\"count\":4"), replace(base, "\"count\":3", "\"count\":0"),
          replace(base, "\"count\":3", "\"count\":262145"),
          replace(base, "\"count\":3", "\"count\":-1"),
          replace(base, "\"count\":3", "\"count\":1.5"),
          replace(base, "\"componentType\":5126", "\"componentType\":9999"),
          replace(base, "\"bufferView\":0", "\"bufferView\":1"), base + ",\"byteOffset\":4",
          base + ",\"byteOffset\":1", base + ",\"byteOffset\":18446744073709551615"}) {
        INFO(accessor);
        rejected(inspect(regular_accessor(accessor), external));
    }
    rejected(inspect(regular_accessor(base, 36, ",\"byteStride\":4"), external));
}

TEST_CASE("glTF matrix accessors account for column padding before touching bytes",
          "[gltf-source]") {
    const std::string data(12, '\0');
    const std::array external{GltfExternalSource{path("values.bin"), asset(1), bytes(data)}};
    constexpr auto accessor = R"("bufferView":0,"componentType":5121,"count":1,"type":"MAT3")";
    CHECK(inspect(regular_accessor(accessor, 12), external));
    const auto short_view = replace(regular_accessor(accessor, 12), R"("buffer":0,"byteLength":12)",
                                    R"("buffer":0,"byteLength":9)");
    rejected(inspect(short_view, external));
}

TEST_CASE("glTF sparse accessor fixtures retain validated implicit-zero source storage",
          "[gltf-source]") {
    const auto data = fixture("sparse.bin");
    const std::array external{GltfExternalSource{path("sparse.bin"), asset(3), bytes(data)}};
    const auto report = take(inspect(fixture("sparse.gltf"), external));
    CHECK(report.counts.accessors == 1);
    REQUIRE(report.resources.size() == 1);
    CHECK(report.resources[0].byte_count == 16);
    CHECK(report.resources[0].digest.to_hex() ==
          "547d701ac96ae29bfd2feef20150ce603277d24bdb0ab37efeb424359e5961eb");
    for (const auto& invalid :
         {replace(fixture("sparse.gltf"), R"("sparse":{"count":1)", R"("sparse":{"count":4)"),
          replace(fixture("sparse.gltf"), R"("componentType":5121)", R"("componentType":5126)"),
          replace(fixture("sparse.gltf"), R"("values":{"bufferView":1})",
                  R"("values":{"bufferView":1,"byteOffset":4})"),
          replace(fixture("sparse.gltf"), R"("indices":{"bufferView":0)",
                  R"("indices":{"bufferView":2)"),
          replace(fixture("sparse.gltf"), R"("byteOffset":0,"byteLength":1)",
                  R"("byteOffset":0,"byteLength":1,"target":34962)"),
          replace(fixture("sparse.gltf"), R"("byteOffset":0,"byteLength":1)",
                  R"("byteOffset":0,"byteLength":1,"byteStride":4)"),
          replace(fixture("sparse.gltf"), R"("byteOffset":4,"byteLength":12)",
                  R"("byteOffset":4,"byteLength":12,"byteStride":0)")}) {
        rejected(inspect(invalid, external));
    }
    const auto compensating =
        replace(replace(fixture("sparse.gltf"), R"("byteOffset":4,"byteLength":12)",
                        R"("byteOffset":3,"byteLength":13)"),
                R"("values":{"bufferView":1})", R"("values":{"bufferView":1,"byteOffset":1})");
    rejected(inspect(compensating, external));
}

TEST_CASE("glTF sparse indices must be sorted unique and within accessor count", "[gltf-source]") {
    const auto source = top_level(
        R"("buffers":[{"uri":"sparse.bin","byteLength":32}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":2},{"buffer":0,"byteOffset":8,"byteLength":24}],)"
        R"("accessors":[{"componentType":5126,"count":3,"type":"VEC3",)"
        R"("sparse":{"count":2,"indices":{"bufferView":0,"componentType":5121},"values":{"bufferView":1}}}])");
    std::string data(32, '\0');
    data[0] = 0;
    data[1] = 2;
    const std::array external{GltfExternalSource{path("sparse.bin"), asset(3), bytes(data)}};
    CHECK(inspect(source, external));
    for (const std::array<char, 2> pair : {std::array<char, 2>{1, 1}, {2, 1}, {0, 3}}) {
        data[0] = pair[0];
        data[1] = pair[1];
        rejected(inspect(source, external));
    }
}

TEST_CASE("glTF base64 requires valid MIME padding alphabet and declared decoded length",
          "[gltf-source]") {
    for (const auto uri :
         {"data:application/octet-stream;base64,?AAA", "data:application/octet-stream;base64,AA=A",
          "data:application/octet-stream;base64,AAAA=", "data:application/octet-stream;base64,AB==",
          "data:application/octet-stream;base64,AAB=", "data:application/octet-stream,AAAA",
          "data:text/plain;base64,AAAA", "data:application/octet-stream;base64,AAAA\n"}) {
        INFO(uri);
        rejected(inspect(buffer_document(uri, 3)));
    }
    CHECK(inspect(buffer_document("data:application/octet-stream;base64,AQ==", 1)));
    CHECK(inspect(buffer_document("data:application/gltf-buffer;base64,AQI=", 2)));
    rejected(inspect(buffer_document("data:application/octet-stream;base64,AQ==", 2)));
    const auto source =
        buffer_document("data:application/octet-stream;base64," + std::string(4096, 'A'), 3072);
    const auto report = take(inspect(source));
    REQUIRE(report.resources.size() == 1);
    CHECK(report.resources[0].byte_count == 3072);
}

TEST_CASE(
    "glTF primitive index validation uses effective sparse values instead of invalid base bytes",
    "[gltf-source]") {
    auto data = fixture("repaired-indices.bin");
    REQUIRE(data.size() == 46);
    const auto source = fixture("repaired-indices.gltf");
    const std::array external{
        GltfExternalSource{path("repaired-indices.bin"), asset(4), bytes(data)}};
    const auto report = take(inspect(source, external));
    CHECK(report.counts.accessors == 2);
    CHECK(report.counts.primitives == 1);
    REQUIRE(report.resources.size() == 1);
    CHECK(report.resources[0].byte_count == 46);
    data[44] = 3;
    rejected(inspect(source, external));
    data[44] = 2;
    const auto unpatched = replace(
        source,
        R"(,"sparse":{"count":1,"indices":{"bufferView":2,"componentType":5121},"values":{"bufferView":3}})",
        "");
    rejected(inspect(unpatched, external));
}

TEST_CASE("glTF image MIME and buffer-view source signatures are validated without decoding",
          "[gltf-source]") {
    const auto image = fixture("pixel.png");
    const std::array external{GltfExternalSource{path("image.bin"), asset(2), bytes(image)}};
    const auto source = top_level(R"("buffers":[{"uri":"image.bin","byteLength":68}],)"
                                  R"("bufferViews":[{"buffer":0,"byteLength":68}],)"
                                  R"("images":[{"bufferView":0,"mimeType":"image/png"}])");
    const auto report = take(inspect(source, external));
    REQUIRE(report.resources.size() == 2);
    CHECK(report.resources[1].kind == GltfResourceKind::image);
    CHECK(report.resources[1].storage == GltfResourceStorage::buffer_view);
    CHECK(report.resources[1].byte_count == 68);
    CHECK(report.resources[1].digest == report.resources[0].digest);
    rejected(inspect(replace(source, "image/png", "image/ktx2"), external));
    rejected(inspect(replace(source, "image/png", "image/jpeg"), external));
    rejected(inspect(replace(source, R"(,"mimeType":"image/png")", ""), external));
    const std::string invalid = "not an image";
    const std::array bad{GltfExternalSource{path("pixel.png"), asset(2), bytes(invalid)}};
    rejected(inspect(top_level(R"("images":[{"uri":"pixel.png"}])"), bad));
}

TEST_CASE("glTF inspection cancellation is cooperative and publishes no partial result",
          "[gltf-source]") {
    const auto data = fixture("embedded.gltf");
    std::stop_source stop;
    stop.request_stop();
    const auto result =
        inspect_gltf_source({path("model.gltf"), bytes(data), {}, stop.get_token()});
    rejected(result);
}

TEST_CASE("glTF inspection returns owned references and digests after snapshot buffers disappear",
          "[gltf-source]") {
    std::optional<GltfSourceInspection> retained;
    {
        std::string data = fixture("triangle.bin");
        std::string source = fixture("external.gltf");
        std::vector<GltfExternalSource> external{{path("triangle.bin"), asset(1), bytes(data)}};
        retained = take(inspect(source, external));
        data.assign(data.size(), 'x');
        source.clear();
        external.clear();
    }
    REQUIRE(retained);
    REQUIRE(retained->resources.size() == 1);
    CHECK(retained->resources[0].path == path("triangle.bin"));
    CHECK(retained->resources[0].asset == asset(1));
    CHECK(retained->resources[0].digest.to_hex() ==
          "d8b87038ed2ace74ed183166f6b0a541eab77f7184d50e65edc4bca5ccb5445a");
}

TEST_CASE("glTF inspection has no shared mutable parser state across concurrent callers",
          "[gltf-source]") {
    const auto data = fixture("embedded.gltf");
    const auto input_path = path("model.gltf");
    std::array<std::optional<GltfSourceInspection>, 2> results;
    std::array workers{std::thread{[&] {
                           auto result = inspect_gltf_source({input_path, bytes(data), {}, {}});
                           if (result)
                               results[0] = std::move(result).value();
                       }},
                       std::thread{[&] {
                           auto result = inspect_gltf_source({input_path, bytes(data), {}, {}});
                           if (result)
                               results[1] = std::move(result).value();
                       }}};
    for (auto& worker : workers)
        worker.join();
    REQUIRE(results[0]);
    REQUIRE(results[1]);
    REQUIRE(results[0]->resources.size() == 1);
    REQUIRE(results[1]->resources.size() == 1);
    CHECK(results[0]->resources[0].digest == results[1]->resources[0].digest);
    CHECK(results[0]->counts.accessors == results[1]->counts.accessors);
}

TEST_CASE("glTF every private parser allocation can fail without leaking or escaping Result",
          "[gltf-source]") {
    AllocationReset restore;
    const auto source = fixture("embedded.gltf");
    detail::gltf_fail_allocation(std::nullopt);
    REQUIRE(inspect(source));
    const auto allocations = detail::gltf_allocation_attempts();
    REQUIRE(allocations > 0);
    REQUIRE(allocations < 512);
    CHECK(detail::gltf_live_allocations() == 0);
    for (std::size_t index = 0; index < allocations; ++index) {
        INFO(index);
        detail::gltf_fail_allocation(index);
        rejected(inspect(source));
        CHECK(detail::gltf_live_allocations() == 0);
    }
    detail::gltf_fail_allocation(std::nullopt);
    REQUIRE(inspect(source));
    CHECK(detail::gltf_live_allocations() == 0);
}
