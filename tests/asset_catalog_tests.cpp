#include "shark/tools/asset_catalog.hpp"

#include "project_files_native.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <thread>

namespace {
using namespace shark;
using namespace shark::tools;
namespace fs = std::filesystem;
fs::path native(std::string_view text) {
    return fs::path{std::u8string{reinterpret_cast<const char8_t*>(text.data()), text.size()}};
}
template <typename T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().message);
        INFO(result.error().context.path);
    }
    REQUIRE(result);
    return std::move(result).value();
}
template <typename T> T id(unsigned value) {
    std::ostringstream stream;
    stream << "30000000-0000-4000-8000-" << std::hex << std::setw(12) << std::setfill('0') << value;
    return T{take(StableId::parse(stream.str()))};
}
ProjectPath path(std::string_view text) {
    return take(ProjectPath::parse(text));
}
AssetMetadata metadata(unsigned value = 10) {
    return {id<ProjectId>(1),
            id<AssetId>(value),
            "fixture",
            "Shark authored offline fixture",
            "MIT",
            "shark.fixture",
            AssetAudience::shared,
            R"({"quality":2})",
            {},
            R"({"note":"retained"})"};
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
struct Fixture {
    fs::path root =
        fs::temp_directory_path() / ("shark-assets-" + take(generate_stable_id()).to_string());
    Fixture() {
        REQUIRE(fs::create_directory(root));
    }
    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    std::unique_ptr<ProjectFiles> files() const {
        return take(ProjectFiles::open(root));
    }
    void seed(std::string_view source = "one.txt", unsigned value = 10) const {
        const auto source_path = path(source);
        const auto source_file = root / native(source);
        fs::create_directories(source_file.parent_path());
        write(source_file, "Shark asset fixture\n");
        write(root / native(take(asset_sidecar_path(source_path)).text()),
              take(encode_asset_metadata(metadata(value))));
    }
};
struct Control {
    detail::FileTestControl previous;
    explicit Control(detail::FileFault fault)
        : previous(detail::set_file_test_control({fault, {}, {}})) {}
    ~Control() {
        static_cast<void>(detail::set_file_test_control(previous));
    }
};
template <typename T> void rejected(const Result<T>& result, ErrorCode code) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == code);
    CHECK_FALSE(result.error().context.operation.empty());
}
} // namespace

TEST_CASE("Asset metadata has canonical independent identity, provenance and dependency values",
          "[assets]") {
    auto value = metadata();
    value.dependencies = {id<AssetId>(12), id<AssetId>(11)};
    value.settings_json = R"({ "z": 2, "a": {"x":true} })";
    const auto canonical = take(validate_asset_metadata(value));
    REQUIRE(canonical.dependencies == std::vector<AssetId>{id<AssetId>(11), id<AssetId>(12)});
    CHECK(canonical.settings_json == R"({"a":{"x":true},"z":2})");
    const auto text = take(encode_asset_metadata(value));
    CHECK(text.back() == '\n');
    CHECK(take(decode_asset_metadata(text)) == canonical);
    CHECK(take(encode_asset_metadata(canonical)) == text);
    value.dependencies = {id<AssetId>(11), id<AssetId>(12)};
    CHECK(take(encode_asset_metadata(value)) == text);
    CHECK(canonical.asset == id<AssetId>(10));
    CHECK(canonical.origin == value.origin);
    CHECK(canonical.license == "MIT");
    for (auto audience : {AssetAudience::client, AssetAudience::server, AssetAudience::shared}) {
        value.audience = audience;
        CHECK(take(decode_asset_metadata(take(encode_asset_metadata(value)))).audience == audience);
    }
}

