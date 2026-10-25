#include <shark/storage/characters.hpp>

#include "durable_test_hooks.hpp"
#include "fixtures/storage/fixture.hpp"
#include "storage_recovery_vfs.hpp"
#include "store_test_hooks.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace {
using namespace shark;
using namespace shark::net;
using namespace shark::storage;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace fixture = storage_fixture;
using fixture::check;
using fixture::take;
using Options = std::map<std::string, std::string>;

template <typename T> void reject(const Result<T>& result) {
    check(!result, "Expected integrated denial was admitted");
    check(!result.error().message.empty() && !result.error().context.operation.empty(),
          "Integrated denial omitted owned diagnostics");
}
fs::path path(const std::string& value) {
    return fs::path{std::u8string{value.begin(), value.end()}};
}
std::string file(const fs::path& source) {
    std::ifstream input{source, std::ios::binary};
    check(static_cast<bool>(input), "Cannot read integrated fixture file");
    std::string result{std::istreambuf_iterator<char>{input}, {}};
    check(!result.empty() && result.size() <= 65536, "Integrated fixture file exceeds its bound");
    return result;
}
void publish(const fs::path& destination, const std::string& content) {
    check(!fs::exists(destination), "Integrated fixture refuses an existing publication");
    auto temporary = destination;
    temporary += ".partial";
    check(!fs::exists(temporary), "Integrated fixture refuses an existing temporary file");
    std::ofstream output{temporary, std::ios::binary};
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.close();
    check(output.good(), "Integrated fixture publication failed");
    fs::rename(temporary, destination);
}
SessionEnvelope envelope(const SessionSnapshot& session, std::uint64_t sequence) {
    return {session.session, session.ownership_epoch, sequence, session.character};
}
CharacterState checkpoint() {
    return {Name{"Integrated checkpoint"}, {fixture::world(), {55, -8, 9}, {0.5, 1.25, 777.75}}};
}
TransferRequest request(unsigned actor, std::uint64_t sequence) {
    const bool second = actor == 2;
    std::uint64_t revision = sequence == 1 ? 1 : sequence == 2 ? 2 : sequence == 3 ? 3 : 1;
    EconomyState quantity = sequence == 1   ? EconomyState{7, 2}
                            : sequence == 2 ? EconomyState{3, 1}
                            : sequence == 3 ? EconomyState{4, 1}
                                            : EconomyState{1, 0};
    if (second) {
        revision = 1;
        quantity = {5, 1};
    } else if (sequence == 11) {
        revision = 4;
        quantity = {2, 1};
    }
    return {{fixture::store_id(), fixture::account(actor), sequence},
            fixture::character(second ? 102U : 101U),
            fixture::character(second ? 101U : 102U),
            revision,
            revision,
            quantity};
}
EconomyRecord economic(unsigned actor, unsigned version) {
    constexpr std::array<EconomyState, 5> first{
        {{100, 20}, {93, 18}, {90, 17}, {86, 16}, {84, 15}}};
    constexpr std::array<EconomyState, 5> second{
        {{100, 20}, {107, 22}, {110, 23}, {114, 24}, {116, 25}}};
    check(version >= 1 && version <= 5, "Unexpected integrated economy revision");
    return {fixture::account(actor), fixture::character(actor + 100), version,
            (actor == 1 ? first : second)[version - 1]};
}
void exact_transfer(const TransferResult& result, unsigned actor, std::uint64_t sequence,
                    TransferStatus status, unsigned revision, bool replayed) {
    const TransferReceipt expected{request(actor, sequence), status, economic(actor, revision),
                                   economic(actor == 1 ? 2U : 1U, revision)};
    check(result.receipt == expected && result.replayed == replayed,
          "Integrated transfer outcome differs from the exact owned receipt");
}
std::string_view status_name(TransferStatus status) {
    return status == TransferStatus::committed ? "committed" : "stale_revision";
}
void prepare(const Options& options) {
    const auto database = path(options.at("--database"));
    const auto backup = path(options.at("--pre-backup"));
    fixture::create(database, 2, path(options.at("--storage-fixtures")));
    const auto legacy = fixture::snapshot(database, 2);
    take(Store::backup(database, backup, fixture::store_id(), fixture::world()));
    const auto migrated =
        take(Store::migrate(database, backup, fixture::store_id(), fixture::world()));
    check(migrated.before.schema_version == 2 && migrated.after.schema_version == 3 &&
              !migrated.already_current,
          "Integrated legacy migration did not run");
    {
        auto owner = take(Store::open(database, fixture::world()));
        fixture::verify_current(*owner, 2);
        for (unsigned actor : {1U, 2U})
            check(take(owner->initialize_economy(fixture::account(actor),
                                                 fixture::character(actor + 100), {100, 20})) ==
                      economic(actor, 1),
                  "Integrated trusted initialization differs");
    }
    std::cout << "{\"status\":\"PASS\",\"mode\":\"prepare\",\"phase\":\"prepare\",\"legacy_"
                 "snapshot\":"
              << legacy << ",\"pre_backup_snapshot\":" << fixture::snapshot(backup, 2)
              << ",\"snapshot\":" << fixture::snapshot(database, 3) << "}\n";
}
void restore(const Options& options) {
    const auto source = path(options.at("--database"));
    const auto pre = path(options.at("--pre-backup"));
    const auto snapshot = path(options.at("--snapshot"));
    const auto restored = path(options.at("--restored"));
    const auto before = fixture::snapshot(source, 3);
    check(take(Store::inspect_snapshot(pre, fixture::store_id(), fixture::world()))
                  .metadata.schema_version == 2,
          "Original pre-upgrade backup changed schema");
    // The historical backup is inspection-only after later acknowledged writes.
    // Only the latest snapshot is eligible for this quiesced same-point restart.
    const auto copied =
        take(Store::backup(source, snapshot, fixture::store_id(), fixture::world()));
    const auto candidate =
        take(Store::restore(snapshot, restored, fixture::store_id(), fixture::world()));
    check(copied.metadata == candidate.metadata && fixture::snapshot(source, 3) == before,
          "Isolated restore changed the authoritative source");
    std::cout << "{\"status\":\"PASS\",\"mode\":\"restore\",\"phase\":\"restore\",\"source_"
                 "snapshot\":"
              << before << ",\"pre_backup_snapshot\":" << fixture::snapshot(pre, 2)
              << ",\"snapshot\":" << fixture::snapshot(snapshot, 3)
              << ",\"candidate_snapshot\":" << fixture::snapshot(restored, 3) << "}\n";
}

