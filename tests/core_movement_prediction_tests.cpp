#include <shark/net/movement_prediction.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>

namespace {
using namespace shark;
using namespace shark::net;
using namespace std::chrono_literals;

template <typename T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().context.operation << ": " << result.error().message);
    }
    REQUIRE(result);
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result) {
        INFO(result.error().context.operation << ": " << result.error().message);
    }
    REQUIRE(result);
}
template <typename T> T id(unsigned value) {
    std::string text = "b0400000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[value & 15U];
        value >>= 4U;
    }
    return T{take(StableId::parse(text))};
}
SessionSnapshot session() {
    return {id<SessionId>(1), id<AccountId>(2), id<CharacterId>(3), 1, 1, true, true};
}
SessionEnvelope envelope(std::uint64_t sequence) {
    const auto value = session();
    return {value.session, value.ownership_epoch, sequence, value.character};
}
MovementState initial() {
    return {session().session, 1, id<MovementStreamId>(4), 10, 0, {}, {}};
}
std::unique_ptr<MovementPredictor> predictor() {
    return take(MovementPredictor::create(session(), initial(), {0, 8}));
}
void same(const MovementPredictionSnapshot& a, const MovementPredictionSnapshot& b) {
    REQUIRE(a.authoritative == b.authoritative);
    REQUIRE(a.predicted == b.predicted);
    REQUIRE(a.pending_inputs == b.pending_inputs);
    REQUIRE(a.revision == b.revision);
    REQUIRE(a.reconciliation_required == b.reconciliation_required);
}
std::string pem(const char* name) {
    std::ifstream input{std::filesystem::path{SHARK_NETWORK_FIXTURE_DIRECTORY} / name,
                        std::ios::binary};
    REQUIRE(input);
    return {std::istreambuf_iterator<char>{input}, {}};
}
struct Pair {
    std::unique_ptr<TlsListener> listener;
    std::unique_ptr<TlsTransport> client;
    std::unique_ptr<TlsTransport> server;
    explicit Pair(std::size_t queued_frames = 32) {
        TransportLimits limits;
        limits.queue_frames = queued_frames;
        listener = take(TlsListener::listen_loopback_authenticated(
            0, {pem("server.pem"), pem("server-key.pem")}, pem("ca.pem"), limits));
        client = take(TlsTransport::connect_loopback_authenticated(
            take(listener->port()), {pem("ca.pem"), "shark.test"},
            {pem("session/client-a.pem"), pem("session/client-a-key.pem")}, 0ns, limits));
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
    Frame receive(bool at_server) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            take(client->poll(0ns));
            take(server->poll(0ns));
            auto frame = take((at_server ? server : client)->receive());
            if (frame)
                return std::move(*frame);
            std::this_thread::yield();
        }
    }
};
} // namespace

TEST_CASE("Prediction prepares exact input and records one actual TLS admission",
          "[movement-prediction]") {
    Pair pair;
    auto owner = predictor();
    const auto before = take(owner->snapshot());
    const auto prepared = take(owner->prepare(envelope(2), 1000, 0));
    REQUIRE(prepared.input().input_sequence == 1);
    REQUIRE(take(decode_movement_input(prepared.bytes())) == prepared.input());
    same(take(owner->snapshot()), before);
    REQUIRE(take(owner->send(*pair.client, prepared)) == PredictionSendResult::admitted);
    const auto admitted = take(owner->snapshot());
    REQUIRE(admitted.pending_inputs == 1);
    REQUIRE(admitted.predicted.position.x == Catch::Approx(0.3).margin(1e-12));
    const auto queued = take(pair.client->stats()).send_queue_frames;
    REQUIRE(take(owner->send(*pair.client, prepared)) == PredictionSendResult::duplicate);
    REQUIRE(take(pair.client->stats()).send_queue_frames == queued);
    same(take(owner->snapshot()), admitted);
    const auto wire = pair.receive(true);
    REQUIRE(wire.kind == MessageKind::movement_input);
    REQUIRE(take(decode_movement_input(wire.payload)) == prepared.input());
    auto correction = initial();
    correction.completed_tick = 11;
    correction.last_input = 1;
    correction.position.x = 0.25;
    take(pair.server->send(MessageKind::movement_state,
                           take(encode_movement_reply(MovementReply{correction}))));
    const auto returned = pair.receive(false);
    REQUIRE(returned.kind == MessageKind::movement_state);
    REQUIRE(take(owner->apply(take(decode_movement_reply(returned.payload)))) ==
            PredictionCorrectionResult::applied);
    REQUIRE(take(owner->snapshot()).pending_inputs == 0);
    REQUIRE(take(owner->snapshot()).predicted.position.x == 0.25);
    const auto sent = take(pair.client->stats()).frames_sent;
    REQUIRE(take(owner->send(*pair.client, prepared)) == PredictionSendResult::duplicate);
    REQUIRE(take(pair.client->stats()).frames_sent == sent);
    REQUIRE(take(pair.client->stats()).send_queue_frames == 0);
}

