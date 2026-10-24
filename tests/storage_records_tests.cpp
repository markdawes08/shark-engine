#include <shark/storage/store.hpp>

#include "store_test_hooks.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace {
using namespace shark;
using namespace shark::storage;
namespace fs = std::filesystem;

template <typename T> T take(Result<T> value) {
    REQUIRE(value);
    return std::move(value).value();
}

template <typename T> void rejected(const Result<T>& value, ErrorCode code) {
    REQUIRE_FALSE(value);
    CHECK(value.error().code == code);
    CHECK_FALSE(value.error().context.operation.empty());
}

StableId identity(unsigned value) {
    std::ostringstream text;
    text << "00000000-0000-4000-8000-" << std::hex << std::setw(12) << std::setfill('0') << value;
    return take(StableId::parse(text.str()));
}

net::AccountId account(unsigned value = 1) {
    return net::AccountId{identity(value)};
}
WorldId world() {
    return WorldId{identity(900)};
}

net::CertificateFingerprint certificate(unsigned value = 1) {
    net::CertificateFingerprint result{};
    result[30] = static_cast<std::byte>((value >> 8) & 255U);
    result[31] = static_cast<std::byte>(value & 255U);
    return result;
}

CharacterState state(std::string name = "Checkpoint") {
    return {Name{std::move(name)},
            WorldPosition{world(), {world_cell_min, 17, world_cell_max}, {0.0, 1023.75, 0.125}}};
}

std::string bytes(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input);
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void raw(const fs::path& path, const std::string& sql, bool create = false) {
    const auto utf8 = path.u8string();
    const std::string path_text{utf8.begin(), utf8.end()};
    sqlite3* opened = nullptr;
    const auto code =
        sqlite3_open_v2(path_text.c_str(), &opened,
                        SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0), nullptr);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> owner{opened, sqlite3_close_v2};
    REQUIRE(code == SQLITE_OK);
    char* message = nullptr;
    const auto result = sqlite3_exec(opened, sql.c_str(), nullptr, nullptr, &message);
    const std::string diagnostic = message == nullptr ? "" : message;
    sqlite3_free(message);
    INFO(diagnostic);
    REQUIRE(result == SQLITE_OK);
}

