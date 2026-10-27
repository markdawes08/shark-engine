// Original Shark-authored offline fixtures; MIT. The PNG bytes also appear in
// fixtures/gltf_textures/gamma-alpha.png and its stdlib generator.
#include "shark/tools/gltf_asset.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>

namespace {
using namespace shark;
using namespace shark::tools;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr auto deadline = std::chrono::seconds{15};

template <typename T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().message);
        INFO(result.error().context.path);
    }
    REQUIRE(result);
    return std::move(result).value();
}
template <typename T> T id(unsigned number) {
    std::ostringstream text;
    text << "12d00000-0000-4000-8000-" << std::hex << std::setw(12) << std::setfill('0') << number;
    return T{take(StableId::parse(text.str()))};
}
ProjectPath path(std::string_view text) {
    return take(ProjectPath::parse(text));
}
void write(const fs::path& target, std::string_view data) {
    std::ofstream file{target, std::ios::binary | std::ios::trunc};
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    file.close();
    REQUIRE(file.good());
}
std::string read(const fs::path& target) {
    std::ifstream file{target, std::ios::binary};
    REQUIRE(file.good());
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}
std::string from_hex(std::string_view text) {
    REQUIRE(text.size() % 2 == 0);
    const auto digit = [](char c) {
        return static_cast<unsigned>(c >= 'a' ? c - 'a' + 10 : c - '0');
    };
    std::string result;
    for (std::size_t i = 0; i < text.size(); i += 2)
        result += static_cast<char>((digit(text[i]) << 4U) | digit(text[i + 1]));
    return result;
}
std::string triangle(float x = 1.0F) {
    const std::array<float, 15> values{0, 0, 0, x, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1};
    std::string result;
    for (const float value : values) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned shift = 0; shift < 32; shift += 8)
            result += static_cast<char>((bits >> shift) & 255U);
    }
    return result;
}
std::string image() {
    return from_hex("89504e470d0a1a0a0000000d494844520000000200000002080600000072b60d24"
                    "000000134944415478da63600082ff4000a21d80740300389b07ba38201f190000"
                    "000049454e44ae426082");
}
std::string model(bool embedded = false) {
    const std::string buffer =
        embedded
            ? "data:application/octet-stream;base64,"
              "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/"
            : "triangle.bin";
    const std::string texture = embedded ? "data:image/png;base64,"
                                           "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAE0lEQVR"
                                           "42mNgAIL/QACiHYB0AwA4mwe6OCAfGQAAAABJRU5ErkJggg=="
                                         : "../textures/gamma.png";
    return R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":60,"uri":")" + buffer +
           R"("}],"bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"}],"images":[{"uri":")" +
           texture +
           R"("}],"textures":[{"source":0}],"materials":[{"name":"paint","pbrMetallicRoughness":{"baseColorFactor":[0.25,0.5,0.75,0.5],"metallicFactor":0.25,"roughnessFactor":0.75,"baseColorTexture":{"index":0},"metallicRoughnessTexture":{"index":0}},"normalTexture":{"index":0,"scale":0.5},"occlusionTexture":{"index":0,"strength":0.25},"emissiveTexture":{"index":0},"emissiveFactor":[0.25,0.5,1],"alphaMode":"MASK","alphaCutoff":0.25,"doubleSided":true}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"material":0}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]},{"nodes":[]}],"scene":0})";
}
std::string settings(unsigned image_id = 11, std::optional<unsigned> scene = {}) {
    std::string value =
        R"({"path":"models/model.gltf","resources":[{"path":"models/triangle.bin","asset":")" +
        id<AssetId>(10).value().to_string() + R"("},{"path":"textures/gamma.png","asset":")" +
        id<AssetId>(image_id).value().to_string() + R"("}])";
    if (scene)
        value += ",\"scene\":" + std::to_string(*scene);
    return value + '}';
}
bool terminal(AssetImportJobState state) {
    return state == AssetImportJobState::succeeded || state == AssetImportJobState::failed ||
           state == AssetImportJobState::cancelled;
}

