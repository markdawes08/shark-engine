#include "shark/world_cell_residency.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace shark {
namespace {
constexpr std::size_t maximum_cells = 8;
constexpr std::size_t maximum_resident_bytes = 256 * 1024 * 1024;
constexpr std::size_t maximum_pending_bytes = 160 * 1024 * 1024;

struct Failure {
    Error error;
};
[[noreturn]] void fail(ErrorCode code, const char* message) {
    throw Failure{Error{code, message, {}}};
}
template <typename T> T require_value(Result<T> result) {
    if (!result)
        throw Failure{std::move(result).error()};
    return std::move(result).value();
}
void require_value(Result<void> result) {
    if (!result)
        throw Failure{std::move(result).error()};
}
template <typename T, typename F> Result<T> boundary(const char* operation, F&& function) {
    try {
        if constexpr (std::is_void_v<T>) {
            function();
            return {};
        } else {
            return function();
        }
    } catch (Failure& failure) {
        failure.error.context.operation = operation;
        return std::move(failure.error);
    } catch (...) {
        return Error{
            ErrorCode::execution_failed, "World cell residency operation failed", {operation}};
    }
}
void bound_error(Error& error) {
    for (auto* value : {&error.message, &error.context.operation, &error.context.request_id,
                        &error.context.path, &error.context.entity_id, &error.context.asset_id}) {
        if (value->size() > 1024 || value->capacity() > 2048)
            std::string{}.swap(*value);
    }
    error.context.operation = "world.cell.read";
}
bool terminal(WorldCellJobState state) noexcept {
    return state == WorldCellJobState::published || state == WorldCellJobState::failed ||
           state == WorldCellJobState::cancelled;
}
std::size_t add_bytes(std::size_t left, std::size_t right) {
    if (right > std::numeric_limits<std::size_t>::max() - left)
        fail(ErrorCode::invalid_configuration, "World cell byte accounting overflow");
    return left + right;
}
std::uint64_t fresh_scope() {
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    while (value != std::numeric_limits<std::uint64_t>::max()) {
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
            return value;
    }
    fail(ErrorCode::conflict, "World cell residency scope exhausted");
}
void validate_key(WorldCellKey key) {
    require_value(validate_world_position({key.world, key.cell, {0.0, 0.0, 0.0}}));
}
struct ApiGuard {
    bool& active;
    explicit ApiGuard(bool& value) : active(value) {
        if (active)
            fail(ErrorCode::conflict, "World cell residency cannot be reentered");
        active = true;
    }
    ~ApiGuard() {
        active = false;
    }
};
} // namespace

struct WorldCellResidency::Impl {
    struct Job {
        WorldCellJobStatus status;
        std::shared_ptr<const PreparedAssets> assets;
        std::stop_source stop;
        std::uint64_t baseline_generation;
        std::size_t reservation;
        std::vector<std::byte> payload;
        // Already allocated before invoking trusted reader code. The worker can
        // move this fixed error even if a callback/allocation throws.
        Error fallback{ErrorCode::execution_failed,
                       "World cell reader failed",
                       {.operation = "world.cell.read"}};
    };
    struct Resident {
        WorldCellResident info;
        std::unique_ptr<LoadedWorldCell> value;
    };

    WorldCellReader reader;
    WorldCellResidencyLimits limits;
    const std::thread::id owner{std::this_thread::get_id()};
    const std::uint64_t scope;
    std::uint64_t next_sequence{1};
    std::uint64_t next_generation{1};
    mutable bool api_active{false};
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::array<std::unique_ptr<Job>, world_cell_maximum_jobs> slots{};
    std::array<std::optional<Resident>, maximum_cells> cells{};
    std::array<std::optional<WorldCellPin>, world_cell_maximum_pins> pins{};
    std::uint64_t next_pin{1};
    std::size_t resident_count{0};
    std::size_t resident_bytes{0};
    std::size_t pending_bytes{0};
    std::uint64_t bytes_read{0};
    std::size_t peak_resident_bytes{0};
    std::size_t peak_pending_bytes{0};
    bool closing{false};
    std::jthread worker;

