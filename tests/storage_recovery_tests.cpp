#include <shark/storage/characters.hpp>

#include "durable_test_hooks.hpp"
#include "store_test_hooks.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace {
using namespace shark;
using namespace shark::net;
using namespace shark::storage;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

template <typename T> T value(Result<T> result) {
    if (!result) {
        INFO(result.error().context.operation << ": " << result.error().message);
        REQUIRE(result);
    }
    return std::move(result).value();
}
void value(Result<void> result) {
    if (!result) {
        INFO(result.error().context.operation << ": " << result.error().message);
        REQUIRE(result);
    }
}
template <typename T> void rejected(const Result<T>& result) {
    REQUIRE_FALSE(result);
    CHECK_FALSE(result.error().message.empty());
    CHECK_FALSE(result.error().context.operation.empty());
}
StableId id(const char* text) {
    return value(StableId::parse(text));
}
WorldId world() {
    return WorldId{id("00000000-0000-4000-8000-000000000001")};
}
AccountId account(bool other = false) {
    return AccountId{id(other ? "00000000-0000-4000-8000-000000000102"
                              : "00000000-0000-4000-8000-000000000101")};
}
CharacterState state(bool changed = false) {
    return {Name{changed ? "Recovered checkpoint" : "Initial checkpoint"},
            {world(), {changed ? 100 : -100, 2, 3}, {0.125, 4.5, 1023.75}}};
}
std::string pem(const fs::path& name) {
    std::ifstream input{fs::path{SHARK_NETWORK_FIXTURE_DIRECTORY} / name, std::ios::binary};
    REQUIRE(input);
    return {std::istreambuf_iterator<char>{input}, {}};
}
CertificateFingerprint certificate(bool other = false) {
    return value(
        certificate_fingerprint(pem(other ? "session/client-b.pem" : "session/client-a.pem")));
}
SessionLimits session_limits() {
    SessionLimits limits;
    limits.auth_attempts_per_window = 128;
    limits.commands_per_window = 128;
    return limits;
}
struct Pair {
    std::unique_ptr<TlsListener> listener;
    std::unique_ptr<TlsTransport> client;
    std::unique_ptr<TlsTransport> server;
    explicit Pair(bool other = false) {
        listener = value(TlsListener::listen_loopback_authenticated(
            0, {pem("server.pem"), pem("server-key.pem")}, pem("ca.pem")));
        client = value(TlsTransport::connect_loopback_authenticated(
            value(listener->port()), {pem("ca.pem"), "shark.test"},
            {pem(other ? "session/client-b.pem" : "session/client-a.pem"),
             pem(other ? "session/client-b-key.pem" : "session/client-a-key.pem")},
            0ns));
        const auto end = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            if (std::chrono::steady_clock::now() >= end)
                FAIL("mTLS handshake exceeded the fixture deadline");
            if (!server) {
                auto accepted = value(listener->accept(0ns));
                if (accepted)
                    server = std::move(*accepted);
            }
            value(client->poll(0ns));
            if (server) {
                value(server->poll(0ns));
                if (value(client->stats()).state == TransportState::ready &&
                    value(server->stats()).state == TransportState::ready)
                    break;
            }
            std::this_thread::yield();
        }
    }
};
SessionEnvelope envelope(const SessionSnapshot& session, std::uint64_t sequence) {
    return {session.session, session.ownership_epoch, sequence, session.character};
}
std::unique_ptr<Scene> region() {
    return value(
        Scene::create(RegionSceneScope{world(),
                                       {RegionId{value(generate_stable_id())},
                                        RegionInstanceId{value(generate_stable_id())}, 1}}));
}
struct Fixture {
    fs::path parent{fs::current_path()};
    fs::path root{parent / ("shark-recovery-" + value(generate_stable_id()).to_string())};
    fs::path path{root / fs::path{u8"Runtime \u4e16\u754c.db"}};
    StoreMetadata metadata{StoreId{value(generate_stable_id())}, world(), store_schema_version};
    CharacterRecord first{account(), CharacterId{value(generate_stable_id())},
                          WorldEntityId{value(generate_stable_id())}, 1, state()};
    CharacterRecord second{account(true), CharacterId{value(generate_stable_id())},
                           WorldEntityId{value(generate_stable_id())}, 1, state()};
    std::unique_ptr<Scene> scene{region()};
    EntityHandle entity;
    EntityHandle other_entity;
    std::unique_ptr<CharacterAuthority> owner;
    Fixture() {
        REQUIRE(root.is_absolute());
        REQUIRE(fs::create_directory(root));
        auto store = value(Store::create(path, world()));
        metadata = value(store->metadata());
        for (const bool other : {false, true}) {
            value(store->provision_account(account(other)));
            value(store->enroll_certificate(account(other), certificate(other)));
        }
        first = value(store->create_character(account(), 1, state()));
        second = value(store->create_character(account(true), 1, state()));
        value(store->initialize_economy(account(), first.character, {100, 20}));
        value(store->initialize_economy(account(true), second.character, {100, 20}));
        entity = value(scene->create_entity({first.entity}));
        other_entity = value(scene->create_entity({second.entity}));
    }
    ~Fixture() {
        owner.reset();
        if (root.is_absolute() && root.parent_path() == parent &&
            root.filename().string().starts_with("shark-recovery-")) {
            std::error_code error;
            fs::remove_all(root, error);
        }
    }
    void start(RuntimeLimits limits = {}, SessionLimits sessions = session_limits()) {
        owner =
            value(CharacterAuthority::open_runtime(path, metadata.id, world(), sessions, limits));
    }
    SessionSnapshot login(Pair& pair, bool other = false, MonotonicTime now = 0ns) {
        auto session = value(owner->apply(
            *pair.server, LoginRequest{account(other), other ? second.character : first.character},
            now));
        return value(owner->apply(*pair.server, SetReadyRequest{envelope(session, 1), true}, now));
    }
    TransferRequest transfer(bool other = false, std::uint64_t sequence = 1) const {
        return {{metadata.id, account(other), sequence},
                other ? second.character : first.character,
                other ? first.character : second.character,
                1,
                1,
                {7, 2}};
    }
    void settled(MonotonicTime now = 0ns) {
        const auto end = std::chrono::steady_clock::now() + 5s;
        while (value(owner->poll(now)).unsettled != 0) {
            if (std::chrono::steady_clock::now() >= end)
                FAIL("Persistence work did not settle within the fixture deadline");
            std::this_thread::yield();
        }
    }
};

