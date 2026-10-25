#include <shark/storage/characters.hpp>

#include "fixtures/storage/fixture.hpp"
#include "maintenance_test_hooks.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace {
using namespace shark;
using namespace shark::net;
using namespace shark::storage;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace fixture_data = storage_fixture;

template <typename T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().context.operation << ": " << result.error().message);
        REQUIRE(result);
    }
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result) {
        INFO(result.error().context.operation << ": " << result.error().message);
        REQUIRE(result);
    }
}
template <typename T> void reject(const Result<T>& result) {
    REQUIRE_FALSE(result);
    CHECK_FALSE(result.error().message.empty());
    CHECK_FALSE(result.error().context.operation.empty());
}
std::string bytes(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input);
    return {std::istreambuf_iterator<char>{input}, {}};
}
void write(const fs::path& path, std::string_view value) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    output.close();
    REQUIRE(output);
}
struct Fixture {
    fs::path parent{fs::current_path()};
    fs::path root{parent / ("shark-migration-" + take(generate_stable_id()).to_string())};
    fs::path source{root / "live" / fs::path{u8"World \u4e16\u754c.db"}};
    fs::path backup{root / "backup" / fs::path{u8"Snapshot caf\u00e9.db"}};
    fs::path restored{root / "restored" / "candidate.db"};
    explicit Fixture(int version = 3) {
        REQUIRE(root.is_absolute());
        REQUIRE(root.parent_path() == parent);
        REQUIRE(fs::create_directory(root));
        for (const auto* directory : {"live", "backup", "restored", "upgrade-backup"})
            REQUIRE(fs::create_directory(root / directory));
        fixture_data::create(source, version, SHARK_STORAGE_FIXTURE_DIRECTORY);
    }
    ~Fixture() {
        if (root.is_absolute() && root.parent_path() == parent &&
            root.filename().string().starts_with("shark-migration-")) {
            std::error_code error;
            fs::remove_all(root, error);
        }
    }
    auto inspect(const fs::path& path) const {
        return Store::inspect_snapshot(path, fixture_data::store_id(), fixture_data::world());
    }
    SnapshotInfo copy() const {
        return take(Store::backup(source, backup, fixture_data::store_id(), fixture_data::world()));
    }
    auto migrate() const {
        return Store::migrate(source, backup, fixture_data::store_id(), fixture_data::world());
    }
    auto restore() const {
        return Store::restore(backup, restored, fixture_data::store_id(), fixture_data::world());
    }
    auto open(const fs::path& path) const {
        return take(Store::open(path, fixture_data::world()));
    }
};

thread_local std::optional<detail::MaintenancePhase> failing_phase;
thread_local std::size_t phase_visits{};
struct Fault {
    explicit Fault(detail::MaintenancePhase phase) {
        failing_phase = phase;
        phase_visits = 0;
        detail::set_maintenance_fault_hook([](detail::MaintenancePhase current) noexcept {
            if (failing_phase == current) {
                ++phase_visits;
                return true;
            }
            return false;
        });
    }
    ~Fault() {
        detail::set_maintenance_fault_hook(nullptr);
        failing_phase.reset();
    }
};

std::string pem(const fs::path& name) {
    return bytes(fs::path{SHARK_NETWORK_FIXTURE_DIRECTORY} / name);
}
struct Pair {
    std::unique_ptr<TlsListener> listener;
    std::unique_ptr<TlsTransport> client;
    std::unique_ptr<TlsTransport> server;
    explicit Pair(bool second = false) {
        listener = take(TlsListener::listen_loopback_authenticated(
            0, {pem("server.pem"), pem("server-key.pem")}, pem("ca.pem")));
        client = take(TlsTransport::connect_loopback_authenticated(
            take(listener->port()), {pem("ca.pem"), "shark.test"},
            {pem(second ? "session/client-b.pem" : "session/client-a.pem"),
             pem(second ? "session/client-b-key.pem" : "session/client-a-key.pem")},
            0ns));
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            if (std::chrono::steady_clock::now() >= deadline)
                FAIL("migration fixture mutual TLS readiness deadline exceeded");
            if (!server) {
                auto accepted = take(listener->accept(0ns));
                if (accepted)
                    server = std::move(*accepted);
            }
            take(client->poll(0ns));
            if (server) {
                take(server->poll(0ns));
                if (take(client->stats()).state == TransportState::ready &&
                    take(server->stats()).state == TransportState::ready)
                    break;
            }
            std::this_thread::yield();
        }
    }
};
SessionEnvelope envelope(const SessionSnapshot& session, std::uint64_t sequence) {
    return {session.session, session.ownership_epoch, sequence, session.character};
}
SessionSnapshot login(CharacterAuthority& owner, Pair& pair) {
    const auto session = take(owner.apply(
        *pair.server, LoginRequest{fixture_data::account(), fixture_data::character()}, 0ns));
    return take(owner.apply(*pair.server, SetReadyRequest{envelope(session, 1), true}, 0ns));
}
} // namespace

