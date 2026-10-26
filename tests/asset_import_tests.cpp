#include "shark/tools/asset_import.hpp"

#include "asset_cache_format.hpp"
#include "project_files_native.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <thread>

namespace {
using namespace shark;
using namespace shark::tools;
using namespace std::string_view_literals;
namespace fs = std::filesystem;
template <typename T> T take(Result<T> result) {
    INFO((result ? "" : result.error().message));
    INFO((result ? "" : result.error().context.path));
    REQUIRE(result);
    return std::move(result).value();
}
template <typename T> T id(unsigned value) {
    std::ostringstream out;
    out << "40000000-0000-4000-8000-" << std::hex << std::setw(12) << std::setfill('0') << value;
    return T{take(StableId::parse(out.str()))};
}
ProjectPath path(std::string_view text) {
    return take(ProjectPath::parse(text));
}
void write(const fs::path& target, std::string_view bytes) {
    std::ofstream stream{target, std::ios::binary | std::ios::trunc};
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    REQUIRE(stream.good());
}
std::string read(const fs::path& target) {
    std::ifstream stream{target, std::ios::binary};
    REQUIRE(stream.good());
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}
std::string body(const AssetArtifact& artifact) {
    REQUIRE(artifact.payload.size() >= 36);
    return {reinterpret_cast<const char*>(artifact.payload.data() + 36),
            artifact.payload.size() - 36};
}
class Counting final : public AssetImporter {
  public:
    ByteAssetImporter bytes;
    AssetImporterInfo descriptor = bytes.info();
    unsigned calls{0};
    bool failure{false};
    bool throws{false};
    bool malformed{false};
    bool oversized{false};
    std::vector<AssetId> order;
    std::function<void(const AssetImportInput&)> hook;
    const AssetImporterInfo& info() const noexcept override {
        return descriptor;
    }
    Result<std::string> normalize_settings(std::string_view text) const override {
        return bytes.normalize_settings(text);
    }
    Result<std::vector<std::byte>> import(const AssetImportInput& input) override {
        ++calls;
        order.push_back(input.asset);
        if (hook)
            hook(input);
        if (throws)
            throw std::runtime_error{"private callback failure"};
        if (failure)
            return Error{ErrorCode::execution_failed, "fixture import failed", {}};
        if (malformed)
            return std::vector<std::byte>{std::byte{1}};
        if (oversized)
            return std::vector<std::byte>(asset_import_maximum_payload_bytes + 1);
        return bytes.import(input);
    }
    Result<void> validate_payload(const AssetArtifact& artifact) const override {
        auto copy = artifact;
        copy.importer_version = 1;
        return bytes.validate_payload(copy);
    }
};
struct Fixture {
    fs::path root =
        fs::temp_directory_path() / ("shark-imports-" + take(generate_stable_id()).to_string());
    std::unique_ptr<ProjectFiles> files;
    std::unique_ptr<AssetCatalog> catalog;
    Counting importer;
    std::unique_ptr<AssetImports> cache;
    std::vector<ProjectPath> inventory;
    const AssetTargetProfile client{"test-x64", AssetAudience::client};
    Fixture() {
        REQUIRE(fs::create_directory(root));
        files = take(ProjectFiles::open(root));
        catalog = take(AssetCatalog::create(*files, id<ProjectId>(1)));
        reopen();
    }
    ~Fixture() {
        cache.reset();
        catalog.reset();
        files.reset();
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    void reopen() {
        cache.reset();
        const std::array<AssetImporter*, 1> importers{&importer};
        cache = take(AssetImports::open(*files, *catalog, id<ProjectId>(1), importers));
    }
    ProjectPath source(unsigned number) const {
        return path("asset" + std::to_string(number) + ".bin");
    }
    ProjectPath sidecar(unsigned number) const {
        return take(asset_sidecar_path(source(number)));
    }
    AssetMetadata metadata(unsigned number) const {
        return take(files->read_asset_metadata(sidecar(number))).source.document;
    }
    void set(unsigned number, AssetMetadata value) {
        const auto current = take(files->read_asset_metadata(sidecar(number)));
        take(files->write_asset_metadata(sidecar(number), value, current.revision));
    }
    void seed(unsigned number, std::string_view data = "hello",
              std::vector<AssetId> dependencies = {},
              AssetAudience audience = AssetAudience::shared) {
        write(root / source(number).text(), data);
        AssetMetadata meta{id<ProjectId>(1),
                           id<AssetId>(number),
                           "bytes",
                           "Shark authored offline fixture",
                           "MIT",
                           "shark.bytes",
                           audience,
                           "{}",
                           std::move(dependencies)};
        take(files->write_asset_metadata(sidecar(number), meta, {}));
        inventory.push_back(sidecar(number));
    }
    AssetCatalogRevision refresh() {
        return take(catalog->refresh(inventory, take(catalog->revision())));
    }
    AssetCacheRevision revision() const {
        return take(cache->revision());
    }
    PreparedAssetImport prepare(unsigned number, const AssetTargetProfile* target = nullptr) {
        return take(cache->prepare(id<AssetId>(number), target ? *target : client,
                                   take(catalog->revision()), revision()));
    }
    AssetImportSummary commit(unsigned number, const AssetTargetProfile* target = nullptr) {
        auto prepared = prepare(number, target);
        auto summary = take(prepared.summary());
        take(cache->publish(std::move(prepared), take(catalog->revision()), revision()));
        return summary;
    }
    AssetCacheStatus query(unsigned number, const AssetTargetProfile* target = nullptr) {
        return take(cache->query(id<AssetId>(number), target ? *target : client,
                                 take(catalog->revision()), revision()));
    }
    fs::path file() const {
        return root / std::string{asset_cache_filename};
    }
};
struct Fault {
    detail::FileTestControl previous;
    explicit Fault(detail::FileFault fault)
        : previous(detail::set_file_test_control({fault, {}, {}})) {}
    ~Fault() {
        static_cast<void>(detail::set_file_test_control(previous));
    }
};
template <typename T> void rejected(const Result<T>& result, ErrorCode code) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == code);
    CHECK_FALSE(result.error().context.operation.empty());
}
} // namespace