// Declare after Fixture, before start(). Destruction always releases the worker
// before Fixture joins it, including when a Catch requirement unwinds the test.
class Gate final {
  public:
    explicit Gate(Fixture& fixture, detail::RuntimeWorkerPhase phase,
                  std::optional<detail::WritePhase> fault = {})
        : fixture_(fixture), phase_(phase), fault_(fault) {
        REQUIRE(active_.load() == nullptr);
        active_.store(this);
        detail::set_runtime_worker_hook(callback);
    }
    ~Gate() {
        release();
        fixture_.owner.reset();
        detail::set_runtime_worker_hook(nullptr);
        active_.store(nullptr);
    }
    void entered() {
        std::unique_lock lock{mutex_};
        REQUIRE(ready_.wait_for(lock, 5s, [this] { return entered_; }));
    }
    void release() {
        {
            const std::lock_guard lock{mutex_};
            released_ = true;
        }
        ready_.notify_all();
    }
    void release_on_shutdown() {
        release_on_shutdown_ = true;
    }
    void hide_store_on_close() {
        hide_store_ = true;
    }
    void restore_store() {
        REQUIRE_FALSE(move_error_);
        hide_store_ = false;
        std::error_code error;
        fs::rename(fixture_.root / "temporarily-offline.db", fixture_.path, error);
        REQUIRE_FALSE(error);
    }

  private:
    static void callback(detail::RuntimeWorkerPhase phase, std::uint64_t sequence) noexcept {
        auto* gate = active_.load();
        if (gate == nullptr)
            return;
        if (phase == detail::RuntimeWorkerPhase::closed && gate->hide_store_) {
            fs::rename(gate->fixture_.path, gate->fixture_.root / "temporarily-offline.db",
                       gate->move_error_);
            return;
        }
        if (phase == detail::RuntimeWorkerPhase::shutdown_requested && gate->release_on_shutdown_) {
            gate->release();
            return;
        }
        if (sequence != 1)
            return;
        if (phase == detail::RuntimeWorkerPhase::before_execute && gate->fault_) {
            selected_fault_ = *gate->fault_;
            detail::set_write_fault_hook([](detail::WritePhase point) noexcept {
                return selected_fault_ && point == *selected_fault_;
            });
        }
        if (phase != gate->phase_)
            return;
        std::unique_lock lock{gate->mutex_};
        gate->entered_ = true;
        gate->ready_.notify_all();
        gate->ready_.wait(lock, [gate] { return gate->released_; });
    }
    inline static std::atomic<Gate*> active_{nullptr};
    inline static thread_local std::optional<detail::WritePhase> selected_fault_;
    Fixture& fixture_;
    detail::RuntimeWorkerPhase phase_;
    std::optional<detail::WritePhase> fault_;
    std::mutex mutex_;
    std::condition_variable ready_;
    bool entered_{false};
    bool released_{false};
    bool release_on_shutdown_{false};
    bool hide_store_{false};
    std::error_code move_error_;
};