    Impl(WorldCellReader callback, WorldCellResidencyLimits bounds)
        : reader(std::move(callback)), limits(bounds), scope(fresh_scope()) {}

    void check_owner() const {
        if (owner != std::this_thread::get_id())
            fail(ErrorCode::conflict, "World cell residency requires its creating thread");
    }
    Job& find_job(WorldCellJobId id) const {
        if (id.scope != scope)
            fail(ErrorCode::conflict, "World cell job belongs to another owner");
        for (const auto& job : slots)
            if (job && job->status.id == id)
                return *job;
        fail(ErrorCode::invalid_id, "World cell job is unknown or already taken");
    }
    std::optional<Resident>* find_cell(WorldCellKey key) {
        for (auto& cell : cells)
            if (cell && cell->info.key == key)
                return &cell;
        return nullptr;
    }
    const std::optional<Resident>* find_cell(WorldCellKey key) const {
        for (const auto& cell : cells)
            if (cell && cell->info.key == key)
                return &cell;
        return nullptr;
    }
    bool pending_key(WorldCellKey key) const {
        for (const auto& job : slots)
            if (job && !terminal(job->status.state) && job->status.key == key)
                return true;
        return false;
    }
    bool pinned(WorldCellKey key) const {
        return std::any_of(pins.begin(), pins.end(),
                           [&](const auto& pin) { return pin && pin->key == key; });
    }
    Job* queued() const {
        Job* result = nullptr;
        for (const auto& job : slots)
            if (job && job->status.state == WorldCellJobState::queued &&
                (!result || job->status.id.sequence < result->status.id.sequence))
                result = job.get();
        return result;
    }
    void release_pending(Job& job) noexcept {
        pending_bytes -= job.reservation;
        job.reservation = 0;
        std::vector<std::byte>{}.swap(job.payload);
        job.assets.reset();
    }
    void mark_cancelled(Job& job) noexcept {
        job.status.state = WorldCellJobState::cancelled;
        job.status.error.reset();
        release_pending(job);
    }
    void run() noexcept {
        try {
            for (;;) {
                Job* job = nullptr;
                {
                    std::unique_lock lock{mutex};
                    wake.wait(lock, [&] { return closing || queued() != nullptr; });
                    if (closing)
                        return;
                    job = queued();
                    job->status.state = WorldCellJobState::running;
                }
                std::vector<std::byte> payload;
                std::optional<Error> error;
                std::size_t read_bytes = 0;
                try {
                    const WorldCellReadRequest request{job->status.key,
                                                       job->status.content_revision};
                    auto returned = reader(request, job->stop.get_token());
                    if (!returned)
                        throw Failure{std::move(returned).error()};
                    payload = std::move(returned).value();
                    read_bytes = payload.size();
                    if (payload.size() > request.maximum_bytes ||
                        payload.capacity() > request.maximum_bytes)
                        fail(ErrorCode::invalid_configuration,
                             "World cell reader exceeded its byte capacity");
                    if (!job->stop.stop_requested()) {
                        const auto decoded = require_value(decode_world_cell(payload));
                        if (decoded.key != request.key ||
                            decoded.content_revision != request.content_revision)
                            fail(ErrorCode::conflict,
                                 "World cell payload does not match the requested key/revision");
                        for (const auto& entity : decoded.scene.entities)
                            if (entity.mesh_renderer)
                                static_cast<void>(
                                    require_value(job->assets->resolve(*entity.mesh_renderer)));
                    }
                } catch (Failure& failure) {
                    try {
                        bound_error(failure.error);
                        error = std::move(failure.error);
                    } catch (...) {
                        error = std::move(job->fallback);
                    }
                } catch (...) {
                    error = std::move(job->fallback);
                }
                {
                    const std::lock_guard lock{mutex};
                    const auto count = static_cast<std::uint64_t>(read_bytes);
                    bytes_read = count > std::numeric_limits<std::uint64_t>::max() - bytes_read
                                     ? std::numeric_limits<std::uint64_t>::max()
                                     : bytes_read + count;
                    if (!error && !job->stop.stop_requested())
                        job->payload = std::move(payload);
                    job->status.error = std::move(error);
                    job->status.state = WorldCellJobState::ready;
                }
                // Do not touch this Job after publishing ready: the owner may
                // cancel/take it immediately while the worker selects another.
            }
        } catch (...) {
            // A synchronization failure cannot leave a live worker silently
            // unjoined or permit publication from an unknown execution state.
            std::terminate();
        }
    }
    void shutdown() {
        std::array<std::optional<std::stop_source>, world_cell_maximum_jobs> stops{};
        {
            const std::lock_guard lock{mutex};
            if (closing)
                return;
            closing = true;
            for (std::size_t index = 0; index < slots.size(); ++index)
                if (slots[index] && !terminal(slots[index]->status.state)) {
                    slots[index]->status.cancellation_requested = true;
                    stops[index] = slots[index]->stop;
                }
        }
        // request_stop can synchronously call trusted registered callbacks.
        // API guards reject reentrancy and no service mutex is held here.
        for (auto& stop : stops)
            if (stop)
                stop->request_stop();
        wake.notify_all();
        if (worker.joinable())
            worker.join();
        const std::lock_guard lock{mutex};
        for (auto& job : slots)
            if (job && !terminal(job->status.state))
                mark_cancelled(*job);
    }
};

