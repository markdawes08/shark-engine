#include "shark/net/interest.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <thread>
#include <type_traits>
#include <utility>

namespace shark::net {
namespace {
constexpr std::size_t maximum_candidates = 128;
constexpr std::uint64_t byte_fraction = 1000000000;

Error rejected(const char* operation, ErrorCode code = ErrorCode::conflict) {
    return {code, "Interest operation rejected", {.operation = operation}};
}

void add(std::uint64_t& destination, std::uint64_t amount) noexcept {
    destination += std::min(amount, std::numeric_limits<std::uint64_t>::max() - destination);
}

bool valid_entry(const InterestEntry& entry) {
    return (entry.visibility == InterestVisibility::hidden ||
            entry.visibility == InterestVisibility::public_pose ||
            entry.visibility == InterestVisibility::owner_pose) &&
           ((entry.visibility != InterestVisibility::owner_pose && !entry.observer) || entry.owner);
}

bool authorized(const InterestEntry& entry, CharacterId character) {
    return entry.visibility == InterestVisibility::public_pose ||
           (entry.visibility == InterestVisibility::owner_pose && entry.owner == character);
}

struct WorkBudget {
    InterestWork& work;
    std::size_t maximum;

    bool charge(std::size_t& field) {
        if (work.selection_units() >= maximum) {
            return false;
        }
        ++field;
        return true;
    }
};

// Bottom-up merge avoids implementation-dependent sort comparison bounds. No
// recursion or allocation; a budget failure leaves only this private candidate.
template <typename T, typename Less>
bool bounded_sort(std::array<T, maximum_candidates>& values, std::size_t count, Less less,
                  WorkBudget& budget) {
    std::array<T, maximum_candidates> scratch{};
    for (std::size_t width = 1; width < count; width *= 2) {
        for (std::size_t start = 0; start < count; start += 2 * width) {
            const auto middle = std::min(start + width, count);
            const auto end = std::min(start + 2 * width, count);
            auto left = start;
            auto right = middle;
            for (auto output = start; output < end; ++output) {
                if (left < middle && right < end) {
                    if (!budget.charge(budget.work.catalog_comparisons)) {
                        return false;
                    }
                    scratch[output] =
                        less(values[right], values[left]) ? values[right++] : values[left++];
                } else {
                    scratch[output] = left < middle ? values[left++] : values[right++];
                }
            }
        }
        values = scratch;
    }
    return true;
}

Result<SceneRevision> source_revision(Scene& scene) {
    SceneQueryOptions options;
    options.fields = {false, false, false};
    options.page_size = 1;
    options.scan_budget = 1;
    auto page = scene.query_entities(options);
    if (!page || !std::holds_alternative<RegionSceneScope>(page.value().scope())) {
        return rejected("net.interest.scene");
    }
    return page.value().revision();
}

struct Ranked {
    EntityHandle handle;
    std::uint8_t priority;
    bool required;
    bool retained;
    double distance;
};

bool precedes(const Ranked& left, const Ranked& right) {
    if (left.required != right.required) {
        return left.required;
    }
    if (left.priority != right.priority) {
        return left.priority > right.priority;
    }
    if (left.retained != right.retained) {
        return left.retained;
    }
    if (left.distance != right.distance) {
        return left.distance < right.distance;
    }
    return left.handle < right.handle;
}

struct Selection {
    bool limited{false};
    std::vector<EntityHandle> handles;
    std::size_t eligible{0};
};

Result<void> validate_policy(Scene& scene, std::span<const InterestEntry> catalog,
                             CharacterId character, const InterestLimits& limits,
                             InterestWork& work) {
    if (catalog.empty() || catalog.size() > limits.candidate_limit) {
        return rejected("net.interest.catalog", ErrorCode::invalid_argument);
    }
    std::array<const InterestEntry*, maximum_candidates> rows{};
    std::array<const InterestEntry*, maximum_candidates> observers{};
    std::size_t observer_count = 0;
    std::size_t matching_observers = 0;
    for (std::size_t index = 0; index < catalog.size(); ++index) {
        ++work.policy_rows;
        const auto& row = catalog[index];
        if (!valid_entry(row)) {
            return rejected("net.interest.catalog", ErrorCode::invalid_argument);
        }
        ++work.policy_scene_reads;
        if (!scene.reference(row.entity)) {
            return rejected("net.interest.catalog", ErrorCode::invalid_id);
        }
        rows[index] = &row;
        if (row.observer) {
            observers[observer_count++] = &row;
            if (row.owner == character) {
                ++matching_observers;
            }
        }
    }
    if (matching_observers != 1) {
        return rejected("net.interest.observer", ErrorCode::invalid_argument);
    }
    InterestWork sorting;
    WorkBudget budget{sorting, 65536};
    static_cast<void>(bounded_sort(
        rows, catalog.size(), [](const auto* a, const auto* b) { return a->entity < b->entity; },
        budget));
    static_cast<void>(bounded_sort(
        observers, observer_count, [](const auto* a, const auto* b) { return a->owner < b->owner; },
        budget));
    work.policy_comparisons += sorting.catalog_comparisons;
    for (std::size_t index = 1; index < catalog.size(); ++index) {
        ++work.policy_comparisons;
        if (rows[index - 1]->entity == rows[index]->entity) {
            return rejected("net.interest.catalog", ErrorCode::invalid_argument);
        }
    }
    for (std::size_t index = 1; index < observer_count; ++index) {
        ++work.policy_comparisons;
        if (observers[index - 1]->owner == observers[index]->owner) {
            return rejected("net.interest.observer", ErrorCode::invalid_argument);
        }
    }
    return {};
}

Result<Selection> select(Scene& scene, std::span<const InterestEntry> catalog,
                         const SceneRevision& revision, const SessionSnapshot& session,
                         std::span<const EntityHandle> previous, const InterestLimits& limits,
                         InterestWork& work) {
    WorkBudget budget{work, limits.work_units};
    const auto limited = [] { return Selection{true, {}}; };
    if (catalog.empty() || catalog.size() > limits.candidate_limit) {
        return rejected("net.interest.catalog", ErrorCode::invalid_argument);
    }
    std::array<const InterestEntry*, maximum_candidates> rows{};
    std::array<const InterestEntry*, maximum_candidates> observers{};
    std::size_t observer_count = 0;
    const InterestEntry* observer = nullptr;
    for (std::size_t index = 0; index < catalog.size(); ++index) {
        if (!budget.charge(work.catalog_rows)) {
            return limited();
        }
        const auto& row = catalog[index];
        if (!valid_entry(row)) {
            return rejected("net.interest.catalog", ErrorCode::invalid_argument);
        }
        rows[index] = &row;
        if (row.observer) {
            observers[observer_count++] = &row;
            if (row.owner == session.character) {
                observer = &row;
            }
        }
    }
    if (!observer) {
        return rejected("net.interest.observer", ErrorCode::invalid_argument);
    }
    if (!bounded_sort(
            rows, catalog.size(),
            [](const auto* a, const auto* b) { return a->entity < b->entity; }, budget) ||
        !bounded_sort(
            observers, observer_count,
            [](const auto* a, const auto* b) { return a->owner < b->owner; }, budget)) {
        return limited();
    }
    for (std::size_t index = 1; index < catalog.size(); ++index) {
        if (!budget.charge(work.catalog_comparisons)) {
            return limited();
        }
        if (rows[index - 1]->entity == rows[index]->entity) {
            return rejected("net.interest.catalog", ErrorCode::invalid_argument);
        }
    }
    for (std::size_t index = 1; index < observer_count; ++index) {
        if (!budget.charge(work.catalog_comparisons)) {
            return limited();
        }
        if (observers[index - 1]->owner == observers[index]->owner) {
            return rejected("net.interest.observer", ErrorCode::invalid_argument);
        }
    }

    std::array<math::Vec3, maximum_candidates> positions{};
    math::Vec3 focus{};
    for (std::size_t index = 0; index < catalog.size(); ++index) {
        if (!budget.charge(work.scene_reads)) {
            return limited();
        }
        auto value = scene.inspect_entity(rows[index]->entity, {false, true, false}, revision);
        if (!value) {
            return rejected("net.interest.catalog", ErrorCode::invalid_id);
        }
        const auto& matrix = value.value().entities().front().transform->world_matrix;
        positions[index] = {matrix[3][0], matrix[3][1], matrix[3][2]};
        if (rows[index] == observer) {
            focus = positions[index];
        }
    }

    std::array<Ranked, replication_max_entities> ranked{};
    std::size_t selected_count = 0;
    std::size_t required_count = 0;
    std::size_t eligible_count = 0;
    for (std::size_t index = 0; index < catalog.size(); ++index) {
        const auto& row = *rows[index];
        if (!authorized(row, session.character)) {
            continue;
        }
        std::size_t first = 0;
        std::size_t last = previous.size();
        while (first < last) {
            if (!budget.charge(work.ranking_comparisons)) {
                return limited();
            }
            const auto middle = first + (last - first) / 2;
            if (previous[middle] < row.entity) {
                first = middle + 1;
            } else {
                last = middle;
            }
        }
        if (first < previous.size() && !budget.charge(work.ranking_comparisons)) {
            return limited();
        }
        const bool retained = first < previous.size() && previous[first] == row.entity;
        if (!budget.charge(work.distance_tests)) {
            return limited();
        }
        const double radius = retained ? limits.exit_radius : limits.enter_radius;
        std::array<double, 3> deltas{};
        bool outside = false;
        for (int axis = 0; axis < 3; ++axis) {
            const double delta = positions[index][axis] - focus[axis];
            // Subtraction of finite opposite extremes can overflow. It is outside
            // the finite radius; never square an unbounded global coordinate.
            if (std::abs(delta) > radius) {
                outside = true;
                break;
            }
            deltas[static_cast<std::size_t>(axis)] = delta;
        }
        const double distance = outside ? 0 : std::hypot(deltas[0], deltas[1], deltas[2]);
        if (outside || distance > radius) {
            continue;
        }
        ++eligible_count;
        if (row.required && ++required_count > limits.replication.entities_per_stream) {
            return rejected("net.interest.required", ErrorCode::conflict);
        }
        const Ranked candidate{row.entity, row.priority, row.required, retained, distance};
        std::size_t position = 0;
        while (position < selected_count) {
            if (!budget.charge(work.ranking_comparisons)) {
                return limited();
            }
            if (precedes(candidate, ranked[position])) {
                break;
            }
            ++position;
        }
        if (position < limits.replication.entities_per_stream) {
            const auto next_count =
                std::min(selected_count + 1, limits.replication.entities_per_stream);
            for (auto shift = next_count - 1; shift > position; --shift) {
                ranked[shift] = ranked[shift - 1];
            }
            ranked[position] = candidate;
            selected_count = next_count;
        }
    }
    std::array<EntityHandle, maximum_candidates> handles{};
    for (std::size_t index = 0; index < selected_count; ++index) {
        handles[index] = ranked[index].handle;
    }
    if (!bounded_sort(
            handles, selected_count, [](EntityHandle a, EntityHandle b) { return a < b; },
            budget)) {
        return limited();
    }
    Selection result;
    result.eligible = eligible_count;
    result.handles.assign(handles.begin(),
                          handles.begin() + static_cast<std::ptrdiff_t>(selected_count));
    return result;
}
} // namespace

struct InterestServer::Impl {
    struct Stream {
        SessionSnapshot session;
        std::uint64_t scene_instance;
        std::vector<EntityHandle> selected;
        std::vector<EntityHandle> last_sent;
        MonotonicTime last_selection;
        MonotonicTime last_refill;
        std::uint64_t credit;
        bool due{true};
        std::size_t eligible{0};
    };
    struct Accounting {
        InterestStats& counters;
        InterestWork& work;
        ~Accounting() {
            add(counters.selection_units, static_cast<std::uint64_t>(work.selection_units()));
            add(counters.privacy_checks, static_cast<std::uint64_t>(work.privacy_checks));
            add(counters.policy_units,
                static_cast<std::uint64_t>(work.policy_rows + work.policy_scene_reads +
                                           work.policy_comparisons));
        }
    };

