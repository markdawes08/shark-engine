#include <shark/storage/store.hpp>

#include "store_test_hooks.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sqlite3.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

using namespace shark;
using namespace shark::storage;
namespace fs = std::filesystem;

template <typename T> T take(Result<T> value) {
    REQUIRE(value);
    return std::move(value).value();
}

template <typename T>
void rejected(const Result<T>& value, ErrorCode code, std::string_view operation) {
    REQUIRE_FALSE(value);
    CHECK(value.error().code == code);
    CHECK(value.error().context.operation == operation);
    CHECK_FALSE(value.error().message.empty());
}

WorldId world() {
    return WorldId{take(StableId::parse("00000000-0000-4000-8000-000000000001"))};
}

WorldId other_world() {
    return WorldId{take(StableId::parse("00000000-0000-4000-8000-000000000002"))};
}

std::string utf8(const fs::path& path) {
    const auto bytes = path.u8string();
    return {bytes.begin(), bytes.end()};
}

std::string read_bytes(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error{"storage fixture read failed"};
    }
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void write_bytes(const fs::path& path, std::string_view bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        throw std::runtime_error{"storage fixture write failed"};
    }
}

class Fixture final {
  public:
    Fixture()
        : parent(fs::current_path()),
          root(parent / ("shark-storage-" + take(generate_stable_id()).to_string())),
          path(root / fs::path{u8"Durable \u4e16\u754c store.db"}) {
        REQUIRE(root.is_absolute());
        REQUIRE(root.parent_path() == parent);
        REQUIRE(fs::create_directory(root));
    }

    ~Fixture() {
        if (root.is_absolute() && root.parent_path() == parent &&
            root.filename().string().starts_with("shark-storage-")) {
            std::error_code error;
            fs::remove_all(root, error);
        }
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    std::unique_ptr<Store> create() const {
        return take(Store::create(path, world()));
    }

    std::unique_ptr<Store> open() const {
        return take(Store::open(path, world()));
    }

    const fs::path parent;
    const fs::path root;
    const fs::path path;
};

struct DatabaseCloser {
    void operator()(sqlite3* value) const noexcept {
        static_cast<void>(sqlite3_close_v2(value));
    }
};

class Database final {
  public:
    explicit Database(const fs::path& path, bool create = false) {
        sqlite3* opened = nullptr;
        const auto status =
            sqlite3_open_v2(utf8(path).c_str(), &opened,
                            SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0), nullptr);
        handle.reset(opened);
        REQUIRE(status == SQLITE_OK);
    }

    void execute(const char* sql) const {
        char* error = nullptr;
        const auto status = sqlite3_exec(handle.get(), sql, nullptr, nullptr, &error);
        const std::string message = error ? error : "";
        sqlite3_free(error);
        INFO(message);
        REQUIRE(status == SQLITE_OK);
    }

    std::string scalar(const char* sql) const {
        sqlite3_stmt* prepared = nullptr;
        REQUIRE(sqlite3_prepare_v2(handle.get(), sql, -1, &prepared, nullptr) == SQLITE_OK);
        const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement{
            prepared, sqlite3_finalize};
        REQUIRE(sqlite3_step(statement.get()) == SQLITE_ROW);
        const auto* text = sqlite3_column_text(statement.get(), 0);
        REQUIRE(text != nullptr);
        const std::string value{reinterpret_cast<const char*>(text),
                                static_cast<std::size_t>(sqlite3_column_bytes(statement.get(), 0))};
        REQUIRE(sqlite3_step(statement.get()) == SQLITE_DONE);
        return value;
    }

  private:
    std::unique_ptr<sqlite3, DatabaseCloser> handle;
};

struct Mutation {
    const char* name;
    const char* sql;
};

thread_local std::array<detail::CreatePhase, 3> observed_phases{};
thread_local std::size_t phase_count = 0;