class Hooks final {
  public:
    Hooks(std::unique_ptr<CharacterAuthority>& owner, test::RecoveryVfs& vfs, std::string phase)
        : owner_(owner), vfs_(vfs), phase_(std::move(phase)) {
        check(active_.load() == nullptr, "Overlapping integrated worker hook");
        active_.store(this);
        detail::set_runtime_worker_hook(callback);
    }
    ~Hooks() {
        release();
        owner_.reset();
        detail::set_runtime_worker_hook(nullptr);
        active_.store(nullptr);
    }
    void entered() {
        std::unique_lock lock{mutex_};
        check(changed_.wait_for(lock, 5s, [this] { return entered_; }),
              "Integrated worker barrier was not reached");
    }
    void release() {
        {
            const std::lock_guard lock{mutex_};
            released_ = true;
        }
        changed_.notify_all();
    }

  private:
    static bool commit_hook(detail::WritePhase phase) noexcept {
        if (phase == detail::WritePhase::after_commit) {
            constexpr std::string_view result =
                "{\"status\":\"PASS\",\"mode\":\"server\",\"phase\":\"commit-crash\","
                "\"event\":\"after_commit\",\"authenticated\":1,\"ready\":1,\"actor\":1,"
                "\"sequence\":2,\"currency\":3,\"items\":1,\"expected_source_revision\":2,"
                "\"expected_target_revision\":2}\n";
            std::fwrite(result.data(), 1, result.size(), stdout);
            std::fflush(stdout);
            std::_Exit(73);
        }
        return false;
    }
    static void callback(detail::RuntimeWorkerPhase phase, std::uint64_t sequence) noexcept {
        auto* hooks = active_.load();
        if (hooks == nullptr || phase != detail::RuntimeWorkerPhase::before_execute ||
            sequence == 0)
            return;
        if (hooks->phase_ == "commit-crash" && sequence == 1)
            detail::set_write_fault_hook(commit_hook);
        if (hooks->phase_ == "outage" && sequence == 1 && !hooks->armed_.exchange(true))
            hooks->vfs_.arm(test::IoFault::write);
        if (hooks->phase_ != "runtime" || sequence != 2)
            return;
        std::unique_lock lock{hooks->mutex_};
        hooks->entered_ = true;
        hooks->changed_.notify_all();
        hooks->changed_.wait(lock, [hooks] { return hooks->released_; });
    }
    inline static std::atomic<Hooks*> active_{nullptr};
    std::unique_ptr<CharacterAuthority>& owner_;
    test::RecoveryVfs& vfs_;
    std::string phase_;
    std::atomic<bool> armed_{false};
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_{false};
    bool released_{false};
};