TEST_CASE(
    "Import preparation publishes nothing and a real byte payload survives native cache reopen",
    "[asset-import]") {
    Fixture f;
    f.seed(10, "hi\0there"sv);
    const auto catalog = f.refresh();
    const auto initial = f.revision();
    const auto list = take(f.cache->importers());
    REQUIRE(list.size() == 1);
    CHECK(list[0].id == "shark.bytes");
    auto prepared = f.prepare(10);
    const auto summary = take(prepared.summary());
    CHECK(summary.nodes == 1);
    CHECK(summary.imported == 1);
    CHECK(summary.reused == 0);
    CHECK_FALSE(fs::exists(f.file()));
    CHECK(f.query(10).state == AssetCacheState::missing);
    auto committed = take(f.cache->publish(std::move(prepared), catalog, initial));
    CHECK(committed.changed);
    REQUIRE(committed.file);
    CHECK(committed.revision.value == initial.value + 1);
    CHECK_FALSE(prepared.summary());
    const auto artifact = take(f.cache->read(summary.root_key, f.revision()));
    CHECK(body(artifact) == "hi\0there"sv);
    CHECK(artifact.payload.size() == 44);
    CHECK(std::string(reinterpret_cast<const char*>(artifact.payload.data()), 8) == "SHKBYT01");
    CHECK(artifact.payload[8] == std::byte{1});
    CHECK(std::equal(artifact.asset.value().bytes().begin(), artifact.asset.value().bytes().end(),
                     artifact.payload.begin() + 12));
    CHECK(take(sha256(artifact.payload)) == artifact.payload_digest);
    const auto stored = read(f.file());
    f.reopen();
    CHECK(take(f.cache->read(summary.root_key, f.revision())) == artifact);
    CHECK(read(f.file()) == stored);
    CHECK(f.query(10).state == AssetCacheState::fresh);
}
TEST_CASE("Matching work reuses the existing key and no-op publication retains revision and bytes",
          "[asset-import]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    const auto first = f.commit(10);
    const auto revision = f.revision();
    const auto stored = read(f.file());
    auto prepared = f.prepare(10);
    const auto summary = take(prepared.summary());
    CHECK(summary.imported == 0);
    CHECK(summary.reused == 1);
    CHECK(summary.root_key == first.root_key);
    CHECK(f.importer.calls == 1);
    const auto committed =
        take(f.cache->publish(std::move(prepared), take(f.catalog->revision()), revision));
    CHECK_FALSE(committed.changed);
    CHECK_FALSE(committed.file);
    CHECK(committed.revision == revision);
    CHECK(read(f.file()) == stored);
}
TEST_CASE("Dependency changes invalidate their parents while unrelated bindings remain fresh",
          "[asset-import]") {
    Fixture f;
    f.seed(10, "leaf");
    f.seed(11, "parent", {id<AssetId>(10)});
    f.seed(12, "unrelated");
    f.refresh();
    const auto old = f.commit(11);
    const auto unrelated = f.commit(12);
    CHECK(f.importer.order ==
          std::vector<AssetId>{id<AssetId>(10), id<AssetId>(11), id<AssetId>(12)});
    write(f.root / f.source(10).text(), "changed");
    CHECK(f.query(11).state == AssetCacheState::stale);
    CHECK(f.query(12).state == AssetCacheState::fresh);
    CHECK(f.query(12).last_valid_key == unrelated.root_key);
    CHECK(f.query(11).last_valid_key == old.root_key);
    const auto changed = f.commit(11);
    CHECK(changed.imported == 2);
    CHECK(changed.reused == 0);
    CHECK(changed.root_key != old.root_key);
    CHECK(body(take(f.cache->read(old.root_key, f.revision()))) == "parent");
}
TEST_CASE(
    "Canonical settings ignore formatting and explicit defaults but semantic settings invalidate",
    "[asset-import]") {
    Fixture f;
    f.seed(10, "hello!");
    f.refresh();
    const auto first = f.commit(10);
    auto meta = f.metadata(10);
    meta.settings_json = "{ \"transform\" : \"identity\" }";
    f.set(10, meta);
    f.refresh();
    CHECK(f.query(10).state == AssetCacheState::fresh);
    CHECK(f.commit(10).reused == 1);
    CHECK(f.importer.calls == 1);
    meta.settings_json = R"({"transform":"uppercase-ascii"})";
    f.set(10, meta);
    f.refresh();
    CHECK(f.query(10).state == AssetCacheState::stale);
    const auto upper = f.commit(10);
    CHECK(upper.root_key != first.root_key);
    CHECK(body(take(f.cache->read(upper.root_key, f.revision()))) == "HELLO!");
    meta.settings_json = R"({"transform":"unknown"})";
    f.set(10, meta);
    f.refresh();
    const auto blocked = f.query(10);
    CHECK(blocked.state == AssetCacheState::stale);
    REQUIRE(blocked.blocked_by);
    CHECK(blocked.last_valid_key == upper.root_key);
    CHECK_FALSE(
        f.cache->prepare(id<AssetId>(10), f.client, take(f.catalog->revision()), f.revision()));
}
TEST_CASE(
    "Source and sidecar moves preserve cache identity and provenance is not an importer input",
    "[asset-import]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    const auto first = f.commit(10);
    auto meta = f.metadata(10);
    meta.origin = "Changed provenance";
    meta.license = "CC0-1.0";
    meta.extras_json = R"({"note":2})";
    f.set(10, meta);
    f.refresh();
    CHECK(f.query(10).required_key == first.root_key);
    REQUIRE(fs::create_directory(f.root / "moved"));
    fs::rename(f.root / f.source(10).text(), f.root / "moved/renamed.bin");
    fs::rename(f.root / f.sidecar(10).text(), f.root / "moved/renamed.bin.sharkasset");
    f.inventory = {path("moved/renamed.bin.sharkasset")};
    f.refresh();
    const auto prepared = f.prepare(10);
    CHECK(take(prepared.summary()).root_key == first.root_key);
    CHECK(take(prepared.summary()).reused == 1);
    CHECK(f.query(10).state == AssetCacheState::fresh);
}
TEST_CASE("Target profile and declared importer versions are independent key inputs",
          "[asset-import]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    const auto first = f.commit(10);
    const AssetTargetProfile other{"other-x64", AssetAudience::client};
    const AssetTargetProfile server{"test-x64", AssetAudience::server};
    CHECK(f.commit(10, &other).root_key != first.root_key);
    CHECK(f.commit(10, &server).root_key != first.root_key);
    f.importer.descriptor.version = 2;
    rejected(f.cache->prepare(id<AssetId>(10), f.client, take(f.catalog->revision()), f.revision()),
             ErrorCode::conflict);
    f.reopen();
    CHECK(f.query(10).state == AssetCacheState::stale);
    const auto version = f.commit(10);
    CHECK(version.root_key != first.root_key);
}
TEST_CASE("Missing sources, unknown dependencies, cycles and unsupported importers preserve last "
          "valid bytes",
          "[asset-import]") {
    Fixture f;
    f.seed(10);
    f.seed(11, "parent", {id<AssetId>(10)});
    f.refresh();
    const auto first = f.commit(11);
    const auto original = read(f.file());
    REQUIRE(fs::remove(f.root / f.source(10).text()));
    auto status = f.query(11);
    REQUIRE(status.blocked_by);
    CHECK(status.last_valid_key == first.root_key);
    CHECK_FALSE(
        f.cache->prepare(id<AssetId>(11), f.client, take(f.catalog->revision()), f.revision()));
    CHECK(read(f.file()) == original);
    write(f.root / f.source(10).text(), "hello");
    auto meta = f.metadata(10);
    meta.dependencies = {id<AssetId>(11)};
    f.set(10, meta);
    f.refresh();
    status = f.query(11);
    REQUIRE(status.blocked_by);
    CHECK(status.blocked_by->code == ErrorCode::invalid_configuration);
    meta.dependencies = {id<AssetId>(99)};
    f.set(10, meta);
    f.refresh();
    status = f.query(11);
    REQUIRE(status.blocked_by);
    CHECK(status.blocked_by->code == ErrorCode::invalid_id);
    meta.dependencies.clear();
    meta.importer = "unknown.importer";
    f.set(10, meta);
    f.refresh();
    status = f.query(11);
    REQUIRE(status.blocked_by);
    CHECK(status.blocked_by->code == ErrorCode::capability_unavailable);
    CHECK(read(f.file()) == original);
    CHECK(body(take(f.cache->read(first.root_key, f.revision()))) == "parent");
}
TEST_CASE("Client and server roots reject private cross-side and shared-to-private dependencies",
          "[asset-import]") {
    Fixture f;
    f.seed(10, "private", {}, AssetAudience::server);
    f.seed(11, "client", {id<AssetId>(10)}, AssetAudience::client);
    f.refresh();
    const AssetTargetProfile server{"server-x64", AssetAudience::server};
    CHECK_FALSE(
        f.cache->prepare(id<AssetId>(10), f.client, take(f.catalog->revision()), f.revision()));
    CHECK_FALSE(
        f.cache->prepare(id<AssetId>(11), f.client, take(f.catalog->revision()), f.revision()));
    CHECK_FALSE(
        f.cache->prepare(id<AssetId>(11), server, take(f.catalog->revision()), f.revision()));
    CHECK(f.commit(10, &server).imported == 1);
    auto meta = f.metadata(11);
    meta.audience = AssetAudience::shared;
    f.set(11, meta);
    f.refresh();
    CHECK_FALSE(
        f.cache->prepare(id<AssetId>(11), server, take(f.catalog->revision()), f.revision()));
}
TEST_CASE("Late source, metadata, catalog and cache changes fence prepared publication",
          "[asset-import]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    const auto first = f.commit(10);
    const auto original = read(f.file());
    auto prepared = f.prepare(10);
    write(f.root / f.source(10).text(), "late");
    rejected(f.cache->publish(std::move(prepared), take(f.catalog->revision()), f.revision()),
             ErrorCode::conflict);
    CHECK(read(f.file()) == original);
    prepared = f.prepare(10);
    auto meta = f.metadata(10);
    meta.settings_json = R"({"transform":"uppercase-ascii"})";
    f.set(10, meta);
    rejected(f.cache->publish(std::move(prepared), take(f.catalog->revision()), f.revision()),
             ErrorCode::conflict);
    CHECK(read(f.file()) == original);
    f.refresh();
    prepared = f.prepare(10);
    const auto old_catalog = take(f.catalog->revision());
    meta.origin = "new";
    f.set(10, meta);
    f.refresh();
    rejected(f.cache->publish(std::move(prepared), old_catalog, f.revision()), ErrorCode::conflict);
    prepared = f.prepare(10);
    const auto old_cache = f.revision();
    take(f.cache->clear(old_cache));
    rejected(f.cache->publish(std::move(prepared), take(f.catalog->revision()), old_cache),
             ErrorCode::conflict);
    CHECK_FALSE(f.cache->read(first.root_key, f.revision()));
}
TEST_CASE("Importer callback failures and malformed or oversized output cannot publish",
          "[asset-import]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    f.commit(10);
    const auto original = read(f.file());
    write(f.root / f.source(10).text(), "new");
    for (unsigned mode = 0; mode < 4; ++mode) {
        f.importer.failure = mode == 0;
        f.importer.throws = mode == 1;
        f.importer.malformed = mode == 2;
        f.importer.oversized = mode == 3;
        CHECK_FALSE(
            f.cache->prepare(id<AssetId>(10), f.client, take(f.catalog->revision()), f.revision()));
        CHECK(read(f.file()) == original);
    }
}
TEST_CASE("Source changes made during import are rejected at publication", "[asset-import]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    f.commit(10);
    const auto original = read(f.file());
    write(f.root / f.source(10).text(), "new");
    f.importer.hook = [&](const AssetImportInput&) {
        write(f.root / f.source(10).text(), "changed during callback");
    };
    auto prepared = f.prepare(10);
    rejected(f.cache->publish(std::move(prepared), take(f.catalog->revision()), f.revision()),
             ErrorCode::conflict);
    CHECK(read(f.file()) == original);
}
TEST_CASE("Native cache publication faults preserve old snapshot and clean failed cache stages",
          "[asset-import]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    f.commit(10);
    const auto original = read(f.file());
    write(f.root / f.source(10).text(), "changed");
    for (auto fault : {detail::FileFault::stage_create, detail::FileFault::stage_write,
                       detail::FileFault::stage_flush, detail::FileFault::publish}) {
        auto prepared = f.prepare(10);
        Fault control{fault};
        CHECK_FALSE(
            f.cache->publish(std::move(prepared), take(f.catalog->revision()), f.revision()));
        CHECK(read(f.file()) == original);
        for (const auto& e : fs::directory_iterator(f.root))
            CHECK_FALSE(e.path().filename().string().starts_with(".shark-cache-stage-"));
    }
    auto prepared = f.prepare(10);
    {
        Fault control{detail::FileFault::directory_flush};
        const auto commit =
            take(f.cache->publish(std::move(prepared), take(f.catalog->revision()), f.revision()));
        REQUIRE(commit.file);
        CHECK(commit.file->durability == FileDurability::directory_flush_failed);
    }
    CHECK(read(f.file()) != original);
    CHECK(f.query(10).state == AssetCacheState::fresh);
}
TEST_CASE(
    "Prune retains dependency versions used by stale parents and clear preserves authored data",
    "[asset-import]") {
    Fixture f;
    f.seed(10, "leaf");
    f.seed(11, "parent", {id<AssetId>(10)});
    f.refresh();
    const auto parent = f.commit(11);
    const auto old_leaf = take(f.cache->read(parent.root_key, f.revision())).dependencies[0].key;
    write(f.root / f.source(10).text(), "leaf2");
    f.commit(10);
    take(f.cache->prune(f.revision()));
    CHECK(f.cache->read(old_leaf, f.revision()));
    CHECK(f.query(11).state == AssetCacheState::stale);
    f.commit(11);
    take(f.cache->prune(f.revision()));
    CHECK_FALSE(f.cache->read(parent.root_key, f.revision()));
    CHECK_FALSE(f.cache->read(old_leaf, f.revision()));
    const auto source = read(f.root / f.source(10).text());
    const auto metadata = read(f.root / f.sidecar(10).text());
    take(f.cache->clear(f.revision()));
    CHECK(take(f.cache->usage(f.revision())).artifacts == 0);
    CHECK(read(f.root / f.source(10).text()) == source);
    CHECK(read(f.root / f.sidecar(10).text()) == metadata);
}
TEST_CASE(
    "Cache corruption, external edits and foreign project snapshots reject without source changes",
    "[asset-import]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    const auto first = f.commit(10);
    const auto original = read(f.file());
    auto corrupted = original;
    corrupted.back() ^= 1;
    write(f.file(), corrupted);
    rejected(f.cache->read(first.root_key, f.revision()), ErrorCode::conflict);
    CHECK_FALSE(f.cache->reload(f.revision()));
    const std::array<AssetImporter*, 1> importers{&f.importer};
    CHECK_FALSE(AssetImports::open(*f.files, *f.catalog, id<ProjectId>(1), importers));
    write(f.file(), original);
    CHECK(f.query(10).state == AssetCacheState::fresh);
    CHECK_FALSE(AssetImports::open(*f.files, *f.catalog, id<ProjectId>(2), importers));
    write(f.file(), original.substr(0, 55));
    CHECK_FALSE(f.cache->reload(f.revision()));
    CHECK(read(f.root / f.source(10).text()) == "hello");
}
TEST_CASE("Stale cache owners require explicit reload and prepared imports cannot cross services",
          "[asset-import]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    const std::array<AssetImporter*, 1> importers{&f.importer};
    auto other = take(AssetImports::open(*f.files, *f.catalog, id<ProjectId>(1), importers));
    auto prepared = f.prepare(10);
    f.commit(10);
    rejected(other->query(id<AssetId>(10), f.client, take(f.catalog->revision()),
                          take(other->revision())),
             ErrorCode::conflict);
    const auto reloaded = take(other->reload(take(other->revision())));
    CHECK(take(other->query(id<AssetId>(10), f.client, take(f.catalog->revision()), reloaded))
              .state == AssetCacheState::fresh);
    rejected(other->publish(std::move(prepared), take(f.catalog->revision()), reloaded),
             ErrorCode::conflict);
}
TEST_CASE("Source, closure and cache entry limits reject without partial publication",
          "[asset-import]") {
    Fixture f;
    f.seed(10, std::string(asset_import_maximum_source_bytes, 'x'));
    f.refresh();
    const auto first = f.commit(10);
    const auto original = read(f.file());
    write(f.root / f.source(10).text(), std::string(asset_import_maximum_source_bytes + 1, 'x'));
    CHECK_FALSE(
        f.cache->prepare(id<AssetId>(10), f.client, take(f.catalog->revision()), f.revision()));
    CHECK(read(f.file()) == original);
    CHECK(f.cache->read(first.root_key, f.revision()));
    // Fixed graph cap: a 129-node chain fails before publication.
    for (unsigned n = 100; n < 229; ++n)
        f.seed(n, "small",
               n == 228 ? std::vector<AssetId>{} : std::vector<AssetId>{id<AssetId>(n + 1)});
    f.refresh();
    CHECK_FALSE(
        f.cache->prepare(id<AssetId>(100), f.client, take(f.catalog->revision()), f.revision()));
    CHECK(read(f.file()) == original);
    take(f.cache->clear(f.revision()));
    auto exact = f.prepare(101);
    CHECK(take(exact.summary()).nodes == asset_import_maximum_nodes);
    take(f.cache->publish(std::move(exact), take(f.catalog->revision()), f.revision()));
    f.reopen();
    CHECK(take(f.cache->usage(f.revision())).artifacts == asset_import_maximum_nodes);
    take(f.cache->prune(f.revision()));
    CHECK(take(f.cache->usage(f.revision())).artifacts == asset_import_maximum_nodes);
}