TEST_CASE("Prediction bounds input history and retries only a known failed FIFO admission",
          "[movement-prediction]") {
    SECTION("eight unacknowledged inputs impose backpressure without moving the server clock") {
        Pair pair;
        auto owner = predictor();
        const auto before = take(owner->snapshot());
        REQUIRE_FALSE(owner->prepare(envelope(2), 1000, 1000));
        auto foreign = envelope(2);
        foreign.character = id<CharacterId>(90);
        REQUIRE_FALSE(owner->prepare(foreign, 1000, 0));
        same(take(owner->snapshot()), before);
        for (std::uint64_t index = 0; index < 8; ++index) {
            // Other session commands may occupy the intervening shared numbers.
            const auto prepared = take(owner->prepare(envelope(2 + index * 2), 1000, 0));
            REQUIRE(prepared.input().input_sequence == index + 1);
            REQUIRE(take(owner->send(*pair.client, prepared)) == PredictionSendResult::admitted);
        }
        const auto full = take(owner->snapshot());
        REQUIRE(full.pending_inputs == 8);
        REQUIRE(full.authoritative.completed_tick == 10);
        REQUIRE(full.predicted.position.x == Catch::Approx(2.4).margin(1e-12));
        REQUIRE_FALSE(owner->prepare(envelope(18), 1000, 0));
        same(take(owner->snapshot()), full);
        auto correction = initial();
        correction.completed_tick = 11;
        correction.last_input = 1;
        correction.position.x = 0.3;
        REQUIRE(take(owner->apply(MovementReply{correction})) ==
                PredictionCorrectionResult::applied);
        REQUIRE(take(owner->snapshot()).pending_inputs == 7);
        const auto next = take(owner->prepare(envelope(18), 0, 0));
        REQUIRE(next.input().input_sequence == 9);
    }
    SECTION("a full transport queue preserves the exact prepared input for a later send") {
        Pair pair{1};
        auto owner = predictor();
        const auto first = take(owner->prepare(envelope(2), 1000, 0));
        REQUIRE(take(owner->send(*pair.client, first)) == PredictionSendResult::admitted);
        const auto second = take(owner->prepare(envelope(3), 0, -1000));
        const auto before = take(owner->snapshot());
        REQUIRE_FALSE(owner->send(*pair.client, second));
        same(take(owner->snapshot()), before);
        REQUIRE(pair.receive(true).kind == MessageKind::movement_input);
        REQUIRE(take(owner->send(*pair.client, second)) == PredictionSendResult::admitted);
        const auto received = pair.receive(true);
        REQUIRE(take(decode_movement_input(received.payload)) == second.input());
        REQUIRE(take(owner->snapshot()).pending_inputs == 2);
    }
}