TransferResult transfer_outcome(const RuntimeCompletion& completion) {
    REQUIRE(completion.outcome);
    REQUIRE(*completion.outcome);
    REQUIRE(std::holds_alternative<TransferResult>(completion.outcome->value()));
    return std::get<TransferResult>(completion.outcome->value());
}
} // namespace

TEST_CASE("runtime shutdown joins dispatched writes and cancels queued writes",
          "[storage-recovery]") {
    Fixture f;
    Gate gate{f, detail::RuntimeWorkerPhase::before_execute};
    gate.release_on_shutdown();
    f.start({2});
    Pair first;
    Pair second{true};
    const auto first_session = f.login(first);
    const auto second_session = f.login(second, true);
    value(f.owner->submit_save(*first.server, envelope(first_session, 2), *f.scene, f.entity, 1,
                               state(true), 10s, 0ns));
    gate.entered();
    value(f.owner->submit_save(*second.server, envelope(second_session, 2), *f.scene,
                               f.other_entity, 1, state(true), 10s, 0ns));
    CHECK(value(f.owner->runtime_stats()).retained == 2);
    f.owner.reset();
    auto reopened = value(Store::open(f.path, world()));
    CHECK(value(reopened->character(account(), f.first.character)).revision == 2);
    CHECK(value(reopened->character(account(), f.first.character)).state == state(true));
    CHECK(value(reopened->character(account(true), f.second.character)).revision == 1);
    CHECK(value(reopened->character(account(true), f.second.character)).state == state());
}

TEST_CASE("new runtime submissions observe expired retained work before admission",
          "[storage-recovery]") {
    Fixture f;
    Gate gate{f, detail::RuntimeWorkerPhase::before_execute};
    f.start({2});
    Pair first;
    Pair second{true};
    const auto first_session = f.login(first);
    const auto second_session = f.login(second, true);
    const auto ticket = value(f.owner->submit_save(*first.server, envelope(first_session, 2),
                                                   *f.scene, f.entity, 1, state(true), 1s, 0ns));
    gate.entered();
    rejected(f.owner->submit_save(*second.server, envelope(second_session, 2), *f.scene,
                                  f.other_entity, 1, state(true), 10s, 1s));
    CHECK(value(f.owner->runtime_stats()).retained == 1);
    CHECK(value(f.owner->runtime_stats()).reconciliation_required);
    gate.release();
    f.settled(1s);
    const auto result = value(f.owner->take(ticket, *first.server, *f.scene, 1s));
    CHECK(result.delivery == RuntimeDelivery::timed_out);
    REQUIRE(result.outcome);
    REQUIRE(*result.outcome);
    CHECK(std::get<CharacterRecord>(result.outcome->value()).revision == 2);
}

TEST_CASE("runtime startup validates capacity identity world and creating thread",
          "[storage-recovery]") {
    Fixture f;
    rejected(CharacterAuthority::open_runtime(f.path, f.metadata.id, world(), {}, {0}));
    rejected(CharacterAuthority::open_runtime(f.path, f.metadata.id, world(), {},
                                              {maximum_runtime_pending + 1}));
    rejected(
        CharacterAuthority::open_runtime(f.path, StoreId{value(generate_stable_id())}, world()));
    rejected(CharacterAuthority::open_runtime(f.path, f.metadata.id,
                                              WorldId{value(generate_stable_id())}));
    rejected(CharacterAuthority::open_runtime(f.root / "missing.db", f.metadata.id, world()));
    f.start();
    CHECK(value(f.owner->metadata()) == f.metadata);
    CHECK(value(f.owner->runtime_stats()).capacity == maximum_runtime_pending);
    std::optional<Result<RuntimeStats>> wrong_thread;
    std::jthread worker{[&] { wrong_thread = f.owner->runtime_stats(); }};
    worker.join();
    REQUIRE(wrong_thread);
    rejected(*wrong_thread);
}

TEST_CASE("failed live recovery keeps admission closed until explicit successful reopen",
          "[storage-recovery]") {
    Fixture f;
    Gate gate{f, detail::RuntimeWorkerPhase::opened};
    f.start();
    Pair peer;
    const auto session = f.login(peer);
    gate.hide_store_on_close();
    rejected(f.owner->recover());
    CHECK(value(f.owner->runtime_stats()).reconciliation_required);
    rejected(f.owner->apply(*peer.server, HeartbeatRequest{envelope(session, 2)}, 0ns));
    rejected(f.owner->submit_save(*peer.server, envelope(session, 2), *f.scene, f.entity, 1,
                                  state(true), 10s, 0ns));
    gate.restore_store();
    value(f.owner->recover());
    CHECK_FALSE(value(f.owner->runtime_stats()).reconciliation_required);
    rejected(f.owner->apply(*peer.server, HeartbeatRequest{envelope(session, 2)}, 0ns));
    const auto fresh = f.login(peer);
    CHECK(value(f.owner->load(*peer.server, envelope(fresh, 2), 0ns)) == f.first);
}