WorldCellResidency::WorldCellResidency(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
WorldCellResidency::~WorldCellResidency() {
    if (impl_->owner != std::this_thread::get_id())
        std::terminate();
    try {
        const ApiGuard guard{impl_->api_active};
        impl_->shutdown();
        if (std::any_of(impl_->pins.begin(), impl_->pins.end(),
                        [](const auto& pin) { return pin.has_value(); }))
            std::terminate();
    } catch (...) {
        std::terminate();
    }
}

Result<std::unique_ptr<WorldCellResidency>>
WorldCellResidency::create(WorldCellReader reader, WorldCellResidencyLimits limits) {
    return boundary<std::unique_ptr<WorldCellResidency>>("world.cells.create", [&] {
        if (!reader || limits.maximum_resident_cells == 0 ||
            limits.maximum_resident_cells > maximum_cells || limits.maximum_resident_bytes == 0 ||
            limits.maximum_resident_bytes > maximum_resident_bytes ||
            limits.maximum_pending_bytes == 0 ||
            limits.maximum_pending_bytes > maximum_pending_bytes)
            fail(ErrorCode::invalid_configuration, "Invalid world cell residency limits or reader");
        return std::unique_ptr<WorldCellResidency>(
            new WorldCellResidency{std::make_unique<Impl>(std::move(reader), limits)});
    });
}

Result<WorldCellJobId> WorldCellResidency::submit(WorldCellKey key, std::uint64_t content_revision,
                                                  std::shared_ptr<const PreparedAssets> assets) {
    return boundary<WorldCellJobId>("world.cells.submit", [&] {
        impl_->check_owner();
        const ApiGuard guard{impl_->api_active};
        validate_key(key);
        if (content_revision == 0 || !assets)
            fail(ErrorCode::invalid_argument, "World cell submission requires revision/assets");
        const auto reservation =
            add_bytes(world_cell_maximum_payload_bytes, assets->retained_bytes());
        const std::lock_guard lock{impl_->mutex};
        if (impl_->closing)
            fail(ErrorCode::conflict, "World cell admission is closed");
        if (impl_->pending_key(key))
            fail(ErrorCode::conflict, "World cell already has a pending intent");
        auto free = std::find_if(impl_->slots.begin(), impl_->slots.end(),
                                 [](const auto& slot) { return !slot; });
        if (free == impl_->slots.end() || reservation > impl_->limits.maximum_pending_bytes ||
            impl_->pending_bytes > impl_->limits.maximum_pending_bytes - reservation)
            fail(ErrorCode::conflict, "World cell pending capacity is full; drain and take jobs");
        if (impl_->next_sequence == std::numeric_limits<std::uint64_t>::max())
            fail(ErrorCode::conflict, "World cell job sequence exhausted");
        const auto* current = impl_->find_cell(key);
        const auto baseline = current ? (*current)->info.generation : 0;
        const WorldCellJobId id{impl_->scope, impl_->next_sequence};
        auto job = std::make_unique<Impl::Job>(
            Impl::Job{{id, key, content_revision, WorldCellJobState::queued},
                      std::move(assets),
                      {},
                      baseline,
                      reservation,
                      {}});
        // Start before publishing the slot, so failure cannot leave an admitted
        // job without a worker. The worker waits until this mutex is released.
        if (!impl_->worker.joinable())
            impl_->worker = std::jthread{[state = impl_.get()] { state->run(); }};
        *free = std::move(job);
        ++impl_->next_sequence;
        impl_->pending_bytes += reservation;
        impl_->peak_pending_bytes = std::max(impl_->peak_pending_bytes, impl_->pending_bytes);
        impl_->wake.notify_one();
        return id;
    });
}

Result<std::vector<WorldCellJobStatus>> WorldCellResidency::jobs() const {
    return boundary<std::vector<WorldCellJobStatus>>("world.cells.jobs", [&] {
        impl_->check_owner();
        const ApiGuard guard{impl_->api_active};
        const std::lock_guard lock{impl_->mutex};
        std::vector<WorldCellJobStatus> result;
        result.reserve(world_cell_maximum_jobs);
        for (const auto& job : impl_->slots)
            if (job)
                result.push_back(job->status);
        std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
            return left.id.sequence < right.id.sequence;
        });
        return result;
    });
}