// The gate establishes an exact captured-input/worker/owner ordering. Deadlines
// fail a broken fixture; elapsed time never selects a successful interleaving.
class GatedGltf final : public AssetImporter {
  public:
    GltfAssetImporter gltf;
    std::atomic<unsigned> calls{0};
    std::atomic<bool> on_worker{false};
    const AssetImporterInfo& info() const noexcept override {
        return gltf.info();
    }
    Result<std::string> normalize_settings(std::string_view text) const override {
        return gltf.normalize_settings(text);
    }
    Result<void> validate_payload(const AssetArtifact& artifact) const override {
        return gltf.validate_payload(artifact);
    }
    Result<std::vector<std::byte>> import(const AssetImportInput& input) override {
        on_worker = std::this_thread::get_id() != owner_;
        {
            std::unique_lock lock{mutex_};
            ++calls;
            entered_.notify_all();
            if (blocked_ && !released_ &&
                !release_.wait_until(lock, Clock::now() + deadline, [&] { return released_; }))
                return Error{ErrorCode::execution_failed, "glTF fixture gate timed out", {}};
        }
        return gltf.import(input);
    }
    void block() {
        const std::lock_guard lock{mutex_};
        blocked_ = true;
        released_ = false;
    }
    void release() noexcept {
        {
            const std::lock_guard lock{mutex_};
            released_ = true;
        }
        release_.notify_all();
    }
    bool wait_calls(unsigned count) {
        std::unique_lock lock{mutex_};
        return entered_.wait_until(lock, Clock::now() + deadline,
                                   [&] { return calls.load() >= count; });
    }

  private:
    const std::thread::id owner_ = std::this_thread::get_id();
    std::mutex mutex_;
    std::condition_variable entered_;
    std::condition_variable release_;
    bool blocked_{};
    bool released_{};
};
struct Fixture {
    fs::path root =
        fs::temp_directory_path() / ("shark-gltf-import-" + take(generate_stable_id()).to_string());
    GatedGltf gltf;
    GltfResourceImporter resource;
    ByteAssetImporter bytes;
    std::unique_ptr<ProjectFiles> files;
    std::unique_ptr<AssetCatalog> catalog;
    std::unique_ptr<AssetImports> imports;
    std::vector<ProjectPath> inventory;
    const AssetTargetProfile target{"gltf-test-x64", AssetAudience::client};