class CreateRecorder final {
  public:
    CreateRecorder() {
        phase_count = 0;
        detail::set_create_hook([](detail::CreatePhase phase) noexcept {
            if (phase_count < observed_phases.size()) {
                observed_phases[phase_count++] = phase;
            }
        });
    }
    ~CreateRecorder() {
        detail::set_create_hook(nullptr);
    }
    CreateRecorder(const CreateRecorder&) = delete;
    CreateRecorder& operator=(const CreateRecorder&) = delete;
};

static_assert(!std::is_copy_constructible_v<Store>);
static_assert(!std::is_move_constructible_v<Store>);
static_assert(!std::is_default_constructible_v<StoreId>);
static_assert(!std::is_constructible_v<StoreId, WorldId>);
static_assert(!std::is_constructible_v<WorldId, StoreId>);

} // namespace

TEST_CASE("durable store identity survives close and reopen with owned snapshots", "[storage]") {
    Fixture fixture;
    auto created = fixture.create();
    const auto saved = take(created->metadata());
    CHECK(saved.world == world());
    CHECK(saved.schema_version == store_schema_version);
    CHECK(take(StableId::parse(saved.id.value().to_string())) == saved.id.value());
    CHECK(take(created->metadata()) == saved);
    created.reset();
    CHECK(fs::file_size(fixture.path) > 0);
    CHECK(fs::file_size(fixture.path) <= maximum_store_bytes);
    auto reopened = fixture.open();
    CHECK(take(reopened->metadata()) == saved);
    reopened.reset();
    CHECK(saved.world == world());
    CHECK(saved.schema_version == 2);
}

TEST_CASE("separate stores receive distinct stable identities for the same world", "[storage]") {
    Fixture first;
    Fixture second;
    auto a = first.create();
    auto b = second.create();
    const auto original_a = take(a->metadata());
    const auto original_b = take(b->metadata());
    CHECK(original_a.id != original_b.id);
    CHECK(original_a.world == original_b.world);
    a.reset();
    b.reset();
    CHECK(take(first.open()->metadata()) == original_a);
    CHECK(take(second.open()->metadata()) == original_b);
}

TEST_CASE("opening a missing store does not create a database or directory", "[storage]") {
    Fixture fixture;
    rejected(Store::open(fixture.path, world()), ErrorCode::io_failure, "storage.open");
    CHECK_FALSE(fs::exists(fixture.path));
    CHECK(fs::is_empty(fixture.root));
    const auto absent_parent = fixture.root / "missing";
    CHECK_FALSE(Store::open(absent_parent / "store.db", world()));
    CHECK_FALSE(fs::exists(absent_parent));
}

TEST_CASE("creating a store never replaces any existing file", "[storage]") {
    Fixture fixture;
    for (const auto& bytes : {std::string{}, std::string{"foreign file"}}) {
        CAPTURE(bytes);
        write_bytes(fixture.path, bytes);
        rejected(Store::create(fixture.path, world()), ErrorCode::conflict, "storage.create");
        CHECK(read_bytes(fixture.path) == bytes);
    }
    REQUIRE(fs::remove(fixture.path));
    const auto original = take(fixture.create()->metadata());
    const auto bytes = read_bytes(fixture.path);
    rejected(Store::create(fixture.path, world()), ErrorCode::conflict, "storage.create");
    CHECK(read_bytes(fixture.path) == bytes);
    CHECK(take(fixture.open()->metadata()) == original);
}

TEST_CASE("opening empty foreign and truncated storage leaves its bytes intact", "[storage]") {
    Fixture fixture;
    for (const auto& bytes : {std::string{}, std::string{"not a database"},
                              std::string{"{\"format\":\"shark.scene\",\"schema_version\":1}"},
                              std::string{"SQLite format 3\0", 16}}) {
        CAPTURE(bytes.size());
        write_bytes(fixture.path, bytes);
        rejected(Store::open(fixture.path, world()), ErrorCode::invalid_configuration,
                 "storage.open");
        CHECK(read_bytes(fixture.path) == bytes);
    }
}