TEST_CASE("Authoritative corrections replay bounded pending directions and reject stale fences",
          "[movement-prediction]") {
    Pair pair;
    auto owner = predictor();
    for (std::uint64_t index = 0; index < 3; ++index) {
        const auto prepared = take(
            owner->prepare(envelope(index + 2), static_cast<std::int16_t>(index == 2 ? 0 : 1000),
                           static_cast<std::int16_t>(index == 2 ? 1000 : 0)));
        REQUIRE(take(owner->send(*pair.client, prepared)) == PredictionSendResult::admitted);
    }
    const auto stale_preparation = take(owner->prepare(envelope(5), -1000, 0));
    auto correction = initial();
    correction.completed_tick = 11;
    correction.last_input = 1;
    correction.position.x = 0.1;
    REQUIRE(take(owner->apply(MovementReply{correction})) == PredictionCorrectionResult::applied);
    const auto corrected = take(owner->snapshot());
    REQUIRE(corrected.pending_inputs == 2);
    REQUIRE(corrected.predicted.position.x == Catch::Approx(0.4).margin(1e-12));
    REQUIRE(corrected.predicted.position.z == Catch::Approx(0.3).margin(1e-12));
    REQUIRE(take(owner->apply(MovementReply{correction})) == PredictionCorrectionResult::duplicate);
    same(take(owner->snapshot()), corrected);
    const auto queued = take(pair.client->stats()).send_queue_frames;
    REQUIRE_FALSE(owner->send(*pair.client, stale_preparation));
    REQUIRE(take(pair.client->stats()).send_queue_frames == queued);
    for (unsigned variant = 0; variant < 7; ++variant) {
        auto bad = correction;
        switch (variant) {
        case 0:
            bad.position.x = 1;
            break; // Same tick, different receipt.
        case 1:
            bad.completed_tick = 10;
            break;
        case 2:
            bad.stream = id<MovementStreamId>(90);
            break;
        case 3:
            bad.session = id<SessionId>(90);
            break;
        case 4:
            ++bad.ownership_epoch;
            break;
        case 5:
            ++bad.completed_tick;
            bad.last_input = 0;
            break;
        default:
            ++bad.completed_tick;
            bad.last_input = 3;
            break; // Two inputs in one tick.
        }
        REQUIRE_FALSE(owner->apply(MovementReply{bad}));
        same(take(owner->snapshot()), corrected);
    }
    auto beyond_sent = correction;
    beyond_sent.completed_tick = 100;
    beyond_sent.last_input = 4;
    REQUIRE_FALSE(owner->apply(MovementReply{beyond_sent}));
    auto another = predictor();
    const auto foreign = take(another->prepare(envelope(2), 1000, 0));
    REQUIRE_FALSE(owner->send(*pair.client, foreign));
    same(take(owner->snapshot()), corrected);
    correction.completed_tick = 100;
    correction.last_input = 3;
    correction.position = {0.1, 0, 0.2};
    REQUIRE(take(owner->apply(MovementReply{correction})) == PredictionCorrectionResult::applied);
    const auto settled = take(owner->snapshot());
    REQUIRE(settled.pending_inputs == 0);
    REQUIRE(settled.predicted.position == correction.position);
}

TEST_CASE("Prediction rejection and suspension preserve history without implicitly resuming",
          "[movement-prediction]") {
    Pair pair;
    auto owner = predictor();
    const auto prepared = take(owner->prepare(envelope(2), 1000, 0));
    REQUIRE(take(owner->send(*pair.client, prepared)) == PredictionSendResult::admitted);
    SECTION("generic server rejection") {
        REQUIRE(take(owner->apply(MovementReply{MovementRejected{}})) ==
                PredictionCorrectionResult::reconciliation_required);
    }
    SECTION("host reports an uncertain outcome") {
        take(owner->suspend());
    }
    SECTION("a valid server correction survives an out-of-bounds approximate replay") {
        auto edge = initial();
        edge.completed_tick = 11;
        edge.position.x = 8191.9;
        REQUIRE(take(owner->apply(MovementReply{edge})) ==
                PredictionCorrectionResult::reconciliation_required);
        REQUIRE(take(owner->snapshot()).authoritative == edge);
    }
    auto frozen = take(owner->snapshot());
    REQUIRE(frozen.reconciliation_required);
    REQUIRE(frozen.pending_inputs == 1);
    REQUIRE(frozen.predicted.position == frozen.authoritative.position);
    REQUIRE_FALSE(owner->prepare(envelope(3), 1000, 0));
    auto correction = initial();
    correction.completed_tick = frozen.authoritative.completed_tick + 1;
    correction.last_input = 1;
    correction.position.x = 0.2;
    REQUIRE(take(owner->apply(MovementReply{correction})) == PredictionCorrectionResult::applied);
    frozen = take(owner->snapshot());
    REQUIRE(frozen.reconciliation_required);
    REQUIRE(frozen.pending_inputs == 0);
    REQUIRE(frozen.predicted.position == correction.position);
    REQUIRE_FALSE(owner->prepare(envelope(3), 0, 0));
    REQUIRE_FALSE(MovementPredictor::create(session(), correction, {0, 8}));
    auto fresh = initial();
    fresh.stream = id<MovementStreamId>(91);
    auto replacement = take(MovementPredictor::create(session(), fresh, {0, 8}));
    REQUIRE_FALSE(replacement->apply(MovementReply{correction}));
    const auto next = take(replacement->prepare(envelope(3), 1000, 0));
    REQUIRE(next.input().input_sequence == 1);
}