    Fixture() {
        REQUIRE(fs::create_directory(root));
        files = take(ProjectFiles::open(root));
        catalog = take(AssetCatalog::create(*files, id<ProjectId>(1)));
        reopen();
    }
    ~Fixture() {
        gltf.release();
        imports.reset();
        catalog.reset();
        files.reset();
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    void reopen() {
        imports.reset();
        const std::array<AssetImporter*, 3> registry{&gltf, &resource, &bytes};
        imports = take(AssetImports::open(*files, *catalog, id<ProjectId>(1), registry));
    }
    ProjectPath source(unsigned number) const {
        return path("source" + std::to_string(number) + (number == 20 ? ".gltf" : ".bin"));
    }
    ProjectPath sidecar(unsigned number) const {
        return take(asset_sidecar_path(source(number)));
    }
    void seed(unsigned number, std::string_view data, std::string kind, std::string importer,
              std::string configuration = "{}", std::vector<AssetId> dependencies = {},
              AssetAudience audience = AssetAudience::shared) {
        write(root / source(number).text(), data);
        const AssetMetadata metadata{id<ProjectId>(1),
                                     id<AssetId>(number),
                                     std::move(kind),
                                     "Original Shark offline integration fixture",
                                     "MIT",
                                     std::move(importer),
                                     audience,
                                     std::move(configuration),
                                     std::move(dependencies)};
        take(files->write_asset_metadata(sidecar(number), metadata, {}));
        inventory.push_back(sidecar(number));
    }
    void seed_external() {
        seed(10, triangle(), "gltf-resource", "shark.gltf-resource");
        seed(11, image(), "gltf-resource", "shark.gltf-resource");
        seed(20, model(), "gltf", "shark.gltf", settings(), {id<AssetId>(10), id<AssetId>(11)});
        refresh();
    }
    AssetMetadata metadata(unsigned number) const {
        return take(files->read_asset_metadata(sidecar(number))).source.document;
    }
    void set(unsigned number, AssetMetadata metadata) {
        const auto current = take(files->read_asset_metadata(sidecar(number)));
        take(files->write_asset_metadata(sidecar(number), metadata, current.revision));
    }
    AssetCatalogRevision refresh() {
        return take(catalog->refresh(inventory, take(catalog->revision())));
    }
    AssetCacheRevision revision() const {
        return take(imports->revision());
    }
    PreparedAssetImport prepare() {
        return take(
            imports->prepare(id<AssetId>(20), target, take(catalog->revision()), revision()));
    }
    AssetImportSummary commit() {
        auto prepared = prepare();
        auto summary = take(prepared.summary());
        take(imports->publish(std::move(prepared), take(catalog->revision()), revision()));
        return summary;
    }
    AssetCacheStatus query() const {
        return take(imports->query(id<AssetId>(20), target, take(catalog->revision()), revision()));
    }
    AssetArtifact artifact(AssetCacheKey key) const {
        return take(imports->read(key, revision()));
    }
    fs::path cache_file() const {
        return root / std::string{asset_cache_filename};
    }
    AssetImportJobAdmission submit(const AssetTargetProfile* selected = nullptr) {
        return take(imports->submit(id<AssetId>(20), selected ? *selected : target,
                                    take(catalog->revision()), revision()));
    }
    AssetImportJobStatus status(AssetImportJobId job) const {
        for (const auto& value : take(imports->jobs()))
            if (value.id == job)
                return value;
        FAIL("Expected a retained glTF import job");
    }
    AssetImportJobStatus wait_ready(AssetImportJobId job) const {
        const auto end = Clock::now() + deadline;
        for (;;) {
            auto value = status(job);
            if (value.state == AssetImportJobState::ready)
                return value;
            REQUIRE_FALSE(terminal(value.state));
            REQUIRE(Clock::now() < end);
            std::this_thread::yield();
        }
    }
    AssetImportJobStatus finish(AssetImportJobId job) {
        const auto end = Clock::now() + deadline;
        for (;;) {
            take(imports->drain_jobs());
            auto value = status(job);
            if (terminal(value.state))
                return value;
            REQUIRE(Clock::now() < end);
            std::this_thread::yield();
        }
    }
};
void check_asset(const GltfAsset& asset, float x = 1.0F) {
    REQUIRE(asset.geometry.primitives.size() == 1);
    REQUIRE(asset.geometry.meshes.size() == 1);
    REQUIRE(asset.geometry.nodes.size() == 1);
    const auto& primitive = asset.geometry.primitives[0];
    REQUIRE(primitive.vertices.size() == 3);
    CHECK(primitive.vertices[0].position == std::array<float, 3>{0, 0, 0});
    CHECK(primitive.vertices[1].position == std::array<float, 3>{x, 0, 0});
    CHECK(primitive.vertices[2].position == std::array<float, 3>{0, 1, 0});
    CHECK(primitive.indices == std::vector<std::uint32_t>{0, 1, 2});
    CHECK(primitive.source_material == 0);
    CHECK(asset.geometry.nodes[0].mesh == 0);
    REQUIRE(asset.materials.materials.size() == 1);
    const auto& material = asset.materials.materials[0];
    CHECK(material.source_material == 0);
    CHECK(material.name == "paint");
    CHECK(material.base_color == std::array<float, 4>{0.25F, 0.5F, 0.75F, 0.5F});
    CHECK(material.metallic == 0.25F);
    CHECK(material.roughness == 0.75F);
    CHECK(material.emissive == std::array<float, 3>{0.25F, 0.5F, 1.0F});
    CHECK(material.normal_scale == 0.5F);
    CHECK(material.occlusion_strength == 0.25F);
    CHECK(material.alpha_mode == GltfAlphaMode::mask);
    CHECK(material.alpha_cutoff == 0.25F);
    CHECK(material.double_sided);
    REQUIRE(asset.textures.images.size() == 3);
    REQUIRE(asset.textures.usages.size() == 5);
    const std::array<std::array<unsigned, 4>, 3> expected{
        std::array<unsigned, 4>{188, 188, 188, 112}, std::array<unsigned, 4>{128, 128, 128, 112},
        std::array<unsigned, 4>{128, 128, 255, 112}};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto& texture = asset.textures.images[i];
        REQUIRE(texture.mips.size() == 2);
        CHECK(texture.component_bits == 8);
        CHECK(texture.mips[0].width == 2);
        CHECK(texture.mips[0].height == 2);
        CHECK(texture.mips[1].width == 1);
        CHECK(texture.mips[1].height == 1);
        REQUIRE(texture.mips[1].rgba.size() == 4);
        for (std::size_t c = 0; c < 4; ++c)
            CHECK(std::to_integer<unsigned>(texture.mips[1].rgba[c]) == expected[i][c]);
    }
}
} // namespace

