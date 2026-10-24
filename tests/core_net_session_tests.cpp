#include <shark/net/session.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <thread>
#include <type_traits>

namespace {
using namespace shark::net;
using namespace std::chrono_literals;
using shark::MonotonicTime;

static_assert(!std::is_convertible_v<AccountId, CharacterId>);
static_assert(!std::is_convertible_v<SessionId, AccountId>);

shark::StableId uuid(const char* value) {
    auto parsed = shark::StableId::parse(value);
    REQUIRE(parsed);
    return parsed.value();
}
AccountId account_a() {
    return AccountId{uuid("00112233-4455-4677-8899-aabbccddee01")};
}
AccountId account_b() {
    return AccountId{uuid("00112233-4455-4677-8899-aabbccddee02")};
}
CharacterId character_a() {
    return CharacterId{uuid("10213243-5465-4768-89ab-bbccddeeff01")};
}
CharacterId character_b() {
    return CharacterId{uuid("10213243-5465-4768-89ab-bbccddeeff02")};
}
SessionId example_session() {
    return SessionId{uuid("20314253-6475-4869-8abc-ccddeeff0001")};
}

std::string fixture(const std::string& name) {
    std::ifstream stream{std::filesystem::path{SHARK_NETWORK_FIXTURE_DIRECTORY} / name,
                         std::ios::binary};
    REQUIRE(stream);
    return {std::istreambuf_iterator<char>{stream}, {}};
}
CertificateFingerprint fingerprint(const std::string& name) {
    auto parsed = certificate_fingerprint(fixture("session/" + name + ".pem"));
    REQUIRE(parsed);
    return parsed.value();
}

struct Pair {
    std::unique_ptr<TlsListener> listener;
    std::unique_ptr<TlsTransport> client;
    std::unique_ptr<TlsTransport> server;

    explicit Pair(const std::string& name = "client-a", bool authenticated = true) {
        const ServerIdentity identity{fixture("server.pem"), fixture("server-key.pem")};
        const ClientTrust trust{fixture("ca.pem"), "shark.test"};
        auto listened =
            authenticated
                ? TlsListener::listen_loopback_authenticated(0, identity, fixture("ca.pem"))
                : TlsListener::listen_loopback(0, identity);
        REQUIRE(listened);
        listener = std::move(listened).value();
        const auto port = listener->port().value();
        auto connected = authenticated ? TlsTransport::connect_loopback_authenticated(
                                             port, trust,
                                             {fixture("session/" + name + ".pem"),
                                              fixture("session/" + name + "-key.pem")},
                                             0ns)
                                       : TlsTransport::connect_loopback(port, trust, 0ns);
        REQUIRE(connected);
        client = std::move(connected).value();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            if (!server) {
                auto accepted = listener->accept(0ns);
                REQUIRE(accepted);
                if (accepted.value()) {
                    server = std::move(*accepted.value());
                }
            }
            REQUIRE(client->poll(0ns));
            if (server) {
                REQUIRE(server->poll(0ns));
                if (client->stats().value().state == TransportState::ready &&
                    server->stats().value().state == TransportState::ready) {
                    break;
                }
            }
            std::this_thread::yield();
        }
    }
};

std::unique_ptr<SessionAuthority> authority(const SessionLimits& limits = {}, bool second = false) {
    auto created = SessionAuthority::create(limits);
    REQUIRE(created);
    auto result = std::move(created).value();
    REQUIRE(result->provision_account(account_a()));
    REQUIRE(result->assign_character(account_a(), character_a()));
    REQUIRE(result->enroll_certificate(account_a(), fingerprint("client-a")));
    if (second) {
        REQUIRE(result->provision_account(account_b()));
        REQUIRE(result->assign_character(account_b(), character_b()));
        REQUIRE(result->enroll_certificate(account_b(), fingerprint("client-b")));
    }
    return result;
}

SessionSnapshot login(SessionAuthority& service, Pair& pair, MonotonicTime now = 0ns,
                      bool second = false) {
    auto result = service.apply(
        *pair.server,
        LoginRequest{second ? account_b() : account_a(), second ? character_b() : character_a()},
        now);
    REQUIRE(result);
    return result.value();
}

SessionEnvelope envelope(const SessionSnapshot& snapshot, std::uint64_t sequence) {
    return {snapshot.session, snapshot.ownership_epoch, sequence, snapshot.character};
}

