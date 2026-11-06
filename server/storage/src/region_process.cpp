#include "shark/server/region_process.hpp"
#include "region_process_test_hooks.hpp"
#include "region_rpc_codec.hpp"
#include "shark/server/region_executor.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace shark::server {
namespace {
using namespace rpc_detail;
constexpr std::uint64_t magic = 0x5348524b52504331ULL;
thread_local std::optional<std::pair<Op, detail::RegionControlFault>> link_fault;

class RemoteAccess final : public GameplayAccess {
  public:
    std::span<const RegionPeer> peers;
    bool preparing{false};
    bool reached_admission{false};
    explicit RemoteAccess(std::span<const RegionPeer> values, bool prepare = false)
        : peers(values), preparing(prepare) {}
    Result<net::SessionSnapshot> authorize(std::uint64_t connection, const Scene& scene,
                                           EntityHandle entity, MonotonicTime) override {
        return boundary<net::SessionSnapshot>([&] {
            const auto reference = need(scene.reference(entity));
            for (const auto& peer : peers)
                if (peer.connection == connection && peer.session.active && peer.session.ready &&
                    reference.identity == EntityIdentity{peer.entity})
                    return peer.session;
            fail("Closed host did not supply a current entity/session observation");
        });
    }
    Result<net::SessionSnapshot>
    admit(std::uint64_t connection, const net::SessionEnvelope& envelope, MonotonicTime) override {
        return boundary<net::SessionSnapshot>([&] {
            for (const auto& peer : peers) {
                const auto& session = peer.session;
                if (peer.connection != connection || session.session != envelope.session ||
                    session.character != envelope.character ||
                    session.ownership_epoch != envelope.ownership_epoch || !session.ready ||
                    !session.active)
                    continue;
                if (preparing) {
                    if (session.last_sequence == std::numeric_limits<std::uint64_t>::max() ||
                        envelope.sequence != session.last_sequence + 1)
                        fail("Prepared input does not have the next session sequence");
                    reached_admission = true;
                    fail("Input validated; gateway admission is still required");
                }
                // This is the original post-admission receipt sent by the sole
                // gateway, including when replaying its retained uncertain route.
                if (session.last_sequence != envelope.sequence)
                    fail("Gateway admission receipt does not cover this exact input");
                reached_admission = true;
                return session;
            }
            fail("Missing original gateway admission receipt");
        });
    }
};

std::vector<std::byte> error_payload(const Error& error) {
    Writer out;
    out.u64(static_cast<std::uint64_t>(error.code));
    for (const auto* text : {&error.message, &error.context.operation}) {
        const auto bounded = std::string_view{*text}.substr(0, 2048);
        out.blob(std::as_bytes(std::span{bounded.data(), bounded.size()}));
    }
    return std::move(out.bytes);
}
Error read_error(std::span<const std::byte> bytes) {
    Reader in{bytes};
    const auto code = in.u64();
    if (code > static_cast<std::uint64_t>(ErrorCode::execution_failed))
        fail("Invalid worker error");
    const auto message = in.blob();
    const auto operation = in.blob();
    if (message.size() > 2048 || operation.size() > 2048)
        fail("Oversized worker error");
    Error result{static_cast<ErrorCode>(code),
                 {reinterpret_cast<const char*>(message.data()), message.size()},
                 {{reinterpret_cast<const char*>(operation.data()), operation.size()}}};
    in.end();
    return result;
}
struct Worker {
    std::unique_ptr<RegionExecutor> executor;
    std::optional<RegionProcessImage> installed;
    std::vector<RegionPeer> peers;
    std::vector<storage::RegionWriter> writers;
    std::uint64_t watermark{0};
    struct Prepared {
        Op kind;
        std::uint64_t route, connection;
        std::vector<std::byte> input;
    };
    std::optional<Prepared> prepared;
    void ready(bool allow_prepared = false) const {
        if (!executor || executor->phase() != RegionExecutorPhase::active ||
            (prepared && !allow_prepared))
            fail("Worker is not available for this operation");
    }
    std::vector<std::byte> capture() {
        return need(
            encode_region_process_image(need(executor->capture(peers, writers, watermark))));
    }
    Result<std::vector<std::byte>> apply(Op op, MonotonicTime now, Reader& in) {
        return boundary<std::vector<std::byte>>([&]() -> std::vector<std::byte> {
            if (op == Op::close) {
                in.end();
                executor.reset();
                installed.reset();
                prepared.reset();
                return {};
            }
            if (op == Op::stage) {
                if (executor)
                    fail("A process accepts only one staged incarnation");
                auto image = need(decode_region_process_image(in.blob()));
                in.end();
                auto candidate = need(RegionExecutor::stage(image, image.executor_instance, now));
                peers = image.peers;
                writers = image.writers;
                watermark = image.route_watermark;
                installed = std::move(image);
                executor = std::move(candidate);
                return {};
            }
            if (!executor)
                fail("Worker has no staged simulation");
            if (op == Op::activate) {
                if (executor->phase() != RegionExecutorPhase::staged || !installed)
                    fail("Worker cannot be activated in this phase");
                auto proof = need(decode_region_process_image(in.blob()));
                in.end();
                if (proof.writers.size() != writers.size())
                    fail("Incomplete activation owners");
                auto comparison = proof;
                comparison.writers = installed->writers;
                if (need(encode_region_process_image(comparison)) !=
                    need(encode_region_process_image(*installed)))
                    fail("Activation changed staged state");
                for (const auto& source : writers) {
                    const auto found = std::find_if(
                        proof.writers.begin(), proof.writers.end(),
                        [&](const auto& row) { return row.character == source.character; });
                    if (found == proof.writers.end() || found->store != source.store ||
                        found->entity != source.entity ||
                        found->instance != proof.executor_instance ||
                        found->epoch !=
                            source.epoch + (source.instance == found->instance ? 0U : 1U))
                        fail("Activation does not cover the staged writer successor");
                }
                writers = std::move(proof.writers);
                need(executor->activate(writers));
                installed.reset();
                return {};
            }
            if (op == Op::inspect) {
                in.end();
                return capture();
            }
            if (op == Op::freeze) {
                ready();
                in.end();
                need(executor->freeze());
                return capture();
            }
            if (op == Op::cancel) {
                ready(true);
                in.end();
                prepared.reset();
                return {};
            }
            if (op == Op::step) {
                ready();
                auto current = in.peers();
                in.end();
                std::vector<std::uint64_t> connections;
                for (const auto& peer : current)
                    connections.push_back(peer.connection);
                RemoteAccess access{current};
                const auto report = need(executor->step(access, connections, now));
                if (report.error)
                    throw Failure{*report.error};
                if (report.gameplay.error)
                    throw Failure{*report.gameplay.error};
                peers = std::move(current);
                return capture();
            }
            const bool prepare = op == Op::prepare_move || op == Op::prepare_combat;
            const bool commit = op == Op::commit_move || op == Op::commit_combat;
            if (!prepare && !commit)
                fail("Unknown control operation");
            ready(commit);
            const auto route = in.u64(), connection = in.u64();
            auto current = in.peers();
            const auto input = in.blob();
            in.end();
            if (watermark == std::numeric_limits<std::uint64_t>::max() || route != watermark + 1)
                fail("Nonconsecutive gateway route");
            const auto kind = (op == Op::prepare_move || op == Op::commit_move)
                                  ? Op::prepare_move
                                  : Op::prepare_combat;
            if (commit && (!prepared || prepared->route != route ||
                           prepared->connection != connection || prepared->kind != kind ||
                           !std::equal(input.begin(), input.end(), prepared->input.begin(),
                                       prepared->input.end())))
                fail("Commit changed prepared input");
            RemoteAccess access{current, prepare};
            const auto outcome =
                kind == Op::prepare_move
                    ? executor->submit_movement(access, connection,
                                                need(net::decode_movement_input(input)), now)
                    : executor->submit_combat(access, connection,
                                              need(net::decode_combat_action(input)), now);
            if (prepare) {
                if (outcome || !access.reached_admission) {
                    if (!outcome)
                        throw Failure{outcome.error()};
                    fail("Prepare unexpectedly mutated simulation");
                }
                prepared = Prepared{kind, route, connection, {input.begin(), input.end()}};
                return {};
            }
            need(outcome);
            prepared.reset();
            watermark = route;
            peers = std::move(current);
            return capture();
        });
    }
};

bool read_exact(std::span<std::byte> bytes) {
    while (!bytes.empty()) {
        const auto size = std::fread(bytes.data(), 1, bytes.size(), stdin);
        if (size == 0)
            return false;
        bytes = bytes.subspan(size);
    }
    return true;
}
bool write_frame(std::span<const std::byte> bytes) {
    if (bytes.size() > region_child_maximum_frame_bytes)
        return false;
    const auto size = static_cast<std::uint32_t>(bytes.size());
    const std::array<std::byte, 4> prefix{
        static_cast<std::byte>(size >> 24), static_cast<std::byte>(size >> 16),
        static_cast<std::byte>(size >> 8), static_cast<std::byte>(size)};
    return std::fwrite(prefix.data(), 1, prefix.size(), stdout) == prefix.size() &&
           std::fwrite(bytes.data(), 1, bytes.size(), stdout) == bytes.size() &&
           std::fflush(stdout) == 0;
}
} // namespace