TEST_CASE("glTF imports typed external resources and reuses exact payloads after reopen",
          "[gltf-import]") {
    Fixture f;
    f.seed_external();
    const auto initial = f.revision();
    auto prepared = f.prepare();
    const auto summary = take(prepared.summary());
    CHECK(summary.nodes == 3);
    CHECK(summary.imported == 3);
    CHECK(summary.reused == 0);
    CHECK(f.query().state == AssetCacheState::missing);
    CHECK_FALSE(fs::exists(f.cache_file()));
    take(f.imports->publish(std::move(prepared), take(f.catalog->revision()), initial));
    const auto artifact = f.artifact(summary.root_key);
    CHECK(artifact.dependencies.size() == 2);
    CHECK(artifact.importer == "shark.gltf");
    CHECK(artifact.payload_version == 1);
    const auto decoded = take(decode_gltf_asset(artifact.payload));
    check_asset(decoded);
    REQUIRE(decoded.geometry.source.resources.size() == 2);
    CHECK(decoded.geometry.source.resources[0].asset == id<AssetId>(10));
    CHECK(decoded.geometry.source.resources[0].path == path("models/triangle.bin"));
    CHECK(decoded.geometry.source.resources[1].asset == id<AssetId>(11));
    CHECK(decoded.geometry.source.resources[1].path == path("textures/gamma.png"));
    const auto saved = read(f.cache_file());
    const auto reused = f.commit();
    CHECK(reused.root_key == summary.root_key);
    CHECK(reused.imported == 0);
    CHECK(reused.reused == 3);
    CHECK(f.gltf.calls == 1);
    CHECK(read(f.cache_file()) == saved);
    f.reopen();
    CHECK(f.query().state == AssetCacheState::fresh);
    CHECK(f.artifact(summary.root_key) == artifact);
    check_asset(take(decode_gltf_asset(f.artifact(summary.root_key).payload)));
    CHECK(f.commit().reused == 3);
}

TEST_CASE("glTF embedded images and geometry publish without invented dependency identities",
          "[gltf-import]") {
    Fixture f;
    f.seed(20, model(true), "gltf", "shark.gltf", R"({"path":"models/model.gltf"})");
    f.refresh();
    const auto summary = f.commit();
    CHECK(summary.nodes == 1);
    const auto artifact = f.artifact(summary.root_key);
    CHECK(artifact.dependencies.empty());
    check_asset(take(decode_gltf_asset(artifact.payload)));
}