Result<std::size_t> WorldCellResidency::drain() {
    return boundary<std::size_t>("world.cells.drain", [&] {
        impl_->check_owner();
        const ApiGuard guard{impl_->api_active};
        std::unique_lock lock{impl_->mutex};
        Impl::Job* job = nullptr;
        for (const auto& slot : impl_->slots)
            if (slot && slot->status.state == WorldCellJobState::ready &&
                (!job || slot->status.id.sequence < job->status.id.sequence))
                job = slot.get();
        if (!job)
            return std::size_t{0};
        if (job->status.cancellation_requested || job->stop.stop_requested()) {
            impl_->mark_cancelled(*job);
            return std::size_t{1};
        }
        if (!job->status.error) {
            auto published = boundary<std::uint64_t>("world.cells.publish", [&] {
                auto* current = impl_->find_cell(job->status.key);
                const auto baseline = current ? (*current)->info.generation : 0;
                if (baseline != job->baseline_generation)
                    fail(ErrorCode::conflict, "World cell resident changed during preparation");
                if (impl_->pinned(job->status.key))
                    fail(ErrorCode::conflict, "World cell is pinned by an active consumer");
                if (!current && impl_->resident_count == impl_->limits.maximum_resident_cells)
                    fail(ErrorCode::conflict, "World cell resident count is full");
                const auto charge = add_bytes(add_bytes(prepared_scene_maximum_decoded_bytes +
                                                            world_geometry_maximum_decoded_bytes,
                                                        job->payload.size()),
                                              job->assets->retained_bytes());
                if (charge > impl_->limits.maximum_resident_bytes ||
                    impl_->resident_bytes > impl_->limits.maximum_resident_bytes - charge)
                    fail(ErrorCode::conflict,
                         "World cell replacement peak exceeds resident budget");
                if (impl_->next_generation == std::numeric_limits<std::uint64_t>::max())
                    fail(ErrorCode::conflict, "World cell resident generation exhausted");
                impl_->peak_resident_bytes =
                    std::max(impl_->peak_resident_bytes, impl_->resident_bytes + charge);
                lock.unlock();
                auto loaded = [&] {
                    try {
                        return LoadedWorldCell::load(job->payload, job->assets);
                    } catch (...) {
                        lock.lock();
                        throw;
                    }
                }();
                lock.lock();
                if (!loaded)
                    throw Failure{std::move(loaded).error()};
                if (job->status.cancellation_requested || job->stop.stop_requested())
                    fail(ErrorCode::conflict, "World cell publication was cancelled");
                current = impl_->find_cell(job->status.key);
                if ((current ? (*current)->info.generation : 0) != job->baseline_generation)
                    fail(ErrorCode::conflict, "World cell publication baseline changed");
                if (!current)
                    current = &*std::find_if(impl_->cells.begin(), impl_->cells.end(),
                                             [](const auto& cell) { return !cell; });
                const auto old_charge = *current ? (*current)->info.charged_bytes : 0;
                const auto generation = impl_->next_generation++;
                Impl::Resident replacement{
                    {job->status.key, job->status.content_revision, generation, charge},
                    std::move(loaded).value()};
                if (!*current)
                    ++impl_->resident_count;
                *current = std::move(replacement);
                impl_->resident_bytes = impl_->resident_bytes - old_charge + charge;
                return generation;
            });
            if (published) {
                job->status.resident_generation = published.value();
                job->status.state = WorldCellJobState::published;
            } else {
                auto error = std::move(published).error();
                bound_error(error);
                error.context.operation = "world.cells.publish";
                job->status.error = std::move(error);
            }
        }
        if (job->status.error)
            job->status.state = WorldCellJobState::failed;
        impl_->release_pending(*job);
        return std::size_t{1};
    });
}