template <typename T>
void rejected(const shark::Result<T>& result, const char* operation = nullptr) {
    REQUIRE_FALSE(result);
    CHECK_FALSE(result.error().context.operation.empty());
    CHECK(result.error().context.entity_id.empty());
    CHECK(result.error().context.request_id.empty());
    CHECK(result.error().context.path.empty());
    if (operation) {
        CHECK(result.error().context.operation == operation);
    }
}

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}
} // namespace

TEST_CASE("Session limits bound retained registries and permanent revocation records",
          "[net-session]") {
    for (const auto count :
         {std::size_t{0}, std::size_t{1025}, std::numeric_limits<std::size_t>::max()}) {
        SessionLimits limits;
        limits.accounts = count;
        rejected(SessionAuthority::create(limits));
    }
    for (const auto duration : {-1ns, 0ns, MonotonicTime{25h}}) {
        SessionLimits limits;
        limits.session_lifetime = duration;
        rejected(SessionAuthority::create(limits));
    }
    SessionLimits limits;
    limits.accounts = limits.characters = limits.certificates = limits.live_sessions = 1;
    auto service = authority(limits);
    rejected(service->provision_account(account_a()));
    rejected(service->provision_account(account_b()));
    rejected(service->assign_character(account_b(), character_b()));
    rejected(service->assign_character(account_a(), character_b()));
    rejected(service->enroll_certificate(account_a(), fingerprint("client-b")));
    REQUIRE(service->revoke_certificate(fingerprint("client-a")));
    REQUIRE(service->revoke_certificate(fingerprint("client-a")));
    rejected(service->enroll_certificate(account_a(), fingerprint("client-a")));
    REQUIRE(service->revoke_account(account_a()));
    REQUIRE(service->revoke_account(account_a()));
    rejected(service->provision_account(account_a()));
    CHECK(service->stats().value().accounts == 1);
    CHECK(service->stats().value().certificates == 1);
    CHECK(service->stats().value().live_sessions == 0);
    limits = {};
    limits.auth_window = 0ns;
    rejected(SessionAuthority::create(limits));
    limits = {};
    limits.command_window = 2h;
    rejected(SessionAuthority::create(limits));
    limits = {};
    limits.commands_per_window = 0;
    rejected(SessionAuthority::create(limits));
    limits = {};
    limits.login_attempts_per_window = 65537;
    rejected(SessionAuthority::create(limits));
    limits = {};
    limits.session_idle_timeout = 2h;
    rejected(SessionAuthority::create(limits));
}

TEST_CASE("Session authority checks its owner and monotonic clock without mutation",
          "[net-session]") {
    auto service = authority();
    std::optional<shark::Error> wrong_thread;
    std::thread other{[&] {
        auto result = service->revoke_account(account_a());
        if (!result) {
            wrong_thread = result.error();
        }
    }};
    other.join();
    REQUIRE(wrong_thread);
    CHECK(wrong_thread->context.operation == "net.session.owner");
    rejected(service->expire(-1ns), "net.session.time");
    REQUIRE(service->expire(10ns));
    rejected(service->expire(9ns), "net.session.time");
    REQUIRE(service->expire(10ns));
    Pair pair;
    CHECK(login(*service, pair, 10ns).active);
}

TEST_CASE("Only a live server-side mutual TLS transport can obtain session authority",
          "[net-session]") {
    auto service = authority();
    Pair anonymous{"client-a", false};
    const SessionRequest request{LoginRequest{account_a(), character_a()}};
    rejected(service->apply(*anonymous.server, request, 0ns), "net.session.peer");
    Pair authenticated;
    rejected(service->apply(*authenticated.client, request, 0ns), "net.session.peer");
    auto snapshot = login(*service, authenticated);
    REQUIRE(authenticated.server->close());
    rejected(service->apply(*authenticated.server, HeartbeatRequest{envelope(snapshot, 1)}, 0ns),
             "net.session.peer");
    REQUIRE(service->revoke_session(snapshot.session));
    CHECK(service->stats().value().live_sessions == 0);
}