TEST_CASE("Cache capacity requires explicit pruning and total source input is bounded",
          "[asset-import]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    for (unsigned i = 0; i < asset_cache_maximum_entries; ++i) {
        write(f.root / f.source(10).text(), std::to_string(i));
        f.commit(10);
    }
    CHECK(take(f.cache->usage(f.revision())).artifacts == asset_cache_maximum_entries);
    const auto before = read(f.file());
    write(f.root / f.source(10).text(), "over capacity");
    auto prepared = f.prepare(10);
    CHECK_FALSE(f.cache->publish(std::move(prepared), take(f.catalog->revision()), f.revision()));
    CHECK(read(f.file()) == before);
    take(f.cache->prune(f.revision()));
    CHECK(take(f.cache->usage(f.revision())).artifacts == 1);
    CHECK(f.commit(10).imported == 1);
    for (unsigned n = 100; n < 117; ++n)
        f.seed(n, std::string(asset_import_maximum_source_bytes, 'x'));
    std::vector<AssetId> dependencies;
    for (unsigned n = 100; n < 117; ++n)
        dependencies.push_back(id<AssetId>(n));
    f.seed(117, "root", dependencies);
    f.refresh();
    const auto calls = f.importer.calls;
    CHECK_FALSE(
        f.cache->prepare(id<AssetId>(117), f.client, take(f.catalog->revision()), f.revision()));
    CHECK(f.importer.calls == calls);
    // Exactly 16 MiB of source is admitted; the per-artifact header makes its
    // payload closure exceed 16 MiB. This exercises the independent output cap.
    for (unsigned n = 100; n < 116; ++n) {
        auto metadata = f.metadata(n);
        metadata.dependencies =
            n == 100 ? std::vector<AssetId>{} : std::vector<AssetId>{id<AssetId>(n - 1)};
        f.set(n, std::move(metadata));
    }
    f.refresh();
    const auto snapshot = read(f.file());
    CHECK_FALSE(
        f.cache->prepare(id<AssetId>(115), f.client, take(f.catalog->revision()), f.revision()));
    CHECK(f.importer.calls == calls + 16);
    CHECK(read(f.file()) == snapshot);
}

