#include <shark/storage/characters.hpp>

#include "store_test_hooks.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

namespace {
using namespace shark;
using namespace shark::net;
using namespace shark::storage;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

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
WorldId world() {
    return WorldId{take(StableId::parse("00000000-0000-4000-8000-000000000001"))};
}
AccountId account(bool second = false) {
    return AccountId{take(StableId::parse(second ? "00000000-0000-4000-8000-000000000102"
                                                 : "00000000-0000-4000-8000-000000000101"))};
}
CharacterState state(bool changed = false) {
    return {
        Name{changed ? "Changed character" : "First character"},
        {world(), {changed ? 123456 : -123456, 17, -4}, {changed ? 512.25 : 0.125, 42.5, 1023.75}}};
}
std::string pem(const fs::path& name) {
    std::ifstream input{fs::path{SHARK_NETWORK_FIXTURE_DIRECTORY} / name, std::ios::binary};
    REQUIRE(input);
    return {std::istreambuf_iterator<char>{input}, {}};
}
CertificateFingerprint certificate(bool second = false) {
    return take(
        certificate_fingerprint(pem(second ? "session/client-b.pem" : "session/client-a.pem")));
}
SessionLimits limits() {
    SessionLimits result;
    result.auth_attempts_per_window = 128;
    result.commands_per_window = 128;
    return result;
}

struct Pair {
    std::unique_ptr<TlsListener> listener;
    std::unique_ptr<TlsTransport> client;
    std::unique_ptr<TlsTransport> server;
    explicit Pair(bool second = false, bool authenticated = true) {
        const ServerIdentity identity{pem("server.pem"), pem("server-key.pem")};
        const ClientTrust trust{pem("ca.pem"), "shark.test"};
        listener = take(authenticated
                            ? TlsListener::listen_loopback_authenticated(0, identity, pem("ca.pem"))
                            : TlsListener::listen_loopback(0, identity));
        const auto port = take(listener->port());
        client =
            take(authenticated
                     ? TlsTransport::connect_loopback_authenticated(
                           port, trust,
                           {pem(second ? "session/client-b.pem" : "session/client-a.pem"),
                            pem(second ? "session/client-b-key.pem" : "session/client-a-key.pem")},
                           0ns)
                     : TlsTransport::connect_loopback(port, trust, 0ns));
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
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

struct Fixture {
    fs::path parent{fs::current_path()};
    fs::path root{parent / ("shark-characters-" + take(generate_stable_id()).to_string())};
    fs::path path{root / fs::path{u8"Characters \u4e16\u754c.db"}};
    std::unique_ptr<CharacterAuthority> service;
    explicit Fixture(const SessionLimits& configuration = limits()) {
        REQUIRE(root.is_absolute());
        REQUIRE(root.parent_path() == parent);
        REQUIRE(fs::create_directory(root));
        service =
            take(CharacterAuthority::create(take(Store::create(path, world())), configuration));
        for (const bool second : {false, true}) {
            take(service->provision_account(account(second)));
            take(service->enroll_certificate(account(second), certificate(second)));
        }
    }
    ~Fixture() {
        service.reset();
        if (root.is_absolute() && root.parent_path() == parent &&
            root.filename().string().starts_with("shark-characters-")) {
            std::error_code error;
            fs::remove_all(root, error);
        }
    }
    void reopen(const SessionLimits& configuration = limits()) {
        service.reset();
        service = take(CharacterAuthority::create(take(Store::open(path, world())), configuration));
    }
    CharacterRecord create(Pair& pair, bool second = false) {
        const auto roster = take(service->roster(*pair.server, account(second), 0ns));
        return take(service->create_character(*pair.server, account(second), roster.revision,
                                              state(), 0ns));
    }
};

SessionEnvelope envelope(const SessionSnapshot& session, std::uint64_t sequence) {
    return {session.session, session.ownership_epoch, sequence, session.character};
}
SessionSnapshot login(Fixture& fixture, Pair& pair, const CharacterRecord& record,
                      bool ready = true) {
    auto session = take(
        fixture.service->apply(*pair.server, LoginRequest{record.account, record.character}, 0ns));
    if (ready)
        session = take(
            fixture.service->apply(*pair.server, SetReadyRequest{envelope(session, 1), true}, 0ns));
    return session;
}
static_assert(!std::is_copy_constructible_v<CharacterAuthority>);
static_assert(!std::is_move_constructible_v<CharacterAuthority>);
static_assert(!std::is_convertible_v<CharacterId, WorldEntityId>);
static_assert(!std::is_convertible_v<AuthoredEntityId, CharacterId>);

class AfterCommitFault final {
  public:
    AfterCommitFault() {
        detail::set_write_fault_hook([](detail::WritePhase phase) noexcept {
            return phase == detail::WritePhase::after_commit;
        });
    }
    ~AfterCommitFault() {
        detail::set_write_fault_hook(nullptr);
    }
    AfterCommitFault(const AfterCommitFault&) = delete;
    AfterCommitFault& operator=(const AfterCommitFault&) = delete;
};

void frozen(Fixture& fixture, Pair& pair, const SessionSnapshot& session) {
    reject(fixture.service->metadata());
    reject(fixture.service->roster(*pair.server, account(), 0ns));
    reject(fixture.service->create_character(*pair.server, account(), 2, state(), 0ns));
    reject(fixture.service->apply(*pair.server, HeartbeatRequest{envelope(session, 3)}, 0ns));
    reject(fixture.service->load(*pair.server, envelope(session, 3), 0ns));
    reject(fixture.service->save(*pair.server, envelope(session, 3), 1, state(), 0ns));
    Pair fresh;
    reject(fixture.service->roster(*fresh.server, account(), 0ns));
    reject(fixture.service->apply(*fresh.server, LoginRequest{account(), session.character}, 0ns));
}
} // namespace

TEST_CASE("authenticated character creation save and restart preserve complete durable identity",
          "[storage-characters]") {
    Fixture fixture;
    Pair a;
    Pair b{true};
    const auto metadata = take(fixture.service->metadata());
    const auto first = fixture.create(a);
    const auto other = fixture.create(b, true);
    CHECK(first.character != other.character);
    CHECK(first.entity != other.entity);
    CHECK(first.revision == 1);
    const auto session = login(fixture, a, first);
    CHECK(take(fixture.service->load(*a.server, envelope(session, 2), 0ns)) == first);
    const auto saved = take(
        fixture.service->save(*a.server, envelope(session, 3), first.revision, state(true), 0ns));
    CHECK(saved.character == first.character);
    CHECK(saved.entity == first.entity);
    CHECK(saved.account == first.account);
    CHECK(saved.revision == 2);
    CHECK(saved.state == state(true));
    fixture.reopen();
    Pair next;
    CHECK(take(fixture.service->metadata()) == metadata);
    const auto roster = take(fixture.service->roster(*next.server, account(), 0ns));
    REQUIRE(roster.characters.size() == 1);
    CHECK(roster.characters.front() == saved);
    const auto replacement = login(fixture, next, saved);
    CHECK(replacement.session != session.session);
    reject(fixture.service->load(*next.server, envelope(session, 4), 0ns));
    CHECK(take(fixture.service->load(*next.server, envelope(replacement, 2), 0ns)) == saved);
    Pair next_b{true};
    CHECK(take(fixture.service->roster(*next_b.server, account(true), 0ns)).characters ==
          std::vector{other});
}

TEST_CASE("character roster comparison prevents duplicate creation from a stale snapshot",
          "[storage-characters]") {
    Fixture fixture;
    Pair pair;
    const auto empty = take(fixture.service->roster(*pair.server, account(), 0ns));
    CHECK(empty.revision == 1);
    CHECK(empty.characters.empty());
    const auto first = take(
        fixture.service->create_character(*pair.server, account(), empty.revision, state(), 0ns));
    reject(
        fixture.service->create_character(*pair.server, account(), empty.revision, state(), 0ns));
    const auto one = take(fixture.service->roster(*pair.server, account(), 0ns));
    CHECK(one.revision == empty.revision + 1);
    REQUIRE(one.characters == std::vector{first});
    const auto second = take(
        fixture.service->create_character(*pair.server, account(), one.revision, state(), 0ns));
    CHECK(second.character != first.character);
    CHECK(second.entity != first.entity);
    CHECK(take(fixture.service->roster(*pair.server, account(), 0ns)).revision == one.revision + 1);
}

TEST_CASE("character admission rejects foreign and nonauthenticated transport provenance",
          "[storage-characters]") {
    Fixture fixture;
    Pair a;
    Pair b{true};
    Pair diagnostic{false, false};
    const auto first = fixture.create(a);
    const auto other = fixture.create(b, true);
    reject(fixture.service->roster(*b.server, account(), 0ns));
    reject(fixture.service->create_character(*b.server, account(), 2, state(), 0ns));
    reject(fixture.service->roster(*a.client, account(), 0ns));
    reject(fixture.service->roster(*diagnostic.server, account(), 0ns));
    reject(fixture.service->create_character(*diagnostic.server, account(), 2, state(), 0ns));
    reject(fixture.service->apply(*b.server, LoginRequest{account(), first.character}, 0ns));
    reject(fixture.service->apply(*b.server, LoginRequest{account(true), first.character}, 0ns));
    const auto session = login(fixture, b, other);
    auto forged = envelope(session, 2);
    forged.character = first.character;
    reject(fixture.service->load(*b.server, forged, 0ns));
    reject(fixture.service->save(*b.server, forged, first.revision, state(true), 0ns));
    CHECK(take(fixture.service->load(*b.server, envelope(session, 2), 0ns)) == other);
    CHECK(take(fixture.service->roster(*a.server, account(), 0ns)).characters ==
          std::vector{first});
}

TEST_CASE("character load and save require readiness and exact session sequence",
          "[storage-characters]") {
    Fixture fixture;
    Pair pair;
    const auto record = fixture.create(pair);
    const auto session = login(fixture, pair, record, false);
    reject(fixture.service->load(*pair.server, envelope(session, 1), 0ns));
    reject(fixture.service->save(*pair.server, envelope(session, 1), 1, state(true), 0ns));
    take(fixture.service->apply(*pair.server, SetReadyRequest{envelope(session, 1), true}, 0ns));
    CHECK(take(fixture.service->load(*pair.server, envelope(session, 2), 0ns)) == record);
    reject(fixture.service->load(*pair.server, envelope(session, 2), 0ns));
    reject(fixture.service->save(*pair.server, envelope(session, 4), 1, state(true), 0ns));
    auto wrong_epoch = envelope(session, 3);
    ++wrong_epoch.ownership_epoch;
    reject(fixture.service->save(*pair.server, wrong_epoch, 1, state(true), 0ns));
    CHECK(take(fixture.service->save(*pair.server, envelope(session, 3), 1, state(true), 0ns))
              .revision == 2);
}

TEST_CASE(
    "character state rejection preserves sequence while admitted revision conflicts consume it",
    "[storage-characters]") {
    Fixture fixture;
    Pair pair;
    const auto record = fixture.create(pair);
    const auto session = login(fixture, pair, record);
    auto invalid = state(true);
    SECTION("name") {
        invalid.name.value = std::string(maximum_name_bytes + 1, 'x');
    }
    SECTION("nonfinite") {
        invalid.position.offset[0] = std::numeric_limits<double>::infinity();
    }
    SECTION("noncanonical") {
        invalid.position.offset[0] = world_cell_size_meters;
    }
    SECTION("world") {
        invalid.position.world = WorldId{account().value()};
    }
    reject(fixture.service->save(*pair.server, envelope(session, 2), 1, invalid, 0ns));
    reject(fixture.service->save(*pair.server, envelope(session, 2), 0, state(true), 0ns));
    reject(fixture.service->load(*pair.server, envelope(session, 2), 0ns));
    CHECK(take(fixture.service->load(*pair.server, envelope(session, 3), 0ns)) == record);
    const auto saved =
        take(fixture.service->save(*pair.server, envelope(session, 4), 1, state(true), 0ns));
    reject(fixture.service->save(*pair.server, envelope(session, 5), 1, state(), 0ns));
    CHECK(take(fixture.service->load(*pair.server, envelope(session, 6), 0ns)) == saved);
}

TEST_CASE("admitted character revision conflicts spend the shared command budget",
          "[storage-characters]") {
    auto configuration = limits();
    configuration.commands_per_window = 3;
    Fixture fixture{configuration};
    Pair pair;
    const auto record = fixture.create(pair);
    const auto session = login(fixture, pair, record);
    reject(fixture.service->save(*pair.server, envelope(session, 2), 0, state(true), 0ns));
    reject(fixture.service->save(*pair.server, envelope(session, 3), 0, state(true), 0ns));
    reject(fixture.service->load(*pair.server, envelope(session, 4), 0ns));
    fixture.service.reset();
    CHECK(take(take(Store::open(fixture.path, world()))->character(account(), record.character)) ==
          record);
}

TEST_CASE("replacement character sessions fence old transports and copied envelopes",
          "[storage-characters]") {
    Fixture fixture;
    Pair first;
    const auto record = fixture.create(first);
    const auto old = login(fixture, first, record);
    Pair next;
    const auto current = login(fixture, next, record);
    CHECK(current.session != old.session);
    CHECK(current.ownership_epoch > old.ownership_epoch);
    reject(fixture.service->roster(*first.server, account(), 0ns));
    reject(fixture.service->create_character(*first.server, account(), 2, state(), 0ns));
    reject(fixture.service->save(*first.server, envelope(old, 2), 1, state(true), 0ns));
    reject(fixture.service->save(*next.server, envelope(old, 2), 1, state(true), 0ns));
    CHECK_FALSE(take(fixture.service->revoke_session(old.session)));
    CHECK(take(fixture.service->save(*next.server, envelope(current, 2), 1, state(true), 0ns))
              .revision == 2);
}

TEST_CASE("logout and exact session revocation cannot authorize later character writes",
          "[storage-characters]") {
    Fixture fixture;
    Pair first;
    const auto record = fixture.create(first);
    const auto old = login(fixture, first, record);
    SECTION("logout") {
        take(fixture.service->apply(*first.server, LogoutRequest{envelope(old, 2)}, 0ns));
    }
    SECTION("revocation") {
        CHECK(take(fixture.service->revoke_session(old.session)));
        CHECK_FALSE(take(fixture.service->revoke_session(old.session)));
    }
    reject(fixture.service->load(*first.server, envelope(old, 3), 0ns));
    reject(fixture.service->save(*first.server, envelope(old, 3), 1, state(true), 0ns));
    Pair next;
    const auto current = login(fixture, next, record);
    CHECK(take(fixture.service->load(*next.server, envelope(current, 2), 0ns)) == record);
}

TEST_CASE("account and certificate revocation survive character service reconstruction",
          "[storage-characters]") {
    Fixture fixture;
    Pair first;
    const auto record = fixture.create(first);
    const auto session = login(fixture, first, record);
    bool revoked_account = false;
    SECTION("account") {
        revoked_account = true;
        take(fixture.service->revoke_account(account()));
    }
    SECTION("certificate") {
        take(fixture.service->revoke_certificate(certificate()));
    }
    reject(fixture.service->load(*first.server, envelope(session, 2), 0ns));
    reject(fixture.service->save(*first.server, envelope(session, 2), 1, state(true), 0ns));
    fixture.reopen();
    Pair next;
    reject(fixture.service->roster(*next.server, account(), 0ns));
    reject(fixture.service->create_character(*next.server, account(), 2, state(true), 0ns));
    reject(fixture.service->apply(*next.server, LoginRequest{account(), record.character}, 0ns));
    if (revoked_account)
        reject(fixture.service->provision_account(account()));
    else
        reject(fixture.service->enroll_certificate(account(), certificate()));
    fixture.service.reset();
    const auto registry = take(take(Store::open(fixture.path, world()))->registry());
    REQUIRE(registry.characters.size() == 1);
    CHECK(registry.characters.front() == record);
}

TEST_CASE("expired character authority rejects checkpoints without refreshing its lease",
          "[storage-characters]") {
    auto configuration = limits();
    configuration.session_lifetime = 10ns;
    configuration.session_idle_timeout = 10ns;
    Fixture fixture{configuration};
    Pair pair;
    const auto record = fixture.create(pair);
    const auto session = login(fixture, pair, record);
    reject(fixture.service->save(*pair.server, envelope(session, 2), 1, state(true), 10ns));
    reject(fixture.service->load(*pair.server, envelope(session, 2), 11ns));
    fixture.service.reset();
    CHECK(take(take(Store::open(fixture.path, world()))->character(account(), record.character)) ==
          record);
}

TEST_CASE("character hydration rejects a registry that cannot fit the whole session owner",
          "[storage-characters]") {
    Fixture fixture;
    Pair a;
    Pair b{true};
    fixture.create(a);
    fixture.create(b, true);
    fixture.service.reset();
    auto store = take(Store::open(fixture.path, world()));
    const auto before = take(store->registry());
    auto configuration = limits();
    SECTION("accounts") {
        configuration.accounts = 1;
    }
    SECTION("certificates") {
        configuration.certificates = 1;
    }
    SECTION("characters") {
        configuration.characters = 1;
    }
    reject(CharacterAuthority::create(std::move(store), configuration));
    CHECK(take(take(Store::open(fixture.path, world()))->registry()) == before);
}

TEST_CASE("character authority checks its owning thread before accessing storage",
          "[storage-characters]") {
    Fixture fixture;
    std::optional<Result<StoreMetadata>> result;
    std::thread worker{[&] { result.emplace(fixture.service->metadata()); }};
    worker.join();
    REQUIRE(result);
    reject(*result);
    CHECK(result->error().code == ErrorCode::conflict);
    CHECK(take(fixture.service->metadata()).schema_version == store_schema_version);
    reject(CharacterAuthority::create(nullptr));
}

TEST_CASE("character records and rosters are independent owned snapshots", "[storage-characters]") {
    Fixture fixture;
    Pair pair;
    const auto record = fixture.create(pair);
    auto roster = take(fixture.service->roster(*pair.server, account(), 0ns));
    roster.characters.front().state.name.value = "Caller edited copy";
    roster.characters.clear();
    const auto session = login(fixture, pair, record);
    auto loaded = take(fixture.service->load(*pair.server, envelope(session, 2), 0ns));
    loaded.state.position.offset[1] = 99;
    fixture.reopen();
    CHECK(record.state == state());
    Pair next;
    CHECK(take(fixture.service->roster(*next.server, account(), 0ns)).characters ==
          std::vector{record});
    CHECK(loaded.state.position.offset[1] == 99);
}

TEST_CASE("character account admission shares bounded budgets across fresh transports",
          "[storage-characters]") {
    auto configuration = limits();
    configuration.auth_attempts_per_window = 2;
    Fixture fixture{configuration};
    Pair first;
    const auto roster = take(fixture.service->roster(*first.server, account(), 0ns));
    take(
        fixture.service->create_character(*first.server, account(), roster.revision, state(), 0ns));
    reject(fixture.service->roster(*first.server, account(), 0ns));
    Pair next;
    reject(fixture.service->roster(*next.server, account(), 0ns));
    reject(fixture.service->create_character(*next.server, account(), roster.revision + 1, state(),
                                             0ns));
}

TEST_CASE("uncertain committed character save freezes admissions until durable reconciliation",
          "[storage-characters]") {
    Fixture fixture;
    Pair pair;
    const auto record = fixture.create(pair);
    const auto session = login(fixture, pair, record);
    {
        AfterCommitFault fault;
        const auto result =
            fixture.service->save(*pair.server, envelope(session, 2), 1, state(true), 0ns);
        reject(result);
        CHECK(result.error().code == ErrorCode::io_failure);
    }
    frozen(fixture, pair, session);
    fixture.reopen();
    Pair next;
    const auto restored = take(fixture.service->roster(*next.server, account(), 0ns));
    REQUIRE(restored.characters.size() == 1);
    const auto& committed = restored.characters.front();
    CHECK(committed.account == record.account);
    CHECK(committed.character == record.character);
    CHECK(committed.entity == record.entity);
    CHECK(committed.revision == 2);
    CHECK(committed.state == state(true));
    const auto current = login(fixture, next, committed);
    CHECK(current.session != session.session);
    reject(fixture.service->load(*next.server, envelope(session, 3), 0ns));
    CHECK(take(fixture.service->load(*next.server, envelope(current, 2), 0ns)) == committed);
}

TEST_CASE("uncertain committed revocation freezes the service and stays revoked after reopen",
          "[storage-characters]") {
    Fixture fixture;
    Pair pair;
    const auto record = fixture.create(pair);
    const auto session = login(fixture, pair, record);
    bool revoke_account = false;
    SECTION("account") {
        revoke_account = true;
    }
    SECTION("certificate") {}
    {
        AfterCommitFault fault;
        const auto result = revoke_account ? fixture.service->revoke_account(account())
                                           : fixture.service->revoke_certificate(certificate());
        reject(result);
        CHECK(result.error().code == ErrorCode::io_failure);
    }
    frozen(fixture, pair, session);
    fixture.reopen();
    Pair next;
    reject(fixture.service->roster(*next.server, account(), 0ns));
    reject(fixture.service->apply(*next.server, LoginRequest{account(), record.character}, 0ns));
    reject(fixture.service->load(*next.server, envelope(session, 2), 0ns));
    fixture.service.reset();
    const auto registry = take(take(Store::open(fixture.path, world()))->registry());
    REQUIRE(registry.characters.size() == 1);
    CHECK(registry.characters.front() == record);
    if (revoke_account) {
        const auto found = std::ranges::find(registry.accounts, account(), &AccountRecord::account);
        REQUIRE(found != registry.accounts.end());
        CHECK(found->revoked);
    } else {
        const auto found =
            std::ranges::find(registry.certificates, certificate(), &EnrollmentRecord::certificate);
        REQUIRE(found != registry.certificates.end());
        CHECK(found->revoked);
    }
}

TEST_CASE("authenticated transfers preserve durable retry identity across fresh sessions",
          "[storage-characters]") {
    Fixture fixture;
    Pair a;
    Pair b{true};
    const auto first = fixture.create(a);
    const auto second = fixture.create(b, true);
    take(fixture.service->initialize_economy(account(), first.character, {100, 10}));
    take(fixture.service->initialize_economy(account(true), second.character, {50, 5}));
    const auto sa = login(fixture, a, first);
    const auto sb = login(fixture, b, second);
    const TransferRequest request{{take(fixture.service->metadata()).id, account(), 1},
                                  first.character,
                                  second.character,
                                  1,
                                  1,
                                  {25, 3}};
    // Neither an operation ID nor a source character can bypass the live owner.
    reject(fixture.service->transfer(*b.server, envelope(sb, 2), request, 0ns));
    auto copied = request;
    copied.operation.actor = account(true);
    reject(fixture.service->transfer(*b.server, envelope(sb, 2), copied, 0ns));
    CHECK(
        take(fixture.service->operation_stream(*b.server, envelope(sb, 2), 0ns)).receipts.empty());
    const auto committed =
        take(fixture.service->transfer(*a.server, envelope(sa, 2), request, 0ns));
    CHECK(committed.receipt.status == TransferStatus::committed);
    CHECK_FALSE(committed.replayed);
    REQUIRE(committed.receipt.source);
    REQUIRE(committed.receipt.target);
    CHECK(committed.receipt.source->state == EconomyState{75, 7});
    CHECK(committed.receipt.target->state == EconomyState{75, 8});
    reject(fixture.service->transfer(*a.server, envelope(sa, 2), request, 0ns));
    const auto retry = take(fixture.service->transfer(*a.server, envelope(sa, 3), request, 0ns));
    CHECK(retry.replayed);
    CHECK(retry.receipt == committed.receipt);
    CHECK(take(fixture.service->economy(*b.server, envelope(sb, 3), 0ns)) ==
          *committed.receipt.target);
    const auto stream = take(fixture.service->operation_stream(*a.server, envelope(sa, 4), 0ns));
    CHECK(stream.actor == account());
    CHECK(stream.high_watermark == 1);
    CHECK(stream.receipts == std::vector{committed.receipt});

    fixture.reopen();
    Pair fresh;
    const auto session = login(fixture, fresh, first);
    CHECK(session.session != sa.session);
    reject(fixture.service->transfer(*fresh.server, envelope(sa, 5), request, 0ns));
    const auto restored =
        take(fixture.service->transfer(*fresh.server, envelope(session, 2), request, 0ns));
    CHECK(restored.replayed);
    CHECK(restored.receipt == committed.receipt);
    CHECK(take(fixture.service->economy(*fresh.server, envelope(session, 3), 0ns)) ==
          *committed.receipt.source);
}

TEST_CASE("transfer shape rejection and terminal outcomes have distinct admission semantics",
          "[storage-characters]") {
    Fixture fixture;
    Pair a;
    Pair b{true};
    const auto first = fixture.create(a);
    const auto second = fixture.create(b, true);
    take(fixture.service->initialize_economy(account(), first.character, {100, 10}));
    take(fixture.service->initialize_economy(account(true), second.character, {50, 5}));
    const auto session = login(fixture, a, first);
    TransferRequest request{{take(fixture.service->metadata()).id, account(), 1},
                            first.character,
                            second.character,
                            2,
                            1,
                            {10, 1}};
    auto malformed = request;
    malformed.quantity = {0, 0};
    reject(fixture.service->transfer(*a.server, envelope(session, 2), malformed, 0ns));
    const auto stale =
        take(fixture.service->transfer(*a.server, envelope(session, 2), request, 0ns));
    CHECK(stale.receipt.status == TransferStatus::stale_revision);
    CHECK_FALSE(stale.replayed);
    const auto retry =
        take(fixture.service->transfer(*a.server, envelope(session, 3), request, 0ns));
    CHECK(retry.replayed);
    CHECK(retry.receipt == stale.receipt);
    request.expected_source_revision = 1;
    reject(fixture.service->transfer(*a.server, envelope(session, 4), request, 0ns));
    // Admitted durable mismatch consumes a live command sequence but never the
    // next durable operation; fixing payload requires a deliberately new ID.
    request.operation.sequence = 2;
    reject(fixture.service->transfer(*a.server, envelope(session, 4), request, 0ns));
    CHECK(take(fixture.service->transfer(*a.server, envelope(session, 5), request, 0ns))
              .receipt.status == TransferStatus::committed);
    const auto stream =
        take(fixture.service->operation_stream(*a.server, envelope(session, 6), 0ns));
    CHECK(stream.high_watermark == 2);
    REQUIRE(stream.receipts.size() == 2);
    CHECK(stream.receipts.front() == stale.receipt);
}

TEST_CASE("economic retries cannot restore revoked disconnected or stale session authority",
          "[storage-characters]") {
    auto configuration = limits();
    configuration.session_lifetime = 10ns;
    configuration.session_idle_timeout = 10ns;
    Fixture fixture{configuration};
    Pair a;
    Pair b{true};
    const auto first = fixture.create(a);
    const auto second = fixture.create(b, true);
    take(fixture.service->initialize_economy(account(), first.character, {100, 10}));
    take(fixture.service->initialize_economy(account(true), second.character, {50, 5}));
    const auto session = login(fixture, a, first);
    const TransferRequest request{{take(fixture.service->metadata()).id, account(), 1},
                                  first.character,
                                  second.character,
                                  1,
                                  1,
                                  {10, 1}};
    const auto result =
        take(fixture.service->transfer(*a.server, envelope(session, 2), request, 0ns));
    MonotonicTime now = 0ns;
    std::uint64_t sequence = 3;
    std::unique_ptr<Pair> replacement;
    SECTION("account revoked") {
        take(fixture.service->revoke_account(account()));
    }
    SECTION("certificate revoked") {
        take(fixture.service->revoke_certificate(certificate()));
    }
    SECTION("session revoked") {
        CHECK(take(fixture.service->revoke_session(session.session)));
    }
    SECTION("logout") {
        take(fixture.service->apply(*a.server, LogoutRequest{envelope(session, 3)}, 0ns));
        sequence = 4;
    }
    SECTION("not ready") {
        take(fixture.service->apply(*a.server, SetReadyRequest{envelope(session, 3), false}, 0ns));
        sequence = 4;
    }
    SECTION("expired") {
        now = 10ns;
    }
    SECTION("closed transport") {
        take(a.server->close());
    }
    SECTION("replacement") {
        replacement = std::make_unique<Pair>();
        login(fixture, *replacement, first);
    }
    reject(fixture.service->transfer(*a.server, envelope(session, sequence), request, now));
    reject(fixture.service->economy(*a.server, envelope(session, sequence), now));
    reject(fixture.service->operation_stream(*a.server, envelope(session, sequence), now));
    fixture.service.reset();
    auto store = take(Store::open(fixture.path, world()));
    // Trusted recovery inspection remains available; player admission does not.
    CHECK(take(store->economy(account(), first.character)) == *result.receipt.source);
    CHECK(take(store->operation_stream(account())).receipts == std::vector{result.receipt});
    CHECK(take(store->economy(account(true), second.character)) == *result.receipt.target);
}

TEST_CASE("economic operations share command budgets and creating thread ownership",
          "[storage-characters]") {
    auto configuration = limits();
    configuration.commands_per_window = 2;
    Fixture fixture{configuration};
    Pair a;
    Pair b{true};
    const auto first = fixture.create(a);
    const auto second = fixture.create(b, true);
    take(fixture.service->initialize_economy(account(), first.character, {100, 10}));
    take(fixture.service->initialize_economy(account(true), second.character, {50, 5}));
    const auto session = login(fixture, a, first);
    const TransferRequest request{{take(fixture.service->metadata()).id, account(), 1},
                                  first.character,
                                  second.character,
                                  1,
                                  1,
                                  {10, 1}};
    std::optional<Result<TransferResult>> wrong_thread;
    std::thread worker{[&] {
        wrong_thread.emplace(
            fixture.service->transfer(*a.server, envelope(session, 2), request, 0ns));
    }};
    worker.join();
    REQUIRE(wrong_thread);
    reject(*wrong_thread);
    take(fixture.service->economy(*a.server, envelope(session, 2), 0ns));
    reject(fixture.service->transfer(*a.server, envelope(session, 3), request, 0ns));
    reject(fixture.service->operation_stream(*a.server, envelope(session, 3), 0ns));
    fixture.service.reset();
    auto store = take(Store::open(fixture.path, world()));
    CHECK(take(store->operation_stream(account())).high_watermark == 0);
    CHECK(take(store->economy(account(), first.character)).state == EconomyState{100, 10});
}

TEST_CASE("uncertain economic acknowledgement freezes the owner and reopens one durable outcome",
          "[storage-characters]") {
    Fixture fixture;
    Pair a;
    Pair b{true};
    const auto first = fixture.create(a);
    const auto second = fixture.create(b, true);
    take(fixture.service->initialize_economy(account(), first.character, {100, 10}));
    take(fixture.service->initialize_economy(account(true), second.character, {50, 5}));
    const auto session = login(fixture, a, first);
    const TransferRequest request{{take(fixture.service->metadata()).id, account(), 1},
                                  first.character,
                                  second.character,
                                  1,
                                  1,
                                  {10, 1}};
    {
        AfterCommitFault fault;
        reject(fixture.service->transfer(*a.server, envelope(session, 2), request, 0ns));
    }
    reject(fixture.service->economy(*a.server, envelope(session, 3), 0ns));
    reject(fixture.service->operation_stream(*a.server, envelope(session, 3), 0ns));
    reject(fixture.service->initialize_economy(account(), first.character, {500, 500}));
    fixture.reopen();
    Pair fresh;
    const auto restored_session = login(fixture, fresh, first);
    const auto retry =
        take(fixture.service->transfer(*fresh.server, envelope(restored_session, 2), request, 0ns));
    CHECK(retry.replayed);
    CHECK(retry.receipt.status == TransferStatus::committed);
    REQUIRE(retry.receipt.source);
    REQUIRE(retry.receipt.target);
    CHECK(retry.receipt.source->state == EconomyState{90, 9});
    CHECK(retry.receipt.target->state == EconomyState{60, 6});
    CHECK(take(fixture.service->operation_stream(*fresh.server, envelope(restored_session, 3), 0ns))
              .high_watermark == 1);
}