TEST_CASE("opening a foreign SQLite database does not adopt or mutate it", "[storage]") {
    Fixture fixture;
    {
        Database database{fixture.path, true};
        database.execute(
            "CREATE TABLE foreign_data(value TEXT); INSERT INTO foreign_data VALUES('kept')");
    }
    const auto bytes = read_bytes(fixture.path);
    rejected(Store::open(fixture.path, world()), ErrorCode::invalid_configuration, "storage.open");
    CHECK(read_bytes(fixture.path) == bytes);
    Database database{fixture.path};
    CHECK(database.scalar("SELECT value FROM foreign_data") == "kept");
}

TEST_CASE("durable storage headers reject future foreign and unsupported physical versions",
          "[storage]") {
    const std::array mutations{
        Mutation{"future database schema", "PRAGMA user_version=3"},
        Mutation{"older database schema", "PRAGMA user_version=0"},
        Mutation{"foreign application", "PRAGMA application_id=1"},
        Mutation{"different page size", ""},
    };
    for (const auto& mutation : mutations) {
        DYNAMIC_SECTION(mutation.name) {
            Fixture fixture;
            fixture.create().reset();
            if (mutation.sql[0] == '\0') {
                // Header preflight must reject unsupported physical formats without
                // SQLite rewriting the file. VACUUM itself needs a disabled ATTACH.
                auto bytes = read_bytes(fixture.path);
                REQUIRE(bytes.size() >= 100);
                bytes[16] = 0x20;
                bytes[17] = 0;
                write_bytes(fixture.path, bytes);
            } else {
                Database database{fixture.path};
                database.execute(mutation.sql);
            }
            const auto bytes = read_bytes(fixture.path);
            rejected(Store::open(fixture.path, world()), ErrorCode::invalid_configuration,
                     "storage.open");
            CHECK(read_bytes(fixture.path) == bytes);
        }
    }
}

TEST_CASE("durable store rejects malformed metadata and altered schemas", "[storage]") {
    const std::array mutations{
        Mutation{"missing metadata row", "DELETE FROM shark_store"},
        Mutation{"duplicate metadata row",
                 "PRAGMA ignore_check_constraints=ON; INSERT INTO shark_store SELECT "
                 "2,store_id,world_id,schema_version FROM shark_store"},
        Mutation{"malformed store identity",
                 "PRAGMA ignore_check_constraints=ON; UPDATE shark_store SET store_id='bad-id'"},
        Mutation{"malformed world identity",
                 "PRAGMA ignore_check_constraints=ON; UPDATE shark_store SET world_id='bad-world'"},
        Mutation{"noncanonical identity",
                 "UPDATE shark_store SET store_id='00000000-0000-4000-8000-00000000000A'"},
        Mutation{"oversized identity", "PRAGMA ignore_check_constraints=ON; UPDATE shark_store SET "
                                       "store_id=printf('%010000d',1)"},
        Mutation{"future row version",
                 "PRAGMA ignore_check_constraints=ON; UPDATE shark_store SET schema_version=3"},
        Mutation{"missing metadata table", "DROP TABLE shark_store"},
        Mutation{"added column", "ALTER TABLE shark_store ADD COLUMN unsupported TEXT"},
        Mutation{"additional table", "CREATE TABLE unsupported(value INTEGER)"},
        Mutation{"additional view", "CREATE VIEW unsupported AS SELECT store_id FROM shark_store"},
        Mutation{"additional trigger",
                 "CREATE TRIGGER unsupported AFTER UPDATE ON shark_store BEGIN SELECT 1; END"},
    };
    for (const auto& mutation : mutations) {
        DYNAMIC_SECTION(mutation.name) {
            Fixture fixture;
            fixture.create().reset();
            {
                Database database{fixture.path};
                database.execute(mutation.sql);
            }
            const auto bytes = read_bytes(fixture.path);
            rejected(Store::open(fixture.path, world()), ErrorCode::invalid_configuration,
                     "storage.open");
            CHECK(read_bytes(fixture.path) == bytes);
        }
    }
}