TEST_CASE("Distinct real TLS identities own independent ready heartbeat and logout sessions",
          "[net-session]") {
    auto service = authority({}, true);
    Pair a;
    Pair b{"client-b"};
    auto first = login(*service, a);
    auto second = login(*service, b, 0ns, true);
    CHECK(first.session != second.session);
    CHECK(first.account != second.account);
    CHECK(first.ownership_epoch == 1);
    CHECK(first.last_sequence == 0);
    CHECK_FALSE(first.ready);
    auto ready = service->apply(*a.server, SetReadyRequest{envelope(first, 1), true}, 1ns);
    REQUIRE(ready);
    CHECK(ready.value().ready);
    auto heartbeat = service->apply(*a.server, HeartbeatRequest{envelope(first, 2)}, 2ns);
    REQUIRE(heartbeat);
    CHECK(heartbeat.value().ready);
    auto logout = service->apply(*a.server, LogoutRequest{envelope(first, 3)}, 3ns);
    REQUIRE(logout);
    CHECK_FALSE(logout.value().active);
    CHECK_FALSE(logout.value().ready);
    CHECK(logout.value().last_sequence == 3);
    rejected(service->apply(*a.server, HeartbeatRequest{envelope(first, 4)}, 3ns));
    CHECK(service->stats().value().live_sessions == 1);
    auto remaining = service->apply(*b.server, HeartbeatRequest{envelope(second, 1)}, 3ns);
    REQUIRE(remaining);
    CHECK_FALSE(remaining.value().ready);
}

TEST_CASE("Forged ownership replay gaps and cross-connection envelopes cannot mutate state",
          "[net-session]") {
    auto service = authority({}, true);
    Pair a;
    Pair b{"client-b"};
    const auto wrong_account =
        service->apply(*a.server, LoginRequest{account_b(), character_b()}, 0ns);
    rejected(wrong_account, "net.session.ownership");
    rejected(service->apply(*a.server, LoginRequest{account_a(), character_b()}, 0ns));
    auto first = login(*service, a);
    auto second = login(*service, b, 0ns, true);
    for (const auto sequence :
         {std::uint64_t{0}, std::uint64_t{2}, std::numeric_limits<std::uint64_t>::max()}) {
        rejected(service->apply(*a.server, SetReadyRequest{envelope(first, sequence), true}, 0ns),
                 "net.session.sequence");
    }
    auto forged = envelope(first, 1);
    forged.character = character_b();
    rejected(service->apply(*a.server, SetReadyRequest{forged, true}, 0ns));
    forged = envelope(first, 1);
    ++forged.ownership_epoch;
    rejected(service->apply(*a.server, HeartbeatRequest{forged}, 0ns));
    forged = envelope(first, 1);
    forged.session = second.session;
    rejected(service->apply(*a.server, HeartbeatRequest{forged}, 0ns));
    auto stolen = service->apply(*b.server, SetReadyRequest{envelope(first, 1), true}, 0ns);
    rejected(stolen);
    CHECK(encode_session_reply(public_session_reply(stolen)).value() == bytes({0, 1, 2, 0}));
    CHECK(public_session_reply(stolen) == public_session_reply(wrong_account));
    auto accepted = service->apply(*a.server, HeartbeatRequest{envelope(first, 1)}, 0ns);
    REQUIRE(accepted);
    CHECK_FALSE(accepted.value().ready);
    rejected(service->apply(*a.server, SetReadyRequest{envelope(first, 1), true}, 0ns));
    REQUIRE(service->apply(*a.server, HeartbeatRequest{envelope(first, 2)}, 0ns));
    auto own_b = service->apply(*b.server, HeartbeatRequest{envelope(second, 1)}, 0ns);
    REQUIRE(own_b);
    CHECK_FALSE(own_b.value().ready);
}

TEST_CASE("Reconnect atomically rotates authority and fences older connections and stale teardown",
          "[net-session]") {
    auto service = authority();
    Pair older;
    Pair first;
    auto initial = login(*service, first);
    rejected(service->apply(*older.server, LoginRequest{account_a(), character_a()}, 0ns),
             "net.session.reconnect");
    Pair next;
    auto replacement = login(*service, next);
    CHECK(replacement.session != initial.session);
    CHECK(replacement.ownership_epoch == initial.ownership_epoch + 1);
    CHECK(service->stats().value().live_sessions == 1);
    rejected(service->apply(*first.server, HeartbeatRequest{envelope(initial, 1)}, 0ns));
    rejected(service->apply(*first.server, LoginRequest{account_a(), character_a()}, 0ns),
             "net.session.reconnect");
    rejected(service->apply(*next.server, LoginRequest{account_a(), character_a()}, 0ns));
    auto stale = service->revoke_session(initial.session);
    REQUIRE(stale);
    CHECK_FALSE(stale.value());
    REQUIRE(service->apply(*next.server, SetReadyRequest{envelope(replacement, 1), true}, 0ns));
    REQUIRE(service->assign_character(account_a(), character_b()));
    Pair switched;
    auto changed = service->apply(*switched.server, LoginRequest{account_a(), character_b()}, 0ns);
    REQUIRE(changed);
    CHECK(changed.value().ownership_epoch == replacement.ownership_epoch + 1);
    CHECK(changed.value().character == character_b());
    rejected(service->apply(*next.server, LogoutRequest{envelope(replacement, 2)}, 0ns));
    CHECK(service->stats().value().live_sessions == 1);
}