TEST_CASE("Asset metadata rejects unsupported envelopes, unknown fields and duplicate decoded keys",
          "[assets]") {
    const auto good = take(encode_asset_metadata(metadata()));
    for (const auto& [from, to] : std::array<std::pair<std::string, std::string>, 7>{
             {{"\"schema_version\": 1", "\"schema_version\": 0"},
              {"\"schema_version\": 1", "\"schema_version\": 2"},
              {"\"schema_version\": 1", "\"schema_version\": 1.0"},
              {"shark.asset", "shark.scene"},
              {"\"shared\"", "\"public\""},
              {"\"kind\"", "\"unsupported\""},
              {"\"settings\":", "\"settings\":{},\"settings\":"}}}) {
        auto text = good;
        const auto where = text.find(from);
        REQUIRE(where != std::string::npos);
        text.replace(where, from.size(), to);
        REQUIRE_FALSE(decode_asset_metadata(text));
    }
    auto text = good;
    text.insert(1, R"("unknown":true,)");
    CHECK_FALSE(decode_asset_metadata(text));
    text = good;
    text.insert(1, R"("\u006b\u0069\u006e\u0064":"fixture",)");
    CHECK_FALSE(decode_asset_metadata(text));
    CHECK_FALSE(decode_asset_metadata("[]"));
    CHECK_FALSE(decode_asset_metadata("{"));
    CHECK_FALSE(decode_asset_metadata(std::string(asset_metadata_maximum_bytes + 1, ' ')));
}

TEST_CASE("Asset metadata enforces text, object, dependency and syntax budgets", "[assets]") {
    const auto good = metadata();
    auto value = good;
    value.origin = std::string(1025, 'x');
    CHECK_FALSE(validate_asset_metadata(value));
    value = good;
    value.license.clear();
    CHECK_FALSE(validate_asset_metadata(value));
    value = good;
    value.license = std::string(129, 'x');
    CHECK_FALSE(validate_asset_metadata(value));
    value = good;
    value.kind = "Unknown Kind";
    CHECK_FALSE(validate_asset_metadata(value));
    value = good;
    value.importer = std::string(65, 'a');
    CHECK_FALSE(validate_asset_metadata(value));
    value = good;
    value.origin = std::string(1, '\xff');
    CHECK_FALSE(validate_asset_metadata(value));
    value = good;
    value.origin = "line\nbreak";
    CHECK_FALSE(validate_asset_metadata(value));
    value = good;
    value.audience = static_cast<AssetAudience>(99);
    CHECK_FALSE(validate_asset_metadata(value));
    for (const std::string text :
         {"[]", "null", "{\"x\":1,\"x\":2}", "{\"x\":1e999}", "{\"x\":18446744073709551616}"}) {
        value = good;
        value.settings_json = text;
        CHECK_FALSE(validate_asset_metadata(value));
        value = good;
        value.extras_json = text;
        CHECK_FALSE(validate_asset_metadata(value));
    }
    value = good;
    value.settings_json = std::string(4097, ' ');
    CHECK_FALSE(validate_asset_metadata(value));
    value = good;
    value.settings_json = "{\"x\":" + std::string(32, '[') + "0" + std::string(32, ']') + '}';
    CHECK_FALSE(validate_asset_metadata(value));
    value = good;
    value.dependencies = {value.asset};
    CHECK_FALSE(validate_asset_metadata(value));
    value.dependencies = {id<AssetId>(11), id<AssetId>(11)};
    CHECK_FALSE(validate_asset_metadata(value));
    value.dependencies.clear();
    for (unsigned i = 100; i < 356; ++i)
        value.dependencies.push_back(id<AssetId>(i));
    CHECK(take(decode_asset_metadata(take(encode_asset_metadata(value)))).dependencies.size() ==
          256);
    value.dependencies.push_back(id<AssetId>(356));
    CHECK_FALSE(validate_asset_metadata(value));
}

TEST_CASE("Asset sidecar naming validates both paths and preserves Unicode", "[assets]") {
    const auto source = path("assets/caf\xc3\xa9/one.txt");
    CHECK(take(asset_source_path(take(asset_sidecar_path(source)))) == source);
    CHECK_FALSE(asset_sidecar_path(path("one.SHARKASSET")));
    CHECK_FALSE(asset_source_path(path("one.txt")));
    CHECK_FALSE(asset_source_path(path("one.SHARKASSET")));
    CHECK_FALSE(asset_source_path(path(".sharkasset")));
    CHECK_FALSE(asset_source_path(path("one.sharkasset.sharkasset")));
    CHECK_FALSE(asset_sidecar_path(path(std::string(250, 'a'))));
    auto moved = path("valid.txt");
    auto retained = std::move(moved);
    static_cast<void>(retained);
    if (moved.text().empty())
        CHECK_FALSE(asset_sidecar_path(moved));
}