struct RegionProcess::Impl {
    std::unique_ptr<RegionChild> child;
    std::uint64_t sequence{0};
    std::chrono::milliseconds timeout{std::chrono::seconds{10}};
    bool poisoned{false};
    Result<std::vector<std::byte>> call(Op op, Writer payload, MonotonicTime now) {
        return boundary<std::vector<std::byte>>([&] {
            if (poisoned || sequence == std::numeric_limits<std::uint64_t>::max() ||
                now.count() < 0)
                fail("Control stream is poisoned, exhausted or has an invalid clock");
            Writer out;
            out.u64(magic);
            out.u64(sequence + 1);
            out.u64(static_cast<std::uint64_t>(op));
            out.u64(static_cast<std::uint64_t>(now.count()));
            out.bytes.insert(out.bytes.end(), payload.bytes.begin(), payload.bytes.end());
            std::optional<detail::RegionControlFault> fault;
            if (link_fault && link_fault->first == op) {
                fault = link_fault->second;
                link_fault.reset();
            }
            if (fault == detail::RegionControlFault::drop_request) {
                poisoned = true;
                throw Failure{{ErrorCode::io_failure,
                               "Closed control request was lost",
                               {"server.region.process.link"}}};
            }
            auto response = child->request(out.bytes, timeout);
            if (!response) {
                poisoned = true;
                throw Failure{response.error()};
            }
            if (fault == detail::RegionControlFault::drop_reply) {
                poisoned = true;
                throw Failure{{ErrorCode::io_failure,
                               "Closed control response was lost",
                               {"server.region.process.link"}}};
            }
            poisoned = true;
            Reader in{response.value()};
            if (in.u64() != magic || in.u64() != sequence + 1) {
                poisoned = true;
                fail("Control reply identity mismatch");
            }
            const auto status = in.u64();
            const auto bytes = in.blob();
            in.end();
            if (status > 1)
                fail("Invalid control reply status");
            const auto error = status == 1 ? std::optional<Error>{read_error(bytes)} : std::nullopt;
            ++sequence;
            poisoned = false;
            if (error)
                throw Failure{*error};
            return std::vector<std::byte>{bytes.begin(), bytes.end()};
        });
    }
};
RegionProcess::RegionProcess(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
RegionProcess::~RegionProcess() = default;
Result<std::unique_ptr<RegionProcess>> RegionProcess::launch(const std::filesystem::path& server) {
    return boundary<std::unique_ptr<RegionProcess>>([&] {
        auto impl = std::make_unique<Impl>();
        impl->child = need(RegionChild::launch(server, {}, true));
        return std::unique_ptr<RegionProcess>{new RegionProcess{std::move(impl)}};
    });
}
Result<void> RegionProcess::stage(const RegionProcessImage& image, MonotonicTime now) {
    return boundary<void>([&] {
        Writer out;
        out.blob(need(encode_region_process_image(image)));
        need(impl_->call(Op::stage, std::move(out), now));
    });
}
Result<void> RegionProcess::activate(const RegionProcessImage& image, MonotonicTime now) {
    return boundary<void>([&] {
        Writer out;
        out.blob(need(encode_region_process_image(image)));
        need(impl_->call(Op::activate, std::move(out), now));
    });
}
Result<RegionProcessImage> RegionProcess::inspect(MonotonicTime now) {
    return boundary<RegionProcessImage>(
        [&] { return need(decode_region_process_image(need(impl_->call(Op::inspect, {}, now)))); });
}
Result<RegionProcessImage> RegionProcess::freeze(MonotonicTime now) {
    return boundary<RegionProcessImage>(
        [&] { return need(decode_region_process_image(need(impl_->call(Op::freeze, {}, now)))); });
}
namespace {
Writer input_payload(std::uint64_t route, std::uint64_t connection,
                     std::span<const RegionPeer> peers, std::span<const std::byte> input) {
    Writer out;
    out.u64(route);
    out.u64(connection);
    out.peers(peers);
    out.blob(input);
    return out;
}
} // namespace
Result<void> RegionProcess::prepare(std::uint64_t route, std::uint64_t connection,
                                    const net::MovementInput& input,
                                    std::span<const RegionPeer> peers, MonotonicTime now) {
    return boundary<void>([&] {
        need(impl_->call(
            Op::prepare_move,
            input_payload(route, connection, peers, need(net::encode_movement_input(input))), now));
    });
}
Result<void> RegionProcess::prepare(std::uint64_t route, std::uint64_t connection,
                                    const net::CombatAction& input,
                                    std::span<const RegionPeer> peers, MonotonicTime now) {
    return boundary<void>([&] {
        need(impl_->call(
            Op::prepare_combat,
            input_payload(route, connection, peers, need(net::encode_combat_action(input))), now));
    });
}
Result<RegionProcessImage> RegionProcess::commit(std::uint64_t route, std::uint64_t connection,
                                                 const net::MovementInput& input,
                                                 std::span<const RegionPeer> peers,
                                                 MonotonicTime now) {
    return boundary<RegionProcessImage>([&] {
        return need(decode_region_process_image(need(impl_->call(
            Op::commit_move,
            input_payload(route, connection, peers, need(net::encode_movement_input(input))),
            now))));
    });
}
Result<RegionProcessImage> RegionProcess::commit(std::uint64_t route, std::uint64_t connection,
                                                 const net::CombatAction& input,
                                                 std::span<const RegionPeer> peers,
                                                 MonotonicTime now) {
    return boundary<RegionProcessImage>([&] {
        return need(decode_region_process_image(need(impl_->call(
            Op::commit_combat,
            input_payload(route, connection, peers, need(net::encode_combat_action(input))),
            now))));
    });
}
Result<void> RegionProcess::cancel(MonotonicTime now) {
    return boundary<void>([&] { need(impl_->call(Op::cancel, {}, now)); });
}
Result<RegionProcessImage> RegionProcess::step(std::span<const RegionPeer> peers,
                                               MonotonicTime now) {
    return boundary<RegionProcessImage>([&] {
        Writer out;
        out.peers(peers);
        return need(decode_region_process_image(need(impl_->call(Op::step, std::move(out), now))));
    });
}
Result<void> RegionProcess::terminate() {
    return impl_->child->terminate();
}
Result<void> RegionProcess::retire(MonotonicTime now) {
    return boundary<void>([&] {
        if (need(impl_->child->alive()))
            need(impl_->call(Op::close, {}, now));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (need(impl_->child->alive())) {
            if (std::chrono::steady_clock::now() >= deadline) {
                need(impl_->child->terminate());
                fail("Worker retirement required forced termination");
            }
            std::this_thread::yield();
        }
        const auto status = need(impl_->child->exit_status());
        if (!status || *status != 0)
            fail("Region worker did not exit cleanly");
    });
}
Result<bool> RegionProcess::alive() const {
    return impl_->child->alive();
}
bool RegionProcess::channel_healthy() const noexcept {
    return !impl_->poisoned;
}
std::uint64_t RegionProcess::process_id() const noexcept {
    return impl_->child->process_id();
}
void RegionProcess::set_timeout(std::chrono::milliseconds timeout) {
    impl_->timeout = timeout;
}

void detail::set_region_control_fault(RegionControlPoint point, RegionControlFault fault) noexcept {
    Op op = Op::freeze;
    switch (point) {
    case RegionControlPoint::freeze:
        op = Op::freeze;
        break;
    case RegionControlPoint::activate:
        op = Op::activate;
        break;
    case RegionControlPoint::movement_commit:
        op = Op::commit_move;
        break;
    case RegionControlPoint::combat_commit:
        op = Op::commit_combat;
        break;
    case RegionControlPoint::step:
        op = Op::step;
        break;
    }
    link_fault = std::pair{op, fault};
}
void detail::clear_region_control_fault() noexcept {
    link_fault.reset();
}

int run_region_worker() {
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1)
        return 2;
#endif
    try {
        Worker worker;
        std::uint64_t sequence = 0;
        std::vector<std::byte> last_request, last_reply;
        for (;;) {
            std::array<std::byte, 4> prefix{};
            if (!read_exact(prefix))
                return std::feof(stdin) ? 0 : 1;
            std::size_t length = 0;
            for (const auto value : prefix)
                length = (length << 8) | std::to_integer<unsigned>(value);
            if (length < 32 || length > region_child_maximum_frame_bytes)
                return 2;
            std::vector<std::byte> bytes(length);
            if (!read_exact(bytes))
                return 2;
            Reader in{bytes};
            if (in.u64() != magic)
                return 2;
            const auto request_sequence = in.u64();
            if (request_sequence == sequence && bytes == last_request) {
                if (!write_frame(last_reply))
                    return 1;
                continue;
            }
            if (sequence == std::numeric_limits<std::uint64_t>::max() ||
                request_sequence != sequence + 1)
                return 2;
            const auto op = static_cast<Op>(in.u64());
            const auto now = in.time();
            const auto result = worker.apply(op, now, in);
            Writer out;
            out.u64(magic);
            out.u64(request_sequence);
            out.u64(result ? 0 : 1);
            out.blob(result ? result.value() : error_payload(result.error()));
            sequence = request_sequence;
            last_request = std::move(bytes);
            last_reply = std::move(out.bytes);
            if (!write_frame(last_reply))
                return 1;
            if (op == Op::close && result)
                return 0;
        }
    } catch (...) {
        return 2;
    }
}
} // namespace shark::server