Result<WorldCellJobStatus> WorldCellResidency::cancel(WorldCellJobId id) {
    return boundary<WorldCellJobStatus>("world.cells.cancel", [&] {
        impl_->check_owner();
        const ApiGuard guard{impl_->api_active};
        std::unique_lock lock{impl_->mutex};
        auto& job = impl_->find_job(id);
        if (terminal(job.status.state))
            return job.status;
        job.status.cancellation_requested = true;
        auto stop = job.stop;
        // Queued cancellation must withdraw dispatch while still holding the
        // queue lock. Otherwise the worker could enter a callback in the gap
        // before request_stop observes its token.
        if (job.status.state == WorldCellJobState::queued ||
            job.status.state == WorldCellJobState::ready)
            impl_->mark_cancelled(job);
        lock.unlock();
        stop.request_stop();
        lock.lock();
        if (job.status.state == WorldCellJobState::ready)
            impl_->mark_cancelled(job);
        return job.status;
    });
}

Result<WorldCellJobStatus> WorldCellResidency::take(WorldCellJobId id) {
    return boundary<WorldCellJobStatus>("world.cells.take", [&] {
        impl_->check_owner();
        const ApiGuard guard{impl_->api_active};
        const std::lock_guard lock{impl_->mutex};
        auto& job = impl_->find_job(id);
        if (!terminal(job.status.state))
            fail(ErrorCode::conflict, "World cell job is not terminal; drain it first");
        auto result = job.status;
        for (auto& slot : impl_->slots)
            if (slot.get() == &job) {
                slot.reset();
                break;
            }
        return result;
    });
}

Result<std::vector<WorldCellResident>> WorldCellResidency::residents() const {
    return boundary<std::vector<WorldCellResident>>("world.cells.residents", [&] {
        impl_->check_owner();
        const ApiGuard guard{impl_->api_active};
        std::vector<WorldCellResident> result;
        result.reserve(impl_->resident_count);
        for (const auto& cell : impl_->cells)
            if (cell)
                result.push_back(cell->info);
        std::sort(result.begin(), result.end(),
                  [](const auto& left, const auto& right) { return left.key < right.key; });
        return result;
    });
}

Result<const LoadedWorldCell*> WorldCellResidency::resident(WorldCellKey key) const {
    return boundary<const LoadedWorldCell*>("world.cells.resident", [&] {
        impl_->check_owner();
        const ApiGuard guard{impl_->api_active};
        validate_key(key);
        const auto* cell = impl_->find_cell(key);
        if (!cell)
            fail(ErrorCode::invalid_id, "World cell is not resident");
        return (*cell)->value.get();
    });
}