TEST_CASE("Asset metadata files share exact-content revisions and atomic publication", "[assets]") {
    Fixture fixture;
    auto files = fixture.files();
    const auto sidecar = path("one.txt.sharkasset");
    auto value = metadata();
    auto commit = take(files->write_asset_metadata(sidecar, value, {}));
    CHECK(commit.revision.byte_count() == take(encode_asset_metadata(value)).size());
    const auto first = take(files->read_asset_metadata(sidecar));
    CHECK(first.source.document == value);
    CHECK(first.source.source_schema_version == 1);
    CHECK(first.revision == commit.revision);
    rejected(files->write_asset_metadata(sidecar, value, {}), ErrorCode::conflict);
    take(files->write_asset_metadata(path("other.txt.sharkasset"), metadata(11), {}));
    rejected(files->write_asset_metadata(path("other.txt.sharkasset"), value, first.revision),
             ErrorCode::conflict);
    value.settings_json = "{\"quality\":3}";
    commit = take(files->write_asset_metadata(sidecar, value, first.revision));
    CHECK(commit.revision != first.revision);
    rejected(files->write_asset_metadata(sidecar, metadata(), first.revision), ErrorCode::conflict);
    CHECK(take(files->read_asset_metadata(sidecar)).source.document == value);
    write(fixture.root / "one.txt.sharkasset", "external edit");
    rejected(files->write_asset_metadata(sidecar, value, commit.revision), ErrorCode::conflict);
    CHECK(read(fixture.root / "one.txt.sharkasset") == "external edit");
}

TEST_CASE("Asset metadata rejects foreign-root revisions and cooperating writers", "[assets]") {
    Fixture first;
    Fixture second;
    auto a = first.files();
    auto b = second.files();
    const auto sidecar = path("one.txt.sharkasset");
    const auto revision = take(a->write_asset_metadata(sidecar, metadata(), {})).revision;
    rejected(b->write_asset_metadata(sidecar, metadata(), revision), ErrorCode::conflict);
    rejected(ProjectFiles::open(first.root), ErrorCode::conflict);
    CHECK_FALSE(fs::exists(second.root / "one.txt.sharkasset"));
}

TEST_CASE("Asset metadata publication faults preserve the prior valid sidecar and clean stages",
          "[assets]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.files();
    const auto sidecar = path("one.txt.sharkasset");
    const auto original = read(fixture.root / sidecar.text());
    const auto prior = take(files->read_asset_metadata(sidecar)).revision;
    auto changed = metadata();
    changed.license = "Apache-2.0";
    for (auto fault :
         {detail::FileFault::read, detail::FileFault::stage_create, detail::FileFault::stage_write,
          detail::FileFault::stage_flush, detail::FileFault::publish}) {
        Control control{fault};
        CHECK_FALSE(files->write_asset_metadata(sidecar, changed, prior));
        CHECK(read(fixture.root / sidecar.text()) == original);
        for (const auto& entry : fs::directory_iterator(fixture.root)) {
            CHECK_FALSE(entry.path().filename().string().starts_with(".shark-stage-"));
        }
    }
    CHECK(take(files->write_asset_metadata(sidecar, changed, prior)).revision != prior);
}

TEST_CASE(
    "Asset metadata reads have a native 64 KiB limit and source inspection never decodes source",
    "[assets]") {
    Fixture fixture;
    auto files = fixture.files();
    const auto sidecar = path("one.txt.sharkasset");
    const auto prior = take(files->write_asset_metadata(sidecar, metadata(), {})).revision;
    write(fixture.root / sidecar.text(), std::string(asset_metadata_maximum_bytes + 1, ' '));
    rejected(files->read_asset_metadata(sidecar), ErrorCode::invalid_configuration);
    rejected(files->write_asset_metadata(sidecar, metadata(), prior),
             ErrorCode::invalid_configuration);
    CHECK_FALSE(take(files->asset_source_exists(path("absent/one.txt"))));
    write(fixture.root / "one.txt", std::string(source_maximum_bytes + 1, '\xff'));
    CHECK(take(files->asset_source_exists(path("one.txt"))));
    REQUIRE(fs::create_directory(fixture.root / "directory"));
    CHECK_FALSE(files->asset_source_exists(path("directory")));
    CHECK_FALSE(files->read_asset_metadata(path("one.txt")));
}