TEST_CASE(
    "Cache range validation rejects forged contiguous lengths even with a valid container digest",
    "[asset-import]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    f.commit(10);
    const auto original = read(f.file());
    std::size_t index_size = 0;
    for (unsigned i = 0; i < 4; ++i)
        index_size |= static_cast<std::size_t>(static_cast<unsigned char>(original[12 + i]))
                      << (i * 8U);
    auto index =
        take(detail::decode_cache_index(std::string_view{original}.substr(56, index_size)));
    index.entries[0].offset = 1;
    const auto json = take(detail::encode_cache_index(index));
    const auto payload = original.substr(56 + index_size);
    const auto contents = json + payload;
    const auto digest = take(detail::hash_text(contents));
    std::string forged = original.substr(0, 8);
    auto append = [&](std::uint64_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i)
            forged.push_back(static_cast<char>((value >> (i * 8U)) & 255U));
    };
    append(1, 4);
    append(json.size(), 4);
    append(payload.size(), 8);
    forged.append(reinterpret_cast<const char*>(digest.bytes.data()), digest.bytes.size());
    forged += contents;
    CHECK_FALSE(detail::decode_cache(forged, id<ProjectId>(1)));
    CHECK_FALSE(detail::decode_cache_index(
        R"({"format":"shark.development-cache","schema_version":1,"schema_version":1,"project_id":"40000000-0000-4000-8000-000000000001","entries":[],"bindings":[]})"));
}
TEST_CASE("Cache serialization rejects independent header, range, key and dependency corruption",
          "[asset-import]") {
    Fixture f;
    f.seed(10);
    f.seed(11, "parent", {id<AssetId>(10)});
    f.refresh();
    const auto parent = f.commit(11);
    auto state = take(detail::decode_cache(read(f.file()), id<ProjectId>(1)));
    CHECK(take(detail::encode_cache(state)) == read(f.file()));
    auto invalid = state;
    invalid.artifacts[0].key.bytes[0] ^= std::byte{1};
    CHECK_FALSE(detail::encode_cache(invalid));
    invalid = state;
    invalid.artifacts[0].payload[0] ^= std::byte{1};
    CHECK_FALSE(detail::encode_cache(invalid));
    invalid = state;
    invalid.bindings[0].key = {};
    CHECK_FALSE(detail::encode_cache(invalid));
    invalid = state;
    invalid.artifacts.push_back(invalid.artifacts[0]);
    CHECK_FALSE(detail::encode_cache(invalid));
    auto bytes = read(f.file());
    bytes[8] = 2;
    CHECK_FALSE(detail::decode_cache(bytes, id<ProjectId>(1)));
    bytes = read(f.file());
    bytes.push_back('x');
    CHECK_FALSE(detail::decode_cache(bytes, id<ProjectId>(1)));
    CHECK(take(f.cache->read(parent.root_key, f.revision())).dependencies.size() == 1);
}
TEST_CASE(
    "Reproducible payloads and cache bytes do not depend on preparation order or editing session",
    "[asset-import]") {
    Fixture f;
    f.seed(10);
    f.seed(11, "parent", {id<AssetId>(10)});
    f.refresh();
    const auto first = f.commit(11);
    const auto original = read(f.file());
    const auto artifact = take(f.cache->read(first.root_key, f.revision()));
    take(f.cache->clear(f.revision()));
    f.reopen();
    const auto second = f.commit(11);
    CHECK(second.root_key == first.root_key);
    CHECK(take(f.cache->read(second.root_key, f.revision())) == artifact);
    CHECK(read(f.file()) == original);
}
TEST_CASE("Reserved cache stages bound disk growth and recovery never removes source-save stages",
          "[asset-import]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    f.commit(10);
    const auto original = read(f.file());
    const auto stage = ".shark-cache-stage-" + take(generate_stable_id()).to_string();
    write(f.root / stage, "partial");
    const auto source_stage = ".shark-stage-" + take(generate_stable_id()).to_string();
    write(f.root / source_stage, "source evidence");
    CHECK(take(f.cache->usage(f.revision())).unpublished_stage);
    write(f.root / f.source(10).text(), "new");
    auto prepared = f.prepare(10);
    rejected(f.cache->publish(std::move(prepared), take(f.catalog->revision()), f.revision()),
             ErrorCode::conflict);
    CHECK(read(f.file()) == original);
    CHECK(take(AssetImports::recover(*f.files)) == 1);
    CHECK_FALSE(fs::exists(f.root / stage));
    CHECK(read(f.root / source_stage) == "source evidence");
    CHECK(take(AssetImports::recover(*f.files)) == 0);
    CHECK(f.commit(10).imported == 1);
    write(f.root / stage, "one");
    const auto second = ".shark-cache-stage-" + take(generate_stable_id()).to_string();
    write(f.root / second, "two");
    CHECK_FALSE(AssetImports::recover(*f.files));
    CHECK(read(f.root / stage) == "one");
    CHECK(read(f.root / second) == "two");
    fs::remove(f.root / second);
    CHECK(take(AssetImports::recover(*f.files)) == 1);
    write(f.root / ".shark-cache-stage-invalid", "data");
    CHECK_FALSE(AssetImports::recover(*f.files));
    CHECK(fs::exists(f.root / ".shark-cache-stage-invalid"));
}
TEST_CASE(
    "Asset import services reject invalid registry, target, reserved paths and non-owner access",
    "[asset-import]") {
    Fixture f;
    f.seed(10);
    f.refresh();
    const std::array<AssetImporter*, 2> duplicate{&f.importer, &f.importer};
    CHECK_FALSE(AssetImports::open(*f.files, *f.catalog, id<ProjectId>(1), duplicate));
    const std::array<AssetImporter*, 1> null{nullptr};
    CHECK_FALSE(AssetImports::open(*f.files, *f.catalog, id<ProjectId>(1), null));
    const std::array<AssetImporter*, 17> over_registry{};
    CHECK_FALSE(AssetImports::open(*f.files, *f.catalog, id<ProjectId>(1), over_registry));
    const AssetTargetProfile invalid{"../escape", AssetAudience::client};
    CHECK_FALSE(
        f.cache->prepare(id<AssetId>(10), invalid, take(f.catalog->revision()), f.revision()));
    CHECK_FALSE(asset_sidecar_path(path(asset_cache_filename)));
    const auto revision = f.revision();
    std::array<std::optional<Error>, 3> errors;
    std::thread other{[&] {
        auto a = f.cache->revision();
        if (!a)
            errors[0] = a.error();
        auto b = f.cache->usage(revision);
        if (!b)
            errors[1] = b.error();
        auto c = AssetImports::recover(*f.files);
        if (!c)
            errors[2] = c.error();
    }};
    other.join();
    for (const auto& error : errors) {
        REQUIRE(error);
        CHECK(error->code == ErrorCode::conflict);
    }
}
