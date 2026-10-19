#include "shark/net/replication.hpp"

#include <algorithm>
#include <limits>
#include <thread>
#include <type_traits>
#include <utility>

namespace shark::net {
namespace {
Error rejected(const char* operation, ErrorCode code = ErrorCode::conflict) {
    return {code, "Replication operation rejected", {.operation = operation}};
}

struct Window {
    MonotonicTime start{0};
    std::uint32_t attempts{0};
};

bool spend(Window& window, MonotonicTime now, MonotonicTime duration, std::uint32_t maximum) {
    if (now - window.start >= duration) {
        window.start = now;
        window.attempts = 0;
    }
    if (window.attempts >= maximum) {
        return false;
    }
    ++window.attempts;
    return true;
}

bool valid_limits(const ReplicationLimits& limits) {
    using namespace std::chrono_literals;
    return limits.entities_per_stream > 0 &&
           limits.entities_per_stream <= replication_max_entities && limits.streams > 0 &&
           limits.streams <= 64 && limits.acknowledgement_timeout > 0ns &&
           limits.acknowledgement_timeout <= 60s && limits.controls_per_window > 0 &&
           limits.controls_per_window <= 65536 && limits.control_window > 0ns &&
           limits.control_window <= 1h && limits.resyncs_per_window > 0 &&
           limits.resyncs_per_window <= 64 && limits.resync_window > 0ns &&
           limits.resync_window <= 1h;
}

struct SourceEntity {
    EntityHandle handle;
    ReplicatedPose pose;
};
struct Capture {
    std::uint64_t scene_instance;
    std::vector<SourceEntity> entities;
};

Result<SceneRevision> source_revision(Scene& scene) {
    // Scope and revision are present even for an empty page or a first-slot hole.
    // Do not use Scene::stats here: its retired-slot count scans the whole Scene.
    SceneQueryOptions options;
    options.fields = {false, false, false};
    options.page_size = 1;
    options.scan_budget = 1;
    auto page = scene.query_entities(options);
    if (!page || !std::holds_alternative<RegionSceneScope>(page.value().scope())) {
        return rejected("net.replication.scene");
    }
    return page.value().revision();
}

Result<Capture> capture(Scene& scene, std::span<const EntityHandle> selection,
                        std::size_t maximum) {
    if (selection.size() > maximum) {
        return rejected("net.replication.selection", ErrorCode::invalid_argument);
    }
    auto revision = source_revision(scene);
    if (!revision) {
        return revision.error();
    }
    Capture candidate{revision.value().scene_instance, {}};
    candidate.entities.reserve(selection.size());
    for (const auto handle : selection) {
        auto inspected = scene.inspect_entity(handle, {false, true, false}, revision.value());
        if (!inspected) {
            return rejected("net.replication.selection", ErrorCode::invalid_id);
        }
        const auto& matrix = inspected.value().entities().front().transform->world_matrix;
        ReplicatedPose pose;
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 4; ++column) {
                pose.coefficients[static_cast<std::size_t>(row * 4 + column)] = matrix[column][row];
            }
        }
        auto canonical = canonicalize_replicated_pose(pose);
        if (!canonical) {
            return rejected("net.replication.pose", ErrorCode::invalid_argument);
        }
        candidate.entities.push_back({handle, canonical.value()});
    }
    std::ranges::sort(candidate.entities, {}, &SourceEntity::handle);
    for (std::size_t index = 1; index < candidate.entities.size(); ++index) {
        if (candidate.entities[index - 1].handle == candidate.entities[index].handle) {
            return rejected("net.replication.selection", ErrorCode::invalid_argument);
        }
    }
    return candidate;
}
} // namespace

struct ReplicationServer::Impl {
    struct Mapping {
        EntityHandle handle;
        ReplicaId id;
    };
    struct Pending {
        ReplicationUpdate update;
        std::vector<ReplicaEntity> target;
        MonotonicTime deadline;
    };
    struct Stream {
        SessionSnapshot session;
        ReplicaSessionId id;
        std::uint64_t scene_instance;
        std::uint64_t last_id{0};
        std::uint64_t last_revision{0};
        std::uint64_t acknowledged_revision{0};
        bool force_baseline{true};
        // Latest issued mapping, not only ACKed state: an ACK may have been lost.
        std::vector<Mapping> mapping;
        std::vector<ReplicaEntity> acknowledged;
        std::optional<Pending> pending;
        Window controls;
        Window resyncs;
        bool managed{false};
    };