TEST_CASE("Prediction validates initial authority model bounds and creating-thread access",
          "[movement-prediction]") {
    auto not_ready = session();
    not_ready.ready = false;
    REQUIRE_FALSE(MovementPredictor::create(not_ready, initial()));
    auto inactive = session();
    inactive.active = false;
    REQUIRE_FALSE(MovementPredictor::create(inactive, initial()));
    auto wrong = initial();
    ++wrong.ownership_epoch;
    REQUIRE_FALSE(MovementPredictor::create(session(), wrong));
    wrong = initial();
    wrong.last_input = 1;
    REQUIRE_FALSE(MovementPredictor::create(session(), wrong));
    REQUIRE_FALSE(MovementPredictor::create(session(), initial(), {0, 0}));
    REQUIRE_FALSE(MovementPredictor::create(session(), initial(), {0, 9}));
    REQUIRE_FALSE(MovementPredictor::create(session(), initial(),
                                            {std::numeric_limits<double>::quiet_NaN(), 8}));
    auto edge = initial();
    edge.position.x = 8191.9;
    auto bounded = take(MovementPredictor::create(session(), edge, {0, 8}));
    const auto before = take(bounded->snapshot());
    REQUIRE_FALSE(bounded->prepare(envelope(2), 1000, 0));
    same(take(bounded->snapshot()), before);
    bool rejected = false;
    std::thread foreign{[&] { rejected = !bounded->snapshot(); }};
    foreign.join();
    REQUIRE(rejected);
}

TEST_CASE("Prediction gravity is an explicit approximate three-substep model",
          "[movement-prediction]") {
    Pair pair;
    SECTION("gravity from rest uses independent fixed-step expected values") {
        auto owner = take(MovementPredictor::create(session(), initial()));
        const auto prepared = take(owner->prepare(envelope(2), 0, 0));
        REQUIRE(take(owner->send(*pair.client, prepared)) == PredictionSendResult::admitted);
        const auto state = take(owner->snapshot());
        REQUIRE(state.predicted.position.y == Catch::Approx(-0.01635).margin(1e-12));
        REQUIRE(state.predicted.velocity.y == Catch::Approx(-0.4905).margin(1e-12));
    }
    SECTION("terminal vertical motion retains headroom for a horizontal direction") {
        auto falling = initial();
        falling.velocity.y = -256;
        auto owner = take(MovementPredictor::create(session(), falling, {0, 8}));
        const auto prepared = take(owner->prepare(envelope(2), 1000, 0));
        REQUIRE(take(owner->send(*pair.client, prepared)) == PredictionSendResult::admitted);
        const auto state = take(owner->snapshot());
        REQUIRE(state.predicted.position.y == Catch::Approx(-12.5).margin(1e-12));
        REQUIRE(state.predicted.velocity.y == -250);
        REQUIRE(state.predicted.position.x == Catch::Approx(0.3).margin(1e-12));
    }
}