TEST_CASE(
    "Session idle and absolute deadlines reject exactly at expiry and never refresh on rejection",
    "[net-session]") {
    SessionLimits limits;
    limits.session_lifetime = 100ns;
    limits.session_idle_timeout = 10ns;
    auto service = authority(limits);
    Pair first;
    auto initial = login(*service, first);
    rejected(service->apply(*first.server, HeartbeatRequest{envelope(initial, 2)}, 9ns));
    CHECK(service->expire(9ns).value() == 0);
    CHECK(service->expire(10ns).value() == 1);
    rejected(service->apply(*first.server, HeartbeatRequest{envelope(initial, 1)}, 10ns));
    Pair next;
    auto renewed = login(*service, next, 10ns);
    CHECK(renewed.ownership_epoch == 2);
    REQUIRE(service->apply(*next.server, HeartbeatRequest{envelope(renewed, 1)}, 19ns));
    CHECK(service->expire(20ns).value() == 0);
    CHECK(service->expire(29ns).value() == 1);

    limits.session_lifetime = 30ns;
    limits.session_idle_timeout = 20ns;
    auto absolute = authority(limits);
    Pair final;
    auto fixed = login(*absolute, final);
    REQUIRE(absolute->apply(*final.server, HeartbeatRequest{envelope(fixed, 1)}, 19ns));
    CHECK(absolute->expire(29ns).value() == 0);
    CHECK(absolute->expire(30ns).value() == 1);
}

TEST_CASE("Certificate and account admission windows survive fresh TLS reconnects",
          "[net-session]") {
    SessionLimits limits;
    limits.auth_attempts_per_window = 2;
    limits.auth_window = 10ns;
    auto service = authority(limits);
    REQUIRE(service->enroll_certificate(account_a(), fingerprint("client-b")));
    Pair first;
    auto initial = login(*service, first);
    rejected(service->apply(*first.server, LoginRequest{account_a(), character_a()}, 0ns));
    Pair same_cert;
    rejected(service->apply(*same_cert.server, LoginRequest{account_a(), character_a()}, 0ns),
             "net.session.rate.certificate");
    Pair other_cert{"client-b"};
    rejected(service->apply(*other_cert.server, LoginRequest{account_a(), character_a()}, 0ns),
             "net.session.rate.account");
    REQUIRE(service->apply(*first.server, HeartbeatRequest{envelope(initial, 1)}, 0ns));
    auto later = service->apply(*other_cert.server, LoginRequest{account_a(), character_a()}, 10ns);
    REQUIRE(later);
    CHECK(later.value().ownership_epoch == 2);
}

TEST_CASE("Unknown enrollment attempts consume the bounded authority-wide login window",
          "[net-session]") {
    SessionLimits limits;
    limits.login_attempts_per_window = 2;
    limits.auth_window = 10ns;
    auto service = authority(limits);
    Pair unknown{"client-b"};
    Pair known;
    for (int attempt = 0; attempt < 2; ++attempt) {
        rejected(service->apply(*unknown.server, LoginRequest{account_a(), character_a()}, 0ns),
                 "net.session.enrollment");
    }
    rejected(service->apply(*known.server, LoginRequest{account_a(), character_a()}, 9ns),
             "net.session.rate.authority");
    CHECK(service->stats().value().certificates == 1);
    CHECK(service->stats().value().live_sessions == 0);
    CHECK(login(*service, known, 10ns).active);
}

TEST_CASE("Session command rate rejection preserves exact next sequence and accepted state",
          "[net-session]") {
    SessionLimits limits;
    limits.commands_per_window = 2;
    limits.command_window = 10ns;
    auto service = authority(limits);
    Pair pair;
    auto snapshot = login(*service, pair);
    REQUIRE(service->apply(*pair.server, HeartbeatRequest{envelope(snapshot, 1)}, 0ns));
    REQUIRE(service->apply(*pair.server, SetReadyRequest{envelope(snapshot, 2), true}, 0ns));
    rejected(service->apply(*pair.server, SetReadyRequest{envelope(snapshot, 3), false}, 9ns),
             "net.session.rate.command");
    auto next = service->apply(*pair.server, HeartbeatRequest{envelope(snapshot, 3)}, 10ns);
    REQUIRE(next);
    CHECK(next.value().ready);
    CHECK(next.value().last_sequence == 3);
}