    explicit Impl(ReplicationLimits value) : limits(value) {
        streams.reserve(limits.streams);
    }
    Result<void> check_owner() const {
        if (std::this_thread::get_id() != owner) {
            return rejected("net.replication.owner");
        }
        return {};
    }
    Result<void> advance(MonotonicTime now) {
        if (now < MonotonicTime::zero() || now < last_time) {
            return rejected("net.replication.time", ErrorCode::invalid_argument);
        }
        last_time = now;
        return {};
    }

    Result<bool> prepare_candidate(Stream& candidate, const Capture& captured, MonotonicTime now) {
        std::vector<Mapping> mapping;
        std::vector<ReplicaEntity> target;
        mapping.reserve(captured.entities.size());
        target.reserve(captured.entities.size());
        for (const auto& source : captured.entities) {
            const auto found =
                std::ranges::find(candidate.mapping, source.handle, &Mapping::handle);
            ReplicaId id;
            if (found != candidate.mapping.end()) {
                id = found->id;
            } else {
                if (candidate.last_id == std::numeric_limits<std::uint64_t>::max()) {
                    return rejected("net.replication.identity");
                }
                id = ReplicaId{++candidate.last_id};
            }
            mapping.push_back({source.handle, id});
            target.push_back({id, source.pose});
        }
        std::ranges::sort(target, {}, &ReplicaEntity::id);
        std::vector<ReplicationRecord> changes;
        changes.reserve(candidate.acknowledged.size() + target.size());
        if (candidate.force_baseline) {
            for (const auto& entity : target) {
                changes.push_back({ReplicationChange::spawn, entity.id, entity.pose});
            }
        } else {
            for (const auto& previous : candidate.acknowledged) {
                if (std::ranges::find(target, previous.id, &ReplicaEntity::id) == target.end()) {
                    changes.push_back({ReplicationChange::despawn, previous.id, std::nullopt});
                }
            }
            for (const auto& entity : target) {
                const auto previous =
                    std::ranges::find(candidate.acknowledged, entity.id, &ReplicaEntity::id);
                if (previous == candidate.acknowledged.end()) {
                    changes.push_back({ReplicationChange::spawn, entity.id, entity.pose});
                } else if (previous->pose != entity.pose) {
                    changes.push_back({ReplicationChange::update, entity.id, entity.pose});
                }
            }
            if (changes.empty()) {
                return false;
            }
            std::ranges::sort(changes, {}, &ReplicationRecord::id);
        }
        auto revision = next_sequence(candidate.last_revision);
        if (!revision) {
            return rejected("net.replication.revision");
        }
        if (now.count() > std::numeric_limits<MonotonicTime::rep>::max() -
                              limits.acknowledgement_timeout.count()) {
            return rejected("net.replication.time", ErrorCode::invalid_argument);
        }
        ReplicationUpdate update{candidate.session.session,
                                 candidate.session.ownership_epoch,
                                 candidate.id,
                                 revision.value(),
                                 candidate.force_baseline ? 0 : candidate.acknowledged_revision,
                                 candidate.force_baseline,
                                 std::move(changes)};
        if (auto valid = validate_replication_update(update); !valid) {
            return rejected("net.replication.update", ErrorCode::invalid_argument);
        }
        candidate.mapping = std::move(mapping);
        candidate.pending =
            Pending{std::move(update), std::move(target), now + limits.acknowledgement_timeout};
        candidate.last_revision = revision.value();
        return true;
    }

    ReplicationLimits limits;
    const std::thread::id owner{std::this_thread::get_id()};
    MonotonicTime last_time{0};
    std::vector<Stream> streams;
};