TEST_CASE("independent historical and current snapshots inspect without changing bytes",
          "[storage-migration]") {
    for (const int version : {1, 2, 3}) {
        CAPTURE(version);
        Fixture fixture{version};
        const auto before = bytes(fixture.source);
        const auto info = take(fixture.inspect(fixture.source));
        CHECK(info.metadata == StoreMetadata{fixture_data::store_id(), fixture_data::world(),
                                             static_cast<std::uint32_t>(version)});
        CHECK(info.bytes == before.size());
        CHECK(info.bytes <= maximum_store_bytes);
        CHECK(info.accounts == (version == 1 ? 0U : 3U));
        CHECK(info.certificates == (version == 1 ? 0U : 4U));
        CHECK(info.characters == (version == 1 ? 0U : 3U));
        CHECK(info.economies == (version == 3 ? 2U : 0U));
        CHECK(info.receipts == (version == 3 ? 8U : 0U));
        CHECK(bytes(fixture.source) == before);
    }
}

TEST_CASE("ordinary open continues to reject actual legacy databases without migration",
          "[storage-migration]") {
    for (const int version : {1, 2}) {
        Fixture fixture{version};
        const auto before = bytes(fixture.source);
        reject(Store::open(fixture.source, fixture_data::world()));
        CHECK(bytes(fixture.source) == before);
        CHECK(take(fixture.inspect(fixture.source)).metadata.schema_version ==
              static_cast<std::uint32_t>(version));
    }
}

TEST_CASE("sequential database upgrades preserve full historical records without economic grants",
          "[storage-migration]") {
    for (const int version : {1, 2}) {
        CAPTURE(version);
        Fixture fixture{version};
        const auto original = fixture_data::snapshot(fixture.source, version);
        const auto backup_info = fixture.copy();
        const auto backup_bytes = bytes(fixture.backup);
        CHECK(fixture_data::snapshot(fixture.backup, version) == original);
        const auto result = take(fixture.migrate());
        CHECK(result.before == backup_info.metadata);
        CHECK(result.after == StoreMetadata{fixture_data::store_id(), fixture_data::world(), 3});
        CHECK_FALSE(result.already_current);
        auto owner = fixture.open(fixture.source);
        fixture_data::verify_current(*owner, version);
        CHECK(bytes(fixture.backup) == backup_bytes);
    }
}

TEST_CASE("committed migration retries use the original backup and reject subsequent writes",
          "[storage-migration]") {
    Fixture fixture{2};
    const auto backup_info = fixture.copy();
    take(fixture.migrate());
    const auto before_retry = bytes(fixture.source);
    const auto retried = take(fixture.migrate());
    CHECK(retried.already_current);
    CHECK(retried.before == StoreMetadata{backup_info.metadata.id, backup_info.metadata.world, 3});
    CHECK(bytes(fixture.source) == before_retry);
    {
        auto owner = fixture.open(fixture.source);
        const auto record =
            take(owner->character(fixture_data::account(), fixture_data::character()));
        auto state = record.state;
        state.name = Name{"After migration"};
        take(owner->save_character(record.account, record.character, record.revision, state));
    }
    const auto later = bytes(fixture.source);
    reject(fixture.migrate());
    CHECK(bytes(fixture.source) == later);
    CHECK(take(fixture.inspect(fixture.backup)).metadata.schema_version == 2);
}

TEST_CASE("migration requires an existing complete backup before changing the source",
          "[storage-migration]") {
    Fixture fixture{2};
    const auto before = bytes(fixture.source);
    reject(fixture.migrate());
    CHECK(bytes(fixture.source) == before);
    CHECK_FALSE(fs::exists(fixture.backup));
    write(fixture.backup, "incomplete backup");
    reject(fixture.migrate());
    CHECK(bytes(fixture.source) == before);
    CHECK(bytes(fixture.backup) == "incomplete backup");
}