TEST_CASE("Certificate account and exact-session revocation immediately remove authority",
          "[net-session]") {
    auto service = authority({}, true);
    Pair a;
    Pair b{"client-b"};
    auto first = login(*service, a);
    auto second = login(*service, b, 0ns, true);
    SECTION("certificate") {
        REQUIRE(service->revoke_certificate(fingerprint("client-a")));
        Pair reconnect;
        rejected(service->apply(*reconnect.server, LoginRequest{account_a(), character_a()}, 0ns),
                 "net.session.revoked");
        rejected(service->enroll_certificate(account_a(), fingerprint("client-a")));
    }
    SECTION("account") {
        REQUIRE(service->revoke_account(account_a()));
        Pair reconnect;
        rejected(service->apply(*reconnect.server, LoginRequest{account_a(), character_a()}, 0ns),
                 "net.session.revoked");
        rejected(service->assign_character(account_a(), CharacterId{example_session().value()}));
    }
    SECTION("session") {
        auto removed = service->revoke_session(first.session);
        REQUIRE(removed);
        CHECK(removed.value());
        CHECK_FALSE(service->revoke_session(first.session).value());
    }
    rejected(service->apply(*a.server, HeartbeatRequest{envelope(first, 1)}, 0ns));
    CHECK(service->stats().value().live_sessions == 1);
    REQUIRE(service->apply(*b.server, HeartbeatRequest{envelope(second, 1)}, 0ns));
}

TEST_CASE("Capacity and failed reconnect checks preserve existing ownership", "[net-session]") {
    SessionLimits limits;
    limits.live_sessions = 1;
    auto service = authority(limits, true);
    Pair a;
    Pair b{"client-b"};
    auto initial = login(*service, a);
    rejected(service->apply(*b.server, LoginRequest{account_b(), character_b()}, 0ns),
             "net.session.capacity");
    Pair replacement;
    rejected(service->apply(*replacement.server, LoginRequest{account_a(), character_b()}, 0ns));
    REQUIRE(service->apply(*a.server, HeartbeatRequest{envelope(initial, 1)}, 0ns));
    auto next = login(*service, replacement);
    CHECK(next.ownership_epoch == 2);
    CHECK(service->stats().value().live_sessions == 1);
    CHECK_FALSE(service->revoke_session(initial.session).value());
    REQUIRE(service->revoke_session(next.session));
    CHECK(login(*service, b, 0ns, true).ownership_epoch == 1);
}

TEST_CASE("Ownership epochs never wrap and unrepresentable deadlines cannot admit a session",
          "[net-session]") {
    CHECK(next_ownership_epoch(0).value() == 1);
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    CHECK(next_ownership_epoch(maximum - 1).value() == maximum);
    rejected(next_ownership_epoch(maximum), "net.session.epoch");
    auto service = authority();
    Pair pair;
    rejected(service->apply(*pair.server, LoginRequest{account_a(), character_a()},
                            MonotonicTime::max()),
             "net.session.time");
    CHECK(service->stats().value().live_sessions == 0);
}

TEST_CASE("Account admission authenticates enrollment before a first character exists",
          "[net-session]") {
    auto created = SessionAuthority::create();
    REQUIRE(created);
    auto service = std::move(created).value();
    REQUIRE(service->provision_account(account_a()));
    REQUIRE(service->enroll_certificate(account_a(), fingerprint("client-a")));
    Pair pair;
    auto admitted = service->authorize_account(*pair.server, account_a(), 0ns);
    REQUIRE(admitted);
    CHECK(admitted.value() == account_a());
    CHECK(service->stats().value().accounts == 1);
    CHECK(service->stats().value().characters == 0);
    CHECK(service->stats().value().live_sessions == 0);
    rejected(service->current(*pair.server, 0ns), "net.session.connection");

    REQUIRE(service->assign_character(account_a(), character_a()));
    const auto first = login(*service, pair);
    CHECK(first.ownership_epoch == 1);
    CHECK(first.last_sequence == 0);
    CHECK_FALSE(first.ready);
}