TEST_CASE("Asset catalog preserves saved references across rename, directory move and reopen",
          "[assets]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.files();
    auto catalog = take(AssetCatalog::create(*files, id<ProjectId>(1)));
    auto revision = take(catalog->revision());
    const std::array initial{path("one.txt.sharkasset")};
    revision = take(catalog->refresh(initial, revision));
    const auto stable_reference = id<AssetId>(10);
    const auto before = take(catalog->resolve(stable_reference, revision));
    REQUIRE(before.record);
    const auto old_bytes = read(fixture.root / "one.txt.sharkasset");
    REQUIRE(fs::create_directory(fixture.root / fs::path{u8"café 世界"}));
    fs::rename(fixture.root / "one.txt", fixture.root / fs::path{u8"café 世界/renamed.txt"});
    fs::rename(fixture.root / "one.txt.sharkasset",
               fixture.root / fs::path{u8"café 世界/renamed.txt.sharkasset"});
    const std::array moved{path("caf\xc3\xa9 \xe4\xb8\x96\xe7\x95\x8c/renamed.txt.sharkasset")};
    const auto changed = take(catalog->refresh(moved, revision));
    CHECK(changed.value == revision.value + 1);
    rejected(catalog->query(revision), ErrorCode::conflict);
    const auto after = take(catalog->resolve(stable_reference, changed));
    CHECK(after.availability == AssetAvailability::source_present);
    REQUIRE(after.record);
    CHECK(after.record->metadata == before.record->metadata);
    CHECK(after.record->source == take(asset_source_path(moved[0])));
    CHECK(before.record->source == path("one.txt"));
    CHECK(read(fixture.root / native(moved[0].text())) == old_bytes);
    catalog.reset();
    files.reset();
    files = fixture.files();
    catalog = take(AssetCatalog::create(*files, id<ProjectId>(1)));
    rejected(catalog->resolve(stable_reference, changed), ErrorCode::conflict);
    const auto reopened = take(catalog->refresh(moved, take(catalog->revision())));
    CHECK(take(catalog->resolve(stable_reference, reopened)).record->metadata ==
          before.record->metadata);
}

TEST_CASE(
    "Asset catalog distinguishes missing sources and unknown IDs and never invents replacements",
    "[assets]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.files();
    auto catalog = take(AssetCatalog::create(*files, id<ProjectId>(1)));
    const std::array inventory{path("one.txt.sharkasset")};
    auto revision = take(catalog->refresh(inventory, take(catalog->revision())));
    fs::rename(fixture.root / "one.txt", fixture.root / "orphan.txt");
    revision = take(catalog->refresh(inventory, revision));
    const auto missing = take(catalog->resolve(id<AssetId>(10), revision));
    CHECK(missing.availability == AssetAvailability::source_missing);
    REQUIRE(missing.record);
    CHECK(missing.record->metadata == metadata());
    const auto unknown = take(catalog->resolve(id<AssetId>(999), revision));
    CHECK(unknown.availability == AssetAvailability::unknown_id);
    CHECK_FALSE(unknown.record);
    fs::rename(fixture.root / "orphan.txt", fixture.root / "one.txt");
    revision = take(catalog->refresh(inventory, revision));
    CHECK(take(catalog->resolve(id<AssetId>(10), revision)).availability ==
          AssetAvailability::source_present);
    revision = take(catalog->refresh({}, revision));
    CHECK(take(catalog->resolve(id<AssetId>(10), revision)).availability ==
          AssetAvailability::unknown_id);
    CHECK(fs::exists(fixture.root / "one.txt.sharkasset"));
}