TEST_CASE("matching database identities do not excuse a different backup logical state",
          "[storage-migration]") {
    for (const auto* sql : {"UPDATE shark_characters SET name='Different backup' WHERE revision=7",
                            "UPDATE shark_accounts SET revoked=1 WHERE roster_revision=6",
                            "UPDATE shark_certificates SET revoked=1 WHERE revoked=0"}) {
        Fixture fixture{2};
        fixture.copy();
        fixture_data::execute(fixture.backup, sql);
        const auto source_bytes = bytes(fixture.source);
        const auto backup_bytes = bytes(fixture.backup);
        reject(fixture.migrate());
        CHECK(bytes(fixture.source) == source_bytes);
        CHECK(bytes(fixture.backup) == backup_bytes);
    }
}

TEST_CASE("current backup and isolated restore retain complete economic history and expired ids",
          "[storage-migration]") {
    Fixture fixture;
    const auto original = fixture_data::snapshot(fixture.source, 3);
    const auto copied = fixture.copy();
    const auto restored = take(fixture.restore());
    CHECK(restored.metadata == copied.metadata);
    CHECK(fixture_data::snapshot(fixture.backup, 3) == original);
    CHECK(fixture_data::snapshot(fixture.restored, 3) == original);
    auto owner = fixture.open(fixture.restored);
    fixture_data::verify_current(*owner, 3);
    const auto retry = take(owner->transfer(fixture_data::request(9)));
    CHECK(retry.replayed);
    CHECK(retry.receipt == fixture_data::expected_stream().receipts.back());
    reject(owner->transfer(fixture_data::request(1)));
    fixture_data::verify_current(*owner, 3);
}

TEST_CASE("legacy restoration retains its schema until explicit migration of the candidate",
          "[storage-migration]") {
    for (const int version : {1, 2}) {
        Fixture fixture{version};
        fixture.copy();
        const auto restored = take(fixture.restore());
        CHECK(restored.metadata.schema_version == static_cast<std::uint32_t>(version));
        reject(Store::open(fixture.restored, fixture_data::world()));
        const auto second_backup = fixture.root / "upgrade-backup" / "candidate-before-upgrade.db";
        take(Store::backup(fixture.restored, second_backup, fixture_data::store_id(),
                           fixture_data::world()));
        take(Store::migrate(fixture.restored, second_backup, fixture_data::store_id(),
                            fixture_data::world()));
        auto owner = fixture.open(fixture.restored);
        fixture_data::verify_current(*owner, version);
        CHECK(take(fixture.inspect(fixture.source)).metadata.schema_version ==
              static_cast<std::uint32_t>(version));
    }
}

TEST_CASE("current schema migration reconciliation requires full economic equality",
          "[storage-migration]") {
    Fixture fixture;
    fixture.copy();
    const auto result = take(fixture.migrate());
    CHECK(result.already_current);
    {
        auto owner = fixture.open(fixture.source);
        auto request = fixture_data::request(9);
        request.operation.sequence = 10;
        request.expected_source_revision = 3;
        request.expected_target_revision = 3;
        CHECK(take(owner->transfer(request)).receipt.status == TransferStatus::committed);
    }
    const auto before = bytes(fixture.source);
    reject(fixture.migrate());
    CHECK(bytes(fixture.source) == before);
}

TEST_CASE("injected migration failures roll back every uncommitted sequential step",
          "[storage-migration]") {
    for (const auto phase :
         {detail::MaintenancePhase::after_v1_to_v2, detail::MaintenancePhase::after_v2_to_v3,
          detail::MaintenancePhase::before_migration_commit}) {
        CAPTURE(static_cast<int>(phase));
        Fixture fixture{1};
        const auto original = fixture_data::snapshot(fixture.source, 1);
        fixture.copy();
        {
            Fault fault{phase};
            reject(fixture.migrate());
            CHECK(phase_visits == 1);
        }
        CHECK(fixture_data::snapshot(fixture.source, 1) == original);
        take(fixture.migrate());
        auto owner = fixture.open(fixture.source);
        fixture_data::verify_current(*owner, 1);
    }
}