TEST_CASE("worker storage faults settle queued operations without executing later writes",
          "[storage-recovery]") {
    for (const auto fault : {detail::WritePhase::before_commit, detail::WritePhase::after_commit}) {
        Fixture f;
        Gate gate{f, detail::RuntimeWorkerPhase::before_execute, fault};
        f.start({2});
        Pair first;
        Pair second{true};
        const auto a = f.login(first);
        const auto b = f.login(second, true);
        const auto first_ticket = value(f.owner->submit_save(
            *first.server, envelope(a, 2), *f.scene, f.entity, 1, state(true), 10s, 0ns));
        gate.entered();
        const auto second_ticket = value(f.owner->submit_save(
            *second.server, envelope(b, 2), *f.scene, f.other_entity, 1, state(true), 10s, 0ns));
        gate.release();
        f.settled();
        CHECK(value(f.owner->runtime_stats()).retained == 2);
        for (const auto& [ticket, connection] : {std::pair{first_ticket, first.server.get()},
                                                 std::pair{second_ticket, second.server.get()}}) {
            rejected(f.owner->recover());
            const auto result = value(f.owner->take(ticket, *connection, *f.scene, 0ns));
            CHECK(result.delivery == RuntimeDelivery::reconciliation_required);
            REQUIRE(result.outcome);
            rejected(*result.outcome);
        }
        value(f.owner->recover());
        const auto fresh_a = f.login(first);
        const auto fresh_b = f.login(second, true);
        const auto saved = value(f.owner->load(*first.server, envelope(fresh_a, 2), 0ns));
        CHECK(saved.revision == (fault == detail::WritePhase::after_commit ? 2 : 1));
        CHECK(saved.state == state(fault == detail::WritePhase::after_commit));
        CHECK(value(f.owner->load(*second.server, envelope(fresh_b, 2), 0ns)) == f.second);
    }
}

TEST_CASE("runtime saves deliver owned durable values without mutating the scene",
          "[storage-recovery]") {
    Fixture f;
    f.start();
    Pair peer;
    const auto session = f.login(peer);
    const auto before = value(f.scene->entity_state(f.entity));
    const auto ticket = value(f.owner->submit_save(*peer.server, envelope(session, 2), *f.scene,
                                                   f.entity, 1, state(true), 10s, 0ns));
    f.settled();
    const auto result = value(f.owner->take(ticket, *peer.server, *f.scene, 0ns));
    CHECK(result.delivery == RuntimeDelivery::ready);
    REQUIRE(result.outcome);
    REQUIRE(*result.outcome);
    const auto saved = std::get<CharacterRecord>(result.outcome->value());
    CHECK(saved.character == f.first.character);
    CHECK(saved.entity == f.first.entity);
    CHECK(saved.revision == 2);
    CHECK(saved.state == state(true));
    CHECK(value(f.scene->entity_state(f.entity)).name == before.name);
    CHECK(value(f.owner->load(*peer.server, envelope(session, 3), 0ns)) == saved);
    CHECK(value(f.owner->runtime_stats()).retained == 0);
    rejected(f.owner->take(ticket, *peer.server, *f.scene, 0ns));
}

TEST_CASE("runtime transfers reconcile exact economic retries without duplicate effects",
          "[storage-recovery]") {
    Fixture f;
    f.start();
    Pair peer;
    const auto session = f.login(peer);
    const auto request = f.transfer();
    const auto first = value(f.owner->submit_transfer(*peer.server, envelope(session, 2), *f.scene,
                                                      f.entity, request, 10s, 0ns));
    f.settled();
    const auto original = value(f.owner->take(first, *peer.server, *f.scene, 0ns));
    CHECK(original.delivery == RuntimeDelivery::ready);
    const auto committed = transfer_outcome(original);
    CHECK(committed.receipt.status == TransferStatus::committed);
    CHECK_FALSE(committed.replayed);
    const auto retry = value(f.owner->submit_transfer(*peer.server, envelope(session, 3), *f.scene,
                                                      f.entity, request, 10s, 0ns));
    f.settled();
    const auto replayed =
        transfer_outcome(value(f.owner->take(retry, *peer.server, *f.scene, 0ns)));
    CHECK(replayed.replayed);
    CHECK(replayed.receipt == committed.receipt);
    CHECK(value(f.owner->economy(*peer.server, envelope(session, 4), 0ns)).state ==
          EconomyState{93, 18});
}