TEST_CASE(
    "Asset catalog candidate failure preserves published records, revision and owned snapshots",
    "[assets]") {
    Fixture fixture;
    fixture.seed();
    fixture.seed("two.txt", 11);
    auto files = fixture.files();
    auto catalog = take(AssetCatalog::create(*files, id<ProjectId>(1)));
    const std::array inventory{path("one.txt.sharkasset"), path("two.txt.sharkasset")};
    auto revision = take(catalog->refresh(inventory, take(catalog->revision())));
    const auto prior = take(catalog->query(revision));
    write(fixture.root / "two.txt.sharkasset", take(encode_asset_metadata(metadata())));
    auto failed = catalog->refresh(inventory, revision);
    rejected(failed, ErrorCode::conflict);
    CHECK(failed.error().context.asset_id == id<AssetId>(10).value().to_string());
    CHECK(take(catalog->query(revision)).records == prior.records);
    auto foreign = metadata(11);
    foreign.project = id<ProjectId>(2);
    write(fixture.root / "two.txt.sharkasset", take(encode_asset_metadata(foreign)));
    rejected(catalog->refresh(inventory, revision), ErrorCode::conflict);
    write(fixture.root / "two.txt.sharkasset", "malformed");
    CHECK_FALSE(catalog->refresh(inventory, revision));
    REQUIRE(fs::remove(fixture.root / "two.txt.sharkasset"));
    CHECK_FALSE(catalog->refresh(inventory, revision));
    CHECK(take(catalog->revision()) == revision);
    CHECK(take(catalog->query(revision)).records == prior.records);
    fixture.seed("two.txt", 11);
    const std::array reversed{inventory[1], inventory[0]};
    CHECK(take(catalog->refresh(reversed, revision)) == revision);
    CHECK(prior.records.front().metadata == metadata());
}

TEST_CASE("Asset catalog paths and inventory budgets reject before mutation", "[assets]") {
    Fixture fixture;
    auto files = fixture.files();
    auto catalog = take(AssetCatalog::create(*files, id<ProjectId>(1)));
    const auto revision = take(catalog->revision());
    for (const auto& entries : std::array<std::array<ProjectPath, 2>, 4>{
             {{path("one.txt.sharkasset"), path("one.txt.sharkasset")},
              {path("One.txt.sharkasset"), path("one.txt.sharkasset")},
              {path("Dir/a.sharkasset"), path("dir/b.sharkasset")},
              {path("one.txt.sharkasset"), path("one.txt/child.sharkasset")}}}) {
        rejected(catalog->refresh(entries, revision), ErrorCode::conflict);
    }
    const std::vector<ProjectPath> large(asset_maximum_catalog_entries + 1, path("a.sharkasset"));
    rejected(catalog->refresh(large, revision), ErrorCode::invalid_argument);
    CHECK(take(catalog->revision()) == revision);
}

TEST_CASE("Asset catalog enforces aggregate input budget and publishes no partial overflow",
          "[assets]") {
    Fixture fixture;
    auto files = fixture.files();
    auto catalog = take(AssetCatalog::create(*files, id<ProjectId>(1)));
    std::vector<ProjectPath> inventory;
    for (unsigned i = 0; i < 256; ++i) {
        auto sidecar = path("asset" + std::to_string(i) + ".txt.sharkasset");
        auto text = take(encode_asset_metadata(metadata(100 + i)));
        text.resize(asset_metadata_maximum_bytes, ' ');
        write(fixture.root / sidecar.text(), text);
        inventory.push_back(sidecar);
    }
    const auto empty = take(catalog->revision());
    rejected(catalog->refresh(inventory, empty), ErrorCode::invalid_configuration);
    CHECK(take(catalog->query(empty)).records.empty());
    inventory.pop_back();
    const auto revision = take(catalog->refresh(inventory, empty));
    CHECK(take(catalog->query(revision)).total == 255);
}

TEST_CASE("Asset catalog query pages are ordered, bounded and tied to a fresh owner revision",
          "[assets]") {
    Fixture fixture;
    fixture.seed("z.txt", 12);
    fixture.seed("a.txt", 10);
    fixture.seed("b.txt", 11);
    auto files = fixture.files();
    auto catalog = take(AssetCatalog::create(*files, id<ProjectId>(1)));
    const std::array inventory{path("z.txt.sharkasset"), path("a.txt.sharkasset"),
                               path("b.txt.sharkasset")};
    const auto revision = take(catalog->refresh(inventory, take(catalog->revision())));
    std::vector<AssetId> ids;
    std::size_t offset = 0;
    for (;;) {
        auto page = take(catalog->query(revision, offset, 1));
        REQUIRE(page.records.size() == 1);
        CHECK(page.total == 3);
        CHECK(page.revision == revision);
        ids.push_back(page.records[0].metadata.asset);
        if (!page.next_offset)
            break;
        offset = *page.next_offset;
    }
    CHECK(ids == std::vector<AssetId>{id<AssetId>(10), id<AssetId>(11), id<AssetId>(12)});
    CHECK(take(catalog->query(revision, 3)).records.empty());
    rejected(catalog->query(revision, 4), ErrorCode::invalid_argument);
    rejected(catalog->query(revision, 0, 0), ErrorCode::invalid_argument);
    rejected(catalog->query(revision, 0, 129), ErrorCode::invalid_argument);
    auto other = take(AssetCatalog::create(*files, id<ProjectId>(1)));
    rejected(other->query(revision), ErrorCode::conflict);
}