struct Peer {
    std::unique_ptr<TlsTransport> transport;
    unsigned actor{};
    bool authenticated{false};
    bool ready{false};
    bool denied{false};
    bool replied{false};
    std::optional<SessionSnapshot> session{};
    std::uint64_t sequence{2};
    SessionEnvelope next() {
        check(session.has_value(), "Missing integrated session");
        return envelope(*session, sequence++);
    }
};
struct Evidence {
    unsigned peak_retained{};
    std::uint64_t vfs_failures{};
    unsigned recovery_fenced_sessions{};
    std::vector<std::string> checks;
    std::vector<std::string> outcomes;
    void outcome(const TransferResult& value, unsigned actor, std::uint64_t sequence,
                 TransferStatus status, unsigned revision, bool replayed = false) {
        exact_transfer(value, actor, sequence, status, revision, replayed);
        std::ostringstream text;
        text << "{\"actor\":" << actor << ",\"sequence\":" << sequence
             << ",\"status\":" << fixture::quoted(status_name(status))
             << ",\"replayed\":" << (replayed ? "true" : "false") << '}';
        outcomes.push_back(text.str());
    }
};
void settled(CharacterAuthority& owner) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (take(owner.poll(0ns)).unsettled != 0) {
        check(std::chrono::steady_clock::now() < deadline,
              "Integrated durable operation timed out");
        std::this_thread::yield();
    }
}
RuntimeCompletion completed(CharacterAuthority& owner, RuntimeTicket ticket, Peer& peer,
                            Scene& scene) {
    settled(owner);
    auto result = take(owner.take(ticket, *peer.transport, scene, 0ns));
    check(result.outcome.has_value(), "Integrated result was not settled");
    return result;
}
TransferResult transferred(CharacterAuthority& owner, RuntimeTicket ticket, Peer& peer,
                           Scene& scene, const TransferRequest& expected) {
    auto completion = completed(owner, ticket, peer, scene);
    check(completion.delivery == RuntimeDelivery::ready && completion.transfer == expected &&
              completion.outcome->has_value() &&
              std::holds_alternative<TransferResult>(completion.outcome->value()),
          "Integrated transfer completion lost its owned request/result");
    return std::get<TransferResult>(std::move(completion.outcome->value()));
}
void exercise(const std::string& phase, CharacterAuthority& owner, std::vector<Peer>& peers,
              Hooks& hooks, test::RecoveryVfs& vfs, Evidence& evidence) {
    const auto find = [&](unsigned actor) -> Peer& {
        auto found = std::find_if(peers.begin(), peers.end(),
                                  [actor](const auto& peer) { return peer.actor == actor; });
        check(found != peers.end(), "Integrated actor did not authenticate");
        return *found;
    };
    auto& first = find(1);
    auto scene = take(Scene::create(RegionSceneScope{
        fixture::world(),
        {RegionId{take(generate_stable_id())}, RegionInstanceId{take(generate_stable_id())}, 1}}));
    const auto entity_a = take(scene->create_entity({fixture::entity(201)}));
    const auto entity_b = take(scene->create_entity({fixture::entity(202)}));
    const auto scene_revision = take(scene->stats()).revision;
    if (phase == "runtime") {
        auto& second = find(2);
        const auto ticket = take(owner.submit_save(*first.transport, first.next(), *scene, entity_a,
                                                   7, checkpoint(), 5s, 0ns));
        const auto saved = completed(owner, ticket, first, *scene);
        auto expected = fixture::expected_registry().characters[0];
        expected.revision = 8;
        expected.state = checkpoint();
        check(saved.delivery == RuntimeDelivery::ready && saved.outcome->has_value() &&
                  std::get<CharacterRecord>(saved.outcome->value()) == expected,
              "Integrated runtime checkpoint differs");
        evidence.checks.push_back("checkpoint_saved");
        reject(owner.load(*first.transport, envelope(*first.session, 2), 0ns));
        evidence.checks.push_back("consumed_session_sequence_rejected");
        const auto request_a = request(1, 1);
        const auto a = take(owner.submit_transfer(*first.transport, first.next(), *scene, entity_a,
                                                  request_a, 5s, 0ns));
        hooks.entered();
        const auto request_b = request(2, 1);
        const auto b = take(owner.submit_transfer(*second.transport, second.next(), *scene,
                                                  entity_b, request_b, 5s, 0ns));
        const auto retained = take(owner.runtime_stats());
        check(retained.retained == 2 && retained.unsettled == 2,
              "Both authenticated transfers were not concurrently retained");
        evidence.peak_retained = 2;
        hooks.release();
        evidence.outcome(transferred(owner, a, first, *scene, request_a), 1, 1,
                         TransferStatus::committed, 2);
        evidence.outcome(transferred(owner, b, second, *scene, request_b), 2, 1,
                         TransferStatus::stale_revision, 2);
        evidence.checks.push_back("concurrent_transfer_and_stale_outcome");
    } else if (phase == "commit-crash") {
        take(owner.submit_transfer(*first.transport, first.next(), *scene, entity_a, request(1, 2),
                                   5s, 0ns));
        settled(owner);
        throw std::runtime_error{"Commit crash hook did not terminate the server"};
    } else if (phase == "outage") {
        auto& second = find(2);
        const auto loaded = take(owner.load(*first.transport, first.next(), 0ns));
        check(loaded.revision == 8 && loaded.state == checkpoint() &&
                  loaded.character == fixture::character() && loaded.entity == fixture::entity(),
              "Acknowledged runtime checkpoint was lost on process restart");
        evidence.checks.push_back("checkpoint_reopened");
        evidence.outcome(take(owner.transfer(*first.transport, first.next(), request(1, 2), 0ns)),
                         1, 2, TransferStatus::committed, 3, true);
        const auto original = request(1, 3);
        const auto ticket = take(owner.submit_transfer(*first.transport, first.next(), *scene,
                                                       entity_a, original, 5s, 0ns));
        const auto result = completed(owner, ticket, first, *scene);
        check(result.delivery == RuntimeDelivery::reconciliation_required &&
                  result.transfer == original && !result.outcome->has_value() &&
                  result.outcome->error().code == ErrorCode::io_failure &&
                  take(owner.runtime_stats()).reconciliation_required,
              "Actual SQLite outage was not retained as an uncertain owned failure");
        evidence.peak_retained = 1;
        evidence.vfs_failures = vfs.failures();
        check(evidence.vfs_failures > 0, "No actual SQLite write failed");
        evidence.checks.push_back("actual_vfs_write_failed");
        reject(owner.load(*first.transport, first.next(), 0ns));
        evidence.checks.push_back("faulted_admission_rejected");
        vfs.clear();
        take(owner.recover());
        reject(owner.load(*first.transport, first.next(), 0ns));
        reject(owner.load(*second.transport, second.next(), 0ns));
        evidence.recovery_fenced_sessions = 2;
        evidence.checks.push_back("explicit_recovery_fenced_sessions");
    } else if (phase == "recover") {
        auto& second = find(2);
        evidence.outcome(take(owner.transfer(*first.transport, first.next(), request(1, 3), 0ns)),
                         1, 3, TransferStatus::committed, 4);
        evidence.checks.push_back("original_outage_identity_reconciled");
        for (std::uint64_t sequence = 4; sequence <= 10; ++sequence)
            evidence.outcome(
                take(owner.transfer(*first.transport, first.next(), request(1, sequence), 0ns)), 1,
                sequence, TransferStatus::stale_revision, 4);
        evidence.outcome(take(owner.transfer(*first.transport, first.next(), request(1, 11), 0ns)),
                         1, 11, TransferStatus::committed, 5);
        const auto stream = take(owner.operation_stream(*first.transport, first.next(), 0ns));
        check(stream.high_watermark == 11 && stream.receipts.size() == 8 &&
                  stream.receipts.front().request.operation.sequence == 4 &&
                  stream.receipts.back().request.operation.sequence == 11,
              "Integrated retention window differs");
        evidence.checks.push_back("retention_window_advanced");
        take(owner.revoke_account(fixture::account(2)));
        reject(owner.economy(*second.transport, second.next(), 0ns));
        evidence.checks.push_back("durable_revocation_fenced_live_session");
    } else if (phase == "restored") {
        evidence.outcome(take(owner.transfer(*first.transport, first.next(), request(1, 11), 0ns)),
                         1, 11, TransferStatus::committed, 5, true);
        for (const std::uint64_t sequence : {1U, 3U}) {
            const auto result =
                owner.transfer(*first.transport, first.next(), request(1, sequence), 0ns);
            reject(result);
            check(result.error().context.operation == "storage.transfer.expired",
                  "Restore revived an expired durable operation identity");
        }
        evidence.checks.push_back("expired_operations_remain_expired");
        auto changed = request(1, 11);
        ++changed.quantity.currency;
        const auto result = owner.transfer(*first.transport, first.next(), changed, 0ns);
        reject(result);
        check(result.error().context.operation == "storage.transfer.mismatch",
              "Restore admitted a changed retained request");
        evidence.checks.push_back("changed_retained_payload_rejected");
        check(find(2).denied, "Restored revoked account logged in");
        evidence.checks.push_back("restored_revocation_rejected_login");
    } else {
        throw std::runtime_error{"Unknown integrated server phase"};
    }
    check(take(scene->stats()).revision == scene_revision,
          "Durable completion mutated the live Scene");
    evidence.checks.push_back("scene_unchanged");
}