TEST_CASE("Account admission requires live server mTLS and preserves owner and clock guards",
          "[net-session]") {
    auto service = authority();
    Pair diagnostic{"client-a", false};
    rejected(service->authorize_account(*diagnostic.server, account_a(), 0ns), "net.session.peer");
    Pair known;
    rejected(service->authorize_account(*known.client, account_a(), 0ns), "net.session.peer");
    Pair unknown{"client-b"};
    rejected(service->authorize_account(*unknown.server, account_a(), 0ns),
             "net.session.enrollment");
    const auto account = account_a();
    std::optional<shark::Error> wrong_thread;
    std::thread other{[&] {
        auto result = service->authorize_account(*known.server, account, 100ns);
        if (!result) {
            wrong_thread = result.error();
        }
    }};
    other.join();
    REQUIRE(wrong_thread);
    CHECK(wrong_thread->context.operation == "net.session.owner");
    REQUIRE(service->authorize_account(*known.server, account, 10ns));
    rejected(service->authorize_account(*known.server, account, 9ns), "net.session.time");
    rejected(service->authorize_account(*known.server, account, -1ns), "net.session.time");
    REQUIRE(known.server->close());
    rejected(service->authorize_account(*known.server, account, 10ns), "net.session.peer");
    CHECK(service->stats().value().live_sessions == 0);
}

TEST_CASE("Account admission and login share one authority attempt each including unknown peers",
          "[net-session]") {
    SessionLimits limits;
    limits.login_attempts_per_window = 2;
    limits.auth_window = 10ns;
    auto service = authority(limits);
    Pair unknown{"client-b"};
    Pair known;
    rejected(service->authorize_account(*unknown.server, account_a(), 0ns),
             "net.session.enrollment");
    const auto first = login(*service, known);
    rejected(service->authorize_account(*known.server, account_a(), 9ns),
             "net.session.rate.authority");
    REQUIRE(service->authorize_account(*known.server, account_a(), 10ns));
    Pair next;
    const auto replacement = login(*service, next, 10ns);
    CHECK(replacement.ownership_epoch == first.ownership_epoch + 1);
    rejected(service->apply(*unknown.server, LoginRequest{account_a(), character_a()}, 10ns),
             "net.session.rate.authority");
    CHECK(service->stats().value().live_sessions == 1);
}

TEST_CASE("Mismatched account admission shares retained certificate and account login budgets",
          "[net-session]") {
    SessionLimits limits;
    limits.auth_attempts_per_window = 2;
    limits.auth_window = 10ns;
    auto service = authority(limits);
    REQUIRE(service->enroll_certificate(account_a(), fingerprint("client-b")));
    Pair first;
    Pair other_certificate{"client-b"};
    rejected(service->authorize_account(*first.server, account_b(), 0ns), "net.session.ownership");
    const auto selected = login(*service, first);
    rejected(service->authorize_account(*first.server, account_a(), 9ns),
             "net.session.rate.certificate");
    rejected(service->authorize_account(*other_certificate.server, account_a(), 9ns),
             "net.session.rate.account");
    CHECK(service->current(*first.server, 9ns).value() == selected);
    REQUIRE(service->authorize_account(*other_certificate.server, account_a(), 10ns));
    const auto replacement = login(*service, other_certificate, 10ns);
    CHECK(replacement.ownership_epoch == selected.ownership_epoch + 1);
}

TEST_CASE("Revocation rejects account admission on existing and fresh TLS connections",
          "[net-session]") {
    auto service = authority();
    Pair pair;
    REQUIRE(service->authorize_account(*pair.server, account_a(), 0ns));
    const auto selected = login(*service, pair);
    SECTION("certificate") {
        REQUIRE(service->revoke_certificate(fingerprint("client-a")));
    }
    SECTION("account") {
        REQUIRE(service->revoke_account(account_a()));
    }
    rejected(service->authorize_account(*pair.server, account_a(), 0ns), "net.session.revoked");
    Pair next;
    rejected(service->authorize_account(*next.server, account_a(), 0ns), "net.session.revoked");
    rejected(service->apply(*next.server, LoginRequest{account_a(), character_a()}, 0ns),
             "net.session.revoked");
    rejected(service->apply(*pair.server, HeartbeatRequest{envelope(selected, 1)}, 0ns));
    CHECK(service->stats().value().live_sessions == 0);
}