TEST_CASE("Asset catalog and metadata services enforce the creating thread", "[assets]") {
    Fixture fixture;
    auto files = fixture.files();
    auto catalog = take(AssetCatalog::create(*files, id<ProjectId>(1)));
    const auto revision = take(catalog->revision());
    std::array<std::optional<Error>, 6> errors;
    const auto sidecar = path("a.sharkasset");
    const auto source = path("a");
    const auto candidate = metadata();
    std::thread other{[&] {
        auto a = catalog->revision();
        if (!a)
            errors[0] = a.error();
        auto b = catalog->query(revision);
        if (!b)
            errors[1] = b.error();
        auto c = catalog->refresh({}, revision);
        if (!c)
            errors[2] = c.error();
        auto d = files->read_asset_metadata(sidecar);
        if (!d)
            errors[3] = d.error();
        auto e = files->write_asset_metadata(sidecar, candidate, {});
        if (!e)
            errors[4] = e.error();
        auto f = files->asset_source_exists(source);
        if (!f)
            errors[5] = f.error();
    }};
    other.join();
    for (const auto& error : errors) {
        REQUIRE(error);
        CHECK(error->code == ErrorCode::conflict);
    }
    CHECK_FALSE(fs::exists(fixture.root / "a.sharkasset"));
}

TEST_CASE("Asset sources and sidecars reject symlink escapes, internal aliases and hard links",
          "[assets]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.files();
    REQUIRE(fs::create_directory(fixture.root / "outside"));
    fs::create_symlink(fixture.root / "one.txt", fixture.root / "alias.txt");
    fs::create_symlink(fixture.root / "one.txt.sharkasset", fixture.root / "alias.txt.sharkasset");
    CHECK_FALSE(files->asset_source_exists(path("alias.txt")));
    CHECK_FALSE(files->read_asset_metadata(path("alias.txt.sharkasset")));
    fs::create_directory_symlink(fixture.root.parent_path(), fixture.root / "escape");
    CHECK_FALSE(files->asset_source_exists(path("escape/missing.txt")));
    fs::create_hard_link(fixture.root / "one.txt", fixture.root / "hard.txt");
    CHECK_FALSE(files->asset_source_exists(path("one.txt")));
    CHECK_FALSE(files->asset_source_exists(path("hard.txt")));
}

TEST_CASE("Asset metadata reports publication separately from directory durability", "[assets]") {
    Fixture fixture;
    auto files = fixture.files();
    Control control{detail::FileFault::directory_flush};
    const auto committed =
        take(files->write_asset_metadata(path("one.txt.sharkasset"), metadata(), {}));
    CHECK(take(files->read_asset_metadata(path("one.txt.sharkasset"))).revision ==
          committed.revision);
    CHECK(committed.durability == FileDurability::directory_flush_failed);
}

TEST_CASE("Deep asset inventories charge full paths without retaining every directory prefix",
          "[assets]") {
    Fixture fixture;
    auto files = fixture.files();
    auto catalog = take(AssetCatalog::create(*files, id<ProjectId>(1)));
    std::string directories;
    for (unsigned i = 0; i < 1500; ++i)
        directories += "d/";
    std::vector<ProjectPath> inventory;
    inventory.reserve(asset_maximum_catalog_entries);
    for (std::size_t i = 0; i < asset_maximum_catalog_entries; ++i) {
        inventory.push_back(path(std::to_string(i) + '/' + directories + "source.txt.sharkasset"));
    }
    const auto revision = take(catalog->revision());
    rejected(catalog->refresh(inventory, revision), ErrorCode::invalid_configuration);
    CHECK(take(catalog->revision()) == revision);
    CHECK(take(catalog->query(revision)).records.empty());
}