ReplicationServer::ReplicationServer(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
ReplicationServer::~ReplicationServer() = default;

Result<std::unique_ptr<ReplicationServer>>
ReplicationServer::create(const ReplicationLimits& limits) {
    if (!valid_limits(limits)) {
        return rejected("net.replication.create", ErrorCode::invalid_configuration);
    }
    try {
        return std::unique_ptr<ReplicationServer>{
            new ReplicationServer{std::make_unique<Impl>(limits)}};
    } catch (...) {
        return rejected("net.replication.create", ErrorCode::execution_failed);
    }
}

Result<std::optional<ReplicationUpdate>>
ReplicationServer::prepare(Scene& scene, std::span<const EntityHandle> published,
                           SessionAuthority& sessions, TlsTransport& transport, MonotonicTime now) {
    if (auto owner = impl_->check_owner(); !owner) {
        return owner.error();
    }
    if (auto time = impl_->advance(now); !time) {
        return time.error();
    }
    auto current = sessions.current(transport, now);
    if (!current || !current.value().ready) {
        return rejected("net.replication.session");
    }
    try {
        auto captured = capture(scene, published, impl_->limits.entities_per_stream);
        if (!captured) {
            return captured.error();
        }
        auto existing =
            std::ranges::find(impl_->streams, current.value().account,
                              [](const Impl::Stream& stream) { return stream.session.account; });
        const bool same_session = existing != impl_->streams.end() &&
                                  existing->session.session == current.value().session;
        if (same_session) {
            if (existing->managed) {
                return rejected("net.replication.mode");
            }
            if (existing->scene_instance != captured.value().scene_instance) {
                return rejected("net.replication.scene");
            }
            if (existing->pending) {
                if (now >= existing->pending->deadline) {
                    return rejected("net.replication.ack-timeout");
                }
                return std::optional<ReplicationUpdate>{existing->pending->update};
            }
        } else if (existing == impl_->streams.end() &&
                   impl_->streams.size() >= impl_->limits.streams) {
            return rejected("net.replication.capacity");
        }
        auto candidate = [&]() -> Result<Impl::Stream> {
            if (same_session) {
                return *existing;
            }
            auto id = generate_stable_id();
            if (!id) {
                return rejected("net.replication.entropy", id.error().code);
            }
            const ReplicaSessionId stream_id{id.value()};
            if (std::ranges::find(impl_->streams, stream_id, &Impl::Stream::id) !=
                impl_->streams.end()) {
                return rejected("net.replication.entropy");
            }
            return Impl::Stream{current.value(),
                                stream_id,
                                captured.value().scene_instance,
                                0,
                                0,
                                0,
                                true,
                                {},
                                {},
                                {},
                                {now, 0},
                                {now, 0}};
        }();
        if (!candidate) {
            return candidate.error();
        }
        auto prepared = impl_->prepare_candidate(candidate.value(), captured.value(), now);
        if (!prepared) {
            return prepared.error();
        }
        if (!prepared.value()) {
            return std::optional<ReplicationUpdate>{};
        }
        // Allocate the returned copy before committing bounded owner state.
        std::optional<ReplicationUpdate> result{candidate.value().pending->update};
        if (existing == impl_->streams.end()) {
            impl_->streams.push_back(std::move(candidate).value());
        } else {
            *existing = std::move(candidate).value();
        }
        return result;
    } catch (...) {
        return rejected("net.replication.prepare", ErrorCode::execution_failed);
    }
}

Result<ReplicationPublication>
ReplicationServer::publish(Scene& scene, std::span<const EntityHandle> published,
                           SessionAuthority& sessions, TlsTransport& transport, MonotonicTime now,
                           std::size_t available_framed_bytes) {
    if (auto owner = impl_->check_owner(); !owner) {
        return owner.error();
    }
    if (auto time = impl_->advance(now); !time) {
        return time.error();
    }
    auto current = sessions.current(transport, now);
    if (!current || !current.value().ready) {
        return rejected("net.replication.session");
    }
    try {
        auto existing =
            std::ranges::find(impl_->streams, current.value().account,
                              [](const Impl::Stream& stream) { return stream.session.account; });
        const bool same_session = existing != impl_->streams.end() &&
                                  existing->session.session == current.value().session;
        if (same_session) {
            if (!existing->managed) {
                return rejected("net.replication.mode");
            }
            auto source = source_revision(scene);
            if (!source || source.value().scene_instance != existing->scene_instance) {
                return rejected("net.replication.scene");
            }
            if (existing->pending) {
                if (now >= existing->pending->deadline) {
                    return rejected("net.replication.ack-timeout");
                }
                return ReplicationPublication{ReplicationPublishStatus::pending_ack};
            }
        } else if (existing == impl_->streams.end() &&
                   impl_->streams.size() >= impl_->limits.streams) {
            return rejected("net.replication.capacity");
        }
        auto queued = transport.stats();
        if (!queued) {
            return queued.error();
        }
        if (queued.value().send_queue_frames != 0) {
            return ReplicationPublication{ReplicationPublishStatus::queue_limited};
        }
        auto captured = capture(scene, published, impl_->limits.entities_per_stream);
        if (!captured) {
            return captured.error();
        }
        auto candidate = [&]() -> Result<Impl::Stream> {
            if (same_session) {
                return *existing;
            }
            auto id = generate_stable_id();
            if (!id) {
                return rejected("net.replication.entropy", id.error().code);
            }
            const ReplicaSessionId stream_id{id.value()};
            if (std::ranges::find(impl_->streams, stream_id, &Impl::Stream::id) !=
                impl_->streams.end()) {
                return rejected("net.replication.entropy");
            }
            return Impl::Stream{current.value(),
                                stream_id,
                                captured.value().scene_instance,
                                0,
                                0,
                                0,
                                true,
                                {},
                                {},
                                {},
                                {now, 0},
                                {now, 0},
                                true};
        }();
        if (!candidate) {
            return candidate.error();
        }
        auto prepared = impl_->prepare_candidate(candidate.value(), captured.value(), now);
        if (!prepared) {
            return prepared.error();
        }
        if (!prepared.value()) {
            return ReplicationPublication{ReplicationPublishStatus::unchanged, 0,
                                          captured.value().entities.size(), 0};
        }
        const auto& pending = *candidate.value().pending;
        auto encoded = encode_replication_update(pending.update);
        if (!encoded) {
            return encoded.error();
        }
        const ReplicationPublication result{ReplicationPublishStatus::sent,
                                            wire_header_bytes + encoded.value().size(),
                                            pending.target.size(), pending.update.changes.size()};
        if (result.framed_bytes > available_framed_bytes) {
            return ReplicationPublication{ReplicationPublishStatus::byte_limited,
                                          result.framed_bytes, result.entities, result.records};
        }
        auto sent = transport.send(MessageKind::replication_state, encoded.value());
        if (!sent) {
            return sent.error();
        }
        // Capacity was reserved at creation; all potentially throwing work is
        // complete before admission. Commit cannot leave an untracked FIFO frame.
        static_assert(std::is_nothrow_move_constructible_v<Impl::Stream>);
        static_assert(std::is_nothrow_move_assignable_v<Impl::Stream>);
        if (existing == impl_->streams.end()) {
            impl_->streams.push_back(std::move(candidate).value());
        } else {
            *existing = std::move(candidate).value();
        }
        return result;
    } catch (...) {
        return rejected("net.replication.publish", ErrorCode::execution_failed);
    }
}

Result<ReplicationControlResult> ReplicationServer::control(SessionAuthority& sessions,
                                                            TlsTransport& transport,
                                                            const ReplicationControl& control,
                                                            MonotonicTime now) {
    if (auto owner = impl_->check_owner(); !owner) {
        return owner.error();
    }
    if (auto time = impl_->advance(now); !time) {
        return time.error();
    }
    auto current = sessions.current(transport, now);
    if (!current || !current.value().ready) {
        return rejected("net.replication.session");
    }
    auto found =
        std::ranges::find(impl_->streams, current.value().session,
                          [](const Impl::Stream& stream) { return stream.session.session; });
    if (found == impl_->streams.end()) {
        return rejected("net.replication.stream");
    }
    auto& stream = *found;
    if (!spend(stream.controls, now, impl_->limits.control_window,
               impl_->limits.controls_per_window)) {
        return rejected("net.replication.rate.control");
    }
    const auto envelope =
        std::visit([](const auto& value) -> SessionEnvelope { return value.authority; }, control);
    const auto stream_id =
        std::visit([](const auto& value) -> ReplicaSessionId { return value.stream; }, control);
    if (envelope.session != current.value().session ||
        envelope.ownership_epoch != current.value().ownership_epoch ||
        envelope.character != current.value().character || stream_id != stream.id) {
        return rejected("net.replication.ownership");
    }
    try {
        if (const auto* ack = std::get_if<ReplicationAck>(&control)) {
            const bool duplicate =
                ack->revision != 0 && ack->revision == stream.acknowledged_revision;
            if (!duplicate &&
                (!stream.pending || ack->revision != stream.pending->update.revision)) {
                return rejected("net.replication.ack");
            }
            if (!duplicate && now >= stream.pending->deadline) {
                return rejected("net.replication.ack-timeout");
            }
            std::vector<ReplicaEntity> target;
            if (!duplicate) {
                target = stream.pending->target;
            }
            auto admitted = sessions.apply(transport, HeartbeatRequest{envelope}, now);
            if (!admitted) {
                return rejected("net.replication.admission");
            }
            if (duplicate) {
                return ReplicationControlResult::duplicate;
            }
            stream.acknowledged.swap(target);
            stream.acknowledged_revision = ack->revision;
            stream.pending.reset();
            stream.force_baseline = false;
            return ReplicationControlResult::acknowledged;
        }
        const auto& resync = std::get<ReplicationResync>(control);
        if (!spend(stream.resyncs, now, impl_->limits.resync_window,
                   impl_->limits.resyncs_per_window)) {
            return rejected("net.replication.rate.resync");
        }
        if (resync.known_revision > stream.last_revision ||
            stream.last_revision == std::numeric_limits<std::uint64_t>::max()) {
            return rejected("net.replication.resync");
        }
        auto admitted = sessions.apply(transport, HeartbeatRequest{envelope}, now);
        if (!admitted) {
            return rejected("net.replication.admission");
        }
        stream.pending.reset();
        stream.force_baseline = true;
        return ReplicationControlResult::resync_requested;
    } catch (...) {
        return rejected("net.replication.control", ErrorCode::execution_failed);
    }
}

Result<bool> ReplicationServer::forget(SessionId session) {
    if (auto owner = impl_->check_owner(); !owner) {
        return owner.error();
    }
    return std::erase_if(impl_->streams, [session](const Impl::Stream& stream) {
               return stream.session.session == session;
           }) != 0;
}

Result<ReplicationStats> ReplicationServer::stats() const {
    if (auto owner = impl_->check_owner(); !owner) {
        return owner.error();
    }
    ReplicationStats result{impl_->streams.size(), 0, 0};
    for (const auto& stream : impl_->streams) {
        result.mapped_entities += stream.mapping.size();
        result.pending_updates += stream.pending.has_value() ? 1U : 0U;
    }
    return result;
}

struct ReplicaClient::Impl {
    Impl(const SessionSnapshot& value, std::size_t limit)
        : session(value.session), epoch(value.ownership_epoch), maximum(limit) {}
    Result<void> check_owner() const {
        if (std::this_thread::get_id() != owner) {
            return rejected("net.replica.owner");
        }
        return {};
    }
    const std::thread::id owner{std::this_thread::get_id()};
    SessionId session;
    std::uint64_t epoch;
    std::size_t maximum;
    std::optional<ReplicaSessionId> stream;
    std::uint64_t revision{0};
    std::uint64_t highest_id{0};
    bool has_baseline{false};
    std::vector<ReplicaEntity> entities;
    // Retain bounded lifecycle fences across explicit local cache loss.
    std::vector<ReplicaId> known_live;
    std::optional<ReplicationUpdate> latest;
};

ReplicaClient::ReplicaClient(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ReplicaClient::~ReplicaClient() = default;

Result<std::unique_ptr<ReplicaClient>> ReplicaClient::create(const SessionSnapshot& session,
                                                             std::size_t max_entities) {
    if (!session.active || !session.ready || session.ownership_epoch == 0 ||
        session.last_sequence == 0 || max_entities == 0 ||
        max_entities > replication_max_entities) {
        return rejected("net.replica.create", ErrorCode::invalid_argument);
    }
    try {
        return std::unique_ptr<ReplicaClient>{
            new ReplicaClient{std::make_unique<Impl>(session, max_entities)}};
    } catch (...) {
        return rejected("net.replica.create", ErrorCode::execution_failed);
    }
}

Result<ReplicaApplyResult> ReplicaClient::apply(const ReplicationUpdate& update) {
    if (auto owner = impl_->check_owner(); !owner) {
        return owner.error();
    }
    if (auto valid = validate_replication_update(update); !valid) {
        return rejected("net.replica.update", ErrorCode::invalid_argument);
    }
    if (update.session != impl_->session || update.ownership_epoch != impl_->epoch ||
        (impl_->stream && update.stream != *impl_->stream)) {
        return rejected("net.replica.ownership");
    }
    if (update.revision == impl_->revision) {
        if (impl_->latest && update == *impl_->latest) {
            return impl_->has_baseline ? ReplicaApplyResult::duplicate
                                       : ReplicaApplyResult::resync_required;
        }
        return rejected("net.replica.revision");
    }
    if (update.revision < impl_->revision) {
        return rejected("net.replica.revision");
    }
    if (!update.baseline && (!impl_->has_baseline || update.base_revision != impl_->revision)) {
        return ReplicaApplyResult::resync_required;
    }
    try {
        auto candidate = update.baseline ? std::vector<ReplicaEntity>{} : impl_->entities;
        candidate.reserve(impl_->maximum);
        auto highest = impl_->highest_id;
        for (const auto& change : update.changes) {
            auto found = std::ranges::find(candidate, change.id, &ReplicaEntity::id);
            if (change.kind == ReplicationChange::spawn) {
                const bool retained =
                    update.baseline &&
                    std::ranges::find(impl_->known_live, change.id) != impl_->known_live.end();
                if (found != candidate.end() || candidate.size() >= impl_->maximum ||
                    (change.id.value <= impl_->highest_id && !retained)) {
                    return rejected("net.replica.spawn");
                }
                candidate.push_back({change.id, *change.pose});
                highest = std::max(highest, change.id.value);
            } else if (change.kind == ReplicationChange::update) {
                if (found == candidate.end()) {
                    return rejected("net.replica.change");
                }
                found->pose = *change.pose;
            } else {
                if (found == candidate.end()) {
                    return rejected("net.replica.despawn");
                }
                candidate.erase(found);
            }
        }
        std::ranges::sort(candidate, {}, &ReplicaEntity::id);
        std::vector<ReplicaId> known;
        known.reserve(candidate.size());
        for (const auto& entity : candidate) {
            known.push_back(entity.id);
        }
        std::optional<ReplicationUpdate> latest{update};
        impl_->entities.swap(candidate);
        impl_->known_live.swap(known);
        impl_->latest = std::move(latest);
        impl_->highest_id = highest;
        impl_->stream = update.stream;
        impl_->revision = update.revision;
        impl_->has_baseline = true;
        return ReplicaApplyResult::applied;
    } catch (...) {
        return rejected("net.replica.apply", ErrorCode::execution_failed);
    }
}

Result<ReplicaSnapshot> ReplicaClient::snapshot() const {
    if (auto owner = impl_->check_owner(); !owner) {
        return owner.error();
    }
    try {
        return ReplicaSnapshot{impl_->stream, impl_->revision, impl_->has_baseline,
                               impl_->entities};
    } catch (...) {
        return rejected("net.replica.snapshot", ErrorCode::execution_failed);
    }
}

Result<void> ReplicaClient::discard_baseline() {
    if (auto owner = impl_->check_owner(); !owner) {
        return owner.error();
    }
    impl_->entities.clear();
    impl_->has_baseline = false;
    return {};
}

} // namespace shark::net