TEST_CASE("runtime capacity includes unsettled and untaken work without spending rejected commands",
          "[storage-recovery]") {
    Fixture f;
    Gate gate{f, detail::RuntimeWorkerPhase::before_execute};
    f.start({1});
    Pair a;
    Pair b{true};
    const auto sa = f.login(a);
    const auto sb = f.login(b, true);
    const auto ticket = value(f.owner->submit_save(*a.server, envelope(sa, 2), *f.scene, f.entity,
                                                   1, state(true), 10s, 0ns));
    gate.entered();
    rejected(f.owner->submit_save(*b.server, envelope(sb, 2), *f.scene, f.other_entity, 1,
                                  state(true), 10s, 0ns));
    rejected(f.owner->submit_save(*a.server, envelope(sa, 3), *f.scene, f.entity, 1, state(true),
                                  10s, 0ns));
    rejected(f.owner->recover());
    rejected(f.owner->provision_account(AccountId{value(generate_stable_id())}));
    CHECK(value(f.owner->take(ticket, *a.server, *f.scene, 0ns)).delivery ==
          RuntimeDelivery::pending);
    gate.release();
    f.settled();
    CHECK(value(f.owner->runtime_stats()).retained == 1);
    rejected(f.owner->submit_save(*b.server, envelope(sb, 2), *f.scene, f.other_entity, 1,
                                  state(true), 10s, 0ns));
    CHECK(value(f.owner->take(ticket, *a.server, *f.scene, 0ns)).delivery ==
          RuntimeDelivery::ready);
    const auto next = value(f.owner->submit_save(*b.server, envelope(sb, 2), *f.scene,
                                                 f.other_entity, 1, state(true), 10s, 0ns));
    f.settled();
    CHECK(value(f.owner->take(next, *b.server, *f.scene, 0ns)).delivery == RuntimeDelivery::ready);
}

TEST_CASE("runtime deadlines retain unsettled work and never acknowledge a late commit",
          "[storage-recovery]") {
    for (const auto phase :
         {detail::RuntimeWorkerPhase::before_execute, detail::RuntimeWorkerPhase::before_publish}) {
        Fixture f;
        Gate gate{f, phase};
        f.start({1});
        Pair peer;
        const auto session = f.login(peer);
        const auto request = f.transfer();
        const auto ticket = value(f.owner->submit_transfer(*peer.server, envelope(session, 2),
                                                           *f.scene, f.entity, request, 1s, 0ns));
        gate.entered();
        CHECK(value(f.owner->poll(1s)).timed_out == 1);
        const auto waiting = value(f.owner->take(ticket, *peer.server, *f.scene, 1s));
        CHECK(waiting.delivery == RuntimeDelivery::timed_out);
        CHECK_FALSE(waiting.outcome);
        CHECK(value(f.owner->runtime_stats()).retained == 1);
        rejected(f.owner->recover());
        gate.release();
        f.settled(1s);
        const auto settled = value(f.owner->take(ticket, *peer.server, *f.scene, 1s));
        CHECK(settled.delivery == RuntimeDelivery::timed_out);
        CHECK(settled.transfer == request);
        CHECK(transfer_outcome(settled).receipt.status == TransferStatus::committed);
        value(f.owner->recover());
        rejected(f.owner->take(ticket, *peer.server, *f.scene, 1s));
        Pair fresh;
        const auto renewed = f.login(fresh, false, 1s);
        const auto retry =
            value(f.owner->transfer(*fresh.server, envelope(renewed, 2), request, 1s));
        CHECK(retry.replayed);
        CHECK(retry.receipt.source->state == EconomyState{93, 18});
    }
}

TEST_CASE("runtime late results cannot cross replacement sessions", "[storage-recovery]") {
    Fixture f;
    Gate gate{f, detail::RuntimeWorkerPhase::before_publish};
    f.start();
    Pair old;
    const auto first = f.login(old);
    const auto ticket = value(f.owner->submit_transfer(*old.server, envelope(first, 2), *f.scene,
                                                       f.entity, f.transfer(), 10s, 0ns));
    gate.entered();
    Pair fresh;
    const auto replacement = f.login(fresh);
    CHECK(replacement.session != first.session);
    gate.release();
    f.settled();
    CHECK(value(f.owner->take(ticket, *fresh.server, *f.scene, 0ns)).delivery ==
          RuntimeDelivery::fenced);
    CHECK(value(f.owner->economy(*fresh.server, envelope(replacement, 2), 0ns)).state ==
          EconomyState{93, 18});
}