void server(const Options& options) {
    const auto& phase = options.at("--phase");
    const auto database = path(options.at(phase == "restored" ? "--restored" : "--database"));
    const auto fixtures = path(options.at("--network-fixtures"));
    test::RecoveryVfs vfs;
    std::unique_ptr<CharacterAuthority> owner;
    Hooks hooks{owner, vfs, phase};
    SessionLimits limits;
    limits.commands_per_window = 128;
    limits.auth_attempts_per_window = 128;
    owner = take(
        CharacterAuthority::open_runtime(database, fixture::store_id(), fixture::world(), limits));
    auto listener = take(TlsListener::listen_loopback_authenticated(
        0, {file(fixtures / "server.pem"), file(fixtures / "server-key.pem")},
        file(fixtures / "ca.pem")));
    publish(path(options.at("--ready")), std::to_string(take(listener->port())) + "\n");
    const std::array certificates{
        take(certificate_fingerprint(file(fixtures / "session/client-a.pem"))),
        take(certificate_fingerprint(file(fixtures / "session/client-b.pem")))};
    const unsigned expected_peers = phase == "commit-crash" ? 1U : 2U;
    std::vector<Peer> peers;
    peers.reserve(expected_peers);
    SteadyMonotonicClock clock;
    unsigned authenticated = 0, login_accepted = 0, login_rejected = 0, stale_rejected = 0;
    unsigned ready = 0, closed = 0;
    bool exercised = false;
    Evidence evidence;
    while (clock.now() < 20s) {
        if (peers.size() < expected_peers) {
            auto accepted = take(listener->accept(clock.now()));
            if (accepted)
                peers.push_back({std::move(*accepted)});
        }
        for (auto& peer : peers) {
            if (!peer.transport)
                continue;
            auto polled = peer.transport->poll(clock.now(), 16);
            if (!polled || polled.value().state == TransportState::closed) {
                check(peer.replied || peer.denied,
                      "Integrated peer closed before its expected result");
                peer.transport.reset();
                ++closed;
                continue;
            }
            check(polled.value().operations <= 16, "Integrated transport exceeded poll budget");
            if (polled.value().state != TransportState::ready)
                continue;
            if (!peer.authenticated) {
                const auto identity = take(peer.transport->authenticated_peer());
                check(identity.certificate == certificates[0] ||
                          identity.certificate == certificates[1],
                      "Unexpected integrated client certificate");
                peer.actor = identity.certificate == certificates[0] ? 1U : 2U;
                check(std::count_if(peers.begin(), peers.end(),
                                    [&](const auto& value) {
                                        return value.authenticated && value.actor == peer.actor;
                                    }) == 0,
                      "Integrated client identity repeated");
                peer.authenticated = true;
                ++authenticated;
            }
            auto received = take(peer.transport->receive());
            if (!received)
                continue;
            check(received->kind == MessageKind::session_request,
                  "Integrated client sent an unexpected message kind");
            const auto command = take(decode_session_request(received->payload));
            auto result = owner->apply(*peer.transport, command, 0ns);
            if (std::holds_alternative<HeartbeatRequest>(command)) {
                check(phase != "runtime" && !peer.session, "Unexpected integrated heartbeat");
                reject(result);
                ++stale_rejected;
            } else if (const auto* login = std::get_if<LoginRequest>(&command)) {
                check(login->account == fixture::account(peer.actor) &&
                          login->character == fixture::character(peer.actor + 100),
                      "Integrated login identity differs from authenticated client");
                if (phase == "restored" && peer.actor == 2) {
                    reject(result);
                    peer.denied = true;
                    ++login_rejected;
                } else {
                    peer.session = take(result);
                    ++login_accepted;
                }
            } else if (std::holds_alternative<SetReadyRequest>(command)) {
                check(peer.session.has_value() && !peer.ready,
                      "Unexpected integrated ready request");
                peer.session = take(result);
                peer.ready = true;
                ++ready;
                continue; // Acknowledge only after the fixed trusted server work settles.
            } else {
                throw std::runtime_error{"Unexpected integrated session request"};
            }
            take(peer.transport->send(MessageKind::session_reply,
                                      take(encode_session_reply(public_session_reply(result)))));
        }
        const auto expected_ready = phase == "restored" ? 1U : expected_peers;
        if (!exercised && ready == expected_ready && authenticated == expected_peers &&
            (phase != "restored" || login_rejected == 1)) {
            exercise(phase, *owner, peers, hooks, vfs, evidence);
            exercised = true;
            for (auto& peer : peers) {
                if (!peer.ready)
                    continue;
                take(peer.transport->send(MessageKind::session_reply,
                                          take(encode_session_reply(*peer.session))));
                peer.replied = true;
            }
        }
        if (closed == expected_peers)
            break;
        std::this_thread::yield();
    }
    check(exercised && closed == expected_peers && authenticated == expected_peers,
          "Integrated server did not complete every client");
    owner.reset();
    std::cout << "{\"status\":\"PASS\",\"mode\":\"server\",\"phase\":" << fixture::quoted(phase)
              << ",\"authenticated\":" << authenticated << ",\"login_accepted\":" << login_accepted
              << ",\"login_rejected\":" << login_rejected
              << ",\"stale_session_rejected\":" << stale_rejected << ",\"ready\":" << ready
              << ",\"peak_retained\":" << evidence.peak_retained
              << ",\"vfs_failures\":" << evidence.vfs_failures
              << ",\"recovery_fenced_sessions\":" << evidence.recovery_fenced_sessions
              << ",\"checks\":[";
    for (std::size_t index = 0; index < evidence.checks.size(); ++index)
        std::cout << (index == 0 ? "" : ",") << fixture::quoted(evidence.checks[index]);
    std::cout << "],\"outcomes\":[";
    for (std::size_t index = 0; index < evidence.outcomes.size(); ++index)
        std::cout << (index == 0 ? "" : ",") << evidence.outcomes[index];
    std::cout << "],\"snapshot\":" << fixture::snapshot(database, 3) << "}\n";
}