TEST_CASE("lost migration acknowledgement reconciles the real committed outcome",
          "[storage-migration]") {
    Fixture fixture{2};
    fixture.copy();
    {
        Fault fault{detail::MaintenancePhase::after_migration_commit};
        reject(fixture.migrate());
        CHECK(phase_visits == 1);
    }
    CHECK(take(fixture.inspect(fixture.source)).metadata.schema_version == 3);
    CHECK(take(fixture.migrate()).already_current);
    auto owner = fixture.open(fixture.source);
    fixture_data::verify_current(*owner, 2);
}

TEST_CASE("backup failures preserve source and never authorize overwrite retries",
          "[storage-migration]") {
    for (const auto phase :
         {detail::MaintenancePhase::backup_step, detail::MaintenancePhase::backup_complete,
          detail::MaintenancePhase::backup_validated}) {
        Fixture fixture;
        const auto original = fixture_data::snapshot(fixture.source, 3);
        {
            Fault fault{phase};
            reject(Store::backup(fixture.source, fixture.backup, fixture_data::store_id(),
                                 fixture_data::world()));
            CHECK(phase_visits == 1);
        }
        CHECK(fixture_data::snapshot(fixture.source, 3) == original);
        REQUIRE(fs::exists(fixture.backup));
        const auto partial = bytes(fixture.backup);
        reject(Store::backup(fixture.source, fixture.backup, fixture_data::store_id(),
                             fixture_data::world()));
        CHECK(bytes(fixture.backup) == partial);
    }
}

TEST_CASE("restore failures retain the backup and leave outputs for explicit inspection",
          "[storage-migration]") {
    for (const auto phase :
         {detail::MaintenancePhase::restore_step, detail::MaintenancePhase::restore_complete,
          detail::MaintenancePhase::restore_validated}) {
        Fixture fixture;
        fixture.copy();
        const auto original = bytes(fixture.backup);
        {
            Fault fault{phase};
            reject(fixture.restore());
            CHECK(phase_visits == 1);
        }
        CHECK(bytes(fixture.backup) == original);
        REQUIRE(fs::exists(fixture.restored));
        const auto candidate = bytes(fixture.restored);
        reject(fixture.restore());
        CHECK(bytes(fixture.restored) == candidate);
    }
}

TEST_CASE("future databases reject all maintenance before recovery or modification",
          "[storage-migration]") {
    Fixture fixture;
    fixture.copy();
    fixture_data::execute(fixture.source, "PRAGMA user_version=4");
    const auto source = bytes(fixture.source);
    const auto backup = bytes(fixture.backup);
    reject(fixture.inspect(fixture.source));
    reject(fixture.migrate());
    reject(Store::backup(fixture.source, fixture.restored, fixture_data::store_id(),
                         fixture_data::world()));
    reject(Store::restore(fixture.source, fixture.restored, fixture_data::store_id(),
                          fixture_data::world()));
    CHECK(bytes(fixture.source) == source);
    CHECK(bytes(fixture.backup) == backup);
    CHECK_FALSE(fs::exists(fixture.restored));
}

TEST_CASE("maintenance rejects expected identity mismatches without publishing output",
          "[storage-migration]") {
    Fixture fixture{2};
    fixture.copy();
    const auto original = bytes(fixture.source);
    const auto other_store = StoreId{fixture_data::identity(701)};
    const auto other_world = WorldId{fixture_data::identity(901)};
    for (const bool wrong_store : {false, true}) {
        const auto store = wrong_store ? other_store : fixture_data::store_id();
        const auto world = wrong_store ? fixture_data::world() : other_world;
        reject(Store::inspect_snapshot(fixture.source, store, world));
        reject(Store::backup(fixture.source, fixture.restored, store, world));
        reject(Store::migrate(fixture.source, fixture.backup, store, world));
        reject(Store::restore(fixture.backup, fixture.restored, store, world));
        CHECK(bytes(fixture.source) == original);
        CHECK_FALSE(fs::exists(fixture.restored));
    }
}