TEST_CASE("runtime late results reject ended leases logout revocation and disconnect",
          "[storage-recovery]") {
    for (const int mode : {0, 1, 2, 3, 4}) {
        Fixture f;
        Gate gate{f, detail::RuntimeWorkerPhase::before_publish};
        auto limits = session_limits();
        limits.session_idle_timeout = 1s;
        f.start({}, limits);
        Pair peer;
        const auto session = f.login(peer);
        const auto ticket = value(f.owner->submit_save(*peer.server, envelope(session, 2), *f.scene,
                                                       f.entity, 1, state(true), 10s, 0ns));
        gate.entered();
        if (mode == 0)
            value(f.owner->apply(*peer.server, LogoutRequest{envelope(session, 3)}, 0ns));
        if (mode == 1)
            CHECK(value(f.owner->revoke_session(session.session)));
        if (mode == 2)
            value(peer.server->close());
        if (mode == 3)
            value(f.owner->apply(*peer.server, SetReadyRequest{envelope(session, 3), false}, 0ns));
        const auto now = mode == 4 ? 2s : 0s;
        gate.release();
        f.settled(now);
        CHECK(value(f.owner->take(ticket, *peer.server, *f.scene, now)).delivery ==
              RuntimeDelivery::fenced);
    }
}

TEST_CASE("runtime entity and region incarnations fence completed values", "[storage-recovery]") {
    for (const int mode : {0, 1, 2}) {
        Fixture f;
        Gate gate{f, detail::RuntimeWorkerPhase::before_publish};
        f.start();
        Pair peer;
        const auto session = f.login(peer);
        const auto ticket = value(f.owner->submit_save(*peer.server, envelope(session, 2), *f.scene,
                                                       f.entity, 1, state(true), 10s, 0ns));
        gate.entered();
        auto replacement = region();
        Scene* selected = f.scene.get();
        if (mode == 0) {
            value(f.scene->destroy_entity(f.entity));
            const auto reused = value(f.scene->create_entity({f.first.entity}));
            CHECK(reused != f.entity);
        } else if (mode == 1) {
            value(f.scene->close());
        } else {
            value(replacement->create_entity({f.first.entity}));
            selected = replacement.get();
        }
        gate.release();
        f.settled();
        CHECK(value(f.owner->take(ticket, *peer.server, *selected, 0ns)).delivery ==
              RuntimeDelivery::fenced);
    }
}

TEST_CASE("runtime rejects invalid scene ownership deadlines and tickets before mutation",
          "[storage-recovery]") {
    Fixture f;
    f.start();
    Pair peer;
    const auto session = f.login(peer);
    auto authored = value(Scene::create(AuthoredSceneScope{SceneId{value(generate_stable_id())}}));
    const auto authored_entity =
        value(authored->create_entity({AuthoredEntityId{value(generate_stable_id())}}));
    rejected(f.owner->submit_save(*peer.server, envelope(session, 2), *authored, authored_entity, 1,
                                  state(true), 10s, 0ns));
    rejected(f.owner->submit_save(*peer.server, envelope(session, 2), *f.scene, f.other_entity, 1,
                                  state(true), 10s, 0ns));
    rejected(f.owner->submit_save(*peer.server, envelope(session, 2), *f.scene, f.entity, 1,
                                  state(true), 0ns, 0ns));
    rejected(f.owner->submit_save(*peer.server, envelope(session, 2), *f.scene, f.entity, 1,
                                  state(true), 61s, 0ns));
    const auto ticket = value(f.owner->submit_save(*peer.server, envelope(session, 2), *f.scene,
                                                   f.entity, 1, state(true), 10s, 0ns));
    rejected(f.owner->take({RuntimeId{value(generate_stable_id())}, ticket.sequence}, *peer.server,
                           *f.scene, 0ns));
    rejected(f.owner->take({ticket.owner, ticket.sequence + 1}, *peer.server, *f.scene, 0ns));
    f.settled();
    CHECK(value(f.owner->take(ticket, *peer.server, *f.scene, 0ns)).delivery ==
          RuntimeDelivery::ready);
}