TEST_CASE("Account admission and rejection never refresh a selected session lease",
          "[net-session]") {
    SessionLimits limits;
    limits.session_lifetime = 30ns;
    limits.session_idle_timeout = 10ns;
    auto service = authority(limits);
    Pair pair;
    const auto selected = login(*service, pair);
    auto ready = service->apply(*pair.server, SetReadyRequest{envelope(selected, 1), true}, 0ns);
    REQUIRE(ready);
    REQUIRE(service->authorize_account(*pair.server, account_a(), 9ns));
    rejected(service->authorize_account(*pair.server, account_b(), 9ns), "net.session.ownership");
    CHECK(service->current(*pair.server, 9ns).value() == ready.value());
    CHECK(service->expire(10ns).value() == 1);
    REQUIRE(service->authorize_account(*pair.server, account_a(), 10ns));
    CHECK(service->stats().value().live_sessions == 0);
    rejected(service->current(*pair.server, 10ns), "net.session.connection");
    Pair next;
    CHECK(login(*service, next, 10ns).ownership_epoch == selected.ownership_epoch + 1);
}

TEST_CASE("Account admission preserves command budgets and reconnect ownership fences",
          "[net-session]") {
    SessionLimits limits;
    limits.commands_per_window = 1;
    limits.command_window = 10ns;
    auto service = authority(limits);
    Pair older;
    Pair selected;
    const auto initial = login(*service, selected);
    auto ready = service->apply(*selected.server, SetReadyRequest{envelope(initial, 1), true}, 0ns);
    REQUIRE(ready);
    REQUIRE(service->authorize_account(*selected.server, account_a(), 0ns));
    CHECK(service->current(*selected.server, 0ns).value() == ready.value());
    rejected(service->apply(*selected.server, HeartbeatRequest{envelope(initial, 2)}, 0ns),
             "net.session.rate.command");
    rejected(service->authorize_account(*older.server, account_a(), 0ns), "net.session.reconnect");
    rejected(service->apply(*older.server, LoginRequest{account_a(), character_b()}, 0ns),
             "net.session.ownership");
    rejected(service->apply(*older.server, LoginRequest{account_a(), character_a()}, 0ns),
             "net.session.reconnect");
    REQUIRE(service->apply(*selected.server, HeartbeatRequest{envelope(initial, 2)}, 10ns));
    Pair next;
    const auto replacement = login(*service, next, 10ns);
    rejected(service->authorize_account(*selected.server, account_a(), 10ns),
             "net.session.reconnect");
    REQUIRE(service->authorize_account(*next.server, account_a(), 10ns));
    rejected(service->current(*selected.server, 10ns), "net.session.connection");
    CHECK(service->current(*next.server, 10ns).value() == replacement);
    CHECK(replacement.ownership_epoch == initial.ownership_epoch + 1);
    CHECK_FALSE(service->revoke_session(initial.session).value());
}

TEST_CASE("Session request bytes independently specify UUID order integers and boolean layout",
          "[net-session]") {
    const SessionRequest login_request{LoginRequest{account_a(), character_a()}};
    const auto golden =
        bytes({0,    1,    1,    0,    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x46, 0x77,
               0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01, 0x10, 0x21, 0x32, 0x43,
               0x54, 0x65, 0x47, 0x68, 0x89, 0xab, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01});
    REQUIRE(encode_session_request(login_request));
    CHECK(encode_session_request(login_request).value() == golden);
    REQUIRE(decode_session_request(golden));
    CHECK(decode_session_request(golden).value() == login_request);
    const SessionEnvelope key{example_session(), 0x0102030405060708ULL, 0x1112131415161718ULL,
                              character_a()};
    const std::array<SessionRequest, 3> requests{SetReadyRequest{key, true}, HeartbeatRequest{key},
                                                 LogoutRequest{key}};
    for (const auto& request : requests) {
        auto encoded = encode_session_request(request);
        REQUIRE(encoded);
        CHECK(std::vector<std::byte>(encoded.value().begin() + 20, encoded.value().begin() + 36) ==
              bytes({1, 2, 3, 4, 5, 6, 7, 8, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18}));
        auto decoded = decode_session_request(encoded.value());
        REQUIRE(decoded);
        CHECK(decoded.value() == request);
        CHECK(encoded.value().size() ==
              (std::holds_alternative<SetReadyRequest>(request) ? 53U : 52U));
    }
    CHECK(encode_session_request(requests[0]).value().back() == std::byte{1});
    CHECK(encode_session_request(SetReadyRequest{key, false}).value().back() == std::byte{0});
}