void client(const Options& options) {
    const auto& phase = options.at("--phase");
    const auto& identity = options.at("--client");
    check(identity == "a" || identity == "b", "Unexpected integrated client identity");
    const unsigned actor = identity == "a" ? 1U : 2U;
    const auto fixtures = path(options.at("--network-fixtures"));
    const auto text = file(path(options.at("--ready")));
    unsigned port = 0;
    const auto number = std::from_chars(text.data(), text.data() + text.size() - 1, port);
    check(text.back() == '\n' && number.ec == std::errc{} &&
              number.ptr == text.data() + text.size() - 1 && port > 0 && port <= 65535,
          "Invalid integrated ready port");
    SteadyMonotonicClock clock;
    auto transport = take(TlsTransport::connect_loopback_authenticated(
        static_cast<std::uint16_t>(port), {file(fixtures / "ca.pem"), "shark.test"},
        {file(fixtures / (actor == 1 ? "session/client-a.pem" : "session/client-b.pem")),
         file(fixtures / (actor == 1 ? "session/client-a-key.pem" : "session/client-b-key.pem"))},
        clock.now()));
    const auto handshake_deadline = clock.now() + 5s;
    while (take(transport->poll(clock.now(), 16)).state != TransportState::ready) {
        check(clock.now() < handshake_deadline, "Integrated client handshake timed out");
        std::this_thread::yield();
    }
    unsigned accepted = 0, rejected = 0;
    const auto exchange = [&](const SessionRequest& command,
                              bool expect_disconnect = false) -> std::optional<SessionReply> {
        take(transport->send(MessageKind::session_request, take(encode_session_request(command))));
        const auto deadline = clock.now() + 12s;
        while (clock.now() < deadline) {
            auto polled = transport->poll(clock.now(), 16);
            if (!polled || polled.value().state == TransportState::closed) {
                check(expect_disconnect, "Integrated server disconnected before reply");
                return {};
            }
            auto received = take(transport->receive());
            if (received) {
                check(!expect_disconnect && received->kind == MessageKind::session_reply,
                      "Crash returned a success reply or unexpected wire kind");
                auto reply = take(decode_session_reply(received->payload));
                if (std::holds_alternative<SessionRejected>(reply)) {
                    check(received->payload == take(encode_session_reply(SessionRejected{})),
                          "Integrated denial leaked private durable state");
                    ++rejected;
                } else {
                    ++accepted;
                }
                return reply;
            }
            std::this_thread::yield();
        }
        throw std::runtime_error{"Integrated client reply timed out"};
    };
    const auto session_path = path(options.at(actor == 1 ? "--session-a" : "--session-b"));
    if (phase != "runtime") {
        const auto stored = file(session_path);
        const auto previous =
            take(decode_session_reply(std::as_bytes(std::span{stored.data(), stored.size()})));
        check(std::holds_alternative<SessionSnapshot>(previous),
              "Missing previous integrated session");
        const auto denial =
            exchange(HeartbeatRequest{envelope(std::get<SessionSnapshot>(previous), 99)});
        check(denial && std::holds_alternative<SessionRejected>(*denial),
              "Old process session was restored");
    }
    const auto login =
        exchange(LoginRequest{fixture::account(actor), fixture::character(actor + 100)});
    std::optional<SessionSnapshot> session;
    const bool revoked = phase == "restored" && actor == 2;
    const bool crash = phase == "commit-crash";
    if (revoked) {
        check(login && std::holds_alternative<SessionRejected>(*login),
              "Restored revocation did not reject login");
    } else {
        check(login && std::holds_alternative<SessionSnapshot>(*login),
              "Fresh integrated login failed");
        session = std::get<SessionSnapshot>(*login);
        const auto reply = exchange(SetReadyRequest{envelope(*session, 1), true}, crash);
        if (!crash) {
            check(reply && std::holds_alternative<SessionSnapshot>(*reply),
                  "Integrated ready failed");
            session = std::get<SessionSnapshot>(*reply);
        }
        if (phase == "runtime") {
            const auto encoded = take(encode_session_reply(*session));
            publish(session_path, {reinterpret_cast<const char*>(encoded.data()), encoded.size()});
        }
    }
    if (!crash)
        take(transport->close());
    std::cout << "{\"status\":\"PASS\",\"mode\":\"client\",\"phase\":" << fixture::quoted(phase)
              << ",\"client\":" << fixture::quoted(identity) << ",\"accepted\":" << accepted
              << ",\"rejected\":" << rejected
              << ",\"stale_rejected\":" << (phase != "runtime" ? "true" : "false")
              << ",\"disconnect_before_ready\":" << (crash ? "true" : "false")
              << ",\"session_saved\":" << (phase == "runtime" ? "true" : "false") << ",\"session\":"
              << (session ? fixture::quoted(session->session.value().to_string()) : "null")
              << "}\n";
}
} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        check(argc > 1 && argc % 2 == 1, "Expected named integrated probe options");
        Options options;
        for (int index = 1; index < argc; index += 2) {
            const auto key = fs::path{argv[index]}.u8string();
            const auto value = fs::path{argv[index + 1]}.u8string();
            check(options
                      .emplace(std::string{key.begin(), key.end()},
                               std::string{value.begin(), value.end()})
                      .second,
                  "Duplicate integrated probe option");
        }
        const auto& mode = options.at("--mode");
        if (mode == "prepare")
            prepare(options);
        else if (mode == "restore")
            restore(options);
        else if (mode == "server")
            server(options);
        else if (mode == "client")
            client(options);
        else
            throw std::runtime_error{"Unknown integrated probe mode"};
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::cout << "{\"status\":\"FAIL\"}\n";
        return 1;
    }
}