Result<void> WorldCellResidency::evict(WorldCellKey key, std::uint64_t expected_generation) {
    return boundary<void>("world.cells.evict", [&] {
        impl_->check_owner();
        const ApiGuard guard{impl_->api_active};
        validate_key(key);
        const std::lock_guard lock{impl_->mutex};
        if (impl_->pinned(key))
            fail(ErrorCode::conflict, "World cell is pinned by an active consumer");
        if (impl_->pending_key(key))
            fail(ErrorCode::conflict, "World cell has a pending intent; cancel and drain first");
        auto* cell = impl_->find_cell(key);
        if (!cell)
            fail(ErrorCode::invalid_id, "World cell is not resident");
        if (expected_generation == 0 || (*cell)->info.generation != expected_generation)
            fail(ErrorCode::conflict, "World cell resident generation changed");
        impl_->resident_bytes -= (*cell)->info.charged_bytes;
        --impl_->resident_count;
        cell->reset();
    });
}

Result<WorldCellResidencyUsage> WorldCellResidency::usage() const {
    return boundary<WorldCellResidencyUsage>("world.cells.usage", [&] {
        impl_->check_owner();
        const ApiGuard guard{impl_->api_active};
        const std::lock_guard lock{impl_->mutex};
        const auto retained = static_cast<std::size_t>(std::count_if(
            impl_->slots.begin(), impl_->slots.end(), [](const auto& job) { return !!job; }));
        return WorldCellResidencyUsage{impl_->resident_count,
                                       impl_->resident_bytes,
                                       impl_->pending_bytes,
                                       retained,
                                       impl_->bytes_read,
                                       impl_->peak_resident_bytes,
                                       impl_->peak_pending_bytes,
                                       impl_->closing,
                                       static_cast<std::size_t>(std::count_if(
                                           impl_->pins.begin(), impl_->pins.end(),
                                           [](const auto& pin) { return pin.has_value(); }))};
    });
}

Result<WorldCellPin> WorldCellResidency::pin(WorldCellKey key, std::uint64_t expected_generation) {
    return boundary<WorldCellPin>("world.cells.pin", [&] {
        impl_->check_owner();
        const ApiGuard guard{impl_->api_active};
        validate_key(key);
        const std::lock_guard lock{impl_->mutex};
        const auto* current = impl_->find_cell(key);
        if (!current || expected_generation == 0 ||
            (*current)->info.generation != expected_generation)
            fail(ErrorCode::conflict, "World cell pin requires the exact resident generation");
        auto free = std::find_if(impl_->pins.begin(), impl_->pins.end(),
                                 [](const auto& pin) { return !pin; });
        if (free == impl_->pins.end() ||
            impl_->next_pin == std::numeric_limits<std::uint64_t>::max())
            fail(ErrorCode::conflict, "World cell pin capacity exhausted");
        const WorldCellPin result{impl_->scope, impl_->next_pin++, key, expected_generation};
        *free = result;
        return result;
    });
}

Result<void> WorldCellResidency::unpin(const WorldCellPin& pin) {
    return boundary<void>("world.cells.unpin", [&] {
        impl_->check_owner();
        const ApiGuard guard{impl_->api_active};
        const std::lock_guard lock{impl_->mutex};
        auto found =
            std::find_if(impl_->pins.begin(), impl_->pins.end(),
                         [&](const auto& retained) { return retained && *retained == pin; });
        if (pin.scope != impl_->scope || found == impl_->pins.end())
            fail(ErrorCode::conflict, "World cell pin is stale or foreign");
        found->reset();
    });
}

Result<void> WorldCellResidency::close() {
    return boundary<void>("world.cells.close", [&] {
        impl_->check_owner();
        const ApiGuard guard{impl_->api_active};
        impl_->shutdown();
    });
}

} // namespace shark