TEST_CASE("glTF dependency edits invalidate the parent and retain the old complete artifact",
          "[gltf-import]") {
    Fixture f;
    f.seed_external();
    const auto old = f.commit();
    const auto old_payload = f.artifact(old.root_key).payload;
    write(f.root / f.source(10).text(), triangle(2.0F));
    const auto stale = f.query();
    CHECK(stale.state == AssetCacheState::stale);
    CHECK(stale.last_valid_key == old.root_key);
    const auto changed = f.commit();
    CHECK(changed.root_key != old.root_key);
    CHECK(changed.imported == 2);
    CHECK(changed.reused == 1);
    check_asset(take(decode_gltf_asset(f.artifact(changed.root_key).payload)), 2.0F);
    CHECK(f.artifact(old.root_key).payload == old_payload);
}

TEST_CASE("glTF settings and persistent resource IDs participate in root keys", "[gltf-import]") {
    Fixture f;
    f.seed_external();
    const auto first = f.commit();
    f.seed(12, image(), "gltf-resource", "shark.gltf-resource");
    auto metadata = f.metadata(20);
    metadata.settings_json = settings(12);
    metadata.dependencies = {id<AssetId>(10), id<AssetId>(12)};
    f.set(20, metadata);
    f.refresh();
    const auto replaced = f.commit();
    CHECK(replaced.root_key != first.root_key);
    const auto artifact = f.artifact(replaced.root_key);
    REQUIRE(artifact.dependencies.size() == 2);
    CHECK(artifact.dependencies[1].asset == id<AssetId>(12));
    check_asset(take(decode_gltf_asset(artifact.payload)));
    metadata.settings_json = settings(12, 1);
    f.set(20, metadata);
    f.refresh();
    const auto selected = f.commit();
    CHECK(selected.root_key != replaced.root_key);
    const auto empty = take(decode_gltf_asset(f.artifact(selected.root_key).payload));
    CHECK(empty.geometry.selected_scene == 1);
    CHECK(empty.geometry.nodes.empty());
    CHECK(empty.textures.images.empty());
    CHECK(empty.materials.materials.empty());
}

TEST_CASE("glTF logical URI aliases retain asset identity across coordinated physical moves",
          "[gltf-import]") {
    Fixture f;
    f.seed_external();
    const auto first = f.commit();
    const auto source = f.source(11);
    const auto sidecar = f.sidecar(11);
    const auto moved = path("moved-image.bin");
    const auto moved_sidecar = take(asset_sidecar_path(moved));
    fs::rename(f.root / source.text(), f.root / moved.text());
    fs::rename(f.root / sidecar.text(), f.root / moved_sidecar.text());
    for (auto& entry : f.inventory)
        if (entry == sidecar)
            entry = moved_sidecar;
    f.refresh();
    const auto repeated = f.commit();
    CHECK(repeated.root_key == first.root_key);
    CHECK(repeated.reused == 3);
    CHECK(repeated.imported == 0);
    check_asset(take(decode_gltf_asset(f.artifact(first.root_key).payload)));
}

TEST_CASE("glTF prepared publication rejects changed original sources", "[gltf-import]") {
    Fixture f;
    f.seed_external();
    const auto old = f.commit();
    write(f.root / f.source(10).text(), triangle(2.0F));
    auto candidate = f.prepare();
    const auto revision = f.revision();
    const auto saved = read(f.cache_file());
    write(f.root / f.source(10).text(), triangle(3.0F));
    const auto rejected =
        f.imports->publish(std::move(candidate), take(f.catalog->revision()), revision);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == ErrorCode::conflict);
    CHECK(f.revision() == revision);
    CHECK(read(f.cache_file()) == saved);
    CHECK(f.query().last_valid_key == old.root_key);
    check_asset(take(decode_gltf_asset(f.artifact(old.root_key).payload)));
    check_asset(take(decode_gltf_asset(f.artifact(f.commit().root_key).payload)), 3.0F);
}