TEST_CASE(
    "Session codecs reject all truncations trailing bytes unknown kinds versions and invalid UUIDs",
    "[net-session]") {
    const SessionEnvelope key{example_session(), 1, 1, character_a()};
    const std::array<SessionRequest, 4> requests{LoginRequest{account_a(), character_a()},
                                                 SetReadyRequest{key, true}, HeartbeatRequest{key},
                                                 LogoutRequest{key}};
    for (const auto& request : requests) {
        const auto valid = encode_session_request(request).value();
        for (std::size_t count = 0; count < valid.size(); ++count) {
            rejected(decode_session_request(std::span{valid}.first(count)));
        }
        auto changed = valid;
        changed.push_back(std::byte{0});
        rejected(decode_session_request(changed));
        for (const auto index : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3}}) {
            changed = valid;
            changed[index] = std::byte{0xff};
            rejected(decode_session_request(changed));
        }
        for (const auto index : {std::size_t{10}, std::size_t{12}}) {
            changed = valid;
            changed[index] = std::byte{0};
            rejected(decode_session_request(changed));
        }
        const std::size_t character_offset =
            std::holds_alternative<LoginRequest>(request) ? 20U : 36U;
        for (const auto offset : {std::size_t{6}, std::size_t{8}}) {
            changed = valid;
            changed[character_offset + offset] = std::byte{0};
            rejected(decode_session_request(changed));
        }
        changed = valid;
        std::fill(changed.begin() + 4, changed.begin() + 20, std::byte{0});
        rejected(decode_session_request(changed));
    }
    auto ready = encode_session_request(SetReadyRequest{key, true}).value();
    ready[52] = std::byte{2};
    rejected(decode_session_request(ready));
    for (const auto offset : {std::size_t{20}, std::size_t{28}}) {
        auto changed = encode_session_request(HeartbeatRequest{key}).value();
        std::fill(changed.begin() + static_cast<std::ptrdiff_t>(offset),
                  changed.begin() + static_cast<std::ptrdiff_t>(offset + 8), std::byte{0});
        rejected(decode_session_request(changed));
    }
    rejected(encode_session_request(HeartbeatRequest{{example_session(), 0, 1, character_a()}}));
    rejected(encode_session_request(HeartbeatRequest{{example_session(), 1, 0, character_a()}}));
    rejected(decode_session_request(std::vector<std::byte>(4096, std::byte{0})));
}

TEST_CASE("Session replies contain only own state or one canonical generic rejection",
          "[net-session]") {
    const SessionSnapshot snapshot{
        example_session(), account_a(), character_a(), 1, 0, false, true};
    auto encoded = encode_session_reply(SessionReply{snapshot});
    REQUIRE(encoded);
    CHECK(encoded.value().size() == 70);
    CHECK(std::vector<std::byte>(encoded.value().begin(), encoded.value().begin() + 4) ==
          bytes({0, 1, 1, 0}));
    CHECK(encoded.value()[59] == std::byte{1});
    CHECK(encoded.value()[67] == std::byte{0});
    CHECK(encoded.value()[68] == std::byte{0});
    CHECK(encoded.value()[69] == std::byte{1});
    REQUIRE(decode_session_reply(encoded.value()));
    CHECK(decode_session_reply(encoded.value()).value() == SessionReply{snapshot});
    const auto generic = encode_session_reply(SessionRejected{}).value();
    CHECK(generic == bytes({0, 1, 2, 0}));
    CHECK(decode_session_reply(generic).value() == SessionReply{SessionRejected{}});
    for (std::size_t count = 0; count < encoded.value().size(); ++count) {
        rejected(decode_session_reply(std::span{encoded.value()}.first(count)));
    }
    for (const auto index : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3},
                             std::size_t{68}, std::size_t{69}}) {
        auto changed = encoded.value();
        changed[index] = std::byte{0xff};
        rejected(decode_session_reply(changed));
    }
    for (const auto index : {std::size_t{10}, std::size_t{26}, std::size_t{42}}) {
        auto changed = encoded.value();
        changed[index] = std::byte{0};
        rejected(decode_session_reply(changed));
    }
    auto trailing = generic;
    trailing.push_back(std::byte{0});
    rejected(decode_session_reply(trailing));
    trailing = encoded.value();
    trailing.push_back(std::byte{0});
    rejected(decode_session_reply(trailing));
    auto impossible_ready = snapshot;
    impossible_ready.ready = true;
    rejected(encode_session_reply(impossible_ready));
    auto impossible_bytes = encoded.value();
    impossible_bytes[68] = std::byte{1};
    rejected(decode_session_reply(impossible_bytes));
    auto inactive = snapshot;
    inactive.active = false;
    rejected(encode_session_reply(inactive));
    inactive.last_sequence = 1;
    REQUIRE(encode_session_reply(inactive));
    inactive.ready = true;
    rejected(encode_session_reply(inactive));
    inactive = snapshot;
    inactive.ownership_epoch = 0;
    rejected(encode_session_reply(inactive));
}