TEST_CASE("durable store rejects a wrong world without changing the original identity",
          "[storage]") {
    Fixture fixture;
    const auto metadata = take(fixture.create()->metadata());
    const auto bytes = read_bytes(fixture.path);
    rejected(Store::open(fixture.path, other_world()), ErrorCode::conflict, "storage.open");
    CHECK(read_bytes(fixture.path) == bytes);
    CHECK(take(fixture.open()->metadata()) == metadata);
}

TEST_CASE("one store owner excludes another connection until destruction", "[storage]") {
    Fixture fixture;
    auto first = fixture.create();
    const auto metadata = take(first->metadata());
    rejected(Store::open(fixture.path, world()), ErrorCode::conflict, "storage.open");
    rejected(Store::create(fixture.path, world()), ErrorCode::conflict, "storage.create");
    CHECK(take(first->metadata()) == metadata);
    first.reset();
    auto second = fixture.open();
    CHECK(take(second->metadata()) == metadata);
    rejected(Store::open(fixture.path, world()), ErrorCode::conflict, "storage.open");
}

TEST_CASE("store metadata requires the creating thread and returns independent values",
          "[storage]") {
    Fixture fixture;
    auto owner = fixture.create();
    const auto original = take(owner->metadata());
    std::optional<Result<StoreMetadata>> result;
    std::thread worker{[&] { result.emplace(owner->metadata()); }};
    worker.join();
    REQUIRE(result);
    rejected(*result, ErrorCode::conflict, "storage.metadata");
    CHECK(take(owner->metadata()) == original);
    auto altered = original;
    altered.world = other_world();
    altered.schema_version = 19;
    CHECK(take(owner->metadata()) == original);
    owner.reset();
    CHECK(original.world == world());
    CHECK(altered.world == other_world());
}

TEST_CASE("storage creation commits one bounded fixed schema and metadata row", "[storage]") {
    Fixture fixture;
    const auto metadata = take(fixture.create()->metadata());
    CHECK(sqlite3_libversion_number() == SQLITE_VERSION_NUMBER);
    Database database{fixture.path};
    CHECK(database.scalar("PRAGMA application_id") == "1397246788");
    CHECK(database.scalar("PRAGMA user_version") == "2");
    CHECK(database.scalar("PRAGMA page_size") == "4096");
    CHECK(database.scalar("PRAGMA journal_mode") == "delete");
    CHECK(database.scalar("PRAGMA integrity_check") == "ok");
    CHECK(database.scalar("SELECT count(*) FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%'") ==
          "4");
    CHECK(database.scalar("SELECT count(*) FROM shark_store") == "1");
    CHECK(database.scalar("SELECT singleton FROM shark_store") == "1");
    CHECK(database.scalar("SELECT store_id FROM shark_store") == metadata.id.value().to_string());
    CHECK(database.scalar("SELECT world_id FROM shark_store") ==
          metadata.world.value().to_string());
    CHECK(database.scalar("SELECT schema_version FROM shark_store") == "2");
    CHECK(database.scalar("SELECT strict FROM pragma_table_list WHERE name='shark_store'") == "1");
}

TEST_CASE("storage bounds reject oversized databases before opening them", "[storage]") {
    Fixture fixture;
    fixture.create().reset();
    const auto original = read_bytes(fixture.path);
    fs::resize_file(fixture.path, maximum_store_bytes + 4096);
    rejected(Store::open(fixture.path, world()), ErrorCode::invalid_configuration, "storage.open");
    CHECK(fs::file_size(fixture.path) == maximum_store_bytes + 4096);
    fs::resize_file(fixture.path, original.size());
    CHECK(read_bytes(fixture.path) == original);
    CHECK(fixture.open());
}