TEST_CASE("glTF malformed replacement preserves last valid bytes through reopen", "[gltf-import]") {
    Fixture f;
    f.seed_external();
    const auto first = f.commit();
    const auto saved = read(f.cache_file());
    write(f.root / f.source(11).text(), "invalid image");
    REQUIRE_FALSE(
        f.imports->prepare(id<AssetId>(20), f.target, take(f.catalog->revision()), f.revision()));
    CHECK(read(f.cache_file()) == saved);
    f.reopen();
    CHECK(f.query().last_valid_key == first.root_key);
    check_asset(take(decode_gltf_asset(f.artifact(first.root_key).payload)));
    write(f.root / f.source(11).text(), image());
    CHECK(f.commit().root_key == first.root_key);
}

TEST_CASE("glTF client roots cannot import a Server private dependency", "[gltf-import]") {
    Fixture f;
    f.seed_external();
    auto private_image = f.metadata(11);
    private_image.audience = AssetAudience::server;
    f.set(11, private_image);
    auto root = f.metadata(20);
    root.audience = AssetAudience::client;
    f.set(20, root);
    f.refresh();
    const auto rejected =
        f.imports->prepare(id<AssetId>(20), f.target, take(f.catalog->revision()), f.revision());
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == ErrorCode::invalid_configuration);
    CHECK(f.gltf.calls == 0);
    CHECK_FALSE(fs::exists(f.cache_file()));
}

TEST_CASE("glTF shared roots reject side-private resources even on the matching target",
          "[gltf-import]") {
    Fixture f;
    f.seed_external();
    auto private_image = f.metadata(11);
    private_image.audience = AssetAudience::client;
    f.set(11, private_image);
    f.refresh();
    REQUIRE_FALSE(
        f.imports->prepare(id<AssetId>(20), f.target, take(f.catalog->revision()), f.revision()));
    CHECK(f.gltf.calls == 0);
    CHECK_FALSE(fs::exists(f.cache_file()));
}

TEST_CASE("glTF resources require typed identity output rather than transformed byte payloads",
          "[gltf-import]") {
    Fixture f;
    f.seed_external();
    const auto first = f.commit();
    const auto saved = read(f.cache_file());
    auto bytes = f.metadata(11);
    bytes.kind = "bytes";
    bytes.importer = "shark.bytes";
    bytes.settings_json = R"({"transform":"uppercase-ascii"})";
    f.set(11, bytes);
    f.refresh();
    REQUIRE_FALSE(
        f.imports->prepare(id<AssetId>(20), f.target, take(f.catalog->revision()), f.revision()));
    CHECK(read(f.cache_file()) == saved);
    CHECK(f.query().last_valid_key == first.root_key);
}

TEST_CASE("glTF background work publishes only at owner drain and deduplicates pending input",
          "[gltf-import]") {
    Fixture f;
    f.seed_external();
    const auto revision = f.revision();
    f.gltf.block();
    const auto admitted = f.submit();
    REQUIRE(f.gltf.wait_calls(1));
    const auto duplicate = f.submit();
    CHECK(duplicate.deduplicated);
    CHECK(duplicate.id == admitted.id);
    f.gltf.release();
    const auto ready = f.wait_ready(admitted.id);
    CHECK(ready.completed_nodes == 3);
    CHECK(ready.total_nodes == 3);
    CHECK_FALSE(ready.commit);
    CHECK(f.gltf.on_worker);
    CHECK(f.revision() == revision);
    CHECK_FALSE(fs::exists(f.cache_file()));
    const auto finished = f.finish(admitted.id);
    CHECK(finished.state == AssetImportJobState::succeeded);
    REQUIRE(finished.summary);
    REQUIRE(finished.commit);
    CHECK(finished.commit->changed);
    check_asset(take(decode_gltf_asset(f.artifact(finished.summary->root_key).payload)));
    const auto retained = take(f.imports->cancel_job(admitted.id));
    REQUIRE(retained.commit);
    CHECK(retained.state == AssetImportJobState::succeeded);
    CHECK(retained.commit->revision == finished.commit->revision);
    CHECK(take(f.imports->take_job(admitted.id)).state == AssetImportJobState::succeeded);
    CHECK(take(f.imports->jobs()).empty());
}