TEST_CASE("malformed historical schemas and records never become upgraded databases",
          "[storage-migration]") {
    for (const auto* sql : {"CREATE TABLE unexpected(value INTEGER)",
                            "PRAGMA ignore_check_constraints=ON; UPDATE shark_characters SET "
                            "name='' WHERE revision=7",
                            "PRAGMA ignore_check_constraints=ON; UPDATE shark_accounts SET "
                            "revoked=2 WHERE roster_revision=6"}) {
        Fixture fixture{2};
        fixture.copy();
        fixture_data::execute(fixture.source, sql);
        const auto original = bytes(fixture.source);
        reject(fixture.inspect(fixture.source));
        reject(fixture.migrate());
        reject(Store::backup(fixture.source, fixture.restored, fixture_data::store_id(),
                             fixture_data::world()));
        CHECK(bytes(fixture.source) == original);
        CHECK_FALSE(fs::exists(fixture.restored));
    }
}

TEST_CASE("missing economic history invalidates a backup even with intact schema and counts",
          "[storage-migration]") {
    Fixture fixture;
    fixture.copy();
    fixture_data::execute(fixture.backup,
                          "UPDATE shark_transfer_receipts SET sequence=1 WHERE sequence=2");
    const auto original = bytes(fixture.backup);
    reject(fixture.inspect(fixture.backup));
    reject(fixture.restore());
    reject(fixture.migrate());
    CHECK(bytes(fixture.backup) == original);
    CHECK_FALSE(fs::exists(fixture.restored));
}

TEST_CASE("create-only maintenance outputs never replace existing paths or journals",
          "[storage-migration]") {
    Fixture fixture;
    fixture.copy();
    write(fixture.restored, "existing destination");
    reject(fixture.restore());
    reject(Store::backup(fixture.source, fixture.restored, fixture_data::store_id(),
                         fixture_data::world()));
    CHECK(bytes(fixture.restored) == "existing destination");
    const auto fresh = fixture.restored.parent_path() / "fresh.db";
    auto journal = fresh;
    journal += "-journal";
    write(journal, "existing journal");
    reject(Store::restore(fixture.backup, fresh, fixture_data::store_id(), fixture_data::world()));
    CHECK_FALSE(fs::exists(fresh));
    CHECK(bytes(journal) == "existing journal");
}

TEST_CASE("maintenance enforces distinct canonical directories and regular single-link inputs",
          "[storage-migration]") {
    Fixture fixture;
    fixture.copy();
    reject(Store::backup(fixture.source, fixture.source.parent_path() / "." / "same.db",
                         fixture_data::store_id(), fixture_data::world()));
    reject(Store::restore(fixture.backup, fixture.backup.parent_path() / "same.db",
                          fixture_data::store_id(), fixture_data::world()));
    reject(Store::migrate(fixture.source, fixture.source, fixture_data::store_id(),
                          fixture_data::world()));
    reject(Store::backup("relative.db", fixture.restored, fixture_data::store_id(),
                         fixture_data::world()));
    const auto alias = fixture.root / "source-alias.db";
    fs::create_hard_link(fixture.source, alias);
    reject(fixture.inspect(alias));
    reject(Store::backup(alias, fixture.restored, fixture_data::store_id(), fixture_data::world()));
    CHECK_FALSE(fs::exists(fixture.restored));
}

TEST_CASE("active database ownership excludes maintenance without changing durable state",
          "[storage-migration]") {
    Fixture fixture;
    fixture.copy();
    auto owner = fixture.open(fixture.source);
    reject(fixture.inspect(fixture.source));
    reject(fixture.migrate());
    reject(Store::backup(fixture.source, fixture.restored, fixture_data::store_id(),
                         fixture_data::world()));
    fixture_data::verify_current(*owner, 3);
    CHECK_FALSE(fs::exists(fixture.restored));
}

TEST_CASE("old snapshot restoration is isolated and does not rewind the active store",
          "[storage-migration]") {
    Fixture fixture;
    fixture.copy();
    {
        auto owner = fixture.open(fixture.source);
        auto request = fixture_data::request(9);
        request.operation.sequence = 10;
        request.expected_source_revision = 3;
        request.expected_target_revision = 3;
        take(owner->transfer(request));
        take(owner->revoke_account(fixture_data::account(2)));
    }
    const auto current = fixture_data::snapshot(fixture.source, 3);
    take(fixture.restore());
    CHECK(fixture_data::snapshot(fixture.source, 3) == current);
    {
        auto candidate = fixture.open(fixture.restored);
        fixture_data::verify_current(*candidate, 3);
        CHECK(take(candidate->operation_stream(fixture_data::account())).high_watermark == 9);
    }
    auto live = fixture.open(fixture.source);
    CHECK(take(live->operation_stream(fixture_data::account())).high_watermark == 10);
    CHECK(take(live->registry()).accounts[1].revoked);
}