TEST_CASE("storage creation reaches deterministic schema and commit phase boundaries",
          "[storage]") {
    Fixture fixture;
    CreateRecorder recorder;
    auto owner = fixture.create();
    REQUIRE(phase_count == 3);
    CHECK(observed_phases[0] == detail::CreatePhase::schema_written);
    CHECK(observed_phases[1] == detail::CreatePhase::before_commit);
    CHECK(observed_phases[2] == detail::CreatePhase::after_commit);
    const auto original = take(owner->metadata());
    owner.reset();
    CHECK(take(fixture.open()->metadata()) == original);
    CHECK(phase_count == 3);
}

TEST_CASE("storage rejects invalid or unbounded paths before creating anything", "[storage]") {
    Fixture fixture;
    const std::array paths{fs::path{}, fs::path{"relative-store.db"}, fixture.root / "CON",
                           fixture.root / std::string(241, 'a'),
                           fixture.root / "missing" / "store.db"};
    for (const auto& path : paths) {
        CAPTURE(utf8(path));
        rejected(Store::create(path, world()), ErrorCode::invalid_path, "storage.create");
        rejected(Store::open(path, world()), ErrorCode::invalid_path, "storage.open");
        CHECK(fs::is_empty(fixture.root));
    }
}

TEST_CASE("storage rejects incompatible or unsafe sidecars without replacing their bytes",
          "[storage]") {
    for (const auto suffix : {"-wal", "-shm", "-journal"}) {
        DYNAMIC_SECTION(suffix) {
            Fixture fixture;
            const auto original = take(fixture.create()->metadata());
            const auto bytes = read_bytes(fixture.path);
            auto sidecar = fixture.path;
            sidecar += suffix;
            if (std::string_view{suffix} == "-journal") {
                REQUIRE(fs::create_directory(sidecar));
                rejected(Store::open(fixture.path, world()), ErrorCode::invalid_path,
                         "storage.open");
                CHECK(fs::is_directory(sidecar));
            } else {
                write_bytes(sidecar, "external sidecar");
                rejected(Store::open(fixture.path, world()), ErrorCode::invalid_configuration,
                         "storage.open");
                CHECK(read_bytes(sidecar) == "external sidecar");
            }
            CHECK(read_bytes(fixture.path) == bytes);
            REQUIRE(fs::remove(sidecar));
            CHECK(take(fixture.open()->metadata()) == original);
        }
    }
}

TEST_CASE("storage preserves a leftover journal and rejects oversized journal input", "[storage]") {
    Fixture fixture;
    auto journal = fixture.path;
    journal += "-journal";
    write_bytes(journal, "retained incomplete creation");
    rejected(Store::create(fixture.path, world()), ErrorCode::conflict, "storage.create");
    CHECK_FALSE(fs::exists(fixture.path));
    CHECK(read_bytes(journal) == "retained incomplete creation");
    REQUIRE(fs::remove(journal));
    const auto original = take(fixture.create()->metadata());
    const auto before = read_bytes(fixture.path);
    write_bytes(journal, {});
    fs::resize_file(journal, maximum_store_bytes * 2 + 1);
    rejected(Store::open(fixture.path, world()), ErrorCode::invalid_configuration, "storage.open");
    CHECK(fs::file_size(journal) == maximum_store_bytes * 2 + 1);
    CHECK(read_bytes(fixture.path) == before);
    REQUIRE(fs::remove(journal));
    CHECK(take(fixture.open()->metadata()) == original);
}

TEST_CASE("storage rejects hard link aliases without disturbing the original file", "[storage]") {
    Fixture fixture;
    const auto original = take(fixture.create()->metadata());
    const auto before = read_bytes(fixture.path);
    const auto alias = fixture.root / "alias.db";
    fs::create_hard_link(fixture.path, alias);
    REQUIRE(fs::hard_link_count(fixture.path) == 2);
    rejected(Store::open(fixture.path, world()), ErrorCode::invalid_path, "storage.open");
    rejected(Store::open(alias, world()), ErrorCode::invalid_path, "storage.open");
    CHECK(read_bytes(alias) == before);
    REQUIRE(fs::remove(alias));
    CHECK(take(fixture.open()->metadata()) == original);
}