TEST_CASE("glTF background source changes reject stale completion and allow explicit retry",
          "[gltf-import]") {
    Fixture f;
    f.seed_external();
    const auto first = f.commit();
    const auto saved = read(f.cache_file());
    write(f.root / f.source(10).text(), triangle(2.0F));
    f.gltf.block();
    const auto job = f.submit().id;
    REQUIRE(f.gltf.wait_calls(2));
    write(f.root / f.source(10).text(), triangle(3.0F));
    f.gltf.release();
    const auto failed = f.finish(job);
    CHECK(failed.state == AssetImportJobState::failed);
    REQUIRE(failed.error);
    CHECK(failed.error->code == ErrorCode::conflict);
    CHECK_FALSE(failed.commit);
    CHECK(read(f.cache_file()) == saved);
    CHECK(f.query().last_valid_key == first.root_key);
    take(f.imports->take_job(job));
    const auto retry = f.submit().id;
    CHECK(retry != job);
    const auto completed = f.finish(retry);
    CHECK(completed.state == AssetImportJobState::succeeded);
    REQUIRE(completed.summary);
    check_asset(take(decode_gltf_asset(f.artifact(completed.summary->root_key).payload)), 3.0F);
}

TEST_CASE("glTF cancelled changed candidates retain original valid cache", "[gltf-import]") {
    Fixture f;
    f.seed_external();
    const auto first = f.commit();
    const auto revision = f.revision();
    const auto saved = read(f.cache_file());
    write(f.root / f.source(10).text(), triangle(2.0F));
    f.gltf.block();
    const auto job = f.submit().id;
    REQUIRE(f.gltf.wait_calls(2));
    take(f.imports->cancel_job(job));
    f.gltf.release();
    const auto cancelled = f.finish(job);
    CHECK(cancelled.state == AssetImportJobState::cancelled);
    CHECK_FALSE(cancelled.commit);
    CHECK(f.revision() == revision);
    CHECK(read(f.cache_file()) == saved);
    CHECK(f.query().last_valid_key == first.root_key);
    take(f.imports->take_job(job));
    check_asset(take(decode_gltf_asset(f.artifact(first.root_key).payload)));
}

TEST_CASE("glTF jobs retain the existing four-slot admission bound", "[gltf-import]") {
    Fixture f;
    f.seed_external();
    f.gltf.block();
    const auto first = f.submit().id;
    REQUIRE(f.gltf.wait_calls(1));
    std::vector<AssetImportJobId> queued;
    for (unsigned i = 1; i < 4; ++i) {
        const AssetTargetProfile target{"gltf-target-" + std::to_string(i), AssetAudience::client};
        queued.push_back(f.submit(&target).id);
    }
    const AssetTargetProfile overflow{"gltf-target-4", AssetAudience::client};
    const auto rejected =
        f.imports->submit(id<AssetId>(20), overflow, take(f.catalog->revision()), f.revision());
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == ErrorCode::conflict);
    CHECK(take(f.imports->jobs()).size() == asset_import_maximum_jobs);
    for (const auto job : queued)
        CHECK(take(f.imports->cancel_job(job)).state == AssetImportJobState::cancelled);
    take(f.imports->cancel_job(first));
    f.gltf.release();
    CHECK(f.finish(first).state == AssetImportJobState::cancelled);
    CHECK(f.gltf.calls == 1);
    CHECK_FALSE(fs::exists(f.cache_file()));
}

TEST_CASE("glTF oversized resources fail capture before an importer runs", "[gltf-import]") {
    Fixture f;
    f.seed_external();
    write(f.root / f.source(10).text(), std::string(asset_import_maximum_source_bytes + 1, 'x'));
    REQUIRE_FALSE(
        f.imports->submit(id<AssetId>(20), f.target, take(f.catalog->revision()), f.revision()));
    CHECK(f.gltf.calls == 0);
    CHECK(take(f.imports->jobs()).empty());
    CHECK_FALSE(fs::exists(f.cache_file()));
}