    Impl(InterestLimits value, std::unique_ptr<ReplicationServer> server)
        : limits(value), replication(std::move(server)) {
        streams.reserve(limits.replication.streams);
    }
    Result<void> check_owner() const {
        if (owner != std::this_thread::get_id()) {
            return rejected("net.interest.owner");
        }
        return {};
    }
    Result<void> advance(MonotonicTime now) {
        if (now < MonotonicTime::zero() || now < last_time) {
            return rejected("net.interest.time", ErrorCode::invalid_argument);
        }
        last_time = now;
        return {};
    }
    void refill(Stream& stream, MonotonicTime now) const {
        const auto maximum = static_cast<std::uint64_t>(limits.framed_burst_bytes) * byte_fraction;
        const auto deficit = maximum - stream.credit;
        const auto rate = static_cast<std::uint64_t>(limits.framed_bytes_per_second);
        const auto elapsed = static_cast<std::uint64_t>((now - stream.last_refill).count());
        // Compare before multiplication, even near the monotonic clock maximum.
        if (elapsed >= (deficit + rate - 1) / rate) {
            stream.credit = maximum;
        } else {
            stream.credit += elapsed * rate;
        }
        stream.last_refill = now;
    }

    // The complete catalog is caller-owned and bounded. Do not trust absent,
    // duplicated, malformed or foreign rows to renew an earlier disclosure.
    bool privacy(Scene& scene, std::span<const InterestEntry> catalog, const Stream& stream,
                 const Result<SceneRevision>& revision, InterestWork& work) const {
        if (!revision || revision.value().scene_instance != stream.scene_instance ||
            catalog.size() > limits.candidate_limit) {
            return false;
        }
        if (stream.last_sent.empty()) {
            return true;
        }
        for (const auto handle : stream.last_sent) {
            ++work.privacy_checks;
            if (!scene.reference(handle)) {
                // Same live Scene incarnation: a checked stale handle has ended.
                continue;
            }
            const InterestEntry* found = nullptr;
            for (const auto& row : catalog) {
                if (row.entity == handle) {
                    if (found) {
                        return false;
                    }
                    found = &row;
                }
            }
            if (!found || !valid_entry(*found) || !authorized(*found, stream.session.character)) {
                return false;
            }
        }
        return true;
    }