TEST_CASE("migrated characters resume through actual authenticated owner admission",
          "[storage-migration]") {
    Fixture fixture{2};
    fixture.copy();
    take(fixture.migrate());
    auto owner = take(CharacterAuthority::create(fixture.open(fixture.source)));
    Pair pair;
    const auto session = login(*owner, pair);
    const auto loaded = take(owner->load(*pair.server, envelope(session, 2), 0ns));
    CHECK(loaded == fixture_data::expected_registry().characters[0]);
    reject(owner->economy(*pair.server, envelope(session, 3), 0ns));
    reject(owner->apply(*pair.server,
                        LoginRequest{fixture_data::account(3), fixture_data::character(103)}, 0ns));
}

TEST_CASE("same-point restored restart requires fresh login and retains exact retry outcomes",
          "[storage-migration]") {
    Fixture fixture;
    Pair original_pair;
    std::optional<SessionSnapshot> old_session;
    {
        auto owner = take(CharacterAuthority::create(fixture.open(fixture.source)));
        old_session = login(*owner, original_pair);
        CHECK(take(owner->transfer(*original_pair.server, envelope(*old_session, 2),
                                   fixture_data::request(9), 0ns))
                  .replayed);
    }
    fixture.copy();
    take(fixture.restore());
    auto owner = take(CharacterAuthority::open_runtime(fixture.restored, fixture_data::store_id(),
                                                       fixture_data::world()));
    reject(owner->load(*original_pair.server, envelope(*old_session, 3), 0ns));
    Pair fresh_pair;
    const auto current_session = login(*owner, fresh_pair);
    CHECK(current_session.session != old_session->session);
    const auto loaded = take(owner->load(*fresh_pair.server, envelope(current_session, 2), 0ns));
    CHECK(loaded == fixture_data::expected_registry().characters[0]);
    const auto retry = take(owner->transfer(*fresh_pair.server, envelope(current_session, 3),
                                            fixture_data::request(9), 0ns));
    CHECK(retry.replayed);
    CHECK(retry.receipt == fixture_data::expected_stream().receipts.back());
    reject(owner->transfer(*fresh_pair.server, envelope(current_session, 4),
                           fixture_data::request(1), 0ns));
    CHECK(take(owner->operation_stream(*fresh_pair.server, envelope(current_session, 5), 0ns)) ==
          fixture_data::expected_stream());
}

TEST_CASE("migration preserves actual certificate and account revocation during fresh login",
          "[storage-migration]") {
    for (const bool account_revoked : {false, true}) {
        Fixture fixture{2};
        fixture_data::execute(fixture.source,
                              account_revoked
                                  ? "UPDATE shark_accounts SET revoked=1 WHERE roster_revision=4"
                                  : "UPDATE shark_certificates SET revoked=1 WHERE "
                                    "account_id='00000000-0000-4000-8000-000000000001'");
        fixture.copy();
        take(fixture.migrate());
        auto owner = take(CharacterAuthority::create(fixture.open(fixture.source)));
        Pair pair{account_revoked};
        const auto rejected =
            owner->apply(*pair.server,
                         LoginRequest{fixture_data::account(account_revoked ? 2U : 1U),
                                      fixture_data::character(account_revoked ? 102U : 101U)},
                         0ns);
        reject(rejected);
    }
}

TEST_CASE("readonly inspection and restore preserve inert journal bytes", "[storage-migration]") {
    Fixture fixture;
    const auto summary = fixture.copy();
    auto journal = fixture.backup;
    journal += "-journal";
    const std::string inert_journal(512, '\0');
    write(journal, inert_journal);
    const auto original = bytes(fixture.backup);
    CHECK(take(fixture.inspect(fixture.backup)) == summary);
    CHECK(take(fixture.restore()).metadata == summary.metadata);
    CHECK(bytes(fixture.backup) == original);
    CHECK(fs::exists(journal));
    CHECK(bytes(journal) == inert_journal);
    auto owner = fixture.open(fixture.restored);
    fixture_data::verify_current(*owner, 3);
}