TEST_CASE("runtime uncertain commits quiesce admission until explicit settled recovery",
          "[storage-recovery]") {
    for (const auto fault : {detail::WritePhase::before_commit, detail::WritePhase::after_commit}) {
        Fixture f;
        Gate gate{f, detail::RuntimeWorkerPhase::before_publish, fault};
        f.start();
        Pair peer;
        const auto session = f.login(peer);
        const auto request = f.transfer();
        const auto ticket = value(f.owner->submit_transfer(*peer.server, envelope(session, 2),
                                                           *f.scene, f.entity, request, 10s, 0ns));
        gate.entered();
        gate.release();
        f.settled();
        CHECK(value(f.owner->runtime_stats()).reconciliation_required);
        rejected(f.owner->apply(*peer.server, HeartbeatRequest{envelope(session, 3)}, 0ns));
        rejected(f.owner->recover());
        const auto completion = value(f.owner->take(ticket, *peer.server, *f.scene, 0ns));
        CHECK(completion.delivery == RuntimeDelivery::reconciliation_required);
        CHECK(completion.transfer == request);
        REQUIRE(completion.outcome);
        rejected(*completion.outcome);
        value(f.owner->recover());
        CHECK_FALSE(value(f.owner->runtime_stats()).reconciliation_required);
        rejected(f.owner->apply(*peer.server, HeartbeatRequest{envelope(session, 3)}, 0ns));
        Pair fresh;
        const auto recovered = f.login(fresh);
        const auto outcome =
            value(f.owner->transfer(*fresh.server, envelope(recovered, 2), request, 0ns));
        CHECK(outcome.replayed == (fault == detail::WritePhase::after_commit));
        CHECK(outcome.receipt.source->state == EconomyState{93, 18});
        CHECK(outcome.receipt.target->state == EconomyState{107, 22});
    }
}

TEST_CASE("runtime stale save errors retain admission sequencing without poisoning storage",
          "[storage-recovery]") {
    Fixture f;
    f.start();
    Pair peer;
    const auto session = f.login(peer);
    const auto ticket = value(f.owner->submit_save(*peer.server, envelope(session, 2), *f.scene,
                                                   f.entity, 2, state(true), 10s, 0ns));
    f.settled();
    const auto failed = value(f.owner->take(ticket, *peer.server, *f.scene, 0ns));
    CHECK(failed.delivery == RuntimeDelivery::ready);
    REQUIRE(failed.outcome);
    rejected(*failed.outcome);
    CHECK_FALSE(value(f.owner->runtime_stats()).reconciliation_required);
    const auto retry = value(f.owner->submit_save(*peer.server, envelope(session, 3), *f.scene,
                                                  f.entity, 1, state(true), 10s, 0ns));
    f.settled();
    CHECK(value(f.owner->take(retry, *peer.server, *f.scene, 0ns)).delivery ==
          RuntimeDelivery::ready);
}

TEST_CASE("runtime admits two accounts but retains one operation per account",
          "[storage-recovery]") {
    Fixture f;
    Gate gate{f, detail::RuntimeWorkerPhase::before_execute};
    f.start({2});
    Pair a;
    Pair b{true};
    const auto sa = f.login(a);
    const auto sb = f.login(b, true);
    const auto first = value(f.owner->submit_transfer(*a.server, envelope(sa, 2), *f.scene,
                                                      f.entity, f.transfer(), 10s, 0ns));
    gate.entered();
    rejected(f.owner->submit_save(*a.server, envelope(sa, 3), *f.scene, f.entity, 1, state(true),
                                  10s, 0ns));
    const auto second = value(f.owner->submit_transfer(*b.server, envelope(sb, 2), *f.scene,
                                                       f.other_entity, f.transfer(true), 10s, 0ns));
    CHECK(value(f.owner->runtime_stats()).retained == 2);
    gate.release();
    f.settled();
    const auto committed = transfer_outcome(value(f.owner->take(first, *a.server, *f.scene, 0ns)));
    const auto stale = transfer_outcome(value(f.owner->take(second, *b.server, *f.scene, 0ns)));
    CHECK(committed.receipt.status == TransferStatus::committed);
    CHECK(stale.receipt.status == TransferStatus::stale_revision);
    CHECK(committed.receipt.source->state.currency + committed.receipt.target->state.currency ==
          200);
    CHECK(committed.receipt.source->state.item_units + committed.receipt.target->state.item_units ==
          40);
}

TEST_CASE("runtime completion admission permits later heartbeats from the same live session",
          "[storage-recovery]") {
    Fixture f;
    Gate gate{f, detail::RuntimeWorkerPhase::before_publish};
    f.start();
    Pair peer;
    const auto session = f.login(peer);
    const auto ticket = value(f.owner->submit_save(*peer.server, envelope(session, 2), *f.scene,
                                                   f.entity, 1, state(true), 10s, 0ns));
    gate.entered();
    value(f.owner->apply(*peer.server, HeartbeatRequest{envelope(session, 3)}, 1ns));
    gate.release();
    f.settled(1ns);
    CHECK(value(f.owner->take(ticket, *peer.server, *f.scene, 1ns)).delivery ==
          RuntimeDelivery::ready);
}