    void close_private(SessionAuthority& sessions, TlsTransport& transport, SessionId session) {
        static_cast<void>(transport.close());
        static_cast<void>(sessions.revoke_session(session));
        static_cast<void>(replication->forget(session));
        std::erase_if(streams,
                      [session](const Stream& item) { return item.session.session == session; });
        add(counters.privacy_closures, 1);
    }

    const std::thread::id owner{std::this_thread::get_id()};
    InterestLimits limits;
    std::unique_ptr<ReplicationServer> replication;
    std::vector<Stream> streams;
    InterestStats counters;
    MonotonicTime last_time{0};
};

InterestServer::InterestServer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
InterestServer::~InterestServer() = default;

Result<std::unique_ptr<InterestServer>> InterestServer::create(const InterestLimits& limits) {
    using namespace std::chrono_literals;
    if (limits.candidate_limit == 0 || limits.candidate_limit > maximum_candidates ||
        !std::isfinite(limits.enter_radius) || !std::isfinite(limits.exit_radius) ||
        limits.enter_radius <= 0 || limits.exit_radius < limits.enter_radius ||
        limits.exit_radius > 1000000 || limits.publish_period < 1ms || limits.publish_period > 1s ||
        limits.framed_bytes_per_second == 0 || limits.framed_bytes_per_second > 1048576 ||
        limits.framed_burst_bytes < 3742 + wire_header_bytes ||
        limits.framed_burst_bytes > 1048576 || limits.work_units == 0 ||
        limits.work_units > 65536) {
        return rejected("net.interest.create", ErrorCode::invalid_configuration);
    }
    auto publisher = ReplicationServer::create(limits.replication);
    if (!publisher) {
        return rejected("net.interest.create", ErrorCode::invalid_configuration);
    }
    try {
        return std::unique_ptr<InterestServer>{
            new InterestServer{std::make_unique<Impl>(limits, std::move(publisher).value())}};
    } catch (...) {
        return rejected("net.interest.create", ErrorCode::execution_failed);
    }
}

Result<InterestPublishResult> InterestServer::publish(Scene& scene,
                                                      std::span<const InterestEntry> catalog,
                                                      SessionAuthority& sessions,
                                                      TlsTransport& transport, MonotonicTime now) {
    auto& self = *impl_;
    auto owned = self.check_owner();
    if (!owned) {
        return owned.error();
    }
    auto advanced = self.advance(now);
    if (!advanced) {
        return advanced.error();
    }
    auto session = sessions.current(transport, now);
    if (!session || !session.value().ready) {
        static_cast<void>(transport.close());
        return rejected("net.interest.session");
    }
    try {
        InterestPublishResult result{InterestPublishStatus::unchanged};
        Impl::Accounting accounting{self.counters, result.work};
        auto existing = std::ranges::find_if(self.streams, [&](const Impl::Stream& stream) {
            return stream.session.account == session.value().account;
        });
        const bool same =
            existing != self.streams.end() && existing->session.session == session.value().session;
        auto revision = source_revision(scene);
        auto policy = revision ? validate_policy(scene, catalog, session.value().character,
                                                 self.limits, result.work)
                               : Result<void>{revision.error()};
        if (same && (!policy || !self.privacy(scene, catalog, *existing, revision, result.work))) {
            self.close_private(sessions, transport, session.value().session);
            result.status = InterestPublishStatus::privacy_closed;
            return result;
        }
        if (!revision || (same && existing->scene_instance != revision.value().scene_instance)) {
            return rejected("net.interest.scene");
        }
        if (!policy) {
            return policy.error();
        }
        if (same && !existing->due && now - existing->last_selection < self.limits.publish_period) {
            result.status = InterestPublishStatus::cadence_limited;
            result.selected_entities = existing->selected.size();
            result.eligible_entities = existing->eligible;
            result.omitted_entities = existing->eligible - existing->selected.size();
            add(self.counters.coalesced, 1);
            return result;
        }
        if (existing == self.streams.end() &&
            self.streams.size() >= self.limits.replication.streams) {
            return rejected("net.interest.capacity");
        }
        const std::span<const EntityHandle> previous =
            same ? std::span<const EntityHandle>{existing->selected}
                 : std::span<const EntityHandle>{};
        auto selected = select(scene, catalog, revision.value(), session.value(), previous,
                               self.limits, result.work);
        if (!selected) {
            return selected.error();
        }
        if (selected.value().limited) {
            if (same) {
                existing->last_selection = now;
                existing->due = false;
                result.selected_entities = existing->selected.size();
                result.eligible_entities = existing->eligible;
                result.omitted_entities = existing->eligible - existing->selected.size();
            } else {
                Impl::Stream limited{session.value(),
                                     revision.value().scene_instance,
                                     {},
                                     {},
                                     now,
                                     now,
                                     static_cast<std::uint64_t>(self.limits.framed_burst_bytes) *
                                         byte_fraction,
                                     false};
                if (existing != self.streams.end()) {
                    *existing = std::move(limited);
                } else {
                    self.streams.push_back(std::move(limited));
                }
            }
            result.status = InterestPublishStatus::work_limited;
            add(self.counters.coalesced, 1);
            return result;
        }
        Impl::Stream candidate =
            same ? *existing
                 : Impl::Stream{session.value(),
                                revision.value().scene_instance,
                                {},
                                {},
                                now,
                                now,
                                static_cast<std::uint64_t>(self.limits.framed_burst_bytes) *
                                    byte_fraction,
                                true};
        candidate.eligible = selected.value().eligible;
        candidate.selected = std::move(selected).value().handles;
        candidate.last_selection = now;
        candidate.due = false;
        self.refill(candidate, now);
        // All wrapper allocations precede transport admission. Stream storage was
        // reserved at creation; publication commits with noexcept moves/swaps only.
        auto next_sent = candidate.selected;
        auto publication =
            self.replication->publish(scene, candidate.selected, sessions, transport, now,
                                      static_cast<std::size_t>(candidate.credit / byte_fraction));
        if (!publication) {
            return publication.error();
        }
        result.selected_entities = candidate.selected.size();
        result.eligible_entities = candidate.eligible;
        result.omitted_entities = candidate.eligible - candidate.selected.size();
        add(self.counters.omitted_entities, static_cast<std::uint64_t>(result.omitted_entities));
        switch (publication.value().status) {
        case ReplicationPublishStatus::sent:
            result.status = InterestPublishStatus::sent;
            result.framed_bytes = publication.value().framed_bytes;
            result.records = publication.value().records;
            candidate.credit -= static_cast<std::uint64_t>(result.framed_bytes) * byte_fraction;
            candidate.last_sent.swap(next_sent);
            add(self.counters.publications, 1);
            add(self.counters.framed_bytes, static_cast<std::uint64_t>(result.framed_bytes));
            add(self.counters.records, static_cast<std::uint64_t>(result.records));
            break;
        case ReplicationPublishStatus::unchanged:
            result.status = InterestPublishStatus::unchanged;
            break;
        case ReplicationPublishStatus::pending_ack:
            result.status = InterestPublishStatus::pending_ack;
            break;
        case ReplicationPublishStatus::byte_limited:
            result.status = InterestPublishStatus::byte_limited;
            break;
        case ReplicationPublishStatus::queue_limited:
            result.status = InterestPublishStatus::queue_limited;
            break;
        }
        if (result.status != InterestPublishStatus::sent &&
            result.status != InterestPublishStatus::unchanged) {
            add(self.counters.coalesced, 1);
        }
        static_assert(std::is_nothrow_move_constructible_v<Impl::Stream>);
        static_assert(std::is_nothrow_move_assignable_v<Impl::Stream>);
        if (existing != self.streams.end()) {
            *existing = std::move(candidate);
        } else {
            self.streams.push_back(std::move(candidate));
        }
        return result;
    } catch (...) {
        self.close_private(sessions, transport, session.value().session);
        return rejected("net.interest.publish", ErrorCode::execution_failed);
    }
}

Result<ReplicationControlResult>
InterestServer::control(Scene& scene, std::span<const InterestEntry> catalog,
                        SessionAuthority& sessions, TlsTransport& transport,
                        const ReplicationControl& control, MonotonicTime now) {
    auto& self = *impl_;
    auto owned = self.check_owner();
    if (!owned) {
        return owned.error();
    }
    auto advanced = self.advance(now);
    if (!advanced) {
        return advanced.error();
    }
    auto session = sessions.current(transport, now);
    if (!session || !session.value().ready) {
        static_cast<void>(transport.close());
        return rejected("net.interest.session");
    }
    try {
        auto stream = std::ranges::find_if(self.streams, [&](const Impl::Stream& item) {
            return item.session.session == session.value().session;
        });
        if (stream == self.streams.end()) {
            return rejected("net.interest.control");
        }
        InterestWork work;
        Impl::Accounting accounting{self.counters, work};
        auto revision = source_revision(scene);
        auto policy =
            revision ? validate_policy(scene, catalog, session.value().character, self.limits, work)
                     : Result<void>{revision.error()};
        if (!policy || !self.privacy(scene, catalog, *stream, revision, work)) {
            self.close_private(sessions, transport, session.value().session);
            return rejected("net.interest.privacy");
        }
        if (!revision || revision.value().scene_instance != stream->scene_instance) {
            return rejected("net.interest.scene");
        }
        return self.replication->control(sessions, transport, control, now);
    } catch (...) {
        self.close_private(sessions, transport, session.value().session);
        return rejected("net.interest.control", ErrorCode::execution_failed);
    }
}

Result<bool> InterestServer::forget(SessionId session) {
    auto& self = *impl_;
    auto owned = self.check_owner();
    if (!owned) {
        return owned.error();
    }
    auto forgotten = self.replication->forget(session);
    if (!forgotten) {
        return forgotten.error();
    }
    const auto count = std::erase_if(self.streams, [session](const Impl::Stream& stream) {
        return stream.session.session == session;
    });
    return count != 0 || forgotten.value();
}

Result<InterestStats> InterestServer::stats() const {
    const auto& self = *impl_;
    auto owned = self.check_owner();
    if (!owned) {
        return owned.error();
    }
    auto result = self.counters;
    result.streams = self.streams.size();
    for (const auto& stream : self.streams) {
        result.selected_entities += stream.selected.size();
        result.last_sent_entities += stream.last_sent.size();
    }
    return result;
}

} // namespace shark::net