class Fixture final {
  public:
    Fixture()
        : parent(fs::current_path()),
          root(parent / ("shark-storage-records-" + take(generate_stable_id()).to_string())),
          path(root / "records.db") {
        REQUIRE(fs::create_directory(root));
    }
    ~Fixture() {
        if (root.is_absolute() && root.parent_path() == parent &&
            root.filename().string().starts_with("shark-storage-records-")) {
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

thread_local detail::WritePhase fault_phase = detail::WritePhase::after_write;
thread_local bool fail_rollback = false;

class WriteFault final {
  public:
    explicit WriteFault(detail::WritePhase phase, bool rollback = false) {
        fault_phase = phase;
        fail_rollback = rollback;
        detail::set_write_fault_hook([](detail::WritePhase value) noexcept {
            return value == fault_phase ||
                   (fail_rollback && value == detail::WritePhase::before_rollback);
        });
    }
    ~WriteFault() {
        detail::set_write_fault_hook(nullptr);
    }
    WriteFault(const WriteFault&) = delete;
    WriteFault& operator=(const WriteFault&) = delete;
};
} // namespace

TEST_CASE("durable account certificate and character records survive reopen in stable order",
          "[storage]") {
    Fixture fixture;
    auto owner = fixture.create();
    CHECK(take(owner->registry()) == DurableRegistry{});
    for (unsigned value : {3U, 1U, 2U}) {
        const auto provisioned = take(owner->provision_account(account(value)));
        CHECK(provisioned == AccountRecord{account(value), false, 1});
        REQUIRE(owner->enroll_certificate(account(value), certificate(value)));
    }
    const auto empty = take(owner->roster(account()));
    CHECK(empty.revision == 1);
    CHECK(empty.characters.empty());
    auto first = take(owner->create_character(account(), 1, state("First")));
    auto second = take(owner->create_character(account(), 2, state("Second")));
    CHECK(first.revision == 1);
    CHECK(first.character != second.character);
    CHECK(first.entity != second.entity);
    CHECK(first.character.value() != first.entity.value());
    const auto roster = take(owner->roster(account()));
    CHECK(roster.revision == 3);
    REQUIRE(roster.characters.size() == 2);
    CHECK(roster.characters[0].character < roster.characters[1].character);
    const auto registry = take(owner->registry());
    REQUIRE(registry.accounts.size() == 3);
    REQUIRE(registry.certificates.size() == 3);
    for (std::size_t i = 1; i < 3; ++i) {
        CHECK(registry.accounts[i - 1].account < registry.accounts[i].account);
        CHECK(registry.certificates[i - 1].certificate < registry.certificates[i].certificate);
    }
    owner.reset();
    owner = fixture.open();
    CHECK(take(owner->registry()) == registry);
    CHECK(take(owner->roster(account())) == roster);
    CHECK(take(owner->character(account(), first.character)) == first);
    CHECK(take(owner->character(account(), second.character)) == second);
    first.state.name.value = "independent snapshot";
    CHECK(take(owner->character(account(), first.character)).state.name.value == "First");
}

TEST_CASE("character checkpoints compare revisions and preserve immutable ownership identities",
          "[storage]") {
    Fixture fixture;
    auto owner = fixture.create();
    take(owner->provision_account(account()));
    take(owner->provision_account(account(2)));
    const auto initial = take(owner->create_character(account(), 1, state("Initial")));
    const auto baseline = take(owner->registry());
    rejected(owner->create_character(account(), 1, state()), ErrorCode::conflict);
    rejected(owner->create_character(account(), 0, state()), ErrorCode::conflict);
    rejected(owner->create_character(account(), maximum_durable_revision + 1, state()),
             ErrorCode::conflict);
    rejected(owner->save_character(account(2), initial.character, 1, state()), ErrorCode::conflict);
    rejected(owner->character(account(2), initial.character), ErrorCode::conflict);
    rejected(owner->save_character(account(), initial.character, 0, state()), ErrorCode::conflict);
    rejected(
        owner->save_character(account(), initial.character, maximum_durable_revision + 1, state()),
        ErrorCode::conflict);
    CHECK(take(owner->registry()) == baseline);
    auto accepted = state("Saved \xE4\xB8\x96\xE7\x95\x8C");
    accepted.position.cell = {42, -30, 90};
    accepted.position.offset = {0.125, 1.5, 1023.5};
    const auto saved = take(owner->save_character(account(), initial.character, 1, accepted));
    CHECK(saved.revision == 2);
    CHECK(saved.character == initial.character);
    CHECK(saved.account == initial.account);
    CHECK(saved.entity == initial.entity);
    CHECK(saved.state == accepted);
    CHECK(take(owner->roster(account())).revision == 2);
    rejected(owner->save_character(account(), initial.character, 1, state()), ErrorCode::conflict);
    owner.reset();
    owner = fixture.open();
    CHECK(take(owner->character(account(), initial.character)) == saved);
}

TEST_CASE("durable revocation tombstones reject reassignment and future writes after restart",
          "[storage]") {
    Fixture fixture;
    auto owner = fixture.create();
    take(owner->provision_account(account()));
    take(owner->provision_account(account(2)));
    REQUIRE(owner->enroll_certificate(account(), certificate()));
    const auto created = take(owner->create_character(account(), 1, state()));
    REQUIRE(owner->revoke_certificate(certificate()));
    REQUIRE(owner->revoke_certificate(certificate()));
    REQUIRE(owner->revoke_account(account()));
    REQUIRE(owner->revoke_account(account()));
    const auto tombstones = take(owner->registry());
    CHECK(tombstones.accounts[0].revoked);
    CHECK(tombstones.certificates[0].revoked);
    owner.reset();
    owner = fixture.open();
    CHECK(take(owner->registry()) == tombstones);
    rejected(owner->provision_account(account()), ErrorCode::conflict);
    rejected(owner->enroll_certificate(account(2), certificate()), ErrorCode::conflict);
    rejected(owner->enroll_certificate(account(), certificate(2)), ErrorCode::conflict);
    rejected(owner->create_character(account(), 2, state()), ErrorCode::conflict);
    rejected(owner->save_character(account(), created.character, 1, state()), ErrorCode::conflict);
    CHECK(take(owner->character(account(), created.character)) == created);
    CHECK(take(owner->registry()) == tombstones);
}

TEST_CASE("durable operations reject unknown owners and empty enrollment fingerprints",
          "[storage]") {
    Fixture fixture;
    auto owner = fixture.create();
    rejected(owner->roster(account()), ErrorCode::conflict);
    rejected(owner->create_character(account(), 1, state()), ErrorCode::conflict);
    rejected(owner->enroll_certificate(account(), certificate()), ErrorCode::conflict);
    rejected(owner->revoke_account(account()), ErrorCode::conflict);
    rejected(owner->revoke_certificate(certificate()), ErrorCode::conflict);
    take(owner->provision_account(account()));
    rejected(owner->enroll_certificate(account(), {}), ErrorCode::invalid_argument);
    CHECK(take(owner->registry()).certificates.empty());
}

TEST_CASE("durable checkpoint validation rejects invalid names worlds and noncanonical positions",
          "[storage]") {
    Fixture fixture;
    auto owner = fixture.create();
    take(owner->provision_account(account()));
    const auto created = take(owner->create_character(account(), 1, state()));
    std::vector<CharacterState> invalid;
    for (const auto& name : {std::string{}, std::string(257, 'x'), std::string{"\xC0\xAF"},
                             std::string{"bad\nname"}, std::string{"nul\0name", 8}}) {
        invalid.push_back(state(name));
    }
    auto other_world = state();
    other_world.position.world = WorldId{identity(901)};
    invalid.push_back(other_world);
    for (const auto offset : {-0.0, -1.0, 1024.0, std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::quiet_NaN()}) {
        auto value = state();
        value.position.offset[1] = offset;
        invalid.push_back(value);
    }
    auto cell = state();
    cell.position.cell.x = world_cell_min - 1;
    invalid.push_back(cell);
    cell.position.cell.x = world_cell_max + 1;
    invalid.push_back(cell);
    const auto before = take(owner->registry());
    for (const auto& candidate : invalid) {
        rejected(validate_character_state(candidate, world()), ErrorCode::invalid_argument);
        rejected(owner->create_character(account(), 2, candidate), ErrorCode::invalid_argument);
        rejected(owner->save_character(account(), created.character, 1, candidate),
                 ErrorCode::invalid_argument);
        CHECK(take(owner->registry()) == before);
    }
    REQUIRE(validate_character_state(state(std::string(256, 'x')), world()));
    CHECK(take(owner->save_character(account(), created.character, 1, state(std::string(256, 'x'))))
              .revision == 2);
}

TEST_CASE("durable registry capacities include retained revocation tombstones", "[storage]") {
    Fixture fixture;
    auto owner = fixture.create();
    for (unsigned value = 1; value <= maximum_durable_accounts; ++value) {
        take(owner->provision_account(account(value)));
    }
    REQUIRE(owner->revoke_account(account(64)));
    rejected(owner->provision_account(account(65)), ErrorCode::conflict);
    for (unsigned value = 1; value <= maximum_durable_certificates; ++value) {
        REQUIRE(owner->enroll_certificate(account(), certificate(value)));
    }
    REQUIRE(owner->revoke_certificate(certificate(128)));
    rejected(owner->enroll_certificate(account(), certificate(129)), ErrorCode::conflict);
    for (unsigned owner_id = 1; owner_id <= 16; ++owner_id) {
        for (std::uint64_t revision = 1; revision <= maximum_characters_per_account; ++revision) {
            take(owner->create_character(account(owner_id), revision, state()));
        }
    }
    rejected(owner->create_character(account(), 9, state()), ErrorCode::conflict);
    rejected(owner->create_character(account(17), 1, state()), ErrorCode::conflict);
    const auto full = take(owner->registry());
    CHECK(full.accounts.size() == maximum_durable_accounts);
    CHECK(full.certificates.size() == maximum_durable_certificates);
    CHECK(full.characters.size() == maximum_durable_characters);
    owner.reset();
    CHECK(take(fixture.open()->registry()) == full);
}

TEST_CASE("durable revision exhaustion rejects without wrap or database changes", "[storage]") {
    Fixture fixture;
    auto owner = fixture.create();
    take(owner->provision_account(account()));
    const auto created = take(owner->create_character(account(), 1, state()));
    owner.reset();
    raw(fixture.path, "UPDATE shark_accounts SET roster_revision=9223372036854775807;"
                      "UPDATE shark_characters SET revision=9223372036854775807");
    const auto before_bytes = bytes(fixture.path);
    owner = fixture.open();
    const auto before = take(owner->registry());
    rejected(owner->create_character(account(), maximum_durable_revision, state()),
             ErrorCode::conflict);
    rejected(owner->save_character(account(), created.character, maximum_durable_revision, state()),
             ErrorCode::conflict);
    CHECK(take(owner->registry()) == before);
    owner.reset();
    CHECK(bytes(fixture.path) == before_bytes);
}

TEST_CASE("durable operations require the owner thread without modifying cache or disk",
          "[storage]") {
    Fixture fixture;
    auto owner = fixture.create();
    take(owner->provision_account(account()));
    REQUIRE(owner->enroll_certificate(account(), certificate()));
    const auto created = take(owner->create_character(account(), 1, state()));
    const auto before = take(owner->registry());
    std::array<bool, 9> rejected_calls{};
    std::thread worker{[&] {
        const auto failed = [](const auto& value) {
            return !value && value.error().code == ErrorCode::conflict;
        };
        rejected_calls = {failed(owner->registry()),
                          failed(owner->roster(account())),
                          failed(owner->character(account(), created.character)),
                          failed(owner->provision_account(account(2))),
                          failed(owner->enroll_certificate(account(), certificate(2))),
                          failed(owner->revoke_account(account())),
                          failed(owner->revoke_certificate(certificate())),
                          failed(owner->create_character(account(), 2, state())),
                          failed(owner->save_character(account(), created.character, 1, state()))};
    }};
    worker.join();
    for (const bool rejected_call : rejected_calls) {
        CHECK(rejected_call);
    }
    CHECK(take(owner->registry()) == before);
    owner.reset();
    CHECK(take(fixture.open()->registry()) == before);
}

TEST_CASE("durable writes roll back precommit failures before permitting another operation",
          "[storage]") {
    Fixture fixture;
    auto owner = fixture.create();
    take(owner->provision_account(account()));
    const auto created = take(owner->create_character(account(), 1, state()));
    const auto before = take(owner->registry());
    {
        WriteFault fault{detail::WritePhase::after_write};
        rejected(owner->save_character(account(), created.character, 1, state("Rejected")),
                 ErrorCode::io_failure);
        CHECK(take(owner->registry()) == before);
        rejected(owner->create_character(account(), 2, state()), ErrorCode::io_failure);
        CHECK(take(owner->registry()) == before);
    }
    CHECK(
        take(owner->save_character(account(), created.character, 1, state("Accepted"))).revision ==
        2);
    const auto saved = take(owner->registry());
    owner.reset();
    CHECK(take(fixture.open()->registry()) == saved);
}

TEST_CASE("failed rollback and uncertain commits quiesce every store operation until reopen",
          "[storage]") {
    for (const auto phase : {detail::WritePhase::before_commit, detail::WritePhase::after_commit,
                             detail::WritePhase::after_write}) {
        DYNAMIC_SECTION(static_cast<int>(phase)) {
            Fixture fixture;
            auto owner = fixture.create();
            take(owner->provision_account(account()));
            REQUIRE(owner->enroll_certificate(account(), certificate()));
            const auto created = take(owner->create_character(account(), 1, state("Before")));
            {
                WriteFault fault{phase, phase == detail::WritePhase::after_write};
                rejected(owner->save_character(account(), created.character, 1, state("After")),
                         ErrorCode::io_failure);
            }
            rejected(owner->metadata(), ErrorCode::execution_failed);
            rejected(owner->registry(), ErrorCode::execution_failed);
            rejected(owner->roster(account()), ErrorCode::execution_failed);
            rejected(owner->character(account(), created.character), ErrorCode::execution_failed);
            rejected(owner->provision_account(account(2)), ErrorCode::execution_failed);
            rejected(owner->enroll_certificate(account(), certificate(2)),
                     ErrorCode::execution_failed);
            rejected(owner->revoke_account(account()), ErrorCode::execution_failed);
            rejected(owner->revoke_certificate(certificate()), ErrorCode::execution_failed);
            rejected(owner->create_character(account(), 2, state()), ErrorCode::execution_failed);
            rejected(owner->save_character(account(), created.character, 1, state()),
                     ErrorCode::execution_failed);
            owner.reset();
            owner = fixture.open();
            const auto actual = take(owner->character(account(), created.character));
            const bool committed = phase == detail::WritePhase::after_commit;
            CHECK(actual.revision == (committed ? 2 : 1));
            CHECK(actual.state.name.value == (committed ? "After" : "Before"));
            CHECK(take(owner->save_character(account(), created.character, actual.revision,
                                             state("Reconciled")))
                      .revision == actual.revision + 1);
        }
    }
}

TEST_CASE("schema one stores reject before any write and preserve their legacy bytes",
          "[storage]") {
    Fixture fixture;
    raw(fixture.path,
        "PRAGMA page_size=4096; PRAGMA application_id=1397246788; PRAGMA user_version=1;"
        "CREATE TABLE shark_store(singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
        "store_id TEXT NOT NULL CHECK(length(store_id)=36),world_id TEXT NOT NULL "
        "CHECK(length(world_id)=36),"
        "schema_version INTEGER NOT NULL CHECK(schema_version=1)) STRICT;"
        "INSERT INTO shark_store VALUES(1,'00000000-0000-4000-8000-000000000001',"
        "'00000000-0000-4000-8000-000000000384',1)",
        true);
    const auto before = bytes(fixture.path);
    rejected(Store::open(fixture.path, world()), ErrorCode::invalid_configuration);
    CHECK(bytes(fixture.path) == before);
    CHECK_FALSE(fs::exists(fs::path{fixture.path.string() + "-journal"}));
}

TEST_CASE("schema two rejects malformed durable records foreign keys and altered indexes",
          "[storage]") {
    const std::array mutations{
        "UPDATE shark_accounts SET account_id='00000000-0000-4000-8000-00000000000A'",
        "UPDATE shark_accounts SET revoked=2",
        "UPDATE shark_accounts SET roster_revision=0",
        "UPDATE shark_accounts SET roster_revision=1",
        "UPDATE shark_certificates SET certificate=zeroblob(32)",
        "UPDATE shark_certificates SET certificate=zeroblob(31)",
        "UPDATE shark_certificates SET revoked=2",
        "UPDATE shark_certificates SET account_id='00000000-0000-4000-8000-000000000099'",
        "UPDATE shark_characters SET account_id='00000000-0000-4000-8000-000000000099'",
        "UPDATE shark_characters SET character_id='bad'",
        "UPDATE shark_characters SET entity_id=character_id",
        "UPDATE shark_characters SET revision=0",
        "UPDATE shark_characters SET name=CAST(x'C0AF' AS TEXT)",
        "UPDATE shark_characters SET name=CAST(x'610062' AS TEXT)",
        "UPDATE shark_characters SET name=''",
        "UPDATE shark_characters SET cell_x=2147483648",
        "UPDATE shark_characters SET offset_y=-1",
        "UPDATE shark_characters SET offset_z=1024",
        "UPDATE shark_characters SET offset_x=1e999",
        "CREATE INDEX unauthorized_index ON shark_characters(name)",
        "ALTER TABLE shark_accounts ADD COLUMN extra INTEGER",
        "DELETE FROM shark_accounts",
        "DROP TABLE shark_certificates",
    };
    for (const auto* mutation : mutations) {
        DYNAMIC_SECTION(mutation) {
            Fixture fixture;
            auto owner = fixture.create();
            take(owner->provision_account(account()));
            REQUIRE(owner->enroll_certificate(account(), certificate()));
            take(owner->create_character(account(), 1, state()));
            owner.reset();
            raw(fixture.path,
                std::string{"PRAGMA foreign_keys=OFF; PRAGMA ignore_check_constraints=ON;"} +
                    mutation);
            const auto before = bytes(fixture.path);
            rejected(Store::open(fixture.path, world()), ErrorCode::invalid_configuration);
            CHECK(bytes(fixture.path) == before);
        }
    }
}

TEST_CASE("schema two rejects oversized persisted registries before exposing their records",
          "[storage]") {
    for (const std::string_view kind : {"accounts", "certificates", "characters", "roster"}) {
        DYNAMIC_SECTION(kind) {
            Fixture fixture;
            auto owner = fixture.create();
            take(owner->provision_account(account()));
            REQUIRE(owner->enroll_certificate(account(), certificate()));
            take(owner->create_character(account(), 1, state()));
            owner.reset();
            std::string sql = "BEGIN;";
            const unsigned accounts = kind == "accounts" ? 65U : (kind == "characters" ? 17U : 1U);
            for (unsigned i = 2; i <= accounts; ++i) {
                sql += "INSERT INTO shark_accounts VALUES('" + identity(i).to_string() + "',0,9);";
            }
            if (kind == "certificates") {
                for (unsigned i = 2; i <= 129; ++i) {
                    std::ostringstream fingerprint;
                    fingerprint << std::hex << std::setfill('0') << std::setw(64) << i;
                    sql += "INSERT INTO shark_certificates VALUES(x'" + fingerprint.str() + "','" +
                           identity(1).to_string() + "',0);";
                }
            }
            if (kind == "characters" || kind == "roster") {
                const unsigned characters = kind == "characters" ? 129U : 9U;
                for (unsigned i = 2; i <= characters; ++i) {
                    const auto owner_id = kind == "characters" ? (i - 1) / 8 + 1 : 1U;
                    sql += "INSERT INTO shark_characters VALUES('" +
                           identity(10000 + i).to_string() + "','" +
                           identity(owner_id).to_string() + "','" +
                           identity(20000 + i).to_string() + "',1,'Bounded',0,0,0,0.0,0.0,0.0);";
                }
                sql += "UPDATE shark_accounts SET roster_revision=10;";
            }
            sql += "COMMIT;";
            raw(fixture.path, sql);
            const auto before = bytes(fixture.path);
            rejected(Store::open(fixture.path, world()), ErrorCode::invalid_configuration);
            CHECK(bytes(fixture.path) == before);
        }
    }
}

TEST_CASE("lost character creation acknowledgement requires roster reconciliation after reopen",
          "[storage]") {
    Fixture fixture;
    auto owner = fixture.create();
    take(owner->provision_account(account()));
    {
        WriteFault fault{detail::WritePhase::after_commit};
        rejected(owner->create_character(account(), 1, state("Committed once")),
                 ErrorCode::io_failure);
    }
    rejected(owner->create_character(account(), 1, state()), ErrorCode::execution_failed);
    owner.reset();
    owner = fixture.open();
    const auto recovered = take(owner->roster(account()));
    CHECK(recovered.revision == 2);
    REQUIRE(recovered.characters.size() == 1);
    CHECK(recovered.characters[0].revision == 1);
    CHECK(recovered.characters[0].state.name.value == "Committed once");
    rejected(owner->create_character(account(), 1, state()), ErrorCode::conflict);
    CHECK(take(owner->roster(account())) == recovered);
}