TEST_CASE("runtime monotonic observations reject regression without releasing pending capacity",
          "[storage-recovery]") {
    Fixture f;
    Gate gate{f, detail::RuntimeWorkerPhase::before_execute};
    f.start();
    Pair peer;
    const auto session = f.login(peer);
    rejected(f.owner->poll(-1ns));
    value(f.owner->poll(5ns));
    rejected(f.owner->submit_save(*peer.server, envelope(session, 2), *f.scene, f.entity, 1,
                                  state(true), 10s, 4ns));
    const auto ticket = value(f.owner->submit_save(*peer.server, envelope(session, 2), *f.scene,
                                                   f.entity, 1, state(true), 10s, 5ns));
    gate.entered();
    rejected(f.owner->poll(4ns));
    rejected(f.owner->take(ticket, *peer.server, *f.scene, 4ns));
    CHECK(value(f.owner->runtime_stats()).retained == 1);
    gate.release();
    f.settled(5ns);
    CHECK(value(f.owner->take(ticket, *peer.server, *f.scene, 5ns)).delivery ==
          RuntimeDelivery::ready);
}

TEST_CASE(
    "runtime blocking maintenance keeps newly created character ownership and revocation coherent",
    "[storage-recovery]") {
    Fixture f;
    f.start();
    Pair peer;
    const auto extra =
        value(f.owner->create_character(*peer.server, account(), 2, state(true), 0ns));
    const auto entity = value(f.scene->create_entity({extra.entity}));
    auto session =
        value(f.owner->apply(*peer.server, LoginRequest{account(), extra.character}, 0ns));
    session = value(f.owner->apply(*peer.server, SetReadyRequest{envelope(session, 1), true}, 0ns));
    const auto ticket = value(f.owner->submit_save(*peer.server, envelope(session, 2), *f.scene,
                                                   entity, 1, state(), 10s, 0ns));
    f.settled();
    CHECK(value(f.owner->take(ticket, *peer.server, *f.scene, 0ns)).delivery ==
          RuntimeDelivery::ready);
    value(f.owner->revoke_account(account()));
    rejected(f.owner->apply(*peer.server, HeartbeatRequest{envelope(session, 3)}, 0ns));
    value(f.owner->recover());
    Pair fresh;
    rejected(f.owner->apply(*fresh.server, LoginRequest{account(), extra.character}, 0ns));
}

TEST_CASE("runtime worker exceptions settle bounded outcomes and require reopen",
          "[storage-recovery]") {
    struct InjectException {
        InjectException() {
            detail::set_runtime_worker_exception(true);
        }
        ~InjectException() {
            detail::set_runtime_worker_exception(false);
        }
    } injection;
    Fixture f;
    f.start();
    Pair peer;
    const auto session = f.login(peer);
    const auto request = f.transfer();
    const auto ticket = value(f.owner->submit_transfer(*peer.server, envelope(session, 2), *f.scene,
                                                       f.entity, request, 10s, 0ns));
    f.settled();
    const auto result = value(f.owner->take(ticket, *peer.server, *f.scene, 0ns));
    CHECK(result.delivery == RuntimeDelivery::reconciliation_required);
    REQUIRE(result.outcome);
    rejected(*result.outcome);
    CHECK(result.transfer == request);
    detail::set_runtime_worker_exception(false);
    value(f.owner->recover());
    Pair fresh;
    const auto recovered = f.login(fresh);
    const auto transferred =
        value(f.owner->transfer(*fresh.server, envelope(recovered, 2), request, 0ns));
    CHECK_FALSE(transferred.replayed);
    CHECK(transferred.receipt.status == TransferStatus::committed);
}

TEST_CASE("runtime tickets cannot be reused after recovery or a new owner", "[storage-recovery]") {
    Fixture f;
    f.start();
    Pair peer;
    auto session = f.login(peer);
    const auto old = value(f.owner->submit_save(*peer.server, envelope(session, 2), *f.scene,
                                                f.entity, 1, state(true), 10s, 0ns));
    f.settled();
    value(f.owner->take(old, *peer.server, *f.scene, 0ns));
    value(f.owner->recover());
    Pair fresh;
    session = f.login(fresh);
    const auto next = value(f.owner->submit_save(*fresh.server, envelope(session, 2), *f.scene,
                                                 f.entity, 2, state(), 10s, 0ns));
    CHECK(next.owner != old.owner);
    rejected(f.owner->take(old, *fresh.server, *f.scene, 0ns));
    f.settled();
    CHECK(value(f.owner->take(next, *fresh.server, *f.scene, 0ns)).delivery ==
          RuntimeDelivery::ready);
    f.owner.reset();
    f.start();
    rejected(f.owner->take(next, *fresh.server, *f.scene, 0ns));
}
